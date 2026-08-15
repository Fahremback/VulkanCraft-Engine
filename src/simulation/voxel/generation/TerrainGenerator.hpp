#pragma once

#include <cstdint>

// Families distilled from Terralith's climate parameter list.  They are kept
// engine-native instead of loading Minecraft's density-function JSON at runtime.
enum class BiomeType : uint8_t {
    DeepOcean,
    Ocean,
    Coast,
    River,
    Plains,
    Forest,
    BirchTaiga,
    Meadow,
    Desert,
    DesertOasis,
    Badlands,
    Savanna,
    Swamp,
    Jungle,
    Highlands,
    Alpine,
    Glacial,
    RockyMountains,
    Yosemite,
    Volcanic,
    VolcanicCrater,
    Count
};

struct TerrainSample {
    int height;
    BiomeType biome;
    float temperature;
    float moisture;
    float continentalness;
    float river;
    float erosion;
    float weirdness;
    float slope;
};

class TerrainGenerator {
public:
    // Minecraft/Terralith sea level 63 translated from -64..319 to our 0..383 range.
    static constexpr int SeaLevel = 127;

    static TerrainSample sample(float worldX, float worldZ);
    // Surface-only LOD query. It evaluates the expensive height router once;
    // callers that own a grid derive slope from adjacent cached samples.
    static TerrainSample sample_coarse(float worldX, float worldZ);
    static float cave_density(float worldX, float worldY, float worldZ);
    static float extra_terrain_density(float worldX, float worldY, float worldZ,
                                       const TerrainSample& surface);
    static float ore_density(float worldX, float worldY, float worldZ);
};
