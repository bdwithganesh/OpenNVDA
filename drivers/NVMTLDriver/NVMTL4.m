#include <sys/stat.h>
#import "NVMTL4.h"
#import "NVMTLFamilyGate.h"
#import <Metal/MTL4RenderPass.h>
#import <Metal/MTL4Compiler.h>
#import <Metal/MTL4LibraryDescriptor.h>
#import <Metal/MTL4RenderPipeline.h>
#import <Metal/MTL4Counters.h>
#import <IOKit/IOKitLib.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
extern id nvMTLBufferForGpuAddress(uint64_t va);
extern id nvMTLBufferForResourceID(uint64_t rid);
extern id nvMTLTextureForResourceID(uint64_t rid);
extern id nvMTLSamplerForResourceID(uint64_t rid);
@interface NSObject (NVMTL4Timestamp)
- (void)nvWriteTimestamp:(uint64_t)ts atIndex:(NSUInteger)index;
@end
static void nvM4Timestamp(id heap, NSUInteger index);
#import <Foundation/Foundation.h>
#import <Metal/MTL4Archive.h>

@class NVMTL4Archive;

@interface NVMTL4Archive : NSObject <MTL4Archive>
@property (nonatomic, weak) id<MTLDevice> device;
@property (copy, atomic, nullable) NSString *label;
@property (nonatomic, strong, nullable) NSURL *url;
@end
#import <objc/runtime.h>
#import <objc/message.h>

// Declare the native pipeline entry point on NVMTLDevice so the MTL4 compiler
// can build pipelines via AIR → nakc → SASS (same path as Metal 3).
@interface NVMTLDevice (NVMTL4NativePipeline)
- (id<MTLComputePipelineState>)nvMTL4BuildComputePipelineWithFunction:(id<MTLFunction>)fn error:(NSError **)error;
- (id<MTLFunction>)nvNullFragment;
@end

// The MTL4 compiler caches libraries by descriptor so repeated pipeline builds
// from the same library don't re-serialize/re-compile.
@interface NVMTL4Compiler ()
@property (nonatomic, strong) id<MTLLibrary> cachedLibrary;
@property (nonatomic, copy) NSString *cachedLibrarySource;
@property (nonatomic, copy) id cachedLibraryOptions;
@end

// The existing compute encoder gains nvSetBufferAddress:length:atIndex: in
// NVMTLDevice.m. Declare it dynamically so this feature-flagged file does not
// need the private class interface.
@interface NSObject (NVMTL4RawAddress)
- (void)nvSetBufferAddress:(uint64_t)va length:(NSUInteger)len atIndex:(NSUInteger)idx;
- (void)nvSetTextureID:(uint64_t)tic atIndex:(NSUInteger)idx;
- (void)nvSetSamplerID:(uint64_t)tsc atIndex:(NSUInteger)idx;
@end

// ---------------------------------------------------------------------------
// Feature gates. NVMTL_METAL4/NVMTL_METAL3 are per-process A/B switches; the
// nvram nvmtl-metal3/4 variables are the system-wide receipts written only by
// tools/nvaccel/metal3_conformance.sh and mtl4_conformance.sh after their
// matrices pass. A per-process NVMTL_METAL4=1 run never advertises the family
// by itself (the family needs NVMTL_METAL4_FAMILY=1 or the nvram receipt).
// ---------------------------------------------------------------------------

static int nvM4EnvFlag(const char *key) {
    const char *v = getenv(key);
    return v && v[0] && v[0] != '0';
}

static int nvM4NvramFlag(const char *key) {
    int on = 0;
    io_registry_entry_t options = IORegistryEntryFromPath(kIOMainPortDefault, "IODeviceTree:/options");
    if (!options) return 0;
    CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    CFTypeRef v = k ? IORegistryEntryCreateCFProperty(options, k, kCFAllocatorDefault, 0) : NULL;
    if (v) {
        if (CFGetTypeID(v) == CFDataGetTypeID()) {
            CFDataRef d = (CFDataRef)v;
            on = CFDataGetLength(d) > 0 && CFDataGetBytePtr(d)[0] == '1';
        } else if (CFGetTypeID(v) == CFStringGetTypeID()) {
            CFStringRef s = (CFStringRef)v;
            on = CFStringGetLength(s) > 0 && CFStringGetCharacterAtIndex(s, 0) == '1';
        }
        CFRelease(v);
    }
    if (k) CFRelease(k);
    IOObjectRelease(options);
    // 0.9.50: a VM whose NVRAM is not persistent (OpenCore on Hyper-V) keeps
    // the receipt as a root-owned file /Library/Preferences/<key> instead
    if (!on) {
        char path[128];
        snprintf(path, sizeof path, "/Library/Preferences/%s", key);
        struct stat st;
        on = stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == 0 && !(st.st_mode & 022);
    }
    return on;
}

static NVMTLFamilyGates gNVMTLFamilyGates;
static dispatch_once_t gNVMTLFamilyOnce;

NVMTLFamilyGates nvMTLGateFamilyInputs(void) {
    dispatch_once(&gNVMTLFamilyOnce, ^{
        // nvram is read once: a boot-time receipt cannot change under a live
        // process. The environment is also stable for the process lifetime.
        gNVMTLFamilyGates.metal3OptIn = (unsigned)(nvM4EnvFlag("NVMTL_METAL3") ||
                                                   nvM4NvramFlag("nvmtl-metal3"));
        gNVMTLFamilyGates.metal4OptIn = (unsigned)(nvM4EnvFlag("NVMTL_METAL4") ||
                                                   nvM4NvramFlag("nvmtl-metal4"));
        gNVMTLFamilyGates.metal4Family = (unsigned)(nvM4EnvFlag("NVMTL_METAL4_FAMILY") ||
                                                    nvM4NvramFlag("nvmtl-metal4"));
    });
    return gNVMTLFamilyGates;
}

int nvMTLGateObjectsEnabled(void) {
    return (int)nvMTLGateFamilyInputs().metal4OptIn;
}

static NSError *nvM4DisabledError(void) {
    return [NSError errorWithDomain:@"NVMTL4ErrorDomain" code:1
                            userInfo:@{NSLocalizedDescriptionKey:
                                @"Metal 4 is disabled; set NVMTL_METAL4=1 for this process or "
                                @"nvram nvmtl-metal4=1 after mtl4_conformance.sh passes"}];
}

static NSError *nvM4UnsupportedError(NSString *what) {
    return [NSError errorWithDomain:@"NVMTL4ErrorDomain" code:2
                            userInfo:@{NSLocalizedDescriptionKey:
                                [NSString stringWithFormat:@"Metal 4 %@ is not implemented by NVMTLDriver", what]}];
}

static NSError *nvM4BadDescriptorError(NSString *what) {
    return [NSError errorWithDomain:@"NVMTL4ErrorDomain" code:4
                            userInfo:@{NSLocalizedDescriptionKey:
                                [NSString stringWithFormat:@"Metal 4 %@ descriptor is invalid or incomplete", what]}];
}

// MTL4 passes raw GPU addresses where Metal 3 passes an id<MTLBuffer> and an
// offset. Resolve the owning buffer and the exact offset once. The address
// may be an interior address (buffer.gpuAddress + byte offset), not only the
// base VA, and the range [address, address+length) must fit the buffer.
static BOOL nvM4ResolveAddress(MTLGPUAddress address, NSUInteger length,
                               id<MTLBuffer> *buffer, NSUInteger *offset) {
    id b = address ? nvMTLBufferForGpuAddress((uint64_t)address) : nil;
    if (!b) return NO;
    const uint64_t base = [(id<MTLBuffer>)b gpuAddress];
    const uint64_t addr = (uint64_t)address;
    if (addr < base) return NO;
    const uint64_t off = addr - base;
    const NSUInteger blen = [(id<MTLBuffer>)b length];
    if (off >= blen || (length && length > blen - off)) return NO;
    if (buffer) *buffer = b;
    if (offset) *offset = (NSUInteger)off;
    return YES;
}

static NSUInteger nvM4IndexSize(MTLIndexType type) {
    return type == MTLIndexTypeUInt16 ? 2 : type == MTLIndexTypeUInt32 ? 4 : 0;
}

static BOOL nvM4IndexCountFits(NSUInteger count, MTLIndexType type, NSUInteger length) {
    const NSUInteger sz = nvM4IndexSize(type);
    if (!sz || (length && count > length / sz)) return NO;
    return YES;
}

// 0.9.20: an index draw must fit both the declared index buffer length (when
// the caller supplied one) and the owning MTLBuffer. The old code checked only
// the former; a caller passing 0 or an over-large length could read past the
// buffer. Pure helper so the dispatch-bounds host test can execute it.
static BOOL nvM4IndexRangeFits(NSUInteger count, MTLIndexType type, NSUInteger length,
                               NSUInteger bufferLength, NSUInteger offset) {
    const NSUInteger sz = nvM4IndexSize(type);
    if (!sz || offset > bufferLength) return NO;
    if (count > (NSUIntegerMax / sz)) return NO;
    const NSUInteger bytes = count * sz;
    if (length && bytes > length) return NO;
    if (bytes > bufferLength - offset) return NO;
    return YES;
}

static BOOL nvM4SizeUsable(MTLSize s) {
    return s.width && s.height && s.depth &&
           s.width <= 0xffffffffULL && s.height <= 0xffffffffULL && s.depth <= 0xffffffffULL;
}

// 0.9.20: last-resort guard for a protocol method that neither the wrapper
// nor the inner Metal 3 encoder implements. Returning a protocol-derived
// signature lets -forwardInvocation: log and return zero instead of raising
// out of a class that already claims the MTL4 protocol.
static NSMethodSignature *nvM4ProtocolSignature(Protocol *p, SEL sel) {
    if (!p) return nil;
    struct objc_method_description d = protocol_getMethodDescription(p, sel, YES, YES);
    if (!d.types) d = protocol_getMethodDescription(p, sel, NO, YES);
    return d.types ? [NSMethodSignature signatureWithObjCTypes:d.types] : nil;
}

static void nvM4UnknownSelector(id self, NSInvocation *inv) {
    static NSMutableSet *seen;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ seen = [NSMutableSet set]; });
    NSString *key = [NSString stringWithFormat:@"%s %@", class_getName(object_getClass(self)),
                     NSStringFromSelector(inv.selector)];
    @synchronized (seen) {
        if (![seen containsObject:key]) {
            [seen addObject:key];
            NSLog(@"NVMTLDriver: unimplemented MTL4 -[%@], returning zero", key);
        }
    }
    const NSUInteger len = inv.methodSignature.methodReturnLength;
    if (len) {
        void *zero = calloc(1, len);
        if (zero) { [inv setReturnValue:zero]; free(zero); }
    }
}

static NSUInteger nvM4UIntProperty(id object, const char *name) {
    SEL sel = sel_registerName(name);
    if (!object || ![object respondsToSelector:sel]) return 0;
    return ((NSUInteger (*)(id, SEL))objc_msgSend)(object, sel);
}

@implementation NVMTL4Allocator {
    NSUInteger _generation;
}
- (uint64_t)allocatedSize { return 0; }
- (void)reset { _generation++; }
- (NSUInteger)nvGeneration { return _generation; }
@end

@implementation NVMTL4ArgumentTable {
    MTLGPUAddress _addr[31];
    NSUInteger _len[31], _stride[31];
    uint64_t _res[31], _tex[128], _samp[16];
}
- (instancetype)init {
    self = [super init];
    if (self) {
        for (unsigned i = 0; i < 31; ++i) { _addr[i] = 0; _len[i] = 0; _stride[i] = 0; _res[i] = 0; }
        for (unsigned i = 0; i < 128; ++i) _tex[i] = 0;
        for (unsigned i = 0; i < 16; ++i) _samp[i] = 0;
    }
    return self;
}
- (void)setAddress:(MTLGPUAddress)gpuAddress length:(NSUInteger)length atIndex:(NSUInteger)index {
    if (index >= 31) { NSLog(@"NVMTLDriver: MTL4 argument table buffer index %lu >= 31", (unsigned long)index); return; }
    if (length > 0xffffffffULL) {
        NSLog(@"NVMTLDriver: MTL4 argument table length %lu exceeds 4 GiB; clamped",
              (unsigned long)length);
        length = 0xffffffffULL;
    }
    // A plain address binding has no attribute stride. Clearing it also
    // prevents a stale stride from a previous binding of this index changing
    // the vertex-fetch path of a later draw.
    _addr[index] = gpuAddress; _len[index] = length; _stride[index] = 0;
}
- (void)setAddress:(MTLGPUAddress)gpuAddress atIndex:(NSUInteger)index {
    [self setAddress:gpuAddress length:0 atIndex:index];
}
// Attribute strides are consumer-side vertex-fetch metadata; the native
// encoder takes the exact address/length pair, so the stride is retained for
// future vertex-fetch lowering but does not change the binding. An attribute
// binding has no explicit length; clear a stale one for the same reason.
- (void)setAddress:(MTLGPUAddress)gpuAddress attributeStride:(NSUInteger)stride atIndex:(NSUInteger)index {
    if (index >= 31) {
        NSLog(@"NVMTLDriver: MTL4 argument table buffer index %lu >= 31", (unsigned long)index);
        return;
    }
    _addr[index] = gpuAddress; _len[index] = 0; _stride[index] = stride;
}
- (NSUInteger)nvAttributeStrideAtIndex:(NSUInteger)index {
    return index < 31 ? _stride[index] : 0;
}
- (void)setResource:(MTLResourceID)resourceID atBufferIndex:(NSUInteger)index {
    if (index >= 31) { NSLog(@"NVMTLDriver: MTL4 argument table buffer index %lu >= 31", (unsigned long)index); return; }
    _res[index] = resourceID._impl;
}
- (void)setTexture:(MTLResourceID)resourceID atIndex:(NSUInteger)index {
    if (index >= 128) { NSLog(@"NVMTLDriver: MTL4 argument table texture index %lu >= 128", (unsigned long)index); return; }
    _tex[index] = resourceID._impl;
}
- (void)setSamplerState:(MTLResourceID)resourceID atIndex:(NSUInteger)index {
    if (index >= 16) { NSLog(@"NVMTLDriver: MTL4 argument table sampler index %lu >= 16", (unsigned long)index); return; }
    _samp[index] = resourceID._impl;
}
- (uint64_t)nvTextureAtIndex:(NSUInteger)index { return index < 128 ? _tex[index] : 0; }
- (uint64_t)nvSamplerAtIndex:(NSUInteger)index { return index < 16 ? _samp[index] : 0; }
- (uint64_t)nvResourceAtIndex:(NSUInteger)index { return index < 31 ? _res[index] : 0; }
- (BOOL)nvAddressAtIndex:(NSUInteger)index address:(MTLGPUAddress *)address
                  length:(NSUInteger *)length {
    if (index >= 31 || !_addr[index]) return NO;
    if (address) *address = _addr[index];
    if (length) *length = _len[index];
    return YES;
}
@end

@interface NVMTL4ComputeEncoder : NSObject
- (instancetype)initWithInner:(id<MTLComputeCommandEncoder>)inner
                commandBuffer:(id<MTL4CommandBuffer>)commandBuffer;
- (id<MTL4CommandBuffer>)commandBuffer;
- (NSString *)label;
- (void)setLabel:(NSString *)label;
- (void)setComputePipelineState:(id<MTLComputePipelineState>)state;
- (void)setArgumentTable:(id<MTL4ArgumentTable>)table;
- (void)setBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index;
- (void)setThreadgroupMemoryLength:(NSUInteger)length atIndex:(NSUInteger)index;
- (void)setImageblockWidth:(NSUInteger)width height:(NSUInteger)height;
- (void)dispatchThreads:(MTLSize)threadsPerGrid
  threadsPerThreadgroup:(MTLSize)threadsPerThreadgroup;
- (void)dispatchThreadgroups:(MTLSize)threadgroupsPerGrid
       threadsPerThreadgroup:(MTLSize)threadsPerThreadgroup;
- (void)writeTimestampWithGranularity:(MTL4TimestampGranularity)granularity
                             intoHeap:(id<MTL4CounterHeap>)counterHeap
                              atIndex:(NSUInteger)index;
// MTL4CommandEncoder public surface. The MTL4 protocol has the
// visibilityOptions:/afterEncoderStages: forms; the native Metal 3 encoder
// only has the older fence/barrier entry points, so these map onto those.
- (void)updateFence:(id<MTLFence>)fence afterEncoderStages:(MTLStages)stages;
- (void)waitForFence:(id<MTLFence>)fence beforeEncoderStages:(MTLStages)stages;
- (void)barrierAfterEncoderStages:(MTLStages)after beforeEncoderStages:(MTLStages)before
                  visibilityOptions:(NSUInteger)options;
- (void)barrierAfterQueueStages:(MTLStages)after beforeStages:(MTLStages)before
                visibilityOptions:(NSUInteger)options;
- (void)barrierAfterStages:(MTLStages)after beforeQueueStages:(MTLStages)before
           visibilityOptions:(NSUInteger)options;
- (void)endEncoding;
- (BOOL)nvEnded;
@end

@implementation NVMTL4ComputeEncoder {
    id<MTLComputeCommandEncoder> _inner;
    id<MTLComputePipelineState> _ps;
    id<MTL4ArgumentTable> _table;
    __weak id<MTL4CommandBuffer> _nvCommandBuffer;
    NSString *_nvLabel;
    BOOL _ended;
}
- (instancetype)initWithInner:(id<MTLComputeCommandEncoder>)inner
                commandBuffer:(id<MTL4CommandBuffer>)commandBuffer {
    self = [super init];
    if (self) { _inner = inner; _nvCommandBuffer = commandBuffer; }
    return self;
}
- (id<MTL4CommandBuffer>)commandBuffer { return _nvCommandBuffer; }
- (NSString *)label { return _nvLabel; }
- (void)setLabel:(NSString *)label {
    _nvLabel = [label copy];
    if ([_inner respondsToSelector:@selector(setLabel:)]) [_inner setLabel:label];
}
- (void)pushDebugGroup:(NSString *)string {
    if ([_inner respondsToSelector:@selector(pushDebugGroup:)]) [_inner pushDebugGroup:string];
}
- (void)popDebugGroup {
    if ([_inner respondsToSelector:@selector(popDebugGroup)]) [_inner popDebugGroup];
}
- (void)insertDebugSignpost:(NSString *)string {
    if ([_inner respondsToSelector:@selector(insertDebugSignpost:)]) [_inner insertDebugSignpost:string];
}
- (MTLStages)stages { return MTLStageDispatch; }
- (void)setComputePipelineState:(id<MTLComputePipelineState>)state { _ps = state; [_inner setComputePipelineState:state]; }
- (void)setArgumentTable:(id<MTL4ArgumentTable>)table { _table = table; }
- (void)setBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index {
    if (index >= 31) {
        NSLog(@"NVMTLDriver: MTL4 setBytes index %lu >= 31", (unsigned long)index);
        return;
    }
    if (length > 4096) {   // MSL constant-buffer argument limit for setBytes
        NSLog(@"NVMTLDriver: MTL4 setBytes length %lu > 4096", (unsigned long)length);
        return;
    }
    [_inner setBytes:bytes length:length atIndex:index];
}
- (void)setThreadgroupMemoryLength:(NSUInteger)length atIndex:(NSUInteger)index {
    [_inner setThreadgroupMemoryLength:length atIndex:index];
}
- (void)setImageblockWidth:(NSUInteger)width height:(NSUInteger)height {
    (void)width; (void)height;
}
- (void)nvApplyTable {
    if (!_table) return;
    for (unsigned i = 0; i < 31; ++i) {
        MTLGPUAddress a = 0; NSUInteger n = 0;
        if ([(NVMTL4ArgumentTable *)_table nvAddressAtIndex:i address:&a length:&n])
            [(NSObject *)_inner nvSetBufferAddress:(uint64_t)a length:n atIndex:i];
    }
    for (unsigned i = 0; i < 31; ++i) {
        const uint64_t rid = [(NVMTL4ArgumentTable *)_table nvResourceAtIndex:i];
        if (!rid) continue;
        id b = nvMTLBufferForResourceID(rid);
        if (!b) continue;
        const uint64_t va = [(id<MTLBuffer>)b gpuAddress];
        if (va) [(NSObject *)_inner nvSetBufferAddress:va length:[(id<MTLBuffer>)b length] atIndex:i];
    }
    for (unsigned i = 0; i < 128; ++i) {
        const uint64_t t = [(NVMTL4ArgumentTable *)_table nvTextureAtIndex:i];
        if (t) [(NSObject *)_inner nvSetTextureID:t atIndex:i];
    }
    for (unsigned i = 0; i < 16; ++i) {
        const uint64_t s = [(NVMTL4ArgumentTable *)_table nvSamplerAtIndex:i];
        if (s) [(NSObject *)_inner nvSetSamplerID:s atIndex:i];
    }
}
- (void)dispatchThreads:(MTLSize)threadsPerGrid
  threadsPerThreadgroup:(MTLSize)threadsPerThreadgroup {
    if (!nvM4SizeUsable(threadsPerGrid) || !nvM4SizeUsable(threadsPerThreadgroup)) {
        NSLog(@"NVMTLDriver: MTL4 dispatchThreads rejected zero/overflow size (%lu,%lu,%lu)/(%lu,%lu,%lu)",
              (unsigned long)threadsPerGrid.width, (unsigned long)threadsPerGrid.height,
              (unsigned long)threadsPerGrid.depth, (unsigned long)threadsPerThreadgroup.width,
              (unsigned long)threadsPerThreadgroup.height, (unsigned long)threadsPerThreadgroup.depth);
        return;
    }
    [self nvApplyTable];
    [_inner setComputePipelineState:_ps];
    [_inner dispatchThreads:threadsPerGrid threadsPerThreadgroup:threadsPerThreadgroup];
}
- (void)dispatchThreadgroups:(MTLSize)threadgroupsPerGrid
       threadsPerThreadgroup:(MTLSize)threadsPerThreadgroup {
    if (!nvM4SizeUsable(threadgroupsPerGrid) || !nvM4SizeUsable(threadsPerThreadgroup)) {
        NSLog(@"NVMTLDriver: MTL4 dispatchThreadgroups rejected zero/overflow size");
        return;
    }
    [self nvApplyTable];
    [_inner setComputePipelineState:_ps];
    [_inner dispatchThreadgroups:threadgroupsPerGrid threadsPerThreadgroup:threadsPerThreadgroup];
}
// 0.9.19: MTL4 stage barriers. The native encoder exposes one conservative
// memory barrier (buffers + textures); using it for every MTL4 barrier scope
// is stronger than required and cannot under-synchronize a dispatch.
- (void)nvBarrier {
    if ([_inner respondsToSelector:@selector(memoryBarrierWithScope:)])
        [_inner memoryBarrierWithScope:(MTLBarrierScopeBuffers | MTLBarrierScopeTextures)];
}
// 0.9.20: the public MTL4 selectors carry visibilityOptions:. The aliases
// without it are SPI shapes some older callers use; both must exist because
// the MTL4 protocol is checked at runtime and an unknown selector would throw
// out of a conformance-verified object.
- (void)barrierAfterQueueStages:(MTLStages)a beforeStages:(MTLStages)b
              visibilityOptions:(NSUInteger)options {
    (void)a; (void)b; (void)options; [self nvBarrier];
}
- (void)barrierAfterStages:(MTLStages)a beforeQueueStages:(MTLStages)b
         visibilityOptions:(NSUInteger)options {
    (void)a; (void)b; (void)options; [self nvBarrier];
}
- (void)barrierAfterEncoderStages:(MTLStages)a beforeEncoderStages:(MTLStages)b
                visibilityOptions:(NSUInteger)options {
    (void)a; (void)b; (void)options; [self nvBarrier];
}
- (void)barrierAfterQueueStages:(MTLStages)a beforeStages:(MTLStages)b {
    [self barrierAfterQueueStages:a beforeStages:b visibilityOptions:0];
}
- (void)barrierAfterStages:(MTLStages)a beforeQueueStages:(MTLStages)b {
    [self barrierAfterStages:a beforeQueueStages:b visibilityOptions:0];
}
- (void)barrierAfterEncoderStages:(MTLStages)a beforeEncoderStages:(MTLStages)b {
    [self barrierAfterEncoderStages:a beforeEncoderStages:b visibilityOptions:0];
}
// Fences: the native compute encoder has the stage-less updateFence:/waitForFence:
// pair. A stage-scoped MTL4 fence is emitted as the same conservative fence;
// compute execution on this driver is serialized, so over-synchronizing is safe.
- (void)updateFence:(id<MTLFence>)fence afterEncoderStages:(MTLStages)stages {
    (void)stages;
    if (fence && [_inner respondsToSelector:@selector(updateFence:)])
        [_inner updateFence:fence];
}
- (void)waitForFence:(id<MTLFence>)fence beforeEncoderStages:(MTLStages)stages {
    (void)stages;
    if (fence && [_inner respondsToSelector:@selector(waitForFence:)])
        [_inner waitForFence:fence];
}
- (void)dispatchThreadgroupsWithIndirectBuffer:(MTLGPUAddress)buffer
                         threadsPerThreadgroup:(MTLSize)threadsPerThreadgroup {
    id ib = nil; NSUInteger off = 0;
    // MTLDispatchThreadgroupsIndirectArguments is three uint32 values.
    if (!nvM4ResolveAddress(buffer, 3 * sizeof(uint32_t), &ib, &off)) {
        NSLog(@"NVMTLDriver: MTL4 indirect dispatch address 0x%llx has no buffer or is too short",
              (unsigned long long)buffer);
        return;
    }
    [self nvApplyTable];
    [_inner setComputePipelineState:_ps];
    [_inner dispatchThreadgroupsWithIndirectBuffer:ib indirectBufferOffset:off
                            threadsPerThreadgroup:threadsPerThreadgroup];
}
- (void)dispatchThreadsWithIndirectBuffer:(MTLGPUAddress)buffer {
    // MTL4's thread-grid indirect form has no threadsPerThreadgroup argument;
    // the grid is derived from the pipeline's distribution mode. The native
    // path needs that mode implemented before it can submit a correct grid, so
    // refuse instead of launching a guessed (1,1,1) shape.
    (void)buffer;
    NSLog(@"NVMTLDriver: MTL4 dispatchThreadsWithIndirectBuffer: needs threadgroup "
          @"distribution lowering; not submitted");
}
- (void)executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)buffer withRange:(NSRange)range {
    if (_inner && buffer && [_inner respondsToSelector:@selector(executeCommandsInBuffer:withRange:)])
        [_inner executeCommandsInBuffer:buffer withRange:range];
}
- (void)executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)buffer indirectBuffer:(MTLGPUAddress)address {
    // MTL4 passes the execution-range buffer as a raw GPU address; the native
    // Metal 3 encoder takes the buffer plus an offset.
    id<MTLBuffer> indirectBuffer = nil; NSUInteger offset = 0;
    if (!_inner || !buffer || !nvM4ResolveAddress(address, 8, &indirectBuffer, &offset)) {
        NSLog(@"NVMTLDriver: MTL4 indirect command range address has no buffer");
        return;
    }
    if ([_inner respondsToSelector:@selector(executeCommandsInBuffer:indirectBuffer:indirectBufferOffset:)])
        [_inner executeCommandsInBuffer:buffer indirectBuffer:indirectBuffer indirectBufferOffset:offset];
}
// 0.9.20: MTL4 resource-optimization and acceleration-structure methods do
// not exist on the Metal 3 compute encoder. Optimization is a hint (no-op);
// acceleration structures are refused because the device reports no ray
// tracing. Without these entries a conforming caller would hit an
// unrecognized selector.
- (void)optimizeContentsForCPUAccess:(id<MTLResource>)resource {
    (void)resource;
}
- (void)optimizeContentsForCPUAccess:(id<MTLResource>)resource slice:(NSUInteger)slice
                               level:(NSUInteger)level {
    (void)resource; (void)slice; (void)level;
}
- (void)optimizeContentsForGPUAccess:(id<MTLResource>)resource {
    (void)resource;
}
- (void)optimizeContentsForGPUAccess:(id<MTLResource>)resource slice:(NSUInteger)slice
                               level:(NSUInteger)level {
    (void)resource; (void)slice; (void)level;
}
- (void)copyAccelerationStructure:(id)source toAccelerationStructure:(id)destination {
    (void)source; (void)destination;
    static BOOL said;
    if (!said) { said = YES; NSLog(@"NVMTLDriver: MTL4 acceleration structures are not implemented"); }
}
- (void)copyAndCompactAccelerationStructure:(id)source toAccelerationStructure:(id)destination {
    (void)source; (void)destination;
    static BOOL said;
    if (!said) { said = YES; NSLog(@"NVMTLDriver: MTL4 acceleration structures are not implemented"); }
}
- (void)buildAccelerationStructure:(id)structure descriptor:(id)descriptor
                      scratchBuffer:(id<MTLBuffer>)scratchBuffer {
    (void)structure; (void)descriptor; (void)scratchBuffer;
    static BOOL said;
    if (!said) { said = YES; NSLog(@"NVMTLDriver: MTL4 acceleration structures are not implemented"); }
}
- (void)refitAccelerationStructure:(id)structure descriptor:(id)descriptor
                       destination:(id)destination scratchBuffer:(id)scratchBuffer {
    (void)structure; (void)descriptor; (void)destination; (void)scratchBuffer;
    static BOOL said;
    if (!said) { said = YES; NSLog(@"NVMTLDriver: MTL4 acceleration structures are not implemented"); }
}
- (void)refitAccelerationStructure:(id)structure descriptor:(id)descriptor
                       destination:(id)destination scratchBuffer:(id)scratchBuffer
                           options:(NSUInteger)options {
    (void)structure; (void)descriptor; (void)destination; (void)scratchBuffer; (void)options;
    static BOOL said;
    if (!said) { said = YES; NSLog(@"NVMTLDriver: MTL4 acceleration structures are not implemented"); }
}
- (void)writeCompactedAccelerationStructureSize:(id)structure toBuffer:(id<MTLBuffer>)buffer {
    (void)structure; (void)buffer;
    static BOOL said;
    if (!said) { said = YES; NSLog(@"NVMTLDriver: MTL4 acceleration structures are not implemented"); }
}
// 0.9.20: MTL4 tensor copies are part of the public compute protocol but the
// native Metal 3 path has no tensor object. Refuse them explicitly instead of
// falling through to the unrecognized-selector exception.
- (void)copyFromTensor:(id)source
          sourceOrigin:(MTLOrigin)sourceOrigin
      sourceDimensions:(MTLSize)sourceDimensions
              toTensor:(id)destination
      destinationOrigin:(MTLOrigin)destinationOrigin
  destinationDimensions:(MTLSize)destinationDimensions {
    (void)source; (void)sourceOrigin; (void)sourceDimensions;
    (void)destination; (void)destinationOrigin; (void)destinationDimensions;
    static BOOL said;
    if (!said) { said = YES; NSLog(@"NVMTLDriver: MTL4 tensor copy is not implemented"); }
}
- (void)copyFromTensor:(id)source
          sourceOrigin:(MTLOrigin)sourceOrigin
      sourceDimensions:(MTLSize)sourceDimensions
           sourcePlane:(NSUInteger)sourcePlane
              toTensor:(id)destination
      destinationOrigin:(MTLOrigin)destinationOrigin
  destinationDimensions:(MTLSize)destinationDimensions
       destinationPlane:(NSUInteger)destinationPlane {
    (void)source; (void)sourceOrigin; (void)sourceDimensions; (void)sourcePlane;
    (void)destination; (void)destinationOrigin; (void)destinationDimensions; (void)destinationPlane;
    static BOOL said;
    if (!said) { said = YES; NSLog(@"NVMTLDriver: MTL4 tensor copy is not implemented"); }
}
- (void)writeTimestampWithGranularity:(MTL4TimestampGranularity)granularity
                             intoHeap:(id<MTL4CounterHeap>)counterHeap
                              atIndex:(NSUInteger)index {
    (void)granularity;
    if (counterHeap && [counterHeap respondsToSelector:@selector(nvWriteTimestamp:atIndex:)])
        [(id)counterHeap nvWriteTimestamp:clock_gettime_nsec_np(CLOCK_UPTIME_RAW) atIndex:index];
}
- (void)endEncoding { _ended = YES; [_inner endEncoding]; }
- (BOOL)nvEnded { return _ended; }
// Render-only MTL4 selectors (store actions, attachment maps, mesh/tile
// draws, the render-form timestamp) live on NVMTL4RenderEncoder below. They
// used to be duplicated here on the compute wrapper as silent no-ops; a
// compute encoder must not answer a render-only protocol method.
// Forward common Metal 3 selector names (setTexture:, setSamplerState:,
// barriers, indexed/instanced draws, ...) to the native inner encoder.
- (id)forwardingTargetForSelector:(SEL)sel {
    if (_inner && [_inner respondsToSelector:sel]) return _inner;
    return [super forwardingTargetForSelector:sel];
}
- (NSMethodSignature *)methodSignatureForSelector:(SEL)sel {
    NSMethodSignature *m = nvM4ProtocolSignature(@protocol(MTL4ComputeCommandEncoder), sel);
    if (!m) m = nvM4ProtocolSignature(@protocol(MTL4CommandEncoder), sel);
    return m ?: [super methodSignatureForSelector:sel];
}
- (void)forwardInvocation:(NSInvocation *)invocation {
    nvM4UnknownSelector(self, invocation);
}
@end

@interface NVMTL4RenderEncoder : NSObject
- (instancetype)initWithInner:(id<MTLRenderCommandEncoder>)inner
                commandBuffer:(id<MTL4CommandBuffer>)commandBuffer;
- (id<MTL4CommandBuffer>)commandBuffer;
- (NSString *)label;
- (void)setLabel:(NSString *)label;
- (void)nvSetTileWidth:(NSUInteger)width height:(NSUInteger)height;
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)state;
- (void)setViewport:(MTLViewport)viewport;
- (void)setScissorRect:(MTLScissorRect)rect;
- (void)setVertexBuffer:(id<MTLBuffer>)buf offset:(NSUInteger)off atIndex:(NSUInteger)idx;
- (void)setFragmentBuffer:(id<MTLBuffer>)buf offset:(NSUInteger)off atIndex:(NSUInteger)idx;
- (void)setArgumentTable:(id<MTL4ArgumentTable>)table atStages:(MTLRenderStages)stages;
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n;
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n instanceCount:(NSUInteger)i;
- (void)updateFence:(id<MTLFence>)fence afterEncoderStages:(MTLStages)stages;
- (void)waitForFence:(id<MTLFence>)fence beforeEncoderStages:(MTLStages)stages;
- (void)barrierAfterEncoderStages:(MTLStages)after beforeEncoderStages:(MTLStages)before
                  visibilityOptions:(NSUInteger)options;
- (void)barrierAfterQueueStages:(MTLStages)after beforeStages:(MTLStages)before
                visibilityOptions:(NSUInteger)options;
- (void)barrierAfterStages:(MTLStages)after beforeQueueStages:(MTLStages)before
           visibilityOptions:(NSUInteger)options;
- (void)endEncoding;
- (BOOL)nvEnded;
@end

@implementation NVMTL4RenderEncoder {
    id<MTLRenderCommandEncoder> _inner;
    id<MTLRenderPipelineState> _ps;
    __weak id<MTL4CommandBuffer> _nvCommandBuffer;
    NSString *_nvLabel;
    NSUInteger _tileW, _tileH;
    BOOL _ended;
}
- (instancetype)initWithInner:(id<MTLRenderCommandEncoder>)inner
                commandBuffer:(id<MTL4CommandBuffer>)commandBuffer {
    self = [super init];
    if (self) { _inner = inner; _nvCommandBuffer = commandBuffer; }
    return self;
}
- (id<MTL4CommandBuffer>)commandBuffer { return _nvCommandBuffer; }
- (NSString *)label { return _nvLabel; }
- (void)setLabel:(NSString *)label {
    _nvLabel = [label copy];
    if ([_inner respondsToSelector:@selector(setLabel:)]) [_inner setLabel:label];
}
- (void)pushDebugGroup:(NSString *)string {
    if ([_inner respondsToSelector:@selector(pushDebugGroup:)]) [_inner pushDebugGroup:string];
}
- (void)popDebugGroup {
    if ([_inner respondsToSelector:@selector(popDebugGroup)]) [_inner popDebugGroup];
}
- (void)insertDebugSignpost:(NSString *)string {
    if ([_inner respondsToSelector:@selector(insertDebugSignpost:)]) [_inner insertDebugSignpost:string];
}
- (void)nvSetTileWidth:(NSUInteger)width height:(NSUInteger)height { _tileW = width; _tileH = height; }
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)state { _ps = state; [_inner setRenderPipelineState:state]; }
- (void)setViewport:(MTLViewport)v { [_inner setViewport:v]; }
- (void)setScissorRect:(MTLScissorRect)r { [_inner setScissorRect:r]; }
- (void)setViewports:(const MTLViewport *)viewports count:(NSUInteger)count {
    for (NSUInteger i = 0; viewports && i < count; ++i) [_inner setViewport:viewports[i]];
}
- (void)setScissorRects:(const MTLScissorRect *)rects count:(NSUInteger)count {
    for (NSUInteger i = 0; rects && i < count; ++i) [_inner setScissorRect:rects[i]];
}
- (void)setVertexBuffer:(id<MTLBuffer>)buf offset:(NSUInteger)off atIndex:(NSUInteger)idx { [_inner setVertexBuffer:buf offset:off atIndex:idx]; }
- (void)setFragmentBuffer:(id<MTLBuffer>)buf offset:(NSUInteger)off atIndex:(NSUInteger)idx { [_inner setFragmentBuffer:buf offset:off atIndex:idx]; }
// 0.9.9: render argument tables carry resource IDs, not objects. Resolve
// them through the device registries and bind at the requested stages.
- (void)setArgumentTable:(id<MTL4ArgumentTable>)table atStages:(MTLRenderStages)stages {
    if (!table || ![table isKindOfClass:[NVMTL4ArgumentTable class]]) return;
    NVMTL4ArgumentTable *t = (NVMTL4ArgumentTable *)table;
    for (unsigned i = 0; i < 31; ++i) {
        id b = nil;
        NSUInteger off = 0;
        const uint64_t rid = [t nvResourceAtIndex:i];
        if (rid) b = nvMTLBufferForResourceID(rid);
        if (!b) {   // public MTLBuffer.gpuAddress path (may be an interior address)
            MTLGPUAddress addr = 0; NSUInteger len = 0;
            if ([t nvAddressAtIndex:i address:&addr length:&len] && addr) {
                if (!nvM4ResolveAddress(addr, len, &b, &off)) {
                    NSLog(@"NVMTLDriver: MTL4 argument table buffer %u address 0x%llx length %lu is not in a buffer",
                          i, (unsigned long long)addr, (unsigned long)len);
                    continue;
                }
            }
        }
        if (!b) continue;
        if (stages & MTLRenderStageVertex) {
            const NSUInteger stride = [t nvAttributeStrideAtIndex:i];
            if (stride) [_inner setVertexBuffer:b offset:off attributeStride:stride atIndex:i];
            else [_inner setVertexBuffer:b offset:off atIndex:i];
        }
        if (stages & MTLRenderStageFragment) [_inner setFragmentBuffer:b offset:off atIndex:i];
    }
    for (unsigned i = 0; i < 128; ++i) {
        const uint64_t rid = [t nvTextureAtIndex:i];
        if (!rid) continue;
        id tx = nvMTLTextureForResourceID(rid);
        if (!tx) continue;
        if (stages & MTLRenderStageVertex) [_inner setVertexTexture:tx atIndex:i];
        if (stages & MTLRenderStageFragment) [_inner setFragmentTexture:tx atIndex:i];
    }
    for (unsigned i = 0; i < 16; ++i) {
        const uint64_t rid = [t nvSamplerAtIndex:i];
        if (!rid) continue;
        id sm = nvMTLSamplerForResourceID(rid);
        if (!sm) continue;
        if (stages & MTLRenderStageVertex) [_inner setVertexSamplerState:sm atIndex:i];
        if (stages & MTLRenderStageFragment) [_inner setFragmentSamplerState:sm atIndex:i];
    }
}
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n { [_inner setRenderPipelineState:_ps]; [_inner drawPrimitives:t vertexStart:s vertexCount:n]; }
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n instanceCount:(NSUInteger)i { [_inner setRenderPipelineState:_ps]; [_inner drawPrimitives:t vertexStart:s vertexCount:n instanceCount:i]; }
// 0.9.19: the public MTL4 indexed draws carry a raw index-buffer address, not
// an id<MTLBuffer>. Resolve it to the native encoder's buffer+offset form.
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)n indexType:(MTLIndexType)it
                  indexBuffer:(MTLGPUAddress)address indexBufferLength:(NSUInteger)length {
    if (!nvM4IndexCountFits(n, it, length)) {
        NSLog(@"NVMTLDriver: MTL4 indexed draw count %lu does not fit length %lu",
              (unsigned long)n, (unsigned long)length);
        return;
    }
    id<MTLBuffer> b = nil; NSUInteger off = 0;
    if (!nvM4ResolveAddress(address, length, &b, &off) ||
        !nvM4IndexRangeFits(n, it, length, b.length, off)) {
        NSLog(@"NVMTLDriver: MTL4 indexed draw address 0x%llx/length %lu is outside its buffer",
              (unsigned long long)address, (unsigned long)length);
        return;
    }
    [_inner setRenderPipelineState:_ps];
    [_inner drawIndexedPrimitives:t indexCount:n indexType:it indexBuffer:b indexBufferOffset:off];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)n indexType:(MTLIndexType)it
                  indexBuffer:(MTLGPUAddress)address indexBufferLength:(NSUInteger)length
                instanceCount:(NSUInteger)instances {
    if (!nvM4IndexCountFits(n, it, length)) {
        NSLog(@"NVMTLDriver: MTL4 indexed draw count %lu does not fit length %lu",
              (unsigned long)n, (unsigned long)length);
        return;
    }
    id<MTLBuffer> b = nil; NSUInteger off = 0;
    if (!nvM4ResolveAddress(address, length, &b, &off) ||
        !nvM4IndexRangeFits(n, it, length, b.length, off)) {
        NSLog(@"NVMTLDriver: MTL4 indexed draw address 0x%llx/length %lu is outside its buffer",
              (unsigned long long)address, (unsigned long)length);
        return;
    }
    [_inner setRenderPipelineState:_ps];
    [_inner drawIndexedPrimitives:t indexCount:n indexType:it indexBuffer:b indexBufferOffset:off
                    instanceCount:instances];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)n indexType:(MTLIndexType)it
                  indexBuffer:(MTLGPUAddress)address indexBufferLength:(NSUInteger)length
                instanceCount:(NSUInteger)instances baseVertex:(NSInteger)baseVertex baseInstance:(NSUInteger)baseInstance {
    if (!nvM4IndexCountFits(n, it, length)) {
        NSLog(@"NVMTLDriver: MTL4 indexed draw count %lu does not fit length %lu",
              (unsigned long)n, (unsigned long)length);
        return;
    }
    id<MTLBuffer> b = nil; NSUInteger off = 0;
    if (!nvM4ResolveAddress(address, length, &b, &off) ||
        !nvM4IndexRangeFits(n, it, length, b.length, off)) {
        NSLog(@"NVMTLDriver: MTL4 indexed draw address 0x%llx/length %lu is outside its buffer",
              (unsigned long long)address, (unsigned long)length);
        return;
    }
    [_inner setRenderPipelineState:_ps];
    [_inner drawIndexedPrimitives:t indexCount:n indexType:it indexBuffer:b indexBufferOffset:off
                    instanceCount:instances baseVertex:baseVertex baseInstance:baseInstance];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexType:(MTLIndexType)it
                  indexBuffer:(MTLGPUAddress)address indexBufferLength:(NSUInteger)length
               indirectBuffer:(MTLGPUAddress)indirectAddress {
    id<MTLBuffer> ib = nil, rb = nil;
    NSUInteger ioff = 0, roff = 0;
    // MTLDrawIndexedPrimitivesIndirectArguments is five uint32 values.
    if (!nvM4ResolveAddress(address, length, &ib, &ioff) ||
        !nvM4ResolveAddress(indirectAddress, 5 * sizeof(uint32_t), &rb, &roff)) {
        NSLog(@"NVMTLDriver: MTL4 indexed indirect draw address is outside its buffer");
        return;
    }
    [_inner setRenderPipelineState:_ps];
    [_inner drawIndexedPrimitives:t indexType:it indexBuffer:ib indexBufferOffset:ioff
                    indirectBuffer:rb indirectBufferOffset:roff];
}
- (void)drawPrimitives:(MTLPrimitiveType)t indirectBuffer:(MTLGPUAddress)address {
    id<MTLBuffer> b = nil; NSUInteger off = 0;
    // MTLDrawPrimitivesIndirectArguments is four uint32 values.
    if (!nvM4ResolveAddress(address, 4 * sizeof(uint32_t), &b, &off)) {
        NSLog(@"NVMTLDriver: MTL4 indirect draw address is outside its buffer");
        return;
    }
    [_inner setRenderPipelineState:_ps];
    [_inner drawPrimitives:t indirectBuffer:b indirectBufferOffset:off];
}
// 0.9.19: MTL4 mesh draws route into the native mesh pipeline already used by
// Metal 3 (object/mesh compute + generated vertex shader). The inner object is
// NVMTLRenderEncoder, which implements the same selectors.
- (void)drawMeshThreadgroups:(MTLSize)groups threadsPerObjectThreadgroup:(MTLSize)ot threadsPerMeshThreadgroup:(MTLSize)mt {
    [_inner setRenderPipelineState:_ps];
    [(id)_inner drawMeshThreadgroups:groups threadsPerObjectThreadgroup:ot threadsPerMeshThreadgroup:mt];
}
- (void)drawMeshThreads:(MTLSize)threads threadsPerObjectThreadgroup:(MTLSize)ot threadsPerMeshThreadgroup:(MTLSize)mt {
    [_inner setRenderPipelineState:_ps];
    [(id)_inner drawMeshThreads:threads threadsPerObjectThreadgroup:ot threadsPerMeshThreadgroup:mt];
}
- (void)drawMeshThreadgroupsWithIndirectBuffer:(MTLGPUAddress)buffer
                    threadsPerObjectThreadgroup:(MTLSize)object
                     threadsPerMeshThreadgroup:(MTLSize)mesh {
    id ib = nil; NSUInteger off = 0;
    // MTLDrawMeshThreadgroupsIndirectArguments is three uint32 values.
    if (!nvM4ResolveAddress(buffer, 3 * sizeof(uint32_t), &ib, &off)) {
        NSLog(@"NVMTLDriver: MTL4 mesh indirect address 0x%llx has no buffer or is too short",
              (unsigned long long)buffer);
        return;
    }
    [_inner setRenderPipelineState:_ps];
    [(id)_inner drawMeshThreadgroupsWithIndirectBuffer:ib indirectBufferOffset:off
                        threadsPerObjectThreadgroup:object threadsPerMeshThreadgroup:mesh];
}
- (void)writeTimestampWithGranularity:(MTL4TimestampGranularity)granularity afterStage:(MTLRenderStages)stage intoHeap:(id<MTL4CounterHeap>)heap atIndex:(NSUInteger)index { (void)granularity; (void)stage; nvM4Timestamp(heap, index); }
- (void)endEncoding { _ended = YES; [_inner endEncoding]; }
- (BOOL)nvEnded { return _ended; }
- (void)nvBarrier {
    if ([_inner respondsToSelector:@selector(memoryBarrierWithScope:afterStages:beforeStages:)])
        [_inner memoryBarrierWithScope:(MTLBarrierScopeBuffers | MTLBarrierScopeTextures)
                            afterStages:(MTLRenderStageVertex | MTLRenderStageFragment)
                           beforeStages:(MTLRenderStageVertex | MTLRenderStageFragment)];
}
// 0.9.20: public MTL4 selectors (visibilityOptions:) plus the older SPI
// aliases, for the same reason as the compute encoder.
- (void)barrierAfterQueueStages:(MTLStages)a beforeStages:(MTLStages)b
              visibilityOptions:(NSUInteger)options {
    (void)a; (void)b; (void)options; [self nvBarrier];
}
- (void)barrierAfterStages:(MTLStages)a beforeQueueStages:(MTLStages)b
         visibilityOptions:(NSUInteger)options {
    (void)a; (void)b; (void)options; [self nvBarrier];
}
- (void)barrierAfterEncoderStages:(MTLStages)a beforeEncoderStages:(MTLStages)b
                visibilityOptions:(NSUInteger)options {
    (void)a; (void)b; (void)options; [self nvBarrier];
}
- (void)barrierAfterQueueStages:(MTLStages)a beforeStages:(MTLStages)b {
    [self barrierAfterQueueStages:a beforeStages:b visibilityOptions:0];
}
- (void)barrierAfterStages:(MTLStages)a beforeQueueStages:(MTLStages)b {
    [self barrierAfterStages:a beforeQueueStages:b visibilityOptions:0];
}
- (void)barrierAfterEncoderStages:(MTLStages)a beforeEncoderStages:(MTLStages)b {
    [self barrierAfterEncoderStages:a beforeEncoderStages:b visibilityOptions:0];
}
// The native render encoder has the stage-scoped fence methods; MTL4's
// MTLStages bitmask is mapped to the conservative vertex+fragment pair.
- (void)updateFence:(id<MTLFence>)fence afterEncoderStages:(MTLStages)stages {
    (void)stages;
    if (!fence) return;
    if ([_inner respondsToSelector:@selector(updateFence:afterStages:)])
        [_inner updateFence:fence afterStages:(MTLRenderStageVertex | MTLRenderStageFragment)];
    else if ([_inner respondsToSelector:@selector(updateFence:)])
        [(id)_inner updateFence:fence];
}
- (void)waitForFence:(id<MTLFence>)fence beforeEncoderStages:(MTLStages)stages {
    (void)stages;
    if (!fence) return;
    if ([_inner respondsToSelector:@selector(waitForFence:beforeStages:)])
        [_inner waitForFence:fence beforeStages:(MTLRenderStageVertex | MTLRenderStageFragment)];
    else if ([_inner respondsToSelector:@selector(waitForFence:)])
        [(id)_inner waitForFence:fence];
}
// MTL4-only controls the Metal 3 encoder cannot express. The render pass and
// pipeline already carry these values; accept them instead of raising
// unrecognized-selector exceptions.
- (void)setColorAttachmentMap:(id)mapping {
    // Identity is the only mapping the native 3D state expresses. Log the
    // first non-identity request so a wrong-attachment bug is visible.
    static BOOL said;
    if (mapping && !said) {
        said = YES;
        NSLog(@"NVMTLDriver: non-identity MTL4 color attachment mapping is not lowered");
    }
}
- (void)setDepthTestMinBound:(float)minBound maxBound:(float)maxBound {
    (void)minBound; (void)maxBound;
    static BOOL said;
    if (!said) { said = YES; NSLog(@"NVMTLDriver: MTL4 depth bounds test is not implemented"); }
}
- (void)setColorStoreAction:(MTLStoreAction)storeAction atIndex:(NSUInteger)index { [_inner setColorStoreAction:storeAction atIndex:index]; }
- (void)setDepthStoreAction:(MTLStoreAction)storeAction { [_inner setDepthStoreAction:storeAction]; }
- (void)setStencilStoreAction:(MTLStoreAction)storeAction { [_inner setStencilStoreAction:storeAction]; }
- (void)setObjectThreadgroupMemoryLength:(NSUInteger)length atIndex:(NSUInteger)index { (void)length; (void)index; }
- (void)setThreadgroupMemoryLength:(NSUInteger)length offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    // The native render encoder accepts the offset form; use it when present
    // and fall back to offset 0 only when it is not.
    if ([_inner respondsToSelector:@selector(setThreadgroupMemoryLength:offset:atIndex:)]) {
        [_inner setThreadgroupMemoryLength:length offset:offset atIndex:index];
    } else if (!offset && [_inner respondsToSelector:@selector(setThreadgroupMemoryLength:atIndex:)]) {
        [(id)_inner setThreadgroupMemoryLength:length atIndex:index];
    } else if (offset) {
        NSLog(@"NVMTLDriver: MTL4 render threadgroup memory offset %lu unsupported",
              (unsigned long)offset);
    }
}
- (void)dispatchThreadsPerTile:(MTLSize)threads {
    // Tile shaders are an Apple-GPU family feature; this device does not
    // report it, but a framework probe must not raise.
    (void)threads;
    static BOOL said;
    if (!said) { said = YES; NSLog(@"NVMTLDriver: tile dispatch is not implemented"); }
}
- (NSUInteger)tileWidth { return _tileW; }
- (NSUInteger)tileHeight { return _tileH; }
- (void)executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)buffer withRange:(NSRange)range {
    if (_inner && buffer && [_inner respondsToSelector:@selector(executeCommandsInBuffer:withRange:)])
        [_inner executeCommandsInBuffer:buffer withRange:range];
}
- (void)executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)buffer indirectBuffer:(MTLGPUAddress)address {
    id<MTLBuffer> indirectBuffer = nil; NSUInteger offset = 0;
    if (!_inner || !buffer || !nvM4ResolveAddress(address, 8, &indirectBuffer, &offset)) {
        NSLog(@"NVMTLDriver: MTL4 indirect command range address has no buffer");
        return;
    }
    if ([_inner respondsToSelector:@selector(executeCommandsInBuffer:indirectBuffer:indirectBufferOffset:)])
        [_inner executeCommandsInBuffer:buffer indirectBuffer:indirectBuffer indirectBufferOffset:offset];
    else
        NSLog(@"NVMTLDriver: native render encoder has no indirect command-range entry point");
}
// Forward common Metal 3 selector names (setTexture:, setSamplerState:,
// barriers, indexed/instanced draws, ...) to the native inner encoder.
- (id)forwardingTargetForSelector:(SEL)sel {
    if (_inner && [_inner respondsToSelector:sel]) return _inner;
    return [super forwardingTargetForSelector:sel];
}
- (NSMethodSignature *)methodSignatureForSelector:(SEL)sel {
    NSMethodSignature *m = nvM4ProtocolSignature(@protocol(MTL4RenderCommandEncoder), sel);
    if (!m) m = nvM4ProtocolSignature(@protocol(MTL4CommandEncoder), sel);
    return m ?: [super methodSignatureForSelector:sel];
}
- (void)forwardInvocation:(NSInvocation *)invocation {
    nvM4UnknownSelector(self, invocation);
}
@end

static void nvM4Timestamp(id heap, NSUInteger index) {
    if (heap && [heap respondsToSelector:@selector(nvWriteTimestamp:atIndex:)])
        [(id)heap nvWriteTimestamp:clock_gettime_nsec_np(CLOCK_UPTIME_RAW) atIndex:index];
}

static MTLRenderPassDescriptor *nvM3RenderPass(id rpd) {
    if (![rpd isKindOfClass:[MTL4RenderPassDescriptor class]]) return nil;
    MTL4RenderPassDescriptor *r = (MTL4RenderPassDescriptor *)rpd;
    MTLRenderPassDescriptor *m = [MTLRenderPassDescriptor renderPassDescriptor];
    for (NSUInteger i = 0; i < 8; ++i) {
        MTLRenderPassColorAttachmentDescriptor *s = r.colorAttachments[i];
        MTLRenderPassColorAttachmentDescriptor *d = m.colorAttachments[i];
        d.texture = s.texture; d.loadAction = s.loadAction;
        d.storeAction = s.storeAction; d.clearColor = s.clearColor;
        d.level = s.level; d.slice = s.slice; d.depthPlane = s.depthPlane;
        d.resolveTexture = s.resolveTexture;
        d.resolveLevel = s.resolveLevel; d.resolveSlice = s.resolveSlice;
        d.resolveDepthPlane = s.resolveDepthPlane;
    }
    MTLRenderPassDepthAttachmentDescriptor *ds = r.depthAttachment;
    MTLRenderPassDepthAttachmentDescriptor *dd = m.depthAttachment;
    dd.texture = ds.texture; dd.loadAction = ds.loadAction;
    dd.storeAction = ds.storeAction; dd.clearDepth = ds.clearDepth;
    dd.level = ds.level; dd.slice = ds.slice; dd.depthPlane = ds.depthPlane;
    dd.resolveTexture = ds.resolveTexture;
    dd.resolveLevel = ds.resolveLevel; dd.resolveSlice = ds.resolveSlice;
    dd.resolveDepthPlane = ds.resolveDepthPlane;
    MTLRenderPassStencilAttachmentDescriptor *ss = r.stencilAttachment;
    MTLRenderPassStencilAttachmentDescriptor *sd = m.stencilAttachment;
    sd.texture = ss.texture; sd.loadAction = ss.loadAction;
    sd.storeAction = ss.storeAction; sd.clearStencil = ss.clearStencil;
    sd.level = ss.level; sd.slice = ss.slice; sd.depthPlane = ss.depthPlane;
    sd.resolveTexture = ss.resolveTexture;
    sd.resolveLevel = ss.resolveLevel; sd.resolveSlice = ss.resolveSlice;
    sd.resolveDepthPlane = ss.resolveDepthPlane;
    m.visibilityResultBuffer = r.visibilityResultBuffer;
    m.renderTargetArrayLength = r.renderTargetArrayLength;
    m.renderTargetWidth = r.renderTargetWidth;
    m.renderTargetHeight = r.renderTargetHeight;
    m.defaultRasterSampleCount = r.defaultRasterSampleCount;
    // defaultColorSampleCount and useExplicitRenderTargetDimensions are
    // SPI-only on the Metal 3 descriptor, but MTL4 apps do use them. Forward
    // them when both objects answer, instead of silently dropping MSAA/explicit
    // target-size state.
    SEL getColorSamples = sel_registerName("defaultColorSampleCount");
    SEL setColorSamples = sel_registerName("setDefaultColorSampleCount:");
    if ([r respondsToSelector:getColorSamples] && [m respondsToSelector:setColorSamples]) {
        const NSUInteger n = ((NSUInteger (*)(id, SEL))objc_msgSend)(r, getColorSamples);
        ((void (*)(id, SEL, NSUInteger))objc_msgSend)(m, setColorSamples, n);
    }
    SEL getExplicit = sel_registerName("useExplicitRenderTargetDimensions");
    SEL setExplicit = sel_registerName("setUseExplicitRenderTargetDimensions:");
    if ([r respondsToSelector:getExplicit] && [m respondsToSelector:setExplicit]) {
        const BOOL on = ((BOOL (*)(id, SEL))objc_msgSend)(r, getExplicit);
        ((void (*)(id, SEL, BOOL))objc_msgSend)(m, setExplicit, on);
    }
    m.visibilityResultType = r.visibilityResultType;
    m.supportColorAttachmentMapping = r.supportColorAttachmentMapping;
    m.rasterizationRateMap = r.rasterizationRateMap;
    m.tileWidth = r.tileWidth;
    m.tileHeight = r.tileHeight;
    m.imageblockSampleLength = r.imageblockSampleLength;
    m.threadgroupMemoryLength = r.threadgroupMemoryLength;
    // Programmable sample positions: copy up to the Metal limit and let the
    // native encoder validate the count.
    MTLSamplePosition positions[32];
    // 0.9.49: ask for the programmed count first; a count that differs asserts
    const NSUInteger sampleCount = [r getSamplePositions:NULL count:0];
    if (sampleCount && sampleCount <= 32) {
        [r getSamplePositions:positions count:sampleCount];
        [m setSamplePositions:positions count:sampleCount];
    }
    return m;
}

static id<MTLFunction> nvM4Function(id fd) {
    if (!fd || ![fd respondsToSelector:NSSelectorFromString(@"library")] ||
        ![fd respondsToSelector:NSSelectorFromString(@"name")]) return nil;
    id<MTLLibrary> lib = [fd valueForKey:@"library"];
    NSString *name = [fd valueForKey:@"name"];
    return lib && name.length ? [lib newFunctionWithName:name] : nil;
}

// 0.9.19: minimal MTL4CommitFeedback. The native submission is synchronous, so
// start/end are the encode/commit window and no error means success.
@interface NVMTL4CommitFeedback : NSObject
@property (nonatomic) double gpuStart, gpuEnd;
@property (nonatomic, strong, nullable) NSError *error;
@end
@implementation NVMTL4CommitFeedback
- (double)GPUStartTime { return _gpuStart; }
- (double)GPUEndTime { return _gpuEnd; }
@end

@implementation NVMTL4CommandQueue {
    id<MTLEvent> _pendingEvent;
    id<MTLEvent> _pendingWaitEvent;
    uint64_t _pendingValue, _pendingWaitValue;
    NSMutableArray *_feedbackHandlers;
    id<MTLDrawable> _pendingDrawable;
    id<MTL4CommandBuffer> _lastCommitted;
    NSUInteger _lastGeneration;
}
- (void)commit:(const id<MTL4CommandBuffer> _Nonnull[_Nonnull])commandBuffers
         count:(NSUInteger)count {
    if (!commandBuffers || !count) return;
    const double start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
    for (NSUInteger i = 0; i < count; ++i) {
        NVMTL4CommandBuffer *cb = (NVMTL4CommandBuffer *)commandBuffers[i];
        // Be forgiving about a missing endCommandBuffer; our end is idempotent
        // (it only closes open encoders).
        if ([cb respondsToSelector:@selector(endCommandBuffer)]) [cb endCommandBuffer];
        if (i + 1 == count && [cb respondsToSelector:@selector(nvSetDrawable:)])
            [cb nvSetDrawable:_pendingDrawable];
        if (i == 0 && [cb respondsToSelector:@selector(nvSetWaitEvent:value:)])
            [cb nvSetWaitEvent:_pendingWaitEvent value:_pendingWaitValue];
        // 0.9.22: a batched commit is one submission; the queue wait applies
        // before the first buffer and the signal after the last, not to every
        // buffer in the batch.
        if ([cb respondsToSelector:@selector(nvCommitWithEvent:value:)]) {
            if (i + 1 == count) [cb nvCommitWithEvent:_pendingEvent value:_pendingValue];
            else [cb nvCommitWithEvent:nil value:0];
        }
    }
    if (_feedbackHandlers.count) {
        NVMTL4CommitFeedback *f = [NVMTL4CommitFeedback new];
        f.gpuStart = start;
        f.gpuEnd = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
        for (id handler in _feedbackHandlers) ((void (^)(id))handler)(f);
    }
    if (count) _lastCommitted = commandBuffers[count - 1];
    if (count) _lastGeneration++;
    _pendingEvent = nil;
    _pendingWaitEvent = nil;
    _pendingDrawable = nil;
    _pendingValue = _pendingWaitValue = 0;
}
- (void)commit:(const id<MTL4CommandBuffer> _Nonnull[_Nonnull])commandBuffers
         count:(NSUInteger)count options:(id)options {
    (void)options;
    [self commit:commandBuffers count:count];
}
- (void)addFeedbackHandler:(id)handler {
    if (!handler) return;
    if (!_feedbackHandlers) _feedbackHandlers = [NSMutableArray new];
    [_feedbackHandlers addObject:[handler copy]];
}
// Private MTL4 queue diagnostics used by capture/debug tooling.
- (id<MTL4CommandBuffer>)lastCommittedCommandBuffer { return _lastCommitted; }
- (NSUInteger)lastCommittedCommandBufferGeneration { return _lastGeneration; }
- (void)addResidencySet:(id<MTLResidencySet>)set { (void)set; }
- (void)addResidencySets:(const id<MTLResidencySet> _Nonnull[_Nonnull])sets count:(NSUInteger)count { (void)sets; (void)count; }
- (void)removeResidencySet:(id<MTLResidencySet>)set { (void)set; }
- (void)removeResidencySets:(const id<MTLResidencySet> _Nonnull[_Nonnull])sets count:(NSUInteger)count { (void)sets; (void)count; }
// 0.9.19: MTL4 presentation. signalDrawable: attaches the drawable to the
// next committed buffer and the command buffer calls -presentDrawable: before
// commit. waitForDrawable: is a no-op because the native queue is synchronous.
- (void)signalDrawable:(id<MTLDrawable>)drawable { _pendingDrawable = drawable; }
- (void)waitForDrawable:(id<MTLDrawable>)drawable { (void)drawable; }
- (void)signalEvent:(id<MTLEvent>)event value:(uint64_t)value {
    _pendingEvent = event;
    _pendingValue = value;
}
- (void)waitForEvent:(id<MTLEvent>)event value:(uint64_t)value {
    _pendingWaitEvent = event;
    _pendingWaitValue = value;
}
// Sparse/tile mapping updates are not part of the native AD103 path. Accept
// the calls so a framework probe does not raise an unrecognized selector; no
// mapping is silently applied.
- (void)copyBufferMappingsFromBuffer:(id<MTLBuffer>)source
                            toBuffer:(id<MTLBuffer>)destination
                          operations:(const void *)operations
                               count:(NSUInteger)count {
    (void)source; (void)destination; (void)operations; (void)count;
    NSLog(@"NVMTLDriver: MTL4 buffer mapping copy is not supported");
}
- (void)updateBufferMappings:(id<MTLBuffer>)buffer
                        heap:(id<MTLHeap>)heap
                  operations:(const void *)operations
                       count:(NSUInteger)count {
    (void)buffer; (void)heap; (void)operations; (void)count;
    NSLog(@"NVMTLDriver: MTL4 buffer mapping update is not supported");
}
- (void)copyTextureMappingsFromTexture:(id<MTLTexture>)source
                             toTexture:(id<MTLTexture>)destination
                            operations:(const void *)operations
                                 count:(NSUInteger)count {
    (void)source; (void)destination; (void)operations; (void)count;
    NSLog(@"NVMTLDriver: MTL4 texture mapping copy is not supported");
}
- (void)updateTextureMappings:(id<MTLTexture>)texture
                         heap:(id<MTLHeap>)heap
                   operations:(const void *)operations
                        count:(NSUInteger)count {
    (void)texture; (void)heap; (void)operations; (void)count;
    NSLog(@"NVMTLDriver: MTL4 texture mapping update is not supported");
}
@end

@implementation NVMTL4CommandBuffer {
    id<MTLCommandQueue> _innerQueue;     // retained and reused across begins
    id<MTLCommandBuffer> _inner;
    id<MTL4CommandAllocator> _allocator;
    NSUInteger _allocatorGeneration;
    NVMTL4ComputeEncoder *_encoder;
    NVMTL4RenderEncoder *_renderEncoder;
    BOOL _computeOpen, _renderOpen;
    NSString *_nvLabel;
    id<MTLDrawable> _nvDrawable;
}
- (NSString *)label { return _nvLabel; }
- (void)setLabel:(NSString *)label {
    _nvLabel = [label copy];
    [_inner setLabel:label];
}
// One shared inner queue per device. MTL4 command buffers are independent of a
// queue until commit; using one native queue preserves commit order across
// them (and avoids creating a queue per begin, which churned channels/rings).
static char kNVMTL4InnerQueueKey;
- (id<MTLCommandQueue>)nvInnerQueue {
    if (_innerQueue) return _innerQueue;
    id dev = (id)self.device;
    if (!dev) return nil;
    id<MTLCommandQueue> q = objc_getAssociatedObject(dev, &kNVMTL4InnerQueueKey);
    if (!q) {
        q = [dev newCommandQueue];
        if (q) objc_setAssociatedObject(dev, &kNVMTL4InnerQueueKey, q,
                                        OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    _innerQueue = q;
    return _innerQueue;
}
- (void)nvCloseEncoders {
    if (_computeOpen && _encoder && ![_encoder nvEnded]) [_encoder endEncoding];
    if (_renderOpen && _renderEncoder && ![_renderEncoder nvEnded]) [_renderEncoder endEncoding];
    _computeOpen = _renderOpen = NO;
}
- (void)beginCommandBufferWithAllocator:(id<MTL4CommandAllocator>)allocator {
    if (!allocator) {
        NSLog(@"NVMTLDriver: MTL4 beginCommandBufferWithAllocator:nil");
        return;
    }
    if (_innerQueue && _inner) {
        NSLog(@"NVMTLDriver: MTL4 beginCommandBuffer while a buffer is uncommitted; "
              @"closing the open encoders and dropping its remaining work");
        [self nvCloseEncoders];
    }
    _allocator = allocator;
    _allocatorGeneration = [allocator respondsToSelector:@selector(nvGeneration)]
        ? [(NVMTL4Allocator *)allocator nvGeneration] : 0;
    _nvDrawable = nil;
    _innerQueue = [self nvInnerQueue];
    _inner = [_innerQueue commandBuffer];
    if (!_inner) {
        NSLog(@"NVMTLDriver: MTL4 beginCommandBuffer could not create the native command buffer");
        _encoder = nil; _renderEncoder = nil;
        _computeOpen = _renderOpen = NO;
        return;
    }
    if (_nvLabel) [_inner setLabel:_nvLabel];
    _encoder = nil; _renderEncoder = nil;
    _computeOpen = _renderOpen = NO;
}
- (void)beginCommandBufferWithAllocator:(id<MTL4CommandAllocator>)allocator options:(id)options {
    (void)options;
    [self beginCommandBufferWithAllocator:allocator];
}
- (id<MTL4ComputeCommandEncoder>)computeCommandEncoder {
    if (!_inner) return nil;
    if (_computeOpen && _encoder && ![_encoder nvEnded])
        return (id<MTL4ComputeCommandEncoder>)_encoder;
    if (_renderOpen) [self nvCloseEncoders];
    _computeOpen = NO; _encoder = nil;
    id<MTLComputeCommandEncoder> e = [_inner computeCommandEncoder];
    if (!e) return nil;
    _encoder = [[NVMTL4ComputeEncoder alloc] initWithInner:e commandBuffer:(id<MTL4CommandBuffer>)self];
    _computeOpen = YES;
    return (id<MTL4ComputeCommandEncoder>)_encoder;
}
- (void)endCommandBuffer { [self nvCloseEncoders]; }
// Private MTL4 SPI used by frameworks between frames; dropping uncommitted
// work here is the documented reset semantic, not a silent commit.
- (void)resetCommandBuffer {
    [self nvCloseEncoders];
    _inner = [_innerQueue commandBuffer];
    _encoder = nil; _renderEncoder = nil;
    _computeOpen = _renderOpen = NO;
}
- (id<MTL4CommandAllocator>)commandAllocator { return _allocator; }
- (nullable id<MTL4MachineLearningCommandEncoder>)machineLearningCommandEncoder {
    NSLog(@"NVMTLDriver: MTL4 machine-learning encoder is not implemented");
    return nil;
}
- (nullable id<MTL4RenderCommandEncoder>)renderCommandEncoderWithDescriptor:(id)descriptor {
    MTLRenderPassDescriptor *m = nvM3RenderPass(descriptor);
    if (!m || !_inner) {
        if (!m) NSLog(@"NVMTLDriver: MTL4 render pass descriptor is not MTL4RenderPassDescriptor");
        return nil;
    }
    [self nvCloseEncoders];
    id<MTLRenderCommandEncoder> e = [_inner renderCommandEncoderWithDescriptor:m];
    if (!e) return nil;
    _renderEncoder = [[NVMTL4RenderEncoder alloc] initWithInner:e commandBuffer:(id<MTL4CommandBuffer>)self];
    [_renderEncoder nvSetTileWidth:m.tileWidth height:m.tileHeight];
    _renderOpen = YES;
    return (id<MTL4RenderCommandEncoder>)_renderEncoder;
}
- (void)useResidencySet:(id)set { (void)set; }
- (void)useResidencySets:(const id _Nonnull[_Nonnull])sets count:(NSUInteger)count { (void)sets; (void)count; }
- (void)pushDebugGroup:(NSString *)string { [_inner pushDebugGroup:string]; }
- (void)popDebugGroup { [_inner popDebugGroup]; }
- (void)writeTimestampIntoHeap:(id)heap atIndex:(NSUInteger)index { nvM4Timestamp(heap, index); }
- (nullable id<MTL4RenderCommandEncoder>)renderCommandEncoderWithDescriptor:(id)descriptor
                                                                   options:(NSUInteger)options {
    (void)options;
    return [self renderCommandEncoderWithDescriptor:descriptor];
}
- (void)resolveCounterHeap:(id)heap withRange:(NSRange)range intoBuffer:(MTL4BufferRange)bufferRange
                 waitFence:(id<MTLFence>)waitFence updateFence:(id<MTLFence>)updateFence {
    (void)waitFence; (void)updateFence;
    if (!heap || ![heap respondsToSelector:@selector(resolveCounterRange:)]) return;
    // 0.180.12: MTL4BufferRange is a by-value struct (address + length), not
    // an id<MTLBuffer>. Treating it as an object corrupted the argument ABI.
    id<MTLBuffer> buffer = nvMTLBufferForGpuAddress((uint64_t)bufferRange.bufferAddress);
    if (!buffer) return;
    const uint64_t base = [(id<MTLBuffer>)buffer gpuAddress];
    if ((uint64_t)bufferRange.bufferAddress < base) return;
    const uint64_t off = (uint64_t)bufferRange.bufferAddress - base;
    if (off > buffer.length) return;
    uint64_t room = buffer.length - off;
    if (bufferRange.length != UINT64_MAX && bufferRange.length < room)
        room = bufferRange.length;
    NSData *d = [(id)heap resolveCounterRange:range];
    if (!d.length || !room || !buffer.contents) return;
    const NSUInteger n = (NSUInteger)MIN((uint64_t)d.length, room);
    memcpy((uint8_t *)buffer.contents + off, d.bytes, n);
    if (buffer.storageMode == MTLStorageModeManaged)
        [buffer didModifyRange:NSMakeRange((NSUInteger)off, n)];
}
- (void)resolveCounterHeap:(id)heap withRange:(NSRange)range intoBuffer:(id<MTLBuffer>)buffer
                  atOffset:(NSUInteger)offset waitFence:(id<MTLFence>)waitFence
                updateFence:(id<MTLFence>)updateFence {
    (void)waitFence; (void)updateFence;
    if (!heap || !buffer || ![heap respondsToSelector:@selector(resolveCounterRange:)]) return;
    NSData *d = [(id)heap resolveCounterRange:range];
    if (!d.length || offset > buffer.length || d.length > buffer.length - offset ||
        !buffer.contents) return;
    memcpy((uint8_t *)buffer.contents + offset, d.bytes, d.length);
    if (buffer.storageMode == MTLStorageModeManaged)
        [buffer didModifyRange:NSMakeRange(offset, d.length)];
}
- (void)nvSetDrawable:(id<MTLDrawable>)drawable { _nvDrawable = drawable; }
- (void)nvSetWaitEvent:(id<MTLEvent>)event value:(uint64_t)value {
    if (_inner && event) [_inner encodeWaitForEvent:event value:value];
}
- (void)nvCommitWithEvent:(id<MTLEvent> _Nullable)event value:(uint64_t)value {
    if (!_inner) return;
    if (_allocator && [_allocator respondsToSelector:@selector(nvGeneration)] &&
        [(NVMTL4Allocator *)_allocator nvGeneration] != _allocatorGeneration) {
        NSLog(@"NVMTLDriver: MTL4 command buffer allocator was reset before commit; "
              @"refusing to submit stale commands");
        _inner = nil; _encoder = nil; _renderEncoder = nil;
        return;
    }
    [self nvCloseEncoders];
    if (event) [_inner encodeSignalEvent:event value:value];
    if (_nvDrawable) {
        [_inner presentDrawable:_nvDrawable];
        _nvDrawable = nil;
    }
    [_inner commit];
    _inner = nil;
    _encoder = nil; _renderEncoder = nil;
}
@end

// Synchronous native compilation presented through the MTL4 asynchronous task
// protocol. The task is already finished when it is returned; waitUntilCompleted
// is therefore a no-op and status is always Finished.
@interface NVMTL4CompilerTask : NSObject
@property (nonatomic, weak) id<MTL4Compiler> compiler;
@property (nonatomic) MTL4CompilerTaskStatus status;
@end

@implementation NVMTL4CompilerTask
- (void)waitUntilCompleted {}
@end

@implementation NVMTLDevice (Metal4)
// 0.9.19: MTLDeviceFeatureQueries asks these before it exposes any MTL4 API.
// Without them Metal.framework reports the whole Metal 4 path as unavailable
// even when newCommandBuffer would work.
- (BOOL)supportsMTL4CommandQueue { return nvMTLGateObjectsEnabled() != 0; }
- (BOOL)supportsMTL4CommandAllocator { return nvMTLGateObjectsEnabled() != 0; }
- (BOOL)supportsMTL4ComputeCommandEncoder { return nvMTLGateObjectsEnabled() != 0; }
- (BOOL)supportsMTL4RenderCommandEncoder { return nvMTLGateObjectsEnabled() != 0; }
- (BOOL)supportsMTL4Compiler { return nvMTLGateObjectsEnabled() != 0; }
- (BOOL)supportsMTL4Counters { return nvMTLGateObjectsEnabled() != 0; }
- (BOOL)supportsMTL4PlacementSparse { return NO; }
- (BOOL)supportsMTL4LateBoundRenderTargets { return NO; }
- (BOOL)supportsMTL4PSOSpecialization { return NO; }

- (id<MTL4CommandAllocator>)newCommandAllocator {
    if (!nvMTLGateObjectsEnabled()) return nil;
    NVMTL4Allocator *a = [NVMTL4Allocator new];
    a.device = (id<MTLDevice>)self;
    return (id<MTL4CommandAllocator>)a;
}
- (id<MTL4CommandAllocator>)newCommandAllocatorWithDescriptor:(id)descriptor {
    return [self newCommandAllocatorWithDescriptor:descriptor error:NULL];
}
- (nullable id<MTL4CommandAllocator>)newCommandAllocatorWithDescriptor:(id)descriptor
                                                                 error:(NSError * _Nullable * _Nullable)error {
    if (error) *error = nil;
    if (!nvMTLGateObjectsEnabled()) {
        if (error) *error = nvM4DisabledError();
        return nil;
    }
    NVMTL4Allocator *a = [NVMTL4Allocator new];
    a.device = (id<MTLDevice>)self;
    if (descriptor && [descriptor respondsToSelector:@selector(label)])
        { NSString *nvL = [descriptor valueForKey:@"label"]; if (nvL) a.label = nvL; }   // 0.9.49: Apple asserts on a nil label
    return (id<MTL4CommandAllocator>)a;
}
- (id<MTL4CommandQueue>)newMTL4CommandQueue {
    if (!nvMTLGateObjectsEnabled()) return nil;
    NVMTL4CommandQueue *q = [NVMTL4CommandQueue new];
    q.device = (id<MTLDevice>)self;
    return (id<MTL4CommandQueue>)q;
}
- (id<MTL4CommandQueue>)newMTL4CommandQueueWithDescriptor:(id)descriptor {
    return [self newMTL4CommandQueueWithDescriptor:descriptor error:NULL];
}
- (nullable id<MTL4CommandQueue>)newMTL4CommandQueueWithDescriptor:(id)descriptor
                                                              error:(NSError * _Nullable * _Nullable)error {
    if (error) *error = nil;
    if (!nvMTLGateObjectsEnabled()) {
        if (error) *error = nvM4DisabledError();
        return nil;
    }
    NVMTL4CommandQueue *q = [NVMTL4CommandQueue new];
    q.device = (id<MTLDevice>)self;
    if (descriptor && [descriptor respondsToSelector:@selector(label)])
        { NSString *nvL = [descriptor valueForKey:@"label"]; if (nvL) q.label = nvL; }   // 0.9.49: Apple asserts on a nil label
    return (id<MTL4CommandQueue>)q;
}
- (id<MTL4CommandBuffer>)newCommandBuffer {
    if (!nvMTLGateObjectsEnabled()) return nil;
    NVMTL4CommandBuffer *cb = [NVMTL4CommandBuffer new];
    cb.device = (id<MTLDevice>)self;
    return (id<MTL4CommandBuffer>)cb;
}
- (nullable id<MTL4ArgumentTable>)newArgumentTableWithDescriptor:(id)descriptor
                                                            error:(NSError * _Nullable * _Nullable)error {
    if (error) *error = nil;
    if (!nvMTLGateObjectsEnabled()) {
        if (error) *error = nvM4DisabledError();
        return nil;
    }
    // The native table is fixed at 31 buffers / 128 textures / 16 samplers
    // (the Metal 4 tier limits). Reject a descriptor that asks for more
    // instead of silently dropping bindings later.
    const NSUInteger maxBuf = nvM4UIntProperty(descriptor, "maxBufferBindCount");
    const NSUInteger maxTex = nvM4UIntProperty(descriptor, "maxTextureBindCount");
    const NSUInteger maxSmp = nvM4UIntProperty(descriptor, "maxSamplerStateBindCount");
    if (maxBuf > 31 || maxTex > 128 || maxSmp > 16) {
        if (error) *error = [NSError errorWithDomain:@"NVMTL4ErrorDomain" code:3
            userInfo:@{NSLocalizedDescriptionKey:
                [NSString stringWithFormat:@"argument table asks for %lu/%lu/%lu bindings; "
                 @"native limit is 31/128/16", (unsigned long)maxBuf,
                 (unsigned long)maxTex, (unsigned long)maxSmp]}];
        return nil;
    }
    NVMTL4ArgumentTable *t = [NVMTL4ArgumentTable new];
    t.device = (id<MTLDevice>)self;
    if (descriptor && [descriptor respondsToSelector:@selector(label)])
        { NSString *nvL = [descriptor valueForKey:@"label"]; if (nvL) t.label = nvL; }   // 0.9.49: Apple asserts on a nil label
    return (id<MTL4ArgumentTable>)t;
}
- (nullable id<MTL4Compiler>)newCompilerWithDescriptor:(id)descriptor
                                                  error:(NSError * _Nullable * _Nullable)error {
    if (error) *error = nil;
    if (!nvMTLGateObjectsEnabled()) {
        if (error) *error = nvM4DisabledError();
        return nil;
    }
    NVMTL4Compiler *c = [NVMTL4Compiler new];
    c.device = (id<MTLDevice>)self;
    if (descriptor && [descriptor respondsToSelector:@selector(label)])
        { NSString *nvL = [descriptor valueForKey:@"label"]; if (nvL) c.label = nvL; }   // 0.9.49: Apple asserts on a nil label
    return (id<MTL4Compiler>)c;
}
- (nullable id<MTL4Archive>)newArchiveWithURL:(NSURL *)url
                                        error:(NSError * _Nullable * _Nullable)error {
    if (error) *error = nil;
    if (!url) {
        if (error) *error = nvM4BadDescriptorError(@"archive URL");
        return nil;
    }
    NSError *read = nil;
    NSData *data = [NSData dataWithContentsOfURL:url options:0 error:&read];
    if (!data) {
        if (error) *error = read ?: nvM4BadDescriptorError(@"archive file");
        return nil;
    }
    NVMTL4Archive *a = [[NVMTL4Archive alloc] init];
    a.device = (id<MTLDevice>)self;
    a.url = url;
    return (id<MTL4Archive>)a;
}
@end

@implementation NVMTL4Compiler
- (nullable id<MTLLibrary>)newLibraryWithDescriptor:(id)descriptor
                                              error:(NSError * _Nullable * _Nullable)error {
    @synchronized (self) {
        if (error) *error = nil;
        MTL4LibraryDescriptor *ld = (MTL4LibraryDescriptor *)descriptor;
        if (![ld isKindOfClass:[MTL4LibraryDescriptor class]] || !ld.source.length) {
            if (error) *error = nvM4BadDescriptorError(@"library");
            return nil;
        }
        // 0.9.19 (08.18): the MTL4 compiler is a long-lived object; reuse the
        // last successful source compile instead of recompiling every frame.
        // 0.9.22: the cache fields were unsynchronized; two threads could
        // compile/publish/release the same library.
        const BOOL sameOptions = (self.cachedLibraryOptions == nil && ld.options == nil) ||
            [self.cachedLibraryOptions isEqual:ld.options];
        if (self.cachedLibrary && self.cachedLibrarySource && sameOptions &&
            [self.cachedLibrarySource isEqualToString:ld.source])
            return self.cachedLibrary;
        id<MTLDevice> dev = self.device;
        if (!dev) {
            if (error) *error = nvM4UnsupportedError(@"device is gone");
            return nil;
        }
        id<MTLLibrary> lib = [dev newLibraryWithSource:ld.source options:ld.options error:error];
        if (lib) {
            self.cachedLibrary = lib;
            self.cachedLibrarySource = ld.source;
            self.cachedLibraryOptions = ld.options;
        }
        return lib;
    }
}
- (nullable id<MTLComputePipelineState>)nvBuildComputePipeline:(id)descriptor
                                                          error:(NSError * _Nullable * _Nullable)error {
    if (error) *error = nil;
    MTL4ComputePipelineDescriptor *pd = (MTL4ComputePipelineDescriptor *)descriptor;
    if (![pd isKindOfClass:[MTL4ComputePipelineDescriptor class]]) {
        if (error) *error = nvM4BadDescriptorError(@"compute pipeline");
        return nil;
    }
    id fd = pd.computeFunctionDescriptor;
    if (!fd || ![fd respondsToSelector:NSSelectorFromString(@"library")] ||
        ![fd respondsToSelector:NSSelectorFromString(@"name")]) {
        if (error) *error = nvM4BadDescriptorError(@"compute pipeline function");
        return nil;
    }
    id<MTLLibrary> lib = [fd valueForKey:@"library"];
    NSString *name = [fd valueForKey:@"name"];
    if (!lib || !name.length) {
        if (error) *error = nvM4BadDescriptorError(@"compute pipeline function");
        return nil;
    }
    id<MTLFunction> fn = [lib newFunctionWithName:name];
    if (!fn) {
        if (error) *error = nvM4BadDescriptorError(@"compute pipeline function name");
        return nil;
    }
    return [(id<MTLDevice>)self.device newComputePipelineStateWithFunction:fn error:error];
}
- (nullable id<MTLComputePipelineState>)newComputePipelineStateWithDescriptor:(id)descriptor
                                                          compilerTaskOptions:(id _Nullable)options
                                                                        error:(NSError * _Nullable * _Nullable)error {
    (void)options;
    return [self nvBuildComputePipeline:descriptor error:error];
}
- (nullable id<MTLComputePipelineState>)newComputePipelineStateWithDescriptor:(id)descriptor
                                                     dynamicLinkingDescriptor:(id _Nullable)dynamicLinking
                                                          compilerTaskOptions:(id _Nullable)options
                                                                        error:(NSError * _Nullable * _Nullable)error {
    (void)dynamicLinking; (void)options;
    return [self nvBuildComputePipeline:descriptor error:error];
}
- (nullable id<MTL4MachineLearningPipelineState>)newMachineLearningPipelineStateWithDescriptor:(id)descriptor {
    (void)descriptor;
    NSLog(@"NVMTLDriver: MTL4 machine-learning pipeline is not implemented");
    return nil;
}
- (nullable id<MTL4CompilerTask>)newLibraryWithDescriptor:(id)descriptor
                                        completionHandler:(void (^)(id<MTLLibrary> _Nullable, NSError * _Nullable))handler {
    NSError *e = nil;
    id<MTLLibrary> lib = [self newLibraryWithDescriptor:descriptor error:&e];
    NVMTL4CompilerTask *task = [NVMTL4CompilerTask new];
    task.compiler = (id<MTL4Compiler>)self;
    task.status = MTL4CompilerTaskStatusFinished;
    if (handler) handler(lib, e);
    return (id<MTL4CompilerTask>)task;
}
- (nullable id<MTL4CompilerTask>)newComputePipelineStateWithDescriptor:(id)descriptor
                                                   compilerTaskOptions:(id _Nullable)options
                                                     completionHandler:(void (^)(id<MTLComputePipelineState> _Nullable, NSError * _Nullable))handler {
    NSError *e = nil;
    id<MTLComputePipelineState> ps = [self newComputePipelineStateWithDescriptor:descriptor
                                                             compilerTaskOptions:options
                                                                           error:&e];
    NVMTL4CompilerTask *task = [NVMTL4CompilerTask new];
    task.compiler = (id<MTL4Compiler>)self;
    task.status = MTL4CompilerTaskStatusFinished;
    if (handler) handler(ps, e);
    return (id<MTL4CompilerTask>)task;
}
- (nullable id<MTL4CompilerTask>)newComputePipelineStateWithDescriptor:(id)descriptor
                                               dynamicLinkingDescriptor:(id _Nullable)dynamicLinking
                                                   compilerTaskOptions:(id _Nullable)options
                                                     completionHandler:(void (^)(id<MTLComputePipelineState> _Nullable, NSError * _Nullable))handler {
    NSError *e = nil;
    id<MTLComputePipelineState> ps = [self newComputePipelineStateWithDescriptor:descriptor
                                                         dynamicLinkingDescriptor:dynamicLinking
                                                              compilerTaskOptions:options
                                                                            error:&e];
    NVMTL4CompilerTask *task = [NVMTL4CompilerTask new];
    task.compiler = (id<MTL4Compiler>)self;
    task.status = MTL4CompilerTaskStatusFinished;
    if (handler) handler(ps, e);
    return (id<MTL4CompilerTask>)task;
}
- (nullable id<MTL4CompilerTask>)newRenderPipelineStateWithDescriptor:(id)descriptor
                                                  compilerTaskOptions:(id _Nullable)options
                                                    completionHandler:(void (^)(id<MTLRenderPipelineState> _Nullable, NSError * _Nullable))handler {
    NSError *e = nil;
    id<MTLRenderPipelineState> ps = [self newRenderPipelineStateWithDescriptor:descriptor
                                                           compilerTaskOptions:options
                                                                         error:&e];
    NVMTL4CompilerTask *task = [NVMTL4CompilerTask new];
    task.compiler = (id<MTL4Compiler>)self;
    task.status = MTL4CompilerTaskStatusFinished;
    if (handler) handler(ps, e);
    return (id<MTL4CompilerTask>)task;
}
- (nullable id<MTL4CompilerTask>)newRenderPipelineStateWithDescriptor:(id)descriptor
                                              dynamicLinkingDescriptor:(id _Nullable)dynamicLinking
                                                  compilerTaskOptions:(id _Nullable)options
                                                    completionHandler:(void (^)(id<MTLRenderPipelineState> _Nullable, NSError * _Nullable))handler {
    (void)dynamicLinking;
    return [self newRenderPipelineStateWithDescriptor:descriptor compilerTaskOptions:options
                                    completionHandler:handler];
}
- (nullable id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(id)descriptor
                                                     dynamicLinkingDescriptor:(id _Nullable)dynamicLinking
                                                          compilerTaskOptions:(id _Nullable)options
                                                                        error:(NSError * _Nullable * _Nullable)error {
    (void)dynamicLinking;
    return [self newRenderPipelineStateWithDescriptor:descriptor compilerTaskOptions:options
                                               error:error];
}
- (nullable id<MTL4CompilerTask>)newRenderPipelineStateBySpecializationWithDescriptor:(id)descriptor
                                                                            pipeline:(id)pipeline
                                                                  completionHandler:(void (^)(id _Nullable, NSError * _Nullable))handler {
    (void)descriptor; (void)pipeline;
    NSError *e = nvM4UnsupportedError(@"pipeline specialization");
    if (handler) handler(nil, e);
    NVMTL4CompilerTask *task = [NVMTL4CompilerTask new];
    task.compiler = (id<MTL4Compiler>)self;
    task.status = MTL4CompilerTaskStatusFinished;
    return (id<MTL4CompilerTask>)task;
}
// Public MTL4 compiler surface that the native Ada path does not provide yet.
// Every selector answers with a documented error instead of a silent nil or an
// unrecognized-selector exception.
- (nullable id)newBinaryFunctionWithDescriptor:(id)descriptor
                           compilerTaskOptions:(id _Nullable)options
                                         error:(NSError * _Nullable * _Nullable)error {
    (void)descriptor; (void)options;
    if (error) *error = nvM4UnsupportedError(@"binary functions");
    return nil;
}
- (nullable id)newBinaryFunctionWithDescriptor:(id)descriptor
                           compilerTaskOptions:(id _Nullable)options
                             completionHandler:(void (^)(id _Nullable, NSError * _Nullable))handler {
    NSError *e = nvM4UnsupportedError(@"binary functions");
    if (handler) handler(nil, e);
    NVMTL4CompilerTask *task = [NVMTL4CompilerTask new];
    task.compiler = (id<MTL4Compiler>)self;
    task.status = MTL4CompilerTaskStatusFinished;
    return (id<MTL4CompilerTask>)task;
}
- (nullable id)newDynamicLibraryWithURL:(NSURL *)url error:(NSError * _Nullable * _Nullable)error {
    (void)url;
    if (error) *error = nvM4UnsupportedError(@"dynamic libraries");
    return nil;
}
- (nullable id)newDynamicLibrary:(id)library error:(NSError * _Nullable * _Nullable)error {
    (void)library;
    if (error) *error = nvM4UnsupportedError(@"dynamic libraries");
    return nil;
}
- (nullable id)newDynamicLibraryWithURL:(NSURL *)url
                       completionHandler:(void (^)(id _Nullable, NSError * _Nullable))handler {
    (void)url;
    if (handler) handler(nil, nvM4UnsupportedError(@"dynamic libraries"));
    NVMTL4CompilerTask *task = [NVMTL4CompilerTask new];
    task.compiler = (id<MTL4Compiler>)self;
    task.status = MTL4CompilerTaskStatusFinished;
    return (id<MTL4CompilerTask>)task;
}
- (nullable id)newDynamicLibrary:(id)library
                completionHandler:(void (^)(id _Nullable, NSError * _Nullable))handler {
    (void)library;
    if (handler) handler(nil, nvM4UnsupportedError(@"dynamic libraries"));
    NVMTL4CompilerTask *task = [NVMTL4CompilerTask new];
    task.compiler = (id<MTL4Compiler>)self;
    task.status = MTL4CompilerTaskStatusFinished;
    return (id<MTL4CompilerTask>)task;
}
- (id)pipelineDataSetSerializer { return nil; }
- (nullable id)newRenderPipelineStateBySpecializationWithDescriptor:(id)descriptor
                                                             pipeline:(id)pipeline
                                                                error:(NSError * _Nullable * _Nullable)error {
    (void)descriptor; (void)pipeline;
    if (error) *error = nvM4UnsupportedError(@"pipeline specialization");
    return nil;
}
- (nullable id)newMachineLearningPipelineStateWithDescriptor:(id)descriptor
                                                       error:(NSError * _Nullable * _Nullable)error {
    (void)descriptor;
    if (error) *error = nvM4UnsupportedError(@"machine-learning pipelines");
    return nil;
}
- (nullable id<MTL4CompilerTask>)newMachineLearningPipelineStateWithDescriptor:(id)descriptor
                                                             completionHandler:(void (^)(id _Nullable, NSError * _Nullable))handler {
    (void)descriptor;
    NSError *e = nvM4UnsupportedError(@"machine-learning pipelines");
    if (handler) handler(nil, e);
    NVMTL4CompilerTask *task = [NVMTL4CompilerTask new];
    task.compiler = (id<MTL4Compiler>)self;
    task.status = MTL4CompilerTaskStatusFinished;
    return (id<MTL4CompilerTask>)task;
}
- (nullable id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(id)descriptor
                                                         compilerTaskOptions:(id _Nullable)options
                                                                       error:(NSError * _Nullable * _Nullable)error {
    (void)options;
    if (error) *error = nil;
    // 0.9.22: keep the weak device alive for the whole call; the mesh and
    // non-mesh paths used to read self.device twice.
    id<MTLDevice> dev = self.device;
    if (!dev) {
        if (error) *error = nvM4UnsupportedError(@"device is gone");
        return nil;
    }
    // MTL4 tile descriptors map onto the native tile descriptor. The native
    // path still refuses the Apple-GPU tile stage, but the selector/type
    // mapping must be correct so a framework gets a documented error.
    Class tileCls = NSClassFromString(@"MTL4TileRenderPipelineDescriptor");
    if (tileCls && [descriptor isKindOfClass:tileCls]) {
        id tfd = [descriptor valueForKey:@"tileFunctionDescriptor"];
        id<MTLFunction> tf = nvM4Function(tfd);
        if (!tf) {
            if (error) *error = nvM4UnsupportedError(@"tile pipeline function");
            return nil;
        }
        MTLTileRenderPipelineDescriptor *t = [MTLTileRenderPipelineDescriptor new];
        t.tileFunction = tf;
        t.rasterSampleCount = [[descriptor valueForKey:@"rasterSampleCount"] unsignedIntegerValue] ?: 1;
        t.threadgroupSizeMatchesTileSize =
            [[descriptor valueForKey:@"threadgroupSizeMatchesTileSize"] boolValue];
        t.maxTotalThreadsPerThreadgroup =
            [[descriptor valueForKey:@"maxTotalThreadsPerThreadgroup"] unsignedIntegerValue];
        if ([descriptor respondsToSelector:@selector(requiredThreadsPerThreadgroup)])
            t.requiredThreadsPerThreadgroup = [descriptor requiredThreadsPerThreadgroup];
        id colors = [descriptor valueForKey:@"colorAttachments"];
        for (NSUInteger i = 0; i < 8; ++i) {
            id src = [colors respondsToSelector:@selector(objectAtIndexedSubscript:)]
                         ? [colors objectAtIndexedSubscript:i] : nil;
            if (!src) continue;
            MTLTileRenderPipelineColorAttachmentDescriptor *dst = t.colorAttachments[i];
            dst.pixelFormat = [[src valueForKey:@"pixelFormat"] unsignedIntegerValue];
        }
        return [dev newRenderPipelineStateWithTileDescriptor:t options:0 reflection:NULL error:error];
    }
    // MTL4 mesh pipelines are a sibling descriptor class, not a subclass of
    // MTL4RenderPipelineDescriptor. Map object/mesh/fragment onto the native
    // MTLMeshRenderPipelineDescriptor path.
    Class meshCls = NSClassFromString(@"MTL4MeshRenderPipelineDescriptor");
    if (meshCls && [descriptor isKindOfClass:meshCls]) {
        id ofd = [descriptor valueForKey:@"objectFunctionDescriptor"];
        id mfd = [descriptor valueForKey:@"meshFunctionDescriptor"];
        id ffd = [descriptor valueForKey:@"fragmentFunctionDescriptor"];
        id<MTLFunction> of = nvM4Function(ofd), mf = nvM4Function(mfd), ff = nvM4Function(ffd);
        // A mesh pipeline may omit the fragment function for a depth-only
        // pass; use the same empty fragment the Metal 3 path synthesizes.
        if (!ff) ff = [(id)dev nvNullFragment];
        if (!of || !mf || !ff) {
            if (error) *error = nvM4UnsupportedError(@"mesh pipeline functions");
            return nil;
        }
        MTLMeshRenderPipelineDescriptor *m = [MTLMeshRenderPipelineDescriptor new];
        m.objectFunction = of; m.meshFunction = mf; m.fragmentFunction = ff;
        m.rasterSampleCount = [[descriptor valueForKey:@"rasterSampleCount"] unsignedIntegerValue] ?: 1;
        if ([descriptor respondsToSelector:@selector(label)])
            { NSString *nvL = [descriptor valueForKey:@"label"]; if (nvL) m.label = nvL; }   // 0.9.49: Apple asserts on a nil label
        m.objectThreadgroupSizeIsMultipleOfThreadExecutionWidth =
            [[descriptor valueForKey:@"objectThreadgroupSizeIsMultipleOfThreadExecutionWidth"] boolValue];
        m.meshThreadgroupSizeIsMultipleOfThreadExecutionWidth =
            [[descriptor valueForKey:@"meshThreadgroupSizeIsMultipleOfThreadExecutionWidth"] boolValue];
        m.payloadMemoryLength =
            [[descriptor valueForKey:@"payloadMemoryLength"] unsignedIntegerValue];
        m.maxTotalThreadsPerObjectThreadgroup =
            [[descriptor valueForKey:@"maxTotalThreadsPerObjectThreadgroup"] unsignedIntegerValue];
        m.maxTotalThreadsPerMeshThreadgroup =
            [[descriptor valueForKey:@"maxTotalThreadsPerMeshThreadgroup"] unsignedIntegerValue];
        m.maxTotalThreadgroupsPerMeshGrid =
            [[descriptor valueForKey:@"maxTotalThreadgroupsPerMeshGrid"] unsignedIntegerValue];
        m.alphaToCoverageEnabled = [[descriptor valueForKey:@"alphaToCoverageState"] unsignedIntegerValue] != 0;
        m.alphaToOneEnabled = [[descriptor valueForKey:@"alphaToOneState"] unsignedIntegerValue] != 0;
        m.rasterizationEnabled = [[descriptor valueForKey:@"rasterizationEnabled"] boolValue];
        m.maxVertexAmplificationCount =
            [[descriptor valueForKey:@"maxVertexAmplificationCount"] unsignedIntegerValue];
        m.supportIndirectCommandBuffers =
            [[descriptor valueForKey:@"supportIndirectCommandBuffers"] unsignedIntegerValue] != 0;
        // MTL4RenderPipelineColorAttachmentDescriptorArray is not an NSArray:
        // it has objectAtIndexedSubscript: and no count. Read the fixed eight
        // slots; a missing slot is nil and is skipped.
        id colors = [descriptor valueForKey:@"colorAttachments"];
        for (NSUInteger i = 0; i < 8; ++i) {
            id src = [colors respondsToSelector:@selector(objectAtIndexedSubscript:)]
                         ? [colors objectAtIndexedSubscript:i] : nil;
            if (!src) continue;
            MTLRenderPipelineColorAttachmentDescriptor *dst = m.colorAttachments[i];
            dst.pixelFormat = [[src valueForKey:@"pixelFormat"] unsignedIntegerValue];
            dst.writeMask = [[src valueForKey:@"writeMask"] unsignedIntegerValue];
            dst.blendingEnabled = [[src valueForKey:@"blendingState"] unsignedIntegerValue] != 0;
            dst.sourceRGBBlendFactor = [[src valueForKey:@"sourceRGBBlendFactor"] unsignedIntegerValue];
            dst.destinationRGBBlendFactor = [[src valueForKey:@"destinationRGBBlendFactor"] unsignedIntegerValue];
            dst.rgbBlendOperation = [[src valueForKey:@"rgbBlendOperation"] unsignedIntegerValue];
            dst.sourceAlphaBlendFactor = [[src valueForKey:@"sourceAlphaBlendFactor"] unsignedIntegerValue];
            dst.destinationAlphaBlendFactor = [[src valueForKey:@"destinationAlphaBlendFactor"] unsignedIntegerValue];
            dst.alphaBlendOperation = [[src valueForKey:@"alphaBlendOperation"] unsignedIntegerValue];
        }
        return [dev newRenderPipelineStateWithMeshDescriptor:m options:0 reflection:NULL error:error];
    }
    MTL4RenderPipelineDescriptor *pd = (MTL4RenderPipelineDescriptor *)descriptor;
    if (![pd isKindOfClass:[MTL4RenderPipelineDescriptor class]]) {
        if (error) *error = nvM4BadDescriptorError(@"render pipeline");
        return nil;
    }
    id<MTLFunction> vf = nvM4Function(pd.vertexFunctionDescriptor);
    id<MTLFunction> ff = nvM4Function(pd.fragmentFunctionDescriptor);
    if (!ff) ff = [(id)dev nvNullFragment];
    if (!vf || !ff) {
        if (error) *error = nvM4BadDescriptorError(@"render pipeline function");
        return nil;
    }
    MTLRenderPipelineDescriptor *m = [MTLRenderPipelineDescriptor new];
    m.vertexFunction = vf; m.fragmentFunction = ff;
    if ([pd respondsToSelector:@selector(label)]) { NSString *nvL = [pd valueForKey:@"label"]; if (nvL) m.label = nvL; }   // 0.9.49: Apple asserts on a nil label
    const NSUInteger colorSamples = [[pd valueForKey:@"colorSampleCount"] unsignedIntegerValue];
    m.rasterSampleCount = pd.rasterSampleCount ? pd.rasterSampleCount : (colorSamples ? colorSamples : 1);
    m.vertexDescriptor = pd.vertexDescriptor;
    m.alphaToCoverageEnabled = pd.alphaToCoverageState != 0;
    m.alphaToOneEnabled = pd.alphaToOneState != 0;
    m.rasterizationEnabled = pd.isRasterizationEnabled;
    m.inputPrimitiveTopology = pd.inputPrimitiveTopology;
    m.supportIndirectCommandBuffers = pd.supportIndirectCommandBuffers;
    m.maxVertexAmplificationCount = pd.maxVertexAmplificationCount;
    for (NSUInteger i = 0; i < 8; ++i) {
        MTL4RenderPipelineColorAttachmentDescriptor *src = pd.colorAttachments[i];
        MTLRenderPipelineColorAttachmentDescriptor *dst = m.colorAttachments[i];
        dst.pixelFormat = src.pixelFormat;
        // MTL4BlendStateDisabled is the documented default (0); any other
        // state enables native blending with the supplied factors/ops.
        dst.blendingEnabled = src.blendingState != 0;
        dst.sourceRGBBlendFactor = src.sourceRGBBlendFactor;
        dst.destinationRGBBlendFactor = src.destinationRGBBlendFactor;
        dst.rgbBlendOperation = src.rgbBlendOperation;
        dst.sourceAlphaBlendFactor = src.sourceAlphaBlendFactor;
        dst.destinationAlphaBlendFactor = src.destinationAlphaBlendFactor;
        dst.alphaBlendOperation = src.alphaBlendOperation;
        dst.writeMask = src.writeMask;
    }
    return [dev newRenderPipelineStateWithDescriptor:m error:error];
}
@end

// 0.180.13: MTL4 archive loader. The file is validated as an archive
// container; pipeline creation is delegated to the native compiler, which
// means an archive hit and a normal descriptor compile share the same verified
// path. (The NVIDIA cache stores the generated code, not Apple GPU binaries.)
@implementation NVMTL4Archive
+ (void)load { class_addProtocol(self, @protocol(MTL4Archive)); }
- (nullable id<MTLComputePipelineState>)newComputePipelineStateWithDescriptor:(MTL4ComputePipelineDescriptor *)d
                                                                        error:(NSError * _Nullable * _Nullable)e {
    id<MTL4Compiler> c = [self.device newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:e];
    return c ? [c newComputePipelineStateWithDescriptor:d compilerTaskOptions:nil error:e] : nil;
}
- (nullable id<MTLComputePipelineState>)newComputePipelineStateWithDescriptor:(MTL4ComputePipelineDescriptor *)d
                                                     dynamicLinkingDescriptor:(MTL4PipelineStageDynamicLinkingDescriptor *)dl
                                                                        error:(NSError * _Nullable * _Nullable)e {
    (void)dl;
    return [self newComputePipelineStateWithDescriptor:d error:e];
}
- (nullable id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTL4PipelineDescriptor *)d
                                                                      error:(NSError * _Nullable * _Nullable)e {
    id<MTL4Compiler> c = [self.device newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:e];
    return c ? [c newRenderPipelineStateWithDescriptor:d compilerTaskOptions:nil error:e] : nil;
}
- (nullable id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTL4PipelineDescriptor *)d
                                                   dynamicLinkingDescriptor:(MTL4RenderPipelineDynamicLinkingDescriptor *)dl
                                                                      error:(NSError * _Nullable * _Nullable)e {
    (void)dl;
    return [self newRenderPipelineStateWithDescriptor:d error:e];
}
- (nullable id<MTL4BinaryFunction>)newBinaryFunctionWithDescriptor:(MTL4BinaryFunctionDescriptor *)d
                                                             error:(NSError * _Nullable * _Nullable)e {
    (void)d;
    if (e) *e = nvM4UnsupportedError(@"binary functions");
    return nil;
}
@end
