#version 450

layout(location = 0) in vec4 inPositionSize;
layout(location = 1) in vec4 inNeighborHeights;
layout(location = 2) in vec4 inColor;
layout(location = 3) in vec4 inMaterial;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec3 fragUV;
layout(location = 3) out vec3 fragWorldPos;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    vec4 cameraPos;
    vec4 sunDirection;
    vec4 sunColor;
    vec4 environment;
} push;

const ivec2 QUAD[6] = ivec2[6](
    ivec2(0, 1), ivec2(1, 1), ivec2(1, 0),
    ivec2(0, 1), ivec2(1, 0), ivec2(0, 0)
);

void emitVertex(vec3 position, vec3 normal, vec2 uv, float layer) {
    gl_Position = push.mvp * vec4(position, 1.0);
    // Alpha is an engine-only FAR marker and carries the source-cell size so
    // the fragment stage can retain material detail for the first LODs.
    fragColor = vec4(inColor.rgb, 2.0 + max(inPositionSize.w, 1.0));
    fragNormal = normal;
    fragUV = vec3(uv, layer);
    fragWorldPos = position;
}

void main() {
    float x = inPositionSize.x;
    float y = inPositionSize.y;
    float z = inPositionSize.z;
    float size = inPositionSize.w;
    float smoothness = clamp(inMaterial.z, 0.0, 1.0);
    bool smoothLod = smoothness >= 0.9999;

    vec3 sw = vec3(x, y, z);
    vec3 se = vec3(x + size, inNeighborHeights.y, z);
    vec3 nw = vec3(x, inNeighborHeights.w, z + size);
    vec3 ne = vec3(x + size, inMaterial.w, z + size);

    if (smoothLod) {
        vec3 corners[6] = vec3[6](nw, ne, se, nw, se, sw);
        int triangleBase = (gl_VertexIndex / 3) * 3;
        vec3 a = corners[triangleBase];
        vec3 b = corners[triangleBase + 1];
        vec3 c = corners[triangleBase + 2];
        vec3 normal = normalize(cross(b - a, c - a));
        if (normal.y < 0.0) normal = -normal;
        vec3 position = corners[gl_VertexIndex];
        // Texture frequency is always one tile per source block.  Dividing by
        // the LOD cell size stretched a single texel set over 8/16/32 blocks
        // and visually exposed every clipmap ring as a flat-colour polygon.
        emitVertex(position, normal, position.xz, inMaterial.x);
        return;
    }

    if (gl_VertexIndex < 6) {
        ivec2 q = QUAD[gl_VertexIndex];
        vec3 blockCorners[6] = vec3[6](
            vec3(nw.x, y, nw.z), vec3(ne.x, y, ne.z), vec3(se.x, y, se.z),
            vec3(nw.x, y, nw.z), vec3(se.x, y, se.z), sw);
        vec3 smoothCorners[6] = vec3[6](nw, ne, se, nw, se, sw);
        vec3 morphedCorners[6];
        for (int i = 0; i < 6; ++i)
            morphedCorners[i] = mix(blockCorners[i], smoothCorners[i], smoothness);
        int triangleBase = (gl_VertexIndex / 3) * 3;
        vec3 normal = normalize(cross(
            morphedCorners[triangleBase + 1] - morphedCorners[triangleBase],
            morphedCorners[triangleBase + 2] - morphedCorners[triangleBase]));
        if (normal.y < 0.0) normal = -normal;
        emitVertex(morphedCorners[gl_VertexIndex], normal,
                   vec2(q) * size, inMaterial.x);
        return;
    }

    int side = (gl_VertexIndex - 6) / 6;
    int localVertex = (gl_VertexIndex - 6) % 6;
    const int TRI[6] = int[6](0, 1, 2, 0, 2, 3);
    int corner = TRI[localVertex];
    float adjacentY = inNeighborHeights[side];
    float bottomY = min(y, adjacentY);
    vec3 position;
    vec3 normal;
    float alongFace = (corner == 1 || corner == 2) ? size : 0.0;

    if (side == 0) {
        float topStart = mix(y, sw.y, smoothness);
        float topEnd = mix(y, nw.y, smoothness);
        vec3 corners[4] = vec3[4](
            vec3(x, mix(bottomY, topStart, smoothness), z),
            vec3(x, mix(bottomY, topEnd, smoothness), z + size),
            vec3(x, topEnd, z + size), vec3(x, topStart, z));
        position = corners[corner];
        normal = vec3(-1.0, 0.0, 0.0);
    } else if (side == 1) {
        float topEnd = mix(y, se.y, smoothness);
        float topStart = mix(y, ne.y, smoothness);
        vec3 corners[4] = vec3[4](
            vec3(x + size, mix(bottomY, topStart, smoothness), z + size),
            vec3(x + size, mix(bottomY, topEnd, smoothness), z),
            vec3(x + size, topEnd, z), vec3(x + size, topStart, z + size));
        position = corners[corner];
        normal = vec3(1.0, 0.0, 0.0);
    } else if (side == 2) {
        float topStart = mix(y, se.y, smoothness);
        float topEnd = mix(y, sw.y, smoothness);
        vec3 corners[4] = vec3[4](
            vec3(x + size, mix(bottomY, topStart, smoothness), z),
            vec3(x, mix(bottomY, topEnd, smoothness), z),
            vec3(x, topEnd, z), vec3(x + size, topStart, z));
        position = corners[corner];
        normal = vec3(0.0, 0.0, -1.0);
    } else {
        float topStart = mix(y, nw.y, smoothness);
        float topEnd = mix(y, ne.y, smoothness);
        vec3 corners[4] = vec3[4](
            vec3(x, mix(bottomY, topStart, smoothness), z + size),
            vec3(x + size, mix(bottomY, topEnd, smoothness), z + size),
            vec3(x + size, topEnd, z + size), vec3(x, topStart, z + size));
        position = corners[corner];
        normal = vec3(0.0, 0.0, 1.0);
    }
    // Repeat vertically as well; tall coarse cliffs must not become one
    // stretched dirt pixel column.
    emitVertex(position, normal, vec2(alongFace, position.y), inMaterial.y);
}
