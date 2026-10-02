// C67D head0 CRC notifier: fixed DP/SF output, no production routing.
// Match NVIDIA570 EvoStart/StopHeadCRC32CaptureC3 and clc37dcrcnotif.h.
#pragma once
#include "nvdisplay_admin.hpp"

struct CrcSample { uint32_t status, count, compositor, raster, output; };
static inline kern_return_t crc_submit(io_connect_t c, uint32_t dma, uint32_t control) {
    const uint32_t words[] = {0x00082180,dma,control,0x00080218,0,0,0x00040200,1};
    return IOConnectCallStructMethod(c, 12, words, sizeof(words), nullptr, nullptr);
}
static inline bool crc_wait(io_connect_t c, const Snapshot &before,
                           uint32_t dma, uint32_t control) {
    const uint64_t end = ns_now()+200000000;
    Snapshot s = {}; uint32_t assy[2] = {}, armed[2] = {}; bool recorded = false;
    do {
        if (!snapshot(c, &s) || peek(c, 0x682180, 2, assy) || peek(c, 0x68a180, 2, armed)) return false;
        if (!recorded && ((s.pending&1) || memcmp(s.exception, before.exception, sizeof(s.exception)))) {
            dump("CRC-first-observed-exception-change", &s); recorded = true;
        }
        if (!(s.pending&1) && armed_matches(&s, before.usage[1], before.armed) &&
            assy[0] == dma && armed[0] == dma && assy[1] == control && armed[1] == control) return true;
        usleep(1000);
    } while (ns_now() < end);
    dump("CRC-timeout", &s);
    printf("CRC-state ASSY=%08x/%08x ARM=%08x/%08x\n", assy[0],assy[1],armed[0],armed[1]);
    return false;
}
static inline bool crc_decode(const uint32_t words[1024], CrcSample *out) {
    if (!words || !out) return false;
    const uint32_t status = words[0], count = (status>>16)&0xfff;
    // FourKiB notifier, eightDWORD stride, first CRC triple at11/12/13.
    if (!(status&1) || (status&0x38) || !count || count > 127) return false;
    const unsigned last = 11+(count-1)*8;
    *out = CrcSample{status,count,words[last],words[last+1],words[last+2]};
    return true;
}
// Caller owns a verified4KiB writable descriptor and zeroed notifier backing.
// Always attempt stop and require exact ARM unreference before caller frees it.
static inline bool crc_capture(io_connect_t c, uint32_t dma, uint64_t address,
                               const Snapshot &before, const uint32_t original[2],
                               CrcSample *out, bool *unreferenced) {
    *unreferenced = false;
    const auto start = crc_submit(c, dma, 0x30000); // DP: PRIMARY_CRC_SF30, WIN0
    const bool started = !start && crc_wait(c, before, dma, 0x30000);
    if (started) usleep(20000); // bounded one frame of notifier data
    const auto stop = crc_submit(c, original[0], original[1]);
    *unreferenced = !stop && crc_wait(c, before, original[0], original[1]);
    printf("CRC start-ARM=%s stop/exact-unreference=%s\n", started?"yes":"no",*unreferenced?"yes":"no");
    if (!started || !*unreferenced) return false;
    const uint64_t end = ns_now()+200000000;
    uint32_t words[1024] = {};
    do {
        if (!vread(c, address, words, 1024)) return false;
        if (words[0]&1) break;
        usleep(1000);
    } while (ns_now() < end);
    printf("CRC notifier status=%08x count=%u overflow=%x\n",words[0],(words[0]>>16)&0xfff,words[0]&0x38);
    if (!crc_decode(words, out)) return false;
    printf("CRC last compositor=%08x raster=%08x primary-DP-SF=%08x frames=%u\n",
        out->compositor,out->raster,out->output,out->count);
    return true;
}
