#pragma once
#include <stdint.h>

namespace nvgsp {

// Selector 21 keeps its 16-byte record size. The last word is now flags;
// old callers already write zero. Advertise supported bits in IORegistry.
constexpr uint32_t kExecNoPrefetch = 1u << 0;
constexpr uint32_t kExecSupportedFlags = kExecNoPrefetch;
constexpr uint32_t kExecMaxSegments = 64;

inline bool execSegmentValid(uint64_t va, uint32_t dwords, uint32_t flags) {
    return !(va & 3) && va < (1ull << 40) && dwords && dwords < (1u << 21) &&
           uint64_t(dwords) * 4 <= (1ull << 40) - va &&
           !(flags & ~kExecSupportedFlags);
}

inline uint32_t execEntryHigh(uint64_t va, uint32_t dwords, uint32_t flags) {
    // NVC56F_GP_ENTRY1_SYNC_WAIT (bit 31) prevents PBDMA from fetching a
    // GPU-written push before the preceding engine sync reaches ESCHED.
    return uint32_t((va >> 32) & 0xff) | (dwords << 10) |
           ((flags & kExecNoPrefetch) ? 0x80000000u : 0);
}

} // namespace nvgsp
