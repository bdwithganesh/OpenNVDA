#include "NVGspControl.hpp"
#include <IOKit/IOLib.h>

#define super IOUserClient
OSDefineMetaClassAndStructors(IOAccelNVGspUserClient, IOUserClient)

bool IOAccelNVGspUserClient::initWithTask(task_t owningTask, void *securityID,
                                          UInt32 type, OSDictionary *properties) {
    // 0.150.0: any process may open us (apps on the desktop use the GPU
    // through NVMTLDriver / NVVTDecoder); non-admin clients only get the
    // selectors those need, on objects they own (see externalMethod)
    admin_ = clientHasPrivilege(securityID, kIOClientPrivilegeAdministrator) == kIOReturnSuccess;
    task_ = owningTask;                                   // 0.134.0: userMemBind
    return super::initWithTask(owningTask, securityID, type, properties);
}

bool IOAccelNVGspUserClient::start(IOService *provider) {
    if (!super::start(provider)) return false;
    driver_ = OSDynamicCast(NVGspControl, provider);
    if (!driver_) return false;
    driver_->clientLive(this, task_, true);              // 0.177.0
    resetGen_ = driver_->resetGeneration();
    driver_->noteGpuBusy();            // 0.135.0: start ramping out of P8 before the first submit
    // 0.171.0: the 61 MiB firmware package buffer is made when an admin
    // client maps it (nvgsp_load), not for every client: each Metal process
    // wired 61 MiB for nothing, 453 clients held ~28 GiB, new clients then
    // failed start() ("open failed 0xe00002c7", nil textures) and 2 MiB
    // contiguous allocations failed (1 Oct 05:47)
    return true;
}

// 0.171.0: see start()
bool IOAccelNVGspUserClient::ensurePackage() {
    if (package_) return true;
    package_ = IOBufferMemoryDescriptor::withOptions(kIOMemoryKernelUserShared | kIODirectionInOut,
                                                     kNVGspPackageBytes, 4096);
    if (!package_ || !package_->getBytesNoCopy()) { OSSafeReleaseNULL(package_); return false; }
    bzero(package_->getBytesNoCopy(), kNVGspPackageBytes);
    return true;
}

void IOAccelNVGspUserClient::stop(IOService *provider) {
    if (package_) { package_->release(); package_ = nullptr; }
    driver_ = nullptr;
    super::stop(provider);
}

IOReturn IOAccelNVGspUserClient::clientClose() {
    // 0.107.0: owner cleanup; 0.110.0: also hands a presented screen back.
    if (driver_) driver_->clientChannelClose(this);   // 0.178.0: before its memory goes
    if (driver_) driver_->memFreeAll(this);
    if (driver_) driver_->clientLive(this, task_, false);   // 0.177.0
    if (!isInactive()) terminate();
    return kIOReturnSuccess;
}

IOReturn IOAccelNVGspUserClient::clientMemoryForType(
    UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) {
    // 0.107.0: type 0x1000 | handle = a sysmem memory object of this client.
    // 0.123.0: a CPU-visible VRAM object (memAlloc domain 2) maps its BAR1
    // range write-combined.
    // 0.162.0: type 0x40000000 | handle for any handle (the 0x1000 form only
    // reaches 0xfff).
    const bool wide = (type & 0xc0000000) == 0x40000000;
    if ((wide || (type & 0xfffff000) == 0x1000) && driver_ && options && memory) {
        bool vram = false;
        IOMemoryDescriptor *d = driver_->memUserDescriptor(this, wide ? type & 0x3fffffff : type & 0xfff, &vram);
        if (!d) return kIOReturnBadArgument;
        *options = kIOMapAnywhere | (vram ? kIOMapWriteCombineCache : 0);
        *memory = d;
        return kIOReturnSuccess;
    }
    if (!admin_) return kIOReturnNotPrivileged;   // 0.150.0: shared window / package: admin only
    // 0.102.0: type 1 = the CPU/GPU shared window (GPU VA 0x30_0000_0000).
    if (type == 1 && driver_ && driver_->sharedWindowDescriptor() && options && memory) {
        *options = kIOMapAnywhere;
        driver_->sharedWindowDescriptor()->retain();
        *memory = driver_->sharedWindowDescriptor();
        return kIOReturnSuccess;
    }
    if (type != 0 || !options || !memory || !ensurePackage())
        return kIOReturnBadArgument;
    *options = kIOMapAnywhere;
    package_->retain();
    *memory = package_;
    return kIOReturnSuccess;
}

IOReturn IOAccelNVGspUserClient::commit(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !self->package_ || !arguments ||
        arguments->scalarInputCount != 1 ||
        arguments->scalarInput[0] != kNVGspPackageBytes)
        return kIOReturnBadArgument;
    return self->driver_->stagePackage(self->package_->getBytesNoCopy(),
                                       kNVGspPackageBytes);
}

IOReturn IOAccelNVGspUserClient::boot(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments || arguments->scalarInputCount)
        return kIOReturnBadArgument;
    return self->driver_->executeBoot();
}

IOReturn IOAccelNVGspUserClient::status(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments || arguments->scalarInputCount)
        return kIOReturnBadArgument;
    return self->driver_->pollStatus();
}

IOReturn IOAccelNVGspUserClient::setFlags(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments ||
        arguments->scalarInputCount != 1)
        return kIOReturnBadArgument;
    return self->driver_->setExperimentFlags(
        static_cast<UInt32>(arguments->scalarInput[0]));
}

IOReturn IOAccelNVGspUserClient::flip(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments ||
        arguments->scalarInputCount != 1)
        return kIOReturnBadArgument;
    return self->driver_->flipWindow(arguments->scalarInput[0]);
}

IOReturn IOAccelNVGspUserClient::submit(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments ||
        arguments->scalarOutputCount != 1)
        return kIOReturnBadArgument;
    // 0.58.0: >4 KiB inputs arrive as structureInputDescriptor.
    IOMemoryDescriptor *md = arguments->structureInputDescriptor;
    const UInt64 size = md ? md->getLength() : arguments->structureInputSize;
    if (size < 4 || (size & 3) || size > 0x40000 ||
        (!md && !arguments->structureInput))
        return kIOReturnBadArgument;
    UInt32 *copy = static_cast<UInt32 *>(IOMalloc(size));
    if (!copy) return kIOReturnNoMemory;
    IOReturn ret = kIOReturnSuccess;
    if (md) {
        ret = md->prepare();
        if (ret == kIOReturnSuccess) {
            if (md->readBytes(0, copy, size) != size) ret = kIOReturnIOError;
            md->complete();
        }
    } else {
        bcopy(arguments->structureInput, copy, size);
    }
    UInt64 ns = 0;
    if (ret == kIOReturnSuccess)
        ret = arguments->selector == 6
            ? self->driver_->submitCe(self, copy, static_cast<UInt32>(size / 4), &ns)
            : self->driver_->submitGr(self, copy, static_cast<UInt32>(size / 4), &ns);
    IOFree(copy, size);
    arguments->scalarOutput[0] = ns;
    return ret;
}

IOReturn IOAccelNVGspUserClient::ping(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments ||
        arguments->scalarOutputCount != 2)
        return kIOReturnBadArgument;
    return self->driver_->pingGsp(&arguments->scalarOutput[0],
                                  &arguments->scalarOutput[1]);
}

IOReturn IOAccelNVGspUserClient::peek(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments || arguments->scalarInputCount != 2 ||
        !arguments->structureOutput)
        return kIOReturnBadArgument;
    const UInt32 count = static_cast<UInt32>(arguments->scalarInput[1]);
    if (!count || count > 64 || arguments->structureOutputSize < count * 4)
        return kIOReturnBadArgument;
    arguments->structureOutputSize = count * 4;
    return self->driver_->peekBar0(static_cast<UInt32>(arguments->scalarInput[0]), count,
                                   static_cast<UInt32 *>(arguments->structureOutput));
}

IOReturn IOAccelNVGspUserClient::poke(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments || arguments->scalarInputCount != 2)
        return kIOReturnBadArgument;
    return self->driver_->pokeBar0(static_cast<UInt32>(arguments->scalarInput[0]),
                                   static_cast<UInt32>(arguments->scalarInput[1]));
}

// input: u32 function + params; output: u32 rpc result + raw queue entry.
IOReturn IOAccelNVGspUserClient::rpc(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments || !arguments->structureInput ||
        arguments->structureInputSize < 4 || !arguments->structureOutput ||
        arguments->structureOutputSize < 8)
        return kIOReturnBadArgument;
    const UInt8 *in = static_cast<const UInt8 *>(arguments->structureInput);
    UInt32 function = 0;
    __builtin_memcpy(&function, in, 4);
    UInt8 *out = static_cast<UInt8 *>(arguments->structureOutput);
    UInt32 replyBytes = arguments->structureOutputSize - 4, result = ~0U;
    const IOReturn r = self->driver_->userRpc(function, in + 4,
        arguments->structureInputSize - 4, out + 4, &replyBytes, &result);
    __builtin_memcpy(out, &result, 4);
    arguments->structureOutputSize = 4 + replyBytes;
    return r;
}

// 0.81.0: scalar in {vram offset, count, write}; struct in = words to
// write, struct out = words read (<= 1024 dwords).
IOReturn IOAccelNVGspUserClient::vram(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments || arguments->scalarInputCount != 3)
        return kIOReturnBadArgument;
    const UInt32 count = static_cast<UInt32>(arguments->scalarInput[1]);
    const bool write = arguments->scalarInput[2] != 0;
    if (!count || count > 1024) return kIOReturnBadArgument;
    if (write) {
        if (!arguments->structureInput || arguments->structureInputSize < count * 4)
            return kIOReturnBadArgument;
        UInt32 *copy = static_cast<UInt32 *>(IOMalloc(count * 4));
        if (!copy) return kIOReturnNoMemory;
        bcopy(arguments->structureInput, copy, count * 4);
        const IOReturn r = self->driver_->vramAccess(arguments->scalarInput[0], copy, count, true);
        IOFree(copy, count * 4);
        return r;
    }
    if (!arguments->structureOutput || arguments->structureOutputSize < count * 4)
        return kIOReturnBadArgument;
    arguments->structureOutputSize = count * 4;
    return self->driver_->vramAccess(arguments->scalarInput[0],
        static_cast<UInt32 *>(arguments->structureOutput), count, false);
}

// 0.83.0: struct in = core channel method words (<= 256 dwords).
IOReturn IOAccelNVGspUserClient::core(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments || !arguments->structureInput ||
        arguments->structureInputSize < 4 || (arguments->structureInputSize & 3) ||
        arguments->structureInputSize > 1024)
        return kIOReturnBadArgument;
    UInt32 words[256];
    bcopy(arguments->structureInput, words, arguments->structureInputSize);
    return self->driver_->submitCore(words, static_cast<UInt32>(arguments->structureInputSize / 4));
}

// 0.87.0: scalar in {log index, offset}; struct out <= 4096 bytes.
IOReturn IOAccelNVGspUserClient::gsplog(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments || arguments->scalarInputCount != 2 ||
        !arguments->structureOutput || !arguments->structureOutputSize)
        return kIOReturnBadArgument;
    const UInt32 n = arguments->structureOutputSize > 4096 ? 4096 : arguments->structureOutputSize;
    arguments->structureOutputSize = n;
    return self->driver_->readGspLog(static_cast<UInt32>(arguments->scalarInput[0]),
        static_cast<UInt32>(arguments->scalarInput[1]),
        static_cast<UInt8 *>(arguments->structureOutput), n);
}

// 0.93.0: in-kext DP modeset on head 0; scalar out = step status (0 = pass).
IOReturn IOAccelNVGspUserClient::modeset(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments ||
        arguments->scalarOutputCount != 1)
        return kIOReturnBadArgument;
    UInt32 code = ~0U;
    const IOReturn r = self->driver_->modesetHead0(&code);
    arguments->scalarOutput[0] = code;
    return r;
}

// 0.100.7: boot modeset — no-op (cache capture only) when the VBIOS mode is lit.
IOReturn IOAccelNVGspUserClient::modesetLight(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments || arguments->scalarOutputCount != 1)
        return kIOReturnBadArgument;
    UInt32 code = ~0U;
    const IOReturn r = self->driver_->modesetHead0(&code, true);
    arguments->scalarOutput[0] = code;
    return r;
}

// 0.100.0: GSP SR cycle (suspend + resume + modeset) without system sleep.
IOReturn IOAccelNVGspUserClient::srcycle(
    OSObject *target, void *, IOExternalMethodArguments *) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_) return kIOReturnBadArgument;
    return self->driver_->srCycle();
}

// 0.137.0 (D4): 10 scalars = setMode timing; out = modeset code.
IOReturn IOAccelNVGspUserClient::setMode(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments || arguments->scalarInputCount != 10)
        return kIOReturnBadArgument;
    UInt32 t[10];
    for (UInt32 i = 0; i < 10; ++i) t[i] = static_cast<UInt32>(arguments->scalarInput[i]);
    UInt32 code = ~0U;
    const IOReturn r = self->driver_->setMode(t, &code);
    arguments->scalarOutput[0] = code;
    return r;
}

IOReturn IOAccelNVGspUserClient::cursorTest(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments || arguments->scalarInputCount != 1)
        return kIOReturnBadArgument;
    return self->driver_->cursorTest(static_cast<UInt32>(arguments->scalarInput[0]));
}

// 0.145.0: scalar out = 1 when someone else submitted to GR since our last
// submission, so our class state (NVK tex/sampler pools, SLM) must be re-pushed.
IOReturn IOAccelNVGspUserClient::grStateOwner(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments || arguments->scalarOutputCount != 1)
        return kIOReturnBadArgument;
    arguments->scalarOutput[0] = self->driver_->grLastOwnerIsOther(self) ? 1 : 0;
    return kIOReturnSuccess;
}

// No memory allocation, MMIO, fence wait or arena change. Native work must
// validate the connection even when an app reuses every existing resource.
IOReturn IOAccelNVGspUserClient::generationStatus() const {
    if (!driver_) return kIOReturnOffline;
    if (driver_->resetBusy()) return kIOReturnNotReady;
    return resetGen_ == driver_->resetGeneration() ? kIOReturnSuccess : kIOReturnOffline;
}

IOReturn IOAccelNVGspUserClient::externalMethod(
    uint32_t selector, IOExternalMethodArguments *arguments,
    IOExternalMethodDispatch *, OSObject *, void *) {
    // 37 (0.174.0): validate this client's generation, without submitting
    // work. Allowed for non-admin clients; used before native encoding.
    if (selector == 37) {
        if (!arguments || arguments->scalarInputCount || arguments->structureInputSize ||
            arguments->structureInputDescriptor || arguments->scalarOutputCount != 1 ||
            !arguments->scalarOutput) return kIOReturnBadArgument;
        const IOReturn r = generationStatus();
        if (r == kIOReturnSuccess) arguments->scalarOutput[0] = resetGen_;
        return r;
    }
    static const IOExternalMethodDispatch methods[] = {
        {commit, 1, 0, 0, 0},
        {boot, 0, 0, 0, 0},
        {status, 0, 0, 0, 0},
        {setFlags, 1, 0, 0, 0},
        {flip, 1, 0, 0, 0},
        {submit, 0, kIOUCVariableStructureSize, 1, 0},
        {submit, 0, kIOUCVariableStructureSize, 1, 0},
        {ping, 0, 0, 2, 0},
        {peek, 2, 0, 0, kIOUCVariableStructureSize},
        {poke, 2, 0, 0, 0},
        {rpc, 0, kIOUCVariableStructureSize, 0, kIOUCVariableStructureSize},
        {vram, 3, kIOUCVariableStructureSize, 0, kIOUCVariableStructureSize},
        {core, 0, kIOUCVariableStructureSize, 0, 0},
        {gsplog, 2, 0, 0, kIOUCVariableStructureSize},
        {modeset, 0, 0, 1, 0},
        {srcycle, 0, 0, 0, 0},
        {modesetLight, 0, 0, 1, 0},
        {submitAsync, 0, kIOUCVariableStructureSize, 1, 0},   // 17 (0.103.0)
        {fenceWait, 2, 0, 1, 0},                              // 18 (0.103.0)
        {gpuReset, 0, 0, 0, 0},                               // 19 (0.104.0)
        {vaBind, 4, 0, 0, 0},                                 // 20 (0.105.0)
        {execSegments, 1, kIOUCVariableStructureSize, 1, 0},  // 21 (0.106.0)
        {memAlloc, 2, 0, 2, 0},                               // 22 (0.107.0)
        {memFree, 1, 0, 0, 0},                                // 23
        {vaBindObject, kIOUCVariableStructureSize, 0, 0, 0},  // 24 (3 or 5 scalars)
        {vaUnbind, 2, 0, 0, 0},                               // 25 (0.109.0)
        {memInfo, 0, 0, 3, 0},                                // 26 (0.109.0)
        {present, 6, 0, 0, 0},                                // 27 (0.110.0)
        {presentStop, 0, 0, 0, 0},                            // 28 (0.110.0)
        {ceFenceWait, 2, 0, 1, 0},                            // 29 (0.112.0)
        {videoOpen, 1, 0, 2, 0},                              // 30 (0.116.0)
        {engineFenceWait, 3, 0, 1, 0},                        // 31 (0.116.0)
        {accelGo, 0, 0, 0, 0},                                // 32 (0.132.0)
        {userMemBind, 4, 0, 1, 0},                            // 33 (0.134.0)
        {setMode, 10, 0, 1, 0},                               // 34 (0.137.0)
        {cursorTest, 1, 0, 0, 0},                             // 35 (0.138.0)
        {grStateOwner, 0, 0, 1, 0},                           // 36 (0.145.0)
        {nullptr, 0, 0, 0, 0},                                // 37: handled above (0.174.0)
        {memAdopt, 1, 0, 2, 0},                               // 38 (0.177.0)
        {diagnosticChannelKick, 1, 0, 0, 0},                 // 39 (0.177.2), admin only
        {clientChannel, 1, 0, 1, 0},                          // 40 (0.178.0)
    };
    if (selector >= sizeof(methods) / sizeof(methods[0]))
        return kIOReturnUnsupported;
    if (!admin_) {
        switch (selector) {
        case 18: case 21: case 22: case 23: case 24: case 25: case 26: case 29:   // fence wait, submit, memory, VA
        case 30: case 31: case 33: case 38: case 40:                              // video engines, user pages, adopt, own GR channel
            break;
        default:
            return kIOReturnNotPrivileged;   // peek/poke, RPC, PRAMIN, modeset, reset, ...
        }
    }
    // 0.110.0: after a GPU reset the channel, fence sequence and VA space
    // are new; a client from before it must not submit, wait or bind
    // against them (its fence numbers would alias the new sequence).
    // Freeing, present-stop and memInfo stay allowed for cleanup.
    switch (selector) {
    case 17: case 18: case 20: case 21: case 22: case 24: case 25: case 27: case 29:
    case 30: case 31: case 33: case 38: case 39: case 40: {
        // 0.164.0: while a reset runs nobody allocates, binds or submits.
        // A client that reopened in that window bound into tables the reset
        // then dropped (WindowServer's staging gone, CE fault, reset again).
        const IOReturn r = generationStatus();
        if (r != kIOReturnSuccess) return r;
        break;
    }
    default:
        break;
    }
    // 0.178.6: own GR channel before the client's first allocation or
    // submission, so none of its GR work ever ran on the shared channel
    // 0.178.10: up to 3 tries (an open can fail on a busy GSP); the kext
    // drains the shared engines before a late channel goes live
    if (ownTries_ < 3 && driver_ && (selector == 5 || selector == 6 || selector == 17 || selector == 21 || selector == 22) &&
        driver_->ownChannelAuto()) {
        UInt32 chid = 0;
        ownTries_ = driver_->clientChannelOpen(this, &chid) == kIOReturnSuccess ? 3 : ownTries_ + 1;
    }
    return super::externalMethod(selector, arguments,
                                 const_cast<IOExternalMethodDispatch *>(&methods[selector]),
                                 this, nullptr);
}

#undef super

// 0.103.0: struct in = GR pushbuffer words; scalar out = fence sequence.
IOReturn IOAccelNVGspUserClient::submitAsync(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments || arguments->scalarOutputCount != 1)
        return kIOReturnBadArgument;
    const UInt32 size = arguments->structureInputSize;
    if (!arguments->structureInput || size < 4 || (size & 3) || size > 4096)
        return kIOReturnBadArgument;
    UInt32 copy[1024];
    bcopy(arguments->structureInput, copy, size);
    UInt32 seq = 0;
    const IOReturn r = self->driver_->submitGrAsync(self, copy, size / 4, &seq);
    arguments->scalarOutput[0] = seq;
    return r;
}

// 0.103.0: scalar in {seq, timeout us}; scalar out = completed sequence.
IOReturn IOAccelNVGspUserClient::fenceWait(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments || arguments->scalarInputCount != 2 ||
        arguments->scalarOutputCount != 1)
        return kIOReturnBadArgument;
    UInt32 done = 0;
    const IOReturn r = self->driver_->waitGrFence(
        static_cast<UInt32>(arguments->scalarInput[0]),
        static_cast<UInt32>(arguments->scalarInput[1] > 5000000 ? 5000000 : arguments->scalarInput[1]),
        &done, self);
    arguments->scalarOutput[0] = done;
    return r;
}

// 0.104.0: GPU reset without reboot (asynchronous; see NVGspControl::gpuReset).
IOReturn IOAccelNVGspUserClient::gpuReset(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments) return kIOReturnBadArgument;
    return self->driver_->gpuReset();
}

// 0.105.0: scalar in {va, phys, bytes (0 = unbind one 2 MiB page), flags}.
IOReturn IOAccelNVGspUserClient::vaBind(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments || arguments->scalarInputCount != 4)
        return kIOReturnBadArgument;
    return self->driver_->vaBind(self, arguments->scalarInput[0], arguments->scalarInput[1],
                                 arguments->scalarInput[2],
                                 static_cast<UInt32>(arguments->scalarInput[3]));
}

// 0.106.0: scalar in {engine}; struct in = n x {u64 va, u32 dwords, u32 flags};
// scalar out = fence sequence (wait: selector 18 GR, 29 CE, 31 any engine;
// 0.116.0: engine 2 + n = video engine n after selector 30).
IOReturn IOAccelNVGspUserClient::execSegments(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments || arguments->scalarInputCount != 1 ||
        arguments->scalarOutputCount != 1 || !arguments->structureInput)
        return kIOReturnBadArgument;
    const UInt32 size = arguments->structureInputSize;
    if (!size || size % 16 || size > 64 * 16) return kIOReturnBadArgument;
    const UInt32 n = size / 16;
    UInt64 va[64];
    UInt32 dw[64], flags[64];
    const UInt8 *in = static_cast<const UInt8 *>(arguments->structureInput);
    for (UInt32 i = 0; i < n; ++i) {
        __builtin_memcpy(&va[i], in + i * 16, 8);
        __builtin_memcpy(&dw[i], in + i * 16 + 8, 4);
        __builtin_memcpy(&flags[i], in + i * 16 + 12, 4);
    }
    UInt32 seq = 0;
    const IOReturn r = self->driver_->submitSegments(
        self, static_cast<UInt32>(arguments->scalarInput[0]), va, dw, flags, n, &seq);
    arguments->scalarOutput[0] = seq;
    return r;
}

// 0.107.0: scalar in {bytes, domain 0 VRAM / 1 SYS / 2 CPU-visible VRAM (0.123.0)}; out {handle, phys}.
IOReturn IOAccelNVGspUserClient::memAlloc(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments || arguments->scalarInputCount != 2 ||
        arguments->scalarOutputCount != 2)
        return kIOReturnBadArgument;
    UInt32 h = 0;
    UInt64 phys = 0;
    const IOReturn r = self->driver_->memAlloc(self, arguments->scalarInput[0],
        static_cast<UInt32>(arguments->scalarInput[1]), &h, &phys);
    arguments->scalarOutput[0] = h;
    arguments->scalarOutput[1] = phys;
    return r;
}

// 0.177.0: scalar in {handle of an object this process's pre-reset client owns}; out {handle, phys}.
IOReturn IOAccelNVGspUserClient::memAdopt(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments || arguments->scalarInputCount != 1 ||
        arguments->scalarOutputCount != 2)
        return kIOReturnBadArgument;
    const UInt32 h = static_cast<UInt32>(arguments->scalarInput[0]);
    UInt64 phys = 0;
    const IOReturn r = self->driver_->memAdopt(self, self->task_, h, &phys);
    arguments->scalarOutput[0] = r == kIOReturnSuccess ? h : 0;
    arguments->scalarOutput[1] = phys;
    return r;
}

IOReturn IOAccelNVGspUserClient::memFree(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments || arguments->scalarInputCount != 1)
        return kIOReturnBadArgument;
    return self->driver_->memFree(self, static_cast<UInt32>(arguments->scalarInput[0]));
}

IOReturn IOAccelNVGspUserClient::diagnosticChannelKick(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments || arguments->scalarInputCount != 1 ||
        arguments->scalarInput[0] > 0xffffffffULL) return kIOReturnBadArgument;
    return self->driver_->diagnosticChannelKick(static_cast<UInt32>(arguments->scalarInput[0]));
}

// scalar in {handle, va, flags (PTE kind)}.
IOReturn IOAccelNVGspUserClient::vaBindObject(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments ||
        (arguments->scalarInputCount != 3 && arguments->scalarInputCount != 5))
        return kIOReturnBadArgument;
    const bool part = arguments->scalarInputCount == 5;
    return self->driver_->vaBindObject(self, static_cast<UInt32>(arguments->scalarInput[0]),
        arguments->scalarInput[1], static_cast<UInt32>(arguments->scalarInput[2]),
        part ? arguments->scalarInput[3] : 0, part ? arguments->scalarInput[4] : 0);
}

// 0.134.0: scalar in {uaddr, bytes, va, flags (bits 7:0 PTE kind)}; out {handle}.
// 4 KiB aligned; the pages stay wired until memFree(handle).
IOReturn IOAccelNVGspUserClient::userMemBind(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments || arguments->scalarInputCount != 4 ||
        arguments->scalarOutputCount != 1)
        return kIOReturnBadArgument;
    UInt32 h = 0;
    const IOReturn r = self->driver_->userMemBind(self, self->task_, arguments->scalarInput[0],
        arguments->scalarInput[1], arguments->scalarInput[2],
        static_cast<UInt32>(arguments->scalarInput[3]), &h);
    arguments->scalarOutput[0] = h;
    return r;
}

// 0.109.0: scalar in {va, bytes}; 2 MiB aligned range in the user VA arena.
IOReturn IOAccelNVGspUserClient::vaUnbind(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments || arguments->scalarInputCount != 2)
        return kIOReturnBadArgument;
    return self->driver_->vaUnbind(self, arguments->scalarInput[0], arguments->scalarInput[1]);
}

// 0.109.0: scalar out {VRAM heap bytes, VRAM used, SYS used} (all clients).
IOReturn IOAccelNVGspUserClient::memInfo(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments || arguments->scalarOutputCount != 3)
        return kIOReturnBadArgument;
    return self->driver_->memInfo(&arguments->scalarOutput[0], &arguments->scalarOutput[1],
                                  &arguments->scalarOutput[2]);
}

// 0.110.0: scalar in {handle, offset, pitch, width, height, format}.
IOReturn IOAccelNVGspUserClient::present(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments || arguments->scalarInputCount != 6)
        return kIOReturnBadArgument;
    const UInt64 *in = arguments->scalarInput;
    if ((in[0] | in[2] | in[3] | in[4] | in[5]) >> 32) return kIOReturnBadArgument;
    return self->driver_->presentObject(self, static_cast<UInt32>(in[0]), in[1],
        static_cast<UInt32>(in[2]), static_cast<UInt32>(in[3]),
        static_cast<UInt32>(in[4]), static_cast<UInt32>(in[5]));
}

IOReturn IOAccelNVGspUserClient::presentStop(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments) return kIOReturnBadArgument;
    return self->driver_->presentStop(self);
}

// 0.112.0: scalar in {seq, timeout us}; scalar out = completed CE sequence.
IOReturn IOAccelNVGspUserClient::ceFenceWait(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments || arguments->scalarInputCount != 2 ||
        arguments->scalarOutputCount != 1)
        return kIOReturnBadArgument;
    UInt32 done = 0;
    const IOReturn r = self->driver_->waitFence(1,
        static_cast<UInt32>(arguments->scalarInput[0]),
        static_cast<UInt32>(arguments->scalarInput[1] > 5000000 ? 5000000 : arguments->scalarInput[1]),
        &done);
    arguments->scalarOutput[0] = done;
    return r;
}

// 0.116.0 (V1): scalar in {video engine 0 NVDEC0 / 1 NVENC0 / 2 OFA0};
// out {setup stage (12 = ready), RM status of that stage}. Idempotent.
IOReturn IOAccelNVGspUserClient::videoOpen(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments || arguments->scalarInputCount != 1 ||
        arguments->scalarOutputCount != 2 || arguments->scalarInput[0] > 0xff)
        return kIOReturnBadArgument;
    UInt32 stage = 0, status = 0;
    const IOReturn r = self->driver_->videoOpen(
        static_cast<UInt32>(arguments->scalarInput[0]), &stage, &status);
    arguments->scalarOutput[0] = stage;
    arguments->scalarOutput[1] = status;
    return r;
}

// 0.116.0: scalar in {engine (0 GR, 1 CE, 2 + video index), seq, timeout us};
// scalar out = completed sequence.
// 0.132.0: let NVAccelerator (B4) match. Its personality has IOResourceMatch
// NVAcceleratorGo, so it starts only when userspace asks, after the GSP chain,
// under a file-based crash guard (a kernel-set NVRAM marker did not survive a
// panic loop, live 26 Sep).
IOReturn IOAccelNVGspUserClient::accelGo(OSObject *target, void *, IOExternalMethodArguments *) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_) return kIOReturnBadArgument;
    self->driver_->publishResource("NVAcceleratorGo", kOSBooleanTrue);
    self->driver_->setProperty("NVGspControl-accel-go", true);
    return kIOReturnSuccess;
}

IOReturn IOAccelNVGspUserClient::engineFenceWait(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments || arguments->scalarInputCount != 3 ||
        arguments->scalarOutputCount != 1 || arguments->scalarInput[0] > 0xff)
        return kIOReturnBadArgument;
    UInt32 done = 0;
    const IOReturn r = self->driver_->waitFence(
        static_cast<UInt32>(arguments->scalarInput[0]),
        static_cast<UInt32>(arguments->scalarInput[1]),
        static_cast<UInt32>(arguments->scalarInput[2] > 5000000 ? 5000000 : arguments->scalarInput[2]),
        &done, self);
    arguments->scalarOutput[0] = done;
    return r;
}

// 0.178.0: scalar in {1 open, 0 close}; out = physical chid of the client's
// own GR channel. Its later GR submissions (selectors 9, 17, 21 engine 0) and
// GR fence waits (18, 31 engine 0) use that channel and its sequence numbers.
IOReturn IOAccelNVGspUserClient::clientChannel(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments || arguments->scalarInputCount != 1 ||
        arguments->scalarOutputCount != 1 || arguments->scalarInput[0] > 1)
        return kIOReturnBadArgument;
    UInt32 chid = 0;
    IOReturn r = kIOReturnSuccess;
    if (arguments->scalarInput[0]) r = self->driver_->clientChannelOpen(self, &chid);
    else self->driver_->clientChannelClose(self);
    arguments->scalarOutput[0] = chid;
    return r;
}
