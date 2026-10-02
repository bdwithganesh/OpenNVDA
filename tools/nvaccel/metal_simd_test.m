// metal_simd_test: simdgroup functions and simdgroup_matrix (8x8 float/half
// multiply-accumulate), checked against the CPU.
//   sudo metal_simd_test
#import <Metal/Metal.h>
#include <stdio.h>
#include <math.h>

static int fails;
static void check(const char *what, int ok) { printf("  %-60s %s\n", what, ok ? "PASS" : "FAIL"); if (!ok) fails++; }

static NSString *const kSrc =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     // one simdgroup: C = A * B + C, 8x8, row major
     "kernel void mma8(device const float *A [[buffer(0)]], device const float *B [[buffer(1)]], device float *C [[buffer(2)]],\n"
     "  uint lane [[thread_index_in_simdgroup]]) {\n"
     "  simdgroup_float8x8 a, b, c;\n"
     "  simdgroup_load(a, A, 8); simdgroup_load(b, B, 8); simdgroup_load(c, C, 8);\n"
     "  simdgroup_multiply_accumulate(c, a, b, c);\n"
     "  simdgroup_store(c, C, 8); }\n"
     // tiled SGEMM with simdgroup matrices: each simdgroup does an 8x8 tile of C (n multiple of 8)
     "kernel void sgemm_simd(device const float *A [[buffer(0)]], device const float *B [[buffer(1)]], device float *C [[buffer(2)]],\n"
     "  constant uint &n [[buffer(3)]], uint2 tg [[threadgroup_position_in_grid]], uint sg [[simdgroup_index_in_threadgroup]]) {\n"
     "  uint row = tg.y * 32 + (sg / 4) * 8, col = tg.x * 32 + (sg % 4) * 8;\n"
     "  simdgroup_float8x8 acc = make_filled_simdgroup_matrix<float, 8, 8>(0.0f);\n"
     "  for (uint k = 0; k < n; k += 8) {\n"
     "    simdgroup_float8x8 a, b;\n"
     "    simdgroup_load(a, A + row * n + k, n); simdgroup_load(b, B + k * n + col, n);\n"
     "    simdgroup_multiply_accumulate(acc, a, b, acc); }\n"
     "  simdgroup_store(acc, C + row * n + col, n); }\n"
     "kernel void mma8h(device const half *A [[buffer(0)]], device const half *B [[buffer(1)]], device float *C [[buffer(2)]],\n"
     "  uint lane [[thread_index_in_simdgroup]]) {\n"
     "  simdgroup_half8x8 a, b; simdgroup_float8x8 c = make_filled_simdgroup_matrix<float, 8, 8>(1.0f);\n"
     "  simdgroup_load(a, A, 8); simdgroup_load(b, B, 8, 0, true);\n"
     "  simdgroup_multiply_accumulate(c, a, b, c);\n"
     "  simdgroup_store(c, C, 8); }\n";

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = nil;
        for (id<MTLDevice> d in MTLCopyAllDevices()) if ([d.name containsString:@"NVIDIA"]) dev = d;
        if (!dev) { printf("no NVIDIA MTLDevice\n"); return 1; }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:nil error:&e];
        if (!lib) { printf("compile: %s\n", e.localizedDescription.UTF8String); return 1; }
        {   // mma8
            id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"mma8"] error:&e];
            id<MTLBuffer> A = [dev newBufferWithLength:256 options:0], B = [dev newBufferWithLength:256 options:0],
                          C = [dev newBufferWithLength:256 options:0];
            float *a = A.contents, *b = B.contents, *c = C.contents, want[64];
            for (int i = 0; i < 64; i++) { a[i] = (float)(i % 9) - 4; b[i] = (float)((i * 5) % 7) - 3; c[i] = (float)i; }
            for (int r = 0; r < 8; r++) for (int col = 0; col < 8; col++) {
                float s = c[r * 8 + col];
                for (int k = 0; k < 8; k++) s += a[r * 8 + k] * b[k * 8 + col];
                want[r * 8 + col] = s;
            }
            int bad = !ps;
            if (ps) {
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
                [ce setComputePipelineState:ps];
                [ce setBuffer:A offset:0 atIndex:0]; [ce setBuffer:B offset:0 atIndex:1]; [ce setBuffer:C offset:0 atIndex:2];
                [ce dispatchThreads:MTLSizeMake(32, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
                [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
                for (int i = 0; i < 64; i++) if (c[i] != want[i] && bad++ < 3) printf("    C[%d] %f want %f\n", i, c[i], want[i]);
            }
            check("simdgroup_float8x8 load / multiply_accumulate / store", !bad);
        }
        {   // mma8h: half inputs, B transposed on load, float accumulator filled with 1
            id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"mma8h"] error:&e];
            id<MTLBuffer> A = [dev newBufferWithLength:128 options:0], B = [dev newBufferWithLength:128 options:0],
                          C = [dev newBufferWithLength:256 options:0];
            __fp16 *a = A.contents, *b = B.contents;
            float *c = C.contents, want[64];
            for (int i = 0; i < 64; i++) { a[i] = (__fp16)((i % 5) - 2); b[i] = (__fp16)((i % 3) - 1); }
            for (int r = 0; r < 8; r++) for (int col = 0; col < 8; col++) {
                float s = 1;
                for (int k = 0; k < 8; k++) s += (float)a[r * 8 + k] * (float)b[col * 8 + k];   // B^T
                want[r * 8 + col] = s;
            }
            int bad = !ps;
            if (ps) {
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
                [ce setComputePipelineState:ps];
                [ce setBuffer:A offset:0 atIndex:0]; [ce setBuffer:B offset:0 atIndex:1]; [ce setBuffer:C offset:0 atIndex:2];
                [ce dispatchThreads:MTLSizeMake(32, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
                [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
                for (int i = 0; i < 64; i++) if (c[i] != want[i] && bad++ < 3) printf("    C[%d] %f want %f\n", i, c[i], want[i]);
            }
            check("simdgroup_half8x8 (transposed load) into float accumulator", !bad);
        }
        {   // SGEMM 1024 with simdgroup matrices
            const uint32_t n = 1024;
            id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"sgemm_simd"] error:&e];
            id<MTLBuffer> A = [dev newBufferWithLength:n * n * 4 options:MTLResourceStorageModeManaged],
                          B = [dev newBufferWithLength:n * n * 4 options:MTLResourceStorageModeManaged],
                          C = [dev newBufferWithLength:n * n * 4 options:MTLResourceStorageModeManaged];
            float *a = A.contents, *b = B.contents, *c = C.contents;
            for (uint32_t i = 0; i < n * n; i++) { a[i] = (float)(i % 7) - 3; b[i] = (float)(i % 5) - 2; }
            [A didModifyRange:NSMakeRange(0, n * n * 4)]; [B didModifyRange:NSMakeRange(0, n * n * 4)];
            int bad = !ps;
            double best = 1e9;
            for (int r = 0; ps && r < 3; r++) {
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
                [ce setComputePipelineState:ps];
                [ce setBuffer:A offset:0 atIndex:0]; [ce setBuffer:B offset:0 atIndex:1]; [ce setBuffer:C offset:0 atIndex:2];
                [ce setBytes:&n length:4 atIndex:3];
                [ce dispatchThreadgroups:MTLSizeMake(n / 32, n / 32, 1) threadsPerThreadgroup:MTLSizeMake(512, 1, 1)];
                [ce endEncoding];
                id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder]; [be synchronizeResource:C]; [be endEncoding];
                NSDate *t0 = [NSDate date];
                [cb commit]; [cb waitUntilCompleted];
                const double d = -[t0 timeIntervalSinceNow] * 1e3;
                if (d < best) best = d;
            }
            for (int s = 0; ps && s < 32; s++) {
                const uint32_t r = (s * 97) % n, col = (s * 31) % n;
                double w = 0;
                for (uint32_t k = 0; k < n; k++) w += (double)a[r * n + k] * b[k * n + col];
                if (c[r * n + col] != (float)w && bad++ < 3) printf("    C[%u][%u] %f want %f\n", r, col, c[r * n + col], w);
            }
            printf("    sgemm_simd 1024: %.2f ms = %.0f GFLOPS\n", best, 2.0 * n * n * n / best / 1e6);
            check("SGEMM 1024 with simdgroup_float8x8", !bad);
        }
    }
    printf("metal_simd_test: %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
