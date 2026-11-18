/* find_visible_module: find the metallib module that defines an inner visible
 * function (prefixPrimary_f, ...) and write that module's AIR bitcode.
 *
 * MPS metallibs do not expose these as top-level MTLFunctions, so the driver's
 * auto-link needs the containing module. This tool is the standalone proof and
 * the test helper for tools/nakc/check_visible_link.sh.
 *
 * Build:
 *   cc -O2 -I drivers/nvidia-metal-drivers-macos/NVMTLDriver/air tools/nakc/find_visible_module.c \
 *      drivers/nvidia-metal-drivers-macos/NVMTLDriver/air/metallib.c drivers/nvidia-metal-drivers-macos/NVMTLDriver/air/airir.c \
 *      drivers/nvidia-metal-drivers-macos/NVMTLDriver/air/bitstream.c -o find_visible_module
 * Usage: find_visible_module LIB.metallib NAME OUT.air
 */
#include "metallib.h"
#include "airir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s LIB.metallib NAME OUT.air\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 1; }
    long n = ftell(f);
    if (n <= 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 1; }
    unsigned char *d = malloc((size_t)n);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); return 1; }
    fclose(f);

    mtllib l;
    char err[256];
    if (mtllib_parse(d, (size_t)n, &l, err, sizeof err)) {
        fprintf(stderr, "parse: %s\n", err);
        free(d);
        return 1;
    }
    const char *want = argv[2];
    for (uint32_t i = 0; i < l.nfunctions; ++i) {
        air_module m;
        if (air_read(l.functions[i].bitcode, l.functions[i].bitcode_len, &m, err, sizeof err))
            continue;
        for (uint32_t j = 0; j < m.nfunctions; ++j) {
            if (!m.functions[j].name || strcmp(m.functions[j].name, want)) continue;
            FILE *o = fopen(argv[3], "wb");
            if (!o) { perror("out"); air_free(&m); mtllib_free(&l); free(d); return 1; }
            fwrite(l.functions[i].bitcode, 1, l.functions[i].bitcode_len, o);
            fclose(o);
            printf("found %s in module %s (%u bytes, type %u)\n", want,
                   l.functions[i].name, l.functions[i].bitcode_len, l.functions[i].type);
            air_free(&m);
            mtllib_free(&l);
            free(d);
            return 0;
        }
        air_free(&m);
    }
    fprintf(stderr, "%s not found in %u modules\n", want, l.nfunctions);
    mtllib_free(&l);
    free(d);
    return 1;
}
