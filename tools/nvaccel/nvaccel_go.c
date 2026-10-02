// Ask NVGspControl (>= 0.132.0, selector 32) to publish NVAcceleratorGo, the
// resource NVAccelerator's personality waits for (B4).
#include <IOKit/IOKitLib.h>
#include <stdio.h>
int main(void) {
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVGspControl"));
    io_connect_t c;
    if (!s || IOServiceOpen(s, mach_task_self(), 0, &c)) { fprintf(stderr, "open NVGspControl failed\n"); return 1; }
    kern_return_t kr = IOConnectCallScalarMethod(c, 32, NULL, 0, NULL, NULL);
    IOServiceClose(c);
    if (kr) { fprintf(stderr, "accelGo: 0x%x\n", kr); return 1; }
    return 0;
}
