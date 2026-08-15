#include "engine/voxel/IVoxelWorld.hpp"
#include "engine/voxel/IVoxelServices.hpp"
#include "engine/registry/BlockRegistry.hpp"
#include "engine/registry/FluidRegistry.hpp"
#include "engine/compression/ICompressionProvider.hpp"
#include "engine/hashing/IHashProvider.hpp"
#if VC_ENABLE_FLATBUFFERS
#include "world_save_generated.h"
#endif

#include "../simulation/voxel/streaming/World.hpp"
#include "../simulation/voxel/streaming/WorldRenderBridge.hpp"
#include "../simulation/voxel/storage/ChunkConstants.hpp"
#include "../simulation/voxel/core/Voxel.hpp"
#include "../simulation/entities/Mob.hpp"
#include "../engine/audio/SoundEngine.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

// Derives the world's dynamic block table from the registry. Catalog entries
// (no builtin mapping) receive dynamic runtime ids (>= BlockType::Count) in
// UUID-sorted order — deterministic, so ids never depend on JSON load order
// (Prioridade 0 item 1, FALTANTES). Builtin ids keep the engine material
// table and are implied by the stable enum.
std::pair<std::unordered_map<RuntimeBlockId, RuntimeBlockInfo>,
          std::unordered_map<std::string, RuntimeBlockId>>
build_runtime_block_table(const engine::registry::BlockRegistry& registry) {
    std::unordered_map<RuntimeBlockId, RuntimeBlockInfo> table;
    std::unordered_map<std::string, RuntimeBlockId> uuidToId;
    RuntimeBlockId nextId = static_cast<RuntimeBlockId>(BlockType::Count);
    for (const engine::registry::BlockDefinition& definition : registry.all_definitions()) {
        if (definition.hasBuiltinMapping) continue;
        RuntimeBlockInfo info;
        info.uuid = definition.uuid;
        info.color = definition.color;
        info.solid = definition.collidable;
        info.transparent = definition.blockClass == engine::registry::BlockClass::Transparent;
        info.fluid = definition.blockClass == engine::registry::BlockClass::Fluid;
        // Discrete lighting (META section 12): JSON floats (0..1) scale to
        // light levels (0..15). Absorption 15 = opaque (default derives from
        // solidity/transparency below).
        info.lightEmission = static_cast<uint8_t>(std::min<int>(
            15, static_cast<int>(definition.lightEmission * 15.0f + 0.5f)));
        // JSON float (0..1) scales to 0..15; 1.0 (default) = opaque. A
        // transparent block declares e.g. 0.0 to let light through.
        info.lightAbsorption = static_cast<uint8_t>(std::min<int>(
            15, static_cast<int>(definition.lightAbsorption * 15.0f + 0.5f)));
        table.emplace(nextId, std::move(info));
        uuidToId.emplace(definition.uuid, nextId);
        ++nextId;
        if (nextId == 0) break;  // id space exhausted (65535 dynamic blocks)
    }
    return { std::move(table), std::move(uuidToId) };
}

// Derives the world's fluid table: engine defaults (water + lava) overridden
// by the project's FluidRegistry definitions (a definition attaches fluid
// behavior to a registered block, by namespaced name). Returns false with a
// diagnostic when a definition references a block the block registry does not
// know — the world would otherwise run the wrong fluid silently.
bool build_fluid_table(const engine::registry::BlockRegistry& blocks,
                       const engine::registry::FluidRegistry* fluids,
                       const World& world,
                       std::unordered_map<RuntimeBlockId, FluidParams>& tableOut,
                       std::string& errorOut) {
    // Engine defaults: water keeps the historical behavior (1 level/tick,
    // range 7, evaporates when unfed); lava becomes a REAL slow fluid with
    // damage instead of a static block.
    auto defaults = [&]() {
        std::unordered_map<RuntimeBlockId, FluidParams> table;
        FluidParams water;
        water.viscosity = 0.5f;
        water.density = 1.0f;
        water.maxLevel = 7;
        water.levelsPerTick = 1;
        water.tickEveryTicks = 1;
        water.source = true;
        water.falling = true;
        water.evaporation = true;
        water.color = glm::vec4(0.30f, 0.60f, 1.00f, 0.65f);
        table[runtime_id(BlockType::Water)] = water;
        FluidParams lava;
        lava.viscosity = 1.0f;
        lava.density = 2.0f;
        lava.maxLevel = 3;
        lava.levelsPerTick = 1;
        lava.tickEveryTicks = 2;
        lava.source = true;
        lava.falling = true;
        lava.evaporation = true;
        lava.damagePerTick = 4.0f;
        lava.color = glm::vec4(1.00f, 0.40f, 0.10f, 0.90f);
        table[runtime_id(BlockType::Lava)] = lava;
        return table;
    };
    tableOut = defaults();
    if (!fluids) return true;

    for (const engine::registry::FluidDefinition& definition :
         fluids->all_definitions()) {
        const engine::registry::BlockDefinition* block =
            blocks.find_by_name(definition.block);
        if (!block) {
            errorOut = "fluid '" + definition.block +
                       "' references an unknown block (register it in the block "
                       "registry first)";
            return false;
        }
        RuntimeBlockId id;
        if (block->hasBuiltinMapping) {
            id = static_cast<RuntimeBlockId>(block->builtinId);
        } else {
            const std::optional<RuntimeBlockId> dynamic =
                world.runtime_block_id_for_uuid(block->uuid);
            if (!dynamic) {
                errorOut = "fluid '" + definition.block +
                           "': its block has no runtime mapping yet (attach the "
                           "block registry before booting)";
                return false;
            }
            id = *dynamic;
        }
        FluidParams params;
        params.viscosity = definition.viscosity;
        params.density = definition.density;
        params.maxLevel = std::clamp(definition.range, 1, 7);
        // Thin fluids (viscosity <= 0.25) gain 2 levels per step; everything
        // else 1 (water/lava keep the historical step).
        params.levelsPerTick = definition.viscosity <= 0.25f ? 2 : 1;
        params.tickEveryTicks = std::max(1, static_cast<int>(std::lround(
            definition.tickInterval / World::kFluidTickSeconds)));
        params.source = definition.source;
        params.falling = definition.falling;
        params.evaporation = definition.evaporation;
        params.damagePerTick = definition.damagePerTick;
        params.color = definition.color;
        params.compressible = definition.compressible;
        tableOut[id] = params;  // project definition overrides the engine default
    }
    errorOut.clear();
    return true;
}

// World save format: "VCWLD" + u32 schema version + u32 chunk count, then per
// chunk (cx, cz, extent u32, extent*256 block bytes, extent*256 water bytes),
// then an FNV-1a checksum over everything before it. Little-endian binary.
//
// v1: blocks are 1 byte each (builtin BlockType ids only), no palette.
// v2: a palette of DYNAMIC runtime ids (>= BlockType::Count) with their
//     persistent UUIDs precedes the chunk table; blocks are 2 bytes each
//     (RuntimeBlockId). Builtin ids are implied (stable enum). The palette is
//     validated against the world's registry-derived table on load, so a save
//     is refused with a diagnostic when the registry content changed.
constexpr char kWorldMagic[6] = "VCWLD";
constexpr uint32_t kWorldSaveVersionV1 = 1;
constexpr uint32_t kWorldSaveVersionV2 = 2;
// v3 adds a block entity section after the chunk table: (x,y,z, typeId,
// dataVersion, opaque project blob) per entity, reconstructed on load through
// registered factories. v1/v2 saves keep loading (no entity section).
constexpr uint32_t kWorldSaveVersionV3 = 3;
// v4 (promoted solutions, META section 32): the trailing checksum switches
// from FNV-1a (8 bytes, v1-v3) to a BLAKE3-256 digest (32 bytes) and the file
// layer compresses saves with Zstandard. v1-v3 loads keep working (FNV path).
constexpr uint32_t kWorldSaveVersionV4 = 4;
// v5 (promoted solutions, META section 32): the body is a FlatBuffers
// container (schema src/engine/sdk/world_save.fbs) instead of the hand-rolled
// binary layout; v4 framing (BLAKE3 + zstd file layer) is unchanged. The
// loader keeps the manual parser for v1-v4 legacy saves.
constexpr uint32_t kWorldSaveVersion = 5;

void append_u16(std::string& out, uint16_t value) {
    out.push_back(static_cast<char>(value & 0xFFu));
    out.push_back(static_cast<char>((value >> 8) & 0xFFu));
}

uint16_t read_u16(const std::string& data, std::size_t offset) {
    if (offset + 2 > data.size()) return 0;
    return static_cast<uint16_t>(static_cast<uint8_t>(data[offset])) |
           static_cast<uint16_t>(static_cast<uint16_t>(static_cast<uint8_t>(data[offset + 1])) << 8);
}

void append_u32(std::string& out, uint32_t value) {
    const char bytes[4] = { static_cast<char>(value & 0xFFu),
                            static_cast<char>((value >> 8) & 0xFFu),
                            static_cast<char>((value >> 16) & 0xFFu),
                            static_cast<char>((value >> 24) & 0xFFu) };
    out.append(bytes, 4);
}

void append_i32(std::string& out, int32_t value) {
    append_u32(out, static_cast<uint32_t>(value));
}

void append(std::string& out, const std::string& bytes) {
    out.append(bytes);
}

void append_u64(std::string& out, uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xFFu));
    }
}

uint64_t read_u64(const std::string& data, std::size_t offset) {
    if (offset + 8 > data.size()) return 0;
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(static_cast<uint8_t>(data[offset + i]))
                 << (8 * i);
    }
    return value;
}

uint32_t read_u32(const std::string& data, std::size_t offset) {
    if (offset + 4 > data.size()) return 0;
    return static_cast<uint32_t>(static_cast<uint8_t>(data[offset])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(data[offset + 1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(data[offset + 2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(data[offset + 3])) << 24);
}

uint64_t fnv1a_bytes(const std::string& data) {
    uint64_t hash = 1469598103934665603ull;
    for (const unsigned char c : data) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return hash;
}

// Headless bridge: generation/meshing run to completion but nothing is ever
// uploaded to a GPU. This is the server/test path of the public world.
class NullBridge final : public WorldRenderBridge {
public:
    void begin_frame() override {}
    void request_far_terrain(int, int, int, float) override {}
    void retire_chunk(ChunkId) override {}
    void upload_chunk(ChunkMeshResult) override {}
};

class VoxelTransactionImpl;  // fwd (defined below the facade)

// Adapts the engine's simulation World to the public SDK contract. Owns its
// audio/mob/world state so the returned IVoxelWorld is fully self-contained.
// All mutations (transactional or the single-edit convenience) flow through
// apply_edits — the single mutation path of the public contract.
class VoxelWorldFacade final : public engine::voxel::IVoxelWorld,
                               public engine::voxel::IVoxelEditService {
public:
    VoxelWorldFacade()
        : mobs_(audio_),
          world_(mobs_),
          registry_(std::make_shared<engine::registry::BlockRegistry>()),
          compression_(engine::compression::create_zstd_compression_provider()),
          hash_(engine::hashing::create_blake3_hash_provider()),
          entityWorld_(engine::entity::create_entity_world()) {
        // The world's dynamic block table comes from the attached registry
        // (empty dynamic set with the builtin default; catalog blocks enter via
        // set_block_registry before boot). The fluid table starts with the
        // engine defaults (water + lava) and is rebuilt whenever a registry is
        // attached.
        auto table = build_runtime_block_table(*registry_);
        world_.set_runtime_block_table(std::move(table.first), std::move(table.second));
        std::string fluidError;
        std::unordered_map<RuntimeBlockId, FluidParams> fluidTable;
        build_fluid_table(*registry_, nullptr, world_, fluidTable, fluidError);
        world_.set_fluid_table(std::move(fluidTable));
    }

    uint32_t get_block(int x, int y, int z) const override {
        return static_cast<uint32_t>(world_.get_block_at(
            glm::vec3(static_cast<float>(x), static_cast<float>(y),
                      static_cast<float>(z))));
    }

    void set_block(int x, int y, int z, uint32_t blockId) override {
        // Convenience API: an implicit single-edit transaction, so even direct
        // edits are undoable and nothing bypasses the transactional path.
        std::string error;
        std::vector<engine::voxel::BlockEdit> edits;
        edits.push_back(engine::voxel::BlockEdit{ { x, y, z }, blockId, 0 });
        apply_edits(std::move(edits), error);
    }

    engine::voxel::VoxelRaycastHit raycast(const glm::vec3& origin,
                                           const glm::vec3& direction,
                                           float maxDistance) const override {
        engine::voxel::VoxelRaycastHit hit;
        const float length = glm::length(direction);
        if (length <= 1e-6f || maxDistance <= 0.0f) return hit;
        const glm::vec3 dir = direction / length;

        glm::ivec3 cell(static_cast<int>(std::floor(origin.x)),
                        static_cast<int>(std::floor(origin.y)),
                        static_cast<int>(std::floor(origin.z)));
        glm::ivec3 step(0);
        glm::vec3 tDelta(0.0f);
        // Zero-velocity axes must never win the closest-face tie, so their
        // distance starts at "infinity" instead of 0.
        glm::vec3 tMax(1e30f);
        glm::vec3 faceNormal(0.0f);

        // Amanatides & Woo: precompute per-axis traversal parameters.
        for (int axis = 0; axis < 3; ++axis) {
            if (dir[axis] > 1e-9f) {
                step[axis] = 1;
                tDelta[axis] = 1.0f / dir[axis];
                tMax[axis] = (static_cast<float>(cell[axis]) + 1.0f - origin[axis]) *
                             tDelta[axis];
                faceNormal[axis] = -1.0f;
            } else if (dir[axis] < -1e-9f) {
                step[axis] = -1;
                tDelta[axis] = -1.0f / dir[axis];
                tMax[axis] = (origin[axis] - static_cast<float>(cell[axis])) *
                             tDelta[axis];
                faceNormal[axis] = 1.0f;
            }
        }

        float t = 0.0f;
        int guard = 0;
        while (t <= maxDistance && guard++ < 4096) {
            const uint32_t blockId = get_block(cell.x, cell.y, cell.z);
            if (world_.is_solid_block_id(static_cast<RuntimeBlockId>(blockId))) {
                hit.hit = true;
                hit.block = cell;
                hit.position = origin + dir * t;
                hit.normal = faceNormal;
                hit.chunk = { static_cast<int>(std::floor(
                                  static_cast<float>(cell.x) / CHUNK_SIZE_X)),
                              static_cast<int>(std::floor(
                                  static_cast<float>(cell.z) / CHUNK_SIZE_Z)) };
                return hit;
            }

            // Advance to the next cell across the closest face.
            if (tMax.x < tMax.y) {
                if (tMax.x < tMax.z) {
                    t = tMax.x;
                    tMax.x += tDelta.x;
                    cell.x += step.x;
                    faceNormal = glm::vec3(-static_cast<float>(step.x), 0.0f, 0.0f);
                } else {
                    t = tMax.z;
                    tMax.z += tDelta.z;
                    cell.z += step.z;
                    faceNormal = glm::vec3(0.0f, 0.0f, -static_cast<float>(step.z));
                }
            } else {
                if (tMax.y < tMax.z) {
                    t = tMax.y;
                    tMax.y += tDelta.y;
                    cell.y += step.y;
                    faceNormal = glm::vec3(0.0f, -static_cast<float>(step.y), 0.0f);
                } else {
                    t = tMax.z;
                    tMax.z += tDelta.z;
                    cell.z += step.z;
                    faceNormal = glm::vec3(0.0f, 0.0f, -static_cast<float>(step.z));
                }
            }
        }
        return hit;
    }

    void register_generator(std::shared_ptr<engine::voxel::IVoxelGenerator> generator) override {
        world_.set_generator_override(std::move(generator));
    }

    void set_mob_spawning(bool enabled) override {
        world_.mobSpawningEnabled = enabled;
    }

    void set_block_registry(
        std::shared_ptr<const engine::registry::BlockRegistry> registry) override {
        registry_ = registry;
        auto table = build_runtime_block_table(*registry);
        world_.set_runtime_block_table(std::move(table.first), std::move(table.second));
        // Fluid definitions resolve their block ids through the block table:
        // attaching a block registry may make previously unresolvable fluids
        // valid, so the fluid table is rebuilt. Dynamic ids are deterministic
        // per UUID, so existing mappings stay valid; a definition whose block
        // still does not exist simply stays out of the table (set_fluid_registry
        // reports that hard diagnostic).
        std::string fluidError;
        std::unordered_map<RuntimeBlockId, FluidParams> fluidTable;
        if (build_fluid_table(*registry_, fluidRegistry_.get(), world_,
                              fluidTable, fluidError)) {
            world_.set_fluid_table(std::move(fluidTable));
        }
    }

    bool set_fluid_registry(
        std::shared_ptr<const engine::registry::FluidRegistry> fluids,
        std::string& errorOut) override {
        fluidRegistry_ = std::move(fluids);
        std::unordered_map<RuntimeBlockId, FluidParams> fluidTable;
        if (!build_fluid_table(*registry_, fluidRegistry_.get(), world_,
                               fluidTable, errorOut)) {
            fluidRegistry_ = nullptr;
            return false;
        }
        world_.set_fluid_table(std::move(fluidTable));
        return true;
    }

    bool resolve_block_id(const std::string& namespacedName, uint32_t& idOut,
                          std::string& errorOut) override {
        if (!registry_) {
            errorOut = "no block registry attached";
            return false;
        }
        const engine::registry::BlockDefinition* definition =
            registry_->find_by_name(namespacedName);
        if (!definition) {
            errorOut = "unknown block '" + namespacedName + "'";
            return false;
        }
        if (definition->hasBuiltinMapping) {
            idOut = definition->builtinId;
            return true;
        }
        // Catalog-only block: the world allocated a dynamic runtime id for it
        // (UUID-sorted order — independent of JSON load order).
        const std::optional<RuntimeBlockId> dynamic =
            world_.runtime_block_id_for_uuid(definition->uuid);
        if (!dynamic) {
            errorOut = "block '" + namespacedName +
                       "' is registered but not mapped by the world (attach the "
                       "registry before booting)";
            return false;
        }
        idOut = static_cast<uint32_t>(*dynamic);
        return true;
    }

    engine::voxel::IVoxelEditService& edit_service() override { return *this; }

    void register_storage(std::shared_ptr<engine::voxel::IChunkStorage> storage) override {
        storage_ = std::move(storage);
    }

    std::shared_ptr<engine::entity::IEntityWorld> entity_world() override {
        return entityWorld_;
    }

    void register_entity_world(
        std::shared_ptr<engine::entity::IEntityWorld> world) override {
        entityWorld_ = std::move(world);
    }

    void register_mesher(std::shared_ptr<engine::voxel::IVoxelMesher> mesher) override {
        mesher_ = std::move(mesher);
    }

    void register_lighting(std::shared_ptr<engine::voxel::IVoxelLighting> lighting) override {
        lighting_ = std::move(lighting);
    }

    void register_fluid_simulation(
        std::shared_ptr<engine::voxel::IVoxelFluidSimulation> fluid) override {
        fluid_ = std::move(fluid);
    }

    void register_replication(
        std::shared_ptr<engine::voxel::IVoxelReplication> replication) override {
        replication_ = std::move(replication);
    }

    std::vector<std::string> registered_services() const override {
        std::vector<std::string> names;
        if (storage_) names.push_back("storage");
        if (mesher_) names.push_back(std::string("mesher:") + mesher_->name());
        if (lighting_) names.push_back(std::string("lighting:") + lighting_->name());
        if (fluid_) names.push_back(std::string("fluid:") + fluid_->name());
        if (replication_) names.push_back(std::string("replication:") + replication_->name());
        return names;
    }

    bool undo() override { return undo_last_transaction(); }
    bool redo() override { return redo_last_transaction(); }

    bool is_chunk_loaded(int chunkX, int chunkZ) const override {
        return world_.is_chunk_loaded_at(glm::vec3(
            static_cast<float>(chunkX) * CHUNK_SIZE_X + 8.0f, 40.0f,
            static_cast<float>(chunkZ) * CHUNK_SIZE_Z + 8.0f));
    }

    int chunk_budget() const override { return world_.chunkBudget; }

    void set_chunk_budget(int budget) override { world_.set_chunk_budget(budget); }

    // Declared here, defined below once VoxelTransactionImpl is complete.
    std::unique_ptr<engine::voxel::IVoxelTransaction> begin_transaction() override;

    bool undo_last_transaction() override {
        if (undoStack_.empty()) return false;
        std::vector<engine::voxel::BlockEdit> edits = std::move(undoStack_.back());
        undoStack_.pop_back();
        // Restore previous ids in reverse order.
        for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
            world_.set_block_at(glm::vec3(static_cast<float>(it->position.x),
                                          static_cast<float>(it->position.y),
                                          static_cast<float>(it->position.z)),
                                static_cast<RuntimeBlockId>(it->previousBlockId));
        }
        redoStack_.push_back(std::move(edits));
        notify(engine::voxel::TransactionEvent::Kind::Undone,
               redoStack_.back().size());
        return true;
    }

    bool redo_last_transaction() override {
        if (redoStack_.empty()) return false;
        std::vector<engine::voxel::BlockEdit> edits = std::move(redoStack_.back());
        redoStack_.pop_back();
        for (const engine::voxel::BlockEdit& edit : edits) {
            world_.set_block_at(glm::vec3(static_cast<float>(edit.position.x),
                                          static_cast<float>(edit.position.y),
                                          static_cast<float>(edit.position.z)),
                                static_cast<RuntimeBlockId>(edit.blockId));
        }
        undoStack_.push_back(std::move(edits));
        notify(engine::voxel::TransactionEvent::Kind::Redone,
               undoStack_.back().size());
        return true;
    }

    std::size_t undo_depth() const override { return undoStack_.size(); }

    std::size_t edit_log_count() const override { return editLog_; }

    void set_transaction_listener(
        std::function<void(const engine::voxel::TransactionEvent&)> listener) override {
        listener_ = std::move(listener);
    }

    // The single mutation path of the public contract. Validates every edit,
    // captures previous state, applies through World::set_block_at (which
    // dirties chunks so the streaming pipeline re-meshes), verifies the apply
    // actually landed (loaded chunk), and rolls back everything on any failure.
    bool apply_edits(std::vector<engine::voxel::BlockEdit> edits,
                     std::string& errorOut) {
        // The world's runtime table is the source of truth for settable ids:
        // builtin ids (< BlockType::Count) are always valid engine blocks;
        // dynamic ids (>= Count) must be registered (UUID identity) — a
        // fabricated id is rejected here instead of silently aliasing Air.
        for (const engine::voxel::BlockEdit& edit : edits) {
            if (edit.blockId > 0xFFFFu ||
                !world_.is_valid_block_id(static_cast<RuntimeBlockId>(edit.blockId))) {
                errorOut = "transaction rejected: blockId " +
                           std::to_string(edit.blockId) +
                           " has no runtime mapping in the world's block registry";
                notify(engine::voxel::TransactionEvent::Kind::RolledBack, edits.size());
                return false;
            }
        }

        std::vector<engine::voxel::BlockEdit> applied;
        applied.reserve(edits.size());
        for (const engine::voxel::BlockEdit& edit : edits) {
            const glm::vec3 position(static_cast<float>(edit.position.x),
                                     static_cast<float>(edit.position.y),
                                     static_cast<float>(edit.position.z));
            const uint32_t previous =
                static_cast<uint32_t>(world_.get_block_at(position));
            world_.set_block_at(position, static_cast<RuntimeBlockId>(edit.blockId));
            const uint32_t now =
                static_cast<uint32_t>(world_.get_block_at(position));
            if (now != edit.blockId) {
                // Apply failed (unloaded chunk / rejected edit): roll back every
                // edit already applied by this transaction.
                for (auto it = applied.rbegin(); it != applied.rend(); ++it) {
                    world_.set_block_at(glm::vec3(static_cast<float>(it->position.x),
                                                  static_cast<float>(it->position.y),
                                                  static_cast<float>(it->position.z)),
                                        static_cast<RuntimeBlockId>(it->previousBlockId));
                }
                errorOut = "transaction rolled back: chunk not loaded at (" +
                           std::to_string(edit.position.x) + ',' +
                           std::to_string(edit.position.y) + ',' +
                           std::to_string(edit.position.z) + ')';
                notify(engine::voxel::TransactionEvent::Kind::RolledBack, edits.size());
                return false;
            }
            applied.push_back(engine::voxel::BlockEdit{ edit.position, edit.blockId,
                                                        previous });
        }

        undoStack_.push_back(std::move(applied));
        redoStack_.clear();
        editLog_ += edits.size();
        notify(engine::voxel::TransactionEvent::Kind::Committed, edits.size());
        return true;
    }

    // ---- Discrete world lighting (META section 12) ----

    uint8_t get_sky_light(int x, int y, int z) const override {
        return world_.get_sky_light(glm::vec3(static_cast<float>(x),
                                              static_cast<float>(y),
                                              static_cast<float>(z)));
    }

    uint8_t get_block_light(int x, int y, int z) const override {
        return world_.get_block_light(glm::vec3(static_cast<float>(x),
                                                static_cast<float>(y),
                                                static_cast<float>(z)));
    }

    uint8_t get_fluid_level(int x, int y, int z) const override {
        return world_.get_fluid_level_at(glm::vec3(static_cast<float>(x),
                                                   static_cast<float>(y),
                                                   static_cast<float>(z)));
    }

    // ---- Block entities (META section 8) ----

    void register_block_entity_type(
        const std::string& typeId,
        engine::voxel::BlockEntityFactory factory) override {
        world_.register_block_entity_type(typeId, std::move(factory));
    }

    bool attach_block_entity(
        int x, int y, int z, std::shared_ptr<engine::voxel::IVoxelBlockEntity> entity,
        std::string& errorOut) override {
        // Attach shares ownership (the caller keeps a valid handle to observe
        // ticks/state); the world copies the shared_ptr into its map.
        return world_.attach_block_entity(x, y, z, entity, errorOut);
    }

    std::shared_ptr<engine::voxel::IVoxelBlockEntity> block_entity_at(
        int x, int y, int z) const override {
        return world_.block_entity_at(x, y, z);
    }

    bool remove_block_entity(int x, int y, int z) override {
        return world_.remove_block_entity(x, y, z);
    }

    std::size_t block_entity_count() const override {
        return world_.block_entities().size();
    }

    void set_block_entity_listener(
        std::function<void(const engine::voxel::BlockEntityEvent&)> listener) override {
        world_.set_block_entity_listener(std::move(listener));
    }

    // ---- Persistence (META section 10) ----

    std::string serialize_world(std::string& errorOut) override {
        if (storage_) return storage_->serialize_world(errorOut);
        // Drain the worker pool first: boot returns as soon as the center chunk
        // is Uploaded, but other chunks may still be mid-generation. Reading
        // blocks[] while a worker writes it is a data race (UB) — the exact
        // class the streaming fixes removed from the snapshot path.
        world_.threadPool.wait_idle();
        std::lock_guard<std::recursive_mutex> lock(world_.chunksMutex);
        std::vector<std::pair<int, int>> keys;
        for (const auto& [key, chunk] : world_.chunks) {
            if (chunk) keys.push_back(key);
        }
        std::sort(keys.begin(), keys.end());

        // Palette of dynamic runtime ids (>= BlockType::Count) with persistent
        // UUIDs. Builtin ids are implied by the stable enum; the palette lets
        // a loader rebuild ids without depending on JSON load order.
        const std::vector<std::pair<RuntimeBlockId, RuntimeBlockInfo>> palette =
            world_.runtime_block_table();
        // Block entities in deterministic (x,y,z) order. The blob is opaque —
        // the project migrates its own versions via deserialize_state.
        const World::BlockEntityMap& entities = world_.block_entities();
        std::vector<TickCell> entityKeys;
        entityKeys.reserve(entities.size());
        for (const auto& [cell, entity] : entities) {
            if (entity) entityKeys.push_back(cell);
        }
        std::sort(entityKeys.begin(), entityKeys.end(),
                  [](const TickCell& a, const TickCell& b) {
            return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z);
        });

#if VC_ENABLE_FLATBUFFERS
        // v5: the body is a FlatBuffers container (typed, verifiable) instead
        // of the hand-rolled binary layout. Framing is unchanged: magic +
        // version + buffer + BLAKE3, and the file layer still zstd-compresses.
        flatbuffers::FlatBufferBuilder builder(1024);
        std::vector<flatbuffers::Offset<engine::voxel::save::PaletteEntry>>
            paletteOffsets;
        paletteOffsets.reserve(palette.size());
        for (const auto& [id, info] : palette) {
            const auto uuid = builder.CreateString(info.uuid);
            engine::voxel::save::PaletteEntryBuilder entry(builder);
            entry.add_id(static_cast<uint16_t>(id));
            entry.add_uuid(uuid);
            paletteOffsets.push_back(entry.Finish());
        }
        const auto paletteVec = builder.CreateVector(paletteOffsets);

        std::vector<flatbuffers::Offset<engine::voxel::save::ChunkEntry>>
            chunkOffsets;
        chunkOffsets.reserve(keys.size());
        const std::size_t layerBytes =
            static_cast<std::size_t>(CHUNK_SIZE_X) * CHUNK_SIZE_Z;
        for (const auto& [cx, cz] : keys) {
            const Chunk& chunk = *world_.chunks.at({ cx, cz });
            const int extent =
                std::clamp(chunk.vertical_render_extent(), 1, CHUNK_SIZE_Y);
            const std::size_t voxelBytes =
                static_cast<std::size_t>(extent) * layerBytes;
            std::vector<uint16_t> blocks;
            std::vector<uint8_t> fluid;
            blocks.reserve(voxelBytes);
            fluid.reserve(voxelBytes);
            for (int y = 0; y < extent; ++y) {
                for (int z = 0; z < CHUNK_SIZE_Z; ++z) {
                    for (int x = 0; x < CHUNK_SIZE_X; ++x) {
                        blocks.push_back(
                            static_cast<uint16_t>(chunk.get_block(x, y, z)));
                        // Fluid-level byte for ANY fluid (water is the builtin
                        // case; data-driven fluids carry levels the same way).
                        fluid.push_back(chunk.get_fluid_level(x, y, z));
                    }
                }
            }
            const auto blocksVec = builder.CreateVector(blocks);
            const auto fluidVec = builder.CreateVector(fluid);
            engine::voxel::save::ChunkEntryBuilder entry(builder);
            entry.add_cx(cx);
            entry.add_cz(cz);
            entry.add_extent(static_cast<uint32_t>(extent));
            entry.add_blocks(blocksVec);
            entry.add_fluid(fluidVec);
            chunkOffsets.push_back(entry.Finish());
        }
        const auto chunkVec = builder.CreateVector(chunkOffsets);

        std::vector<flatbuffers::Offset<engine::voxel::save::BlockEntityEntry>>
            entityOffsets;
        entityOffsets.reserve(entityKeys.size());
        for (const TickCell& cell : entityKeys) {
            const std::shared_ptr<engine::voxel::IVoxelBlockEntity>& entity =
                entities.at(cell);
            const auto typeId = builder.CreateString(entity->type_id());
            const std::vector<uint8_t> blob = entity->serialize_state();
            const auto blobVec = builder.CreateVector(blob);
            engine::voxel::save::BlockEntityEntryBuilder entry(builder);
            entry.add_x(cell.x);
            entry.add_y(cell.y);
            entry.add_z(cell.z);
            entry.add_type_id(typeId);
            entry.add_data_version(entity->data_version());
            entry.add_blob(blobVec);
            entityOffsets.push_back(entry.Finish());
        }
        const auto entityVec = builder.CreateVector(entityOffsets);

        // World entities (META section 15): the EnTT-backed population, as
        // versioned snapshots (type, position, health, tick policy, project
        // components) in deterministic spawn order.
        const std::vector<engine::entity::EntitySnapshot> snapshots =
            entityWorld_ ? entityWorld_->serialize_entities()
                         : std::vector<engine::entity::EntitySnapshot>{};
        std::vector<flatbuffers::Offset<engine::voxel::save::EntityEntry>>
            worldEntityOffsets;
        worldEntityOffsets.reserve(snapshots.size());
        for (const engine::entity::EntitySnapshot& snapshot : snapshots) {
            const auto type = builder.CreateString(snapshot.type);
            std::vector<flatbuffers::Offset<
                engine::voxel::save::EntityComponentEntry>>
                componentOffsets;
            componentOffsets.reserve(snapshot.components.size());
            for (const engine::entity::ComponentData& component :
                 snapshot.components) {
                const auto compType = builder.CreateString(component.type);
                const std::vector<uint8_t> blob(component.blob.begin(),
                                                component.blob.end());
                const auto blobVec = builder.CreateVector(blob);
                engine::voxel::save::EntityComponentEntryBuilder comp(builder);
                comp.add_type(compType);
                comp.add_version(component.version);
                comp.add_blob(blobVec);
                componentOffsets.push_back(comp.Finish());
            }
            const auto componentVec = builder.CreateVector(componentOffsets);
            engine::voxel::save::EntityEntryBuilder entry(builder);
            entry.add_type(type);
            entry.add_x(snapshot.position.x);
            entry.add_y(snapshot.position.y);
            entry.add_z(snapshot.position.z);
            entry.add_health(snapshot.health.value);
            entry.add_max_health(snapshot.health.max);
            entry.add_tick_interval(snapshot.tickInterval);
            entry.add_components(componentVec);
            worldEntityOffsets.push_back(entry.Finish());
        }
        const auto worldEntityVec = builder.CreateVector(worldEntityOffsets);

        engine::voxel::save::WorldSaveBuilder save(builder);
        save.add_palette(paletteVec);
        save.add_chunks(chunkVec);
        save.add_entities(entityVec);
        save.add_world_entities(worldEntityVec);
        builder.Finish(save.Finish(), "WLD5");

        std::string body;
        body.append(kWorldMagic, 5);
        append_u32(body, kWorldSaveVersion);
        body.append(reinterpret_cast<const char*>(builder.GetBufferPointer()),
                    builder.GetSize());
        // BLAKE3-256 digest of the whole body (32 bytes).
        append(body, hash_->hash(body));
        return body;
#else
        // Fallback when FlatBuffers is disabled: the v4 manual binary body.
        // Framing and checksum are identical to v4 saves (which still load).
        std::string body;
        body.append(kWorldMagic, 5);
        append_u32(body, kWorldSaveVersionV4);
        append_u32(body, static_cast<uint32_t>(palette.size()));
        for (const auto& [id, info] : palette) {
            append_u16(body, id);
            append_u32(body, static_cast<uint32_t>(info.uuid.size()));
            body.append(info.uuid);
        }
        append_u32(body, static_cast<uint32_t>(keys.size()));
        for (const auto& [cx, cz] : keys) {
            const Chunk& chunk = *world_.chunks.at({ cx, cz });
            const int extent =
                std::clamp(chunk.vertical_render_extent(), 1, CHUNK_SIZE_Y);
            append_i32(body, cx);
            append_i32(body, cz);
            append_u32(body, static_cast<uint32_t>(extent));
            for (int y = 0; y < extent; ++y) {
                for (int z = 0; z < CHUNK_SIZE_Z; ++z) {
                    for (int x = 0; x < CHUNK_SIZE_X; ++x) {
                        append_u16(body, chunk.get_block(x, y, z));
                    }
                }
            }
            for (int y = 0; y < extent; ++y) {
                for (int z = 0; z < CHUNK_SIZE_Z; ++z) {
                    for (int x = 0; x < CHUNK_SIZE_X; ++x) {
                        body.push_back(static_cast<char>(
                            chunk.get_fluid_level(x, y, z)));
                    }
                }
            }
        }
        append_u32(body, static_cast<uint32_t>(entityKeys.size()));
        for (const TickCell& cell : entityKeys) {
            const std::shared_ptr<engine::voxel::IVoxelBlockEntity>& entity =
                entities.at(cell);
            append_i32(body, cell.x);
            append_i32(body, cell.y);
            append_i32(body, cell.z);
            const std::string& typeId = entity->type_id();
            append_u32(body, static_cast<uint32_t>(typeId.size()));
            body.append(typeId);
            append_u32(body, entity->data_version());
            const std::vector<uint8_t> blob = entity->serialize_state();
            append_u32(body, static_cast<uint32_t>(blob.size()));
            if (!blob.empty()) {
                body.append(reinterpret_cast<const char*>(blob.data()), blob.size());
            }
        }
        append(body, hash_->hash(body));
        return body;
#endif
    }

    bool deserialize_world(const std::string& data, std::string& errorOut) override {
        if (storage_) return storage_->deserialize_world(data, errorOut);
        if (data.size() < 5 + 4 + 8) {
            errorOut = "world save too small";
            return false;
        }
        if (data.compare(0, 5, kWorldMagic) != 0) {
            errorOut = "not a world save (bad magic)";
            return false;
        }
        const uint32_t version = read_u32(data, 5);
        if (version != kWorldSaveVersionV1 && version != kWorldSaveVersionV2 &&
            version != kWorldSaveVersionV3 && version != kWorldSaveVersionV4 &&
            version != kWorldSaveVersion) {
            errorOut = "unsupported world schema version " +
                       std::to_string(version);
            return false;
        }
        // v4+ verifies a BLAKE3 digest (32 bytes); v1-v3 kept the FNV-1a u64.
        const bool wideChecksum = version >= kWorldSaveVersionV4;
        const std::size_t checksumLen = wideChecksum ? 32 : 8;
        if (data.size() < 5 + 4 + checksumLen) {
            errorOut = "world save too small";
            return false;
        }
        const std::string body = data.substr(0, data.size() - checksumLen);
        if (wideChecksum) {
            const std::string stored = data.substr(data.size() - 32);
            if (hash_->hash(body) != stored) {
                errorOut = "world save corrupt (BLAKE3 checksum mismatch)";
                return false;
            }
        } else if (fnv1a_bytes(body) != read_u64(data, data.size() - 8)) {
            errorOut = "world save corrupt (checksum mismatch)";
            return false;
        }
        // v5 body is a FlatBuffers container; v1-v4 use the manual parser.
        if (version >= kWorldSaveVersion) {
            return deserialize_world_v5(body, errorOut);
        }
        const bool hasPalette = version >= kWorldSaveVersionV2;
        // Entity section exists since v3; v1/v2 saves predate it.
        const bool hasBlockEntities = version >= kWorldSaveVersionV3;
        const bool wideBlockIds = version >= kWorldSaveVersionV2;

        std::size_t cursor = 9;
        if (hasPalette) {
            // Palette: (u16 runtimeId, u32 uuidLen, uuid bytes) per dynamic
            // block. Verify each uuid maps back to the SAME runtime id in the
            // current registry-derived table — a registry change is a clear
            // diagnostic, never silent Air.
            if (cursor + 4 > body.size()) {
                errorOut = "world save truncated (palette)";
                return false;
            }
            const uint32_t paletteCount = read_u32(body, cursor);
            cursor += 4;
            for (uint32_t index = 0; index < paletteCount; ++index) {
                if (cursor + 2 + 4 > body.size()) {
                    errorOut = "world save truncated (palette entry " +
                               std::to_string(index) + ")";
                    return false;
                }
                const RuntimeBlockId storedId = read_u16(body, cursor);
                const uint32_t uuidLen = read_u32(body, cursor + 2);
                cursor += 6;
                if (cursor + uuidLen > body.size()) {
                    errorOut = "world save truncated (palette uuid " +
                               std::to_string(index) + ")";
                    return false;
                }
                const std::string uuid = body.substr(cursor, uuidLen);
                cursor += uuidLen;
                const std::optional<RuntimeBlockId> expected =
                    world_.runtime_block_id_for_uuid(uuid);
                if (!expected || *expected != storedId) {
                    errorOut = "world save references block uuid '" + uuid +
                               "' not mapped by the current registry (id " +
                               std::to_string(storedId) + ")";
                    return false;
                }
            }
        }

        if (cursor + 4 > body.size()) {
            errorOut = "world save truncated (chunk table)";
            return false;
        }
        const uint32_t chunkCount = read_u32(body, cursor);
        cursor += 4;
        const std::size_t layerBytes =
            static_cast<std::size_t>(CHUNK_SIZE_X) * CHUNK_SIZE_Z;
        std::size_t restored = 0;
        for (uint32_t index = 0; index < chunkCount; ++index) {
            if (cursor + 4 + 4 + 4 > body.size()) {
                errorOut = "world save truncated (chunk " +
                           std::to_string(index) + ")";
                return false;
            }
            const int32_t cx = static_cast<int32_t>(read_u32(body, cursor));
            const int32_t cz = static_cast<int32_t>(read_u32(body, cursor + 4));
            const uint32_t extent = read_u32(body, cursor + 8);
            cursor += 12;
            const std::size_t voxelBytes = static_cast<std::size_t>(extent) * layerBytes;
            const std::size_t blockBytes = voxelBytes * (wideBlockIds ? 2 : 1);
            if (extent == 0 || extent > static_cast<uint32_t>(CHUNK_SIZE_Y) ||
                cursor + blockBytes + voxelBytes > body.size()) {
                errorOut = "world save corrupt (bad extent on chunk " +
                           std::to_string(index) + ")";
                return false;
            }

            // Parse block ids (u16 in v2+, u8 builtins in v1) into runtime ids.
            std::vector<RuntimeBlockId> blockIds;
            blockIds.reserve(extent * layerBytes);
            for (std::size_t i = 0; i < voxelBytes; ++i) {
                const std::size_t offset = cursor + (wideBlockIds ? i * 2 : i);
                const RuntimeBlockId id = wideBlockIds
                    ? read_u16(body, offset) : static_cast<uint8_t>(body[offset]);
                if (!world_.is_valid_block_id(id)) {
                    errorOut = "world restore: chunk (" + std::to_string(cx) + ',' +
                               std::to_string(cz) + ") contains unknown block id " +
                               std::to_string(id);
                    return false;
                }
                blockIds.push_back(id);
            }
            const std::string water =
                body.substr(cursor + blockBytes, voxelBytes);
            cursor += blockBytes + voxelBytes;

            if (!apply_saved_chunk(cx, cz, extent, blockIds, water)) {
                errorOut = "world restore: chunk (" + std::to_string(cx) + ',' +
                           std::to_string(cz) + ") could not be loaded";
                return false;
            }
            ++restored;
        }

        // Block entity section (v3): reconstruct each entity through its
        // registered factory and hand it its opaque, versioned blob. A type
        // with no factory or data the entity refuses is a hard diagnostic —
        // the world would be inconsistent without the entity's state.
        if (hasBlockEntities) {
            if (cursor + 4 > body.size()) {
                errorOut = "world save truncated (block entities)";
                return false;
            }
            const uint32_t entityCount = read_u32(body, cursor);
            cursor += 4;
            for (uint32_t index = 0; index < entityCount; ++index) {
                if (cursor + 12 > body.size()) {
                    errorOut = "world save truncated (block entity " +
                               std::to_string(index) + ")";
                    return false;
                }
                const int32_t ex = static_cast<int32_t>(read_u32(body, cursor));
                const int32_t ey = static_cast<int32_t>(read_u32(body, cursor + 4));
                const int32_t ez = static_cast<int32_t>(read_u32(body, cursor + 8));
                cursor += 12;
                if (cursor + 4 > body.size()) {
                    errorOut = "world save truncated (block entity type " +
                               std::to_string(index) + ")";
                    return false;
                }
                const uint32_t typeLen = read_u32(body, cursor);
                cursor += 4;
                if (cursor + typeLen > body.size()) {
                    errorOut = "world save truncated (block entity type " +
                               std::to_string(index) + ")";
                    return false;
                }
                const std::string typeId = body.substr(cursor, typeLen);
                cursor += typeLen;
                if (cursor + 4 > body.size()) {
                    errorOut = "world save truncated (block entity data " +
                               std::to_string(index) + ")";
                    return false;
                }
                const uint32_t dataVersion = read_u32(body, cursor);
                cursor += 4;
                if (cursor + 4 > body.size()) {
                    errorOut = "world save truncated (block entity blob " +
                               std::to_string(index) + ")";
                    return false;
                }
                const uint32_t blobLen = read_u32(body, cursor);
                cursor += 4;
                if (cursor + blobLen > body.size()) {
                    errorOut = "world save truncated (block entity blob " +
                               std::to_string(index) + ")";
                    return false;
                }
                const std::vector<uint8_t> blob(
                    body.begin() + static_cast<std::ptrdiff_t>(cursor),
                    body.begin() + static_cast<std::ptrdiff_t>(cursor + blobLen));
                cursor += blobLen;

                const engine::voxel::BlockEntityFactory factory =
                    world_.find_block_entity_factory(typeId);
                if (!factory) {
                    errorOut = "world save references unknown block entity type '" +
                               typeId + "' (register its factory)";
                    return false;
                }
                std::shared_ptr<engine::voxel::IVoxelBlockEntity> entity = factory();
                if (!entity || !entity->deserialize_state(blob, dataVersion)) {
                    errorOut = "world save: block entity '" + typeId +
                               "' refused its data (version " +
                               std::to_string(dataVersion) + ")";
                    return false;
                }
                world_.restore_block_entity(ex, ey, ez, std::move(entity));
            }
        }
        errorOut.clear();
        return true;
    }

#if VC_ENABLE_FLATBUFFERS
    // v5 body: "VCWLD" + u32 version(5) + FlatBuffers container. The buffer is
    // validated with the generated verifier BEFORE any accessor is read; the
    // schema's own identifier is checked as an extra integrity layer.
    bool deserialize_world_v5(const std::string& body, std::string& errorOut) {
        const std::size_t prefix = 5 + 4;
        if (body.size() < prefix + 4) {
            errorOut = "world save too small (v5)";
            return false;
        }
        const char* buffer = body.data() + prefix;
        const std::size_t bufferSize = body.size() - prefix;
        flatbuffers::Verifier verifier(
            reinterpret_cast<const uint8_t*>(buffer), bufferSize);
        if (!engine::voxel::save::VerifyWorldSaveBuffer(verifier)) {
            errorOut = "world save corrupt (FlatBuffers verification failed)";
            return false;
        }
        const engine::voxel::save::WorldSave* save =
            engine::voxel::save::GetWorldSave(buffer);
        if (!save) {
            errorOut = "world save corrupt (no root table)";
            return false;
        }

        // Palette: verify each uuid maps back to the SAME runtime id in the
        // current registry-derived table — a registry change is a clear
        // diagnostic, never silent Air (same contract as v2-v4).
        if (const auto* palette = save->palette(); palette) {
            for (flatbuffers::uoffset_t index = 0; index < palette->size();
                 ++index) {
                const engine::voxel::save::PaletteEntry* entry =
                    palette->Get(index);
                const RuntimeBlockId storedId =
                    static_cast<RuntimeBlockId>(entry->id());
                const std::string uuid = entry->uuid()->str();
                const std::optional<RuntimeBlockId> expected =
                    world_.runtime_block_id_for_uuid(uuid);
                if (!expected || *expected != storedId) {
                    errorOut = "world save references block uuid '" + uuid +
                               "' not mapped by the current registry (id " +
                               std::to_string(storedId) + ")";
                    return false;
                }
            }
        }

        const std::size_t layerBytes =
            static_cast<std::size_t>(CHUNK_SIZE_X) * CHUNK_SIZE_Z;
        std::size_t restored = 0;
        if (const auto* chunks = save->chunks(); chunks) {
            for (flatbuffers::uoffset_t index = 0; index < chunks->size();
                 ++index) {
                const engine::voxel::save::ChunkEntry* entry = chunks->Get(index);
                const int32_t cx = entry->cx();
                const int32_t cz = entry->cz();
                const uint32_t extent = entry->extent();
                if (extent == 0 || extent > static_cast<uint32_t>(CHUNK_SIZE_Y) ||
                    entry->blocks() == nullptr || entry->fluid() == nullptr ||
                    entry->blocks()->size() !=
                        static_cast<std::size_t>(extent) * layerBytes ||
                    entry->fluid()->size() !=
                        static_cast<std::size_t>(extent) * layerBytes) {
                    errorOut = "world save corrupt (bad extent on chunk " +
                               std::to_string(index) + ")";
                    return false;
                }
                const std::size_t voxelBytes =
                    static_cast<std::size_t>(extent) * layerBytes;
                std::vector<RuntimeBlockId> blockIds;
                blockIds.reserve(voxelBytes);
                for (std::size_t i = 0; i < voxelBytes; ++i) {
                    const RuntimeBlockId id = static_cast<RuntimeBlockId>(
                        entry->blocks()->Get(static_cast<flatbuffers::uoffset_t>(i)));
                    if (!world_.is_valid_block_id(id)) {
                        errorOut = "world restore: chunk (" +
                                   std::to_string(cx) + ',' +
                                   std::to_string(cz) +
                                   ") contains unknown block id " +
                                   std::to_string(id);
                        return false;
                    }
                    blockIds.push_back(id);
                }
                std::string water;
                water.reserve(voxelBytes);
                for (std::size_t i = 0; i < voxelBytes; ++i) {
                    water.push_back(static_cast<char>(entry->fluid()->Get(
                        static_cast<flatbuffers::uoffset_t>(i))));
                }
                if (!apply_saved_chunk(cx, cz, static_cast<int>(extent),
                                       blockIds, water)) {
                    errorOut = "world restore: chunk (" + std::to_string(cx) +
                               ',' + std::to_string(cz) +
                               ") could not be loaded";
                    return false;
                }
                ++restored;
            }
        }

        if (const auto* entities = save->entities(); entities) {
            for (flatbuffers::uoffset_t index = 0; index < entities->size();
                 ++index) {
                const engine::voxel::save::BlockEntityEntry* entry =
                    entities->Get(index);
                const int32_t ex = entry->x();
                const int32_t ey = entry->y();
                const int32_t ez = entry->z();
                const std::string typeId = entry->type_id()->str();
                const engine::voxel::BlockEntityFactory factory =
                    world_.find_block_entity_factory(typeId);
                if (!factory) {
                    errorOut = "world save references unknown block entity type '" +
                               typeId + "' (register its factory)";
                    return false;
                }
                std::vector<uint8_t> blob;
                if (entry->blob() != nullptr) {
                    blob.reserve(entry->blob()->size());
                    for (flatbuffers::uoffset_t i = 0; i < entry->blob()->size();
                         ++i) {
                        blob.push_back(entry->blob()->Get(i));
                    }
                }
                std::shared_ptr<engine::voxel::IVoxelBlockEntity> entity = factory();
                if (!entity || !entity->deserialize_state(blob, entry->data_version())) {
                    errorOut = "world save: block entity '" + typeId +
                               "' refused its data (version " +
                               std::to_string(entry->data_version()) + ")";
                    return false;
                }
                world_.restore_block_entity(ex, ey, ez, std::move(entity));
            }
        }

        // World entities (META section 15): optional field — v5 buffers written
        // before the addition simply have no entities (null == none). Restore
        // all-or-nothing: the entity world validates every snapshot before
        // clearing its current population.
        if (save->world_entities() != nullptr && entityWorld_) {
            std::vector<engine::entity::EntitySnapshot> snapshots;
            snapshots.reserve(save->world_entities()->size());
            for (flatbuffers::uoffset_t index = 0;
                 index < save->world_entities()->size(); ++index) {
                const engine::voxel::save::EntityEntry* entry =
                    save->world_entities()->Get(index);
                engine::entity::EntitySnapshot snapshot;
                snapshot.type = entry->type()->str();
                snapshot.position.x = entry->x();
                snapshot.position.y = entry->y();
                snapshot.position.z = entry->z();
                snapshot.health.value = entry->health();
                snapshot.health.max = entry->max_health();
                snapshot.tickInterval = entry->tick_interval();
                if (entry->components() != nullptr) {
                    snapshot.components.reserve(entry->components()->size());
                    for (flatbuffers::uoffset_t c = 0;
                         c < entry->components()->size(); ++c) {
                        const engine::voxel::save::EntityComponentEntry* comp =
                            entry->components()->Get(c);
                        engine::entity::ComponentData component;
                        component.type = comp->type()->str();
                        component.version = comp->version();
                        if (comp->blob() != nullptr) {
                            component.blob.reserve(comp->blob()->size());
                            for (flatbuffers::uoffset_t b = 0;
                                 b < comp->blob()->size(); ++b) {
                                component.blob.push_back(
                                    static_cast<char>(comp->blob()->Get(b)));
                            }
                        }
                        snapshot.components.push_back(std::move(component));
                    }
                }
                snapshots.push_back(std::move(snapshot));
            }
            std::string entityError;
            if (!entityWorld_->deserialize_entities(snapshots, entityError)) {
                errorOut = "world save: entity restore failed: " + entityError;
                return false;
            }
        }
        errorOut.clear();
        return true;
    }
#endif

    bool save_world(const std::string& filePath, std::string& errorOut) override {
        if (storage_) return storage_->save_world(filePath, errorOut);
        const std::string data = serialize_world(errorOut);
        if (!errorOut.empty()) return false;
        // v4 file layer (META section 32): the serialized body is written as a
        // Zstandard frame. A failed compression falls back to the raw bytes
        // (still loadable — the loader detects the frame magic).
        const std::string frame = compression_ ? compression_->compress(data)
                                               : std::string();
        const std::string& payload = (!frame.empty()) ? frame : data;
        // Atomic-ish write: temp file + rename, so an interrupted save never
        // corrupts the last valid save.
        const std::string tmpPath = filePath + ".tmp";
        std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            errorOut = "cannot open " + tmpPath + " for writing";
            return false;
        }
        out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        out.close();
        if (!out) {
            errorOut = "write to " + tmpPath + " failed";
            return false;
        }
        if (std::rename(tmpPath.c_str(), filePath.c_str()) != 0) {
            // Windows std::rename does not replace an existing destination.
            // The new content is fully written to the temp file; removing the
            // old save and retrying keeps repeated saves to the same slot safe.
            if (std::remove(filePath.c_str()) == 0 &&
                std::rename(tmpPath.c_str(), filePath.c_str()) == 0) {
                errorOut.clear();
                return true;
            }
            errorOut = "cannot move " + tmpPath + " to " + filePath;
            return false;
        }
        errorOut.clear();
        return true;
    }

    bool load_world(const std::string& filePath, std::string& errorOut) override {
        if (storage_) return storage_->load_world(filePath, errorOut);
        std::ifstream in(filePath, std::ios::binary);
        if (!in) {
            errorOut = "cannot open " + filePath + " for reading";
            return false;
        }
        std::ostringstream buffer;
        buffer << in.rdbuf();
        const std::string fileBytes = buffer.str();
        // v4 saves are a zstd frame; legacy v1-v3 files are the raw body.
        if (compression_ && compression_->is_compressed(fileBytes)) {
            const std::string body = compression_->decompress(fileBytes);
            if (body.empty()) {
                errorOut = "world save corrupt (cannot decompress zstd frame)";
                return false;
            }
            return deserialize_world(body, errorOut);
        }
        return deserialize_world(fileBytes, errorOut);
    }

    void update(const glm::vec3& playerPosition, float deltaTime) override {
        playerPos_ = playerPosition;
        world_.update(playerPosition, bridge_, deltaTime);
    }

private:
    // Drives the world until the chunk is loaded (near the last update
    // position), then overwrites its voxel content with the saved state.
    bool apply_saved_chunk(int cx, int cz, int extent,
                           const std::vector<RuntimeBlockId>& blocks,
                           const std::string& water) {
        const glm::vec3 center(static_cast<float>(cx) * CHUNK_SIZE_X + 8.0f, 40.0f,
                               static_cast<float>(cz) * CHUNK_SIZE_Z + 8.0f);
        for (int frame = 0; frame < 600 && !world_.is_chunk_loaded_at(center); ++frame) {
            world_.update(playerPos_, bridge_, 1.0f / 60.0f);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (!world_.is_chunk_loaded_at(center)) return false;

        std::size_t index = 0;
        for (int y = 0; y < extent; ++y) {
            for (int z = 0; z < CHUNK_SIZE_Z; ++z) {
                for (int x = 0; x < CHUNK_SIZE_X; ++x) {
                    const glm::vec3 position(
                        static_cast<float>(cx * CHUNK_SIZE_X + x), static_cast<float>(y),
                        static_cast<float>(cz * CHUNK_SIZE_Z + z));
                    const RuntimeBlockId type = blocks[index];
                    const uint8_t level =
                        static_cast<uint8_t>(water[index]);
                    ++index;
                    world_.set_block_at(position, type);
                    // Any fluid block with a stored level restores it (the
                    // water special case generalized to data-driven fluids).
                    if (level != WATER_LEVEL_NONE &&
                        world_.is_fluid_runtime_id(type)) {
                        world_.set_fluid_level_at(position, level);
                    }
                }
            }
        }
        return true;
    }
    void notify(engine::voxel::TransactionEvent::Kind kind, std::size_t editCount) {
        if (!listener_) return;
        engine::voxel::TransactionEvent event;
        event.kind = kind;
        event.editCount = editCount;
        event.undoDepth = undoStack_.size();
        listener_(event);
    }

    SoundEngine audio_;
    MobManager mobs_;
    World world_;
    NullBridge bridge_;
    glm::vec3 playerPos_{ 8.0f, 200.0f, 8.0f };

    // Registry-driven ids: the source of truth for settable block ids.
    std::shared_ptr<const engine::registry::BlockRegistry> registry_;
    // Data-driven fluid behavior (META section 13): definitions attach fluid
    // parameters to registered blocks; the world table is derived on attach.
    std::shared_ptr<const engine::registry::FluidRegistry> fluidRegistry_;
    // Promoted solutions (META section 32): Zstandard compresses the save
    // file; BLAKE3 hashes the v4 body. Behind the public interfaces, so the
    // external headers never reach the API boundary.
    std::shared_ptr<engine::compression::ICompressionProvider> compression_;
    std::shared_ptr<engine::hashing::IHashProvider> hash_;
    // Entity layer (META section 15): spatially indexed by chunk, persists
    // through the world save (v5 world_entities). Default: EnTT-backed;
    // replaceable via register_entity_world.
    std::shared_ptr<engine::entity::IEntityWorld> entityWorld_;
    // Optional service overrides (see IVoxelServices.hpp for wiring status).
    std::shared_ptr<engine::voxel::IChunkStorage> storage_;
    std::shared_ptr<engine::voxel::IVoxelMesher> mesher_;
    std::shared_ptr<engine::voxel::IVoxelLighting> lighting_;
    std::shared_ptr<engine::voxel::IVoxelFluidSimulation> fluid_;
    std::shared_ptr<engine::voxel::IVoxelReplication> replication_;

    // Transaction state: applied edit lists (undo), undone lists (redo), the
    // committed-edit log counter and the optional event listener.
    std::vector<std::vector<engine::voxel::BlockEdit>> undoStack_;
    std::vector<std::vector<engine::voxel::BlockEdit>> redoStack_;
    std::size_t editLog_{ 0 };
    std::function<void(const engine::voxel::TransactionEvent&)> listener_;
};

// Collects edits and hands them to the facade's single mutation path on
// commit. Rolling back (undo) reapplies the previous block ids.
class VoxelTransactionImpl final : public engine::voxel::IVoxelTransaction {
public:
    explicit VoxelTransactionImpl(VoxelWorldFacade& facade) : facade_(facade) {}

    void set_block(int x, int y, int z, uint32_t blockId) override {
        if (done_) return;
        edits_.push_back(engine::voxel::BlockEdit{ { x, y, z }, blockId, 0 });
    }

    void remove_block(int x, int y, int z) override { set_block(x, y, z, 0); }

    bool commit(std::string& errorOut) override {
        if (done_) {
            errorOut = "transaction already committed or rolled back";
            return false;
        }
        done_ = true;
        return facade_.apply_edits(std::move(edits_), errorOut);
    }

    void rollback() override {
        done_ = true;
        edits_.clear();
    }

    std::size_t edit_count() const override { return edits_.size(); }

private:
    VoxelWorldFacade& facade_;
    std::vector<engine::voxel::BlockEdit> edits_;
    bool done_{ false };
};

std::unique_ptr<engine::voxel::IVoxelTransaction> VoxelWorldFacade::begin_transaction() {
    return std::make_unique<VoxelTransactionImpl>(*this);
}

}  // namespace

namespace engine {
namespace voxel {

std::unique_ptr<IVoxelWorld> create_default_voxel_world() {
    return std::make_unique<VoxelWorldFacade>();
}

}  // namespace voxel
}  // namespace engine
