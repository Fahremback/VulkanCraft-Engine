#include "TerrainGenerator.hpp"
#include "FastNoiseLite.hpp"

#include <algorithm>
#include <cmath>

namespace {
FastNoiseLite make_noise(int seed, float frequency, FastNoiseLite::FractalType fractal,
                         int octaves, float gain = 0.5f) {
    FastNoiseLite noise(seed);
    noise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    noise.SetFrequency(frequency);
    noise.SetFractalType(fractal);
    noise.SetFractalOctaves(octaves);
    noise.SetFractalLacunarity(2.0f);
    noise.SetFractalGain(gain);
    return noise;
}

// The first six fields mirror the vanilla/Terralith noise router.  The remaining
// fields approximate Terralith's extra_terrain_sum (cliffs, dunes, arches, spikes).
const FastNoiseLite continental = make_noise(2701, 0.00165f, FastNoiseLite::FractalType_FBm, 5, 0.54f);
const FastNoiseLite erosion = make_noise(811, 0.0031f, FastNoiseLite::FractalType_FBm, 4);
const FastNoiseLite peaks = make_noise(9923, 0.0042f, FastNoiseLite::FractalType_Ridged, 5, 0.56f);
const FastNoiseLite rangeMask = make_noise(9929, 0.0019f, FastNoiseLite::FractalType_FBm, 3);
const FastNoiseLite detail = make_noise(4919, 0.019f, FastNoiseLite::FractalType_FBm, 4, 0.46f);
const FastNoiseLite temperature = make_noise(551, 0.0019f, FastNoiseLite::FractalType_FBm, 4);
const FastNoiseLite moisture = make_noise(129, 0.0021f, FastNoiseLite::FractalType_FBm, 4);
const FastNoiseLite river = make_noise(7187, 0.0030f, FastNoiseLite::FractalType_FBm, 4);
const FastNoiseLite riverWarp = make_noise(7193, 0.0080f, FastNoiseLite::FractalType_FBm, 3);
const FastNoiseLite weirdness = make_noise(9001, 0.0027f, FastNoiseLite::FractalType_FBm, 4);
const FastNoiseLite cliff = make_noise(4409, 0.0064f, FastNoiseLite::FractalType_Ridged, 4);
const FastNoiseLite plateau = make_noise(4417, 0.0028f, FastNoiseLite::FractalType_FBm, 3);
const FastNoiseLite dune = make_noise(3413, 0.010f, FastNoiseLite::FractalType_FBm, 3);
const FastNoiseLite volcanoRegion = make_noise(6619, 0.0016f, FastNoiseLite::FractalType_FBm, 3);
const FastNoiseLite volcanoCone = make_noise(6629, 0.0044f, FastNoiseLite::FractalType_Ridged, 3);
const FastNoiseLite spires = make_noise(8123, 0.0072f, FastNoiseLite::FractalType_Ridged, 4);

const FastNoiseLite caves = make_noise(4049, 0.030f, FastNoiseLite::FractalType_FBm, 3);
const FastNoiseLite caveCheese = make_noise(4051, 0.016f, FastNoiseLite::FractalType_FBm, 3);
const FastNoiseLite caveNoodle = make_noise(4057, 0.047f, FastNoiseLite::FractalType_FBm, 2);
const FastNoiseLite ravine = make_noise(4067, 0.0068f, FastNoiseLite::FractalType_FBm, 3);
const FastNoiseLite ravineHeight = make_noise(4073, 0.0022f, FastNoiseLite::FractalType_FBm, 2);
const FastNoiseLite overhang = make_noise(5153, 0.023f, FastNoiseLite::FractalType_FBm, 3);
const FastNoiseLite ores = make_noise(7717, 0.079f, FastNoiseLite::FractalType_FBm, 3);

float saturate(float value) { return std::clamp(value, 0.0f, 1.0f); }

float smoothstep(float a, float b, float value) {
    const float t = saturate((value - a) / (b - a));
    return t * t * (3.0f - 2.0f * t);
}

float terrace(float value, float steps, float blend) {
    const float scaled = value * steps;
    const float lower = std::floor(scaled) / steps;
    const float fraction = scaled - std::floor(scaled);
    return lower + smoothstep(0.5f - blend, 0.5f + blend, fraction) / steps;
}

struct HeightFields {
    float height;
    float continentalness;
    float erosion;
    float temperature;
    float moisture;
    float weirdness;
    float riverDistance;
    float riverStrength;
    float mountainStrength;
    float cliffStrength;
    float volcanoStrength;
};

HeightFields evaluate_height(float x, float z) {
    const float warp = riverWarp.GetNoise(x, z);
    const float c = continental.GetNoise(x + warp * 34.0f, z - warp * 27.0f);
    const float e = erosion.GetNoise(x, z);
    const float t = temperature.GetNoise(x, z);
    const float m = moisture.GetNoise(x, z);
    const float w = weirdness.GetNoise(x, z);
    const float r = std::abs(river.GetNoise(x + warp * 72.0f, z - warp * 55.0f));

    const float shelf = smoothstep(-0.62f, -0.12f, c);
    const float land = smoothstep(-0.18f, 0.08f, c);
    const float inland = smoothstep(0.05f, 0.58f, c);
    const float lowErosion = 1.0f - smoothstep(-0.12f, 0.62f, e);
    const float range = smoothstep(0.04f, 0.52f, rangeMask.GetNoise(x, z));
    const float peak = std::pow(saturate(peaks.GetNoise(x, z) * 0.72f + 0.42f), 2.15f);
    const float mountain = land * range * lowErosion * peak;
    const float cliffBand = smoothstep(0.34f, 0.76f, cliff.GetNoise(x, z)) *
                            smoothstep(0.10f, 0.72f, land) * (0.35f + 0.65f * lowErosion);

    float height = 52.0f + shelf * 54.0f + land * 51.0f + inland * 15.0f;
    height += e * (10.5f + land * 13.5f);
    height += detail.GetNoise(x, z) * (6.6f + land * 7.5f);
    height += mountain * (105.0f + 48.0f * saturate(w));
    height += cliffBand * (24.0f + 39.0f * saturate(plateau.GetNoise(x, z)));

    // Terralith-style mesa/plateau silhouettes instead of uniformly smooth hills.
    const float plateauStrength = land * smoothstep(0.28f, 0.68f, plateau.GetNoise(x, z));
    if (plateauStrength > 0.0f) {
        const float terraced = terrace(height, 1.0f / 24.0f, 0.17f);
        height = height * (1.0f - plateauStrength * 0.62f) + terraced * plateauStrength * 0.62f;
    }

    // Narrow, warped channels are cut after terrain composition so rivers cross
    // plains, plateaus and mountain valleys instead of following sea level noise.
    const float riverStrength = land * (1.0f - smoothstep(0.020f, 0.095f, r));
    if (riverStrength > 0.0f) {
        const float riverFloor = static_cast<float>(TerrainGenerator::SeaLevel - 12) -
                                 (1.0f - smoothstep(0.0f, 0.035f, r)) * 10.5f;
        height = height * (1.0f - riverStrength) + std::min(height, riverFloor) * riverStrength;
    }

    // Dunes follow two wind directions and are masked by hot/dry climate.
    const float arid = land * smoothstep(0.10f, 0.38f, t) * (1.0f - smoothstep(-0.22f, 0.08f, m));
    if (arid > 0.0f) {
        const float windA = 0.5f + 0.5f * std::sin(x * 0.105f + z * 0.037f + dune.GetNoise(x, z) * 4.0f);
        const float windB = 0.5f + 0.5f * std::sin(x * 0.041f - z * 0.083f);
        height += arid * (std::pow(windA, 2.3f) * 16.5f + std::pow(windB, 4.0f) * 7.5f);
    }

    // Rare volcanic provinces: ridged cones with a depressed summit ring.
    const float volcanicRegion = smoothstep(0.50f, 0.78f, volcanoRegion.GetNoise(x, z)) * land;
    const float cone = saturate(volcanoCone.GetNoise(x, z) * 0.72f + 0.40f);
    const float volcanoStrength = volcanicRegion * std::pow(cone, 2.25f);
    height += volcanoStrength * 105.0f;
    height -= volcanicRegion * smoothstep(0.82f, 0.96f, cone) * 45.0f;

    // Windswept/stony spires are Terralith's spikes/tendrils reduced to this
    // engine's finite vertical range.
    const float spireRegion = land * lowErosion * smoothstep(0.42f, 0.72f, w);
    const float spire = std::pow(saturate(spires.GetNoise(x, z) * 0.85f + 0.20f), 3.6f);
    height += spireRegion * spire * 84.0f;

    return {
        std::clamp(height, 4.0f, 319.0f), c, e, t, m, w, r, riverStrength,
        mountain, cliffBand, volcanoStrength
    };
}
TerrainSample compose_sample(const HeightFields& center, float slope) {
    const int height = std::clamp(static_cast<int>(std::round(center.height)), 4, 319);
    BiomeType biome = BiomeType::Plains;
    if (height < TerrainGenerator::SeaLevel - 39 && center.riverStrength < 0.20f) biome = BiomeType::DeepOcean;
    else if (height < TerrainGenerator::SeaLevel - 1 && center.riverStrength < 0.20f) biome = BiomeType::Ocean;
    else if (center.riverStrength > 0.28f) biome = BiomeType::River;
    else if (height <= TerrainGenerator::SeaLevel + 6) biome = BiomeType::Coast;
    else if (center.volcanoStrength > 0.34f) biome = BiomeType::VolcanicCrater;
    else if (center.volcanoStrength > 0.10f) biome = BiomeType::Volcanic;
    else if (center.temperature < -0.38f && height > 228) biome = BiomeType::Glacial;
    else if (center.cliffStrength > 0.48f && center.moisture > -0.12f) biome = BiomeType::Yosemite;
    else if (center.mountainStrength > 0.28f && center.temperature > -0.28f) biome = BiomeType::RockyMountains;
    else if (height > 237 || (center.temperature < -0.22f && height > 183)) biome = BiomeType::Alpine;
    else if (center.temperature > 0.20f && center.moisture < -0.20f && center.erosion < 0.28f) biome = BiomeType::Badlands;
    else if (center.temperature > 0.16f && center.moisture < -0.13f) biome = BiomeType::Desert;
    else if (center.temperature > 0.22f && center.moisture < 0.02f) biome = BiomeType::Savanna;
    else if (center.temperature > 0.18f && center.moisture > 0.27f) biome = BiomeType::Jungle;
    else if (center.moisture > 0.34f && height < TerrainGenerator::SeaLevel + 42) biome = BiomeType::Swamp;
    else if (center.temperature < -0.12f && center.moisture > 0.02f) biome = BiomeType::BirchTaiga;
    else if (height > 189 || center.mountainStrength > 0.12f) biome = BiomeType::Highlands;
    else if (center.moisture > 0.10f) biome = BiomeType::Forest;
    else if (center.weirdness > 0.05f) biome = BiomeType::Meadow;

    // Small wet pockets inside deserts become oasis terrain.
    if ((biome == BiomeType::Desert || biome == BiomeType::Badlands) &&
        center.riverDistance < 0.15f && center.moisture > -0.28f) {
        biome = BiomeType::DesertOasis;
    }

    return { height, biome, center.temperature, center.moisture,
             center.continentalness, center.riverDistance, center.erosion,
             center.weirdness, slope };
}
}

TerrainSample TerrainGenerator::sample(float worldX, float worldZ) {
    const HeightFields center = evaluate_height(worldX, worldZ);
    constexpr float slopeStep = 2.0f;
    const float hx = evaluate_height(worldX + slopeStep, worldZ).height;
    const float hz = evaluate_height(worldX, worldZ + slopeStep).height;
    const float slope = std::sqrt((hx - center.height) * (hx - center.height) +
                                  (hz - center.height) * (hz - center.height)) / slopeStep;
    return compose_sample(center, slope);
}

TerrainSample TerrainGenerator::sample_coarse(float worldX, float worldZ) {
    return compose_sample(evaluate_height(worldX, worldZ), 0.0f);
}

float TerrainGenerator::cave_density(float worldX, float worldY, float worldZ) {
    // Uma única banda abs(noise)<limite é uma superfície 2D no volume, não um
    // túnel. A versão anterior ainda unia três bandas com max(), esculpindo 37%
    // da rocha. A interseção de dois campos independentes produz linhas 1D
    // (túneis) e impede continentes ocos.
    const float spaghettiA = 0.052f - std::abs(caves.GetNoise(worldX, worldY * 0.78f, worldZ));
    const float spaghettiB = 0.046f - std::abs(caveNoodle.GetNoise(
        worldX + 913.0f, worldY * 1.10f, worldZ - 577.0f));
    const float spaghetti = std::min(spaghettiA, spaghettiB);

    // Salas esparsas. O segundo campo fragmenta regiões grandes do primeiro,
    // evitando que uma única isosuperfície remova o interior de uma montanha.
    const float cheeseShape = caveCheese.GetNoise(worldX, worldY * 1.18f, worldZ) - 0.74f;
    const float cheeseBreaker = caves.GetNoise(worldX - 431.0f, worldY * 0.91f, worldZ + 719.0f) - 0.12f;
    const float cheese = std::min(cheeseShape, cheeseBreaker);

    const float noodleA = 0.025f - std::abs(caveNoodle.GetNoise(worldX, worldY * 1.10f, worldZ));
    const float noodleB = 0.072f - std::abs(caves.GetNoise(
        worldX - 277.0f, worldY * 0.91f, worldZ + 191.0f));
    const float noodle = std::min(noodleA, noodleB);

    const float ravineLine = 0.030f - std::abs(ravine.GetNoise(worldX, worldZ));
    const float ravineCenter = 62.0f + ravineHeight.GetNoise(worldX, worldZ) * 27.0f;
    const float ravineVertical = 0.032f - std::abs(worldY - ravineCenter) / 520.0f;
    const float ravineDensity = std::min(ravineLine, ravineVertical);

    float density = (std::max)({spaghetti, cheese, noodle, ravineDensity});
    // Cavernas acima do nível das nuvens devem ser excepcionais; a subtração
    // mantém o sinal correto (multiplicar densidade negativa causaria inversão).
    density -= smoothstep(170.0f, 270.0f, worldY) * 0.055f;
    return density;
}

float TerrainGenerator::extra_terrain_density(float worldX, float worldY, float worldZ,
                                               const TerrainSample& surface) {
    if (surface.biome != BiomeType::Yosemite && surface.biome != BiomeType::RockyMountains &&
        surface.biome != BiomeType::Volcanic && surface.biome != BiomeType::VolcanicCrater) return -1.0f;
    const float above = worldY - static_cast<float>(surface.height);
    if (above <= 0.0f || above > 18.0f) return -1.0f;
    const float verticalFade = 1.0f - above / 18.0f;
    const float density = overhang.GetNoise(worldX, worldY * 0.72f, worldZ) +
                          verticalFade * 0.42f - 0.64f;
    return density;
}

float TerrainGenerator::ore_density(float worldX, float worldY, float worldZ) {
    return ores.GetNoise(worldX, worldY * 1.15f, worldZ);
}
