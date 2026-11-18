#pragma once
#include <stdint.h>
#include "NVGspMmu615.hpp"
#include "NVGspExternalVas615.hpp"
namespace nvgsp {
// Both negative and protected-completion probes require attributable faults.
inline bool isolationFaultableVas615(unsigned mode) { return mode==2 || mode==4 || mode==5; }
// A fixed cross-VAS negative probe uses separate addresses so the A channel
// cannot resolve B's address through its own otherwise identical mapping.
inline uint64_t isolationProbeVa615(unsigned mode,unsigned client) {
    return mode==5 && client==1 ? 2ULL<<32 : 1ULL<<32;
}
// Primary alloc_channel.h bit7 forces a nonprivileged CE channel when set.
// The fixed admin-owned mode4 tests client-derived privilege with both bit7
// and bit22 cleared. GPU commands and addresses remain kernel-fixed; actual
// RAMFC authorization must be read back before any submission.
inline uint32_t isolationChannelFlags615(unsigned mode) {
    return mode==4 ? 0x00000040U : 0x004000c0U;
}
// Pinned GSP kernel_channel.c uses internalFlags[1:0] as USER0/ADMIN1/KERNEL2.
// Preserve existing 0x14 fields; only fixed admin mode4 requests ADMIN.
inline uint32_t isolationInternalFlags615(unsigned mode) {
    return mode==4 ? 0x15U : 0x14U;
}
inline bool isolationPte615(uint64_t physical,unsigned page,bool writeDenial,uint64_t *pte) {
    if(page>2)return false;
    return vidPte615(physical,false,page==0 || (writeDenial && page==1),pte);
}
// Fixed experiment: ring read-only, data user-accessible, completion privileged.
inline bool privilegedCompletionPte615(uint64_t physical,unsigned page,uint64_t *pte) {
    return page<=2 && vidPte615(physical,page==2,page==0,pte);
}
// Two fixed GPFIFO entries: USER COPY, then KERNEL HOST semaphore release.
// The backing pushbuffer and ring are GPU read-only and never caller supplied.
inline bool privilegedCompletionEntries615(uint64_t page,uint32_t *entry) {
    if(!entry || !page || (page&4095) || page>0xffffffd000ULL)return false;
    const uint64_t copy=page+0x800,host=copy+13*4;
    entry[0]=uint32_t(copy)&0xfffffffc;entry[1]=(uint32_t(copy>>32)&0xff)|(13U<<10);
    entry[2]=uint32_t(host)&0xfffffffc;entry[3]=(uint32_t(host>>32)&0xff)|(1U<<8)|(6U<<10);
    return true;
}
// Fixed COPY0 bring-up packet, using the same C7B5 virtual/pitch transfer
// already executed by the archived615 diagnostic. No caller methods/addresses.
inline uint32_t isolationCanary615(unsigned client,unsigned word) {
    return (client ? 0xb52a970dU : 0x1673c8a1U) ^ (word*0x9e3779b9U);
}
inline bool isolationCopy615(uint64_t page,uint32_t fence,uint32_t *p) {
    if(!p || !page || page&4095 || page>0xfffffff000ULL || !fence)return false;
    const uint64_t src=page+0xc00,dst=page+0xd00,sem=page+0xe00;
    const uint32_t words[19]={0x20018000,0xc7b5,
        0x20088100,uint32_t(src>>32),uint32_t(src),uint32_t(dst>>32),uint32_t(dst),
        256,256,256,1,0x200180c0,0x186,
        0x20050017,uint32_t(sem)&0xfffffffc,uint32_t(sem>>32)&0xff,fence,0,0x00100001};
    // 13 COPY words +6 HOST words.
    __builtin_memcpy(p,words,19*4);return true;
}
// Ring, user data and completion are separate4KiB pages. The ring is
// GPU read-only; completion remains unprivileged for USER HOST access.
inline bool protectedIsolationCopy615(uint64_t page,uint32_t fence,uint32_t *p) {
    if(!p || !page || page&4095 || page>0xffffffd000ULL || !fence)return false;
    if(!isolationCopy615(page,fence,p))return false;
    const uint64_t src=page+0x1c00,dst=page+0x1d00,sem=page+0x2e00;
    p[3]=uint32_t(src>>32);p[4]=uint32_t(src);p[5]=uint32_t(dst>>32);p[6]=uint32_t(dst);
    p[14]=uint32_t(sem)&0xfffffffc;p[15]=uint32_t(sem>>32)&0xff;return true;
}
// Only the fixed A channel runs this packet. Its source is A's mapped page;
// the destination is B's GPU VA, which must be absent from A's page tree.
inline bool crossVasDenialCopy615(uint64_t aPage,uint64_t bPage,uint32_t fence,uint32_t *p) {
    if(!p || aPage==bPage || !protectedIsolationCopy615(aPage,fence,p) ||
       !bPage || (bPage&4095) || bPage>0xffffffd000ULL)return false;
    const uint64_t target=bPage+0x1d00;
    p[5]=uint32_t(target>>32);p[6]=uint32_t(target);
    return true;
}
struct IsolationChannel615 {
    uint64_t va,vaBase,vaBytes,instance,method,userd,gpfifo,methodBytes,pteAddress,pteValue[3],pageTree;
    uint32_t vas,tsg,share,channel,cid,token,fence,internalFlags,flags,ramfcConfig;
    uint32_t output[64];
    bool scheduled,mapped;
};
struct IsolationState615 {
    IsolationChannel615 channels[2];
    uint32_t objects[24],objectCount,freedCount,stage,status,mode;
    bool attempted,passed;
};
}
