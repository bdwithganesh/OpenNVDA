/* nvdec_detile_crop (0.3.1) must give the same bytes as nvdec_detile + crop for every size and crop.
 * cc -I. tests/nvdec_h264/nvdec_detile_check.c nvdec_h264.c h264_parse.c  (from drivers/nvdec) */
#include "nvdec_h264.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    const uint32_t cases[][4] = {{1920, 1088, 0, 0}, {1920, 1080, 0, 4}, {200, 116, 0, 0}, {176, 144, 16, 2},
                                 {3840, 2160, 0, 0}, {1280, 720, 32, 8}, {200, 116, 0, 1}};
    srand(7);
    for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; ++c) {
        const uint32_t w = cases[c][0], h = cases[c][1], x0 = cases[c][2], y0 = cases[c][3];
        const uint32_t pitch = (x0 + w + 63) & ~63u, rows = (y0 + h + 15) & ~15u;
        uint8_t *bl = malloc((size_t)pitch * rows), *lin = malloc((size_t)pitch * rows);
        const uint32_t dpitch = ((w + 15) & ~15u) + 64;
        uint8_t *a = calloc((size_t)dpitch, h), *b = calloc((size_t)dpitch, h);
        for (size_t i = 0; i < (size_t)pitch * rows; ++i) bl[i] = (uint8_t)rand();
        nvdec_detile(bl, pitch, lin, pitch, pitch, rows);
        for (uint32_t y = 0; y < h; ++y) memcpy(a + (size_t)y * dpitch, lin + (size_t)(y + y0) * pitch + x0, w);
        assert(!nvdec_detile_crop(bl, pitch, b, dpitch, x0, y0, w, h));
        for (uint32_t y = 0; y < h; ++y) assert(!memcmp(a + (size_t)y * dpitch, b + (size_t)y * dpitch, w));
        free(bl); free(lin); free(a); free(b);
    }
    uint8_t one[64 * 16] = {0}, out[64] = {0};
    assert(nvdec_detile_crop(one, 64, out, 64, 8, 0, 16, 1) == -1);   /* x0 not a multiple of 16 */
    assert(nvdec_detile_crop(one, 64, out, 8, 0, 0, 16, 1) == -1);    /* destination row too short */
    puts("nvdec_detile_check: PASS");
    return 0;
}
