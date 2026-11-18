#include <metal_stdlib>
using namespace metal;

// First hardware shader diagnostic: every thread writes a distinct value.
// The output buffer is initialized to zero before dispatch and read back
// independently through PRAMIN; the CPU never writes the expected output.
kernel void nvgsp_write32(device uint *output [[buffer(0)]],
                          uint index [[thread_position_in_grid]]) {
    output[index] = (index * 3u + 7u) ^ 0x6ad0c531u;
}
