#include "FarTerrain.hpp"

#include "TerrainGenerator.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include "engine/core/logging/Log.hpp"
#include <cstring>
#include <iostream>

namespace {
constexpr int kBaseHalfExtentBlocks = 128;
// Keep actual voxel silhouettes until a single source block is already well
// below one screen pixel on a 720p/70-degree reference view.  Coarser levels
// still double progressively, but the smooth height-field is reserved for the
// kilometre-scale horizon where individual blocks can no longer be resolved.
// Vegetation is budgeted independently for every clipmap annulus.  A single
// global counter made the inner grass exhaust the whole buffer, leaving a hard
// vegetation cut exactly where the next LOD started.
constexpr std::size_t kBaseGrassProxyVertices = 1'700'000u;
constexpr std::size_t kFirstRingGrassProxyVertices = 2'200'000u;
constexpr std::size_t kOuterRingGrassProxyVertices = 700'000u;
constexpr std::size_t kBaseTreeProxyVertices = 360'000u;
constexpr std::size_t kOuterRingTreeProxyVertices = 220'000u;
constexpr float kFarVertexMarker = 2.0f;
// Adjacent rings already have a watertight transition skirt. Offsetting every
// level vertically produced a visible contour (and luminous water steps) at
// exactly the LOD boundary, so every level now shares the same physical datum.
constexpr float kTerrainBiasPerLevel = 0.0f;

struct RingSpec {
    int innerHalfExtent{ 0 };
    int outerHalfExtent{ 0 };
    int spacing{ 8 };
    int level{ 0 };
    bool blocky{ false };
    float smoothness{ 1.0f };
    float retainedQuality{ 1.0f };
};

// The percentage selected in-game is the quality retained at the last chunk.
// Q(d) is evaluated for every ring and controls its complete sampling grid;
// power-of-two quantization preserves a watertight clipmap lattice.
int grid_cells_for_quality(float qualityFraction) {
    // Convert the exact geometric Q(d) value into a bounded screen-space grid.
    // The grid remains fixed-cost like a geometry clipmap: low quality uses
    // 128 cells per side, the default endpoint reaches 256, and 99% reaches
    // 1024. This changes terrain/water/tree sampling together without ever
    // attempting to allocate billions of literal far-away blocks.
    const float qualityPercent = std::clamp(qualityFraction * 100.0f, 0.001f, 99.0f);
    const float logPercent = std::log10(qualityPercent);
    float cells = 256.0f;
    if (logPercent <= -1.0f) {
        const float t = std::clamp((logPercent + 3.0f) * 0.5f, 0.0f, 1.0f);
        cells = glm::mix(128.0f, 256.0f, t);
    } else {
        const float t = std::clamp((logPercent + 1.0f) /
                                   (std::log10(99.0f) + 1.0f), 0.0f, 1.0f);
        cells = glm::mix(256.0f, 1024.0f, t);
    }
    return std::clamp(static_cast<int>(std::lround(cells)), 128, 1024);
}

int ring_spacing_for_quality(int outerHalfExtent, float qualityFraction) {
    const float rawSpacing = std::max(1.0f,
        static_cast<float>(outerHalfExtent * 2) /
        static_cast<float>(grid_cells_for_quality(qualityFraction)));
    // Power-of-two spacing keeps every annulus on the same lattice, preventing
    // cracks and swimming while Q(d) moves the transition thresholds.
    const int exponent = std::max(0, static_cast<int>(std::lround(std::log2(rawSpacing))));
    return 1 << std::min(exponent, 20);
}

struct TreeProfile {
    float density{ 0.0f };
    BlockType wood{ BlockType::Wood };
    BlockType leaves{ BlockType::Leaves };
};

float hash_tree(float worldX, float worldZ, float salt) {
    const float value = std::sin(worldX * 12.9898f + worldZ * 78.233f +
                                 salt * 31.719f) * 43758.5453f;
    return value - std::floor(value);
}

TreeProfile tree_profile(const TerrainSample& sample) {
    TreeProfile profile;
    switch (sample.biome) {
    case BiomeType::Forest: profile.density = 0.065f; break;
    case BiomeType::Jungle: profile.density = 0.105f; break;
    case BiomeType::Swamp: profile.density = 0.045f; break;
    case BiomeType::BirchTaiga:
        profile.density = 0.072f;
        profile.wood = BlockType::WoodBirch;
        profile.leaves = BlockType::LeavesBirch;
        break;
    case BiomeType::Meadow:
        profile.density = 0.018f;
        profile.wood = BlockType::WoodBirch;
        profile.leaves = BlockType::LeavesBirch;
        break;
    case BiomeType::Plains:
        profile.density = 0.008f;
        profile.wood = BlockType::WoodBirch;
        profile.leaves = BlockType::LeavesBirch;
        break;
    case BiomeType::DesertOasis: profile.density = 0.028f; break;
    case BiomeType::Savanna: profile.density = 0.016f; break;
    case BiomeType::Highlands:
        profile.density = 0.022f;
        profile.wood = BlockType::WoodSpruce;
        profile.leaves = BlockType::LeavesSpruce;
        break;
    case BiomeType::Yosemite:
        profile.density = 0.025f;
        profile.wood = BlockType::WoodSpruce;
        profile.leaves = BlockType::LeavesSpruce;
        break;
    case BiomeType::Alpine:
        profile.density = sample.height < 228 ? 0.020f : 0.0f;
        profile.wood = BlockType::WoodSpruce;
        profile.leaves = BlockType::LeavesSpruce;
        break;
    default: break;
    }
    return profile;
}

bool tree_has_grass_surface(const TerrainSample& sample) {
    if (sample.height <= TerrainGenerator::SeaLevel + 2) return false;
    switch (sample.biome) {
    case BiomeType::DeepOcean:
    case BiomeType::Ocean:
    case BiomeType::Coast:
    case BiomeType::River:
    case BiomeType::Desert:
    case BiomeType::Badlands:
    case BiomeType::Volcanic:
    case BiomeType::VolcanicCrater:
    case BiomeType::Alpine:
    case BiomeType::Glacial:
    case BiomeType::RockyMountains:
        return false;
    case BiomeType::DesertOasis:
        return true;
    case BiomeType::Highlands:
        return sample.height < 252 && sample.slope <= 4.20f;
    case BiomeType::Savanna:
    case BiomeType::Yosemite:
        return sample.slope <= 4.20f;
    default:
        return sample.slope <= 4.20f;
    }
}

bool has_grass_surface(const TerrainSample& sample) {
    if (sample.height <= TerrainGenerator::SeaLevel + 2) return false;
    switch (sample.biome) {
    case BiomeType::DeepOcean:
    case BiomeType::Ocean:
    case BiomeType::Coast:
    case BiomeType::River:
    case BiomeType::Desert:
    case BiomeType::Badlands:
    case BiomeType::Volcanic:
    case BiomeType::VolcanicCrater:
    case BiomeType::Alpine:
    case BiomeType::Glacial:
    case BiomeType::RockyMountains:
        return false;
    case BiomeType::Highlands:
        return sample.height < 252 && sample.slope <= 4.20f;
    case BiomeType::Savanna:
    case BiomeType::Yosemite:
        return sample.slope <= 4.20f;
    default:
        return sample.slope <= 4.20f;
    }
}

int positive_chunk_local(int worldCoordinate) {
    int local = worldCoordinate % CHUNK_SIZE_X;
    if (local < 0) local += CHUNK_SIZE_X;
    return local;
}

int snap_quantum_chunks(int reachChunks) {
    if (reachChunks < 32) return 1;
    if (reachChunks < 64) return 2;
    // The first LOD used to move in 128-block jumps at long range.  That could
    // put its 1->2 transition only 64 blocks from the player on one side and
    // made the square boundary unmistakable.  A 64-block publication quantum
    // remains cheap enough for the asynchronous builder while keeping the
    // detailed proxy around the complete dense frontier in every direction.
    return 4;
}

int snap_nearest(int value, int quantum) {
    return static_cast<int>(std::lround(static_cast<double>(value) /
                                        static_cast<double>(quantum))) * quantum;
}

BlockType far_surface_material(const TerrainSample& sample) {
    if (sample.height < TerrainGenerator::SeaLevel + 2) return BlockType::Sand;
    switch (sample.biome) {
    case BiomeType::Desert:
    case BiomeType::DesertOasis:
    case BiomeType::Coast:
        return BlockType::Sand;
    case BiomeType::Badlands:
        return BlockType::Terracotta;
    case BiomeType::Volcanic:
    case BiomeType::VolcanicCrater:
        return BlockType::Basalt;
    case BiomeType::Glacial:
    case BiomeType::Alpine:
        return sample.height > 205 ? BlockType::SnowBlock : BlockType::Stone;
    case BiomeType::RockyMountains:
    case BiomeType::Yosemite:
        return sample.slope > 0.35f ? BlockType::Stone : BlockType::Grass;
    default:
        return BlockType::Grass;
    }
}

glm::vec4 far_color(BlockType material, const glm::vec3& normal) {
    glm::vec4 color = get_block_color(material, normal);
    // Values above the normal alpha domain identify render-only clipmap
    // fragments. voxel.frag uses this marker to discard FAR exactly inside the
    // stable detailed frontier; it is not opacity.
    color.a = kFarVertexMarker;
    return color;
}

glm::vec2 projected_block_uv(const glm::vec3& position,
                             const glm::vec3& normal) {
    const glm::vec3 axis = glm::abs(normal);
    if (axis.x >= axis.y && axis.x >= axis.z)
        return glm::vec2(position.z, position.y);
    if (axis.z >= axis.x && axis.z >= axis.y)
        return glm::vec2(position.x, position.y);
    return glm::vec2(position.x, position.z);
}

void append_textured_quad(std::vector<VoxelVertex>& output,
                          const glm::vec3& p0, const glm::vec3& p1,
                          const glm::vec3& p2, const glm::vec3& p3,
                          const glm::vec3& normal, BlockType material) {
    const glm::vec4 color = far_color(material, normal);
    const float layer = get_block_texture_layer(material, normal);
    const auto vertex = [&](const glm::vec3& position) {
        return VoxelVertex{ position, normal, color,
                            glm::vec3(projected_block_uv(position, normal), layer) };
    };
    const VoxelVertex v0 = vertex(p0);
    const VoxelVertex v1 = vertex(p1);
    const VoxelVertex v2 = vertex(p2);
    const VoxelVertex v3 = vertex(p3);
    output.insert(output.end(), { v0, v1, v2, v0, v2, v3 });
}

void append_textured_quad_uv(std::vector<VoxelVertex>& output,
                             const glm::vec3& p0, const glm::vec3& p1,
                             const glm::vec3& p2, const glm::vec3& p3,
                             const glm::vec3& normal, BlockType material,
                             float width, float height, bool doubleSided) {
    const float layer = get_block_texture_layer(material, normal);
    const auto vertex = [&](const glm::vec3& position, float u, float v,
                            const glm::vec3& faceNormal) {
        return VoxelVertex{ position, faceNormal,
                            far_color(material, faceNormal), glm::vec3(u, v, layer) };
    };
    output.insert(output.end(), {
        vertex(p0, 0.0f, 0.0f, normal), vertex(p1, width, 0.0f, normal),
        vertex(p2, width, height, normal), vertex(p0, 0.0f, 0.0f, normal),
        vertex(p2, width, height, normal), vertex(p3, 0.0f, height, normal)
    });
    if (!doubleSided) return;
    const glm::vec3 reverseNormal = -normal;
    output.insert(output.end(), {
        vertex(p1, width, 0.0f, reverseNormal), vertex(p0, 0.0f, 0.0f, reverseNormal),
        vertex(p3, 0.0f, height, reverseNormal), vertex(p1, width, 0.0f, reverseNormal),
        vertex(p3, 0.0f, height, reverseNormal), vertex(p2, width, height, reverseNormal)
    });
}

void append_atlas_quad_uv(std::vector<VoxelVertex>& output,
                          const glm::vec3& p0, const glm::vec3& p1,
                          const glm::vec3& p2, const glm::vec3& p3,
                          const glm::vec3& normal, float textureLayer,
                          const glm::vec4& tint, const glm::vec2& uvMinimum,
                          const glm::vec2& uvMaximum, bool doubleSided) {
    const auto vertex = [&](const glm::vec3& position, const glm::vec2& uv,
                            const glm::vec3& faceNormal) {
        return VoxelVertex{ position, faceNormal, tint,
                            glm::vec3(uv, textureLayer) };
    };
    output.insert(output.end(), {
        vertex(p0, { uvMinimum.x, uvMaximum.y }, normal),
        vertex(p1, { uvMaximum.x, uvMaximum.y }, normal),
        vertex(p2, { uvMaximum.x, uvMinimum.y }, normal),
        vertex(p0, { uvMinimum.x, uvMaximum.y }, normal),
        vertex(p2, { uvMaximum.x, uvMinimum.y }, normal),
        vertex(p3, { uvMinimum.x, uvMinimum.y }, normal)
    });
    if (!doubleSided) return;
    const glm::vec3 reverseNormal = -normal;
    output.insert(output.end(), {
        vertex(p1, { uvMaximum.x, uvMaximum.y }, reverseNormal),
        vertex(p0, { uvMinimum.x, uvMaximum.y }, reverseNormal),
        vertex(p3, { uvMinimum.x, uvMinimum.y }, reverseNormal),
        vertex(p1, { uvMaximum.x, uvMaximum.y }, reverseNormal),
        vertex(p3, { uvMinimum.x, uvMinimum.y }, reverseNormal),
        vertex(p2, { uvMaximum.x, uvMinimum.y }, reverseNormal)
    });
}

void append_grass_proxy(std::vector<VoxelVertex>& output,
                        const TerrainSample& sample,
                        float cellX, float cellZ, int spacing,
                        float retainedQuality,
                        float swHeight, float seHeight,
                        float neHeight, float nwHeight,
                        float ringBudgetScale, std::size_t vertexBudget) {
    if (!has_grass_surface(sample) ||
        output.size() + 24u > vertexBudget) return;

    retainedQuality = std::clamp(retainedQuality, 0.00001f, 1.0f);
    const float cellHash = hash_tree(cellX, cellZ, 71.0f);
    // Keep a dense one-card-per-block silhouette through the detailed/FAR
    // hand-off. Farther rings thin continuously; surviving tufts get slightly
    // wider so coverage fades into the grass top texture without a hard line.
    const float spacingFade = std::clamp(
        7.0f * std::sqrt(retainedQuality) / static_cast<float>(spacing),
        0.0005f, 1.0f);
    // Q(d) already contains the complete distance degradation requested by the
    // user. Applying a second view-distance fade here was the hidden fixed
    // grass radius: even 99% was multiplied down again after the LOD curve.
    const float survival = std::max(0.0005f,
        spacingFade *
        std::clamp(ringBudgetScale, 0.0005f, 1.0f));
    if (cellHash > survival) return;

    const float u = 0.12f + hash_tree(cellX, cellZ, 72.0f) * 0.76f;
    const float v = 0.12f + hash_tree(cellX, cellZ, 73.0f) * 0.76f;
    const float x = cellX + u * static_cast<float>(spacing);
    const float z = cellZ + v * static_cast<float>(spacing);
    const float southHeight = glm::mix(swHeight, seHeight, u);
    const float northHeight = glm::mix(nwHeight, neHeight, u);
    const float y = glm::mix(southHeight, northHeight, v) + 0.012f;

    const float lod = std::clamp(std::log2(static_cast<float>(spacing)) / 5.0f,
                                 0.0f, 1.0f);
    const float width = glm::mix(0.78f, 2.15f, lod);
    const float height = glm::mix(0.48f, 0.92f, lod);
    const float angle = hash_tree(cellX, cellZ, 74.0f) * 6.28318530718f;
    const glm::vec2 axis(std::cos(angle), std::sin(angle));
    const glm::vec2 halfAxis = axis * (width * 0.5f);
    const glm::vec3 p0(x - halfAxis.x, y, z - halfAxis.y);
    const glm::vec3 p1(x + halfAxis.x, y, z + halfAxis.y);
    const glm::vec3 p2 = p1 + glm::vec3(0.0f, height, 0.0f);
    const glm::vec3 p3 = p0 + glm::vec3(0.0f, height, 0.0f);
    const glm::vec3 normal = glm::normalize(glm::vec3(-axis.y, 0.28f, axis.x));

    const int tile = std::min(3, static_cast<int>(
        hash_tree(cellX, cellZ, 75.0f) * 4.0f));
    const glm::vec2 uvMinimum(static_cast<float>(tile & 1) * 0.5f,
                              static_cast<float>(tile >> 1) * 0.5f);
    const glm::vec2 uvMaximum = uvMinimum + glm::vec2(0.5f);
    const float dry = hash_tree(std::floor(cellX * 0.35f),
                                std::floor(cellZ * 0.35f), 76.0f);
    const glm::vec3 healthy(0.76f, 1.06f, 0.70f);
    const glm::vec3 dryTint(1.18f, 0.88f, 0.48f);
    const glm::vec3 tint = glm::mix(healthy, dryTint,
        std::clamp((dry - 0.78f) / 0.19f, 0.0f, 1.0f) * 0.72f);
    const glm::vec4 markerTint(tint, kFarVertexMarker + static_cast<float>(spacing));
    append_atlas_quad_uv(output, p0, p1, p2, p3, normal,
                         static_cast<float>(TextureIndex::GrassBlade), markerTint,
                         uvMinimum, uvMaximum, true);

    // The first two tiers retain a crossed second card. This is only the
    // silhouette proxy (dense chunks still own their 128 true cards), but it
    // prevents grass from collapsing to a single green plane at the frontier.
    if (spacing <= 2 && retainedQuality > 0.40f &&
        output.size() + 12u <= vertexBudget) {
        const glm::vec2 secondAxis(-axis.y, axis.x);
        const glm::vec2 secondHalf = secondAxis * (width * 0.46f);
        const glm::vec3 q0(x - secondHalf.x, y, z - secondHalf.y);
        const glm::vec3 q1(x + secondHalf.x, y, z + secondHalf.y);
        const glm::vec3 q2 = q1 + glm::vec3(0.0f, height * 0.94f, 0.0f);
        const glm::vec3 q3 = q0 + glm::vec3(0.0f, height * 0.94f, 0.0f);
        const glm::vec3 secondNormal = glm::normalize(
            glm::vec3(-secondAxis.y, 0.28f, secondAxis.x));
        append_atlas_quad_uv(output, q0, q1, q2, q3, secondNormal,
                             static_cast<float>(TextureIndex::GrassBlade), markerTint,
                             uvMinimum, uvMaximum, true);
    }
}

void append_open_box(std::vector<VoxelVertex>& output,
                     const glm::vec3& minimum, const glm::vec3& maximum,
                     BlockType material) {
    append_textured_quad(output,
        { minimum.x, maximum.y, maximum.z }, { maximum.x, maximum.y, maximum.z },
        { maximum.x, maximum.y, minimum.z }, { minimum.x, maximum.y, minimum.z },
        { 0.0f, 1.0f, 0.0f }, material);
    append_textured_quad(output,
        { maximum.x, minimum.y, minimum.z }, { maximum.x, maximum.y, minimum.z },
        { maximum.x, maximum.y, maximum.z }, { maximum.x, minimum.y, maximum.z },
        { 1.0f, 0.0f, 0.0f }, material);
    append_textured_quad(output,
        { minimum.x, minimum.y, maximum.z }, { minimum.x, maximum.y, maximum.z },
        { minimum.x, maximum.y, minimum.z }, { minimum.x, minimum.y, minimum.z },
        { -1.0f, 0.0f, 0.0f }, material);
    append_textured_quad(output,
        { maximum.x, minimum.y, maximum.z }, { maximum.x, maximum.y, maximum.z },
        { minimum.x, maximum.y, maximum.z }, { minimum.x, minimum.y, maximum.z },
        { 0.0f, 0.0f, 1.0f }, material);
    append_textured_quad(output,
        { minimum.x, minimum.y, minimum.z }, { minimum.x, maximum.y, minimum.z },
        { maximum.x, maximum.y, minimum.z }, { maximum.x, minimum.y, minimum.z },
        { 0.0f, 0.0f, -1.0f }, material);
}

void append_crossed_card(std::vector<VoxelVertex>& output,
                         const glm::vec3& center, float width, float height,
                         BlockType material) {
    const float radius = width * 0.5f;
    const float diagonalRadius = radius * 0.70710678f;
    const glm::vec3 first0(center.x - diagonalRadius, center.y,
                           center.z - diagonalRadius);
    const glm::vec3 first1(center.x + diagonalRadius, center.y,
                           center.z + diagonalRadius);
    const glm::vec3 first2 = first1 + glm::vec3(0.0f, height, 0.0f);
    const glm::vec3 first3 = first0 + glm::vec3(0.0f, height, 0.0f);
    append_textured_quad_uv(output, first0, first1, first2, first3,
                            glm::normalize(glm::vec3(-1.0f, 0.0f, 1.0f)),
                            material, width, height, true);

    const glm::vec3 second0(center.x + diagonalRadius, center.y,
                            center.z - diagonalRadius);
    const glm::vec3 second1(center.x - diagonalRadius, center.y,
                            center.z + diagonalRadius);
    const glm::vec3 second2 = second1 + glm::vec3(0.0f, height, 0.0f);
    const glm::vec3 second3 = second0 + glm::vec3(0.0f, height, 0.0f);
    append_textured_quad_uv(output, second0, second1, second2, second3,
                            glm::normalize(glm::vec3(-1.0f, 0.0f, -1.0f)),
                            material, width, height, true);
}

glm::vec4 foliage_proxy_tint(BlockType species, const glm::vec3& normal) {
    // FAR crowns always sample the shared green cluster atlas.  Keep species
    // variation in an explicit chromatic tint instead of inheriting the very
    // pale generic leaf material (which made minified cards read as grey).
    glm::vec4 tint(0.88f, 1.04f, 0.82f, 1.0f);
    if (species == BlockType::LeavesSpruce) {
        tint = glm::vec4(0.66f, 0.91f, 0.70f, 1.0f);
    } else if (species == BlockType::LeavesBirch) {
        tint = glm::vec4(1.02f, 1.08f, 0.73f, 1.0f);
    }
    tint.a = kFarVertexMarker;
    return tint;
}

void append_foliage_quad(std::vector<VoxelVertex>& output,
                         const glm::vec3& p0, const glm::vec3& p1,
                         const glm::vec3& p2, const glm::vec3& p3,
                         const glm::vec3& normal, BlockType species,
                         float salt, bool doubleSided = true) {
    const glm::vec3 center = (p0 + p1 + p2 + p3) * 0.25f;
    const int tile = std::min(3, static_cast<int>(
        hash_tree(center.x, center.z, salt) * 4.0f));
    // TextureIndex::Leaves is the generated four-variant alpha atlas used by
    // dense foliage. Species colour is a tint, never a switch to the grayscale
    // resource-pack layers that caused grey/purple FAR crowns.
    const glm::vec2 tileOrigin(static_cast<float>(tile & 1) * 0.5f,
                               static_cast<float>(tile >> 1) * 0.5f);
    const glm::vec2 uvMinimum = tileOrigin + glm::vec2(0.006f);
    const glm::vec2 uvMaximum = tileOrigin + glm::vec2(0.494f);
    append_atlas_quad_uv(output, p0, p1, p2, p3, normal,
                         static_cast<float>(TextureIndex::Leaves),
                         foliage_proxy_tint(species, normal),
                         uvMinimum, uvMaximum, doubleSided);
}

void append_foliage_crossed_card(std::vector<VoxelVertex>& output,
                                 const glm::vec3& center, float width,
                                 float height, BlockType species, float salt) {
    const float diagonalRadius = width * 0.35355339f;
    const glm::vec3 first0(center.x - diagonalRadius, center.y,
                           center.z - diagonalRadius);
    const glm::vec3 first1(center.x + diagonalRadius, center.y,
                           center.z + diagonalRadius);
    append_foliage_quad(output, first0, first1,
                        first1 + glm::vec3(0.0f, height, 0.0f),
                        first0 + glm::vec3(0.0f, height, 0.0f),
                        glm::normalize(glm::vec3(-1.0f, 0.0f, 1.0f)),
                        species, salt, true);

    const glm::vec3 second0(center.x + diagonalRadius, center.y,
                            center.z - diagonalRadius);
    const glm::vec3 second1(center.x - diagonalRadius, center.y,
                            center.z + diagonalRadius);
    append_foliage_quad(output, second0, second1,
                        second1 + glm::vec3(0.0f, height, 0.0f),
                        second0 + glm::vec3(0.0f, height, 0.0f),
                        glm::normalize(glm::vec3(-1.0f, 0.0f, -1.0f)),
                        species, salt + 1.0f, true);
}

void append_foliage_horizontal_cluster(std::vector<VoxelVertex>& output,
                                       const glm::vec3& center, float width,
                                       float depth, BlockType species,
                                       float salt) {
    // Vertical crossed cards disappear when the player looks down from the
    // high-altitude LOD tuning view.  A rotated horizontal cluster supplies a
    // real crown footprint without reverting to the old opaque leaf cube.
    const float phase = hash_tree(center.x, center.z, salt) * 6.28318530718f;
    const glm::vec2 axis(std::cos(phase), std::sin(phase));
    const glm::vec2 perpendicular(-axis.y, axis.x);
    const glm::vec2 halfWidth = axis * (width * 0.5f);
    const glm::vec2 halfDepth = perpendicular * (depth * 0.5f);
    append_foliage_quad(output,
        { center.x - halfWidth.x - halfDepth.x, center.y,
          center.z - halfWidth.y - halfDepth.y },
        { center.x + halfWidth.x - halfDepth.x, center.y,
          center.z + halfWidth.y - halfDepth.y },
        { center.x + halfWidth.x + halfDepth.x, center.y,
          center.z + halfWidth.y + halfDepth.y },
        { center.x - halfWidth.x + halfDepth.x, center.y,
          center.z - halfWidth.y + halfDepth.y },
        { 0.0f, 1.0f, 0.0f }, species, salt + 0.37f, true);
}

void append_tree_proxy(std::vector<VoxelVertex>& output,
                       float blockX, float groundTop, float blockZ,
                       int trunkHeight, const TreeProfile& profile,
                       int spacing, float retainedQuality,
                       std::size_t vertexBudget) {
    // Every tier uses foliage cards rather than replacing the first FAR crown
    // with an opaque cube. Quality only removes secondary lobes progressively;
    // the trunk/crown silhouette and real leaf alpha survive the whole fade.
    if (output.size() + 60u > vertexBudget) return;
    const std::size_t firstVertex = output.size();
    const bool spruce = profile.wood == BlockType::WoodSpruce;
    retainedQuality = std::clamp(retainedQuality, 0.00001f, 1.0f);
    const float lodScale = spacing <= 1 ? 1.0f : std::min(1.68f,
        1.0f + std::log2(static_cast<float>(spacing)) * 0.105f);
    const float centerX = blockX + 0.5f;
    const float centerZ = blockZ + 0.5f;
    const float trunkTop = groundTop + static_cast<float>(trunkHeight);

    if (spacing <= 2) {
        append_open_box(output,
                        { blockX, groundTop, blockZ },
                        { blockX + 1.0f, trunkTop, blockZ + 1.0f }, profile.wood);
    } else {
        const float trunkCardWidth = std::min(2.25f, 1.0f + 0.055f * spacing);
        append_crossed_card(output, { centerX, groundTop, centerZ }, trunkCardWidth,
                            static_cast<float>(trunkHeight), profile.wood);
    }

    const float crownQualityScale = glm::mix(0.92f, 1.46f,
        glm::smoothstep(0.18f, 0.985f, retainedQuality));
    const float crownWidth = (spruce ? 5.4f : 7.0f) * lodScale * crownQualityScale;
    const float crownHeight = (spruce ? 6.8f : 5.8f) * lodScale *
                              glm::mix(0.96f, 1.22f, crownQualityScale - 0.92f);
    const float crownBottom = trunkTop - (spruce ? 4.4f : 3.3f) * lodScale;
    append_foliage_crossed_card(output, { centerX, crownBottom, centerZ },
                                crownWidth, crownHeight, profile.leaves, 83.0f);

    // Organic offset lobes replace the previous square leaf box close-up.
    // They are dropped one at a time as retained quality falls, avoiding a
    // single visible model swap at the detailed chunk boundary.
    const float qualityPreservation = std::clamp(
        (retainedQuality - 0.72f) / 0.26f, 0.0f, 1.0f);
    const float secondaryDetail = retainedQuality * glm::mix(
        1.0f / std::sqrt(static_cast<float>(std::max(1, spacing))),
        1.0f, qualityPreservation);
    if (secondaryDetail > 0.075f &&
        output.size() + 48u <= vertexBudget) {
        const float lateral = crownWidth * 0.27f;
        const float sideWidth = crownWidth * (spruce ? 0.68f : 0.62f);
        const float sideHeight = crownHeight * 0.76f;
        const float phase = hash_tree(blockX, blockZ, 81.0f) * 6.28318530718f;
        const glm::vec2 direction(std::cos(phase), std::sin(phase));
        append_foliage_crossed_card(output,
            { centerX + direction.x * lateral, crownBottom - crownHeight * 0.05f,
              centerZ + direction.y * lateral }, sideWidth, sideHeight,
            profile.leaves, 84.0f);
        append_foliage_crossed_card(output,
            { centerX - direction.x * lateral, crownBottom + crownHeight * 0.08f,
              centerZ - direction.y * lateral }, sideWidth * 0.88f,
            sideHeight * 0.86f, profile.leaves, 86.0f);
    }

    // High retained quality is a real foliage LOD, not the same two cards made
    // slightly larger. Add deterministic compact lobes until the crown reaches
    // the dense-tree silhouette; each lobe disappears independently along Q(d).
    const int qualityLobes = static_cast<int>(std::floor(
        glm::smoothstep(0.48f, 0.985f, retainedQuality) * 7.0f));
    for (int lobe = 0; lobe < qualityLobes &&
         output.size() + 24u <= vertexBudget; ++lobe) {
        const float salt = 90.0f + static_cast<float>(lobe) * 3.0f;
        const float phase = hash_tree(blockX, blockZ, salt) * 6.28318530718f;
        const float radius = crownWidth * (0.12f +
            hash_tree(blockX, blockZ, salt + 1.0f) * 0.25f);
        const float vertical = crownHeight * (0.08f +
            hash_tree(blockX, blockZ, salt + 2.0f) * 0.48f);
        const float lobeWidth = crownWidth * (0.34f +
            hash_tree(blockX, blockZ, salt + 3.0f) * 0.18f);
        append_foliage_crossed_card(output,
            { centerX + std::cos(phase) * radius,
              crownBottom + vertical,
              centerZ + std::sin(phase) * radius },
            lobeWidth, crownHeight * 0.44f, profile.leaves,
            120.0f + static_cast<float>(lobe) * 2.0f);
    }

    // Layered horizontal clusters make the same proxy readable both from the
    // ground and from above.  Each layer retains the alpha silhouette of one
    // atlas cluster, so this adds volume without exposing square card edges.
    const int horizontalLayers = 2 + static_cast<int>(std::floor(
        glm::smoothstep(0.42f, 0.985f, retainedQuality) * 2.0f));
    for (int layer = 0; layer < horizontalLayers &&
         output.size() + 12u <= vertexBudget; ++layer) {
        const float t = horizontalLayers <= 1 ? 0.5f
            : static_cast<float>(layer) / static_cast<float>(horizontalLayers - 1);
        const float verticalT = spruce ? glm::mix(0.16f, 0.88f, t)
                                       : glm::mix(0.24f, 0.82f, t);
        const float silhouette = spruce
            ? glm::mix(1.02f, 0.42f, t)
            : (0.76f + std::sin(t * 3.14159265f) * 0.28f);
        const float offsetPhase = hash_tree(blockX, blockZ,
            160.0f + static_cast<float>(layer)) * 6.28318530718f;
        const float offsetRadius = crownWidth * (spruce ? 0.035f : 0.075f) *
                                   (0.35f + 0.65f * t);
        append_foliage_horizontal_cluster(output,
            { centerX + std::cos(offsetPhase) * offsetRadius,
              crownBottom + crownHeight * verticalT,
              centerZ + std::sin(offsetPhase) * offsetRadius },
            crownWidth * silhouette,
            crownWidth * silhouette * (spruce ? 0.82f : 0.91f),
            profile.leaves, 170.0f + static_cast<float>(layer) * 2.0f);
    }

    // Encode the actual source-cell size in the FAR marker. The fragment
    // shader can now stop expensive close material work on very coarse tree
    // cards without confusing them with detailed foliage.
    const float marker = kFarVertexMarker + static_cast<float>(std::max(1, spacing));
    for (std::size_t i = firstVertex; i < output.size(); ++i)
        output[i].color.a = marker;
}

void append_triangle(std::vector<VoxelVertex>& output,
                     const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
                     BlockType material) {
    glm::vec3 normal = glm::cross(b - a, c - a);
    const float lengthSquared = glm::dot(normal, normal);
    if (lengthSquared < 1.0e-8f) normal = glm::vec3(0.0f, 1.0f, 0.0f);
    else normal *= glm::inversesqrt(lengthSquared);
    if (normal.y < 0.0f) normal = -normal;

    const glm::vec4 color = far_color(material, normal);
    const float layer = get_block_texture_layer(material, normal);
    const auto vertex = [&](const glm::vec3& position) {
        const glm::vec3 axis = glm::abs(normal);
        glm::vec2 uv(position.x, position.z);
        if (axis.x >= axis.y && axis.x >= axis.z) uv = glm::vec2(position.z, position.y);
        else if (axis.z >= axis.x && axis.z >= axis.y) uv = glm::vec2(position.x, position.y);
        return VoxelVertex{ position, normal, color,
                            glm::vec3(uv, layer) };
    };
    output.push_back(vertex(a));
    output.push_back(vertex(b));
    output.push_back(vertex(c));
}

void append_double_sided_skirt(std::vector<VoxelVertex>& output,
                               const glm::vec3& a, const glm::vec3& b,
                               float depth, BlockType material) {
    const glm::vec3 down(0.0f, depth, 0.0f);
    const glm::vec3 ad = a - down;
    const glm::vec3 bd = b - down;
    append_triangle(output, a, b, bd, material);
    append_triangle(output, a, bd, ad, material);
    append_triangle(output, b, a, ad, material);
    append_triangle(output, b, ad, bd, material);
}

void append_water_quad(std::vector<VoxelVertex>& output,
                       float x0, float z0, float x1, float z1, float bias) {
    const float y = static_cast<float>(TerrainGenerator::SeaLevel) + 0.92f - bias;
    const glm::vec3 normal(0.0f, 1.0f, 0.0f);
    const glm::vec4 color = far_color(BlockType::Water, normal);
    const float layer = get_block_texture_layer(BlockType::Water, normal);
    const auto vertex = [&](float x, float z) {
        return VoxelVertex{ glm::vec3(x, y, z), normal, color,
                            glm::vec3(x, z, layer) };
    };
    const VoxelVertex sw = vertex(x0, z0);
    const VoxelVertex se = vertex(x1, z0);
    const VoxelVertex ne = vertex(x1, z1);
    const VoxelVertex nw = vertex(x0, z1);
    output.insert(output.end(), { nw, ne, se, nw, se, sw });
}

void append_ring(FarTerrain::BuildResult& result, float centerX, float centerZ,
                  const RingSpec& ring) {
    const std::size_t firstSurfaceInstance = result.surfaceInstances.size();
    const int cells = (ring.outerHalfExtent * 2) / ring.spacing;
    const float originX = centerX - static_cast<float>(ring.outerHalfExtent);
    const float originZ = centerZ - static_cast<float>(ring.outerHalfExtent);
    const std::size_t side = static_cast<std::size_t>(cells + 1);
    std::vector<TerrainSample> samples(side * side);
    const float qualityWeight = std::sqrt(std::clamp(
        ring.retainedQuality, 0.00001f, 1.0f));
    const std::size_t grassVertexBudget = ring.level == 0
        ? kBaseGrassProxyVertices
        : (ring.level == 1 ? kFirstRingGrassProxyVertices
                           : static_cast<std::size_t>(kOuterRingGrassProxyVertices *
                                                     glm::mix(0.25f, 1.0f, qualityWeight)));
    const std::size_t treeVertexBudget = ring.level == 0
        ? kBaseTreeProxyVertices
        : static_cast<std::size_t>(kOuterRingTreeProxyVertices *
                                   glm::mix(0.30f, 1.0f, qualityWeight));
    std::vector<VoxelVertex> grassVertices;
    std::vector<VoxelVertex> treeVertices;
    grassVertices.reserve(grassVertexBudget);
    treeVertices.reserve(treeVertexBudget);

    const int innerCellSpan = ring.innerHalfExtent <= 0 ? 0 :
        std::min(cells, (ring.innerHalfExtent * 2) / ring.spacing);
    const std::size_t estimatedVisibleCells = std::max<std::size_t>(1u,
        static_cast<std::size_t>(cells) * static_cast<std::size_t>(cells) -
        static_cast<std::size_t>(innerCellSpan) * static_cast<std::size_t>(innerCellSpan));
    const float expectedGrassVertices = ring.spacing <= 2 ? 14.0f : 12.0f;
    const float grassBudgetScale = std::min(1.0f,
        static_cast<float>(grassVertexBudget) /
        (static_cast<float>(estimatedVisibleCells) * expectedGrassVertices));
    const auto sample_index = [side](int x, int z) {
        return static_cast<std::size_t>(z) * side + static_cast<std::size_t>(x);
    };
    const auto sample_needed = [&](int x, int z, bool slopeHalo) {
        if (ring.innerHalfExtent <= 0) return true;
        const float worldX = originX + static_cast<float>(x * ring.spacing);
        const float worldZ = originZ + static_cast<float>(z * ring.spacing);
        const float threshold = static_cast<float>(ring.innerHalfExtent -
            (slopeHalo ? ring.spacing : 0));
        return std::abs(worldX - centerX) >= threshold ||
               std::abs(worldZ - centerZ) >= threshold;
    };

    for (int z = 0; z <= cells; ++z) {
        for (int x = 0; x <= cells; ++x) {
            // The middle of an annulus is owned by finer levels. Keep only a
            // one-sample halo so central-difference slopes remain available.
            if (!sample_needed(x, z, true)) continue;
            samples[sample_index(x, z)] = TerrainGenerator::sample_coarse(
                originX + static_cast<float>(x * ring.spacing),
                originZ + static_cast<float>(z * ring.spacing));
        }
    }

    for (int z = 0; z <= cells; ++z) {
        for (int x = 0; x <= cells; ++x) {
            if (!sample_needed(x, z, false)) continue;
            const int west = std::max(0, x - 1);
            const int east = std::min(cells, x + 1);
            const int north = std::max(0, z - 1);
            const int south = std::min(cells, z + 1);
            const float dx = static_cast<float>(samples[sample_index(east, z)].height -
                                                samples[sample_index(west, z)].height) /
                             static_cast<float>((east - west) * ring.spacing);
            const float dz = static_cast<float>(samples[sample_index(x, south)].height -
                                                samples[sample_index(x, north)].height) /
                             static_cast<float>((south - north) * ring.spacing);
            samples[sample_index(x, z)].slope = std::sqrt(dx * dx + dz * dz);
        }
    }

    const auto cell_is_visible = [&](int x, int z) {
        if (x < 0 || z < 0 || x >= cells || z >= cells) return false;
        if (ring.innerHalfExtent <= 0) return true;
        const float x0 = originX + static_cast<float>(x * ring.spacing);
        const float z0 = originZ + static_cast<float>(z * ring.spacing);
        const float x1 = x0 + static_cast<float>(ring.spacing);
        const float z1 = z0 + static_cast<float>(ring.spacing);
        return !(x0 >= centerX - ring.innerHalfExtent &&
                 x1 <= centerX + ring.innerHalfExtent &&
                 z0 >= centerZ - ring.innerHalfExtent &&
                 z1 <= centerZ + ring.innerHalfExtent);
    };

    const float bias = kTerrainBiasPerLevel * static_cast<float>(ring.level);
    const float skirtDepth = static_cast<float>(std::max(6, ring.spacing));
    for (int z = 0; z < cells; ++z) {
        for (int x = 0; x < cells; ++x) {
            if (!cell_is_visible(x, z)) continue;
            const TerrainSample& swSample = samples[sample_index(x, z)];
            const TerrainSample& seSample = samples[sample_index(x + 1, z)];
            const TerrainSample& neSample = samples[sample_index(x + 1, z + 1)];
            const TerrainSample& nwSample = samples[sample_index(x, z + 1)];
            const float x0 = originX + static_cast<float>(x * ring.spacing);
            const float z0 = originZ + static_cast<float>(z * ring.spacing);
            const float x1 = x0 + static_cast<float>(ring.spacing);
            const float z1 = z0 + static_cast<float>(ring.spacing);
            const glm::vec3 sw(x0, static_cast<float>(swSample.height + 1) - bias, z0);
            const glm::vec3 se(x1, static_cast<float>(seSample.height + 1) - bias, z0);
            const glm::vec3 ne(x1, static_cast<float>(neSample.height + 1) - bias, z1);
            const glm::vec3 nw(x0, static_cast<float>(nwSample.height + 1) - bias, z1);
            const BlockType material = far_surface_material(swSample);

            const int west = std::max(0, x - 1);
            const int east = std::min(cells, x + 1);
            const int south = std::max(0, z - 1);
            const int north = std::min(cells, z + 1);
            const float westHeight = static_cast<float>(samples[sample_index(west, z)].height + 1) - bias;
            const float eastHeight = static_cast<float>(samples[sample_index(east, z)].height + 1) - bias;
            const float southHeight = static_cast<float>(samples[sample_index(x, south)].height + 1) - bias;
            const float northHeight = static_cast<float>(samples[sample_index(x, north)].height + 1) - bias;
            const bool blocky = ring.blocky;
            const glm::vec3 up(0.0f, 1.0f, 0.0f);
            const glm::vec3 side(1.0f, 0.0f, 0.0f);
            result.surfaceInstances.push_back({
                glm::vec4(x0, sw.y, z0, static_cast<float>(ring.spacing)),
                glm::vec4(westHeight, eastHeight, southHeight, northHeight),
                far_color(material, up),
                glm::vec4(get_block_texture_layer(material, up),
                          get_block_texture_layer(material, side),
                          ring.smoothness, ne.y)
            });
            if (blocky) ++result.nearSurfaceInstanceCount;
            else ++result.farSurfaceInstanceCount;

            // Skirts make both the outer rim and every LOD transition watertight.
            // They point down, so a coarse level cannot rise through a finer one.
            if (!cell_is_visible(x - 1, z))
                append_double_sided_skirt(result.terrainVertices, sw, nw, skirtDepth, material);
            if (!cell_is_visible(x + 1, z))
                append_double_sided_skirt(result.terrainVertices, ne, se, skirtDepth, material);
            if (!cell_is_visible(x, z - 1))
                append_double_sided_skirt(result.terrainVertices, se, sw, skirtDepth, material);
            if (!cell_is_visible(x, z + 1))
                append_double_sided_skirt(result.terrainVertices, nw, ne, skirtDepth, material);

            const float proxyDistance = std::max(std::abs(x0 - centerX),
                                                 std::abs(z0 - centerZ));
            append_grass_proxy(grassVertices, swSample, x0, z0,
                               ring.spacing, ring.retainedQuality,
                               sw.y, se.y, ne.y, nw.y,
                               grassBudgetScale, grassVertexBudget);

            float treeX = x0;
            float treeZ = z0;
            TerrainSample treeSample = swSample;
            TreeProfile profile;
            bool selected = false;
            if (ring.spacing == 1) {
                // Chunk::generate_terrain deliberately considers only local
                // coordinates [4,11].  Reusing the same lattice and the same
                // hash makes the proxy occupy exactly the tree that replaces
                // it when its detailed chunk is published.
                const int blockX = static_cast<int>(std::lround(x0));
                const int blockZ = static_cast<int>(std::lround(z0));
                const int localX = positive_chunk_local(blockX);
                const int localZ = positive_chunk_local(blockZ);
                if (localX >= 4 && localX < CHUNK_SIZE_X - 4 &&
                    localZ >= 4 && localZ < CHUNK_SIZE_Z - 4) {
                    treeSample = TerrainGenerator::sample(x0, z0);
                    profile = tree_profile(treeSample);
                    selected = tree_has_grass_surface(treeSample) &&
                               profile.density > 0.0f &&
                               hash_tree(x0, z0, 0.0f) < profile.density;
                }
            } else {
                profile = tree_profile(treeSample);
                if (tree_has_grass_surface(treeSample) && profile.density > 0.0f) {
                    const float representedCandidates =
                        static_cast<float>(ring.spacing * ring.spacing) * 0.25f;
                    float probability = 1.0f - std::exp(-profile.density * representedCandidates);
                    // One representative is enough once several source trees
                    // project into the same coarse cell.  The cap falls
                    // continuously with cell size and prevents each 256x256
                    // clipmap level from becoming a forest-sized vertex dump.
                    probability = std::min(probability,
                        2.0f / static_cast<float>(ring.spacing));
                    probability *= std::clamp(
                        0.15f + 0.85f * std::sqrt(ring.retainedQuality),
                        0.01f, 1.0f);
                    selected = hash_tree(x0, z0, 19.0f) < probability;
                    if (selected) {
                        const int offsetX = std::min(ring.spacing - 1,
                            static_cast<int>(hash_tree(x0, z0, 20.0f) * ring.spacing));
                        const int offsetZ = std::min(ring.spacing - 1,
                            static_cast<int>(hash_tree(x0, z0, 21.0f) * ring.spacing));
                        treeX += static_cast<float>(offsetX);
                        treeZ += static_cast<float>(offsetZ);
                    }
                }
            }

            if (!selected) continue;
            int trunkHeight = 6 + static_cast<int>(hash_tree(treeX, treeZ, 1.0f) * 3.0f);
            if (treeSample.biome == BiomeType::Jungle) trunkHeight += 4;
            if (profile.wood == BlockType::WoodSpruce) trunkHeight += 2;
            if (treeSample.height + trunkHeight + 3 >= GENERATED_TERRAIN_HEIGHT) continue;

            float groundTop = static_cast<float>(treeSample.height + 1) - bias;
            if (ring.spacing > 1) {
                const float u = std::clamp((treeX - x0) / static_cast<float>(ring.spacing),
                                           0.0f, 1.0f);
                const float v = std::clamp((treeZ - z0) / static_cast<float>(ring.spacing),
                                           0.0f, 1.0f);
                const float southHeight = glm::mix(sw.y, se.y, u);
                const float northHeight = glm::mix(nw.y, ne.y, u);
                const float smoothGround = glm::mix(southHeight, northHeight, v);
                groundTop = glm::mix(sw.y, smoothGround, ring.smoothness);
            }
            append_tree_proxy(treeVertices, treeX, groundTop, treeZ,
                               trunkHeight, profile, ring.spacing,
                               ring.retainedQuality, treeVertexBudget);
        }
    }

    result.terrainVertices.insert(result.terrainVertices.end(),
                                  grassVertices.begin(), grassVertices.end());

    // The warped 2048x2048 shadow map has useful terrain resolution only in
    // the first 512 blocks. Rings are emitted from near to far, so this remains
    // one contiguous prefix and costs a single six-vertex instanced draw.
    if (ring.outerHalfExtent <= 512) {
        result.shadowSurfaceInstanceCount += static_cast<uint32_t>(
            result.surfaceInstances.size() - firstSurfaceInstance);
    }

    const uint32_t treeFirstVertex = static_cast<uint32_t>(result.terrainVertices.size());
    result.terrainVertices.insert(result.terrainVertices.end(),
                                  treeVertices.begin(), treeVertices.end());
    // The current warped shadow map has useful texel density for roughly the
    // first 512 blocks. Record only tree ranges in that footprint: distant
    // proxy geometry remains visible but cannot waste shadow bandwidth.
    if (ring.outerHalfExtent <= 512 && !treeVertices.empty()) {
        result.shadowDrawRanges.push_back({
            treeFirstVertex, static_cast<uint32_t>(treeVertices.size()) });
    }

    // Water never follows the one-block terrain grid. Fine clipmap cells are
    // grouped into at least 8x8-block quads, capping memory and draw bandwidth
    // while the vertex/fragment water shaders provide the missing wave detail.
    const int waterGroup = std::max(1, 8 / ring.spacing);
    for (int z = 0; z + waterGroup <= cells; z += waterGroup) {
        for (int x = 0; x + waterGroup <= cells; x += waterGroup) {
            if (!cell_is_visible(x, z) ||
                !cell_is_visible(x + waterGroup - 1, z + waterGroup - 1)) continue;
            const TerrainSample& swSample = samples[sample_index(x, z)];
            const TerrainSample& seSample = samples[sample_index(x + waterGroup, z)];
            const TerrainSample& neSample = samples[sample_index(x + waterGroup, z + waterGroup)];
            const TerrainSample& nwSample = samples[sample_index(x, z + waterGroup)];
            const int averageHeight = (swSample.height + seSample.height +
                                       neSample.height + nwSample.height) / 4;
            if (averageHeight >= TerrainGenerator::SeaLevel) continue;
            const float x0 = originX + static_cast<float>(x * ring.spacing);
            const float z0 = originZ + static_cast<float>(z * ring.spacing);
            const float size = static_cast<float>(waterGroup * ring.spacing);
            append_water_quad(result.waterVertices, x0, z0, x0 + size, z0 + size, bias);
        }
    }
}
}

FarTerrain::BuildResult FarTerrain::build(uint64_t version, int centerChunkX,
                                           int centerChunkZ, int reachChunks,
                                           float endpointQualityFraction,
                                           const std::atomic_uint64_t* latestRequestedVersion) {
    const auto start = std::chrono::steady_clock::now();
    BuildResult result;
    result.version = version;
    result.centerChunkX = centerChunkX;
    result.centerChunkZ = centerChunkZ;
    result.reachChunks = reachChunks;
    if (reachChunks <= 0) return result;

    const int reachBlocks = reachChunks * CHUNK_SIZE_X;
    const float centerX = static_cast<float>(centerChunkX * CHUNK_SIZE_X) +
                          static_cast<float>(CHUNK_SIZE_X) * 0.5f;
    const float centerZ = static_cast<float>(centerChunkZ * CHUNK_SIZE_Z) +
                          static_cast<float>(CHUNK_SIZE_Z) * 0.5f;

    endpointQualityFraction = std::clamp(endpointQualityFraction, 1.0e-5f, 0.99f);
    result.endpointQualityFraction = endpointQualityFraction;
    const int baseHalfExtent = std::min(kBaseHalfExtentBlocks, reachBlocks);
    std::vector<RingSpec> rings;
    rings.push_back({ 0, baseHalfExtent, 1, 0, true, 0.0f, 1.0f });
    int coveredHalfExtent = baseHalfExtent;
    int spacing = 2;
    while (coveredHalfExtent < reachBlocks) {
        const int nextHalfExtent = std::min(coveredHalfExtent * 2, reachBlocks);
        const float normalizedDistance = std::clamp(
            static_cast<float>(nextHalfExtent) / static_cast<float>(reachBlocks),
            0.0f, 1.0f);
        const float ringQuality = std::pow(endpointQualityFraction, normalizedDistance);
        spacing = ring_spacing_for_quality(nextHalfExtent, ringQuality);
        // Q(d) is the single authority. There is deliberately no independent
        // spacing or screen-size switch here: at an endpoint of 99%, every ring
        // keeps its voxel silhouette; lowering the endpoint morphs every ring
        // progressively toward the six-vertex height field.
        const float blockyAmount = glm::smoothstep(0.55f, 0.985f, ringQuality);
        const float smoothness = 1.0f - blockyAmount;
        rings.push_back({ coveredHalfExtent, nextHalfExtent, spacing,
                          static_cast<int>(rings.size()),
                          blockyAmount > 1.0e-4f, smoothness, ringQuality });
        coveredHalfExtent = nextHalfExtent;
    }
    result.clipmapLevels = static_cast<int>(rings.size());

    // Reserve the exact clipmap surface footprint. At 99% this avoids several
    // hundred megabytes of transient vector copies while a new quality setting
    // is being built in the background.
    std::size_t surfaceReserve = 0;
    for (const RingSpec& ring : rings) {
        const std::size_t cells = static_cast<std::size_t>(
            (ring.outerHalfExtent * 2) / ring.spacing);
        const std::size_t inner = ring.innerHalfExtent <= 0 ? 0u :
            static_cast<std::size_t>((ring.innerHalfExtent * 2) / ring.spacing);
        surfaceReserve += cells * cells - std::min(cells * cells, inner * inner);
    }
    result.surfaceInstances.reserve(surfaceReserve);
    result.terrainVertices.reserve(4'600'000u);
    result.waterVertices.reserve(rings.size() * 2048u * 6u);
    for (const RingSpec& ring : rings) {
        if (latestRequestedVersion &&
            latestRequestedVersion->load(std::memory_order_acquire) != version) {
            return result;
        }
        append_ring(result, centerX, centerZ, ring);
    }

    result.buildMicroseconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count());
    return result;
}

void FarTerrain::request(ThreadPool& pool, int centerChunkX, int centerChunkZ,
                         int reachChunks, float endpointQualityFraction) {
    endpointQualityFraction = std::clamp(endpointQualityFraction, 1.0e-5f, 0.99f);
    const int snapQuantum = snap_quantum_chunks(std::max(1, reachChunks));
    centerChunkX = snap_nearest(centerChunkX, snapQuantum);
    centerChunkZ = snap_nearest(centerChunkZ, snapQuantum);

    uint64_t version = 0;
    {
        std::lock_guard lock(requestMutex);
        const bool changed = centerChunkX != requestedCenterChunkX ||
                             centerChunkZ != requestedCenterChunkZ ||
                             reachChunks != requestedBudget ||
                             std::abs(endpointQualityFraction - requestedEndpointQuality) > 1.0e-9f;
        if (changed) {
            requestedCenterChunkX = centerChunkX;
            requestedCenterChunkZ = centerChunkZ;
            requestedBudget = reachChunks;
            requestedEndpointQuality = endpointQualityFraction;
            ++requestedVersion;
            latestRequestedVersion.store(requestedVersion, std::memory_order_release);
        }
        if (building.load(std::memory_order_acquire)) return;
        std::lock_guard readyLock(readyMutex);
        if (readyResult && readyResult->version == requestedVersion) return;
        if (uploadedVersion == requestedVersion) return;
        version = requestedVersion;
        centerChunkX = requestedCenterChunkX;
        centerChunkZ = requestedCenterChunkZ;
        reachChunks = requestedBudget;
        endpointQualityFraction = requestedEndpointQuality;
        building.store(true, std::memory_order_release);
    }

    pool.enqueue([this, version, centerChunkX, centerChunkZ, reachChunks,
                  endpointQualityFraction]() {
        BuildResult result = build(version, centerChunkX, centerChunkZ, reachChunks,
                                   endpointQualityFraction, &latestRequestedVersion);
        bool current = false;
        {
            std::lock_guard lock(requestMutex);
            current = version == requestedVersion;
        }
        if (current) {
            std::lock_guard lock(readyMutex);
            readyResult = std::move(result);
        }
        building.store(false, std::memory_order_release);
    });
}

void FarTerrain::upload_ready(VkDevice device, VmaAllocator allocator,
                              std::vector<AllocatedBuffer>* retiredBuffers) {
    std::optional<BuildResult> result;
    {
        std::lock_guard lock(readyMutex);
        if (!readyResult) return;
        result = std::move(readyResult);
        readyResult.reset();
    }
    {
        std::lock_guard lock(requestMutex);
        if (result->version != requestedVersion) return;
    }

    auto retire = [&](AllocatedBuffer& buffer) {
        if (buffer.buffer == VK_NULL_HANDLE) return;
        if (retiredBuffers) retiredBuffers->push_back(buffer);
        else {
            vkDeviceWaitIdle(device);
            vmaDestroyBuffer(allocator, buffer.buffer, buffer.allocation);
        }
        buffer = {};
    };
    auto upload = [&](const void* data, std::size_t byteCount, AllocatedBuffer& buffer) {
        if (data == nullptr || byteCount == 0) return;
        VkBufferCreateInfo bufferInfo{ .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bufferInfo.size = static_cast<VkDeviceSize>(byteCount);
        bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        VmaAllocationCreateInfo allocationInfo{};
        allocationInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        VK_CHECK(vmaCreateBuffer(allocator, &bufferInfo, &allocationInfo,
                                 &buffer.buffer, &buffer.allocation, nullptr));
        void* mapped = nullptr;
        VK_CHECK(vmaMapMemory(allocator, buffer.allocation, &mapped));
        std::memcpy(mapped, data, byteCount);
        VK_CHECK(vmaFlushAllocation(allocator, buffer.allocation, 0, bufferInfo.size));
        vmaUnmapMemory(allocator, buffer.allocation);
    };

    AllocatedBuffer newTerrain;
    AllocatedBuffer newSurface;
    AllocatedBuffer newWater;
    upload(result->terrainVertices.data(),
           result->terrainVertices.size() * sizeof(VoxelVertex), newTerrain);
    upload(result->surfaceInstances.data(),
           result->surfaceInstances.size() * sizeof(FarSurfaceInstance), newSurface);
    upload(result->waterVertices.data(),
           result->waterVertices.size() * sizeof(VoxelVertex), newWater);
    retire(terrainBuffer);
    retire(surfaceBuffer);
    retire(waterBuffer);
    terrainBuffer = newTerrain;
    surfaceBuffer = newSurface;
    waterBuffer = newWater;
    terrainVertexCount = static_cast<uint32_t>(result->terrainVertices.size());
    nearSurfaceInstanceCount = result->nearSurfaceInstanceCount;
    farSurfaceInstanceCount = result->farSurfaceInstanceCount;
    shadowSurfaceInstanceCount = result->shadowSurfaceInstanceCount;
    farSurfaceBufferOffset = static_cast<VkDeviceSize>(nearSurfaceInstanceCount) *
                             sizeof(FarSurfaceInstance);
    waterVertexCount = static_cast<uint32_t>(result->waterVertices.size());
    shadowDrawRanges = std::move(result->shadowDrawRanges);
    representedReachChunks.store(result->reachChunks, std::memory_order_release);
    publishedClipmapLevels.store(result->clipmapLevels, std::memory_order_release);
    lastBuildMicroseconds.store(result->buildMicroseconds, std::memory_order_release);
    publishedEndpointQuality.store(result->endpointQualityFraction, std::memory_order_release);
    {
        std::lock_guard lock(requestMutex);
        uploadedVersion = result->version;
    }
    VC_LOG_INFO("[FAR LOD] aplicado {}% | {} niveis | {} celulas | {} vertices | {:.1f} ms",
                (result->endpointQualityFraction * 100.0f), result->clipmapLevels,
                result->surfaceInstances.size(), result->terrainVertices.size(),
                (static_cast<double>(result->buildMicroseconds) / 1000.0));
}

void FarTerrain::draw(VkCommandBuffer cmd) const {
    if (terrainVertexCount == 0 || terrainBuffer.buffer == VK_NULL_HANDLE) return;
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &terrainBuffer.buffer, &offset);
    vkCmdDraw(cmd, terrainVertexCount, 1, 0, 0);
}

void FarTerrain::draw_near_surface(VkCommandBuffer cmd) const {
    if (nearSurfaceInstanceCount == 0 || surfaceBuffer.buffer == VK_NULL_HANDLE) return;
    constexpr VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &surfaceBuffer.buffer, &offset);
    vkCmdDraw(cmd, 30, nearSurfaceInstanceCount, 0, 0);
}

void FarTerrain::draw_far_surface(VkCommandBuffer cmd) const {
    if (farSurfaceInstanceCount == 0 || surfaceBuffer.buffer == VK_NULL_HANDLE) return;
    vkCmdBindVertexBuffers(cmd, 0, 1, &surfaceBuffer.buffer, &farSurfaceBufferOffset);
    vkCmdDraw(cmd, 6, farSurfaceInstanceCount, 0, 0);
}

void FarTerrain::draw_water(VkCommandBuffer cmd) const {
    if (waterVertexCount == 0 || waterBuffer.buffer == VK_NULL_HANDLE) return;
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &waterBuffer.buffer, &offset);
    vkCmdDraw(cmd, waterVertexCount, 1, 0, 0);
}

void FarTerrain::draw_surface_shadow(VkCommandBuffer cmd) const {
    if (shadowSurfaceInstanceCount == 0 || surfaceBuffer.buffer == VK_NULL_HANDLE) return;
    constexpr VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &surfaceBuffer.buffer, &offset);
    vkCmdDraw(cmd, 6, shadowSurfaceInstanceCount, 0, 0);
}

void FarTerrain::draw_shadow(VkCommandBuffer cmd) const {
    if (terrainBuffer.buffer == VK_NULL_HANDLE || shadowDrawRanges.empty()) return;
    constexpr VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &terrainBuffer.buffer, &offset);
    for (const auto& range : shadowDrawRanges) {
        if (range[1] > 0) vkCmdDraw(cmd, range[1], 1, range[0], 0);
    }
}

void FarTerrain::cleanup(VkDevice device, VmaAllocator allocator, bool deviceAlreadyIdle) {
    if (!deviceAlreadyIdle && (terrainBuffer.buffer != VK_NULL_HANDLE ||
                               surfaceBuffer.buffer != VK_NULL_HANDLE ||
                               waterBuffer.buffer != VK_NULL_HANDLE)) {
        vkDeviceWaitIdle(device);
    }
    if (terrainBuffer.buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator, terrainBuffer.buffer, terrainBuffer.allocation);
        terrainBuffer = {};
    }
    if (surfaceBuffer.buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator, surfaceBuffer.buffer, surfaceBuffer.allocation);
        surfaceBuffer = {};
    }
    if (waterBuffer.buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator, waterBuffer.buffer, waterBuffer.allocation);
        waterBuffer = {};
    }
    terrainVertexCount = 0;
    nearSurfaceInstanceCount = 0;
    farSurfaceInstanceCount = 0;
    shadowSurfaceInstanceCount = 0;
    farSurfaceBufferOffset = 0;
    waterVertexCount = 0;
    shadowDrawRanges.clear();
    representedReachChunks.store(0, std::memory_order_release);
    publishedClipmapLevels.store(0, std::memory_order_release);
    lastBuildMicroseconds.store(0, std::memory_order_release);
    publishedEndpointQuality.store(0.0f, std::memory_order_release);
}
