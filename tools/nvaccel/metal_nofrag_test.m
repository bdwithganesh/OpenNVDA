// Render pipelines without a fragment function:
//  1. rasterizationEnabled = NO: the vertex function writes to a device
//     buffer (out[vid] = vid * 3 + 1), nothing is rasterized
//  2. depth-only pass: a triangle at z = 0.25 into a Depth32Float target,
//     read back with a blit; the covered half must hold 0.25, the rest 1.0
// Run on Apple's driver for the reference.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

static NSString *const kSrc =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "vertex void vwrite(uint vid [[vertex_id]], device uint *o [[buffer(0)]]) { o[vid] = vid * 3 + 1; }\n"
     "vertex float4 vdepth(uint vid [[vertex_id]]) {\n"
     "  float2 p[3] = {float2(-1, -1), float2(1, -1), float2(-1, 1)}; return float4(p[vid], 0.25, 1); }\n";

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:nil error:&e];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        int bad = 0;
        // 1. vertex only
        MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
        rd.vertexFunction = [lib newFunctionWithName:@"vwrite"];
        rd.rasterizationEnabled = NO;
        rd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:rd error:&e];
        if (!ps) { printf("  vertex-only pipeline: %s\n", e.localizedDescription.UTF8String); bad++; }
        else {
            id<MTLBuffer> out = [dev newBufferWithLength:64 * 4 options:MTLResourceStorageModeShared];
            memset(out.contents, 0, 64 * 4);
            MTLTextureDescriptor *ct = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:8 height:8 mipmapped:NO];
            ct.usage = MTLTextureUsageRenderTarget; ct.storageMode = MTLStorageModePrivate;
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = [dev newTextureWithDescriptor:ct];
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
            [re setRenderPipelineState:ps];
            [re setVertexBuffer:out offset:0 atIndex:0];
            [re drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:64];
            [re endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            int w = 0;
            for (uint32_t i = 0; i < 64; i++) w += ((uint32_t *)out.contents)[i] != i * 3 + 1;
            printf("  vertex-only (rasterization off): %d of 64 values wrong\n", w);
            bad += w != 0;
        }
        // 2. depth only
        rd = [MTLRenderPipelineDescriptor new];
        rd.vertexFunction = [lib newFunctionWithName:@"vdepth"];
        rd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        ps = [dev newRenderPipelineStateWithDescriptor:rd error:&e];
        if (!ps) { printf("  depth-only pipeline: %s\n", e.localizedDescription.UTF8String); bad++; }
        else {
            MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
            dd.depthCompareFunction = MTLCompareFunctionLess; dd.depthWriteEnabled = YES;
            MTLTextureDescriptor *zt = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:32 height:32 mipmapped:NO];
            zt.usage = MTLTextureUsageRenderTarget; zt.storageMode = MTLStorageModePrivate;
            id<MTLTexture> z = [dev newTextureWithDescriptor:zt];
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.depthAttachment.texture = z;
            rp.depthAttachment.loadAction = MTLLoadActionClear;
            rp.depthAttachment.clearDepth = 1.0;
            rp.depthAttachment.storeAction = MTLStoreActionStore;
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
            if (!getenv("NOFRAG_CLEAR_ONLY")) {
                [re setRenderPipelineState:ps];
                [re setDepthStencilState:[dev newDepthStencilStateWithDescriptor:dd]];
                [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            }
            [re endEncoding];
            id<MTLBuffer> zb = [dev newBufferWithLength:32 * 32 * 4 options:MTLResourceStorageModeShared];
            id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
            [be copyFromTexture:z sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(32, 32, 1)
                       toBuffer:zb destinationOffset:0 destinationBytesPerRow:128 destinationBytesPerImage:32 * 128];
            [be endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            const float *d = zb.contents;
            int quarter = 0, one = 0;
            for (int i = 0; i < 32 * 32; i++) { if (d[i] == 0.25f) quarter++; else if (d[i] == 1.0f) one++; }
            printf("  depth-only: %d texels at 0.25, %d at 1.0 (corners %.2f %.2f)\n", quarter, one, d[31 * 32], d[31]);
            bad += quarter < 400 || quarter + one != 1024;
        }
        printf("metal_nofrag_test: %s\n", bad ? "FAIL" : "PASS");
        return bad != 0;
    }
}
