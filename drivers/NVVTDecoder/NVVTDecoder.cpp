#include "NVVTDecoder.h"
#include "spi/VTVideoDecoderSPI.h"
#include <IOKit/IOKitLib.h>
#include <mach/mach_time.h>
#include <atomic>
#include <mutex>
#include <new>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdlib>
extern "C" {
#include "nvdec_h264.h"
}

namespace {
std::atomic<uint64_t> sessions{0}, submitted{0}, emitted{0}, errors{0}, gpuNS{0};
struct Buffer { uint64_t handle = 0, va = 0, bytes = 0; uint8_t *cpu = nullptr; };
struct Decoder {
    std::mutex mutex;
    io_connect_t conn = IO_OBJECT_NULL;
    VTVideoDecoderSession session = nullptr;
    nvdec_h264_dec h264;
    nvdec_h264_layout layout{};
    nvdec_h264_addrs addrs{};
    Buffer input, push, aux, surfaces;
    uint64_t stride = 0;
    int lengthBytes = 0, width = 0, height = 0, x0 = 0, y0 = 0;
    bool failed = false;
    std::vector<uint8_t> linear;
    Decoder() { nvdec_h264_init(&h264); }
    void release(Buffer &b) {
        if (b.cpu) IOConnectUnmapMemory64(conn, 0x1000 | (uint32_t)b.handle,
            mach_task_self(), (mach_vm_address_t)b.cpu);
        if (b.handle) IOConnectCallScalarMethod(conn, 23, &b.handle, 1, nullptr, nullptr);
        b = {};
    }
    ~Decoder() {
        if (conn) {
            release(surfaces); release(aux); release(push); release(input);
            IOServiceClose(conn);
        }
    }
    bool allocate(Buffer &b, uint64_t bytes, uint64_t va) {
        b.bytes = (bytes + 0x1fffff) & ~0x1fffffull;
        uint64_t args[] = {b.bytes, 1}, out[2]{};
        uint32_t count = 2;
        if (IOConnectCallScalarMethod(conn, 22, args, 2, out, &count) || count != 2) return false;
        b.handle = out[0]; b.va = va;
        uint64_t bind[] = {b.handle, va, 0};
        if (IOConnectCallScalarMethod(conn, 24, bind, 3, nullptr, nullptr)) return false;
        mach_vm_address_t address = 0; mach_vm_size_t size = 0;
        if (IOConnectMapMemory64(conn, 0x1000 | (uint32_t)b.handle, mach_task_self(),
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
        uint64_t engine = 0, out[2]{}; uint32_t count = 2;
        if (IOConnectCallScalarMethod(conn, 30, &engine, 1, out, &count)) return false;
        return allocate(input, 4ull << 20, 0x2b00000000ull) &&
               allocate(push, 2ull << 20, 0x2b40000000ull);
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
    OSStatus execute(uint32_t words) {
        struct { uint64_t va; uint32_t words, flags; } segment{push.va, words, 0};
        uint64_t engine = 2, seq = 0, done = 0; uint32_t count = 1;
        const uint64_t start = mach_absolute_time();
        auto kr = IOConnectCallMethod(conn, 21, &engine, 1, &segment, sizeof(segment),
                                     &seq, &count, nullptr, nullptr);
        if (kr) return kVTVideoDecoderMalfunctionErr;
        ++submitted;
        uint64_t wait[] = {2, seq, 2000000}; count = 1;
        kr = IOConnectCallScalarMethod(conn, 31, wait, 3, &done, &count);
        mach_timebase_info_data_t tb; mach_timebase_info(&tb);
        gpuNS += (mach_absolute_time() - start) * tb.numer / tb.denom;
        if (kr || done != seq || *(volatile uint32_t *)(input.cpu + NVDEC_IN_SEM) != h264.pictures + 1)
            return kVTVideoDecoderMalfunctionErr;
        return noErr;
    }
};
struct Storage { Decoder *decoder; };
Storage *storage(CMBaseObjectRef object) {
    return static_cast<Storage *>(CMBaseObjectGetDerivedStorage(object));
}
OSStatus invalidate(CMBaseObjectRef object) {
    Storage *s = storage(object);
    delete s->decoder; s->decoder = nullptr;
    return noErr;
}
void finalize(CMBaseObjectRef object) { invalidate(object); }
CFStringRef describe(CMBaseObjectRef) { return CFSTR("NVDEC H.264 on RTX 4080"); }
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
    if (st->decoder || CMFormatDescriptionGetMediaSubType(format) != kCMVideoCodecType_H264)
        return kVTVideoDecoderUnsupportedDataFormatErr;
    Decoder *d = new (std::nothrow) Decoder;
    if (!d) return kVTAllocationFailedErr;
    st->decoder = d; d->session = session;
    size_t nsets = 0, size = 0; const uint8_t *data = nullptr;
    OSStatus err = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, 0, &data,
                                                      &size, &nsets, &d->lengthBytes);
    if (err || (d->lengthBytes != 1 && d->lengthBytes != 2 && d->lengthBytes != 4))
        return kVTVideoDecoderBadDataErr;
    for (size_t i = 0; i < nsets; ++i) {
        err = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, i, &data, &size, nullptr, nullptr);
        if (err || !size) return kVTVideoDecoderBadDataErr;
        h264_nal nal{data, size, data[0] & 31, (data[0] >> 5) & 3};
        if (nvdec_h264_param_nal(&d->h264, &nal)) return kVTVideoDecoderBadDataErr;
    }
    const auto dim = CMVideoFormatDescriptionGetDimensions(format);
    d->width = dim.width; d->height = dim.height;
    if (d->width <= 0 || d->height <= 0 || d->width > 4096 || d->height > 2304)
        return kVTVideoDecoderUnsupportedDataFormatErr;
    int formatCode = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
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
    if (!d->open()) return kVTVideoDecoderNotAvailableNowErr;
    ++sessions;
    return noErr;
}

OSStatus decode(VTVideoDecoderRef instance, VTVideoDecoderFrame frame,
                CMSampleBufferRef sample, VTDecodeFrameFlags flags, VTDecodeInfoFlags *info) {
    Decoder *d = storage(reinterpret_cast<CMBaseObjectRef>(instance))->decoder;
    if (info) *info = 0;
    if (!d) return kVTVideoDecoderMalfunctionErr;
    std::lock_guard<std::mutex> guard(d->mutex);
    if (d->failed) return kVTVideoDecoderMalfunctionErr;
    auto fail = [&](OSStatus e) { ++errors; d->failed = true; return e; };
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
    if (!block || CMSampleBufferGetNumSamples(sample) != 1) return fail(kVTVideoDecoderBadDataErr);
    const size_t bytes = CMBlockBufferGetDataLength(block);
    if (!bytes || bytes > d->input.bytes - NVDEC_IN_BITSTREAM - 32)
        return fail(kVTVideoDecoderBadDataErr);
    std::vector<uint8_t> data(bytes);
    if (CMBlockBufferCopyDataBytes(block, 0, bytes, data.data())) return fail(kVTVideoDecoderBadDataErr);
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
    OSStatus err = d->execute(words);
    if (err) return fail(err);
    nvdec_h264_end(&d->h264); d->h264.n_out = 0;
    if (flags & kVTDecodeFrame_DoNotOutputFrame)
        return VTDecoderSessionEmitDecodedFrame(d->session, frame, noErr, kVTDecodeInfo_FrameDropped, nullptr);
    CVPixelBufferPoolRef pool = VTDecoderSessionGetPixelBufferPool(d->session);
    CVPixelBufferRef pixel = nullptr;
    if (!pool || CVPixelBufferPoolCreatePixelBuffer(nullptr, pool, &pixel)) return fail(kVTAllocationFailedErr);
    if (CVPixelBufferGetPixelFormatType(pixel) != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
        CVPixelBufferGetWidth(pixel) != (size_t)d->width || CVPixelBufferGetHeight(pixel) != (size_t)d->height ||
        CVPixelBufferGetPlaneCount(pixel) != 2 || CVPixelBufferLockBaseAddress(pixel, 0)) {
        CFRelease(pixel); return fail(kVTVideoDecoderMalfunctionErr);
    }
    const uint8_t *base = d->surfaces.cpu + surface * d->stride;
    for (int plane = 0; plane < 2; ++plane) {
        const uint32_t rows = d->layout.height >> plane;
        nvdec_detile(base + (plane ? d->layout.luma_bytes : 0), d->layout.pitch,
                     d->linear.data(), d->layout.width, d->layout.width, rows);
        uint8_t *dst = static_cast<uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(pixel, plane));
        const size_t pitch = CVPixelBufferGetBytesPerRowOfPlane(pixel, plane);
        for (int y = 0; y < (d->height >> plane); ++y)
            memcpy(dst + y * pitch, d->linear.data() +
                   (size_t)(y + (d->y0 >> plane)) * d->layout.width + d->x0, d->width);
    }
    CVPixelBufferUnlockBaseAddress(pixel, 0);
    err = VTDecoderSessionEmitDecodedFrame(d->session, frame, noErr, 0, pixel);
    CFRelease(pixel);
    if (err) return fail(err);
    ++emitted;
    return noErr;
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
    if (!out || codec != kCMVideoCodecType_H264) return kVTParameterErr;
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
extern "C" void NVVTGetStats(NVVTStats *out) {
    if (out) *out = {sessions.load(), submitted.load(), emitted.load(), errors.load(), gpuNS.load()};
}
