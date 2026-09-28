// Host test for NVGspArenaMap.hpp: binds/unbinds through a fake VRAM and
// checks every translation with an independent MMU v2 page walker.
#include "../../NVGspCore/NVGspArenaMap.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>

struct FakeVram {
    std::map<uint64_t, uint64_t> q;
    uint64_t nextChunk = 0x300000000ULL;
    int chunks = 0;
    bool read64(uint64_t a, uint64_t *v) { assert(!(a & 7)); auto it = q.find(a); *v = it == q.end() ? 0 : it->second; return true; }
    bool write64(uint64_t a, uint64_t v) { assert(!(a & 7)); q[a] = v; return true; }
    bool zero(uint64_t a, uint64_t n) { for (uint64_t i = 0; i < n; i += 8) q.erase(a + i); return true; }
    bool allocChunk(uint64_t *p) { *p = nextChunk; nextChunk += 0x200000; ++chunks; return true; }
    void *allocOwners(uint32_t n) { return std::malloc(n); }
    void freeOwners(void *p, uint32_t) { std::free(p); }
};
using Map = nvgsp::ArenaMap<FakeVram>;

// Independent walker: returns true + phys/flags if va is mapped.
static bool walk(FakeVram &v, uint64_t tables, uint64_t va, uint64_t *phys, uint64_t *flags) {
    const uint64_t r = (va - nvgsp::kArenaBase) >> 21;
    uint64_t e0 = 0, e1 = 0;
    v.read64(tables + (r / 256) * 4096 + (r % 256) * 16, &e0);
    v.read64(tables + (r / 256) * 4096 + (r % 256) * 16 + 8, &e1);
    assert(e1 == 0);                              // never a small-page table
    if (e0 & 1) {                                  // 2 MiB PTE
        *phys = ((e0 & nvgsp::kPteAddrMask) << 4) + (va & 0x1fffff);
        *flags = e0 & ~nvgsp::kPteAddrMask;
        return true;
    }
    if (!e0) return false;
    assert((e0 & 0xf) == 2);                       // VRAM PDE
    const uint64_t lpt = (e0 & ~0xfULL) << 4;
    assert(!(lpt & 0xff));
    uint64_t pte = 0;
    v.read64(lpt + ((va >> 16) & 31) * 8, &pte);
    if (!(pte & 1)) { assert(pte == nvgsp::kLptInvalid); return false; }
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

int main() {
    const uint64_t B = nvgsp::kArenaBase, M2 = 0x200000, K64 = 0x10000;
    const uint64_t VRAM = 0, SYS = 0xC, KIND6 = 6ULL << 56;
    FakeVram v;
    std::unique_ptr<Map> mp(new Map());            // value-init: zeroed
    Map &m = *mp;
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
    // owner 4's page survives, the split region keeps only it
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
    assert(e0 == 0);

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
    std::puts("nvgsp_arena_map_check: PASS");
    return 0;
}
