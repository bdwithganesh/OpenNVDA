#include "../../NVGspCore/NVGspChannel.hpp"
#include "../../NVGspCore/NVGspVideo.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>

static uint32_t rd(const uint8_t *p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; std::memcpy(&v, p, 8); return v; }

int main() {
    using namespace nvgsp;
    // engine table
    const VideoEngineDesc *d = videoEngine(kVideoNvdec), *e = videoEngine(kVideoNvenc),
                          *o = videoEngine(kVideoOfa);
    assert(d && e && o && !videoEngine(3));
    assert(d->engineType == 0x13 && d->objClass == 0xc9b0 && d->engDesc == (0x8f99e1u << 8));
    assert(e->engineType == 0x1b && e->objClass == 0xc9b7 && e->engDesc == (0xe97b6cu << 8));
    assert(o->engineType == 0x33 && o->objClass == 0xc9fa && o->engDesc == (0xdd7babu << 8));
    // Handles unique across engines and distinct from the CE (0xc0d1....) ones
    uint32_t hs[3][6];
    for (uint32_t i = 0; i < 3; ++i) {
        const VideoEngineDesc &v = *videoEngine(i);
        const uint32_t h[6] = {videoChannelHandle(v), videoBackingHandle(v, 0),
                               videoBackingHandle(v, 1), videoBackingHandle(v, 2),
                               videoObjectHandle(v), v.chid};
        std::memcpy(hs[i], h, sizeof(h));
        assert((h[0] >> 16) == 0xc0d2);
    }
    for (uint32_t i = 0; i < 3; ++i)
        for (uint32_t j = 0; j < 3; ++j)
            for (uint32_t a = 0; a < 5; ++a)
                for (uint32_t b = 0; b < 5; ++b)
                    if (i != j || a != b) assert(hs[i][a] != hs[j][b]);
    // chid 5/6/7 (GR 3, CE 4); flags: PAGE_FIXED + index value
    assert(videoChannelFlags(*d) == 0x00200500 && videoChannelFlags(*o) == 0x00200700);

    // channel alloc accepts the video engine types
    NvChannelAllocParams chan{};
    for (uint32_t i = 0; i < 3; ++i)
        assert(buildChannelAllocParams(0xc0d090f1, videoBackingHandle(*videoEngine(i), 0),
                                       0x2040000000ULL, 512, videoEngine(i)->engineType, &chan) &&
               chan.engineType == videoEngine(i)->engineType &&
               chan.hUserdMemory[0] == videoBackingHandle(*videoEngine(i), 0));
    assert(!buildChannelAllocParams(0xc0d090f1, 0, 0, 512, 0x14, &chan));   // NVDEC1: no

    // falcon info parse
    uint8_t info[kFalconInfoBytes]{};
    const uint32_t n = 3;
    std::memcpy(info, &n, 4);
    const uint32_t rows[3][5] = {{0x12345600, 0, 0x100, 0, 0},
                                 {d->engDesc, 1, 0x4000, 1, 0x848000},
                                 {e->engDesc, 1, 0, 1, 0x1c8000}};
    std::memcpy(info + 4, rows, sizeof(rows));
    uint32_t cb = 99;
    assert(falconCtxBytes(info, sizeof(info), d->engDesc, &cb) && cb == 0x4000);
    assert(falconCtxBytes(info, sizeof(info), e->engDesc, &cb) && cb == 0);
    assert(!falconCtxBytes(info, sizeof(info), o->engDesc, &cb));   // not constructed
    assert(!falconCtxBytes(info, 4 + 20, e->engDesc, &cb));         // truncated reply
    const uint32_t bad = 0x41;
    std::memcpy(info, &bad, 4);
    assert(!falconCtxBytes(info, sizeof(info), d->engDesc, &cb));

    // promote params
    uint8_t pr[kPromoteCtxBytes];
    assert(!buildFalconPromote(0x13, 1, 5, 2, 0x100, 0x2000000100ULL, 0, pr, sizeof(pr)));
    assert(!buildFalconPromote(0x13, 1, 5, 2, 0x180, 0x2000000100ULL, 64, pr, sizeof(pr)));
    assert(!buildFalconPromote(0x13, 1, 5, 2, 0x100, 0x100, 64, pr, 100));
    assert(buildFalconPromote(0x13, 0xc0d00001, 5, 0xc0d2006f, 0x40100000ULL,
                              0x2040100000ULL, 0x4000, pr, sizeof(pr)));
    assert(rd(pr) == 0x13 && rd(pr + 4) == 0xc0d00001 && rd(pr + 8) == 5 &&
           rd(pr + 12) == 0xc0d00001 && rd(pr + 16) == 0xc0d2006f && rd(pr + 20) == 0);
    assert(rd64(pr + 24) == 0x2040100000ULL && rd64(pr + 32) == 0x4000 && rd(pr + 40) == 1);
    assert(rd64(pr + 48) == 0x40100000ULL && rd64(pr + 56) == 0x2040100000ULL &&
           rd64(pr + 64) == 0x4000 && rd(pr + 72) == 4 && pr[76] == 0 && pr[77] == 0 &&
           pr[78] == 1 && pr[79] == 0);
    for (uint32_t i = 80; i < kPromoteCtxBytes; ++i) assert(pr[i] == 0);
    // variants
    assert(buildFalconPromote(0x13, 0xc0d00001, 5, 0xc0d2006f, 0x40100000ULL, 0x2040100000ULL,
                              0x4000, pr, sizeof(pr), kPromoteRmExternal));
    assert(rd(pr + 4) == 0xc0d00001 && rd(pr + 8) == 5 && rd64(pr + 24) == 0 &&
           rd64(pr + 32) == 0x4000 && rd64(pr + 48) == 0x40100000ULL && rd64(pr + 56) == 0 &&
           pr[78] == 1 && pr[79] == 1 && rd(pr + 72) == 4);
    assert(buildFalconPromote(0x13, 0xc0d00001, 5, 0xc0d2006f, 0x40100000ULL, 0x2040100000ULL,
                              0x4000, pr, sizeof(pr), kPromoteGrStyle));
    assert(rd(pr) == 0x13 && rd(pr + 4) == 0 && rd(pr + 8) == 0 && rd(pr + 12) == 0xc0d00001 &&
           rd(pr + 16) == 0xc0d2006f && rd64(pr + 24) == 0 && rd64(pr + 32) == 0 &&
           rd(pr + 40) == 1 && rd64(pr + 56) == 0x2040100000ULL && pr[78] == 1 && pr[79] == 0);
    assert(!buildFalconPromote(0x13, 1, 5, 2, 0x100, 0x100, 64, pr, sizeof(pr), 3));
    // VA bind promote (UVM layout)
    assert(!buildFalconPromoteVa(0x13, 1, 2, 0, pr, sizeof(pr)));
    assert(!buildFalconPromoteVa(0x13, 1, 2, 0x100080, pr, sizeof(pr)));
    assert(buildFalconPromoteVa(0x13, 0xc0d00001, 0xc0d2006f, 0x2040100000ULL, pr, sizeof(pr)));
    assert(rd(pr) == 0x13 && rd(pr + 4) == 0 && rd(pr + 8) == 0 && rd(pr + 12) == 0xc0d00001 &&
           rd(pr + 16) == 0xc0d2006f && rd64(pr + 24) == 0 && rd64(pr + 32) == 0 && rd(pr + 40) == 1 &&
           rd64(pr + 48) == 0 && rd64(pr + 56) == 0x2040100000ULL && rd64(pr + 64) == 0 &&
           rd(pr + 72) == 0 && pr[76] == 0 && pr[77] == 0 && pr[78] == 0 && pr[79] == 0);
    // 560 = the GR promote size the kext already sends
    assert(kPromoteCtxBytes == 48 + 16 * 32);

    // object params
    uint8_t op[12];
    buildVideoObjectParams(0, op);
    assert(rd(op) == 12 && rd(op + 4) == 0 && rd(op + 8) == 0);

    // chunk layout
    assert(videoChunkBytes(0) == 0x200000 && videoCtxOffset(0x4000) == 0x100000);
    assert(videoChunkBytes(0x100000) == 0x200000);
    assert(videoCtxOffset(0x100001) == 0x200000 && videoChunkBytes(0x100001) == 0x400000);
    assert(kVideoPbOff + kVideoPbBytes <= 0x100000 && kVideoSemOff >= 512 * 8);

    // semaphore release pushbuffer
    uint32_t w[8];
    assert(!buildVideoSemRelease(4, 0xc9b0, 0x2800000002ULL, 1, w, 8));
    assert(!buildVideoSemRelease(8, 0xc9b0, 0x2800000000ULL, 1, w, 8));
    assert(!buildVideoSemRelease(4, 0xc9b0, 0x2800000000ULL, 1, w, 7));
    assert(buildVideoSemRelease(4, 0xc9b0, 0x2812345670ULL, 0xabc, w, 8) == 8);
    assert(w[0] == 0x20018000 && w[1] == 0xc9b0);
    assert(w[2] == (0x20038000 | 0x90) && w[3] == 0x28 && w[4] == 0x12345670 && w[5] == 0xabc);
    assert(w[6] == (0x20018000 | 0xc1) && w[7] == 0);
    std::puts("nvgsp_video_check: PASS");
    return 0;
}
