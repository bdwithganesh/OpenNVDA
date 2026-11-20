/* Stage 1 of hardware H.264 encode: NVGspControl >= 0.180.207 selector 45 op 5 with engine 1 (NVENC0, class C9B7).
 * Encodes 30 synthetic 1920x1080 IDR-only frames at QP 26, writes Annex-B, decodes it with VideoToolbox (software decoder
 * requested) and prints PASS when every frame decodes with Y PSNR > 30 dB. Run on the macOS guest as the user (no admin).
 *   ./nvenc_one [--status] [out.h264]        default out /tmp/nvenc_one.h264; --status dumps frame 0's raw status words.
 * Run with /Library/Preferences/nvmtl-nvvt-off present if Apple's decoder must be forced (NVVTDecoder is H.264 NVDEC).
 * cc -O2 -arch x86_64 -I../../drivers/nvidia-metal-drivers-macos/nvenc nvenc_one.c ../../drivers/nvidia-metal-drivers-macos/nvenc/nvenc_h264.c -framework IOKit -framework CoreFoundation -framework CoreMedia -framework CoreVideo -framework VideoToolbox -o nvenc_one
 * Same open path as NVVTDecoder::open: the accelerator's 'NVGP' client, else NVGspControl, then 45 op 3 (owned client);
 * buffers = selector-47 sysmem allocations (handle index -> IOConnectMapMemory64 type 0x80615001 + index);
 * 45 op 4 {4, 1} opens NVENC0 in the client; 45 op 5 {5, push VA, words, 1} submits; the fence is the engine semaphore. */
#include "nvenc_h264.h"
#include <IOKit/IOKitLib.h>
#include <VideoToolbox/VideoToolbox.h>
#include <mach/mach_time.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { W = 1920, H = 1080, QP = 26, FPS = 30, FRAMES = 30, SUBCH = 4 };

static double ms(uint64_t t) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)t * tb.numer / tb.denom / 1e6;
}

static io_connect_t conn;
typedef struct { uint64_t index, va, bytes; uint8_t *cpu; } buf;

static int open_owned(void) {
    kern_return_t kr = KERN_FAILURE;
    io_service_t acc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVAccelerator"));
    if (acc) { kr = IOServiceOpen(acc, mach_task_self(), 0x4E564750, &conn); IOObjectRelease(acc); }
    if (kr) {
        io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVGspControl"));
        if (!service) return -1;
        kr = IOServiceOpen(service, mach_task_self(), 0, &conn);
        IOObjectRelease(service);
    }
    if (kr) return -1;
    uint64_t op = 3, out[3] = {0}; uint32_t count = 3;
    kr = IOConnectCallScalarMethod(conn, 45, &op, 1, out, &count);
    for (int i = 0; kr == kIOReturnBusy && i < 500; i++) { usleep(20000); count = 3; kr = IOConnectCallScalarMethod(conn, 45, &op, 1, out, &count); }
    if (kr) { IOServiceClose(conn); conn = IO_OBJECT_NULL; return -1; }
    return 0;
}

static int alloc_buf(buf *b, uint64_t bytes) {
    b->bytes = (bytes + 0xffff) & ~0xffffull;
    uint64_t args[] = {0, b->bytes}, out[3] = {0}; uint32_t count = 3;
    if (IOConnectCallScalarMethod(conn, 47, args, 2, out, &count) || out[2] != b->bytes) return -1;
    b->index = out[0]; b->va = out[1];
    mach_vm_address_t address = 0; mach_vm_size_t size = 0;
    if (IOConnectMapMemory64(conn, 0x80615001u + (uint32_t)out[0], mach_task_self(), &address, &size, kIOMapAnywhere)) return -1;
    b->cpu = (uint8_t *)address;
    if (size < b->bytes) return -1;
    memset(b->cpu, 0, b->bytes);
    return 0;
}

static void free_buf(buf *b) {
    if (!b->va) return;
    if (b->cpu) IOConnectUnmapMemory64(conn, 0x80615001u + (uint32_t)b->index, mach_task_self(), (mach_vm_address_t)b->cpu);
    uint64_t args[] = {1, b->index}, out[3]; uint32_t count = 3;
    IOConnectCallScalarMethod(conn, 47, args, 2, out, &count);
    memset(b, 0, sizeof *b);
}

/* Smooth gradient plus a 200x200 box that moves 50 px right and 10 px down per frame. */
static void gen_frame(unsigned f, uint8_t *nv12) {
    uint8_t *y = nv12, *uv = nv12 + (size_t)W * H;
    const unsigned bx = 20 + f * 50, by = 100 + f * 10;
    for (unsigned r = 0; r < H; ++r)
        for (unsigned c = 0; c < W; ++c)
            y[(size_t)r * W + c] = (c >= bx && c < bx + 200 && r >= by && r < by + 200) ? 235 : (uint8_t)(16 + c * 180 / W + r * 40 / H);
    for (unsigned r = 0; r < H / 2; ++r)
        for (unsigned c = 0; c < W / 2; ++c) {
            const int in = 2 * c >= bx && 2 * c < bx + 200 && 2 * r >= by && 2 * r < by + 200;
            uv[(size_t)r * W + 2 * c] = in ? 90 : (uint8_t)(64 + c * 128 / (W / 2));
            uv[(size_t)r * W + 2 * c + 1] = in ? 240 : (uint8_t)(192 - r * 128 / (H / 2));
        }
}

static void dump_status(const uint8_t *status, unsigned frame) {
    const uint32_t *w = (const uint32_t *)status;
    printf("status words, frame %u (pic_stat 0-31, slice_stat 32-35, semaphore word %u):\n", frame, NVENC_SEM_OFFSET / 4);
    for (unsigned i = 0; i < 36; i += 4) printf("  [%2u] %08x %08x %08x %08x\n", i, w[i], w[i + 1], w[i + 2], w[i + 3]);
    printf("  sem %08x\n", w[NVENC_SEM_OFFSET / 4]);
}

/* ---- VideoToolbox decode ---- */
static struct { int got[FRAMES]; double psnr[FRAMES]; OSStatus err; int dim_bad; uint8_t *src; } vt;

static void on_frame(void *refcon, void *frame_ref, OSStatus status, VTDecodeInfoFlags flags, CVImageBufferRef img,
                     CMTime pts, CMTime duration) {
    (void)refcon; (void)flags; (void)pts; (void)duration;
    const int f = (int)(intptr_t)frame_ref;
    if (status || !img || f < 0 || f >= FRAMES) { vt.err = status ? status : -1; return; }
    if (CVPixelBufferGetWidth(img) != W || CVPixelBufferGetHeight(img) != H) { vt.dim_bad = 1; return; }
    gen_frame((unsigned)f, vt.src);
    CVPixelBufferLockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
    const uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(img, 0);
    const size_t stride = CVPixelBufferGetBytesPerRowOfPlane(img, 0);
    double sse = 0;
    for (unsigned r = 0; r < H; ++r)
        for (unsigned c = 0; c < W; ++c) {
            const int d = (int)y[r * stride + c] - (int)vt.src[(size_t)r * W + c];
            sse += (double)d * d;
        }
    CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
    vt.psnr[f] = sse ? 10 * log10(255.0 * 255.0 * W * H / sse) : 99;
    vt.got[f] = 1;
}

static const uint8_t *next_start(const uint8_t *p, const uint8_t *end) {   /* 00 00 00 01 */
    for (; p + 4 <= end; ++p) if (!p[0] && !p[1] && !p[2] && p[3] == 1) return p;
    return end;
}

static int decode_all(const uint8_t *hdr, size_t hdr_n, uint8_t *const *frames, const size_t *sizes) {
    const uint8_t *sps = hdr + 4, *pps = next_start(sps, hdr + hdr_n);
    if (pps == hdr + hdr_n) { printf("FAIL: headers() has no PPS\n"); return 1; }
    const uint8_t *sets[2] = {sps, pps + 4};
    const size_t lens[2] = {(size_t)(pps - sps), (size_t)(hdr + hdr_n - pps - 4)};
    CMVideoFormatDescriptionRef fmt = NULL;
    OSStatus st = CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault, 2, sets, lens, 4, &fmt);
    if (st) { printf("FAIL: CMVideoFormatDescriptionCreateFromH264ParameterSets %d\n", (int)st); return 1; }

    CFMutableDictionaryRef spec = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(spec, kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder, kCFBooleanFalse);
    int fmt_v = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    CFNumberRef num = CFNumberCreate(NULL, kCFNumberIntType, &fmt_v);
    CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(attrs, kCVPixelBufferPixelFormatTypeKey, num);
    VTDecompressionOutputCallbackRecord cb = {on_frame, NULL};
    VTDecompressionSessionRef session = NULL;
    st = VTDecompressionSessionCreate(NULL, fmt, spec, attrs, &cb, &session);
    if (st) { printf("FAIL: VTDecompressionSessionCreate %d\n", (int)st); return 1; }
    CFTypeRef hw = NULL;
    if (!VTSessionCopyProperty(session, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder, NULL, &hw) && hw) {
        printf("VT decoder: %s\n", CFBooleanGetValue(hw) ? "hardware (NVDEC plug-in); create /Library/Preferences/nvmtl-nvvt-off for Apple's" : "software");
        CFRelease(hw);
    }
    vt.src = malloc((size_t)W * H * 3 / 2);
    int bad = 0;
    for (int f = 0; f < FRAMES && !bad; ++f) {
        const uint8_t *nal = frames[f];
        const size_t prefix = nal[2] == 1 ? 3 : 4;
        size_t n = sizes[f] - prefix;
        while (n && nal[prefix + n - 1] == 0) --n;   /* trailing_zero_8bits are not part of the NAL */
        uint8_t *avcc = malloc(4 + n);
        avcc[0] = (uint8_t)(n >> 24); avcc[1] = (uint8_t)(n >> 16); avcc[2] = (uint8_t)(n >> 8); avcc[3] = (uint8_t)n;
        memcpy(avcc + 4, nal + prefix, n);
        CMBlockBufferRef bb = NULL; CMSampleBufferRef sb = NULL;
        const size_t total = 4 + n;
        CMSampleTimingInfo timing = {CMTimeMake(1, FPS), CMTimeMake(f, FPS), kCMTimeInvalid};
        st = CMBlockBufferCreateWithMemoryBlock(NULL, NULL, total, NULL, NULL, 0, total, kCMBlockBufferAssureMemoryNowFlag, &bb);
        if (!st) st = CMBlockBufferReplaceDataBytes(avcc, bb, 0, total);
        if (!st) st = CMSampleBufferCreateReady(NULL, bb, fmt, 1, 1, &timing, 1, &total, &sb);
        if (!st) st = VTDecompressionSessionDecodeFrame(session, sb, 0, (void *)(intptr_t)f, NULL);
        if (st) { printf("FAIL: frame %d does not decode (VT status %d)\n", f, (int)st); bad = 1; }
        if (sb) CFRelease(sb);
        if (bb) CFRelease(bb);
        free(avcc);
    }
    VTDecompressionSessionFinishDelayedFrames(session);
    VTDecompressionSessionWaitForAsynchronousFrames(session);
    VTDecompressionSessionInvalidate(session);
    CFRelease(session); CFRelease(fmt); CFRelease(spec); CFRelease(attrs); CFRelease(num);
    free(vt.src);
    if (bad) return 1;
    if (vt.err) { printf("FAIL: VT output callback error %d\n", (int)vt.err); return 1; }
    if (vt.dim_bad) { printf("FAIL: decoded size is not %dx%d (SPS crop)\n", W, H); return 1; }
    double min = 1e9;
    for (int f = 0; f < FRAMES; ++f) {
        if (!vt.got[f]) { printf("FAIL: frame %d produced no picture\n", f); bad = 1; continue; }
        printf("frame %2d: Y PSNR %.2f dB\n", f, vt.psnr[f]);
        if (vt.psnr[f] < min) min = vt.psnr[f];
        if (vt.psnr[f] <= 30) { printf("FAIL: frame %d Y PSNR %.2f dB <= 30\n", f, vt.psnr[f]); bad = 1; }
    }
    if (!bad) printf("min Y PSNR %.2f dB\n", min);
    return bad;
}

extern unsigned nvenc_h264_push_skip;
int main(int argc, char **argv) {
    if (getenv("NVENC_SKIP")) nvenc_h264_push_skip = (unsigned)strtoul(getenv("NVENC_SKIP"), NULL, 0);
    const char *path = "/tmp/nvenc_one.h264";
    int want_status = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--status")) want_status = 1; else path = argv[i];
    }
    const nvenc_h264_config cfg = {W, H, QP, FPS, 0, 2};   /* ipcm off, block height 2^2 = 4 GOBs (32 rows) */
    nvenc_h264_layout layout;
    if (nvenc_h264_layout_init(&cfg, &layout)) { printf("FAIL: nvenc_h264_layout_init refused the config\n"); return 1; }
    if (open_owned()) { printf("FAIL: cannot open an owned client (NVGspControl/NVAccelerator missing or busy)\n"); return 1; }

    buf input = {0}, recon = {0}, bits = {0}, hist = {0}, coloc = {0}, setup = {0}, status = {0}, scratch = {0}, mepred = {0}, push = {0};
    uint8_t *frames[FRAMES] = {0}; size_t sizes[FRAMES] = {0};
    uint8_t hdr[512], *raw = malloc((size_t)W * H * 3 / 2);
    int rc = 1, owned = 1;
    if (alloc_buf(&input, layout.surface_bytes) || alloc_buf(&recon, layout.surface_bytes) ||
        alloc_buf(&bits, layout.bitstream_bytes) || alloc_buf(&hist, layout.history_bytes) ||
        alloc_buf(&coloc, layout.coloc_bytes) || alloc_buf(&setup, NVENC_SETUP_BYTES) ||
        alloc_buf(&status, NVENC_STATUS_BYTES) || alloc_buf(&scratch, 256u << 10) || alloc_buf(&mepred, 1u << 20) || alloc_buf(&push, 64u << 10)) {
        printf("FAIL: buffer allocation (selector 47)\n"); goto done;
    }
    printf("buffers: surface %u, bitstream %u, history %u, coloc %u bytes\n", layout.surface_bytes, layout.bitstream_bytes,
           layout.history_bytes, layout.coloc_bytes);
    {   /* NVENC0 channel in the client (stage 7 = ready) */
        uint64_t args[] = {4, 1}, out[3] = {0}; uint32_t count = 3;
        kern_return_t kr = IOConnectCallScalarMethod(conn, 45, args, 2, out, &count);
        printf("open NVENC0: IOReturn 0x%x, stage %llu, RM status 0x%llx, scheduled %llu\n", kr, out[0], out[1], out[2]);
        if (kr) { printf("FAIL: NVENC0 open (45 op 4 engine 1)\n"); goto done; }
    }
    const size_t hdr_n = nvenc_h264_headers(&cfg, hdr, sizeof hdr);
    if (!hdr_n) { printf("FAIL: nvenc_h264_headers\n"); goto done; }
    FILE *fp = fopen(path, "wb");
    if (!fp) { printf("FAIL: cannot write %s\n", path); goto done; }
    fwrite(hdr, 1, hdr_n, fp);

    volatile uint32_t *sem = (volatile uint32_t *)(status.cpu + NVENC_SEM_OFFSET);
    int bad = 0;
    for (unsigned f = 0; f < FRAMES && !bad; ++f) {
        nvenc_h264_setup s;
        gen_frame(f, raw);
        if (nvenc_h264_pack(&layout, raw, (size_t)W * H * 3 / 2, input.cpu, input.bytes) ||
            nvenc_h264_setup_init(&cfg, &layout, f, &s)) { printf("FAIL: frame %u pack/setup\n", f); bad = 1; break; }
        memcpy(setup.cpu, &s, sizeof s);
        memset(status.cpu, 0, NVENC_SEM_OFFSET);   /* the semaphore keeps its value: fences are f + 1 */
        const nvenc_h264_addresses a = {setup.va, status.va, bits.va, input.va, recon.va, hist.va, coloc.va, scratch.va, mepred.va, SUBCH};
        const uint32_t fence = f + 1;
        const size_t words = nvenc_h264_push(&a, &layout, f, fence, (uint32_t *)push.cpu, push.bytes / 4);
        if (!words) { printf("FAIL: frame %u nvenc_h264_push refused the addresses\n", f); bad = 1; break; }

        const uint64_t t0 = mach_absolute_time();
        uint64_t args[] = {5, push.va, words, 1}, out[3] = {0}; uint32_t count = 3;
        kern_return_t kr = IOConnectCallScalarMethod(conn, 45, args, 4, out, &count);
        if (kr) { printf("FAIL: frame %u submit (45 op 5 engine 1) IOReturn 0x%x, stage %llu\n", f, kr, out[0]); bad = 1; break; }
        double ns = 0;
        while (*sem != fence && ns < (getenv("NVENC_WAIT_MS") ? atof(getenv("NVENC_WAIT_MS")) * 1e6 : 2e9)) {   /* 2 s, as NVVTDecoder */
            ns = ms(mach_absolute_time() - t0) * 1e6;
            if (ns > 50000) usleep(50);
        }
        const double t = ms(mach_absolute_time() - t0);
        __sync_synchronize();
        if (want_status && f == 0) dump_status(status.cpu, f);
        if (*sem != fence) {
            printf("FAIL: frame %u: engine semaphore %08x never reached %08x (2 s)\n", f, *sem, fence);
            if (!want_status || f) dump_status(status.cpu, f);
            bad = 1; break;
        }
        size_t off = 0, n = 0;
        if (nvenc_h264_result(status.cpu, NVENC_STATUS_BYTES, f, bits.cpu, bits.bytes, &off, &n)) {
            printf("FAIL: frame %u: engine finished but reported an error or no IDR slice\n", f);
            if (!want_status || f) dump_status(status.cpu, f);
            bad = 1; break;
        }
        printf("frame %2u: %7zu bytes  %7.3f ms\n", f, n, t);
        fwrite(bits.cpu + off, 1, n, fp);
        frames[f] = malloc(n); sizes[f] = n; memcpy(frames[f], bits.cpu + off, n);
    }
    fclose(fp);
    if (!bad) { printf("wrote %s\n", path); rc = decode_all(hdr, hdr_n, frames, sizes); }
    else rc = 1;
done:
    for (int f = 0; f < FRAMES; ++f) free(frames[f]);
    free(raw);
    if (conn) {
        buf *all[] = {&push, &mepred, &scratch, &status, &setup, &coloc, &hist, &bits, &recon, &input};
        for (unsigned i = 0; i < sizeof all / sizeof all[0]; ++i) free_buf(all[i]);
        if (owned) { uint64_t op = 1, out[3]; uint32_t count = 3; IOConnectCallScalarMethod(conn, 45, &op, 1, out, &count); }
        IOServiceClose(conn);
    }
    printf(rc ? "FAIL\n" : "PASS\n");
    return rc != 0;
}
