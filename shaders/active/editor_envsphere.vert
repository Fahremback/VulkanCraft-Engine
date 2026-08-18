#version 450

// Reflective sphere used to preview an environment probe's captured cubemap.

layout (location = 0) in vec3 inPosition;
layout (location = 1) in vec3 inNormal;

layout (push_constant) uniform EnvPush {
    mat4 mvp;
    mat4 model;
    vec4 camPos;   // xyz = camera world position
} push;

layout (location = 0) out vec3 vWorldPos;
layout (location = 1) out vec3 vNormal;

void main() {
    vec4 world = push.model * vec4(inPosition, 1.0);
    gl_Position = push.mvp * vec4(inPosition, 1.0);
    vWorldPos = world.xyz;
    vNormal = mat3(push.model) * inNormal;
}
