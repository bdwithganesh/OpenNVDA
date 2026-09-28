#include "../../NVGspCore/NVGspChainWait.hpp"
#include <cassert>
#include <cstdio>

int main() {
    // Parked chain (phase 33): never wait, with or without messages.
    assert(nvgsp::drainShouldExit(0, 0, true));
    assert(nvgsp::drainShouldExit(3, 0, true));
    assert(nvgsp::drainShouldExit(0, 4000, true));
    // Waiting on a phase and nothing collected yet: keep waiting,
    // however long it takes. (Regression guard: exiting here once parked
    // the chain at pb-backing / disp-sched and the watchdog tore it
    // down.)
    assert(!nvgsp::drainShouldExit(0, 0, false));
    assert(!nvgsp::drainShouldExit(0, 50, false));
    assert(!nvgsp::drainShouldExit(0, 150, false));
    assert(!nvgsp::drainShouldExit(0, 3999, false));
    // Messages collected: wait out the coalescing quantum for bursts...
    assert(!nvgsp::drainShouldExit(1, 0, false));
    assert(!nvgsp::drainShouldExit(1, 50, false));
    assert(!nvgsp::drainShouldExit(5, 50, false));
    // ...then exit instead of burning the remaining ~4 s tail.
    assert(nvgsp::drainShouldExit(1, 51, false));
    assert(nvgsp::drainShouldExit(1, 4000, false));
    assert(nvgsp::drainShouldExit(9, 51, false));
    std::printf("chain-wait: coalesce %u ms; empty-queue wait preserved\n",
                nvgsp::kDrainCoalesceMs);
    return 0;
}
