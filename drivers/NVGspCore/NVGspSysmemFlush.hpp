#pragma once
#include <stdint.h>

namespace nvgsp {
// NVIDIA 615.78.08 kmemsysProgramSysmemFlushBuffer_GA100 and ga100/dev_fb.h.
// AD103 uses this Ampere register layout. The page must remain DMA-mapped
// while bus mastering is enabled: a system flush can issue a real read.
constexpr uint32_t kSysmemFlushLo = 0x00100c10;
constexpr uint32_t kSysmemFlushHi = 0x00100c40;
constexpr uint64_t kSysmemFlushAddressLimit = 1ULL << 47;

template<class Io>
bool programSysmemFlush(Io &io, uint64_t busAddress,
                        uint32_t *readbackLo, uint32_t *readbackHi) {
    if (!readbackLo || !readbackHi) return false;
    *readbackLo = *readbackHi = 0;
    if (!busAddress || (busAddress & 4095) ||
        busAddress > kSysmemFlushAddressLimit - 4096) return false;
    const uint32_t lo = static_cast<uint32_t>(busAddress >> 8);
    const uint32_t hi = static_cast<uint32_t>(busAddress >> 40);
    uint32_t oldHi = 0;
    if (!io.read(kSysmemFlushHi, &oldHi) ||
        !io.write(kSysmemFlushHi, (oldHi & 0xff000000U) | hi) ||
        !io.write(kSysmemFlushLo, lo) ||
        !io.read(kSysmemFlushHi, readbackHi) ||
        !io.read(kSysmemFlushLo, readbackLo)) return false;
    return *readbackLo == lo && (*readbackHi & 0x00ffffffU) == hi;
}
} // namespace nvgsp
