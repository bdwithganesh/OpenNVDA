#include "../drivers/NVGspCore/NVGspComputePacket615.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
int main(int argc,char **argv) {
    assert(argc==3);
    std::ifstream f(argv[1],std::ios::binary);
    std::vector<uint8_t> fixture{std::istreambuf_iterator<char>(f),{}};
    assert(fixture.size()==256);
    nvgsp::ComputePacket615 p{};
    constexpr uint64_t program=4319322112;
    assert(nvgsp::computePacket615(program,program+0x1000,program+0x2000,program+0x3f00,24,0x51a0c038,&p));
    // Compare all2048 bits to the actual GPU-passing QMD archived from38.
    assert(!std::memcmp(p.qmd,fixture.data(),fixture.size()));
    assert(p.commandCount==14 && p.commands[0]==0x20012000 && p.commands[1]==0xc9c0);
    assert(p.commands[p.commandCount-1]==0x800320b0);
    assert(!nvgsp::computePacket615(program+4,program+0x1000,program+0x2000,program+0x3f00,24,1,&p));
    assert(!nvgsp::computePacket615(program,program+0x1004,program+0x2000,program+0x3f00,24,1,&p));
    assert(!nvgsp::computePacket615(program,program+0x1000,program+0x2004,program+0x3f00,24,1,&p));
    assert(!nvgsp::computePacket615(program,1ULL<<40,program+0x2000,program+0x3f00,24,1,&p));
    assert(!nvgsp::computePacket615(program,program+0x1000,program+0x2000,1ULL<<40,24,1,&p));
    assert(!nvgsp::computePacket615(1ULL<<49,program+0x1000,program+0x2000,program+0x3f00,24,1,&p));
    assert(!nvgsp::computePacket615(program,program+0x1000,program+0x2000,program+0x3f00,0,1,&p));
    assert(!nvgsp::computePacket615(program,program+0x1000,program+0x2000,program+0x3f00,256,1,&p));
    // 0.180.99: grid width and CB0 size change only those two QMD fields.
    nvgsp::ComputePacket615 wide{};
    assert(nvgsp::computePacket615(program,program+0x1000,program+0x2000,program+0x3f00,24,0x51a0c038,&wide,64,48));
    unsigned differing=0;
    for(unsigned i=0;i<64;++i)if(wide.qmd[i]!=p.qmd[i])++differing;
    assert(differing>=1 && differing<=2 && !std::memcmp(wide.commands,p.commands,sizeof(p.commands)));
    assert(!nvgsp::computePacket615(program,program+0x1000,program+0x2000,program+0x3f00,24,1,&wide,0,32));
    assert(!nvgsp::computePacket615(program,program+0x1000,program+0x2000,program+0x3f00,24,1,&wide,65536,32));
    assert(!nvgsp::computePacket615(program,program+0x1000,program+0x2000,program+0x3f00,24,1,&wide,1,40));
    assert(!nvgsp::computePacket615(program,program+0x1000,program+0x2000,program+0x3f00,24,1,&wide,1,272));
    std::ifstream nf(argv[2],std::ios::binary);
    std::vector<uint8_t> actual{std::istreambuf_iterator<char>(nf),{}};
    uint32_t size=0,regs=0;
    assert(actual.size()==464 && nvgsp::computeNak615(actual.data(),actual.size(),&size,&regs));
    assert(size==400 && regs==24);
    assert(!nvgsp::computeNak615(actual.data(),actual.size()-1,&size,&regs));
    auto bad=actual;bad[9*4]=4;assert(!nvgsp::computeNak615(bad.data(),bad.size(),&size,&regs));
    bad=actual;bad[3*4]=16;assert(!nvgsp::computeNak615(bad.data(),bad.size(),&size,&regs));
    bad=actual;bad[5*4]=128;assert(!nvgsp::computeNak615(bad.data(),bad.size(),&size,&regs));
    bad=actual;bad[15*4]=1;assert(!nvgsp::computeNak615(bad.data(),bad.size(),&size,&regs));
    std::puts("Compute615: full GPU-passing QMD fixture, method stream, NAK and bounds checks PASS");
}
