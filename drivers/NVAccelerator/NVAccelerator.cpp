// NVAccelerator: the RTX 4080 as an IOAcceleratorFamily2 accelerator, the
// kernel half of Metal (B4, milestone M14 "Metal Device").
//
// 0.1.0 = M1 skeleton: the family's user clients open and Metal can enumerate
// the device. Every vendor hook is implemented the way AppleParavirtGPU (KDK
// 14.8.9) does it where that is known, otherwise as a logged stub. GPU work
// (memory, submission) goes through NVGspControl in later milestones.
// 0.1.5 = M2 memory: NVAccelSysMemory subclass (the family's MetaClass::alloc
// returns NULL), factory tracing.
//
// Safety: a panic in an accelerator path would repeat on every boot (Aux KC).
// A kernel-set NVRAM marker did not survive such a loop (0.1.2, 26 Sep), so
// the accelerator only starts after NVGspControl publishes the resource
// NVAcceleratorGo, which userspace (tools/nvaccel/nvaccel-go.sh) does after
// the GSP chain under a file-based crash guard.
// 0.1.6: that gate used to be IOResourceMatch on the GPU personality. IOKit's
// checkResource() parks the provider's matching thread in waitForService()
// until the resource exists, so VGA@0 stayed busy from boot until someone
// ran nvaccel_go; WindowServer waits for the GPU to go quiet, timed out after
// 240 s ("FB: 0 of 1 opened") and respawned forever: Apple logo stuck (27 Sep).
// Now nothing matches the GPU: NVAccelLauncher sits on IOResources, polls for
// the resource once a second and attaches/starts NVAccelerator on the RTX
// itself.
#include "IOAF2.hpp"
#include "IOAF2Pipe.hpp"
#include "AGDC.hpp"
#include "../NVGspCore/NVGspKernelApi.hpp"
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IORangeAllocator.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <kern/clock.h>
#ifndef KEXT_BUNDLE_VERSION
#define KEXT_BUNDLE_VERSION "unknown"
#endif

#define NVALOG(fmt, ...) IOLog("NVAccelerator: " fmt "\n", ##__VA_ARGS__)

// ---------------------------------------------------------------- event machine
// 0.4.0 (native N1): what the family hands the vendor as a
// vendevtCommandRec when it wants a stamp written. We only note which stamp
// and value; NVAccelChannel::submitBuffer releases it on the GPU.
struct NVStampRec {
    UInt32 magic;             // 'NVSR'
    SInt32 idx;
    UInt32 value;
};
static constexpr UInt32 kNVStampRecMagic = 0x4E565352;

class NVAccelEventMachine : public IOAccelEventMachineFast2 {
    OSDeclareDefaultStructors(NVAccelEventMachine)
public:
    // AppleParavirtEventMachine::writeStamp = CommandAllocator::addSignal(idx, value)
    // 0.4.1: AppleParavirtEventMachine::init = super + setStampBaseAddress(
    // accel->getStampBaseAddress()); without it the per-stamp table (+0x28)
    // stays NULL and signalStamp faults (0.4.0 panic, 28 Sep 19:00)
    bool init(IOGraphicsAccelerator2 *a, unsigned int n, int t) override;
    void *writeStamp(int idx, vendevtCommandRec *rec, unsigned int value) override {
        auto *r = reinterpret_cast<NVStampRec *>(rec);
        if (r && r->magic == kNVStampRecMagic) { r->idx = idx; r->value = value; }
        return nullptr;
    }
    void *prepareBarrier(vendevtBarrierRec *) override { return nullptr; }
    void *completeBarrier(vendevtBarrierRec *) override { return nullptr; }
    void *writeBarrierElement(vendevtBarrierRec *, int, unsigned int) override { return nullptr; }
};
OSDefineMetaClassAndStructors(NVAccelEventMachine, IOAccelEventMachineFast2)

// ---------------------------------------------------------------- display machine
// IOGraphicsAccelerator2::start requires one ("fDisplayMachine alloc failed",
// live 0.1.1). Mode changes are NVDisplay's business; nothing to do yet.
extern unsigned ioaf2DmFramebufferCount(const IOAccelDisplayMachine *m)
    __asm("__ZNK21IOAccelDisplayMachine19getFramebufferCountEv");
extern IOAccelDisplayPipe *ioaf2DmDisplayPipe(const IOAccelDisplayMachine *m, unsigned idx)
    __asm("__ZNK21IOAccelDisplayMachine14getDisplayPipeEj");
extern void *ioaf2PipeIsActive(const IOAccelDisplayPipe *p) __asm("__ZNK18IOAccelDisplayPipe8isActiveEv");
extern IOAccelResource2 *ioaf2PipeFbResource(const IOAccelDisplayPipe *p, unsigned idx)
    __asm("__ZNK18IOAccelDisplayPipe22getFramebufferResourceEj");
extern void *ioaf2PipeInitFbResource(IOAccelDisplayPipe *p, IOAccelResource2 *r)
    __asm("__ZN18IOAccelDisplayPipe25init_framebuffer_resourceEP16IOAccelResource2");
extern void ioaf2PipeOnlineChanged(IOAccelDisplayPipe *p, bool on)
    __asm("__ZN18IOAccelDisplayPipe33framebuffer_online_status_changedEb");

class NVAccelDisplayMachine : public IOAccelDisplayMachine {
    OSDeclareDefaultStructors(NVAccelDisplayMachine)
public:
    // bool: returning false panics in IOAccelDisplayMachine::display_mode_did_change
    // ("driver returns false", WindowServer opening the framebuffer, live 0.1.2)
    bool displayModeWillChange() override { return true; }
    bool displayModeDidChange() override { return true; }
    // 0.2.1: the family looks for IOFramebuffers among the PCI device's
    // children only; NVDisplay sits under NVGspControl, so hand it over
    bool start(IOPCIDevice *pci) override;
    bool started_ = false;
    unsigned attachNVDisplay();
    void activatePipe(unsigned idx, IOFramebuffer *fb);
    // 0.5.34: NVDisplay 1.3.0 does not publish the display until an
    // IOAccelerator is registered, so keep looking for it here.
    thread_call_t fbRetry_ = nullptr;
    unsigned fbTries_ = 0;
    bool fbDone_ = false;
    static void fbRetryCallout(thread_call_param_t p, thread_call_param_t);
    void scheduleFbRetry();
};
OSDefineMetaClassAndStructors(NVAccelDisplayMachine, IOAccelDisplayMachine)

bool NVAccelDisplayMachine::start(IOPCIDevice *pci) {
    NVALOG("display machine: start");
    // 0.5.23: the family did not call this (its start only iterates PCI
    // children for framebuffers and NVDisplay is not a child), so the machine
    // was left un-started and the family's user client reported no display
    // pipes ("System set to use IOP but no display pipes are present",
    // WindowServer). Call the family's start ourselves, then add NVDisplay.
    const bool base = IOAccelDisplayMachine::start(pci);
    NVALOG("display machine: base start %s", base ? "true" : "false");
    if (!attachNVDisplay()) scheduleFbRetry();     // 0.5.34: display may come later
    started_ = true;
    return true;
}

void NVAccelDisplayMachine::scheduleFbRetry() {
    if (fbDone_ || fbTries_ >= 90) return;
    if (!fbRetry_) fbRetry_ = thread_call_allocate(&NVAccelDisplayMachine::fbRetryCallout, this);
    if (!fbRetry_) return;
    UInt64 deadline = 0;
    clock_interval_to_deadline(2, kSecondScale, &deadline);
    thread_call_enter_delayed(fbRetry_, deadline);
}

void NVAccelDisplayMachine::fbRetryCallout(thread_call_param_t p, thread_call_param_t) {
    NVAccelDisplayMachine *m = static_cast<NVAccelDisplayMachine *>(p);
    if (!m) return;
    if (m->attachNVDisplay()) {
        NVALOG("display machine: framebuffer arrived after %u tries", m->fbTries_);
        m->fbDone_ = true;
        return;
    }
    ++m->fbTries_;
    if (m->fbTries_ >= 90) NVALOG("display machine: no framebuffer after %u tries, giving up", m->fbTries_);
    else m->scheduleFbRetry();
}

// 0.5.27: the framebuffers this GPU composites for, NVDisplay first (pipe 0,
// AGDC framebuffer 0), then every other IOFramebuffer (the Hyper-V synthetic
// display). With the accelerator up, WindowServer gives each display that has
// a display pipe the Metal compositor and the rest the software one; mixing
// them aborted every WindowServer started after the accelerator (flattened
// window surfaces made by CompositorMetal reached CompositorSW's SWContext,
// which never tracks them: push_surface "found != slist->end ()").
// Returns the count; each entry is retained. Empty without NVDisplay.
static unsigned nvCopyFramebuffers(IOFramebuffer **out, unsigned max) {
    unsigned n = 0;
    OSDictionary *match = IOService::serviceMatching("NVDisplay");
    IOFramebuffer *nv = OSDynamicCast(IOFramebuffer, match ? IOService::copyMatchingService(match) : nullptr);
    OSSafeReleaseNULL(match);
    if (!nv || !max) { OSSafeReleaseNULL(nv); return 0; }
    out[n++] = nv;
    match = IOService::serviceMatching("IOFramebuffer");
    OSIterator *it = match ? IOService::getMatchingServices(match) : nullptr;
    OSSafeReleaseNULL(match);
    while (OSObject *o = it ? it->getNextObject() : nullptr) {
        IOFramebuffer *fb = OSDynamicCast(IOFramebuffer, o);
        if (!fb || fb == nv || fb->metaCast("NVDisplay") || n >= max) continue;
        fb->retain();
        out[n++] = fb;
    }
    OSSafeReleaseNULL(it);
    return n;
}

unsigned NVAccelDisplayMachine::attachNVDisplay() {
    if (fbDone_) return 1;
    IOFramebuffer *fbs[2] = {};
    const unsigned nfb = nvCopyFramebuffers(fbs, 2);
    for (unsigned i = 0; i < nfb; ++i) {
        const bool ok = (uintptr_t)found_framebuffer(fbs[i]) & 0xff;   // returns bool
        NVALOG("display machine: framebuffer %u %s %p, found_framebuffer %s", i, fbs[i]->getMetaClass()->getClassName(),
               fbs[i], ok ? "true" : "false");
    }
    // 0.5.26: without a framebuffer driver the pipe still exists and the mode
    // query below dereferenced fb (panic at attachNVDisplay+0x154)
    if (!nfb) return 0;
    // 0.2.4: the pipe comes after the framebuffer went online, so it never saw
    // that notification: tell it, then check what WindowServer will see
    const unsigned count = ioaf2DmFramebufferCount(this);
    for (unsigned i = 0; i < count && i < nfb; ++i) activatePipe(i, fbs[i]);
    for (unsigned i = 0; i < nfb; ++i) OSSafeReleaseNULL(fbs[i]);
    fbDone_ = true;
    return nfb;
}

void NVAccelDisplayMachine::activatePipe(unsigned idx, IOFramebuffer *fb) {
    IOAccelDisplayPipe *pipe = ioaf2DmDisplayPipe(this, idx);
    bool active = pipe && (((uintptr_t)ioaf2PipeIsActive(pipe)) & 0xff);
    NVALOG("display machine: pipe %u %p, active %d", idx, pipe, active);
    if (pipe && !active) {
        // displayModeDidChange registers the framebuffer's VBL interrupt and
        // runs init_framebuffer_resource, which marks the pipe active; the
        // family only calls it on a mode change, and ours came first
        pipe->displayModeDidChange();
        active = ((uintptr_t)ioaf2PipeIsActive(pipe)) & 0xff;
        IOAccelResource2 *res = ioaf2PipeFbResource(pipe, 0);
        NVALOG("display machine: pipe displayModeDidChange, active %d, framebuffer resource %p", active, res);
        {
            IODisplayModeID mode = 0; IOIndex depth = 0; IOPixelInformation pi{};
            const IOReturn r1 = fb->getCurrentDisplayMode(&mode, &depth);
            const IOReturn r2 = fb->getPixelInformation(mode, depth, kIOFBSystemAperture, &pi);
            NVALOG("display machine: fb mode 0x%x depth %d (0x%x), pixel info 0x%x: %ux%u %u bpp %u bpc rowbytes %u, res 0x%llx",
                   mode, depth, r1, r2, pi.activeWidth, pi.activeHeight, pi.bitsPerPixel, pi.bitsPerComponent,
                   pi.bytesPerRow, (unsigned long long)(uintptr_t)res);
        }
        if (!active && res) {
            const bool r = ((uintptr_t)ioaf2PipeInitFbResource(pipe, res)) & 0xff;
            active = ((uintptr_t)ioaf2PipeIsActive(pipe)) & 0xff;
            NVALOG("display machine: init_framebuffer_resource %d, active %d", r, active);
        }
    }
}

// ---------------------------------------------------------------- display pipe
// 0.2.0 (W1): WindowServer composites into IOSurfaces and hands them to the
// display pipe of the framebuffer (without one: "Unable to find display
// pipe for IOFB service"). v1 copies plane 0 of each transaction into
// NVDisplay's scan-out memory (the aperture the software path used) and
// completes at once; the family's own VBL handling (IOFramebuffer VBL
// interrupt -> signalVBLInterrupt -> signalTransactionInterrupt) retires
// transactions, so enable/disableVBLInterrupt stay the base ones.
// Kernel entry points of other kexts, bound by their mangled names:
extern void *ioaf2TxnPlaneSurface(const IOAccelDisplayPipeTransaction2 *t, unsigned int plane, unsigned int idx)
    __asm("__ZNK30IOAccelDisplayPipeTransaction217getPlaneIOSurfaceEjj");
extern IOMemoryDescriptor *iosurfMemory(void *s, IOService *forService)
    __asm("__ZN9IOSurface19getMemoryDescriptorEP9IOService");
// returns a 32-bit value (the upper half of rax is not defined)
extern uint32_t iosurfBytesPerRow(const void *s) __asm("__ZNK9IOSurface14getBytesPerRowEv");
extern uint64_t iosurfAllocSize(const void *s) __asm("__ZNK9IOSurface12getAllocSizeEv");
extern uint32_t iosurfPixelFormat(const void *s) __asm("__ZNK9IOSurface14getPixelFormatEv");

// 0.5.9: how an asynchronous display pipe retires its transactions: once the
// flip is done, signalTransactionInterrupt() (it only acts while the family
// has transaction interrupts enabled, +0x2a4) kicks the family's workloop;
// transaction_interrupt_gated asks isTransactionComplete(live) and then
// completeTransaction. 0.5.7/0.5.8 never signalled: releaseLiveTransaction in
// a display change slept for good and WindowServer hung (IOAcceleratorFamily2
// 487.4.4 disassembly, KDK 25G83).
extern void ioaf2SignalTxnInterrupt(IOAccelDisplayPipe *p, void *ref)
    __asm("__ZN18IOAccelDisplayPipe26signalTransactionInterruptEPv");
extern void *ioaf2SysMemWithMD(IOGraphicsAccelerator2 *a, IOAccelResource2 *r, IOMemoryDescriptor *md)
    __asm("__ZN16IOAccelSysMemory20withMemoryDescriptorEP22IOGraphicsAccelerator2P16IOAccelResource2P18IOMemoryDescriptor");

// 0.5.37: a transaction's colour state (IOAcceleratorFamily2, KDK 26.7.2
// set_pipe_postgamma_table and friends): +0x38 points at the tables (+0
// pre-gamma, +8 post-gamma, +0x40 + 0x70 * plane plane gamma), each an
// IOAccelDisplayPipeGammaTable2 (+0x10 data, +0x18 size); +0x48 holds the
// dirty bits (0x4 << plane plane gamma, 0x20 pre, 0x80 post, 0x400 post-CSC
// VID). Table data: {u32 count, 12 bytes, count x {float r, g, b, pad}}.
static constexpr unsigned kTxnColor = 0x38, kTxnDirty = 0x48;
static constexpr UInt64 kDirtyPostGamma = 0x80, kDirtyColorOther = 0x4 | 0x8 | 0x20 | 0x400;
static constexpr UInt32 kGamma = 1024;   // entries CoreDisplay sends and the OLUT takes

// [0, 1] float bits -> UNORM16 with integer maths (kernel code keeps off the FPU)
static UInt16 unorm16(UInt32 f) {
    const UInt32 e = (f >> 23) & 0xff;
    if (f & 0x80000000u) return 0;
    if (e >= 127) return 0xffff;
    const UInt32 sh = 150 - e;                    // value = mantissa * 2^-sh
    if (sh >= 48) return 0;
    const UInt64 m = (f & 0x7fffffu) | 0x800000u;
    return static_cast<UInt16>((m * 65535u + (1ULL << (sh - 1))) >> sh);
}

class NVAccelDisplayPipe : public IOAccelDisplayPipe {
    OSDeclareDefaultStructors(NVAccelDisplayPipe)
    IOFramebuffer *fb_ = nullptr;
    IOGraphicsAccelerator2 *accel_ = nullptr;   // not retained (it owns us)
    IOBufferMemoryDescriptor *fbMem_ = nullptr;   // 0.2.9: sysmem framebuffer resource backing
    IOMemoryDescriptor *fbScan_ = nullptr;        // 0.5.33: the live aperture as the FB resource
    IOMemoryMap *map_ = nullptr;
    UInt32 fbRowBytes_ = 0, fbHeight_ = 0, fbWidthBytes_ = 0;
    UInt64 copies_ = 0, empty_ = 0, lastFormat_ = 0;
    // 0.3.9: surfaces WindowServer flips, wired and mapped once (it cycles a
    // few). readBytes on an unprepared descriptor panics (IOGMD: not wired).
    struct SurfMap { IOMemoryDescriptor *md; IOMemoryMap *map; UInt64 used; };
    SurfMap surf_[6] = {};
    UInt64 copyTicks_ = 0;
    UInt64 surfTick_ = 0;
    // 0.4.9 (native N4): the flip on the kernel copy engine (NVGspControl
    // "nvgsp-flip-copy"); the CPU copy stays as the fallback. NVRAM
    // nvaccel-ceflip=0 keeps the CPU copy.
    UInt64 apBus_ = 0;
    // 0.5.30: the native615 display pipeline scans out from a VRAM surface;
    // NVGspControl publishes it ("NVGspControl-display615-scanout-phys") and
    // repairs the flip into it instead of into the framebuffer aperture (the
    // CPU copy into VRAM was 3.3 s for 4K, the copy engine takes ~8 ms).
    UInt64 scanoutBus_ = 0;
    IOService *gsp_ = nullptr;
    int ceFlip_ = -1;
    // 0.5.27: a framebuffer that is not NVDisplay (Hyper-V synthetic video):
    // its aperture is not reachable by our copy engine, CPU copy only
    bool foreign_ = false;
    // 0.5.37: with the GPU colour pass gone, CoreDisplay's colour arrives as
    // the transaction's pipe post-gamma table; the head OLUT applies it
    void applyPostGamma(IOAccelDisplayPipeTransaction2 *t);
    UInt16 *gamma_ = nullptr;          // 3 x kGamma (R, G, B planes), the nvgsp-gamma-set layout
    UInt64 gammaHash_ = 0, gammaSets_ = 0, gammaFails_ = 0, colorOther_ = 0;
    IOReturn gammaLast_ = 0;
    UInt64 ceFlips_ = 0, ceFails_ = 0;
    UInt64 ceRetryAt_ = 0;   // 0.5.20: after a boot-time burst of failures, retry CE later
    IOReturn ceLastErr_ = 0;
    // 0.5.1: vsync. The CPU copy went into the live scan-out buffer at any
    // point of the frame, so the beam showed half old, half new (glitches,
    // tearing). Now the copy starts right after a vblank: 1.5 ms for a 4K
    // frame against 16.7 ms of scan-out, it stays ahead of the beam. The
    // vblank comes from NVGspControl's display interrupt callback. NVRAM
    // nvaccel-vsync=0 copies at once, as before.
    IOLock *vblLock_ = nullptr;
    volatile UInt32 vblSeen_ = 0;
    int vsync_ = -1;                         // -1 not set up, 0 off, 1 on
    UInt64 vblWaits_ = 0, vblTimeouts_ = 0;
    static void onVblank(void *ref, UInt32 count, UInt64 at);
    void waitVblank();
    // 0.5.7: asynchronous flips. submitTransaction used to wait for the next
    // vblank and copy on WindowServer's thread (up to ~18 ms a frame blocked:
    // the compositor could not prepare the next frame, animations stuttered).
    // Now it queues the surface and returns; a thread call started by the
    // vblank callback copies it right after the vblank, and the transaction
    // stays incomplete (isTransactionComplete) until its copy is done, so
    // WindowServer does not reuse the surface early. A newer submit replaces
    // a queued one that was not copied yet (that frame is skipped, as a swap
    // queue does). NVRAM nvaccel-asyncflip=0 keeps the synchronous path.
    // 0.5.8: the family waits for the live transaction in display changes
    // (releaseLiveTransaction), when vblanks may stop while the head is
    // reprogrammed: 0.5.7 then never copied, WindowServer hung, watchdog
    // panic. A 25 ms backstop call copies without a vblank, and a transaction
    // pending for more than 100 ms reads complete whatever happens.
    IOLock *flipLock_ = nullptr;
    thread_call_t flipCall_ = nullptr, flipBackstop_ = nullptr;
    volatile UInt64 pendingSince_ = 0;
    int asyncFlip_ = -1;
    struct PendingFlip { IOAccelDisplayPipeTransaction2 *t; IOMemoryDescriptor *md; UInt64 bpr, size; UInt32 fmt;
                         bool ringWait; UInt32 grTarget, ceTarget; };
    PendingFlip pending_ = {};
    IOAccelDisplayPipeTransaction2 *volatile pendingTxn_ = nullptr;   // queued, not copied yet
    IOAccelDisplayPipeTransaction2 *volatile copyingTxn_ = nullptr;   // being copied now
    UInt64 asyncFlips_ = 0, skippedFlips_ = 0;
    // 0.5.19: frame-pacing diagnostics. Every copy is one presented frame:
    // record the submit->copy latency and the copy-to-copy interval in a
    // 256-entry ring, plus aggregates, and publish them on the accelerator
    // so userspace can measure compositor smoothness without kernel logs.
    UInt64 stSubmits_ = 0, stCopies_ = 0, stNoCopy_ = 0;
    UInt64 stLastSubmit_ = 0, stLastCopy_ = 0, stSubmitAt_ = 0;
    UInt64 stSumSubmitGap_ = 0, stMaxSubmitGap_ = 0, stSubmitGaps_ = 0;
    UInt64 stSumCopyGap_ = 0, stMaxCopyGap_ = 0, stCopyGaps_ = 0;
    UInt64 stLate20_ = 0, stLate33_ = 0;
    UInt64 stSumLat_ = 0, stMaxLat_ = 0, stLatCount_ = 0;
    UInt32 stRingHead_ = 0;
    UInt32 stIntervalRing_[256] = {};
    UInt32 stLatencyRing_[256] = {};
    void statsReset();
    void statsSubmit();
    void statsCopy();
    void statsPublish();
    static void flipCallout(thread_call_param_t self, thread_call_param_t);
    UInt64 renderWaits_ = 0;
    // 0.5.13: the last frame on screen, put back after a GPU reset wiped the
    // scan-out memory (black until WindowServer's next flip)
    PendingFlip lastFlip_ = {};
    thread_call_t restoreCall_ = nullptr;
    UInt32 restores_ = 0;
    static void onResetDone(void *ref);
    static void restoreCallout(thread_call_param_t self, thread_call_param_t);
    UInt32 eventLogs_ = 0;
    bool renderDone(IOAccelDisplayPipeTransaction2 *t);
    bool ringProgress(UInt32 p[4]);                   // 0.5.11: NVGspControl "nvgsp-ring-progress"
    bool ringsPassed(const PendingFlip &f);
    bool setupVsync();
    void copySurface(IOMemoryDescriptor *md, UInt64 bpr, UInt64 size, UInt32 fmt);
    IOService *gspService();
    bool flipOnCopyEngine(IOMemoryDescriptor *md, UInt64 bpr, UInt32 rows, UInt32 n);
    IOMemoryMap *surfaceMap(IOMemoryDescriptor *md);
    void dropSurfaces();
    bool mapFramebuffer();
public:
    bool init(IOGraphicsAccelerator2 *a, IOAccelDisplayMachine *m, IOFramebuffer *fb, unsigned int idx) override;
    void free() override;
    // 0.2.8: the framebuffer resource wraps the scan-out memory, as Paravirt's
    // pipe does (IOAccelSysMemory::withMemoryDescriptor); without it
    // init_framebuffer_resource fails and the pipe never becomes active
    void *initFramebufferResource(unsigned int idx, IOAccelResource2 *res) override;
    void *destroyFramebufferResource(unsigned int idx, IOAccelResource2 *res) override;
    bool isTransactionComplete(IOAccelDisplayPipeTransaction2 *t) override {
        if (t != pendingTxn_ && t != copyingTxn_) return true;   // 0.5.7: see asynchronous flips
        UInt64 ns = 0;
        const UInt64 since = pendingSince_;
        if (since) absolutetime_to_nanoseconds(mach_absolute_time() - since, &ns);
        return since && ns > 100000000ULL;                        // 0.5.8: never wait forever
    }
    // 0.3.7: trace how far WindowServer's swaps get into the family
    UInt32 trace_[8] = {};
    bool tr(unsigned k) { return trace_[k] < 6 && ++trace_[k]; }
    void *newDisplayPipeTransaction() override {
        void *t = IOAccelDisplayPipe::newDisplayPipeTransaction();
        if (tr(0)) NVALOG("display pipe: newDisplayPipeTransaction -> %p", t);
        return t;
    }
    IOReturn validateTransaction(IOAccelDisplayPipeTransaction2 *t) override {
        const IOReturn r = kIOReturnSuccess;   // the family's own check wants hardware state we lack
        if (tr(1)) NVALOG("display pipe: validateTransaction %p -> 0x%x", t, r);
        return r;
    }
    void *performTransaction(IOAccelDisplayPipeTransaction2 *t) override {
        void *r = IOAccelDisplayPipe::performTransaction(t);
        if (tr(2)) NVALOG("display pipe: performTransaction %p -> %p", t, r);
        return r;
    }
    void *beginTransaction(IOAccelEvent *e) override {
        void *r = IOAccelDisplayPipe::beginTransaction(e);
        if (tr(3)) NVALOG("display pipe: beginTransaction -> %p", r);
        return r;
    }
    void *wsaaEnterDefer(int a) override { if (tr(4)) NVALOG("display pipe: wsaaEnterDefer %d", a); return IOAccelDisplayPipe::wsaaEnterDefer(a); }
    void *wsaaDidExitDefer(int a) override { if (tr(5)) NVALOG("display pipe: wsaaDidExitDefer %d", a); return IOAccelDisplayPipe::wsaaDidExitDefer(a); }
    void *copyCapabilities() override {
        void *r = IOAccelDisplayPipe::copyCapabilities();
        if (tr(6)) NVALOG("display pipe: copyCapabilities -> %p", r);
        return r;
    }
    IOReturn submitTransaction(IOAccelDisplayPipeTransaction2 *t) override;
};
OSDefineMetaClassAndStructors(NVAccelDisplayPipe, IOAccelDisplayPipe)

bool NVAccelDisplayPipe::init(IOGraphicsAccelerator2 *a, IOAccelDisplayMachine *m, IOFramebuffer *fb, unsigned int idx) {
    const OSSymbol *bn = OSSymbol::withCStringNoCopy("IOAccelDisplayPipe");
    const OSMetaClass *base = bn ? OSMetaClass::getMetaClassWithName(bn) : nullptr;
    OSSafeReleaseNULL(bn);
    NVALOG("display pipe init fb %p index %u, IOAccelDisplayPipe size 0x%x (room 0x1000)", fb, idx,
           base ? base->getClassSize() : 0);
    if (base && base->getClassSize() > sizeof(IOAccelDisplayPipe)) {
        NVALOG("IOAccelDisplayPipe is larger than our layout: no display pipe");
        return false;
    }
    if (!IOAccelDisplayPipe::init(a, m, fb, idx)) { NVALOG("IOAccelDisplayPipe::init failed"); return false; }
    fb_ = fb;
    if (fb_) fb_->retain();
    foreign_ = !(fb_ && fb_->metaCast("NVDisplay"));
    accel_ = a;
    return true;
}

void *NVAccelDisplayPipe::initFramebufferResource(unsigned int idx, IOAccelResource2 *res) {
    // 0.2.9: system memory, as Paravirt's framebuffer is (the aperture as an
    // IOAccelSysMemory never prepared: "failed to wire down system map");
    // submitTransaction copies whatever is flipped into the aperture
    OSSafeReleaseNULL(fbMem_);
    OSSafeReleaseNULL(fbScan_);
    IODeviceMemory *ap = fb_ ? fb_->getApertureRange(kIOFBSystemAperture) : nullptr;
    const IOByteCount len = ap ? ap->getLength() : 0;
    if (!len) { OSSafeReleaseNULL(ap); return nullptr; }
    // 0.5.33: the family hands this resource to whoever composites the display
    // without a GPU (SkyLight's Software compositor writes the frame straight
    // into the "framebuffer resource"). Wrapping a private buffer therefore
    // threw the software-composited desktop away: the screen stayed black with
    // a strip of stale VRAM under it, while the display tick kept copying a
    // black aperture. When the aperture IS WindowServer's system-memory
    // framebuffer (the native615 case), wrap the aperture itself, so a
    // software-composited frame lands in the memory the tick presents.
    UInt64 fbPhys = 0;
    if (IOService *g = gspService()) {
        OSObject *o = g->copyProperty("NVGspControl-display615-fb-phys");
        if (OSNumber *n = OSDynamicCast(OSNumber, o)) fbPhys = n->unsigned64BitValue();
        OSSafeReleaseNULL(o);
    }
    const bool liveAperture = fbPhys && ap->getPhysicalAddress() == fbPhys;
    IOMemoryDescriptor *md = nullptr;
    if (liveAperture) {
        ap->retain();
        fbScan_ = ap;                      // the live scan-out aperture
        md = fbScan_;
    } else {
        fbMem_ = IOBufferMemoryDescriptor::withOptions(kIODirectionInOut | kIOMemoryKernelUserShared, len, PAGE_SIZE);
        if (fbMem_) bzero(fbMem_->getBytesNoCopy(), len);
        md = fbMem_;
    }
    OSSafeReleaseNULL(ap);
    void *mem = md && accel_ ? ioaf2SysMemWithMD(accel_, res, md) : nullptr;
    NVALOG("display pipe: framebuffer resource %u -> memory %s (%llu bytes, %s)", idx, mem ? "ok" : "FAILED",
           static_cast<unsigned long long>(len), liveAperture ? "live aperture" : "private buffer");
    return mem;
}

void *NVAccelDisplayPipe::destroyFramebufferResource(unsigned int, IOAccelResource2 *) {
    OSSafeReleaseNULL(fbMem_);
    OSSafeReleaseNULL(fbScan_);
    return nullptr;
}

IOMemoryMap *NVAccelDisplayPipe::surfaceMap(IOMemoryDescriptor *md) {
    SurfMap *slot = &surf_[0];
    for (auto &e : surf_) {
        if (e.md == md) { e.used = ++surfTick_; return e.map; }
        if (e.used < slot->used) slot = &e;
    }
    if (slot->md) {                                  // least recently flipped goes
        OSSafeReleaseNULL(slot->map);
        slot->md->complete();
        OSSafeReleaseNULL(slot->md);
    }
    if (md->prepare(kIODirectionOut) != kIOReturnSuccess) return nullptr;
    // 0.5.29: cached. CoreDisplay's swap-chain surfaces are write-combined, and
    // the default (the surface's own) cache mode made the CPU read of a 4K
    // frame take 3.3 s (flip-latency-avg 3.37e9 ns). Only the GPU writes these
    // pages (snooped sysmem), so a write-back view stays coherent.
    IOMemoryMap *m = md->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapReadOnly | kIOMapCopybackCache);
    if (!m) { md->complete(); return nullptr; }
    md->retain();
    *slot = SurfMap{md, m, ++surfTick_};
    return m;
}

void NVAccelDisplayPipe::dropSurfaces() {
    for (auto &e : surf_) {
        if (!e.md) continue;
        OSSafeReleaseNULL(e.map);
        e.md->complete();
        OSSafeReleaseNULL(e.md);
        e.used = 0;
    }
}

IOService *NVAccelDisplayPipe::gspService() {
    if (!gsp_)
        if (OSDictionary *m = IOService::serviceMatching("NVGspControl")) {
            gsp_ = IOService::copyMatchingService(m);
            m->release();
        }
    return gsp_;
}

void NVAccelDisplayPipe::onVblank(void *ref, UInt32 count, UInt64) {
    NVAccelDisplayPipe *p = static_cast<NVAccelDisplayPipe *>(ref);
    if (!p || !p->vblLock_) return;
    IOLockLock(p->vblLock_);
    p->vblSeen_ = count;
    IOLockWakeup(p->vblLock_, (event_t)&p->vblSeen_, false);
    IOLockUnlock(p->vblLock_);
    if (p->pendingTxn_ && p->flipCall_) thread_call_enter(p->flipCall_);   // 0.5.7
}

// 0.5.19: frame pacing. Uptime nanoseconds for the published rings.
void NVAccelDisplayPipe::statsReset() {
    stSubmits_ = stCopies_ = stNoCopy_ = 0;
    stLastSubmit_ = stLastCopy_ = stSubmitAt_ = 0;
    stSumSubmitGap_ = stMaxSubmitGap_ = stSubmitGaps_ = 0;
    stSumCopyGap_ = stMaxCopyGap_ = stCopyGaps_ = 0;
    stLate20_ = stLate33_ = 0;
    stSumLat_ = stMaxLat_ = stLatCount_ = 0;
    stRingHead_ = 0;
    bzero(stIntervalRing_, sizeof(stIntervalRing_));
    bzero(stLatencyRing_, sizeof(stLatencyRing_));
    statsPublish();
}

void NVAccelDisplayPipe::statsSubmit() {
    const UInt64 now = mach_absolute_time();
    if (stLastSubmit_) {
        UInt64 ns = 0;
        absolutetime_to_nanoseconds(now - stLastSubmit_, &ns);
        stSumSubmitGap_ += ns;
        if (ns > stMaxSubmitGap_) stMaxSubmitGap_ = ns;
        ++stSubmitGaps_;
    }
    stLastSubmit_ = now;
    stSubmitAt_ = now;
    ++stSubmits_;
}

void NVAccelDisplayPipe::statsCopy() {
    const UInt64 now = mach_absolute_time();
    UInt64 lat = 0, gap = 0;
    if (stSubmitAt_ && now > stSubmitAt_) absolutetime_to_nanoseconds(now - stSubmitAt_, &lat);
    if (stLastCopy_ && now > stLastCopy_) absolutetime_to_nanoseconds(now - stLastCopy_, &gap);
    if (lat) {
        stSumLat_ += lat; ++stLatCount_;
        if (lat > stMaxLat_) stMaxLat_ = lat;
    }
    if (gap) {
        stSumCopyGap_ += gap; ++stCopyGaps_;
        if (gap > stMaxCopyGap_) stMaxCopyGap_ = gap;
        if (gap > 20000000ULL) ++stLate20_;
        if (gap > 33000000ULL) ++stLate33_;
    }
    stLastCopy_ = now;
    stSubmitAt_ = 0;
    ++stCopies_;
    const UInt32 i = stRingHead_++ & 255;
    stIntervalRing_[i] = static_cast<UInt32>(gap > 0xffffffffULL ? 0xffffffffULL : gap);
    stLatencyRing_[i] = static_cast<UInt32>(lat > 0xffffffffULL ? 0xffffffffULL : lat);
    statsPublish();
}

void NVAccelDisplayPipe::statsPublish() {
    if (!accel_) return;
    if (foreign_) {                                   // 0.5.27: the NVDisplay pipe owns the flip statistics
        accel_->setProperty("NVAccelerator-flip-copies-foreign", stCopies_, 64);
        return;
    }
    accel_->setProperty("NVAccelerator-flip-submits", stSubmits_, 64);
    accel_->setProperty("NVAccelerator-flip-copies", stCopies_, 64);
    accel_->setProperty("NVAccelerator-flip-skipped", skippedFlips_, 64);
    accel_->setProperty("NVAccelerator-flip-render-waits", renderWaits_, 64);
    accel_->setProperty("NVAccelerator-flip-async", asyncFlips_, 64);
    accel_->setProperty("NVAccelerator-flip-ce", ceFlips_, 64);
    accel_->setProperty("NVAccelerator-flip-ce-fails", ceFails_, 64);
    accel_->setProperty("NVAccelerator-flip-ce-retry-at", ceRetryAt_, 64);
    accel_->setProperty("NVAccelerator-flip-ce-last-err", static_cast<UInt32>(ceLastErr_), 32);
    accel_->setProperty("NVAccelerator-flip-apbus", apBus_, 64);
    accel_->setProperty("NVAccelerator-flip-fb-rowbytes", fbRowBytes_, 32);
    accel_->setProperty("NVAccelerator-flip-fb-widthbytes", fbWidthBytes_, 32);
    accel_->setProperty("NVAccelerator-flip-fb-height", fbHeight_, 32);
    accel_->setProperty("NVAccelerator-flip-empty", empty_, 64);
    accel_->setProperty("NVAccelerator-gamma-sets", gammaSets_, 64);          // 0.5.37
    accel_->setProperty("NVAccelerator-gamma-fails", gammaFails_, 64);
    accel_->setProperty("NVAccelerator-gamma-last", static_cast<UInt32>(gammaLast_), 32);
    accel_->setProperty("NVAccelerator-color-other", colorOther_, 64);
    accel_->setProperty("NVAccelerator-flip-submit-avg-ns",
                        stSubmitGaps_ ? stSumSubmitGap_ / stSubmitGaps_ : 0, 64);
    accel_->setProperty("NVAccelerator-flip-submit-max-ns", stMaxSubmitGap_, 64);
    accel_->setProperty("NVAccelerator-flip-interval-avg-ns",
                        stCopyGaps_ ? stSumCopyGap_ / stCopyGaps_ : 0, 64);
    accel_->setProperty("NVAccelerator-flip-interval-max-ns", stMaxCopyGap_, 64);
    accel_->setProperty("NVAccelerator-flip-late20", stLate20_, 64);
    accel_->setProperty("NVAccelerator-flip-late33", stLate33_, 64);
    accel_->setProperty("NVAccelerator-flip-latency-avg-ns",
                        stLatCount_ ? stSumLat_ / stLatCount_ : 0, 64);
    accel_->setProperty("NVAccelerator-flip-latency-max-ns", stMaxLat_, 64);
    accel_->setProperty("NVAccelerator-flip-vbl-waits", vblWaits_, 64);
    accel_->setProperty("NVAccelerator-flip-vbl-timeouts", vblTimeouts_, 64);
    OSData *d = OSData::withBytes(stIntervalRing_, sizeof(stIntervalRing_));
    if (d) { accel_->setProperty("NVAccelerator-flip-intervals", d); d->release(); }
    OSData *l = OSData::withBytes(stLatencyRing_, sizeof(stLatencyRing_));
    if (l) { accel_->setProperty("NVAccelerator-flip-latencies", l); l->release(); }
}

// 0.5.7: the queued flip, right after a vblank (thread call, not the
// interrupt workloop: the copy takes ~1.5 ms for a 4K frame)
void NVAccelDisplayPipe::flipCallout(thread_call_param_t param, thread_call_param_t) {
    NVAccelDisplayPipe *p = static_cast<NVAccelDisplayPipe *>(param);
    if (!p || !p->flipLock_) return;
    IOLockLock(p->flipLock_);
    // 0.5.10: the family queues a transaction (and calls submitTransaction)
    // before the GPU has rendered its surface; the hardware is meant to hold
    // the flip until the transaction's IOAccelEvent (+0x60, 8 x {stamp
    // index, value}) has passed. Copying at once showed half-drawn frames
    // (a black triangle-edged block right after display wake, 1 Oct 04:20).
    if (p->pending_.t && (!p->ringsPassed(p->pending_) || !p->renderDone(p->pending_.t))) {
        IOLockUnlock(p->flipLock_);
        UInt64 deadline = 0;                          // look again soon (and at the next vblank)
        clock_interval_to_deadline(2, kMillisecondScale, &deadline);
        thread_call_enter_delayed(p->flipBackstop_, deadline);
        ++p->renderWaits_;
        return;
    }
    PendingFlip f = p->pending_;
    p->pending_ = PendingFlip{};
    p->copyingTxn_ = f.t;
    p->pendingTxn_ = nullptr;
    p->pendingSince_ = 0;
    if (f.md) {
        p->copySurface(f.md, f.bpr, f.size, f.fmt);
        p->statsCopy();                               // 0.5.19
        if (p->lastFlip_.md != f.md) {                // 0.5.13
            f.md->retain();
            if (p->lastFlip_.md) p->lastFlip_.md->release();
        }
        p->lastFlip_ = f;
        p->lastFlip_.t = nullptr;
    }
    p->copyingTxn_ = nullptr;
    IOLockUnlock(p->flipLock_);
    if (f.md) f.md->release();
    if (!f.md) return;                                // a backstop after the vblank copy: nothing to do
    ++p->asyncFlips_;
    ioaf2SignalTxnInterrupt(p, nullptr);              // 0.5.9: let the family retire it now
}

// 0.5.11: ring positions from NVGspControl (see "nvgsp-ring-progress")
bool NVAccelDisplayPipe::ringProgress(UInt32 p[4]) {
    IOService *g = gspService();
    return g && g->callPlatformFunction("nvgsp-ring-progress", false, p, nullptr, nullptr, nullptr) == kIOReturnSuccess;
}

// everything queued on the GR and CE rings when the flip was queued is done
bool NVAccelDisplayPipe::ringsPassed(const PendingFlip &f) {
    if (!f.ringWait) return true;
    UInt32 p[4] = {};
    if (!ringProgress(p)) return true;                // no GPU state (reset): do not hold the screen
    return static_cast<SInt32>(p[1] - f.grTarget) >= 0 && static_cast<SInt32>(p[3] - f.ceTarget) >= 0;
}

// 0.5.10: has the GPU finished the surface of `t`? (IOAccelEventMachineFast2::
// testEventUnlocked on the transaction's event, lock-free stamp reads)
bool NVAccelDisplayPipe::renderDone(IOAccelDisplayPipeTransaction2 *t) {
    IOAccelEventMachine2 *em = accel_ ? *reinterpret_cast<IOAccelEventMachine2 **>(reinterpret_cast<UInt8 *>(accel_) + 0x380)
                                      : nullptr;
    if (!em || !t) return true;
    IOAccelEvent *ev = reinterpret_cast<IOAccelEvent *>(reinterpret_cast<UInt8 *>(t) + 0x60);
    const bool done = static_cast<bool>(reinterpret_cast<uintptr_t>(em->testEventUnlocked(ev)) & 1);
    if (eventLogs_ < 6) {                             // what the event holds, for the layout check
        const UInt32 *w = reinterpret_cast<const UInt32 *>(ev);
        ++eventLogs_;
        NVALOG("display pipe: txn event %d:%u %d:%u %d:%u -> %s", (int)w[0], w[1], (int)w[2], w[3], (int)w[4], w[5],
               done ? "done" : "pending");
    }
    return done;
}

// Sleep until the next vblank (at most 20 ms: a missing interrupt must not
// stall WindowServer).
// 0.5.13: NVGspControl calls this (no lock held) when a GPU reset is over
void NVAccelDisplayPipe::onResetDone(void *ref) {
    NVAccelDisplayPipe *p = static_cast<NVAccelDisplayPipe *>(ref);
    NVALOG("display pipe: GPU reset over, last frame %s", p && p->lastFlip_.md ? "kept" : "none");
    if (p && p->restoreCall_) thread_call_enter(p->restoreCall_);
}

void NVAccelDisplayPipe::restoreCallout(thread_call_param_t param, thread_call_param_t) {
    NVAccelDisplayPipe *p = static_cast<NVAccelDisplayPipe *>(param);
    if (!p || !p->flipLock_) return;
    IOLockLock(p->flipLock_);
    if (p->lastFlip_.md && p->mapFramebuffer()) {
        p->copySurface(p->lastFlip_.md, p->lastFlip_.bpr, p->lastFlip_.size, p->lastFlip_.fmt);
        ++p->restores_;
    }
    IOLockUnlock(p->flipLock_);
    NVALOG("display pipe: last frame put back after a GPU reset (%u)", p->restores_);
}

bool NVAccelDisplayPipe::setupVsync() {
    if (vsync_ < 0) {
        vsync_ = 1;
        if (IORegistryEntry *o = IORegistryEntry::fromPath("/options", gIODTPlane)) {
            if (OSData *d = OSDynamicCast(OSData, o->getProperty("nvaccel-vsync")))
                vsync_ = !(d->getLength() >= 1 && static_cast<const char *>(d->getBytesNoCopy())[0] == '0');
            o->release();
        }
        if (vsync_) vblLock_ = IOLockAlloc();
        IOService *g = vsync_ && vblLock_ ? gspService() : nullptr;
        const IOReturn r = g ? g->callPlatformFunction("nvgsp-vblank-register", false,
                                                       reinterpret_cast<void *>(&NVAccelDisplayPipe::onVblank),
                                                       this, nullptr, nullptr)
                             : kIOReturnUnsupported;
        if (r != kIOReturnSuccess) vsync_ = 0;
        NVALOG("display pipe: vsync %s (0x%x)", vsync_ ? "on" : "off", r);
        if (!restoreCall_) restoreCall_ = thread_call_allocate(&NVAccelDisplayPipe::restoreCallout, this);
        if (g && restoreCall_) {                        // 0.5.13
            const IOReturn rr = g->callPlatformFunction("nvgsp-reset-register", false,
                                                        reinterpret_cast<void *>(&NVAccelDisplayPipe::onResetDone),
                                                        this, nullptr, nullptr);
            NVALOG("display pipe: reset callback 0x%x", rr);
        }
    }
    return vsync_ == 1;
}

void NVAccelDisplayPipe::waitVblank() {
    if (!setupVsync()) return;
    IOLockLock(vblLock_);
    const UInt32 start = vblSeen_;
    UInt64 deadline = 0;
    clock_interval_to_deadline(20, kMillisecondScale, &deadline);
    while (vblSeen_ == start) {
        if (IOLockSleepDeadline(vblLock_, (event_t)&vblSeen_, deadline, THREAD_UNINT) == THREAD_TIMED_OUT) {
            ++vblTimeouts_;
            break;
        }
    }
    ++vblWaits_;
    IOLockUnlock(vblLock_);
    // a vblank source that never fires: stop waiting for it
    if (vblTimeouts_ > 64 && vblTimeouts_ * 2 > vblWaits_) {
        vsync_ = 0;
        NVALOG("display pipe: no vblanks (%llu timeouts), vsync off", vblTimeouts_);
    }
}

bool NVAccelDisplayPipe::flipOnCopyEngine(IOMemoryDescriptor *md, UInt64 bpr, UInt32 rows, UInt32 n) {
    if (foreign_) return false;
    if (ceFlip_ < 0) {
        ceFlip_ = 1;
        if (IORegistryEntry *o = IORegistryEntry::fromPath("/options", gIODTPlane)) {
            if (OSData *d = OSDynamicCast(OSData, o->getProperty("nvaccel-ceflip")))
                ceFlip_ = !(d->getLength() >= 1 && static_cast<const char *>(d->getBytesNoCopy())[0] == '0');
            o->release();
        }
        gspService();
    }
    if (ceFlip_ != 1 || !gsp_ || !apBus_ || !md) return false;
    // 0.5.31: the copy engine path needs the kernel to accept the VRAM
    // scan-out destination AND to submit a multi-line batch on the native615
    // maintenance channel (NVGspControl 0.180.171+). Until it publishes that,
    // the CPU copy into the (now cached) sysmem aperture is the path, and
    // displayCopyTick615 puts it on the screen.
    {
        bool ready = false;
        OSObject *o = gsp_->copyProperty("NVGspControl-flipcopy-native615");
        if (OSNumber *n = OSDynamicCast(OSNumber, o)) ready = n->unsigned32BitValue() != 0;
        OSSafeReleaseNULL(o);
        if (!ready) return false;
    }
    // 0.5.30: read the VRAM scan-out surface once
    if (!scanoutBus_) {
        OSObject *o = gsp_->copyProperty("NVGspControl-display615-scanout-phys");
        if (OSNumber *num = OSDynamicCast(OSNumber, o)) scanoutBus_ = num->unsigned64BitValue();
        OSSafeReleaseNULL(o);
        if (scanoutBus_) NVALOG("display pipe: flips go to the VRAM scan-out 0x%llx", scanoutBus_);
    }
    // 0.5.20: do not disable the copy engine for the whole session because
    // the first frames at boot failed (GSP rings still warming up, aperture
    // not mapped yet). Retry every 256 presented frames; a later success
    // clears the retry state.
    if (ceRetryAt_ && copies_ < ceRetryAt_) return false;
    NVGspFlipCopy f{};
    f.version = 1; f.rows = rows; f.src = md; f.srcOffset = 0;
    f.srcRowBytes = static_cast<UInt32>(bpr); f.widthBytes = n;
    f.dstBus = scanoutBus_ ? scanoutBus_ : apBus_; f.dstRowBytes = fbRowBytes_;
    const IOReturn r = gsp_->callPlatformFunction("nvgsp-flip-copy", false, &f, nullptr, nullptr, nullptr);
    ceLastErr_ = r;
    if (r == kIOReturnSuccess) { ++ceFlips_; ceRetryAt_ = 0; ceFails_ = 0; return true; }
    // a few failures in a row: stay on the CPU copy
    if (++ceFails_ <= 8) NVALOG("display pipe: copy-engine flip failed 0x%x, CPU copy", r);
    if (ceFails_ > 32 && ceFlips_ == 0) { ceRetryAt_ = copies_ + 256; ceFails_ = 0; }
    return false;
}

void NVAccelDisplayPipe::free() {
    if (restoreCall_ && gsp_)                         // 0.5.13
        gsp_->callPlatformFunction("nvgsp-reset-register", false, nullptr, this, nullptr, nullptr);
    if (restoreCall_) { thread_call_cancel(restoreCall_); thread_call_free(restoreCall_); restoreCall_ = nullptr; }
    if (lastFlip_.md) OSSafeReleaseNULL(lastFlip_.md);
    thread_call_t *calls[2] = {&flipCall_, &flipBackstop_};   // 0.5.7
    for (thread_call_t *c : calls)
        if (*c) { thread_call_cancel(*c); thread_call_free(*c); *c = nullptr; }
    if (pending_.md) OSSafeReleaseNULL(pending_.md);
    pendingTxn_ = copyingTxn_ = nullptr;
    if (flipLock_) { IOLockFree(flipLock_); flipLock_ = nullptr; }
    if (vsync_ == 1 && gsp_)
        gsp_->callPlatformFunction("nvgsp-vblank-unregister", false,
                                   reinterpret_cast<void *>(&NVAccelDisplayPipe::onVblank), this, nullptr, nullptr);
    if (vblLock_) { IOLockFree(vblLock_); vblLock_ = nullptr; }
    OSSafeReleaseNULL(gsp_);
    dropSurfaces();
    OSSafeReleaseNULL(fbMem_);
    OSSafeReleaseNULL(fbScan_);
    OSSafeReleaseNULL(map_);
    if (gamma_) { IOFree(gamma_, 3 * kGamma * sizeof(UInt16)); gamma_ = nullptr; }   // 0.5.37
    OSSafeReleaseNULL(fb_);
    IOAccelDisplayPipe::free();
}

// scan-out memory of the current mode, mapped write-combined
bool NVAccelDisplayPipe::mapFramebuffer() {
    if (!fb_) return false;
    IODisplayModeID mode = 0;
    IOIndex depth = 0;
    IOPixelInformation info{};
    if (fb_->getCurrentDisplayMode(&mode, &depth) != kIOReturnSuccess ||
        fb_->getPixelInformation(mode, depth, kIOFBSystemAperture, &info) != kIOReturnSuccess)
        return false;
    const UInt32 rows = info.activeHeight, rb = info.bytesPerRow;
    if (!rows || rb < 4 || info.activeWidth == 0 ||
        UInt64(rb) < UInt64(info.activeWidth) * (info.bitsPerPixel / 8) ||
        info.bitsPerPixel < 8)
        return false;
    if (map_ && rows == fbHeight_ && rb == fbRowBytes_) return true;
    OSSafeReleaseNULL(map_);
    IODeviceMemory *mem = fb_->getApertureRange(kIOFBSystemAperture);
    if (!mem) return false;
    // 0.5.31: the native615 aperture is WindowServer's *system-memory*
    // framebuffer (NVGspControl publishes its address as display615-fb-phys),
    // not VRAM behind BAR1. Mapping that range write-combined made every store
    // a separate bus transaction: 33 MB took 3.3 s (flip-latency-avg 3.37e9 ns,
    // "copy 3298060 us avg"). Cached stores take ~1 ms (userland: 28-34 GB/s
    // into the same 4K IOSurface). Only this CPU writes the aperture and the
    // display tick reads it with the copy engine as COHERENT_SYSMEM, so a
    // write-back view stays coherent; anything else keeps write-combining.
    UInt64 fbPhys = 0;
    if (IOService *g = gspService()) {
        OSObject *o = g->copyProperty("NVGspControl-display615-fb-phys");
        if (OSNumber *n = OSDynamicCast(OSNumber, o)) fbPhys = n->unsigned64BitValue();
        OSSafeReleaseNULL(o);
    }
    const bool sysmemAperture = fbPhys && mem->getPhysicalAddress() == fbPhys;
    // kIOMapDefaultCache is 0 and means "no cache attribute" for a physical
    // range, i.e. uncached stores; cacheable system memory needs
    // kIOMapCopybackCache explicitly. If the platform refuses the cacheable
    // map, stay on write-combined rather than losing the display.
    if (sysmemAperture) map_ = mem->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapCopybackCache);
    if (!map_) map_ = mem->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapWriteCombineCache);
    apBus_ = mem->getPhysicalAddress();
    mem->release();
    if (!map_) return false;
    NVALOG("display pipe: scan-out aperture 0x%llx %s", apBus_, sysmemAperture ? "sysmem (cacheable map)" : "device (WC map)");
    fbRowBytes_ = rb; fbHeight_ = rows;
    fbWidthBytes_ = static_cast<UInt32>(UInt64(info.activeWidth) * (info.bitsPerPixel / 8));
    NVALOG("display pipe: scan-out %ux%u, %u bytes/row, %u bpp, %llu bytes mapped", info.activeWidth, rows, rb,
           info.bitsPerPixel, map_->getLength());
    return true;
}

void NVAccelDisplayPipe::applyPostGamma(IOAccelDisplayPipeTransaction2 *t) {
    const uint8_t *tb = reinterpret_cast<const uint8_t *>(t);
    const UInt64 dirty = *reinterpret_cast<const UInt64 *>(tb + kTxnDirty);
    if (dirty & kDirtyColorOther) ++colorOther_;      // not applied, counted (NVAccelerator-color-other)
    if (!(dirty & kDirtyPostGamma)) return;
    const uint8_t *st = *reinterpret_cast<const uint8_t *const *>(tb + kTxnColor);
    const uint8_t *tbl = st ? *reinterpret_cast<const uint8_t *const *>(st + 8) : nullptr;
    const UInt32 *d = tbl ? *reinterpret_cast<const UInt32 *const *>(tbl + 0x10) : nullptr;
    const UInt64 size = tbl ? *reinterpret_cast<const UInt64 *>(tbl + 0x18) : 0;
    const UInt32 n = (d && size >= 0x10) ? d[0] : 0;
    if (n < 2 || n > 0x20000 || size < 0x10 + 16ULL * n) { ++gammaFails_; return; }
    const UInt32 *e = d + 4;
    UInt64 h = 0xcbf29ce484222325ULL ^ n;             // FNV-1a: the same table is re-sent on many commits
    for (UInt32 i = 0; i < n * 4; ++i) h = (h ^ e[i]) * 0x100000001b3ULL;
    if (h == gammaHash_ && gammaLast_ == kIOReturnSuccess) return;
    if (!gamma_ && !(gamma_ = static_cast<UInt16 *>(IOMalloc(3 * kGamma * sizeof(UInt16))))) { ++gammaFails_; return; }
    for (UInt32 i = 0; i < kGamma; ++i) {
        const UInt32 j = (i * (n - 1) + (kGamma - 1) / 2) / (kGamma - 1);
        for (UInt32 c = 0; c < 3; ++c) gamma_[c * kGamma + i] = unorm16(e[j * 4 + c]);
    }
    IOService *g = gspService();
    gammaLast_ = g ? g->callPlatformFunction("nvgsp-gamma-set", false, gamma_,
                                             reinterpret_cast<void *>(static_cast<uintptr_t>(kGamma)), nullptr, nullptr)
                   : kIOReturnNotReady;
    gammaHash_ = h;
    if (gammaLast_ == kIOReturnSuccess) ++gammaSets_; else ++gammaFails_;
    NVALOG("display pipe: post-gamma %u entries -> 0x%x (r/g/b mid %04x %04x %04x)", n, gammaLast_,
           gamma_[kGamma / 2], gamma_[kGamma + kGamma / 2], gamma_[2 * kGamma + kGamma / 2]);
}

IOReturn NVAccelDisplayPipe::submitTransaction(IOAccelDisplayPipeTransaction2 *t) {
    if (t && !foreign_) applyPostGamma(t);            // 0.5.37
    void *surf = t ? ioaf2TxnPlaneSurface(t, 0, 0) : nullptr;
    if (tr(7)) NVALOG("display pipe: submitTransaction %p surface %p", t, surf);
    if (!mapFramebuffer()) { if (tr(7)) NVALOG("display pipe: scan-out map failed"); return kIOReturnIOError; }
    IOMemoryDescriptor *md = nullptr;
    UInt64 bpr = 0, size = 0;
    UInt32 fmt = 0;
    if (surf) {
        md = iosurfMemory(surf, this);
        bpr = iosurfBytesPerRow(surf); size = iosurfAllocSize(surf); fmt = iosurfPixelFormat(surf);
    } else if (fbScan_) {                 // 0.5.33: the frame already is the scan-out aperture
        ++empty_;
        return kIOReturnSuccess;
    } else if (fbMem_) {                 // a flip of the framebuffer resource itself
        ++empty_;
        md = fbMem_; bpr = fbRowBytes_; size = fbMem_->getLength(); fmt = 'BGRA';
    }
    if (!md || !bpr) return kIOReturnBadArgument;
    if (asyncFlip_ < 0) {                            // 0.5.7
        asyncFlip_ = 1;
        if (IORegistryEntry *o = IORegistryEntry::fromPath("/options", gIODTPlane)) {
            if (OSData *d = OSDynamicCast(OSData, o->getProperty("nvaccel-asyncflip")))
                asyncFlip_ = !(d->getLength() >= 1 && static_cast<const char *>(d->getBytesNoCopy())[0] == '0');
            o->release();
        }
        if (asyncFlip_) {
            flipLock_ = IOLockAlloc();
            flipCall_ = flipLock_ ? thread_call_allocate(&NVAccelDisplayPipe::flipCallout, this) : nullptr;
            flipBackstop_ = flipCall_ ? thread_call_allocate(&NVAccelDisplayPipe::flipCallout, this) : nullptr;
            if (!flipBackstop_) asyncFlip_ = 0;
        }
        NVALOG("display pipe: asynchronous flips %s", asyncFlip_ ? "on" : "off");
    }
    // without vblanks there is nothing to start the copy: do it here
    if (asyncFlip_ != 1 || !setupVsync()) {
        waitVblank();
        statsSubmit();                                 // 0.5.19
        if (flipLock_) IOLockLock(flipLock_);
        copySurface(md, bpr, size, fmt);
        if (flipLock_) IOLockUnlock(flipLock_);
        statsCopy();                                   // 0.5.19
        return kIOReturnSuccess;
    }
    md->retain();
    UInt32 prog[4] = {};                              // 0.5.11: the GPU work queued so far
    const bool rw = ringProgress(prog);
    IOLockLock(flipLock_);
    IOMemoryDescriptor *dropped = pending_.md;
    if (dropped) ++skippedFlips_;                     // the queued frame never reached the screen
    pending_ = PendingFlip{t, md, bpr, size, fmt, rw, prog[0], prog[2]};
    if (!dropped) pendingSince_ = mach_absolute_time();
    pendingTxn_ = t;                                  // the replaced one now reads complete
    IOLockUnlock(flipLock_);
    if (dropped) dropped->release();
    statsSubmit();                                    // 0.5.19
    // 0.5.35: the backstop was a flat 25 ms, and the measured flip latency was
    // 28 ms - i.e. every frame was presented by the backstop, not by a vblank
    // (the display interrupt is synthetic on this runtime). Copy 4 ms after the
    // submit instead; renderDone() still holds the frame back while the GPU is
    // drawing it, so this only removes dead time.
    UInt64 deadline = 0;                              // 0.5.8: copy even if no vblank comes
    clock_interval_to_deadline(4, kMillisecondScale, &deadline);
    thread_call_enter_delayed(flipBackstop_, deadline);
    return kIOReturnSuccess;
}

// Copy one surface into the scan-out memory. Caller holds flipLock_ (when
// there is one): surf_ and the copy statistics are shared with flipCallout.
void NVAccelDisplayPipe::copySurface(IOMemoryDescriptor *md, UInt64 bpr, UInt64 size, UInt32 fmt) {
    if (!bpr || !fbRowBytes_ || !map_) return;
    UInt32 rows = fbHeight_;
    if (bpr * rows > size) rows = (UInt32)(size / bpr);
    const UInt32 n = (UInt32)(bpr < fbWidthBytes_ ? bpr : fbWidthBytes_);
    uint8_t *dst = map_ ? reinterpret_cast<uint8_t *>(map_->getVirtualAddress()) : nullptr;
    if (!dst) return;
    if (fbRowBytes_ * (UInt64)rows > map_->getLength()) rows = (UInt32)(map_->getLength() / fbRowBytes_);
    // the framebuffer resource is ours and wired; a client surface is wired
    // and mapped here once (surfaceMap), then copied row by row
    IOMemoryMap *sm = md == fbMem_ ? nullptr : surfaceMap(md);
    const uint8_t *src = sm ? reinterpret_cast<const uint8_t *>(sm->getVirtualAddress())
                            : md == fbMem_ ? static_cast<const uint8_t *>(fbMem_->getBytesNoCopy()) : nullptr;
    if (!src) { if (tr(7)) NVALOG("display pipe: surface could not be mapped"); return; }
    const UInt64 have = sm ? sm->getLength() : size;
    if (bpr * (UInt64)rows > have) rows = (UInt32)(have / bpr);
    const UInt64 t0 = mach_absolute_time();
    if (!flipOnCopyEngine(md, bpr, rows, n))
        for (UInt32 y = 0; y < rows; ++y) memcpy(dst + (UInt64)y * fbRowBytes_, src + (UInt64)y * bpr, n);
    copyTicks_ += mach_absolute_time() - t0;
    if (fmt != lastFormat_ || !(copies_ & 1023)) {
        lastFormat_ = fmt;
        UInt64 ns = 0;
        absolutetime_to_nanoseconds(copyTicks_ / (copies_ + 1), &ns);
        NVALOG("display pipe: flip %llu, surface fmt %c%c%c%c bpr %llu, %u rows, copy %llu us avg, %llu on the copy engine, vsync %d (%llu waits, %llu timeouts), async %d (%llu, %llu skipped, %llu render waits)",
               copies_, (char)(fmt >> 24), (char)(fmt >> 16), (char)(fmt >> 8), (char)fmt, bpr, rows, ns / 1000, ceFlips_,
               vsync_, vblWaits_, vblTimeouts_, asyncFlip_, asyncFlips_, skippedFlips_, renderWaits_);
    }
    ++copies_;
}

// ---------------------------------------------------------------- memory
// IOAccelSysMemory::MetaClass::alloc in the family returns NULL (0x55686:
// xor %eax,%eax; ret) — the class is abstract-by-alloc, so newSysMemory must
// return our own subclass, exactly like AppleParavirtSysMemory (which only
// overrides dtor/getMetaClass). Verified: without this, withOptions logs
// "allocIOAccelResource failed to alloc texture object" (live 0.1.4).
class NVAccelSysMemory : public IOAccelSysMemory {
    OSDeclareDefaultStructors(NVAccelSysMemory)
};
OSDefineMetaClassAndStructors(NVAccelSysMemory, IOAccelSysMemory)

class NVAccelVidMemory : public IOAccelVidMemory {
    OSDeclareDefaultStructors(NVAccelVidMemory)
public:
    void *getPhysicalSegment(unsigned long long, unsigned long long *len) override {
        if (len) *len = 0;
        return nullptr;
    }
    void *allocPhysical() override { NVALOG("VidMemory::allocPhysical (stub)"); return nullptr; }
    void *deallocPhysical() override { return nullptr; }
};
OSDefineMetaClassAndStructors(NVAccelVidMemory, IOAccelVidMemory)

class NVAccelMemoryMap : public IOAccelMemoryMap {
    OSDeclareDefaultStructors(NVAccelMemoryMap)
public:
    // 0.2.10: IOAccel resources are not in our GPU page tables (NVMTLDriver
    // maps memory through NVGspControl), so committing one is a no-op that
    // succeeds; returning false made every IOAccel prepare fail
    void *commitIntoGPUPageTable() override { return (void *)1; }
    void *releaseFromGPUPageTable() override { return (void *)1; }
    void *updateGPUPageTable() override { return (void *)1; }
    void *prepare() override {
        void *r = IOAccelMemoryMap::prepare();
        if (!((uintptr_t)r & 0xff)) NVALOG("MemoryMap::prepare failed (length %llu)", (unsigned long long)(uintptr_t)getLength());
        return r;
    }
    void *allocGPUVirtualAddress() override {
        void *r = IOAccelMemoryMap::allocGPUVirtualAddress();
        if (!((uintptr_t)r & 0xff)) NVALOG("MemoryMap::allocGPUVirtualAddress failed");
        return r;
    }
};
OSDefineMetaClassAndStructors(NVAccelMemoryMap, IOAccelMemoryMap)

class NVAccelResource : public IOAccelResource2 {
    OSDeclareDefaultStructors(NVAccelResource)
public:
    void *rebuildPagingBuffer() override { return nullptr; }
    void *getLevelOffset(unsigned char, unsigned char, int *o) override { if (o) *o = 0; return nullptr; }
    void *getBackingLevelOffset(unsigned char, unsigned char, int *o) override { if (o) *o = 0; return nullptr; }
    // 0.3.12: as AppleParavirtResource: the surface's allocation size and
    // row bytes. The family takes the first as the length of an IOSurface
    // resource (type 0xc0); 0 here gave WindowServer's display surfaces an
    // empty mapping that could not get a GPU address.
    void *calculateIOSurfaceDeviceCacheVRAMBytes(unsigned long long *a, unsigned long long *b) override {
        const UInt8 *dc = *reinterpret_cast<UInt8 *const *>(reinterpret_cast<const UInt8 *>(this) + 0xe0);
        const void *surf = dc ? *reinterpret_cast<void *const *>(dc + 0x10) : nullptr;
        if (b) *b = surf ? iosurfBytesPerRow(surf) : 0;
        if (a) *a = surf ? iosurfAllocSize(surf) : 0;
        return nullptr;
    }
    void *addToAperture() override { return nullptr; }
    void *removeFromAperture() override { return nullptr; }
    void *getApertureMemoryDescriptor(unsigned long long *o) override { if (o) *o = 0; return nullptr; }
};
OSDefineMetaClassAndStructors(NVAccelResource, IOAccelResource2)

// ---------------------------------------------------------------- GPU task
// AppleParavirtTask::init(accel, vaBytes): IORangeAllocator over [0, va),
// 4 KiB granules, page 0 reserved, then IOAccelTask::init(accel, 1, &ra).
class NVAccelTask : public IOAccelTask {
    OSDeclareDefaultStructors(NVAccelTask)
public:
    bool initWithAccel(IOGraphicsAccelerator2 *accel, UInt64 vaBytes) {
        IORangeAllocator *ra = IORangeAllocator::withRange(vaBytes - 1, 0x1000, 0x400, 0);
        if (!ra) return false;
        IORangeScalar zero = 0;
        ra->allocateRange(zero, 0x1000);   // no GPU VA 0
        const bool ok = IOAccelTask::init(accel, 1, &ra);
        OSSafeReleaseNULL(ra);
        return ok;
    }
};
OSDefineMetaClassAndStructors(NVAccelTask, IOAccelTask)

// ---------------------------------------------------------------- AGDC
// 0.3.0 (W1): IOPresentment (WindowServer's display path on Sonoma) builds
// its device map from AppleGraphicsDeviceControl services: it opens each,
// asks for the vendor information (attribute 1) and the GPU capabilities,
// and only then pairs the IOFramebuffer with a display pipe. Real GPU
// drivers subclass AGDC; so does AppleParavirtGPUControl, whose answers
// (KDK 14.8.9) this follows. Unknown attributes are logged once.
class NVGraphicsDeviceControl : public AppleGraphicsDeviceControl {
    OSDeclareDefaultStructors(NVGraphicsDeviceControl)
    UInt32 logged_ = 0, sub711_ = 0;
    // 0.5.27: the framebuffers handed to AGDC, NVDisplay first (nvCopyFramebuffers)
    IOFramebuffer *fbs_[2] = {};
    unsigned nfb_ = 0;
    void loadFramebuffers() { if (!nfb_) nfb_ = nvCopyFramebuffers(fbs_, 2); }
    IOService *provider_ = nullptr;
    UInt16 gpuVendor_ = 0x10de, gpuDevice_ = 0x2704;
    // 0.5.36: IOPresentment builds the display pipeline capabilities from the
    // 0x711 sub-queries (0x1 CSC, 0x2 gamma, 0x4 linearization, 0x8 pipeline
    // scaler, 0x10 plane scaler, 0x20 rotation, 0x80 queue, ...). A reply can
    // be replaced without a rebuild: bytes from offset 8 (entry count, then
    // the entries) come from an administrator's setProperties
    // ("NVAGDC-711-override" = {"<sub decimal>" = data}), else from the
    // NVGspControl property "NVGspControl-agdc-711" (same form).
    static constexpr unsigned kOverrides = 16;
    OSData *ovr_[kOverrides] = {};
    IOLock *ovrLock_ = nullptr;
    static int subBit(UInt32 sub) { return (sub && !(sub & (sub - 1)) && sub < (1u << kOverrides)) ? __builtin_ctz(sub) : -1; }
    bool applyOverride(UInt32 sub, uint8_t *o, unsigned long osz);
public:
    IOReturn setProperties(OSObject *properties) override;
    void free() override {
        for (auto &d : ovr_) OSSafeReleaseNULL(d);
        if (ovrLock_) { IOLockFree(ovrLock_); ovrLock_ = nullptr; }
        AppleGraphicsDeviceControl::free();
    }
    void setProvider(IOService *p) {
        provider_ = p;
        if (p) {
            OSNumber *v = OSDynamicCast(OSNumber, p->getProperty("NVGspControl-gpu-vendor"));
            OSNumber *d = OSDynamicCast(OSNumber, p->getProperty("NVGspControl-gpu-device"));
            if (v) gpuVendor_ = static_cast<UInt16>(v->unsigned32BitValue());
            if (d) gpuDevice_ = static_cast<UInt16>(d->unsigned32BitValue());
        }
    }
public:
    void *vendor_doDeviceAttribute(unsigned int attr, unsigned long *in, unsigned long inSize, unsigned long *out,
                                   unsigned long *outSize, IOExternalMethodArguments *args) override;
    // AGDP matches every AGDC and derefs Mac-only policy state in start (panic), keep it off us
    bool matchPropertyTable(OSDictionary *table, SInt32 *score) override {
        OSString *cls = OSDynamicCast(OSString, table ? table->getObject("IOClass") : nullptr);
        if (cls && cls->isEqualTo("AppleGraphicsDevicePolicy")) return false;
        return AppleGraphicsDeviceControl::matchPropertyTable(table, score);
    }
};
OSDefineMetaClassAndStructors(NVGraphicsDeviceControl, AppleGraphicsDeviceControl)

IOReturn NVGraphicsDeviceControl::setProperties(OSObject *properties) {
    OSDictionary *d = OSDynamicCast(OSDictionary, properties);
    OSDictionary *m = d ? OSDynamicCast(OSDictionary, d->getObject("NVAGDC-711-override")) : nullptr;
    if (!m) return AppleGraphicsDeviceControl::setProperties(properties);
    if (IOUserClient::clientHasPrivilege(current_task(), kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
        return kIOReturnNotPrivileged;
    if (!ovrLock_ && !(ovrLock_ = IOLockAlloc())) return kIOReturnNoMemory;
    OSData *next[kOverrides] = {};
    bool ok = true;
    if (OSCollectionIterator *it = OSCollectionIterator::withCollection(m)) {
        while (OSSymbol *k = OSDynamicCast(OSSymbol, it->getNextObject())) {
            OSData *v = OSDynamicCast(OSData, m->getObject(k));
            UInt32 sub = 0;
            for (const char *c = k->getCStringNoCopy(); *c >= '0' && *c <= '9' && sub < 0x10000; ++c) sub = sub * 10 + UInt32(*c - '0');
            const int b = subBit(sub);
            if (b < 0 || !v || v->getLength() > 0x196c - 8) { ok = false; break; }
            v->retain();
            OSSafeReleaseNULL(next[b]);
            next[b] = v;
        }
        it->release();
    }
    if (!ok) {
        for (auto &x : next) OSSafeReleaseNULL(x);
        return kIOReturnBadArgument;
    }
    // an empty dictionary clears every override
    IOLockLock(ovrLock_);
    for (unsigned i = 0; i < kOverrides; ++i) { OSSafeReleaseNULL(ovr_[i]); ovr_[i] = next[i]; }
    IOLockUnlock(ovrLock_);
    IOLog("NVAccelerator: AGDC 0x711 overrides set (%u entries)\n", m->getCount());
    return kIOReturnSuccess;
}

bool NVGraphicsDeviceControl::applyOverride(UInt32 sub, uint8_t *o, unsigned long osz) {
    const int b = subBit(sub);
    if (b < 0 || !o || osz != 0x196c) return false;
    OSData *v = nullptr;
    if (ovrLock_) {
        IOLockLock(ovrLock_);
        if ((v = ovr_[b])) v->retain();
        IOLockUnlock(ovrLock_);
    }
    if (!v && provider_) {
        char key[12];
        snprintf(key, sizeof key, "%u", sub);
        if (OSDictionary *g = OSDynamicCast(OSDictionary, provider_->getProperty("NVGspControl-agdc-711")))
            if ((v = OSDynamicCast(OSData, g->getObject(key)))) v->retain();
    }
    if (!v) return false;
    const bool fits = v->getLength() <= osz - 8;
    if (fits) {
        bzero(o + 8, osz - 8);
        memcpy(o + 8, v->getBytesNoCopy(), v->getLength());
    }
    v->release();
    return fits;
}

void *NVGraphicsDeviceControl::vendor_doDeviceAttribute(unsigned int attr, unsigned long *in, unsigned long inSize,
                                                        unsigned long *out, unsigned long *outSize,
                                                        IOExternalMethodArguments *) {
    const unsigned long osz = outSize ? *outSize : 0;
    uint8_t *o = reinterpret_cast<uint8_t *>(out);
    IOReturn r = kIOReturnUnsupported;   // 0xe00002c7, what Paravirt answers for the rest
    switch (attr) {
    case 0x1:                            // kAGDCVendorInfo, 0x2c bytes
        if (osz == 0x2c && o) {
            bzero(o, 0x2c);
            *reinterpret_cast<UInt32 *>(o) = 0x10000;
            memcpy(o + 4, "NVIDIA", 6);
            *reinterpret_cast<UInt32 *>(o + 0x24) =
                (static_cast<UInt32>(gpuVendor_) << 16) | gpuDevice_;
            *reinterpret_cast<UInt32 *>(o + 0x28) = 1;            // a real GPU (Paravirt says 2)
            r = kIOReturnSuccess;
        }
        break;
    case 0x921:                          // kAGDCGPUCapability, 0xb0 bytes (Paravirt's values)
        if (osz == 0xb0 && o) {
            bzero(o, 0xb0);
            *reinterpret_cast<UInt64 *>(o + 0x08) = 1;
            *reinterpret_cast<UInt64 *>(o + 0x1c) = 0x40000000500ULL;
            *reinterpret_cast<UInt32 *>(o + 0x44) = 0x500;
            *reinterpret_cast<UInt64 *>(o + 0x50) = 0x40000000000ULL;
            *reinterpret_cast<UInt64 *>(o + 0x84) = 1;
            r = kIOReturnSuccess;
        }
        break;
    case 0x3:                            // 4 bytes: Paravirt returns a global flag word, 0 here
        if (osz == 4 && o) { *reinterpret_cast<UInt32 *>(o) = 0; r = kIOReturnSuccess; }
        break;
    case 0x980:                          // 0xdc bytes: the framebuffers (masks as Paravirt builds them)
        if (osz == 0xdc && o) {
            bzero(o, 0xdc);
            // 0.3.3: pointers, not IDs; AGDC turns 0x34 (the GPU) and the 0x3c
            // framebuffer list into registry IDs itself (an ID here panicked it)
            loadFramebuffers();
            const UInt32 n = nfb_ ? nfb_ : 1;
            const UInt64 mask = ((1ULL << n) - 1) * 2;
            for (int k = 0; k < 4; ++k) *reinterpret_cast<UInt64 *>(o + 8 * k) = mask;
            for (int k = 0; k < 5; ++k) *reinterpret_cast<UInt32 *>(o + 0x20 + 4 * k) = n;
            // 0.5.26: the GPU (our provider, as Paravirt's getProvider()) and the
            // framebuffer list must be objects. AGDC's filtered_doDeviceAttribute
            // (KDK 26.7.2) turns 0x34 and 0x3c[0x30] into registry IDs before it
            // copies the reply out, so no pointer reaches user space. Nulls here
            // left IOPresentment's device map with F:0x0 (err:gms): a WindowServer
            // started with the accelerator present found no display pipe and
            // aborted in CA::OGL::Context::push_surface (transient surface not found).
            *reinterpret_cast<IOService **>(o + 0x34) = getProvider();
            for (unsigned i = 0; i < nfb_; ++i) *reinterpret_cast<IOService **>(o + 0x3c + 8 * i) = fbs_[i];
            *reinterpret_cast<UInt32 *>(o + 0x30) = nfb_;
            r = kIOReturnSuccess;
        }
        break;
    case 0x2001:                         // 4 bytes in, nothing out: Paravirt just says yes
        r = kIOReturnSuccess;
        break;
    case 0x925:                          // 12 bytes: {fb index + 1, ?, flag}; Paravirt copies a per-fb byte
        if (osz == 0xc && o && in && inSize == 0xc) {
            // `in` is not guaranteed 8-byte aligned; copy the three words out
            // instead of dereferencing a misaligned UInt32 pointer.
            UInt32 vals[3] = {};
            __builtin_memcpy(vals, in, sizeof vals);
            const UInt32 idx = vals[0];
            IOLog("NVAccelerator: AGDC 0x925 in %08x %08x %08x\n", idx, vals[1], vals[2]);
            if (idx <= (nfb_ ? nfb_ : 1)) { *reinterpret_cast<UInt32 *>(o + 8) = 1; r = kIOReturnSuccess; }
        }
        break;
    case 0x711: {                        // 0x196c bytes: {fb index, sub-command, payload}
        const uint8_t *ib = reinterpret_cast<const uint8_t *>(in);
        const UInt32 idx = (ib && inSize >= 8) ? *reinterpret_cast<const UInt32 *>(ib) : ~0u;
        const UInt32 sub = (ib && inSize >= 8) ? *reinterpret_cast<const UInt32 *>(ib + 4) : ~0u;
        // 0.180.12: the old check only required 8 input bytes and then copied
        // the full 0x196c-byte request out of the caller's buffer.
        if (o && osz == 0x196c && ib && ib != o && inSize >= 0x196c)
            memcpy(o, ib, 0x196c);
        // per-display query, answered the way Paravirt's framebuffer does
        auto u32 = [o](unsigned off) -> UInt32 & { return *reinterpret_cast<UInt32 *>(o + off); };
        auto u64 = [o](unsigned off) -> UInt64 & { return *reinterpret_cast<UInt64 *>(o + off); };
        loadFramebuffers();
        if (o && osz == 0x196c && idx < (nfb_ ? nfb_ : 1)) {
            r = kIOReturnSuccess;
            switch (sub) {
            case 0x1:
                u64(0x08) = 0x500000001ULL; u64(0x10) = 0; u64(0x18) = 0x800000000ULL; u32(0x20) = 0;
                u64(0x24) = 0; u32(0x2c) = 0x30300; u64(0x30) = 0x3f800000bf800000ULL;   // -1.0f, 1.0f
                break;
            case 0x2:
                // 0.5.37: one gamma entry at 0xc {flags, u64 plane mask, float
                // non-atomic period, type, fixed format, u64 configuration,
                // float min, float max, u64 max size}. Paravirt's plane mask 0
                // meant "no plane gamma": CoreDisplay's IsEOTFSupportedInHW
                // failed (reason 0x20000) and its GPU colour pass ran on every
                // frame. Plane 0 now has a 3-channel 10-bit (type 0x80) table
                // of kGamma entries; the pipe applies it as the head OLUT.
                u32(0x08) = 1; u32(0x0c) = 5; u64(0x10) = 1; u64(0x18) = 0x8000000000ULL; u32(0x20) = 0;
                u64(0x24) = 0; u64(0x2c) = 0x3f80000000000000ULL; u64(0x34) = kGamma;
                break;
            case 0x4:
                u32(0x08) = 1; bzero(o + 0xc, 0x30);
                break;
            case 0x8:
                u32(0x08) = 1; bzero(o + 0xc, 0x32c);
                break;
            case 0x10: {   // current timing, two identical descriptors (0x28, 0x1b0)
                UInt32 w = 3840, h = 2160;
                if (IOFramebuffer *fb = idx < nfb_ ? fbs_[idx] : nullptr) {
                    IODisplayModeID mode = 0; IOIndex depth = 0; IOPixelInformation pi{};
                    if (fb->getCurrentDisplayMode(&mode, &depth) == kIOReturnSuccess &&
                        fb->getPixelInformation(mode, depth, kIOFBSystemAperture, &pi) == kIOReturnSuccess &&
                        pi.activeWidth) {
                        w = pi.activeWidth; h = pi.activeHeight;
                    }
                }
                // 0.5.36: this is the plane scaler (one entry at 0xc: up count
                // 0x28 + entries, down count 0x1b0 + entries). An entry is
                // {height, width, max height, max width, pixel formats, ...}:
                // IOPresentment turns it into CoreDisplay's {min w, min h,
                // max w, max h}. Width first read min 2160 x 3840, so even a
                // 1:1 4K plane failed "upscaling/downscaling to endpoint" and
                // CoreDisplay ran its GPU colour pass on every frame. min = max
                // = the mode: 1:1 only, the pipe copies and cannot scale.
                u32(0x08) = 1; bzero(o + 0xc, 0x1c);
                static const unsigned kBase[2] = {0x28u, 0x1b0u};
                for (unsigned base : kBase) {
                    u64(base) = 1; u32(base + 0x08) = h; u32(base + 0x0c) = w;
                    u32(base + 0x10) = h; u32(base + 0x14) = w; u32(base + 0x18) = 0x10000;
                    bzero(o + base + 0x1c, 0x16c);
                }
                break;
            }
            case 0x20: case 0x80: case 0x400: case 0x800:
                u64(0x08) = 1; u64(0x10) = 0;
                break;
            case 0x100:
                u32(0x08) = 1; bzero(o + 0xc, 0x14);
                break;
            case 0x200: case 0x2000: case 0x4000:
                u64(0x08) = 1;
                break;
            case 0x1000: case 0x8000:
                u32(0x08) = 1; bzero(o + 0xc, 0x2c);
                break;
            default:
                r = kIOReturnUnsupported;
                break;
            }
        }
        if (o && idx < (nfb_ ? nfb_ : 1) && applyOverride(sub, o, osz)) r = kIOReturnSuccess;   // 0.5.36
        if (sub711_ < 16) { ++sub711_; IOLog("NVAccelerator: AGDC 0x711 fb %u sub 0x%x -> 0x%x\n", idx, sub, r); }
        break;
    }
    default:
        break;
    }
    if (logged_ < 64) {
        ++logged_;
        IOLog("NVAccelerator: AGDC attribute 0x%x (in %lu, out %lu) -> 0x%x\n", attr, inSize, osz, r);
    }
    (void)in;
    return reinterpret_cast<void *>(static_cast<uintptr_t>(r));
}

// ---------------------------------------------------------------- accelerator
class NVAccelerator : public IOGraphicsAccelerator2 {
    OSDeclareDefaultStructors(NVAccelerator)
    IOMemoryDescriptor *stamp_ = nullptr;   // 0.4.0: VRAM stamp region (BAR1) or sysmem fallback
    IOMemoryMap *stampMap_ = nullptr;       // 0.4.1: kernel view for the event machine
    NVAccelDisplayMachine *dm_ = nullptr;   // 0.2.2: not retained (the family owns it)
public:
    // 0.4.0 (native N1): IOAccel stamps written by the GPU, see NVAccelChannel
    IOService *gsp_ = nullptr;              // NVGspControl, retained
    UInt64 stampGpuVa_ = 0;                 // 0 = native path off
    class NVAccelChannel *chan_[2] = {};    // stamp 0 GR, 1 CE
    IOLock *taskLock_ = nullptr;
    volatile UInt32 nativeOut_ = 0;         // native submits so far (0 = never signal)
    // 0.4.5: the GR non-stall interrupt only lands now and then, so the
    // family retired native command buffers on its ~100 ms timer. A poller
    // watches the stamps while work is out and signals them as they land.
    volatile UInt32 stampSubmitted_[2] = {}, stampSignaled_[2] = {};
    // Protected by stampCancelLock_: a rejected stamp may retire only after the
    // last accepted stamp before it has really completed.
    UInt32 stampAccepted_[2] = {}, stampCanceled_[2] = {}, stampCancelAfter_[2] = {};
    bool stampCancelPending_[2] = {}, stampSubmitting_[2] = {};
    IOLock *stampCancelLock_ = nullptr;
    UInt32 canceledCompletions_ = 0;
    void beginNativeStamp(UInt32 index);
    void recordNativeStamp(UInt32 index, UInt32 value, bool accepted);
    void retireCanceledStamps();
    UInt32 failedSubmits_ = 0;              // 0.5.12
    // 0.5.0: the family hands out the stamp value (writeStampCommand) before
    // we put the work on the ring (submitBuffer). Two processes submitting at
    // once could reach the ring in the other order; the later value landed
    // first and completed the earlier command buffer before its work ran, so
    // its memory was freed under the GPU (MMU faults on freed buffers and
    // wrapped surfaces, 28 Sep 20:26). Value and ring order under one lock.
    IOLock *submitLock_ = nullptr;
    UInt64 stampProgressAbs_ = 0;           // last time a stamp moved (stall watchdog)
    UInt32 forcedCompletions_ = 0;
    thread_call_t pollCall_ = nullptr;
    volatile UInt32 pollArmed_ = 0;
    volatile bool pollStop_ = false;
    // 0.4.8 (N6): GPU busy time from native submissions, published as the
    // keys Activity Monitor reads from PerformanceStatistics
    UInt64 busyStartAbs_ = 0, busyAccumAbs_ = 0, busyLastAccumAbs_ = 0, statLastAbs_ = 0;
    UInt32 utilPct_ = 0;
    UInt32 tempC_ = 0;                        // 0.5.5
    const OSSymbol *tempSym_ = nullptr;
    IOLock *statLock_ = nullptr;
    thread_call_t statCall_ = nullptr;
    void noteBusy(bool busy);
    // 0.5.1: PGRAPH busy sampled at 100 Hz through NVGspControl, so every
    // client counts (sync submissions too, not only native ones)
    thread_call_t sampleCall_ = nullptr;
    const OSSymbol *grBusySym_ = nullptr;
    volatile UInt32 samples_ = 0, busySamples_ = 0;
    static void sampleCallout(thread_call_param_t self, thread_call_param_t);
    void startStats();
    static void statCallout(thread_call_param_t self, thread_call_param_t);
    OSDictionary *withUtilization(OSDictionary *family);
    bool setProperty(const OSSymbol *key, OSObject *value) override;
    using IOGraphicsAccelerator2::setProperty;
    UInt32 pollIdle_ = 0;
    void kickStampPoll();
    void checkStamps();
    static void pollCallout(thread_call_param_t self, thread_call_param_t);
    // 0.4.7: the kernel command names its client by tag; a process may
    // hold more than one client (WindowServer does), each with its own arena
    struct TaskClient { void *task; IOUserClient *inner; UInt32 tag; };
    TaskClient tasks_[128] = {};
    void noteTaskClient(void *task, IOUserClient *inner, UInt32 tag, bool add);
    IOUserClient *clientForTag(void *task, UInt32 tag);
    static void stampIrq(void *ref, UInt32 mask);
    IOAccelEventMachine2 *eventMachine() {   // IOGraphicsAccelerator2 + 0x380 (14.8.9)
        return *reinterpret_cast<IOAccelEventMachine2 **>(reinterpret_cast<UInt8 *>(this) + 0x380);
    }
    IOAccelCommandQueue *newCommandQueue() override;
    void nativeStart();
    volatile UInt32 *stampBase();           // 0.4.1
private:
    NVGraphicsDeviceControl *agdc_ = nullptr;   // 0.3.0
    thread_call_t okCall_ = nullptr;
    static void okCallout(thread_call_param_t self, thread_call_param_t);
    NVAccelTask *newTask();

public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;

    IOReturn newUserClient(task *owningTask, void *securityID, unsigned int type,
                           IOUserClient **handler) override;   // 0.3.8
    IOMemoryDescriptor *getStampMemory(unsigned int *count) override;
    IOAccelEventMachine2 *newEventMachine() override;
    IOAccelTask *createUserGPUTask() override;
    IOAccelTask *createKernelGPUTask() override;
    void populateAccelConfig(IOAccelConfig *cfg) override;
    bool configureDevice(IOPCIDevice *pci) override;   // bool: live 0.1.0 (0 = "configureDevice failed")
    void teardownDevice(IOPCIDevice *pci) override;
    IOAccelDisplayMachine *newDisplayMachine() override;
    IOAccelDisplayPipe *newDisplayPipe() override;
    IOService *newGLContext() override { return nullptr; }
    IOService *newCLContext() override { return nullptr; }
    IOService *newSurface() override { return nullptr; }
    IOService *new2DContext() override { return nullptr; }
    IOService *newVideoContext() override { return nullptr; }
    IOAccelSysMemory *newSysMemory() override;
    IOAccelVidMemory *newVidMemory() override;
    IOAccelResource2 *newResource() override;
    IOAccelMemoryMap *newMemoryMap() override;
};
OSDefineMetaClassAndStructors(NVAccelerator, IOGraphicsAccelerator2)

IOService *NVAccelerator::probe(IOService *provider, SInt32 *score) {
    return IOGraphicsAccelerator2::probe(provider, score);
}

// IOGraphicsAccelerator2::enableAccelerator() is non-virtual and not in our
// generated header, but exported (14.8.9 and 26.x). The family only calls it
// from IOAccelDisplayMachine::display_mode_did_change, so an accelerator that
// starts while the display mode is stable stays "disabled" and every
// new_resource blocks in acceleratorWaitEnabled() (27 Sep: metal_test hung in
// newBufferWithLength). We are usable as soon as start succeeds.
extern void ioaf2EnableAccelerator(IOGraphicsAccelerator2 *a) __asm("__ZN22IOGraphicsAccelerator217enableAcceleratorEv");
// The family's display re-probe (IOGraphicsAccelerator2 vtable [229]); bound by
// its mangled name so a vtable-slot mismatch cannot redirect the call.
extern IOReturn ioaf2RequestProbe(IOGraphicsAccelerator2 *a, UInt32 options)
    __asm("__ZN22IOGraphicsAccelerator212requestProbeEj");
// IOFramebuffer finds its accelerator by walking the registry for class
// "IOAccelerator" (KDK 26.7.2, IOFramebuffer::probeAccelerator). Our NVDA
// starts ~40 s before NVAccelerator exists, so it never had one; without it
// CoreDisplay reports "Unable to find display pipe for IOFB service /
// regID: <NVDA>" and WindowServer never submits a display transaction.
extern void ioafbProbeAccelerator(IOFramebuffer *fb)
    __asm("__ZN13IOFramebuffer16probeAcceleratorEv");

void NVAccelerator::okCallout(thread_call_param_t self, thread_call_param_t) {
    NVAccelerator *a = static_cast<NVAccelerator *>(self);
    a->setProperty("NVAccelerator-alive-60s", true);
    // 0.5.28: WindowServer looks the GPU up exactly once, before this
    // accelerator exists (live 15:14:38 "gpu 0x9a50 accel 0x000000000", accel
    // up 15:14:56), so it never submits a display transaction. Give the
    // framebuffer its accelerator after the fact, then nudge the family's
    // display re-probe. Done from this callout, not from start(), because the
    // family's probe takes the accelerator lock. NVRAM nvaccel-reprobe=0 off.
    if (!a->dm_ || !a->dm_->started_) return;
    if (IORegistryEntry *o = IORegistryEntry::fromPath("/options", gIODTPlane)) {
        if (OSData *d = OSDynamicCast(OSData, o->getProperty("nvaccel-reprobe")))
            if (d->getLength() >= 1 && static_cast<const char *>(d->getBytesNoCopy())[0] == '0') {
                o->release();
                return;
            }
        o->release();
    }
    if (OSDictionary *m = IOService::serviceMatching("NVDisplay")) {
        IOService *svc = IOService::copyMatchingService(m);
        m->release();
        if (IOFramebuffer *fb = OSDynamicCast(IOFramebuffer, svc)) {
            ioafbProbeAccelerator(fb);
            NVALOG("framebuffer %s: probeAccelerator", fb->getName());
        }
        if (svc) svc->release();
    }
    const IOReturn r = ioaf2RequestProbe(a, 0x00000001 /* kIOFBUserRequestProbe */);
    NVALOG("reprobe after accelerator up: 0x%x", r);
}

bool NVAccelerator::start(IOService *provider) {
    NVALOG("start: provider %s", provider ? provider->getName() : "?");
    // stamp page(s), as AppleParavirtAccelerator::setupFIFO: 64 KiB,
    // options 0x890 (kernel/user shared, contiguous, in/out)
    taskLock_ = IOLockAlloc();
    statLock_ = IOLockAlloc();
    submitLock_ = IOLockAlloc();
    stampCancelLock_ = IOLockAlloc();
    if (!submitLock_ || !stampCancelLock_) return false;
    // 0.4.0: stamps in VRAM that the GPU releases (native N1), when
    // NVGspControl offers them and NVRAM nvaccel-native isn't "0"
    bool nativeOff = false;
    if (IORegistryEntry *o = IORegistryEntry::fromPath("/options", gIODTPlane)) {
        if (OSData *d = OSDynamicCast(OSData, o->getProperty("nvaccel-native")))
            nativeOff = d->getLength() >= 1 && static_cast<const char *>(d->getBytesNoCopy())[0] == '0';
        o->release();
    }
    if (OSDictionary *m = IOService::serviceMatching("NVGspControl")) {
        gsp_ = IOService::copyMatchingService(m);
        m->release();
    }
    if (gsp_ && !nativeOff && taskLock_) {
        UInt64 region[3] = {};
        IOMemoryDescriptor *md = nullptr;
        const IOReturn r = gsp_->callPlatformFunction("nvgsp-stamp-region", false, region, &md, nullptr, nullptr);
        if (r == kIOReturnSuccess && md && region[2] >= 0x1000) {
            stamp_ = md;
            stampGpuVa_ = region[1];
            NVALOG("native: stamps in VRAM at 0x%llx, GPU VA 0x%llx", region[0], region[1]);
        } else {
            if (md) md->release();
            NVALOG("native: no stamp region (0x%x), family stamps only", r);
        }
    }
    if (!stamp_) {
        IOBufferMemoryDescriptor *b = IOBufferMemoryDescriptor::withOptions(0x890, 0x10000, 1);
        if (b) bzero(b->getBytesNoCopy(), 0x10000);
        stamp_ = b;
    }
    if (!stamp_ || !IOGraphicsAccelerator2::start(provider)) {
        NVALOG("IOGraphicsAccelerator2::start failed");
        return false;
    }
    setProperty("NVAccelerator-version", KEXT_BUNDLE_VERSION);   // from Info.plist (build_kext.sh)
    // 0.5.25: IOGraphicsAccelerator2::start advertises IOAccelerator2D.plugin
    // through IOCFPlugInTypes. WindowServer then loads that plugin for this
    // service, its Start() opens the service with connect type 2, the family
    // answers kIOReturnUnsupported (the 0xccf display-pipe gate is false) and
    // CA subsequently aborts in CA::OGL::Context::push_surface on a null
    // flatten surface. Until the type-2 2D client is implemented, do not
    // advertise the plugin: WindowServer then keeps the software surface path
    // that works when the accelerator appears after it started.
    removeProperty("IOCFPlugInTypes");
    if (stampGpuVa_) nativeStart();
    ioaf2EnableAccelerator(this);
    setProperty("NVAccelerator-enabled-at-start", true);
    // 0.2.2: the family did not start the display machine on the PCI device
    // (NVDisplay is not a child of it): give it NVDisplay ourselves
    if (dm_ && !dm_->started_) {
        NVALOG("display machine not started by the family: starting it with NVDisplay");
        dm_->start(static_cast<IOPCIDevice *>(provider));
    }
    // 0.2.3: the family publishes DisplayPipeSupported=No when start finds no
    // pipe; with NVDisplay's pipe in place say what the NVIDIA web driver
    // said, or CoreDisplay reports "Unable to find display pipe for IOFB"
    if (OSDictionary *caps = OSDictionary::withCapacity(2)) {
        caps->setObject("DisplayPipeSupported", kOSBooleanTrue);
        caps->setObject("TransactionsSupported", kOSBooleanTrue);
        setProperty("IOAccelDisplayPipeCapabilities", caps);
        caps->release();
    }
    // IOGraphicsAccelerator2::start leaves the service unregistered (live
    // 0.1.3: !registered); Metal discovers accelerators by matching.
    registerService();
    // 0.3.0: an AGDC service on the GPU for IOPresentment (NVRAM nvaccel-agdc=0 skips it)
    bool agdcOff = false;
    if (IORegistryEntry *o = IORegistryEntry::fromPath("/options", gIODTPlane)) {
        if (OSData *d = OSDynamicCast(OSData, o->getProperty("nvaccel-agdc")))
            agdcOff = d->getLength() >= 1 && static_cast<const char *>(d->getBytesNoCopy())[0] == '0';
        o->release();
    }
    if (!agdcOff && !agdc_) {
        agdc_ = OSTypeAlloc(NVGraphicsDeviceControl);
        if (agdc_) agdc_->setProvider(provider);
        if (agdc_ && agdc_->init() && agdc_->attach(provider) && agdc_->start(provider)) {
            agdc_->registerService();
            NVALOG("AGDC service started");
        } else {
            NVALOG("AGDC service failed");
            if (agdc_) { agdc_->detach(provider); OSSafeReleaseNULL(agdc_); }
        }
    }
    okCall_ = thread_call_allocate(&NVAccelerator::okCallout, this);
    if (okCall_) {
        UInt64 deadline = 0;
        clock_interval_to_deadline(60, kSecondScale, &deadline);
        thread_call_enter_delayed(okCall_, deadline);
    }
    startStats();                                    // 0.5.1: native or not
    // 0.5.18: the IOAccelerator is registered and the display pipe is up.
    // Let NVGspControl's boot hold release WindowServer, so SkyLight starts
    // on the Metal compositor instead of the firmware-screen software path.
    if (gsp_)
        gsp_->callPlatformFunction("nvgsp-boot-hold-release", false,
                                   nullptr, nullptr, nullptr, nullptr);
    NVALOG("started");
    return true;
}

void NVAccelerator::stop(IOService *provider) {
    NVALOG("stop");
    if (gsp_ && stampGpuVa_)
        gsp_->callPlatformFunction("nvgsp-stamp-register", false, nullptr, nullptr, nullptr, nullptr);
    if (statCall_) {
        pollStop_ = true;
        thread_call_cancel(statCall_);
        for (int i = 0; i < 2000 && !thread_call_free(statCall_); ++i) IOSleep(1);
        statCall_ = nullptr;
    }
    if (sampleCall_) {
        pollStop_ = true;
        thread_call_cancel(sampleCall_);
        for (int i = 0; i < 2000 && !thread_call_free(sampleCall_); ++i) IOSleep(1);
        sampleCall_ = nullptr;
    }
    OSSafeReleaseNULL(grBusySym_);
    OSSafeReleaseNULL(tempSym_);
    if (pollCall_) {   // no thread_call_cancel_wait for kexts: stop re-arming, cancel, free when idle
        pollStop_ = true;
        thread_call_cancel(pollCall_);
        for (int i = 0; i < 1000 && !thread_call_free(pollCall_); ++i) IOSleep(1);
        pollCall_ = nullptr;
    }
    IOGraphicsAccelerator2::stop(provider);
}

void NVAccelerator::free() {
    if (okCall_) {
        thread_call_cancel(okCall_);
        thread_call_free(okCall_);
        okCall_ = nullptr;
    }
    OSSafeReleaseNULL(stampMap_);
    OSSafeReleaseNULL(stamp_);
    for (auto *&c : chan_) if (c) { reinterpret_cast<OSObject *>(c)->release(); c = nullptr; }
    OSSafeReleaseNULL(gsp_);
    if (taskLock_) { IOLockFree(taskLock_); taskLock_ = nullptr; }
    if (statLock_) { IOLockFree(statLock_); statLock_ = nullptr; }
    if (submitLock_) { IOLockFree(submitLock_); submitLock_ = nullptr; }
    if (stampCancelLock_) { IOLockFree(stampCancelLock_); stampCancelLock_ = nullptr; }
    IOGraphicsAccelerator2::free();
}

// 0.3.8: GPU access for sandboxed processes. NVMTLDriver and the Vulkan
// driver talk to NVGspControl's user client, whose class the sandboxes of
// Safari/WebKit, iconservicesagent, VTDecoderXPCService, mediaanalysisd ...
// don't list. Their Metal rules do allow IOAccelSharedUserClient2 (class
// filters match superclasses) on an IOAccelerator, so the accelerator hands
// out, for connect type 'NVGP', this IOAccelSharedUserClient2 that owns a
// real NVGspControl client and passes every call through. NVGspControl
// stays in the boot collection; IOAcceleratorFamily2 isn't there.
static constexpr unsigned int kNVGspConnectType = 0x4E564750;   // 'NVGP'

class IOAccelNVGspSharedClient : public IOAccelSharedUserClient2 {
    OSDeclareDefaultStructors(IOAccelNVGspSharedClient)
    IOUserClient *inner_ = nullptr;
public:
    bool setInner(IOUserClient *uc) { inner_ = uc; return uc != nullptr; }
    // none of IOAccelSharedUserClient2's own code runs: it expects an
    // IOAccelShared2 we never make
    bool initWithTask(task_t t, void *sec, UInt32 type, OSDictionary *props) override {
        return IOUserClient::initWithTask(t, sec, type, props);
    }
    bool start(IOService *provider) override { return IOUserClient::start(provider); }
    void stop(IOService *provider) override { IOUserClient::stop(provider); }
    bool requestTerminate(IOService *p, unsigned int o) override { return IOUserClient::requestTerminate(p, o); }
    bool didTerminate(IOService *p, unsigned int o, bool *d) override { return IOUserClient::didTerminate(p, o, d); }
    IOReturn connectClient(IOUserClient *c) override { return IOUserClient::connectClient(c); }
    IOExternalMethod *getTargetAndMethodForIndex(IOService **t, unsigned int i) override {
        return IOUserClient::getTargetAndMethodForIndex(t, i);
    }
    IOReturn externalMethod(unsigned int sel, IOExternalMethodArguments *a, IOExternalMethodDispatch *,
                            OSObject *, void *) override {
        if (sel == 0x4E5654) {   // 'NVT' (0.4.7): this client's tag for native kernel commands
            if (!a || a->scalarOutputCount != 1 || !a->scalarOutput) return kIOReturnBadArgument;
            UInt64 generation = 0;
            IOExternalMethodArguments valid{};
            valid.scalarOutput = &generation; valid.scalarOutputCount = 1;
            const IOReturn status = inner_ ? inner_->externalMethod(37, &valid, nullptr, nullptr, nullptr)
                                          : kIOReturnOffline;
            if (status != kIOReturnSuccess) return status;
            a->scalarOutput[0] = tag_;
            return tag_ ? kIOReturnSuccess : kIOReturnNotReady;
        }
        return inner_ ? inner_->externalMethod(sel, a, nullptr, nullptr, nullptr) : kIOReturnNotAttached;
    }
    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *opts, IOMemoryDescriptor **mem) override {
        return inner_ ? inner_->clientMemoryForType(type, opts, mem) : kIOReturnNotAttached;
    }
    NVAccelerator *accel_ = nullptr;   // 0.4.0: task -> client table for kernel submits
    void *task_ = nullptr;
    UInt32 tag_ = 0;                   // 0.4.7: names this client in native kernel commands
    void setAccel(NVAccelerator *a, void *t) {
        accel_ = a; task_ = t;
        do { tag_ = static_cast<UInt32>(random()) ^ static_cast<UInt32>(reinterpret_cast<uintptr_t>(this) >> 4); } while (!tag_);
    }
    UInt32 tag() const { return tag_; }
    IOReturn clientClose() override {
        IOUserClient *in = inner_;
        if (accel_ && in) accel_->noteTaskClient(task_, in, 0, false);
        inner_ = nullptr;
        if (in) { in->clientClose(); in->release(); }   // frees the owner's GPU objects
        if (!isInactive()) terminate();
        return kIOReturnSuccess;
    }
    void free() override {
        if (accel_ && inner_) accel_->noteTaskClient(task_, inner_, 0, false);
        if (inner_) { inner_->clientClose(); inner_->release(); inner_ = nullptr; }
        IOUserClient::free();
    }
};
OSDefineMetaClassAndStructors(IOAccelNVGspSharedClient, IOAccelSharedUserClient2)

IOReturn NVAccelerator::newUserClient(task *owningTask, void *securityID, unsigned int type,
                                      IOUserClient **handler) {
    if (type != kNVGspConnectType)
        return IOGraphicsAccelerator2::newUserClient(owningTask, securityID, type, handler);
    if (!handler) return kIOReturnBadArgument;
    OSDictionary *m = IOService::serviceMatching("NVGspControl");
    IOService *gsp = m ? IOService::copyMatchingService(m) : nullptr;
    if (m) m->release();
    if (!gsp) return kIOReturnNotReady;
    // the NVGspControl client, as IOServiceOpen on NVGspControl would make it
    OSObject *o = OSMetaClass::allocClassWithName("IOAccelNVGspUserClient");
    IOUserClient *in = OSDynamicCast(IOUserClient, o);
    bool ok = in && in->initWithTask(owningTask, securityID, 0, nullptr) && in->attach(gsp);
    if (ok && !in->start(gsp)) { in->detach(gsp); ok = false; }
    gsp->release();
    if (!ok) { if (o) o->release(); return kIOReturnNotReady; }
    auto *uc = OSTypeAlloc(IOAccelNVGspSharedClient);
    if (!uc || !uc->initWithTask(owningTask, securityID, type, nullptr) || !uc->setInner(in) ||
        !uc->attach(this)) {
        in->clientClose(); in->release();
        if (uc) { uc->setInner(nullptr); uc->release(); }
        return kIOReturnNoMemory;
    }
    if (!uc->start(this)) { uc->detach(this); uc->release(); return kIOReturnError; }
    uc->setAccel(this, owningTask);
    noteTaskClient(owningTask, in, uc->tag(), true);
    *handler = uc;
    return kIOReturnSuccess;
}

IOMemoryDescriptor *NVAccelerator::getStampMemory(unsigned int *count) {
    if (count) *count = 0;           // as Paravirt
    return stamp_;
}

IOAccelEventMachine2 *NVAccelerator::newEventMachine() {
    NVALOG("newEventMachine");
    return OSTypeAlloc(NVAccelEventMachine);
}

NVAccelTask *NVAccelerator::newTask() {
    NVAccelTask *t = OSTypeAlloc(NVAccelTask);
    // 16 GiB of GPU VA per task, as Paravirt
    if (t && !t->initWithAccel(this, 0x400000000ULL)) OSSafeReleaseNULL(t);
    return t;
}

IOAccelTask *NVAccelerator::createUserGPUTask() {
    NVALOG("createUserGPUTask");
    return newTask();
}

IOAccelTask *NVAccelerator::createKernelGPUTask() {
    NVALOG("createKernelGPUTask");
    return newTask();
}

// Paravirt's values (field meanings to be pinned down): name, flags, GPU VA
// base 0x20_0000_0000 / 1 GiB, ...
void NVAccelerator::populateAccelConfig(IOAccelConfig *cfg) {
    auto *b = reinterpret_cast<UInt8 *>(cfg);
    static const char kName[] = "NVIDIA GeForce RTX 4080";
    *reinterpret_cast<const char **>(b + 0x00) = kName;
    // 0.3.10: bit 21 = every IOAccel resource in system memory
    // (IOAccelResource2::allocMemory tests accel+0xc90 = cfg+0x08, bit 0x15,
    // and skips createVidMemory). Our VRAM belongs to NVGspControl, not to
    // the family; with the display pipe on, WindowServer's VRAM resources
    // otherwise hit the VidMemory stub and fail to prepare.
    *reinterpret_cast<UInt32 *>(b + 0x08) = 0x480000 | 0x200000;
    *reinterpret_cast<UInt64 *>(b + 0x0c) = 7;
    *reinterpret_cast<UInt64 *>(b + 0x14) = 0x2000000000ULL;
    *reinterpret_cast<UInt64 *>(b + 0x20) = 0x40000000ULL;
    *reinterpret_cast<UInt64 *>(b + 0x28) = 0x100000000008ULL;
    *reinterpret_cast<UInt32 *>(b + 0x30) = 0x40004000;
    b[0x47] = 1;
    *reinterpret_cast<UInt32 *>(b + 0x5c) = 4;
    *reinterpret_cast<UInt32 *>(b + 0x64) = 2;
    *reinterpret_cast<UInt64 *>(b + 0x68) = 0x10000000aULL;
    *reinterpret_cast<UInt64 *>(b + 0x88) = 0;
    NVALOG("populateAccelConfig");
}

bool NVAccelerator::configureDevice(IOPCIDevice *pci) {
    // The hardware belongs to NVGspControl (GSP boot, channels, memory); M1
    // needs nothing here.
    NVALOG("configureDevice %p", pci);
    return true;
}

void NVAccelerator::teardownDevice(IOPCIDevice *) { NVALOG("teardownDevice"); }

IOAccelDisplayMachine *NVAccelerator::newDisplayMachine() {
    NVALOG("newDisplayMachine");
    dm_ = OSTypeAlloc(NVAccelDisplayMachine);
    return dm_;
}

// 0.2.0: NVRAM nvaccel-pipe=0 turns the display pipe off again
IOAccelDisplayPipe *NVAccelerator::newDisplayPipe() {
    bool off = false;
    if (IORegistryEntry *o = IORegistryEntry::fromPath("/options", gIODTPlane)) {
        if (OSData *d = OSDynamicCast(OSData, o->getProperty("nvaccel-pipe")))
            off = d->getLength() >= 1 && static_cast<const char *>(d->getBytesNoCopy())[0] == '0';
        o->release();
    }
    NVALOG("newDisplayPipe%s", off ? " (off by nvaccel-pipe=0)" : "");
    return off ? nullptr : OSTypeAlloc(NVAccelDisplayPipe);
}

// 0.5.3: no log in the allocation hooks (tens of thousands of lines a
// minute once apps run)
IOAccelSysMemory *NVAccelerator::newSysMemory() {
    return OSTypeAlloc(NVAccelSysMemory);
}

IOAccelVidMemory *NVAccelerator::newVidMemory() {
    return OSTypeAlloc(NVAccelVidMemory);
}
IOAccelResource2 *NVAccelerator::newResource() {
    return OSTypeAlloc(NVAccelResource);
}
IOAccelMemoryMap *NVAccelerator::newMemoryMap() {
    return OSTypeAlloc(NVAccelMemoryMap);
}

// ---------------------------------------------------------------- launcher
// See the 0.1.6 note at the top. Matches IOResources (IOResourceMatch IOKit,
// always there), so it never blocks anyone's matching.
class NVAccelLauncher : public IOService {
    OSDeclareDefaultStructors(NVAccelLauncher)
    thread_call_t poll_ = nullptr;
    bool launched_ = false;
    UInt32 tries_ = 0;
    static void pollCallout(thread_call_param_t self, thread_call_param_t);
    void armPoll();
    bool launch();

public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;
};
OSDefineMetaClassAndStructors(NVAccelLauncher, IOService)

bool NVAccelLauncher::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    poll_ = thread_call_allocate(&NVAccelLauncher::pollCallout, this);
    if (!poll_) return false;
    setProperty("NVAccelerator-launcher", "waiting for NVAcceleratorGo");
    armPoll();
    return true;
}

void NVAccelLauncher::armPoll() {
    UInt64 deadline = 0;
    clock_interval_to_deadline(1, kSecondScale, &deadline);
    thread_call_enter_delayed(poll_, deadline);
}

void NVAccelLauncher::pollCallout(thread_call_param_t p, thread_call_param_t) {
    NVAccelLauncher *self = static_cast<NVAccelLauncher *>(p);
    if (self->launched_) return;
    IOService *res = IOService::getResourceService();
    if (res && res->getProperty("NVAcceleratorGo")) {
        self->launched_ = true;
        self->launch();
        return;
    }
    self->tries_++;
    self->armPoll();
}

bool NVAccelLauncher::launch() {
    OSNumber *idNum = OSDynamicCast(OSNumber, getProperty("NVAcceleratorPCIID"));
    OSDictionary *tmpl = OSDynamicCast(OSDictionary, getProperty("NVAcceleratorProperties"));
    if (!idNum || !tmpl) {
        setProperty("NVAccelerator-launcher", "bad personality");
        return false;
    }
    const UInt32 want = idNum->unsigned32BitValue();
    IOPCIDevice *gpu = nullptr;
    OSDictionary *match = IOService::serviceMatching("IOPCIDevice");
    OSIterator *it = match ? IOService::getMatchingServices(match) : nullptr;
    OSSafeReleaseNULL(match);
    if (it) {
        while (OSObject *o = it->getNextObject()) {
            IOPCIDevice *d = OSDynamicCast(IOPCIDevice, o);
            if (d && d->configRead32(kIOPCIConfigVendorID) == want) {
                gpu = d;
                gpu->retain();
                break;
            }
        }
        it->release();
    }
    if (!gpu) {
        NVALOG("launcher: no PCI device 0x%08x", want);
        setProperty("NVAccelerator-launcher", "no GPU");
        return false;
    }
    OSDictionary *props = OSDictionary::withDictionary(tmpl);
    NVAccelerator *accel = OSTypeAlloc(NVAccelerator);
    bool ok = props && accel && accel->init(props);
    if (ok) ok = accel->attach(gpu);
    if (ok) {
        if (!accel->start(gpu)) {
            accel->detach(gpu);
            ok = false;
        }
    }
    NVALOG("launcher: accelerator on %s after %u polls: %s", gpu->getName(), tries_,
           ok ? "started" : "FAILED");
    setProperty("NVAccelerator-launcher", ok ? "started" : "start failed");
    OSSafeReleaseNULL(accel);   // the registry holds it once attached
    OSSafeReleaseNULL(props);
    gpu->release();
    return ok;
}

void NVAccelLauncher::stop(IOService *provider) {
    if (poll_) thread_call_cancel(poll_);
    IOService::stop(provider);
}

void NVAccelLauncher::free() {
    if (poll_) {
        thread_call_cancel(poll_);
        thread_call_free(poll_);
        poll_ = nullptr;
    }
    IOService::free();
}


// ---------------------------------------------------------------- native N1 (0.4.0)
// The Apple model (docs/APPLE-GPU-STACK-STUDY.md, sections 7-8, taken from
// AppleParavirtGPU in the 14.8.9 KDK): work arrives as a vendor kernel
// command in a command buffer segment; the queue asks the channel for a
// stamp (the family picks the value and calls our writeStamp), submits on
// the channel and ties the command buffer's event to the stamp. The GPU
// releases the stamp after the work; the fence interrupt calls signalStamp
// and the family retires the command buffer and tells the app.

// Vendor kernel command, type 0x10000 as Paravirt's ExecIndirect:
// {type, size} header, engine, segment count, then n x {va, dwords, flags}.
struct NVExecKernelCommand {
    UInt32 type, size;
    UInt32 engine, n;
    UInt32 tag, reserved;   // 0.4.7: the NVGP client (IOAccelNVGspSharedClient tag) whose arena runs it
    struct { UInt64 va; UInt32 dwords, flags; } seg[];
};
static constexpr UInt32 kNVExecKernelCommand = 0x10000;
static constexpr UInt32 kNVExecMaxSegs = 64;

// What rides in IOAccelCommandDescriptor +0x20 (Paravirt: the command words)
struct NVChannelSubmit {
    NVStampRec stamp;
    IOUserClient *owner;
    const NVExecKernelCommand *cmd;
    IOReturn result;
};

class NVAccelChannel : public IOAccelFIFOChannel2 {
    OSDeclareDefaultStructors(NVAccelChannel)
public:
    NVAccelerator *accel_ = nullptr;
    UInt32 engine_ = 0;
    UInt32 submits_ = 0;
    // AppleParavirtRootChannel::submitBuffer writes desc[+0x20], desc[+0x30] dwords
    void *submitBuffer(IOAccelCommandDescriptor *desc) override {
        auto *sub = *reinterpret_cast<NVChannelSubmit **>(reinterpret_cast<UInt8 *>(desc) + 0x20);
        if (!sub || !accel_ || !accel_->gsp_) return nullptr;
        const NVExecKernelCommand *c = sub->cmd;
        if (!c || !c->n || c->n > kNVExecMaxSegs) return nullptr;
        UInt64 va[kNVExecMaxSegs];
        UInt32 dw[kNVExecMaxSegs], fl[kNVExecMaxSegs];
        for (UInt32 i = 0; i < c->n; ++i) { va[i] = c->seg[i].va; dw[i] = c->seg[i].dwords; fl[i] = c->seg[i].flags; }
        NVGspKernelSubmit ks{};
        ks.version = 1; ks.engine = engine_; ks.owner = sub->owner; ks.n = c->n;
        ks.va = va; ks.dwords = dw; ks.flags = fl;
        if (sub->stamp.idx < -1 || sub->stamp.idx > 1) return nullptr;
        ks.stampVa = sub->stamp.idx >= 0
            ? accel_->stampGpuVa_ + UInt64(sub->stamp.idx) * 4 : 0;
        ks.stampValue = sub->stamp.value;
        if (sub->stamp.idx >= 0 && sub->stamp.idx < 2) accel_->beginNativeStamp(sub->stamp.idx);
        sub->result = accel_->gsp_->callPlatformFunction("nvgsp-submit-stamp", false, &ks, nullptr, nullptr, nullptr);
        ++submits_;
        if (sub->result == kIOReturnSuccess) {
            OSIncrementAtomic(&accel_->nativeOut_);
            accel_->noteBusy(true);
            if (sub->stamp.idx >= 0 && sub->stamp.idx < 2)
                accel_->recordNativeStamp(sub->stamp.idx, sub->stamp.value, true);
            accel_->kickStampPoll();
        } else if (sub->stamp.idx >= 0 && sub->stamp.idx < 2) {
            // The family assigned an event to work the GPU never accepted.
            // Retire it in order after earlier accepted work, preserving the
            // submission error; a healthy delay cannot justify forging fences.
            accel_->recordNativeStamp(sub->stamp.idx, sub->stamp.value, false);
            ++accel_->failedSubmits_;
            accel_->kickStampPoll();
        }
        return nullptr;
    }
    void *resetHardwareAndReplay() override { return nullptr; }    // N5
    void *getHardwareDiagnosisReport(unsigned int *) override { return nullptr; }
};
OSDefineMetaClassAndStructors(NVAccelChannel, IOAccelFIFOChannel2)

// IOAccelChannel2 slots used below, as AppleParavirtCommandQueue calls them
// (vtable offsets from the 14.8.9 disassembly)
static inline void chanWriteStampCommand(IOAccelChannel2 *c, NVStampRec *rec) {
    c->writeStampCommand(nullptr, reinterpret_cast<vendevtCommandRec *>(rec));
}

// IOAccelCommandQueue::parseSegmentList(shared, header) is `return NULL`
// in the family: the vendor makes the segment's resource list (Paravirt:
// AppleParavirtSegmentResourceList + initWithSharedResourceList). Without
// one no segment's kernel commands ever run. The family's own class does.
extern bool ioaf2SegListInit(OSObject *list, IOAccelShared2 *sh, const IOAccelSegmentResourceListHeader *h,
                             IOAccelDeviceShmem *shmem)
    __asm("__ZN26IOAccelSegmentResourceList26initWithSharedResourceListEP14IOAccelShared2PK32IOAccelSegmentResourceListHeaderP18IOAccelDeviceShmem");

class NVAccelCommandQueue : public IOAccelCommandQueue {
    OSDeclareDefaultStructors(NVAccelCommandQueue)
public:
    NVAccelerator *accel_ = nullptr;
    void *parseSegmentList(IOAccelShared2 *sh, IOAccelSegmentResourceListHeader const *h,
                           IOAccelDeviceShmem *shmem) override {
        OSObject *l = OSMetaClass::allocClassWithName("IOAccelSegmentResourceList");
        if (l && ioaf2SegListInit(l, sh, h, shmem)) return l;
        if (l) l->release();
        static UInt32 nlog;
        if (nlog++ < 16) NVALOG("native: segment resource list failed");
        return nullptr;
    }
    void *processSegmentKernelCommand(IOAccelSegmentResourceList *list, IOAccelKernelCommand const *kc,
                                      IOAccelKernelCommand const *end) override {
        const auto *c = reinterpret_cast<const NVExecKernelCommand *>(kc);
        if (c->type != kNVExecKernelCommand)
            return IOAccelCommandQueue::processSegmentKernelCommand(list, kc, end);
        const UInt8 *cb = reinterpret_cast<const UInt8 *>(kc), *ce = reinterpret_cast<const UInt8 *>(end);
        IOReturn r = kIOReturnBadArgument;
        if (accel_ && accel_->stampGpuVa_ && ce - cb >= 24 && c->engine < 2 && c->n && c->n <= kNVExecMaxSegs &&
            c->size >= 24 + c->n * 16 && cb + c->size <= ce) {
            NVAccelChannel *ch = accel_->chan_[c->engine];
            IOUserClient *owner = accel_->clientForTag(getOwningTask(), c->tag);
            if (ch && owner) {
                UInt64 generation = 0;
                IOExternalMethodArguments valid{};
                valid.scalarOutput = &generation; valid.scalarOutputCount = 1;
                r = owner->externalMethod(37, &valid, nullptr, nullptr, nullptr);
                if (r != kIOReturnSuccess) {
                    owner->release();
                    setSubmissionError(10);
                    return nullptr;  // no stamp/FIFO event for rejected old-generation work
                }
                if (accel_->submitLock_) IOLockLock(accel_->submitLock_);
                NVChannelSubmit sub{};
                sub.stamp.magic = kNVStampRecMagic; sub.stamp.idx = -1;
                sub.owner = owner; sub.cmd = c; sub.result = kIOReturnNotReady;
                // as AppleParavirtCommandQueue::processExecIndirect: next stamp
                // value first (channel slot 0x148), else every submit reuses the
                // old value and the command buffer completes before the GPU ran
                ch->incrementStamp();
                chanWriteStampCommand(ch, &sub.stamp);          // family -> our writeStamp(idx, value)
                // submitCommands can wait on a reused FIFO event before it
                // reaches submitBuffer. Arm recovery for the assigned stamp
                // now, so that wait cannot hide outstanding work from poll.
                if (sub.stamp.idx >= 0 && sub.stamp.idx < 2) {
                    const UInt32 i = static_cast<UInt32>(sub.stamp.idx);
                    if (static_cast<SInt32>(sub.stamp.value - accel_->stampSubmitted_[i]) > 0) {
                        if (accel_->stampSignaled_[i] == accel_->stampSubmitted_[i])
                            clock_get_uptime(&accel_->stampProgressAbs_);
                        accel_->stampSubmitted_[i] = sub.stamp.value;
                    }
                    accel_->kickStampPoll();
                }
                alignas(16) UInt8 desc[0x60] = {};
                *reinterpret_cast<void **>(desc + 0x10) = this;
                *reinterpret_cast<NVChannelSubmit **>(desc + 0x20) = &sub;
                *reinterpret_cast<UInt32 *>(desc + 0x30) = sizeof(NVChannelSubmit) / 4;
                ch->submitCommands(reinterpret_cast<IOAccelCommandDescriptor *>(desc));
                // the command buffer completes when the GPU reaches the stamp
                ch->setEventStamp(reinterpret_cast<IOAccelEvent *>(reinterpret_cast<UInt8 *>(this) + 0x5c8));
                if (accel_->submitLock_) IOLockUnlock(accel_->submitLock_);
                r = sub.result;
            } else r = kIOReturnNotReady;
            if (owner) owner->release();
        }
        if (r != kIOReturnSuccess) {
            static UInt32 nlog;
            if (nlog++ < 32) NVALOG("native: exec kernel command failed 0x%x", r);
            setSubmissionError(10);
        }
        return nullptr;
    }
};
OSDefineMetaClassAndStructors(NVAccelCommandQueue, IOAccelCommandQueue)

IOAccelCommandQueue *NVAccelerator::newCommandQueue() {
    if (!stampGpuVa_) return IOGraphicsAccelerator2::newCommandQueue();
    auto *q = OSTypeAlloc(NVAccelCommandQueue);
    if (q) q->accel_ = this;
    return q;
}

void NVAccelerator::nativeStart() {
    for (UInt32 e = 0; e < 2; ++e) {
        auto *c = OSTypeAlloc(NVAccelChannel);
        // AppleParavirtAccelerator::setupChannels: ROOT = init(accel, stamp 0, 0x80, 0x90, 0x4800)
        if (c && c->init(this, static_cast<int>(e), 0x80, 0x90, 0x4800)) {
            c->accel_ = this; c->engine_ = e; chan_[e] = c;
        } else {
            if (c) c->release();
            NVALOG("native: channel %u init failed, native path off", e);
            stampGpuVa_ = 0;
            return;
        }
    }
    pollCall_ = thread_call_allocate_with_options(&NVAccelerator::pollCallout, this, THREAD_CALL_PRIORITY_KERNEL_HIGH, 0);
    const IOReturn r = gsp_->callPlatformFunction("nvgsp-stamp-register", false,
        reinterpret_cast<void *>(&NVAccelerator::stampIrq), this, nullptr, nullptr);
    if (r != kIOReturnSuccess) { NVALOG("native: stamp interrupt not registered 0x%x", r); stampGpuVa_ = 0; return; }
    // what the family set up in our event machine (14.8.9 offsets)
    if (IOAccelEventMachine2 *em = eventMachine()) {
        const UInt8 *e = reinterpret_cast<const UInt8 *>(em);
        const UInt64 dbg[4] = {*reinterpret_cast<const UInt64 *>(e + 0x20), *reinterpret_cast<const UInt64 *>(e + 0x28),
                               *reinterpret_cast<const UInt32 *>(e + 0x30), reinterpret_cast<uintptr_t>(stamp_)};
        setProperty("NVAccelerator-em-stampbase-ptrs-count", const_cast<UInt64 *>(dbg), sizeof(dbg));
    } else setProperty("NVAccelerator-em-missing", true);
    startStats();
    setProperty("NVAccelerator-native", true);
    NVALOG("native: GR/CE channels on stamps 0/1");
}

// MSI workloop, NVGspControl's lock dropped: AppleParavirtEventMachine::signalStamps
// 0.4.1: only once the family has set the event machine's per-stamp
// pointers (+0x28, read by signalStamp) and only while native work is out.
// 0.4.0 signalled on every GR interrupt and panicked on a NULL table.
void NVAccelerator::stampIrq(void *ref, UInt32 mask) {
    auto *self = static_cast<NVAccelerator *>(ref);
    if (!self || !self->nativeOut_) return;
    (void)mask;
    self->checkStamps();
}

// 0.4.6: signalStamp only wakes the event machine; the fence machine
// (IOGraphicsAccelerator2 + 0x390) then waited for its own ~100 ms
// fence_timeout before retiring the command buffer. notify_fences runs its
// eventfence_notifier now (thread context, never from the interrupt itself).
extern void ioaf2NotifyFences(void *fm) __asm("__ZN19IOAccelFenceMachine13notify_fencesEv");

// signal every stamp the GPU moved since last time (interrupt or poller)
void NVAccelerator::checkStamps() {
    IOAccelEventMachine2 *em = eventMachine();
    if (!em) return;
    void *const *stampPtrs = *reinterpret_cast<void *const **>(reinterpret_cast<UInt8 *>(em) + 0x28);
    volatile UInt32 *base = stampMap_ ? reinterpret_cast<volatile UInt32 *>(stampMap_->getVirtualAddress()) : nullptr;
    if (!stampPtrs || !stampPtrs[0] || !stampPtrs[1] || !base) return;
    bool moved = false;
    for (UInt32 i = 0; i < 2; ++i) {
        const UInt32 now = base[i], was = stampSignaled_[i];
        if (now != was && OSCompareAndSwap(was, now, &stampSignaled_[i])) {
            em->signalStamp(static_cast<int>(i), 0);
            moved = true;
        }
    }
    if (moved) clock_get_uptime(&stampProgressAbs_);
    void *fm = *reinterpret_cast<void **>(reinterpret_cast<UInt8 *>(this) + 0x390);
    if (moved && fm) ioaf2NotifyFences(fm);
    if (moved && stampSignaled_[0] == stampSubmitted_[0] && stampSignaled_[1] == stampSubmitted_[1]) noteBusy(false);
}

void NVAccelerator::noteBusy(bool busy) {
    if (!statLock_) return;
    UInt64 now = 0;
    clock_get_uptime(&now);
    IOLockLock(statLock_);
    if (busy && !busyStartAbs_) busyStartAbs_ = now;
    else if (!busy && busyStartAbs_) { busyAccumAbs_ += now - busyStartAbs_; busyStartAbs_ = 0; }
    IOLockUnlock(statLock_);
}

void NVAccelerator::startStats() {
    if (statCall_ || !statLock_) return;
    grBusySym_ = OSSymbol::withCString("nvgsp-gr-busy");
    tempSym_ = OSSymbol::withCString("nvgsp-gpu-temp");   // 0.5.5
    sampleCall_ = thread_call_allocate(&NVAccelerator::sampleCallout, this);
    statCall_ = thread_call_allocate(&NVAccelerator::statCallout, this);
    UInt64 deadline = 0;
    if (sampleCall_) {
        clock_interval_to_deadline(10, kMillisecondScale, &deadline);
        thread_call_enter_delayed(sampleCall_, deadline);
    }
    if (statCall_) {
        clock_get_uptime(&statLastAbs_);
        clock_interval_to_deadline(1, kSecondScale, &deadline);
        thread_call_enter_delayed(statCall_, deadline);
    }
}

void NVAccelerator::sampleCallout(thread_call_param_t p, thread_call_param_t) {
    auto *self = static_cast<NVAccelerator *>(p);
    if (self->pollStop_) return;
    UInt32 st = 0;
    if (self->gsp_ && self->grBusySym_ &&
        self->gsp_->callPlatformFunction(self->grBusySym_, false, &st, nullptr, nullptr, nullptr) == kIOReturnSuccess) {
        OSIncrementAtomic(&self->samples_);
        if (st & 1) OSIncrementAtomic(&self->busySamples_);
    }
    UInt64 deadline = 0;
    clock_interval_to_deadline(10, kMillisecondScale, &deadline);
    thread_call_enter_delayed(self->sampleCall_, deadline);
}

// every second: utilization over the last interval, then republish
void NVAccelerator::statCallout(thread_call_param_t p, thread_call_param_t) {
    auto *self = static_cast<NVAccelerator *>(p);
    if (self->pollStop_) return;
    UInt64 now = 0;
    clock_get_uptime(&now);
    IOLockLock(self->statLock_);
    UInt64 busy = self->busyAccumAbs_ + (self->busyStartAbs_ ? now - self->busyStartAbs_ : 0);
    const UInt64 d = busy - self->busyLastAccumAbs_, span = now - self->statLastAbs_;
    self->busyLastAccumAbs_ = busy; self->statLastAbs_ = now;
    IOLockUnlock(self->statLock_);
    UInt32 pct = span ? static_cast<UInt32>(d * 100 / span) : 0;
    const UInt32 n = OSBitAndAtomic(0, &self->samples_), b = OSBitAndAtomic(0, &self->busySamples_);
    const UInt32 sampled = n ? b * 100 / n : 0;
    if (sampled > pct) pct = sampled;
    self->utilPct_ = pct > 100 ? 100 : pct;
    if (self->gsp_ && self->tempSym_) {
        SInt32 mC = 0;
        if (self->gsp_->callPlatformFunction(self->tempSym_, false, &mC, nullptr, nullptr, nullptr) == kIOReturnSuccess && mC > 0)
            self->tempC_ = static_cast<UInt32>(mC / 1000);
    }
    OSDictionary *cur = OSDynamicCast(OSDictionary, self->copyProperty("PerformanceStatistics"));
    if (!cur) cur = OSDictionary::withCapacity(4);   // the family never published one: start ours
    if (cur) {
        self->setProperty(OSSymbol::withCStringNoCopy("PerformanceStatistics"), cur);   // merges ours in
        cur->release();
    }
    UInt64 deadline = 0;
    clock_interval_to_deadline(1, kSecondScale, &deadline);
    thread_call_enter_delayed(self->statCall_, deadline);
}

// the family republishes PerformanceStatistics from its own counters; add
// the utilization keys (what AMD/Intel drivers publish) on every write
OSDictionary *NVAccelerator::withUtilization(OSDictionary *family) {
    OSDictionary *d = OSDictionary::withDictionary(family, family->getCount() + 4);
    if (!d) return nullptr;
    OSNumber *n = OSNumber::withNumber(utilPct_, 32);
    if (n) {
        d->setObject("Device Utilization %", n);
        d->setObject("Renderer Utilization %", n);
        d->setObject("Tiler Utilization %", n);
        n->release();
    }
    // 0.5.5: memory and temperature under the keys AMD's drivers use (what
    // iStat Menus and other monitors read); VRAM from NVGspControl's books
    auto put = [d](const char *k, UInt64 v) {
        if (OSNumber *x = OSNumber::withNumber(v, 64)) { d->setObject(k, x); x->release(); }
    };
    if (gsp_) {
        // 0.5.6: copyProperty, not getProperty: NVGspControl replaces these
        // numbers all the time (on its own threads), and a borrowed pointer
        // was freed under us (kernel panic in this timer, 30 Sep 13:53, after
        // a burst of GPU resets)
        OSObject *uo = gsp_->copyProperty("NVGspControl-mem-vram-bytes");
        OSObject *so = gsp_->copyProperty("NVGspControl-mem-sys-bytes");
        IORegistryEntry *pci = gsp_->copyParentEntry(gIOServicePlane);
        OSObject *mo = pci ? pci->copyProperty("VRAM,totalMB") : nullptr;
        OSNumber *used = OSDynamicCast(OSNumber, uo), *sys = OSDynamicCast(OSNumber, so), *mb = OSDynamicCast(OSNumber, mo);
        if (used) put("vramUsedBytes", used->unsigned64BitValue());
        if (used && mb && (mb->unsigned64BitValue() << 20) > used->unsigned64BitValue())
            put("vramFreeBytes", (mb->unsigned64BitValue() << 20) - used->unsigned64BitValue());
        if (sys) put("In use system memory", sys->unsigned64BitValue());
        OSSafeReleaseNULL(uo); OSSafeReleaseNULL(so); OSSafeReleaseNULL(mo); OSSafeReleaseNULL(pci);
    }
    if (tempC_) put("Temperature(C)", tempC_);
    return d;
}

bool NVAccelerator::setProperty(const OSSymbol *key, OSObject *value) {
    if (key && statLock_ && key->isEqualTo("PerformanceStatistics"))
        if (OSDictionary *fam = OSDynamicCast(OSDictionary, value))
            if (OSDictionary *d = withUtilization(fam)) {
                const bool r = IOGraphicsAccelerator2::setProperty(key, d);
                d->release();
                return r;
            }
    return IOGraphicsAccelerator2::setProperty(key, value);
}

void NVAccelerator::kickStampPoll() {
    if (!pollCall_ || pollStop_) return;
    pollIdle_ = 0;
    if (OSCompareAndSwap(0, 1, &pollArmed_)) {
        UInt64 deadline = 0;
        clock_interval_to_deadline(20, kMicrosecondScale, &deadline);
        thread_call_enter_delayed(pollCall_, deadline);
    }
}

// Separate from submitLock_: submitCommands may hold that lock while waiting
// for a reused FIFO event, which cancellation must be able to retire.
void NVAccelerator::beginNativeStamp(UInt32 index) {
    if (index >= 2 || !stampCancelLock_) return;
    IOLockLock(stampCancelLock_);
    stampSubmitting_[index] = true;  // exclude CPU stamp writes while GSP may kick newer GPU work
    IOLockUnlock(stampCancelLock_);
}

void NVAccelerator::recordNativeStamp(UInt32 index, UInt32 value, bool accepted) {
    if (index >= 2 || !stampCancelLock_) return;
    IOLockLock(stampCancelLock_);
    stampSubmitting_[index] = false;
    if (stampSignaled_[index] == stampSubmitted_[index]) clock_get_uptime(&stampProgressAbs_);
    if (static_cast<SInt32>(value - stampSubmitted_[index]) > 0) stampSubmitted_[index] = value;
    if (accepted) {
        stampAccepted_[index] = value;
    } else {
        stampCanceled_[index] = value;
        stampCancelAfter_[index] = stampAccepted_[index];
        stampCancelPending_[index] = true;
    }
    IOLockUnlock(stampCancelLock_);
}

void NVAccelerator::retireCanceledStamps() {
    if (!stampMap_ || !stampCancelLock_) return;
    IOLockLock(stampCancelLock_);
    volatile UInt32 *base = reinterpret_cast<volatile UInt32 *>(stampMap_->getVirtualAddress());
    for (UInt32 i = 0; i < 2; ++i) {
        if (!stampCancelPending_[i] || stampSubmitting_[i]) continue;
        const UInt32 done = base[i], canceled = stampCanceled_[i];
        if (static_cast<SInt32>(done - canceled) >= 0) {
            stampCancelPending_[i] = false;  // a later GPU fence already retired it
        } else if (static_cast<SInt32>(stampAccepted_[i] - canceled) <= 0 &&
                   static_cast<SInt32>(done - stampCancelAfter_[i]) >= 0) {
            // A newer accepted GPU stamp may land asynchronously; leave it to
            // retire the cancellation rather than race it with a CPU write.
            // Otherwise all earlier accepted work is complete and beginNativeStamp
            // excludes a new kick. Keep the family's submission error.
            base[i] = canceled;
            stampCancelPending_[i] = false;
            ++canceledCompletions_;
        }
    }
    IOLockUnlock(stampCancelLock_);
}

// 20 us steps while work is out; then a few 1 ms looks, then stop
void NVAccelerator::pollCallout(thread_call_param_t p, thread_call_param_t) {
    auto *self = static_cast<NVAccelerator *>(p);
    if (self->pollStop_) { self->pollArmed_ = 0; return; }
    // Private channels use CPU-ordered stamps. Do not depend on the sparse
    // GR interrupt or the slow status daemon to advance that queue.
    if (self->gsp_) self->gsp_->callPlatformFunction("nvgsp-stamp-poll", false,
                                                  nullptr, nullptr, nullptr, nullptr);
    self->retireCanceledStamps();
    self->checkStamps();
    bool out = false;
    for (UInt32 i = 0; i < 2; ++i) out |= self->stampSignaled_[i] != self->stampSubmitted_[i];
    if (out) self->pollIdle_ = 0;
    // Delay alone does not mean work is lost: at 40 clients the old watchdog
    // signaled success before GPU writes finished. Only retire lost stamps
    // when the driver explicitly reports reset/sleep or GR unavailable.
    if (out && self->stampMap_) {
        UInt64 now = 0, ns = 0;
        clock_get_uptime(&now);
        if (!self->stampProgressAbs_) self->stampProgressAbs_ = now;
        absolutetime_to_nanoseconds(now - self->stampProgressAbs_, &ns);
        if (ns > 3000000000ULL) {
            auto propertyIs = [self](const char *key, bool value) {
                OSObject *o = self->gsp_ ? self->gsp_->copyProperty(key) : nullptr;
                OSBoolean *b = OSDynamicCast(OSBoolean, o);
                const bool match = b && b->isTrue() == value;
                OSSafeReleaseNULL(o);
                return match;
            };
            const bool unavailable = propertyIs("NVGspControl-reset-busy", true) ||
                propertyIs("NVGspControl-sleeping", true) ||
                propertyIs("NVGspControl-gr-persistent", false);
            if (unavailable) {
                volatile UInt32 *base = reinterpret_cast<volatile UInt32 *>(self->stampMap_->getVirtualAddress());
                IOAccelEventMachine2 *em = self->eventMachine();
                for (UInt32 i = 0; i < 2; ++i) {
                    UInt32 value = self->stampSubmitted_[i];
                    if (em) {
                        const UInt32 assigned = static_cast<UInt32>(reinterpret_cast<uintptr_t>(em->getStamp(i)));
                        if (static_cast<SInt32>(assigned - value) > 0) value = assigned;
                    }
                    self->stampSubmitted_[i] = value;
                    base[i] = value;
                }
                ++self->forcedCompletions_;
                NVALOG("native: GPU unavailable, retiring lost stamps (%u)",
                       self->forcedCompletions_);
                self->checkStamps();
            } else {
                NVALOG("native: stamps delayed 3 s while GPU available; waiting for GPU fences");
            }
            self->stampProgressAbs_ = now;
        }
    }
    if (out || self->pollIdle_++ < 4) {
        UInt64 deadline = 0;
        clock_interval_to_deadline(out ? 20 : 1000, kMicrosecondScale, &deadline);
        thread_call_enter_delayed(self->pollCall_, deadline);
        return;
    }
    self->pollArmed_ = 0;
    for (UInt32 i = 0; i < 2; ++i)   // a submit may have raced the disarm
        if (self->stampSignaled_[i] != self->stampSubmitted_[i]) { self->kickStampPoll(); break; }
}

// 0.4.7: every client with its tag; entries go with clientClose/free
void NVAccelerator::noteTaskClient(void *task, IOUserClient *inner, UInt32 tag, bool add) {
    if (!taskLock_ || !task || !inner) return;
    IOLockLock(taskLock_);
    for (auto &t : tasks_) if (t.inner == inner) { t.task = nullptr; t.inner = nullptr; t.tag = 0; }
    if (add)
        for (auto &t : tasks_) if (!t.task) { t.task = task; t.inner = inner; t.tag = tag; break; }
    IOLockUnlock(taskLock_);
}

// the submitting process's own client with that tag, nothing else
IOUserClient *NVAccelerator::clientForTag(void *task, UInt32 tag) {
    if (!taskLock_ || !task || !tag) return nullptr;
    IOUserClient *r = nullptr;
    IOLockLock(taskLock_);
    for (auto &t : tasks_) if (t.task == task && t.tag == tag) {
        r = t.inner;
        if (r) r->retain();   // 0.5.15: clientClose can remove the entry after taskLock_ drops
        break;
    }
    IOLockUnlock(taskLock_);
    return r;
}

// 0.4.1: as AppleParavirtAccelerator::setupFIFO maps its stamp memory
volatile UInt32 *NVAccelerator::stampBase() {
    if (!stampMap_ && stamp_) stampMap_ = stamp_->createMappingInTask(kernel_task, 0, kIOMapAnywhere);
    return stampMap_ ? reinterpret_cast<volatile UInt32 *>(stampMap_->getVirtualAddress()) : nullptr;
}

bool NVAccelEventMachine::init(IOGraphicsAccelerator2 *a, unsigned int n, int t) {
    if (!IOAccelEventMachineFast2::init(a, n, t)) return false;
    auto *nv = OSDynamicCast(NVAccelerator, reinterpret_cast<OSObject *>(a));
    volatile UInt32 *base = nv ? nv->stampBase() : nullptr;
    if (!base) { NVALOG("event machine: no stamp mapping"); return false; }
    setStampBaseAddress(base);
    NVALOG("event machine: %u stamps at %p", n, base);
    return true;
}
