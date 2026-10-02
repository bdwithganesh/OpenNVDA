// sRGB textures must come back linear from read() and sample(): 128 in an
// RGBA8Unorm_sRGB texel reads as 0.2159 (not 0.502). Core Image uploads
// images as sRGB IOSurface textures and relies on it (QuickLook thumbnails
// came out washed out). Plain texture and IOSurface texture, read + sample.
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <Foundation/Foundation.h>

static NSString *const kSrc =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "kernel void k(texture2d<float, access::read> t [[texture(0)]], texture2d<float> s [[texture(1)]], sampler sm [[sampler(0)]],\n"
     "              device float4 *o [[buffer(0)]], uint g [[thread_position_in_grid]]) {\n"
     "  if (g == 0) o[0] = t.read(uint2(1, 1)); else o[1] = s.sample(sm, float2(0.5, 0.5)); }\n";

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:nil error:&e];
        id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"k"] error:&e];
        id<MTLSamplerState> sm = [dev newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        uint8_t px[4 * 4 * 4];
        for (int i = 0; i < 64; i++) px[i] = 128;
        int bad = 0;
        for (int kind = 0; kind < 2; kind++) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB width:4 height:4 mipmapped:NO];
            td.usage = MTLTextureUsageShaderRead;
            id<MTLTexture> t;
            IOSurfaceRef io = NULL;
            if (kind) {
                io = IOSurfaceCreate((__bridge CFDictionaryRef)@{(id)kIOSurfaceWidth: @4, (id)kIOSurfaceHeight: @4,
                                                                  (id)kIOSurfaceBytesPerElement: @4, (id)kIOSurfacePixelFormat: @('RGBA')});
                IOSurfaceLock(io, 0, NULL);
                for (int y = 0; y < 4; y++) memcpy((uint8_t *)IOSurfaceGetBaseAddress(io) + y * IOSurfaceGetBytesPerRow(io), px, 16);
                IOSurfaceUnlock(io, 0, NULL);
                td.storageMode = MTLStorageModeManaged;
                t = [dev newTextureWithDescriptor:td iosurface:io plane:0];
            } else {
                t = [dev newTextureWithDescriptor:td];
                [t replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 withBytes:px bytesPerRow:16];
            }
            id<MTLBuffer> out = [dev newBufferWithLength:32 options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            [ce setComputePipelineState:ps];
            [ce setTexture:t atIndex:0]; [ce setTexture:t atIndex:1];
            [ce setSamplerState:sm atIndex:0];
            [ce setBuffer:out offset:0 atIndex:0];
            [ce dispatchThreads:MTLSizeMake(2, 1, 1) threadsPerThreadgroup:MTLSizeMake(2, 1, 1)];
            [ce endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            const float *f = out.contents;
            printf("  %-9s read %.4f %.4f %.4f %.4f  sample %.4f %.4f %.4f %.4f\n", kind ? "iosurface" : "texture", f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
            for (int c = 0; c < 3; c++) { bad += fabsf(f[c] - 0.2159f) > 0.01f; bad += fabsf(f[4 + c] - 0.2159f) > 0.01f; }
            if (io) CFRelease(io);
        }
        printf("metal_srgb_test: %s\n", bad ? "FAIL" : "PASS");
        return bad != 0;
    }
}
