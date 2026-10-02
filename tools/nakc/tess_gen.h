#ifndef TESS_GEN_H
#define TESS_GEN_H
#include "compiler/nir/nir.h"
/* kind "vs" or "tcs"; locs: control point attribute locations; domain 1 triangle, 2 quad */
nir_shader *tess_gen(const char *kind, const nir_shader_compiler_options *opts, const int *locs, unsigned nlocs,
                     unsigned cps, unsigned domain);
nir_shader *mesh_vs(const nir_shader_compiler_options *opts, unsigned nvdata, unsigned max_v, unsigned max_p,
                    unsigned vpp, unsigned vstride, unsigned gstride);
#endif
