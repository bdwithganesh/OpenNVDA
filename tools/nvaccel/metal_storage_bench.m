// GPU read speed by storage mode: one kernel re-reads a 4 MiB buffer many
// times (every thread walks 256 floats from a spread start), timed for a
// Shared, a Managed and a Private buffer. On a discrete GPU Shared lives in
// system memory; this shows how much slower the GPU sees it.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <mach/mach_time.h>

static NSString *const kSrc =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "kernel void rd(device const float *b [[buffer(0)]], device float *o [[buffer(1)]], constant uint &n [[buffer(2)]],\n"
     "               uint g [[thread_position_in_grid]]) {\n"
     "  float s = 0; uint i = (g * 2654435761u) % n;\n"
     "  for (uint k = 0; k < 256; k++) { s += b[i]; i += 33; if (i >= n) i -= n; }\n"
     "  o[g] = s;\n"
     "}\n";

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:nil error:&e];
        id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"rd"] error:&e];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        const uint32_t n = 1u << 20;   // 4 MiB of floats
        const NSUInteger threads = 1u << 20;
        id<MTLBuffer> out = [dev newBufferWithLength:threads * 4 options:MTLResourceStorageModePrivate];
        const MTLResourceOptions modes[3] = {MTLResourceStorageModeShared, MTLResourceStorageModeManaged, MTLResourceStorageModePrivate};
        const char *names[3] = {"Shared", "Managed", "Private"};
        mach_timebase_info_data_t tb; mach_timebase_info(&tb);
        for (int m = 0; m < 3; m++) {
            id<MTLBuffer> b = [dev newBufferWithLength:n * 4 options:modes[m]];
            if (m < 2) { float *p = b.contents; for (uint32_t i = 0; i < n; i++) p[i] = 1.0f; if (m == 1) [b didModifyRange:NSMakeRange(0, n * 4)]; }
            double best = 1e30;
            for (int rep = 0; rep < 4; rep++) {
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
                [ce setComputePipelineState:ps];
                [ce setBuffer:b offset:0 atIndex:0];
                [ce setBuffer:out offset:0 atIndex:1];
                [ce setBytes:&n length:4 atIndex:2];
                [ce dispatchThreads:MTLSizeMake(threads, 1, 1) threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
                [ce endEncoding];
                const uint64_t t0 = mach_absolute_time();
                [cb commit];
                [cb waitUntilCompleted];
                const double ms = (mach_absolute_time() - t0) * tb.numer / tb.denom / 1e6;
                if (ms < best) best = ms;
            }
            const double gb = (double)threads * 256 * 4 / 1e9;
            printf("  %-8s %8.2f ms  %7.1f GB/s of loads\n", names[m], best, gb / (best / 1e3));
        }
        return 0;
    }
}
