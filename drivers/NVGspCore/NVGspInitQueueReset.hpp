#pragma once
#include <stdint.h>
namespace nvgsp {
enum class InitQueueResetState { Parse, AwaitProducer, TimedOut };
// The first signed HS request resets the GSP processor's MMIO head to0
// temporarily after host consumed slot0. Only a pending, successful initial
// HS handoff enables this finite grace window. Never move either cursor or
// accept an empty packet as a firmware response.
inline InitQueueResetState initQueueResetState(uint64_t deadline, uint64_t now,
                                               uint32_t read, uint32_t write) {
    if (!deadline || read!=1 || write!=0) return InitQueueResetState::Parse;
    return now<deadline ? InitQueueResetState::AwaitProducer : InitQueueResetState::TimedOut;
}
}
