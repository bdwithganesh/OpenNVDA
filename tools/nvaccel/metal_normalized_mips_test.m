// Normalized mipmap generation on M1 and RTX. CPU verifies every lower level
// of power-of-two 2D/3D textures, including signed values and quantization.
// NVMTL_CPU_MIPS=1 exercises the RTX CPU fallback with the same checks.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { MTLPixelFormat fmt; const char *name; unsigned comps, bytes; BOOL sign; } Format;
static const Format formats[] = {
    {MTLPixelFormatR8Unorm, "R8Unorm", 1, 1, NO},
    {MTLPixelFormatR8Snorm, "R8Snorm", 1, 1, YES},
    {MTLPixelFormatRG8Snorm, "RG8Snorm", 2, 1, YES},
    {MTLPixelFormatRGBA8Snorm, "RGBA8Snorm", 4, 1, YES},
    {MTLPixelFormatR16Unorm, "R16Unorm", 1, 2, NO},
    {MTLPixelFormatRG16Unorm, "RG16Unorm", 2, 2, NO},
    {MTLPixelFormatRGBA16Unorm, "RGBA16Unorm", 4, 2, NO},
    {MTLPixelFormatR16Snorm, "R16Snorm", 1, 2, YES},
    {MTLPixelFormatRG16Snorm, "RG16Snorm", 2, 2, YES},
    {MTLPixelFormatRGBA16Snorm, "RGBA16Snorm", 4, 2, YES},
};
static int32_t load(const uint8_t *p, const Format *f) {
    if (f->bytes == 1) return f->sign ? *(const int8_t *)p : *p;
    if (f->sign) { int16_t v; memcpy(&v, p, 2); return v; }
    uint16_t v; memcpy(&v, p, 2); return v;
}
static void store(uint8_t *p, const Format *f, int32_t v) {
    if (f->bytes == 1) *p = (uint8_t)v;
    else { uint16_t x = (uint16_t)v; memcpy(p, &x, 2); }
}
int main(void) {
    @autoreleasepool {
        id<MTLDevice> d = MTLCreateSystemDefaultDevice();
        if (!d) return 1;
        id<MTLCommandQueue> q = [d newCommandQueue];
        unsigned failures = 0;
        for (unsigned dim = 2; dim <= 3; dim++) for (unsigned k = 0; k < sizeof formats / sizeof formats[0]; k++) {
            const Format *f = &formats[k];
            NSUInteger w = 8, h = 4, z = dim == 3 ? 2 : 1;
            NSUInteger bpp = f->comps * f->bytes;
            MTLTextureDescriptor *td = [MTLTextureDescriptor new];
            td.textureType = dim == 3 ? MTLTextureType3D : MTLTextureType2D;
            td.width = w; td.height = h; td.depth = z; td.mipmapLevelCount = 4;
            td.pixelFormat = f->fmt;
            td.storageMode = d.hasUnifiedMemory ? MTLStorageModeShared : MTLStorageModeManaged;
            td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
            id<MTLTexture> t = [d newTextureWithDescriptor:td];
            if (!t) { printf("%s %uD creation FAIL\n", f->name, dim); failures++; continue; }
            uint8_t src[8 * 4 * 2 * 8] = {0}, dst[8 * 4 * 2 * 8] = {0}, got[sizeof dst];
            for (NSUInteger zz = 0; zz < z; zz++) for (NSUInteger y = 0; y < h; y++)
                for (NSUInteger x = 0; x < w; x++) for (unsigned c = 0; c < f->comps; c++) {
                    int32_t v = (int32_t)(x * 8 + y * 12 + zz * 16 + c * 4);
                    if (f->sign) v -= 60;
                    if (f->bytes == 2) v *= 256;
                    store(src + ((zz * h + y) * w + x) * bpp + c * f->bytes, f, v);
                }
            MTLRegion region = MTLRegionMake3D(0, 0, 0, w, h, z);
            [t replaceRegion:region mipmapLevel:0 slice:0 withBytes:src bytesPerRow:w * bpp bytesPerImage:w * h * bpp];
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            [blit generateMipmapsForTexture:t];
            if (t.storageMode == MTLStorageModeManaged) [blit synchronizeResource:t];
            [blit endEncoding]; [cb commit]; [cb waitUntilCompleted];
            unsigned bad = 0;
            for (NSUInteger level = 1; level < 4; level++) {
                NSUInteger nw = MAX(w / 2, 1u), nh = MAX(h / 2, 1u), nz = MAX(z / 2, 1u);
                for (NSUInteger zz = 0; zz < nz; zz++) for (NSUInteger y = 0; y < nh; y++)
                    for (NSUInteger x = 0; x < nw; x++) for (unsigned c = 0; c < f->comps; c++) {
                        int64_t sum = 0; unsigned count = 0;
                        for (NSUInteger dz = 0; dz < (z > 1 ? 2u : 1u); dz++)
                            for (NSUInteger dy = 0; dy < (h > 1 ? 2u : 1u); dy++)
                                for (NSUInteger dx = 0; dx < (w > 1 ? 2u : 1u); dx++) {
                                    sum += load(src + (((zz * 2 + dz) * h + y * 2 + dy) * w + x * 2 + dx) * bpp + c * f->bytes, f);
                                    count++;
                                }
                        store(dst + ((zz * nh + y) * nw + x) * bpp + c * f->bytes, f, (int32_t)lrint((double)sum / count));
                    }
                memset(got, 0x5a, sizeof got);
                [t getBytes:got bytesPerRow:nw * bpp bytesPerImage:nw * nh * bpp fromRegion:MTLRegionMake3D(0, 0, 0, nw, nh, nz) mipmapLevel:level slice:0];
                for (NSUInteger i = 0; i < nw * nh * nz * f->comps; i++)
                    bad += abs(load(got + i * f->bytes, f) - load(dst + i * f->bytes, f)) > 1;
                memcpy(src, dst, nw * nh * nz * bpp);
                w = nw; h = nh; z = nz;
            }
            BOOL ok = !bad && cb.status == MTLCommandBufferStatusCompleted;
            printf("%s %uD mip1..3 bad=%u status=%lu %s\n", f->name, dim, bad, (unsigned long)cb.status, ok ? "PASS" : "FAIL");
            failures += !ok;
        }
        printf("metal_normalized_mips_test on %s: %u failures\n", d.name.UTF8String, failures);
        return failures != 0;
    }
}
