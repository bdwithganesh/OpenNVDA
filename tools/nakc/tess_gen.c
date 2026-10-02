/* Shaders the Metal tessellation pipeline needs besides the app's
 * post-tessellation vertex function (which nakc --air builds as the
 * tessellation evaluation stage):
 *   vs   control points in: vertex attribute L -> VARYING_SLOT_VAR0 + L
 *   tcs  passes each control point through and loads the patch's
 *        tessellation factors from the factor buffer Metal apps fill
 *        (MTLTriangleTessellationFactorsHalf / MTLQuadTessellationFactorsHalf)
 * Push block of the tcs: u64 factor buffer address (patchStart applied),
 * f32 max tessellation factor. Attributes travel as raw 32-bit vec4. */
#include "tess_gen.h"
#include "compiler/nir/nir_builder.h"

static nir_variable *var(nir_shader *s, nir_variable_mode m, const struct glsl_type *t, int loc, const char *n) {
   nir_variable *v = nir_variable_create(s, m, t, n);
   v->data.location = loc;
   return v;
}

nir_shader *tess_gen(const char *kind, const nir_shader_compiler_options *opts, const int *locs, unsigned nlocs,
                     unsigned cps, unsigned domain)
{
   const struct glsl_type *v4 = glsl_vec4_type();
   if (!strcmp(kind, "vs")) {
      nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, opts, "tess_vs");
      for (unsigned i = 0; i < nlocs; ++i) {
         nir_variable *in = var(b.shader, nir_var_shader_in, v4, VERT_ATTRIB_GENERIC0 + locs[i], "cp_in");
         nir_variable *out = var(b.shader, nir_var_shader_out, v4, VARYING_SLOT_VAR0 + locs[i], "cp_out");
         nir_store_var(&b, out, nir_load_var(&b, in), 0xf);
      }
      return b.shader;
   }
   if (strcmp(kind, "tcs") || !cps || cps > 32) return NULL;
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_TESS_CTRL, opts, "tess_tcs");
   b.shader->info.tess.tcs_vertices_out = cps;
   b.shader->info.tess._primitive_mode = domain == 2 ? TESS_PRIMITIVE_QUADS : TESS_PRIMITIVE_TRIANGLES;
   nir_def *iid = nir_load_invocation_id(&b);
   for (unsigned i = 0; i < nlocs; ++i) {
      nir_variable *in = var(b.shader, nir_var_shader_in, glsl_array_type(v4, 32, 0), VARYING_SLOT_VAR0 + locs[i], "cp_in");
      nir_variable *out = var(b.shader, nir_var_shader_out, glsl_array_type(v4, cps, 0), VARYING_SLOT_VAR0 + locs[i], "cp_out");
      nir_def *v = nir_load_deref(&b, nir_build_deref_array(&b, nir_build_deref_var(&b, in), iid));
      nir_store_deref(&b, nir_build_deref_array(&b, nir_build_deref_var(&b, out), iid), v, 0xf);
   }
   const bool quad = domain == 2;
   nir_variable *outer = var(b.shader, nir_var_shader_out, glsl_array_type(glsl_float_type(), 4, 0),
                             VARYING_SLOT_TESS_LEVEL_OUTER, "outer");
   nir_variable *inner = var(b.shader, nir_var_shader_out, glsl_array_type(glsl_float_type(), 2, 0),
                             VARYING_SLOT_TESS_LEVEL_INNER, "inner");
   outer->data.patch = inner->data.patch = true;
   outer->data.compact = inner->data.compact = true;
   nir_def *base = nir_load_push_constant(&b, 1, 64, nir_imm_int(&b, 0), .base = 0, .range = 16);
   nir_def *maxf = nir_load_push_constant(&b, 1, 32, nir_imm_int(&b, 8), .base = 0, .range = 16);
   const unsigned n = quad ? 6 : 4;          /* halfs: edges then inside */
   nir_def *addr = nir_iadd(&b, base, nir_u2u64(&b, nir_imul_imm(&b, nir_load_primitive_id(&b), n * 2)));
   nir_def *f[6];
   for (unsigned k = 0; k < n; ++k) {
      nir_def *h = nir_load_global(&b, 1, 16, nir_iadd_imm(&b, addr, k * 2), .align_mul = 2);
      f[k] = nir_fmin(&b, nir_f2f32(&b, h), maxf);
   }
   const unsigned edges = quad ? 4 : 3;
   for (unsigned k = 0; k < edges; ++k)
      nir_store_deref(&b, nir_build_deref_array_imm(&b, nir_build_deref_var(&b, outer), k), f[k], 1);
   for (unsigned k = 0; k < (quad ? 2u : 1u); ++k)
      nir_store_deref(&b, nir_build_deref_array_imm(&b, nir_build_deref_var(&b, inner), k), f[edges + k], 1);
   return b.shader;
}

/* Mesh shaders (Metal 3) run as compute (air_nir.c mesh_sysvals); this vertex shader draws their output.
 * Push: u64 output base (buffer slot 0). Vertex v of the draw is corner v % vpp of primitive (v / vpp) % max_p of
 * mesh group v / (max_p * vpp). Primitives past the group's count, and groups that never ran (count 0, the
 * runtime clears the block), collapse to one point and draw nothing. Varyings go out as raw 32-bit vec4. */
nir_shader *mesh_vs(const nir_shader_compiler_options *opts, unsigned nvdata, unsigned max_v, unsigned max_p,
                    unsigned vpp, unsigned vstride, unsigned gstride)
{
   if (!max_v || !max_p || !vpp || nvdata > 30) return NULL;
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, opts, "mesh_vs");
   const struct glsl_type *v4 = glsl_vec4_type();
   nir_def *base = nir_load_push_constant(&b, 1, 64, nir_imm_int(&b, 0), .base = 0, .range = 16);
   nir_def *vid = nir_load_vertex_id(&b);
   const unsigned per = max_p * vpp;
   nir_def *g = nir_udiv_imm(&b, vid, per);
   nir_def *k = nir_umod_imm(&b, vid, per);
   nir_def *gb = nir_iadd(&b, base, nir_u2u64(&b, nir_imul_imm(&b, g, gstride)));
   nir_def *cnt = nir_load_global(&b, 1, 32, gb, .align_mul = 16);
   nir_def *live = nir_ult(&b, nir_udiv_imm(&b, k, vpp), nir_umin(&b, cnt, nir_imm_int(&b, (int)max_p)));
   nir_def *ix = nir_load_global(&b, 1, 32, nir_iadd(&b, gb, nir_u2u64(&b, nir_iadd_imm(&b, nir_imul_imm(&b, k, 4),
                                 16 + max_v * vstride))), .align_mul = 4);
   ix = nir_bcsel(&b, live, nir_umin(&b, ix, nir_imm_int(&b, (int)max_v - 1)), nir_imm_int(&b, 0));
   nir_def *va = nir_iadd(&b, gb, nir_u2u64(&b, nir_iadd_imm(&b, nir_imul_imm(&b, ix, vstride), 16)));
   nir_def *pos = nir_load_global(&b, 4, 32, va, .align_mul = 16);
   pos = nir_bcsel(&b, live, pos, nir_imm_vec4(&b, 0, 0, 0, 1));
   nir_store_var(&b, var(b.shader, nir_var_shader_out, v4, VARYING_SLOT_POS, "pos"), pos, 0xf);
   for (unsigned i = 0; i < nvdata; ++i) {
      nir_def *d = nir_load_global(&b, 4, 32, nir_iadd_imm(&b, va, 16 * (1 + i)), .align_mul = 16);
      nir_store_var(&b, var(b.shader, nir_var_shader_out, v4, VARYING_SLOT_VAR0 + i, "mv"), d, 0xf);
   }
   return b.shader;
}
