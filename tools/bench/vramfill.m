// Low-VRAM behaviour: allocate Private textures / buffers (256 MiB each) past the 16 GiB of VRAM,
// write each on the GPU (texture: render-pass clear, buffer: blit fill), read one value back.
// PASS = no nil before LIMIT GiB, every resource holds its value, the process does not crash.
// clang -fobjc-arc -O2 vramfill.m -framework Metal -framework Foundation -o vramfill
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    @autoreleasepool {
        const int limitGiB = argc > 1 ? atoi(argv[1]) : 24;
        const bool buffers = argc > 2 && !strcmp(argv[2], "buf");
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> q = [dev newCommandQueue];
        printf("%s, recommendedMaxWorkingSetSize %.1f GiB, mode %s\n", dev.name.UTF8String,
               dev.recommendedMaxWorkingSetSize / 1073741824.0, buffers ? "private buffers" : "private textures");
        NSMutableArray *res = [NSMutableArray array];
        id<MTLBuffer> rb = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
        const uint64_t each = 256ull << 20;
        int bad = 0, nilAt = -1;
        NSDate *t0 = [NSDate date];
        for (int i = 0; (uint64_t)i * each < ((uint64_t)limitGiB << 30); i++) {
          @autoreleasepool {   // command buffers / pass descriptors must not keep released resources alive
            const uint8_t v = (uint8_t)(i * 37 + 11);
            id<MTLCommandBuffer> cb = [q commandBuffer];
            if (buffers) {
                id<MTLBuffer> b = [dev newBufferWithLength:each options:MTLResourceStorageModePrivate];
                if (!b) { nilAt = i; goto done; }
                [res addObject:b];
                id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
                [bl fillBuffer:b range:NSMakeRange(0, each) value:v];
                [bl copyFromBuffer:b sourceOffset:each - 4096 toBuffer:rb destinationOffset:0 size:4096];
                [bl endEncoding];
            } else {
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                              width:8192 height:8192 mipmapped:NO];
                td.storageMode = MTLStorageModePrivate;
                td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                id<MTLTexture> t = [dev newTextureWithDescriptor:td];
                if (!t) { nilAt = i; goto done; }
                [res addObject:t];
                MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                rp.colorAttachments[0].texture = t;
                rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                rp.colorAttachments[0].clearColor = MTLClearColorMake(v / 255.0, 0, 1, 1);
                [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
                id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
                [bl copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(8191, 8191, 0)
                         sourceSize:MTLSizeMake(1, 1, 1) toBuffer:rb destinationOffset:0
                destinationBytesPerRow:4 destinationBytesPerImage:4];
                [bl endEncoding];
            }
            [cb commit];
            [cb waitUntilCompleted];
            const uint8_t got = ((const uint8_t *)rb.contents)[0];
            const bool ok = cb.status == MTLCommandBufferStatusCompleted && got == v;
            if (!ok && bad++ < 6) printf("  #%d: status %ld got %u want %u\n", i, (long)cb.status, got, v);
            if (i % 8 == 7) printf("  %d x 256 MiB = %.1f GiB, currentAllocatedSize %.1f GiB\n", i + 1,
                                   (i + 1) / 4.0, dev.currentAllocatedSize / 1073741824.0);
          }
        }
      done:;
        // the first resources again: still intact after the later ones went to system memory?
        int rebad = 0;
        for (NSUInteger i = 0; i < res.count && i < 4; i++) {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
            if (buffers) [bl copyFromBuffer:res[i] sourceOffset:0 toBuffer:rb destinationOffset:0 size:4];
            else [bl copyFromTexture:res[i] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(1, 1, 1) toBuffer:rb destinationOffset:0
               destinationBytesPerRow:4 destinationBytesPerImage:4];
            [bl endEncoding]; [cb commit]; [cb waitUntilCompleted];
            rebad += ((const uint8_t *)rb.contents)[0] != (uint8_t)(i * 37 + 11);
        }
        const double s = -[t0 timeIntervalSinceNow];
        printf("allocated %lu x 256 MiB = %.1f GiB in %.1f s, first nil at %d, %d bad, %d bad on re-read: %s\n",
               (unsigned long)res.count, res.count / 4.0, s, nilAt, bad, rebad,
               (nilAt < 0 && !bad && !rebad) ? "PASS" : "FAIL");
        if (getenv("VRAMFILL_HOLD")) {   // stay idle with everything alive (another app may evict us), then check all
            printf("holding %s s in use\n", getenv("VRAMFILL_HOLD")); fflush(stdout);
            sleep((unsigned)atoi(getenv("VRAMFILL_HOLD")));
            int holdbad = 0;
            for (NSUInteger i = 0; i < res.count; i++) {
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
                if (buffers) [bl copyFromBuffer:res[i] sourceOffset:each / 2 toBuffer:rb destinationOffset:0 size:4];
                else [bl copyFromTexture:res[i] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(4097, 3001, 0)
                              sourceSize:MTLSizeMake(1, 1, 1) toBuffer:rb destinationOffset:0
                   destinationBytesPerRow:4 destinationBytesPerImage:4];
                [bl endEncoding]; [cb commit]; [cb waitUntilCompleted];
                holdbad += cb.status != MTLCommandBufferStatusCompleted ||
                           ((const uint8_t *)rb.contents)[0] != (uint8_t)(i * 37 + 11);
            }
            printf("after hold: %d of %lu bad: %s\n", holdbad, (unsigned long)res.count, holdbad ? "FAIL" : "PASS");
        }
        [res removeAllObjects];
        // after release: VRAM must be usable again
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                      width:8192 height:8192 mipmapped:NO];
        td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> again = [dev newTextureWithDescriptor:td];
        printf("after release: new 256 MiB texture %s\n", again ? "ok" : "nil");
        again = nil;
        if (argc > 3) { printf("holding %s s\n", argv[3]); fflush(stdout); sleep((unsigned)atoi(argv[3])); }
        return (nilAt < 0 && !bad && !rebad) ? 0 : 1;
    }
}
