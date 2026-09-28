#include "nvenc_h264.h"
#include <string.h>

_Static_assert(sizeof(nvenc_h264_drv_pic_setup_s) == 512, "picture ABI");
_Static_assert(sizeof(nvenc_h264_slice_control_s) == 128, "slice ABI");
_Static_assert(sizeof(nvenc_h264_me_control_s) == 192, "ME ABI");
_Static_assert(sizeof(nvenc_h264_md_control_s) == 128, "MD ABI");
_Static_assert(sizeof(nvenc_h264_quant_control_s) == 192, "quant ABI");
_Static_assert(sizeof(nvenc_pic_stat_s) == 128, "status ABI");
_Static_assert(sizeof(nvenc_h264_setup) <= NVENC_SETUP_BYTES, "setup allocation");

static uint32_t align_up(uint32_t n, uint32_t a) { return (n + a - 1) & ~(a - 1); }

int nvenc_h264_layout_init(const nvenc_h264_config *c, nvenc_h264_layout *l) {
    if (!c || !l || c->width < 16 || c->height < 16 || c->width > 4096 ||
        c->height > 4096 || ((c->width | c->height) & 1) || c->qp > 51 ||
        !c->fps || c->fps > 60 || c->ipcm > 1 || c->block_height > 5) return -1;
    memset(l, 0, sizeof(*l));
    l->width = c->width; l->height = c->height;
    l->padded_width = align_up(c->width, 16);
    l->padded_height = align_up(c->height, 16);
    l->pitch = align_up(c->width, 64);
    l->block_height = c->block_height;
    uint32_t rows = 8u << c->block_height;
    l->luma_bytes = l->pitch * align_up(l->padded_height, rows);
    l->surface_bytes = l->luma_bytes + l->pitch * align_up(l->padded_height / 2, rows);
    uint32_t mbs = (l->padded_width / 16) * (l->padded_height / 16);
    if (mbs > 36864 || (uint64_t)mbs*c->fps > 2073600) return -1; /* Level 5.2 */
    /* IPCM is 384 bytes/MB. Leave room for headers and emulation prevention. */
    l->bitstream_bytes = align_up(mbs * 768 + 4096, 4096);
    l->history_bytes = align_up((l->padded_width / 16 + 16) * HIST_BLOCK_SIZE, 4096);
    l->coloc_bytes = align_up(mbs * sizeof(nvenc_h264_coloc_mb_s), 4096);
    return 0;
}

static void surface(nvenc_h264_surface_cfg_s *s, const nvenc_h264_layout *l, int tiled) {
    s->frame_width_minus1 = l->width - 1;
    s->frame_height_minus1 = l->height - 1;
    s->sfc_pitch = s->sfc_pitch_chroma = l->pitch;
    s->chroma_top_frm_offset = l->luma_bytes >> 8;
    s->block_height = l->block_height;
    s->tiled_16x16 = tiled;
}

int nvenc_h264_setup_init(const nvenc_h264_config *c, const nvenc_h264_layout *l,
                         uint32_t picture, nvenc_h264_setup *s) {
    nvenc_h264_layout expected;
    if (!s || !l || nvenc_h264_layout_init(c, &expected) ||
        memcmp(l, &expected, sizeof(*l))) return -1;
    memset(s, 0, sizeof(*s));
    s->pic.magic = NV_NVENC_DRV_MAGIC_VALUE;
    surface(&s->pic.input_cfg, l, 0);
    surface(&s->pic.refpic_cfg, l, 1);
    surface(&s->pic.outputpic_cfg, l, 1);
    s->pic.sps_data.profile_idc = 66;
    s->pic.sps_data.level_idc = 52;
    s->pic.sps_data.chroma_format_idc = 1;
    s->pic.sps_data.pic_order_cnt_type = 2;
    s->pic.sps_data.frame_mbs_only = 1;
    s->pic.pps_data.pic_init_qp_minus26 = (int)c->qp - 26;
    s->pic.pps_data.deblocking_filter_control_present_flag = 1;
    for (unsigned i = 0; i < 3; ++i) {
        s->pic.rate_control.QP[i] = c->qp;
        s->pic.rate_control.minQP[i] = c->qp;
        s->pic.rate_control.maxQP[i] = c->qp;
        s->pic.rate_control.rhopbi[i] = 256;
    }
    s->pic.rate_control.framerate = c->fps;
    s->pic.rate_control.gop_length = 1;
    s->pic.rate_control.session_max_qp = 51;
    nvenc_h264_pic_control_s *p = &s->pic.pic_control;
    memset(p->l0, -1, sizeof(p->l0));
    memset(p->l1, -1, sizeof(p->l1));
    p->pic_type = 3; /* IDR: no dependency on a preceding frame. */
    p->ref_pic_flag = 1;
    p->slice_mode = 1;
    p->codec = 3;
    p->e4byteStartCode = 1;
    p->ipcm_rewind_enable = 1;
    p->idr_pic_id = picture & 1;
    p->hist_buf_size = l->history_bytes;
    p->bitstream_buf_size = l->bitstream_bytes;
    p->max_byte_count_before_resid_zero = l->bitstream_bytes;
    p->slice_control_offset = offsetof(nvenc_h264_setup, slice);
    p->me_control_offset = offsetof(nvenc_h264_setup, me);
    p->md_control_offset = offsetof(nvenc_h264_setup, md);
    p->q_control_offset = offsetof(nvenc_h264_setup, quant);
    p->wp_control_offset = offsetof(nvenc_h264_setup, weights);
    p->slice_stat_offset = sizeof(nvenc_pic_stat_s);
    p->mpec_stat_offset = 256;
    p->aq_stat_offset = 512;
    p->stats_fifo_offset = 768;
    p->slice_encoding_row_num = l->padded_height / 16;
    p->strips_in_frame = 1;
    s->pic.gpTimer_timeout_val = 0xffffffffu;
    s->slice.num_mb = (l->padded_width / 16) * (l->padded_height / 16);
    s->slice.qp_avr = c->qp;
    s->slice.qp_slice_min = s->slice.qp_slice_max = c->qp;
    s->slice.force_intra = 1;
    s->slice.disable_deblocking_filter_idc = 1;
    s->me.hint_type1 = 1; s->me.hint_type2 = 2;
    s->me.hint_type3 = 3; s->me.hint_type4 = 4;
    s->md.intra_luma4x4_mode_enable = 0x1ff;
    s->md.intra_luma16x16_mode_enable = 0xf;
    s->md.intra_chroma_mode_enable = 0xf;
    s->md.early_intra_mode_control = 3;
    s->md.ip_search_mode = 5; /* 4x4 and 16x16; baseline has no 8x8 transform. */
    s->md.intra_most_prob_force_on = 1;
    s->md.force_ipcm = c->ipcm;
    return 0;
}

/* GPU block-linear GOB swizzle, with 2^block_height GOBs per block. */
static size_t bl_offset(uint32_t x, uint32_t y, const nvenc_h264_layout *l) {
    uint32_t bh = 1u << l->block_height;
    return (size_t)(y / (8 * bh)) * l->pitch * 8 * bh +
        (size_t)(x / 64) * 512 * bh + (size_t)((y / 8) % bh) * 512 +
        ((x & 63) / 32) * 256 + ((y & 7) / 4) * 128 +
        ((x & 31) / 16) * 64 + (y & 3) * 16 + (x & 15);
}

int nvenc_h264_pack(const nvenc_h264_layout *l, const uint8_t *raw, size_t raw_size,
                    uint8_t *out, size_t out_size) {
    if (!l || !raw || !out || !l->width || !l->height || l->block_height > 5 ||
        l->pitch < l->padded_width || (l->pitch & 63) ||
        raw_size != (size_t)l->width * l->height * 3 / 2 ||
        out_size < l->surface_bytes) return -1;
    memset(out, 0, l->surface_bytes);
    for (unsigned plane = 0; plane < 2; ++plane) {
        const uint8_t *src = raw + (plane ? (size_t)l->width * l->height : 0);
        uint8_t *dst = out + (plane ? l->luma_bytes : 0);
        uint32_t h = l->height >> plane, ph = l->padded_height >> plane;
        for (uint32_t y = 0; y < ph; ++y) {
            uint32_t sy = y < h ? y : h - 1;
            for (uint32_t x = 0; x < l->padded_width; ++x) {
                uint32_t sx = x < l->width ? x : l->width - (plane ? 2 : 1) + (plane ? x % 2 : 0);
                dst[bl_offset(x, y, l)] = src[(size_t)sy * l->width + sx];
            }
        }
    }
    return 0;
}

/* Only parameter sets are assembled here. Slice data always comes from NVENC. */
typedef struct { uint8_t b[128]; unsigned bits; } bit_writer;
static void bit(bit_writer *w, unsigned v) {
    w->b[w->bits / 8] |= (v & 1) << (7 - w->bits % 8); ++w->bits;
}
static void bits(bit_writer *w, uint32_t v, unsigned n) {
    while (n) bit(w, v >> --n);
}
static void ue(bit_writer *w, uint32_t v) {
    uint32_t n = v + 1, t = n; unsigned width = 0;
    while (t >>= 1) ++width;
    for (unsigned i = 0; i < width; ++i) bit(w, 0);
    bits(w, n, width + 1);
}
static void se(bit_writer *w, int v) { ue(w, v <= 0 ? (uint32_t)(-2*v) : (uint32_t)(2*v-1)); }
static size_t nal(bit_writer *w, uint8_t type, uint8_t *out, size_t cap) {
    bit(w, 1); while (w->bits % 8) bit(w, 0);
    uint8_t temp[256] = {0,0,0,1}; size_t n = 5; temp[4] = type;
    unsigned zeros = 0;
    for (unsigned i = 0; i < w->bits / 8; ++i) {
        uint8_t b = w->b[i];
        if (zeros >= 2 && b <= 3) { temp[n++] = 3; zeros = 0; }
        temp[n++] = b; zeros = b ? 0 : zeros + 1;
    }
    if (cap < n) return 0;
    memcpy(out, temp, n); return n;
}

size_t nvenc_h264_headers(const nvenc_h264_config *c, uint8_t *out, size_t cap) {
    nvenc_h264_layout l;
    if (!out || nvenc_h264_layout_init(c, &l)) return 0;
    bit_writer s = {{0},0}, p = {{0},0};
    bits(&s, 66, 8); bits(&s, 0xc0, 8); bits(&s, 52, 8); ue(&s, 0);
    ue(&s, 0); ue(&s, 2); ue(&s, 1); bit(&s, 0);
    ue(&s, l.padded_width / 16 - 1); ue(&s, l.padded_height / 16 - 1);
    bit(&s, 1); bit(&s, 1);
    unsigned crop = l.padded_width != l.width || l.padded_height != l.height;
    bit(&s, crop);
    if (crop) { ue(&s, 0); ue(&s, (l.padded_width-l.width)/2); ue(&s, 0); ue(&s, (l.padded_height-l.height)/2); }
    bit(&s, 1); /* VUI with fixed timing. */
    bit(&s, 0); bit(&s, 0); bit(&s, 0); bit(&s, 0);
    bit(&s, 1); bits(&s, 1, 32); bits(&s, 2*c->fps, 32); bit(&s, 1);
    bit(&s, 0); bit(&s, 0); bit(&s, 0); bit(&s, 0);
    ue(&p, 0); ue(&p, 0); bit(&p, 0); bit(&p, 0); ue(&p, 0);
    ue(&p, 0); ue(&p, 0); bit(&p, 0); bits(&p, 0, 2);
    se(&p, (int)c->qp - 26); se(&p, 0); se(&p, 0);
    bit(&p, 1); bit(&p, 0); bit(&p, 0);
    uint8_t temp[512]; size_t n = nal(&s, 0x67, temp, sizeof(temp));
    n += nal(&p, 0x68, temp+n, sizeof(temp)-n);
    if (cap < n) return 0;
    memcpy(out, temp, n); return n;
}

static int addr_ok(uint64_t a) { return a && !(a & 255) && a < (1ull << 40); }
size_t nvenc_h264_push(const nvenc_h264_addresses *a, const nvenc_h264_layout *l,
                       uint32_t picture, uint32_t fence, uint32_t *out, size_t cap) {
    if (!a || !l || !out || cap < 64 || a->subchannel > 7 || !fence) return 0;
    const uint64_t addresses[] = {a->setup,a->status,a->bitstream,a->input,a->recon,
                                 a->history,a->coloc,a->scratch,
                                 a->status+NVENC_SEM_OFFSET,a->input+l->luma_bytes,
                                 a->recon+l->luma_bytes};
    for (size_t i=0; i<sizeof(addresses)/sizeof(addresses[0]); ++i)
        if (!addr_ok(addresses[i])) return 0;
    size_t n = 0;
#define M(m,v) do { out[n++]=0x20000000u | (1u<<16) | (a->subchannel<<13) | ((m)>>2); out[n++]=(uint32_t)(v); } while(0)
    M(0x000,NVENC_CLASS); M(0x200,1); /* Application H.264. */
    M(0x204,0xffffffffu);
    M(0x700,3 | (1u<<11) | (1u<<12) | (1u<<14)); /* constant QP, slice stats, timer */
    M(0x704,picture);
    M(0x710,a->setup>>8); M(0x718,a->status>>8); M(0x71c,a->bitstream>>8);
    M(0x720,a->history>>8); M(0x724,a->scratch>>8);
    M(0x72c,a->coloc>>8); M(0x730,a->recon>>8);
    M(0x734,a->input>>8); M(0x740,a->input>>8); M(0x744,a->input>>8);
    M(0x74c,a->recon>>8);
    M(0x738,0); M(0x73c,0); M(0x714,0); M(0x728,0); M(0x748,0);
    M(0x300,1u<<8);
    uint64_t sem = a->status + NVENC_SEM_OFFSET;
    M(0x240,sem>>32); M(0x244,sem); M(0x248,fence); M(0x304,0);
#undef M
    return n;
}

int nvenc_h264_result(const void *status, size_t status_bytes, uint32_t picture,
                      const uint8_t *bs, size_t capacity, size_t *offset, size_t *bytes) {
    if (!status || !bs || !offset || !bytes ||
        status_bytes < sizeof(nvenc_pic_stat_s)+sizeof(nvenc_slice_stat_s)) return -1;
    nvenc_pic_stat_s p; nvenc_slice_stat_s s;
    memcpy(&p, status, sizeof(p)); memcpy(&s, (const uint8_t *)status+sizeof(p), sizeof(s));
    if (p.picture_index != picture || p.error_status || p.ucode_error_status ||
        p.pic_type != 3 || p.num_slices != 1 || !p.total_bit_count ||
        !s.slice_size || s.slice_offset > capacity || s.slice_size > capacity-s.slice_offset ||
        (uint64_t)p.total_bit_count > (uint64_t)capacity*8) return -1;
    const uint8_t *nal_start = bs+s.slice_offset; size_t prefix = 0;
    if (s.slice_size >= 4 && !memcmp(nal_start,"\0\0\1",3)) prefix=3;
    if (s.slice_size >= 5 && !memcmp(nal_start,"\0\0\0\1",4)) prefix=4;
    if (!prefix || (nal_start[prefix]&0x9f) != 5 || !(nal_start[prefix]&0x60)) return -1;
    *offset=s.slice_offset; *bytes=s.slice_size; return 0;
}
