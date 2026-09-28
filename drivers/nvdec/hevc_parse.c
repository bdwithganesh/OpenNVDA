/*
 * HEVC syntax parser (see hevc_parse.h). Written from ITU-T H.265 (syntax), 7.4
 * (semantics, including the 7.4.8 RPS derivation), Tables 7-5/7-6.
 */
#include "hevc_parse.h"

#include <string.h>

#define HEVC_RBSP_CAP 8192

static size_t load_rbsp(const uint8_t *nal, size_t size, uint8_t *rbsp) {
    return size > 2 ? h264_unescape(nal + 2, size - 2, rbsp, HEVC_RBSP_CAP) : 0;
}

static int ceil_log2(int v) {
    int n = 0;
    while ((1 << n) < v) ++n;
    return n;
}

/* ---------------------------------------------------------- scaling lists */

static const uint8_t kIntra8[64] = {
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 17, 16, 17, 16, 17, 18, 17, 18, 18, 17, 18, 21,
    19, 20, 21, 20, 19, 21, 24, 22, 22, 24, 24, 22, 22, 24, 25, 25, 27, 30, 27, 25, 25, 29,
    31, 35, 35, 31, 29, 36, 41, 44, 41, 36, 47, 54, 54, 47, 65, 70, 65, 88, 88, 115};
static const uint8_t kInter8[64] = {
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 17, 17, 17, 17, 17, 18, 18, 18, 18, 18, 18, 20,
    20, 20, 20, 20, 20, 20, 24, 24, 24, 24, 24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 28,
    28, 28, 28, 28, 28, 33, 33, 33, 33, 33, 41, 41, 41, 41, 54, 54, 54, 71, 71, 91};

/* 6.5.3 up-right diagonal scan: scan position -> raster index */
static void diag_scan(int n, uint8_t *pos) {
    int i = 0, x = 0, y = 0;
    while (i < n * n) {
        while (y >= 0) {
            if (x < n && y < n) pos[i++] = (uint8_t)(y * n + x);
            --y;
            ++x;
        }
        y = x;
        x = 0;
    }
}

static void scaling_default(hevc_scaling *s) {
    uint8_t d8[64];
    diag_scan(8, d8);
    memset(s->list4x4, 16, sizeof(s->list4x4));
    for (int m = 0; m < 6; ++m) {
        const uint8_t *def = m < 3 ? kIntra8 : kInter8;
        for (int i = 0; i < 64; ++i) {
            s->list8x8[m][d8[i]] = def[i];
            s->list16x16[m][d8[i]] = def[i];
            s->list32x32[m][d8[i]] = def[i];
        }
        s->dc16x16[m] = s->dc32x32[m] = 16;
    }
}

/* 7.3.4 scaling_list_data() */
static int parse_scaling(h264_bits *b, hevc_scaling *s) {
    uint8_t d4[16], d8[64];
    diag_scan(4, d4);
    diag_scan(8, d8);
    scaling_default(s);
    for (int size = 0; size < 4; ++size) {
        for (int m = 0; m < 6; m += size == 3 ? 3 : 1) {
            uint8_t *list = size == 0 ? s->list4x4[m] : size == 1 ? s->list8x8[m]
                          : size == 2 ? s->list16x16[m] : s->list32x32[m];
            uint8_t *dc = size == 2 ? &s->dc16x16[m] : size == 3 ? &s->dc32x32[m] : NULL;
            const int n = size == 0 ? 16 : 64;
            if (!h264_u(b, 1)) {   /* scaling_list_pred_mode_flag == 0 */
                const int delta = (int)h264_ue(b) * (size == 3 ? 3 : 1);
                if (delta > m) return -1;
                if (delta) {
                    const int r = m - delta;
                    const uint8_t *src = size == 0 ? s->list4x4[r] : size == 1 ? s->list8x8[r]
                                       : size == 2 ? s->list16x16[r] : s->list32x32[r];
                    memcpy(list, src, (size_t)n);
                    if (dc) *dc = size == 2 ? s->dc16x16[r] : s->dc32x32[r];
                }   /* delta 0: default list, already there */
            } else {
                int next = 8;
                if (size > 1) {
                    const int v = h264_se(b) + 8;
                    if (v < 1 || v > 255) return -1;
                    next = v;
                    *dc = (uint8_t)v;
                }
                for (int i = 0; i < n; ++i) {
                    next = (next + h264_se(b) + 256) % 256;
                    list[size == 0 ? d4[i] : d8[i]] = (uint8_t)next;
                }
            }
        }
    }
    /* 4:2:0: 32x32 chroma lists (1,2,4,5) follow 16x16 ones (unused by NVDEC here) */
    for (int m = 1; m < 6; ++m)
        if (m != 3) {
            memcpy(s->list32x32[m], s->list16x16[m], 64);
            s->dc32x32[m] = s->dc16x16[m];
        }
    return b->err ? -1 : 0;
}

/* ------------------------------------------------------------------- RPS */

/* 7.3.7 st_ref_pic_set(idx) + 7.4.8 derivation; `sets` holds 0..idx-1 */
static int parse_st_rps(h264_bits *b, int idx, int num_sets, const hevc_st_rps *sets,
                        hevc_st_rps *out) {
    memset(out, 0, sizeof(*out));
    if (idx != 0 && h264_u(b, 1)) {   /* inter_ref_pic_set_prediction_flag */
        int delta_idx = 1;
        if (idx == num_sets) delta_idx = (int)h264_ue(b) + 1;
        if (delta_idx > idx) return -1;
        const hevc_st_rps *r = &sets[idx - delta_idx];
        const int sign = (int)h264_u(b, 1);
        const int abs_delta = (int)h264_ue(b) + 1;
        const int delta_rps = (1 - 2 * sign) * abs_delta;
        const int nd = r->num_negative + r->num_positive;
        int used[33], use_delta[33];
        for (int j = 0; j <= nd; ++j) {
            used[j] = (int)h264_u(b, 1);
            use_delta[j] = used[j] ? 1 : (int)h264_u(b, 1);
        }
        int i = 0;
        for (int j = r->num_positive - 1; j >= 0; --j) {
            const int d = r->delta_poc_s1[j] + delta_rps;
            if (d < 0 && use_delta[r->num_negative + j] && i < 16) {
                out->delta_poc_s0[i] = d;
                out->used_s0[i++] = used[r->num_negative + j];
            }
        }
        if (delta_rps < 0 && use_delta[nd] && i < 16) {
            out->delta_poc_s0[i] = delta_rps;
            out->used_s0[i++] = used[nd];
        }
        for (int j = 0; j < r->num_negative; ++j) {
            const int d = r->delta_poc_s0[j] + delta_rps;
            if (d < 0 && use_delta[j] && i < 16) {
                out->delta_poc_s0[i] = d;
                out->used_s0[i++] = used[j];
            }
        }
        out->num_negative = i;
        i = 0;
        for (int j = r->num_negative - 1; j >= 0; --j) {
            const int d = r->delta_poc_s0[j] + delta_rps;
            if (d > 0 && use_delta[j] && i < 16) {
                out->delta_poc_s1[i] = d;
                out->used_s1[i++] = used[j];
            }
        }
        if (delta_rps > 0 && use_delta[nd] && i < 16) {
            out->delta_poc_s1[i] = delta_rps;
            out->used_s1[i++] = used[nd];
        }
        for (int j = 0; j < r->num_positive; ++j) {
            const int d = r->delta_poc_s1[j] + delta_rps;
            if (d > 0 && use_delta[r->num_negative + j] && i < 16) {
                out->delta_poc_s1[i] = d;
                out->used_s1[i++] = used[r->num_negative + j];
            }
        }
        out->num_positive = i;
    } else {
        out->num_negative = (int)h264_ue(b);
        out->num_positive = (int)h264_ue(b);
        if (out->num_negative > 16 || out->num_positive > 16 ||
            out->num_negative + out->num_positive > 16)
            return -1;
        int poc = 0;
        for (int i = 0; i < out->num_negative; ++i) {
            poc -= (int)h264_ue(b) + 1;
            out->delta_poc_s0[i] = poc;
            out->used_s0[i] = (int)h264_u(b, 1);
        }
        poc = 0;
        for (int i = 0; i < out->num_positive; ++i) {
            poc += (int)h264_ue(b) + 1;
            out->delta_poc_s1[i] = poc;
            out->used_s1[i] = (int)h264_u(b, 1);
        }
    }
    return b->err ? -1 : 0;
}

/* -------------------------------------------------------------------- SPS */

static void skip_ptl(h264_bits *b, int max_sub_layers_minus1) {
    h264_u(b, 32); h264_u(b, 32); h264_u(b, 24);   /* general profile: 88 bits */
    h264_u(b, 8);                                  /* general_level_idc */
    int prof[8] = {0}, lev[8] = {0};
    for (int i = 0; i < max_sub_layers_minus1; ++i) {
        prof[i] = (int)h264_u(b, 1);
        lev[i] = (int)h264_u(b, 1);
    }
    if (max_sub_layers_minus1 > 0)
        for (int i = max_sub_layers_minus1; i < 8; ++i) h264_u(b, 2);
    for (int i = 0; i < max_sub_layers_minus1; ++i) {
        if (prof[i]) { h264_u(b, 32); h264_u(b, 32); h264_u(b, 24); }
        if (lev[i]) h264_u(b, 8);
    }
}

int hevc_parse_sps(const uint8_t *nal, size_t size, hevc_sps *out) {
    static uint8_t rbsp[HEVC_RBSP_CAP];
    static hevc_sps s;
    h264_bits b;
    h264_bits_init(&b, rbsp, load_rbsp(nal, size, rbsp));
    memset(&s, 0, sizeof(s));
    h264_u(&b, 4);                                  /* sps_video_parameter_set_id */
    const int max_sub_layers_minus1 = (int)h264_u(&b, 3);
    h264_u(&b, 1);
    if (max_sub_layers_minus1 > 6) return -1;
    skip_ptl(&b, max_sub_layers_minus1);
    s.id = (int)h264_ue(&b);
    if (s.id >= HEVC_MAX_SPS) return -1;
    s.chroma_format_idc = (int)h264_ue(&b);
    if (s.chroma_format_idc > 3) return -1;
    if (s.chroma_format_idc == 3) s.separate_colour_plane_flag = (int)h264_u(&b, 1);
    s.width = (int)h264_ue(&b);
    s.height = (int)h264_ue(&b);
    if (h264_u(&b, 1)) {
        const int ux = s.chroma_format_idc == 1 || s.chroma_format_idc == 2 ? 2 : 1;
        const int uy = s.chroma_format_idc == 1 ? 2 : 1;
        s.conf_left = (int)h264_ue(&b) * ux;
        s.conf_right = (int)h264_ue(&b) * ux;
        s.conf_top = (int)h264_ue(&b) * uy;
        s.conf_bottom = (int)h264_ue(&b) * uy;
    }
    s.bit_depth_luma = (int)h264_ue(&b) + 8;
    s.bit_depth_chroma = (int)h264_ue(&b) + 8;
    s.log2_max_poc_lsb = (int)h264_ue(&b) + 4;
    if (s.log2_max_poc_lsb > 16) return -1;
    const int ordering_all = (int)h264_u(&b, 1);
    for (int i = ordering_all ? 0 : max_sub_layers_minus1; i <= max_sub_layers_minus1; ++i) {
        s.max_dec_pic_buffering = (int)h264_ue(&b) + 1;
        s.max_num_reorder = (int)h264_ue(&b);
        s.max_latency_increase = (int)h264_ue(&b);
    }
    s.log2_min_cb = (int)h264_ue(&b) + 3;
    s.log2_max_cb = s.log2_min_cb + (int)h264_ue(&b);
    s.log2_min_tb = (int)h264_ue(&b) + 2;
    s.log2_max_tb = s.log2_min_tb + (int)h264_ue(&b);
    s.max_th_depth_inter = (int)h264_ue(&b);
    s.max_th_depth_intra = (int)h264_ue(&b);
    s.scaling_list_enabled_flag = (int)h264_u(&b, 1);
    scaling_default(&s.scaling);
    if (s.scaling_list_enabled_flag && h264_u(&b, 1) && parse_scaling(&b, &s.scaling)) return -1;
    s.amp_enabled_flag = (int)h264_u(&b, 1);
    s.sao_enabled_flag = (int)h264_u(&b, 1);
    s.pcm_enabled_flag = (int)h264_u(&b, 1);
    if (s.pcm_enabled_flag) {
        s.pcm_bit_depth_luma = (int)h264_u(&b, 4) + 1;
        s.pcm_bit_depth_chroma = (int)h264_u(&b, 4) + 1;
        s.log2_min_pcm_cb = (int)h264_ue(&b) + 3;
        s.log2_max_pcm_cb = s.log2_min_pcm_cb + (int)h264_ue(&b);
        s.pcm_loop_filter_disabled_flag = (int)h264_u(&b, 1);
    }
    s.num_short_term_ref_pic_sets = (int)h264_ue(&b);
    if (s.num_short_term_ref_pic_sets > 64) return -1;
    for (int i = 0; i < s.num_short_term_ref_pic_sets; ++i)
        if (parse_st_rps(&b, i, s.num_short_term_ref_pic_sets, s.st_rps, &s.st_rps[i])) return -1;
    s.long_term_ref_pics_present_flag = (int)h264_u(&b, 1);
    if (s.long_term_ref_pics_present_flag) {
        s.num_long_term_ref_pics_sps = (int)h264_ue(&b);
        if (s.num_long_term_ref_pics_sps > 32) return -1;
        for (int i = 0; i < s.num_long_term_ref_pics_sps; ++i) {
            s.lt_ref_pic_poc_lsb_sps[i] = (int)h264_u(&b, s.log2_max_poc_lsb);
            s.used_by_curr_pic_lt_sps[i] = (int)h264_u(&b, 1);
        }
    }
    s.temporal_mvp_enabled_flag = (int)h264_u(&b, 1);
    s.strong_intra_smoothing_enabled_flag = (int)h264_u(&b, 1);
    /* VUI and extensions: not needed by the driver */
    if (b.err || s.width <= 0 || s.height <= 0 || s.width > 8192 || s.height > 8192 ||
        s.log2_max_cb > 6 || s.log2_min_cb > s.log2_max_cb || s.max_dec_pic_buffering > 16)
        return -1;
    const int ctb = 1 << s.log2_max_cb;
    s.ctb_width = (s.width + ctb - 1) / ctb;
    s.ctb_height = (s.height + ctb - 1) / ctb;
    s.valid = 1;
    *out = s;
    return 0;
}

/* -------------------------------------------------------------------- PPS */

void hevc_pps_tiles(hevc_pps *p, const hevc_sps *s) {
    if (!p->tiles_enabled_flag) {
        p->num_tile_columns = p->num_tile_rows = 1;
        p->column_width[0] = s->ctb_width;
        p->row_height[0] = s->ctb_height;
        return;
    }
    if (p->uniform_spacing_flag) {
        for (int i = 0; i < p->num_tile_columns; ++i)
            p->column_width[i] = ((i + 1) * s->ctb_width) / p->num_tile_columns -
                                 (i * s->ctb_width) / p->num_tile_columns;
        for (int i = 0; i < p->num_tile_rows; ++i)
            p->row_height[i] = ((i + 1) * s->ctb_height) / p->num_tile_rows -
                               (i * s->ctb_height) / p->num_tile_rows;
    } else {   /* last column / row takes the rest */
        int sum = 0;
        for (int i = 0; i < p->num_tile_columns - 1; ++i) sum += p->column_width[i];
        p->column_width[p->num_tile_columns - 1] = s->ctb_width - sum;
        sum = 0;
        for (int i = 0; i < p->num_tile_rows - 1; ++i) sum += p->row_height[i];
        p->row_height[p->num_tile_rows - 1] = s->ctb_height - sum;
    }
}

int hevc_parse_pps(const uint8_t *nal, size_t size, const hevc_sps *sps_table, hevc_pps *out) {
    static uint8_t rbsp[HEVC_RBSP_CAP];
    h264_bits b;
    h264_bits_init(&b, rbsp, load_rbsp(nal, size, rbsp));
    hevc_pps p;
    memset(&p, 0, sizeof(p));
    p.id = (int)h264_ue(&b);
    p.sps_id = (int)h264_ue(&b);
    if (p.id >= HEVC_MAX_PPS || p.sps_id >= HEVC_MAX_SPS || !sps_table[p.sps_id].valid) return -1;
    const hevc_sps *s = &sps_table[p.sps_id];
    p.dependent_slice_segments_enabled_flag = (int)h264_u(&b, 1);
    p.output_flag_present_flag = (int)h264_u(&b, 1);
    p.num_extra_slice_header_bits = (int)h264_u(&b, 3);
    p.sign_data_hiding_enabled_flag = (int)h264_u(&b, 1);
    p.cabac_init_present_flag = (int)h264_u(&b, 1);
    p.num_ref_idx_l0_default_active = (int)h264_ue(&b) + 1;
    p.num_ref_idx_l1_default_active = (int)h264_ue(&b) + 1;
    p.init_qp_minus26 = h264_se(&b);
    p.constrained_intra_pred_flag = (int)h264_u(&b, 1);
    p.transform_skip_enabled_flag = (int)h264_u(&b, 1);
    p.cu_qp_delta_enabled_flag = (int)h264_u(&b, 1);
    if (p.cu_qp_delta_enabled_flag) p.diff_cu_qp_delta_depth = (int)h264_ue(&b);
    p.cb_qp_offset = h264_se(&b);
    p.cr_qp_offset = h264_se(&b);
    p.slice_chroma_qp_offsets_present_flag = (int)h264_u(&b, 1);
    p.weighted_pred_flag = (int)h264_u(&b, 1);
    p.weighted_bipred_flag = (int)h264_u(&b, 1);
    p.transquant_bypass_enabled_flag = (int)h264_u(&b, 1);
    p.tiles_enabled_flag = (int)h264_u(&b, 1);
    p.entropy_coding_sync_enabled_flag = (int)h264_u(&b, 1);
    if (p.tiles_enabled_flag) {
        p.num_tile_columns = (int)h264_ue(&b) + 1;
        p.num_tile_rows = (int)h264_ue(&b) + 1;
        if (p.num_tile_columns > 20 || p.num_tile_rows > 22) return -1;
        p.uniform_spacing_flag = (int)h264_u(&b, 1);
        if (!p.uniform_spacing_flag) {
            for (int i = 0; i < p.num_tile_columns - 1; ++i) p.column_width[i] = (int)h264_ue(&b) + 1;
            for (int i = 0; i < p.num_tile_rows - 1; ++i) p.row_height[i] = (int)h264_ue(&b) + 1;
        }
        p.loop_filter_across_tiles_enabled_flag = (int)h264_u(&b, 1);
    }
    p.loop_filter_across_slices_enabled_flag = (int)h264_u(&b, 1);
    p.deblocking_filter_control_present_flag = (int)h264_u(&b, 1);
    if (p.deblocking_filter_control_present_flag) {
        p.deblocking_filter_override_enabled_flag = (int)h264_u(&b, 1);
        p.pps_deblocking_filter_disabled_flag = (int)h264_u(&b, 1);
        if (!p.pps_deblocking_filter_disabled_flag) {
            p.beta_offset = h264_se(&b) * 2;
            p.tc_offset = h264_se(&b) * 2;
        }
    }
    p.scaling = s->scaling;
    p.scaling_list_data_present_flag = (int)h264_u(&b, 1);
    if (p.scaling_list_data_present_flag && parse_scaling(&b, &p.scaling)) return -1;
    p.lists_modification_present_flag = (int)h264_u(&b, 1);
    p.log2_parallel_merge_level = (int)h264_ue(&b) + 2;
    p.slice_segment_header_extension_present_flag = (int)h264_u(&b, 1);
    if (b.err) return -1;
    hevc_pps_tiles(&p, s);
    p.valid = 1;
    *out = p;
    return 0;
}

/* ---------------------------------------------------------- slice header */

int hevc_parse_slice(const uint8_t *nal, size_t size, const hevc_sps *sps_table,
                     const hevc_pps *pps_table, hevc_slice *out) {
    static uint8_t rbsp[HEVC_RBSP_CAP];
    h264_bits b;
    const size_t n = size < HEVC_RBSP_CAP ? size : HEVC_RBSP_CAP;
    h264_bits_init(&b, rbsp, load_rbsp(nal, n, rbsp));
    hevc_slice sl;
    memset(&sl, 0, sizeof(sl));
    sl.nal_type = hevc_nal_type(nal);
    sl.tid = hevc_nal_tid(nal);
    sl.first_slice_segment_in_pic_flag = (int)h264_u(&b, 1);
    if (hevc_is_irap(sl.nal_type)) sl.no_output_of_prior_pics_flag = (int)h264_u(&b, 1);
    sl.pps_id = (int)h264_ue(&b);
    if (sl.pps_id >= HEVC_MAX_PPS || !pps_table[sl.pps_id].valid) return -1;
    const hevc_pps *p = &pps_table[sl.pps_id];
    const hevc_sps *s = &sps_table[p->sps_id];
    if (!s->valid) return -1;
    if (!sl.first_slice_segment_in_pic_flag) {
        if (p->dependent_slice_segments_enabled_flag) sl.dependent_slice_segment_flag = (int)h264_u(&b, 1);
        sl.slice_segment_address = (int)h264_u(&b, ceil_log2(s->ctb_width * s->ctb_height));
    }
    sl.pic_output_flag = 1;
    if (sl.dependent_slice_segment_flag) {   /* header copied from the previous segment */
        *out = sl;
        return b.err ? -1 : 0;
    }
    h264_u(&b, p->num_extra_slice_header_bits);
    sl.slice_type = (int)h264_ue(&b);
    if (sl.slice_type > 2) return -1;
    const size_t skip_start = b.pos;
    if (p->output_flag_present_flag) sl.pic_output_flag = (int)h264_u(&b, 1);
    if (s->separate_colour_plane_flag) h264_u(&b, 2);
    if (!hevc_is_idr(sl.nal_type)) {
        sl.pic_order_cnt_lsb = (int)h264_u(&b, s->log2_max_poc_lsb);
        sl.short_term_ref_pic_set_sps_flag = (int)h264_u(&b, 1);
        if (!sl.short_term_ref_pic_set_sps_flag) {
            if (parse_st_rps(&b, s->num_short_term_ref_pic_sets, s->num_short_term_ref_pic_sets,
                             s->st_rps, &sl.st_rps))
                return -1;
            sl.short_term_ref_pic_set_idx = s->num_short_term_ref_pic_sets;
        } else {
            if (!s->num_short_term_ref_pic_sets) return -1;
            const int bits = ceil_log2(s->num_short_term_ref_pic_sets);
            sl.short_term_ref_pic_set_idx = bits ? (int)h264_u(&b, bits) : 0;
            if (sl.short_term_ref_pic_set_idx >= s->num_short_term_ref_pic_sets) return -1;
            sl.st_rps = s->st_rps[sl.short_term_ref_pic_set_idx];
        }
        if (s->long_term_ref_pics_present_flag) {
            int num_lt_sps = 0;
            if (s->num_long_term_ref_pics_sps > 0) num_lt_sps = (int)h264_ue(&b);
            const int num_lt_pics = (int)h264_ue(&b);
            sl.num_long_term = num_lt_sps + num_lt_pics;
            if (num_lt_sps > s->num_long_term_ref_pics_sps || sl.num_long_term > 32) return -1;
            for (int i = 0; i < sl.num_long_term; ++i) {
                if (i < num_lt_sps) {
                    const int bits = ceil_log2(s->num_long_term_ref_pics_sps);
                    const int idx = bits ? (int)h264_u(&b, bits) : 0;
                    sl.poc_lsb_lt[i] = s->lt_ref_pic_poc_lsb_sps[idx];
                    sl.used_by_curr_pic_lt[i] = s->used_by_curr_pic_lt_sps[idx];
                } else {
                    sl.poc_lsb_lt[i] = (int)h264_u(&b, s->log2_max_poc_lsb);
                    sl.used_by_curr_pic_lt[i] = (int)h264_u(&b, 1);
                }
                sl.delta_poc_msb_present[i] = (int)h264_u(&b, 1);
                int cycle = sl.delta_poc_msb_present[i] ? (int)h264_ue(&b) : 0;
                if (i != 0 && i != num_lt_sps) cycle += sl.delta_poc_msb_cycle_lt[i - 1];
                sl.delta_poc_msb_cycle_lt[i] = cycle;
            }
        }
    }
    sl.skip_bits = (uint32_t)(b.pos - skip_start);
    if (b.err) return -1;
    *out = sl;
    return 0;
}
