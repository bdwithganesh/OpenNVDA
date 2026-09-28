/*
 * Host test for drivers/nvk-macos/nvkmd_macos.c: runs the NVK kernel-mode
 * backend against an in-memory mock of the NVGspControl user client (selectors
 * 8, 18, 20-26), linked with the real Mesa nvkmd.c / util code.
 * Build + run: tests/nvkmd_macos/run.sh <patched mesa-26.0.8 tree>.
 *
 * Covers: VA ranges unbound on free and reused (util_vma_heap), capture/replay
 * fixed addresses, fence waits longer than 5 s, the WAIT_ANY path, PTIMER
 * timestamps, kext heap size / VRAM usage, and the fallbacks for kexts without
 * selectors 25/26.
 */
#include "nvkmd/nvkmd.h"
#include "nv_push.h"
#include "vk_log.h"
#include "vk_sync.h"
#include "vk_sync_timeline.h"
#include "util/cache_ops.h"

#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

VkResult nvkmd_macos_try_create_pdev(struct vk_object_base *log_obj,
                                     enum nvk_debug debug_flags,
                                     struct nvkmd_pdev **pdev_out);
VkResult nvkmd_macos_mem_present(struct nvkmd_mem *mem, uint64_t offset_B,
                                 uint32_t pitch_B, uint32_t width,
                                 uint32_t height, uint32_t wnd_format);
void nvkmd_macos_present_stop(struct nvkmd_dev *dev);

#define CHECK(c) do { if (!(c)) { \
   fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

#define ARENA_BASE 0x2800000000ull
#define PAGES 16384u
#define GRAN 0x200000ull
#define MAXOBJ 1024

/* ------------------------------------------------------------ mock kext */

static struct {
   bool old_kext;                 /* no selectors 25/26 (older kext) */
   uint32_t gpu_done, next_seq;
   uint32_t complete_after;       /* complete pending work on the Nth wait */
   uint32_t fence_calls;
   uint64_t fence_timeouts[64];
   uint32_t fence_seqs[64];
   uint16_t page[PAGES];          /* 0 unbound, else handle / 0xffff raw */
   uint32_t sel20_unbinds, sel25_calls;
   uint64_t obj_bytes[MAXOBJ];
   uint32_t obj_domain[MAXOBJ];
   uint64_t cpu_window_B;   /* BAR1 window left */
   uint32_t cpu_allocs;
   void *obj_map[MAXOBJ];
   uint32_t next_handle;
   uint64_t vram_used;
   uint32_t ptimer_hi[8], ptimer_hi_n, ptimer_hi_i, ptimer_lo;
   uint64_t present_args[6];
   uint32_t presents, present_stops;
   int present_ret;
   bool offline;                  /* GPU reset since open (kIOReturnOffline) */
   uint32_t ce_done, ce_next, ce_fence_calls, last_exec_engine;
   uint32_t last_exec_segs, last_seg0_dw;   /* GPU-side waits */
   uint64_t last_seg0_va;
   int live_objs;
} K;

const mach_port_t kIOMainPortDefault = 0;
task_port_t mach_task_self(void) { return 1; }
CFMutableDictionaryRef IOServiceMatching(const char *name) { return (CFMutableDictionaryRef)name; }
io_service_t IOServiceGetMatchingService(mach_port_t p, CFDictionaryRef m) { return 7; }
kern_return_t IOServiceOpen(io_service_t s, task_port_t t, uint32_t type, io_connect_t *c)
{ *c = 9; return KERN_SUCCESS; }
kern_return_t IOServiceClose(io_connect_t c) { return KERN_SUCCESS; }
kern_return_t IOObjectRelease(io_object_t o) { return KERN_SUCCESS; }

/* Registry properties of the mock NVGspControl service. */
static struct { const char *key; int64_t value; bool boolean; } props[16];
static int nprops;
struct mock_cf { int kind; int64_t v; char s[64]; };   /* 1 string, 2 number, 3 bool */
static void set_prop(const char *key, int64_t v, bool boolean)
{
   for (int i = 0; i < nprops; i++)
      if (!strcmp(props[i].key, key)) { props[i].value = v; props[i].boolean = boolean; return; }
   props[nprops].key = key; props[nprops].value = v; props[nprops].boolean = boolean; nprops++;
}
CFStringRef CFStringCreateWithCString(CFAllocatorRef a, const char *s, uint32_t enc)
{
   struct mock_cf *c = calloc(1, sizeof(*c));
   c->kind = 1;
   snprintf(c->s, sizeof(c->s), "%s", s);
   return (CFStringRef)c;
}
CFTypeRef IORegistryEntryCreateCFProperty(io_object_t e, CFStringRef key, CFAllocatorRef a,
                                          IOOptionBits o)
{
   const struct mock_cf *k = (const struct mock_cf *)key;
   for (int i = 0; i < nprops; i++)
      if (!strcmp(props[i].key, k->s)) {
         struct mock_cf *c = calloc(1, sizeof(*c));
         c->kind = props[i].boolean ? 3 : 2;
         c->v = props[i].value;
         return c;
      }
   return NULL;
}
CFTypeID CFGetTypeID(CFTypeRef cf) { return ((const struct mock_cf *)cf)->kind; }
CFTypeID CFNumberGetTypeID(void) { return 2; }
CFTypeID CFBooleanGetTypeID(void) { return 3; }
Boolean CFNumberGetValue(CFNumberRef n, int type, void *out)
{ *(int64_t *)out = ((const struct mock_cf *)n)->v; return 1; }
Boolean CFBooleanGetValue(CFBooleanRef b) { return ((const struct mock_cf *)b)->v != 0; }
void CFRelease(CFTypeRef cf) { free((void *)cf); }

/* ABI 114 kext: 64 KiB granular arena pages */
#define K64 0x10000ull
static uint16_t page64[PAGES * 32];
static bool gran64;
static uint64_t last_bind[5];

static void
mark64(uint64_t va, uint64_t bytes, uint16_t v)
{
   CHECK(va >= ARENA_BASE && !(va & (K64 - 1)) && !(bytes & (K64 - 1)));
   for (uint64_t a = va; a < va + bytes; a += K64) {
      CHECK((a - ARENA_BASE) / K64 < PAGES * 32);
      page64[(a - ARENA_BASE) / K64] = v;
   }
}

static void
mark(uint64_t va, uint64_t bytes, uint16_t v)
{
   if (gran64) {
      mark64(va, bytes, v);
      return;
   }
   CHECK(va >= ARENA_BASE && !(va & (GRAN - 1)) && !(bytes & (GRAN - 1)));
   for (uint64_t a = va; a < va + bytes; a += GRAN) {
      CHECK((a - ARENA_BASE) / GRAN < PAGES);
      K.page[(a - ARENA_BASE) / GRAN] = v;
   }
}

static uint32_t
pages_of(uint16_t v)
{
   uint32_t n = 0;
   for (uint32_t i = 0; i < PAGES; i++)
      n += K.page[i] == v;
   return n;
}

kern_return_t
IOConnectCallMethod(mach_port_t c, uint32_t sel, const uint64_t *in, uint32_t nin,
                    const void *sin, size_t nsin, uint64_t *out, uint32_t *nout,
                    void *sout, size_t *nsout)
{
   switch (sel) {
   case 8: /* peek {offset, count} -> words */
      CHECK(nin == 2 && in[1] == 1 && *nsout >= 4);
      if (in[0] == 0x9410)
         *(uint32_t *)sout = K.ptimer_hi[K.ptimer_hi_i++ % K.ptimer_hi_n];
      else if (in[0] == 0x9400)
         *(uint32_t *)sout = K.ptimer_lo;
      else
         CHECK(!"unexpected peek");
      *nsout = 4;
      return KERN_SUCCESS;
   case 18: { /* fence wait {seq, timeout_us} -> done */
      CHECK(nin == 2 && in[1] <= 5000000);
      if (K.offline)
         return kIOReturnOffline;
      if (K.fence_calls < 64) {
         K.fence_timeouts[K.fence_calls] = in[1];
         K.fence_seqs[K.fence_calls] = (uint32_t)in[0];
      }
      K.fence_calls++;
      if (K.complete_after && --K.complete_after == 0)
         K.gpu_done = K.next_seq;
      out[0] = K.gpu_done;
      return (int32_t)((uint32_t)in[0] - K.gpu_done) <= 0 ? KERN_SUCCESS : kIOReturnTimeout;
   }
   case 20: /* vaBind {va, phys, bytes, flags}; bytes 0 = unbind one page */
      CHECK(nin == 4);
      if (in[2] == 0) {
         K.sel20_unbinds++;
         mark(in[0], GRAN, 0);
      } else {
         mark(in[0], in[2], 0xffff);
      }
      return KERN_SUCCESS;
   case 21: /* execSegments {engine} + segments -> seq */
      CHECK(nin == 1 && nsin && nsin % 16 == 0 && in[0] <= 1);
      if (K.offline)
         return kIOReturnOffline;
      K.last_exec_engine = (uint32_t)in[0];
      K.last_exec_segs = (uint32_t)(nsin / 16);
      memcpy(&K.last_seg0_va, sin, 8);
      memcpy(&K.last_seg0_dw, (const uint8_t *)sin + 8, 4);
      out[0] = in[0] ? ++K.ce_next : ++K.next_seq;
      return KERN_SUCCESS;
   case 29: /* CE fence wait {seq, timeout_us} */
      CHECK(nin == 2 && in[1] <= 5000000);
      K.ce_fence_calls++;
      K.ce_done = K.ce_next;          /* CE work completes when waited on */
      out[0] = K.ce_done;
      return (int32_t)((uint32_t)in[0] - K.ce_done) <= 0 ? KERN_SUCCESS : kIOReturnTimeout;
   case 22: { /* memAlloc {bytes, domain} -> {handle, phys} */
      CHECK(nin == 2 && !(in[0] & (GRAN - 1)));
      uint32_t h = ++K.next_handle;
      CHECK(h < MAXOBJ);
      K.obj_bytes[h] = in[0];
      K.live_objs++;
      if (in[1] == 2 && K.cpu_window_B < in[0]) {   /* BAR1 window full */
         K.next_handle--;
         K.live_objs--;
         K.obj_bytes[h] = 0;
         return kIOReturnNoMemory;
      }
      K.obj_domain[h] = (uint32_t)in[1];
      if (in[1] == 2) {
         K.cpu_window_B -= in[0];
         K.cpu_allocs++;
      }
      if (in[1] != 1)
         K.vram_used += in[0];
      out[0] = h;
      out[1] = in[1] != 1 ? 0x40000000ull + h * 0x10000000ull : 0;
      return KERN_SUCCESS;
   }
   case 23: /* memFree {handle}: a new kext unbinds the leftovers itself */
      CHECK(nin == 1 && in[0] < MAXOBJ && K.obj_bytes[in[0]]);
      K.live_objs--;
      if (K.obj_domain[in[0]] != 1)
         K.vram_used -= K.obj_bytes[in[0]];
      if (K.obj_domain[in[0]] == 2)
         K.cpu_window_B += K.obj_bytes[in[0]];
      K.obj_bytes[in[0]] = 0;
      return KERN_SUCCESS;
   case 24: /* vaBindObject {handle, va, kind, offset, range} */
      CHECK(nin == 5 && in[0] < MAXOBJ && K.obj_bytes[in[0]]);
      CHECK(in[3] + in[4] <= K.obj_bytes[in[0]]);
      memcpy(last_bind, in, sizeof(last_bind));
      mark(in[1], in[4], (uint16_t)in[0]);
      return KERN_SUCCESS;
   case 25: /* vaUnbind {va, bytes} */
      if (K.old_kext)
         return kIOReturnUnsupported;
      CHECK(nin == 2 && in[1]);
      K.sel25_calls++;
      mark(in[0], in[1], 0);
      return KERN_SUCCESS;
   case 27: /* present {handle, offset, pitch, width, height, format} */
      if (K.old_kext)
         return kIOReturnUnsupported;
      CHECK(nin == 6);
      memcpy(K.present_args, in, sizeof(K.present_args));
      K.presents++;
      return K.present_ret;
   case 28: /* presentStop */
      if (K.old_kext)
         return kIOReturnUnsupported;
      K.present_stops++;
      return KERN_SUCCESS;
   case 26: /* memInfo -> {heap, vram used, sys used} */
      if (K.old_kext)
         return kIOReturnUnsupported;
      CHECK(*nout == 3);
      out[0] = 15ull << 30;
      out[1] = K.vram_used;
      out[2] = 0;
      return KERN_SUCCESS;
   }
   CHECK(!"unexpected selector");
   return kIOReturnUnsupported;
}

kern_return_t
IOConnectMapMemory64(io_connect_t c, uint32_t type, task_port_t t,
                     mach_vm_address_t *addr, mach_vm_size_t *size, IOOptionBits o)
{
   uint32_t h = type & 0xfff;
   CHECK((type & 0xfffff000) == 0x1000 && h < MAXOBJ && (K.obj_domain[h] == 1 || K.obj_domain[h] == 2));
   if (!K.obj_map[h])
      K.obj_map[h] = calloc(1, K.obj_bytes[h]);
   *addr = (mach_vm_address_t)(uintptr_t)K.obj_map[h];
   *size = K.obj_bytes[h];
   return KERN_SUCCESS;
}

kern_return_t
IOConnectUnmapMemory64(io_connect_t c, uint32_t type, task_port_t t, mach_vm_address_t a)
{
   return KERN_SUCCESS;
}

/* ------------------------------------------- Mesa symbols not linked in */

VkResult
__vk_errorf(const void *obj, VkResult error, const char *file, int line,
            const char *format, ...)
{
   return error;
}

struct vk_sync_timeline_type
vk_sync_timeline_get_type(const struct vk_sync_type *point_sync_type)
{
   struct vk_sync_timeline_type t;
   memset(&t, 0, sizeof(t));
   return t;
}

VkResult nvkmd_nouveau_try_create_pdev(struct _drmDevice *d, struct vk_object_base *o,
                                       enum nvk_debug f, struct nvkmd_pdev **p)
{ return VK_ERROR_INCOMPATIBLE_DRIVER; }
void vk_push_print(FILE *fp, const struct nv_push *push,
                   const struct nv_device_info *devinfo) { }
void util_flush_range(void *start, size_t size) { }
void util_flush_inval_range(void *start, size_t size) { }

/* ----------------------------------------------------------------- tests */

static struct vk_sync *
new_sync(struct nvkmd_pdev *pdev)
{
   const struct vk_sync_type *t = pdev->sync_types[0];
   struct vk_sync *s = calloc(1, t->size);
   s->type = t;
   CHECK(t->init(NULL, s, 0) == VK_SUCCESS);
   return s;
}

static void
submit_and_signal(struct nvkmd_ctx *ctx, struct vk_sync *s)
{
   const struct nvkmd_ctx_exec e = { .addr = ARENA_BASE, .size_B = 64 };
   CHECK(ctx->ops->exec(ctx, NULL, 1, &e) == VK_SUCCESS);
   const struct vk_sync_signal sig = { .sync = s };
   CHECK(ctx->ops->signal(ctx, NULL, 1, &sig) == VK_SUCCESS);
}

static void
reset_fences(void)
{
   K.fence_calls = 0;
   K.complete_after = 0;
}

static void
test_new_kext(void)
{
   memset(&K, 0, sizeof(K));
   struct nvkmd_pdev *pdev = NULL;
   CHECK(nvkmd_macos_try_create_pdev(NULL, 0, &pdev) == VK_SUCCESS);
   CHECK(pdev->dev_info.vram_size_B == 15ull << 30);        /* kext heap */
   CHECK(pdev->kmd_info.has_get_vram_used);
   struct nvkmd_dev *dev = NULL;
   CHECK(nvkmd_pdev_create_dev(pdev, NULL, &dev) == VK_SUCCESS);

   /* VRAM object: bound on alloc, unbound + VA recycled on free. */
   struct nvkmd_mem *m = NULL;
   CHECK(nvkmd_dev_alloc_mem(dev, NULL, 3 << 20, 0, NVKMD_MEM_LOCAL, &m) == VK_SUCCESS);
   CHECK(m->size_B == 4 << 20 && m->va && m->va->size_B == 4 << 20);
   const uint64_t addr = m->va->addr;
   CHECK(pages_of(1) == 2);
   CHECK(nvkmd_pdev_get_vram_used(pdev) == 4 << 20);
   nvkmd_mem_unref(m);
   CHECK(pages_of(1) == 0 && K.sel25_calls == 1 && K.sel20_unbinds == 0);
   CHECK(nvkmd_pdev_get_vram_used(pdev) == 0);
   CHECK(nvkmd_dev_alloc_mem(dev, NULL, 4 << 20, 0, NVKMD_MEM_LOCAL, &m) == VK_SUCCESS);
   CHECK(m->va->addr == addr);                              /* reused */
   nvkmd_mem_unref(m);

   /* Mapped sysmem object round trip. */
   CHECK(nvkmd_dev_alloc_mapped_mem(dev, NULL, 2 << 20, 0, NVKMD_MEM_GART,
                                    NVKMD_MEM_MAP_RDWR, &m) == VK_SUCCESS);
   CHECK(m->map);
   memset(m->map, 0x5a, 4096);
   nvkmd_mem_unref(m);

   /*
    * Churn far past the 28 GiB main heap with mixed sizes: the old bump +
    * exact-size-hole allocator ran out here; util_vma_heap coalesces.
    */
   for (uint32_t i = 0; i < 20000; i++) {
      struct nvkmd_va *a = NULL, *b = NULL;
      CHECK(nvkmd_dev_alloc_va(dev, NULL, 0, 0, (1 + i % 7) << 21, 0, 0, &a) == VK_SUCCESS);
      CHECK(nvkmd_dev_alloc_va(dev, NULL, 0, 0, (1 + i % 5) << 22, 0, 0, &b) == VK_SUCCESS);
      CHECK(a->addr >= ARENA_BASE && b->addr + b->size_B <= 0x2F00000000ull);
      nvkmd_va_free(a);
      nvkmd_va_free(b);
   }

   /* Capture/replay: fixed addresses live in the last 4 GiB only. */
   struct nvkmd_va *f = NULL, *g = NULL;
   CHECK(nvkmd_dev_alloc_va(dev, NULL, NVKMD_VA_REPLAY | NVKMD_VA_ALLOC_FIXED, 0,
                            4 << 20, 0, 0x2F10000000ull, &f) == VK_SUCCESS);
   CHECK(f->addr == 0x2F10000000ull);
   CHECK(nvkmd_dev_alloc_va(dev, NULL, NVKMD_VA_REPLAY | NVKMD_VA_ALLOC_FIXED, 0,
                            2 << 20, 0, 0x2F10200000ull, &g) ==
         VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS);          /* collision */
   CHECK(nvkmd_dev_alloc_va(dev, NULL, NVKMD_VA_REPLAY | NVKMD_VA_ALLOC_FIXED, 0,
                            2 << 20, 0, ARENA_BASE, &g) ==
         VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS);          /* main heap */
   nvkmd_va_free(f);
   CHECK(nvkmd_dev_alloc_va(dev, NULL, NVKMD_VA_REPLAY | NVKMD_VA_ALLOC_FIXED, 0,
                            2 << 20, 0, 0x2F10200000ull, &g) == VK_SUCCESS);
   nvkmd_va_free(g);
   CHECK(nvkmd_dev_alloc_va(dev, NULL, NVKMD_VA_SPARSE, 0, 2 << 20, 0, 0, &g) ==
         VK_ERROR_FEATURE_NOT_PRESENT);

   /* Partial unbind only drops whole 2 MiB pages inside the range. */
   CHECK(nvkmd_dev_alloc_mem(dev, NULL, 8 << 20, 0, NVKMD_MEM_LOCAL, &m) == VK_SUCCESS);
   const uint16_t h = (uint16_t)K.next_handle;
   CHECK(pages_of(h) == 4);
   CHECK(nvkmd_va_unbind(m->va, NULL, 1 << 20, 5 << 20) == VK_SUCCESS);
   CHECK(pages_of(h) == 2);                     /* pages 1 and 2 dropped */
   nvkmd_mem_unref(m);
   CHECK(pages_of(h) == 0);

   /* Fences: an infinite wait survives the kext's 5 s cap. */
   struct nvkmd_ctx *ctx = NULL;
   CHECK(nvkmd_dev_create_ctx(dev, NULL, NVKMD_ENGINE_3D, &ctx) == VK_SUCCESS);
   struct vk_sync *s1 = new_sync(pdev);
   submit_and_signal(ctx, s1);
   reset_fences();
   K.complete_after = 3;
   struct vk_sync_wait w1 = { .sync = s1 };
   CHECK(s1->type->wait_many(NULL, 1, &w1, 0, UINT64_MAX) == VK_SUCCESS);
   CHECK(K.fence_calls == 3 && K.fence_timeouts[0] == 5000000);
   /* Already signaled: no kext call. */
   reset_fences();
   CHECK(s1->type->wait_many(NULL, 1, &w1, 0, 0) == VK_SUCCESS && K.fence_calls == 0);
   /* Pending with a past deadline: one zero-timeout poll, VK_TIMEOUT. */
   struct vk_sync *s2 = new_sync(pdev);
   submit_and_signal(ctx, s2);
   reset_fences();
   struct vk_sync_wait w2 = { .sync = s2 };
   CHECK(s2->type->wait_many(NULL, 1, &w2, 0, 0) == VK_TIMEOUT);
   CHECK(K.fence_calls == 1 && K.fence_timeouts[0] == 0);
   /* WAIT_PENDING returns once submitted. */
   CHECK(s2->type->wait_many(NULL, 1, &w2, VK_SYNC_WAIT_PENDING, 0) == VK_SUCCESS);

   /* WAIT_ANY waits on the oldest submission, across seq wrap-around. */
   K.gpu_done = K.next_seq = 0xfffffffdu;
   struct vk_sync *old = new_sync(pdev), *young = new_sync(pdev);
   submit_and_signal(ctx, old);      /* seq 0xfffffffe */
   K.next_seq = 1;
   submit_and_signal(ctx, young);    /* seq 2 */
   reset_fences();
   K.complete_after = 2;
   struct vk_sync_wait any[2] = { { .sync = young }, { .sync = old } };
   CHECK(young->type->wait_many(NULL, 2, any, VK_SYNC_WAIT_ANY, UINT64_MAX) == VK_SUCCESS);
   CHECK(K.fence_seqs[0] == 0xfffffffeu);
   /* CPU-signaled sync satisfies WAIT_ANY without touching the kext. */
   struct vk_sync *cpu = new_sync(pdev), *never = new_sync(pdev);
   CHECK(cpu->type->signal(NULL, cpu, 1) == VK_SUCCESS);
   reset_fences();
   struct vk_sync_wait any2[2] = { { .sync = never }, { .sync = cpu } };
   CHECK(cpu->type->wait_many(NULL, 2, any2, VK_SYNC_WAIT_ANY, 0) == VK_SUCCESS);
   CHECK(K.fence_calls == 0);
   /* Unsubmitted + expired deadline: timeout. */
   struct vk_sync_wait none = { .sync = never };
   CHECK(never->type->wait_many(NULL, 1, &none, VK_SYNC_WAIT_ANY, 0) == VK_TIMEOUT);

   /* Ctx sync (NVK_DEBUG=push_sync) waits for the last submission. */
   reset_fences();
   K.complete_after = 1;
   CHECK(ctx->ops->sync && ctx->ops->sync(ctx, NULL) == VK_SUCCESS);

   /*
    * GPU reset since this process opened the kext: waits and submits report
    * device lost instead of aliasing the new fence sequence; the first loss
    * prints the kext's RC record (Xid, engine, fault VA).
    */
   set_prop("NVGspControl-rc-count", 1, false);
   set_prop("NVGspControl-gr-persistent", 0, true);
   set_prop("NVGspControl-rc-except-type", 31, false);
   set_prop("NVGspControl-rc-engine", 1, false);
   set_prop("NVGspControl-rc-chid", 3, false);
   set_prop("NVGspControl-rc-mmu-fault-lo", 0x40001000, false);
   set_prop("NVGspControl-rc-mmu-fault-hi", 0x28, false);
   set_prop("NVGspControl-rc-mmu-fault-type", 2, false);
   struct vk_sync *lost = new_sync(pdev);
   submit_and_signal(ctx, lost);
   K.offline = true;
   struct vk_sync_wait wl = { .sync = lost };
   CHECK(lost->type->wait_many(NULL, 1, &wl, 0, UINT64_MAX) == VK_ERROR_DEVICE_LOST);
   const struct nvkmd_ctx_exec e2 = { .addr = ARENA_BASE, .size_B = 64 };
   CHECK(ctx->ops->exec(ctx, NULL, 1, &e2) == VK_ERROR_DEVICE_LOST);
   K.offline = false;
   free(lost);

   /* PTIMER: retried when the high word moves, PRI errors fall back. */
   K.ptimer_hi[0] = 5; K.ptimer_hi[1] = 6; K.ptimer_hi[2] = 6; K.ptimer_hi[3] = 6;
   K.ptimer_hi_n = 4; K.ptimer_hi_i = 0; K.ptimer_lo = 0x1234;
   CHECK(nvkmd_dev_get_gpu_timestamp(dev) == ((6ull << 32) | 0x1234));
   K.ptimer_hi[0] = 0xbadf1100; K.ptimer_hi_n = 1; K.ptimer_hi_i = 0;
   const uint64_t t = nvkmd_dev_get_gpu_timestamp(dev);
   CHECK(t && (t >> 32) != 0xbadf1100);

   /*
    * Present: VRAM objects go to selector 27 with their kext handle; sysmem
    * cannot be scanned out; kext errors map to WSI results.
    */
   struct nvkmd_mem *scan = NULL, *host = NULL;
   CHECK(nvkmd_dev_alloc_mem(dev, NULL, 34 << 20, 0, NVKMD_MEM_LOCAL, &scan) == VK_SUCCESS);
   const uint32_t scan_handle = K.next_handle;
   CHECK(nvkmd_macos_mem_present(scan, 0, 15360, 3840, 2160, 0xCF) == VK_SUCCESS);
   CHECK(K.presents == 1 && K.present_args[0] == scan_handle && K.present_args[1] == 0 &&
         K.present_args[2] == 15360 && K.present_args[3] == 3840 &&
         K.present_args[4] == 2160 && K.present_args[5] == 0xCF);
   K.present_ret = kIOReturnBadArgument;
   CHECK(nvkmd_macos_mem_present(scan, 0, 15360, 3840, 2160, 0xCF) == VK_ERROR_OUT_OF_DATE_KHR);
   K.present_ret = kIOReturnNotPermitted;
   CHECK(nvkmd_macos_mem_present(scan, 0, 15360, 3840, 2160, 0xCF) == VK_ERROR_SURFACE_LOST_KHR);
   K.present_ret = KERN_SUCCESS;
   CHECK(nvkmd_dev_alloc_mem(dev, NULL, 2 << 20, 0, NVKMD_MEM_GART, &host) == VK_SUCCESS);
   const uint32_t before = K.presents;
   CHECK(nvkmd_macos_mem_present(host, 0, 256, 64, 64, 0xCF) == VK_ERROR_SURFACE_LOST_KHR);
   CHECK(K.presents == before);                   /* never reached the kext */
   nvkmd_macos_present_stop(dev);
   CHECK(K.present_stops == 1);
   nvkmd_mem_unref(host);
   nvkmd_mem_unref(scan);

   free(s1); free(s2); free(old); free(young); free(cpu); free(never);
   nvkmd_ctx_destroy(ctx);
   nvkmd_dev_destroy(dev);
   nvkmd_pdev_destroy(pdev);
}

/* Copy-engine contexts: GR by default, CE ring with NVK_MACOS_CE=1. */
static void
test_copy_engine(void)
{
   for (int use_ce = 0; use_ce < 2; use_ce++) {
      memset(&K, 0, sizeof(K));
      if (use_ce)
         setenv("NVK_MACOS_CE", "1", 1);
      else
         unsetenv("NVK_MACOS_CE");
      struct nvkmd_pdev *pdev = NULL;
      CHECK(nvkmd_macos_try_create_pdev(NULL, 0, &pdev) == VK_SUCCESS);
      CHECK(pdev->dev_info.has_transfer_queue == (use_ce != 0));
      struct nvkmd_dev *dev = NULL;
      CHECK(nvkmd_pdev_create_dev(pdev, NULL, &dev) == VK_SUCCESS);
      struct nvkmd_ctx *gr = NULL, *copy = NULL;
      CHECK(nvkmd_dev_create_ctx(dev, NULL, NVKMD_ENGINE_3D | NVKMD_ENGINE_COMPUTE |
                                 NVKMD_ENGINE_COPY, &gr) == VK_SUCCESS);
      CHECK(nvkmd_dev_create_ctx(dev, NULL, NVKMD_ENGINE_COPY, &copy) == VK_SUCCESS);

      struct vk_sync *cs = new_sync(pdev), *gs = new_sync(pdev);
      submit_and_signal(copy, cs);
      CHECK(K.last_exec_engine == (uint32_t)use_ce);
      submit_and_signal(gr, gs);
      CHECK(K.last_exec_engine == 0);

      /* GR context waiting on its own engine's fence: ordered, no kext call */
      K.fence_calls = K.ce_fence_calls = 0;
      struct vk_sync_wait wg = { .sync = gs };
      CHECK(gr->ops->wait(gr, NULL, 1, &wg) == VK_SUCCESS);
      CHECK(K.fence_calls == 0 && K.ce_fence_calls == 0);
      /* GR context waiting on the copy fence: CPU wait only across engines */
      struct vk_sync_wait wc = { .sync = cs };
      CHECK(gr->ops->wait(gr, NULL, 1, &wc) == VK_SUCCESS);
      CHECK(K.ce_fence_calls == (use_ce ? 1u : 0u) && K.fence_calls == 0);
      /* CPU wait on a CE fence goes to selector 29 */
      if (use_ce) {
         struct vk_sync *c2 = new_sync(pdev);
         submit_and_signal(copy, c2);
         K.ce_fence_calls = 0;
         struct vk_sync_wait w2 = { .sync = c2 };
         CHECK(c2->type->wait_many(NULL, 1, &w2, 0, UINT64_MAX) == VK_SUCCESS);
         CHECK(K.ce_fence_calls == 1 && K.fence_calls == 0);
         /* WAIT_ANY across both engines */
         struct vk_sync *g2 = new_sync(pdev), *c3 = new_sync(pdev);
         submit_and_signal(gr, g2);
         submit_and_signal(copy, c3);
         K.ce_fence_calls = 0;
         struct vk_sync_wait any[2] = { { .sync = g2 }, { .sync = c3 } };
         CHECK(g2->type->wait_many(NULL, 2, any, VK_SYNC_WAIT_ANY, UINT64_MAX) == VK_SUCCESS);
         CHECK(K.ce_fence_calls >= 1);
         free(c2); free(g2); free(c3);
      }
      free(cs); free(gs);
      nvkmd_ctx_destroy(copy);
      nvkmd_ctx_destroy(gr);
      nvkmd_dev_destroy(dev);
      nvkmd_pdev_destroy(pdev);
   }
   unsetenv("NVK_MACOS_CE");
}

/*
 * NVK_MACOS_GPU_WAIT=1: a GR wait on a CE fence becomes a host semaphore
 * acquire pushed ahead of the next exec (no CPU wait).
 */
static bool
find_words(const uint32_t *want, int n)
{
   for (uint32_t h = 1; h <= K.next_handle && h < MAXOBJ; h++) {
      const uint32_t *m = K.obj_map[h];
      if (!m || K.obj_domain[h] == 0)
         continue;
      for (uint64_t i = 0; i + n <= K.obj_bytes[h] / 4; i++)
         if (!memcmp(m + i, want, n * 4))
            return true;
   }
   return false;
}

static void
test_gpu_wait(void)
{
   memset(&K, 0, sizeof(K));
   setenv("NVK_MACOS_CE", "1", 1);
   setenv("NVK_MACOS_GPU_WAIT", "1", 1);
   set_prop("NVGspControl-abi", 123, false);
   set_prop("NVGspControl-gr-sem-va", 0x1069FF000ll, false);
   set_prop("NVGspControl-ce-sem-va", 0x108FF0000ll, false);
   struct nvkmd_pdev *pdev = NULL;
   CHECK(nvkmd_macos_try_create_pdev(NULL, 0, &pdev) == VK_SUCCESS);
   struct nvkmd_dev *dev = NULL;
   CHECK(nvkmd_pdev_create_dev(pdev, NULL, &dev) == VK_SUCCESS);
   struct nvkmd_ctx *gr = NULL, *copy = NULL;
   CHECK(nvkmd_dev_create_ctx(dev, NULL, NVKMD_ENGINE_3D | NVKMD_ENGINE_COMPUTE |
                              NVKMD_ENGINE_COPY, &gr) == VK_SUCCESS);
   CHECK(nvkmd_dev_create_ctx(dev, NULL, NVKMD_ENGINE_COPY, &copy) == VK_SUCCESS);
   struct vk_sync *cs = new_sync(pdev), *gs = new_sync(pdev), *g2 = new_sync(pdev);
   submit_and_signal(copy, cs);                 /* CE fence 1 */
   CHECK(K.last_exec_engine == 1);
   K.ce_fence_calls = K.fence_calls = 0;
   struct vk_sync_wait wc = { .sync = cs };
   CHECK(gr->ops->wait(gr, NULL, 1, &wc) == VK_SUCCESS);
   CHECK(K.ce_fence_calls == 0);                /* not waited on the CPU */
   submit_and_signal(gr, gs);
   CHECK(K.last_exec_engine == 0 && K.last_exec_segs == 2 && K.last_seg0_dw == 6);
   const uint32_t acq[6] = { 0x20050017, 0x08FF0000, 0x1, 1, 0, 0x1003 };
   CHECK(find_words(acq, 6));
   /* a wait followed only by a signal still submits the acquire */
   struct vk_sync *c2 = new_sync(pdev);
   submit_and_signal(copy, c2);                 /* CE fence 2 */
   struct vk_sync_wait w2 = { .sync = c2 };
   CHECK(gr->ops->wait(gr, NULL, 1, &w2) == VK_SUCCESS);
   const uint32_t seq_before = K.next_seq;
   struct vk_sync_signal sg = { .sync = g2 };
   CHECK(gr->ops->signal(gr, NULL, 1, &sg) == VK_SUCCESS);
   CHECK(K.next_seq == seq_before + 1 && K.last_exec_segs == 1 && K.last_seg0_dw == 6);
   const uint32_t acq2[6] = { 0x20050017, 0x08FF0000, 0x1, 2, 0, 0x1003 };
   CHECK(find_words(acq2, 6) && K.ce_fence_calls == 0);
   free(cs); free(gs); free(g2); free(c2);
   nvkmd_ctx_destroy(copy);
   nvkmd_ctx_destroy(gr);
   nvkmd_dev_destroy(dev);
   nvkmd_pdev_destroy(pdev);
   unsetenv("NVK_MACOS_CE");
   unsetenv("NVK_MACOS_GPU_WAIT");
   set_prop("NVGspControl-abi", 0, false);
   set_prop("NVGspControl-gr-sem-va", 0, false);
   set_prop("NVGspControl-ce-sem-va", 0, false);
}

/* Small host-visible allocations share 2 MiB slabs. */
static void
test_suballocation(void)
{
   memset(&K, 0, sizeof(K));
   struct nvkmd_pdev *pdev = NULL;
   CHECK(nvkmd_macos_try_create_pdev(NULL, 0, &pdev) == VK_SUCCESS);
   struct nvkmd_dev *dev = NULL;
   CHECK(nvkmd_pdev_create_dev(pdev, NULL, &dev) == VK_SUCCESS);
   enum { N = 4000 };
   static struct nvkmd_mem *m[N];
   for (int i = 0; i < N; i++) {
      const uint64_t sz = (i % 3 == 0) ? 64 : (i % 3 == 1) ? 3000 : 20000;
      CHECK(nvkmd_dev_alloc_mem(dev, NULL, sz, 0, NVKMD_MEM_GART | NVKMD_MEM_CAN_MAP,
                                &m[i]) == VK_SUCCESS);
      CHECK(m[i]->va && (m[i]->va->addr & 4095) == 0);
   }
   /* ~ (1334*4K + 1333*4K + 1333*20K) = ~37 MiB -> ~19 slabs, not 4000 objects */
   CHECK(K.live_objs > 0 && K.live_objs < 40);
   /* disjoint VAs; CPU maps land inside the slab and are writable */
   void *p0 = NULL, *p1 = NULL;
   CHECK(nvkmd_mem_map(m[0], NULL, NVKMD_MEM_MAP_RDWR, NULL, &p0) == VK_SUCCESS);
   CHECK(nvkmd_mem_map(m[1], NULL, NVKMD_MEM_MAP_RDWR, NULL, &p1) == VK_SUCCESS);
   CHECK(p0 != p1);
   memset(p0, 0xab, 64);
   memset(p1, 0xcd, 3000);
   CHECK(((uint8_t *)p0)[63] == 0xab && ((uint8_t *)p1)[0] == 0xcd);
   for (int i = 1; i < 64; i++)
      CHECK(m[i]->va->addr != m[i - 1]->va->addr);
   nvkmd_mem_unmap(m[0], 0);
   nvkmd_mem_unmap(m[1], 0);
   /* big or VRAM allocations still get their own kernel object */
   const int before = K.live_objs;
   struct nvkmd_mem *big = NULL, *vram = NULL;
   CHECK(nvkmd_dev_alloc_mem(dev, NULL, 4 << 20, 0, NVKMD_MEM_GART, &big) == VK_SUCCESS);
   CHECK(nvkmd_dev_alloc_mem(dev, NULL, 4096, 0, NVKMD_MEM_LOCAL, &vram) == VK_SUCCESS);
   CHECK(K.live_objs == before + 2);
   nvkmd_mem_unref(big);
   nvkmd_mem_unref(vram);
   /* free everything: all slabs but one go back to the kext */
   for (int i = 0; i < N; i++)
      nvkmd_mem_unref(m[i]);
   CHECK(K.live_objs == 1);
   /* the kept slab is reused */
   struct nvkmd_mem *again = NULL;
   CHECK(nvkmd_dev_alloc_mem(dev, NULL, 128, 0, NVKMD_MEM_GART, &again) == VK_SUCCESS);
   CHECK(K.live_objs == 1);
   nvkmd_mem_unref(again);
   nvkmd_dev_destroy(dev);
   CHECK(K.live_objs == 0);
   nvkmd_pdev_destroy(pdev);
}

/* ABI 114 kext: 64 KiB binds, VRAM suballocation, kinded rebinds. */
static void
test_abi114(void)
{
   memset(&K, 0, sizeof(K));
   memset(page64, 0, sizeof(page64));
   gran64 = true;
   set_prop("NVGspControl-abi", 114, false);
   struct nvkmd_pdev *pdev = NULL;
   CHECK(nvkmd_macos_try_create_pdev(NULL, 0, &pdev) == VK_SUCCESS);
   CHECK(pdev->bind_align_B == 0x10000);
   struct nvkmd_dev *dev = NULL;
   CHECK(nvkmd_pdev_create_dev(pdev, NULL, &dev) == VK_SUCCESS);

   /* 200 small VRAM allocations share a few 2 MiB objects, 64 KiB aligned */
   enum { N = 200 };
   static struct nvkmd_mem *m[N];
   for (int i = 0; i < N; i++) {
      CHECK(nvkmd_dev_alloc_mem(dev, NULL, 5000 + i * 100, 0, NVKMD_MEM_LOCAL, &m[i]) == VK_SUCCESS);
      CHECK((m[i]->va->addr & 0xffff) == 0 && m[i]->size_B == 0x10000);
   }
   CHECK(K.live_objs == 7);                      /* 200 x 64 KiB / 2 MiB, rounded up */
   void *p = NULL;
   CHECK(nvkmd_mem_map(m[0], NULL, NVKMD_MEM_MAP_RDWR, NULL, &p) != VK_SUCCESS);

   /* depth-image style rebind: own VA with a kind, pointing at a submem */
   struct nvkmd_va *iva = NULL;
   CHECK(nvkmd_dev_alloc_va(dev, NULL, 0, 0x06, 0x10000, 0, 0, &iva) == VK_SUCCESS);
   CHECK((iva->addr & 0xffff) == 0);
   CHECK(nvkmd_va_bind_mem(iva, NULL, 0, m[5], 0, 0x10000) == VK_SUCCESS);
   CHECK(last_bind[1] == iva->addr && last_bind[2] == 0x06 && last_bind[4] == 0x10000);
   CHECK(last_bind[3] % 0x10000 == 0 && last_bind[3] < 0x200000);   /* offset in slab */
   CHECK(page64[(iva->addr - ARENA_BASE) / K64] == last_bind[0]);
   /* unbinding it drops exactly one 64 KiB page */
   CHECK(nvkmd_va_unbind(iva, NULL, 0, 0x10000) == VK_SUCCESS);
   CHECK(page64[(iva->addr - ARENA_BASE) / K64] == 0);
   nvkmd_va_free(iva);

   for (int i = 0; i < N; i++)
      nvkmd_mem_unref(m[i]);
   CHECK(K.live_objs == 1);                      /* one spare VRAM slab */
   nvkmd_dev_destroy(dev);
   CHECK(K.live_objs == 0);
   nvkmd_pdev_destroy(pdev);
   gran64 = false;
   set_prop("NVGspControl-abi", 0, false);
}

/* ABI 123 kext with a BAR1 window: mappable device-local memory in VRAM. */
static void
test_abi123(void)
{
   memset(&K, 0, sizeof(K));
   memset(page64, 0, sizeof(page64));
   gran64 = true;
   K.cpu_window_B = 8ull << 20;
   set_prop("NVGspControl-abi", 123, false);
   set_prop("NVGspControl-vram-cpu-bytes", 7ull << 30, false);
   struct nvkmd_pdev *pdev = NULL;
   CHECK(nvkmd_macos_try_create_pdev(NULL, 0, &pdev) == VK_SUCCESS);
   CHECK(pdev->dev_info.bar_size_B == 7ull << 30);
   struct nvkmd_dev *dev = NULL;
   CHECK(nvkmd_pdev_create_dev(pdev, NULL, &dev) == VK_SUCCESS);
   const enum nvkmd_mem_flags bar = NVKMD_MEM_LOCAL | NVKMD_MEM_CAN_MAP | NVKMD_MEM_COHERENT;
   /* Big mappable device-local -> kext domain 2, CPU map works, counted as VRAM */
   struct nvkmd_mem *a = NULL, *b = NULL, *c = NULL, *d = NULL;
   CHECK(nvkmd_dev_alloc_mem(dev, NULL, 4ull << 20, 0, bar, &a) == VK_SUCCESS);
   CHECK(K.cpu_allocs == 1 && K.obj_domain[K.next_handle] == 2 && K.vram_used == 4ull << 20);
   void *p = NULL;
   CHECK(nvkmd_mem_map(a, NULL, NVKMD_MEM_MAP_RDWR, NULL, &p) == VK_SUCCESS && p);
   memset(p, 0x5a, 4096);
   nvkmd_mem_unmap(a, 0);
   /* Second 4 MiB fills the 8 MiB window; the third falls back to sysmem */
   CHECK(nvkmd_dev_alloc_mem(dev, NULL, 4ull << 20, 0, bar, &b) == VK_SUCCESS);
   CHECK(nvkmd_dev_alloc_mem(dev, NULL, 4ull << 20, 0, bar, &c) == VK_SUCCESS);
   CHECK(K.cpu_allocs == 2 && K.obj_domain[K.next_handle] == 1);
   CHECK(nvkmd_mem_map(c, NULL, NVKMD_MEM_MAP_RDWR, NULL, &p) == VK_SUCCESS && p);
   nvkmd_mem_unmap(c, 0);
   /* small mappable device-local stays in a host-visible slab */
   CHECK(nvkmd_dev_alloc_mem(dev, NULL, 8192, 0, bar, &d) == VK_SUCCESS);
   CHECK(K.cpu_allocs == 2);
   /* plain device-local is still domain 0 */
   struct nvkmd_mem *e = NULL;
   CHECK(nvkmd_dev_alloc_mem(dev, NULL, 4ull << 20, 0, NVKMD_MEM_LOCAL, &e) == VK_SUCCESS);
   CHECK(K.obj_domain[K.next_handle] == 0);
   nvkmd_mem_unref(a);
   nvkmd_mem_unref(b);
   nvkmd_mem_unref(c);
   nvkmd_mem_unref(d);
   nvkmd_mem_unref(e);
   CHECK(K.cpu_window_B == 8ull << 20);
   nvkmd_dev_destroy(dev);
   CHECK(K.live_objs == 0);
   nvkmd_pdev_destroy(pdev);
   gran64 = false;
   set_prop("NVGspControl-abi", 0, false);
   set_prop("NVGspControl-vram-cpu-bytes", 0, false);
}

static void
test_old_kext(void)
{
   memset(&K, 0, sizeof(K));
   K.old_kext = true;
   struct nvkmd_pdev *pdev = NULL;
   CHECK(nvkmd_macos_try_create_pdev(NULL, 0, &pdev) == VK_SUCCESS);
   CHECK(pdev->dev_info.vram_size_B == 14ull << 30);        /* fallback */
   struct nvkmd_dev *dev = NULL;
   CHECK(nvkmd_pdev_create_dev(pdev, NULL, &dev) == VK_SUCCESS);
   struct nvkmd_mem *m = NULL;
   CHECK(nvkmd_dev_alloc_mem(dev, NULL, 6 << 20, 0, NVKMD_MEM_LOCAL, &m) == VK_SUCCESS);
   CHECK(nvkmd_pdev_get_vram_used(pdev) == 6 << 20);        /* local count */
   CHECK(pages_of(1) == 3);
   nvkmd_mem_unref(m);
   CHECK(pages_of(1) == 0 && K.sel20_unbinds == 3);         /* per-page path */
   CHECK(nvkmd_pdev_get_vram_used(pdev) == 0);
   /* present on a kext without present support: surface lost, but no crash */
   CHECK(nvkmd_dev_alloc_mem(dev, NULL, 2 << 20, 0, NVKMD_MEM_LOCAL, &m) == VK_SUCCESS);
   CHECK(nvkmd_macos_mem_present(m, 0, 256, 64, 64, 0xCF) == VK_ERROR_SURFACE_LOST_KHR);
   nvkmd_mem_unref(m);
   nvkmd_dev_destroy(dev);
   nvkmd_pdev_destroy(pdev);
}

int
main(void)
{
   test_new_kext();
   test_copy_engine();
   test_gpu_wait();
   test_suballocation();
   test_abi114();
   test_abi123();
   test_old_kext();
   puts("nvkmd_macos_mock_check: PASS");
   return 0;
}
