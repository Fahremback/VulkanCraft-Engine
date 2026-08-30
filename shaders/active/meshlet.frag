#version 450

// Fragment shader for the real mesh-shader meshlet submission (Conta 2,
// item 1). It is deliberately self-contained (no descriptor bindings) so the
// mesh pipeline has the smallest possible surface: it lights the flat
// block-soup material with the sun diffuse + hemisphere ambient already used
// by the voxel path, driven purely by push constants. Visually equivalent to
// the indexed path's color-only markers, with zero external binding risk.

layout (location = 0) in vec4 fragColor;
layout (location = 1) in vec3 fragNormal;
layout (location = 2) in vec3 fragWorldPos;
layout (location = 0) out vec4 outColor;

layout (push_constant) uniform MeshletPush {
    mat4 mvp;
    vec4 sunDirection;
    vec4 sunColor;
    vec4 environment;
} push;

void main() {
    vec3 n = normalize(fragNormal);
    vec3 l = normalize(push.sunDirection.xyz);
    float ndl = clamp(dot(n, l), 0.0, 1.0);
    float daylight = clamp(push.environment.y, 0.0, 1.0);
    vec3 amb = mix(vec3(0.02, 0.02, 0.04), vec3(0.30, 0.32, 0.36), daylight);
    vec3 color = fragColor.rgb;
    vec3 lit = color * (amb + vec3(ndl) * push.sunColor.rgb);
    outColor = vec4(lit, 1.0);
}