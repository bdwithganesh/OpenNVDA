#version 460
layout(location = 0) out vec3 color;
void main() {
    int i = gl_VertexIndex;
    vec2 p = i == 0 ? vec2(0.0, -0.8) : (i == 1 ? vec2(0.8, 0.8) : vec2(-0.8, 0.8));
    color = vec3(i == 0 ? 1.0 : 0.0, i == 1 ? 1.0 : 0.0, i == 2 ? 1.0 : 0.0);
    gl_Position = vec4(p, 0.0, 1.0);
}
