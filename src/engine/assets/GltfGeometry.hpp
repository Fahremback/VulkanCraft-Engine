#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Engine {

// A single glTF mesh primitive (drawable geometry). Positions are required;
// normals are computed when the source has none; uvs are optional. Indices may
// be empty for non-indexed geometry. JOINTS_0/WEIGHTS_0 are optional skinning
// data: uvec4 joint indices + vec4 weights per vertex.
struct GltfMeshPrimitive {
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec2> uvs;
    std::vector<uint32_t> indices;
    std::vector<glm::uvec4> joints;
    std::vector<glm::vec4> weights;
    bool indexed{ false };
};

// Skeleton extracted from a glTF skin: joint names (node names), parent index
// per joint (resolved from the node hierarchy) and inverse-bind matrices.
struct GltfGeometrySkin {
    std::string name;
    std::vector<std::string> jointNames;
    std::vector<int32_t> jointParents;
    std::vector<glm::mat4> inverseBindMatrices;
};

struct GltfGeometryResult {
    bool success{ false };
    std::string error;
    std::vector<GltfMeshPrimitive> primitives;
    std::vector<GltfGeometrySkin> skins;
    uint32_t vertexCount{ 0 };
    uint32_t indexCount{ 0 };
};

// Extracts renderable geometry (POSITION/NORMAL/TEXCOORD_0/indices) from
// glTF 2.0 documents. Supports:
//   - .gltf JSON text with data-URI buffers
//   - .glb binary containers (JSON chunk + BIN chunk)
//   - cooked .vcmesh files (VCMESH header wrapping the original source bytes)
// Integer normalized accessors, interleaved buffer views and 8/16/32-bit
// indices are handled; flat normals are generated when NORMAL is missing.
class GltfGeometryParser final {
public:
    // Parses geometry from raw bytes: either glTF JSON text or a full GLB file.
    static GltfGeometryResult parse(const std::vector<uint8_t>& bytes,
                                    std::string* error = nullptr);

    // Parses a cooked .vcmesh file (magic + header + payload). Version 1 wraps
    // the original glTF source bytes; version 2 stores binary geometry directly
    // (no JSON re-parse at load time).
    static GltfGeometryResult parse_vcmesh(const std::filesystem::path& cookedPath,
                                           std::string* error = nullptr);

    // Writes a .vcmesh v2 file (binary geometry payload) for cooked meshes.
    // Returns false and fills *error when geometry is empty.
    static bool write_cooked(const std::filesystem::path& cookedPath,
                             const GltfGeometryResult& geometry,
                             std::string* error = nullptr);
};

} // namespace Engine
