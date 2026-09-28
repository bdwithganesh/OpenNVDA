// NVDisplay, native IOFramebuffer for the RTX 4080 (AD103).
//
// Phase A display driver. Scans out the surface the GOP left at VRAM 0 (BAR1+0,
// which our C67E window channel now owns) and delivers real hardware vblank
// interrupts from NVGspControl's MSI path (head-0 FE LAST_DATA) to
// WindowServer. Mode setting is still the firmware mode; the nvkms nvEvoC6
// modeset port plugs in behind setDisplayMode later.
//
// Safety: probe() only claims the GPU when the one-shot NVRAM variable
// "nvdisp" is present, and removes it immediately, so a hang during takeover
// is undone by a plain reset (the next boot falls back to IONDRV).
// "nvdisp=persist" keeps the variable for repeated boots once validated.
//
// the EDID (NVGspControl-edid) is parsed in-kernel (NVDisplayEdid.hpp)
// into a timing table published as "NVDisplay-edid-modes". The single
// firmware mode stays the only settable mode, but now carries the EDID's
// real refresh rate, the native flag and full detailed timing
// (getTimingInfoForDisplayMode), so IODisplay/WindowServer see real timing.
//
// HiDPI / scaled modes, why no IOFBScalerInfo (kIOFBScalerInfoKey):
// IOFBScalerInfo tells the OS that the framebuffer has a *hardware* scaler.
// WindowServer then installs detailed timings with horizontalScaled /
// verticalScaled != active via validateDetailedTiming + setDetailedTimings
// and switches to them with setDisplayMode; the driver must then scan out a
// horizontalScaled x verticalScaled surface stretched to the native timing
// (see IONDRVFramebuffer: cscGetScalerInfo / csHorizontalPixels). We cannot
// program the head's scaler or surface size yet (the nvkms modeset port is
// pending), so advertising it would let WindowServer pick modes we must
// either reject or display wrongly. IOFBTransform / kIOFBScalerUnderscan are
// private IOFramebuffer bookkeeping (IOGraphicsTypesPrivate.h), set by
// IOFramebuffer itself from the scaler info, not by subclasses.
// What does work without a hardware scaler: WindowServer's own HiDPI
// rendering on top of the native mode (e.g. "looks like 1920x1080" is a 2x
// backing store at exactly 3840x2160). Those are userland decisions driven by
// the display's EDID (now served correctly via getDDCBlock) and, if needed, a
// display override plist with "scale-resolutions", nothing to set here.
// Non-2x "looks like" sizes additionally need WindowServer to downsample,
// which depends on acceleration, not on this driver.

#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <libkern/OSAtomic.h>

#include "NVDisplayEdid.hpp"

#define kNVDisplayMode ((IODisplayModeID)1)
// CoreDisplay's Framebuffer-construction path asks for mode 0
// explicitly (dtrace 27 Sep: getPixelInformation(0,0,0) under
// IOFBSetDisplayModeAndDepth during CGXMappedDisplayStart); refusing it
// leaves the main display offline and WindowServer exits (Apple logo
// stuck). Accept 0 as an alias of the single native mode.
static inline bool nvModeOK(IODisplayModeID m) { return m == kNVDisplayMode || m == 0; }

class NVDisplay : public IOFramebuffer {
    OSDeclareDefaultStructors(NVDisplay)
public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free(void) override;

    IOReturn enableController(void) override;
    bool isConsoleDevice(void) override;
    IODeviceMemory *getApertureRange(IOPixelAperture aperture) override;
    const char *getPixelFormats(void) override;
    IOItemCount getDisplayModeCount(void) override;
    IOReturn getDisplayModes(IODisplayModeID *allDisplayModes) override;
    IOReturn getInformationForDisplayMode(IODisplayModeID displayMode,
                                          IODisplayModeInformation *info) override;
    IOReturn getTimingInfoForDisplayMode(IODisplayModeID displayMode,
                                         IOTimingInformation *info) override;
    UInt64 getPixelFormatsForDisplayMode(IODisplayModeID displayMode, IOIndex depth) override;
    IOReturn getPixelInformation(IODisplayModeID displayMode, IOIndex depth,
                                 IOPixelAperture aperture, IOPixelInformation *pixelInfo) override;
    IOReturn getCurrentDisplayMode(IODisplayModeID *displayMode, IOIndex *depth) override;
    IOReturn setDisplayMode(IODisplayModeID displayMode, IOIndex depth) override;
    IOItemCount getConnectionCount(void) override;
    IOReturn getAttribute(IOSelect attribute, uintptr_t *value) override;
    IOReturn setAttribute(IOSelect attribute, uintptr_t value) override;
    // Power management, ported from IONDRVFramebuffer::initForPM (sleep 0 /
    // doze 1 / wake 2) so display sleep reaches kIOPowerAttribute.
    unsigned long maxCapabilityForDomainState(IOPMPowerFlags domainState) override;
    unsigned long initialPowerStateForDomainState(IOPMPowerFlags domainState) override;
    unsigned long powerStateForDomainState(IOPMPowerFlags domainState) override;
    IOReturn setCursorImage(void *cursorImage) override;
    IOReturn setCursorState(SInt32 x, SInt32 y, bool visible) override;
    IOReturn getAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
                                       uintptr_t *value) override;
    IOReturn setAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
                                       uintptr_t value) override;
    IOReturn registerForInterruptType(IOSelect interruptType, IOFBInterruptProc proc,
                                      OSObject *target, void *ref, void **interruptRef) override;
    IOReturn unregisterInterrupt(void *interruptRef) override;
    IOReturn setInterruptState(void *interruptRef, UInt32 state) override;
    bool hasDDCConnect(IOIndex connectIndex) override;
    IOReturn getDDCBlock(IOIndex connectIndex, UInt32 blockNumber, IOSelect blockType,
                         IOOptionBits options, UInt8 *data, IOByteCount *length) override;

private:
    static void onVblank(void *ref, UInt32 count, UInt64 uptimeAbs);
    void evictIondrv();
    // EDID read by NVGspControl over DP AUX (published as
    // "NVGspControl-edid" once the GSP chain ends, i.e. after we attached).
    static void edidCallout(thread_call_param_t self, thread_call_param_t);
    static void vblWatchCallout(thread_call_param_t self, thread_call_param_t);
    bool fetchEdid();
    // Parsed EDID timing table (IOMalloc'd, published once via CAS).
    void parseEdid(OSData *edid);
    const nvedid::Timing *nativeTiming();
    // mode 1 is the firmware mode; 0x100 + i is EDID timing i
    // when head 0 can drive it (progressive, fits the desktop surface, and
    // under the 4-lane HBR2 limit at 30 bpp).
    const nvedid::Timing *modeTiming(IODisplayModeID mode);
    bool extraModeOK(uint32_t index);
    UInt32 fBootW = 0, fBootH = 0;
    // NVRAM "nvdisp-default-mode" = "WxH" (first progressive EDID
    // timing of that size) moves kDisplayModeDefaultFlag off the firmware
    // mode, so WindowServer switches at login. Unset = firmware mode.
    IODisplayModeID fDefaultMode = 0;
    void pickDefaultMode();
    nvedid::Info *fEdidInfo = nullptr;
    bool fRefreshOverride = false;
    OSData *fEdid = nullptr;
    // Hardware cursor through NVGspControl (C67A PIO + core CONTROL_CURSOR).
    // Opt-in via NVRAM "nvdisp-hwcursor" until proven, since a silently broken
    // HW cursor would leave the user with no pointer.
    bool hwCursorAllowed();
    int fHwCursor = -1;        // -1 unknown, 0 off, 1 on
    bool fCursorVisible = false;
    UInt32 *fCursorBuf = nullptr;
    IOReturn gspCall(const char *fn, void *a, void *b, void *c);
    void setDpms(bool on);
    // IONDRVFramebuffer::ndrvSetPowerState port, the subclass must send
    // handleEvent(WillPowerOff/WillSleep/DidWake/DidPowerOn) itself; without
    // kIOFBNotifyDidWake IOFramebuffer's pagingState stays false after system
    // sleep and every WindowServer call sleeps forever in _extEntry.
    IOReturn nvSetPowerState(UInt32 newState);
    UInt32 fPowerState = 2;
    UInt32 fPowerChanges = 0;
    void initForPM();
    bool fPMInited = false;
    // hotplug from NVGspControl (GSP NV2080_NOTIFIERS_HOTPLUG).
    static void onHotplug(void *ref, UInt32 plugMask, UInt32 unplugMask);
    bool fHotplugRegistered = false;
    UInt32 fHotplugTries = 0;
    void registerHotplug();
    UInt32 fHotplugs = 0;
    int fDpms = -1;
    thread_call_t fEdidCall = nullptr;
    UInt32 fEdidPolls = 0;
    bool fEdidFired = false;
    // EDID cached in NVRAM (nvdisp-edid) from the previous boot, served from
    // start so WindowServer never re-probes when the live EDID arrives.
    bool fEdidCached = false;
    void saveEdidToNvram(OSData *edid);
    IOFBInterruptProc fConnProc = nullptr;
    OSObject *fConnTarget = nullptr;
    void *fConnRef = nullptr;
    bool attachVblank();
    void detachVblank();

    IOPCIDevice *fPCI = nullptr;
    IODeviceMemory *fAperture = nullptr;
    UInt32 fWidth = 0, fHeight = 0, fRowBytes = 0;
    UInt32 fRefresh = 60;
    // track the current mode (0 is an accepted alias, so report back
    // whatever was set rather than a constant).
    IODisplayModeID fCurMode = kNVDisplayMode;
    IOIndex fCurDepth = 0;
    // VBL client (one; IOFramebuffer registers a single VBL handler).
    IOFBInterruptProc fVblProc = nullptr;
    OSObject *fVblTarget = nullptr;
    void *fVblRef = nullptr;
    volatile bool fVblEnabled = false;
    IOService *fGsp = nullptr;
    bool fVblAttached = false;
    volatile UInt32 fVblCount = 0;
    UInt32 fVblDelivered = 0;
    // synthetic-vblank watchdog (timer keeps CoreDisplay alive when GSP
    // vblanks stall, e.g. dead GSP after S3 while the re-boot runs).
    thread_call_t fVblWatchCall = nullptr;
    UInt32 fVblWatchLast = 0, fVblWatchStall = 0, fVblSynthetic = 0;
};

OSDefineMetaClassAndStructors(NVDisplay, IOFramebuffer)

// CoreDisplay works out the GPU vendor from the framebuffer's class
// name (GetGPUVendorForFramebufferService: "Intel", "AMD", "AppleParavirt",
// "NVDA"). "NVDisplay" is none of them, so the vendor came back unknown,
// gpuVendors ended up 0 and UseIOPresentment() said no: WindowServer never
// used our display pipe. The personality instantiates this subclass; code
// and matching on NVDisplay see it as one.
class NVDA : public NVDisplay {
    OSDeclareDefaultStructors(NVDA)
};
OSDefineMetaClassAndStructors(NVDA, NVDisplay)

IOService *NVDisplay::probe(IOService *provider, SInt32 *score) {
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci || pci->configRead16(kIOPCIConfigVendorID) != 0x10DE ||
        pci->configRead16(kIOPCIConfigDeviceID) != 0x2704)
        return nullptr;
    // NVRAM via the public /options registry entry (IODTNVRAM); the PE*NVRAM
    // helpers are not exported to Auxiliary-KC kexts.
    IORegistryEntry *options = IORegistryEntry::fromPath("/options", gIODTPlane);
    OSData *gate = options ? OSDynamicCast(OSData, options->getProperty("nvdisp")) : nullptr;
    if (!gate) {
        IOLog("NVDisplay: nvdisp not set, leaving the GPU to IONDRV\n");
        OSSafeReleaseNULL(options);
        return nullptr;
    }
    const bool persist = gate->getLength() >= 7 &&
        !memcmp(gate->getBytesNoCopy(), "persist", 7);
    if (!persist) options->removeProperty("nvdisp");   // one-shot
    OSSafeReleaseNULL(options);
    IOLog("NVDisplay: takeover armed (%s)\n", persist ? "persist" : "one-shot");
    return IOFramebuffer::probe(provider, score);
}

bool NVDisplay::start(IOService *provider) {
    fPCI = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCI) return false;
    PE_Video info;
    bzero(&info, sizeof(info));
    IOPlatformExpert *platform = getPlatform();
    if (!platform) return false;
    platform->getConsoleInfo(&info);
    fWidth = static_cast<UInt32>(info.v_width);
    fHeight = static_cast<UInt32>(info.v_height);
    fRowBytes = static_cast<UInt32>(info.v_rowBytes);
    fBootW = fWidth;
    fBootH = fHeight;
    if (!fWidth || !fHeight || !fRowBytes || info.v_depth != 32) {
        IOLog("NVDisplay: unusable console %ux%u depth %lu\n", fWidth, fHeight,
              static_cast<unsigned long>(info.v_depth));
        return false;
    }
    // NVGspControl may have resized BAR1 and moved it above DRAM (its own
    // ReBAR; it runs first from the Boot KC, we're in the Aux KC). It also
    // points the kernel console somewhere new, so the console base is either
    // inside the new BAR1 (moved) or inside IOPCIFamily's BAR1 (not moved).
    IODeviceMemory *bar1 = fPCI->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress1);
    if (!bar1) return false;
    UInt64 gspPhys = 0, gspBytes = 0;
    {
        OSDictionary *match = serviceMatching("NVGspControl");
        IOService *gsp = match ? waitForMatchingService(match, 2000ULL * 1000 * 1000) : nullptr;
        OSSafeReleaseNULL(match);
        OSNumber *phys = gsp ? OSDynamicCast(OSNumber, gsp->getProperty("NVGspControl-bar1-phys")) : nullptr;
        OSNumber *bytes = gsp ? OSDynamicCast(OSNumber, gsp->getProperty("NVGspControl-bar1-bytes")) : nullptr;
        if (phys && bytes) { gspPhys = phys->unsigned64BitValue(); gspBytes = bytes->unsigned64BitValue(); }
        OSSafeReleaseNULL(gsp);
    }
    const IOPhysicalAddress bar1Start = bar1->getPhysicalAddress();
    const IOPhysicalAddress base =
        static_cast<IOPhysicalAddress>(info.v_baseAddr & ~static_cast<IOPhysicalAddress>(3));
    const IOByteCount visible = static_cast<IOByteCount>(fHeight) * fRowBytes;
    const bool moved = gspPhys && gspBytes && gspPhys != bar1Start;
    if (moved && base >= gspPhys && base + visible <= gspPhys + gspBytes) {
        IODeviceMemory *nb = IODeviceMemory::withRange(gspPhys, gspBytes);
        fAperture = nb ? IODeviceMemory::withSubRange(nb, base - gspPhys, visible) : nullptr;
        OSSafeReleaseNULL(nb);
        IOLog("NVDisplay: BAR1 moved by NVGspControl to 0x%llx, console at +0x%llx\n",
              static_cast<unsigned long long>(gspPhys), static_cast<unsigned long long>(base - gspPhys));
    } else if (base >= bar1Start && base + visible <= bar1Start + bar1->getLength()) {
        if (moved) {
            // BAR1 moved but the console was not re-pointed: follow it here
            const IOPhysicalAddress off = base - bar1Start;
            IODeviceMemory *nb = IODeviceMemory::withRange(gspPhys, gspBytes);
            fAperture = nb ? IODeviceMemory::withSubRange(nb, off, visible) : nullptr;
            OSSafeReleaseNULL(nb);
            info.v_baseAddr = (gspPhys + off) | (info.v_baseAddr & 3);
            platform->setConsoleInfo(&info, kPEBaseAddressChange);
        } else {
            fAperture = IODeviceMemory::withSubRange(bar1, base - bar1Start, visible);
        }
    } else {
        IOLog("NVDisplay: console 0x%llx outside BAR1\n", static_cast<unsigned long long>(base));
        return false;
    }
    if (!fAperture) return false;
    UInt32 hz = 0;
    if (PE_parse_boot_argn("nvdisp-hz", &hz, sizeof(hz)) && hz >= 24 && hz <= 240) {
        fRefresh = hz;             // explicit override beats the EDID rate
        fRefreshOverride = true;
    }
    IOLog("NVDisplay: %ux%u rowBytes %u console 0x%llx %u Hz\n", fWidth, fHeight, fRowBytes,
          static_cast<unsigned long long>(base), fRefresh);
    if (!IOFramebuffer::start(provider)) {
        OSSafeReleaseNULL(fAperture);
        return false;
    }
    setProperty("NVDisplay-version", "0.8.2");
    // IONDRVFramebuffer (System KC, category IOFramebuffer) attaches to the
    // same PCI device long before this Aux KC kext loads, so we ended up
    // with two live framebuffers. Kick it out so we're the only one.
    evictIondrv();
    setProperty("NVDisplay-mode", (UInt64(fWidth) << 32) | fHeight, 64);
    fEdidCall = thread_call_allocate(&NVDisplay::edidCallout, this);
    {
        IORegistryEntry *options = IORegistryEntry::fromPath("/options", gIODTPlane);
        OSData *cached = options ? OSDynamicCast(OSData, options->getProperty("nvdisp-edid")) : nullptr;
        const UInt8 hdr[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
        if (cached && (cached->getLength() == 128 || cached->getLength() == 256) &&
            !memcmp(cached->getBytesNoCopy(), hdr, 8)) {
            OSData *copy = OSData::withData(cached);
            if (copy) {
                parseEdid(copy);
                fEdid = copy;
                fEdidCached = true;
                setProperty("NVDisplay-edid-bytes", copy->getLength(), 32);
                setProperty("NVDisplay-edid-cached", true);
            }
        }
        OSSafeReleaseNULL(options);
    }
    if (fEdidCached && fEdidCall) {
        uint64_t deadline = 0;
        clock_interval_to_deadline(2000, kMillisecondScale, &deadline);
        thread_call_enter_delayed(fEdidCall, deadline);
    } else if (!fetchEdid() && fEdidCall) {
        uint64_t deadline = 0;
        clock_interval_to_deadline(2000, kMillisecondScale, &deadline);
        thread_call_enter_delayed(fEdidCall, deadline);
    }
    // permanent 500 ms vblank watchdog (see vblWatchCallout).
    fVblWatchCall = thread_call_allocate(&NVDisplay::vblWatchCallout, this);
    if (fVblWatchCall) {
        uint64_t deadline = 0;
        clock_interval_to_deadline(500, kMillisecondScale, &deadline);
        thread_call_enter_delayed(fVblWatchCall, deadline);
    }
    setProperty("NVDisplay-vblank-watch", fVblWatchCall != nullptr);
    return true;
}

void NVDisplay::stop(IOService *provider) {
    if (fEdidCall) thread_call_cancel(fEdidCall);
    if (fVblWatchCall) thread_call_cancel(fVblWatchCall);
    detachVblank();
    IOFramebuffer::stop(provider);
}

void NVDisplay::free(void) {
    if (fEdidCall) {
        thread_call_cancel(fEdidCall);
        thread_call_free(fEdidCall);
        fEdidCall = nullptr;
    }
    if (fVblWatchCall) {
        thread_call_cancel(fVblWatchCall);
        thread_call_free(fVblWatchCall);
        fVblWatchCall = nullptr;
    }
    OSSafeReleaseNULL(fEdid);
    if (fCursorBuf) { IOFree(fCursorBuf, 64 * 64 * 4 * 2); fCursorBuf = nullptr; }
    if (fEdidInfo) {
        IOFree(fEdidInfo, sizeof(*fEdidInfo));
        fEdidInfo = nullptr;
    }
    detachVblank();
    OSSafeReleaseNULL(fAperture);
    IOFramebuffer::free();
}

void NVDisplay::evictIondrv() {
    OSIterator *it = fPCI ? fPCI->getChildIterator(gIOServicePlane) : nullptr;
    UInt32 evicted = 0;
    if (it) {
        while (IORegistryEntry *child = OSDynamicCast(IORegistryEntry, it->getNextObject())) {
            if (child == this) continue;
            const OSMetaClass *mc = child->getMetaClass();
            const char *name = mc ? mc->getClassName() : nullptr;
            IOService *svc = OSDynamicCast(IOService, child);
            if (svc && name && !strcmp(name, "IONDRVFramebuffer")) {
                IOLog("NVDisplay: terminating IONDRVFramebuffer %s\n", svc->getName());
                if (svc->terminate(kIOServiceRequired)) ++evicted;
            }
        }
        it->release();
    }
    setProperty("NVDisplay-iondrv-evicted", evicted, 32);
}

bool NVDisplay::fetchEdid() {
    if (fEdid) return true;
    OSDictionary *match = serviceMatching("NVGspControl");
    IOService *gsp = match ? copyMatchingService(match) : nullptr;
    OSSafeReleaseNULL(match);
    OSData *edid = gsp ? OSDynamicCast(OSData, gsp->getProperty("NVGspControl-edid")) : nullptr;
    const UInt8 hdr[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    if (edid && edid->getLength() >= 128 && !memcmp(edid->getBytesNoCopy(), hdr, 8)) {
        // Timing table first, so fEdid != nullptr implies the parse is done.
        parseEdid(edid);
        edid->retain();
        // FetchEdid runs from start, the EDID thread_call and IOFramebuffer
        // callbacks; publish once.
        if (OSCompareAndSwapPtr(nullptr, edid, reinterpret_cast<void * volatile *>(&fEdid)))
            setProperty("NVDisplay-edid-bytes", edid->getLength(), 32);
        else
            edid->release();
    }
    OSSafeReleaseNULL(gsp);
    return fEdid != nullptr;
}

static void dictSetNumber(OSDictionary *dict, const char *key, UInt64 value) {
    OSNumber *num = OSNumber::withNumber(value, 64);
    if (!num) return;
    dict->setObject(key, num);
    num->release();
}

void NVDisplay::parseEdid(OSData *edid) {
    if (fEdidInfo || !edid) return;
    nvedid::Info *info = static_cast<nvedid::Info *>(IOMalloc(sizeof(nvedid::Info)));
    if (!info) return;
    const bool ok = nvedid::parse(static_cast<const uint8_t *>(edid->getBytesNoCopy()),
                                  static_cast<uint32_t>(edid->getLength()), *info);
    setProperty("NVDisplay-edid-parsed", ok);
    if (!ok || !OSCompareAndSwapPtr(nullptr, info,
                                    reinterpret_cast<void * volatile *>(&fEdidInfo))) {
        IOFree(info, sizeof(nvedid::Info));
        return;
    }
    // Debug view of the timing table.
    OSArray *modes = OSArray::withCapacity(info->count);
    for (UInt32 i = 0; modes && i < info->count; ++i) {
        const nvedid::Timing &t = info->timings[i];
        OSDictionary *d = OSDictionary::withCapacity(12);
        if (!d) break;
        d->setObject("source", OSSymbol::withCStringNoCopy(
            t.source == nvedid::kSourceBaseDtd ? "base-dtd" :
            t.source == nvedid::kSourceCtaDtd ? "cta-dtd" : "standard"));
        dictSetNumber(d, "width", t.hActive);
        dictSetNumber(d, "height", t.vActive);
        dictSetNumber(d, "refresh-millihz", t.refreshMilliHz);
        dictSetNumber(d, "flags", t.flags);
        if (t.flags & nvedid::kFlagDetailed) {
            dictSetNumber(d, "pixel-clock-khz", t.pixelClockKHz);
            dictSetNumber(d, "htotal", t.hActive + t.hBlank);
            dictSetNumber(d, "vtotal", t.vActive + t.vBlank);
            dictSetNumber(d, "hsync-offset", t.hSyncOffset);
            dictSetNumber(d, "hsync-width", t.hSyncWidth);
            dictSetNumber(d, "vsync-offset", t.vSyncOffset);
            dictSetNumber(d, "vsync-width", t.vSyncWidth);
        }
        modes->setObject(d);
        d->release();
    }
    if (modes) {
        setProperty("NVDisplay-edid-modes", modes);
        modes->release();
    }
    OSDictionary *sum = OSDictionary::withCapacity(10);
    if (sum) {
        OSString *name = OSString::withCString(info->name);
        if (name) {
            sum->setObject("name", name);
            name->release();
        }
        dictSetNumber(sum, "version", (UInt32(info->version) << 8) | info->revision);
        dictSetNumber(sum, "extensions", info->extensionCount);
        dictSetNumber(sum, "extensions-parsed", info->extensionsParsed);
        dictSetNumber(sum, "bad-checksums", info->badChecksums);
        dictSetNumber(sum, "preferred", static_cast<UInt64>(static_cast<SInt64>(info->preferred)));
        if (info->hasRange) {
            dictSetNumber(sum, "min-vhz", info->minVHz);
            dictSetNumber(sum, "max-vhz", info->maxVHz);
            dictSetNumber(sum, "max-pixel-clock-mhz", info->maxPixelClockMHz);
        }
        setProperty("NVDisplay-edid-info", sum);
        sum->release();
    }
    const nvedid::Timing *native = nativeTiming();
    if (native)
        IOLog("NVDisplay: EDID \"%s\" native %ux%u pclk %u kHz total %ux%u %u mHz\n",
              info->name, native->hActive, native->vActive, native->pixelClockKHz,
              native->hActive + native->hBlank, native->vActive + native->vBlank,
              native->refreshMilliHz);
    else
        IOLog("NVDisplay: EDID \"%s\" has no detailed timing for %ux%u\n", info->name,
              fWidth, fHeight);
    setProperty("NVDisplay-edid-native-timing", native != nullptr);
    pickDefaultMode();
}

// The EDID detailed timing for the firmware mode's active size (preferred
// DTD first). Matched on active size only: we cannot yet read the head's
// programmed timing back, and the GOP drives the panel's preferred timing.
const nvedid::Timing *NVDisplay::nativeTiming() {
    nvedid::Info *info = fEdidInfo;
    if (!info) return nullptr;
    const int32_t idx = nvedid::findDetailed(*info, fBootW, fBootH);
    return idx >= 0 ? &info->timings[idx] : nullptr;
}

static constexpr IODisplayModeID kNVExtraModeBase = 0x100;

bool NVDisplay::extraModeOK(uint32_t index) {
    nvedid::Info *info = fEdidInfo;
    if (!info || index >= info->count) return false;
    const nvedid::Timing &t = info->timings[index];
    if (!(t.flags & nvedid::kFlagDetailed) || (t.flags & nvedid::kFlagInterlaced)) return false;
    if (t.hActive > fBootW || t.vActive > fBootH || t.hActive <= 60) return false;
    // 30 bpp on HBR2 x4 caps the clock at 576 MHz; the VPLL post divider
    // (P <= 15, VCO >= 1.4 GHz) floors it at ~93.4 MHz.
    if (t.pixelClockKHz < 93400 || t.pixelClockKHz >= 576000) return false;
    const nvedid::Timing *native = nativeTiming();
    return &t != native;
}

const nvedid::Timing *NVDisplay::modeTiming(IODisplayModeID mode) {
    if (nvModeOK(mode)) return nativeTiming();
    if (mode < kNVExtraModeBase) return nullptr;
    const uint32_t i = static_cast<uint32_t>(mode - kNVExtraModeBase);
    return extraModeOK(i) ? &fEdidInfo->timings[i] : nullptr;
}

void NVDisplay::saveEdidToNvram(OSData *edid) {
    if (!edid) return;
    IORegistryEntry *options = IORegistryEntry::fromPath("/options", gIODTPlane);
    if (options) {
        OSData *cur = OSDynamicCast(OSData, options->getProperty("nvdisp-edid"));
        if (!cur || !cur->isEqualTo(edid)) {
            const bool ok = options->setProperty("nvdisp-edid", edid);
            setProperty("NVDisplay-edid-saved", ok);
        }
        options->release();
    }
}

void NVDisplay::edidCallout(thread_call_param_t param, thread_call_param_t) {
    NVDisplay *self = static_cast<NVDisplay *>(param);
    self->setProperty("NVDisplay-edid-polls", self->fEdidPolls, 32);
    // Hotplug arming fails until NVGspControl's chain has its persistent client
    // (~9 min); retry from this thread context (RPCs are not allowed from the
    // vblank/MSI path).
    self->registerHotplug();
    // Verify the NVRAM-cached EDID against the live one (no connect
    // interrupt when equal, no WindowServer re-probe / screen refresh).
    if (self->fEdidCached) {
        OSDictionary *match = serviceMatching("NVGspControl");
        IOService *gsp = match ? copyMatchingService(match) : nullptr;
        OSSafeReleaseNULL(match);
        OSData *live = gsp ? OSDynamicCast(OSData, gsp->getProperty("NVGspControl-edid")) : nullptr;
        if (live && live->getLength() >= 128) {
            const bool same = self->fEdid && self->fEdid->isEqualTo(live);
            self->setProperty("NVDisplay-edid-cache-hit", same);
            self->fEdidCached = false;
            if (!same) {
                OSData *old = self->fEdid;
                nvedid::Info *oldInfo = self->fEdidInfo;
                self->fEdid = nullptr;
                self->fEdidInfo = nullptr;
                OSSafeReleaseNULL(old);
                if (oldInfo) IOFree(oldInfo, sizeof(nvedid::Info));
                self->fEdidFired = false;
                self->fEdidPolls = 0;
                OSSafeReleaseNULL(gsp);
                // fall through to the normal fetch + connect path below
            } else {
                self->fEdidFired = true;
                OSSafeReleaseNULL(gsp);
                if (!self->fHotplugRegistered && ++self->fEdidPolls < 600) {
                    uint64_t deadline = 0;
                    clock_interval_to_deadline(2000, kMillisecondScale, &deadline);
                    thread_call_enter_delayed(self->fEdidCall, deadline);
                }
                return;
            }
        } else {
            OSSafeReleaseNULL(gsp);
            if (++self->fEdidPolls < 600) {
                uint64_t deadline = 0;
                clock_interval_to_deadline(2000, kMillisecondScale, &deadline);
                thread_call_enter_delayed(self->fEdidCall, deadline);
            }
            return;
        }
    }
    if (self->fEdidFired) {
        if (!self->fHotplugRegistered && ++self->fEdidPolls < 600) {
            uint64_t deadline = 0;
            clock_interval_to_deadline(2000, kMillisecondScale, &deadline);
            thread_call_enter_delayed(self->fEdidCall, deadline);
        }
        return;
    }
    if (self->fetchEdid()) {
        self->fEdidFired = true;
        self->saveEdidToNvram(self->fEdid);
        // Connection changed: IOFramebuffer re-probes and IODisplay re-reads
        // the EDID through getDDCBlock.
        self->setProperty("NVDisplay-edid-connect-fired", self->fConnProc != nullptr);
        if (self->fConnProc) self->fConnProc(self->fConnTarget, self->fConnRef);
        if (self->fHotplugRegistered) return;
    }
    if (++self->fEdidPolls < 600) {
        uint64_t deadline = 0;
        clock_interval_to_deadline(2000, kMillisecondScale, &deadline);
        thread_call_enter_delayed(self->fEdidCall, deadline);
    }
}

// Vblank watchdog, runs every 500 ms forever. If the client enabled vblank
// but the GSP count has not advanced for 2+ ticks (dead GSP after S3, slow
// re-boot), deliver a synthetic tick so CoreDisplay's VBL waits complete
// instead of wedging WindowServer into a watchdog panic. Real vblanks
// automatically preempt (the stall counter resets on any advance); during
// S3 the callout is frozen with the CPU, so no ticks fire while asleep.
void NVDisplay::vblWatchCallout(thread_call_param_t param, thread_call_param_t) {
    NVDisplay *self = static_cast<NVDisplay *>(param);
    const UInt32 count = self->fVblCount;
    if (self->fVblEnabled && self->fVblProc) {
        if (count == self->fVblWatchLast) {
            if (++self->fVblWatchStall >= 2) {
                self->fVblProc(self->fVblTarget, self->fVblRef);
                ++self->fVblSynthetic;
                self->setProperty("NVDisplay-vblank-synthetic", self->fVblSynthetic, 32);
            }
        } else {
            self->fVblWatchLast = count;
            self->fVblWatchStall = 0;
        }
    } else {
        self->fVblWatchLast = count;
        self->fVblWatchStall = 0;
    }
    uint64_t deadline = 0;
    clock_interval_to_deadline(500, kMillisecondScale, &deadline);
    thread_call_enter_delayed(self->fVblWatchCall, deadline);
}

bool NVDisplay::hasDDCConnect(IOIndex connectIndex) {
    return connectIndex == 0 && fetchEdid();
}

IOReturn NVDisplay::getDDCBlock(IOIndex connectIndex, UInt32 blockNumber, IOSelect blockType,
                                IOOptionBits, UInt8 *data, IOByteCount *length) {
    if (connectIndex != 0 || blockType != kIODDCBlockTypeEDID || !data || !length ||
        !blockNumber || !fetchEdid())
        return kIOReturnUnsupported;
    const size_t off = (blockNumber - 1) * 128;
    if (off + 128 > fEdid->getLength()) return kIOReturnUnsupported;
    const size_t n = *length < 128 ? *length : 128;
    memcpy(data, static_cast<const UInt8 *>(fEdid->getBytesNoCopy()) + off, n);
    *length = n;
    return kIOReturnSuccess;
}

// ---- vblank from NVGspControl ------------------------------------------------------------------
// NVGspControl calls onVblank from its MSI workloop on every head-0 vblank
// (FE_RM_INTR_STAT_HEAD_TIMING LAST_DATA). No link dependency: the callback is registered through
// callPlatformFunction.

void NVDisplay::onVblank(void *ref, UInt32 count, UInt64) {
    NVDisplay *self = static_cast<NVDisplay *>(ref);
    self->fVblCount = count;
    // backup EDID trigger from the vblank path (the 2 s thread_call alone
    // never picked it up)
    if (!self->fEdidFired && self->fEdidCall && (count & 127) == 0)
        thread_call_enter(self->fEdidCall);
    if (self->fVblEnabled && self->fVblProc) {
        self->fVblProc(self->fVblTarget, self->fVblRef);
        ++self->fVblDelivered;
    }
}

void NVDisplay::onHotplug(void *ref, UInt32 plugMask, UInt32 unplugMask) {
    NVDisplay *self = static_cast<NVDisplay *>(ref);
    ++self->fHotplugs;
    self->setProperty("NVDisplay-hotplugs", self->fHotplugs, 32);
    self->setProperty("NVDisplay-hotplug-last", (UInt64(plugMask) << 32) | unplugMask, 64);
    // Drop the cached EDID; the callout re-fetches it (NVGspControl re-read it
    // over AUX before calling us on a plug) and raises the connect interrupt.
    OSData *old = self->fEdid;
    self->fEdid = nullptr;
    OSSafeReleaseNULL(old);
    self->fEdidFired = false;
    self->fEdidPolls = 0;
    if (self->fEdidCall) thread_call_enter(self->fEdidCall);
}

void NVDisplay::registerHotplug() {
    if (fHotplugRegistered) return;
    fHotplugRegistered = gspCall("nvgsp-hotplug-register",
        reinterpret_cast<void *>(&NVDisplay::onHotplug), this, nullptr) == kIOReturnSuccess;
    ++fHotplugTries;
    setProperty("NVDisplay-hotplug-registered", fHotplugRegistered);
    setProperty("NVDisplay-hotplug-tries", fHotplugTries, 32);
}

bool NVDisplay::attachVblank() {
    registerHotplug();
    if (fVblAttached) return true;
    if (!fGsp) {
        OSDictionary *match = serviceMatching("NVGspControl");
        if (match) {
            fGsp = copyMatchingService(match);
            match->release();
        }
    }
    if (!fGsp) return false;
    const OSSymbol *sym = OSSymbol::withCString("nvgsp-vblank-register");
    const IOReturn r = sym ? fGsp->callPlatformFunction(
        sym, false, reinterpret_cast<void *>(&NVDisplay::onVblank), this, nullptr, nullptr)
        : kIOReturnNoMemory;
    OSSafeReleaseNULL(sym);
    fVblAttached = r == kIOReturnSuccess;
    setProperty("NVDisplay-vblank-attached", fVblAttached);
    return fVblAttached;
}

void NVDisplay::detachVblank() {
    if (fVblAttached && fGsp) {
        const OSSymbol *sym = OSSymbol::withCString("nvgsp-vblank-unregister");
        if (sym)
            fGsp->callPlatformFunction(sym, false,
                reinterpret_cast<void *>(&NVDisplay::onVblank), this, nullptr, nullptr);
        OSSafeReleaseNULL(sym);
    }
    fVblAttached = false;
    OSSafeReleaseNULL(fGsp);
}

IOReturn NVDisplay::registerForInterruptType(IOSelect interruptType, IOFBInterruptProc proc,
                                             OSObject *target, void *ref, void **interruptRef) {
    if (interruptType == kIOFBConnectInterruptType) {
        fConnProc = proc;
        fConnTarget = target;
        fConnRef = ref;
        if (interruptRef) *interruptRef = &fConnProc;
        return kIOReturnSuccess;
    }
    if (interruptType != kIOFBVBLInterruptType)
        return IOFramebuffer::registerForInterruptType(interruptType, proc, target, ref,
                                                        interruptRef);
    if (fVblProc) return kIOReturnBusy;
    fVblTarget = target;
    fVblRef = ref;
    fVblEnabled = true;
    fVblProc = proc;
    if (interruptRef) *interruptRef = &fVblProc;
    attachVblank();
    setProperty("NVDisplay-vbl-registered", true);
    return kIOReturnSuccess;
}

IOReturn NVDisplay::unregisterInterrupt(void *interruptRef) {
    if (interruptRef == &fConnProc) {
        fConnProc = nullptr;
        return kIOReturnSuccess;
    }
    if (interruptRef != &fVblProc) return IOFramebuffer::unregisterInterrupt(interruptRef);
    fVblEnabled = false;
    fVblProc = nullptr;
    return kIOReturnSuccess;
}

IOReturn NVDisplay::setInterruptState(void *interruptRef, UInt32 state) {
    if (interruptRef == &fConnProc) return kIOReturnSuccess;
    if (interruptRef != &fVblProc) return IOFramebuffer::setInterruptState(interruptRef, state);
    fVblEnabled = state != 0;
    if (fVblEnabled) attachVblank();   // NVGspControl may have come up late
    return kIOReturnSuccess;
}

// ---- IOFramebuffer -----------------------------------------------------------------------------

void NVDisplay::initForPM() {
    if (fPMInited) return;
    static IOPMPowerState states[3] = {
        {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {1, 0, 0, IOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0},
        {1, IOPMDeviceUsable, IOPMPowerOn, IOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0},
    };
    registerPowerDriver(this, states, 3);
    temporaryPowerClampOn();
    changePowerStateTo(1);   // not below doze until system sleep (IONDRV)
    if (fPCI) fPCI->setProperty("IOPMIsPowerManaged", true);
    fPMInited = true;
    setProperty("NVDisplay-pm", true);
}

unsigned long NVDisplay::maxCapabilityForDomainState(IOPMPowerFlags domainState) {
    return (domainState & IOPMPowerOn) ? 2 : 0;
}

unsigned long NVDisplay::initialPowerStateForDomainState(IOPMPowerFlags domainState) {
    return (domainState & IOPMPowerOn) ? 2 : 0;
}

unsigned long NVDisplay::powerStateForDomainState(IOPMPowerFlags domainState) {
    return (domainState & IOPMPowerOn) ? getPowerState() : 0;
}

IOReturn NVDisplay::enableController(void) {
    initForPM();
    attachVblank();
    return kIOReturnSuccess;
}

bool NVDisplay::isConsoleDevice(void) { return true; }

IODeviceMemory *NVDisplay::getApertureRange(IOPixelAperture aperture) {
    if (aperture != kIOFBSystemAperture || !fAperture) return nullptr;
    fAperture->retain();
    return fAperture;
}

const char *NVDisplay::getPixelFormats(void) { return IO32BitDirectPixels "\0\0"; }

void NVDisplay::pickDefaultMode() {
    fDefaultMode = 0;
    IORegistryEntry *options = IORegistryEntry::fromPath("/options", gIODTPlane);
    OSData *d = options ? OSDynamicCast(OSData, options->getProperty("nvdisp-default-mode")) : nullptr;
    char want[24] = {};
    if (d && d->getLength() < sizeof(want)) __builtin_memcpy(want, d->getBytesNoCopy(), d->getLength());
    OSSafeReleaseNULL(options);
    if (!want[0] || !fEdidInfo) return;
    UInt32 w = 0, h = 0;
    const char *p = want;
    while (*p >= '0' && *p <= '9') w = w * 10 + static_cast<UInt32>(*p++ - '0');
    if (*p++ != 'x') return;
    while (*p >= '0' && *p <= '9') h = h * 10 + static_cast<UInt32>(*p++ - '0');
    for (uint32_t i = 0; i < fEdidInfo->count; ++i) {
        const nvedid::Timing &t = fEdidInfo->timings[i];
        if (extraModeOK(i) && t.hActive == w && t.vActive == h) {
            fDefaultMode = kNVExtraModeBase + static_cast<IODisplayModeID>(i);
            break;
        }
    }
    setProperty("NVDisplay-default-mode", static_cast<UInt64>(fDefaultMode), 32);
}

IOItemCount NVDisplay::getDisplayModeCount(void) {
    IOItemCount n = 1;
    nvedid::Info *info = fEdidInfo;
    for (uint32_t i = 0; info && i < info->count; ++i) n += extraModeOK(i);
    return n;
}

IOReturn NVDisplay::getDisplayModes(IODisplayModeID *allDisplayModes) {
    if (!allDisplayModes) return kIOReturnBadArgument;
    IOItemCount n = 0;
    allDisplayModes[n++] = kNVDisplayMode;
    nvedid::Info *info = fEdidInfo;
    for (uint32_t i = 0; info && i < info->count; ++i)
        if (extraModeOK(i)) allDisplayModes[n++] = kNVExtraModeBase + static_cast<IODisplayModeID>(i);
    return kIOReturnSuccess;
}

IOReturn NVDisplay::getInformationForDisplayMode(IODisplayModeID displayMode,
                                                 IODisplayModeInformation *info) {
    if (!info) return kIOReturnBadArgument;
    if (!nvModeOK(displayMode)) {
        const nvedid::Timing *t = modeTiming(displayMode);
        if (!t) return kIOReturnBadArgument;
        bzero(info, sizeof(*info));
        info->nominalWidth = t->hActive;
        info->nominalHeight = t->vActive;
        info->refreshRate = nvedid::refreshFixed16(*t);
        info->maxDepthIndex = 0;
        info->flags = kDisplayModeValidFlag | kDisplayModeSafeFlag |
                      (displayMode == fDefaultMode ? kDisplayModeDefaultFlag : 0);
        return kIOReturnSuccess;
    }
    bzero(info, sizeof(*info));
    info->nominalWidth = fBootW;
    info->nominalHeight = fBootH;
    const nvedid::Timing *native = fRefreshOverride ? nullptr : nativeTiming();
    info->refreshRate = native ? nvedid::refreshFixed16(*native) : (fRefresh << 16);
    info->maxDepthIndex = 0;
    info->flags = kDisplayModeValidFlag | kDisplayModeSafeFlag | kDisplayModeNativeFlag |
                  (fDefaultMode ? 0 : kDisplayModeDefaultFlag);
    return kIOReturnSuccess;
}

IOReturn NVDisplay::getTimingInfoForDisplayMode(IODisplayModeID displayMode,
                                                IOTimingInformation *info) {
    if (!info || (!nvModeOK(displayMode) && !modeTiming(displayMode))) return kIOReturnBadArgument;
    bzero(info, sizeof(*info));
    info->appleTimingID = kIOTimingIDInvalid;   // "timingInvalid": not an Apple constant
    const nvedid::Timing *t = modeTiming(displayMode);
    if (!t) return kIOReturnSuccess;            // no EDID yet: no detailed timing
    IODetailedTimingInformationV2 &d = info->detailedInfo.v2;
    d.pixelClock = static_cast<UInt64>(t->pixelClockKHz) * 1000ULL;
    d.minPixelClock = d.pixelClock;
    d.maxPixelClock = d.pixelClock;
    d.horizontalActive = t->hActive;
    d.horizontalBlanking = t->hBlank;
    d.horizontalSyncOffset = t->hSyncOffset;
    d.horizontalSyncPulseWidth = t->hSyncWidth;
    d.verticalActive = t->vActive;
    d.verticalBlanking = t->vBlank;
    d.verticalSyncOffset = t->vSyncOffset;
    d.verticalSyncPulseWidth = t->vSyncWidth;
    // Borders are 0 (bzero); DTD borders are not used by DisplayPort sinks.
    d.horizontalSyncConfig = (t->flags & nvedid::kFlagHSyncPositive) ? kIOSyncPositivePolarity : 0;
    d.verticalSyncConfig = (t->flags & nvedid::kFlagVSyncPositive) ? kIOSyncPositivePolarity : 0;
    d.signalConfig = kIODigitalSignal;
    info->flags = kIODetailedTimingValid;
    return kIOReturnSuccess;
}

UInt64 NVDisplay::getPixelFormatsForDisplayMode(IODisplayModeID, IOIndex) { return 0; }

IOReturn NVDisplay::getPixelInformation(IODisplayModeID displayMode, IOIndex depth,
                                        IOPixelAperture aperture, IOPixelInformation *pixelInfo) {
    const nvedid::Timing *mt = nvModeOK(displayMode) ? nullptr : modeTiming(displayMode);
    if ((!nvModeOK(displayMode) && !mt) || depth != 0 || aperture != kIOFBSystemAperture ||
        !pixelInfo)
        return kIOReturnBadArgument;
    bzero(pixelInfo, sizeof(*pixelInfo));
    pixelInfo->bytesPerRow = fRowBytes;
    pixelInfo->bitsPerPixel = 32;
    pixelInfo->pixelType = kIORGBDirectPixels;
    pixelInfo->componentCount = 3;
    pixelInfo->bitsPerComponent = 8;
    pixelInfo->componentMasks[0] = 0x00FF0000;
    pixelInfo->componentMasks[1] = 0x0000FF00;
    pixelInfo->componentMasks[2] = 0x000000FF;
    pixelInfo->activeWidth = mt ? mt->hActive : fBootW;
    pixelInfo->activeHeight = mt ? mt->vActive : fBootH;
    const char *src = IO32BitDirectPixels;
    size_t i = 0;
    while (src[i] && i < sizeof(pixelInfo->pixelFormat) - 1) {
        pixelInfo->pixelFormat[i] = src[i];
        ++i;
    }
    pixelInfo->pixelFormat[i] = '\0';
    return kIOReturnSuccess;
}

IOReturn NVDisplay::getCurrentDisplayMode(IODisplayModeID *displayMode, IOIndex *depth) {
    if (displayMode) *displayMode = fCurMode;
    if (depth) *depth = fCurDepth;
    return kIOReturnSuccess;
}

// a different mode is a real modeset on head 0 through
// NVGspControl (nvgsp-set-mode: head methods, DP stream, VPLL, window size).
// Mode 0/1 back to the firmware timing uses the same path once we left it.
IOReturn NVDisplay::setDisplayMode(IODisplayModeID displayMode, IOIndex depth) {
    if (depth != 0) return kIOReturnUnsupported;
    const nvedid::Timing *t = modeTiming(displayMode);
    if (!nvModeOK(displayMode) && !t) return kIOReturnUnsupported;
    const bool same = (nvModeOK(displayMode) && nvModeOK(fCurMode)) || displayMode == fCurMode;
    if (!same) {
        if (!t) return kIOReturnUnsupported;   // no EDID timing to go back to
        UInt32 tv[10] = {t->pixelClockKHz, t->hActive, t->hBlank, t->hSyncOffset, t->hSyncWidth,
                         t->vActive, t->vBlank, t->vSyncOffset, t->vSyncWidth,
                         ((t->flags & nvedid::kFlagHSyncPositive) ? 1U : 0U) |
                         ((t->flags & nvedid::kFlagVSyncPositive) ? 2U : 0U)};
        UInt32 code = ~0U;
        const IOReturn r = gspCall("nvgsp-set-mode", tv, &code, nullptr);
        setProperty("NVDisplay-setmode-result", static_cast<UInt32>(r), 32);
        setProperty("NVDisplay-setmode-code", code, 32);
        IOLog("NVDisplay: mode 0x%x %ux%u pclk %u kHz -> 0x%x code 0x%x\n",
              static_cast<unsigned>(displayMode), t->hActive, t->vActive, t->pixelClockKHz,
              r, code);
        if (r != kIOReturnSuccess) return kIOReturnIOError;
        fWidth = t->hActive;
        fHeight = t->vActive;
        setProperty("NVDisplay-mode", (UInt64(fWidth) << 32) | fHeight, 64);
    }
    fCurMode = displayMode;
    fCurDepth = depth;
    return kIOReturnSuccess;
}

IOItemCount NVDisplay::getConnectionCount(void) { return 1; }

IOReturn NVDisplay::gspCall(const char *fn, void *a, void *b, void *c) {
    OSDictionary *match = serviceMatching("NVGspControl");
    IOService *gsp = match ? copyMatchingService(match) : nullptr;
    OSSafeReleaseNULL(match);
    const OSSymbol *sym = OSSymbol::withCString(fn);
    const IOReturn r = (gsp && sym) ? gsp->callPlatformFunction(sym, false, a, b, c, nullptr)
                                    : kIOReturnNotReady;
    OSSafeReleaseNULL(sym);
    OSSafeReleaseNULL(gsp);
    return r;
}

IOReturn NVDisplay::nvSetPowerState(UInt32 newState) {
    if (newState > 2) newState = 2;
    if (newState == fPowerState) return kIOReturnSuccess;
    const UInt32 oldState = fPowerState;
    IOIndex postEvent = 0;
    if (oldState == 2) {
        IOFramebuffer::handleEvent(kIOFBNotifyWillPowerOff);
        postEvent = kIOFBNotifyDidPowerOff;
        setDpms(false);   // display sleep (doze) and system sleep: sink D3
    } else if (newState == 2) {
        IOFramebuffer::handleEvent(kIOFBNotifyWillPowerOn);
        postEvent = kIOFBNotifyDidPowerOn;
    }
    if (newState == 0)
        IOFramebuffer::handleEvent(kIOFBNotifyWillSleep, reinterpret_cast<void *>(true));
    fPowerState = newState;
    if (oldState == 0) {
        // Sets pagingState and wakes every thread parked in _extEntry.
        IOFramebuffer::handleEvent(kIOFBNotifyDidWake, reinterpret_cast<void *>(true));
        fDpms = -1;   // Sink state unknown after S3; NVGspControl's resume modeset owns it
    }
    if (postEvent) {
        IOFramebuffer::handleEvent(postEvent);
        // After system sleep the GSP is being re-booted: no RPC on the power
        // thread (the resume modeset powers the sink); display wake only.
        if (postEvent == kIOFBNotifyDidPowerOn && oldState != 0) setDpms(true);
    }
    ++fPowerChanges;
    setProperty("NVDisplay-power-state", newState, 32);
    setProperty("NVDisplay-power-changes", fPowerChanges, 32);
    return kIOReturnSuccess;
}

void NVDisplay::setDpms(bool on) {
    if (fDpms == (on ? 1 : 0)) return;
    // A failed sink wake leaves the monitor in standby while macOS believes
    // the display is on, retry (NVGspControl retries AUX itself).
    IOReturn r = kIOReturnError;
    for (int attempt = 0; attempt < (on ? 5 : 2); ++attempt) {
        r = gspCall("nvgsp-dpms", on ? reinterpret_cast<void *>(1) : nullptr, nullptr, nullptr);
        if (r == kIOReturnSuccess) break;
        IOSleep(100);
    }
    if (r == kIOReturnSuccess) fDpms = on ? 1 : 0;
    setProperty("NVDisplay-dpms", on);
    setProperty("NVDisplay-dpms-result", static_cast<UInt32>(r), 32);
}

bool NVDisplay::hwCursorAllowed() {
    if (fHwCursor >= 0) return fHwCursor == 1;
    IORegistryEntry *options = IORegistryEntry::fromPath("/options", gIODTPlane);
    const bool opt = options && options->getProperty("nvdisp-hwcursor");
    OSSafeReleaseNULL(options);
    fHwCursor = opt ? 1 : 0;
    setProperty("NVDisplay-hwcursor", fHwCursor == 1);
    return fHwCursor == 1;
}

IOReturn NVDisplay::setCursorImage(void *cursorImage) {
    if (!hwCursorAllowed()) return kIOReturnUnsupported;
    if (!fCursorBuf) fCursorBuf = static_cast<UInt32 *>(IOMalloc(64 * 64 * 4 * 2));
    if (!fCursorBuf) return kIOReturnNoMemory;
    IOHardwareCursorDescriptor desc;
    bzero(&desc, sizeof(desc));
    desc.majorVersion = kHardwareCursorDescriptorMajorVersion;
    desc.minorVersion = kHardwareCursorDescriptorMinorVersion;
    desc.height = 64;
    desc.width = 64;
    desc.bitDepth = 32;
    IOHardwareCursorInfo info;
    bzero(&info, sizeof(info));
    info.majorVersion = kHardwareCursorInfoMajorVersion;
    info.minorVersion = kHardwareCursorInfoMinorVersion;
    UInt32 *raw = fCursorBuf + 64 * 64;
    bzero(raw, 64 * 64 * 4);
    info.hardwareCursorData = reinterpret_cast<UInt8 *>(raw);
    if (!convertCursorImage(cursorImage, &desc, &info)) {
        gspCall("nvgsp-cursor-show", nullptr, nullptr, nullptr);
        return kIOReturnUnsupported;
    }
    // Repack (cursorWidth-pitched) into the 64x64 hardware surface.
    const UInt32 w = info.cursorWidth > 64 ? 64 : info.cursorWidth;
    const UInt32 h = info.cursorHeight > 64 ? 64 : info.cursorHeight;
    bzero(fCursorBuf, 64 * 64 * 4);
    for (UInt32 y = 0; y < h; ++y)
        for (UInt32 x = 0; x < w; ++x)
            fCursorBuf[y * 64 + x] = raw[y * w + x];
    // MacOS positions the image's top-left (setCursorState), so hotspot 0.
    const IOReturn r = gspCall("nvgsp-cursor-image", fCursorBuf, nullptr, nullptr);
    setProperty("NVDisplay-cursor-image-result", static_cast<UInt32>(r), 32);
    return r;
}

IOReturn NVDisplay::setCursorState(SInt32 x, SInt32 y, bool visible) {
    if (!hwCursorAllowed()) return kIOReturnUnsupported;
    IOReturn r = kIOReturnSuccess;
    if (visible != fCursorVisible) {
        r = gspCall("nvgsp-cursor-show", visible ? reinterpret_cast<void *>(1) : nullptr,
                    nullptr, nullptr);
        fCursorVisible = visible;
    }
    if (visible)
        r = gspCall("nvgsp-cursor-move", reinterpret_cast<void *>(static_cast<intptr_t>(x)),
                    reinterpret_cast<void *>(static_cast<intptr_t>(y)), nullptr);
    return r;
}

IOReturn NVDisplay::setAttribute(IOSelect attribute, uintptr_t value) {
    // Framebuffer power (kIOPowerAttribute: 0 = off) also drives DPMS. like
    // IONDRVFramebuffer::setAttribute, the power attributes are handled here
    // and NOT forwarded (the base turns 'pwrs' into 'powr' and 'powr' into
    // the GPU-mux power path of setAttributeExt).
    if (attribute == kIOPowerStateAttribute || attribute == kIODriverPowerAttribute ||
        attribute == kIOPowerAttribute) {
        setProperty("NVDisplay-fb-power", static_cast<UInt32>(value), 32);
        return nvSetPowerState(static_cast<UInt32>(value));
    }
    return IOFramebuffer::setAttribute(attribute, value);
}

IOReturn NVDisplay::getAttribute(IOSelect attribute, uintptr_t *value) {
    if (attribute == kIOHardwareCursorAttribute) {
        if (value) *value = hwCursorAllowed() ? 1 : 0;
        return kIOReturnSuccess;
    }
    return IOFramebuffer::getAttribute(attribute, value);
}

IOReturn NVDisplay::getAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
                                              uintptr_t *value) {
    switch (attribute) {
    case kConnectionEnable:
        if (value) *value = 1;
        return kIOReturnSuccess;
    case kConnectionFlags:
        if (value) *value = 0;
        return kIOReturnSuccess;
    case kConnectionSupportsHLDDCSense:
        return fetchEdid() ? kIOReturnSuccess : kIOReturnUnsupported;
    case kConnectionSupportsAppleSense:
    case kConnectionSupportsLLDDCSense:
        return kIOReturnUnsupported;
    default:
        return IOFramebuffer::getAttributeForConnection(connectIndex, attribute, value);
    }
}

IOReturn NVDisplay::setAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
                                              uintptr_t value) {
    if (attribute == kConnectionPower) {
        // display sleep/wake → DP DPMS (DPCD 0x600 via NVGspControl).
        setDpms(value != 0);
        return kIOReturnSuccess;
    }
    return IOFramebuffer::setAttributeForConnection(connectIndex, attribute, value);
}
