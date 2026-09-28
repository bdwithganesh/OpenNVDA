// NVMTLDriver.bundle: the user half of Metal on the RTX 4080.
// Metal.framework creates NSPrincipalClass with -initWithAcceleratorPort:
// for every IOAccelerator whose MetalPluginName names this bundle;
// MTLIOAccelDevice (private, inside Metal.framework) opens NVAccelerator's
// IOAcceleratorFamily2 device/shared user clients.
//
// What works so far: buffers, command queues (we have to override the
// descriptor variant, otherwise _MTLDevice and MTLIOAccelDevice just keep
// tail-calling each other forever), blit fill/copy on the copy engine, compute
// through our own shader path, and a simple triangle render. Blits and draws
// run synchronously at commit time for now.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <os/lock.h>
#import <objc/message.h>
#include <IOKit/IOKitLib.h>
#import <IOSurface/IOSurface.h>
#import <CoreVideo/CoreVideo.h>
#import "NVMTLGsp.h"
#import "NVMTLCompiler.h"
#import "NVMTLRender.h"
#import "NVMTLTexHw.h"
#import "NVMTLBufferVa.h"
static uint64_t nvBufVa(id<MTLBuffer> b, NSUInteger off, NSUInteger len);
static char kNVVramKey;   // tentative, defined with the VRAM holder
// the process's device, for objects that don't keep their own (MPS
// checks texture.device == device and asserts on nil)
static __weak id gNVDevice;
static void nvResolve(id ms, id dst, NSUInteger level, NSUInteger slice);
static BOOL nvGpuResolve(id cb, id ms, id dst, NSUInteger level, NSUInteger slice);

// MTLStorageModeManaged, the discrete-GPU way: the CPU copy lives in
// the host heap (contents), the GPU copy in VRAM. didModifyRange: marks CPU
// writes, which go up before the next command buffer runs;
// synchronizeResource: (blit) brings GPU writes back down.
// root of the objects we hand out. The classes take their Metal
// protocols with class_addProtocol, so a method we never wrote used to throw
// straight out of WindowServer (SkyLight's -setLabel: on a texture killed the
// login). Unknown protocol methods now log once and return zero/nil.
@interface NVMTLObject : NSObject
@property (nonatomic, copy) NSString *label;
@end
static const char *nvProtoTypes(Class c, SEL sel) {
    for (; c; c = class_getSuperclass(c)) {
        unsigned n = 0;
        Protocol * __unsafe_unretained *pl = class_copyProtocolList(c, &n);
        for (unsigned i = 0; i < n; ++i) {
            for (int req = 1; req >= 0; --req) {
                struct objc_method_description d = protocol_getMethodDescription(pl[i], sel, req, YES);
                if (d.types) { free(pl); return d.types; }
            }
        }
        free(pl);
    }
    return NULL;
}
static NSMethodSignature *nvFallbackSig(id self, SEL _cmd, SEL sel) {
    struct objc_super sup = {self, class_getSuperclass(object_getClass(self))};
    // walk up to the first class that isn't carrying the fallback itself
    Class c = object_getClass(self);
    while (c && class_getMethodImplementation(c, @selector(methodSignatureForSelector:)) == (IMP)nvFallbackSig)
        c = class_getSuperclass(c);
    sup.super_class = c ? c : [NSObject class];
    NSMethodSignature *m = ((NSMethodSignature *(*)(struct objc_super *, SEL, SEL))objc_msgSendSuper)(&sup, _cmd, sel);
    if (m) return m;
    const char *t = nvProtoTypes(object_getClass(self), sel);
    return [NSMethodSignature signatureWithObjCTypes:t ? t : "v@:"];
}
static void nvFallbackInvoke(id self, SEL _cmd, NSInvocation *inv) {
    static NSMutableSet *seen; static dispatch_once_t once;
    dispatch_once(&once, ^{ seen = [NSMutableSet set]; });
    NSString *key = [NSString stringWithFormat:@"%s %@", class_getName(object_getClass(self)),
                     NSStringFromSelector(inv.selector)];
    BOOL first = NO;
    @synchronized (seen) { if (![seen containsObject:key]) { [seen addObject:key]; first = YES; } }
    if (first) NSLog(@"NVMTLDriver: unimplemented -[%@], returning zero", key);
    const NSUInteger len = inv.methodSignature.methodReturnLength;
    if (len) { void *z = calloc(1, len); [inv setReturnValue:z]; free(z); }
}
// also for the classes built on Apple's MTLIOAccel* ones (encoders,
// command buffer, queue, device), which the NVMTLObject root doesn't cover
static void nvInstallFallback(Class c) {
    class_addMethod(c, @selector(methodSignatureForSelector:), (IMP)nvFallbackSig, "@@::");
    class_addMethod(c, @selector(forwardInvocation:), (IMP)nvFallbackInvoke, "v@:@");
}
// Apple's MTLIOAccel*/_MTLCommandEncoder bases define many protocol
// methods as stubs that raise doesNotRecognizeSelector themselves, so the
// fallback above never saw them (respondsToSelector: says YES). The ones we
// don't implement are sent to _objc_msgForward, i.e. into the fallback.
static void nvForwardStubs(Class cls, Protocol *p, const char *const *names) {
    for (; *names; ++names) {
        SEL sel = sel_registerName(*names);
        Method own = NULL;
        unsigned n = 0;
        Method *ml = class_copyMethodList(cls, &n);
        for (unsigned i = 0; i < n; ++i) if (method_getName(ml[i]) == sel) own = ml[i];
        free(ml);
        if (own) continue;                        // we implement it ourselves
        struct objc_method_description d = protocol_getMethodDescription(p, sel, YES, YES);
        if (!d.types) d = protocol_getMethodDescription(p, sel, NO, YES);
        if (d.types) class_addMethod(cls, sel, _objc_msgForward, d.types);
    }
}
@implementation NVMTLObject
+ (void)initialize { if (self == [NVMTLObject class]) nvInstallFallback(self); }
- (id)device { return gNVDevice; }   // subclasses with their own device property override this
@end

@interface NVManaged : NSObject
@property (nonatomic) uint64_t heapVa, vramVa, len, lo, hi;   // dirty [lo, hi)
@end
@implementation NVManaged
@end
static char kNVManagedKey;
static NSHashTable *gManagedDirty;
static os_unfair_lock gManagedLock = OS_UNFAIR_LOCK_INIT;
static void nvManagedMark(id buf, NSUInteger lo, NSUInteger hi) {
    NVManaged *m = objc_getAssociatedObject(buf, &kNVManagedKey);
    if (!m || lo >= hi) return;
    if (hi > m.len) hi = m.len;
    os_unfair_lock_lock(&gManagedLock);
    if (m.lo >= m.hi) { m.lo = lo; m.hi = hi; }
    else { if (lo < m.lo) m.lo = lo; if (hi > m.hi) m.hi = hi; }
    if (!gManagedDirty) gManagedDirty = [NSHashTable weakObjectsHashTable];
    [gManagedDirty addObject:buf];
    os_unfair_lock_unlock(&gManagedLock);
}
// upload every dirty managed buffer (called before a command buffer runs)
static void nvManagedFlush(void) {
    os_unfair_lock_lock(&gManagedLock);
    NSArray *all = gManagedDirty.allObjects;
    [gManagedDirty removeAllObjects];
    NSMutableArray *todo = [NSMutableArray new];
    for (id b in all) {
        NVManaged *m = objc_getAssociatedObject(b, &kNVManagedKey);
        if (!m || m.lo >= m.hi) continue;
        [todo addObject:@[m, @(m.lo), @(m.hi)]];
        m.lo = m.hi = 0;
    }
    os_unfair_lock_unlock(&gManagedLock);
    for (NSArray *t in todo) {
        NVManaged *m = t[0];
        const uint64_t lo = [t[1] unsignedLongLongValue], hi = [t[2] unsignedLongLongValue];
        if (!nvCeCopy(m.vramVa + lo, m.heapVa + lo, hi - lo)) NSLog(@"NVMTLDriver: managed upload failed");
    }
}
static void (*gOrigDidModify)(id, SEL, NSRange);
static void nvDidModifyRange(id self, SEL _cmd, NSRange r) {
    if (gOrigDidModify) gOrigDidModify(self, _cmd, r);
    nvManagedMark(self, r.location, r.location + r.length);
}

// PTE kind for block-linear colour: GENERIC_MEMORY, which is 0x06 on Turing+
// (NIL; 0 is the pitch kind, 8 the compressible one that needs comptags)
#define NV_KIND_COLOR 6u
static bool nvComputeBlit(BOOL fill, uint64_t dst, uint64_t src, uint64_t bytes, uint32_t value);
static bool nvCopy2D(uint64_t dst, uint64_t dpitch, uint64_t src, uint64_t spitch,
                     uint64_t rowBytes, uint64_t rows);

// Private base classes, declared only as far as used. The real superclass
// chain (MTLIOAccelDevice : _MTLDevice : ...) is resolved by the runtime.
@interface MTLIOAccelDevice : NSObject
- (instancetype)initWithAcceleratorPort:(io_service_t)port;
- (id)newLibraryWithSource:(NSString *)src options:(id)opts error:(NSError **)err;
- (id)newLibraryWithData:(dispatch_data_t)data error:(NSError **)err;
- (id)newLibraryWithURL:(NSURL *)url error:(NSError **)err;
- (id)newLibraryWithFile:(NSString *)path error:(NSError **)err;
@end

@interface MTLIOAccelBuffer : NSObject
- (instancetype)initWithDevice:(id)device pointer:(void *)ptr length:(NSUInteger)len
                       options:(NSUInteger)options sysMemSize:(uint64_t)sys vidMemSize:(uint64_t)vid
                          args:(void *)args argsSize:(uint32_t)argsSize deallocator:(id)dealloc;
@end

@interface MTLIOAccelCommandQueue : NSObject
// id, not MTLCommandQueueDescriptor: the class is macOS 15+ in the SDK,
// the selector works on 14.x.
- (instancetype)initWithDevice:(id)device descriptor:(id)desc;
@end

@interface NVMTLDevice : MTLIOAccelDevice
@end

// As AppleParavirtCommandQueue : MTLIOAccelCommandQueue: init is a pure super
// call today; commandBuffer overrides land here.
@interface MTLIOAccelCommandBuffer : NSObject
- (instancetype)initWithQueue:(id)q retainedReferences:(BOOL)retained;
- (void)commit;
@end

@interface MTLIOAccelRenderCommandEncoder : NSObject
- (instancetype)initWithCommandBuffer:(id)cb;
- (void)endEncoding;
@end

// a heap sub-resource's share of its heap's budget, kept on the
// resource as an associated object; it comes back when the resource goes
// or on makeAliasable. Vision/MPS make and drop hundreds of sub-resources
// against a few heaps (3 Vision requests: 27 heaps, 653 sub-resources,
// 500 freed, maxAvailableSizeWithAlignment: before each allocation). With
// the budget never returned the heaps filled after a few allocations and
// mediaanalysisd got nil resources at login.
@interface NVHeapLease : NSObject
@property (nonatomic, weak) id heap;          // NVMTLHeap
@property (nonatomic) NSUInteger leaseBytes;
@property (nonatomic) BOOL returned;
- (void)nvReturn;
@end
static char kNvHeapLeaseKey;
static NVHeapLease *nvLeaseOf(id r) { return r ? objc_getAssociatedObject(r, &kNvHeapLeaseKey) : nil; }

// v1 texture: RGBA8/BGRA8Unorm 2D over a shared buffer (no
// MTLIOAccelTexture: its init goes through the Paravirt host serializer).
@interface NVMTLTexture : NVMTLObject
@property (nonatomic) id<MTLBuffer> buf;
@property (nonatomic) NSUInteger w, h;
@property (nonatomic) MTLPixelFormat fmt;
@property (nonatomic) uint32_t bpp, fcode;   // bytes per texel, shader format code
@property (nonatomic) NSUInteger pitch;      // bytes per row (tight: w * bpp)
@property (nonatomic) MTLTextureUsage use;
@property (nonatomic) BOOL priv;              // StorageModePrivate, in VRAM (no CPU view)
@property (nonatomic) uint32_t tic;            // hardware texture header, 0 = not made yet
@property (nonatomic) uint64_t blVa;           // block-linear VRAM surface (depth), 0 = linear
@property (nonatomic) uint32_t blHandle, blBh; // its kext object, GOBs-per-block height log2
// block-linear twin of a pitch colour target, for passes with depth
// (the 3D class refuses a depth target next to a pitch colour target)
@property (nonatomic) uint64_t shVa;
@property (nonatomic) uint32_t shHandle, shBh;
// block-linear texture (mipmaps / arrays / cube / 3D) in VRAM
@property (nonatomic) NSData *lay;             // NVTexLayout
@property (nonatomic) MTLTextureType type;
@property (nonatomic) NSUInteger dep, layersN, levelsN;
@property (nonatomic) BOOL ticSwap;            // stored BGRA: shader writes swap red and blue
@property (nonatomic) NSUInteger samples;      // MSAA sample count (0/1 = single)
// texture views share the parent's memory (and keep it alive)
@property (nonatomic, strong) id parent;
@property (nonatomic) NSUInteger parentLevel, parentSlice;
@property (nonatomic) IOSurfaceRef ios;          // backing surface (not retained; buf's deallocator holds it)
@property (nonatomic) NSUInteger iosPlane;
@property (nonatomic) void *alCpu;              // aligned copy for sampling (tight pitch)
@property (nonatomic) uint64_t alVa;
@end

// linear 2D formats and the format codes the compiler's texture
// helpers understand (NVMTLCompiler.m nvTextureHelpers). Zero = unsupported.
// depth/stencil formats: SET_ZT_FORMAT value, what they hold
static bool nvZetaFormat(MTLPixelFormat f, uint32_t *zt, bool *hasZ, bool *hasS) {
    uint32_t v = 0; bool z = true, st = false;
    switch (f) {
    case MTLPixelFormatDepth16Unorm: v = 0x13; break;                           // Z16
    case MTLPixelFormatDepth32Float: v = 0x0a; break;                           // ZF32
    case MTLPixelFormatStencil8: v = 0x17; z = false; st = true; break;         // S8
    case MTLPixelFormatDepth24Unorm_Stencil8: v = 0x16; st = true; break;       // S8Z24: z low 24, s high 8
    case MTLPixelFormatDepth32Float_Stencil8: v = 0x19; st = true; break;       // ZF32_X24S8
    default: return false;
    }
    if (zt) *zt = v;
    if (hasZ) *hasZ = z;
    if (hasS) *hasS = st;
    return true;
}
// PTE kinds as NIL's tu102_choose_pte_kind (uncompressed)
static uint32_t nvZetaKind(MTLPixelFormat f) {
    switch (f) {
    case MTLPixelFormatDepth16Unorm: return 0x01;              // Z16
    case MTLPixelFormatStencil8: return 0x02;                  // S8
    case MTLPixelFormatDepth24Unorm_Stencil8: return 0x03;     // S8Z24
    case MTLPixelFormatDepth32Float_Stencil8: return 0x04;     // ZF32_X24S8
    default: return 0x06;                                      // GENERIC_MEMORY (Z32_FLOAT)
    }
}
static uint32_t nvZetaBytes(MTLPixelFormat f) {
    switch (f) {
    case MTLPixelFormatDepth16Unorm: return 2;
    case MTLPixelFormatStencil8: return 1;
    case MTLPixelFormatDepth32Float_Stencil8: return 8;
    case MTLPixelFormatDepth24Unorm_Stencil8: case MTLPixelFormatDepth32Float: return 4;
    default: return 0;
    }
}

// NIL sample layouts: samples per pixel in x/y, SET_ANTI_ALIAS mode
static bool nvSampleLayout(NSUInteger n, uint32_t *sx, uint32_t *sy, uint32_t *mode) {
    uint32_t x = 1, y = 1, m = 0;
    switch (n) {
    case 0: case 1: break;
    case 2: x = 2; m = 1; break;              // 2X1
    case 4: x = 2; y = 2; m = 2; break;       // 2X2
    case 8: x = 4; y = 2; m = 3; break;       // 4X2
    default: return false;
    }
    if (sx) *sx = x;
    if (sy) *sy = y;
    if (mode) *mode = m;
    return true;
}

static uint32_t nvTexFormat(MTLPixelFormat f, uint32_t *bpp) {
    static const struct { MTLPixelFormat f; uint32_t code, bpp; } k[] = {
        {MTLPixelFormatR8Unorm, 1, 1},      {MTLPixelFormatRG8Unorm, 2, 2},
        {MTLPixelFormatRGBA8Unorm, 3, 4},   {MTLPixelFormatBGRA8Unorm, 4, 4},
        {MTLPixelFormatR16Float, 5, 2},     {MTLPixelFormatRG16Float, 6, 4},
        {MTLPixelFormatRGBA16Float, 7, 8},  {MTLPixelFormatR32Float, 8, 4},
        {MTLPixelFormatRG32Float, 9, 8},    {MTLPixelFormatRGBA32Float, 10, 16},
        {MTLPixelFormatR32Uint, 11, 4},     {MTLPixelFormatRGBA8Uint, 12, 4},
        {MTLPixelFormatRGBA32Uint, 13, 16}, {MTLPixelFormatR8Uint, 14, 1},
        {MTLPixelFormatDepth32Float, 8, 4},   // depth, R32F texels
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        if (k[i].f == f) { if (bpp) *bpp = k[i].bpp; return k[i].code; }
    return 0;
}
@implementation NVMTLTexture
+ (void)load { class_addProtocol(self, @protocol(MTLTexture)); }
// heap membership (a view answers for its parent's memory)
- (id)heap { NVHeapLease *l = nvLeaseOf(self); return l ? l.heap : [_parent respondsToSelector:@selector(heap)] ? [_parent heap] : nil; }
- (NSUInteger)heapOffset { return 0; }        // never placed: own memory
- (BOOL)isAliasable { NVHeapLease *l = nvLeaseOf(self); return l && l.returned; }
- (void)makeAliasable { [nvLeaseOf(self) nvReturn]; }
- (void)dealloc {
    nvTicFree(_tic); nvVramFreeKind(_blHandle); nvVramFreeKind(_shHandle);
    if (_alCpu) nvHeapFree(_alCpu, ((_pitch + 127) & ~(NSUInteger)127) * _h);
}
// MTLTexture properties WindowServer reads
- (IOSurfaceRef)iosurface { return _ios ? _ios : (_parent ? [(NVMTLTexture *)_parent iosurface] : NULL); }
- (NSUInteger)iosurfacePlane { return _iosPlane; }
- (BOOL)isFramebufferOnly { return NO; }
- (BOOL)isShareable { return NO; }
- (BOOL)allowGPUOptimizedContents { return YES; }
- (MTLTextureCompressionType)compressionType { return MTLTextureCompressionTypeLossless; }
- (MTLTextureSwizzleChannels)swizzle { return MTLTextureSwizzleChannelsDefault; }
- (id)rootResource { return _parent ? [(NVMTLTexture *)_parent rootResource] : self; }
- (NSUInteger)protectionOptions { return 0; }
- (void)setResponsibleProcess:(pid_t)pid { (void)pid; }
// WindowServer asks these of surface textures; nothing is ever purged
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)st { (void)st; return MTLPurgeableStateNonVolatile; }
- (void)didModifyData { }
// MTLTextureSPI/MTLResourceSPI that WindowServer's IOPresentment
// path uses; zero from the fallback would read as "not complete yet"
- (BOOL)isComplete { return YES; }
- (BOOL)isWriteComplete { return YES; }
- (void)waitUntilComplete { }
- (BOOL)isPurgeable { return NO; }
- (BOOL)isDrawable { return NO; }
- (BOOL)isCompressed { return NO; }
- (NSUInteger)compressionFootprint { return 0; }
- (NSUInteger)numFaces { return _type == MTLTextureTypeCube || _type == MTLTextureTypeCubeArray ? 6 : 1; }
- (NSUInteger)rotation { return 0; }
- (uint64_t)allocationID { return (uint64_t)(uintptr_t)self; }
- (uint64_t)gpuHandle { return [self gpuResourceID]._impl; }
- (MTLResourceOptions)unfilteredResourceOptions { return [(id<MTLTexture>)self resourceOptions]; }
- (BOOL)doesAliasResource:(id)r { return r == self; }
- (BOOL)doesAliasAnyResources:(const id *)r count:(NSUInteger)n { for (NSUInteger i = 0; i < n; i++) if (r[i] == self) return YES; return NO; }
- (BOOL)doesAliasAllResources:(const id *)r count:(NSUInteger)n { for (NSUInteger i = 0; i < n; i++) if (r[i] != self) return NO; return n > 0; }

// argument buffers hold this; AIR code treats a texture value as
// tic | flags << 32, the same word the dispatch pushes
static uint32_t nvTicType(MTLTextureType t) {
    switch (t) {
    case MTLTextureType1D: return 0;          case MTLTextureType1DArray: return 4;
    case MTLTextureType3D: return 2;          case MTLTextureTypeCube: return 3;
    case MTLTextureType2DArray: return 5;     case MTLTextureTypeCubeArray: return 8;
    default: return 1;
    }
}
- (MTLResourceID)gpuResourceID {
    if (!_tic && _lay) {
        bool swap = false;
        _tic = nvTicAllocBL(_fmt, _blVa, nvTicType(_type), _lay.bytes, &swap);
        _ticSwap = swap;
    }
    if (!_tic && !_alCpu) {
        bool swap = false;
        const uint64_t va = nvBufVa(_buf, 0, _pitch * _h);
        _tic = va ? nvTicAlloc(_fmt, va, (uint32_t)_pitch, (uint32_t)_w, (uint32_t)_h, &swap) : 0;
        _ticSwap = swap;
        // the header needs a 32-byte aligned pitch and address; a
        // linear texture on an app buffer or surface with a tighter row
        // (an 8x8 RG8 is 16 bytes a row) is sampled from an aligned copy
        const NSUInteger ap = (_pitch + 127) & ~(NSUInteger)127;
        if (!_tic && _buf.contents && ((_pitch & 31) || (va & 31)) && nvHeapAlloc(ap * _h, &_alCpu, &_alVa)) {
            _tic = nvTicAlloc(_fmt, _alVa, (uint32_t)ap, (uint32_t)_w, (uint32_t)_h, &swap);
            _ticSwap = swap;
            if (!_tic) { nvHeapFree(_alCpu, ap * _h); _alCpu = NULL; _alVa = 0; }
        }
    }
    if (_alCpu) {                       // refresh the aligned copy for this use
        const NSUInteger ap = (_pitch + 127) & ~(NSUInteger)127, row = _w * _bpp;
        const uint8_t *src = _buf.contents;
        for (NSUInteger y = 0; y < _h; y++) memcpy((uint8_t *)_alCpu + y * ap, src + y * _pitch, row);
    }
    MTLResourceID r;
    r._impl = (uint64_t)_tic | (uint64_t)(_ticSwap ? 1 : 0) << 32;
    return r;
}
- (NSUInteger)width { return _w; }
- (NSUInteger)height { return _h; }
- (NSUInteger)depth { return _lay && _type == MTLTextureType3D ? _dep : 1; }
- (NSUInteger)arrayLength {
    if (!_lay) return 1;
    return _type == MTLTextureTypeCube ? 1 : _type == MTLTextureTypeCubeArray ? _layersN / 6 : _layersN;
}
- (NSUInteger)mipmapLevelCount { return _lay ? _levelsN : 1; }
- (NSUInteger)sampleCount { return _samples > 1 ? _samples : 1; }
- (MTLTextureType)textureType { return _samples > 1 ? MTLTextureType2DMultisample : _lay ? _type : MTLTextureType2D; }
// texture views. Block-linear: a sub-range of levels/slices is the
// same NIL layout started at the base level (the block sizes of a level
// only depend on its own size), with the parent's array stride.
- (id)parentTexture { return _parent; }
- (NSUInteger)parentRelativeLevel { return _parentLevel; }
- (NSUInteger)parentRelativeSlice { return _parentSlice; }
- (id)newTextureViewWithPixelFormat:(MTLPixelFormat)f {
    const NSUInteger layers = _lay ? ((const NVTexLayout *)_lay.bytes)->layers : 1;
    return [self newTextureViewWithPixelFormat:f textureType:self.textureType
                                        levels:NSMakeRange(0, self.mipmapLevelCount) slices:NSMakeRange(0, layers)];
}
- (id)newTextureViewWithPixelFormat:(MTLPixelFormat)f textureType:(MTLTextureType)tt levels:(NSRange)lv
                             slices:(NSRange)sl swizzle:(MTLTextureSwizzleChannels)sw {
    if (sw.red != MTLTextureSwizzleRed || sw.green != MTLTextureSwizzleGreen || sw.blue != MTLTextureSwizzleBlue ||
        sw.alpha != MTLTextureSwizzleAlpha)
        NSLog(@"NVMTLDriver: texture view swizzle not applied yet");
    return [self newTextureViewWithPixelFormat:f textureType:tt levels:lv slices:sl];
}
- (id)newTextureViewWithPixelFormat:(MTLPixelFormat)f textureType:(MTLTextureType)tt levels:(NSRange)lv
                             slices:(NSRange)sl {
    uint32_t bpp = nvFormatBytes(f);
    if (!bpp) nvTexFormat(f, &bpp);
    if (bpp != _bpp || !lv.length || !sl.length) { NSLog(@"NVMTLDriver: texture view format %lu unsupported", (unsigned long)f); return nil; }
    NVMTLTexture *v = [NVMTLTexture new];
    v.fmt = f; v.bpp = _bpp; v.use = _use; v.priv = _priv; v.parent = _parent ? _parent : self;
    v.parentLevel = _parentLevel + lv.location; v.parentSlice = _parentSlice + sl.location;
    if (!_lay) {
        if (lv.location || sl.location || lv.length > 1 || sl.length > 1) return nil;
        v.buf = _buf; v.w = _w; v.h = _h; v.pitch = _pitch; v.fcode = nvTexFormat(f, NULL);
        return v;
    }
    const NVTexLayout *L = _lay.bytes;
    if (lv.location + lv.length > L->levels || sl.location + sl.length > L->layers) return nil;
    const bool is3D = _type == MTLTextureType3D;
    NVTexLayout V;
    nvTexLayoutInit(&V, MAX(L->w >> lv.location, 1u), MAX(L->h >> lv.location, 1u),
                    is3D ? MAX(L->d >> lv.location, 1u) : 1, (uint32_t)sl.length, (uint32_t)lv.length, L->bpp, is3D);
    for (NSUInteger k = 0; k < lv.length; k++)
        if (V.offset[k] != L->offset[lv.location + k] - L->offset[lv.location] || V.ylog[k] != L->ylog[lv.location + k]) {
            NSLog(@"NVMTLDriver: texture view layout mismatch at level %lu", (unsigned long)(lv.location + k));
            return nil;
        }
    V.arrayStride = L->arrayStride;
    V.size = V.arrayStride * V.layers;
    v.blVa = _blVa + sl.location * L->arrayStride + L->offset[lv.location];
    v.blBh = V.ylog[0];
    v.lay = [NSData dataWithBytes:&V length:sizeof V];
    v.w = V.w; v.h = V.h; v.pitch = V.rowBytes[0];
    v.type = tt; v.dep = V.d; v.layersN = V.layers; v.levelsN = V.levels;
    v.samples = (lv.location == 0 && lv.length == L->levels) ? _samples : 0;
    return v;
}
// the samples of a multisampled texture seen as a (w*sx) x (h*sy) 2D texture
- (NVMTLTexture *)nvSampleGridView {
    if (_samples < 2 || !_lay) return nil;
    const NVTexLayout *L = _lay.bytes;
    NVMTLTexture *v = [NVMTLTexture new];
    v.fmt = _fmt; v.bpp = _bpp; v.priv = YES; v.parent = self;
    v.blVa = _blVa; v.blBh = L->ylog[0]; v.lay = _lay;
    v.w = L->w; v.h = L->h; v.pitch = L->rowBytes[0];
    v.type = MTLTextureType2D; v.dep = 1; v.layersN = 1; v.levelsN = 1;
    return v;
}
// CPU access to a block-linear texture: through a linear staging
// buffer and the copy engine, one depth slice at a time
- (BOOL)nvBLCopy:(BOOL)up region:(MTLRegion)r level:(NSUInteger)l slice:(NSUInteger)sl
           bytes:(void *)pix bytesPerRow:(NSUInteger)bpr bytesPerImage:(NSUInteger)bpi {
    const NVTexLayout *L = _lay.bytes;
    if (l >= L->levels || sl >= L->layers || !r.size.width || !r.size.height) return NO;
    const uint32_t rowB = (uint32_t)(r.size.width * _bpp);
    const uint32_t pitch = (rowB + 255) & ~255u;
    const uint64_t bytes = (uint64_t)pitch * r.size.height;
    void *cpu = NULL;
    uint64_t va = 0;
    if (!nvHeapAlloc(bytes, &cpu, &va)) return NO;
    const uint64_t base = _blVa + sl * L->arrayStride + L->offset[l];
    BOOL ok = YES;
    const NSUInteger zs = r.size.depth ? r.size.depth : 1;
    for (NSUInteger z = 0; z < zs && ok; z++) {
        uint8_t *img = (uint8_t *)pix + z * bpi;
        if (up) for (NSUInteger y = 0; y < r.size.height; y++) memcpy((uint8_t *)cpu + y * pitch, img + y * bpr, rowB);
        ok = nvCeCopyBLRegion(up, va, pitch, base, L->rowBytes[l], L->rows[l], L->depth[l], L->ylog[l], L->zlog[l],
                              (uint32_t)(r.origin.x * _bpp), (uint32_t)r.origin.y, (uint32_t)(r.origin.z + z),
                              rowB, (uint32_t)r.size.height);
        if (ok && !up) for (NSUInteger y = 0; y < r.size.height; y++) memcpy(img + y * bpr, (uint8_t *)cpu + y * pitch, rowB);
    }
    nvHeapFree(cpu, bytes);
    return ok;
}
- (MTLPixelFormat)pixelFormat { return _fmt; }
- (MTLTextureUsage)usage { return _use ? _use : (MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget); }
- (MTLStorageMode)storageMode { return _priv ? MTLStorageModePrivate : MTLStorageModeShared; }
- (id)buffer { return _buf; }
- (NSUInteger)bufferOffset { return 0; }
- (NSUInteger)bufferBytesPerRow { return _pitch; }
- (void)getBytes:(void *)pix bytesPerRow:(NSUInteger)bpr fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)l {
    if (_lay) { [self getBytes:pix bytesPerRow:bpr bytesPerImage:bpr * r.size.height fromRegion:r mipmapLevel:l slice:0]; return; }
    if (l || !pix || r.origin.x + r.size.width > _w || r.origin.y + r.size.height > _h) return;
    if (!_buf.contents) { NSLog(@"NVMTLDriver: getBytes on a private texture (use a blit)"); return; }
    uint8_t *s = (uint8_t *)_buf.contents + r.origin.y * _pitch + r.origin.x * _bpp;
    for (NSUInteger y = 0; y < r.size.height; y++)
        memcpy((uint8_t *)pix + y * bpr, s + y * _pitch, r.size.width * _bpp);
}
- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)l withBytes:(const void *)pix bytesPerRow:(NSUInteger)bpr {
    if (_lay) { [self replaceRegion:r mipmapLevel:l slice:0 withBytes:pix bytesPerRow:bpr bytesPerImage:bpr * r.size.height]; return; }
    if (l || !pix || r.origin.x + r.size.width > _w || r.origin.y + r.size.height > _h) return;
    if (!_buf.contents) { NSLog(@"NVMTLDriver: replaceRegion on a private texture (use a blit)"); return; }
    uint8_t *d = (uint8_t *)_buf.contents + r.origin.y * _pitch + r.origin.x * _bpp;
    for (NSUInteger y = 0; y < r.size.height; y++)
        memcpy(d + y * _pitch, (const uint8_t *)pix + y * bpr, r.size.width * _bpp);
}
- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)l slice:(NSUInteger)sl withBytes:(const void *)pix
          bytesPerRow:(NSUInteger)bpr bytesPerImage:(NSUInteger)bpi {
    if (_lay) { if (![self nvBLCopy:YES region:r level:l slice:sl bytes:(void *)pix bytesPerRow:bpr bytesPerImage:bpi])
                    NSLog(@"NVMTLDriver: replaceRegion (block linear) failed");
                return; }
    (void)bpi; if (!sl) [self replaceRegion:r mipmapLevel:l withBytes:pix bytesPerRow:bpr];
}
- (void)getBytes:(void *)pix bytesPerRow:(NSUInteger)bpr bytesPerImage:(NSUInteger)bpi
      fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)l slice:(NSUInteger)sl {
    if (_lay) { if (![self nvBLCopy:NO region:r level:l slice:sl bytes:pix bytesPerRow:bpr bytesPerImage:bpi])
                    NSLog(@"NVMTLDriver: getBytes (block linear) failed");
                return; }
    (void)bpi; if (!sl) [self getBytes:pix bytesPerRow:bpr fromRegion:r mipmapLevel:l];
}
@end

// fences are just inert markers for now (all execution is synchronous, so every
// wait is trivially satisfied); shared events carry a real atomic value
// with max-semantics GPU signals and listener notifications.
@interface NVMTLFence : NVMTLObject
@property (nonatomic, weak) id device;
@end
@implementation NVMTLFence
+ (void)load { class_addProtocol(self, @protocol(MTLFence)); }
@end

// sampler state = one mode word for the shader helpers:
// bit0 linear (mag filter), bits 2:1 address mode (s axis), bit3 pixel coords.
@interface NVMTLSamplerState : NVMTLObject
@property (nonatomic) uint32_t mode;
@property (nonatomic) uint32_t tsc;            // hardware sampler index
@property (nonatomic, weak) id device;
@end
@implementation NVMTLSamplerState
+ (void)load { class_addProtocol(self, @protocol(MTLSamplerState)); }
- (void)dealloc { nvTscFree(_tsc); }
- (MTLResourceID)gpuResourceID { MTLResourceID r; r._impl = _tsc; return r; }
@end

@interface NVMTLSharedEvent : NVMTLObject
@property (nonatomic, weak) id device;
- (uint64_t)signaledValue;
- (void)setSignaledValue:(uint64_t)v;
- (void)nvSignal:(uint64_t)v; // GPU-side: max semantics + listener fire
@end
@implementation NVMTLSharedEvent {
    _Atomic uint64_t _val;
    NSMutableArray *_pending; // {l: listener, v: value, b: block}
}
+ (void)load { class_addProtocol(self, @protocol(MTLSharedEvent)); class_addProtocol(self, @protocol(MTLEvent)); }
- (instancetype)init {
    self = [super init];
    if (self) _pending = [NSMutableArray new];
    return self;
}
- (uint64_t)signaledValue { return atomic_load(&_val); }
- (void)setSignaledValue:(uint64_t)v { atomic_store(&_val, v); [self nvFire]; }
- (void)nvSignal:(uint64_t)v {
    uint64_t cur = atomic_load(&_val);
    while (v > cur && !atomic_compare_exchange_weak(&_val, &cur, v)) {}
    [self nvFire];
}
- (void)nvFire {
    NSArray *due;
    @synchronized (self) {
        NSMutableArray *keep = [NSMutableArray new], *fire = [NSMutableArray new];
        const uint64_t cur = atomic_load(&_val);
        for (NSDictionary *p in _pending)
            [[p[@"v"] unsignedLongLongValue] <= cur ? fire : keep addObject:p];
        _pending = keep;
        due = fire;
    }
    for (NSDictionary *p in due) {
        MTLSharedEventListener *l = p[@"l"];
        const uint64_t v = [p[@"v"] unsignedLongLongValue];
        MTLSharedEventNotificationBlock b = p[@"b"];
        dispatch_async(l.dispatchQueue, ^{ b((id<MTLSharedEvent>)self, v); });
    }
}
- (void)notifyListener:(MTLSharedEventListener *)l atValue:(uint64_t)v
                 block:(MTLSharedEventNotificationBlock)b {
    if (!l || !b) return;
    if (atomic_load(&_val) >= v) { dispatch_async(l.dispatchQueue, ^{ b((id<MTLSharedEvent>)self, v); }); return; }
    @synchronized (self) {
        if (atomic_load(&_val) >= v) { dispatch_async(l.dispatchQueue, ^{ b((id<MTLSharedEvent>)self, v); }); return; }
        [_pending addObject:@{@"l": l, @"v": @(v), @"b": [b copy]}];
    }
}
- (MTLSharedEventHandle *)newSharedEventHandle {
    NSLog(@"NVMTLDriver: no cross-process event handles yet");
    return nil;
}
@end

@interface MTLIOAccelBlitCommandEncoder : NSObject
- (instancetype)initWithCommandBuffer:(id)cb;
- (void)endEncoding;
@end

// Blit: the encoder only records ops; -commit executes them on the CE
// ring (NVMTLGsp) and copies results to the shared buffers' CPU mappings.
// v2 moves submission behind a kext vendor command (see NVMTLGsp.h).
@class NVMTLComputePipelineState;
@class NVMTLRenderPipelineState;

@interface NVMTLCommandBuffer : MTLIOAccelCommandBuffer
- (void)nvAddFill:(id)buf range:(NSRange)r value:(uint8_t)v;
- (void)nvAddCopy:(id)src so:(NSUInteger)so dst:(id)dst do:(NSUInteger)do_ n:(NSUInteger)n;
- (void)nvAddSignal:(id)ev value:(uint64_t)v;
- (void)nvAddWait:(id)ev value:(uint64_t)v;
- (void)nvAddDispatch:(NVMTLComputePipelineState *)ps bufs:(NSArray *)bufs texs:(NSArray *)texs
                samps:(NSArray *)samps grid:(MTLSize)grid block:(MTLSize)block;
- (void)nvExecuteComputeOp:(NSDictionary *)op;
- (void)nvAddDispatch:(NVMTLComputePipelineState *)ps bufs:(NSArray *)bufs texs:(NSArray *)texs
                samps:(NSArray *)samps indirect:(id<MTLBuffer>)ib offset:(NSUInteger)io block:(MTLSize)block;
- (void)nvAddDraw:(NVMTLRenderPipelineState *)ps vbufs:(NSArray *)vbufs
               vp:(MTLViewport)vp sc:(MTLScissorRect)sc tex:(NVMTLTexture *)tex
             load:(MTLLoadAction)load clear:(MTLClearColor)clear
            vStart:(NSUInteger)vStart vCount:(NSUInteger)vCount idx:(NSData *)idx;
- (void)nvExecuteDrawOp:(NSDictionary *)op;
- (void)nvAddDraw2:(NSDictionary *)op;
- (void)nvExecuteDraw2Op:(NSDictionary *)op;
- (void)nvAddOp:(NSDictionary *)op;
@end

// depth compare function + write enable (no stencil)
@interface NVMTLDepthStencilState : NVMTLObject
@property (nonatomic) uint32_t mode;          // bits 2:0 MTLCompareFunction, bit 3 write
// stencil, [0] front [1] back: fail, depth fail, pass (OGL ops),
// func (OGL), read mask, write mask
@property (nonatomic) BOOL stencil;
@property (nonatomic) NSData *sten;           // uint32_t[2][6]
@property (nonatomic, weak) id device;
@end
@implementation NVMTLDepthStencilState
+ (void)load { class_addProtocol(self, @protocol(MTLDepthStencilState)); }
@end

// what a texture takes, as the device allocates it: every mip
// level of every slice, rows padded, samples counted (was w * h * 16 for
// any texture, too small for mipmapped, array and 3D textures).
static NSUInteger nvHeapTextureBytes(MTLTextureDescriptor *td) {
    NSUInteger bpp = nvFormatBytes(td.pixelFormat);
    if (!bpp) bpp = 16;
    const BOOL cube = td.textureType == MTLTextureTypeCube || td.textureType == MTLTextureTypeCubeArray;
    const NSUInteger layers = (td.arrayLength ? td.arrayLength : 1) * (cube ? 6 : 1);
    const NSUInteger samples = td.sampleCount ? td.sampleCount : 1;
    NSUInteger bytes = 0;
    for (NSUInteger i = 0; i < (td.mipmapLevelCount ? td.mipmapLevelCount : 1); i++) {
        const NSUInteger w = MAX(td.width >> i, (NSUInteger)1), h = MAX(td.height >> i, (NSUInteger)1),
                         d = MAX(td.depth >> i, (NSUInteger)1);
        bytes += ((w * bpp + 255) & ~(NSUInteger)255) * h * d;
    }
    return (bytes * layers * samples + 0xffff) & ~(NSUInteger)0xffff;
}

// MTLHeap. Resources come from the device allocators (heap
// buffers are already GPU-mapped host memory); the heap keeps the budget.
@interface NVMTLHeap : NVMTLObject
@property (nonatomic, weak) id device;
@property (nonatomic) NSUInteger size, usedSize;
@property (nonatomic) MTLStorageMode storageMode;
@property (nonatomic) MTLCPUCacheMode cpuCacheMode;
@property (nonatomic) MTLHazardTrackingMode hazardTrackingMode;
@property (nonatomic) MTLHeapType type;
@end
@implementation NVMTLHeap
+ (void)load { class_addProtocol(self, @protocol(MTLHeap)); }
- (MTLResourceOptions)resourceOptions {
    return (MTLResourceOptions)_storageMode << MTLResourceStorageModeShift |
           (MTLResourceOptions)_cpuCacheMode << MTLResourceCPUCacheModeShift;
}
- (NSUInteger)currentAllocatedSize { return _size; }
- (NSUInteger)protectionOptions { return 0; }
- (NSUInteger)maxAvailableSizeWithAlignment:(NSUInteger)a {
    (void)a; return _usedSize < _size ? _size - _usedSize : 0;
}
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)st { (void)st; return MTLPurgeableStateNonVolatile; }
- (BOOL)nvTake:(NSUInteger)n {
    @synchronized (self) {
        if (_usedSize + n > _size) return NO;
        _usedSize += n;
    }
    return YES;
}
- (void)nvGive:(NSUInteger)n {
    @synchronized (self) { _usedSize = n < _usedSize ? _usedSize - n : 0; }
}
// Resources keep their own memory (heapOffset 0), so an aliasable one only
// gives its share of the budget back, nothing is overwritten.
- (id)nvLease:(id)r bytes:(NSUInteger)n {
    if (!r) { [self nvGive:n]; return nil; }
    NVHeapLease *l = [NVHeapLease new];
    l.heap = self; l.leaseBytes = n;
    objc_setAssociatedObject(r, &kNvHeapLeaseKey, l, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return r;
}
- (id)newBufferWithLength:(NSUInteger)len options:(MTLResourceOptions)opts {
    const NSUInteger n = (len + 0xffff) & ~(NSUInteger)0xffff;
    if (![self nvTake:n]) return nil;
    return [self nvLease:[(id<MTLDevice>)_device newBufferWithLength:len options:opts] bytes:n];
}
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)td {
    const NSUInteger n = nvHeapTextureBytes(td);
    if (![self nvTake:n]) return nil;
    return [self nvLease:[(id<MTLDevice>)_device newTextureWithDescriptor:td] bytes:n];
}
@end
@implementation NVHeapLease
- (void)nvReturn {
    if (_returned) return;
    _returned = YES;
    [(NVMTLHeap *)_heap nvGive:_leaseBytes];
}
- (void)dealloc { [self nvReturn]; }
@end

@interface NVMTLRenderPipelineState : NVMTLObject
// precompiled vertex/fragment functions on the 3D engine
@property (nonatomic) NVMTLKernel *hwVS, *hwFS;
// tessellation: hwVS is the generated control point fetch, hwTES the
// app's post-tessellation vertex function, hwTCS the generated factor stage
@property (nonatomic) NVMTLKernel *hwTCS, *hwTES;
@property (nonatomic) float maxTessFactor;
@property (nonatomic) MTLIndexType cpIndexType;
@property (nonatomic) MTLVertexDescriptor *vdesc;
@property (nonatomic) NSArray *colorAtt;      // MTLRenderPipelineColorAttachmentDescriptor copies
@property (nonatomic) NVMTLKernel *vs;
@property (nonatomic) NVMTLFragment *fs;      // raster v2 when fs.raster
@property (nonatomic) uint32_t blend;         // raster v2 blend word (0 = off)
@property (nonatomic, weak) id device;
- (void)nvSetFsColor:(const float *)c;
- (const float *)nvFsColor;
@end
@implementation NVMTLRenderPipelineState {
    float _fsColor[4];
}
+ (void)load { class_addProtocol(self, @protocol(MTLRenderPipelineState)); }
- (void)nvSetFsColor:(const float *)c { memcpy(_fsColor, c, sizeof _fsColor); }
- (const float *)nvFsColor { return _fsColor; }
@end

@interface NVMTLRenderEncoder : MTLIOAccelRenderCommandEncoder
- (void)nvSetCB:(id)cb;
- (void)nvSetDesc:(MTLRenderPassDescriptor *)desc;
@end
@implementation NVMTLRenderEncoder {
    NVMTLCommandBuffer *_nvCb;
    MTLRenderPassDescriptor *_desc;
    NVMTLRenderPipelineState *_ps;
    NSMutableArray *_vbufs;
    NSMutableArray *_fbufs, *_ftexs, *_fsamps;   // fragment bindings
    BOOL _drew;                                  // the load action ran already
    NVMTLDepthStencilState *_ds;
    id<MTLBuffer> _pendInd; NSUInteger _pendIndOff;   // next nvDraw2Start is indirect
    NSUInteger _pendInst;                            // next indexed draw's instances (0 = 1)
    MTLViewport _vp; BOOL _hasVp;
    MTLScissorRect _sc; BOOL _hasSc;
    NSMutableArray *_vtexs, *_vsamps;            // vertex-stage textures / samplers
    MTLCullMode _cull;
    MTLWinding _winding;
    BOOL _zdrew;                                 // the depth load action ran already
    uint32_t _sref[2];                           // stencil reference front/back
    id<MTLBuffer> _tfBuf; NSUInteger _tfOff;       // tessellation factor buffer
    NSDictionary *_pendPatch;                      // next nvDraw3D is a patch draw
}
+ (void)load { class_addProtocol(self, @protocol(MTLRenderCommandEncoder)); }
- (instancetype)initWithCommandBuffer:(id)cb {
    self = [super initWithCommandBuffer:cb];
    if (self) {
        _vbufs = [NSMutableArray new];
        _fbufs = [NSMutableArray new]; _ftexs = [NSMutableArray new]; _fsamps = [NSMutableArray new];
        _vtexs = [NSMutableArray new]; _vsamps = [NSMutableArray new];
        for (int i = 0; i < 16; i++) { [_vtexs addObject:[NSNull null]]; [_vsamps addObject:[NSNull null]]; }
        for (int i = 16; i < 31; i++) {                  // buffers go up to index 30
            [_vbufs addObject:[NSNull null]]; [_fbufs addObject:[NSNull null]];
        }
        for (int i = 0; i < 16; i++) {
            [_vbufs addObject:[NSNull null]];
            [_fbufs addObject:[NSNull null]]; [_ftexs addObject:[NSNull null]]; [_fsamps addObject:[NSNull null]];
        }
        // textures up to 128 per stage, as Metal allows (was 16)
        for (int i = 16; i < 128; i++) { [_vtexs addObject:[NSNull null]]; [_ftexs addObject:[NSNull null]]; }
    }
    return self;
}
- (void)nvSetCB:(id)cb { _nvCb = cb; }
- (void)nvSetDesc:(MTLRenderPassDescriptor *)desc { _desc = desc; }
- (void)setRenderPipelineState:(id)ps { _ps = ps; }
- (void)setVertexBuffer:(id)buf offset:(NSUInteger)off atIndex:(NSUInteger)idx {
    if (idx < 31) _vbufs[idx] = buf ? @{@"buf": buf, @"off": @(off)} : (id)[NSNull null];
}
- (id)nvBytesBuffer:(const void *)bytes length:(NSUInteger)len {
    uint32_t zero = 0;
    return [(id<MTLDevice>)[(id<MTLCommandBuffer>)_nvCb device] newBufferWithBytes:(len ? bytes : &zero)
                                                                          length:(len ? len : 4)
                                                                         options:MTLResourceStorageModeShared];
}
- (void)setVertexBytes:(const void *)bytes length:(NSUInteger)len atIndex:(NSUInteger)idx {
    [self setVertexBuffer:[self nvBytesBuffer:bytes length:len] offset:0 atIndex:idx];
}
- (void)setDepthStencilState:(id)ds { _ds = ds; }
// tessellation
- (void)setTessellationFactorBuffer:(id)b offset:(NSUInteger)off instanceStride:(NSUInteger)is {
    (void)is;   // per-instance factors: not yet (the TCS has no instance id)
    _tfBuf = b; _tfOff = off;
}
- (void)setTessellationFactorScale:(float)s { (void)s; }
- (void)nvDrawPatches:(NSUInteger)cps start:(NSUInteger)ps count:(NSUInteger)pc cpIndex:(id)cib cpOffset:(NSUInteger)cio
            instances:(NSUInteger)ni baseInstance:(NSUInteger)bi {
    [self nvDrawPatches:cps start:ps count:pc patchIndex:nil patchOffset:0 cpIndex:cib cpOffset:cio instances:ni
           baseInstance:bi];
}
- (void)nvDrawPatches:(NSUInteger)cps start:(NSUInteger)ps count:(NSUInteger)pc patchIndex:(id)pib patchOffset:(NSUInteger)pio
              cpIndex:(id)cib cpOffset:(NSUInteger)cio instances:(NSUInteger)ni baseInstance:(NSUInteger)bi {
    if (!_ps.hwTES || !_tfBuf || !pc || !ni) {
        NSLog(@"NVMTLDriver: drawPatches needs a tessellation pipeline and a factor buffer"); return;
    }
    if (cps != _ps.hwTES.tessCps) { NSLog(@"NVMTLDriver: drawPatches with %lu control points, pipeline has %u",
                                          (unsigned long)cps, _ps.hwTES.tessCps); return; }
    NSMutableDictionary *pt = [@{@"cps": @(cps), @"start": @(ps), @"tf": _tfBuf, @"tfOff": @(_tfOff)} mutableCopy];
    if (pib) {   // patch i is patchIndex[patchStart + i]: gathered when the draw runs
        pt[@"pib"] = pib; pt[@"pio"] = @(pio); pt[@"pc"] = @(pc);
        if (cib) { pt[@"cib"] = cib; pt[@"cio"] = @(cio); }
    }
    _pendPatch = pt;
    if (pib) {
        [self nvDraw3D:MTLPrimitiveTypeTriangle start:0 count:pc * cps instances:ni baseInstance:bi indexBuffer:nil
           indexOffset:0 indexType:MTLIndexTypeUInt32 baseVertex:0];
        return;
    }
    [self nvDraw3D:MTLPrimitiveTypeTriangle start:cib ? 0 : ps * cps count:pc * cps instances:ni baseInstance:bi
       indexBuffer:cib indexOffset:cib ? cio + ps * cps * (_ps.cpIndexType == MTLIndexTypeUInt16 ? 2 : 4) : 0
         indexType:_ps.cpIndexType baseVertex:0];
}
- (void)drawPatches:(NSUInteger)cps patchStart:(NSUInteger)ps patchCount:(NSUInteger)pc patchIndexBuffer:(id)pib
    patchIndexBufferOffset:(NSUInteger)pio instanceCount:(NSUInteger)ni baseInstance:(NSUInteger)bi {
    [self nvDrawPatches:cps start:ps count:pc patchIndex:pib patchOffset:pio cpIndex:nil cpOffset:0 instances:ni
           baseInstance:bi];
}
- (void)drawIndexedPatches:(NSUInteger)cps patchStart:(NSUInteger)ps patchCount:(NSUInteger)pc
          patchIndexBuffer:(id)pib patchIndexBufferOffset:(NSUInteger)pio controlPointIndexBuffer:(id)cib
    controlPointIndexBufferOffset:(NSUInteger)cio instanceCount:(NSUInteger)ni baseInstance:(NSUInteger)bi {
    [self nvDrawPatches:cps start:ps count:pc patchIndex:pib patchOffset:pio cpIndex:cib cpOffset:cio instances:ni
           baseInstance:bi];
}
- (void)setStencilReferenceValue:(uint32_t)v { _sref[0] = _sref[1] = v; }
- (void)setStencilFrontReferenceValue:(uint32_t)f backReferenceValue:(uint32_t)b { _sref[0] = f; _sref[1] = b; }
- (void)setFragmentBuffer:(id)buf offset:(NSUInteger)off atIndex:(NSUInteger)idx {
    if (idx < 31) _fbufs[idx] = buf ? @{@"buf": buf, @"off": @(off)} : (id)[NSNull null];
}
- (void)setFragmentBytes:(const void *)bytes length:(NSUInteger)len atIndex:(NSUInteger)idx {
    [self setFragmentBuffer:[self nvBytesBuffer:bytes length:len] offset:0 atIndex:idx];
}
- (void)setFragmentTexture:(id)tex atIndex:(NSUInteger)idx {
    if (idx < 128) _ftexs[idx] = tex ? tex : (id)[NSNull null];
}
- (void)setFragmentTextures:(const id __unsafe_unretained *)texs withRange:(NSRange)r {
    for (NSUInteger i = 0; i < r.length; i++) [self setFragmentTexture:texs[i] atIndex:r.location + i];
}
- (void)setVertexTexture:(id)tex atIndex:(NSUInteger)idx {
    if (idx < 128) _vtexs[idx] = tex ? tex : (id)[NSNull null];
}
- (void)setVertexSamplerState:(id)smp atIndex:(NSUInteger)idx {
    if (idx < 16) _vsamps[idx] = smp ? smp : (id)[NSNull null];
}
// the rest of the binding surface CoreAnimation and apps use
static const char *const kNvRenderStubs[] = {
    "setObjectBuffer:offset:atIndex:",
    "setMeshSamplerStates:withRange:",
    "drawMeshThreadgroupsWithIndirectBuffer:indirectBufferOffset:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:",
    "executeCommandsInBuffer:indirectBuffer:indirectBufferOffset:",
    "executeCommandsInBuffer:withRange:",
    "sampleCountersInBuffer:atSampleIndex:withBarrier:",
    "setFragmentVisibleFunctionTable:atBufferIndex:",
    "setFragmentVisibleFunctionTables:withBufferRange:",
    "setMeshBuffer:offset:atIndex:",
    "setMeshBufferOffset:atIndex:",
    "setMeshBuffers:offsets:withRange:",
    "setMeshBytes:length:atIndex:",
    "setMeshSamplerState:atIndex:",
    "setMeshSamplerState:lodMinClamp:lodMaxClamp:atIndex:",
    "setMeshSamplerStates:lodMinClamps:lodMaxClamps:withRange:",
    "setMeshTexture:atIndex:",
    "setMeshTextures:withRange:",
    "setObjectBufferOffset:atIndex:",
    "setObjectBuffers:offsets:withRange:",
    "setObjectBytes:length:atIndex:",
    "setObjectSamplerState:atIndex:",
    "setObjectSamplerState:lodMinClamp:lodMaxClamp:atIndex:",
    "setObjectSamplerStates:lodMinClamps:lodMaxClamps:withRange:",
    "setObjectSamplerStates:withRange:",
    "setObjectTexture:atIndex:",
    "setObjectTextures:withRange:",
    "setObjectThreadgroupMemoryLength:atIndex:",
    "setTileVisibleFunctionTable:atBufferIndex:",
    "setTileVisibleFunctionTables:withBufferRange:",
    "setVertexAmplificationCount:viewMappings:",
    "setVertexVisibleFunctionTable:atBufferIndex:",
    "setVertexVisibleFunctionTables:withBufferRange:",
    "drawMeshThreadgroups:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:",
    "drawMeshThreads:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:",
    NULL};
+ (void)initialize {
    if (self != [NVMTLRenderEncoder class]) return;
    nvInstallFallback(self);
    nvForwardStubs(self, @protocol(MTLRenderCommandEncoder), kNvRenderStubs);
}
// base-class stubs that throw, done for real. Submission is serial and
// all our memory stays resident, so barriers and residency calls are no-ops.
- (void)memoryBarrierWithScope:(MTLBarrierScope)sc afterStages:(MTLRenderStages)a beforeStages:(MTLRenderStages)b {
    (void)sc; (void)a; (void)b;
}
- (void)memoryBarrierWithResources:(const id __unsafe_unretained *)r count:(NSUInteger)n
                       afterStages:(MTLRenderStages)a beforeStages:(MTLRenderStages)b {
    (void)r; (void)n; (void)a; (void)b;
}
- (void)useResource:(id)r usage:(MTLResourceUsage)u stages:(MTLRenderStages)st { (void)r; (void)u; (void)st; }
- (void)useResources:(const id __unsafe_unretained *)r count:(NSUInteger)n usage:(MTLResourceUsage)u
              stages:(MTLRenderStages)st { (void)r; (void)n; (void)u; (void)st; }
- (void)useHeap:(id)h stages:(MTLRenderStages)st { (void)h; (void)st; }
- (void)useHeaps:(const id __unsafe_unretained *)h count:(NSUInteger)n stages:(MTLRenderStages)st { (void)h; (void)n; (void)st; }
- (void)useResource:(id)r usage:(MTLResourceUsage)u { (void)r; (void)u; }
- (void)useResources:(const id __unsafe_unretained *)r count:(NSUInteger)n usage:(MTLResourceUsage)u { (void)r; (void)n; (void)u; }
- (void)useHeap:(id)h { (void)h; }
- (void)useHeaps:(const id __unsafe_unretained *)h count:(NSUInteger)n { (void)h; (void)n; }
// we always store (the pass descriptor's action was taken at encoder start)
- (void)setColorStoreAction:(MTLStoreAction)a atIndex:(NSUInteger)i { (void)a; (void)i; }
- (void)setDepthStoreAction:(MTLStoreAction)a { (void)a; }
- (void)setStencilStoreAction:(MTLStoreAction)a { (void)a; }
- (void)setColorStoreActionOptions:(MTLStoreActionOptions)o atIndex:(NSUInteger)i { (void)o; (void)i; }
- (void)setDepthStoreActionOptions:(MTLStoreActionOptions)o { (void)o; }
- (void)setStencilStoreActionOptions:(MTLStoreActionOptions)o { (void)o; }
// dynamic attribute strides: the pipeline's vertex descriptor stride is used
- (void)setVertexBuffer:(id)buf offset:(NSUInteger)off attributeStride:(NSUInteger)st atIndex:(NSUInteger)idx {
    (void)st; [self setVertexBuffer:buf offset:off atIndex:idx];
}
- (void)setVertexBufferOffset:(NSUInteger)off attributeStride:(NSUInteger)st atIndex:(NSUInteger)idx {
    (void)st; [self setVertexBufferOffset:off atIndex:idx];
}
- (void)setVertexBuffers:(const id __unsafe_unretained *)bufs offsets:(const NSUInteger *)offs
        attributeStrides:(const NSUInteger *)st withRange:(NSRange)r {
    (void)st; [self setVertexBuffers:bufs offsets:offs withRange:r];
}
- (void)setVertexBytes:(const void *)bytes length:(NSUInteger)len attributeStride:(NSUInteger)st atIndex:(NSUInteger)idx {
    (void)st; [self setVertexBytes:bytes length:len atIndex:idx];
}
- (void)setVertexBufferOffset:(NSUInteger)off atIndex:(NSUInteger)idx {
    id cur = idx < 31 ? _vbufs[idx] : nil;
    if ([cur isKindOfClass:[NSDictionary class]]) [self setVertexBuffer:cur[@"buf"] offset:off atIndex:idx];
}
- (void)setFragmentBufferOffset:(NSUInteger)off atIndex:(NSUInteger)idx {
    id cur = idx < 31 ? _fbufs[idx] : nil;
    if ([cur isKindOfClass:[NSDictionary class]]) [self setFragmentBuffer:cur[@"buf"] offset:off atIndex:idx];
}
- (void)setVertexBuffers:(const id __unsafe_unretained *)bufs offsets:(const NSUInteger *)offs withRange:(NSRange)r {
    for (NSUInteger i = 0; i < r.length; i++) [self setVertexBuffer:bufs[i] offset:offs ? offs[i] : 0 atIndex:r.location + i];
}
- (void)setFragmentBuffers:(const id __unsafe_unretained *)bufs offsets:(const NSUInteger *)offs withRange:(NSRange)r {
    for (NSUInteger i = 0; i < r.length; i++) [self setFragmentBuffer:bufs[i] offset:offs ? offs[i] : 0 atIndex:r.location + i];
}
- (void)setVertexTextures:(const id __unsafe_unretained *)texs withRange:(NSRange)r {
    for (NSUInteger i = 0; i < r.length; i++) [self setVertexTexture:texs[i] atIndex:r.location + i];
}
- (void)setVertexSamplerStates:(const id __unsafe_unretained *)smps withRange:(NSRange)r {
    for (NSUInteger i = 0; i < r.length; i++) [self setVertexSamplerState:smps[i] atIndex:r.location + i];
}
// LOD clamps live in the sampler here; the plain binding is what we can honour
- (void)setVertexSamplerState:(id)smp lodMinClamp:(float)a lodMaxClamp:(float)b atIndex:(NSUInteger)idx {
    (void)a; (void)b; [self setVertexSamplerState:smp atIndex:idx];
}
- (void)setFragmentSamplerState:(id)smp lodMinClamp:(float)a lodMaxClamp:(float)b atIndex:(NSUInteger)idx {
    (void)a; (void)b; [self setFragmentSamplerState:smp atIndex:idx];
}
- (void)setVertexSamplerStates:(const id __unsafe_unretained *)smps lodMinClamps:(const float *)a
                  lodMaxClamps:(const float *)b withRange:(NSRange)r {
    (void)a; (void)b; [self setVertexSamplerStates:smps withRange:r];
}
- (void)setFragmentSamplerStates:(const id __unsafe_unretained *)smps lodMinClamps:(const float *)a
                    lodMaxClamps:(const float *)b withRange:(NSRange)r {
    (void)a; (void)b; [self setFragmentSamplerStates:smps withRange:r];
}
- (void)setViewports:(const MTLViewport *)vps count:(NSUInteger)n { if (n && vps) [self setViewport:vps[0]]; }
- (void)setScissorRects:(const MTLScissorRect *)rs count:(NSUInteger)n { if (n && rs) [self setScissorRect:rs[0]]; }
- (void)setBlendColorRed:(float)r green:(float)g blue:(float)b alpha:(float)a { (void)r; (void)g; (void)b; (void)a; }
- (void)setDepthBias:(float)b slopeScale:(float)s clamp:(float)c { (void)b; (void)s; (void)c; }
- (void)setDepthClipMode:(MTLDepthClipMode)m { (void)m; }
- (void)setTriangleFillMode:(MTLTriangleFillMode)m { (void)m; }
- (void)setVisibilityResultMode:(MTLVisibilityResultMode)m offset:(NSUInteger)o { (void)m; (void)o; }
- (void)setThreadgroupMemoryLength:(NSUInteger)l offset:(NSUInteger)o atIndex:(NSUInteger)i { (void)l; (void)o; (void)i; }
- (void)setCullMode:(MTLCullMode)m { _cull = m; }
- (void)setFrontFacingWinding:(MTLWinding)w { _winding = w; }
// a draw on the 3D engine (pipelines built from precompiled functions)
- (void)nvDraw3D:(MTLPrimitiveType)t start:(NSUInteger)s count:(NSUInteger)n instances:(NSUInteger)ni
    baseInstance:(NSUInteger)bi indexBuffer:(id<MTLBuffer>)ib indexOffset:(NSUInteger)io
       indexType:(MTLIndexType)it baseVertex:(NSInteger)bv {
    const bool clearOnly = !n && !ni;   // from endEncoding, a pass without draws
    if (!_desc || (!clearOnly && (!n || !ni || !_ps))) return;
    NSMutableArray *rts = [NSMutableArray new];
    for (NSUInteger i = 0; i < 8; i++) {
        MTLRenderPassColorAttachmentDescriptor *a = _desc.colorAttachments[i];
        NVMTLTexture *tex = a.texture;
        if (![tex isKindOfClass:objc_getClass("NVMTLTexture")]) break;
        const MTLClearColor cc = a.clearColor;
        [rts addObject:@{@"tex": tex, @"load": @(_drew ? MTLLoadActionLoad : a.loadAction),
                         @"level": @(a.level), @"slice": @(a.slice), @"plane": @(a.depthPlane),
                         @"clear": @[@(cc.red), @(cc.green), @(cc.blue), @(cc.alpha)]}];
    }
    if (!rts.count) { NSLog(@"NVMTLDriver: 3D draw without a color target"); return; }
    NVMTLTexture *t0 = rts[0][@"tex"];
    const NSUInteger l0 = [rts[0][@"level"] unsignedIntegerValue];
    const NSUInteger w0 = MAX(t0.w >> l0, 1u), h0 = MAX(t0.h >> l0, 1u);
    MTLViewport vp = _hasVp ? _vp : (MTLViewport){0, 0, (double)w0, (double)h0, 0, 1};
    MTLScissorRect sc = _hasSc ? _sc : (MTLScissorRect){0, 0, w0, h0};
    _drew = YES;
    NSMutableDictionary *op = [@{@"op": @"draw3d", @"ps": clearOnly ? (id)[NSNull null] : _ps, @"rts": rts,
        @"vp": @[@(vp.originX), @(vp.originY), @(vp.width), @(vp.height), @(vp.znear), @(vp.zfar)],
        @"sc": @[@(sc.x), @(sc.y), @(sc.width), @(sc.height)],
        @"prim": @(t), @"start": @(s), @"count": @(n), @"inst": @(ni), @"binst": @(bi), @"bv": @(bv),
        @"cull": @(_cull), @"winding": @(_winding),
        @"vbufs": [_vbufs copy], @"vtexs": [_vtexs copy], @"vsamps": [_vsamps copy],
        @"fbufs": [_fbufs copy], @"ftexs": [_ftexs copy], @"fsamps": [_fsamps copy]} mutableCopy];
    if (ib) { op[@"ib"] = ib; op[@"io"] = @(io); op[@"it"] = @(it); }
    if (_pendInd && !clearOnly) { op[@"ind"] = _pendInd; op[@"indOff"] = @(_pendIndOff); _pendInd = nil; }
    if (_pendPatch && !clearOnly) { op[@"patch"] = _pendPatch; _pendPatch = nil; }
    MTLRenderPassDepthAttachmentDescriptor *da = _desc.depthAttachment;
    MTLRenderPassStencilAttachmentDescriptor *sa = _desc.stencilAttachment;
    NVMTLTexture *zt = da.texture ? da.texture : sa.texture;
    if ([zt isKindOfClass:objc_getClass("NVMTLTexture")] && nvZetaFormat(zt.fmt, NULL, NULL, NULL)) {
        bool hasZ = false, hasS = false;
        nvZetaFormat(zt.fmt, NULL, &hasZ, &hasS);
        op[@"ztex"] = zt;
        op[@"zclear"] = @(hasZ && da.texture && !_zdrew && da.loadAction == MTLLoadActionClear);
        op[@"zclearv"] = @(da.clearDepth);
        op[@"zmode"] = @(hasZ && _ds ? _ds.mode : 0u);
        if (hasS) {
            op[@"sclear"] = @(sa.texture && !_zdrew && sa.loadAction == MTLLoadActionClear);
            op[@"sclearv"] = @(sa.clearStencil);
            if (_ds.stencil) { op[@"sten"] = _ds.sten; op[@"sref"] = @[@(_sref[0]), @(_sref[1])]; }
        }
        _zdrew = YES;
    }
    [_nvCb nvAddDraw2:op];
}
- (void)setFragmentSamplerState:(id)smp atIndex:(NSUInteger)idx {
    if (idx < 16) _fsamps[idx] = smp ? smp : (id)[NSNull null];
}
- (void)setFragmentSamplerStates:(const id __unsafe_unretained *)smps withRange:(NSRange)r {
    for (NSUInteger i = 0; i < r.length; i++) [self setFragmentSamplerState:smps[i] atIndex:r.location + i];
}
// queue a raster-v2 draw (vCount 0 = only the load action).
- (BOOL)nvDraw2Start:(NSUInteger)s count:(NSUInteger)n idx:(NSData *)idx {
    return [self nvDraw2Start:s count:n idx:idx instances:1];
}
- (BOOL)nvDraw2Start:(NSUInteger)s count:(NSUInteger)n idx:(NSData *)idx instances:(NSUInteger)ni {
    id att = _desc.colorAttachments[0];
    NVMTLTexture *tex = [att texture];
    if (![tex isKindOfClass:objc_getClass("NVMTLTexture")]) { NSLog(@"NVMTLDriver: draw to non-NV texture"); return NO; }
    MTLViewport vp = _hasVp ? _vp : (MTLViewport){0, 0, (double)tex.w, (double)tex.h, 0, 1};
    MTLScissorRect sc = _hasSc ? _sc : (MTLScissorRect){0, 0, tex.w, tex.h};
    const MTLLoadAction load = _drew ? MTLLoadActionLoad : [att loadAction];
    const MTLClearColor cc = [att clearColor];
    _drew = YES;
    NSMutableDictionary *op = [@{@"op": @"draw2", @"tex": tex, @"load": @(load),
        @"clear": @[@(cc.red), @(cc.green), @(cc.blue), @(cc.alpha)],
        @"vp": @[@(vp.originX), @(vp.originY), @(vp.width), @(vp.height)],
        @"sc": @[@(sc.x), @(sc.y), @(sc.width), @(sc.height)],
        @"vStart": @(s), @"vCount": @(n)} mutableCopy];
    if (_ps) op[@"ps"] = _ps;
    MTLRenderPassDepthAttachmentDescriptor *da = _desc.depthAttachment;
    NVMTLTexture *zt = da.texture;
    if ([zt isKindOfClass:objc_getClass("NVMTLTexture")]) {
        op[@"ztex"] = zt;
        op[@"zload"] = @(load == MTLLoadActionLoad ? MTLLoadActionLoad : da.loadAction);
        op[@"zclear"] = @(da.clearDepth);
        op[@"zmode"] = @(_ds ? _ds.mode : 0u);
    }
    op[@"vbufs"] = [_vbufs copy]; op[@"fbufs"] = [_fbufs copy];
    op[@"ftexs"] = [_ftexs copy]; op[@"fsamps"] = [_fsamps copy];
    if (idx) op[@"idx"] = idx;
    op[@"inst"] = @(ni);
    if (_pendInd) { op[@"ind"] = _pendInd; op[@"indOff"] = @(_pendIndOff); _pendInd = nil; }
    [_nvCb nvAddDraw2:op];
    return YES;
}
// Synchronous execution: prior work is always complete, so fence
// wait/update are correct no-ops (stages ignored for now).
- (void)waitForFence:(id)f beforeStages:(MTLRenderStages)s { (void)f; (void)s; }
- (void)updateFence:(id)f afterStages:(MTLRenderStages)s { (void)f; (void)s; }
- (void)setViewport:(MTLViewport)vp { _vp = vp; _hasVp = YES; }
- (void)setScissorRect:(MTLScissorRect)sc { _sc = sc; _hasSc = YES; }
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n
            instanceCount:(NSUInteger)ni baseInstance:(NSUInteger)bi {
    if (_ps.hwVS) { [self nvDraw3D:t start:s count:n instances:ni baseInstance:bi indexBuffer:nil
                           indexOffset:0 indexType:0 baseVertex:0]; return; }
    if (!bi) [self drawPrimitives:t vertexStart:s vertexCount:n instanceCount:ni];
    else NSLog(@"NVMTLDriver: base instance needs a 3D-engine pipeline");
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)n indexType:(MTLIndexType)it
                  indexBuffer:(id)buf indexBufferOffset:(NSUInteger)off instanceCount:(NSUInteger)ni
                   baseVertex:(NSInteger)bv baseInstance:(NSUInteger)bi {
    if (_ps.hwVS) { [self nvDraw3D:t start:0 count:n instances:ni baseInstance:bi indexBuffer:buf
                           indexOffset:off indexType:it baseVertex:bv]; return; }
    if (!bv && !bi) [self drawIndexedPrimitives:t indexCount:n indexType:it indexBuffer:buf indexBufferOffset:off
                                  instanceCount:ni];
    else NSLog(@"NVMTLDriver: base vertex/instance needs a 3D-engine pipeline");
}
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n {
    if (_ps.hwVS) { [self nvDraw3D:t start:s count:n instances:1 baseInstance:0 indexBuffer:nil
                           indexOffset:0 indexType:0 baseVertex:0]; return; }
    if (t != MTLPrimitiveTypeTriangle) { NSLog(@"NVMTLDriver: only triangles in v1"); return; }
    if (!_ps || !_desc) { NSLog(@"NVMTLDriver: draw without pipeline/desc"); return; }
    if (_ps.fs.raster && _ps.vs.voutStride) { [self nvDraw2Start:s count:n idx:nil]; return; }
    id att = _desc.colorAttachments[0];
    NVMTLTexture *tex = [att texture];
    if (![tex isKindOfClass:objc_getClass("NVMTLTexture")]) { NSLog(@"NVMTLDriver: draw to non-NV texture"); return; }
    MTLViewport vp = _hasVp ? _vp : (MTLViewport){0, 0, (double)tex.w, (double)tex.h, 0, 1};
    MTLScissorRect sc = _hasSc ? _sc : (MTLScissorRect){0, 0, tex.w, tex.h};
    _drew = YES;
    [_nvCb nvAddDraw:_ps vbufs:_vbufs vp:vp sc:sc tex:tex
                load:[att loadAction] clear:[att clearColor] vStart:s vCount:n idx:nil];
}
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n
            instanceCount:(NSUInteger)ni {
    if (_ps.hwVS) { [self nvDraw3D:t start:s count:n instances:ni baseInstance:0 indexBuffer:nil
                           indexOffset:0 indexType:0 baseVertex:0]; return; }
    if (t != MTLPrimitiveTypeTriangle || !_ps || !_desc || !(_ps.fs.raster && _ps.vs.voutStride)) {
        if (ni == 1) [self drawPrimitives:t vertexStart:s vertexCount:n];
        else NSLog(@"NVMTLDriver: instanced draw needs a raster-v2 triangle pipeline");
        return;
    }
    if (ni) [self nvDraw2Start:s count:n idx:nil instances:ni];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexType:(MTLIndexType)it indexBuffer:(id)buf
             indexBufferOffset:(NSUInteger)off indirectBuffer:(id)ib indirectBufferOffset:(NSUInteger)io {
    if (!_ps.hwVS) { NSLog(@"NVMTLDriver: indexed indirect draw needs a 3D-engine pipeline"); return; }
    _pendInd = ib; _pendIndOff = io;
    [self nvDraw3D:t start:0 count:1 instances:1 baseInstance:0 indexBuffer:buf indexOffset:off indexType:it baseVertex:0];
}
- (void)drawPrimitives:(MTLPrimitiveType)t indirectBuffer:(id)ib indirectBufferOffset:(NSUInteger)io {
    if (_ps.hwVS) {   // counts read from the buffer when the draw executes
        _pendInd = ib; _pendIndOff = io;
        [self nvDraw3D:t start:0 count:1 instances:1 baseInstance:0 indexBuffer:nil indexOffset:0 indexType:0 baseVertex:0];
        return;
    }
    if (t != MTLPrimitiveTypeTriangle || !_ps || !_desc || !(_ps.fs.raster && _ps.vs.voutStride)) {
        NSLog(@"NVMTLDriver: indirect draw needs a raster-v2 triangle pipeline"); return;
    }
    _pendInd = ib; _pendIndOff = io;
    [self nvDraw2Start:0 count:1 idx:nil];   // real counts come from the buffer
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)n indexType:(MTLIndexType)it
                  indexBuffer:(id)buf indexBufferOffset:(NSUInteger)off instanceCount:(NSUInteger)ni {
    if (_ps.hwVS) { [self nvDraw3D:t start:0 count:n instances:ni baseInstance:0 indexBuffer:buf
                           indexOffset:off indexType:it baseVertex:0]; return; }
    if (!ni) return;
    if (ni > 1 && !(_ps.fs.raster && _ps.vs.voutStride)) {
        NSLog(@"NVMTLDriver: instanced draw needs a raster-v2 triangle pipeline"); return;
    }
    _pendInst = ni;
    [self drawIndexedPrimitives:t indexCount:n indexType:it indexBuffer:buf indexBufferOffset:off];
    _pendInst = 0;   // also when that draw bailed out early
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)n indexType:(MTLIndexType)it
                  indexBuffer:(id)buf indexBufferOffset:(NSUInteger)off {
    if (_ps.hwVS) { [self nvDraw3D:t start:0 count:n instances:1 baseInstance:0 indexBuffer:buf
                           indexOffset:off indexType:it baseVertex:0]; return; }
    if (t != MTLPrimitiveTypeTriangle) { NSLog(@"NVMTLDriver: only triangles in v1"); return; }
    if (!_ps || !_desc) { NSLog(@"NVMTLDriver: draw without pipeline/desc"); return; }
    if (!n || n % 3) { NSLog(@"NVMTLDriver: bad indexCount %lu", (unsigned long)n); return; }
    if (it != MTLIndexTypeUInt16 && it != MTLIndexTypeUInt32) { NSLog(@"NVMTLDriver: bad indexType"); return; }
    uint8_t *c = [buf contents];
    const NSUInteger esz = it == MTLIndexTypeUInt16 ? 2 : 4;
    if (!c || off + n * esz > [buf length]) { NSLog(@"NVMTLDriver: index buffer out of range"); return; }
    NSMutableData *idx = [NSMutableData dataWithLength:n * 4];
    uint32_t *dst = idx.mutableBytes, mx = 0;
    for (NSUInteger i = 0; i < n; i++) {
        const uint32_t v = it == MTLIndexTypeUInt16 ? ((uint16_t *)(c + off))[i] : ((uint32_t *)(c + off))[i];
        dst[i] = v; if (v > mx) mx = v;
    }
    if (_ps.fs.raster && _ps.vs.voutStride) {
        const NSUInteger ni = _pendInst ? _pendInst : 1;
        _pendInst = 0;
        [self nvDraw2Start:0 count:mx + 1 idx:idx instances:ni];
        return;
    }
    _pendInst = 0;
    if (mx >= 4093) { NSLog(@"NVMTLDriver: index %u out of v1 range", mx); return; }
    id att = _desc.colorAttachments[0];
    NVMTLTexture *tex = [att texture];
    if (![tex isKindOfClass:objc_getClass("NVMTLTexture")]) { NSLog(@"NVMTLDriver: draw to non-NV texture"); return; }
    MTLViewport vp = _hasVp ? _vp : (MTLViewport){0, 0, (double)tex.w, (double)tex.h, 0, 1};
    MTLScissorRect sc = _hasSc ? _sc : (MTLScissorRect){0, 0, tex.w, tex.h};
    _drew = YES;
    [_nvCb nvAddDraw:_ps vbufs:_vbufs vp:vp sc:sc tex:tex
                load:[att loadAction] clear:[att clearColor] vStart:0 vCount:mx + 1 idx:idx];
}
- (void)endEncoding {
    // a pass with no draws still runs its clears; block-linear / multisampled
    // targets and depth/stencil clears go through the 3D engine
    if (!_drew && _desc) {
        NVMTLTexture *c0 = _desc.colorAttachments[0].texture;
        NVMTLTexture *zs = _desc.depthAttachment.texture ? _desc.depthAttachment.texture : _desc.stencilAttachment.texture;
        const bool zsClear = zs && (_desc.depthAttachment.loadAction == MTLLoadActionClear ||
                                    _desc.stencilAttachment.loadAction == MTLLoadActionClear);
        if ([c0 isKindOfClass:objc_getClass("NVMTLTexture")] && (c0.lay || c0.samples > 1 || zsClear)) {
            if (zsClear || _desc.colorAttachments[0].loadAction == MTLLoadActionClear)
                [self nvDraw3D:MTLPrimitiveTypeTriangle start:0 count:0 instances:0 baseInstance:0 indexBuffer:nil
                   indexOffset:0 indexType:MTLIndexTypeUInt16 baseVertex:0];
        } else if ([_desc.colorAttachments[0] loadAction] == MTLLoadActionClear)
            [self nvDraw2Start:0 count:0 idx:nil];
    }
    // multisample resolves
    for (NSUInteger i = 0; _desc && i < 8; i++) {
        MTLRenderPassColorAttachmentDescriptor *a = _desc.colorAttachments[i];
        NVMTLTexture *ms = a.texture, *rs = a.resolveTexture;
        if (![ms isKindOfClass:objc_getClass("NVMTLTexture")] || ![rs isKindOfClass:objc_getClass("NVMTLTexture")] ||
            ms.samples < 2 || (a.storeAction != MTLStoreActionMultisampleResolve &&
                               a.storeAction != MTLStoreActionStoreAndMultisampleResolve))
            continue;
        const NSUInteger rl = a.resolveLevel, rsl = a.resolveSlice;
        if (nvGpuResolve((NVMTLCommandBuffer *)_nvCb, ms, rs, rl, rsl)) continue;   // on the GPU
        [(NVMTLCommandBuffer *)_nvCb nvAddOp:@{@"op": @"block", @"fn": [^{ nvResolve(ms, rs, rl, rsl); } copy]}];
    }
    [super endEncoding];
    _nvCb = nil;
}
@end

@interface MTLIOAccelComputeCommandEncoder : NSObject
- (instancetype)initWithCommandBuffer:(id)cb;
- (void)endEncoding;
@end

@interface NVMTLComputeEncoder : MTLIOAccelComputeCommandEncoder
- (void)nvSetCB:(id)cb;
@end
@implementation NVMTLComputeEncoder {
    NVMTLCommandBuffer *_nvCb;
    NVMTLComputePipelineState *_ps;
    NSMutableArray *_bufs; // per index: {buf, off} or NSNull
    NSMutableArray *_texs, *_samps;   // per index: object or NSNull
    NSMutableDictionary<NSNumber *, NSNumber *> *_tg;   // dynamic threadgroup memory per index
}
+ (void)load { class_addProtocol(self, @protocol(MTLComputeCommandEncoder)); }
- (instancetype)initWithCommandBuffer:(id)cb {
    self = [super initWithCommandBuffer:cb];
    if (self) {
        _bufs = [NSMutableArray new];
        _texs = [NSMutableArray new];
        _samps = [NSMutableArray new];
        for (int i = 0; i < 128; i++) {   // Metal limits (31 buffers, 128 textures, 16 samplers)
            [_bufs addObject:[NSNull null]];
            [_texs addObject:[NSNull null]];
            [_samps addObject:[NSNull null]];
        }
        // was 16 of each. MPS binds kernel params at buffer 29/30
        // (cnnNeuron, cnnConv): they were dropped, the kernel read VA 0 and
        // GR hung on the MMU fault (mediaanalysisd right after login).
        [_bufs removeObjectsInRange:NSMakeRange(31, 128 - 31)];
        [_samps removeObjectsInRange:NSMakeRange(16, 128 - 16)];
    }
    return self;
}
- (void)nvSetCB:(id)cb { _nvCb = cb; }
- (void)setComputePipelineState:(id)ps { _ps = ps; }
- (void)setBuffer:(id)buf offset:(NSUInteger)off atIndex:(NSUInteger)idx {
    if (idx < 31) _bufs[idx] = buf ? @{@"buf": buf, @"off": @(off)} : (id)[NSNull null];
}
- (void)setBytes:(const void *)bytes length:(NSUInteger)len atIndex:(NSUInteger)idx {
    // Small constants: a fresh shared buffer per call (Metal copies at encode time too).
    uint32_t zero = 0;
    id b = [(id<MTLDevice>)[(id<MTLCommandBuffer>)_nvCb device] newBufferWithBytes:(len ? bytes : &zero)
                                                                           length:(len ? len : 4)
                                                                          options:MTLResourceStorageModeShared];
    [self setBuffer:b offset:0 atIndex:idx];
}
- (void)setTexture:(id)tex atIndex:(NSUInteger)idx {
    if (idx < 128) _texs[idx] = tex ? tex : (id)[NSNull null];
}
- (void)setTextures:(const id __unsafe_unretained *)texs withRange:(NSRange)r {
    for (NSUInteger i = 0; i < r.length; i++) [self setTexture:texs[i] atIndex:r.location + i];
}
- (void)setSamplerState:(id)smp atIndex:(NSUInteger)idx {
    if (idx < 16) _samps[idx] = smp ? smp : (id)[NSNull null];
}
- (void)setSamplerStates:(const id __unsafe_unretained *)smps withRange:(NSRange)r {
    for (NSUInteger i = 0; i < r.length; i++) [self setSamplerState:smps[i] atIndex:r.location + i];
}
static const char *const kNvComputeStubs[] = {
    "executeCommandsInBuffer:indirectBuffer:indirectBufferOffset:",
    "executeCommandsInBuffer:withRange:",
    "sampleCountersInBuffer:atSampleIndex:withBarrier:",
    "setVisibleFunctionTable:atBufferIndex:",
    "setVisibleFunctionTables:withBufferRange:",
    "setIntersectionFunctionTable:atBufferIndex:",
    "setIntersectionFunctionTables:withBufferRange:",
    "setAccelerationStructure:atBufferIndex:",
    NULL};
+ (void)initialize {
    if (self != [NVMTLComputeEncoder class]) return;
    nvInstallFallback(self);
    nvForwardStubs(self, @protocol(MTLComputeCommandEncoder), kNvComputeStubs);
}
- (void)memoryBarrierWithScope:(MTLBarrierScope)sc { (void)sc; }
- (void)memoryBarrierWithResources:(const id __unsafe_unretained *)r count:(NSUInteger)n { (void)r; (void)n; }
- (void)useResource:(id)r usage:(MTLResourceUsage)u { (void)r; (void)u; }
- (void)useResources:(const id __unsafe_unretained *)r count:(NSUInteger)n usage:(MTLResourceUsage)u { (void)r; (void)n; (void)u; }
- (void)useHeap:(id)h { (void)h; }
- (void)useHeaps:(const id __unsafe_unretained *)h count:(NSUInteger)n { (void)h; (void)n; }
- (void)setBuffer:(id)buf offset:(NSUInteger)off attributeStride:(NSUInteger)st atIndex:(NSUInteger)idx {
    (void)st; [self setBuffer:buf offset:off atIndex:idx];
}
- (void)setBufferOffset:(NSUInteger)off attributeStride:(NSUInteger)st atIndex:(NSUInteger)idx {
    (void)st; [self setBufferOffset:off atIndex:idx];
}
- (void)setBuffers:(const id __unsafe_unretained *)bufs offsets:(const NSUInteger *)offs
  attributeStrides:(const NSUInteger *)st withRange:(NSRange)r {
    (void)st; [self setBuffers:bufs offsets:offs withRange:r];
}
- (void)setBytes:(const void *)bytes length:(NSUInteger)len attributeStride:(NSUInteger)st atIndex:(NSUInteger)idx {
    (void)st; [self setBytes:bytes length:len atIndex:idx];
}
- (void)setBufferOffset:(NSUInteger)off atIndex:(NSUInteger)idx {
    id cur = idx < 31 ? _bufs[idx] : nil;
    if ([cur isKindOfClass:[NSDictionary class]]) [self setBuffer:cur[@"buf"] offset:off atIndex:idx];
}
- (void)setBuffers:(const id __unsafe_unretained *)bufs offsets:(const NSUInteger *)offs withRange:(NSRange)r {
    for (NSUInteger i = 0; i < r.length; i++) [self setBuffer:bufs[i] offset:offs ? offs[i] : 0 atIndex:r.location + i];
}
- (void)setSamplerState:(id)smp lodMinClamp:(float)a lodMaxClamp:(float)b atIndex:(NSUInteger)idx {
    (void)a; (void)b; [self setSamplerState:smp atIndex:idx];
}
- (void)setSamplerStates:(const id __unsafe_unretained *)smps lodMinClamps:(const float *)a
            lodMaxClamps:(const float *)b withRange:(NSRange)r {
    (void)a; (void)b; [self setSamplerStates:smps withRange:r];
}
- (void)setThreadgroupMemoryLength:(NSUInteger)len atIndex:(NSUInteger)idx {
    if (!_tg) _tg = [NSMutableDictionary new];
    _tg[@(idx)] = @((len + 15) & ~(NSUInteger)15);
}
// the kernel rebuilt with the threadgroup sizes set on this encoder (nakc tgI=BYTES)
- (id)nvPsForDispatch {
    NVMTLKernel *k = _tg.count ? [(id)_ps valueForKey:@"kernel"] : nil;
    if (!k.airLib) return _ps;
    NSMutableArray *tga = [NSMutableArray new];
    for (NSNumber *i in [_tg.allKeys sortedArrayUsingSelector:@selector(compare:)])
        [tga addObject:[NSString stringWithFormat:@"tg%@=%@", i, _tg[i]]];
    NSString *key = [@"tg|" stringByAppendingString:[tga componentsJoinedByString:@","]];
    NVMTLKernel *v;
    @synchronized (k.variants) {
        v = k.variants[key];
        if (!v) {
            v = nvCompileAir(k.airLib, k.name, 256, 1, 1, [(k.airArgs ? k.airArgs : @[]) arrayByAddingObjectsFromArray:tga]);
            if (v) k.variants[key] = v;
        }
    }
    if (!v) return _ps;
    id ps = [[(id)_ps class] new];
    [ps setValue:v forKey:@"kernel"];
    [ps setValue:[(id)_ps valueForKey:@"device"] forKey:@"device"];
    return ps;
}
- (void)dispatchThreads:(MTLSize)grid threadsPerThreadgroup:(MTLSize)block {
    [_nvCb nvAddDispatch:[self nvPsForDispatch] bufs:_bufs texs:_texs samps:_samps grid:grid block:block];
}
- (void)dispatchThreadgroupsWithIndirectBuffer:(id)ib indirectBufferOffset:(NSUInteger)io
                          threadsPerThreadgroup:(MTLSize)block {
    [_nvCb nvAddDispatch:[self nvPsForDispatch] bufs:_bufs texs:_texs samps:_samps indirect:ib offset:io block:block];
}
- (void)dispatchThreadgroups:(MTLSize)groups threadsPerThreadgroup:(MTLSize)block {
    const MTLSize grid = {groups.width * block.width, groups.height * block.height, groups.depth * block.depth};
    [self dispatchThreads:grid threadsPerThreadgroup:block];
}
- (void)waitForFence:(id)f { (void)f; } // synchronous: already complete
- (void)updateFence:(id)f { (void)f; }
- (void)endEncoding { [super endEncoding]; _nvCb = nil; }
@end

// No MTLIOAccelComputePipelineState exists in Metal: plain object.
@interface NVMTLComputePipelineState : NVMTLObject
@property (nonatomic) NVMTLKernel *kernel;
@property (nonatomic, weak) id device;
@end
@implementation NVMTLComputePipelineState
+ (void)load { class_addProtocol(self, @protocol(MTLComputePipelineState)); }
- (NSUInteger)maxTotalThreadsPerThreadgroup { return 1024; }
- (NSUInteger)threadExecutionWidth { return 32; }
- (NSUInteger)staticThreadgroupMemoryLength { return 0; }
@end

// ---------------------------------------------------------------- 3D
static uint32_t nvColorTargetFormat(MTLPixelFormat f) {
    switch (f) {
    case MTLPixelFormatBGRA8Unorm: return 0xCF;         case MTLPixelFormatBGRA8Unorm_sRGB: return 0xD0;
    case MTLPixelFormatRGBA8Unorm: return 0xD5;         case MTLPixelFormatRGBA8Unorm_sRGB: return 0xD6;
    case MTLPixelFormatRGBA8Uint: return 0xD9;          case MTLPixelFormatRGB10A2Unorm: return 0xD1;
    case MTLPixelFormatRGBA16Float: return 0xCA;        case MTLPixelFormatRGBA32Float: return 0xC0;
    case MTLPixelFormatRG32Float: return 0xCB;          case MTLPixelFormatRG16Float: return 0xDE;
    case MTLPixelFormatRG16Unorm: return 0xDA;          case MTLPixelFormatRG11B10Float: return 0xE0;
    case MTLPixelFormatR32Uint: return 0xE4;            case MTLPixelFormatR32Float: return 0xE5;
    case MTLPixelFormatRG8Unorm: return 0xEA;           case MTLPixelFormatR16Unorm: return 0xEE;
    case MTLPixelFormatR16Uint: return 0xF1;            case MTLPixelFormatR16Float: return 0xF2;
    case MTLPixelFormatR8Unorm: return 0xF3;            case MTLPixelFormatR8Uint: return 0xF6;
    default: return 0;
    }
}

// MTLVertexFormat -> SET_VERTEX_ATTRIBUTE_A COMPONENT_BIT_WIDTHS / NUMERICAL_TYPE / SWAP_R_AND_B
static bool nvVertexFormat(MTLVertexFormat f, uint32_t *widths, uint32_t *type, bool *swap) {
    enum { SN = 1, UN = 2, SI = 3, UI = 4, FL = 7 };
    *swap = false;
    switch (f) {
    case MTLVertexFormatFloat: *widths = 0x12; *type = FL; return true;
    case MTLVertexFormatFloat2: *widths = 0x04; *type = FL; return true;
    case MTLVertexFormatFloat3: *widths = 0x02; *type = FL; return true;
    case MTLVertexFormatFloat4: *widths = 0x01; *type = FL; return true;
    case MTLVertexFormatHalf: *widths = 0x1b; *type = FL; return true;
    case MTLVertexFormatHalf2: *widths = 0x0f; *type = FL; return true;
    case MTLVertexFormatHalf3: *widths = 0x05; *type = FL; return true;
    case MTLVertexFormatHalf4: *widths = 0x03; *type = FL; return true;
    case MTLVertexFormatInt: *widths = 0x12; *type = SI; return true;
    case MTLVertexFormatInt2: *widths = 0x04; *type = SI; return true;
    case MTLVertexFormatInt3: *widths = 0x02; *type = SI; return true;
    case MTLVertexFormatInt4: *widths = 0x01; *type = SI; return true;
    case MTLVertexFormatUInt: *widths = 0x12; *type = UI; return true;
    case MTLVertexFormatUInt2: *widths = 0x04; *type = UI; return true;
    case MTLVertexFormatUInt3: *widths = 0x02; *type = UI; return true;
    case MTLVertexFormatUInt4: *widths = 0x01; *type = UI; return true;
    case MTLVertexFormatUChar4Normalized: *widths = 0x0a; *type = UN; return true;
    case MTLVertexFormatUChar4Normalized_BGRA: *widths = 0x0a; *type = UN; *swap = true; return true;
    case MTLVertexFormatChar4Normalized: *widths = 0x0a; *type = SN; return true;
    case MTLVertexFormatUChar4: *widths = 0x0a; *type = UI; return true;
    case MTLVertexFormatUChar2Normalized: *widths = 0x18; *type = UN; return true;
    case MTLVertexFormatUShort2Normalized: *widths = 0x0f; *type = UN; return true;
    case MTLVertexFormatShort2Normalized: *widths = 0x0f; *type = SN; return true;
    case MTLVertexFormatUShort4Normalized: *widths = 0x03; *type = UN; return true;
    case MTLVertexFormatShort4Normalized: *widths = 0x03; *type = SN; return true;
    case MTLVertexFormatUShort2: *widths = 0x0f; *type = UI; return true;
    case MTLVertexFormatShort2: *widths = 0x0f; *type = SI; return true;
    case MTLVertexFormatUInt1010102Normalized: *widths = 0x30; *type = UN; return true;
    default: return false;
    }
}

static uint32_t nvBlendFactor(MTLBlendFactor f) {
    switch (f) {
    case MTLBlendFactorZero: return 0x4000;                    case MTLBlendFactorOne: return 0x4001;
    case MTLBlendFactorSourceColor: return 0x4300;             case MTLBlendFactorOneMinusSourceColor: return 0x4301;
    case MTLBlendFactorSourceAlpha: return 0x4302;             case MTLBlendFactorOneMinusSourceAlpha: return 0x4303;
    case MTLBlendFactorDestinationAlpha: return 0x4304;        case MTLBlendFactorOneMinusDestinationAlpha: return 0x4305;
    case MTLBlendFactorDestinationColor: return 0x4306;        case MTLBlendFactorOneMinusDestinationColor: return 0x4307;
    case MTLBlendFactorSourceAlphaSaturated: return 0x4308;
    case MTLBlendFactorBlendColor: return 0xc001;              case MTLBlendFactorOneMinusBlendColor: return 0xc002;
    case MTLBlendFactorBlendAlpha: return 0xc003;              case MTLBlendFactorOneMinusBlendAlpha: return 0xc004;
    default: return 0x4001;
    }
}

static uint32_t nvBlendOp(MTLBlendOperation o) {
    switch (o) {
    case MTLBlendOperationSubtract: return 0x800a;   case MTLBlendOperationReverseSubtract: return 0x800b;
    case MTLBlendOperationMin: return 0x8007;        case MTLBlendOperationMax: return 0x8008;
    default: return 0x8006;
    }
}

// Upload a vertex/fragment binary (SPH + code) once; the GPU reads it from our heap.
static uint64_t nvShaderVa(NVMTLKernel *k) {
    @synchronized (k) {
        if (k.codeVa) return k.codeVa;
        void *cpu = NULL;
        uint64_t va = 0;
        if (!nvHeapAlloc(k.code.length + 1024, &cpu, &va)) return 0;
        memset(cpu, 0, k.code.length + 1024);
        memcpy(cpu, k.code.bytes, k.code.length);
        k.codeVa = va;
        // matches the PC in a GR exception (SM warp ESR) to its shader
        NSLog(@"NVMTLDriver: shader %@ stage %u at 0x%llx..0x%llx slm %u", k.name, k.stage,
              (unsigned long long)va, (unsigned long long)(va + k.code.length), k.slm);
        return va;
    }
}

// One stage's cbuf 0, in the kernels' layout: u64 buffers, u64 textures
// (tic | flags << 32), u32 samplers + constexpr samplers, u32 base vertex,
// base instance, then the constant-data pointer.
// a buffer slot with no GPU address must never reach a shader as 0.
// On the 3D channel a generic load near 0 lands in the shared/local window:
// SM warp error 0x10 (INVALID_ADDR_SPACE), a GR exception and a GPU reset,
// which is the black flash after login. Point such slots at 1 MiB of zeros.
static uint64_t nvNullVa(void) {
    static uint64_t va;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        void *cpu = NULL;
        if (nvHeapAlloc(1 << 20, &cpu, &va)) memset(cpu, 0, 1 << 20);
        else va = 0;
    });
    return va;
}
static void nvNoteNoVa(id<MTLBuffer> b, uint32_t slot, NVMTLKernel *k) {
    static uint32_t n;
    if (n++ < 32)
        NSLog(@"NVMTLDriver: %@ slot %u: buffer %@ (%s, %lu bytes, mode %lu, contents %p) has no GPU address",
              k.name, slot, b ? b.label : @"(unbound)", b ? object_getClassName(b) : "-",
              (unsigned long)b.length, (unsigned long)b.storageMode, b.contents);
}

static uint32_t nvStagePush(NVMTLKernel *k, id dev, NSArray *bufs, NSArray *texs, NSArray *samps,
                            int32_t baseVertex, uint32_t baseInstance, uint32_t *push) {
    if (k.ntex) {   // what this stage samples, for a faulting draw
        char note[480]; int nn = 0; note[0] = 0;
        for (uint32_t i = 0; i < k.ntex && nn < (int)sizeof note - 120; i++) {
            NVMTLTexture *t = i < texs.count && texs[i] != (id)[NSNull null] ? texs[i] : nil;
            if (!t) { nn += snprintf(note + nn, sizeof note - nn, " t%u unbound", i); continue; }
            const uint32_t idx = (uint32_t)[t gpuResourceID]._impl;
            uint32_t td[8] = {0};
            uint64_t va = 0;
            if (idx && nvTicRead(idx, td)) va = ((uint64_t)(td[1] >> 5) << 5) | ((uint64_t)(td[2] & 0xffff) << 32);
            nn += snprintf(note + nn, sizeof note - nn, " t%u %lux%lu fmt %lu tic %u va %llx (%s)%s", i,
                           (unsigned long)t.w, (unsigned long)t.h, (unsigned long)t.fmt, idx, va,
                           va ? nvVaState(va) : "-", t.ios ? " iosurface" : "");
        }
        nvGrNoteTextures(note);
    }
    const uint32_t texBase = k.nbuf * 2, sampBase = texBase + k.ntex * 2;
    const uint32_t baseW = sampBase + k.nsamp + k.nconstSamp;
    uint32_t n = baseW + 2;
    if (k.cdataDword + 2 > n) n = k.cdataDword + 2;
    if (n > 1024) return 0;
    memset(push, 0, n * 4);
    for (uint32_t i = 0; i < k.nbuf; i++) {
        NSDictionary *be = i < bufs.count ? bufs[i] : nil;
        if ((id)be == [NSNull null]) be = nil;
        id<MTLBuffer> b = be[@"buf"];
        const NSUInteger off = [be[@"off"] unsignedIntegerValue];
        uint64_t va = b && off < b.length ? nvBufVa(b, off, b.length - off) : 0;
        if (!va) { if (b) nvNoteNoVa(b, i, k); va = nvNullVa(); }
        push[2 * i] = (uint32_t)va; push[2 * i + 1] = (uint32_t)(va >> 32);
    }
    for (uint32_t i = 0; i < k.ntex && i < texs.count; i++) {
        NVMTLTexture *t = texs[i] != (id)[NSNull null] ? texs[i] : nil;
        if (!t) continue;
        const MTLResourceID r = [t gpuResourceID];
        push[texBase + 2 * i] = (uint32_t)r._impl; push[texBase + 2 * i + 1] = (uint32_t)(r._impl >> 32);
    }
    for (uint32_t i = 0; i < k.nsamp && i < samps.count; i++) {
        NVMTLSamplerState *sm = samps[i] != (id)[NSNull null] ? samps[i] : nil;
        push[sampBase + i] = sm ? sm.tsc : 0;
    }
    for (uint32_t i = 0; i < k.nconstSamp; i++) push[sampBase + k.nsamp + i] = ((const uint32_t *)k.constTsc.bytes)[i];
    push[baseW] = (uint32_t)baseVertex;
    push[baseW + 1] = baseInstance;
    if (k.cdataDword) {
        @synchronized (k) {
            if (!k.cdataBuf) k.cdataBuf = [dev newBufferWithBytes:k.cdata.bytes length:k.cdata.length
                                                          options:MTLResourceStorageModeShared];
        }
        const uint64_t cva = nvBufVa(k.cdataBuf, 0, k.cdata.length);
        push[k.cdataDword] = (uint32_t)cva; push[k.cdataDword + 1] = (uint32_t)(cva >> 32);
    }
    return n;
}

static BOOL nvBufIO(id<MTLBuffer> b, BOOL write, NSUInteger off, NSUInteger len, void *cpu);
static void nvExecuteDraw3D(NSDictionary *op) {
    if (op[@"ind"]) {
        // indirect draw. Ops run in order and synchronously, so the
        // arguments written by earlier GPU work are in the buffer by now.
        // MTLDrawPrimitivesIndirectArguments {count, instances, start, baseInstance};
        // indexed: {count, instances, indexStart, baseVertex, baseInstance}
        uint32_t a[5] = {0};
        const bool indexed = op[@"ib"] != nil;
        if (!nvBufIO(op[@"ind"], NO, [op[@"indOff"] unsignedIntegerValue], indexed ? 20 : 16, a)) {
            NSLog(@"NVMTLDriver: indirect arguments unreadable"); return;
        }
        if (!a[0] || !a[1]) return;
        NSMutableDictionary *m = [op mutableCopy];
        m[@"count"] = @(a[0]); m[@"inst"] = @(a[1]);
        if (indexed) {
            const NSUInteger isz = [op[@"it"] unsignedIntegerValue] == MTLIndexTypeUInt16 ? 2 : 4;
            m[@"io"] = @([op[@"io"] unsignedIntegerValue] + a[2] * isz);
            m[@"bv"] = @((int32_t)a[3]); m[@"binst"] = @(a[4]);
        } else { m[@"start"] = @(a[2]); m[@"binst"] = @(a[3]); }
        op = m;
    }
    NVMTLRenderPipelineState *ps = op[@"ps"] == (id)[NSNull null] ? nil : op[@"ps"];
    if (op[@"patch"][@"pib"] && ps) {
        // patch index buffer. Patch i of the draw is P = list[start + i]:
        // its control points (P * cps + j, or through the control point index
        // buffer) become a 32-bit index list, its factors are gathered in
        // draw order, and the TES reads patch_id from the list.
        NSDictionary *pt = op[@"patch"];
        const NSUInteger cps = [pt[@"cps"] unsignedIntegerValue], pc = [pt[@"pc"] unsignedIntegerValue];
        const NSUInteger start = [pt[@"start"] unsignedIntegerValue];
        const NSUInteger listOff = [pt[@"pio"] unsignedIntegerValue] + start * 4;
        const uint32_t stride = ps.hwTES.tessDomain == 2 ? 12 : 8;
        id<MTLBuffer> list = pt[@"pib"], tf = pt[@"tf"];
        id dev = ps.device;
        NSMutableData *ids = [NSMutableData dataWithLength:pc * 4];
        if (!nvBufIO(list, NO, listOff, pc * 4, ids.mutableBytes)) { NSLog(@"NVMTLDriver: patch index buffer unreadable"); return; }
        const uint32_t *P = ids.bytes;
        uint32_t maxP = 0;
        for (NSUInteger i = 0; i < pc; i++) if (P[i] > maxP) maxP = P[i];
        NSMutableData *fall = [NSMutableData dataWithLength:(maxP + 1) * stride];
        if (!nvBufIO(tf, NO, [pt[@"tfOff"] unsignedIntegerValue], (maxP + 1) * stride, fall.mutableBytes)) {
            NSLog(@"NVMTLDriver: tessellation factors unreadable"); return;
        }
        NSMutableData *fg = [NSMutableData dataWithLength:pc * stride], *idx = [NSMutableData dataWithLength:pc * cps * 4];
        uint32_t *I = idx.mutableBytes;
        NSMutableData *cpi = nil;
        const bool u16 = ps.cpIndexType == MTLIndexTypeUInt16;
        if (pt[@"cib"]) {
            cpi = [NSMutableData dataWithLength:(maxP + 1) * cps * (u16 ? 2 : 4)];
            if (!nvBufIO(pt[@"cib"], NO, [pt[@"cio"] unsignedIntegerValue], cpi.length, cpi.mutableBytes)) {
                NSLog(@"NVMTLDriver: control point index buffer unreadable"); return;
            }
        }
        for (NSUInteger i = 0; i < pc; i++) {
            memcpy((uint8_t *)fg.mutableBytes + i * stride, (const uint8_t *)fall.bytes + P[i] * stride, stride);
            for (NSUInteger j = 0; j < cps; j++) {
                const NSUInteger k = P[i] * cps + j;
                I[i * cps + j] = !cpi ? (uint32_t)k : u16 ? ((const uint16_t *)cpi.bytes)[k] : ((const uint32_t *)cpi.bytes)[k];
            }
        }
        NSMutableDictionary *m = [op mutableCopy], *np = [pt mutableCopy];
        np[@"tf"] = [dev newBufferWithBytes:fg.bytes length:fg.length options:MTLResourceStorageModeShared];
        np[@"tfOff"] = @0; np[@"start"] = @0;
        np[@"pidList"] = [dev newBufferWithBytes:ids.bytes length:ids.length options:MTLResourceStorageModeShared];
        m[@"patch"] = np;
        m[@"ib"] = [dev newBufferWithBytes:idx.bytes length:idx.length options:MTLResourceStorageModeShared];
        m[@"io"] = @0; m[@"it"] = @(MTLIndexTypeUInt32);
        m[@"count"] = @(pc * cps); m[@"start"] = @0;
        op = m;
    }
    const bool clearOnly = ps == nil;   // a pass without draws, only its load actions
    static NV3DDraw d;
    static uint32_t vsPush[1024], fsPush[1024];
    memset(&d, 0, sizeof d);
    NSArray *rts = op[@"rts"];
    for (NSUInteger i = 0; i < rts.count && i < 8; i++) {
        NVMTLTexture *t = rts[i][@"tex"];
        NV3DTarget *rt = &d.rt[d.nrt];
        rt->format = nvColorTargetFormat(t.fmt);
        rt->va = t.lay ? t.blVa : nvBufVa(t.buf, 0, t.pitch * t.h);
        if (!rt->va) {                                          // no writes to VA 0
            static uint32_t nrt;
            if (nrt++ < 16) NSLog(@"NVMTLDriver: render target %@ %lux%lu fmt %lu has no GPU address, draw skipped",
                                  t.label, (unsigned long)t.w, (unsigned long)t.h, (unsigned long)t.fmt);
            return;
        }
        uint32_t lw = (uint32_t)t.w, lh = (uint32_t)t.h;
        if (t.lay) {
            // the attachment's level and slice (or 3D depth plane)
            const NVTexLayout *L = t.lay.bytes;
            const uint32_t lv = [rts[i][@"level"] unsignedIntValue], sl = [rts[i][@"slice"] unsignedIntValue];
            const uint32_t pl = [rts[i][@"plane"] unsignedIntValue];
            if (lv >= L->levels || sl >= L->layers) { NSLog(@"NVMTLDriver: attachment level/slice out of range"); return; }
            rt->va = t.blVa + sl * L->arrayStride + L->offset[lv];
            rt->blockLinear = true;
            rt->blockHeightLog2 = L->ylog[lv];
            rt->blockDepthLog2 = L->zlog[lv];
            lw = MAX(L->w >> lv, 1u); lh = MAX(L->h >> lv, 1u);
            if (t.samples > 1) {
                nvSampleLayout(t.samples, &rt->sx, &rt->sy, &d.aaMode);
                lw = (uint32_t)t.w; lh = (uint32_t)t.h;
            }
            if (L->d > 1) {
                rt->depthIsZ = true;
                rt->depth = MAX(L->d >> lv, 1u);
                rt->layer = pl;
            }
        }
        if (!rt->format || !rt->va) {
            NSLog(@"NVMTLDriver: color target %lu (format %lu) not usable on the 3D engine", (unsigned long)i,
                  (unsigned long)t.fmt);
            return;
        }
        rt->pitch = (uint32_t)t.pitch; rt->w = lw; rt->h = lh;
        rt->clear = [rts[i][@"load"] unsignedIntegerValue] == MTLLoadActionClear;
        NSArray *cc = rts[i][@"clear"];
        for (int k = 0; k < 4; k++) rt->clearColor[k] = [cc[k] floatValue];
        MTLRenderPipelineColorAttachmentDescriptor *ca = i < ps.colorAtt.count ? ps.colorAtt[i] : nil;
        const MTLColorWriteMask wm = ca ? ca.writeMask : MTLColorWriteMaskAll;
        rt->writeMask = ((wm & MTLColorWriteMaskRed) ? 1u : 0) | ((wm & MTLColorWriteMaskGreen) ? 1u << 4 : 0) |
                        ((wm & MTLColorWriteMaskBlue) ? 1u << 8 : 0) | ((wm & MTLColorWriteMaskAlpha) ? 1u << 12 : 0);
        rt->blend = ca.blendingEnabled;
        rt->colorOp = nvBlendOp(ca.rgbBlendOperation);
        rt->colorSrc = nvBlendFactor(ca.sourceRGBBlendFactor);
        rt->colorDst = nvBlendFactor(ca.destinationRGBBlendFactor);
        rt->alphaOp = nvBlendOp(ca.alphaBlendOperation);
        rt->alphaSrc = nvBlendFactor(ca.sourceAlphaBlendFactor);
        rt->alphaDst = nvBlendFactor(ca.destinationAlphaBlendFactor);
        d.nrt++;
    }
    NSArray *vp = op[@"vp"], *sc = op[@"sc"];
    for (int k = 0; k < 6; k++) d.vp[k] = [vp[k] doubleValue];
    for (int k = 0; k < 4; k++) d.sc[k] = [sc[k] unsignedIntValue];
    if (!clearOnly) {
        id dev = ps.device;
        const int32_t bv = (int32_t)[op[@"bv"] integerValue];
        const uint32_t bi = [op[@"binst"] unsignedIntValue];
        if (op[@"patch"]) {
            // tessellation. TCS push: factor buffer address at the
            // first patch, max factor; TES push: vertex bindings + patchStart
            static uint32_t tcsPush[4], tesPush[1024];
            NSDictionary *pt = op[@"patch"];
            id<MTLBuffer> tf = pt[@"tf"];
            const NSUInteger start = [pt[@"start"] unsignedIntegerValue];
            const uint32_t stride = ps.hwTES.tessDomain == 2 ? 12 : 8;
            const NSUInteger fo = [pt[@"tfOff"] unsignedIntegerValue] + start * stride;
            const uint64_t fva = fo < tf.length ? nvBufVa(tf, fo, tf.length - fo) : 0;
            const float mf = ps.maxTessFactor;
            tcsPush[0] = (uint32_t)fva; tcsPush[1] = (uint32_t)(fva >> 32); memcpy(&tcsPush[2], &mf, 4);
            d.tcs.va = nvShaderVa(ps.hwTCS); d.tcs.gprs = ps.hwTCS.regs; d.tcs.slm = ps.hwTCS.slm;
            d.tcs.push = tcsPush; d.tcs.pushWords = 3;
            d.tes.va = nvShaderVa(ps.hwTES); d.tes.gprs = ps.hwTES.regs; d.tes.slm = ps.hwTES.slm;
            d.tes.push = tesPush;
            uint32_t n = nvStagePush(ps.hwTES, dev, op[@"vbufs"], op[@"vtexs"], op[@"vsamps"], bv, bi, tesPush);
            const uint32_t g = ps.hwTES.nbuf * 2 + ps.hwTES.ntex * 2 + ps.hwTES.nsamp + ps.hwTES.nconstSamp;
            tesPush[g + 2] = (uint32_t)start;
            id<MTLBuffer> pl = pt[@"pidList"];
            if (!pl) {   // no patch index buffer: patchStart, patchStart + 1, ...
                const NSUInteger pc = [op[@"count"] unsignedIntegerValue] / MAX([pt[@"cps"] unsignedIntegerValue], 1u);
                NSMutableData *ids = [NSMutableData dataWithLength:MAX(pc, 1u) * 4];
                for (NSUInteger i = 0; i < pc; i++) ((uint32_t *)ids.mutableBytes)[i] = (uint32_t)(start + i);
                pl = [dev newBufferWithBytes:ids.bytes length:ids.length options:MTLResourceStorageModeShared];
            }
            const uint64_t plva = pl ? nvBufVa(pl, 0, pl.length) : 0;
            tesPush[g + 4] = (uint32_t)plva; tesPush[g + 5] = (uint32_t)(plva >> 32);
            if (n < g + 6) n = g + 6;
            d.tes.pushWords = n;
            d.patchCps = [pt[@"cps"] unsignedIntValue];
            d.tessParams = ps.hwTES.tessParams;
            if (!fva || !d.tcs.va || !d.tes.va) { NSLog(@"NVMTLDriver: tessellation setup failed"); return; }
        }
        d.vs.va = nvShaderVa(ps.hwVS); d.vs.gprs = ps.hwVS.regs; d.vs.slm = ps.hwVS.slm;
        d.fs.va = nvShaderVa(ps.hwFS); d.fs.gprs = ps.hwFS.regs; d.fs.slm = ps.hwFS.slm;
        d.vs.push = vsPush; d.fs.push = fsPush;
        d.vs.pushWords = nvStagePush(ps.hwVS, dev, op[@"vbufs"], op[@"vtexs"], op[@"vsamps"], bv, bi, vsPush);
        d.fs.pushWords = nvStagePush(ps.hwFS, dev, op[@"fbufs"], op[@"ftexs"], op[@"fsamps"], bv, bi, fsPush);
        if (!d.vs.va || !d.fs.va || !d.vs.pushWords || !d.fs.pushWords) { NSLog(@"NVMTLDriver: 3D draw setup failed"); return; }
        // vertex fetch from the pipeline's vertex descriptor
        NSArray *vbufs = op[@"vbufs"];
        for (uint32_t i = 0; i < 31 && ps.vdesc; i++) {
            MTLVertexAttributeDescriptor *a = ps.vdesc.attributes[i];
            if (a.format == MTLVertexFormatInvalid) continue;
            const NSUInteger bidx = a.bufferIndex;
            if (bidx >= 31 || bidx >= vbufs.count || vbufs[bidx] == (id)[NSNull null]) continue;
            uint32_t widths, type;
            bool swap;
            if (!nvVertexFormat(a.format, &widths, &type, &swap)) {
                NSLog(@"NVMTLDriver: vertex format %lu not handled", (unsigned long)a.format);
                continue;
            }
            d.attr[i] = (NV3DAttr){true, (uint32_t)bidx, (uint32_t)a.offset, widths, type, swap};
            NV3DStream *st = &d.stream[bidx];
            if (!st->used) {
                NSDictionary *be = vbufs[bidx];
                id<MTLBuffer> b = be[@"buf"];
                const NSUInteger off = [be[@"off"] unsignedIntegerValue];
                MTLVertexBufferLayoutDescriptor *l = ps.vdesc.layouts[bidx];
                st->used = true;
                st->va = b && off < b.length ? nvBufVa(b, off, b.length - off) : 0;
                st->size = b.length > off ? b.length - off : 0;
                if (!st->va) {                                  // never fetch from VA 0
                    nvNoteNoVa(b, (uint32_t)bidx, ps.hwVS);
                    st->va = nvNullVa(); st->size = 1 << 20;
                }
                st->stride = (uint32_t)l.stride;
                st->divisor = l.stepFunction == MTLVertexStepFunctionPerInstance ? (uint32_t)(l.stepRate ? l.stepRate : 1) : 0;
            }
        }
        static const uint32_t topo[] = {0, 1, 3, 4, 5};   // point, line, line strip, triangle, triangle strip
        const NSUInteger prim = [op[@"prim"] unsignedIntegerValue];
        d.topology = op[@"patch"] ? 0xe : prim < 5 ? topo[prim] : 4;   // 0xe: PATCH
        d.first = [op[@"start"] unsignedIntValue];
        d.count = [op[@"count"] unsignedIntValue];
        d.instances = [op[@"inst"] unsignedIntValue];
        d.baseInstance = bi;
        d.baseVertex = bv;
        if (op[@"ib"]) {
            id<MTLBuffer> ib = op[@"ib"];
            const NSUInteger io = [op[@"io"] unsignedIntegerValue];
            const bool u16 = [op[@"it"] unsignedIntegerValue] == MTLIndexTypeUInt16;
            d.indexed = true;
            d.indexVa = nvBufVa(ib, io, ib.length - io);
            d.indexBytes = ib.length - io;
            d.indexSize = u16 ? 1 : 2;
            d.first = 0;
        }
        const NSUInteger cull = [op[@"cull"] unsignedIntegerValue];
        d.cull = cull == MTLCullModeFront ? 1 : cull == MTLCullModeBack ? 2 : 0;
        d.frontCCW = [op[@"winding"] unsignedIntegerValue] == MTLWindingCounterClockwise;
    }
    NVMTLTexture *zt = op[@"ztex"];
    uint32_t ztFmt = 0, ztKind = 6;
    nvZetaFormat(zt.fmt, &ztFmt, NULL, NULL);
    ztKind = nvZetaKind(zt.fmt);
    if (zt && !zt.blVa) {
        // the 3D engine's zeta surface: block linear in VRAM (PTE kind as
        // NIL picks it for the format), made at first use; for Depth32Float
        // the linear copy stays for the MetalSL raster path
        uint32_t bh = 0;
        uint32_t zsx = 1, zsy = 1;
        nvSampleLayout(zt.samples, &zsx, &zsy, NULL);
        while (bh < 4 && (8u << bh) < zt.h * zsy) bh++;
        const NSUInteger zpitch = (zt.w * zsx * zt.bpp + 63) & ~(NSUInteger)63;
        const NSUInteger rows = (zt.h * zsy + (8u << bh) - 1) & ~(NSUInteger)((8u << bh) - 1);
        uint64_t va = 0;
        uint32_t handle = 0;
        if (!nvVramAllocKind(zpitch * rows, ztKind, &va, &handle)) { NSLog(@"NVMTLDriver: no zeta surface"); zt = nil; }
        else { zt.blVa = va; zt.blHandle = handle; zt.blBh = bh; }
    }
    if (zt) {
        const uint32_t mode = [op[@"zmode"] unsignedIntValue];
        d.zVa = zt.blVa;
        d.zBpp = zt.bpp;
        uint32_t zsx = 1, zsy = 1, zmode = 0;
        nvSampleLayout(zt.samples, &zsx, &zsy, &zmode);
        if (zmode != d.aaMode) NSLog(@"NVMTLDriver: depth and colour sample counts differ");
        d.zWidthEl = (uint32_t)(((zt.w * zsx * zt.bpp + 63) & ~(NSUInteger)63) / zt.bpp);
        d.zHeight = (uint32_t)(zt.h * zsy);
        d.zBlockHeightLog2 = zt.blBh;
        d.zFormat = ztFmt;
        d.sClear = [op[@"sclear"] boolValue];
        d.sClearValue = [op[@"sclearv"] unsignedIntValue];
        if (op[@"sten"]) {
            const uint32_t (*st)[6] = (const uint32_t (*)[6])[op[@"sten"] bytes];
            d.sTest = true;
            for (int k = 0; k < 2; k++) {
                d.sFail[k] = st[k][0]; d.sZFail[k] = st[k][1]; d.sZPass[k] = st[k][2]; d.sFunc[k] = st[k][3];
                d.sReadMask[k] = st[k][4]; d.sWriteMask[k] = st[k][5];
                d.sRef[k] = [op[@"sref"][k] unsignedIntValue] & 0xff;
            }
        }
        d.zClear = [op[@"zclear"] boolValue];
        d.zClearValue = [op[@"zclearv"] floatValue];
        d.zTest = mode != 0;
        d.zWrite = (mode & 8) != 0;
        d.zFunc = 0x200 + (mode & 7);
    }
    d.draw = !clearOnly;
    // with depth, pitch colour targets are drawn through a block-linear twin;
    // so are pitch targets whose rows are not 128-byte aligned (the
    // 3D engine hangs on them; imported IOSurfaces can have such rows)
    NVMTLTexture *shadowed[8] = {nil};
    for (uint32_t i = 0; i < d.nrt; i++) {
        NVMTLTexture *t = rts[i][@"tex"];
        NV3DTarget *rt = &d.rt[i];
        if (rt->blockLinear) continue;             // already block linear
        if (!d.zVa && !(rt->pitch & 127)) continue;
        if (!t.shVa) {
            uint32_t bh = 0;
            while (bh < 4 && (8u << bh) < t.h) bh++;
            const NSUInteger rows = (t.h + (8u << bh) - 1) & ~(NSUInteger)((8u << bh) - 1);
            uint64_t va = 0;
            uint32_t hnd = 0;
            if (!nvVramAllocKind(t.pitch * rows, NV_KIND_COLOR, &va, &hnd)) { NSLog(@"NVMTLDriver: no block-linear twin"); return; }
            t.shVa = va; t.shHandle = hnd; t.shBh = bh;
        }
        if (!rt->clear &&
            !nvCeCopy2DBL(true, rt->va, rt->pitch, t.shVa, (uint32_t)t.pitch, t.shBh, (uint32_t)(t.w * t.bpp), (uint32_t)t.h))
            NSLog(@"NVMTLDriver: colour load into the block-linear twin failed");
        rt->va = t.shVa;
        rt->blockLinear = true;
        rt->blockHeightLog2 = t.shBh;
        shadowed[i] = t;
    }
    {   // label for a stuck native command buffer
        char lb[96];
        snprintf(lb, sizeof lb, "draw(%s/%s)", ps.hwVS.name ? ps.hwVS.name.UTF8String : "?",
                 ps.hwFS.name ? ps.hwFS.name.UTF8String : "?");
        nvGrLabel(lb);
    }
    if (!nvGr3DDraw(&d)) NSLog(@"NVMTLDriver: 3D draw failed");
    for (uint32_t i = 0; i < d.nrt; i++) {
        NVMTLTexture *t = shadowed[i];
        if (t && !nvCeCopy2DBL(false, nvBufVa(t.buf, 0, t.pitch * t.h), (uint32_t)t.pitch, t.shVa,
                               (uint32_t)t.pitch, t.shBh, (uint32_t)(t.w * t.bpp), (uint32_t)t.h))
            NSLog(@"NVMTLDriver: colour store from the block-linear twin failed");
    }
}

@implementation NVMTLCommandBuffer {
    NSMutableArray *_nvOps;
}
+ (void)initialize { if (self == [NVMTLCommandBuffer class]) nvInstallFallback(self); }
- (instancetype)initWithQueue:(id)q retainedReferences:(BOOL)r {
    self = [super initWithQueue:q retainedReferences:r];
    if (self) _nvOps = [NSMutableArray new];
    return self;
}
- (void)nvAddFill:(id)buf range:(NSRange)r value:(uint8_t)v {
    [_nvOps addObject:@{@"op": @"fill", @"buf": buf, @"loc": @(r.location),
                        @"len": @(r.length), @"val": @(v)}];
}
- (void)nvAddCopy:(id)src so:(NSUInteger)so dst:(id)dst do:(NSUInteger)do_ n:(NSUInteger)n {
    [_nvOps addObject:@{@"op": @"copy", @"src": src, @"so": @(so), @"dst": dst,
                        @"do": @(do_), @"n": @(n)}];
}
- (void)nvAddSignal:(id)ev value:(uint64_t)v {
    [_nvOps addObject:@{@"op": @"sig", @"ev": ev, @"val": @(v)}];
}
- (void)nvAddWait:(id)ev value:(uint64_t)v {
    [_nvOps addObject:@{@"op": @"wait", @"ev": ev, @"val": @(v)}];
}
- (void)encodeSignalEvent:(id)ev value:(uint64_t)v { [self nvAddSignal:ev value:v]; }
- (void)encodeWaitForEvent:(id)ev value:(uint64_t)v { [self nvAddWait:ev value:v]; }
- (void)nvAddDispatch:(NVMTLComputePipelineState *)ps bufs:(NSArray *)bufs texs:(NSArray *)texs
                samps:(NSArray *)samps grid:(MTLSize)grid block:(MTLSize)block {
    [_nvOps addObject:@{@"op": @"dispatch", @"ps": ps, @"bufs": [bufs copy],
                        @"texs": [texs copy], @"samps": [samps copy],
                        @"gx": @(grid.width), @"gy": @(grid.height), @"gz": @(grid.depth),
                        @"bx": @(block.width), @"by": @(block.height), @"bz": @(block.depth)}];
}
// threadgroup counts read from the buffer when the op runs, so
// a count an earlier op of the same command buffer wrote is honoured.
- (void)nvAddDispatch:(NVMTLComputePipelineState *)ps bufs:(NSArray *)bufs texs:(NSArray *)texs
                samps:(NSArray *)samps indirect:(id<MTLBuffer>)ib offset:(NSUInteger)io block:(MTLSize)block {
    [_nvOps addObject:@{@"op": @"dispatch", @"ps": ps, @"bufs": [bufs copy],
                        @"texs": [texs copy], @"samps": [samps copy], @"ind": ib, @"indOff": @(io),
                        @"gx": @0, @"gy": @0, @"gz": @0,
                        @"bx": @(block.width), @"by": @(block.height), @"bz": @(block.depth)}];
}
- (void)nvExecuteComputeOp:(NSDictionary *)op {
    NVMTLComputePipelineState *ps = op[@"ps"];
    NVMTLKernel *k = ps.kernel;
    NSArray *bufs = op[@"bufs"]; // per index: {buf, off} or NSNull
    if (!k || !k.code.length) { NSLog(@"NVMTLDriver: dispatch without kernel"); return; }
    // Stage every bound buffer fully (inputs and outputs alike, for now).
    uint8_t *st = nvStageCpu();
    const uint64_t sv = nvStageVa();
    uint64_t stageOff = 0;
    uint32_t push[256] = {0};
    const uint32_t texBase = k.nbuf * 2, texInfo = texBase + k.ntex * 2;
    // hardware layout (AIR kernels): u64 texture {tic, flags}, u32 samplers,
    // constexpr samplers; MetalSL kernels: address, 4 info words, mode
    const uint32_t sampBase = k.hwTex ? texBase + k.ntex * 2 : texInfo + k.ntex * 4;
    const uint32_t gridBase = sampBase + k.nsamp + (k.hwTex ? k.nconstSamp : 0);
    uint32_t npush = gridBase + 6;
    if (k.cdataDword) npush = k.cdataDword + 2;
    if (npush > 256) { NSLog(@"NVMTLDriver: too many kernel arguments"); return; }
    for (NSUInteger i = 0; i < bufs.count && i < k.nbuf; i++) {
        NSDictionary *be = bufs[i];
        if ((id)be == [NSNull null]) continue;
        id<MTLBuffer> b = be[@"buf"];
        NSUInteger off = [be[@"off"] unsignedIntegerValue];
        uint8_t *c = b.contents;
        if (off >= b.length) { NSLog(@"NVMTLDriver: dispatch buf %lu bad", (unsigned long)i); return; }
        NSUInteger len = b.length - off;
        const uint64_t hva = nvBufVa(b, off, len);
        if (!hva && !c) { NSLog(@"NVMTLDriver: dispatch buf %lu has no GPU address", (unsigned long)i); return; }
        if (hva) { push[2 * i] = (uint32_t)hva; push[2 * i + 1] = (uint32_t)(hva >> 32); continue; }
        if (stageOff + len > NVGSP_STAGE_DATA_MAX) { NSLog(@"NVMTLDriver: dispatch staging overflow"); return; }
        memcpy(st + NVGSP_STAGE_DATA + stageOff, c + off, len);
        const uint64_t va = sv + NVGSP_STAGE_DATA + stageOff;
        push[2 * i] = (uint32_t)va; push[2 * i + 1] = (uint32_t)(va >> 32);
        stageOff += (len + 63) & ~63ull;
    }
    NSArray *texs = op[@"texs"], *samps = op[@"samps"];
    {   // what the launch reads through textures, for the fault dump
        char note[480]; int nn = 0; note[0] = 0;
        for (uint32_t i = 0; i < k.ntex && nn < (int)sizeof note - 96; i++) {
            NVMTLTexture *t = i < texs.count && texs[i] != (id)[NSNull null] ? texs[i] : nil;
            if (!t) { nn += snprintf(note + nn, sizeof note - nn, " t%u unbound", i); continue; }
            nn += snprintf(note + nn, sizeof note - nn, " t%u %lux%lux%lu fmt %lu %s%s bl %llx va %llx", i,
                           (unsigned long)t.w, (unsigned long)t.h, (unsigned long)[t arrayLength], (unsigned long)t.fmt,
                           t.priv ? "priv" : "shared", nvLeaseOf(t) ? " heap" : "", t.blVa,
                           t.buf ? nvBufVa(t.buf, 0, 1) : 0ULL);
        }
        nvGrNoteTextures(note);
    }
    for (uint32_t i = 0; k.hwTex && i < k.ntex; i++) {
        NVMTLTexture *t = i < texs.count && texs[i] != (id)[NSNull null] ? texs[i] : nil;
        if (!t) continue;                        // unbound: null header, reads give zero
        const MTLResourceID rid = [t gpuResourceID];
        if (!(uint32_t)rid._impl) { NSLog(@"NVMTLDriver: texture %u has no hardware header (format %lu pitch %lu)", i,
                                          (unsigned long)t.fmt, (unsigned long)t.pitch); return; }
        push[texBase + 2 * i] = (uint32_t)rid._impl;
        push[texBase + 2 * i + 1] = (uint32_t)(rid._impl >> 32);
    }
    for (uint32_t i = 0; k.hwTex && i < k.nsamp; i++) {
        NVMTLSamplerState *sm = i < samps.count && samps[i] != (id)[NSNull null] ? samps[i] : nil;
        push[sampBase + i] = sm ? sm.tsc : 0;
    }
    for (uint32_t i = 0; k.hwTex && i < k.nconstSamp; i++)
        push[sampBase + k.nsamp + i] = ((const uint32_t *)k.constTsc.bytes)[i];
    for (uint32_t i = 0; !k.hwTex && i < k.ntex; i++) {
        NVMTLTexture *t = i < texs.count && texs[i] != (id)[NSNull null] ? texs[i] : nil;
        if (!t) { NSLog(@"NVMTLDriver: dispatch texture %u not bound", i); return; }
        const uint64_t va = nvBufVa(t.buf, 0, t.pitch * t.h);
        if (!va) { NSLog(@"NVMTLDriver: dispatch texture %u has no GPU address", i); return; }
        push[texBase + 2 * i] = (uint32_t)va;
        push[texBase + 2 * i + 1] = (uint32_t)(va >> 32);
        push[texInfo + 4 * i + 0] = (uint32_t)t.pitch;
        push[texInfo + 4 * i + 1] = (uint32_t)t.w;
        push[texInfo + 4 * i + 2] = (uint32_t)t.h;
        push[texInfo + 4 * i + 3] = t.fcode;
    }
    for (uint32_t i = 0; !k.hwTex && i < k.nsamp; i++) {
        NVMTLSamplerState *sm = i < samps.count && samps[i] != (id)[NSNull null] ? samps[i] : nil;
        push[sampBase + i] = sm ? sm.mode : 0;   // unbound: nearest, clamp to edge
    }
    uint32_t grid[3] = {[op[@"gx"] unsignedIntValue], [op[@"gy"] unsignedIntValue], [op[@"gz"] unsignedIntValue]};
    if (op[@"ind"]) {
        id<MTLBuffer> ib = op[@"ind"];
        const NSUInteger io = [op[@"indOff"] unsignedIntegerValue];
        if (!ib.contents || io + 12 > ib.length) { NSLog(@"NVMTLDriver: bad indirect dispatch buffer"); return; }
        nvGrSync();                              // a queued launch may write the arguments
        const uint32_t *g = (const uint32_t *)((const uint8_t *)ib.contents + io);
        if (!g[0] || !g[1] || !g[2]) return;   // an empty dispatch is legal
        grid[0] = g[0] * [op[@"bx"] unsignedIntValue];
        grid[1] = g[1] * [op[@"by"] unsignedIntValue];
        grid[2] = g[2] * [op[@"bz"] unsignedIntValue];
    }
    uint32_t block[3] = {[op[@"bx"] unsignedIntValue], [op[@"by"] unsignedIntValue], [op[@"bz"] unsignedIntValue]};
    for (int d = 0; d < 3; d++) {
        if (!grid[d]) grid[d] = 1;
        push[gridBase + d] = grid[d];
        push[gridBase + 3 + d] = block[d];
    }
    // Metal allows grid % block != 0; v1 rounds up (test uses exact).
    uint32_t groups[3];
    for (int d = 0; d < 3; d++) {
        if (!block[d]) { NSLog(@"NVMTLDriver: zero block dim"); return; }
        groups[d] = (grid[d] + block[d] - 1) / block[d];
    }
    if (!k.isVertex) k = nvKernelForBlock(k, block[0], block[1], block[2]);
    if (k.cdataDword) {                          // AIR constant globals, uploaded once
        @synchronized (k) {
            if (!k.cdataBuf)
                k.cdataBuf = [ps.device newBufferWithBytes:k.cdata.bytes length:k.cdata.length
                                                   options:MTLResourceStorageModeShared];
        }
        const uint64_t cva = nvBufVa(k.cdataBuf, 0, k.cdata.length);
        if (!cva) { NSLog(@"NVMTLDriver: kernel constants have no GPU address"); return; }
        push[k.cdataDword] = (uint32_t)cva;
        push[k.cdataDword + 1] = (uint32_t)(cva >> 32);
        if (k.cdataDword + 2 > npush) npush = k.cdataDword + 2;
    }
    const uint32_t *code = k.code.bytes;
    // GPU-visible buffers only: queue it with the rest of the
    // command buffer; staged buffers need the copy back right away
    nvGrLabel(k.name ? k.name.UTF8String : "?");   // (0.8.12: always, for the fault dump)
    if (!(stageOff ? nvGrLaunch : nvGrQueue)(code, (uint32_t)(k.code.length / 4), k.regs, k.slm, k.smem, k.barriers,
                                             push, k.isVertex ? k.nbuf * 2 : npush, groups, block)) {
        NSLog(@"NVMTLDriver: GR launch failed"); return;
    }
    if (!stageOff) return;
    // Copy back every bound buffer (v1: inputs get identical bytes back).
    stageOff = 0;
    for (NSUInteger i = 0; i < bufs.count && i < k.nbuf; i++) {
        NSDictionary *be = bufs[i];
        if ((id)be == [NSNull null]) continue;
        id<MTLBuffer> b = be[@"buf"];
        NSUInteger off = [be[@"off"] unsignedIntegerValue];
        NSUInteger len = b.length - off;
        if (nvBufVa(b, off, len)) continue;
        memcpy(((uint8_t *)b.contents) + off, st + NVGSP_STAGE_DATA + stageOff, len);
        stageOff += (len + 63) & ~63ull;
    }
}
- (id)blitCommandEncoder {
    Class cls = objc_getClass("NVMTLBlitEncoder");
    id enc = [[cls alloc] initWithCommandBuffer:self];
    ((void (*)(id, SEL, id))objc_msgSend)(enc, @selector(nvSetCB:), self);
    return enc;
}
- (id)computeCommandEncoder {
    Class cls = objc_getClass("NVMTLComputeEncoder");
    id enc = [[cls alloc] initWithCommandBuffer:self];
    ((void (*)(id, SEL, id))objc_msgSend)(enc, @selector(nvSetCB:), self);
    return enc;
}
- (id)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)desc {
    Class cls = objc_getClass("NVMTLRenderEncoder");
    id enc = [[cls alloc] initWithCommandBuffer:self];
    ((void (*)(id, SEL, id))objc_msgSend)(enc, @selector(nvSetCB:), self);
    ((void (*)(id, SEL, id))objc_msgSend)(enc, @selector(nvSetDesc:), desc);
    return enc;
}
- (void)nvAddDraw:(NVMTLRenderPipelineState *)ps vbufs:(NSArray *)vbufs
               vp:(MTLViewport)vp sc:(MTLScissorRect)sc tex:(NVMTLTexture *)tex
             load:(MTLLoadAction)load clear:(MTLClearColor)clear
            vStart:(NSUInteger)vStart vCount:(NSUInteger)vCount idx:(NSData *)idx {
    [_nvOps addObject:@{@"op": @"draw", @"ps": ps, @"vbufs": [vbufs copy], @"tex": tex,
                        @"load": @(load), @"vStart": @(vStart), @"vCount": @(vCount),
                        @"idx": idx ? idx : (id)[NSNull null],
                        @"vp": @[@(vp.originX), @(vp.originY), @(vp.width), @(vp.height)],
                        @"sc": @[@(sc.x), @(sc.y), @(sc.width), @(sc.height)],
                        @"clear": @[@(clear.red), @(clear.green), @(clear.blue), @(clear.alpha)]}];
}
- (void)nvAddDraw2:(NSDictionary *)op { [_nvOps addObject:op]; }
- (void)nvAddOp:(NSDictionary *)op { [_nvOps addObject:op]; }

// Resource push block of a kernel (buffers, textures, samplers), the layout
// nvCompileKernels/compileFragmentRaster declare. Returns the dword count,
// 0 on error.
static uint32_t nvResourcePush(NVMTLKernel *k, NSArray *bufs, NSArray *texs, NSArray *samps,
                               uint32_t *push, uint32_t max) {
    const uint32_t texBase = k.nbuf * 2, texInfo = texBase + k.ntex * 2;
    const uint32_t sampBase = texInfo + k.ntex * 4, n = sampBase + k.nsamp;
    if (n > max) return 0;
    for (uint32_t i = 0; i < k.nbuf; i++) {
        NSDictionary *be = i < bufs.count ? bufs[i] : nil;
        if (!be || (id)be == [NSNull null]) continue;
        id<MTLBuffer> b = be[@"buf"];
        const NSUInteger off = [be[@"off"] unsignedIntegerValue];
        const uint64_t va = off < b.length ? nvBufVa(b, off, b.length - off) : 0;
        if (!va) { NSLog(@"NVMTLDriver: buffer %u has no GPU address", i); return 0; }
        push[2 * i] = (uint32_t)va; push[2 * i + 1] = (uint32_t)(va >> 32);
    }
    for (uint32_t i = 0; i < k.ntex; i++) {
        NVMTLTexture *t = i < texs.count && texs[i] != (id)[NSNull null] ? texs[i] : nil;
        if (!t) { NSLog(@"NVMTLDriver: texture %u not bound", i); return 0; }
        const uint64_t va = nvBufVa(t.buf, 0, t.pitch * t.h);
        if (!va) { NSLog(@"NVMTLDriver: texture %u has no GPU address", i); return 0; }
        push[texBase + 2 * i] = (uint32_t)va; push[texBase + 2 * i + 1] = (uint32_t)(va >> 32);
        push[texInfo + 4 * i + 0] = (uint32_t)t.pitch;
        push[texInfo + 4 * i + 1] = (uint32_t)t.w;
        push[texInfo + 4 * i + 2] = (uint32_t)t.h;
        push[texInfo + 4 * i + 3] = t.fcode;
    }
    for (uint32_t i = 0; i < k.nsamp; i++) {
        NVMTLSamplerState *sm = i < samps.count && samps[i] != (id)[NSNull null] ? samps[i] : nil;
        push[sampBase + i] = sm ? sm.mode : 0;
    }
    return n;
}

static uint16_t nvHalf(float f) {
    __fp16 h = (__fp16)f;
    uint16_t u; memcpy(&u, &h, 2);
    return u;
}

// Clear value for a 4-byte-per-pixel format, false for the rest.
static bool nvClearWord(uint32_t fcode, const double *c, uint32_t *out) {
    uint32_t r = (uint32_t)(fmin(fmax(c[0], 0), 1) * 255 + 0.5), g = (uint32_t)(fmin(fmax(c[1], 0), 1) * 255 + 0.5);
    uint32_t b = (uint32_t)(fmin(fmax(c[2], 0), 1) * 255 + 0.5), a = (uint32_t)(fmin(fmax(c[3], 0), 1) * 255 + 0.5);
    float fr = (float)c[0];
    switch (fcode) {
    case 3: *out = r | g << 8 | b << 16 | a << 24; return true;          // RGBA8
    case 4: *out = b | g << 8 | r << 16 | a << 24; return true;          // BGRA8
    case 6: *out = nvHalf((float)c[0]) | (uint32_t)nvHalf((float)c[1]) << 16; return true;   // RG16F
    case 8: memcpy(out, &fr, 4); return true;                            // R32F
    case 11: *out = (uint32_t)c[0]; return true;                         // R32Uint
    case 12: *out = (uint32_t)c[0] | (uint32_t)c[1] << 8 | (uint32_t)c[2] << 16 | (uint32_t)c[3] << 24; return true;
    default: return false;
    }
}

// Vertex outputs of the current draw: one growable heap buffer (command
// buffers run one at a time here).
static void *gVoutCpu;
static uint64_t gVoutVa, gVoutLen;
static bool nvVoutEnsure(uint64_t bytes) {
    if (bytes <= gVoutLen) return true;
    if (gVoutCpu) nvHeapFree(gVoutCpu, gVoutLen);
    gVoutCpu = NULL; gVoutLen = 0;
    const uint64_t len = (bytes + 0xfffff) & ~0xfffffULL;
    if (!nvHeapAlloc(len, &gVoutCpu, &gVoutVa)) return false;
    gVoutLen = len;
    return true;
}

- (void)nvExecuteDraw2Op:(NSDictionary *)op {
    NVMTLTexture *tex = op[@"tex"];
    const uint64_t rtVa = nvBufVa(tex.buf, 0, tex.pitch * tex.h);
    if (!rtVa) { NSLog(@"NVMTLDriver: render target has no GPU address"); return; }
    if ([op[@"load"] intValue] == MTLLoadActionClear) {
        NSArray *cc = op[@"clear"];
        const double c[4] = {[cc[0] doubleValue], [cc[1] doubleValue], [cc[2] doubleValue], [cc[3] doubleValue]};
        uint32_t w = 0;
        if (!nvClearWord(tex.fcode, c, &w)) NSLog(@"NVMTLDriver: clear of format %lu not supported", (unsigned long)tex.fmt);
        else if (!nvCeFill(rtVa, tex.pitch * tex.h, w)) NSLog(@"NVMTLDriver: RT clear failed");
    }
    NVMTLTexture *zt = op[@"ztex"];
    const uint64_t zVa = zt ? nvBufVa(zt.buf, 0, zt.pitch * zt.h) : 0;
    if (zt && !zVa) NSLog(@"NVMTLDriver: depth attachment has no GPU address");
    if (zVa && [op[@"zload"] intValue] == MTLLoadActionClear) {
        const float zc = [op[@"zclear"] floatValue];
        uint32_t w; memcpy(&w, &zc, 4);
        if (!nvCeFill(zVa, zt.pitch * zt.h, w)) NSLog(@"NVMTLDriver: depth clear failed");
    }
    const uint32_t zmode = zVa ? [op[@"zmode"] unsignedIntValue] : 0;
    NSUInteger vCount = [op[@"vCount"] unsignedIntegerValue], vStartI = [op[@"vStart"] unsignedIntegerValue];
    uint32_t nInst = op[@"inst"] ? [op[@"inst"] unsignedIntValue] : 1;
    if (op[@"ind"]) {   // MTLDrawPrimitivesIndirectArguments
        id<MTLBuffer> ib = op[@"ind"];
        const NSUInteger io = [op[@"indOff"] unsignedIntegerValue];
        if (!ib.contents || io + 16 > ib.length) { NSLog(@"NVMTLDriver: bad indirect draw buffer"); return; }
        const uint32_t *a = (const uint32_t *)((const uint8_t *)ib.contents + io);
        if (!a[1]) return;
        vCount = a[0]; vStartI = a[2]; nInst = a[1];
    }
    NVMTLRenderPipelineState *ps = op[@"ps"];
    if (!vCount || !ps) return;
    NVMTLKernel *vs = ps.vs, *rast = ps.fs.raster;
    NSData *idxd = op[@"idx"];
    const uint32_t *idx = idxd.bytes;
    const NSUInteger tri = idxd ? idxd.length / 4 : vCount;
    if (tri % 3) { NSLog(@"NVMTLDriver: vertex/index count not a multiple of 3"); return; }
    const uint32_t S = vs.voutStride, pos = vs.posSlot;
    if (S > 8) { NSLog(@"NVMTLDriver: vertex output has %u slots, raster takes 8", S); return; }
    // 1. vertex stage into the vout buffer
    const uint32_t groups = (uint32_t)((vCount + 255) / 256);
    if (!nvVoutEnsure((uint64_t)groups * 256 * S * 16)) { NSLog(@"NVMTLDriver: no vertex output memory"); return; }
    uint32_t push[256] = {0};
    const uint32_t nb = nvResourcePush(vs, op[@"vbufs"], nil, nil, push, 250);
    if (vs.nbuf && !nb) return;
    push[vs.nbuf * 2] = (uint32_t)gVoutVa; push[vs.nbuf * 2 + 1] = (uint32_t)(gVoutVa >> 32);
    push[vs.nbuf * 2 + 2] = (uint32_t)vStartI;
    push[vs.nbuf * 2 + 3] = (uint32_t)vCount;
    for (uint32_t inst = 0; inst < nInst; inst++) {
    push[vs.nbuf * 2 + 4] = inst;
    uint32_t grid[3] = {groups, 1, 1}, block[3] = {256, 1, 1};
    if (!nvGrLaunch(vs.code.bytes, (uint32_t)(vs.code.length / 4), vs.regs, vs.slm, vs.smem, vs.barriers,
                    push, vs.nbuf * 2 + 5, grid, block)) {
        NSLog(@"NVMTLDriver: vertex launch failed"); return;
    }
    // 2. one raster launch per triangle over its screen bbox
    NSArray *va = op[@"vp"], *sa = op[@"sc"];
    const float vp[4] = {[va[0] floatValue], [va[1] floatValue], [va[2] floatValue], [va[3] floatValue]};
    NSUInteger sx0 = [sa[0] unsignedIntegerValue], sy0 = [sa[1] unsignedIntegerValue];
    NSUInteger sx1 = MIN(sx0 + [sa[2] unsignedIntegerValue], tex.w), sy1 = MIN(sy0 + [sa[3] unsignedIntegerValue], tex.h);
    if (sx0 >= sx1 || sy0 >= sy1) return;
    uint32_t rp[256] = {0};
    const uint32_t nr = nvResourcePush(rast, op[@"fbufs"], op[@"ftexs"], op[@"fsamps"], rp, 200);
    if ((rast.nbuf || rast.ntex || rast.nsamp) && !nr) return;
    const uint32_t r0 = (nr + 1) & ~1u;   // scalar layout: the u64 after it is 8-byte aligned
    if (r0 + 26 + 3 * S * 4 > 256) { NSLog(@"NVMTLDriver: fragment arguments + vertex slots exceed the push space"); return; }
    const float *vo = gVoutCpu;
    for (NSUInteger t = 0; t < tri; t += 3) {
        const uint32_t iv[3] = {idx ? idx[t] : (uint32_t)t, idx ? idx[t + 1] : (uint32_t)t + 1,
                                idx ? idx[t + 2] : (uint32_t)t + 2};
        if (iv[0] >= vCount || iv[1] >= vCount || iv[2] >= vCount) { NSLog(@"NVMTLDriver: index out of range"); return; }
        float mnx = 1e30f, mny = 1e30f, mxx = -1e30f, mxy = -1e30f;
        bool behind = false;
        for (int k = 0; k < 3; k++) {
            const float *c = vo + ((size_t)iv[k] * S + pos) * 4;
            if (!(c[3] > 0)) { behind = true; break; }
            const float x = vp[0] + (c[0] / c[3] * 0.5f + 0.5f) * vp[2];
            const float y = vp[1] + (0.5f - c[1] / c[3] * 0.5f) * vp[3];
            mnx = fminf(mnx, x); mny = fminf(mny, y); mxx = fmaxf(mxx, x); mxy = fmaxf(mxy, y);
        }
        NSUInteger bx0 = sx0, by0 = sy0, bx1 = sx1, by1 = sy1;
        if (!behind) {
            if (mxx < (float)sx0 || mxy < (float)sy0 || mnx >= (float)sx1 || mny >= (float)sy1) continue;
            bx0 = MAX(sx0, (NSUInteger)fmaxf(floorf(mnx), 0)); by0 = MAX(sy0, (NSUInteger)fmaxf(floorf(mny), 0));
            bx1 = MIN(sx1, (NSUInteger)ceilf(mxx) + 1); by1 = MIN(sy1, (NSUInteger)ceilf(mxy) + 1);
            if (bx0 >= bx1 || by0 >= by1) continue;
        }
        uint32_t *q = rp + r0;
        q[0] = (uint32_t)gVoutVa; q[1] = (uint32_t)(gVoutVa >> 32);
        q[2] = (uint32_t)rtVa; q[3] = (uint32_t)(rtVa >> 32);
        q[4] = (uint32_t)tex.pitch; q[5] = (uint32_t)tex.w; q[6] = (uint32_t)tex.h; q[7] = tex.fcode;
        q[8] = iv[0]; q[9] = iv[1]; q[10] = iv[2]; q[11] = S; q[12] = pos;
        q[13] = (uint32_t)bx0; q[14] = (uint32_t)by0; q[15] = (uint32_t)(bx1 - bx0); q[16] = (uint32_t)(by1 - by0);
        memcpy(q + 17, vp, 16);
        q[21] = ps.blend;
        q[22] = (uint32_t)zVa; q[23] = (uint32_t)(zVa >> 32);
        q[24] = zt ? (uint32_t)zt.pitch : 0; q[25] = zmode;
        for (int k = 0; k < 3; k++)   // the triangle's vertex slots ride in push
            memcpy(q + 26 + (size_t)k * S * 4, vo + (size_t)iv[k] * S * 4, S * 16);
        const uint32_t npx = q[15] * q[16];
        uint32_t rg[3] = {(npx + 255) / 256, 1, 1};
        if (!nvGrLaunch(rast.code.bytes, (uint32_t)(rast.code.length / 4), rast.regs, rast.slm, rast.smem, rast.barriers,
                        rp, r0 + 26 + 3 * S * 4, rg, block)) {
            NSLog(@"NVMTLDriver: raster launch failed"); return;
        }
    }
    }   // instances
}
- (void)nvExecuteDrawOp:(NSDictionary *)op {
    NVMTLRenderPipelineState *ps = op[@"ps"];
    NVMTLTexture *tex = op[@"tex"];
    if (!ps.vs) { NSLog(@"NVMTLDriver: draw without VS"); return; }
    const NSUInteger W = tex.w, H = tex.h, stride = W;
    if (!W || !H || W * H * 4 > NVGSP_STAGE_DATA_MAX) { NSLog(@"NVMTLDriver: RT too big"); return; }
    if (tex.fmt != MTLPixelFormatRGBA8Unorm && tex.fmt != MTLPixelFormatBGRA8Unorm) {
        NSLog(@"NVMTLDriver: render target format %lu not supported by the v1 rasterizer",
              (unsigned long)tex.fmt);
        return;
    }
    uint8_t *st = nvStageCpu();
    uint8_t *rt = st + NVGSP_STAGE_DATA;
    const bool bgra = tex.fmt == MTLPixelFormatBGRA8Unorm;
    // Load/clear into RT staging.
    if ([op[@"load"] intValue] == MTLLoadActionClear) {
        NSArray *cc = op[@"clear"];
        const uint32_t r = (uint32_t)([cc[0] floatValue] * 255);
        const uint32_t g = (uint32_t)([cc[1] floatValue] * 255);
        const uint32_t b = (uint32_t)([cc[2] floatValue] * 255);
        const uint32_t a = (uint32_t)([cc[3] floatValue] * 255);
        const uint32_t v = bgra ? (b | (g << 8) | (r << 16) | (a << 24))
                                : (r | (g << 8) | (b << 16) | (a << 24));
        if (!nvCeFill(nvStageVa() + NVGSP_STAGE_DATA, W * H * 4, v)) {
            NSLog(@"NVMTLDriver: RT clear failed"); return;
        }
    } else if ([op[@"load"] intValue] == MTLLoadActionLoad) {
        for (NSUInteger y = 0; y < H; y++)
            memcpy(rt + y * W * 4, (uint8_t *)tex.buf.contents + y * tex.pitch, W * 4);
    }
    NSArray *va = op[@"vp"], *sa = op[@"sc"];
    double vp[4] = {[va[0] doubleValue], [va[1] doubleValue], [va[2] doubleValue], [va[3] doubleValue]};
    NSUInteger sc[4] = {[sa[0] unsignedIntegerValue], [sa[1] unsignedIntegerValue],
                        [sa[2] unsignedIntegerValue], [sa[3] unsignedIntegerValue]};
    id idxo = op[@"idx"];
    NSData *idxd = idxo == (id)[NSNull null] ? nil : idxo;
    if (!nvRenderDraw(ps.vs, ps.nvFsColor, op[@"vbufs"], vp, sc,
                      (uint32_t)W, (uint32_t)H, (uint32_t)stride,
                      [op[@"vStart"] unsignedIntegerValue], [op[@"vCount"] unsignedIntegerValue], bgra,
                      idxd ? idxd.bytes : NULL, idxd ? idxd.length / 4 : 0)) {
        NSLog(@"NVMTLDriver: draw failed"); return;
    }
    for (NSUInteger y = 0; y < H; y++)
        memcpy((uint8_t *)tex.buf.contents + y * tex.pitch, rt + y * W * 4, W * 4);
}
- (void)nvExecuteOps {
    if (!_nvOps.count) return;
    if (!nvGspEnsure()) { NSLog(@"NVMTLDriver: GSP unavailable, %lu ops dropped", (unsigned long)_nvOps.count); return; }
    uint8_t *st = nvStageCpu();
    const uint64_t sv = nvStageVa();
    nvManagedFlush();
    // one GPU submitter at a time, and compute work of the whole
    // command buffer goes out as one batch (nvGrQueue). Ops that touch
    // memory on the CPU or signal flush what is queued first.
    const bool native = nvNativeEnabled();
    if (native) nvGrBatchBeginNative(); else nvGrBatchBegin();
    for (NSDictionary *op in _nvOps) {
        NSString *kind = op[@"op"];
        if (!([kind isEqual:@"dispatch"] || [kind isEqual:@"fill"] || [kind isEqual:@"copy"] ||
              [kind isEqual:@"copy2d"] || [kind isEqual:@"draw3d"]))   // draws batch too
            nvGrSync();
        if ([op[@"op"] isEqual:@"fill"]) {
            id<MTLBuffer> b = op[@"buf"];
            NSUInteger loc = [op[@"loc"] unsignedIntegerValue], len = [op[@"len"] unsignedIntegerValue];
            uint8_t v = [op[@"val"] unsignedCharValue];
            uint8_t *c = b.contents;
            const uint64_t hva = nvBufVa(b, loc, len);
            if ((!c && !hva) || !len || loc + len > b.length) {
                NSLog(@"NVMTLDriver: fill skipped (contents %p len %lu)", c, (unsigned long)len); continue;
            }
            const uint32_t w = (uint32_t)v * 0x01010101u;
            if (hva) {
                if (!nvComputeBlit(YES, hva, 0, len, w) && !nvCeFill(hva, len, w)) NSLog(@"NVMTLDriver: CE fill failed");
                continue;
            }
            nvGrSync();
            bool fok = true;
            for (NSUInteger o = 0; o < len && fok; o += NVGSP_STAGE_DATA_MAX) {
                const NSUInteger k = len - o > NVGSP_STAGE_DATA_MAX ? NVGSP_STAGE_DATA_MAX : len - o;
                fok = nvCeFill(sv + NVGSP_STAGE_DATA, k, w);
                if (fok) memcpy(c + loc + o, st + NVGSP_STAGE_DATA, k);
            }
            if (!fok) NSLog(@"NVMTLDriver: CE fill failed");
        } else if ([op[@"op"] isEqual:@"copy"]) {
            id<MTLBuffer> s = op[@"src"], d = op[@"dst"];
            NSUInteger so = [op[@"so"] unsignedIntegerValue], do_ = [op[@"do"] unsignedIntegerValue],
                       n = [op[@"n"] unsignedIntegerValue];
            uint8_t *sc = s.contents, *dc = d.contents;
            const uint64_t sva = nvBufVa(s, so, n), dva = nvBufVa(d, do_, n);
            if ((!sc && !sva) || (!dc && !dva) || !n || so + n > s.length || do_ + n > d.length) {
                NSLog(@"NVMTLDriver: copy skipped"); continue;
            }
            if (sva && dva) {
                if (!nvComputeBlit(NO, dva, sva, n, 0) && !nvCeCopy(dva, sva, n)) NSLog(@"NVMTLDriver: CE copy failed");
                continue;
            }
            // 512 KiB halves of DATA: src half + dst half.
            nvGrSync();
            bool ok = true;
            for (NSUInteger o = 0; o < n && ok; o += 1 << 19) {
                const NSUInteger k = n - o > (1u << 19) ? (1u << 19) : n - o;
                memcpy(st + NVGSP_STAGE_DATA, sc + so + o, k);
                ok = nvCeCopy(sv + NVGSP_STAGE_DATA + (1 << 19), sv + NVGSP_STAGE_DATA, k);
                if (ok) memcpy(dc + do_ + o, st + NVGSP_STAGE_DATA + (1 << 19), k);
            }
            if (!ok) NSLog(@"NVMTLDriver: CE copy failed");
        } else if ([op[@"op"] isEqual:@"copy2d"]) {
            id<MTLBuffer> sb = op[@"src"], db = op[@"dst"];
            const NSUInteger so = [op[@"so"] unsignedIntegerValue], do_ = [op[@"do"] unsignedIntegerValue];
            const NSUInteger sp = [op[@"sp"] unsignedIntegerValue], dp = [op[@"dp"] unsignedIntegerValue];
            const NSUInteger rb = [op[@"rb"] unsignedIntegerValue], rows = [op[@"rows"] unsignedIntegerValue];
            const NSUInteger sEnd = so + (rows ? (rows - 1) * sp : 0) + rb, dEnd = do_ + (rows ? (rows - 1) * dp : 0) + rb;
            if (sEnd > sb.length || dEnd > db.length) { NSLog(@"NVMTLDriver: 2D copy out of range"); continue; }
            const uint64_t sva = nvBufVa(sb, so, sEnd - so), dva = nvBufVa(db, do_, dEnd - do_);
            if (sva && dva) {
                if (!nvCopy2D(dva, dp, sva, sp, rb, rows)) NSLog(@"NVMTLDriver: 2D copy failed");
            } else if (sb.contents && db.contents) {
                nvGrSync();
                for (NSUInteger y = 0; y < rows; y++)
                    memcpy((uint8_t *)db.contents + do_ + y * dp, (const uint8_t *)sb.contents + so + y * sp, rb);
            } else if ((sva && db.contents) || (sb.contents && dva)) {
                // one side has only a GPU address, the other only
                // CPU pages (WindowServer, 28 Sep): the whole span through
                // the copy engine once, rows on the CPU
                nvGrSync();
                const BOOL down = sva != 0;
                const NSUInteger span = down ? sEnd - so : dEnd - do_;
                uint8_t *tmp = malloc(span);
                BOOL ok = tmp != NULL;
                if (ok && down) {
                    ok = nvBufIO(sb, NO, so, span, tmp);
                    for (NSUInteger y = 0; ok && y < rows; y++)
                        memcpy((uint8_t *)db.contents + do_ + y * dp, tmp + y * sp, rb);
                } else if (ok) {
                    ok = nvBufIO(db, NO, do_, span, tmp);   // keep the bytes between rows
                    for (NSUInteger y = 0; ok && y < rows; y++)
                        memcpy(tmp + y * dp, (const uint8_t *)sb.contents + so + y * sp, rb);
                    ok = ok && nvBufIO(db, YES, do_, span, tmp);
                }
                free(tmp);
                if (!ok) NSLog(@"NVMTLDriver: 2D copy through the copy engine failed");
            } else {
                NSLog(@"NVMTLDriver: 2D copy between resources without GPU or CPU access: %s %lu (%s) -> %s %lu (%s)",
                      object_getClassName(sb), (unsigned long)sb.length, sva ? "va" : sb.contents ? "cpu" : "none",
                      object_getClassName(db), (unsigned long)db.length, dva ? "va" : db.contents ? "cpu" : "none");
            }
        } else if ([op[@"op"] isEqual:@"block"]) {    // CPU-staged blits
            ((void (^)(void))op[@"fn"])();
        } else if ([op[@"op"] isEqual:@"dispatch"]) {
            [self nvExecuteComputeOp:op];
        } else if ([op[@"op"] isEqual:@"draw"]) {
            [self nvExecuteDrawOp:op];
        } else if ([op[@"op"] isEqual:@"draw2"]) {
            [self nvExecuteDraw2Op:op];
        } else if ([op[@"op"] isEqual:@"draw3d"]) {
            nvExecuteDraw3D(op);
        } else if ([op[@"op"] isEqual:@"sig"]) {
            id ev = op[@"ev"];
            if ([ev isKindOfClass:objc_getClass("NVMTLSharedEvent")])
                [ev nvSignal:[op[@"val"] unsignedLongLongValue]];
            else NSLog(@"NVMTLDriver: signal on foreign event");
        } else if ([op[@"op"] isEqual:@"wait"]) {
            id ev = op[@"ev"];
            const uint64_t v = [op[@"val"] unsignedLongLongValue];
            if (![ev isKindOfClass:objc_getClass("NVMTLSharedEvent")]) {
                NSLog(@"NVMTLDriver: wait on foreign event"); continue;
            }
            // In-order commit: a same-CB signal already ran. Cross-thread
            // signals poll briefly; never hang forever (v2: GPU event wait).
            uint64_t spins = 0;
            while ([ev signaledValue] < v && spins++ < 2000000ull) { __asm__ volatile("pause"); }
            if ([ev signaledValue] < v)
                NSLog(@"NVMTLDriver: event wait timed out (want %llu)", v);
        }
    }
    // native N2: the batch rides in this command buffer to the family,
    // which completes it; commit returns without waiting
    if (native && nvGrFlushNative(self)) {
        nvGrBatchEndNative();
        // the GPU runs this after commit returns: keep every buffer,
        // texture and pipeline the ops name alive until the family says the
        // command buffer completed (an app may drop them right after commit;
        // freeing unbinds their GPU pages under running work)
        NSArray *keep = [_nvOps copy];
        [(id<MTLCommandBuffer>)self addCompletedHandler:^(id<MTLCommandBuffer> c) { (void)c; (void)keep; }];
    } else nvGrBatchEnd();
    [_nvOps removeAllObjects];
}
// Our GPU work runs first (synchronously), then the empty CB goes to the
// family, which drives status -> Completed (proven M3). 0.3.2: it used to be
// the other way round, and completed handlers on the family's thread could
// run before our work was done (metal_sync_test: 14/20 handlers saw stale
// buffers).
- (void)commit {
    static int trace = -1;
    if (trace < 0) trace = getenv("NVMTL_TRACE") != NULL;
    const uint64_t t0 = mach_absolute_time();
    [self nvExecuteOps];
    const uint64_t t1 = mach_absolute_time();
    [super commit];

    if (trace) {
        mach_timebase_info_data_t tb; mach_timebase_info(&tb);
        const uint64_t t2 = mach_absolute_time();
        NSLog(@"NVMTL_TRACE commit: our ops %.1f us, family %.1f us",
              (t1 - t0) * tb.numer / tb.denom / 1e3, (t2 - t1) * tb.numer / tb.denom / 1e3);
    }
}
@end

// general texture/buffer access for blits the 2D fast path can't
// take (block-linear, other levels/slices, 3D, private resources). Goes
// through CPU memory; runs at commit time in submission order.
static BOOL nvBufIO(id<MTLBuffer> b, BOOL write, NSUInteger off, NSUInteger len, void *cpu) {
    if (!len) return YES;
    nvGrSync();   // queued or in-flight GPU work first (the CPU looks at the memory)
    if (off + len > b.length) return NO;
    if (b.contents && !objc_getAssociatedObject(b, &kNVVramKey)) {   // managed: the GPU copy counts
        if (write) memcpy((uint8_t *)b.contents + off, cpu, len); else memcpy(cpu, (uint8_t *)b.contents + off, len);
        return YES;
    }
    const uint64_t va = nvBufVa(b, off, len);
    void *h = NULL;
    uint64_t hva = 0;
    if (!va || !nvHeapAlloc(len, &h, &hva)) return NO;
    if (write) memcpy(h, cpu, len);
    const BOOL ok = write ? nvCeCopy(va, hva, len) : nvCeCopy(hva, va, len);
    if (ok && !write) memcpy(cpu, h, len);
    nvHeapFree(h, len);
    return ok;
}
// copy-engine view of texel (x, y, z) at level/slice of a texture;
// NO when the texture has no GPU address
static BOOL nvTexSurf(NVMTLTexture *t, NSUInteger l, NSUInteger sl, NSUInteger x, NSUInteger y, NSUInteger z,
                      NVCeSurf *f) {
    memset(f, 0, sizeof *f);
    if (t.lay) {
        const NVTexLayout *L = t.lay.bytes;
        if (l >= L->levels || sl >= L->layers) return NO;
        f->bl = true; f->va = t.blVa + sl * L->arrayStride + L->offset[l];
        f->rowBytes = L->rowBytes[l]; f->rows = L->rows[l]; f->depth = L->depth[l];
        f->ylog = L->ylog[l]; f->zlog = L->zlog[l];
        f->xBytes = (uint32_t)(x * t.bpp); f->y = (uint32_t)y; f->z = (uint32_t)z;
        return f->va != 0;
    }
    if (l || sl || z) return NO;
    const uint64_t base = nvBufVa(t.buf, 0, t.pitch * t.h);
    if (!base) return NO;
    f->va = base + y * t.pitch + x * t.bpp; f->pitch = (uint32_t)t.pitch;
    return YES;
}
static BOOL nvBufSurf(id<MTLBuffer> b, NSUInteger off, NSUInteger bpr, NSUInteger bytes, NVCeSurf *f) {
    memset(f, 0, sizeof *f);
    if (off + bytes > b.length) return NO;
    f->va = nvBufVa(b, off, bytes); f->pitch = (uint32_t)bpr;
    return f->va != 0 && bpr < (1u << 31);
}

static BOOL nvTexIO(NVMTLTexture *t, BOOL write, NSUInteger l, NSUInteger sl, MTLRegion r, void *pix,
                    NSUInteger bpr, NSUInteger bpi) {
    if (t.lay) return [t nvBLCopy:write region:r level:l slice:sl bytes:pix bytesPerRow:bpr bytesPerImage:bpi];
    if (l || sl || r.origin.z || r.size.depth > 1 || r.origin.x + r.size.width > t.w || r.origin.y + r.size.height > t.h)
        return NO;
    const NSUInteger rb = r.size.width * t.bpp;
    for (NSUInteger y = 0; y < r.size.height; y++)
        if (!nvBufIO(t.buf, write, (r.origin.y + y) * t.pitch + r.origin.x * t.bpp, rb, (uint8_t *)pix + y * bpr))
            return NO;
    return YES;
}
static NSUInteger nvTexLevelDim(NSUInteger d, NSUInteger l) { return MAX(d >> l, (NSUInteger)1); }

// box filter of one texel quad/octet; kind 0 unorm8, 1 half, 2 float
static int nvMipKind(MTLPixelFormat f, uint32_t *comps) {
    switch (f) {
    case MTLPixelFormatR8Unorm: *comps = 1; return 0;
    case MTLPixelFormatRG8Unorm: *comps = 2; return 0;
    case MTLPixelFormatRGBA8Unorm: case MTLPixelFormatRGBA8Unorm_sRGB:
    case MTLPixelFormatBGRA8Unorm: case MTLPixelFormatBGRA8Unorm_sRGB: *comps = 4; return 0;
    case MTLPixelFormatR16Float: *comps = 1; return 1;
    case MTLPixelFormatRG16Float: *comps = 2; return 1;
    case MTLPixelFormatRGBA16Float: *comps = 4; return 1;
    case MTLPixelFormatR32Float: *comps = 1; return 2;
    case MTLPixelFormatRG32Float: *comps = 2; return 2;
    case MTLPixelFormatRGBA32Float: *comps = 4; return 2;
    default: return -1;
    }
}
static float nvMipGet(const void *p, int kind, NSUInteger i) {
    return kind == 0 ? ((const uint8_t *)p)[i] / 255.0f : kind == 1 ? (float)((const __fp16 *)p)[i] : ((const float *)p)[i];
}
static void nvMipPut(void *p, int kind, NSUInteger i, float v) {
    if (kind == 0) ((uint8_t *)p)[i] = (uint8_t)(v * 255.0f + 0.5f);
    else if (kind == 1) ((__fp16 *)p)[i] = (__fp16)v;
    else ((float *)p)[i] = v;
}
// multisample resolve: average the samples of every pixel (CPU,
// through the copy engine; box filter like the hardware's default resolve)
static void nvResolve(id msId, id dstId, NSUInteger level, NSUInteger slice) {
    NVMTLTexture *ms = msId, *dst = dstId;
    uint32_t comps = 0, sx = 1, sy = 1;
    const int kind = nvMipKind(ms.fmt, &comps);
    nvSampleLayout(ms.samples, &sx, &sy, NULL);
    if (kind < 0 || dst.bpp != ms.bpp) { NSLog(@"NVMTLDriver: resolve of format %lu not handled", (unsigned long)ms.fmt); return; }
    const NSUInteger w = ms.w, h = ms.h, bpp = ms.bpp, sw = w * sx, sh = h * sy;
    uint8_t *src = malloc(sw * sh * bpp), *out = malloc(w * h * bpp);
    if (src && out && nvTexIO(ms, NO, 0, 0, MTLRegionMake2D(0, 0, sw, sh), src, sw * bpp, sw * sh * bpp)) {
        for (NSUInteger y = 0; y < h; y++)
            for (NSUInteger x = 0; x < w; x++)
                for (uint32_t c = 0; c < comps; c++) {
                    float acc = 0;
                    for (uint32_t j = 0; j < sy; j++)
                        for (uint32_t i = 0; i < sx; i++)
                            acc += nvMipGet(src, kind, ((y * sy + j) * sw + x * sx + i) * comps + c);
                    nvMipPut(out, kind, (y * w + x) * comps + c, acc / (sx * sy));
                }
        const NSUInteger rw = MIN(w, nvTexLevelDim(dst.w, level)), rh = MIN(h, nvTexLevelDim(dst.h, level));
        if (!nvTexIO(dst, YES, level, slice, MTLRegionMake2D(0, 0, rw, rh), out, w * bpp, w * h * bpp))
            NSLog(@"NVMTLDriver: resolve write failed");
    } else NSLog(@"NVMTLDriver: resolve read failed");
    free(src); free(out);
}

static void nvGenerateMips(NVMTLTexture *t) {
    uint32_t comps = 0;
    const int kind = nvMipKind(t.fmt, &comps);
    const NSUInteger levels = t.mipmapLevelCount;
    if (levels < 2) return;
    if (kind < 0) { NSLog(@"NVMTLDriver: generateMipmaps: format %lu not handled", (unsigned long)t.fmt); return; }
    const bool is3D = t.textureType == MTLTextureType3D;
    const NSUInteger slices = is3D ? 1 : (t.lay ? ((const NVTexLayout *)t.lay.bytes)->layers : 1);
    const NSUInteger bpp = t.bpp;
    for (NSUInteger sl = 0; sl < slices; sl++) {
        NSUInteger w = t.w, h = t.h, d = is3D ? t.depth : 1;
        uint8_t *src = malloc(w * h * d * bpp);
        if (!src || !nvTexIO(t, NO, 0, sl, MTLRegionMake3D(0, 0, 0, w, h, d), src, w * bpp, w * h * bpp)) {
            free(src); NSLog(@"NVMTLDriver: generateMipmaps read failed"); return;
        }
        for (NSUInteger l = 1; l < levels; l++) {
            const NSUInteger nw = nvTexLevelDim(t.w, l), nh = nvTexLevelDim(t.h, l), nd = is3D ? nvTexLevelDim(t.depth, l) : 1;
            uint8_t *dst = malloc(nw * nh * nd * bpp);
            if (!dst) { free(src); return; }
            for (NSUInteger z = 0; z < nd; z++)
                for (NSUInteger y = 0; y < nh; y++)
                    for (NSUInteger x = 0; x < nw; x++)
                        for (uint32_t c = 0; c < comps; c++) {
                            float acc = 0; int n = 0;
                            for (NSUInteger dz = 0; dz < (d > 1 ? 2u : 1u); dz++)
                                for (NSUInteger dy = 0; dy < (h > 1 ? 2u : 1u); dy++)
                                    for (NSUInteger dx = 0; dx < (w > 1 ? 2u : 1u); dx++) {
                                        const NSUInteger sx = MIN(x * 2 + dx, w - 1), sy = MIN(y * 2 + dy, h - 1),
                                                         sz = MIN(z * 2 + dz, d - 1);
                                        acc += nvMipGet(src, kind, ((sz * h + sy) * w + sx) * comps + c); n++;
                                    }
                            nvMipPut(dst, kind, ((z * nh + y) * nw + x) * comps + c, acc / n);
                        }
            if (!nvTexIO(t, YES, l, sl, MTLRegionMake3D(0, 0, 0, nw, nh, nd), dst, nw * bpp, nw * nh * bpp))
                NSLog(@"NVMTLDriver: generateMipmaps write failed (level %lu)", (unsigned long)l);
            free(src); src = dst; w = nw; h = nh; d = nd;
        }
        free(src);
    }
}

// driver-internal GPU kernels (compiled once per process through
// our own AIR path): mipmap downsampling and multisample resolve
static NSString *const kNvInternalSrc =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "kernel void nv_mip2d(texture2d<float, access::read> s [[texture(0)]], texture2d<float, access::write> d [[texture(1)]],\n"
     "  constant uint4 &p [[buffer(0)]], uint2 g [[thread_position_in_grid]]) {\n"
     "  if (g.x >= p.x || g.y >= p.y) return;\n"
     "  uint2 m = uint2(p.z - 1, p.w - 1), a = g * 2;\n"
     "  float4 c = s.read(min(a, m)) + s.read(min(a + uint2(1, 0), m)) + s.read(min(a + uint2(0, 1), m)) + s.read(min(a + 1, m));\n"
     "  d.write(c * 0.25f, g); }\n"
     "kernel void nv_mip3d(texture3d<float, access::read> s [[texture(0)]], texture3d<float, access::write> d [[texture(1)]],\n"
     "  constant uint4 *p [[buffer(0)]], uint3 g [[thread_position_in_grid]]) {\n"
     "  uint3 dd = p[0].xyz, sd = p[1].xyz;\n"
     "  if (g.x >= dd.x || g.y >= dd.y || g.z >= dd.z) return;\n"
     "  uint3 m = sd - 1, a = g * 2; float4 c = 0;\n"
     "  for (uint k = 0; k < 8; k++) c += s.read(min(a + uint3(k & 1, (k >> 1) & 1, k >> 2), m));\n"
     "  d.write(c * 0.125f, g); }\n"
     "static float4 nv_srgb(float4 c) {\n"
     "  float3 l = saturate(c.rgb);\n"
     "  float3 e = select(1.055f * pow(l, 1.0f / 2.4f) - 0.055f, l * 12.92f, l <= 0.0031308f);\n"
     "  return float4(e, c.a); }\n"
     // sRGB: the source is read through the sRGB format (linear values), the
     // destination is its UNORM view, so the kernel encodes
     "kernel void nv_mip2d_srgb(texture2d<float, access::read> s [[texture(0)]], texture2d<float, access::write> d [[texture(1)]],\n"
     "  constant uint4 &p [[buffer(0)]], uint2 g [[thread_position_in_grid]]) {\n"
     "  if (g.x >= p.x || g.y >= p.y) return;\n"
     "  uint2 m = uint2(p.z - 1, p.w - 1), a = g * 2;\n"
     "  float4 c = s.read(min(a, m)) + s.read(min(a + uint2(1, 0), m)) + s.read(min(a + uint2(0, 1), m)) + s.read(min(a + 1, m));\n"
     "  d.write(nv_srgb(c * 0.25f), g); }\n"
     "kernel void nv_resolve_srgb(texture2d<float, access::read> s [[texture(0)]], texture2d<float, access::write> d [[texture(1)]],\n"
     "  constant uint4 &p [[buffer(0)]], uint2 g [[thread_position_in_grid]]) {\n"
     "  if (g.x >= p.x || g.y >= p.y) return;\n"
     "  float4 c = 0;\n"
     "  for (uint j = 0; j < p.w; j++) for (uint i = 0; i < p.z; i++) c += s.read(uint2(g.x * p.z + i, g.y * p.w + j));\n"
     "  d.write(nv_srgb(c / float(p.z * p.w)), g); }\n"
     "kernel void nv_resolve(texture2d<float, access::read> s [[texture(0)]], texture2d<float, access::write> d [[texture(1)]],\n"
     "  constant uint4 &p [[buffer(0)]], uint2 g [[thread_position_in_grid]]) {\n"
     "  if (g.x >= p.x || g.y >= p.y) return;\n"
     "  float4 c = 0;\n"
     "  for (uint j = 0; j < p.w; j++) for (uint i = 0; i < p.z; i++) c += s.read(uint2(g.x * p.z + i, g.y * p.w + j));\n"
     "  d.write(c / float(p.z * p.w), g); }\n";
static NVMTLComputePipelineState *nvInternalPS(id<MTLDevice> dev, NSString *name) {
    static NSMutableDictionary *cache;
    static id<MTLLibrary> lib;
    static os_unfair_lock lk = OS_UNFAIR_LOCK_INIT;
    os_unfair_lock_lock(&lk);
    if (!cache) cache = [NSMutableDictionary new];
    NVMTLComputePipelineState *ps = cache[name];
    if (!ps) {
        if (!lib) {
            NSError *e = nil;
            lib = [dev newLibraryWithSource:kNvInternalSrc options:nil error:&e];
            if (!lib) NSLog(@"NVMTLDriver: internal kernels: %@", e);
        }
        id<MTLFunction> fn = [lib newFunctionWithName:name];
        ps = fn ? [dev newComputePipelineStateWithFunction:fn error:nil] : nil;
        if (ps) cache[name] = ps;
    }
    os_unfair_lock_unlock(&lk);
    return ps;
}
// formats the float kernels can read and write; sRGB ones go through their
// UNORM twin on the write side (image stores don't encode sRGB)
static MTLPixelFormat nvLinearTwin(MTLPixelFormat f) {
    return f == MTLPixelFormatRGBA8Unorm_sRGB ? MTLPixelFormatRGBA8Unorm
         : f == MTLPixelFormatBGRA8Unorm_sRGB ? MTLPixelFormatBGRA8Unorm : f;
}
static bool nvGpuFilterable(MTLPixelFormat f) {
    uint32_t comps = 0;
    return nvMipKind(f, &comps) >= 0;
}
static NSArray *nvTexSlots(id a, id b) {
    NSMutableArray *t = [NSMutableArray new];
    for (int i = 0; i < 16; i++) [t addObject:[NSNull null]];
    t[0] = a; t[1] = b;
    return t;
}
static NSArray *nvBufSlot0(id<MTLDevice> dev, const void *p, NSUInteger n) {
    NSMutableArray *b = [NSMutableArray new];
    for (int i = 0; i < 16; i++) [b addObject:[NSNull null]];
    b[0] = @{@"buf": [dev newBufferWithBytes:p length:n options:MTLResourceStorageModeShared], @"off": @0};
    return b;
}
// queue the GPU mip chain; NO when the texture needs the CPU path
static BOOL nvGpuMips(NVMTLCommandBuffer *cb, NVMTLTexture *t) {
    if (!t.lay || t.mipmapLevelCount < 2 || !nvGpuFilterable(t.fmt) || t.samples > 1 || getenv("NVMTL_CPU_MIPS")) return NO;
    id<MTLDevice> dev = [(id<MTLCommandBuffer>)cb device];
    const bool is3D = t.textureType == MTLTextureType3D;
    const bool srgb = nvLinearTwin(t.fmt) != t.fmt;
    if (srgb && is3D) return NO;
    NVMTLComputePipelineState *ps = nvInternalPS(dev, is3D ? @"nv_mip3d" : srgb ? @"nv_mip2d_srgb" : @"nv_mip2d");
    if (!ps) return NO;
    const NVTexLayout *L = t.lay.bytes;
    NSMutableArray *views = [NSMutableArray new];
    for (NSUInteger sl = 0; sl < (is3D ? 1 : L->layers); sl++)
        for (NSUInteger l = 1; l < L->levels; l++) {
            const MTLTextureType vt = is3D ? MTLTextureType3D : MTLTextureType2D;
            NVMTLTexture *src = [t newTextureViewWithPixelFormat:t.fmt textureType:vt levels:NSMakeRange(l - 1, 1)
                                                          slices:NSMakeRange(sl, 1)];
            NVMTLTexture *dst = [t newTextureViewWithPixelFormat:nvLinearTwin(t.fmt) textureType:vt levels:NSMakeRange(l, 1)
                                                          slices:NSMakeRange(sl, 1)];
            if (!src || !dst) return NO;
            [views addObject:@[src, dst]];
        }
    for (NSArray *sd in views) {
        NVMTLTexture *src = sd[0], *dst = sd[1];
        if (is3D) {
            const uint32_t p[8] = {(uint32_t)dst.w, (uint32_t)dst.h, (uint32_t)dst.depth, 0,
                                   (uint32_t)src.w, (uint32_t)src.h, (uint32_t)src.depth, 0};
            [cb nvAddDispatch:ps bufs:nvBufSlot0(dev, p, sizeof p) texs:nvTexSlots(src, dst) samps:@[]
                         grid:MTLSizeMake(dst.w, dst.h, dst.depth) block:MTLSizeMake(4, 4, 4)];
        } else {
            const uint32_t p[4] = {(uint32_t)dst.w, (uint32_t)dst.h, (uint32_t)src.w, (uint32_t)src.h};
            [cb nvAddDispatch:ps bufs:nvBufSlot0(dev, p, sizeof p) texs:nvTexSlots(src, dst) samps:@[]
                         grid:MTLSizeMake(dst.w, dst.h, 1) block:MTLSizeMake(16, 16, 1)];
        }
    }
    return YES;
}
// queue a GPU resolve; NO when it needs the CPU path
static BOOL nvGpuResolve(id cbId, id msId, id dstId, NSUInteger level, NSUInteger slice) {
    NVMTLCommandBuffer *cb = cbId;
    NVMTLTexture *ms = msId, *dst = dstId;
    if (!nvGpuFilterable(ms.fmt) || !nvGpuFilterable(dst.fmt) || getenv("NVMTL_CPU_RESOLVE")) return NO;
    id<MTLDevice> dev = [(id<MTLCommandBuffer>)cb device];
    const bool srgb = nvLinearTwin(dst.fmt) != dst.fmt;
    NVMTLComputePipelineState *ps = nvInternalPS(dev, srgb ? @"nv_resolve_srgb" : @"nv_resolve");
    NVMTLTexture *grid = [ms nvSampleGridView];
    NVMTLTexture *out = dst;
    if (dst.lay) out = [dst newTextureViewWithPixelFormat:nvLinearTwin(dst.fmt) textureType:MTLTextureType2D
                                                   levels:NSMakeRange(level, 1) slices:NSMakeRange(slice, 1)];
    else if (level || slice) return NO;
    else if (srgb) out = [dst newTextureViewWithPixelFormat:nvLinearTwin(dst.fmt)];
    if (!ps || !grid || !out) return NO;
    uint32_t sx = 1, sy = 1;
    nvSampleLayout(ms.samples, &sx, &sy, NULL);
    const uint32_t w = (uint32_t)MIN(ms.w, out.w), h = (uint32_t)MIN(ms.h, out.h);
    const uint32_t p[4] = {w, h, sx, sy};
    [cb nvAddDispatch:ps bufs:nvBufSlot0(dev, p, sizeof p) texs:nvTexSlots(grid, out) samps:@[]
                 grid:MTLSizeMake(w, h, 1) block:MTLSizeMake(16, 16, 1)];
    return YES;
}

@interface NVMTLBlitEncoder : MTLIOAccelBlitCommandEncoder
- (void)nvSetCB:(id)cb;
@end
@implementation NVMTLBlitEncoder {
    NVMTLCommandBuffer *_nvCb;
}
static const char *const kNvBlitStubs[] = {
    "copyIndirectCommandBuffer:sourceRange:destination:destinationIndex:",
    "optimizeIndirectCommandBuffer:withRange:",
    "resolveCounters:inRange:destinationBuffer:destinationOffset:",
    "sampleCountersInBuffer:atSampleIndex:withBarrier:",
    "resetCommandsInBuffer:withRange:",
    NULL};
+ (void)initialize {
    if (self != [NVMTLBlitEncoder class]) return;
    nvInstallFallback(self);
    nvForwardStubs(self, @protocol(MTLBlitCommandEncoder), kNvBlitStubs);
}
+ (void)load { class_addProtocol(self, @protocol(MTLBlitCommandEncoder)); }
- (void)nvSetCB:(id)cb { _nvCb = cb; }
- (void)fillBuffer:(id)buffer range:(NSRange)range value:(uint8_t)value {
    [_nvCb nvAddFill:buffer range:range value:value];
}
- (void)copyFromBuffer:(id)src sourceOffset:(NSUInteger)so
              toBuffer:(id)dst destinationOffset:(NSUInteger)do_ size:(NSUInteger)n {
    [_nvCb nvAddCopy:src so:so dst:dst do:do_ n:n];
}
// texture copies (2D, level 0, slice 0: what our textures are)
- (void)nvCopy2DFrom:(id<MTLBuffer>)sb so:(NSUInteger)so sp:(NSUInteger)sp
                  to:(id<MTLBuffer>)db do:(NSUInteger)do_ dp:(NSUInteger)dp rb:(NSUInteger)rb rows:(NSUInteger)rows {
    if (!sb || !db || !rb || !rows) return;
    [(NVMTLCommandBuffer *)_nvCb nvAddOp:@{@"op": @"copy2d", @"src": sb, @"so": @(so), @"sp": @(sp),
                                           @"dst": db, @"do": @(do_), @"dp": @(dp), @"rb": @(rb), @"rows": @(rows)}];
}
- (void)copyFromBuffer:(id)src sourceOffset:(NSUInteger)so sourceBytesPerRow:(NSUInteger)sbpr
    sourceBytesPerImage:(NSUInteger)sbpi sourceSize:(MTLSize)size toTexture:(id)dstTex
       destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)o {
    NVMTLTexture *t = dstTex;
    if (![t isKindOfClass:objc_getClass("NVMTLTexture")]) { NSLog(@"NVMTLDriver: buffer->texture copy unsupported"); return; }
    if (t.lay || ds || dl || size.depth > 1) {
        id<MTLBuffer> b = src;
        const NSUInteger bpi = sbpi ? sbpi : sbpr * size.height;
        const NSUInteger n = MIN(bpi * MAX(size.depth, (NSUInteger)1), b.length > so ? b.length - so : 0);
        [(NVMTLCommandBuffer *)_nvCb nvAddOp:@{@"op": @"block", @"fn": [^{
            // straight on the copy engine, one depth slice at a time
            BOOL gpu = YES;
            for (NSUInteger z = 0; gpu && z < MAX(size.depth, (NSUInteger)1); z++) {
                NVCeSurf a, d;
                gpu = nvBufSurf(b, so + z * bpi, sbpr, sbpr * (size.height - 1) + size.width * t.bpp, &a) &&
                      nvTexSurf(t, dl, ds, o.x, o.y, o.z + z, &d) &&
                      nvCeCopySurf(&a, &d, (uint32_t)(size.width * t.bpp), (uint32_t)size.height);
            }
            if (gpu) return;
            void *tmp = malloc(n);
            if (!tmp || !nvBufIO(b, NO, so, n, tmp) ||
                !nvTexIO(t, YES, dl, ds, (MTLRegion){o, size}, tmp, sbpr, bpi))
                NSLog(@"NVMTLDriver: buffer->texture copy failed");
            free(tmp);
        } copy]}];
        return;
    }
    if (o.x + size.width > t.w || o.y + size.height > t.h) { NSLog(@"NVMTLDriver: buffer->texture copy unsupported"); return; }
    [self nvCopy2DFrom:src so:so sp:sbpr to:t.buf do:o.y * t.pitch + o.x * t.bpp dp:t.pitch
                    rb:size.width * t.bpp rows:size.height];
}
// the options forms (depth/stencil plane selection, PVRTC rows);
// our formats have none of those, so they are the plain copies
- (void)copyFromBuffer:(id)src sourceOffset:(NSUInteger)so sourceBytesPerRow:(NSUInteger)sbpr
    sourceBytesPerImage:(NSUInteger)sbpi sourceSize:(MTLSize)size toTexture:(id)dstTex
       destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)o
                options:(MTLBlitOption)opt {
    (void)opt;
    [self copyFromBuffer:src sourceOffset:so sourceBytesPerRow:sbpr sourceBytesPerImage:sbpi sourceSize:size
               toTexture:dstTex destinationSlice:ds destinationLevel:dl destinationOrigin:o];
}
- (void)copyFromTexture:(id)srcTex sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl
           sourceOrigin:(MTLOrigin)o sourceSize:(MTLSize)size toBuffer:(id)dst
      destinationOffset:(NSUInteger)doff destinationBytesPerRow:(NSUInteger)dbpr
    destinationBytesPerImage:(NSUInteger)dbpi options:(MTLBlitOption)opt {
    (void)opt;
    [self copyFromTexture:srcTex sourceSlice:ss sourceLevel:sl sourceOrigin:o sourceSize:size toBuffer:dst
        destinationOffset:doff destinationBytesPerRow:dbpr destinationBytesPerImage:dbpi];
}
- (void)copyFromTexture:(id)srcTex sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl
           sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)size toTexture:(id)dstTex
       destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dO
                options:(MTLBlitOption)opt {
    (void)opt;
    [self copyFromTexture:srcTex sourceSlice:ss sourceLevel:sl sourceOrigin:so sourceSize:size toTexture:dstTex
         destinationSlice:ds destinationLevel:dl destinationOrigin:dO];
}
- (void)copyFromTexture:(id)srcTex sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl
           sourceOrigin:(MTLOrigin)o sourceSize:(MTLSize)size toBuffer:(id)dst
      destinationOffset:(NSUInteger)doff destinationBytesPerRow:(NSUInteger)dbpr
    destinationBytesPerImage:(NSUInteger)dbpi {
    NVMTLTexture *t = srcTex;
    if (![t isKindOfClass:objc_getClass("NVMTLTexture")]) { NSLog(@"NVMTLDriver: texture->buffer copy unsupported"); return; }
    if (t.lay || ss || sl || size.depth > 1) {
        id<MTLBuffer> b = dst;
        const NSUInteger bpi = dbpi ? dbpi : dbpr * size.height;
        const NSUInteger n = MIN(bpi * MAX(size.depth, (NSUInteger)1), b.length > doff ? b.length - doff : 0);
        [(NVMTLCommandBuffer *)_nvCb nvAddOp:@{@"op": @"block", @"fn": [^{
            BOOL gpu = YES;                      // copy engine first
            for (NSUInteger z = 0; gpu && z < MAX(size.depth, (NSUInteger)1); z++) {
                NVCeSurf a, d;
                gpu = nvTexSurf(t, sl, ss, o.x, o.y, o.z + z, &a) &&
                      nvBufSurf(b, doff + z * bpi, dbpr, dbpr * (size.height - 1) + size.width * t.bpp, &d) &&
                      nvCeCopySurf(&a, &d, (uint32_t)(size.width * t.bpp), (uint32_t)size.height);
            }
            if (gpu) return;
            void *tmp = calloc(1, n);
            if (!tmp || !nvBufIO(b, NO, doff, n, tmp) ||      // keep the gaps between rows
                !nvTexIO(t, NO, sl, ss, (MTLRegion){o, size}, tmp, dbpr, bpi) || !nvBufIO(b, YES, doff, n, tmp))
                NSLog(@"NVMTLDriver: texture->buffer copy failed");
            free(tmp);
        } copy]}];
        return;
    }
    if (o.x + size.width > t.w || o.y + size.height > t.h) { NSLog(@"NVMTLDriver: texture->buffer copy unsupported"); return; }
    [self nvCopy2DFrom:t.buf so:o.y * t.pitch + o.x * t.bpp sp:t.pitch to:dst do:doff dp:dbpr
                    rb:size.width * t.bpp rows:size.height];
}
- (void)copyFromTexture:(id)srcTex sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl
           sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)size toTexture:(id)dstTex
       destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dO {
    NVMTLTexture *a = srcTex, *b = dstTex;
    if ([a isKindOfClass:objc_getClass("NVMTLTexture")] && [b isKindOfClass:objc_getClass("NVMTLTexture")] &&
        a.bpp == b.bpp && (a.lay || b.lay || ss || sl || ds || dl || size.depth > 1)) {
        const NSUInteger bpr = size.width * a.bpp, bpi = bpr * size.height, n = bpi * MAX(size.depth, (NSUInteger)1);
        [(NVMTLCommandBuffer *)_nvCb nvAddOp:@{@"op": @"block", @"fn": [^{
            BOOL gpu = YES;                      // copy engine first (BL <-> BL too)
            for (NSUInteger z = 0; gpu && z < MAX(size.depth, (NSUInteger)1); z++) {
                NVCeSurf x, y;
                gpu = nvTexSurf(a, sl, ss, so.x, so.y, so.z + z, &x) && nvTexSurf(b, dl, ds, dO.x, dO.y, dO.z + z, &y) &&
                      nvCeCopySurf(&x, &y, (uint32_t)(size.width * a.bpp), (uint32_t)size.height);
            }
            if (gpu) return;
            void *tmp = malloc(n);
            if (!tmp || !nvTexIO(a, NO, sl, ss, (MTLRegion){so, size}, tmp, bpr, bpi) ||
                !nvTexIO(b, YES, dl, ds, (MTLRegion){dO, size}, tmp, bpr, bpi))
                NSLog(@"NVMTLDriver: texture->texture copy failed");
            free(tmp);
        } copy]}];
        return;
    }
    if (![a isKindOfClass:objc_getClass("NVMTLTexture")] || ![b isKindOfClass:objc_getClass("NVMTLTexture")] ||
        ss || sl || ds || dl || size.depth > 1 || a.bpp != b.bpp ||
        so.x + size.width > a.w || so.y + size.height > a.h || dO.x + size.width > b.w || dO.y + size.height > b.h) {
        NSLog(@"NVMTLDriver: texture->texture copy unsupported"); return;
    }
    [self nvCopy2DFrom:a.buf so:so.y * a.pitch + so.x * a.bpp sp:a.pitch to:b.buf
                    do:dO.y * b.pitch + dO.x * b.bpp dp:b.pitch rb:size.width * a.bpp rows:size.height];
}
- (void)copyFromTexture:(id)srcTex toTexture:(id)dstTex {
    NVMTLTexture *a = srcTex;
    const NSUInteger slices = a.lay ? ((const NVTexLayout *)a.lay.bytes)->layers : 1;
    for (NSUInteger sl = 0; sl < slices; sl++)
        for (NSUInteger l = 0; l < a.mipmapLevelCount; l++)
            [self copyFromTexture:srcTex sourceSlice:sl sourceLevel:l sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(nvTexLevelDim(a.w, l), nvTexLevelDim(a.h, l), nvTexLevelDim(a.depth, l))
                        toTexture:dstTex destinationSlice:sl destinationLevel:l destinationOrigin:MTLOriginMake(0, 0, 0)];
}
- (void)copyFromTexture:(id)srcTex sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl toTexture:(id)dstTex
       destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl sliceCount:(NSUInteger)sc levelCount:(NSUInteger)lc {
    NVMTLTexture *a = srcTex;
    for (NSUInteger i = 0; i < sc; i++)
        for (NSUInteger j = 0; j < lc; j++) {
            const NSUInteger l = sl + j;
            [self copyFromTexture:srcTex sourceSlice:ss + i sourceLevel:l sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(nvTexLevelDim(a.w, l), nvTexLevelDim(a.h, l), nvTexLevelDim(a.depth, l))
                        toTexture:dstTex destinationSlice:ds + i destinationLevel:dl + j
                destinationOrigin:MTLOriginMake(0, 0, 0)];
        }
}
// Shared and managed resources are CPU-coherent host memory here; one level only.
- (void)synchronizeResource:(id)r {
    // managed buffer: GPU copy -> CPU copy, in command order
    NVManaged *m = objc_getAssociatedObject(r, &kNVManagedKey);
    if (!m) return;
    [(NVMTLCommandBuffer *)_nvCb nvAddOp:@{@"op": @"block", @"fn": [^{
        if (!nvCeCopy(m.heapVa, m.vramVa, m.len)) NSLog(@"NVMTLDriver: managed download failed");
    } copy]}];
}
- (void)synchronizeTexture:(id)t slice:(NSUInteger)s level:(NSUInteger)l { (void)t; (void)s; (void)l; }
- (void)generateMipmapsForTexture:(id)t {
    if (![t isKindOfClass:objc_getClass("NVMTLTexture")]) return;
    NVMTLTexture *tex = t;
    if (nvGpuMips((NVMTLCommandBuffer *)_nvCb, tex)) return;     // on the GPU
    [(NVMTLCommandBuffer *)_nvCb nvAddOp:@{@"op": @"block", @"fn": [^{ nvGenerateMips(tex); } copy]}];
}
- (void)waitForFence:(id)f { (void)f; } // synchronous: already complete
- (void)updateFence:(id)f { (void)f; }
// Super unregisters the encoder with the CB (without it, commit asserts
// "commit command buffer with uncommitted encoder", live 27 Sep).
- (void)endEncoding { [super endEncoding]; _nvCb = nil; }
@end

@interface NVMTLCommandQueue : MTLIOAccelCommandQueue
@end
@implementation NVMTLCommandQueue
+ (void)initialize { if (self == [NVMTLCommandQueue class]) nvInstallFallback(self); }
// MTLIOAccelCommandQueue has no commandBuffer (Paravirt overrides both).
- (id)commandBuffer {
    return [[NVMTLCommandBuffer alloc] initWithQueue:self retainedReferences:YES];
}
- (id)commandBufferWithUnretainedReferences {
    return [[NVMTLCommandBuffer alloc] initWithQueue:self retainedReferences:NO];
}
@end

// ---------------------------------------------------------------- buffer VAs
// Private (VRAM) buffers carry their GPU VA in an associated holder that
// gives the range back when the MTLBuffer goes away. Shared buffers are
// found through the zero-copy heap by their CPU address.
@interface NVVramHolder : NSObject
@property (nonatomic) uint64_t va, len;
@end
@implementation NVVramHolder
- (void)dealloc { if (_va) nvVramFree(_va, _len); }
@end
static char kNVVramKey;

static uint64_t nvBufVa(id<MTLBuffer> b, NSUInteger off, NSUInteger len) {
    NVVramHolder *h = objc_getAssociatedObject(b, &kNVVramKey);
    if (h) return off <= h.len && len <= h.len - off ? h.va + off : 0;
    return nvBufferMappedVa(b, off, len, nvHeapVa);
}

// ---------------------------------------------------------------- compute blits
// One copy engine tops out near 45 GB/s; VRAM<->VRAM fills and copies of
// 1 MiB+ run as uvec4 compute kernels instead (grid-stride, 76 SMs x 16
// groups of 256), which is how CUDA gets ~300 GB/s D2D on this card.
#define NV_VRAM_LO 0x2A00000000ULL
#define NV_VRAM_HI 0x2C00000000ULL
static bool nvIsVram(uint64_t va, uint64_t n) { return va >= NV_VRAM_LO && va + n <= NV_VRAM_HI; }
static NVMTLKernel *nvBlitKernel(BOOL fill) {
    static NVMTLKernel *k[2];
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSString *hdr = @"#version 460\n#extension GL_EXT_buffer_reference : require\n"
                        @"#extension GL_EXT_scalar_block_layout : require\n"
                        @"#extension GL_EXT_shader_explicit_arithmetic_types : require\n"
                        @"layout(buffer_reference, scalar) buffer Q { uvec4 q[]; };\n"
                        @"layout(local_size_x=256) in;\n";
        k[1] = nvCompileGLSL(@"nv_blit_fill", [hdr stringByAppendingString:
            @"layout(push_constant, scalar) uniform PC { uint64_t dst; uvec4 v; uint n16; uint stride; } pc;\n"
            @"void main() { Q d = Q(pc.dst);\n"
            @"  for (uint i = gl_GlobalInvocationID.x; i < pc.n16; i += pc.stride) d.q[i] = pc.v; }\n"]);
        k[0] = nvCompileGLSL(@"nv_blit_copy", [hdr stringByAppendingString:
            @"layout(push_constant, scalar) uniform PC { uint64_t dst; uint64_t src; uint n16; uint stride; } pc;\n"
            @"void main() { Q d = Q(pc.dst); Q s = Q(pc.src);\n"
            @"  for (uint i = gl_GlobalInvocationID.x; i < pc.n16; i += pc.stride) d.q[i] = s.q[i]; }\n"]);
    });
    return k[fill ? 1 : 0];
}
static bool nvComputeBlit(BOOL fill, uint64_t dst, uint64_t src, uint64_t bytes, uint32_t value) {
    if (bytes < (1u << 20) || (dst & 15) || (!fill && (src & 15)) || bytes / 16 > 0xffffffffULL) return false;
    if (!nvIsVram(dst, bytes) || (!fill && !nvIsVram(src, bytes))) return false;
    NVMTLKernel *k = nvBlitKernel(fill);
    if (!k || !k.code.length) return false;
    const uint64_t n16 = bytes / 16;
    uint32_t push[8] = {0};
    uint32_t npush;
    push[0] = (uint32_t)dst; push[1] = (uint32_t)(dst >> 32);
    // NAK here can't lower gl_NumWorkGroups (NVK feeds it from a root cbuf), so the stride rides in push.
    const uint32_t grid[3] = {76 * 16, 1, 1}, block[3] = {256, 1, 1};
    const uint32_t stride = grid[0] * block[0];
    if (fill) { push[2] = push[3] = push[4] = push[5] = value; push[6] = (uint32_t)n16; push[7] = stride; npush = 8; }
    else { push[2] = (uint32_t)src; push[3] = (uint32_t)(src >> 32); push[4] = (uint32_t)n16; push[5] = stride; npush = 6; }
    if (!nvGrQueue(k.code.bytes, (uint32_t)(k.code.length / 4), k.regs, k.slm, k.smem, k.barriers, push, npush, grid, block))
        return false;
    const uint64_t done = n16 * 16;
    if (done == bytes) return true;
    return fill ? nvCeFill(dst + done, bytes - done, value) : nvCeCopy(dst + done, src + done, bytes - done);
}

// pitched 2D copy (texture uploads/downloads/copies through the blit
// encoder; private textures live in VRAM and have no CPU view). Words when
// everything is 4-byte aligned, else one CE copy per row.
static NVMTLKernel *nvCopy2DKernel(void) {
    static NVMTLKernel *k;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        k = nvCompileGLSL(@"nv_copy2d", @"#version 460\n#extension GL_EXT_buffer_reference : require\n"
            @"#extension GL_EXT_scalar_block_layout : require\n"
            @"#extension GL_EXT_shader_explicit_arithmetic_types : require\n"
            @"layout(buffer_reference, scalar) buffer W { uint w[]; };\n"
            @"layout(local_size_x=256) in;\n"
            @"layout(push_constant, scalar) uniform PC { uint64_t dst; uint64_t src; uint dpw; uint spw;"
            @" uint rw; uint rows; uint stride; } pc;\n"
            @"void main() { W d = W(pc.dst); W s = W(pc.src); uint n = pc.rw * pc.rows;\n"
            @"  for (uint i = gl_GlobalInvocationID.x; i < n; i += pc.stride) {\n"
            @"    uint y = i / pc.rw, x = i - y * pc.rw; d.w[y * pc.dpw + x] = s.w[y * pc.spw + x]; } }\n");
    });
    return k;
}
static bool nvCopy2D(uint64_t dst, uint64_t dpitch, uint64_t src, uint64_t spitch,
                     uint64_t rowBytes, uint64_t rows) {
    if (!rowBytes || !rows) return true;
    NVMTLKernel *k = nvCopy2DKernel();
    if (k && k.code.length && !((dst | src | dpitch | spitch | rowBytes) & 3) &&
        rowBytes / 4 * rows <= 0xffffffffULL) {
        uint32_t push[9] = {(uint32_t)dst, (uint32_t)(dst >> 32), (uint32_t)src, (uint32_t)(src >> 32),
                            (uint32_t)(dpitch / 4), (uint32_t)(spitch / 4), (uint32_t)(rowBytes / 4),
                            (uint32_t)rows, 0};
        const uint64_t words = rowBytes / 4 * rows;
        uint32_t groups = (uint32_t)((words + 255) / 256);
        if (groups > 76 * 16) groups = 76 * 16;
        const uint32_t grid[3] = {groups, 1, 1}, block[3] = {256, 1, 1};
        push[8] = groups * 256;
        return nvGrQueue(k.code.bytes, (uint32_t)(k.code.length / 4), k.regs, k.slm, k.smem, k.barriers,
                         push, 9, grid, block);
    }
    for (uint64_t y = 0; y < rows; y++)
        if (!nvCeCopy(dst + y * dpitch, src + y * spitch, rowBytes)) return false;
    return true;
}

// IOAccelNewResourceArgs as AppleParavirtBuffer passes it: 0x80 bytes; the
// family fills the generic IOAccelNewResourceData, the tail (0x68..) is the
// vendor's (our kernel NVAccelResource reads it). Paravirt: +0x68 = 1 (kind),
// +0x70 = length, +0x78 = storage mode is not shared.
typedef struct { uint8_t b[0x80]; } NVResourceArgs;

static id nvNewBuffer(id dev, void *ptr, NSUInteger len, NSUInteger options, id dealloc) {
    const uint64_t page = vm_page_size;
    NVResourceArgs a = {0};
    *(uint32_t *)(a.b + 0x68) = 1;
    *(uint64_t *)(a.b + 0x70) = len;
    *(uint32_t *)(a.b + 0x78) = (options & 0xf0) != 0;
    Class cls = objc_getClass("MTLIOAccelBuffer");
    id buf = [[cls alloc] initWithDevice:dev pointer:ptr length:len options:options
                              sysMemSize:(len + page - 1) & ~(page - 1) vidMemSize:0
                                    args:&a argsSize:sizeof(a) deallocator:dealloc];
    if (!buf) NSLog(@"NVMTLDriver: buffer %lu opts 0x%lx failed", (unsigned long)len, (unsigned long)options);
    return buf;
}

static id nvBufferNewTexture(id buf, SEL _cmd, MTLTextureDescriptor *td, NSUInteger off, NSUInteger bpr) NS_RETURNS_RETAINED;
// a buffer that starts `skew` bytes into another one (a surface plane
// off a page boundary). contents/length are its own, so nvBufVa finds the
// right GPU address through the wrapped pages; the rest goes to the buffer.
@interface NVPlaneBuffer : NSObject
- (instancetype)initWithBuffer:(id<MTLBuffer>)b skew:(NSUInteger)skew;
@end
@implementation NVPlaneBuffer { id<MTLBuffer> _b; NSUInteger _skew; }
+ (void)load { class_addProtocol(self, @protocol(MTLBuffer)); }
- (instancetype)initWithBuffer:(id<MTLBuffer>)b skew:(NSUInteger)skew { if ((self = [super init])) { _b = b; _skew = skew; } return self; }
- (void *)contents { return (uint8_t *)_b.contents + _skew; }
- (NSUInteger)length { return _b.length - _skew; }
- (uint64_t)gpuAddress { return nvBufVa((id<MTLBuffer>)self, 0, self.length); }
- (id)forwardingTargetForSelector:(SEL)sel { (void)sel; return _b; }
- (BOOL)respondsToSelector:(SEL)sel { return [super respondsToSelector:sel] || [(NSObject *)_b respondsToSelector:sel]; }
@end

@implementation NVMTLDevice {
    NSMutableDictionary *_nvKernels; // name -> NVMTLKernel (M4 v1)
    NSMutableDictionary *_nvFrags;   // name -> NVMTLFragment (M6 v1)
    NSMutableDictionary *_nvAirLibs; // function name -> cached metallib path (28 Sep)
    NSMutableDictionary *_nvSubset;  // MetalSL subset kernels (fallback)
    NSMutableArray *_nvSubsetSrc;    // sources not run through the subset yet
}
+ (void)initialize {
    if (self != [NVMTLDevice class]) return;
    nvInstallFallback(self);
    Class b = NSClassFromString(@"MTLIOAccelBuffer");
    if (b) class_addMethod(b, @selector(newTextureWithDescriptor:offset:bytesPerRow:), (IMP)nvBufferNewTexture, "@@:@QQ");
    // SkyLight's MetalTiledBacking (the IOPresentment path) asks for a
    // "tiled" texture on a buffer; the layout is ours to pick, linear it is
    if (b) class_addMethod(b, @selector(newTiledTextureWithDescriptor:offset:bytesPerRow:), (IMP)nvBufferNewTexture, "@@:@QQ");
    // and the Apple buffer class gets the same net as our own objects
    if (b) nvInstallFallback(b);
}

// MTLIOAccelBuffer's gpuAddress is the IOAccel resource address, not
// where our kernels see the buffer. Argument buffers store gpuAddress, so it
// has to be the address our channel uses.
static uint64_t nvBufferGpuAddress(id self, SEL _cmd) {
    (void)_cmd;
    const uint64_t va = nvBufVa(self, 0, [(id<MTLBuffer>)self length]);
    if (va) return va;
    // argument buffers carry this; 0 sent a shader to VA 0
    nvNoteNoVa(self, 0, nil);
    return nvNullVa();
}

// MTLAddDevice asserts [device conformsToProtocol:@protocol(MTLDevice)]; the
// conformance is declared by each vendor subclass (AppleParavirtDevice
// <MTLDevice>), not by MTLIOAccelDevice. Declaring it in the @interface would
// make the compiler demand every required method, which the private base
// class implements; add it at runtime instead.
+ (void)load {
    // AppleParavirtDevice conforms to MTLDeviceSPI only (it refines MTLDevice)
    Protocol *spi = objc_getProtocol("MTLDeviceSPI");
    class_addProtocol(self, spi ? spi : @protocol(MTLDevice));
    // match Paravirt's conformances. MPS checks
    // [cb conformsToProtocol:@protocol(MTLCommandBuffer)] before it builds its
    // image cache and hands back nil otherwise, which is the WindowServer
    // crash in MPSAutoCache after login. The MTLIOAccel bases don't declare
    // these, each vendor subclass does.
    static const char *const kConf[][2] = {
        { "NVMTLCommandBuffer", "MTLCommandBufferSPI" },
        { "NVMTLCommandQueue",  "MTLCommandQueueSPI" },
        { "MTLIOAccelBuffer",   "MTLBufferSPI" },
        { "NVMTLTexture",       "MTLTextureSPI" },
        { "NVMTLRenderEncoder", "MTLRenderCommandEncoderSPI" },
        { "NVMTLComputeEncoder","MTLComputeCommandEncoderSPI" },
        { "NVMTLBlitEncoder",   "MTLBlitCommandEncoderSPI" },
        { "NVMTLRenderPipelineState",  "MTLRenderPipelineStateSPI" },
        { "NVMTLComputePipelineState", "MTLComputePipelineStateSPI" },
        { "NVMTLDepthStencilState",    "MTLDepthStencilStateSPI" },
        { "NVMTLSamplerState",  "MTLSamplerStateSPI" },
    };
    static const char *const kBase[] = { "MTLCommandBuffer", "MTLCommandQueue", "MTLBuffer" };
    for (size_t i = 0; i < sizeof kConf / sizeof kConf[0]; i++) {
        Class c = objc_getClass(kConf[i][0]);
        Protocol *pr = objc_getProtocol(kConf[i][1]);
        if (!pr && i < 3) pr = objc_getProtocol(kBase[i]);
        if (c && pr) class_addProtocol(c, pr);
    }
    Class buf = objc_getClass("MTLIOAccelBuffer");
    Method m = buf ? class_getInstanceMethod(buf, @selector(gpuAddress)) : NULL;
    if (m) method_setImplementation(m, (IMP)nvBufferGpuAddress);
    else if (buf) class_addMethod(buf, @selector(gpuAddress), (IMP)nvBufferGpuAddress, "Q16@0:8");
    Method dm = buf ? class_getInstanceMethod(buf, @selector(didModifyRange:)) : NULL;
    if (dm) gOrigDidModify = (void (*)(id, SEL, NSRange))method_setImplementation(dm, (IMP)nvDidModifyRange);
    else if (buf) class_addMethod(buf, @selector(didModifyRange:), (IMP)nvDidModifyRange, "v32@0:8{_NSRange=QQ}16");
}

- (instancetype)initWithAcceleratorPort:(io_service_t)port {
    NSLog(@"NVMTLDriver: initWithAcceleratorPort 0x%x", port);
    self = [super initWithAcceleratorPort:port];
    NSLog(@"NVMTLDriver: super init -> %p", self);
    if (self) gNVDevice = self;
    return self;
}

// Values: AppleParavirtDevice (featureProfile 0x2710, llvmVersion 0x7d17, no
// GL / argument buffers / heaps) and Ada limits (1024 threads per group).
- (NSUInteger)featureProfile { return 0x2710; }
- (int)llvmVersion { return 0x7d17; }
- (MTLSize)maxThreadsPerThreadgroup { return MTLSizeMake(1024, 1024, 64); }
- (NSUInteger)maxThreadgroupMemoryLength { return 32768; }
- (BOOL)supportsOpenGL { return NO; }
- (BOOL)supportsSampleCount:(NSUInteger)n { return n == 1 || n == 2 || n == 4 || n == 8; }
- (BOOL)isMagicMipmapSupported { return NO; }
- (BOOL)supportsArgumentBuffers { return NO; }
- (BOOL)supportsResourceHeaps { return NO; }
- (BOOL)supportsDynamicLibraries { return NO; }
- (BOOL)supportsStatefulDynamicLibraries { return NO; }
- (BOOL)supportsRenderDynamicLibraries { return NO; }
- (BOOL)supportsFunctionPointers { return NO; }
- (BOOL)supportsFunctionPointersFromRender { return NO; }
- (BOOL)supportsBinaryArchives { return NO; }
- (BOOL)supportsSharedTextureHandles { return NO; }
- (BOOL)areProgrammableSamplePositionsSupported { return NO; }
- (BOOL)isDepth24Stencil8PixelFormatSupported { return YES; }
- (BOOL)metalAssertionsEnabled { return NO; }
- (void)setMetalAssertionsEnabled:(BOOL)on { (void)on; }
- (NSUInteger)doubleFPConfig { return 0; }
- (NSUInteger)singleFPConfig { return 0; }
- (NSUInteger)halfFPConfig { return 0; }
// MTLDeviceInfo limits table (layout from AppleParavirtDevice's type
// encoding: 31 int/float unions, 32 bytes, 2 uints). Stays zeroed till resources are wired up.
- (const void *)deviceInfo {
    static uint8_t info[4096];
    return info;
}

// Buffers come from the GPU heap (NVMTLGsp) when it's up: the MTLBuffer wraps
// GPU-visible memory, so compute and blits hit it by VA without staging.
// Default since (27 Sep: metal_test 1-11 PASS, copy 19.6 GB/s and
// fill 26.1 GB/s vs 4.1 / 2.5 with staging). NVMTL_NO_HEAP=1 goes back to
// IOAccel sysmem + staging.
static id nvNewHeapBuffer(id dev, NSUInteger len, NSUInteger options) {
    static int off = -1;
    if (off < 0) off = getenv("NVMTL_NO_HEAP") != NULL;
    // MTLResourceStorageModePrivate (2 << 4): GPU-only, so it lives in VRAM.
    if (!off && len && (options & 0xf0) == 0x20) {
        uint64_t va = 0;
        if (nvVramAlloc(len, &va)) {
            id buf = nvNewBuffer(dev, NULL, len, options, nil);
            if (!buf) { nvVramFree(va, len); return nil; }
            NVVramHolder *h = [NVVramHolder new];
            h.va = va; h.len = len;
            objc_setAssociatedObject(buf, &kNVVramKey, h, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            return buf;
        }
    }
    void *cpu = NULL;
    uint64_t va = 0;
    if (off || !len || !nvHeapAlloc(len, &cpu, &va)) return nvNewBuffer(dev, NULL, len, options, nil);
    const NSUInteger plen = (len + vm_page_size - 1) & ~(vm_page_size - 1);
    id buf = nvNewBuffer(dev, cpu, len, options, ^(void *p, NSUInteger l) { (void)l; nvHeapFree(p, plen); });
    if (!buf) { nvHeapFree(cpu, plen); return nil; }
    // managed (1 << 4): GPU copy in VRAM next to the host one
    static int managedOff = -1;
    if (managedOff < 0) managedOff = getenv("NVMTL_MANAGED_SYSMEM") != NULL;
    uint64_t vva = 0;
    if (!managedOff && (options & 0xf0) == 0x10 && nvVramAlloc(len, &vva)) {
        NVVramHolder *h = [NVVramHolder new];
        h.va = vva; h.len = len;
        objc_setAssociatedObject(buf, &kNVVramKey, h, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        NVManaged *m = [NVManaged new];
        m.heapVa = va; m.vramVa = vva; m.len = len;
        objc_setAssociatedObject(buf, &kNVManagedKey, m, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        nvManagedMark(buf, 0, len);          // first use uploads what the app wrote
    }
    return buf;
}

- (id)newBufferWithLength:(NSUInteger)len options:(NSUInteger)options {
    return nvNewHeapBuffer(self, len, options);
}

- (id)newBufferWithBytes:(const void *)bytes length:(NSUInteger)len options:(NSUInteger)options {
    id buf = nvNewHeapBuffer(self, len, options);
    void *dst = buf ? ((void *(*)(id, SEL))objc_msgSend)(buf, @selector(contents)) : NULL;
    if (dst && bytes) memcpy(dst, bytes, len);
    return buf;
}

- (id)newBufferWithBytesNoCopy:(void *)bytes length:(NSUInteger)len options:(NSUInteger)options
                   deallocator:(void (^)(void *, NSUInteger))dealloc {
    return nvNewBuffer(self, bytes, len, options, dealloc);
}

// CoreAnimation uploads images through these. MTLIOAccelDevice wraps
// the bytes in a buffer and asks that buffer for a tiled texture, which only
// vendor buffer classes answer (WindowServer aborted on it at login). We copy
// into an ordinary texture instead; the caller's bytes are not kept.
- (id)newTiledTextureWithBytesNoCopy:(void *)bytes length:(NSUInteger)len
                         deallocator:(void (^)(void *, NSUInteger))dealloc
                          descriptor:(MTLTextureDescriptor *)td offset:(NSUInteger)off
                         bytesPerRow:(NSUInteger)bpr {
    MTLTextureDescriptor *d = [td copy];
    if (d.storageMode == MTLStorageModePrivate) d.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [self newTextureWithDescriptor:d];
    if (t && bytes && bpr) {
        const NSUInteger rows = d.height ? d.height : 1;
        if (off + bpr * (rows - 1) + 1 <= len)
            [t replaceRegion:MTLRegionMake2D(0, 0, d.width, rows) mipmapLevel:0
                   withBytes:(const uint8_t *)bytes + off bytesPerRow:bpr];
    }
    if (dealloc) dealloc(bytes, len);
    return t;
}
- (id)newTiledTextureWithBytesNoCopy:(void *)bytes length:(NSUInteger)len
                          descriptor:(MTLTextureDescriptor *)td offset:(NSUInteger)off
                         bytesPerRow:(NSUInteger)bpr {
    return [self newTiledTextureWithBytesNoCopy:bytes length:len deallocator:nil descriptor:td offset:off
                                    bytesPerRow:bpr];
}

// The vendor MUST override this: _MTLDevice's
// newCommandQueueWithMaxCommandBufferCount: tail-calls
// newCommandQueueWithDescriptor:, and MTLIOAccelDevice's implementation
// tail-calls back - without an override the two recurse forever (RSS
// +1.4 GB/s, live 27 Sep). Paravirt: [[AppleParavirtCommandQueue alloc]
// initWithDevice:descriptor:].
- (id)newCommandQueueWithDescriptor:(id)desc {
    return [[NVMTLCommandQueue alloc] initWithDevice:self descriptor:desc];
}

// textures over a buffer. Buffers are Apple's MTLIOAccelBuffer, which
// leaves this to the vendor, so the method goes onto that class. At offset 0
// the texture shares the buffer's memory (as the IOSurface path does);
// elsewhere it is a copy, which covers upload-style use.
static id nvBufferNewTexture(id buf, SEL _cmd, MTLTextureDescriptor *td, NSUInteger off, NSUInteger bpr) NS_RETURNS_RETAINED {
    (void)_cmd;
    id<MTLDevice> dev = [(id<MTLBuffer>)buf device];
    uint32_t bpp = 0;
    const uint32_t code = nvTexFormat(td.pixelFormat, &bpp);
    const NSUInteger len = [(id<MTLBuffer>)buf length];
    if (!code || !bpp || !td.width || !td.height || bpr < td.width * bpp || off + bpr * (td.height - 1) + td.width * bpp > len) {
        NSLog(@"NVMTLDriver: buffer texture %lux%lu fmt %lu bpr %lu off %lu not supported",
              (unsigned long)td.width, (unsigned long)td.height, (unsigned long)td.pixelFormat,
              (unsigned long)bpr, (unsigned long)off);
        return nil;
    }
    if (off == 0 && td.textureType == MTLTextureType2D && bpr % bpp == 0) {
        NVMTLTexture *t = [NVMTLTexture new];
        t.w = td.width; t.h = td.height; t.fmt = td.pixelFormat;
        t.bpp = bpp; t.fcode = code; t.pitch = bpr; t.use = td.usage;
        t.type = MTLTextureType2D; t.buf = buf;
        return t;
    }
    MTLTextureDescriptor *d = [td copy];
    d.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [dev newTextureWithDescriptor:d];
    const uint8_t *src = [(id<MTLBuffer>)buf contents];
    if (t && src) [t replaceRegion:MTLRegionMake2D(0, 0, td.width, td.height) mipmapLevel:0
                        withBytes:src + off bytesPerRow:bpr];
    return t;
}
- (NSUInteger)minimumTextureBufferAlignmentForPixelFormat:(MTLPixelFormat)f { (void)f; return 256; }
- (NSString *)name { return @"NVIDIA GeForce RTX 4080"; }
- (NSString *)productName { return @"GeForce RTX 4080"; }
- (NSString *)vendorName { return @"NVIDIA"; }
- (NSString *)familyName { return @"Ada Lovelace"; }

// super builds Apple's library (AIR, for validation/function names);
// we compile the MetalSL subset to NAK eagerly and cache by kernel name.
- (id)newLibraryWithSource:(NSString *)src options:(id)opts error:(NSError **)err {
    id lib = [super newLibraryWithSource:src options:opts error:err];
    if (!lib) return nil;
    if (!_nvKernels) _nvKernels = [NSMutableDictionary new];
    if (!_nvFrags) _nvFrags = [NSMutableDictionary new];
    // Apple's front end already turned the source into AIR; take the
    // serialized metallib so these functions go AIR -> NIR -> NAK like
    // precompiled ones (full language). The MetalSL subset stays as the
    // fallback (NVMTL_SUBSET=1 prefers it).
    if ([lib respondsToSelector:@selector(serializeToURL:error:)]) {
        NSString *tmp = [NSTemporaryDirectory() stringByAppendingPathComponent:
                         [NSString stringWithFormat:@"nvmtl-src-%d-%@.metallib", getpid(), [NSUUID UUID].UUIDString]];
        NSURL *u = [NSURL fileURLWithPath:tmp];
        NSError *se = nil;
        if (((BOOL (*)(id, SEL, NSURL *, NSError **))objc_msgSend)(lib, @selector(serializeToURL:error:), u, &se)) {
            NSData *bytes = [NSData dataWithContentsOfURL:u];
            if (bytes.length) [self nvNoteMetallib:bytes library:lib];
        } else NSLog(@"NVMTLDriver: serializing the source library failed: %@", se);
        [[NSFileManager defaultManager] removeItemAtURL:u error:nil];
    }
    // the subset only compiles when something needs it (AIR failed)
    if (!_nvSubsetSrc) _nvSubsetSrc = [NSMutableArray new];
    @synchronized (self) { [_nvSubsetSrc addObject:[src copy]]; }
    return lib;
}
// the async form went straight to Apple's compiler and never reached
// the code above, so its functions had no AIR on our side and no pipeline
// could be built from them. CoreAnimation compiles its shaders this way
// (WindowServer died with "Metal failed to build render pipeline").
- (void)newLibraryWithSource:(NSString *)src options:(MTLCompileOptions *)opts
           completionHandler:(void (^)(id<MTLLibrary>, NSError *))handler {
    if (!handler) return;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError *e = nil;
        id lib = [self newLibraryWithSource:src options:opts error:&e];
        handler(lib, lib ? nil : e);
    });
}
- (void)nvSubsetLoad {
    NSArray *pending;
    @synchronized (self) { pending = [_nvSubsetSrc copy]; [_nvSubsetSrc removeAllObjects]; }
    if (!_nvSubset) _nvSubset = [NSMutableDictionary new];
    for (NSString *src in pending) {
        @try {   // the MetalSL subset translator must never take the app down
            NSDictionary *k = nvCompileKernels(src), *f = nvCompileFragments(src);
            if (k) [_nvSubset addEntriesFromDictionary:k];
            if (f) [_nvFrags addEntriesFromDictionary:f];
        } @catch (NSException *x) {
            NSLog(@"NVMTLDriver: MetalSL subset compile threw %@", x.reason);
        }
    }
}

// 28 Sep: precompiled libraries (metallib / AIR). super still builds Apple's
// library object (function names, reflection); we keep the bytes so each
// kernel can be compiled by nakc --air when a pipeline asks for it.
static void nvHookLibrary(id lib);
- (void)nvNoteMetallib:(NSData *)bytes library:(id<MTLLibrary>)lib {
    NSString *path = nvSaveMetallib(bytes);
    if (!path || !lib) return;
    nvHookLibrary(lib);
    if (!_nvAirLibs) _nvAirLibs = [NSMutableDictionary new];
    for (NSString *n in lib.functionNames) _nvAirLibs[n] = path;
}

- (id)newLibraryWithData:(dispatch_data_t)data error:(NSError **)err {
    id lib = [super newLibraryWithData:data error:err];
    if (lib && data) [self nvNoteMetallib:(NSData *)data library:lib];
    return lib;
}

- (id)newLibraryWithURL:(NSURL *)url error:(NSError **)err {
    id lib = [super newLibraryWithURL:url error:err];
    if (lib && url) [self nvNoteMetallib:[NSData dataWithContentsOfURL:url] library:lib];
    return lib;
}

- (id)newLibraryWithFile:(NSString *)path error:(NSError **)err {
    id lib = [super newLibraryWithFile:path error:err];
    if (lib && path) [self nvNoteMetallib:[NSData dataWithContentsOfFile:path] library:lib];
    return lib;
}

// Function constants. With our device, Apple's specialized function keeps no
// constant values (baseFunctionConstantValues is nil), so the library's
// newFunctionWithName:constantValues:error: is wrapped to hang a copy of the
// values on the function it returns; MTLFunctionConstantValuesInternal's
// newIndexedConstantArray lists {index, dataType, data}.
static char kNvFcKey;
static IMP gOrigNewFnConst;
static id nvNewFunctionConst(id self, SEL _cmd, NSString *name, MTLFunctionConstantValues *cv, NSError **err) {
    id fn = ((id (*)(id, SEL, NSString *, id, NSError **))gOrigNewFnConst)(self, _cmd, name, cv, err);
    if (fn && cv) objc_setAssociatedObject(fn, &kNvFcKey, [cv copy], OBJC_ASSOCIATION_RETAIN);
    return fn;
}

// MPS asks for its specialised kernels through an MTLFunctionDescriptor
static IMP gOrigNewFnDesc;
static id nvNewFunctionDesc(id self, SEL _cmd, MTLFunctionDescriptor *fd, NSError **err) {
    id fn = ((id (*)(id, SEL, id, NSError **))gOrigNewFnDesc)(self, _cmd, fd, err);
    if (fn && fd.constantValues && !objc_getAssociatedObject(fn, &kNvFcKey))
        objc_setAssociatedObject(fn, &kNvFcKey, [fd.constantValues copy], OBJC_ASSOCIATION_RETAIN);
    return fn;
}

static void nvHookLibrary(id lib) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        Method m = class_getInstanceMethod([lib class], @selector(newFunctionWithName:constantValues:error:));
        if (m) gOrigNewFnConst = method_setImplementation(m, (IMP)nvNewFunctionConst);
        Method d = class_getInstanceMethod([lib class], @selector(newFunctionWithDescriptor:error:));
        if (d) gOrigNewFnDesc = method_setImplementation(d, (IMP)nvNewFunctionDesc);
    });
}

static NSArray<NSString *> *nvFunctionConstantArgs(id<MTLFunction> fn) {
    NSMutableArray *out = [NSMutableArray new];
    id cv = objc_getAssociatedObject(fn, &kNvFcKey);
    if (!cv || ![cv respondsToSelector:@selector(newIndexedConstantArray)]) return out;
    NSArray *consts = ((id (*)(id, SEL))objc_msgSend)(cv, @selector(newIndexedConstantArray));
    for (id fc in consts) {
        const NSUInteger idx = ((NSUInteger (*)(id, SEL))objc_msgSend)(fc, @selector(index));
        const MTLDataType type = ((MTLDataType (*)(id, SEL))objc_msgSend)(fc, @selector(dataType));
        const void *p = ((const void *(*)(id, SEL))objc_msgSend)(fc, @selector(data));
        if (!p) continue;
        // vectors too; 3-wide ones take 4 lanes in memory, as in MSL
        size_t n = 0;
        switch (type) {
        case MTLDataTypeBool: case MTLDataTypeChar: case MTLDataTypeUChar: n = 1; break;
        case MTLDataTypeShort: case MTLDataTypeUShort: case MTLDataTypeHalf:
        case MTLDataTypeBool2: case MTLDataTypeChar2: case MTLDataTypeUChar2: n = 2; break;
        case MTLDataTypeInt: case MTLDataTypeUInt: case MTLDataTypeFloat:
        case MTLDataTypeShort2: case MTLDataTypeUShort2: case MTLDataTypeHalf2:
        case MTLDataTypeBool3: case MTLDataTypeChar3: case MTLDataTypeUChar3:
        case MTLDataTypeBool4: case MTLDataTypeChar4: case MTLDataTypeUChar4: n = 4; break;
        case MTLDataTypeLong: case MTLDataTypeULong:
        case MTLDataTypeInt2: case MTLDataTypeUInt2: case MTLDataTypeFloat2:
        case MTLDataTypeShort3: case MTLDataTypeUShort3: case MTLDataTypeHalf3:
        case MTLDataTypeShort4: case MTLDataTypeUShort4: case MTLDataTypeHalf4: n = 8; break;
        case MTLDataTypeInt3: case MTLDataTypeUInt3: case MTLDataTypeFloat3:
        case MTLDataTypeInt4: case MTLDataTypeUInt4: case MTLDataTypeFloat4:
        case MTLDataTypeLong2: case MTLDataTypeULong2: n = 16; break;
        default: n = 0; break;
        }
        if (!n) { NSLog(@"NVMTLDriver: function constant %lu type %lu not handled", (unsigned long)idx, (unsigned long)type); continue; }
        uint64_t v[2] = {0, 0};
        const size_t have = type == MTLDataTypeInt3 || type == MTLDataTypeUInt3 || type == MTLDataTypeFloat3 ? 12
                          : type == MTLDataTypeShort3 || type == MTLDataTypeUShort3 || type == MTLDataTypeHalf3 ? 6
                          : type == MTLDataTypeBool3 || type == MTLDataTypeChar3 || type == MTLDataTypeUChar3 ? 3 : n;
        memcpy(v, p, have);
        [out addObject:n > 8 ? [NSString stringWithFormat:@"fc%lu=%llx,%llx", (unsigned long)idx,
                                (unsigned long long)v[0], (unsigned long long)v[1]]
                             : [NSString stringWithFormat:@"fc%lu=%llx", (unsigned long)idx, (unsigned long long)v[0]]];
    }
    return out;
}

// AIR for a function no matter how its library was made. Libraries
// that come through our newLibrary* overrides are on file already; the rest
// (CoreAnimation builds its shaders through paths we never see) still carry
// their bitcode, which nakc --air takes as is when it isn't a metallib.
static char kNvAirKey;
- (NSString *)nvAirFor:(id<MTLFunction>)fn {
    if (!fn) return nil;
    NSString *p = objc_getAssociatedObject(fn, &kNvAirKey);
    if (p) return p;
    p = _nvAirLibs[fn.name];
    if (!p && [(NSObject *)fn respondsToSelector:@selector(bitcodeData)]) {
        id bc = ((id (*)(id, SEL))objc_msgSend)(fn, @selector(bitcodeData));
        NSData *d = [bc isKindOfClass:[NSData class]] ? bc : nil;
        if (d.length > 8) p = nvSaveAir(d);
        if (!p) NSLog(@"NVMTLDriver: no AIR for %@ (bitcode %lu bytes)", fn.name, (unsigned long)d.length);
    }
    if (p) objc_setAssociatedObject(fn, &kNvAirKey, p, OBJC_ASSOCIATION_RETAIN);
    return p;
}

// every other way in to a pipeline lands on the two builders below.
// _MTLDevice's own versions go to the vendor compiler service and come back
// nil for us (MPS uses the descriptor + reflection form). No reflection yet.
- (id)newComputePipelineStateWithFunction:(id<MTLFunction>)fn options:(MTLPipelineOption)o
                               reflection:(MTLAutoreleasedComputePipelineReflection *)r error:(NSError **)err {
    (void)o; if (r) *r = nil;
    return [self newComputePipelineStateWithFunction:fn error:err];
}
- (id)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)cd options:(MTLPipelineOption)o
                                 reflection:(MTLAutoreleasedComputePipelineReflection *)r error:(NSError **)err {
    (void)o; if (r) *r = nil;
    NVMTLComputePipelineState *ps = [self newComputePipelineStateWithFunction:cd.computeFunction error:err];
    if (ps && cd.label) ps.label = cd.label;
    return ps;
}
- (void)newComputePipelineStateWithFunction:(id<MTLFunction>)fn completionHandler:(MTLNewComputePipelineStateCompletionHandler)h {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError *e = nil; id ps = [self newComputePipelineStateWithFunction:fn error:&e]; h(ps, e);
    });
}
- (void)newComputePipelineStateWithFunction:(id<MTLFunction>)fn options:(MTLPipelineOption)o
                          completionHandler:(MTLNewComputePipelineStateWithReflectionCompletionHandler)h {
    (void)o;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError *e = nil; id ps = [self newComputePipelineStateWithFunction:fn error:&e]; h(ps, nil, e);
    });
}
- (void)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)cd options:(MTLPipelineOption)o
                            completionHandler:(MTLNewComputePipelineStateWithReflectionCompletionHandler)h {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError *e = nil; id ps = [self newComputePipelineStateWithDescriptor:cd options:o reflection:NULL error:&e]; h(ps, nil, e);
    });
}
- (id)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)rd options:(MTLPipelineOption)o
                                reflection:(MTLAutoreleasedRenderPipelineReflection *)r error:(NSError **)err {
    (void)o; if (r) *r = nil;
    return [self newRenderPipelineStateWithDescriptor:rd error:err];
}
- (void)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)rd
                           completionHandler:(MTLNewRenderPipelineStateCompletionHandler)h {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError *e = nil; id ps = [self newRenderPipelineStateWithDescriptor:rd error:&e]; h(ps, e);
    });
}
- (void)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)rd options:(MTLPipelineOption)o
                           completionHandler:(MTLNewRenderPipelineStateWithReflectionCompletionHandler)h {
    (void)o;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError *e = nil; id ps = [self newRenderPipelineStateWithDescriptor:rd error:&e]; h(ps, nil, e);
    });
}

- (id)newComputePipelineStateWithFunction:(id<MTLFunction>)fn error:(NSError **)err {
    NSString *air = [self nvAirFor:fn];
    NSArray<NSString *> *fcArgs = air ? nvFunctionConstantArgs(fn) : @[];
    // specialized functions share a name: key the kernel by its constants too
    NSString *kkey = fcArgs.count ? [NSString stringWithFormat:@"%@|%@", fn.name, [fcArgs componentsJoinedByString:@","]]
                                  : fn.name;
    NVMTLKernel *k = _nvKernels[kkey];
    const bool subsetFirst = getenv("NVMTL_SUBSET") != NULL;
    if (!k && subsetFirst) { [self nvSubsetLoad]; k = _nvSubset[fn.name]; }
    if (!k && air) {
        k = nvCompileAir(air, fn.name, 256, 1, 1, fcArgs);
        if (!_nvKernels) _nvKernels = [NSMutableDictionary new];
        if (k) _nvKernels[kkey] = k;
    }
    if (!k) { [self nvSubsetLoad]; k = _nvSubset[fn.name]; }
    if (!k) {
        NSLog(@"NVMTLDriver: no NAK for function %@", fn.name);
        if (err) *err = [NSError errorWithDomain:@"NVMTLDriver" code:1
                            userInfo:@{NSLocalizedDescriptionKey: @"unsupported kernel (outside the subset we compile)"}];
        return nil;
    }
    NVMTLComputePipelineState *ps = [NVMTLComputePipelineState new];
    ps.kernel = k; ps.device = self;
    return ps;
}

// vertex goes through NAK via our compiler; fragment = const color (nvCompileFragments).
- (id)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)rd error:(NSError **)err {
    // precompiled vertex + fragment functions -> 3D engine
    NSString *vn = rd.vertexFunction.name, *fname = rd.fragmentFunction.name;
    NSString *vair = [self nvAirFor:rd.vertexFunction], *fair = [self nvAirFor:rd.fragmentFunction];
    if (!getenv("NVMTL_SUBSET") && vn && fname && vair && fair) {
        NVMTLKernel *vs = nvCompileAir(vair, vn, 1, 1, 1, nvFunctionConstantArgs(rd.vertexFunction));
        NSMutableArray *fa = [nvFunctionConstantArgs(rd.fragmentFunction) mutableCopy];
        if (vs.io.length) [fa addObject:[@"io=" stringByAppendingString:vs.io]];
        NVMTLKernel *fs = vs ? nvCompileAir(fair, fname, 1, 1, 1, fa) : nil;
        if (vs.stage == 3 && fs.stage == 2) {
            // post-tessellation vertex function: rebuild it with the partitioning
            // and winding, then generate the fetch VS and the factor TCS
            static const uint32_t spacing[4] = {0, 0, 1, 2};   // pow2 (as integer), integer, fract. odd, fract. even
            NSMutableArray *ta = [nvFunctionConstantArgs(rd.vertexFunction) mutableCopy];
            [ta addObject:[NSString stringWithFormat:@"tspace=%u", spacing[rd.tessellationPartitionMode & 3]]];
            [ta addObject:[NSString stringWithFormat:@"tccw=%u", rd.tessellationOutputWindingOrder == MTLWindingCounterClockwise]];
            NVMTLKernel *tes = nvCompileAir(vair, vn, 1, 1, 1, ta);
            NSMutableArray<NSNumber *> *locs = [NSMutableArray new];
            for (NSUInteger i = 0; i < 31 && rd.vertexDescriptor; i++)
                if (rd.vertexDescriptor.attributes[i].format != MTLVertexFormatInvalid) [locs addObject:@(i)];
            NVMTLKernel *gvs = tes ? nvCompileTessGen(@"vs", locs, tes.tessCps, tes.tessDomain) : nil;
            NVMTLKernel *tcs = gvs ? nvCompileTessGen(@"tcs", locs, tes.tessCps, tes.tessDomain) : nil;
            if (tes && gvs && tcs && tes.tessParams) {
                NVMTLRenderPipelineState *ps = [NVMTLRenderPipelineState new];
                ps.hwVS = gvs; ps.hwTCS = tcs; ps.hwTES = tes; ps.hwFS = fs; ps.device = self; ps.label = rd.label;
                ps.maxTessFactor = rd.maxTessellationFactor ? (float)rd.maxTessellationFactor : 16.0f;
                ps.cpIndexType = rd.tessellationControlPointIndexType == MTLTessellationControlPointIndexTypeUInt16
                                 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
                ps.vdesc = [rd.vertexDescriptor copy];
                NSMutableArray *ca = [NSMutableArray new];
                for (NSUInteger i = 0; i < 8; i++) [ca addObject:[rd.colorAttachments[i] copy]];
                ps.colorAtt = ca;
                return ps;
            }
            NSLog(@"NVMTLDriver: tessellation pipeline %@ not built", vn);
            if (err) *err = [NSError errorWithDomain:@"NVMTLDriver" code:3
                                userInfo:@{NSLocalizedDescriptionKey: @"tessellation pipeline failed"}];
            return nil;
        }
        if (vs && fs && vs.stage == 1 && fs.stage == 2) {
            NVMTLRenderPipelineState *ps = [NVMTLRenderPipelineState new];
            ps.hwVS = vs; ps.hwFS = fs; ps.device = self; ps.label = rd.label;
            ps.vdesc = [rd.vertexDescriptor copy];
            NSMutableArray *ca = [NSMutableArray new];
            for (NSUInteger i = 0; i < 8; i++) [ca addObject:[rd.colorAttachments[i] copy]];
            ps.colorAtt = ca;
            return ps;
        }
        NSLog(@"NVMTLDriver: %@ / %@ not built for the 3D engine, trying the MetalSL path", vn, fname);
    }
    [self nvSubsetLoad];
    NVMTLKernel *vs = _nvSubset[rd.vertexFunction.name];
    NVMTLFragment *fc = _nvFrags[rd.fragmentFunction.name];
    if (!vs || !fc || (!fc.raster && !fc.hasColor)) {
        NSLog(@"NVMTLDriver: render pipe unsupported (vs=%@ fc=%@)", rd.vertexFunction.name, rd.fragmentFunction.name);
        if (err) *err = [NSError errorWithDomain:@"NVMTLDriver" code:2
                            userInfo:@{NSLocalizedDescriptionKey: @"unsupported render pipeline (outside what we support yet)"}];
        return nil;
    }
    NVMTLRenderPipelineState *ps = [NVMTLRenderPipelineState new];
    ps.vs = vs; ps.fs = fc; ps.device = self; ps.label = rd.label;
    if (fc.hasColor) [ps nvSetFsColor:fc.nvColor];
    MTLRenderPipelineColorAttachmentDescriptor *ca = rd.colorAttachments[0];
    if (ca.blendingEnabled)
        ps.blend = 1u | (uint32_t)ca.sourceRGBBlendFactor << 4 | (uint32_t)ca.destinationRGBBlendFactor << 8 |
                   (uint32_t)ca.sourceAlphaBlendFactor << 12 | (uint32_t)ca.destinationAlphaBlendFactor << 16 |
                   (uint32_t)ca.rgbBlendOperation << 20 | (uint32_t)ca.alphaBlendOperation << 24;
    if (!fc.raster && !vs.voutStride) NSLog(@"NVMTLDriver: pipeline %@ on the v1 constant-color path", rd.label);
    return ps;
}

// M6 v1: 2D RGBA8/BGRA8 RT, CPU-backed (draw uploads/downloads via staging).
// any format in nvTexFormat, linear (tight pitch) in the zero-copy
// heap, so the GPU reaches it directly and the CPU can map it.
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)td {
    // mipmapped / array / cube / 3D / 1D: block linear in VRAM
    const uint32_t hwbpp = nvFormatBytes(td.pixelFormat);
    // managed textures (the macOS default) live in VRAM too; CPU
    // access goes through the copy engine like the other block-linear ones
    static int managedTexOff = -1;
    if (managedTexOff < 0) managedTexOff = getenv("NVMTL_MANAGED_SYSMEM") != NULL;
    const bool managedVram = !managedTexOff && td.storageMode == MTLStorageModeManaged &&
                             td.textureType == MTLTextureType2D && !nvZetaBytes(td.pixelFormat);
    if (hwbpp && td.sampleCount <= 1 && td.width &&
        (td.mipmapLevelCount > 1 || td.textureType != MTLTextureType2D || managedVram)) {
        const MTLTextureType tt = td.textureType;
        const bool cube = tt == MTLTextureTypeCube || tt == MTLTextureTypeCubeArray;
        const bool arr = tt == MTLTextureType2DArray || tt == MTLTextureType1DArray || tt == MTLTextureTypeCubeArray;
        const NSUInteger h = (tt == MTLTextureType1D || tt == MTLTextureType1DArray) ? 1 : td.height;
        const NSUInteger layers = (arr ? td.arrayLength : 1) * (cube ? 6 : 1);
        NVTexLayout L;
        nvTexLayoutInit(&L, (uint32_t)td.width, (uint32_t)h, (uint32_t)td.depth, (uint32_t)layers,
                        (uint32_t)td.mipmapLevelCount, hwbpp, tt == MTLTextureType3D);
        uint64_t va = 0;
        uint32_t handle = 0;
        if (!nvVramAllocKind(L.size, NV_KIND_COLOR, &va, &handle)) return nil;
        NVMTLTexture *t = [NVMTLTexture new];
        t.w = td.width; t.h = h; t.fmt = td.pixelFormat; t.bpp = hwbpp; t.use = td.usage;
        t.pitch = L.rowBytes[0]; t.priv = YES;
        t.blVa = va; t.blHandle = handle; t.blBh = L.ylog[0];
        t.lay = [NSData dataWithBytes:&L length:sizeof L];
        t.type = tt; t.dep = td.depth; t.layersN = layers; t.levelsN = L.levels;
        return t;
    }
    uint32_t msx = 1, msy = 1;
    if (td.sampleCount > 1 && hwbpp && !nvZetaBytes(td.pixelFormat) && td.width && td.height &&
        nvSampleLayout(td.sampleCount, &msx, &msy, NULL)) {
        // multisampled colour: block linear, one sample per element
        // of a (w*sx) x (h*sy) surface, as NIL lays out MSAA images
        NVTexLayout L;
        nvTexLayoutInit(&L, (uint32_t)(td.width * msx), (uint32_t)(td.height * msy), 1, 1, 1, hwbpp, false);
        uint64_t va = 0;
        uint32_t handle = 0;
        if (!nvVramAllocKind(L.size, NV_KIND_COLOR, &va, &handle)) return nil;
        NVMTLTexture *t = [NVMTLTexture new];
        t.w = td.width; t.h = td.height; t.fmt = td.pixelFormat; t.bpp = hwbpp; t.use = td.usage;
        t.pitch = L.rowBytes[0]; t.priv = YES; t.samples = td.sampleCount;
        t.blVa = va; t.blHandle = handle; t.blBh = L.ylog[0];
        t.lay = [NSData dataWithBytes:&L length:sizeof L];
        t.type = MTLTextureType2DMultisample; t.dep = 1; t.layersN = 1; t.levelsN = 1;
        return t;
    }
    if (td.pixelFormat == MTLPixelFormatDepth32Float && td.sampleCount > 1) {
        // multisampled Depth32Float: zeta surface only, like the other formats
        if (!nvSampleLayout(td.sampleCount, NULL, NULL, NULL)) return nil;
        NVMTLTexture *t = [NVMTLTexture new];
        t.w = td.width; t.h = td.height; t.fmt = td.pixelFormat; t.bpp = 4; t.samples = td.sampleCount;
        t.pitch = (td.width * 4 + 63) & ~(NSUInteger)63; t.use = td.usage; t.priv = YES;
        return t;
    }
    if (td.pixelFormat != MTLPixelFormatDepth32Float && nvZetaBytes(td.pixelFormat) &&
        (td.textureType == MTLTextureType2D || td.textureType == MTLTextureType2DMultisample) &&
        td.width && td.height && nvSampleLayout(td.sampleCount, NULL, NULL, NULL)) {
        // depth/stencil attachment: only the block-linear zeta surface,
        // made at first use in a pass (not sampleable, no CPU access yet)
        NVMTLTexture *t = [NVMTLTexture new];
        t.w = td.width; t.h = td.height; t.fmt = td.pixelFormat; t.bpp = nvZetaBytes(td.pixelFormat);
        t.pitch = (td.width * t.bpp + 63) & ~(NSUInteger)63; t.use = td.usage; t.priv = YES;
        t.samples = td.sampleCount;
        return t;
    }
    uint32_t bpp = 0;
    const uint32_t code = nvTexFormat(td.pixelFormat, &bpp);
    if (td.textureType != MTLTextureType2D || !code || !td.width || !td.height ||
        td.mipmapLevelCount > 1 || td.sampleCount > 1) {
        NSLog(@"NVMTLDriver: texture type %lu format %lu %lux%lu unsupported",
              (unsigned long)td.textureType, (unsigned long)td.pixelFormat,
              (unsigned long)td.width, (unsigned long)td.height);
        return nil;
    }
    NVMTLTexture *t = [NVMTLTexture new];
    t.w = td.width; t.h = td.height; t.fmt = td.pixelFormat;
    // rows 64-byte aligned, as hardware texture headers need;
    // 128, so the 3D engine can render to them (NIL's linear rule)
    t.bpp = bpp; t.fcode = code; t.pitch = (td.width * bpp + 127) & ~(NSUInteger)127; t.use = td.usage;
    // private textures (render targets, depth, anything the CPU never
    // touches) live in VRAM, the rest in the zero-copy host heap: rendering
    // and sampling through PCIe was most of the cost for real content.
    t.priv = td.storageMode == MTLStorageModePrivate;
    t.buf = [self newBufferWithLength:t.pitch * t.h
                              options:t.priv ? MTLResourceStorageModePrivate : MTLResourceStorageModeShared];
    return t.buf ? t : nil;
}

- (id)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)sd {
    NVMTLSamplerState *s = [NVMTLSamplerState new];
    uint32_t am = 0;
    switch (sd.sAddressMode) {
    case MTLSamplerAddressModeRepeat: am = 1; break;
    case MTLSamplerAddressModeMirrorRepeat: am = 2; break;
    case MTLSamplerAddressModeClampToZero:
    case MTLSamplerAddressModeClampToBorderColor: am = 3; break;
    default: am = 0; break;
    }
    s.mode = (sd.magFilter == MTLSamplerMinMagFilterLinear ? 1u : 0u) | am << 1 |
             (sd.normalizedCoordinates ? 0u : 8u);
    s.tsc = nvTscAlloc(sd);
    s.device = self; s.label = sd.label;
    return s;
}

// M6-full: wrap an IOSurface's plane 0 in a texture. The surface owns the
// memory; the texture's buffer is a NoCopy view of it (zero-copy, the GPU
// maps the same pages, so another process that looks the surface up sees
// our writes). 0.2.1: any format in nvTexFormat whose bytes per element
// match, and the surface's real row pitch (CoreAnimation/WindowServer
// surfaces pad rows, v1 took tight rows only), no 1 MiB cap.
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)td iosurface:(IOSurfaceRef)io plane:(NSUInteger)plane {
    uint32_t bpp = 0;
    uint32_t code = nvTexFormat(td.pixelFormat, &bpp);
    if (!code) bpp = nvFormatBytes(td.pixelFormat);   // hardware-sampled formats (R16Unorm, RG16Unorm ...)
    // planar surfaces (video, 'x420' / '420v'): one texture per plane
    const size_t planes = io ? IOSurfaceGetPlaneCount(io) : 0;
    if (!io || (planes ? plane >= planes : plane != 0) || td.textureType != MTLTextureType2D || !bpp) {
        NSLog(@"NVMTLDriver: IOSurface %u texture refused: plane %lu/%zu type %lu format %lu surface fmt 0x%x",
              io ? IOSurfaceGetID(io) : 0, (unsigned long)plane, planes, (unsigned long)td.textureType,
              (unsigned long)td.pixelFormat, io ? IOSurfaceGetPixelFormat(io) : 0);
        return nil;
    }
    const size_t w = planes ? IOSurfaceGetWidthOfPlane(io, plane) : IOSurfaceGetWidth(io);
    const size_t h = planes ? IOSurfaceGetHeightOfPlane(io, plane) : IOSurfaceGetHeight(io);
    const size_t bpr = planes ? IOSurfaceGetBytesPerRowOfPlane(io, plane) : IOSurfaceGetBytesPerRow(io);
    const size_t bpe = planes ? IOSurfaceGetBytesPerElementOfPlane(io, plane) : IOSurfaceGetBytesPerElement(io);
    // the texture may cover less than the surface (icon services asks
    // for 32x32 out of a 64x64 surface); it starts at the surface origin
    if (!w || !h || !td.width || !td.height || td.width > w || td.height > h ||
        bpe != bpp || bpr < w * bpp || bpr % bpp) {
        NSLog(@"NVMTLDriver: IOSurface %u plane %lu %zux%zu bpr %zu bpe %zu does not fit format %lu %lux%lu",
              IOSurfaceGetID(io), (unsigned long)plane, w, h, bpr, bpe, (unsigned long)td.pixelFormat,
              (unsigned long)td.width, (unsigned long)td.height);
        return nil;
    }
    uint8_t *base = IOSurfaceGetBaseAddress(io);
    uint8_t *pbase = planes ? IOSurfaceGetBaseAddressOfPlane(io, plane) : base;
    const size_t alloc = IOSurfaceGetAllocSize(io);
    const size_t off = base && pbase >= base ? (size_t)(pbase - base) : ~(size_t)0;
    if (!base || off >= alloc || alloc - off < bpr * h) {
        NSLog(@"NVMTLDriver: IOSurface %u plane %lu at +0x%zx alloc %zu (need %zu)",
              IOSurfaceGetID(io), (unsigned long)plane, off, alloc, bpr * h);
        return nil;
    }
    // map from the page the plane starts in; a plane that doesn't start on a
    // page gets a view whose contents point at it (the VA follows contents)
    const size_t skew = off & 0xfff;
    uint8_t *mbase = pbase - skew;
    const size_t len = alloc - off + skew;
    NVMTLTexture *t = [NVMTLTexture new];
    t.w = td.width; t.h = td.height; t.fmt = td.pixelFormat;
    t.bpp = bpp; t.fcode = code; t.pitch = bpr; t.use = td.usage;
    // The GPU needs the surface's pages at a VA of ours: wire and map them
    // (kext userMemBind). Another process wrapping the same surface maps the
    // same physical pages into its own arena.
    uint64_t va = 0;
    if (!nvHeapWrap(mbase, len, &va)) {
        NSLog(@"NVMTLDriver: IOSurface %u pages could not be mapped for the GPU", IOSurfaceGetID(io));
        return nil;
    }
    CFRetain(io);   // the view must not outlive the pages
    id<MTLBuffer> whole = [self newBufferWithBytesNoCopy:mbase length:(len + 0xfff) & ~(size_t)0xfff
                                   options:MTLResourceStorageModeShared
                               deallocator:^(void *p, NSUInteger l){ (void)l; nvHeapUnwrap(p, va); CFRelease(io); }];
    if (!whole) { nvHeapUnwrap(mbase, va); CFRelease(io); return nil; }
    // The deallocator owns exactly this VA. Never rediscover it by CPU
    // address: another wrapper of this surface may be released first.
    nvBufferSetMapping(whole, va, (len + 0xfff) & ~(size_t)0xfff);
    t.buf = skew ? (id<MTLBuffer>)[[NVPlaneBuffer alloc] initWithBuffer:whole skew:skew] : whole;
    if (skew) nvBufferSetMapping(t.buf, va + skew, whole.length - skew);
    t.ios = io;
    t.iosPlane = plane;
    return t;
}

- (id)newHeapWithDescriptor:(MTLHeapDescriptor *)hd {
    if (!hd.size) return nil;
    NVMTLHeap *h = [NVMTLHeap new];
    h.device = self; h.size = hd.size; h.storageMode = hd.storageMode;
    h.cpuCacheMode = hd.cpuCacheMode; h.hazardTrackingMode = hd.hazardTrackingMode;
    h.type = hd.type;
    return h;
}
- (MTLSizeAndAlign)heapBufferSizeAndAlignWithLength:(NSUInteger)len options:(MTLResourceOptions)o {
    (void)o; return (MTLSizeAndAlign){(len + 0xffff) & ~(NSUInteger)0xffff, 0x10000};
}
- (MTLSizeAndAlign)heapTextureSizeAndAlignWithDescriptor:(MTLTextureDescriptor *)td {
    return (MTLSizeAndAlign){nvHeapTextureBytes(td), 0x10000};
}
- (id)newDepthStencilStateWithDescriptor:(MTLDepthStencilDescriptor *)dd {
    NVMTLDepthStencilState *s = [NVMTLDepthStencilState new];
    s.mode = ((uint32_t)dd.depthCompareFunction & 7u) | (dd.depthWriteEnabled ? 8u : 0u);
    static const uint32_t ops[8] = {0x1e00, 0, 0x1e01, 0x1e02, 0x1e03, 0x150a, 0x8507, 0x8508};
    uint32_t st[2][6];
    BOOL on = NO;
    MTLStencilDescriptor *sd[2] = {dd.frontFaceStencil, dd.backFaceStencil};
    for (int i = 0; i < 2; i++) {
        MTLStencilDescriptor *x = sd[i];
        if (!x) { st[i][0] = st[i][1] = st[i][2] = 0x1e00; st[i][3] = 0x207; st[i][4] = st[i][5] = 0xff; continue; }
        st[i][0] = ops[x.stencilFailureOperation & 7]; st[i][1] = ops[x.depthFailureOperation & 7];
        st[i][2] = ops[x.depthStencilPassOperation & 7]; st[i][3] = 0x200 + ((uint32_t)x.stencilCompareFunction & 7);
        st[i][4] = x.readMask & 0xff; st[i][5] = x.writeMask & 0xff;
        if (st[i][3] != 0x207 || st[i][0] != 0x1e00 || st[i][1] != 0x1e00 || st[i][2] != 0x1e00) on = YES;
    }
    s.stencil = on;
    s.sten = [NSData dataWithBytes:st length:sizeof st];
    // Always + no write needs no depth work at all
    if (s.mode == MTLCompareFunctionAlways) s.mode = 0;
    s.device = self; s.label = dd.label;
    return s;
}

// M18 v1: fences inert (synchronous execution), events fully valued.
- (id)newFence {
    NVMTLFence *f = [NVMTLFence new];
    f.device = self;
    return f;
}
- (id)newSharedEvent {
    NVMTLSharedEvent *e = [NVMTLSharedEvent new];
    e.device = self;
    return e;
}

@end
