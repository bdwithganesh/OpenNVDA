// M20: an IOSurface shared between two processes, both sides on the GPU.
//   iosurface_xproc            parent: makes the surface, runs the child, checks
//   iosurface_xproc child ID   (internal) child: looks the surface up, GPU-writes it
// The surface is BGRA8 with a padded row pitch (odd width, so bytesPerRow is
// rounded up the way CoreAnimation surfaces are), so the pitch path is
// tested too. Checks:
// 1 child GPU write -> parent CPU read (every pixel)
// 2 parent GPU read of the same surface (its own mapping) -> buffer -> CPU
// 3 parent GPU write -> child CPU read (second child run, "verify" mode)
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <CoreVideo/CoreVideo.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>

extern char **environ;

static const size_t W = 333, H = 201;

static id<MTLDevice> findNV(void) {
    for (id<MTLDevice> d in MTLCopyAllDevices())
        if ([d.name containsString:@"NVIDIA"]) return d;
    return nil;
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

static id<MTLTexture> wrap(id<MTLDevice> dev, IOSurfaceRef io) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                 width:W height:H mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    return [dev newTextureWithDescriptor:td iosurface:io plane:0];
}

// expected BGRA bytes at (x, y) for a given seed
static uint32_t pat(size_t x, size_t y, uint32_t seed) {
    const uint32_t b = (uint32_t)(x * 3 + seed) & 0xff, g = (uint32_t)(y * 5 + seed) & 0xff;
    const uint32_t r = (uint32_t)(x ^ y) & 0xff;
    return b | g << 8 | r << 16 | 0xffu << 24;
}

// GPU write of pat(seed) into the surface (float channels of a BGRA8 texture are r,g,b,a)
static int gpuWrite(id<MTLDevice> dev, IOSurfaceRef io, uint32_t seed) {
    id<MTLTexture> t = wrap(dev, io);
    id<MTLComputePipelineState> ps = mkPipe(dev,
        @"kernel void wr(texture2d<float, access::write> dst [[texture(0)]],"
        @"               constant uint *seed [[buffer(0)]],"
        @"               uint2 gid [[thread_position_in_grid]]) {"
        @"  uint s = seed[0];"
        @"  float b = float((gid.x * 3u + s) & 255u) / 255.0;"
        @"  float g = float((gid.y * 5u + s) & 255u) / 255.0;"
        @"  float r = float((gid.x ^ gid.y) & 255u) / 255.0;"
        @"  dst.write(float4(r, g, b, 1.0), gid); }", @"wr");
    if (!t || !ps) return 0;
    id<MTLCommandQueue> q = [dev newCommandQueue];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:ps];
    [ce setTexture:t atIndex:0];
    [ce setBytes:&seed length:4 atIndex:0];
    [ce dispatchThreads:MTLSizeMake(W, H, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    [ce endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    return cb.status == MTLCommandBufferStatusCompleted;
}

static size_t cpuBad(IOSurfaceRef io, uint32_t seed) {
    IOSurfaceLock(io, kIOSurfaceLockReadOnly, NULL);
    const uint8_t *base = IOSurfaceGetBaseAddress(io);
    const size_t bpr = IOSurfaceGetBytesPerRow(io);
    size_t bad = 0;
    for (size_t y = 0; y < H; y++)
        for (size_t x = 0; x < W; x++)
            if (((const uint32_t *)(base + y * bpr))[x] != pat(x, y, seed)) bad++;
    if (bad && getenv("XPROC_DEBUG")) {
        const uint32_t *r0 = (const uint32_t *)base, *r1 = (const uint32_t *)(base + bpr);
        printf("     px(0,0)=%08x want %08x  px(1,0)=%08x want %08x  px(0,1)=%08x want %08x\n",
               r0[0], pat(0, 0, seed), r0[1], pat(1, 0, seed), r1[0], pat(0, 1, seed));
    }
    IOSurfaceUnlock(io, kIOSurfaceLockReadOnly, NULL);
    return bad;
}

static int child(uint32_t sid, int verify) {
    @autoreleasepool {
        IOSurfaceRef io = IOSurfaceLookup(sid);
        if (!io) { printf("  child: IOSurfaceLookup(%u) failed\n", sid); return 2; }
        if (verify) {
            const size_t bad = cpuBad(io, 77);
            printf("  child: CPU sees the parent's GPU write, %zu bad pixels\n", bad);
            CFRelease(io);
            return bad ? 1 : 0;
        }
        id<MTLDevice> dev = findNV();
        if (!dev) { printf("  child: no NVIDIA MTLDevice\n"); return 2; }
        const int ok = gpuWrite(dev, io, 11);
        printf("  child pid %d: GPU write %s\n", getpid(), ok ? "done" : "FAILED");
        CFRelease(io);
        return ok ? 0 : 1;
    }
}

static int runChild(const char *self, uint32_t sid, const char *mode) {
    char idb[16];
    snprintf(idb, sizeof idb, "%u", sid);
    char *argv[] = {(char *)self, (char *)mode, idb, NULL};
    pid_t pid = 0;
    if (posix_spawn(&pid, self, NULL, NULL, argv, environ) != 0) return -1;
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "child")) return child((uint32_t)strtoul(argv[2], NULL, 0), 0);
    if (argc == 3 && !strcmp(argv[1], "verify")) return child((uint32_t)strtoul(argv[2], NULL, 0), 1);
    int fails = 0;
    @autoreleasepool {
        id<MTLDevice> dev = findNV();
        if (!dev) { printf("no NVIDIA MTLDevice\n"); return 1; }
        const size_t bpr = IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, W * 4);
        NSDictionary *props = @{(id)kIOSurfaceWidth: @(W), (id)kIOSurfaceHeight: @(H),
                                (id)kIOSurfacePixelFormat: @(kCVPixelFormatType_32BGRA),
                                (id)kIOSurfaceBytesPerElement: @4,
                                (id)kIOSurfaceBytesPerRow: @(bpr),
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                                (id)kIOSurfaceIsGlobal: @YES};
#pragma clang diagnostic pop
        IOSurfaceRef io = IOSurfaceCreate((CFDictionaryRef)props);
        if (!io) { printf("IOSurfaceCreate failed\n"); return 1; }
        const uint32_t sid = IOSurfaceGetID(io);
        printf("surface %u: %zux%zu BGRA8, bytesPerRow %zu (tight would be %zu)\n",
               sid, W, H, IOSurfaceGetBytesPerRow(io), W * 4);
        IOSurfaceLock(io, 0, NULL);
        memset(IOSurfaceGetBaseAddress(io), 0, IOSurfaceGetAllocSize(io));
        IOSurfaceUnlock(io, 0, NULL);

        // 1: another process writes it on the GPU
        const int rc = runChild(argv[0], sid, "child");
        size_t bad = cpuBad(io, 11);
        printf("  %-60s %s\n", "1 child GPU write -> parent CPU read",
               rc == 0 && !bad ? "PASS" : "FAIL");
        if (rc || bad) { printf("     child rc %d, %zu bad pixels\n", rc, bad); fails++; }

        // 2: parent reads the same pages through its own GPU mapping
        id<MTLTexture> t = wrap(dev, io);
        id<MTLComputePipelineState> ps = mkPipe(dev,
            @"kernel void rd(texture2d<float, access::read> src [[texture(0)]],"
            @"               device uint *out [[buffer(0)]],"
            @"               uint2 gid [[thread_position_in_grid]]) {"
            @"  float4 c = src.read(gid);"
            @"  uint r = uint(c.x * 255.0 + 0.5), g = uint(c.y * 255.0 + 0.5);"
            @"  uint b = uint(c.z * 255.0 + 0.5), a = uint(c.w * 255.0 + 0.5);"
            @"  out[gid.y * 333u + gid.x] = b | (g << 8) | (r << 16) | (a << 24); }", @"rd");
        id<MTLBuffer> out = [dev newBufferWithLength:W * H * 4 options:MTLResourceStorageModeShared];
        size_t gbad = W * H;
        if (t && ps && out) {
            id<MTLCommandQueue> q = [dev newCommandQueue];
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            [ce setComputePipelineState:ps];
            [ce setTexture:t atIndex:0];
            [ce setBuffer:out offset:0 atIndex:0];
            [ce dispatchThreads:MTLSizeMake(W, H, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
            [ce endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            const uint32_t *o = out.contents;
            gbad = 0;
            for (size_t y = 0; y < H; y++)
                for (size_t x = 0; x < W; x++)
                    if (o[y * W + x] != pat(x, y, 11)) gbad++;
        }
        printf("  %-60s %s\n", "2 parent GPU read of the child's pixels", !gbad ? "PASS" : "FAIL");
        if (gbad) { printf("     %zu bad pixels\n", gbad); fails++; }

        // 3: parent writes on the GPU, a fresh process checks on the CPU
        const int wok = gpuWrite(dev, io, 77);
        const int vrc = wok ? runChild(argv[0], sid, "verify") : -1;
        printf("  %-60s %s\n", "3 parent GPU write -> other process CPU read",
               wok && vrc == 0 ? "PASS" : "FAIL");
        if (!wok || vrc) fails++;
        CFRelease(io);
    }
    printf("%s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
