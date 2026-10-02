#pragma once

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMultiMemoryDescriptor.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <kern/thread_call.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/pci/IOPCIDevice.h>
#include "../NVGspCore/NVGspArenaMap.hpp"
#include "../NVGspCore/NVGspRegistry.hpp"
#include "../NVGspCore/NVGspWindowSurface.hpp"
#include "../NVGspCore/NVGspBooterStaging.hpp"
#include "../NVGspCore/NVGspBooterFalcon.hpp"
#include "../NVGspCore/NVGspChainWait.hpp"
#include "../NVGspCore/NVGspChannel.hpp"
#include "../NVGspCore/NVGspVideo.hpp"
#include "../NVGspCore/NVGspVramHeap.hpp"
#include "../NVGspCore/NVGspEvict.hpp"
#include "../NVGspCore/NVGspFalcon.hpp"
#include "../NVGspCore/NVGspFwsecStaging.hpp"
#include "../NVGspCore/NVGspInit.hpp"
#include "../NVGspCore/NVGspPackage.hpp"
#include "../NVGspCore/NVGspStaging.hpp"
#include "../NVGspCore/NVGspVbios.hpp"

constexpr size_t kNVGspPackageBytes = 63651840;

struct NVGspFlipCopy;   // NVGspCore/NVGspKernelApi.hpp
class NVGspControl : public IOService {
    OSDeclareDefaultStructors(NVGspControl)
public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    IOReturn callPlatformFunction(const OSSymbol *name, bool wait, void *p1,
                                  void *p2, void *p3, void *p4) override;
    IOReturn stagePackage(const void *bytes, size_t length);
    IOReturn executeBoot();
    IOReturn pollStatus();
    IOReturn pollStatusLocked();
    // 0.51.0: experiment flags from userspace (nvgsp_load --flags):
    // bit0 = GPU test draw on scanout, bit1 = display core-channel probe.
    IOReturn setExperimentFlags(UInt32 flags);
    // 0.56.0: live window-0 flip (offset in bytes into VRAM, 256 B aligned)
    // on the persistent window channel; serialized with pollStatus.
    IOReturn flipWindow(UInt64 offsetBytes, bool fullInit = false);
    // 0.57.0: submit a GR method stream on the persistent channel; a host
    // WFI semaphore is appended, the call waits for it and returns the
    // GPU execution time in ns.
    IOReturn submitGr(const void *owner, const UInt32 *words, UInt32 count, UInt64 *nsOut);
    // 0.103.0: asynchronous GR submission + fence wait (Metal-style queues).
    IOReturn submitGrAsync(const void *owner, const UInt32 *words, UInt32 count,
                           UInt32 *seqOut);
    IOReturn waitGrFence(UInt32 seq, UInt32 timeoutUs, UInt32 *completedOut,
                         const void *owner = nullptr);
    IOReturn waitFence(UInt32 engine, UInt32 seq, UInt32 timeoutUs,
                       UInt32 *completedOut,
                       const void *owner = nullptr);   // 0.112.0: 0 GR, 1 CE; 0.116.0: 2+ video
    // 0.178.0: own GR channel + VAS for `owner` (opt-in, selector 40). Both
    // run without lock_ (RPCs). chidOut = physical channel id.
    IOReturn clientChannelOpen(const void *owner, UInt32 *chidOut);
    void clientChannelClose(const void *owner);
    // 0.178.6: NVRAM nvgsp-ownchannel=1: every client gets one on its first
    // memory allocation or submission (before any of its GR work)
    bool ownChannelAuto();
    bool privateChannelGenerationAllowed() const;
    // 0.116.0 (V1): bring up video engine channel `index` (0 NVDEC0,
    // 1 NVENC0, 2 OFA0) on first use; its work goes through submitSegments /
    // waitFence with engine 2 + index. stage/status = last setup step + RM status.
    IOReturn videoOpen(UInt32 index, UInt32 *stageOut, UInt32 *statusOut);
    IOReturn gpuReset();                 // 0.104.0 (B3)
    IOReturn diagnosticChannelKick(UInt32 handle); // 0.177.2: reserved probe only
    // 0.110.0: bumped by every gpuReset(); user clients opened before a
    // reset see their GPU state (fences, VAS, channel) as lost.
    // 0.164.0: + a second step when the reset is over (see resumeDriveCallout)
    UInt32 resetGeneration() const { return gpuResets_ + genEnd_; }
    // 0.164.0: a reset is running (from gpuReset until its resume, retries
    // included, is over): clients must not allocate, bind or submit
    bool resetBusy() const { return resetBusy_; }
    // 0.105.0: shared kernel interface (NVK nvkmd / NVAccelerator): TLB
    // invalidate + VA bind in the user VA arena PD1[320..383]
    // (GPU VA 0x28_0000_0000 .. 0x30_0000_0000), 2 MiB pages.
    IOReturn tlbInvalidate();
    IOReturn vaBind(const void *owner, UInt64 va, UInt64 phys, UInt64 bytes, UInt32 flags);
    IOReturn submitSegments(const void *owner, UInt32 engine, const UInt64 *segVa,
                            const UInt32 *segDwords, const UInt32 *segFlags,
                            UInt32 n, UInt32 *seqOut,
                            UInt64 stampVa = 0, UInt32 stampValue = 0);   // 0.106.0, 0.152.0 stamp
    // 0.107.0: GPU memory objects (VRAM first-fit allocator / sysmem 2 MiB
    // chunks), owned by a user client and freed when it closes.
    IOReturn memAlloc(const void *owner, UInt64 bytes, UInt32 domain,
                      UInt32 *handleOut, UInt64 *physOut);
    IOReturn memFree(const void *owner, UInt32 handle);
    // 0.177.0: a reopened client takes over its own process's objects from the client the
    // reset left behind (keeps the VRAM contents gpuReset saved and restored)
    IOReturn memAdopt(const void *owner, task_t task, UInt32 handle, UInt64 *physOut);
    void clientLive(const void *owner, task_t task, bool live);
    void memFreeAll(const void *owner);
    IOReturn vaBindObject(const void *owner, UInt32 handle, UInt64 va, UInt32 flags,
                          UInt64 memOffset = 0, UInt64 range = 0);
    IOMemoryDescriptor *memUserDescriptor(const void *owner, UInt32 handle,
                                          bool *isVram = nullptr);
    // 0.109.0: unbind [va, va + bytes) (pages of this owner's objects or raw
    // binds only) and heap/usage info {VRAM heap bytes, VRAM used, SYS used}.
    IOReturn vaUnbind(const void *owner, UInt64 va, UInt64 bytes);
    IOReturn memInfo(UInt64 *heapBytesOut, UInt64 *vramUsedOut, UInt64 *sysUsedOut);
    // 0.110.0: zero-copy present of a VRAM object on window 0 / hand the
    // screen back to the desktop surface.
    IOReturn presentObject(const void *owner, UInt32 handle, UInt64 offset, UInt32 pitch,
                           UInt32 width, UInt32 height, UInt32 format);
    IOReturn presentStop(const void *owner);
    // 0.63.0: same for the copy-engine channel.
    IOReturn submitCe(const void *owner, const UInt32 *words, UInt32 count, UInt64 *nsOut);
    // 0.71.0: GSP round-trip probe — sends a P-state read RPC and waits
    // for its reply; returns latency and whether the MSI path drained it.
    IOReturn pingGsp(UInt64 *nsOut, UInt64 *viaIntrOut);
    // 0.80.0: live debug surface for display bring-up without reboots.
    IOReturn peekBar0(UInt32 offset, UInt32 count, UInt32 *out);
    IOReturn pokeBar0(UInt32 offset, UInt32 value);
    IOReturn vramAccess(UInt64 offset, UInt32 *words, UInt32 count, bool write);
    IOReturn submitCore(const UInt32 *words, UInt32 count);
    // 0.138.0 (D1): core UPDATE hygiene. coreUnstick() releases a core that
    // waits on a stale interlock (SET_ACCL IGNORE_INTERLOCK, interlocks 0,
    // accel back to none); coreException() acks a pending FE exception on
    // the core and returns its stat word (0 when none was pending).
    IOReturn coreUnstick();
    UInt32 coreException();
    IOReturn waitCursorArmed(UInt32 usage, const UInt32 state[7]);
    IOReturn cursorTest(UInt32 op);
    // 0.145.0: true when the last GR submission came from a client other
    // than `owner` (class state such as tex/sampler pools may be theirs).
    bool grLastOwnerIsOther(const void *owner);
    IOReturn readGspLog(UInt32 index, UInt32 offset, UInt8 *out, UInt32 bytes);
    IOReturn cursorSetup();
    IOReturn olutSetup();
    IOReturn ilutSetup();
    bool wndComposite_ = false;
    UInt32 installVramWindow();
    UInt32 installSharedWindow();
    IOMemoryDescriptor *sharedWindowDescriptor() const { return shmUser_; }
    IOReturn cursorImage(const UInt32 *argb64x64, UInt32 hotX, UInt32 hotY);
    IOReturn cursorShow(bool visible);
    IOReturn cursorMove(SInt32 x, SInt32 y);
    IOReturn dpSetPower(bool on);
    // 0.94.0: native DPCD read over DP AUX (cmd 0x9), <= 16 bytes.
    IOReturn dpAuxRead(UInt32 addr, UInt8 *out, UInt32 bytes);
    // 0.93.0: driver-driven DP modeset on head 0 — the modeset3.py sequence
    // (snapshot, manual-DP, detach, link release, ASSIGN_SOR, D0, train,
    // CONFIG_STREAM, attach) run in-kernel. *statusOut is 0 on a full pass,
    // else 0xSS000000 | step status with SS = failing step number.
    IOReturn modesetHead0(UInt32 *statusOut, bool lightIfLit = false);
    // 0.137.0 (D4): switch head 0 to an EDID timing. t = {pixel clock kHz,
    // hActive, hBlank, hSyncOffset, hSyncWidth, vActive, vBlank, vSyncOffset,
    // vSyncWidth, flags (bit0 hsync+, bit1 vsync+, bit2 64 px cursor)}. The timing sticks: resume
    // and hotplug modesets re-apply it. statusOut = modesetHead0's code.
    IOReturn setMode(const UInt32 t[10], UInt32 *statusOut);
    // 0.134.0: wire [uaddr, uaddr + bytes) of `task` and map its 4 KiB pages at
    // va in the caller's arena (small page tables). Freed with memFree.
    IOReturn userMemBind(const void *owner, task_t task, UInt64 uaddr, UInt64 bytes, UInt64 va,
                         UInt32 flags, UInt32 *handleOut);
    IOReturn armHotplug();
    void deliverHotplug();
    IOReturn dpReadEdid();
    typedef void (*HotplugFn)(void *ref, UInt32 plugMask, UInt32 unplugMask);
    HotplugFn hotplugFn_ = nullptr;
    void *hotplugRef_ = nullptr;
    bool hotplugArmed_ = false, hotplugDeliver_ = false;
    UInt32 postEvents_ = 0, hotplugs_ = 0, hotplugPlug_ = 0, hotplugUnplug_ = 0;
    IOReturn userRpc(UInt32 function, const UInt8 *params, UInt32 bytes,
                     UInt8 *reply, UInt32 *replyBytes, UInt32 *rpcResult);
    // 0.96.0: S3 sleep/wake. Quiesce on WillSleep (sink off, phase-0 fence,
    // MSI teardown), full chain-state reset for a daemon-driven re-boot
    // after wake; sleeping_ gates the daemon's resume trigger.
    static IOReturn sleepWakeHandler(void *target, void *refCon, UInt32 messageType,
                                     IOService *provider, void *messageArgument,
                                     vm_size_t argSize);
    void quiesceForSleep();
    void resumeFromSleep();
    // 0.100.0: NVIDIA-style GSP suspend/resume (nouveau r535_gsp_fini/
    // tu102_gsp_fini/init + r570 fbsr). srSuspend unloads GSP-RM with
    // bInPMTransition and tears WPR2 down via Booter Unload into srData_;
    // srResume boots GSP-RM from the SR metadata (RM restores its state).
    IOReturn srSuspend();
    IOReturn srResume();
    IOReturn srCycle();   // debug: suspend + resume without system sleep
    bool srStageBuffers();
    IOReturn executeUnload();
    // 0.144.0: wait (lock_ held, dropped while sleeping) until no other RPC
    // owns the reply slot; false when phase/sleep rule it out or it stays busy.
    bool waitRpcSlotLocked(UInt32 ms);
    // 0.146.0: lock_ hold profiling. The boot logo animation froze for a
    // while and the interrupt handler once waited 1.16 s for lock_: record
    // the longest hold, the source line that took the lock, when (uptime)
    // and at which post-init phase.
    void lk(UInt32 line) {
        IOLockLock(lock_);
        clock_get_uptime(&lockAt_);
        lockLine_ = line;
    }
    void ulk();
    // 0.146.5: boot timeline (uptime ms per step) for the logo-animation freeze
    void markBoot(const char *tag);
    bool zeroVram(UInt64 phys, UInt64 bytes);   // 0.147.3: BAR1 memset, lock_ not held
    UInt32 vramZeroMisses_ = 0, vramZeroed_ = 0;
    // 0.146.6: BAR1 remap right after GSP INIT_DONE (the kernel console,
    // i.e. the boot logo animation, draws through BAR1 and was frozen from
    // GSP boot until the end of the post-init chain, ~5 s); redone at the end.
    bool bar1EarlyPending_ = false, bar1Early_ = false;
    char bootTl_[640] = {};
    UInt32 bootTlLen_ = 0;
    int sleepLk(void *event, UInt64 deadline, UInt32 interType);   // IOLockSleep(Deadline) with the hold split
    UInt64 lockAt_ = 0, lockHoldMaxNs_ = 0, lockHoldMaxUptimeMs_ = 0;
    UInt32 lockLine_ = 0, lockHoldMaxLine_ = 0, lockHoldMaxPhase_ = 0, lockHoldsOver10ms_ = 0;
    struct LongHold { UInt32 line, count; UInt64 maxNs; } longHolds_[16] = {};   // holds > 5 ms by line
    IOReturn userRpcLarge(UInt32 function, const UInt8 *payload, UInt32 bytes,
                          UInt32 *rpcResult);
    void resetForResume();
    static void resumeDriveCallout(thread_call_param_t self, thread_call_param_t);
    static void autoResetCallout(thread_call_param_t self, thread_call_param_t);   // 0.115.0
    static void rebarOkCallout(thread_call_param_t self, thread_call_param_t);     // 0.131.0
    void scheduleAutoResetLocked();

private:
    // 0.99.2: system info used by the cold-boot init staging, kept so the S3
    // re-boot can re-stage init_ exactly like a cold boot.
    nvgsp::GspSystemInfoParameters systemInfo_{};
    bool systemInfoValid_ = false;
    // 0.99.4: last VBIOS/driver-lit head 0 state (ARMED head methods) — the
    // S3 re-boot finds the display engine at reset defaults.
    // 0.100.4: boot-chain profiling ring (per daemon poll).
    struct PollProf { UInt16 phaseIn, phaseOut; UInt16 records, idle; UInt32 lockUs, firstUs, drainUs, totalUs; };
    PollProf pollProf_[160] = {};
    UInt32 pollProfCount_ = 0;
    uint64_t profT0_ = 0, profFirst_ = 0, profDrainEnd_ = 0;
    UInt32 profRecords_ = 0, profIdle_ = 0;
    bool drainNoWait_ = false;   // 0.100.6: last chain step queued no RPC
    IOMemoryMap *doorbellMap_ = nullptr;   // 0.100.5: persistent BAR0 map for 0x110c00
    static void gspDoorbell(void *ctx);
    UInt32 *headCache_ = nullptr;
    bool headCacheValid_ = false;
    // 0.100.2: head-0 VPLL (0x00ef00..0x00ef1c) as programmed by the VBIOS/
    // GOP; after S3 the PLL is at reset and GSP-RM does not re-program it.
    UInt32 vpllCache_[8] = {};
    bool vpllCacheValid_ = false;
    UInt32 experimentFlags_ = 0;
    // 0.70.0: MSI interrupt path (experiment bit3). The GSP stall vector
    // from INTR_GET_KERNEL_TABLE is enabled in the VF CPU interrupt tree;
    // each MSI clears the leaf + GSP falcon SWGEN0, drains the status
    // queue (pollStatusLocked) and re-arms the top level.
    static constexpr UInt32 kVecGsp = 0, kVecDisp = 1, kVecGrNs = 2, kVecCeNs = 3,
                           kVecCount = 4;   // 0.119.0: + CE0 non-stall
    // 0.93.0: one NV04_DISPLAY_COMMON control (function 76). Returns the RM
    // status (reply+92), ~0U on transport failure; optionally copies the
    // echoed params (reply+104) for out-fields like DP_CTRL err.
    UInt32 impCheckHead0();
    UInt32 displayCtrl(UInt32 cmd, const UInt8 *params, UInt32 paramBytes,
                       UInt8 *echoOut = nullptr, UInt32 echoBytes = 0);
    bool armInterrupts(const UInt32 *vectors);
    void disarmVector(UInt32 k);
    void disarmInterrupts(const char *why);
    UInt32 vec_[kVecCount] = {~0U, ~0U, ~0U, ~0U};
    UInt32 vecCount_[kVecCount] = {};
    UInt32 vblanks_ = 0, dispOther_ = 0, lastDispOther_ = 0, vblankEnLost_ = 0;
    UInt64 lastVblank_ = 0;
    UInt64 vblankMinNs_ = 0, vblankMaxNs_ = 0, vblankSumNs_ = 0;
    UInt32 fenceSleeps_ = 0, fenceWakes_ = 0;
    UInt32 coreInitWords_ = 2;
    UInt32 corePut_ = 0;
    UInt32 dpmsCalls_ = 0;
    UInt64 stallStartAbs_ = 0;
    bool cursorReady_ = false, cursorVisible_ = true;
    UInt64 cursorBase_ = 0;
    UInt32 cursorBuf_ = 0, cursorControl_ = 0;
    UInt32 vblankHist_[5] = {};
    bool vblankDeliver_ = false;
    UInt64 vblankDeliverAt_ = 0, vblankCbMaxNs_ = 0;
    UInt64 intrLockWaitMaxNs_ = 0, intrServiceMaxNs_ = 0;
    // 0.77.0: DP AUX (DPCD + EDID) state.
    UInt32 auxStep_ = 0, auxRetry_ = 0, auxLastStatus_ = 0, auxLastReply_ = 0;
    bool auxOk_ = false;
    UInt8 dpcd_[48] = {};
    UInt8 edid_[256] = {};
    UInt32 edidBytes_ = 0;
    typedef void (*VblankFn)(void *ref, UInt32 count, UInt64 uptimeAbs);
    // 0.152.0 (native N1): IOAccel stamps. GPU writes them in the VRAM stamp
    // region; every GR/CE non-stall interrupt calls stampFn_(ref, mask)
    // (bit 0 GR, bit 1 CE) after lock_ is dropped, like the vblank clients.
    typedef void (*StampFn)(void *ref, UInt32 engineMask);
    StampFn stampFn_ = nullptr;
    void *stampRef_ = nullptr;
    UInt32 stampDeliverMask_ = 0;
    bool stampZeroed_ = false;
    static constexpr UInt64 kStampCtxOff = 0x29F0000;   // ctx block spare, 60 KiB
    static constexpr UInt64 kStampBytes = 0xF000;
    static constexpr UInt32 kMaxVblankClients = 4;
    VblankFn vblankFn_[kMaxVblankClients] = {};
    void *vblankRef_[kMaxVblankClients] = {};
    static void onInterrupt(OSObject *owner, IOInterruptEventSource *, int);
    void serviceInterrupt();
    IOWorkLoop *wl_ = nullptr;
    IOInterruptEventSource *irq_ = nullptr;
    IOMemoryMap *intrBar0_ = nullptr;
    bool intrArmed_ = false;
    // 0.96.0: S3 sleep/wake interest + quiesce flag.
    IONotifier *sleepWakeNotifier_ = nullptr;
    bool sleeping_ = false;
    // 0.99.0: slow-path chain self-drive (async; no daemon dependency).
    thread_call_t resumeCall_ = nullptr;
    thread_call_t autoResetCall_ = nullptr;   // 0.115.0
    thread_call_t rebarOkCall_ = nullptr;     // 0.131.0: clears the ReBAR boot marker
    // 0.135.0: activity boost. Submissions re-arm a short BOOST_TO_MAX so the
    // GPU leaves P8 (PCIe Gen1, 405 MHz memory) while work runs and falls back
    // on its own once idle. CE-only work did not wake it (sysmem 3.2 GB/s).
    thread_call_t boostCall_ = nullptr;
    volatile UInt64 lastBoostAbs_ = 0;
    UInt32 boostSent_ = 0;
    UInt32 nvramBoostPinned_ = 0;             // nvram nvgsp-boost=1: old pinned max
public:
    void noteGpuBusy();
private:
    static void boostCallout(thread_call_param_t self, thread_call_param_t);
    bool rebarOkArmed_ = false;
    UInt64 lastAutoResetNs_ = 0;
    UInt32 autoResets_ = 0, autoResetsSkipped_ = 0, autoResetStreak_ = 0;
    // 0.145.0: the resume in flight came from gpuReset (not S3), and how
    // many times in a row it came back without a GR channel.
    bool resetDriven_ = false;
    UInt32 resetRetries_ = 0;
    // 0.145.0: owner of the last GR submission (nullptr on a fresh channel)
    const void *lastGrOwner_ = nullptr;
    UInt32 grOwnerSwitches_ = 0;
    void noteGrOwnerLocked(const void *owner);
    volatile bool resumeActive_ = false, resumeAbort_ = false;
    // 0.99.0: post-stage queue snapshot (516 KiB) for the S3 re-boot.
    UInt8 *queueSnap_ = nullptr;
    UInt32 intrCount_ = 0, intrSpurious_ = 0, intrStuck_ = 0;
    bool inIntr_ = false;
    volatile bool pingOutstanding_ = false;
    bool sysCachedUsed_ = false;              // 0.163.0: a GPU-cacheable sysmem bind exists
    bool pingViaIntr_ = false;
    UInt64 pingSentAt_ = 0, pingDoneAt_ = 0;
    UInt32 pings_ = 0;
    volatile bool userRpcOutstanding_ = false;
    bool userRpcActive_ = false;   // reply belongs to its caller until copied out
    UInt32 userRpcFunction_ = 0, userRpcReplyBytes_ = 0, userRpcResult_ = 0, userRpcs_ = 0;
    UInt8 userRpcReply_[4096] = {};
    UInt64 dispInstOffset_ = 0;
    // 0.50.0: BAR1 physical-mode bind + evidence; runs once per boot on
    // every ctx-path exit (success, refusal, watchdog).
    void finishBar1();
    bool bar1Finished_ = false;
    // 0.50.0: GPU test draw on the scanout only with boot-arg nvgspdraw=1.
    bool drawTest_ = false;
    IOPCIDevice *pci_ = nullptr;
    nvgsp::GspStaging gsp_;
    nvgsp::BooterStaging booter_;
    nvgsp::GspInitStaging init_;
    // 0.100.0: SR state.
    nvgsp::BooterStaging booterUnload_;
    nvgsp::DmaBuffer srData_, srRadix_, srMeta_, fbsrBuf_;
    UInt32 fuseVersion_ = 0;
    UInt64 bootMailboxOverride_ = 0;   // 0 = WPR metadata (cold boot)
    UInt8 *dispSave_ = nullptr;        // display RAMIN copy across SR
    bool srBuffersReady_ = false, srSuspended_ = false, largeReplyOk_ = false;
    UInt32 srStep_ = 0;
    bool staged_ = false;
    bool executed_ = false;
    UInt32 statusSequence_ = 0;
    bool initDone_ = false;
    UInt32 initResult_ = ~0U;
    UInt32 initPrivateResult_ = ~0U;
    UInt32 postInitPhase_ = 0;
    UInt32 internalClient_ = 0;
    UInt32 internalDevice_ = 0;
    UInt32 internalSubdevice_ = 0;
    UInt64 virtualOffset_ = 0;
    UInt64 localMemoryOffset_ = 0;
    UInt64 gpfifoBackingOffset_ = 0;
    UInt64 userdBackingOffset_ = 0;
    UInt64 instanceBackingOffset_ = 0;
    UInt64 methodBackingOffset_ = 0;
    UInt64 dispPbBackingOffset_ = 0;
    UInt64 gpfifoBackingSize_ = 0;
    UInt64 userdBackingSize_ = 0;
    UInt64 instanceBackingSize_ = 0;
    UInt64 methodBackingSize_ = 0;
    UInt32 methodBufferBytes_ = 0;
    UInt64 errBackingOffset_ = 0;
    UInt64 errBackingSize_ = 0;
    UInt64 channelGpFifoVa_ = 0;
    UInt32 channelCid_ = 0;
    // 0.7.2: RM-assigned TSG handle decoded live from the channel-alloc
    // echo (offset 240). Never hardcoded: 0 means unknown, skip TSG path.
    UInt32 channelTsgHandle_ = 0;
    // 0.7.3: hardware TSG ID from GET_INFO (diagnostic, output-only).
    UInt32 tsgHwId_ = 0;
    // 0.7.0: bounds the wait for the first unproven function-76 control
    // since the phase-39 stall (CE fault-method-buffer size, phase 49).
    UInt32 ceSizeStallPolls_ = 0;
    // 0.35.0: NOP-kick state. Token from GPFIFO_GET_WORK_SUBMIT_TOKEN
    // (phase 118); doorbell = BAR0 0xBB0090 (NV_VIRTUAL_FUNCTION
    // 0x30000 + FULL_PHYS_OFFSET 0xB80000 + DOORBELL 0x90).
    UInt32 kickToken_ = 0;
    UInt32 kickPolls_ = 0;
    bool kickTimeOk_ = false;
    bool kickRung_ = false;
    // 0.36.0: pushbuffer kick — host SEM_EXECUTE release of
    // 0xC0FFEE35 into the GPFIFO page (+0xF00), PB at +0x800.
    bool pbRung_ = false;
    UInt32 pbPolls_ = 0;
    // 0.38.0: client-RM GR context (compute set) — one contiguous VRAM
    // block (MAIN|PATCH|FECS_EVENT|PRIV_ACCESS_MAP) mapped at GPFIFO VA
    // + 0x1000 through PTE indices 1..ctxPtesInstalled_ of the same PT.
    UInt64 ctxBackingOffset_ = 0;
    UInt32 ctxPtesInstalled_ = 0;
    // 0.39.0: PD0 (dual PDE, 16B entries, 2 MiB each) table address from
    // GET_PDE_INFO; our 2 MiB huge PTEs live at indices 32..52.
    UInt64 pd0Address_ = 0;
    UInt64 pdbAddress_ = 0;      // 0.101.0: channel VAS PDB (pde-info)
    UInt32 vramVaTables_ = 0;    // 0.101.0: PD0 tables of the VRAM VA window
    // 0.102.0: CPU/GPU shared window (MTLStorageModeShared-style): 2 MiB
    // physically contiguous wired chunks at GPU VA 0x30_0000_0000 (sysmem
    // coherent PTEs), mappable into user tasks via clientMemoryForType(1).
    static constexpr UInt32 kShmChunks = 32;
    IOBufferMemoryDescriptor *shmChunk_[kShmChunks] = {};
    UInt32 shmChunkCount_ = 0;
    IOMemoryDescriptor *shmUser_ = nullptr;
    UInt32 ctxHugeInstalled_ = 0;
    bool gr3dOk_ = false;
    // 0.41.0: consumed GSP RC_TRIGGERED / MMU_FAULT_QUEUED events.
    UInt32 rcEvents_ = 0;
    UInt32 mmuFaultEvents_ = 0;
    UInt32 otherEvents_ = 0;
    // 0.43.0: 2 MiB PTEs mapping VRAM [0,36 MiB) (GOP scanout) at PD0
    // slots 53..70; left for RM's VAS free like the ctx ones.
    UInt32 fbHugeInstalled_ = 0;
    // 0.46.0: pre-GSP proof that BAR1+0 (GOP scanout) is VRAM offset 0.
    bool gopAtVram0_ = false;
    // 0.55.0: our window channel scans out VRAM 0; never free the client.
    bool wndOwnsScreen_ = false;
    // 0.56.0: window-0 pushbuffer write position (bytes) and the lock
    // shared by pollStatus/flipWindow (both drive the PRAMIN window).
    UInt32 wndPut_ = 0;
    UInt32 flips_ = 0;
    // 0.57.0: persistent GR submission state.
    bool grPersistent_ = false;
    UInt32 subPbOff_ = 0;
    UInt32 subSeq_ = 0;
    UInt32 asyncOutstanding_ = 0;   // 0.103.0
    UInt32 gpuResets_ = 0;          // 0.104.0
    bool resetPrivateDiagnostic_ = false; // .178.24: opt-in only, first reset generation
    UInt32 tlbInvalidates_ = 0;
public:
    struct GpuMem {
        const void *owner;
        UInt64 bytes, phys;            // phys: VRAM objects
        UInt32 domain, chunks;         // domain 0 VRAM, 1 SYS, 3 wired user memory (0.134.0)
        IOBufferMemoryDescriptor **chunk;
        IOMemoryDescriptor *user;      // CPU view: SYS chunks, or (cpu) BAR1 range
        bool cpu;                      // 0.123.0: VRAM object inside the BAR1 window
        IOBufferMemoryDescriptor **saved;   // 0.127.0: S3 copy (2 MiB chunks)
        UInt32 savedChunks;
        bool client, presented;        // 0.149.0: eviction candidates
        UInt64 stamp;                  // 0.149.0: last alloc/bind (LRU tie-break)
    };
private:
    // 0.112.0: 1024 -> 4095 (NVK advertises maxMemoryAllocationCount 4096;
    // handles must fit the 12-bit field of map type 0x1000 | handle).
    // 0.162.0: 16383. The table is shared by every client: after a Tahoe
    // login mediaanalysisd (2301) and Spotlight (1359) filled all 4095 and
    // every new process got memAlloc failures. Handles above 0xfff map with
    // type 0x40000000 | handle.
    static constexpr UInt32 kMaxMem = 16383;
    GpuMem mem_[kMaxMem] = {};
    nvgsp::VramHeap<kMaxMem> vramHeap_;   // 0.126.0: live VRAM ranges, sorted
    // 0.127.0: S3 save/restore of the kext-heap VRAM objects (SR path).
    bool vramFrozen_ = false;      // sleep in progress: memory/submit calls wait
    bool vramSaved_ = false;       // every live VRAM object has a sysmem copy
    bool resetVramSaved_ = false;  // 0.176.0: that copy was taken by gpuReset, restore it at resume end
    struct LiveClient { const void *owner; task_t task; };
    LiveClient liveClients_[256] = {};
    UInt32 adopted_ = 0;   // 0.177.0: memAdopt checks the old owner here, never dereferences it
    UInt64 vramSavedBytes_ = 0;
    void waitThawLocked();
    void thawLocked();
    IOReturn flipCopy(NVGspFlipCopy *f);            // 0.154.0
    void waitSubmittedUnlocked(UInt32 timeoutUs);   // 0.155.0
    UInt32 nullArenaRefusals_ = 0, unloadFailures_ = 0;   // 0.168.0
    char gpuEvents_[16][176] = {};                   // 0.170.0
    UInt32 gpuEventCount_ = 0;
    UInt32 arenaReclaims_ = 0, arenaFull_ = 0;      // 0.172.0
    typedef void (*ResetDoneFn)(void *ref);          // 0.173.0, "nvgsp-reset-register"
    ResetDoneFn resetDoneFn_[4] = {};
    void *resetDoneRef_[4] = {};
    void noteGpuEventLocked(const char *what);
    UInt64 lastStallAbs_ = 0;
    UInt32 stalls_ = 0;
    bool ceCopyLinesLocked(const nvgsp::EvictCopy *c, UInt32 n);
    bool ceCopyBatchLocked(const nvgsp::EvictCopy *c, UInt32 n);
    bool saveVramLocked(bool needGr = true, bool clientOnly = false);
    bool restoreVramLocked(UInt32 *skipped = nullptr);
    void freeSavedVramLocked();
    void publishSemVa();             // 0.128.0
    // 0.111.0/0.114.0: per-client user VA arena (see NVGspControl.cpp
    // "per-client arenas"): private tables managed by nvgsp::ArenaMap with
    // 2 MiB and 64 KiB pages; ArenaBackend gives it PRAMIN access and VRAM.
    struct ArenaCtx;
    struct ArenaBackend {
        NVGspControl *d;
        ArenaCtx *ctx;
        bool read64(UInt64 vram, UInt64 *v);
        bool write64(UInt64 vram, UInt64 v);
        bool zero(UInt64 vram, UInt64 bytes);
        bool fill16(UInt64 vram, UInt64 bytes, UInt64 q0, UInt64 q1);
        bool allocChunk(UInt64 *phys);
        void *allocOwners(UInt32 bytes);
        void freeOwners(void *p, UInt32 bytes);
    };
    struct ArenaCtx {
        const void *owner;
        int pid;                  // 0.158.0: process that made it (fault reports)
        bool ready;               // tables initialised for the current VAS
        nvgsp::ArenaMap<ArenaBackend> map;
    };
    static constexpr UInt32 kRegistryCap = 4096 - 48 - 32;   // one queue slot (0.113.0)
    static constexpr UInt32 kUserAbi = 124;   // NVGspControl-abi
    // 0.172.0: 64 (was 16). Every GPU process holds one; 15 apps + WindowServer +
    // helpers reached 16, and after a reset the stale contexts kept every slot,
    // so no client could reopen (WindowServer "could not reopen", black screen).
    static constexpr UInt32 kMaxArenaCtx = 64;
    static constexpr UInt64 kArenaCtxBytes = nvgsp::kArenaTablesBytes;
    ArenaCtx *arenaCtx_[kMaxArenaCtx] = {};
    UInt64 arenaPoolPhys_ = 0;     // 4 MiB kext-owned VRAM object (tag: &arenaPoolPhys_)
    ArenaCtx *arenaActive_ = nullptr;
    bool arenaInstalled_ = false;  // PD1[320..383] reflect arenaActive_
    UInt32 arenaSwitches_ = 0;
    ArenaCtx *arenaForLocked(const void *owner, bool create);
    bool arenaWritePd1Locked(const ArenaCtx *c);
    bool drainEnginesLocked();
    IOReturn arenaSwitchLocked(const void *owner);
    void arenaReleaseLocked(const void *owner);
    void arenaDropAllLocked(bool keepContexts);
    bool arenaFlushIfLiveLocked(const ArenaCtx *c);
    void explainFaultLocked(UInt64 va);                // 0.158.0
    void publishMemByClientLocked();                   // 0.160.0
    void arenaFreeChunksLocked(ArenaCtx *c);
    void arenaFreeCtxLocked(UInt32 slot);
    void releaseGpuMemLocked(UInt32 index);
    // 0.149.0: VRAM oversubscription. When the heap is full, an idle client
    // object moves to system memory (CE copy, arena PTEs remapped).
    struct OwnerUse { const void *owner; UInt64 stamp; };
    static constexpr UInt32 kOwnerUse = 32;
    OwnerUse ownerUse_[kOwnerUse] = {};
    UInt64 useClock_ = 0;
    UInt32 evicted_ = 0, evictFails_ = 0;
    UInt64 evictedBytes_ = 0, evictNs_ = 0;
    void noteUseLocked(const void *owner);
    UInt64 ownerStampLocked(const void *owner) const;
    bool evictOneLocked(const void *requester);
    bool evictObjectLocked(UInt32 index);
    IOReturn memAllocLocked(const void *owner, UInt64 bytes, UInt32 domain,
                            UInt32 *handleOut, UInt64 *physOut,
                            IOBufferMemoryDescriptor **pre = nullptr);   // 0.147.0: takes pre
    // 0.110.0: window 0 surface state + current Vulkan presenter.
    struct WindowSurface {
        UInt32 pitch, width, height, format;
        bool operator==(const WindowSurface &o) const {
            return pitch == o.pitch && width == o.width && height == o.height &&
                   format == o.format;
        }
    };
    static constexpr WindowSurface kDesktopSurface{16384, 3840, 2160, 0xCF};
    // 0.137.0: the desktop surface follows the current mode (same VRAM 0 and
    // pitch, smaller SIZE_IN/SIZE_OUT for smaller modes).
    WindowSurface desktopSurface_ = kDesktopSurface;
    struct HeadMode {
        bool valid;
        UInt32 pclkHz, hActive, vActive, rasterSize, syncEnd, blankEnd, blankStart,
            minFrameIdle, polarity, hBlankSym, vBlankSym;
        bool cursor64;   // 0.139.0: IMP + usage bounds with a 64x64 cursor
        bool olut;       // 0.140.0: identity OLUT + OCSC0 inside the modeset
        bool composite;  // 0.142.0: window 0 out of BYPASS, with an identity ILUT
    };
    HeadMode mode_{};
    const void *presentOwner_ = nullptr;
    UInt32 presentHandle_ = 0;
    WindowSurface presentSurface_{};
    UInt32 presents_ = 0;
    IOReturn flipWindowLocked(UInt64 offsetBytes, bool fullInit, const WindowSurface &surf,
                              bool interlockCore = false);
    IOReturn presentStopLocked(const void *owner);
    bool unbindObjectLocked(UInt32 handle);
    // 0.58.0: 256 MiB benchmark scratch VRAM at GPU VA 0x1_1000_0000.
    UInt64 scratchOffset_ = 0;
    UInt32 scratchHuge_ = 0;
    UInt64 scratchTry_ = 0;
    bool scratchRetry_ = false;
    // 0.62.0: client FB heap (largest unreserved FB region).
    UInt64 fbFreeBase_ = 0;
    UInt64 fbFreeLimit_ = 0;
    // 0.63.0: copy-engine channel living in one 2 MiB chunk of the client
    // heap mapped at PD0 slot 71 (VA 0x1_08E0_0000): GPFIFO +0, USERD
    // +0x1000, instance/RAMFC +0x2000, method buffer +0x4000, PB ring
    // +0x10000, semaphore +0x1F0000.
    UInt64 ceChunk_ = 0;
    bool ceMapped_ = false;
    bool ceStarted_ = false;
    bool cePersistent_ = false;
    UInt32 ceToken_ = 0;
    UInt32 cePbOff_ = 0;
    UInt32 ceSeq_ = 0;
    UInt64 ceUserdOffset_ = 0;
    UInt64 ceInstOffset_ = 0;
    UInt64 ceMthdOffset_ = 0;
    struct Ring {
        UInt64 pbPhys, pbVa, pbBytes, semPhys, semVa, gpfifoPhys, userdPhys;
        UInt32 token;
        UInt32 *pbOff;
        UInt32 *seq;
    };
    IOReturn submitRing(const Ring &ring, const UInt32 *words, UInt32 count,
                        UInt64 *nsOut, bool async = false);
    bool readRingSem(const Ring &ring, UInt32 *value);
    void captureStallLocked(const Ring &ring, UInt32 want, UInt32 where);   // 0.156.0
    bool waitRingSem(const Ring &ring, UInt32 seq, UInt32 timeoutUs);
    // 0.164.0: rings that stopped moving. Once a long wait on a ring has run
    // out and its semaphore has not moved since, further waits get 20 ms:
    // lock_ used to be held for 2-4 s per call while the GPU was dead, and
    // WindowServer starved behind it (userspace watchdog panic, 30 Sep
    // 09:33). No progress for 5 s, or an RC on the channel = hung: submits
    // fail at once and the automatic reset is asked for.
    struct RingStall { bool stalled, hung; UInt32 sem; UInt64 since; };
    RingStall grStall_{}, ceStall_{};
    UInt32 genEnd_ = 0, genSeenAtEnd_ = 0;
    volatile bool resetBusy_ = false;
    void syncRingSeqLocked(bool gr, bool ce);
    RingStall *stallFor(const Ring &ring);
    UInt32 stallCapUs(const Ring &ring, UInt32 timeoutUs);
    void noteRingWaitLocked(const Ring &ring, bool reached);
    void ringHungLocked(RingStall *st);
    // 0.164.0: the last Xid texts, oldest first (an SM exception sends several
    // lines and the last one only names the channel)
    char osErrLog_[8][120] = {};
    UInt32 osErrCount_ = 0;
    bool waitFenceSleep(UInt32 engine, UInt32 seq, UInt32 timeoutUs,
                        const void *owner = nullptr);   // 0.118.0/0.119.0
    bool grRing(Ring *out);
    bool ceRing(Ring *out);          // 0.112.0
    UInt32 ceOutstanding_ = 0;       // 0.112.0: CE GP entries in flight
    // 0.123.0: BAR1 in physical mode maps VRAM [0, bar1Bytes_) 1:1; VRAM
    // objects allocated inside it (memAlloc domain 2) are CPU-mappable.
    bool bar1Live_ = false;
    // 0.130.0: ReBAR done by the kext (macOS IOPCIFamily cannot size a BAR
    // above 1 GiB): BAR1/BAR3 moved above RAM, GPP0 prefetch window widened.
    IODeviceMemory *bar1Mem_ = nullptr, *bar3Mem_ = nullptr;
    struct Rebar {
        bool active;
        UInt32 ctrlOff, ctrl, sizeLog2Mb;
        UInt64 bar1Base, bar1Bytes, bar3Base, bar3Bytes;
    } rebar_ = {};
    bool rebarResize();
    bool rebarProgram(IOPCIDevice *bridge, UInt32 ctrl, UInt64 bar1, UInt64 bar3, UInt64 winBase,
                      UInt64 winEnd);
    IODeviceMemory *bar1Dev() const;
    IODeviceMemory *bar3Dev() const;
    IOMemoryMap *mapBar1Head(IOByteCount bytes) const;
    // 0.151.0: ring words (GPFIFO, USERD, pushbuffer tails, semaphores)
    // straight through BAR1 (physical mode, BAR1 offset = VRAM address)
    // instead of the PRAMIN window. Cached uncached 2 MiB kernel windows.
    volatile UInt32 *bar1Ptr(UInt64 phys, UInt32 bytes);
    bool ringWrite(UInt64 phys, const UInt32 *words, UInt32 count);
    bool ringRead(UInt64 phys, UInt32 *value);
    IOLock *bar1Lock_ = nullptr;
    struct Bar1Win { UInt64 base; IOMemoryMap *map; };
    Bar1Win bar1Win_[16] = {};
    UInt32 bar1Wins_ = 0;
    bool bar1Direct_ = true;          // boot-arg nvgsp-nobar1ring turns it off
    UInt64 bar1Bytes_ = 0;
    UInt64 vramCpuWindowEnd() const;
    // 0.116.0 (V1): video engine channels (see videoOpen).
    struct VideoChan {
        UInt64 chunk, chunkBytes, userd, inst, mthd, ctxOff;
        UInt32 runlist, chram;   // 0.125.0: PRI bases from the runlist diagnostic
        UInt32 memHandle, ctxBytes, token, pbOff, seq, outstanding, stage, status;
        UInt32 rmToken;          // 0.129.0: RM's token (chid only); token = doorbell id << 16 | chid
        bool ready, dead;   // 0.122.0: dead = fence timed out, not drained
    };
    VideoChan video_[nvgsp::kVideoEngineCount] = {};
    // 0.149.0: kernel-only copy channel (COPY0, chid 8, ADMIN privilege) for
    // physical CE copies (eviction, S3). User channels are USER privilege and
    // PBDMA rejects physical-mode copies there (Xid 32). Never reachable from
    // a user client; it is videoSetup's index kVideoEngineCount.
    VideoChan kce_ = {};
    UInt32 kceFailGen_ = ~0U;
    bool kceRing(Ring *out);
    void kceEnsure();
    bool videoBusy_ = false;
    bool videoRing(UInt32 index, Ring *out);
    IOReturn videoSetup(UInt32 index, UInt32 gen);
    void videoDropLocked();
    // 0.178.0: per-client GR channels. state: 0 free, 1 opening/closing, 2 live.
    struct ClientChannel {
        const void *owner;
        UInt32 state, gen, token, pbOff, seq, memHandle, made, outstanding, serial;
        UInt64 block, ctx, userd, inst, mthd, pd1, pd1Saved8;
        UInt64 arenaTables;   // tables PD1[320..383] point at (0 = none yet)
        UInt64 pd1Written[8]; // PD1 entries we set (restored before the VAS free)
        bool closeAsked;      // 0.178.16: owner closed while the open ran
    };
    static constexpr UInt32 kMaxClientChannels = 32;   // chids 9..40
    ClientChannel cchan_[kMaxClientChannels] = {};
    UInt32 cchanOpens_ = 0, cchanFails_ = 0, cchanSubmits_ = 0;
    UInt32 cchanSerial_ = 0;   // survives close/slot reuse; reset clears pending stamps
    bool cchanMutationBusy_ = false;
    bool cchanLifecycleBusy_ = false;
    bool waitChannelMutationLocked();
    bool waitChannelLifecycleLocked();
    bool drainClientChannelsLocked();
    UInt32 userRpcSeq_ = 0, userRpcSeqCounter_ = 0, userRpcSeqMismatch_ = 0;   // 0.178.9
    // 0.178.12: IOAccel GR stamps while client channels exist. The stamp is
    // one counter for every process ("all work up to N is done"); with several
    // GR channels the kext writes it from the CPU, in submission order, once
    // each submission's own ring fence has landed (no GPU-side acquires).
    struct PendingStamp { UInt32 value, seq, offset, serial; UInt16 slot, pad; };
    static constexpr UInt32 kStampQ = 4096;
    PendingStamp stampQ_[kStampQ] = {};
    UInt32 stampQHead_ = 0, stampQTail_ = 0, stampWritten_ = 0, stampQueued_ = 0;
    bool stampAdvanceLocked();
    bool anyClientChannelLocked() const;
    ClientChannel *clientChannelLocked(const void *owner);   // live only
    bool clientRingLocked(ClientChannel *c, Ring *out);
    bool grRingFor(const void *owner, Ring *out);   // own channel, else shared GR
    IOReturn clientArenaLocked(ClientChannel *c);
    void clientChannelsDropLocked();                   // VAS/GSP gone (reset, stop)
    bool clientChannelFreeRm(UInt32 slot, UInt32 made, UInt32 *failOut);
    void clientChannelsTeardown();                     // 0.178.10: before a reset
    void videoMarkDeadLocked(UInt32 index);   // 0.122.0
    void videoRunlistDiagLocked(UInt32 index, const char *suffix);   // 0.123.0
    void runlistDiagLocked(const char *key, UInt32 devType, UInt32 inst, UInt32 chid,
                           UInt32 token, UInt32 *runlistOut, UInt32 *chramOut, UInt32 *tokenOut = nullptr);
    IOLock *lock_ = nullptr;
    bool grRung_ = false;
    UInt32 grPolls_ = 0;
    bool channelBackingComplete_ = false;
    bool channelAllocOk_ = false;
    bool gpfifoPteInstalled_ = false;
    bool gpfifoPteRestored_ = false;
    UInt64 pte4KAddress_ = 0;
    UInt64 originalPte_ = 0;
    bool dmaMapCompleted_ = false;
    bool bar2MapUnsupported_ = false;
    bool praminPteReadOk_ = false;
    bool hostPteMapped_ = false;
    bool hostPteValidated_ = false;
    bool hostPteRestored_ = false;
    bool pteMapInvalidateOk_ = false;
    bool pteRestoreInvalidateOk_ = false;
    // 0.9.2: GOP surface (BAR1+0, first 4 KiB) FNV-1a hash captured at
    // stage time (pre-FWSEC) for the post-boot preservation check.
    UInt64 gopSurfaceHashPre_ = 0;
    bool gopSurfaceHashPreOk_ = false;
    // 0.9.4: first 64 bytes (8x LE u64) of BAR1+0 pre-FWSEC, plus the
    // mid-point hash (end of executeBoot) to isolate the clobberer.
    UInt64 gopSurfaceBytesPre_[8] = {};
    UInt64 gopSurfaceHashMid_ = 0;
    bool gopSurfaceHashMidOk_ = false;
    // 0.9.5: per-poll BAR1+0 bisect hash index.
    UInt32 bisectPoll_ = 0;
    bool hashBar1Surface(UInt64 *hash);
    bool hashBar1Range(UInt64 offset, unsigned len, UInt64 *hash);
};

// 0.150.1: the class name starts with IOAccel because WindowServer's sandbox
// (com.apple.WindowServer.sb) opens user clients by that prefix only
class IOAccelNVGspUserClient : public IOUserClient {
    OSDeclareDefaultStructors(IOAccelNVGspUserClient)
public:
    bool initWithTask(task_t owningTask, void *securityID, UInt32 type,
                      OSDictionary *properties) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    IOReturn callPlatformFunction(const OSSymbol *name, bool wait, void *p1,
                                  void *p2, void *p3, void *p4) override;
    IOReturn clientClose() override;
    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options,
                                 IOMemoryDescriptor **memory) override;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *arguments,
                            IOExternalMethodDispatch *dispatch = nullptr,
                            OSObject *target = nullptr, void *reference = nullptr) override;

    IOReturn generationStatus() const;  // 0.174.0: native and scalar validation use the same gate

private:
    UInt32 resetGen_ = 0;   // 0.110.0: driver reset generation at open
    static IOReturn commit(OSObject *target, void *, IOExternalMethodArguments *arguments);
    static IOReturn boot(OSObject *target, void *, IOExternalMethodArguments *arguments);
    static IOReturn status(OSObject *target, void *, IOExternalMethodArguments *arguments);
    static IOReturn setFlags(OSObject *target, void *, IOExternalMethodArguments *arguments);
    static IOReturn flip(OSObject *target, void *, IOExternalMethodArguments *arguments);
    static IOReturn submit(OSObject *target, void *, IOExternalMethodArguments *arguments);
    static IOReturn ping(OSObject *target, void *, IOExternalMethodArguments *arguments);
    static IOReturn peek(OSObject *target, void *, IOExternalMethodArguments *arguments);
    static IOReturn poke(OSObject *target, void *, IOExternalMethodArguments *arguments);
    static IOReturn rpc(OSObject *target, void *, IOExternalMethodArguments *arguments);
    static IOReturn vram(OSObject *target, void *, IOExternalMethodArguments *arguments);
    static IOReturn core(OSObject *target, void *, IOExternalMethodArguments *arguments);
    static IOReturn gsplog(OSObject *target, void *, IOExternalMethodArguments *arguments);
    static IOReturn modeset(OSObject *target, void *, IOExternalMethodArguments *arguments);
    static IOReturn userMemBind(OSObject *target, void *, IOExternalMethodArguments *arguments);
    static IOReturn setMode(OSObject *target, void *, IOExternalMethodArguments *arguments);
    static IOReturn cursorTest(OSObject *target, void *, IOExternalMethodArguments *arguments);
    static IOReturn grStateOwner(OSObject *target, void *, IOExternalMethodArguments *arguments);
    static IOReturn memAdopt(OSObject *target, void *, IOExternalMethodArguments *arguments);
    static IOReturn diagnosticChannelKick(OSObject *target, void *, IOExternalMethodArguments *arguments);
    task_t task_ = nullptr;
    bool admin_ = false;   // 0.150.0: non-admin clients get the Metal/video selectors only
    UInt32 ownTries_ = 0;   // 0.178.6/0.178.10
    static IOReturn modesetLight(OSObject *target, void *reference,
                                 IOExternalMethodArguments *arguments);
    static IOReturn srcycle(OSObject *target, void *reference,
                            IOExternalMethodArguments *arguments);
    static IOReturn submitAsync(OSObject *target, void *, IOExternalMethodArguments *arguments);
    static IOReturn fenceWait(OSObject *target, void *, IOExternalMethodArguments *arguments);
    static IOReturn gpuReset(OSObject *target, void *, IOExternalMethodArguments *arguments);
    static IOReturn vaBind(OSObject *target, void *, IOExternalMethodArguments *arguments);
    static IOReturn execSegments(OSObject *target, void *, IOExternalMethodArguments *arguments);
    static IOReturn memAlloc(OSObject *target, void *, IOExternalMethodArguments *arguments);
    static IOReturn memFree(OSObject *target, void *, IOExternalMethodArguments *arguments);
    static IOReturn vaBindObject(OSObject *target, void *, IOExternalMethodArguments *arguments);
    static IOReturn vaUnbind(OSObject *target, void *, IOExternalMethodArguments *arguments);
    static IOReturn memInfo(OSObject *target, void *, IOExternalMethodArguments *arguments);
    static IOReturn present(OSObject *target, void *, IOExternalMethodArguments *arguments);
    static IOReturn presentStop(OSObject *target, void *, IOExternalMethodArguments *arguments);
    static IOReturn ceFenceWait(OSObject *target, void *, IOExternalMethodArguments *arguments);
    static IOReturn videoOpen(OSObject *target, void *, IOExternalMethodArguments *arguments);
    static IOReturn engineFenceWait(OSObject *target, void *, IOExternalMethodArguments *arguments);
    static IOReturn accelGo(OSObject *target, void *, IOExternalMethodArguments *arguments);   // 0.132.0
    static IOReturn clientChannel(OSObject *target, void *, IOExternalMethodArguments *arguments);   // 0.178.0
    NVGspControl *driver_ = nullptr;
    IOBufferMemoryDescriptor *package_ = nullptr;
    bool ensurePackage();                         // 0.171.0
};
