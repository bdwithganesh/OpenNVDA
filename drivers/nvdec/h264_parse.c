/* V1: H.264 syntax parser (see h264_parse.h). Written from the H.264
 * specification (7.3 syntax, 7.4 semantics, Table 7-2/7-3, Table A-1). */
#include "h264_parse.h"

#include <string.h>

/* ------------------------------------------------------------- NAL units */

int h264_next_nal(const uint8_t *buf, size_t len, size_t *pos, h264_nal *nal) {
    size_t i = *pos;
    while (i + 3 <= len && !(buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1)) ++i;
    if (i + 3 > len) { *pos = len; return 0; }
    const size_t start = i + 3;
    size_t end = start;
    while (end + 3 <= len && !(buf[end] == 0 && buf[end + 1] == 0 &&
                               (buf[end + 2] == 1 || buf[end + 2] == 0)))
        ++end;
    if (end + 3 > len) end = len;
    size_t trimmed = end;
    while (trimmed > start && buf[trimmed - 1] == 0) --trimmed;   /* trailing_zero_8bits */
    *pos = end;
    if (trimmed == start) return h264_next_nal(buf, len, pos, nal);
    nal->data = buf + start;
    nal->size = trimmed - start;
    nal->type = buf[start] & 0x1f;
    nal->ref_idc = (buf[start] >> 5) & 3;
    return 1;
}

size_t h264_unescape(const uint8_t *src, size_t n, uint8_t *dst, size_t cap) {
    size_t o = 0;
    int zeros = 0;
    for (size_t i = 0; i < n && o < cap; ++i) {
        if (zeros >= 2 && src[i] == 3) { zeros = 0; continue; }
        dst[o++] = src[i];
        zeros = src[i] ? 0 : zeros + 1;
    }
    return o;
}

/* ------------------------------------------------------------ bit reader */

void h264_bits_init(h264_bits *b, const uint8_t *rbsp, size_t bytes) {
    b->p = rbsp;
    b->bits = bytes * 8;
    b->pos = 0;
    b->err = 0;
}

uint32_t h264_u(h264_bits *b, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; ++i) {
        if (b->pos >= b->bits) { b->err = 1; return 0; }
        v = (v << 1) | ((b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1);
        ++b->pos;
    }
    return v;
}

uint32_t h264_ue(h264_bits *b) {
    int lz = 0;
    while (!h264_u(b, 1)) {
        if (b->err || ++lz > 31) { b->err = 1; return 0; }
    }
    return lz ? ((1u << lz) - 1) + h264_u(b, lz) : 0;
}

int32_t h264_se(h264_bits *b) {
    const uint32_t k = h264_ue(b);
    return (k & 1) ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2);
}

int h264_more_rbsp_data(const h264_bits *b) {
    if (b->pos >= b->bits) return 0;
    /* last 1 bit in the buffer is the rbsp_stop_one_bit */
    size_t last = b->bits;
    while (last > 0 && !((b->p[(last - 1) >> 3] >> (7 - ((last - 1) & 7))) & 1)) --last;
    return last > 0 && b->pos < last - 1;
}

/* RBSP of a NAL (header byte dropped) into a bounded local buffer. */
#define RBSP_CAP 4096
static size_t load_rbsp(const uint8_t *nal, size_t size, uint8_t *rbsp) {
    return size > 1 ? h264_unescape(nal + 1, size - 1, rbsp, RBSP_CAP) : 0;
}

/* -------------------------------------------------------- scaling lists */

static const uint8_t kZigzag4[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
static const uint8_t kZigzag8[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
static const uint8_t kDefault4Intra[16] = {6, 13, 13, 20, 20, 20, 28, 28, 28, 28, 32, 32, 32, 37, 37, 42};
static const uint8_t kDefault4Inter[16] = {10, 14, 14, 20, 20, 20, 24, 24, 24, 24, 27, 27, 27, 30, 30, 34};
static const uint8_t kDefault8Intra[64] = {
    6,  10, 10, 13, 11, 13, 16, 16, 16, 16, 18, 18, 18, 18, 18, 23,
    23, 23, 23, 23, 23, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27,
    27, 27, 27, 27, 29, 29, 29, 29, 29, 29, 29, 31, 31, 31, 31, 31,
    31, 33, 33, 33, 33, 33, 36, 36, 36, 36, 38, 38, 38, 40, 40, 42};
static const uint8_t kDefault8Inter[64] = {
    9,  13, 13, 15, 13, 15, 17, 17, 17, 17, 19, 19, 19, 19, 19, 21,
    21, 21, 21, 21, 21, 22, 22, 22, 22, 22, 22, 22, 24, 24, 24, 24,
    24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27, 27,
    27, 28, 28, 28, 28, 28, 30, 30, 30, 30, 32, 32, 32, 33, 33, 35};

/* zig-zag ordered list -> raster order */
static void to_raster(const uint8_t *zz, const uint8_t *scan, int n, uint8_t *out) {
    for (int i = 0; i < n; ++i) out[scan[i]] = zz[i];
}

/* scaling_list(): 1 = parsed, 0 = use the default list (7.3.2.1.1.1). */
static int parse_scaling_list(h264_bits *b, int n, const uint8_t *scan, uint8_t *raster) {
    uint8_t zz[64];
    int last = 8, next = 8;
    for (int j = 0; j < n; ++j) {
        if (next) {
            next = (last + h264_se(b) + 256) % 256;
            if (j == 0 && next == 0) return 0;
        }
        zz[j] = (uint8_t)(next ? next : last);
        last = zz[j];
    }
    to_raster(zz, scan, n, raster);
    return 1;
}

/* Parse the lists present in the bitstream; fall-back per Table 7-2.
 * seq4/seq8 = the sequence-level lists (rule B), NULL = rule A. */
static void parse_scaling_matrix(h264_bits *b, int count, uint8_t s4[6][16], uint8_t s8[6][64],
                                 const uint8_t (*seq4)[16], const uint8_t (*seq8)[64]) {
    for (int i = 0; i < 12; ++i) {
        const int present = i < count ? (int)h264_u(b, 1) : 0;
        if (i < 6) {
            if (present && parse_scaling_list(b, 16, kZigzag4, s4[i])) continue;
            if (present) {   /* useDefaultScalingMatrixFlag */
                to_raster(i < 3 ? kDefault4Intra : kDefault4Inter, kZigzag4, 16, s4[i]);
            } else if (i == 0 || i == 3) {
                if (seq4) memcpy(s4[i], seq4[i], 16);
                else to_raster(i == 0 ? kDefault4Intra : kDefault4Inter, kZigzag4, 16, s4[i]);
            } else {
                memcpy(s4[i], s4[i - 1], 16);
            }
        } else {
            const int k = i - 6;
            if (present && parse_scaling_list(b, 64, kZigzag8, s8[k])) continue;
            if (present) {
                to_raster((k & 1) ? kDefault8Inter : kDefault8Intra, kZigzag8, 64, s8[k]);
            } else if (k < 2) {
                if (seq8) memcpy(s8[k], seq8[k], 64);
                else to_raster(k ? kDefault8Inter : kDefault8Intra, kZigzag8, 64, s8[k]);
            } else {
                memcpy(s8[k], s8[k - 2], 64);
            }
        }
    }
}

static void flat_matrix(uint8_t s4[6][16], uint8_t s8[6][64]) {
    memset(s4, 16, 6 * 16);
    memset(s8, 16, 6 * 64);
}

/* -------------------------------------------------------------------- SPS */

static void skip_hrd(h264_bits *b) {
    const uint32_t cnt = h264_ue(b) + 1;
    h264_u(b, 8);
    for (uint32_t i = 0; i < cnt && i < 32 && !b->err; ++i) {
        h264_ue(b);
        h264_ue(b);
        h264_u(b, 1);
    }
    h264_u(b, 20);
}

static void parse_vui(h264_bits *b, h264_sps *s) {
    if (h264_u(b, 1) && h264_u(b, 8) == 255) h264_u(b, 32);   /* aspect ratio, SAR */
    if (h264_u(b, 1)) h264_u(b, 1);                           /* overscan */
    if (h264_u(b, 1)) {                                       /* video signal type */
        h264_u(b, 4);
        if (h264_u(b, 1)) h264_u(b, 24);
    }
    if (h264_u(b, 1)) { h264_ue(b); h264_ue(b); }             /* chroma loc */
    if (h264_u(b, 1)) { h264_u(b, 32); h264_u(b, 32); h264_u(b, 1); }   /* timing */
    const int nal_hrd = (int)h264_u(b, 1);
    if (nal_hrd) skip_hrd(b);
    const int vcl_hrd = (int)h264_u(b, 1);
    if (vcl_hrd) skip_hrd(b);
    if (nal_hrd || vcl_hrd) h264_u(b, 1);                     /* low_delay_hrd */
    h264_u(b, 1);                                             /* pic_struct_present */
    s->bitstream_restriction_flag = (int)h264_u(b, 1);
    if (s->bitstream_restriction_flag) {
        h264_u(b, 1);
        h264_ue(b); h264_ue(b); h264_ue(b); h264_ue(b);
        s->num_reorder_frames = (int)h264_ue(b);
        s->max_dec_frame_buffering = (int)h264_ue(b);
    }
}

static int high_profile(int p) {
    return p == 100 || p == 110 || p == 122 || p == 244 || p == 44 || p == 83 || p == 86 ||
           p == 118 || p == 128 || p == 138 || p == 139 || p == 134 || p == 135;
}

int h264_parse_sps(const uint8_t *nal, size_t size, h264_sps *out) {
    uint8_t rbsp[RBSP_CAP];
    h264_bits b;
    h264_bits_init(&b, rbsp, load_rbsp(nal, size, rbsp));
    h264_sps s;
    memset(&s, 0, sizeof(s));
    s.profile_idc = (int)h264_u(&b, 8);
    s.constraint_flags = (int)h264_u(&b, 8);
    s.level_idc = (int)h264_u(&b, 8);
    s.id = (int)h264_ue(&b);
    if (s.id >= H264_MAX_SPS) return -1;
    s.chroma_format_idc = 1;
    s.bit_depth_luma = s.bit_depth_chroma = 8;
    flat_matrix(s.scaling4, s.scaling8);
    if (high_profile(s.profile_idc)) {
        s.chroma_format_idc = (int)h264_ue(&b);
        if (s.chroma_format_idc > 3) return -1;
        if (s.chroma_format_idc == 3) s.separate_colour_plane_flag = (int)h264_u(&b, 1);
        s.bit_depth_luma = (int)h264_ue(&b) + 8;
        s.bit_depth_chroma = (int)h264_ue(&b) + 8;
        s.qpprime_y_zero_transform_bypass_flag = (int)h264_u(&b, 1);
        s.seq_scaling_matrix_present_flag = (int)h264_u(&b, 1);
        if (s.seq_scaling_matrix_present_flag)
            parse_scaling_matrix(&b, s.chroma_format_idc != 3 ? 8 : 12, s.scaling4, s.scaling8,
                                 NULL, NULL);
    }
    s.log2_max_frame_num = (int)h264_ue(&b) + 4;
    if (s.log2_max_frame_num > 16) return -1;
    s.poc_type = (int)h264_ue(&b);
    if (s.poc_type == 0) {
        s.log2_max_poc_lsb = (int)h264_ue(&b) + 4;
        if (s.log2_max_poc_lsb > 16) return -1;
    } else if (s.poc_type == 1) {
        s.delta_pic_order_always_zero_flag = (int)h264_u(&b, 1);
        s.offset_for_non_ref_pic = h264_se(&b);
        s.offset_for_top_to_bottom_field = h264_se(&b);
        s.num_ref_frames_in_poc_cycle = (int)h264_ue(&b);
        if (s.num_ref_frames_in_poc_cycle > 255) return -1;
        for (int i = 0; i < s.num_ref_frames_in_poc_cycle; ++i)
            s.offset_for_ref_frame[i] = h264_se(&b);
    } else if (s.poc_type != 2) {
        return -1;
    }
    s.max_num_ref_frames = (int)h264_ue(&b);
    s.gaps_in_frame_num_allowed_flag = (int)h264_u(&b, 1);
    s.pic_width_in_mbs = (int)h264_ue(&b) + 1;
    s.pic_height_in_map_units = (int)h264_ue(&b) + 1;
    s.frame_mbs_only_flag = (int)h264_u(&b, 1);
    if (!s.frame_mbs_only_flag) s.mb_adaptive_frame_field_flag = (int)h264_u(&b, 1);
    s.direct_8x8_inference_flag = (int)h264_u(&b, 1);
    s.frame_height_in_mbs = (2 - s.frame_mbs_only_flag) * s.pic_height_in_map_units;
    s.width = s.pic_width_in_mbs * 16;
    s.height = s.frame_height_in_mbs * 16;
    if (h264_u(&b, 1)) {
        const int cat = s.separate_colour_plane_flag ? 0 : s.chroma_format_idc;
        const int ux = cat == 0 || cat == 3 ? 1 : 2;
        const int uy = (cat == 1 ? 2 : 1) * (2 - s.frame_mbs_only_flag);
        s.crop_left = (int)h264_ue(&b) * ux;
        s.crop_right = (int)h264_ue(&b) * ux;
        s.crop_top = (int)h264_ue(&b) * uy;
        s.crop_bottom = (int)h264_ue(&b) * uy;
    }
    if (h264_u(&b, 1)) parse_vui(&b, &s);
    if (b.err || s.max_num_ref_frames > 16 || s.pic_width_in_mbs > 512 ||
        s.frame_height_in_mbs > 512)
        return -1;
    s.valid = 1;
    *out = s;
    return 0;
}

/* -------------------------------------------------------------------- PPS */

int h264_parse_pps(const uint8_t *nal, size_t size, const h264_sps *sps_table, h264_pps *out) {
    uint8_t rbsp[RBSP_CAP];
    h264_bits b;
    h264_bits_init(&b, rbsp, load_rbsp(nal, size, rbsp));
    h264_pps p;
    memset(&p, 0, sizeof(p));
    p.id = (int)h264_ue(&b);
    p.sps_id = (int)h264_ue(&b);
    if (p.id >= H264_MAX_PPS || p.sps_id >= H264_MAX_SPS || !sps_table[p.sps_id].valid) return -1;
    const h264_sps *s = &sps_table[p.sps_id];
    p.entropy_coding_mode_flag = (int)h264_u(&b, 1);
    p.bottom_field_pic_order_in_frame_present_flag = (int)h264_u(&b, 1);
    p.num_slice_groups = (int)h264_ue(&b) + 1;
    if (p.num_slice_groups != 1) return -2;   /* FMO (baseline/extended only): unsupported */
    p.num_ref_idx_l0_default_active = (int)h264_ue(&b) + 1;
    p.num_ref_idx_l1_default_active = (int)h264_ue(&b) + 1;
    if (p.num_ref_idx_l0_default_active > 32 || p.num_ref_idx_l1_default_active > 32) return -1;
    p.weighted_pred_flag = (int)h264_u(&b, 1);
    p.weighted_bipred_idc = (int)h264_u(&b, 2);
    p.pic_init_qp = 26 + h264_se(&b);
    p.pic_init_qs = 26 + h264_se(&b);
    p.chroma_qp_index_offset = h264_se(&b);
    p.deblocking_filter_control_present_flag = (int)h264_u(&b, 1);
    p.constrained_intra_pred_flag = (int)h264_u(&b, 1);
    p.redundant_pic_cnt_present_flag = (int)h264_u(&b, 1);
    p.second_chroma_qp_index_offset = p.chroma_qp_index_offset;
    memcpy(p.scaling4, s->scaling4, sizeof(p.scaling4));
    memcpy(p.scaling8, s->scaling8, sizeof(p.scaling8));
    if (h264_more_rbsp_data(&b)) {
        p.transform_8x8_mode_flag = (int)h264_u(&b, 1);
        p.pic_scaling_matrix_present_flag = (int)h264_u(&b, 1);
        if (p.pic_scaling_matrix_present_flag) {
            const int n = 6 + (s->chroma_format_idc != 3 ? 2 : 6) * p.transform_8x8_mode_flag;
            const int ruleB = s->seq_scaling_matrix_present_flag;
            parse_scaling_matrix(&b, n, p.scaling4, p.scaling8,
                                 ruleB ? (const uint8_t(*)[16])s->scaling4 : NULL,
                                 ruleB ? (const uint8_t(*)[64])s->scaling8 : NULL);
        }
        p.second_chroma_qp_index_offset = h264_se(&b);
    }
    if (b.err || p.pic_init_qp < 0 || p.pic_init_qp > 51) return -1;
    p.valid = 1;
    *out = p;
    return 0;
}

/* ------------------------------------------------------------ slice header */

static void skip_ref_list_mod(h264_bits *b) {
    if (!h264_u(b, 1)) return;
    for (int n = 0; n < 100 && !b->err; ++n) {
        const uint32_t idc = h264_ue(b);
        if (idc == 3) return;
        if (idc > 5) { b->err = 1; return; }
        h264_ue(b);
    }
    b->err = 1;
}

static void skip_pred_weight(h264_bits *b, const h264_slice *sl, int chroma) {
    h264_ue(b);
    if (chroma) h264_ue(b);
    for (int l = 0; l < (sl->slice_type == H264_SLICE_B ? 2 : 1); ++l)
        for (int i = 0; i < sl->num_ref_idx_active[l] && !b->err; ++i) {
            if (h264_u(b, 1)) { h264_se(b); h264_se(b); }
            if (chroma && h264_u(b, 1))
                for (int j = 0; j < 4; ++j) h264_se(b);
        }
}

int h264_parse_slice(const uint8_t *nal, size_t size, const h264_sps *sps_table,
                     const h264_pps *pps_table, h264_slice *out) {
    /* the header fits in the first bytes; bound the unescape work */
    uint8_t rbsp[RBSP_CAP];
    h264_bits b;
    h264_bits_init(&b, rbsp, load_rbsp(nal, size < RBSP_CAP ? size : RBSP_CAP, rbsp));
    h264_slice sl;
    memset(&sl, 0, sizeof(sl));
    sl.nal_type = nal[0] & 0x1f;
    sl.nal_ref_idc = (nal[0] >> 5) & 3;
    sl.idr = sl.nal_type == H264_NAL_IDR;
    sl.first_mb_in_slice = (int)h264_ue(&b);
    sl.slice_type = (int)(h264_ue(&b) % 5);
    sl.pps_id = (int)h264_ue(&b);
    if (sl.pps_id >= H264_MAX_PPS || !pps_table[sl.pps_id].valid) return -1;
    const h264_pps *p = &pps_table[sl.pps_id];
    const h264_sps *s = &sps_table[p->sps_id];
    if (!s->valid) return -1;
    if (s->separate_colour_plane_flag) h264_u(&b, 2);
    sl.frame_num = (int)h264_u(&b, s->log2_max_frame_num);
    if (!s->frame_mbs_only_flag) {
        sl.field_pic_flag = (int)h264_u(&b, 1);
        if (sl.field_pic_flag) sl.bottom_field_flag = (int)h264_u(&b, 1);
    }
    if (sl.idr) sl.idr_pic_id = (int)h264_ue(&b);
    if (s->poc_type == 0) {
        sl.pic_order_cnt_lsb = (int)h264_u(&b, s->log2_max_poc_lsb);
        if (p->bottom_field_pic_order_in_frame_present_flag && !sl.field_pic_flag)
            sl.delta_pic_order_cnt_bottom = h264_se(&b);
    }
    if (s->poc_type == 1 && !s->delta_pic_order_always_zero_flag) {
        sl.delta_pic_order_cnt[0] = h264_se(&b);
        if (p->bottom_field_pic_order_in_frame_present_flag && !sl.field_pic_flag)
            sl.delta_pic_order_cnt[1] = h264_se(&b);
    }
    if (p->redundant_pic_cnt_present_flag) sl.redundant_pic_cnt = (int)h264_ue(&b);
    sl.num_ref_idx_active[0] = p->num_ref_idx_l0_default_active;
    sl.num_ref_idx_active[1] = p->num_ref_idx_l1_default_active;
    const int st = sl.slice_type;
    if (st == H264_SLICE_B) h264_u(&b, 1);   /* direct_spatial_mv_pred_flag */
    if (st == H264_SLICE_P || st == H264_SLICE_SP || st == H264_SLICE_B) {
        if (h264_u(&b, 1)) {
            sl.num_ref_idx_active[0] = (int)h264_ue(&b) + 1;
            if (st == H264_SLICE_B) sl.num_ref_idx_active[1] = (int)h264_ue(&b) + 1;
        }
    }
    if (sl.num_ref_idx_active[0] > 32 || sl.num_ref_idx_active[1] > 32) return -1;
    if (st != H264_SLICE_I && st != H264_SLICE_SI) skip_ref_list_mod(&b);
    if (st == H264_SLICE_B) skip_ref_list_mod(&b);
    if ((p->weighted_pred_flag && (st == H264_SLICE_P || st == H264_SLICE_SP)) ||
        (p->weighted_bipred_idc == 1 && st == H264_SLICE_B))
        skip_pred_weight(&b, &sl, s->separate_colour_plane_flag ? 0 : s->chroma_format_idc);
    if (sl.nal_ref_idc) {
        if (sl.idr) {
            sl.no_output_of_prior_pics_flag = (int)h264_u(&b, 1);
            sl.long_term_reference_flag = (int)h264_u(&b, 1);
        } else {
            sl.adaptive_ref_pic_marking_mode_flag = (int)h264_u(&b, 1);
            while (sl.adaptive_ref_pic_marking_mode_flag && !b.err) {
                const int op = (int)h264_ue(&b);
                if (!op) break;
                if (op > 6 || sl.n_mmco >= H264_MAX_MMCO) return -1;
                h264_mmco *m = &sl.mmco[sl.n_mmco++];
                m->op = op;
                if (op == 1 || op == 3) m->diff_pic_nums_minus1 = (int)h264_ue(&b);
                if (op == 2) m->long_term_pic_num = (int)h264_ue(&b);
                if (op == 3 || op == 6) m->long_term_frame_idx = (int)h264_ue(&b);
                if (op == 4) m->max_long_term_frame_idx_plus1 = (int)h264_ue(&b);
            }
        }
    }
    if (b.err) return -1;
    *out = sl;
    return 0;
}

/* ------------------------------------------------------------ DPB size */

int h264_dpb_frames(const h264_sps *s) {
    int frames;
    if (s->bitstream_restriction_flag) {
        frames = s->max_dec_frame_buffering;
    } else {
        int mbs;
        switch (s->level_idc) {
        case 9: case 10: mbs = 396; break;
        case 11: mbs = (s->constraint_flags & 0x10) && s->profile_idc != 100 ? 396 : 900; break;
        case 12: case 13: case 20: mbs = 2376; break;
        case 21: mbs = 4752; break;
        case 22: case 30: mbs = 8100; break;
        case 31: mbs = 18000; break;
        case 32: mbs = 20480; break;
        case 40: case 41: mbs = 32768; break;
        case 42: mbs = 34816; break;
        case 50: mbs = 110400; break;
        case 51: case 52: mbs = 184320; break;
        default: mbs = 696320; break;   /* 6.x and unknown: largest */
        }
        frames = mbs / (s->pic_width_in_mbs * s->frame_height_in_mbs);
    }
    if (frames < s->max_num_ref_frames) frames = s->max_num_ref_frames;
    if (frames < 1) frames = 1;
    return frames > 16 ? 16 : frames;
}
