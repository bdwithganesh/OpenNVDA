// Private (VRAM) textures on NVMTLDriver 0.3.5 (M30), CPU-verified, plus timing.
//   sudo metal_priv_test
// 1 buffer -> private texture blit, textured quad into a private target,
//   private target -> buffer blit, compare with the source pattern
// 2 texture -> texture blit (private -> shared) with an origin offset
// 3 1920x1080 textured full-screen draws: private vs shared target+texture
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>

static id<MTLDevice> findNV(void) {
    for (id<MTLDevice> d in MTLCopyAllDevices())
        if ([d.name containsString:@"NVIDIA"]) return d;
    return nil;
}
static int fails;
static void check(const char *what, int ok) {
    printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) fails++;
}
static NSString *kSrc =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
    @"struct VOut { float4 position [[position]]; float2 uv; };\n"
    @"vertex VOut vs(device const float4 *pos [[buffer(0)]], uint vid [[vertex_id]]) {"
    @"  VOut o; o.position = pos[vid]; o.uv = pos[vid].xy * 0.5 + 0.5; return o; }\n"
    @"fragment float4 fs(VOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {"
    @"  return t.sample(s, float2(in.uv.x, 1.0 - in.uv.y)); }\n";

static id<MTLTexture> tex(id<MTLDevice> dev, NSUInteger w, NSUInteger h, MTLStorageMode sm, MTLTextureUsage u) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                 width:w height:h mipmapped:NO];
    td.storageMode = sm;
    td.usage = u;
    return [dev newTextureWithDescriptor:td];
}

static double drawQuads(id<MTLCommandQueue> q, id<MTLRenderPipelineState> ps, id<MTLBuffer> vb,
                        id<MTLTexture> src, id<MTLSamplerState> ss, id<MTLTexture> rt, int n) {
    const CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent();
    for (int i = 0; i < n; i++) {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = rt;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = (MTLClearColor){0, 0, 0, 1};
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
        [re setRenderPipelineState:ps];
        [re setVertexBuffer:vb offset:0 atIndex:0];
        [re setFragmentTexture:src atIndex:0];
        [re setFragmentSamplerState:ss atIndex:0];
        [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
        [re endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
    }
    return (CFAbsoluteTimeGetCurrent() - t0) / n * 1e3;
}

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = findNV();
        if (!dev) { printf("no NVIDIA MTLDevice\n"); return 1; }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:nil error:&e];
        MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
        rd.vertexFunction = [lib newFunctionWithName:@"vs"];
        rd.fragmentFunction = [lib newFunctionWithName:@"fs"];
        rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:rd error:&e];
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
        id<MTLSamplerState> ss = [dev newSamplerStateWithDescriptor:sd];
        const float quad[] = {-1, -1, 0, 1,  1, -1, 0, 1,  -1, 1, 0, 1,  -1, 1, 0, 1,  1, -1, 0, 1,  1, 1, 0, 1};
        id<MTLBuffer> vb = [dev newBufferWithBytes:quad length:sizeof quad options:MTLResourceStorageModeShared];
        if (!ps) { printf("pipeline failed\n"); return 1; }

        // 1
        const NSUInteger W = 64, H = 64;
        uint32_t chk[64];
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) chk[y * 8 + x] = ((x ^ y) & 1) ? 0xff00ff00u : 0xff0000ffu | (uint32_t)(x * 16) << 16;
        id<MTLBuffer> up = [dev newBufferWithBytes:chk length:sizeof chk options:MTLResourceStorageModeShared];
        id<MTLTexture> ptex = tex(dev, 8, 8, MTLStorageModePrivate, MTLTextureUsageShaderRead);
        id<MTLTexture> prt = tex(dev, W, H, MTLStorageModePrivate, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
        id<MTLBuffer> down = [dev newBufferWithLength:W * H * 4 options:MTLResourceStorageModeShared];
        check("private textures report MTLStorageModePrivate",
              ptex.storageMode == MTLStorageModePrivate && prt.storageMode == MTLStorageModePrivate);
        {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
            [be copyFromBuffer:up sourceOffset:0 sourceBytesPerRow:32 sourceBytesPerImage:256
                    sourceSize:MTLSizeMake(8, 8, 1) toTexture:ptex destinationSlice:0 destinationLevel:0
             destinationOrigin:MTLOriginMake(0, 0, 0)];
            [be endEncoding];
            [cb commit]; [cb waitUntilCompleted];
        }
        drawQuads(q, ps, vb, ptex, ss, prt, 1);
        {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
            [be copyFromTexture:prt sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                     sourceSize:MTLSizeMake(W, H, 1) toBuffer:down destinationOffset:0
         destinationBytesPerRow:W * 4 destinationBytesPerImage:W * H * 4];
            [be endEncoding];
            [cb commit]; [cb waitUntilCompleted];
        }
        const uint32_t *img = down.contents;
        int bad = 0;
        for (NSUInteger y = 0; y < H; y++)
            for (NSUInteger x = 0; x < W; x++)
                if (img[y * W + x] != chk[(y / 8) * 8 + x / 8]) bad++;
        char msg[96];
        snprintf(msg, sizeof msg, "upload blit -> sample -> private RT -> readback blit (%d bad)", bad);
        check(msg, bad == 0);

        // 2
        id<MTLTexture> shared = tex(dev, W, H, MTLStorageModeShared, MTLTextureUsageShaderRead);
        {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
            [be copyFromTexture:prt sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(8, 16, 0)
                     sourceSize:MTLSizeMake(24, 8, 1) toTexture:shared destinationSlice:0 destinationLevel:0
              destinationOrigin:MTLOriginMake(1, 2, 0)];
            [be endEncoding];
            [cb commit]; [cb waitUntilCompleted];
        }
        uint32_t row[24];
        [shared getBytes:row bytesPerRow:96 fromRegion:MTLRegionMake2D(1, 2, 24, 1) mipmapLevel:0];
        int ok2 = 1;
        for (int x = 0; x < 24; x++) ok2 &= row[x] == img[16 * W + 8 + x];
        check("private -> shared texture blit with offsets", ok2);

        // 3
        const NSUInteger BW = 1920, BH = 1080;
        id<MTLTexture> srcP = tex(dev, BW, BH, MTLStorageModePrivate, MTLTextureUsageShaderRead);
        id<MTLTexture> rtP = tex(dev, BW, BH, MTLStorageModePrivate, MTLTextureUsageRenderTarget);
        id<MTLTexture> srcS = tex(dev, BW, BH, MTLStorageModeShared, MTLTextureUsageShaderRead);
        id<MTLTexture> rtS = tex(dev, BW, BH, MTLStorageModeShared, MTLTextureUsageRenderTarget);
        drawQuads(q, ps, vb, srcP, ss, rtP, 2);
        drawQuads(q, ps, vb, srcS, ss, rtS, 2);
        const double tp = drawQuads(q, ps, vb, srcP, ss, rtP, 20);
        const double ts = drawQuads(q, ps, vb, srcS, ss, rtS, 20);
        printf("  1080p textured full-screen pass: private %.2f ms, shared %.2f ms (%.1fx)\n", tp, ts, ts / tp);
        check("private (VRAM) pass is faster than shared (sysmem)", tp < ts);
    }
    printf("metal_priv_test: %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
