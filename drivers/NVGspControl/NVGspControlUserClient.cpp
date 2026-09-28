#include "NVGspControl.hpp"
#include <IOKit/IOLib.h>

#define super IOUserClient
OSDefineMetaClassAndStructors(IOAccelNVGspUserClient, IOUserClient)

bool IOAccelNVGspUserClient::initWithTask(task_t owningTask, void *securityID,
                                          UInt32 type, OSDictionary *properties) {
    // any process may open us (apps on the desktop use the GPU
    // through NVMTLDriver / NVVTDecoder); non-admin clients only get the
    // selectors those need, on objects they own (see externalMethod)
    admin_ = clientHasPrivilege(securityID, kIOClientPrivilegeAdministrator) == kIOReturnSuccess;
    task_ = owningTask;                                   // userMemBind
    return super::initWithTask(owningTask, securityID, type, properties);
}

bool IOAccelNVGspUserClient::start(IOService *provider) {
    if (!super::start(provider)) return false;
    driver_ = OSDynamicCast(NVGspControl, provider);
    if (!driver_) return false;
    resetGen_ = driver_->resetGeneration();
    driver_->noteGpuBusy();            // start ramping out of P8 before the first submit
    package_ = IOBufferMemoryDescriptor::withOptions(
        kIOMemoryKernelUserShared | kIODirectionInOut,
        kNVGspPackageBytes, 4096);
    if (!package_ || !package_->getBytesNoCopy()) return false;
    bzero(package_->getBytesNoCopy(), kNVGspPackageBytes);
    return true;
}

void IOAccelNVGspUserClient::stop(IOService *provider) {
    if (package_) { package_->release(); package_ = nullptr; }
    driver_ = nullptr;
    super::stop(provider);
}

IOReturn IOAccelNVGspUserClient::clientClose() {
    // owner cleanup; 0.110.0: also hands a presented screen back.
    if (driver_) driver_->memFreeAll(this);
    if (!isInactive()) terminate();
    return kIOReturnSuccess;
}

IOReturn IOAccelNVGspUserClient::clientMemoryForType(
    UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) {
    // Type 0x1000 | handle = a sysmem memory object of this client. a
    // CPU-visible VRAM object (memAlloc domain 2) maps its BAR1 range
    // write-combined.
    if ((type & 0xfffff000) == 0x1000 && driver_ && options && memory) {
        bool vram = false;
        IOMemoryDescriptor *d = driver_->memUserDescriptor(this, type & 0xfff, &vram);
        if (!d) return kIOReturnBadArgument;
        *options = kIOMapAnywhere | (vram ? kIOMapWriteCombineCache : 0);
        *memory = d;
        return kIOReturnSuccess;
    }
    if (!admin_) return kIOReturnNotPrivileged;   // shared window / package: admin only
    // type 1 = the CPU/GPU shared window (GPU VA 0x30_0000_0000).
    if (type == 1 && driver_ && driver_->sharedWindowDescriptor() && options && memory) {
        *options = kIOMapAnywhere;
        driver_->sharedWindowDescriptor()->retain();
        *memory = driver_->sharedWindowDescriptor();
        return kIOReturnSuccess;
    }
    if (type != 0 || !package_ || !options || !memory)
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
    // >4 KiB inputs arrive as structureInputDescriptor.
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

// scalar in {vram offset, count, write}; struct in = words to
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

// struct in = core channel method words (<= 256 dwords).
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

// scalar in {log index, offset}; struct out <= 4096 bytes.
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

// in-kext DP modeset on head 0; scalar out = step status (0 = pass).
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

// boot modeset - no-op (cache capture only) when the VBIOS mode is lit.
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

// GSP SR cycle (suspend + resume + modeset) without system sleep.
IOReturn IOAccelNVGspUserClient::srcycle(
    OSObject *target, void *, IOExternalMethodArguments *) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_) return kIOReturnBadArgument;
    return self->driver_->srCycle();
}

// 10 scalars = setMode timing; out = modeset code.
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

// scalar out = 1 when someone else submitted to GR since our last
// submission, so our class state (NVK tex/sampler pools, SLM) must be re-pushed.
IOReturn IOAccelNVGspUserClient::grStateOwner(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments || arguments->scalarOutputCount != 1)
        return kIOReturnBadArgument;
    arguments->scalarOutput[0] = self->driver_->grLastOwnerIsOther(self) ? 1 : 0;
    return kIOReturnSuccess;
}

IOReturn IOAccelNVGspUserClient::externalMethod(
    uint32_t selector, IOExternalMethodArguments *arguments,
    IOExternalMethodDispatch *, OSObject *, void *) {
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
        {submitAsync, 0, kIOUCVariableStructureSize, 1, 0},   // 17
        {fenceWait, 2, 0, 1, 0},                              // 18
        {gpuReset, 0, 0, 0, 0},                               // 19
        {vaBind, 4, 0, 0, 0},                                 // 20
        {execSegments, 1, kIOUCVariableStructureSize, 1, 0},  // 21
        {memAlloc, 2, 0, 2, 0},                               // 22
        {memFree, 1, 0, 0, 0},                                // 23
        {vaBindObject, kIOUCVariableStructureSize, 0, 0, 0},  // 24 (3 or 5 scalars)
        {vaUnbind, 2, 0, 0, 0},                               // 25
        {memInfo, 0, 0, 3, 0},                                // 26
        {present, 6, 0, 0, 0},                                // 27
        {presentStop, 0, 0, 0, 0},                            // 28
        {ceFenceWait, 2, 0, 1, 0},                            // 29
        {videoOpen, 1, 0, 2, 0},                              // 30
        {engineFenceWait, 3, 0, 1, 0},                        // 31
        {accelGo, 0, 0, 0, 0},                                // 32
        {userMemBind, 4, 0, 1, 0},                            // 33
        {setMode, 10, 0, 1, 0},                               // 34
        {cursorTest, 1, 0, 0, 0},                             // 35
        {grStateOwner, 0, 0, 1, 0},                           // 36
    };
    if (selector >= sizeof(methods) / sizeof(methods[0]))
        return kIOReturnUnsupported;
    if (!admin_) {
        switch (selector) {
        case 18: case 21: case 22: case 23: case 24: case 25: case 26: case 29:   // fence wait, submit, memory, VA
        case 30: case 31: case 33:                                                // video engines, user pages
            break;
        default:
            return kIOReturnNotPrivileged;   // peek/poke, RPC, PRAMIN, modeset, reset, ...
        }
    }
    // after a GPU reset the channel, fence sequence and VA space
    // are new; a client from before it must not submit, wait or bind
    // against them (its fence numbers would alias the new sequence).
    // Freeing, present-stop and memInfo stay allowed for cleanup.
    switch (selector) {
    case 17: case 18: case 20: case 21: case 22: case 24: case 25: case 27: case 29:
    case 30: case 31:
        if (!driver_ || resetGen_ != driver_->resetGeneration()) return kIOReturnOffline;
        break;
    default:
        break;
    }
    return super::externalMethod(selector, arguments,
                                 const_cast<IOExternalMethodDispatch *>(&methods[selector]),
                                 this, nullptr);
}

#undef super

// struct in = GR pushbuffer words; scalar out = fence sequence.
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

// scalar in {seq, timeout us}; scalar out = completed sequence.
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
        &done);
    arguments->scalarOutput[0] = done;
    return r;
}

// GPU reset without reboot (asynchronous; see NVGspControl::gpuReset).
IOReturn IOAccelNVGspUserClient::gpuReset(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments) return kIOReturnBadArgument;
    return self->driver_->gpuReset();
}

// scalar in {va, phys, bytes (0 = unbind one 2 MiB page), flags}.
IOReturn IOAccelNVGspUserClient::vaBind(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments || arguments->scalarInputCount != 4)
        return kIOReturnBadArgument;
    return self->driver_->vaBind(self, arguments->scalarInput[0], arguments->scalarInput[1],
                                 arguments->scalarInput[2],
                                 static_cast<UInt32>(arguments->scalarInput[3]));
}

// scalar in {engine}; struct in = n x {u64 va, u32 dwords, u32 flags};
// scalar out = fence sequence (wait: selector 18 GR, 29 CE, 31 any engine;
// engine 2 + n = video engine n after selector 30).
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

// scalar in {bytes, domain 0 VRAM / 1 SYS / 2 CPU-visible VRAM}; out {handle, phys}.
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

IOReturn IOAccelNVGspUserClient::memFree(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments || arguments->scalarInputCount != 1)
        return kIOReturnBadArgument;
    return self->driver_->memFree(self, static_cast<UInt32>(arguments->scalarInput[0]));
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

// scalar in {uaddr, bytes, va, flags (bits 7:0 PTE kind)}; out {handle}.
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

// scalar in {va, bytes}; 2 MiB aligned range in the user VA arena.
IOReturn IOAccelNVGspUserClient::vaUnbind(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments || arguments->scalarInputCount != 2)
        return kIOReturnBadArgument;
    return self->driver_->vaUnbind(self, arguments->scalarInput[0], arguments->scalarInput[1]);
}

// scalar out {VRAM heap bytes, VRAM used, SYS used} (all clients).
IOReturn IOAccelNVGspUserClient::memInfo(
    OSObject *target, void *, IOExternalMethodArguments *arguments) {
    auto *self = OSDynamicCast(IOAccelNVGspUserClient, target);
    if (!self || !self->driver_ || !arguments || arguments->scalarOutputCount != 3)
        return kIOReturnBadArgument;
    return self->driver_->memInfo(&arguments->scalarOutput[0], &arguments->scalarOutput[1],
                                  &arguments->scalarOutput[2]);
}

// scalar in {handle, offset, pitch, width, height, format}.
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

// scalar in {seq, timeout us}; scalar out = completed CE sequence.
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

// scalar in {video engine 0 NVDEC0 / 1 NVENC0 / 2 OFA0};
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

// scalar in {engine (0 GR, 1 CE, 2 + video index), seq, timeout us};
// scalar out = completed sequence.
// let NVAccelerator match. Its personality has IOResourceMatch
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
        &done);
    arguments->scalarOutput[0] = done;
    return r;
}
