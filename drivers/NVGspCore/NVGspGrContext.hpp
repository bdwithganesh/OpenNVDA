#pragma once
#include <stddef.h>
#include <stdint.h>

namespace nvgsp {
// Proven AD103 boot layout (nouveau r535 golden set). Each active GR channel
// needs its own backing; ATTRIBUTE_CB VA must be 64 MiB aligned.
constexpr uint64_t kGrContextBytes = 0x2a00000;
constexpr uint32_t kGrPromoteBytes = 560;
struct GrContextEntry { uint16_t id; uint32_t offset, bytes; uint8_t init, nonmapped; };
constexpr GrContextEntry kGrContextEntries[] = {
    {0, 0x2600000, 0x18c000, 1, 0}, {2, 0x2790000, 0x5000, 1, 0},
    {3, 0x27a0000, 0x3000, 0, 0}, {4, 0x27b0000, 0x20000, 0, 0},
    {5, 0, 0x25e8800, 0, 0}, {6, 0x2800000, 0x80000, 0, 0},
    {9, 0x2880000, 0x10000, 1, 0}, {10, 0x2890000, 0x80000, 1, 1},
    {11, 0x2910000, 0x80000, 1, 0}
};
inline void grPut(uint8_t *p, size_t offset, uint64_t value, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i) p[offset + i] = uint8_t(value >> (8 * i));
}
inline bool buildGrPromote(uint32_t client, uint32_t channel, uint64_t physical,
                           uint64_t va, uint8_t *out, size_t bytes, uint64_t globalVa = 0) {
    const bool sharedGlobals = globalVa != 0;
    if (!globalVa) globalVa = va;
    if (!client || !channel || !physical || !va || !out || bytes < kGrPromoteBytes ||
        (physical & 0x1fffff) || (va & 0x3ffffff) || (globalVa & 0x3ffffff) ||
        physical > UINT64_MAX - kGrContextBytes || va > UINT64_MAX - kGrContextBytes ||
        globalVa > UINT64_MAX - kGrContextBytes)
        return false;
    for (size_t i = 0; i < kGrPromoteBytes; ++i) out[i] = 0;
    grPut(out, 0, 1, 4); grPut(out, 12, client, 4); grPut(out, 16, channel, 4);
    size_t count = 0;
    for (size_t i = 0; i < sizeof(kGrContextEntries) / sizeof(kGrContextEntries[0]); ++i) {
        const auto &e = kGrContextEntries[i];
        // Global FECS/PAM resources already initialized by the stock channel.
        // Its nonmapped PAM has neither initialization nor a VA to promote.
        if (sharedGlobals && e.nonmapped) continue;
        const bool privateBuffer = e.id == 0 || e.id == 2;
        const bool initialize = e.init && (!sharedGlobals || privateBuffer);
        auto *p = out + 48 + count++ * 32;
        grPut(p, 8, e.nonmapped ? 0 : (privateBuffer || !sharedGlobals ? va : globalVa) + e.offset, 8);
        if (initialize) {
            grPut(p, 0, physical + e.offset, 8); grPut(p, 16, e.bytes, 8);
            grPut(p, 24, 4, 4); // VIDMEM, uncached
        }
        grPut(p, 28, e.id, 2); p[30] = initialize; p[31] = e.nonmapped;
    }
    grPut(out, 40, count, 4);
    return true;
}
// Extra GR channels sharing the stock channel's global buffers own only MAIN
// and PATCH. Compact layout: MAIN at +0, PATCH at +0x190000, one 2 MiB block.
constexpr uint64_t kGrPrivateBytes = 0x200000;
constexpr uint32_t kGrPrivatePatchOffset = 0x190000;
inline bool buildGrPromoteCompact(uint32_t client, uint32_t channel, uint64_t physical,
                                  uint64_t va, uint64_t globalVa, uint8_t *out, size_t bytes) {
    if (!client || !channel || !physical || !va || !globalVa || !out || bytes < kGrPromoteBytes ||
        (physical & 0x1fffff) || (va & 0x1fffff) || (globalVa & 0x3ffffff) ||
        physical > UINT64_MAX - kGrPrivateBytes || va > UINT64_MAX - kGrPrivateBytes ||
        globalVa > UINT64_MAX - kGrContextBytes)
        return false;
    for (size_t i = 0; i < kGrPromoteBytes; ++i) out[i] = 0;
    grPut(out, 0, 1, 4); grPut(out, 12, client, 4); grPut(out, 16, channel, 4);
    size_t count = 0;
    for (size_t i = 0; i < sizeof(kGrContextEntries) / sizeof(kGrContextEntries[0]); ++i) {
        const auto &e = kGrContextEntries[i];
        if (e.nonmapped) continue;
        auto *p = out + 48 + count++ * 32;
        if (e.id == 0 || e.id == 2) {
            const uint64_t off = e.id == 0 ? 0 : kGrPrivatePatchOffset;
            grPut(p, 8, va + off, 8);
            grPut(p, 0, physical + off, 8); grPut(p, 16, e.bytes, 8);
            grPut(p, 24, 4, 4);
            p[30] = 1;
        } else {
            grPut(p, 8, globalVa + e.offset, 8);
        }
        grPut(p, 28, e.id, 2);
    }
    grPut(out, 40, count, 4);
    return true;
}
static_assert(0x18c000 <= kGrPrivatePatchOffset &&
              kGrPrivatePatchOffset + 0x5000 <= kGrPrivateBytes, "compact GR layout");
}
