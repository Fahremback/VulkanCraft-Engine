// ActionMap.cpp — the ONLY TU with the input-map runtime (agente 4 §1 item 8).
// Pure and deterministic: poll() resolves a raw input event against the map
// and returns the activated actions; no device, no wall clock, no RNG. JSON
// parse/emit uses the shared RegistryJson helpers.

#include "engine/input/IActionMap.hpp"

#include "RegistryJson.hpp"

#include <cmath>
#include <sstream>

namespace engine {
namespace input {

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

const char* source_name(InputSource source) {
    switch (source) {
        case InputSource::Keyboard: return "keyboard";
        case InputSource::Mouse: return "mouse";
        case InputSource::Gamepad: return "gamepad";
        case InputSource::Touch: return "touch";
        case InputSource::Other: break;
    }
    return "other";
}

bool source_from_name(const std::string& name, InputSource& out) {
    if (name == "keyboard") { out = InputSource::Keyboard; return true; }
    if (name == "mouse") { out = InputSource::Mouse; return true; }
    if (name == "gamepad") { out = InputSource::Gamepad; return true; }
    if (name == "touch") { out = InputSource::Touch; return true; }
    if (name == "other") { out = InputSource::Other; return true; }
    return false;
}

bool is_finite(double v) { return std::isfinite(v); }

// Two bindings "physically" collide when they name the same source/device/
// input/axis (the scale/deadzone are per-binding modifiers, not identity).
bool same_physical(const InputBinding& a, const InputBinding& b) {
    return a.source == b.source && a.device == b.device && a.input == b.input &&
           a.axis == b.axis;
}

std::string binding_to_json(const InputBinding& b) {
    std::ostringstream out;
    out << "{\"source\":\"" << source_name(b.source) << "\",\"device\":\""
        << json_escape(b.device) << "\",\"input\":\"" << json_escape(b.input)
        << "\",\"axis\":" << b.axis;
    out << ",\"scale\":";
    std::ostringstream scale;
    scale.precision(9);
    scale << b.scale;
    out << scale.str();
    out << ",\"deadzone\":";
    std::ostringstream dead;
    dead.precision(9);
    dead << b.deadzone;
    out << dead.str();
    out << '}';
    return out.str();
}

bool binding_from_json(const sdk::JsonValue& obj, InputBinding& out,
                       std::string& errorOut) {
    const sdk::JsonValue* sourceField = obj.field("source");
    if (sourceField == nullptr || !sourceField->is_string()) {
        errorOut = "binding missing source";
        return false;
    }
    if (!source_from_name(sourceField->string, out.source)) {
        errorOut = "unknown input source: " + sourceField->string;
        return false;
    }
    out.device = sdk::json_string(obj, "device", "");
    out.input = sdk::json_string(obj, "input", "");
    out.axis = static_cast<int>(sdk::json_number(obj, "axis", 0));
    out.scale = sdk::json_number(obj, "scale", 1.0);
    out.deadzone = sdk::json_number(obj, "deadzone", 0.0);
    return true;
}

bool binding_validate(const InputBinding& b, std::string& errorOut) {
    if (b.input.empty()) {
        errorOut = "binding has an empty input name";
        return false;
    }
    if (!is_finite(b.scale)) {
        errorOut = "binding scale must be finite";
        return false;
    }
    if (!is_finite(b.deadzone) || b.deadzone < 0.0 || b.deadzone > 1.0) {
        errorOut = "binding deadzone must be in [0,1]";
        return false;
    }
    if (b.axis < 0) {
        errorOut = "binding axis must be >= 0";
        return false;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Spec validation + JSON
// ---------------------------------------------------------------------------

bool ActionMapSpec::validate(std::string& errorOut) const {
    errorOut.clear();
    if (version != 1) {
        errorOut = "unsupported action map version";
        return false;
    }
    std::map<std::string, bool> seen;
    for (const ActionBinding& action : actions) {
        if (action.action.empty()) {
            errorOut = "action has an empty name";
            return false;
        }
        if (seen.count(action.action)) {
            errorOut = "duplicate action: " + action.action;
            return false;
        }
        seen[action.action] = true;
        if (action.bindings.empty()) {
            errorOut = "action \"" + action.action + "\" has no bindings";
            return false;
        }
        for (const InputBinding& b : action.bindings) {
            if (!binding_validate(b, errorOut)) return false;
        }
    }
    // Hard conflict across actions: two actions sharing the same physical
    // binding are refused (the spec is the canonical source).
    for (std::size_t i = 0; i < actions.size(); ++i) {
        for (std::size_t j = i + 1; j < actions.size(); ++j) {
            for (const InputBinding& a : actions[i].bindings) {
                for (const InputBinding& b : actions[j].bindings) {
                    if (same_physical(a, b)) {
                        errorOut = "conflicting bindings between \"" +
                                   actions[i].action + "\" and \"" +
                                   actions[j].action + "\"";
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

std::string ActionMapSpec::to_json() const {
    std::ostringstream out;
    out << "{\"version\":1,\"actions\":[";
    for (std::size_t i = 0; i < actions.size(); ++i) {
        if (i != 0) out << ',';
        const ActionBinding& action = actions[i];
        out << "{\"action\":\"" << json_escape(action.action) << "\",\"bindings\":[";
        for (std::size_t k = 0; k < action.bindings.size(); ++k) {
            if (k != 0) out << ',';
            out << binding_to_json(action.bindings[k]);
        }
        out << "]}";
    }
    out << "]}";
    return out.str();
}

bool ActionMapSpec::load_from_json(const std::string& jsonText,
                                   std::string& errorOut) {
    sdk::JsonValue doc;
    if (!sdk::json_parse(jsonText, doc, errorOut)) return false;
    if (!doc.is_object()) {
        errorOut = "action map document must be an object";
        return false;
    }
    const int version = static_cast<int>(sdk::json_number(doc, "version", 1));
    if (version != 1) {
        errorOut = "unsupported action map version";
        return false;
    }
    const sdk::JsonValue* actionsField = doc.field("actions");
    if (actionsField == nullptr || !actionsField->is_array()) {
        errorOut = "action map missing actions array";
        return false;
    }
    ActionMapSpec candidate;
    candidate.version = version;
    candidate.actions.reserve(actionsField->array.size());
    for (const sdk::JsonValue& actionObj : actionsField->array) {
        if (!actionObj.is_object()) {
            errorOut = "action must be an object";
            return false;
        }
        ActionBinding action;
        action.action = sdk::json_string(actionObj, "action", "");
        const sdk::JsonValue* bindingsField = actionObj.field("bindings");
        if (bindingsField == nullptr || !bindingsField->is_array()) {
            errorOut = "action \"" + action.action + "\" missing bindings array";
            return false;
        }
        for (const sdk::JsonValue& bindingObj : bindingsField->array) {
            if (!bindingObj.is_object()) {
                errorOut = "binding must be an object";
                return false;
            }
            InputBinding b;
            if (!binding_from_json(bindingObj, b, errorOut)) return false;
            action.bindings.push_back(std::move(b));
        }
        candidate.actions.push_back(std::move(action));
    }
    if (!candidate.validate(errorOut)) return false;
    *this = std::move(candidate);
    return true;
}

// ---------------------------------------------------------------------------
// Runtime
// ---------------------------------------------------------------------------

namespace {

class ActionMapRuntime final : public IActionMap {
public:
    explicit ActionMapRuntime(const ActionMapSpec& spec) : spec_(spec) {}

    std::vector<ActionActivation> poll(InputSource source,
                                       const std::string& device,
                                       const std::string& input, int axis,
                                       double value, double dt) override {
        (void)dt;
        std::vector<ActionActivation> result;
        if (!is_finite(value)) return result;
        for (std::size_t ai = 0; ai < spec_.actions.size(); ++ai) {
            const ActionBinding& action = spec_.actions[ai];
            for (std::size_t bi = 0; bi < action.bindings.size(); ++bi) {
                const InputBinding& b = action.bindings[bi];
                if (b.source != source || b.device != device ||
                    b.input != input || b.axis != axis) {
                    continue;
                }
                double v = value * b.scale;
                if (std::fabs(v) < b.deadzone) v = 0.0;
                ActionActivation activation;
                activation.action = action.action;
                activation.value = v;
                activation.bindingIndex = bi;
                result.push_back(activation);
                // First matching binding per action wins (a button + a key
                // bound to the same action still resolve to one activation).
                break;
            }
        }
        return result;
    }

    bool rebind(const std::string& action, std::size_t slot,
                const InputBinding& binding, bool overrideBinding,
                std::string& errorOut) override {
        errorOut.clear();
        if (!binding_validate(binding, errorOut)) return false;

        // Locate the action.
        std::size_t actionIndex = spec_.actions.size();
        for (std::size_t i = 0; i < spec_.actions.size(); ++i) {
            if (spec_.actions[i].action == action) {
                actionIndex = i;
                break;
            }
        }
        if (actionIndex == spec_.actions.size()) {
            errorOut = "unknown action: " + action;
            return false;
        }
        ActionBinding& target = spec_.actions[actionIndex];
        if (slot >= target.bindings.size()) {
            errorOut = "binding slot out of range";
            return false;
        }

        // Conflict: the new binding must not physically collide with another
        // action's binding (or another slot of the same action) unless
        // override.
        if (!overrideBinding) {
            for (std::size_t i = 0; i < spec_.actions.size(); ++i) {
                const ActionBinding& other = spec_.actions[i];
                for (std::size_t k = 0; k < other.bindings.size(); ++k) {
                    if (i == actionIndex && k == slot) continue;
                    if (same_physical(binding, other.bindings[k])) {
                        errorOut = "conflict with action \"" + other.action + "\"";
                        return false;
                    }
                }
            }
        }

        target.bindings[slot] = binding;
        return true;
    }

    std::vector<std::pair<std::string, std::size_t>> conflicts() const override {
        std::vector<std::pair<std::string, std::size_t>> result;
        for (std::size_t i = 0; i < spec_.actions.size(); ++i) {
            for (std::size_t bi = 0; bi < spec_.actions[i].bindings.size(); ++bi) {
                const InputBinding& b = spec_.actions[i].bindings[bi];
                for (std::size_t j = 0; j < spec_.actions.size(); ++j) {
                    if (j == i) continue;
                    for (const InputBinding& other : spec_.actions[j].bindings) {
                        if (same_physical(b, other)) {
                            result.emplace_back(spec_.actions[i].action, bi);
                            break;
                        }
                    }
                }
            }
        }
        return result;
    }

    const ActionMapSpec& spec() const override { return spec_; }

private:
    ActionMapSpec spec_;
};

}  // namespace

std::unique_ptr<IActionMap> create_action_map(const ActionMapSpec& spec,
                                              std::string& errorOut) {
    errorOut.clear();
    if (!spec.validate(errorOut)) return nullptr;
    return std::make_unique<ActionMapRuntime>(spec);
}

}  // namespace input
}  // namespace engine
