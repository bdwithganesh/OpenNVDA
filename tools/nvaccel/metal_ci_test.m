// metal_ci_test: Apple's kernels on our device, the way WindowServer and
// mediaanalysisd use them: CoreImage gaussian blur and MPS gaussian blur +
// Sobel, many rounds, no waits in between.
#import <Metal/Metal.h>
#import <MetalPerformanceShaders/MetalPerformanceShaders.h>
#import <CoreImage/CoreImage.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    @autoreleasepool {
        id<MTLDevice> dev = nil;
        for (id<MTLDevice> d in MTLCopyAllDevices()) if ([d.name containsString:@"RTX"]) dev = d;
        if (!dev) { printf("no RTX Metal device\n"); return 1; }
        const int rounds = argc > 1 ? atoi(argv[1]) : 60;
        id<MTLCommandQueue> q = [dev newCommandQueue];
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:512 height:512 mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
        id<MTLTexture> src = [dev newTextureWithDescriptor:td], dst = [dev newTextureWithDescriptor:td];
        uint8_t *px = malloc(512 * 512 * 4);
        for (int i = 0; i < 512 * 512 * 4; i++) px[i] = (uint8_t)(i * 13);
        [src replaceRegion:MTLRegionMake2D(0, 0, 512, 512) mipmapLevel:0 withBytes:px bytesPerRow:2048];
        CIContext *ci = [CIContext contextWithMTLDevice:dev];
        MPSImageGaussianBlur *gb = [[MPSImageGaussianBlur alloc] initWithDevice:dev sigma:4.0f];
        MPSImageSobel *sob = [[MPSImageSobel alloc] initWithDevice:dev];
        for (int r = 0; r < rounds; r++) @autoreleasepool {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            [gb encodeToCommandBuffer:cb sourceTexture:src destinationTexture:dst];
            [sob encodeToCommandBuffer:cb sourceTexture:dst destinationTexture:src];
            [cb commit];
            CIImage *im = [[CIImage imageWithMTLTexture:dst options:nil] imageByApplyingGaussianBlurWithSigma:6];
            id<MTLCommandBuffer> cb2 = [q commandBuffer];
            [ci render:im toMTLTexture:dst commandBuffer:cb2 bounds:CGRectMake(0, 0, 512, 512) colorSpace:CGColorSpaceCreateDeviceRGB()];
            [cb2 commit];
            if ((r & 15) == 15) [cb2 waitUntilCompleted];
        }
        id<MTLCommandBuffer> last = [q commandBuffer]; [last commit]; [last waitUntilCompleted];
        printf("metal_ci_test: PASS (status %ld)\n", (long)last.status);
        return 0;
    }
}
