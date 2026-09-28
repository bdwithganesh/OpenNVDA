#pragma once

#include <stdint.h>

namespace nvgsp {

// Ownership table for the user VA arena (GPU VA
// 0x28_0000_0000..0x30_0000_0000, 32 GiB, 2 MiB PTEs = 16384 pages).
//
// Each entry says what the page's PTE points at right now:
//   0                    unbound
//   1..kArenaRawOwner-1  memory-object handle (vaBindObject)
//   kArenaRawOwner       raw physical bind (selector 20), no object
//
// Earlier memFree released an object's pages while its PTEs were still
// valid, so the GPU could keep reading/writing freed VRAM or freed kernel
// sysmem through a stale VA. With this table memFree/memFreeAll can find
// and unbind exactly the pages that still point at the object, and
// vaBindObject can refuse to overwrite another client's live mapping.
constexpr uint64_t kArenaBase = 0x2800000000ULL;
constexpr uint64_t kArenaEnd = 0x3000000000ULL;
constexpr uint64_t kArenaPageBytes = 0x200000ULL;
constexpr uint32_t kArenaPages =
    static_cast<uint32_t>((kArenaEnd - kArenaBase) / kArenaPageBytes);
constexpr uint16_t kArenaRawOwner = 0xffff;

// [va, va + bytes) must be non-empty, 2 MiB aligned and inside the arena.
inline bool arenaRange(uint64_t va, uint64_t bytes, uint32_t *first,
                       uint32_t *count) {
    if (!first || !count || !bytes || ((va | bytes) & (kArenaPageBytes - 1)) ||
        va < kArenaBase || va >= kArenaEnd || bytes > kArenaEnd - va)
        return false;
    *first = static_cast<uint32_t>((va - kArenaBase) / kArenaPageBytes);
    *count = static_cast<uint32_t>(bytes / kArenaPageBytes);
    return true;
}

inline uint64_t arenaPageVa(uint32_t page) {
    return kArenaBase + static_cast<uint64_t>(page) * kArenaPageBytes;
}

inline void arenaMark(uint16_t *table, uint32_t first, uint32_t count,
                      uint16_t owner) {
    for (uint32_t i = 0; i < count && first + i < kArenaPages; ++i)
        table[first + i] = owner;
}

// Next maximal run of pages equal to `owner` at or after `start`. Returns
// false when there is none. Used to unbind a freed object with one
// PTE-range write per contiguous run.
inline bool arenaNextRun(const uint16_t *table, uint16_t owner, uint32_t start,
                         uint32_t *first, uint32_t *count) {
    if (!table || !first || !count || !owner) return false;
    uint32_t i = start;
    while (i < kArenaPages && table[i] != owner) ++i;
    if (i >= kArenaPages) return false;
    uint32_t j = i;
    while (j < kArenaPages && table[j] == owner) ++j;
    *first = i;
    *count = j - i;
    return true;
}

}  // namespace nvgsp
