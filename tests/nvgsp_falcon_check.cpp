#include "../drivers/NVGspCore/NVGspFalcon.hpp"
#include "../drivers/NVGspCore/NVGspVbios.hpp"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <map>
#include <vector>
static std::vector<unsigned char> load(const char *p) { std::ifstream f(p,std::ios::binary); return {std::istreambuf_iterator<char>(f),{}}; }
struct FakeIo {
    std::map<unsigned,unsigned> r; unsigned transfers=0, cpuReads=0;
    bool read(unsigned o,unsigned *v) {
        if (o==nvgsp::falcon::kDmaCommand) { *v=2; return true; }
        if (o==nvgsp::falcon::kCpuCtl) { *v=cpuReads++ ? 16 : 16; if (started && cpuReads==2) *v=0; return true; }
        *v=r[o]; return true;
    }
    bool write(unsigned o,unsigned v) {
        r[o]=v; if (o==nvgsp::falcon::kDmaCommand) ++transfers;
        if ((o==nvgsp::falcon::kCpuCtl || o==nvgsp::falcon::kCpuCtlAlias) && v==2) { started=true; cpuReads=0; }
        return true;
    }
    void delay(unsigned) {}
    bool started=false;
};
int main(int argc,char **argv) {
    assert(argc==2); auto rom=load(argv[1]); nvgsp::VbiosFwsecView vb{};
    assert(nvgsp::parseVbiosFwsec(rom.data(),rom.size(),&vb));
    FakeIo io; constexpr unsigned long long frts=0x3ff600000ULL;
    io.r[nvgsp::falcon::kFbifCtl]=0x110; io.r[nvgsp::falcon::kFbifTranscfg0]=0x110;
    io.r[nvgsp::falcon::kWpr2Lo]=unsigned((frts>>12)<<4); io.r[nvgsp::falcon::kWpr2Hi]=0x3ff7000;
    nvgsp::FwsecExecutionResult result{};
    assert(nvgsp::executeFwsecFrts(io,vb.fwsec,0x17cbca8000ULL,vb.dmaImageSize,frts,&result));
    assert(result.dmaTransfers==261 && io.transfers==261);
    assert(io.r[nvgsp::falcon::kFbifCtl]==0x190 && io.r[nvgsp::falcon::kFbifTranscfg0]==0x115);
    assert(io.r[nvgsp::falcon::kBromParaAddr0]==2852 && io.r[nvgsp::falcon::kBromEngineMask]==0x400);
    assert(io.r[nvgsp::falcon::kBromUcodeId]==9 && io.r[nvgsp::falcon::kBromModSel]==1);
    printf("Falcon FWSEC plan: %u DMA blocks, FRTS 0x%llx, WPR2 verified\n",result.dmaTransfers,frts);
}
