#include "metallib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t le(const uint8_t *p, unsigned n) {
    uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int is_bitcode(const uint8_t *p, uint32_t n) {
    return n >= 4 && ((p[0] == 'B' && p[1] == 'C' && p[2] == 0xc0 && p[3] == 0xde) ||
                      (p[0] == 0xde && p[1] == 0xc0 && p[2] == 0x17 && p[3] == 0x0b));
}

int mtllib_parse(const uint8_t *d, size_t len, mtllib *out, char *err, size_t errlen) {
    memset(out, 0, sizeof(*out));
    /* universal file (0xcafebabe): one metallib per GPU family. Take the
     * slice whose functions carry LLVM bitcode (generic AIR); the others
     * hold Apple GPU machine code. */
    if (len >= 8 && be32(d) == 0xcafebabe) {
        const uint32_t n = be32(d + 4);
        for (uint32_t i = 0; i < n && 8 + (i + 1) * 20 <= len; ++i) {
            const uint8_t *a = d + 8 + i * 20;
            const uint32_t off = be32(a + 8), size = be32(a + 12);
            if ((uint64_t)off + size > len) continue;
            mtllib sl;
            if (mtllib_parse(d + off, size, &sl, err, errlen)) continue;
            if (sl.nfunctions && is_bitcode(sl.functions[0].bitcode, sl.functions[0].bitcode_len)) {
                *out = sl;
                return 0;
            }
            mtllib_free(&sl);
        }
        snprintf(err, errlen, "universal metallib without an AIR slice");
        return -1;
    }
    if (len < 0x58 || memcmp(d, "MTLB", 4)) { snprintf(err, errlen, "not a metallib"); return -1; }
    const uint64_t list = le(d + 0x18, 8), list_len = le(d + 0x20, 8);
    const uint64_t bc = le(d + 0x48, 8), bc_len = le(d + 0x50, 8);
    if (list + list_len > len || bc + bc_len > len || list_len < 4) {
        snprintf(err, errlen, "metallib sections out of range");
        return -1;
    }
    const uint32_t count = (uint32_t)le(d + list, 4);
    out->functions = calloc(count ? count : 1, sizeof(mtllib_function));
    size_t p = list + 4;
    for (uint32_t i = 0; i < count; ++i) {
        if (p + 4 > list + list_len) { snprintf(err, errlen, "function list truncated"); return -1; }
        const uint32_t entry = (uint32_t)le(d + p, 4);
        size_t t = p + 4;
        const size_t end = p + entry;
        mtllib_function *f = &out->functions[out->nfunctions];
        uint64_t off = ~0ull, size = 0;
        while (t + 4 <= end && memcmp(d + t, "ENDT", 4)) {
            const uint32_t n = (uint32_t)le(d + t + 4, 2);
            const uint8_t *v = d + t + 6;
            if (t + 6 + n > end) break;
            if (!memcmp(d + t, "NAME", 4)) snprintf(f->name, sizeof f->name, "%.*s", (int)n, (const char *)v);
            else if (!memcmp(d + t, "TYPE", 4) && n >= 1) f->type = v[0];
            else if (!memcmp(d + t, "HASH", 4) && n >= 32) memcpy(f->hash, v, 32);
            else if (!memcmp(d + t, "MDSZ", 4) && n >= 8) size = le(v, 8);
            else if (!memcmp(d + t, "OFFT", 4) && n >= 24) off = le(v + 16, 8);
            t += 6 + n;
        }
        p = end;
        if (off == ~0ull || off + size > bc_len) { snprintf(err, errlen, "%s: bad bitcode range", f->name); return -1; }
        f->bitcode = d + bc + off;
        f->bitcode_len = (uint32_t)size;
        ++out->nfunctions;
    }
    return 0;
}

void mtllib_free(mtllib *l) { free(l->functions); memset(l, 0, sizeof(*l)); }
