// RTX-only native submission rejection. Change only this process's marked
// command-buffer packet to an unaligned segment VA. execSegmentValid refuses
// it before ring writes/kick; this does not inject a GPU fault or reset.
// The last rejected command must report Error without a later good submit
// being needed, and earlier accepted work must retain correct output.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    uint32_t type, size, engine, n, tag, reserved;
    uint64_t va;
    uint32_t dwords, flags;
} Packet;
static char armedKey, packetKey;
static IMP reserveOriginal, endOriginal;
static unsigned injected;

static void *reserveHook(id cb, SEL sel, unsigned long bytes) {
    void *p = ((void *(*)(id, SEL, unsigned long))reserveOriginal)(cb, sel, bytes);
    if (p && bytes == sizeof(Packet) && objc_getAssociatedObject(cb, &armedKey))
        objc_setAssociatedObject(cb, &packetKey, [NSValue valueWithPointer:p], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return p;
}

static void endHook(id cb, SEL sel) {
    Packet *p = [objc_getAssociatedObject(cb, &packetKey) pointerValue];
    if (p && objc_getAssociatedObject(cb, &armedKey) && p->type == 0x10000 &&
        p->size == sizeof(Packet) && p->engine == 0 && p->n == 1 && p->dwords && !(p->va & 3)) {
        p->va |= 1; // known host validation failure; never sent to GPU
        ++injected;
        printf("reject packet: unaligned VA 0x%llx, words %u\n", (unsigned long long)p->va, p->dwords);
        objc_setAssociatedObject(cb, &armedKey, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    objc_setAssociatedObject(cb, &packetKey, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    ((void (*)(id, SEL))endOriginal)(cb, sel);
}

static BOOL installHooks(id cb) {
    Class cls = object_getClass(cb);
    SEL reserve = NSSelectorFromString(@"_reserveKernelCommandBufferSpace:");
    SEL end = NSSelectorFromString(@"endCurrentSegment");
    Method rm = class_getInstanceMethod(cls, reserve), em = class_getInstanceMethod(cls, end);
    if (!rm || !em) return NO;
    reserveOriginal = method_getImplementation(rm); endOriginal = method_getImplementation(em);
    // Install on the concrete class, keeping inherited methods/processes intact.
    class_replaceMethod(cls, reserve, (IMP)reserveHook, method_getTypeEncoding(rm));
    class_replaceMethod(cls, end, (IMP)endHook, method_getTypeEncoding(em));
    return YES;
}

static id<MTLCommandBuffer> encode(id<MTLCommandQueue> q, id<MTLComputePipelineState> ps,
                                  id<MTLBuffer> b, uint32_t seed) {
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:ps]; [ce setBuffer:b offset:0 atIndex:0];
    [ce setBytes:&seed length:sizeof seed atIndex:1];
    [ce dispatchThreads:MTLSizeMake(b.length / 4, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [ce endEncoding]; return cb;
}

int main(void) {
    alarm(30);
    setbuf(stdout, NULL);
    @autoreleasepool {
        id<MTLDevice> d = MTLCreateSystemDefaultDevice();
        if (![d.name containsString:@"NVIDIA"]) { puts("RTX required"); return 2; }
        NSError *err = nil;
        id<MTLLibrary> lib = [d newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal; kernel void fill(device uint *a [[buffer(0)]], constant uint &seed [[buffer(1)]], uint i [[thread_position_in_grid]]) { a[i]=i*3u+seed; }" options:nil error:&err];
        id<MTLComputePipelineState> ps = [d newComputePipelineStateWithFunction:[lib newFunctionWithName:@"fill"] error:&err];
        id<MTLCommandQueue> q = [d newCommandQueue];
        id<MTLCommandBuffer> dummy = [q commandBuffer];
        if (!ps || !q || !installHooks(dummy)) { printf("setup failed: %s\n", err.description.UTF8String); return 3; }
        unsigned wrong = 0;
        for (unsigned round = 0; round < 4; ++round) {
            id<MTLBuffer> a = [d newBufferWithLength:262144 * 4 options:MTLResourceStorageModeShared];
            id<MTLBuffer> b = [d newBufferWithLength:262144 * 4 options:MTLResourceStorageModeShared];
            id<MTLBuffer> bad = [d newBufferWithLength:4096 * 4 options:MTLResourceStorageModeShared];
            if (!a || !b || !bad) return 4;
            memset(a.contents, 0, a.length); memset(b.contents, 0, b.length);
            for (unsigned i = 0; i < 4096; ++i) ((uint32_t *)bad.contents)[i] = 0xdec0adde;
            uint32_t sa = 101 + round, sb = 201 + round;
            id<MTLCommandBuffer> ca = encode(q, ps, a, sa), cb = encode(q, ps, b, sb), rejected = encode(q, ps, bad, 301 + round);
            objc_setAssociatedObject(rejected, &armedKey, @YES, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            NSDate *start = [NSDate date];
            [ca commit]; [cb commit]; [rejected commit];
            [rejected waitUntilCompleted]; // no later accepted submit used for retirement
            double elapsed = -[start timeIntervalSinceNow];
            [ca waitUntilCompleted]; [cb waitUntilCompleted];
            unsigned outputWrong = 0;
            for (unsigned i = 0; i < 262144; ++i) {
                outputWrong += ((uint32_t *)a.contents)[i] != i * 3u + sa;
                outputWrong += ((uint32_t *)b.contents)[i] != i * 3u + sb;
            }
            for (unsigned i = 0; i < 4096; ++i) outputWrong += ((uint32_t *)bad.contents)[i] != 0xdec0adde;
            BOOL ok = ca.status == MTLCommandBufferStatusCompleted && cb.status == MTLCommandBufferStatusCompleted &&
                rejected.status == MTLCommandBufferStatusError && rejected.error && elapsed < 5 && !outputWrong;
            printf("round %u: accepted=%ld,%ld rejected=%ld error=%ld %.3fs wrong=%u: %s\n", round + 1,
                   (long)ca.status, (long)cb.status, (long)rejected.status, (long)rejected.error.code,
                   elapsed, outputWrong, ok ? "PASS" : "FAIL");
            wrong += !ok;
        }
        BOOL ok = !wrong && injected == 4;
        printf("native rejection: injected=%u failing rounds=%u: %s\n", injected, wrong, ok ? "PASS" : "FAIL");
        return !ok;
    }
}
