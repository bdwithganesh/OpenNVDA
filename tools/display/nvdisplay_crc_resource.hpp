// Fixed CRC backing and optional fixture lifetime. Keep parent LUT resources separate.
#pragma once
#include "nvdisplay_crc_probe.hpp"
#include "nvdisplay_fixture_probe.hpp"
#include "nvdisplay_cursor_crc_probe.hpp"
#include "nvdisplay_cursor_pio_probe.hpp"

static constexpr uint32_t kCrcBacking=0xc0d000f0,kCrcDma=0xc0d000f1;
static constexpr unsigned kCrcClientSlot=251,kCrcLegacySlot=255;
static inline int crc_resource_trial(io_connect_t c, io_service_t service, bool fixture=false, bool normal=false,
                                    CrcSample *capturedSample=nullptr,uint64_t cursorOffset=0,bool *cursorAccepted=nullptr) {
    Snapshot before = {}; uint64_t phase=0,inst=0;
    uint32_t original[2],assy[2],hash[2048],objects[96],coreFlags[2],oldPoint=0;
    WindowProbeState windowBefore={};uint32_t originalPb[1024],windowPut=0,oldOffset=0;
    if (!snapshot(c,&before) || !armed_matches(&before,before.usage[1],before.armed) ||
        before.pending || before.error ||
        !property_u64(service,CFSTR("NVGspControl-post-init-phase"),&phase) || phase!=33 ||
        !property_u64(service,CFSTR("NVGspControl-dispinst-offset"),&inst) ||
        !inst || inst>vramBytes-0x10000 || peek(c,0x682180,2,assy) ||
        peek(c,0x68a180,2,original) || memcmp(assy,original,sizeof(assy)) ||
        original[0] || original[1] || !vread(c,inst,hash,2048) ||
        !vread(c,inst+0x2000,objects,96) || peek(c,0x680218,2,coreFlags)) return 1;
    if(cursorAccepted)*cursorAccepted=false;
    if(cursorOffset && (!fixture || !normal || !cursorAccepted || (cursorOffset&0xfff) ||
        cursorOffset>vramBytes-0x8000 || before.armed[0] || before.armed[1] ||
        (before.armed[5]&0x80000000) || (before.usage[1]&7)))return 1;
    if(cursorOffset){
        uint32_t freeCount=0;
        if(peek(c,0x6d8208,1,&oldPoint) || peek(c,0x6d8008,1,&freeCount) ||
            (oldPoint&0xffff)>=fixtureWidth || (oldPoint>>16)>=fixtureHeight || !(freeCount&0x3f)){
            fprintf(stderr,"cursor CRC guard requires current on-screen point and available PIO\n");return 1;
        }
        printf("cursor-output original-point=%08x free=%x\n",oldPoint,freeCount);
    }
    if (hash[kCrcClientSlot*2] || hash[kCrcClientSlot*2+1] || hash[kCrcLegacySlot*2] || hash[kCrcLegacySlot*2+1]) return 1;
    for(unsigned i=64;i<72;++i)if(objects[i])return 1;
    if(fixture && (!window_guard(c,service,inst,&windowBefore,originalPb,normal) ||
        !fixture_guard(c,objects,&oldOffset)))return 1;
    windowPut=windowBefore.getput[0];
    dump("CRC-before",&before);
    uint8_t memory[128]={},reply[4096];size_t bytes=0;
    const uint32_t type=6,attr=0x10800000;const uint64_t size=4096+(fixture?fixtureBytes:0);
    memcpy(memory,&client,4);memcpy(memory+4,&type,4);memcpy(memory+24,&attr,4);memcpy(memory+64,&size,8);
    const auto status=alloc(c,kCrcBacking,0x40,memory,sizeof(memory),reply,&bytes);
    if(status!=nvgsp::AdminRpcResult::Success){
        if(status==nvgsp::AdminRpcResult::Uncertain)fprintf(stderr,"CRC allocation uncertain: reboot required\n");
        return 1;
    }
    uint64_t offset=0,allocated=0;uint32_t actualAttr=0;
    if(bytes>=244){memcpy(&offset,reply+196,8);memcpy(&allocated,reply+180,8);memcpy(&actualAttr,reply+140,4);}
    if(bytes<244 || !offset || offset>vramBytes-size || (offset&0xfff) || allocated<size ||
        (actualAttr&0x19800000)!=attr){fprintf(stderr,"CRC memory reply unverified: retained, reboot required\n");return 1;}
    uint32_t zeros[1024]={},check[1024];
    if(!vwrite(c,offset,zeros,1024) || !vread(c,offset,check,1024) || memcmp(zeros,check,sizeof(zeros))){
        release(c,kCrcBacking);return 1;
    }
    if(fixture && !fixture_upload(c,offset+4096)){release(c,kCrcBacking);return 1;}
    // Matching v03 descriptor: VRAM target1 + read/write bit2 =5, pitch-kind.
    const uint32_t descriptor[]={5,uint32_t(offset>>8),uint32_t(offset>>40),
        uint32_t((offset+4095)>>8),uint32_t((offset+4095)>>40)};
    const uint32_t entry[]={kCrcDma,0x00420001}; // client1/chid0/object2100
    if(!vwrite(c,inst+0x2100,descriptor,5) || !vwrite(c,inst+kCrcClientSlot*8,entry,2) ||
        !vwrite(c,inst+kCrcLegacySlot*8,entry,2)){
        fprintf(stderr,"CRC descriptor write uncertain: retained, reboot required\n");return 1;
    }
    uint32_t afterHash[2048],afterObjects[96];
    bool layout=vread(c,inst,afterHash,2048) && vread(c,inst+0x2000,afterObjects,96) &&
        !memcmp(afterHash+kCrcClientSlot*2,entry,sizeof(entry)) &&
        !memcmp(afterHash+kCrcLegacySlot*2,entry,sizeof(entry)) &&
        !memcmp(afterObjects+64,descriptor,sizeof(descriptor));
    for(unsigned i=0;layout && i<2048;++i)
        if(i/2!=kCrcClientSlot && i/2!=kCrcLegacySlot && afterHash[i]!=hash[i])layout=false;
    for(unsigned i=0;layout && i<96;++i)
        if((i<64 || i>=69) && afterObjects[i]!=objects[i])layout=false;
    if(!layout){fprintf(stderr,"CRC layout unverified: retained, reboot required\n");return 1;}
    if(fixture && !fixture_switch(c,inst,uint32_t((offset+4096)>>8),&windowPut,windowBefore)){
        fprintf(stderr,"fixture presentation unverified: retained, reboot required\n");return 1;
    }
    CrcSample sample={};bool unreferenced=false;
    bool captured=crc_capture(c,kCrcDma,offset,before,original,&sample,&unreferenced);
    if(!unreferenced){fprintf(stderr,"CRC stop unverified: retained, reboot required\n");return 1;}
    if(fixture){
        bool stable=captured;
        for(unsigned i=0;stable && i<2;++i){
            CrcSample next={};
            if(!vwrite(c,offset,zeros,1024)){stable=false;break;}
            const bool got=crc_capture(c,kCrcDma,offset,before,original,&next,&unreferenced);
            if(!unreferenced){fprintf(stderr,"fixture CRC stop unverified: retained, reboot required\n");return 1;}
            stable=got && fixture_crc_equal(sample,next);
        }
        printf("fixture three-captures stable=%s hardware-ARM=unpublished\n",stable?"yes":"no");
        captured=stable;
        if(cursorOffset && stable){
            CursorCrcContext ctx={};ctx.notifier=offset;ctx.image=cursorOffset;ctx.dma=kCrcDma;
            memcpy(ctx.original,original,sizeof(original));
            if(!cursor_pio_point(c,0x00400040)){
                fprintf(stderr,"cursor PIO initialization uncertain: retained, reboot required\n");return 1;
            }
            if(!cursor_compare(c,inst,cursorOffset,before,cursorAccepted,cursor_crc_observer,&ctx)){
                fprintf(stderr,"cursor CRC cleanup uncertain: retained, reboot required\n");return 1;
            }
            captured=*cursorAccepted && ctx.colorVerified && ctx.disabledVerified &&
                ctx.red.output!=sample.output && fixture_crc_equal(sample,ctx.disabled);
            printf("cursor-output bitmap-fetch=%s enabled/disabled-DP-effect=%s disabled-baseline-return=%s\n",
                ctx.colorVerified?"verified":"unverified",captured?"verified":"unverified",
                ctx.disabledVerified && fixture_crc_equal(sample,ctx.disabled)?"yes":"no");
            if(!cursor_pio_point(c,oldPoint)){
                fprintf(stderr,"cursor PIO point restore uncertain: retained, reboot required\n");return 1;
            }
            // cursor_compare has verified NULL/disabled at retained capacity.
            // Restore its owned descriptor/hash before full CRC-object check.
            if(!vwrite(c,inst+0x20a0,objects+40,5) || !vwrite(c,inst+56*8,hash+112,2) ||
                !vwrite(c,inst+60*8,hash+120,2)){
                fprintf(stderr,"cursor CRC object restore uncertain: retained, reboot required\n");return 1;
            }
        }
        if(!fixture_switch(c,inst,oldOffset,&windowPut,windowBefore) ||
            !window_rewind(c,inst,windowPut,windowBefore,originalPb)){
            fprintf(stderr,"fixture/window/PB restoration unverified: retained, reboot required\n");return 1;
        }
    }
    uint32_t flagsNow[2];
    if(peek(c,0x680218,2,flagsNow))return 1;
    if(memcmp(flagsNow,coreFlags,sizeof(coreFlags))){
        const uint32_t flagsPacket[]={0x00080218,coreFlags[0],coreFlags[1]};
        if(IOConnectCallStructMethod(c,12,flagsPacket,sizeof(flagsPacket),nullptr,nullptr) ||
            peek(c,0x680218,2,flagsNow) || memcmp(flagsNow,coreFlags,sizeof(coreFlags))){
            fprintf(stderr,"CRC core interlock ASSY restore unverified: retained, reboot required\n");return 1;
        }
    }
    if(!vwrite(c,inst+kCrcClientSlot*8,hash+kCrcClientSlot*2,2) ||
        !vwrite(c,inst+kCrcLegacySlot*8,hash+kCrcLegacySlot*2,2) ||
        !vwrite(c,inst+0x2100,objects+64,5) || !vread(c,inst,afterHash,2048) ||
        !vread(c,inst+0x2000,afterObjects,96) || memcmp(hash,afterHash,sizeof(hash)) ||
        memcmp(objects,afterObjects,sizeof(objects))){
        fprintf(stderr,"CRC restoration unverified: retained, reboot required\n");return 1;
    }
    const bool freed=release(c,kCrcBacking)==nvgsp::AdminRpcResult::Success;
    printf("CRC result capture=%s exact-restore=yes backing-free=%s cursor/gamma-output-effect=unverified\n",
        captured?"yes":"no",freed?"yes":"no");
    if(captured && freed && capturedSample)*capturedSample=sample;
    return captured && freed?0:1;
}
