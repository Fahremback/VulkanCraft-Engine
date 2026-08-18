#version 450

layout (location = 0) in vec3 fragColor;
layout (location = 1) in vec3 fragNormal;
layout (location = 2) in vec3 fragWorldPos;

layout (location = 0) out vec4 outColor;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 color;
    vec4 fogParams;  // x=density, y=start, z=heightFog(0/1), w=unused
    vec4 fogColor;   // xyz=fog color, w=unused
} push;

void main() {
    vec3 n = normalize(fragNormal);
    vec3 lightDir = normalize(vec3(0.45, 0.85, 0.55));

    // Multi-light: main + ambient + rim
    float NdotL = max(dot(n, lightDir), 0.0);
    float ambient = 0.25;
    float diffuse = NdotL * 0.65;

    // Rim light: brightens edges at grazing angles (fixes dark edges when looking from side)
    vec3 viewDir = normalize(-fragWorldPos);
    float rim = 1.0 - max(dot(n, viewDir), 0.0);
    rim = pow(rim, 3.0) * 0.15;

    // Fill light from below (subtle blue bounce)
    float fill = max(dot(n, vec3(0.0, -1.0, 0.0)), 0.0) * 0.08;

    float shade = ambient + diffuse + rim + fill;
    vec3 baseColor = fragColor * shade;

    // Fog
    float density = push.fogParams.x;
    float fogStart = push.fogParams.y;
    bool useHeightFog = push.fogParams.z > 0.5;

    float dist = length(fragWorldPos);
    float fogFactor = 1.0 - exp(-density * max(dist - fogStart, 0.0));
    fogFactor = clamp(fogFactor, 0.0, 1.0);

    // Height fog: denser near ground (y ≈ 0)
    if (useHeightFog) {
        float heightFactor = exp(-fragWorldPos.y * 0.05) * 0.5;
        fogFactor = clamp(fogFactor + heightFactor, 0.0, 1.0);
    }

    vec3 finalColor = mix(baseColor, push.fogColor.rgb, fogFactor);
    outColor = vec4(finalColor, 1.0);
}
