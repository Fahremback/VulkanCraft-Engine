#ifndef VULKANCRAFT_RADIANCE_CACHE_GLSL
#define VULKANCRAFT_RADIANCE_CACHE_GLSL

#ifndef RADIANCE_CACHE_SET
#define RADIANCE_CACHE_SET 0
#endif
#ifndef RADIANCE_CACHE_BINDING
#define RADIANCE_CACHE_BINDING 6
#endif

#define RADIANCE_CACHE_MAX_CASCADES 6

struct RadianceCascade {
    ivec4 minCellResolution;
    vec4 spacingBase;
};

struct RadianceProbe {
    vec4 radianceVisibility;
    vec4 directionConfidence;
    ivec4 worldCellCascade;
    vec4 depthMoments;
    vec4 reflectionRadianceDistance;
};

layout(std430, set = RADIANCE_CACHE_SET, binding = RADIANCE_CACHE_BINDING)
readonly buffer RadianceCacheBuffer {
    uvec4 counts;
    vec4 sunDirection;
    RadianceCascade cascades[RADIANCE_CACHE_MAX_CASCADES];
    RadianceProbe probes[];
} radianceCache;

ivec3 radiance_cache_positive_mod(ivec3 value, int divisor) {
    return ivec3((value.x % divisor + divisor) % divisor,
                 (value.y % divisor + divisor) % divisor,
                 (value.z % divisor + divisor) % divisor);
}

uint radiance_cache_probe_index(int cascadeIndex, ivec3 cell) {
    RadianceCascade cascade = radianceCache.cascades[cascadeIndex];
    int resolution = cascade.minCellResolution.w;
    ivec3 wrapped = radiance_cache_positive_mod(cell, resolution);
    uint localIndex = uint((wrapped.z * resolution + wrapped.y) * resolution + wrapped.x);
    return uint(cascade.spacingBase.y + 0.5) + localIndex;
}

bool radiance_cache_contains(int cascadeIndex, vec3 worldPosition) {
    RadianceCascade cascade = radianceCache.cascades[cascadeIndex];
    ivec3 cell = ivec3(floor(worldPosition * cascade.spacingBase.w));
    ivec3 local = cell - cascade.minCellResolution.xyz;
    return all(greaterThanEqual(local, ivec3(1))) &&
           all(lessThan(local, ivec3(cascade.minCellResolution.w - 2)));
}

vec4 radiance_cache_fetch(int cascadeIndex, ivec3 cell, vec3 worldPosition,
                          vec3 surfaceNormal) {
    RadianceCascade cascade = radianceCache.cascades[cascadeIndex];
    RadianceProbe probe = radianceCache.probes[radiance_cache_probe_index(cascadeIndex, cell)];
    if (probe.worldCellCascade != ivec4(cell, cascadeIndex)) return vec4(0.0);
    float directional = 0.35 + 0.65 * max(dot(surfaceNormal, probe.directionConfidence.xyz), 0.0);
    vec3 probePosition = (vec3(cell) + 0.5) * cascade.spacingBase.x;
    float surfaceDistance = length(worldPosition - probePosition);
    float depthMean = probe.depthMoments.x;
    float depthVariance = max(probe.depthMoments.y - depthMean * depthMean,
                              cascade.spacingBase.x * cascade.spacingBase.x * 0.04);
    float beyondMean = max(surfaceDistance - depthMean, 0.0);
    float depthVisibility = depthMean > 0.0
        ? exp(-(beyondMean * beyondMean) / max(2.0 * depthVariance, 1.0e-4))
        : 1.0;
    return vec4(probe.radianceVisibility.rgb * directional,
                max(probe.radianceVisibility.a * depthVisibility, 0.001));
}

// Four-tap tetrahedral interpolation is continuous like trilinear filtering,
// but halves SSBO reads. Validity normalization prevents black seams while a
// freshly exposed toroidal slab is still filling incrementally.
vec3 sample_radiance_cache(vec3 worldPosition, vec3 surfaceNormal) {
    int selectedCascade = -1;
    for (int cascade = 0; cascade < int(radianceCache.counts.x); ++cascade) {
        if (radiance_cache_contains(cascade, worldPosition)) {
            selectedCascade = cascade;
            break;
        }
    }
    if (selectedCascade < 0) return vec3(0.0);

    RadianceCascade cascade = radianceCache.cascades[selectedCascade];
    vec3 probeCoordinate = worldPosition * cascade.spacingBase.w - vec3(0.5);
    ivec3 baseCell = ivec3(floor(probeCoordinate));
    vec3 fraction = fract(probeCoordinate);
    ivec3 firstAxis;
    ivec3 secondCorner;
    vec3 orderedFraction;
    if (fraction.x >= fraction.y) {
        if (fraction.y >= fraction.z) {
            firstAxis = ivec3(1, 0, 0); secondCorner = ivec3(1, 1, 0);
            orderedFraction = fraction.xyz;
        } else if (fraction.x >= fraction.z) {
            firstAxis = ivec3(1, 0, 0); secondCorner = ivec3(1, 0, 1);
            orderedFraction = fraction.xzy;
        } else {
            firstAxis = ivec3(0, 0, 1); secondCorner = ivec3(1, 0, 1);
            orderedFraction = fraction.zxy;
        }
    } else {
        if (fraction.x >= fraction.z) {
            firstAxis = ivec3(0, 1, 0); secondCorner = ivec3(1, 1, 0);
            orderedFraction = fraction.yxz;
        } else if (fraction.y >= fraction.z) {
            firstAxis = ivec3(0, 1, 0); secondCorner = ivec3(0, 1, 1);
            orderedFraction = fraction.yzx;
        } else {
            firstAxis = ivec3(0, 0, 1); secondCorner = ivec3(0, 1, 1);
            orderedFraction = fraction.zyx;
        }
    }
    ivec3 offsets[4] = ivec3[4](ivec3(0), firstAxis,
                                      secondCorner, ivec3(1));
    float weights[4] = float[4](
        1.0 - orderedFraction.x,
        orderedFraction.x - orderedFraction.y,
        orderedFraction.y - orderedFraction.z,
        orderedFraction.z);
    vec3 accumulated = vec3(0.0);
    float accumulatedWeight = 0.0;
    for (int sampleIndex = 0; sampleIndex < 4; ++sampleIndex) {
        vec4 sampleValue = radiance_cache_fetch(
            selectedCascade, baseCell + offsets[sampleIndex], worldPosition,
            surfaceNormal);
        float validWeight = weights[sampleIndex] * step(0.0005, sampleValue.a);
        accumulated += sampleValue.rgb * validWeight;
        accumulatedWeight += validWeight;
    }
    return accumulated / max(accumulatedWeight, 1.0e-4);
}

// Spatial probe selection + parallax-corrected specular lookup.  The RGB field
// is produced every frame by the renderer reflection runtime (hardware RT when
// available, probe capture otherwise); alpha stores the traced hit distance.
vec3 sample_reflection_cache(vec3 worldPosition, vec3 surfaceNormal,
                             vec3 viewDirection, float roughness) {
    int selectedCascade = -1;
    for (int cascadeIndex = 0; cascadeIndex < int(radianceCache.counts.x); ++cascadeIndex) {
        if (radiance_cache_contains(cascadeIndex, worldPosition)) {
            selectedCascade = cascadeIndex;
            break;
        }
    }
    if (selectedCascade < 0) return vec3(0.0);

    RadianceCascade cascade = radianceCache.cascades[selectedCascade];
    vec3 coordinate = worldPosition * cascade.spacingBase.w - vec3(0.5);
    ivec3 baseCell = ivec3(floor(coordinate));
    vec3 fraction = fract(coordinate);
    vec3 reflectionDirection = normalize(reflect(-normalize(viewDirection),
                                                  normalize(surfaceNormal)));
    vec3 accumulated = vec3(0.0);
    float accumulatedWeight = 0.0;

    for (int z = 0; z <= 1; ++z) {
        for (int y = 0; y <= 1; ++y) {
            for (int x = 0; x <= 1; ++x) {
                ivec3 offset = ivec3(x, y, z);
                ivec3 cell = baseCell + offset;
                RadianceProbe probe = radianceCache.probes[
                    radiance_cache_probe_index(selectedCascade, cell)];
                if (probe.worldCellCascade != ivec4(cell, selectedCascade)) continue;

                vec3 axisWeight = mix(vec3(1.0) - fraction, fraction, vec3(offset));
                float spatialWeight = axisWeight.x * axisWeight.y * axisWeight.z;
                if (spatialWeight <= 0.0) continue;

                vec3 probePosition = (vec3(cell) + 0.5) * cascade.spacingBase.x;
                float hitDistance = probe.reflectionRadianceDistance.a;
                vec3 reflectionRadiance = hitDistance > 0.0
                    ? probe.reflectionRadianceDistance.rgb
                    : probe.radianceVisibility.rgb * 0.35;

                // Box-free parallax correction: project the stored hit point
                // along the current reflection ray, then re-aim from the real
                // shaded point. This removes the "reflection stuck to probe"
                // artifact while keeping the field compact.
                float parallaxDistance = hitDistance > 0.0
                    ? hitDistance : max(probe.depthMoments.x, cascade.spacingBase.x * 2.0);
                vec3 capturedHit = probePosition + reflectionDirection * parallaxDistance;
                vec3 correctedDirection = normalize(capturedHit - worldPosition);
                float parallaxWeight = 0.25 + 0.75 *
                    max(dot(correctedDirection, reflectionDirection), 0.0);

                float depthMean = probe.depthMoments.x;
                float depthVariance = max(probe.depthMoments.y - depthMean * depthMean,
                    cascade.spacingBase.x * cascade.spacingBase.x * 0.04);
                float probeDistance = length(worldPosition - probePosition);
                float beyondMean = max(probeDistance - depthMean, 0.0);
                float visibilityWeight = depthMean > 0.0
                    ? exp(-(beyondMean * beyondMean) /
                          max(2.0 * depthVariance, 1.0e-4))
                    : 1.0;
                float weight = spatialWeight * parallaxWeight * visibilityWeight;
                accumulated += reflectionRadiance * weight;
                accumulatedWeight += weight;
            }
        }
    }

    vec3 reflected = accumulated / max(accumulatedWeight, 1.0e-4);
    // Rough surfaces intentionally converge toward the low-frequency probe GI.
    vec3 diffuseProbe = sample_radiance_cache(worldPosition, surfaceNormal);
    return mix(reflected, diffuseProbe * 0.45,
               smoothstep(0.35, 0.95, clamp(roughness, 0.0, 1.0)));
}

#endif
