#pragma once
#include <stdint.h>
// Fixed fill3 shader: output[i] = i*3+1, workgroup64, no SLM/shared memory.
// Compiled artifact tools/nakc/kernels/fill3.nak, SHA256:
// 64f5a5f031cab01410fcbe2f7827faa31c973d53d8c5720b2efda3e84daaa417
namespace nvgsp {
constexpr uint32_t kProbeFillCode[] = {
    0x000079c3u, 0x00000000u, 0x00002500u, 0x000e0200u,
    0x00007919u, 0x00000000u, 0x00002100u, 0x000e4200u,
    0x00027c02u, 0x00000000u, 0x08000f00u, 0x001fc800u,
    0x02007211u, 0x00000000u, 0x000e30ffu, 0x002fc600u,
    0x00017802u, 0x00000001u, 0x00000f00u, 0x000fc200u,
    0x00027816u, 0x0000bbbbu, 0x000000ffu, 0x000fc200u,
    0x00047a11u, 0x00000000u, 0x000010ffu, 0x000fc600u,
    0x00017824u, 0x00000003u, 0x000e0001u, 0x000fc200u,
    0x00057a11u, 0x00000100u, 0x000f1402u, 0x000fca00u,
    0x04007386u, 0x00000001u, 0x0010e900u, 0x0001c400u,
    0x0000794du, 0x00000000u, 0x03800000u, 0x001fc200u,
};
inline void probeQset(uint32_t *q, unsigned hi, unsigned lo, uint64_t value) {
    for (unsigned b = lo; b <= hi; ++b) {
        const uint32_t mask = 1u << (b % 32);
        if ((value >> (b - lo)) & 1) q[b / 32] |= mask; else q[b / 32] &= ~mask;
    }
}
// QMD v03_00 matching the proven nvrun NAK launch path, fixed256 threads.
inline bool buildProbeFillQmd(uint32_t *q, uint64_t program, uint64_t cb0) {
    if (!q || !program || !cb0 || (program & 255) || (cb0 & 15) ||
        program >= (1ULL << 49) || cb0 >= (1ULL << 49)) return false;
    for (unsigned i = 0; i < 64; ++i) q[i] = 0;
    probeQset(q, 583, 580, 3); probeQset(q, 378, 378, 1); probeQset(q, 134, 134, 1);
    probeQset(q, 415, 384, 4); probeQset(q, 431, 416, 1); probeQset(q, 463, 448, 1);
    probeQset(q, 607, 592, 64); probeQset(q, 623, 608, 1); probeQset(q, 639, 624, 1);
    probeQset(q, 1567, 1536, uint32_t(program)); probeQset(q, 1584, 1568, program >> 32);
    probeQset(q, 656, 648, 24);
    probeQset(q, 567, 562, 1); probeQset(q, 662, 657, 1); probeQset(q, 574, 569, 26);
    probeQset(q, 640, 640, 1);
    probeQset(q, 1055, 1024, uint32_t(cb0)); probeQset(q, 1072, 1056, cb0 >> 32);
    probeQset(q, 1087, 1075, 1);
    return true;
}
}
