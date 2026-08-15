#version 450

layout (location = 0) in vec3 inPosition;
layout (location = 1) in vec3 inNormal;
layout (location = 2) in vec4 inColor;
layout (location = 3) in vec3 inUV;

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

float blissWaveLayer(vec2 position, vec2 direction, float wavelength, float speed, float phase) {
    float frequency = 6.28318530718 / wavelength;
    float angle = dot(position, normalize(direction)) * frequency + push.environment.x * speed + phase;
    return sin(angle) + sin(angle * 2.07 - phase * 1.7) * 0.24;
}

float blissWaterHeight(vec2 position) {
    float largeModulation = 0.62 + 0.38 * sin(position.x * 0.0105 + position.y * 0.008 - push.environment.x * 0.045);
    float height = blissWaveLayer(position, vec2(1.0, 0.25), 48.0, 0.34, 0.2) * 0.082;
    height += blissWaveLayer(position, vec2(-0.22, 1.0), 31.0, -0.43, 1.8) * 0.057;
    height += blissWaveLayer(position, vec2(0.72, -0.69), 17.0, 0.61, 3.1) * 0.036;
    height += blissWaveLayer(position, vec2(-0.86, -0.51), 9.5, -0.82, 4.7) * 0.019;
    return height * largeModulation;
}

void main() {
    vec3 pos = inPosition;
    vec3 normal = inNormal;
    bool isWater = abs(inUV.z - 13.0) < 0.25;
    bool isGrassBlade = abs(inUV.z - 15.0) < 0.25;
    bool isPlayerSkin = abs(inUV.z - 16.0) < 0.25;

    if (isPlayerSkin && push.cameraPos.w > 1.0) {
        float movement = clamp(push.cameraPos.w - 1.0, 0.0, 1.0);
        float phase = sin(push.environment.x * 10.5) * movement;
        float marker = inColor.a;
        float angle = 0.0;
        vec3 pivot = vec3(0.0);
        if (abs(marker - 0.91) < 0.005) { angle = phase * 0.62; pivot = vec3(-0.125,0.75,0); }
        if (abs(marker - 0.92) < 0.005) { angle = -phase * 0.62; pivot = vec3(0.125,0.75,0); }
        if (abs(marker - 0.93) < 0.005) { angle = -phase * 0.72; pivot = vec3(-0.375,1.45,0); }
        if (abs(marker - 0.94) < 0.005) { angle = phase * 0.72; pivot = vec3(0.375,1.45,0); }
        if (angle != 0.0) {
            float c = cos(angle), s = sin(angle);
            vec3 local = pos - pivot;
            local.yz = mat2(c,-s,s,c) * local.yz;
            pos = local + pivot;
            normal.yz = mat2(c,-s,s,c) * normal.yz;
        }
    }

    if (isGrassBlade && inUV.y > 0.15) {
        float bend = sin(pos.x * 1.7 + pos.z * 2.1 + push.environment.x * 1.9) * 0.035;
        pos.xz += vec2(bend, bend * 0.55) * inUV.y;
    }

    if (isWater && normal.y > 0.5) {
        float distanceLod = smoothstep(45.0, 260.0, length(pos.xz - push.cameraPos.xz));
        float sampleDistance = mix(0.08, 0.55, distanceLod);
        float center = blissWaterHeight(pos.xz);
        float right = blissWaterHeight(pos.xz + vec2(sampleDistance, 0.0));
        float forward = blissWaterHeight(pos.xz + vec2(0.0, sampleDistance));
        pos.y += center;
        normal = normalize(vec3((center-right)/sampleDistance, 1.0, (center-forward)/sampleDistance));
    }

    gl_Position = push.mvp * vec4(pos, 1.0);
    fragColor = inColor;
    fragNormal = normal;
    fragUV = inUV;
    fragWorldPos = pos;
}
