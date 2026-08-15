#version 450
layout(location=0) in vec3 inPosition;
layout(location=1) in vec3 inNormal;
layout(location=2) in vec4 inColor;
layout(location=3) in vec3 inUV;
layout(location=0) out vec3 shadowUV;
layout(push_constant) uniform PushConstants { mat4 mvp; vec4 cameraPos; vec4 sunDirection; vec4 sunColor; vec4 environment; } push;
vec3 projectShadow(vec3 worldPosition) {
    vec3 lightDir = normalize(push.environment.y > 0.03 ? push.sunDirection.xyz : -push.sunDirection.xyz);
    vec3 referenceUp = abs(lightDir.y) > .96 ? vec3(0,0,1) : vec3(0,1,0);
    vec3 right = normalize(cross(referenceUp, lightDir));
    vec3 up = normalize(cross(lightDir, right));
    const float snap = .125;
    vec2 center = floor(vec2(dot(push.cameraPos.xyz,right),dot(push.cameraPos.xyz,up))/snap+.5)*snap;
    vec2 plane = vec2(dot(worldPosition,right),dot(worldPosition,up))-center;
    vec2 projected = plane/512.0;
    float distortion = .16+.84*clamp(length(projected),0.0,1.0);
    vec2 warped = projected/distortion;
    float depth = clamp(.5-dot(worldPosition-push.cameraPos.xyz,lightDir)/1280.0,0.0,1.0);
    return vec3(warped,depth);
}
void main() {
    vec3 worldPosition = (push.mvp * vec4(inPosition,1.0)).xyz;
    vec3 projected=projectShadow(worldPosition);
    gl_Position = vec4(projected,1.0);
    shadowUV=inUV;
}
