#include "../../NVGspCore/NVGspFbLayout.hpp"
#include <cassert>
#include <cstdio>

int main() {
    nvgsp::FbLayout fb{};
    assert(nvgsp::ad103HeapSize(16376 * nvgsp::kMiB) == 128 * nvgsp::kMiB);
    assert(nvgsp::planAd103FbLayout(16376 * nvgsp::kMiB, false, 0,
                                    36864, 63541248, &fb));
    assert(fb.fbSize == 0x3ff800000ULL);
    assert(fb.vgaWorkspaceOffset == 0x3ff700000ULL);
    assert(fb.gspFwWprEnd == 0x3ff700000ULL);
    assert(fb.frtsOffset == 0x3ff600000ULL);
    assert(fb.bootBinOffset == 0x3ff5f7000ULL);
    assert(fb.gspFwOffset == 0x3fb950000ULL);
    assert(fb.gspFwHeapOffset == 0x3f3900000ULL);
    assert(fb.gspFwHeapSize == 128 * nvgsp::kMiB);
    assert(fb.gspFwWprStart == 0x3f3800000ULL);
    assert(fb.gspFwRsvdStart == 0x3f3700000ULL);
    assert(fb.gspFwRsvdStart >= fb.fbSize - 256 * nvgsp::kMiB);
    assert(!nvgsp::planAd103FbLayout(128 * nvgsp::kMiB, false, 0,
                                     36864, 63541248, &fb));
    assert(!nvgsp::planAd103FbLayout(16376 * nvgsp::kMiB, true,
                                     16377 * nvgsp::kMiB,
                                     36864, 63541248, &fb));
    std::printf("AD103 FB: %llu MiB usable, WPR 0x%llx..0x%llx, heap %llu MiB\n",
                (unsigned long long)(fb.fbSize / nvgsp::kMiB),
                (unsigned long long)fb.gspFwWprStart,
                (unsigned long long)fb.gspFwWprEnd,
                (unsigned long long)(fb.gspFwHeapSize / nvgsp::kMiB));
}
