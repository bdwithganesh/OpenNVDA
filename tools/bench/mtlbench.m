// mtlbench: memory fill / copy, FP32 FMA throughput and host<->GPU blits through plain Metal.
// Each dispatch stays well under 100 ms so the GPU hang detector never trips.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <mach/mach_time.h>

static NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\n"
"kernel void fill(device float4 *d [[buffer(0)]], uint i [[thread_position_in_grid]]) { d[i] = float4(i); }\n"
"kernel void copyk(device const float4 *s [[buffer(0)]], device float4 *d [[buffer(1)]], uint i [[thread_position_in_grid]]) { d[i] = s[i]; }\n"
"kernel void fma(device float4 *d [[buffer(0)]], constant uint &n [[buffer(1)]], uint i [[thread_position_in_grid]]) {\n"
"  float4 a = float4(i) * 1e-7, b = a + 0.5, c = a + 0.25, e = a + 0.125, x = float4(0.999), y = float4(1e-4);\n"
"  for (uint k = 0; k < n; ++k) { a = fma(a, x, y); b = fma(b, x, y); c = fma(c, x, y); e = fma(e, x, y);\n"
"    a = fma(a, x, y); b = fma(b, x, y); c = fma(c, x, y); e = fma(e, x, y); }\n"
"  d[i] = a + b + c + e; }\n";

static double best(id<MTLCommandQueue> q, int reps, void (^enc)(id<MTLCommandBuffer>)) {
    double b = 1e30;
    for (int r = 0; r < reps; ++r) {
        id<MTLCommandBuffer> cb = [q commandBuffer]; enc(cb); [cb commit]; [cb waitUntilCompleted];
        if (cb.status != MTLCommandBufferStatusCompleted) { fprintf(stderr, "cb failed %s\n", cb.error.description.UTF8String); exit(1); }
        double t = cb.GPUEndTime - cb.GPUStartTime;
        if (t <= 0) { fprintf(stderr, "no GPU timestamps\n"); exit(1); }
        if (r && t < b) b = t;   // first rep is warm-up
    }
    return b;
}

int main(void) { @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    printf("device %s\n", dev.name.UTF8String);
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&err];
    if (!lib) { printf("compile failed %s\n", err.description.UTF8String); return 1; }
    id<MTLComputePipelineState> pf = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"fill"] error:&err];
    id<MTLComputePipelineState> pc = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"copyk"] error:&err];
    id<MTLComputePipelineState> pm = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"fma"] error:&err];
    if (!pf || !pc || !pm) { printf("pipeline failed %s\n", err.description.UTF8String); return 1; }
    id<MTLCommandQueue> q = [dev newCommandQueue];
    const NSUInteger N = 256u << 20;   // 256 MiB per buffer
    id<MTLBuffer> a = [dev newBufferWithLength:N options:MTLResourceStorageModePrivate];
    id<MTLBuffer> b = [dev newBufferWithLength:N options:MTLResourceStorageModePrivate];
    id<MTLBuffer> h = [dev newBufferWithLength:N options:MTLResourceStorageModeShared];
    memset(h.contents, 1, N);
    const NSUInteger n4 = N / 16, tg = 256;
    void (^grid)(id<MTLComputeCommandEncoder>, NSUInteger) = ^(id<MTLComputeCommandEncoder> e, NSUInteger n) {
        [e dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(tg, 1, 1)]; };

    double t = best(q, 6, ^(id<MTLCommandBuffer> cb) { id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:pf]; [e setBuffer:a offset:0 atIndex:0]; grid(e, n4); [e endEncoding]; });
    printf("fill kernel      %7.1f GB/s (bytes written)\n", N / t / 1e9);
    t = best(q, 6, ^(id<MTLCommandBuffer> cb) { id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:pc]; [e setBuffer:a offset:0 atIndex:0]; [e setBuffer:b offset:0 atIndex:1]; grid(e, n4); [e endEncoding]; });
    printf("copy kernel      %7.1f GB/s (bytes copied), %.1f GB/s read+write\n", N / t / 1e9, 2.0 * N / t / 1e9);
    t = best(q, 6, ^(id<MTLCommandBuffer> cb) { id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
        [e copyFromBuffer:a sourceOffset:0 toBuffer:b destinationOffset:0 size:N]; [e endEncoding]; });
    printf("blit VRAM->VRAM  %7.1f GB/s (bytes copied)\n", N / t / 1e9);
    t = best(q, 6, ^(id<MTLCommandBuffer> cb) { id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
        [e copyFromBuffer:h sourceOffset:0 toBuffer:a destinationOffset:0 size:N]; [e endEncoding]; });
    printf("blit host->GPU   %7.1f GB/s\n", N / t / 1e9);
    t = best(q, 6, ^(id<MTLCommandBuffer> cb) { id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
        [e copyFromBuffer:a sourceOffset:0 toBuffer:h destinationOffset:0 size:N]; [e endEncoding]; });
    printf("blit GPU->host   %7.1f GB/s\n", N / t / 1e9);

    const uint32_t iters = getenv("ITERS") ? atoi(getenv("ITERS")) : 512; const NSUInteger thr = 76 * 1536 * 8;   // 76 SMs, 8 waves of full occupancy
    t = best(q, 6, ^(id<MTLCommandBuffer> cb) { id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:pm]; [e setBuffer:a offset:0 atIndex:0]; [e setBytes:&iters length:4 atIndex:1]; grid(e, thr); [e endEncoding]; });
    // 8 float4 FMAs per iteration = 8 * 4 lanes * 2 flops
    printf("FP32 FMA         %7.2f TFLOPS (%.2f ms)\n", (double)thr * iters * 8 * 4 * 2 / t / 1e12, t * 1e3);
    float *v = h.contents; (void)v;
    return 0;
}}
