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
    if (!d || !out) return -1;
    memset(out, 0, sizeof(*out));
    if (err && errlen) err[0] = 0;
    /* universal file (0xcafebabe): one metallib per GPU family. Take the
     * slice whose functions carry LLVM bitcode (generic AIR); the others
     * hold Apple GPU machine code. */
    if (len >= 8 && be32(d) == 0xcafebabe) {
        const uint32_t n = be32(d + 4);
        for (uint32_t i = 0; i < n && i < 4096; ++i) {
            if (8 + (size_t)i * 20 > len || 20 > len - (8 + (size_t)i * 20)) break;
            const uint8_t *a = d + 8 + (size_t)i * 20;
            const uint32_t off = be32(a + 8), size = be32(a + 12);
            if (off > len || size > len - off) continue;
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
    /* The section length counts tagged entries, excluding the leading u32
     * function count. Real Apple-produced fixtures place the last entry four
     * bytes beyond list + list_len. Bound the actual entry region without
     * discarding the count prefix or permitting integer wraparound. */
    if (list > len || 4 > len - list || list_len > len - list - 4 ||
        bc > len || bc_len > len - bc) {
        snprintf(err, errlen, "metallib sections out of range");
        return -1;
    }
    const size_t list_end = (size_t)list + 4 + (size_t)list_len;
    const uint32_t count = (uint32_t)le(d + list, 4);
    // A file-controlled count must not drive a multi-terabyte calloc. Real
    // metallibs have a few thousand functions at most.
    if (count > (1u << 20)) { snprintf(err, errlen, "function count %u too large", count); return -1; }
    if (count > list_len / 4) { snprintf(err, errlen, "function count exceeds entry region"); return -1; }
    out->functions = calloc(count ? count : 1, sizeof(mtllib_function));
    if (!out->functions) { snprintf(err, errlen, "function table allocation failed"); return -1; }
    size_t p = list + 4;
    for (uint32_t i = 0; i < count; ++i) {
        if (p > list_end || 4 > list_end - p) {
            snprintf(err, errlen, "function list truncated"); goto fail;
        }
        const uint32_t entry = (uint32_t)le(d + p, 4);
        if (entry < 4 || entry > list_end - p) {
            snprintf(err, errlen, "function entry out of range"); goto fail;
        }
        size_t t = p + 4;
        const size_t end = p + entry;
        mtllib_function *f = &out->functions[out->nfunctions];
        uint64_t off = ~0ull, size = 0;
        while (t + 4 <= end && memcmp(d + t, "ENDT", 4)) {
            if (t + 6 > end) break;
            const uint32_t n = (uint32_t)le(d + t + 4, 2);
            const uint8_t *v = d + t + 6;
            if (n > end - t - 6) break;
            if (!memcmp(d + t, "NAME", 4)) snprintf(f->name, sizeof f->name, "%.*s", (int)n, (const char *)v);
            else if (!memcmp(d + t, "TYPE", 4) && n >= 1) f->type = v[0];
            else if (!memcmp(d + t, "HASH", 4) && n >= 32) memcpy(f->hash, v, 32);
            else if (!memcmp(d + t, "MDSZ", 4) && n >= 8) size = le(v, 8);
            else if (!memcmp(d + t, "OFFT", 4) && n >= 24) off = le(v + 16, 8);
            t += 6 + n;
        }
        p = end;
        if (off == ~0ull || off > bc_len || size > bc_len - off ||
            off > SIZE_MAX - (size_t)bc) {
            snprintf(err, errlen, "%s: bad bitcode range", f->name);
            goto fail;
        }
        f->bitcode = d + bc + off;
        f->bitcode_len = (uint32_t)size;
        ++out->nfunctions;
    }
    return 0;
fail:
    mtllib_free(out);
    return -1;
}

void mtllib_free(mtllib *l) { free(l->functions); memset(l, 0, sizeof(*l)); }
