// Interaction.cpp — the only TU implementing the public interaction component
// (Agente 4 §1 item 26 "interação"): data-driven interaction points with
// radius/facing/cooldown evaluation, deterministic, JSON all-or-nothing.
// Pure std; RegistryJson only for the parser.

#include "engine/gameplay/IInteraction.hpp"

#include "RegistryJson.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace engine {
namespace gameplay {
namespace {

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

bool is_number(const sdk::JsonValue& v) {
    return v.kind == sdk::JsonValue::Kind::Number;
}

bool finite_float(float value) {
    return std::isfinite(value);
}

class Interaction final : public IInteraction {
public:
    Interaction() = default;

    bool configure(const std::vector<InteractionDef>& defs,
                   std::string& errorOut) override {
        std::map<std::string, InteractionDef> parsed;
        for (const InteractionDef& def : defs) {
            if (def.name.empty()) {
                errorOut = "interaction: name must be non-empty";
                return false;
            }
            if (def.prompt.empty()) {
                errorOut = "interaction: prompt for '" + def.name +
                           "' must be non-empty";
                return false;
            }
            if (!finite_float(def.radius) || def.radius <= 0.0f) {
                errorOut = "interaction: radius for '" + def.name +
                           "' must be > 0";
                return false;
            }
            if (!finite_float(def.cooldownSeconds) || def.cooldownSeconds < 0.0f) {
                errorOut = "interaction: cooldownSeconds for '" + def.name +
                           "' must be >= 0";
                return false;
            }
            if (parsed.count(def.name) != 0) {
                errorOut = "interaction: duplicate name '" + def.name + "'";
                return false;
            }
            parsed[def.name] = def;
        }
        defs_ = std::move(parsed);
        cooldowns_.clear();
        return true;
    }

    std::vector<InteractionState> evaluate(
        float entityX, float entityZ, float entityFacing,
        float interatorX, float interatorZ, float interatorFacing) override {
        std::vector<InteractionState> out;
        out.reserve(defs_.size());
        const float dx = entityX - interatorX;
        const float dz = entityZ - interatorZ;
        const float distance = std::sqrt(dx * dx + dz * dz);
        for (const auto& entry : defs_) {
            const InteractionDef& def = entry.second;
            InteractionState state;
            state.name = def.name;
            state.distance = distance;
            const auto cooldown = cooldowns_.find(def.name);
            state.remainingCooldown =
                cooldown == cooldowns_.end() ? 0.0f : cooldown->second;
            bool ok = distance <= def.radius + 1e-5f;
            if (ok && def.requiresFacing) {
                // dirX/dirZ = entidade − interator (direção interator→entidade).
                // cosEntity: entidade deve olhar para o interator (usa −dir).
                // cosInterator: interator deve olhar para a entidade (usa +dir).
                const float dirLen = std::sqrt(dx * dx + dz * dz);
                if (dirLen > 1e-6f) {
                    const float cosEntity =
                        (std::cos(entityFacing) * -dx +
                         std::sin(entityFacing) * -dz) / dirLen;
                    const float cosInterator =
                        (std::cos(interatorFacing) * dx +
                         std::sin(interatorFacing) * dz) / dirLen;
                    ok = cosEntity >= 0.0f && cosInterator >= 0.0f;
                } else {
                    ok = false;
                }
            }
            if (ok && state.remainingCooldown > 0.0f) ok = false;
            state.available = ok;
            out.push_back(state);
        }
        return out;
    }

    bool activate(const std::string& name) override {
        const auto def = defs_.find(name);
        if (def == defs_.end()) return false;
        if (def->second.cooldownSeconds <= 0.0f) {
            // Sem cooldown: registra 0 para não bloquear; ativação sempre ok
            // (a disponibilidade é avaliada por evaluate()).
            cooldowns_[name] = 0.0f;
            return true;
        }
        cooldowns_[name] = def->second.cooldownSeconds;
        return true;
    }

    bool advance(float dt) override {
        if (!finite_float(dt) || dt < 0.0f) return false;
        for (auto it = cooldowns_.begin(); it != cooldowns_.end();) {
            it->second -= dt;
            if (it->second <= 0.0f) {
                it = cooldowns_.erase(it);
            } else {
                ++it;
            }
        }
        return true;
    }

    bool to_json(std::string& outJson) const override {
        std::ostringstream out;
        out << "{\"version\":1,\"interactions\":[";
        bool first = true;
        for (const auto& entry : defs_) {
            const InteractionDef& def = entry.second;
            if (!first) out << ",";
            first = false;
            out << "{\"name\":\"" << json_escape(def.name)
                << "\",\"prompt\":\"" << json_escape(def.prompt)
                << "\",\"radius\":" << def.radius
                << ",\"requiresFacing\":" << (def.requiresFacing ? "true" : "false")
                << ",\"cooldownSeconds\":" << def.cooldownSeconds << "}";
        }
        out << "]}";
        outJson = out.str();
        return true;
    }

    bool from_json(const std::string& json, std::string& errorOut) override {
        sdk::JsonValue root;
        std::string parseError;
        if (!sdk::json_parse(json, root, parseError) ||
            root.kind != sdk::JsonValue::Kind::Object) {
            errorOut = "interaction: invalid JSON";
            return false;
        }
        const sdk::JsonValue* version = root.field("version");
        if (version == nullptr || !is_number(*version) || version->number != 1.0) {
            errorOut = "interaction: unsupported version";
            return false;
        }
        const sdk::JsonValue* list = root.field("interactions");
        if (list == nullptr || list->kind != sdk::JsonValue::Kind::Array) {
            errorOut = "interaction: missing interactions array";
            return false;
        }
        std::vector<InteractionDef> defs;
        defs.reserve(list->array.size());
        for (const sdk::JsonValue& item : list->array) {
            if (item.kind != sdk::JsonValue::Kind::Object) {
                errorOut = "interaction: element must be an object";
                return false;
            }
            InteractionDef def;
            const sdk::JsonValue* name = item.field("name");
            const sdk::JsonValue* prompt = item.field("prompt");
            const sdk::JsonValue* radius = item.field("radius");
            const sdk::JsonValue* facing = item.field("requiresFacing");
            const sdk::JsonValue* cooldown = item.field("cooldownSeconds");
            if (name == nullptr || name->kind != sdk::JsonValue::Kind::String ||
                prompt == nullptr || prompt->kind != sdk::JsonValue::Kind::String) {
                errorOut = "interaction: name/prompt must be strings";
                return false;
            }
            def.name = name->string;
            def.prompt = prompt->string;
            if (radius != nullptr && is_number(*radius)) {
                def.radius = static_cast<float>(radius->number);
            }
            if (facing != nullptr && facing->kind == sdk::JsonValue::Kind::Bool) {
                def.requiresFacing = facing->boolean;
            }
            if (cooldown != nullptr && is_number(*cooldown)) {
                def.cooldownSeconds = static_cast<float>(cooldown->number);
            }
            defs.push_back(def);
        }
        std::string configError;
        if (!configure(defs, configError)) {
            errorOut = "interaction: " + configError;
            return false;
        }
        return true;
    }

    std::size_t count() const override { return defs_.size(); }
    void clear() override {
        defs_.clear();
        cooldowns_.clear();
    }

private:
    std::map<std::string, InteractionDef> defs_;
    std::map<std::string, float> cooldowns_;
};

}  // namespace

std::unique_ptr<IInteraction> create_interaction() {
    return std::make_unique<Interaction>();
}

}  // namespace gameplay
}  // namespace engine
