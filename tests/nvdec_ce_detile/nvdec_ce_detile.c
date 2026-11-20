/* NVGspControl >= 0.180.204 selector 45 op 6 (copy-engine detile of an NVDEC surface) against nvdec_detile_crop.
 * Run on the macOS guest as the user (op 6 needs no admin), e.g.: ./nvdec_ce_detile
 * cc -O2 -arch x86_64 -I../../drivers/nvdec nvdec_ce_detile.c ../../drivers/nvdec/nvdec_h264.c ../../drivers/nvdec/h264_parse.c -framework IOKit -framework CoreFoundation -o nvdec_ce_detile
 * Same open path as NVVTDecoder::open: the accelerator's 'NVGP' client, else NVGspControl, then 45 op 3 (owned client). */
#include "nvdec_h264.h"
#include <IOKit/IOKitLib.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static double ms(uint64_t t) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)t * tb.numer / tb.denom / 1e6;
}

static io_connect_t open_owned(void) {
    io_connect_t conn = IO_OBJECT_NULL;
    kern_return_t kr = KERN_FAILURE;
    io_service_t acc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVAccelerator"));
    if (acc) { kr = IOServiceOpen(acc, mach_task_self(), 0x4E564750, &conn); IOObjectRelease(acc); }
    if (kr) {
        io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVGspControl"));
        if (!service) return IO_OBJECT_NULL;
        kr = IOServiceOpen(service, mach_task_self(), 0, &conn);
        IOObjectRelease(service);
    }
    if (kr) return IO_OBJECT_NULL;
    uint64_t op = 3, out[3] = {0}; uint32_t count = 3;
    kr = IOConnectCallScalarMethod(conn, 45, &op, 1, out, &count);
    for (int i = 0; kr == kIOReturnBusy && i < 500; i++) { usleep(20000); count = 3; kr = IOConnectCallScalarMethod(conn, 45, &op, 1, out, &count); }
    if (kr) { IOServiceClose(conn); return IO_OBJECT_NULL; }
    return conn;
}

static void *pages(size_t bytes) {
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

#include <sys/resource.h>
static double cpu_ms(void) {
    struct rusage r; getrusage(RUSAGE_SELF, &r);
    return (r.ru_utime.tv_sec + r.ru_stime.tv_sec) * 1e3 + (r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1e3;
}

int main(void) {
    /* visible w, h, crop x0, y0, coded width */
    const uint32_t cases[][5] = {{1920, 1080, 0, 0, 1920}, {1920, 1080, 16, 8, 1936},
                                 {3840, 2160, 0, 0, 3840}, {3840, 2160, 16, 8, 3856}};
    io_connect_t conn = open_owned();
    if (!conn) { printf("FAIL: cannot open an owned client (NVGspControl/NVAccelerator missing or busy)\n"); return 1; }
    int bad = 0;
    srand(11);
    for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; ++c) {
        const uint32_t w = cases[c][0], h = cases[c][1], x0 = cases[c][2], y0 = cases[c][3];
        const uint32_t pitch = (cases[c][4] + 63) & ~63u, rows = (y0 + h + 31) & ~31u;   /* aligned_height: multiple of 32 */
        const uint32_t lumaBytes = pitch * rows;
        const size_t srcBytes = ((size_t)lumaBytes + lumaBytes / 2 + 0xffff) & ~(size_t)0xffff;
        const uint32_t dpitch = (w + 63) & ~63u;   /* pixel-buffer-like: Y then UV, UV start deliberately not page aligned */
        const size_t uvOff = (size_t)dpitch * h, dstBytes = (uvOff + (size_t)dpitch * h / 2 + 0x3fff) & ~(size_t)0x3fff;
        uint8_t *lin = malloc((size_t)lumaBytes * 3 / 2), *bl = pages(srcBytes), *dst = pages(dstBytes);
        uint8_t *ref = calloc(1, dstBytes);
        if (!lin || !bl || !dst || !ref) { printf("FAIL: out of memory\n"); return 1; }
        for (size_t i = 0; i < (size_t)lumaBytes * 3 / 2; ++i) lin[i] = (uint8_t)(rand() >> 3);
        memset(bl, 0, srcBytes);
        nvdec_tile(lin, pitch, bl, pitch, pitch, rows);                                    /* luma */
        nvdec_tile(lin + lumaBytes, pitch, bl + lumaBytes, pitch, pitch, rows / 2);        /* chroma */
        free(lin);
        /* CPU reference and its time (both planes, as emitSurface) */
        const int N = 100;
        uint64_t t0 = mach_absolute_time();
        for (int i = 0; i < N; ++i)
            for (int p = 0; p < 2; ++p)
                if (nvdec_detile_crop(bl + (p ? lumaBytes : 0), pitch, ref + (p ? uvOff : 0), dpitch, x0, y0 >> p, w, h >> p)) {
                    printf("FAIL: nvdec_detile_crop refused case %u\n", c); return 1;
                }
        const double cpuMs = ms(mach_absolute_time() - t0) / N;
        uint64_t args[] = {6, (uint64_t)(uintptr_t)bl, srcBytes, lumaBytes, pitch, x0, y0, w, h,
                           (uint64_t)(uintptr_t)dst, dpitch, (uint64_t)(uintptr_t)(dst + uvOff), dpitch, 1000 + c}, out[3];
        double first = 0, steady = 0;
        kern_return_t kr = 0;
        memset(dst, 0xA5, dstBytes);
        const double cpu0 = cpu_ms();
        for (int i = 0; i < N && !kr; ++i) {
            uint32_t count = 3;
            t0 = mach_absolute_time();
            kr = IOConnectCallScalarMethod(conn, 45, args, 14, out, &count);
            const double t = ms(mach_absolute_time() - t0);
            if (i == 0) first = t; else steady += t;
        }
        const double ceCpu = (cpu_ms() - cpu0) / N;   /* user+sys incl. kernel spin */
        int ok = !kr;
        for (int p = 0; ok && p < 2; ++p)
            for (uint32_t y = 0; ok && y < (h >> p); ++y)
                ok = !memcmp(dst + (p ? uvOff : 0) + (size_t)y * dpitch, ref + (p ? uvOff : 0) + (size_t)y * dpitch, w);
        printf("%s %ux%u crop %u,%u: CE first %.3f ms, steady %.3f ms (calls 2..%d) using %.3f ms CPU, CPU detile %.3f ms%s", ok ? "PASS" : "FAIL",
               w, h, x0, y0, first, steady / (N - 1), N, ceCpu, cpuMs, ok ? "\n" : "");
        if (!ok) printf(" (IOReturn 0x%x)\n", kr);
        bad += !ok;
        munmap(bl, srcBytes); munmap(dst, dstBytes); free(ref);
    }
    uint64_t op = 1, out[3]; uint32_t count = 3;
    IOConnectCallScalarMethod(conn, 45, &op, 1, out, &count);
    IOServiceClose(conn);
    printf(bad ? "FAIL\n" : "PASS\n");
    return bad != 0;
}
