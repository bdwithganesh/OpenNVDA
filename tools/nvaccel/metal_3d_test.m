// Render pipelines from a precompiled metallib on the 3D engine (NVMTLDriver 0.5.0).
//   sudo metal_3d_test gfx.metallib
// 1 v_pull + f_flat: vertices pulled from a buffer by vertex_id, a flat colour
//   from a fragment buffer; clear colour outside the triangle
// 2 v_main + f_main: stage_in attributes through a vertex descriptor, colour
//   and uv varyings, a texture sampled in the fragment stage
// 3 depth test; 4 render into a mip level / array slice / 3D depth plane;
// 5 blending. 4 and 5 check every pixel of a two-triangle quad: warps with
//   helper pixels once lost the colour (M3D_MAP=1 prints the coverage)
#import <Metal/Metal.h>
#include <math.h>
#include <stdio.h>

static id<MTLDevice> findNV(void) {
    for (id<MTLDevice> d in MTLCopyAllDevices())
        if ([d.name containsString:@"NVIDIA"]) return d;
    return nil;
}
static int fails;
static void check(const char *what, int ok) {
    printf("  %-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) fails++;
}

enum { W = 64, H = 64 };

static id<MTLCommandQueue> gq;
static int gPriv;   // METAL3D_PRIVATE=1: target in VRAM, read back with a blit

static id<MTLTexture> target(id<MTLDevice> dev) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                 width:W height:H mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    if (gPriv) td.storageMode = MTLStorageModePrivate;
    return [dev newTextureWithDescriptor:td];
}

static void readback(id<MTLDevice> dev, id<MTLTexture> rt, uint32_t *px) {
    if (!gPriv) {
        [rt getBytes:px bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
        return;
    }
    id<MTLBuffer> b = [dev newBufferWithLength:W * H * 4 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [gq commandBuffer];
    id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    [be copyFromTexture:rt sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
             sourceSize:MTLSizeMake(W, H, 1) toBuffer:b destinationOffset:0 destinationBytesPerRow:W * 4
destinationBytesPerImage:W * H * 4];
    [be endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    memcpy(px, b.contents, W * H * 4);
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 2) { fprintf(stderr, "usage: %s gfx.metallib\n", argv[0]); return 2; }
        id<MTLDevice> dev = findNV();
        if (!dev) { printf("no NVIDIA MTLDevice\n"); return 1; }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        gq = q;
        gPriv = getenv("METAL3D_PRIVATE") != NULL;
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&e];
        if (!lib) { printf("library: %s\n", e.localizedDescription.UTF8String); return 1; }

        {   // 1: pulled vertices, flat colour
            MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
            rd.vertexFunction = [lib newFunctionWithName:@"v_pull"];
            rd.fragmentFunction = [lib newFunctionWithName:@"f_flat"];
            rd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
            id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:rd error:&e];
            id<MTLTexture> rt = target(dev);
            // left half of the target: (-1,-1) (0,-1) (-1,1) ... two triangles
            const float pos[12] = {-1, -1, 0, -1, -1, 1,  -1, 1, 0, -1, 0, 1};
            const uint16_t col[4] = {0x3800, 0x3c00, 0x0000, 0x3c00};   // half4 (0.5, 1, 0, 1)
            int ok = ps != nil;
            if (ok) {
                MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                rp.colorAttachments[0].texture = rt;
                rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                rp.colorAttachments[0].clearColor = (MTLClearColor){0, 0, 1, 1};
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
                [re setRenderPipelineState:ps];
                [re setVertexBytes:pos length:sizeof pos atIndex:0];
                [re setFragmentBytes:col length:sizeof col atIndex:0];
                [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
                [re endEncoding];
                [cb commit];
                [cb waitUntilCompleted];
                uint32_t px[W * H];
                readback(dev, rt, px);
                const uint32_t in = px[32 * W + 10], out = px[32 * W + 50];
                printf("    inside 0x%08x (want ~0xff00ff80), outside 0x%08x (want 0xff0000ff)\n", in, out);
                const uint32_t r = (in >> 16) & 255, g = (in >> 8) & 255, b = in & 255;
                ok = r >= 126 && r <= 129 && g == 255 && b == 0 && out == 0xff0000ff;
            }
            check("v_pull + f_flat (buffer vertices, clear, flat colour)", ok);
        }
        {   // 2: vertex descriptor + varyings + texture
            MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
            vd.attributes[0].format = MTLVertexFormatFloat2; vd.attributes[0].offset = 0; vd.attributes[0].bufferIndex = 0;
            vd.attributes[1].format = MTLVertexFormatFloat4; vd.attributes[1].offset = 8; vd.attributes[1].bufferIndex = 0;
            vd.layouts[0].stride = 24;
            MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
            rd.vertexFunction = [lib newFunctionWithName:@"v_main"];
            rd.fragmentFunction = [lib newFunctionWithName:@"f_main"];
            rd.vertexDescriptor = vd;
            rd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
            id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:rd error:&e];
            id<MTLTexture> rt = target(dev);
            // 1x1 white texture: the output is just the interpolated colour
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                         width:1 height:1 mipmapped:NO];
            id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
            const uint32_t white = 0xffffffff;
            [tex replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&white bytesPerRow:4];
            id<MTLSamplerState> smp = [dev newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
            // full-screen triangle, red at the bottom-left corner, green top-left, blue right
            const float vtx[18] = {-1, -1, 1, 0, 0, 1,   -1, 3, 0, 1, 0, 1,   3, -1, 0, 0, 1, 1};
            const float offset[2] = {0, 0};
            int ok = ps != nil;
            if (ok) {
                MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                rp.colorAttachments[0].texture = rt;
                rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                rp.colorAttachments[0].clearColor = (MTLClearColor){0, 0, 0, 0};
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
                [re setRenderPipelineState:ps];
                [re setVertexBytes:vtx length:sizeof vtx atIndex:0];
                [re setVertexBytes:offset length:sizeof offset atIndex:1];
                [re setFragmentTexture:tex atIndex:0];
                [re setFragmentSamplerState:smp atIndex:0];
                [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [re endEncoding];
                [cb commit];
                [cb waitUntilCompleted];
                uint32_t px[W * H];
                readback(dev, rt, px);
                // pixel (x, y), y down: NDC x = (x+0.5)/32-1, y = 1-(y+0.5)/32
                // barycentrics of (-1,-1) R, (-1,3) G, (3,-1) B: g = (ny+1)/4, b = (nx+1)/4, r = 1-g-b
                int bad = 0;
                for (int y = 0; y < H; y += 7)
                    for (int x = 0; x < W; x += 7) {
                        const float nx = (x + 0.5f) / 32 - 1, ny = 1 - (y + 0.5f) / 32;
                        const float g = (ny + 1) / 4, b = (nx + 1) / 4, r = 1 - g - b;
                        const uint32_t p = px[y * W + x];
                        const int pr = (p >> 16) & 255, pg = (p >> 8) & 255, pbl = p & 255;
                        if (fabsf(pr - r * 255) > 3 || fabsf(pg - g * 255) > 3 || fabsf(pbl - b * 255) > 3) {
                            if (bad < 4) printf("    (%d,%d) got %08x want r%.0f g%.0f b%.0f\n", x, y, p, r * 255, g * 255, b * 255);
                            bad++;
                        }
                    }
                ok = !bad;
            }
            check("v_main + f_main (vertex descriptor, varyings, texture)", ok);
        }
        {   // 3: depth test (Depth32Float, Less, write): a far red quad drawn after a near green one stays hidden
            MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
            rd.vertexFunction = [lib newFunctionWithName:@"v_depth"];
            rd.fragmentFunction = [lib newFunctionWithName:@"f_flat"];
            rd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
            rd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
            id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:rd error:&e];
            MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
            dd.depthCompareFunction = MTLCompareFunctionLess;
            dd.depthWriteEnabled = YES;
            id<MTLDepthStencilState> ds = [dev newDepthStencilStateWithDescriptor:dd];
            id<MTLTexture> rt = target(dev);
            MTLTextureDescriptor *zd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                         width:W height:H mipmapped:NO];
            zd.usage = MTLTextureUsageRenderTarget;
            zd.storageMode = MTLStorageModePrivate;
            id<MTLTexture> zt = [dev newTextureWithDescriptor:zd];
            // full-screen triangles: near (z 0.2) left half only, far (z 0.8) everything
            const float nearTri[12] = {-1, -1, 0.2f, 1,  0, -1, 0.2f, 1,  -1, 3, 0.2f, 1};
            const float farTri[12] = {-1, -1, 0.8f, 1,  3, -1, 0.8f, 1,  -1, 3, 0.8f, 1};
            const uint16_t green[4] = {0x0000, 0x3c00, 0x0000, 0x3c00}, red[4] = {0x3c00, 0x0000, 0x0000, 0x3c00};
            int ok = ps && ds && zt;
            if (ok) {
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
                [re setVertexBytes:nearTri length:sizeof nearTri atIndex:0];
                [re setFragmentBytes:green length:sizeof green atIndex:0];
                [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [re setVertexBytes:farTri length:sizeof farTri atIndex:0];
                [re setFragmentBytes:red length:sizeof red atIndex:0];
                [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [re endEncoding];
                [cb commit];
                [cb waitUntilCompleted];
                uint32_t px[W * H];
                readback(dev, rt, px);
                const uint32_t l = px[40 * W + 4], rgt = px[40 * W + 60];
                printf("    left 0x%08x (want green 0xff00ff00), right 0x%08x (want red 0xffff0000)\n", l, rgt);
                ok = l == 0xff00ff00 && rgt == 0xffff0000;
            }
            check("depth test (Depth32Float, Less, write, clear)", ok);
        }
        {   // 4: render into a mip level + array slice, and into a 3D depth plane
            MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
            rd.vertexFunction = [lib newFunctionWithName:@"v_pull"];
            rd.fragmentFunction = [lib newFunctionWithName:@"f_flat"];
            rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:rd error:&e];
            const float full[12] = {-1, -1, 1, -1, -1, 1,  -1, 1, 1, -1, 1, 1};
            const uint16_t red[4] = {0x3c00, 0, 0, 0x3c00};
            MTLTextureDescriptor *ad = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                         width:64 height:64 mipmapped:YES];
            ad.textureType = MTLTextureType2DArray; ad.arrayLength = 3; ad.mipmapLevelCount = 3;
            ad.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            id<MTLTexture> arr = [dev newTextureWithDescriptor:ad];
            MTLTextureDescriptor *vd3 = [MTLTextureDescriptor new];
            vd3.textureType = MTLTextureType3D; vd3.pixelFormat = MTLPixelFormatRGBA8Unorm;
            vd3.width = 16; vd3.height = 16; vd3.depth = 8;
            vd3.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            id<MTLTexture> vol = [dev newTextureWithDescriptor:vd3];
            int ok = ps && arr && vol;
            if (ok) {
                // zero everything first so untouched parts read back as 0
                static uint32_t zero[64 * 64];
                for (NSUInteger sl = 0; sl < 3; sl++)
                    for (NSUInteger l = 0; l < 3; l++)
                        [arr replaceRegion:MTLRegionMake2D(0, 0, 64 >> l, 64 >> l) mipmapLevel:l slice:sl
                                 withBytes:zero bytesPerRow:(64 >> l) * 4 bytesPerImage:0];
                for (NSUInteger z = 0; z < 8; z++)
                    [vol replaceRegion:MTLRegionMake3D(0, 0, z, 16, 16, 1) mipmapLevel:0 slice:0
                             withBytes:zero bytesPerRow:64 bytesPerImage:0];
                id<MTLCommandBuffer> cb = [q commandBuffer];
                MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                rp.colorAttachments[0].texture = arr;
                rp.colorAttachments[0].level = 1; rp.colorAttachments[0].slice = 2;
                rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
                id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
                [re setRenderPipelineState:ps];
                [re setVertexBytes:full length:sizeof full atIndex:0];
                [re setFragmentBytes:red length:sizeof red atIndex:0];
                [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
                [re endEncoding];
                rp = [MTLRenderPassDescriptor renderPassDescriptor];
                rp.colorAttachments[0].texture = vol;
                rp.colorAttachments[0].depthPlane = 5;
                rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
                re = [cb renderCommandEncoderWithDescriptor:rp];
                [re setRenderPipelineState:ps];
                [re setVertexBytes:full length:sizeof full atIndex:0];
                [re setFragmentBytes:red length:sizeof red atIndex:0];
                [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
                [re endEncoding];
                [cb commit];
                [cb waitUntilCompleted];
                static uint32_t px[64 * 64];
                int bad = 0;
                for (NSUInteger sl = 0; sl < 3; sl++)
                    for (NSUInteger l = 0; l < 3; l++) {
                        const NSUInteger n = 64 >> l;
                        memset(px, 0xee, sizeof px);
                        [arr getBytes:px bytesPerRow:n * 4 bytesPerImage:0 fromRegion:MTLRegionMake2D(0, 0, n, n)
                          mipmapLevel:l slice:sl];
                        const uint32_t want = (l == 1 && sl == 2) ? 0xff0000ff : 0;
                        int wrong = 0;
                        for (NSUInteger k = 0; k < n * n; k++)
                            if (px[k] != want && wrong++ < 12) printf("      (%lu,%lu) %08x\n", (unsigned long)(k % n),
                                                                       (unsigned long)(k / n), px[k]);
                        if (wrong && getenv("M3D_MAP"))
                            for (NSUInteger y = 0; y < n; y++) {
                                for (NSUInteger x = 0; x < n; x++) putchar(px[y * n + x] == want ? '#' : px[y * n + x] ? '?' : '.');
                                putchar('\n');
                            }
                        if (wrong) { printf("    array level %lu slice %lu: %d wrong, px0 %08x want %08x\n",
                                            (unsigned long)l, (unsigned long)sl, wrong, px[0], want); bad++; }
                    }
                for (NSUInteger z = 0; z < 8; z++) {
                    memset(px, 0xee, 16 * 16 * 4);
                    [vol getBytes:px bytesPerRow:64 bytesPerImage:0 fromRegion:MTLRegionMake3D(0, 0, z, 16, 16, 1)
                      mipmapLevel:0 slice:0];
                    const uint32_t want = z == 5 ? 0xff0000ff : 0;
                    int wrong = 0;
                    for (NSUInteger k = 0; k < 256; k++) wrong += px[k] != want;
                    if (wrong) { printf("    3D plane %lu: %d wrong, px0 %08x want %08x\n", (unsigned long)z, wrong,
                                        px[0], want); bad++; }
                }
                ok = !bad;
            }
            check("render into mip level/array slice and a 3D depth plane", ok);
        }
        {   // 5: blending (src alpha, one minus src alpha) over a blue clear
            MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
            rd.vertexFunction = [lib newFunctionWithName:@"v_pull"];
            rd.fragmentFunction = [lib newFunctionWithName:@"f_flat"];
            rd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
            rd.colorAttachments[0].blendingEnabled = YES;
            rd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
            rd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            rd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
            rd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorZero;
            id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:rd error:&e];
            id<MTLTexture> rt = target(dev);
            const float full[12] = {-1, -1, 1, -1, -1, 1,  -1, 1, 1, -1, 1, 1};
            const uint16_t col[4] = {0x3c00, 0, 0, 0x3800};   // red, alpha 0.5
            int ok = ps != nil;
            if (ok) {
                MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                rp.colorAttachments[0].texture = rt;
                rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                rp.colorAttachments[0].clearColor = (MTLClearColor){0, 0, 1, 1};
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
                [re setRenderPipelineState:ps];
                [re setVertexBytes:full length:sizeof full atIndex:0];
                [re setFragmentBytes:col length:sizeof col atIndex:0];
                [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
                [re endEncoding];
                [cb commit];
                [cb waitUntilCompleted];
                uint32_t px[W * H];
                readback(dev, rt, px);
                int holes = 0;
                for (int k = 0; k < W * H; k++) holes += px[k] != px[20 * W + 20];
                if (holes) {
                    printf("    %d pixels differ from (20,20) 0x%08x:", holes, px[20 * W + 20]);
                    for (int k = 0, sh = 0; k < W * H && sh < 4; k++)
                        if (px[k] != px[20 * W + 20]) { printf(" (%d,%d)=%08x", k % W, k / W, px[k]); sh++; }
                    printf("\n");
                }
                if (holes && getenv("M3D_MAP"))
                    for (int y = 0; y < 20; y++) {
                        for (int x = 0; x < W; x++) putchar(px[y * W + x] == px[20 * W + 20] ? '#' : '.');
                        putchar('\n');
                    }
                const uint32_t p = holes ? 0 : px[20 * W + 20];
                const int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255, a = p >> 24;
                printf("    blended 0x%08x (want ~0x80800080)\n", p);
                ok = r >= 126 && r <= 129 && g == 0 && b >= 126 && b <= 129 && a >= 126 && a <= 129;
            }
            check("blending (src alpha / one minus src alpha)", ok);
        }
        {   // 6: generateMipmaps + blits on block-linear textures
            MTLTextureDescriptor *md = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                         width:64 height:64 mipmapped:YES];
            id<MTLTexture> a = [dev newTextureWithDescriptor:md];
            static uint32_t img[64 * 64], got[64 * 64];
            for (int y = 0; y < 64; y++)
                for (int x = 0; x < 64; x++) img[y * 64 + x] = 0xff000000u | (uint32_t)(y * 4) << 8 | (uint32_t)(x * 4);
            [a replaceRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0 withBytes:img bytesPerRow:256];
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
            [be generateMipmapsForTexture:a];
            [be endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            int bad = 0;
            for (NSUInteger l = 1; l < a.mipmapLevelCount; l++) {
                const NSUInteger n = 64 >> l;
                [a getBytes:got bytesPerRow:n * 4 fromRegion:MTLRegionMake2D(0, 0, n, n) mipmapLevel:l];
                for (NSUInteger y = 0; y < n; y++)
                    for (NSUInteger x = 0; x < n; x++) {
                        const uint32_t k = 1u << (l + 2), c = (1u << (l + 1)) - 2;
                        const uint32_t want = 0xff000000u | (uint32_t)(y * k + c) << 8 | (uint32_t)(x * k + c);
                        if (got[y * n + x] != want && bad++ < 3)
                            printf("    mip %lu (%lu,%lu) %08x want %08x\n", (unsigned long)l, (unsigned long)x,
                                   (unsigned long)y, got[y * n + x], want);
                    }
            }
            check("generateMipmaps (RGBA8, 7 levels, box filter)", !bad);
            {   // sRGB: filtering happens on linear values (checker 0/255 -> 188, not 128)
                MTLTextureDescriptor *sd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB
                                                                                             width:8 height:8 mipmapped:YES];
                id<MTLTexture> st = [dev newTextureWithDescriptor:sd];
                uint32_t chk[64], l1[16];
                for (int i = 0; i < 64; i++) chk[i] = ((i % 8 + i / 8) & 1) ? 0xffffffffu : 0xff000000u;
                [st replaceRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0 withBytes:chk bytesPerRow:32];
                id<MTLCommandBuffer> scb = [q commandBuffer];
                id<MTLBlitCommandEncoder> sbe = [scb blitCommandEncoder];
                [sbe generateMipmapsForTexture:st];
                [sbe endEncoding]; [scb commit]; [scb waitUntilCompleted];
                [st getBytes:l1 bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:1];
                int sbad = 0;
                for (int i = 0; i < 16; i++) {
                    const int r = l1[i] & 255, a = l1[i] >> 24;
                    if ((r < 187 || r > 189 || a != 255) && sbad++ < 2) printf("    sRGB mip texel %d %08x (want ~bcbcbc)\n", i, l1[i]);
                }
                check("generateMipmaps sRGB (filtered in linear space)", !sbad);
            }

            // texture (level 2) -> buffer, buffer -> array slice 1 level 1, texture -> texture
            MTLTextureDescriptor *ad = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                         width:32 height:32 mipmapped:YES];
            ad.textureType = MTLTextureType2DArray; ad.arrayLength = 3;
            id<MTLTexture> arr = [dev newTextureWithDescriptor:ad];
            id<MTLBuffer> buf = [dev newBufferWithLength:16 * 16 * 4 options:MTLResourceStorageModeShared];
            id<MTLBuffer> pbuf = [dev newBufferWithLength:16 * 16 * 4 options:MTLResourceStorageModePrivate];
            cb = [q commandBuffer];
            be = [cb blitCommandEncoder];
            [be copyFromTexture:a sourceSlice:0 sourceLevel:2 sourceOrigin:MTLOriginMake(0, 0, 0)
                     sourceSize:MTLSizeMake(16, 16, 1) toBuffer:pbuf destinationOffset:0
         destinationBytesPerRow:64 destinationBytesPerImage:64 * 16];
            [be copyFromBuffer:pbuf sourceOffset:0 sourceBytesPerRow:64 sourceBytesPerImage:64 * 16
                    sourceSize:MTLSizeMake(16, 16, 1) toTexture:arr destinationSlice:1 destinationLevel:1
             destinationOrigin:MTLOriginMake(0, 0, 0)];
            [be copyFromTexture:arr sourceSlice:1 sourceLevel:1 sourceOrigin:MTLOriginMake(4, 4, 0)
                     sourceSize:MTLSizeMake(8, 8, 1) toTexture:arr destinationSlice:2 destinationLevel:0
              destinationOrigin:MTLOriginMake(20, 10, 0)];
            [be copyFromBuffer:pbuf sourceOffset:0 toBuffer:buf destinationOffset:0 size:buf.length];
            [be endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            bad = 0;
            [a getBytes:img bytesPerRow:64 fromRegion:MTLRegionMake2D(0, 0, 16, 16) mipmapLevel:2];
            if (memcmp(img, buf.contents, 16 * 16 * 4)) { printf("    level 2 -> private buffer -> buffer differs\n"); bad++; }
            memset(got, 0, sizeof got);
            [arr getBytes:got bytesPerRow:64 bytesPerImage:0 fromRegion:MTLRegionMake2D(0, 0, 16, 16) mipmapLevel:1 slice:1];
            if (memcmp(img, got, 16 * 16 * 4)) { printf("    buffer -> slice 1 level 1 differs\n"); bad++; }
            [arr getBytes:got bytesPerRow:32 bytesPerImage:0 fromRegion:MTLRegionMake2D(20, 10, 8, 8) mipmapLevel:0 slice:2];
            for (int y = 0; y < 8; y++)
                if (memcmp(&got[y * 8], &img[(y + 4) * 16 + 4], 32)) { printf("    texture -> texture row %d differs\n", y); bad++; break; }
            check("blits: BL level -> private buffer -> BL slice/level -> BL region", !bad);
        }
        {   // 7: stencil: mark the left half (ref 1, replace), then draw red where stencil == 0
            const MTLPixelFormat fmts[3] = {MTLPixelFormatDepth32Float_Stencil8, MTLPixelFormatDepth24Unorm_Stencil8,
                                            MTLPixelFormatStencil8};
            const char *names[3] = {"Depth32Float_Stencil8", "Depth24Unorm_Stencil8", "Stencil8"};
            for (int f = 0; f < 3; f++) {
                const bool depth = fmts[f] != MTLPixelFormatStencil8;
                MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
                rd.vertexFunction = [lib newFunctionWithName:@"v_pull"];
                rd.fragmentFunction = [lib newFunctionWithName:@"f_flat"];
                rd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
                if (depth) rd.depthAttachmentPixelFormat = fmts[f];
                rd.stencilAttachmentPixelFormat = fmts[f];
                id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:rd error:&e];
                MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
                MTLStencilDescriptor *st = [MTLStencilDescriptor new];
                st.stencilCompareFunction = MTLCompareFunctionAlways;
                st.depthStencilPassOperation = MTLStencilOperationReplace;
                dd.frontFaceStencil = st; dd.backFaceStencil = st;
                id<MTLDepthStencilState> mark = [dev newDepthStencilStateWithDescriptor:dd];
                st = [MTLStencilDescriptor new];
                st.stencilCompareFunction = MTLCompareFunctionEqual;
                dd.frontFaceStencil = st; dd.backFaceStencil = st;
                id<MTLDepthStencilState> test = [dev newDepthStencilStateWithDescriptor:dd];
                MTLTextureDescriptor *zd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmts[f]
                                                                                             width:W height:H mipmapped:NO];
                zd.usage = MTLTextureUsageRenderTarget; zd.storageMode = MTLStorageModePrivate;
                id<MTLTexture> zt = [dev newTextureWithDescriptor:zd];
                id<MTLTexture> rt = target(dev);
                const float left[12] = {-1, -1, 0, -1, -1, 1,  -1, 1, 0, -1, 0, 1};
                const float full[12] = {-1, -1, 1, -1, -1, 1,  -1, 1, 1, -1, 1, 1};
                const uint16_t green[4] = {0, 0x3c00, 0, 0x3c00}, red[4] = {0x3c00, 0, 0, 0x3c00};
                int ok = ps && mark && test && zt;
                if (ok) {
                    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                    rp.colorAttachments[0].texture = rt;
                    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                    rp.colorAttachments[0].clearColor = (MTLClearColor){0, 0, 1, 1};
                    if (depth) { rp.depthAttachment.texture = zt; rp.depthAttachment.loadAction = MTLLoadActionClear; }
                    rp.stencilAttachment.texture = zt;
                    rp.stencilAttachment.loadAction = MTLLoadActionClear;
                    rp.stencilAttachment.clearStencil = 0;
                    id<MTLCommandBuffer> cb = [q commandBuffer];
                    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
                    [re setRenderPipelineState:ps];
                    [re setDepthStencilState:mark];
                    [re setStencilReferenceValue:1];
                    [re setVertexBytes:left length:sizeof left atIndex:0];
                    [re setFragmentBytes:green length:sizeof green atIndex:0];
                    [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
                    [re setDepthStencilState:test];
                    [re setStencilReferenceValue:0];
                    [re setVertexBytes:full length:sizeof full atIndex:0];
                    [re setFragmentBytes:red length:sizeof red atIndex:0];
                    [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
                    [re endEncoding];
                    [cb commit];
                    [cb waitUntilCompleted];
                    uint32_t px[W * H];
                    readback(dev, rt, px);
                    int bad = 0;
                    for (int y = 0; y < H; y++)
                        for (int x = 0; x < W; x++) {
                            const uint32_t want = x < W / 2 ? 0xff00ff00 : 0xffff0000;
                            if (px[y * W + x] != want && bad++ < 3)
                                printf("    %s (%d,%d) %08x want %08x\n", names[f], x, y, px[y * W + x], want);
                        }
                    ok = !bad;
                }
                char what[96];
                snprintf(what, sizeof what, "stencil %s (replace, then equal 0)", names[f]);
                check(what, ok);
            }
        }
        for (int msN = 2; msN <= 8; msN *= 2) {   // 8: MSAA with a resolve (+ depth 4x)
            MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
            rd.vertexFunction = [lib newFunctionWithName:@"v_pull"];
            rd.fragmentFunction = [lib newFunctionWithName:@"f_flat"];
            rd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
            rd.rasterSampleCount = msN;
            if (msN == 4) rd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
            id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:rd error:&e];
            MTLTextureDescriptor *md = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                         width:W height:H mipmapped:NO];
            md.textureType = MTLTextureType2DMultisample; md.sampleCount = msN;
            md.usage = MTLTextureUsageRenderTarget; md.storageMode = MTLStorageModePrivate;
            id<MTLTexture> ms = [dev newTextureWithDescriptor:md];
            id<MTLTexture> zt = nil;
            if (msN == 4) {
                md.pixelFormat = MTLPixelFormatDepth32Float;
                zt = [dev newTextureWithDescriptor:md];
            }
            id<MTLTexture> rt = target(dev);
            const float tri[6] = {-1, -1, -1, 1, 1, 1};   // top-left half, edge x + y = 64
            const uint16_t green[4] = {0, 0x3c00, 0, 0x3c00};
            int ok = ps && ms && (msN != 4 || zt);
            if (ok) {
                MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                rp.colorAttachments[0].texture = ms;
                rp.colorAttachments[0].resolveTexture = rt;
                rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                rp.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
                rp.colorAttachments[0].clearColor = (MTLClearColor){0, 0, 0, 1};
                if (zt) { rp.depthAttachment.texture = zt; rp.depthAttachment.loadAction = MTLLoadActionClear;
                          rp.depthAttachment.storeAction = MTLStoreActionDontCare; }
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
                [re setRenderPipelineState:ps];
                [re setVertexBytes:tri length:sizeof tri atIndex:0];
                [re setFragmentBytes:green length:sizeof green atIndex:0];
                [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [re endEncoding];
                [cb commit];
                [cb waitUntilCompleted];
                uint32_t px[W * H];
                readback(dev, rt, px);
                // x + y < 63 green, > 63 black, x + y == 63 partly covered (every layout has a sample each side)
                int bad = 0, partial = 0;
                for (int y = 0; y < H; y++)
                    for (int x = 0; x < W; x++) {
                        const uint32_t p = px[y * W + x], g = (p >> 8) & 255;
                        if (x + y < 63 && p != 0xff00ff00) { if (bad++ < 3) printf("    in (%d,%d) %08x\n", x, y, p); }
                        else if (x + y > 63 && p != 0xff000000) { if (bad++ < 3) printf("    out (%d,%d) %08x\n", x, y, p); }
                        else if (x + y == 63 && g > 0 && g < 255) partial++;
                    }
                printf("    %dx: %d edge pixels partly covered (px(20,20) %08x)\n", msN, partial, px[20 * W + 20]);
                ok = !bad && partial > W / 2;
            }
            char what[64];
            snprintf(what, sizeof what, "MSAA %dx + resolve%s", msN, msN == 4 ? " (with 4x depth)" : "");
            check(what, ok);
        }
    }
    printf("metal_3d_test: %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
