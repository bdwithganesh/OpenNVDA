#include "../../NVGspCore/NVGspEvict.hpp"
#include <cassert>
#include <cstdio>

int main() {
    using namespace nvgsp;
    // header format = nvrun's CE macro (subch 4, incrementing)
    assert(ceMethod(0x300, 1) == (0x20000000u | (1u << 16) | (4u << 13) | (0x300 >> 2)));
    assert(kCeLaunchPhysCopy == 0x3186);   // pitch/pitch, phys/phys, non-pipelined, flush
    uint32_t w[2 + 3 * kCeCopyWords];
    const EvictCopy c[3] = {
        {0x40000000ULL, 0x123400000ULL, false, true, kEvictChunkBytes},     // save
        {0x123400000ULL, 0x40000000ULL, true, false, kEvictChunkBytes},     // restore
        {0x3f0000000ULL, 0x0ffe00000ULL, false, true, 4096},
    };
    assert(buildEvictBatch(c, 3, w, 2 + 3 * kCeCopyWords - 1) == 0);   // too small
    const uint32_t n = buildEvictBatch(c, 3, w, 2 + 3 * kCeCopyWords);
    assert(n == 2 + 3 * kCeCopyWords);
    assert(w[0] == ceMethod(0, 1) && w[1] == 0xc7b5);
    const uint32_t *k = w + 2;
    assert(k[0] == ceMethod(0x260, 2) && k[1] == kCePhysLocalFb && k[2] == kCePhysCoherentSysmem);
    assert(k[3] == ceMethod(0x400, 8) && k[4] == 0 && k[5] == 0x40000000u && k[6] == 1 &&
           k[7] == 0x23400000u && k[8] == kEvictChunkBytes && k[11] == 1);
    assert(k[12] == ceMethod(0x300, 1) && k[13] == 0x3186);
    k += kCeCopyWords;
    assert(k[1] == kCePhysCoherentSysmem && k[2] == kCePhysLocalFb && k[4] == 1 && k[5] == 0x23400000u);
    k += kCeCopyWords;
    assert(k[4] == 3 && k[5] == 0xf0000000u && k[10] == 4096);
    // bad copies
    const EvictCopy empty{0, 0, false, true, 0}, far{1ULL << 49, 0, false, true, 1};
    assert(!buildEvictBatch(&empty, 1, w, 64) && !buildEvictBatch(&far, 1, w, 64));
    assert(!buildEvictBatch(c, 0, w, 64));
    assert(evictChunks(0) == 0 && evictChunks(1) == 1 && evictChunks(4ULL << 20) == 2);
    // A full batch fits the CE ring PB (0x1E0000 bytes) many times over
    static_assert((2 + kEvictBatchCopies * kCeCopyWords + 8) * 4 < 0x10000, "batch size");
    printf("nvgsp_evict_check: PASS\n");
    return 0;
}
