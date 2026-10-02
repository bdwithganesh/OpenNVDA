// What happens to stencil operations in a pass that has no stencil attachment
// (SpriteKit's shape fill does this): pass 1 writes stencil 1 inside a small
// triangle with colour writes off, pass 2 draws a full-screen quad where
// stencil == 1. Prints how many pixels pass 2 coloured (32x32 target).
// Run with Apple's driver for the reference behaviour.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

static NSString *const kSrc =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "struct V { float4 p [[position]]; };\n"
     "vertex V vs(uint v [[vertex_id]], constant float2 *q [[buffer(0)]]) { V o; o.p = float4(q[v], 0.5, 1); return o; }\n"
     "fragment half4 fs() { return half4(0, 1, 0, 1); }\n";

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:nil error:&e];
        for (int pipeStencil = 0; pipeStencil < 2; pipeStencil++) {
            MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
            rd.vertexFunction = [lib newFunctionWithName:@"vs"];
            rd.fragmentFunction = [lib newFunctionWithName:@"fs"];
            rd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
            if (pipeStencil) rd.stencilAttachmentPixelFormat = MTLPixelFormatStencil8;
            id<MTLRenderPipelineState> color = [dev newRenderPipelineStateWithDescriptor:rd error:&e];
            rd.colorAttachments[0].writeMask = MTLColorWriteMaskNone;
            id<MTLRenderPipelineState> noColor = [dev newRenderPipelineStateWithDescriptor:rd error:&e];
            MTLDepthStencilDescriptor *w = [MTLDepthStencilDescriptor new];
            w.frontFaceStencil.depthStencilPassOperation = MTLStencilOperationReplace;
            w.backFaceStencil.depthStencilPassOperation = MTLStencilOperationReplace;
            MTLDepthStencilDescriptor *t = [MTLDepthStencilDescriptor new];
            t.frontFaceStencil.stencilCompareFunction = MTLCompareFunctionEqual;
            t.backFaceStencil.stencilCompareFunction = MTLCompareFunctionEqual;
            id<MTLDepthStencilState> sw = [dev newDepthStencilStateWithDescriptor:w], st = [dev newDepthStencilStateWithDescriptor:t];
            MTLTextureDescriptor *ct = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                          width:32 height:32 mipmapped:NO];
            ct.usage = MTLTextureUsageRenderTarget;
            ct.storageMode = MTLStorageModePrivate;
            id<MTLTexture> c = [dev newTextureWithDescriptor:ct];
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = c;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLCommandQueue> q = [dev newCommandQueue];
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
            const float tri[6] = {-0.5f, -0.5f, 0.5f, -0.5f, 0, 0.5f};
            const float quad[12] = {-1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1};
            [re setRenderPipelineState:noColor];
            [re setDepthStencilState:sw];
            [re setStencilReferenceValue:1];
            [re setVertexBytes:tri length:sizeof tri atIndex:0];
            [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [re setRenderPipelineState:color];
            [re setDepthStencilState:st];
            [re setVertexBytes:quad length:sizeof quad atIndex:0];
            [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
            [re endEncoding];
            id<MTLBuffer> out = [dev newBufferWithLength:32 * 32 * 4 options:MTLResourceStorageModeShared];
            id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
            [be copyFromTexture:c sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(32, 32, 1)
                       toBuffer:out destinationOffset:0 destinationBytesPerRow:128 destinationBytesPerImage:32 * 128];
            [be endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            int g = 0;
            for (int i = 0; i < 32 * 32; i++) if (((uint8_t *)out.contents)[i * 4 + 1] > 200) g++;
            printf("pipeline stencil format %-8s: %4d of 1024 pixels coloured (%s)\n", pipeStencil ? "Stencil8" : "none", g,
                   g == 1024 ? "stencil ignored" : g == 0 ? "stencil blocked all" : "stencil applied");
        }
        return 0;
    }
}
