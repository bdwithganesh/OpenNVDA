#include "../drivers/NVGspCore/NVGspLifecycle615.hpp"
#include "../drivers/NVGspCore/NVGspInitQueueReset.hpp"
#include "../drivers/NVGspCore/NVGspGmcConfig.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
using namespace nvgsp;
int main(int argc,char **argv) {
    uint8_t request[32+128]{};
    assert(!buildMemoryProbe615(request,sizeof(request)-1));
    assert(buildMemoryProbe615(request,sizeof(request)));
    AllocHeader615 h{};MemoryParams615 p{};
    std::memcpy(&h,request,sizeof(h));std::memcpy(&p,request+32,sizeof(p));
    assert(h.client==kLifecycleClient615 && h.parent==kLifecycleSubdevice615 &&
           h.object==kLifecycleMemory615 && h.objectClass==0x40 && h.paramsBytes==128);
    assert(p.flags==0x100 && p.attr==(1u<<23) && p.size==4096 && p.alignment==4096);
    p.offset=0x8080000;p.limit=4095;
    assert(memoryProbeRange615(p,0x3ff800000));
    assert(memoryProbeRange615(p,0x3ff800000,0x8080000,0x3f292ffff));
    assert(!memoryProbeRange615(p,0x3ff800000,0x8090000,0x3f292ffff));
    assert(!memoryProbeRange615(p,0x3ff800000,0x8080000,p.offset+4094));
    p.limit=p.offset+4095;assert(!memoryProbeRange615(p,0x3ff800000));p.limit=4095;
    assert(memoryProbeRegion615(p,0x3ff800000,0x8080000,0x3f292ffff,3,5));
    assert(!memoryProbeRegion615(p,0x3ff800000,0x8080000,0x3f292ffff,7,5));
    assert(!memoryProbeRegion615(p,0x3ff800000,0x8080000,0x3f292ffff,3,1));
    assert(!memoryProbeRegion615(p,0x3ff800000,0x8080000,0x3f292ffff,3,4));
    p.limit--;assert(!memoryProbeRange615(p,0x3ff800000));p.limit++;
    p.offset++;assert(!memoryProbeRange615(p,0x3ff800000));p.offset--;
    p.size=UINT64_MAX;assert(!memoryProbeRange615(p,0x3ff800000));p.size=4096;
    p.offset=UINT64_MAX-4095;p.limit=UINT64_MAX;assert(!memoryProbeRange615(p,0x3ff800000));
    p.offset=0x8080000;p.limit=4095;p.owner++;assert(!memoryProbeRange615(p,0x3ff800000));
    assert(initQueueResetState(0,10,1,0)==InitQueueResetState::Parse);
    assert(initQueueResetState(100,99,1,0)==InitQueueResetState::AwaitProducer);
    assert(initQueueResetState(100,100,1,0)==InitQueueResetState::TimedOut);
    assert(initQueueResetState(100,101,1,0)==InitQueueResetState::TimedOut);
    assert(initQueueResetState(100,99,1,4)==InitQueueResetState::Parse);
    assert(initQueueResetState(100,99,0,0)==InitQueueResetState::Parse);
    assert(initQueueResetState(100,99,2,0)==InitQueueResetState::Parse);
    if(argc==3) {
        std::ifstream mf(argv[1],std::ios::binary),gf(argv[2],std::ios::binary);
        std::vector<uint8_t> memory((std::istreambuf_iterator<char>(mf)),{}),config((std::istreambuf_iterator<char>(gf)),{});
        const uint8_t *params=nullptr;
        assert(decodeAlloc615(memory.data(),memory.size(),kLifecycleSubdevice615,kLifecycleMemory615,0x40,128,&params).successful());
        std::memcpy(&p,params,sizeof(p));
        GmcStaticConfig c{};assert(parseGmcStaticConfig(config.data(),config.size(),&c));
        unsigned matches=0;
        for(unsigned i=0;i<c.regionCount;++i) {
            const auto &r=c.regions[i];
            if(memoryProbeRegion615(p,c.fbBytes,r.base,r.limit,r.flags,r.tag))++matches;
        }
        assert(matches==1 && p.limit==4095 && p.size==4096);
    }
    std::puts("6154KiB memory ownership/range and bounded HS queue-reset grace PASS");
}
