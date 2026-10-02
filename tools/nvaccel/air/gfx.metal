#include <metal_stdlib>
using namespace metal;
struct VOut { float4 pos [[position]]; float4 color; float2 uv; };
struct VIn { float2 pos [[attribute(0)]]; float4 color [[attribute(1)]]; };
vertex VOut v_main(VIn in [[stage_in]], constant float2 &offset [[buffer(1)]], uint vid [[vertex_id]]) {
    VOut o;
    o.pos = float4(in.pos + offset, 0.0f, 1.0f);
    o.color = in.color;
    o.uv = float2(vid & 1, vid >> 1);
    return o;
}
fragment float4 f_main(VOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
    return in.color * t.sample(s, in.uv);
}
vertex float4 v_pull(const device float2 *p [[buffer(0)]], uint vid [[vertex_id]]) { return float4(p[vid], 0, 1); }
fragment half4 f_flat(constant half4 &c [[buffer(0)]]) { return c; }
vertex float4 v_depth(const device float4 *p [[buffer(0)]], uint vid [[vertex_id]]) { return p[vid]; }
