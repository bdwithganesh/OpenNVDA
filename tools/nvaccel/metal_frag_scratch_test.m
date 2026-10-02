// metal_frag_scratch_test: a fragment function with a per-pixel local array indexed by a loop
// counter (lowered to scratch / local memory). RenderBox's blur keeps its tap offsets and weights in
// such an array; on the RTX the blurred icon glyphs came out as horizontal streaks (1 Oct 2026).
// Each pixel fills w[i] = (x * 7 + y * 13 + i * 5) % 17 and sums w[(i * 3 + y) % 8] * (i + 1);
// the CPU computes the same per pixel.
#import <Metal/Metal.h>
#include <stdio.h>
int main(void)
{
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "struct VO { float4 p [[position]]; };\n"
             "vertex VO vs(uint v [[vertex_id]]) { float2 t[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)}; VO o; o.p = float4(t[v], 0, 1); return o; }\n"
             "fragment float4 fs(VO i [[stage_in]], constant uint &n [[buffer(0)]]) { uint x = uint(i.p.x), y = uint(i.p.y); float w[40];"
             " for (uint k = 0; k < 40; k++) w[k] = float((x * 7 + y * 13 + k * 5) % 17);"
             " float s = 0; for (uint k = 0; k < n; k++) s += w[(k * 3 + y) % 40] * float(k + 1); return float4(s, 0, 0, 1); }\n"
                                              options:nil error:&err];
        if (!lib) { printf("compile: %s\n", err.description.UTF8String); return 1; }
        MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
        rd.vertexFunction = [lib newFunctionWithName:@"vs"];
        rd.fragmentFunction = [lib newFunctionWithName:@"fs"];
        rd.colorAttachments[0].pixelFormat = MTLPixelFormatR32Float;
        id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:rd error:&err];
        const int S = 96;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float width:S height:S mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> t = [dev newTextureWithDescriptor:td];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
        p.colorAttachments[0].texture = t; p.colorAttachments[0].loadAction = MTLLoadActionClear;
        p.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:p];
        [re setRenderPipelineState:ps];
        const uint32_t n = 6;
        [re setFragmentBytes:&n length:4 atIndex:0];
        [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [re endEncoding];
        id<MTLBuffer> rb = [dev newBufferWithLength:S * S * 4 options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
        [bl copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(S, S, 1)
                   toBuffer:rb destinationOffset:0 destinationBytesPerRow:S * 4 destinationBytesPerImage:S * S * 4];
        [bl endEncoding];
        [cb commit]; [cb waitUntilCompleted];
        const float *o = rb.contents;
        int bad = 0;
        for (int y = 0; y < S; y++)
            for (int x = 0; x < S; x++) {
                float w[40], s = 0;
                for (int k = 0; k < 40; k++) w[k] = (float)((x * 7 + y * 13 + k * 5) % 17);
                for (int k = 0; k < (int)n; k++) s += w[(k * 3 + y) % 40] * (k + 1);
                if (o[y * S + x] != s) { if (bad < 4) printf("  (%d,%d) got %g want %g\n", x, y, o[y * S + x], s); bad++; }
            }
        printf("metal_frag_scratch_test on %s: %d of %d pixels wrong: %s\n", dev.name.UTF8String, bad, S * S, bad ? "FAIL" : "PASS");
        return bad != 0;
    }
}
