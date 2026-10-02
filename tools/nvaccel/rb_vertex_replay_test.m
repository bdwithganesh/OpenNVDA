// rb_vertex_replay_test: RenderBox's own primitive_gradient_vertex (from the system RenderBox
// default.metallib) with the globals and indices an icon's rounded-rect ring used (captured with
// NVMTL_DRAWLOG_VB, 1 Oct 2026), and a fragment function of ours that writes the interpolated
// primitive_position. Compare the coverage mask and varyings between the RTX and the M1.
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int main(int argc, char **argv)
{
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *err = nil;
        id<MTLLibrary> rb = [dev newLibraryWithURL:[NSURL fileURLWithPath:
            @"/System/Library/PrivateFrameworks/RenderBox.framework/Versions/A/Resources/default.metallib"] error:&err];
        id<MTLFunction> vf = [rb newFunctionWithName:@"primitive_gradient_vertex"];
        if (!vf) { printf("no primitive_gradient_vertex: %s\n", err.description.UTF8String); return 1; }
        id<MTLLibrary> lib = [dev newLibraryWithSource:
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "struct VO { float4 p [[position]]; float2 primitive_position [[center_no_perspective]]; float2 gradient_coord [[center_no_perspective]]; };\n"
             "fragment float4 fs(VO i [[stage_in]]) { return float4(i.primitive_position, i.gradient_coord.x, 1); }\n"
             // RenderBox's ring coverage (see metal_rb_sdf_test), discard at zero, written as .z
             "fragment float4 sdf(VO i [[stage_in]]) { const float2 b = float2(0.217692); const float r = 0.274615;"
             " float2 q = abs(i.primitive_position) - b; float2 m = max(q, 0.0); float len = sqrt(dot(m, m)); float mn = min(m.x, m.y), mx = max(m.x, m.y);"
             " float t = max(r - len, 0.0); float2 v = float2(mn, t * 0.25 + mx); float den = dot(v, float2(0.361, 0.639));"
             " float k = den != 0.0 ? mn / den : 0.0; float k3 = k * k * k * ((k * -0.53841 + 1.346025) * k - 0.89735);"
             " float s = k3 * r + len; float d = min(max(q.x, q.y), 0.0) + s - r;"
             " half h = half(fwidth(d)) * 0.5h; half c = clamp((h - half(d)) / (h + h), 0.0h, 1.0h); c = c * c * (3.0h - 2.0h * c);"
             " if (c == 0.0h) discard_fragment(); return float4(i.primitive_position, float(c), 1); }\n"
             // the same with RenderBox's tail: a noise-texture sample (implicit LOD) after the discard feeding the colour
             "fragment float4 sdftex(VO i [[stage_in]], texture2d<float> nz [[texture(0)]], sampler sm [[sampler(0)]]) {"
             " const float2 b = float2(0.217692); const float r = 0.274615;"
             " float2 q = abs(i.primitive_position) - b; float2 m = max(q, 0.0); float len = sqrt(dot(m, m)); float mn = min(m.x, m.y), mx = max(m.x, m.y);"
             " float t = max(r - len, 0.0); float2 v = float2(mn, t * 0.25 + mx); float den = dot(v, float2(0.361, 0.639));"
             " float k = den != 0.0 ? mn / den : 0.0; float k3 = k * k * k * ((k * -0.53841 + 1.346025) * k - 0.89735);"
             " float s = k3 * r + len; float d = min(max(q.x, q.y), 0.0) + s - r;"
             " half h = half(fwidth(d)) * 0.5h; half c = clamp((h - half(d)) / (h + h), 0.0h, 1.0h); c = c * c * (3.0h - 2.0h * c);"
             " if (c == 0.0h) discard_fragment(); float4 n = nz.sample(sm, i.p.xy * 0.03125);"
             " return float4(i.primitive_position.x + n.r * 0.0, n.g, float(c), 1); }\n" options:nil error:&err];
        if (!lib) { printf("compile: %s\n", err.description.UTF8String); return 1; }
        MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
        rd.vertexFunction = vf;
        const int useSdf = argc > 2 && argv[2][0] == 's', useTex = argc > 2 && argv[2][0] == 't';
        rd.fragmentFunction = [lib newFunctionWithName:useTex ? @"sdftex" : useSdf ? @"sdf" : @"fs"];
        const int ios = getenv("IOS") != NULL;
        rd.colorAttachments[0].pixelFormat = ios ? MTLPixelFormatRGBA8Unorm : MTLPixelFormatRGBA32Float;
        // RB=1: the icon draw's state: premultiplied source-over on colour 0, an RG16Float colour 1 with
        // write mask 0 and load DontCare
        const int rbState = getenv("RB") != NULL;
        if (rbState) {
            rd.colorAttachments[0].blendingEnabled = YES;
            rd.colorAttachments[0].sourceRGBBlendFactor = rd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
            rd.colorAttachments[0].destinationRGBBlendFactor = rd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            rd.colorAttachments[1].pixelFormat = MTLPixelFormatRG16Float;
            rd.colorAttachments[1].writeMask = MTLColorWriteMaskNone;
        }
        id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:rd error:&err];
        if (!ps) { printf("pipeline: %s\n", err.description.UTF8String); return 1; }
        // exact words of the icon draw's globals (NVMTL_DRAWLOG_VB hex, 1 Oct 2026)
        const uint32_t gw[36] = { 0x40020000, 0x80000000, 0, 0xc0020000, 0xbf820000, 0x3f820000, 0x3e5eeabb, 0x3e5eeabb, 0x3e8c9a63,
            0x420ecccd, 0x420ecccd, 0x420ecccd, 0x3dc43c05, 0x3dc43c05, 0x3dc43c05, 0x3dc43c05, 0x3f800000, 0x3f800000, 0, 0,
            0x00003c00, 0, 0xbf000000, 0xbf000000, 0x80000000, 0x3f000000, 0, 0x439b6770, 0x00007ff8, 8, 0, 0x80, 0, 0, 0x00024000, 0 };
        const float *g = (const float *)gw;
        const uint16_t ring[10] = { 4, 0, 5, 1, 6, 2, 7, 3, 4, 0 }, inner[4] = { 5, 4, 6, 7 };
        const int useInner = argc > 1 && argv[1][0] == 'i';
        id<MTLBuffer> ib = [dev newBufferWithBytes:useInner ? (const void *)inner : (const void *)ring length:useInner ? 8 : 20
                                           options:MTLResourceStorageModeShared];
        const int S = 128;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:S height:S mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> t = [dev newTextureWithDescriptor:td];
        IOSurfaceRef surf = NULL;
        if (ios) {   // IOS=1: an RGBA8 IOSurface target like ImageRenderer's
            surf = IOSurfaceCreate((__bridge CFDictionaryRef)@{ (id)kIOSurfaceWidth: @(S), (id)kIOSurfaceHeight: @(S),
                (id)kIOSurfaceBytesPerElement: @4, (id)kIOSurfaceBytesPerRow: @(S * 4), (id)kIOSurfacePixelFormat: @((uint32_t)'RGBA') });
            MTLTextureDescriptor *sd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:S height:S mipmapped:NO];
            sd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            t = [dev newTextureWithDescriptor:sd iosurface:surf plane:0];
        }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
        p.colorAttachments[0].texture = t; p.colorAttachments[0].loadAction = MTLLoadActionClear;
        p.colorAttachments[0].clearColor = MTLClearColorMake(9, 9, 9, 0); p.colorAttachments[0].storeAction = MTLStoreActionStore;
        if (rbState) {
            MTLTextureDescriptor *t1 = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG16Float width:S height:S mipmapped:NO];
            t1.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; t1.storageMode = MTLStorageModePrivate;
            p.colorAttachments[1].texture = [dev newTextureWithDescriptor:t1];
            p.colorAttachments[1].loadAction = MTLLoadActionDontCare;
            p.colorAttachments[1].storeAction = MTLStoreActionDontCare;
            p.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        }
        id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:p];
        [re setRenderPipelineState:ps];
        MTLTextureDescriptor *nd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:32 height:32 mipmapped:NO];
        id<MTLTexture> nzt = [dev newTextureWithDescriptor:nd];
        uint8_t ones[32 * 32 * 4]; memset(ones, 255, sizeof ones);
        [nzt replaceRegion:MTLRegionMake2D(0, 0, 32, 32) mipmapLevel:0 withBytes:ones bytesPerRow:128];
        [re setFragmentTexture:nzt atIndex:0];
        MTLSamplerDescriptor *sdsc = [MTLSamplerDescriptor new];
        sdsc.sAddressMode = sdsc.tAddressMode = MTLSamplerAddressModeRepeat;
        [re setFragmentSamplerState:[dev newSamplerStateWithDescriptor:sdsc] atIndex:0];
        [re setVertexBytes:g length:sizeof gw atIndex:0];
        [re setVertexBytes:g length:sizeof gw atIndex:1];
        [re drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip indexCount:useInner ? 4 : 10 indexType:MTLIndexTypeUInt16 indexBuffer:ib indexBufferOffset:0];
        [re endEncoding];
        id<MTLBuffer> out = [dev newBufferWithLength:S * S * 16 options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
        [bl copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(S, S, 1)
                   toBuffer:out destinationOffset:0 destinationBytesPerRow:S * 16 destinationBytesPerImage:S * S * 16];
        [bl endEncoding];
        [cb commit]; [cb waitUntilCompleted];
        if (ios) {   // expand to the float layout the checks below read: alpha -> w, coverage (blue channel) -> z
            [cb waitUntilCompleted];
            IOSurfaceLock(surf, kIOSurfaceLockReadOnly, NULL);
            const uint8_t *b = IOSurfaceGetBaseAddress(surf);
            float *f = out.contents;
            for (int i = 0; i < S * S; i++) { f[i * 4] = f[i * 4 + 1] = 0; f[i * 4 + 2] = b[i * 4 + 2] / 255.0f; f[i * 4 + 3] = b[i * 4 + 3] / 255.0f; }
            IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
        }
        const float *o = out.contents;
        int cov = 0;
        for (int i = 0; i < S * S; i++) cov += o[i * 4 + 3] != 0;
        int full = 0;
        for (int i = 0; i < S * S; i++) full += o[i * 4 + 3] != 0 && o[i * 4 + 2] >= 0.995f;
        printf("rb_vertex_replay_test %s%s on %s: %d pixels covered, %d with coverage 1\n", useInner ? "inner" : "ring",
               useTex ? " sdf+tex" : useSdf ? " sdf" : "", dev.name.UTF8String, cov, full);
        if (useSdf || useTex) {
            for (int y = 0; y < S; y += 1) {   // holes: covered-by-geometry pixels in the band whose coverage is not 1
                int n = 0; for (int x = 0; x < 11; x++) n += o[(y * S + x) * 4 + 3] == 0 || o[(y * S + x) * 4 + 2] < 0.995f;
                if (y > 12 && y < 115 && n) printf("  row %d: %d of cols 0..10 not full\n", y, n);
            }
        }
        for (int y = 2; y <= 64; y += 62 / 2 > 0 ? 19 : 1) {
            printf(" row %2d:", y);
            for (int x = 0; x < 16; x++) { const float *v = o + (y * S + x) * 4; if (v[3] == 0) printf("   ----   "); else printf(" %+.3f/%+.3f", v[0], v[1]); }
            printf("\n");
        }
        return 0;
    }
}
