#version 450

layout (location = 0) in vec3 inPosition;
layout (location = 1) in vec3 inNormal;
layout (location = 2) in vec3 inColor;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 color;      // rgb = tint; a = unused
    vec4 fogParams;  // x=density, y=start, z=heightFog, w=unused
    vec4 fogColor;   // xyz=fog color, w=unused
    mat4 model;      // world transform: fragWorldPos = model * inPosition
} push;

layout (location = 0) out vec3 fragColor;
layout (location = 1) out vec3 fragNormal;
layout (location = 2) out vec3 fragWorldPos;

void main() {
    gl_Position = push.mvp * vec4(inPosition, 1.0);
    fragColor = inColor * push.color.rgb;
    fragNormal = inNormal;
    // World position (not local): fog distance and rim lighting in the
    // fragment stage use real world coordinates, correct for transformed
    // entities (blocks, characters, meshes placed in the scene).
    fragWorldPos = (push.model * vec4(inPosition, 1.0)).xyz;
}
