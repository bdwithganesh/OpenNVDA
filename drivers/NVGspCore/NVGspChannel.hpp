#pragma once

// Ampere GPFIFO channel allocation ABI for AD103 bare-metal GSP client.
// Field order, sizes and constants follow MIT-licensed upstream
// NVIDIA 570.144:
//   src/common/sdk/nvidia/inc/alloc/alloc_channel.h  (NV_CHANNEL_ALLOC_PARAMS)
//   src/common/sdk/nvidia/inc/class/clc56f.h          (AMPERE_CHANNEL_GPFIFO_A)
//   src/common/sdk/nvidia/inc/class/cl2080_notification.h (NV2080_ENGINE_TYPE_*)
//   src/common/unix/nvidia-push/src/nvidia-push-init.c (AllocChannelObject)
//
// Host-only header: no IOKit, no hardware access. The kext includes this
// for compile-time layout checks; live channel allocation RPC is a later
// block and must prove backing DMA objects + clean alloc/free lifecycle
// before any deferred method 0x0200 submission (see WORKLOG 23 Sep).
#include <stddef.h>
#include <stdint.h>

namespace nvgsp {

constexpr uint32_t kAmpereChannelGpfifoA = 0xc56f;
constexpr uint32_t kChannelAllocMessageId = 0x906f;
constexpr uint32_t kContextDma = 0x2;  // NV01_CONTEXT_DMA
// NV_CONTEXT_DMA_ALLOCATION_PARAMS flags (DRF positions from nvos.h):
// MAPPING field bit 20 = KERNEL(1); HASH_TABLE field bit 29 = DISABLE(1).
constexpr uint32_t kCtxDmaFlagsKernelNoHash = 0x20100000;
// PB ctxdma flags (nvkms-exact): ACCESS READ_WRITE(0) + HASH_TABLE DISABLE(1).
constexpr uint32_t kCtxDmaFlagsRwNoHash = 0x20000000;
// NVDisplay classes (Ada): NVC770_DISPLAY parent, NVC77D core channel.
constexpr uint32_t kDispDisplay = 0xc770;
constexpr uint32_t kDispCoreChannelDma = 0xc77d;
constexpr uint32_t kDispSetChannelPushbuffer = 0x20800a58;

// NV2080 engine types (subset used for channel binding).
constexpr uint32_t kEngineTypeNull = 0x00000000;
constexpr uint32_t kEngineTypeGraphics = 0x00000001;  // GR0
constexpr uint32_t kEngineTypeCopy0 = 0x00000009;

// NVOS04 channel flags: zero is the correct default for a physical,
// non-VPR, non-CC, non-delayed channel (see alloc_channel.h).
constexpr uint32_t kChannelFlagsPhysicalDefault = 0x00000000;
// CPU-RM assigns channel id 3 in the isolated client tree used by the live
// probe. Its split-GSP path then fixes the USERD page, leaves INDEX_FIXED
// false, and places slot 3 in bits 10:8.
constexpr uint32_t kChannelFlagsUserdPageSlot3 = 0x00200300;
// ERROR_NOTIFIER_TYPE_NONE (bits 3:2) plus ECC_ERROR_NOTIFIER_TYPE_NONE
// (bits 5:4). Privilege USER occupies zero in bits 1:0.
constexpr uint32_t kChannelInternalFlagsNotifierNone = 0x00000014;

constexpr uint32_t kMaxSubdevices = 8;

struct NvMemoryDescParams {
    uint64_t base;
    uint64_t size;
    uint32_t addressSpace;
    uint32_t cacheAttrib;
};

static_assert(sizeof(NvMemoryDescParams) == 24,
              "NV_MEMORY_DESC_PARAMS must be 24 bytes");

struct NvChannelAllocParams {
    uint32_t hObjectError;    // error context DMA (from notifiers)
    uint32_t hObjectBuffer;   // legacy: VASpace ctx DMA when hVASpace==0
    uint64_t gpFifoOffset;    // GPU-VA offset to GPFIFO start (ctx-DMA relative)
    uint32_t gpFifoEntries;   // number of GPFIFO entries
    uint32_t flags;           // NVOS04_FLAGS_* (0 = physical default)
    uint32_t hContextShare;   // context share handle (0 when unused)
    uint32_t hVASpace;        // VASpace for the channel (proven FERMI_VASPACE_A)
    uint32_t hUserdMemory[kMaxSubdevices];  // USERD memory object per subdevice
    uint64_t userdOffset[kMaxSubdevices];   // USERD offset within object
    uint32_t engineType;      // NV2080_ENGINE_TYPE_* (1 = GRAPHICS)
    uint32_t cid;             // RM-session-unique channel id (filled by RM)
    uint32_t subDeviceId;     // one-hot subdevice mask (0 = default)
    uint32_t hObjectEccError;  // ECC error context DMA (0 when unused)
    NvMemoryDescParams instanceMem;   // RM-filled: channel instance
    NvMemoryDescParams userdMem;      // RM-filled: USERD mapping
    NvMemoryDescParams ramfcMem;      // RM-filled: RAMFC
    NvMemoryDescParams mthdbufMem;    // RM-filled: method buffer
    uint32_t hPhysChannelGroup;       // reserved (0)
    uint32_t internalFlags;           // reserved (0)
    NvMemoryDescParams errorNotifierMem;     // reserved (zero)
    NvMemoryDescParams eccErrorNotifierMem;  // reserved (zero)
    uint32_t processId;                 // reserved (0)
    uint32_t subProcessId;              // reserved (0)
    uint32_t encryptIv[3];              // reserved (zero)
    uint32_t decryptIv[3];              // reserved (zero)
    uint32_t hmacNonce[8];              // reserved (zero)
    uint32_t tpcConfigId;               // TPC config (0 = default)
};

static_assert(sizeof(NvChannelAllocParams) == 368,
              "NV_CHANNEL_ALLOC_PARAMS must be 368 bytes");
static_assert(offsetof(NvChannelAllocParams, hObjectError) == 0,
              "hObjectError offset");
static_assert(offsetof(NvChannelAllocParams, hObjectBuffer) == 4,
              "hObjectBuffer offset");
static_assert(offsetof(NvChannelAllocParams, gpFifoOffset) == 8,
              "gpFifoOffset offset");
static_assert(offsetof(NvChannelAllocParams, gpFifoEntries) == 16,
              "gpFifoEntries offset");
static_assert(offsetof(NvChannelAllocParams, flags) == 20, "flags offset");
static_assert(offsetof(NvChannelAllocParams, hContextShare) == 24,
              "hContextShare offset");
static_assert(offsetof(NvChannelAllocParams, hVASpace) == 28, "hVASpace offset");
static_assert(offsetof(NvChannelAllocParams, hUserdMemory) == 32,
              "hUserdMemory offset");
static_assert(offsetof(NvChannelAllocParams, userdOffset) == 64,
              "userdOffset offset");
static_assert(offsetof(NvChannelAllocParams, engineType) == 128,
              "engineType offset");
static_assert(offsetof(NvChannelAllocParams, cid) == 132, "cid offset");
static_assert(offsetof(NvChannelAllocParams, subDeviceId) == 136,
              "subDeviceId offset");
static_assert(offsetof(NvChannelAllocParams, hObjectEccError) == 140,
              "hObjectEccError offset");
static_assert(offsetof(NvChannelAllocParams, instanceMem) == 144,
              "instanceMem offset");
static_assert(offsetof(NvChannelAllocParams, userdMem) == 168,
              "userdMem offset");
static_assert(offsetof(NvChannelAllocParams, ramfcMem) == 192,
              "ramfcMem offset");
static_assert(offsetof(NvChannelAllocParams, mthdbufMem) == 216,
              "mthdbufMem offset");
static_assert(offsetof(NvChannelAllocParams, hPhysChannelGroup) == 240,
              "hPhysChannelGroup offset");
static_assert(offsetof(NvChannelAllocParams, internalFlags) == 244,
              "internalFlags offset");
static_assert(offsetof(NvChannelAllocParams, errorNotifierMem) == 248,
              "errorNotifierMem offset");
static_assert(offsetof(NvChannelAllocParams, eccErrorNotifierMem) == 272,
              "eccErrorNotifierMem offset");
static_assert(offsetof(NvChannelAllocParams, processId) == 296,
              "processId offset");
static_assert(offsetof(NvChannelAllocParams, subProcessId) == 300,
              "subProcessId offset");
static_assert(offsetof(NvChannelAllocParams, encryptIv) == 304,
              "encryptIv offset");
static_assert(offsetof(NvChannelAllocParams, decryptIv) == 316,
              "decryptIv offset");
static_assert(offsetof(NvChannelAllocParams, hmacNonce) == 328,
              "hmacNonce offset");
static_assert(offsetof(NvChannelAllocParams, tpcConfigId) == 360,
              "tpcConfigId offset");

// Backing-object plan for the safe next live block (no RPC yet):
// GPFIFO and USERD must exist as RM memory objects before channel alloc.
// Sizes follow nvidia-push defaults: GPFIFO entries are 8 bytes each;
// USERD is one 4 KiB page (512 B per channel slot, 8 slots per page).
// The live block must allocate these with the already-proven
// NV01_MEMORY_LOCAL_USER path (function 103 + function 10 free) and prove
// clean teardown BEFORE any 0xc56f channel RPC is attempted.
constexpr uint32_t kGpfifoEntryBytes = 8;
constexpr uint32_t kMinGpfifoEntries = 256;  // 2 KiB FIFO, push-validated minimum
constexpr uint64_t kUserdBytes = 4096;

// Build a zeroed channel-alloc payload with only caller-owned fields set.
// RM fills cid + instance/userd/ramfc/mthdbuf descriptors on success.
// Returns false on bad handles/sizes; never touches hardware.
inline bool buildChannelAllocParams(uint32_t vaspaceHandle,
                                    uint32_t userdMemoryHandle,
                                    uint64_t gpFifoOffset,
                                    uint32_t gpFifoEntries,
                                    uint32_t engineType,
                                    NvChannelAllocParams *out) {
    if (!out || !vaspaceHandle || gpFifoEntries < kMinGpfifoEntries ||
        (gpFifoEntries & 1U))
        return false;
    // 0.116.0 (V1): + NVDEC0 0x13, NVENC0 0x1b, OFA0 0x33
    if (engineType != kEngineTypeGraphics && engineType != kEngineTypeCopy0 &&
        engineType != kEngineTypeNull && engineType != 0x13 &&
        engineType != 0x1b && engineType != 0x33)
        return false;
    NvChannelAllocParams params{};
    params.hVASpace = vaspaceHandle;
    // Single-GPU client: only subdevice 0 owns a USERD handle. CPU-RM copies
    // the full array to GSP, leaving absent subdevices zero.
    if (userdMemoryHandle) {
        params.hUserdMemory[0] = userdMemoryHandle;
        params.userdOffset[0] = 0;
    }
    params.gpFifoOffset = gpFifoOffset;
    params.gpFifoEntries = gpFifoEntries;
    params.flags = kChannelFlagsPhysicalDefault;
    params.engineType = engineType;
    *out = params;
    return true;
}

// NV_CONTEXT_DMA_ALLOCATION_PARAMS (nvos.h): hSubDevice, flags, hMemory,
// then 8-aligned offset/limit. Upstream zero-inits and sets only hMemory,
// flags (KERNEL mapping + HASH_TABLE DISABLE), offset 0 and limit.
struct NvCtxDmaAllocParams {
    uint32_t hSubDevice;
    uint32_t flags;
    uint32_t hMemory;
    uint32_t reservedPad;
    uint64_t offset;
    uint64_t limit;
};

static_assert(sizeof(NvCtxDmaAllocParams) == 32,
              "NV_CONTEXT_DMA_ALLOCATION_PARAMS must be 32 bytes");
static_assert(offsetof(NvCtxDmaAllocParams, hSubDevice) == 0,
              "hSubDevice offset");
static_assert(offsetof(NvCtxDmaAllocParams, flags) == 4, "ctxdma flags offset");
static_assert(offsetof(NvCtxDmaAllocParams, hMemory) == 8,
              "ctxdma hMemory offset");
static_assert(offsetof(NvCtxDmaAllocParams, offset) == 16,
              "ctxdma offset offset");
static_assert(offsetof(NvCtxDmaAllocParams, limit) == 24,
              "ctxdma limit offset");

// NV50VAIO_CHANNELDMA_ALLOCATION_PARAMETERS (nvos.h:2483-2500): 40 bytes
// (36 used + 4 tail pad from pControl's 8 B alignment). nvkms sets only
// channelInstance, hObjectBuffer and offset; pControl is OUT (host VA,
// meaningless over GSP-RPC — ignored).
struct NvDispChannelDmaAllocParams {
    uint32_t channelInstance;
    uint32_t hObjectBuffer;
    uint32_t hObjectNotify;
    uint32_t offset;
    uint64_t pControl;
    uint32_t flags;
    uint32_t channelPBSize;
    uint32_t subDeviceId;
    uint32_t reservedPad;
};

static_assert(sizeof(NvDispChannelDmaAllocParams) == 40,
              "display channel alloc params must be 40 bytes");
static_assert(offsetof(NvDispChannelDmaAllocParams, channelInstance) == 0,
              "disp instance offset");
static_assert(offsetof(NvDispChannelDmaAllocParams, hObjectBuffer) == 4,
              "disp hObjectBuffer offset");
static_assert(offsetof(NvDispChannelDmaAllocParams, hObjectNotify) == 8,
              "disp hObjectNotify offset");
static_assert(offsetof(NvDispChannelDmaAllocParams, offset) == 12,
              "disp offset offset");
static_assert(offsetof(NvDispChannelDmaAllocParams, pControl) == 16,
              "disp pControl offset");
static_assert(offsetof(NvDispChannelDmaAllocParams, flags) == 24,
              "disp flags offset");
static_assert(offsetof(NvDispChannelDmaAllocParams, channelPBSize) == 28,
              "disp channelPBSize offset");
static_assert(offsetof(NvDispChannelDmaAllocParams, subDeviceId) == 32,
              "disp subDeviceId offset");

// NV2080_CTRL_INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER_PARAMS. In NVIDIA's
// split-RM display path CPU-RM sends this control to the physical internal
// subdevice; the C77D resource remains host-owned because its constructor
// maps the channel's user registers into the host process. A bare GSP client
// therefore must program the physical channel with this ABI instead of asking
// GSP-RM to perform the host-side C77D allocation/map.
struct NvDispChannelPushbufferParams {
    uint32_t addressSpace;
    uint32_t reservedPad0;
    uint64_t physicalAddr;
    uint64_t limit;
    uint32_t cacheSnoop;
    uint32_t hclass;
    uint32_t channelInstance;
    uint8_t valid;
    uint8_t reservedPad1[3];
    uint32_t pbTargetAperture;
    uint32_t channelPBSize;
    uint32_t subDeviceId;
    uint32_t reservedPad2;
};

constexpr uint32_t kAddressSpaceFbmem = 2;
constexpr uint32_t kPbTargetPhysNvm = 1;
constexpr uint32_t kChannelPbSize4K = 0;

static_assert(sizeof(NvDispChannelPushbufferParams) == 56,
              "display pushbuffer control params must be 56 bytes");
static_assert(offsetof(NvDispChannelPushbufferParams, addressSpace) == 0,
              "display pushbuffer addressSpace offset");
static_assert(offsetof(NvDispChannelPushbufferParams, physicalAddr) == 8,
              "display pushbuffer physicalAddr offset");
static_assert(offsetof(NvDispChannelPushbufferParams, limit) == 16,
              "display pushbuffer limit offset");
static_assert(offsetof(NvDispChannelPushbufferParams, cacheSnoop) == 24,
              "display pushbuffer cacheSnoop offset");
static_assert(offsetof(NvDispChannelPushbufferParams, hclass) == 28,
              "display pushbuffer hclass offset");
static_assert(offsetof(NvDispChannelPushbufferParams, channelInstance) == 32,
              "display pushbuffer channelInstance offset");
static_assert(offsetof(NvDispChannelPushbufferParams, valid) == 36,
              "display pushbuffer valid offset");
static_assert(offsetof(NvDispChannelPushbufferParams, pbTargetAperture) == 40,
              "display pushbuffer target offset");
static_assert(offsetof(NvDispChannelPushbufferParams, channelPBSize) == 44,
              "display pushbuffer size offset");
static_assert(offsetof(NvDispChannelPushbufferParams, subDeviceId) == 48,
              "display pushbuffer subdevice offset");

}  // namespace nvgsp
