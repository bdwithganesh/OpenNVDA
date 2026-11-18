#pragma once
// NVIDIA615.78.08 gmcapi_gsp_config.h / nvkv.h. MIT permission notice is
// included by NVGspGmcInit.hpp. This decodes real GMC configuration; it does
// not manufacture the retired1656-byte GspStaticConfigInfo structure.
#include "NVGspGmcInit.hpp"

namespace nvgsp {
struct GmcFbRegion {
    uint64_t base=0, limit=0, reserved=0;
    uint32_t performance=0, flags=0, tag=0, seen=0;
};
struct GmcStaticConfig {
    uint64_t fbBytes=0, bar1Pde=0, bar2Pde=0, vmmuSegmentBytes=0;
    uint32_t internalClient=0, internalDevice=0, internalSubdevice=0;
    uint32_t regionCount=0, seen=0;
    char gpuName[65]{};
    GmcFbRegion regions[16]{};
};
inline bool gmcConfigScalarKey(uint16_t key) {
    return key==0x10 || key==0xff0 || key==0xff1 || key==0xff2 ||
        (key>=0x1010 && key<=0x1013) || key==0x1020 || key==0x1021 ||
        key==0x1050 || (key>=0x11 && key<=0x13);
}

inline bool gmcConfigScalar(GmcStaticConfig &c, uint16_t key, uint16_t index,
                             uint64_t value, bool wide) {
    uint32_t bit=0;
    switch (key) {
        case 0x10: bit=1; break;
        case 0xff0: bit=2; break;
        case 0xff1: bit=4; break;
        case 0xff2: bit=8; break;
        case 0x1010: bit=16; break;
        case 0x1020: bit=32; break;
        case 0x1021: bit=64; break;
        case 0x1050: bit=128; break;
    }
    if (bit) {
        if (index || (c.seen & bit) || (wide != (key >= 0x1000))) return false;
        c.seen |= bit;
        switch (key) {
            case 0x10: c.regionCount=uint32_t(value); break;
            case 0xff0: c.internalClient=uint32_t(value); break;
            case 0xff1: c.internalDevice=uint32_t(value); break;
            case 0xff2: c.internalSubdevice=uint32_t(value); break;
            case 0x1010: c.fbBytes=value; break;
            case 0x1020: c.bar1Pde=value; break;
            case 0x1021: c.bar2Pde=value; break;
            case 0x1050: c.vmmuSegmentBytes=value; break;
        }
        return true;
    }
    uint32_t field=0;
    switch (key) {
        case 0x1011: field=1; break;
        case 0x1012: field=2; break;
        case 0x1013: field=4; break;
        case 0x11: field=8; break;
        case 0x12: field=16; break;
        case 0x13: field=32; break;
        default: return true; // Stable ABI permits unknown keys.
    }
    if (index >= 16 || wide != (key >= 0x1000)) return false;
    auto &r=c.regions[index];
    if (r.seen & field) return false;
    r.seen |= field;
    switch (field) {
        case 1: r.base=value; break;
        case 2: r.limit=value; break;
        case 4: r.reserved=value; break;
        case 8: r.performance=uint32_t(value); break;
        case 16: r.flags=uint32_t(value); break;
        case 32: r.tag=uint32_t(value); break;
    }
    return true;
}

inline bool parseGmcStaticConfig(const void *payload, size_t bytes,
                                 GmcStaticConfig *out) {
    if (!payload || !out || !bytes || bytes > kGmcInitMaxResponse || (bytes & 7))
        return false;
    GmcStaticConfig c{};
    const auto *data=static_cast<const uint8_t *>(payload);
    for (size_t off=0;off<bytes;) {
        uint64_t h=0; __builtin_memcpy(&h,data+off,8); off+=8;
        const uint16_t key=uint16_t(h), index=uint16_t((h>>16)&4095);
        const uint32_t opcode=uint32_t((h>>28)&15), count=uint32_t(h>>32);
        if ((opcode<=2 && key==0x2000) || (opcode>=3 && gmcConfigScalarKey(key)))
            return false;
        if (!opcode) {
            if (!gmcConfigScalar(c,key,index,count,false)) return false;
            continue;
        }
        if (opcode>5) return false;
        const size_t element=(opcode==3 ? 1 : (opcode==1 || opcode==4 ? 4 : 8));
        if (count>(bytes-off)/element) return false;
        const size_t raw=size_t(count)*element, padded=(raw+7)&~size_t(7);
        if (padded>bytes-off) return false;
        if (opcode==1 || opcode==2) {
            if (!count || uint32_t(key)+count>65536) return false;
            for (uint32_t n=0;n<count;++n) {
                uint64_t value=0; __builtin_memcpy(&value,data+off+n*element,element);
                if (!gmcConfigScalar(c,uint16_t(key+n),index,value,opcode==2)) return false;
            }
        } else if (key==0x2000) {
            if (index || opcode!=3 || !count || count>64 ||
                data[off+count-1] || (c.seen&256)) return false;
            c.seen|=256;
            __builtin_memcpy(c.gpuName,data+off,count);
        }
        off+=padded;
    }
    if (c.seen!=511 || !c.regionCount || c.regionCount>16 ||
        !c.internalClient || !c.internalDevice || !c.internalSubdevice ||
        c.internalClient==UINT32_MAX || c.internalDevice==UINT32_MAX ||
        c.internalSubdevice==UINT32_MAX || !c.fbBytes ||
        !c.bar1Pde || !c.bar2Pde || (c.bar1Pde&4095) || (c.bar2Pde&4095) ||
        c.bar1Pde>=c.fbBytes || c.bar2Pde>=c.fbBytes ||
        !c.vmmuSegmentBytes || (c.vmmuSegmentBytes&(c.vmmuSegmentBytes-1)) ||
        !c.gpuName[0]) return false;
    for (uint32_t i=0;i<16;++i) {
        const auto &r=c.regions[i];
        if (i>=c.regionCount) { if (r.seen) return false; continue; }
        // Performance is optional. The other five fields define safe usable FB.
        if ((r.seen&55)!=55 || r.base>r.limit || r.limit>=c.fbBytes ||
            r.reserved>r.limit-r.base+1 || (r.flags&~7U) || r.tag>5 ||
            (i && r.base<=c.regions[i-1].limit)) return false;
    }
    *out=c;
    return true;
}
} // namespace nvgsp
