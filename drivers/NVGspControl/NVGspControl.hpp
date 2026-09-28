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
    // Experiment flags from userspace (nvgsp_load --flags): bit0 = GPU
    // test draw on scanout, bit1 = display core-channel probe.
    IOReturn setExperimentFlags(UInt32 flags);
    // Live window-0 flip (offset in bytes into VRAM, 256 B aligned) on the
    // persistent window channel; serialized with pollStatus.
    IOReturn flipWindow(UInt64 offsetBytes, bool fullInit = false);
    // Submit a GR method stream on the persistent channel; a host WFI
    // semaphore is appended, the call waits for it and returns the GPU
    // execution time in ns.
    IOReturn submitGr(const void *owner, const UInt32 *words, UInt32 count, UInt64 *nsOut);
    // Asynchronous GR submission + fence wait (Metal-style queues).
    IOReturn submitGrAsync(const void *owner, const UInt32 *words, UInt32 count,
                           UInt32 *seqOut);
    IOReturn waitGrFence(UInt32 seq, UInt32 timeoutUs, UInt32 *completedOut);
    IOReturn waitFence(UInt32 engine, UInt32 seq, UInt32 timeoutUs,
                       UInt32 *completedOut);   // 0 GR, 1 CE, 2 and up video
    // Bring up video engine channel `index` (0 NVDEC0, 1 NVENC0, 2 OFA0) on first
    // use; its work goes through submitSegments / waitFence with engine 2 + index.
    // stage/status = last setup step + RM status.
    IOReturn videoOpen(UInt32 index, UInt32 *stageOut, UInt32 *statusOut);
    IOReturn gpuReset();
    // Bumped by every gpuReset(); user clients opened before a reset
    // see their GPU state (fences, VAS, channel) as lost.
    UInt32 resetGeneration() const { return gpuResets_; }
    // Shared kernel interface (NVK nvkmd / NVAccelerator): TLB
    // invalidate + VA bind in the user VA arena PD1[320..383] (GPU VA
    // 0x28_0000_0000 .. 0x30_0000_0000), 2 MiB pages.
    IOReturn tlbInvalidate();
    IOReturn vaBind(const void *owner, UInt64 va, UInt64 phys, UInt64 bytes, UInt32 flags);
    IOReturn submitSegments(const void *owner, UInt32 engine, const UInt64 *segVa,
                            const UInt32 *segDwords, const UInt32 *segFlags,
                            UInt32 n, UInt32 *seqOut,
                            UInt64 stampVa = 0, UInt32 stampValue = 0);   // stamp
    // GPU memory objects (VRAM first-fit allocator / sysmem 2 MiB
    // chunks), owned by a user client and freed when it closes.
    IOReturn memAlloc(const void *owner, UInt64 bytes, UInt32 domain,
                      UInt32 *handleOut, UInt64 *physOut);
    IOReturn memFree(const void *owner, UInt32 handle);
    void memFreeAll(const void *owner);
    IOReturn vaBindObject(const void *owner, UInt32 handle, UInt64 va, UInt32 flags,
                          UInt64 memOffset = 0, UInt64 range = 0);
    IOMemoryDescriptor *memUserDescriptor(const void *owner, UInt32 handle,
                                          bool *isVram = nullptr);
    // Unbind [va, va + bytes) (pages of this owner's objects or raw binds
    // only) and heap/usage info {VRAM heap bytes, VRAM used, SYS used}.
    IOReturn vaUnbind(const void *owner, UInt64 va, UInt64 bytes);
    IOReturn memInfo(UInt64 *heapBytesOut, UInt64 *vramUsedOut, UInt64 *sysUsedOut);
    // zero-copy present of a VRAM object on window 0 / hand the screen
    // back to the desktop surface.
    IOReturn presentObject(const void *owner, UInt32 handle, UInt64 offset, UInt32 pitch,
                           UInt32 width, UInt32 height, UInt32 format);
    IOReturn presentStop(const void *owner);
    // same for the copy-engine channel.
    IOReturn submitCe(const void *owner, const UInt32 *words, UInt32 count, UInt64 *nsOut);
    // GSP round-trip probe, sends a P-state read RPC and waits for its
    // reply; returns latency and whether the MSI path drained it.
    IOReturn pingGsp(UInt64 *nsOut, UInt64 *viaIntrOut);
    // live debug surface for display bring-up without reboots.
    IOReturn peekBar0(UInt32 offset, UInt32 count, UInt32 *out);
    IOReturn pokeBar0(UInt32 offset, UInt32 value);
    IOReturn vramAccess(UInt64 offset, UInt32 *words, UInt32 count, bool write);
    IOReturn submitCore(const UInt32 *words, UInt32 count);
    // core UPDATE hygiene. coreUnstick releases a core that
    // waits on a stale interlock (SET_ACCL IGNORE_INTERLOCK, interlocks 0,
    // accel back to none); coreException acks a pending FE exception on
    // the core and returns its stat word (0 when none was pending).
    IOReturn coreUnstick();
    UInt32 coreException();
    IOReturn cursorTest(UInt32 op);
    // true when the last GR submission came from a client other
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
    // native DPCD read over DP AUX (cmd 0x9), <= 16 bytes.
    IOReturn dpAuxRead(UInt32 addr, UInt8 *out, UInt32 bytes);
    // driver-driven DP modeset on head 0, the modeset3.py sequence
    // (snapshot, manual-DP, detach, link release, ASSIGN_SOR, D0, train,
    // CONFIG_STREAM, attach) run in-kernel. *statusOut is 0 on a full pass,
    // else 0xSS000000 | step status with SS = failing step number.
    IOReturn modesetHead0(UInt32 *statusOut, bool lightIfLit = false);
    // switch head 0 to an EDID timing. t = {pixel clock kHz,
    // hActive, hBlank, hSyncOffset, hSyncWidth, vActive, vBlank, vSyncOffset,
    // vSyncWidth, flags (bit0 hsync+, bit1 vsync+, bit2 64 px cursor)}. The timing sticks: resume
    // and hotplug modesets re-apply it. statusOut = modesetHead0's code.
    IOReturn setMode(const UInt32 t[10], UInt32 *statusOut);
    // wire [uaddr, uaddr + bytes) of `task` and map its 4 KiB pages at
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
    // S3 sleep/wake. Quiesce on WillSleep (sink off, phase-0 fence, MSI
    // teardown), full chain-state reset for a daemon-driven re-boot after
    // wake; sleeping_ gates the daemon's resume trigger.
    static IOReturn sleepWakeHandler(void *target, void *refCon, UInt32 messageType,
                                     IOService *provider, void *messageArgument,
                                     vm_size_t argSize);
    void quiesceForSleep();
    void resumeFromSleep();
    // NVIDIA-style GSP suspend/resume (nouveau r535_gsp_fini/
    // tu102_gsp_fini/init + r570 fbsr). srSuspend unloads GSP-RM with
    // bInPMTransition and tears WPR2 down via Booter Unload into srData_;
    // srResume boots GSP-RM from the SR metadata (RM restores its state).
    IOReturn srSuspend();
    IOReturn srResume();
    IOReturn srCycle();   // debug: suspend + resume without system sleep
    bool srStageBuffers();
    IOReturn executeUnload();
    // wait (lock_ held, dropped while sleeping) until no other RPC
    // owns the reply slot; false when phase/sleep rule it out or it stays busy.
    bool waitRpcSlotLocked(UInt32 ms);
    // lock_ hold profiling. The boot logo animation froze for a
    // while and the interrupt handler once waited 1.16 s for lock_: record
    // the longest hold, the source line that took the lock, when (uptime)
    // and at which post-init phase.
    void lk(UInt32 line) {
        IOLockLock(lock_);
        clock_get_uptime(&lockAt_);
        lockLine_ = line;
    }
    void ulk();
    // boot timeline (uptime ms per step) for the logo-animation freeze
    void markBoot(const char *tag);
    bool zeroVram(UInt64 phys, UInt64 bytes);   // BAR1 memset, lock_ not held
    UInt32 vramZeroMisses_ = 0, vramZeroed_ = 0;
    // BAR1 remap right after GSP INIT_DONE (the kernel console,
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
    static void autoResetCallout(thread_call_param_t self, thread_call_param_t);
    static void rebarOkCallout(thread_call_param_t self, thread_call_param_t);
    void scheduleAutoResetLocked();

private:
    // System info used by the cold-boot init staging, kept so the S3 re-boot
    // can re-stage init_ exactly like a cold boot.
    nvgsp::GspSystemInfoParameters systemInfo_{};
    bool systemInfoValid_ = false;
    // Last VBIOS/driver-lit head 0 state (ARMED head methods), the S3
    // re-boot finds the display engine at reset defaults. boot-chain
    // profiling ring (per daemon poll).
    struct PollProf { UInt16 phaseIn, phaseOut; UInt16 records, idle; UInt32 lockUs, firstUs, drainUs, totalUs; };
    PollProf pollProf_[160] = {};
    UInt32 pollProfCount_ = 0;
    uint64_t profT0_ = 0, profFirst_ = 0, profDrainEnd_ = 0;
    UInt32 profRecords_ = 0, profIdle_ = 0;
    bool drainNoWait_ = false;   // last chain step queued no RPC
    IOMemoryMap *doorbellMap_ = nullptr;   // persistent BAR0 map for 0x110c00
    static void gspDoorbell(void *ctx);
    UInt32 *headCache_ = nullptr;
    bool headCacheValid_ = false;
    // head-0 VPLL (0x00ef00..0x00ef1c) as programmed by the VBIOS/ GOP;
    // after S3 the PLL is at reset and GSP-RM does not re-program it.
    UInt32 vpllCache_[8] = {};
    bool vpllCacheValid_ = false;
    UInt32 experimentFlags_ = 0;
    // MSI interrupt path (experiment bit3). The GSP stall vector from
    // INTR_GET_KERNEL_TABLE is enabled in the VF CPU interrupt tree; each
    // MSI clears the leaf + GSP falcon SWGEN0, drains the status queue
    // (pollStatusLocked) and re-arms the top level.
    static constexpr UInt32 kVecGsp = 0, kVecDisp = 1, kVecGrNs = 2, kVecCeNs = 3,
                           kVecCount = 4;   // + CE0 non-stall
    // One NV04_DISPLAY_COMMON control (function 76). Returns the RM status
    // (reply+92), ~0U on transport failure; optionally copies the echoed
    // params (reply+104) for out-fields like DP_CTRL err.
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
    // DP AUX (DPCD + EDID) state.
    UInt32 auxStep_ = 0, auxRetry_ = 0, auxLastStatus_ = 0, auxLastReply_ = 0;
    bool auxOk_ = false;
    UInt8 dpcd_[48] = {};
    UInt8 edid_[256] = {};
    UInt32 edidBytes_ = 0;
    typedef void (*VblankFn)(void *ref, UInt32 count, UInt64 uptimeAbs);
    // IOAccel stamps. GPU writes them in the VRAM stamp
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
    // S3 sleep/wake interest + quiesce flag.
    IONotifier *sleepWakeNotifier_ = nullptr;
    bool sleeping_ = false;
    // slow-path chain self-drive (async; no daemon dependency).
    thread_call_t resumeCall_ = nullptr;
    thread_call_t autoResetCall_ = nullptr;
    thread_call_t rebarOkCall_ = nullptr;     // clears the ReBAR boot marker
    // activity boost. Submissions re-arm a short BOOST_TO_MAX so the
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
    UInt32 autoResets_ = 0, autoResetsSkipped_ = 0;
    // the resume in flight came from gpuReset (not S3), and how
    // many times in a row it came back without a GR channel.
    bool resetDriven_ = false;
    UInt32 resetRetries_ = 0;
    // owner of the last GR submission (nullptr on a fresh channel)
    const void *lastGrOwner_ = nullptr;
    UInt32 grOwnerSwitches_ = 0;
    void noteGrOwnerLocked(const void *owner);
    volatile bool resumeActive_ = false, resumeAbort_ = false;
    // post-stage queue snapshot (516 KiB) for the S3 re-boot.
    UInt8 *queueSnap_ = nullptr;
    UInt32 intrCount_ = 0, intrSpurious_ = 0, intrStuck_ = 0;
    bool inIntr_ = false;
    volatile bool pingOutstanding_ = false;
    bool pingViaIntr_ = false;
    UInt64 pingSentAt_ = 0, pingDoneAt_ = 0;
    UInt32 pings_ = 0;
    volatile bool userRpcOutstanding_ = false;
    UInt32 userRpcFunction_ = 0, userRpcReplyBytes_ = 0, userRpcResult_ = 0, userRpcs_ = 0;
    UInt8 userRpcReply_[4096] = {};
    UInt64 dispInstOffset_ = 0;
    // BAR1 physical-mode bind + evidence; runs once per boot on every
    // ctx-path exit (success, refusal, watchdog).
    void finishBar1();
    bool bar1Finished_ = false;
    // GPU test draw on the scanout only with boot-arg nvgspdraw=1.
    bool drawTest_ = false;
    IOPCIDevice *pci_ = nullptr;
    nvgsp::GspStaging gsp_;
    nvgsp::BooterStaging booter_;
    nvgsp::GspInitStaging init_;
    // SR state.
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
    // RM-assigned TSG handle decoded live from the channel-alloc echo
    // (offset 240). Never hardcoded: 0 means unknown, skip TSG path.
    UInt32 channelTsgHandle_ = 0;
    // hardware TSG ID from GET_INFO (diagnostic, output-only).
    UInt32 tsgHwId_ = 0;
    // Bounds the wait for the first unproven function-76 control since
    // the phase-39 stall (CE fault-method-buffer size, phase 49).
    UInt32 ceSizeStallPolls_ = 0;
    // NOP-kick state. Token from GPFIFO_GET_WORK_SUBMIT_TOKEN (phase
    // 118); doorbell = BAR0 0xBB0090 (NV_VIRTUAL_FUNCTION 0x30000 +
    // FULL_PHYS_OFFSET 0xB80000 + DOORBELL 0x90).
    UInt32 kickToken_ = 0;
    UInt32 kickPolls_ = 0;
    bool kickTimeOk_ = false;
    bool kickRung_ = false;
    // Pushbuffer kick, host SEM_EXECUTE release of 0xC0FFEE35 into the
    // GPFIFO page (+0xF00), PB at +0x800.
    bool pbRung_ = false;
    UInt32 pbPolls_ = 0;
    // client-RM GR context (compute set), one contiguous VRAM block
    // (MAIN|PATCH|FECS_EVENT|PRIV_ACCESS_MAP) mapped at GPFIFO VA +
    // 0x1000 through PTE indices 1..ctxPtesInstalled_ of the same PT.
    UInt64 ctxBackingOffset_ = 0;
    UInt32 ctxPtesInstalled_ = 0;
    // PD0 (dual PDE, 16B entries, 2 MiB each) table address from
    // GET_PDE_INFO; our 2 MiB huge PTEs live at indices 32..52.
    UInt64 pd0Address_ = 0;
    UInt64 pdbAddress_ = 0;      // channel VAS PDB (pde-info)
    UInt32 vramVaTables_ = 0;    // PD0 tables of the VRAM VA window
    // CPU/GPU shared window (MTLStorageModeShared-style): 2 MiB physically
    // contiguous wired chunks at GPU VA 0x30_0000_0000 (sysmem coherent
    // PTEs), mappable into user tasks via clientMemoryForType(1).
    static constexpr UInt32 kShmChunks = 32;
    IOBufferMemoryDescriptor *shmChunk_[kShmChunks] = {};
    UInt32 shmChunkCount_ = 0;
    IOMemoryDescriptor *shmUser_ = nullptr;
    UInt32 ctxHugeInstalled_ = 0;
    bool gr3dOk_ = false;
    // consumed GSP RC_TRIGGERED / MMU_FAULT_QUEUED events.
    UInt32 rcEvents_ = 0;
    UInt32 mmuFaultEvents_ = 0;
    UInt32 otherEvents_ = 0;
    // 2 MiB PTEs mapping VRAM [0,36 MiB) (GOP scanout) at PD0 slots
    // 53..70; left for RM's VAS free like the ctx ones.
    UInt32 fbHugeInstalled_ = 0;
    // pre-GSP proof that BAR1+0 (GOP scanout) is VRAM offset 0.
    bool gopAtVram0_ = false;
    // our window channel scans out VRAM 0; never free the client.
    bool wndOwnsScreen_ = false;
    // window-0 pushbuffer write position (bytes) and the lock shared by
    // pollStatus/flipWindow (both drive the PRAMIN window).
    UInt32 wndPut_ = 0;
    UInt32 flips_ = 0;
    // persistent GR submission state.
    bool grPersistent_ = false;
    UInt32 subPbOff_ = 0;
    UInt32 subSeq_ = 0;
    UInt32 asyncOutstanding_ = 0;
    UInt32 gpuResets_ = 0;
    UInt32 tlbInvalidates_ = 0;
public:
    struct GpuMem {
        const void *owner;
        UInt64 bytes, phys;            // phys: VRAM objects
        UInt32 domain, chunks;         // domain 0 VRAM, 1 SYS, 3 wired user memory 
        IOBufferMemoryDescriptor **chunk;
        IOMemoryDescriptor *user;      // CPU view: SYS chunks, or (cpu) BAR1 range
        bool cpu;                      // VRAM object inside the BAR1 window
        IOBufferMemoryDescriptor **saved;   // S3 copy (2 MiB chunks)
        UInt32 savedChunks;
        bool client, presented;        // eviction candidates
        UInt64 stamp;                  // last alloc/bind (LRU tie-break)
    };
private:
    // 1024 -> 4095 (NVK advertises maxMemoryAllocationCount 4096; handles
    // must fit the 12-bit field of map type 0x1000 | handle).
    static constexpr UInt32 kMaxMem = 4095;
    GpuMem mem_[kMaxMem] = {};
    nvgsp::VramHeap<kMaxMem> vramHeap_;   // live VRAM ranges, sorted
    // S3 save/restore of the kext-heap VRAM objects (SR path).
    bool vramFrozen_ = false;      // sleep in progress: memory/submit calls wait
    bool vramSaved_ = false;       // every live VRAM object has a sysmem copy
    UInt64 vramSavedBytes_ = 0;
    void waitThawLocked();
    void thawLocked();
    IOReturn flipCopy(NVGspFlipCopy *f);
    void waitSubmittedUnlocked(UInt32 timeoutUs);
    UInt64 lastStallAbs_ = 0;
    UInt32 stalls_ = 0;
    bool ceCopyLinesLocked(const nvgsp::EvictCopy *c, UInt32 n);
    bool ceCopyBatchLocked(const nvgsp::EvictCopy *c, UInt32 n);
    bool saveVramLocked();
    bool restoreVramLocked();
    void freeSavedVramLocked();
    void publishSemVa();
    // per-client user VA arena (see NVGspControl.cpp "per-client arenas"):
    // private tables managed by nvgsp::ArenaMap with 2 MiB and 64 KiB
    // pages; ArenaBackend gives it PRAMIN access and VRAM.
    struct ArenaCtx;
    struct ArenaBackend {
        NVGspControl *d;
        ArenaCtx *ctx;
        bool read64(UInt64 vram, UInt64 *v);
        bool write64(UInt64 vram, UInt64 v);
        bool zero(UInt64 vram, UInt64 bytes);
        bool allocChunk(UInt64 *phys);
        void *allocOwners(UInt32 bytes);
        void freeOwners(void *p, UInt32 bytes);
    };
    struct ArenaCtx {
        const void *owner;
        int pid;                  // process that made it (fault reports)
        bool ready;               // tables initialised for the current VAS
        nvgsp::ArenaMap<ArenaBackend> map;
    };
    static constexpr UInt32 kRegistryCap = 4096 - 48 - 32;   // one queue slot
    static constexpr UInt32 kUserAbi = 123;   // NVGspControl-abi
    static constexpr UInt32 kMaxArenaCtx = 16;
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
    void explainFaultLocked(UInt64 va);
    void publishMemByClientLocked();
    void arenaFreeChunksLocked(ArenaCtx *c);
    void arenaFreeCtxLocked(UInt32 slot);
    void releaseGpuMemLocked(UInt32 index);
    // VRAM oversubscription. When the heap is full, an idle client
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
                            IOBufferMemoryDescriptor **pre = nullptr);   // takes pre
    // window 0 surface state + current Vulkan presenter.
    struct WindowSurface {
        UInt32 pitch, width, height, format;
        bool operator==(const WindowSurface &o) const {
            return pitch == o.pitch && width == o.width && height == o.height &&
                   format == o.format;
        }
    };
    static constexpr WindowSurface kDesktopSurface{16384, 3840, 2160, 0xCF};
    // the desktop surface follows the current mode (same VRAM 0 and
    // pitch, smaller SIZE_IN/SIZE_OUT for smaller modes).
    WindowSurface desktopSurface_ = kDesktopSurface;
    struct HeadMode {
        bool valid;
        UInt32 pclkHz, hActive, vActive, rasterSize, syncEnd, blankEnd, blankStart,
            minFrameIdle, polarity, hBlankSym, vBlankSym;
        bool cursor64;   // IMP + usage bounds with a 64x64 cursor
        bool olut;       // identity OLUT + OCSC0 inside the modeset
        bool composite;  // window 0 out of BYPASS, with an identity ILUT
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
    // 256 MiB benchmark scratch VRAM at GPU VA 0x1_1000_0000.
    UInt64 scratchOffset_ = 0;
    UInt32 scratchHuge_ = 0;
    UInt64 scratchTry_ = 0;
    bool scratchRetry_ = false;
    // client FB heap (largest unreserved FB region).
    UInt64 fbFreeBase_ = 0;
    UInt64 fbFreeLimit_ = 0;
    // copy-engine channel living in one 2 MiB chunk of the client heap
    // mapped at PD0 slot 71 (VA 0x1_08E0_0000): GPFIFO +0, USERD +0x1000,
    // instance/RAMFC +0x2000, method buffer +0x4000, PB ring +0x10000,
    // semaphore +0x1F0000.
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
    void captureStallLocked(const Ring &ring, UInt32 want, UInt32 where);
    bool waitRingSem(const Ring &ring, UInt32 seq, UInt32 timeoutUs);
    bool waitFenceSleep(UInt32 engine, UInt32 seq, UInt32 timeoutUs);
    bool grRing(Ring *out);
    bool ceRing(Ring *out);
    UInt32 ceOutstanding_ = 0;       // CE GP entries in flight
    // BAR1 in physical mode maps VRAM [0, bar1Bytes_) 1:1; VRAM objects
    // allocated inside it (memAlloc domain 2) are CPU-mappable.
    bool bar1Live_ = false;
    // ReBAR done by the kext (macOS IOPCIFamily cannot size a BAR above 1
    // GiB): BAR1/BAR3 moved above RAM, GPP0 prefetch window widened.
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
    // ring words (GPFIFO, USERD, pushbuffer tails, semaphores)
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
    // video engine channels (see videoOpen).
    struct VideoChan {
        UInt64 chunk, chunkBytes, userd, inst, mthd, ctxOff;
        UInt32 runlist, chram;   // PRI bases from the runlist diagnostic
        UInt32 memHandle, ctxBytes, token, pbOff, seq, outstanding, stage, status;
        UInt32 rmToken;          // RM's token (chid only); token = doorbell id << 16 | chid
        bool ready, dead;   // dead = fence timed out, not drained
    };
    VideoChan video_[nvgsp::kVideoEngineCount] = {};
    // kernel-only copy channel (COPY0, chid 8, ADMIN privilege) for
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
    void videoMarkDeadLocked(UInt32 index);
    void videoRunlistDiagLocked(UInt32 index, const char *suffix);
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
    // GOP surface (BAR1+0, first 4 KiB) FNV-1a hash captured at stage
    // time (pre-FWSEC) for the post-boot preservation check.
    UInt64 gopSurfaceHashPre_ = 0;
    bool gopSurfaceHashPreOk_ = false;
    // First 64 bytes (8x LE u64) of BAR1+0 pre-FWSEC, plus the
    // mid-point hash (end of executeBoot) to isolate the clobberer.
    UInt64 gopSurfaceBytesPre_[8] = {};
    UInt64 gopSurfaceHashMid_ = 0;
    bool gopSurfaceHashMidOk_ = false;
    // per-poll BAR1+0 bisect hash index.
    UInt32 bisectPoll_ = 0;
    bool hashBar1Surface(UInt64 *hash);
    bool hashBar1Range(UInt64 offset, unsigned len, UInt64 *hash);
};

// the class name starts with IOAccel because WindowServer's sandbox
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

private:
    UInt32 resetGen_ = 0;   // driver reset generation at open
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
    task_t task_ = nullptr;
    bool admin_ = false;   // non-admin clients get the Metal/video selectors only
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
    static IOReturn accelGo(OSObject *target, void *, IOExternalMethodArguments *arguments);
    NVGspControl *driver_ = nullptr;
    IOBufferMemoryDescriptor *package_ = nullptr;
};
