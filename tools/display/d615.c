// NVGspControl 0.180.135 selector 49 (admin display transport) client.
//   d615 alloc PARENT OBJECT CLASS [HEXPARAMS]   -> rm status + returned params
//   d615 ctrl OBJECT COMMAND HEXPARAMS           -> rm status + returned params
//   d615 ictrl COMMAND HEXPARAMS                 -> internal display control (0.180.136)
//   d615 free OBJECT
//   d615 rd ADDR DWORDS        BAR0 read (0x610000-0x6fffff)
//   d615 wr ADDR HEXWORDS      BAR0 write (little-endian dwords)
//   d615 vrd OFFSET DWORDS     VRAM read
//   d615 vwr OFFSET HEXBYTES   VRAM write
//   d615 vfile OFFSET FILE     VRAM write of a whole file (3840-byte chunks)
// Output is one JSON line.
#include <IOKit/IOKitLib.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t kClient = 0xc0d00001;
static io_connect_t gConn;

static uint32_t u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t num(const char *s) { return strtoull(s, NULL, 0); }
static int digit(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
static int unhex(const char *s, uint8_t *out, size_t cap) {
    size_t n = strlen(s);
    if (n % 2 || n / 2 > cap) return -1;
    for (size_t i = 0; i < n / 2; i++) {
        int hi = digit(s[2 * i]), lo = digit(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)(hi * 16 + lo);
    }
    return (int)(n / 2);
}
static void hex(const uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) printf("%02x", p[i]); }

// op header + payload -> selector 49
static kern_return_t call(uint32_t op, uint32_t a, uint64_t b, const uint8_t *payload, size_t bytes,
                          uint8_t *out, size_t *outBytes) {
    uint8_t in[3904] = {0};
    if (bytes > sizeof(in) - 16) return kIOReturnBadArgument;
    memcpy(in, &op, 4); memcpy(in + 4, &a, 4); memcpy(in + 8, &b, 8);
    if (bytes) memcpy(in + 16, payload, bytes);
    // the transport refuses (NotReady/Busy) while a native operation runs, e.g.
    // WindowServer's Metal on the same GPU: retry for up to ~10 s
    const size_t capacity = *outBytes;
    kern_return_t kr = kIOReturnNotReady;
    for (int i = 0; i < 5000; i++) {
        *outBytes = capacity;
        kr = IOConnectCallStructMethod(gConn, 49, in, 16 + bytes, out, outBytes);
        if (kr != kIOReturnNotReady && kr != kIOReturnBusy) break;
        usleep(2000);
    }
    return kr;
}

int main(int argc, char **argv) {
    if (argc < 3) { fputs("usage: see source\n", stderr); return 2; }
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVGspControl"));
    if (!s || IOServiceOpen(s, mach_task_self(), 0, &gConn)) { printf("{\"error\":\"open\"}\n"); return 1; }
    static uint8_t req[3904], out[4096];
    size_t outBytes = sizeof(out);
    kern_return_t kr;
    const char *cmd = argv[1];
    if (!strcmp(cmd, "alloc") && argc >= 5) {
        int n = argc > 5 ? unhex(argv[5], req + 32, sizeof(req) - 32) : 0;
        if (n < 0) return 2;
        const uint32_t h[8] = {kClient, (uint32_t)num(argv[2]), (uint32_t)num(argv[3]), (uint32_t)num(argv[4]), 0, (uint32_t)n, 0, 0};
        memcpy(req, h, 32);
        kr = call(0, 103, 0, req, 32 + n, out, &outBytes);
        printf("{\"op\":\"alloc\",\"kr\":\"0x%x\",\"rpc\":\"0x%x\"", kr, u32(out));
        if (!kr && outBytes >= 4 + 88) {
            printf(",\"status\":\"0x%x\",\"params\":\"", u32(out + 4 + 72));
            hex(out + 4 + 88, outBytes - 4 - 88 < (size_t)n ? outBytes - 4 - 88 : (size_t)n);
            printf("\"");
        }
        printf("}\n");
    } else if (!strcmp(cmd, "ctrl") && argc >= 5) {
        int n = unhex(argv[4], req + 40, sizeof(req) - 40);
        if (n < 0) return 2;
        const uint32_t h[10] = {kClient, (uint32_t)num(argv[2]), (uint32_t)num(argv[3]), 0, (uint32_t)n, 0, 0, 0, 0, 0};
        memcpy(req, h, 40);
        kr = call(0, 76, 0, req, 40 + n, out, &outBytes);
        printf("{\"op\":\"ctrl\",\"kr\":\"0x%x\",\"rpc\":\"0x%x\"", kr, u32(out));
        if (!kr && outBytes >= 4 + 96) {
            printf(",\"status\":\"0x%x\",\"params\":\"", u32(out + 4 + 68));
            hex(out + 4 + 96, outBytes - 4 - 96 < (size_t)n ? outBytes - 4 - 96 : (size_t)n);
            printf("\"");
        }
        printf("}\n");
    } else if (!strcmp(cmd, "ictrl") && argc >= 4) {
        int n = unhex(argv[3], req, sizeof(req) - 16);
        if (n < 0) return 2;
        kr = call(5, (uint32_t)num(argv[2]), 0, req, n, out, &outBytes);
        printf("{\"op\":\"ictrl\",\"kr\":\"0x%x\",\"rpc\":\"0x%x\"", kr, u32(out));
        if (!kr && outBytes >= 4 + 96) {
            printf(",\"status\":\"0x%x\",\"params\":\"", u32(out + 4 + 68));
            hex(out + 4 + 96, outBytes - 4 - 96 < (size_t)n ? outBytes - 4 - 96 : (size_t)n);
            printf("\"");
        }
        printf("}\n");
    } else if (!strcmp(cmd, "free")) {
        const uint32_t h[4] = {kClient, 0, (uint32_t)num(argv[2]), 0};
        kr = call(0, 10, 0, (const uint8_t *)h, 16, out, &outBytes);
        printf("{\"op\":\"free\",\"kr\":\"0x%x\",\"rpc\":\"0x%x\"}\n", kr, u32(out));
    } else if ((!strcmp(cmd, "rd") || !strcmp(cmd, "vrd")) && argc >= 4) {
        kr = call(cmd[0] == 'v' ? 2 : 3, (uint32_t)num(argv[3]), num(argv[2]), NULL, 0, out, &outBytes);
        printf("{\"op\":\"%s\",\"kr\":\"0x%x\",\"words\":[", cmd, kr);
        for (size_t i = 4; !kr && i + 4 <= outBytes; i += 4) printf("%s\"0x%08x\"", i > 4 ? "," : "", u32(out + i));
        printf("]}\n");
    } else if (!strcmp(cmd, "vdump") && argc >= 5) {   // ADDR BYTES FILE: VRAM to a file, 4000 bytes per call
        const uint64_t at = num(argv[2]), bytes = num(argv[3]);
        FILE *f = fopen(argv[4], "wb");
        if (!f) return 2;
        kr = 0;
        for (uint64_t o = 0; !kr && o < bytes; o += 4000) {
            const uint32_t words = (uint32_t)((bytes - o < 4000 ? bytes - o : 4000) / 4);
            outBytes = sizeof(out);
            kr = call(2, words, at + o, NULL, 0, out, &outBytes);
            if (!kr) fwrite(out + 4, 1, outBytes - 4, f);
        }
        fclose(f);
        printf("{\"op\":\"vdump\",\"kr\":\"0x%x\"}\n", kr);
    } else if ((!strcmp(cmd, "wr") || !strcmp(cmd, "vwr")) && argc >= 4) {
        int n = unhex(argv[3], req, sizeof(req) - 16);
        if (n < 0) return 2;
        kr = call(cmd[0] == 'v' ? 1 : 4, 0, num(argv[2]), req, n, out, &outBytes);
        printf("{\"op\":\"%s\",\"kr\":\"0x%x\"}\n", cmd, kr);
    } else if (!strcmp(cmd, "sysfb") && argc >= 3) {
        kr = call(6, (uint32_t)num(argv[2]), 0, NULL, 0, out, &outBytes);
        uint64_t phys = 0, bytes = 0;
        if (!kr && outBytes >= 20) { memcpy(&phys, out + 4, 8); memcpy(&bytes, out + 12, 8); }
        printf("{\"op\":\"sysfb\",\"kr\":\"0x%x\",\"phys\":\"0x%llx\",\"bytes\":%llu}\n", kr, phys, bytes);
    } else if (!strcmp(cmd, "fbcopy") && argc >= 6) {   // MODE DST PITCH LINES (0.180.138)
        const uint32_t geo[2] = {(uint32_t)num(argv[4]), (uint32_t)num(argv[5])};
        kr = call(7, (uint32_t)num(argv[2]), num(argv[3]), (const uint8_t *)geo, 8, out, &outBytes);
        printf("{\"op\":\"fbcopy\",\"kr\":\"0x%x\"}\n", kr);
    } else if (!strcmp(cmd, "native") && argc >= 4) {   // CURSOR_SURFACE CURSOR_USER (0.180.141)
        uint8_t p[12];
        const uint64_t surf = num(argv[2]);
        const uint32_t user = (uint32_t)num(argv[3]);
        memcpy(p, &surf, 8); memcpy(p + 8, &user, 4);
        kr = call(10, surf ? 1 : 0, 0, p, 12, out, &outBytes);
        printf("{\"op\":\"native\",\"kr\":\"0x%x\"}\n", kr);
    } else if (!strcmp(cmd, "dbl") && argc >= 3) {   // BACK [WND_PB WND_USER]; BACK 0 = one buffer (0.180.181)
        uint8_t p[12] = {0};
        const uint64_t back = num(argv[2]), pb = argc >= 5 ? num(argv[3]) : 0;
        const uint32_t user = argc >= 5 ? (uint32_t)num(argv[4]) : 0;
        memcpy(p, &pb, 8); memcpy(p + 8, &user, 4);
        kr = call(11, back ? 1 : 0, back, p, 12, out, &outBytes);
        printf("{\"op\":\"dbl\",\"kr\":\"0x%x\"}\n", kr);
    } else if (!strcmp(cmd, "scaled") && argc >= 5) {   // CORE_PB FRONT BACK: scaled scan-out buffers (0.180.200)
        uint64_t p[3] = {num(argv[2]), num(argv[3]), num(argv[4])};
        kr = call(12, 0, 0, (const uint8_t *)p, 24, out, &outBytes);
        printf("{\"op\":\"scaled\",\"kr\":\"0x%x\"}\n", kr);
    } else if (!strcmp(cmd, "mode") && argc >= 4) {     // W H: scan-out size, native = 1:1 (0.180.200)
        kr = call(13, (uint32_t)num(argv[2]), num(argv[3]), NULL, 0, out, &outBytes);
        printf("{\"op\":\"mode\",\"kr\":\"0x%x\"}\n", kr);
    } else if (!strcmp(cmd, "reserve") && argc >= 4) {  // MIB PHYS (0.180.138)
        kr = call(8, (uint32_t)num(argv[2]), num(argv[3]), NULL, 0, out, &outBytes);
        printf("{\"op\":\"reserve\",\"kr\":\"0x%x\"}\n", kr);
    } else if (!strcmp(cmd, "fbfile") && argc >= 4) {   // OFFSET FILE into the sysmem framebuffer (0.180.140)
        FILE *f = fopen(argv[3], "rb");
        if (!f) return 2;
        uint64_t at = num(argv[2]), done = 0;
        size_t n;
        kr = 0;
        while (!kr && (n = fread(req, 1, 3840, f)) > 0) {
            outBytes = sizeof(out);
            kr = call(9, 0, at + done, req, n, out, &outBytes);
            done += n;
        }
        fclose(f);
        printf("{\"op\":\"fbfile\",\"kr\":\"0x%x\",\"bytes\":%llu}\n", kr, done);
    } else if (!strcmp(cmd, "vfile") && argc >= 4) {
        FILE *f = fopen(argv[3], "rb");
        if (!f) return 2;
        uint64_t at = num(argv[2]), done = 0;
        size_t n;
        kr = 0;
        while (!kr && (n = fread(req, 1, 3840, f)) > 0) {
            outBytes = sizeof(out);
            kr = call(1, 0, at + done, req, (n + 3) & ~3UL, out, &outBytes);
            done += n;
        }
        fclose(f);
        printf("{\"op\":\"vfile\",\"kr\":\"0x%x\",\"bytes\":%llu}\n", kr, done);
    } else { fputs("bad command\n", stderr); return 2; }
    IOServiceClose(gConn);
    return kr != 0;
}
