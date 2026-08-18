#version 450

// Samples the probe cubemap with the reflected view ray.

layout (location = 0) in vec3 vWorldPos;
layout (location = 1) in vec3 vNormal;

layout (push_constant) uniform EnvPush {
    mat4 mvp;
    mat4 model;
    vec4 camPos;   // xyz = camera world position
} push;

layout (set = 0, binding = 0) uniform samplerCube envMap;

layout (location = 0) out vec4 outColor;

void main() {
    vec3 n = normalize(vNormal);
    vec3 v = normalize(vWorldPos - push.camPos.xyz);
    vec3 r = reflect(-v, n);
    vec3 col = texture(envMap, r).rgb;
    // Fade the reflection with fresnel so the sphere reads as a preview.
    float fres = pow(1.0 - max(dot(n, -v), 0.0), 2.0);
    outColor = vec4(mix(col * 0.35, col, fres), 1.0);
}
