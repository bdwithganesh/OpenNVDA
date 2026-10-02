// List Metal devices (B4 M1 check): MTLCopyAllDevices + registry IDs.
#import <Metal/Metal.h>
#include <stdio.h>
int main(void) {
    @autoreleasepool {
        NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
        printf("%lu Metal device(s)\n", (unsigned long)all.count);
        for (id<MTLDevice> d in all)
            printf("  %s  registryID 0x%llx  lowPower %d  headless %d  maxBuffer %llu MiB\n",
                   d.name.UTF8String, d.registryID, d.lowPower, d.headless,
                   (unsigned long long)(d.maxBufferLength >> 20));
        id<MTLDevice> def = MTLCreateSystemDefaultDevice();
        printf("default: %s\n", def ? def.name.UTF8String : "(none)");
    }
    return 0;
}
