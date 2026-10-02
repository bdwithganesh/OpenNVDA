// metal_blurchain_test: the login blur chain in one command buffer. A
// fullscreen draw into A, then downsample passes (each samples the last
// render target into a smaller one) and blur passes back up, ~30 passes,
// every pass reading the previous pass's colour. Checks the final colour.
#import <Metal/Metal.h>
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
             "struct VO { float4 p [[position]]; float2 uv; };\n"
             "vertex VO vs(uint v [[vertex_id]]) { float2 t[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)}; VO o; o.p = float4(t[v], 0, 1); o.uv = t[v] * float2(0.5, -0.5) + 0.5; return o; }\n"
             "fragment float4 fill(constant float4 &c [[buffer(0)]]) { return c; }\n"
             "fragment float4 blur(VO i [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant float2 &px [[buffer(0)]], constant int &taps [[buffer(1)]]) {"
             " float4 a = 0; for (int k = -taps; k <= taps; k++) a += t.sample(s, i.uv + float2(k, 0) * px); return a / float(2 * taps + 1); }\n"
                                              options:nil error:&err];
        if (!lib) { printf("compile: %s\n", err.description.UTF8String); return 1; }
        MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
        rd.vertexFunction = [lib newFunctionWithName:@"vs"];
        rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
        rd.fragmentFunction = [lib newFunctionWithName:@"fill"];
        id<MTLRenderPipelineState> pf = [dev newRenderPipelineStateWithDescriptor:rd error:&err];
        rd.fragmentFunction = [lib newFunctionWithName:@"blur"];
        id<MTLRenderPipelineState> pb = [dev newRenderPipelineStateWithDescriptor:rd error:&err];
        if (!pf || !pb) { printf("pipelines: %s\n", err.description.UTF8String); return 1; }
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
        id<MTLSamplerState> smp = [dev newSamplerStateWithDescriptor:sd];
        const int sizes[] = {1024, 512, 256, 128, 64, 128, 256, 512, 1024};
        const int ns = sizeof sizes / sizeof sizes[0];
        NSMutableArray *rts = [NSMutableArray new];
        for (int i = 0; i < ns; i++) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                                         width:sizes[i] height:sizes[i] mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModePrivate;
            [rts addObject:[dev newTextureWithDescriptor:td]];
        }
        MTLTextureDescriptor *od = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:1024 height:1024 mipmapped:NO];
        od.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        id<MTLTexture> outT = [dev newTextureWithDescriptor:od];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        const int frames = argc > 1 ? atoi(argv[1]) : 10;
        id<MTLCommandBuffer> cb = nil;
        for (int f = 0; f < frames; f++) {
            cb = [q commandBuffer];
            float col[4] = {0.2f, 0.4f, 0.6f, 1.0f};
            for (int i = 0; i < ns; i++) {
                MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
                p.colorAttachments[0].texture = rts[i]; p.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                p.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:p];
                if (i == 0) { [re setRenderPipelineState:pf]; [re setFragmentBytes:col length:16 atIndex:0]; }
                else {
                    [re setRenderPipelineState:pb]; [re setFragmentTexture:rts[i - 1] atIndex:0]; [re setFragmentSamplerState:smp atIndex:0];
                    float px[2] = {1.0f / sizes[i - 1], 1.0f / sizes[i - 1]}; int taps = 5 + (i % 4) * 2;
                    [re setFragmentBytes:px length:8 atIndex:0]; [re setFragmentBytes:&taps length:4 atIndex:1];
                }
                [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [re endEncoding];
            }
            MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
            p.colorAttachments[0].texture = outT; p.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            p.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:p];
            [re setRenderPipelineState:pb]; [re setFragmentTexture:rts[ns - 1] atIndex:0]; [re setFragmentSamplerState:smp atIndex:0];
            float px[2] = {1.0f / 1024, 1.0f / 1024}; int taps = 3;
            [re setFragmentBytes:px length:8 atIndex:0]; [re setFragmentBytes:&taps length:4 atIndex:1];
            [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [re endEncoding];
            [cb commit];
        }
        [cb waitUntilCompleted];
        uint16_t px4[4] = {0};
        [outT getBytes:px4 bytesPerRow:8 fromRegion:MTLRegionMake2D(512, 512, 1, 1) mipmapLevel:0];
        const float g = (float)(__fp16)*(__fp16 *)&px4[1];
        const bool ok = g > 0.35f && g < 0.45f;
        printf("metal_blurchain_test: %s (green %.3f, status %ld)\n", ok ? "PASS" : "FAIL", g, (long)cb.status);
        return !ok;
    }
}
