// Metal "advanced" pieces on NVMTLDriver (M19), CPU-verified.
//   sudo metal_adv_test
// 1 MTLHeap: buffers from a heap, used by a kernel; budget enforced
// 2 indirect dispatch whose group count a kernel wrote earlier in the same CB
// 3 indirect draw (MTLDrawPrimitivesIndirectArguments)
// 4 instanced draw: [[instance_id]] shifts each copy
#import <Metal/Metal.h>
#include <stdio.h>

static id<MTLDevice> findNV(void) {
    for (id<MTLDevice> d in MTLCopyAllDevices())
        if ([d.name containsString:@"NVIDIA"]) return d;
    return nil;
}
static int fails;
static void check(const char *what, int ok) {
    printf("  %-60s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) fails++;
}

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = findNV();
        if (!dev) { printf("no NVIDIA MTLDevice\n"); return 1; }
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:
            @"#include <metal_stdlib>\nusing namespace metal;\n"
            @"kernel void fill(device uint *b [[buffer(0)]], uint i [[thread_position_in_grid]]) { b[i] = i * 3u + 1u; }\n"
            @"kernel void setcount(device uint *c [[buffer(0)]], uint i [[thread_position_in_grid]]) {"
            @"  if (i == 0u) { c[0] = 5u; c[1] = 1u; c[2] = 1u; } }\n"
            @"kernel void mark(device uint *b [[buffer(0)]], uint i [[thread_position_in_grid]]) { b[i] = 7u; }\n"
            @"struct VOut { float4 position [[position]]; float4 color; };\n"
            @"vertex VOut vs(device const float4 *pos [[buffer(0)]], uint vid [[vertex_id]]) {"
            @"  VOut o; o.position = pos[vid]; o.color = float4(0.0, 0.0, 1.0, 1.0); return o; }\n"
            @"fragment float4 fs(VOut in [[stage_in]]) { return in.color; }\n" options:nil error:&e];
        if (!lib) { printf("library: %s\n", e.localizedDescription.UTF8String); return 1; }
        id<MTLComputePipelineState> pFill = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"fill"] error:&e];
        id<MTLComputePipelineState> pCount = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"setcount"] error:&e];
        id<MTLComputePipelineState> pMark = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"mark"] error:&e];
        id<MTLCommandQueue> q = [dev newCommandQueue];

        // 1
        {
            MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
            hd.size = 1 << 20;
            hd.storageMode = MTLStorageModeShared;
            id<MTLHeap> h = [dev newHeapWithDescriptor:hd];
            id<MTLBuffer> b = [h newBufferWithLength:4096 * 4 options:MTLResourceStorageModeShared];
            int ok = h && b;
            if (ok) {
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
                [ce setComputePipelineState:pFill];
                [ce setBuffer:b offset:0 atIndex:0];
                [ce dispatchThreads:MTLSizeMake(4096, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
                [ce endEncoding];
                [cb commit]; [cb waitUntilCompleted];
                const uint32_t *v = b.contents;
                for (int i = 0; i < 4096 && ok; i++) ok = v[i] == (uint32_t)i * 3 + 1;
            }
            check("heap buffer written by a kernel", ok);
            id<MTLBuffer> big = [h newBufferWithLength:2 << 20 options:MTLResourceStorageModeShared];
            check("heap refuses an allocation past its size", big == nil && [h maxAvailableSizeWithAlignment:256] > 0);
        }
        // 2
        {
            id<MTLBuffer> cnt = [dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
            id<MTLBuffer> b = [dev newBufferWithLength:4096 * 4 options:MTLResourceStorageModeShared];
            memset(cnt.contents, 0, 16); memset(b.contents, 0, 4096 * 4);
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            [ce setComputePipelineState:pCount];
            [ce setBuffer:cnt offset:0 atIndex:0];
            [ce dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
            [ce setComputePipelineState:pMark];
            [ce setBuffer:b offset:0 atIndex:0];
            [ce dispatchThreadgroupsWithIndirectBuffer:cnt indirectBufferOffset:0
                                 threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            [ce endEncoding];
            [cb commit]; [cb waitUntilCompleted];
            const uint32_t *v = b.contents;
            int ok = 1;
            for (int i = 0; i < 4096 && ok; i++) ok = v[i] == (i < 5 * 64 ? 7u : 0u);
            check("indirect dispatch: 5 groups x 64 from a GPU-written count", ok);
        }
        // 3
        {
            MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
            rd.vertexFunction = [lib newFunctionWithName:@"vs"];
            rd.fragmentFunction = [lib newFunctionWithName:@"fs"];
            rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:rd error:&e];
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                          width:32 height:32 mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget;
            id<MTLTexture> rt = [dev newTextureWithDescriptor:td];
            // 6 verts: a junk triangle then the real one; the args start at vertex 3
            const float pos[] = {5, 5, 0, 1,  6, 5, 0, 1,  5, 6, 0, 1,
                                 -1, -1, 0, 1,  1, -1, 0, 1,  -1, 1, 0, 1};
            id<MTLBuffer> vb = [dev newBufferWithBytes:pos length:sizeof pos options:MTLResourceStorageModeShared];
            const uint32_t args[4] = {3, 1, 3, 0};
            id<MTLBuffer> ab = [dev newBufferWithBytes:args length:16 options:MTLResourceStorageModeShared];
            uint8_t in[4] = {0}, out[4] = {0};
            if (ps && rt) {
                MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                rp.colorAttachments[0].texture = rt;
                rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                rp.colorAttachments[0].clearColor = (MTLClearColor){0, 0, 0, 1};
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
                [re setRenderPipelineState:ps];
                [re setVertexBuffer:vb offset:0 atIndex:0];
                [re drawPrimitives:MTLPrimitiveTypeTriangle indirectBuffer:ab indirectBufferOffset:0];
                [re endEncoding];
                [cb commit]; [cb waitUntilCompleted];
                [rt getBytes:in bytesPerRow:128 fromRegion:MTLRegionMake2D(2, 29, 1, 1) mipmapLevel:0];
                [rt getBytes:out bytesPerRow:128 fromRegion:MTLRegionMake2D(30, 2, 1, 1) mipmapLevel:0];
            }
            check("indirect draw from vertex 3", in[2] == 255 && in[0] == 0 && out[2] == 0 && out[3] == 255);
        }
    }
    @autoreleasepool {   // 4
        id<MTLDevice> dev = findNV();
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:
            @"#include <metal_stdlib>\nusing namespace metal;\n"
            @"struct VOut { float4 position [[position]]; float4 color; };\n"
            @"vertex VOut vsi(device const float4 *pos [[buffer(0)]], uint vid [[vertex_id]], uint iid [[instance_id]]) {"
            @"  VOut o; o.position = pos[vid] + float4(float(iid) * 1.0, 0.0, 0.0, 0.0); o.color = float4(0.0, 1.0, 0.0, 1.0); return o; }\n"
            @"fragment float4 fs(VOut in [[stage_in]]) { return in.color; }\n" options:nil error:&e];
        MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
        rd.vertexFunction = [lib newFunctionWithName:@"vsi"];
        rd.fragmentFunction = [lib newFunctionWithName:@"fs"];
        rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:rd error:&e];
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                      width:32 height:32 mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> rt = [dev newTextureWithDescriptor:td];
        // left half triangle; instance 1 is the same shifted right by 1.0 (half the target)
        const float pos[] = {-1, -1, 0, 1,  -0.1, -1, 0, 1,  -1, 1, 0, 1};
        id<MTLBuffer> vb = [dev newBufferWithBytes:pos length:sizeof pos options:MTLResourceStorageModeShared];
        uint8_t l[4] = {0}, r[4] = {0};
        if (ps && rt) {
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = rt;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].clearColor = (MTLClearColor){0, 0, 0, 1};
            id<MTLCommandQueue> q = [dev newCommandQueue];
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
            [re setRenderPipelineState:ps];
            [re setVertexBuffer:vb offset:0 atIndex:0];
            const uint16_t ix[3] = {0, 1, 2};
            id<MTLBuffer> ib = [dev newBufferWithBytes:ix length:6 options:MTLResourceStorageModeShared];
            [re drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:3 indexType:MTLIndexTypeUInt16
                          indexBuffer:ib indexBufferOffset:0 instanceCount:2];
            [re endEncoding];
            [cb commit]; [cb waitUntilCompleted];
            [rt getBytes:l bytesPerRow:128 fromRegion:MTLRegionMake2D(1, 30, 1, 1) mipmapLevel:0];
            [rt getBytes:r bytesPerRow:128 fromRegion:MTLRegionMake2D(17, 30, 1, 1) mipmapLevel:0];
        }
        check("indexed instanced draw: 2 instances via [[instance_id]]", l[1] == 255 && r[1] == 255);
    }
    printf("metal_adv_test: %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
