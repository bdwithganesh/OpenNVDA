// metal_deriv_divergent_test: fwidth/dfdx inside control flow that differs between the pixels of
// a 2x2 quad. Metal leaves it undefined in principle, but Apple's GPUs give the right answer and
// RenderBox relies on it for anti-aliased edges (wrong values put partial-alpha lines along
// triangle edges outside shapes, 1 Oct 2026). d = x*x/8 + y: dfdx(d) = (2x+1)/8 at a pixel pair.
// Compared per pixel: uniform branch vs a branch taken by every other pixel.
#import <Metal/Metal.h>
#include <stdio.h>
#include <math.h>
int main(void)
{
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "struct VO { float4 p [[position]]; float2 q; };\n"
             "vertex VO vs(uint v [[vertex_id]]) { float2 t[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)}; VO o;"
             " o.p = float4(t[v], 0, 1); o.q = (t[v] * float2(0.5, -0.5) + 0.5) * 32.0; return o; }\n"
             "fragment float4 uni(VO i [[stage_in]]) { float d = i.q.x * i.q.x / 8.0 + i.q.y; return float4(fwidth(d), dfdx(d), dfdy(d), 1); }\n"
             "fragment float4 div(VO i [[stage_in]]) { float d = i.q.x * i.q.x / 8.0 + i.q.y; float4 r = float4(0, 0, 0, 1);"
             " if (((uint(i.p.x) + uint(i.p.y)) & 1) != 0) { r.xyz = float3(fwidth(d), dfdx(d), dfdy(d)); }"
             " else { float e = d * 1.0; r.xyz = float3(fwidth(e), dfdx(e), dfdy(e)); } return r; }\n" options:nil error:&err];
        if (!lib) { printf("compile: %s\n", err.description.UTF8String); return 1; }
        const int S = 32;
        float *res[2];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        NSArray *names = @[ @"uni", @"div" ];
        for (int k = 0; k < 2; k++) {
            MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
            rd.vertexFunction = [lib newFunctionWithName:@"vs"];
            rd.fragmentFunction = [lib newFunctionWithName:names[k]];
            rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
            id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:rd error:&err];
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:S height:S mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
            id<MTLTexture> t = [dev newTextureWithDescriptor:td];
            id<MTLCommandBuffer> cb = [q commandBuffer];
            MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
            p.colorAttachments[0].texture = t; p.colorAttachments[0].loadAction = MTLLoadActionClear;
            p.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:p];
            [re setRenderPipelineState:ps];
            [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [re endEncoding];
            id<MTLBuffer> rb = [dev newBufferWithLength:S * S * 16 options:MTLResourceStorageModeShared];
            id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
            [bl copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(S, S, 1)
                       toBuffer:rb destinationOffset:0 destinationBytesPerRow:S * 16 destinationBytesPerImage:S * S * 16];
            [bl endEncoding];
            [cb commit]; [cb waitUntilCompleted];
            res[k] = malloc(S * S * 16); memcpy(res[k], rb.contents, S * S * 16);
        }
        int bad = 0;
        for (int i = 0; i < S * S * 4; i++) if (fabsf(res[0][i] - res[1][i]) > 1e-3f) {
            if (bad < 4) printf("  pixel %d ch %d: uniform %.4f divergent %.4f\n", i / 4, i % 4, res[0][i], res[1][i]);
            bad++;
        }
        printf("metal_deriv_divergent_test on %s: %d values differ: %s\n", dev.name.UTF8String, bad, bad ? "FAIL" : "PASS");
        return bad != 0;
    }
}
