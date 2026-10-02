// metal_private_getbytes_test: CPU reads of private textures. Apple's GPUs return the texels
// for getBytes on a private texture, and IconServices relies on it: the final Tahoe app icon is
// a private RGB10A2 render target (usage read|write|render target) read back with getBytes.
// Our driver refused it for linear private textures, so every Dock icon came out transparent
// (1 Oct 2026). Per format/usage: clear with a render pass to a known colour, getBytes a region
// (tight and padded rows), then replaceRegion a patch and read it back.
#import <Metal/Metal.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { printf("no Metal device\n"); return 1; }
        // Apple's own GPUs fault on it (no CPU mapping of private memory; apps there use shared
        // storage because hasUnifiedMemory is YES); the discrete-GPU drivers serve it.
        if (dev.hasUnifiedMemory) { printf("metal_private_getbytes_test on %s: SKIP (unified memory)\n", dev.name.UTF8String); return 0; }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        // usage 7 = shader read | write | render target (linear on our side), 5 = read | render target
        struct { const char *name; MTLPixelFormat f; uint32_t bpp; MTLTextureUsage u; } cases[] = {
            { "rgb10a2 rw+rt", MTLPixelFormatRGB10A2Unorm, 4, 7 },
            { "rgb10a2 rt", MTLPixelFormatRGB10A2Unorm, 4, 5 },
            { "bgra8 rw+rt", MTLPixelFormatBGRA8Unorm, 4, 7 },
            { "rgba8 rw+rt", MTLPixelFormatRGBA8Unorm, 4, 7 },
            { "rgba16f rw+rt", MTLPixelFormatRGBA16Float, 8, 7 },
        };
        // the clear colour (0.25, 0.5, 0.75, 1) is checked with a tolerance (8-bit 0.5 is 127 or 128)
        int fails = 0;
        for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
            const int W = 130, H = 130;
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:cases[c].f width:W height:H mipmapped:NO];
            td.usage = cases[c].u;
            td.storageMode = MTLStorageModePrivate;
            id<MTLTexture> t = [dev newTextureWithDescriptor:td];
            id<MTLCommandBuffer> cb = [q commandBuffer];
            MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
            p.colorAttachments[0].texture = t;
            p.colorAttachments[0].loadAction = MTLLoadActionClear;
            p.colorAttachments[0].clearColor = MTLClearColorMake(0.25, 0.5, 0.75, 1);
            p.colorAttachments[0].storeAction = MTLStoreActionStore;
            [[cb renderCommandEncoderWithDescriptor:p] endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            const uint32_t bpp = cases[c].bpp;
            // padded rows, like IconServices (768 bytes for 130 pixels)
            const NSUInteger bpr = 768 * (bpp / 4);
            uint8_t *buf = calloc(bpr * H, 1);
            [t getBytes:buf bytesPerRow:bpr fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
            int bad = 0;
            for (int y = 0; y < H && !bad; y += 7)
                for (int x = 0; x < W; x += 5) {
                    const uint8_t *e = buf + y * bpr + x * bpp;
                    float v[4];
                    if (cases[c].f == MTLPixelFormatRGB10A2Unorm) { uint32_t w = *(const uint32_t *)e;
                        v[0] = (w & 1023) / 1023.f; v[1] = (w >> 10 & 1023) / 1023.f; v[2] = (w >> 20 & 1023) / 1023.f; v[3] = (w >> 30) / 3.f; }
                    else if (cases[c].f == MTLPixelFormatBGRA8Unorm) { v[0] = e[2] / 255.f; v[1] = e[1] / 255.f; v[2] = e[0] / 255.f; v[3] = e[3] / 255.f; }
                    else if (cases[c].f == MTLPixelFormatRGBA8Unorm) { for (int i = 0; i < 4; i++) v[i] = e[i] / 255.f; }
                    else { for (int i = 0; i < 4; i++) v[i] = (float)((const __fp16 *)e)[i]; }
                    const float want[4] = { 0.25f, 0.5f, 0.75f, 1 };
                    for (int i = 0; i < 4; i++) if (v[i] < want[i] - 0.01f || v[i] > want[i] + 0.01f) bad = 1;
                    if (bad) { printf("  %s (%d,%d) got (%.3f %.3f %.3f %.3f)\n", cases[c].name, x, y, v[0], v[1], v[2], v[3]); break; }
                }
            // replaceRegion a 10x3 patch of 0x5a, read back a region around it
            uint8_t patch[10 * 3 * 8];
            memset(patch, 0x5a, sizeof patch);
            [t replaceRegion:MTLRegionMake2D(20, 30, 10, 3) mipmapLevel:0 withBytes:patch bytesPerRow:10 * bpp];
            uint8_t back[12 * 5 * 8];
            [t getBytes:back bytesPerRow:12 * bpp fromRegion:MTLRegionMake2D(19, 29, 12, 5) mipmapLevel:0];
            int rbad = 0;
            for (int y = 0; y < 5; y++)
                for (int x = 0; x < 12; x++) {
                    const int in = x >= 1 && x <= 10 && y >= 1 && y <= 3;
                    const uint8_t *e = back + (y * 12 + x) * bpp;
                    const uint8_t *o = buf + (29 + y) * bpr + (19 + x) * bpp;   // the clear colour from before
                    for (uint32_t k = 0; k < bpp; k++) if (e[k] != (in ? 0x5a : o[k])) rbad = 1;
                }
            printf("%-16s getBytes %s replaceRegion %s\n", cases[c].name, bad ? "FAIL" : "ok", rbad ? "FAIL" : "ok");
            fails += bad + rbad;
            free(buf);
        }
        printf("metal_private_getbytes_test on %s: %s\n", dev.name.UTF8String, fails ? "FAIL" : "PASS");
        return fails ? 1 : 0;
    }
}
