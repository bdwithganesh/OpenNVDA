// Shared events through command buffers: signal after GPU work, the value
// seen through a handle-opened event, a wait released by another thread, and
// a listener fired by a command buffer signal.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

static NSString *const kSrc =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "kernel void k(device uint *o [[buffer(0)]], uint i [[thread_position_in_grid]]) { o[i] = i * 3 + 1; }\n";

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *e = nil;
        id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:
            [[dev newLibraryWithSource:kSrc options:nil error:&e] newFunctionWithName:@"k"] error:&e];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLSharedEvent> ev = [dev newSharedEvent];
        int bad = 0;
        printf("  event class %s\n", object_getClassName(ev));
        // 1. signal after a dispatch; the CPU waits on the event, not the command buffer
        id<MTLBuffer> b = [dev newBufferWithLength:256 * 4 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:ps];
        [ce setBuffer:b offset:0 atIndex:0];
        [ce dispatchThreads:MTLSizeMake(256, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [ce endEncoding];
        [cb encodeSignalEvent:ev value:3];
        [cb commit];
        const BOOL got = [ev waitUntilSignaledValue:3 timeoutMS:2000];
        int wrong = 0;
        for (uint32_t i = 0; i < 256; i++) if (((uint32_t *)b.contents)[i] != i * 3 + 1) wrong++;
        printf("  signal after dispatch: event %llu, %d wrong  %s\n", ev.signaledValue, wrong, got && !wrong ? "PASS" : "FAIL");
        bad += !(got && !wrong);
        // 2. a handle-opened event sees command buffer signals
        MTLSharedEventHandle *h = [ev newSharedEventHandle];
        id<MTLSharedEvent> ev2 = h ? [dev newSharedEventWithHandle:h] : nil;
        cb = [q commandBuffer];
        [cb encodeSignalEvent:ev value:5];
        [cb commit];
        [cb waitUntilCompleted];
        printf("  handle %s, other side sees %llu  %s\n", h ? "made" : "nil", ev2.signaledValue,
               ev2.signaledValue == 5 ? "PASS" : "FAIL");
        bad += ev2.signaledValue != 5;
        // 3. a command buffer waits for a value another thread sets 50 ms later
        memset(b.contents, 0, b.length);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC), dispatch_get_global_queue(0, 0), ^{
            ev2.signaledValue = 7;
        });
        const uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        cb = [q commandBuffer];
        [cb encodeWaitForEvent:ev value:7];
        ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:ps];
        [ce setBuffer:b offset:0 atIndex:0];
        [ce dispatchThreads:MTLSizeMake(256, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [ce endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        const double ms = (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1e6;
        wrong = 0;
        for (uint32_t i = 0; i < 256; i++) if (((uint32_t *)b.contents)[i] != i * 3 + 1) wrong++;
        printf("  wait released by another thread after %.0f ms, %d wrong  %s\n", ms, wrong,
               ms >= 40 && !wrong ? "PASS" : "FAIL");
        bad += !(ms >= 40 && !wrong);
        // 4. a listener fires on a command buffer signal
        dispatch_semaphore_t s = dispatch_semaphore_create(0);
        MTLSharedEventListener *l = [[MTLSharedEventListener alloc] initWithDispatchQueue:dispatch_get_global_queue(0, 0)];
        [ev notifyListener:l atValue:9 block:^(id<MTLSharedEvent> x, uint64_t v) { (void)x; (void)v; dispatch_semaphore_signal(s); }];
        cb = [q commandBuffer];
        [cb encodeSignalEvent:ev value:9];
        [cb commit];
        const long lw = dispatch_semaphore_wait(s, dispatch_time(DISPATCH_TIME_NOW, 1000 * NSEC_PER_MSEC));
        printf("  listener on command buffer signal  %s\n", lw == 0 ? "PASS" : "FAIL");
        bad += lw != 0;
        // 5. MTLEvent (not shared) orders two command buffers on two queues
        id<MTLEvent> pe = [dev newEvent];
        id<MTLCommandQueue> q2 = [dev newCommandQueue];
        memset(b.contents, 0, b.length);
        id<MTLCommandBuffer> c1 = [q commandBuffer];
        ce = [c1 computeCommandEncoder];
        [ce setComputePipelineState:ps];
        [ce setBuffer:b offset:0 atIndex:0];
        [ce dispatchThreads:MTLSizeMake(256, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [ce endEncoding];
        [c1 encodeSignalEvent:pe value:1];
        id<MTLCommandBuffer> c2 = [q2 commandBuffer];
        [c2 encodeWaitForEvent:pe value:1];
        id<MTLBlitCommandEncoder> be = [c2 blitCommandEncoder];
        id<MTLBuffer> b2 = [dev newBufferWithLength:b.length options:MTLResourceStorageModeShared];
        [be copyFromBuffer:b sourceOffset:0 toBuffer:b2 destinationOffset:0 size:b.length];
        [be endEncoding];
        [c1 commit];
        [c2 commit];
        [c2 waitUntilCompleted];
        wrong = 0;
        for (uint32_t i = 0; i < 256; i++) if (((uint32_t *)b2.contents)[i] != i * 3 + 1) wrong++;
        printf("  MTLEvent orders two queues (%s), %d wrong  %s\n", object_getClassName(pe), wrong, wrong ? "FAIL" : "PASS");
        bad += wrong != 0;
        printf("metal_event_test: %s (%d failed)\n", bad ? "FAIL" : "PASS", bad);
        return bad != 0;
    }
}
