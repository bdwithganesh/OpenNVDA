// Depth test variants SceneKit and games use: reverse Z (clear 0, GreaterEqual),
// normal Z (clear 1, Less), Depth32Float and Depth32Float_Stencil8, one
// triangle at z = 0.5 over the whole target. Every pixel must be green.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

static NSString *const kSrc =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "struct V { float4 p [[position]]; };\n"
     "vertex V vs(uint v [[vertex_id]]) { float2 q = float2((v << 1) & 2, v & 2); V o; o.p = float4(q * 2 - 1, 0.5, 1); return o; }\n"
     "fragment half4 fs() { return half4(0, 1, 0, 1); }\n";

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:nil error:&e];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        int bad = 0;
        const MTLPixelFormat zf[2] = {MTLPixelFormatDepth32Float, MTLPixelFormatDepth32Float_Stencil8};
        for (int f = 0; f < 2; f++)
            for (int rev = 0; rev < 2; rev++) {
                MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
                rd.vertexFunction = [lib newFunctionWithName:@"vs"];
                rd.fragmentFunction = [lib newFunctionWithName:@"fs"];
                rd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
                rd.depthAttachmentPixelFormat = zf[f];
                if (f) rd.stencilAttachmentPixelFormat = zf[f];
                id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:rd error:&e];
                MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
                dd.depthCompareFunction = rev ? MTLCompareFunctionGreaterEqual : MTLCompareFunctionLess;
                dd.depthWriteEnabled = YES;
                id<MTLDepthStencilState> ds = [dev newDepthStencilStateWithDescriptor:dd];
                MTLTextureDescriptor *ct = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                              width:64 height:64 mipmapped:NO];
                ct.usage = MTLTextureUsageRenderTarget;
                ct.storageMode = MTLStorageModePrivate;
                id<MTLTexture> c = [dev newTextureWithDescriptor:ct];
                MTLTextureDescriptor *zt = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:zf[f] width:64 height:64
                                                                                          mipmapped:NO];
                zt.usage = MTLTextureUsageRenderTarget;
                zt.storageMode = MTLStorageModePrivate;
                id<MTLTexture> z = [dev newTextureWithDescriptor:zt];
                MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                rp.colorAttachments[0].texture = c;
                rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 1, 1);
                rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                rp.depthAttachment.texture = z;
                rp.depthAttachment.loadAction = MTLLoadActionClear;
                rp.depthAttachment.clearDepth = rev ? 0.0 : 1.0;
                rp.depthAttachment.storeAction = MTLStoreActionDontCare;
                if (f) { rp.stencilAttachment.texture = z; rp.stencilAttachment.loadAction = MTLLoadActionClear; }
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
                [re setRenderPipelineState:ps];
                [re setDepthStencilState:ds];
                [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [re endEncoding];
                id<MTLBuffer> out = [dev newBufferWithLength:64 * 64 * 4 options:MTLResourceStorageModeShared];
                id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
                [be copyFromTexture:c sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(64, 64, 1)
                           toBuffer:out destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:64 * 256];
                [be endEncoding];
                [cb commit];
                [cb waitUntilCompleted];
                const uint8_t *p = out.contents;
                int green = 0;
                for (int i = 0; i < 64 * 64; i++) if (p[i * 4 + 1] > 250 && p[i * 4] < 5) green++;
                printf("  %-22s %-24s %4d/4096 green (px0 %d %d %d)  %s\n", f ? "Depth32Float_Stencil8" : "Depth32Float",
                       rev ? "reverse Z (0, GEqual)" : "normal Z (1, Less)", green, p[2], p[1], p[0], green == 4096 ? "PASS" : "FAIL");
                bad += green != 4096;
            }
        printf("metal_depth_test: %s (%d failed)\n", bad ? "FAIL" : "PASS", bad);
        return bad != 0;
    }
}
