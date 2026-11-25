#version 450
layout(location = 0) out vec4 clipPos;

void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    float z = p.x * 0.25 + p.y * 0.125 + 0.1;
    gl_Position = vec4(p * 2.0 - 1.0, z, 1.0);
    clipPos = gl_Position;
}
