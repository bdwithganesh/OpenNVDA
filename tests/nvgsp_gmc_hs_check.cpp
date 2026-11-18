#include "../drivers/NVGspCore/NVGspGmcHs.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

struct HsIo {
    std::map<uint32_t,uint32_t> r;
    unsigned transfers=0, cpuReads=0;
    bool started=false, haltStuck=false, coreAck=true;
    uint32_t mailboxResult=0;
    int failTransfer=-1;
    bool read(uint32_t off,uint32_t *out) {
        if (off==0x110118) { *out=2; return true; }
        if (off==0x110100 && started) {
            *out=haltStuck || cpuReads++==0 ? 0 : 0x10;
            return true;
        }
        if (off==0x110040 && started) { *out=mailboxResult; return true; }
        *out=r[off]; return true;
    }
    bool write(uint32_t off,uint32_t value) {
        if (off==0x111668) {
            r[off]=(value & ~1U) | (coreAck ? 1U : 0U); return true;
        }
        if (off==0x110118) {
            if (int(transfers)==failTransfer) return false;
            ++transfers;
        }
        if ((off==0x110100 || off==0x110130) && value==2) started=true;
        if ((off==0x840100 || off==0x840130) && value==2) {
            r[0x1180f8]=1U<<26; r[0x840040]=0; r[0x111388]=0x80;
        }
        r[off]=value; return true;
    }
    void delay(uint32_t) {}
};

int main() {
    HsIo core;
    assert(nvgsp::selectGspCore(core,false) && core.r[0x111668]==1);
    assert(nvgsp::selectGspCore(core,true) && core.r[0x111668]==0x111);
    core.coreAck=false;
    assert(!nvgsp::selectGspCore(core,false));
    // Captured signed615.78.08 request from the real AD103, after the
    // sysmem-flush prerequisite was supplied. This is a wire fixture.
    const char *hex="5054434d90000000010000c07ede1026780000000000000003000100500000000000000000000080000000000000000000000000000000000000000000000000003074f503000000007274f50300000000000000000000000000000000000000000100000000000000410000ffffffff0000000000250000101f00000004000001000000000000000000000000000000";
    uint8_t packet[144]{};
    assert(std::strlen(hex)==sizeof(packet)*2);
    for (unsigned i=0;i<sizeof(packet);++i) {
        auto nibble=[](char c) { return unsigned(c <= '9' ? c - '0' : c - 'a' + 10); };
        packet[i]=uint8_t((nibble(hex[i*2])<<4) | nibble(hex[i*2+1]));
    }
    nvgsp::GmcHsParams p{};
    assert(nvgsp::parseGmcHs(packet,sizeof(packet),&p));
    assert(p.imemPhysAddr==17002934272ULL && p.dmemPhysAddr==17002951168ULL);
    assert(p.imemVa==256 && p.imemPa==0 && p.imemSize==16640);
    assert(p.dmemVa==UINT32_MAX && p.dmemSize==9472 && p.signatureDmem==7952);
    assert(p.engineMask==1024 && p.ucodeId==1);
    constexpr uint64_t fb=uint64_t(16376)<<20;
    assert(nvgsp::validGmcHs(p,fb));
    assert(!nvgsp::parseGmcHs(packet,sizeof(packet)-1,&p));
    packet[27]=1; assert(!nvgsp::parseGmcHs(packet,sizeof(packet),&p)); packet[27]=0;
    packet[80]=1; assert(!nvgsp::parseGmcHs(packet,sizeof(packet),&p)); packet[80]=0;
    assert(nvgsp::parseGmcHs(packet,sizeof(packet),&p));

    HsIo io; io.r[0x110040]=0x80000000; io.r[0x110600]=0x10007;
    std::vector<bool> resets;
    auto reset=[&](bool riscv) { resets.push_back(riscv); return true; };
    nvgsp::GmcHsResult result{};
    assert(nvgsp::executeGmcHs(io,p,fb,0x12345000,0x615,reset,&result));
    assert(result.stage==100 && result.dmaTransfers==102 && io.transfers==102);
    assert((resets==std::vector<bool>{false,true}));
    assert(io.r[0x110600]==4 && io.r[0x110104]==256);
    assert(io.r[0x110110]==uint32_t(p.dmemPhysAddr>>8));
    assert(io.r[0x111210]==7952 && io.r[0x11119c]==1024 && io.r[0x111198]==1);
    assert(io.r[0x110040]==0x12345000 && io.r[0x110080]==0x615);

    HsIo interrupted; interrupted.r[0x110040]=0x80000000; interrupted.failTransfer=2;
    resets.clear();
    assert(!nvgsp::executeGmcHs(interrupted,p,fb,0x12345000,0x615,reset,&result));
    assert(result.stage==3 && result.dmaTransfers==2 && resets.size()==1);
    for (bool stuck : {false,true}) {
        HsIo failed; failed.r[0x110040]=0x80000000;
        failed.haltStuck=stuck; failed.mailboxResult=0x90;
        resets.clear();
        assert(!nvgsp::executeGmcHs(failed,p,fb,0x12345000,0x615,reset,&result));
        assert(result.stage==5 && resets.size()==1);
    }
    auto bad=p; bad.imemPhysAddr=fb-256;
    assert(!nvgsp::validGmcHs(bad,fb));
    bad=p; bad.imemSize=0x40001; assert(!nvgsp::validGmcHs(bad,fb));
    bad=p; bad.dmemPa=0xffffff; assert(!nvgsp::validGmcHs(bad,fb));
    bad=p; bad.engineMask=1; assert(!nvgsp::validGmcHs(bad,fb));
    bad=p; bad.signatureDmem=bad.dmemSize; assert(!nvgsp::validGmcHs(bad,fb));
    std::puts("GMC HS: captured ABI, FB/TCM bounds,102DMA, signed Falcon execution, SEC2 resume and failure stages pass");
}
