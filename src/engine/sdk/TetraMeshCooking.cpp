// TetraMeshCooking.cpp
//
// The ITetraMeshCooking adapter (FALTANTES §16 item 4): a self-contained,
// deterministic tetrahedralizer for the voxel family (the specialized
// fTetWild cooker stays out of the runtime base — DEPENDENCY_POLICY). The
// only TU that crosses into the tetrahedralizer.
//
//   simulation — every solid voxel becomes the canonical 6-tet decomposition:
//                all six tetrahedra share the voxel's min->max body diagonal,
//                so adjacent voxels split their shared square face on the
//                SAME physical diagonal (water-tight) and interior faces
//                cancel. Nodes are deduplicated by grid position (first
//                insertion wins — the y,z,x scan order).
//   collider   — per-voxel box triangles (12 per voxel, internal faces kept):
//                the conservative physics proxy of a voxel solid.
//   render     — the deduplicated EXPOSED surface (2 triangles per face with
//                an air neighbor) on the voxel path; the INPUT triangles on
//                the triangle-mesh path.
//
// Deterministic: fixed scan order, hash lookups only (never iterated), no
// randomness — bit-identical meshes for identical inputs.

#include "engine/deformable/ITetraMeshCooking.hpp"

#include "engine/sdk/RegistryJson.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>
#include <unordered_map>

namespace Engine::Deformable {

namespace {

// The 6-tet decomposition of a cube: all tetrahedra share the min->max body
// diagonal (local corners 0 and 6). The 6 remaining corners form a hexagonal
// cycle around the diagonal; each tet = diagonal + one cycle edge.
constexpr int kTetCycle[6][2] = { { 1, 2 }, { 2, 3 }, { 3, 7 },
                                  { 7, 4 }, { 4, 5 }, { 5, 1 } };

struct GridKey {
    int x, y, z;
    bool operator==(const GridKey& other) const noexcept {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct GridKeyHash {
    std::size_t operator()(const GridKey& key) const noexcept {
        std::size_t h = 1469598103934665603ull;
        const auto mix = [&h](int v) {
            h ^= static_cast<std::size_t>(v);
            h *= 1099511628211ull;
        };
        mix(key.x);
        mix(key.y);
        mix(key.z);
        return h;
    }
};

// Local corner positions of a unit cube (0..7), in the order used by the
// diagonal decomposition above.
constexpr glm::ivec3 kCubeCorner[8] = {
    { 0, 0, 0 }, { 1, 0, 0 }, { 1, 1, 0 }, { 0, 1, 0 },
    { 0, 0, 1 }, { 1, 0, 1 }, { 1, 1, 1 }, { 0, 1, 1 },
};

// One voxel's 6 faces (local corner pairs) for the collider/render surface.
constexpr int kCubeFace[6][4] = {
    { 0, 1, 2, 3 },  // -z
    { 4, 5, 6, 7 },  // +z
    { 0, 1, 5, 4 },  // -y
    { 3, 2, 6, 7 },  // +y
    { 0, 3, 7, 4 },  // -x
    { 1, 2, 6, 5 },  // +x
};

bool config_valid(const TetraCookingConfig& config, std::string& errorOut) {
    if (config.maxTets < 1 || config.maxTets > 10000000) {
        errorOut = "tetra cooking config: maxTets must be in [1, 10000000]";
        return false;
    }
    return true;
}

class TetraCooker final : public ITetraMeshCooking {
public:
    explicit TetraCooker(const TetraCookingConfig& config) : config_(config) {}

    const TetraCookingConfig& config() const noexcept override { return config_; }

    TetraCookedMesh cook_voxel_region(engine::voxel::IVoxelWorld& world,
                                      const glm::ivec3& minimum,
                                      const glm::ivec3& maximum,
                                      std::string& errorOut) override {
        TetraCookedMesh out;
        if (minimum.x > maximum.x || minimum.y > maximum.y || minimum.z > maximum.z) {
            errorOut = "tetra cooking: empty region (min > max) refused";
            return out;
        }

        // First pass: collect the solid cells (fixed y,z,x scan order).
        std::vector<glm::ivec3> solids;
        for (int y = minimum.y; y <= maximum.y; ++y)
            for (int z = minimum.z; z <= maximum.z; ++z)
                for (int x = minimum.x; x <= maximum.x; ++x) {
                    const std::uint32_t block = world.get_block(x, y, z);
                    const bool solid = config_.solidBlockFilter == 0
                                           ? block != 0u
                                           : block == config_.solidBlockFilter;
                    if (solid) solids.push_back({ x, y, z });
                }
        if (solids.empty()) {
            errorOut = "tetra cooking: region has no solid cells";
            return out;
        }
        if (solids.size() * 6 > config_.maxTets) {
            errorOut = "tetra cooking: tet count exceeds maxTets (" +
                       std::to_string(config_.maxTets) + ")";
            return out;
        }

        // Second pass: emit nodes (dedup by grid position) + tets + collider
        // boxes + exposed surface (the render).
        std::unordered_map<GridKey, std::uint32_t, GridKeyHash> nodeIndex;
        out.simTets.reserve(solids.size() * 6);
        for (const glm::ivec3& cell : solids) {
            std::uint32_t corners[8];
            for (int c = 0; c < 8; ++c) {
                const glm::ivec3 pos = cell + kCubeCorner[c];
                const auto found = nodeIndex.find({ pos.x, pos.y, pos.z });
                if (found != nodeIndex.end()) {
                    corners[c] = found->second;
                } else {
                    corners[c] = static_cast<std::uint32_t>(out.simNodes.size());
                    nodeIndex[{ pos.x, pos.y, pos.z }] = corners[c];
                    out.simNodes.push_back(glm::vec3(pos));
                }
            }
            for (const auto& pair : kTetCycle) {
                out.simTets.push_back(
                    glm::ivec4(corners[0], corners[6], corners[pair[0]], corners[pair[1]]));
            }
        }

        // Collider: per-voxel boxes (12 triangles each, internal faces kept).
        out.colliderVertices.reserve(solids.size() * 24);
        out.colliderTriangles.reserve(solids.size() * 12);
        for (const glm::ivec3& cell : solids) {
            const std::uint32_t base = static_cast<std::uint32_t>(out.colliderVertices.size());
            for (const glm::ivec3& corner : kCubeCorner)
                out.colliderVertices.push_back(glm::vec3(cell + corner));
            for (const auto& face : kCubeFace) {
                out.colliderTriangles.push_back(
                    glm::uvec3(base + face[0], base + face[1], base + face[2]));
                out.colliderTriangles.push_back(
                    glm::uvec3(base + face[0], base + face[2], base + face[3]));
            }
        }

        // Render: the EXPOSED surface (a face with an air neighbor is drawn;
        // interior faces are skipped). Node dedup by grid position again, but
        // in the render's own arrays (the render mesh is independent).
        std::unordered_map<GridKey, std::uint32_t, GridKeyHash> renderIndex;
        const auto isSolid = [&](const glm::ivec3& p) {
            return world.get_block(p.x, p.y, p.z) != 0u;
        };
        const glm::ivec3 faceNormals[6] = { { 0, 0, -1 }, { 0, 0, 1 },
                                            { 0, -1, 0 }, { 0, 1, 0 },
                                            { -1, 0, 0 }, { 1, 0, 0 } };
        for (const glm::ivec3& cell : solids) {
            for (int f = 0; f < 6; ++f) {
                if (isSolid(cell + faceNormals[f])) continue;  // interior face
                std::uint32_t quad[4];
                for (int c = 0; c < 4; ++c) {
                    const glm::ivec3 pos = cell + kCubeCorner[kCubeFace[f][c]];
                    const auto found = renderIndex.find({ pos.x, pos.y, pos.z });
                    if (found != renderIndex.end()) {
                        quad[c] = found->second;
                    } else {
                        quad[c] = static_cast<std::uint32_t>(out.renderVertices.size());
                        renderIndex[{ pos.x, pos.y, pos.z }] = quad[c];
                        out.renderVertices.push_back(glm::vec3(pos));
                    }
                }
                out.renderTriangles.push_back(glm::uvec3(quad[0], quad[1], quad[2]));
                out.renderTriangles.push_back(glm::uvec3(quad[0], quad[2], quad[3]));
            }
        }
        return out;
    }

    TetraCookedMesh cook_triangle_mesh(const std::vector<glm::vec3>& vertices,
                                       const std::vector<glm::uvec3>& triangles,
                                       std::string& errorOut) override {
        TetraCookedMesh out;
        if (triangles.empty() || vertices.empty()) {
            errorOut = "tetra cooking: empty triangle mesh refused";
            return out;
        }
        // AABB of the mesh.
        glm::vec3 aabbMin = vertices[0], aabbMax = vertices[0];
        for (const glm::vec3& v : vertices) {
            aabbMin = glm::min(aabbMin, v);
            aabbMax = glm::max(aabbMax, v);
        }

        // Voxelize the interior: a voxel is inside when its center's +X ray
        // crosses the mesh an odd number of times (even-odd rule). Fixed grid
        // over the AABB — deterministic.
        const int minX = static_cast<int>(std::floor(aabbMin.x));
        const int minY = static_cast<int>(std::floor(aabbMin.y));
        const int minZ = static_cast<int>(std::floor(aabbMin.z));
        const int maxX = static_cast<int>(std::floor(aabbMax.x));
        const int maxY = static_cast<int>(std::floor(aabbMax.y));
        const int maxZ = static_cast<int>(std::floor(aabbMax.z));

        std::vector<glm::ivec3> inside;
        for (int y = minY; y <= maxY; ++y)
            for (int z = minZ; z <= maxZ; ++z)
                for (int x = minX; x <= maxX; ++x) {
                    // The +X ray is fired from a fixed tiny offset in z so it
                    // never passes exactly through a mesh edge/vertex (the
                    // classic even-odd double-count on boundary hits): its
                    // (y,z) = (0.5, 0.5+eps) coincides with neither an
                    // integer grid line nor a face diagonal, so a closed
                    // box voxelizes correctly and deterministically.
                    const glm::vec3 center(x + 0.5f, y + 0.5f,
                                           z + 0.5f + 1.0e-4f);
                    if (point_inside(center, vertices, triangles)) inside.push_back({ x, y, z });
                }
        if (inside.empty()) {
            errorOut = "tetra cooking: no voxel inside the closed mesh";
            return out;
        }
        if (inside.size() * 6 > config_.maxTets) {
            errorOut = "tetra cooking: tet count exceeds maxTets (" +
                       std::to_string(config_.maxTets) + ")";
            return out;
        }

        // Simulation mesh from the inside voxels (same canonical 6-tet).
        std::unordered_map<GridKey, std::uint32_t, GridKeyHash> nodeIndex;
        out.simTets.reserve(inside.size() * 6);
        for (const glm::ivec3& cell : inside) {
            std::uint32_t corners[8];
            for (int c = 0; c < 8; ++c) {
                const glm::ivec3 pos = cell + kCubeCorner[c];
                const auto found = nodeIndex.find({ pos.x, pos.y, pos.z });
                if (found != nodeIndex.end()) {
                    corners[c] = found->second;
                } else {
                    corners[c] = static_cast<std::uint32_t>(out.simNodes.size());
                    nodeIndex[{ pos.x, pos.y, pos.z }] = corners[c];
                    out.simNodes.push_back(glm::vec3(pos));
                }
            }
            for (const auto& pair : kTetCycle) {
                out.simTets.push_back(
                    glm::ivec4(corners[0], corners[6], corners[pair[0]], corners[pair[1]]));
            }
        }

        // Render: the INPUT triangles verbatim (the visual asset).
        out.renderVertices = vertices;
        out.renderTriangles = triangles;

        // Collider: the outer surface of the voxelized solid (dedup exposed
        // faces) — the physics proxy of the cooked volume.
        std::unordered_map<GridKey, std::uint32_t, GridKeyHash> colliderIndex;
        const auto inInside = [&](const glm::ivec3& p) {
            const auto found =
                std::find(inside.begin(), inside.end(), p);
            return found != inside.end();
        };
        const glm::ivec3 faceNormals[6] = { { 0, 0, -1 }, { 0, 0, 1 },
                                            { 0, -1, 0 }, { 0, 1, 0 },
                                            { -1, 0, 0 }, { 1, 0, 0 } };
        for (const glm::ivec3& cell : inside) {
            for (int f = 0; f < 6; ++f) {
                if (inInside(cell + faceNormals[f])) continue;
                std::uint32_t quad[4];
                for (int c = 0; c < 4; ++c) {
                    const glm::ivec3 pos = cell + kCubeCorner[kCubeFace[f][c]];
                    const auto found = colliderIndex.find({ pos.x, pos.y, pos.z });
                    if (found != colliderIndex.end()) {
                        quad[c] = found->second;
                    } else {
                        quad[c] =
                            static_cast<std::uint32_t>(out.colliderVertices.size());
                        colliderIndex[{ pos.x, pos.y, pos.z }] = quad[c];
                        out.colliderVertices.push_back(glm::vec3(pos));
                    }
                }
                out.colliderTriangles.push_back(glm::uvec3(quad[0], quad[1], quad[2]));
                out.colliderTriangles.push_back(glm::uvec3(quad[0], quad[2], quad[3]));
            }
        }
        return out;
    }

private:
    // Even-odd point-in-mesh: count +X ray crossings with the triangles.
    static bool point_inside(const glm::vec3& point,
                             const std::vector<glm::vec3>& vertices,
                             const std::vector<glm::uvec3>& triangles) {
        int crossings = 0;
        for (const glm::uvec3& tri : triangles) {
            const glm::vec3& a = vertices[tri.x];
            const glm::vec3& b = vertices[tri.y];
            const glm::vec3& c = vertices[tri.z];
            // Quick reject: the ray (+X at fixed y,z) must pass through the
            // triangle's y/z extent.
            const float minY = std::min(a.y, std::min(b.y, c.y));
            const float maxY = std::max(a.y, std::max(b.y, c.y));
            const float minZ = std::min(a.z, std::min(b.z, c.z));
            const float maxZ = std::max(a.z, std::max(b.z, c.z));
            if (point.y < minY || point.y > maxY || point.z < minZ || point.z > maxZ)
                continue;
            // Find the ray intersection x with the triangle's plane.
            const glm::vec3 normal = glm::cross(b - a, c - a);
            const float denom = normal.x;
            if (std::fabs(denom) < 1.0e-9f) continue;  // parallel to the ray
            const float t = (glm::dot(normal, a - point)) / denom;
            if (t <= 0.0f) continue;  // intersection behind the point
            const glm::vec3 hit = point + glm::vec3(t, 0.0f, 0.0f);
            if (point_in_triangle(hit, a, b, c)) ++crossings;
        }
        return (crossings % 2) == 1;
    }

    static bool point_in_triangle(const glm::vec3& p, const glm::vec3& a,
                                  const glm::vec3& b, const glm::vec3& c) {
        const glm::vec3 n = glm::cross(b - a, c - a);
        const float area2 = glm::dot(n, n);
        if (area2 < 1.0e-12f) return false;
        const glm::vec3 n0 = glm::cross(b - a, p - a);
        const glm::vec3 n1 = glm::cross(c - b, p - b);
        const glm::vec3 n2 = glm::cross(a - c, p - c);
        return glm::dot(n, n0) >= 0.0f && glm::dot(n, n1) >= 0.0f &&
               glm::dot(n, n2) >= 0.0f;
    }

    TetraCookingConfig config_;
};

}  // namespace

bool TetraCookingConfig::load_from_json(const std::string& json,
                                        std::string& errorOut) {
    engine::sdk::JsonValue document;
    if (!engine::sdk::json_parse(json, document, errorOut)) return false;
    if (!document.is_object()) {
        errorOut = "tetra cooking config must be a JSON object";
        return false;
    }
    const double maxTets = engine::sdk::json_number(
        document, "maxTets", static_cast<double>(this->maxTets));
    const double solidBlockFilter = engine::sdk::json_number(
        document, "solidBlockFilter", static_cast<double>(this->solidBlockFilter));
    TetraCookingConfig candidate;
    candidate.maxTets = static_cast<std::size_t>(maxTets);
    candidate.solidBlockFilter = static_cast<std::uint32_t>(solidBlockFilter);
    if (!config_valid(candidate, errorOut)) return false;
    *this = candidate;
    return true;
}

std::unique_ptr<ITetraMeshCooking> create_tetra_mesh_cooking(
    const TetraCookingConfig& config, std::string& errorOut) {
    if (!config_valid(config, errorOut)) return nullptr;
    return std::make_unique<TetraCooker>(config);
}

}  // namespace Engine::Deformable
