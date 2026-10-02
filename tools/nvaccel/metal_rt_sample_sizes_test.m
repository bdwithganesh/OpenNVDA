// metal_rt_sample_sizes_test: render a per-pixel pattern into a private render target, then read it
// back through a texture read in a second pass (and by blit), for sizes that are not powers of two
// (RenderBox's offscreen 96x96 RG16Float / BGRA8 targets came back streaky on the right, 1 Oct 2026).
// Pattern: c = (x / 255, y / 255) as RG16Float or BGRA8 (r = x, g = y).
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
             "vertex VO vs(uint v [[vertex_id]]) { float2 t[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)}; VO o; o.p = float4(t[v], 0, 1); return o; }\n"
             "fragment float4 pat(VO i [[stage_in]]) { return float4(floor(i.p.x) / 255.0, floor(i.p.y) / 255.0, 0, (uint(i.p.x) & 1) ? 1.0 : 0.0); }\n"
             "fragment float4 cpy(VO i [[stage_in]], texture2d<float> t [[texture(0)]], sampler sm [[sampler(0)]]) { float4 a = t.read(uint2(i.p.xy)); float4 b = t.sample(sm, i.p.xy / float2(t.get_width(), t.get_height())); return float4(a.x, a.y, a.w, b.w + b.x * 0.0 + (abs(b.x - a.x) > 0.01 ? 10.0 : 0.0)); }\n" options:nil error:&err];
        if (!lib) { printf("compile: %s\n", err.description.UTF8String); return 1; }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        const int sizes[] = { 64, 96, 86, 100, 130, 72 };
        const MTLPixelFormat fmts[] = { MTLPixelFormatRG16Float, MTLPixelFormatBGRA8Unorm, MTLPixelFormatRGB10A2Unorm, MTLPixelFormatRGBA16Float };
        const char *fnames[] = { "rg16f  ", "bgra8  ", "rgb10a2", "rgba16f" };
        int fails = 0;
        for (int fi = 0; fi < 4; fi++)
            for (unsigned si = 0; si < sizeof sizes / sizeof sizes[0]; si++) {
                const int S = sizes[si];
                const MTLPixelFormat f = fmts[fi];
                MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
                rd.vertexFunction = [lib newFunctionWithName:@"vs"];
                rd.fragmentFunction = [lib newFunctionWithName:@"pat"];
                rd.colorAttachments[0].pixelFormat = f;
                id<MTLRenderPipelineState> pp = [dev newRenderPipelineStateWithDescriptor:rd error:&err];
                rd.fragmentFunction = [lib newFunctionWithName:@"cpy"];
                rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
                id<MTLRenderPipelineState> pc = [dev newRenderPipelineStateWithDescriptor:rd error:&err];
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:S height:S mipmapped:NO];
                td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModePrivate;
                id<MTLTexture> a = [dev newTextureWithDescriptor:td];
                td.pixelFormat = MTLPixelFormatRGBA32Float;
                id<MTLTexture> b = [dev newTextureWithDescriptor:td];
                id<MTLCommandBuffer> cb = [q commandBuffer];
                MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
                p.colorAttachments[0].texture = a; p.colorAttachments[0].loadAction = MTLLoadActionClear;
                p.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:p];
                [re setRenderPipelineState:pp];
                [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [re endEncoding];
                p = [MTLRenderPassDescriptor renderPassDescriptor];
                p.colorAttachments[0].texture = b; p.colorAttachments[0].loadAction = MTLLoadActionClear;
                p.colorAttachments[0].storeAction = MTLStoreActionStore;
                re = [cb renderCommandEncoderWithDescriptor:p];
                [re setRenderPipelineState:pc];
                [re setFragmentTexture:a atIndex:0];
                [re setFragmentSamplerState:[dev newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]] atIndex:0];
                [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [re endEncoding];
                id<MTLBuffer> rb = [dev newBufferWithLength:S * S * 16 options:MTLResourceStorageModeShared];
                id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
                [bl copyFromTexture:b sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(S, S, 1)
                           toBuffer:rb destinationOffset:0 destinationBytesPerRow:S * 16 destinationBytesPerImage:S * S * 16];
                [bl endEncoding];
                [cb commit]; [cb waitUntilCompleted];
                const float *o = rb.contents;
                int bad = 0, fx = -1, fy = -1;
                for (int y = 0; y < S; y++)
                    for (int x = 0; x < S; x++) {
                        const float *v = o + (y * S + x) * 4;
                        const float wa = fi == 0 ? 1.0f : (x & 1) ? 1.0f : 0.0f;   // RG16F has no alpha (reads 1)
                        const float tol = fi == 2 ? 0.003f : 0.004f;
                        if (v[0] < x / 255.0f - tol || v[0] > x / 255.0f + tol || v[1] < y / 255.0f - tol || v[1] > y / 255.0f + tol ||
                            v[2] != wa || v[3] != wa) {
                            if (!bad) { fx = x; fy = y; printf("  got (%.3f %.3f a %.3f s %.3f) ", v[0], v[1], v[2], v[3]); }
                            bad++;
                        }
                    }
                printf("%s %3dx%-3d: %d texels wrong%s", fnames[fi], S, S, bad, bad ? "" : "\n");
                if (bad) printf(" (first at %d,%d)\n", fx, fy);
                fails += bad != 0;
            }
        printf("metal_rt_sample_sizes_test on %s: %s\n", dev.name.UTF8String, fails ? "FAIL" : "PASS");
        return fails != 0;
    }
}
