#version 450
layout(location = 0) in vec4 clipPos;
layout(location = 0) out vec4 out_color;

void main() {
    float zCoord = clipPos.z / clipPos.w;
    int iz = int(round(clamp(zCoord, 0.0, 1.0) * 16777216.0));
    float d = float(iz) / 16777216.0;
    gl_FragDepth = d;
    out_color = vec4(d, 0.5, 0.25, 1.0);
}
