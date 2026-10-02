/* AIR (precompiled Metal shader bitcode) -> NIR, for nakc --air.
 *
 * Compute kernel ABI (push constants, cbuf 0), the same as the MetalSL path in
 * NVMTLCompiler.m so the runtime fills both the same way:
 *   u64 buffer[nbuf]            [[buffer(i)]] device addresses (0 if unused)
 *   u64 texture[ntex]           low word texture header index, high word flags
 *                               (bit 0: stored red/blue swapped, BGRA)
 *   u32 sampler[nsamp + nconst_samp]  sampler index; the constexpr samplers
 *                               of the shader follow the argument ones
 *   u32 gX, gY, gZ              threads in the grid
 *   u32 kX, kY, kZ              threads per threadgroup (the variant's shape)
 *   u64 cdata                   only when cdata_bytes != 0: the kernel's
 *                               constant-address-space globals, which the
 *                               runtime uploads once from air_abi.cdata
 *
 * Fragment shaders that read their colour attachments ([[color(n)]] inputs,
 * framebuffer fetch) get one more u64 per attachment at rt_read_offset (8-byte
 * aligned, after everything above): the texture header index of attachment n,
 * read with txf at the pixel's integer position.
 */
#ifndef AIR_NIR_H
#define AIR_NIR_H
#include "compiler/nir/nir.h"
#include "airir.h"

typedef struct {
    uint32_t nbuf;                 /* buffer slots (max index + 1) */
    uint32_t ntex, nsamp;          /* [[texture(i)]] / [[sampler(i)]] slots (max index + 1) */
    uint32_t grid_offset;          /* byte offset of gX in the push block */
    uint32_t cdata_offset;         /* byte offset of the cdata pointer, 0 = none */
    uint32_t push_bytes;
    uint32_t tg_bytes;             /* threadgroup memory used */
    uint8_t *cdata;                /* initial bytes of constant globals (malloc'd) */
    uint32_t cdata_bytes;
    uint32_t tg_arg_offset[32];    /* [[threadgroup(i)]] offsets in shared memory */
    uint32_t nconst_samp;          /* constexpr samplers, in slots nsamp .. nsamp + n - 1 */
    uint32_t rt_read_mask;         /* fragment: colour attachments read as inputs (bit n = [[color(n)]]) */
    uint32_t rt_read_offset;       /* byte offset of their u64 texture handles, 0 = none */
    uint64_t const_samp[16];       /* their AIR sampler-state bits */
    char io[2048];                 /* vertex stage: the varying list it used (VAR0, VAR1, ...) */
    uint32_t stage;                /* 0 compute, 1 vertex, 2 fragment, 3 post-tessellation vertex */
    uint32_t tess_domain, tess_cps; /* post-tessellation vertex: 1 triangle / 2 quad, control points */
    char *refl;                    /* pipeline reflection lines (malloc'd): see build_reflection() */
    /* mesh shaders (Metal 3), run as compute: 1 object, 2 mesh. slot = first of three hidden buffer slots after
     * the app's: payloads, mesh grids (u32 x4 per object threadgroup), mesh output (one group_stride block per
     * mesh threadgroup: u32 primitive count, 12 pad, max_v vertices of vstride bytes (position, then each
     * mesh_vertex_data as a 32-bit vec4), max_p * vpp u32 indices) */
    uint32_t mesh_kind, mesh_slot, mesh_payload_stride, mesh_max_v, mesh_max_p, mesh_nvdata, mesh_vstride,
             mesh_group_stride, mesh_vpp;
} air_abi;

typedef struct {
    uint32_t block[3];             /* threadgroup shape the variant is built for */
    uint32_t tg_arg_bytes[32];     /* setThreadgroupMemoryLength:atIndex: */
    uint32_t fc_defined[4];        /* function constants given (bit per index, 0..127) */
    uint64_t fc_value[128];        /* their bits, little endian in the constant's type */
    uint64_t fc_value_hi[128];     /* bytes 8..15 of a vector constant */
    const char *io;                /* vertex/fragment: comma-separated varying names; the
                                      i-th gets VARYING_SLOT_VAR0 + i in both stages */
    uint32_t tess_spacing;         /* post-tessellation vertex: 0 equal, 1 fractional odd, 2 fractional even */
    uint32_t tess_ccw;             /* 1: counter-clockwise output triangles */
    /* compute [[stage_in]] from MTLComputePipelineDescriptor.stageInputDescriptor
     * (nakc args si<attr>=format,offset,buffer  sb<buffer>=stride,step,rate
     * sx=indexType,indexBuffer). MTLAttributeFormat / MTLStepFunction values. */
    uint16_t si_fmt[31];           /* 0 = attribute not given */
    uint32_t si_off[31];
    uint8_t si_buf[31];
    uint32_t sb_stride[31];
    uint8_t sb_step[31];
    uint32_t sb_rate[31];
    uint8_t sx_given, sx_type;     /* index buffer: type 0 uint16, 1 uint32 */
    uint8_t sx_buf;
    /* linked functions ([[visible]] ones the shader calls as NAME.MTL_VISIBLE_FN_REF),
     * from nakc args link=NAME@PATH: inlined from their own module */
    uint32_t nlink;
    const char *link_name[16];
    air_module *link_mod[16];
    air_function *link_fn[16];
} air_variant;

/* Build the shader for entry point `f` (kernel, vertex or fragment, from its
 * !air.* metadata). Vertex/fragment use the same push layout without the
 * grid words. NULL on failure (reason in err). */
nir_shader *air_to_nir(air_module *m, air_function *f, const nir_shader_compiler_options *opts,
                       const air_variant *var, air_abi *abi, char *err, size_t errlen);
/* the "varg" lines of a [[visible]] function (see air_nir.c), 0 on success */
int air_visible_reflection(air_module *m, air_function *f, char *out, size_t outlen);
#endif
