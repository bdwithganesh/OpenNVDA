#pragma once
// Native Metal 4 object graph, feature-flagged by NVMTL_METAL4=1 or NVRAM
// nvmtl-metal4=1. The family bit is separate: supportsFamily(Metal4) needs
// NVMTL_METAL4_FAMILY=1 or the nvmtl-metal4 receipt written by
// tools/nvaccel/mtl4_conformance.sh after the native compiler/pipeline
// (AIR -> NAK/SASS -> GR) and argument-table matrix pass. See
// NVMTLFamilyGate.h; a per-process NVMTL_METAL4=1 run never advertises the
// family to other applications.
#import <Metal/Metal.h>

NS_ASSUME_NONNULL_BEGIN

@interface NVMTL4Allocator : NSObject
@property (nonatomic, weak) id<MTLDevice> device;
@property (nonatomic, copy, nullable) NSString *label;
// Generation is bumped by reset; a command buffer refuses to commit if its
// allocator was reset after begin (Metal 4 allocator lifetime contract).
- (NSUInteger)nvGeneration;
@end

@interface NVMTL4ArgumentTable : NSObject
@property (nonatomic, weak) id<MTLDevice> device;
@property (nonatomic, copy, nullable) NSString *label;
- (void)setAddress:(MTLGPUAddress)gpuAddress length:(NSUInteger)length atIndex:(NSUInteger)index;
- (void)setAddress:(MTLGPUAddress)gpuAddress atIndex:(NSUInteger)index;
- (void)setAddress:(MTLGPUAddress)gpuAddress attributeStride:(NSUInteger)stride atIndex:(NSUInteger)index;
- (void)setResource:(MTLResourceID)resourceID atBufferIndex:(NSUInteger)index;
- (void)setTexture:(MTLResourceID)resourceID atIndex:(NSUInteger)index;
- (void)setSamplerState:(MTLResourceID)resourceID atIndex:(NSUInteger)index;
- (BOOL)nvAddressAtIndex:(NSUInteger)index address:(MTLGPUAddress *)address
                  length:(NSUInteger *)length;
- (uint64_t)nvTextureAtIndex:(NSUInteger)index;
- (uint64_t)nvSamplerAtIndex:(NSUInteger)index;
- (uint64_t)nvResourceAtIndex:(NSUInteger)index;
- (NSUInteger)nvAttributeStrideAtIndex:(NSUInteger)index;
@end

@interface NVMTL4CommandQueue : NSObject
@property (nonatomic, weak) id<MTLDevice> device;
@property (nonatomic, copy, nullable) NSString *label;
- (void)commit:(const id<MTL4CommandBuffer> _Nonnull[_Nonnull])commandBuffers
         count:(NSUInteger)count;
- (void)signalEvent:(id<MTLEvent>)event value:(uint64_t)value;
- (void)waitForEvent:(id<MTLEvent>)event value:(uint64_t)value;
- (void)copyBufferMappingsFromBuffer:(id<MTLBuffer>)source
                            toBuffer:(id<MTLBuffer>)destination
                          operations:(const void *)operations
                               count:(NSUInteger)count;
- (void)updateBufferMappings:(id<MTLBuffer>)buffer
                        heap:(id<MTLHeap>)heap
                  operations:(const void *)operations
                       count:(NSUInteger)count;
- (void)copyTextureMappingsFromTexture:(id<MTLTexture>)source
                             toTexture:(id<MTLTexture>)destination
                            operations:(const void *)operations
                                 count:(NSUInteger)count;
- (void)updateTextureMappings:(id<MTLTexture>)texture
                         heap:(id<MTLHeap>)heap
                   operations:(const void *)operations
                        count:(NSUInteger)count;
@end

@interface NVMTL4CommandBuffer : NSObject
@property (nonatomic, weak) id<MTLDevice> device;
@property (nonatomic, copy, nullable) NSString *label;
- (void)beginCommandBufferWithAllocator:(id<MTL4CommandAllocator>)allocator;
- (void)beginCommandBufferWithAllocator:(id<MTL4CommandAllocator>)allocator options:(id)options;
- (nullable id<MTL4ComputeCommandEncoder>)computeCommandEncoder;
- (nullable id<MTL4RenderCommandEncoder>)renderCommandEncoderWithDescriptor:(id)descriptor;
- (nullable id<MTL4MachineLearningCommandEncoder>)machineLearningCommandEncoder;
- (void)resolveCounterHeap:(id)heap withRange:(NSRange)range intoBuffer:(MTL4BufferRange)bufferRange
                 waitFence:(id<MTLFence>)waitFence updateFence:(id<MTLFence>)updateFence;
- (void)endCommandBuffer;
- (void)nvSetDrawable:(id<MTLDrawable>)drawable;
- (void)nvSetWaitEvent:(id<MTLEvent>)event value:(uint64_t)value;
- (void)nvCommitWithEvent:(id<MTLEvent> _Nullable)event value:(uint64_t)value;
@end

@interface NVMTL4Compiler : NSObject
@property (nonatomic, weak) id<MTLDevice> device;
@property (nonatomic, copy, nullable) NSString *label;
- (nullable id<MTLLibrary>)newLibraryWithDescriptor:(id)descriptor
                                              error:(NSError * _Nullable * _Nullable)error;
- (nullable id<MTLComputePipelineState>)newComputePipelineStateWithDescriptor:(id)descriptor
                                                          compilerTaskOptions:(id _Nullable)options
                                                                        error:(NSError * _Nullable * _Nullable)error;
- (nullable id<MTLComputePipelineState>)newComputePipelineStateWithDescriptor:(id)descriptor
                                                     dynamicLinkingDescriptor:(id _Nullable)dynamicLinking
                                                          compilerTaskOptions:(id _Nullable)options
                                                                        error:(NSError * _Nullable * _Nullable)error;
- (nullable id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(id)descriptor
                                                         compilerTaskOptions:(id _Nullable)options
                                                                       error:(NSError * _Nullable * _Nullable)error;
- (nullable id<MTL4CompilerTask>)newLibraryWithDescriptor:(id)descriptor
                                        completionHandler:(void (^)(id<MTLLibrary> _Nullable, NSError * _Nullable))handler;
- (nullable id<MTL4CompilerTask>)newComputePipelineStateWithDescriptor:(id)descriptor
                                                   compilerTaskOptions:(id _Nullable)options
                                                     completionHandler:(void (^)(id<MTLComputePipelineState> _Nullable, NSError * _Nullable))handler;
- (nullable id<MTL4CompilerTask>)newRenderPipelineStateWithDescriptor:(id)descriptor
                                                  compilerTaskOptions:(id _Nullable)options
                                                    completionHandler:(void (^)(id<MTLRenderPipelineState> _Nullable, NSError * _Nullable))handler;
- (nullable id<MTL4MachineLearningPipelineState>)newMachineLearningPipelineStateWithDescriptor:(id)descriptor;
@end

// Compile-time shape only; the real NVMTLDevice is MTLIOAccelDevice.
@interface NVMTLDevice : NSObject
@end
@interface NVMTLDevice (Metal4)
- (id<MTL4CommandAllocator>)newCommandAllocator;
- (id<MTL4CommandAllocator>)newCommandAllocatorWithDescriptor:(id)descriptor;
- (nullable id<MTL4CommandAllocator>)newCommandAllocatorWithDescriptor:(id)descriptor
                                                                 error:(NSError * _Nullable * _Nullable)error;
- (id<MTL4CommandQueue>)newMTL4CommandQueue;
- (id<MTL4CommandQueue>)newMTL4CommandQueueWithDescriptor:(id)descriptor;
- (nullable id<MTL4CommandQueue>)newMTL4CommandQueueWithDescriptor:(id)descriptor
                                                              error:(NSError * _Nullable * _Nullable)error;
- (id<MTL4CommandBuffer>)newCommandBuffer;
- (nullable id<MTL4ArgumentTable>)newArgumentTableWithDescriptor:(id)descriptor error:(NSError * _Nullable * _Nullable)error;
- (nullable id<MTL4Compiler>)newCompilerWithDescriptor:(id)descriptor error:(NSError * _Nullable * _Nullable)error;
- (nullable id<MTL4Archive>)newArchiveWithURL:(NSURL *)url error:(NSError * _Nullable * _Nullable)error;
@end

NS_ASSUME_NONNULL_END
