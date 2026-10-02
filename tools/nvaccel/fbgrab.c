// fbgrab: the scan-out frame as the display shows it, read from VRAM through
// NVGspControl (selector 11, 1024 words a call), saved as PNG. No TCC, no
// WindowServer: what the head scans is what we get (tearing, missing layers).
// Build on the target: clang -O2 -framework IOKit -framework CoreFoundation
//   -framework CoreGraphics -framework ImageIO fbgrab.c -o fbgrab
// Use: sudo ./fbgrab out.png [vram_offset width height pitch]   (4K BGRA at 0)
#include <IOKit/IOKitLib.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: sudo fbgrab out.png [vram_offset width height pitch]\n"); return 2; }
    const uint64_t off = argc > 2 ? strtoull(argv[2], NULL, 0) : 0;
    const uint32_t w = argc > 3 ? (uint32_t)atoi(argv[3]) : 3840, h = argc > 4 ? (uint32_t)atoi(argv[4]) : 2160;
    const uint32_t pitch = argc > 5 ? (uint32_t)atoi(argv[5]) : w * 4;
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVGspControl"));
    io_connect_t c = 0;
    if (!s || IOServiceOpen(s, mach_task_self(), 0, &c) != KERN_SUCCESS) { fprintf(stderr, "fbgrab: no NVGspControl\n"); return 1; }
    const size_t bytes = (size_t)pitch * h;
    uint8_t *buf = malloc(bytes);
    for (size_t done = 0; done < bytes;) {
        const uint32_t words = (uint32_t)((bytes - done) / 4 > 1024 ? 1024 : (bytes - done) / 4);
        uint64_t in[3] = {off + done, words, 0};
        size_t out = words * 4;
        const kern_return_t kr = IOConnectCallMethod(c, 11, in, 3, NULL, 0, NULL, NULL, buf + done, &out);
        if (kr != KERN_SUCCESS) { fprintf(stderr, "fbgrab: vram read at 0x%llx: 0x%x\n", (unsigned long long)(off + done), kr); return 1; }
        done += (size_t)words * 4;
    }
    IOServiceClose(c);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(buf, w, h, 8, pitch, cs, kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    CGImageRef img = ctx ? CGBitmapContextCreateImage(ctx) : NULL;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)argv[1], (CFIndex)strlen(argv[1]), false);
    CGImageDestinationRef d = img && url ? CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL) : NULL;
    if (!d) { fprintf(stderr, "fbgrab: cannot write %s\n", argv[1]); return 1; }
    CGImageDestinationAddImage(d, img, NULL);
    const bool ok = CGImageDestinationFinalize(d);
    printf("fbgrab: %ux%u pitch %u from VRAM 0x%llx -> %s %s\n", w, h, pitch, (unsigned long long)off, argv[1], ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
