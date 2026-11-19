/*
 * HEVC (Main, 4:2:0 8-bit) decode on NVDEC (NVC9B0, AD103): driver-side state machine, the HEVC
 * counterpart of nvdec_h264.h. The firmware parses the slice data and most of each slice header;
 * the driver supplies, per picture, nvdec_hevc_pic_s (SPS/PPS fields, reference slots and POC
 * differences, sw_hdr_skip_length), the scaling list and tile tables, the bitstream (00 00 01 +
 * NAL per slice segment) and the surfaces. Kept here: POC (8.3.1), RPS marking (8.3.2) and DPB
 * output ("bumping", C.5.2).
 *
 * Hardware conventions: methods from NVIDIA open-gpu-doc clc9b0.h (MIT); structures from
 * nvdec_drv.h (MIT). Buffer sizes / table formats as used by the open Tegra NVDEC hwaccel
 * (facts only); surfaces are block linear KBL / GOB_2 like H.264 (nvdec_h264.c bl_offset).
 */
#pragma once
#include <stdint.h>

#include "hevc_parse.h"
#include "nvdec_drv.h"
#include "nvdec_h264.h"   /* nvdec_h264_stream_*, NVDEC_IN_*, nvdec_surface_nv12_md5 */

#define NVDEC_HEVC_SURFACES 16   /* one per DPB slot = curr_pic_idx / reference index */
#define NVDEC_HEVC_MAX_OUT 32

enum {
    NVDEC_HEVC_OK = 0,
    NVDEC_HEVC_EPARAM = -1,    /* missing / bad VPS-SPS-PPS */
    NVDEC_HEVC_EFORMAT = -3,   /* not 4:2:0 8-bit */
    NVDEC_HEVC_ENOSURF = -4,
    NVDEC_HEVC_SKIP = 1,       /* RASL picture of a CRA/BLA that starts decoding: not decoded */
};

typedef struct {
    int used, poc, mark;       /* mark: 0 unused for reference, 1 short-term, 2 long-term */
    int output, latency;       /* needed for output, PicLatencyCount */
} nvdec_hevc_frame;

typedef struct {
    hevc_sps sps[HEVC_MAX_SPS];
    hevc_pps pps[HEVC_MAX_PPS];
    nvdec_hevc_frame fs[NVDEC_HEVC_SURFACES];   /* index = surface */
    int sps_id;                                 /* active SPS of the current picture */
    int first, eos;                             /* no picture yet / after an end of sequence */
    int prev_tid0_poc, irap_no_rasl;
    uint32_t pictures;                          /* SET_PICTURE_INDEX counter */
    int cur, cur_output;
    int out_surface[NVDEC_HEVC_MAX_OUT], out_poc[NVDEC_HEVC_MAX_OUT];
    int n_out;
} nvdec_hevc_dec;

typedef struct {
    uint32_t width, height;                 /* coded size (pic_*_in_luma_samples) */
    uint32_t pitch, aligned_height;         /* block-linear luma plane, height to 64 */
    uint32_t luma_bytes, surface_bytes;     /* chroma at +luma_bytes */
    uint32_t coloc_frame_bytes, coloc_bytes, filter_bytes, sao_offset, bsd_offset;
} nvdec_hevc_layout;

/* Input buffer extras (the rest as NVDEC_IN_*: setup 0, status 0x400, sem 0x500, slices 0x1000,
 * bitstream 0x8000). */
#define NVDEC_HEVC_IN_SCALING 0x0600u   /* 1008 B */
#define NVDEC_HEVC_IN_TILES 0x5000u     /* 0x900 B */
#define NVDEC_HEVC_MISC_BYTES (5u << 20)

typedef struct {
    uint64_t in_va, coloc_va, filter_va;
    uint64_t misc_va;   /* NVDEC_HEVC_MISC_BYTES zeroed: history, intra top, SAO, slice info, scratch */
    uint64_t surface_va[NVDEC_HEVC_SURFACES];
    uint32_t subch, obj_class;
} nvdec_hevc_addrs;

void nvdec_hevc_init(nvdec_hevc_dec *d);
/* VPS/SPS/PPS/EOS NALs (data starts at the 2-byte NAL header); others ignored. */
int nvdec_hevc_param_nal(nvdec_hevc_dec *d, const uint8_t *nal, size_t size);
void nvdec_hevc_layout_for(const hevc_sps *s, nvdec_hevc_layout *l);
/* First slice segment of a picture: POC, RPS, bumping, surface, setup + scaling/tile tables
 * (written into `in`, the CPU view of the input buffer). Pictures leave in display order
 * through out_surface[] (read them before the next begin). */
int nvdec_hevc_begin(nvdec_hevc_dec *d, const hevc_slice *sl, uint8_t *in, nvdec_hevc_pic_s *setup,
                     int *surface);
void nvdec_hevc_end(nvdec_hevc_dec *d);
void nvdec_hevc_flush(nvdec_hevc_dec *d);
/* stream_len + setup copy + status clear (bitstream built with nvdec_h264_stream_begin/slice). */
int nvdec_hevc_stream_end(nvdec_h264_stream *s, nvdec_hevc_pic_s *setup);
uint32_t nvdec_hevc_push(const nvdec_hevc_addrs *a, const nvdec_hevc_layout *l, uint32_t picture_index,
                         uint32_t fence, uint32_t *out, uint32_t max);
