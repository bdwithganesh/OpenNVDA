// metal_restart_test: indexed triangle and line strips restart at the all-ones index (0xffff for
// uint16, 0xffffffff for uint32), always, in Metal. Two quads separated by a restart must not be
// joined by the strip's bridging triangles. RenderBox's path hulls are restarted strips; without
// restart every vector glyph got streaks (1 Oct 2026).
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
             "vertex VO vs(uint v [[vertex_id]]) { float2 t[8] = { float2(-1,-1), float2(-1,1), float2(-0.5,-1), float2(-0.5,1),"
             " float2(0.5,-1), float2(0.5,1), float2(1,-1), float2(1,1) }; VO o; o.p = float4(v < 8 ? t[v] : float2(0), 0, 1); return o; }\n"
             "fragment half4 fs() { return half4(1); }\n" options:nil error:&err];
        if (!lib) { printf("compile: %s\n", err.description.UTF8String); return 1; }
        MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
        rd.vertexFunction = [lib newFunctionWithName:@"vs"];
        rd.fragmentFunction = [lib newFunctionWithName:@"fs"];
        rd.colorAttachments[0].pixelFormat = MTLPixelFormatR8Unorm;
        id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:rd error:&err];
        const int S = 32;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm width:S height:S mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> t = [dev newTextureWithDescriptor:td];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        int fails = 0;
        for (int wide = 0; wide < 2; wide++) {
            const uint16_t i16[9] = { 0, 1, 2, 3, 0xffff, 4, 5, 6, 7 };
            const uint32_t i32[9] = { 0, 1, 2, 3, 0xffffffffu, 4, 5, 6, 7 };
            id<MTLBuffer> ib = wide ? [dev newBufferWithBytes:i32 length:sizeof i32 options:0] : [dev newBufferWithBytes:i16 length:sizeof i16 options:0];
            id<MTLCommandBuffer> cb = [q commandBuffer];
            MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
            p.colorAttachments[0].texture = t; p.colorAttachments[0].loadAction = MTLLoadActionClear;
            p.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:p];
            [re setRenderPipelineState:ps];
            [re drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip indexCount:9 indexType:wide ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                          indexBuffer:ib indexBufferOffset:0];
            [re endEncoding];
            id<MTLBuffer> rb = [dev newBufferWithLength:S * S options:MTLResourceStorageModeShared];
            id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
            [bl copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(S, S, 1)
                       toBuffer:rb destinationOffset:0 destinationBytesPerRow:S destinationBytesPerImage:S * S];
            [bl endEncoding];
            [cb commit]; [cb waitUntilCompleted];
            const uint8_t *o = rb.contents;
            // quads cover columns 0..7 and 24..31; the gap 8..23 must stay empty
            int gap = 0, quads = 0;
            for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
                if (x >= 9 && x <= 22) gap += o[y * S + x] != 0;
                if (x <= 6 || x >= 25) quads += o[y * S + x] != 0;
            }
            const int ok = gap == 0 && quads == 2 * 7 * S;
            printf("%s indices: gap pixels %d (want 0), quad pixels %d (want %d) %s\n", wide ? "uint32" : "uint16", gap, quads, 2 * 7 * S, ok ? "ok" : "FAIL");
            fails += !ok;
        }
        printf("metal_restart_test on %s: %s\n", dev.name.UTF8String, fails ? "FAIL" : "PASS");
        return fails != 0;
    }
}
