/* hevc_mp4_params <file.mp4>: print the SPS/PPS coding tools of the hvcC parameter sets, as our parser reads
 * them (which tools a real-world stream uses that the test vectors may not).
 * cc -I../../drivers/nvdec hevc_mp4_params.c ../../drivers/nvdec/{nvdec_hevc,hevc_parse,nvdec_h264,h264_parse}.c */
#include "nvdec_hevc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *d = malloc((size_t)n);
    if (fread(d, 1, (size_t)n, f) != (size_t)n) return 1;
    unsigned char *h = NULL;
    for (long i = 0; i + 4 < n; i++) if (!memcmp(d + i, "hvcC", 4)) { h = d + i + 4; break; }
    if (!h) { puts("no hvcC"); return 1; }
    static nvdec_hevc_dec dec; nvdec_hevc_init(&dec);
    unsigned char *p = h + 22; int arrays = *p++;
    for (int a = 0; a < arrays; a++) {
        int type = *p++ & 63, count = p[0] << 8 | p[1]; p += 2;
        for (int k = 0; k < count; k++) {
            int len = p[0] << 8 | p[1]; p += 2;
            printf("nal type %d len %d -> %d\n", type, len, nvdec_hevc_param_nal(&dec, p, (size_t)len));
            p += len;
        }
    }
    for (int i = 0; i < HEVC_MAX_SPS; i++) if (dec.sps[i].valid) {
        const hevc_sps *s = &dec.sps[i];
        printf("sps %d: %dx%d crop l%d r%d t%d b%d depth %d/%d cb %d..%d tb %d..%d th %d/%d scaling %d amp %d sao %d pcm %d "
               "strps %d lt %d tmvp %d strong_intra %d reorder %d dpb %d\n", i, s->width, s->height, s->conf_left, s->conf_right,
               s->conf_top, s->conf_bottom, s->bit_depth_luma, s->bit_depth_chroma, s->log2_min_cb, s->log2_max_cb, s->log2_min_tb,
               s->log2_max_tb, s->max_th_depth_inter, s->max_th_depth_intra, s->scaling_list_enabled_flag, s->amp_enabled_flag,
               s->sao_enabled_flag, s->pcm_enabled_flag, s->num_short_term_ref_pic_sets, s->long_term_ref_pics_present_flag,
               s->temporal_mvp_enabled_flag, s->strong_intra_smoothing_enabled_flag, s->max_num_reorder, s->max_dec_pic_buffering);
    }
    for (int i = 0; i < HEVC_MAX_PPS; i++) if (dec.pps[i].valid) {
        const hevc_pps *q = &dec.pps[i];
        printf("pps %d: dep_slices %d out_flag %d extra_bits %d sdh %d cabac_init %d refs %d/%d qp %d cip %d tskip %d cuqpd %d/%d "
               "cbcr %d/%d slice_qp_offs %d wp %d/%d tqbypass %d tiles %d wpp %d lf_tiles %d lf_slices %d dbk_ctl %d override %d "
               "dbk_off %d beta %d tc %d scaling %d listmod %d pml %d ext %d\n", i, q->dependent_slice_segments_enabled_flag,
               q->output_flag_present_flag, q->num_extra_slice_header_bits, q->sign_data_hiding_enabled_flag,
               q->cabac_init_present_flag, q->num_ref_idx_l0_default_active, q->num_ref_idx_l1_default_active, q->init_qp_minus26,
               q->constrained_intra_pred_flag, q->transform_skip_enabled_flag, q->cu_qp_delta_enabled_flag, q->diff_cu_qp_delta_depth,
               q->cb_qp_offset, q->cr_qp_offset, q->slice_chroma_qp_offsets_present_flag, q->weighted_pred_flag,
               q->weighted_bipred_flag, q->transquant_bypass_enabled_flag, q->tiles_enabled_flag,
               q->entropy_coding_sync_enabled_flag, q->loop_filter_across_tiles_enabled_flag,
               q->loop_filter_across_slices_enabled_flag, q->deblocking_filter_control_present_flag,
               q->deblocking_filter_override_enabled_flag, q->pps_deblocking_filter_disabled_flag, q->beta_offset, q->tc_offset,
               q->scaling_list_data_present_flag, q->lists_modification_present_flag, q->log2_parallel_merge_level,
               q->slice_segment_header_extension_present_flag);
    }
    return 0;
}
