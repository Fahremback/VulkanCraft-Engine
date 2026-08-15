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

class IVoxelGenerator {
public:
    virtual ~IVoxelGenerator() = default;

    virtual TerrainPoint sample(float worldX, float worldZ) const = 0;
    virtual float cave_density(float worldX, float worldY, float worldZ) const = 0;
    virtual float ore_density(float worldX, float worldY, float worldZ) const = 0;
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
    virtual void register_mesher(std::shared_ptr<IVoxelMesher> mesher) = 0;
    virtual void register_lighting(std::shared_ptr<IVoxelLighting> lighting) = 0;
    virtual void register_fluid_simulation(
        std::shared_ptr<IVoxelFluidSimulation> fluid) = 0;
    virtual void register_replication(
        std::shared_ptr<IVoxelReplication> replication) = 0;
    virtual std::vector<std::string> registered_services() const = 0;

    // Server/test knob: disables mob spawning for deterministic headless runs.
    virtual void set_mob_spawning(bool enabled) = 0;

    virtual bool is_chunk_loaded(int chunkX, int chunkZ) const = 0;
    virtual int chunk_budget() const = 0;
    virtual void set_chunk_budget(int budget) = 0;

    // Transactional editing (META section 11). Edits via this API are the only
    // mutation path of the public contract: they are validated, applied
    // atomically (full rollback on failure), and land on the undo stack.
    virtual std::unique_ptr<IVoxelTransaction> begin_transaction() = 0;
    virtual bool undo_last_transaction() = 0;
    virtual bool redo_last_transaction() = 0;
    virtual std::size_t undo_depth() const = 0;

    // Event log: total edits committed through transactions (logs/replay) and
    // an optional listener for commit/rollback/undo/redo notifications.
    virtual std::size_t edit_log_count() const = 0;
    virtual void set_transaction_listener(
        std::function<void(const TransactionEvent&)> listener) = 0;

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

    // Persistence (META section 10). save/load are versioned and checksummed:
    // a save captures the authoritative voxel state of every loaded chunk;
    // load restores it and refuses corrupted or newer-schema data with a
    // clear diagnostic instead of crashing. serialize/deserialize expose the
    // same format in memory (tests, networking, replays).
    virtual bool save_world(const std::string& filePath, std::string& errorOut) = 0;
    virtual bool load_world(const std::string& filePath, std::string& errorOut) = 0;
    virtual std::string serialize_world(std::string& errorOut) = 0;
    virtual bool deserialize_world(const std::string& data, std::string& errorOut) = 0;

    // Headless simulation tick (server/tests): generation, meshing and fluid
    // simulation run without any renderer attached.
    virtual void update(const glm::vec3& playerPosition, float deltaTime) = 0;
};

// Creates the default engine world (builtin generator, headless bridge).
// The returned world owns its simulation state and can be ticked headless.
std::unique_ptr<IVoxelWorld> create_default_voxel_world();

}  // namespace voxel
}  // namespace engine
