// Minimal IOFramebuffer for the RTX 4080 (10DE:2704), the very first step of the GPU driver.
//
// Neither GPU on this board has a macOS driver, so WindowServer never starts and no installer GUI shows up;
// the boot just stops printing after the last driver message. WindowServer doesn't need Metal, only an
// IOFramebuffer provider (that's why macOS gives you a desktop on plain framebuffers under QEMU/VMware,
// software rendered). So we publish the linear surface the UEFI GOP already set up, the same surface all the
// verbose boot text gets drawn on.
//
// What NVFBProbe measured on this machine:
//   console relocated to 0x1890000000; 3840x2160, depth 32, rowBytes 16384 (stride padded to 4096 px)
//   BAR1 phys=0x1890000000 len=0x10000000, so the console lives at BAR1 + 0
//   cmd=0x0003, memory/IO decode already on.
// Geometry is still read at runtime, not hardcoded; the numbers above are just what we expect.
//
// Scope: scan-out of the existing mode only. No mode setting, no GSP, no acceleration, and on purpose no
// BAR0 registers.
//
// A lot of debugging code lives in here because this box has no serial port and the verbose console is
// useless once WindowServer grabs the display (or fails to). Short story of how it went:
//  - First we traced through NVRAM (survives a hard reset, readable from Windows with
// tools/read_apple_nvram.ps1): nvfb-trace (which IOFramebuffer entry points got called), nvfb-beat (uptime
// heartbeat), nvfb-ps (processes), nvfb-mnt (mounts), nvfb-dmesg0 (graphics/root lines from the message
// buffer), nvfb-pci0..4 (every IOPCIDevice with BARs, link status, bridge windows, and the first bytes of
// BAR0 for NVMe/xHCI) and nvfb-stor (boot-uuid vs every IOMedia).
//  - That showed root never got mounted ("Still waiting for root device") because one NVMe controller failed
// the CAP MPS check; its BAR was sitting on the second Promontory's GPIO. SSDT-PT2R fixed it and root mounts
// now.
//  - Then the NVRAM store filled up: deletes only flip a header bit and writes need append space that UEFI
// only reclaims at boot. So NVRAM writing is off by default now (boot-arg nvfb-nvram=1 turns it on), and
// every variable this driver ever made gets deleted. Status goes to the console instead.
//  - Next a HUD drawn straight into the scan-out buffer, with a lock-free 1 Hz tick so a frozen screen and a
// frozen kernel can be told apart, plus callback entry/return marks, the last IOKit publish/match events,
// the registry busy state, per-process property reads, user clients per key process and task counters for
// WindowServer.
//  - All that showed the machine freezing around T=9-28 s, and not in the display path. For a channel that
// doesn't go through the GPU we added a UDP log to the Mac build host (tools/udp_log_receiver.py) on its own
// real-time kernel thread, with a proper NIC pick (name "en", Ethernet, sane MTU; the first try grabbed XHC0
// with MTU 0 and hit an assertion in ip_output_list).
//  - Finally everything that touches the machine went behind boot-arg nvfb-diag=1. With it all off the
// machine still stopped, even earlier, so the polling was never the cause. The quiet path now only prints
// "alive t=<n>s" on the console every second (the NVRAM beat thread stopped coming back after a few writes).
// NVRAM writes, when enabled, are change-driven and capped (about 40 KB per boot at worst) to go easy on the
// flash, and need OpenCore's Booter/Quirks/DisableVariableWrite off.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <mach/task_info.h>
#include <mach/thread_policy.h>
#include <sys/mount.h>
#include <sys/proc.h>
#include <libkern/OSByteOrder.h>
#include <net/if.h>
#include <net/kpi_interface.h>
#include <netinet/in.h>
#include <sys/kpi_socket.h>
#include <net/if_types.h>
#include <net/route.h>
#include <netinet/in_var.h>
#include <sys/socket.h>
#include <sys/sockio.h>

#define kNVDisplayMode ((IODisplayModeID)1)

namespace {
enum TraceEvent : unsigned {
    kEvOpen, kEvClose, kEvEnable, kEvAperture, kEvPixFormats, kEvModeCount, kEvModes, kEvModeInfo,
    kEvPixInfo, kEvCurMode, kEvSetMode, kEvConnCount, kEvIsConsole, kEvUserClient, kEvStop,
    // entry marks and callbacks for the HUD
    kEvUcIn, kEvOpIn, kEvSetup, kEvVram, kEvClut, kEvGamma, kEvTiming, kEvDdc, kEvDdcBlock, kEvStartup,
    kEvRegIntr, kEvIntrState, kEvPower, kEvSema, kEvGetAttr, kEvSetAttr, kEvGetConn, kEvSetConn,
    kEvCount
};
const char *const kEvNames[kEvCount] = {
    "op", "cl", "en", "ap", "pf", "mc", "md", "mi", "pi", "cm", "sm", "cc", "ic", "uc", "sp",
    "ui", "oi", "su", "vr", "lu", "gm", "ti", "dd", "db", "st", "ri", "is", "pw", "se", "ga", "sa", "gc", "sc"
};



// Why start() gave up, recorded because a failed start leaves nothing else behind.
enum StartStatus : int {
    kStartRunning = 0, kStartOK = 1, kStartNotOurs = 2, kStartBadConsole = 3, kStartNoBAR1 = 4,
    kStartOutsideBAR1 = 5, kStartNoAperture = 6, kStartSuperFailed = 7
};

struct SelectorSlot {
    char     kind;       // A getAttribute, a setAttribute, C/c the ...ForConnection variants
    UInt32   selector;
    UInt32   count;
    IOReturn last;
};

constexpr unsigned kMaxSelectors     = 24;
constexpr unsigned kMaxEventFlushes  = 8;
constexpr unsigned kMaxBeats         = 20;      // every 30 s for the first ten minutes
constexpr UInt32   kEventDelayMs     = 1000;    // coalesce a burst of calls into one write
constexpr UInt32   kBeatMs           = 30000;
constexpr UInt32   kQuietBeatMs      = 1000;    // without nvfb-diag: the only thing this driver does
constexpr size_t   kTraceBytes       = 512;
constexpr size_t   kVarBytes         = 2000;    // largest single NVRAM variable we write
constexpr int      kScanPids         = 2000;
constexpr unsigned kMaxPsWrites      = 4;
constexpr unsigned kMaxMntWrites     = 2;
constexpr size_t   kDmesgBytes       = 1900;    // kept lines; header + lines fit one variable
constexpr unsigned kDmesgVars        = 1;
constexpr size_t   kPsBytes          = 1500;
constexpr size_t   kDmesgLine        = 256;
constexpr unsigned kPciVars          = 5;
constexpr size_t   kBigBytes         = kVarBytes * kPciVars;

// Kernel log lines kept now that root mounts: graphics, userspace bring-up, and anything fatal.
const char *const kDmesgKeep[] = {
    "WindowServer", "IOFramebuffer", "NVFramebuffer", "IOGraphics", "IOFB", "IONDRV", "AGDC",
    "Display", "display", "launchd", "kernelmanagerd", "Recovery", "BSD root", "root", "panic",
    "watchdog", "kext", "Kext"
};

// Beats are 30 s apart. The registry snapshot (only with nvfb-probe=1) is taken at 60 s; the kernel
// log once at 180 s; the process list at 60/120/180/300 s, and only when it changed.
bool isProbeBeat(unsigned beat) { return beat == 2; }
bool isDmesgBeat(unsigned beat) { return beat == 6; }
bool isPsBeat(unsigned beat) { return beat == 2 || beat == 4 || beat == 6 || beat == 10; }

// Variables only the heavy probes write; deleted at start when those probes are off so that the
// firmware can reclaim their space.
const char *const kStaleVars[] = {
    "nvfb-pci0", "nvfb-pci1", "nvfb-pci2", "nvfb-pci3", "nvfb-pci4", "nvfb-stor",
    "nvfb-dmesg1", "nvfb-dmesg2"
};

// Every variable any version of this driver has written; all deleted at start unless nvfb-nvram=1.
const char *const kAllVars[] = {
    "nvfb-trace", "nvfb-beat", "nvfb-ps", "nvfb-mnt", "nvfb-dmesg0", "nvfb-dmesg1", "nvfb-dmesg2",
    "nvfb-stor", "nvfb-pci0", "nvfb-pci1", "nvfb-pci2", "nvfb-pci3", "nvfb-pci4"
};

// Processes whose presence decides whether a GUI session can come up in Recovery.
struct KeyProc { const char *label; const char *name; };
const KeyProc kKeyProcs[] = {
    {"ws", "WindowServer"}, {"hidd", "hidd"}, {"logd", "logd"}, {"secd", "securityd"},
    {"rosd", "recoveryosd"}, {"lw", "loginwindow"}, {"kmd", "kernelmanagerd"}, {"rec", "Recovery"}
};
constexpr unsigned kKeyProcCount = sizeof(kKeyProcs) / sizeof(kKeyProcs[0]);
constexpr unsigned kMaxPsPrints = 6;

UInt32 fnv1a(const char *p, size_t n) {
    UInt32 h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= static_cast<unsigned char>(p[i]); h *= 16777619u; }
    return h;
}

bool containsText(const char *hay, size_t n, const char *needle) {
    size_t m = 0;
    while (needle[m]) m++;
    for (size_t i = 0; i + m <= n; i++) {
        size_t j = 0;
        while (j < m && hay[i + j] == needle[j]) j++;
        if (j == m) return true;
    }
    return false;
}

// Layout of XNU's struct msgbuf (bsd/sys/msgbuf.h). Declared here rather than included because the
// header is not part of the kernel SDK; msg_magic is checked before anything else is trusted.
struct NVMsgbuf {
    int   msg_magic;
    int   msg_size;
    int   msg_bufx;     // write position: the oldest byte once the buffer has wrapped
    int   msg_bufr;
    char *msg_bufc;
};
constexpr int kMsgMagic = 0x063061;

// Bounded text builder. snprintf is avoided on purpose: it can compile to ___snprintf_chk, which this
// kernel does not export, and an unresolved symbol stops the kext from linking.
struct Text {
    char  *p;
    size_t left;
    void ch(char c) { if (left > 1) { *p++ = c; left--; } }
    void str(const char *s) { while (*s) ch(*s++); }
    void dec(uint64_t v) {
        char t[20]; unsigned n = 0;
        do { t[n++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
        while (n) ch(t[--n]);
    }
    void hex(uint64_t v) {
        char t[16]; unsigned n = 0;
        do { t[n++] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v);
        while (n) ch(t[--n]);
    }
    void hexw(uint64_t v, unsigned width) {
        char t[16];
        if (width > 16) width = 16;
        for (unsigned i = 0; i < width; i++) { t[width - 1 - i] = "0123456789abcdef"[v & 15]; v >>= 4; }
        for (unsigned i = 0; i < width; i++) ch(t[i]);
    }
    void fourcc(UInt32 v) {
        bool printable = true;
        for (int i = 0; i < 4; i++) {
            const UInt32 c = (v >> (24 - 8 * i)) & 0xFF;
            if (c < 0x20 || c > 0x7E) printable = false;
        }
        if (!printable) { str("0x"); hex(v); return; }
        for (int i = 0; i < 4; i++) ch(static_cast<char>((v >> (24 - 8 * i)) & 0xFF));
    }
};
// 5x7 bitmap font for the HUD: digits, upper-case letters and a little punctuation.
struct Glyph { char c; UInt8 rows[7]; };
const Glyph kFont[] = {
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}}, {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}}, {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}}, {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}}, {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}}, {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}}, {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}}, {'D', {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}}, {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}}, {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}}, {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}}, {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}}, {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}}, {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}}, {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}}, {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04}}, {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {'=', {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00}}, {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {'+', {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}}, {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}}, {'/', {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00}},
};

const UInt8 *glyphRows(char c) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    for (const Glyph &g : kFont) {
        if (g.c == c) return g.rows;
    }
    return nullptr;   // space and anything unknown draw as blank
}

constexpr unsigned kHudScale   = 4;                     // one font pixel = 4x4 screen pixels
constexpr unsigned kHudCellW   = 6 * kHudScale;         // 5 columns + 1 spacing
constexpr unsigned kHudCellH   = 9 * kHudScale;         // 7 rows + 2 spacing
constexpr unsigned kHudCols    = 158;
constexpr unsigned kHudBand    = 640;                   // bottom rows reserved for the HUD
constexpr unsigned kLeafMax    = 3;                     // busy leaves shown
constexpr unsigned kClientMax  = 10;                    // user clients shown per watched process
constexpr unsigned kCreatorMax = 12;                    // distinct user-client creators tallied
constexpr unsigned kWsPidMax   = 6;                     // WindowServer pids remembered
constexpr unsigned kPropProcs  = 6;                     // distinct processes tallied for property reads
constexpr unsigned kRingCol    = 78;                    // characters per ring column
constexpr unsigned kHudLines   = 15;
constexpr unsigned kRingSize   = 16;
constexpr uint64_t kHudMinRedrawUs = 100000;  // notifications redraw at most every 100 ms
constexpr unsigned kHudPad     = 12;
constexpr unsigned kHudBlink   = kHudCellH;             // square left of the text
constexpr UInt32   kHudTickMs  = 250;
constexpr UInt32   kHudProbeMs = 1000;
constexpr int      kHudScanPids = 4000;
constexpr unsigned kNewProcs    = 6;                    // newest processes shown on the HUD
// UDP log to a second machine. The addresses below are placeholders (the 192.0.2.x / 198.51.100.x
// documentation ranges), put your own LAN in before you build with it.
constexpr UInt32   kNetTickMs   = 200;
constexpr unsigned kNetPayload  = 1200;                 // text bytes per datagram, well under the MTU
constexpr unsigned kNetBurst    = 32;                   // datagrams per tick at most
constexpr UInt16   kNetPort     = 5140;
constexpr UInt32   kNetDstAddr  = (198u << 24) | (51u << 16) | (100u << 8) | 2u;   // the machine that listens
// The driver configures its own address rather than waiting for DHCP, which had not finished by the time
// any boot so far stopped. Pick a free address on your LAN; the gateway is the hop to the listener's
// subnet. An address configdadds later simply coexists with this one.
constexpr UInt32   kNetSelfAddr = (192u << 24) | (0u << 16) | (2u << 8) | 250u;
constexpr UInt32   kNetSelfMask = 0xFFFFFF00u;
constexpr UInt32   kNetGateway  = (192u << 24) | (0u << 16) | (2u << 8) | 1u;
constexpr UInt32   kNetDstNet   = (198u << 24) | (51u << 16) | (100u << 8) | 0u;
constexpr UInt32   kNetSelfAtS  = 6;                    // uptime before the interface is configured
constexpr UInt32   kNetMinMtu   = 1280;                 // an interface with a smaller MTU is not usable
constexpr UInt32   kHudBg = 0x00101840, kHudFg = 0x00FFFFFF, kHudOn = 0x0000E040, kHudOff = 0x00E02020;

struct DriverFlag { const char *label; const char *className; };
const DriverFlag kAuxDrivers[] = {
    {"X86", "X86PlatformPlugin"}, {"SMC", "ACPI_SMC_PlatformPlugin"}, {"HDA", "AppleHDAController"},
    {"LPC", "AppleLPC"}, {"SMB", "AppleSMBusPCI"}, {"AGDC", "AppleGraphicsDeviceControl"},
    {"NDRV", "IONDRVFramebuffer"}, {"PDRC", "AppleSMCPDRC"}, {"MUX", "AppleMuxControl"}, {"HV", "AppleHV"},
};
constexpr unsigned kAuxDriverCount = sizeof(kAuxDrivers) / sizeof(kAuxDrivers[0]);

struct MountWalk {
    Text    *t;
    unsigned count;
};

void childDrivers(Text &t, IORegistryEntry *e) {
    unsigned n = 0;
    if (OSIterator *it = e->getChildIterator(gIOServicePlane)) {
        while (OSObject *o = it->getNextObject()) {
            IORegistryEntry *c = OSDynamicCast(IORegistryEntry, o);
            if (!c) continue;
            t.ch(n ? ',' : '='); t.str(c->getMetaClass()->getClassName());
            if (++n == 3) break;
        }
        it->release();
    }
    if (!n) t.str("=-");
}

// One line per PCI function. Register reads here are the same read-only accesses the owning
// drivers already make (IONVMeFamily read CAP to produce the assert we are chasing).
void describePci(Text &t, IOPCIDevice *d) {
    const UInt32 id  = d->configRead32(kIOPCIConfigVendorID);
    const UInt32 cls = d->configRead32(kIOPCIConfigRevisionID) >> 8;
    const UInt16 cmd = d->configRead16(kIOPCIConfigCommand);
    const UInt8  hdr = d->configRead8(kIOPCIConfigHeaderType) & 0x7F;
    t.hexw(d->getBusNumber(), 2); t.ch(':'); t.hexw(d->getDeviceNumber(), 2); t.ch('.');
    t.hexw(d->getFunctionNumber(), 1);
    t.ch(' '); t.hexw(id & 0xFFFF, 4); t.ch(':'); t.hexw(id >> 16, 4);
    t.str(" c="); t.hexw(cls, 6); t.str(" cmd="); t.hexw(cmd, 4);

    UInt8 cap = 0;
    if (d->findPCICapability(kIOPCIPCIExpressCapability, &cap) && cap) {
        t.str(" ls="); t.hexw(d->configRead16(static_cast<UInt8>(cap + 0x12)), 4);
    }

    if (hdr == 1) {
        const UInt32 buses = d->configRead32(0x18);
        const UInt32 mem   = d->configRead32(0x20);
        const UInt32 pf    = d->configRead32(0x24);
        const uint64_t pfBase  = (static_cast<uint64_t>(d->configRead32(0x28)) << 32) |
                                 (static_cast<uint64_t>(pf & 0xFFF0) << 16);
        const uint64_t pfLimit = (static_cast<uint64_t>(d->configRead32(0x2C)) << 32) |
                                 (pf & 0xFFF00000u) | 0xFFFFFu;
        t.str(" br="); t.hexw((buses >> 8) & 0xFF, 2); t.ch('-'); t.hexw((buses >> 16) & 0xFF, 2);
        t.str(" mw="); t.hex((mem & 0xFFF0u) << 16); t.ch('-'); t.hex((mem & 0xFFF00000u) | 0xFFFFFu);
        t.str(" pw="); t.hex(pfBase); t.ch('-'); t.hex(pfLimit);
    } else {
        t.str(" b0="); t.hex(d->configRead32(kIOPCIConfigBaseAddress0));
        t.str(" b1="); t.hex(d->configRead32(kIOPCIConfigBaseAddress1));
        IODeviceMemory *mem = d->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
        if (mem) {
            t.str(" m0="); t.hex(mem->getPhysicalAddress()); t.ch('+'); t.hex(mem->getLength());
            const UInt32 base = cls >> 8;
            const bool probe = (cls >> 16) == 0x01 || base == 0x0C03;   // storage, USB
            if (probe) {
                if (IOMemoryMap *map = mem->map(kIOMapInhibitCache)) {
                    volatile UInt32 *r =
                        reinterpret_cast<volatile UInt32 *>(static_cast<uintptr_t>(map->getVirtualAddress()));
                    t.str(" r0="); t.hexw(r[1], 8); t.hexw(r[0], 8);
                    map->release();
                } else {
                    t.str(" r0=nomap");
                }
            }
        } else {
            t.str(" m0=-");
        }
    }
    t.str(" d"); childDrivers(t, d);
    t.ch('\n');
}

void appendProperty(Text &t, IORegistryEntry *e, const char *key, size_t max) {
    OSObject *o = e->getProperty(key);
    t.str(key); t.ch('=');
    if (OSData *data = OSDynamicCast(OSData, o)) {
        const unsigned char *b = static_cast<const unsigned char *>(data->getBytesNoCopy());
        const size_t n = data->getLength() < max ? data->getLength() : max;
        bool text = true;
        for (size_t i = 0; i < n; i++) {
            if (b[i] && (b[i] < 0x20 || b[i] > 0x7E) && b[i] != '\n' && b[i] != '\t') text = false;
        }
        for (size_t i = 0; i < n; i++) {
            if (text) { if (b[i]) t.ch(static_cast<char>(b[i])); }
            else t.hexw(b[i], 2);
        }
    } else if (OSString *str = OSDynamicCast(OSString, o)) {
        t.str(str->getCStringNoCopy());
    } else {
        t.str(o ? "?" : "-");
    }
    t.ch('\n');
}

const char *stringProperty(IORegistryEntry *e, const char *key) {
    OSString *str = OSDynamicCast(OSString, e->getProperty(key));
    return str ? str->getCStringNoCopy() : "-";
}

int mountCallout(mount_t mp, void *arg) {
    MountWalk *walk = static_cast<MountWalk *>(arg);
    struct vfsstatfs *sf = vfs_statfs(mp);
    if (sf) {
        walk->t->str(sf->f_mntfromname); walk->t->str(" on "); walk->t->str(sf->f_mntonname);
        walk->t->str(" ("); walk->t->str(sf->f_fstypename); walk->t->str(")\n");
        walk->count++;
    }
    return VFS_RETURNED;
}
}  // namespace

extern "C" NVMsgbuf *msgbufp;
extern "C" {
kern_return_t task_info(task_t task, task_flavor_t flavor, task_info_t task_info_out,
                        mach_msg_type_number_t *task_info_count);
task_t proc_task(proc_t proc);
kern_return_t thread_terminate(thread_t thread);
kern_return_t thread_policy_set(thread_t thread, thread_policy_flavor_t flavor, thread_policy_t policy_info,
                                mach_msg_type_number_t count);
}

class NVFramebuffer : public IOFramebuffer {
    OSDeclareDefaultStructors(NVFramebuffer)

    IOPCIDevice    *fPCI      {nullptr};
    IODeviceMemory *fAperture {nullptr};
    UInt32          fWidth    {0};
    UInt32          fHeight   {0};
    UInt32          fRowBytes {0};

    // Trace state; fTraceLock guards everything below it.
    IOLock         *fFlushLock  {nullptr};   // serializes writes so an older snapshot never lands last
    IOLock         *fTraceLock  {nullptr};
    thread_call_t   fEventCall  {nullptr};
    thread_call_t   fBeatCall   {nullptr};
    UInt32          fEvents[kEvCount] {};
    SelectorSlot    fSelectors[kMaxSelectors] {};
    unsigned        fSelectorCount {0};
    UInt32          fUserClientTypes {0};
    IOReturn        fUserClientResult {kIOReturnSuccess};
    int             fStartStatus {kStartRunning};
    IOReturn        fOpenResult {kIOReturnSuccess};
    unsigned        fFlushes {0};
    unsigned        fNewsFlushes {0};
    unsigned        fBeats {0};
    bool            fEventPending {false};
    bool            fDirty {false};
    bool            fProbe {false};   // boot-arg nvfb-probe=1: also write the pci/stor snapshots
    bool            fNvram {false};   // boot-arg nvfb-nvram=1: NVRAM writes on (off: store exhausted)
    bool            fDiag  {false};   // boot-arg nvfb-diag=1: HUD, probes, registry walk, net thread
    UInt32          fPrintedPsHash {0};
    unsigned        fPsPrints {0};

    // HUD: drawn into the scan-out buffer. The tick only reads the cached probe results below.
    IOMemoryMap    *fHudMap    {nullptr};
    thread_call_t   fHudCall   {nullptr};
    thread_call_t   fProbeCall {nullptr};
    unsigned        fHudTicks  {0};
    clock_sec_t     fProbeAt   {0};
    unsigned        fProbeRuns {0};
    unsigned        fHudProcs  {0};
    int             fHudWs {-1}, fHudHidd {-1}, fHudLogd {-1}, fHudRosd {-1};
    bool            fHudX86 {false}, fHudAcpiSmc {false}, fHudHidSystem {false};
    bool            fHudAux[kAuxDriverCount] {};
    // Written lock-free from any callback: what the HUD shows as the last thing entered or left.
    UInt32          fHudCount[kEvCount] {};
    unsigned        fHudLastEv {kEvCount};
    bool            fHudLastIn {false};
    char            fHudSelKind {0};
    UInt32          fHudSel {0};
    unsigned        fHudCalls {0};

    // Service ring fed by IOKit notifications; written lock-free, torn reads only affect the display.
    struct RingEntry { UInt32 ms; char kind; char cls[30]; char name[26]; };
    RingEntry       fRing[kRingSize] {};
    unsigned        fRingNext {0};
    UInt32          fPubCount {0}, fMatchCount {0};
    uint64_t        fLastDrawUs {0};
    IONotifier     *fPubNotifier   {nullptr};
    IONotifier     *fMatchNotifier {nullptr};
    bool            fConsoleShrunk {false};

    // Incremental repaint: what each HUD line currently shows on screen, and a one-drawer gate.
    char            fDrawn[kHudLines][kHudCols + 8] {};
    volatile UInt32 fDrawBusy {0};
    bool            fBandPainted {false};
    clock_sec_t     fLastFullPaint {0};
    // Newest processes (highest pids) from the last probe.
    int             fNewPid[kNewProcs] {};
    char            fNewName[kNewProcs][17] {};
    unsigned        fNewCount {0};

    // Busy leaves from the last registry walk (probe thread writes, HUD reads; torn reads only
    // affect the display).
    struct BusyLeaf { UInt32 busy; char cls[30]; char name[24]; char prov[24]; };
    BusyLeaf        fLeaves[kLeafMax] {};
    unsigned        fLeafCount {0};
    unsigned        fLeafTotal {0};
    unsigned        fWalkEntries {0};
    // User clients opened by WindowServer, recoveryosd and loginwindow, from the same walk.
    struct UcEntry { char who; char cls[28]; char prov[24]; };
    UcEntry         fClients[kClientMax] {};
    unsigned        fClientCount {0};
    unsigned        fClientTotal[3] {};           // W, R, L
    unsigned        fClientAll {0};
    struct Creator { char name[20]; unsigned count; };
    Creator         fCreators[kCreatorMax] {};
    unsigned        fCreatorCount {0};
    // Task counters for WindowServer, recoveryosd, loginwindow: current and one probe ago.
    struct TaskStat { int pid; bool ok; UInt32 csw, msent, mrecv, smach, sunix; uint64_t cpuUs; };
    TaskStat        fTask[3] {}, fTaskPrev[3] {};
    int             fHudLw {-1};
    // Every WindowServer pid seen, with the uptime it was first seen.
    int             fWsPid[kWsPidMax] {};
    UInt32          fWsSeenAt[kWsPidMax] {};
    unsigned        fWsPidCount {0};
    // User processes reading this framebuffer's properties, one slot per process.
    struct PropProc { int pid; char proc[17]; UInt32 count; char lastKey[30]; };
    PropProc        fPropProcs[kPropProcs] {};
    UInt32          fPropCount {0};
    UInt32          fPropOther {0};              // reads by processes beyond the tally's capacity
    // UDP log (net thread call only; the HUD reads the counters, torn reads only affect the display).
    thread_t        fNetThread {nullptr};
    volatile UInt32 fNetThreadDone {0};
    kern_return_t   fNetPolicy {KERN_FAILURE};   // real-time policy of the net thread
    unsigned        fWatchBeats {0};             // net-thread iterations
    bool            fNetSelfDone {false};        // our own address and route are in place
    int             fNetSelfErr {0};
    UInt32          fNetSelfAt {0};
    unsigned        fNetSelfTries {0};
    char            fNetIfList[120] {};          // "en0:8863/1500/6 XHC0:8843/0/6 ..." from the last scan
    // USB and HID nubs counted by the registry walk: how far USB enumeration got.
    unsigned        fUsbDevs {0}, fUsbIfaces {0}, fHidNubs {0}, fHidKbd {0}, fXhci {0};
    unsigned        fTcSeen {0};                 // HUD tick count as last seen by the net thread
    UInt32          fTcSeenAt {0};               // uptime when it last changed
    socket_t        fNetSock {nullptr};
    int             fNetCursor {-1};             // next msgbuf index to send; -1 before the first send
    UInt32          fNetSeq {0};                 // datagrams sent, so gaps show up at the receiver
    UInt32          fNetFail {0};
    int             fNetErr {0};                 // last sock_socket/sock_send error
    UInt32          fNetIp {0};                  // first non-link-local IPv4 address (host order)
    UInt32          fNetIpAt {0};                // uptime when an address first appeared
    unsigned        fNetIfCount {0};             // non-loopback interfaces
    char            fNetIfName[16] {};
    unsigned        fNetTicks {0};
    UInt32          fNetStatusAt {0};
    bool            fNetHello {false};
    char            fNetBuf[kNetPayload + 128];
    // GPU path health, sampled once a second by the net tick.
    UInt16          fGpuCmd0 {0}, fGpuCmd {0};
    UInt32          fGpuBar1Lo0 {0}, fGpuBar1Hi0 {0}, fGpuBar1Lo {0}, fGpuBar1Hi {0};
    UInt32          fRbOk {0}, fRbBad {0}, fRbBadAt {0}, fRbGot {0};
    // System snapshot state; only beatCallout touches these, and a thread call never overlaps itself.
    UInt32          fPsHash {0};
    UInt32          fMntHash {0};
    unsigned        fPsWrites {0};
    unsigned        fMntWrites {0};
    char            fSysBuf[kVarBytes + 64];
    char            fBig[kBigBytes + 64];
    char            fDmesgRing[kDmesgBytes];
    char            fDmesgOut[kDmesgBytes + 128];
    char            fDmesgLine[kDmesgLine];
    bool            fStopping {false};

    void traceEvent(unsigned event);
    void traceSelector(char kind, IOSelect selector, IOReturn result);
    void noteNewsLocked();
    bool flushTrace();
    void writeBeat(unsigned beat);
    void writeProcesses(clock_sec_t now);
    void writeMounts(clock_sec_t now);
    void writeDmesg(unsigned beat, clock_sec_t now);
    void writePci(clock_sec_t now);
    void writeStorage(clock_sec_t now);
    void writeChunks(const char *prefix, const char *buf, size_t len, unsigned maxVars);
    bool put(const char *name, const void *buf, size_t len);
    void logStatus(clock_sec_t now);
    void hudFill(unsigned x, unsigned y, unsigned w, unsigned h, UInt32 color);
    void hudText(unsigned x, unsigned y, const char *text);
    void hudDraw(clock_sec_t now);
    void hudMark(unsigned event, bool entering);
    void hudSelector(char kind, IOSelect selector);
    void ringAdd(char kind, IOService *service);
    void walkBusy();
    static void readTask(int pid, TaskStat &out);
    void propSeen(const char *key);
    static bool publishHandler(void *target, void *ref, IOService *service, IONotifier *notifier);
    static bool matchedHandler(void *target, void *ref, IOService *service, IONotifier *notifier);
    void hudProbe(clock_sec_t now);
    static void hudCallout(thread_call_param_t self, thread_call_param_t);
    static void probeCallout(thread_call_param_t self, thread_call_param_t);
    static void netThreadMain(void *param, wait_result_t);
    void watchDraw(UInt32 nowS);
    void netTick(uint64_t nowMs);
    void netSendTick(uint64_t nowMs, UInt32 nowS, bool second);
    void netScanIf(UInt32 nowS);
    void netSelfConfig(UInt32 nowS);
    void gpuCheck(UInt32 nowS);
    bool netSend(const char *buf, size_t len);
    void netSendLog(uint64_t nowMs);
    void netSendStatus(uint64_t nowMs);
    static bool serviceExists(const char *className);
    static void eventCallout(thread_call_param_t self, thread_call_param_t);
    static void beatCallout(thread_call_param_t self, thread_call_param_t);
    bool failStart(int status);

public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free(void) override;

    // Traced pass-throughs: these exist only to record that the system called them.
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
                           IOUserClient **handler) override;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Woverloaded-virtual"
    IOReturn open(void) override;
    void close(void) override;
#pragma clang diagnostic pop
    IOReturn setAttribute(IOSelect attribute, uintptr_t value) override;
    IOReturn setupForCurrentConfig(void) override;
    IODeviceMemory *getVRAMRange(void) override;
    IOReturn setCLUTWithEntries(IOColorEntry *colors, UInt32 index, UInt32 numEntries,
                                IOOptionBits options) override;
    IOReturn setGammaTable(UInt32 channelCount, UInt32 dataCount, UInt32 dataWidth,
                           void *data) override;
    IOReturn setGammaTable(UInt32 channelCount, UInt32 dataCount, UInt32 dataWidth, void *data,
                           bool syncToVBL) override;
    IOReturn getTimingInfoForDisplayMode(IODisplayModeID displayMode,
                                         IOTimingInformation *info) override;
    bool hasDDCConnect(IOIndex connectIndex) override;
    IOReturn getDDCBlock(IOIndex connectIndex, UInt32 blockNumber, IOSelect blockType,
                         IOOptionBits options, UInt8 *data, IOByteCount *length) override;
    IOReturn getStartupDisplayMode(IODisplayModeID *displayMode, IOIndex *depth) override;
    IOReturn registerForInterruptType(IOSelect interruptType, IOFBInterruptProc proc,
                                      OSObject *target, void *ref, void **interruptRef) override;
    IOReturn setInterruptState(void *interruptRef, UInt32 state) override;
    IOReturn setPowerState(unsigned long powerStateOrdinal, IOService *device) override;
    IOReturn getNotificationSemaphore(IOSelect interruptType, semaphore_t *semaphore) override;

    // Property reads, traced when a user process makes them. All six overloads are overridden so
    // none is hidden.
    OSObject *copyProperty(const char *aKey) const override;
    OSObject *copyProperty(const OSString *aKey) const override;
    OSObject *copyProperty(const OSSymbol *aKey) const override;
    OSObject *copyProperty(const char *aKey, const IORegistryPlane *plane,
                           IOOptionBits options) const override;
    OSObject *copyProperty(const OSString *aKey, const IORegistryPlane *plane,
                           IOOptionBits options) const override;
    OSObject *copyProperty(const OSSymbol *aKey, const IORegistryPlane *plane,
                           IOOptionBits options) const override;
    bool serializeProperties(OSSerialize *serialize) const override;

    // Pure virtuals every IOFramebuffer must supply.
    IODeviceMemory *getApertureRange(IOPixelAperture aperture) override;
    const char *getPixelFormats(void) override;
    IOItemCount getDisplayModeCount(void) override;
    IOReturn getDisplayModes(IODisplayModeID *allDisplayModes) override;
    IOReturn getInformationForDisplayMode(IODisplayModeID displayMode,
                                          IODisplayModeInformation *info) override;
    UInt64 getPixelFormatsForDisplayMode(IODisplayModeID displayMode, IOIndex depth) override;
    IOReturn getPixelInformation(IODisplayModeID displayMode, IOIndex depth,
                                 IOPixelAperture aperture, IOPixelInformation *pixelInfo) override;
    IOReturn getCurrentDisplayMode(IODisplayModeID *displayMode, IOIndex *depth) override;

    IOReturn enableController(void) override;
    bool isConsoleDevice(void) override;
    IOReturn setDisplayMode(IODisplayModeID displayMode, IOIndex depth) override;
    IOItemCount getConnectionCount(void) override;
    IOReturn getAttribute(IOSelect attribute, uintptr_t *value) override;
    IOReturn getAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
                                       uintptr_t *value) override;
    IOReturn setAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
                                       uintptr_t value) override;
};

OSDefineMetaClassAndStructors(NVFramebuffer, IOFramebuffer)

// ---- NVRAM trace ----------------------------------------------------------------------------------

// Caller holds fTraceLock. Something new happened: schedule one coalesced write, unless one is already
// pending or the budget is spent. Repeats of known calls only mark the trace dirty for the heartbeat,
// so a polling loop cannot exhaust the budget before WindowServer ever shows up.
void NVFramebuffer::noteNewsLocked() {
    fDirty = true;
    if (fEventPending || fStopping || !fEventCall || fNewsFlushes >= kMaxEventFlushes) return;
    fEventPending = true;
    uint64_t deadline = 0;
    clock_interval_to_deadline(kEventDelayMs, kMillisecondScale, &deadline);
    thread_call_enter_delayed(fEventCall, deadline);
}

void NVFramebuffer::traceEvent(unsigned event) {
    if (!fTraceLock || event >= kEvCount) return;
    IOLockLock(fTraceLock);
    if (fEvents[event]++ == 0) noteNewsLocked();
    else fDirty = true;
    IOLockUnlock(fTraceLock);
}

void NVFramebuffer::traceSelector(char kind, IOSelect selector, IOReturn result) {
    if (!fTraceLock) return;
    IOLockLock(fTraceLock);
    SelectorSlot *slot = nullptr;
    for (unsigned i = 0; i < fSelectorCount; i++) {
        if (fSelectors[i].kind == kind && fSelectors[i].selector == selector) {
            slot = &fSelectors[i];
            break;
        }
    }
    if (!slot && fSelectorCount < kMaxSelectors) {
        slot = &fSelectors[fSelectorCount++];
        slot->kind = kind;
        slot->selector = selector;
    }
    if (slot) {
        const bool news = slot->count == 0 || slot->last != result;
        slot->count++;
        slot->last = result;
        if (news) noteNewsLocked();
        else fDirty = true;
    }
    IOLockUnlock(fTraceLock);
}

// Snapshot, format and write nvfb-trace. Returns the write result so start() can show it on screen.
bool NVFramebuffer::flushTrace() {
    if (!fTraceLock || !fFlushLock) return false;
    IOLockLock(fFlushLock);

    UInt32 events[kEvCount];
    SelectorSlot slots[kMaxSelectors];
    IOLockLock(fTraceLock);
    for (unsigned i = 0; i < kEvCount; i++) events[i] = fEvents[i];
    const unsigned slotCount = fSelectorCount;
    for (unsigned i = 0; i < slotCount; i++) slots[i] = fSelectors[i];
    const unsigned flushes = ++fFlushes;
    const UInt32 ucTypes = fUserClientTypes;
    const IOReturn ucResult = fUserClientResult;
    const IOReturn openResult = fOpenResult;
    const int status = fStartStatus;
    fDirty = false;
    IOLockUnlock(fTraceLock);

    clock_sec_t secs = 0;
    clock_usec_t usecs = 0;
    clock_get_system_microtime(&secs, &usecs);

    char buf[kTraceBytes];
    Text t{buf, sizeof(buf)};
    t.str("nvfb2 up="); t.dec(secs);
    t.str(" f=");       t.dec(flushes);
    t.str(" ss=");      t.dec(static_cast<unsigned>(status));
    for (unsigned i = 0; i < kEvCount; i++) {
        if (!events[i]) continue;
        t.ch(' '); t.str(kEvNames[i]); t.ch('='); t.dec(events[i]);
    }
    if (openResult) { t.str(" or="); t.hex(static_cast<UInt32>(openResult)); }
    if (ucTypes) {
        t.str(" ut="); t.hex(ucTypes);
        t.str(" ur="); t.hex(static_cast<UInt32>(ucResult));
    }
    for (unsigned i = 0; i < slotCount; i++) {
        t.ch(' '); t.ch(slots[i].kind); t.ch('.'); t.fourcc(slots[i].selector);
        t.ch('='); t.dec(slots[i].count);
        if (slots[i].last) { t.ch('!'); t.hex(static_cast<UInt32>(slots[i].last)); }
    }
    *t.p = '\0';
    const bool ok = put("nvfb-trace", buf, static_cast<size_t>(t.p - buf));

    IOLockUnlock(fFlushLock);
    return ok;
}

void NVFramebuffer::writeBeat(unsigned beat) {
    clock_sec_t secs = 0;
    clock_usec_t usecs = 0;
    clock_get_system_microtime(&secs, &usecs);
    char buf[48];
    Text t{buf, sizeof(buf)};
    t.str("up="); t.dec(secs);
    t.str(" n="); t.dec(beat);
    *t.p = '\0';
    put("nvfb-beat", buf, static_cast<size_t>(t.p - buf));
}

// Running processes by pid. pid order matters: a daemon that keeps crashing and being relaunched
// shows up as the same name at ever-higher pids.
void NVFramebuffer::writeProcesses(clock_sec_t now) {
    Text list{fSysBuf, kPsBytes};
    unsigned count = 0;
    int maxPid = -1;
    int windowServer = -1;
    char name[32];
    for (int pid = 0; pid < kScanPids; pid++) {
        proc_name(pid, name, sizeof(name));
        if (!name[0]) continue;
        count++;
        maxPid = pid;
        if (containsText(name, sizeof(name), "WindowServer")) windowServer = pid;
        list.dec(static_cast<unsigned>(pid)); list.ch(':'); list.str(name); list.ch(' ');
    }
    *list.p = '\0';
    const size_t listLen = static_cast<size_t>(list.p - fSysBuf);
    const UInt32 hash = fnv1a(fSysBuf, listLen) ^ count;
    if (hash == fPsHash || fPsWrites >= kMaxPsWrites) return;
    fPsHash = hash;
    fPsWrites++;

    char head[64];
    Text h{head, sizeof(head)};
    h.str("up="); h.dec(now); h.str(" n="); h.dec(count);
    h.str(" max="); h.dec(static_cast<unsigned>(maxPid + 1));
    h.str(" ws="); if (windowServer < 0) h.ch('-'); else h.dec(static_cast<unsigned>(windowServer));
    h.str(" | ");
    *h.p = '\0';
    // Prepend the header by shifting the list right; both fit in fSysBuf by construction.
    const size_t headLen = static_cast<size_t>(h.p - head);
    size_t total = headLen + listLen;
    if (total > kPsBytes) total = kPsBytes;
    for (size_t i = total; i-- > headLen;) fSysBuf[i] = fSysBuf[i - headLen];
    for (size_t i = 0; i < headLen && i < total; i++) fSysBuf[i] = head[i];
    put("nvfb-ps", fSysBuf, total);
}

void NVFramebuffer::writeMounts(clock_sec_t now) {
    Text t{fSysBuf, kVarBytes};
    t.str("up="); t.dec(now); t.ch('\n');
    MountWalk walk{&t, 0};
    vfs_iterate(0, mountCallout, &walk);
    if (!walk.count) t.str("(no mounts)");
    *t.p = '\0';
    const size_t len = static_cast<size_t>(t.p - fSysBuf);
    // Hash without the uptime line, so only a real change in the mount table triggers a write.
    size_t body = 0;
    while (body < len && fSysBuf[body] != '\n') body++;
    const UInt32 hash = fnv1a(fSysBuf + body, len - body) ^ walk.count;
    if (hash == fMntHash || fMntWrites >= kMaxMntWrites) return;
    fMntHash = hash;
    fMntWrites++;
    put("nvfb-mnt", fSysBuf, len);
}

// ---- HUD ------------------------------------------------------------------------------------------

void NVFramebuffer::hudFill(unsigned x, unsigned y, unsigned w, unsigned h, UInt32 color) {
    if (!fHudMap || x >= fWidth || y >= fHeight) return;
    if (x + w > fWidth) w = fWidth - x;
    if (y + h > fHeight) h = fHeight - y;
    const uintptr_t base = static_cast<uintptr_t>(fHudMap->getVirtualAddress());
    for (unsigned row = 0; row < h; row++) {
        volatile UInt32 *px = reinterpret_cast<volatile UInt32 *>(base + (y + row) * static_cast<uintptr_t>(fRowBytes)) + x;
        for (unsigned col = 0; col < w; col++) px[col] = color;
    }
}

void NVFramebuffer::hudText(unsigned x, unsigned y, const char *text) {
    for (unsigned i = 0; text[i] && i < kHudCols; i++) {
        const UInt8 *rows = glyphRows(text[i]);
        if (!rows) continue;
        for (unsigned r = 0; r < 7; r++) {
            for (unsigned c = 0; c < 5; c++) {
                if (rows[r] & (0x10 >> c)) {
                    hudFill(x + i * kHudCellW + c * kHudScale, y + r * kHudScale, kHudScale, kHudScale, kHudFg);
                }
            }
        }
    }
}

void NVFramebuffer::hudDraw(clock_sec_t now) {
    if (fWidth < 1024 || fHeight < kHudBand * 2) return;
    // One drawer at a time; a caller that finds the HUD busy just skips - the next tick catches up.
    if (!OSCompareAndSwap(0, 1, &fDrawBusy)) return;
    const unsigned x0 = 0, y0 = fHeight - kHudBand;
    const bool full = !fBandPainted || (now != fLastFullPaint && now % 5 == 0);
    if (full) {
        hudFill(x0, y0, fWidth, fBandPainted ? kHudPad + kHudLines * kHudCellH : kHudBand, kHudBg);
        for (unsigned i = 0; i < kHudLines; i++) fDrawn[i][0] = '\0';
        fBandPainted = true;
        fLastFullPaint = now;
    }
    hudFill(x0 + kHudPad, y0 + kHudPad, kHudBlink, kHudBlink, (fHudTicks & 1) ? kHudOn : kHudOff);

    const unsigned age = fProbeRuns ? static_cast<unsigned>(now - fProbeAt) : 999;
    char line[kHudLines][kHudCols + 8];
    for (unsigned i = 0; i < kHudLines; i++) line[i][0] = '\0';
    auto pid = [](Text &t, const char *label, int v) {
        t.ch(' '); t.str(label); t.ch('=');
        if (v < 0) t.ch('-'); else t.dec(static_cast<unsigned>(v));
    };

    Text a{line[0], sizeof(line[0])};
    a.str("NVFB T="); a.dec(now); a.str(" AGE="); a.dec(age); a.str(" PR="); a.dec(fHudProcs);
    pid(a, "WS", fHudWs); pid(a, "HID", fHudHidd); pid(a, "LOG", fHudLogd); pid(a, "ROS", fHudRosd);
    a.str(" HS"); a.ch(fHudHidSystem ? '+' : '-');
    a.str("   LAST=");
    if (fHudLastEv < kEvCount) { a.str(kEvNames[fHudLastEv]); a.ch(fHudLastIn ? '>' : '<'); }
    else a.ch('-');
    a.str(" N="); a.dec(fHudCalls);
    a.str(" PUB="); a.dec(fPubCount); a.str(" MAT="); a.dec(fMatchCount);
    if (fHudSelKind) { a.str(" SEL="); a.ch(fHudSelKind); a.ch('.'); a.fourcc(fHudSel); }
    a.str(fConsoleShrunk ? " CON=BAND" : " CON=FULL");
    if (IOService *root = IOService::getServiceRoot()) { a.str(" BUSY="); a.dec(root->getBusyState()); }
    a.str(" UNM="); a.dec(fPubCount > fMatchCount ? fPubCount - fMatchCount : 0);
    *a.p = '\0';

    // Every callback counter, split over two lines.
    const unsigned half = (kEvCount + 1) / 2;
    for (unsigned row = 0; row < 2; row++) {
        Text t{line[1 + row], sizeof(line[1 + row])};
        for (unsigned i = row * half; i < kEvCount && i < (row + 1) * half; i++) {
            if (i != row * half) t.ch(' ');
            t.str(kEvNames[i]); t.dec(fHudCount[i]);
        }
        *t.p = '\0';
    }

    // Newest processes: the highest pids alive at the last probe.
    {
        Text t{line[3], sizeof(line[3])};
        t.str("NEW PROCS:");
        for (unsigned i = 0; i < fNewCount && i < kNewProcs; i++) {
            t.ch(' '); t.dec(static_cast<unsigned>(fNewPid[i])); t.ch(':'); t.str(fNewName[i]);
        }
        *t.p = '\0';
    }

    // network log and GPU path: "NET en0 <addr> AT 21 IF=2 TX=.. FAIL=.. E=.. GPU CMD=.. B1=.. RB=ok/bad"
    {
        Text t{line[4], sizeof(line[4])};
        t.str("NET ");
        if (fNetIp) {
            t.str(fNetIfName); t.ch(' ');
            t.dec(fNetIp >> 24); t.ch('.'); t.dec((fNetIp >> 16) & 0xFF); t.ch('.');
            t.dec((fNetIp >> 8) & 0xFF); t.ch('.'); t.dec(fNetIp & 0xFF);
            t.str(" AT "); t.dec(fNetIpAt);
        } else {
            t.str("NOIP");
        }
        t.str(" IF="); t.dec(fNetIfCount); t.str(" TX="); t.dec(fNetSeq); t.str(" FAIL="); t.dec(fNetFail);
        t.str(" SELF="); t.dec(fNetSelfDone ? fNetSelfAt : 0); t.ch('/'); t.dec(fNetSelfTries);
        t.str(" SE="); t.dec(static_cast<unsigned>(fNetSelfErr));
        t.str(" E="); t.dec(static_cast<unsigned>(fNetErr));
        t.str("  GPU CMD="); t.hexw(fGpuCmd, 4);
        if (fGpuCmd != fGpuCmd0) { t.str("-WAS-"); t.hexw(fGpuCmd0, 4); }
        t.str(" B1="); t.hex((static_cast<uint64_t>(fGpuBar1Hi) << 32) | fGpuBar1Lo);
        if (fGpuBar1Lo != fGpuBar1Lo0 || fGpuBar1Hi != fGpuBar1Hi0) t.str("-MOVED");
        t.str(" RB="); t.dec(fRbOk); t.ch('/'); t.dec(fRbBad);
        if (fRbBad) { t.str(" BADAT "); t.dec(fRbBadAt); t.str(" GOT "); t.hex(fRbGot); }
        t.str("  BL "); t.dec(fLeafTotal); t.ch('/'); t.dec(fWalkEntries);
        *t.p = '\0';
    }
    // User clients: watched processes' counts, the total, and the busiest creators.
    {
        Text t{line[5], sizeof(line[5])};
        t.str("CLIENTS WS="); t.dec(fClientTotal[0]); t.str(" ROS="); t.dec(fClientTotal[1]);
        t.str(" LW="); t.dec(fClientTotal[2]); t.str(" ALL="); t.dec(fClientAll); t.str(" TOP:");
        bool used[kCreatorMax] = {};
        for (unsigned shown = 0; shown < 5; shown++) {
            int best = -1;
            for (unsigned k = 0; k < fCreatorCount && k < kCreatorMax; k++) {
                if (!used[k] && (best < 0 || fCreators[k].count > fCreators[best].count)) best = static_cast<int>(k);
            }
            if (best < 0) break;
            used[best] = true;
            t.ch(' '); t.str(fCreators[best].name); t.str(" x"); t.dec(fCreators[best].count);
        }
        for (unsigned k = 0; k < fClientCount && k < 3; k++) {
            const UcEntry &e = fClients[k];
            t.str(k ? " " : " | "); t.ch(e.who); t.ch(':'); t.str(e.cls); t.ch('<'); t.str(e.prov);
        }
        *t.p = '\0';
    }
    // WindowServer pid history: a crash loop shows up as a growing list.
    {
        Text t{line[6], sizeof(line[6])};
        t.str("WS PIDS "); t.dec(fWsPidCount); t.ch(':');
        for (unsigned k = 0; k < fWsPidCount && k < kWsPidMax; k++) {
            t.ch(' '); t.dec(static_cast<unsigned>(fWsPid[k])); t.ch('@'); t.dec(fWsSeenAt[k]); t.ch('s');
        }
        t.str("   USB XHCI="); t.dec(fXhci); t.str(" DEV="); t.dec(fUsbDevs); t.str(" IFACE=");
        t.dec(fUsbIfaces); t.str(" HID="); t.dec(fHidNubs); t.str(" KBD="); t.dec(fHidKbd);
        *t.p = '\0';
    }
    // Task counters, with the change since the previous probe (about one second).
    {
        Text t{line[7], sizeof(line[7])};
        static const char *const kWho[3] = {"WS", "ROS", "LW"};
        for (unsigned w = 0; w < 3; w++) {
            const TaskStat &c = fTask[w], &o = fTaskPrev[w];
            if (w) t.str(" | ");
            t.str(kWho[w]);
            if (!c.ok) { t.str(" -"); continue; }
            const bool same = o.ok && o.pid == c.pid;
            auto delta = [&](UInt32 now, UInt32 before) {
                t.ch('('); t.ch('+'); t.dec(same && now >= before ? now - before : 0); t.ch(')');
            };
            t.str(" csw="); t.dec(c.csw); delta(c.csw, o.csw);
            t.str(" msg="); t.dec(c.msent); t.ch('/'); t.dec(c.mrecv); delta(c.msent + c.mrecv, o.msent + o.mrecv);
            t.str(" sc="); t.dec(c.smach + c.sunix); delta(c.smach + c.sunix, o.smach + o.sunix);
            t.str(" cpu="); t.dec(c.cpuUs / 1000); t.str("ms");
        }
        *t.p = '\0';
    }

    // Property reads per user process.
    {
        Text t{line[8], sizeof(line[8])};
        t.str("IFS"); t.str(fNetIfList[0] ? fNetIfList : " -");
        t.str("   PROPS "); t.dec(fPropCount); t.str(":");
        for (unsigned k = 0; k < kPropProcs; k++) {
            const PropProc &e = fPropProcs[k];
            if (!e.count) continue;
            t.ch(' '); t.dec(static_cast<unsigned>(e.pid)); t.ch(':'); t.str(e.proc);
            t.str(" x"); t.dec(e.count); t.ch(' '); t.str(e.lastKey); t.ch(';');
        }
        if (fPropOther) { t.str(" other x"); t.dec(fPropOther); }
        *t.p = '\0';
    }

    // Ring, newest first: down the left column, then down the right one.
    const unsigned ringRows = kHudLines - 9;
    for (unsigned i = 0; i < kRingSize && i < ringRows * 2; i++) {
        const RingEntry &r = fRing[(fRingNext + kRingSize - 1 - i) % kRingSize];
        char cell[kRingCol + 1];
        Text t{cell, sizeof(cell)};
        if (r.kind) {
            t.dec(r.ms / 1000); t.ch('.');
            const UInt32 frac = r.ms % 1000;
            t.ch(static_cast<char>('0' + frac / 100)); t.ch(static_cast<char>('0' + frac / 10 % 10));
            t.ch(static_cast<char>('0' + frac % 10));
            t.ch(' '); t.ch(r.kind); t.ch(' '); t.str(r.cls); t.ch(' '); t.str(r.name);
        }
        *t.p = '\0';
        char *dst = line[9 + i % ringRows];
        size_t at = 0;
        while (dst[at]) at++;
        if (i >= ringRows) {                     // right column starts at a fixed position
            while (at < kRingCol) dst[at++] = ' ';
        }
        for (size_t k = 0; cell[k] && at < kHudCols; k++) dst[at++] = cell[k];
        dst[at] = '\0';
    }

    // Repaint only the lines whose text changed since they were last drawn.
    const unsigned tx = x0 + kHudPad * 2 + kHudBlink;
    for (unsigned i = 0; i < kHudLines; i++) {
        size_t k = 0;
        while (line[i][k] && line[i][k] == fDrawn[i][k]) k++;
        if (line[i][k] == fDrawn[i][k]) continue;       // identical, including the terminator
        const unsigned ly = y0 + kHudPad + i * kHudCellH;
        hudFill(tx, ly, fWidth - tx, kHudCellH, kHudBg);
        hudText(tx, ly, line[i]);
        for (k = 0; line[i][k] && k < kHudCols + 7; k++) fDrawn[i][k] = line[i][k];
        fDrawn[i][k] = '\0';
    }
    OSCompareAndSwap(1, 0, &fDrawBusy);
}

// Called at the top of every IOFramebuffer callback, before IOFramebuffer itself runs, and again on
// the way out of the ones that call into IOFramebuffer. Lock-free: if the machine stops inside a
// callback, this is the last thing that reached the screen.
void NVFramebuffer::hudMark(unsigned event, bool entering) {
    if (event >= kEvCount) return;
    if (entering) {
        fHudCount[event]++;
        fHudCalls++;
    }
    fHudLastEv = event;
    fHudLastIn = entering;
    if (!fHudMap) return;
    clock_sec_t now = 0;
    clock_usec_t usecs = 0;
    clock_get_system_microtime(&now, &usecs);
    hudDraw(now);
}

void NVFramebuffer::hudSelector(char kind, IOSelect selector) {
    fHudSelKind = kind;
    fHudSel = selector;
}

void NVFramebuffer::ringAdd(char kind, IOService *service) {
    clock_sec_t secs = 0;
    clock_usec_t usecs = 0;
    clock_get_system_microtime(&secs, &usecs);
    const uint64_t nowUs = static_cast<uint64_t>(secs) * 1000000u + usecs;

    RingEntry &r = fRing[fRingNext % kRingSize];
    r.kind = 0;   // Mark in-progress so a concurrent draw shows a blank rather than a mix
    r.ms = static_cast<UInt32>(nowUs / 1000);
    const char *cls = service->getMetaClass()->getClassName();
    const char *name = service->getName();
    size_t i = 0;
    for (; cls && cls[i] && i < sizeof(r.cls) - 1; i++) r.cls[i] = cls[i];
    r.cls[i] = '\0';
    i = 0;
    for (; name && name[i] && i < sizeof(r.name) - 1; i++) r.name[i] = name[i];
    r.name[i] = '\0';
    r.kind = kind;
    fRingNext++;

    if (!fHudMap || nowUs - fLastDrawUs < kHudMinRedrawUs) return;
    fLastDrawUs = nowUs;
    hudDraw(secs);
}

// Walk the whole service plane and keep the busy leaves: services with a non-zero busy count none of
// whose children is busy. IOKit propagates busy counts to every provider up to the root, so the
// leaves are where the outstanding matching or start work actually is.
void NVFramebuffer::walkBusy() {
    IOService *root = IOService::getServiceRoot();
    if (!root) return;
    IORegistryIterator *it = IORegistryIterator::iterateOver(root, gIOServicePlane, kIORegistryIterateRecursively);
    if (!it) return;
    BusyLeaf leaves[kLeafMax] = {};
    UcEntry clients[kClientMax] = {};
    Creator creators[kCreatorMax] = {};
    unsigned usbDevs = 0, usbIfaces = 0, hidNubs = 0, hidKbd = 0, xhci = 0;
    unsigned count = 0, total = 0, entries = 0, ucCount = 0, allClients = 0, nCreators = 0;
    unsigned ucTotal[3] = {};
    for (int attempt = 0; attempt < 3; attempt++) {
        count = total = entries = ucCount = allClients = nCreators = 0;
        ucTotal[0] = ucTotal[1] = ucTotal[2] = 0;
        while (IORegistryEntry *e = it->getNextObject()) {
            entries++;
            {
                // USB and HID inventory: class names say how far enumeration got, and a keyboard nub
                // reports usage page 1 / usage 6 whether or not anything in userspace reads it.
                const char *cn = e->getMetaClass()->getClassName();
                size_t cl = 0;
                while (cn && cn[cl] && cl < 64) cl++;
                if (containsText(cn, cl, "XHCI")) xhci++;
                if (containsText(cn, cl, "IOUSBHostDevice")) usbDevs++;
                if (containsText(cn, cl, "IOUSBHostInterface")) usbIfaces++;
                if (containsText(cn, cl, "HIDDevice") || containsText(cn, cl, "HIDInterface")) {
                    hidNubs++;
                    OSNumber *page = OSDynamicCast(OSNumber, e->getProperty("PrimaryUsagePage"));
                    OSNumber *usage = OSDynamicCast(OSNumber, e->getProperty("PrimaryUsage"));
                    if (page && usage && page->unsigned32BitValue() == 1 && usage->unsigned32BitValue() == 6) {
                        hidKbd++;
                    }
                }
            }
            // IOUserClientCreator is "pid <n>, <name>" - set when a process opens a user client.
            if (IOUserClient *uc = OSDynamicCast(IOUserClient, e)) {
                OSString *creator = OSDynamicCast(OSString, uc->getProperty("IOUserClientCreator"));
                const char *cs = creator ? creator->getCStringNoCopy() : nullptr;
                size_t cl = creator ? creator->getLength() : 0;
                allClients++;
                {
                    // "pid 233, WindowServer" -> "WindowServer"; no creator -> "?".
                    const char *nm = "?";
                    for (size_t k = 0; cs && k + 1 < cl; k++) {
                        if (cs[k] == ',' && cs[k + 1] == ' ') { nm = cs + k + 2; break; }
                    }
                    int slot = -1;
                    for (unsigned k = 0; k < nCreators; k++) {
                        size_t j = 0;
                        while (j < sizeof(creators[k].name) - 1 && nm[j] && creators[k].name[j] == nm[j]) j++;
                        if (creators[k].name[j] == '\0' && (nm[j] == '\0' || j == sizeof(creators[k].name) - 1)) {
                            slot = static_cast<int>(k);
                            break;
                        }
                    }
                    if (slot < 0 && nCreators < kCreatorMax) {
                        slot = static_cast<int>(nCreators++);
                        size_t j = 0;
                        for (; nm[j] && j < sizeof(creators[slot].name) - 1; j++) creators[slot].name[j] = nm[j];
                        creators[slot].name[j] = '\0';
                        creators[slot].count = 0;
                    }
                    if (slot >= 0) creators[slot].count++;
                }
                int who = -1;
                if (cs && containsText(cs, cl, "WindowServer")) who = 0;
                else if (cs && containsText(cs, cl, "recoveryosd")) who = 1;
                else if (cs && containsText(cs, cl, "loginwindow")) who = 2;
                if (who >= 0) {
                    ucTotal[who]++;
                    if (ucCount < kClientMax) {
                        UcEntry &u = clients[ucCount++];
                        u.who = "WRL"[who];
                        const char *ucls = uc->getMetaClass()->getClassName();
                        IOService *prov = uc->getProvider();
                        const char *pname = prov ? prov->getMetaClass()->getClassName() : "-";
                        size_t k = 0;
                        for (; ucls && ucls[k] && k < sizeof(u.cls) - 1; k++) u.cls[k] = ucls[k];
                        u.cls[k] = '\0';
                        k = 0;
                        for (; pname && pname[k] && k < sizeof(u.prov) - 1; k++) u.prov[k] = pname[k];
                        u.prov[k] = '\0';
                    }
                }
            }
            IOService *svc = OSDynamicCast(IOService, e);
            if (!svc) continue;
            const UInt32 busy = svc->getBusyState();
            if (!busy) continue;
            bool childBusy = false;
            if (OSIterator *ci = svc->getChildIterator(gIOServicePlane)) {
                while (OSObject *c = ci->getNextObject()) {
                    IOService *cs = OSDynamicCast(IOService, c);
                    if (cs && cs->getBusyState()) { childBusy = true; break; }
                }
                ci->release();
            }
            if (childBusy) continue;
            total++;
            if (count >= kLeafMax) continue;
            BusyLeaf &l = leaves[count++];
            l.busy = busy;
            const char *cls = svc->getMetaClass()->getClassName();
            const char *name = svc->getName();
            IOService *prov = svc->getProvider();
            const char *pname = prov ? prov->getName() : "-";
            size_t k = 0;
            for (; cls && cls[k] && k < sizeof(l.cls) - 1; k++) l.cls[k] = cls[k];
            l.cls[k] = '\0';
            k = 0;
            for (; name && name[k] && k < sizeof(l.name) - 1; k++) l.name[k] = name[k];
            l.name[k] = '\0';
            k = 0;
            for (; pname && pname[k] && k < sizeof(l.prov) - 1; k++) l.prov[k] = pname[k];
            l.prov[k] = '\0';
        }
        if (it->isValid()) break;      // the registry changed under us: walk again
        it->reset();
    }
    it->release();
    for (unsigned i = 0; i < count; i++) fLeaves[i] = leaves[i];
    fLeafCount = count;
    fLeafTotal = total;
    fWalkEntries = entries;
    fUsbDevs = usbDevs; fUsbIfaces = usbIfaces; fHidNubs = hidNubs; fHidKbd = hidKbd; fXhci = xhci;
    for (unsigned i = 0; i < ucCount; i++) fClients[i] = clients[i];
    fClientCount = ucCount;
    for (unsigned i = 0; i < 3; i++) fClientTotal[i] = ucTotal[i];
    fClientAll = allClients;
    for (unsigned i = 0; i < nCreators; i++) fCreators[i] = creators[i];
    fCreatorCount = nCreators;
}

// Task-level counters of one process. proc_find() holds the proc while its task is queried.
void NVFramebuffer::readTask(int pid, TaskStat &out) {
    out = TaskStat{};
    out.pid = pid;
    if (pid <= 0) return;
    proc_t proc = proc_find(pid);
    if (!proc) return;
    if (task_t task = proc_task(proc)) {
        task_events_info_data_t ev;
        mach_msg_type_number_t n = TASK_EVENTS_INFO_COUNT;
        if (task_info(task, TASK_EVENTS_INFO, reinterpret_cast<task_info_t>(&ev), &n) == KERN_SUCCESS) {
            out.csw = static_cast<UInt32>(ev.csw);
            out.msent = static_cast<UInt32>(ev.messages_sent);
            out.mrecv = static_cast<UInt32>(ev.messages_received);
            out.smach = static_cast<UInt32>(ev.syscalls_mach);
            out.sunix = static_cast<UInt32>(ev.syscalls_unix);
            out.ok = true;
        }
        task_thread_times_info_data_t tt;
        n = TASK_THREAD_TIMES_INFO_COUNT;
        if (task_info(task, TASK_THREAD_TIMES_INFO, reinterpret_cast<task_info_t>(&tt), &n) == KERN_SUCCESS) {
            out.cpuUs = static_cast<uint64_t>(tt.user_time.seconds + tt.system_time.seconds) * 1000000u +
                        static_cast<uint64_t>(tt.user_time.microseconds + tt.system_time.microseconds);
        }
    }
    proc_rele(proc);
}

void NVFramebuffer::propSeen(const char *key) {
    const int pid = proc_selfpid();
    if (pid <= 0) return;                          // kernel threads: IOKit's own reads
    fPropCount++;
    PropProc *slot = nullptr;
    for (unsigned k = 0; k < kPropProcs && !slot; k++) {
        if (fPropProcs[k].count && fPropProcs[k].pid == pid) slot = &fPropProcs[k];
    }
    for (unsigned k = 0; k < kPropProcs && !slot; k++) {
        if (!fPropProcs[k].count) {
            slot = &fPropProcs[k];
            slot->pid = pid;
            proc_selfname(slot->proc, sizeof(slot->proc));
        }
    }
    if (!slot) { fPropOther++; return; }
    size_t k = 0;
    for (; key && key[k] && k < sizeof(slot->lastKey) - 1; k++) slot->lastKey[k] = key[k];
    slot->lastKey[k] = '\0';
    slot->count++;
}

bool NVFramebuffer::publishHandler(void *target, void *, IOService *service, IONotifier *) {
    NVFramebuffer *fb = static_cast<NVFramebuffer *>(target);
    fb->fPubCount++;
    fb->ringAdd('P', service);
    return true;
}

bool NVFramebuffer::matchedHandler(void *target, void *, IOService *service, IONotifier *) {
    NVFramebuffer *fb = static_cast<NVFramebuffer *>(target);
    fb->fMatchCount++;
    fb->ringAdd('M', service);
    return true;
}

bool NVFramebuffer::serviceExists(const char *className) {
    OSDictionary *match = IOService::serviceMatching(className);
    if (!match) return false;
    IOService *svc = IOService::copyMatchingService(match);
    match->release();
    if (!svc) return false;
    svc->release();
    return true;
}

// Runs on its own thread call so that, if it ever blocks (process or registry locks), the 1 Hz tick
// keeps drawing and AGE grows - which is itself the diagnosis.
void NVFramebuffer::hudProbe(clock_sec_t now) {
    unsigned procs = 0;
    int ws = -1, hidd = -1, logd = -1, rosd = -1, lw = -1;
    char name[32];
    // Pids are scanned in ascending order, so a small ring of the last ones seen holds the newest.
    int newPid[kNewProcs] = {};
    char newName[kNewProcs][17] = {};
    unsigned newSeen = 0;
    for (int pid = 0; pid < kHudScanPids; pid++) {
        proc_name(pid, name, sizeof(name));
        if (!name[0]) continue;
        procs++;
        {
            const unsigned slot = newSeen % kNewProcs;
            newPid[slot] = pid;
            size_t k = 0;
            for (; name[k] && k < sizeof(newName[slot]) - 1; k++) newName[slot][k] = name[k];
            newName[slot][k] = '\0';
            newSeen++;
        }
        if (ws < 0 && containsText(name, sizeof(name), "WindowServer")) ws = pid;
        if (hidd < 0 && containsText(name, sizeof(name), "hidd")) hidd = pid;
        if (logd < 0 && containsText(name, sizeof(name), "logd")) logd = pid;
        if (rosd < 0 && containsText(name, sizeof(name), "recoveryosd")) rosd = pid;
        if (lw < 0 && containsText(name, sizeof(name), "loginwindow")) lw = pid;
    }
    fHudProcs = procs;
    // Publish newest first.
    const unsigned have = newSeen < kNewProcs ? newSeen : kNewProcs;
    for (unsigned i = 0; i < have; i++) {
        const unsigned slot = (newSeen - 1 - i) % kNewProcs;
        fNewPid[i] = newPid[slot];
        size_t k = 0;
        for (; newName[slot][k] && k < sizeof(fNewName[i]) - 1; k++) fNewName[i][k] = newName[slot][k];
        fNewName[i][k] = '\0';
    }
    fNewCount = have;
    fHudWs = ws; fHudHidd = hidd; fHudLogd = logd; fHudRosd = rosd; fHudLw = lw;
    if (ws > 0) {
        bool known = false;
        for (unsigned k = 0; k < fWsPidCount; k++) known = known || fWsPid[k] == ws;
        if (!known && fWsPidCount < kWsPidMax) {
            fWsPid[fWsPidCount] = ws;
            fWsSeenAt[fWsPidCount] = static_cast<UInt32>(now);
            fWsPidCount++;
        }
    }
    const int watched[3] = {ws, rosd, lw};
    for (unsigned w = 0; w < 3; w++) {
        fTaskPrev[w] = fTask[w];
        readTask(watched[w], fTask[w]);
    }
    fHudX86 = serviceExists("X86PlatformPlugin");
    fHudAcpiSmc = serviceExists("ACPI_SMC_PlatformPlugin");
    fHudHidSystem = serviceExists("IOHIDSystem");
    walkBusy();
    for (unsigned i = 0; i < kAuxDriverCount; i++) fHudAux[i] = serviceExists(kAuxDrivers[i].className);
    fProbeAt = now;
    fProbeRuns++;
}

void NVFramebuffer::hudCallout(thread_call_param_t self, thread_call_param_t) {
    NVFramebuffer *fb = static_cast<NVFramebuffer *>(self);
    clock_sec_t now = 0;
    clock_usec_t usecs = 0;
    clock_get_system_microtime(&now, &usecs);
    fb->fHudTicks++;
    fb->hudDraw(now);
    IOLockLock(fb->fTraceLock);
    if (!fb->fStopping) {
        uint64_t deadline = 0;
        clock_interval_to_deadline(kHudTickMs, kMillisecondScale, &deadline);
        thread_call_enter_delayed(fb->fHudCall, deadline);
    }
    IOLockUnlock(fb->fTraceLock);
}

void NVFramebuffer::probeCallout(thread_call_param_t self, thread_call_param_t) {
    NVFramebuffer *fb = static_cast<NVFramebuffer *>(self);
    clock_sec_t now = 0;
    clock_usec_t usecs = 0;
    clock_get_system_microtime(&now, &usecs);
    fb->hudProbe(now);
    IOLockLock(fb->fTraceLock);
    if (!fb->fStopping) {
        uint64_t deadline = 0;
        clock_interval_to_deadline(kHudProbeMs, kMillisecondScale, &deadline);
        thread_call_enter_delayed(fb->fProbeCall, deadline);
    }
    IOLockUnlock(fb->fTraceLock);
}

// ---- UDP log and WATCH line ---------------------------------------------------------------------- A
// dedicated kernel thread rather than a thread call: the HUD tick and the probe are thread calls from
// one shared pool, so this loop keeps running (and says so on screen and on the network) if that pool
// stalls. Real-time policy, 2 ms of work per 200 ms period, so a CPU-bound spinner cannot starve it.

void NVFramebuffer::netThreadMain(void *param, wait_result_t) {
    NVFramebuffer *fb = static_cast<NVFramebuffer *>(param);
    uint64_t period = 0, computation = 0, constraint = 0;
    nanoseconds_to_absolutetime(static_cast<uint64_t>(kNetTickMs) * 1000000u, &period);
    nanoseconds_to_absolutetime(2000000u, &computation);
    nanoseconds_to_absolutetime(10000000u, &constraint);
    thread_time_constraint_policy_data_t tc;
    tc.period = static_cast<uint32_t>(period);
    tc.computation = static_cast<uint32_t>(computation);
    tc.constraint = static_cast<uint32_t>(constraint);
    tc.preemptible = TRUE;
    fb->fNetPolicy = thread_policy_set(current_thread(), THREAD_TIME_CONSTRAINT_POLICY,
                                       reinterpret_cast<thread_policy_t>(&tc), THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    while (!fb->fStopping) {
        clock_sec_t secs = 0;
        clock_usec_t usecs = 0;
        clock_get_system_microtime(&secs, &usecs);
        fb->netTick(static_cast<uint64_t>(secs) * 1000u + usecs / 1000u);
        IOSleep(kNetTickMs);
    }
    OSCompareAndSwap(0, 1, &fb->fNetThreadDone);
    thread_terminate(current_thread());
}

// One row below the HUD lines, drawn only by the net thread: its own uptime and iteration count, the HUD
// tick count it last saw and for how long that has not changed (TCAGE), and the GPU read-back tally.
void NVFramebuffer::watchDraw(UInt32 nowS) {
    fWatchBeats++;
    if (fHudTicks != fTcSeen) { fTcSeen = fHudTicks; fTcSeenAt = nowS; }
    if (!fHudMap || fWidth < 1024 || fHeight < kHudBand * 2) return;
    const unsigned y = fHeight - kHudBand + kHudPad + kHudLines * kHudCellH;
    hudFill(kHudPad, y, kHudBlink, kHudBlink, (fWatchBeats & 1) ? kHudOn : kHudOff);
    char text[kHudCols + 8];
    Text t{text, sizeof(text)};
    t.str("WATCH T="); t.dec(nowS); t.str(" BEAT="); t.dec(fWatchBeats); t.str(" TC="); t.dec(fTcSeen);
    t.str(" TCAGE="); t.dec(nowS >= fTcSeenAt ? nowS - fTcSeenAt : 0);
    t.str(" RB="); t.dec(fRbOk); t.ch('/'); t.dec(fRbBad);
    t.str(" RT="); t.dec(fNetPolicy == KERN_SUCCESS ? 1 : 0); t.str(" TX="); t.dec(fNetSeq);
    *t.p = '\0';
    const unsigned tx = kHudPad * 2 + kHudBlink;
    hudFill(tx, y, fWidth - tx, kHudCellH, kHudBg);
    hudText(tx, y, text);
}

// The order matters: the datagrams go out first, because they are the evidence that this thread ran at
// all. The GPU read-back and the on-screen row come after - if either of those ever wedges, the send for
// that tick has already happened.
void NVFramebuffer::netTick(uint64_t nowMs) {
    const UInt32 nowS = static_cast<UInt32>(nowMs / 1000);
    const bool second = (fNetTicks++ % (1000 / kNetTickMs)) == 0;
    netSendTick(nowMs, nowS, second);
    if (second) gpuCheck(nowS);
    watchDraw(nowS);
}

void NVFramebuffer::netSendTick(uint64_t nowMs, UInt32 nowS, bool second) {
    // Launchd running means bsd_init has finished, so the socket layer is initialized.
    if (proc_t launchd = proc_find(1)) proc_rele(launchd);
    else return;
    if (second || !fNetIp) netScanIf(nowS);
    if (!fNetIp && !fNetSelfDone && nowS >= kNetSelfAtS && second) netSelfConfig(nowS);
    if (!fNetIp) return;
    if (!fNetSock) {
        const errno_t e = sock_socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, nullptr, &fNetSock);
        if (e || !fNetSock) { fNetErr = e; fNetFail++; fNetSock = nullptr; return; }
    }
    if (!fNetHello) {
        // One small datagram first: it resolves the gateway's ARP entry before the backlog goes out.
        Text t{fNetBuf, sizeof(fNetBuf)};
        t.str("NVFB H "); t.dec(fNetSeq); t.ch(' '); t.dec(nowMs); t.str(" NVFramebuffer 0.24.0 ");
        t.str(fNetIfName); t.str(" up at "); t.dec(fNetIpAt); t.str(" s\n");
        fNetHello = netSend(fNetBuf, static_cast<size_t>(t.p - fNetBuf));
        return;
    }
    netSendLog(nowMs);
    if (nowS != fNetStatusAt) {
        fNetStatusAt = nowS;
        netSendStatus(nowMs);
    }
}

// Give the first Ethernet interface an address and a route to the Mac's subnet, so the log does not wait
// for DHCP. This is what ifconfig and route(8) do, through the same kernel entry points: SIOCAIFADDR on an
// inet socket, then an RTM_ADD message on a routing socket. Both are additive; a DHCP address configd sets
// later is a second address on the same interface.
void NVFramebuffer::netSelfConfig(UInt32 nowS) {
    fNetSelfTries++;
    // Only a real wired NIC. Once we picked XHC0 (the USB controller's virtual Ethernet, MTU 0) and the first
    // datagram tripped an assertion in ip_output_list. So: name "en", Ethernet type, sane MTU, but not "link
    // up", because nobody has brought the NIC up yet at this point and waiting for a link that only shows up
    // after that never ends. We bring it up ourselves and the sends keep retrying till the link is there.
    char name[IFNAMSIZ] = {};
    {
        ifnet_t *list = nullptr;
        u_int32_t count = 0;
        if (ifnet_list_get(IFNET_FAMILY_ANY, &list, &count) != 0 || !list) return;
        Text seen{fNetIfList, sizeof(fNetIfList)};
        for (u_int32_t i = 0; i < count; i++) {
            ifnet_t ifp = list[i];
            const u_int16_t flags = ifnet_flags(ifp);
            const char *n = ifnet_name(ifp);
            if (flags & IFF_LOOPBACK) continue;
            if (seen.left > 24) {
                seen.ch(' '); seen.str(n ? n : "?"); seen.dec(ifnet_unit(ifp));
                seen.ch(':'); seen.hexw(flags, 4); seen.ch('/'); seen.dec(ifnet_mtu(ifp));
                seen.ch('/'); seen.dec(ifnet_type(ifp));
            }
            if (name[0]) continue;
            if (ifnet_type(ifp) != IFT_ETHER) continue;
            if (ifnet_mtu(ifp) < kNetMinMtu) continue;
            if (!n || n[0] != 'e' || n[1] != 'n' || n[2]) continue;
            Text t{name, sizeof(name)};
            t.str(n);
            t.dec(ifnet_unit(ifp));
            *t.p = '\0';
        }
        *seen.p = '\0';
        ifnet_list_free(list);
    }
    if (!name[0]) return;

    socket_t so = nullptr;
    if (sock_socket(PF_INET, SOCK_DGRAM, 0, nullptr, nullptr, &so) != 0 || !so) return;

    struct ifreq ifr;
    bzero(&ifr, sizeof(ifr));
    for (size_t k = 0; name[k] && k < IFNAMSIZ - 1; k++) ifr.ifr_name[k] = name[k];
    if (sock_ioctl(so, SIOCGIFFLAGS, &ifr) == 0 && !(ifr.ifr_flags & IFF_UP)) {
        ifr.ifr_flags |= IFF_UP;
        sock_ioctl(so, SIOCSIFFLAGS, &ifr);
    }

    struct in_aliasreq ifra;
    bzero(&ifra, sizeof(ifra));
    for (size_t k = 0; name[k] && k < IFNAMSIZ - 1; k++) ifra.ifra_name[k] = name[k];
    auto setSin = [](struct sockaddr_in &sin, UInt32 host) {
        sin.sin_len = sizeof(struct sockaddr_in);
        sin.sin_family = AF_INET;
        sin.sin_addr.s_addr = OSSwapHostToBigInt32(host);
    };
    setSin(ifra.ifra_addr, kNetSelfAddr);
    setSin(ifra.ifra_mask, kNetSelfMask);
    setSin(ifra.ifra_broadaddr, (kNetSelfAddr & kNetSelfMask) | ~kNetSelfMask);
    const errno_t addrErr = sock_ioctl(so, SIOCAIFADDR, &ifra);
    sock_close(so);
    if (addrErr && addrErr != EEXIST) { fNetSelfErr = addrErr; return; }

    socket_t rs = nullptr;
    if (sock_socket(PF_ROUTE, SOCK_RAW, 0, nullptr, nullptr, &rs) == 0 && rs) {
        struct RouteMsg {
            struct rt_msghdr hdr;
            struct sockaddr_in dst, gw, mask;
        } msg;
        bzero(&msg, sizeof(msg));
        msg.hdr.rtm_msglen = sizeof(msg);
        msg.hdr.rtm_version = RTM_VERSION;
        msg.hdr.rtm_type = RTM_ADD;
        msg.hdr.rtm_flags = RTF_UP | RTF_GATEWAY | RTF_STATIC;
        msg.hdr.rtm_addrs = RTA_DST | RTA_GATEWAY | RTA_NETMASK;
        msg.hdr.rtm_seq = 1;
        setSin(msg.dst, kNetDstNet);
        setSin(msg.gw, kNetGateway);
        setSin(msg.mask, kNetSelfMask);
        struct iovec iov;
        iov.iov_base = &msg;
        iov.iov_len = sizeof(msg);
        struct msghdr m;
        bzero(&m, sizeof(m));
        m.msg_iov = &iov;
        m.msg_iovlen = 1;
        size_t sent = 0;
        const errno_t routeErr = sock_send(rs, &m, 0, &sent);
        sock_close(rs);
        if (routeErr && routeErr != EEXIST) fNetSelfErr = routeErr;
    }
    fNetSelfDone = true;
    fNetSelfAt = nowS;
    IOLog("NVFramebuffer: configured %s with the fixed log address + route at %us (err %d)\n",
          name, nowS, fNetSelfErr);
}

void NVFramebuffer::netScanIf(UInt32 nowS) {
    ifnet_t *list = nullptr;
    u_int32_t count = 0;
    if (ifnet_list_get(IFNET_FAMILY_ANY, &list, &count) != 0 || !list) return;
    UInt32 ip = 0;
    unsigned ifs = 0;
    char name[sizeof(fNetIfName)] = {};
    for (u_int32_t i = 0; i < count; i++) {
        ifnet_t ifp = list[i];
        if (ifnet_flags(ifp) & IFF_LOOPBACK) continue;
        ifs++;
        if (ip) continue;
        ifaddr_t *addrs = nullptr;
        if (ifnet_get_address_list_family(ifp, &addrs, AF_INET) != 0 || !addrs) continue;
        for (unsigned k = 0; addrs[k] && !ip; k++) {
            struct sockaddr_in sin;
            bzero(&sin, sizeof(sin));
            if (ifaddr_address(addrs[k], reinterpret_cast<struct sockaddr *>(&sin), sizeof(sin)) != 0) continue;
            if (sin.sin_family != AF_INET) continue;
            const UInt32 a = OSSwapBigToHostInt32(sin.sin_addr.s_addr);
            if (!a || (a >> 16) == 0xA9FE) continue;       // unset or link-local: no route to the Mac
            ip = a;
            Text t{name, sizeof(name)};
            t.str(ifnet_name(ifp)); t.dec(ifnet_unit(ifp));
            *t.p = '\0';
        }
        ifnet_free_address_list(addrs);
    }
    ifnet_list_free(list);
    fNetIfCount = ifs;
    if (ip && !fNetIp) fNetIpAt = nowS;
    if (ip) {
        size_t k = 0;
        for (; name[k] && k < sizeof(fNetIfName) - 1; k++) fNetIfName[k] = name[k];
        fNetIfName[k] = '\0';
    }
    fNetIp = ip;
}

// Command register and BAR1 as programmed now, and whether a CPU write to VRAM still reads back. The
// pixel is the last one of the last row's padding (rowBytes 16384 = 4096 px for a 3840 px mode), which
// is never scanned out, so the test is invisible and nothing else draws there.
void NVFramebuffer::gpuCheck(UInt32 nowS) {
    if (!fPCI) return;
    fGpuCmd = fPCI->configRead16(kIOPCIConfigCommand);
    fGpuBar1Lo = fPCI->configRead32(kIOPCIConfigBaseAddress1);
    fGpuBar1Hi = fPCI->configRead32(kIOPCIConfigBaseAddress2);
    if (!fHudMap) return;
    const uintptr_t base = static_cast<uintptr_t>(fHudMap->getVirtualAddress());
    const uintptr_t off = static_cast<uintptr_t>(fHeight - 1) * fRowBytes + fRowBytes - 4;
    volatile UInt32 *px = reinterpret_cast<volatile UInt32 *>(base + off);
    const UInt32 want = 0x00A5A500u | ((fRbOk + fRbBad) & 0xFF);
    *px = want;
    __asm__ volatile("mfence" ::: "memory");
    const UInt32 got = *px;
    fRbGot = got;
    if (got == want) {
        fRbOk++;
    } else {
        if (!fRbBad) fRbBadAt = nowS;
        fRbBad++;
    }
}

bool NVFramebuffer::netSend(const char *buf, size_t len) {
    struct sockaddr_in dst;
    bzero(&dst, sizeof(dst));
    dst.sin_len = sizeof(dst);
    dst.sin_family = AF_INET;
    dst.sin_port = OSSwapHostToBigInt16(kNetPort);
    dst.sin_addr.s_addr = OSSwapHostToBigInt32(kNetDstAddr);
    struct iovec iov;
    iov.iov_base = const_cast<char *>(buf);
    iov.iov_len = len;
    struct msghdr msg;
    bzero(&msg, sizeof(msg));
    msg.msg_name = &dst;
    msg.msg_namelen = sizeof(dst);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    size_t sent = 0;
    const errno_t e = sock_send(fNetSock, &msg, MSG_DONTWAIT, &sent);
    if (e) {
        fNetErr = e;
        fNetFail++;
        return false;
    }
    fNetSeq++;
    return true;
}

// The kernel message buffer from where the last datagram stopped. The first datagram starts at the oldest
// byte (index 0 until the ring wraps; after that the write position). "NVFB L <seq> <ms> <index>" heads
// each datagram so the receiver can see gaps.
void NVFramebuffer::netSendLog(uint64_t nowMs) {
    NVMsgbuf *mb = msgbufp;
    if (!mb || mb->msg_magic != kMsgMagic || !mb->msg_bufc || mb->msg_size <= 0) return;
    const int size = mb->msg_size;
    const int wr = mb->msg_bufx;
    if (wr < 0 || wr >= size) return;
    if (fNetCursor < 0 || fNetCursor >= size) fNetCursor = mb->msg_bufc[wr] ? wr : 0;
    for (unsigned n = 0; n < kNetBurst && fNetCursor != wr; n++) {
        Text t{fNetBuf, sizeof(fNetBuf)};
        t.str("NVFB L "); t.dec(fNetSeq); t.ch(' '); t.dec(nowMs); t.ch(' ');
        t.dec(static_cast<unsigned>(fNetCursor)); t.ch('\n');
        int pos = fNetCursor;
        unsigned body = 0;
        while (pos != wr && body < kNetPayload) {
            const char c = mb->msg_bufc[pos];
            if (c) { t.ch(c); body++; }
            pos = (pos + 1) % size;
        }
        if (!netSend(fNetBuf, static_cast<size_t>(t.p - fNetBuf))) return;   // same bytes next tick
        fNetCursor = pos;
    }
}

// Once a second: a live line computed now (independent of whether the HUD tick still runs), then the
// HUD lines as last drawn. "NVFB S <seq> <ms> <part>".
void NVFramebuffer::netSendStatus(uint64_t nowMs) {
    unsigned part = 0;
    Text t{fNetBuf, sizeof(fNetBuf)};
    auto header = [&]() {
        t.p = fNetBuf; t.left = sizeof(fNetBuf);
        t.str("NVFB S "); t.dec(fNetSeq); t.ch(' '); t.dec(nowMs); t.ch(' '); t.dec(part++); t.ch('\n');
    };
    header();
    t.str("LIVE ms="); t.dec(nowMs); t.str(" hudTicks="); t.dec(fHudTicks); t.str(" probeRuns=");
    t.dec(fProbeRuns); t.str(" probeAt="); t.dec(fProbeAt); t.str(" procs="); t.dec(fHudProcs);
    t.str(" ws="); t.dec(static_cast<uint64_t>(fHudWs < 0 ? 0 : fHudWs));
    t.str(" wsCsw="); t.dec(fTask[0].csw); t.str(" wsSc="); t.dec(fTask[0].smach + fTask[0].sunix);
    t.str(" rosCsw="); t.dec(fTask[1].csw); t.str(" pub="); t.dec(fPubCount); t.str(" mat=");
    t.dec(fMatchCount);
    if (IOService *root = IOService::getServiceRoot()) { t.str(" busy="); t.dec(root->getBusyState()); }
    t.str(" gpuCmd="); t.hexw(fGpuCmd, 4); t.str(" bar1="); t.hexw(fGpuBar1Hi, 8); t.hexw(fGpuBar1Lo, 8);
    t.str(" rb="); t.dec(fRbOk); t.ch('/'); t.dec(fRbBad); t.str(" rbGot="); t.hex(fRbGot);
    t.str(" txFail="); t.dec(fNetFail); t.str(" err="); t.dec(static_cast<unsigned>(fNetErr));
    t.str(" watchBeats="); t.dec(fWatchBeats); t.str(" tcAge="); t.dec(nowMs / 1000 >= fTcSeenAt ? nowMs / 1000 - fTcSeenAt : 0);
    t.str(" rt="); t.dec(fNetPolicy == KERN_SUCCESS ? 1 : 0); t.ch('\n');
    for (unsigned i = 0; i < kHudLines; i++) {
        size_t len = 0;
        while (len < kHudCols + 7 && fDrawn[i][len]) len++;
        if (t.left <= len + 2) {
            if (!netSend(fNetBuf, static_cast<size_t>(t.p - fNetBuf))) return;
            header();
        }
        for (size_t k = 0; k < len; k++) t.ch(fDrawn[i][k]);
        t.ch('\n');
    }
    netSend(fNetBuf, static_cast<size_t>(t.p - fNetBuf));
}

bool NVFramebuffer::put(const char *name, const void *buf, size_t len) {
    if (!fNvram) return false;
    return PEWriteNVRAMProperty(name, buf, static_cast<unsigned>(len));
}

// Console report. The verbose console stays visible until WindowServer takes the display, so this is
// the channel while NVRAM is full: one summary line every beat, the full list when it changes.
void NVFramebuffer::logStatus(clock_sec_t now) {
    int keyPid[kKeyProcCount];
    for (unsigned k = 0; k < kKeyProcCount; k++) keyPid[k] = -1;
    Text list{fSysBuf, kPsBytes};
    unsigned count = 0;
    char name[32];
    for (int pid = 0; pid < kScanPids; pid++) {
        proc_name(pid, name, sizeof(name));
        if (!name[0]) continue;
        count++;
        for (unsigned k = 0; k < kKeyProcCount; k++) {
            if (keyPid[k] < 0 && containsText(name, sizeof(name), kKeyProcs[k].name)) keyPid[k] = pid;
        }
        list.dec(static_cast<unsigned>(pid)); list.ch(':'); list.str(name); list.ch(' ');
    }
    *list.p = '\0';
    const size_t listLen = static_cast<size_t>(list.p - fSysBuf);

    UInt32 events[kEvCount];
    IOLockLock(fTraceLock);
    for (unsigned i = 0; i < kEvCount; i++) events[i] = fEvents[i];
    const UInt32 ucTypes = fUserClientTypes;
    const IOReturn ucResult = fUserClientResult;
    const IOReturn openResult = fOpenResult;
    IOLockUnlock(fTraceLock);

    char line[256];
    Text t{line, sizeof(line)};
    t.str("NVFB "); t.dec(now); t.str("s procs="); t.dec(count);
    for (unsigned k = 0; k < kKeyProcCount; k++) {
        t.ch(' '); t.str(kKeyProcs[k].label); t.ch('=');
        if (keyPid[k] < 0) t.ch('-'); else t.dec(static_cast<unsigned>(keyPid[k]));
    }
    t.str(" | fb");
    for (unsigned i = 0; i < kEvCount; i++) {
        if (!events[i]) continue;
        t.ch(' '); t.str(kEvNames[i]); t.ch('='); t.dec(events[i]);
    }
    if (ucTypes) { t.str(" ut="); t.hex(ucTypes); t.str(" ur="); t.hex(static_cast<UInt32>(ucResult)); }
    if (openResult) { t.str(" or="); t.hex(static_cast<UInt32>(openResult)); }
    *t.p = '\0';
    IOLog("%s\n", line);

    const UInt32 hash = fnv1a(fSysBuf, listLen) ^ count;
    if (hash == fPrintedPsHash || fPsPrints >= kMaxPsPrints) return;
    fPrintedPsHash = hash;
    fPsPrints++;
    // IOLog lines are bounded; print the list in slices a photo can still read.
    constexpr size_t kSlice = 200;
    for (size_t off = 0; off < listLen; off += kSlice) {
        const size_t n = listLen - off < kSlice ? listLen - off : kSlice;
        char slice[kSlice + 1];
        for (size_t i = 0; i < n; i++) slice[i] = fSysBuf[off + i];
        slice[n] = '\0';
        IOLog("NVFB ps%u: %s\n", fPsPrints, slice);
    }
}

// Split `buf` across <prefix>0, <prefix>1, ... so no single NVRAM variable exceeds kVarBytes.
void NVFramebuffer::writeChunks(const char *prefix, const char *buf, size_t len, unsigned maxVars) {
    char name[32];
    for (unsigned i = 0; i < maxVars && len; i++) {
        Text n{name, sizeof(name)};
        n.str(prefix); n.dec(i);
        *n.p = '\0';
        const size_t part = len < kVarBytes ? len : kVarBytes;
        put(name, buf, part);
        buf += part;
        len -= part;
    }
}

void NVFramebuffer::writePci(clock_sec_t now) {
    Text t{fBig, kBigBytes};
    t.str("up="); t.dec(now); t.ch('\n');
    unsigned count = 0;
    OSDictionary *match = IOService::serviceMatching("IOPCIDevice");
    OSIterator *it = match ? IOService::getMatchingServices(match) : nullptr;
    OSSafeReleaseNULL(match);
    if (it) {
        while (OSObject *o = it->getNextObject()) {
            IOPCIDevice *d = OSDynamicCast(IOPCIDevice, o);
            if (!d) continue;
            describePci(t, d);
            count++;
        }
        it->release();
    }
    t.str("count="); t.dec(count);
    *t.p = '\0';
    writeChunks("nvfb-pci", fBig, static_cast<size_t>(t.p - fBig), kPciVars);
}

void NVFramebuffer::writeStorage(clock_sec_t now) {
    Text t{fBig, kVarBytes};
    t.str("up="); t.dec(now); t.ch('\n');
    if (IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane)) {
        appendProperty(t, chosen, "boot-uuid", 64);
        appendProperty(t, chosen, "root-matching", 700);
        chosen->release();
    } else {
        t.str("/chosen missing\n");
    }
    unsigned count = 0;
    OSDictionary *match = IOService::serviceMatching("IOMedia");
    OSIterator *it = match ? IOService::getMatchingServices(match) : nullptr;
    OSSafeReleaseNULL(match);
    if (it) {
        while (OSObject *o = it->getNextObject()) {
            IORegistryEntry *m = OSDynamicCast(IORegistryEntry, o);
            if (!m) continue;
            count++;
            t.str(stringProperty(m, "BSD Name"));
            if (OSNumber *size = OSDynamicCast(OSNumber, m->getProperty("Size"))) {
                t.ch(' '); t.dec(size->unsigned64BitValue() >> 20); t.str("MB");
            }
            OSBoolean *whole = OSDynamicCast(OSBoolean, m->getProperty("Whole"));
            t.str(whole && whole->isTrue() ? " W " : " p ");
            t.str(stringProperty(m, "Content")); t.ch(' '); t.str(stringProperty(m, "UUID"));
            // Owning PCI function, and the product name the block device reports.
            const char *product = nullptr;
            IORegistryEntry *e = m;
            for (int depth = 0; depth < 24 && e; depth++) {
                e = e->getParentEntry(gIOServicePlane);
                if (!e) break;
                if (!product) {
                    if (OSDictionary *dc = OSDynamicCast(OSDictionary, e->getProperty("Device Characteristics"))) {
                        if (OSString *pn = OSDynamicCast(OSString, dc->getObject("Product Name"))) {
                            product = pn->getCStringNoCopy();
                        }
                    }
                }
                if (IOPCIDevice *pd = OSDynamicCast(IOPCIDevice, e)) {
                    t.str(" pci="); t.hexw(pd->getBusNumber(), 2); t.ch(':');
                    t.hexw(pd->getDeviceNumber(), 2); t.ch('.'); t.hexw(pd->getFunctionNumber(), 1);
                    break;
                }
            }
            if (product) { t.ch(' '); t.str(product); }
            t.ch('\n');
        }
        it->release();
    }
    t.str("media="); t.dec(count);
    *t.p = '\0';
    put("nvfb-stor", fBig, static_cast<size_t>(t.p - fBig));
}

// Kernel log lines relevant to storage and the root device, from the whole message buffer.
void NVFramebuffer::writeDmesg(unsigned beat, clock_sec_t now) {
    NVMsgbuf *mb = msgbufp;
    if (!mb || mb->msg_magic != kMsgMagic || !mb->msg_bufc || mb->msg_size <= 0) {
        static const char kBad[] = "msgbuf unavailable or magic mismatch";
        put("nvfb-dmesg0", kBad, sizeof(kBad) - 1);
        return;
    }
    const int size = mb->msg_size;
    int start = mb->msg_bufx;
    if (start < 0 || start >= size) start = 0;

    size_t ringPos = 0, ringLen = 0, lineLen = 0;
    unsigned dropped = 0;
    auto emit = [&](void) {
        bool keep = false;
        for (const char *k : kDmesgKeep) {
            if (containsText(fDmesgLine, lineLen, k)) { keep = true; break; }
        }
        if (!keep) { dropped++; lineLen = 0; return; }
        for (size_t j = 0; j < lineLen; j++) {
            fDmesgRing[ringPos] = fDmesgLine[j];
            ringPos = (ringPos + 1) % kDmesgBytes;
            if (ringLen < kDmesgBytes) ringLen++;
        }
        lineLen = 0;
    };
    // Racy by design: other CPUs keep logging while we copy. A torn line is acceptable here.
    for (int i = 0; i < size; i++) {
        const char c = mb->msg_bufc[(start + i) % size];
        if (c == '\0') continue;
        fDmesgLine[lineLen++] = c;
        if (c == '\n' || lineLen == kDmesgLine) emit();
    }
    if (lineLen) emit();

    Text t{fDmesgOut, sizeof(fDmesgOut)};
    t.str("beat="); t.dec(beat); t.str(" up="); t.dec(now);
    t.str(" msgbuf="); t.dec(static_cast<unsigned>(size));
    t.str(" dropped_lines="); t.dec(dropped); t.ch('\n');
    const size_t oldest = ringLen < kDmesgBytes ? 0 : ringPos;
    for (size_t j = 0; j < ringLen; j++) t.ch(fDmesgRing[(oldest + j) % kDmesgBytes]);
    *t.p = '\0';
    writeChunks("nvfb-dmesg", fDmesgOut, static_cast<size_t>(t.p - fDmesgOut), kDmesgVars);
}

void NVFramebuffer::eventCallout(thread_call_param_t self, thread_call_param_t) {
    NVFramebuffer *fb = static_cast<NVFramebuffer *>(self);
    IOLockLock(fb->fTraceLock);
    fb->fEventPending = false;
    const bool go = !fb->fStopping;
    if (go) fb->fNewsFlushes++;
    IOLockUnlock(fb->fTraceLock);
    if (go) fb->flushTrace();
}

void NVFramebuffer::beatCallout(thread_call_param_t self, thread_call_param_t) {
    NVFramebuffer *fb = static_cast<NVFramebuffer *>(self);
    IOLockLock(fb->fTraceLock);
    const bool go = !fb->fStopping;
    const bool dirty = fb->fDirty;
    const unsigned beat = ++fb->fBeats;
    IOLockUnlock(fb->fTraceLock);
    if (!go) return;

    if (fb->fDiag) {
        fb->writeBeat(beat);
        if (dirty) fb->flushTrace();
    }

    clock_sec_t now = 0;
    clock_usec_t usecs = 0;
    clock_get_system_microtime(&now, &usecs);
    if (!fb->fDiag) {
        // Quiet mode: the console and nothing else. No NVRAM write here (the writing thread stopped coming
        // back after a few of them), no process scan, no mount walk, no status line.
        IOLog("NVFramebuffer: alive t=%us beat=%u\n", static_cast<unsigned>(now), beat);
    } else {
        fb->logStatus(now);
        if (beat == 1 || isPsBeat(beat)) fb->writeMounts(now);
        if (isPsBeat(beat)) fb->writeProcesses(now);
        if (fb->fProbe && isProbeBeat(beat)) {
            fb->writePci(now);
            fb->writeStorage(now);
        }
        if (isDmesgBeat(beat)) fb->writeDmesg(beat, now);
    }

    // Re-arm under the lock: stop() sets fStopping under the same lock before cancelling, so either
    // we see it and stop here, or our re-arm happens first and stop()'s cancel removes it.
    IOLockLock(fb->fTraceLock);
    if (!fb->fStopping && beat < (fb->fDiag ? kMaxBeats : 600u)) {
        uint64_t deadline = 0;
        clock_interval_to_deadline(fb->fDiag ? kBeatMs : kQuietBeatMs, kMillisecondScale, &deadline);
        thread_call_enter_delayed(fb->fBeatCall, deadline);
    }
    IOLockUnlock(fb->fTraceLock);
}

bool NVFramebuffer::failStart(int status) {
    if (fTraceLock) {
        IOLockLock(fTraceLock);
        fStartStatus = status;
        IOLockUnlock(fTraceLock);
        flushTrace();
    }
    return false;
}

// ---- IOService lifecycle --------------------------------------------------------------------------

bool NVFramebuffer::start(IOService *provider) {
    // Trace plumbing first, so that even an early failure leaves a record behind.
    fFlushLock = IOLockAlloc();
    fTraceLock = IOLockAlloc();
    if (fTraceLock) {
        fEventCall = thread_call_allocate(&NVFramebuffer::eventCallout, this);
        fBeatCall  = thread_call_allocate(&NVFramebuffer::beatCallout, this);
    }

    fPCI = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCI) return failStart(kStartNotOurs);
    // Defense in depth: only ever drive the GPU this driver was written for.
    if (fPCI->configRead16(kIOPCIConfigVendorID) != 0x10DE ||
        fPCI->configRead16(kIOPCIConfigDeviceID) != 0x2704) return failStart(kStartNotOurs);

    // Geometry from the console the kernel is already drawing on.
    PE_Video info;
    bzero(&info, sizeof(info));
    IOPlatformExpert *platform = getPlatform();
    if (!platform) return failStart(kStartBadConsole);
    platform->getConsoleInfo(&info);
    fWidth    = static_cast<UInt32>(info.v_width);
    fHeight   = static_cast<UInt32>(info.v_height);
    fRowBytes = static_cast<UInt32>(info.v_rowBytes);
    if (!fWidth || !fHeight || !fRowBytes || info.v_depth != 32) {
        IOLog("NVFramebuffer: unusable console %ux%u depth=%lu rowBytes=%u\n",
              fWidth, fHeight, static_cast<unsigned long>(info.v_depth), fRowBytes);
        return failStart(kStartBadConsole);
    }

    // The scan-out buffer sits inside BAR1; locate it rather than assuming offset 0.
    IODeviceMemory *bar1 = fPCI->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress1);
    if (!bar1) {
        IOLog("NVFramebuffer: no BAR1\n");
        return failStart(kStartNoBAR1);
    }
    const IOPhysicalAddress bar1Start = bar1->getPhysicalAddress();
    // v_baseAddr carries a flag in bit 0; clear it before comparing.
    const IOPhysicalAddress consoleBase =
        static_cast<IOPhysicalAddress>(info.v_baseAddr & ~static_cast<IOPhysicalAddress>(1));
    const IOByteCount visible = static_cast<IOByteCount>(fHeight) * fRowBytes;
    if (consoleBase < bar1Start || consoleBase + visible > bar1Start + bar1->getLength()) {
        IOLog("NVFramebuffer: console 0x%llx not inside BAR1 0x%llx+0x%llx\n",
              static_cast<unsigned long long>(consoleBase),
              static_cast<unsigned long long>(bar1Start),
              static_cast<unsigned long long>(bar1->getLength()));
        return failStart(kStartOutsideBAR1);
    }
    fAperture = IODeviceMemory::withSubRange(bar1, consoleBase - bar1Start, visible);
    if (!fAperture) return failStart(kStartNoAperture);

    IOLog("NVFramebuffer: %ux%u rowBytes=%u fb=0x%llx (BAR1+0x%llx) bytes=0x%llx\n",
          fWidth, fHeight, fRowBytes, static_cast<unsigned long long>(consoleBase),
          static_cast<unsigned long long>(consoleBase - bar1Start),
          static_cast<unsigned long long>(visible));

    if (!IOFramebuffer::start(provider)) {
        IOLog("NVFramebuffer: IOFramebuffer::start failed\n");
        return failStart(kStartSuperFailed);
    }

    if (fTraceLock) {
        IOLockLock(fTraceLock);
        fStartStatus = kStartOK;
        IOLockUnlock(fTraceLock);
    }
    UInt32 probeArg = 0, nvramArg = 0, diagArg = 0;
    fProbe = PE_parse_boot_argn("nvfb-probe", &probeArg, sizeof(probeArg)) && probeArg;
    fNvram = PE_parse_boot_argn("nvfb-nvram", &nvramArg, sizeof(nvramArg)) && nvramArg;
    fDiag  = PE_parse_boot_argn("nvfb-diag", &diagArg, sizeof(diagArg)) && diagArg;
    unsigned removed = 0;
    if (!fNvram) {
        for (const char *var : kAllVars) removed += PERemoveNVRAMProperty(var) ? 1 : 0;
    } else if (!fProbe) {
        for (const char *var : kStaleVars) removed += PERemoveNVRAMProperty(var) ? 1 : 0;
    }

    // Probe NVRAM once, synchronously, while the console is still text so the result can be read off
    // the screen: read=0 means /options is not up yet; read=1 write=0 means writes are being refused.
    char probe[64];
    unsigned probeLen = sizeof(probe);
    const bool readable = PEReadNVRAMProperty("boot-args", probe, &probeLen);
    const bool written = flushTrace();
    IOLog("NVFramebuffer: 0.6.0 nvram read=%d write=%d removed=%u nvram=%d probe=%d (status every %us)\n",
          readable, written, removed, fNvram, fProbe, static_cast<unsigned>(kBeatMs / 1000));

    if (!fDiag) writeBeat(0);       // Proves the write path at start, before anything else can stop
    if (fBeatCall) {
        uint64_t deadline = 0;
        clock_interval_to_deadline(fDiag ? kBeatMs : kQuietBeatMs, kMillisecondScale, &deadline);
        thread_call_enter_delayed(fBeatCall, deadline);
    }

    if (!fDiag) {
        // The differential build: nothing below this point runs. No mapping of the scan-out buffer, no
        // console resize, no tick, no probe, no registry walk, no notifiers, no kernel thread, no writes to
        // VRAM - only IOFramebuffer itself and the NVRAM beat above.
        IOLog("NVFramebuffer: 0.24.0 quiet (no HUD/probe/walk/net; beat every %us, nvram=%d). "
              "Boot with nvfb-diag=1 for the diagnostic build.\n",
              static_cast<unsigned>(kQuietBeatMs / 1000), fNvram);
        return true;
    }

    // HUD: our own mapping of the scan-out buffer, independent of the console's.
    fHudMap = fAperture->map(kIOMapWriteCombineCache);
    if (fHudMap && fTraceLock) {
        fHudCall = thread_call_allocate(&NVFramebuffer::hudCallout, this);
        fProbeCall = thread_call_allocate(&NVFramebuffer::probeCallout, this);
    }
    if (fHudCall && fProbeCall) {
        uint64_t deadline = 0;
        clock_interval_to_deadline(kHudTickMs, kMillisecondScale, &deadline);
        thread_call_enter_delayed(fHudCall, deadline);
        clock_interval_to_deadline(kHudTickMs * 2, kMillisecondScale, &deadline);
        thread_call_enter_delayed(fProbeCall, deadline);
    }
    // UDP log and GPU path check: baseline registers now, first tick after a second.
    fGpuCmd0 = fGpuCmd = fPCI->configRead16(kIOPCIConfigCommand);
    fGpuBar1Lo0 = fGpuBar1Lo = fPCI->configRead32(kIOPCIConfigBaseAddress1);
    fGpuBar1Hi0 = fGpuBar1Hi = fPCI->configRead32(kIOPCIConfigBaseAddress2);
    // The UDP log gives the NIC a fixed lab address, so it only runs when asked for with nvfb-net=1.
    UInt32 netArg = 0;
    const bool netOn = PE_parse_boot_argn("nvfb-net", &netArg, sizeof(netArg)) && netArg;
    thread_t netThread = nullptr;
    if (netOn && kernel_thread_start(&NVFramebuffer::netThreadMain, this, &netThread) == KERN_SUCCESS) {
        fNetThread = netThread;
        thread_deallocate(netThread);      // the running thread holds its own reference
    }
    IOLog("NVFramebuffer: 0.24.0 net thread %s, udp log to %u.%u.%u.%u:%u, gpu cmd=0x%04x bar1=0x%08x%08x\n",
          fNetThread ? "on" : "off", kNetDstAddr >> 24, (kNetDstAddr >> 16) & 0xFF, (kNetDstAddr >> 8) & 0xFF,
          kNetDstAddr & 0xFF, kNetPort, fGpuCmd0, fGpuBar1Hi0, fGpuBar1Lo0);

    // Keep the kernel console out of the bottom band, so the HUD is never scrolled over. This is the
    // sequence IOFramebuffer::setPlatformConsole() uses for a mode change: pause, then re-enable with the
    // new geometry. v_baseAddr bit 0 marks a physical address, which makes the console map it itself;
    // every other field (scale, depth, rowBytes) stays as the booter left it.
    if (fHudMap && fHeight > kHudBand * 2) {
        PE_Video shrunk = info;
        shrunk.v_baseAddr = static_cast<unsigned long>(consoleBase) | 1;
        shrunk.v_height   = fHeight - kHudBand;
        platform->setConsoleInfo(nullptr, kPEDisableScreen);
        platform->setConsoleInfo(&shrunk, kPEEnableScreen);
        fConsoleShrunk = true;
        IOLog("NVFramebuffer: console limited to %lu rows; HUD band is the bottom %u\n",
              static_cast<unsigned long>(shrunk.v_height), kHudBand);
    }

    // Every service in the system: the HUD ring then shows what appeared last before any freeze.
    if (fHudMap) {
        if (OSDictionary *all = IOService::serviceMatching("IOService")) {
            fPubNotifier = IOService::addMatchingNotification(gIOPublishNotification, all,
                                                              &NVFramebuffer::publishHandler, this);
            fMatchNotifier = IOService::addMatchingNotification(gIOMatchedNotification, all,
                                                                &NVFramebuffer::matchedHandler, this);
            all->release();
        }
    }
    IOLog("NVFramebuffer: HUD %s, service ring %s\n", (fHudCall && fProbeCall) ? "on" : "off",
          (fPubNotifier && fMatchNotifier) ? "on" : "off");
    return true;
}

void NVFramebuffer::stop(IOService *provider) {
    if (fTraceLock) {
        IOLockLock(fTraceLock);
        fEvents[kEvStop]++;
        fStopping = true;
        IOLockUnlock(fTraceLock);
        if (fBeatCall)  thread_call_cancel_wait(fBeatCall);
        if (fEventCall) thread_call_cancel_wait(fEventCall);
        if (fHudCall)   thread_call_cancel_wait(fHudCall);
        if (fProbeCall) thread_call_cancel_wait(fProbeCall);
        for (int i = 0; fNetThread && !fNetThreadDone && i < 300; i++) IOSleep(10);
        fNetThread = nullptr;
        if (fNetSock)   { sock_close(fNetSock); fNetSock = nullptr; }
        if (fPubNotifier)   { fPubNotifier->remove();   fPubNotifier = nullptr; }
        if (fMatchNotifier) { fMatchNotifier->remove(); fMatchNotifier = nullptr; }
        flushTrace();   // last word
    }
    OSSafeReleaseNULL(fAperture);
    IOFramebuffer::stop(provider);
}

// Also reached when start() fails, which never calls stop(), so the trace plumbing is released here.
void NVFramebuffer::free(void) {
    if (fBeatCall)  { thread_call_cancel_wait(fBeatCall);  thread_call_free(fBeatCall);  fBeatCall = nullptr; }
    if (fEventCall) { thread_call_cancel_wait(fEventCall); thread_call_free(fEventCall); fEventCall = nullptr; }
    if (fHudCall)   { thread_call_cancel_wait(fHudCall);   thread_call_free(fHudCall);   fHudCall = nullptr; }
    if (fProbeCall) { thread_call_cancel_wait(fProbeCall); thread_call_free(fProbeCall); fProbeCall = nullptr; }
    fStopping = true;
    for (int i = 0; fNetThread && !fNetThreadDone && i < 300; i++) IOSleep(10);
    fNetThread = nullptr;
    if (fNetSock)   { sock_close(fNetSock); fNetSock = nullptr; }
    if (fPubNotifier)   { fPubNotifier->remove();   fPubNotifier = nullptr; }
    if (fMatchNotifier) { fMatchNotifier->remove(); fMatchNotifier = nullptr; }
    OSSafeReleaseNULL(fHudMap);
    if (fTraceLock) { IOLockFree(fTraceLock); fTraceLock = nullptr; }
    if (fFlushLock) { IOLockFree(fFlushLock); fFlushLock = nullptr; }
    OSSafeReleaseNULL(fAperture);
    IOFramebuffer::free();
}

// ---- Traced pass-throughs -------------------------------------------------------------------------

IOReturn NVFramebuffer::newUserClient(task_t owningTask, void *securityID, UInt32 type,
                                      IOUserClient **handler) {
    // Printed first: for the server connection IOFramebuffer releases the console before returning.
    hudMark(kEvUcIn, true);
    IOLog("NVFramebuffer: newUserClient type=%u\n", static_cast<unsigned>(type));
    const IOReturn result = IOFramebuffer::newUserClient(owningTask, securityID, type, handler);
    hudMark(kEvUcIn, false);
    IOLog("NVFramebuffer: newUserClient type=%u -> 0x%x\n", static_cast<unsigned>(type),
          static_cast<unsigned>(result));
    if (fTraceLock) {
        IOLockLock(fTraceLock);
        fUserClientTypes |= type < 31 ? (1u << type) : 0x80000000u;
        fUserClientResult = result;
        IOLockUnlock(fTraceLock);
    }
    traceEvent(kEvUserClient);
    return result;
}

IOReturn NVFramebuffer::open(void) {
    hudMark(kEvOpIn, true);
    IOLog("NVFramebuffer: open()\n");
    const IOReturn result = IOFramebuffer::open();
    hudMark(kEvOpIn, false);
    IOLog("NVFramebuffer: open() -> 0x%x\n", static_cast<unsigned>(result));
    if (fTraceLock) {
        IOLockLock(fTraceLock);
        fOpenResult = result;
        IOLockUnlock(fTraceLock);
    }
    traceEvent(kEvOpen);
    return result;
}

void NVFramebuffer::close(void) {
    hudMark(kEvClose, true);
    traceEvent(kEvClose);
    IOFramebuffer::close();
}

IOReturn NVFramebuffer::setAttribute(IOSelect attribute, uintptr_t value) {
    hudSelector('a', attribute);
    hudMark(kEvSetAttr, true);
    const IOReturn result = IOFramebuffer::setAttribute(attribute, value);
    hudMark(kEvSetAttr, false);
    traceSelector('a', attribute, result);
    return result;
}

// ---- IOFramebuffer ---------------------------------------------------------------------------------

IOReturn NVFramebuffer::enableController(void) {
    hudMark(kEvEnable, true);
    traceEvent(kEvEnable);
    return kIOReturnSuccess;
}

// Tell the system this is the boot console, so it keeps using this surface.
bool NVFramebuffer::isConsoleDevice(void) {
    hudMark(kEvIsConsole, true);
    traceEvent(kEvIsConsole);
    return true;
}

IODeviceMemory *NVFramebuffer::getApertureRange(IOPixelAperture aperture) {
    hudMark(kEvAperture, true);
    traceEvent(kEvAperture);
    if (aperture != kIOFBSystemAperture || !fAperture) return nullptr;
    fAperture->retain();   // caller owns the reference
    return fAperture;
}

const char *NVFramebuffer::getPixelFormats(void) {
    hudMark(kEvPixFormats, true);
    traceEvent(kEvPixFormats);
    return IO32BitDirectPixels "\0\0";
}

IOItemCount NVFramebuffer::getDisplayModeCount(void) {
    hudMark(kEvModeCount, true);
    traceEvent(kEvModeCount);
    return 1;
}

IOReturn NVFramebuffer::getDisplayModes(IODisplayModeID *allDisplayModes) {
    hudMark(kEvModes, true);
    traceEvent(kEvModes);
    if (!allDisplayModes) return kIOReturnBadArgument;
    allDisplayModes[0] = kNVDisplayMode;
    return kIOReturnSuccess;
}

IOReturn NVFramebuffer::getInformationForDisplayMode(IODisplayModeID displayMode,
                                                     IODisplayModeInformation *info) {
    hudMark(kEvModeInfo, true);
    traceEvent(kEvModeInfo);
    if (displayMode != kNVDisplayMode || !info) return kIOReturnBadArgument;
    bzero(info, sizeof(*info));
    info->nominalWidth  = fWidth;
    info->nominalHeight = fHeight;
    info->refreshRate   = 60 << 16;   // 16.16 fixed point
    info->maxDepthIndex = 0;
    info->flags = kDisplayModeValidFlag | kDisplayModeSafeFlag | kDisplayModeDefaultFlag;
    return kIOReturnSuccess;
}

// Documented as obsolete: must return zero.
UInt64 NVFramebuffer::getPixelFormatsForDisplayMode(IODisplayModeID, IOIndex) { return 0; }

IOReturn NVFramebuffer::getPixelInformation(IODisplayModeID displayMode, IOIndex depth,
                                            IOPixelAperture aperture,
                                            IOPixelInformation *pixelInfo) {
    hudMark(kEvPixInfo, true);
    traceEvent(kEvPixInfo);
    if (displayMode != kNVDisplayMode || depth != 0 ||
        aperture != kIOFBSystemAperture || !pixelInfo) return kIOReturnBadArgument;
    bzero(pixelInfo, sizeof(*pixelInfo));
    pixelInfo->bytesPerRow        = fRowBytes;
    pixelInfo->bytesPerPlane      = 0;
    pixelInfo->bitsPerPixel       = 32;
    pixelInfo->pixelType          = kIORGBDirectPixels;
    pixelInfo->componentCount     = 3;
    pixelInfo->bitsPerComponent   = 8;
    pixelInfo->componentMasks[0]  = 0x00FF0000;   // R
    pixelInfo->componentMasks[1]  = 0x0000FF00;   // G
    pixelInfo->componentMasks[2]  = 0x000000FF;   // B
    pixelInfo->activeWidth        = fWidth;
    pixelInfo->activeHeight       = fHeight;
    // Copy by hand rather than strlcpy: fortified string calls compile to *_chk variants, and this
    // kernel does not export all of them (___snprintf_chk is missing). A bounded loop needs none.
    {
        const char *src = IO32BitDirectPixels;
        size_t i = 0;
        const size_t limit = sizeof(pixelInfo->pixelFormat) - 1;
        while (src[i] && i < limit) { pixelInfo->pixelFormat[i] = src[i]; i++; }
        pixelInfo->pixelFormat[i] = '\0';
    }
    return kIOReturnSuccess;
}

IOReturn NVFramebuffer::getCurrentDisplayMode(IODisplayModeID *displayMode, IOIndex *depth) {
    hudMark(kEvCurMode, true);
    traceEvent(kEvCurMode);
    if (displayMode) *displayMode = kNVDisplayMode;
    if (depth) *depth = 0;
    return kIOReturnSuccess;
}

// Only the firmware-programmed mode exists; accept it and reject anything else.
IOReturn NVFramebuffer::setDisplayMode(IODisplayModeID displayMode, IOIndex depth) {
    hudMark(kEvSetMode, true);
    traceEvent(kEvSetMode);
    return (displayMode == kNVDisplayMode && depth == 0) ? kIOReturnSuccess : kIOReturnUnsupported;
}

IOItemCount NVFramebuffer::getConnectionCount(void) {
    hudMark(kEvConnCount, true);
    traceEvent(kEvConnCount);
    return 1;
}

// The attribute entry points are split into a pure handler and a traced wrapper, so every selector the
// system asks about is recorded together with the answer it got.
static IOReturn hardwareCursorAttribute(uintptr_t *value) {
    // No hardware cursor: make the system composite it in software.
    if (value) *value = 0;
    return kIOReturnSuccess;
}

IOReturn NVFramebuffer::getAttribute(IOSelect attribute, uintptr_t *value) {
    hudSelector('A', attribute);
    hudMark(kEvGetAttr, true);
    const IOReturn result = attribute == kIOHardwareCursorAttribute
        ? hardwareCursorAttribute(value)
        : IOFramebuffer::getAttribute(attribute, value);
    hudMark(kEvGetAttr, false);
    traceSelector('A', attribute, result);
    return result;
}

IOReturn NVFramebuffer::getAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
                                                  uintptr_t *value) {
    hudSelector('C', attribute);
    hudMark(kEvGetConn, true);
    IOReturn result;
    switch (attribute) {
        case kConnectionEnable:
            if (value) *value = 1;
            result = kIOReturnSuccess;
            break;
        case kConnectionFlags:
            if (value) *value = 0;
            result = kIOReturnSuccess;
            break;
        case kConnectionSupportsAppleSense:
        case kConnectionSupportsLLDDCSense:
        case kConnectionSupportsHLDDCSense:
            result = kIOReturnUnsupported;
            break;
        default:
            result = IOFramebuffer::getAttributeForConnection(connectIndex, attribute, value);
            break;
    }
    hudMark(kEvGetConn, false);
    traceSelector('C', attribute, result);
    return result;
}

IOReturn NVFramebuffer::setAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
                                                  uintptr_t value) {
    hudSelector('c', attribute);
    hudMark(kEvSetConn, true);
    const IOReturn result = attribute == kConnectionPower
        ? kIOReturnSuccess
        : IOFramebuffer::setAttributeForConnection(connectIndex, attribute, value);
    hudMark(kEvSetConn, false);
    traceSelector('c', attribute, result);
    return result;
}

// ---- traced pass-throughs for the HUD ----
// Each one marks entry, lets IOFramebuffer do exactly what it always did, and marks the return.

IOReturn NVFramebuffer::setupForCurrentConfig(void) {
    hudMark(kEvSetup, true);
    const IOReturn r = IOFramebuffer::setupForCurrentConfig();
    hudMark(kEvSetup, false);
    return r;
}

IODeviceMemory *NVFramebuffer::getVRAMRange(void) {
    hudMark(kEvVram, true);
    IODeviceMemory *r = IOFramebuffer::getVRAMRange();
    hudMark(kEvVram, false);
    return r;
}

IOReturn NVFramebuffer::setCLUTWithEntries(IOColorEntry *colors, UInt32 index, UInt32 numEntries,
                                           IOOptionBits options) {
    hudMark(kEvClut, true);
    const IOReturn r = IOFramebuffer::setCLUTWithEntries(colors, index, numEntries, options);
    hudMark(kEvClut, false);
    return r;
}

IOReturn NVFramebuffer::setGammaTable(UInt32 channelCount, UInt32 dataCount, UInt32 dataWidth,
                                      void *data) {
    hudMark(kEvGamma, true);
    const IOReturn r = IOFramebuffer::setGammaTable(channelCount, dataCount, dataWidth, data);
    hudMark(kEvGamma, false);
    return r;
}

IOReturn NVFramebuffer::setGammaTable(UInt32 channelCount, UInt32 dataCount, UInt32 dataWidth,
                                      void *data, bool syncToVBL) {
    hudMark(kEvGamma, true);
    const IOReturn r = IOFramebuffer::setGammaTable(channelCount, dataCount, dataWidth, data, syncToVBL);
    hudMark(kEvGamma, false);
    return r;
}

IOReturn NVFramebuffer::getTimingInfoForDisplayMode(IODisplayModeID displayMode,
                                                    IOTimingInformation *info) {
    hudMark(kEvTiming, true);
    const IOReturn r = IOFramebuffer::getTimingInfoForDisplayMode(displayMode, info);
    hudMark(kEvTiming, false);
    return r;
}

bool NVFramebuffer::hasDDCConnect(IOIndex connectIndex) {
    hudMark(kEvDdc, true);
    const bool r = IOFramebuffer::hasDDCConnect(connectIndex);
    hudMark(kEvDdc, false);
    return r;
}

IOReturn NVFramebuffer::getDDCBlock(IOIndex connectIndex, UInt32 blockNumber, IOSelect blockType,
                                    IOOptionBits options, UInt8 *data, IOByteCount *length) {
    hudMark(kEvDdcBlock, true);
    const IOReturn r = IOFramebuffer::getDDCBlock(connectIndex, blockNumber, blockType, options, data, length);
    hudMark(kEvDdcBlock, false);
    return r;
}

IOReturn NVFramebuffer::getStartupDisplayMode(IODisplayModeID *displayMode, IOIndex *depth) {
    hudMark(kEvStartup, true);
    const IOReturn r = IOFramebuffer::getStartupDisplayMode(displayMode, depth);
    hudMark(kEvStartup, false);
    return r;
}

IOReturn NVFramebuffer::registerForInterruptType(IOSelect interruptType, IOFBInterruptProc proc,
                                                 OSObject *target, void *ref, void **interruptRef) {
    hudSelector('I', interruptType);
    hudMark(kEvRegIntr, true);
    const IOReturn r = IOFramebuffer::registerForInterruptType(interruptType, proc, target, ref, interruptRef);
    hudMark(kEvRegIntr, false);
    return r;
}

IOReturn NVFramebuffer::setInterruptState(void *interruptRef, UInt32 state) {
    hudMark(kEvIntrState, true);
    const IOReturn r = IOFramebuffer::setInterruptState(interruptRef, state);
    hudMark(kEvIntrState, false);
    return r;
}

IOReturn NVFramebuffer::setPowerState(unsigned long powerStateOrdinal, IOService *device) {
    hudMark(kEvPower, true);
    const IOReturn r = IOFramebuffer::setPowerState(powerStateOrdinal, device);
    hudMark(kEvPower, false);
    return r;
}

IOReturn NVFramebuffer::getNotificationSemaphore(IOSelect interruptType, semaphore_t *semaphore) {
    hudSelector('N', interruptType);
    hudMark(kEvSema, true);
    const IOReturn r = IOFramebuffer::getNotificationSemaphore(interruptType, semaphore);
    hudMark(kEvSema, false);
    return r;
}

// ---- property reads ----
// Only reads made on behalf of a user process get recorded (see propSeen); behaviour doesn't change.

OSObject *NVFramebuffer::copyProperty(const char *aKey) const {
    const_cast<NVFramebuffer *>(this)->propSeen(aKey);
    return IOFramebuffer::copyProperty(aKey);
}

OSObject *NVFramebuffer::copyProperty(const OSString *aKey) const {
    const_cast<NVFramebuffer *>(this)->propSeen(aKey ? aKey->getCStringNoCopy() : nullptr);
    return IOFramebuffer::copyProperty(aKey);
}

OSObject *NVFramebuffer::copyProperty(const OSSymbol *aKey) const {
    const_cast<NVFramebuffer *>(this)->propSeen(aKey ? aKey->getCStringNoCopy() : nullptr);
    return IOFramebuffer::copyProperty(aKey);
}

OSObject *NVFramebuffer::copyProperty(const char *aKey, const IORegistryPlane *plane,
                                      IOOptionBits options) const {
    const_cast<NVFramebuffer *>(this)->propSeen(aKey);
    return IOFramebuffer::copyProperty(aKey, plane, options);
}

OSObject *NVFramebuffer::copyProperty(const OSString *aKey, const IORegistryPlane *plane,
                                      IOOptionBits options) const {
    const_cast<NVFramebuffer *>(this)->propSeen(aKey ? aKey->getCStringNoCopy() : nullptr);
    return IOFramebuffer::copyProperty(aKey, plane, options);
}

OSObject *NVFramebuffer::copyProperty(const OSSymbol *aKey, const IORegistryPlane *plane,
                                      IOOptionBits options) const {
    const_cast<NVFramebuffer *>(this)->propSeen(aKey ? aKey->getCStringNoCopy() : nullptr);
    return IOFramebuffer::copyProperty(aKey, plane, options);
}

bool NVFramebuffer::serializeProperties(OSSerialize *serialize) const {
    const_cast<NVFramebuffer *>(this)->propSeen("<all properties>");
    return IOFramebuffer::serializeProperties(serialize);
}
