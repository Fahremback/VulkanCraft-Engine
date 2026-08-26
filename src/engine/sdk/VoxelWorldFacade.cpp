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

#include "engine/entity/IMobBehavior.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <set>
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
        // Per-face material + occlusion + render layer (FALTANTES §14).
        info.faceTop = definition.faceTop;
        info.faceBottom = definition.faceBottom;
        info.faceSide = definition.faceSide;
        info.faceTopSet = definition.faceTopSet;
        info.faceBottomSet = definition.faceBottomSet;
        info.faceSideSet = definition.faceSideSet;
        info.occludes = definition.occludes;
        info.renderLayer = static_cast<uint8_t>(definition.renderLayer & 0xFF);
        // Collision/selection shapes (FALTANTES item 2): None always wins over
        // the legacy collidable bool — a ghost block is never solid for the
        // voxel raycast. Cross stays solid at cell granularity here; the thin
        // hitbox belongs to the physics milestone.
        info.solid = definition.is_collidable();
        info.collisionShape = static_cast<uint8_t>(definition.collisionShape);
        info.selectionShape = static_cast<uint8_t>(definition.selectionShape);
        info.transparent = definition.blockClass == engine::registry::BlockClass::Transparent;
        info.fluid = definition.blockClass == engine::registry::BlockClass::Fluid;
        // Tool/physics component (FALTANTES item 4): mirror for the gameplay/
        // physics milestones (tool gating, destruction scaling, dynamic bodies).
        info.tool = static_cast<uint8_t>(definition.tool);
        info.toolTier = static_cast<uint8_t>(definition.toolTier);
        info.resistance = definition.resistance;
        info.friction = definition.friction;
        info.bounciness = definition.bounciness;
        info.density = definition.density;
        info.flammability = definition.flammability;
        info.soundPlace = definition.soundPlace;
        info.soundBreak = definition.soundBreak;
        info.soundStep = definition.soundStep;
        info.soundHit = definition.soundHit;
        info.particleBreak = definition.particleBreak;
        info.behaviorId = definition.behaviorId;
        // Discrete lighting (META section 12): JSON floats (0..1) scale to
        // light levels (0..15). Absorption 15 = opaque (default derives from
        // solidity/transparency below).
        info.lightEmission = static_cast<uint8_t>(std::min<int>(
            15, static_cast<int>(definition.lightEmission * 15.0f + 0.5f)));
        // JSON float (0..1) scales to 0..15; 1.0 (default) = opaque. A
        // transparent block declares e.g. 0.0 to let light through.
        info.lightAbsorption = static_cast<uint8_t>(std::min<int>(
            15, static_cast<int>(definition.lightAbsorption * 15.0f + 0.5f)));
        // Named states (FALTANTES item 5): mirror per-state material + light.
        info.states.reserve(definition.states.size());
        for (const engine::registry::BlockState& state : definition.states) {
            RuntimeBlockInfo::RuntimeBlockState mirror;
            mirror.name = state.name;
            mirror.color = state.color;
            mirror.faceTop = state.faceTop;
            mirror.faceBottom = state.faceBottom;
            mirror.faceSide = state.faceSide;
            mirror.faceTopSet = state.faceTopSet;
            mirror.faceBottomSet = state.faceBottomSet;
            mirror.faceSideSet = state.faceSideSet;
            mirror.lightEmission = static_cast<uint8_t>(std::min<int>(
                15, static_cast<int>(state.lightEmission * 15.0f + 0.5f)));
            info.states.push_back(std::move(mirror));
        }
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

    // FALTANTES item 7: a catalog-only block that declares an inline fluid
    // binding drives the simulation directly — no separate FluidRegistry asset
    // needed. Runs even with no fluid registry. An explicit registry
    // definition for the same block overwrites the inline entry below.
    for (const engine::registry::BlockDefinition& block : blocks.all_definitions()) {
        if (block.hasBuiltinMapping || !block.fluid.declared) continue;
        const std::optional<RuntimeBlockId> dynamic =
            world.runtime_block_id_for_uuid(block.uuid);
        if (!dynamic) continue;  // block not attached yet; attach pass re-runs
        FluidParams params;
        params.viscosity = block.fluid.viscosity;
        params.density = block.fluid.density;
        params.maxLevel = std::clamp(block.fluid.range, 1, 7);
        params.levelsPerTick = block.fluid.viscosity <= 0.25f ? 2 : 1;
        params.tickEveryTicks = std::max(1, static_cast<int>(std::lround(
            block.fluid.tickInterval / World::kFluidTickSeconds)));
        params.source = block.fluid.source;
        params.falling = block.fluid.falling;
        params.evaporation = block.fluid.evaporation;
        params.damagePerTick = block.fluid.damagePerTick;
        params.color = block.color;  // the block's data-driven base color
        params.compressible = block.fluid.compressible;
        tableOut[*dynamic] = params;
    }

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

// Region-paged saves (FALTANTES §4 item 1): each region tile covers
// kRegionChunks x kRegionChunks world chunks (8 -> 128x128 blocks), persisted
// as its own page by a region-capable IChunkStorage.
constexpr int kRegionChunks = 8;
constexpr char kRegionMagic[5] = "VCWR";
// Layout v2 (FALTANTES §4 item 2): per-chunk palette (u8 indices when a chunk
// has <= 256 distinct blocks) + per-chunk zstd of the payload. v1 (raw u16
// ids) stays decodable via its magic.
constexpr char kRegionMagicV2[5] = "VCW2";
// Region page chunk flags (layout v2).
constexpr uint8_t kRegionFlagPalette = 0x01;
constexpr uint8_t kRegionFlagCompressed = 0x02;

// Page id for a region tile ("r.<x>.<z>") — the id the region backend stores.
std::string region_page_id(int regionX, int regionZ) {
    return "r." + std::to_string(regionX) + "." + std::to_string(regionZ);
}

// Integer division toward -infinity (region tiles span negative chunk coords).
int floor_div(int value, int divisor) {
    int q = value / divisor;
    if ((value % divisor) != 0 && ((value < 0) != (divisor < 0))) --q;
    return q;
}

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

// Completion state of one async save/load (FALTANTES §4 item 5). Jobs on the
// world's thread pool share this; the last one to finish records the result
// and runs the completion (clears the in-flight slot, wakes waiters, invokes
// the optional callback).
struct AsyncOpState {
    std::mutex mutex;
    std::condition_variable cv;
    int pending = 0;
    bool ok = true;
    std::string error;
    bool finished = false;
    std::function<void(bool, std::string)> onDone;
};

// Frozen per-chunk data for a save (FALTANTES §4 item 4). Captured under the
// chunk mutex (a brief copy), then grouped/encoded/written WITHOUT holding it:
// the slow parts of a save (palette, zstd, file I/O) never block the world.
struct ChunkSaveSnapshot {
    int cx = 0;
    int cz = 0;
    int extent = 0;
    uint64_t revision = 0;  // dataVersion at capture (delta gating)
    std::vector<RuntimeBlockId> blocks;
    std::vector<uint8_t> fluid;
    std::vector<uint8_t> stateIndices;  // per-voxel state (FALTANTES item 2)
};

// Neutral world-save content (FALTANTES §4 item 9, schema migration): the one
// in-memory form BOTH the live serializer (from the world) and the legacy
// migration path (from a v1-v4 parse) build, then emit at the CURRENT schema
// version through the same emitter — serialize and migrate can never drift.
// Palette = (runtime id, persistent uuid) pairs; chunks carry the block ids
// and fluid bytes in y,z,x order; block entities carry the project's opaque
// blob (migration passes it through untouched — the load of the migrated save
// does the factory/state validation, exactly as it would for the legacy one).
struct WorldSaveData {
    struct Chunk {
        int cx = 0;
        int cz = 0;
        uint32_t extent = 0;
        std::vector<uint16_t> blocks;  // y,z,x order
        std::vector<uint8_t> fluid;    // one byte per voxel
        // Per-voxel block state index (FALTANTES item 2): 0 = default; >0 = named.
        std::vector<uint8_t> stateIndices;  // y,z,x order, one byte per voxel
    };
    struct BlockEntity {
        int x = 0;
        int y = 0;
        int z = 0;
        std::string typeId;
        uint32_t dataVersion = 0;
        std::vector<uint8_t> blob;
    };
    std::vector<std::pair<RuntimeBlockId, std::string>> palette;
    std::vector<Chunk> chunks;
    std::vector<BlockEntity> blockEntities;
    std::vector<engine::entity::EntitySnapshot> worldEntities;
    // Opaque scheduler capture (FALTANTES §6 item 129). The LIVE serializer
    // (serialize_world) captures the world's scheduler; migrate of a v1-v4
    // save leaves it empty (legacy saves have no scheduler state and the
    // migrated save starts a fresh clock).
    std::vector<std::byte> schedulerState;
    // World identity + content provenance (FALTANTES §4 item 4): seed, world
    // name, rules JSON, plugin versions and the block-registry fingerprint at
    // save time. The LIVE serializer captures the facade's metadata and the
    // registry stamp; a migrated v1-v4 save carries the migrating world's
    // metadata (the migrated bytes are emitted by this world).
    uint64_t seed{ 0 };
    std::string worldName;
    std::string rulesJson;
    std::vector<std::pair<std::string, std::string>> pluginVersions;
    uint64_t registryVersion{ 0 };
};

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
// world/registry/entity state so the returned IVoxelWorld is fully
// self-contained. All mutations (transactional or the single-edit convenience)
// flow through apply_edits — the single mutation path of the public contract.
class VoxelWorldFacade final : public engine::voxel::IVoxelWorld,
                               public engine::voxel::IVoxelEditService,
                               public engine::entity::IMobWorldQuery {
public:
    VoxelWorldFacade()
        : world_(),
          registry_(std::make_shared<engine::registry::BlockRegistry>()),
          compression_(engine::compression::create_zstd_compression_provider()),
          hash_(engine::hashing::create_blake3_hash_provider()),
          entityWorld_(engine::entity::create_entity_world()) {
        // The world's dynamic block table comes from the attached registry
        // (empty dynamic set with the builtin default; catalog blocks enter via
        // set_block_registry before boot). The fluid table starts with the
        // engine defaults (water + lava) and is rebuilt whenever a registry is
        // attached.
        push_runtime_table();
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

    // ---- IMobWorldQuery (FALTANTES item 11): the minimal world view the
    // public mob behavior needs. Mob spawning/advancement moved out of the
    // simulation World to the entity layer; these queries host it.
    uint32_t block_at(int x, int y, int z) const override {
        return static_cast<uint32_t>(world_.get_block_at(glm::vec3(
            static_cast<float>(x), static_cast<float>(y),
            static_cast<float>(z))));
    }

    bool is_fluid_block_at(int x, int y, int z) const override {
        return world_.is_fluid_block_at(glm::vec3(
            static_cast<float>(x), static_cast<float>(y),
            static_cast<float>(z)));
    }

    float fluid_damage_per_second_at(int x, int y, int z) const override {
        const FluidParams* params = world_.fluid_params_at(glm::ivec3(x, y, z));
        return params != nullptr ? params->damagePerTick : 0.0f;
    }

    std::shared_ptr<const engine::registry::BlockRegistry> block_registry() const override {
        return registry_;
    }

    void set_block_registry(
        std::shared_ptr<const engine::registry::BlockRegistry> registry) override {
        registry_ = registry;
        push_runtime_table();
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

    engine::entity::IMobWorldQuery& mob_world_query() override { return *this; }

    void register_entity_world(
        std::shared_ptr<engine::entity::IEntityWorld> world) override {
        entityWorld_ = std::move(world);
    }

    void register_mesher(std::shared_ptr<engine::voxel::IVoxelMesher> mesher) override {
        mesher_ = std::move(mesher);
        // The plugin takes over the mesh POLICY: the runtime block table the
        // mesher consumes is re-derived from the registry + the plugin's
        // per-block overrides (FALTANTES §3 item 2).
        push_runtime_table();
    }

    void register_lighting(std::shared_ptr<engine::voxel::IVoxelLighting> lighting) override {
        lighting_ = std::move(lighting);
        // The plugin takes over the light PROPERTIES: per-block emission/
        // absorption overrides are pushed into the world's light tables
        // (consulted before the builtin ones; FALTANTES §3 item 2).
        std::unordered_map<RuntimeBlockId, World::LightOverride> overrides;
        if (lighting_) {
            for (const auto& [id, props] : lighting_->light_property_overrides()) {
                overrides[static_cast<RuntimeBlockId>(id & 0xFFFF)] =
                    World::LightOverride{ props.emission, props.absorption };
            }
        }
        world_.set_light_table_overrides(std::move(overrides));
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

    void set_memory_budget(uint64_t bytes) override {
        world_.set_memory_budget(bytes);
    }

    // Streaming/budget observability (FALTANTES §3): forward the world's live
    // snapshot; optional push monitor dispatched after update() (change-gated).
    engine::voxel::StreamingSnapshot streaming_snapshot() const override {
        return world_.streaming_snapshot();
    }

    void set_streaming_monitor(
        std::shared_ptr<engine::voxel::IVoxelStreamingMonitor> monitor) override {
        monitor_ = std::move(monitor);
    }

    // Effective runtime block table (FALTANTES §3 item 2): after the registry
    // and plugin overrides merge — exactly what the mesher/light consumers
    // read. Sorted by id.
    std::vector<engine::voxel::BlockRuntimeView> runtime_block_views() const override {
        std::vector<engine::voxel::BlockRuntimeView> views;
        for (const auto& [id, info] : world_.runtime_block_table()) {
            engine::voxel::BlockRuntimeView view;
            view.id = static_cast<uint32_t>(id);
            view.uuid = info.uuid;
            view.solid = info.solid;
            view.transparent = info.transparent;
            view.occludes = info.occludes;
            view.renderLayer = info.renderLayer;
            view.lightEmission = info.lightEmission;
            view.lightAbsorption = info.lightAbsorption;
            // Physical material (FALTANTES §16 item 7 + item 10): the registry
            // JSON is the single source; the physics layer reads it through
            // this view. resistance + flammability drive the explosion
            // response (blast carve vs heat ignition).
            view.friction = info.friction;
            view.bounciness = info.bounciness;
            view.density = info.density;
            view.resistance = info.resistance;
            view.flammability = info.flammability;
            views.push_back(std::move(view));
        }
        return views;
    }

    // Declared here, defined below once VoxelTransactionImpl is complete.
    std::unique_ptr<engine::voxel::IVoxelTransaction> begin_transaction() override;

    engine::voxel::EditDryRunResult dry_run_edits(
        const std::vector<engine::voxel::BlockEdit>& edits) const override {
        engine::voxel::EditDryRunResult result;
        // Same validation as commit() (limits, policy, block registry) — a
        // dry-run never fires events and never touches the undo stack.
        if (!validate_edits(edits, result.error)) return result;
        // The diff: per-edit before-state from the live world. Unloaded chunk
        // mirrors commit's outcome (the commit would roll back there).
        for (const engine::voxel::BlockEdit& edit : edits) {
            const glm::vec3 position(static_cast<float>(edit.position.x),
                                     static_cast<float>(edit.position.y),
                                     static_cast<float>(edit.position.z));
            if (!world_.is_chunk_loaded_at(position)) {
                result.error = "transaction would roll back: chunk not loaded at (" +
                               std::to_string(edit.position.x) + ',' +
                               std::to_string(edit.position.y) + ',' +
                               std::to_string(edit.position.z) + ')';
                return result;
            }
            engine::voxel::BlockEdit diff = edit;
            diff.previousBlockId =
                static_cast<uint32_t>(world_.get_block_at(position));
            result.diff.push_back(diff);
        }
        result.valid = true;
        return result;
    }

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

    void set_transaction_limits(
        const engine::voxel::TransactionLimits& limits) override {
        transactionLimits_ = limits;
    }

    engine::voxel::TransactionLimits transaction_limits() const override {
        return transactionLimits_;
    }

    void set_transaction_policy(
        std::shared_ptr<engine::voxel::ITransactionPolicy> policy) override {
        transactionPolicy_ = std::move(policy);
    }

    // Shared validation stage for apply_edits AND dry_run_edits (FALTANTES
    // §7 items 138 + 141): per-transaction limits + the authoritative policy +
    // the block registry gate, BEFORE anything is applied. Returns false with
    // the same diagnostic commit() would produce. The caller decides whether
    // to notify (commit fires RolledBack; a dry-run never fires events).
    bool validate_edits(const std::vector<engine::voxel::BlockEdit>& edits,
                        std::string& errorOut) const {
        if (transactionLimits_.maxEdits > 0 &&
            edits.size() > transactionLimits_.maxEdits) {
            errorOut = "transaction rejected: " + std::to_string(edits.size()) +
                       " edits exceed the per-transaction limit (" +
                       std::to_string(transactionLimits_.maxEdits) + ")";
            return false;
        }
        if (transactionLimits_.maxBoxVolume > 0 && !edits.empty()) {
            int minX = edits[0].position.x, maxX = edits[0].position.x;
            int minY = edits[0].position.y, maxY = edits[0].position.y;
            int minZ = edits[0].position.z, maxZ = edits[0].position.z;
            for (const engine::voxel::BlockEdit& edit : edits) {
                minX = std::min(minX, edit.position.x); maxX = std::max(maxX, edit.position.x);
                minY = std::min(minY, edit.position.y); maxY = std::max(maxY, edit.position.y);
                minZ = std::min(minZ, edit.position.z); maxZ = std::max(maxZ, edit.position.z);
            }
            const uint64_t volume =
                static_cast<uint64_t>(maxX - minX + 1) *
                static_cast<uint64_t>(maxY - minY + 1) *
                static_cast<uint64_t>(maxZ - minZ + 1);
            if (volume > transactionLimits_.maxBoxVolume) {
                errorOut = "transaction rejected: edit box volume " +
                           std::to_string(volume) + " exceeds the limit (" +
                           std::to_string(transactionLimits_.maxBoxVolume) + ")";
                return false;
            }
        }
        if (transactionPolicy_) {
            for (const engine::voxel::BlockEdit& edit : edits) {
                const std::string policyError = transactionPolicy_->validate_edit(edit);
                if (!policyError.empty()) {
                    errorOut = "transaction rejected by policy: " + policyError;
                    return false;
                }
            }
            const std::string txError = transactionPolicy_->validate_transaction(edits);
            if (!txError.empty()) {
                errorOut = "transaction rejected by policy: " + txError;
                return false;
            }
        }

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
                return false;
            }
        }
        return true;
    }

    // The single mutation path of the public contract. Validates every edit,
    // captures previous state, applies through World::set_block_at (which
    // dirties chunks so the streaming pipeline re-meshes), verifies the apply
    // actually landed (loaded chunk), and rolls back everything on any failure.
    bool apply_edits(std::vector<engine::voxel::BlockEdit> edits,
                     std::string& errorOut) {
        // ---- Validation stage (FALTANTES §7 item 138): a rejection leaves
        // the world untouched (nothing applied, RolledBack). ----
        if (!validate_edits(edits, errorOut)) {
            notify(engine::voxel::TransactionEvent::Kind::RolledBack, edits.size());
            return false;
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

        // FALTANTES §7 item 137: the committed transaction is the single
        // broadcast point — every successful commit (editor/MCP/host edits and
        // server_submit_edit alike) reaches replication clients as ordered
        // deltas. No-op edits are skipped inside the adapter.
        if (replication_) replication_->server_broadcast_edits(applied);
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

    // Per-voxel block state index (FALTANTES item 2 "variantes de modelo"):
    // 0 = default state; >0 = named state from BlockDefinition.
    uint8_t get_block_state(int x, int y, int z) const override {
        return world_.get_state_at(glm::vec3(static_cast<float>(x),
                                             static_cast<float>(y),
                                             static_cast<float>(z)));
    }

    void set_block_state(int x, int y, int z, uint8_t stateIndex) override {
        world_.set_state_at(glm::vec3(static_cast<float>(x),
                                      static_cast<float>(y),
                                      static_cast<float>(z)), stateIndex);
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

    std::vector<std::pair<glm::ivec3, std::shared_ptr<engine::voxel::IVoxelBlockEntity>>>
    block_entities_in_chunk(int chunkX, int chunkZ) const override {
        std::vector<std::pair<glm::ivec3, std::shared_ptr<engine::voxel::IVoxelBlockEntity>>>
            out;
        const World::BlockEntityMap& map = world_.block_entities();
        const int minX = chunkX * CHUNK_SIZE_X;
        const int minZ = chunkZ * CHUNK_SIZE_Z;
        for (const auto& [cell, entity] : map) {
            if (cell.x < minX || cell.x >= minX + CHUNK_SIZE_X) continue;
            if (cell.z < minZ || cell.z >= minZ + CHUNK_SIZE_Z) continue;
            out.emplace_back(glm::ivec3{ cell.x, cell.y, cell.z }, entity);
        }
        std::sort(out.begin(), out.end(),
                  [](const auto& a, const auto& b) {
                      return a.first.x != b.first.x
                                 ? a.first.x < b.first.x
                                 : (a.first.y != b.first.y ? a.first.y < b.first.y
                                                           : a.first.z < b.first.z);
                  });
        return out;
    }

    std::shared_ptr<engine::voxel::IVoxelBlockEntity> create_block_entity(
        const std::string& typeId, std::string& errorOut) override {
        const engine::voxel::BlockEntityFactory factory =
            world_.find_block_entity_factory(typeId);
        if (!factory) {
            errorOut = "no factory registered for block entity type '" + typeId + "'";
            return nullptr;
        }
        return factory();
    }

    void set_block_entity_listener(
        std::function<void(const engine::voxel::BlockEntityEvent&)> listener) override {
        world_.set_block_entity_listener(std::move(listener));
    }

    // ---- World identity + content provenance (FALTANTES §4 item 4) ----
    // The facade stores the project's identity (seed/name/rules/plugins),
    // rides it with every v5 save (`meta`) and restores it on load. The
    // registry fingerprint is derived deterministically at save time so a
    // caller can detect "the registry changed since this save was written".
    void set_world_metadata(
        const engine::voxel::IVoxelWorld::WorldMetadata& metadata) override {
        metadata_ = metadata;
    }

    engine::voxel::IVoxelWorld::WorldMetadata world_metadata() const override {
        return metadata_;
    }

    uint64_t registry_version() const override {
        return compute_registry_version(*registry_);
    }

    uint64_t saved_registry_version() const override {
        return savedRegistryVersion_;
    }

    // Shared v5 `meta` builder (FALTANTES §4 item 4): world identity +
    // content provenance (seed, name, rules, plugin versions, registry
    // fingerprint). Used by BOTH the monolithic emitter (emit_world_save_
    // body) and the region manifest page (build_region_manifest_body) so the
    // two can never drift.
    static flatbuffers::Offset<engine::voxel::save::WorldMeta>
    build_meta_offset(
        flatbuffers::FlatBufferBuilder& builder, uint64_t seed,
        const std::string& worldName, const std::string& rulesJson,
        const std::vector<std::pair<std::string, std::string>>& pluginVersions,
        uint64_t registryVersion) {
        std::vector<flatbuffers::Offset<
            engine::voxel::save::PluginVersionEntry>> pluginOffsets;
        pluginOffsets.reserve(pluginVersions.size());
        for (const auto& [name, version] : pluginVersions) {
            const auto nameStr = builder.CreateString(name);
            const auto versionStr = builder.CreateString(version);
            engine::voxel::save::PluginVersionEntryBuilder entry(builder);
            entry.add_name(nameStr);
            entry.add_version(versionStr);
            pluginOffsets.push_back(entry.Finish());
        }
        engine::voxel::save::WorldMetaBuilder metaBuilder(builder);
        metaBuilder.add_seed(seed);
        if (!worldName.empty()) {
            metaBuilder.add_world_name(builder.CreateString(worldName));
        }
        if (!rulesJson.empty()) {
            metaBuilder.add_rules_json(builder.CreateString(rulesJson));
        }
        metaBuilder.add_registry_version(registryVersion);
        if (!pluginOffsets.empty()) {
            metaBuilder.add_plugins(builder.CreateVector(pluginOffsets));
        }
        return metaBuilder.Finish();
    }

    // Deterministic fingerprint of a block registry: FNV-1a 64 over the
    // concatenation of (uuid, definition version, namespaced name) of every
    // definition sorted by uuid (the order all_definitions() already uses), so
    // the stamp is independent of JSON load order and changes whenever any
    // definition's identity or version does.
    static uint64_t compute_registry_version(
        const engine::registry::BlockRegistry& registry) {
        std::string stream;
        for (const engine::registry::BlockDefinition& definition :
             registry.all_definitions()) {
            stream.append(definition.uuid);
            stream.append(1, '\0');
            stream.append(definition.namespaced());
            stream.append(1, '\0');
            stream.append(reinterpret_cast<const char*>(&definition.version),
                          sizeof(definition.version));
        }
        uint64_t hash = 1469598103934665603ull;  // FNV-1a 64 offset basis
        for (const char c : stream) {
            hash ^= static_cast<unsigned char>(c);
            hash *= 1099511628211ull;
        }
        return hash;
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

        // Build the neutral save snapshot from the live world, then emit it at
        // the CURRENT schema version through the shared emitter (the same one
        // the migration path uses — serialize and migrate cannot drift).
        WorldSaveData data;
        const std::vector<std::pair<RuntimeBlockId, RuntimeBlockInfo>> palette =
            world_.runtime_block_table();
        data.palette.reserve(palette.size());
        for (const auto& [id, info] : palette) {
            data.palette.emplace_back(id, info.uuid);
        }

        std::vector<std::pair<int, int>> keys;
        for (const auto& [key, chunk] : world_.chunks) {
            if (chunk) keys.push_back(key);
        }
        std::sort(keys.begin(), keys.end());
        const std::size_t layerBytes =
            static_cast<std::size_t>(CHUNK_SIZE_X) * CHUNK_SIZE_Z;
        data.chunks.reserve(keys.size());
        for (const auto& [cx, cz] : keys) {
            const Chunk& chunk = *world_.chunks.at({ cx, cz });
            const int extent =
                std::clamp(chunk.vertical_render_extent(), 1, CHUNK_SIZE_Y);
            WorldSaveData::Chunk outChunk;
            outChunk.cx = cx;
            outChunk.cz = cz;
            outChunk.extent = static_cast<uint32_t>(extent);
            const std::size_t voxelBytes =
                static_cast<std::size_t>(extent) * layerBytes;
            outChunk.blocks.reserve(voxelBytes);
            outChunk.fluid.reserve(voxelBytes);
            outChunk.stateIndices.reserve(voxelBytes);
            for (int y = 0; y < extent; ++y) {
                for (int z = 0; z < CHUNK_SIZE_Z; ++z) {
                    for (int x = 0; x < CHUNK_SIZE_X; ++x) {
                        outChunk.blocks.push_back(static_cast<uint16_t>(
                            chunk.get_block(x, y, z)));
                        // Fluid-level byte for ANY fluid (water is the builtin
                        // case; data-driven fluids carry levels the same way).
                        outChunk.fluid.push_back(chunk.get_fluid_level(x, y, z));
                        // Per-voxel state index (FALTANTES item 2): 0 = default.
                        outChunk.stateIndices.push_back(chunk.get_state(x, y, z));
                    }
                }
            }
            data.chunks.push_back(std::move(outChunk));
        }

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
        data.blockEntities.reserve(entityKeys.size());
        for (const TickCell& cell : entityKeys) {
            const std::shared_ptr<engine::voxel::IVoxelBlockEntity>& entity =
                entities.at(cell);
            WorldSaveData::BlockEntity outEntity;
            outEntity.x = cell.x;
            outEntity.y = cell.y;
            outEntity.z = cell.z;
            outEntity.typeId = entity->type_id();
            outEntity.dataVersion = entity->data_version();
            outEntity.blob = entity->serialize_state();
            data.blockEntities.push_back(std::move(outEntity));
        }

        // World entities (META section 15): the EnTT-backed population, as
        // versioned snapshots (type, position, health, tick policy, project
        // components) in deterministic spawn order.
        data.worldEntities =
            entityWorld_ ? entityWorld_->serialize_entities()
                         : std::vector<engine::entity::EntitySnapshot>{};

        // Scheduler state (FALTANTES §6 item 129): the authoritative fixed
        // tick clock + pending queues ride with the save so a dedicated/
        // headless server continues tick work where the session was. The
        // scheduler's binary capture is versioned and self-validating.
        data.schedulerState = world_.scheduler().serialize_state();

        // World identity + content provenance (FALTANTES §4 item 4): seed,
        // world name, rules JSON, plugin versions and the block-registry
        // fingerprint ride with the save so identity survives load and a
        // caller can detect a registry change since the save was written.
        data.seed = metadata_.seed;
        data.worldName = metadata_.worldName;
        data.rulesJson = metadata_.rulesJson;
        data.pluginVersions = metadata_.pluginVersions;
        data.registryVersion = compute_registry_version(*registry_);

        return emit_world_save_body(data, errorOut);
    }

    // Emits the CURRENT schema version from the neutral snapshot. v5 when
    // FlatBuffers is enabled; otherwise the v4 manual binary body (framing and
    // checksum identical to v4 saves, which still load). Shared by serialize
    // (live world) and migrate (legacy parse) — one emitter, no drift.
    std::string emit_world_save_body(const WorldSaveData& data,
                                     std::string& errorOut) const {
        (void)errorOut;
#if VC_ENABLE_FLATBUFFERS
        // v5: the body is a FlatBuffers container (typed, verifiable) instead
        // of the hand-rolled binary layout. Framing is unchanged: magic +
        // version + buffer + BLAKE3, and the file layer still zstd-compresses.
        flatbuffers::FlatBufferBuilder builder(1024);
        std::vector<flatbuffers::Offset<engine::voxel::save::PaletteEntry>>
            paletteOffsets;
        paletteOffsets.reserve(data.palette.size());
        for (const auto& [id, uuid] : data.palette) {
            const auto uuidStr = builder.CreateString(uuid);
            engine::voxel::save::PaletteEntryBuilder entry(builder);
            entry.add_id(static_cast<uint16_t>(id));
            entry.add_uuid(uuidStr);
            paletteOffsets.push_back(entry.Finish());
        }
        const auto paletteVec = builder.CreateVector(paletteOffsets);

        std::vector<flatbuffers::Offset<engine::voxel::save::ChunkEntry>>
            chunkOffsets;
        chunkOffsets.reserve(data.chunks.size());
        for (const WorldSaveData::Chunk& chunk : data.chunks) {
            const auto blocksVec = builder.CreateVector(chunk.blocks);
            const auto fluidVec = builder.CreateVector(chunk.fluid);
            // Per-voxel state indices (FALTANTES item 2): optional field —
            // null means "all state 0" (older saves, backward compatible).
            flatbuffers::Offset<flatbuffers::Vector<uint8_t>> stateVec;
            if (!chunk.stateIndices.empty()) {
                stateVec = builder.CreateVector(chunk.stateIndices);
            }
            engine::voxel::save::ChunkEntryBuilder entry(builder);
            entry.add_cx(chunk.cx);
            entry.add_cz(chunk.cz);
            entry.add_extent(chunk.extent);
            entry.add_blocks(blocksVec);
            entry.add_fluid(fluidVec);
            if (stateVec.o) entry.add_state_indices(stateVec);
            chunkOffsets.push_back(entry.Finish());
        }
        const auto chunkVec = builder.CreateVector(chunkOffsets);

        std::vector<flatbuffers::Offset<engine::voxel::save::BlockEntityEntry>>
            entityOffsets;
        entityOffsets.reserve(data.blockEntities.size());
        for (const WorldSaveData::BlockEntity& entity : data.blockEntities) {
            const auto typeId = builder.CreateString(entity.typeId);
            const auto blobVec = builder.CreateVector(entity.blob);
            engine::voxel::save::BlockEntityEntryBuilder entry(builder);
            entry.add_x(entity.x);
            entry.add_y(entity.y);
            entry.add_z(entity.z);
            entry.add_type_id(typeId);
            entry.add_data_version(entity.dataVersion);
            entry.add_blob(blobVec);
            entityOffsets.push_back(entry.Finish());
        }
        const auto entityVec = builder.CreateVector(entityOffsets);

        std::vector<flatbuffers::Offset<engine::voxel::save::EntityEntry>>
            worldEntityOffsets;
        worldEntityOffsets.reserve(data.worldEntities.size());
        for (const engine::entity::EntitySnapshot& snapshot : data.worldEntities) {
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
            if (!snapshot.stableId.empty()) {
                entry.add_stable_id(builder.CreateString(snapshot.stableId));
            }
            worldEntityOffsets.push_back(entry.Finish());
        }
        const auto worldEntityVec = builder.CreateVector(worldEntityOffsets);

        // Opaque scheduler capture (FALTANTES §6 item 129): rides as a byte
        // blob; the engine restores it through the scheduler's own versioned
        // deserializer on load (never interpreted here).
        std::vector<uint8_t> schedulerBytes;
        schedulerBytes.reserve(data.schedulerState.size());
        for (const std::byte b : data.schedulerState) {
            schedulerBytes.push_back(static_cast<uint8_t>(b));
        }
        const auto schedulerVec = schedulerBytes.empty()
            ? builder.CreateVector(static_cast<const uint8_t*>(nullptr), 0)
            : builder.CreateVector(schedulerBytes);

        // World identity + content provenance (FALTANTES §4 item 4): seed,
        // world name, rules JSON, plugin versions and the block-registry
        // fingerprint. Optional — older v5 buffers have no meta (null loads
        // as default metadata).
        const auto meta =
            build_meta_offset(builder, data.seed, data.worldName,
                              data.rulesJson, data.pluginVersions,
                              data.registryVersion);

        engine::voxel::save::WorldSaveBuilder save(builder);
        save.add_palette(paletteVec);
        save.add_chunks(chunkVec);
        save.add_entities(entityVec);
        save.add_world_entities(worldEntityVec);
        save.add_scheduler_state(schedulerVec);
        save.add_meta(meta);
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
        append_u32(body, static_cast<uint32_t>(data.palette.size()));
        for (const auto& [id, uuid] : data.palette) {
            append_u16(body, id);
            append_u32(body, static_cast<uint32_t>(uuid.size()));
            body.append(uuid);
        }
        append_u32(body, static_cast<uint32_t>(data.chunks.size()));
        for (const WorldSaveData::Chunk& chunk : data.chunks) {
            append_i32(body, chunk.cx);
            append_i32(body, chunk.cz);
            append_u32(body, chunk.extent);
            for (const uint16_t block : chunk.blocks) append_u16(body, block);
            for (const uint8_t fluid : chunk.fluid) {
                body.push_back(static_cast<char>(fluid));
            }
        }
        append_u32(body, static_cast<uint32_t>(data.blockEntities.size()));
        for (const WorldSaveData::BlockEntity& entity : data.blockEntities) {
            append_i32(body, entity.x);
            append_i32(body, entity.y);
            append_i32(body, entity.z);
            append_u32(body, static_cast<uint32_t>(entity.typeId.size()));
            body.append(entity.typeId);
            append_u32(body, entity.dataVersion);
            append_u32(body, static_cast<uint32_t>(entity.blob.size()));
            if (!entity.blob.empty()) {
                body.append(reinterpret_cast<const char*>(entity.blob.data()),
                            entity.blob.size());
            }
        }
        append(body, hash_->hash(body));
        return body;
#endif
    }

    // Schema version introspection (FALTANTES §4 item 9): reads the version
    // field of a serialized save WITHOUT a full parse — a caller (editor/CLI/
    // migration tool) can ask "what schema is this?" before deciding to load
    // or migrate. Returns 0 when the bytes are not a world save (bad magic or
    // too small). Never throws; purely a header read.
    uint32_t world_save_schema_version(const std::string& data) const override {
        if (data.size() < 5 + 4) return 0;
        if (data.compare(0, 5, kWorldMagic) != 0) return 0;
        return read_u32(data, 5);
    }

    // Explicit schema migration (FALTANTES §4 item 9): upgrades a legacy save
    // (v1-v4) to the CURRENT schema version WITHOUT touching the live world —
    // the parse below is pure (only const world queries for validation) and
    // the migrated bytes are returned for the caller to persist. All-or-nothing
    // with the same gates as load: checksum verified, palette uuids must still
    // map to the same runtime ids in the current registry, block ids must be
    // valid. Block-entity blobs pass through OPAQUELY (no factory required to
    // migrate); the load of the migrated save does the factory/state
    // validation, exactly as it would for the legacy save. A save already at
    // the current version is a no-op success; a save from a NEWER engine is
    // refused with a clear forward-compatibility diagnostic.
    bool migrate_world_save(const std::string& legacyData,
                            std::string& migratedOut,
                            std::string& errorOut) override {
        const uint32_t version = world_save_schema_version(legacyData);
        if (version == 0) {
            errorOut = "not a world save (bad magic)";
            return false;
        }
        if (version == kWorldSaveVersion) {
            migratedOut = legacyData;  // already current: no-op upgrade
            errorOut.clear();
            return true;
        }
        if (version > kWorldSaveVersion) {
            errorOut = "world save uses schema v" + std::to_string(version) +
                       " from a NEWER engine (this engine writes schema v" +
                       std::to_string(kWorldSaveVersion) + ")";
            return false;
        }
        if (version < kWorldSaveVersionV1) {
            errorOut = "world save uses unknown schema version " +
                       std::to_string(version);
            return false;
        }

        // Legacy (v1-v4): verify the checksum first — a corrupt save is
        // refused, never migrated (garbage in, garbage out).
        const bool wideChecksum = version >= kWorldSaveVersionV4;
        const std::size_t checksumLen = wideChecksum ? 32 : 8;
        if (legacyData.size() < 5 + 4 + checksumLen) {
            errorOut = "world save too small";
            return false;
        }
        const std::string body =
            legacyData.substr(0, legacyData.size() - checksumLen);
        if (wideChecksum) {
            const std::string stored = legacyData.substr(legacyData.size() - 32);
            if (hash_->hash(body) != stored) {
                errorOut = "world save corrupt (BLAKE3 checksum mismatch)";
                return false;
            }
        } else if (fnv1a_bytes(body) != read_u64(legacyData,
                                                 legacyData.size() - 8)) {
            errorOut = "world save corrupt (checksum mismatch)";
            return false;
        }

        WorldSaveData data;
        if (!parse_legacy_world_save(body, version, data, errorOut)) {
            return false;
        }
        // Legacy saves carry no identity, so the migrated bytes inherit the
        // MIGRATING world's metadata (FALTANTES §4 item 4): the caller who
        // migrates decides what identity the upgraded save carries.
        data.seed = metadata_.seed;
        data.worldName = metadata_.worldName;
        data.rulesJson = metadata_.rulesJson;
        data.pluginVersions = metadata_.pluginVersions;
        data.registryVersion = compute_registry_version(*registry_);
        migratedOut = emit_world_save_body(data, errorOut);
        return !migratedOut.empty();
    }

    // Pure legacy body parse (v1-v4) into the neutral snapshot. Mirrors the
    // field walk + bounds checks + registry gates of the legacy branch of
    // deserialize_world, but WRITES NOTHING to the world — it is the migration
    // half of item 9. The checksum was already verified by the caller; body
    // here excludes the trailing checksum.
    bool parse_legacy_world_save(const std::string& body, uint32_t version,
                                 WorldSaveData& out, std::string& errorOut) const {
        const bool hasPalette = version >= kWorldSaveVersionV2;
        const bool hasBlockEntities = version >= kWorldSaveVersionV3;
        const bool wideBlockIds = version >= kWorldSaveVersionV2;
        const std::size_t layerBytes =
            static_cast<std::size_t>(CHUNK_SIZE_X) * CHUNK_SIZE_Z;

        std::size_t cursor = 9;  // magic (5) + version u32 (4)
        if (hasPalette) {
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
                // Same gate as load: a registry change is a clear diagnostic,
                // never a guessed id.
                const std::optional<RuntimeBlockId> expected =
                    world_.runtime_block_id_for_uuid(uuid);
                if (!expected || *expected != storedId) {
                    errorOut = "world save references block uuid '" + uuid +
                               "' not mapped by the current registry (id " +
                               std::to_string(storedId) + ")";
                    return false;
                }
                out.palette.emplace_back(storedId, uuid);
            }
        }

        if (cursor + 4 > body.size()) {
            errorOut = "world save truncated (chunk table)";
            return false;
        }
        const uint32_t chunkCount = read_u32(body, cursor);
        cursor += 4;
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
            const std::size_t voxelBytes =
                static_cast<std::size_t>(extent) * layerBytes;
            const std::size_t blockBytes = voxelBytes * (wideBlockIds ? 2 : 1);
            if (extent == 0 || extent > static_cast<uint32_t>(CHUNK_SIZE_Y) ||
                cursor + blockBytes + voxelBytes > body.size()) {
                errorOut = "world save corrupt (bad extent on chunk " +
                           std::to_string(index) + ")";
                return false;
            }

            WorldSaveData::Chunk chunk;
            chunk.cx = cx;
            chunk.cz = cz;
            chunk.extent = extent;
            chunk.blocks.reserve(voxelBytes);
            for (std::size_t i = 0; i < voxelBytes; ++i) {
                const std::size_t offset = cursor + (wideBlockIds ? i * 2 : i);
                const RuntimeBlockId id = wideBlockIds
                    ? read_u16(body, offset)
                    : static_cast<uint8_t>(body[offset]);
                if (!world_.is_valid_block_id(id)) {
                    errorOut = "world save: chunk (" + std::to_string(cx) + ',' +
                               std::to_string(cz) + ") contains unknown block id " +
                               std::to_string(id);
                    return false;
                }
                chunk.blocks.push_back(static_cast<uint16_t>(id));
            }
            chunk.fluid.assign(
                body.begin() + static_cast<std::ptrdiff_t>(cursor + blockBytes),
                body.begin() +
                    static_cast<std::ptrdiff_t>(cursor + blockBytes + voxelBytes));
            cursor += blockBytes + voxelBytes;
            out.chunks.push_back(std::move(chunk));
        }

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
                WorldSaveData::BlockEntity entity;
                entity.x = static_cast<int32_t>(read_u32(body, cursor));
                entity.y = static_cast<int32_t>(read_u32(body, cursor + 4));
                entity.z = static_cast<int32_t>(read_u32(body, cursor + 8));
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
                entity.typeId = body.substr(cursor, typeLen);
                cursor += typeLen;
                if (cursor + 4 > body.size()) {
                    errorOut = "world save truncated (block entity data " +
                               std::to_string(index) + ")";
                    return false;
                }
                entity.dataVersion = read_u32(body, cursor);
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
                entity.blob.assign(
                    body.begin() + static_cast<std::ptrdiff_t>(cursor),
                    body.begin() +
                        static_cast<std::ptrdiff_t>(cursor + blobLen));
                cursor += blobLen;
                out.blockEntities.push_back(std::move(entity));
            }
        }
        // v1-v4 carry no world entities (added in v5); a migrated save starts
        // with an empty population, exactly what the legacy save implied.
        errorOut.clear();
        return true;
    }

    // Restore mode guard (FALTANTES §4 item 1): while a load runs, chunk
    // eviction in World::update is suppressed so restored chunks survive the
    // load's own update() waits; normal streaming resumes on scope exit. Also
    // bumps the facade's restoringDepth_ so the autosave tick (which runs
    // inside update()) never snapshots a half-restored world.
    struct RestoreModeGuard {
        RestoreModeGuard(VoxelWorldFacade& facade, World& world)
            : facade_(facade), world_(world) {
            world_.set_restoring(true);
            ++facade_.restoringDepth_;
        }
        ~RestoreModeGuard() {
            --facade_.restoringDepth_;
            world_.set_restoring(false);
        }
        VoxelWorldFacade& facade_;
        World& world_;
    };

    // FALTANTES §4 item 19: rollback do carregamento completo se QUALQUER
    // chunk (ou entidade) falhar. Captura o estado pré-load — conteúdo+flags
    // de cada chunk carregado, mapa de block entities, população de world
    // entities — e, se o load falhar em qualquer ponto (parse de chunk,
    // aplicação, entidades), o destrutor restaura o mundo EXATAMENTE ao
    // estado pré-load: chunks criados pelo load são removidos, chunks
    // assumidos no lugar recuperam o conteúdo antigo, entidades voltam.
    // commit() no sucesso descarta o snapshot. (O custo é uma cópia tipo
    // save dos chunks carregados — pago por load, mas só o rollback a usa.)
    struct LoadRollback {
        VoxelWorldFacade& facade_;
        World& world_;
        std::map<std::pair<int, int>, ChunkSaveSnapshot> chunks_;
        std::map<std::pair<int, int>, bool> preUnsaved_;
        World::BlockEntityMap blockEntities_;
        std::vector<engine::entity::EntitySnapshot> worldEntities_;
        // World identity + registry stamp (FALTANTES §4 item 4): the load
        // restores them from the save's meta; a rollback must bring back the
        // pre-load values exactly like the chunk/entity state.
        engine::voxel::IVoxelWorld::WorldMetadata metadata_;
        uint64_t savedRegistryVersion_{ 0 };
        bool committed_{ false };

        explicit LoadRollback(VoxelWorldFacade& facade, World& world)
            : facade_(facade), world_(world) {
            const std::size_t layerBytes =
                static_cast<std::size_t>(CHUNK_SIZE_X) * CHUNK_SIZE_Z;
            std::lock_guard<std::recursive_mutex> lock(world_.chunksMutex);
            for (const auto& [key, chunk] : world_.chunks) {
                if (!chunk) continue;
                ChunkSaveSnapshot snap;
                snap.cx = key.first;
                snap.cz = key.second;
                snap.revision = chunk->revision();
                snap.extent = std::clamp(chunk->vertical_render_extent(), 1,
                                         CHUNK_SIZE_Y);
                const std::size_t voxelBytes =
                    static_cast<std::size_t>(snap.extent) * layerBytes;
                snap.blocks.reserve(voxelBytes);
                snap.fluid.reserve(voxelBytes);
                snap.stateIndices.reserve(voxelBytes);
                for (int y = 0; y < snap.extent; ++y) {
                    for (int z = 0; z < CHUNK_SIZE_Z; ++z) {
                        for (int x = 0; x < CHUNK_SIZE_X; ++x) {
                            snap.blocks.push_back(chunk->get_block(x, y, z));
                            snap.fluid.push_back(static_cast<uint8_t>(
                                chunk->get_fluid_level(x, y, z)));
                            snap.stateIndices.push_back(chunk->get_state(x, y, z));
                        }
                    }
                }
                chunks_[key] = std::move(snap);
                preUnsaved_[key] =
                    chunk->hasUnsavedEdits.load(std::memory_order_acquire);
            }
            blockEntities_ = world_.block_entities();
            if (facade_.entityWorld_) {
                worldEntities_ = facade_.entityWorld_->serialize_entities();
            }
            metadata_ = facade_.metadata_;
            savedRegistryVersion_ = facade_.savedRegistryVersion_;
        }

        ~LoadRollback() {
            if (!committed_) rollback();
        }

        void commit() { committed_ = true; }

        void rollback() {
            // 1. Chunks: remove os criados pelo load; re-restaura conteúdo e
            // flags dos chunks assumidos no lugar (mesmo caminho em lote do
            // item 18; o destrutor roda com restoring_ ainda ativo).
            {
                std::lock_guard<std::recursive_mutex> lock(world_.chunksMutex);
                for (auto it = world_.chunks.begin();
                     it != world_.chunks.end();) {
                    if (chunks_.find(it->first) == chunks_.end()) {
                        facade_.bridge_.retire_chunk(ChunkId{
                            { it->second->chunkX, it->second->chunkZ }, 0 });
                        world_.scheduler().cancel_chunk(it->first.first,
                                                        it->first.second);
                        it = world_.chunks.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            for (auto& [key, snap] : chunks_) {
                world_.restore_chunk_data(snap.cx, snap.cz, snap.extent,
                                          snap.blocks.data(), snap.fluid.data(),
                                          snap.stateIndices.data(),
                                          facade_.playerPos_, facade_.bridge_);
                std::lock_guard<std::recursive_mutex> lock(world_.chunksMutex);
                auto it = world_.chunks.find(key);
                if (it != world_.chunks.end() && it->second) {
                    it->second->hasUnsavedEdits.store(
                        preUnsaved_[key], std::memory_order_release);
                }
            }
            // 2. Block entities: remove as criadas pelo load, restaura as
            // pré-existentes (o mapa capturado é a autoridade).
            const World::BlockEntityMap current = world_.block_entities();
            for (const auto& [cell, entity] : current) {
                (void)entity;
                if (!blockEntities_.contains(cell)) {
                    world_.remove_block_entity(cell.x, cell.y, cell.z);
                }
            }
            for (const auto& [cell, entity] : blockEntities_) {
                world_.restore_block_entity(cell.x, cell.y, cell.z, entity);
            }
            // 3. World entities: restaura a população pré-load (o
            // deserialize_entities interno é all-or-nothing). Sempre chamado:
            // com população vazia ele limpa o que o load tiver inserido.
            if (facade_.entityWorld_) {
                std::string entityError;
                facade_.entityWorld_->deserialize_entities(worldEntities_,
                                                           entityError);
            }
            // 4. World identity + registry stamp (FALTANTES §4 item 4): o
            // load pode ter sobrescrito a metadata do save; o rollback
            // restaura os valores pré-load.
            facade_.metadata_ = metadata_;
            facade_.savedRegistryVersion_ = savedRegistryVersion_;
        }
    };

    bool deserialize_world(const std::string& data, std::string& errorOut) override {
        RestoreModeGuard restoreGuard(*this, world_);
        LoadRollback loadRollback(*this, world_);
        if (storage_) {
            const bool ok = storage_->deserialize_world(data, errorOut);
            if (ok) {
                loadRollback.commit();
                reset_session_history();
            }
            return ok;
        }
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
            // A NEWER schema than this engine writes is a forward-compat
            // diagnostic (migrate_world_save refuses it the same way) — never
            // silently downgraded. Anything else is simply unknown.
            if (version > kWorldSaveVersion) {
                errorOut = "world save uses schema v" + std::to_string(version) +
                           " from a NEWER engine (this engine writes schema v" +
                           std::to_string(kWorldSaveVersion) + ")";
            } else {
                errorOut = "unsupported world schema version " +
                           std::to_string(version);
            }
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
            const bool ok = deserialize_world_v5(body, errorOut);
            if (ok) {
                loadRollback.commit();
                reset_session_history();
            }
            return ok;
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

            std::vector<uint8_t> stateIndicesLegacy;  // v1-v4: no state data
            if (!apply_saved_chunk(cx, cz, extent, blockIds, water, stateIndicesLegacy)) {
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
        loadRollback.commit();
        reset_session_history();
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
                // Per-voxel state indices (FALTANTES item 2): optional — null means
                // all state 0 (older saves, backward compatible).
                std::vector<uint8_t> stateIndicesV5;
                if (entry->state_indices() != nullptr) {
                    stateIndicesV5.reserve(voxelBytes);
                    for (std::size_t i = 0; i < voxelBytes; ++i) {
                        stateIndicesV5.push_back(entry->state_indices()->Get(
                            static_cast<flatbuffers::uoffset_t>(i)));
                    }
                }
                if (!apply_saved_chunk(cx, cz, static_cast<int>(extent),
                                       blockIds, water, stateIndicesV5)) {
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
                if (entry->stable_id() != nullptr) {
                    snapshot.stableId = entry->stable_id()->str();
                }
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

        // Scheduler state (FALTANTES §6 item 129): restore the fixed-tick
        // clock + pending queues so a dedicated/headless server continues tick
        // work exactly where the saved session was. Optional: v5 buffers
        // without the field (null/empty, e.g. migrated v1-v4 saves) start a
        // fresh clock. The scheduler's versioned deserializer refuses
        // malformed/foreign state with a clear diagnostic (never resets).
        if (save->scheduler_state() != nullptr &&
            save->scheduler_state()->size() > 0) {
            const auto* raw = save->scheduler_state();
            std::vector<std::byte> schedulerBytes;
            schedulerBytes.reserve(raw->size());
            for (flatbuffers::uoffset_t i = 0; i < raw->size(); ++i) {
                schedulerBytes.push_back(static_cast<std::byte>(raw->Get(i)));
            }
            std::string schedulerError;
            if (!world_.scheduler().deserialize_state(schedulerBytes,
                                                      schedulerError)) {
                errorOut = "world save: scheduler state refused: " + schedulerError;
                return false;
            }
        }

        // World identity + content provenance (FALTANTES §4 item 4): restore
        // the save's metadata (seed/name/rules/plugin versions) and the
        // block-registry fingerprint. Optional: v5 buffers without the field
        // (null, e.g. written before this addition) keep the caller's
        // metadata and clear the saved registry stamp.
        if (const auto* meta = save->meta(); meta != nullptr) {
            metadata_.seed = meta->seed();
            metadata_.worldName = meta->world_name() != nullptr
                ? meta->world_name()->str() : std::string();
            metadata_.rulesJson = meta->rules_json() != nullptr
                ? meta->rules_json()->str() : std::string();
            metadata_.pluginVersions.clear();
            if (meta->plugins() != nullptr) {
                metadata_.pluginVersions.reserve(meta->plugins()->size());
                for (flatbuffers::uoffset_t i = 0; i < meta->plugins()->size();
                     ++i) {
                    const engine::voxel::save::PluginVersionEntry* plugin =
                        meta->plugins()->Get(i);
                    metadata_.pluginVersions.emplace_back(
                        plugin->name() != nullptr ? plugin->name()->str()
                                                  : std::string(),
                        plugin->version() != nullptr ? plugin->version()->str()
                                                     : std::string());
                }
            }
            savedRegistryVersion_ = meta->registry_version();
        } else {
            savedRegistryVersion_ = 0;
        }
        errorOut.clear();
        return true;
    }
#endif

    bool save_world(const std::string& filePath, std::string& errorOut) override {
        if (storage_ && storage_->supports_regions()) {
            return save_world_regions(filePath, errorOut);
        }
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

    // Async persistence (FALTANTES §4 item 5): dispatch save/load work to the
    // world's thread pool; the result arrives via the callback (pool thread)
    // or wait_async_saves. Requires a region-capable storage.
    bool save_world_async(
        const std::string& filePath,
        std::function<void(bool, std::string)> onDone,
        std::string& errorOut) override {
        if (storage_ && storage_->supports_regions()) {
            return save_world_regions_async(filePath, std::move(onDone),
                                            errorOut);
        }
        errorOut = "async saves require a region-capable (paged) storage";
        return false;
    }

    bool load_world_async(
        const std::string& filePath,
        std::function<void(bool, std::string)> onDone,
        std::string& errorOut) override {
        if (storage_ && storage_->supports_regions()) {
            return load_world_regions_async(filePath, std::move(onDone),
                                            errorOut);
        }
        errorOut = "async loads require a region-capable (paged) storage";
        return false;
    }

    bool wait_async_saves(std::string& errorOut) override {
        std::shared_ptr<AsyncOpState> op;
        {
            std::lock_guard<std::mutex> lock(asyncMutex_);
            op = asyncOp_;
        }
        if (!op) {
            errorOut.clear();
            return true;
        }
        return wait_async_op(op, errorOut);
    }

    void set_autosave(const engine::voxel::IVoxelWorld::AutosaveConfig& config,
                      const std::string& autosavePath) override {
        autosaveConfig_ = config;
        autosavePath_ = autosavePath;
        autosaveElapsed_ = 0.0;
    }

    engine::voxel::IVoxelWorld::AutosaveConfig autosave_config() const override {
        return autosaveConfig_;
    }

    // Number of loaded chunks whose data moved since the last region save —
    // the same gate the delta save uses (revision differs or key is new).
    // Caller-thread reads; savedRevision_ is written by save jobs, so the
    // read takes the revision mutex. Snapshot revision is compared under the
    // chunk mutex to avoid a torn read of the map itself.
    std::size_t changed_chunk_count() const {
        std::lock_guard<std::recursive_mutex> lock(world_.chunksMutex);
        std::lock_guard<std::mutex> revLock(savedRevisionMutex_);
        std::size_t changed = 0;
        for (const auto& [key, chunk] : world_.chunks) {
            if (!chunk) continue;
            const auto found = savedRevision_.find(key);
            if (found == savedRevision_.end() ||
                found->second != chunk->revision()) {
                ++changed;
            }
        }
        return changed;
    }

    // Autosave tick (FALTANTES §4 item 7): fires a delta async save when the
    // elapsed-time interval OR the change-volume threshold is crossed. Skipped
    // while another op is in flight (timers keep running, next update
    // retries) and while a load is restoring chunks (update() runs inside
    // load waits — the snapshot would capture a half-restored world).
    void tick_autosave(float deltaTime) {
        if (!autosaveConfig_.enabled || autosavePath_.empty() ||
            restoringDepth_ > 0) {
            return;
        }
        autosaveElapsed_ += deltaTime;
        const bool timeFired = autosaveConfig_.intervalSeconds > 0.0 &&
                               autosaveElapsed_ >= autosaveConfig_.intervalSeconds;
        const bool volumeFired = autosaveConfig_.dirtyChunkThreshold > 0 &&
                                 changed_chunk_count() >=
                                     autosaveConfig_.dirtyChunkThreshold;
        if (!timeFired && !volumeFired) return;
        std::string error;
        // onDone null would make this the synchronous path (blocks update());
        // pass a real (no-op) callback so it stays async. A refused fire
        // (op in flight) leaves the timers running.
        const bool dispatched = save_world_async(
            autosavePath_, [](bool, std::string) {}, error);
        if (dispatched) autosaveElapsed_ = 0.0;
    }

    ~VoxelWorldFacade() override {
        // Never destroy the facade while an async op still references it:
        // wait out any in-flight save/load before members go away.
        std::shared_ptr<AsyncOpState> op;
        {
            std::lock_guard<std::mutex> lock(asyncMutex_);
            op = asyncOp_;
        }
        if (op) {
            std::unique_lock<std::mutex> lock(op->mutex);
            op->cv.wait(lock, [&] { return op->finished; });
        }
    }

    bool load_world(const std::string& filePath, std::string& errorOut) override {
        if (storage_ && storage_->supports_regions()) {
            return load_world_regions(filePath, errorOut);
        }
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
        tick_autosave(deltaTime);
        if (monitor_) {
            const engine::voxel::StreamingSnapshot snapshot = world_.streaming_snapshot();
            // Change-gated: only fire when streaming state actually moved.
            if (!(snapshot == lastSnapshot_)) {
                lastSnapshot_ = snapshot;
                monitor_->on_streaming_update(snapshot);
            }
        }
    }

private:
    // Drives the world until the chunk is loaded (near the last update
    // position), then overwrites its voxel content with the saved state.
    bool apply_saved_chunk(int cx, int cz, int extent,
                           const std::vector<RuntimeBlockId>& blocks,
                           const std::string& water,
                           const std::vector<uint8_t>& stateIndices) {
        // FALTANTES §4 item 17/18: o chunk é materializado SOB DEMANDA — criado
        // no mapa do mundo ou assumido no lugar (um gerador em voo é esperado),
        // sem depender da janela de streaming e sem gerar o chunk antes de
        // restaurá-lo; os dados voxel são escritos em UM lote com invalidação
        // única (restore_chunk_data), não set_block_at por voxel. O caller
        // (load) segura o modo restoring: os update() de espera não despejam
        // chunks já restaurados nem disparam geração nova de janela.
        return world_.restore_chunk_data(
            cx, cz, extent, blocks.data(),
            reinterpret_cast<const uint8_t*>(water.data()),
            stateIndices.empty() ? nullptr : stateIndices.data(),
            playerPos_, bridge_);
    }

    // ---- Region-paged persistence (FALTANTES §4 item 1) ----
    // A region-capable IChunkStorage persists per-page payloads: one page per
    // region tile ("r.<x>.<z>", kRegionChunks x kRegionChunks chunks) plus the
    // "world" manifest page (palette + entities + world entities + region
    // list, no chunk data). The WORLD owns the page encoding; the backend only
    // stores opaque bytes. Non-paged backends keep the monolithic path.

    // Groups every loaded chunk into its region tile and saves one page per
    // region, all under a SINGLE chunk-mutex acquisition (no per-chunk locks).
    bool save_world_regions(const std::string& filePath, std::string& errorOut) {
        return dispatch_region_save(filePath, errorOut, nullptr);
    }

    bool save_world_regions_async(
        const std::string& filePath,
        std::function<void(bool, std::string)> onDone,
        std::string& errorOut) {
        // onDone non-null => async: the bool only reports dispatch (the result
        // arrives via the callback or wait_async_saves).
        return dispatch_region_save(filePath, errorOut, std::move(onDone));
    }

    // One async-op slot (FALTANTES §4 item 5): a save/load while another is
    // in flight is refused. The slot is claimed at dispatch and released when
    // the last job finishes (finish_async_op).
    std::shared_ptr<AsyncOpState> begin_async_op(std::string& errorOut) {
        std::lock_guard<std::mutex> lock(asyncMutex_);
        if (asyncOp_) {
            errorOut = "an async save/load is already in progress";
            return nullptr;
        }
        auto op = std::make_shared<AsyncOpState>();
        asyncOp_ = op;
        return op;
    }

    void finish_async_op(const std::shared_ptr<AsyncOpState>& op) {
        std::function<void(bool, std::string)> onDone;
        {
            std::lock_guard<std::mutex> lock(asyncMutex_);
            if (asyncOp_ == op) asyncOp_.reset();
        }
        {
            std::lock_guard<std::mutex> lock(op->mutex);
            op->finished = true;
            onDone = std::move(op->onDone);
        }
        op->cv.notify_all();
        if (onDone) onDone(op->ok, op->error);
    }

    bool wait_async_op(const std::shared_ptr<AsyncOpState>& op,
                       std::string& errorOut) {
        std::unique_lock<std::mutex> lock(op->mutex);
        op->cv.wait(lock, [&] { return op->finished; });
        if (!op->ok) {
            errorOut = op->error;
            return false;
        }
        return true;
    }

    // Claims the op slot, captures the consistent snapshot, plans tiles/delta/
    // manifest, then runs encode + page writes as background jobs. With a null
    // onDone it behaves like the synchronous save (waits for completion); with
    // a callback it returns immediately after dispatch (result via callback or
    // wait_async_saves).
    bool dispatch_region_save(
        const std::string& filePath, std::string& errorOut,
        std::function<void(bool, std::string)> onDone) {
#if VC_ENABLE_FLATBUFFERS
        const bool isAsync = static_cast<bool>(onDone);
        auto op = begin_async_op(errorOut);
        if (!op) return false;
        auto failDispatch = [this, &op, &errorOut](const std::string& message) {
            errorOut = message;
            {
                std::lock_guard<std::mutex> lock(op->mutex);
                op->ok = false;
                op->error = message;
            }
            finish_async_op(op);
        };

        // Phase 1 (brief, under the chunk mutex — FALTANTES §4 item 4): wait
        // out background generation, then copy each loaded chunk's data +
        // revision. The snapshot is a point-in-time: edits landing after
        // capture are persisted by the NEXT save.
        world_.threadPool.wait_idle();
        auto holder = std::make_shared<std::vector<ChunkSaveSnapshot>>();
        {
            std::lock_guard<std::recursive_mutex> lock(world_.chunksMutex);
            holder->reserve(world_.chunks.size());
            const std::size_t layerBytes =
                static_cast<std::size_t>(CHUNK_SIZE_X) * CHUNK_SIZE_Z;
            for (const auto& [key, chunk] : world_.chunks) {
                if (!chunk) continue;
                ChunkSaveSnapshot snap;
                snap.cx = key.first;
                snap.cz = key.second;
                snap.revision = chunk->revision();
                snap.extent =
                    std::clamp(chunk->vertical_render_extent(), 1, CHUNK_SIZE_Y);
                const std::size_t voxelBytes =
                    static_cast<std::size_t>(snap.extent) * layerBytes;
                snap.blocks.reserve(voxelBytes);
                snap.fluid.reserve(voxelBytes);
                snap.stateIndices.reserve(voxelBytes);
                for (int y = 0; y < snap.extent; ++y) {
                    for (int z = 0; z < CHUNK_SIZE_Z; ++z) {
                        for (int x = 0; x < CHUNK_SIZE_X; ++x) {
                            snap.blocks.push_back(chunk->get_block(x, y, z));
                            snap.fluid.push_back(static_cast<uint8_t>(
                                chunk->get_fluid_level(x, y, z)));
                            snap.stateIndices.push_back(chunk->get_state(x, y, z));
                        }
                    }
                }
                holder->push_back(std::move(snap));
            }
        }

        // Phase 2 (NO chunk mutex): group into tiles, gate unchanged tiles
        // (delta), build the manifest, dispatch encode + page writes as jobs.
        std::map<std::pair<int, int>, std::vector<const ChunkSaveSnapshot*>>
            regions;
        for (const ChunkSaveSnapshot& snap : *holder) {
            const std::pair<int, int> tile{
                floor_div(snap.cx, kRegionChunks),
                floor_div(snap.cz, kRegionChunks)
            };
            regions[tile].push_back(&snap);
        }
        if (!storage_->save_world(filePath, errorOut)) {
            failDispatch(errorOut);
            return false;
        }

        // Delta pages (FALTANTES §4 item 3): only tiles holding chunks whose
        // revision changed since the last save are re-encoded and rewritten;
        // unchanged tiles keep their pages untouched on disk.
        std::vector<std::pair<int, int>> changedTiles;
        for (const auto& [tile, snaps] : regions) {
            bool changed = false;
            for (const ChunkSaveSnapshot* snap : snaps) {
                const std::pair<int, int> key{ snap->cx, snap->cz };
                const auto found = [&] {
                    std::lock_guard<std::mutex> lock(savedRevisionMutex_);
                    return savedRevision_.find(key);
                }();
                if (found == savedRevision_.end() ||
                    found->second != snap->revision) {
                    changed = true;
                    break;
                }
            }
            if (changed) changedTiles.push_back(tile);
        }

        std::vector<std::pair<int, int>> regionList;
        regionList.reserve(regions.size());
        for (const auto& [tile, snaps] : regions) regionList.push_back(tile);
        const std::string manifest =
            build_region_manifest_body(regionList, errorOut);
        if (manifest.empty()) {
            failDispatch(errorOut);
            return false;
        }

        // One job per changed tile + one for the manifest. The last job to
        // finish records the result, updates the delta bookkeeping on success
        // and releases the op slot.
        op->pending = 1 + static_cast<int>(changedTiles.size());
        {
            std::lock_guard<std::mutex> lock(op->mutex);
            op->onDone = std::move(onDone);
        }
        auto job_done = [this, op, holder](bool ok, std::string err) {
            bool last = false;
            bool allOk = false;
            {
                std::lock_guard<std::mutex> lock(op->mutex);
                if (!ok) {
                    op->ok = false;
                    if (op->error.empty()) op->error = std::move(err);
                }
                --op->pending;
                last = op->pending == 0;
                allOk = op->ok;
            }
            if (last) {
                if (allOk) {
                    // Remember the saved revision of every loaded chunk so the
                    // next save compares against what was just persisted, and
                    // release the "unsaved edits" pin on chunks that a save
                    // persisted (FALTANTES §4 item 6: only chunks whose
                    // revision is still the saved one — a mid-save edit bumps
                    // the revision and keeps the pin).
                    std::vector<std::pair<std::pair<int, int>, uint64_t>>
                        savedChunks;
                    savedChunks.reserve(holder->size());
                    {
                        std::lock_guard<std::mutex> lock(savedRevisionMutex_);
                        for (const ChunkSaveSnapshot& snap : *holder) {
                            savedRevision_[{ snap.cx, snap.cz }] = snap.revision;
                            savedChunks.push_back(
                                { { snap.cx, snap.cz }, snap.revision });
                        }
                    }
                    world_.mark_chunks_saved(savedChunks);
                    // WAL commit (FALTANTES §4 item 8): the save wrote every
                    // page; make it permanent by dropping the journal. A
                    // failure here means the journal could not be removed —
                    // the data is on disk but the next load would roll it
                    // back, so report the save as failed.
                    std::string commitErr;
                    if (!storage_->commit_save(commitErr)) {
                        op->ok = false;
                        if (op->error.empty()) op->error = std::move(commitErr);
                    }
                }
                finish_async_op(op);
            }
        };
        world_.threadPool.enqueue([this, op, manifest, job_done]() mutable {
            std::string err;
            const bool ok = storage_->save_page("world", manifest, err);
            job_done(ok, std::move(err));
        });
        for (const auto& tile : changedTiles) {
            const std::vector<const ChunkSaveSnapshot*>& snaps = regions.at(tile);
            const std::string pageId =
                region_page_id(tile.first, tile.second);
            world_.threadPool.enqueue(
                [this, op, snaps, pageId, job_done]() mutable {
                std::string err;
                const std::string payload = encode_region_payload(snaps);
                const bool ok = storage_->save_page(pageId, payload, err);
                job_done(ok, std::move(err));
            });
        }
        errorOut.clear();
        if (isAsync) return true;
        return wait_async_op(op, errorOut);
#else
        errorOut = "region-paged saves require VC_ENABLE_FLATBUFFERS";
        return false;
#endif
    }

    // Restores a paged save: manifest first (palette/entities/world entities,
    // no chunks), then each region page's chunks in batch.
    bool load_world_regions(const std::string& filePath, std::string& errorOut) {
#if VC_ENABLE_FLATBUFFERS
        // FALTANTES §4 item 20: o SLOT ÚNICO de op também vale para o load
        // SINCRONO — um load durante um save/load async em voo é recusado
        // (antes, só o caminho async checava; o sync rodava em paralelo com o
        // job e corrompia o mundo em voo).
        auto op = begin_async_op(errorOut);
        if (!op) return false;
        const bool ok = load_world_regions_impl(filePath, errorOut);
        finish_async_op(op);
        return ok;
#else
        errorOut = "region-paged loads require VC_ENABLE_FLATBUFFERS";
        return false;
#endif
    }

    // Core load (slot-free): o chamador já segura o slot. Compartilhado pelo
    // caminho sync (claim + executa + finish) e pelo async (claim + job).
    bool load_world_regions_impl(const std::string& filePath, std::string& errorOut) {
        RestoreModeGuard restoreGuard(*this, world_);
        LoadRollback loadRollback(*this, world_);
        if (!storage_->load_world(filePath, errorOut)) return false;
        std::string manifest;
        if (!storage_->load_page("world", manifest, errorOut)) return false;
        // The manifest is a v5 body with zero chunks: the shared deserializer
        // applies palette + block entities + world entities (all-or-nothing),
        // and region pages restore the chunk data.
        if (!deserialize_world_v5(manifest, errorOut)) return false;
        std::vector<std::pair<int, int>> regions;
        if (!manifest_region_list(manifest, regions, errorOut)) return false;
        for (const auto& [rx, rz] : regions) {
            std::string payload;
            if (!storage_->load_page(region_page_id(rx, rz), payload, errorOut)) {
                return false;
            }
            if (!decode_region_payload(payload, errorOut)) return false;
        }
        errorOut.clear();
        loadRollback.commit();
        reset_session_history();
        return true;
    }

    // Async load (FALTANTES §4 item 5): the file reads, decompression and
    // chunk apply run as ONE background job on the world's thread pool; the
    // result arrives via the callback (pool thread) or wait_async_saves. The
    // caller must not touch the world while the load is in flight (chunks are
    // applied from a background thread).
    bool load_world_regions_async(
        const std::string& filePath,
        std::function<void(bool, std::string)> onDone,
        std::string& errorOut) {
#if VC_ENABLE_FLATBUFFERS
        const bool isAsync = static_cast<bool>(onDone);
        auto op = begin_async_op(errorOut);
        if (!op) return false;
        op->pending = 1;
        {
            std::lock_guard<std::mutex> lock(op->mutex);
            op->onDone = std::move(onDone);
        }
        auto job_done = [this, op](bool ok, std::string err) {
            bool last = false;
            {
                std::lock_guard<std::mutex> lock(op->mutex);
                if (!ok) {
                    op->ok = false;
                    if (op->error.empty()) op->error = std::move(err);
                }
                --op->pending;
                last = op->pending == 0;
            }
            if (last) finish_async_op(op);
        };
        world_.threadPool.enqueue([this, op, filePath, job_done]() mutable {
            std::string err;
            const bool ok = load_world_regions_impl(filePath, err);
            job_done(ok, std::move(err));
        });
        errorOut.clear();
        if (isAsync) return true;
        return wait_async_op(op, errorOut);
#else
        errorOut = "region-paged loads require VC_ENABLE_FLATBUFFERS";
        return false;
#endif
    }

    // v5 manifest page: palette + block entities + world entities + region
    // list, NO chunk data (chunks live in the region pages). Mirrors the v5
    // builder in serialize_world (reference implementation); the round-trip
    // test guards both against drift.
    std::string build_region_manifest_body(
        const std::vector<std::pair<int, int>>& regions, std::string& errorOut) const {
#if VC_ENABLE_FLATBUFFERS
        const std::vector<std::pair<RuntimeBlockId, RuntimeBlockInfo>> palette =
            world_.runtime_block_table();
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
            if (!snapshot.stableId.empty()) {
                entry.add_stable_id(builder.CreateString(snapshot.stableId));
            }
            worldEntityOffsets.push_back(entry.Finish());
        }
        const auto worldEntityVec = builder.CreateVector(worldEntityOffsets);

        std::vector<flatbuffers::Offset<engine::voxel::save::RegionEntry>>
            regionOffsets;
        regionOffsets.reserve(regions.size());
        for (const auto& [rx, rz] : regions) {
            engine::voxel::save::RegionEntryBuilder entry(builder);
            entry.add_x(rx);
            entry.add_z(rz);
            regionOffsets.push_back(entry.Finish());
        }
        const auto regionVec = builder.CreateVector(regionOffsets);

        // Scheduler capture (FALTANTES §6 item 129): the paged manifest rides
        // the same opaque scheduler blob so load_world_regions restores the
        // fixed-tick clock for dedicated/headless servers.
        const std::vector<std::byte> schedulerRaw =
            world_.scheduler().serialize_state();
        std::vector<uint8_t> schedulerBytes;
        schedulerBytes.reserve(schedulerRaw.size());
        for (const std::byte b : schedulerRaw) {
            schedulerBytes.push_back(static_cast<uint8_t>(b));
        }
        const auto schedulerVec = schedulerBytes.empty()
            ? builder.CreateVector(static_cast<const uint8_t*>(nullptr), 0)
            : builder.CreateVector(schedulerBytes);

        // World identity + content provenance (FALTANTES §4 item 4): the
        // paged manifest carries the same meta as the monolithic save so
        // load_world_regions restores seed/name/rules/plugins and the
        // registry fingerprint identically.
        const auto meta = build_meta_offset(
            builder, metadata_.seed, metadata_.worldName,
            metadata_.rulesJson, metadata_.pluginVersions,
            compute_registry_version(*registry_));

        engine::voxel::save::WorldSaveBuilder save(builder);
        save.add_palette(paletteVec);
        save.add_entities(entityVec);
        save.add_world_entities(worldEntityVec);
        save.add_regions(regionVec);
        save.add_scheduler_state(schedulerVec);
        save.add_meta(meta);
        builder.Finish(save.Finish(), "WLD5");

        std::string body;
        body.append(kWorldMagic, 5);
        append_u32(body, kWorldSaveVersion);
        body.append(reinterpret_cast<const char*>(builder.GetBufferPointer()),
                    builder.GetSize());
        append(body, hash_->hash(body));
        return body;
#else
        (void)regions;
        errorOut = "region manifest requires VC_ENABLE_FLATBUFFERS";
        return {};
#endif
    }

    // One region page: "VCWR" + u32 count + per chunk (i32 cx, i32 cz, u32
    // extent, u16 blocks[], u8 fluid[]) in y,z,x order — the same layout as the
    // v4 binary body. Caller holds world_.chunksMutex.
    // Encodes the frozen chunks of one region tile (layout v2 "VCW2",
    // FALTANTES §4 item 2): per chunk — header (cx, cz, extent, flags),
    // optional palette (u16 count + u16 runtime ids), then a u32 payload size
    // and the payload — palette-index bytes (u8, when the chunk has <= 256
    // distinct blocks) or raw u16 ids, plus the fluid bytes — optionally
    // zstd-compressed as a whole (only when it shrinks). Reads the SNAPSHOT,
    // not the live world (item 4: the caller does not hold the chunk mutex).
    std::string encode_region_payload(
        const std::vector<const ChunkSaveSnapshot*>& chunkSnapshots) const {
        std::string payload;
        payload.append(kRegionMagicV2, 4);
        append_u32(payload, static_cast<uint32_t>(chunkSnapshots.size()));
        const std::size_t layerBytes =
            static_cast<std::size_t>(CHUNK_SIZE_X) * CHUNK_SIZE_Z;
        for (const ChunkSaveSnapshot* snap : chunkSnapshots) {
            const int cx = snap->cx;
            const int cz = snap->cz;
            const int extent = snap->extent;
            const std::size_t voxelBytes =
                static_cast<std::size_t>(extent) * layerBytes;

            // Per-chunk palette: distinct runtime ids (sorted, deterministic)
            // -> index. Index bytes halve the block payload when the chunk has
            // <= 256 distinct blocks (the common case for terrain).
            std::set<RuntimeBlockId> distinct;
            for (const RuntimeBlockId id : snap->blocks) distinct.insert(id);
            const bool usePalette =
                !distinct.empty() && distinct.size() <= 256;
            std::map<RuntimeBlockId, uint16_t> indexOf;
            if (usePalette) {
                uint16_t next = 0;
                for (const RuntimeBlockId id : distinct) indexOf[id] = next++;
            }

            std::string inner;
            inner.reserve((usePalette ? voxelBytes : voxelBytes * 2) +
                          voxelBytes);
            if (usePalette) {
                for (const RuntimeBlockId id : snap->blocks) {
                    inner.push_back(static_cast<char>(
                        static_cast<uint8_t>(indexOf.at(id))));
                }
            } else {
                for (const RuntimeBlockId id : snap->blocks) {
                    append_u16(inner, id);
                }
            }
            inner.append(reinterpret_cast<const char*>(snap->fluid.data()),
                         snap->fluid.size());
            const std::string compressed =
                compression_ ? compression_->compress(inner) : std::string();
            const bool useCompression =
                !compressed.empty() && compressed.size() < inner.size();
            const std::string& body = useCompression ? compressed : inner;

            append_i32(payload, cx);
            append_i32(payload, cz);
            append_u32(payload, static_cast<uint32_t>(extent));
            uint8_t flags = 0;
            if (usePalette) flags |= kRegionFlagPalette;
            if (useCompression) flags |= kRegionFlagCompressed;
            payload.push_back(static_cast<char>(flags));
            if (usePalette) {
                append_u16(payload, static_cast<uint16_t>(distinct.size()));
                for (const RuntimeBlockId id : distinct) append_u16(payload, id);
            }
            append_u32(payload, static_cast<uint32_t>(body.size()));
            payload.append(body);
        }
        return payload;
    }

    // Parses a region page (layout v1 or v2) and restores its chunks in batch
    // (one invalidation per chunk, through the shared apply_saved_chunk path).
    bool decode_region_payload(const std::string& payload, std::string& errorOut) {
        if (payload.size() >= 4 && payload.compare(0, 4, kRegionMagicV2) == 0) {
            return decode_region_payload_v2(payload, errorOut);
        }
        if (payload.size() >= 4 && payload.compare(0, 4, kRegionMagic) == 0) {
            return decode_region_payload_v1(payload, errorOut);
        }
        errorOut = "region page corrupt (bad magic)";
        return false;
    }

    // Layout v1: raw u16 block ids + fluid bytes (pre-palette/compression).
    bool decode_region_payload_v1(const std::string& payload,
                                  std::string& errorOut) {
        const std::size_t layerBytes =
            static_cast<std::size_t>(CHUNK_SIZE_X) * CHUNK_SIZE_Z;
        std::size_t offset = 4;
        const uint32_t count = read_u32(payload, offset);
        offset += 4;
        for (uint32_t i = 0; i < count; ++i) {
            if (offset + 12 > payload.size()) {
                errorOut = "region page corrupt (truncated chunk header)";
                return false;
            }
            const int32_t cx = static_cast<int32_t>(read_u32(payload, offset));
            const int32_t cz = static_cast<int32_t>(read_u32(payload, offset + 4));
            const uint32_t extent = read_u32(payload, offset + 8);
            offset += 12;
            if (extent == 0 || extent > static_cast<uint32_t>(CHUNK_SIZE_Y)) {
                errorOut = "region page corrupt (bad extent)";
                return false;
            }
            const std::size_t voxelBytes =
                static_cast<std::size_t>(extent) * layerBytes;
            if (offset + voxelBytes * 2 + voxelBytes > payload.size()) {
                errorOut = "region page corrupt (truncated chunk data)";
                return false;
            }
            std::vector<RuntimeBlockId> blockIds;
            blockIds.reserve(voxelBytes);
            for (std::size_t v = 0; v < voxelBytes; ++v) {
                const RuntimeBlockId id =
                    static_cast<RuntimeBlockId>(read_u16(payload, offset + v * 2));
                if (!world_.is_valid_block_id(id)) {
                    errorOut = "region page: chunk (" + std::to_string(cx) +
                               ',' + std::to_string(cz) +
                               ") contains unknown block id " +
                               std::to_string(id);
                    return false;
                }
                blockIds.push_back(id);
            }
            offset += voxelBytes * 2;
            std::string water(payload.data() + offset, voxelBytes);
            offset += voxelBytes;
            if (!restore_decoded_chunk(cx, cz, static_cast<int>(extent),
                                       blockIds, water, errorOut)) {
                return false;
            }
        }
        return true;
    }

    // Layout v2: flags byte, optional palette, u32 payload size, payload
    // (palette indices or raw ids + fluid), optionally a zstd frame.
    bool decode_region_payload_v2(const std::string& payload,
                                  std::string& errorOut) {
        const std::size_t layerBytes =
            static_cast<std::size_t>(CHUNK_SIZE_X) * CHUNK_SIZE_Z;
        std::size_t offset = 4;
        const uint32_t count = read_u32(payload, offset);
        offset += 4;
        for (uint32_t i = 0; i < count; ++i) {
            if (offset + 13 > payload.size()) {
                errorOut = "region page corrupt (truncated chunk header)";
                return false;
            }
            const int32_t cx = static_cast<int32_t>(read_u32(payload, offset));
            const int32_t cz = static_cast<int32_t>(read_u32(payload, offset + 4));
            const uint32_t extent = read_u32(payload, offset + 8);
            const uint8_t flags = static_cast<uint8_t>(payload[offset + 12]);
            offset += 13;
            if (extent == 0 || extent > static_cast<uint32_t>(CHUNK_SIZE_Y)) {
                errorOut = "region page corrupt (bad extent)";
                return false;
            }
            const bool hasPalette =
                (flags & kRegionFlagPalette) != 0;
            const bool hasCompression =
                (flags & kRegionFlagCompressed) != 0;

            // Palette: u16 count + count runtime ids, each validated.
            std::vector<RuntimeBlockId> palette;
            if (hasPalette) {
                if (offset + 2 > payload.size()) {
                    errorOut = "region page corrupt (truncated palette)";
                    return false;
                }
                const uint16_t paletteCount = read_u16(payload, offset);
                offset += 2;
                if (paletteCount == 0 ||
                    offset + static_cast<std::size_t>(paletteCount) * 2 >
                        payload.size()) {
                    errorOut = "region page corrupt (truncated palette)";
                    return false;
                }
                palette.reserve(paletteCount);
                for (uint16_t p = 0; p < paletteCount; ++p) {
                    const RuntimeBlockId id = static_cast<RuntimeBlockId>(
                        read_u16(payload, offset + static_cast<std::size_t>(p) * 2));
                    if (!world_.is_valid_block_id(id)) {
                        errorOut = "region page: chunk (" + std::to_string(cx) +
                                   ',' + std::to_string(cz) +
                                   ") palette holds unknown block id " +
                                   std::to_string(id);
                        return false;
                    }
                    palette.push_back(id);
                }
                offset += static_cast<std::size_t>(paletteCount) * 2;
            }

            const std::size_t voxelBytes =
                static_cast<std::size_t>(extent) * layerBytes;
            if (offset + 4 > payload.size()) {
                errorOut = "region page corrupt (truncated payload size)";
                return false;
            }
            const uint32_t payloadBytes = read_u32(payload, offset);
            offset += 4;
            if (offset + payloadBytes > payload.size()) {
                errorOut = "region page corrupt (truncated chunk data)";
                return false;
            }
            const std::string blob = payload.substr(offset, payloadBytes);
            offset += payloadBytes;

            // Decompress when flagged; a corrupt frame fails cleanly.
            std::string inner;
            if (hasCompression) {
                if (!compression_) {
                    errorOut = "region page: chunk (" + std::to_string(cx) +
                               ',' + std::to_string(cz) +
                               ") is compressed but no provider is registered";
                    return false;
                }
                inner = compression_->decompress(blob);
                if (inner.empty()) {
                    errorOut = "region page: chunk (" + std::to_string(cx) +
                               ',' + std::to_string(cz) +
                               ") failed to decompress (corrupt frame)";
                    return false;
                }
            } else {
                inner = blob;
            }
            const std::size_t expected =
                (hasPalette ? voxelBytes : voxelBytes * 2) + voxelBytes;
            if (inner.size() != expected) {
                errorOut = "region page: chunk (" + std::to_string(cx) +
                           ',' + std::to_string(cz) +
                           ") payload size mismatch (expected " +
                           std::to_string(expected) + ", got " +
                           std::to_string(inner.size()) + ")";
                return false;
            }

            std::vector<RuntimeBlockId> blockIds;
            blockIds.reserve(voxelBytes);
            if (hasPalette) {
                for (std::size_t v = 0; v < voxelBytes; ++v) {
                    const uint8_t index =
                        static_cast<uint8_t>(inner[v]);
                    if (index >= palette.size()) {
                        errorOut = "region page: chunk (" + std::to_string(cx) +
                                   ',' + std::to_string(cz) +
                                   ") palette index " + std::to_string(index) +
                                   " out of range (" +
                                   std::to_string(palette.size()) + ")";
                        return false;
                    }
                    blockIds.push_back(palette[index]);
                }
            } else {
                for (std::size_t v = 0; v < voxelBytes; ++v) {
                    const RuntimeBlockId id = static_cast<RuntimeBlockId>(
                        read_u16(inner, v * 2));
                    if (!world_.is_valid_block_id(id)) {
                        errorOut = "region page: chunk (" + std::to_string(cx) +
                                   ',' + std::to_string(cz) +
                                   ") contains unknown block id " +
                                   std::to_string(id);
                        return false;
                    }
                    blockIds.push_back(id);
                }
            }
            std::string water(inner.data() +
                                  (hasPalette ? voxelBytes : voxelBytes * 2),
                              voxelBytes);
            if (!restore_decoded_chunk(cx, cz, static_cast<int>(extent),
                                       blockIds, water, errorOut)) {
                return false;
            }
        }
        return true;
    }

    // Shared restore tail of the region decoders (v1/v2).
    bool restore_decoded_chunk(int cx, int cz, int extent,
                               std::vector<RuntimeBlockId>& blockIds,
                               const std::string& water,
                               std::string& errorOut) {
        std::vector<uint8_t> stateIndicesRegion;
        if (!apply_saved_chunk(cx, cz, extent, blockIds, water, stateIndicesRegion)) {
            errorOut = "region page: chunk (" + std::to_string(cx) +
                       ',' + std::to_string(cz) + ") could not be loaded";
            return false;
        }
        return true;
    }

    // Reads the region list out of a v5 manifest body (chunks empty).
    bool manifest_region_list(const std::string& body,
                              std::vector<std::pair<int, int>>& regionsOut,
                              std::string& errorOut) const {
#if VC_ENABLE_FLATBUFFERS
        const std::size_t prefix = 5 + 4;
        if (body.size() < prefix + 4) {
            errorOut = "region manifest too small";
            return false;
        }
        const char* buffer = body.data() + prefix;
        const std::size_t bufferSize = body.size() - prefix;
        flatbuffers::Verifier verifier(
            reinterpret_cast<const uint8_t*>(buffer), bufferSize);
        if (!engine::voxel::save::VerifyWorldSaveBuffer(verifier)) {
            errorOut = "region manifest corrupt (FlatBuffers verification failed)";
            return false;
        }
        const engine::voxel::save::WorldSave* save =
            engine::voxel::save::GetWorldSave(buffer);
        if (!save || save->regions() == nullptr) {
            errorOut = "region manifest corrupt (no region list)";
            return false;
        }
        regionsOut.reserve(save->regions()->size());
        for (flatbuffers::uoffset_t i = 0; i < save->regions()->size(); ++i) {
            const engine::voxel::save::RegionEntry* entry = save->regions()->Get(i);
            regionsOut.emplace_back(entry->x(), entry->z());
        }
        return true;
#else
        (void)body;
        errorOut = "region manifest requires VC_ENABLE_FLATBUFFERS";
        return false;
#endif
    }

    void notify(engine::voxel::TransactionEvent::Kind kind, std::size_t editCount) {
        if (!listener_) return;
        engine::voxel::TransactionEvent event;
        event.kind = kind;
        event.editCount = editCount;
        event.undoDepth = undoStack_.size();
        listener_(event);
    }

    // FALTANTES §7 item 140: undo/redo history is SESSION-SCOPED. A successful
    // world load replaces the world content, so the previous session's edit
    // history is meaningless (an undo would revert against freshly loaded
    // blocks). Every load success path clears the stacks and the edit log.
    void reset_session_history() {
        undoStack_.clear();
        redoStack_.clear();
        editLog_ = 0;
    }

    // Derives the world's dynamic block table from the registry and pushes it,
    // merging the registered mesher plugin's per-block policy overrides
    // (FALTANTES §3 item 2). Called at construction, on set_block_registry and
    // on register_mesher.
    void push_runtime_table() {
        auto table = build_runtime_block_table(*registry_);
        if (mesher_) {
            for (const auto& [id, policy] : mesher_->mesh_policy_overrides()) {
                const RuntimeBlockId runtimeId = static_cast<RuntimeBlockId>(id & 0xFFFF);
                auto found = table.first.find(runtimeId);
                if (found == table.first.end()) continue;  // not a dynamic block
                found->second.occludes = policy.occludes;
                found->second.transparent = policy.transparent;
                found->second.renderLayer = policy.renderLayer;
            }
        }
        world_.set_runtime_block_table(std::move(table.first), std::move(table.second));
    }

    World world_;
    NullBridge bridge_;
    glm::vec3 playerPos_{ 8.0f, 200.0f, 8.0f };

    // Streaming/budget observability (FALTANTES §3): optional push monitor +
    // last snapshot for change-gating the dispatch.
    std::shared_ptr<engine::voxel::IVoxelStreamingMonitor> monitor_;
    engine::voxel::StreamingSnapshot lastSnapshot_;

    // World identity + content provenance (FALTANTES §4 item 4): the project's
    // metadata (seed/name/rules/plugin versions) stored on the world, ridden
    // with every v5 save and restored on load. savedRegistryVersion_ is the
    // block-registry fingerprint carried by the last loaded save (0 when
    // nothing was loaded or the save had no metadata).
    engine::voxel::IVoxelWorld::WorldMetadata metadata_;
    uint64_t savedRegistryVersion_{ 0 };

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

    // Delta persistence (FALTANTES §4 item 3): revision of each chunk at the
    // last region save. A chunk is "changed" when its revision moved (edits,
    // fluid writes, generation) or the key is new; unchanged tiles are not
    // rewritten on save. Grows with the loaded window; stale entries for
    // evicted chunks are harmless (replaced on the next save of that key).
    // Written by background save jobs (job_done) and read by the caller
    // (delta gate, autosave trigger) — the mutex makes the map safe across
    // those threads.
    mutable std::mutex savedRevisionMutex_;
    std::map<std::pair<int, int>, uint64_t> savedRevision_;

    // Autosave (FALTANTES §4 item 7): the config, the autosave path and the
    // elapsed-time accumulator driven by update(). A fire dispatches a delta
    // async save (items 3 + 5) to autosavePath_; refused fires (another op in
    // flight) leave the timers running so the next eligible update retries.
    engine::voxel::IVoxelWorld::AutosaveConfig autosaveConfig_;
    std::string autosavePath_;
    double autosaveElapsed_{ 0.0 };
    // >0 while a load is restoring chunks (RestoreModeGuard): update() runs
    // inside load waits, and an autosave fired then would snapshot a
    // half-restored world. Autosave is suppressed while restoring.
    int restoringDepth_{ 0 };

    // Async persistence (FALTANTES §4 item 5): the single in-flight op slot
    // and the guard for it. begin_async_op claims, finish_async_op releases.
    std::shared_ptr<AsyncOpState> asyncOp_;
    std::mutex asyncMutex_;

    // Transaction state: applied edit lists (undo), undone lists (redo), the
    // committed-edit log counter and the optional event listener.
    std::vector<std::vector<engine::voxel::BlockEdit>> undoStack_;
    std::vector<std::vector<engine::voxel::BlockEdit>> redoStack_;
    std::size_t editLog_{ 0 };
    std::function<void(const engine::voxel::TransactionEvent&)> listener_;
    // Permissions/limits (FALTANTES §7 item 138): the registered policy is the
    // permission authority; structural limits bound edit count and box volume.
    engine::voxel::TransactionLimits transactionLimits_;
    std::shared_ptr<engine::voxel::ITransactionPolicy> transactionPolicy_;
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
