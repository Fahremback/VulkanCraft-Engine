#pragma once

// IMeshGeometryProcessing — computational geometry seam backed by geometry-central.
// Provides surface-mesh geodesic distance, heat-method smoothing, and
// quadric-error-decimation. The adapter is the ONLY TU that includes
// geometry-central headers; callers see only std+glm.
//
// Deterministic: identical inputs produce identical outputs (geometry-central
// is deterministic for fixed input mesh). Headless, no GPU required.

#include <array>
#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Engine::Procgen {

struct MeshGeometryConfig {
    std::size_t maxVertices{ 65536 };
    std::size_t maxFaces{ 131072 };

    bool validate() const {
        return maxVertices >= 3 && maxFaces >= 1;
    }
};

// A triangle mesh: positions + triangle indices (triplets of vertex indices).
struct TriMesh {
    std::vector<glm::vec3> positions;
    std::vector<std::array<std::uint32_t, 3>> indices;

    bool validate() const {
        if (positions.size() < 3 || indices.empty()) return false;
        for (auto& tri : indices) {
            for (auto idx : tri) {
                if (idx >= positions.size()) return false;
            }
        }
        return true;
    }
};

using MeshHandle = std::uint64_t;
constexpr MeshHandle InvalidMesh = 0;

class IMeshGeometryProcessing {
public:
    virtual ~IMeshGeometryProcessing() = default;

    // Upload a triangle mesh; returns a handle for subsequent operations.
    virtual MeshHandle upload_mesh(const TriMesh& mesh, std::string& errorOut) = 0;
    virtual bool release_mesh(MeshHandle h) = 0;

    // Geodesic distance from a source vertex to all others (Dijkstra/heat method).
    // Returns per-vertex distances (same order as upload).
    virtual std::vector<float> geodesic_distance(
        MeshHandle h, std::uint32_t sourceVertex, std::string& errorOut) const = 0;

    // Laplacian smoothing: returns new positions after 1 pass of umbrella-weight
    // smoothing. lambda in (0,1] controls blend with original.
    virtual std::vector<glm::vec3> smooth_positions(
        MeshHandle h, float lambda, std::string& errorOut) const = 0;

    // Quadric-error-decimation: reduces face count toward targetFaces.
    // Returns the simplified mesh (new positions + indices).
    virtual TriMesh decimate(
        MeshHandle h, std::size_t targetFaces, std::string& errorOut) const = 0;
};

std::unique_ptr<IMeshGeometryProcessing> create_mesh_geometry_processor(
    const MeshGeometryConfig& config, std::string& errorOut);

}  // namespace Engine::Procgen
