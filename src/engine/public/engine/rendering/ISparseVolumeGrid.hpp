// ISparseVolumeGrid — Agente 1 (task_plan C.8 openvdb): the PUBLIC sparse
// volume grid contract. Headless, deterministic, self-contained std+glm.
//
// OpenVDB-style sparse volume: brick tree with 8x8x8 leaf nodes, leaf-centric
// accessor, fog-of-war decay by distance, and SDF (signed distance field) operations.
// All-or-nothing: any invalid input → error + all outputs unchanged.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <glm/vec3.hpp>

namespace vc::rendering {

// Configuration — all-or-nothing.
struct SparseVolumeConfig {
    // Grid dimensions in bricks (each brick = 8^3 voxels).
    int bricksX = 16;
    int bricksY = 16;
    int bricksZ = 16;
    // Fog-of-war: maximum reveal distance in bricks.
    float fogMaxDist = 8.0f;
    // Fog-of-war: decay rate (higher = faster falloff).
    float fogDecay = 2.0f;
    // SDF: iso-surface threshold.
    float sdfThreshold = 0.0f;

    bool validate() const;
    std::string toJson() const;
    static SparseVolumeConfig fromJson(const std::string& json, std::string& errorOut);
};

// Sparse brick — 8x8x8 float voxels.
struct VolumeBrick {
    static constexpr int SIZE = 8;
    float voxels[SIZE * SIZE * SIZE] = {};
};

// Accessor result for a single voxel query.
struct VolumeSample {
    float value = 0.0f;
    bool active = false;  // Is this voxel in an active brick?
};

// The public contract.
class ISparseVolumeGrid {
public:
    virtual ~ISparseVolumeGrid() = default;

    // Set/get a voxel value. Activates the brick if needed.
    virtual void setVoxel(int x, int y, int z, float value) = 0;
    virtual float getVoxel(int x, int y, int z) const = 0;

    // Trilinear interpolation at fractional coordinates.
    virtual float sample(float x, float y, float z) const = 0;

    // SDF: compute signed distance to iso-surface at a point.
    virtual float sdf(float x, float y, float z) const = 0;

    // Fog-of-war: apply distance-based decay from a center point.
    // Returns the decayed value at each voxel.
    virtual float fogOfWar(int x, int y, int z,
                           float centerX, float centerY, float centerZ) const = 0;

    // Flood fill: activate all connected bricks within a radius.
    virtual void floodFill(int cx, int cy, int cz, int radius) = 0;

    // Count active bricks.
    virtual int activeBrickCount() const = 0;

    // Get a brick by index (for iteration).
    virtual const VolumeBrick* getBrick(int bx, int by, int bz) const = 0;

    // Serialization round-trip (deterministic).
    virtual std::vector<uint32_t> serialize() const = 0;
    virtual bool deserialize(const uint32_t* data, size_t wordCount, std::string& errorOut) = 0;
};

// Factory.
std::unique_ptr<ISparseVolumeGrid> create_sparse_volume_grid(
    const SparseVolumeConfig& config, std::string& errorOut);

} // namespace vc::rendering
