// metal_ctxsw_test <tag> [seconds]: run in two processes at once with different
// tags. Each dispatches a kernel that writes (tag << 16 | gid) and checks every
// value. Both processes place their kernels at the same GPU VAs, so a result
// with the other tag (or a GR exception) means one context ran the other's code
// or data after a GR context switch.
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    @autoreleasepool {
        const unsigned tag = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) & 0xffff : 1;
        const double secs = argc > 2 ? atof(argv[2]) : 10;
        id<MTLDevice> dev = nil;
        for (id<MTLDevice> d in MTLCopyAllDevices()) if ([d.name containsString:@"RTX"]) dev = d;
        if (!dev) dev = MTLCreateSystemDefaultDevice();   // M1 reference run
        // tag in the code itself: each process compiles different instructions
        NSString *src = [NSString stringWithFormat:
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "kernel void k(device uint *o [[buffer(0)]], uint g [[thread_position_in_grid]]) {"
             " uint v = g; for (int i = 0; i < 64; i++) v = v * 1664525u + 1013904223u;"
             " o[g] = (%uu << 16) | (g & 0xffffu); o[g + 65536] = v; }\n", tag];
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&err];
        id<MTLComputePipelineState> p = lib ? [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"k"] error:&err] : nil;
        if (!p) { printf("compile: %s\n", err.description.UTF8String); return 1; }
        id<MTLBuffer> b = [dev newBufferWithLength:65536 * 8 options:MTLResourceStorageModeShared];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        unsigned rounds = 0, bad = 0, other = 0, zero = 0, badRounds = 0, hiBad = 0;
        NSDate *end = [NSDate dateWithTimeIntervalSinceNow:secs];
        while ([end timeIntervalSinceNow] > 0) {
            memset(b.contents, 0, 65536 * 8);
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
            [e setComputePipelineState:p]; [e setBuffer:b offset:0 atIndex:0];
            for (int k = 0; k < 4; k++)
                [e dispatchThreads:MTLSizeMake(65536, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
            [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
            if (cb.status != MTLCommandBufferStatusCompleted) { printf("cb status %ld round %u\n", (long)cb.status, rounds); bad++; break; }
            const uint32_t *o = b.contents;
            const unsigned before = bad;
            uint32_t firstBad = ~0u;
            for (uint32_t g = 0; g < 65536; g++) {
                uint32_t v = g; for (int i = 0; i < 64; i++) v = v * 1664525u + 1013904223u;
                if (o[g] != ((tag << 16) | g)) {
                    bad++; if (firstBad == ~0u) firstBad = g;
                    if (!o[g]) zero++; else if ((o[g] >> 16) != tag) other++;
                }
                if (o[g + 65536] != v) { bad++; hiBad++; }
            }
            if (bad != before && badRounds++ < 3)
                printf("  round %u: %u wrong, first at %u (got 0x%08x)\n", rounds, bad - before, firstBad,
                       firstBad != ~0u ? o[firstBad] : 0);
            rounds++;
        }
        printf("metal_ctxsw_test tag %u: %u rounds, %u wrong in %u rounds (%u zero, %u another tag, %u hash): %s\n",
               tag, rounds, bad, badRounds, zero, other, hiBad, bad ? "FAIL" : "PASS");
        return bad ? 1 : 0;
    }
}
