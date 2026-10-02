// metal_batch_test: command buffers the batched compute path has to get
// right. 1) first dispatch without textures, second samples one (texture
// pool made between two launches of one batch). 2) a chain of dependent
// dispatches (serial order). 3) 1000 dispatches, then CPU check.
#import <Metal/Metal.h>
#include <stdio.h>

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = nil;
        for (id<MTLDevice> d in MTLCopyAllDevices())
            if ([d.name containsString:@"RTX"]) dev = d;
        if (!dev) { printf("no RTX Metal device\n"); return 1; }
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "kernel void inc(device uint *o [[buffer(0)]], uint i [[thread_position_in_grid]]) { o[i] += 1; }\n"
             "kernel void rd(texture2d<float> t [[texture(0)]], device float *o [[buffer(0)]],"
             " uint2 p [[thread_position_in_grid]]) { o[p.y * 16 + p.x] = t.read(p).r; }\n"
                                              options:nil error:&err];
        if (!lib) { printf("compile: %s\n", err.description.UTF8String); return 1; }
        id<MTLComputePipelineState> inc = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"inc"] error:&err];
        id<MTLComputePipelineState> rd = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"rd"] error:&err];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        int fail = 0;
        id<MTLBuffer> b = [dev newBufferWithLength:4096 * 4 options:MTLResourceStorageModeShared];
        memset(b.contents, 0, 4096 * 4);
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float
                                                                                     width:16 height:16 mipmapped:NO];
        id<MTLTexture> t = [dev newTextureWithDescriptor:td];
        float px[256];
        for (int i = 0; i < 256; i++) px[i] = i * 0.5f;
        [t replaceRegion:MTLRegionMake2D(0, 0, 16, 16) mipmapLevel:0 withBytes:px bytesPerRow:64];
        id<MTLBuffer> o = [dev newBufferWithLength:256 * 4 options:MTLResourceStorageModeShared];
        // 1 + 2
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:inc];
        [ce setBuffer:b offset:0 atIndex:0];
        for (int i = 0; i < 50; i++)
            [ce dispatchThreads:MTLSizeMake(4096, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [ce setComputePipelineState:rd];
        [ce setTexture:t atIndex:0];
        [ce setBuffer:o offset:0 atIndex:0];
        [ce dispatchThreads:MTLSizeMake(16, 16, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
        [ce endEncoding];
        [cb commit]; [cb waitUntilCompleted];
        const uint32_t *bv = b.contents;
        const float *ov = o.contents;
        for (int i = 0; i < 4096; i++) if (bv[i] != 50) { printf("chain: [%d] = %u, want 50\n", i, bv[i]); fail++; break; }
        for (int i = 0; i < 256; i++) if (ov[i] != i * 0.5f) { printf("tex: [%d] = %f\n", i, ov[i]); fail++; break; }
        // 3
        cb = [q commandBuffer];
        ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:inc];
        [ce setBuffer:b offset:0 atIndex:0];
        for (int i = 0; i < 1000; i++)
            [ce dispatchThreads:MTLSizeMake(4096, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
        [ce endEncoding];
        [cb commit]; [cb waitUntilCompleted];
        for (int i = 0; i < 4096; i++) if (bv[i] != 1050) { printf("1000: [%d] = %u, want 1050\n", i, bv[i]); fail++; break; }
        printf("metal_batch_test: %s (%d failed)\n", fail ? "FAIL" : "PASS", fail);
        return fail != 0;
    }
}
