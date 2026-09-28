/* C9B7 H.264 bring-up: progressive NV12, one IDR slice per frame. */
#ifndef NVENC_H264_H
#define NVENC_H264_H
#include <stddef.h>
#include <stdint.h>
#define NV_NVENC_8_2 1
#include "nvenc_drv.h"

enum { NVENC_CLASS = 0xc9b7, NVENC_SETUP_BYTES = 2048,
       NVENC_STATUS_BYTES = 4096, NVENC_SEM_OFFSET = 2048 };

typedef struct {
    uint32_t width, height, qp, fps, ipcm, block_height;
} nvenc_h264_config;

typedef struct {
    uint32_t width, height, pitch, padded_width, padded_height, block_height;
    uint32_t luma_bytes, surface_bytes, bitstream_bytes, history_bytes, coloc_bytes;
} nvenc_h264_layout;

typedef struct {
    nvenc_h264_drv_pic_setup_s pic;
    nvenc_h264_slice_control_s slice;
    nvenc_h264_me_control_s me;
    nvenc_h264_md_control_s md;
    nvenc_h264_quant_control_s quant;
    nvenc_pred_weight_table_s weights;
} nvenc_h264_setup;

typedef struct {
    uint64_t setup, status, bitstream, input, recon, history, coloc, scratch;
    uint32_t subchannel;
} nvenc_h264_addresses;

int nvenc_h264_layout_init(const nvenc_h264_config *, nvenc_h264_layout *);
int nvenc_h264_setup_init(const nvenc_h264_config *, const nvenc_h264_layout *,
                         uint32_t picture, nvenc_h264_setup *);
/* A complete NV12 frame; padding repeats the nearest edge sample. */
int nvenc_h264_pack(const nvenc_h264_layout *, const uint8_t *, size_t,
                    uint8_t *, size_t);
size_t nvenc_h264_headers(const nvenc_h264_config *, uint8_t *, size_t);
size_t nvenc_h264_push(const nvenc_h264_addresses *, const nvenc_h264_layout *,
                       uint32_t picture, uint32_t fence, uint32_t *, size_t);
/* Call only after both the channel fence and engine semaphore have completed. */
int nvenc_h264_result(const void *status, size_t status_bytes, uint32_t picture,
                      const uint8_t *bitstream, size_t capacity,
                      size_t *offset, size_t *bytes);
#endif
