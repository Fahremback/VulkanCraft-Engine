#version 450

// Entity pick shader (reads back an encoded entity id from fragColor).
// Declares the same SceneLights UBO as editor_viewport.frag because both
// pipelines share m_scenePipelineLayout — the layout must stay compatible
// even though this shader never reads the lights.

layout (location = 0) in vec3 fragColor;
layout (location = 1) in vec3 fragNormal;
layout (location = 0) out vec4 outColor;

layout (set = 0, binding = 0) uniform SceneLights {
    vec4 cameraPosition;
    vec4 sunDirection;
    vec4 sunColor;
    mat4 sunViewProj;
    vec4 shadowParams;
    vec4 pointLightPos[8];
    vec4 pointLightColor[8];
    vec4 spotLightPos[4];
    vec4 spotLightDir[4];
    vec4 spotLightParams[4];
    vec4 spotLightColor[4];
    vec4 areaLightPos[4];
    vec4 areaLightNormal[4];
    vec4 areaLightHalf[4];
    vec4 areaLightColor[4];
    mat4 sunCascadeVP[4];
    vec4 sunCascadeSplits;
    vec4 cameraForward;
} lights;

void main() {
    outColor = vec4(fragColor, 1.0);
}
