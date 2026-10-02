// Host test for NVGspArenaMap.hpp: binds/unbinds through a fake VRAM and
// checks every translation with an independent MMU v2 page walker.
#include "../../NVGspCore/NVGspArenaMap.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

struct FakeVram {
    std::map<uint64_t, uint64_t> q;
    uint64_t nextChunk = 0x300000000ULL;
    int chunks = 0;
    int writes = 0, failWrite = -1, failRestore = -1;
    bool read64(uint64_t a, uint64_t *v) { assert(!(a & 7)); auto it = q.find(a); *v = it == q.end() ? 0 : it->second; return true; }
    bool write64(uint64_t a, uint64_t v) {
        assert(!(a & 7)); q[a] = v;
        const int n = writes++;
        return n != failWrite && n != failRestore; // failure may still have changed the PTE
    }
    bool zero(uint64_t a, uint64_t n) { for (uint64_t i = 0; i < n; i += 8) q.erase(a + i); return true; }
    bool fill16(uint64_t a, uint64_t n, uint64_t q0, uint64_t q1) {
        for (uint64_t i = 0; i < n; i += 16) { q[a + i] = q0; q[a + i + 8] = q1; }
        return true;
    }
    bool allocChunk(uint64_t *p) { *p = nextChunk; nextChunk += 0x200000; ++chunks; return true; }
    void *allocOwners(uint32_t n) { return std::malloc(n); }
    void freeOwners(void *p, uint32_t) { std::free(p); }
};
using Map = nvgsp::ArenaMap<FakeVram>;

// 30 Sep: with sparse tables every unmapped leaf must be sparse (VALID 0,
// VOL 1), never 0 or PRIV-only, or the MMU faults there.
static bool gSparse = false;

// Independent walker: returns true + phys/flags if va is mapped.
static bool walk(FakeVram &v, uint64_t tables, uint64_t va, uint64_t *phys, uint64_t *flags) {
    const uint64_t r = (va - nvgsp::kArenaBase) >> 21;
    uint64_t e0 = 0, e1 = 0;
    v.read64(tables + (r / 256) * 4096 + (r % 256) * 16, &e0);
    v.read64(tables + (r / 256) * 4096 + (r % 256) * 16 + 8, &e1);
    if (!e0 && e1) {                               // small page table (27 Sep)
        assert((e1 & 0xf) == 2);
        const uint64_t spt = (e1 & ~0xfULL) << 4;
        assert(!(spt & 0xfff));
        uint64_t pte = 0;
        v.read64(spt + ((va >> 12) & 511) * 8, &pte);
        if (!(pte & 1)) { assert(pte == (gSparse ? nvgsp::kSparse : 0)); return false; }
        *phys = ((pte & nvgsp::kPteAddrMask) << 4) + (va & 0xfff);
        *flags = pte & ~nvgsp::kPteAddrMask;
        return true;
    }
    assert(e1 == 0);                              // big modes never carry a small table
    if (e0 & 1) {                                  // 2 MiB PTE
        *phys = ((e0 & nvgsp::kPteAddrMask) << 4) + (va & 0x1fffff);
        *flags = e0 & ~nvgsp::kPteAddrMask;
        return true;
    }
    if (e0 == (gSparse ? nvgsp::kSparse : 0)) return false;
    assert((e0 & 0xf) == 2);                       // VRAM PDE (a 0 here under sparse fails)
    const uint64_t lpt = (e0 & ~0xfULL) << 4;
    assert(!(lpt & 0xff));
    uint64_t pte = 0;
    v.read64(lpt + ((va >> 16) & 31) * 8, &pte);
    if (!(pte & 1)) { assert(pte == (gSparse ? nvgsp::kSparse : nvgsp::kLptInvalid)); return false; }
    *phys = ((pte & nvgsp::kPteAddrMask) << 4) + (va & 0xffff);
    *flags = pte & ~nvgsp::kPteAddrMask;
    return true;
}

static void expectMapped(FakeVram &v, Map &m, uint64_t va, uint64_t phys, uint64_t flags, uint16_t owner) {
    uint64_t p = 0, f = 0;
    assert(walk(v, m.tables, va, &p, &f));
    assert(p == phys && f == (flags | 1));
    assert(m.ownerAt(va) == owner);
}
static void expectUnmapped(FakeVram &v, Map &m, uint64_t va) {
    uint64_t p, f;
    assert(!walk(v, m.tables, va, &p, &f));
    assert(m.ownerAt(va) == 0);
}

static void runCore() {
    const uint64_t B = nvgsp::kArenaBase, M2 = 0x200000, K64 = 0x10000;
    const uint64_t VRAM = 0, SYS = 0xC, KIND6 = 6ULL << 56;
    FakeVram v;
    std::unique_ptr<Map> mp(new Map());            // value-init: zeroed
    Map &m = *mp;
    m.sparse = gSparse;
    assert(m.init(v, 0x3f1000000ULL));

    // 1. whole 2 MiB regions -> 2 MiB PTEs, no LPT
    assert(m.bind(v, B, 2 * M2, 0x40000000, VRAM, 1));
    expectMapped(v, m, B + 0x1234, 0x40001234, VRAM, 1);
    expectMapped(v, m, B + M2 + 0x10, 0x40200010, VRAM, 1);
    assert(m.lptCount == 0 && v.chunks == 0);

    // 2. 64 KiB bind in an empty region -> LPT; neighbours unmapped
    const uint64_t R = B + 10 * M2;
    assert(m.bind(v, R + 3 * K64, 2 * K64, 0x50030000, KIND6, 2));
    expectMapped(v, m, R + 3 * K64 + 5, 0x50030005, KIND6, 2);
    expectMapped(v, m, R + 4 * K64, 0x50040000, KIND6, 2);
    expectUnmapped(v, m, R + 2 * K64);
    expectUnmapped(v, m, R + 5 * K64);
    assert(m.lptCount == 1 && v.chunks == 1);

    // 3. 2 MiB-aligned bind whose phys is NOT 2 MiB aligned -> LPT too
    const uint64_t R2 = B + 20 * M2;
    assert(m.bind(v, R2, M2, 0x60010000, SYS, 3));
    expectMapped(v, m, R2, 0x60010000, SYS, 3);
    expectMapped(v, m, R2 + M2 - 1, 0x60010000 + M2 - 1, SYS, 3);
    assert(m.lptCount == 2);

    // 4. partial unbind of a 2 MiB mapping splits it, rest stays identical
    assert(m.unbind(v, B + 4 * K64, K64));
    expectUnmapped(v, m, B + 4 * K64);
    expectMapped(v, m, B + 3 * K64 + 7, 0x40030007, VRAM, 1);
    expectMapped(v, m, B + 5 * K64, 0x40050000, VRAM, 1);
    expectMapped(v, m, B + M2, 0x40200000, VRAM, 1);   // other region untouched
    assert(m.lptCount == 3);

    // 5. rebind a 64 KiB hole with another owner, then free owner 1:
    //    owner 4's page survives, the split region keeps only it
    assert(m.bind(v, B + 4 * K64, K64, 0x70000000, KIND6, 4));
    bool any = false;
    assert(m.unbindOwner(v, 1, &any) && any);
    expectMapped(v, m, B + 4 * K64, 0x70000000, KIND6, 4);
    expectUnmapped(v, m, B);
    expectUnmapped(v, m, B + 5 * K64);
    expectUnmapped(v, m, B + M2);
    assert(m.lptCount == 3);

    // 6. last page of an LPT unbound -> LPT released, region empty
    assert(m.unbind(v, B + 4 * K64, K64));
    expectUnmapped(v, m, B + 4 * K64);
    assert(m.lptCount == 2);
    uint64_t e0 = 1;
    v.read64(m.tables, &e0);
    assert(e0 == (gSparse ? nvgsp::kSparse : 0));

    // 7. whole-region unbind of an LPT region clears it
    assert(m.unbind(v, R, M2));
    expectUnmapped(v, m, R + 3 * K64);
    assert(m.lptCount == 1);

    // 8. freed LPT slots are reused, no new chunk
    const uint64_t R3 = B + 100 * M2;
    assert(m.bind(v, R3 + K64, K64, 0x80000000, VRAM, 5));
    assert(m.lptCount == 2 && v.chunks == 1);
    assert(m.unbindOwner(v, 5, &any) && any && m.lptCount == 1);
    assert(m.unbindOwner(v, 5, &any) && !any);

    // 9. rejects: misaligned va/phys/size, outside arena, owner 0
    assert(!m.bind(v, B + 0x1000, K64, 0x40000000, VRAM, 1));
    assert(!m.bind(v, B, K64, 0x40001000, VRAM, 1));
    assert(!m.bind(v, B, 0x1000, 0x40000000, VRAM, 1));
    assert(!m.bind(v, nvgsp::kArenaEnd, K64, 0, VRAM, 1));
    assert(!m.bind(v, nvgsp::kArenaEnd - K64, 2 * K64, 0, VRAM, 1));
    assert(!m.bind(v, B, K64, 0x40000000, VRAM, 0));

    // 10. last arena region, spanning bind across 3 regions mixing modes
    const uint64_t Last = nvgsp::kArenaEnd - M2;
    assert(m.bind(v, Last, M2, 0x90000000, VRAM, 6));
    expectMapped(v, m, nvgsp::kArenaEnd - 1, 0x90000000 + M2 - 1, VRAM, 6);
    const uint64_t S = B + 200 * M2 + 31 * K64;       // last 64K of a region, a full one, 1 page
    assert(m.bind(v, S, K64 + M2 + K64, 0xA0000000 - K64 * 0, VRAM, 7));
    expectMapped(v, m, S, 0xA0000000, VRAM, 7);
    expectMapped(v, m, S + K64, 0xA0010000, VRAM, 7);          // region start, phys not 2M aligned -> LPT
    expectMapped(v, m, S + K64 + M2, 0xA0010000 + M2, VRAM, 7);
    assert(m.unbindOwner(v, 7, &any) && any);
    expectUnmapped(v, m, S + K64);

    m.release(v);
    std::printf("nvgsp_arena_map_check%s: PASS\n", gSparse ? " (sparse)" : "");
    {   // small pages: scattered 4 KiB host pages, owner unbind, no mixing
        FakeVram v2;
        std::unique_ptr<Map> m2(new Map());
        m2->sparse = gSparse;
        assert(m2->init(v2, 0x200000000ULL));
        const uint64_t base = nvgsp::kArenaBase + 0x40000000ULL + 0x1f0000;  // crosses a 2 MiB region
        uint64_t ph[40];
        for (int i = 0; i < 40; ++i) ph[i] = 0x7000000000ULL + uint64_t((i * 7919) % 97) * 0x1000;  // scattered
        assert(m2->bindPages(v2, base, 40, ph, 4 /* SYS aperture */, 9));
        for (int i = 0; i < 40; ++i) {
            uint64_t p = 0, f = 0;
            assert(walk(v2, m2->tables, base + i * 0x1000 + 0x123, &p, &f));
            assert(p == ph[i] + 0x123 && (f & 0xf) == 5);
            assert(m2->ownerAt(base + i * 0x1000) == 9);
        }
        uint64_t p = 0, f = 0;
        assert(!walk(v2, m2->tables, base - 0x1000, &p, &f));
        assert(m2->sptCount == 2);
        // a big bind into a small-page region is refused, and vice versa
        assert(!m2->bind(v2, base & ~0x1fffffULL, 0x10000, 0x100000000ULL, 0, 3));
        assert(m2->bind(v2, nvgsp::kArenaBase, 0x200000, 0x100000000ULL, 0, 3));
        assert(!m2->bindPages(v2, nvgsp::kArenaBase + 0x1000, 1, ph, 4, 9));
        bool any = false;
        assert(m2->unbindOwner(v2, 9, &any) && any);
        assert(m2->sptCount == 0);
        for (int i = 0; i < 40; ++i) assert(!walk(v2, m2->tables, base + i * 0x1000, &p, &f));
        assert(walk(v2, m2->tables, nvgsp::kArenaBase + 0x5000, &p, &f));   // big mapping untouched
        m2->release(v2);
        std::printf("small pages%s: PASS\n", gSparse ? " (sparse)" : "");
    }
}

int main() {
    const uint64_t B = nvgsp::kArenaBase, M2 = 0x200000, K64 = 0x10000;
    const uint64_t VRAM = 0, SYS = 0xC, KIND6 = 6ULL << 56;
    (void)B; (void)K64; (void)VRAM; (void)SYS; (void)KIND6;
    runCore();
    gSparse = true;
    runCore();
    gSparse = false;
    {   // Residency: round trips preserve offsets, aliases, kinds and holes.
        FakeVram vr;
        std::unique_ptr<Map> mr(new Map());
        assert(mr->init(vr, 0x3e0000000ULL));
        const uint64_t source = 0x80000000, target = 0x140000000ULL;
        const uint64_t sys[3] = {0x900000000ULL, 0x700000000ULL, 0xb00000000ULL};
        const nvgsp::ResidencyBacking local{source, 3 * M2, nullptr, 0};
        const nvgsp::ResidencyBacking spilled{0, 3 * M2, sys, 3};
        const nvgsp::ResidencyTranslation out{local, spilled};
        const nvgsp::ResidencyTranslation in{spilled, {target, 3 * M2, nullptr, 0}};
        assert(out.valid() && in.valid());
        assert(mr->bind(vr, B, 3 * M2, source, KIND6, 11));
        assert(mr->unbind(vr, B + M2 + 4 * K64, K64));
        assert(mr->bind(vr, B + M2 + 5 * K64, K64, 0x60000000, VRAM, 12));
        assert(mr->bind(vr, B + 20 * M2 + K64, 2 * K64, source + M2 + 7 * K64, KIND6, 11));
        const uint64_t small[2] = {source + 0x1000, source + 2 * M2 + 0x5000};
        assert(mr->bindPages(vr, B + 30 * M2, 2, small, KIND6, 11));
        uint32_t count = 0;
        assert(mr->planOwnerRemap(vr, 11, out, SYS, nullptr, 0, &count));
        assert(count == 36); // 2 huge + 30 split + 2 aliases + 2 small
        std::vector<nvgsp::ArenaRemapEntry> entries(count);
        assert(!mr->planOwnerRemap(vr, 11, out, SYS, entries.data(), count - 1, &count));
        assert(mr->planOwnerRemap(vr, 11, out, SYS, entries.data(), entries.size(), &count));
        const auto before = vr.q;
        for (uint32_t fail = 0; fail < count; ++fail) {
            vr.writes = 0; vr.failWrite = fail;
            assert(nvgsp::applyArenaRemap(vr, entries.data(), count) == nvgsp::ArenaRemapResult::RolledBack);
            assert(vr.q == before);
        }
        vr.writes = 0; vr.failWrite = 2; vr.failRestore = 3;
        assert(nvgsp::applyArenaRemap(vr, entries.data(), count) == nvgsp::ArenaRemapResult::RollbackFailed);
        vr.failWrite = vr.failRestore = -1;
        vr.q = before;
        // Stale journal refuses before the first write.
        vr.q[entries.back().address] ^= 8;
        vr.writes = 0;
        assert(nvgsp::applyArenaRemap(vr, entries.data(), count) == nvgsp::ArenaRemapResult::Stale);
        assert(!vr.writes);
        vr.q = before;
        assert(nvgsp::applyArenaRemap(vr, entries.data(), count) == nvgsp::ArenaRemapResult::Success);
        expectMapped(vr, *mr, B + 123, sys[0] + 123, KIND6 | SYS, 11);
        expectMapped(vr, *mr, B + 2 * M2 + 123, sys[2] + 123, KIND6 | SYS, 11);
        expectUnmapped(vr, *mr, B + M2 + 4 * K64);
        expectMapped(vr, *mr, B + M2 + 5 * K64, 0x60000000, VRAM, 12);
        expectMapped(vr, *mr, B + 20 * M2 + K64 + 99, sys[1] + 7 * K64 + 99, KIND6 | SYS, 11);
        expectMapped(vr, *mr, B + 30 * M2 + 0x1000 + 9, sys[2] + 0x5000 + 9, KIND6 | SYS, 11);
        assert(mr->planOwnerRemap(vr, 11, in, VRAM, entries.data(), entries.size(), &count));
        assert(nvgsp::applyArenaRemap(vr, entries.data(), count) == nvgsp::ArenaRemapResult::Success);
        expectMapped(vr, *mr, B + 123, target + 123, KIND6, 11);
        expectMapped(vr, *mr, B + 20 * M2 + K64 + 99, target + M2 + 7 * K64 + 99, KIND6, 11);
        expectMapped(vr, *mr, B + 30 * M2 + 0x1000 + 9, target + 2 * M2 + 0x5000 + 9, KIND6, 11);
        expectUnmapped(vr, *mr, B + M2 + 4 * K64);
        expectMapped(vr, *mr, B + M2 + 5 * K64, 0x60000000, VRAM, 12);
        assert(!mr->planOwnerRemap(vr, nvgsp::kArenaRawOwner, in, 0, nullptr, 0, &count));
        assert(!mr->planOwnerRemap(vr, 11, in, 0x10, nullptr, 0, &count));
        assert(!mr->planOwnerRemap(vr, 11, out, 0, nullptr, 0, &count)); // wrong source backing
        assert(mr->planOwnerRemap(vr, 13, out, 0, nullptr, 0, &count) && !count);
        mr->release(vr);
        std::puts("residency remap: PASS (36 rollback positions, aliases, holes, mixed page sizes)");
    }
    return 0;
}
