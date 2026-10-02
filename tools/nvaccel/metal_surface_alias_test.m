// Regression: cached texture headers must use the mapping owned by that
// wrapper, even when another wrapper of the same IOSurface is destroyed.
// clang -fobjc-arc -framework Foundation -framework Metal -framework IOSurface \
//   metal_surface_alias_test.m -o metal_surface_alias_test
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#include <stdio.h>

enum { W = 33, H = 17, PITCH = 256 };
static BOOL readSurface(id<MTLCommandQueue> q, id<MTLComputePipelineState> ps,
                        id<MTLTexture> texture, id<MTLBuffer> output) {
    memset(output.contents, 0, output.length);
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:ps];
    [enc setTexture:texture atIndex:0];
    [enc setBuffer:output offset:0 atIndex:0];
    [enc dispatchThreads:MTLSizeMake(W, H, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
    [enc endEncoding];
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [cb addCompletedHandler:^(id<MTLCommandBuffer> completed) { (void)completed; dispatch_semaphore_signal(done); }];
    // Commit itself can block in the synchronous driver path.
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ [cb commit]; });
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC))) {
        fprintf(stderr, "FAIL: GPU read timed out\n"); return NO;
    }
    if (cb.status != MTLCommandBufferStatusCompleted) {
        fprintf(stderr, "FAIL: %s\n", cb.error.description.UTF8String); return NO;
    }
    const uint32_t *pixels = output.contents;
    for (unsigned i = 0; i < W * H; ++i)
        if (pixels[i] != 0xff563412u) {
            fprintf(stderr, "FAIL: pixel %u = %08x\n", i, pixels[i]); return NO;
        }
    return YES;
}
int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = nil;
        for (id<MTLDevice> d in MTLCopyAllDevices())
            if ([d.name containsString:@"NVIDIA"]) dev = d;
        if (!dev) { fprintf(stderr, "SKIP: NVIDIA device unavailable\n"); return 77; }
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "kernel void readback(texture2d<float, access::read> src [[texture(0)]],"
             " device uint *dst [[buffer(0)]], uint2 p [[thread_position_in_grid]]) {"
             " uint4 c = uint4(src.read(p) * 255.0f + 0.5f);"
             " dst[p.y * 33 + p.x] = c.x | (c.y << 8) | (c.z << 16) | (c.w << 24); }"
            options:nil error:&err];
        id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"readback"] error:&err];
        if (!ps) { fprintf(stderr, "FAIL: pipeline %s\n", err.description.UTF8String); return 1; }
        IOSurfaceRef io = IOSurfaceCreate((__bridge CFDictionaryRef)@{
            (id)kIOSurfaceWidth:@(W), (id)kIOSurfaceHeight:@(H),
            (id)kIOSurfaceBytesPerElement:@4, (id)kIOSurfaceBytesPerRow:@(PITCH),
            (id)kIOSurfaceAllocSize:@(PITCH * H)});
        if (!io) return 1;
        IOSurfaceLock(io, 0, NULL);
        for (unsigned y = 0; y < H; ++y)
            for (unsigned x = 0; x < W; ++x)
                ((uint32_t *)((uint8_t *)IOSurfaceGetBaseAddress(io) + PITCH * y))[x] = 0xff563412u;
        IOSurfaceUnlock(io, 0, NULL);
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:W height:H mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> survivor = nil;
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLBuffer> output = [dev newBufferWithLength:W * H * 4 options:MTLResourceStorageModeShared];
        @autoreleasepool {
            __attribute__((objc_precise_lifetime)) id<MTLTexture> first = [dev newTextureWithDescriptor:td iosurface:io plane:0];
            survivor = [dev newTextureWithDescriptor:td iosurface:io plane:0];
            if (!first || !survivor || !output || !q || !readSurface(q, ps, survivor, output)) return 1;
        } // The first mapping is unbound; survivor's TIC has already been cached.
        BOOL ok = readSurface(q, ps, survivor, output);
        CFRelease(io);
        if (!ok) return 1;
        puts("PASS: all pixels before and after releasing the duplicate surface wrapper");
        return 0;
    }
}
