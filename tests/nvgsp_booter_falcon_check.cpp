#include "../drivers/NVGspCore/NVGspBooterFalcon.hpp"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <map>
#include <vector>
static std::vector<unsigned char> load(const char *p) { std::ifstream f(p,std::ios::binary); return {std::istreambuf_iterator<char>(f),{}}; }
struct FakeIo {
    std::map<unsigned,unsigned> r; unsigned transfers=0, cpuReads=0, mailboxArg=0;
    bool started=false, stuckHalted=false; int failTransferAt=-1;
    unsigned requiredBootVector=0;
    bool read(unsigned o,unsigned *v) {
        if (o==nvgsp::sec2::kDmaCommand) { *v=2; return true; }
        if (o==nvgsp::sec2::kCpuCtl) { *v=(!stuckHalted && started && cpuReads++==0)?0:16; return true; }
        if (o==nvgsp::sec2::kMailbox0 && started) { *v=0; return true; }
        *v=r[o]; return true;
    }
    bool write(unsigned o,unsigned v) {
        if (o==nvgsp::sec2::kDmaCommand && int(transfers)==failTransferAt) return false;
        r[o]=v; if (o==nvgsp::sec2::kDmaCommand) ++transfers;
        if (o==nvgsp::sec2::kMailbox0) mailboxArg=v;
        if ((o==nvgsp::sec2::kCpuCtl || o==nvgsp::sec2::kCpuCtlAlias) && v==2) {
            started=(r[nvgsp::sec2::kBootVec]==requiredBootVector); cpuReads=0;
        }
        return true;
    }
    void delay(unsigned) {}
};
int main(int argc,char **argv) {
    assert(argc==2); auto data=load(argv[1]); nvgsp::BooterView view{};
    assert(nvgsp::parseBooterLoad(data.data(),data.size(),&view));
    const unsigned allocation=(view.imageSize+4095u)&~4095u;
    const unsigned expectedTransfers=(view.layout.appCodeSize+view.layout.osDataSize)/256;
    assert(view.layout.appCodeOffset != 0);
    FakeIo io; io.requiredBootVector=view.layout.appCodeOffset;
    io.r[nvgsp::sec2::kFbifCtl]=0x10; io.r[nvgsp::sec2::kFbifTranscfg0]=0x10;
    nvgsp::BooterExecutionResult result{};
    assert(nvgsp::executeBooterLoad(io,view,0x38000000,allocation,0x39000000,&result));
    assert(result.dmaTransfers==expectedTransfers && io.transfers==expectedTransfers);
    assert(io.r[nvgsp::sec2::kBromParaAddr0]==view.patchLocation-view.layout.osDataOffset);
    assert(io.mailboxArg==0x39000000 && result.mailbox0==0);
    assert(io.r[nvgsp::sec2::kBootVec]==view.layout.appCodeOffset);
    // A halted CPU after all DMA must report the real completed DMA count,
    // rather than the old all-zero result that obscured live bring-up failures.
    FakeIo halted; halted.requiredBootVector=view.layout.appCodeOffset; halted.stuckHalted=true; unsigned stage=0;
    assert(!nvgsp::executeBooterLoad(halted,view,0x38000000,allocation,0x39000000,&result,&stage));
    assert(stage==6 && result.dmaTransfers==expectedTransfers && halted.transfers==expectedTransfers);
    // Preserve partial progress when the third IMEM block cannot be written.
    FakeIo interrupted; interrupted.failTransferAt=2;
    assert(!nvgsp::executeBooterLoad(interrupted,view,0x38000000,allocation,0x39000000,&result,&stage));
    assert(stage==3 && result.dmaTransfers==2 && interrupted.transfers==2);
    assert(!nvgsp::executeBooterLoad(interrupted,view,0x38000001,allocation,0x39000000,&result,&stage));
    assert(stage==1 && result.dmaTransfers==0);
    std::printf("SEC2 Booter: nominal %u DMA blocks; halt, partial-DMA and invalid-input telemetry pass\n",io.transfers);
}
