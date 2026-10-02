// metal_async_test: what asynchronous commit must survive.
//   1) buffers released right after commit, while the GPU still uses them
//   2) the process exits with command buffers still running (argv[1] == "exit")
#import <Metal/Metal.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
    @autoreleasepool {
        id<MTLDevice> dev = nil;
        for (id<MTLDevice> d in MTLCopyAllDevices()) if ([d.name containsString:@"RTX"]) dev = d;
        if (!dev) { printf("no RTX Metal device\n"); return 1; }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLLibrary> lib = [dev newLibraryWithSource:
            @"kernel void k(device uint *o [[buffer(0)]], uint i [[thread_position_in_grid]]) {"
             " uint v = o[i]; for (uint j = 0; j < 2000u; j++) v = v * 1664525u + 1013904223u; o[i] = v; }"
                                              options:nil error:nil];
        id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"k"] error:nil];
        id<MTLCommandBuffer> last = nil;
        for (int i = 0; i < 200; i++) @autoreleasepool {
            id<MTLBuffer> b = [dev newBufferWithLength:1 << 20 options:(i & 1) ? MTLResourceStorageModePrivate : MTLResourceStorageModeShared];
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            [ce setComputePipelineState:ps]; [ce setBuffer:b offset:0 atIndex:0];
            [ce dispatchThreads:MTLSizeMake(1 << 18, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
            [ce endEncoding];
            [cb commit];
            last = cb;
            b = nil;               // released while the GPU runs it
        }
        if (argc > 1 && !strcmp(argv[1], "exit")) { printf("metal_async_test: exiting with work in flight\n"); return 0; }
        [last waitUntilCompleted];
        printf("metal_async_test: %s (status %ld)\n", last.status == MTLCommandBufferStatusCompleted ? "PASS" : "FAIL", (long)last.status);
        return last.status != MTLCommandBufferStatusCompleted;
    }
}
