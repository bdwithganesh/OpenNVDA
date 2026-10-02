#version 460
layout(push_constant) uniform P { vec4 rect; } pc;   // x0, y0, x1, y1 in NDC
layout(location = 0) out vec2 uv;
void main() {
    int k = gl_VertexIndex;
    float u = (k == 1 || k == 2 || k == 4) ? 1.0 : 0.0;
    float v = (k == 2 || k == 4 || k == 5) ? 1.0 : 0.0;
    uv = vec2(u, v);
    gl_Position = vec4(mix(pc.rect.x, pc.rect.z, u), mix(pc.rect.y, pc.rect.w, v), 0.0, 1.0);
}
