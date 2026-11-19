// See NVMTLGsp.h.
#import "NVMTLLog.h"
#import "NVMTLGsp.h"
#import "NVMTLTexHw.h"
#import <IOKit/IOKitLib.h>
#import <objc/message.h>
#include <string.h>
#include <sys/stat.h>
#include <os/lock.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/mach_time.h>
#include <time.h>
#include <unistd.h>
#import <Metal/Metal.h>
#include <pthread.h>
#include <stdatomic.h>

#define NVGSP_ARENA_BASE 0x2800000000ULL
#define NVGSP_STAGE_BYTES (22 << 20)     // 0.6.13: + 4 MiB GR batch at 2 MiB; 0.8.0: + 8 x 2 MiB native slabs at 6 MiB

// NVGspControl user-client selectors (NVGspControlUserClient.cpp).
enum { kSelExecSeg = 21, kSelMemAlloc = 22, kSelMemFree = 23, kSelVaBindObj = 24, kSelCeWait = 29, kSelMemAdopt = 38 };

// NV906F DMA_INCR method header: (1<<29)|(count<<16)|(subch<<13)|(mthd>>2).
// map type of a memory object: 0x1000 | handle up to 0xfff (every kext),
// 0x40000000 | handle above (NVGspControl >= 0.162.0 has 16383 handles)
// how long a fence wait may take before the launch counts as failed (us);
// NVMTL_FENCE_US for long kernels while debugging
static uint64_t nvFenceUs(void) {
    static uint64_t us;
    if (!us) { const char *e = getenv("NVMTL_FENCE_US"); us = e && atoll(e) > 0 ? (uint64_t)atoll(e) : 2000000; }
    return us;
}

static inline uint32_t nvMapType(uint32_t handle) {
    return handle < 0x1000 ? 0x1000u | handle : 0x40000000u | handle;
}

static inline uint32_t mthd(unsigned sub, uint32_t m, unsigned count) {
    return (1u << 29) | ((count & 0x1fff) << 16) | ((sub & 7) << 13) | ((m >> 2) & 0xfff);
}

// NV90B5 copy methods + LAUNCH_DMA bits (Mesa cl90b5.h; sequence = NVK's).
#define B5_OFF_IN_UP 0x400u
#define B5_LINE_LEN 0x418u
#define B5_LAUNCH 0x300u
#define B5_LAUNCH_PITCH_COPY (0x001u | 0x200u | 0x004u | 0x080u | 0x100u) // PIPELINED|MULTI|FLUSH|PITCH|PITCH
#define CE_CHUNK (1 << 17) // 128 KiB, as NVK

static _Atomic(io_connect_t) gConn = IO_OBJECT_NULL;
static uint8_t *gCpu = NULL;
static uint64_t gVa = 0;
static uint32_t gHandle = 0;

// 0.6.13: one submitter at a time. The staging object (QMD, cbuf, method
// streams, DATA) is one per process, and WindowServer commits from several
// threads; two commits at once overwrote each other's launches. Recursive:
// nvExecuteOps holds it for a whole command buffer and the helpers take it
// again.
static pthread_mutex_t gGpuMtx;
static pthread_once_t gGpuOnce = PTHREAD_ONCE_INIT;
static void gpuMtxInit(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&gGpuMtx, &a);
}
static void nv615WaitPending(void);
void nvGpuLock(void) {
    pthread_once(&gGpuOnce, gpuMtxInit);
    pthread_mutex_lock(&gGpuMtx);
    nv615WaitPending();   // 0.9.51: nothing touches GPU memory under queued async work
}
void nvGpuUnlock(void) { pthread_mutex_unlock(&gGpuMtx); }
static inline void gpuUnlockAtExit(int *p) { (void)p; nvGpuUnlock(); }
#define NV_GPU_LOCKED nvGpuLock(); int nvLk_ __attribute__((cleanup(gpuUnlockAtExit), unused)) = 0
// 0.9.64: the lock without waiting for queued async work. For paths that do
// not touch memory the queued work may use (mapping new pages at a free VA,
// bump allocation from a slab's untouched end). CoreAnimation's IOSurface
// textures and buffers waited for all in-flight GPU work on every creation
// (nvGr615WaitSeq: 405 of 3705 main-thread samples).
static void nvGpuLockNoWait(void) { pthread_once(&gGpuOnce, gpuMtxInit); pthread_mutex_lock(&gGpuMtx); }
#define NV_GPU_LOCKED_NOWAIT nvGpuLockNoWait(); int nvLk_ __attribute__((cleanup(gpuUnlockAtExit), unused)) = 0

// 0.8.17: GPU work that failed on this thread (submit refused, fence never
// came). The command buffer whose commit saw it reports an error instead of
// "completed" (a kernel that never ran came back as success after a reset).
static _Thread_local uint32_t tFailures, tFailKind;
void nvNoteGpuFailure(uint32_t kind) { tFailures++; if (kind > tFailKind) tFailKind = kind; }
uint32_t nvGpuFailureCount(void) { return tFailures; }
uint32_t nvGpuFailureKind(void) { return tFailKind; }
void nvGpuFailureReset(void) { tFailKind = 0; }

static bool nvGspRecover(io_connect_t dead);
static _Atomic uint64_t gRecoverySerial, gGeneration;
static _Thread_local uint32_t tEncoding;
void nvGspEncodingBegin(void) { tEncoding++; }
void nvGspEncodingEnd(void) { if (tEncoding) tEncoding--; }
uint64_t nvGspRecoverySerial(void) { return atomic_load(&gRecoverySerial); }
uint64_t nvGspGeneration(void) { return atomic_load(&gGeneration); }
static uint32_t gPostResetLog;                    // 0.8.27, see nvPostResetNote
// 0.8.28: is NVGspControl in a reset (its "reset-busy" property)
static bool nvResetBusy(void) {
    static io_service_t g;
    if (!g) g = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVGspControl"));
    if (!g) return false;
    CFTypeRef r = IORegistryEntryCreateCFProperty(g, CFSTR("NVGspControl-reset-busy"), kCFAllocatorDefault, 0);
    const bool busy = r == kCFBooleanTrue;
    if (r) CFRelease(r);
    return busy;
}
static kern_return_t callOn(io_connect_t c, uint32_t sel, const uint64_t *in, uint32_t inN,
                            uint64_t *out, uint32_t *outN, const void *sIn, size_t sInN) {
    return IOConnectCallMethod(c, sel, in, inN, sIn, sInN, out, outN, NULL, NULL);
}

// 0.9.26: NVMTL_NATIVE615=1 runs this process on an owned native615 GR client
// (NVGspControl >= 0.180.105, selectors 45-48): its own VAS, a 6 MiB staging
// allocation (stage + GR batch area), MTLBuffer memory suballocated from 8 MiB
// allocations, and GR method streams submitted with selector 48, which adds a
// kernel HOST fence and waits. No copy engine, scratch (slm), VRAM objects or
// native slabs on this path: those requests fail instead of using the legacy
// selectors. Closing the process tears the client down in the kernel.
static bool gN615;
// 0.9.36: on by default once the kernel's native615 runtime is ready (the
// legacy selectors are closed then); NVMTL_NATIVE615=0/1 still overrides.
static bool nvN615KernelReady(void) {
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVGspControl"));
    if (!s) return false;
    CFTypeRef p = IORegistryEntryCreateCFProperty(s, CFSTR("NVGspControl-native615-ready"), kCFAllocatorDefault, 0);
    const bool ready = p && CFGetTypeID(p) == CFBooleanGetTypeID() && CFBooleanGetValue((CFBooleanRef)p);
    if (p) CFRelease(p);
    IOObjectRelease(s);
    return ready;
}
static bool nvN615Requested(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("NVMTL_NATIVE615");
        on = e ? atoi(e) != 0 : nvN615KernelReady();
    }
    return on;
}
bool nvGspNative615(void) { return nvN615Requested(); }
#define NV615_STAGE_BYTES (6ull << 20)
// 0.9.29 (kernel 0.180.107: 256 MiB window, 64 MiB allocations, 16 slots):
// 32 MiB slabs, and a buffer above 32 MiB gets its own allocation (a "slab"
// whose size is exactly that buffer).
#define NV615_HEAP_SLAB (32ull << 20)
// 0.9.40 (kernel 0.180.118: 1 GiB window, 512 MiB allocations, 32 slots)
#define NV615_BIG_MAX (512ull << 20)
#define NV615_HEAP_SLABS 60   // 0.9.91 (kernel 0.180.193: 16 GiB window, 64 slots)
// 0.9.91: slab size doubles every 8 kernel allocations (64 slots in all), so
// one process reaches the 16 GiB of VRAM and then system memory before the
// slots run out; a small app still starts with 32/64 MiB slabs.
static unsigned g615KernelAllocs;
static uint64_t nv615SlabBytes(uint64_t len, uint64_t base) {
    const unsigned shift = g615KernelAllocs / 8 < 4 ? g615KernelAllocs / 8 : 4;
    uint64_t s = base << shift;
    if (s > NV615_BIG_MAX) s = NV615_BIG_MAX;
    return len > s ? len : s;
}
typedef struct { uint8_t *cpu; uint64_t va, used, size; uint32_t kind; } NV615Slab;
// 0.9.34: PTE kind the next heap allocation needs (nvVramAllocKind sets it
// under the GPU lock): zeta surfaces get their Z kind, colour GENERIC_MEMORY.
static uint32_t g615ReqKind;
static NV615Slab g615Slab[NV615_HEAP_SLABS];
static unsigned g615NSlab;
// 0.9.27: freed suballocations, reused first fit (a larger span is split)
typedef struct { uint8_t *cpu; uint64_t va, len; uint32_t kind; } NV615Span;
static NV615Span g615Free[1024];
static unsigned g615NFree;
// CPU address of [va, va+bytes) in staging or a heap slab, else NULL. Every
// native615 GPU byte is pinned sysmem that this process also maps.
static uint8_t *nv615WrapCpu(uint64_t va, uint64_t bytes);
static uint8_t *nv615Cpu(uint64_t va, uint64_t bytes) {
    if (va >= gVa && bytes <= NV615_STAGE_BYTES && va - gVa <= NV615_STAGE_BYTES - bytes) return gCpu + (va - gVa);
    for (unsigned i = 0; i < g615NSlab; i++)
        if (va >= g615Slab[i].va && bytes <= g615Slab[i].size && va - g615Slab[i].va <= g615Slab[i].size - bytes)
            return g615Slab[i].cpu + (va - g615Slab[i].va);
    return nv615WrapCpu(va, bytes);   // 0.9.42: wrapped IOSurfaces / NoCopy buffers (CAMetalLayer drawables)
}
// 0.9.30: block-linear surfaces on the CPU (no copy engine on native615).
// GOB = 64 bytes x 8 rows = 512 bytes; a block is 2^ylog GOBs high and
// 2^zlog deep, blocks laid out x, then y, then z. Inside a GOB the byte
// offset interleaves x and y bits (see nv615BlOffset).
// 0.9.39: the zeta kinds lay a GOB out their own way (measured with
// zeta_probe / stencil_probe on the RTX 4080): gob 1 = Z16 (x[3:0], y0, y1,
// y2, x4, x5); gob 2 = S8 (8x8 tiles, y2 ^= x3); gob 4 = ZF32_X24S8, planar:
// GOBs in pairs of 1 KiB (the block's two GOBs, or two blocks side by side
// when a block is one GOB): depth 256 B each, then stencil 64 B in each
// second half; 8-byte texels (float, stencil, 3 unused) on the linear side.
typedef struct { uint32_t rowBytes, rows, depth, ylog, zlog, gob; } NV615Bl;
static uint64_t nv615BlBytes(const NV615Bl *b) {
    const uint64_t bw = (b->rowBytes + 63) / 64, bh = (b->rows + (8u << b->ylog) - 1) / (8u << b->ylog);
    const uint64_t bd = (b->depth + (1u << b->zlog) - 1) / (1u << b->zlog);
    return bw * bh * bd * (512ull << (b->ylog + b->zlog));
}
static inline uint64_t nv615BlOffset(const NV615Bl *b, uint32_t x, uint32_t y, uint32_t z) {
    const uint32_t blockH = 8u << b->ylog, blockD = 1u << b->zlog;
    const uint64_t bw = (b->rowBytes + 63) / 64, bh = (b->rows + blockH - 1) / blockH;
    const uint64_t block = ((uint64_t)(z / blockD) * bh + y / blockH) * bw + x / 64;
    const uint32_t gob = (z % blockD) * (1u << b->ylog) + (y % blockH) / 8;
    const uint32_t gx = x % 64, gy = y % 8;
    // 0.9.32: measured on the RTX 4080 (mtl_bl_probe): x[3:0], y0, y1, x4, y2, x5
    const uint32_t inner = b->gob == 1 ? (gx & 15) | gy << 4 | ((gx >> 4) & 1) << 7 | ((gx >> 5) & 1) << 8
                         : b->gob == 2 ? ((gx & 7) | gy << 3 | (gx >> 3) << 6) ^ ((gx >> 3) & 1) << 5
                         : (gx & 15) | (gy & 1) << 4 | ((gy >> 1) & 1) << 5 | ((gx >> 4) & 1) << 6 |
                           ((gy >> 2) & 1) << 7 | ((gx >> 5) & 1) << 8;
    return block * (512ull << (b->ylog + b->zlog)) + (uint64_t)gob * 512 + inner;
}
// 0.9.39: NVMTL_ZDUMP=<path>: raw bytes of a GPU-only surface (layout probes)
void nvDebugDumpVa(uint64_t va, uint64_t bytes, const char *path) {
    uint8_t *p = nv615Cpu(va, bytes);
    FILE *f = p ? fopen(path, "wb") : NULL;
    if (f) { fwrite(p, 1, bytes, f); fclose(f); }
}
// Copy widthBytes x height between a linear surface (va, pitch) and a
// block-linear one at origin (x, y, z). Caller flushed queued GR work.
static bool nv615BlCopy(bool toBL, uint64_t linVa, uint32_t linPitch, uint64_t blVa, const NV615Bl *b,
                        uint32_t x, uint32_t y, uint32_t z, uint32_t widthBytes, uint32_t height) {
    uint8_t *bl = nv615Cpu(blVa, nv615BlBytes(b));
    uint8_t *lin = nv615Cpu(linVa, (uint64_t)linPitch * (height - 1) + widthBytes);
    if (!bl || !lin) { NSLog(@"NVMTLGsp: native615 block-linear copy outside mapped memory"); return false; }
    if (b->gob == 4) {
        const uint32_t bw = (b->rowBytes + 63) / 64, blockH = 8u << b->ylog;
        for (uint32_t r = 0; r < height; r++)
            for (uint32_t i = 0; i < widthBytes; i++) {
                const uint32_t px = (x + i) / 8, k = (x + i) % 8, py = y + r, gc = px / 8, lx = px % 8, ly = py % 8;
                uint64_t pair;
                uint32_t member;
                if (b->ylog) {
                    const uint32_t g = (py % blockH) / 8;
                    pair = ((uint64_t)(py / blockH) * bw + gc) * (512ull << b->ylog) + (g >> 1) * 1024ull;
                    member = g & 1;
                } else {
                    pair = ((uint64_t)(py / 8) * bw + (gc & ~1u)) * 512ull;
                    member = gc & 1;
                }
                uint8_t *q = lin + (uint64_t)r * linPitch + i;
                if (k > 4) { if (!toBL) *q = 0; continue; }
                uint8_t *p = bl + pair + member * 256 + (k < 4 ? (lx & 3) * 4 + ly * 16 + (lx >> 2) * 128 + k
                                                               : 512 + (lx & 3) + ly * 4 + (lx >> 2) * 32);
                if (toBL) *p = *q; else *q = *p;
            }
        return true;
    }
    for (uint32_t r = 0; r < height; r++)
        for (uint32_t i = 0; i < widthBytes; i++) {
            uint8_t *p = bl + nv615BlOffset(b, x + i, y + r, z), *q = lin + (uint64_t)r * linPitch + i;
            if (toBL) *p = *q; else *q = *p;
        }
    return true;
}
// 0.9.30: GPU-only surfaces (nvVramAllocKind) come from the same heap
// 0.9.46: cpu NULL = a VRAM span (va, size) of PTE kind `kind`
typedef struct { uint32_t id; uint8_t *cpu; uint64_t va, size; uint32_t kind; } NV615Kind;
typedef struct { uint64_t va, size; } NV615VSpan;
static NV615VSpan g615VK[8][256];   // free VRAM spans per PTE kind
static unsigned g615NVK[8];
static bool nv615VramTexOn(void);
static bool nv615VramKindTake(uint32_t kind, uint64_t len, uint64_t *va);
static void nv615VramKindGive(uint32_t kind, uint64_t va, uint64_t len);
static bool nv615VramTrimLocked(void);   // 0.9.91
static NV615Kind g615Kind[2048];
static unsigned g615NKind;
static uint32_t g615KindId = 0x615000;
// 0.9.34: kind = PTE kind for the whole allocation (kernel 0.180.111, 0..7)
// 0.9.42: the CPU mirror of the client's 1 GiB allocation window. Every
// kernel allocation is mapped at mirror + (va - window), so buffers adjacent
// on the GPU are adjacent on the CPU too and freed spans can be merged.
#define NV615_WINDOW_BYTES (16384ull << 20)   // 0.9.47: kernel 0.180.132 window; 0.9.91: 0.180.193 (16 GiB)
static mach_vm_address_t g615Mirror;
static uint64_t g615MirrorVa;
// 0.9.76: native615 maps system memory uncached in the GPU L2 (VOL) unless
// asked otherwise, so every GPU read of a Shared buffer or an IOSurface was a
// PCIe round trip: a 4K pass reading 36 uniform words per pixel took 61 ms
// against 0.16 ms without them, and CoreDisplay's colour pass cost 75 ms a
// frame. Kernel 0.180.177 maps kind | 0x10 L2-cacheable and gives that
// client's submissions the host access barrier (L2 sysmem invalidate before,
// SET_REFERENCE before the fence). Heap slabs and wraps only: the staging
// object holds the pushbuffers, which the host may fetch ahead of the barrier.
static int g615SysCached = -1;
static bool nvSysCachedOn(void) {
    if (g615SysCached < 0) {
        const char *e = getenv("NVMTL_SYSMEM_CACHED");
        struct stat st;
        g615SysCached = e ? atoi(e) != 0
                          : (stat("/Library/Preferences/nvmtl-syscached", &st) == 0 && st.st_uid == 0);
    }
    return g615SysCached == 1;
}
static bool nv615Alloc(io_connect_t c, uint64_t bytes, uint8_t **cpu, uint64_t *va, uint32_t kind) {
    uint64_t in[2] = {0, bytes | ((uint64_t)(kind & 0x17) << 56)}, out[3] = {0, 0, 0};
    uint32_t n = 3;
    kern_return_t kr = callOn(c, 47, in, 2, out, &n, NULL, 0);
    if (kr == kIOReturnBadArgument && (kind & 0x10)) {   // a kernel before 0.180.177: uncached from now on
        // 0.9.91: only BadArgument means that; NoSpace (window full) used to turn caching off for good
        NSLog(@"NVMTLGsp: L2-cacheable system memory refused (0x%x), uncached", kr);
        g615SysCached = 0;
        return nv615Alloc(c, bytes, cpu, va, kind & 7);
    }
    if (kr || out[2] != bytes) { NSLog(@"NVMTLGsp: native615 alloc of %llu bytes -> 0x%x", bytes, kr); return false; }
    ++g615KernelAllocs;
    mach_vm_address_t a = 0;
    mach_vm_size_t s = 0;
    if (!g615Mirror && mach_vm_allocate(mach_task_self(), &g615Mirror, NV615_WINDOW_BYTES, VM_FLAGS_ANYWHERE) == KERN_SUCCESS)
        g615MirrorVa = out[1] & ~0x1fffffull;   // the first allocation (staging) sits just above the window base
    kr = KERN_FAILURE;
    if (g615Mirror && out[1] >= g615MirrorVa && out[1] - g615MirrorVa + bytes <= NV615_WINDOW_BYTES) {
        a = g615Mirror + (out[1] - g615MirrorVa);
        if (mach_vm_deallocate(mach_task_self(), a, bytes) == KERN_SUCCESS)
            kr = IOConnectMapMemory64(c, 0x80615001u + (uint32_t)out[0], mach_task_self(), &a, &s, 0);
        if (kr) { mach_vm_address_t r = a; mach_vm_allocate(mach_task_self(), &r, bytes, VM_FLAGS_FIXED); }
    }
    if (kr) { a = 0; kr = IOConnectMapMemory64(c, 0x80615001u + (uint32_t)out[0], mach_task_self(), &a, &s, kIOMapAnywhere); }
    if (kr || s != bytes) { NSLog(@"NVMTLGsp: native615 map of slot %llu -> 0x%x", out[0], kr); return false; }
    *cpu = (uint8_t *)(uintptr_t)a;
    *va = out[1];
    return true;
}
// 0.8.73: the reset generation of the connection in gConn. A NotReady is only
// treated as "the client predates a reset" when the kext's counter moved;
// otherwise every transient NotReady reopened the client (mediaanalysisd
// reopened 13 times in one second and the GPU stayed wedged).
static uint32_t gConnReset;
static uint32_t nvResetCountNow(void) {
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVGspControl"));
    if (!s) return 0;
    uint32_t v = 0;
    CFTypeRef p = IORegistryEntryCreateCFProperty(s, CFSTR("NVGspControl-gpu-reset-count"),
                                                  kCFAllocatorDefault, 0);
    if (p && CFGetTypeID(p) == CFNumberGetTypeID()) {
        long long n = 0;
        CFNumberGetValue((CFNumberRef)p, kCFNumberSInt64Type, &n);
        v = (uint32_t)n;
    }
    if (p) CFRelease(p);
    IOObjectRelease(s);
    return v;
}
bool nvGspGenerationMatches(uint64_t generation) {
    uint64_t current = 0;
    uint32_t count = 1;
    // A terminal-status callback must never wait on the encoding mutex or
    // reopen: encoding can itself be waiting for that callback's slab.
    return callOn(atomic_load(&gConn), 37, NULL, 0, &current, &count, NULL, 0) == KERN_SUCCESS &&
           count == 1 && current == generation;
}
static bool callMethod(uint32_t sel, const uint64_t *in, uint32_t inN,
                       uint64_t *out, uint32_t *outN, const void *sIn, size_t sInN) {
    const io_connect_t c = gConn;
    kern_return_t kr = callOn(c, sel, in, inN, out, outN, sIn, sInN);
    // 0.8.28: NotReady while a GPU reset runs (kext 0.164.0 refuses binds and
    // submits then). Failing them left IOSurface textures without a GPU VA:
    // WindowServer drew with them after the reset, GR exception, another
    // reset, every 30 s (1 Oct 08:03). Wait for the reset (bounded), then the
    // Offline path below reopens.
    for (uint32_t w = 0; kr == kIOReturnNotReady && w < 400 && nvResetBusy(); w++) {
        usleep(20000);
        kr = callOn(gConn, sel, in, inN, out, outN, sIn, sInN);
    }
    // 0.6.12: after a GPU reset the kext refuses a client from before it
    // (kIOReturnOffline) for good; open a new one, put every object back at
    // its VA and go on, instead of failing every draw until WindowServer
    // restarts. 0.8.72: a client whose reset finished but whose arena/channel
    // stayed dead answers kIOReturnNotReady forever; treat that as offline too
    // once the reset is no longer busy (WindowServer went black after this).
    const bool stuckNotReady = kr == kIOReturnNotReady && !nvResetBusy() &&
                               nvResetCountNow() != gConnReset;
    if ((kr == kIOReturnOffline || stuckNotReady) && !tEncoding && nvGspRecover(c)) {
        // The old staging bytes disappeared with the connection. Rebinding
        // resources does not make an already-encoded method stream replayable.
        if (sel != kSelExecSeg) kr = callOn(gConn, sel, in, inN, out, outN, sIn, sInN);
    }
    if (kr != KERN_SUCCESS) {
        // 0.6.8: say why; rate limited so a stuck engine can't flood the log
        static uint32_t nlog;
        if (nlog++ < 64 || !(nlog & 1023)) NSLog(@"NVMTLGsp: selector %u -> 0x%x (#%u)", sel, kr, nlog);
    }
    return kr == KERN_SUCCESS;
}

// 0.6.8: through the accelerator first (connect type 'NVGP'): the same
// client, but reachable from any sandbox that allows Metal. NVGspControl
// directly is the fallback for older kexts.
static io_connect_t nvGspOpen(void) {
    io_connect_t c = IO_OBJECT_NULL;
    kern_return_t kr = KERN_FAILURE;
    io_service_t acc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVAccelerator"));
    if (acc) { kr = IOServiceOpen(acc, mach_task_self(), 0x4E564750, &c); IOObjectRelease(acc); }
    if (kr == KERN_SUCCESS) return c;
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVGspControl"));
    if (!svc) { NSLog(@"NVMTLGsp: no NVGspControl service"); return IO_OBJECT_NULL; }
    const kern_return_t k2 = IOServiceOpen(svc, mach_task_self(), 0, &c);
    IOObjectRelease(svc);
    if (k2 == KERN_SUCCESS) return c;
    static bool said;
    if (!said) { said = true; NSLog(@"NVMTLGsp: open failed (accelerator 0x%x, NVGspControl 0x%x)", kr, k2); }
    return IO_OBJECT_NULL;
}

// 0.8.68: own GR channel + VAS for this process (kext 0.178.0, selector 40),
// opt-in by NVMTL_OWN_CHANNEL=1 or /Library/Preferences/nvmtl-ownchannel.
// GR work then never waits for another process's arena switch. Refusal
// (older kext, no free slot) keeps the shared channel.
static void nvGspOwnChannel(io_connect_t c) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("NVMTL_OWN_CHANNEL");
        on = e ? atoi(e) != 0 : access("/Library/Preferences/nvmtl-ownchannel", F_OK) == 0;
    }
    if (!on) return;
    uint64_t in[1] = {1}, out[1] = {0};
    uint32_t outN = 1;
    const kern_return_t kr = callOn(c, 40, in, 1, out, &outN, NULL, 0);
    NSLog(@"NVMTLGsp: own GR channel kr 0x%x chid %llu", kr, out[0]);
}

// 6 MiB SYS object (2 MiB-aligned contiguous chunks, bindable) at gVa,
// mapped into the task
// 0.9.51: asynchronous submission (kernel 0.180.133 selector 48 flags).
// The last batch of a command buffer may return right after the kick; the
// next GPU-lock holder, waitUntilCompleted, status and completed handlers
// wait for its sequence. NVMTL_ASYNC=0 turns it off.
static uint32_t g615Pending, g615Completed;
static bool g615AsyncTail;          // set by the encoder for the final flush
static int g615AsyncOk = -1;        // kernel accepts flags (-1 unknown)
static bool g615WaitFailed;
bool nvGr615AsyncOn(void) {
    static int on = -1;
    // 0.9.52: default on (suite84 83/84, mtl4 16/16, 16 parallel, hang -> Error + recovery)
    if (on < 0) { const char *e = getenv("NVMTL_ASYNC"); on = e ? strcmp(e, "0") != 0 : 1; }
    return on && g615AsyncOk != 0;
}
void nvGr615SetAsyncTail(bool on) { g615AsyncTail = on; }
uint32_t nvGr615PendingSeq(void) { return g615Pending > g615Completed ? g615Pending : 0; }
// wait for `seq`; false when the GPU work failed (hang/fault)
bool nvGr615WaitSeq(uint32_t seq) {
    if (!seq || (int32_t)(seq - g615Completed) <= 0) return !g615WaitFailed || (int32_t)(seq - g615Completed) <= 0;
    uint64_t in[3] = {seq, 0, 2}, out[3] = {0, 0, 0};
    uint32_t n = 3;
    const kern_return_t kr = callOn(gConn, 48, in, 3, out, &n, NULL, 0);
    if (kr) {
        NSLog(@"NVMTLGsp: native615 wait for sequence %u -> 0x%x", seq, kr);
        g615WaitFailed = true; g615Completed = seq; nvNoteGpuFailure(2);
        return false;
    }
    if ((int32_t)(out[1] - g615Completed) > 0) g615Completed = (uint32_t)out[1];
    if ((int32_t)(seq - g615Completed) > 0) g615Completed = seq;
    return true;
}
static void nv615WaitPending(void) {
    if (gN615 && g615Pending && (int32_t)(g615Pending - g615Completed) > 0) nvGr615WaitSeq(g615Pending);
}
static bool nv615Submit(uint64_t va, uint32_t words) {
    uint64_t in[3] = {va, words, 0}, out[3] = {0, 0, 0};
    uint32_t n = 3;
    const bool async = g615AsyncTail && nvGr615AsyncOn();
    g615AsyncTail = false;
    if (async) {
        in[2] = 1;
        const kern_return_t ka = callOn(gConn, 48, in, 3, out, &n, NULL, 0);
        if (ka == KERN_SUCCESS) { g615AsyncOk = 1; g615Pending = (uint32_t)out[0]; return true; }
        if (ka == kIOReturnBadArgument && g615AsyncOk < 0) {
            g615AsyncOk = 0;   // an older kernel: synchronous from now on
            NSLog(@"NVMTLGsp: native615 async submission unsupported by the kernel");
        } else { NSLog(@"NVMTLGsp: native615 async submit of %u words -> 0x%x", words, ka); return false; }
        n = 3;
    }
    static int trace = -1;
    if (trace < 0) trace = getenv("NVMTL_TRACE") != NULL;
    const uint64_t t0 = trace ? mach_absolute_time() : 0;
    const kern_return_t kr = callOn(gConn, 48, in, 2, out, &n, NULL, 0);
    if (kr == KERN_SUCCESS) g615Completed = (uint32_t)out[0];   // synchronous: everything before it is done too
    if (trace) {
        mach_timebase_info_data_t tb; mach_timebase_info(&tb);
        NSLog(@"NVMTL_TRACE native615 submit %u words: %.1f us", words, (double)(mach_absolute_time() - t0) * tb.numer / tb.denom / 1e3);
    }
    if (kr) NSLog(@"NVMTLGsp: native615 submit of %u words at 0x%llx -> 0x%x", words, va, kr);
    return kr == KERN_SUCCESS;
}

static bool nvGspStaging(io_connect_t c, uint32_t *handle, uint8_t **cpu) {
    uint64_t in[2] = {NVGSP_STAGE_BYTES, 1}, out[2] = {0, 0};
    uint32_t outN = 2;
    if (callOn(c, kSelMemAlloc, in, 2, out, &outN, NULL, 0) || !out[0]) {
        NSLog(@"NVMTLGsp: memAlloc failed"); return false;
    }
    uint64_t b[3] = {out[0], NVGSP_ARENA_BASE, 0}; // whole object, PTE kind 0 (as Mesa)
    if (callOn(c, kSelVaBindObj, b, 3, NULL, NULL, NULL, 0)) {
        NSLog(@"NVMTLGsp: vaBindObject failed"); return false;
    }
    mach_vm_address_t addr = 0;
    mach_vm_size_t size = 0;
    if (IOConnectMapMemory(c, nvMapType((uint32_t)out[0]), mach_task_self(), &addr, &size,
                           kIOMapAnywhere) != KERN_SUCCESS || !addr || size < NVGSP_STAGE_BYTES) {
        NSLog(@"NVMTLGsp: map staging failed"); return false;
    }
    *handle = (uint32_t)out[0];
    *cpu = (uint8_t *)(uintptr_t)addr;
    return true;
}

bool nvGspEnsure(void) {
    NV_GPU_LOCKED;
    if (gCpu) return true;
    if (nvN615Requested()) {
        // 0.9.61: sandboxed apps (Safari, iconservicesagent, NotificationCenter)
        // cannot IOServiceOpen NVGspControl directly: MACF denies it and every
        // heap allocation then fails. Use the accelerator's 'NVGP' proxy
        // (IOAccelNVGspSharedClient, an IOAccelSharedUserClient2 that sandboxes
        // allow) which owns the real NVGspControl client; fall back to the
        // direct open only for older kexts.
        io_connect_t c = nvGspOpen();
        if (!c) { NSLog(@"NVMTLGsp: native615 open failed"); return false; }
        uint64_t op = 3, out[3] = {0, 0, 0};
        uint32_t n = 3;
        kern_return_t kr = callOn(c, 45, &op, 1, out, &n, NULL, 0);
        // 0.9.41: every client slot taken (or another client mid-operation):
        // wait for one to free up rather than fail the app, up to 30 s
        for (int i = 0; kr == kIOReturnBusy && i < 1500; i++) {
            usleep(20000);
            n = 3;
            kr = callOn(c, 45, &op, 1, out, &n, NULL, 0);
        }
        uint8_t *cpu = NULL;
        uint64_t va = 0;
        if (kr || !nv615Alloc(c, NV615_STAGE_BYTES, &cpu, &va, 0)) {
            NSLog(@"NVMTLGsp: native615 client create -> 0x%x", kr);
            IOServiceClose(c);
            return false;
        }
        gN615 = true;
        gConn = c; gHandle = 0; gCpu = cpu; gVa = va;
        gConnReset = nvResetCountNow();
        NSLog(@"NVMTLGsp: native615 client: staging va 0x%llx cpu %p", gVa, gCpu);
        return true;
    }
    const io_connect_t c = nvGspOpen();
    if (!c) return false;
    uint32_t handle = 0;
    uint8_t *cpu = NULL;
    if (!nvGspStaging(c, &handle, &cpu)) {
        // A reset/memory failure during first use must not leak a client
        // and its partial objects on every later allocation attempt.
        IOServiceClose(c);
        return false;
    }
    // Publish only a fully initialized client; another initializer cannot
    // overwrite a live connection while this thread builds its staging.
    nvGspOwnChannel(c);
    gConn = c; gHandle = handle; gCpu = cpu;
    gConnReset = nvResetCountNow();
    gVa = NVGSP_ARENA_BASE;
    NSLog(@"NVMTLGsp: staging va 0x%llx cpu %p handle %u", gVa, gCpu, gHandle);
    return true;
}

// 0.8.31: pure generation probe. Native encoding can otherwise reuse all
// resources after a reset and never hit a scalar bind that returns Offline.
bool nvGspValidate(void) {
    NV_GPU_LOCKED;
    if (!nvGspEnsure()) return false;
    uint64_t generation = 0;
    uint32_t count = 1;
    const bool ok = callMethod(37, NULL, 0, &generation, &count, NULL, 0);
    if (ok) atomic_store(&gGeneration, generation);
    return ok;
}

// ---------------------------------------------------------------- GPU heap
// Buffers are ordinary page-aligned process memory; the kext wires them and
// maps their 4 KiB pages at a VA of ours (selector 33, small page tables).
// The MTLBuffer wraps the same pages (NoCopy), so CPU and GPU share them.
#define NVGSP_USER_VA_BASE 0x2C00000000ULL
#define NVGSP_USER_VA_END  0x2F00000000ULL      // NVK's replay range starts here
enum { kSelUserMemBind = 33, kMaxUserBufs = 16384, kMaxUFree = 16384 };
typedef struct { uint8_t *cpu; uint64_t va, size; uint32_t handle, bindFlags; } NVUserBuf;
static NVUserBuf gBufs[kMaxUserBufs];
static unsigned gNBufs;
static uint64_t gUserVa = NVGSP_USER_VA_BASE;
static os_unfair_lock gHeapLock = OS_UNFAIR_LOCK_INIT;
static uint8_t *nv615WrapCpu(uint64_t va, uint64_t bytes) {
    uint8_t *p = NULL;
    os_unfair_lock_lock(&gHeapLock);
    for (unsigned i = 0; i < gNBufs && !p; i++)
        if (gBufs[i].cpu && (gBufs[i].handle & 0x40000000u) && va >= gBufs[i].va && bytes <= gBufs[i].size &&
            va - gBufs[i].va <= gBufs[i].size - bytes)
            p = gBufs[i].cpu + (va - gBufs[i].va);
    os_unfair_lock_unlock(&gHeapLock);
    return p;
}

// 0.6.8: user VA is handed back on free. It used to only grow, and
// WindowServer (a buffer or surface view every frame) ran through the 12 GiB
// in a minute or two; after that new buffers had no GPU address, shaders got
// 0 and the GR exception + GPU reset showed as a black flash.
// Spans are 64 KiB granular; caller holds gHeapLock.
typedef struct { uint64_t va, size; } NVUSpan;
static NVUSpan gUFree[kMaxUFree];
static unsigned gNUFree;
static uint64_t userVaTakeLocked(uint64_t len, uint64_t align) {
    len = (len + 0xffff) & ~0xffffULL;
    for (unsigned i = 0; i < gNUFree; i++) {
        const uint64_t a = (gUFree[i].va + align - 1) & ~(align - 1);
        const uint64_t lead = a - gUFree[i].va;
        if (gUFree[i].size < lead + len) continue;
        const NVUSpan f = gUFree[i];
        gUFree[i] = gUFree[--gNUFree];
        if (lead && gNUFree < kMaxUFree) gUFree[gNUFree++] = (NVUSpan){f.va, lead};
        if (f.size > lead + len && gNUFree < kMaxUFree) gUFree[gNUFree++] = (NVUSpan){a + len, f.size - lead - len};
        return a;
    }
    const uint64_t a = (gUserVa + align - 1) & ~(align - 1);
    if (a + len > NVGSP_USER_VA_END) return 0;
    gUserVa = a + len;
    return a;
}
static void userVaGiveLocked(uint64_t va, uint64_t len) {
    if (!va) return;
    len = (len + 0xffff) & ~0xffffULL;
    for (unsigned i = 0; i < gNUFree; i++) {            // merge with neighbours
        if (gUFree[i].va + gUFree[i].size == va) { va = gUFree[i].va; len += gUFree[i].size; gUFree[i] = gUFree[--gNUFree]; i = (unsigned)-1; continue; }
        if (va + len == gUFree[i].va) { len += gUFree[i].size; gUFree[i] = gUFree[--gNUFree]; i = (unsigned)-1; continue; }
    }
    if (va + len == gUserVa) { gUserVa = va; return; }  // top of the range: just lower it
    if (gNUFree < kMaxUFree) gUFree[gNUFree++] = (NVUSpan){va, len};
}

// 0.8.5: the last user VA ranges handed back, to name an MMU fault after a
// GPU reset: a range freed moments ago means work outlived its memory, a
// live range means the wrong VA arena was installed
typedef struct { uint64_t va, len, when; uint32_t kind; } NVFreed;
static NVFreed gFreed[512];
static uint32_t gFreedN;
static void noteFreed(uint64_t va, uint64_t len, uint32_t kind) {
    NVFreed *f = &gFreed[gFreedN++ & 511];
    f->va = va; f->len = len; f->when = mach_absolute_time(); f->kind = kind;
}
static uint64_t regU64(io_service_t s, CFStringRef key) {
    uint64_t v = 0;
    CFTypeRef r = IORegistryEntryCreateCFProperty(s, key, kCFAllocatorDefault, 0);
    if (r && CFGetTypeID(r) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)r, kCFNumberSInt64Type, &v);
    if (r) CFRelease(r);
    return v;
}
static void nvExplainFault(void) {
    io_service_t g = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVGspControl"));
    if (!g) return;
    // 32-bit properties come back as signed numbers: keep 32 bits of each
    const uint64_t va = ((regU64(g, CFSTR("NVGspControl-rc-mmu-fault-hi")) & 0xffffffffull) << 32) |
                        (regU64(g, CFSTR("NVGspControl-rc-mmu-fault-lo")) & 0xffffffffull);
    IOObjectRelease(g);
    if (!va) return;
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    const uint64_t now = mach_absolute_time();
    bool said = false;
    for (uint32_t i = 0; i < 512 && i < gFreedN; i++) {
        const NVFreed *f = &gFreed[(gFreedN - 1 - i) & 511];
        if (va >= f->va && va < f->va + f->len) {
            NSLog(@"NVMTLGsp: fault VA 0x%llx was %s 0x%llx+0x%llx, freed %.1f ms before the recovery",
                  va, f->kind ? "a wrapped surface" : "a heap buffer", f->va, f->len,
                  (now - f->when) * tb.numer / tb.denom / 1e6);
            said = true; break;
        }
    }
    os_unfair_lock_lock(&gHeapLock);
    for (unsigned i = 0; i < gNBufs; i++)
        if (va >= gBufs[i].va && va < gBufs[i].va + gBufs[i].size) {
            NSLog(@"NVMTLGsp: fault VA 0x%llx is inside a live buffer 0x%llx+0x%llx (%s): wrong arena?", va,
                  gBufs[i].va, gBufs[i].size, (gBufs[i].handle & 0x40000000u) ? "wrapped surface" : "heap");
            said = true; break;
        }
    os_unfair_lock_unlock(&gHeapLock);
    if (!said) NSLog(@"NVMTLGsp: fault VA 0x%llx: not one of ours (not live, not among the last %u frees)", va, gFreedN < 512 ? gFreedN : 512);
}

static void nvHeapFull(uint64_t len) {
    static uint32_t n;
    if (n++ < 8) NSLog(@"NVMTLGsp: no GPU VA left for %llu bytes (%u buffers, %u free spans)",
                       (unsigned long long)len, gNBufs, gNUFree);
}
// Unbind (the kext frees the object, unwires the pages and flushes the TLB),
// then the VA may go to the next buffer.
// Caller holds gHeapLock, which also protects the connection/handle swap
// in recovery. Never reopen and replay an old handle in a new namespace.
static void nvHeapReleaseVaLocked(NVUserBuf u, uint64_t h) {
    const kern_return_t kr = callOn(gConn, kSelMemFree, &h, 1, NULL, NULL, NULL, 0);
    if (kr == KERN_SUCCESS || kr == kIOReturnOffline) {
        noteFreed(u.va, u.size, (u.handle & 0x40000000u) ? 1 : 0);
        userVaGiveLocked(u.va, u.size);
    }
}

// NVMTL_HEAP=2: big-page variant. Kext SYS objects (2 MiB contiguous
// chunks, 2 MiB PTEs) mapped into our task; one object per buffer.
static bool heapAllocChunks(uint64_t bytes, void **cpu, uint64_t *va) {
    const uint64_t len = (bytes + 0x1fffff) & ~0x1fffffULL;
    uint64_t in[2] = {len, 1}, out[2] = {0, 0};
    uint32_t outN = 2;
    if (!callMethod(kSelMemAlloc, in, 2, out, &outN, NULL, 0) || !out[0]) return false;
    const uint32_t handle = (uint32_t)out[0];
    os_unfair_lock_lock(&gHeapLock);
    const uint64_t v = gNBufs < kMaxUserBufs ? userVaTakeLocked(len, 0x200000) : 0;
    const bool room = v != 0;
    os_unfair_lock_unlock(&gHeapLock);
    uint64_t bnd[3] = {handle, v, 0};
    mach_vm_address_t addr = 0;
    mach_vm_size_t msz = 0;
    if (!room || !callMethod(kSelVaBindObj, bnd, 3, NULL, NULL, NULL, 0) ||
        IOConnectMapMemory(gConn, nvMapType(handle), mach_task_self(), &addr, &msz, kIOMapAnywhere) != KERN_SUCCESS) {
        uint64_t h = handle;
        callMethod(kSelMemFree, &h, 1, NULL, NULL, NULL, 0);
        os_unfair_lock_lock(&gHeapLock); userVaGiveLocked(v, len); os_unfair_lock_unlock(&gHeapLock);
        return false;
    }
    os_unfair_lock_lock(&gHeapLock);
    gBufs[gNBufs++] = (NVUserBuf){(uint8_t *)(uintptr_t)addr, v, len, handle | 0x80000000u, 0};
    os_unfair_lock_unlock(&gHeapLock);
    *cpu = (void *)(uintptr_t)addr; *va = v;
    return true;
}

static bool heapAllocFlags(uint64_t bytes, void **cpu, uint64_t *va, uint32_t flags);
bool nvHeapAlloc(uint64_t bytes, void **cpu, uint64_t *va) { return heapAllocFlags(bytes, cpu, va, 0); }
// 0.8.16: Metal Shared buffers: system memory the GPU L2 may cache (kext
// 0.163.0 flag 0x200; it invalidates / syncs around every submission).
// Uncached, every GPU read was a PCIe round trip: 2 GB/s against 227 GB/s,
// 2.5 s per Vision convolution. NVMTL_SYSMEM_VOL=1 goes back to uncached.
bool nvHeapAllocShared(uint64_t bytes, void **cpu, uint64_t *va) {
    static int vol = -1;
    if (vol < 0) vol = getenv("NVMTL_SYSMEM_VOL") != NULL;
    return heapAllocFlags(bytes, cpu, va, vol ? 0 : 0x200);
}
static bool heapAllocFlags(uint64_t bytes, void **cpu, uint64_t *va, uint32_t flags) {
    NV_GPU_LOCKED_NOWAIT;   // 0.9.64: waits below only before reusing a freed span
    if (!bytes || !nvGspEnsure()) return false;
    if (gN615) {   // 0.9.26: bump suballocation, 64 KiB granular, never reused
        const uint64_t len = (bytes + 0xffff) & ~0xffffULL;
        const uint32_t kind = g615ReqKind;
        if (len > NV615_BIG_MAX) { nvHeapFull(len); return false; }
        bool reuse = false;
        os_unfair_lock_lock(&gHeapLock);
        for (unsigned i = 0; i < g615NFree; i++)
            if (g615Free[i].len >= len && g615Free[i].kind == kind) { reuse = true; break; }
        os_unfair_lock_unlock(&gHeapLock);
        if (reuse) nv615WaitPending();   // queued work may still use a freed span
        os_unfair_lock_lock(&gHeapLock);
        for (unsigned i = 0; i < g615NFree && gNBufs < kMaxUserBufs; i++) {
            if (g615Free[i].len < len || g615Free[i].kind != kind) continue;
            NV615Span f = g615Free[i];
            if (f.len > len) { g615Free[i] = (NV615Span){f.cpu + len, f.va + len, f.len - len, kind}; }
            else g615Free[i] = g615Free[--g615NFree];
            gBufs[gNBufs++] = (NVUserBuf){f.cpu, f.va, len, 0x20000000u, flags};
            os_unfair_lock_unlock(&gHeapLock);
            memset(f.cpu, 0, len);
            *cpu = f.cpu; *va = f.va;
            return true;
        }
        os_unfair_lock_unlock(&gHeapLock);
        NV615Slab *s = NULL;
        for (unsigned i = 0; i < g615NSlab && !s; i++)
            if (g615Slab[i].kind == kind && g615Slab[i].used + len <= g615Slab[i].size) s = &g615Slab[i];
        if (!s) {
            uint8_t *c = NULL;
            uint64_t v = 0;
            const uint64_t size = nv615SlabBytes(len, NV615_HEAP_SLAB);
            bool got = g615NSlab < NV615_HEAP_SLABS && nv615Alloc(gConn, size, &c, &v, kind | (nvSysCachedOn() ? 0x10u : 0u));
            if (!got && g615NSlab < NV615_HEAP_SLABS) {   // 0.9.91: window full: give free VRAM slabs back, retry
                os_unfair_lock_lock(&gHeapLock);
                const bool trimmed = nv615VramTrimLocked();
                os_unfair_lock_unlock(&gHeapLock);
                got = trimmed && nv615Alloc(gConn, size, &c, &v, kind | (nvSysCachedOn() ? 0x10u : 0u));
            }
            if (!got) { nvHeapFull(len); return false; }
            s = &g615Slab[g615NSlab++];
            *s = (NV615Slab){c, v, 0, size, kind};
        }
        os_unfair_lock_lock(&gHeapLock);
        const bool room = gNBufs < kMaxUserBufs;
        uint8_t *p = s->cpu + s->used;
        const uint64_t v = s->va + s->used;
        if (room) {
            s->used += len;
            gBufs[gNBufs++] = (NVUserBuf){p, v, len, 0x20000000u, flags};
        }
        os_unfair_lock_unlock(&gHeapLock);
        if (!room) return false;
        *cpu = p;
        *va = v;
        return true;
    }
    const char *m = getenv("NVMTL_HEAP");
    if (m && m[0] == '2') return heapAllocChunks(bytes, cpu, va);
    const uint64_t len = (bytes + 0xfff) & ~0xfffULL;
    mach_vm_address_t addr = 0;
    if (mach_vm_allocate(mach_task_self(), &addr, len, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) return false;
    os_unfair_lock_lock(&gHeapLock);
    const uint64_t v = gNBufs < kMaxUserBufs ? userVaTakeLocked(len, 0x10000) : 0;
    const bool room = v != 0;
    os_unfair_lock_unlock(&gHeapLock);
    uint64_t in[4] = {addr, len, v, flags}, out[1] = {0};
    uint32_t outN = 1;
    if (!room || !callMethod(kSelUserMemBind, in, 4, out, &outN, NULL, 0) || !out[0]) {
        if (!room) nvHeapFull(len);
        mach_vm_deallocate(mach_task_self(), addr, len);
        os_unfair_lock_lock(&gHeapLock); userVaGiveLocked(v, len); os_unfair_lock_unlock(&gHeapLock);
        return false;
    }
    os_unfair_lock_lock(&gHeapLock);
    gBufs[gNBufs++] = (NVUserBuf){(uint8_t *)(uintptr_t)addr, v, len, (uint32_t)out[0], flags};
    os_unfair_lock_unlock(&gHeapLock);
    *cpu = (void *)(uintptr_t)addr;
    *va = v;
    return true;
}

void nvHeapFree(void *cpu, uint64_t bytes) {
    (void)bytes;
    if (gN615) nvGrFlush();   // 0.9.27: nothing queued may still use a span we hand out again
    NVUserBuf u = {0};
    os_unfair_lock_lock(&gHeapLock);
    for (unsigned i = 0; i < gNBufs; i++)
        if (gBufs[i].cpu == cpu && !(gBufs[i].handle & 0x40000000u)) {   // never a wrapped surface
            u = gBufs[i]; gBufs[i] = gBufs[--gNBufs];
            // 0.9.26: native615 suballocations stay mapped until the process ends;
            // 0.9.27: their span goes on the free list (after queued GPU work)
            if (!(u.handle & 0x20000000u)) nvHeapReleaseVaLocked(u, u.handle & 0x7fffffffu);
            else if (g615NFree < 1024) {
                uint32_t k = 0;
                for (unsigned j = 0; j < g615NSlab; j++)
                    if (u.va >= g615Slab[j].va && u.va - g615Slab[j].va < g615Slab[j].size) k = g615Slab[j].kind;
                NV615Span f = {u.cpu, u.va, u.size, k};
                // 0.9.42: merge with free neighbours (adjacent on GPU and CPU, same kind)
                for (unsigned j = 0; j < g615NFree;) {
                    const NV615Span o = g615Free[j];
                    if (o.kind == k && o.va + o.len == f.va && o.cpu + o.len == f.cpu) f = (NV615Span){o.cpu, o.va, o.len + f.len, k};
                    else if (o.kind == k && f.va + f.len == o.va && f.cpu + f.len == o.cpu) f.len += o.len;
                    else { j++; continue; }
                    g615Free[j] = g615Free[--g615NFree];
                }
                g615Free[g615NFree++] = f;
            }
            break;
        }
    os_unfair_lock_unlock(&gHeapLock);
    if (!u.cpu || (u.handle & 0x20000000u)) return;
    uint64_t h = u.handle & 0x7fffffffu;
    if (u.handle & 0x80000000u) {                               // kext SYS object
        IOConnectUnmapMemory(gConn, nvMapType((uint32_t)h), mach_task_self(),
                             (mach_vm_address_t)(uintptr_t)u.cpu);
        // 0.180.12: the mapping was released but the kext object was never
        // freed. A long-running NVMTL_HEAP=2 process leaked every buffer on
        // the GPU side until reset/reload.
        uint64_t mh = h;
        callMethod(kSelMemFree, &mh, 1, NULL, NULL, NULL, 0);
        return;
    }
    mach_vm_deallocate(mach_task_self(), (mach_vm_address_t)(uintptr_t)u.cpu, u.size);
}

// handle bit 30: wrapped foreign memory (unbind only, the owner frees it)
// 0.9.35: native615 wraps through the owned client (kernel 0.180.117,
// selector 47 op 2/3); handle bit 28 marks them, the low bits are the index.
static bool nv615Wrap(void *cpu, uint64_t len, uint64_t *va) {
    const bool cached = nvSysCachedOn();   // 0.9.76
    uint64_t in[3] = {2, (uint64_t)(uintptr_t)cpu, len | (cached ? 0x10ull << 56 : 0)}, out[3] = {0, 0, 0};
    uint32_t n = 3;
    kern_return_t kr = callOn(gConn, 47, in, 3, out, &n, NULL, 0);
    if (kr && cached) {
        NSLog(@"NVMTLGsp: L2-cacheable wrap refused (0x%x), uncached", kr);
        g615SysCached = 0;
        in[2] = len; n = 3;
        kr = callOn(gConn, 47, in, 3, out, &n, NULL, 0);
    }
    if (kr || out[2] != len || !out[1]) {
        NSLog(@"NVMTLGsp: native615 wrap of %llu bytes -> 0x%x", (unsigned long long)len, kr);
        return false;
    }
    os_unfair_lock_lock(&gHeapLock);
    const bool room = gNBufs < kMaxUserBufs;
    if (room) gBufs[gNBufs++] = (NVUserBuf){(uint8_t *)cpu, out[1], len, 0x50000000u | (uint32_t)out[0], 0};
    os_unfair_lock_unlock(&gHeapLock);
    if (!room) {
        uint64_t u[2] = {3, out[0]}, o[3];
        uint32_t m = 3;
        callOn(gConn, 47, u, 2, o, &m, NULL, 0);
        return false;
    }
    *va = out[1];
    return true;
}
bool nvHeapWrap(void *cpu, uint64_t bytes, uint64_t *va) {
    NV_GPU_LOCKED_NOWAIT;   // 0.9.64: new pages at a free VA; queued work cannot see them
    if (!cpu || !bytes || ((uintptr_t)cpu & 0xfff) || !nvGspEnsure()) return false;
    if (gN615) return nv615Wrap(cpu, (bytes + 0xfff) & ~0xfffULL, va);
    const uint64_t len = (bytes + 0xfff) & ~0xfffULL;
    os_unfair_lock_lock(&gHeapLock);
    const uint64_t v = gNBufs < kMaxUserBufs ? userVaTakeLocked(len, 0x10000) : 0;
    const bool room = v != 0;
    os_unfair_lock_unlock(&gHeapLock);
    uint64_t in[4] = {(uint64_t)(uintptr_t)cpu, len, v, 0}, out[1] = {0};
    uint32_t outN = 1;
    if (!room || !callMethod(kSelUserMemBind, in, 4, out, &outN, NULL, 0) || !out[0]) {
        if (!room) nvHeapFull(len);
        os_unfair_lock_lock(&gHeapLock); userVaGiveLocked(v, len); os_unfair_lock_unlock(&gHeapLock);
        return false;
    }
    os_unfair_lock_lock(&gHeapLock);
    gBufs[gNBufs++] = (NVUserBuf){(uint8_t *)cpu, v, len, (uint32_t)out[0] | 0x40000000u, 0};
    os_unfair_lock_unlock(&gHeapLock);
    *va = v;
    return true;
}

// 0.6.11: by address AND GPU VA. CoreAnimation makes several textures on
// one IOSurface; each wrap has its own VA over the same pages, and taking
// the first entry with that CPU address unbound another texture's VA while
// the GPU still used it (MMU fault, PTE invalid, GPU reset at login).
void nvHeapUnwrap(void *cpu, uint64_t va) {
    if (gN615) nvGrFlush();   // 0.9.35: queued work may still read the surface
    NVUserBuf u = {0};
    os_unfair_lock_lock(&gHeapLock);
    for (unsigned i = 0; i < gNBufs; i++)
        if (gBufs[i].cpu == cpu && gBufs[i].va == va && (gBufs[i].handle & 0x40000000u)) {
            u = gBufs[i]; gBufs[i] = gBufs[--gNBufs];
            if (!(u.handle & 0x10000000u)) nvHeapReleaseVaLocked(u, u.handle & 0x3fffffffu);
            break;
        }
    os_unfair_lock_unlock(&gHeapLock);
    if (!u.cpu || !(u.handle & 0x10000000u)) return;
    uint64_t in[2] = {3, u.handle & 0xffffu}, out[3];
    uint32_t n = 3;
    const kern_return_t kr = callOn(gConn, 47, in, 2, out, &n, NULL, 0);
    if (kr) NSLog(@"NVMTLGsp: native615 unwrap %u -> 0x%x", u.handle & 0xffffu, kr);
}

uint64_t nvHeapVa(const void *cpu, uint64_t bytes) {
    const uint8_t *p = cpu;
    uint64_t va = 0;
    os_unfair_lock_lock(&gHeapLock);
    for (unsigned i = 0; i < gNBufs; i++) {
        const NVUserBuf *u = &gBufs[i];
        if (p >= u->cpu && p + bytes <= u->cpu + u->size) { va = u->va + (uint64_t)(p - u->cpu); break; }
    }
    os_unfair_lock_unlock(&gHeapLock);
    return va;
}

// ---------------------------------------------------------------- VRAM heap
#define NVGSP_VRAM_VA_BASE 0x2A00000000ULL
#define NVGSP_VRAM_VA_END  0x2C00000000ULL
#define NVGSP_VRAM_CHUNK   (64ULL << 20)
typedef struct { uint64_t va, size; } NVRange;
static NVRange gVFree[2048];
static unsigned gNVFree;
static uint64_t gVramVa = NVGSP_VRAM_VA_BASE;
static NVRange g615VSlab[256];               // 0.9.87/88: native615 kind-0 VRAM slabs
static unsigned g615NVSlab;
// 0.9.91: every native615 VRAM slab with its kernel slot, so a slab whose memory is all free again
// goes back to the kernel when an allocation fails (kind -1 = linear). Free spans are per kind, so
// without this a process that freed kind-6 textures could not get linear VRAM or system memory.
typedef struct { uint64_t va, size; uint32_t slot; int kind; } NV615Slot;
static NV615Slot g615Slots[64];
static unsigned g615NSlots;
static void nv615SlotNote(uint64_t va, uint64_t size, uint64_t slot, int kind) {
    if (g615NSlots < 64) g615Slots[g615NSlots++] = (NV615Slot){va, size, (uint32_t)slot, kind};
}
// Take [va, va+size) out of a free list if one span covers it (split the rest off).
static bool nv615SpanCut(NVRange *list, unsigned *n, unsigned cap, uint64_t va, uint64_t size) {
    for (unsigned i = 0; i < *n; i++) {
        const NVRange f = list[i];
        if (va < f.va || va + size > f.va + f.size) continue;
        const NVRange lo = {f.va, va - f.va}, hi = {va + size, f.va + f.size - va - size};
        if ((lo.size && hi.size) && *n >= cap) return false;   // no room for the split
        list[i] = list[--*n];
        if (lo.size) list[(*n)++] = lo;
        if (hi.size) list[(*n)++] = hi;
        return true;
    }
    return false;
}
// Caller holds gHeapLock (dropped around the kernel calls). True if at least one slab went back.
static bool nv615VramTrimLocked(void) {
    uint32_t slots[64];
    unsigned nfree = 0;
    for (unsigned i = 0; i < g615NSlots;) {
        NV615Slot *t = &g615Slots[i];
        bool cut;
        if (t->kind < 0) cut = nv615SpanCut(gVFree, &gNVFree, 2048, t->va, t->size);
        else cut = nv615SpanCut((NVRange *)g615VK[t->kind], &g615NVK[t->kind], 256, t->va, t->size);
        if (!cut) { i++; continue; }
        if (t->kind < 0)
            for (unsigned j = 0; j < g615NVSlab; j++)
                if (g615VSlab[j].va == t->va) { g615VSlab[j] = g615VSlab[--g615NVSlab]; break; }
        slots[nfree++] = t->slot;
        *t = g615Slots[--g615NSlots];
    }
    if (!nfree) return false;
    os_unfair_lock_unlock(&gHeapLock);
    for (unsigned i = 0; i < nfree; i++) {
        uint64_t in[2] = {1, slots[i]}, out[3];
        uint32_t n = 3;
        const kern_return_t kr = callOn(gConn, 47, in, 2, out, &n, NULL, 0);
        if (kr) NSLog(@"NVMTLGsp: native615 VRAM slab slot %u free -> 0x%x", slots[i], kr);
        else if (g615KernelAllocs) --g615KernelAllocs;
    }
    os_unfair_lock_lock(&gHeapLock);
    NSLog(@"NVMTLGsp: returned %u free VRAM slab(s) to the kernel", nfree);
    return true;
}

// 0.6.12: kext objects, kept so a GPU reset can put them back
typedef struct { uint64_t va, size; uint32_t handle; } NVChunk;
static NVChunk gVChunk[256];                 // VRAM heap chunks
static unsigned gNVChunk;
typedef struct { uint64_t va, size; uint32_t kind, handle, id; } NVKindObj;
static NVKindObj gKObj[1024];                // block-linear surfaces, scratch
static unsigned gNKObj;

static bool vramGrow(uint64_t need) {
    const uint64_t size = need > NVGSP_VRAM_CHUNK ? (need + (2 << 20) - 1) & ~((2ULL << 20) - 1) : NVGSP_VRAM_CHUNK;
    if (gVramVa + size > NVGSP_VRAM_VA_END || gNVFree >= 2048) return false;
    uint64_t in[2] = {size, 0}, out[2] = {0, 0};
    uint32_t outN = 2;
    if (!callMethod(kSelMemAlloc, in, 2, out, &outN, NULL, 0) || !out[0]) return false;
    uint64_t b[3] = {out[0], gVramVa, 0};
    if (!callMethod(kSelVaBindObj, b, 3, NULL, NULL, NULL, 0)) {
        uint64_t h = out[0];
        callMethod(kSelMemFree, &h, 1, NULL, NULL, NULL, 0);
        return false;
    }
    gVFree[gNVFree++] = (NVRange){gVramVa, size};
    if (gNVChunk < 256) gVChunk[gNVChunk++] = (NVChunk){gVramVa, size, (uint32_t)out[0]};
    gVramVa += size;
    return true;
}

// 0.5.1: block-linear surfaces get their own kext object, bound with the
// PTE kind the layout needs, in a VA range of their own
#define NVGSP_BL_VA_BASE 0x2880000000ULL
#define NVGSP_BL_VA_END  0x2A00000000ULL
static uint64_t gBlVa = NVGSP_BL_VA_BASE;
static NVRange gBlFree[2048];
static unsigned gNBlFree;

// 0.8.34: a lifetime bump pointer exhausted the 6 GiB arena after only
// 3072 tiny, already-released surfaces. All spans are 2 MiB aligned.
// Caller holds gHeapLock; frees coalesce before another allocation.
static uint64_t kindVaTakeLocked(uint64_t len) {
    for (unsigned i = 0; i < gNBlFree; i++) {
        if (gBlFree[i].size < len) continue;
        const uint64_t v = gBlFree[i].va;
        if (gBlFree[i].size == len) gBlFree[i] = gBlFree[--gNBlFree];
        else { gBlFree[i].va += len; gBlFree[i].size -= len; }
        return v;
    }
    if (len > NVGSP_BL_VA_END - gBlVa) return 0;
    const uint64_t v = gBlVa;
    gBlVa += len;
    return v;
}
static void kindVaGiveLocked(uint64_t va, uint64_t len) {
    for (unsigned i = 0; i < gNBlFree; i++) {
        if (gBlFree[i].va + gBlFree[i].size == va) {
            va = gBlFree[i].va; len += gBlFree[i].size;
            gBlFree[i] = gBlFree[--gNBlFree]; i = (unsigned)-1;
        } else if (va + len == gBlFree[i].va) {
            len += gBlFree[i].size;
            gBlFree[i] = gBlFree[--gNBlFree]; i = (unsigned)-1;
        }
    }
    if (va + len == gBlVa) gBlVa = va;
    else if (gNBlFree < 2048) gBlFree[gNBlFree++] = (NVRange){va, len};
}

static bool nvVramAllocKindRaw(uint64_t bytes, uint32_t kind, uint64_t *va, uint32_t *handle) {
    NV_GPU_LOCKED;
    if (nvN615Requested()) {   // 0.9.30: pinned sysmem, PTE kind 0 (block linear is a TIC/copy layout)
        const uint32_t k8 = kind <= 7 ? kind : 6;
        if (gN615 && nv615VramTexOn()) {   // 0.9.46: VRAM with the surface's kind
            uint64_t v = 0;
            const uint64_t len = (bytes + 0xffff) & ~0xffffULL;
            if (bytes && g615NKind < 2048 && nv615VramKindTake(k8, len, &v)) {
                os_unfair_lock_lock(&gHeapLock);
                const uint32_t id = ++g615KindId;
                g615Kind[g615NKind++] = (NV615Kind){id, NULL, v, len, k8};
                os_unfair_lock_unlock(&gHeapLock);
                *va = v; *handle = id;
                return true;
            }
        }
        void *cpu = NULL;
        g615ReqKind = kind <= 7 ? kind : 6;
        const bool got = bytes && g615NKind < 2048 && heapAllocFlags(bytes, &cpu, va, 0);
        g615ReqKind = 0;
        if (!got) return false;
        os_unfair_lock_lock(&gHeapLock);
        const uint32_t id = ++g615KindId;
        g615Kind[g615NKind++] = (NV615Kind){id, (uint8_t *)cpu, *va, bytes, k8};
        os_unfair_lock_unlock(&gHeapLock);
        *handle = id;
        return true;
    }
    if (!bytes || bytes > NVGSP_BL_VA_END - NVGSP_BL_VA_BASE || !nvGspValidate()) return false;
    const uint64_t len = (bytes + 0x1fffff) & ~0x1fffffULL;
    os_unfair_lock_lock(&gHeapLock);
    // Every successful allocation must be tracked for free and recovery.
    const uint64_t v = gNKObj < 1024 ? kindVaTakeLocked(len) : 0;
    os_unfair_lock_unlock(&gHeapLock);
    if (!v) return false;
    const io_connect_t c = gConn;
    uint64_t in[2] = {len, 0}, out[2] = {0, 0};
    uint32_t outN = 2;
    bool ok = callOn(c, kSelMemAlloc, in, 2, out, &outN, NULL, 0) == KERN_SUCCESS && out[0];
    uint64_t b[3] = {out[0], v, kind & 0xff};
    if (ok) ok = callOn(c, kSelVaBindObj, b, 3, NULL, NULL, NULL, 0) == KERN_SUCCESS;
    if (!ok) {
        uint64_t h = out[0];
        const kern_return_t kr = h ? callOn(c, kSelMemFree, &h, 1, NULL, NULL, NULL, 0) : KERN_SUCCESS;
        if (kr == KERN_SUCCESS || kr == kIOReturnOffline) {
            os_unfair_lock_lock(&gHeapLock); kindVaGiveLocked(v, len); os_unfair_lock_unlock(&gHeapLock);
        }
        return false;
    }
    // Caller-visible IDs survive reset; they are never raw kext handles.
    static uint32_t nextId = 0x40000000u;
    os_unfair_lock_lock(&gHeapLock);
    const uint32_t id = ++nextId ? nextId : ++nextId;
    gKObj[gNKObj++] = (NVKindObj){v, len, kind, (uint32_t)out[0], id};
    os_unfair_lock_unlock(&gHeapLock);
    *va = v; *handle = id;
    return true;
}

void nvVramFreeKind(uint32_t handle) {
    if (!handle) return;
    if (gN615) {   // 0.9.30
        NV615Kind u = {0};
        os_unfair_lock_lock(&gHeapLock);
        for (unsigned i = 0; i < g615NKind; i++)
            if (g615Kind[i].id == handle) { u = g615Kind[i]; g615Kind[i] = g615Kind[--g615NKind]; break; }
        os_unfair_lock_unlock(&gHeapLock);
        if (u.cpu) nvHeapFree(u.cpu, 0);
        else if (u.id && u.size) { nvGrFlush(); nv615VramKindGive(u.kind, u.va, u.size); }
        return;
    }
    // Do not take the encoding mutex in a completion handler. Holding the
    // heap lock across the pure free prevents recovery swapping namespaces.
    os_unfair_lock_lock(&gHeapLock);
    for (unsigned i = 0; i < gNKObj; i++) {
        if (gKObj[i].id != handle) continue;
        const NVKindObj u = gKObj[i];
        uint64_t h = u.handle;
        const kern_return_t kr = callOn(gConn, kSelMemFree, &h, 1, NULL, NULL, NULL, 0);
        gKObj[i] = gKObj[--gNKObj];
        if (kr == KERN_SUCCESS || kr == kIOReturnOffline) kindVaGiveLocked(u.va, u.size);
        break;
    }
    os_unfair_lock_unlock(&gHeapLock);
}

// 0.8.12: is a VRAM-range VA inside a live allocation (fault dumps)
static const char *nvVramState(uint64_t p) {
    const char *r = "not in any VRAM chunk or surface";
    os_unfair_lock_lock(&gHeapLock);
    for (unsigned i = 0; i < gNKObj; i++)
        if (p >= gKObj[i].va && p < gKObj[i].va + gKObj[i].size) { r = "live block-linear/kind object"; goto out; }
    for (unsigned i = 0; i < gNVChunk; i++)
        if (p >= gVChunk[i].va && p < gVChunk[i].va + gVChunk[i].size) {
            r = "live VRAM allocation";
            for (unsigned f = 0; f < gNVFree; f++)
                if (p >= gVFree[f].va && p < gVFree[f].va + gVFree[f].size) { r = "FREED VRAM (in the free list)"; break; }
            goto out;
        }
out:
    os_unfair_lock_unlock(&gHeapLock);
    return r;
}

// 0.8.13: any VA of ours: heap buffer, VRAM, block-linear, or nothing
const char *nvVaState(uint64_t p) {
    if (p >= NVGSP_USER_VA_BASE && p < NVGSP_USER_VA_END) {
        bool live = false;
        os_unfair_lock_lock(&gHeapLock);
        for (unsigned b = 0; b < gNBufs && !live; b++) live = p >= gBufs[b].va && p < gBufs[b].va + gBufs[b].size;
        os_unfair_lock_unlock(&gHeapLock);
        return live ? "live heap buffer" : "NOT a live heap buffer";
    }
    if (p >= 0x2880000000ULL && p < NVGSP_USER_VA_BASE) return nvVramState(p);
    return "outside our arenas";
}

static void nvSlabsReleaseAll(void);
static bool nvGspRecover(io_connect_t dead) {
    if (gN615) return false;   // 0.9.26: a lost native615 client is not rebuilt
    // Allocation callers may hold gHeapLock. Never block on gGpuMtx from
    // there: an encoder can hold it while looking up a heap VA. Defer that
    // caller's recovery if encoding is in progress on another thread.
    pthread_once(&gGpuOnce, gpuMtxInit);
    if (pthread_mutex_trylock(&gGpuMtx) != 0) return false;
    int recoverLk __attribute__((cleanup(gpuUnlockAtExit), unused)) = 0;
    static os_unfair_lock rl = OS_UNFAIR_LOCK_INIT;
    static uint32_t n;
    os_unfair_lock_lock(&rl);
    if (gConn != dead) { os_unfair_lock_unlock(&rl); return true; }   // another thread did it
    // 0.8.13: a GPU that stays down must not turn every call into a reopen
    // attempt (a Vision client spun through 2180 of them in 6 minutes):
    // back off 100 ms, doubling to 5 s, and fail fast in between
    static uint64_t nextTry;
    static uint32_t fails;
    const uint64_t nowAbs = mach_absolute_time();
    if (nowAbs < nextTry) { os_unfair_lock_unlock(&rl); return false; }
    const io_connect_t c = nvGspOpen();
    uint32_t sh = 0;
    uint8_t *scpu = NULL;
    bool ok = c && nvGspStaging(c, &sh, &scpu);
    if (ok) nvGspOwnChannel(c);
    // 0.8.79: keep the dead client alive until every rebind succeeds. The
    // kext's shared GR context block is allocated from client heap memory;
    // closing the old client first freed it and grRing() then failed
    // (seg-stage 2). kMaxMem is 65535 in kext 0.178.30, so the table has room
    // for the old + new objects during recovery.
    bool deadClosed = false;
    unsigned nb = 0, nv = 0, nk = 0;
    // the caller may hold the heap lock (vramGrow under nvVramAlloc): then
    // this call fails and the next one recovers
    if (!os_unfair_lock_trylock(&gHeapLock)) {
        if (c) IOServiceClose(c);
        os_unfair_lock_unlock(&rl);
        return false;
    }
    // Commit new handles only after every rebind succeeds. A failed late
    // bind used to leave a mixture of old/new client handle namespaces,
    // so a resource freed before the next retry could free the wrong object.
    const unsigned total = gNBufs + gNVChunk + gNKObj;
    uint32_t *handles = calloc(total ? total : 1, sizeof(uint32_t));
    if (!handles) ok = false;
    const char *rstage = ok ? "rebind" : "open";
    unsigned rindex = 0;
    uint64_t rva = 0, rsize = 0;
    kern_return_t rkr = 0;
    for (unsigned i = 0; ok && i < gNBufs; i++) {          // process pages: bind them again
        NVUserBuf *u = &gBufs[i];
        if (u->handle & 0x80000000u) continue;              // NVMTL_HEAP=2 objects are not kept
        uint64_t in[4] = {(uint64_t)(uintptr_t)u->cpu, u->size, u->va, u->bindFlags}, out[1] = {0};
        uint32_t outN = 1;
        rindex = i; rva = u->va; rsize = u->size;
        rkr = callOn(c, kSelUserMemBind, in, 4, out, &outN, NULL, 0);
        if (rkr || !out[0]) { ok = false; rstage = "bind"; break; }
        handles[i] = (uint32_t)out[0] | (u->handle & 0x40000000u);
        nb++;
    }
    unsigned adopted = 0;
    // NVMTL_HEAP=2 big-page objects: they live in gBufs with the tag bit but
    // are kext SYS objects, so the user-page path above skips them. Re-adopt
    // or reallocate them at the same VA and keep the child mapping valid.
    for (unsigned i = 0; ok && i < gNBufs; i++) {
        NVUserBuf *u = &gBufs[i];
        if (!(u->handle & 0x80000000u)) continue;
        const uint32_t oldHandle = u->handle & 0x7fffffffu;
        uint64_t in[2] = {u->size, 1}, out[2] = {0, 0};
        uint32_t outN = 2;
        uint64_t ah[1] = {oldHandle};
        rindex = i; rva = u->va; rsize = u->size;
        if (!callOn(c, kSelMemAdopt, ah, 1, out, &outN, NULL, 0) && out[0]) {
            adopted++;
        } else if ((outN = 2, out[0] = 0,
                    callOn(c, kSelMemAlloc, in, 2, out, &outN, NULL, 0)) || !out[0]) {
            ok = false; rstage = "chunk-alloc"; break;
        }
        uint64_t b[3] = {out[0], u->va, 0};
        rkr = callOn(c, kSelVaBindObj, b, 3, NULL, NULL, NULL, 0);
        if (rkr) { ok = false; rstage = "chunk-bind"; break; }
        // The CPU mapping belonged to the old client. Map the adopted object
        // through the new client before the old client is closed; otherwise
        // the next contents access is a stale mapping (or unmapped memory).
        mach_vm_address_t naddr = 0;
        mach_vm_size_t msz = 0;
        rkr = IOConnectMapMemory(c, nvMapType((uint32_t)out[0]), mach_task_self(),
                                 &naddr, &msz, kIOMapAnywhere);
        if (rkr != KERN_SUCCESS || msz < u->size) {
            if (rkr == KERN_SUCCESS)
                mach_vm_deallocate(mach_task_self(), naddr, msz);
            ok = false; rstage = "chunk-map"; break;
        }
        IOConnectUnmapMemory(dead, nvMapType(oldHandle), mach_task_self(),
                             (mach_vm_address_t)(uintptr_t)u->cpu);
        u->cpu = (uint8_t *)(uintptr_t)naddr;
        handles[i] = (uint32_t)out[0] | 0x80000000u;
        nb++;
    }
    for (unsigned i = 0; ok && i < gNVChunk; i++) {        // VRAM: same memory if the kext kept it, same VA
        uint64_t in[2] = {gVChunk[i].size, 0}, out[2] = {0, 0};
        uint32_t outN = 2;
        // 0.8.52: take the old object over (kext 0.177: its contents were saved and put back)
        uint64_t ah[1] = {gVChunk[i].handle};
        if (!callOn(c, kSelMemAdopt, ah, 1, out, &outN, NULL, 0) && out[0]) adopted++;
        else if ((outN = 2, out[0] = 0, callOn(c, kSelMemAlloc, in, 2, out, &outN, NULL, 0)) || !out[0]) { ok = false; rstage = "vram-alloc"; break; }
        uint64_t b[3] = {out[0], gVChunk[i].va, 0};
        rindex = i; rva = gVChunk[i].va; rsize = gVChunk[i].size;
        rkr = callOn(c, kSelVaBindObj, b, 3, NULL, NULL, NULL, 0);
        if (rkr) { ok = false; rstage = "vram-bind"; break; }
        handles[gNBufs + i] = (uint32_t)out[0];
        nv++;
    }
    for (unsigned i = 0; ok && i < gNKObj; i++) {
        uint64_t in[2] = {gKObj[i].size, 0}, out[2] = {0, 0};
        uint32_t outN = 2;
        uint64_t ah[1] = {gKObj[i].handle};
        if (!callOn(c, kSelMemAdopt, ah, 1, out, &outN, NULL, 0) && out[0]) adopted++;
        else if ((outN = 2, out[0] = 0, callOn(c, kSelMemAlloc, in, 2, out, &outN, NULL, 0)) || !out[0]) { ok = false; rstage = "kobj-alloc"; break; }
        uint64_t b[3] = {out[0], gKObj[i].va, gKObj[i].kind & 0xff};
        rindex = i; rva = gKObj[i].va; rsize = gKObj[i].size;
        rkr = callOn(c, kSelVaBindObj, b, 3, NULL, NULL, NULL, 0);
        if (rkr) { ok = false; rstage = "kobj-bind"; break; }
        handles[gNBufs + gNVChunk + i] = (uint32_t)out[0];
        nk++;
    }
    if (ok) {
        for (unsigned i = 0; i < gNBufs; i++)
            gBufs[i].handle = handles[i];
        for (unsigned i = 0; i < gNVChunk; i++) gVChunk[i].handle = handles[gNBufs + i];
        for (unsigned i = 0; i < gNKObj; i++) gKObj[i].handle = handles[gNBufs + gNVChunk + i];
        gConn = c; gHandle = sh; gCpu = scpu;
        gConnReset = nvResetCountNow();
        gRecoverySerial++;
        if (!deadClosed) IOServiceClose(dead);              // the old client's objects go
    }
    free(handles);
    os_unfair_lock_unlock(&gHeapLock);
    if (!ok && c) IOServiceClose(c);
    ++n;
    if (ok || fails < 5 || !(fails % 100))
        NSLog(@"NVMTLGsp: GPU reset: %s (recovery %u: %u buffers, %u VRAM chunks, %u surfaces, %u kept with contents)",
              ok ? "reopened" : "could not reopen", n, nb, nv, nk, adopted);
    if (!ok)
        NSLog(@"NVMTLGsp: reset detail: stage %s index %u va 0x%llx size %llu kr 0x%x",
              rstage, rindex, rva, rsize, rkr);
    if (ok) { fails = 0; nextTry = 0; gPostResetLog = 20; }
    // 0.8.29: VRAM contents are gone, so the command buffer that saw the
    // reset reports an error. WindowServer then flushes its display space
    // ("to aid GPU restart recovery") and redraws layers it kept in VRAM;
    // with 0.8.28 nothing failed and the lock-screen clock stayed black.
    if (ok) nvNoteGpuFailure(1);
    else {
        mach_timebase_info_data_t tb; mach_timebase_info(&tb);
        const uint64_t ms = fails < 6 ? 100ull << fails : 5000;
        nextTry = mach_absolute_time() + ms * 1000000ull * tb.denom / tb.numer;
        fails++;
    }
    nvExplainFault();   // 0.8.5
    // 0.8.6: command buffers out with the old connection never complete
    // through it; their slabs are ours again (NVAccelerator completes them)
    nvSlabsReleaseAll();
    os_unfair_lock_unlock(&rl);
    return ok;
}

// 0.9.45: Private buffers in VRAM (kernel 0.180.131
// selector 47 op 4: zeroed by the kernel, GPU-only, no CPU mapping). They
// are reached only through the copy engine (blits, fills, staging reads).
static bool nv615CeOn(void);
static bool nv615VramOn(void) {
    static int on = -1;
    // default on (suite84 83/84, 128 MiB compute copy 505 GB/s vs 5-42 in sysmem); NVMTL_N615_VRAM=0 off
    if (on < 0) { const char *e = getenv("NVMTL_N615_VRAM"); on = !e || strcmp(e, "0"); }
    return on;
}
static bool nv615VramGrowLocked(uint64_t len) {
    const uint64_t bytes = (nv615SlabBytes(len, 64ull << 20) + 0x1fffff) & ~0x1fffffULL;
    uint64_t in[2] = {4, bytes}, out[3] = {0, 0, 0};
    uint32_t n = 3;
    os_unfair_lock_unlock(&gHeapLock);
    kern_return_t kr = callOn(gConn, 47, in, 2, out, &n, NULL, 0);
    os_unfair_lock_lock(&gHeapLock);
    if (kr == kIOReturnNoSpace && nv615VramTrimLocked()) {
        n = 3;
        os_unfair_lock_unlock(&gHeapLock);
        kr = callOn(gConn, 47, in, 2, out, &n, NULL, 0);
        os_unfair_lock_lock(&gHeapLock);
    }
    if (kr || out[2] < bytes || gNVFree >= 2048) {
        NSLog(@"NVMTLGsp: native615 VRAM slab of %llu bytes -> 0x%x", bytes, kr);
        return false;
    }
    ++g615KernelAllocs;
    nv615SlotNote(out[1], out[2], out[0], -1);
    gVFree[gNVFree++] = (NVRange){out[1], out[2]};
    if (g615NVSlab < 256) g615VSlab[g615NVSlab++] = (NVRange){out[1], out[2]};
    return true;
}
// 0.9.87/88: pitch (kind 0) VRAM, legacy heap or native615 slabs. Native slabs sit
// below 0x2A00000000, so the compute blit's old range test sent them to the CE.
bool nvGspVramLinear(uint64_t va, uint64_t n) {
    if (va >= NVGSP_VRAM_VA_BASE && n <= NVGSP_VRAM_VA_END - va) return true;
    bool in = false;
    os_unfair_lock_lock(&gHeapLock);
    for (unsigned i = 0; i < g615NVSlab && !in; i++)
        in = va >= g615VSlab[i].va && n <= g615VSlab[i].size && va - g615VSlab[i].va <= g615VSlab[i].size - n;
    os_unfair_lock_unlock(&gHeapLock);
    return in;
}
static bool nv615VramTexOn(void) {
    static int on = -1;
    // 0.9.46: block-linear textures, MSAA and zeta in VRAM by default (suite84 83/84); NVMTL_N615_VRAMTEX=0 off
    if (on < 0) { const char *e = getenv("NVMTL_N615_VRAMTEX"); on = !e || strcmp(e, "0"); }
    return on && nv615VramOn() && nv615CeOn();
}
static bool nv615VramKindTake(uint32_t kind, uint64_t len, uint64_t *va) {
    os_unfair_lock_lock(&gHeapLock);
    for (int pass = 0; pass < 2; pass++) {
        for (unsigned i = 0; i < g615NVK[kind]; i++) {
            NV615VSpan *f = &g615VK[kind][i];
            if (f->size < len) continue;
            *va = f->va;
            if (f->size == len) *f = g615VK[kind][--g615NVK[kind]];
            else { f->va += len; f->size -= len; }
            os_unfair_lock_unlock(&gHeapLock);
            return true;
        }
        if (pass || g615NVK[kind] >= 256) break;
        const uint64_t bytes = (nv615SlabBytes(len, 64ull << 20) + 0x1fffff) & ~0x1fffffULL;
        uint64_t in[2] = {4, bytes | ((uint64_t)kind << 56)}, out[3] = {0, 0, 0};
        uint32_t n = 3;
        os_unfair_lock_unlock(&gHeapLock);
        kern_return_t kr = callOn(gConn, 47, in, 2, out, &n, NULL, 0);
        os_unfair_lock_lock(&gHeapLock);
        if (kr == kIOReturnNoSpace && nv615VramTrimLocked()) {
            n = 3;
            os_unfair_lock_unlock(&gHeapLock);
            kr = callOn(gConn, 47, in, 2, out, &n, NULL, 0);
            os_unfair_lock_lock(&gHeapLock);
        }
        if (kr || out[2] < bytes) { NSLog(@"NVMTLGsp: native615 VRAM kind %u slab of %llu bytes -> 0x%x", kind, bytes, kr); break; }
        ++g615KernelAllocs;
        nv615SlotNote(out[1], out[2], out[0], (int)kind);
        g615VK[kind][g615NVK[kind]++] = (NV615VSpan){out[1], out[2]};
    }
    os_unfair_lock_unlock(&gHeapLock);
    return false;
}
static void nv615VramKindGive(uint32_t kind, uint64_t va, uint64_t len) {
    os_unfair_lock_lock(&gHeapLock);
    for (unsigned i = 0; i < g615NVK[kind]; i++) {
        NV615VSpan *f = &g615VK[kind][i];
        if (f->va + f->size == va) { va = f->va; len += f->size; *f = g615VK[kind][--g615NVK[kind]]; i = (unsigned)-1; }
        else if (va + len == f->va) { len += f->size; *f = g615VK[kind][--g615NVK[kind]]; i = (unsigned)-1; }
    }
    if (g615NVK[kind] < 256) g615VK[kind][g615NVK[kind]++] = (NV615VSpan){va, len};
    os_unfair_lock_unlock(&gHeapLock);
}
static bool nvVramAllocRaw(uint64_t bytes, uint64_t *va) {
    if (!bytes || !nvGspEnsure()) return false;
    if (gN615 && !(nv615VramOn() && nv615CeOn())) { void *cpu = NULL; return heapAllocFlags(bytes, &cpu, va, 0); }   // 0.9.27
    const uint64_t len = (bytes + 0xffff) & ~0xffffULL;
    bool ok = false;
    os_unfair_lock_lock(&gHeapLock);
    for (int pass = 0; pass < 2 && !ok; pass++) {
        for (unsigned i = 0; i < gNVFree; i++) {
            if (gVFree[i].size < len) continue;
            *va = gVFree[i].va;
            if (gVFree[i].size == len) gVFree[i] = gVFree[--gNVFree];
            else { gVFree[i].va += len; gVFree[i].size -= len; }
            ok = true;
            break;
        }
        if (!ok && (pass || !(gN615 ? nv615VramGrowLocked(len) : vramGrow(len)))) break;
    }
    os_unfair_lock_unlock(&gHeapLock);
    return ok;
}

void nvVramFree(uint64_t va, uint64_t bytes) {
    if (gN615) {   // 0.9.27; 0.9.45: a VRAM span has no CPU address
        uint8_t *c = nv615Cpu(va, bytes ? bytes : 1);
        if (c) { nvHeapFree(c, bytes); return; }
        if (!nv615VramOn()) return;
        nvGrFlush();   // queued work may still use the span
    }
    uint64_t len = (bytes + 0xffff) & ~0xffffULL;
    os_unfair_lock_lock(&gHeapLock);
    // 0.8.38: a freed bridge must absorb BOTH neighbours. Merging only
    // once left a fully freed 64 MiB chunk split into two unusable spans.
    for (unsigned i = 0; i < gNVFree; i++) {
        if (gVFree[i].va + gVFree[i].size == va) {
            va = gVFree[i].va; len += gVFree[i].size;
            gVFree[i] = gVFree[--gNVFree]; i = (unsigned)-1;
        } else if (va + len == gVFree[i].va) {
            len += gVFree[i].size;
            gVFree[i] = gVFree[--gNVFree]; i = (unsigned)-1;
        }
    }
    if (gNVFree < 2048) gVFree[gNVFree++] = (NVRange){va, len};
    os_unfair_lock_unlock(&gHeapLock);
}

uint8_t *nvStageCpu(void) { return gCpu; }
uint64_t nvStageVa(void) { return gVa; }

// One 8-word fence tail is appended by the kext (submitSegments); the
// pushbuffer holds SET_OBJECT + copy chunks only.
// Submit w words from the push area on the CE and wait for them.
// 0.9.43: kernel 0.180.128 gives every owned client a copy object on its GR
// channel (client615-copy-engine). NVMTL_N615_CE=1 sends the CE method
// streams there (subchannel 4 bound to AMPERE_DMA_COPY_B per stream).
static bool nv615VramOn(void);
static bool nv615CeOn(void) {
    static int on = -1;
    if (on >= 0) return on;
    on = 0;
    const char *e = getenv("NVMTL_N615_CE");
    if ((!e || strcmp(e, "1")) && !nv615VramOn()) return false;
    io_service_t sv = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVGspControl"));
    CFTypeRef p = sv ? IORegistryEntryCreateCFProperty(sv, CFSTR("NVGspControl-client615-copy-engine"), kCFAllocatorDefault, 0) : NULL;
    long long ce = 0;
    if (p && CFGetTypeID(p) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)p, kCFNumberSInt64Type, &ce);
    if (p) CFRelease(p);
    if (sv) IOObjectRelease(sv);
    on = ce >= 9;
    NSLog(@"NVMTLGsp: native615 copy engine %s (engine %lld)", on ? "on" : "unavailable", ce);
    return on;
}
static bool ceSubmit(uint32_t w) {
    if (gN615) {
        if (!nv615CeOn()) { NSLog(@"NVMTLGsp: native615 has no copy engine (%u words refused)", w); nvNoteGpuFailure(1); return false; }
        if (!nvGrFlush()) return false;
        uint32_t *pb = (uint32_t *)(gCpu + NVGSP_STAGE_PUSH);
        memmove(pb + 2, pb, (size_t)w * 4);
        pb[0] = mthd(NVGSP_CE_SUBCH, 0x0, 1); pb[1] = 0xC7B5;   // SET_OBJECT AMPERE_DMA_COPY_B
        if (!nv615Submit(gVa + NVGSP_STAGE_PUSH, w + 2)) { nvNoteGpuFailure(1); return false; }
        return true;
    }
    nvGrFlush();                                   // queued launches first, in order
    nvNativeDrain();                               // 0.8.0: and native GR work (other ring)
    struct { uint64_t va; uint32_t dw; uint32_t pad; } seg = {gVa + NVGSP_STAGE_PUSH, w, 0};
    uint64_t in[1] = {1}; // engine 1 = CE
    uint64_t out[1] = {0};
    uint32_t outN = 1;
    if (!callMethod(kSelExecSeg, in, 1, out, &outN, &seg, sizeof(seg)) || !out[0]) {
        NSLog(@"NVMTLGsp: execSegments failed"); nvNoteGpuFailure(1); return false;
    }
    uint64_t wi[2] = {out[0], nvFenceUs()}, wo[1] = {0};
    outN = 1;
    if (!callMethod(kSelCeWait, wi, 2, wo, &outN, NULL, 0)) {
        NSLog(@"NVMTLGsp: ceFenceWait failed"); nvNoteGpuFailure(2); return false;
    }
    return true;
}

// Any size: 128 KiB lines like NVK; when the push area fills up we submit
// and keep going (zero-copy buffers are no longer capped by staging).
bool nvCeCopy(uint64_t dstVa, uint64_t srcVa, uint64_t bytes) {
    NV_GPU_LOCKED;
    if (!gCpu || !bytes) return false;
    // 0.9.27: the CPU sees every sysmem native615 byte; 0.9.45: VRAM goes through the CE
    if (gN615 && (!nv615CeOn() || (nv615Cpu(dstVa, bytes) && nv615Cpu(srcVa, bytes)))) {
        if (!nvGrFlush()) return false;
        uint8_t *d = nv615Cpu(dstVa, bytes), *s = nv615Cpu(srcVa, bytes);
        if (!d || !s) { NSLog(@"NVMTLGsp: native615 copy outside mapped memory"); nvNoteGpuFailure(1); return false; }
        memmove(d, s, bytes);
        return true;
    }
    uint32_t *pb = (uint32_t *)(gCpu + NVGSP_STAGE_PUSH);
    uint32_t w = 0;
    const uint32_t cap = NVGSP_STAGE_PUSH_MAX / 4;
    uint64_t s = srcVa, d = dstVa, left = bytes;
    while (left) {
        if (w + 12 > cap) { if (!ceSubmit(w)) return false; w = 0; }
        const uint32_t n = left > CE_CHUNK ? CE_CHUNK : (uint32_t)left;
        pb[w++] = mthd(NVGSP_CE_SUBCH, B5_OFF_IN_UP, 4);
        pb[w++] = (uint32_t)(s >> 32); pb[w++] = (uint32_t)s;
        pb[w++] = (uint32_t)(d >> 32); pb[w++] = (uint32_t)d;
        pb[w++] = mthd(NVGSP_CE_SUBCH, B5_LINE_LEN, 2);
        pb[w++] = n; pb[w++] = 1;
        pb[w++] = mthd(NVGSP_CE_SUBCH, B5_LAUNCH, 1);
        pb[w++] = B5_LAUNCH_PITCH_COPY;
        s += n; d += n; left -= n;
    }
    return ceSubmit(w);
}

bool nvCeCopy2DBL(bool toBL, uint64_t pitchVa, uint32_t pitch, uint64_t blVa, uint32_t blWidthBytes,
                  uint32_t bh, uint32_t widthBytes, uint32_t height) {
    NV_GPU_LOCKED;
    if (!gCpu || !widthBytes || !height) return false;
    const NV615Bl b615 = {blWidthBytes, height, 1, bh & 15, 0, (bh >> 8) & 0xff};
    if (gN615 && (!nv615CeOn() || (nv615Cpu(pitchVa, (uint64_t)pitch * (height - 1) + widthBytes) &&
                                   nv615Cpu(blVa, nv615BlBytes(&b615))))) {   // 0.9.30; 0.9.45 CE when VRAM
        if (!nvGrFlush()) return false;
        const NV615Bl b = b615;   // bh bits 8+: GOB variant (the CE takes it from the PTE kind)
        if (!nv615BlCopy(toBL, pitchVa, pitch, blVa, &b, 0, 0, 0, widthBytes, height)) { nvNoteGpuFailure(1); return false; }
        return true;
    }
    uint32_t *pb = (uint32_t *)(gCpu + NVGSP_STAGE_PUSH), w = 0;
    const uint64_t s = toBL ? pitchVa : blVa, d = toBL ? blVa : pitchVa;
    // block-linear side: block size (1 GOB wide, 2^bh high, GOB height 8), width in bytes,
    // height, depth 1, layer 0, origin 0
    const uint32_t blk = ((bh & 15) << 4) | (1u << 12);
    pb[w++] = mthd(NVGSP_CE_SUBCH, toBL ? 0x70c : 0x728, 6);
    pb[w++] = blk; pb[w++] = blWidthBytes; pb[w++] = height; pb[w++] = 1; pb[w++] = 0; pb[w++] = 0;
    pb[w++] = mthd(NVGSP_CE_SUBCH, B5_OFF_IN_UP, 4);
    pb[w++] = (uint32_t)(s >> 32); pb[w++] = (uint32_t)s;
    pb[w++] = (uint32_t)(d >> 32); pb[w++] = (uint32_t)d;
    pb[w++] = mthd(NVGSP_CE_SUBCH, 0x410, 4);                   // PITCH_IN, PITCH_OUT, LINE_LENGTH_IN, LINE_COUNT
    pb[w++] = toBL ? pitch : 0; pb[w++] = toBL ? 0 : pitch; pb[w++] = widthBytes; pb[w++] = height;
    // NON_PIPELINED | FLUSH | MULTI_LINE, the pitch side PITCH, the other BLOCKLINEAR
    const uint32_t launch = 0x2u | 0x4u | 0x200u | (toBL ? 0x80u : 0x100u);
    pb[w++] = mthd(NVGSP_CE_SUBCH, B5_LAUNCH, 1);
    pb[w++] = launch;
    return ceSubmit(w);
}

bool nvCeCopyBLRegion(bool toBL, uint64_t linVa, uint32_t linPitch, uint64_t blBase, uint32_t blRowBytes,
                      uint32_t blRows, uint32_t blDepth, uint32_t ylog, uint32_t zlog,
                      uint32_t xBytes, uint32_t y, uint32_t z, uint32_t widthBytes, uint32_t height) {
    NV_GPU_LOCKED;
    if (!gCpu || !widthBytes || !height) return false;
    const NV615Bl r615 = {blRowBytes, blRows, blDepth ? blDepth : 1, ylog & 15, zlog & 15};
    if (gN615 && (!nv615CeOn() || (nv615Cpu(linVa, (uint64_t)linPitch * (height - 1) + widthBytes) &&
                                   nv615Cpu(blBase, nv615BlBytes(&r615))))) {   // 0.9.30; 0.9.45 CE when VRAM
        if (!nvGrFlush()) return false;
        const NV615Bl b = r615;
        if (!nv615BlCopy(toBL, linVa, linPitch, blBase, &b, xBytes, y, z, widthBytes, height)) { nvNoteGpuFailure(1); return false; }
        return true;
    }
    uint32_t *pb = (uint32_t *)(gCpu + NVGSP_STAGE_PUSH), w = 0;
    const uint64_t s = toBL ? linVa : blBase, d = toBL ? blBase : linVa;
    const uint32_t blk = ((ylog & 15) << 4) | ((zlog & 15) << 8) | (1u << 12);
    pb[w++] = mthd(NVGSP_CE_SUBCH, toBL ? 0x70c : 0x728, 6);
    pb[w++] = blk; pb[w++] = blRowBytes; pb[w++] = blRows; pb[w++] = blDepth; pb[w++] = z;
    pb[w++] = (xBytes & 0xffff) | (y << 16);                     // ORIGIN x (bytes), y
    pb[w++] = mthd(NVGSP_CE_SUBCH, B5_OFF_IN_UP, 4);
    pb[w++] = (uint32_t)(s >> 32); pb[w++] = (uint32_t)s;
    pb[w++] = (uint32_t)(d >> 32); pb[w++] = (uint32_t)d;
    pb[w++] = mthd(NVGSP_CE_SUBCH, 0x410, 4);
    pb[w++] = toBL ? linPitch : 0; pb[w++] = toBL ? 0 : linPitch; pb[w++] = widthBytes; pb[w++] = height;
    pb[w++] = mthd(NVGSP_CE_SUBCH, B5_LAUNCH, 1);
    pb[w++] = 0x2u | 0x4u | 0x200u | (toBL ? 0x80u : 0x100u);
    return ceSubmit(w);
}

bool nvCeCopySurf(const NVCeSurf *src, const NVCeSurf *dst, uint32_t widthBytes, uint32_t height) {
    NV_GPU_LOCKED;
    if (!gCpu || !widthBytes || !height) return false;
    // 0.9.45: a VRAM side (no CPU address) goes to the copy engine
    bool cpuSides = true;
    if (gN615 && nv615CeOn()) {
        const NV615Bl sb = {src->rowBytes, src->rows, src->depth ? src->depth : 1, src->ylog & 15, src->zlog & 15};
        const NV615Bl db = {dst->rowBytes, dst->rows, dst->depth ? dst->depth : 1, dst->ylog & 15, dst->zlog & 15};
        cpuSides = (src->bl ? nv615Cpu(src->va, nv615BlBytes(&sb)) : nv615Cpu(src->va, (uint64_t)src->pitch * (height - 1) + widthBytes)) &&
                   (dst->bl ? nv615Cpu(dst->va, nv615BlBytes(&db)) : nv615Cpu(dst->va, (uint64_t)dst->pitch * (height - 1) + widthBytes));
    }
    if (gN615 && cpuSides) {   // 0.9.27: linear <-> linear rows on the CPU; 0.9.30: block linear sides too
        if (!nvGrFlush()) { nvNoteGpuFailure(1); return false; }
        if (src->bl || dst->bl) {
            const NV615Bl sb = {src->rowBytes, src->rows, src->depth ? src->depth : 1, src->ylog & 15, src->zlog & 15};
            const NV615Bl db = {dst->rowBytes, dst->rows, dst->depth ? dst->depth : 1, dst->ylog & 15, dst->zlog & 15};
            uint8_t *sp = src->bl ? nv615Cpu(src->va, nv615BlBytes(&sb)) : nv615Cpu(src->va, (uint64_t)src->pitch * (height - 1) + widthBytes);
            uint8_t *dp = dst->bl ? nv615Cpu(dst->va, nv615BlBytes(&db)) : nv615Cpu(dst->va, (uint64_t)dst->pitch * (height - 1) + widthBytes);
            if (!sp || !dp) { nvNoteGpuFailure(1); return false; }
            for (uint32_t r = 0; r < height; r++)
                for (uint32_t i = 0; i < widthBytes; i++) {
                    const uint64_t so = src->bl ? nv615BlOffset(&sb, src->xBytes + i, src->y + r, src->z) : (uint64_t)r * src->pitch + i;
                    const uint64_t dO = dst->bl ? nv615BlOffset(&db, dst->xBytes + i, dst->y + r, dst->z) : (uint64_t)r * dst->pitch + i;
                    dp[dO] = sp[so];
                }
            return true;
        }
        for (uint32_t r = 0; r < height; r++) {
            uint8_t *d = nv615Cpu(dst->va + (uint64_t)r * dst->pitch, widthBytes);
            uint8_t *s = nv615Cpu(src->va + (uint64_t)r * src->pitch, widthBytes);
            if (!d || !s) { nvNoteGpuFailure(1); return false; }
            memmove(d, s, widthBytes);
        }
        return true;
    }
    uint32_t *pb = (uint32_t *)(gCpu + NVGSP_STAGE_PUSH), w = 0;
    const NVCeSurf *side[2] = {dst, src};
    for (int k = 0; k < 2; ++k) {                                 // SET_DST_* (0x70c), SET_SRC_* (0x728)
        const NVCeSurf *f = side[k];
        if (!f->bl) continue;
        pb[w++] = mthd(NVGSP_CE_SUBCH, k ? 0x728 : 0x70c, 6);
        pb[w++] = ((f->ylog & 15) << 4) | ((f->zlog & 15) << 8) | (1u << 12);
        pb[w++] = f->rowBytes; pb[w++] = f->rows; pb[w++] = f->depth; pb[w++] = f->z;
        pb[w++] = (f->xBytes & 0xffff) | (f->y << 16);
    }
    pb[w++] = mthd(NVGSP_CE_SUBCH, B5_OFF_IN_UP, 4);
    pb[w++] = (uint32_t)(src->va >> 32); pb[w++] = (uint32_t)src->va;
    pb[w++] = (uint32_t)(dst->va >> 32); pb[w++] = (uint32_t)dst->va;
    pb[w++] = mthd(NVGSP_CE_SUBCH, 0x410, 4);
    pb[w++] = src->bl ? 0 : src->pitch; pb[w++] = dst->bl ? 0 : dst->pitch; pb[w++] = widthBytes; pb[w++] = height;
    pb[w++] = mthd(NVGSP_CE_SUBCH, B5_LAUNCH, 1);
    // pipelined, multi-line, flush; bit 7 source pitch, bit 8 destination pitch
    pb[w++] = 0x2u | 0x4u | 0x200u | (src->bl ? 0 : 0x80u) | (dst->bl ? 0 : 0x100u);
    return ceSubmit(w);
}

// --- GR compute (QMD v03_00 + ADA_COMPUTE_A, ported from nvrun) ---
static void qset(uint32_t *q, unsigned hi, unsigned lo, uint64_t v) {
    for (unsigned b = lo; b <= hi; ++b) {
        uint32_t m = 1u << (b % 32);
        if ((v >> (b - lo)) & 1) q[b / 32] |= m; else q[b / 32] &= ~m;
    }
}

// 28 Sep: shader local memory (scratch: private arrays, spills). Without a
// backing area STL/LDL go nowhere, which is why k_tex_kinds read the same
// cube direction for every lane. Sized the way NVK does it: per lane ->
// per warp (0x200) -> 48 warps x 2 SMs per TPC (0x8000) -> TPCs (0x20000).
// Grows only; launches are synchronous so the old area can go at once.
#define NVSLM_TPCS 40      // AD103 full die; the 4080 enables 38
static uint64_t gSlmVa, gSlmPerWarp, gSlmPerTpc, gSlmSize;
static uint32_t gSlmHandle;

static bool slmEnsure(uint32_t perLane) {
    if (!perLane) return true;
    uint64_t warp = ((uint64_t)((perLane + 15) & ~15u) * 32 + 0x1ff) & ~0x1ffULL;
    if (warp <= gSlmPerWarp) return true;
    nvGrFlush();                                    // queued launches still use the old area
    nvNativeDrain();                                // 0.8.7: and command buffers still out with it
    if (warp < 0x2000 && !gN615) warp = 0x2000;     // 256 B per lane at least, fewer regrows
    const uint64_t tpc = (warp * 48 * 2 + 0x7fff) & ~0x7fffULL;
    const uint64_t size = (tpc * NVSLM_TPCS + 0x1ffff) & ~0x1ffffULL;
    uint64_t va = 0; uint32_t h = 0;
    // 0.9.90: native615 scratch in VRAM. In sysmem every spill/private-array access crossed PCIe:
    // MPS cnnConvArray (24 b128 local stores/loads) ran at 0.18 TFLOPS.
    static bool slmVram;
    if (gN615 && nv615VramOn() && nv615CeOn() && nvVramAllocRaw(size, &va)) {
        if (slmVram) nvVramFree(gSlmVa, gSlmSize);
        slmVram = true;
        gSlmVa = va; gSlmHandle = 0; gSlmPerWarp = warp; gSlmPerTpc = tpc; gSlmSize = size;
        return true;
    }
    if (gN615) {   // 0.9.28: scratch is pinned sysmem from the native615 heap (exact size, at most one slab)
        void *cpu = NULL;
        bool ok = false;
        if (size <= NV615_BIG_MAX) ok = heapAllocFlags(size, &cpu, &va, 0);
        else if (size <= 2 * NV615_BIG_MAX) {
            // 0.9.39: SceneKit's 624 B per lane needs 74 MiB, past one kernel
            // allocation. Two halves over a slab each get fresh slabs, which the
            // kernel bump-allocates next to each other; use them only if they are.
            const uint64_t half = ((size / 2) + 0xffff) & ~0xffffULL;
            void *cpu2 = NULL;
            uint64_t va2 = 0;
            if (half > NV615_HEAP_SLAB && heapAllocFlags(half, &cpu, &va, 0)) {
                if (heapAllocFlags(size - half, &cpu2, &va2, 0) && va2 == va + half) ok = true;
                else {
                    if (cpu2) nvHeapFree(cpu2, size - half);
                    nvHeapFree(cpu, half);
                    NSLog(@"NVMTLGsp: native615 scratch halves not adjacent (0x%llx, 0x%llx)", va, va2);
                }
            }
        }
        if (!ok) {
            NSLog(@"NVMTLGsp: native615 scratch area of %llu bytes failed", size);
            return false;
        }
        gSlmVa = va; gSlmHandle = 0; gSlmPerWarp = warp; gSlmPerTpc = tpc; gSlmSize = size;
        return true;
    }
    if (!nvVramAllocKind(size, 0, &va, &h)) {
        NSLog(@"NVMTLGsp: scratch area of %llu bytes failed", size);
        return false;
    }
    nvVramFreeKind(gSlmHandle);
    gSlmVa = va; gSlmHandle = h; gSlmPerWarp = warp; gSlmPerTpc = tpc; gSlmSize = size;
    return true;
}

static void buildQmd(uint32_t *q, uint64_t prog, uint64_t cb0, uint32_t cb0b,
                     const uint32_t *grid, const uint32_t *block, uint32_t regs) {
    memset(q, 0, 256);
    qset(q, 583, 580, 3);  qset(q, 579, 576, 0);          /* QMD v03_00 */
    qset(q, 378, 378, 1);                                   /* API_VISIBLE_CALL_LIMIT NO_CHECK */
    qset(q, 134, 134, 1);                                   /* SM_GLOBAL_CACHING_ENABLE */
    qset(q, 415, 384, grid[0]); qset(q, 431, 416, grid[1]); qset(q, 463, 448, grid[2]);
    qset(q, 607, 592, block[0]); qset(q, 623, 608, block[1]); qset(q, 639, 624, block[2]);
    qset(q, 1567, 1536, prog & 0xffffffff); qset(q, 1584, 1568, prog >> 32);
    qset(q, 656, 648, regs);
    qset(q, 567, 562, 1); qset(q, 662, 657, 1); qset(q, 574, 569, 26);  /* smem cfg */
    qset(q, 640, 640, 1);                                   /* CONSTANT_BUFFER_VALID(0) */
    qset(q, 1055, 1024, cb0 & 0xffffffff); qset(q, 1072, 1056, cb0 >> 32);
    qset(q, 1087, 1075, ((cb0b + 15) & ~15u) >> 4);
}

// Shared-memory config (nvrun's block, runtime CTA dims) + scratch size.
static void qmdShared(uint32_t *qs, uint32_t smem, const uint32_t *block, uint32_t barriers, uint32_t slm) {
    {
        static const uint32_t kb[6] = {0, 8, 16, 32, 64, 100};
        uint32_t sm = (smem + 255) & ~255u;
        // 0.9.89: MetalFX unified_pass_tile reads 224 B past its threadgroup memory (RC 13 at 19424 of
        // 19200; a full 1280x720 grid of those wedged GSP). Apple GPUs do not trap: give each CTA 2 KiB slack.
        const uint32_t kMaxCta = 99u * 1024;
        if (sm && sm < kMaxCta) sm = sm + 2048 < kMaxCta ? sm + 2048 : kMaxCta;
        uint32_t threads = block[0] * block[1] * block[2];
        uint32_t warps = (threads + 31) / 32, wgs = 48 / (warps ? warps : 1);
        uint32_t fit1 = 100, fitn = 100;
        if (sm && wgs > 100u * 1024 / sm) wgs = 100u * 1024 / sm;
        for (int i = 5; i >= 0; --i) {
            if (kb[i] * 1024 >= sm) fit1 = kb[i];
            if (kb[i] * 1024 >= sm * wgs) fitn = kb[i];
        }
        qset(qs, 561, 544, sm);
        qset(qs, 567, 562, fit1 / 4 + 1);
        qset(qs, 662, 657, fitn / 4 + 1);
        qset(qs, 574, 569, 100 / 4 + 1);
        qset(qs, 767, 763, barriers);
    }
    qset(qs, 759, 736, (slm + 15) & ~15u);                  /* SHADER_LOCAL_MEMORY_LOW_SIZE */
}

// 0.8.9 / 0.8.12: what a stage's constant buffer pointers point at. A
// launch that never ends or faults reads through one of these; print the
// first words of every user-arena pointer that lands in a live buffer, and
// say which do not (a freed or never-bound VA is the MMU fault).
static char gLastLabel[128];                     // 0.8.12: the launch now being built
static char gLastTex[480];
void nvGrNoteTextures(const char *what) { snprintf(gLastTex, sizeof gLastTex, "%s", what); }
static const char *nvVramState(uint64_t p);
static void nvDumpPointers(const char *stage, const uint32_t *push, uint32_t pushWords) {
    NSMutableString *raw = [NSMutableString new];
    for (uint32_t i = 0; push && i < pushWords && i < 96; i++) [raw appendFormat:@" %x", push[i]];
    NSLog(@"NVMTLGsp:   %s push:%@", stage, raw);
    for (uint32_t i = 0; push && i + 1 < pushWords && i < 96; i += 2) {
        const uint64_t p = (uint64_t)push[i] | (uint64_t)push[i + 1] << 32;
        if (p >= 0x2880000000ULL && p < NVGSP_USER_VA_BASE) {       // VRAM / block-linear
            NSLog(@"NVMTLGsp:   %s cb0[%u] 0x%llx: %s", stage, i * 4, p, nvVramState(p));
            continue;
        }
        if (p < NVGSP_USER_VA_BASE || p >= NVGSP_USER_VA_END) continue;
        const uint32_t *w = NULL;
        uint64_t left = 0;
        os_unfair_lock_lock(&gHeapLock);
        for (unsigned b = 0; b < gNBufs; b++)
            if (p >= gBufs[b].va && p < gBufs[b].va + gBufs[b].size && gBufs[b].cpu) {
                w = (const uint32_t *)(gBufs[b].cpu + (p - gBufs[b].va));
                left = gBufs[b].va + gBufs[b].size - p;
                break;
            }
        os_unfair_lock_unlock(&gHeapLock);
        if (!w) { NSLog(@"NVMTLGsp:   %s cb0[%u] 0x%llx: NOT a live buffer", stage, i * 4, p); continue; }
        const uint32_t n = left >= 64 ? 16 : (uint32_t)(left / 4);
        NSMutableString *h = [NSMutableString new];
        for (uint32_t j = 0; j < n; j++) [h appendFormat:@" %08x", w[j]];
        NSLog(@"NVMTLGsp:   %s cb0[%u] 0x%llx (0x%llx bytes left):%@", stage, i * 4, p, left, h);
    }
}

static bool grQueue(bool force, const uint32_t *code, uint32_t codeWords, uint32_t regs, uint32_t slm,
                    uint32_t smem, uint32_t barriers, const uint32_t *push, uint32_t npush,
                    const uint32_t grid[3], const uint32_t block[3]);
// 0.8.14: why the last nvGrLaunch / nvGrQueue said no (for the caller's log)
const char *nvGrWhy = "";

bool nvGrLaunch(const uint32_t *code, uint32_t codeWords, uint32_t regs, uint32_t slm,
                uint32_t smem, uint32_t barriers,
                const uint32_t *push, uint32_t npush,
                const uint32_t grid[3], const uint32_t block[3]) {
    NV_GPU_LOCKED;
    nvGrFlush();
    if (!gCpu) { nvGrWhy = "no GPU mapping"; return false; }
    if (!codeWords) { nvGrWhy = "no code"; return false; }
    // 0.8.14: kernels over the 63 KiB code slot (Vision/Espresso
    // cnnConvArray_32x32_128 is 114 KiB) go through the batch area as a
    // one-launch batch, flushed (and waited on) here
    if (codeWords * 4 + 1024 > 0x10000)
        return grQueue(true, code, codeWords, regs, slm, smem, barriers, push, npush, grid, block) && nvGrFlush();
    if (!grid[0] || !block[0]) { nvGrWhy = "empty grid"; return false; }
    if (!slmEnsure(slm)) { nvGrWhy = "scratch (slm) allocation"; return false; }
    // Code (+1 KiB zero pad, as nvrun), cbuf, QMD.
    uint8_t *cd = gCpu + NVGSP_GR_CODE;
    memcpy(cd, code, codeWords * 4);
    memset(cd + codeWords * 4, 0, 1024);
    uint32_t cb[256] = {0};
    if (npush > 256) { nvGrWhy = "push block over 1 KiB"; return false; }
    memcpy(cb, push, npush * 4);
    uint32_t cbw = (npush + 3) & ~3u;
    if (cbw < 4) cbw = 4;
    memcpy(gCpu + NVGSP_GR_CB0, cb, cbw * 4);
    uint32_t qs[64];
    const uint64_t codeVa = gVa + NVGSP_GR_CODE, cbVa = gVa + NVGSP_GR_CB0;
    buildQmd(qs, codeVa, cbVa, cbw * 4, grid, block, regs);
    qmdShared(qs, smem, block, barriers, slm);
    const uint64_t qmdVa = gVa + NVGSP_GR_QMD;
    memcpy(gCpu + NVGSP_GR_QMD, qs, 256);
    // Method stream on subch 1 (compute); the kext tail owns subch 0.
    uint32_t *pb = (uint32_t *)(gCpu + NVGSP_GR_PUSH), c = 0;
    pb[c++] = mthd(1, 0x0, 1);   pb[c++] = 0xC9C0;
    pb[c++] = mthd(1, 0x2A0, 2); pb[c++] = 0; pb[c++] = 0xFE000000;
    pb[c++] = mthd(1, 0x7B0, 2); pb[c++] = 0; pb[c++] = 0xFF000000;
    pb[c++] = mthd(1, 0x21C, 1); pb[c++] = 0x1011;
    if (gSlmVa) {   // SET_SHADER_LOCAL_MEMORY_A/B, NON_THROTTLED_A/B/C
        pb[c++] = mthd(1, 0x790, 2); pb[c++] = (uint32_t)(gSlmVa >> 32); pb[c++] = (uint32_t)gSlmVa;
        pb[c++] = mthd(1, 0x2E4, 3);
        pb[c++] = (uint32_t)(gSlmPerTpc >> 32); pb[c++] = (uint32_t)gSlmPerTpc; pb[c++] = 0xff;
    }
    {   // 28 Sep: texture header / sampler pools (hardware texturing); the
        // caches are dropped every launch since descriptors and texture data
        // change between dispatches
        uint64_t ticVa, tscVa;
        uint32_t ticMax, tscMax;
        if (nvTexPool(&ticVa, &ticMax, &tscVa, &tscMax)) {
            pb[c++] = mthd(1, 0x1574, 3);
            pb[c++] = (uint32_t)(ticVa >> 32); pb[c++] = (uint32_t)ticVa; pb[c++] = ticMax;
            pb[c++] = mthd(1, 0x155C, 3);
            pb[c++] = (uint32_t)(tscVa >> 32); pb[c++] = (uint32_t)tscVa; pb[c++] = tscMax;
            pb[c++] = (0x4u << 29) | (1u << 13) | (0x244u >> 2);    // IMMD INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI
            pb[c++] = (0x4u << 29) | (1u << 13) | (0x1424u >> 2);   // IMMD INVALIDATE_SAMPLER_CACHE_NO_WFI
            pb[c++] = (0x4u << 29) | (1u << 13) | (0x1288u >> 2);   // IMMD INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI
        }
    }
    pb[c++] = (0x4u << 29) | (1u << 13) | (0x298u >> 2); // IMMD(0x298,0)
    pb[c++] = mthd(1, 0x2B4, 1); pb[c++] = (uint32_t)(qmdVa >> 8);
    pb[c++] = (0x4u << 29) | (3u << 16) | (1u << 13) | (0x2C0u >> 2); // IMMD(0x2C0,3)
    struct { uint64_t va; uint32_t dw; uint32_t pad; } seg = {gVa + NVGSP_GR_PUSH, c, 0};
    if (gN615) {   // 0.9.26: synchronous, kernel fence
        if (!nv615Submit(seg.va, seg.dw)) { nvNoteGpuFailure(2); return false; }
        return true;
    }
    uint64_t in[1] = {0}; // engine 0 = GR
    uint64_t out[1] = {0};
    uint32_t outN = 1;
    const uint64_t ta = mach_absolute_time();
    if (!callMethod(kSelExecSeg, in, 1, out, &outN, &seg, sizeof(seg)) || !out[0]) {
        NSLog(@"NVMTLGsp: GR execSegments failed"); nvNoteGpuFailure(1); return false;
    }
    const uint64_t tb0 = mach_absolute_time();
    uint64_t wi[2] = {out[0], nvFenceUs()}, wo[1] = {0};
    outN = 1;
    if (IOConnectCallMethod(gConn, 18, wi, 2, NULL, 0, wo, &outN, NULL, NULL)) {
        NSLog(@"NVMTLGsp: GR fenceWait failed"); nvNoteGpuFailure(2);
        static uint32_t dumps;
        if (dumps++ < 4) {   // 0.8.12: name the launch that never finished
            NSLog(@"NVMTLGsp: faulting launch %s: grid %ux%ux%u block %ux%ux%u, %u regs, slm %u, smem %u, %u push words",
                  gLastLabel, grid[0], grid[1], grid[2], block[0], block[1], block[2], regs, slm, smem, npush);
            NSLog(@"NVMTLGsp:   cs textures:%s", gLastTex);
            nvDumpPointers("cs", push, npush);
        }
        return false;
    }
    if (getenv("NVMTL_TRACE")) {
        mach_timebase_info_data_t tb; mach_timebase_info(&tb);
        const uint64_t tc = mach_absolute_time();
        NSLog(@"NVMTL_TRACE gr: submit %.1f us, wait %.1f us",
              (tb0 - ta) * tb.numer / tb.denom / 1e3, (tc - tb0) * tb.numer / tb.denom / 1e3);
    }
    return true;
}

// 0.6.13: batched compute. Every nvGrLaunch is a kernel round trip
// (execSegments + fence wait, ~30 us each, 13 us of it the submit alone),
// so a command buffer of 1000 small dispatches took 30 ms. Between
// nvGrBatchBegin/End, nvGrQueue appends the launch to one method stream in
// a 4 MiB part of the staging object (code once per kernel, cbuf + QMD per launch)
// and nvGrFlush submits the lot with one fence. Metal's serial dispatch
// order is kept with WAIT_FOR_IDLE + shader cache invalidates between
// launches. Anything that submits on its own, or reads memory on the CPU,
// flushes first.
// 0.8.0: the batch goes to one of two places. The shared area at 2 MiB
// (4 MiB, 512 KiB of methods) for batches we submit and wait for; or a
// native slab at 6 MiB + 2 MiB * n (256 KiB of methods) that belongs to a
// command buffer until the family completes it (N2, asynchronous commit).
#define NVB_SLABS 8
#define NVB_SLAB_BYTES (2u << 20)
static uint32_t NVB_BYTES = 4u << 20, NVB_PUSH_MAX = 512u << 10;
static int gBSlab = -1;                            // slab the open batch lives in, -1 shared area
static bool nvNativeDrainShared;                    // the shared area is out with a command buffer
static uint32_t gSlabBusy;                          // bit n: slab n owned by a command buffer
static uint64_t gSlabLease[NVB_SLABS + 1];          // protected by gSlabMtx
static pthread_mutex_t gSlabMtx = PTHREAD_MUTEX_INITIALIZER;   // never the GPU lock: completion
static pthread_cond_t gSlabCond = PTHREAD_COND_INITIALIZER;    // handlers take only this one
// 0.8.7: what each command buffer that owns a slab carries, so a stuck one
// can be named: launch labels (kernel / draw shaders), stream VA and size
static char gSlabLabels[NVB_SLABS + 1][512];
static uint64_t gSlabVa[NVB_SLABS + 1];
static uint32_t gSlabWords[NVB_SLABS + 1];
static char gBLabels[512];                       // the open batch
// 0.8.27: the first submissions after a GPU reset recovery, by label, to
// find what faults right after it (1 Oct: WindowServer at VA 0, every reset)
static void nvPostResetNote(const char *path, uint32_t n, uint32_t words, uint64_t va) {
    static int fromFile = -1;                     // /Library/Preferences/nvmtl-submitlog: the first 200 too
    if (fromFile < 0) {
        fromFile = access("/Library/Preferences/nvmtl-submitlog", F_OK) == 0;
        if (fromFile) gPostResetLog += 200;
    }
    if (!gPostResetLog) return;
    gPostResetLog--;
    NSLog(@"NVMTLGsp: after reset, %s submit: %u launches, %u words at 0x%llx: %s", path, n, words,
          (unsigned long long)va, gBLabels[0] ? gBLabels : "(no label)");
}
void nvGrLabel(const char *what) {
    snprintf(gLastLabel, sizeof gLastLabel, "%s", what);
    const size_t n = strlen(gBLabels);
    if (n + strlen(what) + 2 < sizeof gBLabels) { if (n) strcat(gBLabels, " "); strcat(gBLabels, what); }
}
static void nvSlabsReleaseAll(void) {
    pthread_mutex_lock(&gSlabMtx);
    for (int i = 0; i <= NVB_SLABS; i++) gSlabLease[i]++;
    gSlabBusy = 0;
    pthread_cond_broadcast(&gSlabCond);
    pthread_mutex_unlock(&gSlabMtx);
}
// It lives in the staging object (at 2 MiB): the host fetches method
// streams fine from kernel SYS chunks, but a pushbuffer in heap pages (4 KiB
// user pages) was an invalid-pushbuffer exception (rc-except 32, live 28 Sep).
#define gBOff (gBSlab < 0 ? (2u << 20) : (6u << 20) + (uint32_t)gBSlab * NVB_SLAB_BYTES)
#define gBCpu (gCpu + gBOff)
#define gBVa (gVa + gBOff)
static uint32_t gBPush, gBData, gBN, gBDepth;
static struct { const void *code; uint32_t words; uint64_t va; } gBCode[32];
static uint32_t gBNCode;
static bool gBComputeSeen;   // 0.8.69: compute class + windows set in this batch

void nvGrBatchBegin(void) { nvGpuLock(); gBDepth++; }

// 0.8.1: the native path (N1/N2) is on by default when the accelerator
// runs it (NVAccelerator-native in the registry). Off with NVMTL_NATIVE=0,
// or for every process (WindowServer too) with the file
// /Library/Preferences/nvmtl-native-off.
bool nvNativeEnabled(void) {
    static int on = -1;
    if (on >= 0) return on;
    if (nvN615Requested()) { on = 0; return on; }   // 0.9.26: no family slabs on native615
    const char *e = getenv("NVMTL_NATIVE");
    if (e) { on = atoi(e) != 0; return on; }
    if (access("/Library/Preferences/nvmtl-native-off", F_OK) == 0) { on = 0; return on; }
    on = 0;
    io_service_t acc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVAccelerator"));
    if (acc) {
        CFTypeRef v = IORegistryEntryCreateCFProperty(acc, CFSTR("NVAccelerator-native"), kCFAllocatorDefault, 0);
        on = v && CFGetTypeID(v) == CFBooleanGetTypeID() && CFBooleanGetValue((CFBooleanRef)v);
        if (v) CFRelease(v);
        IOObjectRelease(acc);
    }
    NSLog(@"NVMTLGsp: native command submission %s", on ? "on" : "off");
    return on;
}

// 0.8.0 (native N2): wait until every command buffer that owns a slab has
// completed. Anything that reaches the GPU another way (copy engine) or
// reads memory on the CPU calls this first; GR work is ordered by its ring.
void nvNativeDrain(void) {
    pthread_mutex_lock(&gSlabMtx);
    for (int tries = 0; gSlabBusy && tries < 20; tries++) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&gSlabCond, &gSlabMtx, &ts);
    }
    if (gSlabBusy) {
        static uint32_t nlog;
        NSLog(@"NVMTLGsp: native command buffers still out after 2 s (slabs 0x%x)", gSlabBusy);
        for (int i = 0; nlog < 16 && i <= NVB_SLABS; i++)
            if (gSlabBusy & (i == NVB_SLABS ? 1u << 31 : 1u << i)) {
                ++nlog;
                NSLog(@"NVMTLGsp:   slab %d at 0x%llx, %u words: %s", i, gSlabVa[i], gSlabWords[i], gSlabLabels[i]);
            }
    }
    pthread_mutex_unlock(&gSlabMtx);
}
void nvGrSync(void) { nvGrFlush(); nvNativeDrain(); }
// 0.6.16: batching was opt-in (a mediaanalysisd GPU reset at login, 28 Sep).
// 0.8.21: on by default: a command buffer's dispatches go out as one
// submission instead of a submit + fence wait each (25 -> 7 us a dispatch,
// metal_overhead_bench); suite and Core Image match with it. NVMTL_BATCH=0
// turns it off. Draws still batch only with NVMTL_DRAWBATCH=1 (below).
bool nvBatchOn(void) {
    static int on = -1;
    if (on < 0) {
        // 0.8.84: isolation switch. Native mode normally forces batching on;
        // NVMTL_NO_BATCH=1 submits each launch through the one-shot path so a
        // hanging kernel can be named without disabling the whole pipeline.
        const char *nb = getenv("NVMTL_NO_BATCH");
        if (nb && atoi(nb)) { on = 0; return on; }
        const char *e = getenv("NVMTL_BATCH");
        on = e ? atoi(e) != 0 : 1;
        if (nvNativeEnabled()) on = 1;
    }
    return on;
}

// a free slab for this command buffer's batch (waits for one to complete)
void nvGrBatchBeginNative(void) {
    int n = -1;
    pthread_mutex_lock(&gSlabMtx);
    for (int tries = 0; n < 0 && tries < 40; tries++) {
        for (int i = 0; i < NVB_SLABS; i++) if (!(gSlabBusy & (1u << i))) { n = i; break; }
        if (n >= 0) break;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&gSlabCond, &gSlabMtx, &ts);
    }
    pthread_mutex_unlock(&gSlabMtx);
    nvGpuLock(); gBDepth++;
    if (n >= 0 && !gBN) {   // (the shared area when every slab stays busy)
        gBSlab = n; NVB_BYTES = NVB_SLAB_BYTES; NVB_PUSH_MAX = 256u << 10;
    }
}
static void batchToSharedArea(void) { gBSlab = -1; NVB_BYTES = 4u << 20; NVB_PUSH_MAX = 512u << 10; }
// the batch went to the family in the command buffer: nothing to wait for
void nvGrBatchEndNative(void) {
    if (gBDepth) gBDepth--;
    nvGpuUnlock();
}
// 0.7.0: end a batch whose launches went to the family (nvGrFlushNative);
// the GPU lock stays held until nvGrNativeDone + nvGpuUnlock
void nvGrBatchEnd(void) {
    if (gBDepth && !--gBDepth) { nvGrFlush(); if (gBSlab >= 0) batchToSharedArea(); }
    nvGpuUnlock();
}

// 0.7.0 (native N1 test): hand the queued launches to the family instead of
// submitting them ourselves. One vendor kernel command (type 0x10000, as
// AppleParavirtCommandQueue's ExecIndirect) goes into the command buffer's
// segment; NVAccelerator's queue submits it with a stamp and the family
// completes the command buffer when the GPU releases that stamp. Returns
// false (nothing written) when there is nothing queued or no room, and the
// caller flushes the old way.
// 0.8.2: + the tag of our NVGP client (selector 'NVT'), so the kernel runs
// it in this connection's VA arena even when the process has several
typedef struct { uint32_t type, size, engine, n, tag, reserved; uint64_t va; uint32_t dwords, flags; } NVExecKCmd;
static uint32_t connTag(void) {
    uint64_t out[1] = {0};
    uint32_t n = 1;
    // Do not reopen here: commands have already been encoded in the old
    // staging mapping. Fail this buffer; the next commit's preflight reopens.
    const kern_return_t kr = callOn(gConn, 0x4E5654, NULL, 0, out, &n, NULL, 0);
    if (kr != KERN_SUCCESS) nvNoteGpuFailure(1);
    return kr == KERN_SUCCESS ? (uint32_t)out[0] : 0;
}
// 0.8.22: batched draws of one render pass run back to back; the ROP flush
// and wait-for-idle come once, when the pass is left (next pass, a compute
// launch, or the end of the batch), not after every draw.
static uint32_t gB3DPass;
static bool gB3DDirty;
static uint32_t nv3DFlushWords(uint32_t *pb, uint32_t c) {
    const uint64_t sva = gVa + NVGSP_GR_SEM;
    static uint32_t seq;
    pb[c++] = 0x20000000u | (4u << 16) | (0u << 13) | (0x1b00u >> 2);   // SEMAPHORE_A..D, subch 0 (3D)
    pb[c++] = (uint32_t)(sva >> 32); pb[c++] = (uint32_t)sva; pb[c++] = ++seq;
    pb[c++] = (1u << 4) | (15u << 12) | (1u << 28);                     // RELEASE after writes, ALL, ONE_WORD (flushes)
    pb[c++] = (0x4u << 29) | (0u << 13) | (0x110u >> 2);                // IMMD WAIT_FOR_IDLE
    gB3DDirty = false;
    return c;
}

bool nvGrFlushNative(id cb) {
    NV_GPU_LOCKED;
    if (!gBN) return false;
    if (gB3DDirty) gBPush = nv3DFlushWords((uint32_t *)gBCpu, gBPush);   // 0.8.22
    // The family only runs kernel commands inside a segment
    // (IOAccelCommandQueue::processCommandBuffer walks the segment list):
    // MTLIOAccelCommandBufferStorageBeginSegment takes the kernel command
    // pointer the segment starts at, EndSegment closes it at the current one.
    const uint32_t tag = connTag();
    if (!tag) {
        gBPush = gBData = gBN = gBNCode = 0; gBComputeSeen = false;
        gBLabels[0] = 0;
        return false;  // no fallback replay of old-generation staging
    }
    SEL sel = NSSelectorFromString(@"_reserveKernelCommandBufferSpace:");
    SEL get = NSSelectorFromString(@"getCurrentKernelCommandBufferPointer:end:");
    SEL beg = NSSelectorFromString(@"beginSegment:"), end = NSSelectorFromString(@"endCurrentSegment");
    if (![cb respondsToSelector:sel] || ![cb respondsToSelector:get] || ![cb respondsToSelector:beg] ||
        ![cb respondsToSelector:end])
        return false;
    void *cur = NULL, *lim = NULL;
    ((void (*)(id, SEL, void **, void **))objc_msgSend)(cb, get, &cur, &lim);
    if (!cur) return false;
    ((void (*)(id, SEL, void *))objc_msgSend)(cb, beg, cur);
    NVExecKCmd *k = ((void *(*)(id, SEL, unsigned long))objc_msgSend)(cb, sel, sizeof(NVExecKCmd));
    if (k) {
        k->type = 0x10000; k->size = sizeof(NVExecKCmd); k->engine = 0; k->n = 1;
        k->tag = tag; k->reserved = 0;
        k->va = gBVa; k->dwords = gBPush; k->flags = 0;
    }
    ((void (*)(id, SEL))objc_msgSend)(cb, end);
    if (!k) return false;
    static int trace = -1;
    if (trace < 0) trace = getenv("NVMTL_TRACE") != NULL;
    if (trace) NSLog(@"NVMTL_TRACE native: %u launches, %u words in kernel command %p, slab %d", gBN, gBPush, k, gBSlab);
    nvPostResetNote("native", gBN, gBPush, gBVa);
    // 0.8.0: the slab (or, with every slab busy, the shared area) now
    // belongs to this command buffer until the family completes it
    const uint32_t bit = gBSlab >= 0 ? 1u << gBSlab : 1u << 31;
    const int si = gBSlab >= 0 ? gBSlab : NVB_SLABS;
    {
        snprintf(gSlabLabels[si], sizeof gSlabLabels[si], "%s", gBLabels);
        gSlabVa[si] = gBVa; gSlabWords[si] = gBPush;
    }
    pthread_mutex_lock(&gSlabMtx);
    const uint64_t lease = ++gSlabLease[si];
    gSlabBusy |= bit;
    pthread_mutex_unlock(&gSlabMtx);
    [(id<MTLCommandBuffer>)cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
        (void)c;
        pthread_mutex_lock(&gSlabMtx);
        // An old connection's callback can arrive after reset reopened and
        // reused this slab. Only the current owner may make it reusable.
        if (gSlabLease[si] == lease) {
            gSlabBusy &= ~bit;
            pthread_cond_broadcast(&gSlabCond);
        }
        pthread_mutex_unlock(&gSlabMtx);
    }];
    gBPush = gBData = gBN = gBNCode = 0; gBComputeSeen = false;
    gBLabels[0] = 0;
    if (gBSlab >= 0) batchToSharedArea();
    else nvNativeDrainShared = true;
    return true;
}
// after the family completed the command buffer: the batch area is free


bool nvGrFlush(void) {
    if (!gBN) return true;
    NV_GPU_LOCKED;
    if (gB3DDirty) gBPush = nv3DFlushWords((uint32_t *)gBCpu, gBPush);
    const uint32_t words = gBPush, n = gBN;
    char batchLabels[512];
    snprintf(batchLabels, sizeof batchLabels, "%s", gBLabels[0] ? gBLabels : "(no label)");
    nvPostResetNote("batch", n, words, gBVa);
    gBPush = gBData = gBN = gBNCode = 0; gBComputeSeen = false;
    gBLabels[0] = 0;          // the area is free again whatever happens
    struct { uint64_t va; uint32_t dw; uint32_t pad; } seg = {gBVa, words, 0};
    if (gN615) {   // 0.9.26
        if (!nv615Submit(seg.va, words)) {
            NSLog(@"NVMTLGsp: native615 batch of %u launches failed: %s", n, batchLabels);
            nvNoteGpuFailure(2); return false;
        }
        return true;
    }
    uint64_t in[1] = {0}, out[1] = {0};
    uint32_t outN = 1;
    const uint64_t ta = mach_absolute_time();
    if (getenv("NVMTL_TRACE"))
        NSLog(@"NVMTL_TRACE gr batch labels: %s", batchLabels);
    if (!callMethod(kSelExecSeg, in, 1, out, &outN, &seg, sizeof(seg)) || !out[0]) {
        NSLog(@"NVMTLGsp: GR batch of %u launches: execSegments failed: %s", n,
              batchLabels); nvNoteGpuFailure(1); return false;
    }
    const uint64_t tb0 = mach_absolute_time();
    uint64_t wi[2] = {out[0], nvFenceUs()}, wo[1] = {0};
    outN = 1;
    if (IOConnectCallMethod(gConn, 18, wi, 2, NULL, 0, wo, &outN, NULL, NULL)) {
        NSLog(@"NVMTLGsp: GR batch of %u launches: fenceWait failed: %s", n,
              batchLabels); nvNoteGpuFailure(2); return false;
    }
    static int trace = -1;
    if (trace < 0) trace = getenv("NVMTL_TRACE") != NULL;
    if (trace) {
        mach_timebase_info_data_t tb; mach_timebase_info(&tb);
        const uint64_t tc = mach_absolute_time();
        NSLog(@"NVMTL_TRACE gr batch: %u launches %u words, submit %.1f us, wait %.1f us", n, words,
              (tb0 - ta) * tb.numer / tb.denom / 1e3, (tc - tb0) * tb.numer / tb.denom / 1e3);
    }
    return true;
}

static uint64_t batchPut(const void *src, uint32_t bytes, uint32_t pad) {
    const uint32_t off = NVB_PUSH_MAX + gBData;
    memcpy(gBCpu + off, src, bytes);
    if (pad) memset(gBCpu + off + bytes, 0, pad);
    gBData += (bytes + pad + 255) & ~255u;
    return gBVa + off;
}

bool nvGrQueue(const uint32_t *code, uint32_t codeWords, uint32_t regs, uint32_t slm,
               uint32_t smem, uint32_t barriers,
               const uint32_t *push, uint32_t npush,
               const uint32_t grid[3], const uint32_t block[3]) {
    return grQueue(false, code, codeWords, regs, slm, smem, barriers, push, npush, grid, block);
}

// force: queue even with batching off (the caller flushes)
static bool grQueue(bool force, const uint32_t *code, uint32_t codeWords, uint32_t regs, uint32_t slm,
                    uint32_t smem, uint32_t barriers,
                    const uint32_t *push, uint32_t npush,
                    const uint32_t grid[3], const uint32_t block[3]) {
    NV_GPU_LOCKED;
    // 0.9.37: never launch a threadgroup the SM cannot hold; it hangs the channel
    if (block[0] && block[1] && block[2]) {
        const uint64_t threads = (uint64_t)block[0] * block[1] * block[2];
        const uint64_t warps = (threads + 31) / 32, r = (regs + 7) & ~7u;
        if (threads > 1024 || warps * 32 * r > 65536) {
            nvGrWhy = "threadgroup exceeds the SM register file";
            NSLog(@"NVMTLGsp: refused launch %ux%ux%u with %u registers", block[0], block[1], block[2], regs);
            return false;
        }
    }
    const bool on = nvBatchOn() || force;
    if (gBSlab < 0 && nvNativeDrainShared && !gBN) {   // 0.8.0: shared area still owned by a command buffer
        nvNativeDrain(); nvNativeDrainShared = false;
    }
    if (!force && (!gBDepth || !on))
        return nvGrLaunch(code, codeWords, regs, slm, smem, barriers, push, npush, grid, block);
    if (!gCpu) { nvGrWhy = "no GPU mapping"; return false; }
    // code goes in the batch data area (after the push area), not the 64 KiB slot
    if (!codeWords || codeWords * 4 + 1024 + 256 + 8192 > NVB_BYTES - NVB_PUSH_MAX) {
        nvGrWhy = "code larger than the batch area"; return false;
    }
    if (npush > 256) { nvGrWhy = "push block over 1 KiB"; return false; }
    if (!grid[0] || !block[0]) { nvGrWhy = "empty grid"; return false; }
    if (!slmEnsure(slm)) { nvGrWhy = "scratch (slm) allocation"; return false; }   // may flush
    int ci = -1;
    for (uint32_t i = 0; i < gBNCode; i++)
        if (gBCode[i].code == code && gBCode[i].words == codeWords) { ci = (int)i; break; }
    const uint32_t need = (ci < 0 ? codeWords * 4 + 1024 + 256 : 0) + 1024 + 512;
    if (gBN && (gBPush + 64 > NVB_PUSH_MAX / 4 || NVB_PUSH_MAX + gBData + need > NVB_BYTES ||
                (ci < 0 && gBNCode == 32)))
        nvGrFlush();
    if (!gBN) ci = -1;
    uint32_t *pb = (uint32_t *)gBCpu, c = gBPush;
    // Class and windows once per batch. Scratch and texture pools every
    // launch, as the one-shot path does: the pool is made on first texture
    // use, which can be between two launches of one batch (mediaanalysisd,
    // 28 Sep: pool bound at 0 for the second launch -> MMU fault at VA 0).
    if (!gBN) {
        pb[c++] = mthd(1, 0x0, 1);   pb[c++] = 0xC9C0;
        pb[c++] = mthd(1, 0x2A0, 2); pb[c++] = 0; pb[c++] = 0xFE000000;
        pb[c++] = mthd(1, 0x7B0, 2); pb[c++] = 0; pb[c++] = 0xFF000000;
    } else {
        if (gB3DDirty) c = nv3DFlushWords(pb, c);              // 0.8.22: a pass's draws before this launch
        pb[c++] = (0x4u << 29) | (1u << 13) | (0x110u >> 2);   // IMMD WAIT_FOR_IDLE: serial dispatch order
        // 0.8.69: a batch that started with draws never set the compute
        // windows; a fresh GR context has them overlapping (SKEDCHECK17)
        if (!gBComputeSeen) {
            pb[c++] = mthd(1, 0x0, 1);   pb[c++] = 0xC9C0;
            pb[c++] = mthd(1, 0x2A0, 2); pb[c++] = 0; pb[c++] = 0xFE000000;
            pb[c++] = mthd(1, 0x7B0, 2); pb[c++] = 0; pb[c++] = 0xFF000000;
        }
    }
    gBComputeSeen = true;
    if (gSlmVa) {
        pb[c++] = mthd(1, 0x790, 2); pb[c++] = (uint32_t)(gSlmVa >> 32); pb[c++] = (uint32_t)gSlmVa;
        pb[c++] = mthd(1, 0x2E4, 3);
        pb[c++] = (uint32_t)(gSlmPerTpc >> 32); pb[c++] = (uint32_t)gSlmPerTpc; pb[c++] = 0xff;
    }
    {
        uint64_t ticVa, tscVa;
        uint32_t ticMax, tscMax;
        if (nvTexPool(&ticVa, &ticMax, &tscVa, &tscMax)) {
            pb[c++] = mthd(1, 0x1574, 3);
            pb[c++] = (uint32_t)(ticVa >> 32); pb[c++] = (uint32_t)ticVa; pb[c++] = ticMax;
            pb[c++] = mthd(1, 0x155C, 3);
            pb[c++] = (uint32_t)(tscVa >> 32); pb[c++] = (uint32_t)tscVa; pb[c++] = tscMax;
        }
    }
    uint64_t codeVa;
    if (ci >= 0) codeVa = gBCode[ci].va;
    else {
        codeVa = batchPut(code, codeWords * 4, 1024);
        gBCode[gBNCode].code = code; gBCode[gBNCode].words = codeWords; gBCode[gBNCode].va = codeVa;
        gBNCode++;
    }
    uint32_t cb[256] = {0};
    memcpy(cb, push, npush * 4);
    uint32_t cbw = (npush + 3) & ~3u;
    if (cbw < 4) cbw = 4;
    const uint64_t cbVa = batchPut(cb, cbw * 4, 0);
    uint32_t qs[64];
    buildQmd(qs, codeVa, cbVa, cbw * 4, grid, block, regs);
    qmdShared(qs, smem, block, barriers, slm);
    const uint64_t qmdVa = batchPut(qs, 256, 0);
    pb[c++] = mthd(1, 0x21C, 1); pb[c++] = 0x1011;            // INVALIDATE_SHADER_CACHES (instr, data, const)
    pb[c++] = (0x4u << 29) | (1u << 13) | (0x244u >> 2);      // IMMD INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI
    pb[c++] = (0x4u << 29) | (1u << 13) | (0x1424u >> 2);     // IMMD INVALIDATE_SAMPLER_CACHE_NO_WFI
    pb[c++] = (0x4u << 29) | (1u << 13) | (0x1288u >> 2);     // IMMD INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI
    pb[c++] = (0x4u << 29) | (1u << 13) | (0x298u >> 2);
    pb[c++] = mthd(1, 0x2B4, 1); pb[c++] = (uint32_t)(qmdVa >> 8);
    pb[c++] = (0x4u << 29) | (3u << 16) | (1u << 13) | (0x2C0u >> 2);
    gBPush = c;
    gBN++;
    return true;
}

// CE memset like NVK's vkCmdFillBuffer: REMAP_CONST_A as every 4-byte
// component, pitch layout, up to 32768 x 32768 words per launch. A trailing
// 1-3 bytes (Metal allows any length) are done through the old pattern copy.
#define B5_PITCH_IN 0x410u
#define B5_REMAP_CONST_A 0x700u
#define B5_REMAP_COMPONENTS 0x708u
#define B5_REMAP_ALL_CONST_A_4B (0x4444u | (3u << 16))
bool nvCeFill(uint64_t dstVa, uint64_t bytes, uint32_t value) {
    NV_GPU_LOCKED;
    if (!gCpu || !bytes) return false;
    // 0.9.27; 0.9.45 CE for VRAM; 0.9.87/88: odd lengths in VRAM (no CPU view) take the CE + pattern-tail path too
    if (gN615 && (!nv615CeOn() || nv615Cpu(dstVa, bytes))) {
        if (!nvGrFlush()) return false;
        uint8_t *d = nv615Cpu(dstVa, bytes);
        if (!d) { NSLog(@"NVMTLGsp: native615 fill outside mapped memory"); nvNoteGpuFailure(1); return false; }
        for (uint64_t i = 0; i < bytes; i++) d[i] = (uint8_t)(value >> (8 * (i & 3)));
        return true;
    }
    uint32_t *pb = (uint32_t *)(gCpu + NVGSP_STAGE_PUSH);
    const uint32_t cap = NVGSP_STAGE_PUSH_MAX / 4, maxDim = 1u << 15;
    uint32_t w = 0;
    uint64_t d = dstVa, left = bytes & ~3ULL;
    while (left) {
        if (w + 16 > cap || !w) {
            if (w && !ceSubmit(w)) return false;
            w = 0;
            pb[w++] = mthd(NVGSP_CE_SUBCH, B5_REMAP_CONST_A, 1); pb[w++] = value;
            pb[w++] = mthd(NVGSP_CE_SUBCH, B5_REMAP_COMPONENTS, 1); pb[w++] = B5_REMAP_ALL_CONST_A_4B;
            pb[w++] = mthd(NVGSP_CE_SUBCH, B5_PITCH_IN, 2); pb[w++] = maxDim * 4; pb[w++] = maxDim * 4;
        }
        uint64_t width, height;
        if (left >= (uint64_t)maxDim * maxDim * 4) width = height = maxDim;
        else if (left >= maxDim * 4) { width = maxDim; height = left / (maxDim * 4); }
        else { width = left / 4; height = 1; }
        pb[w++] = mthd(NVGSP_CE_SUBCH, 0x408u /* OFFSET_OUT_UPPER */, 2);
        pb[w++] = (uint32_t)(d >> 32); pb[w++] = (uint32_t)d;
        pb[w++] = mthd(NVGSP_CE_SUBCH, B5_LINE_LEN, 2);
        pb[w++] = (uint32_t)width; pb[w++] = (uint32_t)height;
        pb[w++] = mthd(NVGSP_CE_SUBCH, B5_LAUNCH, 1);
        pb[w++] = 0x001u | 0x004u | 0x080u | 0x100u | 0x400u | (height > 1 ? 0x200u : 0);
        d += width * height * 4; left -= width * height * 4;
    }
    if (w && !ceSubmit(w)) return false;
    if (bytes & 3) {   // tail: pattern page (CPU-filled) + byte copy
        uint32_t *pat = (uint32_t *)(gCpu + NVGSP_STAGE_PATTERN);
        pat[0] = value;
        return nvCeCopy(dstVa + (bytes & ~3ULL), gVa + NVGSP_STAGE_PATTERN, bytes & 3);
    }
    return true;
}

// ---------------------------------------------------------------- 3D (0.5.0)
// Method offsets: NVIDIA clc997.h (ADA_A), sequences as nvrun's first
// triangle and NVK's draw macros (BEGIN / SET_VERTEX_ARRAY_START /
// DRAW_VERTEX_ARRAY / END, instance count on Pascal B+).
#define NVGSP_3D_CB_VS 0x160000ULL   // 4 KiB: vertex stage cbuf 0
#define NVGSP_3D_CB_FS 0x161000ULL   // 4 KiB: fragment stage cbuf 0
#define NVGSP_3D_CB_TCS 0x162000ULL  // 4 KiB: tessellation control cbuf 0
#define NVGSP_3D_CB_TES 0x163000ULL  // 4 KiB: tessellation evaluation cbuf 0

static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

// Fixed 3D state every draw starts from (a subset of NVK's
// nvk_push_draw_state_init: render enable forced on, SPH v3 / AAM v2, no
// compression, clears use the clear rect, vertex id counts from the array
// start, blend per target, depth/stencil off, fill mode, raster on, sample
// masks, zcull off). Encoded with Mesa's generated class headers by
// tools/nvaccel/air/gen3d.c; subchannel 0.
static const uint32_t kInit3D[] = {
0x20010556, 0x00000001, 0x20010651, 0x00000001, 0x200100c3, 0x00000000,
0x20010673, 0x00000000, 0x20080678, 0x00000000, 0x00000000, 0x00000000,
0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x20010083,
0x00000001, 0x2001037a, 0x00000000, 0x200104cf, 0x00000001, 0x200103e4,
0x00000001, 0x200104d7, 0x00000000, 0x20010565, 0x00000001, 0x200104b5,
0x00001d01, 0x20010359, 0x00000008, 0x200100c2, 0x00000003, 0x200105aa,
0x00030003, 0x200105e5, 0x00020002, 0x20010450, 0x00000010, 0x20010584,
0x0000000e, 0x20010593, 0x00001000, 0x200100c0, 0x00000003, 0x200103f7,
0x00000001, 0x20010670, 0x00000001, 0x200104b9, 0x00000001, 0x200104bb,
0x00000000, 0x200105a2, 0x00000000, 0x20010980, 0x00000001, 0x200104ea,
0x00000000, 0x20010546, 0x3f800000, 0x20010644, 0x00000001, 0x20010574,
0x00000000, 0x2001054d, 0x00000000, 0x200104b3, 0x00000000, 0x200104ba,
0x00000000, 0x200104e0, 0x00000000, 0x2001066f, 0x00000000, 0x2001036b,
0x00001b02, 0x2001036c, 0x00001b02, 0x20010372, 0x00000000, 0x200100df,
0x00000001, 0x20010671, 0x00000000, 0x2001043e, 0x00000010, 0x2001064f,
0x00000018, 0x200105a1, 0x00000000, 0x200103ef, 0x0000ffff, 0x200103f0,
0x0000ffff, 0x200103f1, 0x0000ffff, 0x200103f2, 0x0000ffff, 0x20010564,
0x0000003f, 0x2001065a, 0x00000000, 0x20010547, 0x00000000, 0x2001065f,
0x00000000, 0x2001064b, 0x00000001, 0x20010649, 0x00000000, 0x2002037e,
0x00000000, 0x00000000, 0x2001036d, 0x00000000, 0x20010673, 0x00000000,
};

// 0.6.12: what a draw that hung or faulted the GPU was made of: the shader
// VAs (match "shader X stage N at" upload lines), targets and the 64-bit
// words of each stage's cbuf 0 (buffer addresses among them)
static void nvGr3DDump(const NV3DDraw *d) {
    static uint32_t n;
    if (n++ >= 4) return;
    NSLog(@"NVMTLGsp: faulting draw textures:%s", gLastTex);
    NSLog(@"NVMTLGsp: faulting draw: vs 0x%llx fs 0x%llx tcs 0x%llx tes 0x%llx, rt0 0x%llx %ux%u fmt 0x%x, z 0x%llx",
          d->vs.va, d->fs.va, d->tcs.va, d->tes.va, d->rt[0].va, d->rt[0].w, d->rt[0].h, d->rt[0].format, d->zVa);
    const NV3DStage *st[2] = {&d->vs, &d->fs};
    for (int k = 0; k < 2; k++) {
        NSMutableString *m = [NSMutableString new];
        for (uint32_t i = 0; st[k]->push && i + 1 < st[k]->pushWords && i < 64; i += 2)
            [m appendFormat:@" %llx", (unsigned long long)st[k]->push[i] | (unsigned long long)st[k]->push[i + 1] << 32];
        NSLog(@"NVMTLGsp: faulting draw %s cb0:%@", k ? "fs" : "vs", m);
        nvDumpPointers(k ? "fs" : "vs", st[k]->push, st[k]->pushWords);
    }
}

// 0.8.4 (native N2): inside a batch a draw is appended to the batch's method
// stream (cbufs in the batch's data area) like a compute launch, so a
// command buffer of draws and dispatches is one submission; the family
// completes it on the native path. Outside a batch it is submitted and
// waited for as before.
// 0.9.75: per-draw GPU time. With /Library/Preferences/nvmtl-gputime, every
// 3D draw is bracketed by two four-word semaphore releases ({id, 0, GPU
// timestamp}): one after the earlier work (start), one after the draw's own
// writes (end), so GPU idle time between frames is not charged to the draw.
// A thread folds finished pairs by draw label every 2 s and logs the top
// costs as NVMTL_GPUTIME. Diagnostic only: the extra releases order the pipe.
#define NV_GT_N 4096                             // draws in flight in the ring
typedef struct { uint32_t id, pad; uint64_t ts; } nvGtStamp;
static nvGtStamp *gGt;                           // 2 stamps per draw
static uint64_t gGtVa;
static uint32_t gGtNext;
static char gGtLabel[NV_GT_N][96];
static uint32_t gGtId[NV_GT_N];                  // id the slot expects (0: free)
static void *nvGpuTimeThread(void *arg) {
    (void)arg;
    for (;;) {
        sleep(2);
        struct { char label[96]; uint64_t ns, max; uint32_t n; } agg[64];
        uint32_t na = 0;
        uint64_t total = 0;
        for (uint32_t i = 0; i < NV_GT_N; i++) {
            const uint32_t id = gGtId[i];
            if (!id) continue;
            const nvGtStamp a = gGt[2 * i], b = gGt[2 * i + 1];
            if (a.id != id || b.id != id || b.ts < a.ts) continue;   // not finished yet
            const uint64_t ns = b.ts - a.ts;
            gGtId[i] = 0;
            total += ns;
            uint32_t k = 0;
            while (k < na && strcmp(agg[k].label, gGtLabel[i])) k++;
            if (k == na) {
                if (na == 64) continue;
                snprintf(agg[na].label, sizeof agg[na].label, "%s", gGtLabel[i]);
                agg[na].ns = agg[na].max = 0; agg[na].n = 0; na++;
            }
            agg[k].ns += ns; agg[k].n++;
            if (ns > agg[k].max) agg[k].max = ns;
        }
        if (!total) continue;
        for (uint32_t r = 0; r < 12 && r < na; r++) {   // top 12 by total time
            uint32_t best = r;
            for (uint32_t k = r + 1; k < na; k++) if (agg[k].ns > agg[best].ns) best = k;
            if (best != r) { __typeof__(agg[0]) t = agg[r]; agg[r] = agg[best]; agg[best] = t; }
            NSLog(@"NVMTL_GPUTIME %5.1f%% %8.3f ms total %5u draws avg %7.3f max %7.3f ms  %s",
                  100.0 * agg[r].ns / total, agg[r].ns / 1e6, agg[r].n, agg[r].ns / 1e6 / agg[r].n,
                  agg[r].max / 1e6, agg[r].label);
        }
        NSLog(@"NVMTL_GPUTIME window total %.3f ms over 2 s", total / 1e6);
    }
    return NULL;
}
static bool nvGpuTimeOn(void) {
    static int on = -1;
    if (on < 0) {
        struct stat st;
        on = stat("/Library/Preferences/nvmtl-gputime", &st) == 0 && st.st_uid == 0;
        void *mem = NULL;
        if (on && posix_memalign(&mem, 4096, NV_GT_N * 2 * sizeof(nvGtStamp)) == 0) {
            memset(mem, 0, NV_GT_N * 2 * sizeof(nvGtStamp));
            uint64_t va = 0;
            if (nvHeapWrap(mem, NV_GT_N * 2 * sizeof(nvGtStamp), &va)) {
                gGt = mem; gGtVa = va;
                pthread_t th;
                pthread_create(&th, NULL, nvGpuTimeThread, NULL);
                pthread_detach(th);
                NSLog(@"NVMTL_GPUTIME on (ring %u draws at 0x%llx)", NV_GT_N, (unsigned long long)va);
            }
        }
        if (!gGt) on = 0;
    }
    return on == 1;
}

bool nvGr3DDraw(const NV3DDraw *d) {
    NV_GPU_LOCKED;
    // 0.8.8: draws stay out of the batch unless NVMTL_DRAWBATCH=1 or the file
    // /Library/Preferences/nvmtl-drawbatch: every native login hang (28 Sep,
    // attempts 1-6) was a WindowServer batch of draws, the login blur chain
    // (downsample_blur / narrow_blur passes sampling the previous pass)
    static int drawBatch = -1;
    if (drawBatch < 0) {
        // 0.9.44: on by default on native615 (suite84 83/84 and fps 107 -> 120
        // with it; the login hangs were the legacy channel)
        const char *e = getenv("NVMTL_DRAWBATCH");
        drawBatch = e ? atoi(e) != 0 : (gN615 || access("/Library/Preferences/nvmtl-drawbatch", F_OK) == 0);
    }
    const bool batched = gBDepth && nvBatchOn() && drawBatch;
    if (!batched) nvGrFlush();
    if (!gCpu || (!d->nrt && !d->zVa) || d->nrt > 8) return false;   // 0.8.16: depth-only passes
    uint32_t slm = d->vs.slm > d->fs.slm ? d->vs.slm : d->fs.slm;
    if (d->tcs.slm > slm) slm = d->tcs.slm;
    if (d->tes.slm > slm) slm = d->tes.slm;
    if (!slmEnsure(slm)) return false;                  // may flush the batch: before we take its cursor
    const uint32_t maxw = 0x10000 / 4 - 64;
    if (batched && gBN && (gBPush + maxw + 8 > NVB_PUSH_MAX / 4 || NVB_PUSH_MAX + gBData + 5 * 4096 > NVB_BYTES))
        nvGrFlush();
    uint32_t *pb = batched ? (uint32_t *)gBCpu + gBPush : (uint32_t *)(gCpu + NVGSP_GR_PUSH), c = 0;
#define M3(m, n) (0x20000000u | ((uint32_t)(n) << 16) | (0u << 13) | ((m) >> 2))
#define P1(m, v) do { pb[c++] = M3(m, 1); pb[c++] = (uint32_t)(v); } while (0)
    uint32_t gtSlot = 0, gtId = 0;                                      // 0.9.75: per-draw GPU time
    // 0.8.22: the next draw of the same pass follows without a wait; a new
    // pass (or compute before it) first flushes the previous pass's ROP writes
    // 0.8.40: a draw that reads its colour attachments waits for the earlier draws' writes too
    const bool samePass = batched && gBN && gB3DDirty && d->passId && d->passId == gB3DPass && !d->fbFetch;
    if (batched && gBN && !samePass) {
        if (gB3DDirty) c = nv3DFlushWords(pb, c);
        else pb[c++] = (0x4u << 29) | (0u << 13) | (0x110u >> 2);   // IMMD WAIT_FOR_IDLE after earlier work
    }
    // 0.9.48: the next draw of the same pass keeps the class and the static
    // init state (a third of every draw's words); NVMTL_3D_REINIT=1 restores it
    static int reinit = -1;
    if (reinit < 0) reinit = getenv("NVMTL_3D_REINIT") != NULL;
    if (!samePass || reinit) {
        pb[c++] = M3(0x0, 1); pb[c++] = 0xC997;                          // SET_OBJECT ADA_A, subch 0
        memcpy(pb + c, kInit3D, sizeof kInit3D);
        c += sizeof kInit3D / 4;
    }
    if (gSlmVa) {   // SET_SHADER_LOCAL_MEMORY_A..E (the SPH carries each stage's size)
        pb[c++] = M3(0x790, 5);
        pb[c++] = (uint32_t)(gSlmVa >> 32); pb[c++] = (uint32_t)gSlmVa;
        pb[c++] = (uint32_t)(gSlmSize >> 32); pb[c++] = (uint32_t)gSlmSize;
        pb[c++] = (uint32_t)gSlmPerWarp;
    }
    {   // texture pools (shared with compute) and caches
        uint64_t ticVa, tscVa;
        uint32_t ticMax, tscMax;
        if (nvTexPool(&ticVa, &ticMax, &tscVa, &tscMax)) {
            pb[c++] = M3(0x1574, 3); pb[c++] = (uint32_t)(ticVa >> 32); pb[c++] = (uint32_t)ticVa; pb[c++] = ticMax;
            pb[c++] = M3(0x155c, 3); pb[c++] = (uint32_t)(tscVa >> 32); pb[c++] = (uint32_t)tscVa; pb[c++] = tscMax;
            P1(0x1428, 0);                                              // INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI
            P1(0x1424, 0);                                              // INVALIDATE_SAMPLER_CACHE_NO_WFI
            P1(0x1288, 0);                                              // INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI
        }
    }
    // color targets, pitch linear
    uint32_t w = ~0u, h = ~0u;
    for (uint32_t i = 0; i < d->nrt; ++i) {
        const NV3DTarget *t = &d->rt[i];
        pb[c++] = M3(0x800 + i * 64, 9);
        pb[c++] = (uint32_t)(t->va >> 32); pb[c++] = (uint32_t)t->va;
        // WIDTH: pixels (BL) / bytes (pitch); multisampled: in samples
        pb[c++] = t->blockLinear ? t->w * (t->sx ? t->sx : 1) : t->pitch; pb[c++] = t->h * (t->sy ? t->sy : 1);
        pb[c++] = t->format;
        pb[c++] = t->blockLinear ? (t->blockHeightLog2 & 15) << 4 | (t->blockDepthLog2 & 15) << 8 |
                                   (t->depthIsZ ? 1u << 16 : 0) : 1u << 12;           // MEMORY
        pb[c++] = t->depth ? t->depth : 1;                              // THIRD_DIMENSION
        pb[c++] = (uint32_t)(t->arrayPitch >> 2); pb[c++] = t->layer;   // ARRAY_PITCH, LAYER
        if (t->w < w) w = t->w;
        if (t->h < h) h = t->h;
    }
    if (!d->nrt) { w = d->zClipW; h = d->zClipH; }                     // depth-only: the zeta surface's size
    uint32_t sel = d->nrt;                                              // CT_SELECT: count, then target ids
    for (uint32_t i = 0; i < d->nrt; ++i) sel |= i << (4 + 3 * i);
    P1(0x121c, sel);
    // 0.8.20: layered pass: the vertex stage's [[render_target_array_index]]
    // picks the slice (SET_RT_LAYER CONTROL = shader selects, as NVK when the
    // stage writes the layer); otherwise slice 0
    P1(0x15cc, d->layers > 1 ? 1u << 16 : 0);
    static int zdbg = -1;   // 0.9.48: read once, not per draw
    if (zdbg < 0) zdbg = getenv("NVMTL_ZDBG") ? atoi(getenv("NVMTL_ZDBG")) : 0;
    if (d->zVa && !(zdbg & 1)) {                                        // zeta: block linear
        pb[c++] = M3(0x0fe0, 5);
        pb[c++] = (uint32_t)(d->zVa >> 32); pb[c++] = (uint32_t)d->zVa;
        pb[c++] = d->zFormat;
        pb[c++] = (d->zBlockHeightLog2 & 15) << 4;                       // BLOCK_SIZE: 1 GOB wide
        // ARRAY_PITCH (>> 2): one layer's size, as NVK passes array_stride_B
        pb[c++] = (uint32_t)(((uint64_t)d->zWidthEl * (d->zBpp ? d->zBpp : 4) *
                              ((d->zHeight + (8u << d->zBlockHeightLog2) - 1) & ~((8u << d->zBlockHeightLog2) - 1))) >> 2);
        P1(0x1538, 1);                                                  // ZT_SELECT: 1 target
        pb[c++] = M3(0x1228, 3);
        pb[c++] = d->zWidthEl; pb[c++] = d->zHeight; pb[c++] = 1u | (1u << 16);   // SIZE_A/B/C: array size one
        P1(0x179c, 0);                                                  // ZT_LAYER
        P1(0x19cc, 0);                                                  // Z_COMPRESSION off
        P1(0x1208, 0);                                                  // ZT_SPARSE off (NVK, Maxwell B+)
    } else P1(0x1538, 0);                                               // ZT_SELECT: none
    P1(0x0ff4, w << 16); P1(0x0ff8, h << 16);                           // SURFACE_CLIP
    // Multisampling: sample_id and per-sample interpolation need one pass
    // per sample, with centroid evaluated per pass (NVK anti-alias macro).
    P1(0x15d0, d->aaMode);                                              // ANTI_ALIAS samples mode
    P1(0x1534, d->aaMode ? 1 : 0);                                      // ANTI_ALIAS_ENABLE
    P1(0x0754, d->sampleShading && d->aaMode ? (1u << d->aaMode) | 0x10u : 1u);
    {
        // standard sample positions (Metal = D3D = Vulkan), 1/16 pixel units,
        // repeated over the 2x2 pixel quad the registers cover (NVK does this)
        static const uint8_t k1[] = {8, 8}, k2[] = {12, 12, 4, 4},
            k4[] = {6, 2, 14, 6, 2, 10, 10, 14},
            k8[] = {9, 5, 7, 11, 13, 9, 5, 3, 3, 13, 1, 7, 11, 15, 15, 1};
        static const uint8_t *const tab[4] = {k1, k2, k4, k8};
        const uint32_t n = 1u << d->aaMode, m = d->aaMode < 4 ? d->aaMode : 0;
        pb[c++] = M3(0x11e0, 4);                                        // ANTI_ALIAS_SAMPLE_POSITIONS(0..3)
        for (uint32_t i = 0; i < 4; ++i) {
            uint32_t v = 0;
            for (uint32_t k = 0; k < 4; ++k) {
                const uint32_t smp = (i * 4 + k) % n;
                v |= (uint32_t)(tab[m][smp * 2] & 15) << (k * 8) | (uint32_t)(tab[m][smp * 2 + 1] & 15) << (k * 8 + 4);
            }
            pb[c++] = v;
        }
    }
    P1(0x13ac, 0);                                                      // WINDOW_ORIGIN upper left
    // 0.8.40: the hardware clear honours the colour mask, but a Metal load action clears the whole
    // attachment whatever the first draw's pipeline masks: cleared targets get all four channels here,
    // the pipeline's masks after the clears. (RenderBox starts its passes with a stencil-only draw,
    // mask 0, so its coverage and colour targets were never cleared.)
    for (uint32_t i = 0; i < d->nrt; ++i) P1(0x1a00 + i * 4, d->rt[i].clear ? 0x1111u : d->rt[i].writeMask);
    // 0.8.40: kInit3D sets SINGLE_CT_WRITE_CONTROL, which applies COLOR_MASK(0) to every target; with
    // differing masks (RenderBox: colour 0 off, coverage target green only) each target needs its own,
    // as NVK does. The clears below use full masks per target, so per-target control from here on.
    P1(0x0f90, 0);                                                      // SET_SINGLE_CT_WRITE_CONTROL false
    // clears (load action), full surface, before the viewport/scissor apply
    P1(0x0e00, 0);                                                      // SCISSOR_ENABLE(0) off
    if (d->zVa && (d->zClear || d->sClear) && !(zdbg & 3)) {
        if (d->zClear) P1(0x0d90, f2u(d->zClearValue));                 // Z_CLEAR_VALUE
        if (d->sClear) { P1(0x0da0, d->sClearValue & 0xff); P1(0x139c, 0xff); }   // STENCIL_CLEAR_VALUE, mask
        P1(0x0d6c, w << 16); P1(0x0d70, h << 16);
        P1(0x19d0, (d->zClear ? 1u : 0) | (d->sClear ? 2u : 0));        // CLEAR_SURFACE: Z, stencil
    }
    for (uint32_t i = 0; i < d->nrt; ++i) {
        if (!d->rt[i].clear) continue;
        pb[c++] = M3(0x0d80, 4);
        for (int k = 0; k < 4; ++k) pb[c++] = f2u(d->rt[i].clearColor[k]);
        P1(0x0d6c, w << 16);                                            // CLEAR_RECT_HORIZONTAL 0..w
        P1(0x0d70, h << 16);                                            // CLEAR_RECT_VERTICAL 0..h
        // 0.8.20: every slice of a layered target (RT_ARRAY_INDEX 25:10)
        const uint32_t nl = d->layers && !d->rt[i].depthIsZ && d->rt[i].depth > 1 ? d->rt[i].depth : 1;
        for (uint32_t l = 0; l < nl; ++l)
            P1(0x19d0, 0x3c | (i << 6) | (l << 10));                    // CLEAR_SURFACE RGBA, target i, slice l
    }
    // 0.8.15: 0 is MTLColorWriteMaskNone (SpriteKit's shape fill draws its
    // coverage pass with no colour writes), not "unset": every target gets
    // its mask from the pipeline (All when there is none)
    for (uint32_t i = 0; i < d->nrt; ++i) if (d->rt[i].clear) P1(0x1a00 + i * 4, d->rt[i].writeMask);
    // NVMTL_3D_STOP=n cuts the stream after phase n (1 clears, 2 fixed
    // state, 3 programs, 4 vertex fetch) to find a method the class refuses
    static int stop = -1;
    if (stop < 0) stop = getenv("NVMTL_3D_STOP") ? atoi(getenv("NVMTL_3D_STOP")) : 99;
    if (d->draw && stop > 1) {
        // Metal: y up in NDC, z in [0,1]
        const float sx = (float)(d->vp[2] / 2), sy = (float)(-d->vp[3] / 2);
        const float ox = (float)(d->vp[0] + d->vp[2] / 2), oy = (float)(d->vp[1] + d->vp[3] / 2);
        const float sz = (float)(d->vp[5] - d->vp[4]), oz = (float)d->vp[4];
        pb[c++] = M3(0xa00, 3); pb[c++] = f2u(sx); pb[c++] = f2u(sy); pb[c++] = f2u(sz);   // VIEWPORT_SCALE
        pb[c++] = M3(0xa0c, 3); pb[c++] = f2u(ox); pb[c++] = f2u(oy); pb[c++] = f2u(oz);   // VIEWPORT_OFFSET
        P1(0x0c00, w << 16); P1(0x0c04, h << 16);                       // VIEWPORT_CLIP h/v
        P1(0x0c08, f2u(0.0f)); P1(0x0c0c, f2u(1.0f));                   // VIEWPORT_CLIP z
        P1(0x0d7c, 1);                                                  // VIEWPORT_Z_CLIP 0..w
        P1(0x0e00, 1);                                                  // SCISSOR_ENABLE(0)
        P1(0x0e04, (d->sc[0] + d->sc[2]) << 16 | d->sc[0]);             // SCISSOR_HORIZONTAL xmax << 16 | xmin
        P1(0x0e08, (d->sc[1] + d->sc[3]) << 16 | d->sc[1]);
        // 0.8.20: viewport arrays ([[viewport_array_index]]): viewport and
        // scissor i at 0x20 / 0x10 strides (NV9097), viewport 0 as above
        for (uint32_t i = 1; i < d->nvp && i < 16; ++i) {
            const double *v = d->vps[i];
            const float vsx = (float)(v[2] / 2), vsy = (float)(-v[3] / 2), vsz = (float)(v[5] - v[4]);
            const float vox = (float)(v[0] + v[2] / 2), voy = (float)(v[1] + v[3] / 2), voz = (float)v[4];
            pb[c++] = M3(0xa00 + i * 0x20, 3); pb[c++] = f2u(vsx); pb[c++] = f2u(vsy); pb[c++] = f2u(vsz);
            pb[c++] = M3(0xa0c + i * 0x20, 3); pb[c++] = f2u(vox); pb[c++] = f2u(voy); pb[c++] = f2u(voz);
            P1(0x0c00 + i * 16, w << 16); P1(0x0c04 + i * 16, h << 16);
            P1(0x0c08 + i * 16, f2u(0.0f)); P1(0x0c0c + i * 16, f2u(1.0f));
            P1(0x0e00 + i * 16, 1);
            P1(0x0e04 + i * 16, (d->scs[i][0] + d->scs[i][2]) << 16 | d->scs[i][0]);
            P1(0x0e08 + i * 16, (d->scs[i][1] + d->scs[i][3]) << 16 | d->scs[i][1]);
        }
        if (d->nvp > 1) {   // viewport 0 from the array too (setViewports overrides setViewport)
            P1(0x0e04, (d->scs[0][0] + d->scs[0][2]) << 16 | d->scs[0][0]);
            P1(0x0e08, (d->scs[0][1] + d->scs[0][3]) << 16 | d->scs[0][1]);
        }
        // blend + write masks
        for (uint32_t i = 0; i < d->nrt; ++i) {
            const NV3DTarget *t = &d->rt[i];
            P1(0x1360 + i * 4, t->blend ? 1 : 0);                       // BLEND(i)
            if (!t->blend) continue;
            pb[c++] = M3(0x1e00 + i * 32, 7);
            pb[c++] = 1;                                                // SEPARATE_FOR_ALPHA
            pb[c++] = t->colorOp; pb[c++] = t->colorSrc; pb[c++] = t->colorDst;
            pb[c++] = t->alphaOp; pb[c++] = t->alphaSrc; pb[c++] = t->alphaDst;
        }
        P1(0x12e4, 1);                                                  // BLEND_STATE_PER_TARGET
        const bool zt = d->zVa && !(zdbg & 5);
        P1(0x12cc, zt && d->zTest ? 1 : 0);                             // DEPTH_TEST
        P1(0x12e8, zt && d->zWrite ? 1 : 0);                            // DEPTH_WRITE
        if (zt && d->zTest) P1(0x130c, d->zFunc);                       // DEPTH_FUNC
        // 0.5.6: stencil, front + two-sided back
        P1(0x1380, zt && d->sTest ? 1 : 0);                             // STENCIL_TEST
        if (zt && d->sTest) {
            pb[c++] = M3(0x1384, 7);
            pb[c++] = d->sFail[0]; pb[c++] = d->sZFail[0]; pb[c++] = d->sZPass[0]; pb[c++] = d->sFunc[0];
            pb[c++] = d->sRef[0]; pb[c++] = d->sReadMask[0]; pb[c++] = d->sWriteMask[0];
            P1(0x1594, 1);                                              // TWO_SIDED_STENCIL_TEST
            pb[c++] = M3(0x1598, 4);
            pb[c++] = d->sFail[1]; pb[c++] = d->sZFail[1]; pb[c++] = d->sZPass[1]; pb[c++] = d->sFunc[1];
            pb[c++] = M3(0x0f54, 3);
            pb[c++] = d->sRef[1]; pb[c++] = d->sWriteMask[1]; pb[c++] = d->sReadMask[1];   // REF, MASK, FUNC_MASK
        }
        // raster
        P1(0x037c, d->rasterOff ? 0 : 1);                               // SET_RASTER_ENABLE (0.8.16)
        P1(0x1918, d->cull ? 1 : 0);                                    // CULL enable
        if (d->cull) P1(0x1920, d->cull == 1 ? 0x404 : 0x405);          // FRONT / BACK
        P1(0x191c, d->frontCCW ? 0x901 : 0x900);                        // front face CCW / CW
        if (stop <= 2) goto submit;
        // programs: VS (1) and FS (5), plus TCS (2) / TES (3) when
        // tessellating; constant buffer 0 of each (bind groups 0..4)
        const bool tess = d->tes.va != 0;
        for (uint32_t st = 0; st < 6; ++st)
            if (st != 1 && st != 5 && !(tess && (st == 2 || st == 3))) P1(0x2000 + st * 0x40, st << 4);   // disabled
        const NV3DStage *stg[4] = {&d->vs, &d->fs, &d->tcs, &d->tes};
        const uint64_t cbOff[4] = {NVGSP_3D_CB_VS, NVGSP_3D_CB_FS, NVGSP_3D_CB_TCS, NVGSP_3D_CB_TES};
        const uint32_t prog[4] = {1, 5, 2, 3}, group[4] = {0, 4, 1, 2};
        if (tess) {
            P1(0x0dcc, d->patchCps);                                    // SET_PATCH: control points
            P1(0x0320, d->tessParams);                                  // SET_TESSELLATION_PARAMETERS
        }
        for (int k = 0; k < (tess ? 4 : 2); ++k) {
            const NV3DStage *s = stg[k];
            P1(0x2000 + prog[k] * 0x40, (prog[k] << 4) | 1);            // SET_PIPELINE_SHADER enable
            pb[c++] = M3(0x2014 + prog[k] * 0x40, 2);
            pb[c++] = (uint32_t)(s->va >> 32); pb[c++] = (uint32_t)s->va;
            pb[c++] = M3(0x200c + prog[k] * 0x40, 2); pb[c++] = s->gprs; pb[c++] = group[k];
            const uint32_t words = s->pushWords < 1024 ? s->pushWords : 1024;
            uint64_t cva;
            if (batched) {
                cva = batchPut(s->push, words * 4, 4096 - words * 4);
            } else {
                memset(gCpu + cbOff[k], 0, 4096);
                if (words) memcpy(gCpu + cbOff[k], s->push, words * 4);
                cva = gVa + cbOff[k];
            }
            pb[c++] = M3(0x2380, 3); pb[c++] = 4096; pb[c++] = (uint32_t)(cva >> 32); pb[c++] = (uint32_t)cva;
            P1(0x2410 + group[k] * 32, 1);                              // BIND_GROUP_CONSTANT_BUFFER slot 0 valid
        }
        P1(0x0da4, 0x1011);                                             // INVALIDATE_SHADER_CACHES_NO_WFI + constants
        if (stop <= 3) goto submit;
        // vertex fetch
        for (uint32_t i = 0; i < 32; ++i) {
            const NV3DAttr *a = &d->attr[i];
            // inactive still needs a valid format: numerical type 0 is
            // "do not use" and the class raises an error (Xid 69) on it
            uint32_t v = (1u << 6) | (1u << 21) | (7u << 27);           // INACTIVE, R32G32B32A32, FLOAT
            if (a->used)
                v = (a->stream & 31) | ((a->offset & 0x3fff) << 7) | ((a->widths & 63) << 21) |
                    ((a->type & 7) << 27) | (a->swapRB ? 1u << 31 : 0);
            P1(0x1160 + i * 4, v);
        }
        for (uint32_t j = 0; j < 32; ++j) {
            const NV3DStream *s = &d->stream[j];
            if (!s->used) { P1(0x1c00 + j * 16, 0); continue; }
            pb[c++] = M3(0x1c00 + j * 16, 4);
            pb[c++] = (s->stride & 0xfff) | (1u << 12);
            pb[c++] = (uint32_t)(s->va >> 32); pb[c++] = (uint32_t)s->va;
            pb[c++] = s->divisor ? s->divisor : 0;
            P1(0x1880 + j * 4, s->divisor ? 1 : 0);                     // VERTEX_STREAM_INSTANCE_A
            // Turing+: stream size, not an end address (NVK: SET_VERTEX_STREAM_SIZE_A/B)
            pb[c++] = M3(0x0600 + j * 8, 2); pb[c++] = (uint32_t)(s->size >> 32); pb[c++] = (uint32_t)s->size;
        }
        if (stop <= 4) goto submit;
        // draw
        if (nvGpuTimeOn()) {                                            // 0.9.75: start stamp
            gtSlot = gGtNext++ % NV_GT_N;
            gtId = gGtNext;
            gGtId[gtSlot] = 0;
            snprintf(gGtLabel[gtSlot], sizeof gGtLabel[gtSlot], "%s rt %ux%u", gLastLabel, d->rt[0].w, d->rt[0].h);
            const uint64_t a = gGtVa + (uint64_t)gtSlot * 2 * sizeof(nvGtStamp);
            pb[c++] = M3(0x1b00, 4);
            pb[c++] = (uint32_t)(a >> 32); pb[c++] = (uint32_t)a; pb[c++] = gtId;
            pb[c++] = (1u << 4) | (15u << 12);                          // RELEASE after writes, ALL, FOUR_WORDS (+timestamp)
        }
        P1(0x1434, (uint32_t)d->baseVertex);                            // GLOBAL_BASE_VERTEX_INDEX
        P1(0x1118, (uint32_t)d->baseVertex);                            // VERTEX_ID_BASE
        P1(0x1438, d->baseInstance);                                    // GLOBAL_BASE_INSTANCE_INDEX
        if (d->indexed) {
            pb[c++] = M3(0x17c8, 2); pb[c++] = (uint32_t)(d->indexVa >> 32); pb[c++] = (uint32_t)d->indexVa;
            pb[c++] = M3(0x0238, 2); pb[c++] = 0; pb[c++] = (uint32_t)d->indexBytes;   // INDEX_BUFFER_SIZE
            // 0.8.43: Metal always restarts line and triangle strips at the all-ones index (0xffff /
            // 0xffffffff); RenderBox draws its path hulls as restarted strips, which were joined up
            // through vertex 65535 into streaks across every glyph
            const bool strip = d->topology == 3 || d->topology == 5;
            P1(0x1644, strip ? 1 : 0);                                  // SET_DA_PRIMITIVE_RESTART
            if (strip) P1(0x1648, d->indexSize == 2 ? 0xffffffffu : d->indexSize == 1 ? 0xffffu : 0xffu);   // _INDEX
            P1(0x17d8, d->indexSize);                                   // INDEX_BUFFER_E: index size
        }
        P1(0x0220, d->instances ? d->instances : 1);                    // SET_INSTANCE_COUNT
        P1(0x1618, d->topology | (1u << 31));                           // BEGIN, instance iterate
        if (d->indexed) { pb[c++] = M3(0x17dc, 2); pb[c++] = d->first; pb[c++] = d->count; }
        else { pb[c++] = M3(0x0d74, 2); pb[c++] = d->first; pb[c++] = d->count; }
        P1(0x1614, 0);                                                  // END
    }
submit:
    if (gtId) {                                                         // 0.9.75: end stamp
        const uint64_t a = gGtVa + (uint64_t)gtSlot * 2 * sizeof(nvGtStamp) + sizeof(nvGtStamp);
        pb[c++] = M3(0x1b00, 4);
        pb[c++] = (uint32_t)(a >> 32); pb[c++] = (uint32_t)a; pb[c++] = gtId;
        pb[c++] = (1u << 4) | (15u << 12);
        gGtId[gtSlot] = gtId;
    }
    // 0.5.4: release a semaphore after all writes, with the flush on: ROP
    // writes still in the colour caches otherwise miss a copy engine readback.
    // 0.8.22: a batched draw leaves it to the pass end (gB3DDirty)
    if (batched) {
        if (c > maxw) { NSLog(@"NVMTLGsp: 3D method stream too long"); return false; }
        gBPush += c; gBN++;
        gB3DDirty = true; gB3DPass = d->passId;
        return true;
    }
    {
        static uint32_t seq;
        const uint64_t sva = gVa + NVGSP_GR_SEM;
        pb[c++] = M3(0x1b00, 4);
        pb[c++] = (uint32_t)(sva >> 32); pb[c++] = (uint32_t)sva; pb[c++] = ++seq;
        pb[c++] = (1u << 4) | (15u << 12) | (1u << 28);                 // RELEASE after writes, ALL, ONE_WORD
    }
    P1(0x0110, 0);                                                      // WAIT_FOR_IDLE
#undef P1
#undef M3
    if (c > maxw) { NSLog(@"NVMTLGsp: 3D method stream too long"); return false; }
    if (batched) { gBPush += c; gBN++; return true; }
    static int trace3d = -1;
    if (trace3d < 0) trace3d = getenv("NVMTL_TRACE") != NULL;
    if (trace3d)
        NSLog(@"NVMTL_TRACE 3d: rt0 va 0x%llx pitch %u %ux%u fmt 0x%x clear %d, vs 0x%llx fs 0x%llx, %u words",
              d->rt[0].va, d->rt[0].pitch, d->rt[0].w, d->rt[0].h, d->rt[0].format, d->rt[0].clear,
              d->vs.va, d->fs.va, c);
    struct { uint64_t va; uint32_t dw; uint32_t pad; } seg = {gVa + NVGSP_GR_PUSH, c, 0};
    if (gN615) {   // 0.9.33: draws on the owned client channel (3D object allocated with it)
        if (!nv615Submit(seg.va, seg.dw)) { nvNoteGpuFailure(2); nvGr3DDump(d); return false; }
        return true;
    }
    uint64_t in[1] = {0}, out[1] = {0};
    uint32_t outN = 1;
    if (!callMethod(kSelExecSeg, in, 1, out, &outN, &seg, sizeof(seg)) || !out[0]) {
        NSLog(@"NVMTLGsp: 3D execSegments failed"); nvNoteGpuFailure(1); return false;
    }
    uint64_t wi[2] = {out[0], nvFenceUs()}, wo[1] = {0};
    outN = 1;
    if (IOConnectCallMethod(gConn, 18, wi, 2, NULL, 0, wo, &outN, NULL, NULL)) {
        NSLog(@"NVMTLGsp: 3D fenceWait failed"); nvNoteGpuFailure(2); nvGr3DDump(d); return false;
    }
    return true;
}

// 0.9.80: macOS hands out new GPU resources zero-filled (fresh VM pages), and
// frameworks rely on it: RenderBox loads new private icon layers without a
// clear, so a VRAM span recycled from an earlier icon showed that icon
// (Stocks with the Apps grid). It also keeps one process's freed VRAM from
// reaching another. NVMTL_ZERO_NEW=0 or /Library/Preferences/nvmtl-zeronew-off: old behaviour.
static bool nvZeroNewOn(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("NVMTL_ZERO_NEW");
        on = e ? atoi(e) != 0 : access("/Library/Preferences/nvmtl-zeronew-off", F_OK) != 0;
    }
    return on;
}
bool nvVramAlloc(uint64_t bytes, uint64_t *va) {
    if (!nvVramAllocRaw(bytes, va)) return false;
    if (nvZeroNewOn() && !nvCeFill(*va, (bytes + 3) & ~3ULL, 0)) NSLog(@"NVMTLGsp: zero fill of a new VRAM span failed");
    return true;
}
bool nvVramAllocKind(uint64_t bytes, uint32_t kind, uint64_t *va, uint32_t *handle) {
    NV_GPU_LOCKED;
    if (!nvVramAllocKindRaw(bytes, kind, va, handle)) return false;
    if (nvZeroNewOn() && !nvCeFill(*va, (bytes + 3) & ~3ULL, 0)) NSLog(@"NVMTLGsp: zero fill of a new surface failed");
    return true;
}
