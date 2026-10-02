// HW cursor bring-up through NVGspControl selector 35 (D2).
//   nvcursor 0   setup (PIO channel, ctxdma)   1  test image + show at 64,64   2  hide
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: nvcursor 0|1|2\n"); return 2; }
    uint64_t op = strtoull(argv[1], NULL, 0);
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVGspControl"));
    io_connect_t c = 0;
    if (!s || IOServiceOpen(s, mach_task_self(), 0, &c)) { fprintf(stderr, "open failed\n"); return 1; }
    kern_return_t kr = IOConnectCallScalarMethod(c, 35, &op, 1, NULL, NULL);
    printf("cursorTest(%llu) kr 0x%x\n", op, kr);
    IOServiceClose(c);
    return kr != 0;
}
