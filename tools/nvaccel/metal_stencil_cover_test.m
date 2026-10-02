// metal_stencil_cover_test: RenderBox's path fill (stencil, then cover) on a freshly made Stencil8
// texture with load/store DontCare, as iconservicesagent does it. Pass 0 (optional) fills a stencil
// texture with 0xff and drops it, so the new one may land on that memory. Then: draw a triangle
// fan with colour writes off and stencil invert (even-odd), cover the bounds with stencil != 0,
// pass op zero. Apple zero-fills new textures; RenderBox relies on a zero stencil.
// Usage: metal_stencil_cover_test [dirty]
#import <Metal/Metal.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        const BOOL dirty = argc > 1 && !strcmp(argv[1], "dirty");
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "struct VO { float4 p [[position]]; };\n"
             "vertex VO vs(uint v [[vertex_id]], constant float2 *pts [[buffer(0)]]) { VO o; o.p = float4(pts[v], 0, 1); return o; }\n"
             "fragment half4 red() { return half4(1, 0, 0, 1); }\n"
                                              options:nil error:&err];
        if (!lib) { printf("compile: %s\n", err.description.UTF8String); return 1; }
        MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
        rd.vertexFunction = [lib newFunctionWithName:@"vs"];
        rd.fragmentFunction = [lib newFunctionWithName:@"red"];
        rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
        rd.stencilAttachmentPixelFormat = MTLPixelFormatStencil8;
        rd.colorAttachments[0].writeMask = MTLColorWriteMaskNone;
        id<MTLRenderPipelineState> psStencil = [dev newRenderPipelineStateWithDescriptor:rd error:&err];
        rd.colorAttachments[0].writeMask = MTLColorWriteMaskAll;
        id<MTLRenderPipelineState> psCover = [dev newRenderPipelineStateWithDescriptor:rd error:&err];
        if (!psStencil || !psCover) { printf("pipeline: %s\n", err.description.UTF8String); return 1; }
        MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
        MTLStencilDescriptor *s = [MTLStencilDescriptor new];
        s.stencilCompareFunction = MTLCompareFunctionAlways;
        s.depthStencilPassOperation = MTLStencilOperationInvert;
        dd.frontFaceStencil = dd.backFaceStencil = s;
        id<MTLDepthStencilState> dsStencil = [dev newDepthStencilStateWithDescriptor:dd];
        s = [MTLStencilDescriptor new];
        s.stencilCompareFunction = MTLCompareFunctionNotEqual;
        s.depthStencilPassOperation = MTLStencilOperationZero;
        s.stencilFailureOperation = MTLStencilOperationKeep;
        dd.frontFaceStencil = dd.backFaceStencil = s;
        id<MTLDepthStencilState> dsCover = [dev newDepthStencilStateWithDescriptor:dd];
        s = [MTLStencilDescriptor new];
        s.stencilCompareFunction = MTLCompareFunctionAlways;
        s.depthStencilPassOperation = MTLStencilOperationReplace;
        dd.frontFaceStencil = dd.backFaceStencil = s;
        id<MTLDepthStencilState> dsFill = [dev newDepthStencilStateWithDescriptor:dd];
        const int S = 64;
        id<MTLCommandQueue> q = [dev newCommandQueue];
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:S height:S mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> c0 = [dev newTextureWithDescriptor:td];
        MTLTextureDescriptor *sd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatStencil8 width:S height:S mipmapped:NO];
        sd.usage = MTLTextureUsageRenderTarget;
        sd.storageMode = MTLStorageModePrivate;
        const float full[8] = { -1, -1, 1, -1, -1, 1, 1, 1 };
        if (dirty) {   // leave 0xff in a stencil surface, then free it
            for (int k = 0; k < 4; k++) @autoreleasepool {
                id<MTLTexture> st = [dev newTextureWithDescriptor:sd];
                id<MTLCommandBuffer> cb = [q commandBuffer];
                MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
                p.colorAttachments[0].texture = c0; p.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                p.stencilAttachment.texture = st; p.stencilAttachment.loadAction = MTLLoadActionDontCare;
                p.stencilAttachment.storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:p];
                [re setRenderPipelineState:psStencil]; [re setDepthStencilState:dsFill]; [re setStencilReferenceValue:0xff];
                [re setVertexBytes:full length:sizeof full atIndex:0];
                [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
                [re endEncoding];
                [cb commit]; [cb waitUntilCompleted];
            }
        }
        id<MTLTexture> st = [dev newTextureWithDescriptor:sd];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
        p.colorAttachments[0].texture = c0;
        p.colorAttachments[0].loadAction = MTLLoadActionClear;
        p.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        p.colorAttachments[0].storeAction = MTLStoreActionStore;
        p.stencilAttachment.texture = st;
        p.stencilAttachment.loadAction = MTLLoadActionDontCare;
        p.stencilAttachment.storeAction = MTLStoreActionDontCare;
        id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:p];
        // a centred square as two triangles: even-odd covers it once
        const float tri[12] = { -0.5f, -0.5f, 0.5f, -0.5f, 0.5f, 0.5f,   -0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f };
        [re setRenderPipelineState:psStencil];
        [re setDepthStencilState:dsStencil];
        [re setVertexBytes:tri length:sizeof tri atIndex:0];
        [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
        [re setRenderPipelineState:psCover];
        [re setDepthStencilState:dsCover];
        [re setStencilReferenceValue:0];
        [re setVertexBytes:full length:sizeof full atIndex:0];
        [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [re endEncoding];
        id<MTLBuffer> rb = [dev newBufferWithLength:S * S * 8 options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
        [bl copyFromTexture:c0 sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(S, S, 1)
                   toBuffer:rb destinationOffset:0 destinationBytesPerRow:S * 8 destinationBytesPerImage:S * S * 8];
        [bl endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        int in = 0, out = 0;
        for (int y = 0; y < S; y++)
            for (int x = 0; x < S; x++) {
                const float r = (float)((const __fp16 *)rb.contents)[(y * S + x) * 4];
                const int inside = x >= 17 && x < 47 && y >= 17 && y < 47, outside = x < 15 || x >= 49 || y < 15 || y >= 49;
                if (inside && r > 0.9f) in++;
                if (outside && r > 0.1f) out++;
            }
        const int ok = in == 30 * 30 && out == 0;
        printf("metal_stencil_cover_test%s on %s: inside red %d/900, outside red %d: %s\n", dirty ? " dirty" : "",
               dev.name.UTF8String, in, out, ok ? "PASS" : "FAIL");
        return ok ? 0 : 1;
    }
}
