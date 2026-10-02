// mtlspy.dylib: DYLD_INSERT_LIBRARIES logger for the NVMTL driver classes inside a system process
// (first use: the iconservicesagent blank-icon hunt, 1 Oct 2026). Logs, as "MTLSPY ..." NSLog lines:
// texture creation (type/format/size/usage/storage/IOSurface), getBytes readbacks with a non-zero
// byte count of what came back, render pass attachments (load/store/clear), blits, IOSurface
// wraps and command-buffer status. Inject with launchctl debug ... --environment DYLD_INSERT_LIBRARIES=...
// Build: clang -fobjc-arc -dynamiclib -framework Metal -framework IOSurface -framework Foundation mtlspy.m -o mtlspy.dylib
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <objc/runtime.h>
#import <objc/message.h>
#import <mach-o/dyld.h>

#import <os/log.h>
// NSLog %@ arguments come out <private> in the unified log; format first, log public.
#define SPY(fmt, ...) os_log(OS_LOG_DEFAULT, "MTLSPY %{public}s", [NSString stringWithFormat:fmt, ##__VA_ARGS__].UTF8String)

static NSString *texDesc(id<MTLTexture> t)
{
    if (!t) return @"(nil)";
    return [NSString stringWithFormat:@"%p[t%lu f%lu %lux%lu m%lu u0x%lx s%lu ios%u]", t, (unsigned long)t.textureType,
            (unsigned long)t.pixelFormat, (unsigned long)t.width, (unsigned long)t.height,
            (unsigned long)t.mipmapLevelCount, (unsigned long)t.usage, (unsigned long)t.storageMode,
            t.iosurface ? IOSurfaceGetID(t.iosurface) : 0];
}

static IMP swap(Class c, SEL s, IMP n)
{
    Method m = class_getInstanceMethod(c, s);
    if (!m) { SPY(@"no %@ on %s", NSStringFromSelector(s), class_getName(c)); return NULL; }
    IMP old = method_getImplementation(m);
    if (!class_addMethod(c, s, n, method_getTypeEncoding(m))) method_setImplementation(m, n);
    return old;
}

static NSString *stDesc(MTLStencilDescriptor *s)
{
    if (!s) return @"-";
    return [NSString stringWithFormat:@"cmp%lu fail%lu zfail%lu pass%lu rm%x wm%x", (unsigned long)s.stencilCompareFunction,
            (unsigned long)s.stencilFailureOperation, (unsigned long)s.depthFailureOperation,
            (unsigned long)s.depthStencilPassOperation, s.readMask, s.writeMask];
}
static IMP o_newDSS;
static id h_newDSS(id self, SEL s, MTLDepthStencilDescriptor *d)
{
    id r = ((id (*)(id, SEL, id))o_newDSS)(self, s, d);
    SPY(@"newDepthStencilState %p depth cmp%lu write%d front[%@] back[%@]", r, (unsigned long)d.depthCompareFunction,
        d.depthWriteEnabled, stDesc(d.frontFaceStencil), stDesc(d.backFaceStencil));
    return r;
}
// MTLSPY_FS_OVERRIDE=<vertex function name>: pipelines with that vertex function get our debug
// fragment function instead, writing the interpolated primitive_position (RenderBox's varying) to
// colour 0 as (x, y, 0, 1) with no blending, every other colour target masked off.
static IMP o_newRPS;
static id h_newRPS(id self, SEL s, MTLRenderPipelineDescriptor *d, NSError **err)
{
    const char *want = getenv("MTLSPY_FS_OVERRIDE");
    if (want && d.vertexFunction && !strcmp(d.vertexFunction.name.UTF8String, want)) {
        static id<MTLFunction> dbg;
        if (!dbg) {
            id<MTLLibrary> lib = [(id<MTLDevice>)self newLibraryWithSource:
                @"#include <metal_stdlib>\nusing namespace metal;\n"
                 "struct VO { float4 p [[position]]; float2 primitive_position [[center_no_perspective]]; float2 gradient_coord [[center_no_perspective]]; };\n"
                 "fragment float4 spyfs(VO i [[stage_in]]) { return float4(i.primitive_position * 0.5 + 0.5, 0.5, 1); }\n" options:nil error:NULL];
            dbg = [lib newFunctionWithName:@"spyfs"];
        }
        MTLRenderPipelineDescriptor *c = [d copy];
        c.fragmentFunction = dbg;
        for (int i = 0; i < 8; i++) {
            c.colorAttachments[i].blendingEnabled = NO;
            c.colorAttachments[i].writeMask = i == 0 ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
        }
        SPY(@"pipeline %@/%@ -> debug fragment", d.vertexFunction.name, d.fragmentFunction.name);
        d = c;
    }
    return ((id (*)(id, SEL, id, NSError **))o_newRPS)(self, s, d, err);
}
static IMP o_newTex, o_newTexIOS, o_getBytes, o_getBytes2, o_rpe, o_blitT2T, o_blitT2B, o_blitB2T, o_commit,
    o_present, o_genMips;

static id h_newTex(id self, SEL s, MTLTextureDescriptor *d)
{
    id t = ((id (*)(id, SEL, id))o_newTex)(self, s, d);
    SPY(@"newTexture %@", texDesc(t));
    return t;
}

static id h_newTexIOS(id self, SEL s, MTLTextureDescriptor *d, IOSurfaceRef io, NSUInteger plane)
{
    id t = ((id (*)(id, SEL, id, IOSurfaceRef, NSUInteger))o_newTexIOS)(self, s, d, io, plane);
    SPY(@"newTexture iosurface id%u fmt'%.4s' bpr%zu plane%lu -> %@", io ? IOSurfaceGetID(io) : 0,
        (char *)&(uint32_t){ CFSwapInt32(io ? IOSurfaceGetPixelFormat(io) : 0) }, io ? IOSurfaceGetBytesPerRow(io) : 0,
        (unsigned long)plane, texDesc(t));
    return t;
}

static size_t nonzero(const void *p, size_t n)
{
    size_t z = 0;
    for (size_t i = 0; i < n; i++) z += ((const uint8_t *)p)[i] != 0;
    return z;
}

static void h_getBytes(id self, SEL s, void *p, NSUInteger bpr, MTLRegion r, NSUInteger lvl)
{
    ((void (*)(id, SEL, void *, NSUInteger, MTLRegion, NSUInteger))o_getBytes)(self, s, p, bpr, r, lvl);
    size_t n = bpr * r.size.height;
    static int dumps;
    if (getenv("MTLSPY_DUMP") && dumps < 16) {      // raw readback, in the process's temp dir
        NSString *f = [NSTemporaryDirectory() stringByAppendingFormat:@"mtlspy-%d-%d-f%lu-%lux%lu-bpr%lu.raw", getpid(), dumps++,
                       (unsigned long)[(id<MTLTexture>)self pixelFormat], (unsigned long)r.size.width, (unsigned long)r.size.height, (unsigned long)bpr];
        [[NSData dataWithBytes:p length:n] writeToFile:f atomically:NO];
        SPY(@"dumped %@", f);
    }
    SPY(@"getBytes %@ region %lux%lu lvl%lu nonzero %zu/%zu", texDesc(self), (unsigned long)r.size.width,
        (unsigned long)r.size.height, (unsigned long)lvl, nonzero(p, n), n);
}

static void h_getBytes2(id self, SEL s, void *p, NSUInteger bpr, NSUInteger bpi, MTLRegion r, NSUInteger lvl, NSUInteger sl)
{
    ((void (*)(id, SEL, void *, NSUInteger, NSUInteger, MTLRegion, NSUInteger, NSUInteger))o_getBytes2)(self, s, p, bpr, bpi, r, lvl, sl);
    size_t n = bpr * r.size.height;
    SPY(@"getBytes2 %@ region %lux%lu lvl%lu slice%lu nonzero %zu/%zu", texDesc(self), (unsigned long)r.size.width,
        (unsigned long)r.size.height, (unsigned long)lvl, (unsigned long)sl, nonzero(p, n), n);
}

static char kTargets;
static NSUInteger bppOf(MTLPixelFormat f)
{
    switch (f) {
    case MTLPixelFormatR8Unorm: return 1;
    case MTLPixelFormatR16Float: case MTLPixelFormatRG8Unorm: return 2;
    case MTLPixelFormatRGBA16Float: case MTLPixelFormatRG32Float: return 8;
    case MTLPixelFormatRGBA32Float: return 16;
    default: return 4;
    }
}

static id h_rpe(id self, SEL s, MTLRenderPassDescriptor *d)
{
    if (getenv("MTLSPY_CONTENT")) {
        NSMutableArray *ts = objc_getAssociatedObject(self, &kTargets);
        if (!ts) { ts = [NSMutableArray array]; objc_setAssociatedObject(self, &kTargets, ts, OBJC_ASSOCIATION_RETAIN); }
        for (int i = 0; i < 8; i++) if (d.colorAttachments[i].texture && ![ts containsObject:d.colorAttachments[i].texture])
            [ts addObject:d.colorAttachments[i].texture];
    }
    NSMutableString *m = [NSMutableString string];
    for (int i = 0; i < 8; i++) {
        MTLRenderPassColorAttachmentDescriptor *a = d.colorAttachments[i];
        if (!a.texture) continue;
        MTLClearColor c = a.clearColor;
        [m appendFormat:@" c%d=%@ l%lu s%lu clr(%.2f,%.2f,%.2f,%.2f)%@", i, texDesc(a.texture), (unsigned long)a.loadAction,
            (unsigned long)a.storeAction, c.red, c.green, c.blue, c.alpha,
            a.resolveTexture ? [@" resolve=" stringByAppendingString:texDesc(a.resolveTexture)] : @""];
    }
    if (d.depthAttachment.texture) [m appendFormat:@" depth=%@", texDesc(d.depthAttachment.texture)];
    if (d.stencilAttachment.texture) [m appendFormat:@" stencil=%@ l%lu s%lu clr%u", texDesc(d.stencilAttachment.texture),
        (unsigned long)d.stencilAttachment.loadAction, (unsigned long)d.stencilAttachment.storeAction, d.stencilAttachment.clearStencil];
    SPY(@"cb %p renderPass%@", self, m);
    return ((id (*)(id, SEL, id))o_rpe)(self, s, d);
}

static void h_blitT2T(id self, SEL s, id src, NSUInteger ss, NSUInteger sl, MTLOrigin so, MTLSize sz, id dst,
                      NSUInteger ds, NSUInteger dl, MTLOrigin dor)
{
    SPY(@"blit tex->tex %@ -> %@ %lux%lu", texDesc(src), texDesc(dst), (unsigned long)sz.width, (unsigned long)sz.height);
    ((void (*)(id, SEL, id, NSUInteger, NSUInteger, MTLOrigin, MTLSize, id, NSUInteger, NSUInteger, MTLOrigin))o_blitT2T)(
        self, s, src, ss, sl, so, sz, dst, ds, dl, dor);
}

static void h_blitT2B(id self, SEL s, id src, NSUInteger ss, NSUInteger sl, MTLOrigin so, MTLSize sz, id<MTLBuffer> dst,
                      NSUInteger off, NSUInteger bpr, NSUInteger bpi)
{
    SPY(@"blit tex->buf %@ -> buf%p+%lu len%lu bpr%lu %lux%lu", texDesc(src), dst, (unsigned long)off,
        (unsigned long)dst.length, (unsigned long)bpr, (unsigned long)sz.width, (unsigned long)sz.height);
    ((void (*)(id, SEL, id, NSUInteger, NSUInteger, MTLOrigin, MTLSize, id, NSUInteger, NSUInteger, NSUInteger))o_blitT2B)(
        self, s, src, ss, sl, so, sz, dst, off, bpr, bpi);
}

static void h_blitB2T(id self, SEL s, id<MTLBuffer> src, NSUInteger off, NSUInteger bpr, NSUInteger bpi, MTLSize sz,
                      id dst, NSUInteger ds, NSUInteger dl, MTLOrigin dor)
{
    SPY(@"blit buf->tex buf%p+%lu bpr%lu -> %@ %lux%lu", src, (unsigned long)off, (unsigned long)bpr, texDesc(dst),
        (unsigned long)sz.width, (unsigned long)sz.height);
    ((void (*)(id, SEL, id, NSUInteger, NSUInteger, NSUInteger, MTLSize, id, NSUInteger, NSUInteger, MTLOrigin))o_blitB2T)(
        self, s, src, off, bpr, bpi, sz, dst, ds, dl, dor);
}

static void h_genMips(id self, SEL s, id t)
{
    SPY(@"blit generateMipmaps %@", texDesc(t));
    ((void (*)(id, SEL, id))o_genMips)(self, s, t);
}

static void h_commit(id<MTLCommandBuffer> self, SEL s)
{
    SPY(@"cb %p commit label=%@", self, self.label);
    NSArray *targets = objc_getAssociatedObject(self, &kTargets);
    [self addCompletedHandler:^(id<MTLCommandBuffer> cb) {
        if (cb.status != MTLCommandBufferStatusCompleted || cb.error)
            SPY(@"cb %p status %ld error %@", cb, (long)cb.status, cb.error);
        // what each render target of this command buffer holds now: pixels with any non-zero byte
        for (id<MTLTexture> t in targets) {
            const NSUInteger bpp = bppOf(t.pixelFormat), bpr = t.width * bpp;
            NSMutableData *buf = [NSMutableData dataWithLength:bpr * t.height];
            ((void (*)(id, SEL, void *, NSUInteger, MTLRegion, NSUInteger))o_getBytes)(t,
                @selector(getBytes:bytesPerRow:fromRegion:mipmapLevel:), buf.mutableBytes, bpr,
                MTLRegionMake2D(0, 0, t.width, t.height), 0);
            const uint8_t *p = buf.bytes;
            size_t px = 0;
            for (NSUInteger i = 0; i < t.width * t.height; i++) {
                int any = 0;
                for (NSUInteger k = 0; k < bpp; k++) any |= p[i * bpp + k];
                px += any != 0;
            }
            SPY(@"cb %p content %@ nonzero px %zu/%lu", cb, texDesc(t), px, (unsigned long)(t.width * t.height));
            static int cdumps;
            if (getenv("MTLSPY_CONTENT_DUMP") && cdumps < 64) {   // raw texels, in the process's temp dir
                NSString *f = [NSTemporaryDirectory() stringByAppendingFormat:@"mtlspy-c%02d-f%lu-%lux%lu.raw", cdumps++,
                               (unsigned long)t.pixelFormat, (unsigned long)t.width, (unsigned long)t.height];
                [buf writeToFile:f atomically:NO];
                SPY(@"dumped %@", f);
            }
        }
    }];
    ((void (*)(id, SEL))o_commit)(self, s);
}


// Generic tracer: every instance method of a class (own + MTLIOAccel superclasses, not NSObject)
// is routed through forwardInvocation:, which logs the selector once per call and runs the
// original under a "spy_" alias. Any signature works, so it catches the selectors we did not
// think of. Hooked methods above keep their typed hooks; skip them here.
static NSMutableSet *gTyped;

static void spyForward(id self, SEL _cmd, NSInvocation *inv)
{
    SEL orig = inv.selector;
    SPY(@"call %s -%@", class_getName(object_getClass(self)), NSStringFromSelector(orig));
    inv.selector = NSSelectorFromString([@"spy_" stringByAppendingString:NSStringFromSelector(orig)]);
    [inv invoke];
}

static void traceClass(Class target)
{
    if (!target) return;
    class_addMethod(target, @selector(forwardInvocation:), (IMP)spyForward, "v@:@");
    for (Class c = target; c && c != [NSObject class]; c = class_getSuperclass(c)) {
        unsigned n = 0;
        Method *ms = class_copyMethodList(c, &n);
        for (unsigned i = 0; i < n; i++) {
            SEL sel = method_getName(ms[i]);
            NSString *name = NSStringFromSelector(sel);
            if ([name hasPrefix:@"spy_"] || [name hasPrefix:@"."] || [name hasPrefix:@"_"] || [gTyped containsObject:name] ||
                [name isEqualToString:@"dealloc"] || [name isEqualToString:@"forwardInvocation:"] ||
                [name hasPrefix:@"retain"] || [name hasPrefix:@"release"] || [name isEqualToString:@"autorelease"] ||
                [name hasPrefix:@"respondsTo"] || [name hasPrefix:@"methodSignature"] || [name hasPrefix:@"is"] ||
                [name isEqualToString:@"device"] || [name hasPrefix:@"set"] || [name hasPrefix:@"label"])
                continue;
            SEL alias = NSSelectorFromString([@"spy_" stringByAppendingString:name]);
            if (class_getInstanceMethod(target, alias)) continue;
            Method m = class_getInstanceMethod(target, sel);
            const char *types = method_getTypeEncoding(m);
            if (!types || types[0] == '{') continue;   // struct returns need _objc_msgForward_stret on x86_64
            class_addMethod(target, alias, method_getImplementation(m), types);
            if (!class_addMethod(target, sel, _objc_msgForward, types))
                class_replaceMethod(target, sel, _objc_msgForward, types);
        }
        free(ms);
    }
}

static void hookAll(void)
{
    Class dev = objc_getClass("NVMTLDevice"), tex = objc_getClass("NVMTLTexture"),
          cb = objc_getClass("NVMTLCommandBuffer"), blit = objc_getClass("NVMTLBlitEncoder");
    o_newTex = swap(dev, @selector(newTextureWithDescriptor:), (IMP)h_newTex);
    o_newDSS = swap(dev, @selector(newDepthStencilStateWithDescriptor:), (IMP)h_newDSS);
    o_newRPS = swap(dev, @selector(newRenderPipelineStateWithDescriptor:error:), (IMP)h_newRPS);
    o_newTexIOS = swap(dev, @selector(newTextureWithDescriptor:iosurface:plane:), (IMP)h_newTexIOS);
    o_getBytes = swap(tex, @selector(getBytes:bytesPerRow:fromRegion:mipmapLevel:), (IMP)h_getBytes);
    o_getBytes2 = swap(tex, @selector(getBytes:bytesPerRow:bytesPerImage:fromRegion:mipmapLevel:slice:), (IMP)h_getBytes2);
    o_rpe = swap(cb, @selector(renderCommandEncoderWithDescriptor:), (IMP)h_rpe);
    o_commit = swap(cb, @selector(commit), (IMP)h_commit);
    o_blitT2T = swap(blit, @selector(copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:), (IMP)h_blitT2T);
    o_blitT2B = swap(blit, @selector(copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toBuffer:destinationOffset:destinationBytesPerRow:destinationBytesPerImage:), (IMP)h_blitT2B);
    o_blitB2T = swap(blit, @selector(copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:), (IMP)h_blitB2T);
    o_genMips = swap(blit, @selector(generateMipmapsForTexture:), (IMP)h_genMips);
    (void)o_present;
    gTyped = [NSMutableSet setWithArray:@[ @"copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:",
        @"copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toBuffer:destinationOffset:destinationBytesPerRow:destinationBytesPerImage:",
        @"copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:",
        @"generateMipmapsForTexture:" ]];
    if (getenv("MTLSPY_GENERIC")) {
        for (NSString *c in [@(getenv("MTLSPY_GENERIC")) componentsSeparatedByString:@","]) traceClass(objc_getClass(c.UTF8String));
    }
    SPY(@"hooked NVMTL classes");
}

static void hookOnce(void)
{
    static dispatch_once_t once;
    if (!objc_getClass("NVMTLDevice")) return;
    dispatch_once(&once, ^{ hookAll(); });
}

// dyld's add-image callback runs before objc registers the bundle's classes, so hook right after
// the process first asks Metal for a device instead.
#define INTERPOSE(r, o) __attribute__((used)) static struct { const void *n, *o; } _i_##o \
    __attribute__((section("__DATA,__interpose"))) = { (const void *)r, (const void *)o };
static id<MTLDevice> my_default(void) { id<MTLDevice> d = MTLCreateSystemDefaultDevice(); hookOnce(); return d; }
static NSArray *my_all(void) { NSArray *a = MTLCopyAllDevices(); hookOnce(); return a; }
INTERPOSE(my_default, MTLCreateSystemDefaultDevice)
INTERPOSE(my_all, MTLCopyAllDevices)
