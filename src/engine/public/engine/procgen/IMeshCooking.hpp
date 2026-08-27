#pragma once

// Public mesh-cooking contracts (META section 18 / FALTANTES item 14).
//
// Cooks procedurally generated meshes into render-ready form, headless:
//   - unwrap: automatic UV atlas generation (xatlas backend)
//   - optimize: vertex-cache/overdraw/fetch reordering (meshoptimizer)
//   - simplify: quadric-error LOD simplification (meshoptimizer)
//   - analyze: vertex-cache/overdraw statistics to prove the effect
//
// Same contract family as the other procgen contracts: the public types are
// self-contained float/index buffers, never leaking the backend. The pipeline
// is deterministic — meshoptimizer is a pure function of the input and
// xatlas's packing is grid/brute-force based (no RNG in the main path), so
// two cooks of the same (mesh, options) produce bit-identical output.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace procgen {

// A mesh as plain float/index buffers (self-contained; no backend types).
// Positions are xyz per vertex, normals xyz (may be empty), uvs xy
// (may be empty), indices reference vertices.
struct CookedMesh {
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> uvs;
    std::vector<std::uint32_t> indices;
    std::vector<float> tangents;  // xyz per vertex (MikkTSpace-style, orthogonal
                                  // to the normal); empty until generated

    std::size_t vertex_count() const { return positions.size() / 3; }
    std::size_t index_count() const { return indices.size(); }
};

// Data-driven cooking options.
struct CookOptions {
    bool unwrap{ true };              // generate UVs via atlas unwrapping
    int atlasResolution{ 1024 };      // xatlas pack resolution (0 = estimate)
    float texelsPerUnit{ 0.0f };      // 0 = estimated to match resolution

    bool optimize{ true };            // vertex-cache + overdraw + fetch reorder
    float overdrawThreshold{ 1.01f }; // overdraw optimization threshold

    std::size_t simplifyTargetIndices{ 0 };  // > 0: simplify to this count
    float simplifyError{ 0.01f };            // max simplification error
};

// Vertex-cache/overdraw statistics of a mesh (meshopt analyzers).
struct CookStats {
    std::size_t inputVertices{ 0 };
    std::size_t inputIndices{ 0 };
    std::size_t outputVertices{ 0 };
    std::size_t outputIndices{ 0 };
    double acmr{ 0.0 };      // average cache miss ratio (triangles/miss)
    double overdraw{ 0.0 };  // overdraw ratio
    bool hasUvs{ false };    // output carries UVs
};

// Cooks meshes headless. All stages are deterministic; invalid input
// (degenerate meshes, bad options) is rejected with a message.
class IMeshCooker {
public:
    virtual ~IMeshCooker() = default;

    // Full pipeline: unwrap -> optimize -> optional simplify.
    virtual bool cook(const CookedMesh& in, const CookOptions& options,
                      CookedMesh& out, CookStats& stats,
                      std::string& errorOut) = 0;

    // UV unwrap only (xatlas). Output positions/normals follow the input;
    // uvs are normalized to [0, 1] and indices are remapped.
    virtual bool unwrap(const CookedMesh& in, const CookOptions& options,
                        CookedMesh& out, std::string& errorOut) = 0;

    // Vertex-cache/overdraw/fetch reordering only.
    virtual bool optimize(const CookedMesh& in, const CookOptions& options,
                          CookedMesh& out, std::string& errorOut) = 0;

    // Quadric-error simplification to at most `targetIndices` indices.
    virtual bool simplify(const CookedMesh& in, std::size_t targetIndices,
                          float error, CookedMesh& out,
                          std::string& errorOut) = 0;

    // Analyzes vertex-cache and overdraw statistics.
    virtual bool analyze(const CookedMesh& mesh, CookStats& stats,
                         std::string& errorOut) const = 0;

    // Generates MikkTSpace-style tangents (xyz per vertex) for a mesh that
    // has UVs. Tangents are orthogonalized against the normals (Gram-Schmidt)
    // and handedness-aware (right-handed TBN). Deterministic — a pure
    // function of (positions, uvs, indices, normals). Rejects meshes without
    // UVs. Output carries the input streams plus `tangents`.
    virtual bool generate_tangents(const CookedMesh& in, CookedMesh& out,
                                   std::string& errorOut) = 0;
};

// Factory (implemented by the SDK adapter — the only TU that includes the
// promoted meshoptimizer/xatlas clones).
std::shared_ptr<IMeshCooker> create_mesh_cooker();

}  // namespace procgen
}  // namespace engine
