// GPU timestamps: the common timestamp counter set, sampled at the start and
// end of a compute pass (stage boundary), for a light and a 16x heavier
// kernel. The heavy pass must take clearly longer, timestamps must rise, and
// sampleTimestamps: must give a GPU time inside the CPU window.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

static NSString *const kSrc =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "kernel void spin(device float *o [[buffer(0)]], constant uint &n [[buffer(1)]], uint i [[thread_position_in_grid]]) {\n"
     "  float x = float(i); for (uint k = 0; k < n; k++) x = fma(x, 1.0001f, 0.5f); o[i] = x; }\n";

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        id<MTLCounterSet> ts = nil;
        for (id<MTLCounterSet> s in dev.counterSets) if ([s.name isEqualToString:MTLCommonCounterSetTimestamp]) ts = s;
        printf("%s: timestamp set %s, stage boundary %d\n", dev.name.UTF8String, ts ? "yes" : "no",
               [dev supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]);
        if (!ts) { printf("metal_counter_test: FAIL\n"); return 1; }
        MTLCounterSampleBufferDescriptor *sd = [MTLCounterSampleBufferDescriptor new];
        sd.counterSet = ts; sd.sampleCount = 4; sd.storageMode = MTLStorageModeShared;
        NSError *e = nil;
        id<MTLCounterSampleBuffer> sb = [dev newCounterSampleBufferWithDescriptor:sd error:&e];
        if (!sb) { printf("sample buffer: %s\nmetal_counter_test: FAIL\n", e.description.UTF8String); return 1; }
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:nil error:&e];
        id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"spin"] error:&e];
        id<MTLBuffer> o = [dev newBufferWithLength:1 << 24 options:MTLResourceStorageModePrivate];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        MTLTimestamp c0 = 0, g0 = 0, c1 = 0, g1 = 0;
        [dev sampleTimestamps:&c0 gpuTimestamp:&g0];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        const uint32_t n[2] = {4000, 64000};
        for (int k = 0; k < 2; k++) {
            MTLComputePassDescriptor *pd = [MTLComputePassDescriptor computePassDescriptor];
            pd.sampleBufferAttachments[0].sampleBuffer = sb;
            pd.sampleBufferAttachments[0].startOfEncoderSampleIndex = k * 2;
            pd.sampleBufferAttachments[0].endOfEncoderSampleIndex = k * 2 + 1;
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoderWithDescriptor:pd];
            [ce setComputePipelineState:ps];
            [ce setBuffer:o offset:0 atIndex:0];
            [ce setBytes:&n[k] length:4 atIndex:1];
            [ce dispatchThreads:MTLSizeMake(1 << 22, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
            [ce endEncoding];
        }
        [cb commit];
        [cb waitUntilCompleted];
        [dev sampleTimestamps:&c1 gpuTimestamp:&g1];
        NSData *d = [sb resolveCounterRange:NSMakeRange(0, 4)];
        const MTLCounterResultTimestamp *t = d.bytes;
        if (!d || d.length < 4 * sizeof(*t)) { printf("resolve failed\nmetal_counter_test: FAIL\n"); return 1; }
        const double light = (double)(t[1].timestamp - t[0].timestamp), heavy = (double)(t[3].timestamp - t[2].timestamp);
        // the GPU ticks per CPU tick, from the two correlation points
        const double scale = (double)(g1 - g0) / (double)(c1 - c0);
        printf("  light %.0f heavy %.0f gpu ticks (ratio %.1f), gpu/cpu tick %.3f, inside window %d, status %ld\n", light, heavy,
               heavy / (light > 0 ? light : 1), scale, t[0].timestamp >= g0 && t[3].timestamp <= g1, (long)cb.status);
        const BOOL ok = t[1].timestamp > t[0].timestamp && t[3].timestamp > t[2].timestamp && heavy > 3 * light &&
                        t[0].timestamp >= g0 && t[3].timestamp <= g1 && cb.status == MTLCommandBufferStatusCompleted;
        printf("metal_counter_test: %s\n", ok ? "PASS" : "FAIL");
        return !ok;
    }
}
