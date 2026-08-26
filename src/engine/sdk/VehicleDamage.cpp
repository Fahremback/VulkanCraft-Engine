// VehicleDamage.cpp — the only TU implementing the public vehicle damage
// contract (Agente 4 §6 item 57 CORE): deterministic per-part health with
// destruction and detachment thresholds. Pure std + RegistryJson.

#include "engine/vehicles/IVehicleDamage.hpp"

#include "RegistryJson.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <map>
#include <sstream>
#include <vector>

namespace engine {
namespace vehicles {
namespace {

bool finite_float(float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float is 32-bit");
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000u) != 0x7f800000u;
}

bool check_parts(const std::vector<VehiclePartSpec>& parts, std::string& errorOut) {
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const VehiclePartSpec& part = parts[i];
        if (part.name.empty()) {
            errorOut = "vehicle damage: part at index " + std::to_string(i) +
                       " has an empty name";
            return false;
        }
        if (!finite_float(part.maxHealth) || part.maxHealth <= 0.0f) {
            errorOut = "vehicle damage: part '" + part.name +
                       "' needs maxHealth > 0 (finite)";
            return false;
        }
        if (!finite_float(part.detachThreshold) || part.detachThreshold < 0.0f ||
            part.detachThreshold > 1.0f) {
            errorOut = "vehicle damage: part '" + part.name +
                       "' detachThreshold must be in [0,1]";
            return false;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (parts[j].name == part.name) {
                errorOut = "vehicle damage: duplicate part name '" + part.name + "'";
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

std::string emit_float(float value) {
    std::ostringstream out;
    out << std::setprecision(9) << value;
    return out.str();
}

struct PartState {
    VehiclePartSpec spec;
    float health{ 0.0f };
    bool destroyed{ false };
    bool detached{ false };
};

class VehicleDamage final : public IVehicleDamage {
public:
    VehicleDamage() = default;

    bool configure(const std::vector<VehiclePartSpec>& parts,
                   std::string& errorOut) override {
        if (!check_parts(parts, errorOut)) return false;
        parts_.clear();
        for (const VehiclePartSpec& part : parts) {
            PartState state;
            state.spec = part;
            state.health = part.maxHealth;
            parts_[part.name] = state;
        }
        return true;
    }

    VehicleDamageResult apply_damage(const std::string& part,
                                     float amount) override {
        VehicleDamageResult result;
        const auto found = parts_.find(part);
        if (found == parts_.end()) return result;  // desconhecida
        PartState& state = found->second;
        if (state.destroyed || state.detached) return result;  // fora de combate

        float applied = amount;
        if (!finite_float(amount) || amount < 0.0f) applied = 0.0f;
        state.health -= applied;
        if (state.health < 0.0f) state.health = 0.0f;
        result.totalDamage = applied;

        if (state.health <= 0.0f) {
            state.destroyed = true;
            result.newlyDestroyed.push_back(part);
        } else if (state.spec.detachable &&
                   state.health <= state.spec.detachThreshold * state.spec.maxHealth) {
            state.detached = true;
            result.newlyDetached.push_back(part);
        }
        return result;
    }

    float health(const std::string& part) const override {
        const auto found = parts_.find(part);
        return found == parts_.end() ? -1.0f : found->second.health;
    }

    bool is_destroyed(const std::string& part) const override {
        const auto found = parts_.find(part);
        return found != parts_.end() && found->second.destroyed;
    }

    bool is_detached(const std::string& part) const override {
        const auto found = parts_.find(part);
        return found != parts_.end() && found->second.detached;
    }

    std::vector<std::string> destroyed_parts() const override {
        std::vector<std::string> out;
        for (const auto& entry : parts_) {  // map: ordem crescente
            if (entry.second.destroyed) out.push_back(entry.first);
        }
        return out;
    }

    std::vector<std::string> detached_parts() const override {
        std::vector<std::string> out;
        for (const auto& entry : parts_) {
            if (entry.second.detached) out.push_back(entry.first);
        }
        return out;
    }

    void repair_all() override {
        for (auto& entry : parts_) {
            entry.second.health = entry.second.spec.maxHealth;
            entry.second.destroyed = false;
            entry.second.detached = false;
        }
    }

    std::string to_json() const override {
        std::ostringstream out;
        out << "{\"version\":1,\"parts\":[";
        bool first = true;
        for (const auto& entry : parts_) {
            if (!first) out << ",";
            first = false;
            const PartState& state = entry.second;
            out << "{\"name\":\"" << json_escape(state.spec.name)
                << "\",\"maxHealth\":" << emit_float(state.spec.maxHealth)
                << ",\"detachable\":" << (state.spec.detachable ? "true" : "false")
                << ",\"detachThreshold\":" << emit_float(state.spec.detachThreshold)
                << ",\"health\":" << emit_float(state.health)
                << ",\"destroyed\":" << (state.destroyed ? "true" : "false")
                << ",\"detached\":" << (state.detached ? "true" : "false") << "}";
        }
        out << "]}";
        return out.str();
    }

    bool load_from_json(const std::string& jsonText,
                        std::string& errorOut) override {
        sdk::JsonValue root;
        if (!sdk::json_parse(jsonText, root, errorOut) || !root.is_object()) {
            if (errorOut.empty()) errorOut = "vehicle damage: root must be an object";
            return false;
        }
        const int version = static_cast<int>(sdk::json_number(root, "version", 1));
        if (version != 1) {
            errorOut = "vehicle damage: unsupported version " + std::to_string(version);
            return false;
        }
        const sdk::JsonValue* listValue = root.field("parts");
        if (listValue == nullptr || listValue->kind != sdk::JsonValue::Kind::Array) {
            errorOut = "vehicle damage: parts must be an array";
            return false;
        }
        std::map<std::string, PartState> parsed;
        for (const sdk::JsonValue& partValue : listValue->array) {
            if (!partValue.is_object()) {
                errorOut = "vehicle damage: each part must be an object";
                return false;
            }
            PartState state;
            state.spec.name = sdk::json_string(partValue, "name", "");
            state.spec.maxHealth = static_cast<float>(
                sdk::json_number(partValue, "maxHealth", 0.0));
            state.spec.detachable = sdk::json_bool(partValue, "detachable", false);
            state.spec.detachThreshold = static_cast<float>(
                sdk::json_number(partValue, "detachThreshold", 0.0));
            state.health = static_cast<float>(
                sdk::json_number(partValue, "health", 0.0));
            state.destroyed = sdk::json_bool(partValue, "destroyed", false);
            state.detached = sdk::json_bool(partValue, "detached", false);
            if (parsed.count(state.spec.name) != 0) {
                errorOut = "vehicle damage: duplicate part '" + state.spec.name + "'";
                return false;
            }
            parsed[state.spec.name] = std::move(state);
        }
        // Validação completa via check_parts.
        std::vector<VehiclePartSpec> specs;
        specs.reserve(parsed.size());
        for (const auto& entry : parsed) specs.push_back(entry.second.spec);
        if (!check_parts(specs, errorOut)) return false;
        // Saúde carregada dentro de [0, maxHealth]; flags consistentes.
        for (auto& entry : parsed) {
            if (!finite_float(entry.second.health) || entry.second.health < 0.0f) {
                entry.second.health = 0.0f;
            }
            if (entry.second.health > entry.second.spec.maxHealth) {
                entry.second.health = entry.second.spec.maxHealth;
            }
        }
        parts_ = std::move(parsed);
        return true;
    }

private:
    std::map<std::string, PartState> parts_;
};

}  // namespace

std::unique_ptr<IVehicleDamage> create_vehicle_damage() {
    return std::make_unique<VehicleDamage>();
}

}  // namespace vehicles
}  // namespace engine
