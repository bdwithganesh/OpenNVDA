#pragma once

#include <stdint.h>

namespace nvgsp {

// 0.127.0: save / restore of kext-heap VRAM objects across S3 (SR path).
//
// GSP-RM's FBSR saves only the VRAM RM itself allocated. The user heap
// ([1 GiB, fbFreeLimit), memAlloc domain 0/2), the per-client arena page
// tables and the video chunks live there too and are lost when the GPU is
// powered down. Before srSuspend the kext copies every live VRAM object into
// 2 MiB physically contiguous system-memory chunks with the persistent copy
// engine channel (physical addressing on both sides: no GPU VA needed), and
// copies them back after a successful srResume.
//
// Copy-engine methods (clc7b5.h, NVIDIA open-gpu-doc, MIT), subchannel 4.
constexpr uint32_t kEvictChunkBytes = 0x200000;
constexpr uint32_t kCeSubch = 4;
constexpr uint32_t kCeClassAda = 0xc7b5;
constexpr uint32_t kCeSetObject = 0x000;
constexpr uint32_t kCeSetSrcPhysMode = 0x260;   // + SET_DST_PHYS_MODE 0x264
constexpr uint32_t kCeOffsetInUpper = 0x400;    // .. LINE_COUNT 0x41c
constexpr uint32_t kCeLaunchDma = 0x300;
constexpr uint32_t kCePhysLocalFb = 0;
constexpr uint32_t kCePhysCoherentSysmem = 1;
// LAUNCH_DMA: NON_PIPELINED (2) | FLUSH_ENABLE (bit 2, FLUSH_TYPE SYS) |
// SRC/DST_MEMORY_LAYOUT PITCH (bits 7, 8) | SRC/DST_TYPE PHYSICAL (12, 13),
// single line (MULTI_LINE_ENABLE 0), no semaphore / interrupt: the ring's
// host semaphore release after the batch is the fence.
constexpr uint32_t kCeLaunchPhysCopy = 0x2u | (1u << 2) | (1u << 7) | (1u << 8) |
                                       (1u << 12) | (1u << 13);
constexpr uint32_t kCeCopyWords = 14;          // one copy, SET_OBJECT excluded
constexpr uint32_t kEvictBatchCopies = 64;     // 128 MiB per submission

inline uint32_t ceMethod(uint32_t mthd, uint32_t count) {
    return 0x20000000u | (count << 16) | (kCeSubch << 13) | (mthd >> 2);
}

struct EvictCopy {
    uint64_t src, dst;     // physical addresses
    bool srcSys, dstSys;   // true = coherent system memory, false = VRAM
    uint32_t bytes;        // 1 .. 2^32-1, one line
};

// Method stream for `n` copies: SET_OBJECT once, then per copy the two
// PHYS_MODE targets, OFFSET_IN/OUT, PITCH_IN/OUT, LINE_LENGTH_IN, LINE_COUNT
// and LAUNCH_DMA. Returns words written, 0 if `max` is too small or a copy
// is empty / above 49-bit addresses.
inline uint32_t buildEvictBatch(const EvictCopy *c, uint32_t n, uint32_t *out, uint32_t max) {
    if (!c || !out || !n || max < 2 + n * kCeCopyWords) return 0;
    uint32_t w = 0;
    out[w++] = ceMethod(kCeSetObject, 1);
    out[w++] = kCeClassAda;
    for (uint32_t i = 0; i < n; ++i) {
        const EvictCopy &k = c[i];
        if (!k.bytes || (k.src >> 49) || (k.dst >> 49)) return 0;
        out[w++] = ceMethod(kCeSetSrcPhysMode, 2);
        out[w++] = k.srcSys ? kCePhysCoherentSysmem : kCePhysLocalFb;
        out[w++] = k.dstSys ? kCePhysCoherentSysmem : kCePhysLocalFb;
        out[w++] = ceMethod(kCeOffsetInUpper, 8);
        out[w++] = static_cast<uint32_t>(k.src >> 32);
        out[w++] = static_cast<uint32_t>(k.src);
        out[w++] = static_cast<uint32_t>(k.dst >> 32);
        out[w++] = static_cast<uint32_t>(k.dst);
        out[w++] = k.bytes;    // PITCH_IN
        out[w++] = k.bytes;    // PITCH_OUT
        out[w++] = k.bytes;    // LINE_LENGTH_IN
        out[w++] = 1;          // LINE_COUNT
        out[w++] = ceMethod(kCeLaunchDma, 1);
        out[w++] = kCeLaunchPhysCopy;
    }
    return w;
}

// 0.147.1: fill VRAM (physical, local FB) with a 32-bit value through the
// remap constant, for zeroing new objects (a fresh object must never show a
// previous owner's data, as kernel drivers on Linux guarantee). Lines of at
// most 2^15 4-byte elements, as NVMTLGsp's virtual fill does.
constexpr uint32_t kCeSetRemapConstA = 0x700;
constexpr uint32_t kCeSetRemapComponents = 0x708;
constexpr uint32_t kCeRemapAllConstA4B = 0x4444u | (3u << 16);
constexpr uint32_t kCeLaunchPhysFill = 0x2u | (1u << 2) | (1u << 7) | (1u << 8) |
                                       (1u << 9) | (1u << 10) | (1u << 13);
constexpr uint32_t kCeFillWordsPerLaunch = 12;

inline uint32_t buildPhysFill(uint64_t dst, uint64_t bytes, uint32_t value,
                              uint32_t *out, uint32_t max) {
    if (!out || !bytes || (bytes & 3) || (dst & 3) || ((dst + bytes) >> 49) || max < 8) return 0;
    uint32_t w = 0;
    out[w++] = ceMethod(kCeSetObject, 1);
    out[w++] = kCeClassAda;
    out[w++] = ceMethod(kCeSetRemapConstA, 1);
    out[w++] = value;
    out[w++] = ceMethod(kCeSetRemapComponents, 1);
    out[w++] = kCeRemapAllConstA4B;
    constexpr uint64_t kMaxDim = 1u << 15;
    uint64_t d = dst, left = bytes;
    while (left) {
        if (w + kCeFillWordsPerLaunch > max) return 0;
        uint64_t width, height;
        if (left >= kMaxDim * kMaxDim * 4) width = height = kMaxDim;
        else if (left >= kMaxDim * 4) { width = kMaxDim; height = left / (kMaxDim * 4); }
        else { width = left / 4; height = 1; }
        out[w++] = ceMethod(kCeSetSrcPhysMode + 4, 1);   // SET_DST_PHYS_MODE
        out[w++] = kCePhysLocalFb;
        out[w++] = ceMethod(kCeOffsetInUpper + 8, 2);    // OFFSET_OUT
        out[w++] = static_cast<uint32_t>(d >> 32);
        out[w++] = static_cast<uint32_t>(d);
        out[w++] = ceMethod(kCeOffsetInUpper + 0x14, 1); // PITCH_OUT
        out[w++] = static_cast<uint32_t>(width * 4);
        out[w++] = ceMethod(kCeOffsetInUpper + 0x18, 2); // LINE_LENGTH_IN, LINE_COUNT
        out[w++] = static_cast<uint32_t>(width);
        out[w++] = static_cast<uint32_t>(height);
        out[w++] = ceMethod(kCeLaunchDma, 1);
        out[w++] = kCeLaunchPhysFill;
        d += width * height * 4;
        left -= width * height * 4;
    }
    return w;
}

// Chunks needed to save `bytes` of VRAM (objects are 2 MiB multiples).
inline uint32_t evictChunks(uint64_t bytes) {
    return static_cast<uint32_t>((bytes + kEvictChunkBytes - 1) / kEvictChunkBytes);
}

}  // namespace nvgsp
