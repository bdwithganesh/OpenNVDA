// GSP-RM performance queries through the kext's user RPC (selector 10,
// function 76 = GSP_RM_CONTROL on the kext's own client/subdevice):
// current P-state and each perf level's clock domains.
//   nvperf            P-state + levels 0..3
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

static io_connect_t open_gsp(void) {
    io_connect_t c = IO_OBJECT_NULL;
    io_service_t acc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVAccelerator"));
    if (acc && IOServiceOpen(acc, mach_task_self(), 0x4E564750, &c) == KERN_SUCCESS) { IOObjectRelease(acc); return c; }
    if (acc) IOObjectRelease(acc);
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVGspControl"));
    if (s && IOServiceOpen(s, mach_task_self(), 0, &c) == KERN_SUCCESS) { IOObjectRelease(s); return c; }
    return IO_OBJECT_NULL;
}

// RM control: header {hClient, hObject, cmd, status, paramsSize, flags} + params
static int rm_control(io_connect_t c, uint32_t cmd, void *params, uint32_t size, uint32_t *status) {
    uint8_t in[4 + 24 + 1024] = {0}, out[4 + 24 + 1024 + 64] = {0};
    const uint32_t fn = 76, client = 0xc0d00001, subdev = 0xc0d02080;
    memcpy(in, &fn, 4);
    memcpy(in + 4, &client, 4); memcpy(in + 8, &subdev, 4); memcpy(in + 12, &cmd, 4);
    memcpy(in + 20, &size, 4);
    memcpy(in + 28, params, size);
    size_t outSize = sizeof out;
    const kern_return_t kr = IOConnectCallStructMethod(c, 10, in, 28 + size, out, &outSize);
    if (kr != KERN_SUCCESS) { fprintf(stderr, "rpc 0x%x: kr 0x%x\n", cmd, kr); return -1; }
    uint32_t result = 0; memcpy(&result, out, 4);
    if (getenv("NVPERF_HEX")) {
        fprintf(stderr, "reply %zu bytes:", outSize);
        for (size_t k = 0; k < outSize && k < 96; k += 4) fprintf(stderr, " %08x", *(uint32_t *)(out + k));
        fprintf(stderr, "\n");
    }
    // the reply is the whole GSP message (queue element, VRPC header, then
    // the control struct): find our {hClient, hObject, cmd} and read after it
    size_t at = 0;
    for (size_t k = 4; k + 12 <= outSize; k += 4) {
        uint32_t w[3]; memcpy(w, out + k, 12);
        if (w[0] == client && w[1] == subdev && w[2] == cmd) { at = k; break; }
    }
    if (!at) { fprintf(stderr, "rpc 0x%x: control not found in the reply\n", cmd); return -1; }
    uint32_t st = 0; memcpy(&st, out + at + 12, 4);
    if (status) *status = st;
    if (at + 24 + size <= outSize) memcpy(params, out + at + 24, size);
    return (int)result;
}

int main(void) {
    io_connect_t c = open_gsp();
    if (!c) { printf("no GSP connection\n"); return 1; }
    uint32_t ps = 0, st = 0;
    int r = rm_control(c, 0x20802068, &ps, 4, &st);   // PERF_GET_CURRENT_PSTATE
    printf("current pstate mask 0x%x (rpc %d status 0x%x)\n", ps, r, st);
    // one domain per query: an unknown one fails the whole list
    for (uint32_t bit = 0; bit < 32; bit++) {
        int any = 0;
        char line[512]; int n = snprintf(line, sizeof line, "domain 0x%08x:", 1u << bit);
        for (uint32_t level = 0; level < 4; level++) {
            struct { uint32_t level, flags; struct { uint32_t flags, domain, cur, def, min, max; } clk[32]; uint32_t n; } p;
            memset(&p, 0, sizeof p);
            p.level = level; p.clk[0].domain = 1u << bit; p.n = 1;
            r = rm_control(c, 0x2080200b, &p, sizeof p, &st);   // PERF_GET_LEVEL_INFO_V2
            if (r || st) continue;
            any = 1;
            n += snprintf(line + n, sizeof line - n, "  L%u cur %u MHz (min %u max %u)", level, p.clk[0].cur / 1000, p.clk[0].min / 1000, p.clk[0].max / 1000);
        }
        if (any) printf("%s\n", line);
    }
    IOServiceClose(c);
    return 0;
}
