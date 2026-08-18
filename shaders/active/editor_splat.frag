#version 450

// Gaussian-splat fragment shader: radial soft falloff with premultiplied
// alpha (blend: ONE / ONE_MINUS_SRC_ALPHA), depth-tested but not written.

layout (location = 0) in vec3 fragColor;
layout (location = 1) in float fragOpacity;

layout (location = 0) out vec4 outColor;

void main() {
    vec2 uv = gl_PointCoord * 2.0 - 1.0;
    float d2 = dot(uv, uv);
    if (d2 > 1.0) discard;
    float a = exp(-2.0 * d2) * fragOpacity;
    vec3 c = fragColor * a;
    outColor = vec4(c, a);
}
