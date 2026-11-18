#pragma once
#include "NVGspLifecycle615.hpp"
#include "NVGspChannel615.hpp"
namespace nvgsp {
inline bool rpcReplyCompleted615(bool outstanding,uint32_t replyBytes,
                                 uint32_t sentGeneration,uint32_t currentGeneration) {
    return !outstanding && replyBytes>=56 && sentGeneration==currentGeneration;
}
// 0.180.115: owned client slot k uses 0xc0d00300|k<<16 (kClientSlots615=4)
// 0.180.125: 16 client slots, slot k's handles are 0xc0d00300 | k << 16
inline bool diagnosticHandle615(uint32_t h) { return (h&0xfff00000U)==0xc0d00000U; }
inline bool diagnosticKickHandle615(uint32_t h) { return h==0xc0d00056U; }
inline bool rpcLateMatch615(bool pending,uint32_t function,uint32_t sequence,
                            uint32_t expectedFunction,uint32_t expectedSequence,
                            uint32_t generation,uint32_t currentGeneration) {
    return pending && sequence && expectedSequence && function==expectedFunction &&
        sequence==expectedSequence && generation==currentGeneration;
}
inline bool diagnosticBaseline615(uint32_t h) {
    return h==kLifecycleClient615 || h==kLifecycleDevice615 || h==kLifecycleSubdevice615 ||
           h==kLifecycleVaspace615 || h==kLifecycleMemory615;
}
// Only explicitly privileged diagnostics may use this while the normal
// engine/channel state is still paused. Never touch firmware-internal handles.
inline bool diagnosticRequest615(uint32_t function,const void *payload,size_t bytes,
                                  const void *classes,size_t classBytes) {
    if (!payload || bytes>3900) return false;
    // Actual baremetal GSP rejected legacy RM-only14 with INVALID_FUNCTION.
    // Page tables are CPU-managed; these unsupported RPCs stay inaccessible.
    if (function==14 || function==15) return false;
    if (function==76) {
        if (bytes<sizeof(ControlHeader615)) return false;
        ControlHeader615 h{};__builtin_memcpy(&h,payload,sizeof(h));
        return h.client==kLifecycleClient615 && diagnosticHandle615(h.object) && h.command &&
            !h.status && !h.rpcFlags && !h.controlFlags && !h.accessRight && !h.reserved &&
            h.paramsBytes==bytes-sizeof(h);
    }
    if (function==103) {
        if (bytes<sizeof(AllocHeader615) || !classes || !classBytes ||
            classBytes%4 || classBytes>kClassList615Capacity*4) return false;
        AllocHeader615 h{};__builtin_memcpy(&h,payload,sizeof(h));
        if (h.client!=kLifecycleClient615 || !diagnosticHandle615(h.parent) ||
            !diagnosticHandle615(h.object) || diagnosticBaseline615(h.object) ||
            h.status || h.flags || h.reserved || h.paramsBytes!=bytes-sizeof(h)) return false;
        const auto *p=static_cast<const uint8_t *>(classes);
        for (size_t i=0;i<classBytes;i+=4) if(control615U32(p+i)==h.objectClass) return true;
        return false;
    }
    if (function==10) {
        if(bytes!=sizeof(FreeParams615)) return false;
        FreeParams615 h{};__builtin_memcpy(&h,payload,sizeof(h));
        return h.client==kLifecycleClient615 && !h.parent && !h.status &&
            diagnosticHandle615(h.object) && !diagnosticBaseline615(h.object);
    }
    return false;
}
}
