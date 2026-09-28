/*
 * H.264 (ITU-T H.264 / ISO 14496-10) syntax parser for the NVDEC driver: Annex
 * B NAL splitting, SPS, PPS and the bits of the slice header the driver needs
 * (NVDEC firmware parses the slice data and the rest of the header on its own).
 * Plain C99, no allocation, builds on the host and on macOS.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define H264_MAX_SPS 32
#define H264_MAX_PPS 256
#define H264_MAX_MMCO 66

enum {
    H264_NAL_SLICE = 1,
    H264_NAL_IDR = 5,
    H264_NAL_SEI = 6,
    H264_NAL_SPS = 7,
    H264_NAL_PPS = 8,
    H264_NAL_AUD = 9,
    H264_NAL_EOSEQ = 10,
    H264_NAL_EOSTREAM = 11,
};

enum { H264_SLICE_P = 0, H264_SLICE_B = 1, H264_SLICE_I = 2, H264_SLICE_SP = 3, H264_SLICE_SI = 4 };

/*
 * One NAL unit inside an Annex B buffer: [data, data + size) starts with the
 * NAL header byte and is still escaped (emulation prevention bytes kept).
 */
typedef struct {
    const uint8_t *data;
    size_t size;
    int type, ref_idc;
} h264_nal;

/* Next NAL at or after *pos (start code search). 0 = none left. */
int h264_next_nal(const uint8_t *buf, size_t len, size_t *pos, h264_nal *nal);

/* Remove emulation prevention bytes (00 00 03 -> 00 00). Returns bytes out. */
size_t h264_unescape(const uint8_t *src, size_t n, uint8_t *dst, size_t cap);

typedef struct {
    const uint8_t *p;
    size_t bits, pos;
    int err;
} h264_bits;

void h264_bits_init(h264_bits *b, const uint8_t *rbsp, size_t bytes);
uint32_t h264_u(h264_bits *b, int n);
uint32_t h264_ue(h264_bits *b);
int32_t h264_se(h264_bits *b);
int h264_more_rbsp_data(const h264_bits *b);

typedef struct {
    int valid;
    int profile_idc, constraint_flags, level_idc, id;
    int chroma_format_idc, separate_colour_plane_flag;
    int bit_depth_luma, bit_depth_chroma;
    int qpprime_y_zero_transform_bypass_flag;
    int seq_scaling_matrix_present_flag;
    uint8_t scaling4[6][16];   /* raster order, after fall-back rule A */
    uint8_t scaling8[6][64];
    int log2_max_frame_num;
    int poc_type, log2_max_poc_lsb, delta_pic_order_always_zero_flag;
    int offset_for_non_ref_pic, offset_for_top_to_bottom_field;
    int num_ref_frames_in_poc_cycle;
    int offset_for_ref_frame[256];
    int max_num_ref_frames, gaps_in_frame_num_allowed_flag;
    int pic_width_in_mbs, pic_height_in_map_units, frame_mbs_only_flag;
    int mb_adaptive_frame_field_flag, direct_8x8_inference_flag;
    int crop_left, crop_right, crop_top, crop_bottom;   /* in luma samples */
    int bitstream_restriction_flag, num_reorder_frames, max_dec_frame_buffering;
    /* derived */
    int frame_height_in_mbs, width, height;             /* coded size */
} h264_sps;

typedef struct {
    int valid;
    int id, sps_id;
    int entropy_coding_mode_flag, bottom_field_pic_order_in_frame_present_flag;
    int num_slice_groups;
    int num_ref_idx_l0_default_active, num_ref_idx_l1_default_active;
    int weighted_pred_flag, weighted_bipred_idc;
    int pic_init_qp, pic_init_qs, chroma_qp_index_offset;
    int deblocking_filter_control_present_flag, constrained_intra_pred_flag;
    int redundant_pic_cnt_present_flag;
    int transform_8x8_mode_flag, pic_scaling_matrix_present_flag;
    int second_chroma_qp_index_offset;
    uint8_t scaling4[6][16];   /* raster order, after fall-back rule B */
    uint8_t scaling8[6][64];
} h264_pps;

typedef struct {
    int op;           /* memory_management_control_operation 1..6 */
    int diff_pic_nums_minus1, long_term_pic_num, long_term_frame_idx;
    int max_long_term_frame_idx_plus1;
} h264_mmco;

typedef struct {
    int nal_type, nal_ref_idc, idr;
    int first_mb_in_slice, slice_type, pps_id;
    int frame_num, field_pic_flag, bottom_field_flag;
    int idr_pic_id, pic_order_cnt_lsb, delta_pic_order_cnt_bottom;
    int delta_pic_order_cnt[2];
    int redundant_pic_cnt;
    int num_ref_idx_active[2];
    /* dec_ref_pic_marking */
    int no_output_of_prior_pics_flag, long_term_reference_flag;
    int adaptive_ref_pic_marking_mode_flag, n_mmco;
    h264_mmco mmco[H264_MAX_MMCO];
} h264_slice;

/*
 * Parse an SPS / PPS NAL (escaped, with header byte). 0 = ok. PPS needs the SPS
 * table (scaling fall-back, chroma format).
 */
int h264_parse_sps(const uint8_t *nal, size_t size, h264_sps *out);
int h264_parse_pps(const uint8_t *nal, size_t size, const h264_sps *sps_table, h264_pps *out);
/* Slice header up to and including dec_ref_pic_marking. */
int h264_parse_slice(const uint8_t *nal, size_t size, const h264_sps *sps_table,
                     const h264_pps *pps_table, h264_slice *out);

/*
 * Default DPB capacity in frames for the SPS level / size (Table A-1), or
 * max_dec_frame_buffering when the VUI gives it.
 */
int h264_dpb_frames(const h264_sps *sps);
