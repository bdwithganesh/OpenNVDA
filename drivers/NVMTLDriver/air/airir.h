/* In-memory form of an AIR (Apple's LLVM IR for Metal) module, read from
 * the bitstream tree. Covers what the Metal front end emits for shaders:
 * typed pointers with address spaces, no exceptions, no varargs. */
#ifndef AIR_IR_H
#define AIR_IR_H
#include <stdint.h>
#include "bitstream.h"

typedef enum {
    AT_VOID, AT_FLOAT, AT_INT, AT_PTR, AT_VEC, AT_ARRAY, AT_STRUCT, AT_FUNC, AT_LABEL,
    AT_METADATA, AT_OPAQUE, AT_OTHER
} air_type_kind;

typedef struct {
    air_type_kind kind;
    uint32_t bits;          /* FLOAT/INT width */
    uint32_t elem;          /* PTR pointee, VEC/ARRAY element, FUNC return */
    uint32_t count;         /* VEC/ARRAY length, STRUCT/FUNC member count */
    uint32_t *members;      /* STRUCT members, FUNC params */
    uint32_t addrspace;     /* PTR */
    int packed, vararg;
    char *name;             /* named struct */
} air_type;

typedef enum {
    AV_GLOBAL, AV_FUNCTION, AV_CINT, AV_CFP, AV_CNULL, AV_CUNDEF, AV_CAGG, AV_CDATA,
    AV_CEXPR, AV_ARG, AV_INST, AV_BAD
} air_value_kind;

/* instruction opcodes (ours, not LLVM's numbering) */
typedef enum {
    AI_BINOP, AI_CAST, AI_GEP, AI_SELECT, AI_EXTRACTELT, AI_INSERTELT, AI_SHUFFLE,
    AI_CMP, AI_RET, AI_BR, AI_SWITCH, AI_UNREACHABLE, AI_PHI, AI_ALLOCA, AI_LOAD,
    AI_STORE, AI_CALL, AI_EXTRACTVAL, AI_INSERTVAL, AI_UNOP, AI_ATOMICRMW, AI_CMPXCHG,
    AI_FENCE, AI_FREEZE
} air_op;

typedef struct {
    air_op op;
    uint32_t sub;           /* binop / cast / cmp predicate / rmw op code */
    uint32_t type;          /* result type (void for none) */
    uint32_t nops;
    uint32_t *ops;          /* value ids; BR/SWITCH/PHI block ids are in `blocks` */
    uint32_t nblocks;
    uint32_t *blocks;
    uint32_t nidx;          /* EXTRACTVAL/INSERTVAL constant indices, GEP inbounds */
    uint32_t *idx;
    uint32_t align, flags, callee_type, value;  /* value: result value id or ~0 */
    uint32_t bb;            /* block this instruction is in */
} air_inst;

typedef struct {
    air_value_kind kind;
    uint32_t type;
    uint64_t ival;          /* CINT (sign-extended), CFP raw bits, ARG index */
    uint32_t n, *elts;      /* CAGG / CEXPR operands */
    uint64_t *data;         /* CDATA elements */
    uint32_t sub, sub2;     /* CEXPR opcode kind / cast op */
    uint32_t inst;          /* AV_INST: index into the function's inst array */
} air_value;

typedef struct {
    char *name;
    uint32_t type;          /* function type */
    int is_proto;
    uint32_t value;         /* its value id */
    /* body */
    uint32_t nargs, first_arg;  /* args are values first_arg .. first_arg + nargs - 1 */
    uint32_t nblocks;
    uint32_t ninsts;
    air_inst *insts;
    air_value *values;      /* function-local value table (module values + locals) */
    uint32_t nvalues;
} air_function;

typedef struct {
    char *name;
    uint32_t type;          /* pointer type of the global */
    uint32_t value_type, addrspace, init; /* init: value id + 1, 0 = none */
    int is_const;
} air_global;

/* metadata: strings and nodes share one id space */
typedef enum { AM_STRING, AM_VALUE, AM_NODE, AM_OTHER } air_md_kind;
typedef struct {
    air_md_kind kind;
    char *str;                   /* STRING */
    uint32_t type, value;        /* VALUE: type id, module value id */
    uint32_t n;
    int32_t *ops;                /* NODE: md ids, -1 = null */
} air_md;

typedef struct { char *name; uint32_t n; uint32_t *ops; } air_named_md;

typedef struct {
    uint32_t ntypes;
    air_type *types;
    uint32_t nvalues;            /* module-level values */
    air_value *values;
    uint32_t nglobals;
    air_global *globals;
    uint32_t nfunctions;
    air_function *functions;
    uint32_t nmd;
    air_md *md;
    uint32_t nnamed;
    air_named_md *named;
    uint32_t version;
} air_module;

int air_read(const uint8_t *data, size_t len, air_module *m, char *err, size_t errlen);
void air_free(air_module *m);
const air_named_md *air_named(const air_module *m, const char *name);
air_function *air_function_by_value(air_module *m, uint32_t value);
const char *air_md_string(const air_module *m, int32_t id);   /* NULL if not a string */
int air_md_int(const air_module *m, int32_t id, int64_t *out); /* VALUE holding a CINT */
#endif
