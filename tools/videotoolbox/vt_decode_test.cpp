/* Real VTDecompressionSession verification. PTS maps reordered H.264 samples
 * back to the display-order NV12 digests used by nvdec_run. A software VT
 * fallback cannot pass: the NVDEC submission counters must match the input. */
#include "NVVTDecoder.h"
#include <CoreVideo/CoreVideo.h>
#include <chrono>
#include <dlfcn.h>
#include <unistd.h>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
extern "C" {
#include "nvdec_h264.h"
#include "nvdec_hevc.h"
#include "nvdec_md5.h"
}

extern "C" void VTRegisterVideoDecoderBundleDirectory(CFURLRef) __attribute__((weak_import));   // SPI
extern "C" OSStatus VTCopyVideoDecoderList(CFDictionaryRef, CFArrayRef *) __attribute__((weak_import));   // SPI
struct Input {
    std::vector<uint8_t> vps, sps, pps;   // vps only for HEVC
    bool hevc = false;
    std::vector<std::vector<uint8_t>> packets;
    std::vector<int> pts;
    std::vector<std::string> hashes;
};
// .h265: one packet per picture (first_slice_segment_in_pic_flag), slices only, VPS/SPS/PPS kept apart.
static bool loadHevc(std::vector<uint8_t> &data, Input &input) {
    auto decoder = std::make_unique<nvdec_hevc_dec>();
    nvdec_hevc_init(decoder.get());
    size_t pos = 0;
    h264_nal nal;
    std::vector<uint8_t> packet;
    auto flush = [&] { if (!packet.empty()) { input.packets.push_back(std::move(packet)); packet.clear(); } };
    while (h264_next_nal(data.data(), data.size(), &pos, &nal)) {
        if (nal.size < 2) return false;
        const int t = hevc_nal_type(nal.data);
        if (!hevc_is_slice(t)) {
            if (t == HEVC_NAL_VPS || t == HEVC_NAL_SPS || t == HEVC_NAL_PPS) {
                auto &params = t == HEVC_NAL_VPS ? input.vps : t == HEVC_NAL_SPS ? input.sps : input.pps;
                std::vector<uint8_t> value(nal.data, nal.data + nal.size);
                if (!params.empty() && params != value) {
                    std::cerr << "parameter changes require separate test sessions\n"; return false;
                }
                params = std::move(value);
                if (nvdec_hevc_param_nal(decoder.get(), nal.data, nal.size)) return false;
            }
            continue;
        }
        hevc_slice sl;
        if (hevc_parse_slice(nal.data, nal.size, decoder->sps, decoder->pps, &sl)) return false;
        if (sl.first_slice_segment_in_pic_flag) flush();
        for (int shift = 24; shift >= 0; shift -= 8) packet.push_back((uint32_t)nal.size >> shift);
        packet.insert(packet.end(), nal.data, nal.data + nal.size);
    }
    flush();
    return !input.vps.empty();
}
static bool loadH264(const std::string &path, Input &input);
static bool checkMeta(const std::string &path, Input &input);
static bool load(const std::string &path, Input &input) {
    if (std::ifstream h265(path + ".h265", std::ios::binary); h265) {
        std::vector<uint8_t> data{std::istreambuf_iterator<char>(h265), {}};
        input.hevc = true;
        if (!loadHevc(data, input)) return false;
    } else if (!loadH264(path, input)) return false;
    return checkMeta(path, input);
}
static bool loadH264(const std::string &path, Input &input) {
    std::ifstream stream(path + ".h264", std::ios::binary);
    if (!stream) { std::cerr << "cannot read H.264: " << path << '\n'; return false; }
    std::vector<uint8_t> data{std::istreambuf_iterator<char>(stream), {}};
    auto decoder = std::make_unique<nvdec_h264_dec>();
    nvdec_h264_init(decoder.get());
    size_t pos = 0;
    h264_nal nal;
    h264_slice prev{};
    std::vector<uint8_t> packet;
    auto flush = [&] { if (!packet.empty()) { input.packets.push_back(std::move(packet)); packet.clear(); } };
    while (h264_next_nal(data.data(), data.size(), &pos, &nal)) {
        const bool slice = nal.type == H264_NAL_SLICE || nal.type == H264_NAL_IDR;
        if (!slice) {
            if (nal.type >= H264_NAL_SEI && nal.type <= H264_NAL_AUD) flush();
            if (nvdec_h264_param_nal(decoder.get(), &nal)) return false;
            if (nal.type == H264_NAL_SPS || nal.type == H264_NAL_PPS) {
                auto &params = nal.type == H264_NAL_SPS ? input.sps : input.pps;
                std::vector<uint8_t> value(nal.data, nal.data + nal.size);
                if (!params.empty() && params != value) {
                    std::cerr << "parameter changes require separate test sessions\n"; return false;
                }
                params = std::move(value);
            }
            continue;
        }
        h264_slice current;
        if (h264_parse_slice(nal.data, nal.size, decoder->sps, decoder->pps, &current)) return false;
        if (!packet.empty() && nvdec_h264_new_picture(decoder.get(), &prev, &current)) flush();
        if (nal.size > UINT32_MAX) return false;
        for (int shift = 24; shift >= 0; shift -= 8) packet.push_back((uint32_t)nal.size >> shift);
        packet.insert(packet.end(), nal.data, nal.data + nal.size);
        prev = current;
    }
    flush();
    return true;
}
static bool checkMeta(const std::string &path, Input &input) {
    std::ifstream pts(path + ".pts"), hashes(path + ".md5");
    int p; std::string hash;
    while (pts >> p) input.pts.push_back(p);
    while (hashes >> hash) {
        if (hash.size() != 32 || hash.find_first_not_of("0123456789abcdef") != std::string::npos) return false;
        input.hashes.push_back(hash);
    }
    if (input.sps.empty() || input.pps.empty() || input.packets.empty() ||
        input.packets.size() != input.pts.size() || input.pts.size() != input.hashes.size()) return false;
    std::vector<bool> seen(input.pts.size());
    for (int index : input.pts) {
        if (index < 0 || (size_t)index >= seen.size() || seen[index]) return false;
        seen[index] = true;
    }
    return true;
}
struct Output {
    const Input *input;
    std::vector<bool> seen;
    std::mutex mutex;
    unsigned count = 0, bad = 0;
    explicit Output(const Input &i) : input(&i), seen(i.hashes.size()) {}
};
static void callback(void *ref, void *, OSStatus status, VTDecodeInfoFlags flags,
                     CVImageBufferRef image, CMTime pts, CMTime) {
    auto &out = *static_cast<Output *>(ref);
    std::lock_guard<std::mutex> lock(out.mutex);
    if (status || !image || (flags & kVTDecodeInfo_FrameDropped) || !CMTIME_IS_NUMERIC(pts)) {
        std::cerr << "callback error=" << status << " flags=" << flags << '\n'; ++out.bad; return;
    }
    const int64_t index = CMTimeConvertScale(pts, 30, kCMTimeRoundingMethod_Default).value;
    if (index < 0 || (size_t)index >= out.seen.size() || out.seen[index]) { ++out.bad; return; }
    out.seen[index] = true; ++out.count;
    if (CVPixelBufferGetPlaneCount(image) != 2 ||
        CVPixelBufferGetPixelFormatType(image) != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
        CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly)) { ++out.bad; return; }
    // VT_NOMD5: timing run, the 3 MB/frame hash (inside the timed window) is skipped and nothing is verified
    if (getenv("VT_NOMD5")) { CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly); return; }
    nvdec_md5 md5; nvdec_md5_init(&md5);
    for (int plane = 0; plane < 2; ++plane) {
        const auto *base = static_cast<const uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(image, plane));
        const size_t pitch = CVPixelBufferGetBytesPerRowOfPlane(image, plane);
        const size_t width = CVPixelBufferGetWidth(image);
        const size_t height = CVPixelBufferGetHeight(image) >> plane;
        for (size_t y = 0; y < height; ++y) nvdec_md5_update(&md5, base + y * pitch, width);
    }
    char hex[33]; nvdec_md5_hex(&md5, hex);
    CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
    if (out.input->hashes[index] != hex) {
        std::cerr << "frame " << index << " got=" << hex << " want=" << out.input->hashes[index] << '\n';
        ++out.bad;
    }
}
int main(int argc, char **argv) {
    if (argc < 2 || argc > 3 || (argc == 3 && std::string(argv[2]) != "--parse-only")) {
        std::cerr << "usage: vt_decode_test vector-prefix [--parse-only]\n"; return 2;
    }
    Input input;
    if (!load(argv[1], input)) { std::cerr << "invalid vector, parameter sets or reference metadata\n"; return 1; }
    const uint8_t *sets[] = {input.sps.data(), input.pps.data()}, *hsets[] = {input.vps.data(), input.sps.data(), input.pps.data()};
    size_t sizes[] = {input.sps.size(), input.pps.size()}, hsizes[] = {input.vps.size(), input.sps.size(), input.pps.size()};
    CMVideoFormatDescriptionRef format = nullptr;
    OSStatus status = input.hevc
        ? CMVideoFormatDescriptionCreateFromHEVCParameterSets(nullptr, 3, hsets, hsizes, 4, nullptr, &format)
        : CMVideoFormatDescriptionCreateFromH264ParameterSets(nullptr, 2, sets, sizes, 4, &format);
    if (status) { std::cerr << "format status=" << status << '\n'; return 1; }
    const auto dimensions = CMVideoFormatDescriptionGetDimensions(format);
    std::cout << argv[1] << ": " << dimensions.width << 'x' << dimensions.height
              << ", " << input.packets.size() << " access units\n";
    if (argc == 3) { CFRelease(format); std::cout << "parse PASS (no GPU execution)\n"; return 0; }
    // VT_SYSTEM=1: no in-process registration, VT has to find the installed plug-in
    if (getenv("VT_METAL")) {   // load the Metal driver first (NVMTL 0.9.93 registers the NVDEC decoders)
        void *m = dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_NOW);
        auto create = m ? (void *(*)())dlsym(m, "MTLCreateSystemDefaultDevice") : nullptr;
        std::cout << "metal device " << (create && create() ? "ok" : "none") << std::endl;
        usleep(500000);
        std::cout << "VTIsHardwareDecodeSupported h264 " << int(VTIsHardwareDecodeSupported(kCMVideoCodecType_H264))
                  << " hevc " << int(VTIsHardwareDecodeSupported(kCMVideoCodecType_HEVC)) << std::endl;
    }
    if (getenv("VT_PRO")) VTRegisterProfessionalVideoWorkflowVideoDecoders();   // lets VT load /Library/Video/Plug-Ins
    if (const char *dir = getenv("VT_BUNDLEDIR")) {   // SPI: a directory of decoder bundles
        CFURLRef u = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8 *)dir, strlen(dir), true);
        if (VTRegisterVideoDecoderBundleDirectory) VTRegisterVideoDecoderBundleDirectory(u);
        CFRelease(u);
    }
    if (getenv("VT_LIST") && VTCopyVideoDecoderList) {   // which decoders VT knows
        CFArrayRef list = nullptr;
        if (!VTCopyVideoDecoderList(nullptr, &list) && list) {
            for (CFIndex i = 0; i < CFArrayGetCount(list); ++i) {
                CFStringRef d = CFCopyDescription(CFArrayGetValueAtIndex(list, i));
                char b[2048] = "";
                CFStringGetCString(d, b, sizeof b, kCFStringEncodingUTF8);
                for (char *c = b; *c; ++c) if (*c == '\n') *c = ' ';
                std::cout << "decoder " << b << '\n';
                CFRelease(d);
            }
            CFRelease(list);
        }
    }
    status = getenv("VT_SYSTEM") ? noErr : input.hevc ? NVVTRegisterHEVCDecoder() : NVVTRegisterH264Decoder();
    if (status) { std::cerr << "register status=" << status << '\n'; CFRelease(format); return 1; }
    Output output(input);
    VTDecompressionOutputCallbackRecord record{callback, &output};
    VTDecompressionSessionRef session = nullptr;
    // ask for a hardware decoder, as players on Macs with a GPU decoder do
    CFMutableDictionaryRef spec = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                            &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(spec, kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder, kCFBooleanTrue);
    if (const char *id = getenv("VT_DECODER_ID")) {   // pick one decoder by its CMClassImplementationID
        extern const CFStringRef kVTVideoDecoderSpecification_DecoderID;
        CFStringRef s = CFStringCreateWithCString(nullptr, id, kCFStringEncodingUTF8);
        CFDictionarySetValue(spec, kVTVideoDecoderSpecification_DecoderID, s);
        CFRelease(s);
    }
    if (getenv("VT_REQUIRE_HW"))
        CFDictionarySetValue(spec, kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder, kCFBooleanTrue);
    status = VTDecompressionSessionCreate(nullptr, format, spec, nullptr, &record, &session);
    CFRelease(spec);
    if (status) { std::cerr << "session status=" << status << '\n'; CFRelease(format); return 1; }
    CFTypeRef hardware = nullptr;
    OSStatus propertyStatus = VTSessionCopyProperty(session,
        kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder, nullptr, &hardware);
    bool hwUsed = hardware && CFEqual(hardware, kCFBooleanTrue);
    std::cout << "hardware property status=" << propertyStatus << " value="
              << (hwUsed ? "true" : "false/unavailable") << '\n';
    if (hardware) CFRelease(hardware);
    const auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < input.packets.size() && !status; ++i) {
        const auto &packet = input.packets[i];
        CMBlockBufferRef block = nullptr;
        status = CMBlockBufferCreateWithMemoryBlock(nullptr, nullptr, packet.size(), nullptr,
                    nullptr, 0, packet.size(), 0, &block);
        if (!status) status = CMBlockBufferReplaceDataBytes(packet.data(), block, 0, packet.size());
        CMSampleBufferRef sample = nullptr;
        CMSampleTimingInfo timing{CMTimeMake(1,30), CMTimeMake(input.pts[i],30), CMTimeMake(i,30)};
        const size_t size = packet.size();
        if (!status) status = CMSampleBufferCreateReady(nullptr, block, format, 1, 1, &timing, 1, &size, &sample);
        if (!status) status = VTDecompressionSessionDecodeFrame(session, sample, 0, nullptr, nullptr);
        if (sample) CFRelease(sample);
        if (block) CFRelease(block);
        if (status) std::cerr << "sample " << i << " status=" << status << '\n';
    }
    OSStatus finish = VTDecompressionSessionFinishDelayedFrames(session);
    OSStatus wait = VTDecompressionSessionWaitForAsynchronousFrames(session);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    NVVTStats stats{}; NVVTGetStats(&stats);
    // system mode: the decoder runs from the installed bundle (loaded by VT or by NVMTL), read its counters
    if (void *b = getenv("VT_SYSTEM") ? dlopen("/Library/Video/Plug-Ins/NVVTDecoder.bundle/Contents/MacOS/NVVTDecoder", RTLD_NOLOAD | RTLD_NOW) : nullptr)
        if (auto get = (void (*)(NVVTStats *))dlsym(b, "NVVTGetStats")) { get(&stats); hwUsed = hwUsed || stats.submitted; }
    VTDecompressionSessionInvalidate(session); CFRelease(session); CFRelease(format);
    const bool sys = getenv("VT_SYSTEM") != nullptr;   // the plug-in keeps its own counters
    const bool frames = !status && !finish && !wait && !output.bad && output.count == input.packets.size();
    const bool pass = frames && (sys ? hwUsed : stats.sessions == 1 && stats.submitted == input.packets.size() && stats.emitted == output.count && !stats.errors);
    std::cout << "VT frames=" << output.count << " mismatches/errors=" << output.bad
              << " NVDEC sessions=" << stats.sessions << " submitted=" << stats.submitted
              << " emitted=" << stats.emitted << " decode_errors=" << stats.errors
              << " finish=" << finish << " wait=" << wait
              << " end_to_end_fps=" << output.count / seconds
              << " submit_wait_fps=" << (stats.gpu_nanoseconds ? stats.submitted * 1e9 / stats.gpu_nanoseconds : 0)
              << ": " << (pass ? "PASS" : "FAIL") << '\n';
    return pass ? 0 : 1;
}
