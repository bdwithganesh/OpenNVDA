// Metal command-buffer ordering and synchronization on NVMTLDriver (M18).
//   sudo metal_sync_test
// 1 addCompletedHandler sees the kernel's results (20 rounds)
// 2 addScheduledHandler runs before the completed one
// 3 MTLSharedEvent: CB2 waits for a value CB1 signals (two queues)
// 4 MTLSharedEvent listener fires with the signalled value
// 5 status/error after waitUntilCompleted
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdatomic.h>

static id<MTLDevice> findNV(void) {
    for (id<MTLDevice> d in MTLCopyAllDevices())
        if ([d.name containsString:@"NVIDIA"]) return d;
    return nil;
}

static int fails;
static void check(const char *what, int ok) {
    printf("  %-60s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) fails++;
}

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = findNV();
        if (!dev) { printf("no NVIDIA MTLDevice\n"); return 1; }
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:
            @"#include <metal_stdlib>\nusing namespace metal;\n"
            @"kernel void inc(device uint *b [[buffer(0)]], uint i [[thread_position_in_grid]]) { b[i] = b[i] + 1u; }\n"
            options:nil error:&e];
        id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"inc"] error:&e];
        if (!ps) { printf("pipeline: %s\n", e.localizedDescription.UTF8String); return 1; }
        id<MTLCommandQueue> q = [dev newCommandQueue], q2 = [dev newCommandQueue];
        const NSUInteger N = 4096;
        id<MTLBuffer> buf = [dev newBufferWithLength:N * 4 options:MTLResourceStorageModeShared];
        memset(buf.contents, 0, N * 4);
        void (^enc)(id<MTLCommandBuffer>) = ^(id<MTLCommandBuffer> cb) {
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            [ce setComputePipelineState:ps];
            [ce setBuffer:buf offset:0 atIndex:0];
            [ce dispatchThreads:MTLSizeMake(N, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
            [ce endEncoding];
        };

        // 1 + 2
        {
            __block _Atomic int stale = 0, order = 0, done = 0;
            for (int r = 1; r <= 20; r++) {
                id<MTLCommandBuffer> cb = [q commandBuffer];
                enc(cb);
                __block _Atomic int scheduled = 0;
                const uint32_t want = (uint32_t)r;
                [cb addScheduledHandler:^(id<MTLCommandBuffer> c) { (void)c; atomic_store(&scheduled, 1); }];
                [cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
                    (void)c;
                    const uint32_t *v = buf.contents;
                    if (v[0] != want || v[N - 1] != want) atomic_fetch_add(&stale, 1);
                    if (!atomic_load(&scheduled)) atomic_fetch_add(&order, 1);
                    atomic_fetch_add(&done, 1);
                }];
                [cb commit];
                [cb waitUntilCompleted];
            }
            for (int i = 0; i < 1000 && atomic_load(&done) < 20; i++) usleep(1000);
            char msg[96];
            snprintf(msg, sizeof msg, "completed handler sees results (%d/20 stale, %d handlers)",
                     atomic_load(&stale), atomic_load(&done));
            check(msg, atomic_load(&stale) == 0 && atomic_load(&done) == 20);
            check("scheduled handler before completed", atomic_load(&order) == 0);
        }
        // 3: cross-queue shared event
        {
            id<MTLSharedEvent> ev = [dev newSharedEvent];
            memset(buf.contents, 0, N * 4);
            id<MTLCommandBuffer> c2 = [q2 commandBuffer];
            [c2 encodeWaitForEvent:ev value:1];
            enc(c2);                               // runs second: 1 -> 2
            id<MTLCommandBuffer> c1 = [q commandBuffer];
            enc(c1);                               // runs first: 0 -> 1
            [c1 encodeSignalEvent:ev value:1];
            dispatch_async(dispatch_get_global_queue(0, 0), ^{ [c2 commit]; });
            usleep(20000);
            [c1 commit];
            [c1 waitUntilCompleted];
            for (int i = 0; i < 2000 && c2.status < MTLCommandBufferStatusCompleted; i++) usleep(1000);
            const uint32_t *v = buf.contents;
            char msg[96];
            snprintf(msg, sizeof msg, "queue 2 waits for queue 1's event (value %u, want 2)", v[0]);
            check(msg, v[0] == 2 && ev.signaledValue == 1);
        }
        // 4: listener
        {
            id<MTLSharedEvent> ev = [dev newSharedEvent];
            MTLSharedEventListener *li = [[MTLSharedEventListener alloc] initWithDispatchQueue:
                                          dispatch_queue_create("nvmtl.sync.test", NULL)];
            __block _Atomic uint64_t got = 0;
            [ev notifyListener:li atValue:7 block:^(id<MTLSharedEvent> x, uint64_t val) { (void)x; atomic_store(&got, val); }];
            id<MTLCommandBuffer> cb = [q commandBuffer];
            enc(cb);
            [cb encodeSignalEvent:ev value:7];
            [cb commit];
            [cb waitUntilCompleted];
            for (int i = 0; i < 1000 && !atomic_load(&got); i++) usleep(1000);
            check("shared event listener fires at 7", atomic_load(&got) == 7);
        }
        // 5
        {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            enc(cb);
            [cb commit];
            [cb waitUntilCompleted];
            check("status Completed, no error", cb.status == MTLCommandBufferStatusCompleted && !cb.error);
        }
    }
    printf("metal_sync_test: %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
