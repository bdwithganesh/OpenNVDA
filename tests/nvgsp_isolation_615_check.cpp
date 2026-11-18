#include "../drivers/NVGspCore/NVGspIsolation615.hpp"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef NVGSP_PRIMARY_FAULT_HEADERS
#include <class/clc36f.h>
#include <alloc/alloc_channel.h>
static_assert((1?NVC36F_GP_ENTRY1_PRIV)==8 && (0?NVC36F_GP_ENTRY1_PRIV)==8,"primary GPFIFO privilege bit");
static_assert(NVC36F_GP_ENTRY1_PRIV_USER==0 && NVC36F_GP_ENTRY1_PRIV_KERNEL==1,"primary privilege values");
static_assert((1?NVC36F_SEM_EXECUTE_RELEASE_WFI)==20 && NVC36F_SEM_EXECUTE_RELEASE_WFI_EN==1,"primary HOST release wait");
static_assert((1?NVOS04_FLAGS_CHANNEL_DENY_PHYSICAL_MODE_CE)==7,"primary CE physical-mode denial bit");
static_assert((1?NVOS04_FLAGS_CHANNEL_DENY_AUTH_LEVEL_PRIV)==22,"primary privileged HOST method denial bit");
#endif
int main() {
    assert(!nvgsp::isolationFaultableVas615(0));
    assert(!nvgsp::isolationFaultableVas615(1));
    assert(nvgsp::isolationFaultableVas615(2));
    assert(!nvgsp::isolationFaultableVas615(3));
    assert(nvgsp::isolationFaultableVas615(4));
    assert(nvgsp::isolationFaultableVas615(5));
    assert(nvgsp::isolationProbeVa615(5,0)==0x100000000ULL);
    assert(nvgsp::isolationProbeVa615(5,1)==0x200000000ULL);
    assert(nvgsp::isolationProbeVa615(2,1)==0x100000000ULL);
    for(unsigned mode=0;mode<=5;++mode) {
        const uint32_t flags=nvgsp::isolationChannelFlags615(mode);
        assert(flags&(1U<<6));
        assert(bool(flags&(1U<<7))==(mode!=4));
        assert(bool(flags&(1U<<22))==(mode!=4));
        assert(!(flags&(1U<<5))); // GSP-vGPU-plugin-only privilege flag
        assert(nvgsp::isolationInternalFlags615(mode)==(mode==4?0x15U:0x14U));
    }
    uint64_t pte=0;
    for(unsigned page=0;page<3;++page) {
        assert(nvgsp::isolationPte615(0x100000+page*4096,page,false,&pte));
        assert(bool(pte&64)==(page==0) && !(pte&32));
        assert(nvgsp::isolationPte615(0x100000+page*4096,page,true,&pte));
        assert(bool(pte&64)==(page<=1) && !(pte&32));
        assert(nvgsp::privilegedCompletionPte615(0x100000+page*4096,page,&pte));
        assert(bool(pte&64)==(page==0) && bool(pte&32)==(page==2));
    }
    assert(!nvgsp::isolationPte615(0x100000,3,true,&pte));
    // Independent fixed fixture, same COPY/HOST packet as the actual615
    // diagnostic, with high GPUVA bits and unequal fence canaries.
    uint32_t packet[20]={};packet[19]=0xdeadbeef;
    const uint32_t expected[19]={0x20018000,0xc7b5,0x20088100,
        0x28,0xc00,0x28,0xd00,256,256,256,1,0x200180c0,0x186,
        0x20050017,0xe00,0x28,0x6150ca43,0,0x00100001};
    assert(nvgsp::isolationCopy615(0x2800000000ULL,0x6150ca43,packet));
    assert(!memcmp(packet,expected,sizeof(expected)) && packet[19]==0xdeadbeef);
    assert(!nvgsp::isolationCopy615(0,1,packet));
    assert(!nvgsp::isolationCopy615(0x1001,1,packet));
    assert(!nvgsp::isolationCopy615(0x10000000000ULL,1,packet));
    assert(!nvgsp::isolationCopy615(0x1000,0,packet));
    assert(!nvgsp::isolationCopy615(0x1000,1,nullptr));
    assert(nvgsp::isolationCopy615(0xfffffff000ULL,1,packet));
    assert(nvgsp::protectedIsolationCopy615(0x2800000000ULL,0x6150ca43,packet));
    assert(packet[3]==0x28 && packet[4]==0x1c00 && packet[5]==0x28 && packet[6]==0x1d00);
    assert(packet[14]==0x2e00 && packet[15]==0x28 && packet[19]==0xdeadbeef);
    assert(!nvgsp::protectedIsolationCopy615(0xffffffe000ULL,1,packet));
    assert(nvgsp::protectedIsolationCopy615(0xffffffd000ULL,1,packet));
    assert(!nvgsp::privilegedCompletionPte615(0x100000,3,&pte));
    uint32_t entries[5]={0,0,0,0,0xdeadbeef};
    assert(nvgsp::privilegedCompletionEntries615(0x2800000000ULL,entries));
    const uint32_t expectedEntries[4]={0x800,0x28|(13U<<10),0x834,0x28|(1U<<8)|(6U<<10)};
    assert(!memcmp(entries,expectedEntries,sizeof(expectedEntries)) && entries[4]==0xdeadbeef);
    assert(!nvgsp::privilegedCompletionEntries615(0x1001,entries));
    assert(!nvgsp::privilegedCompletionEntries615(0,entries));
    assert(!nvgsp::privilegedCompletionEntries615(0xffffffe000ULL,entries));
    assert(!nvgsp::privilegedCompletionEntries615(0x1000,nullptr));
    assert(nvgsp::crossVasDenialCopy615(0x100000000ULL,0x200000000ULL,0x6150ca43,packet));
    assert(packet[3]==1 && packet[4]==0x1c00 && packet[5]==2 && packet[6]==0x1d00);
    assert(packet[14]==0x2e00 && packet[15]==1 && packet[19]==0xdeadbeef);
    assert(!nvgsp::crossVasDenialCopy615(0x100000000ULL,0x100000000ULL,1,packet));
    assert(!nvgsp::crossVasDenialCopy615(0x100000000ULL,0x200000001ULL,1,packet));
    assert(!nvgsp::crossVasDenialCopy615(0x100000000ULL,0,1,packet));
    assert(!nvgsp::crossVasDenialCopy615(0x100000000ULL,0x200000000ULL,1,nullptr));
    for(unsigned i=0;i<64;++i) {
        assert(nvgsp::isolationCanary615(0,i)!=nvgsp::isolationCanary615(1,i));
        for(unsigned j=0;j<i;++j) {
            assert(nvgsp::isolationCanary615(0,i)!=nvgsp::isolationCanary615(0,j));
            assert(nvgsp::isolationCanary615(1,i)!=nvgsp::isolationCanary615(1,j));
        }
    }
    puts("private-VAS COPY615 packet, canary distinctness, address bounds PASS");
}
