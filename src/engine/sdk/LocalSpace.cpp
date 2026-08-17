// LocalSpace.cpp
//
// The only implementation of engine::world::ILocalSpace (SDK, META §19 /
// FALTANTES §15 "espaços locais hierárquicos para planetas e veículos
// grandes"). Pure composition of the public layers (IWorldManager +
// IEntityWorld) — no backend, headless, deterministic.
//
// Model: a tree of local frames rooted at the world frame (""). Each space
// carries a rigid transform (position + yaw) relative to its parent. A space
// that moves carries every entity bound to it and its descendants — bindings
// are space-LOCAL, so only the transform changes. The binding is a reserved
// component (`engine.space_ref`, JSON `{"space":...}`) ON the entity: it
// persists with the world save and travels in the replication region with no
// separate registry. Conversions are exact in double; yaw rotates around +Y
// with cos/sin snapped to exact 0/±1 for cardinal angles (bit-exact 90°).

#include "engine/world/ILocalSpace.hpp"

#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace engine {
namespace world {

namespace {

// Snap a rotation coefficient to the exact cardinal value (0/±1) so 0/90/180/
// 270 degree yaws round-trip bit-exactly instead of through 6e-17 residue.
double snap_axis(double v) {
    if (std::fabs(v) < 1e-12) return 0.0;
    if (std::fabs(std::fabs(v) - 1.0) < 1e-12) {
        return v < 0.0 ? -1.0 : 1.0;
    }
    return v;
}

struct Space {
    std::string parent;      // "" = world frame root
    SpaceTransform transform;
};

}  // namespace

class LocalSpace final : public ILocalSpace {
public:
    explicit LocalSpace(IWorldManager& manager) : manager_(manager) {}

    // ---- Hierarchy ----------------------------------------------------------
    bool create_space(const std::string& name, const std::string& parentName,
                      const SpaceTransform& transform,
                      std::string& errorOut) override {
        if (name.empty()) {
            errorOut = "local space: empty space name";
            return false;
        }
        if (spaces_.count(name) != 0) {
            errorOut = "local space: duplicate space name '" + name + "'";
            return false;
        }
        if (!parentName.empty() && spaces_.count(parentName) == 0) {
            errorOut = "local space: unknown parent space '" + parentName + "'";
            return false;
        }
        Space space;
        space.parent = parentName;
        space.transform = transform;
        spaces_[name] = space;
        errorOut.clear();
        return true;
    }

    bool remove_space(const std::string& name,
                      std::string& errorOut) override {
        if (spaces_.count(name) == 0) {
            errorOut = "local space: unknown space '" + name + "'";
            return false;
        }
        for (const auto& entry : spaces_) {
            if (entry.second.parent == name) {
                errorOut = "local space: space '" + name +
                           "' still has children";
                return false;
            }
        }
        if (has_bound_entities(name)) {
            errorOut = "local space: space '" + name +
                       "' still has bound entities (unbind first)";
            return false;
        }
        spaces_.erase(name);
        errorOut.clear();
        return true;
    }

    bool space_exists(const std::string& name) const override {
        return spaces_.count(name) != 0;
    }

    std::string space_parent(const std::string& name) const override {
        const auto it = spaces_.find(name);
        return it == spaces_.end() ? std::string() : it->second.parent;
    }

    bool space_transform(const std::string& name,
                         SpaceTransform& out) const override {
        const auto it = spaces_.find(name);
        if (it == spaces_.end()) return false;
        out = it->second.transform;
        return true;
    }

    bool set_space_transform(const std::string& name,
                             const SpaceTransform& transform) override {
        auto it = spaces_.find(name);
        if (it == spaces_.end()) return false;
        it->second.transform = transform;
        return true;
    }

    bool move_space(const std::string& name,
                    const glm::dvec3& delta) override {
        auto it = spaces_.find(name);
        if (it == spaces_.end()) return false;
        it->second.transform.position += delta;
        return true;
    }

    // ---- Conversions --------------------------------------------------------
    bool space_to_world(const std::string& name, const glm::dvec3& local,
                        glm::dvec3& worldOut,
                        std::string& errorOut) const override {
        if (spaces_.count(name) == 0) {
            errorOut = "local space: unknown space '" + name + "'";
            return false;
        }
        // Climb from `name` to the root, applying each transform in order
        // (the DEEPEST space first): p_parent = R(yaw) * p + position.
        // chain is [name, parent, ..., root-child]; iterating it forward
        // applies the named space's transform first, then its parent, etc.
        glm::dvec3 p = local;
        std::string current = name;
        std::vector<const Space*> chain;
        while (!current.empty()) {
            const auto it = spaces_.find(current);
            if (it == spaces_.end()) {
                errorOut = "local space: broken parent chain at '" + current +
                           "'";
                return false;
            }
            chain.push_back(&it->second);
            current = it->second.parent;
        }
        for (const Space* space : chain) {
            p = rotate_y(p, space->transform.yawDegrees) +
                space->transform.position;
        }
        worldOut = p;
        errorOut.clear();
        return true;
    }

    bool world_to_space(const std::string& name, const glm::dvec3& world,
                        glm::dvec3& localOut,
                        std::string& errorOut) const override {
        if (spaces_.count(name) == 0) {
            errorOut = "local space: unknown space '" + name + "'";
            return false;
        }
        // Descend from the root to `name` (the SHALLOWEST space first):
        // p_local = R(-yaw) * (p - position). chain is [name, parent, ...,
        // root-child]; iterating it in reverse applies the root-child's
        // inverse transform first, down to the named space.
        std::vector<const Space*> chain;
        std::string current = name;
        while (!current.empty()) {
            const auto it = spaces_.find(current);
            if (it == spaces_.end()) {
                errorOut = "local space: broken parent chain at '" + current +
                           "'";
                return false;
            }
            chain.push_back(&it->second);
            current = it->second.parent;
        }
        glm::dvec3 p = world;
        for (auto rit = chain.rbegin(); rit != chain.rend(); ++rit) {
            p = rotate_y(p - (*rit)->transform.position,
                         -(*rit)->transform.yawDegrees);
        }
        localOut = p;
        errorOut.clear();
        return true;
    }

    // ---- Binding ------------------------------------------------------------
    bool bind_entity(const std::string& worldName,
                     engine::entity::EntityId handle,
                     const std::string& spaceName, const glm::vec3& localPos,
                     std::string& errorOut) override {
        engine::voxel::IVoxelWorld* world = manager_.world(worldName);
        if (world == nullptr) {
            errorOut = "local space: unknown world '" + worldName + "'";
            return false;
        }
        auto* entities = world->entity_world().get();
        if (entities == nullptr || !entities->alive(handle)) {
            errorOut = "local space: entity is not alive";
            return false;
        }
        if (spaces_.count(spaceName) == 0) {
            errorOut = "local space: unknown space '" + spaceName + "'";
            return false;
        }
        if (!entity_space(worldName, handle).empty()) {
            errorOut = "local space: entity is already bound";
            return false;
        }
        engine::entity::Position position{ localPos.x, localPos.y, localPos.z };
        if (!entities->set_position(handle, position)) {
            errorOut = "local space: cannot store the local position";
            return false;
        }
        engine::entity::ComponentData component;
        component.type = kSpaceRefComponent;
        component.version = 1;
        component.blob = "{\"space\":\"" + spaceName + "\"}";
        if (!entities->set_component(handle, component)) {
            errorOut = "local space: cannot store the space binding";
            return false;
        }
        errorOut.clear();
        return true;
    }

    std::string entity_space(const std::string& worldName,
                             engine::entity::EntityId handle) const override {
        engine::voxel::IVoxelWorld* world = manager_.world(worldName);
        if (world == nullptr) return {};
        auto* entities = world->entity_world().get();
        if (entities == nullptr || !entities->alive(handle)) return {};
        engine::entity::ComponentData component;
        if (!entities->get_component(handle, kSpaceRefComponent, component))
            return {};
        // The blob is project data; a malformed blob simply yields no binding.
        if (component.blob.size() < 10) return {};
        // JSON {"space":"name"} — extract the name between the quotes after
        // "space":" (engine's own writer; no JSON dependency needed here).
        const std::string needle = "\"space\":\"";
        const std::size_t at = component.blob.find(needle);
        if (at == std::string::npos) return {};
        const std::size_t valueStart = at + needle.size();
        const std::size_t valueEnd = component.blob.find('"', valueStart);
        if (valueEnd == std::string::npos) return {};
        return component.blob.substr(valueStart, valueEnd - valueStart);
    }

    bool unbind_entity(const std::string& worldName,
                       engine::entity::EntityId handle) override {
        engine::voxel::IVoxelWorld* world = manager_.world(worldName);
        if (world == nullptr) return false;
        auto* entities = world->entity_world().get();
        if (entities == nullptr || !entities->alive(handle)) return false;
        const std::string space = entity_space(worldName, handle);
        if (space.empty()) return true;  // missing binding = no-op success
        // Convert to the world frame so the entity does not teleport.
        glm::dvec3 worldPos;
        std::string error;
        if (!entity_world_position(worldName, handle, worldPos, error)) {
            return false;
        }
        engine::entity::Position position{
            static_cast<float>(worldPos.x), static_cast<float>(worldPos.y),
            static_cast<float>(worldPos.z)
        };
        if (!entities->set_position(handle, position)) return false;
        return entities->remove_component(handle, kSpaceRefComponent);
    }

    bool entity_world_position(const std::string& worldName,
                               engine::entity::EntityId handle,
                               glm::dvec3& out,
                               std::string& errorOut) const override {
        engine::voxel::IVoxelWorld* world = manager_.world(worldName);
        if (world == nullptr) {
            errorOut = "local space: unknown world '" + worldName + "'";
            return false;
        }
        auto* entities = world->entity_world().get();
        if (entities == nullptr || !entities->alive(handle)) {
            errorOut = "local space: entity is not alive";
            return false;
        }
        engine::entity::Position stored;
        if (!entities->get_position(handle, stored)) {
            errorOut = "local space: entity position unreadable";
            return false;
        }
        const std::string space = entity_space(worldName, handle);
        if (space.empty()) {
            out = glm::dvec3(stored.x, stored.y, stored.z);
        } else {
            const glm::dvec3 local(stored.x, stored.y, stored.z);
            if (!space_to_world(space, local, out, errorOut)) return false;
        }
        errorOut.clear();
        return true;
    }

private:
    static glm::dvec3 rotate_y(const glm::dvec3& v, float yawDegrees) {
        const double radians =
            static_cast<double>(yawDegrees) * 3.14159265358979323846 / 180.0;
        const double c = snap_axis(std::cos(radians));
        const double s = snap_axis(std::sin(radians));
        return glm::dvec3(v.x * c + v.z * s, v.y, -v.x * s + v.z * c);
    }

    bool has_bound_entities(const std::string& spaceName) {
        for (const std::string& name : manager_.world_names()) {
            engine::voxel::IVoxelWorld* world = manager_.world(name);
            if (world == nullptr) continue;
            auto* entities = world->entity_world().get();
            if (entities == nullptr) continue;
            bool found = false;
            entities->for_each_entity([&](engine::entity::EntityId handle) {
                if (found) return;
                if (entity_space(name, handle) == spaceName) found = true;
            });
            if (found) return true;
        }
        return false;
    }

    IWorldManager& manager_;
    std::map<std::string, Space> spaces_;
};

std::unique_ptr<ILocalSpace> create_local_space(IWorldManager& manager) {
    return std::make_unique<LocalSpace>(manager);
}

}  // namespace world
}  // namespace engine
