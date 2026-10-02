#pragma once

#include <stdint.h>

namespace nvgsp {

// 0.95.0: status-drain exit policy for pollStatusLocked.
//
// The chain's ~9 min boot cost is almost entirely tail waiting: GSP usually
// answers during the daemon's 0.2 s inter-poll gap, so the drain finds the
// reply on its first snapshot and then burns the remaining ~4000 iterations
// confirming silence. Exiting early is safe ONLY after at least one message
// was collected this call: an awaiting phase with zero arrivals must keep
// waiting exactly like before (0.90/0.91 parked the chain by exiting on an
// empty queue). A short coalescing quantum after the last message still
// catches multi-message bursts (reply + follow-up events) in one drain;
// anything later is picked up by the daemon's next poll with no re-entry
// side effect (reply flags are poll-local; persistent phases are
// unconditional or idempotent).
//
// collected: messages consumed so far this call; idleMs: consecutive empty
// snapshots since the last message; parked: postInitPhase_ == 33.
// Returns true when the drain loop should stop waiting.
// 0.146.8: was 50 ms. Every chain step (a few ms of GSP reply) waited
// that long, ~35 steps before the window took the screen: the boot logo
// progress pause. 3 ms still coalesces a reply with its follow-ups.
constexpr uint32_t kDrainCoalesceMs = 3;

inline bool drainShouldExit(uint32_t collected, uint32_t idleMs, bool parked) {
    if (parked) return true;
    return collected > 0 && idleMs > kDrainCoalesceMs;
}

}  // namespace nvgsp
