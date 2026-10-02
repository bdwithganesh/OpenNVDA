// Sustained GPU load for thermal checks (M28): an FMA loop kernel dispatched
// back to back for N seconds.   sudo metal_burn [seconds]
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    @autoreleasepool {
        const double secs = argc > 1 ? atof(argv[1]) : 180;
        id<MTLDevice> dev = nil;
        for (id<MTLDevice> d in MTLCopyAllDevices()) if ([d.name containsString:@"NVIDIA"]) dev = d;
        if (!dev) { printf("no NVIDIA MTLDevice\n"); return 1; }
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:
            @"#include <metal_stdlib>\nusing namespace metal;\n"
            @"kernel void burn(device float *o [[buffer(0)]], uint i [[thread_position_in_grid]]) {"
            @"  float a = float(i), b = 1.0;"
            @"  for (uint k = 0u; k < 200000u; k++) { a = a * 1.0000001 + 0.5; b = b * 0.9999999 + a; }"
            @"  o[i] = a + b; }\n" options:nil error:&e];
        id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"burn"] error:&e];
        if (!ps) { printf("pipeline: %s\n", e.localizedDescription.UTF8String); return 1; }
        const NSUInteger N = 76 * 2048;   // every SM busy
        id<MTLBuffer> out = [dev newBufferWithLength:N * 4 options:MTLResourceStorageModeShared];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        const CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent();
        unsigned n = 0;
        while (CFAbsoluteTimeGetCurrent() - t0 < secs) {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            [ce setComputePipelineState:ps];
            [ce setBuffer:out offset:0 atIndex:0];
            [ce dispatchThreads:MTLSizeMake(N, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
            [ce endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            n++;
        }
        const double dt = CFAbsoluteTimeGetCurrent() - t0;
        printf("%u dispatches in %.0f s, %.2f TFLOPS\n", n, dt, (double)n * N * 200000 * 4 / dt / 1e12);
    }
    return 0;
}
