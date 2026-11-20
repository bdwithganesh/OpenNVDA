/* HEVC on NVDEC: see nvdec_hevc.h. Clause numbers are ITU-T H.265. */
#include "nvdec_hevc.h"

#include <string.h>

_Static_assert(sizeof(nvdec_hevc_pic_s) <= NVDEC_IN_STATUS, "pic setup overlaps the status block");

void nvdec_hevc_init(nvdec_hevc_dec *d) {
    memset(d, 0, sizeof(*d));
    d->sps_id = -1;
    d->first = 1;
    d->cur = -1;
}

int nvdec_hevc_param_nal(nvdec_hevc_dec *d, const uint8_t *nal, size_t size) {
    if (size < 2) return NVDEC_HEVC_EPARAM;
    const int t = hevc_nal_type(nal);
    if (t == HEVC_NAL_SPS) {
        hevc_sps s;
        if (hevc_parse_sps(nal, size, &s) || s.id < 0 || s.id >= HEVC_MAX_SPS) return NVDEC_HEVC_EPARAM;
        d->sps[s.id] = s;
    } else if (t == HEVC_NAL_PPS) {
        hevc_pps p;
        if (hevc_parse_pps(nal, size, d->sps, &p) || p.id < 0 || p.id >= HEVC_MAX_PPS) return NVDEC_HEVC_EPARAM;
        d->pps[p.id] = p;
    } else if (t == HEVC_NAL_EOS || t == HEVC_NAL_EOB) {
        d->eos = 1;
    }
    return NVDEC_HEVC_OK;
}

int nvdec_hevc_sps_depth(const hevc_sps *s) {
    if (s->chroma_format_idc != 1 || s->bit_depth_luma != s->bit_depth_chroma) return 0;
    if (s->bit_depth_luma == 8) return 8;
    return s->bit_depth_luma == 10 && s->main10_profile ? 10 : 0;
}

void nvdec_hevc_layout_for(const hevc_sps *s, nvdec_hevc_layout *l) {
    memset(l, 0, sizeof(*l));
    l->width = (uint32_t)s->width;
    l->height = (uint32_t)s->height;
    l->bytes_per_sample = s->bit_depth_luma > 8 ? 2 : 1;   /* 10-bit: 16-bit words */
    const uint32_t w64 = (l->width + 63) & ~63u, h64 = (l->height + 63) & ~63u;
    l->pitch = ((l->width + 63) & ~63u) * l->bytes_per_sample;   /* whole GOBs; framestride (samples) stays a multiple of 64 */
    l->aligned_height = h64;                 /* whole CTBs; chroma rows stay whole 16-row blocks */
    l->luma_bytes = l->pitch * l->aligned_height;
    l->surface_bytes = (l->luma_bytes + l->luma_bytes / 2 + 0xff) & ~0xffu;
    l->coloc_frame_bytes = (w64 * h64 / 16 + 0xff) & ~0xffu;   /* MVs of one picture */
    l->coloc_bytes = l->coloc_frame_bytes * (NVDEC_HEVC_SURFACES + 1);
    l->sao_offset = 480 * h64;                                 /* deblock | SAO | BSD control */
    l->bsd_offset = (480 + 3840) * h64;
    l->filter_bytes = (480 + 3840 + 60) * h64;
}

/* ---------------------------------------------------------------- DPB (C.5.2) */
static int count_output(const nvdec_hevc_dec *d) {
    int n = 0;
    for (int i = 0; i < NVDEC_HEVC_SURFACES; ++i) n += d->fs[i].used && d->fs[i].output;
    return n;
}
static int count_used(const nvdec_hevc_dec *d) {
    int n = 0;
    for (int i = 0; i < NVDEC_HEVC_SURFACES; ++i) n += d->fs[i].used;
    return n;
}
static void drop_unneeded(nvdec_hevc_dec *d) {
    for (int i = 0; i < NVDEC_HEVC_SURFACES; ++i)
        if (d->fs[i].used && !d->fs[i].output && !d->fs[i].mark && i != d->cur) d->fs[i].used = 0;
}
/* C.5.2.4: output the smallest-POC picture waiting for output. 0 = nothing to output. */
static int bump(nvdec_hevc_dec *d) {
    int best = -1;
    for (int i = 0; i < NVDEC_HEVC_SURFACES; ++i)
        if (d->fs[i].used && d->fs[i].output && (best < 0 || d->fs[i].poc < d->fs[best].poc)) best = i;
    if (best < 0) return 0;
    if (d->n_out < NVDEC_HEVC_MAX_OUT) {
        d->out_surface[d->n_out] = best;
        d->out_poc[d->n_out++] = d->fs[best].poc;
    }
    d->fs[best].output = 0;
    if (!d->fs[best].mark && best != d->cur) d->fs[best].used = 0;
    return 1;
}
static int latency_hit(const nvdec_hevc_dec *d, const hevc_sps *s) {
    if (!s->max_latency_increase) return 0;
    const int limit = s->max_num_reorder + s->max_latency_increase - 1;   /* SpsMaxLatencyPictures */
    for (int i = 0; i < NVDEC_HEVC_SURFACES; ++i)
        if (d->fs[i].used && d->fs[i].output && d->fs[i].latency >= limit) return 1;
    return 0;
}

void nvdec_hevc_flush(nvdec_hevc_dec *d) {
    while (bump(d)) {}
    for (int i = 0; i < NVDEC_HEVC_SURFACES; ++i) d->fs[i].mark = d->fs[i].used = 0;
}

/* --------------------------------------------------------------------- tables */
static void write_scaling(const hevc_scaling *sc, uint8_t *o) {
    /* nvdec_drv.h GIP_ASIC_SCALING_LIST_SIZE: 6 + 2 DC, 8 pad, 6x16, 6x64, 6x64, 2x64 (raster) */
    memset(o, 0, 1008);
    for (int m = 0; m < 6; ++m) o[m] = sc->dc16x16[m];
    o[6] = sc->dc32x32[0];
    o[7] = sc->dc32x32[3];
    for (int m = 0; m < 6; ++m) {
        memcpy(o + 16 + m * 16, sc->list4x4[m], 16);
        memcpy(o + 112 + m * 64, sc->list8x8[m], 64);
        memcpy(o + 496 + m * 64, sc->list16x16[m], 64);
    }
    memcpy(o + 880, sc->list32x32[0], 64);
    memcpy(o + 944, sc->list32x32[3], 64);
}

static void write_tiles(const hevc_pps *p, const hevc_sps *s, uint8_t *out) {
    uint16_t *t = (uint16_t *)out;
    memset(out, 0, 0x900);
    for (int r = 0; r < p->num_tile_rows; ++r)
        for (int c = 0; c < p->num_tile_columns; ++c) {
            t[2 * (r * p->num_tile_columns + c)] = (uint16_t)p->column_width[c];
            t[2 * (r * p->num_tile_columns + c) + 1] = (uint16_t)p->row_height[r];
        }
    if (!p->tiles_enabled_flag) return;
    /* shortcut: tile boundary table layout (u16 at 0x700: column ends then row ends, 16-sample
     * units) is from the Tegra hwaccel and untested here: x265 emits no tiles; test with a tiled
     * conformance stream before relying on it */
    const int shift = s->log2_max_cb - 4;
    int at = 0x380, sum = 0;
    for (int c = 0; c < p->num_tile_columns; ++c) { sum += p->column_width[c]; t[at++] = (uint16_t)(sum << shift); }
    sum = 0;
    for (int r = 0; r < p->num_tile_rows; ++r) { sum += p->row_height[r]; t[at++] = (uint16_t)(sum << shift); }
}

/* ----------------------------------------------------------- picture begin/end */
static int find_poc(const nvdec_hevc_dec *d, int poc, int mask, int want_long) {
    for (int i = 0; i < NVDEC_HEVC_SURFACES; ++i) {
        const nvdec_hevc_frame *f = &d->fs[i];
        if (!f->used || !f->mark || (want_long ? 0 : f->mark != 1)) continue;
        if ((f->poc & mask) == poc) return i;
    }
    return -1;
}

int nvdec_hevc_begin(nvdec_hevc_dec *d, const hevc_slice *sl, uint8_t *in, nvdec_hevc_pic_s *o, int *surface) {
    if (!sl->first_slice_segment_in_pic_flag || sl->dependent_slice_segment_flag) return NVDEC_HEVC_EPARAM;
    hevc_pps *p = &d->pps[sl->pps_id];
    if (!p->valid || p->sps_id < 0 || p->sps_id >= HEVC_MAX_SPS || !d->sps[p->sps_id].valid) return NVDEC_HEVC_EPARAM;
    const hevc_sps *s = &d->sps[p->sps_id];
    if (!nvdec_hevc_sps_depth(s)) return NVDEC_HEVC_EFORMAT;
    hevc_pps_tiles(p, s);
    d->sps_id = p->sps_id;
    const int t = sl->nal_type, irap = hevc_is_irap(t), idr = hevc_is_idr(t);
    const int no_rasl = irap && (idr || hevc_is_bla(t) || d->first || d->eos);
    if (irap) d->irap_no_rasl = no_rasl;
    if (hevc_is_rasl(t) && d->irap_no_rasl) return NVDEC_HEVC_SKIP;   /* 8.1.3: not output, not decoded */

    /* 8.3.1 POC */
    const int max_lsb = 1 << s->log2_max_poc_lsb, lsb = sl->pic_order_cnt_lsb;
    int msb = 0;
    if (!(irap && no_rasl)) {
        const int prev_lsb = d->prev_tid0_poc & (max_lsb - 1), prev_msb = d->prev_tid0_poc - prev_lsb;
        if (lsb < prev_lsb && prev_lsb - lsb >= max_lsb / 2) msb = prev_msb + max_lsb;
        else if (lsb > prev_lsb && lsb - prev_lsb > max_lsb / 2) msb = prev_msb - max_lsb;
        else msb = prev_msb;
    }
    const int poc = msb + lsb;
    if (sl->tid == 0 && !hevc_is_rasl(t) && t != 6 && t != 7 && !hevc_is_slnr(t)) d->prev_tid0_poc = poc;

    /* 8.3.2 RPS: slots of StCurrBefore / StCurrAfter / LtCurr (-1 = missing) */
    int before[16], after[16], lt[32], nb = 0, na = 0, nl = 0;
    int keep[NVDEC_HEVC_SURFACES] = {0}, is_long[NVDEC_HEVC_SURFACES] = {0};
    if (!idr) {
        for (int i = 0; i < sl->num_long_term; ++i) {
            int v = sl->poc_lsb_lt[i], mask = max_lsb - 1;
            if (sl->delta_poc_msb_present[i]) {
                v += poc - sl->delta_poc_msb_cycle_lt[i] * max_lsb - (poc & (max_lsb - 1));
                mask = -1;
            }
            const int f = find_poc(d, v, mask, 1);
            if (f >= 0) { keep[f] = 1; is_long[f] = 1; }
            if (sl->used_by_curr_pic_lt[i]) lt[nl++] = f;
        }
        const hevc_st_rps *r = &sl->st_rps;
        for (int i = 0; i < r->num_negative; ++i) {
            const int f = find_poc(d, poc + r->delta_poc_s0[i], -1, 0);
            if (f >= 0 && !is_long[f]) keep[f] = 1;
            if (r->used_s0[i] && nb < 16) before[nb++] = f;
        }
        for (int i = 0; i < r->num_positive; ++i) {
            const int f = find_poc(d, poc + r->delta_poc_s1[i], -1, 0);
            if (f >= 0 && !is_long[f]) keep[f] = 1;
            if (r->used_s1[i] && na < 16) after[na++] = f;
        }
    }
    for (int i = 0; i < NVDEC_HEVC_SURFACES; ++i)
        if (d->fs[i].used) d->fs[i].mark = keep[i] ? (is_long[i] ? 2 : 1) : 0;

    /* C.5.2.2 output / removal before the current picture */
    d->cur = -1;
    if (irap && no_rasl && !d->first) {
        if (sl->no_output_of_prior_pics_flag) for (int i = 0; i < NVDEC_HEVC_SURFACES; ++i) d->fs[i].output = 0;
        nvdec_hevc_flush(d);
    } else {
        drop_unneeded(d);
        while ((count_output(d) > s->max_num_reorder || latency_hit(d, s) ||
                count_used(d) >= s->max_dec_pic_buffering) && bump(d)) {}
        drop_unneeded(d);
    }
    int cur = -1;
    for (int i = 0; i < NVDEC_HEVC_SURFACES && cur < 0; ++i) if (!d->fs[i].used) cur = i;
    if (cur < 0) return NVDEC_HEVC_ENOSURF;
    d->cur = cur;
    d->fs[cur] = (nvdec_hevc_frame){1, poc, 0, 0, 0};
    d->cur_output = sl->pic_output_flag && !(hevc_is_rasl(t) && d->irap_no_rasl);
    d->first = d->eos = 0;
    for (int i = 0; i < nb; ++i) if (before[i] < 0) before[i] = cur;   /* missing reference: shortcut, */
    for (int i = 0; i < na; ++i) if (after[i] < 0) after[i] = cur;     /* no generated frame (8.3.3)  */
    for (int i = 0; i < nl; ++i) if (lt[i] < 0) lt[i] = cur;

    /* setup */
    nvdec_hevc_layout l;
    nvdec_hevc_layout_for(s, &l);
    memset(o, 0, sizeof(*o));
    o->tileformat = 1;          /* KBL, as H.264 */
    o->gob_height = 0;          /* GOB_2 */
    o->sw_start_code_e = 1;
    /* measured 2026-11-20: NVDEC scales framestride by the output sample size (10-bit wrote at twice the pitch) */
    o->framestride[0] = o->framestride[1] = l.pitch / l.bytes_per_sample;
    /* nvdec_drv.h: 0 = Rec.709 8 bit, 1 = Rec.709 10 bit. Assumed to select 16-bit sample output; not measured. */
    o->disp_output_mode = s->bit_depth_luma > 8 ? 1 : 0;
    o->colMvBuffersize = l.coloc_frame_bytes >> 8;
    o->HevcSaoBufferOffset = l.sao_offset >> 8;
    o->HevcBsdCtrlOffset = l.bsd_offset >> 8;
    o->pic_width_in_luma_samples = (unsigned short)s->width;
    o->pic_height_in_luma_samples = (unsigned short)s->height;
    o->chroma_format_idc = 1;
    o->bit_depth_luma = (unsigned)s->bit_depth_luma;
    o->bit_depth_chroma = (unsigned)s->bit_depth_chroma;
    o->log2_min_luma_coding_block_size = (unsigned)s->log2_min_cb;
    o->log2_max_luma_coding_block_size = (unsigned)s->log2_max_cb;
    o->log2_min_transform_block_size = (unsigned)s->log2_min_tb;
    o->log2_max_transform_block_size = (unsigned)s->log2_max_tb;
    o->max_transform_hierarchy_depth_inter = (unsigned)s->max_th_depth_inter;
    o->max_transform_hierarchy_depth_intra = (unsigned)s->max_th_depth_intra;
    o->scalingListEnable = (unsigned)s->scaling_list_enabled_flag;
    o->amp_enable_flag = (unsigned)s->amp_enabled_flag;
    o->sample_adaptive_offset_enabled_flag = (unsigned)s->sao_enabled_flag;
    o->pcm_enabled_flag = (unsigned)s->pcm_enabled_flag;
    if (s->pcm_enabled_flag) {
        o->pcm_sample_bit_depth_luma = (unsigned)s->pcm_bit_depth_luma;
        o->pcm_sample_bit_depth_chroma = (unsigned)s->pcm_bit_depth_chroma;
        o->log2_min_pcm_luma_coding_block_size = (unsigned)s->log2_min_pcm_cb;
        o->log2_max_pcm_luma_coding_block_size = (unsigned)s->log2_max_pcm_cb;
        o->pcm_loop_filter_disabled_flag = (unsigned)s->pcm_loop_filter_disabled_flag;
    }
    o->sps_temporal_mvp_enabled_flag = (unsigned)s->temporal_mvp_enabled_flag;
    o->strong_intra_smoothing_enabled_flag = (unsigned)s->strong_intra_smoothing_enabled_flag;
    o->dependent_slice_segments_enabled_flag = (unsigned)p->dependent_slice_segments_enabled_flag;
    o->output_flag_present_flag = (unsigned)p->output_flag_present_flag;
    o->num_extra_slice_header_bits = (unsigned)p->num_extra_slice_header_bits;
    o->sign_data_hiding_enabled_flag = (unsigned)p->sign_data_hiding_enabled_flag;
    o->cabac_init_present_flag = (unsigned)p->cabac_init_present_flag;
    o->num_ref_idx_l0_default_active = (unsigned)p->num_ref_idx_l0_default_active;
    o->num_ref_idx_l1_default_active = (unsigned)p->num_ref_idx_l1_default_active;
    o->init_qp = (unsigned)(26 + p->init_qp_minus26 + 6 * (s->bit_depth_luma - 8));
    o->constrained_intra_pred_flag = (unsigned)p->constrained_intra_pred_flag;
    o->transform_skip_enabled_flag = (unsigned)p->transform_skip_enabled_flag;
    o->cu_qp_delta_enabled_flag = (unsigned)p->cu_qp_delta_enabled_flag;
    o->diff_cu_qp_delta_depth = (unsigned)p->diff_cu_qp_delta_depth;
    o->pps_cb_qp_offset = (char)p->cb_qp_offset;
    o->pps_cr_qp_offset = (char)p->cr_qp_offset;
    o->pps_beta_offset = (char)p->beta_offset;
    o->pps_tc_offset = (char)p->tc_offset;
    o->pps_slice_chroma_qp_offsets_present_flag = (unsigned)p->slice_chroma_qp_offsets_present_flag;
    o->weighted_pred_flag = (unsigned)p->weighted_pred_flag;
    o->weighted_bipred_flag = (unsigned)p->weighted_bipred_flag;
    o->transquant_bypass_enabled_flag = (unsigned)p->transquant_bypass_enabled_flag;
    o->tiles_enabled_flag = (unsigned)p->tiles_enabled_flag;
    o->entropy_coding_sync_enabled_flag = (unsigned)p->entropy_coding_sync_enabled_flag;
    o->num_tile_columns = (unsigned)p->num_tile_columns;
    o->num_tile_rows = (unsigned)p->num_tile_rows;
    o->loop_filter_across_tiles_enabled_flag = (unsigned)p->loop_filter_across_tiles_enabled_flag;
    o->loop_filter_across_slices_enabled_flag = (unsigned)p->loop_filter_across_slices_enabled_flag;
    o->deblocking_filter_control_present_flag = (unsigned)p->deblocking_filter_control_present_flag;
    o->deblocking_filter_override_enabled_flag = (unsigned)p->deblocking_filter_override_enabled_flag;
    o->pps_deblocking_filter_disabled_flag = (unsigned)p->pps_deblocking_filter_disabled_flag;
    o->lists_modification_present_flag = (unsigned)p->lists_modification_present_flag;
    o->log2_parallel_merge_level = (unsigned)p->log2_parallel_merge_level;
    o->slice_segment_header_extension_present_flag = (unsigned)p->slice_segment_header_extension_present_flag;
    o->num_ref_frames = (unsigned char)(nb + na + nl);
    for (int i = 0; i < NVDEC_HEVC_SURFACES; ++i) {
        const nvdec_hevc_frame *f = &d->fs[i];
        if (i == cur || !f->used || !f->mark) continue;
        int diff = poc - f->poc;
        o->RefDiffPicOrderCnts[i] = (short)(diff < -128 ? -128 : diff > 127 ? 127 : diff);
        if (f->mark == 2) o->longtermflag |= (unsigned short)(1u << (15 - i));
    }
    const int total = nb + na + nl;
    for (int k = 0; total && k < 16;) {   /* 8.3.4 initial lists, repeated to 16 entries */
        for (int i = 0; i < nb && k < 16; ++i) o->initreflistidxl0[k++] = (unsigned char)before[i];
        for (int i = 0; i < na && k < 16; ++i) o->initreflistidxl0[k++] = (unsigned char)after[i];
        for (int i = 0; i < nl && k < 16; ++i) o->initreflistidxl0[k++] = (unsigned char)lt[i];
    }
    for (int k = 0; total && k < 16;) {
        for (int i = 0; i < na && k < 16; ++i) o->initreflistidxl1[k++] = (unsigned char)after[i];
        for (int i = 0; i < nb && k < 16; ++i) o->initreflistidxl1[k++] = (unsigned char)before[i];
        for (int i = 0; i < nl && k < 16; ++i) o->initreflistidxl1[k++] = (unsigned char)lt[i];
    }
    o->IDR_picture_flag = (unsigned char)idr;
    o->RAP_picture_flag = (unsigned char)irap;
    o->curr_pic_idx = (unsigned char)cur;
    o->pattern_id = 2;
    o->sw_hdr_skip_length = (unsigned short)sl->skip_bits;
    o->separate_colour_plane_flag = (unsigned)s->separate_colour_plane_flag;
    o->log2_max_pic_order_cnt_lsb_minus4 = (unsigned)(s->log2_max_poc_lsb - 4);
    o->num_short_term_ref_pic_sets = (unsigned)s->num_short_term_ref_pic_sets;
    o->num_long_term_ref_pics_sps = (unsigned)s->num_long_term_ref_pics_sps;
    o->long_term_ref_pics_present_flag = (unsigned)s->long_term_ref_pics_present_flag;

    if (s->scaling_list_enabled_flag)
        write_scaling(p->scaling_list_data_present_flag ? &p->scaling : &s->scaling, in + NVDEC_HEVC_IN_SCALING);
    write_tiles(p, s, in + NVDEC_HEVC_IN_TILES);
    *surface = cur;
    return NVDEC_HEVC_OK;
}

void nvdec_hevc_end(nvdec_hevc_dec *d) {
    if (d->cur < 0) return;
    const hevc_sps *s = &d->sps[d->sps_id];
    /* C.5.2.3 */
    for (int i = 0; i < NVDEC_HEVC_SURFACES; ++i)
        if (i != d->cur && d->fs[i].used && d->fs[i].output) d->fs[i].latency++;
    d->fs[d->cur].output = d->cur_output;
    d->fs[d->cur].latency = 0;
    d->fs[d->cur].mark = 1;
    d->cur = -1;
    while ((count_output(d) > s->max_num_reorder || latency_hit(d, s)) && bump(d)) {}
}

int nvdec_hevc_stream_end(nvdec_h264_stream *s, nvdec_hevc_pic_s *setup) {
    if (!s->slices || (uint64_t)NVDEC_IN_BITSTREAM + s->len + 16 > s->in_bytes) return NVDEC_HEVC_EPARAM;
    memset(s->in + NVDEC_IN_BITSTREAM + s->len, 0, 16);
    setup->stream_len = s->len;
    memcpy(s->in + NVDEC_IN_SETUP, setup, sizeof(*setup));
    memset(s->in + NVDEC_IN_STATUS, 0, sizeof(nvdec_status_s));
    return NVDEC_HEVC_OK;
}

/* ------------------------------------------------------------------- methods */
static uint32_t hdr(uint32_t subch, uint32_t mthd, uint32_t n) {
    return (1u << 29) | (n << 16) | (subch << 13) | (mthd >> 2);
}
static int addr_ok(uint64_t va) { return !(va & 0xff) && va < (1ull << 40); }

uint32_t nvdec_hevc_push(const nvdec_hevc_addrs *a, const nvdec_hevc_layout *l, uint32_t picture_index,
                         uint32_t fence, uint32_t *out, uint32_t max) {
    const uint32_t need = 2 * 18 + 2 * (1 + NVDEC_HEVC_SURFACES) + 6;
    if (max < need || a->subch > 7 || !addr_ok(a->in_va) || !addr_ok(a->coloc_va) || !addr_ok(a->filter_va) || !addr_ok(a->misc_va) ||
        (l->luma_bytes & 0xff))
        return 0;
    for (int i = 0; i < NVDEC_HEVC_SURFACES; ++i)
        if (!addr_ok(a->surface_va[i]) || !addr_ok(a->surface_va[i] + l->luma_bytes)) return 0;
    uint32_t n = 0;
    const uint32_t sc = a->subch;
#define M(mthd, v) do { out[n++] = hdr(sc, (mthd), 1); out[n++] = (v); } while (0)
    M(0x000, a->obj_class);                                       /* SET_OBJECT */
    M(0x200, 7);                                                  /* SET_APPLICATION_ID HEVC */
    M(0x400, 7 | (1u << 4) | (1u << 6));                          /* CONTROL: HEVC, GPTIMER, ERR_CONCEAL */
    M(0x40c, picture_index);                                      /* SET_PICTURE_INDEX */
    M(0x404, (uint32_t)((a->in_va + NVDEC_IN_SETUP) >> 8));       /* DRV_PIC_SETUP */
    M(0x408, (uint32_t)((a->in_va + NVDEC_IN_BITSTREAM) >> 8));   /* IN_BUF_BASE */
    M(0x424, (uint32_t)((a->in_va + NVDEC_IN_STATUS) >> 8));      /* NVDEC_STATUS */
    M(0x580, (uint32_t)((a->in_va + NVDEC_HEVC_IN_SCALING) >> 8));/* HEVC_SET_SCALING_LIST */
    M(0x584, (uint32_t)((a->in_va + NVDEC_HEVC_IN_TILES) >> 8));  /* HEVC_SET_TILE_SIZES */
    M(0x588, (uint32_t)(a->filter_va >> 8));                      /* HEVC_SET_FILTER_BUFFER */
    M(0x414, (uint32_t)(a->coloc_va >> 8));                       /* COLOC_DATA */
    /* live 0.180.191: without these an IDR picture faulted NVDEC0 at VA 0 (RC 31, engine 0x13). Which
     * of them the HEVC firmware reads is not known yet: each gets its own zeroed 1 MiB. */
    M(0x418, (uint32_t)(a->misc_va >> 8));                        /* HISTORY */
    M(0x4c8, (uint32_t)((a->misc_va + (1u << 20)) >> 8));         /* INTRA_TOP_BUF */
    M(0x58c, (uint32_t)((a->misc_va + (2u << 20)) >> 8));         /* HEVC_SET_SAO_BUFFER */
    M(0x590, (uint32_t)((a->misc_va + (3u << 20)) >> 8));         /* HEVC_SET_SLICE_INFO_BUFFER */
    M(0x4b8, (uint32_t)((a->misc_va + (4u << 20)) >> 8));         /* PIC_SCRATCH_BUF */
    out[n++] = hdr(sc, 0x430, NVDEC_HEVC_SURFACES);               /* PICTURE_LUMA_OFFSET0..15 */
    for (int i = 0; i < NVDEC_HEVC_SURFACES; ++i) out[n++] = (uint32_t)(a->surface_va[i] >> 8);
    out[n++] = hdr(sc, 0x474, NVDEC_HEVC_SURFACES);               /* PICTURE_CHROMA_OFFSET0..15 */
    for (int i = 0; i < NVDEC_HEVC_SURFACES; ++i) out[n++] = (uint32_t)((a->surface_va[i] + l->luma_bytes) >> 8);
    M(0x300, 1u << 8);                                            /* EXECUTE, AWAKEN */
    const uint64_t sem = a->in_va + NVDEC_IN_SEM;
    out[n++] = hdr(sc, 0x240, 3);                                 /* SEMAPHORE_A/B/C */
    out[n++] = (uint32_t)(sem >> 32) & 0xff;
    out[n++] = (uint32_t)sem;
    out[n++] = fence;
    M(0x304, 0);                                                  /* SEMAPHORE_D release */
#undef M
    return n;
}
