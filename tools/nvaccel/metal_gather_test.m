// gather at the texture edge with clamp_to_edge (constexpr and runtime
// samplers), with and without an offset: every tap must be the edge texel.
// MetalFX's scaler (air.gather_texture_2d) came out with a dark 2-pixel
// border on the RTX, fine on Apple GPUs.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

static NSString *const kSrc =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "constexpr sampler cs(address::clamp_to_edge, filter::linear);\n"
     "kernel void g(texture2d<half> t [[texture(0)]], sampler rs [[sampler(0)]], device float4 *o [[buffer(0)]]) {\n"
     "  float2 uv = float2(0.0, 0.0);\n"   // corner: taps at -0.5 texel reach outside
     "  o[0] = float4(t.gather(cs, uv));\n"
     "  o[1] = float4(t.gather(rs, uv));\n"
     "  o[2] = float4(t.gather(cs, uv, int2(-1, -1)));\n"
     "  o[3] = float4(t.gather(rs, uv, int2(-1, -1)));\n"
     "  o[4] = float4(t.sample(cs, uv));\n"
     "  o[5] = float4(t.sample(rs, uv));\n"
     "}\n";

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:nil error:&e];
        id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"g"] error:&e];
        if (!ps) { printf("pipeline: %s\n", e.localizedDescription.UTF8String); return 1; }
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                      width:8 height:8 mipmapped:NO];
        id<MTLTexture> t = [dev newTextureWithDescriptor:td];
        uint8_t px[8 * 8 * 4];
        for (int i = 0; i < 64; i++) { px[i * 4] = 200; px[i * 4 + 1] = 100; px[i * 4 + 2] = 50; px[i * 4 + 3] = 255; }
        [t replaceRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0 withBytes:px bytesPerRow:32];
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
        id<MTLSamplerState> rs = [dev newSamplerStateWithDescriptor:sd];
        id<MTLBuffer> o = [dev newBufferWithLength:6 * 16 options:MTLResourceStorageModeShared];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:ps];
        [ce setTexture:t atIndex:0];
        [ce setSamplerState:rs atIndex:0];
        [ce setBuffer:o offset:0 atIndex:0];
        [ce dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [ce endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        const char *names[6] = {"gather constexpr", "gather runtime", "gather constexpr offset", "gather runtime offset",
                                "sample constexpr", "sample runtime"};
        const float *f = o.contents;
        int bad = 0;
        for (int k = 0; k < 6; k++) {
            const float want = k < 4 ? 200 / 255.0f : 200 / 255.0f;   // gather: red of 4 taps; sample: red
            int w = 0;
            for (int c = 0; c < (k < 4 ? 4 : 1); c++) if (fabsf(f[k * 4 + c] - want) > 0.01f) w++;
            printf("  %-24s %.3f %.3f %.3f %.3f  %s\n", names[k], f[k * 4], f[k * 4 + 1], f[k * 4 + 2], f[k * 4 + 3],
                   w ? "FAIL" : "PASS");
            bad += w != 0;
        }
        printf("metal_gather_test: %s (%d failed)\n", bad ? "FAIL" : "PASS", bad);
        return bad != 0;
    }
}
