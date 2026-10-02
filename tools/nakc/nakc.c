/*
 * nakc: offline SPIR-V -> sm_89 SASS compiler for the RTX 4080 macOS driver,
 * built on Mesa's NAK (src/nouveau/compiler) with the macOS NAK-only Mesa
 * build (upstream/mesa-26.0.8/build-nak).
 *
 * Supported shader interface (enough for compute kernels, OpenCL/CUDA style):
 *   - push constants  -> constant buffer 0 at offset 0 (ldc_nv c[0x0][off])
 *   - buffer_reference / PhysicalStorageBuffer (64-bit global addresses)
 *   - shared memory, workgroup/local ids, barriers
 * Descriptor sets (SSBO/UBO/images) are not supported: pass device addresses
 * through push constants.
 *
 * Output file (.nak, little endian):
 *   u32 magic 'NAK1', u32 code_bytes, u32 num_gprs, u32 slm_bytes,
 *   u32 smem_bytes, u32 local_size[3], u32 num_barriers, u32 push_bytes,
 *   u32 reserved[6], code[code_bytes]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler/glsl_types.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/spirv/nir_spirv.h"
#include "spirv_info.h"
#include "nak.h"
#include "nv_device_info.h"
#include "air_nir.h"
#include "tess_gen.h"
#include "metallib.h"

static const struct nv_device_info ad103 = {
   .type = NV_DEVICE_TYPE_DIS,
   .device_id = 0x2704,
   .chipset = 0x193,
   .device_name = "NVIDIA GeForce RTX 4080",
   .chipset_name = "AD103",
   .sm = 89,
   .gpc_count = 7,
   .tpc_count = 38,
   .mp_per_tpc = 2,
   .max_warps_per_mp = 48,
   .cls_copy = 0xc7b5,
   .cls_eng2d = 0x902d,
   .cls_eng3d = 0xc997,
   .cls_m2mf = 0xa140,
   .cls_compute = 0xc9c0,
   .cls_gpfifo = 0xc86f,
   .vram_size_B = 16376ull << 20,
   .max_smem_per_wg_kB = 99,
   .sm_smem_sizes_kB = {0, 8, 16, 32, 64, 100},
   .sm_smem_size_count = 6,
};

static bool
lower_push_const(nir_builder *b, nir_intrinsic_instr *load, void *data)
{
   if (load->intrinsic != nir_intrinsic_load_push_constant)
      return false;
   b->cursor = nir_before_instr(&load->instr);
   nir_def *off = nir_iadd_imm(b, load->src[0].ssa, nir_intrinsic_base(load));
   nir_def *val = nir_ldc_nv(b, load->def.num_components, load->def.bit_size,
                             nir_imm_int(b, 0), off,
                             .align_mul = load->def.bit_size / 8,
                             .align_offset = 0);
   nir_def_rewrite_uses(&load->def, val);
   nir_instr_remove(&load->instr);
   return true;
}

static void
spirv_debug(void *priv, enum nir_spirv_debug_level level, size_t off,
            const char *msg)
{
   fprintf(stderr, "spirv[%zu]: %s\n", off, msg);
}

static void
shared_var_info(const struct glsl_type *type, unsigned *size, unsigned *align)
{
   glsl_get_natural_size_align_bytes(type, size, align);
}

/* Execution model of the first OpEntryPoint (SPIR-V opcode 15). */
static mesa_shader_stage
spirv_stage(const uint32_t *w, size_t n)
{
   for (size_t i = 5; i < n;) {
      const uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
      if (!len) break;
      if (op == 15) {
         switch (w[i + 1]) {
         case 0: return MESA_SHADER_VERTEX;
         case 4: return MESA_SHADER_FRAGMENT;
         default: return MESA_SHADER_COMPUTE;
         }
      }
      i += len;
   }
   return MESA_SHADER_COMPUTE;
}

/* Textures: combined image samplers of set 0 become bindless handles
 * (TIC index = TSC index = binding): handle = binding | binding << 20,
 * the NVK image-descriptor encoding. */
static bool
lower_tex_handles(nir_builder *b, nir_instr *instr, void *data)
{
   if (instr->type != nir_instr_type_tex) return false;
   nir_tex_instr *tex = nir_instr_as_tex(instr);
   nir_deref_instr *t = nir_steal_tex_deref(tex, nir_tex_src_texture_deref);
   nir_deref_instr *smp = nir_steal_tex_deref(tex, nir_tex_src_sampler_deref);
   (void)smp;
   if (!t) return false;
   nir_variable *var = nir_deref_instr_get_variable(t);
   const uint32_t binding = var ? var->data.binding : 0;
   b->cursor = nir_before_instr(&tex->instr);
   nir_tex_instr_add_src(tex, nir_tex_src_texture_handle,
                         nir_imm_int(b, binding | (binding << 20)));
   return true;
}

static void
assign_io_locations(nir_shader *nir)
{
   nir_foreach_shader_in_variable(var, nir)
      var->data.driver_location = var->data.location;
   nir_foreach_shader_out_variable(var, nir)
      var->data.driver_location = var->data.location;
}

/* Derivatives in divergent control flow. NVIDIA forms dFdx/dFdy (and a texture's implicit LOD) from
 * the other lanes of the 2x2 quad; lanes switched off by an if read as 0, so a derivative inside a
 * branch the quad does not take together comes out wrong. Metal calls that undefined, but Apple's
 * GPUs get it right and RenderBox's anti-aliasing depends on it (partial-alpha lines along triangle
 * edges, 1 Oct 2026). Each derivative inside an if/loop moves, with the side-effect-free chain it
 * depends on, to just before the outermost control flow around it, where the whole quad runs it.
 * (Implicit-LOD samples in control flow are not converted yet; see hoist_divergent_derivatives.) Chains that reach a phi, a memory load that
 * could fault, or another texture stay put. */
static bool
deriv_speculatable(nir_instr *in)
{
   switch (in->type) {
   case nir_instr_type_alu: case nir_instr_type_load_const: case nir_instr_type_undef:
      return true;
   case nir_instr_type_intrinsic:
      switch (nir_instr_as_intrinsic(in)->intrinsic) {
      case nir_intrinsic_load_interpolated_input: case nir_intrinsic_load_input:
      case nir_intrinsic_load_barycentric_pixel: case nir_intrinsic_load_barycentric_centroid:
      case nir_intrinsic_load_barycentric_sample: case nir_intrinsic_load_barycentric_at_offset:
      case nir_intrinsic_load_frag_coord: case nir_intrinsic_ldc_nv:
      case nir_intrinsic_load_ubo: case nir_intrinsic_load_push_constant: case nir_intrinsic_load_sample_id:
      case nir_intrinsic_load_front_face: case nir_intrinsic_load_point_coord:
      case nir_intrinsic_ddx: case nir_intrinsic_ddx_fine: case nir_intrinsic_ddx_coarse:
      case nir_intrinsic_ddy: case nir_intrinsic_ddy_fine: case nir_intrinsic_ddy_coarse:
         return true;
      default:
         return false;
      }
   default:
      return false;
   }
}

static bool
inside_cf(nir_block *blk, nir_cf_node *outer)
{
   for (nir_cf_node *n = &blk->cf_node; n; n = n->parent)
      if (n == outer) return true;
   return false;
}

static bool deriv_collect(nir_instr *in, nir_cf_node *outer, nir_instr **list, unsigned *n, unsigned max);
static bool
deriv_src_cb(nir_src *src, void *data)
{
   struct { nir_cf_node *outer; nir_instr **list; unsigned *n, max; bool ok; } *st = data;
   if (!deriv_collect(nir_def_instr(src->ssa), st->outer, st->list, st->n, st->max)) st->ok = false;
   return st->ok;
}

/* post-order: sources first. Instructions already outside `outer` are leaves. */
static bool
deriv_collect(nir_instr *in, nir_cf_node *outer, nir_instr **list, unsigned *n, unsigned max)
{
   if (!inside_cf(in->block, outer) || in->pass_flags) return true;
   if (in->type == nir_instr_type_phi || !deriv_speculatable(in) || *n >= max) return false;
   in->pass_flags = 1;
   struct { nir_cf_node *outer; nir_instr **list; unsigned *n, max; bool ok; } st = { outer, list, n, max, true };
   nir_foreach_src(in, deriv_src_cb, &st);
   if (!st.ok) return false;
   list[(*n)++] = in;
   return true;
}

static bool
is_deriv(nir_instr *in)
{
   if (in->type != nir_instr_type_intrinsic) return false;
   switch (nir_instr_as_intrinsic(in)->intrinsic) {
   case nir_intrinsic_ddx: case nir_intrinsic_ddx_fine: case nir_intrinsic_ddx_coarse:
   case nir_intrinsic_ddy: case nir_intrinsic_ddy_fine: case nir_intrinsic_ddy_coarse:
      return true;
   default:
      return false;
   }
}

/* implicit-LOD sample in control flow -> txd with ddx/ddy of its coordinate */
static bool
tex_to_txd(nir_builder *b, nir_tex_instr *tex)
{
   if (tex->op != nir_texop_tex || tex->is_shadow || tex->sampler_dim == GLSL_SAMPLER_DIM_CUBE ||
       (tex->sampler_dim != GLSL_SAMPLER_DIM_2D && tex->sampler_dim != GLSL_SAMPLER_DIM_3D))
      return false;
   const int ci = nir_tex_instr_src_index(tex, nir_tex_src_coord);
   if (ci < 0 || nir_tex_instr_src_index(tex, nir_tex_src_offset) >= 0 ||
       nir_tex_instr_src_index(tex, nir_tex_src_min_lod) >= 0)
      return false;
   b->cursor = nir_before_instr(&tex->instr);
   nir_def *coord = tex->src[ci].src.ssa;
   const unsigned nc = tex->coord_components - (tex->is_array ? 1 : 0);
   nir_def *cc = nir_trim_vector(b, coord, nc);
   if (cc->bit_size != 32) cc = nir_f2f32(b, cc);
   nir_def *dx = nir_ddx(b, cc), *dy = nir_ddy(b, cc);
   if (coord->bit_size != 32) { dx = nir_f2fN(b, dx, coord->bit_size); dy = nir_f2fN(b, dy, coord->bit_size); }
   tex->op = nir_texop_txd;
   nir_tex_instr_add_src(tex, nir_tex_src_ddx, dx);
   nir_tex_instr_add_src(tex, nir_tex_src_ddy, dy);
   return true;
}

static void
hoist_divergent_derivatives(nir_shader *nir)
{
   if (nir->info.stage != MESA_SHADER_FRAGMENT) return;
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_builder b = nir_builder_create(impl);
   /* Implicit-LOD samples are left alone for now: turning them into txd overflowed NAK's texture
    * lowering (stack smash in nak_nir_lower_tex, WindowServer crash loop at login with 0.8.41,
    * 1 Oct 15:08). tex_to_txd() stays for when that is understood. */
   (void)b; (void)tex_to_txd;
   enum { MAXL = 512 };
   nir_instr *list[MAXL];
   unsigned moved = 0;
   bool progress = true;
   while (progress) {
      progress = false;
      nir_foreach_block(blk, impl) {
         if (blk->cf_node.parent == &impl->cf_node) continue;
         nir_foreach_instr(in, blk) {
            if (!is_deriv(in)) continue;
            nir_cf_node *outer = &blk->cf_node;
            while (outer->parent != &impl->cf_node) outer = outer->parent;
            nir_foreach_block(b2, impl) nir_foreach_instr(i2, b2) i2->pass_flags = 0;
            unsigned n = 0;
            if (!deriv_collect(in, outer, list, &n, MAXL)) continue;
            const nir_cursor at = nir_before_cf_node(outer);
            for (unsigned k = 0; k < n; ++k) nir_instr_move(at, list[k]);
            moved++;
            progress = true;
            break;   /* the block changed: start over */
         }
         if (progress) break;
      }
   }
   nir_foreach_block(b2, impl) nir_foreach_instr(i2, b2) i2->pass_flags = 0;
   if (moved) nir_progress(true, impl, nir_metadata_none);
}

/* Helper lanes and memory. The lanes that only exist to complete a 2x2 quad (pixels outside the
 * triangle) get 0 from global loads on NVIDIA; Apple's GPUs load real data there. Metal buffers are
 * plain addresses in our ABI (ld.global, not constant buffers), so a derivative of anything read
 * from a buffer is garbage in every quad that straddles a triangle edge: RenderBox reads its shape
 * sizes from memory and its anti-aliasing went to 0 along triangle edges (seams in icons, 1 Oct
 * 2026; metal_deriv_edges_test buffer: 1907 of 4053 pixels wrong). Every global load whose value
 * reaches a derivative or an implicit-LOD texture sample is patched: helper lanes take the value
 * of the first live lane of their quad. Exact when the address is the same across the quad (the
 * usual case: uniforms in a buffer), a neighbour's value otherwise (better than 0). */
struct helper_walk { nir_instr *v[4096]; unsigned n; };

static bool
helper_mark_cb(nir_src *src, void *data)
{
   struct helper_walk *w = data;
   nir_instr *p = nir_def_instr(src->ssa);
   if (!p->pass_flags) {
      p->pass_flags = 1;
      if (w->n < 4096) w->v[w->n++] = p;
   }
   return true;
}

static nir_def *
helper_shuffle32(nir_builder *b, nir_def *v, nir_def *lane)
{
   nir_def *c[NIR_MAX_VEC_COMPONENTS];
   for (unsigned i = 0; i < v->num_components; ++i) {
      nir_def *x = nir_channel(b, v, i);
      if (x->bit_size == 64) {
         nir_def *lo = nir_shuffle(b, nir_unpack_64_2x32_split_x(b, x), lane);
         nir_def *hi = nir_shuffle(b, nir_unpack_64_2x32_split_y(b, x), lane);
         c[i] = nir_pack_64_2x32_split(b, lo, hi);
      } else if (x->bit_size < 32) {
         c[i] = nir_u2uN(b, nir_shuffle(b, nir_u2u32(b, x), lane), x->bit_size);
      } else {
         c[i] = nir_shuffle(b, x, lane);
      }
   }
   return nir_vec(b, c, v->num_components);
}

static void
fix_helper_loads(nir_shader *nir)
{
   if (nir->info.stage != MESA_SHADER_FRAGMENT) return;
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_foreach_block(blk, impl) nir_foreach_instr(in, blk) in->pass_flags = 0;
   /* backward from every derivative and implicit-LOD sample: what feeds them */
   static struct helper_walk w;
   w.n = 0;
   nir_foreach_block(blk, impl) {
      nir_foreach_instr(in, blk) {
         bool root = is_deriv(in);
         if (in->type == nir_instr_type_tex) {
            nir_tex_instr *t = nir_instr_as_tex(in);
            root = t->op == nir_texop_tex || t->op == nir_texop_txb || t->op == nir_texop_lod;
         }
         if (root) nir_foreach_src(in, helper_mark_cb, &w);
      }
   }
   while (w.n) {
      nir_instr *in = w.v[--w.n];
      nir_foreach_src(in, helper_mark_cb, &w);
   }
   nir_builder b = nir_builder_create(impl);
   unsigned patched = 0;
   nir_foreach_block(blk, impl) {
      nir_foreach_instr_safe(in, blk) {
         if (!in->pass_flags || in->type != nir_instr_type_intrinsic) continue;
         nir_intrinsic_instr *ld = nir_instr_as_intrinsic(in);
         if (ld->intrinsic != nir_intrinsic_load_global && ld->intrinsic != nir_intrinsic_load_global_constant)
            continue;
         b.cursor = nir_after_instr(in);
         nir_def *v = &ld->def;
         nir_def *helper = nir_load_helper_invocation(&b, 1);
         nir_def *quad = nir_iand_imm(&b, nir_load_subgroup_invocation(&b), ~3u);
         nir_def *pick = NULL;
         for (int k = 3; k >= 0; --k) {
            nir_def *lane = nir_iadd_imm(&b, quad, k);
            nir_def *vk = helper_shuffle32(&b, v, lane);
            nir_def *hk = nir_ine_imm(&b, nir_shuffle(&b, nir_b2i32(&b, helper), lane), 0);
            pick = pick ? nir_bcsel(&b, hk, pick, vk) : vk;
         }
         nir_def *res = nir_bcsel(&b, helper, pick, v);
         nir_def_rewrite_uses_after(v, res);
         patched++;
      }
   }
   nir_foreach_block(blk, impl) nir_foreach_instr(in, blk) in->pass_flags = 0;
   if (patched) nir_progress(true, impl, nir_metadata_none);
}

/* the body of `name` from a metallib or a bare AIR file */
static int air_load_function(const char *path, const char *name, air_module *m, air_function **fn,
                             char *err, size_t errlen) {
   FILE *f = fopen(path, "rb");
   if (!f) { snprintf(err, errlen, "cannot open %s", path); return -1; }
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   rewind(f);
   uint8_t *b = malloc(n);
   if (fread(b, 1, n, f) != (size_t)n) { fclose(f); snprintf(err, errlen, "short read"); return -1; }
   fclose(f);
   const uint8_t *bc = b;
   size_t len = n;
   mtllib lib;
   char e2[256];
   if (!mtllib_parse(b, n, &lib, e2, sizeof e2)) {
      bc = NULL;
      for (uint32_t i = 0; i < lib.nfunctions; ++i)
         if (!strcmp(lib.functions[i].name, name)) { bc = lib.functions[i].bitcode; len = lib.functions[i].bitcode_len; }
      if (!bc) { snprintf(err, errlen, "no function %s in %s", name, path); return -1; }
   }
   if (air_read(bc, len, m, err, errlen)) return -1;
   *fn = NULL;
   for (uint32_t i = 0; i < m->nfunctions; ++i)
      if (!strcmp(m->functions[i].name, name) && !m->functions[i].is_proto) *fn = &m->functions[i];
   if (!*fn) { snprintf(err, errlen, "no body for %s", name); return -1; }
   return 0;
}

int main(int argc, char **argv)
{
   if (argc < 3) {
      fprintf(stderr, "usage: nakc IN.spv OUT.nak [--asm]\n"
                      "       nakc --air LIB.metallib FUNCTION OUT.nak [BX BY BZ] [tgI=BYTES...] [fcI=HEX...] [--asm]\n");
      return 2;
   }
   /* 28 Sep: --air compiles a precompiled Metal kernel (metallib / AIR
    * bitcode) straight to NIR; OUT.nak.abi gets the push layout and
    * OUT.nak.cdata the constant globals the runtime has to upload. */
   const bool air_mode = !strcmp(argv[1], "--air");
   if (air_mode && argc < 5) { fprintf(stderr, "--air needs LIB FUNCTION OUT\n"); return 2; }
   const char *out_path = air_mode ? argv[4] : argv[2];
   bool dump_asm = false;
   for (int i = 3; i < argc; ++i) if (!strcmp(argv[i], "--asm")) dump_asm = true;
   air_abi abi = {0};
   nir_shader *nir = NULL;
   mesa_shader_stage stage = MESA_SHADER_COMPUTE;
   glsl_type_singleton_init_or_ref();
   struct nak_compiler *nak = nak_compiler_create(&ad103);
   const nir_shader_compiler_options *nir_opts = nak_nir_options(nak);
   /* --tess-gen vs|tcs OUT LOCS CPS DOMAIN: the pipeline's generated stages */
   if (argc >= 5 && !strcmp(argv[1], "--visible")) {
      /* --visible LIB FN OUT: the argument list of a [[visible]] function */
      air_module vm = {0};
      air_function *vf = NULL;
      char err[256];
      if (air_load_function(argv[2], argv[3], &vm, &vf, err, sizeof err)) { fprintf(stderr, "air: %s\n", err); return 1; }
      static char text[16384];
      if (air_visible_reflection(&vm, vf, text, sizeof text)) { fprintf(stderr, "air: %s is not a visible function\n", argv[3]); return 1; }
      FILE *o = fopen(argv[4], "w");
      if (!o) { perror(argv[4]); return 2; }
      fputs(text, o);
      fclose(o);
      return 0;
   }
   if (!strcmp(argv[1], "--tess-gen")) {
      if (argc < 7) { fprintf(stderr, "--tess-gen vs|tcs OUT LOCS CPS DOMAIN\n"); return 2; }
      out_path = argv[3];
      int locs[32];
      unsigned nl = 0;
      for (char *p = argv[4]; *p && nl < 32; ) {
         locs[nl++] = (int)strtol(p, &p, 10);
         if (*p == ',') ++p; else if (*p) break;
      }
      if (!strcmp(argv[4], "-")) nl = 0;
      nir = tess_gen(argv[2], nir_opts, locs, nl, (unsigned)atoi(argv[5]), (unsigned)atoi(argv[6]));
      if (!nir) { fprintf(stderr, "tess-gen: bad arguments\n"); return 2; }
      stage = nir->info.stage;
      abi.stage = stage == MESA_SHADER_VERTEX ? 1 : 4;
      if (dump_asm) nir_print_shader(nir, stderr);
      goto common;
   }
   /* --mesh-gen OUT NVDATA MAXV MAXP VPP VSTRIDE GSTRIDE: the vertex shader that draws mesh shader output */
   if (!strcmp(argv[1], "--mesh-gen")) {
      if (argc < 9) { fprintf(stderr, "--mesh-gen OUT NVDATA MAXV MAXP VPP VSTRIDE GSTRIDE\n"); return 2; }
      out_path = argv[2];
      nir = mesh_vs(nir_opts, (unsigned)atoi(argv[3]), (unsigned)atoi(argv[4]), (unsigned)atoi(argv[5]),
                    (unsigned)atoi(argv[6]), (unsigned)atoi(argv[7]), (unsigned)atoi(argv[8]));
      if (!nir) { fprintf(stderr, "mesh-gen: bad arguments\n"); return 2; }
      stage = nir->info.stage;
      abi.stage = 1;
      if (dump_asm) nir_print_shader(nir, stderr);
      goto common;
   }
   if (air_mode) {
      air_variant var = {{64, 1, 1}, {0}};
      int pos = 0;
      for (int i = 5; i < argc; ++i) {
         unsigned idx, bytes;
         unsigned long long fv;
         if (sscanf(argv[i], "tg%u=%u", &idx, &bytes) == 2 && idx < 32) var.tg_arg_bytes[idx] = bytes;
         else if (!strncmp(argv[i], "io=", 3)) var.io = argv[i] + 3;
         else if (sscanf(argv[i], "tspace=%u", &idx) == 1) var.tess_spacing = idx;
         else if (sscanf(argv[i], "tccw=%u", &idx) == 1) var.tess_ccw = idx;
         else if (!strncmp(argv[i], "si", 2) && argv[i][2] >= '0' && argv[i][2] <= '9') {
            unsigned f, o, bi;
            if (sscanf(argv[i], "si%u=%u,%u,%u", &idx, &f, &o, &bi) == 4 && idx < 31 && bi < 31) {
               var.si_fmt[idx] = (uint16_t)f; var.si_off[idx] = o; var.si_buf[idx] = (uint8_t)bi;
            }
         }
         else if (!strncmp(argv[i], "sb", 2) && argv[i][2] >= '0' && argv[i][2] <= '9') {
            unsigned st, sf, sr;
            if (sscanf(argv[i], "sb%u=%u,%u,%u", &idx, &st, &sf, &sr) == 4 && idx < 31) {
               var.sb_stride[idx] = st; var.sb_step[idx] = (uint8_t)sf; var.sb_rate[idx] = sr;
            }
         }
         else if (!strncmp(argv[i], "link=", 5) && strchr(argv[i], '@') && var.nlink < 16) {
            /* linked function NAME@PATH (metallib or bare AIR): read further down */
            var.link_name[var.nlink++] = argv[i] + 5;
         }
         else if (!strncmp(argv[i], "sx=", 3)) {
            unsigned ty, bi;
            if (sscanf(argv[i], "sx=%u,%u", &ty, &bi) == 2 && bi < 31) {
               var.sx_given = 1; var.sx_type = (uint8_t)ty; var.sx_buf = (uint8_t)bi;
            }
         }
         else if (sscanf(argv[i], "fc%u=%llx", &idx, &fv) == 2 && idx < 128) {
            var.fc_defined[idx / 32] |= 1u << (idx % 32);
            var.fc_value[idx] = fv;
            /* vectors up to 16 bytes (uint4, float4, long2): fcN=lo,hi */
            const char *comma = strchr(argv[i], ',');
            var.fc_value_hi[idx] = comma ? strtoull(comma + 1, NULL, 16) : 0;
         }
         else if (argv[i][0] >= '0' && argv[i][0] <= '9' && pos < 3) var.block[pos++] = (uint32_t)atoi(argv[i]);
      }
      FILE *lf = fopen(argv[2], "rb");
      if (!lf) { perror(argv[2]); return 2; }
      fseek(lf, 0, SEEK_END);
      long lsz = ftell(lf);
      rewind(lf);
      uint8_t *lb = malloc(lsz);
      if (fread(lb, 1, lsz, lf) != (size_t)lsz) return 2;
      fclose(lf);
      char err[512];
      const uint8_t *bc = lb;
      size_t bclen = lsz;
      mtllib lib;
      if (!mtllib_parse(lb, lsz, &lib, err, sizeof err)) {
         bc = NULL;
         for (uint32_t i = 0; i < lib.nfunctions; ++i)
            if (!strcmp(lib.functions[i].name, argv[3])) { bc = lib.functions[i].bitcode; bclen = lib.functions[i].bitcode_len; }
         if (!bc) { fprintf(stderr, "%s: no function %s\n", argv[2], argv[3]); return 1; }
      }
      air_module am;
      if (air_read(bc, bclen, &am, err, sizeof err)) { fprintf(stderr, "air: %s\n", err); return 1; }
      for (uint32_t li = 0; li < var.nlink; ++li) {
         char *spec = strdup(var.link_name[li]), *at = strchr(spec, '@');
         *at = 0;
         var.link_name[li] = spec;
         var.link_mod[li] = calloc(1, sizeof(air_module));
         if (air_load_function(at + 1, spec, var.link_mod[li], &var.link_fn[li], err, sizeof err)) {
            fprintf(stderr, "air: linked %s: %s\n", spec, err);
            return 1;
         }
      }
      air_function *fn = NULL;
      for (uint32_t i = 0; i < am.nfunctions; ++i)
         if (!strcmp(am.functions[i].name, argv[3]) && !am.functions[i].is_proto) fn = &am.functions[i];
      if (!fn) { fprintf(stderr, "air: no body for %s\n", argv[3]); return 1; }
      nir = air_to_nir(&am, fn, nir_opts, &var, &abi, err, sizeof err);
      if (!nir) { fprintf(stderr, "air: %s: %s\n", argv[3], err); return 1; }
      stage = nir->info.stage;
      if (dump_asm) nir_print_shader(nir, stderr);
      goto common;
   }
   FILE *f = fopen(argv[1], "rb");
   if (!f) { perror(argv[1]); return 2; }
   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   rewind(f);
   uint32_t *spv = malloc(sz);
   if (fread(spv, 1, sz, f) != (size_t)sz) return 2;
   fclose(f);

   struct spirv_capabilities caps = {
      .Shader = true,
      .Int64 = true,
      .Int16 = true,
      .Int8 = true,
      .Float64 = true,
      .Float16 = true,
      .PhysicalStorageBufferAddresses = true,
      .StorageBuffer8BitAccess = true,
      .GroupNonUniform = true,
      .GroupNonUniformBallot = true,
      .GroupNonUniformShuffle = true,
      .GroupNonUniformArithmetic = true,
      .GroupNonUniformVote = true,
      .GroupNonUniformShuffleRelative = true,
      .GroupNonUniformClustered = true,
      .GroupNonUniformQuad = true,
      .ShaderClockKHR = true,
      .DemoteToHelperInvocation = true,
      .StorageBuffer16BitAccess = true,
      .VulkanMemoryModel = true,
      .VulkanMemoryModelDeviceScope = true,
      .Int64Atomics = true,
   };
   struct spirv_to_nir_options sopts = {
      .environment = NIR_SPIRV_VULKAN,
      .capabilities = &caps,
      .ssbo_addr_format = nir_address_format_64bit_global,
      .phys_ssbo_addr_format = nir_address_format_64bit_global,
      .ubo_addr_format = nir_address_format_64bit_global,
      .push_const_addr_format = nir_address_format_32bit_offset,
      .shared_addr_format = nir_address_format_32bit_offset,
      .global_addr_format = nir_address_format_64bit_global,
      .temp_addr_format = nir_address_format_32bit_offset,
      .constant_addr_format = nir_address_format_64bit_global,
      .min_ssbo_alignment = 16,
      .min_ubo_alignment = 16,
      .debug = { .func = spirv_debug },
   };
   stage = spirv_stage(spv, sz / 4);
   nir = spirv_to_nir(spv, sz / 4, NULL, 0, stage,
                      "main", &sopts, nir_opts);
   if (!nir) { fprintf(stderr, "spirv_to_nir failed\n"); return 1; }
common:

   /* vk_spirv_to_nir() clean-up */
   NIR_PASS(_, nir, nir_lower_variable_initializers, nir_var_function_temp);
   NIR_PASS(_, nir, nir_lower_returns);
   NIR_PASS(_, nir, nir_inline_functions);
   NIR_PASS(_, nir, nir_opt_copy_prop);
   NIR_PASS(_, nir, nir_opt_constant_folding);
   NIR_PASS(_, nir, nir_opt_deref);
   nir_remove_non_entrypoints(nir);
   NIR_PASS(_, nir, nir_lower_variable_initializers, ~0);
   NIR_PASS(_, nir, nir_split_var_copies);
   NIR_PASS(_, nir, nir_split_per_member_structs);
   if (stage != MESA_SHADER_COMPUTE) {
      nir_remove_dead_variables_options dv = {0};
      NIR_PASS(_, nir, nir_remove_dead_variables,
               nir_var_shader_in | nir_var_shader_out | nir_var_system_value, &dv);
      assign_io_locations(nir);
   }

   nak_preprocess_nir(nir, nak);

   /* nvk_lower_nir() subset */
   if (stage == MESA_SHADER_COMPUTE) {
      nir_lower_compute_system_values_options csv = {0};
      NIR_PASS(_, nir, nir_lower_compute_system_values, &csv);
   }
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_push_const,
            nir_address_format_32bit_offset);
   NIR_PASS(_, nir, nir_shader_intrinsics_pass, lower_push_const,
            nir_metadata_control_flow, NULL);
   NIR_PASS(_, nir, nir_shader_instructions_pass, lower_tex_handles,
            nir_metadata_control_flow, NULL);
   NIR_PASS(_, nir, nir_opt_dce);
   NIR_PASS(_, nir, nir_remove_dead_derefs);
   NIR_PASS(_, nir, nir_remove_dead_variables, nir_var_uniform | nir_var_image, NULL);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_global,
            nir_address_format_64bit_global);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_ssbo,
            nir_address_format_64bit_global);
   NIR_PASS(_, nir, nir_lower_vars_to_explicit_types, nir_var_mem_shared,
            shared_var_info);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_shared,
            nir_address_format_32bit_offset);

   hoist_divergent_derivatives(nir);
   fix_helper_loads(nir);

   struct nak_fs_key fs_key = {0};
   struct nak_shader_bin *bin = nak_compile_shader(nir, dump_asm, nak, 0,
      stage == MESA_SHADER_FRAGMENT ? &fs_key : NULL);
   if (!bin) { fprintf(stderr, "nak_compile_shader failed\n"); return 1; }
   if (dump_asm && bin->asm_str) fputs(bin->asm_str, stdout);

   /* word 9 = stage (0 VS, 4 FS, 5 CS); word 10 = SPH dwords that follow
    * the 16-word header (Turing+ 0x80-byte shader program header).
    * word 11 = fragment uses per-sample shading; old outputs leave it zero. */
   const uint32_t sph = stage == MESA_SHADER_COMPUTE ? 0 : 32;
   uint32_t hdr[16] = {
      0x314b414e, bin->code_size, bin->info.num_gprs, bin->info.slm_size,
      nir->info.shared_size,
      nir->info.workgroup_size[0], nir->info.workgroup_size[1],
      nir->info.workgroup_size[2],
      bin->info.num_control_barriers,
      stage == MESA_SHADER_VERTEX ? 0 : stage == MESA_SHADER_FRAGMENT ? 4 : stage == MESA_SHADER_TESS_CTRL ? 1
         : stage == MESA_SHADER_TESS_EVAL ? 2 : 5,
      sph,
      stage == MESA_SHADER_FRAGMENT && bin->info.fs.uses_sample_shading,
   };
   FILE *o = fopen(out_path, "wb");
   if (!o) { perror(out_path); return 2; }
   fwrite(hdr, 4, 16, o);
   if (sph) fwrite(bin->info.hdr, 4, sph, o);
   fwrite(bin->code, 1, bin->code_size, o);
   fclose(o);
   if (stage == MESA_SHADER_TESS_CTRL || stage == MESA_SHADER_TESS_EVAL) {
      char side[1100];
      snprintf(side, sizeof side, "%s.ts", out_path);   /* SET_TESSELLATION_PARAMETERS fields */
      FILE *t = fopen(side, "w");
      if (t) {
         fprintf(t, "domain %u spacing %u prims %u domain_md %u cps %u\n", bin->info.ts.domain, bin->info.ts.spacing,
                 bin->info.ts.prims, abi.tess_domain, abi.tess_cps);
         fclose(t);
      }
   }
   if (air_mode) {
      char side[1100];
      snprintf(side, sizeof side, "%s.abi", out_path);
      FILE *a = fopen(side, "w");
      if (a) {
         fprintf(a, "ntex %u\nnsamp %u\n", abi.ntex, abi.nsamp);
         fprintf(a, "stage %u\n", stage == MESA_SHADER_VERTEX ? 1 : stage == MESA_SHADER_FRAGMENT ? 2
                                 : stage == MESA_SHADER_TESS_EVAL ? 3 : 0);
         if (stage == MESA_SHADER_TESS_EVAL) fprintf(a, "tess %u %u\n", abi.tess_domain, abi.tess_cps);
         if (abi.io[0]) fprintf(a, "io %s\n", abi.io);
         if (abi.mesh_kind)
            fprintf(a, "mesh %u %u %u %u %u %u %u %u %u\n", abi.mesh_kind, abi.mesh_slot, abi.mesh_payload_stride,
                    abi.mesh_max_v, abi.mesh_max_p, abi.mesh_nvdata, abi.mesh_vstride, abi.mesh_group_stride, abi.mesh_vpp);
         fprintf(a, "nbuf %u\ngrid %u\ncdata %u %u\npush %u\ntg %u\n", abi.nbuf, abi.grid_offset,
                 abi.cdata_offset, abi.cdata_bytes, abi.push_bytes, abi.tg_bytes);
         for (unsigned i = 0; i < 32; ++i)
            if (abi.tg_arg_offset[i]) fprintf(a, "tgarg %u %u\n", i, abi.tg_arg_offset[i]);
         for (unsigned i = 0; i < abi.nconst_samp; ++i)
            fprintf(a, "csamp %u %llx\n", abi.nsamp + i, (unsigned long long)abi.const_samp[i]);
         if (abi.rt_read_mask) fprintf(a, "rtread %u %x\n", abi.rt_read_offset, abi.rt_read_mask);
         if (abi.refl) fputs(abi.refl, a);
         fclose(a);
      }
      if (abi.cdata_bytes) {
         snprintf(side, sizeof side, "%s.cdata", out_path);
         FILE *d = fopen(side, "wb");
         if (d) { fwrite(abi.cdata, 1, abi.cdata_bytes, d); fclose(d); }
      }
   }
   fprintf(stderr, "%s: %u bytes, %u GPRs, slm %u, smem %u, local %ux%ux%u\n",
           out_path, bin->code_size, bin->info.num_gprs, bin->info.slm_size,
           nir->info.shared_size, nir->info.workgroup_size[0],
           nir->info.workgroup_size[1], nir->info.workgroup_size[2]);
   nak_shader_bin_destroy(bin);
   ralloc_free(nir);
   nak_compiler_destroy(nak);
   glsl_type_singleton_decref();
   return 0;
}
