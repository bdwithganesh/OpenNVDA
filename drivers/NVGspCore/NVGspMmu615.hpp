#pragma once
#include <stdint.h>
namespace nvgsp {
// Pinned NVIDIA615.78.08 kernel-open/nvidia-uvm/hwref/turing/tu102/dev_mmu.h:
// commit1a82fbfda8f321b056eb3568850f9316c5e465c5, header SHA256
// 302927d7f24ad9318507f07f871ad3fbd2050fe4b5ac4813ad4792cd23b4878d.
// VER2 VALID0, VID aperture2:1=0, PRIVILEGE5, READ_ONLY6,
// ADDRESS_VID32:8 carries physical bits36:12. Bits35:33 are peer selector.
// Current AD103 CPU/GSP VRAM allocations are additionally bounded by GMC.
// 0.180.131: KIND63:56 as sysPte615 (non-compressible kinds only)
inline bool vidPte615(uint64_t physical,bool privileged,bool readOnly,uint64_t *out,uint32_t kind=0) {
    if(!out || !physical || (physical&4095) || physical>=(1ULL<<37) || kind>7)return false;
    *out=((physical>>12)<<8)|1|(privileged?1ULL<<5:0)|(readOnly?1ULL<<6:0)|(uint64_t(kind)<<56);
    return true;
}
// Same header: APERTURE2:1 SYSTEM_COHERENT_MEMORY(2), VOL3 TRUE (not cached
// in L2, so CPU reads see GPU stores), ADDRESS_SYS53:8 = bus address bits.
// 0.180.111: KIND63:56. Only the non-compressible kinds 0x00..0x07 (pitch,
// Z16, S8, S8Z24, ZF32_X24S8, Z24S8, GENERIC_MEMORY, ZF32): no comptags.
// 0.180.177: cached = VOL clear, the GPU L2 may keep the lines. Only for
// clients whose submissions carry the host access barrier (sysCached).
inline bool sysPte615(uint64_t bus,bool readOnly,uint64_t *out,uint32_t kind=0,bool cached=false) {
    if(!out || !bus || (bus&4095) || bus>=(1ULL<<54) || kind>7)return false;
    *out=((bus>>12)<<8)|1|(2ULL<<1)|(cached?0:1ULL<<3)|(readOnly?1ULL<<6:0)|(uint64_t(kind)<<56);
    return true;
}
}
