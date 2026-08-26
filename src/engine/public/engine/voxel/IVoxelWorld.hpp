#pragma once

// Public voxel world contract (SDK). The voxel subsystem is a public engine
// module: no renderer details (Vulkan/VMA) appear here, meshing/upload/
// remeshing are internal reactions to domain events, and the world runs
// headless on servers and in tests.
//
// Block ids are stable uint32 runtime ids. The block registry (see
// engine/registry/BlockRegistry.hpp) maps persistent UUIDs to these ids;
// ids never depend on load order.

#include "engine/entity/IEntityWorld.hpp"
#include "engine/entity/IMobBehavior.hpp"
#include "engine/voxel/IVoxelStreaming.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace registry {
class BlockRegistry;
class FluidRegistry;
}

namespace voxel {

// Block entity contract (engine/voxel/IVoxelBlockEntity.hpp): voxels with
// project-owned state. The engine stores/ticks/persists them; the project
// implements the behavior.
class IVoxelBlockEntity;
struct BlockEntityEvent;
using BlockEntityFactory = std::function<std::shared_ptr<IVoxelBlockEntity>()>;

// Service contracts defined in engine/voxel/IVoxelServices.hpp (included by
// clients that use them; forward declarations keep this header self-contained).
class IVoxelEditService;
class IChunkStorage;
class IVoxelMesher;
class IVoxelLighting;
class IVoxelFluidSimulation;
class IVoxelReplication;

// Result of a voxel raycast in world-block coordinates.
struct VoxelRaycastHit {
    bool hit{ false };
    glm::ivec3 block{ 0, 0, 0 };  // hit block coordinate
    glm::vec3 position{ 0.0f };   // exact entry point on the hit face
    glm::vec3 normal{ 0.0f };     // face normal of the hit block
    glm::ivec2 chunk{ 0, 0 };     // chunk containing the hit block
};

// Terrain sampling contract. Projects may replace the builtin generator with
// their own height/cave/ore functions via IVoxelWorld::register_generator.
// biomeIndex refers to the engine's BiomeType table; unknown values clamp to
// a neutral biome so custom generators stay safe.
struct TerrainPoint {
    int height{ 0 };
    float temperature{ 0.0f };
    float moisture{ 0.0f };
    float continentalness{ 0.0f };
    float river{ 0.0f };
    float erosion{ 0.0f };
    float weirdness{ 0.0f };
    float slope{ 0.0f };
    uint8_t biomeIndex{ 0 };
};

// Per-column decoration context (META section 18). The world fills this for
// every land column before the builtin tree pass and hands it to the
// generator's decorate_column hook.
struct DecorationContext {
    int localX{ 0 };         // chunk-local column X
    int localZ{ 0 };         // chunk-local column Z
    float worldX{ 0.0f };    // world-space column X
    float worldZ{ 0.0f };    // world-space column Z
    int surfaceHeight{ 0 };  // terrain surface height of the column
    uint32_t biomeIndex{ 0 };  // engine BiomeType index (clamped)
};

// Places a block at chunk-local coordinates (localY is world Y). Returns
// false when out of chunk bounds or the slot is occupied by a non-air,
// non-leaf block. The world updates its occupancy bookkeeping on every
// accepted write.
using BlockWriter = std::function<bool(int localX, int localY, int localZ,
                                       uint32_t blockId)>;

class IVoxelGenerator {
public:
    virtual ~IVoxelGenerator() = default;

    virtual TerrainPoint sample(float worldX, float worldZ) const = 0;
    virtual float cave_density(float worldX, float worldY, float worldZ) const = 0;
    virtual float ore_density(float worldX, float worldY, float worldZ) const = 0;

    // Optional data-driven ore substitution (META section 18): called for
    // stone-family blocks at depth > 5, AFTER the builtin geology/vein
    // substitution. `oreDensity` is the world's 3D ore-field sample at this
    // block (the same value the builtin thresholds use); `builtinBlock` is
    // what the builtin would place. Return a non-zero block id to override;
    // 0 keeps the builtin result.
    virtual uint32_t ore_block(float oreDensity, int y, uint32_t builtinBlock) const {
        (void)oreDensity;
        (void)y;
        (void)builtinBlock;
        return 0;
    }

    // Optional data-driven carver fill (META section 18): called for every
    // carved cell (cave_density above the carve threshold). `builtinCarve` is
    // what the world would place (Air, or Lava at low y). Return the block to
    // place; the default returns `builtinCarve`, so behavior is unchanged.
    virtual uint32_t carve_block(float caveDensity, int y, int depth,
                                 uint32_t builtinCarve) const {
        (void)caveDensity;
        (void)y;
        (void)depth;
        return builtinCarve;
    }

    // Optional data-driven surface decoration (META section 18): called for
    // every land column before the builtin tree pass. Return true to skip the
    // builtin per-biome tree placement for this column (data replaces code);
    // false keeps it.
    virtual bool decorate_column(const DecorationContext& ctx,
                                 const BlockWriter& write) const {
        (void)ctx;
        (void)write;
        return false;
    }

    // Optional data-driven surface override (META section 18): return a
    // non-zero block id to place instead of the builtin per-biome surface at
    // `depth` below the terrain surface (0 = top block); return 0 to keep the
    // builtin behavior. The default keeps every existing generator behaving
    // exactly as before.
    virtual uint32_t surface_block(const TerrainPoint& point, int depth) const {
        (void)point;
        (void)depth;
        return 0;
    }
};

// A single block edit inside a transaction. `blockId` 0 removes the block
// (sets Air). `previousBlockId` is filled by the world at commit time so a
// committed transaction can be undone.
struct BlockEdit {
    glm::ivec3 position{ 0, 0, 0 };
    uint32_t blockId{ 0 };
    uint32_t previousBlockId{ 0 };
};

// Notification fired by the world after a transaction outcome. Useful for
// logs/replay, networking and editor undo UI.
struct TransactionEvent {
    enum class Kind { Committed, RolledBack, Undone, Redone };
    Kind kind{ Kind::Committed };
    std::size_t editCount{ 0 };
    std::size_t undoDepth{ 0 };
};

// Structural per-transaction limits (FALTANTES §7 item 138), enforced in the
// VALIDATION stage — before any edit is applied. 0 disables the limit.
struct TransactionLimits {
    std::size_t maxEdits{ 0 };        // 0 = unlimited; a larger transaction is rejected
    uint64_t maxBoxVolume{ 0 };       // 0 = unlimited; bounding-box volume of ALL
                                      // edit positions (dx+1)*(dy+1)*(dz+1)
    bool operator==(const TransactionLimits&) const = default;
};

// Authoritative per-transaction validation (FALTANTES §7 item 138). The
// project (or server) registers one policy; every transaction is validated
// against it in the validation stage, BEFORE any edit is applied — a rejection
// leaves the world untouched (nothing applied, RolledBack event). This is the
// permission authority: block types, regions, or whole-transaction rules.
class ITransactionPolicy {
public:
    virtual ~ITransactionPolicy() = default;

    // Return a non-empty diagnostic to REJECT the edit (permission denied for
    // this block type / position / policy rule). Empty = allow.
    virtual std::string validate_edit(const BlockEdit& edit) const = 0;

    // Whole-transaction rule (edit count, spatial pattern, budget). Return a
    // non-empty diagnostic to reject; the default allows.
    virtual std::string validate_transaction(
        const std::vector<BlockEdit>& edits) const {
        (void)edits;
        return std::string();
    }
};

// Result of a transaction dry-run (FALTANTES §7 item 141): previews what a
// commit WOULD do without applying anything. `valid` mirrors commit()'s
// outcome (same validation: limits, policy, block ids, loaded chunks); on
// failure `error` carries the same diagnostic commit would produce; on success
// `diff` holds one BlockEdit per input edit with previousBlockId = the current
// world block (the before-state), so an editor/MCP can show the exact change
// set and validate before touching the world. Dry-run never mutates the world,
// never fires transaction events and never touches the undo stack.
struct EditDryRunResult {
    bool valid{ false };
    std::string error;
    std::vector<BlockEdit> diff;

    bool operator==(const EditDryRunResult&) const = default;
};

// Transactional edit API (META section 11): every world mutation flows through
// a transaction. commit() validates and applies all edits atomically through
// the single mutation path; on any failure the entire transaction rolls back
// and nothing changes. Committed transactions land on the world's undo stack.
class IVoxelTransaction {
public:
    virtual ~IVoxelTransaction() = default;

    virtual void set_block(int x, int y, int z, uint32_t blockId) = 0;
    virtual void remove_block(int x, int y, int z) = 0;  // sets Air

    // Applies all edits; false => nothing was applied (full rollback).
    virtual bool commit(std::string& errorOut) = 0;
    // Discards the edits without applying them.
    virtual void rollback() = 0;

    virtual std::size_t edit_count() const = 0;
};

class IVoxelWorld {
public:
    virtual ~IVoxelWorld() = default;

    virtual uint32_t get_block(int x, int y, int z) const = 0;
    virtual void set_block(int x, int y, int z, uint32_t blockId) = 0;
    virtual VoxelRaycastHit raycast(const glm::vec3& origin,
                                    const glm::vec3& direction,
                                    float maxDistance) const = 0;

    // Semantic block queries (A.2): consumers ask what a runtime block id
    // MEANS without reaching for the builtin enum (as_builtin_block) — the
    // answers come from the attached registry + runtime tables, so JSON-only
    // blocks behave identically to builtins. is_air matches the empty cell
    // id (0); is_fluid matches every id in the fluid table (water, lava or
    // any project-declared fluid, including inline FluidBinding blocks).
    virtual bool is_air(uint32_t blockId) const = 0;
    virtual bool is_fluid(uint32_t blockId) const = 0;
    virtual bool is_solid(uint32_t blockId) const = 0;

    virtual void register_generator(std::shared_ptr<IVoxelGenerator> generator) = 0;

    // Registry-driven ids (META section 7): the world resolves and validates
    // block ids against the registry instead of a hardcoded enum. A block that
    // is registered but has no storage mapping yet is rejected with a clear
    // diagnostic rather than silently aliasing Air.
    virtual void set_block_registry(
        std::shared_ptr<const registry::BlockRegistry> registry) = 0;
    virtual bool resolve_block_id(const std::string& namespacedName,
                                  uint32_t& idOut,
                                  std::string& errorOut) = 0;

    // Data-driven fluids (META section 13): the project declares what water,
    // lava or any custom fluid IS through FluidDefinition (viscosity, range,
    // cadence, source/falling/evaporation, damage). The engine runs the
    // simulation (levels, spreading, scheduler cadence, persistence). Each
    // definition references a registered block; a definition whose block is
    // unknown is refused with a diagnostic (never a guessed fluid). The world
    // keeps water + lava engine defaults when no registry is attached.
    virtual bool set_fluid_registry(
        std::shared_ptr<const registry::FluidRegistry> fluids,
        std::string& errorOut) = 0;

    // Public services (META section 6). Edit service is backed by the
    // transactional path; storage backs save/load when registered; the
    // mesher/lighting/fluid/replication registrations are stored and observable
    // (runtime integration lands with their milestones).
    virtual IVoxelEditService& edit_service() = 0;
    virtual void register_storage(std::shared_ptr<IChunkStorage> storage) = 0;

    // Entity layer (META section 15 / FALTANTES item 11): the world owns an
    // entity population indexed by voxel chunk, with generational handles,
    // per-entity tick policies (sleeping) and persistence through the world
    // save (v5 `world_entities`). Default: EnTT-backed; replaceable.
    virtual std::shared_ptr<engine::entity::IEntityWorld> entity_world() = 0;
    virtual void register_entity_world(
        std::shared_ptr<engine::entity::IEntityWorld> world) = 0;

    // Mob world queries (FALTANTES item 11): the minimal world view the
    // public IMobBehavior needs (fluid damage, block/fluid probes). The world
    // hosts it, so projects tick mobs through the public entity layer without
    // adapting their own world.
    virtual engine::entity::IMobWorldQuery& mob_world_query() = 0;
    virtual void register_mesher(std::shared_ptr<IVoxelMesher> mesher) = 0;
    virtual void register_lighting(std::shared_ptr<IVoxelLighting> lighting) = 0;
    virtual void register_fluid_simulation(
        std::shared_ptr<IVoxelFluidSimulation> fluid) = 0;
    virtual void register_replication(
        std::shared_ptr<IVoxelReplication> replication) = 0;
    virtual std::vector<std::string> registered_services() const = 0;


    virtual bool is_chunk_loaded(int chunkX, int chunkZ) const = 0;
    virtual int chunk_budget() const = 0;
    virtual void set_chunk_budget(int budget) = 0;
    // RAM-budgeted chunk cache (FALTANTES §4 item 6): bounds the estimated
    // bytes of loaded chunk data. Each update evicts the farthest NON-dirty
    // chunks while the estimate exceeds the budget (dirty chunks are never
    // evicted — edits are never lost to a cache eviction). 0 = unlimited
    // (default). streaming_snapshot reports memoryBudgetBytes/ramUsageBytes.
    virtual void set_memory_budget(uint64_t bytes) = 0;

    // Streaming/budget observability (FALTANTES §3): a snapshot of the
    // world's streaming state (chunk census + budgets), readable headless at
    // any time. Also the query surface the editor/CLI/servers use to show
    // what the world is doing.
    virtual StreamingSnapshot streaming_snapshot() const = 0;
    // Optional push observability: the world calls on_streaming_update after
    // each update() while streaming state changed. Null clears.
    virtual void set_streaming_monitor(
        std::shared_ptr<IVoxelStreamingMonitor> monitor) = 0;
    // Render handoff (task C.3, handoff 3->1): a read-only, NON-consuming
    // snapshot of the chunk-level dirty signals, in deterministic order (sorted
    // by chunkX then chunkZ). The world keeps owning the signals; a renderer
    // polls this and dedupes by (chunk, revision). See ChunkDirtyUpdate in
    // IVoxelStreaming.hpp.
    virtual std::vector<ChunkDirtyUpdate> render_dirty_updates() const = 0;
    // Effective runtime block table (FALTANTES §3 item 2): what the world
    // knows about each block AFTER the registry and plugin overrides merge.
    // Sorted by id; the mesher/light consumers read exactly this data.
    virtual std::vector<BlockRuntimeView> runtime_block_views() const = 0;

    // Transactional editing (META section 11). Edits via this API are the only
    // mutation path of the public contract: they are validated, applied
    // atomically (full rollback on failure), and land on the undo stack.
    //
    // The undo/redo history is SESSION-SCOPED (FALTANTES §7 item 140): it is
    // not persisted with any save format and a successful world load
    // (deserialize_world or load_world/load_world_regions) clears the undo
    // and redo stacks and resets the edit log — an undo after a load would
    // revert blocks against the freshly loaded world (not the session that
    // produced them), so the new session starts with a clean history. The
    // edit log is the total edits committed in the CURRENT session.
    virtual std::unique_ptr<IVoxelTransaction> begin_transaction() = 0;
    virtual bool undo_last_transaction() = 0;
    virtual bool redo_last_transaction() = 0;
    virtual std::size_t undo_depth() const = 0;

    // Dry-run + diff + structured diagnostic (FALTANTES §7 item 141): the
    // editor/MCP previews a transaction before committing. Runs the EXACT
    // validation of commit() (limits, policy, block registry, loaded chunks)
    // without applying, and reports the diff (before-state per edit) on
    // success. Pure: never mutates, never fires events, never touches undo.
    virtual EditDryRunResult dry_run_edits(
        const std::vector<BlockEdit>& edits) const = 0;

    // Event log: total edits committed through transactions (logs/replay) and
    // an optional listener for commit/rollback/undo/redo notifications.
    virtual std::size_t edit_log_count() const = 0;
    virtual void set_transaction_listener(
        std::function<void(const TransactionEvent&)> listener) = 0;

    // Permissions, authoritative validation and per-transaction limits
    // (FALTANTES §7 item 138): a registered policy is the permission authority
    // for every transaction (per-edit and whole-transaction), and structural
    // limits bound edit count and world-space volume. All enforced in the
    // validation stage, before anything is applied.
    virtual void set_transaction_limits(const TransactionLimits& limits) = 0;
    virtual TransactionLimits transaction_limits() const = 0;
    virtual void set_transaction_policy(std::shared_ptr<ITransactionPolicy> policy) = 0;

    // Discrete world lighting (META section 12): skylight + block light,
    // computed deterministically by the engine from block data (emission and
    // absorption are declared by the project through BlockDefinition). Values
    // settle asynchronously: after an edit the world relights within a few
    // frames (budgeted pass), so consumers poll or read once converged.
    virtual uint8_t get_sky_light(int x, int y, int z) const = 0;
    virtual uint8_t get_block_light(int x, int y, int z) const = 0;

    // Fluid level at a world voxel (META section 13): 0xFF = no fluid,
    // 0 = source, 1..7 = depth (WATER_MAX_LEVEL). The byte belongs to whatever
    // fluid block occupies the cell — water, lava or a data-driven fluid.
    // Renderers read this to visualize levels/flows without owning the
    // simulation; 0xFF is returned for non-fluid cells.
    virtual uint8_t get_fluid_level(int x, int y, int z) const = 0;

    // Per-voxel block state index (FALTANTES item 2 "variantes de modelo"):
    // 0 = default state (states[0] in BlockDefinition); >0 = named state.
    // The mesher reads this to pick per-state material (color/light);
    // transitions fire through the gameplay/scripting layer. Stored alongside
    // blocks so state queries are O(1); persists with the save and replicates
    // with the region.
    virtual uint8_t get_block_state(int x, int y, int z) const = 0;
    virtual void set_block_state(int x, int y, int z, uint8_t stateIndex) = 0;

    // Block entities (META section 8): voxels with project-owned state. The
    // engine stores them, ticks them deterministically via the world scheduler
    // (budget, active regions, sleeping), destroys them atomically when their
    // block is removed, and persists them (versioned project blob) with the
    // world save. Attaching requires the type's factory to be registered (so a
    // save can always be reconstructed) and a loaded non-empty block at the
    // position.
    virtual void register_block_entity_type(
        const std::string& typeId, BlockEntityFactory factory) = 0;
    virtual bool attach_block_entity(int x, int y, int z,
                                     std::shared_ptr<IVoxelBlockEntity> entity,
                                     std::string& errorOut) = 0;
    virtual std::shared_ptr<IVoxelBlockEntity> block_entity_at(
        int x, int y, int z) const = 0;
    virtual bool remove_block_entity(int x, int y, int z) = 0;
    virtual std::size_t block_entity_count() const = 0;
    virtual void set_block_entity_listener(
        std::function<void(const BlockEntityEvent&)> listener) = 0;

    // Region replication (META section 17 / FALTANTES item 6): enumerates the
    // block entities attached inside one chunk column, in deterministic
    // position order (x, y, z). Empty when the chunk has none. The engine
    // owns storage; this is how a region snapshot syncs block entities.
    virtual std::vector<std::pair<glm::ivec3, std::shared_ptr<IVoxelBlockEntity>>>
    block_entities_in_chunk(int chunkX, int chunkZ) const = 0;
    // Factory lookup for region replication / save reconstruction: returns a
    // fresh instance for a registered type id, or nullptr with a diagnostic
    // when the type has no registered factory (a snapshot referencing an
    // unknown type is refused — never a guessed entity).
    virtual std::shared_ptr<IVoxelBlockEntity> create_block_entity(
        const std::string& typeId, std::string& errorOut) = 0;

    // Persistence (META section 10). save/load are versioned and checksummed:
    // a save captures the authoritative voxel state of every loaded chunk;
    // load restores it and refuses corrupted or newer-schema data with a
    // clear diagnostic instead of crashing. serialize/deserialize expose the
    // same format in memory (tests, networking, replays).
    virtual bool save_world(const std::string& filePath, std::string& errorOut) = 0;
    virtual bool load_world(const std::string& filePath, std::string& errorOut) = 0;
    virtual std::string serialize_world(std::string& errorOut) = 0;
    virtual bool deserialize_world(const std::string& data, std::string& errorOut) = 0;

    // World identity + content provenance (FALTANTES §4 item 4): the engine
    // stores these on the world (set_world_metadata) and rides them with the
    // world save (v5 `meta`), restoring them on load. `seed` is the per-world
    // generator/behavior RNG seed, `worldName`/`rulesJson` the project's
    // identity, `pluginVersions` the (name, version) pairs of the plugins
    // that produced the content. A save written without metadata (or a legacy
    // v1-v4 save) loads with the defaults, and the caller's set_world_metadata
    // values are kept. Interpretation is the caller's; the engine never
    // guesses the payloads.
    struct WorldMetadata {
        uint64_t seed{ 0 };
        std::string worldName;
        std::string rulesJson;
        std::vector<std::pair<std::string, std::string>> pluginVersions;
    };
    virtual void set_world_metadata(const WorldMetadata& metadata) = 0;
    virtual WorldMetadata world_metadata() const = 0;

    // Registry fingerprint (FALTANTES §4 item 4 "registries/paletas"): a
    // deterministic hash of the attached block registry (uuid + definition
    // version + namespaced name per entry, sorted by uuid — independent of
    // load order). registry_version() is the CURRENT registry's fingerprint;
    // saved_registry_version() is the fingerprint carried by the last loaded
    // save (0 when nothing was loaded or the save had no metadata). A
    // mismatch means the registry changed since the save was written — the
    // UUID palette still restores identities, but callers can warn instead of
    // guessing.
    virtual uint64_t registry_version() const = 0;
    virtual uint64_t saved_registry_version() const = 0;

    // Schema versioning + migration (FALTANTES §4 item 9): the save format is
    // versioned (v1-v5, see the format comment in the SDK). world_save_schema_
    // version reads the version field WITHOUT a full parse (0 = not a world
    // save) so a caller can decide to load or migrate before committing.
    // migrate_world_save upgrades a legacy save (v1-v4) to the CURRENT schema
    // version, all-or-nothing with the same gates as load (checksum, palette
    // uuids vs the current registry, block ids) — it is PURE: the live world
    // is untouched and the migrated bytes are returned for the caller to
    // persist. A save already at the current version is a no-op success; a
    // save from a NEWER engine is refused with a clear diagnostic (never
    // silently downgraded).
    virtual uint32_t world_save_schema_version(const std::string& data) const = 0;
    virtual bool migrate_world_save(const std::string& legacyData,
                                    std::string& migratedOut,
                                    std::string& errorOut) = 0;

    // Async persistence (FALTANTES §4 item 5): the expensive parts of a save
    // (palette + zstd encode) and of a load (file reads, decompression, chunk
    // apply) run on background jobs; `onDone(ok, error)` fires on a background
    // thread when the op finishes. The return value only reports whether the
    // op was DISPATCHED; the result comes from onDone or wait_async_saves.
    // Requires a region-capable (paged) storage — otherwise a clear
    // diagnostic is returned and nothing is dispatched. One async op at a
    // time: a save/load while one is in flight is refused. wait_async_saves
    // blocks until any in-flight op completes and returns its result (no-op
    // true when nothing is in flight). Contract: the async save captures a
    // consistent snapshot at dispatch, so the caller may keep simulating; an
    // async LOAD applies chunks from a background thread, so the caller must
    // not touch the world until onDone/wait_async_saves.
    virtual bool save_world_async(
        const std::string& filePath,
        std::function<void(bool ok, std::string error)> onDone,
        std::string& errorOut) = 0;
    virtual bool load_world_async(
        const std::string& filePath,
        std::function<void(bool ok, std::string error)> onDone,
        std::string& errorOut) = 0;
    virtual bool wait_async_saves(std::string& errorOut) = 0;

    // Autosave (FALTANTES §4 item 7): while enabled, the headless update()
    // tick checks two triggers — an elapsed-time interval and a change-volume
    // threshold (loaded chunks whose revision moved since the last autosave,
    // the same delta gate as region saves). When either fires AND no async op
    // is in flight, a DELTA async save (items 3 + 5: only changed region
    // tiles, encoded/written on background jobs) is dispatched to the
    // configured autosave path; the caller keeps simulating. A fire while
    // another save/load is in flight is skipped and the timers keep running,
    // so the next eligible update fires it. Disabling autosave stops future
    // fires; an in-flight autosave still completes and is reported by
    // wait_async_saves. Requires a region-capable (paged) storage, like
    // save_world_async.
    struct AutosaveConfig {
        bool enabled = false;
        double intervalSeconds = 30.0;        // time trigger; <= 0 disables
        std::size_t dirtyChunkThreshold = 0;  // volume trigger; 0 disables
    };
    virtual void set_autosave(const AutosaveConfig& config,
                              const std::string& autosavePath) = 0;
    virtual AutosaveConfig autosave_config() const = 0;

    // Headless simulation tick (server/tests): generation, meshing and fluid
    // simulation run without any renderer attached.
    virtual void update(const glm::vec3& playerPosition, float deltaTime) = 0;

    // The registry currently driving the world's runtime block table (never
    // null — an empty/default registry when none was attached). Replication
    // reads it to negotiate the block palette: the server ships every
    // catalog-only block's definition so a client reconstructs the same
    // dynamic runtime ids without recompiling (FALTANTES item 1).
    virtual std::shared_ptr<const registry::BlockRegistry> block_registry() const = 0;
};

// Creates the default engine world (builtin generator, headless bridge).
// The returned world owns its simulation state and can be ticked headless.
std::unique_ptr<IVoxelWorld> create_default_voxel_world();

}  // namespace voxel
}  // namespace engine
