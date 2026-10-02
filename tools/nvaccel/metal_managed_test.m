// metal_managed_test: MTLStorageModeManaged buffers (NVMTLDriver 0.5.9: the
// GPU copy lives in VRAM). CPU write + didModifyRange -> kernel -> blit
// synchronizeResource -> CPU read; partial didModifyRange; a blit between
// managed and shared; and SGEMM speed with managed vs shared buffers.
//   sudo metal_managed_test
#import <Metal/Metal.h>
#include <stdio.h>
#include <mach/mach_time.h>

static int fails;
static void check(const char *what, int ok) { printf("  %-60s %s\n", what, ok ? "PASS" : "FAIL"); if (!ok) fails++; }
static double ms(uint64_t t) { static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb); return (double)t * tb.numer / tb.denom / 1e6; }

static NSString *const kSrc =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "kernel void twice(device float *a [[buffer(0)]], uint i [[thread_position_in_grid]]) { a[i] = a[i] * 2.0f + 1.0f; }\n"
     "kernel void tiled(device const float *A [[buffer(0)]], device const float *B [[buffer(1)]],\n"
     "  device float *C [[buffer(2)]], constant uint &n [[buffer(3)]], uint2 g [[thread_position_in_grid]],\n"
     "  uint2 l [[thread_position_in_threadgroup]]) {\n"
     "  threadgroup float ta[16][16], tb[16][16]; float s = 0;\n"
     "  for (uint t = 0; t < n; t += 16) {\n"
     "    ta[l.y][l.x] = A[g.y * n + t + l.x]; tb[l.y][l.x] = B[(t + l.y) * n + g.x];\n"
     "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
     "    for (uint k = 0; k < 16; k++) s += ta[l.y][k] * tb[k][l.x];\n"
     "    threadgroup_barrier(mem_flags::mem_threadgroup); }\n"
     "  C[g.y * n + g.x] = s; }\n";

int main(int argc, char **argv) {
    const bool splitTiming = argc == 2 && !strcmp(argv[1], "--split-timing");
    if (argc > 1 && !splitTiming) {
        fprintf(stderr, "usage: %s [--split-timing]\n", argv[0]); return 2;
    }
    @autoreleasepool {
        id<MTLDevice> dev = nil;
        for (id<MTLDevice> d in MTLCopyAllDevices()) if ([d.name containsString:@"NVIDIA"]) dev = d;
        if (!dev) { printf("no NVIDIA MTLDevice\n"); return 1; }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:nil error:&e];
        id<MTLComputePipelineState> tw = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"twice"] error:&e];
        const NSUInteger N = 1 << 20;
        id<MTLBuffer> m = [dev newBufferWithLength:N * 4 options:MTLResourceStorageModeManaged];
        float *f = m.contents;
        for (NSUInteger i = 0; i < N; i++) f[i] = (float)(i % 1000);
        [m didModifyRange:NSMakeRange(0, N * 4)];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:tw]; [ce setBuffer:m offset:0 atIndex:0];
        [ce dispatchThreads:MTLSizeMake(N, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
        [ce endEncoding];
        id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
        [be synchronizeResource:m];
        [be endEncoding];
        [cb commit]; [cb waitUntilCompleted];
        int bad = 0;
        for (NSUInteger i = 0; i < N; i++) if (f[i] != (float)(i % 1000) * 2 + 1 && bad++ < 3) printf("    [%lu] %f\n", (unsigned long)i, f[i]);
        check("write + didModifyRange -> kernel -> synchronizeResource", !bad);

        // partial update: only [1000, 2000) changes on the CPU side
        for (NSUInteger i = 1000; i < 2000; i++) f[i] = -1;
        [m didModifyRange:NSMakeRange(1000 * 4, 1000 * 4)];
        cb = [q commandBuffer];
        ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:tw]; [ce setBuffer:m offset:0 atIndex:0];
        [ce dispatchThreads:MTLSizeMake(N, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
        [ce endEncoding];
        be = [cb blitCommandEncoder];
        [be synchronizeResource:m];
        [be endEncoding];
        [cb commit]; [cb waitUntilCompleted];
        bad = 0;
        for (NSUInteger i = 0; i < N; i++) {
            const float want = (i >= 1000 && i < 2000) ? -1.0f : ((float)(i % 1000) * 2 + 1) * 2 + 1;
            if (f[i] != want && bad++ < 3) printf("    [%lu] %f want %f\n", (unsigned long)i, f[i], want);
        }
        check("partial didModifyRange keeps the rest of the GPU copy", !bad);

        // blit managed -> shared
        id<MTLBuffer> sh = [dev newBufferWithLength:N * 4 options:MTLResourceStorageModeShared];
        cb = [q commandBuffer];
        be = [cb blitCommandEncoder];
        [be copyFromBuffer:m sourceOffset:0 toBuffer:sh destinationOffset:0 size:N * 4];
        [be endEncoding];
        [cb commit]; [cb waitUntilCompleted];
        check("blit managed -> shared sees the GPU copy", !memcmp(sh.contents, f, N * 4));

        // SGEMM speed: managed vs shared
        id<MTLComputePipelineState> tp = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"tiled"] error:&e];
        const uint32_t n = 2048;
        // The input patterns repeat every 7 rows / 5 columns. Build the CPU
        // reference before submitting, then check the whole result instead
        // of one early row that may miss a late write or a partial dispatch.
        float expected[7][5];
        for (uint32_t row = 0; row < 7; row++)
            for (uint32_t col = 0; col < 5; col++) {
                int32_t sum = 0;
                for (uint32_t k = 0; k < n; k++)
                    sum += ((int32_t)((row + k) % 7) - 3) * ((int32_t)((k * n + col) % 5) - 2);
                expected[row][col] = (float)sum;
            }
        double t[2] = {0};
        for (int mode = 0; mode < 2; mode++) {
            const MTLResourceOptions o = mode ? MTLResourceStorageModeManaged : MTLResourceStorageModeShared;
            id<MTLBuffer> A = [dev newBufferWithLength:n * n * 4 options:o], B = [dev newBufferWithLength:n * n * 4 options:o],
                          C = [dev newBufferWithLength:n * n * 4 options:o];
            float *a = A.contents, *b = B.contents;
            for (uint32_t i = 0; i < n * n; i++) { a[i] = (float)(i % 7) - 3; b[i] = (float)(i % 5) - 2; }
            if (mode) { [A didModifyRange:NSMakeRange(0, n * n * 4)]; [B didModifyRange:NSMakeRange(0, n * n * 4)]; }
            double best = 1e9;
            double bestDownload = 1e9;
            for (int r = 0; r < 3; r++) {
                cb = [q commandBuffer];
                ce = [cb computeCommandEncoder];
                [ce setComputePipelineState:tp];
                [ce setBuffer:A offset:0 atIndex:0]; [ce setBuffer:B offset:0 atIndex:1]; [ce setBuffer:C offset:0 atIndex:2];
                [ce setBytes:&n length:4 atIndex:3];
                [ce dispatchThreads:MTLSizeMake(n, n, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
                [ce endEncoding];
                if (mode && !splitTiming) { be = [cb blitCommandEncoder]; [be synchronizeResource:C]; [be endEncoding]; }
                const uint64_t t0 = mach_absolute_time();
                [cb commit]; [cb waitUntilCompleted];
                const double d = ms(mach_absolute_time() - t0);
                if (d < best) best = d;
                if (mode && splitTiming) {
                    cb = [q commandBuffer]; be = [cb blitCommandEncoder];
                    [be synchronizeResource:C]; [be endEncoding];
                    const uint64_t downloadStart = mach_absolute_time();
                    [cb commit]; [cb waitUntilCompleted];
                    const double download = ms(mach_absolute_time() - downloadStart);
                    if (download < bestDownload) bestDownload = download;
                }
            }
            const float *c = C.contents;
            NSUInteger wrong = 0;
            for (uint32_t j = 0; j < n * n; j++) {
                // Visit distant rows immediately, while completion ordering
                // matters, rather than reading in the GPU's likely order.
                const uint32_t i = (j * 2654435761u) & (n * n - 1);
                wrong += c[i] != expected[((i / n) * n) % 7][(i % n) % 5];
            }
            t[mode] = best;
            printf("    sgemm %u %s: %.2f ms = %.0f GFLOPS, %lu/%u wrong\n", n, mode ? "managed" : "shared ", best,
                   2.0 * n * n * n / best / 1e6, (unsigned long)wrong, n * n);
            if (mode && splitTiming) printf("    managed download separately: %.2f ms (%.2f GB/s)\n", bestDownload, n * n * 4.0 / bestDownload / 1e6);
            if (wrong) fails++;
        }
        // kext 0.163.0 lets the L2 cache Shared (system) memory, so shared is
        // no longer 3x behind; managed (VRAM) must still not be slower
        check(splitTiming ? "managed compute completion not slower than shared (download excluded)" :
              "managed SGEMM not slower than shared", t[1] <= t[0] * 1.1);
    }
    printf("metal_managed_test: %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
