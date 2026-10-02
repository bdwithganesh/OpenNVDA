#include "../drivers/NVGspCore/NVGspFwsec.hpp"
#include <cassert>
#include <fstream>
#include <vector>
#include <cstdio>
static std::vector<unsigned char> read(const char *p) { std::ifstream f(p,std::ios::binary); return {std::istreambuf_iterator<char>(f),{}}; }
int main(int argc,char **argv) {
    assert(argc==3); auto d=read(argv[1]), i=read(argv[2]); nvgsp::FwsecView v{};
    assert(nvgsp::parseFwsecV3(d.data(),d.size(),i.data(),i.size(),&v));
    assert(v.desc.ucodeId==9 && v.desc.signatureCount==2 && v.desc.signatureVersions==3);
    assert(nvgsp::fwsecSignatureForFuse(v,0)==v.signatures);
    assert(nvgsp::fwsecSignatureForFuse(v,1)==v.signatures+384);
    assert(nvgsp::fwsecSignatureForFuse(v,2)==nullptr);
    constexpr uint64_t frts = 0x3ff600000ULL;
    std::vector<unsigned char> patched(v.desc.storedSize);
    nvgsp::FwsecPatchInfo p{};
    assert(nvgsp::patchFwsecFrts(v,1,frts,patched.data(),patched.size(),&p));
    assert(p.selectedSignature==1 && p.signatureOffset==2852 && p.interfaceOffset==28);
    assert(p.mapperOffset==0xae0 && p.commandOffset==0xd40);
    const auto *dmem=patched.data()+v.desc.imemLoadSize;
    assert(!__builtin_memcmp(dmem+p.signatureOffset,v.signatures+384,384));
    nvgsp::FalconDmemMapperV3 mapper{};
    __builtin_memcpy(&mapper,dmem+p.mapperOffset,sizeof(mapper));
    assert(mapper.initCommand==0x15);
    nvgsp::FwsecFrtsCommand command{};
    __builtin_memcpy(&command,dmem+p.commandOffset,sizeof(command));
    assert(command.readVbios.version==1 && command.readVbios.size==24 && command.readVbios.flags==2);
    assert(command.region.version==1 && command.region.size==20);
    assert(command.region.offset4K==(frts>>12) && command.region.size4K==0x100 && command.region.mediaType==2);
    assert(!nvgsp::patchFwsecFrts(v,2,frts,patched.data(),patched.size()));
    assert(!nvgsp::patchFwsecFrts(v,1,frts+1,patched.data(),patched.size()));
    printf("FWSEC: image %u, IMEM %u, DMEM %u, fuse 1 -> sig %u, mapper 0x%x, command 0x%x\n",v.desc.storedSize,v.desc.imemLoadSize,v.desc.dmemLoadSize,p.selectedSignature,p.mapperOffset,p.commandOffset);
}
