#pragma once

#include <stdint.h>

namespace nvgsp {

// When pollStatusLocked should stop draining the status queue.
//
// The chain's ~9 min boot time was nearly all tail waiting: GSP usually
// answers in the daemon's 0.2 s gap between polls, so the drain finds the
// reply in its first snapshot and then burns ~4000 more iterations
// confirming nothing else is coming. Leaving early is safe ONLY once at
// least one message got collected in this call; a phase that's waiting with
// nothing arrived must keep waiting exactly like before (leaving on an
// empty queue once parked the chain). A short quantum after the last
// message still catches bursts (reply + follow-up events) in one drain, and
// anything later gets picked up by the daemon's next poll with no side
// effects (reply flags are per poll; persistent phases are unconditional or
// idempotent).
//
// collected: messages eaten so far in this call; idleMs: empty snapshots in
// a row since the last message; parked: postInitPhase_ == 33.
// Returns true when the drain loop should stop waiting.
// was 50 ms. Every chain step (a few ms of GSP reply) waited
// that long, ~35 steps before the window took the screen: the boot logo
// progress pause. 3 ms still coalesces a reply with its follow-ups.
constexpr uint32_t kDrainCoalesceMs = 3;

inline bool drainShouldExit(uint32_t collected, uint32_t idleMs, bool parked) {
    if (parked) return true;
    return collected > 0 && idleMs > kDrainCoalesceMs;
}

}  // namespace nvgsp
