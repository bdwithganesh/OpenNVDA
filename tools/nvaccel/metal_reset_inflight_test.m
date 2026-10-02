// Submit retained resources while another thread resets the GPU. A lost
// buffer may fail, but Completed must always mean data-correct. --reset is
// explicit/admin-only; default runs the same work without resetting.
#import <Metal/Metal.h>
#import <IOKit/IOKitLib.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint32_t reference(uint32_t x) {
    for (uint32_t j = 0; j < 32768; j++) x = (x * 1664525u + 1013904223u) ^ j;
    return x;
}
int main(int argc, char **argv) {
    bool doReset = argc == 2 && !strcmp(argv[1], "--reset");
    if (argc > 1 && !doReset) return 2;
    alarm(65);
    setvbuf(stdout, NULL, _IONBF, 0);
    @autoreleasepool {
        id<MTLDevice> d = nil;
        for (id<MTLDevice> x in MTLCopyAllDevices())
            if ([x.name containsString:@"NVIDIA"] || [x.name containsString:@"RTX"]) { d = x; break; }
        if (!d && !doReset) d = MTLCreateSystemDefaultDevice();
        if (!d) return 1;
        NSError *e = nil;
        id<MTLLibrary> lib = [d newLibraryWithSource:
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "kernel void spin(device uint *b [[buffer(0)]], uint i [[thread_position_in_grid]]) {"
             " uint x=b[i]; for(uint j=0;j<32768;j++) x=(x*1664525u+1013904223u)^j; b[i]=x; }"
            options:nil error:&e];
        id<MTLComputePipelineState> ps = lib ? [d newComputePipelineStateWithFunction:[lib newFunctionWithName:@"spin"] error:&e] : nil;
        id<MTLCommandQueue> q = [d newCommandQueue];
        if (!ps || !q) { printf("setup failed %s\n", e.description.UTF8String); return 1; }
        const NSUInteger n = 8192, count = 64;
        NSMutableArray *buffers = [NSMutableArray new], *commands = [NSMutableArray new];
        uint32_t *want = malloc(n * sizeof(*want));
        if (!want) return 1;
        for (NSUInteger i = 0; i < n; i++) want[i] = reference((uint32_t)i + 7u);
        for (NSUInteger k = 0; k < count; k++) {
            id<MTLBuffer> b = [d newBufferWithLength:n * 4 options:MTLResourceStorageModeShared];
            if (!b || !b.contents) { free(want); return 1; }
            uint32_t *p = b.contents;
            for (NSUInteger i = 0; i < n; i++) p[i] = (uint32_t)i + 7u;
            [buffers addObject:b];
        }
        printf("device=%s buffers=%lu reset=%d\n", d.name.UTF8String, (unsigned long)count, doReset);
        dispatch_group_t group = dispatch_group_create();
        __block _Atomic int resetResult = 0;
        for (NSUInteger k = 0; k < count; k++) {
            if (doReset && k == 8) {
                dispatch_group_async(group, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
                    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVGspControl"));
                    io_connect_t c = 0;
                    kern_return_t r = s ? IOServiceOpen(s, mach_task_self(), 0, &c) : kIOReturnNotFound;
                    if (!r) r = IOConnectCallScalarMethod(c, 19, NULL, 0, NULL, NULL);
                    printf("overlap RESET request=0x%x\n", r);
                    atomic_store(&resetResult, r ? -1 : 1);
                    if (c) IOServiceClose(c);
                    if (s) IOObjectRelease(s);
                });
            }
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            [ce setComputePipelineState:ps]; [ce setBuffer:buffers[k] offset:0 atIndex:0];
            [ce dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
            [ce endEncoding]; [commands addObject:cb]; [cb commit];
        }
        if (dispatch_group_wait(group, dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC))) return 1;
        NSUInteger errors = 0, falseSuccess = 0, other = 0;
        for (NSUInteger k = 0; k < count; k++) {
            id<MTLCommandBuffer> cb = commands[k];
            [cb waitUntilCompleted];
            const uint32_t *p = [buffers[k] contents];
            NSUInteger bad = 0;
            for (NSUInteger i = 0; i < n; i++) bad += p[i] != want[i];
            if (cb.status == MTLCommandBufferStatusError) errors++;
            else if (cb.status == MTLCommandBufferStatusCompleted) falseSuccess += bad != 0;
            else other++;
            printf("cb=%lu status=%ld bad=%lu\n", (unsigned long)k, (long)cb.status, (unsigned long)bad);
        }
        free(want);
        bool ok = !falseSuccess && !other && (doReset ? atomic_load(&resetResult) == 1 : errors == 0);
        printf("%s errors=%lu falseSuccess=%lu other=%lu resetResult=%d\n", ok ? "PASS" : "FAIL",
               (unsigned long)errors, (unsigned long)falseSuccess, (unsigned long)other, atomic_load(&resetResult));
        return ok ? 0 : 1;
    }
}
