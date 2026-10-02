#pragma once
#include <stddef.h>
#include <stdint.h>

namespace nvgsp {
// NVIDIA 570.144 nvkms-evo3.c: EvoSetupIdentityOutputLutC5 and
// EvoSetupIdentityBaseLutC3. Four zero VSS header entries, 1024 samples,
// then repeat sample 1023. OLUT is fixed-point; ILUT is FP16 of i/1024.
constexpr size_t kIdentityLutWords = (4 + 1025) * 2;
constexpr uint64_t kOlutScratchDelta = 0xff10000;
constexpr uint64_t kIlutScratchDelta = 0xff14000;

inline bool buildIdentityDisplayLut(uint32_t *words, size_t count, bool input) {
    if (!words || count < kIdentityLutWords) return false;
    for (size_t i = 0; i < kIdentityLutWords; ++i) words[i] = 0;
    for (uint32_t i = 0; i < 1025; ++i) {
        const uint32_t sample = i < 1024 ? i : 1023;
        uint16_t value = static_cast<uint16_t>(sample << 6);
        if (input) {
            value = 0;
            if (sample) {
                const uint32_t e = 31U - static_cast<uint32_t>(__builtin_clz(sample));
                const uint32_t mantissa = ((sample << 10) >> e) & 0x3ff;
                value = static_cast<uint16_t>(((e + 5) << 10) | mantissa);
            }
        }
        words[(4 + i)*2] = value | (uint32_t(value) << 16);
        words[(4 + i)*2+1] = value;
    }
    return true;
}
static_assert(kOlutScratchDelta + 0x3000 <= kIlutScratchDelta,
              "input/output LUT DMA ranges must not overlap");
}
