#pragma once
#include "NVGspGmcConfig.hpp"

namespace nvgsp {
constexpr uint64_t kDiagnosticVramAlignment615 = 2ULL << 20;
constexpr uint64_t kDiagnosticVramMaxRequest615 = 64ULL << 20;
constexpr uint64_t kDiagnosticVramBudget615 = 640ULL << 20;   // 0.180.126: 16 clients x (2 + 4) MiB; 168: x (2 + 32) MiB = 544
constexpr uint32_t kDiagnosticVramSlots615 = 40;   // 0.180.125

inline bool diagnosticVramSize615(uint64_t requested,uint64_t used,uint32_t slots,
                                   uint64_t *rounded) {
    if (!rounded || !requested || (requested&4095) ||
        requested>kDiagnosticVramMaxRequest615 || slots>=kDiagnosticVramSlots615 ||
        used>kDiagnosticVramBudget615) return false;
    const uint64_t bytes=(requested+kDiagnosticVramAlignment615-1)&~(kDiagnosticVramAlignment615-1);
    if(bytes>kDiagnosticVramBudget615-used)return false;
    *rounded=bytes;return true;
}

// GMC memory regions are disjoint and validated by parseGmcStaticConfig.
// Tag0, no reservation and no protected flag identify CPU-managed free FB.
// Keep the existing allocator's low1GiB exclusion and64MiB top margin.
inline bool diagnosticVramBounds615(const GmcStaticConfig &config,uint64_t *start,uint64_t *end) {
    if(!start || !end || !config.fbBytes || !config.regionCount || config.regionCount>16)return false;
    bool found=false;uint64_t bestStart=0,bestEnd=0;
    for(uint32_t i=0;i<config.regionCount;++i) {
        const auto &r=config.regions[i];
        if((r.seen&55)!=55 || r.tag || r.reserved || (r.flags&~3U) ||
           r.base>r.limit || r.limit>=config.fbBytes || r.limit==UINT64_MAX)continue;
        uint64_t lo=r.base>(1ULL<<30)?r.base:(1ULL<<30);
        if(lo>UINT64_MAX-(kDiagnosticVramAlignment615-1))continue;
        lo=(lo+kDiagnosticVramAlignment615-1)&~(kDiagnosticVramAlignment615-1);
        uint64_t hi=(r.limit+1)&~(kDiagnosticVramAlignment615-1);
        if(hi<(64ULL<<20))continue;
        hi-=64ULL<<20;
        if(lo>=hi)continue;
        if(!found || hi-lo>bestEnd-bestStart) {bestStart=lo;bestEnd=hi;found=true;}
    }
    if(found){*start=bestStart;*end=bestEnd;}
    return found;
}
}
