// mtl_first: the smallest Metal client. Opening it (device + one blit) builds the NVGspControl maintenance channel
// that the CE framebuffer copy needs; nvdisp615-boot.sh ran the 384 MiB vram_test benchmark (4.3 s) for that alone.
// Exit 0 only when the blit completed and wrote the expected word.
#import <Metal/Metal.h>
#include <stdio.h>
#include <string.h>
#include <mach/mach_time.h>
static double ms(uint64_t a) { mach_timebase_info_data_t t; mach_timebase_info(&t); return (double)(mach_absolute_time() - a) * t.numer / t.denom / 1e6; }
int main(int argc, char **argv) {
    @autoreleasepool {
        const uint64_t t0 = mach_absolute_time();
        id<MTLDevice> d = MTLCreateSystemDefaultDevice();
        const double tDev = ms(t0);
        if (!d) { puts("mtl_first: no device"); return 1; }
        if (argc > 1 && !strcmp(argv[1], "open")) {
            // open mode: a private (VRAM) allocation opens the kernel client without compiling any shader
            id<MTLBuffer> p = [d newBufferWithLength:65536 options:MTLResourceStorageModePrivate];
            printf("mtl_first open: device %.0f ms, private alloc %.0f ms, %s\n", tDev, ms(t0) - tDev, p ? "ok" : "nil");
            return p ? 0 : 1;
        }
        id<MTLBuffer> b = [d newBufferWithLength:4096 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = [[d newCommandQueue] commandBuffer];
        id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
        [e fillBuffer:b range:NSMakeRange(0, 4096) value:0x5a]; [e endEncoding];
        [cb commit]; [cb waitUntilCompleted];
        const unsigned v = ((const unsigned *)b.contents)[1023];
        printf("mtl_first: device %.0f ms, first blit %.0f ms; %s status %ld word %08x\n", tDev, ms(t0) - tDev, v == 0x5a5a5a5au ? "PASS" : "FAIL", (long)cb.status, v);
        return v == 0x5a5a5a5au ? 0 : 1;
    }
}
