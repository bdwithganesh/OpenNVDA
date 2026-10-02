// Runtime API contract checks from the M1/RTX property comparison.
// GPU timestamp frequency must describe sampleTimestamps' GPU clock.
// Optional sparse support queries must return a Boolean without throwing.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
    @autoreleasepool {
        id<MTLDevice> d = MTLCreateSystemDefaultDevice();
        if (!d) return 1;
        int failed = 0;
        SEL sparse = sel_registerName("supportsPlacementSparse");
        if ([d respondsToSelector:sparse]) {
            @try {
                BOOL supported = ((BOOL (*)(id, SEL))objc_msgSend)(d, sparse);
                printf("supportsPlacementSparse=%d (query completed)\n", supported);
            } @catch (NSException *e) {
                printf("supportsPlacementSparse THREW: %s\n", e.reason.UTF8String);
                failed++;
            }
        } else {
            printf("supportsPlacementSparse unavailable on this runtime\n");
        }
        SEL freqSel = sel_registerName("queryTimestampFrequency");
        if ([d respondsToSelector:freqSel]) {
            @try {
                uint64_t freq = ((uint64_t (*)(id, SEL))objc_msgSend)(d, freqSel);
                MTLTimestamp c0 = 0, g0 = 0, c1 = 0, g1 = 0;
                [d sampleTimestamps:&c0 gpuTimestamp:&g0];
                usleep(20000);
                [d sampleTimestamps:&c1 gpuTimestamp:&g1];
                mach_timebase_info_data_t tb;
                mach_timebase_info(&tb);
                double cpuSeconds = (double)(c1 - c0) * tb.numer / tb.denom / 1e9;
                double gpuSeconds = freq ? (double)(g1 - g0) / freq : 0;
                double ratio = cpuSeconds > 0 ? gpuSeconds / cpuSeconds : 0;
                BOOL ok = freq && c1 > c0 && g1 > g0 && ratio > 0.8 && ratio < 1.2;
                printf("timestamp frequency=%llu cpu=%.6f s gpu=%.6f s ratio=%.4f %s\n",
                       (unsigned long long)freq, cpuSeconds, gpuSeconds, ratio, ok ? "PASS" : "FAIL");
                failed += !ok;
            } @catch (NSException *e) {
                printf("timestamp query THREW: %s\n", e.reason.UTF8String);
                failed++;
            }
        } else {
            printf("queryTimestampFrequency unavailable on this runtime\n");
        }
        printf("metal_capability_contract_test on %s: %s\n", d.name.UTF8String, failed ? "FAIL" : "PASS");
        return failed != 0;
    }
}
