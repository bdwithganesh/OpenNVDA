// metal_tess_test: Metal tessellation on the hardware tessellator
// (NVMTLDriver 0.6.0). A compute kernel writes the factors, a triangle patch
// (3 control points from a vertex descriptor) covers the target, the
// post-tessellation vertex function colours with bc * bc: one flat triangle
// shows the linear mean (0.33) at the centroid, a tessellated one ~0.11.
//   sudo metal_tess_test
#import <Metal/Metal.h>
#include <stdio.h>

static int fails;
static void check(const char *what, int ok) { printf("  %-60s %s\n", what, ok ? "PASS" : "FAIL"); if (!ok) fails++; }

static NSString *const kSrc =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "struct CP { float4 pos [[attribute(0)]]; };\n"
     "struct PIn { patch_control_point<CP> cp; };\n"
     "struct VOut { float4 pos [[position]]; float4 col; };\n"
     "[[patch(triangle, 3)]] vertex VOut tess_vs(PIn p [[stage_in]], float3 bc [[position_in_patch]], uint pid [[patch_id]]) {\n"
     "  VOut o; o.pos = p.cp[0].pos * bc.x + p.cp[1].pos * bc.y + p.cp[2].pos * bc.z;\n"
     "  o.col = float4(bc * bc, float(pid)); return o; }\n"
     "fragment float4 tess_fs(VOut in [[stage_in]]) { return float4(in.col.xyz, 1); }\n"
     "kernel void tess_factors(device MTLTriangleTessellationFactorsHalf *f [[buffer(0)]], constant float &t [[buffer(1)]],\n"
     "  uint i [[thread_position_in_grid]]) {\n"
     "  f[i].edgeTessellationFactor[0] = t; f[i].edgeTessellationFactor[1] = t; f[i].edgeTessellationFactor[2] = t;\n"
     "  f[i].insideTessellationFactor = t; }\n";

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = nil;
        for (id<MTLDevice> d in MTLCopyAllDevices()) if ([d.name containsString:@"NVIDIA"]) dev = d;
        if (!dev) { printf("no NVIDIA MTLDevice\n"); return 1; }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:nil error:&e];
        if (!lib) { printf("compile: %s\n", e.localizedDescription.UTF8String); return 1; }
        id<MTLComputePipelineState> fk = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"tess_factors"] error:&e];
        MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
        vd.attributes[0].format = MTLVertexFormatFloat4; vd.attributes[0].offset = 0; vd.attributes[0].bufferIndex = 0;
        vd.layouts[0].stride = 16; vd.layouts[0].stepFunction = MTLVertexStepFunctionPerPatchControlPoint;
        MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
        rd.vertexFunction = [lib newFunctionWithName:@"tess_vs"];
        rd.fragmentFunction = [lib newFunctionWithName:@"tess_fs"];
        rd.vertexDescriptor = vd;
        rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        rd.maxTessellationFactor = 16;
        rd.tessellationPartitionMode = MTLTessellationPartitionModeInteger;
        rd.tessellationFactorStepFunction = MTLTessellationFactorStepFunctionPerPatch;
        rd.tessellationOutputWindingOrder = MTLWindingCounterClockwise;
        id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:rd error:&e];
        check("tessellation render pipeline", ps != nil && fk != nil);
        if (!ps || !fk) { printf("  %s\n", e.localizedDescription.UTF8String); return 1; }
        const float cps[12] = {-1, -1, 0, 1,   3, -1, 0, 1,   -1, 3, 0, 1};   // covers the target
        id<MTLBuffer> vb = [dev newBufferWithBytes:cps length:sizeof cps options:MTLResourceStorageModeShared];
        id<MTLBuffer> tf = [dev newBufferWithLength:64 options:MTLResourceStorageModePrivate];
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                     width:64 height:64 mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModeShared;
        float centre[2] = {0};
        const float factors[2] = {1, 16};
        for (int run = 0; run < 2; run++) {
            id<MTLTexture> rt = [dev newTextureWithDescriptor:td];
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            [ce setComputePipelineState:fk];
            [ce setBuffer:tf offset:0 atIndex:0];
            [ce setBytes:&factors[run] length:4 atIndex:1];
            [ce dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
            [ce endEncoding];
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = rt;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].clearColor = (MTLClearColor){0, 0, 0, 1};
            id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
            [re setRenderPipelineState:ps];
            [re setVertexBuffer:vb offset:0 atIndex:0];
            [re setTessellationFactorBuffer:tf offset:0 instanceStride:0];
            [re drawPatches:3 patchStart:0 patchCount:1 patchIndexBuffer:nil patchIndexBufferOffset:0
              instanceCount:1 baseInstance:0];
            [re endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            uint8_t px[64 * 64 * 4];
            [rt getBytes:px bytesPerRow:256 fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
            // (-1,-1),(3,-1),(-1,3) -> bc = 1/3 each at NDC (1/3, 1/3): pixel (42, 21)
            const uint8_t *c = px + (21 * 64 + 42) * 4;
            centre[run] = (c[0] + c[1] + c[2]) / 3.0f / 255.0f;
            int black = 0;
            for (int i = 0; i < 64 * 64; i++) black += !px[i * 4] && !px[i * 4 + 1] && !px[i * 4 + 2];
            printf("    factor %2.0f: centre %.3f (rgb %u %u %u), black pixels %d\n", factors[run], centre[run],
                   c[0], c[1], c[2], black);
            check(run ? "factor 16: covered" : "factor 1: covered", black < 64);
        }
        check("factor 1 centre ~0.33 (one flat triangle)", centre[0] > 0.29f && centre[0] < 0.38f);
        check("factor 16 centre ~0.11 (tessellated, bc*bc)", centre[1] > 0.08f && centre[1] < 0.16f);
    }
    printf("metal_tess_test: %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
