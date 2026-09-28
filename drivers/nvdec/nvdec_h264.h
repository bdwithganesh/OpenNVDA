/*
 * H.264 decode on NVDEC (NVC9B0, AD103), the driver-side state machine.
 * NVDEC firmware parses the slice data (and most of every slice header). Per
 * picture the driver gives it nvdec_h264_pic_s (SPS/PPS fields, POC, the
 * reference DPB), the bitstream with a slice offset table, and the surface
 * addresses. This file keeps the decoding process state from H.264 clause 8.2.1
 * (POC) and 8.2.5 (reference marking) plus the C.4 output order ("bumping"),
 * and builds the buffers and the method stream.
 *
 * Scope: frames (progressive and MBAFF) and field pictures (PAFF: both fields
 * of a frame share one frame store and surface, marking is per field); 4:2:0
 * 8-bit (NV12 out).
 *
 * Hardware conventions (NVIDIA open-gpu-doc clc9b0.h / nvdec_drv.h, MIT; method
 * order and buffer sizes follow the open Tegra NVDEC FFmpeg hwaccel): addresses
 * are GPU VA >> 8; surfaces are block linear (GOB 64 B x 8, block height 2 GOBs
 * = "TBL"/GOB_2), luma first then interleaved chroma; each slice is 00 00 01 +
 * NAL; the stream ends with a 16-byte end sequence; col_idx = pic_idx = surface
 * index.
 */
#pragma once
#include <stdint.h>

#include "h264_parse.h"
#include "nvdec_drv.h"

#define NVDEC_H264_SURFACES 17
#define NVDEC_H264_MAX_OUT 32

enum {
    NVDEC_H264_OK = 0,
    NVDEC_H264_EPARAM = -1,    /* missing / bad SPS or PPS */
    NVDEC_H264_EFIELD = -2,    /* field picture in a frame_mbs_only stream */
    NVDEC_H264_EFORMAT = -3,   /* not 4:2:0 8-bit */
    NVDEC_H264_ENOSURF = -4,   /* no free surface (stream breaks DPB rules) */
    NVDEC_H264_ESPACE = -5,    /* input buffer / slice table full */
};

typedef struct {
    int used;              /* frame store occupied */
    int surface;           /* 0..16, -1 = non-existing frame (gap) */
    int fields;            /* decoded fields: 1 top, 2 bottom, 3 both (frames) */
    int field_coded;       /* decoded as field picture(s) */
    int mark[2];           /* top, bottom: 0 unused, 1 short-term, 2 long-term */
    int frame_num, long_term_frame_idx;
    int poc_top, poc_bottom;   /* valid for the fields in `fields` */
    int output;            /* needed for output */
    int dpb_slot;          /* NVDEC dpb[] position while a reference, else -1 */
} nvdec_h264_frame;

typedef struct {
    h264_sps sps[H264_MAX_SPS];
    h264_pps pps[H264_MAX_PPS];
    int sps_id;                        /* active SPS (-1 none) */
    nvdec_h264_frame fs[NVDEC_H264_SURFACES];
    int dpb_frames, reorder;
    /* POC / frame_num state */
    int prev_poc_msb, prev_poc_lsb;
    int prev_frame_num_offset, prev_frame_num, prev_ref_frame_num;
    int prev_mmco5;                    /* previous picture had mmco5 */
    int max_long_term_frame_idx;       /* -1 = "no long-term frame indices" */
    uint32_t pictures;                 /* SET_PICTURE_INDEX counter */
    /* current picture (between begin and end) */
    int cur, cur_ref_idc, cur_idr, cur_poc_msb, cur_frame_num_offset;
    int cur_second;                    /* current picture is a second field */
    int open_field;                    /* store with a first field awaiting its pair, -1 */
    h264_slice cur_slice;
    /* output queue (display order), drained by the caller */
    int out_surface[NVDEC_H264_MAX_OUT], out_poc[NVDEC_H264_MAX_OUT];
    int n_out;
} nvdec_h264_dec;

void nvdec_h264_init(nvdec_h264_dec *d);
/* SPS / PPS NALs (other types ignored). */
int nvdec_h264_param_nal(nvdec_h264_dec *d, const h264_nal *nal);
/* 7.4.1.2.4: does slice `b` start a new picture after slice `a`? */
int nvdec_h264_new_picture(const nvdec_h264_dec *d, const h264_slice *a, const h264_slice *b);

/*
 * Per picture: begin (POC, gaps, IDR flush, surface, pic setup), the caller
 * decodes, then end (reference marking, output bumping). Pictures leave in
 * display order through out_surface[] (read them before the next begin).
 */
int nvdec_h264_begin(nvdec_h264_dec *d, const h264_slice *first, nvdec_h264_pic_s *setup,
                     int *surface);
void nvdec_h264_end(nvdec_h264_dec *d);
void nvdec_h264_flush(nvdec_h264_dec *d);

/* ---------------------------------------------------------- buffer layout */
typedef struct {
    uint32_t width_mbs, height_mbs, width, height;   /* coded size */
    uint32_t pitch, aligned_height;                  /* block-linear luma plane */
    uint32_t luma_bytes, surface_bytes;              /* chroma at +luma_bytes */
    uint32_t coloc_frame_bytes, coloc_bytes, mbhist_bytes, history_bytes;
} nvdec_h264_layout;

void nvdec_h264_layout_for(const h264_sps *sps, nvdec_h264_layout *l);

/* Input buffer (one picture in flight): */
#define NVDEC_IN_SETUP 0x0000u      /* nvdec_h264_pic_s (764 B) */
#define NVDEC_IN_STATUS 0x0400u     /* nvdec_status_s */
#define NVDEC_IN_SEM 0x0500u        /* engine semaphore (per-picture fence) */
#define NVDEC_IN_SLICES 0x1000u     /* u32 slice start offsets + end offset */
#define NVDEC_IN_MAX_SLICES 4095u
#define NVDEC_IN_BITSTREAM 0x8000u

typedef struct {
    uint8_t *in;          /* CPU view of the input buffer */
    uint32_t in_bytes;
    uint32_t len;         /* bitstream bytes so far */
    uint32_t slices;
} nvdec_h264_stream;

void nvdec_h264_stream_begin(nvdec_h264_stream *s, uint8_t *in, uint32_t in_bytes);
int nvdec_h264_stream_slice(nvdec_h264_stream *s, const h264_nal *nal);
/* Append the end sequence, fill stream_len/slice_count and copy `setup`. */
int nvdec_h264_stream_end(nvdec_h264_stream *s, nvdec_h264_pic_s *setup);

/* ------------------------------------------------------------ method stream */
typedef struct {
    uint64_t in_va, coloc_va, mbhist_va, history_va;
    uint64_t surface_va[NVDEC_H264_SURFACES];   /* luma; chroma = + luma_bytes */
    uint32_t subch, obj_class;
} nvdec_h264_addrs;

/*
 * SET_OBJECT, application/codec, picture index, buffers, the 17 surfaces,
 * EXECUTE, then an engine semaphore release of `fence` at in_va + NVDEC_IN_SEM.
 * Returns words written, 0 if `max` is too small or an address is not 256-byte
 * aligned / above 40 bits.
 */
uint32_t nvdec_h264_push(const nvdec_h264_addrs *a, const nvdec_h264_layout *l,
                         uint32_t picture_index, uint32_t fence, uint32_t *out, uint32_t max);

/*
 * Block-linear (GOB 64x8, 2 GOBs per block) <-> pitch-linear copy of one plane
 * of `rows` rows x `bytes_per_row` bytes; bl_pitch multiple of 64.
 */
void nvdec_detile(const uint8_t *bl, uint32_t bl_pitch, uint8_t *dst, uint32_t dst_pitch,
                  uint32_t bytes_per_row, uint32_t rows);
/*
 * MD5 of the cropped NV12 picture in a decoded block-linear surface: luma rows
 * (visible width) then interleaved chroma rows, as gen_vectors.py writes the
 * reference digests. 0 = ok, -1 = out of memory.
 */
int nvdec_surface_nv12_md5(const uint8_t *surface, uint32_t pitch, uint32_t luma_bytes,
                           uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, char hex[33]);
int nvdec_h264_surface_md5(const uint8_t *surface, const nvdec_h264_layout *l,
                           const h264_sps *sps, char hex[33]);
void nvdec_tile(const uint8_t *src, uint32_t src_pitch, uint8_t *bl, uint32_t bl_pitch,
                uint32_t bytes_per_row, uint32_t rows);
