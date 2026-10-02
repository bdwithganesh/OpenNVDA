// GPU fault recovery. A kernel that faults (mode "mmu": store 1 GiB past its
// buffer, "oor": threadgroup index far out of range, "loop": never ends) must
// finish in bounded time (an error status is fine), not hang the process or
// WindowServer. "rawmmu" (RTX only) reads an intentionally unmapped GPU VA
// through a tier-2 argument buffer. Unlike mmu, this bypasses buffer bounds
// handling. It is an injection, not a passing test: verify RC/reset events,
// a fresh good kernel and the real scan-out separately afterwards.
// Mode "good": a plain kernel, run it afterwards from a new
// process to see the GPU came back (automatic reset).
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <string.h>

static NSString *const kSrc =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "kernel void good(device uint *a [[buffer(0)]], uint i [[thread_position_in_grid]]) { a[i] = i * 3u + 7u; }\n"
     "kernel void mmu(device uint *a [[buffer(0)]], uint i [[thread_position_in_grid]]) { a[i + 0x10000000u] = i; }\n"
     "struct BadAddress { device volatile uint *p; };\n"
     "kernel void rawmmu(constant BadAddress &bad [[buffer(0)]], device uint *out [[buffer(1)]]) { out[0] = bad.p[0]; }\n"
     "kernel void oor(device uint *a [[buffer(0)]], uint i [[thread_position_in_grid]], uint l [[thread_index_in_threadgroup]]) {\n"
     "  threadgroup uint t[16]; t[(l + 1u) * 0x100000u] = i; threadgroup_barrier(mem_flags::mem_threadgroup); a[i] = t[l & 15u]; }\n"
     "kernel void loop(device uint *a [[buffer(0)]], uint i [[thread_position_in_grid]]) { uint x = a[i]; while (x != 0xffffffffu) { x = x * 1664525u + 1013904223u; if (x == a[1]) break; } a[i] = x; }\n";

int main(int argc, char **argv) {
    @autoreleasepool {
        const char *mode = argc > 1 ? argv[1] : "good";
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { printf("metal_fault_test %s: no device: FAIL\n", mode); return 1; }
        const bool raw = !strcmp(mode, "rawmmu");
        if (raw && ![dev.name containsString:@"NVIDIA"]) {
            printf("rawmmu injection requires the RTX driver\n"); return 2;
        }
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:nil error:&e];
        id<MTLFunction> fn = [lib newFunctionWithName:@(mode)];
        id<MTLComputePipelineState> ps = fn ? [dev newComputePipelineStateWithFunction:fn error:&e] : nil;
        if (!ps) { printf("metal_fault_test %s: no pipeline (%s): FAIL\n", mode, e.localizedDescription.UTF8String); return 1; }
        const NSUInteger n = 4096;
        id<MTLBuffer> b = [dev newBufferWithLength:n * 4 options:MTLResourceStorageModeShared];
        memset(b.contents, 0, n * 4);
        ((uint32_t *)b.contents)[1] = 0x12345678;   // loop: a value the sequence never hits soon
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:ps];
        if (raw) {
            // NVMTL's tier-2 buffer-pointer ABI stores a 64-bit GPU VA.
            // Our arena has no resource at this address. The volatile read
            // must actually reach the GPU, even if its result is undefined.
            const uint64_t badVA = 0x7f00000000ULL;
            id<MTLBuffer> arg = [dev newBufferWithBytes:&badVA length:sizeof badVA options:MTLResourceStorageModeShared];
            [ce setBuffer:arg offset:0 atIndex:0]; [ce setBuffer:b offset:0 atIndex:1];
            [ce dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        } else {
            [ce setBuffer:b offset:0 atIndex:0];
            [ce dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        }
        [ce endEncoding];
        NSDate *t0 = [NSDate date];
        [cb commit];
        [cb waitUntilCompleted];
        const double secs = -[t0 timeIntervalSinceNow];
        if (raw) {
            printf("rawmmu INJECTION: status %ld error %ld, %.2f s; verify RC/reset and fresh-process recovery separately\n",
                   (long)cb.status, (long)cb.error.code, secs);
            return 0;
        }
        if (!strcmp(mode, "good")) {
            int bad = 0;
            for (NSUInteger i = 0; i < n; i++) bad += ((uint32_t *)b.contents)[i] != i * 3u + 7u;
            printf("metal_fault_test good: status %ld, %d wrong, %.2f s: %s\n", (long)cb.status, bad, secs,
                   cb.status == MTLCommandBufferStatusCompleted && !bad ? "PASS" : "FAIL");
            return cb.status != MTLCommandBufferStatusCompleted || bad;
        }
        // a faulting kernel: done in bounded time is the pass (the M1 runs
        // both "mmu" and "oor" to completion without an error; an error
        // status is fine too, a hang is not)
        const bool done = cb.status == MTLCommandBufferStatusError || cb.status == MTLCommandBufferStatusCompleted;
        printf("metal_fault_test %s: status %ld error %ld (%s), %.2f s: %s\n", mode, (long)cb.status, (long)cb.error.code,
               cb.error.localizedDescription.UTF8String ?: "-", secs, done && secs < 15 ? "PASS" : "FAIL");
        return !done || secs >= 15;
    }
}
