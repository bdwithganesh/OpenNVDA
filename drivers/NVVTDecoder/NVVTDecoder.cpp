#include "NVVTDecoder.h"
#include "spi/VTVideoDecoderSPI.h"
#include <IOKit/IOKitLib.h>
#include <IOSurface/IOSurfaceRef.h>
#include <mach/mach_time.h>
#include <atomic>
#include <mutex>
#include <new>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <os/log.h>
extern "C" {
#include "nvdec_h264.h"
#include "nvdec_hevc.h"
}

namespace {
// Measured 2026-11-20: NVDEC writes HEVC Main10 samples in the upper 10 bits (P010), so 0 and the CE detile.
// 6 would mean low bits: CPU detile with a left shift (the CE cannot shift).
constexpr int kHevc10Shift = 0;
std::atomic<uint64_t> sessions{0}, submitted{0}, emitted{0}, errors{0}, gpuNS{0};
struct Buffer { uint64_t handle = 0, va = 0, bytes = 0; uint8_t *cpu = nullptr; };
struct Decoder {
    std::mutex mutex;
    io_connect_t conn = IO_OBJECT_NULL;
    VTVideoDecoderSession session = nullptr;
    nvdec_h264_dec h264;
    nvdec_h264_layout layout{};
    nvdec_h264_addrs addrs{};
    bool hevc = false;   // HEVC Main / Main10: same buffers and submit as H.264
    int depth = 8, bps = 1;   // 0.3.3: sample bit depth, bytes per sample in the NVDEC surface and pixel buffer
    nvdec_hevc_dec hdec;
    nvdec_hevc_layout hlayout{};
    nvdec_hevc_addrs haddrs{};
    Buffer input, push, aux, surfaces;
    uint64_t stride = 0;
    int lengthBytes = 0, width = 0, height = 0, x0 = 0, y0 = 0;
    bool failed = false;
    bool cpuOnly = [] { const char *e = getenv("NVVT_CPU_DETILE"); return e && e[0] == '1'; }();   // 0.3.2
    uint64_t ceFrames = 0, cpuFrames = 0;
    std::vector<uint8_t> linear;
    Decoder() { nvdec_h264_init(&h264); nvdec_hevc_init(&hdec); }
    // native615 (NVGspControl >= 0.180.191): an owned user-buffer client (selector 45 op 3),
    // buffers = selector-47 sysmem allocations at kernel VAs (handle = slot + 1), NVDEC0 open =
    // 45 op 4, submit = 45 op 5 {va, words}, fence = the engine semaphore the push releases.
    bool owned = false;
    void release(Buffer &b) {
        if (b.cpu) IOConnectUnmapMemory64(conn, 0x80615001u + (uint32_t)(b.handle - 1),
            mach_task_self(), (mach_vm_address_t)b.cpu);
        if (b.handle) {
            uint64_t args[] = {1, b.handle - 1}, out[3]; uint32_t count = 3;
            IOConnectCallScalarMethod(conn, 47, args, 2, out, &count);
        }
        b = {};
    }
    ~Decoder() {
        if (conn) {
            release(surfaces); release(aux); release(push); release(input);
            if (owned) { uint64_t op = 1, out[3]; uint32_t count = 3; IOConnectCallScalarMethod(conn, 45, &op, 1, out, &count); }
            IOServiceClose(conn);
        }
    }
    bool allocate(Buffer &b, uint64_t bytes, uint64_t) {
        b.bytes = (bytes + 0xffff) & ~0xffffull;
        uint64_t args[] = {0, b.bytes}, out[3]{};
        uint32_t count = 3;
        if (IOConnectCallScalarMethod(conn, 47, args, 2, out, &count) || out[2] != b.bytes) return false;
        b.handle = out[0] + 1; b.va = out[1];
        mach_vm_address_t address = 0; mach_vm_size_t size = 0;
        if (IOConnectMapMemory64(conn, 0x80615001u + (uint32_t)out[0], mach_task_self(),
                                &address, &size, kIOMapAnywhere)) return false;
        b.cpu = reinterpret_cast<uint8_t *>(address);
        if (size < b.bytes) return false;
        memset(b.cpu, 0, b.bytes);
        return true;
    }
    bool open() {
        // the accelerator's 'NVGP' client first: VTDecoderXPCService's
        // sandbox allows accelerator clients, not NVGspControl's own
        kern_return_t kr = KERN_FAILURE;
        io_service_t acc = IOServiceGetMatchingService(kIOMainPortDefault,
                                               IOServiceMatching("NVAccelerator"));
        if (acc) { kr = IOServiceOpen(acc, mach_task_self(), 0x4E564750, &conn); IOObjectRelease(acc); }
        if (kr) {
            io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault,
                                                   IOServiceMatching("NVGspControl"));
            if (!service) return false;
            kr = IOServiceOpen(service, mach_task_self(), 0, &conn);
            IOObjectRelease(service);
        }
        if (kr) return false;
        uint64_t op = 3, out[3]{}; uint32_t count = 3;
        kr = IOConnectCallScalarMethod(conn, 45, &op, 1, out, &count);
        for (int i = 0; kr == kIOReturnBusy && i < 500; i++) {   // all client slots taken: wait, like NVMTL
            usleep(20000); count = 3; kr = IOConnectCallScalarMethod(conn, 45, &op, 1, out, &count);
        }
        if (kr) return false;
        owned = true;
        if (!allocate(input, 4ull << 20, 0) || !allocate(push, 64ull << 10, 0)) return false;
        op = 4; count = 3;
        return IOConnectCallScalarMethod(conn, 45, &op, 1, out, &count) == KERN_SUCCESS;
    }
    bool configure(const h264_sps &s) {
        nvdec_h264_layout_for(&s, &layout);
        const uint64_t c = (layout.coloc_bytes + 0xffffull) & ~0xffffull;
        const uint64_t m = (layout.mbhist_bytes + 0xffffull) & ~0xffffull;
        const uint64_t h = (layout.history_bytes + 0xffffull) & ~0xffffull;
        stride = (layout.surface_bytes + 0xffffull) & ~0xffffull;
        if (!allocate(aux, c + m + h, 0x2b80000000ull) ||
            !allocate(surfaces, stride * NVDEC_H264_SURFACES, 0x2c00000000ull)) return false;
        addrs.in_va = input.va; addrs.coloc_va = aux.va;
        addrs.mbhist_va = aux.va + c; addrs.history_va = aux.va + c + m;
        addrs.subch = 4; addrs.obj_class = 0xc9b0;
        for (int i = 0; i < NVDEC_H264_SURFACES; ++i)
            addrs.surface_va[i] = surfaces.va + stride * i;
        linear.resize((size_t)layout.width * layout.height * 3 / 2);
        return true;
    }
    bool configureHevc(const hevc_sps &s) {
        nvdec_hevc_layout_for(&s, &hlayout);
        const uint64_t c = (hlayout.coloc_bytes + 0xffffull) & ~0xffffull;
        const uint64_t f = (hlayout.filter_bytes + 0xffffull) & ~0xffffull;
        stride = (hlayout.surface_bytes + 0xffffull) & ~0xffffull;
        if (!allocate(aux, c + f + NVDEC_HEVC_MISC_BYTES, 0) ||
            !allocate(surfaces, stride * NVDEC_HEVC_SURFACES, 0)) return false;
        haddrs.in_va = input.va; haddrs.coloc_va = aux.va; haddrs.filter_va = aux.va + c;
        haddrs.misc_va = aux.va + c + f; haddrs.subch = 4; haddrs.obj_class = 0xc9b0;
        for (int i = 0; i < NVDEC_HEVC_SURFACES; ++i) haddrs.surface_va[i] = surfaces.va + stride * i;
        bps = (int)hlayout.bytes_per_sample;
        linear.resize((size_t)hlayout.width * bps * hlayout.height * 3 / 2);
        return true;
    }
    // 0.3.2: NVGspControl >= 0.180.204 selector 45 op 6: the copy engine detiles one NVDEC surface window
    // (base, stride bytes) into the locked pixel buffer's planes. The IOSurface ID tells the kernel which
    // destination memory its cached wiring of this VA belongs to. Any error: the caller detiles on the CPU.
    bool ceDetile(CVPixelBufferRef pixel, const uint8_t *base, uint32_t blPitch, uint32_t lumaBytes) {
        IOSurfaceRef io = CVPixelBufferGetIOSurface(pixel);
        if (!io) return false;
        uint64_t args[] = {6, (uint64_t)(uintptr_t)base, stride, lumaBytes, blPitch, (uint64_t)x0 * bps, (uint64_t)y0,
            (uint64_t)width * bps, (uint64_t)height,
            (uint64_t)(uintptr_t)CVPixelBufferGetBaseAddressOfPlane(pixel, 0), CVPixelBufferGetBytesPerRowOfPlane(pixel, 0),
            (uint64_t)(uintptr_t)CVPixelBufferGetBaseAddressOfPlane(pixel, 1), CVPixelBufferGetBytesPerRowOfPlane(pixel, 1),
            IOSurfaceGetID(io)}, out[3]; uint32_t count = 3;
        return IOConnectCallScalarMethod(conn, 45, args, 14, out, &count) == KERN_SUCCESS;
    }
    OSStatus execute(uint32_t words, uint32_t want) {
        uint64_t args[] = {5, push.va, words}, out[3]; uint32_t count = 3;
        const uint64_t start = mach_absolute_time();
        auto kr = IOConnectCallScalarMethod(conn, 45, args, 3, out, &count);
        if (kr) return kVTVideoDecoderMalfunctionErr;
        ++submitted;
        mach_timebase_info_data_t tb; mach_timebase_info(&tb);
        const volatile uint32_t *fence = (volatile uint32_t *)(input.cpu + NVDEC_IN_SEM);
        uint64_t ns = 0;
        while (*fence != want && ns < 2000000000ull) {   // 2 s, as the old kernel wait
            ns = (mach_absolute_time() - start) * tb.numer / tb.denom;
            if (ns > 50000) usleep(50);
        }
        gpuNS += (mach_absolute_time() - start) * tb.numer / tb.denom;
        if (*fence != want) return kVTVideoDecoderMalfunctionErr;
        return noErr;
    }
};
struct Storage { Decoder *decoder; };
Storage *storage(CMBaseObjectRef object) {
    return static_cast<Storage *>(CMBaseObjectGetDerivedStorage(object));
}
OSStatus invalidate(CMBaseObjectRef object) {
    Storage *s = storage(object);
    if (s->decoder && s->decoder->ceFrames + s->decoder->cpuFrames)
        os_log(OS_LOG_DEFAULT, "NVVTDecoder: detile frames CE %llu CPU %llu in %{public}s",
               s->decoder->ceFrames, s->decoder->cpuFrames, getprogname());
    delete s->decoder; s->decoder = nullptr;
    return noErr;
}
void finalize(CMBaseObjectRef object) { invalidate(object); }
CFStringRef describe(CMBaseObjectRef) { return CFSTR("NVDEC H.264/HEVC on RTX 4080"); }
OSStatus copyProperty(CMBaseObjectRef, CFStringRef key, CFAllocatorRef, void *out) {
    if (!out) return kVTParameterErr;
    if (CFEqual(key, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder)) {
        *static_cast<CFTypeRef *>(out) = CFRetain(kCFBooleanTrue);
        return noErr;
    }
    return kCMBaseObjectError_ValueNotAvailable;
}

OSStatus start(VTVideoDecoderRef instance, VTVideoDecoderSession session,
               CMVideoFormatDescriptionRef format) {
    Storage *st = storage(reinterpret_cast<CMBaseObjectRef>(instance));
    const FourCharCode codec = CMFormatDescriptionGetMediaSubType(format);
    if (st->decoder || (codec != kCMVideoCodecType_H264 && codec != kCMVideoCodecType_HEVC))
        return kVTVideoDecoderUnsupportedDataFormatErr;
    Decoder *d = new (std::nothrow) Decoder;
    if (!d) return kVTAllocationFailedErr;
    st->decoder = d; d->session = session; d->hevc = codec == kCMVideoCodecType_HEVC;
    auto paramSet = [&](size_t i, const uint8_t **p, size_t *n, size_t *count, int *length) {
        return d->hevc ? CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(format, i, p, n, count, length)
                       : CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, i, p, n, count, length);
    };
    size_t nsets = 0, size = 0; const uint8_t *data = nullptr;
    OSStatus err = paramSet(0, &data, &size, &nsets, &d->lengthBytes);
    if (err || nsets < 1 || nsets > 32 ||
        (d->lengthBytes != 1 && d->lengthBytes != 2 && d->lengthBytes != 4))
        return kVTVideoDecoderBadDataErr;
    for (size_t i = 0; i < nsets; ++i) {
        err = paramSet(i, &data, &size, nullptr, nullptr);
        if (err || !size) return kVTVideoDecoderBadDataErr;
        if (d->hevc) {
            if (size < 2 || nvdec_hevc_param_nal(&d->hdec, data, size)) return kVTVideoDecoderBadDataErr;
            continue;
        }
        h264_nal nal{data, size, data[0] & 31, (data[0] >> 5) & 3};
        if (nvdec_h264_param_nal(&d->h264, &nal)) return kVTVideoDecoderBadDataErr;
    }
    // 0.3.1: refuse what NVDEC is not set up for here, at session start, so VT picks the software decoder
    // (failing on the first frame instead made QuickTime stall on Main10 HEVC). 0.3.3: 4:2:0 8-bit, and
    // HEVC Main10 (4:2:0 10-bit); all SPS must agree. Returns the bit depth, 0 = refuse.
    auto depthOf = [](const auto *sps, int n, auto one) {
        int valid = 0, depth = 0;
        for (int i = 0; i < n; ++i) {
            if (!sps[i].valid) continue;
            const int v = one(sps[i]);
            if (!v || (valid++ && v != depth)) return 0;
            depth = v;
        }
        return depth;
    };
    d->depth = d->hevc ? depthOf(d->hdec.sps, HEVC_MAX_SPS, [](const hevc_sps &s) { return nvdec_hevc_sps_depth(&s); })
                       : depthOf(d->h264.sps, H264_MAX_SPS, [](const h264_sps &s) {
                             return s.chroma_format_idc == 1 && s.bit_depth_luma == 8 && s.bit_depth_chroma == 8 ? 8 : 0; });
    if (!d->depth) {
        os_log(OS_LOG_DEFAULT, "NVVTDecoder: format not supported by NVDEC path (needs 4:2:0 8-bit or HEVC Main10), software decode");
        return kVTVideoDecoderUnsupportedDataFormatErr;
    }
    const auto dim = CMVideoFormatDescriptionGetDimensions(format);
    d->width = dim.width; d->height = dim.height;
    if (d->width <= 0 || d->height <= 0 || d->width > 4096 || d->height > 2304)
        return kVTVideoDecoderUnsupportedDataFormatErr;
    int formatCode = d->depth == 10 ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange
                                    : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    CFNumberRef w = CFNumberCreate(nullptr, kCFNumberIntType, &d->width);
    CFNumberRef h = CFNumberCreate(nullptr, kCFNumberIntType, &d->height);
    CFNumberRef f = CFNumberCreate(nullptr, kCFNumberIntType, &formatCode);
    CFDictionaryRef empty = CFDictionaryCreate(nullptr, nullptr, nullptr, 0,
                                     &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const void *keys[] = {kCVPixelBufferWidthKey, kCVPixelBufferHeightKey,
        kCVPixelBufferPixelFormatTypeKey, kCVPixelBufferIOSurfacePropertiesKey};
    const void *values[] = {w, h, f, empty};
    CFDictionaryRef attrs = CFDictionaryCreate(nullptr, keys, values, 4,
                                     &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    err = VTDecoderSessionSetPixelBufferAttributes(session, attrs);
    CFRelease(attrs); CFRelease(empty); CFRelease(f); CFRelease(h); CFRelease(w);
    if (err) return err;
    if (!d->open()) {
        os_log(OS_LOG_DEFAULT, "NVVTDecoder: NVDEC open failed in %{public}s", getprogname());
        return kVTVideoDecoderNotAvailableNowErr;
    }
    ++sessions;
    os_log(OS_LOG_DEFAULT, "NVVTDecoder: NVDEC %{public}s session in %{public}s", d->hevc ? "HEVC" : "H.264", getprogname());
    return noErr;
}

// Block-linear NV12 surface -> pool buffer, cropped at x0/y0 to width x height.
OSStatus emitSurface(Decoder *d, VTVideoDecoderFrame frame, const uint8_t *base, uint32_t blPitch,
                     uint32_t codedWidth, uint32_t lumaBytes, uint32_t codedHeight) {
    CVPixelBufferPoolRef pool = VTDecoderSessionGetPixelBufferPool(d->session);
    CVPixelBufferRef pixel = nullptr;
    if (!pool || CVPixelBufferPoolCreatePixelBuffer(nullptr, pool, &pixel)) return kVTAllocationFailedErr;
    if (CVPixelBufferGetPixelFormatType(pixel) != (d->depth == 10 ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange
                                                                  : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) ||
        CVPixelBufferGetWidth(pixel) != (size_t)d->width || CVPixelBufferGetHeight(pixel) != (size_t)d->height ||
        CVPixelBufferGetPlaneCount(pixel) != 2 || CVPixelBufferLockBaseAddress(pixel, 0)) {
        CFRelease(pixel); return kVTVideoDecoderMalfunctionErr;
    }
    const bool ce = !d->cpuOnly && !(d->bps == 2 && kHevc10Shift) && d->ceDetile(pixel, base, blPitch, lumaBytes);
    if (ce) ++d->ceFrames; else ++d->cpuFrames;
    for (int plane = 0; plane < 2 && !ce; ++plane) {
        uint8_t *dst = static_cast<uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(pixel, plane));
        const size_t pitch = CVPixelBufferGetBytesPerRowOfPlane(pixel, plane);
        // 0.3.1: straight into the pixel buffer (8 ms -> see LOG); the staged path stays for odd crops
        // 0.3.3: widths and x0 in bytes (x2 for 10-bit); the interleaved chroma row is as many bytes as luma
        const uint32_t rowBytes = codedWidth * d->bps, cropBytes = (uint32_t)d->width * d->bps, x0Bytes = (uint32_t)d->x0 * d->bps;
        if (nvdec_detile_crop(base + (plane ? lumaBytes : 0), blPitch, dst, (uint32_t)pitch, x0Bytes, d->y0 >> plane,
                              cropBytes, d->height >> plane)) {
            nvdec_detile(base + (plane ? lumaBytes : 0), blPitch, d->linear.data(), rowBytes, rowBytes,
                         codedHeight >> plane);
            for (int y = 0; y < (d->height >> plane); ++y)
                memcpy(dst + y * pitch, d->linear.data() + (size_t)(y + (d->y0 >> plane)) * rowBytes + x0Bytes, cropBytes);
        }
        if (d->bps == 2 && kHevc10Shift)
            for (int y = 0; y < (d->height >> plane); ++y) {
                uint16_t *row = reinterpret_cast<uint16_t *>(dst + y * pitch);
                for (int x = 0; x < d->width; ++x) row[x] = (uint16_t)(row[x] << kHevc10Shift);
            }
    }
    CVPixelBufferUnlockBaseAddress(pixel, 0);
    const OSStatus err = VTDecoderSessionEmitDecodedFrame(d->session, frame, noErr, 0, pixel);
    CFRelease(pixel);
    if (!err) ++emitted;
    return err;
}

// One HEVC access unit (length-prefixed NALs = one picture); emitted in decode order like H.264.
template <class Fail>
OSStatus decodeHevc(Decoder *d, VTVideoDecoderFrame frame, std::vector<uint8_t> &data, VTDecodeFrameFlags flags,
                    Fail fail) {
    const size_t bytes = data.size();
    std::vector<h264_nal> slices;
    hevc_slice first{};
    for (size_t pos = 0; pos < bytes;) {
        if (bytes - pos < (size_t)d->lengthBytes) return fail(kVTVideoDecoderBadDataErr);
        uint32_t length = 0;
        for (int i = 0; i < d->lengthBytes; ++i) length = (length << 8) | data[pos++];
        if (length < 2 || length > bytes - pos) return fail(kVTVideoDecoderBadDataErr);
        const uint8_t *nal = data.data() + pos;
        const int t = hevc_nal_type(nal);
        if (hevc_is_slice(t)) {
            hevc_slice sl;
            if (hevc_parse_slice(nal, length, d->hdec.sps, d->hdec.pps, &sl)) return fail(kVTVideoDecoderBadDataErr);
            if (slices.empty() != (bool)sl.first_slice_segment_in_pic_flag) return fail(kVTVideoDecoderBadDataErr);
            if (slices.empty()) first = sl;
            slices.push_back(h264_nal{nal, length, t, 0});
        } else if (t == HEVC_NAL_VPS || t == HEVC_NAL_SPS || t == HEVC_NAL_PPS || t == HEVC_NAL_EOS) {
            // in-band parameter sets; a changed layout is refused below
            if (nvdec_hevc_param_nal(&d->hdec, nal, length)) return fail(kVTVideoDecoderBadDataErr);
        }
        pos += length;
    }
    if (slices.empty()) return fail(kVTVideoDecoderBadDataErr);
    nvdec_hevc_pic_s setup;
    int surface = -1;
    const int r = nvdec_hevc_begin(&d->hdec, &first, d->input.cpu, &setup, &surface);
    d->hdec.n_out = 0;   // VT owns timestamps
    if (r == NVDEC_HEVC_SKIP)   // RASL of the CRA decoding starts at: not decodable
        return VTDecoderSessionEmitDecodedFrame(d->session, frame, noErr, kVTDecodeInfo_FrameDropped, nullptr);
    if (r) return fail(r == NVDEC_HEVC_EFORMAT ? kVTVideoDecoderUnsupportedDataFormatErr : kVTVideoDecoderBadDataErr);
    const hevc_sps &s = d->hdec.sps[d->hdec.sps_id];
    if (nvdec_hevc_sps_depth(&s) != d->depth) return fail(kVTVideoDecoderUnsupportedDataFormatErr);
    if (s.width > 4096 || s.height > 2304 || s.width - s.conf_left - s.conf_right != d->width ||
        s.height - s.conf_top - s.conf_bottom != d->height || ((d->width | d->height | s.conf_left | s.conf_top) & 1))
        return fail(kVTVideoDecoderUnsupportedDataFormatErr);
    if (!d->surfaces.handle) {
        d->x0 = s.conf_left; d->y0 = s.conf_top;
        if (!d->configureHevc(s)) return fail(kVTAllocationFailedErr);
    } else {
        nvdec_hevc_layout current; nvdec_hevc_layout_for(&s, &current);
        if (memcmp(&current, &d->hlayout, sizeof(current)) || d->x0 != s.conf_left || d->y0 != s.conf_top)
            return fail(kVTVideoDecoderUnsupportedDataFormatErr);
    }
    nvdec_h264_stream stream;
    nvdec_h264_stream_begin(&stream, d->input.cpu, (uint32_t)d->input.bytes);
    for (const auto &nal : slices)
        if (nvdec_h264_stream_slice(&stream, &nal)) return fail(kVTVideoDecoderBadDataErr);
    if (nvdec_hevc_stream_end(&stream, &setup)) return fail(kVTVideoDecoderBadDataErr);
    const uint32_t want = d->hdec.pictures + 1;
    const uint32_t words = nvdec_hevc_push(&d->haddrs, &d->hlayout, d->hdec.pictures, want,
                                           reinterpret_cast<uint32_t *>(d->push.cpu), 1024);
    if (!words) return fail(kVTVideoDecoderBadDataErr);
    OSStatus err = d->execute(words, want);
    if (err) return fail(err);
    d->hdec.pictures++;
    const bool shown = d->hdec.cur_output;
    nvdec_hevc_end(&d->hdec); d->hdec.n_out = 0;
    if ((flags & kVTDecodeFrame_DoNotOutputFrame) || !shown)
        return VTDecoderSessionEmitDecodedFrame(d->session, frame, noErr, kVTDecodeInfo_FrameDropped, nullptr);
    err = emitSurface(d, frame, d->surfaces.cpu + surface * d->stride, d->hlayout.pitch, d->hlayout.width,
                      d->hlayout.luma_bytes, d->hlayout.height);
    return err ? fail(err) : noErr;
}

OSStatus decode(VTVideoDecoderRef instance, VTVideoDecoderFrame frame,
                CMSampleBufferRef sample, VTDecodeFrameFlags flags, VTDecodeInfoFlags *info) {
    Decoder *d = storage(reinterpret_cast<CMBaseObjectRef>(instance))->decoder;
    if (info) *info = 0;
    if (!d) return kVTVideoDecoderMalfunctionErr;
    std::lock_guard<std::mutex> guard(d->mutex);
    if (d->failed) return kVTVideoDecoderMalfunctionErr;
    auto fail = [&](OSStatus e) {
        ++errors; d->failed = true;
        os_log_error(OS_LOG_DEFAULT, "NVVTDecoder: decode failed %d after %u pictures (%{public}s) in %{public}s", (int)e,
                     d->hevc ? d->hdec.pictures : 0u, d->hevc ? "HEVC" : "H.264", getprogname());
        return e;
    };
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
    if (!block || CMSampleBufferGetNumSamples(sample) != 1) return fail(kVTVideoDecoderBadDataErr);
    const size_t bytes = CMBlockBufferGetDataLength(block);
    if (!bytes || bytes > d->input.bytes - NVDEC_IN_BITSTREAM - 32)
        return fail(kVTVideoDecoderBadDataErr);
    std::vector<uint8_t> data(bytes);
    if (CMBlockBufferCopyDataBytes(block, 0, bytes, data.data())) return fail(kVTVideoDecoderBadDataErr);
    if (d->hevc) return decodeHevc(d, frame, data, flags, fail);
    std::vector<h264_nal> slices;
    h264_slice first{}, prev{};
    for (size_t pos = 0; pos < bytes;) {
        if (bytes - pos < (size_t)d->lengthBytes) return fail(kVTVideoDecoderBadDataErr);
        uint32_t length = 0;
        for (int i = 0; i < d->lengthBytes; ++i) length = (length << 8) | data[pos++];
        if (!length || length > bytes - pos) return fail(kVTVideoDecoderBadDataErr);
        h264_nal nal{data.data() + pos, length, data[pos] & 31, (data[pos] >> 5) & 3};
        if (nal.type == H264_NAL_SLICE || nal.type == H264_NAL_IDR) {
            h264_slice sl;
            if (h264_parse_slice(nal.data, nal.size, d->h264.sps, d->h264.pps, &sl) ||
                sl.field_pic_flag) return fail(kVTVideoDecoderUnsupportedDataFormatErr);
            if (slices.empty()) first = sl;
            else if (nvdec_h264_new_picture(&d->h264, &prev, &sl)) return fail(kVTVideoDecoderBadDataErr);
            slices.push_back(nal); prev = sl;
        } else if (nal.type == H264_NAL_SPS || nal.type == H264_NAL_PPS) {
            /* Parameter changes need a fresh session; do not mutate a live DPB. */
            return fail(kVTVideoDecoderUnsupportedDataFormatErr);
        }
        pos += length;
    }
    if (slices.empty()) return fail(kVTVideoDecoderBadDataErr);
    if (first.pps_id < 0 || first.pps_id >= H264_MAX_PPS ||
        !d->h264.pps[first.pps_id].valid ||
        d->h264.pps[first.pps_id].sps_id < 0 ||
        d->h264.pps[first.pps_id].sps_id >= H264_MAX_SPS ||
        !d->h264.sps[d->h264.pps[first.pps_id].sps_id].valid)
        return fail(kVTVideoDecoderBadDataErr);
    const h264_sps &s = d->h264.sps[d->h264.pps[first.pps_id].sps_id];
    if (s.chroma_format_idc != 1 || s.bit_depth_luma != 8 || s.bit_depth_chroma != 8 ||
        s.width > 4096 || s.height > 2304 || s.crop_left < 0 || s.crop_top < 0 ||
        s.width - s.crop_left - s.crop_right != d->width ||
        s.height - s.crop_top - s.crop_bottom != d->height ||
        ((d->width | d->height | s.crop_left | s.crop_top) & 1))
        return fail(kVTVideoDecoderUnsupportedDataFormatErr);
    if (!d->surfaces.handle) {
        d->x0 = s.crop_left; d->y0 = s.crop_top;
        if (!d->configure(s)) return fail(kVTAllocationFailedErr);
    } else {
        nvdec_h264_layout current; nvdec_h264_layout_for(&s, &current);
        if (memcmp(&current, &d->layout, sizeof(current)) || d->x0 != s.crop_left || d->y0 != s.crop_top)
            return fail(kVTVideoDecoderUnsupportedDataFormatErr);
    }
    nvdec_h264_pic_s setup;
    int surface = -1;
    if (nvdec_h264_begin(&d->h264, &first, &setup, &surface)) return fail(kVTVideoDecoderBadDataErr);
    d->h264.n_out = 0; // VT owns timestamps; frames are returned in decode order.
    nvdec_h264_stream stream;
    nvdec_h264_stream_begin(&stream, d->input.cpu, (uint32_t)d->input.bytes);
    for (const auto &nal : slices)
        if (nvdec_h264_stream_slice(&stream, &nal)) return fail(kVTVideoDecoderBadDataErr);
    if (nvdec_h264_stream_end(&stream, &setup)) return fail(kVTVideoDecoderBadDataErr);
    const uint32_t words = nvdec_h264_push(&d->addrs, &d->layout, d->h264.pictures,
        d->h264.pictures + 1, reinterpret_cast<uint32_t *>(d->push.cpu), 1024);
    if (!words) return fail(kVTVideoDecoderBadDataErr);
    OSStatus err = d->execute(words, d->h264.pictures + 1);
    if (err) return fail(err);
    nvdec_h264_end(&d->h264); d->h264.n_out = 0;
    if (flags & kVTDecodeFrame_DoNotOutputFrame)
        return VTDecoderSessionEmitDecodedFrame(d->session, frame, noErr, kVTDecodeInfo_FrameDropped, nullptr);
    err = emitSurface(d, frame, d->surfaces.cpu + surface * d->stride, d->layout.pitch, d->layout.width,
                      d->layout.luma_bytes, d->layout.height);
    return err ? fail(err) : noErr;
}

static const CMBaseClass baseClass = {
    kCMBaseObject_ClassVersion_1, sizeof(Storage), nullptr, invalidate, finalize,
    describe, copyProperty, nullptr, nullptr, nullptr
};
static const VTVideoDecoderClass decoderClass = {
    kVTVideoDecoder_ClassVersion_1, start, decode, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr
};
static const VTVideoDecoderVTable vtable = {{nullptr, &baseClass}, &decoderClass};
OSStatus create(FourCharCode codec, CFAllocatorRef allocator, VTVideoDecoderRef *out) {
    if (!out || (codec != kCMVideoCodecType_H264 && codec != kCMVideoCodecType_HEVC)) return kVTParameterErr;
    *out = nullptr;
    return CMDerivedObjectCreate(allocator, &vtable.base, VTVideoDecoderGetClassID(),
                                reinterpret_cast<CMBaseObjectRef *>(out));
}
}
// Factory for the plug-in bundle (Info.plist CMFactoryFunction): VideoToolbox
// loads NVVTDecoder.bundle from /Library/Video/Plug-Ins and matches it for
// avc1 as a hardware decoder, so every VT client can use NVDEC.
extern "C" __attribute__((visibility("default")))
OSStatus NVVT_H264_CreateInstance(FourCharCode codec, CFAllocatorRef allocator, VTVideoDecoderRef *out) {
    if (getenv("NVVT_DEBUG")) fprintf(stderr, "NVVTDecoder: CreateInstance '%.4s'\n", reinterpret_cast<const char *>(&codec));
    return create(codec, allocator, out);
}
// VideoToolbox SPI: (codec, matching info as in a plug-in's CMClassImplementations, factory)
extern "C" OSStatus VTRegisterVideoDecoderWithInfo(FourCharCode, CFDictionaryRef, OSStatus (*)(FourCharCode,
                                                   CFAllocatorRef, VTVideoDecoderRef *)) __attribute__((weak_import));
extern "C" OSStatus NVVTRegisterH264Decoder(void) {
    // with the same matching info as the plug-in bundle (hardware, rating 400)
    if (VTRegisterVideoDecoderWithInfo && !getenv("NVVT_PLAIN_REGISTER")) {
        CFMutableDictionaryRef info = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                                &kCFTypeDictionaryValueCallBacks);
        const int rating = 400;
        CFNumberRef r = CFNumberCreate(nullptr, kCFNumberIntType, &rating);
        CFDictionarySetValue(info, CFSTR("CMClassImplementationID"), CFSTR("org.local.macosdevicelab.videodecoder.h264.nvdec"));
        CFDictionarySetValue(info, CFSTR("VTCodecType"), CFSTR("avc1"));
        CFDictionarySetValue(info, CFSTR("VTIsHardwareAccelerated"), kCFBooleanTrue);
        CFDictionarySetValue(info, CFSTR("VTRating"), r);
        CFDictionarySetValue(info, CFSTR("VTCodecName"), CFSTR("H.264"));
        CFDictionarySetValue(info, CFSTR("VTDecoderName"), CFSTR("NVIDIA NVDEC H.264"));
        const OSStatus s = VTRegisterVideoDecoderWithInfo(kCMVideoCodecType_H264, info, create);
        CFRelease(r); CFRelease(info);
        return s;
    }
    return VTRegisterVideoDecoder(kCMVideoCodecType_H264, create);
}
extern "C" OSStatus NVVTRegisterHEVCDecoder(void) {
    if (!VTRegisterVideoDecoderWithInfo) return VTRegisterVideoDecoder(kCMVideoCodecType_HEVC, create);
    CFMutableDictionaryRef info = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                            &kCFTypeDictionaryValueCallBacks);
    const int rating = 400;
    CFNumberRef r = CFNumberCreate(nullptr, kCFNumberIntType, &rating);
    CFDictionarySetValue(info, CFSTR("CMClassImplementationID"), CFSTR("org.local.macosdevicelab.videodecoder.hevc.nvdec"));
    CFDictionarySetValue(info, CFSTR("VTCodecType"), CFSTR("hvc1"));
    CFDictionarySetValue(info, CFSTR("VTIsHardwareAccelerated"), kCFBooleanTrue);
    CFDictionarySetValue(info, CFSTR("VTRating"), r);
    CFDictionarySetValue(info, CFSTR("VTCodecName"), CFSTR("HEVC"));
    CFDictionarySetValue(info, CFSTR("VTDecoderName"), CFSTR("NVIDIA NVDEC HEVC"));
    const OSStatus s = VTRegisterVideoDecoderWithInfo(kCMVideoCodecType_HEVC, info, create);
    CFRelease(r); CFRelease(info);
    return s;
}
extern "C" void NVVTGetStats(NVVTStats *out) {
    if (out) *out = {sessions.load(), submitted.load(), emitted.load(), errors.load(), gpuNS.load()};
}
