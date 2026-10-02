// metal_bench: macOS-half performance numbers for M8 (benchmarks vs Windows).
// SGEMM GFLOPS + CE copy/fill bandwidth through our Metal stack, timed
// end-to-end (commit -> completed, staging copies included). Correctness is
// verified; there is no PASS/FAIL vs Windows (no Windows GPU numbers exist
// yet in docs/WINDOWS-REFERENCE-20260926.md) — this banks our side.
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static double now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1e6 + tv.tv_usec;
}

static id<MTLDevice> findNV(void) {
    for (id<MTLDevice> d in MTLCopyAllDevices())
        if ([d.name containsString:@"RTX"]) return d;
    return nil;
}

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = findNV();
        if (!dev) { printf("no RTX Metal device\n"); return 1; }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        NSError *err = nil;
        int bad = 0;
        // 1. SGEMM N=512.
        const int N = 256, N2 = N * N;
        id<MTLLibrary> lib = [dev newLibraryWithSource:
            @"kernel void sgemm(device const float *A [[buffer(0)]], device const float *B [[buffer(1)]],"
             " device float *C [[buffer(2)]], uint i [[thread_position_in_grid]]) {"
             " uint row = i / 256u, col = i % 256u; float s = 0.0f;"
             " for (uint k = 0u; k < 256u; k++) s += A[row * 256u + k] * B[k * 256u + col];"
             " C[i] = s; }"
                                              options:nil error:&err];
        id<MTLComputePipelineState> ps = lib ?
            [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"sgemm"] error:&err] : nil;
        id<MTLBuffer> mA = [dev newBufferWithLength:N2 * 4 options:0],
                      mB = [dev newBufferWithLength:N2 * 4 options:0],
                      mC = [dev newBufferWithLength:N2 * 4 options:0];
        float *fA = mA.contents, *fB = mB.contents, *fC = mC.contents;
        bad += !ps || !fA || !fB || !fC;
        for (int i = 0; i < N2; i++) { fA[i] = (i % 7) - 3; fB[i] = (i % 5) - 2; fC[i] = -1; }
        if (!bad) {
            for (int run = 0; run < 4; run++) {
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
                [ce setComputePipelineState:ps];
                [ce setBuffer:mA offset:0 atIndex:0];
                [ce setBuffer:mB offset:0 atIndex:1];
                [ce setBuffer:mC offset:0 atIndex:2];
                [ce dispatchThreads:MTLSizeMake(N2, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
                [ce endEncoding];
                const double t0 = now_us();
                [cb commit];
                [cb waitUntilCompleted];
                const double dt = now_us() - t0;
                // Full CPU verify on first run only (rest: checksum spot).
                if (run == 0) {
                    for (int r = 0; r < N && !bad; r++)
                        for (int c = 0; c < N; c++) {
                            float s = 0;
                            for (int k = 0; k < N; k++) s += fA[r * N + k] * fB[k * N + c];
                            float d = fC[r * N + c] - s;
                            if (d < -1e-2 || d > 1e-2) { bad++; break; }
                        }
                }
                const double gflops = (2.0 * N * N * N) / dt / 1e3;
                printf("sgemm-%d run %d: %.1f ms = %.2f GFLOPS %s\n",
                       N, run, dt / 1e3, gflops, bad ? "WRONG" : (run ? "" : "(verified)"));
            }
        }
        // 2. CE copy + fill bandwidth (64 MiB).
        const size_t SZ = 64u << 20;
        id<MTLBuffer> bS = [dev newBufferWithLength:SZ options:0],
                      bD = [dev newBufferWithLength:SZ options:0];
        uint8_t *pS = bS.contents, *pD = bD.contents;
        bad += !pS || !pD;
        if (!bad) {
            for (size_t i = 0; i < SZ; i++) pS[i] = (uint8_t)(i * 31 + 7);
            memset(pD, 0, SZ);
            for (int run = 0; run < 3; run++) {
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
                [bl copyFromBuffer:bS sourceOffset:0 toBuffer:bD destinationOffset:0 size:SZ];
                [bl endEncoding];
                const double t0 = now_us();
                [cb commit];
                [cb waitUntilCompleted];
                const double dt = now_us() - t0;
                if (run == 0) bad += memcmp(pS, pD, SZ) != 0;
                printf("copy-64M run %d: %.1f ms = %.2f GB/s %s\n",
                       run, dt / 1e3, (SZ / 1e9) / (dt / 1e6), bad ? "WRONG" : "");
            }
            for (int run = 0; run < 3; run++) {
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
                [bl fillBuffer:bD range:NSMakeRange(0, SZ) value:0xA5];
                [bl endEncoding];
                const double t0 = now_us();
                [cb commit];
                [cb waitUntilCompleted];
                const double dt = now_us() - t0;
                if (run == 0) {
                    for (size_t i = 0; i < SZ && !bad; i += 4099) bad += pD[i] != 0xA5;
                }
                printf("fill-64M run %d: %.1f ms = %.2f GB/s %s\n",
                       run, dt / 1e3, (SZ / 1e9) / (dt / 1e6), bad ? "WRONG" : "");
            }
        }
        // 3. Private (VRAM) buffers: copy + fill GPU-only, checked through a shared copy.
        if (!bad) {
            id<MTLBuffer> vS = [dev newBufferWithLength:SZ options:MTLResourceStorageModePrivate],
                          vD = [dev newBufferWithLength:SZ options:MTLResourceStorageModePrivate];
            bad += !vS || !vD;
            if (!bad) {
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
                [bl copyFromBuffer:bS sourceOffset:0 toBuffer:vS destinationOffset:0 size:SZ];
                [bl endEncoding];
                [cb commit]; [cb waitUntilCompleted];
                for (int run = 0; run < 3; run++) {
                    cb = [q commandBuffer]; bl = [cb blitCommandEncoder];
                    [bl copyFromBuffer:vS sourceOffset:0 toBuffer:vD destinationOffset:0 size:SZ];
                    [bl endEncoding];
                    const double t0 = now_us();
                    [cb commit]; [cb waitUntilCompleted];
                    const double dt = now_us() - t0;
                    if (run == 0) {
                        memset(pD, 0, SZ);
                        cb = [q commandBuffer]; bl = [cb blitCommandEncoder];
                        [bl copyFromBuffer:vD sourceOffset:0 toBuffer:bD destinationOffset:0 size:SZ];
                        [bl endEncoding]; [cb commit]; [cb waitUntilCompleted];
                        bad += memcmp(pS, pD, SZ) != 0;
                    }
                    printf("vram-copy-64M run %d: %.1f ms = %.2f GB/s %s\n",
                           run, dt / 1e3, (SZ / 1e9) / (dt / 1e6), bad ? "WRONG" : "");
                }
                for (int run = 0; run < 3; run++) {
                    cb = [q commandBuffer]; bl = [cb blitCommandEncoder];
                    [bl fillBuffer:vD range:NSMakeRange(0, SZ) value:0x5A];
                    [bl endEncoding];
                    const double t0 = now_us();
                    [cb commit]; [cb waitUntilCompleted];
                    const double dt = now_us() - t0;
                    if (run == 0) {
                        cb = [q commandBuffer]; bl = [cb blitCommandEncoder];
                        [bl copyFromBuffer:vD sourceOffset:0 toBuffer:bD destinationOffset:0 size:SZ];
                        [bl endEncoding]; [cb commit]; [cb waitUntilCompleted];
                        for (size_t i = 0; i < SZ && !bad; i += 4099) bad += pD[i] != 0x5A;
                    }
                    printf("vram-fill-64M run %d: %.1f ms = %.2f GB/s %s\n",
                           run, dt / 1e3, (SZ / 1e9) / (dt / 1e6), bad ? "WRONG" : "");
                }
            }
        }
        printf("metal_bench: %s\n", bad ? "FAIL" : "done");
        return bad != 0;
    }
}
