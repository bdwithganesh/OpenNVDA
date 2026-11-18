#pragma once
// Firmware-sized CPU-RM GR backing plan for NVIDIA615.78.08.
// Buffer IDs/size indices and promotion ABI follow the pinned NVIDIA SDK;
// no old r535/r570 context sizes or offsets are used.
#include "NVGspControlAbi615.hpp"

namespace nvgsp {
struct GrBlobBuffer615 {
    uint32_t id=0,bytes=0,alignment=0;
    uint64_t offset=0,backingBytes=0;
    uint8_t initialize=0;
};
struct GrBlob615 {
    GrBlobBuffer615 buffers[12]{};
    uint32_t count=0;
    uint64_t logicalBackingBytes=0,allocationBytes=0;
    uint64_t virtualAlignment=64ULL<<20;
};
// Primary kgraphicsGetMainCtxBufferSize: with per-subcontext context headers
// MAIN is the graphics context size aligned to 4KiB plus one 4KiB header page
// per possible subcontext (VEID). subctxHeaders is that subcontext count.
constexpr uint32_t kAdaMaxSubcontexts615=64;
inline bool planGrBlob615(const void *contextInfo,size_t bytes,GrBlob615 *out,uint32_t subctxHeaders=0) {
    if(!contextInfo || bytes!=1664 || !out || subctxHeaders>64)return false;
    // kgrobjGetPromoteIds_FWCLIENT promote3d -> FIFO context-property index.
    constexpr uint32_t indices[12]={0,9,16,17,18,19,20,21,22,23,24,25};
    GrBlob615 result{};
    const auto *data=static_cast<const uint8_t *>(contextInfo);
    for(uint32_t id=0;id<12;++id) {
        const uint32_t size=control615U32(data+indices[id]*8);
        const uint32_t alignment=control615U32(data+indices[id]*8+4);
        if(!size)continue;
        if(size>64U<<20 || !alignment || (alignment&(alignment-1)) || alignment>64U<<20)return false;
        auto &b=result.buffers[result.count++];
        b.id=id;b.bytes=id==0 && subctxHeaders ? ((size+4095)&~4095U)+4096U*subctxHeaders : size;
        b.alignment=alignment>4096?alignment:4096;
        b.backingBytes=(uint64_t(b.bytes)+4095)&~4095ULL;
        b.initialize=(id<=2 || id==8 || id==9 || id==10 || id==11)?1:0;
    }
    if(result.count<3 || result.buffers[0].id!=0 || result.buffers[0].bytes==0)return false;
    // ATTRIBUTE_CB is placed first so a64MiB-aligned virtual reservation also
    // aligns this size-aligned global buffer. Promotion order stays ID order.
    uint64_t cursor=0;
    for(uint32_t pass=0;pass<2;++pass)for(uint32_t i=0;i<result.count;++i) {
        auto &b=result.buffers[i];
        if((b.id==5)!=(pass==0))continue;
        cursor=(cursor+b.alignment-1)&~uint64_t(b.alignment-1);
        b.offset=cursor;cursor+=b.backingBytes;
        if(cursor>64ULL<<20)return false;
    }
    result.logicalBackingBytes=cursor;
    result.allocationBytes=(cursor+(2ULL<<20)-1)&~((2ULL<<20)-1);
    if(!result.allocationBytes || result.allocationBytes>64ULL<<20)return false;
    *out=result;return true;
}
inline bool promoteGrBlob615(const GrBlob615 &plan,uint32_t client,uint32_t channel,
                              uint64_t physical,uint64_t va,void *out,size_t bytes,
                              bool initializeGlobals=true) {
    if(!out || bytes!=560 || !client || !channel || !plan.count || plan.count>12 ||
       !physical || !va || (physical&((2ULL<<20)-1)) || (va&((64ULL<<20)-1)) ||
       !plan.allocationBytes || plan.allocationBytes>64ULL<<20 ||
       physical>=(1ULL<<40) || plan.allocationBytes>(1ULL<<40)-physical ||
       va>=(1ULL<<49) || plan.allocationBytes>(1ULL<<49)-va)return false;
    auto *p=static_cast<uint8_t *>(out);
    for(size_t i=0;i<bytes;++i)p[i]=0;
    auto put=[&](size_t off,uint64_t value,size_t n) {
        for(size_t i=0;i<n;++i)p[off+i]=uint8_t(value>>(i*8));
    };
    put(0,1,4);put(12,client,4);put(16,channel,4);put(40,plan.count,4);
    for(uint32_t i=0;i<plan.count;++i) {
        const auto &b=plan.buffers[i];
        if(!b.bytes || !b.backingBytes || b.bytes>b.backingBytes || b.offset>plan.allocationBytes ||
           b.backingBytes>plan.allocationBytes-b.offset || b.id>=12 ||
           !b.alignment || (b.alignment&(b.alignment-1)) || b.alignment>64U<<20 ||
           (physical+b.offset)%b.alignment || (va+b.offset)%b.alignment ||
           (b.offset&4095) || (b.backingBytes&4095) || b.initialize>1 ||
           (i && b.id<=plan.buffers[i-1].id))return false;
        for(uint32_t j=0;j<i;++j) {
            const auto &other=plan.buffers[j];
            if(b.offset<other.offset+other.backingBytes && other.offset<b.offset+b.backingBytes)return false;
        }
        const size_t entry=48+i*32;
        put(entry,physical+b.offset,8);put(entry+8,va+b.offset,8);put(entry+16,b.bytes,8);
        put(entry+24,4,4);put(entry+28,b.id,2);
        put(entry+30,b.initialize&&(initializeGlobals||b.id<=2),1);
    }
    return true;
}
}
