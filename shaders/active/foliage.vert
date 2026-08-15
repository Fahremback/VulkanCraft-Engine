#version 450

layout (location = 0) in vec4 inPositionScale;
layout (location = 0) out vec4 fragColor;
layout (location = 1) out vec3 fragNormal;
layout (location = 2) out vec3 fragUV;
layout (location = 3) out vec3 fragWorldPos;

layout (push_constant) uniform PushConstants {
    mat4 mvp;
    vec4 cameraPos;
    vec4 sunDirection;
    vec4 sunColor;
    vec4 environment;
} push;

const vec2 CORNERS[6] = vec2[6](
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
    vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0)
);

float hash12(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }

void main() {
    int plane = gl_VertexIndex / 6;
    vec2 uv = CORNERS[gl_VertexIndex % 6];
    float random = hash12(inPositionScale.xz + vec2(inPositionScale.y, float(plane) * 7.17));
    float viewDistance = length(push.cameraPos.xyz - inPositionScale.xyz);
    float lod = smoothstep(48.0, 420.0, viewDistance);
    float frontierFade = 1.0;
    if (push.sunColor.w >= 0.0) {
        vec2 detailedCenter = (floor(push.cameraPos.xz / 16.0) + vec2(0.5)) * 16.0;
        float detailedHalfExtent = (push.sunColor.w + 0.5) * 16.0;
        float frontierDistance = max(abs(inPositionScale.x - detailedCenter.x),
                                     abs(inPositionScale.z - detailedCenter.y));
        frontierFade = 1.0 - smoothstep(max(0.0, detailedHalfExtent - 24.0),
                                        detailedHalfExtent, frontierDistance);
    }
    float instanceHash = hash12(inPositionScale.xz * vec2(19.31, 43.17)
                              + inPositionScale.yy * 2.73);
    float survival = mix(1.0, 0.14, lod) * frontierFade;
    if (instanceHash > survival) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        fragColor = vec4(0.0);
        fragNormal = vec3(0.0, 1.0, 0.0);
        fragUV = vec3(0.0, 0.0, 8.0);
        fragWorldPos = inPositionScale.xyz;
        return;
    }
    float angle = random * 6.2831853 + float(plane) * 1.5707963;
    vec2 axis = vec2(cos(angle), sin(angle));
    // Fewer cards at distance, but each surviving cluster keeps a readable
    // crown silhouette instead of collapsing into a flat green cutoff.
    float width = inPositionScale.w * mix(1.0, 2.85, lod);
    float height = width * (0.78 + random * 0.16);

    vec3 pos = inPositionScale.xyz;
    vec3 cardNormal;
    if (plane < 2) {
        pos.xz += axis * ((uv.x - 0.5) * width);
        pos.y += (uv.y - 0.5) * height;
        cardNormal = normalize(vec3(axis.y, 0.30, -axis.x));
    } else {
        vec2 perpendicular = vec2(-axis.y, axis.x);
        pos.xz += axis * ((uv.x - 0.5) * width) + perpendicular * ((uv.y - 0.5) * height);
        pos.y += (uv.y - 0.5) * height * 0.16;
        cardNormal = normalize(vec3(-perpendicular.x * 0.12, 1.0, -perpendicular.y * 0.12));
    }

    float phase = random * 6.2831853;
    float wind = sin(push.environment.x * 1.18 + phase + pos.x * 0.31 + pos.z * 0.27) * (0.035 + random * 0.035);
    pos.xz += vec2(wind, wind * 0.58) * uv.y * uv.y;

    int tile = int(floor(hash12(inPositionScale.xy + vec2(float(plane) * 3.1, inPositionScale.z)) * 4.0));
    vec2 atlasUV = uv * 0.5 + vec2(float(tile & 1), float(tile >> 1)) * 0.5;

    gl_Position = push.mvp * vec4(pos, 1.0);
    fragColor = vec4(mix(vec3(0.88, 0.96, 0.84), vec3(1.0, 0.91, 0.76), random * 0.18), 1.0);
    fragNormal = cardNormal;
    fragUV = vec3(atlasUV, 8.0);
    fragWorldPos = pos;
}
