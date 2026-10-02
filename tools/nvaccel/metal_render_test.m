// Metal render pipelines with real fragment functions (M16, NVMTLDriver 0.3.0),
// each check verified on the CPU.
//   sudo metal_render_test
// 1 per-vertex color varying, interpolated (corners exact, centroid = mean)
// 2 textured quad: uv varying + texture2d.sample, nearest, 8x8 checker
// 3 alpha blending (src alpha / one minus src alpha) over a cleared target
// 4 1920x1080 BGRA target (v1 was capped at 1 MiB) with clear + triangle
// 5 two draws in one pass: the clear runs once, both triangles stay
// 6 setFragmentBytes: constant tint from a fragment buffer
// 7 depth (Depth32Float, Less + write): the nearer triangle wins in both draw orders
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

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
    @"struct VOut { float4 position [[position]]; float4 color; float2 uv; };\n"
    @"vertex VOut vs(device const float4 *pos [[buffer(0)]], device const float4 *col [[buffer(1)]],"
    @"               uint vid [[vertex_id]]) {"
    @"  VOut o; o.position = pos[vid]; o.color = col[vid]; o.uv = pos[vid].xy * 0.5 + 0.5; return o; }\n"
    @"fragment float4 fcol(VOut in [[stage_in]]) { return in.color; }\n"
    @"fragment float4 ftex(VOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {"
    @"  return t.sample(s, float2(in.uv.x, 1.0 - in.uv.y)); }\n"
    @"fragment float4 ftint(VOut in [[stage_in]], constant float4 *tint [[buffer(0)]]) { return tint[0]; }\n";

static id<MTLRenderPipelineState> mkPipe(id<MTLDevice> dev, id<MTLLibrary> lib, NSString *fs,
                                       MTLPixelFormat fmt, BOOL blend) {
    MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
    rd.vertexFunction = [lib newFunctionWithName:@"vs"];
    rd.fragmentFunction = [lib newFunctionWithName:fs];
    rd.colorAttachments[0].pixelFormat = fmt;
    if (blend) {
        rd.colorAttachments[0].blendingEnabled = YES;
        rd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        rd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        rd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
        rd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    }
    NSError *e = nil;
    id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:rd error:&e];
    if (!ps) printf("  pipeline %s: %s\n", fs.UTF8String, e.localizedDescription.UTF8String ?: "nil");
    return ps;
}

static id<MTLTexture> target(id<MTLDevice> dev, MTLPixelFormat f, NSUInteger w, NSUInteger h) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    return [dev newTextureWithDescriptor:td];
}

static id<MTLBuffer> vbuf(id<MTLDevice> dev, const float *v, size_t n) {
    return [dev newBufferWithBytes:v length:n * 16 options:MTLResourceStorageModeShared];
}

// run one pass: clear + draws (each {ps, pos, col, count, tex, samp, tint})
typedef struct { id<MTLRenderPipelineState> ps; id<MTLBuffer> pos, col; NSUInteger n;
                 id<MTLTexture> tex; id<MTLSamplerState> samp; const float *tint; } Draw;
static void pass(id<MTLCommandQueue> q, id<MTLTexture> rt, MTLClearColor cc, Draw *d, int nd) {
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = rt;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = cc;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
    for (int i = 0; i < nd; i++) {
        [re setRenderPipelineState:d[i].ps];
        [re setVertexBuffer:d[i].pos offset:0 atIndex:0];
        [re setVertexBuffer:d[i].col offset:0 atIndex:1];
        if (d[i].tex) [re setFragmentTexture:d[i].tex atIndex:0];
        if (d[i].samp) [re setFragmentSamplerState:d[i].samp atIndex:0];
        if (d[i].tint) [re setFragmentBytes:d[i].tint length:16 atIndex:0];
        [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:d[i].n];
    }
    [re endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
}

static void px(id<MTLTexture> t, NSUInteger x, NSUInteger y, uint8_t out[4]) {
    [t getBytes:out bytesPerRow:t.width * 4 fromRegion:MTLRegionMake2D(x, y, 1, 1) mipmapLevel:0];
}
static int near(const uint8_t *p, int r, int g, int b, int a, int tol) {
    return abs(p[0] - r) <= tol && abs(p[1] - g) <= tol && abs(p[2] - b) <= tol && abs(p[3] - a) <= tol;
}

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = findNV();
        if (!dev) { printf("no NVIDIA MTLDevice\n"); return 1; }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:nil error:&e];
        if (!lib) { printf("library: %s\n", e.localizedDescription.UTF8String); return 1; }
        const MTLPixelFormat RGBA = MTLPixelFormatRGBA8Unorm;
        const MTLClearColor black = {0, 0, 0, 1};
        // full-target triangle pair helpers
        const float quadPos[] = {-1, -1, 0, 1,  1, -1, 0, 1,  -1, 1, 0, 1,
                                 -1,  1, 0, 1,  1, -1, 0, 1,   1, 1, 0, 1};
        const float white[24] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};

        // 1: interpolated color
        {
            id<MTLRenderPipelineState> ps = mkPipe(dev, lib, @"fcol", RGBA, NO);
            check("pipeline with a varying fragment", ps != nil);
            const float pos[] = {-1, -1, 0, 1,  1, -1, 0, 1,  -1, 1, 0, 1};
            const float col[] = {1, 0, 0, 1,  0, 1, 0, 1,  0, 0, 1, 1};
            id<MTLTexture> rt = target(dev, RGBA, 64, 64);
            if (ps && rt) {
                Draw d = {ps, vbuf(dev, pos, 3), vbuf(dev, col, 3), 3, nil, nil, NULL};
                pass(q, rt, black, &d, 1);
                uint8_t a[4], b[4], c[4], m[4], o[4];
                // (0,0) sits on the x == y edge, a right edge: not covered under the top-left rule
                px(rt, 0, 63, a); px(rt, 62, 63, b); px(rt, 0, 1, c); px(rt, 21, 42, m); px(rt, 60, 3, o);
                char msg[120];
                snprintf(msg, sizeof msg, "corners r/g/b, centroid %d,%d,%d, outside %d,%d,%d",
                         m[0], m[1], m[2], o[0], o[1], o[2]);
                check(msg, near(a, 255, 0, 0, 255, 12) && near(b, 0, 255, 0, 255, 12) &&
                           near(c, 0, 0, 255, 255, 12) && near(m, 85, 85, 85, 255, 12) &&
                           near(o, 0, 0, 0, 255, 0));
            }
        }
        // 2: textured quad
        {
            id<MTLRenderPipelineState> ps = mkPipe(dev, lib, @"ftex", RGBA, NO);
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:RGBA width:8 height:8 mipmapped:NO];
            td.usage = MTLTextureUsageShaderRead;
            id<MTLTexture> tx = [dev newTextureWithDescriptor:td];
            uint32_t chk[64];
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) chk[y * 8 + x] = ((x ^ y) & 1) ? 0xff00ff00u : 0xff0000ffu;
            [tx replaceRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0 withBytes:chk bytesPerRow:32];
            MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
            sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
            id<MTLSamplerState> ss = [dev newSamplerStateWithDescriptor:sd];
            id<MTLTexture> rt = target(dev, RGBA, 64, 64);
            int bad = -1;
            if (ps && rt && tx) {
                Draw d = {ps, vbuf(dev, quadPos, 6), vbuf(dev, white, 6), 6, tx, ss, NULL};
                pass(q, rt, black, &d, 1);
                uint32_t *img = malloc(64 * 64 * 4);
                [rt getBytes:img bytesPerRow:256 fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
                bad = 0;
                for (int y = 0; y < 64; y++)
                    for (int x = 0; x < 64; x++)
                        if (img[y * 64 + x] != chk[(y / 8) * 8 + x / 8]) bad++;
                free(img);
            }
            char msg[80];
            snprintf(msg, sizeof msg, "textured quad, 64x64 nearest checker (%d bad)", bad);
            check(msg, bad == 0);
        }
        // 3: blending
        {
            id<MTLRenderPipelineState> ps = mkPipe(dev, lib, @"fcol", RGBA, YES);
            const float half[24] = {1, 0, 0, 0.5, 1, 0, 0, 0.5, 1, 0, 0, 0.5, 1, 0, 0, 0.5, 1, 0, 0, 0.5, 1, 0, 0, 0.5};
            id<MTLTexture> rt = target(dev, RGBA, 32, 32);
            uint8_t p[4] = {0};
            if (ps && rt) {
                Draw d = {ps, vbuf(dev, quadPos, 6), vbuf(dev, half, 6), 6, nil, nil, NULL};
                pass(q, rt, (MTLClearColor){0, 0, 1, 1}, &d, 1);
                px(rt, 16, 16, p);
            }
            char msg[80];
            snprintf(msg, sizeof msg, "50%% red over blue -> %d,%d,%d,%d", p[0], p[1], p[2], p[3]);
            check(msg, near(p, 128, 0, 128, 255, 3));
        }
        // 4: big BGRA target
        {
            id<MTLRenderPipelineState> ps = mkPipe(dev, lib, @"fcol", MTLPixelFormatBGRA8Unorm, NO);
            id<MTLTexture> rt = target(dev, MTLPixelFormatBGRA8Unorm, 1920, 1080);
            const float pos[] = {-1, -1, 0, 1,  0, 1, 0, 1,  1, -1, 0, 1};
            const float col[] = {1, 0, 0, 1,  1, 0, 0, 1,  1, 0, 0, 1};
            uint8_t in[4] = {0}, out[4] = {0};
            if (ps && rt) {
                Draw d = {ps, vbuf(dev, pos, 3), vbuf(dev, col, 3), 3, nil, nil, NULL};
                pass(q, rt, (MTLClearColor){0, 1, 0, 1}, &d, 1);
                px(rt, 960, 800, in); px(rt, 10, 10, out);
            }
            // BGRA bytes: red = 00 00 ff ff, green clear = 00 ff 00 ff
            check("1920x1080 BGRA: red triangle, green clear outside",
                  near(in, 0, 0, 255, 255, 0) && near(out, 0, 255, 0, 255, 0));
        }
        // 5: two draws, one clear
        {
            id<MTLRenderPipelineState> ps = mkPipe(dev, lib, @"fcol", RGBA, NO);
            const float left[] = {-1, -1, 0, 1,  -0.1, -1, 0, 1,  -1, 1, 0, 1};
            const float right[] = {0.1, -1, 0, 1,  1, -1, 0, 1,  1, 1, 0, 1};
            const float red[] = {1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1};
            const float grn[] = {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1};
            id<MTLTexture> rt = target(dev, RGBA, 64, 64);
            uint8_t a[4] = {0}, b[4] = {0};
            if (ps && rt) {
                Draw d[2] = {{ps, vbuf(dev, left, 3), vbuf(dev, red, 3), 3, nil, nil, NULL},
                             {ps, vbuf(dev, right, 3), vbuf(dev, grn, 3), 3, nil, nil, NULL}};
                pass(q, rt, black, d, 2);
                px(rt, 2, 60, a); px(rt, 61, 60, b);
            }
            check("two draws in one pass both survive", near(a, 255, 0, 0, 255, 0) && near(b, 0, 255, 0, 255, 0));
        }
        // 6: fragment buffer
        {
            id<MTLRenderPipelineState> ps = mkPipe(dev, lib, @"ftint", RGBA, NO);
            const float tint[4] = {0.25, 0.5, 0.75, 1};
            id<MTLTexture> rt = target(dev, RGBA, 16, 16);
            uint8_t p[4] = {0};
            if (ps && rt) {
                Draw d = {ps, vbuf(dev, quadPos, 6), vbuf(dev, white, 6), 6, nil, nil, tint};
                pass(q, rt, black, &d, 1);
                px(rt, 8, 8, p);
            }
            check("setFragmentBytes tint 0.25/0.5/0.75", near(p, 64, 128, 191, 255, 2));
        }
    }
    @autoreleasepool {
        // 7: depth test
        id<MTLDevice> dev = findNV();
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:nil error:nil];
        id<MTLRenderPipelineState> ps = nil;
        {
            MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
            rd.vertexFunction = [lib newFunctionWithName:@"vs"];
            rd.fragmentFunction = [lib newFunctionWithName:@"fcol"];
            rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            rd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
            ps = [dev newRenderPipelineStateWithDescriptor:rd error:nil];
        }
        MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
        dd.depthCompareFunction = MTLCompareFunctionLess;
        dd.depthWriteEnabled = YES;
        id<MTLDepthStencilState> ds = [dev newDepthStencilStateWithDescriptor:dd];
        MTLTextureDescriptor *zd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                     width:32 height:32 mipmapped:NO];
        zd.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> zt = [dev newTextureWithDescriptor:zd];
        const float nearT[] = {-1, -1, 0.2, 1,  1, -1, 0.2, 1,  0, 1, 0.2, 1};
        const float farT[] = {-1, -1, 0.8, 1,  1, -1, 0.8, 1,  0, 1, 0.8, 1};
        const float red[] = {1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1};
        const float grn[] = {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1};
        int good = ps && ds && zt;
        for (int order = 0; order < 2 && good; order++) {
            id<MTLTexture> rt = target(dev, MTLPixelFormatRGBA8Unorm, 32, 32);
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = rt;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].clearColor = (MTLClearColor){0, 0, 0, 1};
            rp.depthAttachment.texture = zt;
            rp.depthAttachment.loadAction = MTLLoadActionClear;
            rp.depthAttachment.clearDepth = 1.0;
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
            [re setRenderPipelineState:ps];
            [re setDepthStencilState:ds];
            for (int i = 0; i < 2; i++) {
                const int isNear = (i == 0) == (order == 0);
                [re setVertexBuffer:vbuf(dev, isNear ? nearT : farT, 3) offset:0 atIndex:0];
                [re setVertexBuffer:vbuf(dev, isNear ? red : grn, 3) offset:0 atIndex:1];
                [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            }
            [re endEncoding];
            [cb commit]; [cb waitUntilCompleted];
            uint8_t p[4];
            px(rt, 16, 20, p);
            good = near(p, 255, 0, 0, 255, 0);
        }
        check("depth Less+write: near red wins, both draw orders", good);
    }
    printf("metal_render_test: %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
