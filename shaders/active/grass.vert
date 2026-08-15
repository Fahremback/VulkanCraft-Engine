#version 450

layout (location = 0) in vec4 inPositionRotation;
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

float hash12(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

void main() {
    vec2 uv = CORNERS[gl_VertexIndex];
    float random = hash12(inPositionRotation.xz);
    // Height matters: when flying, grass that is hundreds of metres below the
    // camera must already use its silhouette LOD even if XZ is nearby.
    float viewDistance = length(push.cameraPos.xyz-inPositionRotation.xyz);
    float lod = smoothstep(26.0,210.0,viewDistance);
    float instanceHash = hash12(inPositionRotation.xz*vec2(31.73,57.91)+inPositionRotation.yy*4.17);
    float frontierFade = 1.0;
    if (push.sunColor.w >= 0.0) {
        vec2 detailedCenter = (floor(push.cameraPos.xz / 16.0) + vec2(0.5)) * 16.0;
        float detailedHalfExtent = (push.sunColor.w + 0.5) * 16.0;
        float frontierDistance = max(abs(inPositionRotation.x - detailedCenter.x),
                                     abs(inPositionRotation.z - detailedCenter.y));
        frontierFade = 1.0 - smoothstep(max(0.0, detailedHalfExtent - 28.0),
                                        detailedHalfExtent, frontierDistance);
    }
    float survival = mix(1.0,.105,lod) * frontierFade;
    if(instanceHash>survival){
        gl_Position=vec4(2.0,2.0,2.0,1.0);fragColor=vec4(0);fragNormal=vec3(0,1,0);
        fragUV=vec3(0,0,15);fragWorldPos=inPositionRotation.xyz;return;
    }
    // LOD de silhueta: menos cards, porém tufos maiores. A cobertura visual
    // continua sendo de lâminas separadas em vez de colapsar num tapete verde.
    float width = (0.18 + random * 0.06)*mix(1.0,3.65,lod);
    float height = (0.24 + random * 0.12)*mix(1.0,1.62,lod);
    const float TWO_PI = 6.2831853;
    int edgeMask = int(floor(inPositionRotation.w));
    float rotation = fract(inPositionRotation.w) * TWO_PI;
    vec2 axis = vec2(cos(rotation), sin(rotation));
    vec3 pos = inPositionRotation.xyz;
    pos.xz += axis * ((uv.x - 0.5) * width);
    pos.y += uv.y * height;

    if (uv.y > 0.0) {
        float wind = sin(pos.x * 1.31 + pos.z * 1.73 + push.environment.x * 1.65) * 0.025;
        pos.xz += vec2(wind, wind * 0.63) * uv.y;
    }
    vec2 blockMin = floor(inPositionRotation.xz);
    const float edgeInset = 0.002;
    if ((edgeMask & 1) != 0) pos.x = max(pos.x, blockMin.x + edgeInset);
    if ((edgeMask & 2) != 0) pos.x = min(pos.x, blockMin.x + 1.0 - edgeInset);
    if ((edgeMask & 4) != 0) pos.z = max(pos.z, blockMin.y + edgeInset);
    if ((edgeMask & 8) != 0) pos.z = min(pos.z, blockMin.y + 1.0 - edgeInset);

    gl_Position = push.mvp * vec4(pos, 1.0);
    int tile = int(floor(hash12(inPositionRotation.xy + inPositionRotation.zw) * 4.0));
    vec2 atlasUV = vec2(uv.x, 1.0 - uv.y) * 0.5
                 + vec2(float(tile & 1), float(tile >> 1)) * 0.5;
    float dryPatch=smoothstep(.78,.97,hash12(floor(inPositionRotation.xz*.35)+vec2(17.2,9.4)));
    vec3 healthyTint=vec3(.76,1.06,.70);
    vec3 dryTint=vec3(1.18,.88,.48);
    fragColor = vec4(mix(healthyTint,dryTint,dryPatch*.72),1.0);
    fragNormal = normalize(vec3(axis.y * 0.35, 0.88, -axis.x * 0.35));
    fragUV = vec3(atlasUV, 15.0);
    fragWorldPos = pos;
}
