// IProcgenPreview.hpp
//
// Headless textual/statistical preview for every procgen domain of META
// section 18 / FALTANTES item 14. The editor shows these renders without a
// GPU: an ASCII grid plus deterministic stats, produced by the SAME public
// contracts the world uses (IStructureGenerator, IParcellation,
// IShapeGrammarRunner, IHeightmapErosion, IMeshCooker and the noise/climate
// graphs), so a preview always reflects what generation would actually
// produce. Every render is bounded (max sampleSize grid) and deterministic:
// the same inputs produce the same render, bit for bit, on any instance.
//
// This header is self-contained: it composes the public procgen contracts and
// never leaks a backend clone.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/procgen/IClimateBiome.hpp"
#include "engine/procgen/IHeightmapErosion.hpp"
#include "engine/procgen/IMeshCooking.hpp"
#include "engine/procgen/INoiseGraph.hpp"
#include "engine/procgen/IParcellation.hpp"
#include "engine/procgen/IShapeGrammar.hpp"
#include "engine/procgen/IStructureGenerator.hpp"

namespace engine {
namespace procgen {

// One statistical line of a preview render. Order is deterministic (defined
// by each preview method).
struct PreviewStat {
    std::string label;
    std::string value;
};

// A bounded textual render: an ASCII grid plus deterministic stats. The
// editor displays this directly; no window/GPU is involved.
struct PreviewRender {
    std::string title;               // one-line human title
    std::vector<std::string> lines;  // ASCII rows, all sampleSize chars wide
    std::vector<PreviewStat> stats;  // deterministic label/value pairs
};

// Shared bounded options for preview renders.
struct PreviewOptions {
    std::size_t sampleSize{ 48 };  // grid size for 2D renders (clamped to
                                   // [8, 96])
    std::size_t seed{ 1 };         // structure/erosion seed (structure assets
                                   // carry their own seed override)
    bool showStats{ true };        // false -> stats omitted (lines only)
};

// Headless textual/statistical preview of the procgen pipeline.
class IProcgenPreview {
public:
    virtual ~IProcgenPreview() = default;

    // Terrain slice: height from `height` (INoiseGraph::sample_2d), biome map
    // from `climate` + `biomes` over a sampleSize x sampleSize patch at
    // integer world coordinates. Lines = ASCII height field; stats include
    // height min/max/mean and the biome distribution (sorted by name).
    virtual bool preview_terrain(const INoiseGraph& height,
                                 const IClimateSampler& climate,
                                 const IBiomeRegistry& biomes,
                                 const PreviewOptions& opts, PreviewRender& out,
                                 std::string& error) = 0;

    // Structure plan (fast-wfc): generates an sampleSize x sampleSize plan
    // from the asset and renders the block-id field as ASCII. Stats include
    // plan dimensions, non-air block count and the block-id histogram.
    virtual bool preview_structure(const StructureAssetSpec& asset,
                                   const PreviewOptions& opts,
                                   PreviewRender& out,
                                   std::string& error) = 0;

    // Road network + parcels (delaunator + earcut): builds the network for
    // `spec`, extracts parcels and renders the road map as ASCII. Stats
    // include junction/edge counts and parcel area min/mean/max/sum.
    virtual bool preview_parcels(const RoadNetworkSpec& spec,
                                 const PreviewOptions& opts,
                                 PreviewRender& out,
                                 std::string& error) = 0;

    // Shape grammar: runs `grammar` and renders the top-down occupancy of the
    // emitted boxes as ASCII. Stats include box count, total volume and the
    // bounding box of the result.
    virtual bool preview_shape(const ShapeGrammar& grammar,
                               const PreviewOptions& opts, PreviewRender& out,
                               std::string& error) = 0;

    // Erosion: erodes a seeded heightmap ridge under `spec` and renders the
    // before/after fields side by side. Stats include roughness before/after
    // and total-mass conservation (|delta|).
    virtual bool preview_erosion(const ErosionSpec& spec,
                                 const PreviewOptions& opts,
                                 PreviewRender& out,
                                 std::string& error) = 0;

    // Cooked mesh: cooks `mesh` under `options` and renders the resulting
    // stats (input/output counts, ACMR/overdraw, hasUvs) as text. No ASCII
    // grid (lines = empty when showStats).
    virtual bool preview_mesh(const CookedMesh& mesh,
                              const CookOptions& options,
                              const PreviewOptions& opts, PreviewRender& out,
                              std::string& error) = 0;
};

// Factory (implemented by the SDK adapter — the only TU that composes the
// procgen adapters into previews; no new backend).
std::shared_ptr<IProcgenPreview> create_procgen_preview();

}  // namespace procgen
}  // namespace engine
