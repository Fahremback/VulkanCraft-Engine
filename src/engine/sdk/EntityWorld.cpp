// EntityWorld.cpp — EnTT-backed entity world (META section 15).
//
// This is the ONLY translation unit that includes the EnTT headers; the public
// contract lives in engine/entity/IEntityWorld.hpp and never leaks EnTT.
// Entities carry generational handles (a despawn bumps the generation, so a
// stale handle never aliases a reused id), are spatially indexed by voxel
// chunk (16x16 columns), sleep per entity via tick policies and persist as
// versioned snapshots through the world save.

#include "engine/entity/IEntityWorld.hpp"

#include <entt/entt.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine {
namespace entity {

namespace {

constexpr int kChunkSize = 16;  // matches the voxel chunk grid (CHUNK_SIZE_X/Z)

struct TypeTag {
    std::string id;
};

struct TickAccumulator {
    float elapsed{ 0.0f };
};

struct ProjectComponents {
    std::map<std::string, ComponentData> byType;  // map: deterministic order
};

// FNV-1a 64-bit chunk key from (cx, cz) — stable and cheap.
uint64_t chunk_key(int cx, int cz) {
    uint64_t hash = 1469598103934665603ULL;
    const uint64_t values[2] = {
        static_cast<uint64_t>(static_cast<int64_t>(cx)),
        static_cast<uint64_t>(static_cast<int64_t>(cz))};
    for (const uint64_t v : values) {
        for (int shift = 0; shift < 64; shift += 8) {
            hash ^= (v >> static_cast<unsigned>(shift)) & 0xFFu;
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

}  // namespace

class EntityWorldImpl final : public IEntityWorld {
public:
    // ---- Lifecycle ---------------------------------------------------------

    EntityId spawn(const std::string& type, const Position& position,
                   std::string& errorOut) override {
        if (type.empty()) {
            errorOut = "entity: type cannot be empty";
            return {};
        }
        uint32_t id = 0;
        uint32_t generation = 0;
        if (!freeList_.empty()) {
            id = freeList_.back();
            freeList_.pop_back();
            generation = generations_.at(id);
        } else {
            id = nextId_++;
            generations_[id] = 0;
        }
        const entt::entity raw = registry_.create();
        registry_.emplace<TypeTag>(raw, TypeTag{ type });
        registry_.emplace<Position>(raw, position);
        registry_.emplace<Health>(raw);
        registry_.emplace<TickAccumulator>(raw);
        registry_.emplace<ProjectComponents>(raw);
        byId_[id] = raw;
        rebuild_index();
        errorOut.clear();
        return EntityId{ id, generation };
    }

    bool remove_component(EntityId handle, const std::string& type) override {
        if (!alive(handle)) return false;
        if (type.empty()) return true;  // nothing to remove
        registry_.get<ProjectComponents>(byId_.at(handle.id)).byType.erase(type);
        return true;
    }

    bool despawn(EntityId handle) override {
        if (!alive(handle)) return false;
        registry_.destroy(byId_.at(handle.id));
        byId_.erase(handle.id);
        ++generations_.at(handle.id);
        freeList_.push_back(handle.id);
        // Drop the stable id (it identified THIS entity; a stale reference
        // must not alias the entity that reuses the id).
        const auto stable = stableByEntity_.find(handle.id);
        if (stable != stableByEntity_.end()) {
            stableByStable_.erase(stable->second);
            stableByEntity_.erase(stable);
        }
        rebuild_index();
        return true;
    }

    bool alive(EntityId handle) const override {
        if (!handle.valid()) return false;
        const auto found = byId_.find(handle.id);
        if (found == byId_.end()) return false;
        return registry_.valid(found->second) &&
               generations_.at(handle.id) == handle.generation;
    }

    // ---- Builtin components ------------------------------------------------

    // ---- Stable ids --------------------------------------------------------

    bool set_stable_id(EntityId handle, const std::string& stableId) override {
        if (!alive(handle)) return false;
        // Remove the current mapping first (keeps the maps consistent even
        // when the entity is re-tagged).
        const auto current = stableByEntity_.find(handle.id);
        if (current != stableByEntity_.end()) {
            if (current->second == stableId) return true;  // no-op
            stableByStable_.erase(current->second);
            stableByEntity_.erase(current);
        }
        if (stableId.empty()) return true;  // cleared
        if (stableByStable_.count(stableId) != 0) return false;  // duplicate
        stableByEntity_[handle.id] = stableId;
        stableByStable_[stableId] = handle.id;
        return true;
    }

    std::string stable_id(EntityId handle) const override {
        if (!alive(handle)) return {};
        const auto found = stableByEntity_.find(handle.id);
        return found == stableByEntity_.end() ? std::string{} : found->second;
    }

    EntityId entity_by_stable_id(const std::string& stableId) const override {
        if (stableId.empty()) return {};
        const auto found = stableByStable_.find(stableId);
        if (found == stableByStable_.end()) return {};
        const EntityId handle{ found->second, generations_.at(found->second) };
        return alive(handle) ? handle : EntityId{};
    }

    bool set_health(EntityId handle, const Health& health) override {
        if (!alive(handle)) return false;
        registry_.get<Health>(byId_.at(handle.id)) = health;
        return true;
    }

    bool get_health(EntityId handle, Health& out) const override {
        if (!alive(handle)) return false;
        out = registry_.get<Health>(byId_.at(handle.id));
        return true;
    }

    bool set_position(EntityId handle, const Position& position) override {
        if (!alive(handle)) return false;
        registry_.get<Position>(byId_.at(handle.id)) = position;
        chunkDirty_ = true;  // reindex lazily on the next spatial query
        return true;
    }

    bool get_position(EntityId handle, Position& out) const override {
        if (!alive(handle)) return false;
        out = registry_.get<Position>(byId_.at(handle.id));
        return true;
    }

    std::string type_of(EntityId handle) const override {
        if (!alive(handle)) return {};
        return registry_.get<TypeTag>(byId_.at(handle.id)).id;
    }

    bool set_tick_interval(EntityId handle, float interval) override {
        if (!alive(handle) || interval < 0.0f) return false;
        registry_.get<TickAccumulator>(byId_.at(handle.id)).elapsed = 0.0f;
        tickInterval_[handle.id] = interval;
        return true;
    }

    bool get_tick_interval(EntityId handle, float& out) const override {
        if (!alive(handle)) return false;
        out = tickInterval(handle.id);
        return true;
    }

    // ---- Project components -------------------------------------------------

    bool set_component(EntityId handle, const ComponentData& component) override {
        if (!alive(handle)) return false;
        if (component.type.empty()) return false;
        registry_.get<ProjectComponents>(byId_.at(handle.id))
            .byType[component.type] = component;
        return true;
    }

    bool get_component(EntityId handle, const std::string& type,
                       ComponentData& out) const override {
        if (!alive(handle)) return false;
        const auto& byType =
            registry_.get<ProjectComponents>(byId_.at(handle.id)).byType;
        const auto found = byType.find(type);
        if (found == byType.end()) return false;
        out = found->second;
        return true;
    }

    void for_each_component(
        EntityId handle,
        const std::function<void(const ComponentData&)>& visit) const override {
        if (!alive(handle)) return;
        // Deterministic order: sorted by component type (the underlying map
        // order is not stable).
        const auto& byType =
            registry_.get<ProjectComponents>(byId_.at(handle.id)).byType;
        std::vector<std::string> types;
        types.reserve(byType.size());
        for (const auto& [type, component] : byType) {
            (void)component;
            types.push_back(type);
        }
        std::sort(types.begin(), types.end());
        for (const std::string& type : types) {
            visit(byType.at(type));
        }
    }

    // ---- Headless simulation -----------------------------------------------

    void tick(float dt, const std::function<void(EntityId, float)>& onTick) override {
        if (dt <= 0.0f) return;
        // Deterministic order: ids in ascending order (the registry storage
        // order is not stable across destroy/reuse).
        std::vector<EntityId> order;
        order.reserve(byId_.size());
        for (const auto& [id, raw] : byId_) {
            if (registry_.valid(raw)) {
                order.push_back(EntityId{ id, generations_.at(id) });
            }
        }
        std::sort(order.begin(), order.end(),
                  [](const EntityId& a, const EntityId& b) { return a.id < b.id; });
        for (const EntityId& handle : order) {
            if (!alive(handle)) continue;
            TickAccumulator& acc = registry_.get<TickAccumulator>(byId_.at(handle.id));
            const float interval = tickInterval(handle.id);
            if (interval <= 0.0f) {
                if (onTick) onTick(handle, dt);
                continue;
            }
            acc.elapsed += dt;
            if (acc.elapsed >= interval) {
                if (onTick) onTick(handle, acc.elapsed);
                acc.elapsed = 0.0f;
            }
        }
    }

    std::size_t size() const override {
        std::size_t count = 0;
        for (const auto& [id, raw] : byId_) {
            if (registry_.valid(raw)) ++count;
        }
        return count;
    }

    std::size_t sleeping_count() const override {
        std::size_t count = 0;
        for (const auto& [id, raw] : byId_) {
            if (!registry_.valid(raw)) continue;
            const float interval = tickInterval(id);
            if (interval <= 0.0f) continue;
            if (registry_.get<TickAccumulator>(raw).elapsed < interval) ++count;
        }
        return count;
    }

    // ---- Spatial queries ----------------------------------------------------

    std::vector<EntityId> entities_in_chunk(int cx, int cz) const override {
        if (chunkDirty_) const_cast<EntityWorldImpl*>(this)->rebuild_index();
        const auto found = chunkIndex_.find(chunk_key(cx, cz));
        if (found == chunkIndex_.end()) return {};
        std::vector<EntityId> out;
        for (const EntityId& handle : found->second) {
            if (alive(handle)) out.push_back(handle);
        }
        return out;
    }

    std::vector<EntityId> entities_in_aabb(float minX, float minY, float minZ,
                                           float maxX, float maxY,
                                           float maxZ) const override {
        const int minCx = static_cast<int>(std::floor(minX / kChunkSize));
        const int maxCx = static_cast<int>(std::floor(maxX / kChunkSize));
        const int minCz = static_cast<int>(std::floor(minZ / kChunkSize));
        const int maxCz = static_cast<int>(std::floor(maxZ / kChunkSize));
        std::vector<EntityId> out;
        for (int cx = minCx; cx <= maxCx; ++cx) {
            for (int cz = minCz; cz <= maxCz; ++cz) {
                for (const EntityId& handle : entities_in_chunk(cx, cz)) {
                    Position pos;
                    if (!get_position(handle, pos)) continue;
                    if (pos.x < minX || pos.x > maxX || pos.y < minY ||
                        pos.y > maxY || pos.z < minZ || pos.z > maxZ) {
                        continue;
                    }
                    out.push_back(handle);
                }
            }
        }
        return out;
    }

    void for_each_entity(
        const std::function<void(EntityId)>& visit) const override {
        if (!visit) return;
        std::vector<EntityId> order;
        order.reserve(byId_.size());
        for (const auto& [id, raw] : byId_) {
            if (registry_.valid(raw)) {
                order.push_back(EntityId{ id, generations_.at(id) });
            }
        }
        std::sort(order.begin(), order.end(),
                  [](const EntityId& a, const EntityId& b) { return a.id < b.id; });
        for (const EntityId& handle : order) {
            if (!alive(handle)) continue;
            visit(handle);
        }
    }

    // ---- Persistence ---------------------------------------------------------

    std::vector<EntitySnapshot> serialize_entities() const override {
        std::vector<EntityId> order;
        order.reserve(byId_.size());
        for (const auto& [id, raw] : byId_) {
            if (registry_.valid(raw)) {
                order.push_back(EntityId{ id, generations_.at(id) });
            }
        }
        std::sort(order.begin(), order.end(),
                  [](const EntityId& a, const EntityId& b) { return a.id < b.id; });
        std::vector<EntitySnapshot> snapshots;
        snapshots.reserve(order.size());
        for (const EntityId& handle : order) {
            if (!alive(handle)) continue;
            const entt::entity raw = byId_.at(handle.id);
            EntitySnapshot snapshot;
            snapshot.type = registry_.get<TypeTag>(raw).id;
            snapshot.position = registry_.get<Position>(raw);
            snapshot.health = registry_.get<Health>(raw);
            snapshot.tickInterval = tickInterval(handle.id);
            snapshot.stableId = stable_id(handle);
            for (const auto& [type, component] :
                 registry_.get<ProjectComponents>(raw).byType) {
                (void)type;
                snapshot.components.push_back(component);
            }
            snapshots.push_back(std::move(snapshot));
        }
        return snapshots;
    }

    bool deserialize_entities(const std::vector<EntitySnapshot>& entities,
                              std::string& errorOut) override {
        // Validate everything before touching the population (all-or-nothing).
        for (const EntitySnapshot& snapshot : entities) {
            if (snapshot.type.empty()) {
                errorOut = "entity snapshot with empty type";
                return false;
            }
            if (snapshot.tickInterval < 0.0f) {
                errorOut = "entity '" + snapshot.type + "': negative tick interval";
                return false;
            }
            for (const ComponentData& component : snapshot.components) {
                if (component.type.empty()) {
                    errorOut = "entity '" + snapshot.type +
                               "': component with empty type";
                    return false;
                }
            }
        }
        // Clear current population.
        std::vector<EntityId> current;
        for (const auto& [id, raw] : byId_) {
            if (registry_.valid(raw)) {
                current.push_back(EntityId{ id, generations_.at(id) });
            }
        }
        for (const EntityId& handle : current) despawn(handle);
        // Repopulate in snapshot order. Validate stable-id uniqueness BEFORE
        // mutating (all-or-nothing, same as the rest of the snapshot checks).
        {
            std::unordered_map<std::string, int> seen;
            for (const EntitySnapshot& snapshot : entities) {
                if (snapshot.stableId.empty()) continue;
                if (++seen[snapshot.stableId] > 1) {
                    errorOut = "entity restore: duplicate stable id '" +
                               snapshot.stableId + "'";
                    return false;
                }
            }
        }
        for (const EntitySnapshot& snapshot : entities) {
            std::string spawnError;
            const EntityId handle =
                spawn(snapshot.type, snapshot.position, spawnError);
            if (!handle.valid()) {
                errorOut = "entity restore failed: " + spawnError;
                return false;
            }
            set_health(handle, snapshot.health);
            if (snapshot.tickInterval > 0.0f) {
                set_tick_interval(handle, snapshot.tickInterval);
            }
            if (!snapshot.stableId.empty()) {
                if (!set_stable_id(handle, snapshot.stableId)) {
                    errorOut = "entity restore: stable id '" +
                               snapshot.stableId + "' not accepted";
                    return false;
                }
            }
            for (const ComponentData& component : snapshot.components) {
                set_component(handle, component);
            }
        }
        errorOut.clear();
        return true;
    }

private:
    float tickInterval(uint32_t id) const {
        const auto found = tickInterval_.find(id);
        return found == tickInterval_.end() ? 0.0f : found->second;
    }

    void rebuild_index() {
        chunkIndex_.clear();
        for (const auto& [id, raw] : byId_) {
            if (!registry_.valid(raw)) continue;
            const Position pos = registry_.get<Position>(raw);
            const int cx = static_cast<int>(std::floor(pos.x / kChunkSize));
            const int cz = static_cast<int>(std::floor(pos.z / kChunkSize));
            chunkIndex_[chunk_key(cx, cz)].push_back(
                EntityId{ id, generations_.at(id) });
        }
        chunkDirty_ = false;
    }

    entt::registry registry_;
    std::unordered_map<uint32_t, entt::entity> byId_;
    std::unordered_map<uint32_t, uint32_t> generations_;
    std::unordered_map<uint32_t, float> tickInterval_;
    std::unordered_map<uint64_t, std::vector<EntityId>> chunkIndex_;
    // Stable ids (META §19): entity id -> stable id, plus the reverse index.
    std::unordered_map<uint32_t, std::string> stableByEntity_;
    std::unordered_map<std::string, uint32_t> stableByStable_;
    bool chunkDirty_{ false };
    uint32_t nextId_{ 1 };
    std::vector<uint32_t> freeList_;
};

std::unique_ptr<IEntityWorld> create_entity_world() {
    return std::make_unique<EntityWorldImpl>();
}

}  // namespace entity
}  // namespace engine
