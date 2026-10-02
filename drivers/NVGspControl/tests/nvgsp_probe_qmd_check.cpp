#include "../tools/nvgsp_probe_fill.hpp"
#include <cassert>
#include <cstdio>
uint64_t field(const uint32_t *q, unsigned hi, unsigned lo) {
    uint64_t v = 0;
    for (unsigned b = lo; b <= hi; ++b) v |= uint64_t((q[b / 32] >> (b % 32)) & 1) << (b - lo);
    return v;
}
int main() {
    uint32_t q[64]{};
    constexpr uint64_t program = 0x209c004000, cb = 0x209c005000;
    assert(nvgsp::buildProbeFillQmd(q, program, cb));
    assert(field(q, 583, 580) == 3);
    assert(field(q, 415, 384) == 4 && field(q, 431, 416) == 1 && field(q, 463, 448) == 1);
    assert(field(q, 607, 592) == 64 && field(q, 623, 608) == 1 && field(q, 639, 624) == 1);
    assert(field(q, 1584, 1536) == program && field(q, 1072, 1024) == cb);
    assert(field(q, 656, 648) == 24 && field(q, 640, 640) == 1 && field(q, 1087, 1075) == 1);
    assert(field(q, 561, 544) == 0 && field(q, 767, 763) == 0);
    assert(sizeof(nvgsp::kProbeFillCode) == 176);
    assert(!nvgsp::buildProbeFillQmd(q, program + 1, cb));
    assert(!nvgsp::buildProbeFillQmd(q, program, cb + 1));
    assert(!nvgsp::buildProbeFillQmd(q, 1ULL << 49, cb));
    assert(!nvgsp::buildProbeFillQmd(q, program, 1ULL << 49));
    assert(!nvgsp::buildProbeFillQmd(nullptr, program, cb));
    puts("probe QMD geometry, pointers, register/CB ABI and rejected alignment/range PASS (host only)");
}
