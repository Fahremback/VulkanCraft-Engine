#version 450

// Gaussian-splat cloud vertex shader. Each EditorVertex is one splat:
// position (location 0), color (location 2). Points are expanded to soft
// gaussian discs in the fragment shader via gl_PointSize.

layout (location = 0) in vec3 inPosition;
layout (location = 1) in vec3 inNormal;
layout (location = 2) in vec3 inColor;

layout (push_constant) uniform SplatPush {
    mat4 mvp;
    vec4 pointSize;   // x = px size, y = viewport height, z = opacity
} push;

layout (location = 0) out vec3 fragColor;
layout (location = 1) out float fragOpacity;

void main() {
    vec4 clip = push.mvp * vec4(inPosition, 1.0);
    gl_Position = clip;
    // Keep a roughly constant screen size: scale by perspective divide so
    // splats far from the camera shrink like real gaussian primitives.
    float w = max(abs(clip.w), 1e-6);
    float sizePx = push.pointSize.x * (push.pointSize.y / 1080.0) * (1.0 / w);
    gl_PointSize = clamp(sizePx, 1.0, 64.0);
    fragColor = inColor;
    fragOpacity = push.pointSize.z;
}
