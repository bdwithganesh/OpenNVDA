#include "../../NVGspCore/NVGspChainWait.hpp"
#include <cassert>
#include <cstdio>

int main() {
    // Parked chain (phase 33): never wait, with or without messages.
    assert(nvgsp::drainShouldExit(0, 0, true));
    assert(nvgsp::drainShouldExit(3, 0, true));
    assert(nvgsp::drainShouldExit(0, 4000, true));
    // Awaiting phase, nothing collected yet: keep waiting, however long.
    // (This is the 0.90/0.91 regression guard: exiting here parked the
    // chain at pb-backing / disp-sched and tore it down via watchdog.)
    assert(!nvgsp::drainShouldExit(0, 0, false));
    assert(!nvgsp::drainShouldExit(0, 50, false));
    assert(!nvgsp::drainShouldExit(0, 150, false));
    assert(!nvgsp::drainShouldExit(0, 3999, false));
    // Messages collected: wait out the coalescing quantum for bursts...
    assert(!nvgsp::drainShouldExit(1, 0, false));
    assert(!nvgsp::drainShouldExit(1, nvgsp::kDrainCoalesceMs, false));
    assert(!nvgsp::drainShouldExit(5, nvgsp::kDrainCoalesceMs, false));
    // ...then exit instead of burning the remaining ~4 s tail.
    assert(nvgsp::drainShouldExit(1, nvgsp::kDrainCoalesceMs + 1, false));
    assert(nvgsp::drainShouldExit(1, 4000, false));
    assert(nvgsp::drainShouldExit(9, nvgsp::kDrainCoalesceMs + 1, false));
    std::printf("chain-wait: coalesce %u ms; empty-queue wait preserved\n",
                nvgsp::kDrainCoalesceMs);
    return 0;
}
