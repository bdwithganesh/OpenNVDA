// 0.9.69: nil-return and abort tracing for processes we cannot debug
// (WindowServer is a platform binary; SIP debugging restrictions refuse lldb).
// Off unless /Library/Preferences/nvmtl-niltrace exists, so turning it on or
// off needs no rebuild. When on, every object-returning method of this
// bundle's classes whose arguments are all pointers or integers logs its
// caller's backtrace when it returns nil, and SIGABRT logs the aborting
// thread's backtrace before the default action runs.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <objc/runtime.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include "NVMTLLog.h"

static volatile int nvNilLogged;
enum { kNvNilMaxLogs = 64, kNvNilFrames = 14 };

static void nvNilTraceLog(const char *what, int skip) {
    void *pc[kNvNilFrames + 4];
    const int n = backtrace(pc, kNvNilFrames + 4);
    NSMutableString *s = [NSMutableString stringWithFormat:@"NVMTL_NILTRACE %s:", what];
    for (int i = skip; i < n; i++) {
        Dl_info di;
        if (dladdr(pc[i], &di) && di.dli_sname)
            [s appendFormat:@" | %s+%lu", di.dli_sname, (unsigned long)((char *)pc[i] - (char *)di.dli_saddr)];
        else
            [s appendFormat:@" | %p", pc[i]];
    }
    NSLog(@"%@", s);
}

// only types that travel in integer registers: ids, classes, selectors,
// pointers and C integers (no structs, floats or vectors)
static BOOL nvNilTraceArgsOk(Method m) {
    const unsigned n = method_getNumberOfArguments(m);
    if (n > 6) return NO;
    for (unsigned i = 2; i < n; i++) {
        char t[64];
        method_getArgumentType(m, i, t, sizeof t);
        const char *p = t;
        while (*p && strchr("rnNoORV", *p)) p++;   // qualifiers
        if (!*p || !strchr("@#:^*qQiIlLsScCB", *p)) return NO;
    }
    return YES;
}

static BOOL nvNilTraceHook(Class c, Method m) {
    char rt[64];
    method_getReturnType(m, rt, sizeof rt);
    if (rt[0] != '@' || !nvNilTraceArgsOk(m)) return NO;
    const SEL sel = method_getName(m);
    const IMP orig = method_getImplementation(m);
    const unsigned na = method_getNumberOfArguments(m) - 2;
    NSString *label = [NSString stringWithFormat:@"%s.%s", class_getName(c), sel_getName(sel)];
    id blk = nil;
#define NV_NIL_CHECK(r) do { if (!(r) && nvNilLogged < kNvNilMaxLogs) { nvNilLogged++; nvNilTraceLog(label.UTF8String, 2); } } while (0)
    if (na == 0)
        blk = ^id(id s) { id r = ((id (*)(id, SEL))orig)(s, sel); NV_NIL_CHECK(r); return r; };
    else if (na == 1)
        blk = ^id(id s, void *a) { id r = ((id (*)(id, SEL, void *))orig)(s, sel, a); NV_NIL_CHECK(r); return r; };
    else if (na == 2)
        blk = ^id(id s, void *a, void *b) {
            id r = ((id (*)(id, SEL, void *, void *))orig)(s, sel, a, b); NV_NIL_CHECK(r); return r; };
    else if (na == 3)
        blk = ^id(id s, void *a, void *b, void *d) {
            id r = ((id (*)(id, SEL, void *, void *, void *))orig)(s, sel, a, b, d); NV_NIL_CHECK(r); return r; };
    else if (na == 4)
        blk = ^id(id s, void *a, void *b, void *d, void *e) {
            id r = ((id (*)(id, SEL, void *, void *, void *, void *))orig)(s, sel, a, b, d, e); NV_NIL_CHECK(r); return r; };
    else
        return NO;
#undef NV_NIL_CHECK
    method_setImplementation(m, imp_implementationWithBlock(blk));
    return YES;
}

// 0.9.70: CoreAnimation can give up on a transient surface without asking
// us for a texture; log the heap budget answers it bases that on.
static volatile int nvHeapLogged;
enum { kNvHeapMaxLogs = 400 };
#define NV_HEAP_LOG(...) do { if (nvHeapLogged < kNvHeapMaxLogs) { nvHeapLogged++; NSLog(__VA_ARGS__); } } while (0)

static void nvHeapTraceHooks(void) {
    Class heap = objc_getClass("NVMTLHeap"), dev = objc_getClass("NVMTLDevice");
    Method m;
    if (heap && (m = class_getInstanceMethod(heap, @selector(maxAvailableSizeWithAlignment:)))) {
        const IMP o = method_getImplementation(m);
        method_setImplementation(m, imp_implementationWithBlock(^NSUInteger(id h, NSUInteger a) {
            const NSUInteger r = ((NSUInteger (*)(id, SEL, NSUInteger))o)(h, @selector(maxAvailableSizeWithAlignment:), a);
            NV_HEAP_LOG(@"NVMTL_HEAPTRACE avail heap %p size %lu used %lu align %lu -> %lu", h,
                        (unsigned long)[h size], (unsigned long)[h usedSize], (unsigned long)a, (unsigned long)r);
            return r;
        }));
    }
    if (heap && (m = class_getInstanceMethod(heap, @selector(newTextureWithDescriptor:)))) {
        const IMP o = method_getImplementation(m);
        method_setImplementation(m, imp_implementationWithBlock(^id(id h, MTLTextureDescriptor *td) {
            id r = ((id (*)(id, SEL, id))o)(h, @selector(newTextureWithDescriptor:), td);
            NV_HEAP_LOG(@"NVMTL_HEAPTRACE tex heap %p size %lu used %lu fmt %lu %lux%lu usage 0x%lx -> %s", h,
                        (unsigned long)[h size], (unsigned long)[h usedSize], (unsigned long)td.pixelFormat,
                        (unsigned long)td.width, (unsigned long)td.height, (unsigned long)td.usage, r ? "ok" : "NIL");
            return r;
        }));
    }
    if (dev && (m = class_getInstanceMethod(dev, @selector(newHeapWithDescriptor:)))) {
        const IMP o = method_getImplementation(m);
        method_setImplementation(m, imp_implementationWithBlock(^id(id d, MTLHeapDescriptor *hd) {
            id r = ((id (*)(id, SEL, id))o)(d, @selector(newHeapWithDescriptor:), hd);
            NV_HEAP_LOG(@"NVMTL_HEAPTRACE newheap %p size %lu type %ld storage %lu", r, (unsigned long)hd.size,
                        (long)hd.type, (unsigned long)hd.storageMode);
            return r;
        }));
    }
    if (dev && (m = class_getInstanceMethod(dev, @selector(heapTextureSizeAndAlignWithDescriptor:)))) {
        const IMP o = method_getImplementation(m);
        method_setImplementation(m, imp_implementationWithBlock(^MTLSizeAndAlign(id d, MTLTextureDescriptor *td) {
            const MTLSizeAndAlign r =
                ((MTLSizeAndAlign (*)(id, SEL, id))o)(d, @selector(heapTextureSizeAndAlignWithDescriptor:), td);
            NV_HEAP_LOG(@"NVMTL_HEAPTRACE sizealign fmt %lu %lux%lu mips %lu -> %lu / %lu", (unsigned long)td.pixelFormat,
                        (unsigned long)td.width, (unsigned long)td.height, (unsigned long)td.mipmapLevelCount,
                        (unsigned long)r.size, (unsigned long)r.align);
            return r;
        }));
    }
}

static struct sigaction nvOldAbort;
static void nvNilTraceAbort(int sig, siginfo_t *si, void *uc) {
    nvNilTraceLog("SIGABRT", 1);
    sigaction(SIGABRT, &nvOldAbort, NULL);
    (void)si; (void)uc;
    raise(sig);
}

__attribute__((constructor)) static void nvNilTraceInit(void) {
    if (access("/Library/Preferences/nvmtl-niltrace", F_OK) != 0) return;
    Dl_info self;
    if (!dladdr((const void *)&nvNilTraceInit, &self) || !self.dli_fname) return;
    unsigned nc = 0, hooked = 0;
    const char **names = objc_copyClassNamesForImage(self.dli_fname, &nc);
    for (unsigned i = 0; i < nc; i++) {
        Class c = objc_getClass(names[i]);
        unsigned nm = 0;
        Method *ms = c ? class_copyMethodList(c, &nm) : NULL;
        for (unsigned j = 0; j < nm; j++) {
            const char *s = sel_getName(method_getName(ms[j]));
            if (!strncmp(s, "new", 3) || !strncmp(s, "commandBuffer", 13) || strstr(s, "Encoder")) {
                if (nvNilTraceHook(c, ms[j])) hooked++;
            }
        }
        free(ms);
    }
    free(names);
    nvHeapTraceHooks();
    struct sigaction sa = {0};
    sa.sa_sigaction = nvNilTraceAbort;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGABRT, &sa, &nvOldAbort);
    NSLog(@"NVMTL_NILTRACE on in %s: %u methods of %u classes", getprogname(), hooked, nc);
}
