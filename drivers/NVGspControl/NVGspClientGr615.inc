// Admin-only fixed second GR client: own VAS, TSG, channel and MAIN/PM/PATCH
// context, root's global GR buffers mapped as primary RM does. It runs only
// the root-proven NAK and stays scheduled; a separate teardown call disables
// and frees it, so root work can run in between. Any failure pins the rest.
IOReturn NVGspControl::runClientGr615(UInt32 phase) {
    static_assert(kClientSlots615_==nvgsp::kClientSlots615,"client slot count");
    static_assert(kClientWraps615_==nvgsp::kClientWraps615,"client wrap count");
    static_assert(sizeof(nativeAllocDatas_[0])/sizeof(nativeAllocDatas_[0][0])==nvgsp::kClientAllocs615,"client allocation slots");
    const bool teardown=phase==1 || phase==3;
    auto *s=native615_;if(!s)return kIOReturnNotReady;
    auto &g=s->clientGrs[client615Slot_];
    constexpr UInt32 C=nvgsp::kLifecycleClient615,D=nvgsp::kLifecycleDevice615;
    constexpr UInt32 S=nvgsp::kLifecycleSubdevice615;
    const UInt32 H=0xc0d00300U|client615Slot_<<16,VAS=H|0xf1,WINDOW=H|0xa0,RING=H|0x57,TSG=H|0x50,SHARE=H|0x51,CHANNEL=H|0x56;   // 0.180.126: no RM TREE object (H|0x5a)
    UInt64 heapLo=0,heapHi=0;
    lk(__LINE__);
    auto *lo=OSDynamicCast(OSNumber,getProperty("NVGspControl-local-memory-heap-base"));
    auto *hi=OSDynamicCast(OSNumber,getProperty("NVGspControl-local-memory-heap-limit"));
    if(lo)heapLo=lo->unsigned64BitValue();if(hi)heapHi=hi->unsigned64BitValue();
    ulk();if(!heapLo || heapHi<heapLo)return kIOReturnNotReady;
    auto step=[&](UInt32 value) {
        lk(__LINE__);g.stage=value;if(value<16)g.rcAtStage[value]=rcEvents_;
        setProperty("NVGspControl-client615-stage",value,32);
        setProperty("NVGspControl-client615-rc-at-stage",g.rcAtStage,sizeof(g.rcAtStage));ulk();
    };
    auto fail=[&](UInt32 value) {lk(__LINE__);g.status=value;ulk();return false;};
    auto publishLedger=[&]() {
        lk(__LINE__);
        setProperty("NVGspControl-client615-ledger",g.objects,g.objectCount*4);
        setProperty("NVGspControl-client615-object-count",g.objectCount,32);
        setProperty("NVGspControl-client615-freed-count",g.freedCount,32);
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
        if(result || !reply.successful()) {
            UInt32 why[4]={object,command,result,reply.header.status};   // 0.180.125
            lk(__LINE__);setProperty("NVGspControl-client615-fail-control",why,sizeof(why));ulk();
            return fail(result?result:reply.header.status?reply.header.status:kIOReturnError);
        }
        if(size)__builtin_memcpy(params,reply.params,size);return true;
    };
    // 0.180.179: compute preemption of an owned channel. CTA (1) cannot switch
    // away from a CTA that never ends, so a hung app froze the desktop until
    // the 5 s timeout. The admin runtime flag NVGspControl-cilp-request (lost on
    // reboot) asks new channels for CILP (2); a refusal falls back to CTA.
    auto preemptCompute=[&]() {
        UInt32 mode=1;
        if(cilpRequest_) {
            UInt32 cilp[8]={1,CHANNEL,0,2,0,0,0,0};const UInt32 before=g.status;
            const bool ok=ctl(S,0x20801210,cilp,sizeof(cilp));
            lk(__LINE__);setProperty("NVGspControl-client615-cilp-status",ok?0U:g.status,32);ulk();
            if(ok)mode=2; else g.status=before;
        }
        UInt32 cta[8]={1,CHANNEL,0,1,0,0,0,0};
        if(mode==1 && !ctl(S,0x20801210,cta,sizeof(cta)))return false;
        lk(__LINE__);setProperty("NVGspControl-client615-compute-preempt",mode,32);ulk();
        return true;   // callers check health with the next allocation
    };
    // The maintenance context keeps its own ledger (never freed before cold stop).
    UInt32 *ledger=g.objects,*ledgerCount=&g.objectCount,ledgerMax=32;
    auto alloc=[&](UInt32 parent,UInt32 object,UInt32 cls,void *params,UInt32 size) {
        if(*ledgerCount>=ledgerMax || !nvgsp::buildAlloc615(s->request,sizeof(s->request),parent,object,cls,params,size))return fail(kIOReturnBadArgument);
        ledger[(*ledgerCount)++]=object;publishLedger(); // pre-enqueue, uncertain handles remain pinned
        UInt32 bytes=sizeof(s->reply),result=~0U;
        const IOReturn r=userRpcImpl(103,s->request,32+size,s->reply,&bytes,&result,true,true);
        if(r!=kIOReturnSuccess)return fail(static_cast<UInt32>(r));
        const UInt8 *returned=nullptr;
        const auto reply=nvgsp::decodeAlloc615(s->reply,bytes,parent,object,cls,size,&returned);
        if(bytes!=88+size || !reply.valid)return fail(kIOReturnBadMessageID);
        if(result || !reply.successful()) {
            UInt32 why[5]={parent,object,cls,result,reply.status};   // 0.180.125
            lk(__LINE__);setProperty("NVGspControl-client615-fail-alloc",why,sizeof(why));ulk();
            return fail(result?result:reply.status?reply.status:kIOReturnError);
        }
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
        for(UInt32 i=0;i<g.objectCount;++i)if(g.objects[i]==object) {g.objects[i]=0;++g.freedCount;publishLedger();return true;}
        return fail(kIOReturnInternalError);
    };
    auto read=[&](UInt64 at,UInt32 count) {return vramAccess(at,s->readWords,count,false,true)==kIOReturnSuccess;};
    auto write=[&](UInt64 at,const UInt32 *words,UInt32 count) {
        if(!count || count>1024)return false;
        if(words!=s->writeWords)__builtin_memcpy(s->writeWords,words,count*4);
        return vramAccess(at,s->writeWords,count,true,true)==kIOReturnSuccess && read(at,count) &&
            !__builtin_memcmp(s->writeWords,s->readWords,count*4);
    };
    // 0.180.121: zero a page with one write and check its first and last 16
    // words, instead of reading every word back (the read-back was most of a
    // client's 5 s creation)
    auto zero=[&](UInt64 base,UInt64 bytes) {
        // 0.180.132: 64 KiB aligned regions (the client reservation, its page
        // tree) by the maintenance channel's copy engine once that runs
        if(!(base&0xffff) && !(bytes&0xffff) && s->mmu.live)return mmuZero615(base,bytes);
        bzero(s->writeWords,sizeof(s->writeWords));
        for(UInt64 page=0;page<bytes;page+=4096) {
            if(vramAccess(base+page,s->writeWords,1024,true,true)!=kIOReturnSuccess)return false;
            const UInt64 ends[2]={base+page,base+page+4032};
            for(UInt64 at:ends) {
                if(!read(at,16))return false;
                for(unsigned i=0;i<16;++i)if(s->readWords[i])return false;
            }
        }
        return true;
    };
    // 0.180.122: two 10 ms status polls (was five of 100 ms: 0.5 s a check,
    // twelve checks made a Metal process wait 2.5 s to start and 2.5 s to end)
    auto healthy=[&]() {
        for(unsigned i=0;i<2;++i) {
            IOSleep(10);if(pollStatus()!=kIOReturnSuccess)return false;
            lk(__LINE__);
            const bool ok=!gmcMmuFaultPending_ && !userRpcLatePending_ && !resetBusy_ && !sleeping_ &&
                !native615Stopping_ && postInitPhase_==27 && s->generation==resetGeneration();
            auto *bad=OSDynamicCast(OSNumber,getProperty("NVGspControl-status-bad-reason"));
            const bool reason=bad && bad->unsigned32BitValue();ulk();if(!ok || reason)return false;
        }
        return true;
    };
    auto rootIntact=[&]() {
        UInt64 pte=0;PraminPteResult access{};
        return read(s->gpfifo+0xe00,1) && s->readWords[0]==s->fenceExpected &&
            read(s->userd+0x88,2) && s->readWords[0]==s->put && s->readWords[1]==s->put &&
            praminPteAccess(pci_,s->firstGrPteAddress,&pte,false,false,&access) && pte==s->firstGrPteValue;
    };
    // MEM_OP TLB invalidate PDB_ALL (clc56f.h: C bit0 PDB ALL, replay NONE;
    // D MMU_TLB_INVALIDATE) + HOST semaphore on the permanent maintenance
    // channel, built on first use. Its single ring page never changes.
    auto mmuInvalidate=[&](UInt32 memOpC) __attribute__((noinline)) {
        auto &m=s->mmu;
        constexpr UInt32 M=0xc0d00500,MVAS=M|0xf1,MTREE=M|0x5a,MTSG=M|0x50,MSHARE=M|0x51,MCH=M|0x56,MCOPY=M|0x58;
        if(m.attempted && !m.live)return false;
        if(!m.attempted) {
            m.attempted=true;
            UInt32 *const savedLedger=ledger,*const savedCount=ledgerCount;const UInt32 savedMax=ledgerMax;
            UInt64 pipeLo=0;
            ledger=m.objects;ledgerCount=&m.objectCount;ledgerMax=12;
            auto build=[&]() __attribute__((noinline)) {
                nvgsp::VaspaceParams615 vas{};vas.flags=nvgsp::kExternalFaultVasFlags615;
                if(!alloc(D,MVAS,0x90f1,&vas,sizeof(vas)) || vas.flags!=nvgsp::kExternalFaultVasFlags615 || !vas.vaSize ||
                   vas.vaSize>UINT64_MAX-vas.vaBase)return false;
                m.va=s->grVa-8192;
                // 0.180.197: the tree also covers kPipeSlots615 x kPipeSlotBytes615 (80 MiB since 0.180.202) from the next 2 MiB
                // boundary for display pipe surfaces (pipeFlip615), when that fits the VAS
                const UInt64 pLo=((m.va>>21)+1)<<21,pHi=pLo+UInt64(kPipeSlots615)*kPipeSlotBytes615;
                const bool pFits=(m.va>>38)==((pHi-1)>>38) && pHi<=vas.vaBase+vas.vaSize;
                const UInt64 lo=m.va,hi=pFits ? pHi : m.va+4096;const UInt32 tables=nvgsp::externalTreeTables615(lo,hi);
                pipeLo=pFits ? pLo : 0;
                if(lo<vas.vaBase || hi>vas.vaBase+vas.vaSize || !tables)return fail(kIOReturnBadArgument);
                nvgsp::MemoryParams615 t{};t.owner=C;t.type=6;t.flags=0x100;t.attr=0x800000;
                t.size=nvgsp::externalTreeBytes615(lo,hi);t.alignment=4096;const UInt64 size=t.size;   // 0.180.118
                if(!alloc(S,MTREE,0x40,&t,sizeof(t)) || t.size!=size ||
                   !nvgsp::memoryProbeRange615(t,gmcConfig_.fbBytes,heapLo,heapHi))return false;
                if(!zero(t.offset,size) || !nvgsp::externalTreePlan615(t.offset,lo,hi,&m.tree))return fail(kIOReturnBadArgument);
                auto pde=[&](UInt64 at,UInt64 value) {const UInt32 w[2]={UInt32(value),UInt32(value>>32)};return write(at,w,2);};
                for(unsigned i=0;i<m.tree.upperCount;++i)if(!pde(m.tree.upperAddress[i],m.tree.upperValue[i]))return false;
                for(UInt32 i=0;i<m.tree.tables;++i)if(!pde(m.tree.pd0Address[i],m.tree.pd0Value[i]))return false;
                nvgsp::SetPageDirectory615 directory{m.tree.tree,4,8,MVAS,0,1,0};
                if(!ctl(D,0x801813,&directory,sizeof(directory)) || directory.physical!=m.tree.tree ||
                   directory.vas!=MVAS || directory.channel || directory.subdevice!=1)return false;
                UInt32 methodBytes=0;
                if(!ctl(S,0x20802a08,&methodBytes,sizeof(methodBytes)) || !methodBytes || methodBytes>16*1024*1024)return fail(kIOReturnBadArgument);
                const UInt64 mBytes=(UInt64(methodBytes)+4095)&~4095ULL;
                UInt64 *backings[4]={&m.instance,&m.method,&m.userd,&m.ring};
                for(UInt32 i=0;i<4;++i) {
                    nvgsp::MemoryParams615 p{};p.owner=C;p.type=6;p.flags=0x100;p.attr=0x800000;
                    p.size=i==1?mBytes:4096;p.alignment=4096;const UInt64 bytes=p.size;
                    if(!alloc(S,M|(0x52+i),0x40,&p,sizeof(p)))return false;
                    if(!nvgsp::memoryProbeRange615(p,gmcConfig_.fbBytes,heapLo,heapHi) || p.size!=bytes)return fail(kIOReturnBadArgument);
                    *backings[i]=p.offset;if(!zero(p.offset,bytes))return false;
                }
                UInt64 leaf=0,pte=0;
                if(!nvgsp::externalTreeLeaf615(m.tree,m.va,&leaf) || !nvgsp::vidPte615(m.ring,false,false,&pte) ||
                   !read(leaf,2) || s->readWords[0] || s->readWords[1])return fail(kIOReturnBadArgument);
                const UInt32 pteWords[2]={UInt32(pte),UInt32(pte>>32)};
                if(!write(leaf,pteWords,2))return false;
                nvgsp::ChannelGroup615 group{};group.hVASpace=MVAS;group.engineType=9;
                nvgsp::ContextShare615 share{MVAS,0,~0U};
                if(!alloc(D,MTSG,0xa06c,&group,sizeof(group)) || !alloc(MTSG,MSHARE,0x9067,&share,sizeof(share)) ||
                   share.hVASpace!=MVAS || share.flags || share.subctxId==~0U)return false;
                // Admin maintenance channel: bits 7/22 clear, internal privilege ADMIN.
                nvgsp::ChannelAlloc615 ch{};
                ch.gpFifoOffset=m.va;ch.gpFifoEntries=256;ch.flags=0x00000040;
                ch.hContextShare=MSHARE;ch.hUserdMemory[0]=M|0x54;ch.engineType=9;
                ch.instanceMem={m.instance,4096,2,1};ch.userdMem={m.userd,4096,2,1};
                ch.ramfcMem={m.instance,512,2,1};ch.mthdbufMem={m.method,mBytes,2,0};
                ch.hPhysChannelGroup=MTSG;ch.internalFlags=0x15;
                if(!alloc(MTSG,MCH,0xc56f,&ch,sizeof(ch)) || ch.flags!=0x00000040 || ch.internalFlags!=0x15)return false;
                UInt32 copy[2]={1,9};
                if(!alloc(MCH,MCOPY,0xc7b5,copy,8) || copy[0]!=1 || copy[1]!=9)return false;
                UInt32 engine=9,token=0;UInt8 schedule[3]={1,0,0};
                if(!ctl(MCH,0xa06f0104,&engine,4) || !ctl(MCH,0xc36f0108,&token,4) ||
                   !ctl(MCH,0xa06f0103,schedule,sizeof(schedule)))return false;
                if(!read(m.instance+0xf4,1))return false;
                const UInt32 ramfc=s->readWords[0];
                lk(__LINE__);setProperty("NVGspControl-mmu615-ramfc-config",ramfc,32);ulk();
                return (ramfc&0x100)!=0 || fail(kIOReturnNotPrivileged); // never kick without privilege
            };
            const bool built=build();
            ledger=savedLedger;ledgerCount=savedCount;ledgerMax=savedMax;
            lk(__LINE__);m.live=built;mmuKickToken615_=0;   // 0.180.164: a rebuilt channel has a new work-submit token
            pipeVa615_=built ? pipeLo : 0;setProperty("NVGspControl-flipcopy-native615",pipeVa615_ ? 1U : 0U,32);
            setProperty("NVGspControl-mmu615-live",built);
            setProperty("NVGspControl-mmu615-objects",m.objects,m.objectCount*4);ulk();
            if(!built)return false;
        }
        return mmuSubmit615(memOpC);
    };
    // Writes client PTEs into RM's exact managed leaf, one bounded chunk at a
    // time; every chunk is recorded so teardown clears exactly what was set.
    nvgsp::ClientGrLayout615 &layout=s->clientLayout;layout={};
    auto map=[&](UInt64 va,UInt64 bytes,bool ring) {
        for(UInt64 done=0;done<bytes;) {
            UInt8 query[208]{};const UInt64 at=va+done;
            __builtin_memcpy(query,&at,8);__builtin_memcpy(query+200,&g.vas,4);
            if(!ctl(D,0x801809,query,sizeof(query)))return false;
            UInt64 base=0;UInt32 coverage=0,matches=0;
            for(unsigned i=0;i<5;++i) {
                const UInt8 *q=query+32+i*32;if(nvgsp::control615U32(q+16)!=4096)continue;
                __builtin_memcpy(&base,q,8);coverage=nvgsp::control615U32(q+24);++matches;
                if(nvgsp::control615U32(q+12)!=8 || nvgsp::control615U32(q+20))return fail(kIOReturnBadArgument);
            }
            if(matches!=1 || coverage<4096 || (coverage&(coverage-1)) || base<heapLo || base>heapHi ||
               UInt64(coverage)/4096*8-1>heapHi-base || g.leafChunks>=nvgsp::kClientLeafChunks615)return fail(kIOReturnBadArgument);
            UInt64 count=(bytes-done)/4096;if(count>256)count=256;
            if(count>(coverage-at%coverage)/4096)count=(coverage-at%coverage)/4096;
            if(!count)return fail(kIOReturnBadArgument);
            const UInt64 address=base+(at%coverage)/4096*8;
            if(!read(address,UInt32(count*2)))return false;
            for(UInt32 i=0;i<count*2;++i)if(s->readWords[i])return fail(kIOReturnBusy);
            for(UInt32 i=0;i<count;++i) {
                UInt64 physical=0,value=0;bool privileged=false;
                const UInt64 offset=done+i*4096;
                const bool present=ring ? (physical=g.gpfifo+offset,true) :
                    nvgsp::clientGrPage615(s->gr,layout,s->physical,g.physical,offset,&physical,&privileged);
                if(present && !nvgsp::vidPte615(physical,privileged,false,&value))return fail(kIOReturnBadArgument);
                if(present) {++g.mappedPages;if(privileged)++g.privilegedPages;}
                s->writeWords[i*2]=UInt32(value);s->writeWords[i*2+1]=UInt32(value>>32);
            }
            g.leafAddress[g.leafChunks]=address;g.leafCount[g.leafChunks++]=UInt32(count);
            if(!write(address,s->writeWords,UInt32(count*2)))return false;
            UInt32 invalidate[2]={g.vas,0};if(!ctl(D,0x80180c,invalidate,sizeof(invalidate)))return false;
            done+=count*4096;
        }
        return true;
    };
    if(phase==2) {
        // Cancel this client's pending REPLAYABLE faults the way UVM does:
        // a privileged channel issues MEM_OP TLB invalidate REPLAY_CANCEL_GLOBAL
        // (clc56f.h), then the owned buffer GET is advanced to PUT. Only the
        // failed external negative client reaches here; nothing is replayed.
        constexpr UInt32 M=0xc0d00400,MTSG=M|0x50,MSHARE=M|0x51,MCH=M|0x56,MCOPY=M|0x58;
        constexpr UInt32 kReplayGet=0x100E40,kReplayPut=0x100E44,kReplaySize=0x100E48;
        step(14);
        IOMemoryMap *bm=sharedBar0Map(pci_);
        UInt32 get=~0U,put=~0U,size=0;
        bool regs=bm && bm->getLength()>=0x100E4C;
        if(regs) {Bar0Io io{bm};regs=io.read(kReplayGet,&get) && io.read(kReplayPut,&put) && io.read(kReplaySize,&size);}
        if(bm)bm->release();
        const UInt32 entries=size&0xfffff,getPtr=get&0xfffff,putPtr=put&0xfffff;
        const UInt32 pending=entries ? (putPtr+entries-getPtr)%entries : 0;
        lk(__LINE__);
        UInt32 before[3]={get,put,size};
        setProperty("NVGspControl-client615-cancel-replay-regs-before",before,sizeof(before));ulk();
        if(!regs || !(size>>31) || !entries || UInt64(entries)*32>nativeReplayData_.size() ||
           getPtr>=entries || putPtr>=entries || !pending || pending>8)return kIOReturnNotReady;
        for(UInt32 i=0;i<pending;++i) {
            UInt32 w[8]{};
            if(!nativeReplayData_.readDeviceRange(UInt64((getPtr+i)%entries)*32,w,32))return kIOReturnIOError;
            const UInt64 inst=(UInt64(w[1])<<32)|(w[0]&0xfffff000U);
            // Every pending record must be this client's own valid fault.
            if(!(w[7]>>31) || inst!=g.instance)return kIOReturnBadMessageID;
        }
        const UInt64 mVa=g.gpfifoVa-4096;UInt64 mLeaf=0;
        if(!g.external || !nvgsp::externalTreeLeaf615(g.tree,mVa,&mLeaf) || g.leafChunks>=nvgsp::kClientLeafChunks615)return kIOReturnBadArgument;
        nvgsp::ChannelGroup615 group{};group.hVASpace=VAS;group.engineType=9;
        nvgsp::ContextShare615 share{VAS,0,~0U};
        if(!alloc(D,MTSG,0xa06c,&group,sizeof(group)) || !alloc(MTSG,MSHARE,0x9067,&share,sizeof(share)) ||
           share.hVASpace!=VAS || share.flags || share.subctxId==~0U)return kIOReturnNotReady;
        UInt64 mInstance=0,mMethod=0,mUserd=0;const UInt64 mGpfifo=g.maintGpfifo;
        UInt64 *backings[3]={&mInstance,&mMethod,&mUserd};
        for(UInt32 i=0;i<3;++i) {
            nvgsp::MemoryParams615 p{};p.owner=C;p.type=6;p.flags=0x100;p.attr=0x800000;
            p.size=i==1?g.methodBytes:4096;p.alignment=4096;const UInt64 bytes=p.size;
            if(!alloc(S,M|(0x52+i),0x40,&p,sizeof(p)))return kIOReturnNotReady;
            if(!nvgsp::memoryProbeRange615(p,gmcConfig_.fbBytes,heapLo,heapHi) || p.size!=bytes)return kIOReturnBadArgument;
            *backings[i]=p.offset;if(!zero(p.offset,bytes))return kIOReturnIOError;
        }
        // The ring PTE was written at setup; confirm it is still exactly ours.
        UInt64 pte=0;
        if(!mGpfifo || !nvgsp::vidPte615(mGpfifo,false,false,&pte) || !read(mLeaf,2) ||
           s->readWords[0]!=UInt32(pte) || s->readWords[1]!=UInt32(pte>>32))return kIOReturnBadArgument;
        // Admin maintenance channel: bits 7/22 clear, internal privilege ADMIN.
        nvgsp::ChannelAlloc615 ch{};
        ch.gpFifoOffset=mVa;ch.gpFifoEntries=256;ch.flags=0x00000040;
        ch.hContextShare=MSHARE;ch.hUserdMemory[0]=M|0x54;ch.engineType=9;
        ch.instanceMem={mInstance,4096,2,1};ch.userdMem={mUserd,4096,2,1};
        ch.ramfcMem={mInstance,512,2,1};ch.mthdbufMem={mMethod,g.methodBytes,2,0};
        ch.hPhysChannelGroup=MTSG;ch.internalFlags=0x15;
        if(!alloc(MTSG,MCH,0xc56f,&ch,sizeof(ch)) || ch.flags!=0x00000040 || ch.internalFlags!=0x15)return kIOReturnNotReady;
        UInt32 copy[2]={1,9};
        if(!alloc(MCH,MCOPY,0xc7b5,copy,8) || copy[0]!=1 || copy[1]!=9)return kIOReturnNotReady;
        UInt32 engine=9,token=0;UInt8 schedule[3]={1,0,0};
        if(!ctl(MCH,0xa06f0104,&engine,4) || !ctl(MCH,0xc36f0108,&token,4) ||
           !ctl(MCH,0xa06f0103,schedule,sizeof(schedule)))return kIOReturnNotReady;
        if(!read(mInstance+0xf4,1))return kIOReturnIOError;
        const UInt32 ramfc=s->readWords[0];
        lk(__LINE__);setProperty("NVGspControl-client615-cancel-ramfc-config",ramfc,32);ulk();
        if(!(ramfc&0x100))return kIOReturnNotPrivileged; // never kick without privilege
        step(15);
        constexpr UInt32 fence=0x6150cace;
        const UInt64 semaphore=mVa+0xe00,pb=mVa+0x800;
        const UInt32 words[11]={0x2004000a,0,0,0x00000011,0x48000000,
            0x20050017,UInt32(semaphore)&0xfffffffc,UInt32(semaphore>>32)&0xff,fence,0,0x00100001};
        const UInt32 entry[2]={UInt32(pb)&0xfffffffc,(UInt32(pb>>32)&0xff)|(11U<<10)};
        const UInt32 one=1;
        lk(__LINE__);g.cancelChannel=true;ulk();
        if(!write(mGpfifo+0x800,words,11) || !write(mGpfifo,entry,2) || !write(mUserd+0x8c,&one,1) ||
           diagnosticChannelKick(MCH,true)!=kIOReturnSuccess)return kIOReturnIOError;
        UInt64 deadline=0;clock_interval_to_deadline(5,kSecondScale,&deadline);bool done=false;
        do {
            if(!read(mGpfifo+0xe00,1))return kIOReturnIOError;
            if(s->readWords[0]==fence) {done=true;break;}
            IOSleep(1);
        } while(mach_absolute_time()<deadline);
        lk(__LINE__);setProperty("NVGspControl-client615-cancel-fence-done",done);ulk();
        if(!done)return kIOReturnTimeout;
        // Consume exactly the records validated above.
        bm=sharedBar0Map(pci_);bool consumed=bm && bm->getLength()>=0x100E4C;
        UInt32 after[3]={};
        if(consumed) {
            Bar0Io io{bm};
            consumed=io.write(kReplayGet,putPtr) && io.read(kReplayGet,&after[0]) &&
                io.read(kReplayPut,&after[1]) && io.read(kReplaySize,&after[2]);
        }
        if(bm)bm->release();
        lk(__LINE__);setProperty("NVGspControl-client615-cancel-replay-regs-after",after,sizeof(after));ulk();
        if(!consumed || (after[0]&0xfffff)!=putPtr)return kIOReturnIOError;
        IOSleep(500);
        lk(__LINE__);g.cancelled=true;ulk();
        step(15);return kIOReturnSuccess;
    }
    if(phase==4) {
        // 0.180.100: an owned client's dispatch stalled on REPLAYABLE faults.
        // Every pending record must be this client's; then the permanent
        // privileged channel cancels them globally and GET is advanced to PUT.
        step(14);
        IOMemoryMap *bm=sharedBar0Map(pci_);
        UInt32 rr[3]={~0U,~0U,0};
        bool ok=g.userBuffer && bm && bm->getLength()>=0x100E4C;
        if(ok) {Bar0Io io{bm};ok=io.read(0x100E40,&rr[0]) && io.read(0x100E44,&rr[1]) && io.read(0x100E48,&rr[2]);}
        if(bm)bm->release();
        lk(__LINE__);setProperty("NVGspControl-client615-owned-fault-regs-before",rr,sizeof(rr));ulk();
        const UInt32 entries=rr[2]&0xfffff,getPtr=rr[0]&0xfffff,putPtr=rr[1]&0xfffff;
        const UInt32 pending=entries ? (putPtr+entries-getPtr)%entries : 0;
        if(!ok || !(rr[2]>>31) || !entries || UInt64(entries)*32>nativeReplayData_.size() ||
           getPtr>=entries || putPtr>=entries)return kIOReturnNotReady;
        if(!pending)return kIOReturnNoResources;   // 0.180.108: stalled without faults (a hang)
        auto ours=[&](UInt32 from,UInt32 count) {
            for(UInt32 i=0;i<count;++i) {
                UInt32 w[8]{};
                if(!nativeReplayData_.readDeviceRange(UInt64((from+i)%entries)*32,w,32) || !(w[7]>>31) ||
                   ((UInt64(w[1])<<32)|(w[0]&0xfffff000U))!=g.instance)return false;
            }
            return true;
        };
        if(!ours(getPtr,pending))return kIOReturnBadMessageID;
        if(!mmuInvalidate(0x11))return kIOReturnTimeout;
        UInt32 finalPut=putPtr,late=0;
        bm=sharedBar0Map(pci_);ok=bm && bm->getLength()>=0x100E4C;
        if(ok) {Bar0Io io{bm};ok=io.read(0x100E44,&rr[1]);}
        if(ok) {
            finalPut=rr[1]&0xfffff;late=(finalPut+entries-putPtr)%entries;
            ok=finalPut<entries && late<entries-pending && ours(putPtr,late);
        }
        if(ok) {Bar0Io io{bm};ok=io.write(0x100E40,finalPut) && io.read(0x100E40,&rr[0]) && io.read(0x100E44,&rr[1]);}
        if(bm)bm->release();
        lk(__LINE__);setProperty("NVGspControl-client615-owned-fault-regs-after",rr,sizeof(rr));
        setProperty("NVGspControl-client615-owned-fault-records",pending+late,32);
        setProperty("NVGspControl-client615-owned-fault-late-records",late,32);ulk();
        if(!ok || (rr[0]&0xfffff)!=finalPut)return kIOReturnIOError;
        IOSleep(500);
        lk(__LINE__);g.cancelled=true;ulk();
        step(15);return kIOReturnSuccess;
    }
    if(phase==5) {
        // 0.180.178: the owner's work hung (no fault, no RC). Rebuild only its
        // channel (TSG, context share, channel, context image, engine objects).
        // VAS, page tree, caller buffer, allocations and wraps stay mapped at
        // their VAs, so the owner's resources outlive the hang; it used to lose
        // the whole client and every later submission failed.
        // Stage 16: nothing freed yet (the caller may still tear down the
        // normal way); 17: the old channel is being freed; 18: rebuilt.
        UInt32 codeBytes=0,registers=1;
        if(!g.live || !g.scheduled || !g.external || !g.userBuffer || g.subcontext || g.negative || g.touchOnly ||
           !healthy() || !rootIntact() || !nvgsp::computeNak615(s->provenNak,s->provenNakBytes,&codeBytes,&registers) ||
           !nvgsp::clientGrLayout615(s->gr,&layout,g.localGlobals,g.subcontext))return kIOReturnNotReady;
        step(16);
        UInt8 off[3]={0,0,0};
        if(!ctl(CHANNEL,0xa06f0103,off,sizeof(off)) || !healthy())return kIOReturnNotReady;
        step(17);g.scheduled=false;
        if((g.copyEngine && !release(H|0x5b)) || !release(H|0x59) || !release(H|0x58) || !release(CHANNEL) ||
           !release(SHARE) || !release(TSG) || !healthy())return kIOReturnNotReady;
        auto grStatus=[&]() {
            IOMemoryMap *bm=sharedBar0Map(pci_);UInt32 v=~0U;
            if(bm && bm->getLength()>0x400704) {Bar0Io io{bm};if(!io.read(0x400700,&v))v=~0U;}
            if(bm)bm->release();return v;
        };
        UInt32 gr=grStatus();
        for(unsigned w=0;w<20 && gr;++w) {IOSleep(50);gr=grStatus();}
        lk(__LINE__);setProperty("NVGspControl-client615-reset-gr-idle",gr,32);ulk();
        if(gr)return kIOReturnBusy;
        // freed handles leave the ledger (20 entries; every reset re-adds six)
        UInt32 kept=0;
        for(UInt32 i=0;i<g.objectCount;++i)if(g.objects[i])g.objects[kept++]=g.objects[i];
        g.freedCount-=g.objectCount-kept;g.objectCount=kept;publishLedger();
        if(!zero(g.instance,4096) || !zero(g.method,g.methodBytes) || !zero(g.userd,4096) || !zero(g.gpfifo,4096) ||
           !zero(g.physical,layout.clientBytes))return kIOReturnIOError;
        nvgsp::ChannelGroup615 group{};group.hVASpace=g.vas;group.engineType=1;
        nvgsp::ContextShare615 share{g.vas,0,~0U};
        if(!alloc(D,TSG,0xa06c,&group,sizeof(group)) || !alloc(TSG,SHARE,0x9067,&share,sizeof(share)) ||
           share.hVASpace!=g.vas || share.flags || share.subctxId==~0U)return kIOReturnNotReady;
        nvgsp::ChannelAlloc615 ch{};
        ch.gpFifoOffset=g.gpfifoVa;ch.gpFifoEntries=256;ch.flags=0x004000c0U;
        ch.hContextShare=SHARE;ch.hUserdMemory[0]=H|0x54;ch.engineType=1;
        ch.instanceMem={g.instance,4096,2,1};ch.userdMem={g.userd,4096,2,1};
        ch.ramfcMem={g.instance,512,2,1};ch.mthdbufMem={g.method,g.methodBytes,2,0};
        ch.hPhysChannelGroup=TSG;ch.internalFlags=0x14;
        if(!alloc(TSG,CHANNEL,0xc56f,&ch,sizeof(ch)) || ch.internalFlags!=0x14 || ch.flags!=0x004000c0U)return kIOReturnNotReady;
        g.cid=ch.cid;
        UInt8 *const promote=s->clientPromote;constexpr UInt32 promoteBytes=sizeof(s->clientPromote);
        bzero(promote,promoteBytes);
        if(!nvgsp::promoteClientGrBlob615(s->gr,layout,C,CHANNEL,s->physical,g.physical,g.va,promote,promoteBytes,false))
            return kIOReturnBadArgument;
        if(!ctl(S,0x2080012b,promote,promoteBytes) || !healthy() || !preemptCompute())return kIOReturnNotReady;
        UInt32 grParams[4]={2,0,16,0};
        if(!alloc(CHANNEL,H|0x58,0xc997,grParams,sizeof(grParams)) || grParams[0]!=2 || grParams[1] || grParams[2]!=16)
            return kIOReturnNotReady;
        grParams[3]=0;
        if(!alloc(CHANNEL,H|0x59,0xc9c0,grParams,sizeof(grParams)) || grParams[0]!=2 || grParams[1] || grParams[2]!=16)
            return kIOReturnNotReady;
        UInt32 copy[2]={1,g.copyEngine};
        if(g.copyEngine && !alloc(CHANNEL,H|0x5b,0xc7b5,copy,8))return kIOReturnNotReady;
        UInt32 engine=1;UInt8 on[3]={1,0,0};
        if(!healthy() || !ctl(CHANNEL,0xa06f0104,&engine,4) || !ctl(CHANNEL,0xc36f0108,&g.token,4))return kIOReturnNotReady;
        g.scheduled=true;
        if(!ctl(CHANNEL,0xa06f0103,on,sizeof(on)) || !healthy())return kIOReturnNotReady;
        // HOST release, then the class + window setup of a touch-only client.
        // Its fence is the last submitted sequence's, so a wait for any earlier
        // (lost) sequence ends at once; the hung one already returned Aborted.
        const UInt32 touchFence=0x6150c800U+g.dispatches;
        const UInt64 semaphore=g.gpfifoVa+0xe00,pb=g.gpfifoVa+0x800;
        auto submit=[&](UInt32 words,UInt32 put,UInt32 fence) {
            const UInt32 entry[2]={UInt32(pb)&0xfffffffc,(UInt32(pb>>32)&0xff)|(words<<10)};
            if(!write(g.gpfifo+UInt64(put-1)*8,entry,2) || !write(g.userd+0x8c,&put,1) ||
               diagnosticChannelKick(CHANNEL,true)!=kIOReturnSuccess)return false;
            UInt64 deadline=0;clock_interval_to_deadline(5,kSecondScale,&deadline);
            do {
                if(!read(g.gpfifo+0xe00,1))return false;
                if(s->readWords[0]==fence)return read(g.userd+0x88,2) && s->readWords[0]==put && s->readWords[1]==put;
                IOSleep(1);
            } while(mach_absolute_time()<deadline);
            return false;
        };
        const UInt32 host[6]={0x20050017,UInt32(semaphore)&0xfffffffc,UInt32(semaphore>>32)&0xff,0x6150c3a0,0,0x00100001};
        if(!write(g.gpfifo+0x800,host,6) || !submit(6,1,0x6150c3a0))return kIOReturnTimeout;
        const UInt64 va=g.scratchVa;
        if(!nvgsp::computePacket615(va,va+0x8000,va+0x9000,va+0xaf00,registers,0xc0430001,&s->compute))
            return kIOReturnBadArgument;
        constexpr UInt32 touchWords=8;
        __builtin_memcpy(s->writeWords,s->compute.commands,touchWords*4);
        const UInt32 tail[6]={0x20050017,UInt32(semaphore)&0xfffffffc,UInt32(semaphore>>32)&0xff,touchFence,0,0x00100001};
        __builtin_memcpy(s->writeWords+touchWords,tail,sizeof(tail));
        if(!write(g.gpfifo+0x800,s->writeWords,touchWords+6) || !submit(touchWords+6,2,touchFence))return kIOReturnTimeout;
        if(!healthy() || !rootIntact())return kIOReturnNotReady;
        g.put=2;g.fence=touchFence;g.completed=g.dispatches;
        lk(__LINE__);
        setProperty("NVGspControl-client615-cid",g.cid,32);
        setProperty("NVGspControl-client615-token",g.token,32);ulk();
        step(18);return kIOReturnSuccess;
    }
    // 0.180.191: NVDEC0 channel in this client's VAS (phase 6 open, 7 submit). Its ring and
    // falcon context sit in the free pages under the client's maintenance ring, mapped in the
    // owned tree like the client ring (recorded leaf runs are cleared by the normal teardown).
    constexpr UInt32 NTSG=0x60,NSHARE=0x61,NCH=0x66,NCTX=0x67,NOBJ=0x68;
    auto nvdecStep=[&](UInt32 v) {
        lk(__LINE__);g.nvdecStage=v;setProperty("NVGspControl-nvdec615-stage",v,32);ulk();
    };
    if(phase==6) {
        if(!g.live || !g.external || !g.scheduled || !g.userBuffer || g.nvdec || g.negative || g.subcontext || !healthy())
            return kIOReturnNotReady;
      auto open=[&]() __attribute__((noinline)) -> IOReturn {   // own frame: the client GR frame stays < 2 KiB
        const UInt32 savedStatus=g.status;
        g.nvdec=true;nvdecStep(1);
        // An RM refusal (NV_STATUS) means the handle was never made: drop it from the ledger so
        // the client's teardown does not free it.
        auto mk=[&](UInt32 parent,UInt32 object,UInt32 cls,void *params,UInt32 size) {
            g.status=0;
            if(alloc(parent,object,cls,params,size))return true;
            if(g.status && g.status<0x10000 && g.objectCount && g.objects[g.objectCount-1]==object) {
                g.objects[--g.objectCount]=0;publishLedger();
            }
            return false;
        };
        auto done=[&](IOReturn r) {
            lk(__LINE__);g.nvdecStatus=r==kIOReturnSuccess?0:(g.status?g.status:UInt32(r));g.status=savedStatus;
            setProperty("NVGspControl-nvdec615-status",g.nvdecStatus,32);ulk();
            return r;
        };
        UInt32 methodBytes=0;
        if(!ctl(S,0x20802a08,&methodBytes,sizeof(methodBytes)) || !methodBytes || methodBytes>16*1024*1024)
            return done(kIOReturnBadArgument);
        const UInt64 mBytes=(UInt64(methodBytes)+4095)&~4095ULL;
        UInt64 *backings[5]={&g.nvdecInstance,&g.nvdecMethod,&g.nvdecUserd,&g.nvdecGpfifo,&g.nvdecCtx};
        for(UInt32 i=0;i<5;++i) {
            nvgsp::MemoryParams615 p{};p.owner=C;p.type=6;p.flags=0x100;p.attr=0x800000;
            p.size=i==1?mBytes:4096;p.alignment=4096;const UInt64 size=p.size;
            if(!mk(S,H|(i<4?0x62+i:NCTX),0x40,&p,sizeof(p)))return done(kIOReturnNotReady);
            if(!nvgsp::memoryProbeRange615(p,gmcConfig_.fbBytes,heapLo,heapHi) || p.size!=size)return done(kIOReturnBadArgument);
            *backings[i]=p.offset;if(!zero(p.offset,size))return done(kIOReturnIOError);
        }
        nvdecStep(2);
        // 0.180.191 live: the pages under the client ring are outside the owned tree (BadArgument at
        // stage 2), so take 64 KiB of the user window the way a selector-47 allocation does.
        const UInt64 base=(g.userNextVa+0xffff)&~0xffffULL;
        if(base+0x10000>g.userVa+nvgsp::kClientUserWindow615)return done(kIOReturnNoSpace);
        g.userNextVa=base+0x10000;
        g.nvdecRingVa=base;g.nvdecCtxVa=base+4096;
        const UInt64 pageVa[2]={g.nvdecRingVa,g.nvdecCtxVa},pagePhys[2]={g.nvdecGpfifo,g.nvdecCtx};
        for(UInt32 k=0;k<2;++k) {
            UInt64 leaf=0,pte=0;
            if(!nvgsp::externalTreeLeaf615(g.tree,pageVa[k],&leaf) || g.leafChunks>=nvgsp::kClientLeafChunks615 ||
               !nvgsp::vidPte615(pagePhys[k],false,false,&pte) || !read(leaf,2) || s->readWords[0] || s->readWords[1])
                return done(kIOReturnBadArgument);
            const UInt32 w[2]={UInt32(pte),UInt32(pte>>32)};
            g.leafAddress[g.leafChunks]=leaf;g.leafCount[g.leafChunks++]=1;
            if(!write(leaf,w,2))return done(kIOReturnIOError);
        }
        if(!mmuInvalidate(0x1))return done(kIOReturnTimeout);
        nvdecStep(3);
        nvgsp::ChannelGroup615 group{};group.hVASpace=g.vas;group.engineType=nvgsp::kEngineTypeNvdec0;
        nvgsp::ContextShare615 share{g.vas,0,~0U};
        if(!mk(D,H|NTSG,0xa06c,&group,sizeof(group)) || !mk(H|NTSG,H|NSHARE,0x9067,&share,sizeof(share)) ||
           share.hVASpace!=g.vas || share.subctxId==~0U)return done(kIOReturnNotReady);
        nvgsp::ChannelAlloc615 ch{};
        ch.gpFifoOffset=g.nvdecRingVa;ch.gpFifoEntries=256;ch.flags=0x004000c0U;
        ch.hContextShare=H|NSHARE;ch.hUserdMemory[0]=H|0x64;ch.engineType=nvgsp::kEngineTypeNvdec0;
        ch.instanceMem={g.nvdecInstance,4096,2,1};ch.userdMem={g.nvdecUserd,4096,2,1};
        ch.ramfcMem={g.nvdecInstance,512,2,1};ch.mthdbufMem={g.nvdecMethod,mBytes,2,0};
        ch.hPhysChannelGroup=H|NTSG;ch.internalFlags=0x14;
        if(!mk(H|NTSG,H|NCH,0xc56f,&ch,sizeof(ch)))return done(kIOReturnNotReady);
        g.nvdecCid=ch.cid;
        lk(__LINE__);setProperty("NVGspControl-nvdec615-cid",g.nvdecCid,32);ulk();
        nvdecStep(4);
        // falcon context: RM's external-VAS layout + VA bind, then GR style, then everything filled
        static_assert(sizeof(s->clientPromote)>=nvgsp::kPromoteCtxBytes,"promote buffer");
        UInt8 *const pr=s->clientPromote;
        UInt32 used=nvgsp::kPromoteVariants,first=0;
        for(UInt32 v=0;v<nvgsp::kPromoteVariants && used==nvgsp::kPromoteVariants;++v) {
            g.status=0;
            if(nvgsp::buildFalconPromote(nvgsp::kEngineTypeNvdec0,C,g.nvdecCid,H|NCH,g.nvdecCtx,g.nvdecCtxVa,4096,
                                         pr,nvgsp::kPromoteCtxBytes,v) && ctl(S,0x2080012b,pr,nvgsp::kPromoteCtxBytes))used=v;
            else if(!first)first=g.status?g.status:~0U;
        }
        UInt32 vaBind=~0U;
        if(used==nvgsp::kPromoteRmExternal) {
            g.status=0;
            vaBind=nvgsp::buildFalconPromoteVa(nvgsp::kEngineTypeNvdec0,C,H|NCH,g.nvdecCtxVa,pr,nvgsp::kPromoteCtxBytes) &&
                   ctl(S,0x2080012b,pr,nvgsp::kPromoteCtxBytes) ? 0 : (g.status?g.status:~0U);
        }
        g.nvdecPromote=used|first<<8;
        lk(__LINE__);
        const UInt32 promoteInfo[3]={used,first,vaBind};
        setProperty("NVGspControl-nvdec615-promote",(void *)promoteInfo,sizeof(promoteInfo));ulk();
        if(used==nvgsp::kPromoteVariants || !healthy())return done(kIOReturnNotReady);
        nvdecStep(5);
        UInt8 obj[12];nvgsp::buildVideoObjectParams(0,obj);
        if(!mk(H|NCH,H|NOBJ,0xc9b0,obj,sizeof(obj)))return done(kIOReturnNotReady);
        nvdecStep(6);
        UInt32 engine=nvgsp::kEngineTypeNvdec0;UInt8 on[3]={1,0,0};
        if(!ctl(H|NCH,0xa06f0104,&engine,4) || !ctl(H|NCH,0xc36f0108,&g.nvdecToken,4) || !g.nvdecToken)
            return done(kIOReturnNotReady);
        g.nvdecScheduled=true;
        if(!ctl(H|NCH,0xa06f0103,on,sizeof(on)) || !healthy())return done(kIOReturnNotReady);
        g.nvdecPut=0;
        lk(__LINE__);setProperty("NVGspControl-nvdec615-token",g.nvdecToken,32);ulk();
        nvdecStep(7);return done(kIOReturnSuccess);
      };
        return open();
    }
    if(phase==7) {
        // one GPFIFO entry for the caller's pushbuffer (in its own window), GP_PUT, doorbell.
        if(g.nvdecFaulted)return kIOReturnIOError;   // 0.180.192: RC killed the channel; tear the client down
        if(!g.live || !g.nvdecScheduled || !g.nvdecToken || !g.nvdecPbWords || g.nvdecPbWords>0x1fffff ||
           (g.nvdecPbVa&3) || g.nvdecPbVa<g.userVa || g.nvdecPbVa>=g.userNextVa ||
           UInt64(g.nvdecPbWords)*4>g.userNextVa-g.nvdecPbVa)return kIOReturnBadArgument;
        const UInt32 at=g.nvdecPut%256,next=(at+1)%256;
        const UInt32 entry[2]={UInt32(g.nvdecPbVa)&0xfffffffc,(UInt32(g.nvdecPbVa>>32)&0xff)|(g.nvdecPbWords<<10)};
        if(!write(g.nvdecGpfifo+UInt64(at)*8,entry,2) || !write(g.nvdecUserd+0x8c,&next,1))return kIOReturnIOError;
        g.nvdecPut=next;
        lk(__LINE__);
        lastSubmit615_[client615Slot_]=mach_absolute_time();   // 0.180.195: never demote a decoding client
        IOReturn ret=kIOReturnOffline;
        if(!resetBusy_ && !sleeping_ && !gmcMmuFaultPending_ && postInitPhase_==27 && gmcInitReady_) {
            IOMemoryMap *map=sharedBar0Map(pci_);ret=kIOReturnNoMemory;
            if(map && map->getLength()>=0x00BB0094) {Bar0Io bar0{map};ret=bar0.write(0x00BB0090,g.nvdecToken)?kIOReturnSuccess:kIOReturnIOError;}
            if(map)map->release();
        }
        ulk();return ret;
    }
    if(phase==3) {
        // After a verified cancel: free the maintenance channel first, then
        // the faulted client through the normal teardown below.
        constexpr UInt32 M=0xc0d00400;
        if(!g.cancelled || !healthy())return kIOReturnNotReady;
        if(g.negative && (!release(M|0x58) || !release(M|0x56) || !release(M|0x54) || !release(M|0x53) ||
           !release(M|0x52) || !release(M|0x51) || !release(M|0x50)))return kIOReturnNotReady;
        lk(__LINE__);g.cancelChannel=false;ulk();
    }
    if(!teardown) {
    step(1);
    UInt32 codeBytes=0,registers=1;
    // touchOnly loads the client GR context with the proven object/window
    // methods and no LAUNCH, so it needs neither a NAK nor root compute.
    if(!healthy() || !rootIntact() || s->sequence<(g.touchOnly?1U:3U) ||
       (!g.touchOnly && !nvgsp::computeNak615(s->provenNak,s->provenNakBytes,&codeBytes,&registers)) ||
       !nvgsp::clientGrLayout615(s->gr,&layout,g.localGlobals,g.subcontext))return kIOReturnNotReady;
    // Read-only diagnostic: root's PATCH page before any client action.
    UInt64 rootPatch=0;
    for(UInt32 i=0;i<s->gr.count;++i)if(s->gr.buffers[i].id==2)rootPatch=s->physical+s->gr.buffers[i].offset;
    if(!rootPatch || !read(rootPatch,1024))return kIOReturnBadArgument;
    lk(__LINE__);setProperty("NVGspControl-client615-root-patch-before",s->readWords,4096);ulk();
    // sameVas bisection: the client TSG/channel/context share root's VAS.
    g.vas=g.sameVas ? nvgsp::kLifecycleVaspace615 : VAS;
    nvgsp::VaspaceParams615 vas{};
    // external: nvos.h IS_EXTERNALLY_OWNED|ENABLE_PAGE_FAULTING (0x48) with a
    // driver-owned page tree; GSP rejects faulting on a managed VAS (83) and
    // absorbs faults of a non-faulting one (82).
    const UInt32 vasFlags=g.external ? nvgsp::kExternalFaultVasFlags615 : 0U;vas.flags=vasFlags;
    if(g.sameVas) {
        lk(__LINE__);
        auto *base=OSDynamicCast(OSNumber,getProperty("NVGspControl-vaspace-base"));
        auto *size=OSDynamicCast(OSNumber,getProperty("NVGspControl-vaspace-size"));
        if(base)vas.vaBase=base->unsigned64BitValue();if(size)vas.vaSize=size->unsigned64BitValue();ulk();
        if(!vas.vaBase || !vas.vaSize || vas.vaSize>UINT64_MAX-vas.vaBase)return kIOReturnNotReady;
    } else if(!alloc(D,VAS,0x90f1,&vas,sizeof(vas)) || vas.flags!=vasFlags || !vas.vaSize || vas.vaSize>UINT64_MAX-vas.vaBase)
        return kIOReturnNotReady;
    UInt64 generation=0;
    IOReturn reserved=kIOReturnSuccess;bool reused=false;
    // 0.180.126: the owned page tree lives at the end of the client's own VRAM
    // reservation; from GSP-RM's heap the sixth client's tree failed with
    // NV_ERR_NO_MEMORY (0.180.125 client615-fail-alloc: TREE, class 0x40, 0x51)
    const UInt64 reserveBytes=layout.clientBytes+nvgsp::kClientTreeReserve615;
    if(s->clientReserveFree[client615Slot_] && s->clientReserveBytes[client615Slot_]==reserveBytes && s->clientReserveGeneration[client615Slot_]==s->generation) {
        g.physical=s->clientReservePhysical[client615Slot_];g.bytes=s->clientReserveBytes[client615Slot_];generation=s->clientReserveGeneration[client615Slot_];
        s->clientReserveFree[client615Slot_]=false;reused=true;
    } else {
        reserved=diagnosticVramReserve(reserveBytes,&g.physical,&g.bytes,&generation,true);
        if(reserved==kIOReturnSuccess) {
            s->clientReservePhysical[client615Slot_]=g.physical;s->clientReserveBytes[client615Slot_]=g.bytes;
            s->clientReserveGeneration[client615Slot_]=generation;s->clientReserveFree[client615Slot_]=false;
        }
    }
    lk(__LINE__);setProperty("NVGspControl-client615-reserve-reused",reused);ulk();
    if(reserved!=kIOReturnSuccess)return reserved;
    if(generation!=s->generation || g.bytes!=reserveBytes || g.physical==s->physical)return kIOReturnOffline;
    if(!zero(g.physical,layout.clientBytes))return kIOReturnIOError;
    auto window=[&](UInt32 handle,UInt64 bytes,UInt64 alignment,UInt64 &va) {
        nvgsp::MemoryParams615 p{};p.owner=C;p.type=6;p.flags=0x80100;p.attr=0x800000;
        p.size=bytes;p.alignment=alignment;p.vaSpace=g.vas;
        if(!alloc(D,handle,0x50a0,&p,sizeof(p)))return false;
        va=p.offset;
        return (p.owner==C && p.type==6 && p.size==bytes && p.limit==bytes-1 && va>=vas.vaBase &&
            va<vas.vaBase+vas.vaSize && bytes<=vas.vaBase+vas.vaSize-va && !(va&(alignment-1)) &&
            va<(1ULL<<40)) || fail(kIOReturnBadArgument);
    };
    // Root's order (ring, then GR window) so a fresh VAS yields root's GR VA;
    // whether GR globals must sit at the same VA in every VAS is under test.
    if(g.external) {
        // Fixed layout in one PD1 slot: ring page just below root's GR VA.
        g.va=s->grVa;g.gpfifoVa=g.va-4096;
        UInt64 hi=g.va+layout.scratchVaOffset+nvgsp::kClientGrScratchBytes615;
        if(g.userBuffer) {
            g.userVa=(hi+0x1fffff)&~0x1fffffULL;hi=g.userVa+nvgsp::kClientUserWindow615+nvgsp::kClientWrapWindow615;
            g.userNextVa=g.userVa+nvgsp::kClientUserBytes615;
        }
        // One extra page below the client ring holds a maintenance ring.
        const UInt64 lo=g.gpfifoVa-4096;
        const UInt32 tables=nvgsp::externalTreeTables615(lo,hi);
        if(lo<vas.vaBase || hi>vas.vaBase+vas.vaSize || !tables)return kIOReturnBadArgument;
        const UInt64 size=nvgsp::externalTreeBytes615(lo,hi),treePhysical=g.physical+layout.clientBytes;   // 0.180.126
        if(!size || size>nvgsp::kClientTreeReserve615)return kIOReturnBadArgument;
        if(!zero(treePhysical,(size+0xffff)&~0xffffULL) || !nvgsp::externalTreePlan615(treePhysical,lo,hi,&g.tree))return kIOReturnBadArgument;
        auto pde=[&](UInt64 at,UInt64 value) {
            const UInt32 w[2]={UInt32(value),UInt32(value>>32)};return write(at,w,2);
        };
        for(unsigned i=0;i<g.tree.upperCount;++i)if(!pde(g.tree.upperAddress[i],g.tree.upperValue[i]))return kIOReturnIOError;
        for(UInt32 i=0;i<g.tree.tables;++i)if(!pde(g.tree.pd0Address[i],g.tree.pd0Value[i]))return kIOReturnIOError;
        nvgsp::SetPageDirectory615 directory{g.tree.tree,4,8,VAS,0,1,0};
        if(!ctl(D,0x801813,&directory,sizeof(directory)))return kIOReturnNotReady;
        if(directory.physical!=g.tree.tree || directory.entries!=4 || directory.flags!=8 ||
           directory.vas!=VAS || directory.channel || directory.subdevice!=1 || directory.pasid)return kIOReturnBadArgument;
        lk(__LINE__);setProperty("NVGspControl-client615-external-tree",&g.tree,nvgsp::kExternalTreeHeader615);ulk();
    } else if(!window(RING,4096,4096,g.gpfifoVa) || !window(WINDOW,s->bytes,s->gr.virtualAlignment,g.va))
        return kIOReturnNotReady;
    lk(__LINE__);
    setProperty("NVGspControl-client615-physical",g.physical,64);
    setProperty("NVGspControl-client615-bytes",g.bytes,64);
    setProperty("NVGspControl-client615-va",g.va,64);
    setProperty("NVGspControl-client615-gpfifo-va",g.gpfifoVa,64);
    setProperty("NVGspControl-client615-va-matches-root",g.va==s->grVa);
    setProperty("NVGspControl-client615-layout",&layout,sizeof(layout));ulk();
    step(2);
    // subcontext: an ASYNC context share (own VAS, own VEID) inside root's
    // GR TSG, so the TSG keeps one GR context and never switches GR context
    // between TSGs. Otherwise a separate TSG with its own context.
    const UInt32 tsg=g.subcontext ? 0xc0d00050U : TSG,shareFlags=g.subcontext ? 1U : 0U;
    nvgsp::ChannelGroup615 group{};group.hVASpace=g.vas;group.engineType=1;
    nvgsp::ContextShare615 share{g.vas,shareFlags,~0U};
    if((!g.subcontext && !alloc(D,TSG,0xa06c,&group,sizeof(group))) || !alloc(tsg,SHARE,0x9067,&share,sizeof(share)) ||
       share.hVASpace!=g.vas || share.flags!=shareFlags || share.subctxId==~0U)return kIOReturnNotReady;
    lk(__LINE__);setProperty("NVGspControl-client615-subctx-id",share.subctxId,32);ulk();
    UInt32 methodBytes=0;
    if(!ctl(S,0x20802a08,&methodBytes,sizeof(methodBytes)) || !methodBytes || methodBytes>16*1024*1024)return kIOReturnBadArgument;
    g.methodBytes=(UInt64(methodBytes)+4095)&~4095ULL;
    UInt64 *backings[4]={&g.instance,&g.method,&g.userd,&g.gpfifo};
    for(UInt32 i=0;i<4;++i) {
        nvgsp::MemoryParams615 p{};p.owner=C;p.type=6;p.flags=0x100;p.attr=0x800000;
        p.size=i==1?g.methodBytes:4096;p.alignment=4096;const UInt64 size=p.size;
        if(!alloc(S,H|(0x52+i),0x40,&p,sizeof(p)))return kIOReturnNotReady;
        if(!nvgsp::memoryProbeRange615(p,gmcConfig_.fbBytes,heapLo,heapHi) || p.size!=size)return kIOReturnBadArgument;
        *backings[i]=p.offset;if(!zero(p.offset,size))return kIOReturnIOError;
    }
    lk(__LINE__);setProperty("NVGspControl-client615-instance",g.instance,64);ulk();
    step(3);
    // Owned tree: write leaf PTEs directly, one table run at a time, recording
    // each run so teardown clears exactly what was set.
    auto mapOwned=[&](UInt64 va,UInt64 bytes,UInt64 ringPhysical) {
        for(UInt64 done=0;done<bytes;) {
            const UInt64 at=va+done;UInt64 leaf=0;
            if(!nvgsp::externalTreeLeaf615(g.tree,at,&leaf) || g.leafChunks>=nvgsp::kClientLeafChunks615)return fail(kIOReturnBadArgument);
            UInt64 count=(bytes-done)/4096;const UInt64 room=512-((at>>12)&511);
            if(count>room)count=room;
            if(count>256)count=256;
            if(!read(leaf,UInt32(count*2)))return false;
            for(UInt32 i=0;i<count*2;++i)if(s->readWords[i])return fail(kIOReturnBusy);
            for(UInt32 i=0;i<count;++i) {
                UInt64 physical=0,value=0;bool privileged=false;const UInt64 offset=done+i*4096;
                const bool present=ringPhysical ? (physical=ringPhysical+offset,true) :
                    nvgsp::clientGrPage615(s->gr,layout,s->physical,g.physical,offset,&physical,&privileged);
                if(present && !nvgsp::vidPte615(physical,privileged,false,&value))return fail(kIOReturnBadArgument);
                if(present) {++g.mappedPages;if(privileged)++g.privilegedPages;}
                s->writeWords[i*2]=UInt32(value);s->writeWords[i*2+1]=UInt32(value>>32);
            }
            g.leafAddress[g.leafChunks]=leaf;g.leafCount[g.leafChunks++]=UInt32(count);
            if(!write(leaf,s->writeWords,UInt32(count*2)))return false;
            done+=count*4096;
        }
        UInt32 invalidate[2]={VAS,0};return ctl(D,0x80180c,invalidate,sizeof(invalidate));
    };
    const UInt64 grBytes=layout.scratchVaOffset+nvgsp::kClientGrScratchBytes615;
    // A negative client also gets its maintenance ring mapped now, before the
    // GPU caches this page table: a PTE written later was not seen (87).
    if(g.external && g.negative) {
        nvgsp::MemoryParams615 p{};p.owner=C;p.type=6;p.flags=0x100;p.attr=0x800000;p.size=4096;p.alignment=4096;
        if(!alloc(S,0xc0d00455,0x40,&p,sizeof(p)) || p.size!=4096 ||
           !nvgsp::memoryProbeRange615(p,gmcConfig_.fbBytes,heapLo,heapHi))return kIOReturnNotReady;
        g.maintGpfifo=p.offset;if(!zero(p.offset,4096))return kIOReturnIOError;
        if(!mapOwned(g.gpfifoVa-4096,4096,g.maintGpfifo))return kIOReturnIOError;
    }
    if(g.external ? (!mapOwned(g.va,grBytes,0) || !mapOwned(g.gpfifoVa,4096,g.gpfifo)) :
                    (!map(g.va,grBytes,false) || !map(g.gpfifoVa,4096,true)))
        return kIOReturnIOError;
    // Caller buffer: zeroed sysmem pages, coherent+volatile PTEs in one leaf run.
    auto mapUser=[&]() __attribute__((noinline)) {
        const UInt32 pages=UInt32(nvgsp::kClientUserBytes615/4096);UInt64 leaf=0;
        if(!g.external || nativeUserDatas_[client615Slot_].size() || g.leafChunks>=nvgsp::kClientLeafChunks615 ||
           !nativeUserDatas_[client615Slot_].allocate(nvgsp::kClientUserBytes615,false,nullptr,1) || !nativeUserDatas_[client615Slot_].syncToDevice() ||
           !nvgsp::externalTreeLeaf615(g.tree,g.userVa,&leaf) || 512-((g.userVa>>12)&511)<pages ||
           !read(leaf,pages*2))return false;
        for(UInt32 i=0;i<pages*2;++i)if(s->readWords[i])return false;
        for(UInt32 i=0;i<pages;++i) {
            UInt64 bus=0,value=0;
            if(!nativeUserDatas_[client615Slot_].busPage(i,&bus) || !nvgsp::sysPte615(bus,false,&value))return false;
            s->writeWords[i*2]=UInt32(value);s->writeWords[i*2+1]=UInt32(value>>32);
        }
        g.leafAddress[g.leafChunks]=leaf;g.leafCount[g.leafChunks++]=pages;g.mappedPages+=pages;
        UInt32 invalidate[2]={VAS,0};
        return write(leaf,s->writeWords,pages*2) && ctl(D,0x80180c,invalidate,sizeof(invalidate));
    };
    if(g.userBuffer && !mapUser())return kIOReturnIOError;
    if(g.userBuffer && !mmuInvalidate(0x1))return kIOReturnNotReady;
    lk(__LINE__);
    setProperty("NVGspControl-client615-mapped-pages",g.mappedPages,32);
    setProperty("NVGspControl-client615-privileged-pages",g.privilegedPages,32);
    setProperty("NVGspControl-client615-leaf-chunks",g.leafChunks,32);ulk();
    step(4);
    nvgsp::ChannelAlloc615 ch{};
    // alloc_channel.h GROUP_CHANNEL_THREAD 29:28: an async compute subcontext
    // channel runs on thread ONE; thread DEFAULT is the sync/graphics one.
    const UInt32 channelFlags=g.subcontext ? 0x104000c0U : 0x004000c0U;
    ch.gpFifoOffset=g.gpfifoVa;ch.gpFifoEntries=256;ch.flags=channelFlags;
    ch.hContextShare=SHARE;ch.hUserdMemory[0]=H|0x54;ch.engineType=1;
    ch.instanceMem={g.instance,4096,2,1};ch.userdMem={g.userd,4096,2,1};
    ch.ramfcMem={g.instance,512,2,1};ch.mthdbufMem={g.method,g.methodBytes,2,0};
    ch.hPhysChannelGroup=tsg;ch.internalFlags=0x14;
    if(!alloc(tsg,CHANNEL,0xc56f,&ch,sizeof(ch)) || ch.internalFlags!=0x14 || ch.flags!=channelFlags)return kIOReturnNotReady;
    g.cid=ch.cid;
    UInt8 *const promote=s->clientPromote;constexpr UInt32 promoteBytes=sizeof(s->clientPromote);
    bzero(promote,promoteBytes);
    // 68: no PM entry, as primary RM promotes PM only for a PM ctxsw buffer.
    // A subcontext promotes root's existing buffers VA-only, PM included.
    if(!nvgsp::promoteClientGrBlob615(s->gr,layout,C,CHANNEL,s->physical,g.physical,g.va,promote,promoteBytes,g.subcontext))
        return kIOReturnBadArgument;
    lk(__LINE__);setProperty("NVGspControl-client615-promote",promote,promoteBytes);ulk();
    if(!ctl(S,0x2080012b,promote,promoteBytes) || !healthy())return kIOReturnNotReady;
    if(!read(rootPatch,1024))return kIOReturnIOError;
    lk(__LINE__);setProperty("NVGspControl-client615-root-patch-after-promote",s->readWords,4096);ulk();
    if(!read(g.physical+layout.clientOffset[2],1024))return kIOReturnIOError;
    lk(__LINE__);setProperty("NVGspControl-client615-client-patch-after-promote",s->readWords,4096);ulk();
    // Primary ctrl2080gr.h SET_CTXSW_PREEMPTION_MODE: flags CILP field set,
    // compute mode CTA (1), graphics untouched. The promoted context reports
    // WFI, which our 0.178.11 multi-channel work tied to RC 109 timeouts.
    // A subcontext shares root's TSG context, so its preemption mode stays.
    if(!g.subcontext) {
        if(!preemptCompute())return kIOReturnNotReady;
        lk(__LINE__);setProperty("NVGspControl-client615-compute-preempt-cta",true);ulk();
    }
    step(5);
    UInt32 grParams[4]={2,0,16,0};
    // Async subcontexts are compute-only: GSP refuses ADA_A there (76,
    // NV_ERR_INVALID_OBJECT); graphics stays on the sync subcontext.
    if(!g.subcontext && (!alloc(CHANNEL,H|0x58,0xc997,grParams,sizeof(grParams)) ||
       grParams[0]!=2 || grParams[1] || grParams[2]!=16 || !healthy()))return kIOReturnNotReady;
    grParams[3]=0;
    if(!alloc(CHANNEL,H|0x59,0xc9c0,grParams,sizeof(grParams)) ||
       grParams[0]!=2 || grParams[1] || grParams[2]!=16 || !healthy())return kIOReturnNotReady;
    // 0.180.128: a copy engine object on the GR channel (a GRCE), so the Metal
    // driver can copy, fill and convert block-linear surfaces on the GPU
    // (VRAM included). Optional: RM refusing every CE leaves the client as
    // before; a refused allocation is taken off the ledger again.
    g.copyEngine=0;
    for(UInt32 ce=9;ce<=18 && !g.subcontext && !g.copyEngine;++ce) {
        UInt32 copy[2]={1,ce};const UInt32 before=g.objectCount,status=g.status;
        if(alloc(CHANNEL,H|0x5b,0xc7b5,copy,8))g.copyEngine=ce;
        else if(g.status && g.status<0x10000 && g.objectCount==before+1) {g.objectCount=before;g.status=status;}
        else return kIOReturnNotReady;
    }
    lk(__LINE__);setProperty("NVGspControl-client615-copy-engine",g.copyEngine,32);ulk();
    if(!healthy())return kIOReturnNotReady;
    UInt32 engine=1;
    if(!ctl(CHANNEL,0xa06f0104,&engine,4) || !ctl(CHANNEL,0xc36f0108,&g.token,4))return kIOReturnNotReady;
    UInt8 schedule[3]={1,0,0};g.scheduled=true;
    if(!ctl(CHANNEL,0xa06f0103,schedule,sizeof(schedule)) || !healthy())return kIOReturnNotReady;
    lk(__LINE__);
    setProperty("NVGspControl-client615-cid",g.cid,32);
    setProperty("NVGspControl-client615-token",g.token,32);ulk();
    step(6);
    // Two fixed submissions: a HOST-only release, then the root-proven NAK.
    constexpr UInt32 bootFence=0x6150c3a0,hostFence=0x6150c3a1,computeRelease=0xc0430001;
    const UInt64 semaphore=g.gpfifoVa+0xe00,pb=g.gpfifoVa+0x800;
    const UInt64 va=g.va+layout.scratchVaOffset,physical=g.physical+layout.scratchOffset;
    g.scratchVa=va;g.scratchPhysical=physical;
    auto submit=[&](UInt32 words,UInt32 put,UInt32 fence,bool shader) {
        const UInt32 entry[2]={UInt32(pb)&0xfffffffc,(UInt32(pb>>32)&0xff)|(words<<10)};
        if(!write(g.gpfifo+UInt64(put-1)*8,entry,2) || !write(g.userd+0x8c,&put,1) ||
           diagnosticChannelKick(CHANNEL,true)!=kIOReturnSuccess)return false;
        UInt64 deadline=0;clock_interval_to_deadline(5,kSecondScale,&deadline);
        do {
            if(shader && !read(physical+0xaf00,1))return false;
            const bool released=!shader || s->readWords[0]==computeRelease;
            if(!read(g.gpfifo+0xe00,1))return false;
            if(released && s->readWords[0]==fence)
                return read(g.userd+0x88,2) && s->readWords[0]==put && s->readWords[1]==put;
            IOSleep(1);
        } while(mach_absolute_time()<deadline);
        return false;
    };
    const UInt32 host[6]={0x20050017,UInt32(semaphore)&0xfffffffc,UInt32(semaphore>>32)&0xff,bootFence,0,0x00100001};
    if(!read(g.userd+0x88,2) || s->readWords[0] || s->readWords[1] || !write(g.gpfifo+0x800,host,6) ||
       !submit(6,1,bootFence,false))return kIOReturnTimeout;
    step(7);
    if(!nvgsp::computePacket615(va,va+0x8000,va+0x9000,va+0xaf00,registers,computeRelease,&s->compute))
        return kIOReturnBadArgument;
    if(g.touchOnly) {
        // SET_OBJECT ADA_COMPUTE_A + shared/local memory windows: 8 words.
        constexpr UInt32 touchWords=8;
        __builtin_memcpy(s->writeWords,s->compute.commands,touchWords*4);
        const UInt32 tail[6]={0x20050017,UInt32(semaphore)&0xfffffffc,UInt32(semaphore>>32)&0xff,hostFence,0,0x00100001};
        __builtin_memcpy(s->writeWords+touchWords,tail,sizeof(tail));
        if(!write(g.gpfifo+0x800,s->writeWords,touchWords+6) || !submit(touchWords+6,2,hostFence,false))return kIOReturnTimeout;
        lk(__LINE__);setProperty("NVGspControl-client615-touch-only-done",true);ulk();
    } else {
    const auto *code=s->provenNak+64;
    for(UInt32 done=0;done<codeBytes;) {
        const UInt32 count=(codeBytes-done)>4096?1024:(codeBytes-done)/4;
        __builtin_memcpy(s->writeWords,code+done,count*4);
        if(!write(physical+done,s->writeWords,count))return kIOReturnIOError;
        done+=count*4;
    }
    if(!write(physical+0x8000,s->compute.qmd,64))return kIOReturnIOError;
    // negative: the store targets a page root maps (its own backing) but the
    // client VAS leaves unmapped, beyond the client's scratch window.
    const UInt64 negativeOffset=s->bytes-0x10000;
    if(g.negative && negativeOffset<layout.scratchVaOffset+nvgsp::kClientGrScratchBytes615)return kIOReturnBadArgument;
    const UInt64 outputVa=g.negative ? g.va+negativeOffset : g.userBuffer ? g.userVa : va+0xa000;
    if(g.negative) {
        const IOReturn replay=nativeReplayData_.size()?kIOReturnSuccess:registerReplayableFault615();
        if(replay!=kIOReturnSuccess)return replay;
        if(!read(s->physical+negativeOffset,64))return kIOReturnIOError;
        lk(__LINE__);
        setProperty("NVGspControl-client615-negative-target-va",outputVa,64);
        setProperty("NVGspControl-client615-negative-root-before",s->readWords,256);ulk();
    }
    const UInt32 constant[8]={UInt32(outputVa),UInt32(outputVa>>32),64,1,1,64,1,1};
    if(!write(physical+0x9000,constant,8) || !read(physical+0xa000,64))return kIOReturnIOError;
    for(unsigned i=0;i<64;++i)if(s->readWords[i])return kIOReturnIOError;
    if(!read(physical+0xaf00,1) || s->readWords[0])return kIOReturnIOError;
    const UInt32 n=s->compute.commandCount;if(n+6>256)return kIOReturnBadArgument;
    __builtin_memcpy(s->writeWords,s->compute.commands,n*4);
    const UInt32 tail[6]={0x20050017,UInt32(semaphore)&0xfffffffc,UInt32(semaphore>>32)&0xff,hostFence,0,0x00100001};
    __builtin_memcpy(s->writeWords+n,tail,sizeof(tail));
    if(!write(g.gpfifo+0x800,s->writeWords,n+6))return kIOReturnIOError;
    if(!submit(n+6,2,hostFence,true)) {
        // Read-only evidence after the denied store: root's target page must
        // still hold its prior contents; the client's own output stays zero.
        if(g.negative && read(s->physical+negativeOffset,64)) {
            lk(__LINE__);setProperty("NVGspControl-client615-negative-root-after",s->readWords,256);ulk();
        }
        if(g.negative && read(physical+0xa000,64)) {
            lk(__LINE__);setProperty("NVGspControl-client615-negative-client-output",s->readWords,256);ulk();
        }
        if(g.negative && nativeReplayData_.readDeviceRange(0,s->readWords,8*32)) {
            lk(__LINE__);setProperty("NVGspControl-client615-negative-replay-records",s->readWords,8*32);ulk();
        }
        return kIOReturnTimeout;
    }
    if(g.userBuffer ? (!nativeUserDatas_[client615Slot_].syncFromDevice() || !nativeUserDatas_[client615Slot_].readDeviceRange(0,s->readWords,256)) :
       !read(physical+0xa000,64))return kIOReturnIOError;
    __builtin_memcpy(g.output,s->readWords,sizeof(g.output));
    g.put=2;g.fence=hostFence;
    lk(__LINE__);setProperty("NVGspControl-client615-output",g.output,sizeof(g.output));ulk();
    }
    if(!healthy() || !rootIntact())return kIOReturnNotReady;
    // Read-only diagnostic: both instance blocks after work, for an offline
    // field-by-field comparison of RAMFC/RAMIN state.
    if(!read(g.instance,1024))return kIOReturnIOError;
    lk(__LINE__);setProperty("NVGspControl-client615-instance-block",s->readWords,4096);ulk();
    if(!read(s->instance,1024))return kIOReturnIOError;
    lk(__LINE__);setProperty("NVGspControl-client615-root-instance-block",s->readWords,4096);ulk();
    // RAMIN ENGINE_WFI_PTR (words 132/133) names the GR context image VA.
    // Dump that page and the PATCH buffer's first page for root and client.
    const UInt64 ctxVa=(UInt64(s->readWords[133]&0xff)<<32)|(s->readWords[132]&0xfffff000U);
    UInt64 patchOffset=0;bool patch=false;
    for(UInt32 i=0;i<s->gr.count;++i)if(s->gr.buffers[i].id==2) {patchOffset=s->gr.buffers[i].offset;patch=true;}
    if(ctxVa<s->grVa || ctxVa-s->grVa>=layout.scratchVaOffset || !patch)return kIOReturnBadArgument;
    const UInt64 offsets[2]={ctxVa-s->grVa,patchOffset};
    const char *names[4]={"NVGspControl-client615-root-ctx-page","NVGspControl-client615-client-ctx-page",
                          "NVGspControl-client615-root-patch-page","NVGspControl-client615-client-patch-page"};
    for(UInt32 i=0;i<2;++i) {
        UInt64 clientPhysical=0;bool privileged=false;
        if(!nvgsp::clientGrPage615(s->gr,layout,s->physical,g.physical,offsets[i],&clientPhysical,&privileged) ||
           !privileged || !read(s->physical+offsets[i],1024))return kIOReturnIOError;
        lk(__LINE__);setProperty(names[i*2],s->readWords,4096);ulk();
        if(!read(clientPhysical,1024))return kIOReturnIOError;
        lk(__LINE__);setProperty(names[i*2+1],s->readWords,4096);ulk();
    }
    lk(__LINE__);setProperty("NVGspControl-client615-ctx-va",ctxVa,64);ulk();
    step(8);return kIOReturnSuccess;
    }
    if(!healthy() || !rootIntact())return kIOReturnNotReady;
    // Disable, free channel objects, clear exactly the recorded PTEs, flush,
    // then free backing. The client VRAM reservation stays pinned to cold stop.
    // A subcontext channel shares root's TSG; a channel-level schedule-off
    // could stop the whole TSG, so it is freed directly (RM preempts on free).
    // 0.180.191: the NVDEC0 channel first (schedule off, object, channel); its backings go with
    // the client's after the PTEs are cleared. Only handles RM made are in the ledger.
    auto inLedger=[&](UInt32 o) {for(UInt32 i=0;i<g.objectCount;++i)if(g.objects[i]==o)return true;return false;};
    if(g.nvdec) {
        UInt8 off[3]={0,0,0};
        // a faulted channel (RC killed it) is freed without the schedule-off
        if(g.nvdecScheduled && !g.nvdecFaulted && (!ctl(H|NCH,0xa06f0103,off,sizeof(off)) || !healthy()))
            return kIOReturnNotReady;
        g.nvdecScheduled=false;
        if((inLedger(H|NOBJ) && !release(H|NOBJ)) || (inLedger(H|NCH) && !release(H|NCH)) || !healthy())
            return kIOReturnNotReady;
    }
    if(!g.subcontext && phase!=3) {
        UInt8 disable[3]={0,0,0};
        if(!ctl(CHANNEL,0xa06f0103,disable,sizeof(disable)) || !healthy())return kIOReturnNotReady;
    }
    g.scheduled=false;step(9);
    if((g.copyEngine && !release(H|0x5b)) || !release(H|0x59) || (!g.subcontext && !release(H|0x58)) || !healthy())return kIOReturnNotReady;
    step(10);
    if(!release(CHANNEL) || !healthy())return kIOReturnNotReady;
    step(11);
    for(UInt32 i=0;i<g.leafChunks;++i) {
        bzero(s->writeWords,sizeof(s->writeWords));
        if(!write(g.leafAddress[i],s->writeWords,g.leafCount[i]*2))return kIOReturnIOError;
    }
    UInt32 invalidate[2]={g.vas,0};if(!ctl(D,0x80180c,invalidate,sizeof(invalidate)))return kIOReturnNotReady;
    // The VAS/tree (and so its PDB) may be reused by the next client: drop
    // every cached translation before the backing is freed.
    if(g.external && !mmuInvalidate(0x1))return kIOReturnTimeout;
    // No GPU translation reaches the caller pages now; a CPU mapping the
    // owner still holds keeps the descriptor alive.
    // 0.180.109: the backing stays pinned until the owner's connection closes
    // (nativeClient615Close); the GPU can no longer reach it from here on.
    if(g.userBuffer)g.allocLive=0;
    step(12);
    if(g.nvdec) {
        const UInt32 rest[7]={NCTX,0x65,0x64,0x63,0x62,NSHARE,NTSG};
        for(UInt32 h:rest)if(inLedger(H|h) && !release(H|h))return kIOReturnNotReady;
        g.nvdec=false;
    }
    if(!release(H|0x55) || !release(H|0x54) || !release(H|0x53) || !release(H|0x52) ||
       !release(SHARE) || (!g.subcontext && !release(TSG)) ||
       (g.external ? !release(VAS) : (!release(RING) || !release(WINDOW))) ||
       (!g.sameVas && !g.external && !release(VAS)))return kIOReturnNotReady;
    if(phase==3 && g.negative && !release(0xc0d00455))return kIOReturnNotReady;
    if(g.freedCount!=g.objectCount || !healthy() || !rootIntact())return kIOReturnNotReady;
    if(phase==3) {
        // Re-admission evidence: GR idle and the replayable buffer drained.
        IOMemoryMap *bm=sharedBar0Map(pci_);UInt32 regs[3]={~0U,~0U,~0U};
        bool ok=bm && bm->getLength()>=0x100E4C;
        if(ok) {Bar0Io io{bm};ok=io.read(0x400700,&regs[0]) && io.read(0x100E40,&regs[1]) && io.read(0x100E44,&regs[2]);}
        if(bm)bm->release();
        lk(__LINE__);setProperty("NVGspControl-client615-recover-regs",regs,sizeof(regs));ulk();
        if(!ok || regs[0] || (regs[1]&0xfffff)!=(regs[2]&0xfffff))return kIOReturnNotReady;
    }
    if(g.physical==s->clientReservePhysical[client615Slot_])s->clientReserveFree[client615Slot_]=true;   // 0.180.106: verified teardown
    step(13);return kIOReturnSuccess;
}
