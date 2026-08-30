#version 450

// Editor viewport shading (basic mesh path: terrain, entities, wireframes).
// BUG-EDITOR-LIGHTS-001: this path previously shaded with a HARDCODED light
// direction and ignored every scene LightComponent. It now consumes the SAME
// LightUboData the material-graph pipelines already receive (write_light_ubo):
// real directional sun, up to 8 point lights, 4 spots and 4 area lights, with
// the same Lambert + att^2 + 0.22/0.78 normalization so basic meshes match
// material-graph blocks/voxels under identical lights.
//
// BUG-EDITOR-SHADOWS-001/002: the basic path now also samples the three real
// shadow targets the editor records every frame — the sun map (binding 1),
// the spot atlas (binding 2, one 90° tile per spot slot) and the point slot-0
// face atlas (binding 3, six tiles with LINEAR depth = distance/range).
//
// BUG-EDITOR-GI-001: indirect ambient comes from the Agente 1 IProbeGrid core
// (deterministic CPU capture) uploaded as a dense 8^3 irradiance array inside
// EditorShadowUbo (binding 4). Trilinear + window-edge fade replaces the flat
// 0.22 ambient; with GI disabled the constant reproduces the old look exactly.
//
// SYNC: the EditorShadow block must mirror Editor::EditorShadowUbo
// (EditorApplication.hpp) byte for byte, and the spot/face basis formulas
// mirror editor_spot_view_proj / editor_face_basis in
// EditorApplicationVulkan.cpp (guarded by EditorViewportRegressionTests).

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
    mat4 sunViewProj;                 // single sun shadow map projection
    vec4 shadowParams;                // x = enabled, y = bias, z = cascade count, w = 1/size
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

// Must mirror Editor::EditorShadowUbo exactly (EditorApplication.hpp).
layout (set = 0, binding = 4) uniform EditorShadow {
    mat4 spotViewProj[4];             // tile i projection (depth remapped to [0,1])
    vec4 spotEnabled;                 // per-slot 0/1
    vec4 pointLight;                  // xyz = light position, w = range
    vec4 pointParams;                 // x = enabled, y = near, z = far, w = unused
    vec4 probeOrigin;                 // xyz = window min CELL index, w = cellSize
    vec4 probeParams;                 // x = resolution, y = enabled
    vec4 probeIrradiance[512];        // rgb = irradiance, wrapped cell lookup
} shadow;

layout (set = 0, binding = 1) uniform sampler2DShadow sunShadowMap;
layout (set = 0, binding = 2) uniform sampler2DShadow spotShadowAtlas;
layout (set = 0, binding = 3) uniform sampler2DShadow pointShadowAtlas;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 color;
    vec4 fogParams;  // x=density, y=start, z=heightFog(0/1), w=unused
    vec4 fogColor;   // xyz=fog color, w=unused
} push;

// ---------------------------------------------------------------------------
// Sun shadow: single map, 3x3 PCF. Outside the frustum → fully lit.
// ---------------------------------------------------------------------------
float sun_shadow(vec3 worldPos, float ndl) {
    if (lights.shadowParams.x < 0.5) return 1.0;
    vec4 sc = lights.sunViewProj * vec4(worldPos, 1.0);
    vec3 suv = sc.xyz / max(abs(sc.w), 1e-5);
    if (any(lessThan(suv.xy, vec2(0.002))) || any(greaterThan(suv.xy, vec2(0.998)))) return 1.0;
    if (suv.z <= 0.0 || suv.z >= 1.0) return 1.0;
    float bias = max(lights.shadowParams.y * 4.0 * (1.0 - ndl), 0.0004);
    float texel = max(lights.shadowParams.w, 1e-5);
    float s = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            s += texture(sunShadowMap, vec3(suv.xy + vec2(float(x), float(y)) * texel,
                                            suv.z - bias));
        }
    }
    return s / 9.0;
}

// ---------------------------------------------------------------------------
// Spot shadow: tile i of the atlas. The projection (editor_spot_view_proj)
// stores remapped [0,1] depth, exactly what suv.z holds here.
// ---------------------------------------------------------------------------
float spot_shadow(int i, vec3 worldPos, float ndl) {
    if (shadow.spotEnabled[i] < 0.5) return 1.0;
    vec4 sc = shadow.spotViewProj[i] * vec4(worldPos, 1.0);
    if (sc.w <= 0.0) return 1.0;
    vec3 suv = sc.xyz / sc.w;
    vec2 tileUV = suv.xy * 0.5 + 0.5;
    if (any(lessThan(tileUV, vec2(0.002))) || any(greaterThan(tileUV, vec2(0.998)))) return 1.0;
    if (suv.z <= 0.0 || suv.z >= 1.0) return 1.0;
    vec2 uv = vec2((float(i) + tileUV.x) / 4.0, tileUV.y);
    float bias = max(0.0015 * (1.0 - ndl), 0.0006);
    return texture(spotShadowAtlas, vec3(uv, clamp(suv.z - bias, 0.0, 1.0)));
}

// ---------------------------------------------------------------------------
// Point shadow (slot 0): 6-tile atlas, LINEAR depth = distance/range written
// by the shadow fragment (gl_FragDepth). Face selection + basis mirror
// editor_face_basis in EditorApplicationVulkan.cpp EXACTLY.
// ---------------------------------------------------------------------------
float point_shadow(vec3 worldPos) {
    if (shadow.pointParams.x < 0.5) return 1.0;
    vec3 d = worldPos - shadow.pointLight.xyz;
    float dist = length(d);
    float range = max(shadow.pointLight.w, 0.01);
    if (dist >= range || dist < 1e-4) return 1.0;
    vec3 ad = abs(d);
    int face;
    vec3 fd;
    if (ad.x >= ad.y && ad.x >= ad.z) {
        face = d.x > 0.0 ? 0 : 1;
        fd = vec3(d.x > 0.0 ? 1.0 : -1.0, 0.0, 0.0);
    } else if (ad.y >= ad.z) {
        face = d.y > 0.0 ? 2 : 3;
        fd = vec3(0.0, d.y > 0.0 ? 1.0 : -1.0, 0.0);
    } else {
        face = d.z > 0.0 ? 4 : 5;
        fd = vec3(0.0, 0.0, d.z > 0.0 ? 1.0 : -1.0);
    }
    vec3 upPref = abs(fd.y) > 0.9 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    vec3 right = normalize(cross(fd, upPref));
    vec3 up = cross(right, fd);
    vec2 ndc = vec2(dot(d, right), dot(d, up)) / max(dot(d, fd), 1e-5);
    vec2 tileUV = clamp(ndc * 0.5 + 0.5, vec2(0.0), vec2(1.0));
    vec2 uv = vec2((float(face) + tileUV.x) / 6.0, tileUV.y);
    float ref = clamp((dist - 0.05) / range, 0.0, 1.0);
    return texture(pointShadowAtlas, vec3(uv, ref));
}

// ---------------------------------------------------------------------------
// GI ambient (BUG-EDITOR-GI-001): trilinear over the dense probe grid with
// wrapped cell indices (mirror of the C++ wrap in update_gi_probes), fading
// to the flat 0.22 ambient at the toroidal window edge.
// ---------------------------------------------------------------------------
vec3 gi_irradiance(vec3 worldPos) {
    if (shadow.probeParams.y < 0.5) return vec3(0.22);
    float cs = max(shadow.probeOrigin.w, 0.01);
    vec3 g = worldPos / cs - shadow.probeOrigin.xyz; // cell units; centers at half-integers
    vec3 c0 = floor(g - 0.5);
    vec3 f = clamp(g - 0.5 - c0, vec3(0.0), vec3(1.0));
    vec3 acc = vec3(0.0);
    for (int k = 0; k < 2; ++k) {
        for (int j = 0; j < 2; ++j) {
            for (int i = 0; i < 2; ++i) {
                ivec3 c = ivec3(c0) + ivec3(i, j, k);
                ivec3 w = (ivec3(8) + (c % 8)) % 8;
                vec3 val = shadow.probeIrradiance[w.x + w.y * 8 + w.z * 64].rgb;
                vec3 wgt = vec3(i > 0 ? f.x : 1.0 - f.x, j > 0 ? f.y : 1.0 - f.y,
                                k > 0 ? f.z : 1.0 - f.z);
                acc += val * wgt.x * wgt.y * wgt.z;
            }
        }
    }
    vec3 edge = smoothstep(vec3(4.2), vec3(3.2), abs(g - vec3(4.0)));
    float fade = edge.x * edge.y * edge.z;
    return mix(vec3(0.22), acc, fade);
}

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

    // Directional sun — falls back to a faint fill when the scene has no
    // directional sun. Kept LOW on purpose: otherwise the fixed bounce
    // direction dominates and hides any point/spot/area light the user places
    // (the "light always coming from the same spot" symptom). A scene without
    // a sun is still readable from the dim ambient + the user's lights.
    if (lights.sunDirection.w > 0.5) {
        vec3 L = -normalize(lights.sunDirection.xyz);
        float ndl = max(dot(n, L), 0.0);
        float sh = sun_shadow(fragWorldPos, ndl);   // BUG-EDITOR-SHADOWS-001
        lightAccum += ndl * sh * lights.sunColor.rgb;
    } else {
        vec3 L = normalize(vec3(0.45, 0.85, 0.55));
        lightAccum += max(dot(n, L), 0.0) * vec3(0.18, 0.18, 0.17);
    }

    // Point lights (position + range, color * intensity). The point shadow
    // atlas covers slot 0 only — the same light the shadow pass records.
    for (int i = 0; i < 8; ++i) {
        if (lights.pointLightColor[i].w <= 0.5) continue;
        vec3 toLight = lights.pointLightPos[i].xyz - fragWorldPos;
        float dist = length(toLight);
        float range = max(lights.pointLightPos[i].w, 0.01);
        float att = clamp(1.0 - dist / range, 0.0, 1.0);
        att *= att;
        float ndl = max(dot(n, toLight / max(dist, 0.0001)), 0.0);
        float sh = (i == 0) ? point_shadow(fragWorldPos) : 1.0;
        lightAccum += ndl * att * sh * lights.pointLightColor[i].rgb;
    }

    // Spot lights (cone from cos inner/outer) + per-slot shadow tile.
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
        float sh = spot_shadow(i, fragWorldPos, ndl);
        lightAccum += ndl * att * spot * sh * lights.spotLightColor[i].rgb;
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

    // BUG-EDITOR-GI-001: probe-grid indirect ambient replaces the flat 0.22
    // (identical constant when GI is disabled or outside the probe window).
    vec3 ambient = gi_irradiance(fragWorldPos);
    vec3 baseColor = fragColor * (ambient + 0.78 * lightAccum);

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
