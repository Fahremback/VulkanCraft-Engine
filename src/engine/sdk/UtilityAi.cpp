#include "engine/ai/IUtilityAi.hpp"

#include "RegistryJson.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <map>
#include <sstream>

namespace engine {
namespace ai {
namespace {

bool finite(double v) {
    return std::isfinite(v);
}

double clamp01(double v) {
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
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

bool is_uint64(const sdk::JsonValue& v) {
    return v.kind == sdk::JsonValue::Kind::Number && v.number >= 0.0 &&
           v.number == std::floor(v.number);
}

const char* curve_name(UtilityCurve c) {
    switch (c) {
        case UtilityCurve::Linear: return "linear";
        case UtilityCurve::Inverse: return "inverse";
        case UtilityCurve::Step: return "step";
    }
    return "linear";
}

bool curve_from_name(const std::string& name, UtilityCurve& out) {
    if (name == "linear") { out = UtilityCurve::Linear; return true; }
    if (name == "inverse") { out = UtilityCurve::Inverse; return true; }
    if (name == "step") { out = UtilityCurve::Step; return true; }
    return false;
}

bool string_field(const sdk::JsonValue& obj, const char* key, std::string& out,
                  bool required, std::string& errorOut) {
    const sdk::JsonValue* f = obj.field(key);
    if (f == nullptr) {
        if (required) {
            errorOut = std::string("missing field ") + key;
            return false;
        }
        return true;
    }
    if (f->kind != sdk::JsonValue::Kind::String) {
        errorOut = std::string(key) + " must be a string";
        return false;
    }
    out = f->string;
    return true;
}

bool number_field(const sdk::JsonValue& obj, const char* key, double& out,
                  bool required, std::string& errorOut) {
    const sdk::JsonValue* f = obj.field(key);
    if (f == nullptr) {
        if (required) {
            errorOut = std::string("missing field ") + key;
            return false;
        }
        return true;
    }
    if (f->kind != sdk::JsonValue::Kind::Number) {
        errorOut = std::string(key) + " must be a number";
        return false;
    }
    out = f->number;
    return true;
}

}  // namespace

bool UtilitySpec::validate(std::string& errorOut) const {
    if (actions.empty()) {
        errorOut = "utility spec needs at least one action";
        return false;
    }
    std::map<std::string, bool> ids;
    for (const auto& a : actions) {
        if (a.id.empty()) {
            errorOut = "action id must be non-empty";
            return false;
        }
        if (ids.count(a.id)) {
            errorOut = "duplicate action id \"" + a.id + "\"";
            return false;
        }
        ids[a.id] = true;
        for (const auto& c : a.considerations) {
            if (c.input.empty()) {
                errorOut = "consideration input must be non-empty (action \"" + a.id + "\")";
                return false;
            }
            if (!finite(c.weight) || c.weight < 0.0) {
                errorOut = "weight must be finite and >= 0";
                return false;
            }
            if (!finite(c.min) || !finite(c.max) || c.max <= c.min) {
                errorOut = "min/max must be finite with max > min";
                return false;
            }
            if (!finite(c.threshold) || c.threshold < 0.0 || c.threshold > 1.0) {
                errorOut = "threshold must be finite and in [0, 1]";
                return false;
            }
        }
    }
    errorOut.clear();
    return true;
}

bool UtilitySpec::load_from_json(const std::string& json, std::string& errorOut) {
    sdk::JsonValue doc;
    if (!sdk::json_parse(json, doc, errorOut)) {
        return false;
    }
    if (!doc.is_object()) {
        errorOut = "utility spec must be an object";
        return false;
    }
    const sdk::JsonValue* version = doc.field("version");
    if (version != nullptr &&
        (!is_uint64(*version) || static_cast<int>(version->number) != 1)) {
        errorOut = "unsupported utility spec version";
        return false;
    }
    const sdk::JsonValue* actionsField = doc.field("actions");
    if (actionsField == nullptr || !actionsField->is_array()) {
        errorOut = "utility spec needs an actions array";
        return false;
    }
    UtilitySpec candidate;
    for (const auto& item : actionsField->array) {
        if (!item.is_object()) {
            errorOut = "action entries must be objects";
            return false;
        }
        UtilityAction action;
        if (!string_field(item, "id", action.id, true, errorOut)) return false;
        const sdk::JsonValue* consField = item.field("considerations");
        if (consField != nullptr) {
            if (!consField->is_array()) {
                errorOut = "considerations must be an array";
                return false;
            }
            for (const auto& c : consField->array) {
                if (!c.is_object()) {
                    errorOut = "consideration entries must be objects";
                    return false;
                }
                UtilityConsideration cons;
                if (!string_field(c, "input", cons.input, true, errorOut)) return false;
                std::string curveName = "linear";
                if (!string_field(c, "curve", curveName, false, errorOut)) return false;
                if (!curve_from_name(curveName, cons.curve)) {
                    errorOut = "unknown curve \"" + curveName + "\"";
                    return false;
                }
                if (!number_field(c, "weight", cons.weight, false, errorOut)) return false;
                if (!number_field(c, "min", cons.min, false, errorOut)) return false;
                if (!number_field(c, "max", cons.max, false, errorOut)) return false;
                if (!number_field(c, "threshold", cons.threshold, false, errorOut)) return false;
                action.considerations.push_back(cons);
            }
        }
        candidate.actions.push_back(action);
    }
    if (!candidate.validate(errorOut)) {
        return false;
    }
    *this = std::move(candidate);
    return true;
}

std::string UtilitySpec::to_json() const {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "{\"version\":1,\"actions\":[";
    for (std::size_t i = 0; i < actions.size(); ++i) {
        if (i) out << ",";
        out << "{\"id\":\"" << json_escape(actions[i].id) << "\",\"considerations\":[";
        for (std::size_t j = 0; j < actions[i].considerations.size(); ++j) {
            if (j) out << ",";
            const auto& c = actions[i].considerations[j];
            out << "{\"input\":\"" << json_escape(c.input) << "\",\"curve\":\""
                << curve_name(c.curve) << "\",\"weight\":" << c.weight
                << ",\"min\":" << c.min << ",\"max\":" << c.max
                << ",\"threshold\":" << c.threshold << "}";
        }
        out << "]}";
    }
    out << "]}";
    return out.str();
}

namespace {

double consideration_score(const UtilityConsideration& c, double input_value) {
    const double normalized = clamp01((input_value - c.min) / (c.max - c.min));
    switch (c.curve) {
        case UtilityCurve::Inverse: return 1.0 - normalized;
        case UtilityCurve::Step: return normalized >= c.threshold ? 1.0 : 0.0;
        case UtilityCurve::Linear: return normalized;
    }
    return normalized;
}

double action_utility(const UtilityAction& a, const std::map<std::string, double>& inputs) {
    double weighted = 0.0;
    double weight_sum = 0.0;
    for (const auto& c : a.considerations) {
        if (c.weight <= 0.0) {
            continue;  // peso 0 ignorado
        }
        auto it = inputs.find(c.input);
        const double value = it == inputs.end() ? 0.0 : it->second;
        weighted += c.weight * consideration_score(c, value);
        weight_sum += c.weight;
    }
    if (weight_sum <= 0.0) {
        return 0.0;
    }
    return weighted / weight_sum;
}

class UtilityAi final : public IUtilityAi {
public:
    bool configure(const UtilitySpec& spec, std::string& errorOut) override {
        if (!spec.validate(errorOut)) {
            return false;
        }
        spec_ = spec;
        inputs_.clear();
        return true;
    }

    void set_input(const std::string& name, double value) override {
        inputs_[name] = value;
    }

    double score(const std::string& id) const override {
        for (const auto& a : spec_.actions) {
            if (a.id == id) {
                return action_utility(a, inputs_);
            }
        }
        return 0.0;
    }

    UtilitySelection select() const override {
        UtilitySelection best;
        double best_utility = -1.0;  // utilidades são >= 0; -1 = nenhum
        for (const auto& a : spec_.actions) {
            const double u = action_utility(a, inputs_);
            // empate → primeira na ordem de declaração (não substitui)
            if (u > best_utility) {
                best_utility = u;
                best.id = a.id;
                best.utility = u;
            }
        }
        return best;
    }

    std::vector<UtilityScore> utilities() const override {
        std::vector<UtilityScore> out;
        out.reserve(spec_.actions.size());
        for (const auto& a : spec_.actions) {
            out.push_back(UtilityScore{a.id, action_utility(a, inputs_)});
        }
        // utilidade desc; empate → ordem de declaração (stable_sort mantém).
        std::stable_sort(out.begin(), out.end(),
                         [](const UtilityScore& a, const UtilityScore& b) {
                             return a.utility > b.utility;
                         });
        return out;
    }

private:
    UtilitySpec spec_;
    std::map<std::string, double> inputs_;
};

}  // namespace

std::unique_ptr<IUtilityAi> create_utility_ai() {
    return std::make_unique<UtilityAi>();
}

}  // namespace ai
}  // namespace engine
