// Fixed opaque gray ramp for display CRC comparisons, isolated from desktop.
#pragma once
#include "nvdisplay_window_probe.hpp"
static constexpr uint32_t fixtureWidth=3840,fixtureHeight=2160,fixturePitch=16384;
static constexpr uint64_t fixtureBytes=uint64_t(fixturePitch)*fixtureHeight;
static inline void fixture_words(uint64_t offset, uint32_t words[1024]) {
    for(unsigned i=0;i<1024;++i){
        const unsigned x=unsigned(((offset/4)+i)%4096);
        const uint32_t gray=x<fixtureWidth ? x*255/(fixtureWidth-1) : 0;
        words[i]=x<fixtureWidth ? 0xff000000U|gray|(gray<<8)|(gray<<16) : 0;
    }
}
static inline bool fixture_upload(io_connect_t c,uint64_t address) {
    if((address&0xfff) || address>vramBytes-fixtureBytes)return false;
    uint32_t words[1024],check[1024];
    for(uint64_t offset=0;offset<fixtureBytes;offset+=4096){
        fixture_words(offset,words);
        if(!vwrite(c,address+offset,words,1024) || !vread(c,address+offset,check,1024) ||
            memcmp(words,check,sizeof(words)))return false;
    }
    printf("fixture upload/readback verified bytes=%llu opaque3840x2160 gray-ramp/pitch16384\n",
        (unsigned long long)fixtureBytes);
    return true;
}
static inline bool fixture_guard(io_connect_t c,const uint32_t objects[96],uint32_t *oldOffset) {
    uint32_t state[4],dma=0;
    const uint32_t fullVram[]={5,0,0,0x3ffffff,0};
    return !memcmp(objects,fullVram,sizeof(fullVram)) &&
        !peek(c,0x690224,4,state) && state[0]==(fixtureWidth|(fixtureHeight<<16)) &&
        !state[1] && state[2]==0xcf && state[3]==fixturePitch/64 &&
        !peek(c,0x690240,1,&dma) && dma==0xc0d0d001 &&
        !peek(c,0x690260,1,oldOffset);
}
// Offset-only update: composition/LUT/format/bounds stay in caller's guarded state.
static inline bool fixture_switch(io_connect_t c,uint64_t inst,uint32_t offset,
                                 uint32_t *put,const WindowProbeState &expected) {
    uint32_t getput[2],check[7];
    const uint32_t words[]={0x00040260,offset,0x00080370,0,0,0x00040200,1};
    if(*put>4096-sizeof(words)-4 || peek(c,0x690000,2,getput) ||
        getput[0]!=*put || getput[1]!=*put ||
        !vwrite(c,inst+0x8000+*put,words,7) || !vread(c,inst+0x8000+*put,check,7) ||
        memcmp(words,check,sizeof(words)))return false;
    *put+=sizeof(words);
    if(!window_poke_put(c,*put))return false;
    const uint64_t end=ns_now()+200000000;WindowProbeState s={};uint32_t actual=0;
    do{
        if(!window_snapshot(c,&s) || peek(c,0x690260,1,&actual))return false;
        if(s.getput[0]==*put && window_equal(s,expected) && actual==offset &&
            !s.interlock[0] && !s.interlock[1])return true;
        usleep(1000);
    }while(ns_now()<end);
    window_dump("fixture-switch-timeout",s);return false;
}
static inline bool fixture_crc_equal(const CrcSample &a,const CrcSample &b){
    return a.compositor==b.compositor && a.raster==b.raster && a.output==b.output;
}
