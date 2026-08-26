// EntityLifecycle.cpp — the only TU implementing the public entity lifecycle
// contract (Agente 4 §1 item 14 CORE): Despawned/Active/Sleeping state
// machine with pool capacity, persistence flags and dirty tracking.
// Pure std + RegistryJson; deterministic order everywhere.

#include "engine/entity/IEntityLifecycle.hpp"

#include "RegistryJson.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <sstream>
#include <vector>

namespace engine {
namespace entity {

const char* lifecycle_state_name(LifecycleState state) {
    switch (state) {
        case LifecycleState::Despawned: return "despawned";
        case LifecycleState::Active: return "active";
        case LifecycleState::Sleeping: return "sleeping";
    }
    return "despawned";
}

namespace {

bool parse_state(const std::string& text, LifecycleState& out) {
    if (text == "despawned") out = LifecycleState::Despawned;
    else if (text == "active") out = LifecycleState::Active;
    else if (text == "sleeping") out = LifecycleState::Sleeping;
    else return false;
    return true;
}

std::string json_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char ch : text) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += ch; break;
        }
    }
    return out;
}

struct Entry {
    LifecycleState state{ LifecycleState::Despawned };
    bool persistent{ false };
    bool dirty{ true };  // registro conta como mudança
};

class EntityLifecycle final : public IEntityLifecycle {
public:
    EntityLifecycle() = default;

    bool configure(std::size_t poolCapacity, std::string& errorOut) override {
        if (poolCapacity == 0) {
            errorOut = "entity lifecycle: poolCapacity must be >= 1";
            return false;
        }
        poolCapacity_ = poolCapacity;
        entries_.clear();
        return true;
    }

    bool register_entity(std::uint64_t entityId, std::string& errorOut) override {
        if (entityId == 0) {
            errorOut = "entity lifecycle: entityId must be non-zero";
            return false;
        }
        if (entries_.count(entityId) != 0) {
            errorOut = "entity lifecycle: duplicate entity id";
            return false;
        }
        entries_[entityId] = Entry{};
        return true;
    }

    bool spawn(std::uint64_t entityId) override {
        const auto found = entries_.find(entityId);
        if (found == entries_.end()) return false;
        if (found->second.state != LifecycleState::Despawned) return false;
        if (pool_used() >= poolCapacity_) return false;
        found->second.state = LifecycleState::Active;
        found->second.dirty = true;
        return true;
    }

    bool sleep(std::uint64_t entityId) override {
        const auto found = entries_.find(entityId);
        if (found == entries_.end()) return false;
        if (found->second.state != LifecycleState::Active) return false;
        found->second.state = LifecycleState::Sleeping;
        found->second.dirty = true;
        return true;
    }

    bool wake(std::uint64_t entityId) override {
        const auto found = entries_.find(entityId);
        if (found == entries_.end()) return false;
        if (found->second.state != LifecycleState::Sleeping) return false;
        found->second.state = LifecycleState::Active;
        found->second.dirty = true;
        return true;
    }

    bool despawn(std::uint64_t entityId) override {
        const auto found = entries_.find(entityId);
        if (found == entries_.end()) return false;
        if (found->second.state == LifecycleState::Despawned) return false;
        found->second.state = LifecycleState::Despawned;
        found->second.dirty = true;
        return true;
    }

    LifecycleState state(std::uint64_t entityId) const override {
        const auto found = entries_.find(entityId);
        return found == entries_.end() ? LifecycleState::Despawned : found->second.state;
    }

    bool is_registered(std::uint64_t entityId) const override {
        return entries_.count(entityId) != 0;
    }

    std::vector<std::uint64_t> by_state(LifecycleState state) const override {
        std::vector<std::uint64_t> out;
        for (const auto& entry : entries_) {  // map: ordem crescente de id
            if (entry.second.state == state) out.push_back(entry.first);
        }
        return out;
    }

    std::size_t pool_used() const override {
        std::size_t used = 0;
        for (const auto& entry : entries_) {
            if (entry.second.state != LifecycleState::Despawned) ++used;
        }
        return used;
    }

    std::size_t pool_capacity() const override { return poolCapacity_; }

    bool set_persistent(std::uint64_t entityId, bool persistent) override {
        const auto found = entries_.find(entityId);
        if (found == entries_.end()) return false;
        if (found->second.persistent != persistent) {
            found->second.persistent = persistent;
            found->second.dirty = true;
        }
        return true;
    }

    bool is_persistent(std::uint64_t entityId) const override {
        const auto found = entries_.find(entityId);
        return found != entries_.end() && found->second.persistent;
    }

    std::vector<std::uint64_t> dirty() const override {
        std::vector<std::uint64_t> out;
        for (const auto& entry : entries_) {
            if (entry.second.dirty) out.push_back(entry.first);
        }
        return out;
    }

    void mark_checkpoint() override {
        for (auto& entry : entries_) entry.second.dirty = false;
    }

    std::string to_json() const override {
        std::ostringstream out;
        out << "{\"version\":1,\"poolCapacity\":" << poolCapacity_
            << ",\"entities\":[";
        bool first = true;
        for (const auto& entry : entries_) {
            if (!first) out << ",";
            first = false;
            out << "{\"id\":" << entry.first << ",\"state\":\""
                << lifecycle_state_name(entry.second.state)
                << "\",\"persistent\":"
                << (entry.second.persistent ? "true" : "false") << "}";
        }
        out << "]}";
        return out.str();
    }

    bool load_from_json(const std::string& jsonText,
                        std::string& errorOut) override {
        sdk::JsonValue root;
        if (!sdk::json_parse(jsonText, root, errorOut) || !root.is_object()) {
            if (errorOut.empty()) errorOut = "entity lifecycle: root must be an object";
            return false;
        }
        const int version = static_cast<int>(sdk::json_number(root, "version", 1));
        if (version != 1) {
            errorOut = "entity lifecycle: unsupported version " + std::to_string(version);
            return false;
        }
        const std::size_t capacity = static_cast<std::size_t>(
            sdk::json_number(root, "poolCapacity", 0));
        if (capacity == 0) {
            errorOut = "entity lifecycle: poolCapacity must be >= 1";
            return false;
        }
        const sdk::JsonValue* listValue = root.field("entities");
        if (listValue == nullptr || listValue->kind != sdk::JsonValue::Kind::Array) {
            errorOut = "entity lifecycle: entities must be an array";
            return false;
        }
        std::map<std::uint64_t, Entry> parsed;
        for (const sdk::JsonValue& entryValue : listValue->array) {
            if (!entryValue.is_object()) {
                errorOut = "entity lifecycle: each entity must be an object";
                return false;
            }
            const std::uint64_t id = static_cast<std::uint64_t>(
                sdk::json_number(entryValue, "id", 0));
            if (id == 0 || parsed.count(id) != 0) {
                errorOut = "entity lifecycle: duplicate/zero entity id";
                return false;
            }
            Entry entry;
            const std::string stateName = sdk::json_string(entryValue, "state", "");
            if (!parse_state(stateName, entry.state)) {
                errorOut = "entity lifecycle: unknown state '" + stateName + "'";
                return false;
            }
            entry.persistent = sdk::json_bool(entryValue, "persistent", false);
            entry.dirty = false;  // carga não marca dirty
            parsed[id] = entry;
        }
        // Alvos com pool cheio no load: recusa (all-or-nothing).
        std::size_t alive = 0;
        for (const auto& entry : parsed) {
            if (entry.second.state != LifecycleState::Despawned) ++alive;
        }
        if (alive > capacity) {
            errorOut = "entity lifecycle: loaded state exceeds pool capacity";
            return false;
        }
        poolCapacity_ = capacity;
        entries_ = std::move(parsed);
        return true;
    }

private:
    std::size_t poolCapacity_{ 64 };
    std::map<std::uint64_t, Entry> entries_;
};

}  // namespace

std::unique_ptr<IEntityLifecycle> create_entity_lifecycle() {
    return std::make_unique<EntityLifecycle>();
}

}  // namespace entity
}  // namespace engine
