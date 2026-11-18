#include "../drivers/NVGspCore/NVGspLifecycle615.hpp"
#include <cassert>
#include <cstdio>
#include <vector>
#include <fstream>
using namespace nvgsp;
static void put(std::vector<uint8_t>& b,size_t i,uint32_t n) {
    for (unsigned j=0;j<4;++j)b[i+j]=uint8_t(n>>(j*8));
}
static void header(std::vector<uint8_t>& b,uint32_t fn) {
    put(b,24,0x03000000);put(b,28,0x43505256);put(b,32,uint32_t(b.size()-24));put(b,36,fn);
}
int main(int argc,char **argv) {
    std::vector<uint8_t> a(56+32+120,0);header(a,103);
    assert(!buildRootClient615(nullptr,152));
    assert(!buildRootClient615(a.data()+56,151));
    assert(buildRootClient615(a.data()+56,152));
    assert(control615U32(a.data()+56)==kLifecycleClient615);
    assert(control615U32(a.data()+64)==0 && control615U32(a.data()+68)==0);
    assert(control615U32(a.data()+88)==kLifecycleClient615);
    assert(decodeRootClient615(a.data(),a.size()).successful());
    for(size_t i=0;i<a.size();++i)assert(!decodeRootClient615(a.data(),i).successful());
    put(a,72,0x40);assert(!decodeRootClient615(a.data(),a.size()).successful());
    assert(decodeRootClient615(a.data(),a.size()).status==0x40);put(a,72,0);
    put(a,44,1);assert(!decodeRootClient615(a.data(),a.size()).successful());put(a,44,0);
    put(a,64,kLifecycleClient615);assert(!decodeRootClient615(a.data(),a.size()).valid);put(a,64,0);
    put(a,76,UINT32_MAX);assert(!decodeRootClient615(a.data(),a.size()).valid);put(a,76,120);
    put(a,88,kLifecycleClient615+1);assert(!decodeRootClient615(a.data(),a.size()).valid);
    std::vector<uint8_t> f(56+16,0);header(f,10);
    put(f,56,kLifecycleClient615);put(f,64,kLifecycleClient615);
    assert(decodeRootFree615(f.data(),f.size()).successful());
    for(size_t i=0;i<f.size();++i)assert(!decodeRootFree615(f.data(),i).successful());
    put(f,68,0x57);assert(!decodeRootFree615(f.data(),f.size()).successful());
    assert(decodeRootFree615(f.data(),f.size()).status==0x57);put(f,68,0);
    put(f,64,kLifecycleClient615+1);assert(!decodeRootFree615(f.data(),f.size()).valid);
    std::vector<uint8_t> d(56+32+56,0);header(d,103);
    assert(!buildDevice615(d.data()+56,88,0));
    assert(buildDevice615(d.data()+56,88,1u<<31));
    assert(control615U32(d.data()+88)==31);
    assert(decodeAlloc615(d.data(),d.size(),kLifecycleClient615,kLifecycleDevice615,0x80,56).successful());
    for(size_t i=0;i<d.size();++i)
        assert(!decodeAlloc615(d.data(),i,kLifecycleClient615,kLifecycleDevice615,0x80,56).successful());
    assert(!decodeAlloc615(d.data(),d.size(),kLifecycleClient615,kLifecycleDevice615+1,0x80,56).valid);
    assert(!decodeAlloc615(d.data(),d.size(),kLifecycleClient615,kLifecycleDevice615,0x80,48).valid);
    VaspaceParams615 v{};
    assert(!buildAlloc615(d.data()+56,88,kLifecycleDevice615,kLifecycleVaspace615,0x90f1,nullptr,sizeof(v)));
    assert(buildAlloc615(d.data()+56,88,kLifecycleDevice615,kLifecycleVaspace615,0x90f1,&v,sizeof(v)));
    assert(decodeAlloc615(d.data(),d.size(),kLifecycleDevice615,kLifecycleVaspace615,0x90f1,56).successful());
    if(argc>=3) {
        std::ifstream af(argv[1],std::ios::binary),ff(argv[2],std::ios::binary);
        std::vector<uint8_t> ar((std::istreambuf_iterator<char>(af)),{}),fr((std::istreambuf_iterator<char>(ff)),{});
        assert(decodeRootClient615(ar.data(),ar.size()).successful());
        assert(decodeRootFree615(fr.data(),fr.size()).successful());
    }
    if(argc==6) {
        const uint32_t parents[]={kLifecycleClient615,kLifecycleDevice615,kLifecycleDevice615};
        const uint32_t objects[]={kLifecycleDevice615,kLifecycleSubdevice615,kLifecycleVaspace615};
        const uint32_t classes[]={0x80,0x2080,0x90f1},sizes[]={56,4,56};
        for(unsigned i=0;i<3;++i) {
            std::ifstream input(argv[i+3],std::ios::binary);
            std::vector<uint8_t> actual((std::istreambuf_iterator<char>(input)),{});
            assert(decodeAlloc615(actual.data(),actual.size(),parents[i],objects[i],classes[i],sizes[i]).successful());
        }
    }
    std::puts("615 root lifecycle ABI: exact headers/identity, refusal, truncation and private status PASS");
}
