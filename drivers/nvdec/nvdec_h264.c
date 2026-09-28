/*
 * H.264 on NVDEC: decoding process state, buffers and methods (see nvdec_h264.h).
 * POC: H.264 8.2.1; marking: 8.2.5 (per field); output: C.4.
 */
#include "nvdec_h264.h"

#include <stdlib.h>
#include <string.h>

#include "nvdec_md5.h"

void nvdec_h264_init(nvdec_h264_dec *d) {
    memset(d, 0, sizeof(*d));
    d->sps_id = -1;
    d->max_long_term_frame_idx = -1;
    d->cur = -1;
    d->open_field = -1;
    for (int i = 0; i < NVDEC_H264_SURFACES; ++i) d->fs[i].dpb_slot = -1;
}

int nvdec_h264_param_nal(nvdec_h264_dec *d, const h264_nal *nal) {
    if (nal->type == H264_NAL_SPS) {
        h264_sps s;
        if (h264_parse_sps(nal->data, nal->size, &s)) return NVDEC_H264_EPARAM;
        d->sps[s.id] = s;
    } else if (nal->type == H264_NAL_PPS) {
        h264_pps p;
        if (h264_parse_pps(nal->data, nal->size, d->sps, &p)) return NVDEC_H264_EPARAM;
        d->pps[p.id] = p;
    }
    return NVDEC_H264_OK;
}

int nvdec_h264_new_picture(const nvdec_h264_dec *d, const h264_slice *a, const h264_slice *b) {
    if (!a) return 1;
    if (b->first_mb_in_slice == 0) return 1;
    if (a->frame_num != b->frame_num || a->pps_id != b->pps_id ||
        a->field_pic_flag != b->field_pic_flag || a->bottom_field_flag != b->bottom_field_flag ||
        (a->nal_ref_idc == 0) != (b->nal_ref_idc == 0) || a->idr != b->idr ||
        (a->idr && a->idr_pic_id != b->idr_pic_id))
        return 1;
    const h264_sps *s = &d->sps[d->pps[b->pps_id].sps_id];
    if (s->poc_type == 0 && (a->pic_order_cnt_lsb != b->pic_order_cnt_lsb ||
                             a->delta_pic_order_cnt_bottom != b->delta_pic_order_cnt_bottom))
        return 1;
    if (s->poc_type == 1 && (a->delta_pic_order_cnt[0] != b->delta_pic_order_cnt[0] ||
                             a->delta_pic_order_cnt[1] != b->delta_pic_order_cnt[1]))
        return 1;
    return 0;
}

/* ------------------------------------------------------------ DPB helpers */

/*
 * Reference marking is kept per field (mark[0] top, mark[1] bottom); a frame or
 * a complementary field pair has both, a single field one.
 */
static int is_ref(const nvdec_h264_frame *f) { return f->mark[0] || f->mark[1]; }
static int has_mark(const nvdec_h264_frame *f, int m) { return f->mark[0] == m || f->mark[1] == m; }
static void unmark(nvdec_h264_frame *f, int m) {
    for (int k = 0; k < 2; ++k)
        if (f->mark[k] == m) f->mark[k] = 0;
}

static int frame_poc(const nvdec_h264_frame *f) {
    if (f->fields == 1) return f->poc_top;
    if (f->fields == 2) return f->poc_bottom;
    return f->poc_top < f->poc_bottom ? f->poc_top : f->poc_bottom;
}

/*
 * Output the waiting frame with the smallest POC (C.4.5.3), skipping `skip` and
 * any first field whose second field might still come.
 */
static int bump_one(nvdec_h264_dec *d, int skip) {
    int best = -1;
    for (int i = 0; i < NVDEC_H264_SURFACES; ++i) {
        const nvdec_h264_frame *f = &d->fs[i];
        if (i == skip || i == d->open_field || !f->used || !f->output) continue;
        if (best < 0 || frame_poc(f) < frame_poc(&d->fs[best])) best = i;
    }
    if (best < 0) return 0;
    nvdec_h264_frame *f = &d->fs[best];
    if (d->n_out < NVDEC_H264_MAX_OUT) {
        d->out_surface[d->n_out] = f->surface;
        d->out_poc[d->n_out] = frame_poc(f);
        ++d->n_out;
    }
    f->output = 0;
    if (!is_ref(f)) f->used = 0;
    return 1;
}

static void bump_all(nvdec_h264_dec *d, int skip) {
    while (bump_one(d, skip)) {}
}

/* Frames neither referenced nor waiting for output leave the DPB. */
static void cleanup(nvdec_h264_dec *d) {
    for (int i = 0; i < NVDEC_H264_SURFACES; ++i) {
        nvdec_h264_frame *f = &d->fs[i];
        if (f->used && !is_ref(f) && !f->output && i != d->cur) f->used = 0;
        if (!f->used || !is_ref(f)) f->dpb_slot = -1;
    }
}

static int count(const nvdec_h264_dec *d, int what) {   /* 0 used, 1 output, 2 ref */
    int n = 0;
    for (int i = 0; i < NVDEC_H264_SURFACES; ++i) {
        const nvdec_h264_frame *f = &d->fs[i];
        n += f->used && (what == 0 || (what == 1 && f->output) || (what == 2 && is_ref(f)));
    }
    return n;
}

static int frame_num_wrap(int fn, int cur_fn, int max_fn) {
    return fn > cur_fn ? fn - max_fn : fn;
}

/*
 * 8.2.5.3 sliding window, `cur_fn` = frame_num of the picture being decoded
 */
static void sliding_window(nvdec_h264_dec *d, const h264_sps *s, int cur_fn, int skip) {
    const int max_refs = s->max_num_ref_frames > 1 ? s->max_num_ref_frames : 1;
    const int max_fn = 1 << s->log2_max_frame_num;
    for (;;) {
        int refs = 0, oldest = -1;
        for (int i = 0; i < NVDEC_H264_SURFACES; ++i) {
            const nvdec_h264_frame *f = &d->fs[i];
            if (i == skip || !f->used || !is_ref(f)) continue;
            ++refs;
            if (has_mark(f, 1) &&
                (oldest < 0 || frame_num_wrap(f->frame_num, cur_fn, max_fn) <
                               frame_num_wrap(d->fs[oldest].frame_num, cur_fn, max_fn)))
                oldest = i;
        }
        if (refs < max_refs || oldest < 0) return;
        unmark(&d->fs[oldest], 1);
    }
}

static int free_slot(nvdec_h264_dec *d) {
    for (;;) {
        for (int i = 0; i < NVDEC_H264_SURFACES; ++i)
            if (!d->fs[i].used) return i;
        if (!bump_one(d, -1)) return -1;
    }
}

static int free_surface(const nvdec_h264_dec *d) {
    for (int s = 0; s < NVDEC_H264_SURFACES; ++s) {
        int busy = 0;
        for (int i = 0; i < NVDEC_H264_SURFACES && !busy; ++i)
            busy = d->fs[i].used && d->fs[i].surface == s;
        if (!busy) return s;
    }
    return -1;
}

static void unmark_all(nvdec_h264_dec *d) {
    for (int i = 0; i < NVDEC_H264_SURFACES; ++i) d->fs[i].mark[0] = d->fs[i].mark[1] = 0;
}

/* Activate an SPS: a new sequence (size / DPB change) starts from empty. */
static void activate(nvdec_h264_dec *d, int sps_id) {
    const h264_sps *s = &d->sps[sps_id];
    if (d->sps_id >= 0) {
        const h264_sps *o = &d->sps[d->sps_id];
        if (o->pic_width_in_mbs != s->pic_width_in_mbs ||
            o->frame_height_in_mbs != s->frame_height_in_mbs) {
            unmark_all(d);
            d->open_field = -1;
            bump_all(d, -1);
            cleanup(d);
        }
    }
    d->sps_id = sps_id;
    d->dpb_frames = h264_dpb_frames(s);
    d->reorder = s->bitstream_restriction_flag ? s->num_reorder_frames : d->dpb_frames;
    if (d->reorder > d->dpb_frames) d->reorder = d->dpb_frames;
}

/* --------------------------------------------------------------- picture */

/*
 * 8.2.1. Frames: both field POCs; field pictures: *top = *bottom = the POC of
 * that field
 */
static void compute_poc(nvdec_h264_dec *d, const h264_sps *s, const h264_slice *sl,
                        int *top, int *bottom) {
    const int max_fn = 1 << s->log2_max_frame_num;
    const int field = sl->field_pic_flag;
    if (s->poc_type == 0) {
        const int max_lsb = 1 << s->log2_max_poc_lsb;
        const int lsb = sl->pic_order_cnt_lsb;
        const int pmsb = sl->idr ? 0 : d->prev_poc_msb, plsb = sl->idr ? 0 : d->prev_poc_lsb;
        int msb = pmsb;
        if (lsb < plsb && plsb - lsb >= max_lsb / 2) msb = pmsb + max_lsb;
        else if (lsb > plsb && lsb - plsb > max_lsb / 2) msb = pmsb - max_lsb;
        d->cur_poc_msb = msb;
        *top = msb + lsb;
        *bottom = field ? *top : *top + sl->delta_pic_order_cnt_bottom;
        return;
    }
    const int prev_off = d->prev_mmco5 ? 0 : d->prev_frame_num_offset;
    int off = 0;
    if (!sl->idr) off = d->prev_frame_num > sl->frame_num ? prev_off + max_fn : prev_off;
    d->cur_frame_num_offset = off;
    if (s->poc_type == 1) {
        int abs = s->num_ref_frames_in_poc_cycle ? off + sl->frame_num : 0;
        if (sl->nal_ref_idc == 0 && abs > 0) --abs;
        int expected = 0;
        if (abs > 0) {
            int delta_cycle = 0;
            for (int i = 0; i < s->num_ref_frames_in_poc_cycle; ++i)
                delta_cycle += s->offset_for_ref_frame[i];
            const int cycles = (abs - 1) / s->num_ref_frames_in_poc_cycle;
            const int in_cycle = (abs - 1) % s->num_ref_frames_in_poc_cycle;
            expected = cycles * delta_cycle;
            for (int i = 0; i <= in_cycle; ++i) expected += s->offset_for_ref_frame[i];
        }
        if (sl->nal_ref_idc == 0) expected += s->offset_for_non_ref_pic;
        if (field) {
            *top = *bottom = expected + sl->delta_pic_order_cnt[0] +
                             (sl->bottom_field_flag ? s->offset_for_top_to_bottom_field : 0);
        } else {
            *top = expected + sl->delta_pic_order_cnt[0];
            *bottom = *top + s->offset_for_top_to_bottom_field + sl->delta_pic_order_cnt[1];
        }
    } else {
        int t = 0;
        if (!sl->idr) t = 2 * (off + sl->frame_num) - (sl->nal_ref_idc == 0);
        *top = *bottom = t;
    }
}

/* 8.2.5.2: frame_num gap -> "non-existing" short-term frames */
static int fill_gap(nvdec_h264_dec *d, const h264_sps *s, int frame_num) {
    const int max_fn = 1 << s->log2_max_frame_num;
    int unused = (d->prev_ref_frame_num + 1) % max_fn;
    for (int guard = 0; unused != frame_num && guard < max_fn; ++guard) {
        sliding_window(d, s, unused, -1);
        cleanup(d);
        const int slot = free_slot(d);
        if (slot < 0) return NVDEC_H264_ENOSURF;
        nvdec_h264_frame *f = &d->fs[slot];
        memset(f, 0, sizeof(*f));
        f->used = 1;
        f->surface = -1;
        f->fields = 3;
        f->mark[0] = f->mark[1] = 1;
        f->frame_num = unused;
        f->dpb_slot = -1;
        if (d->prev_frame_num > unused) d->prev_frame_num_offset += max_fn;
        d->prev_frame_num = unused;
        d->prev_mmco5 = 0;
        d->prev_ref_frame_num = unused;
        unused = (unused + 1) % max_fn;
        while (count(d, 0) > d->dpb_frames && bump_one(d, -1)) {}
    }
    return NVDEC_H264_OK;
}

void nvdec_h264_layout_for(const h264_sps *s, nvdec_h264_layout *l) {
    memset(l, 0, sizeof(*l));
    l->width_mbs = (uint32_t)s->pic_width_in_mbs;
    l->height_mbs = (uint32_t)s->frame_height_in_mbs;
    l->width = l->width_mbs * 16;
    l->height = l->height_mbs * 16;
    l->pitch = (l->width + 63) & ~63u;                 /* whole GOBs */
    l->aligned_height = (l->height + 31) & ~31u;       /* whole blocks for luma and chroma */
    l->luma_bytes = l->pitch * l->aligned_height;
    l->surface_bytes = (l->luma_bytes + l->luma_bytes / 2 + 0xff) & ~0xffu;
    const uint32_t hmbs2 = (l->height_mbs + 1) & ~1u;
    l->coloc_frame_bytes = (hmbs2 * l->width_mbs * 64 - 63 + 0xff) & ~0xffu;
    l->coloc_bytes = l->coloc_frame_bytes * NVDEC_H264_SURFACES;
    l->mbhist_bytes = (l->width_mbs * 104 + 0xff) & ~0xffu;
    l->history_bytes = (l->width_mbs * 0x200 + 0x1100 + 0x1ff) & ~0x1ffu;
}


static void fill_setup(const nvdec_h264_dec *d, const h264_sps *s, const h264_pps *p,
                       const h264_slice *sl, const nvdec_h264_frame *cur, nvdec_h264_pic_s *o) {
    nvdec_h264_layout l;
    nvdec_h264_layout_for(s, &l);
    memset(o, 0, sizeof(*o));
    o->mbhist_buffer_size = l.mbhist_bytes;
    o->log2_max_pic_order_cnt_lsb_minus4 = s->poc_type == 0 ? s->log2_max_poc_lsb - 4 : 0;
    o->delta_pic_order_always_zero_flag = s->delta_pic_order_always_zero_flag;
    o->frame_mbs_only_flag = s->frame_mbs_only_flag;
    o->PicWidthInMbs = s->pic_width_in_mbs;
    o->FrameHeightInMbs = s->frame_height_in_mbs;
    o->tileFormat = 1;      /* KBL: desktop block linear (0 = Tegra TBL) */
    o->gob_height = 0;      /* GOB_2 */
    o->entropy_coding_mode_flag = p->entropy_coding_mode_flag;
    o->pic_order_present_flag = p->bottom_field_pic_order_in_frame_present_flag;
    o->num_ref_idx_l0_active_minus1 = p->num_ref_idx_l0_default_active - 1;
    o->num_ref_idx_l1_active_minus1 = p->num_ref_idx_l1_default_active - 1;
    o->deblocking_filter_control_present_flag = p->deblocking_filter_control_present_flag;
    o->redundant_pic_cnt_present_flag = p->redundant_pic_cnt_present_flag;
    o->transform_8x8_mode_flag = p->transform_8x8_mode_flag;
    o->pitch_luma = l.pitch;
    o->pitch_chroma = l.pitch;
    o->HistBufferSize = l.history_bytes / 256;
    o->MbaffFrameFlag = s->mb_adaptive_frame_field_flag && !sl->field_pic_flag;
    o->direct_8x8_inference_flag = s->direct_8x8_inference_flag;
    o->weighted_pred_flag = p->weighted_pred_flag;
    o->constrained_intra_pred_flag = p->constrained_intra_pred_flag;
    o->ref_pic_flag = sl->nal_ref_idc != 0;
    o->field_pic_flag = sl->field_pic_flag;
    o->bottom_field_flag = sl->field_pic_flag && sl->bottom_field_flag;
    o->second_field = d->cur_second;
    o->log2_max_frame_num_minus4 = s->log2_max_frame_num - 4;
    o->chroma_format_idc = s->chroma_format_idc;
    o->pic_order_cnt_type = s->poc_type;
    o->pic_init_qp_minus26 = p->pic_init_qp - 26;
    o->chroma_qp_index_offset = p->chroma_qp_index_offset;
    o->second_chroma_qp_index_offset = p->second_chroma_qp_index_offset;
    o->weighted_bipred_idc = p->weighted_bipred_idc;
    o->CurrPicIdx = cur->surface;
    o->CurrColIdx = cur->surface;
    o->frame_num = sl->frame_num;
    o->output_memory_layout = 0;   /* NV12 */
    /* a field not decoded (yet) reads 0 */
    o->CurrFieldOrderCnt[0] = cur->fields & 1 ? cur->poc_top : 0;
    o->CurrFieldOrderCnt[1] = cur->fields & 2 ? cur->poc_bottom : 0;
    /*
     * Every frame store with a reference field, incl. the current frame's first
     * field while its second field is decoded
     */
    for (int i = 0; i < NVDEC_H264_SURFACES; ++i) {
        const nvdec_h264_frame *f = &d->fs[i];
        if (!f->used || !is_ref(f) || f->dpb_slot < 0) continue;
        nvdec_dpb_entry_s *e = &o->dpb[f->dpb_slot];
        const int surf = f->surface >= 0 ? f->surface : cur->surface;
        e->index = surf;
        e->col_idx = surf;
        e->state = (f->mark[0] ? 1u : 0u) | (f->mark[1] ? 2u : 0u);
        e->is_long_term = has_mark(f, 2);
        e->not_existing = f->surface < 0;
        e->is_field = f->field_coded;
        e->top_field_marking = f->mark[0];      /* 1 short, 2 long */
        e->bottom_field_marking = f->mark[1];
        e->output_memory_layout = 0;
        e->FieldOrderCnt[0] = f->fields & 1 ? (unsigned)f->poc_top : 0;
        e->FieldOrderCnt[1] = f->fields & 2 ? (unsigned)f->poc_bottom : 0;
        e->FrameIdx = has_mark(f, 2) ? f->long_term_frame_idx : f->frame_num;
    }
    memcpy(o->WeightScale, p->scaling4, sizeof(o->WeightScale));
    memcpy(o->WeightScale8x8[0], p->scaling8[0], 64);   /* intra Y */
    memcpy(o->WeightScale8x8[1], p->scaling8[1], 64);   /* inter Y */
    o->lossless_ipred8x8_filter_enable = 1;
    o->qpprime_y_zero_transform_bypass_flag = s->qpprime_y_zero_transform_bypass_flag;
}

int nvdec_h264_begin(nvdec_h264_dec *d, const h264_slice *sl, nvdec_h264_pic_s *setup,
                     int *surface) {
    if (sl->pps_id >= H264_MAX_PPS || !d->pps[sl->pps_id].valid) return NVDEC_H264_EPARAM;
    const h264_pps *p = &d->pps[sl->pps_id];
    if (!d->sps[p->sps_id].valid) return NVDEC_H264_EPARAM;
    const h264_sps *s = &d->sps[p->sps_id];
    if (sl->field_pic_flag && s->frame_mbs_only_flag) return NVDEC_H264_EFIELD;
    if (s->chroma_format_idc != 1 || s->bit_depth_luma != 8 || s->bit_depth_chroma != 8)
        return NVDEC_H264_EFORMAT;
    if (d->sps_id != p->sps_id) activate(d, p->sps_id);
    d->n_out = 0;
    const int field = sl->field_pic_flag;
    const int parity = field ? (sl->bottom_field_flag ? 2 : 1) : 3;
    /*
     * 7.4.1.2.4 / C.4.5: the second field of a pair goes into the frame store
     * of the first field that was decoded just before it
     */
    int second = 0;
    if (field && d->open_field >= 0) {
        const nvdec_h264_frame *o = &d->fs[d->open_field];
        second = o->used && o->fields == (3 ^ parity) && !sl->idr &&
                 o->frame_num == sl->frame_num && is_ref(o) == (sl->nal_ref_idc != 0);
    }
    const int first_slot = d->open_field;
    d->open_field = -1;   /* an unpaired first field stays a single field */
    const int max_fn = 1 << s->log2_max_frame_num;
    if (second) {
        /* same frame_num: no gap, no IDR */
    } else if (sl->idr) {
        unmark_all(d);
        if (sl->no_output_of_prior_pics_flag)
            for (int i = 0; i < NVDEC_H264_SURFACES; ++i) d->fs[i].output = 0;
        bump_all(d, -1);
        cleanup(d);
        d->prev_ref_frame_num = 0;
        d->prev_frame_num = 0;
        d->prev_frame_num_offset = 0;
        d->prev_mmco5 = 0;
    } else if (sl->frame_num != d->prev_ref_frame_num &&
               sl->frame_num != (d->prev_ref_frame_num + 1) % max_fn) {
        const int r = fill_gap(d, s, sl->frame_num);
        if (r) return r;
    }
    int top = 0, bottom = 0;
    compute_poc(d, s, sl, &top, &bottom);
    nvdec_h264_frame *f;
    int slot;
    if (second) {
        slot = first_slot;
        f = &d->fs[slot];
        if (parity == 1) f->poc_top = top; else f->poc_bottom = bottom;
        f->fields = 3;
    } else {
        slot = free_slot(d);
        const int surf = free_surface(d);
        if (slot < 0 || surf < 0) return NVDEC_H264_ENOSURF;
        f = &d->fs[slot];
        memset(f, 0, sizeof(*f));
        f->used = 1;
        f->surface = surf;
        f->frame_num = sl->frame_num;
        f->fields = parity;
        f->field_coded = field;
        f->poc_top = parity & 1 ? top : 0;
        f->poc_bottom = parity & 2 ? bottom : 0;
        f->output = 1;
        f->dpb_slot = -1;
        if (field) d->open_field = slot;   /* until the next picture's begin */
    }
    d->cur = slot;
    d->cur_second = second;
    d->cur_slice = *sl;
    /* stable NVDEC dpb[] slots for the references */
    for (int i = 0; i < NVDEC_H264_SURFACES; ++i) {
        nvdec_h264_frame *r = &d->fs[i];
        if (!r->used || !is_ref(r) || r->dpb_slot >= 0) continue;
        for (int k = 0; k < 16; ++k) {
            int taken = 0;
            for (int j = 0; j < NVDEC_H264_SURFACES && !taken; ++j)
                taken = d->fs[j].used && is_ref(&d->fs[j]) && d->fs[j].dpb_slot == k;
            if (!taken) { r->dpb_slot = k; break; }
        }
    }
    fill_setup(d, s, p, sl, f, setup);
    *surface = f->surface;
    return NVDEC_H264_OK;
}

/*
 * 8.2.4.1 / 8.2.5.4: find the short-term (long_term = 0, `num` = picNumX) or
 * long-term (long_term = 1, `num` = LongTermPicNum) reference an MMCO points
 * to. Frames: both fields must have the marking, *field = -1. Field decoding:
 * one field, PicNum = 2 * FrameNumWrap + 1 for the current parity and 2 *
 * FrameNumWrap for the other (same for LongTermPicNum).
 */
static int find_pic(const nvdec_h264_dec *d, const h264_slice *sl, int long_term, int num,
                    int *field) {
    const h264_sps *s = &d->sps[d->sps_id];
    const int max_fn = 1 << s->log2_max_frame_num;
    const int want = long_term ? 2 : 1;
    const int par = sl->bottom_field_flag ? 1 : 0;
    for (int i = 0; i < NVDEC_H264_SURFACES; ++i) {
        const nvdec_h264_frame *r = &d->fs[i];
        if (!r->used) continue;
        const int base = long_term ? r->long_term_frame_idx
                                   : frame_num_wrap(r->frame_num, sl->frame_num, max_fn);
        if (!sl->field_pic_flag) {
            if (i != d->cur && r->mark[0] == want && r->mark[1] == want && base == num) {
                *field = -1;
                return i;
            }
            continue;
        }
        for (int k = 0; k < 2; ++k)
            if (r->mark[k] == want && 2 * base + (k == par) == num) {
                *field = k;
                return i;
            }
    }
    return -1;
}

static void set_mark(nvdec_h264_frame *f, int field, int m) {
    if (field < 0) f->mark[0] = f->mark[1] = m;
    else f->mark[field] = m;
}

/*
 * LongTermFrameIdx `idx` moves to frame store `keep`: anyone else holding that
 * index loses its long-term fields (8.2.5.4.3 / 8.2.5.4.6)
 */
static void free_long_idx(nvdec_h264_dec *d, int idx, int keep) {
    for (int i = 0; i < NVDEC_H264_SURFACES; ++i) {
        nvdec_h264_frame *r = &d->fs[i];
        if (i != keep && r->used && has_mark(r, 2) && r->long_term_frame_idx == idx) unmark(r, 2);
    }
}

void nvdec_h264_end(nvdec_h264_dec *d) {
    if (d->cur < 0) return;
    const h264_slice *sl = &d->cur_slice;
    const h264_sps *s = &d->sps[d->sps_id];
    nvdec_h264_frame *f = &d->fs[d->cur];
    const int field = sl->field_pic_flag;
    const int par = field ? (sl->bottom_field_flag ? 1 : 0) : -1;   /* current field */
    int mmco5 = 0;
    if (sl->nal_ref_idc) {
        if (sl->idr) {
            if (sl->long_term_reference_flag) {
                set_mark(f, par, 2);
                f->long_term_frame_idx = 0;
                d->max_long_term_frame_idx = 0;
            } else {
                set_mark(f, par, 1);
                d->max_long_term_frame_idx = -1;
            }
        } else {
            int made_long = 0;
            if (sl->adaptive_ref_pic_marking_mode_flag) {
                const int cur_pic_num = field ? 2 * sl->frame_num + 1 : sl->frame_num;
                for (int k = 0; k < sl->n_mmco; ++k) {
                    const h264_mmco *m = &sl->mmco[k];
                    const int pic_num_x = cur_pic_num - (m->diff_pic_nums_minus1 + 1);
                    int fld = -1, t = -1;
                    switch (m->op) {
                    case 1:
                        t = find_pic(d, sl, 0, pic_num_x, &fld);
                        if (t >= 0) set_mark(&d->fs[t], fld, 0);
                        break;
                    case 2:
                        t = find_pic(d, sl, 1, m->long_term_pic_num, &fld);
                        if (t >= 0) set_mark(&d->fs[t], fld, 0);
                        break;
                    case 3:
                        t = find_pic(d, sl, 0, pic_num_x, &fld);
                        if (t >= 0) {
                            free_long_idx(d, m->long_term_frame_idx, t);
                            nvdec_h264_frame *r = &d->fs[t];
                            /* a field joining a long-term field of another index */
                            if (has_mark(r, 2) && r->long_term_frame_idx != m->long_term_frame_idx)
                                unmark(r, 2);
                            set_mark(r, fld, 2);
                            r->long_term_frame_idx = m->long_term_frame_idx;
                        }
                        break;
                    case 4:
                        for (int i = 0; i < NVDEC_H264_SURFACES; ++i) {
                            nvdec_h264_frame *r = &d->fs[i];
                            if (r->used && has_mark(r, 2) &&
                                r->long_term_frame_idx > m->max_long_term_frame_idx_plus1 - 1)
                                unmark(r, 2);
                        }
                        d->max_long_term_frame_idx = m->max_long_term_frame_idx_plus1 - 1;
                        break;
                    case 5:
                        unmark_all(d);
                        mmco5 = 1;
                        d->max_long_term_frame_idx = -1;
                        break;
                    case 6:
                        free_long_idx(d, m->long_term_frame_idx, d->cur);
                        if (has_mark(f, 2) && f->long_term_frame_idx != m->long_term_frame_idx)
                            unmark(f, 2);
                        set_mark(f, par, 2);
                        f->long_term_frame_idx = m->long_term_frame_idx;
                        made_long = 1;
                        break;
                    }
                }
            } else if (!(d->cur_second && f->mark[1 - par] == 1)) {
                /*
                 * 8.2.5.3 is skipped for the second field of a pair whose first
                 * field is short-term
                 */
                sliding_window(d, s, sl->frame_num, d->cur);
            }
            if (!made_long) set_mark(f, par, 1);
        }
        if (mmco5) {
            if (field) {
                if (par == 0) f->poc_top = 0; else f->poc_bottom = 0;
            } else {
                const int t = f->poc_top < f->poc_bottom ? f->poc_top : f->poc_bottom;
                f->poc_top -= t;
                f->poc_bottom -= t;
            }
            f->frame_num = 0;
            bump_all(d, d->cur);   /* C.4.5.3: all earlier pictures go out first */
        }
        d->prev_ref_frame_num = mmco5 ? 0 : sl->frame_num;
        d->prev_poc_msb = mmco5 ? 0 : d->cur_poc_msb;
        d->prev_poc_lsb = !mmco5 ? sl->pic_order_cnt_lsb : par == 1 ? 0 : f->poc_top;
    }
    d->prev_frame_num_offset = mmco5 ? 0 : d->cur_frame_num_offset;
    d->prev_frame_num = mmco5 ? 0 : sl->frame_num;
    d->prev_mmco5 = mmco5;
    d->cur = -1;
    cleanup(d);
    while ((count(d, 1) > d->reorder || count(d, 0) > d->dpb_frames) && bump_one(d, -1)) {}
    ++d->pictures;
}

void nvdec_h264_flush(nvdec_h264_dec *d) {
    d->n_out = 0;
    d->open_field = -1;
    bump_all(d, -1);
    cleanup(d);
}

/* ---------------------------------------------------------------- stream */

static const uint8_t kEndSequence[16] = {0x00, 0x00, 0x01, 0x0b, 0x00, 0x00, 0x00, 0x00,
                                         0x00, 0x00, 0x01, 0x0b, 0x00, 0x00, 0x00, 0x00};

void nvdec_h264_stream_begin(nvdec_h264_stream *s, uint8_t *in, uint32_t in_bytes) {
    s->in = in;
    s->in_bytes = in_bytes;
    s->len = 0;
    s->slices = 0;
}

int nvdec_h264_stream_slice(nvdec_h264_stream *s, const h264_nal *nal) {
    if (s->slices >= NVDEC_IN_MAX_SLICES ||
        (uint64_t)NVDEC_IN_BITSTREAM + s->len + 3 + nal->size + sizeof(kEndSequence) > s->in_bytes)
        return NVDEC_H264_ESPACE;
    uint32_t *offs = (uint32_t *)(s->in + NVDEC_IN_SLICES);
    offs[s->slices++] = s->len;
    uint8_t *bs = s->in + NVDEC_IN_BITSTREAM + s->len;
    bs[0] = 0;
    bs[1] = 0;
    bs[2] = 1;
    memcpy(bs + 3, nal->data, nal->size);
    s->len += 3 + (uint32_t)nal->size;
    return NVDEC_H264_OK;
}

int nvdec_h264_stream_end(nvdec_h264_stream *s, nvdec_h264_pic_s *setup) {
    if (!s->slices) return NVDEC_H264_EPARAM;
    ((uint32_t *)(s->in + NVDEC_IN_SLICES))[s->slices] = s->len;
    memcpy(s->in + NVDEC_IN_BITSTREAM + s->len, kEndSequence, sizeof(kEndSequence));
    setup->stream_len = s->len + (uint32_t)sizeof(kEndSequence);
    setup->slice_count = s->slices;
    memcpy(s->in + NVDEC_IN_SETUP, setup, sizeof(*setup));
    memset(s->in + NVDEC_IN_STATUS, 0, sizeof(nvdec_status_s));
    return NVDEC_H264_OK;
}

/* --------------------------------------------------------------- methods */

static uint32_t hdr(uint32_t subch, uint32_t mthd, uint32_t n) {
    return (1u << 29) | (n << 16) | (subch << 13) | (mthd >> 2);
}

static int addr_ok(uint64_t va) { return !(va & 0xff) && va < (1ull << 40); }

uint32_t nvdec_h264_push(const nvdec_h264_addrs *a, const nvdec_h264_layout *l,
                         uint32_t picture_index, uint32_t fence, uint32_t *out, uint32_t max) {
    const uint32_t need = 2 + 2 * 10 + 2 * 18 + 8;
    if (max < need || a->subch > 7 || !addr_ok(a->in_va) || !addr_ok(a->coloc_va) ||
        !addr_ok(a->mbhist_va) || !addr_ok(a->history_va) || (l->luma_bytes & 0xff))
        return 0;
    for (int i = 0; i < NVDEC_H264_SURFACES; ++i)
        if (!addr_ok(a->surface_va[i]) || !addr_ok(a->surface_va[i] + l->luma_bytes)) return 0;
    uint32_t n = 0;
    const uint32_t sc = a->subch;
#define M(mthd, v) do { out[n++] = hdr(sc, (mthd), 1); out[n++] = (v); } while (0)
    M(0x000, a->obj_class);                                  /* SET_OBJECT */
    M(0x200, 3);                                             /* SET_APPLICATION_ID H264 */
    M(0x400, 3 | (1u << 4) | (1u << 6));                     /* CONTROL: H264, GPTIMER, ERR_CONCEAL */
    M(0x40c, picture_index);                                 /* SET_PICTURE_INDEX */
    M(0x404, (uint32_t)((a->in_va + NVDEC_IN_SETUP) >> 8));  /* DRV_PIC_SETUP */
    M(0x408, (uint32_t)((a->in_va + NVDEC_IN_BITSTREAM) >> 8));
    M(0x410, (uint32_t)((a->in_va + NVDEC_IN_SLICES) >> 8));
    M(0x424, (uint32_t)((a->in_va + NVDEC_IN_STATUS) >> 8));
    M(0x414, (uint32_t)(a->coloc_va >> 8));                  /* COLOC_DATA */
    M(0x500, (uint32_t)(a->mbhist_va >> 8));                 /* H264 MBHIST */
    M(0x418, (uint32_t)(a->history_va >> 8));                /* HISTORY */
    out[n++] = hdr(sc, 0x430, NVDEC_H264_SURFACES);          /* PICTURE_LUMA_OFFSET0..16 */
    for (int i = 0; i < NVDEC_H264_SURFACES; ++i) out[n++] = (uint32_t)(a->surface_va[i] >> 8);
    out[n++] = hdr(sc, 0x474, NVDEC_H264_SURFACES);          /* PICTURE_CHROMA_OFFSET0..16 */
    for (int i = 0; i < NVDEC_H264_SURFACES; ++i)
        out[n++] = (uint32_t)((a->surface_va[i] + l->luma_bytes) >> 8);
    M(0x300, 1u << 8);                                       /* EXECUTE, AWAKEN */
    const uint64_t sem = a->in_va + NVDEC_IN_SEM;
    out[n++] = hdr(sc, 0x240, 3);                            /* SEMAPHORE_A/B/C */
    out[n++] = (uint32_t)(sem >> 32) & 0xff;
    out[n++] = (uint32_t)sem;
    out[n++] = fence;
    M(0x304, 0);                                             /* SEMAPHORE_D release */
#undef M
    return n;
}

/* ------------------------------------------------------------ block linear */

static uint32_t bl_offset(uint32_t x, uint32_t y, uint32_t pitch) {
    const uint32_t block = (y >> 4) * (pitch >> 6) + (x >> 6);
    /*
     * GOB (64 B x 8 rows, 512 B) the way NVDEC writes it with KBL, measured on
     * AD103 by decoding base_ip: two 256 B halves split on x bit 5, each is two
     * 128 B row quads, each of those two 64 B columns of four 16 B rows.
     */
    return block * 1024 + ((y >> 3) & 1) * 512 + ((x & 63) >> 5) * 256 + ((y & 7) >> 2) * 128 +
           ((x & 31) >> 4) * 64 + (y & 3) * 16 + (x & 15);
}

void nvdec_detile(const uint8_t *bl, uint32_t bl_pitch, uint8_t *dst, uint32_t dst_pitch,
                  uint32_t bytes_per_row, uint32_t rows) {
    for (uint32_t y = 0; y < rows; ++y)
        for (uint32_t x = 0; x < bytes_per_row; x += 16) {
            const uint32_t n = bytes_per_row - x < 16 ? bytes_per_row - x : 16;
            memcpy(dst + (size_t)y * dst_pitch + x, bl + bl_offset(x, y, bl_pitch), n);
        }
}

void nvdec_tile(const uint8_t *src, uint32_t src_pitch, uint8_t *bl, uint32_t bl_pitch,
                uint32_t bytes_per_row, uint32_t rows) {
    for (uint32_t y = 0; y < rows; ++y)
        for (uint32_t x = 0; x < bytes_per_row; x += 16) {
            const uint32_t n = bytes_per_row - x < 16 ? bytes_per_row - x : 16;
            memcpy(bl + bl_offset(x, y, bl_pitch), src + (size_t)y * src_pitch + x, n);
        }
}

int nvdec_surface_nv12_md5(const uint8_t *surface, uint32_t pitch, uint32_t luma_bytes,
                           uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, char hex[33]) {
    const uint32_t row_bytes = (x0 + w + 15) & ~15u;
    uint8_t *row = malloc(row_bytes);
    if (!row) return -1;
    nvdec_md5 m;
    nvdec_md5_init(&m);
    x0 &= ~1u;   /* keep UV pairs */
    for (int plane = 0; plane < 2; ++plane) {
        const uint8_t *bl = surface + (plane ? luma_bytes : 0);
        const uint32_t top = plane ? y0 / 2 : y0, rows = plane ? h / 2 : h;
        for (uint32_t y = 0; y < rows; ++y) {
            const uint32_t yy = top + y;
            for (uint32_t x = 0; x < row_bytes; x += 16) {   /* one block-linear row */
                memcpy(row + x, bl + bl_offset(x, yy, pitch), 16);
            }
            nvdec_md5_update(&m, row + x0, w);
        }
    }
    free(row);
    nvdec_md5_hex(&m, hex);
    return 0;
}

int nvdec_h264_surface_md5(const uint8_t *surface, const nvdec_h264_layout *l,
                           const h264_sps *s, char hex[33]) {
    return nvdec_surface_nv12_md5(surface, l->pitch, l->luma_bytes, (uint32_t)s->crop_left,
                                  (uint32_t)s->crop_top,
                                  l->width - (uint32_t)(s->crop_left + s->crop_right),
                                  l->height - (uint32_t)(s->crop_top + s->crop_bottom), hex);
}
