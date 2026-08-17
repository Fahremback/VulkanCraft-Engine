// WorldManager.cpp — the only translation unit implementing the public
// IWorldManager (META section 15 / FALTANTES item 8). Worlds are independent
// IVoxelWorld instances with per-world seed/clock/rules/persistence; entity
// transfer is transactional (all-or-nothing: the source is only mutated after
// the destination accepts the rebuild); portals are generic position mappings
// between two world spaces. Pure composition of the public voxel/entity layers
// — no backend of its own.

#include "engine/world/IWorldManager.hpp"

#include "RegistryJson.hpp"

#include <cmath>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine {
namespace world {
namespace {

// The reserved project component that carries a persistent cross-world
// reference (META §19). The blob is a tiny JSON document
// {"toWorld":"<name>","stableId":"<id>"}; the engine never interprets the
// payload beyond this — it travels with the entity's other components through
// the world save and the replication region.
constexpr const char* kWorldRefComponent = "engine.world_ref";

class WorldManagerImpl final : public IWorldManager {
public:
    // ---- World lifecycle ----------------------------------------------------

    bool create_world(const WorldSpec& spec, std::string& errorOut) override {
        return create_impl(spec, false, errorOut);
    }

    bool load_world(const WorldSpec& spec, std::string& errorOut) override {
        return create_impl(spec, true, errorOut);
    }

    bool unload_world(const std::string& name) override {
        return worlds_.erase(name) != 0;
    }

    bool has_world(const std::string& name) const override {
        return worlds_.count(name) != 0;
    }

    std::vector<std::string> world_names() const override {
        std::vector<std::string> names;
        names.reserve(worlds_.size());
        for (const auto& [name, entry] : worlds_) {
            (void)entry;
            names.push_back(name);
        }
        return names;
    }

    std::size_t world_count() const override { return worlds_.size(); }

    WorldInfo world_info(const std::string& name) const override {
        WorldInfo info;
        const auto it = worlds_.find(name);
        if (it == worlds_.end()) return info;
        info.name = it->second.spec.name;
        info.seed = it->second.spec.seed;
        info.rulesJson = it->second.spec.rulesJson;
        info.savePath = it->second.spec.savePath;
        info.loaded = true;
        info.elapsedSeconds = it->second.elapsed;
        const auto* entities = it->second.voxel->entity_world().get();
        info.entityCount = entities != nullptr ? entities->size() : 0;
        return info;
    }

    engine::voxel::IVoxelWorld* world(const std::string& name) override {
        const auto it = worlds_.find(name);
        return it == worlds_.end() ? nullptr : it->second.voxel.get();
    }

    const engine::voxel::IVoxelWorld* world(const std::string& name) const override {
        const auto it = worlds_.find(name);
        return it == worlds_.end() ? nullptr : it->second.voxel.get();
    }

    // ---- Per-world clock / persistence --------------------------------------

    bool update_world(const std::string& name, const glm::vec3& playerPosition,
                      float deltaTime) override {
        const auto it = worlds_.find(name);
        if (it == worlds_.end() || deltaTime <= 0.0f) return false;
        it->second.elapsed += deltaTime;
        it->second.voxel->update(playerPosition, deltaTime);
        return true;
    }

    bool save_world(const std::string& name, const std::string& path,
                    std::string& errorOut) override {
        const auto it = worlds_.find(name);
        if (it == worlds_.end()) {
            errorOut = "save: unknown world '" + name + "'";
            return false;
        }
        if (path.empty()) {
            errorOut = "save: empty path for world '" + name + "'";
            return false;
        }
        // Delegate with a FRESH error out: the world contract may leave the
        // out-param untouched on success (or append), so a stale caller error
        // must never leak into the verdict.
        std::string innerError;
        if (!it->second.voxel->save_world(path, innerError)) {
            errorOut = innerError.empty() ? "save: world '" + name + "' failed"
                                          : innerError;
            return false;
        }
        errorOut.clear();
        return true;
    }

    // ---- Transactional entity transfer --------------------------------------

    engine::entity::EntityId transfer_entity(
        const std::string& fromWorld, engine::entity::EntityId handle,
        const std::string& toWorld,
        const engine::entity::Position& targetPosition,
        std::string& errorOut) override {
        const auto fromIt = worlds_.find(fromWorld);
        if (fromIt == worlds_.end()) {
            errorOut = "transfer: unknown source world '" + fromWorld + "'";
            return {};
        }
        const auto toIt = worlds_.find(toWorld);
        if (toIt == worlds_.end()) {
            errorOut = "transfer: unknown destination world '" + toWorld + "'";
            return {};
        }
        if (fromWorld == toWorld) {
            errorOut = "transfer: source and destination must differ";
            return {};
        }
        auto* fromEntities = fromIt->second.voxel->entity_world().get();
        auto* toEntities = toIt->second.voxel->entity_world().get();
        if (fromEntities == nullptr || toEntities == nullptr) {
            errorOut = "transfer: entity layer unavailable";
            return {};
        }
        if (!fromEntities->alive(handle)) {
            errorOut = "transfer: entity is not alive in source world";
            return {};
        }

        // Capture the full state BEFORE any mutation.
        const std::string type = fromEntities->type_of(handle);
        if (type.empty()) {
            errorOut = "transfer: entity has an empty type";
            return {};
        }
        engine::entity::Health health;
        if (!fromEntities->get_health(handle, health)) {
            errorOut = "transfer: cannot read entity health";
            return {};
        }
        float tickInterval = 0.0f;
        if (!fromEntities->get_tick_interval(handle, tickInterval)) {
            errorOut = "transfer: cannot read entity tick interval";
            return {};
        }
        std::vector<engine::entity::ComponentData> components;
        fromEntities->for_each_component(
            handle, [&components](const engine::entity::ComponentData& component) {
                components.push_back(component);
            });

        // Build in the destination FIRST — the source stays untouched until
        // the rebuild succeeds (all-or-nothing).
        std::string spawnError;
        const engine::entity::EntityId newHandle =
            toEntities->spawn(type, targetPosition, spawnError);
        if (!newHandle.valid()) {
            errorOut = "transfer: destination spawn failed: " + spawnError;
            return {};
        }
        bool ok = toEntities->set_health(newHandle, health);
        if (ok && tickInterval > 0.0f) {
            ok = toEntities->set_tick_interval(newHandle, tickInterval);
        }
        for (const engine::entity::ComponentData& component : components) {
            if (!ok) break;
            ok = toEntities->set_component(newHandle, component);
        }
        if (!ok) {
            toEntities->despawn(newHandle);  // roll back destination
            errorOut = "transfer: destination rebuild failed (rolled back)";
            return {};
        }

        // Commit: remove from the source. If that unexpectedly fails, roll the
        // destination back so the entity exists in exactly one world.
        if (!fromEntities->despawn(handle)) {
            toEntities->despawn(newHandle);
            errorOut = "transfer: source despawn failed (rolled back destination)";
            return {};
        }
        errorOut.clear();
        return newHandle;
    }

    // ---- Portals ------------------------------------------------------------

    uint32_t create_portal(const PortalSpec& spec, std::string& errorOut) override {
        if (spec.fromWorld.empty() || spec.toWorld.empty()) {
            errorOut = "portal: source and destination worlds are required";
            return 0;
        }
        if (spec.fromWorld == spec.toWorld) {
            errorOut = "portal: source and destination worlds must differ";
            return 0;
        }
        if (!worlds_.count(spec.fromWorld)) {
            errorOut = "portal: unknown source world '" + spec.fromWorld + "'";
            return 0;
        }
        if (!worlds_.count(spec.toWorld)) {
            errorOut = "portal: unknown destination world '" + spec.toWorld + "'";
            return 0;
        }
        const uint32_t id = nextPortal_++;
        portals_[id] = spec;
        errorOut.clear();
        return id;
    }

    bool remove_portal(uint32_t portalId) override {
        return portals_.erase(portalId) != 0;
    }

    bool portal(uint32_t portalId, PortalSpec& out) const override {
        const auto it = portals_.find(portalId);
        if (it == portals_.end()) return false;
        out = it->second;
        return true;
    }

    std::vector<uint32_t> portals() const override {
        std::vector<uint32_t> ids;
        ids.reserve(portals_.size());
        for (const auto& [id, spec] : portals_) {
            (void)spec;
            ids.push_back(id);
        }
        return ids;
    }

    // ---- Persistent references between worlds (META §19) -------------------

    bool set_entity_ref(const std::string& fromWorld,
                        engine::entity::EntityId fromEntity,
                        const WorldEntityRef& ref,
                        std::string& errorOut) override {
        const auto fromIt = worlds_.find(fromWorld);
        if (fromIt == worlds_.end()) {
            errorOut = "entity ref: unknown source world '" + fromWorld + "'";
            return false;
        }
        if (!ref.valid()) {
            errorOut = "entity ref: destination world and stable id are required";
            return false;
        }
        if (ref.toWorld == fromWorld) {
            errorOut = "entity ref: reference must cross worlds (same-world refused)";
            return false;
        }
        if (!worlds_.count(ref.toWorld)) {
            errorOut = "entity ref: unknown destination world '" + ref.toWorld + "'";
            return false;
        }
        auto* entities = fromIt->second.voxel->entity_world().get();
        if (entities == nullptr || !entities->alive(fromEntity)) {
            errorOut = "entity ref: source entity is not alive";
            return false;
        }
        // Stored as a reserved component on the entity: travels with the world
        // save and the replication region automatically (no separate registry
        // to keep in sync).
        engine::entity::ComponentData component;
        component.type = kWorldRefComponent;
        component.version = 1;
        component.blob = "{\"toWorld\":\"" + ref.toWorld +
                         "\",\"stableId\":\"" + ref.stableId + "\"}";
        if (!entities->set_component(fromEntity, component)) {
            errorOut = "entity ref: cannot store the reference component";
            return false;
        }
        errorOut.clear();
        return true;
    }

    WorldEntityRef entity_ref(const std::string& fromWorld,
                              engine::entity::EntityId fromEntity) const override {
        WorldEntityRef out;
        const auto it = worlds_.find(fromWorld);
        if (it == worlds_.end()) return out;
        const auto* entities = it->second.voxel->entity_world().get();
        if (entities == nullptr || !entities->alive(fromEntity)) return out;
        engine::entity::ComponentData component;
        if (!entities->get_component(fromEntity, kWorldRefComponent, component))
            return out;
        // Parse the reserved blob (best-effort; a malformed/stale blob simply
        // yields an invalid ref — the reference is project data).
        engine::sdk::JsonValue document;
        std::string jsonError;
        if (!engine::sdk::json_parse(component.blob, document, jsonError) ||
            !document.is_object()) {
            return out;
        }
        out.toWorld = engine::sdk::json_string(document, "toWorld", "");
        out.stableId = engine::sdk::json_string(document, "stableId", "");
        return out;
    }

    bool clear_entity_ref(const std::string& fromWorld,
                          engine::entity::EntityId fromEntity) override {
        const auto it = worlds_.find(fromWorld);
        if (it == worlds_.end()) return false;
        auto* entities = it->second.voxel->entity_world().get();
        if (entities == nullptr || !entities->alive(fromEntity)) return false;
        // Remove the reserved component (a missing one is a no-op -> true).
        return entities->remove_component(fromEntity, kWorldRefComponent);
    }

    engine::entity::EntityId resolve_entity_ref(
        const std::string& fromWorld, engine::entity::EntityId fromEntity,
        std::string& errorOut) override {
        const auto fromIt = worlds_.find(fromWorld);
        if (fromIt == worlds_.end()) {
            errorOut = "entity ref: unknown source world '" + fromWorld + "'";
            return {};
        }
        auto* sourceEntities = fromIt->second.voxel->entity_world().get();
        if (sourceEntities == nullptr || !sourceEntities->alive(fromEntity)) {
            errorOut = "entity ref: source entity is not alive";
            return {};
        }
        engine::entity::ComponentData component;
        if (!sourceEntities->get_component(fromEntity, kWorldRefComponent,
                                           component)) {
            errorOut = "entity ref: entity has no cross-world reference";
            return {};
        }
        const WorldEntityRef ref = entity_ref(fromWorld, fromEntity);
        if (!ref.valid()) {
            errorOut = "entity ref: malformed reference component";
            return {};
        }
        const auto toIt = worlds_.find(ref.toWorld);
        if (toIt == worlds_.end()) {
            errorOut = "entity ref: destination world '" + ref.toWorld +
                       "' is not loaded";
            return {};
        }
        auto* targetEntities = toIt->second.voxel->entity_world().get();
        if (targetEntities == nullptr) {
            errorOut = "entity ref: destination world has no entity layer";
            return {};
        }
        const engine::entity::EntityId target =
            targetEntities->entity_by_stable_id(ref.stableId);
        if (!target.valid()) {
            errorOut = "entity ref: stable id '" + ref.stableId +
                       "' is not alive in '" + ref.toWorld + "'";
            return {};
        }
        errorOut.clear();
        return target;
    }

    engine::entity::EntityId transfer_via_portal(
        const std::string& worldName, engine::entity::EntityId handle,
        uint32_t portalId, std::string& errorOut) override {
        const auto portalIt = portals_.find(portalId);
        if (portalIt == portals_.end()) {
            errorOut = "portal: unknown portal";
            return {};
        }
        const PortalSpec& portal = portalIt->second;
        if (portal.fromWorld != worldName) {
            errorOut = "portal: entity world '" + worldName +
                       "' does not match portal source '" + portal.fromWorld + "'";
            return {};
        }
        const auto it = worlds_.find(worldName);
        if (it == worlds_.end()) {
            errorOut = "portal: unknown source world";
            return {};
        }
        auto* entities = it->second.voxel->entity_world().get();
        if (entities == nullptr || !entities->alive(handle)) {
            errorOut = "portal: entity is not alive";
            return {};
        }
        engine::entity::Position pos;
        if (!entities->get_position(handle, pos)) {
            errorOut = "portal: cannot read entity position";
            return {};
        }
        // Map: local = pos - source anchor; rotate by yaw around Y;
        // target = destination anchor + rotated local.
        const float yawRad = portal.yawDegrees * (3.14159265358979323846f / 180.0f);
        const float cosY = std::cos(yawRad);
        const float sinY = std::sin(yawRad);
        const float lx = pos.x - portal.fromX;
        const float lz = pos.z - portal.fromZ;
        engine::entity::Position target;
        target.x = portal.toX + (cosY * lx - sinY * lz);
        target.y = portal.toY + (pos.y - portal.fromY);
        target.z = portal.toZ + (sinY * lx + cosY * lz);
        return transfer_entity(worldName, handle, portal.toWorld, target,
                               errorOut);
    }

private:
    struct WorldEntry {
        WorldSpec spec;
        std::unique_ptr<engine::voxel::IVoxelWorld> voxel;
        double elapsed{ 0.0 };
    };

    bool create_impl(const WorldSpec& spec, bool loadFromSave,
                     std::string& errorOut) {
        if (spec.name.empty()) {
            errorOut = "world: empty name";
            return false;
        }
        if (worlds_.count(spec.name) != 0) {
            errorOut = "world: '" + spec.name + "' already exists";
            return false;
        }
        if (!spec.rulesJson.empty()) {
            engine::sdk::JsonValue parsed;
            std::string jsonError;
            if (!engine::sdk::json_parse(spec.rulesJson, parsed, jsonError)) {
                errorOut = "world: '" + spec.name +
                           "' rules are not valid JSON: " + jsonError;
                return false;
            }
        }
        auto voxel = engine::voxel::create_default_voxel_world();
        if (!voxel) {
            errorOut = "world: failed to construct '" + spec.name + "'";
            return false;
        }
        if (loadFromSave) {
            if (spec.savePath.empty()) {
                errorOut = "world: '" + spec.name +
                           "' load requires a save path";
                return false;
            }
            // All-or-nothing: on load failure the world is NOT registered.
            std::string loadError;
            if (!voxel->load_world(spec.savePath, loadError)) {
                errorOut = loadError.empty()
                               ? "world: '" + spec.name + "' load failed"
                               : loadError;
                return false;
            }
        }
        WorldEntry entry;
        entry.spec = spec;
        entry.voxel = std::move(voxel);
        entry.elapsed = 0.0;
        worlds_.emplace(spec.name, std::move(entry));
        errorOut.clear();
        return true;
    }

    std::map<std::string, WorldEntry> worlds_;  // sorted: deterministic names
    std::unordered_map<uint32_t, PortalSpec> portals_;
    uint32_t nextPortal_{ 1 };
};

}  // namespace

std::unique_ptr<IWorldManager> create_world_manager() {
    return std::make_unique<WorldManagerImpl>();
}

}  // namespace world
}  // namespace engine
