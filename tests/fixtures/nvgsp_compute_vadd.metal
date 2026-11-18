#include <metal_stdlib>
using namespace metal;

// Second hardware shader for the owned-client dispatch ABI: reads two
// CPU-written input buffers, writes a third. The host checks every element.
kernel void nvgsp_vadd(device const uint *a [[buffer(0)]],
                       device const uint *b [[buffer(1)]],
                       device uint *out [[buffer(2)]],
                       uint index [[thread_position_in_grid]]) {
    out[index] = a[index] * 3u + (b[index] ^ 0x9e3779b9u);
}
