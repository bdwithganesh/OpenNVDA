#include "../drivers/NVGspCore/NVGspGrContext.hpp"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
template<class T> T read(const uint8_t *p, size_t off) { T v{}; memcpy(&v, p + off, sizeof(v)); return v; }
int main() {
    using namespace nvgsp;
    std::array<uint8_t, kGrPromoteBytes + 8> p; p.fill(0xa5);
    assert(buildGrPromote(0xc0d00001, 0xc0e9006f, 0x48000000, 0x2048000000, p.data(), kGrPromoteBytes));
    assert(read<uint32_t>(p.data(), 40) == 9);
    for (size_t i = 0; i < 9; ++i) {
        const auto &e = kGrContextEntries[i]; const auto *d = p.data() + 48 + i * 32;
        assert(uint64_t(e.offset) + e.bytes <= kGrContextBytes);
        assert(read<uint16_t>(d, 28) == e.id);
        assert(read<uint64_t>(d, 8) == (e.nonmapped ? 0ULL : 0x2048000000ULL + e.offset));
        assert(read<uint64_t>(d, 0) == (e.init ? 0x48000000 + e.offset : 0));
        assert(read<uint64_t>(d, 16) == (e.init ? e.bytes : 0));
        assert(d[30] == e.init && d[31] == e.nonmapped);
    }
    for (size_t i = 336; i < kGrPromoteBytes; ++i) assert(p[i] == 0);
    for (size_t i = kGrPromoteBytes; i < p.size(); ++i) assert(p[i] == 0xa5);
    assert(buildGrPromote(1, 2, 0x48000000, 0x2048000000, p.data(), p.size(), 0x104000000));
    assert(read<uint64_t>(p.data(), 48 + 4 * 32 + 8) == 0x104000000);
    assert(read<uint64_t>(p.data(), 48 + 8) == 0x204a600000);
    assert(read<uint32_t>(p.data(), 40) == 8);
    assert(read<uint64_t>(p.data(), 48 + 6 * 32) == 0);
    assert(read<uint64_t>(p.data(), 48 + 6 * 32 + 8) == 0x106880000);
    assert(p[48 + 6 * 32 + 30] == 0);
    assert(read<uint16_t>(p.data(), 48 + 7 * 32 + 28) == 11);
    assert(!buildGrPromote(1, 2, 0x48001000, 0x2048000000, p.data(), p.size()));
    assert(!buildGrPromote(1, 2, 0x48000000, 0x2048200000, p.data(), p.size()));
    assert(!buildGrPromote(1, 2, UINT64_MAX & ~0x1fffffULL, 0x2048000000, p.data(), p.size()));
    assert(!buildGrPromote(1, 2, 0x48000000, 0, p.data(), p.size()));
    assert(!buildGrPromote(1, 2, 0x48000000, 0x2048000000, p.data(), kGrPromoteBytes - 1));
    {
        std::array<uint8_t, kGrPromoteBytes + 8> c; c.fill(0xa5), p.fill(0);
        assert(buildGrPromote(1, 2, 0x48000000, 0x2048000000, p.data(), p.size(), 0x104000000));
        assert(buildGrPromoteCompact(1, 2, 0x48200000, 0x2048200000, 0x104000000, c.data(), kGrPromoteBytes));
        assert(read<uint32_t>(c.data(), 40) == 8);
        for (size_t i = 0; i < 8; ++i) {
            const auto *d = c.data() + 48 + i * 32, *f = p.data() + 48 + i * 32;
            const uint16_t id = read<uint16_t>(d, 28);
            assert(id == read<uint16_t>(f, 28) && d[30] == f[30] && d[31] == f[31]);
            if (id == 0 || id == 2) {
                const uint64_t off = id ? kGrPrivatePatchOffset : 0;
                assert(read<uint64_t>(d, 8) == 0x2048200000 + off && read<uint64_t>(d, 0) == 0x48200000 + off);
                assert(read<uint64_t>(d, 16) == read<uint64_t>(f, 16) && read<uint32_t>(d, 24) == 4);
            } else {
                assert(!memcmp(d, f, 32));
            }
        }
        for (size_t i = kGrPromoteBytes; i < c.size(); ++i) assert(c[i] == 0xa5);
        assert(!buildGrPromoteCompact(1, 2, 0x48100000, 0x2048200000, 0x104000000, c.data(), c.size()));
        assert(!buildGrPromoteCompact(1, 2, 0x48200000, 0x2048300000, 0x104000000, c.data(), c.size()));
        assert(!buildGrPromoteCompact(1, 2, 0x48200000, 0x2048200000, 0x104200000, c.data(), c.size()));
        assert(!buildGrPromoteCompact(1, 2, 0x48200000, 0x2048200000, 0x104000000, c.data(), kGrPromoteBytes - 1));
    }
    puts("GR context promote: aligned regions, init/nonmapped descriptors, overflow and bounds PASS");
}
