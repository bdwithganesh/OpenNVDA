#include "../drivers/NVGspCore/NVGspClientGr615.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
static uint64_t u64(const uint8_t *p) {uint64_t v=0;for(int i=7;i>=0;--i)v=v<<8|p[i];return v;}
int main(int argc,char **argv) {
    using namespace nvgsp;
    assert(argc==2);
    std::ifstream f(argv[1],std::ios::binary);assert(f.good());
    std::vector<uint8_t> info((std::istreambuf_iterator<char>(f)),{});
    GrBlob615 plan{};assert(planGrBlob615(info.data(),info.size(),&plan));
    ClientGrLayout615 l{};assert(clientGrLayout615(plan,&l));
    uint64_t perContext=0;
    for(uint32_t i=0;i<plan.count;++i)if(plan.buffers[i].id<=2)perContext+=plan.buffers[i].backingBytes;
    assert(l.perContextBytes==perContext && l.scratchOffset==perContext);
    assert(l.clientBytes>=perContext+kClientGrScratchBytes615 && !(l.clientBytes&((2ULL<<20)-1)));
    assert(l.clientBytes<plan.allocationBytes);
    assert(l.scratchVaOffset==((plan.logicalBackingBytes+4095)&~4095ULL));
    const uint64_t root=0x40000000ULL,client=0x42000000ULL,va=0x204000000ULL;
    // Every page of every buffer resolves: globals to root, per-context to client.
    uint32_t privileged=0,localPages=0,globalPages=0;
    for(uint32_t i=0;i<plan.count;++i) {
        const auto &b=plan.buffers[i];
        for(uint64_t off=0;off<b.backingBytes;off+=4096) {
            uint64_t phys=0;bool priv=false;
            assert(clientGrPage615(plan,l,root,client,b.offset+off,&phys,&priv) && priv);++privileged;
            if(b.id<=2) {assert(phys==client+l.clientOffset[b.id]+off && phys<client+l.perContextBytes);++localPages;}
            else {assert(phys==root+b.offset+off);++globalPages;}
        }
    }
    assert(localPages*4096ULL==l.perContextBytes);
    for(uint64_t off=0;off<kClientGrScratchBytes615;off+=4096) {
        uint64_t phys=0;bool priv=true;
        assert(clientGrPage615(plan,l,root,client,l.scratchVaOffset+off,&phys,&priv) && !priv);
        assert(phys==client+l.scratchOffset+off && phys+4096<=client+l.clientBytes);
    }
    uint64_t phys=0;bool priv=false;
    assert(!clientGrPage615(plan,l,root,client,l.scratchVaOffset+kClientGrScratchBytes615,&phys,&priv));
    assert(!clientGrPage615(plan,l,root,client,1,&phys,&priv));
    uint8_t promote[560];
    assert(promoteClientGrBlob615(plan,l,0xc0d00001,0xc0d00356,root,client,va,promote,sizeof(promote)));
    assert(promote[0]==1 && u64(promote+40)==plan.count);
    for(uint32_t i=0;i<plan.count;++i) {
        const auto &b=plan.buffers[i];const uint8_t *e=promote+48+i*32;
        assert(u64(e+8)==va+b.offset && e[28]==b.id && !e[31]);
        if(b.id<=2)assert(u64(e)==client+l.clientOffset[b.id] && u64(e+16)==b.bytes && e[24]==4 && e[30]==b.initialize);
        else assert(!u64(e) && !u64(e+16) && !e[24] && !e[30]); // RM VA-only global entry
    }
    // Root's own encoding of the same globals uses the same VAs.
    uint8_t rootPromote[560];
    assert(promoteGrBlob615(plan,0xc0d00001,0xc0d00056,root,va,rootPromote,sizeof(rootPromote)));
    for(uint32_t i=0;i<plan.count;++i)if(plan.buffers[i].id>2)
        assert(!std::memcmp(rootPromote+48+i*32+8,promote+48+i*32+8,8));
    assert(!promoteClientGrBlob615(plan,l,0xc0d00001,0xc0d00356,root,root,va,promote,560));
    assert(!promoteClientGrBlob615(plan,l,0xc0d00001,0xc0d00356,root,root+(2ULL<<20),va,promote,560));
    assert(!promoteClientGrBlob615(plan,l,0xc0d00001,0xc0d00356,root,client,va+4096,promote,560));
    assert(!promoteClientGrBlob615(plan,l,0xc0d00001,0xc0d00356,root,client+4096,va,promote,560));
    assert(!promoteClientGrBlob615(plan,l,0,0xc0d00356,root,client,va,promote,560));
    assert(!promoteClientGrBlob615(plan,l,0xc0d00001,0xc0d00356,root,client,va,promote,559));
    // Local-globals variant: every buffer client-backed at root offsets.
    ClientGrLayout615 full{};assert(clientGrLayout615(plan,&full,true));
    assert(full.clientBytes==plan.allocationBytes && full.scratchOffset==full.scratchVaOffset);
    for(uint32_t i=0;i<plan.count;++i) {
        const auto &b=plan.buffers[i];assert(full.local[b.id] && full.clientOffset[b.id]==b.offset);
        uint64_t ph=0;bool pv=false;
        assert(clientGrPage615(plan,full,root,0x48000000ULL,b.offset,&ph,&pv) && pv && ph==0x48000000ULL+b.offset);
    }
    uint8_t localPromote[560];
    assert(promoteClientGrBlob615(plan,full,0xc0d00001,0xc0d00356,root,0x48000000ULL,va,localPromote,560));
    for(uint32_t i=0;i<plan.count;++i) {
        const auto &b=plan.buffers[i];const uint8_t *e=localPromote+48+i*32;
        assert(u64(e)==0x48000000ULL+b.offset && e[30]==b.initialize);
    }
    // PM-less promote: one fewer entry, ids still ascending, PM absent.
    uint8_t noPm[560];
    assert(promoteClientGrBlob615(plan,l,0xc0d00001,0xc0d00356,root,client,va,noPm,560,false));
    assert(u64(noPm+40)==plan.count-1);
    for(uint32_t i=0,last=0;i<plan.count-1;++i) {
        const uint8_t *e=noPm+48+i*32;assert(e[28]!=1 && (i==0 || e[28]>last));last=e[28];
        assert(u64(e+8)>=va && (e[28]<=2 ? e[24]==4 : !e[24]));
    }
    assert(!u64(noPm+48+(plan.count-1)*32+8));
    // Subcontext layout: every GR page is root's, scratch is the client's 2MiB.
    ClientGrLayout615 sub{};assert(clientGrLayout615(plan,&sub,false,true));
    assert(!sub.perContextBytes && !sub.scratchOffset && sub.clientBytes==(2ULL<<20));
    for(uint32_t i=0;i<plan.count;++i) {
        const auto &b=plan.buffers[i];uint64_t ph=0;bool pv=false;assert(!sub.local[b.id]);
        assert(clientGrPage615(plan,sub,root,client,b.offset,&ph,&pv) && pv && ph==root+b.offset);
    }
    {uint64_t ph=0;bool pv=true;assert(clientGrPage615(plan,sub,root,client,sub.scratchVaOffset,&ph,&pv) && !pv && ph==client);}
    uint8_t subPromote[560];
    assert(promoteClientGrBlob615(plan,sub,0xc0d00001,0xc0d00356,root,client,va,subPromote,560,true));
    assert(u64(subPromote+40)==plan.count);
    for(uint32_t i=0;i<plan.count;++i) {
        const uint8_t *e=subPromote+48+i*32;
        assert(!u64(e) && !u64(e+16) && !e[24] && !e[30] && u64(e+8)==va+plan.buffers[i].offset);
    }
    assert(!clientGrLayout615(plan,&sub,true,true));
    // Overlap with root's blob is refused for the 24MiB client too.
    assert(!promoteClientGrBlob615(plan,full,0xc0d00001,0xc0d00356,root,root+(2ULL<<20),va,localPromote,560));
    std::printf("ClientGR615: %u buffers, per-context %u pages client-owned, %u global pages shared from root, scratch USER, promote encoding, local-globals, PM-less, subcontext variants PASS\n",
        plan.count,localPages,globalPages);
    (void)privileged;
}
