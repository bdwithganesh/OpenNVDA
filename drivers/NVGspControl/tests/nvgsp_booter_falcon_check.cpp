#include "../../NVGspCore/NVGspBooterFalcon.hpp"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <map>
#include <vector>
static std::vector<unsigned char> load(const char *p) { std::ifstream f(p,std::ios::binary); return {std::istreambuf_iterator<char>(f),{}}; }
struct FakeIo {
    std::map<unsigned,unsigned> r; unsigned transfers=0, cpuReads=0, mailboxArg=0; bool started=false;
    bool read(unsigned o,unsigned *v) {
        if (o==nvgsp::sec2::kDmaCommand) { *v=2; return true; }
        if (o==nvgsp::sec2::kCpuCtl) { *v=(started && cpuReads++==0)?0:16; return true; }
        if (o==nvgsp::sec2::kMailbox0 && started) { *v=0; return true; }
        *v=r[o]; return true;
    }
    bool write(unsigned o,unsigned v) {
        r[o]=v; if (o==nvgsp::sec2::kDmaCommand) ++transfers;
        if (o==nvgsp::sec2::kMailbox0) mailboxArg=v;
        if ((o==nvgsp::sec2::kCpuCtl || o==nvgsp::sec2::kCpuCtlAlias) && v==2) { started=true; cpuReads=0; }
        return true;
    }
    void delay(unsigned) {}
};
int main(int argc,char **argv) {
    assert(argc==2); auto data=load(argv[1]); nvgsp::BooterView view{};
    assert(nvgsp::parseBooterLoad(data.data(),data.size(),&view));
    FakeIo io; io.r[nvgsp::sec2::kFbifCtl]=0x10; io.r[nvgsp::sec2::kFbifTranscfg0]=0x10;
    nvgsp::BooterExecutionResult result{};
    assert(nvgsp::executeBooterLoad(io,view,0x38000000,57344,0x39000000,&result));
    assert(result.dmaTransfers==221 && io.transfers==221);
    assert(io.r[nvgsp::sec2::kBromParaAddr0]==view.patchLocation-view.layout.osDataOffset);
    assert(io.mailboxArg==0x39000000 && result.mailbox0==0);
    std::printf("SEC2 Booter plan: %u DMA blocks, metadata mailbox programmed, status zero\n",result.dmaTransfers);
}
