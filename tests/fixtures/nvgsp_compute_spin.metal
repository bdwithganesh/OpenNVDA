#include <metal_stdlib>
using namespace metal;

// Hang probe for owned-client recovery: ~10^12 dependent iterations per
// thread, far beyond the 5 s submit deadline, with no faulting access.
kernel void nvgsp_spin(device uint *out [[buffer(0)]],
                       uint index [[thread_position_in_grid]]) {
    uint acc = index;
    for (uint a = 0; a < 0xfffffff0u; a++)
        for (uint b = 0; b < 256u; b++)
            acc = acc * 1664525u + out[(acc ^ b) & 63u];
    out[index] = acc;
}
