#include "bitstream.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const uint8_t *p; size_t bits, pos; int bad; } rd;

static uint64_t fixed(rd *r, unsigned n) {
    uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i) {
        if (r->pos >= r->bits) { r->bad = 1; return 0; }
        v |= (uint64_t)((r->p[r->pos >> 3] >> (r->pos & 7)) & 1) << i;
        ++r->pos;
    }
    return v;
}

static uint64_t vbr(rd *r, unsigned n) {
    const uint64_t hi = 1ull << (n - 1);
    uint64_t v = 0;
    for (unsigned shift = 0; shift < 64 && !r->bad; shift += n - 1) {
        const uint64_t c = fixed(r, n);
        v |= (c & (hi - 1)) << shift;
        if (!(c & hi)) return v;
    }
    r->bad = 1;
    return 0;
}

static void align32(rd *r) { r->pos = (r->pos + 31) & ~(size_t)31; }

enum { OP_LIT, OP_FIXED, OP_VBR, OP_ARRAY, OP_CHAR6, OP_BLOB };
typedef struct { uint8_t kind; uint64_t val; } abbrev_op;
typedef struct { uint32_t n; abbrev_op *ops; } abbrev;
typedef struct { abbrev *a; uint32_t n, cap; } abbrev_list;

/* abbreviations registered through BLOCKINFO, per block id */
typedef struct { uint32_t id; abbrev_list list; } info_entry;
typedef struct { info_entry *e; uint32_t n; } blockinfo;

static abbrev_list *info_for(blockinfo *bi, uint32_t id, int make) {
    for (uint32_t i = 0; i < bi->n; ++i) if (bi->e[i].id == id) return &bi->e[i].list;
    if (!make) return NULL;
    bi->e = realloc(bi->e, (bi->n + 1) * sizeof(*bi->e));
    memset(&bi->e[bi->n], 0, sizeof(info_entry));
    bi->e[bi->n].id = id;
    return &bi->e[bi->n++].list;
}

static void push_abbrev(abbrev_list *l, abbrev a) {
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 8; l->a = realloc(l->a, l->cap * sizeof(abbrev)); }
    l->a[l->n++] = a;
}

static int read_abbrev(rd *r, abbrev *out) {
    const uint32_t n = (uint32_t)vbr(r, 5);
    out->ops = calloc(n ? n : 1, sizeof(abbrev_op));
    out->n = 0;
    for (uint32_t i = 0; i < n && !r->bad; ++i) {
        abbrev_op op = {0, 0};
        if (fixed(r, 1)) { op.kind = OP_LIT; op.val = vbr(r, 8); }
        else {
            const unsigned e = (unsigned)fixed(r, 3);
            if (e == 1) { op.kind = OP_FIXED; op.val = vbr(r, 5); }
            else if (e == 2) { op.kind = OP_VBR; op.val = vbr(r, 5); }
            else if (e == 3) op.kind = OP_ARRAY;
            else if (e == 4) op.kind = OP_CHAR6;
            else if (e == 5) op.kind = OP_BLOB;
            else return -1;
        }
        out->ops[out->n++] = op;
    }
    return r->bad ? -1 : 0;
}

static uint64_t scalar(rd *r, const abbrev_op *op) {
    switch (op->kind) {
    case OP_LIT: return op->val;
    case OP_FIXED: return op->val ? fixed(r, (unsigned)op->val) : 0;
    case OP_VBR: return op->val ? vbr(r, (unsigned)op->val) : 0;
    case OP_CHAR6: {
        const uint64_t c = fixed(r, 6);
        return c < 26 ? 'a' + c : c < 52 ? 'A' + c - 26 : c < 62 ? '0' + c - 52 : c == 62 ? '.' : '_';
    }
    default: r->bad = 1; return 0;
    }
}

typedef struct { uint64_t *v; uint32_t n, cap; } u64vec;
static void u64push(u64vec *v, uint64_t x) {
    if (v->n == v->cap) { v->cap = v->cap ? v->cap * 2 : 16; v->v = realloc(v->v, v->cap * 8); }
    v->v[v->n++] = x;
}

static int read_record(rd *r, uint32_t id, const abbrev *a, bc_record *rec) {
    u64vec ops = {0};
    memset(rec, 0, sizeof(*rec));
    if (!a) {                                        /* UNABBREV_RECORD */
        rec->code = (uint32_t)vbr(r, 6);
        const uint32_t n = (uint32_t)vbr(r, 6);
        for (uint32_t i = 0; i < n && !r->bad; ++i) u64push(&ops, vbr(r, 6));
    } else {
        int first = 1;
        for (uint32_t i = 0; i < a->n && !r->bad; ++i) {
            const abbrev_op *op = &a->ops[i];
            uint64_t v;
            if (op->kind == OP_ARRAY) {
                if (i + 1 >= a->n) return -1;
                const uint32_t n = (uint32_t)vbr(r, 6);
                const abbrev_op *elt = &a->ops[++i];
                for (uint32_t k = 0; k < n && !r->bad; ++k) {
                    v = scalar(r, elt);
                    if (first) { rec->code = (uint32_t)v; first = 0; } else u64push(&ops, v);
                }
                continue;
            }
            if (op->kind == OP_BLOB) {
                const uint32_t n = (uint32_t)vbr(r, 6);
                align32(r);
                if (r->pos + (size_t)n * 8 > r->bits) return -1;
                rec->blob = r->p + (r->pos >> 3);
                rec->blob_len = n;
                r->pos += (size_t)n * 8;
                align32(r);
                continue;
            }
            v = scalar(r, op);
            if (first) { rec->code = (uint32_t)v; first = 0; } else u64push(&ops, v);
        }
    }
    (void)id;
    rec->ops = ops.v;
    rec->nops = ops.n;
    return r->bad ? -1 : 0;
}

static void add_order(bc_block *b, int32_t v) {
    b->order = realloc(b->order, (b->norder + 1) * sizeof(int32_t));
    b->order[b->norder++] = v;
}

static int read_block(rd *r, bc_block *b, unsigned width, blockinfo *bi, int depth) {
    if (depth > 32) return -1;
    abbrev_list local = {0};
    abbrev_list *inherited = info_for(bi, b->id, 0);
    uint32_t info_target = ~0u;
    for (;;) {
        if (r->bad) return -1;
        const uint32_t code = (uint32_t)fixed(r, width);
        if (code == 0) {                             /* END_BLOCK */
            align32(r);
            for (uint32_t i = 0; i < local.n; ++i) free(local.a[i].ops);
            free(local.a);
            return 0;
        }
        if (code == 1) {                             /* ENTER_SUBBLOCK */
            const uint32_t id = (uint32_t)vbr(r, 8);
            const unsigned w = (unsigned)vbr(r, 4);
            align32(r);
            fixed(r, 32);                            /* length in words */
            b->blocks = realloc(b->blocks, (b->nblocks + 1) * sizeof(bc_block));
            bc_block *c = &b->blocks[b->nblocks];
            memset(c, 0, sizeof(*c));
            c->id = id;
            add_order(b, -(int32_t)b->nblocks - 1);
            ++b->nblocks;
            if (read_block(r, c, w, bi, depth + 1)) return -1;
            if (inherited) inherited = info_for(bi, b->id, 0);   /* BLOCKINFO may have grown it */
            continue;
        }
        if (code == 2) {                             /* DEFINE_ABBREV */
            abbrev a;
            if (read_abbrev(r, &a)) return -1;
            if (b->id == BC_BLOCKINFO) {
                if (info_target == ~0u) return -1;
                push_abbrev(info_for(bi, info_target, 1), a);
                inherited = info_for(bi, b->id, 0);
            } else push_abbrev(&local, a);
            continue;
        }
        const abbrev *a = NULL;
        if (code >= 4) {
            uint32_t idx = code - 4;
            const uint32_t ni = inherited ? inherited->n : 0;
            if (idx < ni) a = &inherited->a[idx];
            else if (idx - ni < local.n) a = &local.a[idx - ni];
            else return -1;
        }
        bc_record rec;
        if (read_record(r, b->id, a, &rec)) return -1;
        if (b->id == BC_BLOCKINFO) {
            if (rec.code == 1 && rec.nops) info_target = (uint32_t)rec.ops[0];   /* SETBID */
            free(rec.ops);
            continue;
        }
        b->records = realloc(b->records, (b->nrecords + 1) * sizeof(bc_record));
        b->records[b->nrecords] = rec;
        add_order(b, (int32_t)b->nrecords);
        ++b->nrecords;
    }
}

int bc_parse(const uint8_t *data, size_t len, bc_block *root, char *err, size_t errlen) {
    memset(root, 0, sizeof(*root));
    root->id = ~0u;
    if (len >= 20 && data[0] == 0xde && data[1] == 0xc0 && data[2] == 0x17 && data[3] == 0x0b) {
        const uint32_t off = (uint32_t)data[8] | (uint32_t)data[9] << 8 | (uint32_t)data[10] << 16 |
                             (uint32_t)data[11] << 24;
        const uint32_t size = (uint32_t)data[12] | (uint32_t)data[13] << 8 |
                              (uint32_t)data[14] << 16 | (uint32_t)data[15] << 24;
        if ((size_t)off + size > len) { snprintf(err, errlen, "wrapper out of range"); return -1; }
        data += off;
        len = size;
    }
    if (len < 4 || data[0] != 'B' || data[1] != 'C' || data[2] != 0xc0 || data[3] != 0xde) {
        snprintf(err, errlen, "not LLVM bitcode");
        return -1;
    }
    rd r = {data, len * 8, 32, 0};
    blockinfo bi = {0};
    int rc = 0;
    while (r.pos + 32 <= r.bits) {
        const uint32_t code = (uint32_t)fixed(&r, 2);
        if (code != 1) {                             /* only blocks at top level */
            if (code == 0 && r.pos + 30 > r.bits) break;
            snprintf(err, errlen, "unexpected top-level code %u at bit %zu", code, r.pos);
            rc = -1;
            break;
        }
        const uint32_t id = (uint32_t)vbr(&r, 8);
        const unsigned w = (unsigned)vbr(&r, 4);
        align32(&r);
        fixed(&r, 32);
        root->blocks = realloc(root->blocks, (root->nblocks + 1) * sizeof(bc_block));
        bc_block *c = &root->blocks[root->nblocks];
        memset(c, 0, sizeof(*c));
        c->id = id;
        add_order(root, -(int32_t)root->nblocks - 1);
        ++root->nblocks;
        if (read_block(&r, c, w, &bi, 0)) {
            snprintf(err, errlen, "malformed block %u near bit %zu", id, r.pos);
            rc = -1;
            break;
        }
    }
    for (uint32_t i = 0; i < bi.n; ++i) {
        for (uint32_t k = 0; k < bi.e[i].list.n; ++k) free(bi.e[i].list.a[k].ops);
        free(bi.e[i].list.a);
    }
    free(bi.e);
    return rc;
}

void bc_free(bc_block *b) {
    for (uint32_t i = 0; i < b->nrecords; ++i) free(b->records[i].ops);
    for (uint32_t i = 0; i < b->nblocks; ++i) bc_free(&b->blocks[i]);
    free(b->records);
    free(b->blocks);
    free(b->order);
    memset(b, 0, sizeof(*b));
}

const bc_block *bc_find(const bc_block *b, uint32_t id) {
    for (uint32_t i = 0; i < b->nblocks; ++i) if (b->blocks[i].id == id) return &b->blocks[i];
    return NULL;
}
