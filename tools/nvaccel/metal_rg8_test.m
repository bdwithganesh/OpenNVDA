// metal_rg8_test: small-texel formats through CPU upload and shader reads: RG8Unorm (RenderBox's
// 256x256 shape masks for Liquid Glass icon glyphs, which read as 1.0 everywhere on the RTX and
// left the App Store icon pale, 1 Oct 2026), R8Unorm, RG16Float, R16Float. Each texture gets
// texel (x, y) = (x*3+y, y*5+x) via replaceRegion (shared and private via blit), then a fragment
// pass reads every texel with read() and sample() into RGBA32Float.
#import <Metal/Metal.h>
#include <stdio.h>
#include <string.h>
static float h2f(uint16_t h) { return (float)*(__fp16 *)&h; }
int main(void)
{
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "struct VO { float4 p [[position]]; };\n"
             "vertex VO vs(uint v [[vertex_id]]) { float2 t[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)}; VO o; o.p = float4(t[v], 0, 1); return o; }\n"
             "fragment float4 rd(VO i [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {"
             " float4 a = t.read(uint2(i.p.xy)); float4 b = t.sample(s, i.p.xy / float2(t.get_width(), t.get_height())); return float4(a.x, a.y, b.x, b.y); }\n"
                                              options:nil error:&err];
        if (!lib) { printf("compile: %s\n", err.description.UTF8String); return 1; }
        MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
        rd.vertexFunction = [lib newFunctionWithName:@"vs"];
        rd.fragmentFunction = [lib newFunctionWithName:@"rd"];
        rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
        id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:rd error:&err];
        id<MTLSamplerState> smp = [dev newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        struct { const char *n; MTLPixelFormat f; int bpp, ch, half; } fm[] = {
            { "rg8unorm", MTLPixelFormatRG8Unorm, 2, 2, 0 }, { "r8unorm ", MTLPixelFormatR8Unorm, 1, 1, 0 },
            { "rg16f   ", MTLPixelFormatRG16Float, 4, 2, 1 }, { "r16f    ", MTLPixelFormatR16Float, 2, 1, 1 } };
        const int S = 64;
        int fails = 0;
        for (int k = 0; k < 4; k++)
            for (int priv = 0; priv < 2; priv++) {
                uint8_t src[S * S * 4];
                for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
                    const int v0 = (x * 3 + y) & 255, v1 = (y * 5 + x) & 255;
                    uint8_t *p = src + (y * S + x) * fm[k].bpp;
                    if (fm[k].half) { __fp16 h0 = v0 / 255.0f, h1 = v1 / 255.0f; memcpy(p, &h0, 2); if (fm[k].ch > 1) memcpy(p + 2, &h1, 2); }
                    else { p[0] = v0; if (fm[k].ch > 1) p[1] = v1; }
                }
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fm[k].f width:S height:S mipmapped:NO];
                td.usage = MTLTextureUsageShaderRead;
                td.storageMode = priv ? MTLStorageModePrivate : MTLStorageModeShared;
                id<MTLTexture> t = [dev newTextureWithDescriptor:td];
                id<MTLCommandBuffer> cb = [q commandBuffer];
                if (priv) {
                    id<MTLBuffer> sb = [dev newBufferWithBytes:src length:S * S * fm[k].bpp options:MTLResourceStorageModeShared];
                    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
                    [bl copyFromBuffer:sb sourceOffset:0 sourceBytesPerRow:S * fm[k].bpp sourceBytesPerImage:S * S * fm[k].bpp
                            sourceSize:MTLSizeMake(S, S, 1) toTexture:t destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
                    [bl endEncoding];
                } else [t replaceRegion:MTLRegionMake2D(0, 0, S, S) mipmapLevel:0 withBytes:src bytesPerRow:S * fm[k].bpp];
                td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:S height:S mipmapped:NO];
                td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
                id<MTLTexture> o = [dev newTextureWithDescriptor:td];
                MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
                p.colorAttachments[0].texture = o; p.colorAttachments[0].loadAction = MTLLoadActionClear;
                p.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:p];
                [re setRenderPipelineState:ps]; [re setFragmentTexture:t atIndex:0]; [re setFragmentSamplerState:smp atIndex:0];
                [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [re endEncoding];
                id<MTLBuffer> rb = [dev newBufferWithLength:S * S * 16 options:MTLResourceStorageModeShared];
                id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
                [bl copyFromTexture:o sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(S, S, 1)
                           toBuffer:rb destinationOffset:0 destinationBytesPerRow:S * 16 destinationBytesPerImage:S * S * 16];
                [bl endEncoding];
                [cb commit]; [cb waitUntilCompleted];
                const float *v = rb.contents;
                int bad = 0;
                for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
                    const float e0 = ((x * 3 + y) & 255) / 255.0f, e1 = fm[k].ch > 1 ? ((y * 5 + x) & 255) / 255.0f : 0;
                    const float *w = v + (y * S + x) * 4;
                    const float tol = fm[k].half ? 0.002f : 0.003f;
                    if (w[0] < e0 - tol || w[0] > e0 + tol || w[1] < e1 - tol || w[1] > e1 + tol || w[2] < e0 - tol || w[2] > e0 + tol) {
                        if (!bad) printf("  %s %s (%d,%d) read (%.3f %.3f) sample (%.3f %.3f) want (%.3f %.3f)\n", fm[k].n, priv ? "private" : "shared",
                                         x, y, w[0], w[1], w[2], w[3], e0, e1);
                        bad++;
                    }
                }
                printf("%s %s: %d wrong\n", fm[k].n, priv ? "private" : "shared ", bad);
                fails += bad != 0;
            }
        printf("metal_rg8_test on %s: %s\n", dev.name.UTF8String, fails ? "FAIL" : "PASS");
        return fails != 0;
    }
}
