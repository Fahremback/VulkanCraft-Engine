// MeshCooking.cpp
//
// SDK adapter for engine/procgen/IMeshCooking.hpp (META section 18 /
// FALTANTES item 14: headless mesh cooking). The public contract never leaks
// the backend; this TU is the ONLY one that includes the promoted clones:
// meshoptimizer (Arseny Kapoulkine, MIT) and xatlas (Jonathan Young, MIT) —
// gate §32, see DEPENDENCY_POLICY. The other two candidates are BLOCKED at
// the gate: manifold (Apache-2.0) requires TBB and geometry-central (MIT)
// requires Eigen, neither vendored in the clone nor the catalog.
//
// Pipeline: unwrap (xatlas atlas -> normalized UVs, remapped indices) ->
// optimize (meshopt vertex-cache + overdraw + vertex-fetch reorder) ->
// optional simplify (meshopt quadric-error LOD). Determinism: meshoptimizer
// is a pure function of the input; xatlas chart growth/packing is
// grid/brute-force based with no RNG in the main path, so two cooks of the
// same (mesh, options) produce bit-identical output (proved by test).

#include "engine/procgen/IMeshCooking.hpp"

#include "meshoptimizer.h"
#include "xatlas.h"

#include <algorithm>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace engine {
namespace procgen {

namespace {

bool validate_mesh(const CookedMesh& mesh, std::string& errorOut) {
    if (mesh.positions.empty() || mesh.positions.size() % 3 != 0) {
        errorOut = "mesh cooking: positions must be xyz triples";
        return false;
    }
    const std::size_t vcount = mesh.vertex_count();
    if (vcount < 3) {
        errorOut = "mesh cooking: need at least 3 vertices";
        return false;
    }
    if (mesh.indices.empty() || mesh.indices.size() % 3 != 0) {
        errorOut = "mesh cooking: indices must be triangle triples";
        return false;
    }
    if (!mesh.normals.empty() && mesh.normals.size() != mesh.positions.size()) {
        errorOut = "mesh cooking: normals size must match positions";
        return false;
    }
    if (!mesh.uvs.empty() && mesh.uvs.size() != vcount * 2) {
        errorOut = "mesh cooking: uvs size must match vertices";
        return false;
    }
    for (const std::uint32_t idx : mesh.indices) {
        if (idx >= vcount) {
            errorOut = "mesh cooking: index out of range";
            return false;
        }
    }
    return true;
}

bool validate_options(const CookOptions& options, std::string& errorOut) {
    if (options.atlasResolution < 0) {
        errorOut = "mesh cooking: atlasResolution must be non-negative";
        return false;
    }
    if (options.texelsPerUnit < 0.0f) {
        errorOut = "mesh cooking: texelsPerUnit must be non-negative";
        return false;
    }
    if (options.overdrawThreshold < 1.0f) {
        errorOut = "mesh cooking: overdrawThreshold must be >= 1";
        return false;
    }
    if (options.simplifyError < 0.0f) {
        errorOut = "mesh cooking: simplifyError must be non-negative";
        return false;
    }
    return true;
}

// Applies a vertex remap (remap[old] = new) to arrays + indices.
void apply_remap(const CookedMesh& in, const std::vector<std::uint32_t>& remap,
                 CookedMesh& out) {
    const std::size_t vcount = in.vertex_count();
    out.positions.resize(vcount * 3);
    out.normals.resize(in.normals.empty() ? 0 : vcount * 3);
    out.uvs.resize(in.uvs.empty() ? 0 : vcount * 2);
    for (std::size_t i = 0; i < vcount; ++i) {
        const std::uint32_t dst = remap[i];
        for (int k = 0; k < 3; ++k) {
            out.positions[dst * 3 + k] = in.positions[i * 3 + k];
            if (!in.normals.empty()) {
                out.normals[dst * 3 + k] = in.normals[i * 3 + k];
            }
        }
        if (!in.uvs.empty()) {
            out.uvs[dst * 2] = in.uvs[i * 2];
            out.uvs[dst * 2 + 1] = in.uvs[i * 2 + 1];
        }
    }
    out.indices.resize(in.indices.size());
    for (std::size_t j = 0; j < in.indices.size(); ++j) {
        out.indices[j] = remap[in.indices[j]];
    }
}

}  // namespace

class MeshCooker final : public IMeshCooker {
public:
    bool cook(const CookedMesh& in, const CookOptions& options,
              CookedMesh& out, CookStats& stats,
              std::string& errorOut) override {
        if (!validate_options(options, errorOut)) {
            return false;
        }
        CookedMesh current = in;
        if (options.unwrap) {
            CookedMesh unwrapped;
            if (!unwrap(current, options, unwrapped, errorOut)) {
                return false;
            }
            current = std::move(unwrapped);
        }
        if (options.optimize) {
            CookedMesh optimized;
            if (!optimize(current, options, optimized, errorOut)) {
                return false;
            }
            current = std::move(optimized);
        }
        if (options.simplifyTargetIndices > 0) {
            CookedMesh simplified;
            if (!simplify(current, options.simplifyTargetIndices,
                          options.simplifyError, simplified, errorOut)) {
                return false;
            }
            current = std::move(simplified);
        }
        if (!analyze(current, stats, errorOut)) {
            return false;
        }
        stats.inputVertices = in.vertex_count();
        stats.inputIndices = in.index_count();
        stats.outputVertices = current.vertex_count();
        stats.outputIndices = current.index_count();
        stats.hasUvs = !current.uvs.empty();
        out = std::move(current);
        return true;
    }

    bool unwrap(const CookedMesh& in, const CookOptions& options,
                CookedMesh& out, std::string& errorOut) override {
        if (!validate_mesh(in, errorOut)) {
            return false;
        }
        xatlas::Atlas* atlas = xatlas::Create();
        xatlas::MeshDecl decl;
        decl.vertexPositionData = in.positions.data();
        decl.vertexPositionStride = sizeof(float) * 3;
        decl.vertexCount = static_cast<std::uint32_t>(in.vertex_count());
        if (!in.normals.empty()) {
            decl.vertexNormalData = in.normals.data();
            decl.vertexNormalStride = sizeof(float) * 3;
        }
        if (!in.uvs.empty()) {
            decl.vertexUvData = in.uvs.data();
            decl.vertexUvStride = sizeof(float) * 2;
        }
        decl.indexData = in.indices.data();
        decl.indexOffset = 0;
        decl.indexCount = static_cast<std::uint32_t>(in.index_count());
        decl.indexFormat = xatlas::IndexFormat::UInt32;

        const xatlas::AddMeshError err = xatlas::AddMesh(atlas, decl);
        if (err != xatlas::AddMeshError::Success) {
            errorOut = "mesh cooking: xatlas AddMesh failed";
            xatlas::Destroy(atlas);
            return false;
        }
        xatlas::ChartOptions chartOptions;
        xatlas::PackOptions packOptions;
        packOptions.resolution =
            static_cast<std::uint32_t>(options.atlasResolution);
        packOptions.texelsPerUnit = options.texelsPerUnit;
        xatlas::Generate(atlas, chartOptions, packOptions);
        if (atlas->width == 0 || atlas->height == 0 || atlas->meshCount != 1) {
            errorOut = "mesh cooking: xatlas Generate failed";
            xatlas::Destroy(atlas);
            return false;
        }
        const xatlas::Mesh& result = atlas->meshes[0];
        const std::size_t outVertices = result.vertexCount;
        const float invW = 1.0f / static_cast<float>(atlas->width);
        const float invH = 1.0f / static_cast<float>(atlas->height);
        out.positions.resize(outVertices * 3);
        out.normals.resize(in.normals.empty() ? 0 : outVertices * 3);
        out.uvs.resize(outVertices * 2);
        for (std::size_t v = 0; v < outVertices; ++v) {
            const xatlas::Vertex& sv = result.vertexArray[v];
            const std::size_t src = sv.xref;
            for (int k = 0; k < 3; ++k) {
                out.positions[v * 3 + k] = in.positions[src * 3 + k];
                if (!in.normals.empty()) {
                    out.normals[v * 3 + k] = in.normals[src * 3 + k];
                }
            }
            out.uvs[v * 2] = sv.uv[0] * invW;
            out.uvs[v * 2 + 1] = sv.uv[1] * invH;
        }
        out.indices.assign(result.indexArray,
                           result.indexArray + result.indexCount);
        xatlas::Destroy(atlas);
        return true;
    }

    bool optimize(const CookedMesh& in, const CookOptions& options,
                  CookedMesh& out, std::string& errorOut) override {
        if (!validate_mesh(in, errorOut)) {
            return false;
        }
        const std::size_t vcount = in.vertex_count();
        const std::size_t icount = in.index_count();

        // meshopt_optimizeVertexCache and meshopt_optimizeOverdraw both write
        // a REORDERED INDEX BUFFER (index_count elements) and leave the vertex
        // order untouched, so the vertex streams stay valid throughout and the
        // overdraw stage can keep reading the original positions. Only the
        // final stage (vertex fetch) produces a vertex remap (vcount
        // elements), which is applied to every stream at once.
        std::vector<std::uint32_t> vcIndices(icount);
        meshopt_optimizeVertexCache(vcIndices.data(), in.indices.data(), icount,
                                    vcount);

        std::vector<std::uint32_t> odIndices(icount);
        meshopt_optimizeOverdraw(odIndices.data(), vcIndices.data(), icount,
                                 in.positions.data(), vcount,
                                 sizeof(float) * 3, options.overdrawThreshold);

        std::vector<std::uint32_t> remap(vcount);
        meshopt_optimizeVertexFetchRemap(remap.data(), odIndices.data(), icount,
                                         vcount);

        // Compose: vertex streams reordered by the fetch remap, indices by the
        // fetch remap applied on top of the overdraw-reordered buffer.
        CookedMesh reorderedIn = in;
        reorderedIn.indices = std::move(odIndices);
        apply_remap(reorderedIn, remap, out);
        return true;
    }

    bool simplify(const CookedMesh& in, std::size_t targetIndices, float error,
                  CookedMesh& out, std::string& errorOut) override {
        if (!validate_mesh(in, errorOut)) {
            return false;
        }
        if (targetIndices < 3 || error < 0.0f) {
            errorOut = "mesh cooking: simplify needs targetIndices >= 3 and "
                       "non-negative error";
            return false;
        }
        const std::size_t vcount = in.vertex_count();
        const std::size_t icount = in.index_count();
        std::vector<std::uint32_t> simplified(icount);
        float resultError = 0.0f;
        const std::size_t newCount = meshopt_simplify(
            simplified.data(), in.indices.data(), icount, in.positions.data(),
            vcount, sizeof(float) * 3, targetIndices, error, 0, &resultError);
        if (newCount == 0) {
            errorOut = "mesh cooking: simplify produced an empty mesh";
            return false;
        }
        simplified.resize(newCount);
        // Compact the vertex streams to exactly the subset referenced by the
        // simplified index buffer.
        std::vector<std::uint32_t> used;
        for (const std::uint32_t idx : simplified) {
            used.push_back(idx);
        }
        std::sort(used.begin(), used.end());
        used.erase(std::unique(used.begin(), used.end()), used.end());
        std::vector<std::uint32_t> remap(vcount, 0);
        for (std::size_t i = 0; i < used.size(); ++i) {
            remap[used[i]] = static_cast<std::uint32_t>(i);
        }
        CookedMesh compact;
        const std::size_t usedCount = used.size();
        compact.positions.resize(usedCount * 3);
        if (!in.normals.empty()) {
            compact.normals.resize(usedCount * 3);
        }
        if (!in.uvs.empty()) {
            compact.uvs.resize(usedCount * 2);
        }
        for (std::size_t i = 0; i < usedCount; ++i) {
            const std::uint32_t src = used[i];
            for (int k = 0; k < 3; ++k) {
                compact.positions[i * 3 + k] = in.positions[src * 3 + k];
                if (!in.normals.empty()) {
                    compact.normals[i * 3 + k] = in.normals[src * 3 + k];
                }
            }
            if (!in.uvs.empty()) {
                compact.uvs[i * 2] = in.uvs[src * 2];
                compact.uvs[i * 2 + 1] = in.uvs[src * 2 + 1];
            }
        }
        compact.indices.resize(newCount);
        for (std::size_t j = 0; j < newCount; ++j) {
            compact.indices[j] = remap[simplified[j]];
        }
        out = std::move(compact);
        return true;
    }

    bool analyze(const CookedMesh& mesh, CookStats& stats,
                 std::string& errorOut) const override {
        if (!validate_mesh(mesh, errorOut)) {
            return false;
        }
        const std::size_t vcount = mesh.vertex_count();
        const std::size_t icount = mesh.index_count();
        const meshopt_VertexCacheStatistics vcs = meshopt_analyzeVertexCache(
            mesh.indices.data(), icount, vcount, 32, 8, 32);
        const meshopt_OverdrawStatistics ods = meshopt_analyzeOverdraw(
            mesh.indices.data(), icount, mesh.positions.data(), vcount,
            sizeof(float) * 3);
        stats.acmr = vcs.acmr;
        stats.overdraw = ods.overdraw;
        stats.hasUvs = !mesh.uvs.empty();
        return true;
    }
};

}  // namespace procgen
}  // namespace engine

namespace engine {
namespace procgen {

std::shared_ptr<IMeshCooker> create_mesh_cooker() {
    return std::make_shared<MeshCooker>();
}

}  // namespace procgen
}  // namespace engine
