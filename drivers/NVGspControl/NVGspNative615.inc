#include "NVGspIsolation615.inc"
#include "NVGspFault615.inc"
#include "NVGspOwnedRecovery615.inc"
#include "NVGspClientGr615.inc"
// Native615 bring-up. Included by NVGspControl.cpp so it shares the same
// compiled transport and PRAMIN lock. The retired phase33 backend stays gated.
IOReturn NVGspControl::nativeRuntime615(UInt32 operation,UInt64 *stage,
                                      UInt64 *status,UInt64 *ready) {
    if(!lock_ || !stage || !status || !ready || operation>1)return kIOReturnBadArgument;
    lk(__LINE__);
    if(!operation && !native615Stopping_ && native615_ && native615_->faultRegistered && !native615Busy_ &&
       !snapshotNativeFault615()) {
        native615Ready_=false;native615Status_=kIOReturnIOError;
        setProperty("NVGspControl-native615-ready",false);
        setProperty("NVGspControl-native615-status",native615Status_,32);
    }
    if(native615Ready_ && (native615Stopping_ || sleeping_ || resetBusy_ ||
       gmcMmuFaultPending_ || userRpcLatePending_)) {
        native615Ready_=false;setProperty("NVGspControl-native615-ready",false);
    }
    if(!operation && native615Ready_ && native615_ && !native615Busy_) {
        // A separate connection can re-read the exact owned fence/counters.
        // No caller address and no method/doorbell submission is involved.
        UInt64 fence=0,counters=0,grPte=0,ramfc=0;PraminPteResult access{};
        const bool observed=postInitPhase_==27 && native615_->generation==resetGeneration() &&
            praminPteAccess(pci_,native615_->gpfifo+0xe00,&fence,false,false,&access) &&
            praminPteAccess(pci_,native615_->userd+0x88,&counters,false,false,&access) &&
            native615_->firstGrPteAddress &&
            praminPteAccess(pci_,native615_->firstGrPteAddress,&grPte,false,false,&access) &&
            praminPteAccess(pci_,native615_->instance+0xf0,&ramfc,false,false,&access);
        setProperty("NVGspControl-native615-host-fence-observed",UInt32(fence),32);
        setProperty("NVGspControl-native615-gp-get",UInt32(counters),32);
        setProperty("NVGspControl-native615-gp-put",UInt32(counters>>32),32);
        setProperty("NVGspControl-native615-gr-pte-observed",grPte,64);
        setProperty("NVGspControl-native615-ramfc-config",UInt32(ramfc>>32),32);
        if(!observed || UInt32(fence)!=native615_->fenceExpected ||
           UInt32(counters)!=native615_->put || UInt32(counters>>32)!=native615_->put ||
           grPte!=native615_->firstGrPteValue || (UInt32(ramfc>>32)&0x100)) {
            native615Ready_=false;native615Status_=kIOReturnIOError;
            setProperty("NVGspControl-native615-ready",false);
            setProperty("NVGspControl-native615-status",native615Status_,32);
        }
    }
    *stage=native615Stage_;*status=native615Status_;*ready=native615Ready_;
    if(!operation || native615Attempted_) {
        const IOReturn r=!operation || native615Ready_ ? kIOReturnSuccess :
            native615Busy_ ? kIOReturnBusy : kIOReturnNotReady;
        ulk();return r;
    }
    if(!gmcInitReady_ || postInitPhase_!=27 || gmcMmuFaultPending_ || userRpcLatePending_ ||
       userRpcActive_ || userRpcOutstanding_ || resetBusy_ || sleeping_ || native615Stopping_ || diagnosticVramSlots_) {
        ulk();return kIOReturnNotReady;
    }
    // Claim before allocating anything. No diagnostic writes/RPCs or reset
    // can race this bootstrap, including after a partial or timed-out failure.
    native615Attempted_=native615Busy_=true;native615Stage_=1;
    setProperty("NVGspControl-native615-attempted",true);
    setProperty("NVGspControl-native615-ready",false);
    setProperty("NVGspControl-native615-stage",1,32);
    ulk();
    native615_=static_cast<NVGspNative615State *>(IOMalloc(sizeof(*native615_)));
    IOReturn result=kIOReturnNoMemory;
    if(native615_) {
        bzero(native615_,sizeof(*native615_));
        native615_->generation=resetGeneration();
        result=initializeNative615();
    }
    lk(__LINE__);
    native615Busy_=false;native615Ready_=result==kIOReturnSuccess;
    if(!native615Status_)native615Status_=static_cast<UInt32>(result);
    setProperty("NVGspControl-native615-ready",native615Ready_);
    setProperty("NVGspControl-native615-status",native615Status_,32);
    setProperty("NVGspControl-native615-lifetime","GPU epoch; cold stop required; no Metal API readiness claim");
    *stage=native615Stage_;*status=native615Status_;*ready=native615Ready_;
    ulk();return result;
}

IOReturn NVGspControl::initializeNative615() {
    auto *s=native615_;
    if(!s)return kIOReturnNoMemory;
    constexpr UInt32 C=nvgsp::kLifecycleClient615,D=nvgsp::kLifecycleDevice615;
    constexpr UInt32 S=nvgsp::kLifecycleSubdevice615,V=nvgsp::kLifecycleVaspace615;
    UInt64 heapLo=0,heapHi=0,vaLo=0,vaBytes=0;
    auto property=[&](const char *key,UInt64 &value) {
        auto *n=OSDynamicCast(OSNumber,getProperty(key));
        if(n)value=n->unsigned64BitValue();return n!=nullptr;
    };
    lk(__LINE__);
    bool ranges=property("NVGspControl-local-memory-heap-base",heapLo) &&
        property("NVGspControl-local-memory-heap-limit",heapHi) &&
        property("NVGspControl-vaspace-base",vaLo) && property("NVGspControl-vaspace-size",vaBytes);
    ulk();
    if(!ranges || !heapLo || heapHi<heapLo || !vaLo || !vaBytes || vaBytes>UINT64_MAX-vaLo)
        return kIOReturnNotReady;
    const UInt64 bootstrapStart=mach_absolute_time();
    UInt32 stageMs[16]={};
    auto step=[&](UInt32 value) {
        UInt64 ns=0;absolutetime_to_nanoseconds(mach_absolute_time()-bootstrapStart,&ns);
        if(value<16)stageMs[value]=UInt32(ns/1000000);
        lk(__LINE__);native615Stage_=value;
        setProperty("NVGspControl-native615-stage",value,32);
        setProperty("NVGspControl-native615-stage-ms",stageMs,sizeof(stageMs));ulk();   // 0.180.198: ms at each stage
    };
    auto failed=[&](UInt32 value) {
        lk(__LINE__);native615Status_=value;
        setProperty("NVGspControl-native615-status",value,32);ulk();return false;
    };
    auto healthy=[&]() {
        // 0.180.198: two 10 ms polls, as the client path since 0.180.122 (five of 100 ms were
        // 3 s of the 8 s bootstrap, i.e. of every boot's time to a picture)
        for(unsigned i=0;i<2;++i) {
            IOSleep(10);
            if(pollStatus()!=kIOReturnSuccess)return failed(kIOReturnNotReady);
            lk(__LINE__);
            const bool ok=postInitPhase_==27 && gmcInitReady_ && !gmcMmuFaultPending_ &&
                !userRpcLatePending_ && !resetBusy_ && !sleeping_ && !native615Stopping_ && s->generation==resetGeneration();
            ++s->healthPolls;
            auto *bad=OSDynamicCast(OSNumber,getProperty("NVGspControl-status-bad-reason"));
            const bool reason=bad && bad->unsigned32BitValue();
            ulk();
            if(!ok || reason)return failed(kIOReturnNotReady);
        }
        return true;
    };
    auto ctl=[&](UInt32 object,UInt32 command,void *params,UInt32 size) {
        if(!nvgsp::buildControl615(s->request,sizeof(s->request),C,object,command,size))
            return failed(kIOReturnBadArgument);
        if(size)__builtin_memcpy(s->request+40,params,size);
        UInt32 bytes=sizeof(s->reply),result=~0U;
        const IOReturn r=userRpcImpl(76,s->request,40+size,s->reply,&bytes,&result,true,true);
        if(r!=kIOReturnSuccess)return failed(static_cast<UInt32>(r));
        nvgsp::ControlReply615 reply{};
        if(bytes!=96+size || !nvgsp::decodeControlReply615(s->reply,bytes,C,object,command,size,reply))
            return failed(kIOReturnBadMessageID);
        if(result || !reply.successful())return failed(result ? result : reply.header.status ? reply.header.status : kIOReturnError);
        if(size)__builtin_memcpy(params,reply.params,size);
        return true;
    };
    auto alloc=[&](UInt32 parent,UInt32 object,UInt32 cls,void *params,UInt32 size) {
        if(s->objectCount>=16 || !nvgsp::buildAlloc615(s->request,sizeof(s->request),parent,object,cls,params,size))
            return failed(kIOReturnBadArgument);
        // Record before enqueue. Never infer ownership rollback from a lost
        // reply or a control refusal after partial global initialization.
        s->objects[s->objectCount++]=object;
        lk(__LINE__);setProperty("NVGspControl-native615-object-ledger",s->objects,s->objectCount*4);ulk();
        UInt32 bytes=sizeof(s->reply),result=~0U;
        const IOReturn r=userRpcImpl(103,s->request,32+size,s->reply,&bytes,&result,true,true);
        if(r!=kIOReturnSuccess)return failed(static_cast<UInt32>(r));
        const uint8_t *returned=nullptr;
        const auto reply=nvgsp::decodeAlloc615(s->reply,bytes,parent,object,cls,size,&returned);
        if(bytes!=88+size || !reply.valid)return failed(kIOReturnBadMessageID);
        if(result || !reply.successful())return failed(result ? result : reply.status);
        if(size)__builtin_memcpy(params,returned,size);
        return true;
    };
    auto read=[&](UInt64 at,UInt32 count) {
        const IOReturn r=vramAccess(at,s->readWords,count,false,true);
        return r==kIOReturnSuccess || failed(static_cast<UInt32>(r));
    };
    auto write=[&](UInt64 at,const UInt32 *words,UInt32 count) {
        if(!count || count>1024)return failed(kIOReturnBadArgument);
        if(words!=s->writeWords)__builtin_memcpy(s->writeWords,words,count*4);
        const IOReturn r=vramAccess(at,s->writeWords,count,true,true);
        if(r!=kIOReturnSuccess)return failed(static_cast<UInt32>(r));
        if(!read(at,count))return false;
        return !__builtin_memcmp(s->writeWords,s->readWords,count*4) || failed(kIOReturnIOError);
    };
    auto zero=[&](UInt64 base,UInt64 bytes) {
        if(!base || !bytes || ((base|bytes)&4095))return failed(kIOReturnBadArgument);
        // 0.180.198: one write a page, then its first and last 16 words read back (as the client
        // path's zero since 0.180.121). The full read-back of all 24 MiB was 4.3 s of every boot.
        bzero(s->writeWords,sizeof(s->writeWords));
        for(UInt64 page=0;page<bytes;page+=4096) {
            const IOReturn r=vramAccess(base+page,s->writeWords,1024,true,true);
            if(r!=kIOReturnSuccess)return failed(static_cast<UInt32>(r));
            const UInt64 ends[2]={base+page,base+page+4032};
            for(UInt64 at:ends) {
                if(!read(at,16))return false;
                for(unsigned i=0;i<16;++i)if(s->readWords[i])return failed(kIOReturnIOError);
            }
            s->zeroWords+=1024;
        }
        return true;
    };
    auto virtualMemory=[&](UInt32 handle,UInt64 bytes,UInt64 alignment,UInt64 &va) {
        nvgsp::MemoryParams615 p{};
        p.owner=C;p.type=6;p.flags=0x80100;p.attr=0x800000;
        p.size=bytes;p.alignment=alignment;p.vaSpace=V;
        if(!alloc(D,handle,0x50a0,&p,sizeof(p)))return false;
        va=p.offset;
        return (p.owner==C && p.type==6 && p.size==bytes && p.limit==bytes-1 &&
            va>=vaLo && va<vaLo+vaBytes && bytes<=vaLo+vaBytes-va && !(va&(alignment-1))) || failed(kIOReturnBadArgument);
    };
    auto map=[&](UInt64 va,UInt64 physical,UInt64 bytes) {
        for(UInt64 done=0;done<bytes;) {
            UInt8 query[208]{};
            const UInt64 at=va+done;
            __builtin_memcpy(query,&at,8);__builtin_memcpy(query+200,&V,4);
            if(!ctl(D,0x801809,query,sizeof(query)))return false;
            UInt64 base=0;UInt32 coverage=0,matches=0;
            for(UInt32 i=0;i<5;++i) {
                const UInt8 *p=query+32+i*32;
                if(nvgsp::control615U32(p+16)!=4096)continue;
                __builtin_memcpy(&base,p,8);coverage=nvgsp::control615U32(p+24);++matches;
                if(nvgsp::control615U32(p+12)!=8 || nvgsp::control615U32(p+20)!=0)
                    return failed(kIOReturnBadArgument);
            }
            if(matches!=1 || !coverage || (coverage&(coverage-1)) || coverage<4096)
                return failed(kIOReturnBadArgument);
            const UInt64 tableBytes=UInt64(coverage)/4096*8;
            if(base<heapLo || base>heapHi || tableBytes-1>heapHi-base)return failed(kIOReturnBadArgument);
            UInt64 count=(bytes-done)/4096;
            if(count>256)count=256;
            if(count>(coverage-at%coverage)/4096)count=(coverage-at%coverage)/4096;
            if(!count)return failed(kIOReturnBadArgument);
            const UInt64 address=base+(at%coverage)/4096*8;
            if(!read(address,static_cast<UInt32>(count*2)))return false;
            for(UInt32 i=0;i<count*2;++i)if(s->readWords[i])return failed(kIOReturnBusy);
            for(UInt32 i=0;i<count;++i) {
                const UInt64 pageVa=at+i*4096;
                const bool privileged=pageVa>=s->grVa && pageVa<s->grVa+s->gr.logicalBackingBytes;
                UInt64 value=0;
                if(!nvgsp::vidPte615(physical+done+i*4096,privileged,false,&value))return failed(kIOReturnBadArgument);
                if(privileged) {
                    ++s->privilegedGrPages;
                    if(!s->firstGrPteAddress) {s->firstGrPteAddress=address+i*8;s->firstGrPteValue=value;}
                }
                s->writeWords[i*2]=UInt32(value);s->writeWords[i*2+1]=UInt32(value>>32);
            }
            if(!write(address,s->writeWords,static_cast<UInt32>(count*2)))return false;
            UInt32 invalidate[2]={V,0};
            if(!ctl(D,0x80180c,invalidate,sizeof(invalidate)))return false;
            s->mappedPages+=static_cast<UInt32>(count);done+=count*4096;
        }
        return true;
    };
    // The exact allocator, table format, descriptors and ordering were proven
    // on this GPU by the archived native615 diagnostics; no legacy offsets.
    if(!healthy() || !ctl(S,nvgsp::kGmmuStaticInfoCommand615,&s->faultInfo,sizeof(s->faultInfo)))
        return kIOReturnNotReady;
    UInt32 faultPages=0,faultEntries=0;
    const bool faultPlan=nvgsp::nonReplayableFaultPlan615(s->faultInfo,&faultPages,&faultEntries);
    lk(__LINE__);
    setProperty("NVGspControl-native615-gmmu-static-info",&s->faultInfo,sizeof(s->faultInfo));
    setProperty("NVGspControl-native615-fault-plan-valid",faultPlan);
    setProperty("NVGspControl-native615-fault-plan-pages",faultPages,32);
    setProperty("NVGspControl-native615-fault-plan-usable-entries",faultEntries,32);
    setProperty("NVGspControl-native615-fault-buffer-registered",false);
    ulk();
    // Sizing is evidence for the next implementation step; it does not add
    // recovery readiness. The owned registration is separately validated.
    const IOReturn faultRegistration=registerNativeFault615();
    if(faultRegistration!=kIOReturnSuccess)return faultRegistration;
    if(!virtualMemory(0xc0d00057,4096,4096,s->gpfifoVa))return kIOReturnNotReady;
    step(2);
    if(!ctl(S,0x20800a32,s->context,sizeof(s->context)) ||
       !nvgsp::planGrBlob615(s->context,sizeof(s->context),&s->gr,nvgsp::kAdaMaxSubcontexts615))return kIOReturnBadArgument;
    UInt64 generation=0;
    const IOReturn reserved=diagnosticVramReserve(s->gr.allocationBytes,&s->physical,&s->bytes,&generation,true);
    if(reserved!=kIOReturnSuccess)return reserved;
    if(generation!=s->generation || s->bytes!=s->gr.allocationBytes)return kIOReturnOffline;
    if(!virtualMemory(0xc0d000a0,s->bytes,s->gr.virtualAlignment,s->grVa))return kIOReturnNotReady;
    lk(__LINE__);
    setProperty("NVGspControl-native615-gr-physical",s->physical,64);
    setProperty("NVGspControl-native615-gr-va",s->grVa,64);
    setProperty("NVGspControl-native615-gr-bytes",s->bytes,64);
    setProperty("NVGspControl-native615-gr-buffer-count",s->gr.count,32);
    ulk();step(3);
    if(!zero(s->physical,s->bytes) || !map(s->grVa,s->physical,s->bytes))return kIOReturnIOError;
    step(4);
    nvgsp::ChannelGroup615 group{};group.hVASpace=V;group.engineType=1;
    nvgsp::ContextShare615 share{V,0,~0U};
    if(!alloc(D,0xc0d00050,0xa06c,&group,sizeof(group)) ||
       !alloc(0xc0d00050,0xc0d00051,0x9067,&share,sizeof(share)) ||
       share.hVASpace!=V || share.flags || share.subctxId==~0U)return kIOReturnNotReady;
    lk(__LINE__);setProperty("NVGspControl-native615-subctx-id",share.subctxId,32);ulk();
    UInt32 methodBytes=0;
    if(!ctl(S,0x20802a08,&methodBytes,sizeof(methodBytes)) || !methodBytes || methodBytes>16*1024*1024)
        return kIOReturnBadArgument;
    s->methodBytes=(UInt64(methodBytes)+4095)&~4095ULL;
    UInt64 *backings[4]={&s->instance,&s->method,&s->userd,&s->gpfifo};
    for(UInt32 i=0;i<4;++i) {
        nvgsp::MemoryParams615 p{};
        p.owner=C;p.type=6;p.flags=0x100;p.attr=0x800000;
        p.size=i==1?s->methodBytes:4096;p.alignment=4096;
        const UInt64 size=p.size;
        if(!alloc(S,0xc0d00052+i,0x40,&p,sizeof(p)))return kIOReturnNotReady;
        if(!nvgsp::memoryProbeRange615(p,gmcConfig_.fbBytes,heapLo,heapHi) || p.size!=size)
            return kIOReturnBadArgument;
        *backings[i]=p.offset;
        if(!zero(p.offset,size))return kIOReturnIOError;
    }
    if(!map(s->gpfifoVa,s->gpfifo,4096))return kIOReturnIOError;
    step(5);
    nvgsp::ChannelAlloc615 ch{};
    // Primary615 alloc_channel.h: delay6, deny physical CE7, deny privileged
    // Host methods22. InternalFlags privilege1:0=USER is a separate field.
    ch.gpFifoOffset=s->gpfifoVa;ch.gpFifoEntries=256;ch.flags=0x004000c0;
    ch.hContextShare=0xc0d00051;ch.hUserdMemory[0]=0xc0d00054;ch.engineType=1;
    ch.instanceMem={s->instance,4096,2,1};ch.userdMem={s->userd,4096,2,1};
    ch.ramfcMem={s->instance,512,2,1};ch.mthdbufMem={s->method,s->methodBytes,2,0};
    ch.hPhysChannelGroup=0xc0d00050;ch.internalFlags=0x14;
    if(!alloc(0xc0d00050,0xc0d00056,0xc56f,&ch,sizeof(ch)))return kIOReturnNotReady;
    lk(__LINE__);setProperty("NVGspControl-native615-channel-internal-flags",ch.internalFlags,32);ulk();
    lk(__LINE__);setProperty("NVGspControl-native615-channel-allocation-flags",ch.flags,32);ulk();
    if(ch.internalFlags!=0x14)
        return kIOReturnNotReady;
    s->cid=ch.cid;
    step(6);
    UInt8 promote[560]{};
    if(!nvgsp::promoteGrBlob615(s->gr,C,0xc0d00056,s->physical,s->grVa,promote,sizeof(promote)) ||
       !ctl(S,0x2080012b,promote,sizeof(promote)) || !healthy())return kIOReturnNotReady;
    step(7);
    UInt32 grParams[4]={2,0,16,0};
    if(!alloc(0xc0d00056,0xc0d00058,0xc997,grParams,sizeof(grParams)) ||
       grParams[0]!=2 || grParams[1] || grParams[2]!=16 || !healthy())return kIOReturnNotReady;
    step(8);grParams[3]=0;
    if(!alloc(0xc0d00056,0xc0d00059,0xc9c0,grParams,sizeof(grParams)) ||
       grParams[0]!=2 || grParams[1] || grParams[2]!=16 || !healthy())return kIOReturnNotReady;
    step(9);
    UInt32 engine=1;
    if(!ctl(0xc0d00056,0xa06f0104,&engine,4) ||
       !ctl(0xc0d00056,0xc36f0108,&s->token,4))return kIOReturnNotReady;
    UInt8 schedule[3]={1,0,0};
    if(!ctl(0xc0d00056,0xa06f0103,schedule,sizeof(schedule)))return kIOReturnNotReady;
    step(10);
    constexpr UInt32 fence=0x6150c039;
    const UInt64 semaphore=s->gpfifoVa+0xe00,pb=s->gpfifoVa+0x800;
    const UInt32 words[6]={0x20050017,UInt32(semaphore)&0xfffffffc,UInt32(semaphore>>32)&0xff,fence,0,0x00100001};
    const UInt32 entry[2]={UInt32(pb)&0xfffffffc,(UInt32(pb>>32)&0xff)|(6U<<10)};
    if(!read(s->gpfifo+0xe00,1) || s->readWords[0] ||
       !read(s->userd+0x88,2) || s->readWords[0] || s->readWords[1] ||
       !write(s->gpfifo+0x800,words,6) || !write(s->gpfifo,entry,2))return kIOReturnIOError;
    const UInt32 put=1;
    if(!write(s->userd+0x8c,&put,1) || diagnosticChannelKick(0xc0d00056,true)!=kIOReturnSuccess)
        return kIOReturnIOError;
    UInt64 deadline=0;clock_interval_to_deadline(3,kSecondScale,&deadline);
    bool completed=false;
    do {
        if(!read(s->gpfifo+0xe00,1))return kIOReturnIOError;
        if(s->readWords[0]==fence) {completed=true;break;}
        IOSleep(1);
    } while(mach_absolute_time()<deadline);
    if(!completed || !read(s->userd+0x88,2) || s->readWords[0]!=1 || s->readWords[1]!=1 || !healthy())
        return kIOReturnTimeout;
    s->sequence=s->put=1;s->fenceExpected=fence;
    lk(__LINE__);
    setProperty("NVGspControl-native615-host-fence",fence,32);
    setProperty("NVGspControl-native615-gp-get",1,32);setProperty("NVGspControl-native615-gp-put",1,32);
    setProperty("NVGspControl-native615-zero-readback-words",s->zeroWords,32);
    setProperty("NVGspControl-native615-mapped-pages",s->mappedPages,32);
    setProperty("NVGspControl-native615-health-polls",s->healthPolls,32);
    setProperty("NVGspControl-native615-channel-cid",s->cid,32);
    setProperty("NVGspControl-native615-channel-privilege","USER hardware channel; admin-only bring-up ABI");
    setProperty("NVGspControl-native615-privileged-gr-pages",s->privilegedGrPages,32);
    setProperty("NVGspControl-native615-gr-pte-address",s->firstGrPteAddress,64);
    setProperty("NVGspControl-native615-gr-pte-expected",s->firstGrPteValue,64);
    ulk();step(11);
    return kIOReturnSuccess;
}

IOReturn NVGspControl::nativeCompute615(const void *nak,UInt32 bytes,
                                      nvgsp::ComputeResult615 *out) {
    UInt32 codeBytes=0,registers=0;
    if(!lock_ || !out || !nvgsp::computeNak615(nak,bytes,&codeBytes,&registers))return kIOReturnBadArgument;
    bzero(out,sizeof(*out));
    lk(__LINE__);
    auto *s=native615_;
    if(!s || !native615Ready_ || native615Busy_ || native615Stopping_ || sleeping_ ||
       resetBusy_ || gmcMmuFaultPending_ || userRpcLatePending_ || postInitPhase_!=27 ||
       s->generation!=resetGeneration()) {ulk();return kIOReturnNotReady;}
    // Ring wrap and larger dispatch shapes are outside this diagnostic ABI.
    // Refuse before touching memory rather than silently reusing an entry.
    if(s->put>=255) {ulk();return kIOReturnNoSpace;}
    const UInt64 offset=(s->gr.logicalBackingBytes+4095)&~4095ULL;
    constexpr UInt64 scratchBytes=0xb000;
    if(offset>s->bytes || scratchBytes>s->bytes-offset) {ulk();return kIOReturnNoMemory;}
    native615Busy_=true;native615Stage_=12;native615Status_=0;
    setProperty("NVGspControl-native615-stage",12,32);
    setProperty("NVGspControl-native615-busy",true);
    ulk();
    const UInt64 physical=s->physical+offset,va=s->grVa+offset;
    const UInt32 sequence=s->sequence+1,put=s->put+1;
    const UInt32 computeRelease=0xc0400000U|sequence,hostFence=0x6150c038U+sequence;
    auto read=[&](UInt64 at,UInt32 count) {
        return vramAccess(at,s->readWords,count,false,true)==kIOReturnSuccess;
    };
    auto write=[&](UInt64 at,const UInt32 *words,UInt32 count) {
        if(!count || count>1024)return false;
        if(words!=s->writeWords)__builtin_memcpy(s->writeWords,words,count*4);
        return vramAccess(at,s->writeWords,count,true,true)==kIOReturnSuccess && read(at,count) &&
            !__builtin_memcmp(s->writeWords,s->readWords,count*4);
    };
    auto healthy=[&]() {
        for(unsigned i=0;i<2;++i) {   // 0.180.198: as above
            IOSleep(10);
            if(pollStatus()!=kIOReturnSuccess)return false;
            lk(__LINE__);
            const bool ok=!gmcMmuFaultPending_ && !userRpcLatePending_ && !resetBusy_ && !sleeping_ &&
                !native615Stopping_ && postInitPhase_==27 && s->generation==resetGeneration();
            auto *bad=OSDynamicCast(OSNumber,getProperty("NVGspControl-status-bad-reason"));
            const bool reason=bad && bad->unsigned32BitValue();
            ulk();if(!ok || reason)return false;
        }
        return true;
    };
    const IOReturn result=[&]() -> IOReturn {
        // Re-establish completion before overwriting the reused pushbuffer or
        // any shader scratch. These ranges never overlap live GR backing.
        if(!read(s->gpfifo+0xe00,1) || s->readWords[0]!=s->fenceExpected ||
           !read(s->userd+0x88,2) || s->readWords[0]!=s->put || s->readWords[1]!=s->put)
            return kIOReturnNotReady;
        if(!nvgsp::computePacket615(va,va+0x8000,va+0x9000,va+0xaf00,
                                   registers,computeRelease,&s->compute))return kIOReturnBadArgument;
        for(UInt64 page=0;page<scratchBytes;page+=4096) {
            bzero(s->writeWords,sizeof(s->writeWords));
            if(!write(physical+page,s->writeWords,1024))return kIOReturnIOError;
        }
        const auto *code=static_cast<const UInt8 *>(nak)+64;
        for(UInt32 done=0;done<codeBytes;) {
            const UInt32 count=(codeBytes-done)>4096?1024:(codeBytes-done)/4;
            __builtin_memcpy(s->writeWords,code+done,count*4);
            if(!write(physical+done,s->writeWords,count))return kIOReturnIOError;
            done+=count*4;
        }
        if(!write(physical+0x8000,s->compute.qmd,64))return kIOReturnIOError;
        const UInt64 outputVa=va+0xa000;
        const UInt32 constant[8]={UInt32(outputVa),UInt32(outputVa>>32),64,1,1,64,1,1};
        if(!write(physical+0x9000,constant,8) || !read(physical+0xa000,64))return kIOReturnIOError;
        for(unsigned i=0;i<64;++i)if(s->readWords[i])return kIOReturnIOError;
        if(!read(physical+0xaf00,1) || s->readWords[0])return kIOReturnIOError;
        const UInt64 semaphore=s->gpfifoVa+0xe00,pb=s->gpfifoVa+0x800;
        const UInt32 n=s->compute.commandCount;
        if(n+6>256)return kIOReturnBadArgument;
        __builtin_memcpy(s->writeWords,s->compute.commands,n*4);
        const UInt32 host[6]={0x20050017,UInt32(semaphore)&0xfffffffc,UInt32(semaphore>>32)&0xff,hostFence,0,0x00100001};
        __builtin_memcpy(s->writeWords+n,host,sizeof(host));
        if(!write(s->gpfifo+0x800,s->writeWords,n+6))return kIOReturnIOError;
        const UInt32 entry[2]={UInt32(pb)&0xfffffffc,(UInt32(pb>>32)&0xff)|((n+6)<<10)};
        if(!write(s->gpfifo+UInt64(s->put)*8,entry,2))return kIOReturnIOError;
        // The expected values occur only in commands/QMD. The CPU never
        // writes them into the GPU output, release or HOST semaphore memory.
        if(!write(s->userd+0x8c,&put,1) || diagnosticChannelKick(0xc0d00056,true)!=kIOReturnSuccess)
            return kIOReturnIOError;
        UInt64 deadline=0;clock_interval_to_deadline(5,kSecondScale,&deadline);
        bool completed=false;
        do {
            if(!read(physical+0xaf00,1))return kIOReturnIOError;
            const bool shader=s->readWords[0]==computeRelease;
            if(!read(s->gpfifo+0xe00,1))return kIOReturnIOError;
            if(shader && s->readWords[0]==hostFence) {completed=true;break;}
            IOSleep(1);
        } while(mach_absolute_time()<deadline);
        if(!completed || !read(s->userd+0x88,2) || s->readWords[0]!=put || s->readWords[1]!=put)
            return kIOReturnTimeout;
        if(!read(physical+0xa000,64))return kIOReturnIOError;
        __builtin_memcpy(out->output,s->readWords,sizeof(out->output));
        if(!healthy())return kIOReturnNotReady;
        out->version=1;out->words=64;out->sequence=sequence;
        out->computeRelease=computeRelease;out->hostFence=hostFence;out->gpGet=out->gpPut=put;
        return kIOReturnSuccess;
    }();
    lk(__LINE__);
    if(result==kIOReturnSuccess) {
        s->sequence=sequence;s->put=put;s->fenceExpected=hostFence;
        if(bytes<=sizeof(s->provenNak)) {__builtin_memcpy(s->provenNak,nak,bytes);s->provenNakBytes=bytes;}
        native615Stage_=13;
        setProperty("NVGspControl-native615-compute-release",computeRelease,32);
        setProperty("NVGspControl-native615-compute-sequence",sequence,32);
        setProperty("NVGspControl-native615-compute-code-bytes",codeBytes,32);
        setProperty("NVGspControl-native615-compute-scratch-physical",physical,64);
        setProperty("NVGspControl-native615-compute-scratch-va",va,64);
        setProperty("NVGspControl-native615-compute-output",out->output,sizeof(out->output));
        setProperty("NVGspControl-native615-host-fence",hostFence,32);
        setProperty("NVGspControl-native615-gp-get",put,32);
        setProperty("NVGspControl-native615-gp-put",put,32);
    } else {
        native615Ready_=false;
        setProperty("NVGspControl-native615-ready",false);
    }
    native615Status_=static_cast<UInt32>(result);native615Busy_=false;
    setProperty("NVGspControl-native615-stage",native615Stage_,32);
    setProperty("NVGspControl-native615-status",native615Status_,32);
    setProperty("NVGspControl-native615-busy",false);
    ulk();return result;
}
