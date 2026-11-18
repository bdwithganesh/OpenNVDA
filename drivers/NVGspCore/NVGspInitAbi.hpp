#pragma once
#include <stddef.h>
#include <stdint.h>
namespace nvgsp {
constexpr uint32_t kGspQueueBytes = 0x40000;
constexpr uint32_t kGspQueuePageTableBytes = 0x1000;
constexpr uint32_t kGspQueuePages = 129;
constexpr uint32_t kGspSharedBytes = kGspQueuePageTableBytes + 2 * kGspQueueBytes;
// Keep the original first five diagnostic indices. 615 adds mandatory
// debugger/root/state-monitor mappings; LOGINIT must remain first.
struct GspLogSpec { const char *id; uint32_t bytes; };
constexpr uint32_t kGspLogCount = 8;
constexpr uint32_t kGspRmStateMonitorBytes = 4096;
constexpr GspLogSpec kGspLogs[kGspLogCount] = {
    {"LOGINIT", 0x10000}, {"LOGINTR", 0x10000}, {"LOGRM", 0x10000},
    {"LOGMNOC", 0x10000}, {"LOGKRNL", 0x10000}, {"LOGDBG", 0x10000},
    {"LOGROOT", 0x1000}, {"LOGRMON", 0x1000}
};

// 615.78.05 GSP msgq v2.  The queue header no longer carries write/read
// pointers: NV_PGSP_QUEUE_HEAD/TAIL(0) move the command queue and
// NV_PGSP_MSGQ_HEAD/TAIL(0) move the status queue.
constexpr uint32_t kMsgqVersionMajor = 2;
constexpr uint32_t kMsgqVersionMinor = 0;
constexpr uint32_t kMsgqMsgSize = 4096;
constexpr uint32_t kMsgqEntryOff = 4096;
constexpr uint32_t kMsgqMsgCount = (kGspQueueBytes - kMsgqEntryOff) / kMsgqMsgSize;

// 615 GSP_MSG_QUEUE_ELEMENT without confidential-compute encryption:
// MCTP header (16 B) + NVDM size/reserved (8 B), then the RPC message.
constexpr uint32_t kGspElementHdrSize = 24;
constexpr uint32_t kMctpMagic = 0x4D435450;      // "MCTP"
constexpr uint32_t kMctpMagicSeen = 0x5345454E;  // "SEEN" (stale)
constexpr uint32_t kMctpHeader = 0xC0000001;     // SOM|EOM|version 1
constexpr uint32_t kNvdmTypeRmRpc = 0x25;
constexpr uint32_t kNvdmHeader = 0x7E | (0x10DE << 8) | (kNvdmTypeRmRpc << 24);

struct MessageQueueInitArguments {
    uint64_t flags;
    uint64_t sharedMemPhysAddr;
    uint64_t cmdQueueOffset, statQueueOffset;
    uint32_t queueElementHdrSize;
    uint32_t queueElementSizeMin, queueElementSizeMax;
    uint32_t queueHeaderAlign, queueElementAlign;
    uint32_t pageTableEntryCount;
    uint8_t reserved[8];
};
struct GspSrInitArguments { uint32_t oldLevel, flags; uint8_t reserved[8]; };
struct GspArgumentsCached {
    uint32_t magic;
    uint16_t size;
    uint8_t reserved[2];
    uint64_t flags;
    MessageQueueInitArguments messageQueue;
    GspSrInitArguments sr;
    uint8_t reserved2[4];
    uint32_t gpuInstance;
    struct { uint64_t pa, size; } profilerArgs;
    struct { uint64_t pa, size; } sysmemHeapArgs;
    struct { uint64_t pa, size; } rmStateMonitorBufferArgs;
    struct { uint64_t radix3, size; } bindataArgs;
    struct { uint64_t pa, size; } vbiosOverrideArgs;
};
struct LibosMemoryRegionInitArgument { uint64_t id8, pa, size; uint8_t kind, loc; uint8_t padding[6]; };
struct MsgqTxHeaderV2 {
    uint16_t versionMajor, versionMinor;
    uint32_t size, msgSize, msgCount, entryOff;
    uint32_t reserved[3];
};
// Driver-side view of the status queue (msgq v2 fields plus the derived
// head/tail slots the phase machine already consumes).
struct MsgqTxHeader { uint32_t version, size, msgSize, msgCount; uint32_t writePtr, flags, rxHdrOff, entryOff; };
struct GspQueueElementHeader {
    uint32_t mctpMagic;
    uint32_t mctpPayloadSize;
    uint32_t mctpHeader;
    uint32_t nvdmHeader;
    uint32_t nvdmPayloadSize;
    uint32_t reserved;
};
struct RpcMessageHeader {
    uint32_t headerVersion, signature, length, function;
    uint32_t result, privateResult, sequence, spare;
};
struct GspSystemInfoParameters {
    uint64_t bar0, bar1, bar2, domainBusDevice;
    uint64_t consoleMemSize, maxUserVa;
    uint32_t pciDeviceId, pciSubDeviceId, pciRevisionId, pcieLinkCap;
    uint8_t gpuBehindBridge, flrSupported, bar0Is64Bit, mnocAvailable;
    uint8_t isPrimary, routeDisplayInterruptsToCpu;
    uint32_t hypervisorType = 4; // NVIDIA OS_HYPERVISOR_UNKNOWN
    uint32_t virtualConfigBits = 0;
    uint8_t isPassthru = 0;
};
struct PackedRegistryTable { uint32_t size, numEntries; };
constexpr uint32_t kGspSystemInfoBytes = 928;
struct GspStatusRecordSummary {
    uint32_t ringIndex, queueSequence, elementCount, checksum;
    uint32_t headerVersion, signature, length, function;
    uint32_t result, privateResult, rpcSequence;
};
static_assert(sizeof(MessageQueueInitArguments) == 64, "GSP MQ ABI");
static_assert(sizeof(GspSrInitArguments) == 16, "GSP SR ABI");
static_assert(sizeof(GspArgumentsCached) == 184, "GSP args ABI");
static_assert(offsetof(GspArgumentsCached, messageQueue) == 16, "GSP MQ offset");
static_assert(offsetof(GspArgumentsCached, sr) == 80, "GSP SR offset");
static_assert(offsetof(GspArgumentsCached, gpuInstance) == 100, "GSP gpuInstance offset");
static_assert(offsetof(GspArgumentsCached, profilerArgs) == 104, "GSP profiler offset");
static_assert(offsetof(GspArgumentsCached, rmStateMonitorBufferArgs) == 136, "GSP state monitor offset");
static_assert(offsetof(GspArgumentsCached, bindataArgs) == 152, "GSP bindata offset");
static_assert(offsetof(GspArgumentsCached, vbiosOverrideArgs) == 168, "GSP vbios offset");
static_assert(sizeof(LibosMemoryRegionInitArgument) == 32, "LibOS region ABI");
static_assert(sizeof(MsgqTxHeaderV2) == 32, "msgq v2 header ABI");
static_assert(sizeof(MsgqTxHeader) == 32, "msgq status view ABI");
static_assert(sizeof(GspQueueElementHeader) == 24, "GSP element header ABI");
static_assert(sizeof(RpcMessageHeader) == 32, "GSP RPC header ABI");
static_assert(sizeof(PackedRegistryTable) == 8, "packed registry ABI");
static_assert(sizeof(GspStatusRecordSummary) == 44, "GSP status summary ABI");
inline GspArgumentsCached buildGspArguments(uint64_t queueRootBus,
                                           uint64_t bindataRadix = 0,
                                           uint64_t bindataBytes = 0,
                                           uint64_t stateMonitorBus = 0) {
    GspArgumentsCached args{};
    // NVIDIA GSP_ARGUMENTS_MAGIC_VALUE; bytes in memory must be "GSP ".
    args.magic = uint32_t(' ') << 24 | uint32_t('P') << 16 |
                 uint32_t('S') << 8 | uint32_t('G');
    args.size = sizeof(args);
    args.flags = 0x02; // GSP_ARGUMENTS_FLAG_STACK_IN_DMEM
    args.messageQueue.sharedMemPhysAddr = queueRootBus;
    args.messageQueue.cmdQueueOffset = kGspQueuePageTableBytes;
    args.messageQueue.statQueueOffset = kGspQueuePageTableBytes + kGspQueueBytes;
    args.messageQueue.queueElementHdrSize = kGspElementHdrSize;
    args.messageQueue.queueElementSizeMin = kMsgqMsgSize;
    args.messageQueue.queueElementSizeMax = kMsgqMsgSize * 16;
    args.messageQueue.queueHeaderAlign = 4;
    args.messageQueue.queueElementAlign = 12;
    args.messageQueue.pageTableEntryCount = kGspQueuePages;
    args.bindataArgs.radix3 = bindataRadix;
    args.bindataArgs.size = bindataBytes;
    args.rmStateMonitorBufferArgs.pa = stateMonitorBus;
    args.rmStateMonitorBufferArgs.size = stateMonitorBus ? kGspRmStateMonitorBytes : 0;
    return args;
}
inline uint64_t initArgId(const char *name) { if (!name) return 0; uint64_t id=0; for(unsigned i=0;i<8&&name[i];++i) id=(id<<8)|static_cast<uint8_t>(name[i]); return id; }
} // namespace nvgsp
