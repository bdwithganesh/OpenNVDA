#include "../drivers/NVGspCore/NVGspDiagnostic615.hpp"
#include "../drivers/NVGspCore/NVGspGmcInit.hpp"
#include "../drivers/NVGspCore/NVGspPramin615.hpp"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
static std::vector<uint8_t> read(const std::string &p) {
    std::ifstream f(p,std::ios::binary);assert(f);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),{});
}
int main(int argc,char **argv) {
    const std::string root=argc==2?argv[1]:"tests/fixtures";
    auto late=read(root+"/late-gr-alloc-refusal-615.78.08.bin");
    assert(late.size()==104 && nvgsp::control615U32(late.data()+40)==0x1a);
    const auto fn=nvgsp::control615U32(late.data()+36),seq=nvgsp::control615U32(late.data()+48);
    assert(fn==103 && seq==0x80000058);
    assert(nvgsp::rpcLateMatch615(true,fn,seq,103,seq,7,7));
    assert(!nvgsp::rpcLateMatch615(false,fn,seq,103,seq,7,7));
    assert(!nvgsp::rpcLateMatch615(true,fn,seq,76,seq,7,7));
    assert(!nvgsp::rpcLateMatch615(true,fn,seq,103,seq+1,7,7));
    assert(!nvgsp::rpcLateMatch615(true,fn,seq,103,seq,7,8));
    assert(!nvgsp::rpcLateMatch615(true,fn,0,103,seq,7,7));
    auto notice=read(root+"/gmc-mmu-fault-notice-615.78.08.bin");
    nvgsp::GmcHeader h{};
    assert(nvgsp::parseGmcMmuNotice615(notice.data(),notice.size(),&h));
    assert(h.command==0x40001 && h.sequence==0x8000000000000001ULL);
    assert(!nvgsp::parseGmcMmuNotice615(notice.data(),notice.size()-1,&h));
    assert(!nvgsp::parseGmcMmuNotice615(nullptr,notice.size(),&h));
    for(size_t at:{size_t(0),size_t(12),size_t(16),size_t(24),size_t(28),size_t(40),size_t(44)}) {
        auto bad=notice;bad[at]^=1;
        assert(!nvgsp::parseGmcMmuNotice615(bad.data(),bad.size(),&h));
    }
    uint32_t memory[1024],out[1024]{};
    for(unsigned i=0;i<1024;++i)memory[i]=0xa0000000U+i;
    unsigned calls=0;
    auto reader=[&](uint64_t at,uint64_t *pair) {
        ++calls;assert(!(at&7) && at<=sizeof(memory)-8);
        std::memcpy(pair,reinterpret_cast<const uint8_t *>(memory)+at,8);return true;
    };
    assert(nvgsp::readPraminDwords615(0,out,4,reader));
    assert(out[0]==0xa0000000 && out[3]==0xa0000003);
    assert(nvgsp::readPraminDwords615(4,out,3,reader));
    assert(out[0]==0xa0000001 && out[2]==0xa0000003);
    // Actual failed GP_PUT read now selects upper half of aligned GET/PUT.
    assert(nvgsp::readPraminDwords615(0x8c,out,1,reader));assert(out[0]==0xa0000023);
    calls=0;assert(nvgsp::readPraminDwords615(0,out,1024,reader));assert(calls==512);
    for(unsigned i=0;i<1024;++i)assert(out[i]==memory[i]);
    calls=0;
    assert(!nvgsp::readPraminDwords615(2,out,1,reader));
    assert(!nvgsp::readPraminDwords615(0,out,0,reader));
    assert(!nvgsp::readPraminDwords615(0,out,1025,reader));
    assert(!nvgsp::readPraminDwords615(0,nullptr,1,reader));
    assert(!nvgsp::readPraminDwords615(UINT64_MAX-3,out,2,reader));assert(calls==0);
    const uint64_t last=(1ULL<<40)-4;
    assert(nvgsp::readPraminDwords615(last,out,1,[&](uint64_t at,uint64_t *pair) {
        assert(at==last-4);*pair=0x1122334455667788ULL;return true;
    }));assert(out[0]==0x11223344);
    assert(!nvgsp::readPraminDwords615(last,out,2,reader));
    assert(!nvgsp::readPraminDwords615(0,out,1,[](uint64_t,uint64_t *) {return false;}));
    std::puts("615 recovery: exact late reply/generation, captured GMC notice, PRAMIN32 alignment/range PASS");
}
