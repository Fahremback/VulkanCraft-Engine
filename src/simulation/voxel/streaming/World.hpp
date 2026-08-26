#pragma once

#include "Chunk.hpp"
#include "FastNoiseLite.hpp"
#include "ThreadPool.hpp"
#include "WorldRenderBridge.hpp"
#include "ChunkSnapshot.hpp"
#include "ChunkMeshResult.hpp"
#include "WorldScheduler.hpp"
#include "engine/voxel/IVoxelWorld.hpp"
#include "engine/voxel/IVoxelBlockEntity.hpp"
#include <unordered_map>
#include <memory>
#include <optional>
#include <tuple>
#include <mutex>
#include <atomic>
#include <deque>
#include <unordered_set>

struct FluidCell {
    int x, y, z;
    bool operator==(const FluidCell&) const = default;
};

// Runtime simulation parameters for one fluid (META section 13), derived from
// the project's FluidDefinition (or the engine defaults for water/lava) when
// the facade builds the world's fluid table. Pure data: the world never
// touches the registry.
struct FluidParams {
    float viscosity{ 0.5f };   // 0..1 (rheology hint; thin = faster level step)
    float density{ 1.0f };
    // Spread budget (levels, 1..7). A cell only spreads while its next level
    // stays within the budget; 7 = the engine's level space.
    int maxLevel{ 7 };
    // Levels gained per simulation step (viscosity < 0.75 -> 2, else 1).
    int levelsPerTick{ 1 };
    // Steps between simulation runs for this fluid (1 = every fluid tick).
    int tickEveryTicks{ 1 };
    bool source{ true };
    bool falling{ true };
    bool evaporation{ true };
    float damagePerTick{ 0.0f };
    glm::vec4 color{ 0.30f, 0.60f, 1.00f, 0.65f };
    bool compressible{ false };
    // Temperature / solidification / combustion (task D.3): declared heat
    // axis; the runtime id an unfed flowing cell solidifies into (0 = none);
    // whether the fluid ignites adjacent flammable blocks.
    float temperature{ 300.0f };
    RuntimeBlockId solidifiesInto{ kRuntimeAirId };
    bool ignites{ false };
};

struct FluidCellHash {
    std::size_t operator()(const FluidCell& cell) const {
        std::size_t h = std::hash<int>{}(cell.x);
        h ^= std::hash<int>{}(cell.y) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>{}(cell.z) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

struct ChunkHash {
    std::size_t operator()(const std::pair<int, int>& p) const {
        return std::hash<int>()(p.first) ^ (std::hash<int>()(p.second) << 16);
    }
};

class World {
    friend class WorldRenderer;
public:
    static constexpr int MIN_CHUNK_BUDGET = 8;
    static constexpr int MAX_CHUNK_BUDGET = 4096;
    // Fixed cadence of the world's fluid simulation (the scheduler's FluidTick
    // phase step). The SDK derives per-fluid tickEveryTicks from this.
    static constexpr float kFluidTickSeconds = 0.08f;
    // Lighting plugin overrides (FALTANTES §3 item 2): per-block emission/
    // absorption pushed by the facade from a registered IVoxelLighting plugin;
    // consulted BEFORE the builtin tables by the light pass.
    struct LightOverride {
        uint8_t emission{ 0 };
        uint8_t absorption{ 15 };
    };

    // Alcance visual em chunks. Apenas a janela de interação próxima vira
    // chunks/voxels completos; o restante é um clipmap de superfície.
    int chunkBudget{ 4096 };
    uint64_t memoryBudget{ 0 };  // bytes; 0 = unlimited (FALTANTES §4 item 6)
    int renderDistance{ 4096 };
    int maxGpuUploadsPerFrame{ 8 };
    // Terrain population (structures) can be disabled for deterministic
    // headless runs (tests): structures never touch the world in that mode.
    bool structureSpawningEnabled{ true };
    // Qualidade relativa no limite do alcance. O restante da curva e derivado
    // continuamente por Q(d) = pow(Qfinal, d / reach). O valor e ajustavel em
    // passos logaritmicos finos entre 0.001% e 99%.
    double farLodEndpointPercentValue{ 0.1 };
    FastNoiseLite noise;

    std::atomic<int> pendingTasks{ 0 };
    std::atomic<uint32_t> nextChunkGeneration{1};

    std::unordered_map<std::pair<int, int>, std::shared_ptr<Chunk>, ChunkHash> chunks;
    std::deque<FluidCell> activeFluidCells;
    std::unordered_set<FluidCell, FluidCellHash> activeFluidSet;
    std::unordered_set<std::pair<int, int>, ChunkHash> structurePopulatedChunks;
    int visibleCenterChunkX{ 0 };
    int visibleCenterChunkZ{ 0 };
    int stableVisibleRadius{ -1 };
    mutable std::recursive_mutex chunksMutex;
    mutable std::mutex meshResultsMutex;
    std::deque<ChunkMeshResult> completedMeshResults;

    // The pool is the LAST member so its destructor (and thus the worker
    // join) runs FIRST during World destruction. Workers capture `this` and
    // may touch chunks/chunksMutex while a task is in flight; destroying that
    // state before the pool drains is use-after-free (a defaulted destructor
    // with the pool declared earlier used to tear down the mutex/map while a
    // worker was still running, which could spin forever on destroyed memory).
    ThreadPool threadPool;

    // Fixed-tick scheduler (META section 9): owns the world's simulation
    // clock. The fluid simulation runs at the fixed cadence through its tick
    // callback; the FluidTick phase feeds the cell queue; block/random/
    // scheduled work is queued by the project. Advanced on the frame thread
    // inside update(); all scheduler state is single-threaded by design.
    WorldScheduler scheduler_;

    // Runs one fluid step immediately (input shortcut: place/break water now
    // instead of waiting for the next fixed tick).
    void force_fluid_tick();

    // Discrete world lighting (META section 12). Queries are O(1)-ish: sky
    // from the column occlusion height, block light from the sparse map.
    // Values settle asynchronously (budgeted relight pass inside update()).
    uint8_t get_sky_light(const glm::vec3& worldPos) const;
    uint8_t get_block_light(const glm::vec3& worldPos) const;

    // Direct access for engine tools/tests to configure phases, budgets and
    // the active region before update() drives the clock.
    WorldScheduler& scheduler() { return scheduler_; }
    const WorldScheduler& scheduler() const { return scheduler_; }

    // ---- Block entities (META section 8) ----
    // Voxels with project-owned state. The engine stores them keyed by world
    // block position, ticks them deterministically through the scheduler's
    // BlockTick phase (dedup, budget, active regions, sleeping), destroys them
    // atomically when the block is removed, and frames their state for the
    // world save. All state is frame-thread only (same thread as update()).
    void register_block_entity_type(const std::string& typeId,
                                    engine::voxel::BlockEntityFactory factory);
    bool attach_block_entity(int x, int y, int z,
                             std::shared_ptr<engine::voxel::IVoxelBlockEntity> entity,
                             std::string& errorOut);
    [[nodiscard]] std::shared_ptr<engine::voxel::IVoxelBlockEntity>
    block_entity_at(int x, int y, int z) const;
    bool remove_block_entity(int x, int y, int z);
    void set_block_entity_listener(
        std::function<void(const engine::voxel::BlockEntityEvent&)> listener);
    [[nodiscard]] engine::voxel::BlockEntityFactory
    find_block_entity_factory(const std::string& typeId) const;
    // Load path: restores an entity without block validation (chunks are not
    // loaded yet) and schedules its tick; a consistency pass prunes entities
    // whose block turned out empty once the chunk uploads.
    void restore_block_entity(int x, int y, int z,
                              std::shared_ptr<engine::voxel::IVoxelBlockEntity> entity);
    using BlockEntityMap = std::unordered_map<
        TickCell, std::shared_ptr<engine::voxel::IVoxelBlockEntity>, TickCellHash>;
    [[nodiscard]] const BlockEntityMap& block_entities() const {
        return blockEntities_;
    }

    // Public SDK generator override (null = builtin TerrainGenerator).
    std::shared_ptr<engine::voxel::IVoxelGenerator> m_generatorOverride;

    // Replaces the builtin terrain generator for all future chunk generation.
    // The override is captured per dispatch, so workers never race the pointer.
    void set_generator_override(std::shared_ptr<engine::voxel::IVoxelGenerator> generator);

    // Registry-driven block identity (Prioridade 0 item 1): the SDK facade
    // derives the dynamic block table from the public registry (UUID-sorted
    // allocation — ids never depend on JSON load order) and pushes it here as
    // plain data, keeping the simulation decoupled from the registry types.
    // Must be called before chunks are generated.
    void set_runtime_block_table(
        std::unordered_map<RuntimeBlockId, RuntimeBlockInfo> table,
        std::unordered_map<std::string, RuntimeBlockId> uuidToId);

    // True when `id` is a builtin id or a registered dynamic block.
    [[nodiscard]] bool is_valid_block_id(RuntimeBlockId id) const;
    // Solid/collidable query used by raycasts, AI and spawning. Builtin ids
    // resolve through the engine material table; dynamic ids through the
    // registry-derived table.
    [[nodiscard]] bool is_solid_block_id(RuntimeBlockId id) const;
    // Dynamic runtime id for a persistent UUID (only for registered blocks).
    [[nodiscard]] std::optional<RuntimeBlockId> runtime_block_id_for_uuid(const std::string& uuid) const;
    // Dynamic runtime table (id >= BlockType::Count). Sorted by id for
    // deterministic serialization.
    [[nodiscard]] std::vector<std::pair<RuntimeBlockId, RuntimeBlockInfo>> runtime_block_table() const;

    // workerThreads = 0 keeps the default (hardware concurrency); tests and
    // headless servers can pass a small fixed count for deterministic runs.
    explicit World(size_t workerThreads = 0);

private:
    // Resolved worker pool size (0 input -> hardware concurrency), exposed in
    // the streaming snapshot.
    size_t workerThreads_{ 0 };
public:
    ~World();

    void set_chunk_budget(int budget);
    // RAM-budgeted chunk cache (FALTANTES §4 item 6): 0 = unlimited. See the
    // public contract on IVoxelWorld for the eviction policy (farthest
    // non-dirty chunks while over budget).
    void set_memory_budget(uint64_t bytes);
    // Clears the "has unsaved edits" pin on the given chunks after a save
    // persisted them. A revision is per chunk: the flag is cleared ONLY when
    // the chunk's current revision still matches the one that was just saved
    // (an edit made mid-save bumps the revision and keeps the flag, so the
    // next save persists it).
    void mark_chunks_saved(const std::vector<std::pair<std::pair<int, int>,
                            uint64_t>>& savedChunks);
    void cycle_chunk_budget(int direction);
    void adjust_far_lod_quality(int direction, int steps = 1);
    [[nodiscard]] float far_lod_endpoint_fraction() const;
    [[nodiscard]] float far_lod_endpoint_percent() const;

    // Streaming/budget observability (FALTANTES §3): snapshot of the streaming
    // state — chunk state census under the chunk mutex, budgets and worker/
    // entity/fluid counts. Best-effort live measurements for the public
    // engine::voxel::StreamingSnapshot contract.
    [[nodiscard]] engine::voxel::StreamingSnapshot streaming_snapshot() const;

    // Render handoff (task C.3): whether the chunk at (cx, cz) is awaiting a
    // relight (present in lightDirtyChunks_). False for an unknown chunk.
    bool is_light_dirty(int cx, int cz) const;

    void update(const glm::vec3& playerPos, WorldRenderBridge& renderBridge, float deltaTime);
    void cleanup();

    // Runtime block ids (builtin prefix + dynamic registry blocks).
    RuntimeBlockId get_block_at(const glm::vec3& worldPos) const;
    // Per-voxel block state index (FALTANTES item 2): 0 = default; >0 = named.
    uint8_t get_state_at(const glm::vec3& worldPos) const;
    void set_state_at(const glm::vec3& worldPos, uint8_t stateIndex);
    uint8_t get_water_level_at(const glm::vec3& worldPos) const;
    void set_block_at(const glm::vec3& worldPos, RuntimeBlockId type);
    void set_water_at(const glm::vec3& worldPos, uint8_t level);
    bool is_chunk_loaded_at(const glm::vec3& worldPos) const;
    // Chunk present and not generating/unloaded — safe for fluid reads AND
    // writes (mirrors set_block_at's guard; Meshing/Uploaded states are fine).
    bool can_touch_chunk_at(const glm::vec3& worldPos) const;

    // Persistence restore support (FALTANTES §4 item 1): while `restoring` is
    // true the eviction pass of update() is suppressed, so chunks written by a
    // load survive the load's own update() calls (each restore may wait out an
    // in-flight generator, and that wait advances the pipeline via update()).
    void set_restoring(bool active);
    // Makes chunk (cx,cz) writable for a restore — creates it in the world map
    // (or takes over an existing one, waiting out an in-flight generator via
    // update()) and marks it Uploaded + dirty so the next remesh pass rebuilds
    // its mesh from the restored data. Returns false only if the generator
    // never finishes (bounded wait). The wait advances the pipeline through
    // update(), so it needs the player position and render bridge like update.
    bool ensure_chunk_restored(int cx, int cz, const glm::vec3& playerPos,
                               WorldRenderBridge& renderBridge);
    // FALTANTES §4 item 18: restores a chunk's voxel data in ONE batch write
    // (no per-voxel set_block_at machinery), with a single invalidation — one
    // dataVersion bump, one dirty flag, one light-dirty pass (chunk + 4
    // neighbors). Creating/take-over semantics match ensure_chunk_restored
    // (absent chunk created directly, in-flight generator waited out via
    // update()); restored content is the persisted state, so hasUnsavedEdits
    // is cleared, never set. `extent` is the vertical count of layers to
    // restore (rows in [0, extent)); blocks/water/stateIndices are laid out y-major,
    // layerBytes per layer (16*16). stateIndices may be nullptr (legacy saves).
    bool restore_chunk_data(int cx, int cz, int extent,
                            const RuntimeBlockId* blocks, const uint8_t* water,
                            const uint8_t* stateIndices,
                            const glm::vec3& playerPos,
                            WorldRenderBridge& renderBridge);

    // Data-driven fluids (META section 13): the facade derives the fluid
    // table from the project's FluidRegistry (plus water/lava defaults) and
    // pushes it here as plain data — the simulation never touches the
    // registry. Must be set before fluids are simulated.
    void set_fluid_table(std::unordered_map<RuntimeBlockId, FluidParams> table);
    // Lighting plugin substitution (FALTANTES §3 item 2): per-block emission/
    // absorption overrides pushed from a registered IVoxelLighting plugin;
    // consulted before the builtin tables by the light pass. Empty map
    // restores the builtin behavior; queued chunks for an immediate relight.
    void set_light_table_overrides(
        std::unordered_map<RuntimeBlockId, LightOverride> overrides);
    // Fluid parameters for a runtime id; nullptr when the block is not a fluid.
    [[nodiscard]] const FluidParams* fluid_params_for_id(RuntimeBlockId id) const;
    [[nodiscard]] bool is_fluid_runtime_id(RuntimeBlockId id) const;
    // True when the block at the position is a fluid (swimming, spawning,
    // gameplay queries — replaces the rigid `== Water` checks).
    [[nodiscard]] bool is_fluid_block_at(const glm::vec3& worldPos) const;
    // Fluid parameters of the block at a world position (nullptr when empty
    // or not a fluid). Used by entities for per-fluid effects (damage).
    [[nodiscard]] const FluidParams* fluid_params_at(const glm::ivec3& worldPos) const;
    // Generic fluid-level byte (any fluid; water-guarded reads keep the
    // historical water semantics for game consumers).
    uint8_t get_fluid_level_at(const glm::vec3& worldPos) const;
    void set_fluid_level_at(const glm::vec3& worldPos, uint8_t level);

private:
    // Player position seen by the scheduler's tick callback (fluid runs with
    // the latest known focus; the callback itself is registered once).
    glm::vec3 lastFluidPlayerPos_{ 0.0f, 0.0f, 0.0f };

    // Chunks whose discrete light is stale (edits, water changes, uploads and
    // changed neighbors). Processed by the budgeted light pass in update().
    std::unordered_set<std::pair<int, int>, ChunkHash> lightDirtyChunks_;
    // C.1: dataVersion of each chunk at its last light compute. When a chunk
    // is re-dirtied with an UNCHANGED dataVersion (neighbor-convergence
    // re-dirty — the common case), its content is provably identical, so the
    // light pass skips the 16x16x256-column sky-occlusion rescan (bit-identical
    // result by construction; only block-light re-runs for halo inflow).
    // Erased on eviction so a re-loaded chunk with a fresh small dataVersion
    // never false-matches a stale entry.
    std::unordered_map<std::pair<int, int>, uint64_t, ChunkHash>
        lightContentRevision_;
    // Restore mode (set_restoring): suppresses chunk eviction during loads.
    bool restoring_{ false };
    void run_light_pass(const glm::vec3& playerPos);
    // Emission/absorption tables (builtin + registry-derived dynamic blocks).
    uint8_t light_emission(RuntimeBlockId id) const;
    uint8_t light_absorption(RuntimeBlockId id) const;
    // Flammability of a runtime block id (0 = not flammable). Builtins declare
    // none; dynamic blocks expose BlockDefinition.flammability. Consumed by
    // the fluid combustion rule (task D.3).
    float flammability(RuntimeBlockId id) const;
    void mark_chunk_light_dirty(int cx, int cz);

    // Block entities (META section 8): position -> entity; factories by type
    // id (reconstruction on load); optional lifecycle listener.
    BlockEntityMap blockEntities_;
    std::unordered_map<std::string, engine::voxel::BlockEntityFactory>
        blockEntityFactories_;
    std::function<void(const engine::voxel::BlockEntityEvent&)>
        blockEntityListener_;

    // Erases the entity, cancels its tick cell and fires the Detached event.
    void destroy_block_entity_at(const TickCell& key, bool notify);
    // After a chunk uploads: drops entities whose block is empty (stale save)
    // and (re)schedules the survivors' tick cells (eviction cancelled them).
    void reconcile_block_entities(int chunkX, int chunkZ);

    // Dynamic block table (id >= Count) pushed by the SDK facade: identity
    // (UUID) + material/behavior snapshot for mesh workers.
    std::unordered_map<RuntimeBlockId, RuntimeBlockInfo> runtimeBlocks_;
    std::unordered_map<std::string, RuntimeBlockId> runtimeUuidToId_;

    bool inside_stable_frontier(const std::pair<int, int>& key) const;
    void enqueue_fluid_neighborhood(const glm::vec3& worldPos);
    void update_fluid_physics(const glm::vec3& playerPos);
    [[nodiscard]] ChunkSnapshot create_chunk_snapshot(const Chunk& chunk) const;
    bool apply_mesh_result(ChunkMeshResult result);

    // Fluid table (runtime id -> parameters), pushed by the SDK facade.
    std::unordered_map<RuntimeBlockId, FluidParams> fluidTable_;

    std::unordered_map<RuntimeBlockId, LightOverride> lightOverrides_;
};
