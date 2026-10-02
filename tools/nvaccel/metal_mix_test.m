// metal_mix_test: draws and compute in one command buffer, compute after a
// draw using threadgroup memory + barriers (what WindowServer does in a
// user session: blur/vibrancy kernels between composites). Checks results.
#import <Metal/Metal.h>
#include <stdio.h>
int main(int argc, char **argv) {
    @autoreleasepool {
        id<MTLDevice> dev = nil;
        for (id<MTLDevice> d in MTLCopyAllDevices()) if ([d.name containsString:@"RTX"]) dev = d;
        if (!dev) { printf("no RTX Metal device\n"); return 1; }
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "struct VO { float4 p [[position]]; };\n"
             "vertex VO vs(uint v [[vertex_id]]) { float2 t[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)}; VO o; o.p = float4(t[v], 0, 1); return o; }\n"
             "fragment float4 fs() { return float4(0.25, 0.5, 0.75, 1); }\n"
             "kernel void red(device const uint *in [[buffer(0)]], device uint *out [[buffer(1)]],"
             " uint l [[thread_position_in_threadgroup]], uint g [[threadgroup_position_in_grid]]) {"
             " threadgroup uint s[256]; s[l] = in[g * 256 + l]; threadgroup_barrier(mem_flags::mem_threadgroup);"
             " for (uint k = 128; k > 0; k >>= 1) { if (l < k) s[l] += s[l + k]; threadgroup_barrier(mem_flags::mem_threadgroup); }"
             " if (l == 0) out[g] = s[0]; }\n"
                                              options:nil error:&err];
        if (!lib) { printf("compile: %s\n", err.description.UTF8String); return 1; }
        MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
        rd.vertexFunction = [lib newFunctionWithName:@"vs"]; rd.fragmentFunction = [lib newFunctionWithName:@"fs"];
        rd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        id<MTLRenderPipelineState> rp = [dev newRenderPipelineStateWithDescriptor:rd error:&err];
        id<MTLComputePipelineState> cp = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"red"] error:&err];
        if (!rp || !cp) { printf("pipelines: %s\n", err.description.UTF8String); return 1; }
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:256 height:256 mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        id<MTLTexture> rt = [dev newTextureWithDescriptor:td];
        const int G = 64;
        id<MTLBuffer> in = [dev newBufferWithLength:G * 256 * 4 options:MTLResourceStorageModeShared];
        id<MTLBuffer> out = [dev newBufferWithLength:G * 4 * 20 options:MTLResourceStorageModeShared];
        uint32_t *iv = in.contents;
        for (int i = 0; i < G * 256; i++) iv[i] = (uint32_t)(i % 7);
        const int rounds = argc > 1 ? atoi(argv[1]) : 20;
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        for (int r = 0; r < rounds; r++) {
            MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
            p.colorAttachments[0].texture = rt; p.colorAttachments[0].loadAction = MTLLoadActionClear;
            p.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:p];
            [re setRenderPipelineState:rp]; [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [re endEncoding];
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            [ce setComputePipelineState:cp]; [ce setBuffer:in offset:0 atIndex:0]; [ce setBuffer:out offset:(r % 20) * G * 4 atIndex:1];
            [ce dispatchThreadgroups:MTLSizeMake(G, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
            [ce endEncoding];
        }
        [cb commit]; [cb waitUntilCompleted];
        const uint32_t *ov = out.contents;
        int bad = 0;
        for (int g = 0; g < G; g++) {
            uint32_t want = 0;
            for (int l = 0; l < 256; l++) want += (uint32_t)((g * 256 + l) % 7);
            if (ov[g] != want) { if (!bad) printf("group %d: %u want %u\n", g, ov[g], want); bad++; }
        }
        printf("metal_mix_test: %s (%d wrong, status %ld)\n", bad ? "FAIL" : "PASS", bad, (long)cb.status);
        return bad != 0;
    }
}
