#pragma once

// Optional tensor-core speed unlock for AD10x (RTX 40) based on the public
// Zagitta/gpu-unlock research. The signed SEC2 Booter has a stack overflow in
// its signature load; a crafted signature image drives a ROP chain that opens
// the FEAT/WPR privilege-level masks from the heavy-secure context. The host
// can then write the SS0/SS1 speed selectors, and the values survive a PCIe
// FLR, so the normal GSP boot proceeds afterwards.
//
// This file contains only the AD102/AD103/AD104/AD106/AD107 data and the
// payload assembler. The Booter image stays the stock signed NVIDIA 610.43.03
// AD102 image already staged by NVGspCore; nothing here patches code or
// bypasses a signature check.
//
// Register/payload facts are from the public gpu-unlock project
// (https://github.com/Zagitta/gpu-unlock, GPL-2.0), recipes.c
// (ad102_v67 / ad102_plm / selectors). This implementation is independent.

#include <stddef.h>
#include <stdint.h>

namespace nvgsp {
namespace unlock {

constexpr uint32_t kFeatPlm = 0x00823804u;
constexpr uint32_t kFeatSs0 = 0x0082381cu;
constexpr uint32_t kFeatSs1 = 0x00823820u;
constexpr uint32_t kWprCfgPlm = 0x001fa7ccu;
constexpr uint32_t kFbpaPlm = 0x009a0148u;
constexpr uint32_t kWprPlm = 0x001fa7c4u;

constexpr uint32_t kSs0Unlock = 0x88888888u;
constexpr uint32_t kSs1Unlock = 0x00000008u;
constexpr uint32_t kAd10xBooterMarker = 0x67b00001u;

struct RegWrite {
    uint32_t address;
    uint32_t value;
    bool optional;
};

// Four stores, one Booter launch: the payload has six slots, and the last
// entry is repeated to fill the unused two.
inline const RegWrite *ad10xPrivilegedWrites(size_t *count) {
    static const RegWrite writes[] = {
        {kWprCfgPlm, 0xfffff0ffu, false},
        {kFbpaPlm,   0xffffffffu, true},   // hardware may keep the old field
        {kWprPlm,    0xffffffffu, false},
        {kFeatPlm,   0xffffffffu, false},
    };
    if (count) *count = sizeof(writes) / sizeof(writes[0]);
    return writes;
}

inline const RegWrite *ad10xSelectors(size_t *count) {
    static const RegWrite writes[] = {
        {kFeatSs0, kSs0Unlock, false},
        {kFeatSs1, kSs1Unlock, false},
    };
    if (count) *count = sizeof(writes) / sizeof(writes[0]);
    return writes;
}

constexpr uint32_t kPayloadBytes = 0xfa00u;
constexpr uint32_t kPayloadFill = 0x0000019cu;
constexpr uint32_t kMaxSlots = 6;

namespace detail {
inline void put32(uint8_t *image, uint32_t offset, uint32_t value) {
    image[offset + 0] = static_cast<uint8_t>(value);
    image[offset + 1] = static_cast<uint8_t>(value >> 8);
    image[offset + 2] = static_cast<uint8_t>(value >> 16);
    image[offset + 3] = static_cast<uint8_t>(value >> 24);
}
}  // namespace detail

// Build the AD102 V67 signature image. `bytes` must be at least
// kPayloadBytes; the caller usually allocates a page-rounded DMA buffer.
// `writes` are the four privileged stores; each launch handles up to six.
inline bool buildAd102V67Payload(uint8_t *image, size_t bytes,
                                 const RegWrite *writes, size_t count) {
    if (!image || bytes < kPayloadBytes || !writes || !count ||
        count > kMaxSlots) return false;

    for (uint32_t offset = 0; offset < kPayloadBytes; offset += 4)
        detail::put32(image, offset, kPayloadFill);

    // Fixed chain words for the 610.43.03 AD102 Booter (see recipes.c
    // ad102_v67_words). Offsets are DMEM/Signature-image offsets, not BAR0.
    static const uint32_t words[][2] = {
        {0x1100, 0x00000007u},
        {0xf93c, 0x00005f50u},   // epilogue: r3 canary pointer, write 1
        {0xf950, 0x0000257eu},   // write entry
        {0xf954, 0x00005f50u},
        {0xf968, 0x0000257eu},
        {0xf96c, 0x00005f50u},
        {0xf980, 0x0000257eu},
        {0xf984, 0x00005f50u},
        {0xf998, 0x0000257eu},
        {0xf99c, 0x00005f50u},
        {0xf9b0, 0x0000257eu},
        {0xf9b4, 0x00005f50u},
        {0xf9c8, 0x0000257eu},
        {0xf9cc, 0x00005f50u},
        {0xf9d8, kAd10xBooterMarker},
        {0xf9e0, 0x00007a02u},   // marker gadget
        {0xf9ec, 0x000031b5u},   // publish I[0x1000]
        {0xf9f0, 0x0000771eu},   // clean HS exit
    };
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i)
        detail::put32(image, words[i][0], words[i][1]);

    // Six frames of 0x18 bytes. Frame i value/address slots hold the store
    // value/address; the return word between them is in the fixed table.
    static const uint32_t slots[kMaxSlots][2] = {
        {0xf944, 0xf948}, {0xf95c, 0xf960}, {0xf974, 0xf978},
        {0xf98c, 0xf990}, {0xf9a4, 0xf9a8}, {0xf9bc, 0xf9c0},
    };
    for (size_t i = 0; i < kMaxSlots; ++i) {
        const RegWrite &w = writes[i < count ? i : count - 1];
        detail::put32(image, slots[i][0], w.value);
        detail::put32(image, slots[i][1], w.address);
    }
    return true;
}

}  // namespace unlock
}  // namespace nvgsp
