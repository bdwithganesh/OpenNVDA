#include "../drivers/NVGspCore/NVGspGmcConfig.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

// Locate a wire instruction independently of the kernel parser. Also used
// to mutate real captured data into malformed responses.
static size_t field(const std::vector<uint8_t> &b,unsigned key,unsigned index=0) {
    for(size_t off=0;off<b.size();) {
        uint64_t h=0;std::memcpy(&h,b.data()+off,8);
        if ((h&65535)==key && ((h>>16)&4095)==index) return off;
        off+=8;const unsigned op=(h>>28)&15,count=h>>32;
        if(op) {const size_t unit=op==3?1:(op==1||op==4?4:8);off+=(size_t(count)*unit+7)&~size_t(7);}
    }
    assert(false);return 0;
}
static void put64(std::vector<uint8_t> &b,size_t off,uint64_t v) {std::memcpy(b.data()+off,&v,8);}
int main(int argc,char **argv) {
    const char *path=argc>1?argv[1]:"tests/fixtures/gsp-init-ad103-615.78.08.bin";
    std::ifstream f(path,std::ios::binary);
    std::vector<uint8_t> b{std::istreambuf_iterator<char>(f),{}};
    assert(b.size()==944);
    nvgsp::GmcStaticConfig c{};
    assert(nvgsp::parseGmcStaticConfig(b.data(),b.size(),&c));
    assert(std::strcmp(c.gpuName,"NVIDIA GeForce RTX 4080")==0);
    assert(c.regionCount==6 && c.fbBytes==17171480576ULL);
    assert(c.bar1Pde==17001660416ULL && c.bar2Pde==17005355008ULL);
    assert(c.internalClient==3254779911U && c.internalDevice==2882338944U && c.internalSubdevice==2882347136U);
    assert(c.vmmuSegmentBytes==67108864);
    assert(c.regions[2].base==134742016 && c.regions[2].limit==16954621951ULL);
    assert(c.regions[2].reserved==0 && c.regions[2].flags==3 && c.regions[2].tag==0);
    // The required region fields end at904. Complete optional trailing keys
    // may be omitted; a cut inside an instruction or required field is invalid.
    for(size_t n=0;n<904;++n) assert(!nvgsp::parseGmcStaticConfig(b.data(),n,&c));
    assert(nvgsp::parseGmcStaticConfig(b.data(),904,&c));
    assert(nvgsp::parseGmcStaticConfig(b.data(),920,&c));
    for(size_t n=905;n<944;++n)
        if(n!=920) assert(!nvgsp::parseGmcStaticConfig(b.data(),n,&c));
    auto bad=b;bad[7]=0xff;assert(!nvgsp::parseGmcStaticConfig(bad.data(),bad.size(),&c));
    bad=b;bad[3]=0xf0;assert(!nvgsp::parseGmcStaticConfig(bad.data(),bad.size(),&c));
    // Duplicate a critical field, while retaining otherwise valid framing.
    bad=b;const size_t pde=field(b,0x1020);
    const size_t duplicate=bad.size();bad.resize(duplicate+8);
    put64(bad,duplicate,(uint64_t(3254779911U)<<32)|0xff0);
    assert(!nvgsp::parseGmcStaticConfig(bad.data(),bad.size(),&c));
    bad=b;put64(bad,pde+8,~0ULL);assert(!nvgsp::parseGmcStaticConfig(bad.data(),bad.size(),&c));
    bad=b;put64(bad,pde+8,0);assert(!nvgsp::parseGmcStaticConfig(bad.data(),bad.size(),&c));
    bad=b;const size_t region=field(b,0x1011,2);bad[region+2]=16;
    assert(!nvgsp::parseGmcStaticConfig(bad.data(),bad.size(),&c));
    bad=b;put64(bad,region+8,0);assert(!nvgsp::parseGmcStaticConfig(bad.data(),bad.size(),&c));
    bad=b;put64(bad,region+16,17171480576ULL);
    assert(!nvgsp::parseGmcStaticConfig(bad.data(),bad.size(),&c));
    // Unknown stable keys are skipped after their full bounded instruction.
    bad=b;const uint64_t unknown=(uint64_t(99)<<32)|0x7fff;
    const size_t end=bad.size();bad.resize(end+8);put64(bad,end,unknown);
    assert(nvgsp::parseGmcStaticConfig(bad.data(),bad.size(),&c));
    assert(!nvgsp::parseGmcStaticConfig(nullptr,944,&c));
    assert(!nvgsp::parseGmcStaticConfig(b.data(),944,nullptr));
    std::puts("GMC config: actual944-byte AD103 response, handles/PDEs/6regions, truncation, duplicates, bounds and unknown keys pass");
}
