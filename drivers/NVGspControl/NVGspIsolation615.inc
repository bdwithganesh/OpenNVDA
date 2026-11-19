// Admin-only, fixed two-VAS COPY0 proof. A failed/uncertain transaction retains
// every remaining object and invalidates the native runtime until cold stop.
IOReturn NVGspControl::nativeIsolation615(UInt32 operation,UInt64 *stage,
                                        UInt64 *status,UInt64 *passed,int slot) {
    if(!lock_ || !stage || !status || !passed || operation>17)return kIOReturnBadArgument;
    *stage=*status=*passed=0;lk(__LINE__);
    // 0.180.112: an owned transaction waits for the running one; the admin
    // diagnostic paths stay on slot 0.
    if(slot>=0 && !waitIdle615()) {ulk();return kIOReturnBusy;}
    if(!selectClient615(slot<0?0U:UInt32(slot))) {ulk();return kIOReturnNotReady;}
    auto *s=native615_;
    if(!s) {ulk();return operation?kIOReturnNotReady:kIOReturnSuccess;}
    auto &p=s->isolation;
    if(operation==6 || (operation>=8 && operation<=13)) {
        auto &g=s->clientGrs[client615Slot_];
        if(g.attempted || !native615Ready_ || native615Busy_ || native615Stopping_ || sleeping_ ||
           resetBusy_ || gmcMmuFaultPending_ || userRpcLatePending_ || userRpcActive_ ||
           userRpcOutstanding_ || postInitPhase_!=27 || s->generation!=resetGeneration() ||
           p.attempted || s->recovery.attempted ||
           (operation==10 ? s->sequence<1 : (s->sequence<3 || !s->provenNakBytes))) {
            *stage=g.stage;*status=g.status;*passed=g.passed;ulk();
            return g.attempted && g.passed?kIOReturnSuccess:kIOReturnNotReady;
        }
        g.attempted=true;g.localGlobals=operation==8;g.sameVas=operation==9;g.touchOnly=operation==10;g.subcontext=operation==11;g.negative=operation==12;g.external=operation==12 || operation==13;native615Busy_=true;
        setProperty("NVGspControl-client615-external",g.external);
        setProperty("NVGspControl-client615-negative",g.negative);
        setProperty("NVGspControl-client615-subcontext",g.subcontext);
        setProperty("NVGspControl-client615-touch-only",g.touchOnly);
        setProperty("NVGspControl-client615-same-vas",g.sameVas);
        setProperty("NVGspControl-client615-attempted",true);
        setProperty("NVGspControl-client615-local-globals",g.localGlobals);
        setProperty("NVGspControl-native615-busy",true);ulk();
        const UInt64 t0=mach_absolute_time();
        const IOReturn r=runClientGr615(0);
        UInt64 ns=0;absolutetime_to_nanoseconds(mach_absolute_time()-t0,&ns);
        lk(__LINE__);if(!g.status)g.status=static_cast<UInt32>(r);g.passed=r==kIOReturnSuccess;g.live=g.passed;
        setProperty("NVGspControl-client615-create-ms",ns/1000000,32);   // 0.180.119
        native615Busy_=false;
        // 0.180.127: an owned client that GSP-RM or the VRAM reservation simply
        // refused (an NV_STATUS such as NO_MEMORY, or NoMemory/NoSpace) while
        // everything else is healthy costs that slot, not every other process.
        const bool refused=r!=kIOReturnSuccess && operation==13 &&
            ((g.status && g.status<0x10000) || r==kIOReturnNoMemory || r==kIOReturnNoSpace) &&
            !gmcMmuFaultPending_ && !userRpcLatePending_ && !resetBusy_ && !sleeping_ && !native615Stopping_ &&
            postInitPhase_==27 && s->generation==resetGeneration();
        if(refused)setProperty("NVGspControl-client615-refused-slot",UInt32(client615Slot_),32);
        if(r!=kIOReturnSuccess && !refused) {
            native615Ready_=false;native615Status_=static_cast<UInt32>(r);
            setProperty("NVGspControl-native615-ready",false);
            setProperty("NVGspControl-native615-status",native615Status_,32);
        }
        setProperty("NVGspControl-native615-busy",false);
        setProperty("NVGspControl-client615-passed",g.passed);
        setProperty("NVGspControl-client615-live",g.live);
        setProperty("NVGspControl-client615-status",g.status,32);
        *stage=g.stage;*status=g.status;*passed=g.passed;ulk();return r;
    }
    if(operation==15) {
        auto &g=s->clientGrs[client615Slot_];
        // A cancelled replayable fault becomes fatal: GSP robust-channel
        // recovers ONLY the client channel (Xid31 MMU fault) and posts a GMC
        // fault notice (89). Accept that pending notice only when the single
        // RC is exactly attributed to this client's denied store.
        auto number=[&](const char *key) {
            auto *n=OSDynamicCast(OSNumber,getProperty(key));return n ? n->unsigned64BitValue() : ~0ULL;
        };
        // The RC/fault notices can still sit in the GSP status queue (90):
        // drain it here (same path as a status poll) for up to ~2 s.
        UInt32 drains=0;
        while(g.attempted && g.cancelled && !native615Busy_ && postInitPhase_==27 &&
              !(rcEvents_>=1 && gmcMmuFaultPending_) && drains<40) {
            ++drains;drainNoWait_=true;pollStatusLocked();
            if(rcEvents_>=1 && gmcMmuFaultPending_)break;
            ulk();IOSleep(50);lk(__LINE__);
        }
        setProperty("NVGspControl-client615-recover-drains",drains,32);
        const UInt64 target=(g.va+s->bytes-0x10000)&~0xfffULL;
        const UInt64 faultVa=(number("NVGspControl-rc-mmu-fault-hi")<<32)|number("NVGspControl-rc-mmu-fault-lo");
        const bool attributed=rcEvents_==1 && number("NVGspControl-rc-chid")==(g.token&0xffff) &&
            number("NVGspControl-rc-engine")==1 && number("NVGspControl-rc-except-type")==31 &&
            (faultVa&~0xfffULL)==target && g.negative && g.external;
        setProperty("NVGspControl-client615-recover-rc-attributed",attributed);
        if(!g.attempted || !g.cancelled || native615Busy_ || native615Stopping_ || sleeping_ || resetBusy_ ||
           (gmcMmuFaultPending_ && !attributed) || userRpcLatePending_ || userRpcActive_ || userRpcOutstanding_ ||
           postInitPhase_!=27 || s->generation!=resetGeneration()) {
            *stage=g.stage;*status=g.status;*passed=false;ulk();return kIOReturnNotReady;
        }
        if(gmcMmuFaultPending_) {
            gmcMmuFaultPending_=false;mmuNoticesUnpaired_=0;nvdecRcUnpaired_=0;otherMmuFault_=false;
            setProperty("NVGspControl-gmc-mmu-fault-pending",false);
            setProperty("NVGspControl-client615-recover-cleared-attributed-notice",true);
        }
        native615Busy_=true;
        setProperty("NVGspControl-client615-recover-attempted",true);
        setProperty("NVGspControl-native615-busy",true);ulk();
        const IOReturn r=runClientGr615(3);
        lk(__LINE__);native615Busy_=false;
        setProperty("NVGspControl-native615-busy",false);
        setProperty("NVGspControl-client615-recover-status",UInt32(r),32);
        *stage=g.stage;*status=UInt32(r);*passed=r==kIOReturnSuccess;
        if(r==kIOReturnSuccess) {
            // Verified teardown: the root runtime is re-admitted and a fresh
            // client may be created; the failed client's records are kept.
            native615Ready_=true;native615Status_=0;
            setProperty("NVGspControl-native615-ready",true);
            setProperty("NVGspControl-native615-status",UInt32(0),32);
            setProperty("NVGspControl-client615-recovered",true);
            bzero(&g,sizeof(g));
        }
        ulk();return r;
    }
    if(operation==14) {
        auto &g=s->clientGrs[client615Slot_];
        if(!g.attempted || !g.negative || !g.external || g.passed || g.stage!=7 || g.cancelChannel ||
           native615Busy_ || native615Stopping_ || sleeping_ || resetBusy_ || userRpcLatePending_ ||
           userRpcActive_ || userRpcOutstanding_ || postInitPhase_!=27 || s->generation!=resetGeneration()) {
            *stage=g.stage;*status=g.status;*passed=false;ulk();return kIOReturnNotReady;
        }
        native615Busy_=true;
        setProperty("NVGspControl-client615-cancel-attempted",true);
        setProperty("NVGspControl-native615-busy",true);ulk();
        const IOReturn r=runClientGr615(2);
        lk(__LINE__);native615Busy_=false;
        setProperty("NVGspControl-native615-busy",false);
        setProperty("NVGspControl-client615-cancel-status",UInt32(r),32);
        *stage=g.stage;*status=UInt32(r);*passed=r==kIOReturnSuccess;ulk();return r;
    }
    if(operation==16 || operation==17) {   // 0.180.191: NVDEC0 open / submit in an owned client
        auto &g=s->clientGrs[client615Slot_];
        if(!g.attempted || !g.live || g.tornDown || !native615Ready_ || native615Busy_ || native615Stopping_ ||
           sleeping_ || resetBusy_ || gmcMmuFaultPending_ || userRpcLatePending_ || userRpcActive_ ||
           userRpcOutstanding_ || postInitPhase_!=27 || s->generation!=resetGeneration()) {
            *stage=g.nvdecStage;*status=g.nvdecStatus;*passed=g.nvdecScheduled;ulk();return kIOReturnNotReady;
        }
        native615Busy_=true;setProperty("NVGspControl-native615-busy",true);ulk();
        const IOReturn r=runClientGr615(operation==16?6:7);
        lk(__LINE__);native615Busy_=false;setProperty("NVGspControl-native615-busy",false);
        *stage=g.nvdecStage;*status=operation==16?g.nvdecStatus:g.nvdecPut;*passed=g.nvdecScheduled;ulk();return r;
    }
    if(operation==7) {
        auto &g=s->clientGrs[client615Slot_];
        if(!g.attempted || !g.live || g.tornDown || !native615Ready_ || native615Busy_ || native615Stopping_ ||
           sleeping_ || resetBusy_ || gmcMmuFaultPending_ || userRpcLatePending_ || userRpcActive_ ||
           userRpcOutstanding_ || postInitPhase_!=27 || s->generation!=resetGeneration()) {
            *stage=g.stage;*status=g.status;*passed=g.tornDown;ulk();
            return g.tornDown && g.stage==13?kIOReturnSuccess:kIOReturnNotReady;
        }
        g.tornDown=true;g.live=false;native615Busy_=true;
        setProperty("NVGspControl-client615-live",false);
        setProperty("NVGspControl-client615-teardown-attempted",true);
        setProperty("NVGspControl-native615-busy",true);ulk();
        const UInt64 t0=mach_absolute_time();
        const IOReturn r=runClientGr615(1);
        UInt64 ns=0;absolutetime_to_nanoseconds(mach_absolute_time()-t0,&ns);
        lk(__LINE__);g.status=static_cast<UInt32>(r);native615Busy_=false;
        setProperty("NVGspControl-client615-teardown-ms",ns/1000000,32);   // 0.180.119
        if(r!=kIOReturnSuccess) {
            native615Ready_=false;native615Status_=static_cast<UInt32>(r);
            setProperty("NVGspControl-native615-ready",false);
            setProperty("NVGspControl-native615-status",native615Status_,32);
        }
        setProperty("NVGspControl-native615-busy",false);
        setProperty("NVGspControl-client615-teardown-passed",r==kIOReturnSuccess);
        setProperty("NVGspControl-client615-status",g.status,32);
        *stage=g.stage;*status=g.status;*passed=r==kIOReturnSuccess;ulk();return r;
    }
    if(operation==3) {
        auto &r=s->recovery;
        if(r.attempted || native615Busy_ || native615Stopping_ || sleeping_ || resetBusy_ ||
           userRpcLatePending_ || userRpcActive_ || userRpcOutstanding_ || !gmcInitReady_ ||
           postInitPhase_!=27 || s->generation!=resetGeneration() || !s->faultRegistered ||
           !gmcMmuFaultPending_ || native615Ready_ || !p.attempted || p.passed || (p.mode!=2 && p.mode!=5) ||
           p.stage!=3 || p.objectCount!=20 || p.freedCount) {ulk();return kIOReturnNotReady;}
        r.attempted=true;r.noticeCount=mmuFaultEvents_;r.rcCount=rcEvents_;native615Busy_=true;
        setProperty("NVGspControl-recovery615-attempted",true);
        setProperty("NVGspControl-native615-busy",true);ulk();
        const IOReturn result=recoverOwnedFault615();
        lk(__LINE__);r.status=UInt32(result);native615Busy_=false;
        setProperty("NVGspControl-recovery615-status",r.status,32);
        setProperty("NVGspControl-native615-busy",false);
        *stage=r.stage;*status=r.status;*passed=r.teardownVerified;ulk();return result;
    }
    if(operation==1 && s->recovery.teardownVerified && !s->recovery.freshProbeStarted) {
        if(!native615Ready_ || native615Busy_ || native615Stopping_ || p.freedCount!=20 ||
           gmcMmuFaultPending_ || s->generation!=resetGeneration()) {ulk();return kIOReturnNotReady;}
        s->recovery.freshProbeStarted=true;
        bzero(&p,sizeof(p)); // Previous channels are gone, never replay failed work.
        setProperty("NVGspControl-recovery615-fresh-positive-probe",true);
    }
    if(!operation && !native615Busy_ && !native615Stopping_ && s->faultRegistered)
        snapshotNativeFault615(); // CPU observation only, even after failed work.
    if(!operation && p.attempted && !p.passed && !native615Busy_ && !native615Stopping_ &&
       p.stage>=3 && p.stage<=5 && p.objectCount==20 && !p.freedCount && s->generation==resetGeneration()) {
        // A failed protection experiment may still own scheduled channels.
        // Read only their validated allocations; no retry, doorbell or cleanup.
        UInt32 fences[2]={},counters[4]={};
        auto read64=[&](UInt64 at,UInt64 *value) {PraminPteResult access{};
            return praminPteAccess(pci_,at,value,false,false,&access);};
        for(unsigned i=0;i<2;++i) {
            auto &c=p.channels[i];
            nvgsp::readPraminDwords615(c.gpfifo+0x2e00,&fences[i],1,read64);
            nvgsp::readPraminDwords615(c.userd+0x88,counters+i*2,2,read64);
            nvgsp::readPraminDwords615(c.gpfifo+0x1d00,c.output,64,read64);
        }
        setProperty("NVGspControl-isolation615-failed-fences",fences,sizeof(fences));
        setProperty("NVGspControl-isolation615-failed-counters",counters,sizeof(counters));
        setProperty("NVGspControl-isolation615-a-output",p.channels[0].output,sizeof(p.channels[0].output));
        setProperty("NVGspControl-isolation615-b-output",p.channels[1].output,sizeof(p.channels[1].output));
    }
    *stage=p.stage;*status=p.status;*passed=p.passed;
    if(!operation || p.attempted) {
        const IOReturn r=!operation || (p.passed && operation==p.mode)?kIOReturnSuccess:kIOReturnNotReady;
        ulk();return r;
    }
    if(!native615Ready_ || native615Busy_ || native615Stopping_ || sleeping_ ||
       resetBusy_ || gmcMmuFaultPending_ || userRpcLatePending_ || userRpcActive_ ||
       userRpcOutstanding_ || postInitPhase_!=27 || s->generation!=resetGeneration()) {
        ulk();return kIOReturnNotReady;
    }
    if(nvgsp::isolationFaultableVas615(operation) &&
       (!s->faultRegistered || !snapshotNativeFault615())) {
        ulk();return kIOReturnNotReady;
    }
    p.mode=operation;
    setProperty("NVGspControl-isolation615-mode",operation,32);
    p.attempted=true;native615Busy_=true;
    setProperty("NVGspControl-isolation615-attempted",true);
    setProperty("NVGspControl-native615-busy",true);ulk();
    const IOReturn r=runIsolation615();
    lk(__LINE__);if(!p.status)p.status=static_cast<UInt32>(r);p.passed=r==kIOReturnSuccess;
    native615Busy_=false;
    if(r!=kIOReturnSuccess) {
        native615Ready_=false;native615Status_=static_cast<UInt32>(r);
        setProperty("NVGspControl-native615-ready",false);
        setProperty("NVGspControl-native615-status",native615Status_,32);
    }
    setProperty("NVGspControl-native615-busy",false);
    setProperty("NVGspControl-isolation615-passed",p.passed);
    setProperty("NVGspControl-isolation615-status",p.status,32);
    *stage=p.stage;*status=p.status;*passed=p.passed;ulk();return r;
}

IOReturn NVGspControl::runIsolation615() {
    auto *s=native615_;if(!s)return kIOReturnNotReady;
    auto &p=s->isolation;
    constexpr UInt32 C=nvgsp::kLifecycleClient615,D=nvgsp::kLifecycleDevice615;
    constexpr UInt32 S=nvgsp::kLifecycleSubdevice615;
    const bool faultable=nvgsp::isolationFaultableVas615(p.mode);
    UInt64 heapLo=0,heapHi=0;
    lk(__LINE__);
    auto *lo=OSDynamicCast(OSNumber,getProperty("NVGspControl-local-memory-heap-base"));
    auto *hi=OSDynamicCast(OSNumber,getProperty("NVGspControl-local-memory-heap-limit"));
    if(lo)heapLo=lo->unsigned64BitValue();if(hi)heapHi=hi->unsigned64BitValue();
    ulk();if(!heapLo || heapHi<heapLo)return kIOReturnNotReady;
    auto step=[&](UInt32 value) {
        lk(__LINE__);p.stage=value;setProperty("NVGspControl-isolation615-stage",value,32);ulk();
    };
    auto fail=[&](UInt32 value) {lk(__LINE__);p.status=value;ulk();return false;};
    auto publishLedger=[&]() {
        lk(__LINE__);
        setProperty("NVGspControl-isolation615-ledger",p.objects,p.objectCount*4);
        setProperty("NVGspControl-isolation615-object-count",p.objectCount,32);
        setProperty("NVGspControl-isolation615-freed-count",p.freedCount,32);
        ulk();
    };
    auto ctl=[&](UInt32 object,UInt32 command,void *params,UInt32 size) {
        if(!nvgsp::buildControl615(s->request,sizeof(s->request),C,object,command,size))return fail(kIOReturnBadArgument);
        if(size)__builtin_memcpy(s->request+40,params,size);
        UInt32 bytes=sizeof(s->reply),result=~0U;
        const IOReturn r=userRpcImpl(76,s->request,40+size,s->reply,&bytes,&result,true,true);
        if(r!=kIOReturnSuccess)return fail(static_cast<UInt32>(r));
        nvgsp::ControlReply615 reply{};
        if(bytes!=96+size || !nvgsp::decodeControlReply615(s->reply,bytes,C,object,command,size,reply))return fail(kIOReturnBadMessageID);
        if(result || !reply.successful())return fail(result?result:reply.header.status?reply.header.status:kIOReturnError);
        if(size)__builtin_memcpy(params,reply.params,size);return true;
    };
    auto alloc=[&](UInt32 parent,UInt32 object,UInt32 cls,void *params,UInt32 size) {
        if(p.objectCount>=24 || !nvgsp::buildAlloc615(s->request,sizeof(s->request),parent,object,cls,params,size))return fail(kIOReturnBadArgument);
        p.objects[p.objectCount++]=object;publishLedger(); // pre-enqueue, uncertain handles remain pinned
        UInt32 bytes=sizeof(s->reply),result=~0U;
        const IOReturn r=userRpcImpl(103,s->request,32+size,s->reply,&bytes,&result,true,true);
        if(r!=kIOReturnSuccess)return fail(static_cast<UInt32>(r));
        const UInt8 *returned=nullptr;
        const auto reply=nvgsp::decodeAlloc615(s->reply,bytes,parent,object,cls,size,&returned);
        if(bytes!=88+size || !reply.valid)return fail(kIOReturnBadMessageID);
        if(result || !reply.successful())return fail(result?result:reply.status?reply.status:kIOReturnError);
        if(size)__builtin_memcpy(params,returned,size);return true;
    };
    auto release=[&](UInt32 object) {
        const nvgsp::FreeParams615 request{C,0,object,0};
        UInt32 bytes=sizeof(s->reply),result=~0U;
        const IOReturn r=userRpcImpl(10,reinterpret_cast<const UInt8 *>(&request),sizeof(request),s->reply,&bytes,&result,true,true);
        if(r!=kIOReturnSuccess)return fail(static_cast<UInt32>(r));
        const UInt8 *payload=nullptr;UInt32 count=0;nvgsp::LifecycleReply615 reply{};
        if(bytes!=72 || !nvgsp::lifecyclePayload615(s->reply,bytes,10,16,payload,count,reply) || count!=16 ||
           nvgsp::control615U32(payload)!=C || nvgsp::control615U32(payload+4) ||
           nvgsp::control615U32(payload+8)!=object || nvgsp::control615U32(payload+12) ||
           result || reply.rpcResult || reply.privateResult)return fail(kIOReturnBadMessageID);
        for(UInt32 i=0;i<p.objectCount;++i)if(p.objects[i]==object) {p.objects[i]=0;++p.freedCount;publishLedger();return true;}
        return fail(kIOReturnInternalError);
    };
    auto read=[&](UInt64 at,UInt32 count) {return vramAccess(at,s->readWords,count,false,true)==kIOReturnSuccess;};
    auto write=[&](UInt64 at,const UInt32 *words,UInt32 count) {
        if(!count || count>1024)return false;
        if(words!=s->writeWords)__builtin_memcpy(s->writeWords,words,count*4);
        return vramAccess(at,s->writeWords,count,true,true)==kIOReturnSuccess && read(at,count) &&
            !__builtin_memcmp(s->writeWords,s->readWords,count*4);
    };
    auto healthy=[&]() {
        for(unsigned i=0;i<5;++i) {
            IOSleep(100);if(pollStatus()!=kIOReturnSuccess)return false;
            lk(__LINE__);
            const bool ok=!gmcMmuFaultPending_ && !userRpcLatePending_ && !resetBusy_ && !sleeping_ &&
                !native615Stopping_ && postInitPhase_==27 && s->generation==resetGeneration();
            auto *bad=OSDynamicCast(OSNumber,getProperty("NVGspControl-status-bad-reason"));
            const bool reason=bad && bad->unsigned32BitValue();ulk();if(!ok || reason)return false;
        }
        return true;
    };
    step(1);
    if(!healthy() || !read(s->gpfifo+0xe00,1) || s->readWords[0]!=s->fenceExpected ||
       !read(s->userd+0x88,2) || s->readWords[0]!=s->put || s->readWords[1]!=s->put)return kIOReturnNotReady;
    for(UInt32 index=0;index<2;++index) {
        auto &c=p.channels[index];const UInt32 h=0xc0d00100+index*0x100;
        c.vas=h|0xf1;c.tsg=h|0x50;c.share=h|0x51;c.channel=h|0x56;
        nvgsp::VaspaceParams615 vas{}; // GPU_NEW=0
        vas.flags=faultable ? nvgsp::kExternalFaultVasFlags615 : 0;
        if(!alloc(D,c.vas,0x90f1,&vas,sizeof(vas)) || !vas.vaSize || vas.vaSize>UINT64_MAX-vas.vaBase)return kIOReturnNotReady;
        if(vas.flags!=(faultable ? nvgsp::kExternalFaultVasFlags615 : 0))return kIOReturnBadArgument;
        c.vaBase=vas.vaBase;c.vaBytes=vas.vaSize;
        if(faultable) {
            c.va=nvgsp::isolationProbeVa615(p.mode,index);
            if(c.va<c.vaBase || c.va>=c.vaBase+c.vaBytes || 0x3000>c.vaBase+c.vaBytes-c.va)return kIOReturnBadArgument;
            nvgsp::MemoryParams615 tree{};tree.owner=C;tree.type=6;tree.flags=0x100;tree.attr=0x800000;
            tree.size=nvgsp::kExternalTreeBytes615;tree.alignment=4096;
            if(!alloc(S,h|0x57,0x40,&tree,sizeof(tree)) || tree.size!=nvgsp::kExternalTreeBytes615 ||
               !nvgsp::memoryProbeRange615(tree,gmcConfig_.fbBytes,heapLo,heapHi))return kIOReturnNotReady;
            c.pageTree=tree.offset;
            for(UInt64 page=0;page<tree.size;page+=4096) {
                bzero(s->writeWords,sizeof(s->writeWords));if(!write(tree.offset+page,s->writeWords,1024))return kIOReturnIOError;
            }
            nvgsp::SetPageDirectory615 directory{c.pageTree,4,8,c.vas,0,1,0};
            if(!ctl(D,0x801813,&directory,sizeof(directory)))return kIOReturnNotReady;
            if(directory.physical!=c.pageTree || directory.entries!=4 || directory.flags!=8 ||
               directory.vas!=c.vas || directory.channel || directory.subdevice!=1 || directory.pasid)return kIOReturnBadArgument;
            lk(__LINE__);
            setProperty(index?"NVGspControl-isolation615-b-external-directory":"NVGspControl-isolation615-a-external-directory",&directory,sizeof(directory));
            setProperty("NVGspControl-isolation615-external-vas-flags",vas.flags,32);ulk();
        } else {
            nvgsp::MemoryParams615 window{};window.owner=C;window.type=6;
            window.flags=0x80100;window.attr=0x800000;window.size=0x3000;window.alignment=4096;window.vaSpace=c.vas;
            if(!alloc(D,h|0x57,0x50a0,&window,sizeof(window)) || window.owner!=C || window.type!=6 ||
               window.size!=0x3000 || window.limit!=0x2fff || window.offset&4095 || window.offset<c.vaBase ||
               window.offset>=c.vaBase+c.vaBytes || 0x3000>c.vaBase+c.vaBytes-window.offset)return kIOReturnBadArgument;
            c.va=window.offset;
        }
        nvgsp::ChannelGroup615 group{};group.hVASpace=c.vas;group.engineType=9;
        nvgsp::ContextShare615 share{c.vas,0,~0U};
        if(!alloc(D,c.tsg,0xa06c,&group,sizeof(group)) || !alloc(c.tsg,c.share,0x9067,&share,sizeof(share)) ||
           share.hVASpace!=c.vas || share.flags || share.subctxId==~0U)return kIOReturnNotReady;
        UInt32 methodBytes=0;
        if(!ctl(S,0x20802a08,&methodBytes,4) || !methodBytes || methodBytes>16*1024*1024)return kIOReturnBadArgument;
        c.methodBytes=(UInt64(methodBytes)+4095)&~4095ULL;
        UInt64 *physical[4]={&c.instance,&c.method,&c.userd,&c.gpfifo};
        for(UInt32 i=0;i<4;++i) {
            nvgsp::MemoryParams615 mem{};mem.owner=C;mem.type=6;mem.flags=0x100;mem.attr=0x800000;
            mem.size=i==1?c.methodBytes:i==3?0x3000:4096;mem.alignment=4096;const UInt64 size=mem.size;
            if(!alloc(S,h|(0x52+i),0x40,&mem,sizeof(mem)))return kIOReturnNotReady;
            if(!nvgsp::memoryProbeRange615(mem,gmcConfig_.fbBytes,heapLo,heapHi) || mem.size!=size)return kIOReturnBadArgument;
            *physical[i]=mem.offset;
            for(UInt64 page=0;page<size;page+=4096) {
                bzero(s->writeWords,sizeof(s->writeWords));if(!write(mem.offset+page,s->writeWords,1024))return kIOReturnIOError;
            }
        }
        // External VAS owns every table; managed VAS queries RM's exact leaf.
        if(faultable) {
            nvgsp::ExternalSmallTree615 tree{};
            if(!nvgsp::externalSmallTree615(c.pageTree,c.va,&tree))return kIOReturnBadArgument;
            if(c.gpfifo<c.pageTree+nvgsp::kExternalTreeBytes615 && c.pageTree<c.gpfifo+0x3000)return kIOReturnBadArgument;
            for(unsigned i=0;i<4;++i) {
                UInt32 value[2]={UInt32(tree.value[i]),UInt32(tree.value[i]>>32)};
                if(!write(tree.address[i],value,2))return kIOReturnIOError;
            }
            c.pteAddress=tree.leaf;
            lk(__LINE__);
            setProperty(index?"NVGspControl-isolation615-b-external-tree":"NVGspControl-isolation615-a-external-tree",&tree,sizeof(tree));ulk();
        } else {
        UInt8 query[208]{};__builtin_memcpy(query,&c.va,8);__builtin_memcpy(query+200,&c.vas,4);
        if(!ctl(D,0x801809,query,sizeof(query)))return kIOReturnNotReady;
        UInt64 table=0;UInt32 coverage=0,matches=0;
        for(unsigned i=0;i<5;++i) {
            const UInt8 *q=query+32+i*32;if(nvgsp::control615U32(q+16)!=4096)continue;
            __builtin_memcpy(&table,q,8);coverage=nvgsp::control615U32(q+24);++matches;
            if(nvgsp::control615U32(q+12)!=8 || nvgsp::control615U32(q+20))return kIOReturnBadArgument;
        }
        if(matches!=1 || coverage<4096 || (coverage&(coverage-1)) || table<heapLo || table>heapHi ||
           UInt64(coverage)/4096*8-1>heapHi-table)return kIOReturnBadArgument;
        c.pteAddress=table+(c.va%coverage)/4096*8;
        if(coverage-c.va%coverage<0x3000)return kIOReturnBadArgument;
        }
        if(!read(c.pteAddress,6))return kIOReturnIOError;
        for(unsigned i=0;i<6;++i)if(s->readWords[i])return kIOReturnBusy;
        UInt32 pte[6]{};
        for(unsigned i=0;i<3;++i) {
            const bool validPte=p.mode==4 ? nvgsp::privilegedCompletionPte615(c.gpfifo+i*4096,i,&c.pteValue[i]) :
                nvgsp::isolationPte615(c.gpfifo+i*4096,i,p.mode==2,&c.pteValue[i]);
            if(!validPte)return kIOReturnBadArgument;
            pte[i*2]=UInt32(c.pteValue[i]);pte[i*2+1]=UInt32(c.pteValue[i]>>32);
        }
        c.mapped=true; // a failed write/invalidate still retains all resources
        if(!write(c.pteAddress,pte,6))return kIOReturnIOError;
        UInt32 inv[2]={c.vas,0};if(!ctl(D,0x80180c,inv,8))return kIOReturnNotReady;
        nvgsp::ChannelAlloc615 channel{};
        channel.gpFifoOffset=c.va;channel.gpFifoEntries=256;
        channel.flags=nvgsp::isolationChannelFlags615(p.mode);
        channel.hContextShare=c.share;channel.hUserdMemory[0]=h|0x54;channel.engineType=9;
        channel.instanceMem={c.instance,4096,2,1};channel.userdMem={c.userd,4096,2,1};
        channel.ramfcMem={c.instance,512,2,1};channel.mthdbufMem={c.method,c.methodBytes,2,0};
        channel.hPhysChannelGroup=c.tsg;channel.internalFlags=nvgsp::isolationInternalFlags615(p.mode);
        if(!alloc(c.tsg,c.channel,0xc56f,&channel,sizeof(channel)) ||
           channel.internalFlags!=nvgsp::isolationInternalFlags615(p.mode) ||
           channel.flags!=nvgsp::isolationChannelFlags615(p.mode))return kIOReturnNotReady;
        c.cid=channel.cid;c.internalFlags=channel.internalFlags;c.flags=channel.flags;
        UInt32 copyParams[2]={1,9};
        if(!alloc(c.channel,h|0x58,0xc7b5,copyParams,8) || copyParams[0]!=1 || copyParams[1]!=9)return kIOReturnNotReady;
        UInt32 engine=9;
        if(!ctl(c.channel,0xa06f0104,&engine,4) || !ctl(c.channel,0xc36f0108,&c.token,4))return kIOReturnNotReady;
        UInt8 schedule[3]={1,0,0};c.scheduled=true;
        if(!ctl(c.channel,0xa06f0103,schedule,3) || !healthy())return kIOReturnNotReady;
        for(UInt32 i=0;i<64;++i)s->writeWords[i]=nvgsp::isolationCanary615(index,i);
        if(!write(c.gpfifo+0x1c00,s->writeWords,64))return kIOReturnIOError;
        step(2+index);
    }
    auto &a=p.channels[0];auto &b=p.channels[1];
    if((p.mode==5 ? a.va==b.va : a.va!=b.va) ||
       a.pteAddress==b.pteAddress || a.gpfifo==b.gpfifo || a.cid==b.cid || a.token==b.token)return kIOReturnBadArgument;
    lk(__LINE__);
    setProperty("NVGspControl-isolation615-protected-pre-submit",p.channels,sizeof(p.channels));
    setProperty("NVGspControl-isolation615-a-ptes",a.pteValue,sizeof(a.pteValue));
    setProperty("NVGspControl-isolation615-b-ptes",b.pteValue,sizeof(b.pteValue));ulk();
    if(faultable) {
        for(unsigned i=0;i<2;++i) {
            if(!read(p.channels[i].instance+0xf4,1))return kIOReturnIOError;
            p.channels[i].ramfcConfig=s->readWords[0];
        }
        UInt64 instance[2]={a.instance,b.instance},va[2]={a.va,b.va},physical[2]={a.gpfifo,b.gpfifo};
        UInt32 configs[2]={a.ramfcConfig,b.ramfcConfig},cids[2]={a.cid,b.cid};
        lk(__LINE__);
        setProperty(p.mode==4?"NVGspControl-isolation615-privileged-instances":"NVGspControl-isolation615-negative-instances",instance,sizeof(instance));
        setProperty(p.mode==4?"NVGspControl-isolation615-privileged-virtual-addresses":"NVGspControl-isolation615-negative-virtual-addresses",va,sizeof(va));
        setProperty(p.mode==4?"NVGspControl-isolation615-privileged-physical-addresses":"NVGspControl-isolation615-negative-physical-addresses",physical,sizeof(physical));
        setProperty(p.mode==4?"NVGspControl-isolation615-privileged-ramfc-config":"NVGspControl-isolation615-negative-ramfc-config",configs,sizeof(configs));
        setProperty(p.mode==4?"NVGspControl-isolation615-privileged-cids":"NVGspControl-isolation615-negative-cids",cids,sizeof(cids));
        if(p.mode==2 || p.mode==5)setProperty("NVGspControl-isolation615-negative-expected-fault-page",(p.mode==5?b.va:a.va)+0x1000,64);
        ulk();
        // Fixed admin mode4 must have actual privileged RAMFC auth before any
        // submission; mode2 must remain nonprivileged for denial proof.
        for(unsigned i=0;i<2;++i)
            if(p.mode==4 ? !(p.channels[i].ramfcConfig&0x100) :
                           (p.channels[i].ramfcConfig&0x100))return kIOReturnNotPrivileged;
    }
    // Both destinations are initially zero, and A must not change B.
    for(UInt32 i=0;i<2;++i) {
        if(!read(p.channels[i].gpfifo+0x1d00,64))return kIOReturnIOError;
        for(UInt32 w=0;w<64;++w)if(s->readWords[w])return kIOReturnIOError;
    }
    for(UInt32 index=0;index<2;++index) {
        auto &c=p.channels[index];c.fence=0x6150ca43+index;
        const bool packet=p.mode==5 && index==0 ?
            nvgsp::crossVasDenialCopy615(c.va,b.va,c.fence,s->writeWords) :
            nvgsp::protectedIsolationCopy615(c.va,c.fence,s->writeWords);
        if(!packet || !write(c.gpfifo+0x800,s->writeWords,19))return kIOReturnIOError;
        const UInt64 pb=c.va+0x800;
        UInt32 entry[4]={UInt32(pb)&0xfffffffc,(UInt32(pb>>32)&0xff)|(19U<<10),0,0};
        const UInt32 put=p.mode==4?2:1;
        if(p.mode==4 && !nvgsp::privilegedCompletionEntries615(c.va,entry))return kIOReturnBadArgument;
        if(p.mode==4) {lk(__LINE__);setProperty(index?"NVGspControl-isolation615-b-gpfifo-entries":"NVGspControl-isolation615-a-gpfifo-entries",entry,sizeof(entry));ulk();}
        if(!read(c.userd+0x88,2) || s->readWords[0] || s->readWords[1] ||
           !write(c.gpfifo,entry,put*2) || !write(c.userd+0x8c,&put,1) ||
           diagnosticChannelKick(c.channel,true)!=kIOReturnSuccess)return kIOReturnIOError;
        UInt64 deadline=0;clock_interval_to_deadline(5,kSecondScale,&deadline);bool completed=false;
        do {
            if(!read(c.gpfifo+0x2e00,1))return kIOReturnIOError;
            if(s->readWords[0]==c.fence) {completed=true;break;}IOSleep(1);
        } while(mach_absolute_time()<deadline);
        if(p.mode==4) {lk(__LINE__);setProperty("NVGspControl-isolation615-privileged-host-completed",completed);ulk();}
        if(p.mode==2 || p.mode==5) {
            lk(__LINE__);setProperty("NVGspControl-isolation615-negative-host-completed",completed);ulk();
            // A negative probe never submits B or frees either channel, even
            // if the denied transfer unexpectedly completes. Independent
            // evidence, not this timeout, decides whether protection worked.
            return completed ? kIOReturnIOError : kIOReturnTimeout;
        }
        if(!completed || !read(c.userd+0x88,2) || s->readWords[0]!=put || s->readWords[1]!=put || !healthy())return kIOReturnTimeout;
        if(!read(c.instance+0xf4,1))return kIOReturnIOError;
        c.ramfcConfig=s->readWords[0];
        if(p.mode==4 ? !(c.ramfcConfig&0x100) : (c.ramfcConfig&0x100))return kIOReturnNotPrivileged;
        for(UInt32 peer=0;peer<2;++peer) {
            if(!read(p.channels[peer].gpfifo+0x1d00,64))return kIOReturnIOError;
            for(UInt32 w=0;w<64;++w)if(s->readWords[w]!=(peer<=index?nvgsp::isolationCanary615(peer,w):0))return kIOReturnIOError;
            if(peer==index)__builtin_memcpy(c.output,s->readWords,sizeof(c.output));
        }
        step(4+index);
    }
    lk(__LINE__);
    setProperty("NVGspControl-isolation615-channel-evidence",p.channels,sizeof(p.channels));
    setProperty("NVGspControl-isolation615-a-output",a.output,sizeof(a.output));
    setProperty("NVGspControl-isolation615-b-output",b.output,sizeof(b.output));ulk();
    UInt64 vas[2]={a.va,b.va},ptes[2]={a.pteAddress,b.pteAddress},phys[2]={a.gpfifo,b.gpfifo};
    UInt32 cids[2]={a.cid,b.cid},tokens[2]={a.token,b.token},flags[2]={a.internalFlags,b.internalFlags};
    lk(__LINE__);
    setProperty("NVGspControl-isolation615-virtual-addresses",vas,sizeof(vas));
    setProperty("NVGspControl-isolation615-pte-addresses",ptes,sizeof(ptes));
    setProperty("NVGspControl-isolation615-physical-addresses",phys,sizeof(phys));
    setProperty("NVGspControl-isolation615-cids",cids,sizeof(cids));
    setProperty("NVGspControl-isolation615-tokens",tokens,sizeof(tokens));
    setProperty("NVGspControl-isolation615-flags",flags,sizeof(flags));
    UInt32 auth[2]={a.ramfcConfig,b.ramfcConfig};
    setProperty("NVGspControl-isolation615-ramfc-config",auth,sizeof(auth));
    setProperty("NVGspControl-isolation615-a-ptes",a.pteValue,sizeof(a.pteValue));
    setProperty("NVGspControl-isolation615-b-ptes",b.pteValue,sizeof(b.pteValue));ulk();
    // Disable both before freeing anything. A timeout/refusal pins the rest.
    for(UInt32 i=0;i<2;++i) {
        UInt8 disable[3]={0,0,0};if(!ctl(p.channels[i].channel,0xa06f0103,disable,3))return kIOReturnNotReady;
        p.channels[i].scheduled=false;
    }
    if(!healthy())return kIOReturnNotReady;step(6);
    for(UInt32 index=0;index<2;++index) {
        auto &c=p.channels[index];const UInt32 h=0xc0d00100+index*0x100;
        if(!release(h|0x58) || !release(c.channel))return kIOReturnNotReady;
        // Channel is gone; remove its exact PTE and flush BEFORE backing free.
        if(!read(c.pteAddress,6))return kIOReturnIOError;
        for(unsigned i=0;i<3;++i)if(s->readWords[i*2]!=UInt32(c.pteValue[i]) ||
            s->readWords[i*2+1]!=UInt32(c.pteValue[i]>>32))return kIOReturnIOError;
        const UInt32 empty[6]={0,0,0,0,0,0};UInt32 inv[2]={c.vas,0};
        if(!write(c.pteAddress,empty,6) || !ctl(D,0x80180c,inv,8))return kIOReturnIOError;
        c.mapped=false;
        if(!release(h|0x55) || !release(h|0x54) || !release(h|0x53) || !release(h|0x52) ||
           !release(c.share) || !release(c.tsg))return kIOReturnNotReady;
        // An external VAS borrows the owned tree, so release it before the tree.
        if(faultable ? (!release(c.vas) || !release(h|0x57)) :
                       (!release(h|0x57) || !release(c.vas)))return kIOReturnNotReady;
        step(7+index);
    }
    if(p.objectCount!=20 || p.freedCount!=20 || !healthy() ||
       !read(s->gpfifo+0xe00,1) || s->readWords[0]!=s->fenceExpected ||
       !read(s->userd+0x88,2) || s->readWords[0]!=s->put || s->readWords[1]!=s->put)return kIOReturnNotReady;
    step(9);return kIOReturnSuccess;
}

// 0.180.92: process-owned GR client. The creating user client is the owner;
// only it may destroy the client, and its close/stop tears the client down.
// One client at a time; a torn-down (stage 13) client slot is reused.
IOReturn NVGspControl::nativeClient615(const void *owner,UInt32 operation,UInt64 *stage,
                                       UInt64 *status,UInt64 *passed,UInt64 pbVa,UInt64 pbWords) {
    if(!lock_ || !owner || !stage || !status || !passed || operation>5)return kIOReturnBadArgument;
    *stage=*status=*passed=0;lk(__LINE__);
    auto *s=native615_;
    if(!s) {ulk();return kIOReturnNotReady;}
    // 0.180.112: one slot per connection; create takes a free slot.
    int slot=ownedSlot615(owner);
    // 0.180.191: 4 = open an NVDEC0 channel in the caller's client, 5 = submit {pbVa, words} on it
    if(operation==4 || operation==5) {
        if(slot<0 || !s->clientGrs[slot].userBuffer) {ulk();return kIOReturnNotFound;}
        if(operation==5) {s->clientGrs[slot].nvdecPbVa=pbVa;s->clientGrs[slot].nvdecPbWords=UInt32(pbWords>0x1fffff?0:pbWords);}
        ulk();
        return nativeIsolation615(operation==4?16:17,stage,status,passed,slot);
    }
    if(operation==2) {
        if(slot<0) {ulk();return kIOReturnNotFound;}
        const auto &q=s->clientGrs[slot];
        *stage=q.stage;*status=q.status;*passed=q.live;ulk();return kIOReturnSuccess;
    }
    if(operation==1 && slot<0) {ulk();return anyOwned615()?kIOReturnNotPermitted:kIOReturnNotFound;}
    if(operation==0 || operation==3) {
        if(slot>=0) {ulk();return kIOReturnBusy;}   // already owns a client
        for(UInt32 k=0;k<nvgsp::kClientSlots615 && slot<0;++k) {
            if(client615Owners_[k] || client615BufferOwners_[k])continue;
            auto &q=s->clientGrs[k];
            if(q.attempted && q.tornDown && q.stage==13 && q.status==0 && !q.live &&
               !(native615Busy_ && client615Slot_==k))bzero(&q,sizeof(q));
            if(!q.attempted && !nativeUserDatas_[k].size())slot=int(k);
        }
        if(slot<0) {ulk();return kIOReturnBusy;}
        auto &q=s->clientGrs[slot];
        client615Owners_[slot]=owner;   // claim before the unlocked transaction
        q.userBuffer=operation==3;
        if(q.userBuffer)client615BufferOwners_[slot]=owner;   // 0.180.109
        setProperty("NVGspControl-client615-user-buffer",q.userBuffer);
    }
    auto &g=s->clientGrs[slot];
    ulk();
    // 0.180.100: a caller-buffer client's bad store must be a replayable,
    // cancellable fault: the owned replay buffer is registered once per boot.
    if(operation==3 && !nativeReplayData_.size()) {
        lk(__LINE__);
        IOReturn replay=kIOReturnSuccess;
        if(!waitIdle615())replay=kIOReturnBusy;
        else if(!nativeReplayData_.size()) {native615Busy_=true;ulk();replay=registerReplayableFault615();lk(__LINE__);native615Busy_=false;}
        if(replay!=kIOReturnSuccess) {
            client615Owners_[slot]=client615BufferOwners_[slot]=nullptr;g.userBuffer=false;ulk();return replay;
        }
        ulk();
    }
    const IOReturn r=nativeIsolation615(operation==1?7:13,stage,status,passed,slot);
    lk(__LINE__);
    if(operation!=1 && r!=kIOReturnSuccess && !g.live) {
        client615Owners_[slot]=nullptr;
        if(!g.attempted)client615BufferOwners_[slot]=nullptr;   // never reached the GPU
    }
    if(operation==1 && r==kIOReturnSuccess) {
        client615Owners_[slot]=nullptr;
        if(g.tornDown && g.stage==13)bzero(&g,sizeof(g));
    }
    setProperty("NVGspControl-client615-owned",anyOwned615());
    setProperty("NVGspControl-client615-last-slot",UInt32(slot),32);
    ulk();return r;
}

IOMemoryDescriptor *NVGspControl::client615UserMemory(const void *owner) {
    if(!lock_ || !owner)return nullptr;
    lk(__LINE__);
    IOMemoryDescriptor *d=nullptr;
    const int k=native615_?ownedSlot615(owner):-1;
    if(k>=0 && native615_->clientGrs[k].userBuffer && native615_->clientGrs[k].live && nativeUserDatas_[k].descriptor()) {
        d=nativeUserDatas_[k].descriptor();d->retain();
    }
    ulk();return d;
}

void NVGspControl::nativeClient615Close(const void *owner) {
    if(!lock_ || !owner)return;
    lk(__LINE__);const bool mine=ownedSlot615(owner)>=0;ulk();
    if(mine) {
        UInt64 stage=0,status=0,passed=0;
        const IOReturn r=nativeClient615(owner,1,&stage,&status,&passed);
        lk(__LINE__);
        setProperty("NVGspControl-client615-close-teardown-status",UInt32(r),32);
        setProperty("NVGspControl-client615-close-teardown-stage",UInt32(stage),32);
        if(r!=kIOReturnSuccess)setProperty("NVGspControl-client615-close-orphaned",true);
        ulk();
    }
    // 0.180.109/112: release a slot's caller buffers only for their owner, and
    // only once its client is gone (a failed teardown leaves them pinned).
    for(UInt32 k=0;k<nvgsp::kClientSlots615;++k) {
        lk(__LINE__);
        const bool release=client615BufferOwners_[k]==owner && client615Owners_[k]!=owner;
        if(release)client615BufferOwners_[k]=nullptr;
        ulk();
        if(release) {
            nativeUserDatas_[k].release();
            for(auto &a:nativeAllocDatas_[k])a.release();
            releaseVram615(k);   // 0.180.131
            releaseWraps615(k);   // 0.180.117
            lk(__LINE__);setProperty("NVGspControl-client615-buffers-released",true);ulk();
        }
    }
}

// 0.180.98: the owner runs its own NAK on its live client channel, inside the
// client's VAS. Same fixed 64x1x1 block shape the header checker enforces;
// the constant words (output pointers etc.) are the caller's and can only
// name its own VAS. A timeout or fault disables the runtime (pinned).
// 0.180.99 input: {'NVD1', gridX, constantBytes, 0} + constants + NAK.
IOReturn NVGspControl::nativeClientDispatch615(const void *owner,const UInt8 *input,UInt32 bytes,UInt64 out[3]) {
    if(!lock_ || !owner || !out || (bytes && !input))return kIOReturnBadArgument;
    out[0]=out[1]=out[2]=0;
    UInt32 codeBytes=0,registers=0,head[4]={};
    if(bytes>=16)__builtin_memcpy(head,input,16);
    const UInt32 gridX=head[1],constantBytes=head[2],nakOffset=16+constantBytes;
    if(bytes && (bytes<16 || head[0]!=0x3144564e || !gridX || gridX>65535 || constantBytes<16 ||
                 constantBytes>256 || (constantBytes&15) || head[3] || bytes<nakOffset+80 ||
                 !nvgsp::computeNak615(input+nakOffset,bytes-nakOffset,&codeBytes,&registers) ||
                 codeBytes>0x8000))return kIOReturnBadArgument;
    lk(__LINE__);
    auto *s=native615_;
    const int owned=s?ownedSlot615(owner):-1;
    if(!s || owned<0) {ulk();return s && anyOwned615()?kIOReturnNotPermitted:kIOReturnNotFound;}
    // 0.180.112: wait for another client's operation, then work on ours
    if(!waitIdle615() || client615Owners_[owned]!=owner || !selectClient615(UInt32(owned))) {ulk();return kIOReturnBusy;}
    auto &g=s->clientGrs[client615Slot_];
    if(!g.live || !g.scheduled || !g.userBuffer || !g.fence ||   /* put wraps to 0 */ !native615Ready_ || native615Busy_ ||
       native615Stopping_ || sleeping_ || resetBusy_ || gmcMmuFaultPending_ || userRpcLatePending_ ||
       postInitPhase_!=27 || s->generation!=resetGeneration()) {ulk();return kIOReturnNotReady;}
    out[0]=g.userVa;out[1]=g.dispatches;
    if(!bytes) {ulk();return kIOReturnSuccess;}
    native615Busy_=true;const UInt32 rcBefore=rcEvents_;lastSubmit615_[client615Slot_]=mach_absolute_time();
    setProperty("NVGspControl-native615-busy",true);ulk();
    const UInt32 slot=g.put,put=(g.put+1)%256,sequence=g.dispatches+1;
    const UInt32 computeRelease=0xc0440000U|sequence,hostFence=0x6150c400U+sequence;
    const UInt64 va=g.scratchVa,physical=g.scratchPhysical,semaphore=g.gpfifoVa+0xe00,pb=g.gpfifoVa+0x800;
    auto read=[&](UInt64 at,UInt32 count) {return vramAccess(at,s->readWords,count,false,true)==kIOReturnSuccess;};
    auto write=[&](UInt64 at,const UInt32 *words,UInt32 count) {
        if(!count || count>1024)return false;
        if(words!=s->writeWords)__builtin_memcpy(s->writeWords,words,count*4);
        return vramAccess(at,s->writeWords,count,true,true)==kIOReturnSuccess && read(at,count) &&
            !__builtin_memcmp(s->writeWords,s->readWords,count*4);
    };
    const IOReturn r=[&]() -> IOReturn {
        // The previous submission must be complete before scratch is reused.
        if(!read(g.gpfifo+0xe00,1) || s->readWords[0]!=g.fence ||
           !read(g.userd+0x88,2) || s->readWords[0]!=g.put || s->readWords[1]!=g.put)return kIOReturnNotReady;
        if(!nvgsp::computePacket615(va,va+0x8000,va+0x9000,va+0xaf00,registers,computeRelease,&s->compute,
                                   gridX,constantBytes))return kIOReturnBadArgument;
        for(UInt32 done=0;done<codeBytes;) {
            const UInt32 count=(codeBytes-done)>4096?1024:(codeBytes-done)/4;
            __builtin_memcpy(s->writeWords,input+nakOffset+64+done,count*4);
            if(!write(physical+done,s->writeWords,count))return kIOReturnIOError;
            done+=count*4;
        }
        __builtin_memcpy(s->writeWords,input+16,constantBytes);
        if(!write(physical+0x9000,s->writeWords,constantBytes/4))return kIOReturnIOError;
        const UInt32 zero=0;
        if(!write(physical+0x8000,s->compute.qmd,64) || !write(physical+0xaf00,&zero,1))return kIOReturnIOError;
        const UInt32 n=s->compute.commandCount;if(n+6>256)return kIOReturnBadArgument;
        __builtin_memcpy(s->writeWords,s->compute.commands,n*4);
        const UInt32 tail[6]={0x20050017,UInt32(semaphore)&0xfffffffc,UInt32(semaphore>>32)&0xff,hostFence,0,0x00100001};
        __builtin_memcpy(s->writeWords+n,tail,sizeof(tail));
        if(!write(g.gpfifo+0x800,s->writeWords,n+6))return kIOReturnIOError;
        const UInt32 entry[2]={UInt32(pb)&0xfffffffc,(UInt32(pb>>32)&0xff)|((n+6)<<10)};
        if(!write(g.gpfifo+UInt64(slot)*8,entry,2) || !write(g.userd+0x8c,&put,1) ||
           diagnosticChannelKick(0xc0d00356U|client615Slot_<<16,true)!=kIOReturnSuccess)return kIOReturnIOError;
        UInt64 deadline=0;clock_interval_to_deadline(5,kSecondScale,&deadline);
        UInt64 spin=0;clock_interval_to_deadline(2,kMillisecondScale,&spin);   // 0.180.159: see mmuSubmit615
        do {
            if(!read(physical+0xaf00,1))return kIOReturnIOError;
            const bool released=s->readWords[0]==computeRelease;
            if(!read(g.gpfifo+0xe00,1))return kIOReturnIOError;
            if(released && s->readWords[0]==hostFence)
                return read(g.userd+0x88,2) && s->readWords[0]==put && s->readWords[1]==put ?
                    kIOReturnSuccess : kIOReturnIOError;
            if(mach_absolute_time()<spin)IODelay(5); else IOSleep(1);
        } while(mach_absolute_time()<deadline);
        return kIOReturnTimeout;
    }();
    lk(__LINE__);
    const bool clean=r==kIOReturnSuccess && rcEvents_==rcBefore && !gmcMmuFaultPending_;
    const bool recovered=!clean && r==kIOReturnTimeout && rcEvents_==rcBefore && nativeReplayData_.size() &&
        recoverOwnedFault615(rcBefore);
    if(clean) {g.put=put;g.fence=hostFence;g.dispatches=sequence;g.completed=sequence;out[1]=sequence;out[2]=codeBytes;}
    else if(!recovered) {
        native615Ready_=false;native615Status_=static_cast<UInt32>(r?r:kIOReturnIOError);
        setProperty("NVGspControl-native615-ready",false);
        setProperty("NVGspControl-native615-status",native615Status_,32);
    }
    native615Busy_=false;
    setProperty("NVGspControl-native615-busy",false);
    setProperty("NVGspControl-client615-dispatches",g.dispatches,32);
    setProperty("NVGspControl-client615-dispatch-status",UInt32(r),32);
    ulk();return clean?kIOReturnSuccess:recovered?kIOReturnAborted:(r?r:kIOReturnIOError);
}

// 0.180.101: MEM_OP + HOST semaphore on the permanent privileged maintenance
// channel (built by runClientGr615). The caller holds native615Busy_.
bool NVGspControl::mmuSubmit615(UInt32 memOpC) {
    auto *s=native615_;if(!s || !s->mmu.live)return false;
    auto &m=s->mmu;
    auto read=[&](UInt64 at,UInt32 count) {return vramAccess(at,s->readWords,count,false,true)==kIOReturnSuccess;};
    auto write=[&](UInt64 at,const UInt32 *words,UInt32 count) {
        if(!count || count>1024)return false;
        if(words!=s->writeWords)__builtin_memcpy(s->writeWords,words,count*4);
        return vramAccess(at,s->writeWords,count,true,true)==kIOReturnSuccess && read(at,count) &&
            !__builtin_memcmp(s->writeWords,s->readWords,count*4);
    };
    const UInt32 fence=0x6150d000|((m.fence+1)&0xfff);
    const UInt64 semaphore=m.va+0xe00,pb=m.va+0x800;
    const UInt32 words[11]={0x2004000a,0,0,memOpC,0x48000000,
        0x20050017,UInt32(semaphore)&0xfffffffc,UInt32(semaphore>>32)&0xff,fence,0,0x00100001};
    const UInt32 entry[2]={UInt32(pb)&0xfffffffc,(UInt32(pb>>32)&0xff)|(11U<<10)};
    const UInt32 put=(m.put+1)%256;
    if(!write(m.ring+0x800,words,11) || !write(m.ring+UInt64(m.put)*8,entry,2) || !write(m.userd+0x8c,&put,1) ||
       diagnosticChannelKick(0xc0d00556,true)!=kIOReturnSuccess)return false;
    UInt64 deadline=0;clock_interval_to_deadline(5,kSecondScale,&deadline);bool done=false;
    // 0.180.159: spin 2 ms before sleeping. A TLB invalidate retires in
    // microseconds, but IOSleep(1) on this VM returns after ~10 ms, which was
    // the fixed cost of every IOSurface wrap (11.8 ms for a 64 KiB surface).
    UInt64 spin=0;clock_interval_to_deadline(2,kMillisecondScale,&spin);
    do {
        if(!read(m.ring+0xe00,1))return false;
        if(s->readWords[0]==fence) {done=true;break;}
        if(mach_absolute_time()<spin)IODelay(5); else IOSleep(1);
    } while(mach_absolute_time()<deadline);
    done=done && read(m.userd+0x88,2) && s->readWords[0]==put && s->readWords[1]==put;
    lk(__LINE__);
    m.put=put;m.fence=fence&0xfff;if(done)++m.invalidates;else m.live=false;
    setProperty("NVGspControl-mmu615-invalidates",m.invalidates,32);
    setProperty("NVGspControl-mmu615-live",m.live);ulk();
    return done;
}

// 0.180.131: zero VRAM [phys, phys + bytes) with the maintenance channel's
// copy engine (physical LOCAL_FB destination, constant remap), so a new VRAM
// allocation never shows another process's old data. bytes: 64 KiB multiple.
bool NVGspControl::mmuZero615(UInt64 phys, UInt64 bytes) {
    auto *s=native615_;if(!s || !s->mmu.live || !bytes || (bytes&0xffff) || (phys&0xffff))return false;
    auto &m=s->mmu;
    auto read=[&](UInt64 at,UInt32 count) {return vramAccess(at,s->readWords,count,false,true)==kIOReturnSuccess;};
    auto write=[&](UInt64 at,const UInt32 *words,UInt32 count) {
        if(!count || count>1024)return false;
        if(words!=s->writeWords)__builtin_memcpy(s->writeWords,words,count*4);
        return vramAccess(at,s->writeWords,count,true,true)==kIOReturnSuccess && read(at,count) &&
            !__builtin_memcmp(s->writeWords,s->readWords,count*4);
    };
    auto ce=[](UInt32 method,UInt32 n) {return 0x20000000U|(n<<16)|(4U<<13)|(method>>2);};
    const UInt32 fence=0x6150d000|((m.fence+1)&0xfff);
    const UInt64 semaphore=m.va+0xe00,pb=m.va+0x800;
    UInt32 w[64];UInt32 c=0;
    w[c++]=ce(0x000,1);w[c++]=0xC7B5;                       // SET_OBJECT AMPERE_DMA_COPY_B
    w[c++]=ce(0x700,1);w[c++]=0;                            // REMAP_CONST_A
    w[c++]=ce(0x708,1);w[c++]=0x4444U|(3U<<16);             // all components CONST_A, 4 bytes
    w[c++]=ce(0x264,1);w[c++]=0;                            // DST_PHYS_MODE LOCAL_FB
    w[c++]=ce(0x410,2);w[c++]=0x20000;w[c++]=0x20000;       // pitch 128 KiB
    UInt64 at=phys,left=bytes;
    while(left) {
        if(c+11+6>64)return false;                          // a few launches cover 512 MiB
        const UInt64 line=left>=0x20000 ? 0x20000 : left;
        UInt64 lines=line==0x20000 ? left/0x20000 : 1;if(lines>32768)lines=32768;
        w[c++]=ce(0x408,2);w[c++]=UInt32(at>>32);w[c++]=UInt32(at);
        w[c++]=ce(0x418,2);w[c++]=UInt32(line/4);w[c++]=UInt32(lines);
        w[c++]=ce(0x300,1);w[c++]=0x002U|0x004U|0x080U|0x100U|0x400U|0x2000U|(lines>1?0x200U:0);
        at+=line*lines;left-=line*lines;
    }
    const UInt32 tail[6]={0x20050017,UInt32(semaphore)&0xfffffffc,UInt32(semaphore>>32)&0xff,fence,0,0x00100001};
    for(UInt32 i=0;i<6;++i)w[c++]=tail[i];
    const UInt32 entry[2]={UInt32(pb)&0xfffffffc,(UInt32(pb>>32)&0xff)|(c<<10)};
    const UInt32 put=(m.put+1)%256;
    if(!write(m.ring+0x800,w,c) || !write(m.ring+UInt64(m.put)*8,entry,2) || !write(m.userd+0x8c,&put,1) ||
       diagnosticChannelKick(0xc0d00556,true)!=kIOReturnSuccess)return false;
    UInt64 deadline=0;clock_interval_to_deadline(5,kSecondScale,&deadline);bool done=false;
    do {
        if(!read(m.ring+0xe00,1))return false;
        if(s->readWords[0]==fence) {done=true;break;}
        IODelay(20);
    } while(mach_absolute_time()<deadline);
    done=done && read(m.userd+0x88,2) && s->readWords[0]==put && s->readWords[1]==put;
    lk(__LINE__);m.put=put;m.fence=fence&0xfff;if(!done)m.live=false;
    setProperty("NVGspControl-mmu615-zero-bytes",bytes,64);ulk();
    if(!done)return false;
    // spot check: the first and last words read back zero
    const UInt64 ends[2]={phys,phys+bytes-64};
    for(UInt64 e:ends) {
        if(!read(e,16))return false;
        for(UInt32 i=0;i<16;++i)if(s->readWords[i])return false;
    }
    return true;
}

// 0.180.195: CE copy between two maintenance-VAS addresses (virtual on both sides, plain byte copy).
// Caller holds native615Busy_ with lock_ released. bytes: 4 KiB multiple.
bool NVGspControl::mmuCopyVa615(UInt64 src, UInt64 dst, UInt64 bytes) {
    auto *s=native615_;if(!s || !s->mmu.live || !bytes || (bytes&4095) || bytes>0xffffffffULL)return false;
    auto &m=s->mmu;
    auto read=[&](UInt64 at,UInt32 count) {return vramAccess(at,s->readWords,count,false,true)==kIOReturnSuccess;};
    auto write=[&](UInt64 at,const UInt32 *words,UInt32 count) {
        if(!count || count>1024)return false;
        if(words!=s->writeWords)__builtin_memcpy(s->writeWords,words,count*4);
        return vramAccess(at,s->writeWords,count,true,true)==kIOReturnSuccess && read(at,count) &&
            !__builtin_memcmp(s->writeWords,s->readWords,count*4);
    };
    auto ce=[](UInt32 method,UInt32 n) {return 0x20000000U|(n<<16)|(4U<<13)|(method>>2);};
    const UInt32 fence=0x6150d000|((m.fence+1)&0xfff);
    const UInt64 semaphore=m.va+0xe00,pb=m.va+0x800;
    UInt32 w[24];UInt32 c=0;
    w[c++]=ce(0x000,1);w[c++]=0xC7B5;                       // SET_OBJECT AMPERE_DMA_COPY_B
    w[c++]=ce(0x400,4);w[c++]=UInt32(src>>32);w[c++]=UInt32(src);w[c++]=UInt32(dst>>32);w[c++]=UInt32(dst);
    w[c++]=ce(0x418,2);w[c++]=UInt32(bytes);w[c++]=1;       // one line of `bytes`
    w[c++]=ce(0x300,1);w[c++]=0x002U|0x004U|0x080U|0x100U;  // non-pipelined, flush, pitch, virtual both sides
    const UInt32 tail[6]={0x20050017,UInt32(semaphore)&0xfffffffc,UInt32(semaphore>>32)&0xff,fence,0,0x00100001};
    for(UInt32 i=0;i<6;++i)w[c++]=tail[i];
    const UInt32 entry[2]={UInt32(pb)&0xfffffffc,(UInt32(pb>>32)&0xff)|(c<<10)};
    const UInt32 put=(m.put+1)%256;
    if(!write(m.ring+0x800,w,c) || !write(m.ring+UInt64(m.put)*8,entry,2) || !write(m.userd+0x8c,&put,1) ||
       diagnosticChannelKick(0xc0d00556,true)!=kIOReturnSuccess)return false;
    UInt64 deadline=0;clock_interval_to_deadline(5,kSecondScale,&deadline);bool done=false;
    do {
        if(!read(m.ring+0xe00,1))return false;
        if(s->readWords[0]==fence) {done=true;break;}
        IODelay(10);
    } while(mach_absolute_time()<deadline);
    done=done && read(m.userd+0x88,2) && s->readWords[0]==put && s->readWords[1]==put;
    lk(__LINE__);m.put=put;m.fence=fence&0xfff;if(!done)m.live=false;ulk();
    return done;
}

// 0.180.195: WDDM-like eviction. A VRAM allocation that finds no free range moves the largest VRAM
// slabs of the idle client (no GR/NVDEC submission for 1 s) holding the most VRAM to L2-cacheable sysmem: same
// GPU VA, same PTE kind, so the owner's resources and shaders keep working. Caller holds lock_
// (released and re-taken here, native615Busy_ held meanwhile). Returns the VRAM bytes freed.
// shortcut: demoted slabs never move back to VRAM (no promotion); add it when a benchmark shows the cost.
UInt64 NVGspControl::vramDemote615(UInt32 requester, UInt64 need) {
    auto *s=native615_;if(!s || !s->mmu.live || s->mmu.tree.tables!=1)return 0;
    UInt64 freed=0,idle=0;nanoseconds_to_absolutetime(1000000000ULL,&idle);
    for(UInt32 round=0;round<16 && freed<need;++round) {
        const UInt64 now=mach_absolute_time();
        int victim=-1;UInt32 vslot=0;UInt64 most=0;
        for(UInt32 k=0;k<nvgsp::kClientSlots615;++k) {
            auto &g=s->clientGrs[k];
            if(k==requester || !client615Owners_[k] || !g.live || !g.external || g.nvdecFaulted ||
               now-lastSubmit615_[k]<idle)continue;
            UInt32 best=nvgsp::kClientAllocs615;
            for(UInt32 j=0;j<nvgsp::kClientAllocs615;++j)
                if((g.allocLive>>j&1) && g.allocChunks[j] && nativeVram_[k][j].bytes &&
                   (best==nvgsp::kClientAllocs615 || nativeVram_[k][j].bytes>nativeVram_[k][best].bytes))best=j;
            if(best==nvgsp::kClientAllocs615)continue;
            UInt64 total=0;
            for(UInt32 j=0;j<nvgsp::kClientAllocs615;++j)if(g.allocLive>>j&1)total+=nativeVram_[k][j].bytes;
            if(total>most) {most=total;victim=int(k);vslot=best;}
        }
        if(victim<0)break;
        native615Busy_=true;ulk();
        const UInt64 got=moveSlot615(UInt32(victim),vslot,0);
        lk(__LINE__);native615Busy_=false;
        if(!got)break;
        freed+=got;
    }
    return freed;
}

// Move one slab between VRAM and sysmem. toPhys == 0: VRAM -> new sysmem pages (demote); otherwise
// sysmem -> the VRAM range at toPhys, already inserted in vramHeap_ (promote). The data goes through
// two 128-page windows of the maintenance VAS leaf (source and destination mapped with the slab's own
// kind), then the owner's PTEs are pointed at the new pages: same GPU VA. native615Busy_ held, lock_ not.
UInt64 NVGspControl::moveSlot615(UInt32 k, UInt32 j, UInt64 toPhys) {
    auto *s=native615_;auto &g=s->clientGrs[k];auto &m=s->mmu;
    const bool promote=toPhys!=0;
    lk(__LINE__);const NativeVram615 v=nativeVram_[k][j];const UInt32 first=g.allocChunk[j],chunks=g.allocChunks[j];
    const UInt64 bytes=promote ? g.allocBytes[j] : v.bytes;ulk();
    auto &buffer=nativeAllocDatas_[k][j];
    if(promote ? (v.bytes || buffer.size()!=bytes) : !v.bytes)return 0;
    if(!bytes || (bytes&4095) || !chunks || first+chunks>g.leafChunks || !clientWaitIdle615(k))return 0;
    auto read=[&](UInt64 at,UInt32 count) {return vramAccess(at,s->readWords,count,false,true)==kIOReturnSuccess;};
    // Posted BAR writes are cheap, BAR reads are not (a full read-back made eviction run at 400 MB/s):
    // check the first and last PTE of every write.
    // shortcut: a silently corrupted middle PTE goes unnoticed; read back everything if vramAccess ever drops writes.
    auto write=[&](UInt64 at,UInt32 count) {
        if(count<2 || count>1024 || (count&1) || vramAccess(at,s->writeWords,count,true,true)!=kIOReturnSuccess)return false;
        const UInt32 a0=s->writeWords[0],a1=s->writeWords[1],z0=s->writeWords[count-2],z1=s->writeWords[count-1];
        return read(at,2) && s->readWords[0]==a0 && s->readWords[1]==a1 &&
            read(at+UInt64(count-2)*4,2) && s->readWords[0]==z0 && s->readWords[1]==z1;
    };
    // the first PTE must be exactly what op 4 (VRAM) or an earlier demotion (cached sysmem) wrote
    if(!read(g.leafAddress[first],2))return 0;
    const UInt32 kind=s->readWords[1]>>24;
    {
        UInt64 bus0=0,pte0=0;
        const bool built=promote ? buffer.busPage(0,&bus0) && nvgsp::sysPte615(bus0,false,&pte0,kind,true)
                                 : nvgsp::vidPte615(v.phys,false,false,&pte0,kind);
        if(!built || s->readWords[0]!=UInt32(pte0) || s->readWords[1]!=UInt32(pte0>>32))return 0;
    }
    if(!promote && (buffer.size() || !buffer.allocate(bytes,false,nullptr,1) || !buffer.syncToDevice())) {buffer.release();return 0;}
    const UInt64 fromPhys=promote ? 0 : v.phys,newPhys=promote ? toPhys : 0;
    // the new home of page p: VRAM (promote) or a sysmem page (demote). cachedSys: L2-cacheable sysmem PTE
    auto homePte=[&](UInt64 p,bool cachedSys,UInt64 *pte) {
        UInt64 bus=0;
        return promote ? nvgsp::vidPte615(newPhys+p*4096,false,false,pte,kind)
                       : buffer.busPage(size_t(p),&bus) && nvgsp::sysPte615(bus,false,pte,kind,cachedSys);
    };
    auto oldPte=[&](UInt64 p,UInt64 *pte) {   // promote: cached, so the CE sees the owner's dirty L2 lines
        UInt64 bus=0;
        return promote ? buffer.busPage(size_t(p),&bus) && nvgsp::sysPte615(bus,false,pte,kind,true)
                       : nvgsp::vidPte615(fromPhys+p*4096,false,false,pte,kind);
    };
    const UInt32 mi=UInt32((m.va>>12)&511),w0=mi>=256?0:256;   // away from the channel's own page
    const UInt64 window=m.tree.tableBase+UInt64(w0)*8;
    const UInt64 srcVa=(m.va&~0x1fffffULL)+UInt64(w0)*4096,dstVa=srcVa+128*4096;
    const UInt64 pages=bytes/4096;
    bool ok=true;
    for(UInt64 p=0;ok && p<pages;) {
        const UInt32 n=pages-p>128 ? 128 : UInt32(pages-p);
        bzero(s->writeWords,256*8);
        for(UInt32 i=0;ok && i<n;++i) {
            UInt64 src=0,dst=0;
            ok=oldPte(p+i,&src) && homePte(p+i,false,&dst);   // demote: VOL, the copy lands in memory
            s->writeWords[i*2]=UInt32(src);s->writeWords[i*2+1]=UInt32(src>>32);
            s->writeWords[256+i*2]=UInt32(dst);s->writeWords[256+i*2+1]=UInt32(dst>>32);
        }
        ok=ok && write(window,512) && mmuSubmit615(0x1) && mmuCopyVa615(srcVa,dstVa,UInt64(n)*4096);
        p+=n;
    }
    if(ok && !promote && !kind) {   // pitch kind: the physical VRAM bytes must equal the copy
        auto *cpu=static_cast<const UInt32 *>(buffer.bytes());
        const UInt64 ends[2]={0,bytes-64};
        for(UInt64 e:ends)ok=ok && cpu && read(v.phys+e,16) && !__builtin_memcmp(s->readWords,cpu+e/4,64);
    }
    bzero(s->writeWords,256*8);
    if(!write(window,512) || !mmuSubmit615(0x1))ok=false;
    if(!ok) {if(!promote)buffer.release();return 0;}   // the owner's PTEs are untouched
    UInt64 p=0;
    for(UInt32 c=0;c<chunks && ok;++c) {
        const UInt64 leaf=g.leafAddress[first+c];const UInt32 count=g.leafCount[first+c];
        for(UInt32 i=0;ok && i<count;++i) {
            UInt64 pte=0;
            ok=homePte(p+i,true,&pte);
            s->writeWords[i*2]=UInt32(pte);s->writeWords[i*2+1]=UInt32(pte>>32);
        }
        ok=ok && write(leaf,count*2);
        p+=count;
    }
    ok=ok && p==pages && mmuSubmit615(0x1);
    lk(__LINE__);
    if(!ok) {   // a half-rewritten tree: fail closed like a failed allocation
        native615Ready_=false;native615Status_=UInt32(kIOReturnIOError);
        setProperty("NVGspControl-native615-ready",false);
        setProperty("NVGspControl-native615-status",native615Status_,32);
        ulk();return 0;
    }
    if(promote) {
        nativeVram_[k][j]={toPhys,bytes};demotedMask_[k]&=~(1ULL<<j);
        vramPromotedBytes_+=bytes;
        setProperty("NVGspControl-vram-promoted-bytes",vramPromotedBytes_,64);
    } else {
        g.sysCached=true;   // its submissions now carry the L2 prologue (the cached PTEs above)
        vramHeap_.remove(v.phys);nativeVram_[k][j]={0,0};demotedMask_[k]|=1ULL<<j;
        vramDemotedBytes_+=bytes;++vramDemotions_;lastDemote615_=mach_absolute_time();
        setProperty("NVGspControl-vram-demoted-bytes",vramDemotedBytes_,64);
        setProperty("NVGspControl-vram-demotions",vramDemotions_,32);
    }
    ulk();
    if(promote)buffer.release();   // no PTE reaches these pages any more
    return bytes;
}

// 0.180.195: a client with demoted slabs submits while VRAM is free again: bring one slab back.
// Only when no demotion ran for 5 s and 1 GiB stays free after it, so two apps never ping-pong.
// Caller holds native615Busy_ with lock_ released (the submit path).
void NVGspControl::promoteOne615(UInt32 k) {
    UInt64 calm=0;nanoseconds_to_absolutetime(5000000000ULL,&calm);
    lk(__LINE__);
    auto *s=native615_;auto &g=s->clientGrs[k];
    UInt64 start=0,end=0,at=0,bytes=0;UInt32 j=0;
    for(;j<nvgsp::kClientAllocs615;++j)if((demotedMask_[k]>>j&1) && (g.allocLive>>j&1))break;
    bool go=j<nvgsp::kClientAllocs615 && mach_absolute_time()-lastDemote615_>=calm &&
        nvgsp::diagnosticVramBounds615(gmcConfig_,&start,&end);
    if(go) {
        bytes=(g.allocBytes[j]+0x1fffff)&~0x1fffffULL;
        go=g.allocBytes[j]==bytes && vramHeap_.fit(start,end,bytes+(1ULL<<30),&at,0x200000) &&
           vramHeap_.fit(start,end,bytes,&at,0x200000) && vramHeap_.insert(at,bytes);
    }
    if(j<nvgsp::kClientAllocs615 && !(g.allocLive>>j&1))demotedMask_[k]&=~(1ULL<<j);
    ulk();
    if(!go)return;
    if(!moveSlot615(k,j,at)) {lk(__LINE__);vramHeap_.remove(at);ulk();}
}

void NVGspControl::releaseVram615(UInt32 slot) {
    if(slot>=nvgsp::kClientSlots615)return;
    lk(__LINE__);
    for(auto &v:nativeVram_[slot])if(v.bytes) {vramHeap_.remove(v.phys);v={0,0};}
    demotedMask_[slot]=0;
    ulk();
}

// 0.180.101: owner-only caller allocations on a live caller-buffer client.
// Pages are pinned sysmem with SYSTEM_COHERENT+VOL PTEs in the client's own
// tree; free clears the PTEs and invalidates before the pages go away.
IOReturn NVGspControl::nativeClientMemory615(const void *owner,UInt32 operation,UInt64 argument,UInt64 out[3],
                                            task_t task,UInt64 argument2) {
    if(!lock_ || !owner || !out || operation>4)return kIOReturnBadArgument;
    out[0]=out[1]=out[2]=0;
    if(operation==2 || operation==3)return nativeClientWrap615(owner,operation,argument,out,task,argument2);
    const bool vram=operation==4;   // 0.180.131: VRAM backing, no CPU mapping
    if(vram)operation=0;
    lk(__LINE__);
    auto *s=native615_;
    const int owned=s?ownedSlot615(owner):-1;
    if(!s || owned<0) {ulk();return s && anyOwned615()?kIOReturnNotPermitted:kIOReturnNotFound;}
    // 0.180.112: wait for another client's operation, then work on ours
    if(!waitIdle615() || client615Owners_[owned]!=owner || !selectClient615(UInt32(owned))) {ulk();return kIOReturnBusy;}
    auto &g=s->clientGrs[client615Slot_];
    if(!g.live || !g.userBuffer || !g.external || !s->mmu.live || !native615Ready_ || native615Busy_ ||
       native615Stopping_ || sleeping_ || resetBusy_ || gmcMmuFaultPending_ || postInitPhase_!=27 ||
       s->generation!=resetGeneration()) {ulk();return kIOReturnNotReady;}
    UInt32 slot=0,kind=0;UInt64 bytes=0,va=0;bool cached=false;   // 0.180.177
    if(operation==0) {
        // 0.180.111: argument = bytes | PTE kind << 56
        kind=UInt32(argument>>56);argument&=(1ULL<<56)-1;
        cached=kind&0x10;kind&=~0x10U;   // 0.180.177: L2-cacheable sysmem
        bytes=(argument+4095)&~4095ULL;
        if(kind>7) {ulk();return kIOReturnBadArgument;}
        if(!argument || bytes>nvgsp::kClientAllocMax615) {ulk();return kIOReturnBadArgument;}
        while(slot<nvgsp::kClientAllocs615 && (g.allocLive>>slot&1))++slot;
        // 0.180.193: first fit over the live slots (and the NVDEC ring page), 64 KiB aligned; freed
        // ranges are reused, so a process that keeps opening and closing decoders never runs out
        const UInt64 rounded=vram ? (bytes+0x1fffff)&~0x1fffffULL : (bytes+0xffff)&~0xffffULL;   // VRAM: 2 MiB below
        va=(g.userVa+nvgsp::kClientUserBytes615+0xffff)&~0xffffULL;
        for(bool moved=true;moved && va+rounded<=g.userVa+nvgsp::kClientUserWindow615;) {
            moved=false;
            for(UInt32 k=0;k<nvgsp::kClientAllocs615;++k)
                if((g.allocLive>>k&1) && va<g.allocVa[k]+g.allocBytes[k] && g.allocVa[k]<va+rounded) {
                    va=(g.allocVa[k]+g.allocBytes[k]+0xffff)&~0xffffULL;moved=true;
                }
            if(g.nvdecRingVa && va<g.nvdecRingVa+0x10000 && g.nvdecRingVa<va+rounded) {va=g.nvdecRingVa+0x10000;moved=true;}
        }
        if(slot==nvgsp::kClientAllocs615 || g.leafChunks+UInt32(bytes>>21)+2>nvgsp::kClientLeafChunks615 ||
           va+bytes>g.userVa+nvgsp::kClientUserWindow615) {ulk();return kIOReturnNoSpace;}
        if(vram) {
            // 2 MiB granular from the free FB range our own allocator manages
            bytes=(bytes+0x1fffff)&~0x1fffffULL;
            UInt64 start=0,end=0,at=0;
            if(va+bytes>g.userVa+nvgsp::kClientUserWindow615 || nativeVram_[client615Slot_][slot].bytes ||
               !nvgsp::diagnosticVramBounds615(gmcConfig_,&start,&end) ||
               (!vramHeap_.fit(start,end,bytes,&at,0x200000) &&
                (!vramDemote615(client615Slot_,bytes) || !vramHeap_.fit(start,end,bytes,&at,0x200000)))) {
                ulk();return kIOReturnNoSpace;
            }
            at=(at+0x1fffff)&~0x1fffffULL;
            if(!vramHeap_.insert(at,bytes)) {ulk();return kIOReturnNoMemory;}
            nativeVram_[client615Slot_][slot]={at,bytes};
        }
    } else {
        slot=UInt32(argument);
        if(slot>=nvgsp::kClientAllocs615 || !(g.allocLive>>slot&1)) {ulk();return kIOReturnBadArgument;}
    }
    native615Busy_=true;setProperty("NVGspControl-native615-busy",true);ulk();
    // 0.180.133: no PTE goes away under queued async work
    if(operation==1 && !clientWaitIdle615(client615Slot_)) {
        lk(__LINE__);native615Busy_=false;setProperty("NVGspControl-native615-busy",false);ulk();return kIOReturnBusy;
    }
    auto read=[&](UInt64 at,UInt32 count) {return vramAccess(at,s->readWords,count,false,true)==kIOReturnSuccess;};
    auto write=[&](UInt64 at,const UInt32 *words,UInt32 count) {
        if(!count || count>1024)return false;
        if(words!=s->writeWords)__builtin_memcpy(s->writeWords,words,count*4);
        return vramAccess(at,s->writeWords,count,true,true)==kIOReturnSuccess && read(at,count) &&
            !__builtin_memcmp(s->writeWords,s->readWords,count*4);
    };
    auto &buffer=nativeAllocDatas_[client615Slot_][slot];
    IOReturn r=kIOReturnSuccess;
    if(operation==0) {
        const UInt64 vphys=vram ? nativeVram_[client615Slot_][slot].phys : 0;
        g.allocChunks[slot]=0;
        r=[&]() -> IOReturn {
            if(vram ? !mmuZero615(vphys,bytes) :
               (buffer.size() || !buffer.allocate(bytes,false,nullptr,1) || !buffer.syncToDevice()))return kIOReturnNoMemory;
            g.allocChunk[slot]=g.leafChunks;g.allocChunks[slot]=0;
            for(UInt64 done=0;done<bytes;) {
                const UInt64 at=va+done;UInt64 leaf=0;
                if(!nvgsp::externalTreeLeaf615(g.tree,at,&leaf) || g.leafChunks>=nvgsp::kClientLeafChunks615)return kIOReturnBadArgument;
                UInt32 count=UInt32((bytes-done)/4096);const UInt32 room=512-UInt32((at>>12)&511);
                if(count>room)count=room;
                if(!read(leaf,count*2))return kIOReturnIOError;
                for(UInt32 k=0;k<count*2;++k)if(s->readWords[k])return kIOReturnBusy;
                for(UInt32 k=0;k<count;++k) {
                    UInt64 bus=0,value=0;
                    if(vram ? !nvgsp::vidPte615(vphys+done+UInt64(k)*4096,false,false,&value,kind) :
                       (!buffer.busPage(UInt32(done/4096)+k,&bus) || !nvgsp::sysPte615(bus,false,&value,kind,cached)))return kIOReturnIOError;
                    if(cached && !vram)g.sysCached=true;
                    s->writeWords[k*2]=UInt32(value);s->writeWords[k*2+1]=UInt32(value>>32);
                }
                g.leafAddress[g.leafChunks]=leaf;g.leafCount[g.leafChunks++]=count;++g.allocChunks[slot];
                if(!write(leaf,s->writeWords,count*2))return kIOReturnIOError;
                g.mappedPages+=count;done+=UInt64(count)*4096;
            }
            return mmuSubmit615(0x1)?kIOReturnSuccess:kIOReturnTimeout;
        }();
        if(r==kIOReturnSuccess) {
            g.allocVa[slot]=va;g.allocBytes[slot]=bytes;g.allocLive|=1ULL<<slot;
            if(va+bytes>g.userNextVa)g.userNextVa=va+bytes;   // high water: bump users (NVDEC ring) stay clear
            out[0]=slot;out[1]=va;out[2]=bytes;
        } else if(vram && !g.allocChunks[slot]) {   // nothing mapped: the range goes back
            lk(__LINE__);vramHeap_.remove(vphys);nativeVram_[client615Slot_][slot]={0,0};ulk();
        }
    } else {
        r=[&]() -> IOReturn {
            for(UInt32 c=0;c<g.allocChunks[slot];++c) {
                const UInt32 chunk=g.allocChunk[slot]+c;
                bzero(s->writeWords,g.leafCount[chunk]*8);
                if(!write(g.leafAddress[chunk],s->writeWords,g.leafCount[chunk]*2))return kIOReturnIOError;
            }
            return mmuSubmit615(0x1)?kIOReturnSuccess:kIOReturnTimeout;
        }();
        if(r==kIOReturnSuccess) {
            // No translation reaches these pages now; a CPU mapping keeps them alive.
            buffer.release();g.allocLive&=~(1ULL<<slot);
            // 0.180.193: drop the slot's (now zero) leaf records so the next allocation can use them
            const UInt32 first=g.allocChunk[slot],n=g.allocChunks[slot];
            if(n && first+n<=g.leafChunks) {
                for(UInt32 i=first;i+n<g.leafChunks;++i) {g.leafAddress[i]=g.leafAddress[i+n];g.leafCount[i]=g.leafCount[i+n];}
                g.leafChunks-=n;
                for(UInt32 k=0;k<nvgsp::kClientAllocs615;++k)if((g.allocLive>>k&1) && g.allocChunk[k]>first)g.allocChunk[k]-=n;
            }
            g.allocChunks[slot]=0;demotedMask_[client615Slot_]&=~(1ULL<<slot);
            auto &v=nativeVram_[client615Slot_][slot];   // 0.180.131
            if(v.bytes) {lk(__LINE__);vramHeap_.remove(v.phys);v={0,0};ulk();}
            out[0]=slot;out[1]=g.allocVa[slot];out[2]=g.allocBytes[slot];
        }
    }
    lk(__LINE__);
    if(r!=kIOReturnSuccess) {   // uncertain PTE/TLB state: fail closed
        native615Ready_=false;native615Status_=static_cast<UInt32>(r);
        setProperty("NVGspControl-native615-ready",false);
        setProperty("NVGspControl-native615-status",native615Status_,32);
    }
    native615Busy_=false;
    setProperty("NVGspControl-native615-busy",false);
    setProperty("NVGspControl-client615-alloc-live",g.allocLive,64);
    setProperty("NVGspControl-client615-leaf-chunks",g.leafChunks,32);
    setProperty("NVGspControl-client615-alloc-status",UInt32(r),32);
    ulk();return r;
}

// 0.180.117: map caller pages (IOSurfaces, NoCopy buffers) into the owner's
// wrap window. The pages stay wired until the owner's connection closes or
// the wrap is removed after its PTEs are cleared and the TLB invalidated.
IOReturn NVGspControl::nativeClientWrap615(const void *owner,UInt32 operation,UInt64 argument,UInt64 out[3],
                                          task_t task,UInt64 argument2) {
    IOMemoryDescriptor *md=nullptr;UInt64 *phys=nullptr;UInt32 pages=0,kind=0;UInt64 bytes=0;bool wrapCached=false;
    // 0.180.161: phase timing of the last wrap/unwrap (us): prepare, idle wait,
    // PTE writes, TLB invalidate, total -> NVGspControl-client615-wrap-us
    UInt64 tPhase[5]={mach_absolute_time(),0,0,0,0};
    if(operation==2) {
        kind=UInt32(argument2>>56);bytes=argument2&((1ULL<<56)-1);
        wrapCached=kind&0x10;kind&=~0x10U;   // 0.180.177: L2-cacheable sysmem
        // x86_64 user space ends below the kernel half (same rule as userMemBind)
        if(!task || !bytes || ((argument|bytes)&0xfff) || bytes>nvgsp::kClientWrapMax615 || kind>7 ||
           argument>UINT64_MAX-bytes || argument+bytes>0x0000800000000000ULL)return kIOReturnBadArgument;
        md=IOMemoryDescriptor::withAddressRange(argument,bytes,kIODirectionInOut,task);
        if(!md)return kIOReturnNoMemory;
        if(md->prepare()!=kIOReturnSuccess) {md->release();return kIOReturnVMError;}
        pages=UInt32(bytes>>12);
        phys=static_cast<UInt64 *>(IOMalloc(pages*sizeof(UInt64)));
        bool ok=phys!=nullptr;
        for(UInt32 k=0;ok && k<pages;++k) {
            IOByteCount len=0;
            phys[k]=md->getPhysicalSegment(UInt64(k)<<12,&len,kIOMemoryMapperNone);
            ok=phys[k] && !(phys[k]&0xfff);
        }
        if(!ok) {
            if(phys)IOFree(phys,pages*sizeof(UInt64));
            md->complete();md->release();return kIOReturnVMError;
        }
    }
    auto drop=[&](IOReturn r) {
        if(phys)IOFree(phys,pages*sizeof(UInt64));
        if(md) {md->complete();md->release();}
        return r;
    };
    tPhase[1]=mach_absolute_time();
    lk(__LINE__);
    auto *s=native615_;
    const int owned=s?ownedSlot615(owner):-1;
    if(!s || owned<0) {ulk();return drop(s && anyOwned615()?kIOReturnNotPermitted:kIOReturnNotFound);}
    if(!waitIdle615() || client615Owners_[owned]!=owner || !selectClient615(UInt32(owned))) {ulk();return drop(kIOReturnBusy);}
    tPhase[2]=mach_absolute_time();
    auto &g=s->clientGrs[client615Slot_];
    if(!g.live || !g.userBuffer || !g.external || !s->mmu.live || !native615Ready_ || native615Busy_ ||
       native615Stopping_ || sleeping_ || resetBusy_ || gmcMmuFaultPending_ || postInitPhase_!=27 ||
       s->generation!=resetGeneration()) {ulk();return drop(kIOReturnNotReady);}
    const UInt64 base=g.userVa+nvgsp::kClientUserWindow615,end=base+nvgsp::kClientWrapWindow615;
    UInt32 index=0;UInt64 va=0;
    if(operation==2) {
        // an entry kept wired by a failed wrap is never reused
        while(index<nvgsp::kClientWraps615 && (nvgsp::clientWrapLive615(g,index) || nativeWraps_[client615Slot_][index]))++index;
        if(index==nvgsp::kClientWraps615) {ulk();return drop(kIOReturnNoSpace);}
        // first fit, 64 KiB aligned
        va=base;
        for(bool moved=true;moved && va+bytes<=end;) {
            moved=false;
            for(UInt32 k=0;k<nvgsp::kClientWraps615;++k)
                if(nvgsp::clientWrapLive615(g,k) && va<g.wrapVa[k]+g.wrapBytes[k] && g.wrapVa[k]<va+bytes) {
                    va=(g.wrapVa[k]+g.wrapBytes[k]+0xffff)&~0xffffULL;moved=true;
                }
        }
        if(va+bytes>end) {ulk();return drop(kIOReturnNoSpace);}
    } else {
        index=UInt32(argument);
        if(index>=nvgsp::kClientWraps615 || !nvgsp::clientWrapLive615(g,index) || !nativeWraps_[client615Slot_][index]) {
            ulk();return kIOReturnBadArgument;
        }
        va=g.wrapVa[index];bytes=g.wrapBytes[index];pages=UInt32(bytes>>12);
    }
    native615Busy_=true;setProperty("NVGspControl-native615-busy",true);ulk();
    if(operation==3 && !clientWaitIdle615(client615Slot_)) {   // 0.180.133: unwrap after queued work
        lk(__LINE__);native615Busy_=false;setProperty("NVGspControl-native615-busy",false);ulk();return drop(kIOReturnBusy);
    }
    auto read=[&](UInt64 at,UInt32 count) {return vramAccess(at,s->readWords,count,false,true)==kIOReturnSuccess;};
    auto write=[&](UInt64 at,const UInt32 *words,UInt32 count) {
        if(!count || count>1024)return false;
        if(words!=s->writeWords)__builtin_memcpy(s->writeWords,words,count*4);
        if(vramAccess(at,s->writeWords,count,true,true)!=kIOReturnSuccess)return false;
        // 0.180.186: spot-check both ends instead of reading the whole run back. A read returns only after
        // the posted writes before it landed (PCIe ordering), and a misplaced PRAMIN window fails at either
        // end. The full read-back (plus the full zero check) was ~2048 MMIO reads per 512 PTEs: 34 ms to
        // wrap a 33 MB surface, 10.8 ms to unwrap one (Mission Control dropped frames on them).
        return read(at,2) && s->readWords[0]==s->writeWords[0] && s->readWords[1]==s->writeWords[1] &&
            read(at+UInt64(count-2)*4,2) && s->readWords[0]==s->writeWords[count-2] &&
            s->readWords[1]==s->writeWords[count-1];
    };
    const IOReturn r=[&]() -> IOReturn {
        for(UInt32 done=0;done<pages;) {
            const UInt64 at=va+UInt64(done)*4096;UInt64 leaf=0;
            if(!nvgsp::externalTreeLeaf615(g.tree,at,&leaf))return kIOReturnBadArgument;
            UInt32 count=pages-done;const UInt32 room=512-UInt32((at>>12)&511);
            if(count>room)count=room;
            if(operation==2) {
                // the VA allocator above never overlaps live wraps; the ends catch a stale or foreign run
                if(!read(leaf,2) || s->readWords[0] || s->readWords[1] ||
                   !read(leaf+UInt64(count-1)*8,2) || s->readWords[0] || s->readWords[1])return kIOReturnBusy;
                for(UInt32 k=0;k<count;++k) {
                    UInt64 value=0;
                    if(!nvgsp::sysPte615(phys[done+k],false,&value,kind,wrapCached))return kIOReturnIOError;
                    if(wrapCached)g.sysCached=true;
                    s->writeWords[k*2]=UInt32(value);s->writeWords[k*2+1]=UInt32(value>>32);
                }
            } else bzero(s->writeWords,count*8);
            if(!write(leaf,s->writeWords,count*2))return kIOReturnIOError;
            done+=count;
        }
        tPhase[3]=mach_absolute_time();
        return mmuSubmit615(0x1)?kIOReturnSuccess:kIOReturnTimeout;
    }();
    tPhase[4]=mach_absolute_time();
    lk(__LINE__);
    IOMemoryDescriptor *unwired=nullptr;
    if(r==kIOReturnSuccess && operation==2) {
        nativeWraps_[client615Slot_][index]=md;md=nullptr;
        g.wrapVa[index]=va;g.wrapBytes[index]=bytes;nvgsp::clientWrapSet615(g,index,true);g.wrapPages+=pages;
        out[0]=index;out[1]=va;out[2]=bytes;
    } else if(r==kIOReturnSuccess) {
        // No translation reaches these pages now.
        unwired=nativeWraps_[client615Slot_][index];nativeWraps_[client615Slot_][index]=nullptr;
        nvgsp::clientWrapSet615(g,index,false);g.wrapPages-=pages;
        out[0]=index;out[1]=va;out[2]=bytes;
    }
    if(r!=kIOReturnSuccess) {   // uncertain PTE/TLB state: fail closed, keep everything wired
        native615Ready_=false;native615Status_=static_cast<UInt32>(r);
        setProperty("NVGspControl-native615-ready",false);
        setProperty("NVGspControl-native615-status",native615Status_,32);
        if(operation==2 && md) {nativeWraps_[client615Slot_][index]=md;md=nullptr;}   // may be partly mapped
    }
    native615Busy_=false;
    setProperty("NVGspControl-native615-busy",false);
    setProperty("NVGspControl-client615-wrap-pages",UInt32(g.wrapPages),32);
    setProperty("NVGspControl-client615-wrap-status",UInt32(r),32);
    {
        UInt32 us[6]={operation,0,0,0,0,0};
        for(UInt32 k=1;k<5;++k) {
            UInt64 ns=0;
            if(tPhase[k] && tPhase[k-1] && tPhase[k]>=tPhase[k-1])absolutetime_to_nanoseconds(tPhase[k]-tPhase[k-1],&ns);
            us[k]=UInt32(ns/1000);
        }
        UInt64 ns=0;if(tPhase[4]>=tPhase[0])absolutetime_to_nanoseconds(tPhase[4]-tPhase[0],&ns);us[5]=UInt32(ns/1000);
        setProperty("NVGspControl-client615-wrap-us",us,sizeof(us));
    }
    ulk();
    if(unwired) {unwired->complete();unwired->release();}
    return drop(r);
}

IOMemoryDescriptor *NVGspControl::client615AllocMemory(const void *owner,UInt32 slot) {
    if(!lock_ || !owner || slot>=nvgsp::kClientAllocs615)return nullptr;
    lk(__LINE__);
    IOMemoryDescriptor *d=nullptr;
    const int k=native615_?ownedSlot615(owner):-1;
    if(k>=0 && native615_->clientGrs[k].live && (native615_->clientGrs[k].allocLive>>slot&1) &&
       nativeAllocDatas_[k][slot].descriptor()) {
        d=nativeAllocDatas_[k][slot].descriptor();d->retain();
    }
    ulk();return d;
}

// 0.180.100/103: the owner's work stalled on REPLAYABLE faults. Cancel on the
// permanent channel, let GSP robust-channel kill only this channel (attributed
// Xid31), then tear the client down. lock_ and native615Busy_ held throughout.
bool NVGspControl::recoverOwnedFault615(UInt32 rcBefore) {
    auto *s=native615_;if(!s)return false;
    auto &g=s->clientGrs[client615Slot_];
    bool recovered=false;
    // 0.180.100: the caller's shader faulted (replayable, stalled). Cancel,
    // let GSP robust-channel kill only this channel, then tear it down.
    ulk();
    IOReturn rec=runClientGr615(4);
    lk(__LINE__);
    if(rec==kIOReturnNoResources && rcEvents_==rcBefore && !gmcMmuFaultPending_) {
        // 0.180.108: the work never finished and raised no fault and no RC.
        // Tear the client down the normal way: the schedule-off and the
        // channel/TSG free make RM preempt it. Re-admit only when GR is idle
        // afterwards and root's ring is intact (checked by the teardown).
        // 0.180.110: this runs with lock_ held (108/109 ran it before the
        // re-lock above and so unlocked a mutex it did not own: guest panic).
        auto gr=[&]() {
            IOMemoryMap *bm=sharedBar0Map(pci_);UInt32 v=~0U;
            if(bm && bm->getLength()>0x400704) {Bar0Io io{bm};if(!io.read(0x400700,&v))v=~0U;}
            if(bm)bm->release();return v;
        };
        const UInt32 before=gr();
        setProperty("NVGspControl-client615-hang-gr-before",before,32);
        // 0.180.189: MetalFX hangs with GR idle and no RC: where does the channel stand?
        // Read-only: runlist/CHRAM[chid], USERD 0x40..0x8c, the GR PBDMA's first 128 registers.
        auto snap=[&](const char *suffix) {
            char key[72]{};size_t pos=0;
            appendStr(key,sizeof(key),&pos,"NVGspControl-client615-hang-runlist");appendStr(key,sizeof(key),&pos,suffix);key[pos]='\0';
            UInt32 rl=0;
            runlistDiagLocked(key,0,0,g.token&0xffff,g.token,&rl,nullptr,nullptr);
            UInt32 u[20];
            for(UInt32 &w : u)w=~0U;
            if(g.userd)praminReadWords(pci_,g.userd+0x40,u,20);
            pos=0;appendStr(key,sizeof(key),&pos,"NVGspControl-client615-hang-userd");appendStr(key,sizeof(key),&pos,suffix);key[pos]='\0';
            setProperty(key,u,sizeof(u));
            UInt32 pb[128];
            for(UInt32 &w : pb)w=~0U;
            IOMemoryMap *bm=sharedBar0Map(pci_);
            if(bm && rl && bm->getLength()>=0x01000000) {
                Bar0Io io{bm};UInt32 cfg=0;
                const UInt32 base=io.read(rl+0x10,&cfg) && (cfg&0x80000000) ? (cfg&0x00fffc00) : 0;
                for(UInt32 i=0;base && i<128;++i)io.read(base+i*4,&pb[i]);
            }
            if(bm)bm->release();
            pos=0;appendStr(key,sizeof(key),&pos,"NVGspControl-client615-hang-pbdma");appendStr(key,sizeof(key),&pos,suffix);key[pos]='\0';
            setProperty(key,pb,sizeof(pb));
        };
        snap("");
        // 0.180.178: rebuild only the channel; the owner keeps its client
        ulk();
        rec=runClientGr615(5);
        lk(__LINE__);
        setProperty("NVGspControl-client615-hang-reset-status",UInt32(rec),32);
        setProperty("NVGspControl-client615-hang-reset-stage",g.stage,32);
        if(rec!=kIOReturnSuccess)snap("-failed");
        if(rec==kIOReturnSuccess && !gmcMmuFaultPending_) {
            ++hangResets615_;
            setProperty("NVGspControl-client615-hang-resets",hangResets615_,32);
            return true;
        }
        if(g.stage==17)return false;   // the old channel is partly freed: pinned
        g.tornDown=true;g.live=false;
        setProperty("NVGspControl-client615-live",false);
        ulk();
        rec=runClientGr615(1);
        lk(__LINE__);
        UInt32 after=gr();
        for(unsigned w=0;w<20 && after;++w) {ulk();IOSleep(50);lk(__LINE__);after=gr();}
        setProperty("NVGspControl-client615-hang-gr-after",after,32);
        setProperty("NVGspControl-client615-hang-teardown-status",UInt32(rec),32);
        setProperty("NVGspControl-client615-hang-rc-delta",rcEvents_-rcBefore,32);
        const bool ok=rec==kIOReturnSuccess && !after && !gmcMmuFaultPending_;
        if(ok) {
            client615Owners_[client615Slot_]=nullptr;bzero(&g,sizeof(g));
            setProperty("NVGspControl-client615-owned",false);
            setProperty("NVGspControl-client615-owned-hang-recovered",true);
        }
        return ok;
    }
    auto number=[&](const char *key) {
        auto *n=OSDynamicCast(OSNumber,getProperty(key));return n ? n->unsigned64BitValue() : ~0ULL;
    };
    // the fault RC (type 31) may follow an earlier RC of the same burst
    auto faultSeen=[&]() {return rcEvents_>rcBefore && gmcMmuFaultPending_ && number("NVGspControl-rc-except-type")==31;};
    UInt32 drains=0;
    while(rec==kIOReturnSuccess && !faultSeen() && drains<40) {
        ++drains;drainNoWait_=true;pollStatusLocked();
        if(faultSeen())break;
        ulk();IOSleep(50);lk(__LINE__);
    }
    // 0.180.183: one fault can raise a burst on the same channel (seen: type
    // 109 CTXSW timeout, then type 31 MMU fault); accept it when every RC since
    // the submission was on this client's channel and the last is the fault.
    const UInt32 rcNew=rcEvents_-rcBefore;
    bool rcOurs=rcNew>=1 && rcNew<=8;
    for(UInt32 i=0;rcOurs && i<rcNew;++i)rcOurs=rcChidLog_[(rcBefore+i)%8]==(g.token&0xffff);
    setProperty("NVGspControl-client615-owned-fault-rc-burst",rcNew,32);
    const bool attributed=rec==kIOReturnSuccess && rcOurs && gmcMmuFaultPending_ &&
        number("NVGspControl-rc-chid")==(g.token&0xffff) && number("NVGspControl-rc-engine")==1 &&
        number("NVGspControl-rc-except-type")==31;
    setProperty("NVGspControl-client615-owned-fault-drains",drains,32);
    setProperty("NVGspControl-client615-owned-fault-attributed",attributed);
    if(attributed) {
        gmcMmuFaultPending_=false;mmuNoticesUnpaired_=0;nvdecRcUnpaired_=0;otherMmuFault_=false;
        setProperty("NVGspControl-gmc-mmu-fault-pending",false);
        // 0.180.183: RC killed only the faulted channel; rebuild it like after
        // a hang (VAS and allocations kept) so the owner's later work runs.
        // The old teardown below lost the whole client.
        ulk();
        rec=runClientGr615(5);
        lk(__LINE__);
        setProperty("NVGspControl-client615-fault-reset-status",UInt32(rec),32);
        setProperty("NVGspControl-client615-fault-reset-stage",g.stage,32);
        if(rec==kIOReturnSuccess && !gmcMmuFaultPending_) {
            g.cancelled=false;++faultResets615_;
            setProperty("NVGspControl-client615-fault-resets",faultResets615_,32);
            return true;
        }
        if(g.stage==17)return false;   // the old channel is partly freed: pinned
        g.tornDown=true;g.live=false;
        setProperty("NVGspControl-client615-live",false);
        ulk();
        rec=runClientGr615(3);
        lk(__LINE__);
        recovered=rec==kIOReturnSuccess;
    }
    setProperty("NVGspControl-client615-owned-fault-recover-status",UInt32(attributed?rec:kIOReturnNotAttached),32);
    if(recovered) {
        // The owner learns its client is gone; root and later clients go on.
        client615Owners_[client615Slot_]=nullptr;bzero(&g,sizeof(g));
        setProperty("NVGspControl-client615-owned",false);
        setProperty("NVGspControl-client615-owned-fault-recovered",true);
    }
    return recovered;
}

// 0.180.133: true once the client's submission fence has reached `fence`
// (submission fences are 0x6150c800 + sequence and only grow)
static inline bool fenceReached615(UInt32 sem,UInt32 fence) {return SInt32(sem-fence)>=0;}

IOReturn NVGspControl::nativeClientSubmit615(const void *owner,UInt64 va,UInt64 words,UInt64 out[3],UInt64 flags) {
    if(!lock_ || !owner || !out || (flags&~3ULL))return kIOReturnBadArgument;
    out[0]=out[1]=out[2]=0;
    // 0.180.164: Metal work raises the clocks (rate-limited to 1/s, 3 s boost)
    if(!(flags&2) && nvramBoostPinned_==0 && boostPinned_==0)noteGpuBusy();
    // 0.180.133: bit 0 = return after the kick (async), bit 1 = only wait
    // until sequence `va` has finished (with the same hang handling)
    const bool async=flags&1,waitOnly=flags&2;
    // GPFIFO entry: VA bits 39:2, LENGTH 30:10 of the second word.
    if(!waitOnly && (!words || words>=(1ULL<<21) || (va&3) || va>=(1ULL<<40)))return kIOReturnBadArgument;
    lk(__LINE__);
    auto *s=native615_;
    const int owned=s?ownedSlot615(owner):-1;
    if(!s || owned<0) {ulk();return s && anyOwned615()?kIOReturnNotPermitted:kIOReturnNotFound;}
    // 0.180.112: wait for another client's operation, then work on ours
    if(!waitIdle615() || client615Owners_[owned]!=owner || !selectClient615(UInt32(owned))) {ulk();return kIOReturnBusy;}
    auto &g=s->clientGrs[client615Slot_];
    if(!g.live || !g.scheduled || !g.userBuffer || !g.fence ||   /* put wraps to 0 */ !native615Ready_ || native615Busy_ ||
       native615Stopping_ || sleeping_ || resetBusy_ || gmcMmuFaultPending_ || userRpcLatePending_ ||
       postInitPhase_!=27 || s->generation!=resetGeneration()) {ulk();return kIOReturnNotReady;}
    // The pushbuffer must lie in the caller window; its contents are the
    // caller's, executed on its unprivileged channel inside its own VAS.
    if(!waitOnly && (va<g.userVa || words*4>nvgsp::kClientUserWindow615 || va+words*4>g.userVa+nvgsp::kClientUserWindow615))
        {ulk();return kIOReturnBadArgument;}
    if(waitOnly && (!va || va>g.dispatches)) {ulk();return kIOReturnBadArgument;}
    native615Busy_=true;const UInt32 rcBefore=rcEvents_;lastSubmit615_[client615Slot_]=mach_absolute_time();
    setProperty("NVGspControl-native615-busy",true);ulk();
    if(demotedMask_[client615Slot_])promoteOne615(client615Slot_);   // 0.180.195: VRAM free again
    // 0.180.177: a sysCached client gets a prologue entry (SET_REFERENCE,
    // MEM_OP_D L2_SYSMEM_INVALIDATE: drop the L2's clean sysmem lines, the CPU
    // may have written them) and SET_REFERENCE before its fence (its sysmem
    // writes are visible when the fence is), the barrier NVK uses pre-Hopper.
    const bool coh=g.sysCached;
    const UInt32 first=g.put,put=(g.put+(coh?3:2))%256;
    const UInt32 sequence=waitOnly ? UInt32(va) : g.dispatches+1;
    const UInt32 hostFence=0x6150c800U+sequence;
    // 0.180.133: one tail slot per sequence in 0x800..0xdff, so a submission
    // queued behind an unfinished one never rewrites its fence. 0.180.177: 32
    // slots of 48 bytes (prologue 3 + barrier 1 + fence 6 words); at most 32
    // submissions are in flight, so a slot is free again when it is reused.
    const UInt64 tailOffset=0x800+UInt64(sequence%32)*48;
    const UInt64 semaphore=g.gpfifoVa+0xe00,tail=g.gpfifoVa+tailOffset;
    UInt32 completed=g.completed;
    auto read=[&](UInt64 at,UInt32 count) {return vramAccess(at,s->readWords,count,false,true)==kIOReturnSuccess;};
    auto write=[&](UInt64 at,const UInt32 *w,UInt32 count) {
        if(!count || count>1024)return false;
        if(w!=s->writeWords)__builtin_memcpy(s->writeWords,w,count*4);
        return vramAccess(at,s->writeWords,count,true,true)==kIOReturnSuccess && read(at,count) &&
            !__builtin_memcmp(s->writeWords,s->readWords,count*4);
    };
    // wait until the fence reaches `target` (5 s, then the hang path)
    auto waitFence=[&](UInt32 target) -> IOReturn {
        UInt64 deadline=0,spin=0;clock_interval_to_deadline(5,kSecondScale,&deadline);
        // 0.180.175: IOSleep(1) costs ~10 ms on this VM (IOKit's own note in
        // NVGspControl.hpp), and one Metal submit is 1-10 ms, so every submit
        // paid a whole sleep quantum of dead time. Spin through the frame
        // budget instead: the submitting thread has nothing else to do while
        // the fence is pending.
        clock_interval_to_deadline(20,kMillisecondScale,&spin);
        do {
            if(!read(g.gpfifo+0xe00,1))return kIOReturnIOError;
            if(fenceReached615(s->readWords[0],target)) {
                const UInt32 seen=s->readWords[0]-0x6150c800U;
                if(SInt32(seen-completed)>0 && SInt32(UInt32(g.dispatches+1)-seen)>=0)completed=seen;
                return kIOReturnSuccess;
            }
            if(mach_absolute_time()<spin)IODelay(5); else IOSleep(1);
        } while(mach_absolute_time()<deadline);
        return kIOReturnTimeout;
    };
    const IOReturn r=[&]() -> IOReturn {
        if(waitOnly)return waitFence(hostFence);
        // at most 32 submissions in flight (64 tail slots, 64 of 256 entries)
        if(SInt32(sequence-completed)>32) {const IOReturn w=waitFence(0x6150c800U+sequence-32);if(w)return w;}
        if(!read(g.userd+0x8c,1) || s->readWords[0]!=g.put)return kIOReturnNotReady;
        if(!async && sequence==completed+1 &&
           (!read(g.gpfifo+0xe00,1) || !fenceReached615(s->readWords[0],g.fence)))return kIOReturnNotReady;
        const UInt32 fenceWords[6]={0x20050017,UInt32(semaphore)&0xfffffffc,UInt32(semaphore>>32)&0xff,hostFence,0,0x00100001};
        UInt32 slotWords[10],nw=0;
        if(coh) {slotWords[nw++]=0x80000014U;slotWords[nw++]=0x2001000dU;slotWords[nw++]=0x70000000U;slotWords[nw++]=0x80000014U;}
        for(UInt32 k=0;k<6;++k)slotWords[nw++]=fenceWords[k];
        const UInt64 fenceVa=coh?tail+12:tail;   // after the prologue: SET_REFERENCE + fence
        const UInt32 e0[2]={UInt32(tail)&0xfffffffc,(UInt32(tail>>32)&0xff)|(3U<<10)};
        const UInt32 e1[2]={UInt32(va)&0xfffffffc,(UInt32(va>>32)&0xff)|(UInt32(words)<<10)};
        const UInt32 e2[2]={UInt32(fenceVa)&0xfffffffc,(UInt32(fenceVa>>32)&0xff)|((coh?7U:6U)<<10)};
        const UInt32 userEntry=coh?(first+1)%256:first,fenceEntry=coh?(first+2)%256:(first+1)%256;
        // 0.180.129: the tail, entries and put go in without a read-back each
        // (the fence check below proves the GPU took them), and the fence is
        // spun on for the first 2 ms before sleeping a millisecond at a time:
        // a small submission cost ~6 ms, nearly all PRAMIN reads and IOSleep.
        auto put32=[&](UInt64 at,const UInt32 *w,UInt32 count) {
            __builtin_memcpy(s->writeWords,w,count*4);
            return vramAccess(at,s->writeWords,count,true,true)==kIOReturnSuccess;
        };
        if(!put32(g.gpfifo+tailOffset,slotWords,nw) || (coh && !put32(g.gpfifo+UInt64(first)*8,e0,2)) ||
           !put32(g.gpfifo+UInt64(userEntry)*8,e1,2) ||
           !put32(g.gpfifo+UInt64(fenceEntry)*8,e2,2) || !write(g.userd+0x8c,&put,1) ||
           diagnosticChannelKick(0xc0d00356U|client615Slot_<<16,true)!=kIOReturnSuccess)return kIOReturnIOError;
        if(async)return kIOReturnSuccess;
        return waitFence(hostFence);
    }();
    lk(__LINE__);
    const bool clean=r==kIOReturnSuccess && rcEvents_==rcBefore && !gmcMmuFaultPending_;
    const bool recovered=!clean && r==kIOReturnTimeout && rcEvents_==rcBefore && nativeReplayData_.size() &&
        recoverOwnedFault615(rcBefore);
    if(clean && waitOnly) {g.completed=completed;out[0]=sequence;out[1]=completed;}
    else if(clean) {
        g.put=put;g.fence=hostFence;g.dispatches=sequence;g.completed=async ? completed : sequence;
        out[0]=sequence;out[1]=g.completed;out[2]=words;
    }
    else if(!recovered) {
        native615Ready_=false;native615Status_=static_cast<UInt32>(r?r:kIOReturnIOError);
        setProperty("NVGspControl-native615-ready",false);
        setProperty("NVGspControl-native615-status",native615Status_,32);
    }
    native615Busy_=false;
    setProperty("NVGspControl-native615-busy",false);
    setProperty("NVGspControl-client615-dispatches",g.dispatches,32);
    setProperty("NVGspControl-client615-submit-status",UInt32(r),32);
    ulk();return clean?kIOReturnSuccess:recovered?kIOReturnAborted:(r?r:kIOReturnIOError);
}


// 0.180.133: wait (5 s) until every submission of `slot` has finished.
// Caller holds native615Busy_ with lock_ released.
bool NVGspControl::clientWaitIdle615(UInt32 slot) {
    auto *s=native615_;if(!s || slot>=nvgsp::kClientSlots615)return false;
    auto &g=s->clientGrs[slot];
    if(!g.fence || !g.scheduled)return true;
    UInt64 deadline=0;clock_interval_to_deadline(5,kSecondScale,&deadline);
    UInt64 spin=0;clock_interval_to_deadline(2,kMillisecondScale,&spin);   // 0.180.159: see mmuSubmit615
    do {
        if(vramAccess(g.gpfifo+0xe00,s->readWords,1,false,true)!=kIOReturnSuccess)return false;
        if(SInt32(s->readWords[0]-g.fence)>=0) {g.completed=g.dispatches;return true;}
        if(mach_absolute_time()<spin)IODelay(5); else IOSleep(1);
    } while(mach_absolute_time()<deadline);
    return false;
}

// 0.180.133: read-only display probe for the paused native runtime: make the
// display common object (class 0x73, handle 0xc0d00073) if needed and ask
// SYSTEM_GET_CONNECT_STATE over the supported mask 0x7f00. out[0] = connected
// mask, out[1] = RM status, out[2] = 1 when the object was made here.
IOReturn NVGspControl::displayProbe615(UInt64 out[3]) {
    if(!lock_ || !out)return kIOReturnBadArgument;
    out[0]=out[1]=out[2]=0;
    lk(__LINE__);
    const bool ready=native615_ && native615Ready_ && !native615Busy_ && postInitPhase_==27 && gmcInitReady_ &&
        !gmcMmuFaultPending_ && !userRpcLatePending_ && !resetBusy_ && !sleeping_;
    if(ready) {native615Busy_=true;setProperty("NVGspControl-native615-busy",true);}
    ulk();
    if(!ready)return kIOReturnNotReady;
    constexpr UInt32 C=nvgsp::kLifecycleClient615,D=nvgsp::kLifecycleDevice615,DISP=0xc0d00073;
    auto *s=native615_;
    IOReturn r=kIOReturnSuccess;
    if(!displayCommon615_) {
        if(!nvgsp::buildAlloc615(s->request,sizeof(s->request),D,DISP,0x73,nullptr,0))r=kIOReturnBadArgument;
        UInt32 bytes=sizeof(s->reply),result=~0U;
        if(!r)r=userRpcImpl(103,s->request,32,s->reply,&bytes,&result,true,true);
        const UInt8 *ret=nullptr;
        if(!r) {
            const auto reply=nvgsp::decodeAlloc615(s->reply,bytes,D,DISP,0x73,0,&ret);
            if(!reply.valid)r=kIOReturnBadMessageID;
            else if(result || !reply.successful())out[1]=result?result:reply.status,r=kIOReturnUnsupported;
            else {displayCommon615_=true;out[2]=1;}
        }
    }
    if(!r) {
        UInt8 params[16]{};const UInt32 mask=0x7f00;__builtin_memcpy(params+8,&mask,4);
        if(!nvgsp::buildControl615(s->request,sizeof(s->request),C,DISP,0x730108,sizeof(params)))r=kIOReturnBadArgument;
        else {
            __builtin_memcpy(s->request+40,params,sizeof(params));
            UInt32 bytes=sizeof(s->reply),result=~0U;
            r=userRpcImpl(76,s->request,40+sizeof(params),s->reply,&bytes,&result,true,true);
            nvgsp::ControlReply615 reply{};
            if(!r && (bytes!=96+sizeof(params) ||
                      !nvgsp::decodeControlReply615(s->reply,bytes,C,DISP,0x730108,sizeof(params),reply)))r=kIOReturnBadMessageID;
            if(!r) {
                out[1]=result?result:reply.header.status;
                UInt32 connected=0;__builtin_memcpy(&connected,reply.params+8,4);
                out[0]=connected;
                if(out[1])r=kIOReturnUnsupported;
            }
        }
    }
    lk(__LINE__);
    native615Busy_=false;setProperty("NVGspControl-native615-busy",false);
    setProperty("NVGspControl-display615-connect-mask",UInt32(out[0]),32);
    setProperty("NVGspControl-display615-status",UInt32(out[1]),32);
    setProperty("NVGspControl-display615-probe-result",UInt32(r),32);
    ulk();
    // 0.180.146: the connect state is readable at this runtime, so start the
    // 300 ms display-port poll here as well as on NVDisplay's registration.
    // The poll's first sample records the baseline mask.
    if(r==kIOReturnSuccess)startHotplugPoll();
    return r;
}


// 0.180.145: selector 50 op 1, read-only GET_SM_ISSUE_RATE_MODIFIER
// (0x20801230) on the lifecycle subdevice through the native615 control RPC.
// One RPC, no GPU work, nothing written. out: u32 version (1), u32 RM status,
// u8 imla0, fmla16, dp, fmla32, ffma, imla1, imla2, imla3, imla4, 7 pad.
// An RM refusal returns its status (the runtime stays as it is); a timeout
// or a malformed reply fails closed without a retry.
IOReturn NVGspControl::smIssueRate615(UInt32 flags, UInt8 *out, UInt32 *outBytes) {
    if(!lock_ || !out || !outBytes || *outBytes<24 || flags)return kIOReturnBadArgument;
    *outBytes=0;
    lk(__LINE__);
    const bool ready=native615_ && native615Ready_ && !native615Busy_ && postInitPhase_==27 && gmcInitReady_ &&
        !gmcMmuFaultPending_ && !userRpcLatePending_ && !resetBusy_ && !sleeping_ && !native615Stopping_;
    if(ready) {native615Busy_=true;setProperty("NVGspControl-native615-busy",true);}
    ulk();
    if(!ready)return kIOReturnNotReady;
    constexpr UInt32 C=nvgsp::kLifecycleClient615,S=nvgsp::kLifecycleSubdevice615,P=nvgsp::kSmIssueRateParams615;
    auto *s=native615_;
    UInt32 status=~0U;
    UInt8 v[nvgsp::kSmIssueRateFields615]{};
    IOReturn r=kIOReturnBadArgument;
    if(nvgsp::buildControl615(s->request,sizeof(s->request),C,S,nvgsp::kSmIssueRateCommand615,P)) {
        bzero(s->request+40,P);   // grRouteInfo flags 0, route 0: the default GR engine
        UInt32 bytes=sizeof(s->reply),result=~0U;
        r=userRpcImpl(76,s->request,40+P,s->reply,&bytes,&result,true,true);
        if(!r && !nvgsp::smIssueRateReply615(s->reply,bytes,C,S,v,&status))
            r=status && status!=~0U ? kIOReturnUnsupported : kIOReturnBadMessageID;
        if(!r && result) {status=result;r=kIOReturnUnsupported;}
    }
    lk(__LINE__);
    native615Busy_=false;setProperty("NVGspControl-native615-busy",false);
    setProperty("native615-sm-issue-rate-result",UInt32(r),32);
    setProperty("native615-sm-issue-rate-status",status,32);
    if(!r) {
        char text[128];
        nvgsp::smIssueRateString615(v,text,sizeof(text));
        setProperty("native615-sm-issue-rate",text);
        for(UInt32 i=0;i<nvgsp::kSmIssueRateFields615;++i) {
            char key[48];
            snprintf(key,sizeof(key),"native615-sm-issue-rate-%s",nvgsp::smIssueRateField615(i));
            setProperty(key,v[i],8);
        }
    }
    ulk();
    const UInt32 version=1;
    __builtin_memcpy(out,&version,4);__builtin_memcpy(out+4,&status,4);
    bzero(out+8,16);
    if(!r)__builtin_memcpy(out+8,v,sizeof(v));
    *outBytes=24;
    return r;
}

// 0.180.190: selector 50 op 2, read-only GET_CONSTRUCTED_FALCON_INFO (0x208001b0) on the lifecycle
// subdevice: is NVDEC0/NVENC0/OFA0 constructed in this GSP-RM, and how big is its falcon context
// (step 1 of an NVDEC channel on native615). One RPC, nothing allocated or written.
// out: u32 version (2), u32 RM status, u32 falcon count, u32 ctx bytes NVDEC0, NVENC0, OFA0 (~0 = absent).
IOReturn NVGspControl::falconInfo615(UInt32 flags, UInt8 *out, UInt32 *outBytes) {
    if(!lock_ || !out || !outBytes || *outBytes<24 || flags)return kIOReturnBadArgument;
    *outBytes=0;
    lk(__LINE__);
    const bool ready=native615_ && native615Ready_ && !native615Busy_ && postInitPhase_==27 && gmcInitReady_ &&
        !gmcMmuFaultPending_ && !userRpcLatePending_ && !resetBusy_ && !sleeping_ && !native615Stopping_;
    if(ready) {native615Busy_=true;setProperty("NVGspControl-native615-busy",true);}
    ulk();
    if(!ready)return kIOReturnNotReady;
    constexpr UInt32 C=nvgsp::kLifecycleClient615,S=nvgsp::kLifecycleSubdevice615,P=nvgsp::kFalconInfoBytes;
    auto *s=native615_;
    UInt32 status=~0U,count=0,ctx[3]={~0U,~0U,~0U};
    IOReturn r=kIOReturnBadArgument;
    if(nvgsp::buildControl615(s->request,sizeof(s->request),C,S,nvgsp::kCmdGetConstructedFalconInfo,P)) {
        bzero(s->request+40,P);
        UInt32 bytes=sizeof(s->reply),result=~0U;
        r=userRpcImpl(76,s->request,40+P,s->reply,&bytes,&result,true,true);
        nvgsp::ControlReply615 reply{};
        if(!r && !nvgsp::decodeControlReply615(s->reply,bytes,C,S,nvgsp::kCmdGetConstructedFalconInfo,P,reply))
            r=kIOReturnBadMessageID;
        if(!r) {
            status=result ? result : reply.rpcResult ? reply.rpcResult : reply.privateResult ? reply.privateResult : reply.header.status;
            if(status || !reply.successful())r=kIOReturnUnsupported;
        }
        if(!r) {
            __builtin_memcpy(&count,reply.params,4);
            for(UInt32 i=0;i<3;++i)nvgsp::falconCtxBytes(reply.params,P,nvgsp::videoEngine(i)->engDesc,&ctx[i]);
            UInt32 n=count<nvgsp::kMaxConstructedFalcons?count:nvgsp::kMaxConstructedFalcons;
            lk(__LINE__);setProperty("native615-falcon-info",const_cast<uint8_t *>(reply.params),4+n*20);ulk();
        }
    }
    lk(__LINE__);
    native615Busy_=false;setProperty("NVGspControl-native615-busy",false);
    setProperty("native615-falcon-info-result",UInt32(r),32);
    setProperty("native615-falcon-info-status",status,32);
    if(!r)setProperty("native615-falcon-ctx-bytes",ctx,sizeof(ctx));
    ulk();
    const UInt32 w[6]={2,status,count,ctx[0],ctx[1],ctx[2]};
    __builtin_memcpy(out,w,24);
    *outBytes=24;
    return r;
}


// 0.180.134: SPECIFIC_GET_EDID_V2 (0x730245, live DDC: flags 0, as nvkms)
// on one connected display of the display common object made by
// displayProbe615. The bytes go to NVGspControl-display615-edid. out[0] =
// EDID size, out[1] = RM status, out[2] = display id.
IOReturn NVGspControl::displayEdid615(UInt32 displayId, UInt64 out[3]) {
    if(!lock_ || !out)return kIOReturnBadArgument;
    out[0]=out[1]=0;out[2]=displayId;
    if(!displayId || (displayId&(displayId-1)) || !(displayId&0x7f00))return kIOReturnBadArgument;
    lk(__LINE__);
    const bool ready=native615_ && native615Ready_ && !native615Busy_ && postInitPhase_==27 && gmcInitReady_ &&
        displayCommon615_ && !gmcMmuFaultPending_ && !userRpcLatePending_ && !resetBusy_ && !sleeping_;
    if(ready) {native615Busy_=true;setProperty("NVGspControl-native615-busy",true);}
    ulk();
    if(!ready)return kIOReturnNotReady;
    constexpr UInt32 C=nvgsp::kLifecycleClient615,DISP=0xc0d00073,CMD=0x730245,PARAMS=2064;
    auto *s=native615_;
    IOReturn r=kIOReturnNoMemory;
    UInt8 *req=static_cast<UInt8 *>(IOMallocZero(40+PARAMS));
    UInt32 size=0;
    if(req) {
        r=kIOReturnBadArgument;
        if(nvgsp::buildControl615(req,40+PARAMS,C,DISP,CMD,PARAMS)) {
            const UInt32 head[4]={0,displayId,2048,0};   // subdevice, display, buffer size, flags
            __builtin_memcpy(req+40,head,sizeof(head));
            UInt32 bytes=sizeof(s->reply),result=~0U;
            r=userRpcImpl(76,req,40+PARAMS,s->reply,&bytes,&result,true,true);
            nvgsp::ControlReply615 reply{};
            if(!r && (bytes!=96+PARAMS || !nvgsp::decodeControlReply615(s->reply,bytes,C,DISP,CMD,PARAMS,reply)))
                r=kIOReturnBadMessageID;
            if(!r) {
                out[1]=result?result:reply.header.status;
                __builtin_memcpy(&size,reply.params+8,4);
                if(out[1] || size>2048)r=kIOReturnUnsupported;
                else {
                    out[0]=size;
                    lk(__LINE__);setProperty("NVGspControl-display615-edid",const_cast<UInt8 *>(reply.params+16),size);
                    if(OSData *e=OSData::withBytes(reply.params+16,size)) {setProperty("NVGspControl-edid",e);e->release();}   // 0.180.141
                    ulk();
                }
            }
        }
        IOFree(req,40+PARAMS);
    }
    lk(__LINE__);
    native615Busy_=false;setProperty("NVGspControl-native615-busy",false);
    setProperty("NVGspControl-display615-edid-status",UInt32(out[1]),32);
    setProperty("NVGspControl-display615-edid-result",UInt32(r),32);
    ulk();return r;
}


// 0.180.135: display bring-up from user space while the native runtime owns
// the paused GSP. Input: u32 op, u32 a, u64 b, payload. Output: u32 status
// (RM result for op 0) + data.
//   op 0  RPC function a (76 control, 103 alloc, 10 free) with payload; the
//         phase-27 diagnostic validator applies (owned 0xc0d handles,
//         published classes). A class 0x40 allocation's VRAM becomes
//         writable through op 1.
//   op 1  VRAM write of the payload at b (inside an op-0 VRAM allocation or
//         the diagnostic range).
//   op 2  VRAM read of a dwords (<= 1000) at b.
//   op 3  BAR0 read of a dwords (<= 256) at b, display range 0x610000-0x6fffff.
//   op 4  BAR0 write of the payload at b, same range.
//   op 6  (0.180.137) contiguous system-memory scan-out test buffer of a
//         MiB (<= 64, kept until stop), filled with a checkerboard; out =
//         u64 physical address, u64 bytes.
//   op 7  (0.180.138) framebuffer copy: a = 0 stop, 1 one copy now, 2 copy
//         every ~16 ms; b = VRAM destination (diagnostic range); payload =
//         u32 pitch, u32 lines (pitch * lines <= the op 6 buffer).
//   op 9  (0.180.140) write the payload into the op 6 framebuffer at offset b.
//   op 11 (0.180.181) double-buffered scan-out: b = second VRAM surface (op 8
//         reserved, same size as the op 7 one), payload = u64 window 0
//         push buffer, u32 window 0 USER (put) offset; a = 0 back to one buffer.
//   op 8  (0.180.138) reserve a MiB of VRAM at b (diagnostic range) in the
//         client VRAM heap so no Metal allocation lands on a display surface.
//   op 5  (0.180.136) internal display control a with payload as params on
//         the GSP internal client/subdevice (CPU-RM's half of a display
//         channel or instance-memory setup); a must be one of the list below.
IOReturn NVGspControl::displayTransport615(const UInt8 *in, UInt32 inBytes, UInt8 *out, UInt32 *outBytes) {
    if(!lock_ || !in || !out || !outBytes || inBytes<16 || *outBytes<8)return kIOReturnBadArgument;
    UInt32 op=0,a=0;UInt64 b=0;
    __builtin_memcpy(&op,in,4);__builtin_memcpy(&a,in+4,4);__builtin_memcpy(&b,in+8,8);
    const UInt8 *payload=in+16;const UInt32 payloadBytes=inBytes-16;
    const UInt32 capacity=*outBytes;*outBytes=4;
    UInt32 status=0;
    lk(__LINE__);
    // 0.180.167: short holders (display copy tick, connect poll, Metal wraps,
    // the perf boost control) made a display op fail at random with NotReady,
    // and disp_all.py/disp_replug.py treat most failures as fatal. Wait up to
    // ~100 ms for the runtime to be free first.
    for(UInt32 i=0;i<100 && native615Busy_;++i) {ulk();IOSleep(1);lk(__LINE__);}
    const bool ready=native615_ && native615Ready_ && !native615Busy_ && postInitPhase_==27 && gmcInitReady_ &&
        !gmcMmuFaultPending_ && !userRpcLatePending_ && !resetBusy_ && !sleeping_ && pci_;
    if(ready) {native615Busy_=true;setProperty("NVGspControl-native615-busy",true);}
    ulk();
    if(!ready)return kIOReturnNotReady;
    IOReturn r=kIOReturnBadArgument;
    auto allowedWrite=[&](UInt64 at,UInt64 bytes) {
        if(!bytes || at+bytes<at)return false;
        for(auto &d:displayRanges615_)if(d.bytes && at>=d.offset && at+bytes<=d.offset+d.bytes)return true;
        UInt64 start=0,end=0;
        return nvgsp::diagnosticVramBounds615(gmcConfig_,&start,&end) && at>=start && at+bytes<=end;
    };
    if(op==0) {
        UInt32 replyBytes=capacity-4,result=~0U;
        r=userRpcImpl(a,payload,payloadBytes,out+4,&replyBytes,&result,true,true);
        status=result;*outBytes=4+replyBytes;
        // remember the VRAM of a successful NV01_MEMORY_LOCAL_USER allocation
        if(!r && a==103 && !result && payloadBytes>=32+sizeof(nvgsp::MemoryParams615)) {
            UInt32 parent=0,object=0,cls=0;
            __builtin_memcpy(&parent,payload+4,4);__builtin_memcpy(&object,payload+8,4);__builtin_memcpy(&cls,payload+12,4);
            const UInt8 *ret=nullptr;
            const auto rep=nvgsp::decodeAlloc615(out+4,replyBytes,parent,object,cls,sizeof(nvgsp::MemoryParams615),&ret);
            if(cls==0x40 && rep.valid && rep.successful() && ret) {
                nvgsp::MemoryParams615 m{};__builtin_memcpy(&m,ret,sizeof(m));
                lk(__LINE__);
                for(auto &d:displayRanges615_)if(!d.bytes) {d={m.offset,m.size};break;}
                ulk();
            }
        }
    } else if(op==1) {
        const UInt32 words=payloadBytes/4;
        r=(!words || words>960 || (payloadBytes&3) || (b&3) || !allowedWrite(b,payloadBytes)) ? kIOReturnNotPermitted :
            vramAccess(b,const_cast<UInt32 *>(reinterpret_cast<const UInt32 *>(payload)),words,true,true);
    } else if(op==2) {
        if(!a || a>1000 || (b&3) || 4+a*4>capacity)r=kIOReturnBadArgument;
        else {r=vramAccess(b,reinterpret_cast<UInt32 *>(out+4),a,false,true);if(!r)*outBytes=4+a*4;}
    } else if(op==6) {
        r=kIOReturnBadArgument;
        if(a && a<=64 && capacity>=20) {
            const UInt64 bytes=UInt64(a)<<20;
            const bool fill=(b&1)!=0;   // 0.180.141: the pattern only on request
            if(displayFb615_ && displayFb615_->getLength()!=bytes) {
                displayFb615_->complete();displayFb615_->release();displayFb615_=nullptr;
            }
            if(!displayFb615_) {
                auto *m=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
                    kIODirectionInOut|kIOMemoryPhysicallyContiguous|kIOMemoryKernelUserShared,bytes,0x000000FFFFE00000ULL);
                if(m && m->prepare()!=kIOReturnSuccess) {m->release();m=nullptr;}
                displayFb615_=m;
            }
            r=kIOReturnNoMemory;
            IOByteCount len=0;
            const UInt64 phys=displayFb615_ ? displayFb615_->getPhysicalSegment(0,&len,kIOMemoryMapperNone) : 0;
            if(phys && len>=bytes) {
                auto *px=static_cast<UInt32 *>(displayFb615_->getBytesNoCopy());
                for(UInt32 y=0;fill && y<2160 && UInt64(y+1)*15360<=bytes;++y)
                    for(UInt32 x=0;x<3840;++x)
                        px[y*3840+x]=((x/120+y/120)&1) ? 0xff00c000U : 0xffc000c0U;
                __builtin_memcpy(out+4,&phys,8);__builtin_memcpy(out+12,&bytes,8);
                *outBytes=20;r=kIOReturnSuccess;
            }
        }
    } else if(op==7) {
        UInt32 geo[2]={0,0};
        if(payloadBytes>=8)__builtin_memcpy(geo,payload,8);
        UInt64 start=0,end=0;
        const UInt64 bytes=UInt64(geo[0])*geo[1];
        r=kIOReturnBadArgument;
        if(a==0) {
            lk(__LINE__);displayCopyOn615_=false;ulk();r=kIOReturnSuccess;
        } else if(a<=2 && displayFb615_ && geo[0] && !(geo[0]&63) && geo[1] && geo[1]<=8192 && !(b&0xff) &&
                  bytes<=displayFb615_->getLength() && nvgsp::diagnosticVramBounds615(gmcConfig_,&start,&end) &&
                  b>=start && b+bytes<=end) {
            lk(__LINE__);
            displayCopyDst615_=b;displayCopyPitch615_=geo[0];displayCopyLines615_=geo[1];
            displayBack615_=displayFront615_=0;displayFlipPending615_=displayDblStop615_=false;   // 0.180.181
            ulk();
            // 0.180.176: the display pipe flips into this surface (LOCAL_FB)
            setProperty("NVGspControl-display615-scanout-phys",b,64);
            // 0.180.176: tell GSP-RM the DP stream carries RGB. The struct is
            // NV0073_CTRL_CMD_DP_CONFIG_STREAM_PARAMS with NvBool = NvU8 (see
            // ctrl0073dp.h + nvtypes.h), so colorFormat sits at 0x24 and the
            // total size is 0x58. bEnableOverride stays 0: RM keeps its own
            // link timing, only the colour format is ours. The boot bring-up
            // never sent this, which is consistent with a correct framebuffer
            // (VNC) and shifted hue/gamma on the panel.
            {
                UInt8 sp[0x58] = {};
                const UInt32 dpLink = 1, rgb = 1;
                __builtin_memcpy(sp + 0x0C, &dpLink, 4);
                __builtin_memcpy(sp + 0x24, &rgb, 4);     // colorFormat = RGB
                const UInt32 sr = displayCtrl(0x731362, sp, sizeof(sp));
                setProperty("NVGspControl-display615-stream-color", sr, 32);
            }
            if(a==1) {
                r=displayFbCopy615(true) ? kIOReturnSuccess : kIOReturnIOError;   // this transport holds busy
            } else {
                if(!displayCopyCall615_)displayCopyCall615_=thread_call_allocate(&NVGspControl::displayCopyCallout615,this);
                r=kIOReturnNoMemory;
                if(displayCopyCall615_) {
                    lk(__LINE__);displayCopyOn615_=true;displayCopyFails615_=0;ulk();
                    thread_call_enter(displayCopyCall615_);r=kIOReturnSuccess;
                }
            }
        }
    } else if(op==11) {   // 0.180.181: double-buffered scan-out
        UInt64 pb=0,start=0,end=0;UInt32 user=0;
        if(payloadBytes>=12) {__builtin_memcpy(&pb,payload,8);__builtin_memcpy(&user,payload+8,4);}
        lk(__LINE__);
        const UInt64 bytes=UInt64(displayCopyPitch615_)*displayCopyLines615_,primary=displayCopyDst615_;
        const bool on=displayBack615_!=0;
        ulk();
        r=kIOReturnBadArgument;
        if(a==0) {lk(__LINE__);displayDblStop615_=on;ulk();r=kIOReturnSuccess;}
        else if(!on && primary && bytes && b && !(b&0xfffff) && pb && !(pb&0xfff) && pb+0x1000<=gmcConfig_.fbBytes &&
                user>=0x690000 && user<0x6f0000 && !(user&0xfff) && nvgsp::diagnosticVramBounds615(gmcConfig_,&start,&end) &&
                b>=start && b+bytes<=end && (b+bytes<=primary || b>=primary+bytes)) {
            lk(__LINE__);
            displayWndPb615_=pb;displayWndUser615_=user;displayFront615_=primary;displayBack615_=b;
            displayFlipPending615_=displayDblStop615_=false;
            displayPrevFirst615_=0;displayPrevLast615_=displayCopyLines615_;   // the back buffer holds nothing yet
            setProperty("NVGspControl-display615-dblbuf",b,64);ulk();
            r=kIOReturnSuccess;
        }
    } else if(op==10) {   // 0.180.141: register the head-0 cursor for NVDisplay
        UInt64 surf=0;UInt32 user=0;
        if(payloadBytes>=12) {__builtin_memcpy(&surf,payload,8);__builtin_memcpy(&user,payload+8,4);}
        UInt64 start=0,end=0;
        r=kIOReturnBadArgument;
        if(a==0) {lk(__LINE__);displayNative615_=false;ulk();r=kIOReturnSuccess;}
        else if(surf && !(surf&0xffff) && user>=0x690000 && user<0x6f0000 && !(user&0xfff) &&
                nvgsp::diagnosticVramBounds615(gmcConfig_,&start,&end) && surf+0x40000<=gmcConfig_.fbBytes) {
            lk(__LINE__);displayCursorSurface615_=surf;displayCursorUser615_=user;displayNative615_=true;
            displayCursorWroteW615_=displayCursorWroteH615_=64;   // 0.180.169: the bring-up's 64x64 arrow goes too
            setProperty("NVGspControl-display615-native",true);ulk();
            // 0.180.143: what WindowServer sent before the head existed
            // (this transport holds busy)
            r=displayCursorWrite615();
            if(!r)r=displayCursorPosition615();
        }
    } else if(op==9) {   // 0.180.140: write the payload into the op 6 framebuffer at b
        r=kIOReturnBadArgument;
        if(displayFb615_ && payloadBytes && b<displayFb615_->getLength() && payloadBytes<=displayFb615_->getLength()-b) {
            __builtin_memcpy(static_cast<UInt8 *>(displayFb615_->getBytesNoCopy())+b,payload,payloadBytes);
            r=kIOReturnSuccess;
        }
    } else if(op==8) {
        UInt64 start=0,end=0;
        const UInt64 bytes=UInt64(a)<<20;
        r=kIOReturnBadArgument;
        if(a && a<=1024 && !(b&0xfffff) && nvgsp::diagnosticVramBounds615(gmcConfig_,&start,&end) &&
           b>=start && b+bytes<=end) {
            lk(__LINE__);r=vramHeap_.insert(b,bytes) ? kIOReturnSuccess : kIOReturnNoSpace;ulk();
        }
    } else if(op==5) {
        static const UInt32 kAllowed[]={0x20800a01,0x20800a49,0x20800a4b,0x20800a58,0x20800a5d,0x20800af1,0x20800af2};
        bool ok=false;
        for(UInt32 c:kAllowed)ok=ok || c==a;
        UInt8 *req=ok && payloadBytes<=3900-40 ? static_cast<UInt8 *>(IOMallocZero(40+payloadBytes)) : nullptr;
        r=ok ? kIOReturnNoMemory : kIOReturnNotPermitted;
        if(req) {
            r=kIOReturnBadArgument;
            if(internalClient_ && nvgsp::buildControl615(req,40+payloadBytes,internalClient_,internalSubdevice_,a,payloadBytes)) {
                if(payloadBytes)__builtin_memcpy(req+40,payload,payloadBytes);
                UInt32 replyBytes=capacity-4,result=~0U;
                lk(__LINE__);displayInternalCmd615_=a;ulk();
                r=userRpcImpl(76,req,40+payloadBytes,out+4,&replyBytes,&result,true,true);
                lk(__LINE__);displayInternalCmd615_=0;ulk();
                status=result;*outBytes=4+replyBytes;
            }
            IOFree(req,40+payloadBytes);
        }
    } else if(op==3 || op==4) {
        const UInt32 words=op==3 ? a : payloadBytes/4;
        if(!words || words>256 || (b&3) || b<0x610000 || b+words*4>0x700000 || (op==3 && 4+words*4>capacity))
            r=kIOReturnNotPermitted;
        else {
            IOMemoryMap *map=sharedBar0Map(pci_);
            r=kIOReturnNoMemory;
            if(map && map->getLength()>=0x700000) {
                Bar0Io bar0{map};r=kIOReturnSuccess;
                for(UInt32 i=0;i<words && !r;++i) {
                    if(op==3) {UInt32 v=0;if(!bar0.read(UInt32(b)+i*4,&v))r=kIOReturnIOError;__builtin_memcpy(out+4+i*4,&v,4);}
                    else {UInt32 v=0;__builtin_memcpy(&v,payload+i*4,4);if(!bar0.write(UInt32(b)+i*4,v))r=kIOReturnIOError;}
                }
                if(op==3 && !r)*outBytes=4+words*4;
            }
            if(map)map->release();
        }
    }
    __builtin_memcpy(out,&status,4);
    lk(__LINE__);
    native615Busy_=false;setProperty("NVGspControl-native615-busy",false);
    setProperty("NVGspControl-display615-transport-last",op,32);
    ulk();return r;
}


// 0.180.138: one CE copy of the sysmem framebuffer (op 6 buffer, physical
// COHERENT_SYSMEM source) to the VRAM scan-out surface (physical LOCAL_FB),
// on the maintenance channel. Caller holds native615Busy_ with lock_
// released.
// 0.180.139: the copy has its own push buffer (ring page +0xc00) and
// semaphore (+0xe10): mmuZero615/mmuSubmit615 use +0x800 and poll +0xe00, so
// an in-flight copy never rewrites their methods or their fence. wait=false
// only kicks; displayCopyTick615 retires it from the semaphore, so busy is
// held for the submission alone (a 9.6 ms 4K copy no longer stalls Metal).
bool NVGspControl::displayFbCopy615(bool wait, UInt32 firstLine, UInt32 lineCount) {
    auto *s=native615_;
    if(!s || !s->mmu.live || !displayFb615_ || !displayCopyDst615_ || !displayCopyPitch615_ || !displayCopyLines615_)
        return false;
    auto &m=s->mmu;
    IOByteCount len=0;
    const UInt64 src=displayFb615_->getPhysicalSegment(0,&len,kIOMemoryMapperNone);
    const UInt32 pitch=displayCopyPitch615_,total=displayCopyLines615_;
    if(!src || len<UInt64(pitch)*total || firstLine>=total)return false;
    const UInt32 lines=lineCount && lineCount<=total-firstLine ? lineCount : total-firstLine;
    const UInt64 dst=(displayBack615_ ? displayBack615_ : displayCopyDst615_)+UInt64(firstLine)*pitch;   // 0.180.181
    const UInt64 srcAt=src+UInt64(firstLine)*pitch;
    auto read=[&](UInt64 at,UInt32 count) {return vramAccess(at,s->readWords,count,false,true)==kIOReturnSuccess;};
    auto write=[&](UInt64 at,const UInt32 *words,UInt32 count) {
        if(!count || count>1024)return false;
        if(words!=s->writeWords)__builtin_memcpy(s->writeWords,words,count*4);
        return vramAccess(at,s->writeWords,count,true,true)==kIOReturnSuccess && read(at,count) &&
            !__builtin_memcmp(s->writeWords,s->readWords,count*4);
    };
    auto ce=[](UInt32 method,UInt32 n) {return 0x20000000U|(n<<16)|(4U<<13)|(method>>2);};
    const UInt32 fence=0x6150e000|((m.fence+1)&0xfff);
    const UInt64 semaphore=m.va+0xe10,pb=m.va+0xc00;
    UInt32 w[40];UInt32 c=0;
    w[c++]=ce(0x000,1);w[c++]=0xC7B5;                                  // SET_OBJECT AMPERE_DMA_COPY_B
    w[c++]=ce(0x260,1);w[c++]=1;                                       // SRC_PHYS_MODE COHERENT_SYSMEM
    w[c++]=ce(0x264,1);w[c++]=0;                                       // DST_PHYS_MODE LOCAL_FB
    w[c++]=ce(0x400,4);w[c++]=UInt32(srcAt>>32);w[c++]=UInt32(srcAt);w[c++]=UInt32(dst>>32);w[c++]=UInt32(dst);
    // 0.180.166: the band is contiguous in both surfaces (same pitch, no
    // padding), so copy it as one 1D line. The 2160-line
    // non-pipelined copy took ~10 ms for 4K while a Metal blit of the same
    // size took 1.6 ms (20 GB/s) once the clocks were boosted.
    const UInt64 span=UInt64(pitch)*lines;
    if(span<=0xffffffffULL) {
        w[c++]=ce(0x410,4);w[c++]=pitch;w[c++]=pitch;w[c++]=UInt32(span);w[c++]=1;
        // non-pipelined (completion ordering unchanged), flush, pitch/pitch, physical
        w[c++]=ce(0x300,1);w[c++]=0x002U|0x004U|0x080U|0x100U|0x1000U|0x2000U;
    } else {
        w[c++]=ce(0x410,4);w[c++]=pitch;w[c++]=pitch;w[c++]=pitch;w[c++]=lines;
        // non-pipelined, flush, pitch/pitch, multi-line, physical source and destination
        w[c++]=ce(0x300,1);w[c++]=0x002U|0x004U|0x080U|0x100U|0x200U|0x1000U|0x2000U;
    }
    const UInt32 tail[6]={0x20050017,UInt32(semaphore)&0xfffffffc,UInt32(semaphore>>32)&0xff,fence,0,0x00100001};
    for(UInt32 i=0;i<6;++i)w[c++]=tail[i];
    const UInt32 entry[2]={UInt32(pb)&0xfffffffc,(UInt32(pb>>32)&0xff)|(c<<10)};
    const UInt32 put=(m.put+1)%256;
    if(!write(m.ring+0xc00,w,c) || !write(m.ring+UInt64(m.put)*8,entry,2) || !write(m.userd+0x8c,&put,1) ||
       diagnosticChannelKick(0xc0d00556,true)!=kIOReturnSuccess)return false;
    lk(__LINE__);m.put=put;m.fence=fence&0xfff;
    displayCopyFence615_=fence;displayCopyInflight615_=true;displayCopyKick615_=mach_absolute_time();ulk();
    if(!wait)return true;
    UInt64 deadline=0;clock_interval_to_deadline(1,kSecondScale,&deadline);bool done=false;
    do {
        if(!read(m.ring+0xe10,1))return false;
        if(s->readWords[0]==fence) {done=true;break;}
        IODelay(50);
    } while(mach_absolute_time()<deadline);
    lk(__LINE__);displayCopyInflight615_=!done;ulk();
    return done;
}

void NVGspControl::displayCopyCallout615(thread_call_param_t p0, thread_call_param_t) {
    static_cast<NVGspControl *>(p0)->displayCopyTick615();
}

// 0.180.138: ~60 Hz framebuffer copy; a frame is skipped (not waited for)
// while another native operation runs, and 30 failed copies in a row stop it.
// 0.180.139: asynchronous - a tick retires the previous copy from its
// semaphore (1 s limit) and kicks the next; busy covers only that work.
void NVGspControl::displayCopyTick615() {
    if(!lock_)return;
    lk(__LINE__);
    if(!displayCopyOn615_ || native615Stopping_) {ulk();return;}
    const UInt32 pitch=displayCopyPitch615_,lines=displayCopyLines615_;
    const bool inflight=displayCopyInflight615_,flipWait=displayFlipPending615_;   // 0.180.181
    ulk();
    // damage: FNV-style hash of each band (CPU only; no GPU, no busy)
    UInt32 dirtyFirst=~0U,dirtyLast=0;
    const UInt32 bandLines=(lines+kDisplayBands615-1)/kDisplayBands615;
    // 0.180.170: while the display pipe is flipping into the VRAM scan-out
    // surface itself, this tick neither hashes the (now stale) aperture nor
    // copies over the flipped frames; hashing 33 MB per tick was also a large
    // CPU cost. 400 ms without a pipe flip brings the tick back.
    UInt64 pipeNs=0;
    if(pipePresentAt615_)absolutetime_to_nanoseconds(mach_absolute_time()-pipePresentAt615_,&pipeNs);
    const bool pipeActive=pipePresentAt615_ && pipeNs<400000000ULL;
    if(!inflight && !flipWait && !pipeActive && displayFb615_ && pitch && lines && bandLines) {
        const auto *px=static_cast<const UInt64 *>(displayFb615_->getBytesNoCopy());
        for(UInt32 b=0;b<kDisplayBands615;++b) {
            const UInt32 l0=b*bandLines;if(l0>=lines)break;
            const UInt32 l1=l0+bandLines<lines ? l0+bandLines : lines;
            const UInt64 *p=px+UInt64(l0)*pitch/8,*e=px+UInt64(l1)*pitch/8;
            UInt64 h=0xcbf29ce484222325ULL;
            for(;p+4<=e;p+=4)h=(h^(p[0]+(p[1]<<1)+(p[2]<<2)+(p[3]<<3)))*0x100000001b3ULL;
            for(;p<e;++p)h=(h^*p)*0x100000001b3ULL;
            if(!displayBandsValid615_ || h!=displayBandHash615_[b]) {
                if(dirtyFirst==~0U)dirtyFirst=l0;
                dirtyLast=l1;
            }
            displayBandHash615_[b]=h;
        }
        displayBandsValid615_=true;
        // 0.180.155: add damage seen on ticks that were skipped while busy
        if(displayPendFirst615_!=~0U) {
            if(dirtyFirst==~0U || displayPendFirst615_<dirtyFirst)dirtyFirst=displayPendFirst615_;
            if(displayPendLast615_>dirtyLast)dirtyLast=displayPendLast615_;
        }
        // a full refresh every 2 s keeps a missed write from staying forever
        // (0.180.170: never once the display pipe presented - it would put the
        // stale aperture back over the flipped frames)
        UInt64 ns=0;absolutetime_to_nanoseconds(mach_absolute_time()-displayLastFull615_,&ns);
        if(ns>2000000000ULL && !pipeEverPresented615_) {dirtyFirst=0;dirtyLast=lines;displayLastFull615_=mach_absolute_time();}
    }
    lk(__LINE__);
    const bool want=inflight || flipWait || dirtyFirst!=~0U;
    const bool ready=want && !pipeActive && native615_ && native615Ready_ && !native615Busy_ && !sleeping_ && !resetBusy_ &&
        !gmcMmuFaultPending_ && !userRpcLatePending_ && postInitPhase_==27;
    // 0.180.155: a busy skip keeps its damage range for the next tick instead
    // of invalidating every band (that forced a full 33 MB copy after each
    // skip, and under Metal load skips are frequent)
    if(ready)native615Busy_=true;
    else if(want) {
        ++displayCopySkips615_;
        if(dirtyFirst!=~0U) {displayPendFirst615_=dirtyFirst;displayPendLast615_=dirtyLast;}
    } else ++displayCopyIdle615_;
    ulk();
    bool retired=false,swapped=false;
    if(ready) {
        auto *s=native615_;
        bool ok=true,kick=dirtyFirst!=~0U;
        if(displayCopyInflight615_) {
            kick=false;
            if(s && vramAccess(s->mmu.ring+0xe10,s->readWords,1,false,true)==kIOReturnSuccess &&
               s->readWords[0]==displayCopyFence615_) {
                UInt64 ns=0;absolutetime_to_nanoseconds(mach_absolute_time()-displayCopyKick615_,&ns);
                lk(__LINE__);displayCopyInflight615_=false;++displayCopyFrames615_;displayCopyFails615_=0;
                displayCopyLastUs615_=UInt32(ns/1000);ulk();
                retired=true;
            } else {
                UInt64 ns=0;absolutetime_to_nanoseconds(mach_absolute_time()-displayCopyKick615_,&ns);
                if(ns>1000000000ULL) {   // lost: count a failure, copy everything next tick
                    ok=false;lk(__LINE__);displayCopyInflight615_=false;displayBandsValid615_=false;ulk();
                }
            }
        }
        // 0.180.181: double buffer. A retired copy filled the back buffer:
        // flip to it. The next copy waits until that flip latched (the old
        // front is scanned out until the vblank).
        if(ok && displayBack615_) {
            if(retired)ok=displayPushFlip615(displayBack615_);
            if(ok && displayFlipPending615_ && displayFlipLatched615()) {
                lk(__LINE__);
                const UInt64 shown=displayBack615_;displayBack615_=displayFront615_;displayFront615_=shown;
                displayFlipPending615_=false;++displayFlips615_;ulk();swapped=true;
                if(!kick && displayPendFirst615_!=~0U) {dirtyFirst=displayPendFirst615_;dirtyLast=displayPendLast615_;kick=true;}
            }
            if(displayFlipPending615_) {
                if(kick) {displayPendFirst615_=dirtyFirst;displayPendLast615_=dirtyLast;kick=false;++displayFlipWaits615_;}
            } else if(displayDblStop615_ && !displayCopyInflight615_) {
                // back to the single primary surface, which then gets a full copy
                if(displayFront615_!=displayCopyDst615_)ok=displayPushFlip615(displayCopyDst615_);
                lk(__LINE__);displayBack615_=displayFront615_=0;displayDblStop615_=displayFlipPending615_=false;
                displayBandsValid615_=false;setProperty("NVGspControl-display615-dblbuf",0ULL,64);ulk();
                kick=false;
            }
            if(!ok) {   // the window channel did not take the flip: single buffer from here on
                lk(__LINE__);displayBack615_=displayFront615_=0;displayFlipPending615_=false;displayBandsValid615_=false;
                setProperty("NVGspControl-display615-dblbuf",0ULL,64);ulk();
            }
        }
        if(kick && ok) {
            UInt32 f0=dirtyFirst,f1=dirtyLast;
            if(displayBack615_) {   // the back buffer is two frames old: add the other frame's damage
                if(displayPrevFirst615_!=~0U && displayPrevFirst615_<f0)f0=displayPrevFirst615_;
                if(displayPrevFirst615_!=~0U && displayPrevLast615_>f1)f1=displayPrevLast615_;
                displayPrevFirst615_=dirtyFirst;displayPrevLast615_=dirtyLast;
            }
            ok=displayFbCopy615(false,f0,f1-f0);
            if(ok)displayCopyLinesTotal615_+=f1-f0;
            else {displayBandsValid615_=false;displayPrevFirst615_=0;displayPrevLast615_=lines;}
            displayPendFirst615_=~0U;displayPendLast615_=0;   // copied, or a full copy follows
        }
        lk(__LINE__);
        native615Busy_=false;
        if(!ok && ++displayCopyFails615_>=30)displayCopyOn615_=false;
        ulk();
    }
    lk(__LINE__);
    const bool on=displayCopyOn615_ && !native615Stopping_;
    if(!((displayCopyFrames615_+displayCopyIdle615_)%60) || !on) {
        setProperty("NVGspControl-display615-copy-idle",displayCopyIdle615_,32);
        setProperty("NVGspControl-display615-copy-lines",displayCopyLinesTotal615_,32);
        setProperty("NVGspControl-display615-copy-frames",displayCopyFrames615_,32);
        setProperty("NVGspControl-display615-copy-skips",displayCopySkips615_,32);
        setProperty("NVGspControl-display615-copy-fails",displayCopyFails615_,32);
        setProperty("NVGspControl-display615-copy-last-us",displayCopyLastUs615_,32);
        setProperty("NVGspControl-display615-copy-on",on);
        setProperty("NVGspControl-display615-flips",displayFlips615_,32);
        setProperty("NVGspControl-display615-flip-waits",displayFlipWaits615_,32);
    }
    ulk();
    if(on) {
        // 0.180.142: a quiet screen is re-hashed at ~30 Hz, a changing one at ~60 Hz
        UInt32 ms=(dirtyFirst!=~0U || inflight) ? 16 : 33;
        // 0.180.155: a frame used to take two 16 ms ticks (one kicks, the next
        // only retires and does not hash), so animation ran at ~30 fps and
        // copy-last-us measured the tick, not the copy. Poll an in-flight copy
        // every 4 ms; once it retires, hash and kick the next frame as soon
        // as 16 ms have passed since the previous kick (~60 Hz pacing).
        if(displayCopyInflight615_ || displayFlipPending615_) ms=4;
        else if(retired || swapped) {   // 0.180.182: a latched flip paces the next frame like a retire
            UInt64 ns=0;absolutetime_to_nanoseconds(mach_absolute_time()-displayCopyKick615_,&ns);
            const UInt64 sinceMs=ns/1000000ULL;
            ms=sinceMs>=15 ? 1 : UInt32(16-sinceMs);
        }
        UInt64 deadline=0;clock_interval_to_deadline(ms,kMillisecondScale,&deadline);
        thread_call_enter_delayed(displayCopyCall615_,deadline);
    }
}


// 0.180.181: window 0 SET_OFFSET(0) (0x260, surface >> 8) + UPDATE (0x200)
// on the window channel user space brought up, in disp_all.py push()'s
// format: one-method headers, JUMP 0x20000000 back to 0 at the end of the
// 4 KiB buffer. The window's present control (0x2a8 = 0x11) latches it at a
// vblank.
bool NVGspControl::displayPushFlip615(UInt64 surface) {
    if(!displayWndPb615_ || !displayWndUser615_ || !surface || (surface&0xff))return false;
    IOMemoryMap *bm=sharedBar0Map(pci_);
    bool ok=bm && bm->getLength()>=0x700000;
    UInt32 put=0,dpca=0;
    if(ok) {Bar0Io io{bm};ok=io.read(displayWndUser615_,&put) && !(put&3) && put<0x1000;}
    UInt32 base=put;
    if(ok && base+16+4>0x1000) {UInt32 jump=0x20000000U;ok=vramAccess(displayWndPb615_+base,&jump,1,true,true)==kIOReturnSuccess;base=0;}
    UInt32 w[4]={(1U<<18)|0x260,UInt32(surface>>8),(1U<<18)|0x200,1};
    if(ok)ok=vramAccess(displayWndPb615_+base,w,4,true,true)==kIOReturnSuccess;
    if(ok) {Bar0Io io{bm};ok=io.read(0x616330,&dpca) && io.write(displayWndUser615_,base+16);}
    if(bm)bm->release();
    lk(__LINE__);
    if(ok) {displayFlipPending615_=true;displayFlipFrame615_=dpca>>16;}
    ulk();
    return ok;
}

// the flip is on screen: window 0 update state IDLE (FE_CHNSTATUS_WIN(0)
// 11:8 == 1), all its methods fetched, and a new frame (RG_DPCA 31:16) began
bool NVGspControl::displayFlipLatched615() {
    IOMemoryMap *bm=sharedBar0Map(pci_);
    bool done=false;
    if(bm && bm->getLength()>=0x700000) {
        Bar0Io io{bm};UInt32 st=0,put=0,get=0,dpca=0;
        done=io.read(0x610664,&st) && io.read(displayWndUser615_,&put) && io.read(displayWndUser615_+4,&get) &&
             io.read(0x616330,&dpca) && ((st>>8)&0xf)==1 && put==get && (dpca>>16)!=displayFlipFrame615_;
    }
    if(bm)bm->release();
    return done;
}

// 0.180.141: the framebuffer NVDisplay hands to WindowServer: contiguous
// sysmem (the display engine refuses ISO from sysmem, so displayCopyTick615
// copies it to the VRAM scan-out surface), black, 3840x2160x4 in 32 MiB.
bool NVGspControl::displayFbAlloc615() {
    if(displayFb615_)return true;
    constexpr UInt64 kBytes=32ULL<<20;
    auto *m=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        kIODirectionInOut|kIOMemoryPhysicallyContiguous|kIOMemoryKernelUserShared,kBytes,0x000000FFFFE00000ULL);
    if(m && m->prepare()!=kIOReturnSuccess) {m->release();m=nullptr;}
    IOByteCount len=0;
    const UInt64 phys=m ? m->getPhysicalSegment(0,&len,kIOMemoryMapperNone) : 0;
    if(!phys || len<kBytes) {
        if(m) {m->complete();m->release();}
        setProperty("NVGspControl-display615-fb-phys",0ULL,64);
        return false;
    }
    bzero(m->getBytesNoCopy(),kBytes);
    displayFb615_=m;
    setProperty("NVGspControl-display615-fb-phys",phys,64);
    setProperty("NVGspControl-display615-fb-bytes",kBytes,64);
    setProperty("NVGspControl-display615-fb-width",3840ULL,32);
    setProperty("NVGspControl-display615-fb-height",2160ULL,32);
    setProperty("NVGspControl-display615-fb-rowbytes",15360ULL,32);
    return true;
}

// 0.180.141: NVDisplay's hardware cursor on the native615 head. op 0 probe,
// 1 image (p1 = 64x64 ARGB, straight alpha -> premultiplied for the head's
// 0x72ff composition, written to the top-left of the 256x256 surface), 2
// move (p1 = x, p2 = y), 3 show (p1 != 0).
// 0.180.143: NVDisplay probes at its start and WindowServer decides on a
// hardware cursor once, both before user space brings up the head (op 10).
// So the probe succeeds as soon as the native615 framebuffer exists; image
// and position are kept and op 10 writes them (displayCursorWrite615).
IOReturn NVGspControl::displayCursorPosition615() {
    IOMemoryMap *map=pci_ ? sharedBar0Map(pci_) : nullptr;
    bool ok=false;
    if(map && displayCursorUser615_ && map->getLength()>=displayCursorUser615_+0x210) {
        Bar0Io bar0{map};
        // a hidden cursor parks outside the 4000x2222 raster
        const bool visible=displayCursorVisible615_;
        const UInt32 px=visible ? UInt32(displayCursorX615_)&0xffff : 0x3fff;
        const UInt32 py=visible ? UInt32(displayCursorY615_)&0xffff : 0x3fff;
        ok=bar0.write(displayCursorUser615_+0x208,(py<<16)|px) && bar0.write(displayCursorUser615_+0x200,1);
    }
    if(map)map->release();
    return ok ? kIOReturnSuccess : kIOReturnIOError;
}

IOReturn NVGspControl::displayCursorWrite615() {
    if(!displayCursorImage615_ || !displayCursorImageValid615_ || !displayCursorSurface615_)return kIOReturnSuccess;
    // 0.180.169: only the rows/columns the new or the previous image used
    // (the rest of the 256x256 surface is already transparent)
    const UInt32 w=displayCursorW615_>displayCursorWroteW615_ ? displayCursorW615_ : displayCursorWroteW615_;
    const UInt32 h=displayCursorH615_>displayCursorWroteH615_ ? displayCursorH615_ : displayCursorWroteH615_;
    IOReturn r=kIOReturnSuccess;
    for(UInt32 y=0;y<h && r==kIOReturnSuccess;++y)
        r=vramAccess(displayCursorSurface615_+UInt64(y)*1024,displayCursorImage615_+y*256,w,true,true);
    if(r==kIOReturnSuccess) {displayCursorWroteW615_=displayCursorW615_;displayCursorWroteH615_=displayCursorH615_;}
    setProperty("NVGspControl-display615-cursor-image",UInt32(r),32);
    setProperty("NVGspControl-display615-cursor-size",displayCursorW615_|displayCursorH615_<<16,32);
    return r;
}

IOReturn NVGspControl::displayCursor615(UInt32 op, void *p1, void *p2) {
    if(!lock_ || !pci_ || (!displayNative615_ && !displayFb615_))return kIOReturnNotReady;
    if(op==0) {setProperty("NVGspControl-cursor-max",256U,32);return kIOReturnSuccess;}   // 0.180.169
    if(op==2 || op==3) {
        lk(__LINE__);
        if(op==2) {
            displayCursorX615_=SInt32(reinterpret_cast<intptr_t>(p1));
            displayCursorY615_=SInt32(reinterpret_cast<intptr_t>(p2));
        } else displayCursorVisible615_=p1!=nullptr;
        const bool native=displayNative615_;
        ulk();
        return native ? displayCursorPosition615() : kIOReturnSuccess;
    }
    if(op!=1 || !p1)return kIOReturnBadArgument;
    // 0.180.169: p2 = used size (w | h << 16) of a 256-pitch image; nullptr = a
    // 64x64 image (NVDisplayFB2)
    const UInt32 size=UInt32(reinterpret_cast<uintptr_t>(p2));
    const UInt32 pitch=p2 ? 256 : 64,cw=p2 ? (size&0xffff) : 64,ch=p2 ? (size>>16) : 64;
    if(!cw || !ch || cw>256 || ch>256)return kIOReturnBadArgument;
    if(!displayCursorImage615_) {
        auto *b=static_cast<UInt32 *>(IOMallocZero(256*256*4));
        if(!b)return kIOReturnNoMemory;
        lk(__LINE__);
        if(!displayCursorImage615_) {displayCursorImage615_=b;b=nullptr;}
        ulk();
        if(b)IOFree(b,256*256*4);
    }
    lk(__LINE__);
    const bool native=displayNative615_;
    if(native && (!native615_ || !native615Ready_ || native615Stopping_ || !waitIdle615())) {ulk();return kIOReturnBusy;}
    if(native)native615Busy_=true;
    ulk();
    const auto *src=static_cast<const UInt32 *>(p1);
    // the part the previous image used is cleared, the new one premultiplied
    for(UInt32 y=0;y<displayCursorH615_;++y)bzero(displayCursorImage615_+y*256,displayCursorW615_*4);
    for(UInt32 y=0;y<ch;++y)
        for(UInt32 x=0;x<cw;++x) {
            const UInt32 v=src[y*pitch+x],a=v>>24;
            displayCursorImage615_[y*256+x]=a==255 ? v : (a<<24)|((((v>>16)&255)*a/255)<<16)|((((v>>8)&255)*a/255)<<8)|((v&255)*a/255);
        }
    displayCursorW615_=cw;displayCursorH615_=ch;
    displayCursorImageValid615_=true;
    const IOReturn r=native ? displayCursorWrite615() : kIOReturnSuccess;
    if(native) {lk(__LINE__);native615Busy_=false;ulk();}
    return r;
}
