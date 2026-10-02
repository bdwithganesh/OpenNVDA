// metal_fbfetch_test: framebuffer fetch ([[color(n)]] fragment inputs), the way RenderBox
// (IconRendering, SwiftUI) composites: colour 0 is the image, colour 1 an RG16Float scratch
// attachment that is never stored. Blank Dock icons on Tahoe (1 Oct 2026) came from reads of
// [[color(n)]] returning 0. One render pass, 64x64, draws in order:
//   1 fill c0 = (1, 0, 0, 1) and c1 = (0.25, 0.75)
//   2 left half:  c0 = dst * 0.5                         (destination-in mask)
//   3 top half:   c0 = dst + (0, 0.25, 0, 0)             (reads draw 2's result where they overlap)
//   4 right half: c0 = (dst.r, c1.x, c1.y, dst.a)         (reads the scratch attachment)
// Then checks one pixel per quadrant. Usage: metal_fbfetch_test [rgba16f|bgra8|rgb10a2]
#import <Metal/Metal.h>
#include <stdio.h>
#include <string.h>

static float half2f(uint16_t h) { return (float)*(__fp16 *)&h; }

int main(int argc, char **argv)
{
    @autoreleasepool {
        const char *fmtName = argc > 1 ? argv[1] : "rgba16f";
        MTLPixelFormat fmt = !strcmp(fmtName, "bgra8") ? MTLPixelFormatBGRA8Unorm
                           : !strcmp(fmtName, "rgb10a2") ? MTLPixelFormatRGB10A2Unorm : MTLPixelFormatRGBA16Float;
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { printf("no Metal device\n"); return 1; }
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "struct VO { float4 p [[position]]; };\n"
             "struct FO { float4 c0 [[color(0)]]; float2 c1 [[color(1)]]; };\n"
             "vertex VO vs(uint v [[vertex_id]], constant float4 &r [[buffer(0)]]) {"
             " float2 t[4] = {r.xy, float2(r.z, r.y), float2(r.x, r.w), r.zw}; VO o; o.p = float4(t[v], 0, 1); return o; }\n"
             "fragment FO fill() { FO o; o.c0 = float4(1, 0, 0, 1); o.c1 = float2(0.25, 0.75); return o; }\n"
             "fragment FO maskhalf(float4 d [[color(0)]], float2 s [[color(1)]]) { FO o; o.c0 = d * 0.5; o.c1 = s; return o; }\n"
             "fragment FO addg(float4 d [[color(0)]], float2 s [[color(1)]]) { FO o; o.c0 = d + float4(0, 0.25, 0, 0); o.c1 = s; return o; }\n"
             "fragment FO scratch(float4 d [[color(0)]], float2 s [[color(1)]]) { FO o; o.c0 = float4(d.r, s.x, s.y, d.a); o.c1 = s; return o; }\n"
                                              options:nil error:&err];
        if (!lib) { printf("compile: %s\n", err.description.UTF8String); return 1; }
        NSArray *names = @[ @"fill", @"maskhalf", @"addg", @"scratch" ];
        id<MTLRenderPipelineState> ps[4];
        for (int i = 0; i < 4; i++) {
            MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
            rd.vertexFunction = [lib newFunctionWithName:@"vs"];
            rd.fragmentFunction = [lib newFunctionWithName:names[i]];
            rd.colorAttachments[0].pixelFormat = fmt;
            rd.colorAttachments[1].pixelFormat = MTLPixelFormatRG16Float;
            ps[i] = [dev newRenderPipelineStateWithDescriptor:rd error:&err];
            if (!ps[i]) { printf("pipeline %s: %s\n", [names[i] UTF8String], err.description.UTF8String); return 1; }
        }
        const int S = 64;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:S height:S mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> c0 = [dev newTextureWithDescriptor:td];
        td.pixelFormat = MTLPixelFormatRG16Float;
        td.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> c1 = [dev newTextureWithDescriptor:td];
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
        id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:p];
        const float rects[4][4] = { { -1, -1, 1, 1 }, { -1, -1, 0, 1 }, { -1, 0, 1, 1 }, { 0, -1, 1, 1 } };
        for (int i = 0; i < 4; i++) {
            [re setRenderPipelineState:ps[i]];
            [re setVertexBytes:rects[i] length:16 atIndex:0];
            [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        }
        [re endEncoding];
        id<MTLBuffer> rb = [dev newBufferWithLength:S * S * 8 options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
        const NSUInteger bpp = fmt == MTLPixelFormatRGBA16Float ? 8 : 4;
        [bl copyFromTexture:c0 sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(S, S, 1)
                   toBuffer:rb destinationOffset:0 destinationBytesPerRow:S * bpp destinationBytesPerImage:S * S * bpp];
        [bl endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        if (cb.status != MTLCommandBufferStatusCompleted) { printf("cb status %ld\n", (long)cb.status); return 1; }
        // quadrant pixels: y grows downward in the texture, NDC top = row 0
        struct { const char *name; int x, y; float e[4]; } chk[] = {
            { "top-left (mask, +g)", 16, 16, { 0.5f, 0.25f, 0, 0.5f } },
            { "bottom-left (mask)", 16, 48, { 0.5f, 0, 0, 0.5f } },
            { "top-right (+g, scratch)", 48, 16, { 1, 0.25f, 0.75f, 1 } },
            { "bottom-right (scratch)", 48, 48, { 1, 0.25f, 0.75f, 1 } },
        };
        int fails = 0;
        for (unsigned k = 0; k < 4; k++) {
            const uint8_t *px = (const uint8_t *)rb.contents + (chk[k].y * S + chk[k].x) * bpp;
            float v[4];
            if (fmt == MTLPixelFormatRGBA16Float) for (int i = 0; i < 4; i++) v[i] = half2f(((const uint16_t *)px)[i]);
            else if (fmt == MTLPixelFormatBGRA8Unorm) { v[0] = px[2] / 255.f; v[1] = px[1] / 255.f; v[2] = px[0] / 255.f; v[3] = px[3] / 255.f; }
            else { uint32_t w = *(const uint32_t *)px; v[0] = (w & 1023) / 1023.f; v[1] = (w >> 10 & 1023) / 1023.f;
                   v[2] = (w >> 20 & 1023) / 1023.f; v[3] = (w >> 30) / 3.f; }
            // rgb10a2 alpha has 2 bits: 0.5 is stored as 0.333 or 0.667, and the next read sees that
            const float tolA = fmt == MTLPixelFormatRGB10A2Unorm ? 0.34f : 0.02f;
            int ok = 1;
            for (int i = 0; i < 4; i++) {
                const float tol = i == 3 ? tolA : (fmt == MTLPixelFormatRGB10A2Unorm && k < 2 ? 0.2f : 0.02f);
                if (v[i] < chk[k].e[i] - tol || v[i] > chk[k].e[i] + tol) ok = 0;
            }
            printf("%-26s got (%.3f %.3f %.3f %.3f) want (%.2f %.2f %.2f %.2f) %s\n", chk[k].name, v[0], v[1], v[2], v[3],
                   chk[k].e[0], chk[k].e[1], chk[k].e[2], chk[k].e[3], ok ? "ok" : "FAIL");
            fails += !ok;
        }
        printf("metal_fbfetch_test %s on %s: %s\n", fmtName, dev.name.UTF8String, fails ? "FAIL" : "PASS");
        return fails ? 1 : 0;
    }
}
