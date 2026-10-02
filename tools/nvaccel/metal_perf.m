// metal_perf: the numbers we race Linux/Windows on, through Metal.
//   latency: empty command buffer and one tiny dispatch, commit -> completed
//   bandwidth: private (VRAM) fill/copy, many ops in one command buffer so
//   submit latency drops out; shared (sysmem) the same for reference.
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach_time.h>

static double us(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e3;
}
static int cmpd(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}
static double med(double *v, int n) { qsort(v, n, sizeof *v, cmpd); return v[n / 2]; }

int main(int argc, char **argv) {
    @autoreleasepool {
        id<MTLDevice> dev = nil;
        for (id<MTLDevice> d in MTLCopyAllDevices())
            if ([d.name containsString:@"RTX"]) dev = d;
        if (!dev) { printf("no RTX Metal device\n"); return 1; }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:
            @"kernel void k(device uint *o [[buffer(0)]], uint i [[thread_position_in_grid]]) { o[i] = i; }"
                                              options:nil error:&err];
        id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"k"] error:&err];
        id<MTLBuffer> tiny = [dev newBufferWithLength:4096 options:MTLResourceStorageModePrivate];
        enum { R = 200 };
        double v[R];
        for (int pass = 0; pass < 2; pass++) {
            for (int i = 0; i < R; i++) {
                id<MTLCommandBuffer> cb = [q commandBuffer];
                if (pass) {
                    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
                    [ce setComputePipelineState:ps];
                    [ce setBuffer:tiny offset:0 atIndex:0];
                    [ce dispatchThreads:MTLSizeMake(32, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
                    [ce endEncoding];
                }
                double t0 = us();
                [cb commit];
                [cb waitUntilCompleted];
                v[i] = us() - t0;
            }
            printf("latency %-9s median %.1f us  min %.1f us\n", pass ? "dispatch" : "empty-cb", med(v, R), v[0]);
        }
        // many dispatches in one command buffer: per-dispatch GPU cost
        {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            [ce setComputePipelineState:ps];
            [ce setBuffer:tiny offset:0 atIndex:0];
            for (int i = 0; i < 1000; i++)
                [ce dispatchThreads:MTLSizeMake(32, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
            [ce endEncoding];
            double t0 = us();
            [cb commit]; [cb waitUntilCompleted];
            printf("1000 dispatches in 1 cb: %.2f us each\n", (us() - t0) / 1000);
        }
        size_t SZ = (argc > 1 ? strtoull(argv[1], 0, 0) : 1024) << 20;
        for (int shared = 0; shared < 2; shared++) {
            MTLResourceOptions o = shared ? MTLResourceStorageModeShared : MTLResourceStorageModePrivate;
            size_t sz = shared ? SZ / 4 : SZ;
            id<MTLBuffer> a = [dev newBufferWithLength:sz options:o], b = [dev newBufferWithLength:sz options:o];
            if (!a || !b) { printf("alloc %zu MiB failed\n", sz >> 20); continue; }
            const int N = 10;
            for (int op = 0; op < 2; op++) {
                double best = 1e18;
                for (int rep = 0; rep < 4; rep++) {
                    id<MTLCommandBuffer> cb = [q commandBuffer];
                    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
                    for (int i = 0; i < N; i++) {
                        if (op) [bl copyFromBuffer:a sourceOffset:0 toBuffer:b destinationOffset:0 size:sz];
                        else [bl fillBuffer:b range:NSMakeRange(0, sz) value:(uint8_t)i];
                    }
                    [bl endEncoding];
                    double t0 = us();
                    [cb commit]; [cb waitUntilCompleted];
                    double dt = us() - t0;
                    if (rep && dt < best) best = dt;
                    if (cb.status != MTLCommandBufferStatusCompleted) printf("cb error %ld\n", (long)cb.status);
                }
                // copy moves 2x bytes (read + write); report like the Linux/Windows runs: bytes read+written
                double bytes = (double)sz * N * (op ? 2 : 1);
                printf("%-7s %-4s %5zu MiB x%d: %.1f GB/s\n", shared ? "shared" : "private",
                       op ? "copy" : "fill", sz >> 20, N, bytes / 1e3 / best);
            }
        }
    }
    return 0;
}
