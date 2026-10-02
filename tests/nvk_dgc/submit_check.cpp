#include <cassert>
#include <cstdint>
#include <cstdio>
#include "../../drivers/NVGspCore/NVGspExec.hpp"

struct nvkmd_ctx_exec {
    uint64_t addr;
    uint32_t size_B;
    bool incomplete;
    bool no_prefetch;
};
#include "../../drivers/nvk-macos/dgc/nvkmd_macos_submit.h"

int main() {
    using namespace nvgsp;
    static_assert(kExecNoPrefetch == NVKMD_MACOS_EXEC_NO_PREFETCH, "ABI flag");
    static_assert(kExecMaxSegments == NVKMD_MACOS_EXEC_MAX, "ABI capacity");
    assert(execSegmentValid(0x2800200000ull, 256, 0));
    assert(execSegmentValid(0x2800200000ull, 256, kExecNoPrefetch));
    assert(!execSegmentValid(0x2800200001ull, 256, 0));
    assert(!execSegmentValid(0x10000000000ull, 256, 0));
    assert(!execSegmentValid(0xfffffffffcull, 2, 0));
    assert(!execSegmentValid(0x2800200000ull, 0, 0));
    assert(!execSegmentValid(0x2800200000ull, 1u << 21, 0));
    assert(!execSegmentValid(0x2800200000ull, 256, 2));
    assert(execEntryHigh(0x2800200000ull, 256, 0) == 0x00040028u);
    assert(execEntryHigh(0x2800200000ull, 256, kExecNoPrefetch) == 0x80040028u);
    assert(execEntryHigh(0xfffffffffcull, 1, kExecNoPrefetch) == 0x800004ffu);
    assert(execEntryHigh(0x2800200000ull, (1u << 21) - 1, 0) == 0x7ffffc28u);

    nvkmd_ctx_exec execs[130]{};
    assert(macos_exec_chains_valid(0, nullptr));
    assert(macos_exec_chains_valid(130, execs));
    assert(macos_exec_batch_count(130, execs, 64) == 64);
    execs[63].incomplete = true;
    execs[64].no_prefetch = true;
    assert(macos_exec_batch_count(130, execs, 64) == 63);
    assert(macos_exec_batch_count(67, execs + 63, 64) == 64);
    assert(macos_exec_chains_valid(130, execs));
    execs[62].incomplete = true;
    assert(macos_exec_batch_count(130, execs, 63) == 62);

    // An exact-capacity chain plus a pending acquire needs two batches.
    for (unsigned i = 0; i < 63; i++) execs[i].incomplete = true;
    execs[63].incomplete = false;
    assert(macos_exec_chains_valid(64, execs));
    assert(macos_exec_batch_count(64, execs, 63) == 0);
    assert(macos_exec_batch_count(64, execs, 64) == 64);
    execs[63].incomplete = true;
    assert(!macos_exec_chains_valid(64, execs));
    assert(!macos_exec_chains_valid(65, execs));

    // Exhaust every continuation pattern for small batches. Each produced
    // boundary must be a complete packet and every record is consumed once.
    unsigned patterns = 0;
    for (unsigned bits = 0; bits < (1u << 12); bits++) {
        for (unsigned i = 0; i < 12; i++) execs[i].incomplete = (bits >> i) & 1;
        execs[12].incomplete = false;
        assert(macos_exec_chains_valid(13, execs));
        for (unsigned capacity = 1; capacity <= 13; capacity++) {
            unsigned offset = 0;
            while (offset < 13) {
                unsigned n = macos_exec_batch_count(13 - offset, execs + offset, capacity);
                if (!n) break; // this test capacity cannot fit the next chain
                assert(n <= capacity && !execs[offset + n - 1].incomplete);
                for (unsigned later = n + 1; later <= capacity && later <= 13 - offset; later++)
                    assert(execs[offset + later - 1].incomplete);
                offset += n;
            }
        }
        patterns++;
    }
    std::printf("DGC submit: ABI packing and boundary cases PASS; %u continuation patterns PASS\n", patterns);
}
