#pragma once

// Public block entity contract (SDK, META section 8). Voxels with their own
// state (chest, furnace, sign, door, TNT, machine) are NOT full ECS entities:
// the ENGINE owns the infrastructure — storage, deterministic ticking through
// the world scheduler (budget + active regions + sleeping), atomic create/
// destroy with the block, versioned persistence framing and lifecycle events —
// while the PROJECT owns the behavior: on_tick decides what the entity does,
// and the state blob is opaque to the engine.
//
// No renderer details (Vulkan/VMA) appear here; the world runs headless.

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "engine/registry/Inventory.hpp"

namespace engine {
namespace voxel {

// An optional project-owned component attached to a block entity. Same shape
// as a project ECS component (registered type id + version + opaque blob) but
// WITHOUT turning the voxel into a full ECS entity: the engine only carries
// and frames it for persistence and hands it back; the project owns the
// semantics. This is how a chest gets "lockable" or a furnace gets "fuel" as
// data-driven capabilities without recompiling the engine.
struct BlockEntityComponent {
    std::string type;              // registered namespaced component type id
    uint32_t version{ 1 };         // project-owned version (migration)
    std::vector<uint8_t> blob;     // opaque payload
};

// A stateful voxel. Implemented by the project; the engine never interprets
// the state blob (it only frames it for persistence and hands it back).
class IVoxelBlockEntity {
public:
    virtual ~IVoxelBlockEntity() = default;

    // Stable namespaced type id, e.g. "project:furnace". Factories are
    // registered under this id so save/load can reconstruct entities.
    virtual std::string type_id() const = 0;

    // Project behavior. The engine calls this at most once per world tick per
    // entity, in deterministic position order, only for entities whose chunk
    // is inside the active region (others sleep). worldTick is the world's
    // fixed-tick counter (see the world scheduler).
    virtual void on_tick(uint64_t worldTick) = 0;

    // Persistence framing: version + opaque project blob. The engine writes
    // (type_id, data_version, blob) with the world save and reconstructs via
    // the registered factory on load. The project migrates its own versions.
    virtual uint32_t data_version() const = 0;
    virtual std::vector<uint8_t> serialize_state() const = 0;
    // false => the engine refuses the save (the world would be inconsistent
    // without the entity's data).
    virtual bool deserialize_state(const std::vector<uint8_t>& data,
                                   uint32_t version) = 0;

    // Optional lifecycle hooks: on_created fires after a successful attach;
    // on_destroyed fires when the block is removed or the entity detached.
    virtual void on_created() {}
    virtual void on_destroyed() {}

    // ---- Optional capabilities (default: none) ---------------------------
    // A block entity MAY expose structured capabilities so a project attaches
    // inventory / script / components to a voxel WITHOUT making every block a
    // full ECS entity. The engine only routes (tick / persistence framing /
    // lifecycle) and never interprets these; the project owns their semantics
    // and persists them through serialize_state / deserialize_state. Every
    // default below keeps a plain entity (e.g. a counter machine) lightweight.

    // The entity's authoritative inventory (chest/furnace/hopper...). Valid
    // for the entity's lifetime; nullptr when the entity has no inventory.
    virtual engine::registry::Inventory* inventory() { return nullptr; }
    virtual const engine::registry::Inventory* inventory() const {
        return nullptr;
    }

    // Optional project-owned script (namespaced id, e.g. "project:door_open").
    // Empty = no script.
    virtual std::string script_id() const { return std::string(); }

    // Optional project components (type + version + blob), in deterministic
    // order (sorted by type). Empty when the entity exposes none.
    virtual std::vector<BlockEntityComponent> components() const { return {}; }
};

// Factory used to reconstruct entities from a save. Registered per type id via
// IVoxelWorld::register_block_entity_type; an entity type with no factory
// cannot be attached (and a save referencing it is refused on load).
using BlockEntityFactory =
    std::function<std::shared_ptr<IVoxelBlockEntity>()>;

// Lifecycle notification for scripts, networking and editor tools.
struct BlockEntityEvent {
    enum class Kind { Attached, Detached };
    Kind kind{ Kind::Attached };
    glm::ivec3 position{ 0, 0, 0 };
    std::string typeId;
};

}  // namespace voxel
}  // namespace engine
