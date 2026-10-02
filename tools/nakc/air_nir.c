/* AIR -> NIR for compute kernels. See air_nir.h for the ABI.
 *
 * The LLVM CFG goes in as unstructured NIR (blocks + goto), the way vtn
 * handles OpenCL kernels, and nir_lower_goto_ifs structures it. Phis become
 * local variables stored at the end of each predecessor (vtn does the same)
 * and lower_vars_to_ssa rebuilds SSA. Memory is plain address arithmetic:
 *   device / constant  (addrspace 1, 2)  64-bit global addresses
 *   threadgroup        (addrspace 3)     32-bit shared memory offsets
 *   thread             (addrspace 0)     32-bit scratch offsets (allocas) */
#include "air_nir.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/nir/nir_builtin_builder.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct tval {
    nir_def *d;             /* scalar / vector / pointer */
    uint32_t n;             /* aggregate member count (d == NULL) */
    struct tval *m;
} tval;

typedef struct {
    air_module *m;
    air_function *f;
    nir_shader *s;
    nir_function_impl *impl;
    nir_builder b;
    tval *vals;             /* per function value id */
    uint8_t *have;          /* vals[i] computed */
    nir_block **blocks;     /* per LLVM basic block, NULL = unreachable */
    nir_instr **end_nop;    /* per LLVM block: phi stores go after it */
    nir_variable **phivar;  /* per instruction index */
    uint32_t *global_off;   /* per module global: shared or cdata offset */
    uint64_t *arg_align;    /* per function argument: known pointer alignment */
    air_abi *abi;
    const air_variant *var;
    nir_def *push_cdata;
    uint32_t tex_base, samp_base;   /* push byte offsets */
    int depth;                      /* inlining depth */
    mesa_shader_stage stage;
    int32_t outputs_node;           /* vertex/fragment: !air.* outputs list */
    const char *io_auto;            /* vertex stage without io=: its own output names */
    uint32_t buf_shift;             /* OpenCL kernels: buffers 0-2 are hidden arguments */
    /* threadgroup accesses checked against tg_limit, out of range ones go to
     * a 64-byte trash slot at tg_trash (see tg_guard_on) */
    uint32_t tg_guard, tg_limit, tg_trash;
    /* pull-model fragment inputs (interpolant<T>): the argument's value is
     * an index into this list, the air.interpolate_* calls read through it */
    nir_variable *interp_var[32];
    uint32_t ninterp;
    uint8_t *gused;                 /* globals this kernel reaches */
    int32_t *const_samp_slot;       /* per global: constexpr sampler slot, -1 none */
    /* the same for each linked module (link=NAME@PATH): their globals share
     * the kernel's constant data, threadgroup memory and sampler slots */
    uint32_t *link_goff[16];
    uint8_t *link_gused[16];
    int32_t *link_csamp[16];
    nir_def *sys_gid, *sys_lid, *sys_wgid, *sys_block, *sys_grid, *sys_lindex;
    nir_def *mesh_payload, *mesh_grid_addr, *mesh_out, *mesh_active;   /* mesh/object stages (as compute) */
    uint32_t scratch;       /* bytes of allocas */
    int32_t cp_node;        /* post-tessellation vertex: the patch_control_point_input arg node */
    nir_variable *cp_var[32];   /* its per-control-point inputs, by attribute location */
    char *err;
    size_t errlen;
    int failed;
} ctx;

static void fail(ctx *c, const char *fmt, ...) {
    if (c->failed) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->err, c->errlen, fmt, ap);
    va_end(ap);
    c->failed = 1;
}

/* ---- data layout (the AIR datalayout string: natural alignment, vectors
 * aligned to the next power of two of their size, 64-bit pointers) ---- */
static const air_type *T(ctx *c, uint32_t t) { return &c->m->types[t]; }

static uint32_t pow2ceil(uint32_t v) {
    uint32_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

static void layout(ctx *c, uint32_t t, uint32_t *size, uint32_t *align) {
    const air_type *x = T(c, t);
    switch (x->kind) {
    case AT_INT: *size = x->bits <= 8 ? 1 : x->bits / 8; *align = *size; return;
    case AT_FLOAT: *size = *align = x->bits / 8; return;
    case AT_PTR: *size = *align = 8; return;
    case AT_VEC: {
        uint32_t es, ea;
        layout(c, x->elem, &es, &ea);
        const uint32_t total = es * x->count;
        *align = pow2ceil(total);
        *size = (total + *align - 1) / *align * *align;
        return;
    }
    case AT_ARRAY: {
        uint32_t es, ea;
        layout(c, x->elem, &es, &ea);
        *size = es * x->count;
        *align = ea;
        return;
    }
    case AT_STRUCT: {
        uint32_t off = 0, al = 1;
        for (uint32_t i = 0; i < x->count; ++i) {
            uint32_t ms, ma;
            layout(c, x->members[i], &ms, &ma);
            if (x->packed) ma = 1;
            off = (off + ma - 1) / ma * ma;
            off += ms;
            if (ma > al) al = ma;
        }
        *align = al;
        *size = (off + al - 1) / al * al;
        return;
    }
    default: *size = *align = 1; return;
    }
}

static uint32_t type_size(ctx *c, uint32_t t) { uint32_t s, a; layout(c, t, &s, &a); return s; }
static uint32_t type_align(ctx *c, uint32_t t) { uint32_t s, a; layout(c, t, &s, &a); return a; }

/* bytes a load/store of t touches (no tail padding): float3 = 12 */
static uint32_t store_size(ctx *c, uint32_t t) {
    const air_type *x = T(c, t);
    if (x->kind == AT_VEC) return type_size(c, x->elem) * x->count;
    return type_size(c, t);
}

static uint32_t member_offset(ctx *c, uint32_t st, uint32_t idx) {
    const air_type *x = T(c, st);
    uint32_t off = 0;
    for (uint32_t i = 0; i <= idx && i < x->count; ++i) {
        uint32_t ms, ma;
        layout(c, x->members[i], &ms, &ma);
        if (x->packed) ma = 1;
        off = (off + ma - 1) / ma * ma;
        if (i == idx) return off;
        off += ms;
    }
    return off;
}

/* NIR bit size and component count of a first-class type */
static int scalar_shape(ctx *c, uint32_t t, uint32_t *bits, uint32_t *comps) {
    const air_type *x = T(c, t);
    *comps = 1;
    /* simdgroup_matrix 8x8 storage (<64 x T>): each lane holds its 2 elements */
    if (x->kind == AT_VEC) { *comps = x->count == 64 ? 2 : x->count; x = T(c, x->elem); }
    switch (x->kind) {
    case AT_INT: *bits = x->bits; break;
    case AT_FLOAT: *bits = x->bits; break;
    case AT_PTR: *bits = (x->addrspace == 1 || x->addrspace == 2 || x->addrspace == 6) ? 64 : 32; break;
    default: return -1;
    }
    if (*bits != 1 && *bits != 8 && *bits != 16 && *bits != 32 && *bits != 64) return -1;
    return 0;
}

static int is_float(ctx *c, uint32_t t) {
    const air_type *x = T(c, t);
    if (x->kind == AT_VEC) x = T(c, x->elem);
    return x->kind == AT_FLOAT;
}

static uint32_t ptr_as(ctx *c, uint32_t t) {
    const air_type *x = T(c, t);
    if (x->kind == AT_VEC) x = T(c, x->elem);
    return x->kind == AT_PTR ? x->addrspace : ~0u;
}

static const struct glsl_type *glsl_for(uint32_t bits, uint32_t comps) {
    enum glsl_base_type bt = bits == 1 ? GLSL_TYPE_BOOL : bits == 8 ? GLSL_TYPE_UINT8 :
                             bits == 16 ? GLSL_TYPE_UINT16 : bits == 64 ? GLSL_TYPE_UINT64 : GLSL_TYPE_UINT;
    return glsl_vector_type(bt, comps);
}

/* ---- constants ---- */
static tval get(ctx *c, uint32_t v);

static nir_def *imm(ctx *c, uint64_t val, uint32_t bits) {
    if (bits == 1) return nir_imm_bool(&c->b, val & 1);
    return nir_imm_intN_t(&c->b, (int64_t)val, bits);
}

static tval zero_of(ctx *c, uint32_t t) {
    tval r = {0};
    const air_type *x = T(c, t);
    if (x->kind == AT_STRUCT || x->kind == AT_ARRAY) {
        r.n = x->kind == AT_STRUCT ? x->count : x->count;
        r.m = ralloc_array(c->s, tval, r.n ? r.n : 1);
        for (uint32_t i = 0; i < r.n; ++i)
            r.m[i] = zero_of(c, x->kind == AT_STRUCT ? x->members[i] : x->elem);
        return r;
    }
    uint32_t bits, comps;
    if (scalar_shape(c, t, &bits, &comps)) { fail(c, "zero of unsupported type"); return r; }
    nir_def *z = imm(c, 0, bits);
    if (comps > 1) {
        nir_def *e[16];
        for (uint32_t i = 0; i < comps; ++i) e[i] = z;
        z = nir_vec(&c->b, e, comps);
    }
    r.d = z;
    return r;
}

static nir_def *global_addr(ctx *c, uint32_t g);
static nir_def *push_u32(ctx *c, uint32_t off);

static tval const_value(ctx *c, const air_value *v, uint32_t id) {
    tval r = {0};
    const air_type *x = T(c, v->type);
    switch (v->kind) {
    case AV_CINT: case AV_CFP: {
        uint32_t bits, comps;
        if (scalar_shape(c, v->type, &bits, &comps)) { fail(c, "constant of odd type"); return r; }
        r.d = imm(c, v->ival, bits);
        return r;
    }
    case AV_CNULL: case AV_CUNDEF: return zero_of(c, v->type);
    case AV_CAGG: case AV_CDATA: {
        if (x->kind == AT_VEC) {
            uint32_t bits, comps;
            if (scalar_shape(c, v->type, &bits, &comps)) { fail(c, "vector constant"); return r; }
            nir_def *e[16];
            for (uint32_t i = 0; i < comps && i < 16; ++i)
                e[i] = v->kind == AV_CDATA ? imm(c, v->data[i], bits) : get(c, v->elts[i]).d;
            r.d = nir_vec(&c->b, e, comps);
            return r;
        }
        r.n = x->count;
        r.m = ralloc_array(c->s, tval, r.n ? r.n : 1);
        for (uint32_t i = 0; i < r.n; ++i) {
            if (v->kind == AV_CAGG) r.m[i] = get(c, v->elts[i]);
            else {
                uint32_t bits, comps;
                scalar_shape(c, x->elem, &bits, &comps);
                r.m[i].d = imm(c, v->data[i], bits);
            }
        }
        return r;
    }
    case AV_GLOBAL:
        if (c->const_samp_slot && c->const_samp_slot[v->ival] >= 0) {   /* constexpr sampler handed on */
            r.d = nir_u2u64(&c->b, push_u32(c, c->samp_base + (c->abi->nsamp + (uint32_t)c->const_samp_slot[v->ival]) * 4));
            return r;
        }
        r.d = global_addr(c, (uint32_t)v->ival);
        return r;
    case AV_CEXPR:
        if (v->sub == 1) {                       /* cast */
            tval a = get(c, v->elts[0]);
            uint32_t bits, comps;
            if (!a.d || scalar_shape(c, v->type, &bits, &comps)) { fail(c, "constant cast"); return r; }
            r.d = a.d->bit_size == bits ? a.d : nir_u2uN(&c->b, a.d, bits);
            return r;
        }
        if (v->sub == 3) {                       /* gep on a global */
            tval base = get(c, v->elts[0]);
            if (!base.d) { fail(c, "constant gep base"); return r; }
            uint32_t t = v->sub2 != ~0u ? v->sub2 : T(c, c->f->values[v->elts[0]].type)->elem;
            uint64_t off = 0;
            for (uint32_t i = 1; i < v->n; ++i) {
                const air_value *iv = &c->f->values[v->elts[i]];
                const int64_t k = (int64_t)iv->ival;
                if (i == 1) { off += (uint64_t)(k * (int64_t)type_size(c, t)); continue; }
                const air_type *tt = T(c, t);
                if (tt->kind == AT_STRUCT) { off += member_offset(c, t, (uint32_t)k); t = tt->members[k]; }
                else { t = tt->elem; off += (uint64_t)(k * (int64_t)type_size(c, t)); }
            }
            r.d = nir_iadd_imm(&c->b, base.d, (int64_t)off);
            return r;
        }
        fail(c, "constant expression kind %u", v->sub);
        return r;
    default:
        fail(c, "value %u of kind %u has no NIR form", id, v->kind);
        return r;
    }
}

static tval get(ctx *c, uint32_t v) {
    if (v >= c->f->nvalues) { fail(c, "value %u out of range", v); return (tval){0}; }
    if (c->have[v]) return c->vals[v];
    const air_value *x = &c->f->values[v];
    if (x->kind == AV_INST || x->kind == AV_ARG) {
        fail(c, "value %u used before it is defined", v);
        return (tval){0};
    }
    /* constants are rebuilt at each use, so they always dominate it */
    return const_value(c, x, v);
}

static nir_def *getd(ctx *c, uint32_t v) {
    tval t = get(c, v);
    if (!t.d && !c->failed) fail(c, "aggregate value %u used as a scalar", v);
    return t.d ? t.d : nir_imm_int(&c->b, 0);
}

static void set(ctx *c, uint32_t v, tval t) {
    c->vals[v] = t;
    c->have[v] = 1;
}

/* ---- globals: threadgroup ones live in shared memory, constant ones in
 * the cdata block the runtime uploads ---- */
static void write_init(ctx *c, uint8_t *dst, uint32_t t, uint32_t v) {
    const air_value *x = &c->m->values[v];
    const air_type *ty = T(c, t);
    if (x->kind == AV_CNULL || x->kind == AV_CUNDEF) return;
    if (x->kind == AV_CINT || x->kind == AV_CFP) {
        uint32_t n = store_size(c, t);
        for (uint32_t i = 0; i < n && i < 8; ++i) dst[i] = (uint8_t)(x->ival >> (8 * i));
        return;
    }
    if (x->kind == AV_CDATA) {
        uint32_t es = ty->kind == AT_VEC || ty->kind == AT_ARRAY ? type_size(c, ty->elem) : 1;
        for (uint32_t i = 0; i < x->n; ++i)
            for (uint32_t k = 0; k < es && k < 8; ++k) dst[i * es + k] = (uint8_t)(x->data[i] >> (8 * k));
        return;
    }
    if (x->kind == AV_CAGG) {
        for (uint32_t i = 0; i < x->n; ++i) {
            uint32_t mt, off;
            if (ty->kind == AT_STRUCT) { mt = ty->members[i]; off = member_offset(c, t, i); }
            else { mt = ty->elem; off = i * type_size(c, mt); }
            write_init(c, dst + off, mt, x->elts[i]);
        }
        return;
    }
    fail(c, "global initializer kind %u", x->kind);
}

/* index of a function-constant initializer global (..MTL_FC_INIT_<i>_<t>), -1 if not one */
static int fc_index(const char *name) {
    const char *p = strstr(name, "MTL_FC_INIT_");
    if (!p) return -1;
    const int i = atoi(p + 12);
    return i >= 0 && i < 128 ? i : -1;
}

/* globals the kernel reaches (directly or through constant expressions);
 * others (llvm.used, other entry points' tables) are never placed */
static void mark_value(ctx *c, uint32_t v, uint8_t *used, int depth) {
    if (v >= c->f->nvalues || depth > 16) return;
    const air_value *x = &c->f->values[v];
    if (x->kind == AV_GLOBAL) {
        if (!used[x->ival]) {
            used[x->ival] = 1;
            const air_global *g = &c->m->globals[x->ival];
            if (g->init) mark_value(c, g->init - 1, used, depth + 1);
        }
        return;
    }
    if (x->kind == AV_CEXPR || x->kind == AV_CAGG)
        for (uint32_t i = 0; i < x->n; ++i) mark_value(c, x->elts[i], used, depth + 1);
}

/* C++ dynamic initialisers (_GLOBAL__sub_I_<file>.metal). Metal emits one
 * when a file-scope constant is set from function constants, e.g.
 *   constant bool is_horizontal = is_horizontal_fc;
 * plus the implicit [[function_constant]] predicates of arguments. SkyLight's
 * window shadow blur has 50 of them (radius and rim state flags); Apple's
 * compiler folds the ctor at pipeline time. Never running it left every flag
 * at 0, so the shader took the general path: a (2r+1)^2 max filter per pixel
 * that kept GR busy for good right after login (28 Sep). The ctors are
 * straight-line code over constants, so evaluate them here, with this
 * pipeline's constants, and write what they store into cdata. */
static const char *callee_name(ctx *c, const air_inst *in);
static int ctor_val(ctx *c, const uint64_t *v, const uint8_t *have, uint32_t id, uint64_t *out) {
    if (id >= c->f->nvalues) return 0;
    const air_value *x = &c->f->values[id];
    switch (x->kind) {
    case AV_CINT: *out = x->ival; return 1;
    case AV_CNULL: case AV_CUNDEF: *out = 0; return 1;
    case AV_INST: if (!have[id]) return 0; *out = v[id]; return 1;
    default: return 0;
    }
}
static uint64_t ctor_mask(ctx *c, uint32_t t, uint64_t x) {
    const air_type *ty = T(c, t);
    if (ty->kind != AT_INT || ty->bits >= 64) return x;
    return x & ((1ull << ty->bits) - 1);
}
static void run_ctors(ctx *c) {
    air_function *kf = c->f;
    for (uint32_t fi = 0; fi < c->m->nfunctions; ++fi) {
        air_function *fn = &c->m->functions[fi];
        if (fn->is_proto || !fn->name || strncmp(fn->name, "_GLOBAL__sub_I", 14)) continue;
        c->f = fn;
        uint64_t *v = calloc(fn->nvalues + 1, 8);
        uint8_t *have = calloc(fn->nvalues + 1, 1);
        uint32_t stores = 0;
        const char *why = NULL;
        for (uint32_t i = 0; i < fn->ninsts && !why; ++i) {
            const air_inst *in = &fn->insts[i];
            uint64_t a = 0, b2 = 0, r = 0;
            switch (in->op) {
            case AI_LOAD: case AI_STORE: {
                const air_value *p = &fn->values[in->ops[0]];
                if (p->kind != AV_GLOBAL || !c->gused[p->ival]) { why = "pointer is not a placed global"; break; }
                const air_global *g = &c->m->globals[p->ival];
                if (g->addrspace == 3) { why = "threadgroup global"; break; }
                uint8_t *d = c->abi->cdata + c->global_off[p->ival];
                const uint32_t t = in->op == AI_LOAD ? in->type : fn->values[in->ops[1]].type;
                const uint32_t n = store_size(c, t);
                if (n > 8) { why = "wide value"; break; }
                if (in->op == AI_LOAD) {
                    for (uint32_t k = 0; k < n; ++k) r |= (uint64_t)d[k] << (8 * k);
                    break;
                }
                if (!ctor_val(c, v, have, in->ops[1], &a)) { why = "stored value unknown"; break; }
                for (uint32_t k = 0; k < n; ++k) d[k] = (uint8_t)(a >> (8 * k));
                stores++;
                continue;
            }
            case AI_CMP:
                if (!ctor_val(c, v, have, in->ops[0], &a) || !ctor_val(c, v, have, in->ops[1], &b2)) { why = "cmp operand"; break; }
                {
                    const uint32_t t = fn->values[in->ops[0]].type;
                    a = ctor_mask(c, t, a); b2 = ctor_mask(c, t, b2);
                    const uint32_t bits = T(c, t)->bits;
                    const int64_t sa = bits && bits < 64 ? (int64_t)(a << (64 - bits)) >> (64 - bits) : (int64_t)a;
                    const int64_t sb = bits && bits < 64 ? (int64_t)(b2 << (64 - bits)) >> (64 - bits) : (int64_t)b2;
                    switch (in->sub) {
                    case 32: r = a == b2; break; case 33: r = a != b2; break;
                    case 34: r = a > b2; break;  case 35: r = a >= b2; break;
                    case 36: r = a < b2; break;  case 37: r = a <= b2; break;
                    case 38: r = sa > sb; break; case 39: r = sa >= sb; break;
                    case 40: r = sa < sb; break; case 41: r = sa <= sb; break;
                    default: why = "cmp predicate";
                    }
                }
                break;
            case AI_SELECT: {
                uint64_t cond = 0;
                if (!ctor_val(c, v, have, in->ops[0], &cond) || !ctor_val(c, v, have, in->ops[1], &a) ||
                    !ctor_val(c, v, have, in->ops[2], &b2)) { why = "select operand"; break; }
                r = (cond & 1) ? a : b2;
                break;
            }
            case AI_BINOP:
                if (!ctor_val(c, v, have, in->ops[0], &a) || !ctor_val(c, v, have, in->ops[1], &b2)) { why = "binop operand"; break; }
                {
                    const air_type *bt = T(c, in->type);
                    if (bt->kind == AT_FLOAT && (bt->bits == 32 || bt->bits == 64)) {
                        /* LLVM float binops share the codes: 0 add, 1 sub, 2 mul, 4 div, 6 rem */
                        double x, y, z = 0;
                        if (bt->bits == 32) { float fx, fy; uint32_t ux = (uint32_t)a, uy = (uint32_t)b2;
                                              memcpy(&fx, &ux, 4); memcpy(&fy, &uy, 4); x = fx; y = fy; }
                        else { memcpy(&x, &a, 8); memcpy(&y, &b2, 8); }
                        switch (in->sub) {
                        case 0: z = x + y; break; case 1: z = x - y; break; case 2: z = x * y; break;
                        case 4: z = x / y; break; case 6: z = fmod(x, y); break;
                        default: why = "float binop kind";
                        }
                        if (bt->bits == 32) { float fz = (float)z; uint32_t uz; memcpy(&uz, &fz, 4); r = uz; }
                        else memcpy(&r, &z, 8);
                        break;
                    }
                    const uint32_t bits = bt->bits ? bt->bits : 64;
                    a = ctor_mask(c, in->type, a); b2 = ctor_mask(c, in->type, b2);
                    const int64_t sa = bits < 64 ? (int64_t)(a << (64 - bits)) >> (64 - bits) : (int64_t)a;
                    const int64_t sb = bits < 64 ? (int64_t)(b2 << (64 - bits)) >> (64 - bits) : (int64_t)b2;
                    const uint32_t sh = (uint32_t)(b2 & 63);
                    switch (in->sub) {
                    case 0: r = a + b2; break; case 1: r = a - b2; break; case 2: r = a * b2; break;
                    case 3: if (!b2) { why = "udiv by zero"; break; } r = a / b2; break;
                    case 4: if (!sb) { why = "sdiv by zero"; break; } r = (uint64_t)(sa / sb); break;
                    case 5: if (!b2) { why = "urem by zero"; break; } r = a % b2; break;
                    case 6: if (!sb) { why = "srem by zero"; break; } r = (uint64_t)(sa % sb); break;
                    case 7: r = sh < bits ? a << sh : 0; break;
                    case 8: r = sh < bits ? a >> sh : 0; break;
                    case 9: r = (uint64_t)(sh < bits ? sa >> sh : (sa < 0 ? -1 : 0)); break;
                    case 10: r = a & b2; break; case 11: r = a | b2; break; case 12: r = a ^ b2; break;
                    default: why = "binop kind";
                    }
                }
                break;
            case AI_CAST:
                if (!ctor_val(c, v, have, in->ops[0], &a)) { why = "cast operand"; break; }
                if (in->sub == 0 || in->sub == 1) r = ctor_mask(c, fn->values[in->ops[0]].type, a);
                else if (in->sub == 2) {
                    const uint32_t bits = T(c, fn->values[in->ops[0]].type)->bits;
                    r = bits && bits < 64 ? (uint64_t)((int64_t)(a << (64 - bits)) >> (64 - bits)) : a;
                } else why = "cast kind";
                break;
            case AI_CALL: {
                const char *name = callee_name(c, in);
                if (name && (!strncmp(name, "llvm.umax.", 10) || !strncmp(name, "llvm.umin.", 10) ||
                             !strncmp(name, "llvm.smax.", 10) || !strncmp(name, "llvm.smin.", 10))) {
                    if (!ctor_val(c, v, have, in->ops[1], &a) || !ctor_val(c, v, have, in->ops[2], &b2)) { why = "min/max operand"; break; }
                    const uint32_t t = fn->values[in->ops[1]].type, bits = T(c, t)->bits ? T(c, t)->bits : 64;
                    a = ctor_mask(c, t, a); b2 = ctor_mask(c, t, b2);
                    const int64_t sa = bits < 64 ? (int64_t)(a << (64 - bits)) >> (64 - bits) : (int64_t)a;
                    const int64_t sb = bits < 64 ? (int64_t)(b2 << (64 - bits)) >> (64 - bits) : (int64_t)b2;
                    if (name[5] == 'u') r = name[6] == 'm' && name[7] == 'a' ? (a > b2 ? a : b2) : (a < b2 ? a : b2);
                    else r = (uint64_t)(name[7] == 'a' ? (sa > sb ? sa : sb) : (sa < sb ? sa : sb));
                    break;
                }
                if (!name || strncmp(name, "air.normalize_function_constant_predicate", 41)) { why = "call"; break; }
                if (!ctor_val(c, v, have, in->ops[1], &a)) { why = "call operand"; break; }
                r = ctor_mask(c, fn->values[in->ops[1]].type, a) != 0;
                break;
            }
            case AI_RET: i = fn->ninsts; continue;
            default: why = "instruction kind";
            }
            if (why) break;
            if (in->value < fn->nvalues) { v[in->value] = ctor_mask(c, in->type, r); have[in->value] = 1; }
        }
        if (why) fprintf(stderr, "air: %s: stopped after %u stores (%s)\n", fn->name, stores, why);
        free(v); free(have);
    }
    c->f = kf;
}

/* offsets for the globals of the current module (c->m) that its functions
 * reach: constexpr samplers -> sampler slots, threadgroup -> tg, constant ->
 * cd (both running on from earlier modules) */
static void place_module_globals(ctx *c, uint32_t **goff_out, uint8_t **used_out, int32_t **csamp_out,
                                 uint32_t *tg_io, uint32_t *cd_io) {
    uint32_t tg = *tg_io, cd = *cd_io;
    uint32_t *goff = calloc(c->m->nglobals + 1, 4);
    uint8_t *used = calloc(c->m->nglobals + 1, 1);
    int32_t *csamp = calloc(c->m->nglobals + 1, 4);
    air_function *kf = c->f;
    for (uint32_t fi = 0; fi < c->m->nfunctions; ++fi) {   /* inlined callees count too */
        air_function *fn = &c->m->functions[fi];
        if (fn->is_proto) continue;
        c->f = fn;
        for (uint32_t i = 0; i < fn->ninsts; ++i)
            for (uint32_t k = 0; k < fn->insts[i].nops; ++k) mark_value(c, fn->insts[i].ops[k], used, 0);
    }
    c->f = kf;
    for (uint32_t i = 0; i < c->m->nglobals; ++i) {
        const air_global *g = &c->m->globals[i];
        csamp[i] = -1;
        if (!used[i]) continue;
        /* constexpr samplers become sampler slots, not constant data */
        if (!strncmp(g->name, "__air_sampler_state", 19) && g->init) {
            const air_value *iv = &c->m->values[g->init - 1];
            if (c->abi->nconst_samp >= 16) { fail(c, "more than 16 constexpr samplers"); continue; }
            csamp[i] = (int32_t)c->abi->nconst_samp;
            /* Sonoma's front end: one i64. Tahoe's (32023): an array of two
             * i64, the state in the first (read as 0 before, so every
             * constexpr sampler was nearest + clamp to zero) */
            uint64_t bits = 0;
            if (iv->kind == AV_CINT) bits = iv->ival;
            else if (iv->kind == AV_CDATA && iv->n) bits = iv->data[0];
            else if (iv->kind == AV_CAGG && iv->n && c->m->values[iv->elts[0]].kind == AV_CINT)
                bits = c->m->values[iv->elts[0]].ival;
            else fail(c, "constexpr sampler %s: initializer kind %d", g->name, (int)iv->kind);
            c->abi->const_samp[c->abi->nconst_samp++] = bits;
            continue;
        }
        uint32_t sz, a;
        layout(c, g->value_type, &sz, &a);
        if (a < 4) a = 4;
        if (g->addrspace == 3) { tg = (tg + a - 1) / a * a; goff[i] = tg; tg += sz; }
        else if (g->addrspace == 2 || g->addrspace == 1 || g->is_const) {
            cd = (cd + a - 1) / a * a; goff[i] = cd; cd += sz;
        }
    }
    *goff_out = goff; *used_out = used; *csamp_out = csamp;
    *tg_io = tg; *cd_io = cd;
}

/* constant data initial values of the current module's placed globals */
static void init_module_globals(ctx *c, const uint32_t *goff, const uint8_t *used, const int32_t *csamp) {
    for (uint32_t i = 0; i < c->m->nglobals; ++i) {
        const air_global *g = &c->m->globals[i];
        if (!used[i] || g->addrspace == 3 || csamp[i] >= 0) continue;
        const int fc = fc_index(g->name);
        if (fc >= 0) {                       /* function constant: the pipeline's value */
            if (c->var->fc_defined[fc / 32] & (1u << (fc % 32))) {
                const uint32_t n = store_size(c, g->value_type);
                for (uint32_t k = 0; k < n && k < 16; ++k)
                    c->abi->cdata[goff[i] + k] = (uint8_t)((k < 8 ? c->var->fc_value[fc]
                                                  : c->var->fc_value_hi[fc]) >> (8 * (k & 7)));
            }
            continue;
        }
        if (!g->init) continue;
        write_init(c, c->abi->cdata + goff[i], g->value_type, g->init - 1);
    }
}

static void place_globals(ctx *c) {
    uint32_t tg = 0, cd = 0;
    place_module_globals(c, &c->global_off, &c->gused, &c->const_samp_slot, &tg, &cd);
    /* 0.8.16: linked [[visible]] functions keep their own globals (RenderBox
     * effects in SecurityAgent sample through a constexpr sampler) */
    air_module *km = c->m;
    for (uint32_t li = 0; li < c->var->nlink && li < 16; ++li) {
        if (!c->var->link_mod[li]) continue;
        c->m = c->var->link_mod[li];
        place_module_globals(c, &c->link_goff[li], &c->link_gused[li], &c->link_csamp[li], &tg, &cd);
    }
    c->m = km;
    /* [[threadgroup(i)]] arguments follow the threadgroup globals */
    for (uint32_t i = 0; i < 32; ++i) {
        if (!c->var->tg_arg_bytes[i]) continue;
        tg = (tg + 15) & ~15u;
        c->abi->tg_arg_offset[i] = tg;
        tg += c->var->tg_arg_bytes[i];
    }
    c->abi->tg_bytes = tg;
    if (cd) {
        c->abi->cdata = calloc(cd, 1);
        c->abi->cdata_bytes = cd;
        init_module_globals(c, c->global_off, c->gused, c->const_samp_slot);
        for (uint32_t li = 0; li < c->var->nlink && li < 16; ++li) {
            if (!c->link_goff[li]) continue;
            c->m = c->var->link_mod[li];
            init_module_globals(c, c->link_goff[li], c->link_gused[li], c->link_csamp[li]);
        }
        c->m = km;
        run_ctors(c);
    }
}

static nir_def *global_addr(ctx *c, uint32_t g) {
    const air_global *gl = &c->m->globals[g];
    if (!c->global_off) { fail(c, "global %s in a linked function", gl->name); return nir_imm_int64(&c->b, 0); }
    if (gl->addrspace == 3) return nir_imm_int(&c->b, (int)c->global_off[g]);
    if (!c->push_cdata) { fail(c, "global %s outside constant/threadgroup space", gl->name); return nir_imm_int64(&c->b, 0); }
    return nir_iadd_imm(&c->b, c->push_cdata, c->global_off[g]);
}

/* ---- memory ---- */
/* A [[threadgroup(i)]] argument the encoder gave no length. VideoToolbox's
 * VTMTSComputeFunction1x1 still stores to it when it runs as a plain crop
 * copy (32x32 threadgroups, no setThreadgroupMemoryLength). On AMD, where
 * Intel Macs ran it, an LDS access past the allocation reads 0 and a write
 * there is dropped; NVIDIA raises an out-of-range SM warp error instead (Xid
 * 13, the GR channel dies, HEIC thumbnails came out black). So such kernels
 * get every shared access checked: out of range reads 0, writes and atomics
 * land in a trash slot past the real data. NAKC_TG_GUARD=1 does it for all. */
static uint32_t tg_guard_on(ctx *c) {
    if (!c->tg_guard) {
        c->tg_guard = 1;
        c->tg_limit = c->abi->tg_bytes;
        c->tg_trash = (c->tg_limit + 63) & ~63u;
        c->abi->tg_bytes = c->tg_trash + 64;
    }
    return c->tg_limit;
}

static nir_def *tg_addr(ctx *c, nir_def *addr, uint32_t bytes, nir_def **inb) {
    nir_builder *b = &c->b;
    *inb = bytes > c->tg_limit ? nir_imm_false(b) : nir_uge(b, nir_imm_int(b, (int)(c->tg_limit - bytes)), addr);
    return nir_bcsel(b, *inb, addr, nir_imm_int(b, (int)c->tg_trash));
}

static nir_def *mem_load(ctx *c, uint32_t as, nir_def *addr, uint32_t bits, uint32_t comps, uint32_t align) {
    const uint32_t mbits = bits == 1 ? 8 : bits;
    nir_def *v;
    if (align < 1) align = 1;
    if (as == 3 && c->tg_guard) {
        nir_def *inb, *a = tg_addr(c, addr, comps * mbits / 8, &inb);
        v = nir_load_shared(&c->b, comps, mbits, a, .base = 0, .align_mul = align);
        v = nir_bcsel(&c->b, nir_replicate(&c->b, inb, comps), v, nir_imm_zero(&c->b, comps, mbits));
        return bits == 1 ? nir_i2b(&c->b, v) : v;
    }
    switch (as) {
    case 1: case 2: case 6: v = nir_load_global(&c->b, comps, mbits, addr, .align_mul = align); break;
    case 3: v = nir_load_shared(&c->b, comps, mbits, addr, .base = 0, .align_mul = align); break;
    case 0: v = nir_load_scratch(&c->b, comps, mbits, addr, .align_mul = align); break;
    default: fail(c, "load from address space %u", as); return nir_imm_int(&c->b, 0);
    }
    return bits == 1 ? nir_i2b(&c->b, v) : v;
}

static void mem_store(ctx *c, uint32_t as, nir_def *addr, nir_def *val, uint32_t align) {
    if (val->bit_size == 1) val = nir_b2iN(&c->b, val, 8);
    if (align < 1) align = 1;
    switch (as) {
    case 1: case 2: case 6: nir_store_global(&c->b, val, addr, .align_mul = align); break;
    case 3:
        if (c->tg_guard) {
            nir_def *inb;
            addr = tg_addr(c, addr, val->num_components * val->bit_size / 8, &inb);
        }
        nir_store_shared(&c->b, val, addr, .base = 0, .align_mul = align); break;
    case 0: nir_store_scratch(&c->b, val, addr, .align_mul = align); break;
    default: fail(c, "store to address space %u", as);
    }
}

static uint32_t min_align(uint32_t a, uint32_t b) { return a < b ? a : b; }
static uint32_t off_align(uint32_t a, uint64_t off) {
    while (a > 1 && (off & (a - 1))) a >>= 1;
    return a;
}

/* load / store a value of type t at addr, splitting aggregates by layout */
static tval load_typed(ctx *c, uint32_t as, nir_def *addr, uint32_t t, uint32_t align) {
    tval r = {0};
    const air_type *x = T(c, t);
    if (x->kind == AT_STRUCT || x->kind == AT_ARRAY) {
        r.n = x->count;
        r.m = ralloc_array(c->s, tval, r.n ? r.n : 1);
        for (uint32_t i = 0; i < r.n; ++i) {
            const uint32_t mt = x->kind == AT_STRUCT ? x->members[i] : x->elem;
            const uint32_t off = x->kind == AT_STRUCT ? member_offset(c, t, i) : i * type_size(c, mt);
            r.m[i] = load_typed(c, as, nir_iadd_imm(&c->b, addr, off), mt, off_align(align, off));
        }
        return r;
    }
    uint32_t bits, comps;
    if (scalar_shape(c, t, &bits, &comps)) { fail(c, "load of unsupported type"); return r; }
    if (x->kind == AT_PTR || (x->kind == AT_VEC && T(c, x->elem)->kind == AT_PTR)) {
        /* pointers are 64-bit in memory whatever space they point to */
        nir_def *p = mem_load(c, as, addr, 64, comps, align);
        r.d = bits == 64 ? p : nir_u2u32(&c->b, p);
        return r;
    }
    if (comps > 4) {                             /* split wide vectors */
        nir_def *e[16];
        const uint32_t es = (bits == 1 ? 8 : bits) / 8;
        for (uint32_t i = 0; i < comps; i += 4) {
            const uint32_t n = comps - i < 4 ? comps - i : 4;
            nir_def *part = mem_load(c, as, nir_iadd_imm(&c->b, addr, i * es), bits, n, off_align(align, i * es));
            for (uint32_t k = 0; k < n; ++k) e[i + k] = nir_channel(&c->b, part, k);
        }
        r.d = nir_vec(&c->b, e, comps);
        return r;
    }
    r.d = mem_load(c, as, addr, bits, comps, align);
    return r;
}

static void store_typed(ctx *c, uint32_t as, nir_def *addr, uint32_t t, tval v, uint32_t align) {
    const air_type *x = T(c, t);
    if (x->kind == AT_STRUCT || x->kind == AT_ARRAY) {
        for (uint32_t i = 0; i < x->count && i < v.n; ++i) {
            const uint32_t mt = x->kind == AT_STRUCT ? x->members[i] : x->elem;
            const uint32_t off = x->kind == AT_STRUCT ? member_offset(c, t, i) : i * type_size(c, mt);
            store_typed(c, as, nir_iadd_imm(&c->b, addr, off), mt, v.m[i], off_align(align, off));
        }
        return;
    }
    if (!v.d) { fail(c, "store of an aggregate as a scalar"); return; }
    nir_def *d = v.d;
    if (x->kind == AT_PTR && d->bit_size != 64) d = nir_u2u64(&c->b, d);
    if (d->num_components > 4) {
        const uint32_t es = (d->bit_size == 1 ? 8 : d->bit_size) / 8;
        for (uint32_t i = 0; i < d->num_components; i += 4) {
            const uint32_t n = d->num_components - i < 4 ? d->num_components - i : 4;
            mem_store(c, as, nir_iadd_imm(&c->b, addr, i * es),
                      nir_channels(&c->b, d, nir_component_mask(n) << i), off_align(align, i * es));
        }
        return;
    }
    mem_store(c, as, addr, d, align);
}

/* alignment we can prove for a pointer value (for memcpy chunking) */
static uint32_t known_align(ctx *c, uint32_t v, int depth) {
    if (depth > 8 || v >= c->f->nvalues) return 1;
    const air_value *x = &c->f->values[v];
    if (x->kind == AV_ARG) return (uint32_t)c->arg_align[x->ival];
    if (x->kind == AV_GLOBAL) return 4;
    if (x->kind != AV_INST) return 1;
    const air_inst *in = &c->f->insts[x->inst];
    if (in->op == AI_ALLOCA) return in->align & 31 ? 1u << ((in->align & 31) - 1) : 4;
    if (in->op == AI_CAST && (in->sub == 11 || in->sub == 12)) return known_align(c, in->ops[0], depth + 1);
    if (in->op == AI_GEP) {
        uint32_t a = known_align(c, in->ops[0], depth + 1);
        uint32_t t = in->callee_type;
        for (uint32_t i = 1; i < in->nops; ++i) {
            const air_value *iv = &c->f->values[in->ops[i]];
            const int konst = iv->kind == AV_CINT || iv->kind == AV_CNULL;
            if (i == 1) {
                a = min_align(a, konst ? off_align(a, (uint64_t)((int64_t)iv->ival * type_size(c, t)))
                                       : off_align(a, type_size(c, t)));
                continue;
            }
            const air_type *tt = T(c, t);
            if (tt->kind == AT_STRUCT) {
                a = off_align(a, member_offset(c, t, (uint32_t)iv->ival));
                t = tt->members[iv->ival];
            } else {
                t = tt->elem;
                a = konst ? off_align(a, (uint64_t)((int64_t)iv->ival * type_size(c, t)))
                          : off_align(a, type_size(c, t));
            }
        }
        return a;
    }
    return 1;
}

/* ---- system values ---- */
static nir_def *push_u32(ctx *c, uint32_t off) {
    return nir_load_push_constant(&c->b, 1, 32, nir_imm_int(&c->b, 0), .base = off, .range = 4);
}
static nir_def *push_u64(ctx *c, uint32_t off) {
    return nir_load_push_constant(&c->b, 1, 64, nir_imm_int(&c->b, 0), .base = off, .range = 8);
}

static nir_def *fit_uvec(ctx *c, nir_def *v3, uint32_t t) {
    uint32_t bits, comps;
    if (scalar_shape(c, t, &bits, &comps)) { fail(c, "odd builtin argument type"); return v3; }
    nir_def *v = comps == 3 ? v3 : nir_channels(&c->b, v3, nir_component_mask(comps));
    return bits == 32 ? v : nir_u2uN(&c->b, v, bits);
}

/* a float vector builtin (barycentrics, interpolants) cut/converted to t */
static nir_def *fit_fvec(ctx *c, nir_def *v, uint32_t t) {
    uint32_t bits, comps;
    if (scalar_shape(c, t, &bits, &comps)) { fail(c, "odd builtin argument type"); return v; }
    if (comps < v->num_components) v = nir_channels(&c->b, v, nir_component_mask(comps));
    return bits == v->bit_size ? v : nir_f2fN(&c->b, v, bits);
}

/* ---- ALU helpers ---- */
static nir_def *bool_to(ctx *c, nir_def *b, uint32_t bits) { return nir_b2iN(&c->b, b, bits); }

static nir_def *fround_away(nir_builder *b, nir_def *x) {
    /* round half away from zero (Metal round) */
    nir_def *a = nir_ffloor(b, nir_fadd(b, nir_fabs(b, x), nir_imm_floatN_t(b, 0.5, x->bit_size)));
    return nir_fmul(b, nir_fsign(b, x), a);
}

static nir_def *splat_like(nir_builder *b, nir_def *s, nir_def *like) {
    if (s->num_components == like->num_components) return s;
    nir_def *e[16];
    for (unsigned i = 0; i < like->num_components; ++i) e[i] = s;
    return nir_vec(b, e, like->num_components);
}

/* atomic op name -> nir op */
static int atomic_op(const char *n, int is_signed, int is_float, nir_atomic_op *op) {
    if (!strcmp(n, "add")) *op = is_float ? nir_atomic_op_fadd : nir_atomic_op_iadd;
    else if (!strcmp(n, "sub")) *op = nir_atomic_op_iadd;          /* negated operand */
    else if (!strcmp(n, "and")) *op = nir_atomic_op_iand;
    else if (!strcmp(n, "or")) *op = nir_atomic_op_ior;
    else if (!strcmp(n, "xor")) *op = nir_atomic_op_ixor;
    else if (!strcmp(n, "min")) *op = is_float ? nir_atomic_op_fmin : is_signed ? nir_atomic_op_imin : nir_atomic_op_umin;
    else if (!strcmp(n, "max")) *op = is_float ? nir_atomic_op_fmax : is_signed ? nir_atomic_op_imax : nir_atomic_op_umax;
    else if (!strcmp(n, "xchg")) *op = nir_atomic_op_xchg;
    else return -1;
    return 0;
}

static nir_def *atomic(ctx *c, uint32_t as, nir_def *addr, nir_def *data, nir_atomic_op op) {
    const unsigned bits = data->bit_size;
    if (as == 3 && c->tg_guard) {
        nir_def *inb, *a = tg_addr(c, addr, bits / 8, &inb);
        return nir_bcsel(&c->b, inb, nir_shared_atomic(&c->b, bits, a, data, .atomic_op = op), nir_imm_intN_t(&c->b, 0, bits));
    }
    if (as == 3) return nir_shared_atomic(&c->b, bits, addr, data, .atomic_op = op);
    return nir_global_atomic(&c->b, bits, addr, data, .atomic_op = op);
}

static nir_def *atomic_swap(ctx *c, uint32_t as, nir_def *addr, nir_def *cmp, nir_def *data) {
    const unsigned bits = data->bit_size;
    if (as == 3 && c->tg_guard) {
        nir_def *inb, *a = tg_addr(c, addr, bits / 8, &inb);
        return nir_bcsel(&c->b, inb, nir_shared_atomic_swap(&c->b, bits, a, cmp, data, .atomic_op = nir_atomic_op_cmpxchg),
                         nir_imm_intN_t(&c->b, 0, bits));
    }
    if (as == 3) return nir_shared_atomic_swap(&c->b, bits, addr, cmp, data, .atomic_op = nir_atomic_op_cmpxchg);
    return nir_global_atomic_swap(&c->b, bits, addr, cmp, data, .atomic_op = nir_atomic_op_cmpxchg);
}

static void barrier(ctx *c, uint32_t flags, mesa_scope exec) {
    /* air mem_flags: 1 device, 2 threadgroup, 4 texture */
    nir_variable_mode modes = 0;
    if (flags & 1) modes |= nir_var_mem_global;
    if (flags & 2) modes |= nir_var_mem_shared;
    if (flags & 4) modes |= nir_var_image;
    if (!flags) modes = 0;
    nir_barrier(&c->b, .execution_scope = exec,
                .memory_scope = modes ? (flags & 1 ? SCOPE_DEVICE : SCOPE_WORKGROUP) : SCOPE_NONE,
                .memory_semantics = modes ? NIR_MEMORY_ACQ_REL : 0, .memory_modes = modes);
}

/* strip "air." [fast_|precise_] and the trailing type suffix(es) */
static void air_base(const char *name, char *base, size_t n, int *fast) {
    const char *p = name + 4;
    *fast = 0;
    if (!strncmp(p, "fast_", 5)) { p += 5; *fast = 1; }
    else if (!strncmp(p, "precise_", 8)) p += 8;
    snprintf(base, n, "%s", p);
    char *dot = strchr(base, '.');
    if (dot) *dot = 0;
}

static tval call(ctx *c, const air_inst *in);
static tval inline_call(ctx *c, const air_inst *in, air_function *callee, int depth);
static tval linked_call(ctx *c, const air_inst *in, air_module *lm, air_function *callee);
static nir_block *new_block(ctx *c);
static uint32_t count_leaves(ctx *c, uint32_t t);
static void make_leaf_vars(ctx *c, uint32_t t, nir_variable **v, uint32_t *k);
static void store_leaves(ctx *c, uint32_t t, nir_variable **v, uint32_t *k, tval val);
static tval load_leaves(ctx *c, uint32_t t, nir_variable **v, uint32_t *k);

/* ---- instructions ---- */
static nir_def *binop(ctx *c, uint32_t sub, uint32_t t, nir_def *a, nir_def *b2) {
    nir_builder *b = &c->b;
    if (is_float(c, t)) {
        switch (sub) {
        case 0: return nir_fadd(b, a, b2);
        case 1: return nir_fsub(b, a, b2);
        case 2: return nir_fmul(b, a, b2);
        case 4: return nir_fdiv(b, a, b2);
        case 6: return nir_frem(b, a, b2);
        }
    } else {
        if (a->bit_size == 1) {
            switch (sub) {
            case 10: return nir_iand(b, a, b2);
            case 11: return nir_ior(b, a, b2);
            case 12: return nir_ixor(b, a, b2);
            case 0: return nir_ixor(b, a, b2);
            case 1: return nir_ixor(b, a, b2);
            case 2: return nir_iand(b, a, b2);
            }
        }
        switch (sub) {
        case 0: return nir_iadd(b, a, b2);
        case 1: return nir_isub(b, a, b2);
        case 2: return nir_imul(b, a, b2);
        case 3: return nir_udiv(b, a, b2);
        case 4: return nir_idiv(b, a, b2);
        case 5: return nir_umod(b, a, b2);
        case 6: return nir_irem(b, a, b2);
        case 7: return nir_ishl(b, a, nir_u2u32(b, b2));
        case 8: return nir_ushr(b, a, nir_u2u32(b, b2));
        case 9: return nir_ishr(b, a, nir_u2u32(b, b2));
        case 10: return nir_iand(b, a, b2);
        case 11: return nir_ior(b, a, b2);
        case 12: return nir_ixor(b, a, b2);
        }
    }
    fail(c, "binop %u", sub);
    return a;
}

static nir_def *cmp(ctx *c, uint32_t pred, nir_def *a, nir_def *b2) {
    nir_builder *b = &c->b;
    if (a->bit_size == 1 && pred >= 32) { a = nir_b2i32(b, a); b2 = nir_b2i32(b, b2); }
    switch (pred) {
    case 0: return nir_imm_false(b);
    case 1: return nir_feq(b, a, b2);                               /* OEQ */
    case 2: return nir_flt(b, b2, a);                               /* OGT */
    case 3: return nir_fge(b, a, b2);                               /* OGE */
    case 4: return nir_flt(b, a, b2);                               /* OLT */
    case 5: return nir_fge(b, b2, a);                               /* OLE */
    case 6: return nir_ior(b, nir_flt(b, a, b2), nir_flt(b, b2, a));/* ONE */
    case 7: return nir_iand(b, nir_feq(b, a, a), nir_feq(b, b2, b2));/* ORD */
    case 8: return nir_inot(b, nir_iand(b, nir_feq(b, a, a), nir_feq(b, b2, b2)));
    case 9: return nir_inot(b, nir_ior(b, nir_flt(b, a, b2), nir_flt(b, b2, a)));  /* UEQ */
    case 10: return nir_inot(b, nir_fge(b, b2, a));                 /* UGT */
    case 11: return nir_inot(b, nir_flt(b, a, b2));                 /* UGE */
    case 12: return nir_inot(b, nir_fge(b, a, b2));                 /* ULT */
    case 13: return nir_inot(b, nir_flt(b, b2, a));                 /* ULE */
    case 14: return nir_fneu(b, a, b2);                             /* UNE */
    case 15: return nir_imm_true(b);
    case 32: return nir_ieq(b, a, b2);
    case 33: return nir_ine(b, a, b2);
    case 34: return nir_ult(b, b2, a);
    case 35: return nir_uge(b, a, b2);
    case 36: return nir_ult(b, a, b2);
    case 37: return nir_uge(b, b2, a);
    case 38: return nir_ilt(b, b2, a);
    case 39: return nir_ige(b, a, b2);
    case 40: return nir_ilt(b, a, b2);
    case 41: return nir_ige(b, b2, a);
    }
    fail(c, "compare predicate %u", pred);
    return nir_imm_false(b);
}

static nir_def *cast(ctx *c, uint32_t op, nir_def *a, uint32_t srct, uint32_t dstt) {
    nir_builder *b = &c->b;
    uint32_t bits, comps;
    if (scalar_shape(c, dstt, &bits, &comps)) { fail(c, "cast to odd type"); return a; }
    switch (op) {
    case 0:                                                          /* trunc */
        if (bits == 1) return nir_i2b(b, nir_iand_imm(b, a, 1));
        return nir_u2uN(b, a, bits);
    case 1: return a->bit_size == 1 ? nir_b2iN(b, a, bits) : nir_u2uN(b, a, bits);   /* zext */
    case 2: return a->bit_size == 1 ? nir_ineg(b, nir_b2iN(b, a, bits)) : nir_i2iN(b, a, bits);
    case 3: return nir_f2uN(b, a, bits);
    case 4: return nir_f2iN(b, a, bits);
    case 5: return a->bit_size == 1 ? nir_b2fN(b, a, bits) : nir_u2fN(b, a, bits);
    case 6: return a->bit_size == 1 ? nir_fneg(b, nir_b2fN(b, a, bits)) : nir_i2fN(b, a, bits);
    case 7: case 8: return nir_f2fN(b, a, bits);
    case 9: case 10: case 12:                                        /* ptr<->int, addrspace */
        return a->bit_size == bits ? a : nir_u2uN(b, a, bits);
    case 11: {                                                       /* bitcast */
        if (ptr_as(c, srct) != ~0u || ptr_as(c, dstt) != ~0u)
            return a->bit_size == bits ? a : nir_u2uN(b, a, bits);
        if (a->bit_size == bits && a->num_components == comps) return a;
        if (a->bit_size == 1 || bits == 1) { fail(c, "bitcast of i1 vectors"); return a; }
        return nir_bitcast_vector(b, a, bits);
    }
    }
    fail(c, "cast op %u", op);
    (void)srct;
    return a;
}

static tval gep(ctx *c, const air_inst *in) {
    tval r = {0};
    nir_builder *b = &c->b;
    nir_def *p = getd(c, in->ops[0]);
    const uint32_t pbits = p->bit_size;
    uint32_t t = in->callee_type;
    if (p->num_components != 1) { fail(c, "vector gep"); return r; }
    int64_t konst = 0;
    for (uint32_t i = 1; i < in->nops; ++i) {
        const air_value *iv = &c->f->values[in->ops[i]];
        uint32_t stride;
        if (i == 1) stride = type_size(c, t);
        else {
            const air_type *tt = T(c, t);
            if (tt->kind == AT_STRUCT) {
                const uint32_t fi = (uint32_t)iv->ival;
                konst += member_offset(c, t, fi);
                t = tt->members[fi];
                continue;
            }
            t = tt->elem;
            stride = type_size(c, t);
        }
        if (iv->kind == AV_CINT || iv->kind == AV_CNULL) {
            konst += (iv->kind == AV_CINT ? (int64_t)iv->ival : 0) * stride;
            continue;
        }
        nir_def *idx = getd(c, in->ops[i]);
        idx = idx->bit_size == pbits ? idx : nir_i2iN(b, idx, pbits);
        p = nir_iadd(b, p, nir_imul_imm(b, idx, stride));
    }
    r.d = konst ? nir_iadd_imm(b, p, konst) : p;
    return r;
}

static tval extract_path(tval v, const uint32_t *idx, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        if (!v.m || idx[i] >= v.n) return (tval){0};
        v = v.m[idx[i]];
    }
    return v;
}

static tval insert_path(ctx *c, tval agg, const uint32_t *idx, uint32_t n, tval val) {
    if (!n) return val;
    tval r = agg;
    r.m = ralloc_array(c->s, tval, agg.n ? agg.n : 1);
    memcpy(r.m, agg.m, agg.n * sizeof(tval));
    if (idx[0] < r.n) r.m[idx[0]] = insert_path(c, agg.m[idx[0]], idx + 1, n - 1, val);
    return r;
}

static void emit_inst(ctx *c, const air_inst *in) {
    nir_builder *b = &c->b;
    tval r = {0};
    switch (in->op) {
    case AI_BINOP: r.d = binop(c, in->sub, in->type, getd(c, in->ops[0]), getd(c, in->ops[1])); break;
    case AI_UNOP: r.d = nir_fneg(b, getd(c, in->ops[0])); break;
    case AI_FREEZE: r = get(c, in->ops[0]); break;
    case AI_CAST:
        r.d = cast(c, in->sub, getd(c, in->ops[0]), c->f->values[in->ops[0]].type, in->type);
        break;
    case AI_GEP: r = gep(c, in); break;
    case AI_SELECT: {
        tval tv = get(c, in->ops[1]), fv = get(c, in->ops[2]);
        nir_def *cond = getd(c, in->ops[0]);
        if (tv.d) r.d = nir_bcsel(b, cond, tv.d, fv.d);
        else {                                   /* aggregate select: per member */
            r.n = tv.n;
            r.m = ralloc_array(c->s, tval, r.n ? r.n : 1);
            for (uint32_t i = 0; i < r.n; ++i)
                r.m[i].d = tv.m[i].d && fv.m[i].d ? nir_bcsel(b, cond, tv.m[i].d, fv.m[i].d) : NULL;
        }
        break;
    }
    case AI_EXTRACTELT: {
        nir_def *v = getd(c, in->ops[0]);
        const air_value *iv = &c->f->values[in->ops[1]];
        r.d = iv->kind == AV_CINT || iv->kind == AV_CNULL ? nir_channel(b, v, (unsigned)(iv->kind == AV_CINT ? iv->ival : 0))
                                  : nir_vector_extract(b, v, nir_u2u32(b, getd(c, in->ops[1])));
        break;
    }
    case AI_INSERTELT: {
        nir_def *v = getd(c, in->ops[0]), *e = getd(c, in->ops[1]);
        const air_value *iv = &c->f->values[in->ops[2]];
        r.d = iv->kind == AV_CINT || iv->kind == AV_CNULL ? nir_vector_insert_imm(b, v, e, (unsigned)(iv->kind == AV_CINT ? iv->ival : 0))
                                  : nir_vector_insert(b, v, e, nir_u2u32(b, getd(c, in->ops[2])));
        break;
    }
    case AI_SHUFFLE: {
        nir_def *v1 = getd(c, in->ops[0]), *v2 = getd(c, in->ops[1]);
        const air_value *mv = &c->f->values[in->ops[2]];
        const uint32_t n = T(c, in->type)->count;
        nir_def *e[16];
        for (uint32_t i = 0; i < n && i < 16; ++i) {
            int64_t k = 0;
            if (mv->kind == AV_CDATA) k = (int64_t)mv->data[i];
            else if (mv->kind == AV_CAGG) {
                const air_value *ev = &c->f->values[mv->elts[i]];
                k = ev->kind == AV_CINT ? (int64_t)ev->ival : 0;
            }
            if (k < 0 || (uint64_t)k >= v1->num_components + v2->num_components) k = 0;
            e[i] = (uint32_t)k < v1->num_components ? nir_channel(b, v1, (unsigned)k)
                                                    : nir_channel(b, v2, (unsigned)k - v1->num_components);
        }
        r.d = nir_vec(b, e, n);
        break;
    }
    case AI_CMP: r.d = cmp(c, in->sub, getd(c, in->ops[0]), getd(c, in->ops[1])); break;
    case AI_ALLOCA: {
        uint32_t s, a;
        layout(c, in->callee_type, &s, &a);
        const air_value *nv = &c->f->values[in->ops[0]];
        const uint32_t count = nv->kind == AV_CINT ? (uint32_t)nv->ival : nv->kind == AV_CNULL ? 0 : 1;
        const uint32_t al = in->align & 31 ? 1u << ((in->align & 31) - 1) : a;
        c->scratch = (c->scratch + al - 1) / al * al;
        r.d = nir_imm_int(b, (int)c->scratch);
        c->scratch += s * count;
        break;
    }
    case AI_LOAD: {
        const uint32_t pt = c->f->values[in->ops[0]].type;
        const uint32_t al = in->align ? 1u << (in->align - 1) : type_align(c, in->type);
        r = load_typed(c, T(c, pt)->addrspace, getd(c, in->ops[0]), in->type, al);
        break;
    }
    case AI_STORE: {
        const uint32_t pt = c->f->values[in->ops[0]].type;
        const uint32_t vt = c->f->values[in->ops[1]].type;
        const uint32_t al = in->align ? 1u << (in->align - 1) : type_align(c, vt);
        store_typed(c, T(c, pt)->addrspace, getd(c, in->ops[0]), vt, get(c, in->ops[1]), al);
        return;
    }
    case AI_EXTRACTVAL: r = extract_path(get(c, in->ops[0]), in->idx, in->nidx); break;
    case AI_INSERTVAL: r = insert_path(c, get(c, in->ops[0]), in->idx, in->nidx, get(c, in->ops[1])); break;
    case AI_ATOMICRMW: {
        /* LLVM atomicrmw ops: 0 xchg 1 add 2 sub 3 and 4 nand 5 or 6 xor 7 max 8 min 9 umax 10 umin 11 fadd 12 fsub */
        static const int map[] = {nir_atomic_op_xchg, nir_atomic_op_iadd, nir_atomic_op_iadd,
            nir_atomic_op_iand, -1, nir_atomic_op_ior, nir_atomic_op_ixor, nir_atomic_op_imax,
            nir_atomic_op_imin, nir_atomic_op_umax, nir_atomic_op_umin, nir_atomic_op_fadd, nir_atomic_op_fadd};
        if (in->sub >= 13 || map[in->sub] < 0) { fail(c, "atomicrmw op %u", in->sub); return; }
        nir_def *v = getd(c, in->ops[1]);
        if (in->sub == 2) v = nir_ineg(b, v);
        if (in->sub == 12) v = nir_fneg(b, v);
        const uint32_t pt = c->f->values[in->ops[0]].type;
        r.d = atomic(c, T(c, pt)->addrspace, getd(c, in->ops[0]), v, (nir_atomic_op)map[in->sub]);
        break;
    }
    case AI_CMPXCHG: {
        const uint32_t pt = c->f->values[in->ops[0]].type;
        nir_def *old = atomic_swap(c, T(c, pt)->addrspace, getd(c, in->ops[0]), getd(c, in->ops[1]),
                                   getd(c, in->ops[2]));
        /* {old, success} */
        r.n = 2;
        r.m = ralloc_array(c->s, tval, 2);
        r.m[0].d = old;
        r.m[1].d = nir_ieq(b, old, getd(c, in->ops[1]));
        r.m[1].n = 0;
        break;
    }
    case AI_FENCE: nir_barrier(b, .memory_scope = SCOPE_DEVICE, .memory_semantics = NIR_MEMORY_ACQ_REL,
                               .memory_modes = nir_var_mem_global | nir_var_mem_shared); return;
    case AI_CALL: r = call(c, in); break;
    default: fail(c, "instruction %u in a block body", in->op); return;
    }
    if (in->value != ~0u) set(c, in->value, r);
}

/* integer constant (LLVM writes 0 as a null constant) */
static int const_int(ctx *c, uint32_t v, int64_t *out) {
    const air_value *x = &c->f->values[v];
    if (x->kind == AV_CINT) { *out = (int64_t)x->ival; return 1; }
    if (x->kind == AV_CNULL) { *out = 0; return 1; }
    return 0;
}

/* ---- calls: air.* and llvm.* intrinsics ---- */
static const char *callee_name(ctx *c, const air_inst *in) {
    const air_value *cv = &c->f->values[in->ops[0]];
    if (cv->kind != AV_FUNCTION) return NULL;
    return c->m->functions[cv->ival].name;
}

static nir_def *arg(ctx *c, const air_inst *in, uint32_t i) { return getd(c, in->ops[1 + i]); }

static nir_def *math1(ctx *c, const char *op, nir_def *x) {
    nir_builder *b = &c->b;
    const unsigned bs = x->bit_size;
    if (!strcmp(op, "fabs")) return nir_fabs(b, x);
    if (!strcmp(op, "floor")) return nir_ffloor(b, x);
    if (!strcmp(op, "ceil")) return nir_fceil(b, x);
    if (!strcmp(op, "trunc")) return nir_ftrunc(b, x);
    if (!strcmp(op, "round")) return fround_away(b, x);
    if (!strcmp(op, "rint")) return nir_fround_even(b, x);
    if (!strcmp(op, "fract")) return nir_ffract(b, x);
    if (!strcmp(op, "sqrt")) return nir_fsqrt(b, x);
    if (!strcmp(op, "rsqrt")) return nir_frsq(b, x);
    if (!strcmp(op, "exp2")) return nir_fexp2(b, x);
    if (!strcmp(op, "exp")) return nir_fexp2(b, nir_fmul_imm(b, x, 1.4426950408889634));
    if (!strcmp(op, "exp10")) return nir_fexp2(b, nir_fmul_imm(b, x, 3.321928094887362));
    if (!strcmp(op, "log2")) return nir_flog2(b, x);
    if (!strcmp(op, "log")) return nir_fmul_imm(b, nir_flog2(b, x), 0.6931471805599453);
    if (!strcmp(op, "log10")) return nir_fmul_imm(b, nir_flog2(b, x), 0.30102999566398120);
    if (!strcmp(op, "sin")) return nir_fsin(b, x);
    if (!strcmp(op, "cos")) return nir_fcos(b, x);
    if (!strcmp(op, "tan")) return nir_fdiv(b, nir_fsin(b, x), nir_fcos(b, x));
    if (!strcmp(op, "sinpi")) return nir_fsin(b, nir_fmul_imm(b, x, M_PI));
    if (!strcmp(op, "cospi")) return nir_fcos(b, nir_fmul_imm(b, x, M_PI));
    if (!strcmp(op, "tanpi")) {
        nir_def *y = nir_fmul_imm(b, x, M_PI);
        return nir_fdiv(b, nir_fsin(b, y), nir_fcos(b, y));
    }
    if (!strcmp(op, "asin") || !strcmp(op, "acos")) {
        nir_def *r = nir_fsqrt(b, nir_fsub(b, nir_imm_floatN_t(b, 1.0, bs), nir_fmul(b, x, x)));
        return op[1] == 's' ? nir_atan2(b, x, r) : nir_atan2(b, r, x);
    }
    if (!strcmp(op, "atan")) return nir_atan(b, x);
    if (!strcmp(op, "sinh")) return nir_fmul_imm(b, nir_fsub(b, nir_fexp(b, x), nir_fexp(b, nir_fneg(b, x))), 0.5);
    if (!strcmp(op, "cosh")) return nir_fmul_imm(b, nir_fadd(b, nir_fexp(b, x), nir_fexp(b, nir_fneg(b, x))), 0.5);
    if (!strcmp(op, "tanh")) {
        nir_def *e = nir_fexp(b, nir_fmul_imm(b, nir_fmin(b, nir_fmax(b, x, nir_imm_floatN_t(b, -10, bs)),
                                                          nir_imm_floatN_t(b, 10, bs)), 2.0));
        return nir_fdiv(b, nir_fadd_imm(b, e, -1.0), nir_fadd_imm(b, e, 1.0));
    }
    if (!strcmp(op, "saturate")) return nir_fsat(b, x);
    if (!strcmp(op, "sign")) return nir_fsign(b, x);
    if (!strcmp(op, "isnan")) return nir_fneu(b, x, x);
    if (!strcmp(op, "isinf")) return nir_feq(b, nir_fabs(b, x), nir_imm_floatN_t(b, INFINITY, bs));
    if (!strcmp(op, "isfinite")) return nir_flt(b, nir_fabs(b, x), nir_imm_floatN_t(b, INFINITY, bs));
    return NULL;
}

static nir_def *reduce_bool(nir_builder *b, nir_def *v, int all) {
    nir_def *r = nir_channel(b, v, 0);
    for (unsigned i = 1; i < v->num_components; ++i)
        r = all ? nir_iand(b, r, nir_channel(b, v, i)) : nir_ior(b, r, nir_channel(b, v, i));
    return r;
}

/* air.convert.<dk>.<dt>.<sk>.<st>, kinds f / s / u, optional .sat / .rte */
static nir_def *convert(ctx *c, const char *name, nir_def *x, uint32_t dstt) {
    nir_builder *b = &c->b;
    char dk = 0, sk = 0;
    const char *p = name + strlen("air.convert.");
    dk = p[0];
    p = strchr(p, '.');
    if (p) p = strchr(p + 1, '.');
    if (p) sk = p[1];
    uint32_t bits, comps;
    if (!dk || !sk || scalar_shape(c, dstt, &bits, &comps)) { fail(c, "convert %s", name); return x; }
    if (dk == 'f') {
        if (sk == 'f') return nir_f2fN(b, x, bits);
        if (x->bit_size == 1) return nir_b2fN(b, x, bits);
        return sk == 's' ? nir_i2fN(b, x, bits) : nir_u2fN(b, x, bits);
    }
    if (bits == 1) return sk == 'f' ? nir_fneu_imm(b, x, 0.0) : nir_ine_imm(b, x, 0);
    if (sk == 'f') return dk == 's' ? nir_f2iN(b, x, bits) : nir_f2uN(b, x, bits);
    if (x->bit_size == 1) return nir_b2iN(b, x, bits);
    if (x->bit_size == bits) return x;
    return sk == 's' ? nir_i2iN(b, x, bits) : nir_u2uN(b, x, bits);
}

/* ---- textures (hardware: tex / txf / txs / tg4 / surface store) ----
 * A texture value is the 64-bit push word (tic | flags << 32), a sampler the
 * 64-bit zero-extended sampler index. */

/* texture kind from an air.*_<kind> name */
typedef struct {
    enum glsl_sampler_dim dim;
    unsigned coords;        /* without the array layer */
    bool array, shadow, cube;
} tex_kind;

static int parse_tex_kind(const char *name, tex_kind *k) {
    memset(k, 0, sizeof(*k));
    const char *p = strstr(name, "_texture_");
    const int depth = !p && (p = strstr(name, "_depth_")) != NULL;
    if (!p) return -1;
    p += depth ? 7 : 9;
    k->shadow = depth;
    if (!strncmp(p, "buffer_1d", 9)) { k->dim = GLSL_SAMPLER_DIM_BUF; k->coords = 1; return 0; }
    if (!strncmp(p, "1d", 2)) { k->dim = GLSL_SAMPLER_DIM_1D; k->coords = 1; p += 2; }
    else if (!strncmp(p, "2d_ms", 5)) { k->dim = GLSL_SAMPLER_DIM_MS; k->coords = 2; p += 5; }
    else if (!strncmp(p, "2d", 2)) { k->dim = GLSL_SAMPLER_DIM_2D; k->coords = 2; p += 2; }
    else if (!strncmp(p, "3d", 2)) { k->dim = GLSL_SAMPLER_DIM_3D; k->coords = 3; p += 2; }
    else if (!strncmp(p, "cube", 4)) { k->dim = GLSL_SAMPLER_DIM_CUBE; k->coords = 3; k->cube = true; p += 4; }
    else return -1;
    k->array = !strncmp(p, "_array", 6);
    return 0;
}

/* texture handle + one nir tex instruction */
static nir_def *emit_tex2(ctx *c, nir_texop op, const tex_kind *k, nir_def *handle, nir_def *coord,
                          nir_def *layer, nir_def *lod, nir_def *bias, nir_def *ddx, nir_def *ddy,
                          nir_def *offset, nir_def *cmp, int comp, nir_alu_type type, unsigned ncomps) {
    nir_builder *b = &c->b;
    nir_tex_src srcs[8];
    unsigned n = 0;
    srcs[n++] = nir_tex_src_for_ssa(nir_tex_src_texture_handle, handle);
    unsigned cc = 0;
    if (coord) {
        const bool fl = op != nir_texop_txf;
        if (layer) {
            nir_def *l = fl ? nir_u2f32(b, layer) : nir_u2u32(b, layer);
            nir_def *ch[5];
            for (unsigned i = 0; i < coord->num_components; ++i) ch[i] = nir_channel(b, coord, i);
            ch[coord->num_components] = l;
            coord = nir_vec(b, ch, coord->num_components + 1);
        }
        cc = coord->num_components;
        srcs[n++] = nir_tex_src_for_ssa(nir_tex_src_coord, coord);
    }
    if (lod) srcs[n++] = nir_tex_src_for_ssa(nir_tex_src_lod, lod);
    if (bias) srcs[n++] = nir_tex_src_for_ssa(nir_tex_src_bias, bias);
    if (ddx) srcs[n++] = nir_tex_src_for_ssa(nir_tex_src_ddx, ddx);
    if (ddy) srcs[n++] = nir_tex_src_for_ssa(nir_tex_src_ddy, ddy);
    if (offset) srcs[n++] = nir_tex_src_for_ssa(nir_tex_src_offset, offset);
    if (cmp) srcs[n++] = nir_tex_src_for_ssa(nir_tex_src_comparator, cmp);
    nir_tex_instr *t = nir_tex_instr_create(b->shader, n);
    t->op = op;
    t->sampler_dim = k->dim;
    t->is_array = k->array;
    t->is_shadow = cmp != NULL;
    t->is_new_style_shadow = cmp != NULL;
    t->dest_type = type;
    t->coord_components = cc;
    t->component = comp;
    for (unsigned i = 0; i < n; ++i) t->src[i] = srcs[i];
    nir_def_init(&t->instr, &t->def, ncomps, 32);
    nir_builder_instr_insert(b, &t->instr);
    return &t->def;
}

/* result type of the air call: vector element kind and bits */
static nir_alu_type tex_type(ctx *c, uint32_t t, uint32_t *bits) {
    const air_type *x = T(c, t);
    if (x->kind == AT_STRUCT && x->count) x = T(c, x->members[0]);
    if (x->kind == AT_VEC) x = T(c, x->elem);
    *bits = x->bits;
    if (x->kind == AT_FLOAT) return nir_type_float32;
    return nir_type_uint32;
}

static tval tex_result(ctx *c, uint32_t t, nir_def *v) {
    /* {<N x T>, i8 residency} or plain <N x T>; N from the declared type */
    uint32_t bits;
    const nir_alu_type ty = tex_type(c, t, &bits);
    const air_type *x = T(c, t);
    const air_type *vt = x->kind == AT_STRUCT && x->count ? T(c, x->members[0]) : x;
    const unsigned want = vt->kind == AT_VEC ? vt->count : 1;
    if (v->num_components > want) v = nir_trim_vector(&c->b, v, want);
    if (bits != 32) v = ty == nir_type_float32 ? nir_f2fN(&c->b, v, bits) : nir_u2uN(&c->b, v, bits);
    tval r = {0};
    if (x->kind == AT_STRUCT) {
        r.n = x->count;
        r.m = ralloc_array(c->s, tval, r.n);
        r.m[0].d = v;
        for (uint32_t i = 1; i < r.n; ++i) r.m[i].d = nir_imm_intN_t(&c->b, 1, 8);
    } else r.d = v;
    return r;
}

static nir_def *tex_handle(ctx *c, nir_def *tex, nir_def *samp) {
    nir_def *tic = nir_iand_imm(&c->b, nir_u2u32(&c->b, tex), 0xfffff);
    if (!samp) return tic;
    return nir_ior(&c->b, tic, nir_ishl_imm(&c->b, nir_u2u32(&c->b, samp), 20));
}

/* a sampler operand: an argument, or (through casts) a constexpr sampler global */
static nir_def *sampler_of(ctx *c, uint32_t v, int depth) {
    const air_value *x = &c->f->values[v];
    if (x->kind == AV_GLOBAL && c->const_samp_slot && c->const_samp_slot[x->ival] >= 0)
        return nir_u2u64(&c->b, push_u32(c, c->samp_base + (c->abi->nsamp + (uint32_t)c->const_samp_slot[x->ival]) * 4));
    if (x->kind == AV_CEXPR && x->n && depth < 4) return sampler_of(c, x->elts[0], depth + 1);
    return getd(c, v);
}

static int const_zero(ctx *c, uint32_t v) {
    const air_value *ov = &c->f->values[v];
    if (ov->kind == AV_CNULL || ov->kind == AV_CUNDEF) return 1;
    if (ov->kind == AV_CINT) return ov->ival == 0;
    if (ov->kind == AV_CDATA) {
        for (uint32_t k = 0; k < ov->n; ++k) if (ov->data[k]) return 0;
        return 1;
    }
    return 0;
}

/* Tahoe's Metal front end (32023.886) passes a sampler to texture reads,
 * made by air.get_read_sampler() right before the read. Sonoma's had none. */
static int is_read_sampler(ctx *c, uint32_t v) {
    const air_value *x = &c->f->values[v];
    if (x->kind != AV_INST) return 0;
    const air_inst *ri = &c->f->insts[x->inst];
    const char *n = ri->op == AI_CALL ? callee_name(c, ri) : NULL;
    return n && !strcmp(n, "air.get_read_sampler");
}

static tval texture_op(ctx *c, const air_inst *in, const char *name) {
    nir_builder *b = &c->b;
    tval r = {0};
    const uint32_t na = in->nops - 1;
    #define A(i) arg(c, in, (i))
    #define OPV(i) in->ops[1 + (i)]
    if (!strncmp(name, "air.is_null_", 12)) {
        r.d = nir_ieq_imm(b, nir_iand_imm(b, nir_u2u32(b, A(0)), 0xfffff), 0);
        return r;
    }
    if (!strncmp(name, "air.get_null_", 13)) { r.d = nir_imm_int64(b, 0); return r; }
    tex_kind k;
    if (parse_tex_kind(name, &k)) { fail(c, "texture kind in %s", name); return r; }
    uint32_t bits;
    const nir_alu_type ty = in->type != ~0u && T(c, in->type)->kind != AT_VOID ? tex_type(c, in->type, &bits) : nir_type_float32;
    const bool grad = strstr(name, "_grad") != NULL;
    /* queries */
    if (!strncmp(name, "air.get_", 8)) {
        nir_def *h = tex_handle(c, A(0), NULL);
        nir_def *q;
        if (!strncmp(name, "air.get_num_mip_levels", 22))
            q = emit_tex2(c, nir_texop_query_levels, &k, h, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0, nir_type_int32, 1);
        else if (!strncmp(name, "air.get_num_samples", 19))
            q = emit_tex2(c, nir_texop_texture_samples, &k, h, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0, nir_type_int32, 1);
        else {
            nir_def *lod = !strncmp(name, "air.get_array_size", 18) || na < 2 ? nir_imm_int(b, 0) : nir_u2u32(b, A(1));
            const unsigned comps = (k.cube ? 2 : k.coords) + (k.array ? 1 : 0);
            nir_def *sz = emit_tex2(c, nir_texop_txs, &k, h, NULL, NULL, lod, NULL, NULL, NULL, NULL, NULL, 0, nir_type_int32, comps);
            unsigned ch = !strncmp(name, "air.get_width", 13) ? 0 : !strncmp(name, "air.get_height", 14) ? 1
                        : !strncmp(name, "air.get_depth", 13) ? 2 : comps - 1;
            q = nir_channel(b, sz, ch < comps ? ch : comps - 1);
        }
        uint32_t qb, qc;
        r.d = !scalar_shape(c, in->type, &qb, &qc) && qb != 32 ? nir_u2uN(b, q, qb) : q;
        return r;
    }
    if (!strncmp(name, "air.write_", 10)) {
        /* (tex, coord, [layer], value, lod, i32) */
        uint32_t i = 1;
        nir_def *coord = nir_u2u32(b, A(i++));
        nir_def *layer = k.array ? nir_u2u32(b, A(i++)) : NULL;
        nir_def *val = A(i++);
        nir_def *lod = i < na ? nir_u2u32(b, A(i)) : nir_imm_int(b, 0);
        const int fl = strstr(name, ".v4f") != NULL || strstr(name, ".f32") != NULL || strstr(name, ".f16") != NULL;
        if (val->bit_size != 32) val = fl ? nir_f2f32(b, val) : nir_u2u32(b, val);
        if (val->num_components < 4) val = nir_pad_vector_imm_int(b, val, 0, 4);
        nir_def *ch[4] = {nir_imm_int(b, 0), nir_imm_int(b, 0), nir_imm_int(b, 0), nir_imm_int(b, 0)};
        for (unsigned q = 0; q < coord->num_components && q < 3; ++q) ch[q] = nir_channel(b, coord, q);
        if (layer) ch[coord->num_components] = layer;
        nir_bindless_image_store(b, tex_handle(c, A(0), NULL), nir_vec(b, ch, 4), nir_undef(b, 1, 32), val, lod,
                                 .image_dim = k.dim == GLSL_SAMPLER_DIM_CUBE ? GLSL_SAMPLER_DIM_2D : k.dim,
                                 .image_array = k.array || k.cube, .format = PIPE_FORMAT_NONE,
                                 .access = ACCESS_NON_READABLE, .src_type = fl ? nir_type_float32 : nir_type_uint32);
        return r;
    }
    if (!strncmp(name, "air.read_", 9)) {
        /* (tex, [read sampler, Tahoe], [i32 depth kind], coord, [layer], lod, i32) */
        uint32_t i = 1 + (na > 1 && is_read_sampler(c, OPV(1)) ? 1 : 0) + (k.shadow ? 1 : 0);
        nir_def *coord = nir_u2u32(b, A(i++));
        nir_def *layer = k.array ? nir_u2u32(b, A(i++)) : NULL;
        nir_def *lod = i < na && k.dim != GLSL_SAMPLER_DIM_MS ? nir_u2u32(b, A(i)) : nir_imm_int(b, 0);
        nir_def *v = emit_tex2(c, nir_texop_txf, &k, tex_handle(c, A(0), NULL), coord, layer, lod, NULL, NULL, NULL,
                               NULL, NULL, 0, ty, 4);
        return tex_result(c, in->type, v);
    }
    const bool gather = !strncmp(name, "air.gather", 10);
    const bool compare = strstr(name, "_compare") != NULL;
    if (strncmp(name, "air.sample", 10) && !gather) { fail(c, "texture operation %s", name); return r; }
    /* (tex, sampler, [i32 depth kind], coord, [layer], [ref], ...) */
    uint32_t i = 2 + (k.shadow ? 1 : 0);
    nir_def *h = tex_handle(c, A(0), sampler_of(c, OPV(1), 0));
    nir_def *coord = A(i++);
    nir_def *layer = k.array ? A(i++) : NULL;
    nir_def *ref = compare ? A(i++) : NULL;
    nir_def *lod = NULL, *bias = NULL, *ddx = NULL, *ddy = NULL, *off = NULL;
    int comp = 0;
    if (grad) {
        ddx = A(i++); ddy = A(i++);
        i++;                                        /* min lod */
        i++;                                        /* i1 */
        if (!k.cube && i < na) { if (!const_zero(c, OPV(i))) off = A(i); i++; }
    } else if (gather) {
        i++;                                        /* i1 */
        if (!k.cube && i < na) { if (!const_zero(c, OPV(i))) off = A(i); i++; }
        if (i < na) { int64_t cv = 0; const_int(c, OPV(i), &cv); comp = (int)cv; }
    } else {
        if (!k.cube) {
            i++;                                    /* i1 */
            if (i < na) { if (!const_zero(c, OPV(i))) off = A(i); i++; }
        }
        int64_t explicit_lod = 0;
        if (i < na) const_int(c, OPV(i), &explicit_lod);
        i++;
        nir_def *lv = i < na ? A(i) : nir_imm_float(b, 0.0f);
        /* compute stages have no derivatives: bias is relative to lod 0 */
        if (c->stage == MESA_SHADER_FRAGMENT && !explicit_lod && !const_zero(c, OPV(i))) bias = lv;
        else if (c->stage == MESA_SHADER_FRAGMENT && !explicit_lod) bias = NULL;
        else lod = lv;
    }
    nir_texop op = gather ? nir_texop_tg4 : grad ? nir_texop_txd : lod ? nir_texop_txl : bias ? nir_texop_txb : nir_texop_tex;
    if (compare && op == nir_texop_tex && c->stage != MESA_SHADER_FRAGMENT) op = nir_texop_txl;
    nir_def *v = emit_tex2(c, op, &k, h, coord, layer, op == nir_texop_txl ? (lod ? lod : nir_imm_float(b, 0.0f)) : NULL,
                           bias, ddx, ddy, off, ref ? nir_f2f32(b, ref) : NULL, comp,
                           k.shadow ? nir_type_float32 : ty, compare && !gather ? 1 : 4);
    return tex_result(c, in->type, v);
    #undef A
    #undef OPV
}

/* ---- simdgroup_matrix 8x8. A matrix value is a vec2 per lane, in the
 * layout Apple GPUs use (MLX and friends rely on it through
 * thread_elements()): qid = lane / 4, row = (qid & 4) + (lane / 2) % 4,
 * col = (qid & 2) * 2 + (lane % 2) * 2; the lane holds (row, col) and
 * (row, col + 1). Multiplies gather rows of A and columns of B with
 * subgroup shuffles. ---- */
static void smat_rc(nir_builder *b, nir_def *lane, nir_def **row, nir_def **col) {
    nir_def *qid = nir_ushr_imm(b, lane, 2);
    *row = nir_iadd(b, nir_iand_imm(b, qid, 4), nir_iand_imm(b, nir_ushr_imm(b, lane, 1), 3));
    *col = nir_iadd(b, nir_ishl_imm(b, nir_iand_imm(b, qid, 2), 1), nir_ishl_imm(b, nir_iand_imm(b, lane, 1), 1));
}
/* the lane holding (row, col), col even */
static nir_def *smat_lane(nir_builder *b, nir_def *row, nir_def *col) {
    nir_def *l = nir_iand_imm(b, nir_ushr_imm(b, col, 1), 1);
    l = nir_ior(b, l, nir_ishl_imm(b, nir_iand_imm(b, row, 3), 1));
    l = nir_ior(b, l, nir_ishl_imm(b, nir_iand_imm(b, nir_ushr_imm(b, col, 2), 1), 3));
    return nir_ior(b, l, nir_ishl_imm(b, nir_iand_imm(b, nir_ushr_imm(b, row, 2), 1), 4));
}
static uint32_t smat_bits(ctx *c, uint32_t t) {
    const air_type *x = T(c, t);
    if (x->kind == AT_VEC) x = T(c, x->elem);
    return x->bits ? x->bits : 32;
}
static nir_def *fconv(nir_builder *b, nir_def *v, uint32_t bits) {
    return v->bit_size == bits ? v : bits == 16 ? nir_f2f16(b, v) : nir_f2f32(b, v);
}
/* address of element (r, c) for load/store: ptr + ((oy + r) * epr + ox + c) * es, or transposed */
static nir_def *smat_addr(ctx *c, nir_def *ptr, nir_def *epr, nir_def *orig, nir_def *tr,
                          nir_def *r, nir_def *col, uint32_t es) {
    nir_builder *b = &c->b;
    nir_def *ox = nir_u2u64(b, nir_channel(b, orig, 0)), *oy = nir_u2u64(b, nir_channel(b, orig, 1));
    nir_def *r64 = nir_u2u64(b, r), *c64 = nir_u2u64(b, col);
    nir_def *mr = nir_bcsel(b, tr, c64, r64), *mc = nir_bcsel(b, tr, r64, c64);
    nir_def *off = nir_imul_imm(b, nir_iadd(b, nir_imul(b, nir_iadd(b, oy, mr), nir_u2u64(b, epr)), nir_iadd(b, ox, mc)), es);
    return nir_iadd(b, ptr, ptr->bit_size == 64 ? off : nir_u2u32(b, off));
}
static int smat_call(ctx *c, const air_inst *in, const char *name, tval *r) {
    if (strncmp(name, "air.simdgroup_matrix_8x8_", 25)) return 0;
    nir_builder *b = &c->b;
    const char *op = name + 25;
    nir_def *lane = nir_load_subgroup_invocation(b), *row, *col;
    smat_rc(b, lane, &row, &col);
    if (!strncmp(op, "init_filled", 11) || !strncmp(op, "init_diag", 9)) {
        const uint32_t bits = smat_bits(c, in->type);
        nir_def *v = fconv(b, arg(c, in, 0), bits), *z = nir_imm_floatN_t(b, 0.0, bits);
        if (op[5] == 'f') r->d = nir_vec2(b, v, v);
        else r->d = nir_vec2(b, nir_bcsel(b, nir_ieq(b, row, col), v, z),
                             nir_bcsel(b, nir_ieq(b, row, nir_iadd_imm(b, col, 1)), v, z));
        return 1;
    }
    if (!strncmp(op, "load", 4) || !strncmp(op, "store", 5)) {
        const int st = op[0] == 's';
        const uint32_t pi = st ? 1 : 0;
        const uint32_t pt = c->f->values[in->ops[1 + pi]].type;
        const uint32_t as = T(c, pt)->addrspace;
        const uint32_t bits = smat_bits(c, st ? c->f->values[in->ops[1]].type : in->type), es = bits / 8;
        nir_def *ptr = arg(c, in, pi);
        nir_def *val = st ? arg(c, in, 0) : NULL, *e[2];
        /* Sonoma: (ptr, i64 elements_per_row, <2 x i64> origin, i1 transpose).
         * Tahoe (front end 32023.886): (ptr, <2 x i64> extent, <2 x i64>
         * strides {column, row}, <2 x i64> origin), transpose = swapped
         * strides; the extent only bounds the access and is not used here. */
        const air_type *a1t = T(c, c->f->values[in->ops[1 + pi + 1]].type);
        const int strided = a1t->kind == AT_VEC;
        nir_def *epr = NULL, *orig, *tr = NULL, *str = NULL;
        if (strided) {
            str = arg(c, in, pi + 2);
            const uint32_t ov = in->ops[1 + pi + 3];
            orig = c->f->values[ov].kind == AV_CNULL ? nir_imm_zero(b, 2, 64) : arg(c, in, pi + 3);
        } else {
            epr = arg(c, in, pi + 1); orig = arg(c, in, pi + 2);
            tr = nir_i2b(b, arg(c, in, pi + 3));
        }
        for (int k = 0; k < 2; ++k) {
            nir_def *a;
            if (strided) {
                nir_def *cc = nir_u2u64(b, nir_iadd_imm(b, col, k)), *rr = nir_u2u64(b, row);
                nir_def *x = nir_iadd(b, nir_u2u64(b, nir_channel(b, orig, 0)), cc);
                nir_def *y = nir_iadd(b, nir_u2u64(b, nir_channel(b, orig, 1)), rr);
                nir_def *off = nir_imul_imm(b, nir_iadd(b, nir_imul(b, x, nir_u2u64(b, nir_channel(b, str, 0))),
                                                         nir_imul(b, y, nir_u2u64(b, nir_channel(b, str, 1)))), es);
                a = nir_iadd(b, ptr, ptr->bit_size == 64 ? off : nir_u2u32(b, off));
            } else a = smat_addr(c, ptr, epr, orig, tr, row, nir_iadd_imm(b, col, k), es);
            if (st) mem_store(c, as, a, nir_channel(b, val, k), es);
            else e[k] = mem_load(c, as, a, bits, 1, es);
        }
        if (!st) r->d = nir_vec2(b, e[0], e[1]);
        return 1;
    }
    if (!strncmp(op, "multiply_accumulate", 19)) {
        const uint32_t bits = smat_bits(c, in->type);
        nir_def *A = arg(c, in, 0), *B = arg(c, in, 1), *C = arg(c, in, 2);
        nir_def *a0 = nir_f2f32(b, nir_channel(b, A, 0)), *a1 = nir_f2f32(b, nir_channel(b, A, 1));
        nir_def *b0 = nir_f2f32(b, nir_channel(b, B, 0)), *b1 = nir_f2f32(b, nir_channel(b, B, 1));
        nir_def *acc0 = nir_f2f32(b, nir_channel(b, C, 0)), *acc1 = nir_f2f32(b, nir_channel(b, C, 1));
        for (int k = 0; k < 8; ++k) {
            nir_def *la = smat_lane(b, row, nir_imm_int(b, k & ~1));   /* A[row][k] */
            nir_def *lb = smat_lane(b, nir_imm_int(b, k), col);       /* B[k][col], B[k][col + 1] */
            nir_def *ak = nir_shuffle(b, (k & 1) ? a1 : a0, la);
            acc0 = nir_ffma(b, ak, nir_shuffle(b, b0, lb), acc0);
            acc1 = nir_ffma(b, ak, nir_shuffle(b, b1, lb), acc1);
        }
        r->d = nir_vec2(b, fconv(b, acc0, bits), fconv(b, acc1, bits));
        return 1;
    }
    fail(c, "simdgroup matrix op %s", name);
    return 1;
}

/* post-tessellation vertex: X.MTL_CONTROL_POINT_FN(i, patch) returns control
 * point i's attributes. Member k is the (k+1)-th node after the
 * patch_control_point_input arg ([air.location_index L 1 ...]); the
 * tessellation control stage hands attribute L over as VARYING_SLOT_VAR0 + L. */
static const air_md *md(ctx *c, int32_t id);
static int arg_int_after(ctx *c, const air_md *n, const char *key, int64_t *out);
static nir_def *fit_bits(ctx *c, nir_def *v, uint32_t t);
static tval control_point(ctx *c, const air_inst *in) {
    nir_builder *b = &c->b;
    tval r = {0};
    const air_md *cp = c->cp_node >= 0 ? &c->m->md[c->cp_node] : NULL;
    const air_type *rt = T(c, in->type);
    if (!cp || rt->kind != AT_STRUCT) { fail(c, "control point fetch without its input"); return r; }
    nir_def *idx = nir_u2u32(b, arg(c, in, 0));
    r.n = rt->count;
    r.m = ralloc_array(c->s, tval, r.n ? r.n : 1);
    uint32_t member = 0;
    for (uint32_t j = 0; j < cp->n && member < r.n; ++j) {
        const air_md *x = md(c, cp->ops[j]);
        if (!x || x->kind != AM_NODE) continue;
        int64_t loc = -1;
        if (arg_int_after(c, x, "air.location_index", &loc) || loc < 0 || loc >= 32) continue;
        const uint32_t mt = rt->members[member];
        uint32_t bits = 32, comps = 4;
        scalar_shape(c, mt, &bits, &comps);
        if (!c->cp_var[loc]) {
            const struct glsl_type *vt = glsl_vector_type(is_float(c, mt) ? GLSL_TYPE_FLOAT : GLSL_TYPE_UINT, 4);
            nir_variable *v = nir_variable_create(c->s, nir_var_shader_in, glsl_array_type(vt, 32, 0), "cp");
            v->data.location = VARYING_SLOT_VAR0 + (int)loc;
            c->cp_var[loc] = v;
        }
        nir_def *val = nir_load_deref(b, nir_build_deref_array(b, nir_build_deref_var(b, c->cp_var[loc]), idx));
        r.m[member++].d = fit_bits(c, val, mt);
    }
    for (; member < r.n; ++member) r.m[member] = zero_of(c, rt->members[member]);
    return r;
}

/* the enqueue's global offset (hidden buffer 0), zero for non-CL kernels */
static nir_def *cl_global_offset(ctx *c) {
    nir_builder *b = &c->b;
    if (!c->buf_shift) return nir_imm_ivec3(b, 0, 0, 0);
    return nir_load_global(b, 3, 32, push_u64(c, 0), .align_mul = 4);
}

/* Function pointers (visible function tables). A table entry holds the
 * function's number in the pipeline's linked functions, 1-based (the driver
 * writes it), and a call through a pointer becomes a compare-and-branch over
 * those functions. NAK has no indirect calls; the linked set is known when
 * the pipeline is built, as Metal requires. Number 0 or an unknown one reads
 * as a zero result. */
static tval indirect_call(ctx *c, const air_inst *in) {
    nir_builder *b = &c->b;
    tval fp = get(c, in->ops[0]);
    nir_def *id = fp.d ? (fp.d->bit_size == 32 ? fp.d : nir_u2u32(b, fp.d)) : nir_imm_int(b, 0);
    const uint32_t rett = in->type;
    const uint32_t nl = count_leaves(c, rett);
    nir_variable **rv = nl ? rzalloc_array(c->s, nir_variable *, nl + 1) : NULL;
    uint32_t k = 0;
    if (nl) make_leaf_vars(c, rett, rv, &k);
    nir_block *join = new_block(c);
    for (uint32_t i = 0; i < c->var->nlink && i < 16 && !c->failed; ++i) {
        if (!c->var->link_fn[i] || c->var->link_fn[i]->nargs > in->nops - 1) continue;
        nir_block *yes = new_block(c), *no = new_block(c);
        nir_goto_if(b, yes, nir_ieq_imm(b, id, (int)(i + 1)), no);
        b->cursor = nir_after_block(yes);
        tval r = linked_call(c, in, c->var->link_mod[i], c->var->link_fn[i]);
        if (nl && !c->failed) { k = 0; store_leaves(c, rett, rv, &k, r); }
        nir_goto(b, join);
        b->cursor = nir_after_block(no);
    }
    if (nl) { k = 0; store_leaves(c, rett, rv, &k, zero_of(c, rett)); }
    nir_goto(b, join);
    b->cursor = nir_after_block(join);
    tval r = {0};
    if (nl && !c->failed) { k = 0; r = load_leaves(c, rett, rv, &k); }
    return r;
}

static tval call(ctx *c, const air_inst *in) {
    nir_builder *b = &c->b;
    tval r = {0};
    const char *name = callee_name(c, in);
    if (!name) return indirect_call(c, in);
    /* pull-model interpolation: air.interpolate_{center,centroid,sample,offset}_{perspective,no_perspective} */
    if (!strncmp(name, "air.interpolate_", 16)) {
        nir_def *h = get(c, in->ops[1]).d;
        const uint32_t idx = h && nir_src_is_const(nir_src_for_ssa(h)) ? (uint32_t)nir_src_as_uint(nir_src_for_ssa(h)) : 0;
        nir_variable *v = idx < c->ninterp ? c->interp_var[idx] : NULL;
        if (!v) return zero_of(c, in->type);
        nir_deref_instr *dr = nir_build_deref_var(b, v);
        const unsigned nc = glsl_get_vector_elements(v->type);
        const char *m = name + 16;
        nir_def *x;
        if (!strncmp(m, "centroid", 8)) x = nir_interp_deref_at_centroid(b, nc, 32, &dr->def);
        else if (!strncmp(m, "sample", 6)) x = nir_interp_deref_at_sample(b, nc, 32, &dr->def, nir_u2u32(b, get(c, in->ops[2]).d));
        /* Metal's offset is from the pixel's top-left corner (the centre is
         * 0.5, 0.5) and not limited to the pixel: the M1 extrapolates, the
         * hardware offset of NVIDIA clamps to half a pixel. So: the value at
         * the centre plus its screen derivatives times the offset. */
        else if (!strncmp(m, "offset", 6)) {
            nir_def *ctr = nir_load_deref(b, dr);
            nir_def *o = nir_fadd_imm(b, get(c, in->ops[2]).d, -0.5);
            x = nir_ffma(b, nir_ddx_fine(b, ctr), nir_channel(b, o, 0),
                         nir_ffma(b, nir_ddy_fine(b, ctr), nir_channel(b, o, 1), ctr));
        }
        else x = nir_load_deref(b, dr);                     /* center */
        r.d = fit_fvec(c, x, in->type);
        return r;
    }
    /* entry i of a visible function table: the 1-based function number */
    if (!strcmp(name, "air.get_function_pointer_visible_function_table")) {
        nir_def *tab = get(c, in->ops[1]).d, *idx = get(c, in->ops[2]).d;
        nir_def *at = nir_iadd(b, tab, nir_u2u64(b, nir_imul_imm(b, idx, 8)));
        r.d = nir_u2u64(b, nir_load_global(b, 1, 32, at, .align_mul = 8));
        return r;
    }
    const uint32_t na = in->nops - 1;
    air_function *callee = &c->m->functions[c->f->values[in->ops[0]].ival];
    if (!callee->is_proto) return inline_call(c, in, callee, c->depth);
    /* the value is only ever the read sampler argument, which texture reads skip */
    if (!strcmp(name, "air.get_read_sampler")) { r.d = nir_imm_int64(b, 0); return r; }

    if (!strncmp(name, "llvm.", 5)) {
        const char *n = name + 5;
        if (!strncmp(n, "lifetime.", 9) || !strncmp(n, "assume", 6) || !strncmp(n, "dbg.", 4) ||
            !strncmp(n, "experimental.noalias", 20) || !strncmp(n, "invariant.", 10))
            return r;
        if (!strncmp(n, "memcpy.", 7) || !strncmp(n, "memmove.", 8) || !strncmp(n, "memset.", 7)) {
            const int set = n[3] == 's';
            int64_t slen = 0;
            if (!const_int(c, in->ops[3], &slen)) { fail(c, "%s with a variable length", name); return r; }
            const uint64_t len = (uint64_t)slen;
            const uint32_t dt = c->f->values[in->ops[1]].type;
            const uint32_t das = T(c, dt)->addrspace;
            uint32_t al = known_align(c, in->ops[1], 0);
            uint32_t sas = 0;
            nir_def *src = NULL, *fillv = NULL;
            if (set) fillv = nir_u2u8(b, arg(c, in, 1));
            else {
                src = arg(c, in, 1);
                sas = T(c, c->f->values[in->ops[2]].type)->addrspace;
                al = min_align(al, known_align(c, in->ops[2], 0));
            }
            if (al > 4) al = 4;
            nir_def *dst = arg(c, in, 0);
            uint64_t o = 0;
            if (len > 65536) { fail(c, "%s of %llu bytes", name, (unsigned long long)len); return r; }
            /* memmove: every chunk is loaded before any is stored */
            nir_def **tmp = set ? NULL : ralloc_array(c->s, nir_def *, (len / al) + 1);
            uint32_t k = 0;
            if (!set)
                for (o = 0; o < len; o += al)
                    tmp[k++] = mem_load(c, sas, nir_iadd_imm(b, src, o), al * 8, 1, al);
            k = 0;
            for (o = 0; o < len; o += al) {
                nir_def *v;
                if (set) {
                    nir_def *w = nir_u2uN(b, fillv, al * 8);
                    for (uint32_t s = 8; s < al * 8; s *= 2) w = nir_ior(b, w, nir_ishl_imm(b, w, s));
                    v = w;
                } else v = tmp[k++];
                mem_store(c, das, nir_iadd_imm(b, dst, o), v, al);
            }
            return r;
        }
        char base[64];
        snprintf(base, sizeof base, "%s", n);
        char *dot = strchr(base, '.');
        if (dot) *dot = 0;
        nir_def *a0 = na > 0 ? arg(c, in, 0) : NULL, *a1 = na > 1 ? arg(c, in, 1) : NULL;
        nir_def *a2 = na > 2 ? arg(c, in, 2) : NULL;
        if (!strcmp(base, "smax")) r.d = nir_imax(b, a0, a1);
        else if (!strcmp(base, "smin")) r.d = nir_imin(b, a0, a1);
        else if (!strcmp(base, "umax")) r.d = nir_umax(b, a0, a1);
        else if (!strcmp(base, "umin")) r.d = nir_umin(b, a0, a1);
        else if (!strcmp(base, "abs")) r.d = nir_iabs(b, a0);
        else if (!strcmp(base, "fabs")) r.d = nir_fabs(b, a0);
        else if (!strcmp(base, "fma") || !strcmp(base, "fmuladd")) r.d = nir_ffma(b, a0, a1, a2);
        else if (!strcmp(base, "minnum") || !strcmp(base, "minimum")) r.d = nir_fmin(b, a0, a1);
        else if (!strcmp(base, "maxnum") || !strcmp(base, "maximum")) r.d = nir_fmax(b, a0, a1);
        else if (!strcmp(base, "copysign"))
            r.d = nir_bcsel(b, nir_flt_imm(b, a1, 0.0), nir_fneg(b, nir_fabs(b, a0)), nir_fabs(b, a0));
        else if (!strcmp(base, "ctpop")) r.d = nir_u2uN(b, nir_bit_count(b, a0), a0->bit_size);
        else if (!strcmp(base, "ctlz")) r.d = nir_u2uN(b, nir_uclz(b, a0), a0->bit_size);
        else if (!strcmp(base, "cttz")) {
            nir_def *t = nir_find_lsb(b, a0);
            r.d = nir_u2uN(b, nir_bcsel(b, nir_ilt_imm(b, t, 0), nir_imm_int(b, (int)a0->bit_size), t), a0->bit_size);
        }
        else if (!strcmp(base, "bitreverse")) r.d = nir_bitfield_reverse(b, a0);
        else if (!strcmp(base, "fshl") || !strcmp(base, "fshr")) {
            const unsigned bs = a0->bit_size;
            nir_def *sh = nir_umod_imm(b, a2, bs);
            nir_def *inv = nir_isub_imm(b, bs, sh);
            nir_def *v = !strcmp(base, "fshl")
                ? nir_ior(b, nir_ishl(b, a0, nir_u2u32(b, sh)), nir_ushr(b, a1, nir_u2u32(b, inv)))
                : nir_ior(b, nir_ushr(b, a1, nir_u2u32(b, sh)), nir_ishl(b, a0, nir_u2u32(b, inv)));
            r.d = nir_bcsel(b, nir_ieq_imm(b, sh, 0), !strcmp(base, "fshl") ? a0 : a1, v);
        }
        else if (!strcmp(base, "uadd")) r.d = nir_uadd_sat(b, a0, a1);
        else if (!strcmp(base, "usub")) r.d = nir_usub_sat(b, a0, a1);
        else if (!strcmp(base, "sadd")) r.d = nir_iadd_sat(b, a0, a1);
        else if (!strcmp(base, "ssub")) r.d = nir_isub_sat(b, a0, a1);
        else if ((r.d = math1(c, base, a0))) {}
        else if (!strcmp(base, "pow")) r.d = nir_fpow(b, a0, a1);
        else fail(c, "intrinsic %s", name);
        return r;
    }
    if (strstr(name, "MTL_CONTROL_POINT_FN")) return control_point(c, in);
    {   /* [[visible]] function from the pipeline's linked functions */
        const char *suf = strstr(name, ".MTL_VISIBLE_FN_REF");
        if (suf && !suf[19]) {
            const size_t bl = (size_t)(suf - name);
            for (uint32_t i = 0; i < c->var->nlink; ++i)
                if (strlen(c->var->link_name[i]) == bl && !strncmp(c->var->link_name[i], name, bl))
                    return linked_call(c, in, c->var->link_mod[i], c->var->link_fn[i]);
            /* MPS kernels reference optional stages (prefixTertiary_f ...)
             * behind function constants and link only what the pipeline
             * uses; such a call is dead once the constants fold, so it
             * reads as zero instead of failing the whole kernel */
            fprintf(stderr, "air: note: visible function %.*s is not linked, its calls read as zero\n", (int)bl, name);
            return zero_of(c, in->type);
        }
    }
    if (strncmp(name, "air.", 4)) { fail(c, "external function %s", name); return r; }

    if (smat_call(c, in, name, &r)) return r;

    /* mesh shader outputs (see mesh_sysvals) */
    if (!strncmp(name, "air.set_", 8) && strstr(name, "_mesh")) {
        const air_abi *abi = c->abi;
        if (!strcmp(name, "air.set_threadgroups_per_grid_mesh_properties") && c->mesh_grid_addr) {
            nir_def *v = arg(c, in, 1);
            if (v->bit_size != 32) v = nir_u2u32(b, v);
            nir_store_global(b, nir_pad_vector_imm_int(b, v, 0, 3), c->mesh_grid_addr, .align_mul = 16);
            return r;
        }
        if (!c->mesh_out) { fail(c, "%s outside a mesh function", name); return r; }
        if (!strcmp(name, "air.set_primitive_count_mesh")) {
            nir_store_global(b, nir_u2u32(b, arg(c, in, 1)), c->mesh_out, .align_mul = 16);
            return r;
        }
        if (!strcmp(name, "air.set_index_mesh")) {
            nir_def *off = nir_iadd_imm(b, nir_imul_imm(b, nir_u2u32(b, arg(c, in, 1)), 4), 16 + abi->mesh_max_v * abi->mesh_vstride);
            nir_store_global(b, nir_u2u32(b, arg(c, in, 2)), nir_iadd(b, c->mesh_out, nir_u2u64(b, off)), .align_mul = 4);
            return r;
        }
        uint32_t slot = 0;
        nir_def *vi = NULL, *val = NULL;
        if (!strcmp(name, "air.set_position_mesh")) { vi = arg(c, in, 1); val = arg(c, in, 2); }
        else if (!strncmp(name, "air.set_vertex_data_mesh", 24)) {
            int64_t di = 0;
            if (!const_int(c, in->ops[2], &di) || di < 0 || (uint32_t)di >= abi->mesh_nvdata) { fail(c, "%s index", name); return r; }
            slot = 1 + (uint32_t)di;
            vi = arg(c, in, 2); val = arg(c, in, 3);
        } else { fail(c, "%s not supported", name); return r; }
        if (val->bit_size == 1) val = nir_b2i32(b, val);
        if (val->bit_size != 32) val = is_float(c, c->f->values[in->ops[slot ? 4 : 3]].type) ? nir_f2f32(b, val) : nir_u2u32(b, val);
        if (val->num_components < 4) val = nir_pad_vector_imm_int(b, val, 0, 4);
        nir_def *off = nir_iadd_imm(b, nir_imul_imm(b, nir_u2u32(b, vi), abi->mesh_vstride), 16 + 16 * slot);
        nir_store_global(b, val, nir_iadd(b, c->mesh_out, nir_u2u64(b, off)), .align_mul = 16);
        return r;
    }

    /* barriers */
    if (!strcmp(name, "air.wg.barrier")) {
        int64_t fl = 3;
        const_int(c, in->ops[1], &fl);
        barrier(c, (uint32_t)fl, SCOPE_WORKGROUP);
        return r;
    }
    if (!strcmp(name, "air.simdgroup.barrier")) {
        int64_t fl = 3;
        const_int(c, in->ops[1], &fl);
        barrier(c, (uint32_t)fl, SCOPE_SUBGROUP);
        return r;
    }
    if (!strncmp(name, "air.mem_barrier", 15) || !strncmp(name, "air.mem.barrier", 15)) {
        barrier(c, 3, SCOPE_NONE);
        return r;
    }
    if (!strncmp(name, "air.convert.", 12)) { r.d = convert(c, name, arg(c, in, 0), in->type); return r; }
    /* OpenCL kernels (Apple's CL over Metal): work-item functions as calls
     * with a dimension, air.get_global_id.i32(dim) ... Past dimension 2
     * sizes read 1 and ids 0, as OpenCL says. No global offset (0). */
    if (!strncmp(name, "air.get_", 8) && c->sys_gid) {
        const char *q = name + 8;
        nir_def *vec = NULL;
        int one = 0;
        if (!strncmp(q, "global_id.", 10)) vec = nir_iadd(b, c->sys_gid, cl_global_offset(c));
        else if (!strncmp(q, "local_id.", 9)) vec = c->sys_lid;
        else if (!strncmp(q, "group_id.", 9)) vec = c->sys_wgid;
        else if (!strncmp(q, "local_size.", 11) || !strncmp(q, "enqueued_local_size.", 20)) { vec = c->sys_block; one = 1; }
        else if (!strncmp(q, "global_size.", 12)) { vec = c->sys_grid; one = 1; }   /* the enqueue's size, offset not included */
        else if (!strncmp(q, "num_groups.", 11)) {
            vec = nir_udiv(b, nir_iadd(b, c->sys_grid, nir_iadd_imm(b, c->sys_block, -1)), c->sys_block);
            one = 1;
        } else if (!strncmp(q, "global_offset.", 14)) vec = cl_global_offset(c);
        else if (!strncmp(q, "work_dim", 8) && c->buf_shift) {
            nir_def *wd = nir_load_global(b, 1, 32, nir_iadd_imm(b, push_u64(c, 0), 12), .align_mul = 4);
            r.d = in->type != ~0u && T(c, in->type)->bits == 64 ? nir_u2u64(b, wd) : wd;
            return r;
        } else if (!strncmp(q, "work_dim", 8)) {
            /* the highest dimension with more than one thread */
            nir_def *gy = nir_channel(b, c->sys_grid, 1), *gz = nir_channel(b, c->sys_grid, 2);
            nir_def *wd = nir_bcsel(b, nir_ugt_imm(b, gz, 1), nir_imm_int(b, 3),
                                    nir_bcsel(b, nir_ugt_imm(b, gy, 1), nir_imm_int(b, 2), nir_imm_int(b, 1)));
            r.d = in->type != ~0u && T(c, in->type)->bits == 64 ? nir_u2u64(b, wd) : wd;
            return r;
        }
        if (vec) {
            nir_def *dim = nir_u2u32(b, arg(c, in, 0));
            nir_def *v = nir_imm_int(b, one);
            for (int k = 2; k >= 0; --k) v = nir_bcsel(b, nir_ieq_imm(b, dim, k), nir_channel(b, vec, (unsigned)k), v);
            r.d = in->type != ~0u && T(c, in->type)->bits == 64 ? nir_u2u64(b, v) : v;
            return r;
        }
    }
    if (!strcmp(name, "air.discard_fragment")) { nir_demote(b); return r; }
    if (!strncmp(name, "air.dfdx", 8) || !strncmp(name, "air.dfdy", 8) || !strncmp(name, "air.fwidth", 10)) {
        nir_def *x = arg(c, in, 0);
        const unsigned bs = x->bit_size;
        if (bs != 32) x = nir_f2f32(b, x);            /* NAK derivatives are 32-bit only */
        r.d = name[4] == 'f' ? nir_fadd(b, nir_fabs(b, nir_ddx(b, x)), nir_fabs(b, nir_ddy(b, x)))
                             : name[7] == 'x' ? nir_ddx(b, x) : nir_ddy(b, x);
        if (bs != 32) r.d = nir_f2fN(b, r.d, bs);
        return r;
    }
    if (!strcmp(name, "air.is_function_constant_defined")) {
        uint32_t v = in->ops[1];
        for (int d = 0; d < 4 && c->f->values[v].kind == AV_CEXPR && c->f->values[v].n; ++d) v = c->f->values[v].elts[0];
        const air_value *x = &c->f->values[v];
        const int fc = x->kind == AV_GLOBAL ? fc_index(c->m->globals[x->ival].name) : -1;
        r.d = nir_imm_bool(b, fc >= 0 && (c->var->fc_defined[fc / 32] & (1u << (fc % 32))));
        return r;
    }
    if (!strncmp(name, "air.normalize_function_constant_predicate", 41)) {
        nir_def *x = arg(c, in, 0);
        r.d = nir_b2iN(b, nir_ine_imm(b, x, 0), x->bit_size);
        return r;
    }

    /* atomics: air.atomic.{global,local}.<op>.<s|u|f>.<type>(ptr, val, order, scope, volatile) */
    if (!strncmp(name, "air.atomic.", 11)) {
        char opn[32] = {0}, sk = 'u';
        const char *p = name + 11;
        const int local = !strncmp(p, "local.", 6);
        p = strchr(p, '.');
        if (!p) { fail(c, "%s", name); return r; }
        ++p;
        const char *e = strchr(p, '.');
        snprintf(opn, sizeof opn, "%.*s", e ? (int)(e - p) : (int)strlen(p), p);
        if (e && (e[1] == 's' || e[1] == 'u' || e[1] == 'f') && e[2] == '.') sk = e[1];
        const uint32_t as = local ? 3 : T(c, c->f->values[in->ops[1]].type)->addrspace;
        nir_def *ptr = arg(c, in, 0);
        if (!strcmp(opn, "load")) { r.d = mem_load(c, as, ptr, in->type == ~0u ? 32 : (T(c, in->type)->bits), 1, 4); return r; }
        if (!strcmp(opn, "store")) { mem_store(c, as, ptr, arg(c, in, 1), 4); return r; }
        if (!strncmp(opn, "cmpxchg", 7)) {
            /* (ptr, expected*, desired, ...) returns bool, updates *expected */
            const uint32_t eas = T(c, c->f->values[in->ops[2]].type)->addrspace;
            nir_def *expp = arg(c, in, 1);
            nir_def *expected = mem_load(c, eas, expp, 32, 1, 4);
            nir_def *old = atomic_swap(c, as, ptr, expected, arg(c, in, 2));
            mem_store(c, eas, expp, old, 4);
            r.d = nir_ieq(b, old, expected);
            return r;
        }
        nir_atomic_op op;
        if (atomic_op(opn, sk == 's', sk == 'f', &op)) { fail(c, "%s", name); return r; }
        nir_def *v = arg(c, in, 1);
        if (!strcmp(opn, "sub")) v = nir_ineg(b, v);
        r.d = atomic(c, as, ptr, v, op);
        return r;
    }

    if (strstr(name, "_texture_") || strstr(name, "_depth_2d") || strstr(name, "_depth_cube"))
        return texture_op(c, in, name);

    char base[64];
    int fast = 0;
    air_base(name, base, sizeof base, &fast);
    nir_def *a0 = na > 0 ? arg(c, in, 0) : NULL, *a1 = na > 1 ? arg(c, in, 1) : NULL;
    nir_def *a2 = na > 2 ? arg(c, in, 2) : NULL;
    const int f = in->type != ~0u && is_float(c, in->type);
    const char *sfx = strrchr(name, '.');
    const int sgn = strstr(name, ".s.") != NULL;

    if ((r.d = (na == 1 && f) ? math1(c, base, a0) : NULL)) return r;
    if (!strcmp(base, "fmax")) r.d = nir_fmax(b, a0, a1);
    else if (!strcmp(base, "fmin")) r.d = nir_fmin(b, a0, a1);
    else if (!strcmp(base, "fmax3")) r.d = nir_fmax(b, nir_fmax(b, a0, a1), a2);
    else if (!strcmp(base, "fmin3")) r.d = nir_fmin(b, nir_fmin(b, a0, a1), a2);
    else if (!strcmp(base, "fmedian3"))
        r.d = nir_fmax(b, nir_fmin(b, a0, a1), nir_fmin(b, nir_fmax(b, a0, a1), a2));
    else if (!strcmp(base, "clamp") && f) r.d = nir_fmin(b, nir_fmax(b, a0, a1), a2);
    else if (!strcmp(base, "clamp")) r.d = sgn ? nir_imin(b, nir_imax(b, a0, a1), a2) : nir_umin(b, nir_umax(b, a0, a1), a2);
    else if (!strcmp(base, "mix")) r.d = nir_flrp(b, a0, a1, a2);
    else if (!strcmp(base, "fma") || !strcmp(base, "fmuladd")) r.d = nir_ffma(b, a0, a1, a2);
    else if (!strcmp(base, "pow") || !strcmp(base, "powr")) r.d = nir_fpow(b, a0, a1);
    else if (!strcmp(base, "atan2")) r.d = nir_atan2(b, a0, a1);
    else if (!strcmp(base, "fmod")) r.d = nir_frem(b, a0, a1);
    else if (!strcmp(base, "copysign"))
        r.d = nir_bcsel(b, nir_flt_imm(b, a1, 0.0), nir_fneg(b, nir_fabs(b, a0)), nir_fabs(b, a0));
    else if (!strcmp(base, "step")) r.d = nir_b2fN(b, nir_fge(b, a1, a0), a1->bit_size);
    else if (!strcmp(base, "smoothstep")) {
        nir_def *t = nir_fsat(b, nir_fdiv(b, nir_fsub(b, a2, a0), nir_fsub(b, a1, a0)));
        r.d = nir_fmul(b, nir_fmul(b, t, t), nir_fsub(b, nir_imm_floatN_t(b, 3.0, t->bit_size), nir_fmul_imm(b, t, 2.0)));
    }
    else if (!strcmp(base, "dot")) r.d = nir_fdot(b, a0, a1);
    else if (!strcmp(base, "length")) r.d = nir_fsqrt(b, nir_fdot(b, a0, a0));
    else if (!strcmp(base, "distance")) { nir_def *d = nir_fsub(b, a0, a1); r.d = nir_fsqrt(b, nir_fdot(b, d, d)); }
    else if (!strcmp(base, "normalize")) r.d = nir_fmul(b, a0, nir_frsq(b, nir_fdot(b, a0, a0)));
    else if (!strcmp(base, "cross")) r.d = nir_cross3(b, a0, a1);
    else if (!strcmp(base, "ldexp")) r.d = nir_ldexp(b, a0, nir_i2i32(b, a1));
    else if (!strcmp(base, "sincos")) {
        /* (x, cos out*) returns sin */
        r.d = nir_fsin(b, a0);
        mem_store(c, T(c, c->f->values[in->ops[2]].type)->addrspace, a1, nir_fcos(b, a0), a0->bit_size / 8);
    }
    else if (!strcmp(base, "all")) r.d = reduce_bool(b, a0, 1);
    else if (!strcmp(base, "any")) r.d = reduce_bool(b, a0, 0);
    else if (!strcmp(base, "select")) r.d = nir_bcsel(b, a2, a1, a0);
    else if (!strcmp(base, "abs")) r.d = f ? nir_fabs(b, a0) : nir_iabs(b, a0);
    else if (!strcmp(base, "max")) r.d = f ? nir_fmax(b, a0, a1) : sgn ? nir_imax(b, a0, a1) : nir_umax(b, a0, a1);
    else if (!strcmp(base, "min")) r.d = f ? nir_fmin(b, a0, a1) : sgn ? nir_imin(b, a0, a1) : nir_umin(b, a0, a1);
    else if (!strcmp(base, "clz")) r.d = nir_u2uN(b, nir_uclz(b, a0), a0->bit_size);
    else if (!strcmp(base, "popcount")) r.d = nir_u2uN(b, nir_bit_count(b, a0), a0->bit_size);
    else if (!strcmp(base, "reverse_bits")) r.d = nir_bitfield_reverse(b, a0);
    else if (!strcmp(base, "rotate")) {
        /* left rotate by (n mod bits); NIR masks shift counts to the bit
         * size, so n == 0 gives x | x (Core Image's portrait blur noise) */
        const unsigned bs = a0->bit_size;
        nir_def *n = nir_iand_imm(b, nir_u2u32(b, a1), bs - 1);
        r.d = nir_ior(b, nir_ishl(b, a0, n), nir_ushr(b, a0, nir_isub(b, nir_imm_int(b, (int)bs), n)));
    }
    else if (!strcmp(base, "mul_hi")) r.d = sgn ? nir_imul_high(b, a0, a1) : nir_umul_high(b, a0, a1);
    else if (!strcmp(base, "add_sat")) r.d = sgn ? nir_iadd_sat(b, a0, a1) : nir_uadd_sat(b, a0, a1);
    else if (!strcmp(base, "sub_sat")) r.d = sgn ? nir_isub_sat(b, a0, a1) : nir_usub_sat(b, a0, a1);
    else if (!strcmp(base, "absdiff"))
        r.d = sgn ? nir_isub(b, nir_imax(b, a0, a1), nir_imin(b, a0, a1)) : nir_isub(b, nir_umax(b, a0, a1), nir_umin(b, a0, a1));
    /* simd group */
    else if (!strcmp(base, "simd_sum")) r.d = nir_reduce(b, a0, .reduction_op = f ? nir_op_fadd : nir_op_iadd);
    else if (!strcmp(base, "simd_product")) r.d = nir_reduce(b, a0, .reduction_op = f ? nir_op_fmul : nir_op_imul);
    else if (!strcmp(base, "simd_max")) r.d = nir_reduce(b, a0, .reduction_op = f ? nir_op_fmax : sgn ? nir_op_imax : nir_op_umax);
    else if (!strcmp(base, "simd_min")) r.d = nir_reduce(b, a0, .reduction_op = f ? nir_op_fmin : sgn ? nir_op_imin : nir_op_umin);
    else if (!strcmp(base, "simd_and")) r.d = nir_reduce(b, a0, .reduction_op = nir_op_iand);
    else if (!strcmp(base, "simd_or")) r.d = nir_reduce(b, a0, .reduction_op = nir_op_ior);
    else if (!strcmp(base, "simd_xor")) r.d = nir_reduce(b, a0, .reduction_op = nir_op_ixor);
    else if (!strcmp(base, "simd_prefix_exclusive_sum"))
        r.d = nir_exclusive_scan(b, a0, .reduction_op = f ? nir_op_fadd : nir_op_iadd);
    else if (!strcmp(base, "simd_prefix_inclusive_sum"))
        r.d = nir_inclusive_scan(b, a0, .reduction_op = f ? nir_op_fadd : nir_op_iadd);
    else if (!strcmp(base, "simd_shuffle")) r.d = nir_shuffle(b, a0, nir_u2u32(b, a1));
    else if (!strcmp(base, "simd_shuffle_xor")) r.d = nir_shuffle_xor(b, a0, nir_u2u32(b, a1));
    else if (!strcmp(base, "simd_shuffle_up")) r.d = nir_shuffle_up(b, a0, nir_u2u32(b, a1));
    else if (!strcmp(base, "simd_shuffle_down")) r.d = nir_shuffle_down(b, a0, nir_u2u32(b, a1));
    else if (!strcmp(base, "simd_broadcast")) r.d = nir_read_invocation(b, a0, nir_u2u32(b, a1));
    else if (!strcmp(base, "simd_broadcast_first")) r.d = nir_read_first_invocation(b, a0);
    else if (!strcmp(base, "simd_ballot")) {
        nir_def *m = nir_ballot(b, 1, 32, a0->bit_size == 1 ? a0 : nir_ine_imm(b, a0, 0));
        r.d = in->type != ~0u && T(c, in->type)->kind == AT_INT && T(c, in->type)->bits == 64 ? nir_u2u64(b, m) : m;
    }
    else if (!strcmp(base, "simd_all")) r.d = nir_vote_all(b, 1, a0);
    else if (!strcmp(base, "simd_any")) r.d = nir_vote_any(b, 1, a0);
    else if (!strcmp(base, "simd_is_first")) r.d = nir_elect(b, 1);
    else if (!strcmp(base, "is_uniform")) {
        /* the same value in every active thread of the simdgroup */
        nir_def *e = nir_read_first_invocation(b, a0);
        nir_def *same = reduce_bool(b, a0->bit_size == 1 ? nir_ieq(b, a0, e) : f ? nir_feq(b, a0, e) : nir_ieq(b, a0, e), 1);
        r.d = nir_vote_all(b, 1, same);
    }
    else if (!strcmp(base, "unpack") && !strstr(name, "rgb10a2")) {
        /* air.unpack.unorm4x8 / snorm4x8 / unorm2x16 / snorm2x16 */
        nir_def *v = nir_u2u32(b, a0);
        if (strstr(name, "unorm4x8")) r.d = nir_unpack_unorm_4x8(b, v);
        else if (strstr(name, "snorm4x8")) r.d = nir_unpack_snorm_4x8(b, v);
        else if (strstr(name, "unorm2x16")) r.d = nir_unpack_unorm_2x16(b, v);
        else if (strstr(name, "snorm2x16")) r.d = nir_unpack_snorm_2x16(b, v);
        else fail(c, "intrinsic %s", name);
        if (r.d && in->type != ~0u) {
            uint32_t bits, comps;
            if (!scalar_shape(c, in->type, &bits, &comps) && bits != 32) r.d = nir_f2fN(b, r.d, bits);
        }
    }
    else if (!strcmp(base, "pack")) {
        nir_def *v = nir_f2f32(b, a0);
        if (strstr(name, "unorm4x8")) r.d = nir_pack_unorm_4x8(b, v);
        else if (strstr(name, "snorm4x8")) r.d = nir_pack_snorm_4x8(b, v);
        else if (strstr(name, "unorm2x16")) r.d = nir_pack_unorm_2x16(b, v);
        else if (strstr(name, "snorm2x16")) r.d = nir_pack_snorm_2x16(b, v);
        else fail(c, "intrinsic %s", name);
    }
    else if (!strcmp(base, "get_simdgroup_size") || !strcmp(base, "get_threads_per_simdgroup"))
        r.d = imm(c, 32, in->type != ~0u ? T(c, in->type)->bits : 32);
    else if (!strcmp(base, "simd_shuffle_and_fill_up") || !strcmp(base, "simd_shuffle_and_fill_down")) {
        /* (data, filling_data, delta[, modulo]): lanes past the edge take filling_data */
        const int up = strstr(base, "_up") != NULL;
        nir_def *delta = nir_u2u32(b, a2);
        nir_def *lane = nir_load_subgroup_invocation(b);
        nir_def *mod = na > 3 ? nir_u2u32(b, arg(c, in, 3)) : nir_imm_int(b, 32);
        nir_def *pos = nir_umod(b, lane, mod);
        nir_def *base_lane = nir_isub(b, lane, pos);
        nir_def *src = up ? nir_isub(b, pos, delta) : nir_iadd(b, pos, delta);
        nir_def *inside = up ? nir_uge(b, pos, delta) : nir_ult(b, src, mod);
        nir_def *wrap = up ? nir_iadd(b, src, mod) : nir_isub(b, src, mod);
        nir_def *from = nir_iadd(b, base_lane, nir_bcsel(b, inside, src, wrap));
        nir_def *v0 = nir_shuffle(b, a0, from), *v1 = nir_shuffle(b, a1, from);
        r.d = nir_bcsel(b, inside, v0, v1);
    }
    else if (!strcmp(base, "unpack") && strstr(name, "rgb10a2")) {
        nir_def *v = nir_u2u32(b, a0);
        nir_def *ch[4];
        for (int q = 0; q < 3; ++q) ch[q] = nir_fmul_imm(b, nir_u2f32(b, nir_ubitfield_extract_imm(b, v, q * 10, 10)), 1.0 / 1023.0);
        ch[3] = nir_fmul_imm(b, nir_u2f32(b, nir_ushr_imm(b, v, 30)), 1.0 / 3.0);
        r.d = nir_vec(b, ch, 4);
        uint32_t ub, uc;
        if (!scalar_shape(c, in->type, &ub, &uc) && ub != 32) r.d = nir_f2fN(b, r.d, ub);
    }
    else if (!strcmp(base, "quad_shuffle_down")) r.d = nir_shuffle_down(b, a0, nir_u2u32(b, a1));
    else if (!strcmp(base, "quad_shuffle_up")) r.d = nir_shuffle_up(b, a0, nir_u2u32(b, a1));
    else if (!strcmp(base, "quad_shuffle_xor")) r.d = nir_shuffle_xor(b, a0, nir_u2u32(b, a1));
    else if (!strcmp(base, "quad_shuffle"))
        r.d = nir_shuffle(b, a0, nir_ior(b, nir_iand_imm(b, nir_load_subgroup_invocation(b), ~3), nir_iand_imm(b, nir_u2u32(b, a1), 3)));
    else if (!strcmp(base, "quad_broadcast"))
        r.d = nir_shuffle(b, a0, nir_ior(b, nir_iand_imm(b, nir_load_subgroup_invocation(b), ~3), nir_iand_imm(b, nir_u2u32(b, a1), 3)));
    else if (!strcmp(base, "ctz")) {
        nir_def *t = nir_find_lsb(b, a0);
        r.d = nir_u2uN(b, nir_bcsel(b, nir_ilt_imm(b, t, 0), nir_imm_int(b, (int)a0->bit_size), t), a0->bit_size);
    }
    else fail(c, "intrinsic %s", name);
    (void)fast; (void)sfx;
    return r;
}

/* ---- kernel signature ---- */
typedef struct { int32_t node; } argmd;

static const air_md *md(ctx *c, int32_t id) {
    return id >= 0 && (uint32_t)id < c->m->nmd ? &c->m->md[id] : NULL;
}

/* the entry point node of f: kernel {fn, -, args}, vertex/fragment {fn, outputs, inputs} */
static int find_entry(ctx *c, mesa_shader_stage *stage, int32_t *args_node, int32_t *outs_node) {
    static const struct { const char *name; mesa_shader_stage st; } kinds[] = {
        {"air.kernel", MESA_SHADER_COMPUTE}, {"air.vertex", MESA_SHADER_VERTEX}, {"air.fragment", MESA_SHADER_FRAGMENT},
        {"air.object", MESA_SHADER_COMPUTE}, {"air.mesh", MESA_SHADER_COMPUTE}};
    for (unsigned kk = 0; kk < 5; ++kk) {
        const air_named_md *k = air_named(c->m, kinds[kk].name);
        for (uint32_t i = 0; k && i < k->n; ++i) {
            const air_md *n = md(c, (int32_t)k->ops[i]);
            if (!n || n->kind != AM_NODE || n->n < 3) continue;
            const air_md *fv = md(c, n->ops[0]);
            if (!fv || fv->kind != AM_VALUE || fv->value != c->f->value) continue;
            *stage = kinds[kk].st;
            *args_node = n->ops[2];
            *outs_node = kk == 1 || kk == 2 ? n->ops[1] : -1;
            if (kk >= 3) c->abi->mesh_kind = kk - 2;
            /* [air.patch triangle|quad air.patch_control_point N]: a
             * post-tessellation vertex function -> tessellation evaluation */
            for (uint32_t e = 3; kk == 1 && e < n->n; ++e) {
                const air_md *x = md(c, n->ops[e]);
                if (!x || x->kind != AM_NODE) continue;
                for (uint32_t j = 0; j + 1 < x->n; ++j) {
                    const char *s1 = air_md_string(c->m, x->ops[j]);
                    if (!s1) continue;
                    if (!strcmp(s1, "air.patch")) {
                        const char *d = air_md_string(c->m, x->ops[j + 1]);
                        c->abi->tess_domain = d && !strcmp(d, "quad") ? 2 : 1;
                        *stage = MESA_SHADER_TESS_EVAL;
                    } else if (!strcmp(s1, "air.patch_control_point")) {
                        int64_t v = 0;
                        if (!air_md_int(c->m, x->ops[j + 1], &v)) c->abi->tess_cps = (uint32_t)v;
                    }
                }
            }
            return 0;
        }
    }
    return -1;
}

static int find_kernel_node(ctx *c, int32_t *args_node) {
    mesa_shader_stage st;
    int32_t outs;
    return find_entry(c, &st, args_node, &outs);
}

/* varying slot of an interface name (generated(...) / user(...)): its index
 * in the pipeline's list, -1 when the other stage does not have it */
static int varying_slot(ctx *c, const char *name) {
    const char *list = c->var->io ? c->var->io : c->io_auto;
    if (!list || !name) return -1;
    const size_t n = strlen(name);
    const char *p = list;
    for (int i = 0; *p; ++i) {
        const char *e = strchr(p, ',');
        const size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == n && !strncmp(p, name, n)) return i < 32 ? VARYING_SLOT_VAR0 + i : -1;
        if (!e) break;
        p = e + 1;
    }
    return -1;
}

/* the string after a key in an entry node, e.g. the name after air.vertex_output */
static const char *arg_str_after(ctx *c, const air_md *n, const char *key) {
    for (uint32_t i = 0; i + 1 < n->n; ++i) {
        const char *s = air_md_string(c->m, n->ops[i]);
        if (s && !strcmp(s, key)) return air_md_string(c->m, n->ops[i + 1]);
    }
    return NULL;
}

static int arg_has(ctx *c, const air_md *n, const char *key) {
    for (uint32_t i = 0; i < n->n; ++i) {
        const char *s = air_md_string(c->m, n->ops[i]);
        if (s && !strcmp(s, key)) return 1;
    }
    return 0;
}

/* the entry kind string of an argument/output node (skipping a function-constant guard) */
static const char *node_kind(ctx *c, const air_md *a, uint32_t first) {
    const char *k = first < a->n ? air_md_string(c->m, a->ops[first]) : NULL;
    if (k && !strcmp(k, "air.function_constant") && first + 2 < a->n) k = air_md_string(c->m, a->ops[first + 2]);
    return k;
}

/* key-value lookup in an argument node: {i32 index, !"air.kind", ...} */
/* An attribute index written as an expression ([[texture(kBase + 1)]] with
 * function constants, MPS does this for every image argument) is a global
 * __metal_implicit_attr_int_expr_N that the file's ctor sets: read it from
 * cdata, where run_ctors() put this pipeline's value, or its initialiser. */
static int md_int_or_global(ctx *c, int32_t id, int64_t *out) {
    if (!air_md_int(c->m, id, out)) return 0;
    if (id < 0 || (uint32_t)id >= c->m->nmd || c->m->md[id].kind != AM_VALUE) return -1;
    const uint32_t v = c->m->md[id].value;
    if (v >= c->m->nvalues || c->m->values[v].kind != AV_GLOBAL) return -1;
    const uint32_t g = (uint32_t)c->m->values[v].ival;
    if (g >= c->m->nglobals) return -1;
    const air_global *gl = &c->m->globals[g];
    const uint32_t n = store_size(c, gl->value_type);
    if (n == 0 || n > 8) return -1;
    if (c->gused && c->gused[g] && c->abi->cdata && gl->addrspace != 3) {
        uint64_t x = 0;
        for (uint32_t k = 0; k < n; ++k) x |= (uint64_t)c->abi->cdata[c->global_off[g] + k] << (8 * k);
        *out = n < 8 ? (int64_t)(x << (64 - 8 * n)) >> (64 - 8 * n) : (int64_t)x;
        return 0;
    }
    if (gl->init) {
        const air_value *iv = &c->m->values[gl->init - 1];
        if (iv->kind == AV_CINT) { *out = (int64_t)iv->ival; return 0; }
        if (iv->kind == AV_CNULL) { *out = 0; return 0; }
    }
    return -1;
}
static int arg_int_after(ctx *c, const air_md *n, const char *key, int64_t *out) {
    for (uint32_t i = 0; i + 1 < n->n; ++i) {
        const char *s = air_md_string(c->m, n->ops[i]);
        if (s && !strcmp(s, key)) return md_int_or_global(c, n->ops[i + 1], out);
    }
    return -1;
}

/* ---- vertex / fragment stage interface ---- */
static nir_variable *io_var(ctx *c, nir_variable_mode mode, int slot, uint32_t comps, int is_int,
                            const char *name) {
    const struct glsl_type *t = glsl_vector_type(is_int ? GLSL_TYPE_UINT : GLSL_TYPE_FLOAT, comps);
    nir_variable *v = nir_variable_create(c->s, mode, t, name);
    v->data.location = slot;
    return v;
}

static nir_def *fit_bits(ctx *c, nir_def *v, uint32_t t) {
    uint32_t bits, comps;
    if (scalar_shape(c, t, &bits, &comps)) return v;
    if (comps < v->num_components) v = nir_trim_vector(&c->b, v, comps);
    if (bits == v->bit_size) return v;
    if (bits == 1) return nir_ine_imm(&c->b, v, 0);
    return is_float(c, t) ? nir_f2fN(&c->b, v, bits) : nir_u2uN(&c->b, v, bits);
}

static int gfx_input(ctx *c, const air_md *a, const char *kind, uint32_t t, tval *out) {
    nir_builder *b = &c->b;
    uint32_t bits = 32, comps = 1;
    scalar_shape(c, t, &bits, &comps);
    const int integer = !is_float(c, t);
    if (c->stage == MESA_SHADER_TESS_EVAL) {
        if (!strcmp(kind, "air.patch_control_point_input")) {
            c->cp_node = (int32_t)(a - c->m->md);      /* the members' locations are read at each fetch */
            *out = zero_of(c, t);
            return 1;
        }
        if (!strcmp(kind, "air.position_in_patch")) { out->d = fit_bits(c, nir_load_tess_coord(b), t); return 1; }
        if (!strcmp(kind, "air.patch_id")) {
            /* the runtime passes each drawn patch's id in a list (push u64
             * after base vertex/instance and patchStart): patchStart + i, or
             * the patch index buffer's entries */
            nir_def *prim = nir_load_primitive_id(b);
            nir_def *addr = nir_iadd(b, push_u64(c, c->abi->grid_offset + 16), nir_u2u64(b, nir_imul_imm(b, prim, 4)));
            out->d = fit_bits(c, nir_load_global(b, 1, 32, addr, .align_mul = 4), t);
            return 1;
        }
        return 0;
    }
    if (c->stage == MESA_SHADER_VERTEX) {
        if (!strcmp(kind, "air.vertex_input")) {
            int64_t loc = 0;
            arg_int_after(c, a, "air.location_index", &loc);
            nir_variable *v = io_var(c, nir_var_shader_in, VERT_ATTRIB_GENERIC0 + (int)loc, comps, integer, "attr");
            out->d = fit_bits(c, nir_load_var(b, v), t);
            return 1;
        }
        /* base vertex / instance come in the push block (grid words' place) */
        const uint32_t g = c->abi->grid_offset;
        if (!strcmp(kind, "air.vertex_id")) { out->d = fit_bits(c, nir_load_vertex_id(b), t); return 1; }
        if (!strcmp(kind, "air.instance_id")) {
            out->d = fit_bits(c, nir_iadd(b, nir_load_instance_id(b), push_u32(c, g + 4)), t);
            return 1;
        }
        if (!strcmp(kind, "air.base_vertex")) { out->d = fit_bits(c, push_u32(c, g), t); return 1; }
        /* one view, no amplification */
        if (!strcmp(kind, "air.amplification_id") || !strcmp(kind, "air.amplification_count")) {
            *out = zero_of(c, t);
            if (!strcmp(kind, "air.amplification_count")) out->d = fit_bits(c, nir_imm_int(b, 1), t);
            return 1;
        }
        if (!strcmp(kind, "air.base_instance")) { out->d = fit_bits(c, push_u32(c, g + 4), t); return 1; }
        return 0;
    }
    /* fragment */
    if (!strcmp(kind, "air.position")) { out->d = nir_load_frag_coord(b); return 1; }
    /* [[barycentric_coord]] */
    if (!strcmp(kind, "air.barycentric_coord")) {
        const enum glsl_interp_mode im = arg_has(c, a, "air.no_perspective") ? INTERP_MODE_NOPERSPECTIVE : INTERP_MODE_SMOOTH;
        nir_def *bc = arg_has(c, a, "air.centroid") ? nir_load_barycentric_coord_centroid(b, 32, .interp_mode = im)
                    : arg_has(c, a, "air.sample") ? nir_load_barycentric_coord_sample(b, 32, .interp_mode = im)
                    : nir_load_barycentric_coord_pixel(b, 32, .interp_mode = im);
        out->d = fit_fvec(c, bc, t);
        return 1;
    }
    if (!strcmp(kind, "air.fragment_input")) {
        const int slot = varying_slot(c, arg_str_after(c, a, "air.fragment_input"));
        const int pull = arg_has(c, a, "air.interpolation_function");
        if (slot < 0 && !pull) { *out = zero_of(c, t); return 1; }    /* the vertex stage does not write it */
        if (pull) {
            /* interpolant<T, mode>: the element type comes from the type name */
            const char *tn = arg_str_after(c, a, "air.arg_type_name");
            uint32_t n = 4;
            if (tn) { const size_t l = strlen(tn); if (l && tn[l - 1] >= '2' && tn[l - 1] <= '4') n = (uint32_t)(tn[l - 1] - '0'); else n = 1; }
            if (c->ninterp >= 32) { fail(c, "too many interpolants"); return 1; }
            nir_variable *v = slot >= 0 ? io_var(c, nir_var_shader_in, slot, n, 0, "interpolant") : NULL;
            if (v) v->data.interpolation = arg_has(c, a, "air.no_perspective") ? INTERP_MODE_NOPERSPECTIVE : INTERP_MODE_SMOOTH;
            c->interp_var[c->ninterp] = v;
            out->d = nir_imm_int64(b, (int64_t)c->ninterp++);
            return 1;
        }
        nir_variable *v = io_var(c, nir_var_shader_in, slot, comps, integer, "varying");
        v->data.interpolation = arg_has(c, a, "air.flat") || integer ? INTERP_MODE_FLAT
                              : arg_has(c, a, "air.no_perspective") ? INTERP_MODE_NOPERSPECTIVE
                              : INTERP_MODE_SMOOTH;
        v->data.centroid = arg_has(c, a, "air.centroid");
        v->data.sample = arg_has(c, a, "air.sample");
        if (v->data.sample) b->shader->info.fs.uses_sample_shading = true;
        out->d = fit_bits(c, nir_load_var(b, v), t);
        return 1;
    }
    if (!strcmp(kind, "air.front_facing")) { out->d = fit_bits(c, nir_b2i32(b, nir_load_front_face(b, 1)), t); return 1; }
    if (!strcmp(kind, "air.point_coord")) {         /* NAK lowers load_point_coord itself */
        out->d = fit_bits(c, nir_load_point_coord(b), t);
        return 1;
    }
    if (!strcmp(kind, "air.sample_id")) {
        b->shader->info.fs.uses_sample_shading = true;
        out->d = fit_bits(c, nir_load_sample_id(b), t);
        return 1;
    }
    if (!strcmp(kind, "air.sample_mask") || !strcmp(kind, "air.sample_mask_in")) {
        out->d = fit_bits(c, nir_load_sample_mask_in(b), t);
        return 1;
    }
    /* the slice / viewport the vertex stage picked */
    if (!strcmp(kind, "air.render_target_array_index")) { out->d = fit_bits(c, nir_load_layer_id(b), t); return 1; }
    if (!strcmp(kind, "air.viewport_array_index")) {
        nir_variable *v = io_var(c, nir_var_shader_in, VARYING_SLOT_VIEWPORT, 1, 1, "viewport");
        v->data.interpolation = INTERP_MODE_FLAT;
        out->d = fit_bits(c, nir_load_var(b, v), t);
        return 1;
    }
    if (!strcmp(kind, "air.viewport_array_index") || !strcmp(kind, "air.render_target_array_index") ||
        !strcmp(kind, "air.amplification_id")) {
        *out = zero_of(c, t);                          /* single viewport / layer / view */
        return 1;
    }
    if (!strcmp(kind, "air.amplification_count")) { out->d = fit_bits(c, nir_imm_int(b, 1), t); return 1; }
    if (!strcmp(kind, "air.primitive_id")) { out->d = fit_bits(c, nir_load_primitive_id(b), t); return 1; }
    if (!strcmp(kind, "air.render_target")) {
        /* framebuffer fetch: attachment n as a texture, read at this pixel. The runtime puts its header
         * index in the push block and orders the draw after the earlier ones' colour writes. */
        int64_t n = 0;
        arg_int_after(c, a, "air.render_target", &n);
        if (n < 0 || n >= 8 || !(c->abi->rt_read_mask & (1u << n)) || !c->abi->rt_read_offset) {
            fail(c, "colour attachment input %lld", (long long)n);
            return 1;
        }
        uint32_t bits;
        const nir_alu_type ty = tex_type(c, t, &bits);
        const tex_kind k = { GLSL_SAMPLER_DIM_2D, 2, false, false, false };
        nir_def *xy = nir_f2u32(b, nir_trim_vector(b, nir_load_frag_coord(b), 2));
        nir_def *v = emit_tex2(c, nir_texop_txf, &k, tex_handle(c, push_u64(c, c->abi->rt_read_offset + (uint32_t)n * 8), NULL),
                               xy, NULL, nir_imm_int(b, 0), NULL, NULL, NULL, NULL, NULL, 0, ty, 4);
        *out = tex_result(c, t, v);
        return 1;
    }
    return 0;
}

/* the entry's return value -> stage outputs, per the !air.* outputs list */
static nir_variable *gOutVar[64];
static void emit_stage_outputs(ctx *c, tval val, uint32_t rett) {
    const air_md *outs = md(c, c->outputs_node);
    if (!outs || outs->kind != AM_NODE) return;
    const air_type *rt = T(c, rett);
    for (uint32_t i = 0; i < outs->n && i < 64; ++i) {
        const air_md *o = md(c, outs->ops[i]);
        if (!o || o->kind != AM_NODE) continue;
        const char *kind = node_kind(c, o, 0);
        tval mv = rt->kind == AT_STRUCT ? (val.m && i < val.n ? val.m[i] : (tval){0}) : val;
        const uint32_t mt = rt->kind == AT_STRUCT ? rt->members[i] : rett;
        if (!mv.d || !kind) continue;
        int slot = -1;
        uint32_t comps = 4;
        int integer = !is_float(c, mt);
        if (c->stage == MESA_SHADER_VERTEX || c->stage == MESA_SHADER_TESS_EVAL) {
            if (!strcmp(kind, "air.position")) slot = VARYING_SLOT_POS;
            else if (!strcmp(kind, "air.point_size")) { slot = VARYING_SLOT_PSIZ; comps = 1; }
            /* layered rendering / viewport arrays: the per-primitive slice and
             * viewport (NAK: RT_ARRAY_INDEX / VIEWPORT_INDEX attributes) */
            else if (!strcmp(kind, "air.render_target_array_index")) { slot = VARYING_SLOT_LAYER; comps = 1; integer = 1; }
            else if (!strcmp(kind, "air.viewport_array_index")) { slot = VARYING_SLOT_VIEWPORT; comps = 1; integer = 1; }
            else if (!strcmp(kind, "air.vertex_output")) slot = varying_slot(c, arg_str_after(c, o, "air.vertex_output"));
            if (slot >= 0 && slot != VARYING_SLOT_POS && slot != VARYING_SLOT_PSIZ && slot != VARYING_SLOT_LAYER &&
                slot != VARYING_SLOT_VIEWPORT) comps = mv.d->num_components;
        } else {
            if (!strcmp(kind, "air.render_target")) {
                int64_t n = 0;
                arg_int_after(c, o, "air.render_target", &n);
                slot = FRAG_RESULT_DATA0 + (int)n;
            } else if (!strcmp(kind, "air.depth")) { slot = FRAG_RESULT_DEPTH; comps = 1; integer = 0; }
            else if (!strcmp(kind, "air.sample_mask")) { slot = FRAG_RESULT_SAMPLE_MASK; comps = 1; integer = 1; }
        }
        if (slot < 0) continue;
        nir_def *d = mv.d;
        if (d->bit_size == 1) d = nir_b2i32(&c->b, d);
        if (d->bit_size != 32) d = integer ? nir_u2u32(&c->b, d) : nir_f2f32(&c->b, d);
        if (d->num_components < comps) d = nir_pad_vector_imm_int(&c->b, d, 0, comps);
        if (d->num_components > comps) d = nir_trim_vector(&c->b, d, comps);
        if (!gOutVar[i]) gOutVar[i] = io_var(c, nir_var_shader_out, slot, comps, integer, "out");
        nir_store_var(&c->b, gOutVar[i], d, nir_component_mask(comps));
    }
}

/* the element count after air.location_index's index (buffer arrays) */
static int arg_count_after(ctx *c, const air_md *n, int64_t *out) {
    for (uint32_t i = 0; i + 2 < n->n; ++i) {
        const char *s = air_md_string(c->m, n->ops[i]);
        if (s && !strcmp(s, "air.location_index")) return md_int_or_global(c, n->ops[i + 2], out);
    }
    return -1;
}

/* ---- compute [[stage_in]] ----
 * Each member of the stage_in struct is its own kernel argument
 * (air.stage_in + air.location_index = the attribute). Fetched in the shader
 * from the buffer the stageInputDescriptor names: index from the thread
 * position (step function), then address = buffer + index * stride + offset,
 * then the format's conversion. Missing components read as (0, 0, 0, 1). */
typedef struct { uint8_t comps, bytes, kind; } si_format;   /* kind: 0 u 1 s 2 unorm 3 snorm 4 half 5 float */
static int si_decode(uint32_t f, si_format *o) {
    static const si_format t[] = {
        [1] = {2, 1, 0}, [2] = {3, 1, 0}, [3] = {4, 1, 0}, [4] = {2, 1, 1}, [5] = {3, 1, 1}, [6] = {4, 1, 1},
        [7] = {2, 1, 2}, [8] = {3, 1, 2}, [9] = {4, 1, 2}, [10] = {2, 1, 3}, [11] = {3, 1, 3}, [12] = {4, 1, 3},
        [13] = {2, 2, 0}, [14] = {3, 2, 0}, [15] = {4, 2, 0}, [16] = {2, 2, 1}, [17] = {3, 2, 1}, [18] = {4, 2, 1},
        [19] = {2, 2, 2}, [20] = {3, 2, 2}, [21] = {4, 2, 2}, [22] = {2, 2, 3}, [23] = {3, 2, 3}, [24] = {4, 2, 3},
        [25] = {2, 2, 4}, [26] = {3, 2, 4}, [27] = {4, 2, 4},
        [28] = {1, 4, 5}, [29] = {2, 4, 5}, [30] = {3, 4, 5}, [31] = {4, 4, 5},
        [32] = {1, 4, 1}, [33] = {2, 4, 1}, [34] = {3, 4, 1}, [35] = {4, 4, 1},
        [36] = {1, 4, 0}, [37] = {2, 4, 0}, [38] = {3, 4, 0}, [39] = {4, 4, 0},
        [45] = {1, 1, 0}, [46] = {1, 1, 1}, [47] = {1, 1, 2}, [48] = {1, 1, 3},
        [49] = {1, 2, 0}, [50] = {1, 2, 1}, [51] = {1, 2, 2}, [52] = {1, 2, 3}, [53] = {1, 2, 4},
    };
    if (f == 42) { *o = (si_format){4, 1, 2}; return 1; }   /* uchar4 normalized BGRA: swizzled below */
    if (f >= sizeof(t) / sizeof(t[0]) || !t[f].comps) return 0;
    *o = t[f];
    return 1;
}

static nir_def *stage_in_fetch(ctx *c, int64_t loc, uint32_t t) {
    nir_builder *b = &c->b;
    const air_variant *var = c->var;
    si_format f;
    if (loc < 0 || loc >= 31) { fail(c, "stage_in attribute %lld not described", (long long)loc); return NULL; }
    if (!var->si_fmt[loc]) {
        /* An attribute the descriptor leaves out may be read only behind a
         * function constant that the shader keeps in a global (VFX
         * copy_generic: normal only when morphNormal), so it cannot be folded
         * here. The driver refuses the pipeline when the specialized function
         * reports the attribute active, as the M1 does; otherwise it is never
         * read and zero stands in. */
        return fit_bits(c, nir_imm_zero(b, 4, 32), t);
    }
    if (!si_decode(var->si_fmt[loc], &f)) { fail(c, "stage_in format %u", var->si_fmt[loc]); return NULL; }
    const uint32_t bi = var->si_buf[loc];
    const uint32_t step = var->sb_step[bi], rate = var->sb_rate[bi] ? var->sb_rate[bi] : 1;
    nir_def *idx;
    if (step == 0) idx = nir_imm_int(b, 0);                       /* constant */
    else if (step >= 5 && step <= 8) {                            /* thread position in grid X/Y (indexed) */
        idx = nir_channel(b, c->sys_gid, step == 5 || step == 7 ? 0 : 1);
        if (step >= 7) {
            if (!var->sx_given) { fail(c, "stage_in indexed step without an index buffer"); return NULL; }
            const uint32_t isz = var->sx_type ? 4 : 2;
            nir_def *ia = nir_iadd(b, push_u64(c, var->sx_buf * 8u), nir_u2u64(b, nir_imul_imm(b, idx, isz)));
            idx = nir_u2u32(b, nir_load_global(b, 1, isz * 8, ia, .align_mul = isz));
        }
        if (rate > 1) idx = nir_udiv_imm(b, idx, rate);
    } else { fail(c, "stage_in step function %u", step); return NULL; }
    nir_def *base = nir_iadd(b, push_u64(c, bi * 8u),
                             nir_u2u64(b, nir_iadd_imm(b, nir_imul_imm(b, idx, var->sb_stride[bi]), var->si_off[loc])));
    const uint32_t bits = f.bytes * 8;
    nir_def *e[4];
    for (uint32_t i = 0; i < 4; ++i) {
        if (i >= f.comps) {
            e[i] = f.kind >= 2 ? nir_imm_float(b, i == 3 ? 1.0f : 0.0f) : nir_imm_int(b, i == 3 ? 1 : 0);
            continue;
        }
        nir_def *r = nir_load_global(b, 1, bits, nir_iadd_imm(b, base, i * f.bytes), .align_mul = f.bytes);
        const double maxv = f.kind == 2 ? (double)((1ull << bits) - 1) : (double)((1ull << (bits - 1)) - 1);
        switch (f.kind) {
        case 0: e[i] = nir_u2u32(b, r); break;
        case 1: e[i] = nir_i2i32(b, r); break;
        case 2: e[i] = nir_fmul_imm(b, nir_u2f32(b, r), 1.0 / maxv); break;
        case 3: e[i] = nir_fmax(b, nir_fmul_imm(b, nir_i2f32(b, r), 1.0 / maxv), nir_imm_float(b, -1.0f)); break;
        case 4: e[i] = nir_f2f32(b, r); break;
        default: e[i] = r; break;
        }
    }
    if (var->si_fmt[loc] == 42) { nir_def *x = e[0]; e[0] = e[2]; e[2] = x; }
    nir_def *v = nir_vec(b, e, 4);
    const int want_float = is_float(c, t);
    const int have_float = f.kind >= 2;
    if (want_float && !have_float) v = f.kind == 1 ? nir_i2f32(b, v) : nir_u2f32(b, v);
    else if (!want_float && have_float) v = nir_f2i32(b, v);
    return fit_bits(c, v, t);
}

/* ---- pipeline reflection ----
 * One line per resource argument of the entry point, what Metal's
 * MTLRenderPipelineReflection / MTLComputePipelineReflection report (SpriteKit
 * and SceneKit bind by these names and indices):
 *   refl <type> <index> <access> <active> <arrayLength> <size> <align>
 *        <dataType> <textureType> <textureDataType> <isDepth> <name>
 * type = MTLBindingType (0 buffer, 1 threadgroup memory, 2 texture, 3 sampler),
 * access = MTLBindingAccess, dataType / textureDataType = MTLDataType,
 * textureType = MTLTextureType. */
static uint32_t mtl_data_type(const char *t) {
    static const struct { const char *n; uint32_t v; } k[] = {
        {"float", 3}, {"float2", 4}, {"float3", 5}, {"float4", 6}, {"float2x2", 7}, {"float2x3", 8}, {"float2x4", 9},
        {"float3x2", 10}, {"float3x3", 11}, {"float3x4", 12}, {"float4x2", 13}, {"float4x3", 14}, {"float4x4", 15},
        {"half", 16}, {"half2", 17}, {"half3", 18}, {"half4", 19}, {"half2x2", 20}, {"half2x3", 21}, {"half2x4", 22},
        {"half3x2", 23}, {"half3x3", 24}, {"half3x4", 25}, {"half4x2", 26}, {"half4x3", 27}, {"half4x4", 28},
        {"int", 29}, {"int2", 30}, {"int3", 31}, {"int4", 32}, {"uint", 33}, {"uint2", 34}, {"uint3", 35}, {"uint4", 36},
        {"short", 37}, {"short2", 38}, {"short3", 39}, {"short4", 40}, {"ushort", 41}, {"ushort2", 42}, {"ushort3", 43},
        {"ushort4", 44}, {"char", 45}, {"char2", 46}, {"char3", 47}, {"char4", 48}, {"uchar", 49}, {"uchar2", 50},
        {"uchar3", 51}, {"uchar4", 52}, {"bool", 53}, {"bool2", 54}, {"bool3", 55}, {"bool4", 56}, {"sampler", 59},
        {"long", 81}, {"long2", 82}, {"long3", 83}, {"long4", 84}, {"ulong", 85}, {"ulong2", 86}, {"ulong3", 87},
        {"ulong4", 88}, {"bfloat", 121}, {"bfloat2", 122}, {"bfloat3", 123}, {"bfloat4", 124},
        {"packed_float2", 4}, {"packed_float3", 5}, {"packed_float4", 6}, {"packed_half2", 17}, {"packed_half3", 18},
        {"packed_half4", 19}, {"packed_int2", 30}, {"packed_int3", 31}, {"packed_int4", 32}, {"packed_uint2", 34},
        {"packed_uint3", 35}, {"packed_uint4", 36}, {"packed_uchar4", 52}, {"packed_char4", 48},
    };
    if (!t) return 0;
    for (size_t i = 0; i < sizeof k / sizeof k[0]; ++i) if (!strcmp(k[i].n, t)) return k[i].v;
    return 1;   /* a struct (or anything else named) */
}

static void refl_add(ctx *c, const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    const size_t old = c->abi->refl ? strlen(c->abi->refl) : 0;
    char *r = realloc(c->abi->refl, old + (size_t)n + 2);
    if (!r) return;
    memcpy(r + old, line, (size_t)n);
    r[old + n] = '\n';
    r[old + n + 1] = 0;
    c->abi->refl = r;
}

/* OpenCL C compiled for Metal (Apple's CL over Metal, AppleMetalOpenGLRenderer):
 * the module has no buffer/texture limits (air.max_device_buffers -1). The
 * runtime binds three hidden buffers in front of the kernel's own: 0 global
 * offset xyz + dimension count (uint32 x4), 1 printf buffer, 2 image
 * order/data type table, and the kernel's buffers after them, as the M1's
 * reflection says (a at 3 for add(a, b, c)). */
static int is_opencl_module(ctx *c) {
    const air_named_md *fl = air_named(c->m, "llvm.module.flags");
    for (uint32_t i = 0; fl && i < fl->n; ++i) {
        const air_md *e = md(c, (int32_t)fl->ops[i]);
        if (!e || e->kind != AM_NODE || e->n < 3) continue;
        const char *k = air_md_string(c->m, e->ops[1]);
        int64_t v = 0;
        if (k && !strcmp(k, "air.max_device_buffers") && !air_md_int(c->m, e->ops[2], &v)) return v == -1 || v == 0xffffffffLL;
    }
    return 0;
}

static void build_reflection(ctx *c, const air_md *args) {
    for (uint32_t i = 0; i < args->n; ++i) {
        const air_md *a = md(c, args->ops[i]);
        if (!a || a->kind != AM_NODE || a->n < 2) continue;
        const char *kind = air_md_string(c->m, a->ops[1]);
        if (kind && !strcmp(kind, "air.function_constant") && a->n > 3) kind = air_md_string(c->m, a->ops[3]);
        if (!kind) continue;
        /* air.constant: an OpenCL by-value argument, its bytes bound at a buffer index (setBytes) */
        const int byval = !strcmp(kind, "air.constant");
        const int buf = !strcmp(kind, "air.buffer") || !strcmp(kind, "air.indirect_buffer") ||
                        !strcmp(kind, "air.visible_function_table") || byval;
        const int tex = !strcmp(kind, "air.texture"), samp = !strcmp(kind, "air.sampler");
        const int tgm = !strcmp(kind, "air.threadgroup");
        if (!buf && !tex && !samp && !tgm) continue;
        int64_t loc = 0, cnt = 1, size = 0, align = 0, as = 1;
        arg_int_after(c, a, "air.location_index", &loc);
        arg_count_after(c, a, &cnt);
        arg_int_after(c, a, "air.arg_type_size", &size);
        arg_int_after(c, a, "air.arg_type_align_size", &align);
        arg_int_after(c, a, "air.address_space", &as);
        const char *tn = arg_str_after(c, a, "air.arg_type_name");
        const char *name = arg_str_after(c, a, "air.arg_name");
        char nm[128];
        snprintf(nm, sizeof nm, "%s", name ? name : "arg");
        for (char *p = nm; *p; ++p) if (*p == ' ' || *p == '\n') *p = '_';
        const int active = !arg_has(c, a, "air.arg_unused");
        const int access = arg_has(c, a, "air.read_write") ? 1 : arg_has(c, a, "air.write") ? 2 : 0;
        if (tex) {
            uint32_t tt = 2, tdt = 3, depth = 0;
            if (tn) {
                depth = !strncmp(tn, "depth", 5);
                const char *b = depth ? tn + 5 : !strncmp(tn, "texture", 7) ? tn + 7 : "2d";
                if (!strncmp(b, "1d_array", 8)) tt = 1; else if (!strncmp(b, "1d", 2)) tt = 0;
                else if (!strncmp(b, "2d_ms_array", 11)) tt = 8; else if (!strncmp(b, "2d_ms", 5)) tt = 4;
                else if (!strncmp(b, "2d_array", 8)) tt = 3; else if (!strncmp(b, "2d", 2)) tt = 2;
                else if (!strncmp(b, "cube_array", 10)) tt = 6; else if (!strncmp(b, "cube", 4)) tt = 5;
                else if (!strncmp(b, "3d", 2)) tt = 7; else if (!strncmp(b, "_buffer", 7)) tt = 9;
                const char *lt = strchr(tn, '<');
                if (lt && !depth) {
                    if (!strncmp(lt + 1, "half", 4)) tdt = 16; else if (!strncmp(lt + 1, "int", 3)) tdt = 29;
                    else if (!strncmp(lt + 1, "uint", 4)) tdt = 33; else if (!strncmp(lt + 1, "short", 5)) tdt = 37;
                    else if (!strncmp(lt + 1, "ushort", 6)) tdt = 41;
                }
            }
            refl_add(c, "refl 2 %lld %d %d %lld 0 0 58 %u %u %u %s", (long long)loc, access, active, (long long)cnt, tt, tdt,
                     depth, nm);
        } else if (samp) {
            refl_add(c, "refl 3 %lld 0 %d %lld 0 0 59 0 0 0 %s", (long long)loc, active, (long long)cnt, nm);
        } else if (tgm || as == 3) {
            refl_add(c, "refl 1 %lld 1 %d 1 %lld %lld 0 0 0 0 %s", (long long)loc, active, (long long)size, (long long)align, nm);
        } else {
            refl_add(c, "refl 0 %lld %d %d %lld %lld %lld %u 0 0 0 %s%s", (long long)(loc + c->buf_shift), access, active, (long long)cnt,
                     (long long)size, (long long)align, mtl_data_type(tn), nm, byval ? " const" : "");
            /* struct members: air.struct_type_info is a flat list of
             * (offset, size, array length, type name, member name) groups;
             * one reflm line each, tied to this refl line */
            int32_t sti = -1;
            for (uint32_t k = 0; k + 1 < a->n; ++k) {
                const char *key = air_md_string(c->m, a->ops[k]);
                if (key && !strcmp(key, "air.struct_type_info")) { sti = a->ops[k + 1]; break; }
            }
            const air_md *st = sti >= 0 ? md(c, sti) : NULL;
            for (uint32_t k = 0; st && st->kind == AM_NODE && k + 4 < st->n; k += 5) {
                int64_t off = 0, msz = 0, arr = 0;
                if (air_md_int(c->m, st->ops[k], &off) || air_md_int(c->m, st->ops[k + 1], &msz)) break;
                air_md_int(c->m, st->ops[k + 2], &arr);
                const char *mt = air_md_string(c->m, st->ops[k + 3]), *mn = air_md_string(c->m, st->ops[k + 4]);
                if (!mt || !mn) break;
                refl_add(c, "reflm %lld %lld %lld %u %s", (long long)off, (long long)msz, (long long)arr, mtl_data_type(mt), mn);
                /* argument buffer member: "air.indirect_argument", !{kind,
                 * location_index id count, access}: one refli line with what
                 * an argument encoder needs (offset, 0 buffer / 2 texture /
                 * 3 sampler, [[id]], count, access) */
                const char *ia = k + 6 < st->n ? air_md_string(c->m, st->ops[k + 5]) : NULL;
                if (ia && !strcmp(ia, "air.indirect_argument")) {
                    const air_md *im = md(c, st->ops[k + 6]);
                    const char *ik = im && im->kind == AM_NODE && im->n > 1 ? air_md_string(c->m, im->ops[1]) : NULL;
                    if (ik) {
                        int64_t id = 0, icnt = 1;
                        arg_int_after(c, im, "air.location_index", &id);
                        arg_count_after(c, im, &icnt);
                        const int rk = !strcmp(ik, "air.texture") ? 2 : !strcmp(ik, "air.sampler") ? 3 : 0;
                        const int acc = arg_has(c, im, "air.read_write") ? 1 : arg_has(c, im, "air.write") ? 2 : 0;
                        refl_add(c, "refli %lld %d %lld %lld %d %s", (long long)off, rk, (long long)id, (long long)icnt, acc, mn);
                    }
                    k += 2;
                    continue;
                }
                /* a nested struct's own member list follows as a node: skip it */
                if (k + 5 < st->n && md(c, st->ops[k + 5]) && md(c, st->ops[k + 5])->kind == AM_NODE) k++;
            }
        }
    }
    /* after the kernel's own, in the M1's order: OpenCL's layer takes CL
     * argument i from arguments[i] */
    if (c->buf_shift) {
        refl_add(c, "refl 0 0 0 1 1 4 4 33 0 0 0 __global_offset_and_num_dims");
        refl_add(c, "refl 0 1 1 1 1 1 1 45 0 0 0 __printf_buffer");
        refl_add(c, "refl 0 2 0 1 1 4 4 33 0 0 0 __image_order_data_type_table");
    }
}

/* ---- mesh shaders (Metal 3) as compute ----
 * Object stage: one payload + one mesh grid per object threadgroup. Mesh stage: dispatched as (slots, objects)
 * threadgroups; each group finds its object's grid, turns its slot number into threadgroup_position_in_grid and
 * writes one output block, which a generated vertex shader draws from (tess_gen.c mesh_vs). */
static int mesh_prescan(ctx *c, const air_md *args, uint32_t nbuf) {
    air_abi *abi = c->abi;
    abi->mesh_slot = nbuf;
    for (uint32_t i = 0; i < args->n; ++i) {
        const air_md *a = md(c, args->ops[i]);
        if (!a || a->kind != AM_NODE || a->n < 2) continue;
        const char *kind = air_md_string(c->m, a->ops[1]);
        if (!kind) continue;
        if (!strcmp(kind, "air.payload")) {
            int64_t sz = 0;
            arg_int_after(c, a, "air.arg_type_size", &sz);
            abi->mesh_payload_stride = ((uint32_t)(sz > 0 ? sz : 16) + 15) & ~15u;
        }
        if (strcmp(kind, "air.mesh") || a->n < 3) continue;
        const air_md *ti = md(c, a->ops[2]);
        if (!ti || ti->kind != AM_NODE || ti->n < 6) { fail(c, "mesh type info"); return -1; }
        const air_md *vm = md(c, ti->ops[1]), *pm = md(c, ti->ops[2]);
        int64_t mv = 0, mp = 0;
        air_md_int(c->m, ti->ops[3], &mv);
        air_md_int(c->m, ti->ops[4], &mp);
        const char *topo = air_md_string(c->m, ti->ops[5]);
        if (mv <= 0 || mv > 256 || mp <= 0 || mp > 512) { fail(c, "mesh of %lld vertices %lld primitives", (long long)mv, (long long)mp); return -1; }
        if (pm && pm->kind == AM_NODE && pm->n) { fail(c, "per-primitive mesh data"); return -1; }
        abi->mesh_max_v = (uint32_t)mv;
        abi->mesh_max_p = (uint32_t)mp;
        abi->mesh_vpp = topo && !strcmp(topo, "air.point") ? 1 : topo && !strcmp(topo, "air.line") ? 2 : 3;
        size_t len = 0;
        for (uint32_t k = 0; vm && vm->kind == AM_NODE && k < vm->n; ++k) {
            const air_md *e = md(c, vm->ops[k]);
            const char *ek = e && e->kind == AM_NODE && e->n ? air_md_string(c->m, e->ops[0]) : NULL;
            if (!ek) continue;
            if (!strcmp(ek, "air.position")) continue;
            if (strcmp(ek, "air.mesh_vertex_data") || e->n < 3) { fail(c, "mesh vertex member %s", ek); return -1; }
            const char *nm = air_md_string(c->m, e->ops[2]);
            if (!nm || len + strlen(nm) + 2 >= sizeof abi->io) { fail(c, "mesh vertex data name"); return -1; }
            if (len) abi->io[len++] = ',';
            memcpy(abi->io + len, nm, strlen(nm));
            len += strlen(nm);
            abi->mesh_nvdata++;
        }
        abi->io[len] = 0;
        abi->mesh_vstride = 16u * (1 + abi->mesh_nvdata);
        abi->mesh_group_stride = (16 + abi->mesh_max_v * abi->mesh_vstride + abi->mesh_max_p * abi->mesh_vpp * 4 + 15) & ~15u;
    }
    if (abi->mesh_kind == 2 && !abi->mesh_max_v) { fail(c, "mesh function without a mesh argument"); return -1; }
    return 0;
}

static void mesh_sysvals(ctx *c) {
    nir_builder *b = &c->b;
    air_abi *abi = c->abi;
    const uint32_t slot = abi->mesh_slot;
    nir_def *pbase = push_u64(c, slot * 8), *gbase = push_u64(c, (slot + 1) * 8), *obase = push_u64(c, (slot + 2) * 8);
    nir_def *ng = nir_udiv(b, nir_iadd(b, c->sys_grid, nir_iadd_imm(b, c->sys_block, -1)), c->sys_block);
    nir_def *wx = nir_channel(b, c->sys_wgid, 0), *wy = nir_channel(b, c->sys_wgid, 1), *wz = nir_channel(b, c->sys_wgid, 2);
    if (abi->mesh_kind == 1) {
        nir_def *lin = nir_iadd(b, wx, nir_imul(b, nir_channel(b, ng, 0), nir_iadd(b, wy, nir_imul(b, nir_channel(b, ng, 1), wz))));
        c->mesh_grid_addr = nir_iadd(b, gbase, nir_u2u64(b, nir_imul_imm(b, lin, 16)));
        c->mesh_payload = nir_iadd(b, pbase, nir_u2u64(b, nir_imul_imm(b, lin, abi->mesh_payload_stride)));
        return;
    }
    nir_def *g = nir_load_global(b, 3, 32, nir_iadd(b, gbase, nir_u2u64(b, nir_imul_imm(b, wy, 16))), .align_mul = 16);
    nir_def *gx = nir_umax(b, nir_channel(b, g, 0), nir_imm_int(b, 1)), *gy = nir_umax(b, nir_channel(b, g, 1), nir_imm_int(b, 1));
    nir_def *total = nir_imul(b, nir_imul(b, nir_channel(b, g, 0), nir_channel(b, g, 1)), nir_channel(b, g, 2));
    c->mesh_active = nir_ult(b, wx, total);
    nir_def *gxy = nir_imul(b, gx, gy);
    nir_def *tg = nir_vec3(b, nir_umod(b, wx, gx), nir_umod(b, nir_udiv(b, wx, gx), gy), nir_udiv(b, wx, gxy));
    nir_def *slotn = nir_iadd(b, nir_imul(b, wy, nir_channel(b, ng, 0)), wx);
    c->mesh_out = nir_iadd(b, obase, nir_u2u64(b, nir_imul_imm(b, slotn, abi->mesh_group_stride)));
    c->mesh_payload = nir_iadd(b, pbase, nir_u2u64(b, nir_imul_imm(b, wy, abi->mesh_payload_stride)));
    c->sys_wgid = tg;
    c->sys_grid = nir_imul(b, g, c->sys_block);
    c->sys_gid = nir_iadd(b, nir_imul(b, tg, c->sys_block), c->sys_lid);
}

static int setup_args(ctx *c) {
    nir_builder *b = &c->b;
    int32_t argsid = -1;
    if (find_kernel_node(c, &argsid)) { fail(c, "%s is not an air.kernel", c->f->name); return -1; }
    const air_md *args = md(c, argsid);
    if (!args || args->kind != AM_NODE) { fail(c, "kernel argument list"); return -1; }
    c->buf_shift = c->stage == MESA_SHADER_COMPUTE && is_opencl_module(c) ? 3 : 0;
    build_reflection(c, args);
    /* buffers first: the push layout needs the highest index */
    uint32_t nbuf = c->buf_shift;
    for (uint32_t i = 0; i < args->n; ++i) {
        const air_md *a = md(c, args->ops[i]);
        if (!a || a->kind != AM_NODE || a->n < 2) continue;
        const char *kind = air_md_string(c->m, a->ops[1]);
        if (kind && !strcmp(kind, "air.function_constant") && a->n > 3) kind = air_md_string(c->m, a->ops[3]);
        int64_t loc = 0;
        int64_t ai = 0;
        if (air_md_int(c->m, a->ops[0], &ai) || (uint64_t)ai >= c->f->nargs) continue;
        const uint32_t as = ptr_as(c, c->f->values[c->f->first_arg + (uint32_t)ai].type);
        if (kind && (!strcmp(kind, "air.buffer") || !strcmp(kind, "air.indirect_buffer") ||
                     !strcmp(kind, "air.visible_function_table") || !strcmp(kind, "air.constant")) && as != 3 &&
            !arg_int_after(c, a, "air.location_index", &loc)) {
            int64_t cnt = 1;
            arg_count_after(c, a, &cnt);
            if ((uint32_t)(loc + cnt) + c->buf_shift > nbuf) nbuf = (uint32_t)(loc + cnt) + c->buf_shift;
        }
        /* [[stage_in]]: its buffers (and the index buffer) take push slots too */
        if (kind && !strcmp(kind, "air.stage_in") && !arg_int_after(c, a, "air.location_index", &loc) &&
            loc >= 0 && loc < 31 && c->var->si_fmt[loc]) {
            if (c->var->si_buf[loc] + 1u > nbuf) nbuf = c->var->si_buf[loc] + 1u;
            if (c->var->sx_given && c->var->sx_buf + 1u > nbuf) nbuf = c->var->sx_buf + 1u;
        }
    }
    uint32_t ntex = 0, nsamp = 0;
    for (uint32_t i = 0; i < args->n; ++i) {
        const air_md *a = md(c, args->ops[i]);
        if (!a || a->kind != AM_NODE || a->n < 2) continue;
        const char *kind = air_md_string(c->m, a->ops[1]);
        if (kind && !strcmp(kind, "air.function_constant") && a->n > 3) kind = air_md_string(c->m, a->ops[3]);
        int64_t loc = 0;
        if (!kind || arg_int_after(c, a, "air.location_index", &loc) || loc < 0 || loc >= 128) continue;
        if (!strcmp(kind, "air.texture") && (uint32_t)loc + 1 > ntex) ntex = (uint32_t)loc + 1;
        if (!strcmp(kind, "air.sampler") && (uint32_t)loc + 1 > nsamp) nsamp = (uint32_t)loc + 1;
    }
    /* framebuffer fetch: [[color(n)]] fragment inputs */
    c->abi->rt_read_mask = 0;
    for (uint32_t i = 0; c->stage == MESA_SHADER_FRAGMENT && i < args->n; ++i) {
        const air_md *a = md(c, args->ops[i]);
        if (!a || a->kind != AM_NODE || a->n < 2) continue;
        const char *kind = air_md_string(c->m, a->ops[1]);
        int64_t n = 0;
        if (kind && !strcmp(kind, "air.render_target") && !arg_int_after(c, a, "air.render_target", &n) && n >= 0 && n < 8)
            c->abi->rt_read_mask |= 1u << n;
    }
    /* mesh shaders: payload size and the mesh<V, P, maxV, maxP, topology> shape, three hidden buffer slots */
    if (c->abi->mesh_kind && mesh_prescan(c, args, nbuf)) return -1;
    if (c->abi->mesh_kind) nbuf += 3;
    c->abi->nbuf = nbuf;
    c->abi->ntex = ntex;
    c->abi->nsamp = nsamp;
    c->tex_base = nbuf * 8;
    c->samp_base = c->tex_base + ntex * 8;
    c->abi->grid_offset = c->samp_base + (nsamp + c->abi->nconst_samp) * 4;
    const uint32_t g = c->abi->grid_offset;
    c->abi->push_bytes = g + 24;
    if (c->abi->cdata_bytes) {
        c->abi->cdata_offset = (g + (c->stage == MESA_SHADER_COMPUTE || c->stage == MESA_SHADER_TESS_EVAL ? 24 : 8) + 7) & ~7u;
        c->abi->push_bytes = c->abi->cdata_offset + 8;
        c->push_cdata = push_u64(c, c->abi->cdata_offset);
    }
    /* system values (compute) */
    if (c->stage == MESA_SHADER_COMPUTE) {
        c->sys_grid = nir_vec3(b, push_u32(c, g), push_u32(c, g + 4), push_u32(c, g + 8));
        c->sys_block = nir_vec3(b, push_u32(c, g + 12), push_u32(c, g + 16), push_u32(c, g + 20));
        c->sys_wgid = nir_load_workgroup_id(b);
        c->sys_lid = nir_load_local_invocation_id(b);
        c->sys_gid = nir_iadd(b, nir_imul(b, c->sys_wgid, c->sys_block), c->sys_lid);
        c->sys_lindex = nir_load_local_invocation_index(b);
        if (c->abi->mesh_kind) mesh_sysvals(c);
    } else if (!c->abi->cdata_bytes) {
        c->abi->push_bytes = g + 8;             /* base vertex, base instance */
    }
    if (c->abi->rt_read_mask) {                 /* after everything else: the runtime appends them */
        c->abi->rt_read_offset = (c->abi->push_bytes + 7) & ~7u;
        c->abi->push_bytes = c->abi->rt_read_offset + 8 * (32 - __builtin_clz(c->abi->rt_read_mask));
    }
    c->arg_align = calloc(c->f->nargs + 1, 8);
    for (uint32_t i = 0; i < args->n; ++i) {
        const air_md *a = md(c, args->ops[i]);
        if (!a || a->kind != AM_NODE || a->n < 2) continue;
        if (getenv("NAKC_ARGDUMP")) {   /* debug: each kernel argument's metadata */
            fprintf(stderr, "arg %u:", i);
            for (uint32_t k = 0; k < a->n; ++k) {
                const char *str = air_md_string(c->m, a->ops[k]);
                int64_t iv = 0;
                if (str) fprintf(stderr, " %s", str);
                else if (!air_md_int(c->m, a->ops[k], &iv)) fprintf(stderr, " %lld", (long long)iv);
                else fprintf(stderr, " ?");
            }
            fprintf(stderr, "\n");
        }
        int64_t ai = 0;
        if (air_md_int(c->m, a->ops[0], &ai) || (uint64_t)ai >= c->f->nargs) continue;
        const char *kind = air_md_string(c->m, a->ops[1]);
        if (kind && !strcmp(kind, "air.function_constant") && a->n > 3)   /* optional argument */
            kind = air_md_string(c->m, a->ops[3]);
        const uint32_t vid = c->f->first_arg + (uint32_t)ai;
        const uint32_t t = c->f->values[vid].type;
        tval v = {0};
        if (!kind) continue;
        /* argument buffers: plain memory holding gpuAddress / gpuResourceID
         * values, which are our addresses and handles, so no rewriting */
        if (!strcmp(kind, "air.buffer") || !strcmp(kind, "air.indirect_buffer") ||
            !strcmp(kind, "air.visible_function_table")) {
            int64_t loc = 0;
            arg_int_after(c, a, "air.location_index", &loc);
            const uint32_t as = ptr_as(c, t);
            if (as == 3) {                       /* threadgroup buffer: [[threadgroup(i)]] storage */
                if (loc < 0 || loc >= 32) { fail(c, "threadgroup index"); return -1; }
                v.d = nir_imm_int(b, (int)(c->var->tg_arg_bytes[loc] ? c->abi->tg_arg_offset[loc] : tg_guard_on(c)));
                c->arg_align[ai] = 16;
            } else if (as == 0) {
                /* array of buffers ([[buffer(i)]] array_ref / T *x[N]): a
                 * thread-space array of their addresses */
                int64_t cnt = 1;
                arg_count_after(c, a, &cnt);
                /* 0 is legal (array_ref<void> with nothing bound: MPSGraph's
                 * stitched kernels size it from function constants) */
                if (cnt < 0 || cnt > 64) { fail(c, "buffer array of %lld", (long long)cnt); return -1; }
                c->scratch = (c->scratch + 7) & ~7u;
                const uint32_t base = c->scratch;
                c->scratch += (uint32_t)cnt * 8;
                for (int64_t k = 0; k < cnt; ++k)
                    nir_store_scratch(b, push_u64(c, (uint32_t)(loc + k + c->buf_shift) * 8), nir_imm_int(b, (int)(base + k * 8)),
                                      .align_mul = 8);
                v.d = nir_imm_int(b, (int)base);
                c->arg_align[ai] = 8;
            } else {
                v.d = push_u64(c, (uint32_t)(loc + c->buf_shift) * 8);
                c->arg_align[ai] = 4;
            }
        } else if (!strcmp(kind, "air.constant")) {
            /* OpenCL by-value argument (scalar, vector, struct): the runtime binds its bytes
             * at this buffer index; a pointer-typed one is that address, a value is loaded */
            int64_t loc = 0, al = 4;
            arg_int_after(c, a, "air.location_index", &loc);
            arg_int_after(c, a, "air.arg_type_align_size", &al);
            nir_def *addr = push_u64(c, (uint32_t)(loc + c->buf_shift) * 8);
            if (ptr_as(c, t) != ~0u) {
                v.d = addr;
                c->arg_align[ai] = 4;
            } else {
                v = load_typed(c, 2, addr, t, al > 0 ? (uint32_t)al : 4);
            }
        } else if (!strcmp(kind, "air.texture")) {
            int64_t loc = 0;
            arg_int_after(c, a, "air.location_index", &loc);
            v.d = push_u64(c, c->tex_base + (uint32_t)loc * 8);
        } else if (!strcmp(kind, "air.sampler")) {
            int64_t loc = 0;
            arg_int_after(c, a, "air.location_index", &loc);
            v.d = nir_u2u64(b, push_u32(c, c->samp_base + (uint32_t)loc * 4));
        } else if (!strcmp(kind, "air.threadgroup")) {
            int64_t loc = 0;
            arg_int_after(c, a, "air.location_index", &loc);
            if (loc < 0 || loc >= 32) { fail(c, "threadgroup index"); return -1; }
            v.d = nir_imm_int(b, (int)(c->var->tg_arg_bytes[loc] ? c->abi->tg_arg_offset[loc] : tg_guard_on(c)));
            c->arg_align[ai] = 16;
        } else if (!strcmp(kind, "air.stage_in") && c->stage == MESA_SHADER_COMPUTE) {
            int64_t loc = -1;
            arg_int_after(c, a, "air.location_index", &loc);
            v.d = stage_in_fetch(c, loc, t);
            if (!v.d) return -1;
        } else if (!strcmp(kind, "air.payload") && c->abi->mesh_kind) {
            v.d = nir_iadd(b, c->mesh_payload, nir_imm_int64(b, 0));
            c->arg_align[ai] = 16;
        } else if ((!strcmp(kind, "air.mesh_grid_properties") || !strcmp(kind, "air.mesh")) && c->abi->mesh_kind) {
            uint32_t bits, comps;
            if (scalar_shape(c, t, &bits, &comps)) bits = 32;
            v.d = imm(c, 0, bits);          /* only the air.set_*_mesh calls take it; they know where to write */
        } else if (!strcmp(kind, "air.thread_position_in_grid")) v.d = fit_uvec(c, c->sys_gid, t);
        else if (!strcmp(kind, "air.thread_position_in_threadgroup")) v.d = fit_uvec(c, c->sys_lid, t);
        else if (!strcmp(kind, "air.threadgroup_position_in_grid")) v.d = fit_uvec(c, c->sys_wgid, t);
        else if (!strcmp(kind, "air.threads_per_threadgroup") || !strcmp(kind, "air.dispatch_threads_per_threadgroup"))
            v.d = fit_uvec(c, c->sys_block, t);
        else if (!strcmp(kind, "air.threads_per_grid")) v.d = fit_uvec(c, c->sys_grid, t);
        else if (!strcmp(kind, "air.threadgroups_per_grid")) {
            nir_def *n = nir_udiv(b, nir_iadd(b, c->sys_grid, nir_iadd_imm(b, c->sys_block, -1)), c->sys_block);
            v.d = fit_uvec(c, n, t);
        } else if (!strcmp(kind, "air.thread_index_in_threadgroup")) {
            uint32_t bits, comps;
            scalar_shape(c, t, &bits, &comps);
            v.d = nir_u2uN(b, c->sys_lindex, bits);
        } else if (!strcmp(kind, "air.thread_index_in_simdgroup") || !strcmp(kind, "air.thread_index_in_quadgroup")) {
            uint32_t bits, comps;
            scalar_shape(c, t, &bits, &comps);
            nir_def *lane = nir_iand_imm(b, c->sys_lindex, strstr(kind, "quad") ? 3 : 31);
            v.d = nir_u2uN(b, lane, bits);
        } else if (!strcmp(kind, "air.simdgroup_index_in_threadgroup") || !strcmp(kind, "air.quadgroup_index_in_threadgroup")) {
            uint32_t bits, comps;
            scalar_shape(c, t, &bits, &comps);
            v.d = nir_u2uN(b, nir_ushr_imm(b, c->sys_lindex, strstr(kind, "quad") ? 2 : 5), bits);
        } else if (!strcmp(kind, "air.threads_per_simdgroup") || !strcmp(kind, "air.thread_execution_width")) {
            uint32_t bits, comps;
            scalar_shape(c, t, &bits, &comps);
            v.d = imm(c, 32, bits);
        } else if (!strcmp(kind, "air.simdgroups_per_threadgroup")) {
            uint32_t bits, comps;
            scalar_shape(c, t, &bits, &comps);
            nir_def *n = nir_imul(b, nir_imul(b, nir_channel(b, c->sys_block, 0), nir_channel(b, c->sys_block, 1)),
                                  nir_channel(b, c->sys_block, 2));
            v.d = nir_u2uN(b, nir_ushr_imm(b, nir_iadd_imm(b, n, 31), 5), bits);
        } else if (c->stage != MESA_SHADER_COMPUTE && gfx_input(c, a, kind, t, &v)) {
            /* vertex / fragment stage input */
        } else {
            fail(c, "kernel argument kind %s", kind);
            return -1;
        }
        set(c, vid, v);
    }
    for (uint32_t i = 0; i < c->f->nargs; ++i)
        if (!c->have[c->f->first_arg + i]) { fail(c, "argument %u has no binding", i); return -1; }
    return 0;
}

/* ---- CFG ---- */
static const air_inst *terminator(ctx *c, uint32_t bb) {
    for (uint32_t i = 0; i < c->f->ninsts; ++i) {
        const air_inst *in = &c->f->insts[i];
        if (in->bb == bb && (in->op == AI_RET || in->op == AI_BR || in->op == AI_SWITCH || in->op == AI_UNREACHABLE))
            return in;
    }
    return NULL;
}

static void rpo_visit(ctx *c, uint32_t bb, uint8_t *seen, uint32_t *out, uint32_t *n, const air_inst **term) {
    seen[bb] = 1;
    const air_inst *t = term[bb];
    if (t)
        for (int i = (int)t->nblocks - 1; i >= 0; --i) {
            const uint32_t s = t->blocks[i];
            if (s < c->f->nblocks && !seen[s]) rpo_visit(c, s, seen, out, n, term);
        }
    out[(*n)++] = bb;
}

static nir_block *new_block(ctx *c) {
    nir_block *nb = nir_block_create(c->s);
    exec_list_push_tail(&c->impl->body, &nb->cf_node.node);
    nb->cf_node.parent = &c->impl->cf_node;
    return nb;
}

/* aggregate values travel through one variable per scalar/vector leaf */
static uint32_t count_leaves(ctx *c, uint32_t t) {
    const air_type *x = T(c, t);
    if (x->kind == AT_STRUCT || x->kind == AT_ARRAY) {
        uint32_t n = 0;
        for (uint32_t i = 0; i < x->count; ++i) n += count_leaves(c, x->kind == AT_STRUCT ? x->members[i] : x->elem);
        return n;
    }
    return x->kind == AT_VOID ? 0 : 1;
}

static void make_leaf_vars(ctx *c, uint32_t t, nir_variable **v, uint32_t *k) {
    const air_type *x = T(c, t);
    if (x->kind == AT_STRUCT || x->kind == AT_ARRAY) {
        for (uint32_t i = 0; i < x->count; ++i) make_leaf_vars(c, x->kind == AT_STRUCT ? x->members[i] : x->elem, v, k);
        return;
    }
    uint32_t bits, comps;
    if (scalar_shape(c, t, &bits, &comps)) { fail(c, "value of unsupported type crosses blocks"); bits = 32; comps = 1; }
    v[(*k)++] = nir_local_variable_create(c->impl, glsl_for(bits, comps), "agg");
}

static void store_leaves(ctx *c, uint32_t t, nir_variable **v, uint32_t *k, tval val) {
    const air_type *x = T(c, t);
    if (x->kind == AT_STRUCT || x->kind == AT_ARRAY) {
        for (uint32_t i = 0; i < x->count; ++i)
            store_leaves(c, x->kind == AT_STRUCT ? x->members[i] : x->elem, v, k,
                         val.m && i < val.n ? val.m[i] : zero_of(c, x->kind == AT_STRUCT ? x->members[i] : x->elem));
        return;
    }
    nir_def *d = val.d ? val.d : zero_of(c, t).d;
    nir_variable *var = v[(*k)++];
    if (d) nir_store_var(&c->b, var, d, nir_component_mask(d->num_components));
}

static tval load_leaves(ctx *c, uint32_t t, nir_variable **v, uint32_t *k) {
    tval r = {0};
    const air_type *x = T(c, t);
    if (x->kind == AT_STRUCT || x->kind == AT_ARRAY) {
        r.n = x->count;
        r.m = ralloc_array(c->s, tval, r.n ? r.n : 1);
        for (uint32_t i = 0; i < r.n; ++i) r.m[i] = load_leaves(c, x->kind == AT_STRUCT ? x->members[i] : x->elem, v, k);
        return r;
    }
    r.d = nir_load_var(&c->b, v[(*k)++]);
    return r;
}

/* Emit function f at the cursor: a goto into its first block, its body as
 * unstructured blocks, returns to ret_block (the kernel's end block, or the
 * continuation of an inlined call) with the value left in retvars. args are
 * the incoming argument values (NULL for the kernel, set up by setup_args). */
static void emit_function(ctx *c, air_function *f, const tval *args, const uint64_t *aligns,
                          nir_block *ret_block, nir_variable **retvars, uint32_t rett, int depth) {
    if (depth > 16) { fail(c, "call depth over 16 (recursion?)"); return; }
    if (f->is_proto || !f->nblocks) { fail(c, "%s has no body", f->name); return; }
    /* per-instance state */
    air_function *sf = c->f;
    tval *svals = c->vals;
    uint8_t *shave = c->have;
    nir_block **sblocks = c->blocks;
    nir_instr **send = c->end_nop;
    nir_variable **sphi = c->phivar;
    uint64_t *salign = c->arg_align;
    const int sdepth = c->depth;
    c->depth = depth;
    c->f = f;
    if (args) {
        c->vals = rzalloc_array(c->s, tval, f->nvalues + 1);
        c->have = rzalloc_array(c->s, uint8_t, f->nvalues + 1);
        c->arg_align = calloc(f->nargs + 1, 8);
        for (uint32_t i = 0; i < f->nargs; ++i) {
            set(c, f->first_arg + i, args[i]);
            c->arg_align[i] = aligns[i];
        }
    }
    c->blocks = rzalloc_array(c->s, nir_block *, f->nblocks + 1);
    c->end_nop = rzalloc_array(c->s, nir_instr *, f->nblocks + 1);
    c->phivar = rzalloc_array(c->s, nir_variable *, f->ninsts + 1);

    const air_inst **term = calloc(f->nblocks + 1, sizeof(*term));
    for (uint32_t i = 0; i < f->nblocks; ++i) term[i] = terminator(c, i);
    uint8_t *seen = calloc(f->nblocks + 1, 1);
    uint32_t *order = calloc(f->nblocks + 1, 4), n = 0;
    rpo_visit(c, 0, seen, order, &n, term);
    for (uint32_t i = 0; i < n / 2; ++i) { uint32_t t = order[i]; order[i] = order[n - 1 - i]; order[n - 1 - i] = t; }
    for (uint32_t i = 0; i < n; ++i) c->blocks[order[i]] = new_block(c);
    nir_goto(&c->b, c->blocks[0]);

    /* phis: one variable each (aggregates: one per leaf, first leaf here) */
    nir_variable ***aggphi = rzalloc_array(c->s, nir_variable **, f->ninsts + 1);
    for (uint32_t i = 0; i < f->ninsts; ++i) {
        const air_inst *in = &f->insts[i];
        if (in->op != AI_PHI || !c->blocks[in->bb]) continue;
        uint32_t bits, comps;
        if (!scalar_shape(c, in->type, &bits, &comps)) {
            c->phivar[i] = nir_local_variable_create(c->impl, glsl_for(bits, comps), "phi");
        } else {
            const uint32_t nl = count_leaves(c, in->type);
            aggphi[i] = rzalloc_array(c->s, nir_variable *, nl + 1);
            uint32_t k = 0;
            make_leaf_vars(c, in->type, aggphi[i], &k);
        }
    }
    for (uint32_t oi = 0; oi < n && !c->failed; ++oi) {
        const uint32_t bb = order[oi];
        c->b.cursor = nir_after_block(c->blocks[bb]);
        for (uint32_t i = 0; i < f->ninsts && !c->failed; ++i) {
            const air_inst *in = &f->insts[i];
            if (in->bb != bb) continue;
            if (in->op == AI_PHI) {
                tval v = {0};
                if (c->phivar[i]) v.d = nir_load_var(&c->b, c->phivar[i]);
                else { uint32_t k = 0; v = load_leaves(c, in->type, aggphi[i], &k); }
                set(c, in->value, v);
                continue;
            }
            if (in->op == AI_RET || in->op == AI_UNREACHABLE) {
                if (in->op == AI_RET && in->nops && retvars) {
                    uint32_t k = 0;
                    store_leaves(c, rett, retvars, &k, get(c, in->ops[0]));
                } else if (in->op == AI_RET && in->nops && depth == 0 && c->stage != MESA_SHADER_COMPUTE) {
                    emit_stage_outputs(c, get(c, in->ops[0]), c->f->values[in->ops[0]].type);
                }
                c->end_nop[bb] = &nir_nop(&c->b)->instr;
                nir_goto(&c->b, ret_block);
                break;
            }
            if (in->op == AI_BR) {
                nir_def *cond = in->nblocks == 2 ? getd(c, in->ops[0]) : NULL;
                c->end_nop[bb] = &nir_nop(&c->b)->instr;
                if (in->nblocks == 1 || in->blocks[0] == in->blocks[1]) nir_goto(&c->b, c->blocks[in->blocks[0]]);
                else nir_goto_if(&c->b, c->blocks[in->blocks[0]], cond, c->blocks[in->blocks[1]]);
                break;
            }
            if (in->op == AI_SWITCH) {
                nir_def *sel = getd(c, in->ops[0]);
                c->end_nop[bb] = &nir_nop(&c->b)->instr;
                for (uint32_t k = 1; k < in->nblocks; ++k) {
                    const air_value *cv = &f->values[in->ops[k]];
                    nir_def *cond = nir_ieq(&c->b, sel, imm(c, cv->kind == AV_CINT ? cv->ival : 0, sel->bit_size));
                    nir_block *next = new_block(c);
                    nir_goto_if(&c->b, c->blocks[in->blocks[k]], cond, next);
                    c->b.cursor = nir_after_block(next);
                }
                nir_goto(&c->b, c->blocks[in->blocks[0]]);
                break;
            }
            emit_inst(c, in);
            (void)depth;
        }
    }
    /* phi stores at the end of each predecessor */
    for (uint32_t i = 0; i < f->ninsts && !c->failed; ++i) {
        const air_inst *in = &f->insts[i];
        if (in->op != AI_PHI || (!c->phivar[i] && !aggphi[i])) continue;
        for (uint32_t k = 0; k < in->nblocks; ++k) {
            const uint32_t pred = in->blocks[k];
            if (pred >= f->nblocks || !c->end_nop[pred]) continue;
            c->b.cursor = nir_after_instr(c->end_nop[pred]);
            if (c->phivar[i]) {
                nir_def *v = getd(c, in->ops[k]);
                nir_store_var(&c->b, c->phivar[i], v, nir_component_mask(v->num_components));
            } else {
                uint32_t leaf = 0;
                store_leaves(c, in->type, aggphi[i], &leaf, get(c, in->ops[k]));
            }
        }
    }
    free(term); free(seen); free(order);
    if (args) free(c->arg_align);
    c->f = sf; c->vals = svals; c->have = shave; c->blocks = sblocks; c->end_nop = send;
    c->phivar = sphi; c->arg_align = salign;
    c->depth = sdepth;
}

/* call to a linked function: inlined from its own module. Everything that is
 * per module (types, values, metadata, global placement) switches with it;
 * its globals have no placement in this kernel, so they are refused. */
static tval linked_call(ctx *c, const air_inst *in, air_module *lm, air_function *callee) {
    tval r = {0};
    /* the caller may pass more than the body takes: MPSGraph's stitched
     * write_fn is declared (state, value, value) and stitched as (state,
     * value); with the function-pointer calling convention the extra
     * trailing argument is simply never read */
    if (in->nops - 1 < callee->nargs) {
        fail(c, "linked %s called with %u arguments, takes %u", callee->name, in->nops - 1, callee->nargs);
        return r;
    }
    const uint32_t n = callee->nargs;
    tval *args = ralloc_array(c->s, tval, n + 1);
    uint64_t *al = calloc(n + 1, 8);
    for (uint32_t i = 0; i < n; ++i) {
        args[i] = get(c, in->ops[1 + i]);
        al[i] = known_align(c, in->ops[1 + i], 0);
    }
    air_module *sm = c->m;
    uint32_t *sgo = c->global_off;
    uint8_t *sgu = c->gused;
    int32_t *scs = c->const_samp_slot;
    uint32_t li = 0;
    while (li < c->var->nlink && li < 16 && c->var->link_mod[li] != lm) ++li;
    c->m = lm;
    c->global_off = li < 16 ? c->link_goff[li] : NULL;
    c->gused = li < 16 ? c->link_gused[li] : NULL;
    c->const_samp_slot = li < 16 ? c->link_csamp[li] : NULL;
    const uint32_t rett = T(c, callee->type)->elem;
    const uint32_t nl = count_leaves(c, rett);
    nir_variable **rv = nl ? rzalloc_array(c->s, nir_variable *, nl + 1) : NULL;
    uint32_t k = 0;
    if (nl) make_leaf_vars(c, rett, rv, &k);
    nir_block *cont = new_block(c);
    emit_function(c, callee, args, al, cont, rv, rett, c->depth + 1);
    free(al);
    c->b.cursor = nir_after_block(cont);
    if (nl && !c->failed) { k = 0; r = load_leaves(c, rett, rv, &k); }
    c->m = sm; c->global_off = sgo; c->gused = sgu; c->const_samp_slot = scs;
    return r;
}

/* call to a function defined in the module: inline it */
static tval inline_call(ctx *c, const air_inst *in, air_function *callee, int depth) {
    tval r = {0};
    const uint32_t n = in->nops - 1;
    if (n != callee->nargs) { fail(c, "call to %s with %u arguments", callee->name, n); return r; }
    tval *args = ralloc_array(c->s, tval, n + 1);
    uint64_t *al = calloc(n + 1, 8);
    for (uint32_t i = 0; i < n; ++i) {
        args[i] = get(c, in->ops[1 + i]);
        al[i] = known_align(c, in->ops[1 + i], 0);
    }
    const uint32_t rett = T(c, in->callee_type)->elem;
    const uint32_t nl = count_leaves(c, rett);
    nir_variable **rv = nl ? rzalloc_array(c->s, nir_variable *, nl + 1) : NULL;
    uint32_t k = 0;
    if (nl) make_leaf_vars(c, rett, rv, &k);
    nir_block *cont = new_block(c);
    emit_function(c, callee, args, al, cont, rv, rett, depth + 1);
    free(al);
    c->b.cursor = nir_after_block(cont);
    if (nl && !c->failed) { k = 0; r = load_leaves(c, rett, rv, &k); }
    return r;
}

nir_shader *air_to_nir(air_module *m, air_function *f, const nir_shader_compiler_options *opts,
                       const air_variant *var, air_abi *abi, char *err, size_t errlen) {
    ctx c = {0};
    c.m = m; c.f = f; c.abi = abi; c.var = var; c.err = err; c.errlen = errlen;
    memset(abi, 0, sizeof(*abi));
    if (f->is_proto || !f->nblocks) { snprintf(err, errlen, "%s has no body", f->name); return NULL; }
    {
        int32_t an = -1, on = -1;
        if (find_entry(&c, &c.stage, &an, &on)) { snprintf(err, errlen, "%s is not an entry point", f->name); return NULL; }
        c.outputs_node = on;
        if ((c.stage == MESA_SHADER_VERTEX || c.stage == MESA_SHADER_TESS_EVAL) && !var->io) {
            /* no list given: this vertex stage's own output order */
            const air_md *outs = md(&c, on);
            size_t len = 0;
            for (uint32_t i = 0; outs && outs->kind == AM_NODE && i < outs->n; ++i) {
                const air_md *o = md(&c, outs->ops[i]);
                const char *nm = o && o->kind == AM_NODE ? arg_str_after(&c, o, "air.vertex_output") : NULL;
                if (!nm) continue;
                if (len + strlen(nm) + 2 >= sizeof abi->io) break;
                if (len) abi->io[len++] = ',';
                memcpy(abi->io + len, nm, strlen(nm));
                len += strlen(nm);
            }
            abi->io[len] = 0;
            c.io_auto = abi->io;
        }
    }
    memset(gOutVar, 0, sizeof gOutVar);
    c.s = nir_shader_create(NULL, c.stage, opts);
    c.s->info.name = ralloc_strdup(c.s, f->name);
    c.cp_node = -1;
    if (c.stage == MESA_SHADER_TESS_EVAL) {
        c.s->info.tess._primitive_mode = abi->tess_domain == 2 ? TESS_PRIMITIVE_QUADS : TESS_PRIMITIVE_TRIANGLES;
        c.s->info.tess.spacing = var->tess_spacing == 1 ? TESS_SPACING_FRACTIONAL_ODD
                               : var->tess_spacing == 2 ? TESS_SPACING_FRACTIONAL_EVEN : TESS_SPACING_EQUAL;
        c.s->info.tess.ccw = var->tess_ccw != 0;
        c.s->info.tess.point_mode = false;
    }
    if (c.stage == MESA_SHADER_COMPUTE) {
        c.s->info.workgroup_size[0] = var->block[0];
        c.s->info.workgroup_size[1] = var->block[1];
        c.s->info.workgroup_size[2] = var->block[2];
    }
    nir_function *fn = nir_function_create(c.s, "main");
    fn->is_entrypoint = true;
    c.impl = nir_function_impl_create(fn);
    c.impl->structured = false;
    c.b = nir_builder_at(nir_after_block(nir_start_block(c.impl)));
    c.vals = rzalloc_array(c.s, tval, f->nvalues + 1);
    c.have = rzalloc_array(c.s, uint8_t, f->nvalues + 1);

    place_globals(&c);
    if (getenv("NAKC_TG_GUARD") && c.stage == MESA_SHADER_COMPUTE) tg_guard_on(&c);
    if (!c.failed) setup_args(&c);
    if (c.failed) goto bad;
    if (abi->tg_bytes > 49152) { fail(&c, "threadgroup memory %u > 48 KiB", abi->tg_bytes); goto bad; }
    if (c.stage == MESA_SHADER_COMPUTE) c.s->info.shared_size = abi->tg_bytes;

    nir_block *first = new_block(&c);
    if (c.stage == MESA_SHADER_COMPUTE) {
        /* grid edge (dispatchThreads with a partial last threadgroup) */
        nir_def *inside = nir_ball(&c.b, nir_ult(&c.b, c.sys_gid, c.sys_grid));
        if (c.mesh_active) inside = nir_iand(&c.b, inside, c.mesh_active);   /* mesh: groups past the object's grid */
        nir_goto_if(&c.b, first, inside, c.impl->end_block);
    } else nir_goto(&c.b, first);
    c.b.cursor = nir_after_block(first);
    /* llvm.global_ctors (function constants are copied into their globals
     * there) run inline before the kernel body */
    for (uint32_t gi = 0; gi < m->nglobals && !c.failed; ++gi) {
        if (strcmp(m->globals[gi].name, "llvm.global_ctors") || !m->globals[gi].init) continue;
        const air_value *arr = &m->values[m->globals[gi].init - 1];
        for (uint32_t e = 0; arr->kind == AV_CAGG && e < arr->n; ++e) {
            const air_value *ent = &m->values[arr->elts[e]];
            if (ent->kind != AV_CAGG || ent->n < 2) continue;
            const air_value *fv = &m->values[ent->elts[1]];
            if (fv->kind != AV_FUNCTION) continue;
            air_function *ctor = &m->functions[fv->ival];
            nir_block *after = new_block(&c);
            emit_function(&c, ctor, (tval[1]){{0}}, (uint64_t[1]){0}, after, NULL, 0, 1);
            c.b.cursor = nir_after_block(after);
        }
    }
    emit_function(&c, f, NULL, NULL, c.impl->end_block, NULL, 0, 0);
    if (c.failed) goto bad;
    c.s->scratch_size = (c.scratch + 15) & ~15u;

    nir_lower_goto_ifs(c.s);
    nir_repair_ssa(c.s);
    NIR_PASS(_, c.s, nir_lower_vars_to_ssa);
    free(c.global_off);
    free(c.arg_align);
    free(c.gused);
    free(c.const_samp_slot);
    for (int li = 0; li < 16; ++li) { free(c.link_goff[li]); free(c.link_gused[li]); free(c.link_csamp[li]); }
    return c.s;
bad:
    free(c.global_off);
    free(c.arg_align);
    free(c.gused);
    free(c.const_samp_slot);
    for (int li = 0; li < 16; ++li) { free(c.link_goff[li]); free(c.link_gused[li]); free(c.link_csamp[li]); }
    free(abi->cdata);
    abi->cdata = NULL;
    ralloc_free(c.s);
    return NULL;
}


/* ---- [[visible]] / [[stitchable]] function arguments ----
 * What -[MTLFunction arguments] reports for a visible function (Core Image's
 * stitchable kernels ask for it): one "varg" line per input,
 *   varg <kind> <dataType> <elemOrTexType> <isConstant> <access> <texDataType> <name> <typeName>
 * kind 18 value, 28 pointer, 29 struct, 2 texture, 3 sampler (Apple's binding
 * types); struct fields follow as "vmem" lines (stitch_emit). */
/* a stitching type node -> what -[MTLFunction arguments] reports for it:
 * Apple binding kind (18 value, 28 pointer, 29 struct by value, 2 texture,
 * 3 sampler), MTLDataType, element data type (pointers) or texture type,
 * texture access and element data type, type name (structs). Matches the
 * M1's answers for Core Image's stitchable kernels. */
typedef struct {
    int kind;
    uint32_t dt, elem, access, tex_dt;
    const char *name;
    int32_t fields;   /* struct: the record_field list node, -1 none */
} stitch_ty;

static uint32_t stitch_scalar(const char *k, int64_t size) {
    if (!strcmp(k, "air.float_type")) return size == 2 ? 16 : 3;
    if (!strcmp(k, "air.half_type")) return 16;
    if (!strcmp(k, "air.bfloat_type")) return 121;
    if (!strcmp(k, "air.bool_type")) return 53;
    /* air.int_type / air.uint_type / air.short_type / air.uchar_type ... */
    if (strncmp(k, "air.", 4) || !strstr(k, "_type")) return 0;
    const char *b = k + 4;
    const int u = b[0] == 'u';
    if (u) ++b;
    int bytes = !strncmp(b, "char", 4) ? 1 : !strncmp(b, "short", 5) ? 2 : !strncmp(b, "long", 4) ? 8
              : (!strncmp(b, "int", 3) || !strncmp(b, "integer", 7)) ? (int)(size ? size : 4) : 0;
    if (!bytes) return 0;
    return bytes == 1 ? (u ? 49 : 45) : bytes == 2 ? (u ? 41 : 37) : bytes == 8 ? (u ? 85 : 81) : (u ? 33 : 29);
}

static void stitch_type(ctx *c, int32_t id, stitch_ty *o, int depth) {
    const air_md *t = md(c, id);
    memset(o, 0, sizeof *o);
    o->kind = 18; o->dt = 1; o->fields = -1;
    if (!t || t->kind != AM_NODE || !t->n || depth > 6) return;
    const char *k = air_md_string(c->m, t->ops[0]);
    if (!k) return;
    int64_t size = 0;
    if (t->n > 1) air_md_int(c->m, t->ops[1], &size);
    uint32_t sc = stitch_scalar(k, size);
    if (sc) { o->dt = sc; return; }
    if (!strcmp(k, "air.vector_type") && t->n > 5) {
        stitch_ty e;
        stitch_type(c, t->ops[4], &e, depth + 1);
        int64_t cnt = 4;
        air_md_int(c->m, t->ops[5], &cnt);
        const uint32_t n = cnt < 2 ? 0 : cnt > 4 ? 2 : (uint32_t)cnt - 1;   /* +1 .. +3 over the scalar */
        switch (e.dt) {
        case 3: case 16: case 29: case 33: case 37: case 41: case 45: case 49: case 53: case 81: case 85: case 121:
            o->dt = e.dt + n; return;
        default: return;
        }
    }
    if (!strcmp(k, "air.matrix_type")) {
        /* size, align, null, element, columns, rows: floatCxR */
        stitch_ty e;
        int64_t cols = 4, rows = 4;
        if (t->n > 4) stitch_type(c, t->ops[4], &e, depth + 1); else e.dt = 3;
        if (t->n > 5) air_md_int(c->m, t->ops[5], &cols);
        if (t->n > 6) air_md_int(c->m, t->ops[6], &rows);
        if (cols < 2 || cols > 4 || rows < 2 || rows > 4) { o->dt = 15; return; }
        o->dt = (e.dt == 16 ? 17u : 7u) + (uint32_t)(cols - 2) * 3 + (uint32_t)(rows - 2);
        return;
    }
    if (!strcmp(k, "air.struct_type")) {
        o->kind = 29; o->dt = 1;
        if (t->n > 4) o->name = air_md_string(c->m, t->ops[4]);
        if (t->n > 5) o->fields = t->ops[5];
        return;
    }
    if (!strcmp(k, "air.array_type")) { o->dt = 2; return; }
    if (!strcmp(k, "air.lvalue_reference_type") || !strcmp(k, "air.pointer_type") || !strcmp(k, "air.rvalue_reference_type")) {
        stitch_ty e;
        if (t->n > 4) stitch_type(c, t->ops[4], &e, depth + 1); else { memset(&e, 0, sizeof e); e.dt = 1; }
        o->kind = 28; o->dt = 60; o->elem = e.dt; o->name = e.name;
        return;
    }
    if (!strncmp(k, "air.texture", 11) || !strncmp(k, "air.depth", 9)) {
        /* air.texture_2d_type size align null <element> "sample"|"read"|"write"|"read_write" */
        const char *b = k + (k[4] == 't' ? 11 : 9);
        o->kind = 2; o->dt = 58;
        o->elem = !strncmp(b, "_1d_array", 9) ? 1 : !strncmp(b, "_1d", 3) ? 0 : !strncmp(b, "_2d_ms_array", 12) ? 8
                : !strncmp(b, "_2d_ms", 6) ? 4 : !strncmp(b, "_2d_array", 9) ? 3 : !strncmp(b, "_cube_array", 11) ? 6
                : !strncmp(b, "_cube", 5) ? 5 : !strncmp(b, "_3d", 3) ? 7 : !strncmp(b, "_buffer", 7) ? 9 : 2;
        stitch_ty e;
        if (t->n > 4) stitch_type(c, t->ops[4], &e, depth + 1); else e.dt = 3;
        o->tex_dt = e.dt ? e.dt : 3;
        const char *acc = t->n > 5 ? air_md_string(c->m, t->ops[5]) : NULL;
        o->access = !acc ? 0 : !strcmp(acc, "write") ? 2 : !strcmp(acc, "read_write") ? 1 : 0;
        return;
    }
    if (!strncmp(k, "air.sampler", 11)) { o->kind = 3; o->dt = 59; return; }
}

/* one "varg" line and, for a struct, a "vmem" line per field:
 *   varg <kind> <dataType> <elemOrTexType> <isConstant> <access> <texDataType> <name> <typeName>
 *   vmem <offset> <kind> <dataType> <elemOrTexType> <access> <texDataType> <name> */
static int stitch_emit(ctx *c, const char *tag, const stitch_ty *ty, const char *nm, int cst, char *out, size_t outlen, size_t *pos) {
    int n = snprintf(out + *pos, outlen - *pos, "%s %d %u %u %d %u %u %s %s\n", tag, ty->kind, ty->dt, ty->elem, cst, ty->access,
                     ty->tex_dt, nm ? nm : "arg", ty->name ? ty->name : "-");
    if (n < 0 || (size_t)n >= outlen - *pos) return -1;
    *pos += (size_t)n;
    const air_md *fl = ty->fields >= 0 ? md(c, ty->fields) : NULL;
    for (uint32_t i = 0; fl && fl->kind == AM_NODE && i < fl->n; ++i) {
        /* air.record_field <offset> <size> <type> "<name>" null */
        const air_md *r = md(c, fl->ops[i]);
        if (!r || r->kind != AM_NODE || r->n < 5) continue;
        const char *rk = air_md_string(c->m, r->ops[0]);
        if (!rk || strcmp(rk, "air.record_field")) continue;
        int64_t off = 0;
        air_md_int(c->m, r->ops[1], &off);
        stitch_ty f;
        stitch_type(c, r->ops[3], &f, 1);
        const char *fn = air_md_string(c->m, r->ops[4]);
        n = snprintf(out + *pos, outlen - *pos, "vmem %lld %d %u %u %u %u %s\n", (long long)off, f.kind,
                     f.kind == 29 ? 1u : f.dt, f.elem, f.access, f.tex_dt, fn ? fn : "field");
        if (n < 0 || (size_t)n >= outlen - *pos) return -1;
        *pos += (size_t)n;
    }
    return 0;
}

int air_visible_reflection(air_module *m, air_function *f, char *out, size_t outlen) {
    ctx c = {0};
    c.m = m; c.f = f;
    const air_named_md *vis = air_named(m, "air.visible");
    size_t pos = 0;
    out[0] = 0;
    for (uint32_t i = 0; vis && i < vis->n; ++i) {
        const air_md *e = md(&c, (int32_t)vis->ops[i]);
        if (!e || e->kind != AM_NODE || e->n < 3) continue;
        const air_md *fv = md(&c, e->ops[0]);
        if (!fv || fv->kind != AM_VALUE || fv->value != f->value) continue;
        /* [[stitchable]]: the source-level arguments are the stitching
         * arguments (a coreimage::Sampler is one argument there, four
         * flattened inputs in the visible list) */
        const air_md *si = e->n > 3 ? md(&c, e->ops[3]) : NULL;
        if (si && si->kind == AM_NODE && si->n && air_md_string(m, si->ops[0]) &&
            !strcmp(air_md_string(m, si->ops[0]), "air.stitching_info")) {
            /* -[MTLFunction returnType]: Core Image wires a node's result
             * into the next one by it (nil and it drops the rest of the pass,
             * write included). void is dataType 61 on the M1. */
            stitch_ty rt;
            memset(&rt, 0, sizeof rt); rt.kind = 18; rt.dt = 61; rt.fields = -1;
            const air_md *rs = si->n > 1 ? md(&c, si->ops[1]) : NULL;
            if (rs && rs->kind == AM_NODE && rs->n > 1) stitch_type(&c, rs->ops[1], &rt, 0);
            if (stitch_emit(&c, "vret", &rt, "-", 0, out, outlen, &pos)) return -1;
            for (uint32_t k = 2; k < si->n; ++k) {
                const air_md *a = md(&c, si->ops[k]);
                if (!a || a->kind != AM_NODE || a->n < 3) continue;
                const char *ak = air_md_string(m, a->ops[0]);
                if (!ak || strcmp(ak, "air.stitching_argument")) continue;
                const air_md *st = md(&c, a->ops[1]);
                const char *nm = air_md_string(m, a->ops[a->n - 1]);
                stitch_ty ty;
                memset(&ty, 0, sizeof ty); ty.kind = 18; ty.dt = 1; ty.fields = -1;
                if (st && st->kind == AM_NODE && st->n > 1) stitch_type(&c, st->ops[1], &ty, 0);
                if (stitch_emit(&c, "varg", &ty, nm, 0, out, outlen, &pos)) return -1;
            }
            return 0;
        }
        {
            const air_md *outs = md(&c, e->ops[1]);
            const air_md *o0 = outs && outs->kind == AM_NODE && outs->n ? md(&c, outs->ops[0]) : NULL;
            const char *otn = o0 && o0->kind == AM_NODE ? arg_str_after(&c, o0, "air.arg_type_name") : NULL;
            const uint32_t odt = otn ? mtl_data_type(otn) : 61;
            const int n = snprintf(out + pos, outlen - pos, "vret 18 %u 0 0 0 0 - -\n", odt ? odt : 61);
            if (n < 0 || (size_t)n >= outlen - pos) return -1;
            pos += (size_t)n;
        }
        const air_md *ins = md(&c, e->ops[2]);
        const air_type *ft = &m->types[f->type];
        for (uint32_t k = 0; ins && ins->kind == AM_NODE && k < ins->n; ++k) {
            const air_md *a = md(&c, ins->ops[k]);
            if (!a || a->kind != AM_NODE || a->n < 2) continue;
            int64_t idx = 0;
            if (air_md_int(m, a->ops[0], &idx)) continue;
            const char *tn = arg_str_after(&c, a, "air.arg_type_name");
            const char *nm = arg_str_after(&c, a, "air.arg_name");
            int kind = 18, tt = 0, cst = 0;
            uint32_t dt = mtl_data_type(tn);
            if (tn && !strncmp(tn, "__metal_texture", 15)) {
                kind = 2; dt = 58;
                const char *b = tn + 15;
                tt = !strncmp(b, "_1d_array", 9) ? 1 : !strncmp(b, "_1d", 3) ? 0 : !strncmp(b, "_2d_ms_array", 12) ? 8
                   : !strncmp(b, "_2d_ms", 6) ? 4 : !strncmp(b, "_2d_array", 9) ? 3 : !strncmp(b, "_cube_array", 11) ? 6
                   : !strncmp(b, "_cube", 5) ? 5 : !strncmp(b, "_3d", 3) ? 7 : !strncmp(b, "_buffer", 7) ? 9 : 2;
            } else if (tn && !strncmp(tn, "__metal_sampler", 15)) {
                kind = 3; dt = 59;
            } else if (idx >= 0 && (uint32_t)idx < ft->count && m->types[ft->members[idx]].kind == AT_PTR) {
                kind = 28; cst = m->types[ft->members[idx]].addrspace == 2;
            }
            /* the type name goes last (it can hold spaces): Core Image looks
             * struct arguments (coreimage::sampler ...) up by it */
            const int acc = kind == 2 && tn ? (strstr(tn, "read_write") ? 1 : strstr(tn, "write") ? 2 : 0) : 0;
            const int n = snprintf(out + pos, outlen - pos, "varg %d %u %d %d %d %u %s %s\n", kind, kind == 28 ? 60u : dt,
                                   kind == 28 ? (int)dt : tt, cst, acc, kind == 2 ? 3u : 0u, nm ? nm : "arg", tn ? tn : "-");
            if (n < 0 || (size_t)n >= outlen - pos) return -1;
            pos += (size_t)n;
        }
        return 0;
    }
    return -1;
}
