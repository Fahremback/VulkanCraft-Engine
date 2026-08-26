#include "engine/ai/IPlanner.hpp"

#include "RegistryJson.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <limits>
#include <queue>
#include <set>
#include <sstream>

namespace engine {
namespace ai {
namespace {

bool finite(double v) {
    return std::isfinite(v);
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

// Lê um mapa nome→bool (preconditions/effects).
bool bool_map_field(const sdk::JsonValue& obj, const char* key,
                    std::map<std::string, bool>& out, std::string& errorOut) {
    const sdk::JsonValue* f = obj.field(key);
    if (f == nullptr) {
        return true;
    }
    if (!f->is_object()) {
        errorOut = std::string(key) + " must be an object";
        return false;
    }
    for (const auto& kv : f->object) {
        if (kv.first.empty()) {
            errorOut = std::string(key) + " atom names must be non-empty";
            return false;
        }
        if (kv.second.kind != sdk::JsonValue::Kind::Bool) {
            errorOut = std::string(key) + "." + kv.first + " must be a bool";
            return false;
        }
        out[kv.first] = kv.second.boolean;
    }
    return true;
}

}  // namespace

bool PlannerSpec::validate(std::string& errorOut) const {
    if (actions.empty()) {
        errorOut = "planner spec needs at least one action";
        return false;
    }
    if (max_plan_length < 1) {
        errorOut = "max_plan_length must be >= 1";
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
        if (!finite(a.cost) || a.cost <= 0.0) {
            errorOut = "action cost must be finite and > 0";
            return false;
        }
    }
    errorOut.clear();
    return true;
}

bool PlannerSpec::load_from_json(const std::string& json, std::string& errorOut) {
    sdk::JsonValue doc;
    if (!sdk::json_parse(json, doc, errorOut)) {
        return false;
    }
    if (!doc.is_object()) {
        errorOut = "planner spec must be an object";
        return false;
    }
    const sdk::JsonValue* version = doc.field("version");
    if (version != nullptr &&
        (!is_uint64(*version) || static_cast<int>(version->number) != 1)) {
        errorOut = "unsupported planner spec version";
        return false;
    }
    PlannerSpec candidate;
    const sdk::JsonValue* len = doc.field("max_plan_length");
    if (len != nullptr) {
        if (!is_uint64(*len)) {
            errorOut = "max_plan_length must be a non-negative integer";
            return false;
        }
        candidate.max_plan_length = static_cast<int>(len->number);
    }
    const sdk::JsonValue* actionsField = doc.field("actions");
    if (actionsField == nullptr || !actionsField->is_array()) {
        errorOut = "planner spec needs an actions array";
        return false;
    }
    for (const auto& item : actionsField->array) {
        if (!item.is_object()) {
            errorOut = "action entries must be objects";
            return false;
        }
        PlannerAction action;
        if (!string_field(item, "id", action.id, true, errorOut)) return false;
        if (!number_field(item, "cost", action.cost, false, errorOut)) return false;
        if (!bool_map_field(item, "preconditions", action.preconditions, errorOut)) {
            return false;
        }
        if (!bool_map_field(item, "effects", action.effects, errorOut)) {
            return false;
        }
        candidate.actions.push_back(action);
    }
    if (!candidate.validate(errorOut)) {
        return false;
    }
    *this = std::move(candidate);
    return true;
}

std::string PlannerSpec::to_json() const {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "{\"version\":1,\"max_plan_length\":" << max_plan_length
        << ",\"actions\":[";
    for (std::size_t i = 0; i < actions.size(); ++i) {
        if (i) out << ",";
        const auto& a = actions[i];
        out << "{\"id\":\"" << json_escape(a.id) << "\",\"cost\":" << a.cost
            << ",\"preconditions\":{";
        bool first = true;
        for (const auto& kv : a.preconditions) {
            if (!first) out << ",";
            first = false;
            out << "\"" << json_escape(kv.first) << "\":"
                << (kv.second ? "true" : "false");
        }
        out << "},\"effects\":{";
        first = true;
        for (const auto& kv : a.effects) {
            if (!first) out << ",";
            first = false;
            out << "\"" << json_escape(kv.first) << "\":"
                << (kv.second ? "true" : "false");
        }
        out << "}}";
    }
    out << "]}";
    return out.str();
}

namespace {

// Estado canônico: bitmask sobre átomos ordenados.
using StateMask = std::uint64_t;

struct Node {
    StateMask mask = 0;
    double cost = 0.0;
    std::vector<int> path;  // índices de ações (ordem de declaração)
};

class Planner final : public IPlanner {
public:
    bool configure(const PlannerSpec& spec, std::string& errorOut) override {
        if (!spec.validate(errorOut)) {
            return false;
        }
        spec_ = spec;
        state_.clear();
        goal_.clear();
        return true;
    }

    void set_atom(const std::string& name, bool value) override {
        state_[name] = value;
    }

    void set_goal(const std::string& name, bool value) override {
        goal_[name] = value;
    }

    PlanResult plan(std::string& errorOut) override {
        // Universo de átomos: spec + estado + objetivo, ordenado (canônico).
        std::set<std::string> names;
        for (const auto& a : spec_.actions) {
            for (const auto& kv : a.preconditions) names.insert(kv.first);
            for (const auto& kv : a.effects) names.insert(kv.first);
        }
        for (const auto& kv : state_) names.insert(kv.first);
        for (const auto& kv : goal_) names.insert(kv.first);

        std::vector<std::string> atoms(names.begin(), names.end());
        std::map<std::string, std::size_t> index;
        for (std::size_t i = 0; i < atoms.size(); ++i) {
            index[atoms[i]] = i;
        }

        const StateMask initial = to_mask(state_, index);
        StateMask goal_true = 0;
        StateMask goal_false = 0;
        for (const auto& kv : goal_) {
            const auto it = index.find(kv.first);
            if (it == index.end()) continue;  // não deveria acontecer (universo)
            if (kv.second) {
                goal_true |= (StateMask{1} << it->second);
            } else {
                goal_false |= (StateMask{1} << it->second);
            }
        }

        PlanResult result;
        if (goal_satisfied(initial, goal_true, goal_false)) {
            result.success = true;
            errorOut.clear();
            return result;
        }

        // Uniform-cost com empate determinístico: (cost, seq).
        struct FrontierEntry {
            double cost;
            std::uint64_t seq;
            Node node;
        };
        struct FrontierLess {
            bool operator()(const FrontierEntry& a, const FrontierEntry& b) const {
                if (a.cost != b.cost) return a.cost > b.cost;
                return a.seq > b.seq;  // menor seq primeiro → FIFO determinístico
            }
        };

        std::priority_queue<FrontierEntry, std::vector<FrontierEntry>,
                            FrontierLess>
            frontier;
        std::set<StateMask> closed;
        std::uint64_t seq_counter = 0;

        Node start;
        start.mask = initial;
        frontier.push(FrontierEntry{0.0, seq_counter++, start});
        closed.insert(initial);

        while (!frontier.empty()) {
            FrontierEntry top = frontier.top();
            frontier.pop();
            Node cur = top.node;

            if (goal_satisfied(cur.mask, goal_true, goal_false)) {
                result.success = true;
                result.total_cost = cur.cost;
                result.actions.reserve(cur.path.size());
                for (const int ai : cur.path) {
                    result.actions.push_back(spec_.actions[static_cast<std::size_t>(ai)].id);
                }
                errorOut.clear();
                return result;
            }
            if (static_cast<int>(cur.path.size()) >= spec_.max_plan_length) {
                continue;  // plano no limite não expande (terminação)
            }

            for (std::size_t ai = 0; ai < spec_.actions.size(); ++ai) {
                const auto& action = spec_.actions[ai];
                if (!applicable(cur.mask, action, index)) {
                    continue;
                }
                // uma ação não se repete no plano
                if (std::find(cur.path.begin(), cur.path.end(), static_cast<int>(ai)) !=
                    cur.path.end()) {
                    continue;
                }
                const StateMask next = apply(cur.mask, action, index);
                if (closed.count(next)) {
                    continue;
                }
                closed.insert(next);
                Node child = cur;
                child.mask = next;
                child.cost = cur.cost + action.cost;
                child.path.push_back(static_cast<int>(ai));
                frontier.push(FrontierEntry{child.cost, seq_counter++, child});
            }
        }

        result.success = false;
        errorOut.clear();
        return result;
    }

private:
    static StateMask to_mask(const std::map<std::string, bool>& values,
                             const std::map<std::string, std::size_t>& index) {
        StateMask mask = 0;
        for (const auto& kv : values) {
            if (!kv.second) continue;
            const auto it = index.find(kv.first);
            if (it != index.end()) {
                mask |= (StateMask{1} << it->second);
            }
        }
        return mask;
    }

    static bool applicable(StateMask mask, const PlannerAction& action,
                           const std::map<std::string, std::size_t>& index) {
        for (const auto& kv : action.preconditions) {
            const auto it = index.find(kv.first);
            if (it == index.end()) return false;
            const bool bit = (mask >> it->second) & StateMask{1};
            if (bit != kv.second) return false;
        }
        return true;
    }

    static StateMask apply(StateMask mask, const PlannerAction& action,
                           const std::map<std::string, std::size_t>& index) {
        for (const auto& kv : action.effects) {
            const auto it = index.find(kv.first);
            if (it == index.end()) continue;
            const StateMask bit = StateMask{1} << it->second;
            if (kv.second) {
                mask |= bit;
            } else {
                mask &= ~bit;
            }
        }
        return mask;
    }

    static bool goal_satisfied(StateMask mask, StateMask goal_true,
                               StateMask goal_false) {
        return (mask & goal_true) == goal_true && (mask & goal_false) == 0;
    }

    PlannerSpec spec_;
    std::map<std::string, bool> state_;
    std::map<std::string, bool> goal_;
};

}  // namespace

std::unique_ptr<IPlanner> create_planner() {
    return std::make_unique<Planner>();
}

}  // namespace ai
}  // namespace engine
