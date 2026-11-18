#pragma once
#include "NVGspFault615.hpp"
namespace nvgsp {
constexpr uint32_t kDisableChannelsCommand615=0x2080110b;
// ctrl2080fifo.h: zero event selects synchronous preemption. Explicit padding
// is part of the pinned 64-bit ABI, not caller-controlled data.
struct DisableOwnedChannels615 {
    uint8_t disable,padding0[3];
    uint32_t count;
    uint8_t onlyScheduling,rewind,padding1[6];
    uint64_t event;
    uint32_t clients[64],channels[64];
};
static_assert(sizeof(DisableOwnedChannels615)==536,"615 disable channels ABI");
static_assert(offsetof(DisableOwnedChannels615,event)==16 &&
              offsetof(DisableOwnedChannels615,clients)==24,"615 disable channels offsets");
inline bool ownedFaultQueue615(const ShadowFaultQueue615 &q,uint64_t capacity,uint32_t *pending) {
    if(!pending)return false;
    *pending=0;
    if(capacity<2 || q.capacity!=capacity || q.msgSize!=32 || q.allocator || q.data ||
       q.get>=capacity || q.put>=capacity)return false;
    const uint64_t n=(q.put+capacity-q.get)%capacity;
    if(!n || n>8)return false;
    *pending=uint32_t(n);return true;
}
// Mode2 expects a read-only violation; mode5 expects a missing PDE.
inline bool ownedFaultBatch615(const void *raw,uint32_t count,uint64_t instance,uint64_t page,
                               uint32_t type=kFaultReadOnlyViolation615) {
    if(!raw || !count || count>8)return false;
    const auto *p=static_cast<const uint8_t *>(raw);
    for(uint32_t i=0;i<count;++i) {
        FaultRecord615 fault{};
        if(!decodeFaultRecord615(p+i*32,32,&fault) ||
           !ownedWriteFault615(fault,instance,page,type) ||
           fault.instanceAperture || fault.engineId!=15 || !fault.replayableEnabled ||
           fault.protectedMode)return false;
    }
    return true;
}
struct OwnedRecoveryState615 {
    DisableOwnedChannels615 disable;
    uint8_t raw[8*32];
    uint64_t oldGet,oldPut;
    uint32_t count,stage,status,noticeCount,rcCount;
    bool attempted,preempted,consumed,teardownVerified,freshProbeStarted;
};
}
