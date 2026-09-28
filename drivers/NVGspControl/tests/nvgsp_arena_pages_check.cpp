#include "../../NVGspCore/NVGspArenaPages.hpp"
#include <cassert>
#include <cstdio>

int main() {
    static_assert(nvgsp::kArenaPages == 16384, "32 GiB / 2 MiB");
    uint32_t first = 0, count = 0;

    // Range validation: aligned, non-empty, inside [base, end).
    assert(nvgsp::arenaRange(0x2800000000ULL, 0x200000, &first, &count));
    assert(first == 0 && count == 1);
    assert(nvgsp::arenaRange(0x2800400000ULL, 0x600000, &first, &count));
    assert(first == 2 && count == 3);
    assert(nvgsp::arenaRange(0x2FFFE00000ULL, 0x200000, &first, &count));
    assert(first == 16383 && count == 1);
    assert(!nvgsp::arenaRange(0x2800000000ULL, 0, &first, &count));
    assert(!nvgsp::arenaRange(0x2800100000ULL, 0x200000, &first, &count));
    assert(!nvgsp::arenaRange(0x2800000000ULL, 0x100000, &first, &count));
    assert(!nvgsp::arenaRange(0x27FFE00000ULL, 0x200000, &first, &count));
    assert(!nvgsp::arenaRange(0x2FFFE00000ULL, 0x400000, &first, &count));
    assert(!nvgsp::arenaRange(0x3000000000ULL, 0x200000, &first, &count));
    // No wrap-around on huge sizes.
    assert(!nvgsp::arenaRange(0x2800000000ULL, 0xFFFFFFFFFFE00000ULL, &first, &count));
    assert(!nvgsp::arenaRange(0x2800000000ULL, 0x200000, nullptr, &count));
    assert(nvgsp::arenaPageVa(3) == 0x2800600000ULL);

    // Runs: object 7 bound at pages 1-2 and 5, object 9 at 3, raw at 4.
    static uint16_t t[nvgsp::kArenaPages] = {};
    nvgsp::arenaMark(t, 1, 2, 7);
    nvgsp::arenaMark(t, 3, 1, 9);
    nvgsp::arenaMark(t, 4, 1, nvgsp::kArenaRawOwner);
    nvgsp::arenaMark(t, 5, 1, 7);
    assert(nvgsp::arenaNextRun(t, 7, 0, &first, &count) && first == 1 && count == 2);
    assert(nvgsp::arenaNextRun(t, 7, first + count, &first, &count) && first == 5 && count == 1);
    assert(!nvgsp::arenaNextRun(t, 7, first + count, &first, &count));
    assert(nvgsp::arenaNextRun(t, 9, 0, &first, &count) && first == 3 && count == 1);
    assert(!nvgsp::arenaNextRun(t, 0, 0, &first, &count));   // 0 = unbound, never a run
    // Rebinding page 2 to object 9 splits object 7's first run.
    nvgsp::arenaMark(t, 2, 1, 9);
    assert(nvgsp::arenaNextRun(t, 7, 0, &first, &count) && first == 1 && count == 1);
    assert(nvgsp::arenaNextRun(t, 9, 0, &first, &count) && first == 2 && count == 2);
    // Run reaching the last page; marking past the end is clipped.
    nvgsp::arenaMark(t, nvgsp::kArenaPages - 2, 5, 11);
    assert(nvgsp::arenaNextRun(t, 11, 0, &first, &count) &&
           first == nvgsp::kArenaPages - 2 && count == 2);
    // Freeing clears.
    nvgsp::arenaMark(t, first, count, 0);
    assert(!nvgsp::arenaNextRun(t, 11, 0, &first, &count));

    std::puts("nvgsp_arena_pages_check: PASS");
    return 0;
}
