// IOSurface-backed textures whose rows are not width * 4 (IOSurface pads
// bytesPerRow; Core Image's CIRandomGenerator uses a 258x258 surface), and a
// CPU rewrite of the surface after the texture exists (the seed changes).
// A compute kernel reads every texel into a buffer; every value must match
// what the CPU wrote. Run on Apple's driver for the reference.
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <Foundation/Foundation.h>

static NSString *const kSrc =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "kernel void rd(texture2d<float, access::read> t [[texture(0)]], device uchar4 *o [[buffer(0)]],\n"
     "               uint2 g [[thread_position_in_grid]]) {\n"
     "  if (g.x >= t.get_width() || g.y >= t.get_height()) return;\n"
     "  o[g.y * t.get_width() + g.x] = uchar4(round(t.read(g) * 255.0));\n"
     "}\n";

static IOSurfaceRef makeSurface(int w, int h, uint32_t fourcc) {
    NSDictionary *p = @{(id)kIOSurfaceWidth: @(w), (id)kIOSurfaceHeight: @(h), (id)kIOSurfaceBytesPerElement: @4,
                        (id)kIOSurfacePixelFormat: @(fourcc)};
    return IOSurfaceCreate((__bridge CFDictionaryRef)p);
}

static void fill(IOSurfaceRef s, int w, int h, int salt) {
    IOSurfaceLock(s, 0, NULL);
    uint8_t *b = IOSurfaceGetBaseAddress(s);
    const size_t rb = IOSurfaceGetBytesPerRow(s);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            for (int c = 0; c < 4; c++) b[y * rb + x * 4 + c] = (uint8_t)((x * 7 + y * 13 + c * 61 + salt * 29) & 0xff);
    IOSurfaceUnlock(s, 0, NULL);
}

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:nil error:&e];
        id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"rd"] error:&e];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        const int widths[] = {64, 258, 23, 100};
        int bad = 0;
        for (int wi = 0; wi < 4; wi++) {
            const int w = widths[wi], h = widths[wi];
            IOSurfaceRef s = makeSurface(w, h, 'BGRA');
            fill(s, w, h, 0);
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:w height:h
                                                                                      mipmapped:NO];
            td.usage = MTLTextureUsageShaderRead;
            td.storageMode = MTLStorageModeManaged;
            id<MTLTexture> t = [dev newTextureWithDescriptor:td iosurface:s plane:0];
            for (int pass = 0; pass < 2; pass++) {
                if (pass) fill(s, w, h, 1);   // CPU rewrite after the texture exists
                id<MTLBuffer> out = [dev newBufferWithLength:(NSUInteger)(w * h * 4) options:MTLResourceStorageModeShared];
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
                [ce setComputePipelineState:ps];
                [ce setTexture:t atIndex:0];
                [ce setBuffer:out offset:0 atIndex:0];
                [ce dispatchThreads:MTLSizeMake((NSUInteger)w, (NSUInteger)h, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
                [ce endEncoding];
                [cb commit];
                [cb waitUntilCompleted];
                const uint8_t *o = out.contents;
                int wrong = 0, first = -1;
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++)
                        for (int c = 0; c < 4; c++) {
                            const uint8_t want = (uint8_t)((x * 7 + y * 13 + c * 61 + pass * 29) & 0xff);
                            if (o[(y * w + x) * 4 + c] != want) { if (first < 0) first = (y * w + x) * 4 + c; wrong++; }
                        }
                printf("  %3dx%-3d rowBytes %4zu %s: %6d wrong%s\n", w, h, IOSurfaceGetBytesPerRow(s), pass ? "after CPU rewrite" : "first read       ",
                       wrong, wrong ? [NSString stringWithFormat:@" (first at byte %d)", first].UTF8String : "");
                bad += wrong != 0;
            }
            CFRelease(s);
        }
        printf("metal_iosurface_stride_test: %s\n", bad ? "FAIL" : "PASS");
        return bad != 0;
    }
}
