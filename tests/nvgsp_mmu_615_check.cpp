#include "../drivers/NVGspCore/NVGspMmu615.hpp"
#include <cassert>
#include <cstdio>
#ifdef NVGSP_PRIMARY_MMU_HEADERS
#include "dev_mmu.h"
static_assert((1 ? NV_MMU_VER2_PTE_VALID)==0,"primary valid bit");
static_assert((1 ? NV_MMU_VER2_PTE_PRIVILEGE)==5,"primary privilege bit");
static_assert((1 ? NV_MMU_VER2_PTE_READ_ONLY)==6,"primary read-only bit");
static_assert((1 ? NV_MMU_VER2_PTE_ADDRESS_VID)==32 &&
              (0 ? NV_MMU_VER2_PTE_ADDRESS_VID)==8,"primary VID address width");
#endif
int main() {
    uint64_t p=0;
    assert(nvgsp::vidPte615(0x40000000,false,false,&p) && p==0x04000001);
    assert(nvgsp::vidPte615(0x40000000,true,false,&p) && p==0x04000021);
    assert(nvgsp::vidPte615(0x40000000,false,true,&p) && p==0x04000041);
    assert(nvgsp::vidPte615((1ULL<<37)-4096,true,true,&p));
    assert(((p>>33)&7)==0 && ((p>>8)&((1ULL<<25)-1))==((1ULL<<25)-1));
    assert(!nvgsp::vidPte615(1ULL<<37,false,false,&p));
    assert(!nvgsp::vidPte615(0,false,false,&p));
    assert(!nvgsp::vidPte615(0x40000004,false,false,&p));
    assert(!nvgsp::vidPte615(0x40000000,false,false,nullptr));
    std::puts("MMU615: VID encoding, privileged/read-only flags, peer isolation and bounds PASS");
}
