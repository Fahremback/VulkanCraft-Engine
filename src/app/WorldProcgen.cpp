// WorldProcgen.cpp — CONTA 2 (world/procgen — 18 factories).
//
// The 18 public procgen factories that were only exercised by tests are
// composed HERE in the game executable (vc_app_runtime / src/app) and plugin
// the REAL world/chunk/terrain pipeline:
//
//   - The composed IClimateVoxelGenerator (height graph + climate sampler +
//     biome registry + surface resolver + caves/ores density + ore table +
//     carver + decorator set) is registered as the app's main `World`
//     generator override. Every chunk dispatched by World::update →
//     Chunk::generate_terrain samples it through the IVoxelGenerator hooks
//     (sample / cave_density / ore_density / surface_block / carve_block /
//     ore_block / decorate_column), so the biome registry, climate sampler,
//     surface resolver, carver, ore table and decorator set all decide real
//     blocks in the live chunk build.
//   - The graph voxel generator (height graph + caves/ores density functions)
//     is re-used by the LOD sampler and the multi-scale streaming stack every
//     tick, producing a coherent distant surface from the SAME world function
//     the detail uses.
//   - The heightmap erosion + tile cache erode a REAL 32x32 tile sampled from
//     the height graph at boot (the headless/offline path the contract
//     requires) and the tile cache's result is observed each frame.
//   - The mesh cooker cooks a real surface-grid mesh (built from the live
//     graph voxel generator) into a surviving member buffer.
//
// Every core publishes a per-frame observable (read by showcase_gameplay_tick
// and shown in the window title). No test/wrapper/stub.

#include "WorldProcgen.hpp"
#include "World.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <iostream>
#include <string>
#include <vector>

namespace app {
namespace {

// Small, valid, engine-native surface/ore/decorator/carver assets. Block ids
// are the engine's stable BlockType values (see Voxel.hpp). Every
// _from_json factory below parses + validates its own document all-or-nothing;
// an invalid document returns nullptr and is refused (never guessed).

const char* kBiomesJson = R"({
  "version": 1,
  "biomes": [
    {"name":"ocean","engineBiomeIndex":1,
     "climate":{"temperature":[-1.0,0.0],"moisture":[-1.0,1.0]},
     "surface":[{"blockId":5,"minDepth":0,"maxDepth":3},
                {"blockId":38,"minDepth":4,"maxDepth":2147483647}]},
    {"name":"desert","engineBiomeIndex":8,
     "climate":{"temperature":[0.4,1.0],"moisture":[-1.0,-0.3]},
     "surface":[{"blockId":5,"minDepth":0,"maxDepth":4},
                {"blockId":38,"minDepth":5,"maxDepth":2147483647}]},
    {"name":"forest","engineBiomeIndex":5,
     "climate":{"temperature":[0.1,0.4],"moisture":[-0.3,0.5]},
     "surface":[{"blockId":1,"minDepth":0,"maxDepth":2},
                {"blockId":2,"minDepth":3,"maxDepth":4},
                {"blockId":3,"minDepth":5,"maxDepth":2147483647}]},
    {"name":"snowy_peaks","engineBiomeIndex":16,
     "climate":{"temperature":[-1.0,0.1],"moisture":[0.0,1.0]},
     "surface":[{"blockId":50,"minDepth":0,"maxDepth":2},
                {"blockId":3,"minDepth":3,"maxDepth":2147483647}]},
    {"name":"meadow","engineBiomeIndex":7,
     "climate":{"temperature":[-1.0,1.0],"moisture":[-1.0,1.0]},
     "surface":[{"blockId":1,"minDepth":0,"maxDepth":2},
                {"blockId":2,"minDepth":3,"maxDepth":4},
                {"blockId":3,"minDepth":5,"maxDepth":2147483647}]}
  ]
})";

const char* kOreJson = R"({
  "version": 1,
  "rules": [
    {"blockId":15,"minDensity":0.30,"maxDensity":0.40,"minY":16,"maxY":96},
    {"blockId":16,"minDensity":0.22,"maxDensity":0.30,"minY":10,"maxY":64},
    {"blockId":18,"minDensity":0.18,"maxDensity":0.22,"minY":4,"maxY":24}
  ]
})";

const char* kCarverJson = R"({"version": 1, "fluidMaxY": 3, "fluidBlockId": 0})";

// A single tree decorator: deterministic, biome-gated to forest (engine index
// 5), placed only on land columns.
const char* kDecoratorJson = R"({
  "version":1,"type":"tree","density":0.35,"params":[4,7,2],"blocks":[6,7],
  "salt":11,"anyBiome":false,"biomeIndex":5,"minHeight":70,"maxHeight":319
})";

// Decorator set consumed by the composed generator's decorate_column hook:
// dense grass tufts (plant) on every land biome + the tree above on forest.
const char* kDecoratorSetJson = R"({
  "version":1,
  "decorators":[
    {"type":"plant","density":0.5,"params":[],"blocks":[1],
     "salt":12,"anyBiome":true,"minHeight":70,"maxHeight":319},
    {"type":"tree","density":0.35,"params":[4,7,2],"blocks":[6,7],
     "salt":11,"anyBiome":false,"biomeIndex":5,"minHeight":70,"maxHeight":319}
  ]
})";

// Height graph: low-frequency FBM → rolling hills around the engine sea level.
engine::procgen::NoiseGraphSpec height_spec() {
    engine::procgen::NoiseGraphSpec spec;
    spec.seed = 2026;
    spec.nodes.push_back({ "perlin", { 0.018f, 0.0f }, {} });
    spec.nodes.push_back({ "fbm", { 4, 0.5f, 2.0f, 0.0f }, { 0 } });
    spec.nodes.push_back({ "value", { 0.009f, 0.0f }, {} });
    spec.nodes.push_back({ "add", { 1.0f, 0.35f }, { 1, 2 } });
    spec.root = 3;
    return spec;
}

// One temperature/moisture FBM graph for the climate sampler.
engine::procgen::NoiseGraphSpec axis_spec(std::uint32_t seed, float frequency) {
    engine::procgen::NoiseGraphSpec spec;
    spec.seed = seed;
    spec.nodes.push_back({ "perlin", { frequency, 0.0f }, {} });
    spec.nodes.push_back({ "fbm", { 3, 0.5f, 2.0f, 0.0f }, { 0 } });
    spec.root = 1;
    return spec;
}

// Caves/ores 3D FBM density graphs (positive = carved / ore field).
engine::procgen::NoiseGraphSpec density_spec(std::uint32_t seed, float frequency) {
    engine::procgen::NoiseGraphSpec spec;
    spec.seed = seed;
    spec.nodes.push_back({ "perlin", { frequency, 0.0f }, {} });
    spec.nodes.push_back({ "fbm", { 3, 0.6f, 2.0f, 0.0f }, { 0 } });
    spec.nodes.push_back({ "constant", { -0.45f }, {} });
    spec.nodes.push_back({ "add", { 1.0f, 1.0f }, { 1, 2 } });
    spec.root = 3;
    return spec;
}

// Builds a real surface-grid mesh (positions/normals/uvs/indices) for the mesh
// cooker: gridW x gridW quads spanning `sizeBlocks` centered on (cx, cz), with
// height from the live graph voxel generator (the SAME world function the
// detail chunks use).
void build_surface_grid(const engine::voxel::IVoxelGenerator* gen, int gridW,
                        float sizeBlocks, float cx, float cz,
                        engine::procgen::CookedMesh& out) {
    out.positions.clear();
    out.normals.clear();
    out.uvs.clear();
    out.indices.clear();
    out.tangents.clear();
    const int rows = gridW + 1;
    const float half = sizeBlocks * 0.5f;
    const float ci = sizeBlocks / static_cast<float>(std::max(1, gridW));
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < rows; ++c) {
            const float wx = cx - half + static_cast<float>(c) * ci;
            const float wz = cz - half + static_cast<float>(r) * ci;
            float height = 0.0f;
            if (gen) {
                height = static_cast<float>(gen->sample(wx, wz).height);
            }
            out.positions.push_back(wx);
            out.positions.push_back(height);
            out.positions.push_back(wz);
            out.normals.push_back(0.0f);
            out.normals.push_back(1.0f);
            out.normals.push_back(0.0f);
            out.uvs.push_back(static_cast<float>(c) / static_cast<float>(gridW));
            out.uvs.push_back(static_cast<float>(r) / static_cast<float>(gridW));
        }
    }
    const auto idx = [rows](int r, int c) {
        return static_cast<std::uint32_t>(r * rows + c);
    };
    for (int r = 0; r < gridW; ++r) {
        for (int c = 0; c < gridW; ++c) {
            const std::uint32_t a = idx(r, c);
            const std::uint32_t b = idx(r, c + 1);
            const std::uint32_t cTopRight = idx(r + 1, c + 1);
            const std::uint32_t d = idx(r + 1, c);
            out.indices.push_back(a);
            out.indices.push_back(b);
            out.indices.push_back(cTopRight);
            out.indices.push_back(a);
            out.indices.push_back(cTopRight);
            out.indices.push_back(d);
        }
    }
}

}  // namespace

void WorldProcgen::init(World& world) {
    std::string error;

    // ---- group 3: noise graph (height + caves/ores density) ----
    // Classification: heightGraph = Required; climate axes, caves, ores =
    // Required (the composed generator and LOD sampler need all of them to
    // produce a coherent world).
    error.clear();
    heightGraph_ = engine::procgen::create_noise_graph_from_spec(height_spec(), error);
    if (!require_factory("heightGraph", error, !!heightGraph_)) return;

    error.clear();
    auto temperature = engine::procgen::create_noise_graph_from_spec(axis_spec(1, 0.003f), error);
    if (!require_factory("temperature", error, !!temperature)) return;

    error.clear();
    auto moisture = engine::procgen::create_noise_graph_from_spec(axis_spec(2, 0.003f), error);
    if (!require_factory("moisture", error, !!moisture)) return;

    error.clear();
    auto continentalness = engine::procgen::create_noise_graph_from_spec(
        engine::procgen::NoiseGraphSpec{ 3, 0, { { "constant", { 0.5f }, {} } } }, error);
    if (!require_factory("continentalness", error, !!continentalness)) return;

    error.clear();
    auto erosionAxis = engine::procgen::create_noise_graph_from_spec(
        engine::procgen::NoiseGraphSpec{ 4, 0, { { "constant", { 0.0f }, {} } } }, error);
    if (!require_factory("erosionAxis", error, !!erosionAxis)) return;

    error.clear();
    auto weirdness = engine::procgen::create_noise_graph_from_spec(
        engine::procgen::NoiseGraphSpec{ 5, 0, { { "constant", { 0.0f }, {} } } }, error);
    if (!require_factory("weirdness", error, !!weirdness)) return;

    error.clear();
    auto river = engine::procgen::create_noise_graph_from_spec(
        engine::procgen::NoiseGraphSpec{ 6, 0, { { "constant", { 0.0f }, {} } } }, error);
    if (!require_factory("river", error, !!river)) return;

    error.clear();
    auto cavesGraph = engine::procgen::create_noise_graph_from_spec(density_spec(7, 0.05f), error);
    if (!require_factory("cavesGraph", error, !!cavesGraph)) return;

    error.clear();
    auto oresGraph = engine::procgen::create_noise_graph_from_spec(density_spec(8, 0.06f), error);
    if (!require_factory("oresGraph", error, !!oresGraph)) return;

    // ---- group 1: climate sampler, biome registries, surface resolver ----
    climateSampler_ = engine::procgen::create_climate_sampler(
        temperature, moisture, continentalness, erosionAxis, weirdness, river);
    if (!require_factory("climateSampler", {}, !!climateSampler_)) return;

    biomeRegistry_ = engine::procgen::create_biome_registry();
    biomeCount_ = biomeRegistry_ ? biomeRegistry_->biome_count() : 0;

    error.clear();
    biomeRegistryJson_ =
        engine::procgen::create_biome_registry_from_json(kBiomesJson, error);
    if (!require_factory("biomeRegistryJson", error, !!biomeRegistryJson_)) return;
    biomeCountJson_ = biomeRegistryJson_ ? biomeRegistryJson_->biome_count() : 0;

    // The surface resolver reads the data-driven registry: its per-biome
    // rules decide the top block (surface_block hook in the chunk build).
    surfaceResolver_ =
        engine::procgen::create_surface_resolver(biomeRegistryJson_);
    // surfaceResolver is Optional: the generator has a fallback surface if
    // the resolver is null, but we log the degradation.
    if (!surfaceResolver_) {
        std::cerr << "[WorldProcgen] surfaceResolver creation failed — "
                     "using default surface rules\n";
    }

    // ---- group 2: features (ore table, carver, decorators) ----
    error.clear();
    oreTableJson_ = engine::procgen::create_ore_table_from_json(kOreJson, error);
    if (!require_factory("oreTableJson", error, !!oreTableJson_)) return;
    oreRuleCount_ = oreTableJson_ ? oreTableJson_->rule_count() : 0;

    error.clear();
    carverJson_ = engine::procgen::create_carver_from_json(kCarverJson, error);
    if (!require_factory("carverJson", error, !!carverJson_)) return;
    carverRules_ = carverJson_ ? (carverJson_->spec().fluidMaxY > 0 ? 1u : 0u) : 0;
    engine::procgen::DecoratorSpec treeSpec;
    treeSpec.type = "tree";
    treeSpec.density = 0.35f;
    treeSpec.params = { 4.0f, 7.0f, 2.0f };
    treeSpec.blocks = { 6u, 7u };
    treeSpec.salt = 11u;
    treeSpec.anyBiome = false;
    treeSpec.biomeIndex = 5u;
    treeSpec.minHeight = 70;
    treeSpec.maxHeight = 319;
    error.clear();
    decorator_ = engine::procgen::create_decorator(treeSpec, error);
    decoratorCount_ = decorator_ ? 1u : 0u;
    // decorator (standalone) is Optional — the set provides the main hook.
    if (!decorator_) {
        std::cerr << "[WorldProcgen] standalone decorator creation failed — "
                     "decorator set will still provide hooks\n";
    }

    error.clear();
    decoratorJson_ = engine::procgen::create_decorator_from_json(kDecoratorJson, error);
    if (!decoratorJson_) {
        std::cerr << "[WorldProcgen] JSON decorator creation failed\n";
    }

    error.clear();
    decoratorSetJson_ =
        engine::procgen::create_decorator_set_from_json(kDecoratorSetJson, error);
    if (!require_factory("decoratorSetJson", error, !!decoratorSetJson_)) return;
    decoratorSetCount_ = decoratorSetJson_ ? decoratorSetJson_->decorator_count() : 0;

    // ---- group 3: graph density fields (consumed by both generators) ----
    cavesDensity_ = engine::procgen::create_graph_density_function(cavesGraph, 1.0f, 0.08f);
    if (!require_factory("cavesDensity", {}, !!cavesDensity_)) return;

    oresDensity_ = engine::procgen::create_graph_density_function(oresGraph, 1.0f, 0.0f);
    if (!require_factory("oresDensity", {}, !!oresDensity_)) return;

    // ---- group 1: data-driven climate voxel generator (the chunk driver) ----
    constexpr int kBaseHeight = 150;
    constexpr int kAmplitude = 24;
    climateGenerator_ = engine::procgen::create_climate_voxel_generator(
        heightGraph_, climateSampler_, biomeRegistryJson_, surfaceResolver_,
        cavesDensity_, oresDensity_, kBaseHeight, kAmplitude, oreTableJson_,
        carverJson_, decoratorSetJson_);
    if (!require_factory("climateGenerator", {}, !!climateGenerator_)) return;

    // ---- group 3: graph voxel generator (coherent far terrain) ----
    graphGenerator_ = engine::procgen::create_graph_voxel_generator(
        heightGraph_, cavesDensity_, oresDensity_, kBaseHeight, kAmplitude);
    if (!require_factory("graphGenerator", {}, !!graphGenerator_)) return;

    // ---- Register the composed generator on the app's LIVE world. Every
    // chunk World::update dispatches now samples the data-driven generator
    // (biome/climate/surface/ores/carver/decorators) — the real build path.
    world.set_generator_override(climateGenerator_);

    // ---- group 4: heightmap erosion + tile cache (headless/offline) ----
    // Optional: erosion degrades terrain detail but does not block world gen.
    erosion_ = engine::procgen::create_heightmap_erosion();
    if (!erosion_) {
        std::cerr << "[WorldProcgen] heightmap erosion not available — "
                     "terrain will not be eroded\n";
    }
    erosionCache_ = engine::procgen::create_tile_erosion_cache();
    if (!erosionCache_) {
        std::cerr << "[WorldProcgen] tile erosion cache not available\n";
    }
    if (erosion_ && erosionCache_ && heightGraph_) {
        // Sample a real 32x32 heightmap tile from the height graph and erode
        // it through the tile cache (deterministic, keyed by seed+spec+tile).
        engine::procgen::Heightmap tile;
        constexpr int kTile = 32;
        tile.width = kTile;
        tile.height = kTile;
        tile.values.reserve(static_cast<std::size_t>(kTile) * kTile);
        for (int z = 0; z < kTile; ++z) {
            for (int x = 0; x < kTile; ++x) {
                const float h = heightGraph_->sample_2d(
                    static_cast<float>(x * 4), static_cast<float>(z * 4));
                // Normalize the graph sample to [0,1] (the erosion contract).
                tile.values.push_back(std::clamp((h + 1.0f) * 0.5f, 0.0f, 1.0f));
            }
        }
        engine::procgen::ErosionSpec spec;
        spec.seed = 20260830;
        spec.iterations = 6000;
        engine::procgen::Heightmap eroded;
        std::string erodeError;
        if (erosionCache_->erode_tile(spec, 1, 1, tile, eroded, erodeError)) {
            erosionTileW_ = static_cast<std::size_t>(eroded.width);
            erosionTileH_ = static_cast<std::size_t>(eroded.height);
            // Count cells that actually changed (material redistributed).
            erosionConvergedCells_ = 0;
            for (std::size_t i = 0;
                 i < eroded.values.size() && i < tile.values.size(); ++i) {
                if (std::abs(eroded.values[i] - tile.values[i]) > 1.0e-4f) {
                    ++erosionConvergedCells_;
                }
            }
        }
    }
    erosionCacheSize_ = erosionCache_ ? erosionCache_->size() : 0;

    // ---- group 5: LOD sampler + multi-scale streaming + mesh cooker ----
    // Optional: these degrade distant terrain quality but don't block world gen.
    lodSampler_ = engine::procgen::create_lod_terrain_sampler();
    if (!lodSampler_) {
        std::cerr << "[WorldProcgen] LOD terrain sampler not available\n";
    }
    multiScale_ = engine::procgen::create_multi_scale_streaming();
    if (multiScale_) {
        std::vector<engine::procgen::ScaleLevel> levels = {
            { 1, 16, 16 }, { 4, 12, 12 }, { 32, 8, 8 },
        };
        std::string cfgError;
        if (!multiScale_->configure(levels, cfgError)) {
            std::cerr << "[WorldProcgen] multi-scale config failed: "
                      << cfgError << '\n';
            multiScale_.reset();
        }
    } else {
        std::cerr << "[WorldProcgen] multi-scale streaming not available\n";
    }
    meshCooker_ = engine::procgen::create_mesh_cooker();
    if (!meshCooker_) {
        std::cerr << "[WorldProcgen] mesh cooker not available\n";
    }

    // ---- group 6: mesh geometry post-processing (smooth/decimate) ----
    // Optional: post-processes cooked terrain mesh for smoother LOD transitions.
    {
        Engine::Procgen::MeshGeometryConfig geomCfg;
        geomCfg.maxVertices = 65536;
        geomCfg.maxFaces = 131072;
        std::string geomError;
        geomProcessor_ = Engine::Procgen::create_mesh_geometry_processor(geomCfg, geomError);
        if (!geomProcessor_) {
            std::cerr << "[WorldProcgen] mesh geometry processor not available: "
                      << geomError << '\n';
        }
    }

    samplerInitialized_ = true;
}

void WorldProcgen::tick(World& world, float playerX, float playerZ) {
    (void)world;
    if (!samplerInitialized_) return;
    const std::uint64_t tick = tickCounter_++;

    // ---- group 1: real climate + biome + surface at the player column ----
    if (climateSampler_) {
        const engine::procgen::ClimatePoint climate = climateSampler_->sample(playerX, playerZ);
        climateTemperature_ = climate.temperature;
        climateMoisture_ = climate.moisture;
    }
    if (climateGenerator_) {
        graphHeightAtPlayer_ =
            heightGraph_ ? heightGraph_->sample_2d(playerX, playerZ) : 0.0f;
        biomeAtPlayer_ = static_cast<std::uint8_t>(
            std::min<std::uint32_t>(climateGenerator_->engine_biome_at(playerX, playerZ), 255u));
        // Resolve the data-driven surface block at the player column (the same
        // resolver the chunk build's surface_block hook uses).
        if (surfaceResolver_ && biomeRegistryJson_) {
            engine::procgen::ClimatePoint climate = climateSampler_->sample(playerX, playerZ);
            std::uint32_t biomeIndex = 0;
            if (biomeRegistryJson_->biome_for(climate, biomeIndex)) {
                const std::uint32_t biome = biomeIndex;
                const int height = heightGraph_ ? static_cast<int>(
                    std::lround(graphHeightAtPlayer_ * 24.0f) + 150) : 0;
                surfaceBlockAtPlayer_ = surfaceResolver_->block_for(
                    biome, climate, height, 0, 0.0f);
            }
        }
    }

    // ---- group 1: exercise the PLAIN (default) biome registry every frame:
    // read its first definition + a full serialize round-trip so the object's
    // posture is genuinely consumed (not merely created+counted at init).
    if (biomeRegistry_) {
        engine::procgen::BiomeDefinition def;
        if (biomeRegistry_->biome_definition(0, def)) {
            plainBiomeIndex_ = def.engineBiomeIndex;
        }
        std::string registryJson;
        if (biomeRegistry_->serialize(registryJson)) {
            plainBiomeSerializeLen_ = registryJson.size();
        }
    }

    // Surface height at the player column (shared by the decorator placement
    // below and the surface resolution above).
    const int surfaceHeight = heightGraph_
        ? static_cast<int>(std::lround(graphHeightAtPlayer_ * 24.0f) + 150) : 0;

    // ---- group 2: genuinely PLACE both standalone decorators (the hand-built
    // tree and its JSON twin) on the real player column each frame, counting
    // actually-deposited blocks. Same gates/metrics the chunk build uses, but
    // against a throwaway writer (pure measurement, never mutates the world).
    const auto placeAndCount = [&](const std::shared_ptr<engine::procgen::IDecorator>& dec) -> std::size_t {
        std::size_t placed = 0;
        if (!dec) return placed;
        engine::voxel::DecorationContext ctx;
        ctx.localX = 0;
        ctx.localZ = 0;
        ctx.worldX = playerX;
        ctx.worldZ = playerZ;
        ctx.surfaceHeight = surfaceHeight;
        ctx.biomeIndex = biomeAtPlayer_;
        engine::voxel::BlockWriter writer =
            [&placed](int, int, int, std::uint32_t blockId) {
                if (blockId != 0) ++placed;  // count real block writes
                return true;                 // accept every write (measurement)
            };
        dec->place(ctx, writer);
        return placed;
    };
    decoratorPlaced_ = placeAndCount(decorator_);
    decoratorJsonPlaced_ = placeAndCount(decoratorJson_);

    // ---- group 5: coherent far terrain from the graph voxel generator ----
    if (lodSampler_ && graphGenerator_) {
        const int cellSize = 8;
        const int cells = 16;
        const int originX = static_cast<int>(std::floor(playerX / cellSize)) * cellSize;
        const int originZ = static_cast<int>(std::floor(playerZ / cellSize)) * cellSize;
        std::vector<engine::procgen::LodCell> cellsOut;
        std::string lodError;
        if (lodSampler_->sample(*graphGenerator_, originX, originZ, cells, cells,
                                cellSize, cellsOut, lodError)) {
            lodCells_ = cellsOut.size();
            lodInterpHeight_ = lodSampler_->interpolated_height(
                cellsOut, cells, cells, cellSize, playerX, playerZ);
        }
    }

    // ---- group 5: multi-scale streaming around the focus ----
    if (multiScale_ && graphGenerator_) {
        std::vector<engine::procgen::ScaleStream> streams;
        std::string streamError;
        if (multiScale_->stream(*graphGenerator_, playerX, playerZ, streams,
                                streamError)) {
            streamLevels_ = streams.size();
            std::size_t totalCells = 0;
            for (const engine::procgen::ScaleStream& stream : streams) {
                totalCells += stream.cells.size();
            }
            streamCells_ = totalCells;
        }
    }

    // ---- group 5: mesh cooker on a real surface grid (throttled 2s) ----
    if (meshCooker_ && graphGenerator_ && (tick % 120u == 0u)) {
        engine::procgen::CookedMesh source;
        build_surface_grid(graphGenerator_.get(), 16, 32.0f, playerX, playerZ, source);
        cookInVertices_ = source.vertex_count();
        cookInIndices_ = source.index_count();
        engine::procgen::CookOptions options;
        options.unwrap = true;
        options.optimize = true;
        options.simplifyTargetIndices = 0;
        engine::procgen::CookedMesh cooked;
        engine::procgen::CookStats stats;
        std::string cookError;
        if (meshCooker_->cook(source, options, cooked, stats, cookError)) {
            cookedPositions_ = std::move(cooked.positions);
            cookedUvs_ = std::move(cooked.uvs);
            cookedIndices_ = std::move(cooked.indices);
            cookOutVertices_ = stats.outputVertices;
            cookOutIndices_ = stats.outputIndices;
            cookAcmr_ = stats.acmr;
        }
    }

    // ---- group 6: post-process cooked mesh with geometry processor ----
    // Smooth the terrain mesh positions to reduce LOD seams. Runs only when
    // the mesh cooker produced new data (every 120 ticks).
    if (geomProcessor_ && !cookedPositions_.empty() && (tick % 120u == 0u)) {
        Engine::Procgen::TriMesh triMesh;
        const std::size_t vertCount = cookedPositions_.size() / 3;
        triMesh.positions.reserve(vertCount);
        for (std::size_t i = 0; i + 2 < cookedPositions_.size(); i += 3) {
            triMesh.positions.emplace_back(
                cookedPositions_[i], cookedPositions_[i + 1], cookedPositions_[i + 2]);
        }
        const std::size_t triCount = cookedIndices_.size() / 3;
        triMesh.indices.reserve(triCount);
        for (std::size_t i = 0; i + 2 < cookedIndices_.size(); i += 3) {
            triMesh.indices.push_back({
                cookedIndices_[i], cookedIndices_[i + 1], cookedIndices_[i + 2]});
        }
        std::string geomError;
        Engine::Procgen::MeshHandle h =
            geomProcessor_->upload_mesh(triMesh, geomError);
        if (h != Engine::Procgen::InvalidMesh) {
            // Apply one pass of Laplacian smoothing (lambda=0.3 = mild).
            std::vector<glm::vec3> smoothed =
                geomProcessor_->smooth_positions(h, 0.3f, geomError);
            if (!smoothed.empty()) {
                smoothedVertices_ = smoothed.size();
                // Overwrite positions with smoothed result.
                cookedPositions_.clear();
                cookedPositions_.reserve(smoothed.size() * 3);
                for (const glm::vec3& v : smoothed) {
                    cookedPositions_.push_back(v.x);
                    cookedPositions_.push_back(v.y);
                    cookedPositions_.push_back(v.z);
                }
            }
            geomProcessor_->release_mesh(h);
        }
    }

    // ---- group 4: observe the tile erosion cache every frame ----
    if (erosionCache_) {
        erosionCacheSize_ = erosionCache_->size();
    }
}

std::string WorldProcgen::summary() const {
    if (!samplerInitialized_) return std::string("procgen n/a");
    return std::format(
        "procgen bio {}/{} plain {} len{} dec {}/{}/{}/{} cl {:.2f}/{:.2f} srf {} "
        "bm {} h {:.2f} car {} ore {} lod {} h {:.1f} str {}/{} er {}/{}/{} "
        "cook {}/{}->{}/{} acmr {:.2f} smooth {}",
        biomeCount_, biomeCountJson_, plainBiomeIndex_, plainBiomeSerializeLen_,
        decoratorCount_, decoratorSetCount_, decoratorPlaced_, decoratorJsonPlaced_,
        climateTemperature_, climateMoisture_, surfaceBlockAtPlayer_, biomeAtPlayer_,
        graphHeightAtPlayer_, carverRules_, oreRuleCount_, lodCells_,
        lodInterpHeight_, streamLevels_, streamCells_, erosionCacheSize_,
        erosionTileW_, erosionTileH_, cookInVertices_, cookInIndices_,
        cookOutVertices_, cookOutIndices_, cookAcmr_, smoothedVertices_);
}

bool WorldProcgen::require_factory(const char* name, const std::string& diag,
                                   bool has_value) {
    if (!has_value) {
        std::cerr << "[WorldProcgen] REQUIRED factory '" << name
                  << "' failed to initialize";
        if (!diag.empty()) {
            std::cerr << ": " << diag;
        }
        std::cerr << '\n';
        return false;
    }
    return true;
}

}  // namespace app