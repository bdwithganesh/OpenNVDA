/*
 * V1: HEVC (ITU-T H.265) syntax parser for the NVDEC driver: SPS, PPS
 * (incl. tiles, scaling lists) and the first part of the slice segment
 * header (up to the long-term RPS, which is what the driver needs; the
 * NVDEC firmware parses the rest, skipping sw_hdr_skip_length bits).
 * Uses the Annex B splitter and bit reader of h264_parse. Plain C99.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "h264_parse.h"

#define HEVC_MAX_SPS 16
#define HEVC_MAX_PPS 64
#define HEVC_MAX_ST_RPS 65          /* 64 in the SPS + 1 in a slice header */
#define HEVC_MAX_DPB 16

enum {
    HEVC_NAL_TRAIL_N = 0, HEVC_NAL_RASL_N = 8, HEVC_NAL_RASL_R = 9,
    HEVC_NAL_BLA_W_LP = 16, HEVC_NAL_BLA_N_LP = 18, HEVC_NAL_IDR_W_RADL = 19,
    HEVC_NAL_IDR_N_LP = 20, HEVC_NAL_CRA = 21, HEVC_NAL_VPS = 32, HEVC_NAL_SPS = 33,
    HEVC_NAL_PPS = 34, HEVC_NAL_AUD = 35, HEVC_NAL_EOS = 36, HEVC_NAL_EOB = 37,
};
enum { HEVC_SLICE_B = 0, HEVC_SLICE_P = 1, HEVC_SLICE_I = 2 };

static inline int hevc_nal_type(const uint8_t *nal) { return (nal[0] >> 1) & 0x3f; }
static inline int hevc_nal_layer(const uint8_t *nal) { return ((nal[0] & 1) << 5) | (nal[1] >> 3); }
static inline int hevc_nal_tid(const uint8_t *nal) { return (nal[1] & 7) - 1; }
static inline int hevc_is_irap(int t) { return t >= 16 && t <= 23; }
static inline int hevc_is_idr(int t) { return t == HEVC_NAL_IDR_W_RADL || t == HEVC_NAL_IDR_N_LP; }
static inline int hevc_is_bla(int t) { return t >= 16 && t <= 18; }
static inline int hevc_is_rasl(int t) { return t == HEVC_NAL_RASL_N || t == HEVC_NAL_RASL_R; }
static inline int hevc_is_slice(int t) { return t <= 9 || (t >= 16 && t <= 21); }
/* sub-layer non-reference picture (TRAIL_N .. RSV_VCL_N14, even types < 16) */
static inline int hevc_is_slnr(int t) { return t < 16 && !(t & 1); }

typedef struct {
    int num_negative, num_positive;
    int delta_poc_s0[16], used_s0[16];
    int delta_poc_s1[16], used_s1[16];
} hevc_st_rps;

typedef struct {
    uint8_t list4x4[6][16];          /* raster order */
    uint8_t list8x8[6][64];
    uint8_t list16x16[6][64];        /* 8x8 base matrices, raster */
    uint8_t list32x32[6][64];        /* matrixId 0 and 3 used for 4:2:0 */
    uint8_t dc16x16[6], dc32x32[6];
} hevc_scaling;

typedef struct {
    int valid, id;
    int chroma_format_idc, separate_colour_plane_flag;
    int width, height;                                  /* pic_*_in_luma_samples */
    int conf_left, conf_right, conf_top, conf_bottom;   /* in luma samples */
    int bit_depth_luma, bit_depth_chroma;
    int log2_max_poc_lsb;
    int max_dec_pic_buffering, max_num_reorder, max_latency_increase;   /* highest sub-layer */
    int log2_min_cb, log2_max_cb, log2_min_tb, log2_max_tb;
    int max_th_depth_inter, max_th_depth_intra;
    int scaling_list_enabled_flag;
    hevc_scaling scaling;
    int amp_enabled_flag, sao_enabled_flag;
    int pcm_enabled_flag, pcm_bit_depth_luma, pcm_bit_depth_chroma;
    int log2_min_pcm_cb, log2_max_pcm_cb, pcm_loop_filter_disabled_flag;
    int num_short_term_ref_pic_sets;
    hevc_st_rps st_rps[HEVC_MAX_ST_RPS];
    int long_term_ref_pics_present_flag, num_long_term_ref_pics_sps;
    int lt_ref_pic_poc_lsb_sps[33], used_by_curr_pic_lt_sps[33];
    int temporal_mvp_enabled_flag, strong_intra_smoothing_enabled_flag;
    /* derived */
    int ctb_width, ctb_height;
} hevc_sps;

typedef struct {
    int valid, id, sps_id;
    int dependent_slice_segments_enabled_flag, output_flag_present_flag;
    int num_extra_slice_header_bits, sign_data_hiding_enabled_flag, cabac_init_present_flag;
    int num_ref_idx_l0_default_active, num_ref_idx_l1_default_active;
    int init_qp_minus26, constrained_intra_pred_flag, transform_skip_enabled_flag;
    int cu_qp_delta_enabled_flag, diff_cu_qp_delta_depth;
    int cb_qp_offset, cr_qp_offset, slice_chroma_qp_offsets_present_flag;
    int weighted_pred_flag, weighted_bipred_flag, transquant_bypass_enabled_flag;
    int tiles_enabled_flag, entropy_coding_sync_enabled_flag;
    int num_tile_columns, num_tile_rows, uniform_spacing_flag;
    int column_width[20], row_height[22];               /* in CTBs (filled after SPS is known) */
    int loop_filter_across_tiles_enabled_flag, loop_filter_across_slices_enabled_flag;
    int deblocking_filter_control_present_flag, deblocking_filter_override_enabled_flag;
    int pps_deblocking_filter_disabled_flag, beta_offset, tc_offset;   /* offsets x2 */
    int scaling_list_data_present_flag;
    hevc_scaling scaling;
    int lists_modification_present_flag, log2_parallel_merge_level;
    int slice_segment_header_extension_present_flag;
} hevc_pps;

typedef struct {
    int nal_type, tid, first_slice_segment_in_pic_flag, no_output_of_prior_pics_flag;
    int pps_id, dependent_slice_segment_flag, slice_segment_address, slice_type;
    int pic_output_flag, pic_order_cnt_lsb;
    int short_term_ref_pic_set_sps_flag, short_term_ref_pic_set_idx;
    hevc_st_rps st_rps;                                 /* the one in use */
    int num_long_term;                                  /* sps + slice entries */
    int poc_lsb_lt[33], used_by_curr_pic_lt[33], delta_poc_msb_present[33];
    int delta_poc_msb_cycle_lt[33];                     /* accumulated (7-52) */
    uint32_t skip_bits;          /* sw_hdr_skip_length: after slice_type .. end of LT RPS */
} hevc_slice;

int hevc_parse_sps(const uint8_t *nal, size_t size, hevc_sps *out);
int hevc_parse_pps(const uint8_t *nal, size_t size, const hevc_sps *sps_table, hevc_pps *out);
/* Tile column/row sizes in CTBs for `sps` (uniform spacing resolved). */
void hevc_pps_tiles(hevc_pps *pps, const hevc_sps *sps);
int hevc_parse_slice(const uint8_t *nal, size_t size, const hevc_sps *sps_table,
                     const hevc_pps *pps_table, hevc_slice *out);
