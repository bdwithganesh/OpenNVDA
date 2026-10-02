// Cursor red/green/red data-fetch comparison while a constant fixture is visible.
#pragma once
#include "nvdisplay_crc_probe.hpp"
#include "nvdisplay_fixture_probe.hpp"
#include "nvdisplay_cursor_probe.hpp"

struct CursorCrcContext {
    uint64_t notifier, image;
    uint32_t dma, original[2];
    CrcSample red,green,redAgain,disabled;
    bool colorVerified=false,disabledVerified=false;
};
static inline CursorObservation cursor_crc_sample(io_connect_t c,const Snapshot &s,
                            const CursorCrcContext &ctx,CrcSample *sample,const char *tag) {
    uint32_t zeros[1024]={};
    for(unsigned i=0;i<3;++i){
        if(!vwrite(c,ctx.notifier,zeros,1024))return CursorObservation::Refused;
        CrcSample next={};bool unreferenced=false;
        const bool got=crc_capture(c,ctx.dma,ctx.notifier,s,ctx.original,&next,&unreferenced);
        if(!unreferenced)return CursorObservation::Uncertain;
        if(!got || (i && !fixture_crc_equal(*sample,next)))return CursorObservation::Refused;
        *sample=next;
    }
    printf("cursor-output %s compositor=%08x raster=%08x primary-DP-SF=%08x three-captures-stable=yes\n",
        tag,sample->compositor,sample->raster,sample->output);
    return CursorObservation::Accepted;
}
static inline CursorObservation cursor_crc_observer(io_connect_t c,const Snapshot &state,
                                                     bool visible,void *context) {
    auto &ctx=*static_cast<CursorCrcContext *>(context);
    if(!visible){
        const auto result=cursor_crc_sample(c,state,ctx,&ctx.disabled,"disabled");
        ctx.disabledVerified=result==CursorObservation::Accepted;
        return result;
    }
    auto result=cursor_crc_sample(c,state,ctx,&ctx.red,"red");
    if(result!=CursorObservation::Accepted)return result;
    // Upload the inactive second16KiB buffer; no writes to active red pixels.
    uint32_t pixels[1024],check[1024];
    for(auto &pixel:pixels)pixel=0xff00ff00;
    for(unsigned i=0;i<4;++i)
        if(!vwrite(c,ctx.image+0x4000+i*4096,pixels,1024) ||
            !vread(c,ctx.image+0x4000+i*4096,check,1024) || memcmp(pixels,check,sizeof(pixels)))
            return CursorObservation::Refused;
    uint32_t green[7];memcpy(green,state.armed,sizeof(green));green[2]=green[3]=0x40;
    Snapshot observed={};
    if(core(c,state.usage[1],green) ||
        !wait_armed(c,"cursor-green",&state,state.usage[1],green) || !snapshot(c,&observed))
        return CursorObservation::Uncertain;
    result=cursor_crc_sample(c,observed,ctx,&ctx.green,"green");
    if(result==CursorObservation::Uncertain)return result;
    const bool greenVerified=result==CursorObservation::Accepted;
    if(core(c,state.usage[1],state.armed) ||
        !wait_armed(c,"cursor-red-again",&observed,state.usage[1],state.armed) || !snapshot(c,&observed))
        return CursorObservation::Uncertain;
    result=cursor_crc_sample(c,observed,ctx,&ctx.redAgain,"red-again");
    ctx.colorVerified=greenVerified && result==CursorObservation::Accepted &&
        fixture_crc_equal(ctx.red,ctx.redAgain) && ctx.red.output!=ctx.green.output;
    printf("cursor-output red-green-red data-fetch=%s\n",ctx.colorVerified?"verified":"unverified");
    return result;
}
