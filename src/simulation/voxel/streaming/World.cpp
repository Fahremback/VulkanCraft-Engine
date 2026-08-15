#include "World.hpp"
#include "VoxelMesher.hpp"
#include "ChunkLighting.hpp"
#include "Mob.hpp"
#include "Structures.hpp"
#include "TerrainGenerator.hpp"
#include <cmath>
#include <algorithm>
#include <array>
#include <thread>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace {
constexpr std::array<int, 10> kChunkBudgetPresets{
    8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096
};
constexpr double kMinimumFarLodEndpointPercent = 0.001;
constexpr double kMaximumFarLodEndpointPercent = 99.0;
// Um toque equivale a 0.01 decada: aproximadamente 2.33%. Isso fornece 501
// posicoes ao longo da faixa completa; segurar a tecla acelera sem sacrificar
// o ajuste fino por toque.
constexpr double kFarLodLogStep = 0.01;
constexpr int kDenseLodRadius = 5;
constexpr int kDenseLodBudget = (kDenseLodRadius * 2 + 1) * (kDenseLodRadius * 2 + 1);
// Quantos chunks com luz suja são relightados por frame (orçamento da §12).
constexpr int kMaxLightChunksPerFrame = 4;

void print_far_lod_curve(float endpointFraction, int reachChunks) {
    const int safeReach = std::max(1, reachChunks);
    const auto qualityAt = [endpointFraction, safeReach](int distance) {
        const float normalizedDistance = std::clamp(
            static_cast<float>(distance) / static_cast<float>(safeReach), 0.0f, 1.0f);
        return std::pow(endpointFraction, normalizedDistance);
    };

    std::ostringstream message;
    message << std::setprecision(7)
            << "[LOD] qualidade final " << endpointFraction * 100.0f << "%"
            << " | Q(d)=pow(" << endpointFraction << ", d/" << safeReach << ')'
            << " | Q(0)=" << qualityAt(0) * 100.0f << '%'
            << " | Q(1)=" << qualityAt(1) * 100.0f << '%'
            << " | Q(" << safeReach / 2 << ")="
            << qualityAt(safeReach / 2) * 100.0f << '%'
            << " | Q(" << safeReach << ")=" << qualityAt(safeReach) * 100.0f << '%';
    std::cout << message.str() << std::endl;
}
}

World::World(MobManager& mobManager, size_t workerThreads)
    : mobManager_(mobManager),
      threadPool(workerThreads == 0 ? std::thread::hardware_concurrency() : workerThreads) {
    noise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    noise.SetFrequency(0.015f);
    set_chunk_budget(chunkBudget);
    print_far_lod_curve(far_lod_endpoint_fraction(), chunkBudget);

    // The scheduler owns the simulation clock (META section 9). The FluidTick
    // phase feeds the cell queue (dedup by cell); the tick callback runs the
    // fluid simulation itself at the fixed cadence with the latest player
    // focus. Both run on the frame thread inside update(); no locks needed.
    scheduler_.set_handler(WorldScheduler::Phase::BlockTick,
                           [this](const TickCell& cell) {
        // Block entities tick through the BlockTick phase: one cell per entity
        // (dedup), deterministic (x,y,z) order, budgeted, and sleeping outside
        // the active region — all scheduler guarantees, no extra machinery.
        const auto found = blockEntities_.find(cell);
        if (found == blockEntities_.end()) return;
        found->second->on_tick(scheduler_.current_tick());
        // Persistent per-tick ticking: re-queue for the next tick. The dedup
        // keeps it a single cell; the active region and budget gate it like
        // any block tick; eviction still cancels it (reconcile re-queues on
        // chunk reload).
        scheduler_.schedule_block_tick(cell, 0);
    });
    scheduler_.set_handler(WorldScheduler::Phase::FluidTick,
                           [this](const TickCell& cell) {
        const FluidCell fluidCell{ cell.x, cell.y, cell.z };
        if (fluidCell.y < 0 || fluidCell.y >= CHUNK_SIZE_Y) return;
        // Data-driven fluids (META section 13): cells of blocks that are no
        // longer a fluid are dropped; per-fluid cadence (tickInterval) keeps
        // the cell queued (dedup) until its step is due.
        const FluidParams* params = fluid_params_for_id(get_block_at(glm::vec3(
            static_cast<float>(cell.x), static_cast<float>(cell.y),
            static_cast<float>(cell.z))));
        if (!params) return;
        if (params->tickEveryTicks > 1 &&
            scheduler_.current_tick() % params->tickEveryTicks != 0) {
            scheduler_.schedule_fluid_tick(cell);
            return;
        }
        if (activeFluidSet.contains(fluidCell)) return;
        activeFluidSet.insert(fluidCell);
        activeFluidCells.push_back(fluidCell);
    });
    scheduler_.set_tick_callback([this](uint64_t) {
        update_fluid_physics(lastFluidPlayerPos_);
    });
}

World::~World() {
    // Drain the pool before any member state is destroyed: generation/mesh
    // workers capture `this` and can still be inside create_chunk_snapshot /
    // VoxelMesher::build when the world goes away.
    threadPool.wait_idle();
}

void World::set_generator_override(std::shared_ptr<engine::voxel::IVoxelGenerator> generator) {
    m_generatorOverride = std::move(generator);
}

void World::set_runtime_block_table(
    std::unordered_map<RuntimeBlockId, RuntimeBlockInfo> table,
    std::unordered_map<std::string, RuntimeBlockId> uuidToId) {
    std::lock_guard<std::recursive_mutex> lock(chunksMutex);
    runtimeBlocks_ = std::move(table);
    runtimeUuidToId_ = std::move(uuidToId);
}

bool World::is_valid_block_id(RuntimeBlockId id) const {
    if (is_builtin_block(id)) return true;
    std::lock_guard<std::recursive_mutex> lock(chunksMutex);
    return runtimeBlocks_.find(id) != runtimeBlocks_.end();
}

bool World::is_solid_block_id(RuntimeBlockId id) const {
    if (is_builtin_block(id)) return is_solid_block(static_cast<BlockType>(id));
    std::lock_guard<std::recursive_mutex> lock(chunksMutex);
    const auto found = runtimeBlocks_.find(id);
    return found == runtimeBlocks_.end() ? false : found->second.solid;
}

std::optional<RuntimeBlockId> World::runtime_block_id_for_uuid(const std::string& uuid) const {
    std::lock_guard<std::recursive_mutex> lock(chunksMutex);
    const auto found = runtimeUuidToId_.find(uuid);
    if (found != runtimeUuidToId_.end()) return found->second;
    return std::nullopt;
}

std::vector<std::pair<RuntimeBlockId, RuntimeBlockInfo>> World::runtime_block_table() const {
    std::lock_guard<std::recursive_mutex> lock(chunksMutex);
    std::vector<std::pair<RuntimeBlockId, RuntimeBlockInfo>> table;
    table.reserve(runtimeBlocks_.size());
    for (const auto& [id, info] : runtimeBlocks_) table.emplace_back(id, info);
    // Deterministic order for serialization (unordered_map iteration is not).
    std::sort(table.begin(), table.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return table;
}

void World::set_fluid_table(std::unordered_map<RuntimeBlockId, FluidParams> table) {
    std::lock_guard<std::recursive_mutex> lock(chunksMutex);
    fluidTable_ = std::move(table);
}

const FluidParams* World::fluid_params_for_id(RuntimeBlockId id) const {
    std::lock_guard<std::recursive_mutex> lock(chunksMutex);
    const auto found = fluidTable_.find(id);
    return found == fluidTable_.end() ? nullptr : &found->second;
}

bool World::is_fluid_runtime_id(RuntimeBlockId id) const {
    return fluid_params_for_id(id) != nullptr;
}

bool World::is_fluid_block_at(const glm::vec3& worldPos) const {
    return is_fluid_runtime_id(get_block_at(worldPos));
}

const FluidParams* World::fluid_params_at(const glm::ivec3& worldPos) const {
    return fluid_params_for_id(get_block_at(glm::vec3(
        static_cast<float>(worldPos.x), static_cast<float>(worldPos.y),
        static_cast<float>(worldPos.z))));
}

uint8_t World::get_fluid_level_at(const glm::vec3& worldPos) const {
    const int cx = static_cast<int>(std::floor(worldPos.x / CHUNK_SIZE_X));
    const int cz = static_cast<int>(std::floor(worldPos.z / CHUNK_SIZE_Z));
    const int bx = static_cast<int>(std::floor(worldPos.x)) - cx * CHUNK_SIZE_X;
    const int by = static_cast<int>(std::floor(worldPos.y));
    const int bz = static_cast<int>(std::floor(worldPos.z)) - cz * CHUNK_SIZE_Z;
    if (by < 0 || by >= CHUNK_SIZE_Y) return WATER_LEVEL_NONE;
    std::lock_guard<std::recursive_mutex> lock(chunksMutex);
    auto it = chunks.find({ cx, cz });
    if (it == chunks.end() || !it->second) return WATER_LEVEL_NONE;
    const ChunkState state = it->second->state.load(std::memory_order_acquire);
    return state != ChunkState::Generating && state != ChunkState::Unloaded
        ? it->second->get_fluid_level(bx, by, bz) : WATER_LEVEL_NONE;
}

void World::set_fluid_level_at(const glm::vec3& worldPos, uint8_t level) {
    const int cx = static_cast<int>(std::floor(worldPos.x / CHUNK_SIZE_X));
    const int cz = static_cast<int>(std::floor(worldPos.z / CHUNK_SIZE_Z));
    const int bx = static_cast<int>(std::floor(worldPos.x)) - cx * CHUNK_SIZE_X;
    const int by = static_cast<int>(std::floor(worldPos.y));
    const int bz = static_cast<int>(std::floor(worldPos.z)) - cz * CHUNK_SIZE_Z;
    if (by < 0 || by >= CHUNK_SIZE_Y) return;
    std::lock_guard<std::recursive_mutex> lock(chunksMutex);
    auto it = chunks.find({ cx, cz });
    if (it == chunks.end() || !it->second) return;
    const ChunkState st = it->second->state.load(std::memory_order_acquire);
    if (st == ChunkState::Unloaded || st == ChunkState::Generating) return;
    it->second->set_fluid_level(bx, by, bz, level);
    it->second->isDirty = true;
    if (bx == 0) { auto n = chunks.find({ cx - 1, cz }); if (n != chunks.end() && n->second) n->second->isDirty = true; }
    if (bx == CHUNK_SIZE_X - 1) { auto n = chunks.find({ cx + 1, cz }); if (n != chunks.end() && n->second) n->second->isDirty = true; }
    if (bz == 0) { auto n = chunks.find({ cx, cz - 1 }); if (n != chunks.end() && n->second) n->second->isDirty = true; }
    if (bz == CHUNK_SIZE_Z - 1) { auto n = chunks.find({ cx, cz + 1 }); if (n != chunks.end() && n->second) n->second->isDirty = true; }
    // Propagation (like set_water_at): every level write re-enqueues the
    // neighborhood so the fluid keeps flowing cell by cell.
    enqueue_fluid_neighborhood(worldPos);
    // Fluids absorb light: relight the chunk and its neighbors.
    mark_chunk_light_dirty(cx, cz);
    mark_chunk_light_dirty(cx - 1, cz);
    mark_chunk_light_dirty(cx + 1, cz);
    mark_chunk_light_dirty(cx, cz - 1);
    mark_chunk_light_dirty(cx, cz + 1);
}

ChunkSnapshot World::create_chunk_snapshot(const Chunk& chunk) const {
    std::lock_guard<std::recursive_mutex> lock(chunksMutex);
    ChunkSnapshot snapshot;
    snapshot.id = chunk.id();
    snapshot.revision = chunk.revision();
    snapshot.verticalExtent = chunk.vertical_render_extent();

    const int denseHeight = std::clamp(chunk.highestOccupiedY + 1, 1, GENERATED_TERRAIN_HEIGHT);
    snapshot.layers.reserve(static_cast<std::size_t>(denseHeight) +
                            chunk.upperSections.size() * VERTICAL_SECTION_SIZE);
    for (int y = 0; y < denseHeight; ++y) snapshot.layers.push_back(y);
    for (const auto& [sectionIndex, section] : chunk.upperSections) {
        if (!section || sectionIndex * VERTICAL_SECTION_SIZE < GENERATED_TERRAIN_HEIGHT) continue;
        const int firstY = sectionIndex * VERTICAL_SECTION_SIZE;
        const int finalY = std::min(firstY + VERTICAL_SECTION_SIZE, CHUNK_SIZE_Y);
        for (int y = firstY; y < finalY; ++y) snapshot.layers.push_back(y);
    }
    std::sort(snapshot.layers.begin(), snapshot.layers.end());
    snapshot.layers.erase(std::unique(snapshot.layers.begin(), snapshot.layers.end()), snapshot.layers.end());

    for (const int y : snapshot.layers) {
        auto& center = snapshot.center[y];
        auto& halo = snapshot.halo[y];
        for (int z = 0; z < CHUNK_SIZE_Z; ++z) {
            for (int x = 0; x < CHUNK_SIZE_X; ++x) {
                const std::size_t index = static_cast<std::size_t>(z * CHUNK_SIZE_X + x);
                center.blocks[index] = chunk.get_block(x, y, z);
                center.water[index] = chunk.get_water_level(x, y, z);
            }
        }
        for (int localZ = -1; localZ <= CHUNK_SIZE_Z; ++localZ) {
            for (int localX = -1; localX <= CHUNK_SIZE_X; ++localX) {
                const std::size_t haloIndex = static_cast<std::size_t>(
                    (localZ + 1) * NeighborVoxelHaloLayer::Width + localX + 1);
                if (localX >= 0 && localX < CHUNK_SIZE_X &&
                    localZ >= 0 && localZ < CHUNK_SIZE_Z) {
                    halo.blocks[haloIndex] = chunk.get_block(localX, y, localZ);
                    halo.water[haloIndex] = chunk.get_water_level(localX, y, localZ);
                    halo.known[haloIndex] = 1;
                    continue;
                }
                const int dx = localX < 0 ? -1 : (localX >= CHUNK_SIZE_X ? 1 : 0);
                const int dz = localZ < 0 ? -1 : (localZ >= CHUNK_SIZE_Z ? 1 : 0);
                const auto found = chunks.find({chunk.chunkX + dx, chunk.chunkZ + dz});
                // Um vizinho em Generating ainda está sendo escrito pelo worker:
                // lê-lo aqui seria data race (UB). Trata-se como desconhecido e o
                // gap é fechado depois, quando o vizinho sobe e força o remesh.
                if (found == chunks.end() || !found->second ||
                    found->second->state.load(std::memory_order_acquire) == ChunkState::Unloaded ||
                    found->second->state.load(std::memory_order_acquire) == ChunkState::Generating) continue;
                snapshot.neighborSeen.push_back(NeighborSeen{
                    { found->second->chunkX, found->second->chunkZ },
                    found->second->revision() });
                const int nx = localX < 0 ? CHUNK_SIZE_X - 1 :
                    (localX >= CHUNK_SIZE_X ? 0 : localX);
                const int nz = localZ < 0 ? CHUNK_SIZE_Z - 1 :
                    (localZ >= CHUNK_SIZE_Z ? 0 : localZ);
                halo.blocks[haloIndex] = found->second->get_block(nx, y, nz);
                halo.water[haloIndex] = found->second->get_water_level(nx, y, nz);
                halo.known[haloIndex] = 1;
            }
        }
    }
    // Embed the dynamic block table so mesh workers never touch the registry
    // or the world (data race). The table is built once at set_block_registry.
    snapshot.runtimeBlocks = runtime_block_table();
    return snapshot;
}

bool World::apply_mesh_result(ChunkMeshResult result) {
    std::lock_guard<std::recursive_mutex> lock(chunksMutex);
    const auto found = chunks.find({result.chunk.coord.x, result.chunk.coord.z});
    if (found == chunks.end() || !found->second || found->second->id() != result.chunk ||
        found->second->revision() != result.sourceRevision || !result.valid) {
        return false;
    }
    Chunk& chunk = *found->second;
    static_cast<ChunkMeshData&>(chunk) = std::move(result.mesh);
    chunk.meshNeighborSeen = std::move(result.neighborSeen);
    chunk.state.store(ChunkState::MeshReady, std::memory_order_release);
    chunk.isDirty.store(false, std::memory_order_release);
    return true;
}

void World::set_chunk_budget(int budget) {
    chunkBudget = std::clamp(budget, MIN_CHUNK_BUDGET, MAX_CHUNK_BUDGET);
    renderDistance = chunkBudget;
}

void World::cycle_chunk_budget(int direction) {
    if (direction == 0) return;
    auto current = std::lower_bound(kChunkBudgetPresets.begin(), kChunkBudgetPresets.end(), chunkBudget);
    int index = current == kChunkBudgetPresets.end()
        ? static_cast<int>(kChunkBudgetPresets.size()) - 1
        : static_cast<int>(std::distance(kChunkBudgetPresets.begin(), current));
    index = std::clamp(index + (direction > 0 ? 1 : -1), 0,
                       static_cast<int>(kChunkBudgetPresets.size()) - 1);
    set_chunk_budget(kChunkBudgetPresets[static_cast<std::size_t>(index)]);
}

float World::far_lod_endpoint_percent() const {
    return static_cast<float>(farLodEndpointPercentValue);
}

float World::far_lod_endpoint_fraction() const {
    return far_lod_endpoint_percent() * 0.01f;
}

void World::adjust_far_lod_quality(int direction, int steps) {
    if (direction == 0 || steps <= 0) return;
    const double multiplier = std::pow(10.0, kFarLodLogStep *
        static_cast<double>(direction > 0 ? steps : -steps));
    farLodEndpointPercentValue = std::clamp(
        farLodEndpointPercentValue * multiplier,
        kMinimumFarLodEndpointPercent, kMaximumFarLodEndpointPercent);
    print_far_lod_curve(far_lod_endpoint_fraction(), chunkBudget);
}

bool World::inside_stable_frontier(const std::pair<int, int>& key) const {
    return stableVisibleRadius >= 0 &&
        std::max(std::abs(key.first - visibleCenterChunkX),
                 std::abs(key.second - visibleCenterChunkZ)) <= stableVisibleRadius;
}

RuntimeBlockId World::get_block_at(const glm::vec3& worldPos) const {
    int cx = static_cast<int>(std::floor(worldPos.x / CHUNK_SIZE_X));
    int cz = static_cast<int>(std::floor(worldPos.z / CHUNK_SIZE_Z));

    int bx = static_cast<int>(std::floor(worldPos.x)) - cx * CHUNK_SIZE_X;
    int by = static_cast<int>(std::floor(worldPos.y));
    int bz = static_cast<int>(std::floor(worldPos.z)) - cz * CHUNK_SIZE_Z;

    if (by < 0 || by >= CHUNK_SIZE_Y) return kRuntimeAirId;

    std::pair<int, int> key = { cx, cz };
    std::lock_guard<std::recursive_mutex> lock(chunksMutex);
    auto it = chunks.find(key);
    if (it != chunks.end() && it->second &&
        it->second->state.load(std::memory_order_acquire) != ChunkState::Generating &&
        it->second->state.load(std::memory_order_relaxed) != ChunkState::Unloaded) {
        return it->second->get_block(bx, by, bz);
    }
    return kRuntimeAirId;
}

uint8_t World::get_water_level_at(const glm::vec3& worldPos) const {
    const int cx = static_cast<int>(std::floor(worldPos.x / CHUNK_SIZE_X));
    const int cz = static_cast<int>(std::floor(worldPos.z / CHUNK_SIZE_Z));
    const int bx = static_cast<int>(std::floor(worldPos.x)) - cx * CHUNK_SIZE_X;
    const int by = static_cast<int>(std::floor(worldPos.y));
    const int bz = static_cast<int>(std::floor(worldPos.z)) - cz * CHUNK_SIZE_Z;
    if (by < 0 || by >= CHUNK_SIZE_Y) return WATER_LEVEL_NONE;
    std::lock_guard<std::recursive_mutex> lock(chunksMutex);
    auto it = chunks.find({ cx, cz });
    if (it == chunks.end() || !it->second) return WATER_LEVEL_NONE;
    const ChunkState state = it->second->state.load(std::memory_order_acquire);
    return state != ChunkState::Generating && state != ChunkState::Unloaded
        ? it->second->get_water_level(bx, by, bz) : WATER_LEVEL_NONE;
}

void World::enqueue_fluid_neighborhood(const glm::vec3& worldPos) {
    // Scheduling goes through the scheduler's FluidTick phase (dedup by cell,
    // active-region sleep, budget and cancel-on-eviction all live there); the
    // phase handler copies due cells into the queue that the fluid step drains.
    const FluidCell center{ static_cast<int>(std::floor(worldPos.x)), static_cast<int>(std::floor(worldPos.y)), static_cast<int>(std::floor(worldPos.z)) };
    const FluidCell offsets[7] = { {0,0,0}, {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} };
    for (const FluidCell& offset : offsets) {
        FluidCell cell{ center.x + offset.x, center.y + offset.y, center.z + offset.z };
        if (cell.y < 0 || cell.y >= CHUNK_SIZE_Y) continue;
        scheduler_.schedule_fluid_tick(TickCell{ cell.x, cell.y, cell.z });
    }
}

void World::set_block_at(const glm::vec3& worldPos, RuntimeBlockId type) {
    // Unknown dynamic ids are rejected here (single source of truth). Builtin
    // ids and registry-registered dynamic ids are the only settable values.
    if (!is_valid_block_id(type)) return;

    int cx = static_cast<int>(std::floor(worldPos.x / CHUNK_SIZE_X));
    int cz = static_cast<int>(std::floor(worldPos.z / CHUNK_SIZE_Z));

    int bx = static_cast<int>(std::floor(worldPos.x)) - cx * CHUNK_SIZE_X;
    int by = static_cast<int>(std::floor(worldPos.y));
    int bz = static_cast<int>(std::floor(worldPos.z)) - cz * CHUNK_SIZE_Z;

    if (by < 0 || by >= CHUNK_SIZE_Y) return;

    std::pair<int, int> key = { cx, cz };
    std::lock_guard<std::recursive_mutex> lock(chunksMutex);
    auto it = chunks.find(key);
    if (it != chunks.end() && it->second) {
        // Nunca editar um chunk cujo worker ainda está gerando (data race).
        const ChunkState st = it->second->state.load(std::memory_order_acquire);
        if (st == ChunkState::Unloaded || st == ChunkState::Generating) return;
        it->second->set_block(bx, by, bz, type);
        it->second->isDirty = true;

        // Placing any fluid creates a source cell (level 0, always fed) — the
        // water special case generalized to data-driven fluids (META §13).
        // Water also gets this via Chunk::set_block; the level write is
        // idempotent.
        if (is_fluid_runtime_id(type)) {
            it->second->set_fluid_level(bx, by, bz, WATER_SOURCE_LEVEL);
        }

        if (bx == 0) { auto n = chunks.find({ cx - 1, cz }); if (n != chunks.end() && n->second) n->second->isDirty = true; }
        if (bx == CHUNK_SIZE_X - 1) { auto n = chunks.find({ cx + 1, cz }); if (n != chunks.end() && n->second) n->second->isDirty = true; }
        if (bz == 0) { auto n = chunks.find({ cx, cz - 1 }); if (n != chunks.end() && n->second) n->second->isDirty = true; }
        if (bz == CHUNK_SIZE_Z - 1) { auto n = chunks.find({ cx, cz + 1 }); if (n != chunks.end() && n->second) n->second->isDirty = true; }
        enqueue_fluid_neighborhood(worldPos);

        // Atomic destroy with the block (META section 8): a block entity cannot
        // outlive its block. Air is the only removal the engine knows; non-Air
        // replacement keeps the entity (the project decides compatibility).
        if (type == kRuntimeAirId) {
            destroy_block_entity_at(TickCell{
                static_cast<int>(std::floor(worldPos.x)),
                static_cast<int>(std::floor(worldPos.y)),
                static_cast<int>(std::floor(worldPos.z)) }, true);
        }

        // Discrete light is stale after any edit: relight this chunk and its
        // four neighbors (block light crosses borders).
        mark_chunk_light_dirty(cx, cz);
        mark_chunk_light_dirty(cx - 1, cz);
        mark_chunk_light_dirty(cx + 1, cz);
        mark_chunk_light_dirty(cx, cz - 1);
        mark_chunk_light_dirty(cx, cz + 1);
    }
}

void World::set_water_at(const glm::vec3& worldPos, uint8_t level) {
    const int cx = static_cast<int>(std::floor(worldPos.x / CHUNK_SIZE_X));
    const int cz = static_cast<int>(std::floor(worldPos.z / CHUNK_SIZE_Z));
    const int bx = static_cast<int>(std::floor(worldPos.x)) - cx * CHUNK_SIZE_X;
    const int by = static_cast<int>(std::floor(worldPos.y));
    const int bz = static_cast<int>(std::floor(worldPos.z)) - cz * CHUNK_SIZE_Z;
    if (by < 0 || by >= CHUNK_SIZE_Y) return;
    std::lock_guard<std::recursive_mutex> lock(chunksMutex);
    auto it = chunks.find({ cx, cz });
    if (it == chunks.end() || !it->second) return;
    const ChunkState st = it->second->state.load(std::memory_order_acquire);
    if (st == ChunkState::Unloaded || st == ChunkState::Generating) return;
    it->second->set_water(bx, by, bz, level);
    it->second->isDirty = true;
    if (bx == 0) { auto n = chunks.find({ cx - 1, cz }); if (n != chunks.end() && n->second) n->second->isDirty = true; }
    if (bx == CHUNK_SIZE_X - 1) { auto n = chunks.find({ cx + 1, cz }); if (n != chunks.end() && n->second) n->second->isDirty = true; }
    if (bz == 0) { auto n = chunks.find({ cx, cz - 1 }); if (n != chunks.end() && n->second) n->second->isDirty = true; }
    if (bz == CHUNK_SIZE_Z - 1) { auto n = chunks.find({ cx, cz + 1 }); if (n != chunks.end() && n->second) n->second->isDirty = true; }
    enqueue_fluid_neighborhood(worldPos);
    // Water affects absorption: relight the chunk and its neighbors.
    mark_chunk_light_dirty(cx, cz);
    mark_chunk_light_dirty(cx - 1, cz);
    mark_chunk_light_dirty(cx + 1, cz);
    mark_chunk_light_dirty(cx, cz - 1);
    mark_chunk_light_dirty(cx, cz + 1);
}

void World::force_fluid_tick() {
    update_fluid_physics(lastFluidPlayerPos_);
}

void World::register_block_entity_type(const std::string& typeId,
                                       engine::voxel::BlockEntityFactory factory) {
    if (factory) blockEntityFactories_[typeId] = std::move(factory);
}

engine::voxel::BlockEntityFactory World::find_block_entity_factory(
    const std::string& typeId) const {
    const auto found = blockEntityFactories_.find(typeId);
    return found == blockEntityFactories_.end() ? nullptr : found->second;
}

bool World::attach_block_entity(
    int x, int y, int z, std::shared_ptr<engine::voxel::IVoxelBlockEntity> entity,
    std::string& errorOut) {
    if (!entity) {
        errorOut = "block entity is null";
        return false;
    }
    if (!find_block_entity_factory(entity->type_id())) {
        errorOut = "block entity type '" + entity->type_id() +
                   "' has no registered factory (a save could not reconstruct it)";
        return false;
    }
    const TickCell key{ x, y, z };
    {
        std::lock_guard<std::recursive_mutex> lock(chunksMutex);
        if (blockEntities_.contains(key)) {
            errorOut = "a block entity already exists at (" +
                       std::to_string(x) + ',' + std::to_string(y) + ',' +
                       std::to_string(z) + ')';
            return false;
        }
        if (get_block_at(glm::vec3(static_cast<float>(x), static_cast<float>(y),
                                   static_cast<float>(z))) == kRuntimeAirId) {
            errorOut = "cannot attach a block entity to an empty block";
            return false;
        }
        blockEntities_[key] = entity;
    }
    scheduler_.schedule_block_tick(key, 0);
    entity->on_created();
    if (blockEntityListener_) {
        engine::voxel::BlockEntityEvent event;
        event.kind = engine::voxel::BlockEntityEvent::Kind::Attached;
        event.position = { x, y, z };
        event.typeId = entity->type_id();
        blockEntityListener_(event);
    }
    return true;
}

std::shared_ptr<engine::voxel::IVoxelBlockEntity> World::block_entity_at(
    int x, int y, int z) const {
    std::lock_guard<std::recursive_mutex> lock(chunksMutex);
    const auto found = blockEntities_.find(TickCell{ x, y, z });
    return found == blockEntities_.end() ? nullptr : found->second;
}

bool World::remove_block_entity(int x, int y, int z) {
    const TickCell key{ x, y, z };
    if (!blockEntities_.contains(key)) return false;
    destroy_block_entity_at(key, true);
    return true;
}

void World::set_block_entity_listener(
    std::function<void(const engine::voxel::BlockEntityEvent&)> listener) {
    blockEntityListener_ = std::move(listener);
}

void World::restore_block_entity(
    int x, int y, int z, std::shared_ptr<engine::voxel::IVoxelBlockEntity> entity) {
    if (!entity) return;
    {
        std::lock_guard<std::recursive_mutex> lock(chunksMutex);
        blockEntities_[TickCell{ x, y, z }] = std::move(entity);
    }
    scheduler_.schedule_block_tick(TickCell{ x, y, z }, 0);
}

void World::destroy_block_entity_at(const TickCell& key, bool notify) {
    const auto found = blockEntities_.find(key);
    if (found == blockEntities_.end()) return;
    const std::shared_ptr<engine::voxel::IVoxelBlockEntity> entity = found->second;
    blockEntities_.erase(found);
    scheduler_.cancel_cell(key);
    if (entity) entity->on_destroyed();
    if (notify && blockEntityListener_) {
        engine::voxel::BlockEntityEvent event;
        event.kind = engine::voxel::BlockEntityEvent::Kind::Detached;
        event.position = { key.x, key.y, key.z };
        if (entity) event.typeId = entity->type_id();
        blockEntityListener_(event);
    }
}

void World::reconcile_block_entities(int chunkX, int chunkZ) {
    const int minX = chunkX * CHUNK_SIZE_X;
    const int minZ = chunkZ * CHUNK_SIZE_Z;
    const int maxX = minX + CHUNK_SIZE_X;
    const int maxZ = minZ + CHUNK_SIZE_Z;
    std::vector<TickCell> stale;
    std::vector<TickCell> survivors;
    for (const auto& [cell, entity] : blockEntities_) {
        if (cell.x < minX || cell.x >= maxX || cell.z < minZ || cell.z >= maxZ) {
            continue;
        }
        if (get_block_at(glm::vec3(static_cast<float>(cell.x),
                                   static_cast<float>(cell.y),
                                   static_cast<float>(cell.z))) == kRuntimeAirId) {
            stale.push_back(cell);
        } else {
            survivors.push_back(cell);
        }
    }
    for (const TickCell& cell : stale) destroy_block_entity_at(cell, true);
    for (const TickCell& cell : survivors) scheduler_.schedule_block_tick(cell, 0);
}

bool World::is_chunk_loaded_at(const glm::vec3& worldPos) const {
    int cx = static_cast<int>(std::floor(worldPos.x / CHUNK_SIZE_X));
    int cz = static_cast<int>(std::floor(worldPos.z / CHUNK_SIZE_Z));

    std::pair<int, int> key = { cx, cz };
    std::lock_guard<std::recursive_mutex> lock(chunksMutex);
    auto it = chunks.find(key);
    if (it != chunks.end() && it->second) {
        return it->second->state.load() == ChunkState::Uploaded;
    }
    return false;
}

void World::update_fluid_physics(const glm::vec3& playerPos) {
    (void)playerPos;
    const FluidCell horizontal[4] = { {1,0,0}, {-1,0,0}, {0,0,1}, {0,0,-1} };
    auto pos = [](const FluidCell& c) { return glm::vec3(static_cast<float>(c.x), static_cast<float>(c.y), static_cast<float>(c.z)); };

    std::lock_guard<std::recursive_mutex> lock(chunksMutex);
    const size_t workCount = std::min<size_t>(activeFluidCells.size(), 512);
    for (size_t work = 0; work < workCount; ++work) {
        const FluidCell cell = activeFluidCells.front();
        activeFluidCells.pop_front();
        activeFluidSet.erase(cell);
        const glm::vec3 worldPos = pos(cell);

        // Data-driven fluids (META section 13): the step is driven by the
        // fluid's parameters (range, viscosity step, source/falling/
        // evaporation, damage) instead of a hardcoded water case. Cells of
        // blocks that are not (or no longer) fluids are dropped.
        const RuntimeBlockId type = get_block_at(worldPos);
        const FluidParams* params = fluid_params_for_id(type);
        if (!params) continue;

        const uint8_t oldLevel = get_fluid_level_at(worldPos);
        uint8_t stableLevel = oldLevel;
        bool fed = oldLevel == WATER_SOURCE_LEVEL;

        if (!fed) {
            const uint8_t above = get_fluid_level_at(pos({ cell.x, cell.y + 1, cell.z }));
            if (above != WATER_LEVEL_NONE) {
                stableLevel = static_cast<uint8_t>(WATER_FALLING_FLAG | water_base_level(above));
                fed = true;
            } else {
                uint8_t best = WATER_LEVEL_NONE;
                for (const FluidCell& d : horizontal) {
                    const FluidCell neighbor{ cell.x + d.x, cell.y, cell.z + d.z };
                    const uint8_t level = get_fluid_level_at(pos(neighbor));
                    if (level == WATER_LEVEL_NONE || get_block_at(pos({ neighbor.x, neighbor.y - 1, neighbor.z })) == kRuntimeAirId) continue;
                    const uint8_t candidate = static_cast<uint8_t>(
                        water_base_level(level) + params->levelsPerTick);
                    if (candidate <= params->maxLevel && (best == WATER_LEVEL_NONE || candidate < best)) best = candidate;
                }
                if (best != WATER_LEVEL_NONE) {
                    stableLevel = best;
                    fed = true;
                }
            }
        }

        if (!fed) {
            // Unfed non-source cell: evaporate (current water behavior) or
            // keep its level when the fluid declares no evaporation (pooled).
            if (params->evaporation) set_block_at(worldPos, kRuntimeAirId);
            continue;
        }
        if (stableLevel != oldLevel) set_fluid_level_at(worldPos, stableLevel);
        if (cell.y <= 0) continue;

        const FluidCell below{ cell.x, cell.y - 1, cell.z };
        if (params->falling && get_block_at(pos(below)) == kRuntimeAirId) {
            // Dropping into air places the fluid block first (the generic
            // level write only records a level, it does not place blocks).
            set_block_at(pos(below), type);
            set_fluid_level_at(pos(below),
                               static_cast<uint8_t>(WATER_FALLING_FLAG | water_base_level(stableLevel)));
            continue;
        }

        const uint8_t spread = static_cast<uint8_t>(
            water_base_level(stableLevel) + params->levelsPerTick);
        if (spread > params->maxLevel) continue;
        for (const FluidCell& d : horizontal) {
            const FluidCell neighbor{ cell.x + d.x, cell.y, cell.z + d.z };
            if (!is_chunk_loaded_at(pos(neighbor))) continue;
            const RuntimeBlockId neighborType = get_block_at(pos(neighbor));
            const uint8_t neighborLevel = get_fluid_level_at(pos(neighbor));
            if (neighborType == kRuntimeAirId ||
                (neighborType == type && neighborLevel != WATER_SOURCE_LEVEL &&
                 water_base_level(neighborLevel) > spread)) {
                if (neighborType == kRuntimeAirId) set_block_at(pos(neighbor), type);
                set_fluid_level_at(pos(neighbor), spread);
            }
        }
    }
}

void World::update(const glm::vec3& playerPos, WorldRenderBridge& renderBridge, float deltaTime) {
    renderBridge.begin_frame();

    std::deque<ChunkMeshResult> finishedMeshes;
    {
        std::lock_guard<std::mutex> lock(meshResultsMutex);
        finishedMeshes.swap(completedMeshResults);
    }
    for (auto& result : finishedMeshes) apply_mesh_result(std::move(result));
    int playerChunkX = static_cast<int>(std::floor(playerPos.x / CHUNK_SIZE_X));
    int playerChunkZ = static_cast<int>(std::floor(playerPos.z / CHUNK_SIZE_Z));

    // O valor configurado é alcance, não residency. A janela de interação fica
    // limitada a 11x11 chunks densos; o clipmap cobre o restante até o alcance.
    const int denseChunkBudget = std::min(chunkBudget, kDenseLodBudget);
    const int denseRenderDistance = std::max(1, static_cast<int>(std::ceil(
        (std::sqrt(static_cast<float>(denseChunkBudget)) - 1.0f) * 0.5f)));
    std::vector<std::pair<int, int>> desiredChunks;
    desiredChunks.reserve(static_cast<std::size_t>(denseChunkBudget));
    for (int r = 0; r <= denseRenderDistance && desiredChunks.size() < static_cast<std::size_t>(denseChunkBudget); ++r) {
        for (int x = -r; x <= r && desiredChunks.size() < static_cast<std::size_t>(denseChunkBudget); ++x) {
            for (int z = -r; z <= r && desiredChunks.size() < static_cast<std::size_t>(denseChunkBudget); ++z) {
                if (std::max(std::abs(x), std::abs(z)) != r) continue;
                desiredChunks.emplace_back(playerChunkX + x, playerChunkZ + z);
            }
        }
    }
    const std::unordered_set<std::pair<int, int>, ChunkHash> desiredSet(
        desiredChunks.begin(), desiredChunks.end());

    // Submit the complete distant surface before any dense work.  Its own
    // priority worker means a 4096-chunk view cannot wait behind the general
    // chunk FIFO; detailed chunks then replace it progressively as an overlay.
    const bool needsFarLod = chunkBudget > kDenseLodRadius;
    renderBridge.request_far_terrain(playerChunkX, playerChunkZ,
                                     needsFarLod ? chunkBudget : 0,
                                     far_lod_endpoint_fraction());

    // 1. Descarregar chunks fora da janela densa de interação.
    {
        std::lock_guard<std::recursive_mutex> lock(chunksMutex);
        std::unordered_set<std::pair<int, int>, ChunkHash> evictions;
        for (const auto& pair : chunks) {
            if (!desiredSet.contains(pair.first)) evictions.insert(pair.first);
        }

        for (auto it = chunks.begin(); it != chunks.end(); ) {
            if (evictions.contains(it->first)) {
                // O fence do frame atual já foi aguardado antes de World::update.
                // Buffers ainda referenciados pelo outro frame em voo entram na
                // mesma fila de aposentadoria usada por remeshes e só são
                // destruídos após FRAME_OVERLAP épocas, sem parar toda a GPU.
                renderBridge.retire_chunk(ChunkId{ {it->second->chunkX, it->second->chunkZ}, 0 });
                // Work que aguardava este chunk não pode sobreviver ao descarrego:
                // cancela células/random ticks do chunk (nada de trabalho morto).
                scheduler_.cancel_chunk(it->first.first, it->first.second);
                it = chunks.erase(it);
            } else {
                ++it;
            }
        }
    }

    // 2. Simulação de Fluidos no relógio fixo do scheduler (META §9). O
    // scheduler acumula deltaTime e executa ticks de 80ms em fases (block,
    // random, fluid, scheduled), com orçamento por fase e região ativa ao
    // redor do jogador; o callback do tick roda a simulação de fluidos.
    lastFluidPlayerPos_ = playerPos;
    scheduler_.set_active_center(playerChunkX, playerChunkZ);
    scheduler_.set_active_radius(denseRenderDistance + 1);
    scheduler_.advance(deltaTime, kFluidTickSeconds);

    // 2b. Remesh de chunks sujos sai da thread do frame. O snapshot é capturado
    // sob o mutex (estado consistente) e o job só produz o mesh na pool; os
    // resultados voltam por completedMeshResults e são aplicados no topo do
    // próximo update, com a mesma proteção por revision/geração dos chunks
    // novos (um remesh velho é descartado se o chunk foi editado de novo).
    {
        std::lock_guard<std::recursive_mutex> lock(chunksMutex);
        std::vector<std::pair<int, std::shared_ptr<Chunk>>> dirtyChunks;
        for (const auto& pair : chunks) {
            if (!pair.second->isDirty || pair.second->state.load() != ChunkState::Uploaded) continue;
            const int dx = pair.first.first - playerChunkX;
            const int dz = pair.first.second - playerChunkZ;
            dirtyChunks.emplace_back(dx * dx + dz * dz, pair.second);
        }
        std::sort(dirtyChunks.begin(), dirtyChunks.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.first < rhs.first;
        });
        constexpr int maxRemeshesPerFrame = 2;
        const int dispatchCount = std::min<int>(maxRemeshesPerFrame, dirtyChunks.size());
        for (int index = 0; index < dispatchCount; ++index) {
            const auto& chunk = dirtyChunks[static_cast<std::size_t>(index)].second;
            if (pendingTasks.load(std::memory_order_acquire) >= 32) break;
            // O snapshot é a versão consistente do conteúdo; a limpeza do flag
            // antes do job evita re-dispatch, e o gate por revision no apply
            // descarta o resultado se o chunk foi editado no meio do caminho
            // (a edição nova re-suja e despacha outro remesh).
            chunk->isDirty.store(false, std::memory_order_release);
            ChunkSnapshot snap = create_chunk_snapshot(*chunk);
            ++pendingTasks;
            threadPool.enqueue([this, chunk, snap = std::move(snap)]() mutable {
                ChunkMeshResult result = VoxelMesher::build(snap);
                {
                    std::lock_guard<std::mutex> lock(this->meshResultsMutex);
                    this->completedMeshResults.push_back(std::move(result));
                }
                this->pendingTasks--;
            });
        }
    }

    // 3. Agendamento multithreaded de novos chunks
    int tasksDispatched = 0;
    const int maxTasksPerFrame = 16;

    if (pendingTasks.load() < 32) {
        for (const auto& key : desiredChunks) {
            if (tasksDispatched >= maxTasksPerFrame || pendingTasks.load() >= 32) break;

            std::shared_ptr<Chunk> chunkPtr = nullptr;
            {
                std::lock_guard<std::recursive_mutex> lock(chunksMutex);
                if (chunks.size() < static_cast<std::size_t>(denseChunkBudget) &&
                    chunks.find(key) == chunks.end()) {
                    chunkPtr = std::make_shared<Chunk>(key.first, key.second, nextChunkGeneration.fetch_add(1));
                    chunkPtr->state.store(ChunkState::Generating);
                    chunks[key] = chunkPtr;
                }
            }

            if (chunkPtr) {
                pendingTasks++;
                tasksDispatched++;
                // The generator override is captured per dispatch: workers
                // never read the member concurrently, so a register_generator
                // call on the main thread cannot race an in-flight job.
                const std::shared_ptr<engine::voxel::IVoxelGenerator> generator =
                    m_generatorOverride;
                threadPool.enqueue([chunkPtr, this, generator]() {
                    chunkPtr->generate_terrain(this->noise, generator.get());
                    ChunkSnapshot snap = this->create_chunk_snapshot(*chunkPtr);
                    ChunkMeshResult result = VoxelMesher::build(snap);
                    {
                        std::lock_guard<std::mutex> lock(this->meshResultsMutex);
                        this->completedMeshResults.push_back(std::move(result));
                    }
                    this->pendingTasks--;
                });
            }
        }
    }

    // 4. Upload para GPU dos chunks prontos
    int uploadsCount = 0;
    std::vector<std::pair<int, int>> newlyUploaded;
    {
        std::lock_guard<std::recursive_mutex> lock(chunksMutex);
        struct ReadyChunk {
            int distanceSquared;
            std::pair<int, int> key;
            std::shared_ptr<Chunk> chunk;
        };
        std::vector<ReadyChunk> readyChunks;
        readyChunks.reserve(chunks.size());
        for (const auto& pair : chunks) {
            if (pair.second->state.load() != ChunkState::MeshReady) continue;
            const int dx = pair.first.first - playerChunkX;
            const int dz = pair.first.second - playerChunkZ;
            readyChunks.push_back({ dx * dx + dz * dz, pair.first, pair.second });
        }
        std::sort(readyChunks.begin(), readyChunks.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.distanceSquared < rhs.distanceSquared;
        });
        for (const ReadyChunk& ready : readyChunks) {
            if (uploadsCount >= maxGpuUploadsPerFrame) break;

            // Mesh pronto → GPU: o mesh CPU é entregue ao bridge (que cria ou
            // substitui os buffers do renderer) e só então o chunk vira Uploaded.
            ChunkMeshResult upload;
            upload.chunk = ready.chunk->id();
            upload.sourceRevision = ready.chunk->revision();
            upload.verticalExtent = ready.chunk->vertical_render_extent();
            upload.valid = true;
            upload.mesh = static_cast<const ChunkMeshData&>(*ready.chunk);
            renderBridge.upload_chunk(std::move(upload));
            ready.chunk->state.store(ChunkState::Uploaded, std::memory_order_release);
            newlyUploaded.push_back(ready.key);
            ++uploadsCount;

            // Vizinhos cujo mesh foi construído antes do conteúdo atual deste
            // chunk ficam sujos (borda do halo). O gate por revision evita o
            // ping-pong: um remesh sem mudança de conteúdo não re-suja ninguém.
            const int cx = ready.key.first;
            const int cz = ready.key.second;
            const std::pair<int, int> offsets[4] = {
                { cx - 1, cz }, { cx + 1, cz }, { cx, cz - 1 }, { cx, cz + 1 }
            };
            for (const auto& key : offsets) {
                auto neighbor = chunks.find(key);
                if (neighbor == chunks.end() || !neighbor->second ||
                    neighbor->second->state.load() != ChunkState::Uploaded) continue;
                uint64_t seen = 0;
                for (const NeighborSeen& entry : ready.chunk->meshNeighborSeen) {
                    if (entry.coord.x == key.first && entry.coord.z == key.second) {
                        seen = entry.revision;
                        break;
                    }
                }
                if (seen < ready.chunk->revision()) {
                    neighbor->second->isDirty.store(true, std::memory_order_release);
                }
            }
        }
    }

    // Só publica anéis completos. Um chunk distante que termina antes de um
    // vizinho pesado (floresta/grama) permanece preparado, mas invisível, em vez
    // de parecer uma ilha com água, árvores e vegetação flutuando no vazio.
    {
        std::lock_guard<std::recursive_mutex> lock(chunksMutex);
        visibleCenterChunkX = playerChunkX;
        visibleCenterChunkZ = playerChunkZ;
        stableVisibleRadius = -1;
        std::size_t cursor = 0;
        for (int radius = 0; radius <= denseRenderDistance && cursor < desiredChunks.size(); ++radius) {
            bool ringComplete = true;
            std::size_t ringChunkCount = 0;
            while (cursor < desiredChunks.size()) {
                const auto& key = desiredChunks[cursor];
                const int keyRadius = std::max(std::abs(key.first - playerChunkX),
                                              std::abs(key.second - playerChunkZ));
                if (keyRadius != radius) break;
                ++ringChunkCount;
                const auto found = chunks.find(key);
                if (found == chunks.end() || !found->second ||
                    found->second->state.load() != ChunkState::Uploaded) {
                    ringComplete = false;
                }
                ++cursor;
            }

            // Um anel de Chebyshev tem 1 chunk no centro e 8*r chunks nos
            // demais raios. O orçamento geralmente termina no meio do último
            // anel (1024 = 961 + somente 63 dos 128 chunks do raio 16). Antes,
            // esse fragmento era publicado como se fosse um anel completo e
            // aparecia no céu como faixas soltas de água/folhagem. Residency
            // parcial continua permitida para pré-carregamento, visibilidade não.
            const std::size_t expectedRingChunkCount = radius == 0
                ? 1u : static_cast<std::size_t>(8 * radius);
            if (ringChunkCount != expectedRingChunkCount || !ringComplete) break;
            stableVisibleRadius = radius;
        }
    }

    // Block entities: chunks that just uploaded reconcile their entities — a
    // stale entity (block empty after the world changed since save) is pruned
    // and survivors get their tick cell (re)scheduled after eviction cancelled
    // it. Loaded chunks always keep their entities ticking.
    for (const auto& key : newlyUploaded) {
        reconcile_block_entities(key.first, key.second);
    }

    // New chunks need their first light pass (sky + emitters).
    for (const auto& key : newlyUploaded) {
        mark_chunk_light_dirty(key.first, key.second);
    }

    // Conteúdo exclusivo da 1.1, aplicado sobre o terreno/árvores/grama da 1.0.
    for (const auto& key : newlyUploaded) {
        if (!mobSpawningEnabled) continue;
        if (!mobPopulatedChunks.insert(key).second) continue;
        const uint32_t hash = static_cast<uint32_t>(key.first * 92837111u) ^
                              static_cast<uint32_t>(key.second * 689287499u);
        if (hash % 23u != 0u) continue;
        const int wx = key.first * CHUNK_SIZE_X + 8;
        const int wz = key.second * CHUNK_SIZE_Z + 8;
        int surfaceY = -1;
        for (int y = GENERATED_TERRAIN_HEIGHT - 3; y >= 1; --y) {
            if (is_solid_block_id(get_block_at(glm::vec3(wx, y, wz))) &&
                get_block_at(glm::vec3(wx, y + 1, wz)) == kRuntimeAirId) {
                surfaceY = y + 1;
                break;
            }
        }
        if (surfaceY > 0) {
            const MobType type = static_cast<MobType>((hash / 23u) % 6u);
            mobManager_.spawn_mob(type, glm::vec3(wx + 0.5f, surfaceY, wz + 0.5f));
        }
    }

    struct StructureSpawn { int cx, cz, y; StructureType type; bool enabled; };
    static const std::array<StructureSpawn, 5> structureSpawns = [] {
        auto find_site = [](int anchorX, int anchorZ, StructureType type) {
            auto biome_score = [type](const TerrainSample& terrain) -> float {
                switch (type) {
                case StructureType::SunTemple:
                    if (terrain.biome == BiomeType::Desert) return 3.0f;
                    if (terrain.biome == BiomeType::Badlands) return 2.4f;
                    if (terrain.biome == BiomeType::Savanna) return 1.5f;
                    break;
                case StructureType::FloatingWizardCitadel:
                    if (terrain.biome == BiomeType::Yosemite) return 3.2f;
                    if (terrain.biome == BiomeType::Highlands) return 2.8f;
                    if (terrain.biome == BiomeType::Alpine || terrain.biome == BiomeType::RockyMountains) return 2.2f;
                    break;
                case StructureType::CrystalLavaDungeon:
                    if (terrain.biome == BiomeType::VolcanicCrater) return 3.5f;
                    if (terrain.biome == BiomeType::Volcanic) return 3.0f;
                    if (terrain.biome == BiomeType::RockyMountains || terrain.biome == BiomeType::Highlands) return 1.7f;
                    break;
                case StructureType::OvergrownRuins:
                    if (terrain.biome == BiomeType::Jungle) return 3.4f;
                    if (terrain.biome == BiomeType::Swamp) return 3.0f;
                    if (terrain.biome == BiomeType::Forest || terrain.biome == BiomeType::BirchTaiga) return 2.3f;
                    break;
                case StructureType::UnderwaterAtlantis:
                    if (terrain.biome == BiomeType::DeepOcean) return 3.5f;
                    if (terrain.biome == BiomeType::Ocean && terrain.height < TerrainGenerator::SeaLevel - 7) return 2.5f;
                    break;
                }
                return -1000.0f;
            };

            int chosenX = anchorX;
            int chosenZ = anchorZ;
            TerrainSample chosen = TerrainGenerator::sample(anchorX * CHUNK_SIZE_X + 8.0f,
                                                             anchorZ * CHUNK_SIZE_Z + 8.0f);
            float bestScore = -1000.0f;
            for (int radius = 0; radius <= 18; ++radius) {
                for (int dz = -radius; dz <= radius; ++dz) {
                    for (int dx = -radius; dx <= radius; ++dx) {
                        if (std::max(std::abs(dx), std::abs(dz)) != radius) continue;
                        const int cx = anchorX + dx;
                        const int cz = anchorZ + dz;
                        const float wx = cx * CHUNK_SIZE_X + 8.0f;
                        const float wz = cz * CHUNK_SIZE_Z + 8.0f;
                        const TerrainSample terrain = TerrainGenerator::sample(wx, wz);
                        float score = biome_score(terrain);
                        if (score < 0.0f) continue;

                        int minHeight = terrain.height;
                        int maxHeight = terrain.height;
                        constexpr float footprint = 6.0f;
                        const TerrainSample corners[] = {
                            TerrainGenerator::sample(wx - footprint, wz - footprint),
                            TerrainGenerator::sample(wx + footprint, wz - footprint),
                            TerrainGenerator::sample(wx - footprint, wz + footprint),
                            TerrainGenerator::sample(wx + footprint, wz + footprint)
                        };
                        for (const TerrainSample& corner : corners) {
                            minHeight = std::min(minHeight, corner.height);
                            maxHeight = std::max(maxHeight, corner.height);
                        }
                        const int relief = maxHeight - minHeight;
                        const int allowedRelief = type == StructureType::FloatingWizardCitadel ? 12 :
                                                  type == StructureType::CrystalLavaDungeon ? 15 : 5;
                        if (relief > allowedRelief) continue;
                        score -= static_cast<float>(relief) * 0.13f;
                        score -= static_cast<float>(radius) * 0.018f;
                        if (score <= bestScore) continue;
                        bestScore = score;
                        chosenX = cx;
                        chosenZ = cz;
                        chosen = terrain;
                    }
                }
            }

            int y = chosen.height + 1;
            if (type == StructureType::CrystalLavaDungeon) y = std::max(6, chosen.height - 18);
            if (type == StructureType::UnderwaterAtlantis) y = chosen.height + 1;
            y = std::clamp(y, 5, GENERATED_TERRAIN_HEIGHT - 10);
            return StructureSpawn{chosenX, chosenZ, y, type, bestScore >= 0.0f};
        };

        return std::array<StructureSpawn, 5>{
            find_site( 2,  2, StructureType::SunTemple),
            find_site(-3,  3, StructureType::FloatingWizardCitadel),
            find_site( 4, -2, StructureType::CrystalLavaDungeon),
            find_site(-2, -4, StructureType::OvergrownRuins),
            find_site( 0,  5, StructureType::UnderwaterAtlantis)
        };
    }();
    for (const StructureSpawn& spawn : structureSpawns) {
        if (!spawn.enabled || !structureSpawningEnabled) continue;
        const std::pair<int, int> key{spawn.cx, spawn.cz};
        if (structurePopulatedChunks.contains(key)) continue;
        bool neighborhoodReady = true;
        {
            std::lock_guard<std::recursive_mutex> lock(chunksMutex);
            for (int dz = -1; dz <= 1 && neighborhoodReady; ++dz) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const auto found = chunks.find({spawn.cx + dx, spawn.cz + dz});
                    if (found == chunks.end() || found->second->state.load() != ChunkState::Uploaded) {
                        neighborhoodReady = false;
                        break;
                    }
                }
            }
        }
        if (!neighborhoodReady) continue;
        structurePopulatedChunks.insert(key);
        Structures::generate_structure(spawn.type, spawn.cx * CHUNK_SIZE_X, spawn.y,
                                       spawn.cz * CHUNK_SIZE_Z,
            [this](int x, int y, int z, BlockType type) {
                if (y >= 0 && y < CHUNK_SIZE_Y) set_block_at(glm::vec3(x, y, z), runtime_id(type));
            });
    }

    // Discrete light relight pass (META section 12): budgeted, sorted by
    // distance; a chunk whose light changed re-dirties its neighbors so light
    // crosses borders and converges chunk by chunk.
    run_light_pass(playerPos);

    mobManager_.update(deltaTime, playerPos, *this);
}

void World::mark_chunk_light_dirty(int cx, int cz) {
    lightDirtyChunks_.insert({ cx, cz });
}

uint8_t World::light_emission(RuntimeBlockId id) const {
    if (is_builtin_block(id)) {
        switch (as_builtin_block(id)) {
        case BlockType::Lava:
        case BlockType::Glowstone:
        case BlockType::SeaLantern:
            return 15;
        case BlockType::MagmaBlock:
            return 12;
        default:
            return 0;
        }
    }
    const auto found = runtimeBlocks_.find(id);
    return found == runtimeBlocks_.end() ? 0 : found->second.lightEmission;
}

uint8_t World::light_absorption(RuntimeBlockId id) const {
    if (is_builtin_block(id)) {
        const BlockType type = as_builtin_block(id);
        if (type == BlockType::Air) return 0;
        if (type == BlockType::Water || type == BlockType::Lava) return 1;
        if (is_leaf_block(type)) return 1;
        if (is_transparent_block(type)) return 0;  // glass passes light
        return 15;  // opaque blocks absorb fully
    }
    const auto found = runtimeBlocks_.find(id);
    return found == runtimeBlocks_.end() ? 15 : found->second.lightAbsorption;
}

uint8_t World::get_sky_light(const glm::vec3& worldPos) const {
    const int cx = static_cast<int>(std::floor(worldPos.x / CHUNK_SIZE_X));
    const int cz = static_cast<int>(std::floor(worldPos.z / CHUNK_SIZE_Z));
    std::lock_guard<std::recursive_mutex> lock(chunksMutex);
    const auto found = chunks.find({ cx, cz });
    if (found == chunks.end() || !found->second) return 0;
    return found->second->get_sky_light(
        static_cast<int>(std::floor(worldPos.x)) - cx * CHUNK_SIZE_X,
        static_cast<int>(std::floor(worldPos.y)),
        static_cast<int>(std::floor(worldPos.z)) - cz * CHUNK_SIZE_Z);
}

uint8_t World::get_block_light(const glm::vec3& worldPos) const {
    const int cx = static_cast<int>(std::floor(worldPos.x / CHUNK_SIZE_X));
    const int cz = static_cast<int>(std::floor(worldPos.z / CHUNK_SIZE_Z));
    std::lock_guard<std::recursive_mutex> lock(chunksMutex);
    const auto found = chunks.find({ cx, cz });
    if (found == chunks.end() || !found->second) return 0;
    return found->second->get_block_light(
        static_cast<int>(std::floor(worldPos.x)) - cx * CHUNK_SIZE_X,
        static_cast<int>(std::floor(worldPos.y)),
        static_cast<int>(std::floor(worldPos.z)) - cz * CHUNK_SIZE_Z);
}

void World::run_light_pass(const glm::vec3& playerPos) {
    if (lightDirtyChunks_.empty()) return;
    const int playerChunkX = static_cast<int>(std::floor(playerPos.x / CHUNK_SIZE_X));
    const int playerChunkZ = static_cast<int>(std::floor(playerPos.z / CHUNK_SIZE_Z));
    std::vector<std::pair<int, int>> dirty(lightDirtyChunks_.begin(), lightDirtyChunks_.end());
    std::sort(dirty.begin(), dirty.end(), [&](const auto& a, const auto& b) {
        const int da = std::max(std::abs(a.first - playerChunkX), std::abs(a.second - playerChunkZ));
        const int db = std::max(std::abs(b.first - playerChunkX), std::abs(b.second - playerChunkZ));
        if (da != db) return da < db;
        return std::tie(a.first, a.second) < std::tie(b.first, b.second);
    });

    int processed = 0;
    for (const auto& key : dirty) {
        if (processed >= kMaxLightChunksPerFrame) break;
        std::shared_ptr<Chunk> chunk;
        std::shared_ptr<Chunk> neighbors[4];
        {
            std::lock_guard<std::recursive_mutex> lock(chunksMutex);
            const auto found = chunks.find(key);
            if (found == chunks.end() || !found->second) {
                lightDirtyChunks_.erase(key);
                continue;
            }
            if (found->second->state.load(std::memory_order_acquire) != ChunkState::Uploaded) {
                continue;  // not generated yet; stays dirty until upload
            }
            chunk = found->second;
            const std::pair<int, int> offsets[4] = {
                { key.first - 1, key.second }, { key.first + 1, key.second },
                { key.first, key.second - 1 }, { key.first, key.second + 1 } };
            for (int i = 0; i < 4; ++i) {
                const auto neighbor = chunks.find(offsets[i]);
                if (neighbor != chunks.end() && neighbor->second) neighbors[i] = neighbor->second;
            }
        }

        // Lock-free lookups: the pass runs on the frame thread; the captured
        // chunk pointers are stable and workers never mutate them mid-frame.
        const int baseX = key.first * CHUNK_SIZE_X;
        const int baseZ = key.second * CHUNK_SIZE_Z;
        ChunkLightAccess access;
        access.blockAt = [chunk, neighbors, key, baseX, baseZ](int wx, int wy, int wz) {
            const int cx = static_cast<int>(std::floor(static_cast<float>(wx) / CHUNK_SIZE_X));
            const int cz = static_cast<int>(std::floor(static_cast<float>(wz) / CHUNK_SIZE_Z));
            const Chunk* target = nullptr;
            if (cx == key.first && cz == key.second) target = chunk.get();
            else {
                for (const auto& n : neighbors) {
                    if (n && n->chunkX == cx && n->chunkZ == cz) { target = n.get(); break; }
                }
            }
            if (!target) return kRuntimeAirId;
            return target->get_block(wx - cx * CHUNK_SIZE_X, wy, wz - cz * CHUNK_SIZE_Z);
        };
        access.blockLightAt = [chunk, neighbors, key](int wx, int wy, int wz) {
            const int cx = static_cast<int>(std::floor(static_cast<float>(wx) / CHUNK_SIZE_X));
            const int cz = static_cast<int>(std::floor(static_cast<float>(wz) / CHUNK_SIZE_Z));
            const Chunk* target = nullptr;
            if (cx == key.first && cz == key.second) target = chunk.get();
            else {
                for (const auto& n : neighbors) {
                    if (n && n->chunkX == cx && n->chunkZ == cz) { target = n.get(); break; }
                }
            }
            if (!target) return static_cast<uint8_t>(0);
            return target->get_block_light(wx - cx * CHUNK_SIZE_X, wy, wz - cz * CHUNK_SIZE_Z);
        };
        access.emission = [this](RuntimeBlockId id) { return light_emission(id); };
        access.absorption = [this](RuntimeBlockId id) { return light_absorption(id); };

        const bool changed = ChunkLighting::compute(*chunk, access);
        lightDirtyChunks_.erase(key);
        // Light changed: neighbors must re-see it (cross-border propagation).
        if (changed) {
            for (const auto& n : neighbors) {
                if (n) mark_chunk_light_dirty(n->chunkX, n->chunkZ);
            }
        }
        ++processed;
    }
}

void World::cleanup() {
    // Engine::cleanup runs before World member destruction. Explicitly drain
    // both pools while every World/Chunk object and the Vulkan device still
    // exist; relying on member destructors here used to permit use-after-free.
    threadPool.wait_idle();
    std::lock_guard<std::recursive_mutex> lock(chunksMutex);
    chunks.clear();
}
