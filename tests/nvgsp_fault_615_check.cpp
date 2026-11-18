#include "../drivers/NVGspCore/NVGspFault615.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
#ifdef NVGSP_PRIMARY_FAULT_HEADERS
#include <nvtypes.h>
#include <nvmisc.h>
#include <class/clc369.h>
#include <ctrl/ctrl2080/ctrl2080internal.h>
#include <swref/published/volta/gv100/dev_fault.h>
// Queue's upstream header requires RM platform hooks even for offsetof.
// Supply only opaque allocator/debug declarations; use the actual Queue
// definition unchanged, and trap if any assertion helper is ever executed.
struct PORT_MEM_ALLOCATOR;
#define PORT_BREAKPOINT() __builtin_trap()
#define PORT_IS_CHECKED_BUILD 1
#define PORT_BREAKPOINT_CHECKED() __builtin_trap()
#define PORT_COVERAGE_PUSH_OFF
#define PORT_COVERAGE_POP
#include <containers/queue.h>
static_assert(sizeof(nvgsp::GmmuStaticInfo615)==sizeof(NV2080_CTRL_INTERNAL_GMMU_GET_STATIC_INFO_PARAMS),"primary static info size");
static_assert(offsetof(nvgsp::GmmuStaticInfo615,nonReplayableBytes)==offsetof(NV2080_CTRL_INTERNAL_GMMU_GET_STATIC_INFO_PARAMS,nonReplayableFaultBufferSize),"primary nonreplayable offset");
static_assert(offsetof(nvgsp::GmmuStaticInfo615,replayableMetadataBytes)==offsetof(NV2080_CTRL_INTERNAL_GMMU_GET_STATIC_INFO_PARAMS,replayableShadowFaultBufferMetadataSize),"primary replayable metadata offset");
static_assert(offsetof(nvgsp::GmmuStaticInfo615,nonReplayableMetadataBytes)==offsetof(NV2080_CTRL_INTERNAL_GMMU_GET_STATIC_INFO_PARAMS,nonReplayableShadowFaultBufferMetadataSize),"primary nonreplayable metadata offset");
static_assert(nvgsp::kGmmuStaticInfoCommand615==NV2080_CTRL_CMD_INTERNAL_GMMU_GET_STATIC_INFO,"primary control ID");
static_assert(nvgsp::kFaultRecordBytes615==NVC369_BUF_SIZE,"primary packet size");
static_assert(nvgsp::kShadowFaultMaxPages615==NV2080_CTRL_INTERNAL_GMMU_CLIENT_SHADOW_FAULT_BUFFER_MAX_PAGES,"primary max pages");
static_assert(sizeof(NV2080_CTRL_INTERNAL_GMMU_REGISTER_CLIENT_SHADOW_FAULT_BUFFER_PARAMS)==24032,"registration transport requirement");
static_assert(sizeof(nvgsp::ShadowFaultRegistration615)==sizeof(NV2080_CTRL_INTERNAL_GMMU_REGISTER_CLIENT_SHADOW_FAULT_BUFFER_PARAMS),"primary registration ABI");
static_assert(offsetof(nvgsp::ShadowFaultRegistration615,pageBus)==offsetof(NV2080_CTRL_INTERNAL_GMMU_REGISTER_CLIENT_SHADOW_FAULT_BUFFER_PARAMS,shadowFaultBufferPteArray),"primary page array offset");
static_assert(offsetof(nvgsp::ShadowFaultRegistration615,type)==offsetof(NV2080_CTRL_INTERNAL_GMMU_REGISTER_CLIENT_SHADOW_FAULT_BUFFER_PARAMS,shadowFaultBufferType),"primary type offset");
static_assert(offsetof(nvgsp::ShadowFaultRegistration615,sharedBus)==offsetof(NV2080_CTRL_INTERNAL_GMMU_REGISTER_CLIENT_SHADOW_FAULT_BUFFER_PARAMS,faultBufferSharedMemoryPhysAddr),"primary shared offset");
static_assert(sizeof(nvgsp::ShadowFaultQueue615)==sizeof(Queue),"primary queue size");
static_assert(offsetof(nvgsp::ShadowFaultQueue615,msgSize)==offsetof(Queue,msgSize),"primary queue message size");
static_assert(offsetof(nvgsp::ShadowFaultQueue615,get)==offsetof(Queue,getIdx),"primary queue GET");
static_assert(offsetof(nvgsp::ShadowFaultQueue615,put)==offsetof(Queue,putIdx),"primary queue PUT");
static_assert(nvgsp::kFaultReadOnlyViolation615==NV_PFAULT_FAULT_TYPE_RO_VIOLATION,"primary readonly fault enum");
static_assert(nvgsp::kFaultVirtualWrite615==NV_PFAULT_ACCESS_TYPE_VIRT_WRITE,"primary write access enum");
#endif
#ifdef NVGSP_PRIMARY_FAULT_HEADERS
static_assert(sizeof(nvgsp::ReplayableFaultRegistration615)==sizeof(NV2080_CTRL_INTERNAL_GMMU_REGISTER_FAULT_BUFFER_PARAMS),"primary replayable size");
static_assert(offsetof(nvgsp::ReplayableFaultRegistration615,faultBufferSize)==offsetof(NV2080_CTRL_INTERNAL_GMMU_REGISTER_FAULT_BUFFER_PARAMS,faultBufferSize),"primary size field");
static_assert(offsetof(nvgsp::ReplayableFaultRegistration615,pages)==offsetof(NV2080_CTRL_INTERNAL_GMMU_REGISTER_FAULT_BUFFER_PARAMS,faultBufferPteArray),"primary page array");
static_assert(nvgsp::kReplayableFaultRegisterCommand615==NV2080_CTRL_CMD_INTERNAL_GMMU_REGISTER_FAULT_BUFFER,"primary command");
#endif
int main(int argc,char **argv) {
    {
        nvgsp::GmmuStaticInfo615 r{};uint32_t pg=0;
        r.replayableBytes=32*2048;assert(nvgsp::replayableFaultPlan615(r,&pg) && pg==16);
        r.replayableBytes=33;assert(!nvgsp::replayableFaultPlan615(r,&pg) && !pg);
        r.replayableBytes=32*2048;r.replayableMetadataBytes=64;assert(!nvgsp::replayableFaultPlan615(r,&pg));
        r.replayableMetadataBytes=0;r.replayableBytes=257*4096;assert(!nvgsp::replayableFaultPlan615(r,&pg));
    }
    using namespace nvgsp;
    uint32_t pages=99,entries=99;
    GmmuStaticInfo615 info{0,0,4096,0};
    assert(nonReplayableFaultPlan615(info,&pages,&entries) && pages==1 && entries==127);
    info.nonReplayableBytes=4128;
    assert(nonReplayableFaultPlan615(info,&pages,&entries) && pages==2 && entries==128);
    for(uint32_t n: {0U,1U,32U,4095U,3000U*4096U+32U,~0U}) {
        info.nonReplayableBytes=n;
        assert(!nonReplayableFaultPlan615(info,&pages,&entries) && !pages && !entries);
    }
    info.nonReplayableBytes=3000U*4096U;
    assert(nonReplayableFaultPlan615(info,&pages,&entries) && pages==3000);
    info.nonReplayableMetadataBytes=32;
    assert(!nonReplayableFaultPlan615(info,&pages,&entries));
    assert(!nonReplayableFaultPlan615(info,nullptr,&entries));
    std::vector<uint64_t> pageBus(545);
    for(size_t i=0;i<pageBus.size();++i)pageBus[i]=0x100000000ULL+i*4096;
    ShadowFaultRegistration615 reg{};
    assert(buildShadowFaultRegistration615(&reg,0x200000000ULL,2231328,pageBus.data(),545));
    assert(reg.queueBus==0x200000000ULL && reg.bufferBytes==2231328 && !reg.type && !reg.metadataBytes && !reg.sharedBus);
    assert(!std::memcmp(reg.pageBus,pageBus.data(),545*8));
    for(unsigned i=545;i<3000;++i)assert(!reg.pageBus[i]);
    assert(!buildShadowFaultRegistration615(&reg,pageBus[0],2231328,pageBus.data(),545));
    auto badPages=pageBus;badPages[544]=badPages[0];
    assert(!buildShadowFaultRegistration615(&reg,0x200000000ULL,2231328,badPages.data(),545));
    badPages=pageBus;badPages[544]=1ULL<<48;
    assert(!buildShadowFaultRegistration615(&reg,0x200000000ULL,2231328,badPages.data(),545));
    assert(!buildShadowFaultRegistration615(&reg,0x200000000ULL,2231328,pageBus.data(),544));
    assert(!buildShadowFaultRegistration615(&reg,0x200000001ULL,2231328,pageBus.data(),545));
    std::vector<uint8_t> request(kShadowFaultRequestBytes615);
    assert(buildControl615(request.data(),request.size(),kLifecycleClient615,kLifecycleSubdevice615,kShadowFaultRegisterCommand615,sizeof(reg)));
    assert(nativeShadowFaultRequest615(76,request.data(),request.size()));
    assert(!nativeShadowFaultRequest615(103,request.data(),request.size()));
    assert(!nativeShadowFaultRequest615(76,request.data(),request.size()-1));
    for(unsigned byte: {0U,4U,8U,12U,16U,20U,24U,28U,32U,39U}) {
        auto bad=request;bad[byte]^=1;
        assert(!nativeShadowFaultRequest615(76,bad.data(),bad.size()));
    }
    std::vector<uint8_t> ring(17*4096),copied(16*4096+2,0xee);
    for(size_t i=0;i<ring.size();++i)ring[i]=uint8_t(i*7+(i/4096)*13);
    auto page=[&](uint32_t n)->const uint8_t * {return ring.data()+n*4096;};
    for(uint32_t bytes: {1U,4095U,4096U,4097U,kShadowFaultReplyBytes615,16U*4096U}) {
        assert(copyRpcReplyPages615(copied.data()+1,copied.size()-2,bytes,15,17,page));
        for(uint32_t i=0;i<bytes;++i)assert(copied[i+1]==ring[((15+i/4096)%17)*4096+i%4096]);
        assert(copied.front()==0xee && copied.back()==0xee);
    }
    assert(!copyRpcReplyPages615(copied.data(),1,2,0,17,page));
    assert(!copyRpcReplyPages615(copied.data(),copied.size(),0,0,17,page));
    assert(!copyRpcReplyPages615(copied.data(),copied.size(),4097,0,1,page));
    assert(!copyRpcReplyPages615(copied.data(),copied.size(),1,17,17,page));
    auto missing=[](uint32_t)->const uint8_t * {return nullptr;};
    assert(!copyRpcReplyPages615(copied.data(),copied.size(),1,0,17,missing));
    FaultRecord615 out{};
    out.instancePageAddress=0x200000;out.faultPageAddress=0x100001000ULL;
    out.faultType=6;out.accessType=1;
    assert(ownedReadOnlyWriteFault615(out,0x200000,0x100001000ULL));
    assert(!ownedReadOnlyWriteFault615(out,0x201000,0x100001000ULL));
    assert(!ownedReadOnlyWriteFault615(out,0x200000,0x100002000ULL));
    out.replayable=true;assert(!ownedReadOnlyWriteFault615(out,0x200000,0x100001000ULL));
    out.replayable=false;out.faultType=5;assert(!ownedReadOnlyWriteFault615(out,0x200000,0x100001000ULL));
    uint32_t state=0x5a764321,words[8]{};
    for(unsigned trial=0;trial<10000;++trial) {
        for(auto &word:words) {state^=state<<13;state^=state>>17;state^=state<<5;word=state;}
        words[7]|=0x80000000U;
        uint8_t raw[33]{};
        for(unsigned i=0;i<32;++i)raw[i+1]=uint8_t(words[i/4]>>(i%4*8));
        assert(decodeFaultRecord615(raw+1,32,&out)); // deliberately unaligned
#ifdef NVGSP_PRIMARY_FAULT_HEADERS
#define CHECK(member,field) assert(out.member==DRF_VAL_MW(C369,_BUF_ENTRY,field,words))
        CHECK(instanceAperture,_INST_APERTURE);CHECK(addressPhysicalAperture,_ADDR_PHYS_APERTURE);
        CHECK(engineId,_ENGINE_ID);CHECK(faultType,_FAULT_TYPE);CHECK(replayable,_REPLAYABLE_FAULT);
        CHECK(clientId,_CLIENT);CHECK(accessType,_ACCESS_TYPE);CHECK(mmuClientType,_MMU_CLIENT_TYPE);
        CHECK(gpcId,_GPC_ID);CHECK(protectedMode,_PROTECTED_MODE);CHECK(replayableEnabled,_REPLAYABLE_FAULT_EN);
        assert(out.instancePageAddress==(uint64_t(DRF_VAL_MW(C369,_BUF_ENTRY,_INST_HI,words))<<32 |
               uint64_t(DRF_VAL_MW(C369,_BUF_ENTRY,_INST_LO,words))<<12));
        assert(out.faultPageAddress==(uint64_t(DRF_VAL_MW(C369,_BUF_ENTRY,_ADDR_HI,words))<<32 |
               uint64_t(DRF_VAL_MW(C369,_BUF_ENTRY,_ADDR_LO,words))<<12));
        assert(out.timestamp==(uint64_t(DRF_VAL_MW(C369,_BUF_ENTRY,_TIMESTAMP_HI,words))<<32 |
               DRF_VAL_MW(C369,_BUF_ENTRY,_TIMESTAMP_LO,words)));
#undef CHECK
#endif
        for(size_t size=0;size<32;++size)assert(!decodeFaultRecord615(raw+1,size,&out));
        words[7]&=~0x80000000U;
        for(unsigned i=0;i<32;++i)raw[i+1]=uint8_t(words[i/4]>>(i%4*8));
        assert(!decodeFaultRecord615(raw+1,32,&out));
    }
    assert(!decodeFaultRecord615(nullptr,32,&out));
    assert(!decodeFaultRecord615(words,32,nullptr));
    assert(argc==2);
    std::ifstream file(argv[1],std::ios::binary);
    std::vector<uint8_t> notice{std::istreambuf_iterator<char>(file),{}};
    assert(notice.size()==64 && !decodeFaultRecord615(notice.data(),notice.size(),&out));
    std::puts("Fault615: primary registration/queue ABI, 545-page ownership/identity/bounds, wrapped RPC copies, 10000 synthetic field comparisons and real GMC-notice rejection PASS; no hardware fault-service claim");
}
