/*
 * Host test for the NVDEC driver's H.264 front end against x264 vectors
 * (tools/nvdec/gen_vectors.py): picture split, POC, reference marking and
 * output order (has to match the encoder's display order), NVDEC picture setup
 * fields, bitstream/slice table, method stream and block-linear tiling.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../nvdec_h264.h"
#include "../../nvdec_md5.h"

static uint8_t *load(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    rewind(f);
    uint8_t *b = malloc(*len + 1);
    if (fread(b, 1, *len, f) != *len) exit(1);
    fclose(f);
    b[*len] = 0;
    return b;
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); ++fails; } } while (0)

static int surf_owner[NVDEC_H264_SURFACES];   /* decode index held by each surface */
static int out_order[512], n_out;

static void drain(nvdec_h264_dec *d) {
    for (int i = 0; i < d->n_out; ++i) out_order[n_out++] = surf_owner[d->out_surface[i]];
    d->n_out = 0;
}

static void check_setup(const nvdec_h264_dec *d, const nvdec_h264_pic_s *o, int surf,
                        const h264_sps *s) {
    CHECK(o->CurrPicIdx == (unsigned)surf && o->CurrColIdx == (unsigned)surf, "cur idx");
    CHECK(o->PicWidthInMbs == s->pic_width_in_mbs && o->FrameHeightInMbs == s->frame_height_in_mbs,
          "size");
    int refs = 0;
    for (int k = 0; k < 16; ++k) {
        const nvdec_dpb_entry_s *e = &o->dpb[k];
        if (!e->state) continue;
        ++refs;
        CHECK(e->index != (unsigned)surf || e->not_existing || o->second_field,
              "ref aliases the current surface");
        CHECK(e->top_field_marking == 1 || e->top_field_marking == 2, "marking");
    }
    CHECK(refs <= (s->max_num_ref_frames ? s->max_num_ref_frames : 1), "%d refs > max %d", refs,
          s->max_num_ref_frames);
    (void)d;
}

static void run_vector(const char *dir, const char *name) {
    char path[512];
    size_t len = 0, plen = 0;
    snprintf(path, sizeof(path), "%s/%s.h264", dir, name);
    uint8_t *bs = load(path, &len);
    snprintf(path, sizeof(path), "%s/%s.pts", dir, name);
    char *ptxt = (char *)load(path, &plen);
    int pts[512], npts = 0;
    for (char *t = strtok(ptxt, "\n"); t && npts < 512; t = strtok(NULL, "\n")) pts[npts++] = atoi(t);

    static nvdec_h264_dec d;
    nvdec_h264_init(&d);
    n_out = 0;
    static uint8_t in[4 << 20];
    nvdec_h264_stream st;
    nvdec_h264_pic_s setup;
    h264_slice prev, sl;
    int have_prev = 0, pics = 0, surf = -1, max_slices = 0, mmco = 0;
    size_t pos = 0;
    h264_nal nal;
    uint32_t push[256];
    nvdec_h264_layout lay;
    for (;;) {
        const int more = h264_next_nal(bs, len, &pos, &nal);
        const int is_slice = more && (nal.type == H264_NAL_SLICE || nal.type == H264_NAL_IDR);
        int newpic = 0;
        if (is_slice) {
            CHECK(!h264_parse_slice(nal.data, nal.size, d.sps, d.pps, &sl), "slice parse");
            newpic = nvdec_h264_new_picture(&d, have_prev ? &prev : NULL, &sl);
        }
        if (have_prev && (!more || newpic || (more && !is_slice && nal.type <= H264_NAL_AUD &&
                                              nal.type >= H264_NAL_SEI))) {
            if (st.slices > (uint32_t)max_slices) max_slices = (int)st.slices;
            CHECK(!nvdec_h264_stream_end(&st, &setup), "stream end");
            const uint32_t *offs = (const uint32_t *)(in + NVDEC_IN_SLICES);
            CHECK(offs[0] == 0 && offs[st.slices] == st.len, "slice table");
            CHECK(in[NVDEC_IN_BITSTREAM] == 0 && in[NVDEC_IN_BITSTREAM + 2] == 1, "start code");
            CHECK(!memcmp(in + NVDEC_IN_BITSTREAM + st.len, "\0\0\1\x0b", 4), "end sequence");
            const nvdec_h264_pic_s *cp = (const nvdec_h264_pic_s *)(in + NVDEC_IN_SETUP);
            CHECK(cp->stream_len == st.len + 16 && cp->slice_count == st.slices, "setup copy");
            nvdec_h264_addrs a;
            memset(&a, 0, sizeof(a));
            a.in_va = 0x2b00000000ull;
            a.coloc_va = 0x2b00400000ull;
            a.mbhist_va = 0x2b00800000ull;
            a.history_va = 0x2b00900000ull;
            for (int i = 0; i < NVDEC_H264_SURFACES; ++i)
                a.surface_va[i] = 0x2c00000000ull + (uint64_t)i * ((lay.surface_bytes + 0xffff) & ~0xffffu);
            a.subch = 4;
            a.obj_class = 0xc9b0;
            const uint32_t n = nvdec_h264_push(&a, &lay, d.pictures, d.pictures + 1, push, 256);
            CHECK(n == 2 * 13 + 2 * 18 + 4 + 2 - 2, "push words %u", n);
            nvdec_h264_end(&d);
            drain(&d);
            have_prev = 0;
            ++pics;
        }
        if (!more) break;
        if (!is_slice) {
            CHECK(!nvdec_h264_param_nal(&d, &nal), "param nal %d", nal.type);
            continue;
        }
        if (!have_prev) {
            const int r = nvdec_h264_begin(&d, &sl, &setup, &surf);
            CHECK(r == 0, "begin %d", r);
            if (r) break;
            drain(&d);   /* IDR / gap flushes */
            surf_owner[surf] = pics;
            const h264_sps *s = &d.sps[d.pps[sl.pps_id].sps_id];
            nvdec_h264_layout_for(s, &lay);
            check_setup(&d, &setup, surf, s);
            nvdec_h264_stream_begin(&st, in, sizeof(in));
        }
        CHECK(!nvdec_h264_stream_slice(&st, &nal), "slice add");
        mmco += sl.n_mmco;
        prev = sl;
        have_prev = 1;
    }
    nvdec_h264_flush(&d);
    drain(&d);
    CHECK(pics == npts, "%d pictures, %d AUs", pics, npts);
    CHECK(n_out == pics, "%d outputs for %d pictures", n_out, pics);
    int order_ok = 1;
    for (int k = 0; k < n_out; ++k) order_ok &= pts[out_order[k]] == k;
    CHECK(order_ok, "output order differs from display order");
    const h264_sps *s = &d.sps[d.sps_id];
    printf("%-9s %dx%d (crop %d,%d) profile %d poc_type %d refs %d dpb %d reorder %d: %d pictures,"
           " max %d slices, %d MMCO ops, output order %s\n", name, s->width, s->height,
           s->crop_right, s->crop_bottom, s->profile_idc, s->poc_type, s->max_num_ref_frames,
           d.dpb_frames, d.reorder, pics, max_slices, mmco, order_ok ? "OK" : "WRONG");
    if (!strcmp(name, "high_cqm")) {
        const h264_pps *p = &d.pps[0];
        CHECK(s->seq_scaling_matrix_present_flag || p->pic_scaling_matrix_present_flag, "cqm present");
        CHECK(setup.WeightScale[0][0][0] == 6 && setup.WeightScale[3][0][0] == 10, "4x4 JVT default");
        CHECK(setup.WeightScale8x8[0][0][0] == 6 && setup.WeightScale8x8[1][0][0] == 9, "8x8 JVT default");
        CHECK(setup.WeightScale[0][0][1] == 13 && setup.WeightScale[0][1][0] == 13 &&
              setup.WeightScale[0][3][3] == 42, "raster order");
        CHECK(setup.transform_8x8_mode_flag == 1 && s->crop_right == 8, "8x8 / crop");
    }
    if (!strcmp(name, "mbaff_tff")) {
        CHECK(!s->frame_mbs_only_flag && s->mb_adaptive_frame_field_flag, "MBAFF SPS");
        CHECK(setup.MbaffFrameFlag == 1 && setup.frame_mbs_only_flag == 0 &&
              setup.field_pic_flag == 0, "MBAFF setup");
        CHECK(s->frame_height_in_mbs == 10 && s->crop_bottom == 16, "160 coded rows, crop 16");
    }
    free(bs);
    free(ptxt);
}


/*
 * Long-term references (MMCO 2/3/4/6) and a frame_num gap, which x264 never
 * produces: synthetic slice headers on a POC type 2 sequence.
 */
static int find_ref(const nvdec_h264_pic_s *o, int long_term, int idx, int not_existing) {
    for (int k = 0; k < 16; ++k) {
        const nvdec_dpb_entry_s *e = &o->dpb[k];
        if (e->state && e->is_long_term == (unsigned)long_term && e->FrameIdx == idx &&
            e->not_existing == (unsigned)not_existing)
            return 1;
    }
    return 0;
}

static int nrefs(const nvdec_h264_pic_s *o) {
    int n = 0;
    for (int k = 0; k < 16; ++k) n += o->dpb[k].state != 0;
    return n;
}

static void long_term_and_gaps(void) {
    static nvdec_h264_dec d;
    nvdec_h264_init(&d);
    h264_sps *s = &d.sps[0];
    s->valid = 1; s->chroma_format_idc = 1; s->bit_depth_luma = s->bit_depth_chroma = 8;
    s->log2_max_frame_num = 4; s->poc_type = 2; s->max_num_ref_frames = 4;
    s->pic_width_in_mbs = 4; s->frame_height_in_mbs = 4; s->pic_height_in_map_units = 4;
    s->frame_mbs_only_flag = 1; s->level_idc = 30; s->gaps_in_frame_num_allowed_flag = 1;
    h264_pps *p = &d.pps[0];
    p->valid = 1; p->num_ref_idx_l0_default_active = p->num_ref_idx_l1_default_active = 1;
    p->pic_init_qp = 26;
    nvdec_h264_pic_s o;
    int surf, outs = 0;
    h264_slice sl;
#define PIC(fn, idr_, setup_mmco) do {                                   \
        memset(&sl, 0, sizeof(sl));                                      \
        sl.frame_num = (fn); sl.idr = (idr_); sl.nal_ref_idc = 1;        \
        sl.nal_type = (idr_) ? 5 : 1; sl.slice_type = (idr_) ? 2 : 0;    \
        setup_mmco;                                                      \
        CHECK(!nvdec_h264_begin(&d, &sl, &o, &surf), "begin fn %d", fn); \
        outs += d.n_out;                                                 \
    } while (0)
#define END() do { nvdec_h264_end(&d); outs += d.n_out; } while (0)
    PIC(0, 1, ); END();
    PIC(1, 0, ); CHECK(nrefs(&o) == 1 && find_ref(&o, 0, 0, 0), "fn1 refs"); END();
    PIC(2, 0, sl.adaptive_ref_pic_marking_mode_flag = 1; sl.n_mmco = 1;
        sl.mmco[0].op = 3; sl.mmco[0].diff_pic_nums_minus1 = 0; sl.mmco[0].long_term_frame_idx = 0);
    END();
    PIC(3, 0, sl.adaptive_ref_pic_marking_mode_flag = 1; sl.n_mmco = 1;
        sl.mmco[0].op = 6; sl.mmco[0].long_term_frame_idx = 1);
    CHECK(nrefs(&o) == 3 && find_ref(&o, 0, 0, 0) && find_ref(&o, 1, 0, 0) &&
          find_ref(&o, 0, 2, 0), "fn3 sees S0 L0(fn1) S2");
    END();
    PIC(5, 0, );   /* frame_num 4 missing */
    CHECK(nrefs(&o) == 4 && !find_ref(&o, 0, 0, 0) && find_ref(&o, 1, 0, 0) &&
          find_ref(&o, 1, 1, 0) && find_ref(&o, 0, 2, 0) && find_ref(&o, 0, 4, 1),
          "fn5: fn0 slid out, L0 L1 S2 + non-existing S4");
    END();
    PIC(6, 0, sl.adaptive_ref_pic_marking_mode_flag = 1; sl.n_mmco = 2;
        sl.mmco[0].op = 2; sl.mmco[0].long_term_pic_num = 0;
        sl.mmco[1].op = 4; sl.mmco[1].max_long_term_frame_idx_plus1 = 0);
    CHECK(nrefs(&o) == 4 && find_ref(&o, 0, 5, 0) && !find_ref(&o, 0, 2, 0), "fn6 refs");
    END();
    PIC(7, 0, );
    CHECK(nrefs(&o) == 3 && !find_ref(&o, 1, 0, 0) && !find_ref(&o, 1, 1, 0) &&
          find_ref(&o, 0, 4, 1) && find_ref(&o, 0, 5, 0) && find_ref(&o, 0, 6, 0),
          "fn7: longs gone (mmco 2 + 4)");
    END();
    nvdec_h264_flush(&d);
    outs += d.n_out;
    CHECK(outs == 7, "7 real pictures output (non-existing never), got %d", outs);
    printf("long-term / gap synthetic sequence: %s\n", fails ? "see failures" : "OK");
#undef PIC
#undef END
}

/*
 * Field pictures (PAFF), which x264 never produces (its interlacing is MBAFF):
 * synthetic slice headers, POC type 0, top field first. Checks pairing into one
 * frame store, per-field POC and marking, the current frame's first field as a
 * reference of its second field, a field MMCO 1, an unpaired field, a frame
 * after fields, and display order.
 */
static const nvdec_dpb_entry_s *entry_for(const nvdec_h264_pic_s *o, int frame_idx) {
    for (int k = 0; k < 16; ++k)
        if (o->dpb[k].state && o->dpb[k].FrameIdx == frame_idx) return &o->dpb[k];
    return NULL;
}

static void field_pictures(void) {
    static nvdec_h264_dec d;
    nvdec_h264_init(&d);
    h264_sps *s = &d.sps[0];
    s->valid = 1; s->chroma_format_idc = 1; s->bit_depth_luma = s->bit_depth_chroma = 8;
    s->log2_max_frame_num = 4; s->poc_type = 0; s->log2_max_poc_lsb = 6; s->max_num_ref_frames = 3;
    s->pic_width_in_mbs = 4; s->frame_height_in_mbs = 4; s->pic_height_in_map_units = 2;
    s->frame_mbs_only_flag = 0; s->level_idc = 30;
    s->bitstream_restriction_flag = 1; s->num_reorder_frames = 2; s->max_dec_frame_buffering = 4;
    h264_pps *p = &d.pps[0];
    p->valid = 1; p->num_ref_idx_l0_default_active = p->num_ref_idx_l1_default_active = 1;
    p->pic_init_qp = 26;
    nvdec_h264_pic_s o;
    int surf = -1, first_surf = -1, pocs[16], npoc = 0;
    h264_slice sl;
    const nvdec_dpb_entry_s *e;
#define COLLECT() do { for (int i_ = 0; i_ < d.n_out && npoc < 16; ++i_) pocs[npoc++] = d.out_poc[i_]; } while (0)
#define FLD(fn, bot, lsb, ref, idr_, extra) do {                          \
        memset(&sl, 0, sizeof(sl));                                       \
        sl.frame_num = (fn); sl.field_pic_flag = 1; sl.bottom_field_flag = (bot); \
        sl.pic_order_cnt_lsb = (lsb); sl.nal_ref_idc = (ref); sl.idr = (idr_);   \
        sl.nal_type = (idr_) ? 5 : 1;                                     \
        extra;                                                            \
        CHECK(!nvdec_h264_begin(&d, &sl, &o, &surf), "begin fn %d bot %d", fn, bot); \
        COLLECT();                                                        \
    } while (0)
#define END() do { nvdec_h264_end(&d); COLLECT(); } while (0)
    FLD(0, 0, 0, 1, 1, );
    first_surf = surf;
    CHECK(o.field_pic_flag == 1 && o.bottom_field_flag == 0 && o.second_field == 0, "IDR top flags");
    CHECK(o.CurrFieldOrderCnt[0] == 0 && o.CurrFieldOrderCnt[1] == 0 && nrefs(&o) == 0, "IDR top");
    END();
    FLD(0, 1, 1, 1, 0, );
    CHECK(surf == first_surf && o.second_field == 1 && o.bottom_field_flag == 1, "pair shares the surface");
    CHECK(o.CurrFieldOrderCnt[0] == 0 && o.CurrFieldOrderCnt[1] == 1, "pair POCs");
    e = entry_for(&o, 0);
    CHECK(nrefs(&o) == 1 && e && e->index == (unsigned)surf && e->state == 1 && e->is_field &&
          e->top_field_marking == 1 && e->bottom_field_marking == 0, "first field is a reference");
    END();
    FLD(1, 0, 8, 1, 0, );
    const int surf1 = surf;
    e = entry_for(&o, 0);
    CHECK(surf1 != first_surf && nrefs(&o) == 1 && e && e->state == 3 &&
          e->FieldOrderCnt[0] == 0 && e->FieldOrderCnt[1] == 1, "frame 0 both fields");
    END();
    FLD(1, 1, 9, 1, 0, );
    CHECK(surf == surf1 && nrefs(&o) == 2 && entry_for(&o, 1) && entry_for(&o, 1)->state == 1, "fn1 pair");
    END();
    FLD(2, 0, 4, 0, 0, );   /* non-reference field pair, displayed between 0 and 1 */
    const int surfb = surf;
    CHECK(o.ref_pic_flag == 0 && nrefs(&o) == 2, "B top");
    END();
    FLD(2, 1, 5, 0, 0, );
    CHECK(surf == surfb && o.second_field == 1 && nrefs(&o) == 2, "B pair");
    END();
    /*
     * Top field, MMCO 1 on frame 0's bottom field: CurrPicNum 5, bottom of
     * FrameNumWrap 0 = opposite parity -> PicNum 0 -> diff_pic_nums_minus1 4
     */
    FLD(2, 0, 12, 1, 0, sl.adaptive_ref_pic_marking_mode_flag = 1; sl.n_mmco = 1;
        sl.mmco[0].op = 1; sl.mmco[0].diff_pic_nums_minus1 = 4);
    const int surf2 = surf;
    END();
    FLD(2, 1, 13, 1, 0, );
    CHECK(surf == surf2 && nrefs(&o) == 3, "fn2 pair sees 0, 1, own top");
    e = entry_for(&o, 0);
    CHECK(e && e->state == 1 && e->top_field_marking == 1 && e->bottom_field_marking == 0,
          "MMCO 1 removed frame 0's bottom field only");
    e = entry_for(&o, 2);
    CHECK(e && e->index == (unsigned)surf && e->state == 1, "own first field");
    END();
    FLD(3, 0, 16, 1, 0, );   /* stays unpaired */
    END();
    /* Frame after fields: sliding window at fn3 dropped frame 0 (3 refs) */
    memset(&sl, 0, sizeof(sl));
    sl.frame_num = 4; sl.pic_order_cnt_lsb = 20; sl.delta_pic_order_cnt_bottom = 1;
    sl.nal_ref_idc = 1; sl.nal_type = 1;
    CHECK(!nvdec_h264_begin(&d, &sl, &o, &surf), "frame fn4");
    COLLECT();
    CHECK(o.field_pic_flag == 0 && o.second_field == 0 && o.CurrFieldOrderCnt[0] == 20 &&
          o.CurrFieldOrderCnt[1] == 21, "frame after fields");
    e = entry_for(&o, 3);
    CHECK(nrefs(&o) == 3 && !entry_for(&o, 0) && e && e->state == 1 && e->is_field &&
          e->FieldOrderCnt[0] == 16, "fn4 refs: 1, 2, unpaired top field 3");
    END();
    nvdec_h264_flush(&d);
    COLLECT();
    static const int want[] = {0, 4, 8, 12, 16, 20};
    int ok = npoc == 6;
    for (int i = 0; ok && i < 6; ++i) ok = pocs[i] == want[i];
    CHECK(ok, "display order (got %d outputs)", npoc);
    printf("field pictures (PAFF) synthetic sequence: %s\n", fails ? "see failures" : "OK");
#undef FLD
#undef END
#undef COLLECT
}


static void md5_check(void) {
    char hex[33];
    nvdec_md5 m;
    nvdec_md5_init(&m);
    nvdec_md5_hex(&m, hex);
    CHECK(!strcmp(hex, "d41d8cd98f00b204e9800998ecf8427e"), "md5 empty %s", hex);
    nvdec_md5_init(&m);
    nvdec_md5_update(&m, "abc", 3);
    nvdec_md5_hex(&m, hex);
    CHECK(!strcmp(hex, "900150983cd24fb0d6963f7d28e17f72"), "md5 abc %s", hex);
    static uint8_t big[100000];
    for (int i = 0; i < 100000; ++i) big[i] = (uint8_t)(i * 31 + 7);
    nvdec_md5_init(&m);
    for (int off = 0; off < 100000; off += 777)   /* odd chunking across block edges */
        nvdec_md5_update(&m, big + off, 100000 - off < 777 ? (size_t)(100000 - off) : 777);
    nvdec_md5_hex(&m, hex);
    CHECK(!strcmp(hex, "7354f16d06a013232daa23cfa7a092df"), "md5 chunked %s", hex);
}


/*
 * Surface -> cropped NV12 md5 equals the md5 of the same picture laid out the
 * way gen_vectors.py hashes it
 */
static void surface_md5_check(void) {
    h264_sps s;
    memset(&s, 0, sizeof(s));
    s.pic_width_in_mbs = 13; s.frame_height_in_mbs = 8;   /* 208x128 coded */
    s.crop_right = 8; s.crop_bottom = 8;                  /* 200x120 visible */
    nvdec_h264_layout l;
    nvdec_h264_layout_for(&s, &l);
    const uint32_t W = 200, H = 120;
    uint8_t *pl = calloc(1, (size_t)l.width * l.aligned_height * 3 / 2);
    uint8_t *surf = calloc(1, l.surface_bytes);
    nvdec_md5 m;
    nvdec_md5_init(&m);
    for (uint32_t y = 0; y < l.aligned_height * 3 / 2; ++y)
        for (uint32_t x = 0; x < l.width; ++x) pl[(size_t)y * l.width + x] = (uint8_t)(x * 5 + y * 3 + (y >> 3));
    for (uint32_t y = 0; y < H; ++y) nvdec_md5_update(&m, pl + (size_t)y * l.width, W);
    const uint8_t *uv = pl + (size_t)l.width * l.aligned_height;
    for (uint32_t y = 0; y < H / 2; ++y) nvdec_md5_update(&m, uv + (size_t)y * l.width, W);
    char want[33], got[33];
    nvdec_md5_hex(&m, want);
    nvdec_tile(pl, l.width, surf, l.pitch, l.width, l.aligned_height);
    nvdec_tile(uv, l.width, surf + l.luma_bytes, l.pitch, l.width, l.aligned_height / 2);
    CHECK(!nvdec_h264_surface_md5(surf, &l, &s, got) && !strcmp(want, got), "surface md5 %s vs %s", got, want);
    free(pl);
    free(surf);
}

static void tiling(void) {
    enum { W = 200, H = 64, P = 256 };
    static uint8_t src[H][W], bl[P * H], back[H][W];
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) src[y][x] = (uint8_t)(x * 7 + y * 13 + (x >> 4));
    memset(bl, 0xee, sizeof(bl));
    nvdec_tile(&src[0][0], W, bl, P, W, H);
    nvdec_detile(bl, P, &back[0][0], W, W, H);
    CHECK(!memcmp(src, back, sizeof(src)), "tile round trip");
    /* Measured on AD103 NVDEC (KBL): 16 B x 4 rows columns, row quads, x halves */
    CHECK(bl[0] == src[0][0] && bl[16] == src[1][0] && bl[48] == src[3][0] && bl[64] == src[0][16],
          "column of four rows");
    CHECK(bl[128] == src[4][0] && bl[256] == src[0][32] && bl[512] == src[8][0], "GOB layout");
    CHECK(bl[1024] == src[0][64] && bl[(P / 64) * 1024] == src[16][0], "block layout");
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "tests/nvdec_h264/vectors";
    assert(sizeof(nvdec_h264_pic_s) == 764 && sizeof(nvdec_dpb_entry_s) == 16);
    run_vector(dir, "base_ip");
    run_vector(dir, "main_b");
    run_vector(dir, "high_cqm");
    run_vector(dir, "mbaff_tff");
    long_term_and_gaps();
    field_pictures();
    tiling();
    md5_check();
    surface_md5_check();
    printf("nvdec_h264_check: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
