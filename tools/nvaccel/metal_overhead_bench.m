// Submission overhead: empty command buffer round trip, a tiny dispatch
// round trip, 1000 tiny dispatches in one command buffer, and 1000 small
// draws in one render pass. Times per item in microseconds.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <mach/mach_time.h>
#include <errno.h>
#include <stdlib.h>

static double us(uint64_t a, uint64_t b) {
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    return (double)(b - a) * tb.numer / tb.denom / 1000.0;
}
static BOOL completed(id<MTLCommandBuffer> cb, const char *phase) {
    [cb waitUntilCompleted];
    if (cb.status == MTLCommandBufferStatusCompleted) return YES;
    fprintf(stderr, "overhead %s: status %lu error %s: FAIL\n", phase,
            (unsigned long)cb.status, cb.error.description.UTF8String ?: "none");
    return NO;
}

static NSString *const kSrc =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "kernel void k(device uint *a [[buffer(0)]], uint i [[thread_position_in_grid]]) { a[i] += 1; }\n"
     "vertex float4 vs(uint v [[vertex_id]]) { return float4(v == 1 ? 1 : -1, v == 2 ? 1 : -1, 0, 1); }\n"
     "fragment float4 fs() { return float4(1, 0, 0, 1); }\n";

int main(void) {
    @autoreleasepool {
        unsigned batchDispatches = 1000;
        const char *requested = getenv("OVH_BATCH_DISPATCHES");
        if (requested) {
            char *end = NULL; errno = 0;
            unsigned long value = strtoul(requested, &end, 10);
            if (errno || !*requested || *end || value < 1 || value > 1000) {
                fprintf(stderr, "OVH_BATCH_DISPATCHES must be 1..1000\n"); return 1;
            }
            batchDispatches = (unsigned)value;
        }
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:nil error:&e];
        id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"k"] error:&e];
        MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
        rd.vertexFunction = [lib newFunctionWithName:@"vs"]; rd.fragmentFunction = [lib newFunctionWithName:@"fs"];
        rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        id<MTLRenderPipelineState> rps = [dev newRenderPipelineStateWithDescriptor:rd error:&e];
        id<MTLBuffer> b = [dev newBufferWithLength:4096 options:MTLResourceStorageModePrivate];
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:256 height:256 mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> rt = [dev newTextureWithDescriptor:td];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        if (!dev || !lib || !ps || !rps || !b || !rt || !q) {
            fprintf(stderr, "overhead resource creation: FAIL %s\n", e.description.UTF8String ?: "none");
            return 1;
        }
        id<MTLBuffer> readback = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
        if (!readback) return 1;
        {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            [blit fillBuffer:b range:NSMakeRange(0, 4096) value:0];
            [blit endEncoding]; [cb commit];
            if (!completed(cb, "initialize")) return 1;
        }
        // warm up
        for (int i = 0; i < 3; i++) { id<MTLCommandBuffer> cb = [q commandBuffer]; [cb commit]; if (!completed(cb, "warmup")) return 1; }
        const int N = 200;
        uint64_t t0 = mach_absolute_time();
        for (int i = 0; i < N; i++) { id<MTLCommandBuffer> cb = [q commandBuffer]; [cb commit]; if (!completed(cb, "empty")) return 1; }
        const double empty = us(t0, mach_absolute_time()) / N;
        t0 = mach_absolute_time();
        for (int i = 0; i < N; i++) {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            [ce setComputePipelineState:ps]; [ce setBuffer:b offset:0 atIndex:0];
            [ce dispatchThreads:MTLSizeMake(64, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            [ce endEncoding]; [cb commit]; if (!completed(cb, "single dispatch")) return 1;
        }
        const double one = us(t0, mach_absolute_time()) / N;
        t0 = mach_absolute_time();
        {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            [ce setComputePipelineState:ps]; [ce setBuffer:b offset:0 atIndex:0];
            for (unsigned i = 0; i < batchDispatches; i++) [ce dispatchThreads:MTLSizeMake(64, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            [ce endEncoding]; [cb commit]; if (!completed(cb, "dispatch batch")) return 1;
        }
        const double many = us(t0, mach_absolute_time()) / batchDispatches;
        {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            [blit copyFromBuffer:b sourceOffset:0 toBuffer:readback destinationOffset:0 size:4096];
            [blit endEncoding]; [cb commit];
            if (!completed(cb, "readback")) return 1;
            const uint32_t *values = readback.contents;
            unsigned wrong = 0;
            for (unsigned i = 0; i < 1024; ++i) wrong += values[i] != (i < 64 ? N + batchDispatches : 0u);
            if (wrong) { fprintf(stderr, "overhead compute output: %u wrong: FAIL\n", wrong); return 1; }
        }
        // OVH_LOOP=d|r: repeat the dispatch / draw batch forever (for sample(1))
        const char *loop = getenv("OVH_LOOP");
        while (loop && loop[0] == 'd') {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            [ce setComputePipelineState:ps]; [ce setBuffer:b offset:0 atIndex:0];
            for (unsigned i = 0; i < batchDispatches; i++) [ce dispatchThreads:MTLSizeMake(64, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            [ce endEncoding]; [cb commit]; if (!completed(cb, "dispatch loop")) return 1;
        }
        t0 = mach_absolute_time();
        {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor new];
            rp.colorAttachments[0].texture = rt; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
            [re setRenderPipelineState:rps];
            for (int i = 0; i < 1000; i++) [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [re endEncoding]; [cb commit]; if (!completed(cb, "draw batch")) return 1;
        }
        const double draws = us(t0, mach_absolute_time()) / 1000;
        while (loop && loop[0] == 'r') {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor new];
            rp.colorAttachments[0].texture = rt; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
            [re setRenderPipelineState:rps];
            for (int i = 0; i < 1000; i++) [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [re endEncoding]; [cb commit]; if (!completed(cb, "draw loop")) return 1;
        }
        printf("%s: empty cb %.1f us, 1-dispatch cb %.1f us, dispatch in batch %.2f us, draw in pass %.2f us\n",
               dev.name.UTF8String, empty, one, many, draws);
        printf("overhead dispatch batch: %u dispatches\n", batchDispatches);
        puts("overhead command buffers and compute output: PASS");
        return 0;
    }
}
