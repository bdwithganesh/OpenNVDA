#include "../drivers/NVGspCore/NVGspResidency.hpp"
#include <cassert>
#include <cstdio>

int main() {
    using namespace nvgsp;
    const uint64_t C = kResidencyChunkBytes;
    const uint64_t chunks[] = {0x900000000ULL, 0x100000000ULL, 0x800000000ULL};
    const ResidencyBacking local{0x40000000, 3 * C, nullptr, 0};
    const ResidencyBacking sys{0, 3 * C, chunks, 3};
    const ResidencyTranslation out{local, sys}, back{sys, local};
    assert(out.valid() && back.valid());
    uint64_t p = 0, q = 0;
    for (uint64_t o = 0; o < 3 * C; o += 4096) {
        assert(out.translate(local.base + o, 4096, &p));
        assert(p == chunks[o / C] + o % C);
        assert(back.translate(p, 4096, &q) && q == local.base + o);
    }
    assert(!out.translate(local.base - 4096, 4096, &p));
    assert(!out.translate(local.base + 3 * C, 4096, &p));
    assert(!out.translate(local.base + C - 4096, 8192, &p));
    assert(!out.translate(local.base, 0, &p));
    assert(!ResidencyBacking({0, C - 1, nullptr, 0}).valid());
    assert(!ResidencyBacking({kResidencyAddressEnd - C, 2 * C, nullptr, 0}).valid());
    assert(!ResidencyBacking({0, 3 * C, chunks, 2}).valid());
    const uint64_t duplicate[] = {C, C};
    assert(!ResidencyBacking({0, 2 * C, duplicate, 2}).valid());
    const uint64_t unaligned[] = {C + 1};
    assert(!ResidencyBacking({0, C, unaligned, 1}).valid());

    ResidencyCandidate c[10] = {};
    for (unsigned i = 0; i < 10; ++i) c[i] = {i, true, true, true, false, false, false, false, false};
    c[0].live = false; c[1].resident = false; c[2].idle = false;
    c[3].cpuMapped = true; c[4].rawBound = true; c[5].internal = true;
    c[6].presented = true; c[7].migrating = true;
    assert(residencyVictim(c, 10) == 8);
    c[9].lastUse = 1;
    assert(residencyVictim(c, 10) == 9);
    c[8].idle = c[9].idle = false;
    assert(residencyVictim(c, 10) == 10);
    assert(residencyVictim(nullptr, 0) == 0);
    std::puts("nvgsp_residency_check: PASS (1536 page round trips, LRU exclusions, bad backing)");
}
