// Owned, non-CC nonreplayable shadow buffers. Included in the control service.
// Once exposed to GSP these DMA mappings survive every failure until stop
// disables PCI bus mastering. No unregister/free/retry is attempted here.
IOReturn NVGspControl::registerNativeFault615() {
    auto *s=native615_;
    UInt32 pages=0,usable=0;
    if(!s || s->faultAttempted || s->generation!=resetGeneration() ||
       !nvgsp::nonReplayableFaultPlan615(s->faultInfo,&pages,&usable))return kIOReturnNotReady;
    s->faultAttempted=true;
    lk(__LINE__);setProperty("NVGspControl-native615-fault-register-attempted",true);ulk();
    // Live GET/packet offsets are not page aligned. Apple's DMA transfer path
    // applies the command's address alignment even to readBytes/writeBytes.
    // Allocation and bus-page validation still require actual4KiB GPU pages.
    if(!nativeFaultQueue_.allocate(4096,true,nullptr,1) ||
       !nativeFaultData_.allocate(UInt64(pages)*4096,false,nullptr,1))return kIOReturnNoMemory;
    const auto *zero=static_cast<const UInt32 *>(nativeFaultData_.bytes());
    for(size_t i=0;i<nativeFaultData_.size()/4;++i)if(zero[i])return kIOReturnIOError;
    // Before exposing either mapping to GSP, prove exact byte-range access on
    // these real DMA commands and verify untouched neighboring fields. Restore
    // all test bytes to zero before initializing/handing over the queue.
    bzero(s->writeWords,192);bzero(s->readWords,192);
    const UInt64 sentinel=0x6150da7a89abcdefULL,empty=0;
    if(!nativeFaultQueue_.writeDeviceRange(64,&sentinel,8) ||
       !nativeFaultQueue_.readDeviceRange(0,s->readWords,192))return kIOReturnIOError;
    __builtin_memcpy(s->writeWords+16,&sentinel,8);
    if(__builtin_memcmp(s->readWords,s->writeWords,192) ||
       !nativeFaultQueue_.writeDeviceRange(64,&empty,8) ||
       !nativeFaultQueue_.readDeviceRange(64,s->readWords,8) ||
       s->readWords[0] || s->readWords[1])return kIOReturnIOError;
    for(unsigned i=0;i<8;++i)s->writeWords[i]=0x6150da00+i;
    if(!nativeFaultData_.writeDeviceRange(32,s->writeWords,32) ||
       !nativeFaultData_.readDeviceRange(32,s->readWords,32) ||
       __builtin_memcmp(s->writeWords,s->readWords,32))return kIOReturnIOError;
    bzero(s->writeWords,32);
    if(!nativeFaultData_.writeDeviceRange(32,s->writeWords,32) ||
       !nativeFaultData_.readDeviceRange(0,s->readWords,96))return kIOReturnIOError;
    for(unsigned i=0;i<24;++i)if(s->readWords[i])return kIOReturnIOError;
    lk(__LINE__);setProperty("NVGspControl-native615-fault-dma-exact-range-preflight",true);ulk();
    for(UInt32 i=0;i<pages;++i)if(!nativeFaultData_.busPage(i,&s->faultPages[i]))return kIOReturnIOError;
    nvgsp::ShadowFaultQueue615 queue{};
    queue.capacity=UInt64(usable)+1;queue.msgSize=nvgsp::kFaultRecordBytes615;
    if(!nativeFaultQueue_.writeDeviceRange(0,&queue,sizeof(queue)) ||
       !nativeFaultData_.syncToDevice())return kIOReturnIOError;
    if(!nvgsp::buildControl615(s->faultRequest,sizeof(s->faultRequest),
            nvgsp::kLifecycleClient615,nvgsp::kLifecycleSubdevice615,
            nvgsp::kShadowFaultRegisterCommand615,sizeof(nvgsp::ShadowFaultRegistration615)))
        return kIOReturnBadArgument;
    auto *registration=reinterpret_cast<nvgsp::ShadowFaultRegistration615 *>(s->faultRequest+40);
    if(!nvgsp::buildShadowFaultRegistration615(registration,nativeFaultQueue_.busAddress(),
            s->faultInfo.nonReplayableBytes,s->faultPages,pages))return kIOReturnBadArgument;
    lk(__LINE__);
    setProperty("NVGspControl-native615-fault-queue-bus",nativeFaultQueue_.busAddress(),64);
    setProperty("NVGspControl-native615-fault-data-pages",s->faultPages,pages*8);
    setProperty("NVGspControl-native615-fault-data-bytes",nativeFaultData_.size(),64);
    setProperty("NVGspControl-native615-fault-data-zero-verified",true);
    setProperty("NVGspControl-native615-fault-queue-initial",&queue,sizeof(queue));
    ulk();
    UInt32 bytes=sizeof(s->faultReply),result=~0U;
    const IOReturn transport=userRpcImpl(76,s->faultRequest,sizeof(s->faultRequest),
        s->faultReply,&bytes,&result,true,true);
    lk(__LINE__);
    setProperty("NVGspControl-native615-fault-register-transport",UInt32(transport),32);
    setProperty("NVGspControl-native615-fault-register-result",result,32);
    setProperty("NVGspControl-native615-fault-register-reply-bytes",bytes,32);
    if(bytes)setProperty("NVGspControl-native615-fault-register-reply",s->faultReply,bytes);
    ulk();
    if(transport!=kIOReturnSuccess)return transport;
    nvgsp::ControlReply615 reply{};
    if(bytes!=sizeof(s->faultReply) || !nvgsp::decodeControlReply615(s->faultReply,bytes,
            nvgsp::kLifecycleClient615,nvgsp::kLifecycleSubdevice615,
            nvgsp::kShadowFaultRegisterCommand615,sizeof(*registration),reply))return kIOReturnBadMessageID;
    if(result || !reply.successful())return kIOReturnError;
    if(__builtin_memcmp(reply.params,registration,sizeof(*registration)))return kIOReturnIOError;
    s->faultRegistered=true;
    lk(__LINE__);
    setProperty("NVGspControl-native615-fault-buffer-registered",true);
    setProperty("NVGspControl-native615-fault-recovery-ready",false);
    const bool observed=snapshotNativeFault615();
    ulk();
    return observed ? kIOReturnSuccess : kIOReturnIOError;
}

// Owned hardware REPLAYABLE buffer (GR/SM faults). Registered once, only for
// the fixed client negative probe; records are observed, never replayed.
IOReturn NVGspControl::registerReplayableFault615() {
    auto *s=native615_;UInt32 pages=0;
    if(!s || !s->faultRegistered || s->generation!=resetGeneration() || nativeReplayData_.size() ||
       !nvgsp::replayableFaultPlan615(s->faultInfo,&pages))return kIOReturnNotReady;
    lk(__LINE__);setProperty("NVGspControl-native615-replay-register-attempted",true);ulk();
    if(!nativeReplayData_.allocate(UInt64(pages)*4096,false,nullptr,1))return kIOReturnNoMemory;
    const auto *zero=static_cast<const UInt32 *>(nativeReplayData_.bytes());
    for(size_t i=0;i<nativeReplayData_.size()/4;++i)if(zero[i])return kIOReturnIOError;
    if(!nativeReplayData_.syncToDevice())return kIOReturnIOError;
    constexpr UInt32 bytes=sizeof(nvgsp::ReplayableFaultRegistration615);
    if(!nvgsp::buildControl615(s->faultRequest,sizeof(s->faultRequest),nvgsp::kLifecycleClient615,
            nvgsp::kLifecycleSubdevice615,nvgsp::kReplayableFaultRegisterCommand615,bytes))return kIOReturnBadArgument;
    auto *r=reinterpret_cast<nvgsp::ReplayableFaultRegistration615 *>(s->faultRequest+40);
    bzero(r,bytes);
    r->hClient=nvgsp::kLifecycleClient615;r->hObject=nvgsp::kLifecycleSubdevice615;
    r->faultBufferSize=s->faultInfo.replayableBytes;
    for(UInt32 i=0;i<pages;++i)if(!nativeReplayData_.busPage(i,&r->pages[i]))return kIOReturnIOError;
    lk(__LINE__);
    setProperty("NVGspControl-native615-replay-pages",r->pages,pages*8);
    setProperty("NVGspControl-native615-replay-bytes",s->faultInfo.replayableBytes,32);ulk();
    UInt32 replyBytes=sizeof(s->faultReply),result=~0U;
    const IOReturn transport=userRpcImpl(76,s->faultRequest,40+bytes,s->faultReply,&replyBytes,&result,true,true);
    lk(__LINE__);
    setProperty("NVGspControl-native615-replay-register-transport",UInt32(transport),32);
    setProperty("NVGspControl-native615-replay-register-result",result,32);ulk();
    if(transport!=kIOReturnSuccess)return transport;
    nvgsp::ControlReply615 reply{};
    if(replyBytes!=96+bytes || !nvgsp::decodeControlReply615(s->faultReply,replyBytes,nvgsp::kLifecycleClient615,
            nvgsp::kLifecycleSubdevice615,nvgsp::kReplayableFaultRegisterCommand615,bytes,reply))return kIOReturnBadMessageID;
    lk(__LINE__);setProperty("NVGspControl-native615-replay-register-status",reply.header.status,32);ulk();
    if(result || !reply.successful())return kIOReturnError;
    lk(__LINE__);setProperty("NVGspControl-native615-replay-registered",true);ulk();
    return kIOReturnSuccess;
}

bool NVGspControl::snapshotNativeFault615() {
    auto *s=native615_;
    if(native615Stopping_ || !s || !s->faultRegistered || s->generation!=resetGeneration() ||
       !nativeFaultQueue_.size())return false;
    nvgsp::ShadowFaultQueue615 queue{};
    if(!nativeFaultQueue_.readDeviceRange(0,&queue,sizeof(queue)))return false;
    const UInt64 capacity=s->faultInfo.nonReplayableBytes/nvgsp::kFaultRecordBytes615;
    const bool valid=queue.capacity==capacity && queue.msgSize==32 && !queue.allocator &&
        !queue.data && capacity>1 && queue.get<capacity && queue.put<capacity;
    setProperty("NVGspControl-native615-fault-queue-observed",&queue,sizeof(queue));
    setProperty("NVGspControl-native615-fault-queue-valid",valid);
    if(!valid)return false;
    const UInt64 pending=(queue.put+capacity-queue.get)%capacity;
    setProperty("NVGspControl-native615-fault-pending-records",pending,64);
    // Observation only: capture at most eight raw records and leave GET
    // untouched. Recovery/consumption is a later independently tested step.
    UInt8 raw[8*32]{};
    const UInt32 count=pending>8 ? 8 : UInt32(pending);
    for(UInt32 i=0;i<count;++i) {
        const UInt64 index=(queue.get+i)%capacity;
        if(!nativeFaultData_.readDeviceRange(index*32,raw+i*32,32))return false;
    }
    UInt32 decodedCount=0;
    for(UInt32 i=0;i<count;++i) {
        nvgsp::FaultRecord615 record{};
        if(!nvgsp::decodeFaultRecord615(raw+i*32,32,&record))continue;
        ++decodedCount;
        if(i)continue;
        setProperty("NVGspControl-native615-fault-first-instance-page",record.instancePageAddress,64);
        setProperty("NVGspControl-native615-fault-first-address-page",record.faultPageAddress,64);
        setProperty("NVGspControl-native615-fault-first-timestamp",record.timestamp,64);
        setProperty("NVGspControl-native615-fault-first-type",record.faultType,32);
        setProperty("NVGspControl-native615-fault-first-access",record.accessType,32);
        setProperty("NVGspControl-native615-fault-first-engine",record.engineId,32);
        setProperty("NVGspControl-native615-fault-first-replayable",record.replayable);
        const auto &probe=s->isolation;
        const bool owned=probe.mode==2 && probe.attempted && probe.objectCount==20 && !probe.freedCount &&
            nvgsp::ownedReadOnlyWriteFault615(record,probe.channels[0].instance,probe.channels[0].va+0x1000);
        setProperty("NVGspControl-native615-fault-first-owned-readonly-denial",owned);
        const bool crossVas=probe.mode==5 && probe.attempted && probe.objectCount==20 && !probe.freedCount &&
            nvgsp::ownedCrossVasWriteFault615(record,probe.channels[0].instance,probe.channels[1].va+0x1000);
        setProperty("NVGspControl-native615-fault-first-owned-cross-vas-denial",crossVas);
    }
    setProperty("NVGspControl-native615-fault-decoded-records",decodedCount,32);
    setProperty("NVGspControl-native615-fault-records-observed",raw,count*32);
    setProperty("NVGspControl-native615-fault-records-consumed",s->recovery.consumed?s->recovery.count:0,32);
    return pending==0; // Unexpected records fail closed; observation does not recover.
}
