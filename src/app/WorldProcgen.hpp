#pragma once

// WorldProcgen.hpp — CONTA 2 (world/procgen — 18 factories).
//
// The 18 public procgen factories that were once TEST-ONLY are composed here in
// the GAME executable (vc_app_runtime / src/app) and wired into the REAL chunk
// generation path: the composed IClimateVoxelGenerator is registered as the
// app's main `World` generator override, so every chunk dispatched by
// World::update -> Chunk::generate_terrain samples the height graph, climate
// sampler, biome registry, surface resolver, carver, ore table and decorator
// set (via the IVoxelGenerator hooks sample/cave_density/ore_density/
// surface_block/carve_block/ore_block/decorate_column). The LOD sampler and
// multi-scale streaming sample the SAME graph voxel generator every tick
// (coherent far terrain), the heightmap erosion runs on a real tile of the
// height graph, and the mesh cooker cooks a real surface-grid mesh into a
// surviving buffer. Every core publishes a per-frame observable.
//
// No test/wrapper/stub: each factory below gets a real call site in this TU and
// its output feeds a per-frame/per-chunk consumer; the observables are read by
// showcase_gameplay_tick and shown in the window title.

#include "engine/procgen/IClimateBiome.hpp"
#include "engine/procgen/IWorldFeatures.hpp"
#include "engine/procgen/INoiseGraph.hpp"
#include "engine/procgen/IHeightmapErosion.hpp"
#include "engine/procgen/ILodTerrain.hpp"
#include "engine/procgen/IMultiScaleStreaming.hpp"
#include "engine/procgen/IMeshCooking.hpp"
#include "engine/procgen/IMeshGeometryProcessing.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class World;

namespace app {

class WorldProcgen {
public:
    void init(World& world);
    void tick(World& world, float playerX, float playerZ);
    // Composed summary appended to the window title (real observables).
    std::string summary() const;

    // A4-PROCGEN-FACTORY-FAILURE: reports a REQUIRED factory failure to stderr
    // (name + diagnostic) and returns false so init() aborts instead of
    // degrading silently with a null factory. Static so the reporting path is
    // directly testable (WorldProcgen.RequiredFactoryFailureReported).
    static bool require_factory(const char* name, const std::string& diag,
                                bool has_value);

private:
    // ---- group 1: biome / climate ---------------------------------------
    std::shared_ptr<engine::procgen::IBiomeRegistry> biomeRegistry_;
    std::shared_ptr<engine::procgen::IBiomeRegistry> biomeRegistryJson_;
    std::shared_ptr<engine::procgen::IClimateSampler> climateSampler_;
    std::shared_ptr<engine::procgen::ISurfaceResolver> surfaceResolver_;
    std::shared_ptr<engine::procgen::IClimateVoxelGenerator> climateGenerator_;
    // ---- group 2: world features ----------------------------------------
    std::shared_ptr<engine::procgen::ICarver> carverJson_;
    std::shared_ptr<engine::procgen::IDecorator> decorator_;
    std::shared_ptr<engine::procgen::IDecorator> decoratorJson_;
    std::shared_ptr<engine::procgen::IDecoratorSet> decoratorSetJson_;
    std::shared_ptr<engine::procgen::IOreTable> oreTableJson_;
    // ---- group 3: noise graph -------------------------------------------
    std::shared_ptr<engine::procgen::INoiseGraph> heightGraph_;
    std::shared_ptr<engine::procgen::IDensityFunction> cavesDensity_;
    std::shared_ptr<engine::procgen::IDensityFunction> oresDensity_;
    std::shared_ptr<engine::procgen::IGraphVoxelGenerator> graphGenerator_;
    // ---- group 4: erosion ------------------------------------------------
    std::shared_ptr<engine::procgen::IHeightmapErosion> erosion_;
    std::shared_ptr<engine::procgen::ITileErosionCache> erosionCache_;
    // ---- group 5: LOD / streaming / cooker ------------------------------
    std::shared_ptr<engine::procgen::ILodTerrainSampler> lodSampler_;
    std::shared_ptr<engine::procgen::IMultiScaleStreaming> multiScale_;
    std::shared_ptr<engine::procgen::IMeshCooker> meshCooker_;
    // ---- group 6: mesh geometry post-processing (geodesic/smooth/decimate)
    std::unique_ptr<Engine::Procgen::IMeshGeometryProcessing> geomProcessor_;
    std::size_t smoothedVertices_{ 0 };

    // Surviving cooked-mesh output (never a dead local).
    std::vector<float> cookedPositions_;
    std::vector<float> cookedUvs_;
    std::vector<std::uint32_t> cookedIndices_;

    // Per-frame observables (published + shown in title).
    std::size_t biomeCount_{ 0 };
    std::size_t biomeCountJson_{ 0 };
    float climateTemperature_{ 0.0f };
    float climateMoisture_{ 0.0f };
    std::uint32_t surfaceBlockAtPlayer_{ 0 };
    std::uint8_t biomeAtPlayer_{ 0 };
    float graphHeightAtPlayer_{ 0.0f };
    std::size_t carverRules_{ 0 };
    std::size_t decoratorCount_{ 0 };
    std::size_t decoratorSetCount_{ 0 };
    // Per-frame consumption of the PLAIN biome registry + decorators (default
    // engine table, hand-built tree, and its JSON twin) — read by tick() and
    // published so they are genuinely exercised every frame, not just counted.
    std::uint8_t plainBiomeIndex_{ 0 };
    std::size_t plainBiomeSerializeLen_{ 0 };
    std::size_t decoratorPlaced_{ 0 };
    std::size_t decoratorJsonPlaced_{ 0 };
    std::size_t oreRuleCount_{ 0 };
    std::size_t lodCells_{ 0 };
    float lodInterpHeight_{ 0.0f };
    std::size_t streamLevels_{ 0 };
    std::size_t streamCells_{ 0 };
    std::size_t erosionCacheSize_{ 0 };
    std::size_t erosionTileW_{ 0 };
    std::size_t erosionTileH_{ 0 };
    int erosionConvergedCells_{ 0 };
    std::size_t cookInVertices_{ 0 };
    std::size_t cookInIndices_{ 0 };
    std::size_t cookOutVertices_{ 0 };
    std::size_t cookOutIndices_{ 0 };
    double cookAcmr_{ 0.0 };
    bool samplerInitialized_{ false };
    std::uint64_t tickCounter_{ 0 };
};

}  // namespace app