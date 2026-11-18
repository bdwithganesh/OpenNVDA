#pragma once
#include <stdint.h>
#include <stddef.h>
#include "NVGspControlAbi615.hpp"
#include "NVGspLifecycle615.hpp"
// NVIDIA open-gpu-kernel-modules 615.78.08, commit
// 1a82fbfda8f321b056eb3568850f9316c5e465c5: ctrl2080internal.h/clc369.h.
// Pure ABI builders, sizing and decoding. DMA ownership lives in the service;
// none of these helpers registers buffers or implements fault recovery.
namespace nvgsp {
constexpr uint32_t kGmmuStaticInfoCommand615=0x20800a59;
constexpr uint32_t kFaultRecordBytes615=32;
constexpr uint32_t kShadowFaultMaxPages615=3000;
constexpr uint32_t kShadowFaultRegisterCommand615=0x20800a9d;
struct ShadowFaultRegistration615 {
    uint64_t queueBus;
    uint32_t bufferBytes,metadataBytes;
    uint64_t pageBus[kShadowFaultMaxPages615];
    uint32_t type,padding;
    uint64_t sharedBus;
};
static_assert(sizeof(ShadowFaultRegistration615)==24032,"615 shadow registration ABI");
static_assert(offsetof(ShadowFaultRegistration615,type)==24016,"615 shadow type offset");
static_assert(offsetof(ShadowFaultRegistration615,sharedBus)==24024,"615 shared memory offset");
constexpr uint32_t kShadowFaultRequestBytes615=40+sizeof(ShadowFaultRegistration615);
constexpr uint32_t kShadowFaultReplyBytes615=96+sizeof(ShadowFaultRegistration615);
// NVIDIA Queue on the pinned 64-bit ABI, initialized as non-managed. The two
// pointers are NULL: firmware uses the separately supplied physical pages.
struct alignas(64) ShadowFaultQueue615 {
    uint64_t capacity,allocator,data,msgSize;
    uint8_t padding0[32];
    uint64_t get;
    uint8_t padding1[56];
    uint64_t put;
    uint8_t padding2[56];
};
static_assert(sizeof(ShadowFaultQueue615)==192,"64-bit non-managed fault queue ABI");
static_assert(offsetof(ShadowFaultQueue615,get)==64 && offsetof(ShadowFaultQueue615,put)==128,"queue cacheline indices");
inline bool faultBus615(uint64_t bus) {return bus && !(bus&4095) && bus<(1ULL<<48);}
inline bool buildShadowFaultRegistration615(ShadowFaultRegistration615 *p,
        uint64_t queueBus,uint32_t bufferBytes,const uint64_t *pages,uint32_t pageCount) {
    if(!p)return false;
    __builtin_memset(p,0,sizeof(*p));
    if(!pages || !faultBus615(queueBus) || bufferBytes<64 || bufferBytes%32 ||
       bufferBytes>kShadowFaultMaxPages615*4096U ||
       pageCount!=(bufferBytes+4095U)/4096U)return false;
    for(uint32_t i=0;i<pageCount;++i) {
        if(!faultBus615(pages[i]) || pages[i]==queueBus)return false;
        for(uint32_t j=0;j<i;++j)if(pages[i]==pages[j])return false;
    }
    p->queueBus=queueBus;p->bufferBytes=bufferBytes;
    __builtin_memcpy(p->pageBus,pages,pageCount*sizeof(uint64_t));
    return true;
}
inline bool nativeShadowFaultRequest615(uint32_t function,const void *payload,size_t bytes) {
    if(!payload || function!=76 || bytes!=kShadowFaultRequestBytes615)return false;
    ControlHeader615 h{};__builtin_memcpy(&h,payload,sizeof(h));
    return h.client==kLifecycleClient615 && h.object==kLifecycleSubdevice615 &&
        h.command==kShadowFaultRegisterCommand615 && h.paramsBytes==sizeof(ShadowFaultRegistration615) &&
        !h.status && !h.rpcFlags && !h.controlFlags && !h.accessRight && !h.reserved;
}
template <typename Page>
inline bool copyRpcReplyPages615(void *out,size_t capacity,uint32_t bytes,
        uint32_t read,uint32_t ringCount,Page page) {
    if(!out || !bytes || bytes>capacity || bytes>16U*4096U || !ringCount ||
       read>=ringCount || (bytes+4095U)/4096U>ringCount)return false;
    auto *target=static_cast<uint8_t *>(out);
    for(uint32_t offset=0;offset<bytes;) {
        const uint32_t slot=(read+offset/4096)%ringCount;
        const uint8_t *part=page(slot);
        const uint32_t take=bytes-offset<4096 ? bytes-offset : 4096;
        if(!part)return false;
        __builtin_memcpy(target+offset,part,take);offset+=take;
    }
    return true;
}
struct GmmuStaticInfo615 {
    uint32_t replayableBytes,replayableMetadataBytes;
    uint32_t nonReplayableBytes,nonReplayableMetadataBytes;
};
static_assert(sizeof(GmmuStaticInfo615)==16,"615 GMMU static info ABI");
// ctrl2080internal.h GMMU_REGISTER_FAULT_BUFFER: CPU-RM pages of the hardware
// REPLAYABLE buffer. Non-CC places it in coherent sysmem (kgmmuFaultBuffer-
// GetAddressSpace), i.e. our own DMA pages.
constexpr uint32_t kReplayableFaultRegisterCommand615=0x20800a9b;
struct ReplayableFaultRegistration615 {
    uint32_t hClient,hObject,faultBufferSize,padding;
    uint64_t pages[256];
};
static_assert(sizeof(ReplayableFaultRegistration615)==2064,"615 replayable registration ABI");
inline bool replayableFaultPlan615(const GmmuStaticInfo615 &info,uint32_t *pages) {
    if(!pages)return false;
    *pages=0;
    if(info.replayableMetadataBytes || info.replayableBytes<64 ||
       info.replayableBytes%kFaultRecordBytes615 || info.replayableBytes>256U*4096U)return false;
    *pages=(info.replayableBytes+4095U)/4096U;
    return true;
}
// For the non-CC AD103 path. Never interpret CC authenticated packets here.
inline bool nonReplayableFaultPlan615(const GmmuStaticInfo615 &info,
                                      uint32_t *pages,uint32_t *entries) {
    if(!pages || !entries)return false;
    *pages=*entries=0;
    if(info.nonReplayableMetadataBytes || info.nonReplayableBytes<64 ||
       info.nonReplayableBytes%kFaultRecordBytes615 ||
       info.nonReplayableBytes>kShadowFaultMaxPages615*4096U)return false;
    *pages=(info.nonReplayableBytes+4095U)/4096U;
    // The non-managed circular queue wastes one slot (NVIDIA queue.c).
    *entries=info.nonReplayableBytes/kFaultRecordBytes615-1;
    return true;
}
struct FaultRecord615 {
    uint64_t instancePageAddress,faultPageAddress,timestamp;
    uint32_t instanceAperture,addressPhysicalAperture,engineId,faultType;
    uint32_t clientId,accessType,mmuClientType,gpcId;
    bool replayable,protectedMode,replayableEnabled;
};
// Pinned GV100 fault enums used by NVIDIA's inherited GMMU decoder on Ada.
constexpr uint32_t kFaultPde615=0,kFaultReadOnlyViolation615=6,kFaultVirtualWrite615=1;
inline bool ownedWriteFault615(const FaultRecord615 &r,uint64_t instance,uint64_t page,uint32_t type) {
    return instance && page && !((instance|page)&4095) &&
        r.instancePageAddress==instance && r.faultPageAddress==page &&
        r.faultType==type && r.accessType==kFaultVirtualWrite615 && !r.replayable;
}
inline bool ownedReadOnlyWriteFault615(const FaultRecord615 &r,uint64_t instance,uint64_t page) {
    return ownedWriteFault615(r,instance,page,kFaultReadOnlyViolation615);
}
// Fixed mode5: A's write to B's VA finds no PDE in A's private tree.
inline bool ownedCrossVasWriteFault615(const FaultRecord615 &r,uint64_t instance,uint64_t page) {
    return ownedWriteFault615(r,instance,page,kFaultPde615);
}
inline uint32_t faultWord615(const uint8_t *p) {
    return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24;
}
// Accept precisely one 32-byte clc369 packet. A 64-byte GMC notification is
// a different protocol and must never be passed off as an address record.
inline bool decodeFaultRecord615(const void *data,size_t bytes,FaultRecord615 *out) {
    if(!out)return false;
    *out={};
    if(!data || bytes!=kFaultRecordBytes615)return false;
    const auto *p=static_cast<const uint8_t *>(data);
    const uint32_t flags=faultWord615(p+28);
    if(!(flags&0x80000000U))return false;
    const uint32_t inst=faultWord615(p),addr=faultWord615(p+8);
    out->instancePageAddress=uint64_t(faultWord615(p+4))<<32 | (inst&0xfffff000U);
    out->faultPageAddress=uint64_t(faultWord615(p+12))<<32 | (addr&0xfffff000U);
    out->timestamp=uint64_t(faultWord615(p+20))<<32 | faultWord615(p+16);
    out->instanceAperture=(inst>>8)&3;
    out->addressPhysicalAperture=addr&3;
    out->engineId=faultWord615(p+24)&0x1ff;
    out->faultType=flags&31;out->replayable=(flags>>7)&1;
    out->clientId=(flags>>8)&0x7f;out->accessType=(flags>>16)&15;
    out->mmuClientType=(flags>>20)&1;out->gpcId=(flags>>24)&31;
    out->protectedMode=(flags>>29)&1;out->replayableEnabled=(flags>>30)&1;
    return true;
}
}
