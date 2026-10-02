// fbcap: a burst of scan-out frames, for transient glitches (wake, animations).
// Maps NVDisplay's VRAM range (IOFramebuffer shared client, kIOFBVRAMMemory,
// NVDisplay >= 0.9.5) read-only and saves every frame at half resolution.
// Build: clang -O2 -framework IOKit -framework CoreFoundation -framework CoreGraphics
//        -framework ImageIO fbcap.c -o fbcap
// Use (console user): fbcap OUTDIR FRAMES [INTERVAL_MS] [STEP] [WIDTH HEIGHT PITCH]
// STEP 2 = half resolution (default), 4 = quarter: uncached BAR1 reads run at
// ~8 MB/s, so a quarter frame takes ~0.25 s.
#include <IOKit/IOKitLib.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void savePng(const char *path, const uint32_t *px, uint32_t w, uint32_t h) {
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate((void *)px, w, h, 8, w * 4, cs, kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    CGImageDestinationRef d = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
    CGImageDestinationAddImage(d, img, NULL);
    CGImageDestinationFinalize(d);
    CFRelease(d); CFRelease(url); CGImageRelease(img); CGContextRelease(ctx); CGColorSpaceRelease(cs);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: fbcap OUTDIR FRAMES [INTERVAL_MS] [STEP] [W H PITCH]\n"); return 2; }
    const int frames = atoi(argv[2]);
    const int intervalMs = argc > 3 ? atoi(argv[3]) : 0;
    const uint32_t step = argc > 4 ? (uint32_t)atoi(argv[4]) : 2;
    const uint32_t W = argc > 6 ? (uint32_t)atoi(argv[5]) : 3840, H = argc > 6 ? (uint32_t)atoi(argv[6]) : 2160;
    const uint32_t pitch = argc > 7 ? (uint32_t)atoi(argv[7]) : 16384;
    mkdir(argv[1], 0755);
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVDisplay"));
    io_connect_t c = 0;
    if (!s || IOServiceOpen(s, mach_task_self(), 1, &c)) { fprintf(stderr, "fbcap: cannot open NVDisplay\n"); return 1; }
    mach_vm_address_t a = 0; mach_vm_size_t sz = 0;
    kern_return_t kr = IOConnectMapMemory64(c, 110, mach_task_self(), &a, &sz, kIOMapAnywhere | kIOMapReadOnly);
    if (kr || sz < (mach_vm_size_t)pitch * H) { fprintf(stderr, "fbcap: VRAM map 0x%x size 0x%llx\n", kr, sz); return 1; }
    const uint32_t w = W / step, h = H / step;
    uint32_t *half = malloc((size_t)w * h * 4 * (size_t)frames);
    double *t = calloc((size_t)frames, sizeof(double));
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    const uint64_t t0 = mach_absolute_time();
    for (int f = 0; f < frames; f++) {                 // copy first, encode later: keep the burst fast
        const uint64_t ts = mach_absolute_time();
        t[f] = (double)(ts - t0) * tb.numer / tb.denom / 1e6;
        uint32_t *dst = half + (size_t)f * w * h;
        for (uint32_t y = 0; y < h; y++) {
            const volatile uint32_t *row = (const volatile uint32_t *)(a + (mach_vm_address_t)(y * step) * pitch);
            for (uint32_t x = 0; x < w; x++) dst[y * w + x] = row[x * step];
        }
        if (intervalMs) {
            const double spent = (double)(mach_absolute_time() - ts) * tb.numer / tb.denom / 1e6;
            if (spent < intervalMs) usleep((useconds_t)((intervalMs - spent) * 1000));
        }
    }
    for (int f = 0; f < frames; f++) {
        char p[1024];
        snprintf(p, sizeof p, "%s/f%03d.png", argv[1], f);
        savePng(p, half + (size_t)f * w * h, w, h);
        printf("f%03d %.1f ms\n", f, t[f]);
    }
    IOServiceClose(c);
    return 0;
}
