// metal_sgemm: SGEMM on VRAM (private) buffers through our Metal stack, a
// naive and a 16x16 shared-memory tiled kernel, N = 1024 / 2048 / 4096.
//   sudo metal_sgemm
#import <Metal/Metal.h>
#include <stdio.h>
#include <mach/mach_time.h>
#include <math.h>

static double ms(uint64_t t) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)t * tb.numer / tb.denom / 1e6;
}

static BOOL completed(id<MTLCommandBuffer> cb, const char *phase, uint32_t n) {
    [cb waitUntilCompleted];
    if (cb.status == MTLCommandBufferStatusCompleted) return YES;
    fprintf(stderr, "sgemm %s n=%u: status %lu error %s: FAIL\n", phase, n,
            (unsigned long)cb.status, cb.error.description.UTF8String ?: "none");
    return NO;
}

static NSString *const kSrc =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "kernel void naive(device const float *A [[buffer(0)]], device const float *B [[buffer(1)]],\n"
     "  device float *C [[buffer(2)]], constant uint &n [[buffer(3)]], uint2 g [[thread_position_in_grid]]) {\n"
     "  float s = 0; for (uint k = 0; k < n; k++) s += A[g.y * n + k] * B[k * n + g.x]; C[g.y * n + g.x] = s; }\n"
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

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = nil;
        for (id<MTLDevice> d in MTLCopyAllDevices()) if ([d.name containsString:@"NVIDIA"]) dev = d;
        if (!dev) { printf("no NVIDIA device\n"); return 1; }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:nil error:&e];
        if (!lib) { printf("compile: %s\n", e.localizedDescription.UTF8String); return 1; }
        unsigned failures = 0, checks = 0;
        const char *kn[2] = {"naive", "tiled"};
        for (uint32_t n = 1024; n <= 4096; n *= 2) {
            const NSUInteger bytes = (NSUInteger)n * n * 4;
            id<MTLBuffer> hA = [dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
            id<MTLBuffer> hB = [dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
            id<MTLBuffer> hC = [dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
            id<MTLBuffer> A = [dev newBufferWithLength:bytes options:MTLResourceStorageModePrivate];
            id<MTLBuffer> B = [dev newBufferWithLength:bytes options:MTLResourceStorageModePrivate];
            id<MTLBuffer> C = [dev newBufferWithLength:bytes options:MTLResourceStorageModePrivate];
            if (!q || !hA || !hB || !hC || !A || !B || !C) {
                fprintf(stderr, "sgemm buffers n=%u: FAIL\n", n); return 1;
            }
            float *a = hA.contents, *b = hB.contents;
            for (uint32_t i = 0; i < n * n; i++) { a[i] = (float)((i * 7) % 11) - 5; b[i] = (float)((i * 3) % 13) - 6; }
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
            [be copyFromBuffer:hA sourceOffset:0 toBuffer:A destinationOffset:0 size:bytes];
            [be copyFromBuffer:hB sourceOffset:0 toBuffer:B destinationOffset:0 size:bytes];
            [be endEncoding];
            [cb commit]; if (!completed(cb, "upload", n)) return 1;
            for (int k = 0; k < 2; k++) {
                if (k == 0 && n > 2048) continue;
                id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@(kn[k])] error:&e];
                if (!ps) { fprintf(stderr, "sgemm pipeline: FAIL\n"); return 1; }
                double best = 1e9;
                for (int run = 0; run < 3; run++) {
                    cb = [q commandBuffer];
                    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
                    [ce setComputePipelineState:ps];
                    [ce setBuffer:A offset:0 atIndex:0]; [ce setBuffer:B offset:0 atIndex:1]; [ce setBuffer:C offset:0 atIndex:2];
                    [ce setBytes:&n length:4 atIndex:3];
                    [ce dispatchThreads:MTLSizeMake(n, n, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
                    [ce endEncoding];
                    const uint64_t t0 = mach_absolute_time();
                    [cb commit]; if (!completed(cb, "dispatch", n)) return 1;
                    const double t = ms(mach_absolute_time() - t0);
                    if (t < best) best = t;
                }
                // check a few entries
                cb = [q commandBuffer];
                be = [cb blitCommandEncoder];
                [be copyFromBuffer:C sourceOffset:0 toBuffer:hC destinationOffset:0 size:bytes];
                [be endEncoding];
                [cb commit]; if (!completed(cb, "readback", n)) return 1;
                const float *c = hC.contents;
                int bad = 0;
                for (int s = 0; s < 16; s++) {
                    const uint32_t r = (s * 977) % n, col = (s * 331) % n;
                    double want = 0;
                    for (uint32_t i = 0; i < n; i++) want += (double)a[r * n + i] * b[i * n + col];
                    if (!isfinite(c[r * n + col]) || fabs(c[r * n + col] - want) > 1e-3 * (fabs(want) + 1)) bad++;
                }
                printf("sgemm %-5s n=%4u: %8.2f ms  %8.1f GFLOPS %s\n", kn[k], n, best,
                       2.0 * n * n * n / best / 1e6, bad ? "WRONG" : "ok");
                failures += bad != 0; ++checks;
            }
        }
        printf("sgemm command buffers and sampled output: %s (%u checks, %u failed)\n", failures ? "FAIL" : "PASS", checks, failures);
        return failures != 0;
    }
}
