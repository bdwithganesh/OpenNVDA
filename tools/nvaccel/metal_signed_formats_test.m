// Signed texture formats: CPU upload -> shader read + write -> CPU verification.
// Runs unchanged on the M1 reference and the RTX driver. Includes sign extrema,
// absent-channel defaults, a ragged grid, and both linear and block-linear storage.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct { MTLPixelFormat fmt; const char *name; unsigned comps, bytes; BOOL norm; } Format;
static const Format formats[] = {
    {MTLPixelFormatR8Snorm, "R8Snorm", 1, 1, YES},
    {MTLPixelFormatRGBA8Sint, "RGBA8Sint", 4, 1, NO},
    {MTLPixelFormatRG8Sint, "RG8Sint", 2, 1, NO},
    {MTLPixelFormatR16Snorm, "R16Snorm", 1, 2, YES},
    {MTLPixelFormatRG16Snorm, "RG16Snorm", 2, 2, YES},
    {MTLPixelFormatRGBA16Snorm, "RGBA16Snorm", 4, 2, YES},
    {MTLPixelFormatRG16Sint, "RG16Sint", 2, 2, NO},
    {MTLPixelFormatRG32Sint, "RG32Sint", 2, 4, NO},
};
static int32_t value(unsigned i, unsigned bytes) {
    static const int32_t v8[] = {-128, -127, -63, -1, 0, 1, 63, 126, 127};
    static const int32_t v16[] = {-32768, -32767, -16383, -1, 0, 1, 16383, 32766, 32767};
    static const int32_t v32[] = {INT32_MIN, -2000000001, -65537, -1, 0, 1, 65537, 2000000001, INT32_MAX};
    return bytes == 1 ? v8[i % 9] : bytes == 2 ? v16[i % 9] : v32[i % 9];
}
static void store(uint8_t *p, unsigned bytes, int32_t v) {
    if (bytes == 1) { int8_t x = v; memcpy(p, &x, 1); }
    else if (bytes == 2) { int16_t x = v; memcpy(p, &x, 2); }
    else memcpy(p, &v, 4);
}
static int32_t load(const uint8_t *p, unsigned bytes) {
    if (bytes == 1) { int8_t x; memcpy(&x, p, 1); return x; }
    if (bytes == 2) { int16_t x; memcpy(&x, p, 2); return x; }
    int32_t x; memcpy(&x, p, 4); return x;
}
int main(void) {
    @autoreleasepool {
        id<MTLDevice> d = MTLCreateSystemDefaultDevice();
        if (!d) return 1;
        NSError *e = nil;
        NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\n"
          @"kernel void sn(texture2d<float, access::read> a [[texture(0)]], texture2d<float, access::write> b [[texture(1)]], device float4 *o [[buffer(0)]], uint2 g [[thread_position_in_grid]]) { float4 v=a.read(g); o[g.y*9+g.x]=v; b.write(v,g); }\n"
          @"kernel void si(texture2d<int, access::read> a [[texture(0)]], texture2d<int, access::write> b [[texture(1)]], device int4 *o [[buffer(0)]], uint2 g [[thread_position_in_grid]]) { int4 v=a.read(g); o[g.y*9+g.x]=v; b.write(v,g); }\n";
        id<MTLLibrary> lib = [d newLibraryWithSource:src options:nil error:&e];
        id<MTLComputePipelineState> sn = [d newComputePipelineStateWithFunction:[lib newFunctionWithName:@"sn"] error:&e];
        id<MTLComputePipelineState> si = [d newComputePipelineStateWithFunction:[lib newFunctionWithName:@"si"] error:&e];
        if (!sn || !si) { printf("pipeline FAIL: %s\n", e.localizedDescription.UTF8String); return 1; }
        id<MTLCommandQueue> q = [d newCommandQueue];
        unsigned failed = 0;
        for (unsigned mode = 0; mode < 2; mode++) for (unsigned f = 0; f < sizeof formats / sizeof formats[0]; f++) {
            const Format *fmt = &formats[f];
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt->fmt width:9 height:3 mipmapped:mode != 0];
            td.storageMode = d.hasUnifiedMemory ? MTLStorageModeShared : (mode ? MTLStorageModeManaged : MTLStorageModeShared);
            td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
            id<MTLTexture> a = [d newTextureWithDescriptor:td], b = [d newTextureWithDescriptor:td];
            if (!a || !b) { printf("%s %s creation FAIL\n", fmt->name, mode ? "mipped" : "linear"); failed++; continue; }
            uint8_t input[27 * 16] = {0}, output[27 * 16] = {0};
            unsigned bpp = fmt->comps * fmt->bytes;
            for (unsigned i = 0; i < 27 * fmt->comps; i++) store(input + i * fmt->bytes, fmt->bytes, value(i, fmt->bytes));
            [a replaceRegion:MTLRegionMake2D(0, 0, 9, 3) mipmapLevel:0 withBytes:input bytesPerRow:9 * bpp];
            id<MTLBuffer> out = [d newBufferWithLength:27 * 16 options:MTLResourceStorageModeShared];
            memset(out.contents, 0x5a, 27 * 16);
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            [ce setComputePipelineState:fmt->norm ? sn : si];
            [ce setTexture:a atIndex:0]; [ce setTexture:b atIndex:1]; [ce setBuffer:out offset:0 atIndex:0];
            [ce dispatchThreads:MTLSizeMake(9, 3, 1) threadsPerThreadgroup:MTLSizeMake(8, 2, 1)];
            [ce endEncoding];
            if (td.storageMode == MTLStorageModeManaged) { id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder]; [blit synchronizeResource:b]; [blit endEncoding]; }
            [cb commit]; [cb waitUntilCompleted];
            [b getBytes:output bytesPerRow:9 * bpp fromRegion:MTLRegionMake2D(0, 0, 9, 3) mipmapLevel:0];
            unsigned readBad = 0, writeBad = 0;
            for (unsigned i = 0; i < 27; i++) for (unsigned c = 0; c < 4; c++) {
                int32_t raw = c < fmt->comps ? value(i * fmt->comps + c, fmt->bytes) : c == 3;
                if (fmt->norm) {
                    float want = c < fmt->comps ? fmaxf(-1, (float)raw / (fmt->bytes == 1 ? 127 : 32767)) : raw;
                    float got = ((float *)out.contents)[i * 4 + c];
                    if (!isfinite(got) || fabsf(got - want) > 0.00004f) readBad++;
                } else if (((int32_t *)out.contents)[i * 4 + c] != raw) readBad++;
                if (c < fmt->comps) {
                    int32_t got = load(output + (i * fmt->comps + c) * fmt->bytes, fmt->bytes);
                    if (llabs((long long)got - raw) > (fmt->norm ? 1 : 0)) writeBad++;
                }
            }
            BOOL ok = !readBad && !writeBad && cb.status == MTLCommandBufferStatusCompleted;
            printf("%s %s read=%u write=%u status=%lu %s\n", fmt->name, mode ? "mipped" : "linear", readBad, writeBad, (unsigned long)cb.status, ok ? "PASS" : "FAIL");
            failed += !ok;
        }
        printf("metal_signed_formats_test on %s: %u failures\n", d.name.UTF8String, failed);
        return failed != 0;
    }
}
