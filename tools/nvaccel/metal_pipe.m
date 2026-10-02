// pipelined commits: N command buffers of one small dispatch each, committed
// back to back, wait only for the last. Per-command-buffer cost + CPU time in commit.
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <mach/mach_time.h>
static double us(void) { static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e3; }
int main(int argc, char **argv) {
    @autoreleasepool {
        id<MTLDevice> dev = nil;
        for (id<MTLDevice> d in MTLCopyAllDevices()) if ([d.name containsString:@"RTX"] || [d.name containsString:@"Apple"]) dev = d;
        id<MTLCommandQueue> q = [dev newCommandQueueWithMaxCommandBufferCount:512];
        id<MTLLibrary> lib = [dev newLibraryWithSource:@"kernel void k(device uint *o [[buffer(0)]], uint i [[thread_position_in_grid]]) { o[i] += 1; }" options:nil error:nil];
        id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"k"] error:nil];
        id<MTLBuffer> b = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
        memset(b.contents, 0, 4096);
        const int N = argc > 1 ? atoi(argv[1]) : 500;
        double tCommit = 0, t0 = us();
        id<MTLCommandBuffer> last = nil;
        for (int i = 0; i < N; i++) @autoreleasepool {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            [ce setComputePipelineState:ps]; [ce setBuffer:b offset:0 atIndex:0];
            [ce dispatchThreads:MTLSizeMake(1024, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            [ce endEncoding];
            double c0 = us(); [cb commit]; tCommit += us() - c0;
            last = cb;
        }
        [last waitUntilCompleted];
        double dt = us() - t0;
        const uint32_t *v = b.contents; int bad = 0;
        for (int i = 0; i < 1024; i++) bad += v[i] != (uint32_t)N;
        printf("pipe %d cbs: %.1f us per cb, commit %.1f us on the caller, %s\n", N, dt / N, tCommit / N, bad ? "WRONG" : "ok");
        return bad != 0;
    }
}
