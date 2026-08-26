#include "engine/animation/IAnimStateMachine.hpp"

#include "RegistryJson.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>

namespace engine {
namespace animation {
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
                bool required, std::string& errorOut) {
    const sdk::JsonValue* f = obj.field(key);
    if (f == nullptr) {
        if (required) {
            errorOut = std::string("missing field ") + key;
            return false;
        }
        return true;
    }
    if (f->kind != sdk::JsonValue::Kind::Bool) {
        errorOut = std::string(key) + " must be a bool";
        return false;
    }
    out = f->boolean;
    return true;
}

}  // namespace

bool AnimStateMachineSpec::validate(std::string& errorOut) const {
    if (id.empty()) {
        errorOut = "state machine id must be non-empty";
        return false;
    }
    if (states.empty()) {
        errorOut = "state machine must have at least one state";
        return false;
    }
    std::set<std::string> ids;
    for (const auto& s : states) {
        if (s.id.empty()) {
            errorOut = "state id must be non-empty";
            return false;
        }
        if (ids.count(s.id)) {
            errorOut = "duplicate state id \"" + s.id + "\"";
            return false;
        }
        ids.insert(s.id);
        if (s.clip.empty()) {
            errorOut = "state \"" + s.id + "\" must reference a clip";
            return false;
        }
        if (!finite(s.speed) || s.speed < 0.0) {
            errorOut = "state speed must be finite and >= 0";
            return false;
        }
    }
    if (!ids.count(initial)) {
        errorOut = "initial state unknown: \"" + initial + "\"";
        return false;
    }
    for (const auto& t : transitions) {
        if (!ids.count(t.from) || !ids.count(t.to)) {
            errorOut = "transition references unknown state";
            return false;
        }
        int triggers = 0;
        if (!t.on_event.empty()) ++triggers;
        if (!t.on_condition.empty()) ++triggers;
        if (t.after_seconds > 0.0) ++triggers;
        if (triggers != 1) {
            errorOut = "transition must have exactly one trigger (event/condition/timer)";
            return false;
        }
        if (!finite(t.after_seconds) || t.after_seconds < 0.0) {
            errorOut = "transition after_seconds must be finite and >= 0";
            return false;
        }
        if (!finite(t.blend_s) || t.blend_s < 0.0) {
            errorOut = "transition blend_s must be finite and >= 0";
            return false;
        }
    }
    errorOut.clear();
    return true;
}

bool AnimStateMachineSpec::load_from_json(const std::string& json,
                                          std::string& errorOut) {
    sdk::JsonValue doc;
    if (!sdk::json_parse(json, doc, errorOut)) {
        return false;
    }
    if (!doc.is_object()) {
        errorOut = "state machine spec must be an object";
        return false;
    }
    const sdk::JsonValue* version = doc.field("version");
    if (version != nullptr &&
        (!is_uint64(*version) || static_cast<int>(version->number) != 1)) {
        errorOut = "unsupported state machine spec version";
        return false;
    }
    AnimStateMachineSpec candidate;
    if (!string_field(doc, "id", candidate.id, true, errorOut)) return false;
    if (!string_field(doc, "initial", candidate.initial, true, errorOut))
        return false;
    const sdk::JsonValue* statesField = doc.field("states");
    if (statesField != nullptr) {
        if (!statesField->is_array()) {
            errorOut = "states must be an array";
            return false;
        }
        for (const auto& item : statesField->array) {
            if (!item.is_object()) {
                errorOut = "state entries must be objects";
                return false;
            }
            AnimState s;
            if (!string_field(item, "id", s.id, true, errorOut)) return false;
            if (!string_field(item, "clip", s.clip, true, errorOut)) return false;
            if (!number_field(item, "speed", s.speed, false, errorOut)) return false;
            if (!bool_field(item, "loop", s.loop, false, errorOut)) return false;
            candidate.states.push_back(s);
        }
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
            AnimTransition t;
            if (!string_field(item, "from", t.from, true, errorOut)) return false;
            if (!string_field(item, "to", t.to, true, errorOut)) return false;
            if (!string_field(item, "on_event", t.on_event, false, errorOut))
                return false;
            if (!string_field(item, "on_condition", t.on_condition, false, errorOut))
                return false;
            if (!number_field(item, "after_seconds", t.after_seconds, false,
                              errorOut))
                return false;
            if (!number_field(item, "blend_s", t.blend_s, false, errorOut))
                return false;
            candidate.transitions.push_back(t);
        }
    }
    if (!candidate.validate(errorOut)) {
        return false;
    }
    *this = std::move(candidate);
    return true;
}

std::string AnimStateMachineSpec::to_json() const {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "{\"version\":1,\"id\":\"" << json_escape(id) << "\",\"initial\":\""
        << json_escape(initial) << "\",\"states\":[";
    for (std::size_t i = 0; i < states.size(); ++i) {
        if (i) out << ",";
        const auto& s = states[i];
        out << "{\"id\":\"" << json_escape(s.id) << "\",\"clip\":\""
            << json_escape(s.clip) << "\",\"speed\":" << s.speed
            << ",\"loop\":" << (s.loop ? "true" : "false") << "}";
    }
    out << "],\"transitions\":[";
    for (std::size_t i = 0; i < transitions.size(); ++i) {
        if (i) out << ",";
        const auto& t = transitions[i];
        out << "{\"from\":\"" << json_escape(t.from) << "\",\"to\":\""
            << json_escape(t.to) << "\",\"on_event\":\"" << json_escape(t.on_event)
            << "\",\"on_condition\":\"" << json_escape(t.on_condition)
            << "\",\"after_seconds\":" << t.after_seconds << ",\"blend_s\":"
            << t.blend_s << "}";
    }
    out << "]}";
    return out.str();
}

namespace {

const AnimState* find_state(const AnimStateMachineSpec& spec, const std::string& id) {
    for (const auto& s : spec.states) {
        if (s.id == id) {
            return &s;
        }
    }
    return nullptr;
}

// Estados com transição de saída (não terminais).
std::set<std::string> outgoing(const AnimStateMachineSpec& spec) {
    std::set<std::string> out;
    for (const auto& t : spec.transitions) {
        out.insert(t.from);
    }
    return out;
}

class AnimStateMachine final : public IAnimStateMachine {
public:
    bool configure(const AnimStateMachineSpec& spec, std::string& errorOut) override {
        if (!spec.validate(errorOut)) {
            return false;
        }
        spec_ = spec;
        current_.clear();
        time_ = 0.0;
        conditions_.clear();
        started_ = false;
        errorOut.clear();
        return true;
    }

    bool start(std::string& errorOut) override {
        if (spec_.states.empty()) {
            errorOut = "state machine not configured";
            return false;
        }
        current_ = spec_.initial;
        time_ = 0.0;
        started_ = true;
        errorOut.clear();
        return true;
    }

    std::string state() const override { return current_; }
    std::string clip() const override {
        const AnimState* s = find_state(spec_, current_);
        return s != nullptr ? s->clip : "";
    }
    double time_in_state() const override { return time_; }
    double state_speed() const override {
        const AnimState* s = find_state(spec_, current_);
        return s != nullptr ? s->speed : 1.0;
    }
    bool is_looping() const override {
        const AnimState* s = find_state(spec_, current_);
        return s != nullptr && s->loop;
    }

    bool send_event(const std::string& event, std::string& errorOut) override {
        if (!started_) {
            errorOut = "state machine not started";
            return false;
        }
        for (const auto& t : spec_.transitions) {
            if (t.from == current_ && t.on_event == event) {
                current_ = t.to;
                time_ = 0.0;
                break;
            }
        }
        errorOut.clear();
        return true;
    }

    bool set_condition(const std::string& name, bool value,
                       std::string& errorOut) override {
        if (name.empty()) {
            errorOut = "condition name must be non-empty";
            return false;
        }
        conditions_[name] = value;
        errorOut.clear();
        return true;
    }

    bool tick(double dt, std::string& errorOut) override {
        if (!started_) {
            errorOut = "state machine not started";
            return false;
        }
        if (!finite(dt) || dt < 0.0) {
            errorOut = "dt must be finite and >= 0";
            return false;
        }
        const AnimState* s = find_state(spec_, current_);
        const double speed = s != nullptr ? s->speed : 1.0;
        time_ += dt * speed;
        for (const auto& t : spec_.transitions) {
            if (t.from != current_) {
                continue;
            }
            bool fire = false;
            if (t.after_seconds > 0.0) {
                fire = time_ >= t.after_seconds;
            } else if (!t.on_condition.empty()) {
                const auto it = conditions_.find(t.on_condition);
                fire = it != conditions_.end() && it->second;
            }
            if (fire) {
                current_ = t.to;
                time_ = 0.0;
                break;
            }
        }
        errorOut.clear();
        return true;
    }

    bool done() const override {
        const std::set<std::string> out = outgoing(spec_);
        return !current_.empty() && !out.count(current_);
    }

    std::string serialize_state() const override {
        std::ostringstream out;
        out << std::setprecision(9);
        out << "{\"state\":\"" << json_escape(current_) << "\",\"time\":" << time_
            << ",\"started\":" << (started_ ? "true" : "false")
            << ",\"conditions\":{";
        bool first = true;
        for (const auto& kv : conditions_) {
            if (!first) out << ",";
            first = false;
            out << "\"" << json_escape(kv.first) << "\":"
                << (kv.second ? "true" : "false");
        }
        out << "}}";
        return out.str();
    }

    bool deserialize_state(const std::string& json,
                           std::string& errorOut) override {
        sdk::JsonValue doc;
        if (!sdk::json_parse(json, doc, errorOut)) {
            return false;
        }
        if (!doc.is_object()) {
            errorOut = "state machine state must be an object";
            return false;
        }
        const sdk::JsonValue* stateField = doc.field("state");
        if (stateField == nullptr || stateField->kind != sdk::JsonValue::Kind::String) {
            errorOut = "state must contain a state string";
            return false;
        }
        if (find_state(spec_, stateField->string) == nullptr) {
            errorOut = "state references unknown state \"" + stateField->string + "\"";
            return false;
        }
        double time = 0.0;
        if (!number_field(doc, "time", time, false, errorOut)) return false;
        if (!finite(time) || time < 0.0) {
            errorOut = "time must be finite and >= 0";
            return false;
        }
        bool started = true;
        if (!bool_field(doc, "started", started, false, errorOut)) return false;
        std::map<std::string, bool> nconditions;
        const sdk::JsonValue* condField = doc.field("conditions");
        if (condField != nullptr) {
            if (!condField->is_object()) {
                errorOut = "conditions must be an object";
                return false;
            }
            for (const auto& kv : condField->object) {
                if (kv.second.kind != sdk::JsonValue::Kind::Bool) {
                    errorOut = "condition values must be booleans";
                    return false;
                }
                nconditions[kv.first] = kv.second.boolean;
            }
        }
        current_ = stateField->string;
        time_ = time;
        started_ = started;
        conditions_ = nconditions;
        errorOut.clear();
        return true;
    }

private:
    AnimStateMachineSpec spec_;
    std::string current_;
    double time_ = 0.0;
    bool started_ = false;
    std::map<std::string, bool> conditions_;
};

}  // namespace

std::unique_ptr<IAnimStateMachine> create_anim_state_machine() {
    return std::make_unique<AnimStateMachine>();
}

}  // namespace animation
}  // namespace engine
