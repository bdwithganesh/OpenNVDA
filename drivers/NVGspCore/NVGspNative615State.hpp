#pragma once
#include "NVGspGrBlob615.hpp"
#include "NVGspChannel615.hpp"
#include "NVGspComputePacket615.hpp"
#include "NVGspMmu615.hpp"
#include "NVGspIsolation615.hpp"
#include "NVGspFault615.hpp"
#include "NVGspOwnedRecovery615.hpp"
#include "NVGspClientGr615.hpp"
// No caller mappings or pointers are stored here. Firmware objects, physical
// backing and PTEs stay pinned until PCI bus mastering is disabled at cold stop.
struct NVGspNative615State {
    uint8_t request[2048],reply[4096],context[1664];
    uint8_t faultRequest[nvgsp::kShadowFaultRequestBytes615];
    uint8_t faultReply[nvgsp::kShadowFaultReplyBytes615];
    uint64_t faultPages[nvgsp::kShadowFaultMaxPages615];
    bool faultAttempted,faultRegistered;
    uint32_t writeWords[1024],readWords[1024];
    nvgsp::GrBlob615 gr;
    nvgsp::GmmuStaticInfo615 faultInfo;
    uint64_t generation,physical,bytes,grVa,gpfifoVa;
    uint64_t instance,method,userd,gpfifo,methodBytes;
    uint64_t firstGrPteAddress,firstGrPteValue;
    uint32_t token,cid,sequence,put,fenceExpected;
    nvgsp::ComputePacket615 compute;
    uint32_t objects[16],objectCount,mappedPages,zeroWords,healthPolls,privilegedGrPages;
    nvgsp::IsolationState615 isolation;
    nvgsp::OwnedRecoveryState615 recovery;
    // Copy of the last NAK that completed on the root channel; the second
    // GR client may run only this already hardware-proven binary.
    uint8_t provenNak[4096];
    uint32_t provenNakBytes;
    nvgsp::ClientGrState615 clientGrs[nvgsp::kClientSlots615];   // 0.180.112
    nvgsp::ClientGrLayout615 clientLayout;   // runClientGr615 scratch (stack budget)
    uint8_t clientPromote[560];              // 0.180.100: PROMOTE_CTX params, off the stack
    // 0.180.106: the client GR backing reservation. Pinned while a client may
    // reference it; after a verified teardown (stage 13: channel freed, PTEs
    // cleared, MMU invalidated) the next client reuses it instead of taking
    // another of the 8 diagnostic VRAM slots (MTL27 ran out after 8 clients).
    uint64_t clientReservePhysical[nvgsp::kClientSlots615],clientReserveBytes[nvgsp::kClientSlots615],
             clientReserveGeneration[nvgsp::kClientSlots615];
    bool clientReserveFree[nvgsp::kClientSlots615];
    // 0.180.93: permanent privileged CE channel in its own external VAS. RM's
    // DMA_INVALIDATE_TLB did not drop an external client VAS's entries (92:
    // a recreated client with the same PDB/VA used the old pages), so every
    // client teardown issues MEM_OP TLB invalidate PDB_ALL from here.
    struct {
        nvgsp::ExternalTree615 tree;
        uint64_t va,ring,instance,method,userd;
        uint32_t objects[12],objectCount,put,fence,invalidates;
        bool attempted,live;
    } mmu;
};
