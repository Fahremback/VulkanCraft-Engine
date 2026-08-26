#include "engine/ai/IFsm.hpp"

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

bool bool_field(const sdk::JsonValue& obj, const char* key, bool& out,
                std::string& errorOut) {
    const sdk::JsonValue* f = obj.field(key);
    if (f == nullptr) {
        return true;
    }
    if (f->kind != sdk::JsonValue::Kind::Bool) {
        errorOut = std::string(key) + " must be a bool";
        return false;
    }
    out = f->boolean;
    return true;
}

// Conta gatilhos ativos de uma transição.
int trigger_count(const FsmTransition& t) {
    return (t.on_event.empty() ? 0 : 1) + (t.on_condition.empty() ? 0 : 1) +
           (t.after_seconds > 0.0 ? 1 : 0);
}

}  // namespace

bool FsmSpec::validate(std::string& errorOut) const {
    if (states.empty()) {
        errorOut = "fsm needs at least one state";
        return false;
    }
    std::map<std::string, bool> ids;
    for (const auto& s : states) {
        if (s.id.empty()) {
            errorOut = "state id must be non-empty";
            return false;
        }
        if (ids.count(s.id)) {
            errorOut = "duplicate state id \"" + s.id + "\"";
            return false;
        }
        ids[s.id] = true;
    }
    if (!ids.count(initial)) {
        errorOut = "initial state \"" + initial + "\" does not exist";
        return false;
    }
    for (const auto& t : transitions) {
        if (!ids.count(t.from)) {
            errorOut = "transition from unknown state \"" + t.from + "\"";
            return false;
        }
        if (!ids.count(t.to)) {
            errorOut = "transition to unknown state \"" + t.to + "\"";
            return false;
        }
        const int triggers = trigger_count(t);
        if (triggers != 1) {
            errorOut = "transition " + t.from + "->" + t.to +
                       " needs exactly one trigger (event/condition/time)";
            return false;
        }
        if (!finite(t.after_seconds) || t.after_seconds < 0.0) {
            errorOut = "after_seconds must be finite and >= 0";
            return false;
        }
    }
    errorOut.clear();
    return true;
}

bool FsmSpec::load_from_json(const std::string& json, std::string& errorOut) {
    sdk::JsonValue doc;
    if (!sdk::json_parse(json, doc, errorOut)) {
        return false;
    }
    if (!doc.is_object()) {
        errorOut = "fsm spec must be an object";
        return false;
    }
    const sdk::JsonValue* version = doc.field("version");
    if (version != nullptr &&
        (!is_uint64(*version) || static_cast<int>(version->number) != 1)) {
        errorOut = "unsupported fsm version";
        return false;
    }
    FsmSpec candidate;
    if (!string_field(doc, "initial", candidate.initial, true, errorOut)) {
        return false;
    }
    const sdk::JsonValue* statesField = doc.field("states");
    if (statesField == nullptr || !statesField->is_array()) {
        errorOut = "fsm needs a states array";
        return false;
    }
    for (const auto& item : statesField->array) {
        if (!item.is_object()) {
            errorOut = "state entries must be objects";
            return false;
        }
        FsmState s;
        if (!string_field(item, "id", s.id, true, errorOut)) return false;
        if (!string_field(item, "enter", s.enter, false, errorOut)) return false;
        if (!string_field(item, "update", s.update, false, errorOut)) return false;
        if (!string_field(item, "exit", s.exit, false, errorOut)) return false;
        if (!bool_field(item, "terminal", s.terminal, errorOut)) return false;
        candidate.states.push_back(s);
    }
    const sdk::JsonValue* transField = doc.field("transitions");
    if (transField != nullptr) {
        if (!transField->is_array()) {
            errorOut = "transitions must be an array";
            return false;
        }
        for (const auto& item : transField->array) {
            if (!item.is_object()) {
                errorOut = "transition entries must be objects";
                return false;
            }
            FsmTransition t;
            if (!string_field(item, "from", t.from, true, errorOut)) return false;
            if (!string_field(item, "to", t.to, true, errorOut)) return false;
            if (!string_field(item, "on_event", t.on_event, false, errorOut)) return false;
            if (!string_field(item, "on_condition", t.on_condition, false, errorOut)) return false;
            const sdk::JsonValue* after = item.field("after_seconds");
            if (after != nullptr) {
                if (after->kind != sdk::JsonValue::Kind::Number) {
                    errorOut = "after_seconds must be a number";
                    return false;
                }
                t.after_seconds = after->number;
            }
            candidate.transitions.push_back(t);
        }
    }
    if (!candidate.validate(errorOut)) {
        return false;
    }
    *this = std::move(candidate);
    return true;
}

std::string FsmSpec::to_json() const {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "{\"version\":1,\"initial\":\"" << json_escape(initial) << "\",\"states\":[";
    for (std::size_t i = 0; i < states.size(); ++i) {
        if (i) out << ",";
        out << "{\"id\":\"" << json_escape(states[i].id) << "\"";
        if (!states[i].enter.empty()) {
            out << ",\"enter\":\"" << json_escape(states[i].enter) << "\"";
        }
        if (!states[i].update.empty()) {
            out << ",\"update\":\"" << json_escape(states[i].update) << "\"";
        }
        if (!states[i].exit.empty()) {
            out << ",\"exit\":\"" << json_escape(states[i].exit) << "\"";
        }
        if (states[i].terminal) {
            out << ",\"terminal\":true";
        }
        out << "}";
    }
    out << "],\"transitions\":[";
    for (std::size_t i = 0; i < transitions.size(); ++i) {
        if (i) out << ",";
        const auto& t = transitions[i];
        out << "{\"from\":\"" << json_escape(t.from) << "\",\"to\":\""
            << json_escape(t.to) << "\"";
        if (!t.on_event.empty()) {
            out << ",\"on_event\":\"" << json_escape(t.on_event) << "\"";
        }
        if (!t.on_condition.empty()) {
            out << ",\"on_condition\":\"" << json_escape(t.on_condition) << "\"";
        }
        if (t.after_seconds > 0.0) {
            out << ",\"after_seconds\":" << t.after_seconds;
        }
        out << "}";
    }
    out << "]}";
    return out.str();
}

namespace {

class Fsm final : public IFsm {
public:
    bool configure(const FsmSpec& spec, std::string& errorOut) override {
        if (!spec.validate(errorOut)) {
            return false;
        }
        spec_ = spec;
        running_ = false;
        actions_.clear();
        return true;
    }

    bool start(std::string& errorOut) override {
        if (spec_.states.empty()) {
            errorOut = "fsm not configured";
            return false;
        }
        current_ = spec_.initial;
        time_in_state_ = 0.0;
        running_ = true;
        for (const auto& s : spec_.states) {
            if (s.id == current_ && !s.enter.empty()) {
                actions_.push_back(s.enter);
                break;
            }
        }
        errorOut.clear();
        return true;
    }

    bool tick(double dt, std::string& errorOut) override {
        if (!running_) {
            errorOut = "fsm not started";
            return false;
        }
        if (!finite(dt) || dt < 0.0) {
            errorOut = "dt must be finite and >= 0";
            return false;
        }
        time_in_state_ += dt;

        // Condição + timer, em ordem de declaração; no máximo uma transição.
        for (const auto& t : spec_.transitions) {
            if (t.from != current_) {
                continue;
            }
            bool fire = false;
            if (!t.on_condition.empty()) {
                auto it = conditions_.find(t.on_condition);
                fire = it != conditions_.end() && it->second;
            } else if (t.after_seconds > 0.0) {
                fire = time_in_state_ >= t.after_seconds;
            }
            if (fire) {
                do_transition(t);
                break;  // primeira que casa vence
            }
        }

        // update_action do estado atual (após possível transição).
        for (const auto& s : spec_.states) {
            if (s.id == current_ && !s.update.empty()) {
                actions_.push_back(s.update);
                break;
            }
        }
        errorOut.clear();
        return true;
    }

    bool send_event(const std::string& name, std::string& errorOut) override {
        if (!running_) {
            errorOut = "fsm not started";
            return false;
        }
        for (const auto& t : spec_.transitions) {
            if (t.from == current_ && !t.on_event.empty() && t.on_event == name) {
                do_transition(t);
                break;  // primeira que casa vence
            }
        }
        errorOut.clear();
        return true;
    }

    void set_condition(const std::string& name, bool value) override {
        conditions_[name] = value;
    }

    std::string state() const override { return current_; }

    bool done() const override {
        if (!running_) {
            return false;
        }
        for (const auto& s : spec_.states) {
            if (s.id == current_) {
                return s.terminal;
            }
        }
        return false;
    }

    double time_in_state() const override { return time_in_state_; }

    std::vector<std::string> drain_actions() override {
        std::vector<std::string> out = std::move(actions_);
        actions_.clear();
        return out;
    }

    std::string serialize_state() const override {
        std::ostringstream out;
        out << std::setprecision(9);
        out << "{\"current\":\"" << json_escape(current_)
            << "\",\"time_in_state\":" << time_in_state_ << "}";
        return out.str();
    }

    bool deserialize_state(const std::string& json, std::string& errorOut) override {
        sdk::JsonValue doc;
        if (!sdk::json_parse(json, doc, errorOut)) {
            return false;
        }
        if (!doc.is_object()) {
            errorOut = "fsm state must be an object";
            return false;
        }
        std::string current;
        if (!string_field(doc, "current", current, true, errorOut)) {
            return false;
        }
        bool found = false;
        for (const auto& s : spec_.states) {
            if (s.id == current) {
                found = true;
                break;
            }
        }
        if (!found) {
            errorOut = "unknown state \"" + current + "\"";
            return false;
        }
        double time_in_state = 0.0;
        const sdk::JsonValue* t = doc.field("time_in_state");
        if (t != nullptr) {
            if (t->kind != sdk::JsonValue::Kind::Number || !finite(t->number) ||
                t->number < 0.0) {
                errorOut = "time_in_state must be finite and >= 0";
                return false;
            }
            time_in_state = t->number;
        }
        current_ = current;
        time_in_state_ = time_in_state;
        running_ = true;
        actions_.clear();
        errorOut.clear();
        return true;
    }

private:
    void do_transition(const FsmTransition& t) {
        // exit do estado velho → enter do novo (nessa ordem).
        for (const auto& s : spec_.states) {
            if (s.id == current_ && !s.exit.empty()) {
                actions_.push_back(s.exit);
                break;
            }
        }
        current_ = t.to;
        time_in_state_ = 0.0;
        for (const auto& s : spec_.states) {
            if (s.id == current_ && !s.enter.empty()) {
                actions_.push_back(s.enter);
                break;
            }
        }
    }

    FsmSpec spec_;
    std::string current_;
    double time_in_state_ = 0.0;
    bool running_ = false;
    std::vector<std::string> actions_;
    std::map<std::string, bool> conditions_;
};

}  // namespace

std::unique_ptr<IFsm> create_fsm() {
    return std::make_unique<Fsm>();
}

}  // namespace ai
}  // namespace engine
