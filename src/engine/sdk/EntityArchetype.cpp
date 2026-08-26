// EntityArchetype.cpp — the only TU implementing the public entity
// archetype registry (Agente 4 §1 item 13 CORE): data-driven entity
// templates by kind (player/mob/vehicle/projectile/interactive) with
// components (type + opaque JSON). Pure std + RegistryJson.

#include "engine/entity/IEntityArchetype.hpp"

#include "RegistryJson.hpp"

#include <algorithm>
#include <map>
#include <sstream>
#include <vector>

namespace engine {
namespace entity {

const char* entity_kind_name(EntityKind kind) {
    switch (kind) {
        case EntityKind::Player: return "player";
        case EntityKind::Mob: return "mob";
        case EntityKind::Vehicle: return "vehicle";
        case EntityKind::Projectile: return "projectile";
        case EntityKind::Interactive: return "interactive";
    }
    return "mob";
}

namespace {

bool parse_kind(const std::string& text, EntityKind& out) {
    if (text == "player") out = EntityKind::Player;
    else if (text == "mob") out = EntityKind::Mob;
    else if (text == "vehicle") out = EntityKind::Vehicle;
    else if (text == "projectile") out = EntityKind::Projectile;
    else if (text == "interactive") out = EntityKind::Interactive;
    else return false;
    return true;
}

bool check_archetype(const EntityArchetype& archetype, std::string& errorOut) {
    if (archetype.name.empty()) {
        errorOut = "entity archetype: name must be non-empty";
        return false;
    }
    for (const ArchetypeComponent& component : archetype.components) {
        if (component.type.empty()) {
            errorOut = "entity archetype '" + archetype.name +
                       "': component type must be non-empty";
            return false;
        }
        if (!component.json.empty()) {
            sdk::JsonValue parsed;
            std::string jsonError;
            if (!sdk::json_parse(component.json, parsed, jsonError)) {
                errorOut = "entity archetype '" + archetype.name +
                           "': component '" + component.type +
                           "' has malformed json";
                return false;
            }
        }
    }
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

class EntityArchetypeRegistry final : public IEntityArchetypeRegistry {
public:
    EntityArchetypeRegistry() = default;

    bool register_archetype(const EntityArchetype& archetype,
                            std::string& errorOut) override {
        if (!check_archetype(archetype, errorOut)) return false;
        if (archetypes_.count(archetype.name) != 0) {
            errorOut = "entity archetype: duplicate name '" + archetype.name + "'";
            return false;
        }
        archetypes_[archetype.name] = archetype;
        return true;
    }

    bool load_from_json(const std::string& jsonText,
                        std::string& errorOut) override {
        sdk::JsonValue root;
        if (!sdk::json_parse(jsonText, root, errorOut) || !root.is_object()) {
            if (errorOut.empty()) errorOut = "entity archetype: root must be an object";
            return false;
        }
        const int version = static_cast<int>(sdk::json_number(root, "version", 1));
        if (version != 1) {
            errorOut = "entity archetype: unsupported version " + std::to_string(version);
            return false;
        }
        const sdk::JsonValue* listValue = root.field("archetypes");
        if (listValue == nullptr || listValue->kind != sdk::JsonValue::Kind::Array) {
            errorOut = "entity archetype: archetypes must be an array";
            return false;
        }
        std::map<std::string, EntityArchetype> parsed;
        for (const sdk::JsonValue& entry : listValue->array) {
            if (!entry.is_object()) {
                errorOut = "entity archetype: each archetype must be an object";
                return false;
            }
            EntityArchetype archetype;
            archetype.name = sdk::json_string(entry, "name", "");
            const std::string kindName = sdk::json_string(entry, "kind", "mob");
            if (!parse_kind(kindName, archetype.kind)) {
                errorOut = "entity archetype: unknown kind '" + kindName + "'";
                return false;
            }
            const sdk::JsonValue* componentsValue = entry.field("components");
            if (componentsValue != nullptr) {
                if (componentsValue->kind != sdk::JsonValue::Kind::Array) {
                    errorOut = "entity archetype: components must be an array";
                    return false;
                }
                for (const sdk::JsonValue& comp : componentsValue->array) {
                    if (!comp.is_object()) {
                        errorOut = "entity archetype: each component must be an object";
                        return false;
                    }
                    ArchetypeComponent component;
                    component.type = sdk::json_string(comp, "type", "");
                    component.json = sdk::json_string(comp, "json", "");
                    archetype.components.push_back(std::move(component));
                }
            }
            if (!check_archetype(archetype, errorOut)) return false;
            if (parsed.count(archetype.name) != 0) {
                errorOut = "entity archetype: duplicate name '" + archetype.name + "'";
                return false;
            }
            parsed[archetype.name] = std::move(archetype);
        }
        archetypes_ = std::move(parsed);
        return true;
    }

    std::string to_json() const override {
        std::ostringstream out;
        out << "{\"version\":1,\"archetypes\":[";
        bool first = true;
        for (const auto& entry : archetypes_) {  // map: ordem crescente
            if (!first) out << ",";
            first = false;
            const EntityArchetype& archetype = entry.second;
            out << "{\"name\":\"" << json_escape(archetype.name) << "\",\"kind\":\""
                << entity_kind_name(archetype.kind) << "\",\"components\":[";
            for (std::size_t i = 0; i < archetype.components.size(); ++i) {
                if (i != 0) out << ",";
                out << "{\"type\":\"" << json_escape(archetype.components[i].type)
                    << "\",\"json\":\"" << json_escape(archetype.components[i].json)
                    << "\"}";
            }
            out << "]}";
        }
        out << "]}";
        return out.str();
    }

    const EntityArchetype* find(const std::string& name) const override {
        const auto found = archetypes_.find(name);
        return found == archetypes_.end() ? nullptr : &found->second;
    }

    std::vector<std::string> names() const override {
        std::vector<std::string> out;
        out.reserve(archetypes_.size());
        for (const auto& entry : archetypes_) out.push_back(entry.first);
        return out;
    }

    std::size_t count() const override { return archetypes_.size(); }
    void clear() override { archetypes_.clear(); }

private:
    std::map<std::string, EntityArchetype> archetypes_;
};

}  // namespace

std::unique_ptr<IEntityArchetypeRegistry> create_entity_archetype_registry() {
    return std::make_unique<EntityArchetypeRegistry>();
}

}  // namespace entity
}  // namespace engine
