// metal_deriv_edges_test: fine derivatives in 2x2 quads that straddle triangle edges. The target is
// covered by a strip of thin slanted triangles; a linear varying p (pixel units) feeds
// f = p.x * p.x / 16 + p.y, and the fragment writes fwidth(f). Within a quad the helper lanes
// extrapolate p exactly, so fwidth must equal the closed form at every pixel, edges included.
// RenderBox's anti-aliasing showed holes along shared triangle edges on the RTX (1 Oct 2026).
#import <Metal/Metal.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
int main(int argc, char **argv)
{
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "struct VO { float4 p [[position]]; float2 q [[center_no_perspective]]; };\n"
             "vertex VO vs(uint v [[vertex_id]]) { uint k = v / 2; float x = float(k) * 4.0 - 8.0 + ((v & 1) ? 12.0 : 0.0);"
             " float y = (v & 1) ? 64.0 : 0.0; VO o; o.q = float2(x, y); o.p = float4(x / 32.0 - 1.0, 1.0 - y / 32.0, 0, 1); return o; }\n"
             "fragment float4 fs(VO i [[stage_in]]) { float f = i.q.x * i.q.x / 16.0 + i.q.y; return float4(fwidth(f), i.q, 1); }\n"
             // the same with the constants loaded from a buffer (RenderBox reads its shape sizes from memory)
             "fragment float4 fsbuf(VO i [[stage_in]], constant float2 &k [[buffer(0)]]) { float f = i.q.x * i.q.x / k.x + i.q.y * k.y; return float4(fwidth(f), i.q, 1); }\n"
                                              options:nil error:&err];
        if (!lib) { printf("compile: %s\n", err.description.UTF8String); return 1; }
        MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
        rd.vertexFunction = [lib newFunctionWithName:@"vs"];
        const int useBuf = argc > 1 && !strcmp(argv[1], "buffer");
        rd.fragmentFunction = [lib newFunctionWithName:useBuf ? @"fsbuf" : @"fs"];
        rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
        id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:rd error:&err];
        const int S = 64;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:S height:S mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> t = [dev newTextureWithDescriptor:td];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
        p.colorAttachments[0].texture = t; p.colorAttachments[0].loadAction = MTLLoadActionClear;
        p.colorAttachments[0].clearColor = MTLClearColorMake(-1, -1, -1, 0); p.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:p];
        [re setRenderPipelineState:ps];
        const float k[2] = { 16.0f, 1.0f };
        id<MTLBuffer> kb = [dev newBufferWithBytes:k length:sizeof k options:MTLResourceStorageModeShared];
        [re setFragmentBuffer:kb offset:0 atIndex:0];
        [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:46];
        [re endEncoding];
        id<MTLBuffer> rb = [dev newBufferWithLength:S * S * 16 options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
        [bl copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(S, S, 1)
                   toBuffer:rb destinationOffset:0 destinationBytesPerRow:S * 16 destinationBytesPerImage:S * S * 16];
        [bl endEncoding];
        [cb commit]; [cb waitUntilCompleted];
        const float *o = rb.contents;
        int bad = 0, covered = 0;
        for (int y = 0; y < S; y++)
            for (int x = 0; x < S; x++) {
                const float *v = o + (y * S + x) * 4;
                if (v[3] == 0) continue;
                covered++;
                // pixel pair of the quad: columns x0 = x & ~1, rows y0 = y & ~1 (centres at .5)
                const float x0 = (x & ~1) + 0.5f, y0 = (y & ~1) + 0.5f, px = x + 0.5f;
                (void)y0;
                const float fx = ((x0 + 1) * (x0 + 1) - x0 * x0) / 16.0f, fy = 1.0f;
                const float want = fabsf(fx) + fabsf(fy);
                // the varying should equal the pixel centre too
                if (fabsf(v[0] - want) > 1e-3f || fabsf(v[1] - px) > 1e-3f) {
                    if (bad < 6) printf("  (%d,%d) fwidth %.4f want %.4f, q (%.3f %.3f)\n", x, y, v[0], want, v[1], v[2]);
                    bad++;
                }
            }
        printf("metal_deriv_edges_test%s on %s: %d of %d pixels wrong: %s\n", useBuf ? " buffer" : "", dev.name.UTF8String, bad, covered, bad || covered < S * S / 2 ? "FAIL" : "PASS");
        return bad != 0;
    }
}
