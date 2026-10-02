// metal_heap_churn_test: the Vision/MPS heap pattern (M1 trace of three
// Vision requests: 27 heaps, 653 sub-resources, 500 freed, the available
// size asked before each). A 4 MiB heap must serve 2000 allocations when
// every resource is dropped (or made aliasable) again, textures must name
// their heap, and the size a texture reserves must cover its mips.
#import <Metal/Metal.h>
#include <stdio.h>
int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = nil;
        for (id<MTLDevice> d in MTLCopyAllDevices()) if ([d.name containsString:@"NVIDIA"] || [d.name containsString:@"RTX"]) dev = d;
        if (!dev) dev = MTLCreateSystemDefaultDevice();
        MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
        hd.size = 4 << 20; hd.storageMode = MTLStorageModePrivate;
        id<MTLHeap> heap = [dev newHeapWithDescriptor:hd];
        const NSUInteger avail0 = [heap maxAvailableSizeWithAlignment:256];
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                                    width:256 height:256 mipmapped:YES];
        td.storageMode = MTLStorageModePrivate; td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        const MTLSizeAndAlign sa = [dev heapTextureSizeAndAlignWithDescriptor:td];
        int fails = 0;
        if (sa.size < 256 * 256 * 8) { printf("FAIL: mipmapped texture reserves %lu bytes\n", (unsigned long)sa.size); fails++; }
        for (int i = 0; i < 2000; i++) @autoreleasepool {
            id<MTLBuffer> b = [heap newBufferWithLength:512 << 10 options:MTLResourceStorageModePrivate];
            id<MTLTexture> t = [heap newTextureWithDescriptor:td];
            if (!b || !t) { printf("FAIL: allocation %d returned nil (available %lu)\n", i, (unsigned long)[heap maxAvailableSizeWithAlignment:256]); fails++; break; }
            if (t.heap != heap) { printf("FAIL: texture.heap is %p, not its heap\n", (__bridge void *)t.heap); fails++; break; }
            if (i & 1) { [b makeAliasable]; [t makeAliasable]; }
        }
        const NSUInteger avail1 = [heap maxAvailableSizeWithAlignment:256];
        if (avail1 < avail0) { printf("FAIL: available %lu after churn, %lu before\n", (unsigned long)avail1, (unsigned long)avail0); fails++; }
        printf("metal_heap_churn_test on %s: %s (available %lu -> %lu, texture %lu bytes)\n", dev.name.UTF8String,
               fails ? "FAIL" : "PASS", (unsigned long)avail0, (unsigned long)avail1, (unsigned long)sa.size);
        return fails != 0;
    }
}
