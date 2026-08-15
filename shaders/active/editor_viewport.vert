#version 450

layout (location = 0) in vec3 inPosition;
layout (location = 1) in vec3 inNormal;
layout (location = 2) in vec3 inColor;

layout (push_constant) uniform PushConstants {
    mat4 mvp;
    vec4 color;   // rgb = tint; a = unused
} push;

layout (location = 0) out vec3 fragColor;
layout (location = 1) out vec3 fragNormal;

void main() {
    gl_Position = push.mvp * vec4(inPosition, 1.0);
    fragColor = inColor * push.color.rgb;
    fragNormal = inNormal;
}
