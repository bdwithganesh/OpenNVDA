// metal_lat: per-commit latency of the Metal stack (empty blit, tiny compute
// dispatch, fill), to see where small-job time goes.
//   sudo metal_lat ak.metallib
#import <Metal/Metal.h>
#include <stdio.h>
#include <mach/mach_time.h>

static double us(uint64_t t) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)t * tb.numer / tb.denom / 1000.0;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        id<MTLDevice> dev = nil;
        for (id<MTLDevice> d in MTLCopyAllDevices()) if ([d.name containsString:@"NVIDIA"]) dev = d;
        if (!dev || argc < 2) { printf("usage: metal_lat ak.metallib (needs the NVIDIA device)\n"); return 1; }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&e];
        id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"k_vadd"] error:&e];
        id<MTLBuffer> a = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
        id<MTLBuffer> b = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
        id<MTLBuffer> c = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
        const char *names[4] = {"empty commit", "fill 4 KiB", "dispatch 1024 thr", "10 dispatches/commit"};
        for (int kind = 0; kind < 4; kind++) {
            double best = 1e9, sum = 0;
            const int N = 50;
            for (int i = 0; i < N; i++) {
                const uint64_t t0 = mach_absolute_time();
                id<MTLCommandBuffer> cb = [q commandBuffer];
                if (kind == 1) {
                    id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
                    [be fillBuffer:c range:NSMakeRange(0, 4096) value:1];
                    [be endEncoding];
                } else if (kind >= 2) {
                    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
                    [ce setComputePipelineState:ps];
                    [ce setBuffer:a offset:0 atIndex:0]; [ce setBuffer:b offset:0 atIndex:1]; [ce setBuffer:c offset:0 atIndex:2];
                    for (int k = 0; k < (kind == 3 ? 10 : 1); k++)
                        [ce dispatchThreads:MTLSizeMake(1024, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
                    [ce endEncoding];
                }
                [cb commit];
                [cb waitUntilCompleted];
                const double t = us(mach_absolute_time() - t0);
                if (i > 2) { sum += t; if (t < best) best = t; }
            }
            printf("%-22s best %8.1f us  avg %8.1f us\n", names[kind], best, sum / (N - 3));
        }
    }
    return 0;
}
