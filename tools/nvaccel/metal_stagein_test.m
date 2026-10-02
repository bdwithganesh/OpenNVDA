// Compute [[stage_in]] through MTLComputePipelineDescriptor.stageInputDescriptor:
// float3 + half2 from one interleaved buffer (thread X), uint from a second
// buffer through a uint16 index buffer (thread X indexed), uchar4 normalized
// BGRA as float4 (constant step).
//   metal_stagein_test si.metallib   (kernel k_stagein, see the .metal below)
//
// struct In { float3 pos [[attribute(0)]]; half2 uv [[attribute(3)]];
//             uint idx [[attribute(5)]]; float4 col [[attribute(6)]]; };
// kernel void k_stagein(In in [[stage_in]], device float4 *out [[buffer(4)]],
//                       uint gid [[thread_position_in_grid]]) {
//   out[gid * 2] = float4(in.pos + float3(float2(in.uv), 0), float(in.idx));
//   out[gid * 2 + 1] = in.col;
// }
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <math.h>

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 2) { fprintf(stderr, "usage: %s si.metallib\n", argv[0]); return 2; }
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&e];
        id<MTLFunction> fn = [lib newFunctionWithName:@"k_stagein"];
        if (!fn) { printf("no k_stagein: %s\n", e.localizedDescription.UTF8String); return 1; }
        enum { N = 37 };
        MTLStageInputOutputDescriptor *si = [MTLStageInputOutputDescriptor stageInputOutputDescriptor];
        si.attributes[0].format = MTLAttributeFormatFloat3;  si.attributes[0].offset = 0; si.attributes[0].bufferIndex = 0;
        si.attributes[3].format = MTLAttributeFormatHalf2;   si.attributes[3].offset = 12; si.attributes[3].bufferIndex = 0;
        si.layouts[0].stride = 16; si.layouts[0].stepFunction = MTLStepFunctionThreadPositionInGridX;
        si.attributes[5].format = MTLAttributeFormatUInt;    si.attributes[5].offset = 0; si.attributes[5].bufferIndex = 1;
        si.layouts[1].stride = 4; si.layouts[1].stepFunction = MTLStepFunctionThreadPositionInGridXIndexed;
        si.attributes[6].format = MTLAttributeFormatUChar4Normalized_BGRA; si.attributes[6].offset = 0;
        si.attributes[6].bufferIndex = 2;
        si.layouts[2].stride = 4; si.layouts[2].stepFunction = MTLStepFunctionConstant; si.layouts[2].stepRate = 0;
        si.indexType = MTLIndexTypeUInt16; si.indexBufferIndex = 3;
        MTLComputePipelineDescriptor *cd = [MTLComputePipelineDescriptor new];
        cd.computeFunction = fn;
        cd.stageInputDescriptor = si;
        id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithDescriptor:cd options:0 reflection:nil error:&e];
        if (!ps) { printf("pipeline: %s\nmetal_stagein_test: FAIL\n", e.localizedDescription.UTF8String); return 1; }

        id<MTLBuffer> vb = [dev newBufferWithLength:16 * N options:MTLResourceStorageModeShared];
        id<MTLBuffer> ib = [dev newBufferWithLength:4 * 64 options:MTLResourceStorageModeShared];
        id<MTLBuffer> cb = [dev newBufferWithLength:4 options:MTLResourceStorageModeShared];
        id<MTLBuffer> xb = [dev newBufferWithLength:2 * N options:MTLResourceStorageModeShared];
        id<MTLBuffer> ob = [dev newBufferWithLength:32 * N options:MTLResourceStorageModeShared];
        for (int i = 0; i < N; i++) {
            float *v = (float *)((uint8_t *)vb.contents + 16 * i);
            v[0] = i; v[1] = 2 * i; v[2] = -i;
            __fp16 *h = (__fp16 *)(v + 3); h[0] = 0.5f; h[1] = (__fp16)(i & 7);
            ((uint16_t *)xb.contents)[i] = (uint16_t)(63 - i);
        }
        for (int i = 0; i < 64; i++) ((uint32_t *)ib.contents)[i] = 1000 + i;
        const uint8_t bgra[4] = {255, 0, 51, 102};   // b g r a
        memcpy(cb.contents, bgra, 4);
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLCommandBuffer> c = [q commandBuffer];
        id<MTLComputeCommandEncoder> enc = [c computeCommandEncoder];
        [enc setComputePipelineState:ps];
        [enc setBuffer:vb offset:0 atIndex:0];
        [enc setBuffer:ib offset:0 atIndex:1];
        [enc setBuffer:cb offset:0 atIndex:2];
        [enc setBuffer:xb offset:0 atIndex:3];
        [enc setBuffer:ob offset:0 atIndex:4];
        [enc dispatchThreads:MTLSizeMake(N, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
        [enc endEncoding];
        [c commit];
        [c waitUntilCompleted];
        int bad = 0;
        const float *o = ob.contents;
        for (int i = 0; i < N; i++) {
            const float want[8] = {i + 0.5f, 2 * i + (i & 7), -i, 1000 + (63 - i), 0.2f, 0, 1, 0.4f};
            for (int k = 0; k < 8; k++)
                if (fabsf(o[8 * i + k] - want[k]) > 1e-3f) {
                    if (bad++ < 6) printf("  thread %d word %d: %g want %g\n", i, k, o[8 * i + k], want[k]);
                }
        }
        printf("metal_stagein_test: %s (%d wrong)\n", bad ? "FAIL" : "PASS", bad);
        return bad != 0;
    }
}
