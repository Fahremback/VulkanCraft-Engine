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
// the spot atlas (binding 2, one authored-cone tile per spot slot) and the point slot-0
// face atlas (binding 3, six tiles with LINEAR depth = distance/range).
//
// BUG-EDITOR-GI-001 / CONTA2-GI-EDITOR-005: indirect ambient comes from the
// canonical IGlobalIlluminationProvider DDGI core, uploaded as a dense 8^3
// irradiance array inside EditorShadowUbo (binding 4). Trilinear + window-edge
// fade replaces the flat 0.22 ambient; disabled GI reproduces the old look.
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
    vec4 areaLightHalf[4];            // x/y = half size, z = attenuation range
    vec4 areaLightColor[4];           // rgb = color * intensity
    mat4 sunCascadeVP[4];             // unused on this path
    vec4 sunCascadeSplits;            // unused on this path
    vec4 cameraForward;               // unused on this path
} lights;

// Must mirror Editor::EditorShadowUbo exactly (EditorApplication.hpp).
layout (set = 0, binding = 4) uniform EditorShadow {
    mat4 spotViewProj[4];             // tile i projection (depth remapped to [0,1])
    vec4 spotEnabled;                 // per-slot 0/1
    mat4 pointViewProj[6];            // +X,-X,+Y,-Y,+Z,-Z face projections
    vec4 pointLight;                  // xyz = light position, w = range
    vec4 pointParams;                 // x=enabled, y=1/faceSize, z=range, w=LightUbo point slot
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
// Sun shadow: cascaded 2x2 atlas, 3x3 PCF. Outside the selected cascade is
// fully lit. Sampling is clamped to the selected tile so PCF never leaks into
// a neighboring cascade at an atlas seam.
// ---------------------------------------------------------------------------
float sample_sun_cascade(int c, vec3 worldPos, float bias) {
    vec4 sc = lights.sunCascadeVP[c] * vec4(worldPos, 1.0);
    vec3 proj = sc.xyz / max(abs(sc.w), 1e-5);
    if (proj.z <= 0.0 || proj.z >= 1.0) return 1.0;
    vec2 tileUV = proj.xy * 0.5 + 0.5;
    if (any(lessThan(tileUV, vec2(0.001))) || any(greaterThan(tileUV, vec2(0.999)))) return 1.0;
    vec2 tileOff = vec2(float(c % 2), float(c / 2)) * 0.5;
    vec2 uv = tileUV * 0.5 + tileOff;
    vec2 tileMin = tileOff;
    vec2 tileMax = tileOff + vec2(0.5);
    float texel = max(lights.shadowParams.w, 1e-5);
    float s = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 tap = uv + vec2(float(x), float(y)) * texel;
            tap = clamp(tap, tileMin + vec2(texel), tileMax - vec2(texel));
            s += texture(sunShadowMap, vec3(tap, proj.z - bias));
        }
    }
    return s / 9.0;
}

float sun_shadow(vec3 worldPos, float ndl) {
    if (lights.shadowParams.x < 0.5) return 1.0;
    const float minBias = 0.00025;
    float bias = max(lights.shadowParams.y * 2.0 * (1.0 - ndl), minBias);
    bool cascaded = lights.shadowParams.z > 1.5;
    if (cascaded) {
        float viewDepth = dot(worldPos - lights.cameraPosition.xyz, lights.cameraForward.xyz);
        int c = 3;
        if (viewDepth < lights.sunCascadeSplits.x) c = 0;
        else if (viewDepth < lights.sunCascadeSplits.y) c = 1;
        else if (viewDepth < lights.sunCascadeSplits.z) c = 2;

        float s = sample_sun_cascade(c, worldPos, bias);
        // Cross-fade the final 8% of each cascade into the next one.  A hard
        // cascade switch makes an otherwise stable edge flash when camera
        // rotation moves a receiver across a split plane.
        if (c < 3) {
            float split = c == 0 ? lights.sunCascadeSplits.x
                                 : (c == 1 ? lights.sunCascadeSplits.y : lights.sunCascadeSplits.z);
            float prev = c == 0 ? 0.0
                                : (c == 1 ? lights.sunCascadeSplits.x : lights.sunCascadeSplits.y);
            float blendWidth = max((split - prev) * 0.08, 0.35);
            float blend = smoothstep(split - blendWidth, split, viewDepth);
            if (blend > 0.0) {
                float nextShadow = sample_sun_cascade(c + 1, worldPos, bias);
                s = mix(s, nextShadow, blend);
            }
        }
        return s;
    }

    // Legacy single-map fallback: sunViewProj already maps XY into [0,1].
    vec4 sc = lights.sunViewProj * vec4(worldPos, 1.0);
    vec3 proj = sc.xyz / max(abs(sc.w), 1e-5);
    if (proj.z <= 0.0 || proj.z >= 1.0) return 1.0;
    vec2 uv = proj.xy;
    if (any(lessThan(uv, vec2(0.002))) || any(greaterThan(uv, vec2(0.998)))) return 1.0;
    float texel = max(lights.shadowParams.w, 1e-5);
    float s = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 tap = clamp(uv + vec2(float(x), float(y)) * texel,
                             vec2(texel), vec2(1.0 - texel));
            s += texture(sunShadowMap, vec3(tap, proj.z - bias));
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
// Point shadow: the sole atlas can belong to ANY visible point-light slot.
// Render and sampling share the exact six pointViewProj matrices, so depth and
// face orientation cannot diverge. pointParams.w identifies the matching
// SceneLights point slot; a non-shadowing earlier light no longer blocks a
// later castShadows=true point light.
// ---------------------------------------------------------------------------
float point_shadow(vec3 worldPos, float ndl) {
    if (shadow.pointParams.x < 0.5) return 1.0;
    vec3 d = worldPos - shadow.pointLight.xyz;
    float dist = length(d);
    float range = max(shadow.pointLight.w, 0.01);
    if (dist >= range || dist < 1e-4) return 1.0;
    vec3 ad = abs(d);
    int face;
    if (ad.x >= ad.y && ad.x >= ad.z) {
        face = d.x > 0.0 ? 0 : 1;
    } else if (ad.y >= ad.z) {
        face = d.y > 0.0 ? 2 : 3;
    } else {
        face = d.z > 0.0 ? 4 : 5;
    }
    vec4 sc = shadow.pointViewProj[face] * vec4(worldPos, 1.0);
    if (sc.w <= 0.0) return 1.0;
    vec3 suv = sc.xyz / sc.w;
    if (suv.z < 0.0 || suv.z > 1.0) return 1.0;
    vec2 tileUV = suv.xy * 0.5 + 0.5;
    // Keep bilinear comparison taps inside this face; otherwise the atlas
    // seam blends depth from the neighboring cubemap face.
    float inset = max(shadow.pointParams.y * 0.5, 0.00001);
    tileUV = clamp(tileUV, vec2(inset), vec2(1.0 - inset));
    vec2 uv = vec2((float(face) + tileUV.x) / 6.0, tileUV.y);
    float bias = max(0.0015 * (1.0 - ndl), 0.0006);
    return texture(pointShadowAtlas, vec3(uv, clamp(suv.z - bias, 0.0, 1.0)));
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

    // Point lights (position + range, color * intensity). One visible point
    // light may own the point-shadow atlas; pointParams.w tells which slot.
    int shadowPointIndex = int(shadow.pointParams.w + 0.5);
    for (int i = 0; i < 8; ++i) {
        if (lights.pointLightColor[i].w <= 0.5) continue;
        vec3 toLight = lights.pointLightPos[i].xyz - fragWorldPos;
        float dist = length(toLight);
        float range = max(lights.pointLightPos[i].w, 0.01);
        float att = clamp(1.0 - dist / range, 0.0, 1.0);
        att *= att;
        float ndl = max(dot(n, toLight / max(dist, 0.0001)), 0.0);
        float sh = (shadow.pointParams.x > 0.5 && i == shadowPointIndex)
            ? point_shadow(fragWorldPos, ndl) : 1.0;
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
        float range = max(lights.areaLightHalf[i].z, 0.01);
        float att = clamp(1.0 - dist / range, 0.0, 1.0);
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

    // Rim light must be camera-relative. The previous -fragWorldPos treated
    // world origin as the camera, so translating an object changed its rim
    // even while camera/object orientation stayed identical.
    vec3 viewDir = normalize(lights.cameraPosition.xyz - fragWorldPos);
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

    // Fog distance is camera -> fragment, never world-origin -> fragment.
    // Using length(fragWorldPos) made identical objects fog differently merely
    // because the whole scene was translated away from (0,0,0).
    float dist = length(fragWorldPos - lights.cameraPosition.xyz);
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
