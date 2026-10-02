// metal_fragcoord_test: [[position]] in a fragment function is the pixel centre (x + 0.5, y + 0.5,
// z, 1/w). RenderBox measures distances to shape edges from it for anti-aliasing; half a pixel off
// puts a 75% alpha seam on every inner edge.
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
             "vertex VO vs(uint v [[vertex_id]]) { float2 t[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)}; VO o; o.p = float4(t[v], 0.25, 1); return o; }\n"
             "fragment float4 pos(VO i [[stage_in]]) { return i.p; }\n" options:nil error:&err];
        if (!lib) { printf("compile: %s\n", err.description.UTF8String); return 1; }
        MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
        rd.vertexFunction = [lib newFunctionWithName:@"vs"];
        rd.fragmentFunction = [lib newFunctionWithName:@"pos"];
        rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
        id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:rd error:&err];
        const int S = 32;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:S height:S mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> t = [dev newTextureWithDescriptor:td];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
        p.colorAttachments[0].texture = t; p.colorAttachments[0].loadAction = MTLLoadActionClear;
        p.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:p];
        [re setRenderPipelineState:ps];
        [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [re endEncoding];
        id<MTLBuffer> rb = [dev newBufferWithLength:S * S * 16 options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
        [bl copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(S, S, 1)
                   toBuffer:rb destinationOffset:0 destinationBytesPerRow:S * 16 destinationBytesPerImage:S * S * 16];
        [bl endEncoding];
        [cb commit]; [cb waitUntilCompleted];
        const float *v = (const float *)rb.contents + (20 * S + 10) * 4;
        const int ok = v[0] == 10.5f && v[1] == 20.5f && v[2] > 0.249f && v[2] < 0.251f && v[3] == 1.0f;
        printf("metal_fragcoord_test on %s: pixel (10,20) position (%.3f %.3f %.3f %.3f) want (10.5 20.5 0.25 1): %s\n",
               dev.name.UTF8String, v[0], v[1], v[2], v[3], ok ? "PASS" : "FAIL");
        return !ok;
    }
}
