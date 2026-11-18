#pragma once
// Fixed second GR client for NVIDIA615.78.08. Primary kernel_graphics_context.c
// keeps GR_GLOBALCTX buffers per GPU (gfid) and maps them into each channel's
// VAS; only MAIN/PM/PATCH are per context. The client therefore owns those
// three buffers and its shader scratch, and maps root's global physical pages.
#include "NVGspGrBlob615.hpp"
#include "NVGspExternalVas615.hpp"

namespace nvgsp {
constexpr uint64_t kClientGrScratchBytes615=0xb000;
inline bool clientGrPerContext615(uint32_t id) { return id<=2; }
struct ClientGrLayout615 {
    uint64_t clientOffset[12]{};
    bool local[12]{}; // buffer backed by client memory rather than root's
    uint64_t perContextBytes=0,scratchVaOffset=0,scratchOffset=0,clientBytes=0;
};
// The client VA window mirrors root's plan offsets, so the root-proven
// compute packet layout applies unchanged at clientVa+scratchVaOffset.
// localGlobals mirrors primary RM's per-channel localCtxBuffer (VPR) path:
// every buffer, globals included, gets client-owned backing at root offsets.
// allShared: a subcontext of root's TSG shares root's whole GR context, so
// every buffer maps root's physical backing; only shader scratch is client's.
inline bool clientGrLayout615(const GrBlob615 &plan,ClientGrLayout615 *out,bool localGlobals=false,
                              bool allShared=false) {
    if(localGlobals && allShared)return false;
    if(!out || !plan.count || plan.count>12 || !plan.logicalBackingBytes ||
       plan.logicalBackingBytes>plan.allocationBytes || plan.allocationBytes>64ULL<<20)return false;
    ClientGrLayout615 l{};
    for(uint32_t i=0;i<plan.count;++i) {
        const auto &b=plan.buffers[i];
        if(b.id>=12 || !b.backingBytes || (b.backingBytes&4095) || (b.offset&4095) ||
           b.offset>plan.logicalBackingBytes || b.backingBytes>plan.logicalBackingBytes-b.offset)return false;
        if(allShared || (!localGlobals && !clientGrPerContext615(b.id)))continue;
        l.local[b.id]=true;
        l.clientOffset[b.id]=localGlobals ? b.offset : l.perContextBytes;
        if(!localGlobals)l.perContextBytes+=b.backingBytes;
    }
    if(localGlobals)l.perContextBytes=plan.logicalBackingBytes;
    if(!l.perContextBytes && !allShared)return false;
    l.scratchVaOffset=(plan.logicalBackingBytes+4095)&~4095ULL;
    if(l.scratchVaOffset>plan.allocationBytes || kClientGrScratchBytes615>plan.allocationBytes-l.scratchVaOffset)return false;
    l.scratchOffset=localGlobals ? l.scratchVaOffset : l.perContextBytes;
    l.clientBytes=(l.scratchOffset+kClientGrScratchBytes615+(2ULL<<20)-1)&~((2ULL<<20)-1);
    *out=l;return true;
}
// Physical page behind one client-window page. Per-context and scratch pages
// are client memory; global pages are root's existing physical backing.
// Gaps between buffers stay unmapped. GR backing is privileged, scratch USER.
inline bool clientGrPage615(const GrBlob615 &plan,const ClientGrLayout615 &l,uint64_t rootPhysical,
                            uint64_t clientPhysical,uint64_t vaOffset,uint64_t *physical,bool *privileged) {
    if(!physical || !privileged || (vaOffset&4095))return false;
    *physical=0;*privileged=false;
    if(vaOffset>=l.scratchVaOffset) {
        if(vaOffset-l.scratchVaOffset>=kClientGrScratchBytes615)return false;
        *physical=clientPhysical+l.scratchOffset+(vaOffset-l.scratchVaOffset);return true;
    }
    for(uint32_t i=0;i<plan.count;++i) {
        const auto &b=plan.buffers[i];
        if(vaOffset<b.offset || vaOffset-b.offset>=b.backingBytes)continue;
        *privileged=true;
        *physical=l.local[b.id] ? clientPhysical+l.clientOffset[b.id]+(vaOffset-b.offset) :
                                                rootPhysical+vaOffset;
        return true;
    }
    return false;
}
// Same 560-byte NV2080_CTRL_GPU_PROMOTE_CTX encoding as promoteGrBlob615,
// with per-buffer physical bases. Globals are never re-initialized. Primary
// kgrctxPrepare*CtxBuffer adds PM only when a PM ctxsw buffer exists, so a
// client may omit it.
inline bool promoteClientGrBlob615(const GrBlob615 &plan,const ClientGrLayout615 &l,uint32_t client,
                                   uint32_t channel,uint64_t rootPhysical,uint64_t clientPhysical,
                                   uint64_t va,void *out,size_t bytes,bool includePm=true) {
    if(!out || bytes!=560 || !client || !channel || !plan.count || plan.count>12 ||
       !rootPhysical || !clientPhysical || rootPhysical==clientPhysical ||
       ((rootPhysical|clientPhysical)&((2ULL<<20)-1)) || (va&((64ULL<<20)-1)) || !va ||
       rootPhysical>=(1ULL<<40) || plan.allocationBytes>(1ULL<<40)-rootPhysical ||
       clientPhysical>=(1ULL<<40) || l.clientBytes>(1ULL<<40)-clientPhysical ||
       va>=(1ULL<<49) || plan.allocationBytes>(1ULL<<49)-va)return false;
    if(clientPhysical<rootPhysical+plan.allocationBytes && rootPhysical<clientPhysical+l.clientBytes)return false;
    auto *p=static_cast<uint8_t *>(out);
    for(size_t i=0;i<bytes;++i)p[i]=0;
    auto put=[&](size_t off,uint64_t value,size_t n) {
        for(size_t i=0;i<n;++i)p[off+i]=uint8_t(value>>(i*8));
    };
    put(0,1,4);put(12,client,4);put(16,channel,4);
    uint32_t entries=0;
    for(uint32_t i=0;i<plan.count;++i) {
        const auto &b=plan.buffers[i];
        if(!b.bytes || b.bytes>b.backingBytes || b.id>=12 || (i && b.id<=plan.buffers[i-1].id) ||
           b.offset>plan.logicalBackingBytes || b.backingBytes>plan.logicalBackingBytes-b.offset)return false;
        if(!includePm && b.id==1)continue;
        const bool local=l.local[b.id];
        const uint64_t physical=local ? clientPhysical+l.clientOffset[b.id] : rootPhysical+b.offset;
        if(local && l.clientOffset[b.id]+b.backingBytes>l.perContextBytes)return false;
        const size_t entry=48+entries++*32;
        // Primary kgrctxPreparePromoteCtxBuffer: an already-initialized global
        // gets a VA-only entry (PA, size and physAttr zero, mapped). Client-
        // backed buffers carry PA/size/physAttr and initialize like root's.
        if(local) {put(entry,physical,8);put(entry+16,b.bytes,8);put(entry+24,4,4);}
        put(entry+8,va+b.offset,8);put(entry+28,b.id,2);put(entry+30,local && b.initialize,1);
    }
    put(40,entries,4);
    return entries!=0;
}
// 0.180.96: an owned client's caller buffer: pinned sysmem, mapped in the
// client VAS one 2 MiB-aligned slot above its GR window, CPU-mapped by owner.
constexpr uint64_t kClientUserBytes615=64*1024;
// 0.180.101: caller allocations: up to 8 more sysmem buffers, bump-allocated
// (64 KiB aligned, never reused) in a window at userVa.
// 0.180.105: window 32 MiB (29 PD0 tables with the GR range, of 32) and
// allocations up to 8 MiB, for the Metal driver's 6 MiB staging + heap.
// 0.180.118: 1 GiB window, 512 MiB allocations, 32 slots (8K render targets,
// big IOSurfaces); the tree now spans several PD1 slots.
// 0.180.132: 2.5 GiB window (apps with more than 1 GiB of buffers and textures)
constexpr uint64_t kClientUserWindow615=2560ULL<<20,kClientAllocMax615=512ULL<<20;
constexpr uint32_t kClientAllocs615=32,kClientLeafChunks615=2304;
// 0.180.112: owned clients alive at once (one per process); slot k uses RM
// handles 0xc0d00300|k<<16 and its own VAS, buffers and GR reservation.
// 0.180.123 tried 16: the second client then failed at stage 1 (status 81,
// NV_ERR_NO_MEMORY) and disabled the runtime; 0.180.125 retries 16 with the
// failing RM call recorded (client615-fail-alloc / -fail-control).
constexpr uint32_t kClientSlots615=16;
// 0.180.126: VRAM after the client reservation for its owned page tree
constexpr uint64_t kClientTreeReserve615=32ULL<<20;   // 0.180.132 (was 4 MiB), 157: 12 MiB, 163: 4 GiB wrap window tree ~13.75 MiB, 168: 12 GiB ~29.2 MiB
// 0.180.117: caller pages (IOSurfaces, NoCopy buffers) wired and mapped into
// a window above the allocation window; first fit, leaves found from the VA.
// 0.180.157: 2 GiB. CoreAnimation keeps ~500 MiB of IOSurface-backed
// surfaces alive in one process (720p layer stress), WindowServer at 4K more;
// the 512 MiB window answered NoSpace and the texture came back nil.
// 0.180.163: 4 GiB; a 4K CoreAnimation layer stress filled 2 GiB of live
// IOSurfaces, and WindowServer's 4K window backing stores are 33 MiB each.
// 0.180.168: 12 GiB. CoreAnimation caches up to 256 IOSurface textures per
// process (ca_stress at 4K: 256 alive by frame 30, up to 86 MiB RGBA16F each)
// and filled 4 GiB by frame ~40; the shadow fallback then had no VA either,
// the blur destination came back nil and a restarted WindowServer aborted
// (transient surface not found) about 6 s after start.
constexpr uint64_t kClientWrapWindow615=12288ULL<<20,kClientWrapMax615=256ULL<<20;   // 0.180.118 (was 128/64 MiB)
// 0.180.157: 512 wraps (was 64). WindowServer/CoreAnimation wrap every
// IOSurface-backed texture separately; 64 entries ran out with ~48 MiB of the
// 512 MiB window used, the wrap answered NoSpace, the texture came back nil and
// MPS/CoreAnimation asserted (WindowServer abort loop).
constexpr uint32_t kClientWraps615=512;
struct ClientGrState615 {
    uint64_t physical,bytes,va,gpfifoVa,instance,method,userd,gpfifo,methodBytes;
    uint64_t leafAddress[kClientLeafChunks615],maintGpfifo,userVa;
    uint64_t scratchVa,scratchPhysical;     // 0.180.98: caller dispatch reuses the scratch window
    uint32_t put,fence,dispatches;          // ring position and last HOST fence after setup
    uint64_t userNextVa,allocVa[kClientAllocs615],allocBytes[kClientAllocs615];
    uint32_t allocChunk[kClientAllocs615],allocChunks[kClientAllocs615],allocLive;   // allocLive: bit i = slot i mapped
    uint64_t wrapVa[kClientWraps615],wrapBytes[kClientWraps615],wrapLive[kClientWraps615/64],wrapPages;   // 0.180.117, 157: bitmap words
    uint32_t leafCount[kClientLeafChunks615],leafChunks,mappedPages,privilegedPages;
    uint32_t objects[20],objectCount,freedCount,stage,status,cid,token,vas;
    uint32_t copyEngine;   // 0.180.128: NV2080 engine type of the GR channel's copy object, 0 = none
    uint32_t completed;    // 0.180.133: last submission sequence seen finished (async submits)
    uint32_t output[64];
    uint32_t rcAtStage[16]; // GSP RC_TRIGGERED count observed at each step
    nvgsp::ExternalTree615 tree;
    bool attempted,passed,scheduled,live,tornDown,localGlobals,sameVas,touchOnly,subcontext,negative,external,cancelChannel,cancelled,userBuffer,
         sysCached;   // 0.180.177: some sysmem mapped L2-cacheable: submissions carry the barrier
};
// 0.180.157: wrap slot bitmap helpers (kClientWraps615/64 words)
inline bool clientWrapLive615(const ClientGrState615 &g,uint32_t i) {return (g.wrapLive[i>>6]>>(i&63))&1;}
inline void clientWrapSet615(ClientGrState615 &g,uint32_t i,bool on) {
    if(on)g.wrapLive[i>>6]|=1ULL<<(i&63);else g.wrapLive[i>>6]&=~(1ULL<<(i&63));
}

}
