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

vec4 radiance_cache_fetch(int cascadeIndex, ivec3 cell, vec3 surfaceNormal) {
    RadianceProbe probe = radianceCache.probes[radiance_cache_probe_index(cascadeIndex, cell)];
    if (probe.worldCellCascade != ivec4(cell, cascadeIndex)) return vec4(0.0);
    float directional = 0.35 + 0.65 * max(dot(surfaceNormal, probe.directionConfidence.xyz), 0.0);
    return vec4(probe.radianceVisibility.rgb * directional,
                max(probe.radianceVisibility.a, 0.001));
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
            selectedCascade, baseCell + offsets[sampleIndex], surfaceNormal);
        float validWeight = weights[sampleIndex] * step(0.0005, sampleValue.a);
        accumulated += sampleValue.rgb * validWeight;
        accumulatedWeight += validWeight;
    }
    return accumulated / max(accumulatedWeight, 1.0e-4);
}

#endif
