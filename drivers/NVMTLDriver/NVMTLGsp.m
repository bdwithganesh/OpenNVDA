// See NVMTLGsp.h.
#import "NVMTLGsp.h"
#import "NVMTLTexHw.h"
#import <IOKit/IOKitLib.h>
#import <objc/message.h>
#include <string.h>
#include <os/lock.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/mach_time.h>
#include <time.h>
#include <unistd.h>
#import <Metal/Metal.h>
#include <pthread.h>

#define NVGSP_ARENA_BASE 0x2800000000ULL
#define NVGSP_STAGE_BYTES (22 << 20)     // + 4 MiB GR batch at 2 MiB; 0.8.0: + 8 x 2 MiB native slabs at 6 MiB

// NVGspControl user-client selectors (NVGspControlUserClient.cpp).
enum { kSelExecSeg = 21, kSelMemAlloc = 22, kSelMemFree = 23, kSelVaBindObj = 24, kSelCeWait = 29 };

// NV906F DMA_INCR method header: (1<<29)|(count<<16)|(subch<<13)|(mthd>>2).
static inline uint32_t mthd(unsigned sub, uint32_t m, unsigned count) {
    return (1u << 29) | ((count & 0x1fff) << 16) | ((sub & 7) << 13) | ((m >> 2) & 0xfff);
}

// NV90B5 copy methods + LAUNCH_DMA bits (Mesa cl90b5.h; sequence = NVK's).
#define B5_OFF_IN_UP 0x400u
#define B5_LINE_LEN 0x418u
#define B5_LAUNCH 0x300u
#define B5_LAUNCH_PITCH_COPY (0x001u | 0x200u | 0x004u | 0x080u | 0x100u) // PIPELINED|MULTI|FLUSH|PITCH|PITCH
#define CE_CHUNK (1 << 17) // 128 KiB, as NVK

static io_connect_t gConn = IO_OBJECT_NULL;
static uint8_t *gCpu = NULL;
static uint64_t gVa = 0;
static uint32_t gHandle = 0;

// one submitter at a time. The staging object (QMD, cbuf, method
// streams, DATA) is one per process, and WindowServer commits from several
// threads; two commits at once overwrote each other's launches. Recursive:
// nvExecuteOps holds it for a whole command buffer and the helpers take it
// again.
static pthread_mutex_t gGpuMtx;
static void gpuMtxInit(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&gGpuMtx, &a);
}
void nvGpuLock(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, gpuMtxInit);
    pthread_mutex_lock(&gGpuMtx);
}
void nvGpuUnlock(void) { pthread_mutex_unlock(&gGpuMtx); }
static inline void gpuUnlockAtExit(int *p) { (void)p; nvGpuUnlock(); }
#define NV_GPU_LOCKED nvGpuLock(); int nvLk_ __attribute__((cleanup(gpuUnlockAtExit), unused)) = 0

static bool nvGspRecover(io_connect_t dead);
static kern_return_t callOn(io_connect_t c, uint32_t sel, const uint64_t *in, uint32_t inN,
                            uint64_t *out, uint32_t *outN, const void *sIn, size_t sInN) {
    return IOConnectCallMethod(c, sel, in, inN, sIn, sInN, out, outN, NULL, NULL);
}
static bool callMethod(uint32_t sel, const uint64_t *in, uint32_t inN,
                       uint64_t *out, uint32_t *outN, const void *sIn, size_t sInN) {
    const io_connect_t c = gConn;
    kern_return_t kr = callOn(c, sel, in, inN, out, outN, sIn, sInN);
    // after a GPU reset the kext refuses a client from before it
    // (kIOReturnOffline) for good; open a new one, put every object back at
    // its VA and go on, instead of failing every draw until WindowServer
    // restarts
    if (kr == kIOReturnOffline && nvGspRecover(c))
        kr = callOn(gConn, sel, in, inN, out, outN, sIn, sInN);
    if (kr != KERN_SUCCESS) {
        // say why; rate limited so a stuck engine can't flood the log
        static uint32_t nlog;
        if (nlog++ < 64 || !(nlog & 1023)) NSLog(@"NVMTLGsp: selector %u -> 0x%x (#%u)", sel, kr, nlog);
    }
    return kr == KERN_SUCCESS;
}

// through the accelerator first (connect type 'NVGP'): the same
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

// 6 MiB SYS object (2 MiB-aligned contiguous chunks, bindable) at gVa,
// mapped into the task
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
    if (IOConnectMapMemory(c, 0x1000u | (uint32_t)out[0], mach_task_self(), &addr, &size,
                           kIOMapAnywhere) != KERN_SUCCESS || !addr || size < NVGSP_STAGE_BYTES) {
        NSLog(@"NVMTLGsp: map staging failed"); return false;
    }
    *handle = (uint32_t)out[0];
    *cpu = (uint8_t *)(uintptr_t)addr;
    return true;
}

bool nvGspEnsure(void) {
    if (gCpu) return true;
    gConn = nvGspOpen();
    if (!gConn) return false;
    gVa = NVGSP_ARENA_BASE; // our arena is fresh; take page 0
    if (!nvGspStaging(gConn, &gHandle, &gCpu)) return false;
    NSLog(@"NVMTLGsp: staging va 0x%llx cpu %p handle %u", gVa, gCpu, gHandle);
    return true;
}

// ---------------------------------------------------------------- GPU heap
// Buffers are ordinary page-aligned process memory; the kext wires them and
// maps their 4 KiB pages at a VA of ours (selector 33, small page tables).
// The MTLBuffer wraps the same pages (NoCopy), so CPU and GPU share them.
#define NVGSP_USER_VA_BASE 0x2C00000000ULL
#define NVGSP_USER_VA_END  0x2F00000000ULL      // NVK's replay range starts here
enum { kSelUserMemBind = 33, kMaxUserBufs = 4096 };
typedef struct { uint8_t *cpu; uint64_t va, size; uint32_t handle; } NVUserBuf;
static NVUserBuf gBufs[kMaxUserBufs];
static unsigned gNBufs;
static uint64_t gUserVa = NVGSP_USER_VA_BASE;
static os_unfair_lock gHeapLock = OS_UNFAIR_LOCK_INIT;

// user VA is handed back on free. It used to only grow, and
// WindowServer (a buffer or surface view every frame) ran through the 12 GiB
// in a minute or two; after that new buffers had no GPU address, shaders got
// 0 and the GR exception + GPU reset showed as a black flash.
// Spans are 64 KiB granular; caller holds gHeapLock.
typedef struct { uint64_t va, size; } NVUSpan;
static NVUSpan gUFree[4096];
static unsigned gNUFree;
static uint64_t userVaTakeLocked(uint64_t len, uint64_t align) {
    len = (len + 0xffff) & ~0xffffULL;
    for (unsigned i = 0; i < gNUFree; i++) {
        const uint64_t a = (gUFree[i].va + align - 1) & ~(align - 1);
        const uint64_t lead = a - gUFree[i].va;
        if (gUFree[i].size < lead + len) continue;
        const NVUSpan f = gUFree[i];
        gUFree[i] = gUFree[--gNUFree];
        if (lead && gNUFree < 4096) gUFree[gNUFree++] = (NVUSpan){f.va, lead};
        if (f.size > lead + len && gNUFree < 4096) gUFree[gNUFree++] = (NVUSpan){a + len, f.size - lead - len};
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
    if (gNUFree < 4096) gUFree[gNUFree++] = (NVUSpan){va, len};
}

// the last user VA ranges handed back, to name an MMU fault after a
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
    const uint64_t va = (regU64(g, CFSTR("NVGspControl-rc-mmu-fault-hi")) << 32) | regU64(g, CFSTR("NVGspControl-rc-mmu-fault-lo"));
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
static void nvHeapReleaseVa(NVUserBuf u, uint64_t h) {
    callMethod(kSelMemFree, &h, 1, NULL, NULL, NULL, 0);
    os_unfair_lock_lock(&gHeapLock);
    noteFreed(u.va, u.size, (u.handle & 0x40000000u) ? 1 : 0);
    userVaGiveLocked(u.va, u.size);
    os_unfair_lock_unlock(&gHeapLock);
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
        IOConnectMapMemory(gConn, 0x1000u | handle, mach_task_self(), &addr, &msz, kIOMapAnywhere) != KERN_SUCCESS) {
        uint64_t h = handle;
        callMethod(kSelMemFree, &h, 1, NULL, NULL, NULL, 0);
        os_unfair_lock_lock(&gHeapLock); userVaGiveLocked(v, len); os_unfair_lock_unlock(&gHeapLock);
        return false;
    }
    os_unfair_lock_lock(&gHeapLock);
    gBufs[gNBufs++] = (NVUserBuf){(uint8_t *)(uintptr_t)addr, v, len, handle | 0x80000000u};
    os_unfair_lock_unlock(&gHeapLock);
    *cpu = (void *)(uintptr_t)addr; *va = v;
    return true;
}

bool nvHeapAlloc(uint64_t bytes, void **cpu, uint64_t *va) {
    if (!bytes || !nvGspEnsure()) return false;
    const char *m = getenv("NVMTL_HEAP");
    if (m && m[0] == '2') return heapAllocChunks(bytes, cpu, va);
    const uint64_t len = (bytes + 0xfff) & ~0xfffULL;
    mach_vm_address_t addr = 0;
    if (mach_vm_allocate(mach_task_self(), &addr, len, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) return false;
    os_unfair_lock_lock(&gHeapLock);
    const uint64_t v = gNBufs < kMaxUserBufs ? userVaTakeLocked(len, 0x10000) : 0;
    const bool room = v != 0;
    os_unfair_lock_unlock(&gHeapLock);
    uint64_t in[4] = {addr, len, v, 0}, out[1] = {0};
    uint32_t outN = 1;
    if (!room || !callMethod(kSelUserMemBind, in, 4, out, &outN, NULL, 0) || !out[0]) {
        if (!room) nvHeapFull(len);
        mach_vm_deallocate(mach_task_self(), addr, len);
        os_unfair_lock_lock(&gHeapLock); userVaGiveLocked(v, len); os_unfair_lock_unlock(&gHeapLock);
        return false;
    }
    os_unfair_lock_lock(&gHeapLock);
    gBufs[gNBufs++] = (NVUserBuf){(uint8_t *)(uintptr_t)addr, v, len, (uint32_t)out[0]};
    os_unfair_lock_unlock(&gHeapLock);
    *cpu = (void *)(uintptr_t)addr;
    *va = v;
    return true;
}

void nvHeapFree(void *cpu, uint64_t bytes) {
    (void)bytes;
    NVUserBuf u = {0};
    os_unfair_lock_lock(&gHeapLock);
    for (unsigned i = 0; i < gNBufs; i++)
        if (gBufs[i].cpu == cpu && !(gBufs[i].handle & 0x40000000u)) {   // never a wrapped surface
            u = gBufs[i]; gBufs[i] = gBufs[--gNBufs]; break;
        }
    os_unfair_lock_unlock(&gHeapLock);
    if (!u.cpu) return;
    uint64_t h = u.handle & 0x7fffffffu;
    nvHeapReleaseVa(u, h);
    if (u.handle & 0x80000000u) {                               // kext SYS object
        IOConnectUnmapMemory(gConn, 0x1000u | (uint32_t)h, mach_task_self(), (mach_vm_address_t)(uintptr_t)u.cpu);
        return;
    }
    mach_vm_deallocate(mach_task_self(), (mach_vm_address_t)(uintptr_t)u.cpu, u.size);
}

// handle bit 30: wrapped foreign memory (unbind only, the owner frees it)
bool nvHeapWrap(void *cpu, uint64_t bytes, uint64_t *va) {
    if (!cpu || !bytes || ((uintptr_t)cpu & 0xfff) || !nvGspEnsure()) return false;
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
    gBufs[gNBufs++] = (NVUserBuf){(uint8_t *)cpu, v, len, (uint32_t)out[0] | 0x40000000u};
    os_unfair_lock_unlock(&gHeapLock);
    *va = v;
    return true;
}

// by address AND GPU VA. CoreAnimation makes several textures on
// one IOSurface; each wrap has its own VA over the same pages, and taking
// the first entry with that CPU address unbound another texture's VA while
// the GPU still used it (MMU fault, PTE invalid, GPU reset at login).
void nvHeapUnwrap(void *cpu, uint64_t va) {
    NVUserBuf u = {0};
    os_unfair_lock_lock(&gHeapLock);
    for (unsigned i = 0; i < gNBufs; i++)
        if (gBufs[i].cpu == cpu && gBufs[i].va == va && (gBufs[i].handle & 0x40000000u)) {
            u = gBufs[i]; gBufs[i] = gBufs[--gNBufs]; break;
        }
    os_unfair_lock_unlock(&gHeapLock);
    if (!u.cpu) return;
    nvHeapReleaseVa(u, u.handle & 0x3fffffffu);
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

// kext objects, kept so a GPU reset can put them back
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

// block-linear surfaces get their own kext object, bound with the
// PTE kind the layout needs, in a VA range of their own
#define NVGSP_BL_VA_BASE 0x2880000000ULL
#define NVGSP_BL_VA_END  0x2A00000000ULL
static uint64_t gBlVa = NVGSP_BL_VA_BASE;

bool nvVramAllocKind(uint64_t bytes, uint32_t kind, uint64_t *va, uint32_t *handle) {
    if (!bytes || !nvGspEnsure()) return false;
    const uint64_t len = (bytes + 0x1fffff) & ~0x1fffffULL;
    uint64_t in[2] = {len, 0}, out[2] = {0, 0};
    uint32_t outN = 2;
    if (!callMethod(kSelMemAlloc, in, 2, out, &outN, NULL, 0) || !out[0]) return false;
    os_unfair_lock_lock(&gHeapLock);
    const uint64_t v = gBlVa;
    const bool room = v + len <= NVGSP_BL_VA_END;
    if (room) gBlVa += len;
    os_unfair_lock_unlock(&gHeapLock);
    uint64_t b[3] = {out[0], v, kind & 0xff};
    if (!room || !callMethod(kSelVaBindObj, b, 3, NULL, NULL, NULL, 0)) {
        uint64_t h = out[0];
        callMethod(kSelMemFree, &h, 1, NULL, NULL, NULL, 0);
        return false;
    }
    *va = v;
    *handle = (uint32_t)out[0];
    os_unfair_lock_lock(&gHeapLock);
    if (gNKObj < 1024) gKObj[gNKObj++] = (NVKindObj){v, len, kind, (uint32_t)out[0], (uint32_t)out[0]};
    os_unfair_lock_unlock(&gHeapLock);
    return true;
}

// callers keep the handle they got first (id); after a reset the kext
// object behind it is a new one
void nvVramFreeKind(uint32_t handle) {
    if (!handle) return;
    uint64_t h = handle;
    os_unfair_lock_lock(&gHeapLock);
    for (unsigned i = 0; i < gNKObj; i++)
        if (gKObj[i].id == handle) { h = gKObj[i].handle; gKObj[i] = gKObj[--gNKObj]; break; }
    os_unfair_lock_unlock(&gHeapLock);
    callMethod(kSelMemFree, &h, 1, NULL, NULL, NULL, 0);   // unbinds too; the VA is not reused
}

// is a VRAM-range VA inside a live allocation (fault dumps)
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

// any VA of ours: heap buffer, VRAM, block-linear, or nothing
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
    static os_unfair_lock rl = OS_UNFAIR_LOCK_INIT;
    static uint32_t n;
    os_unfair_lock_lock(&rl);
    if (gConn != dead) { os_unfair_lock_unlock(&rl); return true; }   // another thread did it
    // a GPU that stays down must not turn every call into a reopen
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
    unsigned nb = 0, nv = 0, nk = 0;
    // the caller may hold the heap lock (vramGrow under nvVramAlloc): then
    // this call fails and the next one recovers
    if (!os_unfair_lock_trylock(&gHeapLock)) {
        if (c) IOServiceClose(c);
        os_unfair_lock_unlock(&rl);
        return false;
    }
    for (unsigned i = 0; ok && i < gNBufs; i++) {          // process pages: bind them again
        NVUserBuf *u = &gBufs[i];
        if (u->handle & 0x80000000u) continue;              // NVMTL_HEAP=2 objects are not kept
        uint64_t in[4] = {(uint64_t)(uintptr_t)u->cpu, u->size, u->va, 0}, out[1] = {0};
        uint32_t outN = 1;
        if (callOn(c, kSelUserMemBind, in, 4, out, &outN, NULL, 0) || !out[0]) { ok = false; break; }
        u->handle = (uint32_t)out[0] | (u->handle & 0x40000000u);
        nb++;
    }
    for (unsigned i = 0; ok && i < gNVChunk; i++) {        // VRAM: new memory, same VA
        uint64_t in[2] = {gVChunk[i].size, 0}, out[2] = {0, 0};
        uint32_t outN = 2;
        if (callOn(c, kSelMemAlloc, in, 2, out, &outN, NULL, 0) || !out[0]) { ok = false; break; }
        uint64_t b[3] = {out[0], gVChunk[i].va, 0};
        if (callOn(c, kSelVaBindObj, b, 3, NULL, NULL, NULL, 0)) { ok = false; break; }
        gVChunk[i].handle = (uint32_t)out[0];
        nv++;
    }
    for (unsigned i = 0; ok && i < gNKObj; i++) {
        uint64_t in[2] = {gKObj[i].size, 0}, out[2] = {0, 0};
        uint32_t outN = 2;
        if (callOn(c, kSelMemAlloc, in, 2, out, &outN, NULL, 0) || !out[0]) { ok = false; break; }
        uint64_t b[3] = {out[0], gKObj[i].va, gKObj[i].kind & 0xff};
        if (callOn(c, kSelVaBindObj, b, 3, NULL, NULL, NULL, 0)) { ok = false; break; }
        gKObj[i].handle = (uint32_t)out[0];
        nk++;
    }
    if (ok) {
        gConn = c; gHandle = sh; gCpu = scpu;
        IOServiceClose(dead);                               // the old client's objects go
    }
    os_unfair_lock_unlock(&gHeapLock);
    if (!ok && c) IOServiceClose(c);
    ++n;
    if (ok || fails < 5 || !(fails % 100))
        NSLog(@"NVMTLGsp: GPU reset: %s (recovery %u: %u buffers, %u VRAM chunks, %u surfaces; VRAM contents are gone)",
              ok ? "reopened" : "could not reopen", n, nb, nv, nk);
    if (ok) { fails = 0; nextTry = 0; }
    else {
        mach_timebase_info_data_t tb; mach_timebase_info(&tb);
        const uint64_t ms = fails < 6 ? 100ull << fails : 5000;
        nextTry = mach_absolute_time() + ms * 1000000ull * tb.denom / tb.numer;
        fails++;
    }
    nvExplainFault();
    // command buffers out with the old connection never complete
    // through it; their slabs are ours again (NVAccelerator completes them)
    nvSlabsReleaseAll();
    os_unfair_lock_unlock(&rl);
    return ok;
}

bool nvVramAlloc(uint64_t bytes, uint64_t *va) {
    if (!bytes || !nvGspEnsure()) return false;
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
        if (!ok && (pass || !vramGrow(len))) break;
    }
    os_unfair_lock_unlock(&gHeapLock);
    return ok;
}

void nvVramFree(uint64_t va, uint64_t bytes) {
    const uint64_t len = (bytes + 0xffff) & ~0xffffULL;
    os_unfair_lock_lock(&gHeapLock);
    // merge with a neighbour when possible, else append
    bool merged = false;
    for (unsigned i = 0; i < gNVFree && !merged; i++) {
        if (gVFree[i].va + gVFree[i].size == va) { gVFree[i].size += len; merged = true; }
        else if (va + len == gVFree[i].va) { gVFree[i].va = va; gVFree[i].size += len; merged = true; }
    }
    if (!merged && gNVFree < 2048) gVFree[gNVFree++] = (NVRange){va, len};
    os_unfair_lock_unlock(&gHeapLock);
}

uint8_t *nvStageCpu(void) { return gCpu; }
uint64_t nvStageVa(void) { return gVa; }

// One 8-word fence tail is appended by the kext (submitSegments); the
// pushbuffer holds SET_OBJECT + copy chunks only.
// Submit w words from the push area on the CE and wait for them.
static bool ceSubmit(uint32_t w) {
    nvGrFlush();                                   // queued launches first, in order
    nvNativeDrain();                               // and native GR work (other ring)
    struct { uint64_t va; uint32_t dw; uint32_t pad; } seg = {gVa + NVGSP_STAGE_PUSH, w, 0};
    uint64_t in[1] = {1}; // engine 1 = CE
    uint64_t out[1] = {0};
    uint32_t outN = 1;
    if (!callMethod(kSelExecSeg, in, 1, out, &outN, &seg, sizeof(seg)) || !out[0]) {
        NSLog(@"NVMTLGsp: execSegments failed"); return false;
    }
    uint64_t wi[2] = {out[0], 2000000}, wo[1] = {0};
    outN = 1;
    if (!callMethod(kSelCeWait, wi, 2, wo, &outN, NULL, 0)) {
        NSLog(@"NVMTLGsp: ceFenceWait failed"); return false;
    }
    return true;
}

// Any size: 128 KiB lines like NVK; when the push area fills up we submit
// and keep going (zero-copy buffers are no longer capped by staging).
bool nvCeCopy(uint64_t dstVa, uint64_t srcVa, uint64_t bytes) {
    NV_GPU_LOCKED;
    if (!gCpu || !bytes) return false;
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
    nvNativeDrain();                                // and command buffers still out with it
    if (warp < 0x2000) warp = 0x2000;               // 256 B per lane at least, fewer regrows
    const uint64_t tpc = (warp * 48 * 2 + 0x7fff) & ~0x7fffULL;
    const uint64_t size = (tpc * NVSLM_TPCS + 0x1ffff) & ~0x1ffffULL;
    uint64_t va; uint32_t h;
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

// / 0.8.12: what a stage's constant buffer pointers point at. A
// launch that never ends or faults reads through one of these; print the
// first words of every user-arena pointer that lands in a live buffer, and
// say which do not (a freed or never-bound VA is the MMU fault).
static char gLastLabel[128];                     // the launch now being built
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

bool nvGrLaunch(const uint32_t *code, uint32_t codeWords, uint32_t regs, uint32_t slm,
                uint32_t smem, uint32_t barriers,
                const uint32_t *push, uint32_t npush,
                const uint32_t grid[3], const uint32_t block[3]) {
    NV_GPU_LOCKED;
    nvGrFlush();
    if (!gCpu || !codeWords || codeWords * 4 + 1024 > 0x10000) return false;
    if (!grid[0] || !block[0] || !slmEnsure(slm)) return false;
    // Code (+1 KiB zero pad, as nvrun), cbuf, QMD.
    uint8_t *cd = gCpu + NVGSP_GR_CODE;
    memcpy(cd, code, codeWords * 4);
    memset(cd + codeWords * 4, 0, 1024);
    uint32_t cb[256] = {0};
    if (npush > 256) return false;
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
    uint64_t in[1] = {0}; // engine 0 = GR
    uint64_t out[1] = {0};
    uint32_t outN = 1;
    const uint64_t ta = mach_absolute_time();
    if (!callMethod(kSelExecSeg, in, 1, out, &outN, &seg, sizeof(seg)) || !out[0]) {
        NSLog(@"NVMTLGsp: GR execSegments failed"); return false;
    }
    const uint64_t tb0 = mach_absolute_time();
    uint64_t wi[2] = {out[0], 2000000}, wo[1] = {0};
    outN = 1;
    if (IOConnectCallMethod(gConn, 18, wi, 2, NULL, 0, wo, &outN, NULL, NULL)) {
        NSLog(@"NVMTLGsp: GR fenceWait failed");
        static uint32_t dumps;
        if (dumps++ < 4) {   // name the launch that never finished
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

// batched compute. Every nvGrLaunch is a kernel round trip
// (execSegments + fence wait, ~30 us each, 13 us of it the submit alone),
// so a command buffer of 1000 small dispatches took 30 ms. Between
// nvGrBatchBegin/End, nvGrQueue appends the launch to one method stream in
// a 4 MiB part of the staging object (code once per kernel, cbuf + QMD per launch)
// and nvGrFlush submits the lot with one fence. Metal's serial dispatch
// order is kept with WAIT_FOR_IDLE + shader cache invalidates between
// launches. Anything that submits on its own, or reads memory on the CPU,
// flushes first.
// the batch goes to one of two places. The shared area at 2 MiB
// (4 MiB, 512 KiB of methods) for batches we submit and wait for; or a
// native slab at 6 MiB + 2 MiB * n (256 KiB of methods) that belongs to a
// command buffer until the family completes it (asynchronous commit).
#define NVB_SLABS 8
#define NVB_SLAB_BYTES (2u << 20)
static uint32_t NVB_BYTES = 4u << 20, NVB_PUSH_MAX = 512u << 10;
static int gBSlab = -1;                            // slab the open batch lives in, -1 shared area
static bool nvNativeDrainShared;                    // the shared area is out with a command buffer
static uint32_t gSlabBusy;                          // bit n: slab n owned by a command buffer
static pthread_mutex_t gSlabMtx = PTHREAD_MUTEX_INITIALIZER;   // never the GPU lock: completion
static pthread_cond_t gSlabCond = PTHREAD_COND_INITIALIZER;    // handlers take only this one
// what each command buffer that owns a slab carries, so a stuck one
// can be named: launch labels (kernel / draw shaders), stream VA and size
static char gSlabLabels[NVB_SLABS + 1][512];
static uint64_t gSlabVa[NVB_SLABS + 1];
static uint32_t gSlabWords[NVB_SLABS + 1];
static char gBLabels[512];                       // the open batch
void nvGrLabel(const char *what) {
    snprintf(gLastLabel, sizeof gLastLabel, "%s", what);
    const size_t n = strlen(gBLabels);
    if (n + strlen(what) + 2 < sizeof gBLabels) { if (n) strcat(gBLabels, " "); strcat(gBLabels, what); }
}
static void nvSlabsReleaseAll(void) {
    pthread_mutex_lock(&gSlabMtx);
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

void nvGrBatchBegin(void) { nvGpuLock(); gBDepth++; }

// the native path (N1/N2) is on by default when the accelerator
// runs it (NVAccelerator-native in the registry). Off with NVMTL_NATIVE=0,
// or for every process (WindowServer too) with the file
// /Library/Preferences/nvmtl-native-off.
bool nvNativeEnabled(void) {
    static int on = -1;
    if (on >= 0) return on;
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

// wait until every command buffer that owns a slab has
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
// batching is opt-in (NVMTL_BATCH=1) or comes with the native path
bool nvBatchOn(void) {
    static int on = -1;
    if (on < 0) on = getenv("NVMTL_BATCH") != NULL || nvNativeEnabled();
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
// end a batch whose launches went to the family (nvGrFlushNative);
// the GPU lock stays held until nvGrNativeDone + nvGpuUnlock
void nvGrBatchEnd(void) {
    if (gBDepth && !--gBDepth) { nvGrFlush(); if (gBSlab >= 0) batchToSharedArea(); }
    nvGpuUnlock();
}

// hand the queued launches to the family instead of
// submitting them ourselves. One vendor kernel command (type 0x10000, as
// AppleParavirtCommandQueue's ExecIndirect) goes into the command buffer's
// segment; NVAccelerator's queue submits it with a stamp and the family
// completes the command buffer when the GPU releases that stamp. Returns
// false (nothing written) when there is nothing queued or no room, and the
// caller flushes the old way.
// + the tag of our NVGP client (selector 'NVT'), so the kernel runs
// it in this connection's VA arena even when the process has several
typedef struct { uint32_t type, size, engine, n, tag, reserved; uint64_t va; uint32_t dwords, flags; } NVExecKCmd;
static uint32_t connTag(void) {
    static io_connect_t forConn;
    static uint32_t tag;
    if (forConn != gConn) {
        uint64_t out[1] = {0};
        uint32_t n = 1;
        tag = IOConnectCallScalarMethod(gConn, 0x4E5654, NULL, 0, out, &n) == KERN_SUCCESS ? (uint32_t)out[0] : 0;
        forConn = gConn;
        if (!tag) NSLog(@"NVMTLGsp: no client tag, native submission off for this connection");
    }
    return tag;
}
bool nvGrFlushNative(id cb) {
    NV_GPU_LOCKED;
    if (!gBN) return false;
    // The family only runs kernel commands inside a segment
    // (IOAccelCommandQueue::processCommandBuffer walks the segment list):
    // MTLIOAccelCommandBufferStorageBeginSegment takes the kernel command
    // pointer the segment starts at, EndSegment closes it at the current one.
    const uint32_t tag = connTag();
    if (!tag) return false;
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
    // the slab (or, with every slab busy, the shared area) now
    // belongs to this command buffer until the family completes it
    const uint32_t bit = gBSlab >= 0 ? 1u << gBSlab : 1u << 31;
    {
        const int si = gBSlab >= 0 ? gBSlab : NVB_SLABS;
        snprintf(gSlabLabels[si], sizeof gSlabLabels[si], "%s", gBLabels);
        gSlabVa[si] = gBVa; gSlabWords[si] = gBPush;
    }
    pthread_mutex_lock(&gSlabMtx);
    gSlabBusy |= bit;
    pthread_mutex_unlock(&gSlabMtx);
    [(id<MTLCommandBuffer>)cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
        (void)c;
        pthread_mutex_lock(&gSlabMtx);
        gSlabBusy &= ~bit;
        pthread_cond_broadcast(&gSlabCond);
        pthread_mutex_unlock(&gSlabMtx);
    }];
    gBPush = gBData = gBN = gBNCode = 0;
    gBLabels[0] = 0;
    if (gBSlab >= 0) batchToSharedArea();
    else nvNativeDrainShared = true;
    return true;
}
// after the family completed the command buffer: the batch area is free


bool nvGrFlush(void) {
    if (!gBN) return true;
    NV_GPU_LOCKED;
    const uint32_t words = gBPush, n = gBN;
    gBPush = gBData = gBN = gBNCode = 0;
    gBLabels[0] = 0;          // the area is free again whatever happens
    struct { uint64_t va; uint32_t dw; uint32_t pad; } seg = {gBVa, words, 0};
    uint64_t in[1] = {0}, out[1] = {0};
    uint32_t outN = 1;
    const uint64_t ta = mach_absolute_time();
    if (!callMethod(kSelExecSeg, in, 1, out, &outN, &seg, sizeof(seg)) || !out[0]) {
        NSLog(@"NVMTLGsp: GR batch of %u launches: execSegments failed", n); return false;
    }
    const uint64_t tb0 = mach_absolute_time();
    uint64_t wi[2] = {out[0], 2000000}, wo[1] = {0};
    outN = 1;
    if (IOConnectCallMethod(gConn, 18, wi, 2, NULL, 0, wo, &outN, NULL, NULL)) {
        NSLog(@"NVMTLGsp: GR batch of %u launches: fenceWait failed", n); return false;
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
    NV_GPU_LOCKED;
    const bool on = nvBatchOn();
    if (gBSlab < 0 && nvNativeDrainShared && !gBN) {   // shared area still owned by a command buffer
        nvNativeDrain(); nvNativeDrainShared = false;
    }
    if (!gBDepth || !on)
        return nvGrLaunch(code, codeWords, regs, slm, smem, barriers, push, npush, grid, block);
    if (!gCpu || !codeWords || codeWords * 4 + 1024 > 0x10000 || npush > 256) return false;
    if (!grid[0] || !block[0] || !slmEnsure(slm)) return false;   // may flush
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
        pb[c++] = (0x4u << 29) | (1u << 13) | (0x110u >> 2);   // IMMD WAIT_FOR_IDLE: serial dispatch order
    }
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

// ---------------------------------------------------------------- 3D
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

// what a draw that hung or faulted the GPU was made of: the shader
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

// inside a batch a draw is appended to the batch's method
// stream (cbufs in the batch's data area) like a compute launch, so a
// command buffer of draws and dispatches is one submission; the family
// completes it on the native path. Outside a batch it is submitted and
// waited for as before.
bool nvGr3DDraw(const NV3DDraw *d) {
    NV_GPU_LOCKED;
    // draws stay out of the batch unless NVMTL_DRAWBATCH=1 or the file
    // /Library/Preferences/nvmtl-drawbatch: every native login hang (28 Sep,
    // attempts 1-6) was a WindowServer batch of draws, the login blur chain
    // (downsample_blur / narrow_blur passes sampling the previous pass)
    static int drawBatch = -1;
    if (drawBatch < 0) {
        const char *e = getenv("NVMTL_DRAWBATCH");
        drawBatch = e ? atoi(e) != 0 : access("/Library/Preferences/nvmtl-drawbatch", F_OK) == 0;
    }
    const bool batched = gBDepth && nvBatchOn() && drawBatch;
    if (!batched) nvGrFlush();
    if (!gCpu || !d->nrt || d->nrt > 8) return false;
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
    if (batched && gBN) pb[c++] = (0x4u << 29) | (0u << 13) | (0x110u >> 2);   // IMMD WAIT_FOR_IDLE after earlier work
    pb[c++] = M3(0x0, 1); pb[c++] = 0xC997;                              // SET_OBJECT ADA_A, subch 0
    memcpy(pb + c, kInit3D, sizeof kInit3D);
    c += sizeof kInit3D / 4;
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
    uint32_t sel = d->nrt;                                              // CT_SELECT: count, then target ids
    for (uint32_t i = 0; i < d->nrt; ++i) sel |= i << (4 + 3 * i);
    P1(0x121c, sel);
    const int zdbg = getenv("NVMTL_ZDBG") ? atoi(getenv("NVMTL_ZDBG")) : 0;
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
    // multisampling (NIL sample layouts; one shading pass)
    P1(0x15d0, d->aaMode);                                              // ANTI_ALIAS samples mode
    P1(0x1534, d->aaMode ? 1 : 0);                                      // ANTI_ALIAS_ENABLE
    P1(0x0754, 0x1);                                                    // HYBRID_ANTI_ALIAS_CONTROL: 1 pass
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
    // write masks first: a clear writes only the enabled channels
    for (uint32_t i = 0; i < d->nrt; ++i) P1(0x1a00 + i * 4, d->rt[i].writeMask ? d->rt[i].writeMask : 0x1111);
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
        P1(0x19d0, 0x3c | (i << 6));                                    // CLEAR_SURFACE RGBA, target i
    }
    // NVMTL_3D_STOP=n cuts the stream after phase n (1 clears, 2 fixed
    // state, 3 programs, 4 vertex fetch) to find a method the class refuses
    const int stop = getenv("NVMTL_3D_STOP") ? atoi(getenv("NVMTL_3D_STOP")) : 99;
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
        // stencil, front + two-sided back
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
        P1(0x1434, (uint32_t)d->baseVertex);                            // GLOBAL_BASE_VERTEX_INDEX
        P1(0x1438, d->baseInstance);                                    // GLOBAL_BASE_INSTANCE_INDEX
        if (d->indexed) {
            pb[c++] = M3(0x17c8, 2); pb[c++] = (uint32_t)(d->indexVa >> 32); pb[c++] = (uint32_t)d->indexVa;
            pb[c++] = M3(0x0238, 2); pb[c++] = 0; pb[c++] = (uint32_t)d->indexBytes;   // INDEX_BUFFER_SIZE
            P1(0x1644, 0);                                              // no primitive restart
            P1(0x17d8, d->indexSize);                                   // INDEX_BUFFER_E: index size
        }
        P1(0x0220, d->instances ? d->instances : 1);                    // SET_INSTANCE_COUNT
        P1(0x1618, d->topology | (1u << 31));                           // BEGIN, instance iterate
        if (d->indexed) { pb[c++] = M3(0x17dc, 2); pb[c++] = d->first; pb[c++] = d->count; }
        else { pb[c++] = M3(0x0d74, 2); pb[c++] = d->first; pb[c++] = d->count; }
        P1(0x1614, 0);                                                  // END
    }
submit:
    // release a semaphore after all writes, with the flush on: ROP
    // writes still in the colour caches otherwise miss a copy engine readback
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
    if (getenv("NVMTL_TRACE"))
        NSLog(@"NVMTL_TRACE 3d: rt0 va 0x%llx pitch %u %ux%u fmt 0x%x clear %d, vs 0x%llx fs 0x%llx, %u words",
              d->rt[0].va, d->rt[0].pitch, d->rt[0].w, d->rt[0].h, d->rt[0].format, d->rt[0].clear,
              d->vs.va, d->fs.va, c);
    struct { uint64_t va; uint32_t dw; uint32_t pad; } seg = {gVa + NVGSP_GR_PUSH, c, 0};
    uint64_t in[1] = {0}, out[1] = {0};
    uint32_t outN = 1;
    if (!callMethod(kSelExecSeg, in, 1, out, &outN, &seg, sizeof(seg)) || !out[0]) {
        NSLog(@"NVMTLGsp: 3D execSegments failed"); return false;
    }
    uint64_t wi[2] = {out[0], 2000000}, wo[1] = {0};
    outN = 1;
    if (IOConnectCallMethod(gConn, 18, wi, 2, NULL, 0, wo, &outN, NULL, NULL)) {
        NSLog(@"NVMTLGsp: 3D fenceWait failed"); nvGr3DDump(d); return false;
    }
    return true;
}
