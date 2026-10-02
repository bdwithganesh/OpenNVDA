// Precompiled Metal kernels (a .metallib from Apple's compiler) on NVMTLDriver.
//   sudo metal_air_test air_kernels.metallib
// Each kernel runs through newLibraryWithData -> pipeline -> dispatch, and the
// output is checked against the same computation on the CPU.
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

static id<MTLDevice> dev;
static id<MTLCommandQueue> q;
static id<MTLLibrary> lib;

static int run(const char *name, NSArray<id<MTLBuffer>> *bufs, MTLSize grid, MTLSize block) {
    NSError *e = nil;
    id<MTLFunction> fn = [lib newFunctionWithName:@(name)];
    id<MTLComputePipelineState> ps = fn ? [dev newComputePipelineStateWithFunction:fn error:&e] : nil;
    if (!ps) { printf("  %s: no pipeline (%s)\n", name, e.localizedDescription.UTF8String); return 0; }
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:ps];
    for (NSUInteger i = 0; i < bufs.count; i++) [ce setBuffer:bufs[i] offset:0 atIndex:i];
    [ce dispatchThreads:grid threadsPerThreadgroup:block];
    [ce endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    return cb.status == MTLCommandBufferStatusCompleted;
}

static id<MTLBuffer> buf(NSUInteger bytes) {
    id<MTLBuffer> b = [dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    memset(b.contents, 0, bytes);
    return b;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 2) { fprintf(stderr, "usage: %s air_kernels.metallib\n", argv[0]); return 2; }
        dev = findNV();
        if (!dev) { printf("no NVIDIA MTLDevice\n"); return 1; }
        q = [dev newCommandQueue];
        NSData *d = [NSData dataWithContentsOfFile:@(argv[1])];
        NSError *e = nil;
        dispatch_data_t dd = dispatch_data_create(d.bytes, d.length, nil, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        lib = [dev newLibraryWithData:dd error:&e];
        if (!lib) { printf("newLibraryWithData failed: %s\n", e.localizedDescription.UTF8String); return 1; }
        printf("metallib: %lu functions\n", (unsigned long)lib.functionNames.count);
        const uint32_t N = 1 << 16;

        {   // vadd
            id<MTLBuffer> a = buf(N * 4), b = buf(N * 4), c = buf(N * 4);
            float *pa = a.contents, *pb = b.contents, *pc = c.contents;
            for (uint32_t i = 0; i < N; i++) { pa[i] = i * 0.5f; pb[i] = 1000.0f - i; }
            int ok = run("k_vadd", @[a, b, c], MTLSizeMake(N, 1, 1), MTLSizeMake(64, 1, 1));
            for (uint32_t i = 0; ok && i < N; i++) ok = pc[i] == pa[i] + pb[i] * 2.0f;
            check("k_vadd (64-thread groups)", ok);
        }
        {   // collatz: loop + phi
            id<MTLBuffer> in = buf(4096 * 4), out = buf(4096 * 4);
            uint32_t *pi = in.contents, *po = out.contents;
            for (uint32_t i = 0; i < 4096; i++) pi[i] = i + 1;
            int ok = run("k_collatz", @[in, out], MTLSizeMake(4096, 1, 1), MTLSizeMake(256, 1, 1));
            for (uint32_t i = 0; ok && i < 4096; i++) {
                uint32_t n = pi[i], s = 0;
                while (n != 1 && s < 1000) { n = (n & 1) ? 3 * n + 1 : n / 2; ++s; }
                ok = po[i] == s;
                if (!ok) printf("    [%u] got %u want %u\n", i, po[i], s);
            }
            check("k_collatz (loop, phi, select)", ok);
        }
        {   // reduce: threadgroup memory + barriers
            id<MTLBuffer> in = buf(N * 4), out = buf(N / 256 * 4);
            float *pi = in.contents, *po = out.contents;
            for (uint32_t i = 0; i < N; i++) pi[i] = (float)(i % 7);
            int ok = run("k_reduce", @[in, out], MTLSizeMake(N, 1, 1), MTLSizeMake(256, 1, 1));
            for (uint32_t g = 0; ok && g < N / 256; g++) {
                float s = 0;
                for (uint32_t i = 0; i < 256; i++) s += pi[g * 256 + i];
                ok = po[g] == s;
                if (!ok) printf("    group %u got %f want %f\n", g, po[g], s);
            }
            check("k_reduce (threadgroup memory, barriers)", ok);
        }
        {   // histogram: device atomics
            id<MTLBuffer> in = buf(N * 4), bins = buf(16 * 4);
            uint32_t *pi = in.contents, *pb = bins.contents, want[16] = {0};
            for (uint32_t i = 0; i < N; i++) { pi[i] = i * 2654435761u; want[pi[i] & 15]++; }
            int ok = run("k_hist", @[in, bins], MTLSizeMake(N, 1, 1), MTLSizeMake(256, 1, 1));
            for (int i = 0; ok && i < 16; i++) ok = pb[i] == want[i];
            check("k_hist (device atomics)", ok);
        }
        {   // constant table
            const float tab[8] = {1.5f, -2.0f, 3.25f, 0.5f, 7.0f, -1.25f, 9.5f, 4.0f};
            id<MTLBuffer> in = buf(1024 * 4), out = buf(1024 * 4);
            uint32_t *pi = in.contents;
            float *po = out.contents;
            for (uint32_t i = 0; i < 1024; i++) pi[i] = i * 3;
            int ok = run("k_table", @[in, out], MTLSizeMake(1024, 1, 1), MTLSizeMake(256, 1, 1));
            for (uint32_t i = 0; ok && i < 1024; i++) ok = po[i] == tab[pi[i] & 7] * 2.0f;
            check("k_table (constant address space global)", ok);
        }
        {   // simd_sum
            id<MTLBuffer> in = buf(4096 * 4), out = buf(128 * 4);
            int32_t *pi = in.contents, *po = out.contents;
            for (uint32_t i = 0; i < 4096; i++) pi[i] = (int32_t)(i % 13) - 6;
            int ok = run("k_simd", @[in, out], MTLSizeMake(4096, 1, 1), MTLSizeMake(256, 1, 1));
            for (uint32_t w = 0; ok && w < 128; w++) {
                int32_t s = 0;
                for (uint32_t i = 0; i < 32; i++) s += pi[w * 32 + i];
                ok = po[w] == s;
            }
            check("k_simd (simd_sum)", ok);
        }
        {   // math, 2D grid, constant struct argument
            struct { float sx, sy, bias; uint32_t width; } p = {0.01f, 0.02f, 0.5f, 128};
            id<MTLBuffer> pb = [dev newBufferWithBytes:&p length:sizeof p options:MTLResourceStorageModeShared];
            id<MTLBuffer> out = buf(128 * 64 * 16);
            int ok = run("k_math", @[pb, out], MTLSizeMake(128, 64, 1), MTLSizeMake(16, 16, 1));
            const float *po = out.contents;
            double worst = 0;
            for (uint32_t y = 0; ok && y < 64; y++)
                for (uint32_t x = 0; x < 128; x++) {
                    const float u = x * p.sx, v = y * p.sy;
                    const float want[4] = {sqrtf(u + 1.0f) + p.bias, fminf(fmaxf(sinf(v), -0.5f), 0.5f),
                                           u + (v - u) * 0.25f, u * 0.5f + v * 0.25f + 2.0f};
                    for (int k = 0; k < 4; k++) {
                        const double err = fabs(po[(y * 128 + x) * 4 + k] - want[k]);
                        if (err > worst) worst = err;
                    }
                }
            printf("    k_math worst error %.2e\n", worst);
            check("k_math (sqrt/sin/clamp/mix/dot, 2D, constant struct)", ok && worst < 1e-3);
        }
        {   // function constants: two specializations of one kernel
            int ok = 1;
            for (int pass = 0; pass < 2 && ok; pass++) {
                MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new];
                bool useB = pass == 0;
                float scale = pass == 0 ? 3.0f : 0.5f;
                int add = 7;
                [cv setConstantValue:&useB type:MTLDataTypeBool atIndex:0];
                [cv setConstantValue:&scale type:MTLDataTypeFloat atIndex:1];
                if (pass == 1) [cv setConstantValue:&add type:MTLDataTypeInt atIndex:2];
                NSError *fe = nil;
                id<MTLFunction> fn = [lib newFunctionWithName:@"k_fconst" constantValues:cv error:&fe];
                id<MTLComputePipelineState> ps = fn ? [dev newComputePipelineStateWithFunction:fn error:&fe] : nil;
                if (!ps) { printf("    k_fconst pass %d: %s\n", pass, fe.localizedDescription.UTF8String); ok = 0; break; }
                id<MTLBuffer> a = buf(1024 * 4), bb = buf(1024 * 4);
                float *pa = a.contents, *pb = bb.contents;
                for (uint32_t i = 0; i < 1024; i++) { pa[i] = (float)i; pb[i] = 1000.0f; }
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
                [ce setComputePipelineState:ps];
                [ce setBuffer:a offset:0 atIndex:0];
                if (useB) [ce setBuffer:bb offset:0 atIndex:1];
                [ce dispatchThreads:MTLSizeMake(1024, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
                [ce endEncoding];
                [cb commit];
                [cb waitUntilCompleted];
                for (uint32_t i = 0; ok && i < 1024; i++) {
                    const float want = pass == 0 ? i * 3.0f + 1000.0f : i * 0.5f + 7.0f;
                    ok = pa[i] == want;
                    if (!ok) printf("    pass %d [%u] got %f want %f\n", pass, i, pa[i], want);
                }
            }
            check("k_fconst (function constants, optional buffer, two specializations)", ok);
        }
        {   // argument buffer with a buffer pointer, a texture handle and a scalar
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float
                                                                                         width:4 height:1 mipmapped:NO];
            td.usage = MTLTextureUsageShaderRead;
            id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
            const float tv[4] = {100, 200, 300, 400};
            [tex replaceRegion:MTLRegionMake2D(0, 0, 4, 1) mipmapLevel:0 withBytes:tv bytesPerRow:16];
            id<MTLBuffer> in = buf(256 * 4), out = buf(256 * 4);
            float *pi = in.contents, *po = out.contents;
            for (uint32_t i = 0; i < 256; i++) pi[i] = (float)i;
            struct { uint64_t out, in, tex; float scale; uint32_t pad; } table = {
                out.gpuAddress, in.gpuAddress, tex.gpuResourceID._impl, 1.5f, 0};
            id<MTLBuffer> tb = [dev newBufferWithBytes:&table length:sizeof table options:MTLResourceStorageModeShared];
            int ok = out.gpuAddress && in.gpuAddress && run("k_argbuf", @[tb], MTLSizeMake(256, 1, 1), MTLSizeMake(64, 1, 1));
            for (uint32_t i = 0; ok && i < 256; i++) {
                ok = po[i] == pi[i] * 1.5f + tv[i % 4];
                if (!ok) printf("    [%u] got %f want %f\n", i, po[i], pi[i] * 1.5f + tv[i % 4]);
            }
            check("k_argbuf (argument buffer: gpuAddress, gpuResourceID)", ok);
        }
        {   // block-linear textures: mips, array, 3D, cube (RGBA32Float, uploaded with replaceRegion)
            #define TD(t, w_, h_) MTLTextureDescriptor *t = [MTLTextureDescriptor new]; t.pixelFormat = MTLPixelFormatRGBA32Float; \
                t.width = w_; t.height = h_; t.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
            TD(dm, 16, 16) dm.mipmapLevelCount = 4;
            TD(da, 8, 8) da.textureType = MTLTextureType2DArray; da.arrayLength = 4;
            TD(dv, 8, 8) dv.textureType = MTLTextureType3D; dv.depth = 4;
            TD(dc, 4, 4) dc.textureType = MTLTextureTypeCube;
            id<MTLTexture> tm = [dev newTextureWithDescriptor:dm], ta = [dev newTextureWithDescriptor:da];
            id<MTLTexture> tv = [dev newTextureWithDescriptor:dv], tc = [dev newTextureWithDescriptor:dc];
            int ok = tm && ta && tv && tc;
            float img[16 * 16 * 4];
            for (int l = 0; ok && l < 4; l++) {
                const int n = 16 >> l;
                for (int p = 0; p < n * n * 4; p++) img[p] = 100.0f * l + p;
                [tm replaceRegion:MTLRegionMake2D(0, 0, n, n) mipmapLevel:l withBytes:img bytesPerRow:n * 16];
            }
            for (int s2 = 0; ok && s2 < 4; s2++) {
                for (int p = 0; p < 8 * 8 * 4; p++) img[p] = 1000.0f * s2 + p;
                [ta replaceRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0 slice:s2 withBytes:img bytesPerRow:8 * 16 bytesPerImage:0];
                [tv replaceRegion:MTLRegionMake3D(0, 0, s2, 8, 8, 1) mipmapLevel:0 slice:0 withBytes:img bytesPerRow:8 * 16 bytesPerImage:0];
            }
            for (int f = 0; ok && f < 6; f++) {
                for (int p = 0; p < 4 * 4 * 4; p++) img[p] = 10.0f * (f + 1);
                [tc replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 slice:f withBytes:img bytesPerRow:4 * 16 bytesPerImage:0];
            }
            id<MTLBuffer> out = buf(18 * 16);
            NSError *e2 = nil;
            id<MTLComputePipelineState> ps = ok ? [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"k_tex_kinds"] error:&e2] : nil;
            if (ps) {
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
                [ce setComputePipelineState:ps];
                [ce setTexture:tm atIndex:0]; [ce setTexture:ta atIndex:1]; [ce setTexture:tv atIndex:2]; [ce setTexture:tc atIndex:3];
                [ce setBuffer:out offset:0 atIndex:0];
                [ce dispatchThreads:MTLSizeMake(18, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
                [ce endEncoding];
                [cb commit]; [cb waitUntilCompleted];
                const float *o = out.contents;
                for (int i = 0; i < 18; i++) {
                    float want;
                    if (i < 4) { const int n = 16 >> i, x = 1 >> i, y = 1 >> i; want = 100.0f * i + (y * n + x) * 4; }
                    else if (i < 8) want = 1000.0f * (i - 4) + (3 * 8 + 2) * 4;
                    else if (i < 12) want = 1000.0f * (i - 8) + (3 * 8 + 2) * 4;
                    else want = 10.0f * (i - 12 + 1);
                    if (o[i * 4] != want) { printf("    [%d] got %.1f want %.1f\n", i, o[i * 4], want); ok = 0; }
                }
            } else ok = 0;
            check("k_tex_kinds (mip levels, 2D array, 3D, cube; block linear)", ok);
            // shader writes into a 3D texture, read back with getBytes
            ok = 0;
            id<MTLComputePipelineState> pw = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"k_tex3d_write"] error:&e2];
            if (pw) {
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
                [ce setComputePipelineState:pw];
                [ce setTexture:tv atIndex:0];
                [ce dispatchThreads:MTLSizeMake(8, 8, 4) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
                [ce endEncoding];
                [cb commit]; [cb waitUntilCompleted];
                static float back[8 * 8 * 4 * 4];
                [tv getBytes:back bytesPerRow:8 * 16 bytesPerImage:8 * 8 * 16 fromRegion:MTLRegionMake3D(0, 0, 0, 8, 8, 4) mipmapLevel:0 slice:0];
                ok = 1;
                for (int z = 0; z < 4 && ok; z++) for (int y = 0; y < 8 && ok; y++) for (int x = 0; x < 8 && ok; x++) {
                    const float *p = &back[((z * 8 + y) * 8 + x) * 4];
                    ok = p[0] == x && p[1] == y && p[2] == z && p[3] == 1.0f;
                    if (!ok) printf("    (%d,%d,%d) got %.0f %.0f %.0f %.0f\n", x, y, z, p[0], p[1], p[2], p[3]);
                }
            }
            check("k_tex3d_write (3D image store, getBytes)", ok);
        }
        {   // call into a noinline function returning a struct
            id<MTLBuffer> o = buf(1024 * 4), oi = buf(1024 * 4);
            int ok = run("k_call", @[o, oi], MTLSizeMake(1024, 1, 1), MTLSizeMake(128, 1, 1));
            const float *pf = o.contents;
            const int32_t *pi = oi.contents;
            for (uint32_t i = 0; ok && i < 1024; i++) {
                float a = 0; int32_t bsum = 0;
                for (int k = 0; k < (int)(i % 7); ++k) { a += (float)i * (float)k; bsum += k; }
                ok = pf[i] == a && pi[i] == bsum;
                if (!ok) printf("    [%u] got %f %d want %f %d\n", i, pf[i], pi[i], a, bsum);
            }
            check("k_call (noinline function, struct return, loop)", ok);
        }
        {   // textures: read -> write RGBA8 into BGRA8 (swap + invert alpha)
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                         width:37 height:21 mipmapped:NO];
            td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
            id<MTLTexture> src = [dev newTextureWithDescriptor:td];
            td.pixelFormat = MTLPixelFormatBGRA8Unorm;
            id<MTLTexture> dst = [dev newTextureWithDescriptor:td];
            uint32_t px[37 * 21], back[37 * 21];
            for (uint32_t i = 0; i < 37 * 21; i++) px[i] = i * 2654435761u;
            [src replaceRegion:MTLRegionMake2D(0, 0, 37, 21) mipmapLevel:0 withBytes:px bytesPerRow:37 * 4];
            NSError *e2 = nil;
            id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"k_tex_copy"] error:&e2];
            int ok = ps != nil;
            if (ok) {
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
                [ce setComputePipelineState:ps];
                [ce setTexture:src atIndex:0];
                [ce setTexture:dst atIndex:1];
                [ce dispatchThreads:MTLSizeMake(37, 21, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
                [ce endEncoding];
                [cb commit];
                [cb waitUntilCompleted];
                [dst getBytes:back bytesPerRow:37 * 4 fromRegion:MTLRegionMake2D(0, 0, 37, 21) mipmapLevel:0];
                int bad = 0;
                for (uint32_t i = 0; i < 37 * 21; i++) {
                    /* src RGBA bytes r g b a -> write (b, g, r, 1-a) as a colour -> BGRA memory b' g' r' a'
                     * holds blue=r, green=g, red=b: memory bytes (r, g, b, 255-a) */
                    const uint32_t p = px[i];
                    const uint32_t want = (p & 0x00ffffffu) | ((255u - (p >> 24)) << 24);
                    if (back[i] != want) { if (bad < 4) printf("    texel %u got %08x want %08x\n", i, back[i], want); bad++; }
                }
                ok = !bad;
            }
            check("k_tex_copy (read RGBA8, write BGRA8, get_width/height)", ok);
        }
        {   // sampling: linear argument sampler + constexpr repeat sampler
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                                         width:8 height:8 mipmapped:NO];
            td.usage = MTLTextureUsageShaderRead;
            id<MTLTexture> src = [dev newTextureWithDescriptor:td];
            float tex[8 * 8 * 4];
            for (uint32_t i = 0; i < 64; i++) for (int k = 0; k < 4; k++) tex[i * 4 + k] = (float)(i * 4 + k);
            [src replaceRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0 withBytes:tex bytesPerRow:8 * 16];
            MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
            sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
            id<MTLSamplerState> smp = [dev newSamplerStateWithDescriptor:sd];
            id<MTLBuffer> out = buf(16 * 16 * 16);
            NSError *e2 = nil;
            id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"k_tex_sample"] error:&e2];
            int ok = ps != nil;
            double worst = 0;
            if (ok) {
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
                [ce setComputePipelineState:ps];
                [ce setTexture:src atIndex:0];
                [ce setSamplerState:smp atIndex:0];
                [ce setBuffer:out offset:0 atIndex:0];
                [ce dispatchThreads:MTLSizeMake(16, 16, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
                [ce endEncoding];
                [cb commit];
                [cb waitUntilCompleted];
                const float *o = out.contents;
                for (uint32_t y = 0; y < 16; y++)
                    for (uint32_t x = 0; x < 16; x++) {
                        /* linear, clamp to edge: pixel coords (x+0.5)/8*8 - 0.5 */
                        const float px = (x + 0.5f) - 0.5f, py = (y + 0.5f) - 0.5f;
                        const int x0 = (int)floorf(px), y0 = (int)floorf(py);
                        const float fx = px - x0, fy = py - y0;
                        for (int k = 0; k < 4; k++) {
                            #define T(xx, yy) tex[((yy) < 0 ? 0 : (yy) > 7 ? 7 : (yy)) * 32 + ((xx) < 0 ? 0 : (xx) > 7 ? 7 : (xx)) * 4 + k]
                            const float lin = (T(x0, y0) * (1 - fx) + T(x0 + 1, y0) * fx) * (1 - fy) +
                                              (T(x0, y0 + 1) * (1 - fx) + T(x0 + 1, y0 + 1) * fx) * fy;
                            /* nearest + repeat at uv + 1: texel ((x+0.5)/8 + 1) * 8 mod 8 = x mod 8 */
                            const float rep = tex[((y % 8) * 8 + (x % 8)) * 4 + k];
                            const double err = fabs(o[(y * 16 + x) * 4 + k] - (lin * 0.5f + rep * 0.25f));
                            if (err > worst) worst = err;
                        }
                    }
                printf("    k_tex_sample worst error %.3f\n", worst);
            }
            check("k_tex_sample (linear sampler + constexpr repeat sampler)", ok && worst < 0.25);
        }
    }
    printf("metal_air_test: %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
