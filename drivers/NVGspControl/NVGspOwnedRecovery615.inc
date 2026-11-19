// Fixed recovery experiment for this driver's own RO or cross-VAS COPY denial. Every
// failed/uncertain step leaves the remaining ledger pinned, with no second try.
IOReturn NVGspControl::recoverOwnedFault615() {
    auto *s=native615_;if(!s)return kIOReturnNotReady;
    auto &r=s->recovery;auto &p=s->isolation;
    constexpr UInt32 C=nvgsp::kLifecycleClient615,D=nvgsp::kLifecycleDevice615;
    auto valid=[&]() {
        return native615Busy_ && !native615Stopping_ && !sleeping_ && !resetBusy_ &&
            s->generation==resetGeneration() && !userRpcLatePending_ && postInitPhase_==27 &&
            mmuFaultEvents_==r.noticeCount && rcEvents_==r.rcCount && gmcMmuFaultPending_;
    };
    auto step=[&](UInt32 stage) {
        lk(__LINE__);r.stage=stage;setProperty("NVGspControl-recovery615-stage",stage,32);ulk();
    };
    auto ctl=[&](UInt32 object,UInt32 command,void *params,UInt32 size) {
        lk(__LINE__);const bool ok=valid();ulk();if(!ok)return false;
        if(!nvgsp::buildControl615(s->request,sizeof(s->request),C,object,command,size))return false;
        __builtin_memcpy(s->request+40,params,size);
        UInt32 bytes=sizeof(s->reply),result=~0U;
        const IOReturn transport=userRpcImpl(76,s->request,40+size,s->reply,&bytes,&result,true,true);
        lk(__LINE__);
        setProperty("NVGspControl-recovery615-last-command",command,32);
        setProperty("NVGspControl-recovery615-last-transport",UInt32(transport),32);
        setProperty("NVGspControl-recovery615-last-result",result,32);
        if(bytes)setProperty("NVGspControl-recovery615-last-reply",s->reply,bytes);
        const bool epoch=valid();ulk();
        nvgsp::ControlReply615 reply{};
        if(!epoch || transport!=kIOReturnSuccess || result || bytes!=96+size ||
           !nvgsp::decodeControlReply615(s->reply,bytes,C,object,command,size,reply) ||
           !reply.successful())return false;
        __builtin_memcpy(params,reply.params,size);return true;
    };
    auto release=[&](UInt32 object) {
        lk(__LINE__);bool owned=false;for(UInt32 i=0;i<p.objectCount;++i)owned|=p.objects[i]==object;
        const bool ok=valid() && r.preempted && r.consumed && owned;ulk();if(!ok)return false;
        const nvgsp::FreeParams615 request{C,0,object,0};
        UInt32 bytes=sizeof(s->reply),result=~0U;
        const IOReturn transport=userRpcImpl(10,reinterpret_cast<const UInt8 *>(&request),sizeof(request),s->reply,&bytes,&result,true,true);
        const UInt8 *payload=nullptr;UInt32 count=0;nvgsp::LifecycleReply615 reply{};
        lk(__LINE__);
        setProperty("NVGspControl-recovery615-last-free-object",object,32);
        setProperty("NVGspControl-recovery615-last-transport",UInt32(transport),32);
        setProperty("NVGspControl-recovery615-last-result",result,32);
        if(bytes)setProperty("NVGspControl-recovery615-last-reply",s->reply,bytes);
        const bool good=valid() && transport==kIOReturnSuccess && !result && bytes==72 &&
            nvgsp::lifecyclePayload615(s->reply,bytes,10,16,payload,count,reply) && count==16 &&
            nvgsp::control615U32(payload)==C && !nvgsp::control615U32(payload+4) &&
            nvgsp::control615U32(payload+8)==object && !nvgsp::control615U32(payload+12) &&
            !reply.rpcResult && !reply.privateResult;
        if(good)for(UInt32 i=0;i<p.objectCount;++i)if(p.objects[i]==object) {
            p.objects[i]=0;++p.freedCount;
            setProperty("NVGspControl-isolation615-ledger",p.objects,p.objectCount*4);
            setProperty("NVGspControl-isolation615-freed-count",p.freedCount,32);break;
        }
        ulk();return good;
    };
    step(1);
    nvgsp::ShadowFaultQueue615 queue{};
    lk(__LINE__);
    bool captured=valid() && nativeFaultQueue_.readDeviceRange(0,&queue,sizeof(queue)) &&
        nvgsp::ownedFaultQueue615(queue,s->faultInfo.nonReplayableBytes/32,&r.count);
    if(captured)for(UInt32 i=0;i<r.count;++i)
        if(!nativeFaultData_.readDeviceRange(((queue.get+i)%queue.capacity)*32,r.raw+i*32,32))captured=false;
    const bool crossVas=p.mode==5;
    captured=captured && nvgsp::ownedFaultBatch615(r.raw,r.count,p.channels[0].instance,
        (crossVas?p.channels[1].va:p.channels[0].va)+0x1000,
        crossVas?nvgsp::kFaultPde615:nvgsp::kFaultReadOnlyViolation615);
    if(captured) {
        r.oldGet=queue.get;r.oldPut=queue.put;
        setProperty("NVGspControl-recovery615-raw-records",r.raw,r.count*32);
        setProperty("NVGspControl-recovery615-queue-before",&queue,sizeof(queue));
        setProperty("NVGspControl-recovery615-owned-records",r.count,32);
        setProperty("NVGspControl-recovery615-notice-count",r.noticeCount,32);
    }
    ulk();if(!captured)return kIOReturnBadMessageID;
    step(2);
    auto &disable=r.disable;disable.disable=1;disable.count=2;disable.rewind=1;
    for(unsigned i=0;i<2;++i) {disable.clients[i]=C;disable.channels[i]=p.channels[i].channel;}
    lk(__LINE__);setProperty("NVGspControl-recovery615-disable-request",&disable,sizeof(disable));ulk();
    if(!ctl(nvgsp::kLifecycleSubdevice615,nvgsp::kDisableChannelsCommand615,&disable,sizeof(disable)))return kIOReturnNotReady;
    // These parameters are input-only. Echo mutations are not accepted as a
    // preemption guarantee; preserve the full returned control packet as proof.
    if(disable.disable!=1 || disable.count!=2 || disable.onlyScheduling || disable.rewind!=1 || disable.event ||
       disable.clients[0]!=C || disable.clients[1]!=C || disable.channels[0]!=p.channels[0].channel ||
       disable.channels[1]!=p.channels[1].channel)return kIOReturnBadMessageID;
    lk(__LINE__);r.preempted=true;
    p.channels[0].scheduled=p.channels[1].scheduled=false;
    setProperty("NVGspControl-recovery615-preempt-reply",s->reply,96+sizeof(disable));
    setProperty("NVGspControl-recovery615-preempted",true);ulk();
    step(3);
    // Re-read producer and consumer after preemption. No unknown new record is
    // acknowledged. Publish only GET, never a stale whole-queue snapshot.
    nvgsp::ShadowFaultQueue615 after{};
    lk(__LINE__);
    bool consumed=valid() && nativeFaultQueue_.readDeviceRange(0,&after,sizeof(after)) &&
        after.get==r.oldGet && after.put==r.oldPut && after.capacity==queue.capacity &&
        after.msgSize==32 && !after.allocator && !after.data;
    setProperty("NVGspControl-recovery615-pre-get-valid",consumed);
    setProperty("NVGspControl-recovery615-pre-get-queue",&after,sizeof(after));
    if(consumed) {
        consumed=nativeFaultQueue_.writeDeviceRange(offsetof(nvgsp::ShadowFaultQueue615,get),&r.oldPut,8);
        setProperty("NVGspControl-recovery615-get-write-complete",consumed);
    }
    if(consumed)consumed=nativeFaultQueue_.readDeviceRange(0,&after,sizeof(after)) &&
        after.get==r.oldPut && after.put==r.oldPut && after.capacity==queue.capacity &&
        after.msgSize==32 && !after.allocator && !after.data;
    setProperty("NVGspControl-recovery615-post-get-queue",&after,sizeof(after));
    if(consumed) {
        r.consumed=true;
        setProperty("NVGspControl-recovery615-queue-after",&after,sizeof(after));
        setProperty("NVGspControl-native615-fault-records-consumed",r.count,32);
    }
    ulk();if(!consumed)return kIOReturnIOError;
    step(4);
    // Restricted PRAMIN exception: only the two validated owned leaf ranges,
    // only after synchronous preemption. The normal MMU-fault guard stays on.
    auto leaf=[&](nvgsp::IsolationChannel615 &c,bool clear) {
        lk(__LINE__);bool ok=valid() && r.preempted && r.consumed && c.mapped &&
            c.pageTree && c.pteAddress>=c.pageTree && c.pteAddress-c.pageTree<=nvgsp::kExternalTreeBytes615-24;
        UInt32 words[6]{};
        if(ok)ok=nvgsp::readPraminDwords615(c.pteAddress,words,6,[&](uint64_t at,uint64_t *pair) {
            PraminPteResult a{};return praminPteAccess(pci_,at,pair,false,false,&a);
        });
        if(ok)for(unsigned j=0;j<3;++j)if(words[j*2]!=UInt32(c.pteValue[j]) || words[j*2+1]!=UInt32(c.pteValue[j]>>32))ok=false;
        if(ok && clear) {
            bzero(words,sizeof(words));ok=praminWriteWords(pci_,c.pteAddress,words,6);
            if(ok)ok=nvgsp::readPraminDwords615(c.pteAddress,words,6,[&](uint64_t at,uint64_t *pair) {
                PraminPteResult a{};return praminPteAccess(pci_,at,pair,false,false,&a);
            });
            if(ok)for(unsigned j=0;j<6;++j)if(words[j])ok=false;
        }
        ulk();return ok;
    };
    for(unsigned i=0;i<2;++i) {
        auto &c=p.channels[i];const UInt32 h=0xc0d00100+i*0x100;
        if(!release(h|0x58) || !release(c.channel) || !leaf(c,true))return kIOReturnNotReady;
        UInt32 inv[2]={c.vas,0};if(!ctl(D,0x80180c,inv,sizeof(inv)))return kIOReturnNotReady;
        c.mapped=false;
        if(!release(h|0x55) || !release(h|0x54) || !release(h|0x53) || !release(h|0x52) ||
           !release(c.share) || !release(c.tsg) || !release(c.vas) || !release(h|0x57))return kIOReturnNotReady;
        // External VAS borrows the PDB tree. Free VAS before its physical tree.
        step(5+i);
    }
    for(unsigned i=0;i<5;++i) {IOSleep(100);if(pollStatus()!=kIOReturnSuccess)return kIOReturnIOError;}
    lk(__LINE__);
    UInt64 fence=0,counters=0,pte=0;PraminPteResult access{};
    auto *bad=OSDynamicCast(OSNumber,getProperty("NVGspControl-status-bad-reason"));
    const bool verified=valid() && !userRpcActive_ && !userRpcOutstanding_ && (!bad || !bad->unsigned32BitValue()) &&
        p.objectCount==20 && p.freedCount==20 && snapshotNativeFault615() &&
        praminPteAccess(pci_,s->gpfifo+0xe00,&fence,false,false,&access) && UInt32(fence)==s->fenceExpected &&
        praminPteAccess(pci_,s->userd+0x88,&counters,false,false,&access) && UInt32(counters)==s->put && UInt32(counters>>32)==s->put &&
        praminPteAccess(pci_,s->firstGrPteAddress,&pte,false,false,&access) && pte==s->firstGrPteValue;
    if(verified) {
        // Software admission follows physical cancellation/teardown, never the
        // fault notice alone. Fresh hardware work must still prove recovery.
        r.teardownVerified=true;gmcMmuFaultPending_=false;mmuNoticesUnpaired_=0;nvdecRcUnpaired_=0;otherMmuFault_=false;native615Ready_=true;native615Status_=0;
        setProperty("NVGspControl-recovery615-teardown-verified",true);
        setProperty("NVGspControl-gmc-mmu-fault-pending",false);
        setProperty("NVGspControl-gmc-mmu-fault-serviced",true);
        setProperty("NVGspControl-native615-ready",true);
        setProperty("NVGspControl-native615-status",UInt32(0),32);
        setProperty("NVGspControl-native615-fault-recovery-ready",false);
    }
    ulk();if(!verified)return kIOReturnNotReady;
    step(7);return kIOReturnSuccess;
}
