// After an actual global GPU reset, explicit private-channel opens must be
// refused without allocating a new RM context. This probe never resets the GPU.
// clang -O2 -Wall -Werror gpu_channel_fallback_test.c -framework IOKit
//   -framework CoreFoundation -o gpu_channel_fallback_test
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
static long long property(io_service_t s, CFStringRef key) {
    CFTypeRef p = IORegistryEntryCreateCFProperty(s, key, NULL, 0);
    long long v = -1;
    if (p && CFGetTypeID(p) == CFNumberGetTypeID()) CFNumberGetValue(p, kCFNumberSInt64Type, &v);
    if (p && CFGetTypeID(p) == CFBooleanGetTypeID()) v = CFBooleanGetValue(p);
    if (p) CFRelease(p);
    return v;
}
int main(void) {
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVGspControl"));
    if (!s) return 1;
    long long reset = property(s, CFSTR("NVGspControl-gpu-reset-count"));
    long long fallback = property(s, CFSTR("NVGspControl-ownchannel-reset-fallback"));
    long long before = property(s, CFSTR("NVGspControl-cchan-opens"));
    if (reset < 1 || fallback != 1 || before < 0 ||
        property(s, CFSTR("NVGspControl-ownchannel-auto")) != 0 ||
        property(s, CFSTR("NVGspControl-gr-persistent")) != 1 ||
        property(s, CFSTR("NVGspControl-reset-busy")) != 0) {
        printf("requires completed reset and active shared fallback\n"); return 2;
    }
    io_connect_t c = 0;
    kern_return_t kr = IOServiceOpen(s, mach_task_self(), 0, &c);
    if (kr) return 1;
    uint64_t in = 1, out = 0;
    uint32_t n = 1;
    kr = IOConnectCallScalarMethod(c, 40, &in, 1, &out, &n);
    long long after = property(s, CFSTR("NVGspControl-cchan-opens"));
    int ok = kr == kIOReturnUnsupported && before == after;
    if (kr == KERN_SUCCESS) { // clean up an unexpected successful open
        in = 0; n = 1;
        IOConnectCallScalarMethod(c, 40, &in, 1, &out, &n);
    }
    printf("post-reset explicit open kr=0x%x opens=%lld->%lld reset=%lld: %s\n",
           kr, before, after, reset, ok ? "PASS" : "FAIL");
    IOServiceClose(c); IOObjectRelease(s);
    return !ok;
}
