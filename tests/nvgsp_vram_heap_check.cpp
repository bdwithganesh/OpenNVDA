#include "../drivers/NVGspCore/NVGspVramHeap.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>

// Reference: the pre-0.126.0 kext first fit (rescan all objects on every move).
struct Obj { uint64_t phys, bytes; bool live; };
static bool refFit(const std::vector<Obj> &o, uint64_t start, uint64_t end, uint64_t bytes,
                   uint64_t *out) {
    uint64_t cand = start;
    for (bool moved = true; moved && cand + bytes <= end;) {
        moved = false;
        for (const Obj &x : o)
            if (x.live && cand < x.phys + x.bytes && x.phys < cand + bytes) {
                cand = x.phys + x.bytes; moved = true;
            }
    }
    if (cand + bytes > end) return false;
    *out = cand;
    return true;
}

int main() {
    using namespace nvgsp;
    constexpr uint64_t M2 = 0x200000, G1 = 0x40000000;
    static VramHeap<64> h;
    uint64_t a = 0;
    // empty heap
    assert(h.fit(G1, G1 + 8 * M2, M2, &a) && a == G1);
    assert(!h.fit(G1, G1 + M2, 2 * M2, &a));
    assert(!h.fit(G1, G1, M2, &a) && !h.fit(G1, G1 + M2, 0, &a));
    // insert / overlap refusal / order
    assert(h.insert(G1 + 2 * M2, M2));
    assert(h.insert(G1, M2));
    assert(!h.insert(G1, M2) && !h.insert(G1 + M2, 2 * M2) && !h.insert(G1 + 2 * M2 - 1, 2));
    assert(h.count() == 2 && h.r[0].phys == G1 && h.r[1].phys == G1 + 2 * M2);
    // gaps: 1 x M2 at G1+M2, then open above G1+3*M2
    assert(h.fit(G1, G1 + 16 * M2, M2, &a) && a == G1 + M2);
    assert(h.fit(G1, G1 + 16 * M2, 2 * M2, &a) && a == G1 + 3 * M2);
    assert(!h.fit(G1, G1 + 4 * M2, 2 * M2, &a));
    // start inside a live range
    assert(h.fit(G1 + M2 / 2, G1 + 16 * M2, M2, &a) && a == G1 + M2);
    assert(h.fit(G1 + 2 * M2 + 1, G1 + 16 * M2, M2, &a) && a == G1 + 3 * M2);
    assert(!h.remove(G1 + M2) && h.remove(G1) && h.count() == 1);
    assert(h.fit(G1, G1 + 16 * M2, 2 * M2, &a) && a == G1);
    h.clear();
    // capacity
    for (uint32_t i = 0; i < 64; ++i) assert(h.insert(G1 + i * M2, M2));
    assert(!h.insert(G1 + 100 * M2, M2));
    h.clear();

    // randomized comparison against the old algorithm, incl. the window-first policy
    static VramHeap<512> H;
    std::vector<Obj> ref;
    srand(12345);
    const uint64_t heapEnd = G1 + 700 * M2, window = G1 + 64 * M2;
    uint32_t allocs = 0, fails = 0;
    for (int step = 0; step < 200000; ++step) {
        const bool doAlloc = ref.size() < 20 || rand() % 3 != 0;
        if (doAlloc && H.count() < 512) {
            const uint64_t bytes = (1 + (rand() % 8 == 0 ? rand() % 40 : rand() % 3)) * M2;
            const bool cpu = rand() % 4 == 0;
            uint64_t p1 = 0, p2 = 0;
            bool ok1, ok2;
            if (cpu) {
                ok1 = refFit(ref, G1, window, bytes, &p1);
                ok2 = H.fit(G1, window, bytes, &p2);
            } else {
                ok1 = refFit(ref, window, heapEnd, bytes, &p1) || refFit(ref, G1, heapEnd, bytes, &p1);
                ok2 = H.fit(window, heapEnd, bytes, &p2) || H.fit(G1, heapEnd, bytes, &p2);
            }
            assert(ok1 == ok2);
            if (ok1) {
                assert(p1 == p2);
                assert(H.insert(p2, bytes));
                ref.push_back({p1, bytes, true});
                ++allocs;
            } else {
                ++fails;
            }
        } else if (!ref.empty()) {
            const size_t k = rand() % ref.size();
            assert(H.remove(ref[k].phys));
            ref[k] = ref.back();
            ref.pop_back();
        }
        assert(H.count() == ref.size());
    }
    for (uint32_t i = 1; i < H.count(); ++i)
        assert(H.r[i - 1].phys + H.r[i - 1].bytes <= H.r[i].phys);
    printf("nvgsp_vram_heap_check: PASS (%u allocs, %u full)\n", allocs, fails);
    return 0;
}
