// Anisotropic filtering: a mip chain whose level L holds the value L, sampled
// with a long, thin footprint (gradients 32:1 and 8:1). Without anisotropy the
// long axis picks the level; with 16x the short one does, so the value drops.
// Checked against what the M1 samples.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

static NSString *const kSrc =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "kernel void k(texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], device float *o [[buffer(0)]], uint i [[thread_position_in_grid]]) {\n"
     "  const float2 g[4] = { float2(32, 1), float2(8, 1), float2(4, 4), float2(1, 16) };\n"
     "  const float2 d = g[i] / 256.0;\n"
     "  o[i] = t.sample(s, float2(0.5), gradient2d(float2(d.x, 0), float2(0, d.y))).r; }\n";

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:nil error:&e];
        id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"k"] error:&e];
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float width:256 height:256 mipmapped:YES];
        id<MTLTexture> t = [dev newTextureWithDescriptor:td];
        for (NSUInteger l = 0; l < t.mipmapLevelCount; l++) {
            const NSUInteger w = MAX(256u >> l, 1u);
            float *px = malloc(w * w * 4);
            for (NSUInteger i = 0; i < w * w; i++) px[i] = (float)l;
            [t replaceRegion:MTLRegionMake2D(0, 0, w, w) mipmapLevel:l withBytes:px bytesPerRow:w * 4];
            free(px);
        }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        printf("%s\n", dev.name.UTF8String);
        // what the M1 samples (the level picked per footprint and anisotropy)
        static const float want[3][4] = {{5, 3, 2, 4}, {3, 1, 2, 2}, {1, 0, 2, 0}};
        int bad = 0;
        for (int a = 0; a < 3; a++) {
            const NSUInteger an = a == 0 ? 1 : a == 1 ? 4 : 16;
            MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
            sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
            sd.mipFilter = MTLSamplerMipFilterLinear;
            sd.maxAnisotropy = an;
            id<MTLSamplerState> s = [dev newSamplerStateWithDescriptor:sd];
            id<MTLBuffer> o = [dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            [ce setComputePipelineState:ps];
            [ce setTexture:t atIndex:0];
            [ce setSamplerState:s atIndex:0];
            [ce setBuffer:o offset:0 atIndex:0];
            [ce dispatchThreads:MTLSizeMake(4, 1, 1) threadsPerThreadgroup:MTLSizeMake(4, 1, 1)];
            [ce endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            const float *v = o.contents;
            printf("  aniso %2lu: 32:1 %.2f  8:1 %.2f  4:4 %.2f  1:16 %.2f\n", (unsigned long)an, v[0], v[1], v[2], v[3]);
            for (int i = 0; i < 4; i++) bad += fabsf(v[i] - want[a][i]) > 0.25f;
        }
        printf("metal_aniso_test: %s\n", bad ? "FAIL" : "PASS");
        return bad != 0;
    }
}
