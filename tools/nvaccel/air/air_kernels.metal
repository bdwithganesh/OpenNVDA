// Kernels for metal_air_test: built to a metallib by Apple's own compiler,
// so the driver has to run real AIR, not our MetalSL subset.
#include <metal_stdlib>
using namespace metal;

kernel void k_vadd(device const float *a [[buffer(0)]], device const float *b [[buffer(1)]],
                   device float *c [[buffer(2)]], uint i [[thread_position_in_grid]]) {
    c[i] = a[i] + b[i] * 2.0f;
}

// loop with a phi, integer math, early continue
kernel void k_collatz(device const uint *in [[buffer(0)]], device uint *steps [[buffer(1)]],
                      uint i [[thread_position_in_grid]]) {
    uint n = in[i], s = 0;
    while (n != 1 && s < 1000) {
        n = (n & 1) ? 3 * n + 1 : n / 2;
        ++s;
    }
    steps[i] = s;
}

// threadgroup memory + barriers: sum of 256 values per group
kernel void k_reduce(device const float *in [[buffer(0)]], device float *out [[buffer(1)]],
                     uint gid [[thread_position_in_grid]], uint lid [[thread_position_in_threadgroup]],
                     uint grp [[threadgroup_position_in_grid]]) {
    threadgroup float tmp[256];
    tmp[lid] = in[gid];
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint s = 128; s > 0; s >>= 1) {
        if (lid < s) tmp[lid] += tmp[lid + s];
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    if (lid == 0) out[grp] = tmp[0];
}

// device atomics: histogram of 16 bins
kernel void k_hist(device const uint *in [[buffer(0)]], device atomic_uint *bins [[buffer(1)]],
                   uint i [[thread_position_in_grid]]) {
    atomic_fetch_add_explicit(&bins[in[i] & 15], 1, memory_order_relaxed);
}

// constant-address-space table (goes through the cdata block)
constant float kTable[8] = {1.5f, -2.0f, 3.25f, 0.5f, 7.0f, -1.25f, 9.5f, 4.0f};
kernel void k_table(device const uint *idx [[buffer(0)]], device float *out [[buffer(1)]],
                    uint i [[thread_position_in_grid]]) {
    out[i] = kTable[idx[i] & 7] * 2.0f;
}

// simd reduction
kernel void k_simd(device const int *in [[buffer(0)]], device int *out [[buffer(1)]],
                   uint i [[thread_position_in_grid]], uint lane [[thread_index_in_simdgroup]]) {
    int s = simd_sum(in[i]);
    if (lane == 0) out[i / 32] = s;
}

// math intrinsics, vectors, struct in constant buffer, 2D grid
struct Params { float2 scale; float bias; uint width; };
kernel void k_math(constant Params &p [[buffer(0)]], device float4 *out [[buffer(1)]],
                   uint2 pos [[thread_position_in_grid]]) {
    float2 uv = float2(pos) * p.scale;
    float4 r;
    r.x = sqrt(uv.x + 1.0f) + p.bias;
    r.y = clamp(sin(uv.y), -0.5f, 0.5f);
    r.z = mix(uv.x, uv.y, 0.25f);
    r.w = dot(float3(uv, 1.0f), float3(0.5f, 0.25f, 2.0f));
    out[pos.y * p.width + pos.x] = r;
}

// hardware textures: read + write (format conversion by the texture unit)
kernel void k_tex_copy(texture2d<float, access::read> src [[texture(0)]],
                       texture2d<float, access::write> dst [[texture(1)]],
                       uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;
    float4 c = src.read(gid);
    dst.write(float4(c.b, c.g, c.r, 1.0f - c.a), gid);
}

// sampling with an argument sampler and a constexpr one (repeat, pixel-centre math)
kernel void k_tex_sample(texture2d<float> src [[texture(0)]], sampler s [[sampler(0)]],
                         device float4 *out [[buffer(0)]], uint2 gid [[thread_position_in_grid]]) {
    constexpr sampler rep(filter::nearest, address::repeat);
    float2 uv = (float2(gid) + 0.5f) / 8.0f;
    out[gid.y * 16 + gid.x] = src.sample(s, uv) * 0.5f + src.sample(rep, uv + 1.0f) * 0.25f;
}

// a real call: noinline helper returning a struct from a loop
struct Pair { float a; int b; };
__attribute__((noinline)) Pair helper_sum(float x, int n) {
    Pair p = {0.0f, 0};
    for (int i = 0; i < n; ++i) { p.a += x * float(i); p.b += i; }
    return p;
}
kernel void k_call(device float *o [[buffer(0)]], device int *oi [[buffer(1)]], uint i [[thread_position_in_grid]]) {
    Pair p = helper_sum(float(i), int(i % 7));
    o[i] = p.a;
    oi[i] = p.b;
}

// function constants: a bool that enables an optional buffer, a float, an
// int that may be left undefined
constant bool kfUseB [[function_constant(0)]];
constant float kfScale [[function_constant(1)]];
constant int kfAdd [[function_constant(2)]];
kernel void k_fconst(device float *a [[buffer(0)]], device const float *b [[buffer(1), function_constant(kfUseB)]],
                     uint i [[thread_position_in_grid]]) {
    float v = a[i] * kfScale;
    if (kfUseB) v += b[i];
    if (is_function_constant_defined(kfAdd)) v += float(kfAdd);
    a[i] = v;
}

// argument buffer (Metal 3 style: the app stores gpuAddress / gpuResourceID)
struct ArgTable {
    device float *out;
    const device float *in;
    texture2d<float> tex;
    float scale;
};
kernel void k_argbuf(constant ArgTable &t [[buffer(0)]], uint i [[thread_position_in_grid]]) {
    const float4 c = t.tex.read(uint2(i % 4, 0));
    t.out[i] = t.in[i] * t.scale + c.x;
}

// block-linear textures: mip levels, array layers, 3D slices, cube faces
kernel void k_tex_kinds(texture2d<float> mips [[texture(0)]], texture2d_array<float> arr [[texture(1)]],
                        texture3d<float> vol [[texture(2)]], texturecube<float> cube [[texture(3)]],
                        device float4 *out [[buffer(0)]], uint i [[thread_position_in_grid]]) {
    constexpr sampler s(filter::nearest);
    const float3 dirs[6] = {float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0), float3(0, -1, 0), float3(0, 0, 1), float3(0, 0, -1)};
    if (i < 4) out[i] = mips.read(uint2(1, 1) >> i, i);
    else if (i < 8) out[i] = arr.read(uint2(2, 3), i - 4);
    else if (i < 12) out[i] = vol.read(uint3(2, 3, i - 8));
    else if (i < 18) out[i] = cube.sample(s, dirs[i - 12]);
}
kernel void k_tex3d_write(texture3d<float, access::write> vol [[texture(0)]], uint3 g [[thread_position_in_grid]]) {
    vol.write(float4(g.x, g.y, g.z, 1.0f), g);
}
