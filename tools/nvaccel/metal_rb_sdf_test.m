// metal_rb_sdf_test: RenderBox's rounded-rect coverage (from the NIR of its ring fragment shader,
// 1 Oct 2026: superellipse-ish corner SDF, fwidth AA in half precision, smoothstep) on a full-target
// quad with the globals an icon used. Writes coverage to R8; prints a checksum and the coverage
// along row 40, to compare the RTX with the M1 pixel for pixel.
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void)
{
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "struct VO { float4 p [[position]]; float2 q [[center_no_perspective]]; };\n"
             "vertex VO vs(uint v [[vertex_id]]) { float2 c = float2(v & 1, v >> 1); VO o;"
             " o.p = float4(c.x * 2.03125 - 1.015625, 1.015625 - c.y * 2.03125, 0, 1); o.q = c - 0.5; return o; }\n"
             "fragment float4 fs(VO i [[stage_in]], texture2d<float> nz [[texture(0)]], sampler sm [[sampler(0)]],"
             " constant int &getenv_discard [[buffer(0)]], constant int &getenv_tex [[buffer(1)]]) { const float2 b = float2(0.217692); const float r = 0.274615;"
             " float2 q = abs(i.q) - b; float2 m = max(q, 0.0); float len = sqrt(dot(m, m)); float mn = min(m.x, m.y), mx = max(m.x, m.y);"
             " float t = max(r - len, 0.0); float2 v = float2(mn, t * 0.25 + mx); float den = dot(v, float2(0.361, 0.639));"
             " float k = den != 0.0 ? mn / den : 0.0; float k3 = k * k * k * ((k * -0.53841 + 1.346025) * k - 0.89735);"
             " float s = k3 * r + len; float d = min(max(q.x, q.y), 0.0) + s - r;"
             " half h = half(fwidth(d)) * 0.5h; half c = clamp((h - half(d)) / (h + h), 0.0h, 1.0h); c = c * c * (3.0h - 2.0h * c);"
             " if (getenv_discard != 0 && c == 0.0h) discard_fragment();"
             " if (getenv_tex != 0) { float4 n = nz.sample(sm, i.p.xy * 0.03125); c = c * half(0.5 + 0.5 * n.r); }"
             " return float4(float(c), 0, 0, 1); }\n" options:nil error:&err];
        if (!lib) { printf("compile: %s\n", err.description.UTF8String); return 1; }
        MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
        rd.vertexFunction = [lib newFunctionWithName:@"vs"];
        rd.fragmentFunction = [lib newFunctionWithName:@"fs"];
        rd.colorAttachments[0].pixelFormat = MTLPixelFormatR8Unorm;
        id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:rd error:&err];
        const int S = 128;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm width:S height:S mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> t = [dev newTextureWithDescriptor:td];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
        p.colorAttachments[0].texture = t; p.colorAttachments[0].loadAction = MTLLoadActionClear;
        p.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:p];
        [re setRenderPipelineState:ps];
        // variants: SDF_DISCARD=1 discards zero coverage, SDF_TEX=1 samples a constant 1.0 noise texture after it
        const int dsc = getenv("SDF_DISCARD") != NULL, tx = getenv("SDF_TEX") != NULL;
        [re setFragmentBytes:&dsc length:4 atIndex:0];
        [re setFragmentBytes:&tx length:4 atIndex:1];
        MTLTextureDescriptor *nd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:32 height:32 mipmapped:NO];
        id<MTLTexture> nzt = [dev newTextureWithDescriptor:nd];
        uint8_t ones[32 * 32 * 4]; memset(ones, 255, sizeof ones);
        [nzt replaceRegion:MTLRegionMake2D(0, 0, 32, 32) mipmapLevel:0 withBytes:ones bytesPerRow:128];
        [re setFragmentTexture:nzt atIndex:0];
        [re setFragmentSamplerState:[dev newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]] atIndex:0];
        [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [re endEncoding];
        id<MTLBuffer> rb = [dev newBufferWithLength:S * S options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
        [bl copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(S, S, 1)
                   toBuffer:rb destinationOffset:0 destinationBytesPerRow:S destinationBytesPerImage:S * S];
        [bl endEncoding];
        [cb commit]; [cb waitUntilCompleted];
        const uint8_t *o = rb.contents;
        unsigned sum = 0, full = 0;
        for (int i = 0; i < S * S; i++) { sum = sum * 31 + o[i]; full += o[i] == 255; }
        printf("metal_rb_sdf_test on %s: full %u checksum %08x\n row 40:", dev.name.UTF8String, full, sum);
        for (int x = 0; x < 24; x++) printf(" %u", o[40 * S + x]);
        printf("\n row 2: ");
        for (int x = 0; x < 24; x++) printf(" %u", o[2 * S + x]);
        printf("\n");
        FILE *f = fopen(getenv("SDF_RAW") ? getenv("SDF_RAW") : "/dev/null", "wb"); if (f) { fwrite(o, 1, S * S, f); fclose(f); }
        return 0;
    }
}
