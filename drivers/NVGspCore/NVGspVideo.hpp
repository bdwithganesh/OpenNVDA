#pragma once

// V1 (0.116.0): video engine channels on AD103 — NVDEC0 (NVC9B0), NVENC0
// (NVC9B7), OFA0 (NVC9FA). Host-only helpers (no IOKit, no hardware):
// the kext uses them to bring a channel up on first use, the host test
// checks every layout.
//
// Sources (MIT, NVIDIA open-gpu-kernel-modules 570.144):
//   class/cl2080_notification.h  NV2080_ENGINE_TYPE_{BSP 0x13, MSENC 0x1b, OFA 0x33}
//   g_eng_desc_nvoc.h             ENG_NVDEC/ENG_NVENC/ENG_OFA = classId << 8 | inst
//                                 (OBJBSP 0x8f99e1, OBJMSENC 0xe97b6c, OBJOFA 0xdd7bab)
//   ctrl2080gpu.h                 GET_CONSTRUCTED_FALCON_INFO 0x208001b0,
//                                 PROMOTE_CTX 0x2080012b
//   nvos.h                        NV_BSP/MSENC/OFA_ALLOCATION_PARAMETERS
//   kernel_falcon.c               GSP client = CPU-RM allocates the falcon
//                                 context buffer (ctxBufferSize, 256 B aligned)
//                                 and promotes it before the engine object.
#include <stddef.h>
#include <stdint.h>

namespace nvgsp {

constexpr uint32_t kEngineTypeNvdec0 = 0x00000013;
constexpr uint32_t kEngineTypeNvenc0 = 0x0000001b;
constexpr uint32_t kEngineTypeOfa0 = 0x00000033;

constexpr uint32_t kCmdGetConstructedFalconInfo = 0x208001b0;
constexpr uint32_t kCmdPromoteCtx = 0x2080012b;
constexpr uint32_t kMaxConstructedFalcons = 0x40;
constexpr uint32_t kFalconInfoBytes = 4 + kMaxConstructedFalcons * 20;   // 1284
constexpr uint32_t kPromoteCtxBytes = 560;   // 48 + 16 entries x 32
constexpr uint32_t kFalconCtxAlign = 256;    // FLCN_BLK_ALIGNMENT

// Engine index used by the user client: execSegments engine = 2 + index.
enum VideoEngineIndex : uint32_t { kVideoNvdec = 0, kVideoNvenc = 1, kVideoOfa = 2 };
constexpr uint32_t kVideoEngineCount = 3;

struct VideoEngineDesc {
    const char *name;
    uint32_t engineType;   // NV2080_ENGINE_TYPE_*
    uint32_t objClass;     // Ada class
    uint32_t engDesc;      // ENGDESCRIPTOR of instance 0 (falcon info key)
    uint32_t handleBase;   // RM handles: base | 0x6f channel, 0x43 USERD,
                           // 0x44 instance, 0x45 method buffer, 0xb0 object
    uint32_t chid;         // CPU-RM chosen channel id (GR 3, CE 4)
};

inline const VideoEngineDesc *videoEngine(uint32_t index) {
    static const VideoEngineDesc kEngines[kVideoEngineCount] = {
        {"nvdec0", kEngineTypeNvdec0, 0xc9b0, 0x8f99e100, 0xc0d20000, 5},
        {"nvenc0", kEngineTypeNvenc0, 0xc9b7, 0xe97b6c00, 0xc0d20100, 6},
        {"ofa0", kEngineTypeOfa0, 0xc9fa, 0xdd7bab00, 0xc0d20200, 7},
    };
    return index < kVideoEngineCount ? &kEngines[index] : nullptr;
}

inline uint32_t videoChannelHandle(const VideoEngineDesc &e) { return e.handleBase | 0x6f; }
inline uint32_t videoBackingHandle(const VideoEngineDesc &e, uint32_t which) {
    return e.handleBase | (0x43 + which);   // 0 USERD, 1 instance, 2 method buffer
}
inline uint32_t videoObjectHandle(const VideoEngineDesc &e) { return e.handleBase | 0xb0; }

// NVOS04 flags: USERD_INDEX_PAGE_FIXED (bit 21) | USERD_INDEX_PAGE_VALUE
// (20:12) | USERD_INDEX_VALUE (10:8): chid = page * 8 + index, as for the GR
// (3) and CE (4) channels. 0.149.0: chid 8 used to spill into INDEX_FIXED.
inline uint32_t videoChannelFlags(const VideoEngineDesc &e) {
    return 0x00200000u | ((e.chid & 7) << 8) | (((e.chid >> 3) & 0x1ff) << 12);
}

// Context buffer size of the falcon `engDesc` in a GET_CONSTRUCTED_FALCON_INFO
// reply (params: u32 count, then {engDesc, ctxAttr, ctxBufferSize,
// addrSpaceList, registerBase} x 64). False = engine not constructed.
inline bool falconCtxBytes(const uint8_t *params, uint32_t bytes, uint32_t engDesc,
                           uint32_t *ctxBytes) {
    if (!params || bytes < 4 || !ctxBytes) return false;
    uint32_t n = 0;
    __builtin_memcpy(&n, params, 4);
    if (n > kMaxConstructedFalcons) return false;
    for (uint32_t i = 0; i < n && 4 + (i + 1) * 20 <= bytes; ++i) {
        uint32_t desc = 0, size = 0;
        __builtin_memcpy(&desc, params + 4 + i * 20, 4);
        __builtin_memcpy(&size, params + 4 + i * 20 + 8, 4);
        if (desc == engDesc) { *ctxBytes = size; return true; }
    }
    return false;
}

// NV2080_CTRL_GPU_PROMOTE_CTX_PARAMS for a falcon context: {engineType,
// hClient, ChID, hChanClient, hObject, hVirtMemory, virtAddress, size,
// entryCount, entries[16] x {gpuPhysAddr, gpuVirtAddr, size, physAttr,
// u16 bufferId, u8 bInitialize, u8 bNonmapped}}, one entry (bufferId 0,
// physAttr VIDMEM | GPU_CACHEABLE_NO = 0x4, initialize).
// 0.117.0 live (0.116.0): the "full" layout was refused with
// NV_ERR_INVALID_STATE, so the kext tries the layouts in this order:
//   kPromoteRmExternal  what RM's kflcn sends for an externally owned VAS
//                       (ours): hClient/ChID/size set, physical entry only,
//                       bNonmapped = 1, no VA (kernel_falcon.c);
//   kPromoteGrStyle     the layout of our working GR promote (nouveau):
//                       top-level ids/size 0, entry with phys + VA;
//   kPromoteFull        everything filled (the 0.116.0 layout).
enum PromoteVariant : uint32_t {
    kPromoteRmExternal = 0,
    kPromoteGrStyle = 1,
    kPromoteFull = 2,
    kPromoteVariants = 3,
};

inline bool buildFalconPromote(uint32_t engineType, uint32_t hClient, uint32_t chid,
                               uint32_t hChannel, uint64_t phys, uint64_t va,
                               uint64_t size, uint8_t *out, uint32_t outBytes,
                               uint32_t variant = kPromoteFull) {
    if (!out || outBytes < kPromoteCtxBytes || !size || (phys | va) % kFalconCtxAlign ||
        variant >= kPromoteVariants)
        return false;
    for (uint32_t i = 0; i < kPromoteCtxBytes; ++i) out[i] = 0;
    const uint32_t one = 1, attr = 0x4;
    const uint16_t id = 0;
    const bool top = variant != kPromoteGrStyle;
    const bool mapped = variant != kPromoteRmExternal;
    const uint64_t entryVa = mapped ? va : 0;
    __builtin_memcpy(out + 0, &engineType, 4);
    if (top) {
        __builtin_memcpy(out + 4, &hClient, 4);
        __builtin_memcpy(out + 8, &chid, 4);
        __builtin_memcpy(out + 32, &size, 8);
    }
    __builtin_memcpy(out + 12, &hClient, 4);
    __builtin_memcpy(out + 16, &hChannel, 4);
    if (variant == kPromoteFull) __builtin_memcpy(out + 24, &va, 8);
    __builtin_memcpy(out + 40, &one, 4);
    uint8_t *e = out + 48;
    __builtin_memcpy(e + 0, &phys, 8);
    __builtin_memcpy(e + 8, &entryVa, 8);
    __builtin_memcpy(e + 16, &size, 8);
    __builtin_memcpy(e + 24, &attr, 4);
    __builtin_memcpy(e + 28, &id, 2);
    e[30] = 1;                  // bInitialize
    e[31] = mapped ? 0 : 1;     // bNonmapped
    return true;
}

// 0.121.0: second promote of an externally owned VAS (NVIDIA
// nvGpuOpsBindChannelResources, nv_gpu_ops.c): after the physical-only
// promote, the context VA is bound with {engineType, hChanClient, hObject,
// entryCount 1, entry.gpuVirtAddr} and everything else 0 (bufferId only
// for GR). Live 0.120.0: without it the engine object alloc failed with
// NV_ERR_OBJECT_NOT_FOUND (no VA for the context in the channel's VAS).
inline bool buildFalconPromoteVa(uint32_t engineType, uint32_t hClient, uint32_t hChannel,
                                 uint64_t va, uint8_t *out, uint32_t outBytes) {
    if (!out || outBytes < kPromoteCtxBytes || !va || va % kFalconCtxAlign) return false;
    for (uint32_t i = 0; i < kPromoteCtxBytes; ++i) out[i] = 0;
    const uint32_t one = 1;
    __builtin_memcpy(out + 0, &engineType, 4);
    __builtin_memcpy(out + 12, &hClient, 4);
    __builtin_memcpy(out + 16, &hChannel, 4);
    __builtin_memcpy(out + 40, &one, 4);
    __builtin_memcpy(out + 48 + 8, &va, 8);
    return true;
}

// NV_BSP / NV_MSENC / NV_OFA_ALLOCATION_PARAMETERS: {size, prohibit
// multiple instances, engineInstance} — identical 12-byte layouts.
inline void buildVideoObjectParams(uint32_t engineInstance, uint8_t out[12]) {
    const uint32_t v[3] = {12, 0, engineInstance};
    __builtin_memcpy(out, v, 12);
}

// Chunk layout of one video channel (2 MiB VRAM, GPU VA = VRAM window + phys):
//   +0x000000 GPFIFO (512 entries)   +0x001000 fence semaphore
//   +0x010000 tail pushbuffer (64 KiB, kext-appended fence methods)
//   +0x100000 falcon context buffer (<= 1 MiB; larger = chunk grows,
//             context at +0x200000)
constexpr uint64_t kVideoGpfifoOff = 0x0;
constexpr uint64_t kVideoSemOff = 0x1000;
constexpr uint64_t kVideoPbOff = 0x10000;
constexpr uint64_t kVideoPbBytes = 0x10000;
inline uint64_t videoCtxOffset(uint32_t ctxBytes) {
    return ctxBytes <= 0x100000 ? 0x100000 : 0x200000;
}
inline uint64_t videoChunkBytes(uint32_t ctxBytes) {
    const uint64_t end = videoCtxOffset(ctxBytes) + ctxBytes;
    return (end + 0x1fffff) & ~0x1fffffULL;
}

// Engine-class methods for a pushbuffer (all three classes share the
// falcon "common" method block: SEMAPHORE_A/B/C 0x240/0x244/0x248,
// SEMAPHORE_D 0x304 with OPERATION RELEASE, STRUCTURE_SIZE ONE).
constexpr uint32_t kVideoMthdNop = 0x100;
constexpr uint32_t kVideoMthdSemA = 0x240;
constexpr uint32_t kVideoMthdExecute = 0x300;
constexpr uint32_t kVideoMthdSemD = 0x304;

// Incrementing method header (SEC_OP INC_METHOD 1 << 29).
inline uint32_t pbInc(uint32_t subch, uint32_t mthd, uint32_t count) {
    return (1u << 29) | (count << 16) | (subch << 13) | (mthd >> 2);
}

// SET_OBJECT(class) + engine semaphore release of `payload` at `va`
// (SEMAPHORE_D: OPERATION RELEASE, STRUCTURE_SIZE ONE = 32-bit payload,
// flush enabled). Returns words written (8), 0 on bad arguments.
constexpr uint32_t kVideoSemReleaseWords = 8;
inline uint32_t buildVideoSemRelease(uint32_t subch, uint32_t objClass, uint64_t va,
                                     uint32_t payload, uint32_t *out, uint32_t max) {
    if (!out || max < kVideoSemReleaseWords || (va & 3) || subch > 7) return 0;
    out[0] = pbInc(subch, 0x0, 1);            // host SET_OBJECT
    out[1] = objClass;
    out[2] = pbInc(subch, kVideoMthdSemA, 3); // SEMAPHORE_A/B/C
    out[3] = static_cast<uint32_t>(va >> 32) & 0xff;
    out[4] = static_cast<uint32_t>(va);
    out[5] = payload;
    out[6] = pbInc(subch, kVideoMthdSemD, 1); // SEMAPHORE_D
    out[7] = 0;                               // RELEASE, ONE, flush, 32-bit
    return kVideoSemReleaseWords;
}

}  // namespace nvgsp
