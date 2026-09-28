#pragma once
#include <stddef.h>
#include <stdint.h>
namespace nvgsp {
constexpr uint32_t kGspQueueBytes = 0x40000;
constexpr uint32_t kGspQueuePageTableBytes = 0x1000;
constexpr uint32_t kGspQueuePages = 129;
constexpr uint32_t kGspSharedBytes = kGspQueuePageTableBytes + 2 * kGspQueueBytes;
struct MessageQueueInitArguments { uint64_t sharedMemPhysAddr; uint32_t pageTableEntryCount; uint32_t padding; uint64_t cmdQueueOffset, statQueueOffset; };
struct GspSrInitArguments { uint32_t oldLevel, flags; uint8_t inPmTransition; uint8_t padding[3]; };
struct GspArgumentsCached { MessageQueueInitArguments messageQueue; GspSrInitArguments sr; uint32_t gpuInstance; uint8_t dmemStack; uint8_t padding[7]; uint64_t profilerPa, profilerSize; };
struct LibosMemoryRegionInitArgument { uint64_t id8, pa, size; uint8_t kind, loc; uint8_t padding[6]; };
struct MsgqTxHeader { uint32_t version, size, msgSize, msgCount; uint32_t writePtr, flags, rxHdrOff, entryOff; };
struct GspQueueElementHeader {
    uint8_t authTag[16], aad[16];
    uint32_t checksum, sequence, elementCount, padding;
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
};
struct PackedRegistryTable { uint32_t size, numEntries; };
constexpr uint32_t kGspSystemInfoBytes = 928;
struct GspStatusRecordSummary {
    uint32_t ringIndex, queueSequence, elementCount, checksum;
    uint32_t headerVersion, signature, length, function;
    uint32_t result, privateResult, rpcSequence;
};
static_assert(sizeof(MessageQueueInitArguments) == 32, "GSP MQ ABI");
static_assert(sizeof(GspArgumentsCached) == 72, "GSP args ABI");
static_assert(sizeof(LibosMemoryRegionInitArgument) == 32, "LibOS region ABI");
static_assert(sizeof(MsgqTxHeader) == 32, "msgq header ABI");
static_assert(sizeof(GspQueueElementHeader) == 48, "GSP queue element ABI");
static_assert(sizeof(RpcMessageHeader) == 32, "GSP RPC header ABI");
static_assert(sizeof(PackedRegistryTable) == 8, "packed registry ABI");
static_assert(sizeof(GspStatusRecordSummary) == 44, "GSP status summary ABI");
inline uint64_t initArgId(const char *name) { if (!name) return 0; uint64_t id=0; for(unsigned i=0;i<8&&name[i];++i) id=(id<<8)|static_cast<uint8_t>(name[i]); return id; }
} // namespace nvgsp
