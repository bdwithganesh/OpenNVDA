#pragma once
#include "NVGspMmu615.hpp"
#include <stddef.h>
namespace nvgsp {
// Pinned nvos.h: external ownership bit3 and page faulting bit6. An external
// VAS must receive a complete owned root through DMA_SET_PAGE_DIRECTORY.
constexpr uint32_t kExternalFaultVasFlags615=0x48;
constexpr uint64_t kExternalTreeBytes615=5*4096;
struct SetPageDirectory615 {
    uint64_t physical;
    uint32_t entries,flags,vas,channel,subdevice,pasid;
};
static_assert(sizeof(SetPageDirectory615)==32,"615 set-page-directory ABI");
struct ExternalSmallTree615 {
    uint64_t address[4],value[4],leaf;
};
// GA10x/Ada49-bit hierarchy from kern_gmmu_fmt_ga10x.c/GP10X:
// PD3[48:47], PD2[46:38], PD1[37:29], dualPD0[28:21], SPT[20:12].
// Five private4KiB pages map one bounded three-page test window. All other
// entries remain zero; there is no sparse/global/foreign mapping here.
inline bool externalSmallTree615(uint64_t tree,uint64_t va,ExternalSmallTree615 *out) {
    if(!out)return false;
    *out={};
    if(!tree || (tree&4095) || tree>=(1ULL<<37) || kExternalTreeBytes615>(1ULL<<37)-tree ||
       !va || (va&4095) || va>=(1ULL<<49) || (va&0x1fffff)>0x1fd000)return false;
    const uint64_t node[4]={tree+4096,tree+8192,tree+12288,tree+16384};
    out->address[0]=tree+((va>>47)&3)*8;
    out->address[1]=node[0]+((va>>38)&511)*8;
    out->address[2]=node[1]+((va>>29)&511)*8;
    out->address[3]=node[2]+((va>>21)&255)*16+8;
    for(unsigned i=0;i<4;++i)out->value[i]=((node[i]>>12)<<8)|2; // VID PDE, peer0
    out->leaf=node[3]+((va>>12)&511)*8;
    return true;
}
// Owned tree for one contiguous VA span: PD3, PD2, PD1, one dual PD0 per
// PD1 slot (512 MiB) the span touches, then one 4KiB small-page table per
// 2MiB chunk. 0.180.107: a PD0 holds 256 dual entries (one PD1 slot).
// 0.180.118: up to 8 PD1 slots (4 GiB) inside one PD2 entry; with one slot the
// layout is the old one (tables at tree+16 KiB).
// 0.180.157: 16 PD1 slots (8 GiB of 2 MiB tables) for the 2 GiB wrap window
// 0.180.168: 32 PD1 slots (16 GiB) for the 12 GiB wrap window (30 slots)
constexpr uint32_t kExternalTreeMaxSlots615=32,kExternalTreeMaxTables615=256*kExternalTreeMaxSlots615;
struct ExternalTree615 {
    uint64_t tree,lo,hi,treeBytes,tableBase;
    uint32_t tables,firstChunk,slots,upperCount;
    uint64_t upperAddress[2+kExternalTreeMaxSlots615],upperValue[2+kExternalTreeMaxSlots615];
    uint64_t pd0Address[kExternalTreeMaxTables615],pd0Value[kExternalTreeMaxTables615];
};
// the header (everything before the PD0 entry lists), for a registry property
constexpr size_t kExternalTreeHeader615=offsetof(ExternalTree615,pd0Address);
inline uint32_t externalTreeTables615(uint64_t lo,uint64_t hi) {
    if(!lo || hi<=lo || (lo&4095) || (hi&4095) || hi>(1ULL<<49) || (lo>>38)!=((hi-1)>>38) ||
       ((hi-1)>>29)-(lo>>29)>=kExternalTreeMaxSlots615)return 0;
    const uint64_t n=((hi-1)>>21)-(lo>>21)+1;
    return n<=kExternalTreeMaxTables615 ? uint32_t(n) : 0;
}
// bytes of the tree for [lo, hi): PD3, PD2, PD1, the PD0s, the tables
inline uint64_t externalTreeBytes615(uint64_t lo,uint64_t hi) {
    const uint32_t tables=externalTreeTables615(lo,hi);
    return tables ? uint64_t(3+((hi-1)>>29)-(lo>>29)+1+tables)*4096 : 0;
}
inline bool externalTreePlan615(uint64_t tree,uint64_t lo,uint64_t hi,ExternalTree615 *out) {
    if(!out)return false;
    *out={};
    const uint32_t tables=externalTreeTables615(lo,hi);
    if(!tables)return false;
    const uint32_t slots=uint32_t(((hi-1)>>29)-(lo>>29)+1);
    const uint64_t bytes=uint64_t(3+slots+tables)*4096;
    if(!tree || (tree&4095) || tree>=(1ULL<<37) || bytes>(1ULL<<37)-tree)return false;
    const uint64_t pd2=tree+4096,pd1=tree+8192,pd0=tree+12288,pt=pd0+uint64_t(slots)*4096;
    out->tree=tree;out->lo=lo;out->hi=hi;out->treeBytes=bytes;out->tables=tables;out->slots=slots;out->tableBase=pt;
    out->firstChunk=uint32_t(lo>>21);
    out->upperAddress[0]=tree+((lo>>47)&3)*8;  out->upperValue[0]=((pd2>>12)<<8)|2;
    out->upperAddress[1]=pd2+((lo>>38)&511)*8; out->upperValue[1]=((pd1>>12)<<8)|2;
    for(uint32_t j=0;j<slots;++j) {
        out->upperAddress[2+j]=pd1+(((lo>>29)+j)&511)*8;
        out->upperValue[2+j]=(((pd0+uint64_t(j)*4096)>>12)<<8)|2;
    }
    out->upperCount=2+slots;
    for(uint32_t i=0;i<tables;++i) {
        const uint64_t chunk=(lo>>21)+i,page=(chunk>>8)-(lo>>29);
        out->pd0Address[i]=pd0+page*4096+(chunk&255)*16+8; // small-page half of dual PDE
        out->pd0Value[i]=(((pt+uint64_t(i)*4096)>>12)<<8)|2;
    }
    return true;
}
inline bool externalTreeLeaf615(const ExternalTree615 &t,uint64_t va,uint64_t *leaf) {
    if(!leaf || !t.tables || va<t.lo || va>=t.hi || (va&4095))return false;
    const uint64_t chunk=(va>>21)-t.firstChunk;
    if(chunk>=t.tables)return false;
    *leaf=t.tableBase+chunk*4096+((va>>12)&511)*8;
    return true;
}
}
