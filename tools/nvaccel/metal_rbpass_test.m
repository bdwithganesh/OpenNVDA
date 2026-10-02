// metal_rbpass_test: RenderBox's pass shape (IconRendering / SwiftUI on a discrete GPU), seen in
// iconservicesagent on 1 Oct 2026: colour 0 RGBA16Float cleared to 0, colour 1 RG16Float not stored,
// a Stencil8 attachment with load/store DontCare, pipelines that name all three formats, no
// depth-stencil state. Draws a half-alpha red quad over the middle, then (after a render-target
// barrier) a second quad that reads colour 1 as a texture. Checks centre and corner pixels.
#import <Metal/Metal.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "struct VO { float4 p [[position]]; };\n"
             "struct FO { half4 c0 [[color(0)]]; half2 c1 [[color(1)]]; };\n"
             "vertex VO vs(uint v [[vertex_id]], constant float4 &r [[buffer(0)]]) {"
             " float2 t[4] = {r.xy, float2(r.z, r.y), float2(r.x, r.w), r.zw}; VO o; o.p = float4(t[v], 0, 1); return o; }\n"
             "fragment FO fillq() { FO o; o.c0 = half4(0.5, 0, 0, 0.5); o.c1 = half2(0.25, 1); return o; }\n"
             "fragment FO readc1(VO i [[stage_in]], texture2d<half> t [[texture(0)]]) {"
             " half2 s = t.read(uint2(i.p.xy)).xy; FO o; o.c0 = half4(0.5, s.x, 0, 0.5 + 0.5 * s.y * 0); o.c1 = s; return o; }\n"
                                              options:nil error:&err];
        if (!lib) { printf("compile: %s\n", err.description.UTF8String); return 1; }
        id<MTLRenderPipelineState> ps[2];
        NSArray *names = @[ @"fillq", @"readc1" ];
        for (int i = 0; i < 2; i++) {
            MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
            rd.vertexFunction = [lib newFunctionWithName:@"vs"];
            rd.fragmentFunction = [lib newFunctionWithName:names[i]];
            rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
            rd.colorAttachments[0].blendingEnabled = YES;     // premultiplied source-over, as RenderBox
            rd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
            rd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
            rd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            rd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            rd.colorAttachments[1].pixelFormat = MTLPixelFormatRG16Float;
            rd.stencilAttachmentPixelFormat = MTLPixelFormatStencil8;
            ps[i] = [dev newRenderPipelineStateWithDescriptor:rd error:&err];
            if (!ps[i]) { printf("pipeline: %s\n", err.description.UTF8String); return 1; }
        }
        const int S = 64;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:S height:S mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> c0 = [dev newTextureWithDescriptor:td];
        td.pixelFormat = MTLPixelFormatRG16Float;
        id<MTLTexture> c1 = [dev newTextureWithDescriptor:td];
        td.pixelFormat = MTLPixelFormatStencil8;
        td.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> st = [dev newTextureWithDescriptor:td];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
        p.colorAttachments[0].texture = c0;
        p.colorAttachments[0].loadAction = MTLLoadActionClear;
        p.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        p.colorAttachments[0].storeAction = MTLStoreActionStore;
        p.colorAttachments[1].texture = c1;
        p.colorAttachments[1].loadAction = MTLLoadActionClear;
        p.colorAttachments[1].storeAction = MTLStoreActionDontCare;
        p.stencilAttachment.texture = st;
        p.stencilAttachment.loadAction = MTLLoadActionDontCare;
        p.stencilAttachment.storeAction = MTLStoreActionDontCare;
        id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:p];
        const float mid[4] = { -0.5f, -0.5f, 0.5f, 0.5f }, left[4] = { -1, -1, 0, 1 };
        [re setRenderPipelineState:ps[0]];
        [re setVertexBytes:mid length:16 atIndex:0];
        [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [re memoryBarrierWithScope:MTLBarrierScopeRenderTargets afterStages:MTLRenderStageFragment beforeStages:MTLRenderStageFragment];
        [re setRenderPipelineState:ps[1]];
        [re setFragmentTexture:c1 atIndex:0];
        [re setVertexBytes:left length:16 atIndex:0];
        [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [re endEncoding];
        id<MTLBuffer> rb = [dev newBufferWithLength:S * S * 8 options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
        [bl copyFromTexture:c0 sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(S, S, 1)
                   toBuffer:rb destinationOffset:0 destinationBytesPerRow:S * 8 destinationBytesPerImage:S * S * 8];
        [bl endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        // centre-left: quad then readc1 over it; centre-right: quad only; far left: readc1 only (c1 = 0);
        // far right: nothing
        struct { const char *name; int x, y; float e[4]; } chk[] = {
            { "quad+read (24,32)", 24, 32, { 0.75f, 0.25f, 0, 0.75f } },
            { "quad (40,32)", 40, 32, { 0.5f, 0, 0, 0.5f } },
            { "read only (4,32)", 4, 32, { 0.5f, 0, 0, 0.5f } },
            { "empty (60,32)", 60, 32, { 0, 0, 0, 0 } },
        };
        int fails = 0;
        for (unsigned k = 0; k < 4; k++) {
            const uint16_t *h = (const uint16_t *)rb.contents + (chk[k].y * S + chk[k].x) * 4;
            float v[4];
            for (int i = 0; i < 4; i++) v[i] = (float)*(const __fp16 *)&h[i];
            int ok = 1;
            for (int i = 0; i < 4; i++) if (v[i] < chk[k].e[i] - 0.02f || v[i] > chk[k].e[i] + 0.02f) ok = 0;
            printf("%-20s got (%.3f %.3f %.3f %.3f) want (%.2f %.2f %.2f %.2f) %s\n", chk[k].name, v[0], v[1], v[2], v[3],
                   chk[k].e[0], chk[k].e[1], chk[k].e[2], chk[k].e[3], ok ? "ok" : "FAIL");
            fails += !ok;
        }
        printf("metal_rbpass_test on %s: %s\n", dev.name.UTF8String, fails ? "FAIL" : "PASS");
        return fails ? 1 : 0;
    }
}
