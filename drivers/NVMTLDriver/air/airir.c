#include "airir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* LLVM record codes used here (llvm/Bitcode/LLVMBitCodes.h) */
enum {
    MOD_VERSION = 1, MOD_GLOBALVAR = 7, MOD_FUNCTION = 8,
    TY_NUMENTRY = 1, TY_VOID = 2, TY_FLOAT = 3, TY_DOUBLE = 4, TY_LABEL = 5, TY_OPAQUE = 6,
    TY_INTEGER = 7, TY_POINTER = 8, TY_HALF = 10, TY_ARRAY = 11, TY_VECTOR = 12,
    TY_METADATA = 16, TY_STRUCT_ANON = 18, TY_STRUCT_NAME = 19, TY_STRUCT_NAMED = 20,
    TY_FUNCTION = 21, TY_BFLOAT = 23, TY_OPAQUE_POINTER = 25,
    CST_SETTYPE = 1, CST_NULL = 2, CST_UNDEF = 3, CST_INTEGER = 4, CST_WIDE_INTEGER = 5,
    CST_FLOAT = 6, CST_AGGREGATE = 7, CST_STRING = 8, CST_CSTRING = 9, CST_CE_BINOP = 10,
    CST_CE_CAST = 11, CST_CE_GEP = 12, CST_CE_INBOUNDS_GEP = 20, CST_DATA = 22, CST_POISON = 26,
    FN_DECLAREBLOCKS = 1, FN_BINOP = 2, FN_CAST = 3, FN_EXTRACTELT = 6, FN_INSERTELT = 7,
    FN_SHUFFLEVEC = 8, FN_RET = 10, FN_BR = 11, FN_SWITCH = 12, FN_UNREACHABLE = 15,
    FN_PHI = 16, FN_ALLOCA = 19, FN_LOAD = 20, FN_EXTRACTVAL = 26, FN_INSERTVAL = 27,
    FN_CMP2 = 28, FN_VSELECT = 29, FN_CALL = 34, FN_FENCE = 36, FN_ATOMICRMW_OLD = 38,
    FN_GEP = 43, FN_STORE = 44, FN_CMPXCHG = 46, FN_UNOP = 56, FN_FREEZE = 58, FN_ATOMICRMW = 59,
    MD_STRING_OLD = 1, MD_VALUE = 2, MD_NODE = 3, MD_NAME = 4, MD_DISTINCT_NODE = 5,
    MD_KIND = 6, MD_NAMED_NODE = 10, MD_ATTACHMENT = 11, MD_STRINGS = 35,
    MD_GLOBAL_DECL_ATTACHMENT = 36, MD_INDEX_OFFSET = 38, MD_INDEX = 39,
};

#define FAIL(...) do { snprintf(err, errlen, __VA_ARGS__); return -1; } while (0)

static char *dupn(const void *p, size_t n) {
    char *s = malloc(n + 1);
    memcpy(s, p, n);
    s[n] = 0;
    return s;
}

static char *ops_str(const bc_record *r, uint32_t from) {
    char *s = malloc(r->nops - from + 1);
    for (uint32_t i = from; i < r->nops; ++i) s[i - from] = (char)r->ops[i];
    s[r->nops - from] = 0;
    return s;
}

static uint32_t add_type(air_module *m, air_type t) {
    m->types = realloc(m->types, (m->ntypes + 1) * sizeof(air_type));
    m->types[m->ntypes] = t;
    return m->ntypes++;
}

uint32_t air_type_ptr(air_module *m, uint32_t elem, uint32_t as);
uint32_t air_type_ptr(air_module *m, uint32_t elem, uint32_t as) {
    for (uint32_t i = 0; i < m->ntypes; ++i)
        if (m->types[i].kind == AT_PTR && m->types[i].elem == elem && m->types[i].addrspace == as)
            return i;
    air_type t = {0};
    t.kind = AT_PTR; t.elem = elem; t.addrspace = as;
    return add_type(m, t);
}

uint32_t air_type_vec(air_module *m, uint32_t elem, uint32_t n);
uint32_t air_type_vec(air_module *m, uint32_t elem, uint32_t n) {
    for (uint32_t i = 0; i < m->ntypes; ++i)
        if (m->types[i].kind == AT_VEC && m->types[i].elem == elem && m->types[i].count == n) return i;
    air_type t = {0};
    t.kind = AT_VEC; t.elem = elem; t.count = n;
    return add_type(m, t);
}

uint32_t air_type_int(air_module *m, uint32_t bits);
uint32_t air_type_int(air_module *m, uint32_t bits) {
    for (uint32_t i = 0; i < m->ntypes; ++i)
        if (m->types[i].kind == AT_INT && m->types[i].bits == bits) return i;
    air_type t = {0};
    t.kind = AT_INT; t.bits = bits;
    return add_type(m, t);
}

static int read_types(air_module *m, const bc_block *b, char *err, size_t errlen) {
    char *pending_name = NULL;
    for (uint32_t i = 0; i < b->nrecords; ++i) {
        const bc_record *r = &b->records[i];
        air_type t = {0};
        switch (r->code) {
        case TY_NUMENTRY: continue;
        case TY_STRUCT_NAME: free(pending_name); pending_name = ops_str(r, 0); continue;
        case TY_VOID: t.kind = AT_VOID; break;
        case TY_HALF: t.kind = AT_FLOAT; t.bits = 16; break;
        case TY_BFLOAT: t.kind = AT_OTHER; t.bits = 16; break;
        case TY_FLOAT: t.kind = AT_FLOAT; t.bits = 32; break;
        case TY_DOUBLE: t.kind = AT_FLOAT; t.bits = 64; break;
        case TY_LABEL: t.kind = AT_LABEL; break;
        case TY_METADATA: t.kind = AT_METADATA; break;
        case TY_OPAQUE: t.kind = AT_OPAQUE; t.name = pending_name; pending_name = NULL; break;
        case TY_INTEGER: t.kind = AT_INT; t.bits = (uint32_t)r->ops[0]; break;
        case TY_POINTER:
            t.kind = AT_PTR; t.elem = (uint32_t)r->ops[0];
            t.addrspace = r->nops > 1 ? (uint32_t)r->ops[1] : 0;
            break;
        case TY_OPAQUE_POINTER:
            t.kind = AT_PTR; t.elem = ~0u; t.addrspace = r->nops ? (uint32_t)r->ops[0] : 0;
            break;
        case TY_ARRAY: case TY_VECTOR:
            t.kind = r->code == TY_ARRAY ? AT_ARRAY : AT_VEC;
            t.count = (uint32_t)r->ops[0]; t.elem = (uint32_t)r->ops[1];
            break;
        case TY_STRUCT_ANON: case TY_STRUCT_NAMED:
            t.kind = AT_STRUCT; t.packed = r->nops ? (int)r->ops[0] : 0;
            t.count = r->nops ? r->nops - 1 : 0;
            t.members = calloc(t.count + 1, 4);
            for (uint32_t k = 0; k < t.count; ++k) t.members[k] = (uint32_t)r->ops[k + 1];
            if (r->code == TY_STRUCT_NAMED) { t.name = pending_name; pending_name = NULL; }
            break;
        case TY_FUNCTION:
            t.kind = AT_FUNC; t.vararg = (int)r->ops[0]; t.elem = (uint32_t)r->ops[1];
            t.count = r->nops - 2;
            t.members = calloc(t.count + 1, 4);
            for (uint32_t k = 0; k < t.count; ++k) t.members[k] = (uint32_t)r->ops[k + 2];
            break;
        default:
            FAIL("unknown type record %u", r->code);
        }
        add_type(m, t);
    }
    free(pending_name);
    return 0;
}

static uint32_t push_value(air_value **vals, uint32_t *n, air_value v) {
    *vals = realloc(*vals, (*n + 1) * sizeof(air_value));
    (*vals)[*n] = v;
    return (*n)++;
}

static int64_t sext_vbr(uint64_t v) { return (v & 1) ? -(int64_t)(v >> 1) : (int64_t)(v >> 1); }

/* a CONSTANTS block, appending to vals */
static int read_constants(air_module *m, const bc_block *b, air_value **vals, uint32_t *n,
                          char *err, size_t errlen) {
    uint32_t ty = 0;
    for (uint32_t i = 0; i < b->nrecords; ++i) {
        const bc_record *r = &b->records[i];
        air_value v = {0};
        v.type = ty;
        switch (r->code) {
        case CST_SETTYPE: ty = (uint32_t)r->ops[0]; continue;
        case CST_NULL: v.kind = AV_CNULL; break;
        case CST_UNDEF: case CST_POISON: v.kind = AV_CUNDEF; break;
        case CST_INTEGER: v.kind = AV_CINT; v.ival = (uint64_t)sext_vbr(r->ops[0]); break;
        case CST_WIDE_INTEGER: v.kind = AV_CINT; v.ival = (uint64_t)sext_vbr(r->ops[0]); break;
        case CST_FLOAT: v.kind = AV_CFP; v.ival = r->ops[0]; break;
        case CST_AGGREGATE:
            v.kind = AV_CAGG; v.n = r->nops; v.elts = calloc(r->nops + 1, 4);
            for (uint32_t k = 0; k < r->nops; ++k) v.elts[k] = (uint32_t)r->ops[k];
            break;
        case CST_STRING: case CST_CSTRING: case CST_DATA:
            v.kind = AV_CDATA; v.n = r->nops + (r->code == CST_CSTRING);
            v.data = calloc(v.n + 1, 8);
            for (uint32_t k = 0; k < r->nops; ++k) v.data[k] = r->ops[k];
            break;
        case CST_CE_CAST:
            v.kind = AV_CEXPR; v.sub = 1; v.sub2 = (uint32_t)r->ops[0];
            v.n = 1; v.elts = calloc(2, 4); v.elts[0] = (uint32_t)r->ops[2];
            break;
        case CST_CE_BINOP:
            v.kind = AV_CEXPR; v.sub = 2; v.sub2 = (uint32_t)r->ops[0];
            v.n = 2; v.elts = calloc(3, 4);
            v.elts[0] = (uint32_t)r->ops[1]; v.elts[1] = (uint32_t)r->ops[2];
            break;
        case CST_CE_GEP: case CST_CE_INBOUNDS_GEP: {
            /* [pointee type, (type, value)...] (odd count), older without pointee */
            uint32_t k = r->nops & 1 ? 1 : 0;
            v.kind = AV_CEXPR; v.sub = 3; v.sub2 = k ? (uint32_t)r->ops[0] : ~0u;
            v.n = (r->nops - k) / 2; v.elts = calloc(v.n + 1, 4);
            for (uint32_t e = 0; e < v.n; ++e) v.elts[e] = (uint32_t)r->ops[k + e * 2 + 1];
            break;
        }
        default:
            v.kind = AV_BAD; v.sub = r->code;
            break;
        }
        push_value(vals, n, v);
    }
    (void)m; (void)err; (void)errlen;
    return 0;
}

/* ---- function bodies ---- */
typedef struct {
    air_module *m;
    air_function *f;
    const bc_record *r;
    uint32_t i;                 /* next operand */
    int bad;
} fnrd;

static uint64_t op(fnrd *x) {
    if (x->i >= x->r->nops) { x->bad = 1; return 0; }
    return x->r->ops[x->i++];
}

static void ensure_value(fnrd *x, uint32_t id, uint32_t type) {
    air_function *f = x->f;
    if (id < f->nvalues) {
        if (f->values[id].kind == AV_BAD && type != ~0u) f->values[id].type = type;
        return;
    }
    /* forward reference: placeholder until defined */
    while (f->nvalues <= id) {
        air_value v = {0};
        v.kind = AV_BAD; v.type = ~0u;
        push_value(&f->values, &f->nvalues, v);
    }
    f->values[id].type = type;
}

/* the instruction number used for relative operands is the next id to
 * define; forward placeholders do not advance it */
typedef struct { uint32_t inst_num; } fnstate;

static uint32_t get_vt(fnrd *x, fnstate *s, uint32_t *type) {
    const uint32_t rel = (uint32_t)op(x);
    const uint32_t v = s->inst_num - rel;
    if (v >= s->inst_num) {                 /* forward: explicit type follows */
        const uint32_t t = (uint32_t)op(x);
        ensure_value(x, v, t);
        if (type) *type = t;
    } else if (type) *type = x->f->values[v].type;
    return v;
}

static uint32_t get_v(fnrd *x, fnstate *s, uint32_t type) {
    const uint32_t v = s->inst_num - (uint32_t)op(x);
    if (v >= s->inst_num) ensure_value(x, v, type);
    return v;
}

static uint32_t value_type(fnrd *x, uint32_t v) {
    return v < x->f->nvalues ? x->f->values[v].type : ~0u;
}

static air_inst *new_inst(air_function *f, air_op o, uint32_t bb) {
    f->insts = realloc(f->insts, (f->ninsts + 1) * sizeof(air_inst));
    air_inst *in = &f->insts[f->ninsts++];
    memset(in, 0, sizeof(*in));
    in->op = o; in->bb = bb; in->value = ~0u; in->type = ~0u;
    return in;
}

static void set_ops(air_inst *in, uint32_t n, const uint32_t *v) {
    in->ops = calloc(n + 1, 4);
    in->nops = n;
    if (n) memcpy(in->ops, v, n * 4);
}

static uint32_t gep_result(air_module *m, uint32_t srcty, uint32_t ptrty, const uint32_t *idx,
                           uint32_t n, const air_value *vals) {
    /* ops[0] is the pointer, ops[1] steps over it, ops[2..] go into srcty */
    uint32_t t = srcty;
    for (uint32_t k = 2; k < n; ++k) {
        const air_type *tt = &m->types[t];
        if (tt->kind == AT_STRUCT) {
            const uint32_t fi = (uint32_t)vals[idx[k]].ival;
            t = fi < tt->count ? tt->members[fi] : 0;
        } else t = tt->elem;
    }
    const uint32_t as = m->types[ptrty].kind == AT_PTR ? m->types[ptrty].addrspace : 0;
    return air_type_ptr(m, t, as);
}

static int read_function(air_module *m, air_function *f, const bc_block *b, char *err, size_t errlen) {
    /* module values, then arguments */
    f->nvalues = 0;
    f->values = NULL;
    for (uint32_t i = 0; i < m->nvalues; ++i) push_value(&f->values, &f->nvalues, m->values[i]);
    const air_type *ft = &m->types[f->type];
    f->first_arg = f->nvalues;
    f->nargs = ft->count;
    for (uint32_t i = 0; i < ft->count; ++i) {
        air_value v = {0};
        v.kind = AV_ARG; v.type = ft->members[i]; v.ival = i;
        push_value(&f->values, &f->nvalues, v);
    }
    fnstate s = {f->nvalues};
    uint32_t bb = 0;
    for (uint32_t oi = 0; oi < b->norder; ++oi) {
        const int32_t o = b->order[oi];
        if (o < 0) {
            const bc_block *sub = &b->blocks[-o - 1];
            if (sub->id == BC_CONSTANTS) {
                if (read_constants(m, sub, &f->values, &f->nvalues, err, errlen)) return -1;
                s.inst_num = f->nvalues;
            }
            continue;
        }
        const bc_record *r = &b->records[o];
        fnrd x = {m, f, r, 0, 0};
        air_inst *in = NULL;
        uint32_t t0 = 0, t1 = 0, v[8];
        switch (r->code) {
        case FN_DECLAREBLOCKS: f->nblocks = (uint32_t)r->ops[0]; continue;
        case FN_BINOP: {
            v[0] = get_vt(&x, &s, &t0);
            v[1] = get_v(&x, &s, t0);
            in = new_inst(f, AI_BINOP, bb);
            in->sub = (uint32_t)op(&x);
            if (x.i < r->nops) in->flags = (uint32_t)op(&x);
            in->type = t0;
            set_ops(in, 2, v);
            break;
        }
        case FN_UNOP:
            v[0] = get_vt(&x, &s, &t0);
            in = new_inst(f, AI_UNOP, bb);
            in->sub = (uint32_t)op(&x);
            in->type = t0;
            set_ops(in, 1, v);
            break;
        case FN_FREEZE:                                /* no poison here: a plain copy */
            v[0] = get_vt(&x, &s, &t0);
            in = new_inst(f, AI_FREEZE, bb);
            in->type = t0;
            set_ops(in, 1, v);
            break;
        case FN_CAST:
            v[0] = get_vt(&x, &s, &t0);
            in = new_inst(f, AI_CAST, bb);
            in->type = (uint32_t)op(&x);
            in->sub = (uint32_t)op(&x);
            set_ops(in, 1, v);
            break;
        case FN_GEP: {
            in = new_inst(f, AI_GEP, bb);
            in->flags = (uint32_t)op(&x);            /* inbounds */
            const uint32_t src = (uint32_t)op(&x);
            uint32_t tmp[64], n = 0;
            while (x.i < r->nops && n < 64) tmp[n++] = get_vt(&x, &s, NULL);
            set_ops(in, n, tmp);
            in->callee_type = src;                    /* source element type */
            in->type = gep_result(m, src, value_type(&x, tmp[0]), tmp, n, f->values);
            break;
        }
        case FN_VSELECT:
            v[1] = get_vt(&x, &s, &t0);              /* true value */
            v[2] = get_v(&x, &s, t0);                 /* false value */
            v[0] = get_vt(&x, &s, NULL);              /* condition */
            in = new_inst(f, AI_SELECT, bb);
            in->type = t0;
            set_ops(in, 3, v);
            break;
        case FN_EXTRACTELT:
            v[0] = get_vt(&x, &s, &t0);
            v[1] = get_vt(&x, &s, NULL);
            in = new_inst(f, AI_EXTRACTELT, bb);
            in->type = m->types[t0].elem;
            set_ops(in, 2, v);
            break;
        case FN_INSERTELT:
            v[0] = get_vt(&x, &s, &t0);
            v[1] = get_v(&x, &s, m->types[t0].elem);
            v[2] = get_vt(&x, &s, NULL);
            in = new_inst(f, AI_INSERTELT, bb);
            in->type = t0;
            set_ops(in, 3, v);
            break;
        case FN_SHUFFLEVEC: {
            v[0] = get_vt(&x, &s, &t0);
            v[1] = get_v(&x, &s, t0);
            v[2] = get_vt(&x, &s, &t1);
            in = new_inst(f, AI_SHUFFLE, bb);
            in->type = air_type_vec(m, m->types[t0].elem, m->types[t1].count);
            set_ops(in, 3, v);
            break;
        }
        case FN_CMP2: {
            v[0] = get_vt(&x, &s, &t0);
            v[1] = get_v(&x, &s, t0);
            in = new_inst(f, AI_CMP, bb);
            in->sub = (uint32_t)op(&x);
            if (x.i < r->nops) in->flags = (uint32_t)op(&x);
            const uint32_t i1 = air_type_int(m, 1);
            in->type = m->types[t0].kind == AT_VEC ? air_type_vec(m, i1, m->types[t0].count) : i1;
            set_ops(in, 2, v);
            break;
        }
        case FN_RET:
            in = new_inst(f, AI_RET, bb);
            if (r->nops) { v[0] = get_vt(&x, &s, NULL); set_ops(in, 1, v); }
            ++bb;
            break;
        case FN_BR:
            in = new_inst(f, AI_BR, bb);
            if (r->nops == 1) {
                in->blocks = calloc(2, 4); in->nblocks = 1; in->blocks[0] = (uint32_t)r->ops[0];
            } else {
                in->blocks = calloc(3, 4); in->nblocks = 2;
                in->blocks[0] = (uint32_t)r->ops[0]; in->blocks[1] = (uint32_t)r->ops[1];
                x.i = 2;
                v[0] = get_v(&x, &s, air_type_int(m, 1));
                set_ops(in, 1, v);
            }
            ++bb;
            break;
        case FN_SWITCH: {
            in = new_inst(f, AI_SWITCH, bb);
            const uint32_t ty = (uint32_t)op(&x);
            v[0] = get_v(&x, &s, ty);
            const uint32_t ncase = (r->nops - 3) / 2;
            in->nblocks = ncase + 1;
            in->blocks = calloc(ncase + 2, 4);
            in->blocks[0] = (uint32_t)op(&x);         /* default */
            uint32_t *ops = calloc(ncase + 2, 4);
            ops[0] = v[0];
            for (uint32_t k = 0; k < ncase; ++k) {
                ops[k + 1] = (uint32_t)op(&x);         /* case value: absolute id */
                in->blocks[k + 1] = (uint32_t)op(&x);
            }
            in->ops = ops; in->nops = ncase + 1;
            ++bb;
            break;
        }
        case FN_UNREACHABLE:
            new_inst(f, AI_UNREACHABLE, bb);
            ++bb;
            continue;
        case FN_PHI: {
            in = new_inst(f, AI_PHI, bb);
            in->type = (uint32_t)op(&x);
            const uint32_t n = (r->nops - 1) / 2;
            in->ops = calloc(n + 1, 4); in->nops = n;
            in->blocks = calloc(n + 1, 4); in->nblocks = n;
            for (uint32_t k = 0; k < n; ++k) {
                const int64_t rel = sext_vbr(op(&x));
                const uint32_t id = (uint32_t)((int64_t)s.inst_num - rel);
                if (id >= s.inst_num) ensure_value(&x, id, in->type);
                in->ops[k] = id;
                in->blocks[k] = (uint32_t)op(&x);
            }
            break;
        }
        case FN_ALLOCA: {
            in = new_inst(f, AI_ALLOCA, bb);
            const uint32_t aty = (uint32_t)op(&x);
            op(&x);                                    /* size operand type */
            v[0] = (uint32_t)op(&x);                   /* size: absolute */
            in->align = (uint32_t)op(&x);
            in->callee_type = aty;
            in->type = air_type_ptr(m, aty, 0);
            set_ops(in, 1, v);
            break;
        }
        case FN_LOAD:
            v[0] = get_vt(&x, &s, NULL);
            in = new_inst(f, AI_LOAD, bb);
            in->type = (uint32_t)op(&x);
            in->align = (uint32_t)op(&x);
            in->flags = (uint32_t)op(&x);
            set_ops(in, 1, v);
            break;
        case FN_STORE:
            v[0] = get_vt(&x, &s, NULL);               /* pointer */
            v[1] = get_vt(&x, &s, NULL);               /* value */
            in = new_inst(f, AI_STORE, bb);
            in->align = (uint32_t)op(&x);
            in->flags = (uint32_t)op(&x);
            set_ops(in, 2, v);
            break;
        case FN_EXTRACTVAL: case FN_INSERTVAL: {
            const int ins = r->code == FN_INSERTVAL;
            v[0] = get_vt(&x, &s, &t0);
            if (ins) v[1] = get_vt(&x, &s, NULL);
            in = new_inst(f, ins ? AI_INSERTVAL : AI_EXTRACTVAL, bb);
            in->nidx = r->nops - x.i;
            in->idx = calloc(in->nidx + 1, 4);
            uint32_t t = t0;
            for (uint32_t k = 0; k < in->nidx; ++k) {
                in->idx[k] = (uint32_t)op(&x);
                const air_type *tt = &m->types[t];
                t = tt->kind == AT_STRUCT ? tt->members[in->idx[k]] : tt->elem;
            }
            in->type = ins ? t0 : t;
            set_ops(in, ins ? 2 : 1, v);
            break;
        }
        case FN_CALL: {
            op(&x);                                    /* paramattrs */
            const uint32_t cc = (uint32_t)op(&x);
            if (cc & (1u << 17)) op(&x);               /* fast-math flags */
            uint32_t fty = ~0u;
            if (cc & (1u << 15)) fty = (uint32_t)op(&x);
            uint32_t cty = 0;
            const uint32_t callee = get_vt(&x, &s, &cty);
            if (fty == ~0u) fty = m->types[cty].elem;
            const air_type *ftp = &m->types[fty];
            uint32_t tmp[64], n = 0;
            for (uint32_t k = 0; k < ftp->count && n < 63; ++k) tmp[n++] = get_v(&x, &s, ftp->members[k]);
            while (x.i < r->nops && n < 63) tmp[n++] = get_vt(&x, &s, NULL);
            in = new_inst(f, AI_CALL, bb);
            in->callee_type = fty;
            in->type = ftp->elem;
            in->nops = n + 1;
            in->ops = calloc(n + 2, 4);
            in->ops[0] = callee;
            memcpy(in->ops + 1, tmp, n * 4);
            break;
        }
        case FN_ATOMICRMW: case FN_ATOMICRMW_OLD:
            v[0] = get_vt(&x, &s, NULL);
            if (r->code == FN_ATOMICRMW) v[1] = get_vt(&x, &s, &t1);
            else { t1 = m->types[value_type(&x, v[0])].elem; v[1] = get_v(&x, &s, t1); }
            in = new_inst(f, AI_ATOMICRMW, bb);
            in->sub = (uint32_t)op(&x);
            in->type = t1;
            set_ops(in, 2, v);
            break;
        case FN_CMPXCHG:
            v[0] = get_vt(&x, &s, NULL);
            v[1] = get_vt(&x, &s, &t1);
            v[2] = get_v(&x, &s, t1);
            in = new_inst(f, AI_CMPXCHG, bb);
            in->type = t1;                             /* we only model the loaded value */
            set_ops(in, 3, v);
            break;
        case FN_FENCE:
            in = new_inst(f, AI_FENCE, bb);
            in->flags = (uint32_t)r->ops[0];
            continue;
        case 33: case 35: continue;                    /* debug locations */
        default:
            FAIL("%s: unsupported instruction record %u", f->name, r->code);
        }
        if (x.bad) FAIL("%s: truncated record %u", f->name, r->code);
        if (in && in->type != ~0u && m->types[in->type].kind != AT_VOID) {
            /* defines the next value */
            const uint32_t id = s.inst_num;
            if (id < f->nvalues) {
                f->values[id].kind = AV_INST;
                f->values[id].type = in->type;
                f->values[id].inst = f->ninsts - 1;
            } else {
                air_value val = {0};
                val.kind = AV_INST; val.type = in->type; val.inst = f->ninsts - 1;
                push_value(&f->values, &f->nvalues, val);
            }
            in->value = id;
            ++s.inst_num;
        }
    }
    return 0;
}

/* ---- metadata ---- */
static void add_md(air_module *m, air_md d) {
    m->md = realloc(m->md, (m->nmd + 1) * sizeof(air_md));
    m->md[m->nmd++] = d;
}

static int read_metadata(air_module *m, const bc_block *b, char *err, size_t errlen) {
    char *name = NULL;
    for (uint32_t i = 0; i < b->nrecords; ++i) {
        const bc_record *r = &b->records[i];
        air_md d = {0};
        switch (r->code) {
        case MD_STRINGS: {
            const uint32_t count = (uint32_t)r->ops[0], off = (uint32_t)r->ops[1];
            if (!r->blob || off > r->blob_len) FAIL("bad metadata strings");
            /* lengths: vbr6 in a little bitstream at the start of the blob */
            size_t pos = 0, cpos = off;
            for (uint32_t k = 0; k < count; ++k) {
                uint64_t len = 0;
                for (unsigned shift = 0;; shift += 5) {
                    uint32_t c = 0;
                    for (unsigned bit = 0; bit < 6; ++bit, ++pos)
                        c |= (uint32_t)((r->blob[pos >> 3] >> (pos & 7)) & 1) << bit;
                    len |= (uint64_t)(c & 31) << shift;
                    if (!(c & 32)) break;
                }
                if (cpos + len > r->blob_len) FAIL("metadata string out of range");
                air_md s = {0};
                s.kind = AM_STRING;
                s.str = dupn(r->blob + cpos, len);
                cpos += len;
                add_md(m, s);
            }
            continue;
        }
        case MD_STRING_OLD: d.kind = AM_STRING; d.str = ops_str(r, 0); break;
        case MD_VALUE: d.kind = AM_VALUE; d.type = (uint32_t)r->ops[0]; d.value = (uint32_t)r->ops[1]; break;
        case MD_NODE: case MD_DISTINCT_NODE:
            d.kind = AM_NODE; d.n = r->nops; d.ops = calloc(r->nops + 1, 4);
            for (uint32_t k = 0; k < r->nops; ++k) d.ops[k] = (int32_t)r->ops[k] - 1;
            break;
        case MD_NAME: free(name); name = ops_str(r, 0); continue;
        case MD_NAMED_NODE: {
            m->named = realloc(m->named, (m->nnamed + 1) * sizeof(air_named_md));
            air_named_md *nm = &m->named[m->nnamed++];
            nm->name = name ? name : dupn("", 0);
            name = NULL;
            nm->n = r->nops;
            nm->ops = calloc(r->nops + 1, 4);
            for (uint32_t k = 0; k < r->nops; ++k) nm->ops[k] = (uint32_t)r->ops[k];
            continue;
        }
        case MD_KIND: case MD_ATTACHMENT: case MD_GLOBAL_DECL_ATTACHMENT:
        case MD_INDEX_OFFSET: case MD_INDEX:
            continue;
        default: d.kind = AM_OTHER; break;
        }
        add_md(m, d);
    }
    free(name);
    return 0;
}

int air_read(const uint8_t *data, size_t len, air_module *m, char *err, size_t errlen) {
    memset(m, 0, sizeof(*m));
    bc_block root;
    if (bc_parse(data, len, &root, err, errlen)) return -1;
    const bc_block *mod = bc_find(&root, BC_MODULE);
    const bc_block *strtab = bc_find(&root, BC_STRTAB);
    const uint8_t *st = NULL;
    uint32_t stlen = 0;
    if (strtab && strtab->nrecords && strtab->records[0].blob) {
        st = strtab->records[0].blob;
        stlen = strtab->records[0].blob_len;
    }
    int rc = -1;
    if (!mod) { snprintf(err, errlen, "no module block"); goto out; }
    const bc_block *types = bc_find(mod, BC_TYPE);
    if (!types || read_types(m, types, err, errlen)) {
        if (!types) snprintf(err, errlen, "no type table");
        goto out;
    }
    /* globals and functions, in record order */
    uint32_t *bodies = calloc(mod->nrecords + 1, 4), nbodies = 0;
    for (uint32_t i = 0; i < mod->nrecords; ++i) {
        const bc_record *r = &mod->records[i];
        if (r->code == MOD_VERSION) { m->version = (uint32_t)r->ops[0]; continue; }
        if (r->code != MOD_GLOBALVAR && r->code != MOD_FUNCTION) continue;
        if (m->version < 2 || !st) { snprintf(err, errlen, "old bitcode (no string table)"); free(bodies); goto out; }
        const uint32_t so = (uint32_t)r->ops[0], sn = (uint32_t)r->ops[1];
        char *nm = so + (uint64_t)sn <= stlen ? dupn(st + so, sn) : dupn("?", 1);
        air_value v = {0};
        if (r->code == MOD_GLOBALVAR) {
            air_global g = {0};
            g.name = nm;
            const uint32_t flags = (uint32_t)r->ops[3];
            g.is_const = flags & 1;
            if (flags & 2) {                           /* explicit type */
                g.value_type = (uint32_t)r->ops[2];
                g.addrspace = flags >> 2;
                g.type = air_type_ptr(m, g.value_type, g.addrspace);
            } else {
                g.type = (uint32_t)r->ops[2];
                g.value_type = m->types[g.type].elem;
                g.addrspace = m->types[g.type].addrspace;
            }
            g.init = (uint32_t)r->ops[4];
            m->globals = realloc(m->globals, (m->nglobals + 1) * sizeof(air_global));
            m->globals[m->nglobals++] = g;
            v.kind = AV_GLOBAL; v.type = g.type; v.ival = m->nglobals - 1;
        } else {
            air_function fn = {0};
            fn.name = nm;
            fn.type = (uint32_t)r->ops[2];
            if (m->types[fn.type].kind == AT_PTR) fn.type = m->types[fn.type].elem;
            fn.is_proto = (int)r->ops[4];
            fn.value = m->nvalues;
            m->functions = realloc(m->functions, (m->nfunctions + 1) * sizeof(air_function));
            m->functions[m->nfunctions] = fn;
            if (!fn.is_proto) bodies[nbodies++] = m->nfunctions;
            ++m->nfunctions;
            v.kind = AV_FUNCTION;
            v.type = air_type_ptr(m, fn.type, 0);
            v.ival = m->nfunctions - 1;
        }
        push_value(&m->values, &m->nvalues, v);
    }
    const bc_block *consts = bc_find(mod, BC_CONSTANTS);
    if (consts && read_constants(m, consts, &m->values, &m->nvalues, err, errlen)) { free(bodies); goto out; }
    for (uint32_t i = 0; i < mod->nblocks; ++i)
        if (mod->blocks[i].id == BC_METADATA && read_metadata(m, &mod->blocks[i], err, errlen)) {
            free(bodies);
            goto out;
        }
    uint32_t body = 0;
    for (uint32_t i = 0; i < mod->nblocks; ++i) {
        if (mod->blocks[i].id != BC_FUNCTION) continue;
        if (body >= nbodies) { snprintf(err, errlen, "more bodies than functions"); free(bodies); goto out; }
        if (read_function(m, &m->functions[bodies[body++]], &mod->blocks[i], err, errlen)) {
            free(bodies);
            goto out;
        }
    }
    free(bodies);
    rc = 0;
out:
    bc_free(&root);
    return rc;
}

static void free_value(air_value *v) { free(v->elts); free(v->data); }

void air_free(air_module *m) {
    for (uint32_t i = 0; i < m->ntypes; ++i) { free(m->types[i].members); free(m->types[i].name); }
    for (uint32_t i = 0; i < m->nvalues; ++i) free_value(&m->values[i]);
    for (uint32_t i = 0; i < m->nglobals; ++i) free(m->globals[i].name);
    for (uint32_t i = 0; i < m->nfunctions; ++i) {
        air_function *f = &m->functions[i];
        for (uint32_t k = 0; k < f->ninsts; ++k) {
            free(f->insts[k].ops); free(f->insts[k].blocks); free(f->insts[k].idx);
        }
        /* local values past the module ones own their arrays; module ones are shared */
        for (uint32_t k = m->nvalues; k < f->nvalues; ++k) free_value(&f->values[k]);
        free(f->insts); free(f->values); free(f->name);
    }
    for (uint32_t i = 0; i < m->nmd; ++i) { free(m->md[i].str); free(m->md[i].ops); }
    for (uint32_t i = 0; i < m->nnamed; ++i) { free(m->named[i].name); free(m->named[i].ops); }
    free(m->types); free(m->values); free(m->globals); free(m->functions); free(m->md); free(m->named);
    memset(m, 0, sizeof(*m));
}

const air_named_md *air_named(const air_module *m, const char *name) {
    for (uint32_t i = 0; i < m->nnamed; ++i) if (!strcmp(m->named[i].name, name)) return &m->named[i];
    return NULL;
}

air_function *air_function_by_value(air_module *m, uint32_t value) {
    for (uint32_t i = 0; i < m->nfunctions; ++i) if (m->functions[i].value == value) return &m->functions[i];
    return NULL;
}

const char *air_md_string(const air_module *m, int32_t id) {
    return id >= 0 && (uint32_t)id < m->nmd && m->md[id].kind == AM_STRING ? m->md[id].str : NULL;
}

int air_md_int(const air_module *m, int32_t id, int64_t *out) {
    if (id < 0 || (uint32_t)id >= m->nmd || m->md[id].kind != AM_VALUE) return -1;
    const uint32_t v = m->md[id].value;
    if (v >= m->nvalues) return -1;
    if (m->values[v].kind == AV_CNULL) { *out = 0; return 0; }   /* i32 0 is written as null */
    if (m->values[v].kind != AV_CINT) return -1;
    *out = (int64_t)m->values[v].ival;
    return 0;
}
