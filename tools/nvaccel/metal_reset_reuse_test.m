// Retain Metal resources across an explicit GPU reset. Default: no reset.
// Build: clang -fobjc-arc -O2 -Wall -Werror -framework Metal -framework
// Foundation -framework IOKit metal_reset_reuse_test.m -o metal_reset_reuse_test
// sudo ./metal_reset_reuse_test --reset [cycles]; --direct is an env setting
// NVMTL_NATIVE=0. Each command has fresh input, checked at completion.
#import <Metal/Metal.h>
#import <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

static bool prop(io_service_t s, CFStringRef key) {
    CFTypeRef p = IORegistryEntryCreateCFProperty(s, key, kCFAllocatorDefault, 0);
    bool v = p && CFGetTypeID(p) == CFBooleanGetTypeID() && CFBooleanGetValue(p);
    if (p) CFRelease(p);
    return v;
}
static bool reset(void) {
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVGspControl"));
    io_connect_t c = 0;
    kern_return_t r = s ? IOServiceOpen(s, mach_task_self(), 0, &c) : kIOReturnNotFound;
    uint64_t before = 0;
    uint32_t count = 1;
    if (!r) r = IOConnectCallScalarMethod(c, 37, NULL, 0, &before, &count);
    if (!r) r = IOConnectCallScalarMethod(c, 19, NULL, 0, NULL, NULL);
    printf("RESET request=0x%x\n", r); fflush(stdout);
    bool ready = false;
    for (int i = 0; !r && i < 200; i++) {
        usleep(100000);
        if (!prop(s, CFSTR("NVGspControl-reset-busy")) &&
            prop(s, CFSTR("NVGspControl-gr-persistent"))) { ready = true; break; }
    }
    if (ready) {
        uint64_t after = 0;
        count = 1;
        kern_return_t old = IOConnectCallScalarMethod(c, 37, NULL, 0, &after, &count);
        io_connect_t fresh = 0;
        kern_return_t now = IOServiceOpen(s, mach_task_self(), 0, &fresh);
        count = 1;
        if (!now) now = IOConnectCallScalarMethod(fresh, 37, NULL, 0, &after, &count);
        if (fresh) IOServiceClose(fresh);
        ready = old == kIOReturnOffline && now == KERN_SUCCESS && before != after;
        printf("GENERATION old=0x%x fresh=0x%x before=%llu after=%llu %s\n", old, now,
               (unsigned long long)before, (unsigned long long)after, ready ? "PASS" : "FAIL");
    }
    if (c) IOServiceClose(c);
    if (s) IOObjectRelease(s);
    printf("RESET ready=%d\n", ready); fflush(stdout);
    return ready;
}
int main(int argc, char **argv) {
    bool doReset = false, managed = false;
    int cycles = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--managed-source")) managed = true;
        else if (!strcmp(argv[i], "--reset") && !doReset && i + 1 < argc) {
            char *end = NULL;
            long n = strtol(argv[++i], &end, 10);
            if (!end || *end || n < 1 || n > 10) return 2;
            doReset = true; cycles = (int)n;
        } else {
            fprintf(stderr, "usage: %s [--reset 1..10] [--managed-source]\n", argv[0]);
            return 2;
        }
    }
    alarm(50u * (unsigned)cycles + 20u);
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
             "kernel void inc(device uint *b [[buffer(0)]], device const uint *a [[buffer(1)]], uint i [[thread_position_in_grid]]) { b[i] += a[i]; }"
            options:nil error:&e];
        id<MTLComputePipelineState> ps = lib ? [d newComputePipelineStateWithFunction:[lib newFunctionWithName:@"inc"] error:&e] : nil;
        id<MTLCommandQueue> q = [d newCommandQueue];
        const NSUInteger n = 4096;
        id<MTLBuffer> b = [d newBufferWithLength:n * sizeof(uint32_t) options:MTLResourceStorageModeShared];
        MTLResourceOptions sourceMode = managed && !d.hasUnifiedMemory ? MTLResourceStorageModeManaged : MTLResourceStorageModeShared;
        id<MTLBuffer> source = [d newBufferWithLength:n * sizeof(uint32_t) options:sourceMode];
        if (!ps || !q || !b || !b.contents || !source || !source.contents) { printf("setup failed %s\n", e.description.UTF8String); return 1; }
        uint32_t *a = source.contents;
        for (NSUInteger i = 0; i < n; i++) a[i] = managed ? (uint32_t)i + 17u : 17u;
        if (source.storageMode == MTLStorageModeManaged) [source didModifyRange:NSMakeRange(0, source.length)];
        printf("device=%s retained queue/pipeline/buffer reset=%d cycles=%d\n", d.name.UTF8String, doReset, cycles);
        printf("immutable source storageMode=%lu modified once before first submission\n", (unsigned long)source.storageMode);
        int failures = 0;
        for (int cycle = 0; cycle <= (doReset ? cycles : 0); cycle++) {
            if (cycle && !reset()) return 1;
            for (int round = 0; round < 6; round++) {
                uint32_t *p = b.contents;
                uint32_t seed = (uint32_t)(cycle * 100 + round * 3 + 1);
                for (NSUInteger i = 0; i < n; i++) p[i] = seed + (uint32_t)i;
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
                [ce setComputePipelineState:ps]; [ce setBuffer:b offset:0 atIndex:0];
                [ce setBuffer:source offset:0 atIndex:1];
                [ce dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
                [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
                NSUInteger bad = 0;
                for (NSUInteger i = 0; i < n; i++) bad += p[i] != seed + (uint32_t)i + a[i];
                bool recoveryError = cycle && round == 0 && cb.status == MTLCommandBufferStatusError;
                bool ok = cb.status == MTLCommandBufferStatusCompleted && !bad;
                if (!ok && !recoveryError) failures++;
                printf("cycle=%d round=%d status=%ld bad=%lu %s error=%s\n", cycle, round,
                       (long)cb.status, (unsigned long)bad, ok ? "PASS" : recoveryError ? "EXPECTED_RECOVERY_ERROR" : "FAIL",
                       cb.error ? cb.error.description.UTF8String : "-");
            }
        }
        printf("%s failures=%d\n", failures ? "FAIL" : "PASS", failures);
        return failures ? 1 : 0;
    }
}
