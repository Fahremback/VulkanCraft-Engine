#version 450
#extension GL_GOOGLE_include_directive : require

#include "radiance_cache.glsl"

layout (location = 0) in vec4 fragColor;
layout (location = 1) in vec3 fragNormal;
layout (location = 2) in vec3 fragUV;
layout (location = 3) in vec3 fragWorldPos;
layout (location = 0) out vec4 outColor;

layout (binding = 0) uniform sampler2DArray albedoSampler;
layout (binding = 1) uniform sampler2DArray normalSampler;
layout (binding = 2) uniform sampler2DArray labPbrSampler;
layout (binding = 3) uniform sampler2DShadow shadowMap;
layout (binding = 4) uniform sampler2D opaqueSceneSampler;
layout (binding = 5) uniform sampler2D opaqueDepthSampler;

// Renderer-owned toroidal probe clipmap. Binding 6 is optional at the API
// level, but the real game always binds it after RadianceCache::init().
layout (binding = 6, std430) readonly buffer RadianceCacheBuffer {
    vec4 radianceData[];
} radianceCache;

layout (push_constant) uniform PushConstants {
    mat4 mvp;
    vec4 cameraPos;
    vec4 sunDirection;
    vec4 sunColor;
    vec4 environment;
} push;

const float PI = 3.14159265359;

mat3 cotangentFrame(vec3 normal, vec3 position, vec2 uv) {
    vec3 dp1 = dFdx(position);
    vec3 dp2 = dFdy(position);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, normal);
    vec3 dp1perp = cross(normal, dp1);
    vec3 tangent = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 bitangent = dp2perp * duv1.y + dp1perp * duv2.y;
    float scale = inversesqrt(max(max(dot(tangent, tangent), dot(bitangent, bitangent)), 1e-8));
    return mat3(tangent * scale, bitangent * scale, normal);
}

vec2 parallaxUV(vec2 uv, float layer, vec3 viewTS) {
    float viewZ = max(abs(viewTS.z), 0.16);
    float steps = mix(24.0, 10.0, clamp(viewZ, 0.0, 1.0));
    float layerDepth = 1.0 / steps;
    vec2 delta = (viewTS.xy / viewZ) * 0.045 / steps;
    vec2 currentUV = uv;
    float currentDepth = 0.0;
    float sampledHeight = texture(normalSampler, vec3(currentUV, layer)).a;
    while (currentDepth < sampledHeight && currentDepth < 1.0) {
        currentUV -= delta;
        currentDepth += layerDepth;
        sampledHeight = texture(normalSampler, vec3(currentUV, layer)).a;
    }
    vec2 previousUV = currentUV + delta;
    float after = sampledHeight - currentDepth;
    float before = texture(normalSampler, vec3(previousUV, layer)).a - currentDepth + layerDepth;
    float denominator = after - before;
    float weight = abs(denominator) > 1e-5 ? clamp(after / denominator, 0.0, 1.0) : 0.0;
    return mix(currentUV, previousUV, weight);
}

float distributionGGX(vec3 normal, vec3 halfway, float roughness) {
    float a2 = roughness * roughness;
    a2 *= a2;
    float ndh = max(dot(normal, halfway), 0.0);
    float denominator = ndh * ndh * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denominator * denominator, 1e-5);
}

float geometrySchlickGGX(float ndv, float roughness) {
    float r = roughness + 1.0;
    float k = r * r * 0.125;
    return ndv / max(ndv * (1.0 - k) + k, 1e-5);
}

vec3 fresnelSchlick(float cosTheta, vec3 f0) {
    return f0 + (1.0 - f0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 srgbToLinear(vec3 color) {
    return pow(clamp(color, 0.0, 1.0), vec3(2.2));
}

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

float blissWaterDetailHeight(vec2 position, float distanceFade) {
    float detail = blissWaveLayer(position, vec2(0.41, 1.0), 4.2, 1.27, 0.9) * 0.0150;
    detail += blissWaveLayer(position, vec2(-1.0, 0.36), 2.1, -1.83, 2.6) * 0.0075;
    detail += blissWaveLayer(position, vec2(0.78, -0.63), 1.05, 2.31, 4.3) * 0.0032;
    return detail * distanceFade;
}

float linearWaterDepth(float deviceDepth) {
    const float nearPlane = 0.1;
    const float farPlane = 3500.0;
    float ndcDepth = deviceDepth * 2.0 - 1.0;
    return (2.0 * nearPlane * farPlane) /
           max(farPlane + nearPlane - ndcDepth * (farPlane - nearPlane), 0.0001);
}

vec3 reconstructOpaqueWorldPosition(vec2 uv, float deviceDepth) {
    vec4 world = inverse(push.mvp) * vec4(uv * 2.0 - 1.0, deviceDepth, 1.0);
    return world.xyz / max(abs(world.w), 0.00001) * sign(world.w);
}

vec4 traceWaterReflection(vec3 worldPosition, vec3 reflectedDirection, vec2 screenSize) {
    vec3 origin = worldPosition + reflectedDirection * 0.18;
    for (int stepIndex = 0; stepIndex < 28; ++stepIndex) {
        float stepValue = float(stepIndex) + 1.0;
        float rayDistance = 0.22 * stepValue + 0.045 * stepValue * stepValue;
        vec4 clip = push.mvp * vec4(origin + reflectedDirection * rayDistance, 1.0);
        if (clip.w <= 0.0) break;
        vec3 ndc = clip.xyz / clip.w;
        vec2 rayUV = ndc.xy * 0.5 + 0.5;
        if (any(lessThan(rayUV, vec2(0.002))) || any(greaterThan(rayUV, vec2(0.998)))) break;
        float opaqueDepth = textureLod(opaqueDepthSampler, rayUV, 0.0).r;
        float rayDepth = ndc.z;
        float thickness = 0.0012 + rayDistance * 0.00018;
        if (opaqueDepth < 0.99999 && rayDepth >= opaqueDepth - thickness && rayDepth <= opaqueDepth + thickness * 7.0) {
            vec3 reflectedColor = textureLod(opaqueSceneSampler, rayUV, 0.0).rgb;
            float edgeFade = smoothstep(0.0, 0.08, min(min(rayUV.x, rayUV.y), min(1.0-rayUV.x, 1.0-rayUV.y)));
            float reliableDistance = 1.0-smoothstep(28.0, 92.0, rayDistance);
            return vec4(reflectedColor, edgeFade * reliableDistance);
        }
    }
    return vec4(0.0);
}

vec3 labMetalF0(int code, vec3 albedo) {
    if (code == 230) return vec3(0.560, 0.570, 0.580); // iron
    if (code == 231) return vec3(1.000, 0.766, 0.336); // gold
    if (code == 232) return vec3(0.913, 0.922, 0.924); // aluminium
    if (code == 233) return vec3(0.550, 0.556, 0.554); // chrome
    if (code == 234) return vec3(0.955, 0.638, 0.538); // copper
    if (code == 235) return vec3(0.624, 0.637, 0.641); // lead
    if (code == 236) return vec3(0.672, 0.637, 0.585); // platinum
    if (code == 237) return vec3(0.972, 0.960, 0.915); // silver
    return albedo;
}

vec3 projectShadowCoordinate(vec3 worldPosition, vec3 lightDir) {
    vec3 referenceUp = abs(lightDir.y) > .96 ? vec3(0,0,1) : vec3(0,1,0);
    vec3 right = normalize(cross(referenceUp, lightDir));
    vec3 up = normalize(cross(lightDir, right));
    const float snap = .5;
    vec2 center = floor(vec2(dot(push.cameraPos.xyz,right),dot(push.cameraPos.xyz,up))/snap+.5)*snap;
    vec2 plane = vec2(dot(worldPosition,right),dot(worldPosition,up))-center;
    vec2 projected = plane/512.0;
    float distortion = .16+.84*clamp(length(projected),0.0,1.0);
    vec2 warped = projected/distortion;
    float depth = .5-dot(worldPosition-push.cameraPos.xyz,lightDir)/1280.0;
    return vec3(warped*.5+.5,depth);
}

// Stable ordered coverage in world space. FAR is rendered first; discarded
// samples inside the fully published frontier are filled by the exact chunk
// mesh afterwards. The representation changes over a band without holes.
float lodCoverageThreshold(vec2 worldXZ) {
    ivec2 p = ivec2(floor(worldXZ * 4.0));
    int x = p.x & 3;
    int y = p.y & 3;
    const float bayer[16] = float[16](
         0.5,  8.5,  2.5, 10.5,
        12.5,  4.5, 14.5,  6.5,
         3.5, 11.5,  1.5,  9.5,
        15.5,  7.5, 13.5,  5.5
    );
    return bayer[x + y * 4] * (1.0 / 16.0);
}

void main() {
    // FAR clipmaps are a complete fallback surface.  Reveal detailed chunks
    // ring-by-ring without ever drawing both surfaces in the same pixels.
    // color.a > 1.5 is an engine-only marker and is not texture alpha.
    bool isFarProxy = fragColor.a > 1.5;
    float farCellSize = isFarProxy ? max(fragColor.a - 2.0, 1.0) : 0.0;
    bool detailedFarMaterial = isFarProxy && farCellSize <= 4.01 &&
        length(push.cameraPos.xyz - fragWorldPos) < 620.0;
    if (isFarProxy && push.sunColor.w >= 0.0) {
        vec2 detailedCenter = (floor(push.cameraPos.xz / 16.0) + vec2(0.5)) * 16.0;
        float detailedHalfExtent = (push.sunColor.w + 0.5) * 16.0;
        float frontierDistance = max(abs(fragWorldPos.x - detailedCenter.x),
                                     abs(fragWorldPos.z - detailedCenter.y));
        float transitionWidth = min(24.0, detailedHalfExtent);
        float farCoverage = smoothstep(detailedHalfExtent - transitionWidth,
                                       detailedHalfExtent, frontierDistance);
        if (frontierDistance <= detailedHalfExtent &&
            lodCoverageThreshold(fragWorldPos.xz) >= farCoverage) {
            discard;
        }
    }

    vec3 sunDir = normalize(push.environment.y > 0.03 ? push.sunDirection.xyz : -push.sunDirection.xyz);
    vec3 sunColor = push.sunColor.rgb;
    bool textured = fragUV.z >= 0.0;
    bool isWater = abs(fragUV.z - 13.0) < 0.25;
    // Oak, birch and spruce occupy different stable atlas layers. Treating
    // only oak as foliage sent the first FAR birch/spruce cards through the
    // opaque PBR path, which produced purple/black crowns at the first LOD.
    bool isFoliage = abs(fragUV.z - 8.0) < 0.25 ||
                     abs(fragUV.z - 27.0) < 0.25 ||
                     abs(fragUV.z - 31.0) < 0.25;
    bool isGrassBlade = abs(fragUV.z - 15.0) < 0.25;
    bool isGlass = abs(fragUV.z - 11.0) < 0.25;
    bool isPlayerSkin = abs(fragUV.z - 16.0) < 0.25;
    bool isViewModel = push.environment.w < 0.0;
    bool underwater = push.cameraPos.w < 0.0;
    vec3 geometricNormal = normalize(fragNormal) * (gl_FrontFacing ? 1.0 : -1.0);
    vec3 viewDir = normalize(push.cameraPos.xyz - fragWorldPos);
    vec3 shadowLightDir = normalize(push.environment.y > 0.03 ? push.sunDirection.xyz : -push.sunDirection.xyz);
    float shadowVisibility = 1.0;
    if (!isFarProxy) {
        vec3 projectedShadow = projectShadowCoordinate(fragWorldPos,shadowLightDir);
        vec2 shadowUV = projectedShadow.xy;
        float shadowDepth = projectedShadow.z;
        shadowVisibility = 0.0;
        vec2 shadowTexel = vec2(1.0/2048.0);
        float shadowBias = max(0.00007, 0.00034 * (1.0 - max(dot(geometricNormal, shadowLightDir),0.0)));
        for(int sy=-1;sy<=1;++sy) for(int sx=-1;sx<=1;++sx)
            shadowVisibility += texture(shadowMap, vec3(shadowUV + vec2(sx,sy)*shadowTexel, shadowDepth-shadowBias));
        shadowVisibility /= 9.0;
        // Penumbra variável: contatos ficam firmes e sombras distantes ficam progressivamente macias.
        float penumbra = mix(1.0, 4.0, 1.0 - abs(shadowVisibility * 2.0 - 1.0));
        float softVisibility = 0.0;
        for(int sy=-2;sy<=2;++sy) for(int sx=-2;sx<=2;++sx)
            softVisibility += texture(shadowMap, vec3(shadowUV + vec2(sx,sy)*shadowTexel*penumbra, shadowDepth-shadowBias));
        shadowVisibility = mix(shadowVisibility, softVisibility/25.0, 0.72);
        float shadowEdge = 1.0-smoothstep(.94,1.0,max(abs(shadowUV.x*2.0-1.0),abs(shadowUV.y*2.0-1.0)));
        shadowVisibility=mix(1.0,shadowVisibility,shadowEdge);
    } else {
        // Preserve the large-scale light field through the dense/FAR hand-off
        // with one comparison. Full 34-tap penumbrae remain exclusive to the
        // detailed mesh; this proxy sample fades before shadow resolution is
        // no longer useful.
        vec3 projectedShadow = projectShadowCoordinate(fragWorldPos, shadowLightDir);
        vec2 shadowUV = projectedShadow.xy;
        float insideShadow = 1.0 - smoothstep(.90, .995,
            max(abs(shadowUV.x * 2.0 - 1.0), abs(shadowUV.y * 2.0 - 1.0)));
        float viewDistanceForShadow = length(push.cameraPos.xyz - fragWorldPos);
        float shadowBias = max(0.00010,
            0.00042 * (1.0 - max(dot(geometricNormal, shadowLightDir), 0.0)));
        float proxyShadow = texture(shadowMap,
            vec3(shadowUV, projectedShadow.z - shadowBias));
        float proxyShadowWeight = insideShadow *
            (1.0 - smoothstep(360.0, 560.0, viewDistanceForShadow));
        shadowVisibility = mix(1.0, proxyShadow, proxyShadowWeight);
    }
    mat3 tbn = mat3(1.0);
    if (!isFarProxy || detailedFarMaterial)
        tbn = cotangentFrame(geometricNormal, fragWorldPos, fragUV.xy);
    vec2 sampleUV = fragUV.xy;

    if (textured && !isFarProxy && !isWater && !isFoliage && !isGrassBlade && !isGlass && !isPlayerSkin && !isViewModel) {
        vec3 viewTS = transpose(tbn) * viewDir;
        sampleUV = parallaxUV(sampleUV, fragUV.z, viewTS);
    }

    float appliedLodQuality = clamp(push.sunDirection.w, 0.00001, 0.99);
    float qualityDetail = smoothstep(0.08, 0.99, appliedLodQuality);
    float vegetationBias = mix(-0.55, -1.65, qualityDetail);
    float farGrassBias = mix(-0.32, -1.30, qualityDetail);
    float textureBias = (isGrassBlade || isFoliage)
        ? vegetationBias
        : ((isFarProxy && abs(fragUV.z) < .25) ? farGrassBias
                                                : (abs(fragUV.z) < .25 ? -.32 : 0.0));
    vec4 texel = textured ? texture(albedoSampler, vec3(sampleUV, fragUV.z), textureBias) : vec4(1.0);
    if (isFarProxy && isFoliage && abs(fragUV.z - 8.0) < 0.25) {
        // Hardware-generated alpha mips average transparent texels.  At the
        // first FAR level that average could fall below the cutout threshold,
        // deleting every crown while leaving only its trunk.  Reconstruct the
        // atlas cluster's ragged macro silhouette analytically; the sampled
        // texture still supplies all leaf detail and holes inside it.
        vec2 clusterUV = fract(sampleUV * 2.0);
        vec2 clusterPosition = (clusterUV - 0.5) / vec2(0.47, 0.44);
        float clusterRadius = length(clusterPosition);
        float clusterAngle = atan(clusterPosition.y, clusterPosition.x);
        float raggedRadius = 0.94 + sin(clusterAngle * 7.0) * 0.055
                                  + sin(clusterAngle * 13.0 + 1.7) * 0.028;
        float edgeWidth = max(fwidth(clusterRadius) * 1.35, 0.018);
        float macroCoverage = 1.0 - smoothstep(raggedRadius - edgeWidth,
                                                raggedRadius + edgeWidth,
                                                clusterRadius);
        texel.a = max(texel.a, macroCoverage * 0.94);

        // Some LabPBR packs store leaf albedo almost monochrome and expect a
        // biome tint.  FAR proxies do not carry that separate tint map, so
        // restore green chroma while retaining the source luminance/detail.
        float maximumChannel = max(texel.r, max(texel.g, texel.b));
        float minimumChannel = min(texel.r, min(texel.g, texel.b));
        float chroma = maximumChannel - minimumChannel;
        float leafLuminance = dot(texel.rgb, vec3(0.2126, 0.7152, 0.0722));
        vec3 tintedLeaf = vec3(0.25, 0.67, 0.19) *
                          clamp(leafLuminance * 1.75 + 0.28, 0.42, 1.18);
        texel.rgb = mix(tintedLeaf, texel.rgb, smoothstep(0.035, 0.16, chroma));
    }
    float alphaCutoff = (isGrassBlade || isFoliage) ? .20 : .30;
    if (texel.a < alphaCutoff) discard;

    vec3 baseColor = texel.rgb * fragColor.rgb;
    // Low-frequency multi-scale GI from the GPU-visible radiance cache. The
    // metadata occupies 8 vec4s; each probe is 3 vec4s. Sample a deterministic
    // camera-relative slot and attenuate by normal-facing visibility so this
    // complements (rather than replaces) direct sun/shadow lighting.
    if (!isViewModel && !isWater && !isFarProxy) {
        uint probeCount = uint(max(radianceCache.radianceData.length() - 8, 1));
        uint probeIndex = uint(abs(int(floor(fragWorldPos.x + fragWorldPos.y * 3.0 + fragWorldPos.z * 7.0)))) % probeCount;
        uint probeBase = 8u + probeIndex * 3u;
        vec4 probeRadiance = radianceCache.radianceData[probeBase];
        vec4 probeDirection = radianceCache.radianceData[probeBase + 1u];
        float giVisibility = clamp(probeRadiance.a * (0.35 + 0.65 * max(dot(geometricNormal, normalize(probeDirection.xyz)), 0.0)), 0.0, 1.0);
        baseColor *= 1.0 + probeRadiance.rgb * giVisibility * 0.42;
    }
    if (isFarProxy && abs(fragUV.z) < 0.25) {
        // Preserve a grass-scale silhouette after anisotropic minification has
        // averaged the source albedo into a flat green field. Derivative fade
        // makes the procedural strands disappear only once they are sub-pixel.
        vec2 grassCell = fragWorldPos.xz * 1.75;
        float derivativeWidth = max(fwidth(grassCell.x), fwidth(grassCell.y));
        float strandVisibility = (1.0 - smoothstep(0.45, 1.8, derivativeWidth)) *
                                 qualityDetail;
        float strands = pow(1.0 - abs(fract(grassCell.x +
            sin(grassCell.y * 0.73) * 0.31) * 2.0 - 1.0), 7.0);
        float clump = 0.5 + 0.5 * sin(fragWorldPos.x * 0.19 +
                                      fragWorldPos.z * 0.23);
        baseColor *= mix(vec3(0.90, 0.96, 0.86),
                         vec3(1.08, 1.15, 0.94),
                         strands * strandVisibility * (0.65 + clump * 0.35));
    }
    if (isPlayerSkin) baseColor = clamp(pow(max(texel.rgb, vec3(0.0)), vec3(0.72)) * 1.18, 0.0, 1.0);

    // Braço e bloco segurado vivem em espaço da câmera e recebem iluminação
    // estável de interface, sem shadow map, distância ou neblina do mundo.
    if (isViewModel) {
        vec3 viewNormal = normalize(fragNormal) * (gl_FrontFacing ? 1.0 : -1.0);
        vec3 keyLight = normalize(vec3(-0.45, 0.72, 0.52));
        vec3 fillLight = normalize(vec3(0.62, 0.25, -0.74));
        float key = max(dot(viewNormal, keyLight), 0.0);
        float fill = max(dot(viewNormal, fillLight), 0.0);
        float rim = pow(1.0 - abs(viewNormal.z), 2.2);
        vec3 warmDay = mix(vec3(0.72, 0.78, 0.95), vec3(1.0, 0.94, 0.82), push.environment.y);
        vec3 litColor = baseColor * (0.58 + key * 0.58 + fill * 0.22) * warmDay;
        litColor += baseColor * rim * 0.10;
        outColor = vec4(max(litColor, baseColor * 0.48), texel.a * fragColor.a);
        return;
    }

    if (isWater) {
        bool waterfall = abs(geometricNormal.y) < 0.55;
        float distanceToWater = length(push.cameraPos.xyz - fragWorldPos);
        float aerialWaterLod=smoothstep(30.0,105.0,distanceToWater)
                            *smoothstep(.54,.91,abs(viewDir.y));
        float normalSample = mix(0.035, 0.10, smoothstep(55.0, 420.0, distanceToWater));
        float detailFade = mix(1.0, 0.58, smoothstep(120.0, 650.0, distanceToWater));
        float centerHeight = blissWaterHeight(fragWorldPos.xz) + blissWaterDetailHeight(fragWorldPos.xz, detailFade);
        float xHeight = blissWaterHeight(fragWorldPos.xz + vec2(normalSample, 0.0))
                      + blissWaterDetailHeight(fragWorldPos.xz + vec2(normalSample, 0.0), detailFade);
        float zHeight = blissWaterHeight(fragWorldPos.xz + vec2(0.0, normalSample))
                      + blissWaterDetailHeight(fragWorldPos.xz + vec2(0.0, normalSample), detailFade);
        // O SEUS multiplica o gradiente da heightfield; isso mantém a leitura das
        // ondas no reflexo mesmo quando a câmera sobe, sem aumentar sua altura física.
        vec3 normal = normalize(vec3((centerHeight-xHeight)/normalSample * 2.85, 1.0,
                                     (centerHeight-zHeight)/normalSample * 2.85));

        if (waterfall) {
            float lateral = fragWorldPos.x * abs(geometricNormal.z) + fragWorldPos.z * abs(geometricNormal.x);
            float flowA = sin(fragWorldPos.y * 4.8 - push.environment.x * 3.4 + lateral * 1.7);
            float flowB = sin(fragWorldPos.y * 11.7 - push.environment.x * 7.1 - lateral * 3.2) * 0.32;
            normal = normalize(geometricNormal + vec3(0.0, (flowA+flowB)*0.13, 0.0));
        }

        vec2 screenSize = vec2(textureSize(opaqueSceneSampler, 0));
        vec2 screenUV = gl_FragCoord.xy / screenSize;
        float facing = clamp(dot(viewDir, normal), 0.0, 1.0);
        float fresnel = 0.020 + 0.980 * pow(1.0-facing, 5.0);

        // Refração do cenário opaco preservado antes do passe translúcido.
        vec2 refractionVector = vec2(normal.x, -normal.z) *
                                mix(0.006, 0.020, 1.0-facing) *
                                min(1.0, distanceToWater * 0.08);
        if (waterfall) refractionVector += vec2(0.0, sin(fragWorldPos.y*9.0-push.environment.x*5.0)*0.004);
        vec2 refractedUV = clamp(screenUV + refractionVector, vec2(0.002), vec2(0.998));
        float waterDepth = gl_FragCoord.z;
        float refractedDepth = textureLod(opaqueDepthSampler, refractedUV, 0.0).r;
        if (refractedDepth + 0.0004 < waterDepth) {
            refractedUV = screenUV;
            refractedDepth = textureLod(opaqueDepthSampler, screenUV, 0.0).r;
        }
        vec3 refractedScene = textureLod(opaqueSceneSampler, refractedUV, 0.0).rgb;
        vec3 opaqueWorldPosition = reconstructOpaqueWorldPosition(refractedUV, refractedDepth);
        float columnDepth = refractedDepth < .99999
            ? max(fragWorldPos.y-opaqueWorldPosition.y,0.0) : 38.0;
        // O depth do mundo voxelizado muda em degraus de um bloco. Filtrar a
        // coluna antes da absorção remove a grade sem borrar ondas/reflexos.
        const vec2 depthRing[8]=vec2[8](vec2(1,0),vec2(-1,0),vec2(0,1),vec2(0,-1),
                                        vec2(1,1),vec2(-1,-1),vec2(1,-1),vec2(-1,1));
        vec2 depthTexel=1.0/screenSize;
        vec2 depthRadius=depthTexel*mix(3.0,9.0,smoothstep(18.0,150.0,distanceToWater));
        float accumulatedColumn=columnDepth;
        float columnSamples=1.0;
        for(int depthIndex=0;depthIndex<8;++depthIndex){
            vec2 depthUV=clamp(refractedUV+depthRing[depthIndex]*depthRadius,vec2(.002),vec2(.998));
            float sampleDepth=textureLod(opaqueDepthSampler,depthUV,0.0).r;
            if(sampleDepth+.0004>=waterDepth){
                float sampledColumn=sampleDepth<.99999
                    ? max(fragWorldPos.y-reconstructOpaqueWorldPosition(depthUV,sampleDepth).y,0.0) : 38.0;
                accumulatedColumn+=min(sampledColumn,48.0);
                columnSamples+=1.0;
            }
        }
        columnDepth=mix(columnDepth,accumulatedColumn/columnSamples,waterfall?.15:.94);
        float organicBasin=.5+.5*sin(fragWorldPos.x*.021+sin(fragWorldPos.z*.014)*2.3);
        organicBasin=mix(organicBasin,.5+.5*cos(fragWorldPos.z*.027-fragWorldPos.x*.009),.43);
        float aerialDepth=mix(4.8,11.5,smoothstep(.08,.92,organicBasin));
        columnDepth=mix(columnDepth,aerialDepth,aerialWaterLod);
        float opticalDepth=waterfall ? 2.4
            : clamp(columnDepth/max(abs(viewDir.y),.20),0.0,48.0)+2.15;

        // A rugosidade da interface espalha apenas a transmissão do fundo. O
        // reflexo SSR continua usando a cena nítida, evitando o borrão distante.
        float transmissionRoughness = smoothstep(.35, 4.5, columnDepth) * (waterfall ? .25 : .88);
        vec2 transmissionTexel = 1.0 / screenSize;
        vec2 transmissionRadius = transmissionTexel * mix(1.0, 5.2, transmissionRoughness);
        vec3 filteredTransmission = refractedScene * .28;
        filteredTransmission += textureLod(opaqueSceneSampler, refractedUV + vec2(transmissionRadius.x,0), 0.0).rgb * .12;
        filteredTransmission += textureLod(opaqueSceneSampler, refractedUV - vec2(transmissionRadius.x,0), 0.0).rgb * .12;
        filteredTransmission += textureLod(opaqueSceneSampler, refractedUV + vec2(0,transmissionRadius.y), 0.0).rgb * .12;
        filteredTransmission += textureLod(opaqueSceneSampler, refractedUV - vec2(0,transmissionRadius.y), 0.0).rgb * .12;
        filteredTransmission += textureLod(opaqueSceneSampler, refractedUV + transmissionRadius, 0.0).rgb * .06;
        filteredTransmission += textureLod(opaqueSceneSampler, refractedUV - transmissionRadius, 0.0).rgb * .06;
        filteredTransmission += textureLod(opaqueSceneSampler, refractedUV + vec2(transmissionRadius.x,-transmissionRadius.y), 0.0).rgb * .06;
        filteredTransmission += textureLod(opaqueSceneSampler, refractedUV + vec2(-transmissionRadius.x,transmissionRadius.y), 0.0).rgb * .06;
        refractedScene = mix(refractedScene, filteredTransmission, transmissionRoughness);

        // Coeficientes de absorção do modelo Bliss: vermelho desaparece primeiro,
        // depois verde, enquanto azul atravessa maiores profundidades.
        vec3 absorption = vec3(0.2629, 0.0565, 0.01011) + vec3(0.048);
        vec3 transmittance = exp(-absorption * opticalDepth * (underwater ? 1.28 : 1.04));

        float climate = 0.5 + 0.5*sin(fragWorldPos.x*.019 + sin(fragWorldPos.z*.014)*2.2);
        float localTone = 0.5 + 0.5*cos(fragWorldPos.z*.087-fragWorldPos.x*.041);
        vec3 coldScatter = srgbToLinear(vec3(25.0, 48.0, 112.0)/255.0);
        vec3 oceanScatter = srgbToLinear(vec3(25.0, 104.0, 158.0)/255.0);
        vec3 warmScatter = srgbToLinear(vec3(24.0, 144.0, 164.0)/255.0);
        vec3 swampScatter = srgbToLinear(vec3(62.0, 94.0, 72.0)/255.0);
        vec3 scattering = mix(coldScatter, oceanScatter, smoothstep(.12,.62,climate));
        scattering = mix(scattering, warmScatter, smoothstep(.61,.93,climate)*.62);
        scattering = mix(scattering, swampScatter, smoothstep(.78,.97,localTone)*.38);

        // Espalhamento não é emissão. Sem iluminação solar, a coluna d'água
        // conserva apenas uma resposta lunar azul muito fraca.
        float daylight = clamp(push.environment.y,0.0,1.0);
        float surfaceLight = smoothstep(.015,.30,daylight);
        vec3 moonlitScattering = scattering * vec3(.035,.055,.11);
        scattering = mix(moonlitScattering,scattering,surfaceLight);

        float waveColor = clamp(0.5 + centerHeight * 3.8, 0.0, 1.0);
        scattering *= mix(vec3(.72,.84,1.08), vec3(.82,1.12,1.16), waveColor);
        vec3 transmittedColor = refractedScene * transmittance;
        transmittedColor += scattering * (vec3(1.0)-transmittance) *
                            mix(0.62, 1.05, shadowVisibility);
        float floorSuppression = smoothstep(.65,4.8,columnDepth);
        float topDownSuppression = smoothstep(.56,.94,abs(viewDir.y))
                                 * smoothstep(18.0,95.0,distanceToWater);
        float scatteringVeil = clamp(.20+floorSuppression*.75+topDownSuppression*.10,.20,.975);
        scatteringVeil=max(scatteringVeil,aerialWaterLod*.985);
        transmittedColor = mix(transmittedColor,scattering*mix(.86,1.10,shadowVisibility),scatteringVeil);

        // Cáusticas procedurais projetadas no fundo visível, mais fortes em água rasa.
        float causticA = abs(sin(fragWorldPos.x*.82 + fragWorldPos.z*.37 + push.environment.x*.72));
        float causticB = abs(sin(fragWorldPos.z*1.13-fragWorldPos.x*.29-push.environment.x*.61));
        float caustics = pow(clamp(1.0-abs(causticA-causticB),0.0,1.0), 9.0);
        // Cáustica pertence ao fundo raso. No LOD aéreo ela criava uma malha
        // branca por aliasing, portanto some antes de chegar à superfície distante.
        caustics *= exp(-opticalDepth*.19) * max(dot(normal,sunDir),0.0) * shadowVisibility
                  * (1.0-floorSuppression) * pow(1.0-aerialWaterLod,2.0) * surfaceLight;
        transmittedColor += push.sunColor.rgb * caustics * 0.22;

        // Reflexo SSR do terreno/vegetação, com céu como fallback fora da tela.
        vec3 incident = normalize(fragWorldPos - push.cameraPos.xyz);
        vec3 reflectedDirection = normalize(reflect(incident, normal));
        vec4 ssr = traceWaterReflection(fragWorldPos, reflectedDirection, screenSize);
        ssr.a*=1.0-aerialWaterLod;
        vec3 nightSky = mix(vec3(.004,.008,.025),vec3(.018,.035,.090),clamp(normal.y,0.0,1.0));
        vec3 daySky = mix(vec3(.055,.14,.28),vec3(.22,.46,.78),clamp(normal.y*.65+.35,0.0,1.0));
        vec3 reflectedSky = mix(nightSky, daySky, push.environment.y);
        vec3 reflectionColor = mix(reflectedSky, ssr.rgb, ssr.a);

        vec3 reflectedSun = reflect(-sunDir, normal);
        float sunAlignment = max(dot(reflectedSun, viewDir), 0.0);
        float sunSparkle = pow(sunAlignment, waterfall ? 34.0 : 180.0);
        float sunGlare = pow(sunAlignment, 18.0) *
                         smoothstep(.35,.92,.5+.5*sin(centerHeight*850.0+fragWorldPos.x*2.3));
        reflectionColor += push.sunColor.rgb * (sunSparkle*7.0 + sunGlare*.75) *
                           surfaceLight * shadowVisibility;

        float reflectionWeight = underwater ? 0.09
            : clamp(.11 + fresnel * mix(.76,1.0,shadowVisibility),.11,.95);
        vec3 color = mix(transmittedColor, reflectionColor, reflectionWeight);
        color *= mix(.66,1.0,shadowVisibility);
        if (waterfall) color = mix(color, scattering*1.35 + reflectionColor*.22, .34);
        // Alpha > 1 marca a superfície para o pós-processamento sem alterar RGB.
        outColor = vec4(max(color, vec3(0.0)), 2.0);
        return;
    }

    if (isWater && false) {
        bool waterfall = abs(geometricNormal.y) < 0.55;
        float time = push.environment.x;
        float largeX = sin(fragWorldPos.x * 0.74 + fragWorldPos.z * 0.29 + time * 1.22)
                     + sin(fragWorldPos.x * 1.31 - fragWorldPos.z * 0.57 - time * 0.83) * 0.58;
        float largeZ = cos(fragWorldPos.z * 0.91 - fragWorldPos.x * 0.22 - time * 1.07)
                     + cos(fragWorldPos.z * 1.67 + fragWorldPos.x * 0.46 + time * 0.71) * 0.52;
        float microX = sin(fragWorldPos.x * 5.8 + fragWorldPos.z * 2.7 + time * 2.9)
                     + sin(fragWorldPos.x * 11.3 - fragWorldPos.z * 4.2 - time * 3.7) * 0.42;
        float microZ = cos(fragWorldPos.z * 6.4 - fragWorldPos.x * 2.2 - time * 2.5)
                     + cos(fragWorldPos.z * 12.7 + fragWorldPos.x * 3.8 + time * 3.3) * 0.38;
        vec2 slope = vec2(largeX, largeZ) * 0.105 + vec2(microX, microZ) * 0.025;
        vec3 normal = normalize(geometricNormal + vec3(slope.x, 0.0, slope.y));
        float fallingStreak = 0.0;
        if (waterfall) {
            float lateral = fragWorldPos.x * abs(geometricNormal.z) + fragWorldPos.z * abs(geometricNormal.x);
            fallingStreak = 0.5 + 0.5 * sin(fragWorldPos.y * 9.0 - time * 5.4 + lateral * 3.1);
            normal = normalize(geometricNormal + vec3(0.0, (fallingStreak - 0.5) * 0.12, 0.0));
        }
        float facing = clamp(dot(viewDir, normal), 0.0, 1.0);
        float fresnel = 0.018 + 0.982 * pow(1.0 - facing, 4.4);
        vec3 nightSky = mix(vec3(0.003, 0.009, 0.032), vec3(0.035, 0.075, 0.17), clamp(normal.y, 0.0, 1.0));
        vec3 daySky = mix(vec3(0.025, 0.085, 0.18), vec3(0.15, 0.34, 0.57), clamp(normal.y * 0.62 + 0.38, 0.0, 1.0));
        vec3 sky = mix(nightSky, daySky, push.environment.y);
        // Variação contínua de clima/bioma em escalas grandes, sem manchas em grade.
        float climateA = 0.5 + 0.5 * sin(fragWorldPos.x * 0.018 + sin(fragWorldPos.z * 0.013) * 2.4);
        float climateB = 0.5 + 0.5 * cos(fragWorldPos.z * 0.021 - sin(fragWorldPos.x * 0.011) * 2.1);
        float climateC = 0.5 + 0.5 * sin((fragWorldPos.x-fragWorldPos.z) * 0.008 + 1.7);
        float temperature = smoothstep(0.14, 0.86, climateA * 0.63 + climateC * 0.37);
        float humidity = smoothstep(0.18, 0.88, climateB * 0.68 + climateC * 0.32);

        // Âncoras vanilla convertidas corretamente de sRGB para espaço linear.
        vec3 coldTint    = srgbToLinear(vec3(57.0,  56.0, 201.0) / 255.0);
        vec3 oceanTint   = srgbToLinear(vec3(63.0, 118.0, 228.0) / 255.0);
        vec3 warmTint    = srgbToLinear(vec3(67.0, 185.0, 238.0) / 255.0);
        vec3 swampTint   = srgbToLinear(vec3(97.0, 123.0, 100.0) / 255.0);
        vec3 biomeTint = mix(coldTint, oceanTint, smoothstep(0.08, 0.58, temperature));
        biomeTint = mix(biomeTint, warmTint, smoothstep(0.58, 0.94, temperature) * 0.72);
        float swampAmount = smoothstep(0.69, 0.94, humidity) * (1.0-smoothstep(0.68,0.92,temperature));
        biomeTint = mix(biomeTint, swampTint, swampAmount * 0.72);

        // Variação local menor que um bioma: evita uma superfície inteira com um único azul.
        float localHue = 0.5 + 0.5*sin(fragWorldPos.x*.094 + fragWorldPos.z*.037 + sin(fragWorldPos.z*.071)*1.8);
        float localHue2 = 0.5 + 0.5*cos(fragWorldPos.z*.126 - fragWorldPos.x*.052 + largeZ*.28);
        vec3 localTint = mix(coldTint, oceanTint, smoothstep(.08,.55,localHue));
        localTint = mix(localTint, warmTint, smoothstep(.57,.94,localHue)*.68);
        localTint = mix(localTint, swampTint, smoothstep(.76,.97,localHue2)*.34);
        biomeTint = mix(biomeTint, localTint, .46);

        // Pseudo-batimetria orgânica em três frequências: áreas rasas, profundas e canais.
        float basin = 0.5 + 0.5 * sin(fragWorldPos.x * 0.031 + sin(fragWorldPos.z*.024)*2.7);
        basin = mix(basin, 0.5+0.5*cos(fragWorldPos.z*.043-fragWorldPos.x*.017), 0.42);
        float channel = 0.5 + 0.5 * sin((fragWorldPos.x+fragWorldPos.z)*.095 + largeX*.65);
        float depthFactor = smoothstep(0.18, 0.88, basin*.72 + channel*.28);
        float lightBands = clamp(dot(normal, sunDir) * 0.5 + 0.5, 0.0, 1.0);
        float waveBand = clamp(0.5 + largeX*.20 + largeZ*.17 + microX*.035, 0.0, 1.0);
        vec3 shallowWater = biomeTint * mix(0.62, 0.88, lightBands);
        vec3 deepWater = srgbToLinear(vec3(12.0, 38.0, 82.0) / 255.0);
        vec3 abyssWater = srgbToLinear(vec3(4.0, 15.0, 42.0) / 255.0);
        vec3 transmission = mix(shallowWater, deepWater, 0.10 + depthFactor * 0.72);
        transmission = mix(transmission, abyssWater, depthFactor * depthFactor * 0.38);
        transmission *= mix(0.62, 1.20, waveBand) * mix(0.64, 1.0, shadowVisibility);

        vec3 reflectedSun = reflect(-sunDir, normal);
        float sunAlignment = max(dot(reflectedSun, viewDir), 0.0);
        float sharpGlint = pow(sunAlignment, waterfall ? 42.0 : 220.0);
        float broadGlare = pow(sunAlignment, 18.0);
        float glitterMask = smoothstep(0.48, 0.94, 0.5 + 0.5 * sin(microX * 5.1 + microZ * 3.7));
        glitterMask *= smoothstep(.42,.78,waveBand);
        float highlight = sharpGlint * 10.0 + broadGlare * glitterMask * 2.35;
        vec3 warmSun = mix(sunColor, vec3(1.35, 0.62, 0.20),
                           (1.0-smoothstep(0.08,0.42,abs(push.sunDirection.y))) * push.environment.y);

        // Reflexo domina apenas em ângulos rasantes; olhando para baixo prevalece a profundidade.
        vec3 reflectedSky = srgbToLinear(clamp(sky, 0.0, 1.0));
        float reflectionWeight = clamp(0.045 + fresnel * 0.70, 0.0, 0.82);
        vec3 color = mix(transmission, reflectedSky, reflectionWeight);
        color *= mix(0.72, 0.96, lightBands);
        color *= mix(0.52, 1.0, shadowVisibility);
        color += warmSun * highlight * max(push.environment.y, 0.22);
        color += biomeTint * waveBand * (1.0-depthFactor) * 0.14;
        if (waterfall) color += vec3(0.10, 0.24, 0.29) * fallingStreak * 0.34;
        // Profundidade não pode herdar o branco da areia como acontecia antes.
        float alpha = waterfall ? 0.94 : mix(0.968, 0.995, depthFactor);
        alpha = mix(alpha, 0.985, fresnel);
        outColor = vec4(color, underwater ? 0.18 : alpha);
        return;
    }

    if (abs(fragUV.z) < 0.25) {
        float chroma = max(baseColor.r, max(baseColor.g, baseColor.b))
                     - min(baseColor.r, min(baseColor.g, baseColor.b));
        if (chroma < 0.08) baseColor *= vec3(0.48, 1.18, 0.34);
    }
    vec3 normal = geometricNormal;
    float ao = 1.0;
    float roughness = isGlass ? 0.08 : 0.82;
    float metallic = 0.0;
    float emission = 0.0;
    float subsurface = 0.0;
    vec3 f0 = vec3(isGlass ? 0.04 : 0.035);

    if (textured && (!isFarProxy || detailedFarMaterial) &&
        !isFoliage && !isGrassBlade && !isPlayerSkin) {
        vec4 packedNormal = texture(normalSampler, vec3(sampleUV, fragUV.z));
        vec2 tangentXY = packedNormal.rg * 2.0 - 1.0;
        tangentXY.y = -tangentXY.y;
        float tangentZ = sqrt(max(1.0 - dot(tangentXY, tangentXY), 0.0));
        normal = normalize(tbn * vec3(tangentXY, tangentZ));
        ao = packedNormal.b;

        vec4 material = texture(labPbrSampler, vec3(sampleUV, fragUV.z));
        float smoothness = material.r;
        roughness = clamp(pow(1.0 - smoothness, 2.0), 0.045, 1.0);
        int materialCode = int(round(material.g * 255.0));
        metallic = materialCode >= 230 && materialCode <= 254 ? 1.0 : 0.0;
        f0 = metallic > 0.5
            ? labMetalF0(materialCode, baseColor)
            : vec3(clamp(float(materialCode) / 229.0 * 0.9, 0.02, 0.9));
        subsurface = material.b >= (65.0 / 255.0) ? material.b : 0.0;
        emission = material.a < (254.5 / 255.0) ? material.a * (255.0 / 254.0) : 0.0;
    }

    vec3 halfway = normalize(viewDir + sunDir);
    float geometricNdl = max(dot(geometricNormal, sunDir), 0.0);
    float detailNdl = max(dot(normal, sunDir), 0.0);
    // A direção macro vem da face real do bloco; o normal map só acrescenta
    // relevo fino. Assim topo, lateral iluminada e lateral oposta não ficam iguais.
    float ndl = mix(geometricNdl, detailNdl, 0.34);
    float ndv = max(dot(normal, viewDir), 0.001);
    float hdotv = max(dot(halfway, viewDir), 0.0);
    vec3 fresnel = fresnelSchlick(hdotv, f0);
    float distribution = distributionGGX(normal, halfway, roughness);
    float geometry = geometrySchlickGGX(ndv, roughness) * geometrySchlickGGX(ndl, roughness);
    vec3 specular = distribution * geometry * fresnel / max(4.0 * ndv * max(ndl, 0.001), 0.001);
    vec3 diffuse = (vec3(1.0) - fresnel) * (1.0 - metallic) * baseColor / PI;

    if (isFoliage || isGrassBlade) {
        float wrapped = clamp((dot(normal, sunDir) + 0.45) / 1.45, 0.0, 1.0);
        ndl = max(wrapped, abs(dot(normal, sunDir)) * 0.38);
        diffuse = baseColor / PI;
        specular *= 0.22;
        subsurface = max(subsurface, 0.42);
    }

    // Iluminação hemisférica direcional inspirada no Bliss: céu em cima,
    // horizonte nas laterais e bounce quente do terreno apontando para baixo.
    vec3 flatNormal = normalize(geometricNormal);
    vec3 ambientCoefficients = flatNormal / max(dot(abs(flatNormal),vec3(1.0)),.001);
    float skyFacing = clamp(flatNormal.y*.5+.5,0.0,1.0);
    float skylightDirection = max(pow(skyFacing,.34)+ambientCoefficients.y*.36,.12);
    vec3 nightSkyLight = vec3(.018,.032,.075)*skylightDirection;
    vec3 daySkyLight = vec3(.115,.165,.225)*skylightDirection;
    vec3 ambient = baseColor*mix(nightSkyLight,daySkyLight,push.environment.y)*ao;
    float groundFacing = pow(clamp(-flatNormal.y,0.0,1.0),.45);
    vec3 groundBounce = mix(vec3(.018,.014,.012),vec3(.105,.077,.043),push.environment.y);
    ambient += baseColor*groundBounce*(.18+groundFacing*.82)*ao;
    // Probe GI replaces only part of the analytic fallback. Invalid toroidal
    // cells return zero while filling, so lighting never flashes black during
    // camera scrolls or a sun refresh.
    if (!isViewModel) {
        vec3 cachedIrradiance = sample_radiance_cache(
            fragWorldPos + flatNormal * 0.35, flatNormal);
        float cacheEnergy = max(cachedIrradiance.r,
            max(cachedIrradiance.g, cachedIrradiance.b));
        float cacheReady = smoothstep(0.006, 0.045, cacheEnergy);
        vec3 cachedAmbient = baseColor * cachedIrradiance * (ao / PI);
        ambient = mix(ambient, ambient * 0.48 + cachedAmbient,
                      cacheReady * 0.72);
    }
    if (isPlayerSkin) {
        roughness = 0.76;
        metallic = 0.0;
        ambient = max(ambient, baseColor * mix(0.34, 0.52, push.environment.y));
    }
    if (isGrassBlade) ambient = baseColor * mix(vec3(.020,.043,.030),vec3(.105,.155,.075),push.environment.y);
    float directShadow = isPlayerSkin ? mix(.30,1.0,shadowVisibility) : mix(.075,1.0,shadowVisibility);
    ambient *= mix(.62,1.0,shadowVisibility);
    vec3 color = ambient + (diffuse + specular) * sunColor * ndl * directShadow * 1.28;
    color += baseColor * vec3(0.28, 0.22, 0.10) * max(-dot(normal, sunDir), 0.0) * subsurface;
    color += baseColor * emission * 4.0;

    if (underwater) {
        float distanceFog = 1.0 - exp(-length(push.cameraPos.xyz - fragWorldPos) * 0.055);
        color *= vec3(0.38, 0.72, 0.76);
        color = mix(color, vec3(0.015, 0.17, 0.22), clamp(distanceFog, 0.0, 0.92));
    } else {
        float viewDistance = length(push.cameraPos.xyz - fragWorldPos);
        float fogAmount = 1.0 - exp(-viewDistance * push.environment.z);
        fogAmount *= smoothstep(35.0, 310.0, viewDistance);
        float sunset = (1.0 - smoothstep(0.02, 0.38, abs(push.sunDirection.y))) * push.environment.y;
        vec3 nightFog = vec3(0.012, 0.026, 0.065);
        float altitude = smoothstep(150.0, 310.0, push.cameraPos.y);
        vec3 biomeFog = mix(vec3(0.40,0.56,0.42), vec3(0.50,0.66,0.86), altitude);
        vec3 dayFog = mix(biomeFog, vec3(0.90, 0.32, 0.10), sunset * 0.68);
        color = mix(color, mix(nightFog, dayFog, push.environment.y), clamp(fogAmount, 0.0, 0.88));
        // Fallback distante no espírito do Bliss: a perspectiva atmosférica
        // continua presente, mas não apaga completamente silhuetas de sombra.
        float distantShadowRetention = (1.0-shadowVisibility)*smoothstep(72.0,420.0,viewDistance);
        color *= 1.0-distantShadowRetention*.17;
    }
    outColor = vec4(color, isPlayerSkin || isFarProxy ? texel.a : texel.a * fragColor.a);
}
