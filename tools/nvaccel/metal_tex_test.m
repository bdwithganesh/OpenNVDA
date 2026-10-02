// Metal textures and samplers on NVMTLDriver (M16/M17), each check verified on the CPU.
//   metal_tex_test
// 1 RGBA8 texture read -> RGBA32Float write, 2D grid with 16x16 threadgroups and a
//   ragged edge (grid not a multiple of the group)
// 2 R8 / RG16Float / R32Uint round trips (sub-word atomic writes)
// 3 sampler: nearest and linear, clamp and repeat, normalized coordinates
// 4 get_width/get_height and dispatchThreadgroups
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
    printf("  %-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) fails++;
}

static id<MTLComputePipelineState> mkPipe(id<MTLDevice> dev, NSString *src, NSString *fn) {
    NSError *e = nil;
    NSString *full = [@"#include <metal_stdlib>\nusing namespace metal;\n" stringByAppendingString:src];
    id<MTLLibrary> lib = [dev newLibraryWithSource:full options:nil error:&e];
    id<MTLFunction> f = [lib newFunctionWithName:fn];
    id<MTLComputePipelineState> ps = f ? [dev newComputePipelineStateWithFunction:f error:&e] : nil;
    if (!ps) printf("  pipeline %s: %s\n", fn.UTF8String, e.localizedDescription.UTF8String ?: "nil");
    return ps;
}

static id<MTLTexture> tex(id<MTLDevice> dev, MTLPixelFormat f, NSUInteger w, NSUInteger h) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    return [dev newTextureWithDescriptor:td];
}

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = findNV();
        if (!dev) { printf("no NVIDIA MTLDevice\n"); return 1; }
        id<MTLCommandQueue> q = [dev newCommandQueue];

        // 1: RGBA8 -> RGBA32F copy with a ragged 2D grid
        {
            const NSUInteger W = 100, H = 70;
            id<MTLTexture> a = tex(dev, MTLPixelFormatRGBA8Unorm, W, H);
            id<MTLTexture> b = tex(dev, MTLPixelFormatRGBA32Float, W, H);
            uint8_t *px = malloc(W * H * 4);
            for (NSUInteger i = 0; i < W * H * 4; i++) px[i] = (uint8_t)(i * 7 + 3);
            [a replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:px bytesPerRow:W * 4];
            id<MTLComputePipelineState> ps = mkPipe(dev,
                @"kernel void cp(texture2d<float, access::read> src [[texture(0)]],"
                @"               texture2d<float, access::write> dst [[texture(1)]],"
                @"               uint2 gid [[thread_position_in_grid]]) {"
                @"  float4 c = src.read(gid); dst.write(c * 2.0, gid); }", @"cp");
            check("RGBA8 read / RGBA32F write pipeline", ps != nil);
            if (ps && a && b) {
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
                [ce setComputePipelineState:ps];
                [ce setTexture:a atIndex:0];
                [ce setTexture:b atIndex:1];
                [ce dispatchThreads:MTLSizeMake(W, H, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
                [ce endEncoding];
                [cb commit]; [cb waitUntilCompleted];
                float *out = malloc(W * H * 16);
                [b getBytes:out bytesPerRow:W * 16 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
                int bad = 0;
                for (NSUInteger i = 0; i < W * H * 4; i++)
                    if (fabsf(out[i] - 2.0f * px[i] / 255.0f) > 1e-3f) bad++;
                char msg[80];
                snprintf(msg, sizeof msg, "RGBA8 -> RGBA32F x2, 100x70 in 16x16 groups (%d bad)", bad);
                check(msg, bad == 0);
                free(out);
            }
            free(px);
        }

        // 2: sub-word formats and uint textures
        {
            const NSUInteger W = 33, H = 9;
            id<MTLTexture> r8 = tex(dev, MTLPixelFormatR8Unorm, W, H);
            id<MTLTexture> rg16 = tex(dev, MTLPixelFormatRG16Float, W, H);
            id<MTLTexture> r32u = tex(dev, MTLPixelFormatR32Uint, W, H);
            id<MTLComputePipelineState> ps = mkPipe(dev,
                @"kernel void gen(texture2d<float, access::write> a [[texture(0)]],"
                @"                texture2d<float, access::write> b [[texture(1)]],"
                @"                texture2d<uint, access::write> c [[texture(2)]],"
                @"                uint2 g [[thread_position_in_grid]]) {"
                @"  float v = float(g.x + g.y * 33u) / 400.0;"
                @"  a.write(float4(v, 0.0, 0.0, 1.0), g);"
                @"  b.write(float4(v, -v, 0.0, 1.0), g);"
                @"  c.write(uint4(g.x * 1000u + g.y, 0u, 0u, 0u), g); }", @"gen");
            check("R8 / RG16F / R32Uint write pipeline", ps != nil);
            if (ps && r8 && rg16 && r32u) {
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
                [ce setComputePipelineState:ps];
                [ce setTexture:r8 atIndex:0];
                [ce setTexture:rg16 atIndex:1];
                [ce setTexture:r32u atIndex:2];
                [ce dispatchThreads:MTLSizeMake(W, H, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
                [ce endEncoding];
                [cb commit]; [cb waitUntilCompleted];
                uint8_t a[33 * 9]; uint16_t bb[33 * 9 * 2]; uint32_t c[33 * 9];
                [r8 getBytes:a bytesPerRow:W fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
                [rg16 getBytes:bb bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
                [r32u getBytes:c bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
                int badA = 0, badB = 0, badC = 0;
                for (NSUInteger y = 0; y < H; y++)
                    for (NSUInteger x = 0; x < W; x++) {
                        const NSUInteger i = y * W + x;
                        const float v = (float)(x + y * 33) / 400.0f;
                        if (abs((int)a[i] - (int)lrintf(v * 255.0f)) > 1) badA++;
                        _Float16 h0, h1;
                        memcpy(&h0, &bb[2 * i], 2); memcpy(&h1, &bb[2 * i + 1], 2);
                        if (fabsf((float)h0 - v) > 2e-3f || fabsf((float)h1 + v) > 2e-3f) badB++;
                        if (c[i] != x * 1000 + y) badC++;
                    }
                char msg[96];
                snprintf(msg, sizeof msg, "R8 (atomic byte writes) %d bad, RG16F %d bad, R32Uint %d bad", badA, badB, badC);
                check(msg, !badA && !badB && !badC);
            }
        }

        // 3: sampling
        {
            const NSUInteger W = 4, H = 4;
            id<MTLTexture> t = tex(dev, MTLPixelFormatR32Float, W, H);
            float v[16];
            for (int i = 0; i < 16; i++) v[i] = (float)i;   // value = x + 4y
            [t replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:v bytesPerRow:W * 4];
            MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
            sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
            id<MTLSamplerState> nearestClamp = [dev newSamplerStateWithDescriptor:sd];
            sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
            id<MTLSamplerState> linearClamp = [dev newSamplerStateWithDescriptor:sd];
            sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeRepeat;
            sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
            id<MTLSamplerState> nearestRepeat = [dev newSamplerStateWithDescriptor:sd];
            id<MTLComputePipelineState> ps = mkPipe(dev,
                @"kernel void smp(texture2d<float, access::sample> t [[texture(0)]],"
                @"                sampler s [[sampler(0)]],"
                @"                device const float2 *uv [[buffer(0)]],"
                @"                device float *out [[buffer(1)]],"
                @"                uint i [[thread_position_in_grid]]) {"
                @"  out[i] = t.sample(s, uv[i]).x; }", @"smp");
            check("sample pipeline", ps != nil);
            const float uv[8] = {0.625f, 0.375f,    // texel (2,1) centre -> 6
                                 0.5f, 0.5f,        // between (1,1),(2,1),(1,2),(2,2)
                                 1.125f, 0.125f,    // x past the edge
                                 -0.01f, 0.95f};
            struct { id<MTLSamplerState> s; const char *name; float want[4]; } cases[] = {
                {nearestClamp, "nearest clamp", {6.0f, 10.0f, 3.0f, 12.0f}},
                {linearClamp, "linear clamp", {6.0f, 7.5f, 3.0f, 12.0f}},
                {nearestRepeat, "nearest repeat", {6.0f, 10.0f, 0.0f, 15.0f}},
            };
            for (int c = 0; ps && c < 3; c++) {
                id<MTLBuffer> ub = [dev newBufferWithBytes:uv length:sizeof uv options:MTLResourceStorageModeShared];
                id<MTLBuffer> ob = [dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
                [ce setComputePipelineState:ps];
                [ce setTexture:t atIndex:0];
                [ce setSamplerState:cases[c].s atIndex:0];
                [ce setBuffer:ub offset:0 atIndex:0];
                [ce setBuffer:ob offset:0 atIndex:1];
                [ce dispatchThreads:MTLSizeMake(4, 1, 1) threadsPerThreadgroup:MTLSizeMake(4, 1, 1)];
                [ce endEncoding];
                [cb commit]; [cb waitUntilCompleted];
                const float *o = ob.contents;
                int ok = 1;
                for (int k = 0; k < 4; k++) if (fabsf(o[k] - cases[c].want[k]) > 1e-3f) ok = 0;
                char msg[120];
                snprintf(msg, sizeof msg, "%s: %.3g %.3g %.3g %.3g (want %.3g %.3g %.3g %.3g)", cases[c].name,
                         o[0], o[1], o[2], o[3], cases[c].want[0], cases[c].want[1], cases[c].want[2], cases[c].want[3]);
                check(msg, ok);
            }
        }

        // 4: size queries + dispatchThreadgroups
        {
            id<MTLTexture> t = tex(dev, MTLPixelFormatRGBA16Float, 37, 21);
            id<MTLComputePipelineState> ps = mkPipe(dev,
                @"kernel void dims(texture2d<half, access::read_write> t [[texture(0)]],"
                @"                 device uint *out [[buffer(0)]],"
                @"                 uint2 g [[thread_position_in_grid]]) {"
                @"  if (g.x == 0u && g.y == 0u) { out[0] = t.get_width(); out[1] = t.get_height(); }"
                @"  t.write(half4(1.0, 0.5, 0.25, 1.0), g); }", @"dims");
            check("dims pipeline", ps != nil);
            if (ps && t) {
                id<MTLBuffer> ob = [dev newBufferWithLength:8 options:MTLResourceStorageModeShared];
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
                [ce setComputePipelineState:ps];
                [ce setTexture:t atIndex:0];
                [ce setBuffer:ob offset:0 atIndex:0];
                [ce dispatchThreadgroups:MTLSizeMake(5, 3, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
                [ce endEncoding];
                [cb commit]; [cb waitUntilCompleted];
                const uint32_t *o = ob.contents;
                uint16_t px[37 * 21 * 4];
                [t getBytes:px bytesPerRow:37 * 8 fromRegion:MTLRegionMake2D(0, 0, 37, 21) mipmapLevel:0];
                int bad = 0;
                for (int i = 0; i < 37 * 21; i++) {
                    _Float16 r, a;
                    memcpy(&r, &px[4 * i], 2); memcpy(&a, &px[4 * i + 3], 2);
                    if ((float)r != 1.0f || (float)a != 1.0f) bad++;
                }
                char msg[96];
                snprintf(msg, sizeof msg, "get_width/height %u x %u, RGBA16F fill %d bad", o[0], o[1], bad);
                check(msg, o[0] == 37 && o[1] == 21 && !bad);
            }
        }
        printf("metal_tex_test: %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
        return fails != 0;
    }
}
