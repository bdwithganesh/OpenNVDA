// CPU <-> GPU coherence of one Shared buffer across command buffers: the CPU
// writes a new pattern, the GPU checks it and writes its own, the CPU checks
// that, 300 rounds, with the GPU re-reading lines it read the round before
// (a GPU cache that keeps sysmem lines must drop them between command
// buffers). Also a GPU->GPU pass through Shared memory within one buffer.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

static NSString *const kSrc =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "kernel void chk(device uint *b [[buffer(0)]], device atomic_uint *bad [[buffer(1)]], constant uint &want [[buffer(2)]],\n"
     "                uint g [[thread_position_in_grid]]) {\n"
     "  if (b[g] != want + g) atomic_fetch_add_explicit(bad, 1, memory_order_relaxed);\n"
     "  b[g] = want * 3 + g;\n"
     "}\n"
     "kernel void cp(device const uint *s [[buffer(0)]], device uint *d [[buffer(1)]], uint g [[thread_position_in_grid]]) { d[g] = s[g] + 1; }\n";

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:nil error:&e];
        id<MTLComputePipelineState> chk = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"chk"] error:&e];
        id<MTLComputePipelineState> cp = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"cp"] error:&e];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        const NSUInteger n = 1 << 16;
        id<MTLBuffer> b = [dev newBufferWithLength:n * 4 options:MTLResourceStorageModeShared];
        id<MTLBuffer> bad = [dev newBufferWithLength:4 options:MTLResourceStorageModeShared];
        uint32_t *p = b.contents;
        int gpuSawStale = 0, cpuSawStale = 0;
        for (uint32_t round = 1; round <= 300; round++) {
            for (NSUInteger i = 0; i < n; i++) p[i] = round + (uint32_t)i;
            *(uint32_t *)bad.contents = 0;
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            [ce setComputePipelineState:chk];
            [ce setBuffer:b offset:0 atIndex:0];
            [ce setBuffer:bad offset:0 atIndex:1];
            [ce setBytes:&round length:4 atIndex:2];
            [ce dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
            [ce endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            if (*(uint32_t *)bad.contents) gpuSawStale++;
            for (NSUInteger i = 0; i < n; i++) if (p[i] != round * 3 + (uint32_t)i) { cpuSawStale++; break; }
        }
        // GPU -> GPU through Shared memory, two dispatches in one command buffer
        id<MTLBuffer> c = [dev newBufferWithLength:n * 4 options:MTLResourceStorageModeShared];
        id<MTLBuffer> d = [dev newBufferWithLength:n * 4 options:MTLResourceStorageModeShared];
        for (NSUInteger i = 0; i < n; i++) ((uint32_t *)c.contents)[i] = (uint32_t)i;
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:cp];
        [ce setBuffer:c offset:0 atIndex:0]; [ce setBuffer:d offset:0 atIndex:1];
        [ce dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
        [ce memoryBarrierWithScope:MTLBarrierScopeBuffers];
        [ce setBuffer:d offset:0 atIndex:0]; [ce setBuffer:c offset:0 atIndex:1];
        [ce dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
        [ce endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        int chain = 0;
        for (NSUInteger i = 0; i < n; i++) if (((uint32_t *)c.contents)[i] != (uint32_t)i + 2) { chain++; }
        printf("  rounds with stale data: GPU %d, CPU %d of 300; GPU->GPU chain wrong %d\n", gpuSawStale, cpuSawStale, chain);
        const int fail = gpuSawStale || cpuSawStale || chain;
        printf("metal_coherence_test: %s\n", fail ? "FAIL" : "PASS");
        return fail;
    }
}
