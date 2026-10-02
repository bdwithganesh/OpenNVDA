/* Dump the 3D-class state init words NVMTLGsp puts in front of every draw
 * (a subset of NVK's nvk_push_draw_state_init, encoded by Mesa's own
 * generated class headers). */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "nv_push.h"
#include "nv_push_cl9097.h"
#include "nv_push_cla097.h"
#include "nv_push_clb197.h"
#include "nv_push_clc397.h"
#include "nv_push_clc597.h"
#include "nv_push_clc997.h"
int main(void) {
    uint32_t buf[512];
    struct nv_push push, *p = &push;
    nv_push_init(p, buf, 512, 1 << 0);
    P_IMMD(p, NV9097, SET_RENDER_ENABLE_C, MODE_TRUE);
    P_IMMD(p, NV9097, SET_RENDER_ENABLE_OVERRIDE, MODE_ALWAYS_RENDER);
    P_IMMD(p, NV9097, SET_RENDER_ENABLE_CONTROL, CONDITIONAL_LOAD_CONSTANT_BUFFER_FALSE);
    P_IMMD(p, NV9097, SET_Z_COMPRESSION, ENABLE_FALSE);
    P_MTHD(p, NV9097, SET_COLOR_COMPRESSION(0));
    for (unsigned i = 0; i < 8; i++) P_NV9097_SET_COLOR_COMPRESSION(p, i, ENABLE_FALSE);
    P_IMMD(p, NV9097, SET_ALIASED_LINE_WIDTH_ENABLE, V_TRUE);
    P_IMMD(p, NV9097, SET_DA_PRIMITIVE_RESTART_VERTEX_ARRAY, ENABLE_FALSE);
    P_IMMD(p, NV9097, SET_BLEND_SEPARATE_FOR_ALPHA, ENABLE_TRUE);
    P_IMMD(p, NV9097, SET_SINGLE_CT_WRITE_CONTROL, ENABLE_TRUE);
    P_IMMD(p, NV9097, SET_SINGLE_ROP_CONTROL, ENABLE_FALSE);
    P_IMMD(p, NV9097, SET_TWO_SIDED_STENCIL_TEST, ENABLE_TRUE);
    P_IMMD(p, NV9097, SET_SHADE_MODE, V_OGL_SMOOTH);
    P_IMMD(p, NV9097, SET_API_VISIBLE_CALL_LIMIT, V__128);
    P_IMMD(p, NV9097, SET_L1_CONFIGURATION, DIRECTLY_ADDRESSABLE_MEMORY_SIZE_48KB);
    P_IMMD(p, NV9097, CHECK_SPH_VERSION, { .current = 3, .oldest_supported = 3 });
    P_IMMD(p, NV9097, CHECK_AAM_VERSION, { .current = 2, .oldest_supported = 2 });
    P_IMMD(p, NV9097, SET_BLEND_PER_FORMAT_ENABLE, SNORM8_UNORM16_SNORM16_TRUE);
    P_IMMD(p, NV9097, SET_ATTRIBUTE_DEFAULT, {
        .color_front_diffuse = COLOR_FRONT_DIFFUSE_VECTOR_0001,
        .color_front_specular = COLOR_FRONT_SPECULAR_VECTOR_0001,
        .generic_vector = GENERIC_VECTOR_VECTOR_0001,
        .fixed_fnc_texture = FIXED_FNC_TEXTURE_VECTOR_0001,
        .dx9_color0 = DX9_COLOR0_VECTOR_0001,
        .dx9_color1_to_color15 = DX9_COLOR1_TO_COLOR15_VECTOR_0000,
    });
    P_IMMD(p, NV9097, SET_DA_OUTPUT, VERTEX_ID_USES_ARRAY_START_TRUE);
    P_IMMD(p, NV9097, SET_PS_OUTPUT_SAMPLE_MASK_USAGE, {
        .enable = ENABLE_TRUE, .qualify_by_anti_alias_enable = QUALIFY_BY_ANTI_ALIAS_ENABLE_ENABLE });
    P_IMMD(p, NV9097, SET_BLEND_OPT_CONTROL, ALLOW_FLOAT_PIXEL_KILLS_TRUE);
    P_IMMD(p, NV9097, SET_BLEND_FLOAT_OPTION, ZERO_TIMES_ANYTHING_IS_ZERO_TRUE);
    P_IMMD(p, NV9097, SET_BLEND_STATE_PER_TARGET, ENABLE_TRUE);
    P_IMMD(p, NV9097, SET_ALPHA_TEST, ENABLE_FALSE);
    P_IMMD(p, NV9097, SET_TWO_SIDED_LIGHT, ENABLE_FALSE);
    P_IMMD(p, NV9097, SET_COLOR_CLAMP, ENABLE_TRUE);
    P_IMMD(p, NV9097, SET_PS_SATURATE, { .output0 = OUTPUT0_FALSE });
    P_IMMD(p, NV9097, SET_POINT_SIZE, fui(1.0));
    P_IMMD(p, NV9097, SET_ATTRIBUTE_POINT_SIZE, { .enable = ENABLE_TRUE });
    P_IMMD(p, NV9097, SET_ANTI_ALIAS, SAMPLES_MODE_1X1);
    P_IMMD(p, NV9097, SET_ANTI_ALIAS_ENABLE, V_FALSE);
        P_IMMD(p, NV9097, SET_DEPTH_TEST, ENABLE_FALSE);
    P_IMMD(p, NV9097, SET_DEPTH_WRITE, ENABLE_FALSE);
    P_IMMD(p, NV9097, SET_STENCIL_TEST, ENABLE_FALSE);
    P_IMMD(p, NV9097, SET_DEPTH_BOUNDS_TEST, ENABLE_FALSE);
    P_IMMD(p, NV9097, SET_FRONT_POLYGON_MODE, V_FILL);
    P_IMMD(p, NV9097, SET_BACK_POLYGON_MODE, V_FILL);
    P_IMMD(p, NV9097, SET_POLY_OFFSET_FILL, ENABLE_FALSE);
    P_IMMD(p, NV9097, SET_RASTER_ENABLE, V_TRUE);
    P_IMMD(p, NV9097, SET_LOGIC_OP, ENABLE_FALSE);
    P_IMMD(p, NV9097, SET_CLEAR_SURFACE_CONTROL, {
        .respect_stencil_mask = RESPECT_STENCIL_MASK_FALSE, .use_clear_rect = USE_CLEAR_RECT_TRUE,
        .use_scissor0 = USE_SCISSOR0_FALSE, .use_viewport_clip0 = USE_VIEWPORT_CLIP0_FALSE });
    P_IMMD(p, NV9097, SET_VIEWPORT_CLIP_CONTROL, {
        .min_z_zero_max_z_one = MIN_Z_ZERO_MAX_Z_ONE_FALSE,
        .pixel_min_z = PIXEL_MIN_Z_CLAMP, .pixel_max_z = PIXEL_MAX_Z_CLAMP,
        .geometry_guardband = GEOMETRY_GUARDBAND_SCALE_256,
        .line_point_cull_guardband = LINE_POINT_CULL_GUARDBAND_SCALE_256,
        .geometry_clip = GEOMETRY_CLIP_WZERO_CLIP,
        .geometry_guardband_z = GEOMETRY_GUARDBAND_Z_SAME_AS_XY_GUARDBAND });
    P_IMMD(p, NV9097, SET_PROVOKING_VERTEX, V_FIRST);
    P_IMMD(p, NV9097, SET_SAMPLE_MASK_X0_Y0, 0xffff);
    P_IMMD(p, NV9097, SET_SAMPLE_MASK_X1_Y0, 0xffff);
    P_IMMD(p, NV9097, SET_SAMPLE_MASK_X0_Y1, 0xffff);
    P_IMMD(p, NV9097, SET_SAMPLE_MASK_X1_Y1, 0xffff);
    P_IMMD(p, NV9097, SET_ACTIVE_ZCULL_REGION, 0x3f);
    P_IMMD(p, NV9097, SET_ZCULL, { .z_enable = Z_ENABLE_FALSE, .stencil_enable = STENCIL_ENABLE_FALSE });
    P_IMMD(p, NV9097, SET_ZCULL_STATS, ENABLE_FALSE);
    P_IMMD(p, NV9097, SET_CLIP_ID_TEST, ENABLE_FALSE);
    P_IMMD(p, NV9097, SET_VIEWPORT_SCALE_OFFSET, ENABLE_TRUE);
    P_IMMD(p, NV9097, SET_VIEWPORT_PIXEL, CENTER_AT_HALF_INTEGERS);
    P_MTHD(p, NV9097, SET_WINDOW_OFFSET_X);
    P_NV9097_SET_WINDOW_OFFSET_X(p, 0);
    P_NV9097_SET_WINDOW_OFFSET_Y(p, 0);
    P_IMMD(p, NV9097, SET_POLY_SMOOTH, ENABLE_FALSE);
    P_IMMD(p, NV9097, SET_Z_COMPRESSION, ENABLE_FALSE);
    const unsigned n = (unsigned)(p->end - buf);
    printf("// %u words, generated with Mesa's nv_push_cl9097.h (gen3d.c)\n", n);
    for (unsigned i = 0; i < n; i++) printf("0x%08x,%s", buf[i], (i % 6 == 5) ? "\n" : " ");
    printf("\n");
    return 0;
}
