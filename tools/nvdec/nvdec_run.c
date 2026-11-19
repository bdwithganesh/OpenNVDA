/* V1 target tool: decode an H.264 elementary stream on NVDEC0 through
 * NVGspControl (>= 0.129.0) and compare each output frame's NV12 MD5 with
 * the x264 reference digests (tests/nvdec_h264/vectors/<name>.md5).
 *
 *   cc -O2 -Idrivers/nvidia-metal-drivers-macos/nvdec tools/nvdec/nvdec_run.c drivers/nvidia-metal-drivers-macos/nvdec/h264_parse.c \
 *      drivers/nvidia-metal-drivers-macos/nvdec/nvdec_h264.c -framework IOKit -framework CoreFoundation -o nvdec_run
 *   sudo ./nvdec_run tests/nvdec_h264/vectors/base_ip [subch] [vram]
 *
 * All buffers are kext memory objects (selector 22) bound in this client's
 * arena (24); the input buffer, push buffer and aux buffers are sysmem
 * mapped into this process, so the setup/bitstream are written and the
 * decoded surfaces read directly by the CPU. One picture in flight: each
 * picture is submitted on engine 2 (NVDEC0, selector 21) and waited on
 * (selector 31) before the input buffer is reused. */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "nvdec_h264.h"

static io_connect_t conn;

static double now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1e6 + tv.tv_usec;
}

static uint8_t *load(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    rewind(f);
    uint8_t *b = malloc(*len + 1);
    if (!b || fread(b, 1, *len, f) != *len) exit(1);
    fclose(f);
    b[*len] = 0;
    return b;
}

typedef struct { uint64_t handle, va, bytes, phys; uint8_t *cpu; int vram; } gpu_buf;

#define GRAN (2ull << 20)

/* domain 0 VRAM (not CPU-mapped here), 1 sysmem (mapped) */
static int buf_alloc(gpu_buf *b, uint64_t bytes, uint64_t va, int domain) {
    bytes = (bytes + GRAN - 1) & ~(GRAN - 1);
    uint64_t in[2] = {bytes, (uint64_t)domain}, out[2] = {0};
    uint32_t cnt = 2;
    if (IOConnectCallMethod(conn, 22, in, 2, NULL, 0, out, &cnt, NULL, NULL)) return -1;
    b->handle = out[0];
    b->va = va;
    b->bytes = bytes;
    b->cpu = NULL;
    b->phys = out[1];
    b->vram = domain == 0;
    uint64_t bi[3] = {b->handle, va, 0};
    if (IOConnectCallMethod(conn, 24, bi, 3, NULL, 0, NULL, NULL, NULL, NULL)) return -1;
    if (domain == 0) {
        b->cpu = malloc(bytes);   /* PRAMIN shadow */
        if (!b->cpu) return -1;
    } else {
        mach_vm_address_t a = 0;
        mach_vm_size_t sz = 0;
        if (IOConnectMapMemory64(conn, 0x1000 | (uint32_t)b->handle, mach_task_self(), &a, &sz,
                                 kIOMapAnywhere)) return -1;
        b->cpu = (uint8_t *)a;
        memset(b->cpu, getenv("NVDEC_FILL") ? atoi(getenv("NVDEC_FILL")) : 0, bytes);
    }
    return 0;
}

/* VRAM object -> CPU copy through PRAMIN (selector 11, absolute phys) */
static int vram_read(uint64_t phys, void *dst, uint32_t bytes) {
    uint32_t *w = dst;
    for (uint32_t n = bytes / 4; n;) {
        const uint32_t c = n > 1024 ? 1024 : n;
        uint64_t in[3] = {phys, c, 0};
        size_t out = c * 4;
        if (IOConnectCallMethod(conn, 11, in, 3, NULL, 0, NULL, NULL, w, &out)) return -1;
        phys += c * 4; w += c; n -= c;
    }
    return 0;
}

static void buf_free(gpu_buf *b) {
    if (!b->handle) return;
    if (b->vram) { free(b->cpu); b->cpu = NULL; }
    if (b->cpu)
        IOConnectUnmapMemory64(conn, 0x1000 | (uint32_t)b->handle, mach_task_self(),
                               (mach_vm_address_t)b->cpu);
    uint64_t h = b->handle;
    IOConnectCallMethod(conn, 23, &h, 1, NULL, 0, NULL, NULL, NULL, NULL);
    b->handle = 0;
}

/* submit `dw` words at `va` on NVDEC0 and wait for the fence */
static int nvdec_exec(uint64_t va, uint32_t dw, double *us) {
    struct { uint64_t va; uint32_t dw, pad; } seg = {va, dw, 0};
    uint64_t eng = 2, seq = 0, done = 0;
    uint32_t cnt = 1;
    const double t0 = now_us();
    kern_return_t kr = IOConnectCallMethod(conn, 21, &eng, 1, &seg, sizeof(seg), &seq, &cnt, NULL, NULL);
    if (kr) { printf("  submit 0x%x\n", kr); return -1; }
    uint64_t wi[3] = {2, seq, 2000000};
    cnt = 1;
    kr = IOConnectCallMethod(conn, 31, wi, 3, NULL, 0, &done, &cnt, NULL, NULL);
    *us = now_us() - t0;
    if (kr || done != seq) { printf("  wait 0x%x (seq %llu done %llu)\n", kr, seq, done); return -1; }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <vector path without extension> [subch]\n", argv[0]);
        return 2;
    }
    const uint32_t subch = argc > 2 ? (uint32_t)atoi(argv[2]) : 4;
    char path[1024];
    size_t len = 0, mlen = 0;
    snprintf(path, sizeof(path), "%s.h264", argv[1]);
    uint8_t *bs = load(path, &len);
    snprintf(path, sizeof(path), "%s.md5", argv[1]);
    char *mtxt = (char *)load(path, &mlen);
    char *want[512];
    int nwant = 0;
    for (char *t = strtok(mtxt, "\n"); t && nwant < 512; t = strtok(NULL, "\n")) want[nwant++] = t;

    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVGspControl"));
    if (!s || IOServiceOpen(s, mach_task_self(), 0, &conn)) { printf("open NVGspControl failed\n"); return 1; }
    IOObjectRelease(s);
    uint64_t vin = 0, vout[2] = {0};
    uint32_t cnt = 2;
    kern_return_t kr = IOConnectCallMethod(conn, 30, &vin, 1, NULL, 0, vout, &cnt, NULL, NULL);
    if (kr) { printf("NVDEC0 open: kr 0x%x stage %llu status 0x%llx\n", kr, vout[0], vout[1]); return 1; }

    gpu_buf in = {0}, pb = {0}, aux = {0}, surf = {0};
    if (buf_alloc(&in, 4ull << 20, 0x2B00000000ull, 1) || buf_alloc(&pb, GRAN, 0x2B40000000ull, 1)) {
        printf("buffer alloc failed\n");
        return 1;
    }

    static nvdec_h264_dec d;
    nvdec_h264_init(&d);
    nvdec_h264_stream st;
    nvdec_h264_pic_s setup;
    h264_slice prev, sl;
    nvdec_h264_layout lay;
    memset(&lay, 0, sizeof(lay));
    nvdec_h264_addrs a;
    memset(&a, 0, sizeof(a));
    uint64_t surf_stride = 0;
    int have_prev = 0, pics = 0, cur = -1, n_out = 0, bad = 0, errors = 0;
    double busy_us = 0;
    size_t pos = 0;
    h264_nal nal;
    const nvdec_status_s *status = (const nvdec_status_s *)(in.cpu + NVDEC_IN_STATUS);

#define DRAIN() do { \
        for (int i_ = 0; i_ < d.n_out; ++i_) { \
            char hex[33] = "?"; \
            if (surf.vram && vram_read(surf.phys + (uint64_t)d.out_surface[i_] * surf_stride, \
                                       surf.cpu + (uint64_t)d.out_surface[i_] * surf_stride, \
                                       lay.surface_bytes)) printf("  PRAMIN read failed\n"); \
            nvdec_h264_surface_md5(surf.cpu + (uint64_t)d.out_surface[i_] * surf_stride, &lay, \
                                   &d.sps[d.sps_id], hex); \
            const int ok_ = n_out < nwant && !strcmp(hex, want[n_out]); \
            if (n_out == 0 && getenv("NVDEC_DUMP")) { \
                FILE *f_ = fopen(getenv("NVDEC_DUMP"), "wb"); \
                if (f_) { fwrite(surf.cpu + (uint64_t)d.out_surface[i_] * surf_stride, 1, \
                                 lay.surface_bytes, f_); fclose(f_); } \
            } \
            if (!ok_ && bad < 8) \
                printf("  frame %d (surface %d): md5 %s want %s\n", n_out, d.out_surface[i_], hex, \
                       n_out < nwant ? want[n_out] : "-"); \
            bad += !ok_; \
            ++n_out; \
        } \
        d.n_out = 0; \
    } while (0)

    for (;;) {
        const int more = h264_next_nal(bs, len, &pos, &nal);
        const int is_slice = more && (nal.type == H264_NAL_SLICE || nal.type == H264_NAL_IDR);
        int newpic = 0;
        if (is_slice) {
            if (h264_parse_slice(nal.data, nal.size, d.sps, d.pps, &sl)) { printf("slice parse failed\n"); return 1; }
            newpic = nvdec_h264_new_picture(&d, have_prev ? &prev : NULL, &sl);
        }
        if (have_prev && (!more || newpic || (!is_slice && nal.type <= H264_NAL_AUD &&
                                              nal.type >= H264_NAL_SEI))) {
            if (nvdec_h264_stream_end(&st, &setup)) { printf("stream end failed\n"); return 1; }
            {   /* layout experiments: override the setup copy in the input buffer */
                nvdec_h264_pic_s *cp = (nvdec_h264_pic_s *)(in.cpu + NVDEC_IN_SETUP);
                if (getenv("NVDEC_TILE")) cp->tileFormat = atoi(getenv("NVDEC_TILE"));
                if (getenv("NVDEC_GOBH")) cp->gob_height = atoi(getenv("NVDEC_GOBH"));
            }
            uint32_t *push = (uint32_t *)pb.cpu;
            const uint32_t n = nvdec_h264_push(&a, &lay, d.pictures, d.pictures + 1, push, 1024);
            if (!n) { printf("push build failed\n"); return 1; }
            double us = 0;
            if (nvdec_exec(pb.va, n, &us)) {
                printf("picture %d: NVDEC submit/wait failed\n", pics);
                ++errors;
                break;
            }
            busy_us += us;
            const uint32_t sem = *(const uint32_t *)(in.cpu + NVDEC_IN_SEM);
            if (pics < 2 || sem != d.pictures + 1)
                printf("  picture %d surface %d: %.0f us, engine sem %u (want %u), status word0 0x%08x\n",
                       pics, cur, us, sem, d.pictures + 1, *(const uint32_t *)status);
            nvdec_h264_end(&d);
            DRAIN();
            have_prev = 0;
            ++pics;
        }
        if (!more) break;
        if (!is_slice) {
            if (nvdec_h264_param_nal(&d, &nal)) { printf("param nal %d failed\n", nal.type); return 1; }
            continue;
        }
        if (!have_prev) {
            if (nvdec_h264_begin(&d, &sl, &setup, &cur)) { printf("begin failed\n"); return 1; }
            DRAIN();
            const h264_sps *sp = &d.sps[d.pps[sl.pps_id].sps_id];
            nvdec_h264_layout lnew;
            nvdec_h264_layout_for(sp, &lnew);
            if (!surf.handle || memcmp(&lnew, &lay, sizeof(lay))) {
                buf_free(&aux);
                buf_free(&surf);
                lay = lnew;
                const uint64_t c = (lay.coloc_bytes + 0xffff) & ~0xffffull;
                const uint64_t m = (lay.mbhist_bytes + 0xffff) & ~0xffffull;
                const uint64_t h = (lay.history_bytes + 0xffff) & ~0xffffull;
                surf_stride = (lay.surface_bytes + 0xffff) & ~0xffffull;
                if (buf_alloc(&aux, c + m + h, 0x2B80000000ull, 1) ||
                    buf_alloc(&surf, surf_stride * NVDEC_H264_SURFACES, 0x2C00000000ull,
                              getenv("NVDEC_SURF_DOMAIN") ? atoi(getenv("NVDEC_SURF_DOMAIN")) : 1)) {
                    printf("surface alloc failed\n");
                    return 1;
                }
                a.in_va = in.va;
                a.coloc_va = aux.va;
                a.mbhist_va = aux.va + c;
                a.history_va = aux.va + c + m;
                for (int i = 0; i < NVDEC_H264_SURFACES; ++i)
                    a.surface_va[i] = surf.va + (uint64_t)i * surf_stride;
                a.subch = subch;
                a.obj_class = 0xc9b0;
                printf("%s: %ux%u, surface %u B (pitch %u), coloc %u mbhist %u history %u\n", argv[1],
                       lay.width, lay.height, lay.surface_bytes, lay.pitch, lay.coloc_bytes,
                       lay.mbhist_bytes, lay.history_bytes);
            }
            nvdec_h264_stream_begin(&st, in.cpu, (uint32_t)in.bytes);
        }
        if (nvdec_h264_stream_slice(&st, &nal)) { printf("slice add failed\n"); return 1; }
        prev = sl;
        have_prev = 1;
    }
    if (!errors) {
        nvdec_h264_flush(&d);
        DRAIN();
    }
    const int pass = !errors && !bad && n_out == nwant;
    printf("nvdec %s: %d pictures, %d frames out (%d expected), %d md5 mismatches, %.2f ms/picture: %s\n",
           argv[1], pics, n_out, nwant, bad, pics ? busy_us / pics / 1e3 : 0.0, pass ? "PASS" : "FAIL");
    buf_free(&surf);
    buf_free(&aux);
    buf_free(&pb);
    buf_free(&in);
    IOServiceClose(conn);
    return !pass;
}
