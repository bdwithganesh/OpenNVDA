#version 460
layout(push_constant) uniform P { mat4 mvp; } pc;
layout(location = 0) out vec3 color;
void main() {
    int i = gl_VertexIndex, f = i / 6, k = i % 6, ax = f / 2;
    float u = (k == 1 || k == 2 || k == 4) ? 1.0 : -1.0;
    float v = (k == 2 || k == 4 || k == 5) ? 1.0 : -1.0;
    float s = (f % 2 == 0) ? 1.0 : -1.0;
    vec3 p = ax == 0 ? vec3(s, u * s, v) : (ax == 1 ? vec3(v, s, u * s) : vec3(u * s, v, s));
    color = vec3(ax == 0 ? 1.0 : 0.15, ax == 1 ? 1.0 : 0.15, ax == 2 ? 1.0 : 0.15) * (s > 0.0 ? 1.0 : 0.55);
    gl_Position = pc.mvp * vec4(p * 0.5, 1.0);
}
