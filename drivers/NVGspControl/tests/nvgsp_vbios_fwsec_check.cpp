#include "../../NVGspCore/NVGspVbios.hpp"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <vector>
static std::vector<unsigned char> read(const char *p) { std::ifstream f(p,std::ios::binary); return {std::istreambuf_iterator<char>(f),{}}; }
int main(int argc,char **argv) {
    assert(argc==2); auto rom=read(argv[1]); nvgsp::VbiosFwsecView v{};
    assert(nvgsp::parseVbiosFwsec(rom.data(),rom.size(),&v));
    assert(v.biosSize==629760 && v.expansionOffset==86016 && v.bitOffset==432);
    assert(v.falconTableOffset==629140 && v.descriptorOffset==284012 && v.targetId==7);
    assert(v.fwsec.desc.storedSize==66688 && v.dmaImageSize==66816 && v.fwsec.desc.ucodeId==9);
    std::vector<unsigned char> broken=rom; broken[432]=0;
    nvgsp::VbiosFwsecView bad{}; assert(!nvgsp::parseVbiosFwsec(broken.data(),broken.size(),&bad));
    printf("VBIOS FWSEC: bios %u expansion 0x%x table 0x%x desc 0x%x image %u\n",
           v.biosSize,v.expansionOffset,v.falconTableOffset,v.descriptorOffset,v.fwsec.desc.storedSize);
}
