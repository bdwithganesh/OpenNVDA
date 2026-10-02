// Features the device claims, exercised: BC1 compressed texture sampling and
// the MetalFX spatial upscaler (Apple's own shaders through nakc).
//   metal_feature_test
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#import <Foundation/Foundation.h>

static NSString *const kSrc =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "kernel void samp(texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]],"
     " device float4 *o [[buffer(0)]], uint2 p [[thread_position_in_grid]]) {"
     " o[p.y * 4 + p.x] = t.sample(s, (float2(p) + 0.5) / 4.0); }\n";

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> q = [dev newCommandQueue];
        NSError *e = nil;
        int bad = 0;
        // --- BC1: one 4x4 block, colour0 red, colour1 blue, top two rows index 0, bottom two index 1
        {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBC1_RGBA
                                                                                          width:4 height:4 mipmapped:NO];
            id<MTLTexture> t = [dev newTextureWithDescriptor:td];
            const uint8_t blk[8] = {0x00, 0xF8, 0x1F, 0x00, 0x00, 0x00, 0x55, 0x55};
            [t replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 withBytes:blk bytesPerRow:8];
            id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:nil error:&e];
            id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"samp"] error:&e];
            MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
            id<MTLSamplerState> ss = [dev newSamplerStateWithDescriptor:sd];
            id<MTLBuffer> o = [dev newBufferWithLength:16 * 16 options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            [ce setComputePipelineState:ps];
            [ce setTexture:t atIndex:0];
            [ce setSamplerState:ss atIndex:0];
            [ce setBuffer:o offset:0 atIndex:0];
            [ce dispatchThreads:MTLSizeMake(4, 4, 1) threadsPerThreadgroup:MTLSizeMake(4, 4, 1)];
            [ce endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            const float *f = o.contents;
            int wrong = 0;
            for (int i = 0; i < 16; i++) {
                const bool top = i < 8;
                const float r = f[i * 4], b = f[i * 4 + 2];
                if ((top && (r < 0.95f || b > 0.05f)) || (!top && (r > 0.05f || b < 0.95f))) wrong++;
            }
            printf("  BC1 sample: %d of 16 texels wrong (texel0 %.2f %.2f %.2f %.2f)  %s\n", wrong, f[0], f[1], f[2], f[3],
                   wrong ? "FAIL" : "PASS");
            bad += wrong != 0;
        }
        // --- MetalFX spatial scaler 64x64 -> 128x128 of a flat colour
        {
            MTLFXSpatialScalerDescriptor *d = [MTLFXSpatialScalerDescriptor new];
            d.inputWidth = d.inputHeight = 64;
            d.outputWidth = d.outputHeight = 128;
            d.colorTextureFormat = d.outputTextureFormat = MTLPixelFormatRGBA8Unorm;
            d.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;
            id<MTLFXSpatialScaler> sc = [d newSpatialScalerWithDevice:dev];
            if (!sc) { printf("  MetalFX spatial scaler: not made  FAIL\n"); bad++; }
            else {
                MTLTextureDescriptor *ti = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                              width:64 height:64 mipmapped:NO];
                ti.usage = sc.colorTextureUsage;
                id<MTLTexture> in = [dev newTextureWithDescriptor:ti];
                NSMutableData *px = [NSMutableData dataWithLength:64 * 64 * 4];
                uint8_t *p = px.mutableBytes;
                for (int i = 0; i < 64 * 64; i++) { p[i * 4] = 200; p[i * 4 + 1] = 100; p[i * 4 + 2] = 50; p[i * 4 + 3] = 255; }
                [in replaceRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0 withBytes:p bytesPerRow:256];
                MTLTextureDescriptor *to = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                              width:128 height:128 mipmapped:NO];
                to.usage = sc.outputTextureUsage;
                to.storageMode = MTLStorageModeManaged;
                id<MTLTexture> out = [dev newTextureWithDescriptor:to];
                sc.colorTexture = in;
                sc.outputTexture = out;
                id<MTLCommandBuffer> cb = [q commandBuffer];
                [sc encodeToCommandBuffer:cb];
                id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
                [bl synchronizeResource:out];
                [bl endEncoding];
                [cb commit];
                [cb waitUntilCompleted];
                NSMutableData *o = [NSMutableData dataWithLength:128 * 128 * 4];
                [out getBytes:o.mutableBytes bytesPerRow:512 fromRegion:MTLRegionMake2D(0, 0, 128, 128) mipmapLevel:0];
                const uint8_t *q8 = o.bytes;
                int wrong = 0;
                for (int i = 0; i < 128 * 128; i++)
                    if (abs(q8[i * 4] - 200) > 6 || abs(q8[i * 4 + 1] - 100) > 6 || abs(q8[i * 4 + 2] - 50) > 6) wrong++;
                printf("  MetalFX spatial 64->128: %d of 16384 pixels off (px0 %d %d %d %d, status %ld)  %s\n", wrong, q8[0],
                       q8[1], q8[2], q8[3], (long)cb.status, wrong ? "FAIL" : "PASS");
                bad += wrong != 0;
            }
        }
        printf("metal_feature_test: %s (%d failed)\n", bad ? "FAIL" : "PASS", bad);
        return bad != 0;
    }
}
