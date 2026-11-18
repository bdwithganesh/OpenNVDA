#include "../drivers/NVGspCore/NVGspOwnedRecovery615.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
#ifdef NVGSP_PRIMARY_FAULT_HEADERS
#include <ctrl/ctrl2080/ctrl2080fifo.h>
static_assert(sizeof(nvgsp::DisableOwnedChannels615)==sizeof(NV2080_CTRL_FIFO_DISABLE_CHANNELS_PARAMS),"primary synchronous preempt ABI size");
static_assert(offsetof(nvgsp::DisableOwnedChannels615,count)==offsetof(NV2080_CTRL_FIFO_DISABLE_CHANNELS_PARAMS,numChannels),"primary count");
static_assert(offsetof(nvgsp::DisableOwnedChannels615,onlyScheduling)==offsetof(NV2080_CTRL_FIFO_DISABLE_CHANNELS_PARAMS,bOnlyDisableScheduling),"primary scheduling flag");
static_assert(offsetof(nvgsp::DisableOwnedChannels615,rewind)==offsetof(NV2080_CTRL_FIFO_DISABLE_CHANNELS_PARAMS,bRewindGpPut),"primary rewind");
static_assert(offsetof(nvgsp::DisableOwnedChannels615,event)==offsetof(NV2080_CTRL_FIFO_DISABLE_CHANNELS_PARAMS,pRunlistPreemptEvent),"primary event");
static_assert(offsetof(nvgsp::DisableOwnedChannels615,channels)==offsetof(NV2080_CTRL_FIFO_DISABLE_CHANNELS_PARAMS,hChannelList),"primary channels");
static_assert(nvgsp::kDisableChannelsCommand615==NV2080_CTRL_CMD_FIFO_DISABLE_CHANNELS,"primary disable method");
#endif
int main(int argc,char **argv) {
    using namespace nvgsp;
    ShadowFaultQueue615 q{};q.capacity=69729;q.msgSize=32;q.put=1;uint32_t n=0;
    assert(ownedFaultQueue615(q,69729,&n) && n==1);
    q.get=69728;q.put=7;assert(ownedFaultQueue615(q,69729,&n) && n==8);
    q.put=8;assert(!ownedFaultQueue615(q,69729,&n) && !n);
    q.put=q.get;assert(!ownedFaultQueue615(q,69729,&n));
    q.put=q.capacity;assert(!ownedFaultQueue615(q,69729,&n));
    q.put=1;q.get=0;q.msgSize=64;assert(!ownedFaultQueue615(q,69729,&n));
    q.msgSize=32;q.data=1;assert(!ownedFaultQueue615(q,69729,&n));
    q.data=0;q.allocator=1;assert(!ownedFaultQueue615(q,69729,&n));
    q.allocator=0;assert(!ownedFaultQueue615(q,69728,&n));
    assert(!ownedFaultQueue615(q,69729,nullptr));
    assert(argc==2 || argc==3);
    std::ifstream f(argv[1],std::ios::binary);assert(f.good());
    std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)),{});assert(raw.size()==32);
    FaultRecord615 record{};assert(decodeFaultRecord615(raw.data(),32,&record));
    assert(ownedFaultBatch615(raw.data(),1,record.instancePageAddress,record.faultPageAddress));
    assert(!ownedFaultBatch615(raw.data(),1,record.instancePageAddress+4096,record.faultPageAddress));
    assert(!ownedFaultBatch615(raw.data(),1,record.instancePageAddress,record.faultPageAddress+4096));
    std::vector<uint8_t> batch(8*32);for(unsigned i=0;i<8;++i)std::memcpy(batch.data()+i*32,raw.data(),32);
    assert(ownedFaultBatch615(batch.data(),8,record.instancePageAddress,record.faultPageAddress));
    // One unrelated/invalid packet prevents acknowledgment of the entire batch.
    batch[7*32+31]^=0x80;
    assert(!ownedFaultBatch615(batch.data(),8,record.instancePageAddress,record.faultPageAddress));
    assert(!ownedFaultBatch615(raw.data(),0,record.instancePageAddress,record.faultPageAddress));
    assert(!ownedFaultBatch615(nullptr,1,record.instancePageAddress,record.faultPageAddress));
    assert(!ownedFaultBatch615(batch.data(),9,record.instancePageAddress,record.faultPageAddress));
    // The RO record is never accepted as the cross-VAS (PDE) kind.
    assert(!ownedFaultBatch615(raw.data(),1,record.instancePageAddress,record.faultPageAddress,kFaultPde615));
    if(argc==3) {
        // Actual 60 hardware record: A's write to B's VA 0x200001000, PDE fault.
        std::ifstream g(argv[2],std::ios::binary);assert(g.good());
        std::vector<uint8_t> pde((std::istreambuf_iterator<char>(g)),{});assert(pde.size()==32);
        FaultRecord615 cross{};assert(decodeFaultRecord615(pde.data(),32,&cross));
        assert(cross.faultType==kFaultPde615 && cross.faultPageAddress==0x200001000ULL && cross.engineId==15);
        assert(ownedCrossVasWriteFault615(cross,cross.instancePageAddress,0x200001000ULL));
        assert(!ownedReadOnlyWriteFault615(cross,cross.instancePageAddress,0x200001000ULL));
        assert(ownedFaultBatch615(pde.data(),1,cross.instancePageAddress,0x200001000ULL,kFaultPde615));
        assert(!ownedFaultBatch615(pde.data(),1,cross.instancePageAddress,0x200001000ULL));
        // A's own VA page is not the faulting page in mode5.
        assert(!ownedFaultBatch615(pde.data(),1,cross.instancePageAddress,0x100001000ULL,kFaultPde615));
        assert(!ownedFaultBatch615(pde.data(),1,cross.instancePageAddress+4096,0x200001000ULL,kFaultPde615));
        puts("Owned recovery: actual60 cross-VAS PDE record accepted only as PDE on B's page PASS");
    }
    puts("Owned recovery: primary preempt ABI, empty/overflow/wrapped queue, actual50 raw fault identity and mixed-batch rejection PASS");
}
