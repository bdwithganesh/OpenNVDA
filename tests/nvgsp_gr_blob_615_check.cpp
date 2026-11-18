#include "../drivers/NVGspCore/NVGspGrBlob615.hpp"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <vector>

int main(int argc,char **argv) {
    assert(argc==2);
    std::ifstream input(argv[1],std::ios::binary);
    std::vector<uint8_t> data{std::istreambuf_iterator<char>(input),{}};
    nvgsp::GrBlob615 p{};
    assert(data.size()==1664 && nvgsp::planGrBlob615(data.data(),data.size(),&p));
    assert(p.count==10 && p.allocationBytes==25165824 && p.logicalBackingBytes==24350720);
    assert(p.buffers[0].id==0 && p.buffers[0].bytes==1357824);
    assert(p.buffers[5].id==5 && p.buffers[5].offset==0 && p.buffers[5].bytes==21641728);
    uint8_t out[560]{};
    constexpr uint64_t pa=0x41600000,va=0x108000000;
    assert(nvgsp::promoteGrBlob615(p,0xc0d00001,0xc0d00056,pa,va,out,sizeof(out)));
    assert(nvgsp::control615U32(out+40)==10);
    auto u64=[](const uint8_t *s) {return uint64_t(nvgsp::control615U32(s)) | uint64_t(nvgsp::control615U32(s+4))<<32;};
    for(unsigned i=0;i<p.count;++i) {
        const auto &b=p.buffers[i];
        const auto *entry=out+48+i*32;
        assert(u64(entry)==pa+b.offset);
        assert(u64(entry+8)==va+b.offset);
        assert(u64(entry+16)==b.bytes);
        assert(nvgsp::control615U32(entry+24)==4 && entry[28]==b.id && entry[29]==0);
        assert(entry[30]==b.initialize && entry[31]==0);
    }
    assert(nvgsp::promoteGrBlob615(p,1,2,pa,va,out,sizeof(out),false));
    for(unsigned i=0;i<p.count;++i)assert(out[48+i*32+30]==(p.buffers[i].id<=2));
    assert(!nvgsp::promoteGrBlob615(p,1,2,pa+4096,va,out,sizeof(out)));
    assert(!nvgsp::promoteGrBlob615(p,1,2,pa,va+4096,out,sizeof(out)));
    assert(!nvgsp::promoteGrBlob615(p,1,2,1ULL<<40,va,out,sizeof(out)));
    assert(!nvgsp::promoteGrBlob615(p,1,2,pa,1ULL<<49,out,sizeof(out)));
    auto bad=p;bad.buffers[1].offset=bad.buffers[0].offset;
    assert(!nvgsp::promoteGrBlob615(bad,1,2,pa,va,out,sizeof(out)));
    bad=p;bad.buffers[1].id=0;
    assert(!nvgsp::promoteGrBlob615(bad,1,2,pa,va,out,sizeof(out)));
    bad=p;bad.buffers[1].backingBytes=p.allocationBytes;
    assert(!nvgsp::promoteGrBlob615(bad,1,2,pa,va,out,sizeof(out)));
    assert(!nvgsp::planGrBlob615(nullptr,1664,&bad));
    assert(!nvgsp::planGrBlob615(data.data(),1663,&bad));
    auto corrupt=data;corrupt[4]=3;corrupt[5]=corrupt[6]=corrupt[7]=0;
    assert(!nvgsp::planGrBlob615(corrupt.data(),corrupt.size(),&bad));
    corrupt=data;corrupt[3]=0x7f;
    assert(!nvgsp::planGrBlob615(corrupt.data(),corrupt.size(),&bad));
    // Physical allocation is only2MiB aligned. A future firmware request
    // requiring64MiB alignment must fail promotion unless the actual PA fits.
    bad=p;bad.buffers[5].alignment=64U<<20;
    assert(!nvgsp::promoteGrBlob615(bad,1,2,pa,va,out,sizeof(out)));
    std::puts("GR blob615: actual firmware fixture, promotion, reuse, bounds and ownership checks PASS");
}
