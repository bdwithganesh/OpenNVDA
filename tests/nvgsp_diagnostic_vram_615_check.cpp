#include "../drivers/NVGspCore/NVGspDiagnosticVram615.hpp"
#include "../drivers/NVGspCore/NVGspVramHeap.hpp"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <vector>

int main(int argc,char **argv) {
    const char *path=argc>1?argv[1]:"tests/fixtures/gsp-init-ad103-615.78.08.bin";
    std::ifstream input(path,std::ios::binary);
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input),{}};
    nvgsp::GmcStaticConfig config{};
    assert(bytes.size()==944 && nvgsp::parseGmcStaticConfig(bytes.data(),bytes.size(),&config));
    uint64_t start=0,end=0,size=0,at=0;
    assert(nvgsp::diagnosticVramBounds615(config,&start,&end));
    assert(start==1073741824 && end==16886267904ULL);
    assert(nvgsp::diagnosticVramSize615(21643264,0,0,&size) && size==23068672);
    nvgsp::VramHeap<8> heap{};
    assert(heap.insert(start,2ULL<<20));
    assert(heap.fit(start,end,size,&at) && at==start+(2ULL<<20));
    assert(heap.insert(at,size));
    uint64_t second=0;
    assert(heap.fit(start,end,size,&second) && second==at+size);
    assert(!heap.insert(at+4096,4096));
    assert(second+size<=end);
    assert(nvgsp::diagnosticVramSize615(4096,0,0,&size) && size==(2ULL<<20));
    for(uint64_t bad:{0ULL,1ULL,4095ULL,134221824ULL,~0ULL})
        assert(!nvgsp::diagnosticVramSize615(bad,0,0,&size));
    assert(!nvgsp::diagnosticVramSize615(4096,nvgsp::kDiagnosticVramBudget615,0,&size));
    assert(!nvgsp::diagnosticVramSize615(4096,~0ULL,0,&size));
    assert(!nvgsp::diagnosticVramSize615(4096,0,nvgsp::kDiagnosticVramSlots615,&size));
    assert(!nvgsp::diagnosticVramSize615(4096,0,0,nullptr));
    auto bad=config;
    bad.regions[2].tag=5;assert(!nvgsp::diagnosticVramBounds615(bad,&start,&end));
    bad=config;bad.regions[2].flags|=4;assert(!nvgsp::diagnosticVramBounds615(bad,&start,&end));
    bad=config;bad.regions[2].reserved=4096;assert(!nvgsp::diagnosticVramBounds615(bad,&start,&end));
    bad=config;bad.regions[2].seen=0;assert(!nvgsp::diagnosticVramBounds615(bad,&start,&end));
    bad=config;bad.regionCount=17;assert(!nvgsp::diagnosticVramBounds615(bad,&start,&end));
    bad=config;bad.regions[2].limit=~0ULL;assert(!nvgsp::diagnosticVramBounds615(bad,&start,&end));
    bad=config;bad.regions[2].base=bad.regions[2].limit;assert(!nvgsp::diagnosticVramBounds615(bad,&start,&end));
    assert(!nvgsp::diagnosticVramBounds615(config,nullptr,&end));
    std::puts("Diagnostic VRAM: captured AD103 free region,22MiB allocation, overlap, quotas, protected/reserved and overflow checks pass");
}
