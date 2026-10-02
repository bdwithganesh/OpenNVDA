// metal_raster_edge_test: which pixels a quad with edges on exact pixel boundaries covers, and the
// half-pixel cases, against Metal's rule (pixel centres at .5, top-left fill). RenderBox draws a
// rounded rectangle as an anti-aliased ring plus an opaque inner quad whose edges sit on pixel
// boundaries; a quad short by one column left a seam at 74% alpha in every icon (1 Oct 2026).
// For each left edge x0 the quad spans [x0, 118] x [10, 118] in pixels; prints the first and
// last covered column and row.
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
             "vertex VO vs(uint v [[vertex_id]], constant float4 &r [[buffer(0)]]) {"
             " float2 t[4] = {r.xy, float2(r.z, r.y), float2(r.x, r.w), r.zw};"
             " float2 n = t[v] / 64.0 - 1.0; VO o; o.p = float4(n.x, -n.y, 0, 1); return o; }\n"
             "fragment half4 white() { return half4(1); }\n"
                                              options:nil error:&err];
        if (!lib) { printf("compile: %s\n", err.description.UTF8String); return 1; }
        MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
        rd.vertexFunction = [lib newFunctionWithName:@"vs"];
        rd.fragmentFunction = [lib newFunctionWithName:@"white"];
        rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:rd error:&err];
        const int S = 128;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:S height:S mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> t = [dev newTextureWithDescriptor:td];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLBuffer> rb = [dev newBufferWithLength:S * S * 4 options:MTLResourceStorageModeShared];
        const float x0s[] = { 10.0f, 10.25f, 10.5f, 10.75f, 9.999f };
        int fails = 0;
        for (unsigned k = 0; k < sizeof x0s / sizeof x0s[0]; k++) {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
            p.colorAttachments[0].texture = t;
            p.colorAttachments[0].loadAction = MTLLoadActionClear;
            p.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
            p.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:p];
            const float r[4] = { x0s[k], 10, 118, 118 };
            [re setRenderPipelineState:ps];
            [re setVertexBytes:r length:16 atIndex:0];
            [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            [re endEncoding];
            id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
            [bl copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(S, S, 1)
                       toBuffer:rb destinationOffset:0 destinationBytesPerRow:S * 4 destinationBytesPerImage:S * S * 4];
            [bl endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            const uint8_t *px = rb.contents;
            int c0 = -1, c1 = -1, r0 = -1, r1 = -1;
            for (int x = 0; x < S; x++) if (px[(64 * S + x) * 4]) { if (c0 < 0) c0 = x; c1 = x; }
            for (int y = 0; y < S; y++) if (px[(y * S + 64) * 4]) { if (r0 < 0) r0 = y; r1 = y; }
            // centres at .5: x0 = 10 .. 10.5 covers column 10 (10.5 on the edge: top-left rule includes it)
            const int want0 = x0s[k] <= 10.5f ? 10 : 11;
            const int ok = c0 == want0 && c1 == 117 && r0 == 10 && r1 == 117;
            printf("x0 %-6.3f columns %d..%d rows %d..%d (want %d..117, 10..117) %s\n", x0s[k], c0, c1, r0, r1, want0, ok ? "ok" : "FAIL");
            fails += !ok;
        }
        printf("metal_raster_edge_test on %s: %s\n", dev.name.UTF8String, fails ? "FAIL" : "PASS");
        return fails ? 1 : 0;
    }
}
