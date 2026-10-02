// Integration map of the Metal driver: makes one of every object type through
// the default device and checks each method of its Metal protocols (required
// and optional, inherited protocols included).
//   MISSING  nothing answers the selector (a caller gets the zero fallback or
//            a crash)
//   BASE     answered by Apple's base class (Metal / IOAccelerator code), not
//            by NVMTLDriver: works only if that code path fits our objects
// metal_coverage [-v]    (-v: list BASE methods too)
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <string.h>

static int gVerbose;
static unsigned gMissing, gBase, gOurs, gFallback;

static const char *imageOf(Class c, SEL s) {
    Method m = class_getInstanceMethod(c, s);
    if (!m) return NULL;
    Dl_info di;
    if (!dladdr((const void *)method_getImplementation(m), &di) || !di.dli_fname) return "?";
    const char *slash = strrchr(di.dli_fname, '/');
    return slash ? slash + 1 : di.dli_fname;
}

static void checkProtocol(id obj, Protocol *p, NSMutableSet<NSString *> *seen, const char *what) {
    unsigned np = 0;
    Protocol * __unsafe_unretained *inh = protocol_copyProtocolList(p, &np);
    for (unsigned i = 0; i < np; i++)
        if (strcmp(protocol_getName(inh[i]), "NSObject")) checkProtocol(obj, inh[i], seen, what);
    free(inh);
    for (int req = 1; req >= 0; req--) {
        unsigned n = 0;
        struct objc_method_description *d = protocol_copyMethodDescriptionList(p, req, YES, &n);
        for (unsigned i = 0; i < n; i++) {
            NSString *name = NSStringFromSelector(d[i].name);
            if ([seen containsObject:name]) continue;
            [seen addObject:name];
            if (![obj respondsToSelector:d[i].name]) {
                gMissing++;
                printf("MISSING %-26s %s -%s\n", what, req ? "required" : "optional", name.UTF8String);
                continue;
            }
            const char *img = imageOf([obj class], d[i].name);
            if (img && strstr(img, "NVMTLDriver")) { gOurs++; continue; }
            // _objc_msgForward: the driver's zero-returning fallback, i.e. not implemented
            if (!img || strstr(img, "libobjc")) {
                gFallback++;
                printf("FALLBACK %-25s %s -%s\n", what, req ? "required" : "optional", name.UTF8String);
                continue;
            }
            gBase++;
            if (gVerbose) printf("BASE    %-26s %s -%s  (%s)\n", what, req ? "required" : "optional", name.UTF8String,
                                 img ? img : "forwarded");
        }
        free(d);
    }
}

static void check(id obj, const char *proto, const char *what) {
    if (!obj) { printf("NOOBJ   %-26s could not be created\n", what); gMissing++; return; }
    Protocol *p = objc_getProtocol(proto);
    if (!p) { printf("NOPROTO %s\n", proto); return; }
    printf("-- %s (%s, %s)\n", what, class_getName([obj class]), proto);
    checkProtocol(obj, p, [NSMutableSet new], what);
}

int main(int argc, char **argv) {
    @autoreleasepool {
        gVerbose = argc > 1 && !strcmp(argv[1], "-v");
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { puts("no Metal device"); return 1; }
        check(dev, "MTLDevice", "device");
        id<MTLBuffer> buf = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
        check(buf, "MTLBuffer", "buffer");
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:64 height:64 mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
        check(tex, "MTLTexture", "texture");
        MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
        hd.size = 1 << 20;
        hd.storageMode = MTLStorageModePrivate;
        check([dev newHeapWithDescriptor:hd], "MTLHeap", "heap");
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        check([dev newSamplerStateWithDescriptor:sd], "MTLSamplerState", "sampler");
        MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
        check([dev newDepthStencilStateWithDescriptor:dd], "MTLDepthStencilState", "depth stencil state");
        check([dev newFence], "MTLFence", "fence");
        check([dev newEvent], "MTLEvent", "event");
        check([dev newSharedEvent], "MTLSharedEvent", "shared event");
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "kernel void k(device float *o [[buffer(0)]], uint i [[thread_position_in_grid]]) { o[i] = i; }\n"
             "struct V { float4 p [[position]]; };\n"
             "vertex V vs(uint v [[vertex_id]]) { V o; o.p = float4(v, 0, 0, 1); return o; }\n"
             "fragment half4 fs() { return half4(1); }\n"
                                               options:nil error:&e];
        check(lib, "MTLLibrary", "library");
        id<MTLFunction> kf = [lib newFunctionWithName:@"k"];
        check(kf, "MTLFunction", "function");
        check(kf ? [dev newComputePipelineStateWithFunction:kf error:&e] : nil, "MTLComputePipelineState",
              "compute pipeline");
        MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
        rd.vertexFunction = [lib newFunctionWithName:@"vs"];
        rd.fragmentFunction = [lib newFunctionWithName:@"fs"];
        rd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        check(lib ? [dev newRenderPipelineStateWithDescriptor:rd error:&e] : nil, "MTLRenderPipelineState",
              "render pipeline");
        id<MTLCommandQueue> q = [dev newCommandQueue];
        check(q, "MTLCommandQueue", "command queue");
        id<MTLCommandBuffer> cb = [q commandBuffer];
        check(cb, "MTLCommandBuffer", "command buffer");
        id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        check(ce, "MTLComputeCommandEncoder", "compute encoder");
        [ce endEncoding];
        id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
        check(be, "MTLBlitCommandEncoder", "blit encoder");
        [be endEncoding];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = tex;
        id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
        check(re, "MTLRenderCommandEncoder", "render encoder");
        [re endEncoding];
        id<MTLParallelRenderCommandEncoder> pe = [cb parallelRenderCommandEncoderWithDescriptor:rp];
        check(pe, "MTLParallelRenderCommandEncoder", "parallel render encoder");
        [pe endEncoding];
        MTLIndirectCommandBufferDescriptor *icd = [MTLIndirectCommandBufferDescriptor new];
        icd.commandTypes = MTLIndirectCommandTypeConcurrentDispatch;
        icd.maxKernelBufferBindCount = 4;
        check([dev newIndirectCommandBufferWithDescriptor:icd maxCommandCount:4 options:0], "MTLIndirectCommandBuffer",
              "indirect command buffer");
        MTLArgumentDescriptor *ad = [MTLArgumentDescriptor argumentDescriptor];
        ad.dataType = MTLDataTypePointer;
        check([dev newArgumentEncoderWithArguments:@[ad]], "MTLArgumentEncoder", "argument encoder");
        [cb commit];
        [cb waitUntilCompleted];
        // private and newer selectors the protocols do not list: every
        // creation method of Apple's device classes that NVMTLDevice does not
        // override goes to Apple's code (often the vendor compiler service,
        // which has nothing for us; MetalFX died on one of these)
        unsigned hz = 0;
        const char *classes[2] = {"_MTLDevice", "MTLIOAccelDevice"};
        for (int ci = 0; ci < 2; ci++) {
            const char *cn = classes[ci];
            Class c = objc_getClass(cn);
            unsigned n = 0;
            Method *ms = class_copyMethodList(c, &n);
            for (unsigned i = 0; i < n; i++) {
                const char *name = sel_getName(method_getName(ms[i]));
                if (strncmp(name, "new", 3) || !strstr(name, "With")) continue;
                if (!strstr(name, "Pipeline") && !strstr(name, "Library") && !strstr(name, "Function") &&
                    !strstr(name, "Texture") && !strstr(name, "Buffer") && !strstr(name, "Event") &&
                    !strstr(name, "Archive") && !strstr(name, "Heap") && !strstr(name, "Sampler")) continue;
                const char *img = imageOf([dev class], method_getName(ms[i]));
                if (img && strstr(img, "NVMTLDriver")) continue;
                hz++;
                printf("BASE-HAZARD %-18s -%s\n", cn, name);
            }
            free(ms);
        }
        printf("\n%u MISSING, %u FALLBACK (zero answer), %u from Apple's base classes, %u ours, %u base-hazard creators\n",
               gMissing, gFallback,
               gBase, gOurs, hz);
        return gMissing || gFallback ? 1 : 0;
    }
}
