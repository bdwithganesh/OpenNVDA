// metal_index_offset_test: indexed triangle strips from one shared 16-bit index buffer at
// different byte offsets, as RenderBox draws a rounded rectangle (ring at offset 768, inner quad
// at 1024 of the same buffer). Vertex ids select corners of a table; each draw must use the
// indices at its own offset. Draw A (offset 768) fills the left half, draw B (offset 1024) the
// right half; the indices at offset 0 would cover the whole target.
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
             "vertex VO vs(uint v [[vertex_id]]) { float2 t[12] = { float2(-1,-1), float2(1,-1), float2(-1,1), float2(1,1),"
             " float2(-1,-1), float2(0,-1), float2(-1,1), float2(0,1),   float2(0,-1), float2(1,-1), float2(0,1), float2(1,1) };"
             " VO o; o.p = float4(t[v], 0, 1); return o; }\n"
             "fragment half4 col(constant half4 &c [[buffer(0)]]) { return c; }\n" options:nil error:&err];
        if (!lib) { printf("compile: %s\n", err.description.UTF8String); return 1; }
        MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
        rd.vertexFunction = [lib newFunctionWithName:@"vs"];
        rd.fragmentFunction = [lib newFunctionWithName:@"col"];
        rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:rd error:&err];
        uint16_t idx[516] = {0};
        idx[0] = 0; idx[1] = 1; idx[2] = 2; idx[3] = 3;                       // offset 0: whole target
        idx[384] = 4; idx[385] = 5; idx[386] = 6; idx[387] = 7;               // offset 768: left half
        idx[512] = 8; idx[513] = 9; idx[514] = 10; idx[515] = 11;             // offset 1024: right half
        id<MTLBuffer> ib = [dev newBufferWithBytes:idx length:sizeof idx options:MTLResourceStorageModeShared];
        const int S = 32;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:S height:S mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> t = [dev newTextureWithDescriptor:td];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
        p.colorAttachments[0].texture = t; p.colorAttachments[0].loadAction = MTLLoadActionClear;
        p.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0); p.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:p];
        [re setRenderPipelineState:ps];
        const __fp16 red[4] = { 1, 0, 0, 1 }, green[4] = { 0, 1, 0, 1 };
        [re setFragmentBytes:red length:8 atIndex:0];
        [re drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip indexCount:4 indexType:MTLIndexTypeUInt16 indexBuffer:ib indexBufferOffset:768];
        [re setFragmentBytes:green length:8 atIndex:0];
        [re drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip indexCount:4 indexType:MTLIndexTypeUInt16 indexBuffer:ib indexBufferOffset:1024];
        [re endEncoding];
        id<MTLBuffer> rb = [dev newBufferWithLength:S * S * 4 options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
        [bl copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(S, S, 1)
                   toBuffer:rb destinationOffset:0 destinationBytesPerRow:S * 4 destinationBytesPerImage:S * S * 4];
        [bl endEncoding];
        [cb commit]; [cb waitUntilCompleted];
        const uint8_t *px = rb.contents;
        const uint8_t *l = px + (16 * S + 4) * 4, *r = px + (16 * S + 28) * 4;
        const int ok = l[0] == 255 && l[1] == 0 && r[0] == 0 && r[1] == 255;
        printf("metal_index_offset_test on %s: left (%u %u %u) want red, right (%u %u %u) want green: %s\n", dev.name.UTF8String,
               l[0], l[1], l[2], r[0], r[1], r[2], ok ? "PASS" : "FAIL");
        return !ok;
    }
}
