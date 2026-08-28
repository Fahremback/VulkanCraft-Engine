#version 450

// Editor viewport shading (basic mesh path: terrain, entities, wireframes).
// BUG-EDITOR-LIGHTS-001: this path previously shaded with a HARDCODED light
// direction (vec3(0.45, 0.85, 0.55)) and ignored every scene LightComponent —
// placing point/spot/area lights changed nothing on screen. The fragment now
// consumes the SAME LightUboData the material-graph pipelines already receive
// (write_light_ubo): real directional sun, up to 8 point lights, 4 spots and
// 4 area lights, with the same Lambert + att^2 + 0.22/0.78 normalization so
// basic meshes match material-graph blocks/voxels under identical lights.
// Sun shadows are still material-path-only (no shadow sampler here yet).

layout (location = 0) in vec3 fragColor;
layout (location = 1) in vec3 fragNormal;
layout (location = 2) in vec3 fragWorldPos;

layout (location = 0) out vec4 outColor;

// Must mirror Rendering::LightUboData exactly (all vec4/mat4 members, so the
// GLSL std140 layout matches the C++ layout byte for byte).
layout (set = 0, binding = 0) uniform SceneLights {
    vec4 cameraPosition;              // xyz = camera position
    vec4 sunDirection;                // xyz = direction (sun -> scene), w = enabled
    vec4 sunColor;                    // rgb = color * intensity
    mat4 sunViewProj;                 // unused on this path (no shadow sampler)
    vec4 shadowParams;                // unused on this path
    vec4 pointLightPos[8];            // xyz = position, w = range
    vec4 pointLightColor[8];          // rgb = color * intensity, w = enabled
    vec4 spotLightPos[4];             // xyz = position, w = range
    vec4 spotLightDir[4];             // xyz = direction, w = enabled
    vec4 spotLightParams[4];          // x = cos inner, y = cos outer
    vec4 spotLightColor[4];           // rgb = color * intensity
    vec4 areaLightPos[4];             // xyz = center, w = enabled
    vec4 areaLightNormal[4];          // xyz = facing normal
    vec4 areaLightHalf[4];            // x = halfWidth, y = halfHeight
    vec4 areaLightColor[4];           // rgb = color * intensity
    mat4 sunCascadeVP[4];             // unused on this path
    vec4 sunCascadeSplits;            // unused on this path
    vec4 cameraForward;               // unused on this path
} lights;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 color;
    vec4 fogParams;  // x=density, y=start, z=heightFog(0/1), w=unused
    vec4 fogColor;   // xyz=fog color, w=unused
} push;

void main() {
    vec3 n = normalize(fragNormal);

    // ------------------------------------------------------------------
    // Scene lighting (BUG-EDITOR-LIGHTS-001): real lights from the scene.
    // Same conventions as the material-graph shader: sunDirection points
    // from the sun toward the scene (surface L = -sunDirection), point/spot
    // attenuation = clamp(1 - dist/range)^2, and the 0.22/0.78 split keeps
    // the editor's basic meshes balanced against material-graph objects.
    // ------------------------------------------------------------------
    vec3 lightAccum = vec3(0.0);

    // Directional sun — falls back to the classic editor key light when the
    // scene has no directional sun so an empty scene is still readable.
    if (lights.sunDirection.w > 0.5) {
        vec3 L = -normalize(lights.sunDirection.xyz);
        lightAccum += max(dot(n, L), 0.0) * lights.sunColor.rgb;
    } else {
        vec3 L = normalize(vec3(0.45, 0.85, 0.55));
        lightAccum += max(dot(n, L), 0.0) * vec3(0.9, 0.9, 0.85);
    }

    // Point lights (position + range, color * intensity)
    for (int i = 0; i < 8; ++i) {
        if (lights.pointLightColor[i].w <= 0.5) continue;
        vec3 toLight = lights.pointLightPos[i].xyz - fragWorldPos;
        float dist = length(toLight);
        float range = max(lights.pointLightPos[i].w, 0.01);
        float att = clamp(1.0 - dist / range, 0.0, 1.0);
        att *= att;
        float ndl = max(dot(n, toLight / max(dist, 0.0001)), 0.0);
        lightAccum += ndl * att * lights.pointLightColor[i].rgb;
    }

    // Spot lights (cone from cos inner/outer)
    for (int i = 0; i < 4; ++i) {
        if (lights.spotLightDir[i].w <= 0.5) continue;
        vec3 toLight = lights.spotLightPos[i].xyz - fragWorldPos;
        float dist = length(toLight);
        float range = max(lights.spotLightPos[i].w, 0.01);
        float att = clamp(1.0 - dist / range, 0.0, 1.0);
        att *= att;
        vec3 L = toLight / max(dist, 0.0001);
        float spot = smoothstep(lights.spotLightParams[i].y,
                                lights.spotLightParams[i].x,
                                dot(-L, lights.spotLightDir[i].xyz));
        float ndl = max(dot(n, L), 0.0);
        lightAccum += ndl * att * spot * lights.spotLightColor[i].rgb;
    }

    // Area lights (approximated as facing attenuated quads)
    for (int i = 0; i < 4; ++i) {
        if (lights.areaLightPos[i].w <= 0.5) continue;
        vec3 toLight = lights.areaLightPos[i].xyz - fragWorldPos;
        float dist = max(length(toLight), 0.0001);
        float reach = max(lights.areaLightHalf[i].x + lights.areaLightHalf[i].y, 0.01);
        float att = clamp(1.0 - dist / reach, 0.0, 1.0);
        att *= att;
        vec3 L = toLight / dist;
        float facing = max(dot(lights.areaLightNormal[i].xyz, -L), 0.0);
        float ndl = max(dot(n, L), 0.0);
        lightAccum += ndl * att * facing * lights.areaLightColor[i].rgb;
    }

    vec3 baseColor = fragColor * (0.22 + 0.78 * lightAccum);

    // Rim light: brightens edges at grazing angles (kept from the previous
    // look so silhouettes stay readable against the grid).
    vec3 viewDir = normalize(-fragWorldPos);
    float rim = 1.0 - max(dot(n, viewDir), 0.0);
    rim = pow(rim, 3.0) * 0.15;
    baseColor += fragColor * rim;

    // Subtle fill from below (kept from the previous look).
    float fill = max(dot(n, vec3(0.0, -1.0, 0.0)), 0.0) * 0.08;
    baseColor += fragColor * fill;

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
