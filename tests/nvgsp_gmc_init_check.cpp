#include "../drivers/NVGspCore/NVGspGmcInit.hpp"
#include "../drivers/NVGspCore/NVGspRegistry.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

// Independent walk of the published NVKV instruction format. It does not
// invoke the encoder to derive expected keys, opcodes, offsets or values.
static std::map<unsigned,std::vector<uint64_t>> decode(const uint8_t *p,size_t n) {
    std::map<unsigned,std::vector<uint64_t>> values;
    assert(n%8==0);
    for(size_t off=0;off<n;) {
        uint64_t h=0;std::memcpy(&h,p+off,8);off+=8;
        const unsigned opcode=(h>>28)&15,key=h&65535;
        assert(((h>>16)&4095)==0);
        if(opcode==0) { values[key].push_back(h>>32);continue; }
        const uint64_t count=h>>32;
        assert(opcode==2 || opcode==3);
        const uint64_t bytes=opcode==2 ? count*8 : count;
        assert(bytes<=n-off);
        if(opcode==2) {
            assert(count==1);uint64_t v=0;std::memcpy(&v,p+off,8);values[key].push_back(v);
        } else {
            assert(bytes && p[off+bytes-1]==0);values[key].push_back(bytes);
        }
        off+=(bytes+7)&~uint64_t(7);
    }
    return values;
}
int main() {
    using namespace nvgsp;
    GspSystemInfoParameters sys{};
    sys.bar0=0x41000000;sys.bar1=0x1000000000;sys.bar2=0xfe2000000;
    sys.domainBusDevice=0x1000;sys.pciDeviceId=0x270410de;
    sys.pciSubDeviceId=0x40bd1458;sys.pciRevisionId=0xa1;
    sys.consoleMemSize=64ULL<<20;sys.maxUserVa=0x7fffffffffff;
    sys.gpuBehindBridge=1;
    sys.hypervisorType=2;sys.isPassthru=1;
    assert(gmcHypervisorType(0x7263694d,0x666f736f,0x76482074)==2);
    assert(gmcHypervisorType(0,0,0)==4);
    std::vector<uint8_t> data(65552,0xa5);uint32_t bytes=123;
    assert(buildGmcInit(data.data(),65536,sys,nullptr,0,&bytes));
    assert(bytes>=64 && bytes<=4096 && bytes%8==0);
    GspQueueElementHeader q{};GmcHeader h{};
    std::memcpy(&q,data.data(),24);std::memcpy(&h,data.data()+24,40);
    assert(q.nvdmHeader==0x2610de7e && q.mctpMagic==0x4d435450);
    assert(q.mctpPayloadSize==bytes && q.nvdmPayloadSize==bytes-24);
    assert(h.command==0x10001 && h.sequence==0x474d430000000001ULL);
    assert(h.statusOrMaxResponse==49152 && h.size==bytes-64);
    auto fields=decode(data.data()+64,h.size);
    assert(fields[1][0]==0x270410de && fields[2][0]==0x40bd1458 && fields[3][0]==0xa1);
    assert(fields[0x1011][0]==0x1000000000 && fields[0x1012][0]==0xfe2000000);
    assert(fields[0x10][0]==0x88000 && fields[0x11][0]==4096);
    assert(fields[0x70][0]==1 && fields[0x71][0]==1 && fields[0x78][0]==4096);
    assert(fields[0x1110][0]==3 && fields[0x1001][0]==2);
    assert(fields[0x40][0]==2 && fields[0x41][0]==0);
    auto bare=sys;bare.hypervisorType=4;bare.isPassthru=0;
    uint8_t bareRecord[4096]{};uint32_t bareBytes=0;
    assert(buildGmcInit(bareRecord,sizeof(bareRecord),bare,nullptr,0,&bareBytes));
    const auto bareFields=decode(bareRecord+64,bareBytes-64);
    assert(bareFields.at(0x40)[0]==4 && bareFields.at(0x1001)[0]==0);
    for(size_t i=65536;i<data.size();++i) assert(data[i]==0xa5);
    std::vector<uint8_t> small(256,0xa5);
    assert(!buildGmcInit(small.data(),128,sys,nullptr,0,&bytes) && bytes==0);
    for(size_t i=128;i<small.size();++i) assert(small[i]==0xa5);
    auto invalid=sys;invalid.bar1=0;
    assert(!buildGmcInit(data.data(),65536,invalid,nullptr,0,&bytes));
    uint8_t registry[1024]{};uint32_t registryBytes=0,entries=0;
    const char spec[]="RmLogon=1;Example=hello";
    assert(buildRegistry(spec,sizeof(spec)-1,registry,sizeof(registry),&registryBytes,&entries));
    assert(buildGmcInit(data.data(),65536,sys,registry,registryBytes,&bytes));
    auto regFields=decode(data.data()+64,bytes-64);
    assert(regFields[0x3070].size()==2 && regFields[0x3071][0]==1 && regFields[0x3072][0]==6);
    auto broken=std::vector<uint8_t>(registry,registry+registryBytes);
    uint32_t overflow=0xffffffff;std::memcpy(broken.data()+8,&overflow,4);
    assert(!buildGmcInit(data.data(),65536,sys,broken.data(),broken.size(),&bytes));
    assert(buildGmcInit(data.data(),65536,sys,nullptr,0,&bytes));
    std::memcpy(&h,data.data()+24,40);
    h.command|=0x01000000;h.statusOrMaxResponse=0;
    std::memcpy(data.data()+24,&h,40);
    GmcHeader out{};
    assert(parseGmcInitResponse(data.data(),bytes,&out));
    assert(out.sequence==0x474d430000000001ULL && out.statusOrMaxResponse==0);
    assert(!parseGmcInitResponse(data.data(),bytes-1,&out));
    auto bad=data;bad[24]^=1;assert(!parseGmcInitResponse(bad.data(),bytes,&out));
    bad=data;bad[39]^=1;assert(!parseGmcInitResponse(bad.data(),bytes,&out));
    bad=data;bad[44]=1;assert(!parseGmcInitResponse(bad.data(),bytes,&out));
    bad=data;bad[15]=0x25;assert(!parseGmcInitResponse(bad.data(),bytes,&out));
    bad=data;std::memcpy(bad.data()+28,&overflow,4);
    assert(!parseGmcInitResponse(bad.data(),bytes,&out));
    std::puts("GMCAPI 615 INIT: wire layout, 64-bit BAR/sequence, NVKV, registry and malformed bounds PASS");
}
