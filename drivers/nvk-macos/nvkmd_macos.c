/*
 * nvkmd_macos: NVK kernel-mode-driver backend for macOS on top of the
 * NVGspControl kext user client (RTX 4080 / AD103, GSP-RM r570.144).
 *
 * Kernel interface (NVGspControl >= 0.107.0 user-client selectors):
 *    8 peek BAR0 {offset, count}        (PTIMER for GPU timestamps)
 *   17 GR submit async (kernel-built)   18 GR fence wait {seq, timeout_us}
 *   20 vaBind {va, phys, bytes (0 = unbind 2 MiB), flags}
 *   21 execSegments {engine} + n x {u64 va, u32 dwords, u32 flags} -> seq
 *   22 memAlloc {bytes, domain 0 VRAM / 1 SYS / 2 CPU-visible VRAM (0.123.0)}
 *      -> {handle, phys}
 *   23 memFree {handle}        24 vaBindObject {handle, va, kind}
 *   25 vaUnbind {va, bytes}    26 memInfo -> {heap, vram used, sys used}
 *      (25/26: 0.109.0; older kexts fall back to per-page 20 / local counts)
 *   27 present {handle, offset, pitch, width, height, format}  (0.110.0)
 *   28 presentStop: window 0 back to the desktop surface       (0.110.0)
 *   29 CE fence wait {seq, timeout_us}                          (0.112.0)
 *   map type 0x1000 | handle   = CPU mapping of a SYS object
 *
 * Model: all engines (3D, compute, copy, 2D) share the kext's single GR
 * channel, so submissions execute in order and GPU-side waits on our own
 * fences are implicit; CPU waits poll the GR fence. Memory and VA granule
 * is 2 MiB (kernel PTE size). Device-local memory is not CPU-mappable (no
 * ReBAR/BAR1 path yet), so CAN_MAP / GART allocations live in system memory.
 */
#include "nvkmd/nvkmd.h"
#include "nvkmd_macos_submit.h"
#include "nvk_entrypoints.h"
#include "nvk_device.h"

#include "vk_device.h"
#include "vk_log.h"
#include "vk_sync.h"
#include "vk_sync_timeline.h"
#include "util/list.h"
#include "util/os_time.h"
#include "util/u_atomic.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "util/vma.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>

/* NVKMD_MACOS_TRACE=1: every VA alloc/bind/unbind/free and exec to stderr */
static int macos_trace = -1;
#define MTRACE(...) do { \
   if (macos_trace < 0) macos_trace = getenv("NVKMD_MACOS_TRACE") != NULL; \
   if (macos_trace) fprintf(stderr, "[nvkmd_trace] " __VA_ARGS__); } while (0)
#define MACOS_ERR(obj, err) (fprintf(stderr, "[nvkmd_macos] %s:%d -> %d\n", __func__, __LINE__, (int)(err)), vk_error(obj, err))

VkResult nvkmd_macos_try_create_pdev(struct vk_object_base *log_obj,
                                     enum nvk_debug debug_flags,
                                     struct nvkmd_pdev **pdev_out);
VkResult nvkmd_macos_mem_present(struct nvkmd_mem *mem, uint64_t offset_B,
                                 uint32_t pitch_B, uint32_t width,
                                 uint32_t height, uint32_t wnd_format);
void nvkmd_macos_present_stop(struct nvkmd_dev *dev);
bool nvkmd_macos_has_dgc(struct nvkmd_pdev *pdev);

#define NVKMD_MACOS_ARENA_BASE 0x2800000000ull
#define NVKMD_MACOS_ARENA_END  0x3000000000ull
/* Last 4 GiB of the arena: capture/replay (fixed-address) allocations, like
 * nvkmd_nouveau's replay heap, so replayed addresses never collide with
 * ordinary allocations. */
#define NVKMD_MACOS_REPLAY_BASE 0x2F00000000ull
#define NVKMD_MACOS_GRAN       0x200000ull
/* The kext caps one fence wait at 5 s; longer waits loop. */
#define NVKMD_MACOS_WAIT_CHUNK_US 5000000ull

struct nvkmd_macos_pdev {
   struct nvkmd_pdev base;
   io_connect_t conn;
   io_service_t svc;         /* NVGspControl, for RC/fault properties */
   uint32_t lost_reported;   /* device-lost diagnostics printed once */
   uint64_t bind_gran;       /* 64 KiB with kext ABI >= 114, else 2 MiB */
   uint32_t exec_segment_flags; /* supported selector-21 per-segment flags */
   bool has_mem_info;        /* kext >= 0.109.0 (selectors 25/26) */
   bool use_ce;              /* NVK_MACOS_CE=1: copy-only contexts on the CE ring */
   uint64_t vram_cpu_B;      /* kext >= 0.123.0: VRAM reachable through BAR1 */
   /* NVK_MACOS_GPU_WAIT=1 (kext >= 0.128.0): a GR <-> CE dependency is a
    * host semaphore acquire on the other ring's fence, not a CPU wait */
   bool gpu_wait;
   uint64_t sem_va[2];       /* GR, CE fence semaphore GPU VAs */
   uint64_t vram_used_local; /* fallback: this process's VRAM objects */
   /* The kext keeps one GPU VA space per client (conn) and the conn belongs
    * to the pdev, so every VkDevice made from it shares that space and the
    * VA heaps live here. With heaps per device, a second device (CTS makes
    * them all the time) handed out the same addresses again, its binds
    * overwrote the first device's PTEs and its frees unmapped them (Xid 31
    * in the next test on the first device). */
   simple_mtx_t va_lock;
   struct util_vma_heap heap;         /* [ARENA_BASE, REPLAY_BASE) */
   struct util_vma_heap replay_heap;  /* [REPLAY_BASE, ARENA_END) */
   struct vk_sync_type sync_type;
   struct vk_sync_timeline_type timeline_type;
   const struct vk_sync_type *sync_types[3];
};

struct nvkmd_macos_dev {
   struct nvkmd_dev base;
   io_connect_t conn;
   struct nvkmd_macos_pdev *vpdev;    /* owner of the VA heaps */
   uint64_t gran;                     /* arena bind granularity (pdev->bind_gran) */
   simple_mtx_t slab_lock;
   struct list_head slabs;            /* host-visible slabs */
   struct list_head vram_slabs;       /* VRAM slabs (kext ABI >= 114) */
};

/* Small host-visible allocations are carved out of 2 MiB system-memory
 * slabs: each kernel object is 2 MiB of wired, physically contiguous
 * memory and the kext has a fixed object table, so one object per
 * VkDeviceMemory would exhaust both long before maxMemoryAllocationCount.
 * VRAM allocations are suballocated too once the kext binds at 64 KiB
 * (ABI >= 114), 64 KiB aligned so kinded image rebinds still work. */
#define NVKMD_MACOS_SUB_MAX   (1u << 20)
#define NVKMD_MACOS_SUB_ALIGN 4096u
/* util_vma_heap cannot return 0; offsets live above this tag. */
#define NVKMD_MACOS_SLAB_TAG  (1ull << 32)

struct nvkmd_macos_slab {
   struct list_head link;
   struct list_head *list;            /* dev->slabs or dev->vram_slabs */
   bool sys;
   struct nvkmd_macos_mem *mem;       /* the 2 MiB kernel object */
   void *map;                         /* its CPU mapping */
   struct util_vma_heap heap;         /* [TAG, TAG + 2 MiB) */
   uint32_t users;
};

struct nvkmd_macos_mem {
   struct nvkmd_mem base;
   uint32_t handle;
   uint64_t phys;
   bool sys;
   bool cpu_vram;                     /* VRAM object mapped through BAR1 */
   struct nvkmd_macos_slab *slab;     /* suballocation: slab + offset */
   uint64_t slab_off;
};

struct nvkmd_macos_va {
   struct nvkmd_va base;
   bool sub;                          /* view into a slab's VA: nothing to unbind */
};

/* Kernel engines (execSegments engine index). */
enum nvkmd_macos_engine {
   NVKMD_MACOS_GR = 0,
   NVKMD_MACOS_CE = 1,
};

#define NVKMD_MACOS_ACQ_MAX   8
#define NVKMD_MACOS_ACQ_SLOTS 1024     /* 64 B push slots in a 64 KiB buffer */

struct nvkmd_macos_ctx {
   struct nvkmd_ctx base;
   uint8_t engine;          /* enum nvkmd_macos_engine */
   uint32_t last_seq;
   /* pending GPU-side waits (pdev->gpu_wait), pushed ahead of the next exec */
   uint32_t n_acq;
   struct { uint64_t va; uint32_t seq; } acq[NVKMD_MACOS_ACQ_MAX];
   struct nvkmd_mem *acq_mem;          /* CPU-written push slots */
   uint32_t acq_next;
   uint32_t acq_slot_seq[NVKMD_MACOS_ACQ_SLOTS];   /* fence that last used a slot */
   bool gr_state;           /* 3D/compute on GR: owns class state there */
};

/* Last 3D/compute context that executed on the GR channel (this process);
 * GR_STATE_GONE once that one was destroyed. */
static struct nvkmd_macos_ctx *g_gr_state_owner;
#define GR_STATE_GONE ((struct nvkmd_macos_ctx *)(uintptr_t)1)

bool
nvkmd_macos_ctx_state_lost(struct nvkmd_ctx *_ctx)
{
   struct nvkmd_macos_ctx *ctx = container_of(_ctx, struct nvkmd_macos_ctx, base);
   struct nvkmd_macos_ctx *owner = __atomic_load_n(&g_gr_state_owner, __ATOMIC_ACQUIRE);
   if (!ctx->gr_state)
      return false;
   if (owner != NULL && owner != ctx)
      return true;
   /* kext >= 0.145.0 (selector 36): did another client (another process,
    * a Metal app) submit to GR since our last submission? */
   struct nvkmd_macos_dev *dev = container_of(_ctx->dev, struct nvkmd_macos_dev, base);
   uint64_t other = 0;
   uint32_t cnt = 1;
   if (IOConnectCallMethod(dev->conn, 36, NULL, 0, NULL, 0, &other, &cnt, NULL, NULL))
      return false;
   return other != 0;
}

/* A binary sync backed by a GR or CE fence sequence number. */
struct nvkmd_macos_sync {
   struct vk_sync base;
   uint32_t seq;        /* 0: not submitted */
   uint8_t engine;      /* ring that seq belongs to */
   bool signaled;       /* CPU-signaled */
   bool timeline;       /* created VK_SYNC_IS_TIMELINE (NVK push_stream) */
   /* Timeline use (NVK's mem streams): value of the last signal, reached
    * when its seq completes (rings run in order), and the highest value
    * known to be reached. */
   uint64_t tl_value;
   uint64_t tl_done;
};

/* The sync type is binary, but NVK's per-queue push_stream creates it as a
 * timeline and, on a queue that never pushed, waits for point 0 at destroy.
 * Point 0 of a timeline is always reached. */
static inline bool
macos_sync_trivial(const struct nvkmd_macos_sync *s, uint64_t wait_value)
{
   return s->timeline && wait_value == 0;
}

static io_connect_t
nvkmd_macos_conn(struct vk_device *vk_dev);
static void
nvkmd_macos_report_device_lost(void);

/* ---------------------------------------------------------------- kext */

static VkResult
kext_fence_wait_once(io_connect_t conn, uint8_t engine, uint32_t seq, uint64_t timeout_us)
{
   uint64_t in[2] = { seq, MIN2(timeout_us, NVKMD_MACOS_WAIT_CHUNK_US) }, out = 0;
   uint32_t cnt = 1;
   kern_return_t kr = IOConnectCallMethod(conn, engine == NVKMD_MACOS_CE ? 29 : 18,
                                          in, 2, NULL, 0, &out, &cnt, NULL, NULL);
   if (kr == kIOReturnTimeout)
      return VK_TIMEOUT;
   if (kr)
      nvkmd_macos_report_device_lost();
   return kr ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
}

/* Wait for fence `seq` of `engine` until abs_timeout_ns (UINT64_MAX = forever). */
static VkResult
kext_fence_wait(io_connect_t conn, uint8_t engine, uint32_t seq, uint64_t abs_timeout_ns)
{
   for (;;) {
      const uint64_t now = os_time_get_nano();
      const uint64_t us = abs_timeout_ns > now ? (abs_timeout_ns - now) / 1000 : 0;
      VkResult r = kext_fence_wait_once(conn, engine, seq, us);
      if (r != VK_TIMEOUT || us <= NVKMD_MACOS_WAIT_CHUNK_US)
         return r;
   }
}

static bool
kext_peek(io_connect_t conn, uint32_t offset, uint32_t *value)
{
   uint64_t in[2] = { offset, 1 };
   size_t size = sizeof(*value);
   return IOConnectCallMethod(conn, 8, in, 2, NULL, 0, NULL, NULL,
                              value, &size) == KERN_SUCCESS && size == 4;
}

static bool
kext_mem_info(io_connect_t conn, uint64_t *heap_B, uint64_t *vram_used_B)
{
   uint64_t out[3] = { 0, 0, 0 };
   uint32_t cnt = 3;
   if (IOConnectCallMethod(conn, 26, NULL, 0, NULL, 0, out, &cnt, NULL, NULL))
      return false;
   if (heap_B)
      *heap_B = out[0];
   if (vram_used_B)
      *vram_used_B = out[1];
   return true;
}

static bool
kext_va_bind(io_connect_t conn, uint64_t va, uint64_t phys, uint64_t bytes,
             uint32_t flags)
{
   uint64_t in[4] = { va, phys, bytes, flags };
   return IOConnectCallMethod(conn, 20, in, 4, NULL, 0, NULL, NULL,
                              NULL, NULL) == KERN_SUCCESS;
}

/* Unbind whole 2 MiB pages [va, va + bytes): one call on kext >= 0.109.0,
 * one selector-20 call per page before that. */
static bool
kext_va_unbind(io_connect_t conn, uint64_t va, uint64_t bytes)
{
   if (!bytes)
      return true;
   uint64_t in[2] = { va, bytes };
   kern_return_t kr = IOConnectCallMethod(conn, 25, in, 2, NULL, 0, NULL, NULL,
                                          NULL, NULL);
   if (kr != kIOReturnUnsupported)
      return kr == KERN_SUCCESS;
   bool ok = true;
   for (uint64_t a = va; a < va + bytes; a += NVKMD_MACOS_GRAN)
      ok = kext_va_bind(conn, a, 0, 0, 0) && ok;
   return ok;
}

/* ---------------------------------------------------------------- sync */

static VkResult
macos_sync_init(struct vk_device *device, struct vk_sync *sync, uint64_t v)
{
   struct nvkmd_macos_sync *s = container_of(sync, struct nvkmd_macos_sync, base);
   s->seq = 0;
   s->signaled = v != 0;
   s->timeline = sync->flags & VK_SYNC_IS_TIMELINE;
   return VK_SUCCESS;
}

static void
macos_sync_finish(struct vk_device *device, struct vk_sync *sync) { }

static VkResult
macos_sync_signal(struct vk_device *device, struct vk_sync *sync, uint64_t v)
{
   struct nvkmd_macos_sync *s = container_of(sync, struct nvkmd_macos_sync, base);
   s->signaled = true;
   return VK_SUCCESS;
}

static VkResult
macos_sync_reset(struct vk_device *device, struct vk_sync *sync)
{
   struct nvkmd_macos_sync *s = container_of(sync, struct nvkmd_macos_sync, base);
   s->signaled = false;
   s->seq = 0;
   return VK_SUCCESS;
}

static VkResult
macos_sync_move(struct vk_device *device, struct vk_sync *dst, struct vk_sync *src)
{
   struct nvkmd_macos_sync *d = container_of(dst, struct nvkmd_macos_sync, base);
   struct nvkmd_macos_sync *s = container_of(src, struct nvkmd_macos_sync, base);
   d->seq = s->seq;
   d->engine = s->engine;
   d->signaled = s->signaled;
   d->tl_value = s->tl_value;
   d->tl_done = s->tl_done;
   s->seq = 0;
   s->signaled = false;
   return VK_SUCCESS;
}

/* Seq numbers wrap; a precedes b if the signed distance is negative. */
static inline bool
seq_before(uint32_t a, uint32_t b)
{
   return (int32_t)(a - b) < 0;
}

static VkResult
macos_sync_wait_one(io_connect_t conn, struct nvkmd_macos_sync *s, uint64_t wait_value,
                    enum vk_sync_wait_flags wait_flags, uint64_t abs_timeout_ns)
{
   for (;;) {
      if (s->signaled || macos_sync_trivial(s, wait_value) ||
          (s->timeline && wait_value <= s->tl_done))
         return VK_SUCCESS;
      if (s->seq) {
         if (wait_flags & VK_SYNC_WAIT_PENDING)
            return VK_SUCCESS;
         VkResult r = kext_fence_wait(conn, s->engine, s->seq, abs_timeout_ns);
         if (r == VK_SUCCESS) {
            s->signaled = true;
            s->tl_done = s->tl_value;
         }
         return r;
      }
      /* wait-before-submit: poll until a queue submission attaches it */
      if (os_time_get_nano() >= abs_timeout_ns)
         return VK_TIMEOUT;
      os_time_sleep(50);
   }
}

static VkResult
macos_sync_wait_many(struct vk_device *device, uint32_t wait_count,
                     const struct vk_sync_wait *waits,
                     enum vk_sync_wait_flags wait_flags,
                     uint64_t abs_timeout_ns)
{
   io_connect_t conn = nvkmd_macos_conn(device);
   if (!(wait_flags & VK_SYNC_WAIT_ANY)) {
      for (uint32_t i = 0; i < wait_count; i++) {
         struct nvkmd_macos_sync *s =
            container_of(waits[i].sync, struct nvkmd_macos_sync, base);
         VkResult r = macos_sync_wait_one(conn, s, waits[i].wait_value,
                                          wait_flags, abs_timeout_ns);
         if (r != VK_SUCCESS)
            return r;
      }
      return VK_SUCCESS;
   }

   /* WAIT_ANY: each engine's ring is in order, so per engine the oldest
    * submitted fence completes first; wait on those in short slices so CPU
    * signals and late submissions of the other syncs are noticed. */
   for (;;) {
      struct nvkmd_macos_sync *oldest[2] = { NULL, NULL };
      for (uint32_t i = 0; i < wait_count; i++) {
         struct nvkmd_macos_sync *s =
            container_of(waits[i].sync, struct nvkmd_macos_sync, base);
         if (s->signaled || macos_sync_trivial(s, waits[i].wait_value))
            return VK_SUCCESS;
         if (s->seq) {
            if (wait_flags & VK_SYNC_WAIT_PENDING)
               return VK_SUCCESS;
            struct nvkmd_macos_sync **o = &oldest[s->engine & 1];
            if (!*o || seq_before(s->seq, (*o)->seq))
               *o = s;
         }
      }
      const uint64_t now = os_time_get_nano();
      const bool expired = now >= abs_timeout_ns;
      const uint64_t slice_us = expired ? 0 :
         MIN2(abs_timeout_ns - now, 1000000ull) / 1000 / (oldest[0] && oldest[1] ? 2 : 1);
      bool any = false;
      for (int e = 0; e < 2; e++) {
         if (!oldest[e])
            continue;
         any = true;
         VkResult r = kext_fence_wait_once(conn, e, oldest[e]->seq, slice_us);
         if (r == VK_SUCCESS) {
            oldest[e]->signaled = true;
            return VK_SUCCESS;
         }
         if (r != VK_TIMEOUT)
            return r;
      }
      if (expired)
         return VK_TIMEOUT;
      if (!any)
         os_time_sleep(50);
   }
}

/* NVK's mem streams read the timeline value to recycle push chunks
 * (vk_sync_get_value); with no get_value that call went through NULL the
 * first time a stream ran out of chunks. */
static VkResult
macos_sync_get_value(struct vk_device *device, struct vk_sync *sync, uint64_t *value)
{
   struct nvkmd_macos_sync *s = container_of(sync, struct nvkmd_macos_sync, base);
   if (!s->signaled && s->seq &&
       kext_fence_wait_once(nvkmd_macos_conn(device), s->engine, s->seq, 0) == VK_SUCCESS)
      s->signaled = true;
   if (s->signaled)
      s->tl_done = s->tl_value;
   *value = s->tl_done;
   return VK_SUCCESS;
}

static const struct vk_sync_type nvkmd_macos_sync_type_template = {
   .size = sizeof(struct nvkmd_macos_sync),
   .features = VK_SYNC_FEATURE_BINARY |
               VK_SYNC_FEATURE_GPU_WAIT |
               VK_SYNC_FEATURE_CPU_WAIT |
               VK_SYNC_FEATURE_CPU_RESET |
               VK_SYNC_FEATURE_CPU_SIGNAL |
               VK_SYNC_FEATURE_WAIT_ANY |
               VK_SYNC_FEATURE_WAIT_PENDING,
   .init = macos_sync_init,
   .finish = macos_sync_finish,
   .signal = macos_sync_signal,
   .reset = macos_sync_reset,
   .move = macos_sync_move,
   .get_value = macos_sync_get_value,
   .wait_many = macos_sync_wait_many,
};

/* ----------------------------------------------------------------- mem */

static void
macos_mem_free(struct nvkmd_mem *_mem)
{
   struct nvkmd_macos_mem *mem = container_of(_mem, struct nvkmd_macos_mem, base);
   struct nvkmd_macos_dev *dev =
      container_of(_mem->dev, struct nvkmd_macos_dev, base);
   /* nvkmd_mem_unref() already dropped any CPU mappings */
   if (_mem->va)
      nvkmd_va_free(_mem->va);
   uint64_t h = mem->handle;
   IOConnectCallMethod(dev->conn, 23, &h, 1, NULL, 0, NULL, NULL, NULL, NULL);
   if (!mem->sys) {
      struct nvkmd_macos_pdev *pdev =
         container_of(_mem->dev->pdev, struct nvkmd_macos_pdev, base);
      p_atomic_add(&pdev->vram_used_local, -(int64_t)_mem->size_B);
   }
   FREE(mem);
}

static VkResult
macos_mem_map(struct nvkmd_mem *_mem, struct vk_object_base *log_obj,
              enum nvkmd_mem_map_flags flags, void *fixed_addr, void **map_out)
{
   struct nvkmd_macos_mem *mem = container_of(_mem, struct nvkmd_macos_mem, base);
   struct nvkmd_macos_dev *dev =
      container_of(_mem->dev, struct nvkmd_macos_dev, base);
   if (!(mem->sys || mem->cpu_vram) || fixed_addr)
      return MACOS_ERR(log_obj, VK_ERROR_MEMORY_MAP_FAILED);
   mach_vm_address_t addr = 0;
   mach_vm_size_t size = 0;
   /* IOUserClient::mapClientMemory64 lets the caller's cache bits override the
    * kext's (kIOMapUserOptionsMask), so BAR1 VRAM must ask for WC itself;
    * with only kIOMapAnywhere it maps uncached (~2 GB/s writes, 26 Sep). */
   if (IOConnectMapMemory64(dev->conn, (mem->handle < 0x1000 ? 0x1000 | mem->handle : 0x40000000u | mem->handle), mach_task_self(),
                            &addr, &size,
                            kIOMapAnywhere | (mem->cpu_vram ? kIOMapWriteCombineCache : 0)))
      return MACOS_ERR(log_obj, VK_ERROR_MEMORY_MAP_FAILED);
   *map_out = (void *)addr;
   return VK_SUCCESS;
}

static void
macos_mem_unmap(struct nvkmd_mem *_mem, enum nvkmd_mem_map_flags flags, void *map)
{
   struct nvkmd_macos_mem *mem = container_of(_mem, struct nvkmd_macos_mem, base);
   struct nvkmd_macos_dev *dev =
      container_of(_mem->dev, struct nvkmd_macos_dev, base);
   IOConnectUnmapMemory64(dev->conn, (mem->handle < 0x1000 ? 0x1000 | mem->handle : 0x40000000u | mem->handle), mach_task_self(),
                          (mach_vm_address_t)map);
}

static uint32_t
macos_mem_log_handle(struct nvkmd_mem *_mem)
{
   return container_of(_mem, struct nvkmd_macos_mem, base)->handle;
}

static const struct nvkmd_mem_ops macos_mem_ops = {
   .free = macos_mem_free,
   .map = macos_mem_map,
   .unmap = macos_mem_unmap,
   .log_handle = macos_mem_log_handle,
};

/* ------------------------------------------------------------------ va */

static VkResult
macos_va_bind_mem(struct nvkmd_va *va, struct vk_object_base *log_obj,
                  uint64_t va_offset_B, struct nvkmd_mem *_mem,
                  uint64_t mem_offset_B, uint64_t range_B)
{
   struct nvkmd_macos_mem *mem = container_of(_mem, struct nvkmd_macos_mem, base);
   struct nvkmd_macos_dev *dev = container_of(va->dev, struct nvkmd_macos_dev, base);
   if (container_of(va, struct nvkmd_macos_va, base)->sub)
      return MACOS_ERR(log_obj, VK_ERROR_FEATURE_NOT_PRESENT);
   if (mem->slab) {   /* rebind a suballocation through its slab object */
      _mem = &mem->slab->mem->base;
      mem_offset_B += mem->slab_off;
      mem = mem->slab->mem;
   }
   uint64_t addr = va->addr + va_offset_B;
   /* The kext binds in dev->gran units (64 KiB, or 2 MiB before ABI 114).
    * When VA and memory offset share the same phase, bind the enclosing
    * window (rebinding the same pages is idempotent). */
   const uint64_t gran = dev->gran;
   if (((addr ^ mem_offset_B) & (gran - 1)) != 0) {
      fprintf(stderr, "[nvkmd_macos] bind va 0x%llx / mem off 0x%llx phase mismatch\n",
              (unsigned long long)addr, (unsigned long long)mem_offset_B);
      return MACOS_ERR(log_obj, VK_ERROR_FEATURE_NOT_PRESENT);
   }
   const uint64_t head = addr & (gran - 1);
   addr -= head;
   mem_offset_B -= head;
   range_B = align64(range_B + head, gran);
   if (mem_offset_B + range_B > _mem->size_B)
      range_B = _mem->size_B - mem_offset_B;
   /* Never map past the VA reservation. NVK's arenas start with a 2 MiB
    * chunk here (NVK_MEM_ARENA_MIN_SIZE) even when the arena's VA is
    * smaller (the 128 KiB sampler table), and the kext would happily write
    * the neighbouring allocation's PTEs; when that object was freed, its
    * PTEs, now the arena's pages, were cleared with it -> Xid 31. */
   if (addr + range_B > va->addr + va->size_B)
      range_B = va->addr + va->size_B - addr;
   /* kext selector 24: {handle, va, kind, mem offset, range} (gran units) */
   uint64_t in[5] = { mem->handle, addr, va->pte_kind, mem_offset_B, range_B };
   MTRACE("bind va=0x%llx h=%u off=0x%llx range=0x%llx kind=%u\n", (unsigned long long)addr,
          mem->handle, (unsigned long long)mem_offset_B, (unsigned long long)range_B, va->pte_kind);
   if (IOConnectCallMethod(dev->conn, 24, in, 5, NULL, 0, NULL, NULL, NULL, NULL)) {
      fprintf(stderr, "[nvkmd_macos] bind va 0x%llx mem %u off 0x%llx range 0x%llx failed\n",
              (unsigned long long)addr, mem->handle, (unsigned long long)mem_offset_B,
              (unsigned long long)range_B);
      return MACOS_ERR(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   }
   return VK_SUCCESS;
}

static VkResult
macos_va_unbind(struct nvkmd_va *va, struct vk_object_base *log_obj,
                uint64_t va_offset_B, uint64_t range_B)
{
   struct nvkmd_macos_dev *dev = container_of(va->dev, struct nvkmd_macos_dev, base);
   /* Only whole pages are unbound; partial ranges stay mapped (the
    * neighbouring sub-allocation may still use the page). */
   const uint64_t start = align64(va->addr + va_offset_B, dev->gran);
   const uint64_t end = (va->addr + va_offset_B + range_B) & ~(dev->gran - 1);
   MTRACE("unbind va=0x%llx..0x%llx\n", (unsigned long long)start, (unsigned long long)end);
   if (end > start && !kext_va_unbind(dev->conn, start, end - start))
      return MACOS_ERR(log_obj, VK_ERROR_UNKNOWN);
   return VK_SUCCESS;
}

static void
macos_va_free(struct nvkmd_va *va)
{
   struct nvkmd_macos_dev *dev = container_of(va->dev, struct nvkmd_macos_dev, base);
   if (container_of(va, struct nvkmd_macos_va, base)->sub) {
      FREE(container_of(va, struct nvkmd_macos_va, base));
      return;
   }
   /* Like nvkmd_nouveau: unmap the whole range before the address can be
    * handed out again; if that fails, leak the range rather than reuse a
    * VA whose PTEs may still point at memory. */
   MTRACE("va_free va=0x%llx size=0x%llx\n", (unsigned long long)va->addr, (unsigned long long)va->size_B);
   if (kext_va_unbind(dev->conn, va->addr, va->size_B)) {
      simple_mtx_lock(&dev->vpdev->va_lock);
      util_vma_heap_free((va->flags & (NVKMD_VA_REPLAY | NVKMD_VA_ALLOC_FIXED)) ?
                         &dev->vpdev->replay_heap : &dev->vpdev->heap,
                         va->addr, va->size_B);
      simple_mtx_unlock(&dev->vpdev->va_lock);
   }
   FREE(container_of(va, struct nvkmd_macos_va, base));
}

static const struct nvkmd_va_ops macos_va_ops = {
   .free = macos_va_free,
   .bind_mem = macos_va_bind_mem,
   .unbind = macos_va_unbind,
};

/* ----------------------------------------------------------------- ctx */

static void
macos_ctx_destroy(struct nvkmd_ctx *_ctx)
{
   struct nvkmd_macos_ctx *ctx = container_of(_ctx, struct nvkmd_macos_ctx, base);
   struct nvkmd_macos_ctx *expected = ctx;
   /* the channel keeps this ctx's state: others must re-push theirs, and a
    * new ctx at the same address must not look like the owner */
   __atomic_compare_exchange_n(&g_gr_state_owner, &expected, GR_STATE_GONE, false,
                               __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
   if (ctx->acq_mem)
      nvkmd_mem_unref(ctx->acq_mem);
   FREE(ctx);
}

/* Host SEM_EXECUTE acquire (ACQ_CIRC_GEQ, 32-bit, TSG switch while waiting)
 * of `seq` at `va`: the same 5-method block the kext's fence release uses. */
static uint32_t
macos_acquire_words(uint32_t *w, uint64_t va, uint32_t seq)
{
   w[0] = 0x20050017;                           /* SEM_ADDR_LO .. SEM_EXECUTE */
   w[1] = (uint32_t)(va & 0xfffffffcull);
   w[2] = (uint32_t)((va >> 32) & 0xff);
   w[3] = seq;
   w[4] = 0;
   w[5] = 0x3 | (1u << 12);
   return 6;
}

/* Write the pending acquires into a push slot, returned in seg_va / seg_dw.
 * A slot is reused only after the fence of the submission that used it. */
static VkResult
macos_ctx_build_acquires(struct nvkmd_macos_ctx *ctx, struct vk_object_base *log_obj,
                         uint64_t *seg_va, uint32_t *seg_dw)
{
   struct nvkmd_macos_dev *dev = container_of(ctx->base.dev, struct nvkmd_macos_dev, base);
   if (!ctx->acq_mem) {
      VkResult r = nvkmd_dev_alloc_mapped_mem(ctx->base.dev, log_obj,
                                              NVKMD_MACOS_ACQ_SLOTS * 64, 0,
                                              NVKMD_MEM_GART, NVKMD_MEM_MAP_WR,
                                              &ctx->acq_mem);
      if (r != VK_SUCCESS)
         return r;
   }
   const uint32_t slot = ctx->acq_next++ % NVKMD_MACOS_ACQ_SLOTS;
   if (ctx->acq_slot_seq[slot] &&
       kext_fence_wait(dev->conn, ctx->engine, ctx->acq_slot_seq[slot], UINT64_MAX) != VK_SUCCESS)
      return MACOS_ERR(log_obj, VK_ERROR_DEVICE_LOST);
   uint32_t *w = (uint32_t *)((uint8_t *)ctx->acq_mem->map + slot * 64u), n = 0;
   for (uint32_t i = 0; i < ctx->n_acq && n + 6 <= 16; i++)
      n += macos_acquire_words(w + n, ctx->acq[i].va, ctx->acq[i].seq);
   ctx->n_acq = 0;
   *seg_va = ctx->acq_mem->va->addr + slot * 64u;
   *seg_dw = n;
   ctx->acq_slot_seq[slot] = UINT32_MAX;   /* set to the real fence after exec */
   return VK_SUCCESS;
}

static VkResult
macos_ctx_wait(struct nvkmd_ctx *_ctx, struct vk_object_base *log_obj,
               uint32_t wait_count, const struct vk_sync_wait *waits)
{
   struct nvkmd_macos_ctx *ctx = container_of(_ctx, struct nvkmd_macos_ctx, base);
   struct nvkmd_macos_dev *dev = container_of(_ctx->dev, struct nvkmd_macos_dev, base);
   /* Each engine's ring is in order, so a fence from this context's own
    * engine is already ordered before what we submit next. A fence from the
    * other engine (GR <-> CE) has no GPU-side wait here: wait on the CPU. */
   for (uint32_t i = 0; i < wait_count; i++) {
      struct nvkmd_macos_sync *s =
         container_of(waits[i].sync, struct nvkmd_macos_sync, base);
      if (s->signaled || macos_sync_trivial(s, waits[i].wait_value) ||
          (s->seq && s->engine == ctx->engine))
         continue;
      struct nvkmd_macos_pdev *pdev =
         container_of(_ctx->dev->pdev, struct nvkmd_macos_pdev, base);
      /* two acquires fit a 64 B slot */
      if (pdev->gpu_wait && s->seq && s->engine < 2 && pdev->sem_va[s->engine] &&
          ctx->n_acq < 2) {
         ctx->acq[ctx->n_acq].va = pdev->sem_va[s->engine];
         ctx->acq[ctx->n_acq].seq = s->seq;
         ctx->n_acq++;
         continue;
      }
      VkResult r = macos_sync_wait_one(dev->conn, s, waits[i].wait_value, 0, UINT64_MAX);
      if (r != VK_SUCCESS)
         return MACOS_ERR(log_obj, r);
   }
   return VK_SUCCESS;
}

static VkResult
macos_ctx_exec(struct nvkmd_ctx *_ctx, struct vk_object_base *log_obj,
               uint32_t exec_count, const struct nvkmd_ctx_exec *execs)
{
   struct nvkmd_macos_ctx *ctx = container_of(_ctx, struct nvkmd_macos_ctx, base);
   struct nvkmd_macos_dev *dev = container_of(_ctx->dev, struct nvkmd_macos_dev, base);
   struct nvkmd_macos_pdev *pdev = dev->vpdev;
   struct { uint64_t va; uint32_t dw, flags; } seg[NVKMD_MACOS_EXEC_MAX];
   /* Validate all chains before submitting anything. A fence cannot appear
    * within a split method, even when a submission exceeds 64 segments. */
   if (!macos_exec_chains_valid(exec_count, execs))
      return vk_errorf(log_obj, VK_ERROR_UNKNOWN,
                       "Incomplete push chain exceeds macOS submit capacity");
   for (uint32_t i = 0; i < exec_count || ctx->n_acq;) {
      uint32_t n = 0, acq_slot = UINT32_MAX;
      if (ctx->n_acq) {
         VkResult r = macos_ctx_build_acquires(ctx, log_obj, &seg[0].va, &seg[0].dw);
         if (r != VK_SUCCESS)
            return r;
         seg[0].flags = 0;
         acq_slot = (ctx->acq_next - 1) % NVKMD_MACOS_ACQ_SLOTS;
         n = 1;
      }
      const uint32_t batch = macos_exec_batch_count(exec_count - i,
         exec_count > i ? &execs[i] : NULL, NVKMD_MACOS_EXEC_MAX - n);
      /* A full-size chain does not fit beside an acquire prefix. Submit
       * just the prefix this time; the complete chain fits the next batch. */
      for (uint32_t j = 0; j < batch; ++j, ++n, ++i) {
         seg[n].va = execs[i].addr;
         seg[n].dw = execs[i].size_B / 4;
         seg[n].flags = execs[i].no_prefetch ?
            pdev->exec_segment_flags & NVKMD_MACOS_EXEC_NO_PREFETCH : 0;
      }
      uint64_t engine = ctx->engine, seq = 0;
      uint32_t cnt = 1;
      if (IOConnectCallMethod(dev->conn, 21, &engine, 1, seg, n * sizeof(seg[0]),
                              &seq, &cnt, NULL, NULL)) {
         nvkmd_macos_report_device_lost();
         return MACOS_ERR(log_obj, VK_ERROR_DEVICE_LOST);
      }
      ctx->last_seq = (uint32_t)seq;
      MTRACE("exec ctx=%p n=%u seq=%u\n", (void *)ctx, n, (unsigned)seq);
      if (ctx->gr_state)
         __atomic_store_n(&g_gr_state_owner, ctx, __ATOMIC_RELEASE);
      if (acq_slot != UINT32_MAX)
         ctx->acq_slot_seq[acq_slot] = (uint32_t)seq;
   }
   return VK_SUCCESS;
}

static VkResult
macos_ctx_signal(struct nvkmd_ctx *_ctx, struct vk_object_base *log_obj,
                 uint32_t signal_count, const struct vk_sync_signal *signals)
{
   struct nvkmd_macos_ctx *ctx = container_of(_ctx, struct nvkmd_macos_ctx, base);
   if (ctx->n_acq) {   /* a wait with nothing to execute still orders the signal */
      VkResult r = macos_ctx_exec(_ctx, log_obj, 0, NULL);
      if (r != VK_SUCCESS)
         return r;
   }
   for (uint32_t i = 0; i < signal_count; i++) {
      struct nvkmd_macos_sync *s =
         container_of(signals[i].sync, struct nvkmd_macos_sync, base);
      s->seq = ctx->last_seq;
      s->engine = ctx->engine;
      s->signaled = ctx->last_seq == 0;
      if (s->timeline) {
         s->tl_value = signals[i].signal_value;
         if (s->signaled)
            s->tl_done = s->tl_value;
      }
   }
   return VK_SUCCESS;
}

static VkResult
macos_ctx_flush(struct nvkmd_ctx *ctx, struct vk_object_base *log_obj)
{
   return VK_SUCCESS;
}

static VkResult
macos_ctx_bind(struct nvkmd_ctx *ctx, struct vk_object_base *log_obj,
               uint32_t bind_count, const struct nvkmd_ctx_bind *binds)
{
   for (uint32_t i = 0; i < bind_count; i++) {
      VkResult r = binds[i].op == NVKMD_BIND_OP_BIND ?
         nvkmd_va_bind_mem(binds[i].va, log_obj, binds[i].va_offset_B,
                           binds[i].mem, binds[i].mem_offset_B, binds[i].range_B) :
         nvkmd_va_unbind(binds[i].va, log_obj, binds[i].va_offset_B, binds[i].range_B);
      if (r != VK_SUCCESS)
         return r;
   }
   return VK_SUCCESS;
}

/* Wait for everything submitted on this context (NVK_DEBUG=push_sync). */
static VkResult
macos_ctx_sync(struct nvkmd_ctx *_ctx, struct vk_object_base *log_obj)
{
   struct nvkmd_macos_ctx *ctx = container_of(_ctx, struct nvkmd_macos_ctx, base);
   struct nvkmd_macos_dev *dev = container_of(_ctx->dev, struct nvkmd_macos_dev, base);
   if (!ctx->last_seq)
      return VK_SUCCESS;
   VkResult r = kext_fence_wait(dev->conn, ctx->engine, ctx->last_seq, UINT64_MAX);
   return r == VK_SUCCESS ? r : MACOS_ERR(log_obj, VK_ERROR_DEVICE_LOST);
}

static const struct nvkmd_ctx_ops macos_ctx_ops = {
   .destroy = macos_ctx_destroy,
   .wait = macos_ctx_wait,
   .exec = macos_ctx_exec,
   .bind = macos_ctx_bind,
   .signal = macos_ctx_signal,
   .flush = macos_ctx_flush,
   .sync = macos_ctx_sync,
};

/* ----------------------------------------------------------------- dev */

static VkResult
macos_dev_alloc_va(struct nvkmd_dev *_dev, struct vk_object_base *log_obj,
                   enum nvkmd_va_flags flags, uint8_t pte_kind,
                   uint64_t size_B, uint64_t align_B, uint64_t fixed_addr,
                   struct nvkmd_va **va_out)
{
   struct nvkmd_macos_dev *dev = container_of(_dev, struct nvkmd_macos_dev, base);
   if (flags & NVKMD_VA_SPARSE)
      return MACOS_ERR(log_obj, VK_ERROR_FEATURE_NOT_PRESENT);
   align_B = MAX2(align_B, dev->gran);
   size_B = align64(size_B, align_B);
   uint64_t addr = 0;
   simple_mtx_lock(&dev->vpdev->va_lock);
   if (flags & NVKMD_VA_ALLOC_FIXED) {
      if (fixed_addr >= NVKMD_MACOS_REPLAY_BASE &&
          !(fixed_addr & (align_B - 1)) &&
          util_vma_heap_alloc_addr(&dev->vpdev->replay_heap, fixed_addr, size_B))
         addr = fixed_addr;
   } else {
      addr = util_vma_heap_alloc((flags & NVKMD_VA_REPLAY) ? &dev->vpdev->replay_heap
                                                           : &dev->vpdev->heap,
                                 size_B, align_B);
   }
   simple_mtx_unlock(&dev->vpdev->va_lock);
   if (!addr) {
      return vk_errorf(log_obj, (flags & NVKMD_VA_ALLOC_FIXED) ?
                       VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS :
                       VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "VA 0x%llx + 0x%llx not available in the kext arena",
                       (unsigned long long)fixed_addr,
                       (unsigned long long)size_B);
   }
   struct nvkmd_macos_va *va = CALLOC_STRUCT(nvkmd_macos_va);
   if (!va) {
      simple_mtx_lock(&dev->vpdev->va_lock);
      util_vma_heap_free((flags & (NVKMD_VA_REPLAY | NVKMD_VA_ALLOC_FIXED)) ?
                         &dev->vpdev->replay_heap : &dev->vpdev->heap, addr, size_B);
      simple_mtx_unlock(&dev->vpdev->va_lock);
      return MACOS_ERR(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   va->base.ops = &macos_va_ops;
   va->base.dev = _dev;
   va->base.flags = flags;
   va->base.pte_kind = pte_kind;
   va->base.addr = addr;
   va->base.size_B = size_B;
   MTRACE("va_alloc va=0x%llx size=0x%llx flags=0x%x kind=%u\n", (unsigned long long)addr,
          (unsigned long long)size_B, flags, pte_kind);
   *va_out = &va->base;
   return VK_SUCCESS;
}

/* ------------------------------------------------------ suballocation */

static void
macos_slab_destroy(struct nvkmd_macos_slab *slab)
{
   util_vma_heap_finish(&slab->heap);
   if (slab->map)
      nvkmd_mem_unmap(&slab->mem->base, 0);
   nvkmd_mem_unref(&slab->mem->base);
   FREE(slab);
}

static void
macos_submem_free(struct nvkmd_mem *_mem)
{
   struct nvkmd_macos_mem *mem = container_of(_mem, struct nvkmd_macos_mem, base);
   struct nvkmd_macos_dev *dev =
      container_of(_mem->dev, struct nvkmd_macos_dev, base);
   struct nvkmd_macos_slab *slab = mem->slab;
   if (_mem->va)
      nvkmd_va_free(_mem->va);
   bool empty;
   simple_mtx_lock(&dev->slab_lock);
   util_vma_heap_free(&slab->heap, NVKMD_MACOS_SLAB_TAG + mem->slab_off, _mem->size_B);
   empty = --slab->users == 0;
   /* keep one empty slab around so alloc/free churn doesn't hit the kext */
   if (empty && !(slab->link.prev == slab->list && slab->link.next == slab->list))
      list_del(&slab->link);
   else
      empty = false;
   simple_mtx_unlock(&dev->slab_lock);
   if (empty)
      macos_slab_destroy(slab);
   FREE(mem);
}

static VkResult
macos_submem_map(struct nvkmd_mem *_mem, struct vk_object_base *log_obj,
                 enum nvkmd_mem_map_flags flags, void *fixed_addr, void **map_out)
{
   struct nvkmd_macos_mem *mem = container_of(_mem, struct nvkmd_macos_mem, base);
   if (fixed_addr || !mem->slab->map)          /* VRAM slabs are not mappable */
      return MACOS_ERR(log_obj, VK_ERROR_MEMORY_MAP_FAILED);
   *map_out = (uint8_t *)mem->slab->map + mem->slab_off;
   return VK_SUCCESS;
}

static void
macos_submem_unmap(struct nvkmd_mem *_mem, enum nvkmd_mem_map_flags flags, void *map)
{
   /* the slab's mapping lives as long as the slab */
}

static uint32_t
macos_submem_log_handle(struct nvkmd_mem *_mem)
{
   return container_of(_mem, struct nvkmd_macos_mem, base)->slab->mem->handle;
}

static const struct nvkmd_mem_ops macos_submem_ops = {
   .free = macos_submem_free,
   .map = macos_submem_map,
   .unmap = macos_submem_unmap,
   .log_handle = macos_submem_log_handle,
};

/* Caller holds slab_lock. */
static struct nvkmd_macos_slab *
macos_slab_create(struct nvkmd_macos_dev *dev, struct vk_object_base *log_obj, bool sys)
{
   struct nvkmd_macos_slab *slab = CALLOC_STRUCT(nvkmd_macos_slab);
   if (!slab)
      return NULL;
   struct nvkmd_mem *m = NULL;
   /* through nvkmd so the slab is on nvkmd_dev::mems like any object
    * (nvkmd_mem_unref removes it from there); 2 MiB > SUB_MAX, so this
    * does not recurse into the suballocator */
   if (nvkmd_dev_alloc_mem(&dev->base, log_obj, NVKMD_MACOS_GRAN, NVKMD_MACOS_GRAN,
                           sys ? NVKMD_MEM_GART | NVKMD_MEM_CAN_MAP : NVKMD_MEM_LOCAL,
                           &m) != VK_SUCCESS) {
      FREE(slab);
      return NULL;
   }
   slab->mem = container_of(m, struct nvkmd_macos_mem, base);
   slab->sys = sys;
   if (sys && nvkmd_mem_map(m, log_obj, NVKMD_MEM_MAP_RDWR, NULL, &slab->map) != VK_SUCCESS) {
      nvkmd_mem_unref(m);
      FREE(slab);
      return NULL;
   }
   util_vma_heap_init(&slab->heap, NVKMD_MACOS_SLAB_TAG, NVKMD_MACOS_GRAN);
   slab->heap.alloc_high = false;
   slab->list = sys ? &dev->slabs : &dev->vram_slabs;
   list_addtail(&slab->link, slab->list);
   return slab;
}

static VkResult
macos_dev_alloc_submem(struct nvkmd_macos_dev *dev, struct vk_object_base *log_obj,
                       uint64_t size_B, uint64_t align_B, bool sys,
                       enum nvkmd_mem_flags flags, struct nvkmd_mem **mem_out)
{
   /* VRAM: whole 64 KiB pages, so a kinded rebind maps only this object */
   const uint64_t sub_align = sys ? NVKMD_MACOS_SUB_ALIGN : dev->gran;
   size_B = align64(size_B, sub_align);
   align_B = MAX2(align_B, sub_align);
   struct list_head *list = sys ? &dev->slabs : &dev->vram_slabs;
   struct nvkmd_macos_mem *mem = CALLOC_STRUCT(nvkmd_macos_mem);
   struct nvkmd_macos_va *va = CALLOC_STRUCT(nvkmd_macos_va);
   if (!mem || !va) {
      FREE(mem);
      FREE(va);
      return MACOS_ERR(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   uint64_t addr = 0;
   struct nvkmd_macos_slab *slab = NULL;
   simple_mtx_lock(&dev->slab_lock);
   list_for_each_entry(struct nvkmd_macos_slab, s, list, link) {
      addr = util_vma_heap_alloc(&s->heap, size_B, align_B);
      if (addr) {
         slab = s;
         break;
      }
   }
   if (!slab && (slab = macos_slab_create(dev, log_obj, sys)))
      addr = util_vma_heap_alloc(&slab->heap, size_B, align_B);
   if (slab && addr)
      slab->users++;
   simple_mtx_unlock(&dev->slab_lock);
   if (!slab || !addr) {
      FREE(mem);
      FREE(va);
      return MACOS_ERR(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   }

   nvkmd_mem_init(&dev->base, &mem->base, &macos_submem_ops, flags, size_B,
                  (uint32_t)sub_align);
   mem->slab = slab;
   mem->slab_off = addr - NVKMD_MACOS_SLAB_TAG;
   /* New host memory reads as zero on Linux (the kernel's pages), and NVK
    * relies on it: e.g. multiview's extra occlusion queries only get their
    * availability written, the reports stay whatever the memory held. A
    * reused slab range still had the previous object's bytes. */
   if (sys && slab->map)
      memset((uint8_t *)slab->map + mem->slab_off, 0, size_B);
   mem->handle = slab->mem->handle;
   mem->phys = slab->mem->phys + mem->slab_off;
   mem->sys = sys;
   va->sub = true;
   va->base.ops = &macos_va_ops;
   va->base.dev = &dev->base;
   va->base.addr = slab->mem->base.va->addr + mem->slab_off;
   va->base.size_B = size_B;
   mem->base.va = &va->base;
   *mem_out = &mem->base;
   return VK_SUCCESS;
}

static VkResult
macos_dev_alloc_mem(struct nvkmd_dev *_dev, struct vk_object_base *log_obj,
                    uint64_t size_B, uint64_t align_B,
                    enum nvkmd_mem_flags flags, struct nvkmd_mem **mem_out)
{
   struct nvkmd_macos_dev *dev = container_of(_dev, struct nvkmd_macos_dev, base);
   struct nvkmd_macos_pdev *pdev = container_of(_dev->pdev, struct nvkmd_macos_pdev, base);
   bool sys = (flags & (NVKMD_MEM_CAN_MAP | NVKMD_MEM_GART)) != 0;
   /* 0.123.0: mappable device-local memory (the BAR heap type) goes to VRAM
    * inside the kext's BAR1 window when there is one; small ones stay in
    * the host-visible slabs */
   bool cpu_vram = pdev->vram_cpu_B && (flags & NVKMD_MEM_CAN_MAP) &&
                   (flags & (NVKMD_MEM_LOCAL | NVKMD_MEM_VRAM)) && !(flags & NVKMD_MEM_GART) &&
                   size_B > NVKMD_MACOS_SUB_MAX;
   if (!cpu_vram && size_B <= NVKMD_MACOS_SUB_MAX && align_B <= NVKMD_MACOS_SUB_MAX &&
       !(flags & NVKMD_MEM_SHARED) && (sys || dev->gran < NVKMD_MACOS_GRAN))
      return macos_dev_alloc_submem(dev, log_obj, size_B, align_B, sys, flags, mem_out);
   size_B = align64(size_B, NVKMD_MACOS_GRAN);
   uint64_t in[2] = { size_B, cpu_vram ? 2 : sys ? 1 : 0 }, out[2] = { 0, 0 };
   uint32_t cnt = 2;
   kern_return_t kr = IOConnectCallMethod(dev->conn, 22, in, 2, NULL, 0, out, &cnt, NULL, NULL);
   if (kr && cpu_vram) {   /* BAR1 window full: fall back to system memory */
      cpu_vram = false;
      in[1] = 1;
      cnt = 2;
      kr = IOConnectCallMethod(dev->conn, 22, in, 2, NULL, 0, out, &cnt, NULL, NULL);
   }
   if (kr)
      return MACOS_ERR(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   sys = sys && !cpu_vram;
   struct nvkmd_macos_mem *mem = CALLOC_STRUCT(nvkmd_macos_mem);
   if (!mem)
      return MACOS_ERR(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);
   nvkmd_mem_init(_dev, &mem->base, &macos_mem_ops, flags, size_B,
                  NVKMD_MACOS_GRAN);
   mem->handle = (uint32_t)out[0];
   mem->phys = out[1];
   mem->sys = sys;
   mem->cpu_vram = cpu_vram;
   if (!sys)
      p_atomic_add(&pdev->vram_used_local, (int64_t)size_B);
   VkResult r = macos_dev_alloc_va(_dev, log_obj, 0, 0, size_B,
                                   MAX2(align_B, NVKMD_MACOS_GRAN), 0,
                                   &mem->base.va);
   if (r == VK_SUCCESS)
      r = macos_va_bind_mem(mem->base.va, log_obj, 0, &mem->base, 0, size_B);
   if (r != VK_SUCCESS) {
      macos_mem_free(&mem->base);
      return r;
   }
   *mem_out = &mem->base;
   return VK_SUCCESS;
}

static VkResult
macos_dev_alloc_tiled_mem(struct nvkmd_dev *dev, struct vk_object_base *log_obj,
                          uint64_t size_B, uint64_t align_B, uint8_t pte_kind,
                          uint16_t tile_mode, enum nvkmd_mem_flags flags,
                          struct nvkmd_mem **mem_out)
{
   return MACOS_ERR(log_obj, VK_ERROR_FEATURE_NOT_PRESENT);
}

static VkResult
macos_dev_create_ctx(struct nvkmd_dev *_dev, struct vk_object_base *log_obj,
                     enum nvkmd_engines engines, struct nvkmd_ctx **ctx_out)
{
   struct nvkmd_macos_pdev *pdev =
      container_of(_dev->pdev, struct nvkmd_macos_pdev, base);
   struct nvkmd_macos_ctx *ctx = CALLOC_STRUCT(nvkmd_macos_ctx);
   if (!ctx)
      return MACOS_ERR(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);
   ctx->base.ops = &macos_ctx_ops;
   ctx->base.dev = _dev;
   /* 0.112.0: copy-only contexts (transfer queue, NVK's upload queue) can
    * run on the kext's copy-engine channel, overlapping GR work. */
   ctx->engine = (pdev->use_ce && engines == NVKMD_ENGINE_COPY) ? NVKMD_MACOS_CE
                                                                : NVKMD_MACOS_GR;
   ctx->gr_state = ctx->engine == NVKMD_MACOS_GR &&
                   (engines & (NVKMD_ENGINE_3D | NVKMD_ENGINE_COMPUTE));
   *ctx_out = &ctx->base;
   return VK_SUCCESS;
}

/* GPU PTIMER (ns, the clock semaphore-release timestamps use): TIME_1 is
 * the high word, TIME_0 the low word; re-read if the high word moved. */
static uint64_t
macos_dev_get_gpu_timestamp(struct nvkmd_dev *_dev)
{
   struct nvkmd_macos_dev *dev = container_of(_dev, struct nvkmd_macos_dev, base);
   for (int tries = 0; tries < 3; tries++) {
      uint32_t hi = 0, lo = 0, hi2 = 0;
      if (!kext_peek(dev->conn, 0x9410, &hi) ||
          !kext_peek(dev->conn, 0x9400, &lo) ||
          !kext_peek(dev->conn, 0x9410, &hi2))
         break;
      if (hi >= 0xbad00000u || (hi | lo) == 0)   /* PRI error or unclocked */
         break;
      if (hi == hi2)
         return ((uint64_t)hi << 32) | lo;
   }
   return os_time_get_nano();
}

static void
macos_dev_destroy(struct nvkmd_dev *_dev)
{
   struct nvkmd_macos_dev *dev = container_of(_dev, struct nvkmd_macos_dev, base);
   list_for_each_entry_safe(struct nvkmd_macos_slab, slab, &dev->slabs, link) {
      list_del(&slab->link);
      macos_slab_destroy(slab);
   }
   list_for_each_entry_safe(struct nvkmd_macos_slab, slab, &dev->vram_slabs, link) {
      list_del(&slab->link);
      macos_slab_destroy(slab);
   }
   simple_mtx_destroy(&dev->slab_lock);
   FREE(dev);
}

static const struct nvkmd_dev_ops macos_dev_ops = {
   .destroy = macos_dev_destroy,
   .get_gpu_timestamp = macos_dev_get_gpu_timestamp,
   .alloc_mem = macos_dev_alloc_mem,
   .alloc_tiled_mem = macos_dev_alloc_tiled_mem,
   .alloc_va = macos_dev_alloc_va,
   .create_ctx = macos_dev_create_ctx,
};

/* Keep DGC opt-in until the full groups have passed on this transport. */
bool
nvkmd_macos_has_dgc(struct nvkmd_pdev *base)
{
   struct nvkmd_macos_pdev *pdev = container_of(base, struct nvkmd_macos_pdev, base);
   const char *enable = getenv("NVK_MACOS_DGC");
   return (pdev->exec_segment_flags & NVKMD_MACOS_EXEC_NO_PREFETCH) &&
          enable && strcmp(enable, "1") == 0;
}

/* ------------------------------------------------------------- present */

/* Zero-copy scan-out of a VRAM object on window 0 (nvk_macos_wsi). Returns
 * once the kext queued the flip; the window channel throttles to one flip
 * per vblank, so the previously presented surface has been latched. */
VkResult
nvkmd_macos_mem_present(struct nvkmd_mem *_mem, uint64_t offset_B,
                        uint32_t pitch_B, uint32_t width, uint32_t height,
                        uint32_t wnd_format)
{
   struct nvkmd_macos_mem *mem = container_of(_mem, struct nvkmd_macos_mem, base);
   struct nvkmd_macos_dev *dev =
      container_of(_mem->dev, struct nvkmd_macos_dev, base);
   if (mem->sys)
      return VK_ERROR_SURFACE_LOST_KHR;   /* the display reads VRAM only */
   /* a suballocation presents through its slab object */
   uint64_t in[6] = { mem->handle, mem->slab_off + offset_B, pitch_B, width, height,
                      wnd_format };
   kern_return_t kr = IOConnectCallMethod(dev->conn, 27, in, 6, NULL, 0,
                                          NULL, NULL, NULL, NULL);
   if (kr == KERN_SUCCESS)
      return VK_SUCCESS;
   fprintf(stderr, "[nvkmd_macos] present handle %u pitch %u %ux%u fmt 0x%x: 0x%x\n",
           mem->handle, pitch_B, width, height, wnd_format, kr);
   /* bad surface parameters: let the app rebuild its swapchain; anything
    * else (old kext, display not owned, GPU reset) loses the surface */
   return kr == kIOReturnBadArgument ? VK_ERROR_OUT_OF_DATE_KHR
                                     : VK_ERROR_SURFACE_LOST_KHR;
}

void
nvkmd_macos_present_stop(struct nvkmd_dev *_dev)
{
   struct nvkmd_macos_dev *dev = container_of(_dev, struct nvkmd_macos_dev, base);
   IOConnectCallMethod(dev->conn, 28, NULL, 0, NULL, 0, NULL, NULL, NULL, NULL);
}

/* ---------------------------------------------------------------- pdev */

static struct nvkmd_macos_pdev *g_macos_pdev;

/* ---------------------------------------------------- device lost */

static bool
svc_number(io_service_t svc, const char *key, uint64_t *out)
{
   CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key,
                                             kCFStringEncodingUTF8);
   if (!k)
      return false;
   CFTypeRef v = IORegistryEntryCreateCFProperty(svc, k, kCFAllocatorDefault, 0);
   CFRelease(k);
   bool ok = false;
   if (v && CFGetTypeID(v) == CFNumberGetTypeID()) {
      int64_t n = 0;
      ok = CFNumberGetValue((CFNumberRef)v, kCFNumberSInt64Type, &n);
      *out = (uint64_t)n;
   } else if (v && CFGetTypeID(v) == CFBooleanGetTypeID()) {
      *out = CFBooleanGetValue((CFBooleanRef)v);
      ok = true;
   }
   if (v)
      CFRelease(v);
   return ok;
}

/* RC error codes GSP-RM reports (NVIDIA Xid numbers), the common ones. */
static const char *
xid_name(uint64_t xid)
{
   switch (xid) {
   case 8:  return "GPU stopped processing (timeout)";
   case 13: return "graphics engine exception";
   case 31: return "GPU memory page fault";
   case 32: return "invalid or corrupted push buffer";
   case 43: return "GPU stopped processing";
   case 45: return "preemptive cleanup";
   case 69: return "graphics engine class error";
   default: return "robust channel error";
   }
}

/* Once per process: say why the GPU was lost, from the kext's RC record. */
static void
nvkmd_macos_report_device_lost(void)
{
   struct nvkmd_macos_pdev *pdev = g_macos_pdev;
   if (!pdev || !pdev->svc || p_atomic_xchg(&pdev->lost_reported, 1))
      return;
   uint64_t rc = 0, xid = 0, eng = 0, chid = 0, lo = 0, hi = 0, type = 0;
   uint64_t gr = 1, resets = 0;
   svc_number(pdev->svc, "NVGspControl-rc-count", &rc);
   svc_number(pdev->svc, "NVGspControl-gr-persistent", &gr);
   svc_number(pdev->svc, "NVGspControl-gpu-reset-count", &resets);
   fprintf(stderr, "[nvkmd_macos] VK_ERROR_DEVICE_LOST: GR channel %s, %llu RC event(s),"
           " %llu GPU reset(s) so far\n", gr ? "alive" : "dead",
           (unsigned long long)rc, (unsigned long long)resets);
   if (rc && svc_number(pdev->svc, "NVGspControl-rc-except-type", &xid)) {
      svc_number(pdev->svc, "NVGspControl-rc-engine", &eng);
      svc_number(pdev->svc, "NVGspControl-rc-chid", &chid);
      fprintf(stderr, "[nvkmd_macos]   last RC: Xid %llu (%s), engine %llu, channel %llu\n",
              (unsigned long long)xid, xid_name(xid), (unsigned long long)eng,
              (unsigned long long)chid);
      if (svc_number(pdev->svc, "NVGspControl-rc-mmu-fault-lo", &lo) &&
          svc_number(pdev->svc, "NVGspControl-rc-mmu-fault-hi", &hi) && (lo | hi)) {
         lo &= 0xffffffffull;   /* a 32-bit property; CF hands it back sign-extended */
         svc_number(pdev->svc, "NVGspControl-rc-mmu-fault-type", &type);
         fprintf(stderr, "[nvkmd_macos]   MMU fault at GPU VA 0x%llx, type %llu\n",
                 (unsigned long long)((hi << 32) | lo), (unsigned long long)type);
      }
   }
   fprintf(stderr, "[nvkmd_macos]   recover with: sudo ~/nvrun/nvrun --reset\n");
}

/* A process can hold several VkInstances (CTS makes a custom one per test),
 * each with its own pdev and kext client; g_macos_pdev is only the newest,
 * and NULL once that one is gone. */
static io_connect_t
nvkmd_macos_conn(struct vk_device *vk_dev)
{
   struct nvk_device *ndev = container_of(vk_dev, struct nvk_device, vk);
   if (!ndev->nvkmd)
      return IO_OBJECT_NULL;
   return container_of(ndev->nvkmd, struct nvkmd_macos_dev, base)->conn;
}

static VkResult
macos_pdev_create_dev(struct nvkmd_pdev *_pdev, struct vk_object_base *log_obj,
                      struct nvkmd_dev **dev_out)
{
   struct nvkmd_macos_pdev *pdev = container_of(_pdev, struct nvkmd_macos_pdev, base);
   struct nvkmd_macos_dev *dev = CALLOC_STRUCT(nvkmd_macos_dev);
   if (!dev)
      return MACOS_ERR(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);
   dev->base.ops = &macos_dev_ops;
   dev->base.pdev = _pdev;
   dev->base.va_start = NVKMD_MACOS_ARENA_BASE;
   dev->base.va_end = NVKMD_MACOS_ARENA_END;
   list_inithead(&dev->base.mems);
   simple_mtx_init(&dev->base.mems_mutex, mtx_plain);
   simple_mtx_init(&dev->slab_lock, mtx_plain);
   list_inithead(&dev->slabs);
   list_inithead(&dev->vram_slabs);
   dev->gran = pdev->bind_gran;
   dev->vpdev = pdev;
   dev->conn = pdev->conn;
   *dev_out = &dev->base;
   return VK_SUCCESS;
}

static uint64_t
macos_pdev_get_vram_used(struct nvkmd_pdev *_pdev)
{
   struct nvkmd_macos_pdev *pdev = container_of(_pdev, struct nvkmd_macos_pdev, base);
   uint64_t used = 0;
   if (pdev->has_mem_info && kext_mem_info(pdev->conn, NULL, &used))
      return used;   /* all clients */
   return p_atomic_read(&pdev->vram_used_local);
}

static void
macos_pdev_destroy(struct nvkmd_pdev *_pdev)
{
   struct nvkmd_macos_pdev *pdev = container_of(_pdev, struct nvkmd_macos_pdev, base);
   IOServiceClose(pdev->conn);
   if (pdev->svc)
      IOObjectRelease(pdev->svc);
   if (g_macos_pdev == pdev)
      g_macos_pdev = NULL;
   util_vma_heap_finish(&pdev->replay_heap);
   util_vma_heap_finish(&pdev->heap);
   simple_mtx_destroy(&pdev->va_lock);
   FREE(pdev);
}

static const struct nvkmd_pdev_ops macos_pdev_ops = {
   .destroy = macos_pdev_destroy,
   .get_vram_used = macos_pdev_get_vram_used,
   .create_dev = macos_pdev_create_dev,
};

VkResult
nvkmd_macos_try_create_pdev(struct vk_object_base *log_obj,
                            enum nvk_debug debug_flags,
                            struct nvkmd_pdev **pdev_out)
{
   io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault,
                                                  IOServiceMatching("NVGspControl"));
   if (!svc)
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   io_connect_t conn = IO_OBJECT_NULL;
   /* The accelerator hands out the same client for type 'NVGP'; sandboxes
    * that allow Metal allow that path, the NVGspControl one needs no sandbox. */
   kern_return_t kr = KERN_FAILURE;
   io_service_t acc = IOServiceGetMatchingService(kIOMainPortDefault,
                                                  IOServiceMatching("NVAccelerator"));
   if (acc) {
      kr = IOServiceOpen(acc, mach_task_self(), 0x4E564750, &conn);
      IOObjectRelease(acc);
   }
   if (kr)
      kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
   if (kr) {
      IOObjectRelease(svc);
      return vk_errorf(log_obj, VK_ERROR_INCOMPATIBLE_DRIVER,
                       "NVGspControl user client: 0x%x (needs root)", kr);
   }

   struct nvkmd_macos_pdev *pdev = CALLOC_STRUCT(nvkmd_macos_pdev);
   if (!pdev) {
      IOServiceClose(conn);
      IOObjectRelease(svc);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   pdev->svc = svc;   /* kept for device-lost diagnostics */
   pdev->base.ops = &macos_pdev_ops;
   pdev->base.debug_flags = debug_flags;
   pdev->conn = conn;
   struct nv_device_info *info = &pdev->base.dev_info;
   *info = (struct nv_device_info) {
      .type = NV_DEVICE_TYPE_DIS,
      .device_id = 0x2704,
      .chipset = 0x193,
      .device_name = "NVIDIA GeForce RTX 4080 (NVK macOS)",
      .chipset_name = "AD103",
      .pci = { .domain = 0, .bus = 1, .dev = 0, .func = 0, .revision_id = 0xa1 },
      .sm = 89,
      .gpc_count = 7,
      .tpc_count = 38,
      .mp_per_tpc = 2,
      .max_warps_per_mp = 48,
      .cls_copy = 0xc7b5,
      .cls_eng2d = 0x902d,
      .cls_eng3d = 0xc997,
      .cls_m2mf = 0xa140,
      .cls_compute = 0xc9c0,
      .cls_gpfifo = 0xc86f,
      .vram_size_B = 14ull << 30,   /* replaced by the kext heap size below */
      .bar_size_B = 0,
      .max_smem_per_wg_kB = 99,
      .sm_smem_sizes_kB = { 0, 8, 16, 32, 64, 100 },
      .sm_smem_size_count = 6,
   };
   uint64_t heap_B = 0;
   pdev->has_mem_info = kext_mem_info(conn, &heap_B, NULL);
   if (pdev->has_mem_info && heap_B)
      info->vram_size_B = heap_B;
   pdev->base.kmd_info.has_get_vram_used = true;
   /* Opt-in until the CE ring path is proven live: without it everything
    * (copies included) runs in order on the GR channel. */
   const char *ce = getenv("NVK_MACOS_CE");
   pdev->use_ce = ce && ce[0] == '1';
   info->has_transfer_queue = pdev->use_ce;
   /* 0.114.0 kexts bind arena pages at 64 KiB */
   uint64_t abi = 0;
   svc_number(svc, "NVGspControl-abi", &abi);
   uint64_t exec_flags = 0;
   svc_number(svc, "NVGspControl-exec-segment-flags", &exec_flags);
   pdev->exec_segment_flags = exec_flags & NVKMD_MACOS_EXEC_NO_PREFETCH;
   pdev->bind_gran = abi >= 114 ? 0x10000 : NVKMD_MACOS_GRAN;
   /* 0.123.0: CPU-visible VRAM (BAR1 window) -> NVK's DEVICE_LOCAL|HOST_VISIBLE
    * memory type gets a real VRAM heap of that size (a ReBAR-sized BAR1). */
   if (abi >= 123)
      svc_number(svc, "NVGspControl-vram-cpu-bytes", &pdev->vram_cpu_B);
   if (pdev->vram_cpu_B)
      info->bar_size_B = pdev->vram_cpu_B;
   /* 0.128.0: fence semaphore VAs of the GR and CE rings */
   const char *gw = getenv("NVK_MACOS_GPU_WAIT");
   if (abi >= 123 && gw && gw[0] == '1' &&
       svc_number(svc, "NVGspControl-gr-sem-va", &pdev->sem_va[0]) &&
       svc_number(svc, "NVGspControl-ce-sem-va", &pdev->sem_va[1]))
      pdev->gpu_wait = pdev->sem_va[0] && pdev->sem_va[1];
   pdev->base.bind_align_B = (uint32_t)pdev->bind_gran;
   pdev->sync_type = nvkmd_macos_sync_type_template;
   pdev->timeline_type = vk_sync_timeline_get_type(&pdev->sync_type);
   pdev->sync_types[0] = &pdev->sync_type;
   pdev->sync_types[1] = &pdev->timeline_type.sync;
   pdev->sync_types[2] = NULL;
   pdev->base.sync_types = pdev->sync_types;
   simple_mtx_init(&pdev->va_lock, mtx_plain);
   util_vma_heap_init(&pdev->heap, NVKMD_MACOS_ARENA_BASE,
                      NVKMD_MACOS_REPLAY_BASE - NVKMD_MACOS_ARENA_BASE);
   util_vma_heap_init(&pdev->replay_heap, NVKMD_MACOS_REPLAY_BASE,
                      NVKMD_MACOS_ARENA_END - NVKMD_MACOS_REPLAY_BASE);
   g_macos_pdev = pdev;
   *pdev_out = &pdev->base;
   return VK_SUCCESS;
}
