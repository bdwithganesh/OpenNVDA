/*
 * Host test: HEVC syntax parser on an x265 stream (SPS/PPS/scaling lists/slice
 * header up to the long-term RPS). Usage: hevc_parse_check FILE.h265
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../hevc_parse.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); ++fails; } } while (0)

static hevc_sps sps[HEVC_MAX_SPS];
static hevc_pps pps[HEVC_MAX_PPS];

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    static uint8_t buf[8 << 20];
    const size_t len = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    size_t pos = 0;
    h264_nal nal;
    int n_sps = 0, n_pps = 0, pics = 0, idr = 0, skip_nonzero = 0;
    while (h264_next_nal(buf, len, &pos, &nal)) {
        if (nal.size < 3) continue;
        const int t = hevc_nal_type(nal.data);
        if (t == HEVC_NAL_SPS) {
            hevc_sps s;
            CHECK(!hevc_parse_sps(nal.data, nal.size, &s), "sps");
            sps[s.id] = s;
            ++n_sps;
        } else if (t == HEVC_NAL_PPS) {
            hevc_pps p;
            CHECK(!hevc_parse_pps(nal.data, nal.size, sps, &p), "pps");
            pps[p.id] = p;
            ++n_pps;
        } else if (hevc_is_slice(t)) {
            hevc_slice sl;
            CHECK(!hevc_parse_slice(nal.data, nal.size, sps, pps, &sl), "slice %d", pics);
            if (!sl.first_slice_segment_in_pic_flag) continue;
            ++pics;
            idr += hevc_is_idr(t);
            if (!hevc_is_idr(t)) {
                skip_nonzero += sl.skip_bits > 0;
                CHECK(sl.st_rps.num_negative + sl.st_rps.num_positive <= 16, "rps size");
                for (int i = 1; i < sl.st_rps.num_negative; ++i)
                    CHECK(sl.st_rps.delta_poc_s0[i] < sl.st_rps.delta_poc_s0[i - 1], "S0 order");
                for (int i = 1; i < sl.st_rps.num_positive; ++i)
                    CHECK(sl.st_rps.delta_poc_s1[i] > sl.st_rps.delta_poc_s1[i - 1], "S1 order");
            }
        }
    }
    const hevc_sps *s = &sps[0];
    const hevc_pps *p = &pps[0];
    printf("%dx%d (conf %d,%d) ctb %d (%dx%d) poc_lsb %d dpb %d reorder %d st_rps %d scaling %d;"
           " %d SPS %d PPS, %d pictures (%d IDR), %d with skip bits\n",
           s->width, s->height, s->conf_right, s->conf_bottom, 1 << s->log2_max_cb, s->ctb_width,
           s->ctb_height, s->log2_max_poc_lsb, s->max_dec_pic_buffering, s->max_num_reorder,
           s->num_short_term_ref_pic_sets, s->scaling_list_enabled_flag, n_sps, n_pps, pics, idr,
           skip_nonzero);
    CHECK(n_sps >= 1 && n_pps >= 1 && pics == 20 && idr >= 1, "counts");
    CHECK(s->width == 208 && s->height == 120, "size");
    CHECK(p->column_width[0] == s->ctb_width && p->row_height[0] == s->ctb_height, "tile geometry");
    CHECK(skip_nonzero == pics - idr, "skip bits on every non-IDR picture");
    /* scaling-list=default: Table 7-6 intra 8x8 default, raster [7][7] = 115 */
    CHECK(s->scaling_list_enabled_flag && p->scaling.list8x8[0][63] == 115 &&
          p->scaling.list8x8[3][63] == 91 && p->scaling.list4x4[0][0] == 16, "default lists");
    printf("hevc_parse_check: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
