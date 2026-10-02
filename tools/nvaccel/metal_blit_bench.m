// metal_blit_bench: texture upload / readback / copy speed through blits
// (buffer <-> block-linear texture, texture -> texture), 4096x4096 RGBA8.
//   sudo metal_blit_bench
#import <Metal/Metal.h>
#include <stdio.h>
#include <mach/mach_time.h>

static double ms(uint64_t t) { static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb); return (double)t * tb.numer / tb.denom / 1e6; }

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = nil;
        for (id<MTLDevice> d in MTLCopyAllDevices()) if ([d.name containsString:@"NVIDIA"]) dev = d;
        if (!dev) { printf("no NVIDIA MTLDevice\n"); return 1; }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        const NSUInteger W = 4096, H = 4096, bytes = W * H * 4;
        id<MTLBuffer> src = [dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        id<MTLBuffer> back = [dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        id<MTLBuffer> vram = [dev newBufferWithLength:bytes options:MTLResourceStorageModePrivate];
        uint32_t *s = src.contents;
        for (NSUInteger i = 0; i < W * H; i++) s[i] = (uint32_t)(i * 2654435761u);
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                     width:W height:H mipmapped:NO];
        td.storageMode = MTLStorageModePrivate;
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        td.textureType = MTLTextureType2DArray; td.arrayLength = 1;       // block linear
        id<MTLTexture> a = [dev newTextureWithDescriptor:td], b = [dev newTextureWithDescriptor:td];
        const char *names[5] = {"shared buffer -> texture", "VRAM buffer -> texture", "texture -> texture",
                                "texture -> shared buffer", "texture -> VRAM buffer"};
        // warm VRAM buffer
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
        [be copyFromBuffer:src sourceOffset:0 toBuffer:vram destinationOffset:0 size:bytes];
        [be endEncoding]; [cb commit]; [cb waitUntilCompleted];
        for (int k = 0; k < 5; k++) {
            double best = 1e9;
            for (int r = 0; r < 3; r++) {
                cb = [q commandBuffer];
                be = [cb blitCommandEncoder];
                const MTLSize sz = MTLSizeMake(W, H, 1);
                const MTLOrigin o0 = MTLOriginMake(0, 0, 0);
                if (k == 0 || k == 1)
                    [be copyFromBuffer:k ? vram : src sourceOffset:0 sourceBytesPerRow:W * 4 sourceBytesPerImage:bytes
                            sourceSize:sz toTexture:a destinationSlice:0 destinationLevel:0 destinationOrigin:o0];
                else if (k == 2)
                    [be copyFromTexture:a sourceSlice:0 sourceLevel:0 sourceOrigin:o0 sourceSize:sz toTexture:b
                       destinationSlice:0 destinationLevel:0 destinationOrigin:o0];
                else
                    [be copyFromTexture:b sourceSlice:0 sourceLevel:0 sourceOrigin:o0 sourceSize:sz
                               toBuffer:k == 3 ? back : vram destinationOffset:0 destinationBytesPerRow:W * 4
                destinationBytesPerImage:bytes];
                [be endEncoding];
                const uint64_t t0 = mach_absolute_time();
                [cb commit]; [cb waitUntilCompleted];
                const double d = ms(mach_absolute_time() - t0);
                if (d < best) best = d;
            }
            printf("  %-28s %7.2f ms  %6.1f GB/s\n", names[k], best, bytes / best / 1e6);
        }
        const int ok = !memcmp(back.contents, s, bytes);
        printf("metal_blit_bench: round trip %s\n", ok ? "PASS" : "FAIL");
        return ok ? 0 : 1;
    }
}
