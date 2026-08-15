#version 450

layout(location = 0) in vec4 inPositionSize;
layout(location = 1) in vec4 inNeighborHeights;
layout(location = 2) in vec4 inColor;
layout(location = 3) in vec4 inMaterial;

layout(location = 0) out vec3 shadowUV;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    vec4 cameraPos;
    vec4 sunDirection;
    vec4 sunColor;
    vec4 environment;
} push;

vec3 projectShadow(vec3 worldPosition) {
    vec3 lightDir = normalize(push.environment.y > 0.03
        ? push.sunDirection.xyz : -push.sunDirection.xyz);
    vec3 referenceUp = abs(lightDir.y) > 0.96 ? vec3(0, 0, 1) : vec3(0, 1, 0);
    vec3 right = normalize(cross(referenceUp, lightDir));
    vec3 up = normalize(cross(lightDir, right));
    const float snap = 0.125;
    vec2 center = floor(vec2(dot(push.cameraPos.xyz, right),
                             dot(push.cameraPos.xyz, up)) / snap + 0.5) * snap;
    vec2 plane = vec2(dot(worldPosition, right), dot(worldPosition, up)) - center;
    vec2 projected = plane / 512.0;
    float distortion = 0.16 + 0.84 * clamp(length(projected), 0.0, 1.0);
    vec2 warped = projected / distortion;
    float depth = clamp(0.5 - dot(worldPosition - push.cameraPos.xyz, lightDir) /
                                  1280.0, 0.0, 1.0);
    return vec3(warped, depth);
}

void main() {
    float x = inPositionSize.x;
    float y = inPositionSize.y;
    float z = inPositionSize.z;
    float size = inPositionSize.w;
    float smoothness = clamp(inMaterial.z, 0.0, 1.0);

    vec3 sw = vec3(x, y, z);
    vec3 se = vec3(x + size, inNeighborHeights.y, z);
    vec3 nw = vec3(x, inNeighborHeights.w, z + size);
    vec3 ne = vec3(x + size, inMaterial.w, z + size);
    vec3 blockCorners[6] = vec3[6](
        vec3(nw.x, y, nw.z), vec3(ne.x, y, ne.z), vec3(se.x, y, se.z),
        vec3(nw.x, y, nw.z), vec3(se.x, y, se.z), sw);
    vec3 smoothCorners[6] = vec3[6](nw, ne, se, nw, se, sw);
    vec3 position = mix(blockCorners[gl_VertexIndex],
                        smoothCorners[gl_VertexIndex], smoothness);
    vec3 worldPosition = (push.mvp * vec4(position, 1.0)).xyz;

    gl_Position = vec4(projectShadow(worldPosition), 1.0);
    // Solid terrain never alpha-tests; this keeps shadow.frag descriptor-
    // compatible while avoiding an albedo sample for every FAR fragment.
    shadowUV = vec3(0.0, 0.0, -1.0);
}
