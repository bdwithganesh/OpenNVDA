#pragma once
#include <stddef.h>
#include <stdint.h>
namespace nvgsp {
// Read advertised32-bit words through a64-bit-aligned PRAMIN primitive.
// The +0x8c USERD GP_PUT read exposed the old alignment mismatch on hardware.
template<class Read64>
bool readPraminDwords615(uint64_t offset,uint32_t *out,uint32_t count,Read64 read64) {
    constexpr uint64_t limit=1ULL<<40;
    if (!out || !count || count>1024 || (offset&3) || offset>=limit ||
        uint64_t(count)*4>limit-offset) return false;
    for (uint32_t i=0;i<count;) {
        const uint64_t at=offset+uint64_t(i)*4;
        uint64_t pair=0;
        if (!read64(at&~7ULL,&pair)) return false;
        out[i++]=uint32_t((at&4) ? pair>>32 : pair);
        if (!(at&4) && i<count) out[i++]=uint32_t(pair>>32);
    }
    return true;
}
}
