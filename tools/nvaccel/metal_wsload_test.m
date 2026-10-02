// metal_wsload_test: a WindowServer-like load. Every round: an IOSurface
// texture is drawn into, sampled by a compute kernel into a buffer, and
// dropped right after commit (only every 8th round waits). Run several at once.
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    @autoreleasepool {
        id<MTLDevice> dev = nil;
        for (id<MTLDevice> d in MTLCopyAllDevices()) if ([d.name containsString:@"RTX"]) dev = d;
        if (!dev) { printf("no RTX Metal device\n"); return 1; }
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "struct VO { float4 p [[position]]; };\n"
             "vertex VO vs(uint v [[vertex_id]], constant float &s [[buffer(0)]]) { float2 t[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)}; VO o; o.p = float4(t[v] * s, 0, 1); return o; }\n"
             "fragment float4 fs(constant float4 &c [[buffer(0)]]) { return c; }\n"
             "kernel void blur(texture2d<float> t [[texture(0)]], device float *o [[buffer(0)]], uint2 p [[thread_position_in_grid]]) {"
             " float a = 0; for (int dy = -2; dy <= 2; dy++) for (int dx = -2; dx <= 2; dx++) a += t.read(uint2(clamp(int2(p) + int2(dx, dy), int2(0), int2(127)))).g;"
             " o[p.y * 128 + p.x] = a / 25.0; }\n"
                                              options:nil error:&err];
        if (!lib) { printf("compile: %s\n", err.description.UTF8String); return 1; }
        MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
        rd.vertexFunction = [lib newFunctionWithName:@"vs"]; rd.fragmentFunction = [lib newFunctionWithName:@"fs"];
        rd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        id<MTLRenderPipelineState> rp = [dev newRenderPipelineStateWithDescriptor:rd error:&err];
        id<MTLComputePipelineState> cp = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"blur"] error:&err];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        const int rounds = argc > 1 ? atoi(argv[1]) : 300;
        int bad = 0;
        for (int r = 0; r < rounds; r++) @autoreleasepool {
            NSDictionary *props = @{(id)kIOSurfaceWidth: @128, (id)kIOSurfaceHeight: @128, (id)kIOSurfaceBytesPerElement: @4,
                                    (id)kIOSurfacePixelFormat: @((uint32_t)'BGRA')};
            IOSurfaceRef io = IOSurfaceCreate((__bridge CFDictionaryRef)props);
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:128 height:128 mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            id<MTLTexture> t = [dev newTextureWithDescriptor:td iosurface:io plane:0];
            id<MTLBuffer> out = [dev newBufferWithLength:128 * 128 * 4 options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> cb = [q commandBuffer];
            MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
            p.colorAttachments[0].texture = t; p.colorAttachments[0].loadAction = MTLLoadActionClear;
            p.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:p];
            float sc = 1.0f; float col[4] = {0.1f, 0.6f, 0.2f, 1.0f};
            [re setRenderPipelineState:rp];
            [re setVertexBytes:&sc length:4 atIndex:0]; [re setFragmentBytes:col length:16 atIndex:0];
            [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [re endEncoding];
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            [ce setComputePipelineState:cp]; [ce setTexture:t atIndex:0]; [ce setBuffer:out offset:0 atIndex:0];
            [ce dispatchThreads:MTLSizeMake(128, 128, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
            [ce endEncoding];
            [cb commit];
            if ((r & 7) == 7) {
                [cb waitUntilCompleted];
                const float v = ((const float *)out.contents)[64 * 128 + 64];
                if (v < 0.55f || v > 0.65f) { if (!bad) printf("round %d: %f\n", r, v); bad++; }
            }
            t = nil; out = nil; CFRelease(io);      // dropped while the GPU may still run it
        }
        id<MTLCommandBuffer> last = [q commandBuffer]; [last commit]; [last waitUntilCompleted];
        printf("metal_wsload_test: %s (%d bad)\n", bad ? "FAIL" : "PASS", bad);
        return bad != 0;
    }
}
