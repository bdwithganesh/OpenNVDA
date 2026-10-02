// A process must reuse released block-linear VA space. No GPU work is
// submitted: each small private texture dies before the next allocation.
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
int main(int argc, char **argv) {
    bool capacity = argc == 2 && !strcmp(argv[1], "--capacity");
    int count = argc > 1 && !capacity ? atoi(argv[1]) : 4096;
    if (count < 1 || count > 10000) return 2;
    alarm(45);
    @autoreleasepool {
        id<MTLDevice> d = nil;
        for (id<MTLDevice> x in MTLCopyAllDevices())
            if ([x.name containsString:@"NVIDIA"] || [x.name containsString:@"RTX"]) { d = x; break; }
        if (!d) d = MTLCreateSystemDefaultDevice();
        if (!d) return 1;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:32 height:32 mipmapped:YES];
        td.storageMode = MTLStorageModePrivate;
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        printf("device=%s count=%d sequential private textures\n", d.name.UTF8String, count); fflush(stdout);
        if (capacity) {
            if (![d.name containsString:@"NVIDIA"] && ![d.name containsString:@"RTX"]) return 2;
            NSMutableArray *live = [NSMutableArray new];
            for (int i = 0; i < 1024; i++) {
                id<MTLTexture> t = [d newTextureWithDescriptor:td];
                if (!t) { printf("FAIL early capacity limit at %d\n", i); return 1; }
                [live addObject:t];
            }
            if ([d newTextureWithDescriptor:td]) { printf("FAIL untracked 1025th allocation accepted\n"); return 1; }
            for (int i = 0; i < 512; i++) [live removeObjectAtIndex:0];
            // Larger objects must coalesce the holes left by adjacent frees.
            td.width = 1024; td.height = 512;
            for (int i = 0; i < 128; i++) {
                id<MTLTexture> t = [d newTextureWithDescriptor:td];
                if (!t) { printf("FAIL fragmented reuse at %d\n", i); return 1; }
                [live addObject:t];
            }
            [live removeAllObjects];
            td.width = 32; td.height = 32;
            printf("capacity guard and larger-span reuse PASS\n"); fflush(stdout);
        }
        for (int i = 0; i < count; i++) @autoreleasepool {
            id<MTLTexture> t = [d newTextureWithDescriptor:td];
            if (!t) { printf("FAIL nil texture at allocation %d\n", i); return 1; }
            if ((i + 1) % 1024 == 0) { printf("allocated and released %d\n", i + 1); fflush(stdout); }
        }
        printf("PASS %d sequential allocations, released each iteration\n", count);
        return 0;
    }
}
