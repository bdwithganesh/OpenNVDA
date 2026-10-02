// Big IOSurface textures (VideoToolbox writes a 6016x6016 HEIC into one):
// a kernel writes a marker into the last texel and the CPU reads it back
// through the surface. Sizes 2048, 4096, 6016, 8192 (R32Uint, 4 B/texel).
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <Foundation/Foundation.h>
#include <stdlib.h>

static NSString *const kSrc =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "kernel void k(texture2d<uint, access::write> t [[texture(0)]], constant uint2 &p [[buffer(0)]], uint2 g [[thread_position_in_grid]]) {\n"
     "  t.write(uint4(0x5a5a0000u | g.x), p + g); }\n";

int main(int argc, char **argv) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:nil error:&e];
        id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"k"] error:&e];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        const int sizes[4] = {2048, 4096, 6016, 8192};
        const int nsizes = argc > 1 ? atoi(argv[1]) : 4;
        int bad = 0;
        for (int si = 0; si < nsizes && si < 4; si++) {
            const int n = sizes[si];
            IOSurfaceRef io = IOSurfaceCreate((__bridge CFDictionaryRef)@{(id)kIOSurfaceWidth: @(n), (id)kIOSurfaceHeight: @(n),
                (id)kIOSurfaceBytesPerElement: @4, (id)kIOSurfacePixelFormat: @('BGRA')});
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Uint width:n height:n mipmapped:NO];
            td.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
            td.storageMode = MTLStorageModeManaged;
            id<MTLTexture> t = [dev newTextureWithDescriptor:td iosurface:io plane:0];
            if (!t) { printf("  %5d: no texture\n", n); bad++; CFRelease(io); continue; }
            // the last 64x64 corner
            const uint32_t p[2] = {(uint32_t)n - 64, (uint32_t)n - 64};
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            [ce setComputePipelineState:ps];
            [ce setTexture:t atIndex:0];
            [ce setBytes:p length:8 atIndex:0];
            [ce dispatchThreads:MTLSizeMake(64, 64, 1) threadsPerThreadgroup:MTLSizeMake(32, 32, 1)];
            [ce endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            IOSurfaceLock(io, kIOSurfaceLockReadOnly, NULL);
            const uint8_t *base = IOSurfaceGetBaseAddress(io);
            const size_t rb = IOSurfaceGetBytesPerRow(io);
            const uint32_t last = *(const uint32_t *)(base + (size_t)(n - 1) * rb + (size_t)(n - 1) * 4);
            IOSurfaceUnlock(io, kIOSurfaceLockReadOnly, NULL);
            printf("  %5d x %-5d (%zu MB, rowBytes %zu): last texel 0x%08x %s, status %ld\n", n, n, IOSurfaceGetAllocSize(io) >> 20, rb, last,
                   last == (0x5a5a0000u | 63u) ? "ok" : "WRONG", (long)cb.status);
            bad += last != (0x5a5a0000u | 63u);
            CFRelease(io);
        }
        printf("metal_big_iosurface_test: %s\n", bad ? "FAIL" : "PASS");
        return bad != 0;
    }
}
