// Layered rendering: one draw into a 4-slice 2D array render target, each
// instance picks its slice with [[render_target_array_index]] and colour;
// then viewport arrays: two viewports, [[viewport_array_index]] per instance.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

static NSString *const kSrc =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "struct V { float4 pos [[position]]; float4 col; uint layer [[render_target_array_index]]; };\n"
     "vertex V vsL(uint vid [[vertex_id]], uint iid [[instance_id]]) {\n"
     "  const float2 p[3] = { float2(-1, -1), float2(3, -1), float2(-1, 3) };\n"
     "  V o; o.pos = float4(p[vid], 0, 1); o.col = float4(0.25 * (iid + 1), 0, 1 - 0.25 * iid, 1); o.layer = iid; return o; }\n"
     "struct W { float4 pos [[position]]; float4 col; uint vp [[viewport_array_index]]; };\n"
     "vertex W vsV(uint vid [[vertex_id]], uint iid [[instance_id]]) {\n"
     "  const float2 p[3] = { float2(-1, -1), float2(3, -1), float2(-1, 3) };\n"
     "  W o; o.pos = float4(p[vid], 0, 1); o.col = iid ? float4(0, 1, 0, 1) : float4(1, 0, 0, 1); o.vp = iid; return o; }\n"
     "fragment float4 fs(float4 c [[stage_in]]) { return c; }\n"
     "struct FI { float4 col; };\n"
     "fragment float4 fsV(W in [[stage_in]]) { return in.col; }\n"
     "fragment float4 fsL(V in [[stage_in]]) { return in.col; }\n";

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:nil error:&e];
        if (!lib) { printf("%s\n", e.description.UTF8String); return 1; }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        int bad = 0;
        // 1. layers
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:16 height:16 mipmapped:NO];
        td.textureType = MTLTextureType2DArray; td.arrayLength = 4;
        td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeManaged;
        id<MTLTexture> arr = [dev newTextureWithDescriptor:td];
        MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
        rd.vertexFunction = [lib newFunctionWithName:@"vsL"]; rd.fragmentFunction = [lib newFunctionWithName:@"fsL"];
        rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        rd.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
        id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:rd error:&e];
        if (!ps) { printf("layered pipeline: %s\n", e.description.UTF8String); return 1; }
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor new];
        rp.colorAttachments[0].texture = arr; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore; rp.renderTargetArrayLength = 4;
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
        [re setRenderPipelineState:ps];
        [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3 instanceCount:4];
        [re endEncoding];
        id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder]; [be synchronizeResource:arr]; [be endEncoding];
        [cb commit]; [cb waitUntilCompleted];
        for (int s = 0; s < 4; s++) {
            uint8_t px[4];
            [arr getBytes:px bytesPerRow:64 bytesPerImage:1024 fromRegion:MTLRegionMake2D(8, 8, 1, 1) mipmapLevel:0 slice:s];
            const int wr = (int)(0.25 * (s + 1) * 255 + 0.5), wb = (int)((1 - 0.25 * s) * 255 + 0.5);
            const int ok = abs(px[0] - wr) <= 1 && abs(px[2] - wb) <= 1;
            printf("  slice %d: %3d %3d %3d %s\n", s, px[0], px[1], px[2], ok ? "ok" : "WRONG");
            bad += !ok;
        }
        // 2. viewports: left half red (viewport 0), right half green (viewport 1)
        MTLTextureDescriptor *t2 = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:32 height:16 mipmapped:NO];
        t2.usage = MTLTextureUsageRenderTarget; t2.storageMode = MTLStorageModeManaged;
        id<MTLTexture> tv = [dev newTextureWithDescriptor:t2];
        rd.vertexFunction = [lib newFunctionWithName:@"vsV"]; rd.fragmentFunction = [lib newFunctionWithName:@"fsV"];
        id<MTLRenderPipelineState> pv = [dev newRenderPipelineStateWithDescriptor:rd error:&e];
        if (!pv) { printf("viewport pipeline: %s\n", e.description.UTF8String); return 1; }
        MTLRenderPassDescriptor *rv = [MTLRenderPassDescriptor new];
        rv.colorAttachments[0].texture = tv; rv.colorAttachments[0].loadAction = MTLLoadActionClear;
        rv.colorAttachments[0].storeAction = MTLStoreActionStore;
        cb = [q commandBuffer];
        re = [cb renderCommandEncoderWithDescriptor:rv];
        [re setRenderPipelineState:pv];
        MTLViewport vps[2] = {{0, 0, 16, 16, 0, 1}, {16, 0, 16, 16, 0, 1}};
        [re setViewports:vps count:2];
        MTLScissorRect sc[2] = {{0, 0, 16, 16}, {16, 0, 16, 16}};
        [re setScissorRects:sc count:2];
        [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3 instanceCount:2];
        [re endEncoding];
        be = [cb blitCommandEncoder]; [be synchronizeResource:tv]; [be endEncoding];
        [cb commit]; [cb waitUntilCompleted];
        uint8_t l[4], r[4];
        [tv getBytes:l bytesPerRow:128 fromRegion:MTLRegionMake2D(4, 8, 1, 1) mipmapLevel:0];
        [tv getBytes:r bytesPerRow:128 fromRegion:MTLRegionMake2D(28, 8, 1, 1) mipmapLevel:0];
        const int okv = l[0] == 255 && l[1] == 0 && r[0] == 0 && r[1] == 255;
        printf("  viewports: left %d %d %d right %d %d %d %s\n", l[0], l[1], l[2], r[0], r[1], r[2], okv ? "ok" : "WRONG");
        bad += !okv;
        printf("metal_layered_test: %s\n", bad ? "FAIL" : "PASS");
        return bad != 0;
    }
}
