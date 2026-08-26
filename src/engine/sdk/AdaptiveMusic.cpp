#include "engine/audio/IAdaptiveMusic.hpp"

#include "RegistryJson.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>

namespace engine {
namespace audio {
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

bool gain_field(const sdk::JsonValue& obj, const char* key, double& out,
                bool required, std::string& errorOut) {
    if (!number_field(obj, key, out, required, errorOut)) {
        return false;
    }
    if (out < 0.0 || out > 1.0) {
        errorOut = std::string(key) + " must be in [0,1]";
        return false;
    }
    return true;
}

bool layer_gains_field(const sdk::JsonValue& obj, const char* key,
                       std::vector<std::pair<std::string, double>>& out,
                       std::string& errorOut) {
    const sdk::JsonValue* f = obj.field(key);
    if (f == nullptr) {
        return true;
    }
    if (!f->is_array()) {
        errorOut = std::string(key) + " must be an array";
        return false;
    }
    out.clear();
    for (const auto& item : f->array) {
        if (!item.is_object()) {
            errorOut = std::string(key) + " entries must be objects";
            return false;
        }
        std::string layer;
        double gain = 0.0;
        if (!string_field(item, "layer", layer, true, errorOut)) return false;
        if (!gain_field(item, "gain", gain, false, errorOut)) return false;
        out.emplace_back(layer, gain);
    }
    return true;
}

}  // namespace

bool AdaptiveMusicSpec::validate(std::string& errorOut) const {
    std::set<std::string> layer_ids;
    for (const auto& l : layers) {
        if (l.id.empty()) {
            errorOut = "layer id must be non-empty";
            return false;
        }
        if (layer_ids.count(l.id)) {
            errorOut = "duplicate layer id \"" + l.id + "\"";
            return false;
        }
        layer_ids.insert(l.id);
    }
    std::set<std::string> state_ids;
    for (const auto& s : states) {
        if (s.id.empty()) {
            errorOut = "state id must be non-empty";
            return false;
        }
        if (state_ids.count(s.id)) {
            errorOut = "duplicate state id \"" + s.id + "\"";
            return false;
        }
        state_ids.insert(s.id);
        if (!finite(s.transition_s) || s.transition_s < 0.0) {
            errorOut = "state transition_s must be finite and >= 0";
            return false;
        }
        std::set<std::string> seen;
        for (const auto& lg : s.layer_gains) {
            if (!layer_ids.count(lg.first)) {
                errorOut = "state \"" + s.id +
                           "\" references unknown layer \"" + lg.first + "\"";
                return false;
            }
            if (seen.count(lg.first)) {
                errorOut = "state \"" + s.id + "\" duplicates layer \"" + lg.first +
                           "\"";
                return false;
            }
            seen.insert(lg.first);
            if (!finite(lg.second) || lg.second < 0.0 || lg.second > 1.0) {
                errorOut = "state gain must be finite and in [0,1]";
                return false;
            }
        }
    }
    std::set<std::string> stinger_ids;
    for (const auto& st : stingers) {
        if (st.id.empty()) {
            errorOut = "stinger id must be non-empty";
            return false;
        }
        if (stinger_ids.count(st.id)) {
            errorOut = "duplicate stinger id \"" + st.id + "\"";
            return false;
        }
        stinger_ids.insert(st.id);
        if (!st.layer.empty() && !layer_ids.count(st.layer)) {
            errorOut = "stinger \"" + st.id + "\" references unknown layer \"" +
                       st.layer + "\"";
            return false;
        }
        if (!finite(st.intensity) || st.intensity < 0.0 || st.intensity > 1.0) {
            errorOut = "stinger intensity must be finite and in [0,1]";
            return false;
        }
    }
    errorOut.clear();
    return true;
}

bool AdaptiveMusicSpec::load_from_json(const std::string& json,
                                       std::string& errorOut) {
    sdk::JsonValue doc;
    if (!sdk::json_parse(json, doc, errorOut)) {
        return false;
    }
    if (!doc.is_object()) {
        errorOut = "adaptive music spec must be an object";
        return false;
    }
    const sdk::JsonValue* version = doc.field("version");
    if (version != nullptr &&
        (!is_uint64(*version) || static_cast<int>(version->number) != 1)) {
        errorOut = "unsupported adaptive music spec version";
        return false;
    }
    AdaptiveMusicSpec candidate;
    const sdk::JsonValue* layersField = doc.field("layers");
    if (layersField != nullptr) {
        if (!layersField->is_array()) {
            errorOut = "layers must be an array";
            return false;
        }
        for (const auto& item : layersField->array) {
            if (!item.is_object()) {
                errorOut = "layer entries must be objects";
                return false;
            }
            MusicLayer l;
            if (!string_field(item, "id", l.id, true, errorOut)) return false;
            candidate.layers.push_back(l);
        }
    }
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
            MusicState s;
            if (!string_field(item, "id", s.id, true, errorOut)) return false;
            if (!number_field(item, "transition_s", s.transition_s, false,
                              errorOut))
                return false;
            if (!layer_gains_field(item, "layer_gains", s.layer_gains, errorOut))
                return false;
            candidate.states.push_back(s);
        }
    }
    const sdk::JsonValue* stingersField = doc.field("stingers");
    if (stingersField != nullptr) {
        if (!stingersField->is_array()) {
            errorOut = "stingers must be an array";
            return false;
        }
        for (const auto& item : stingersField->array) {
            if (!item.is_object()) {
                errorOut = "stinger entries must be objects";
                return false;
            }
            MusicStinger st;
            if (!string_field(item, "id", st.id, true, errorOut)) return false;
            if (!string_field(item, "layer", st.layer, false, errorOut))
                return false;
            if (!gain_field(item, "intensity", st.intensity, false, errorOut))
                return false;
            candidate.stingers.push_back(st);
        }
    }
    if (!candidate.validate(errorOut)) {
        return false;
    }
    *this = std::move(candidate);
    return true;
}

std::string AdaptiveMusicSpec::to_json() const {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "{\"version\":1,\"layers\":[";
    for (std::size_t i = 0; i < layers.size(); ++i) {
        if (i) out << ",";
        out << "{\"id\":\"" << json_escape(layers[i].id) << "\"}";
    }
    out << "],\"states\":[";
    for (std::size_t i = 0; i < states.size(); ++i) {
        if (i) out << ",";
        const auto& s = states[i];
        out << "{\"id\":\"" << json_escape(s.id) << "\",\"transition_s\":"
            << s.transition_s << ",\"layer_gains\":[";
        for (std::size_t j = 0; j < s.layer_gains.size(); ++j) {
            if (j) out << ",";
            out << "{\"layer\":\"" << json_escape(s.layer_gains[j].first)
                << "\",\"gain\":" << s.layer_gains[j].second << "}";
        }
        out << "]}";
    }
    out << "],\"stingers\":[";
    for (std::size_t i = 0; i < stingers.size(); ++i) {
        if (i) out << ",";
        const auto& st = stingers[i];
        out << "{\"id\":\"" << json_escape(st.id) << "\",\"layer\":\""
            << json_escape(st.layer) << "\",\"intensity\":" << st.intensity
            << "}";
    }
    out << "]}";
    return out.str();
}

namespace {

class AdaptiveMusic final : public IAdaptiveMusic {
public:
    bool configure(const AdaptiveMusicSpec& spec, std::string& errorOut) override {
        if (!spec.validate(errorOut)) {
            return false;
        }
        spec_ = spec;
        current_state_.clear();
        pending_state_.clear();
        fade_progress_ = 0.0;
        intensity_ = 1.0;
        gains_.clear();
        for (const auto& l : spec_.layers) {
            gains_[l.id] = 0.0;
        }
        events_.clear();
        errorOut.clear();
        return true;
    }

    bool set_state(const std::string& id, std::string& errorOut) override {
        const MusicState* target = find_state(id);
        if (target == nullptr) {
            errorOut = "unknown state \"" + id + "\"";
            return false;
        }
        if (id == current_state_ && pending_state_.empty()) {
            errorOut.clear();
            return true;  // no-op: já é o estado atual
        }
        const std::map<std::string, double> target_gains = gains_of(*target);
        if (target->transition_s <= 0.0) {
            // salto instantâneo
            gains_ = target_gains;
            current_state_ = id;
            pending_state_.clear();
            fade_progress_ = 0.0;
            events_.push_back(
                MusicEvent{MusicEventKind::StateChanged, id, "", 0.0});
        } else {
            // crossfade linear a partir dos ganhos atuais
            fade_from_ = gains_;
            fade_to_ = target_gains;
            fade_progress_ = 0.0;
            pending_state_ = id;
        }
        errorOut.clear();
        return true;
    }

    std::string current_state() const override {
        return current_state_;
    }

    bool set_intensity(double intensity, std::string& errorOut) override {
        if (!finite(intensity) || intensity < 0.0 || intensity > 1.0) {
            errorOut = "intensity must be finite and in [0,1]";
            return false;
        }
        intensity_ = intensity;
        errorOut.clear();
        return true;
    }

    bool trigger_stinger(const std::string& id, std::string& errorOut) override {
        for (const auto& st : spec_.stingers) {
            if (st.id == id) {
                events_.push_back(MusicEvent{MusicEventKind::StingerTriggered, id,
                                             st.layer, st.intensity});
                errorOut.clear();
                return true;
            }
        }
        errorOut = "unknown stinger \"" + id + "\"";
        return false;
    }

    bool tick(double dt, std::string& errorOut) override {
        if (!finite(dt) || dt < 0.0) {
            errorOut = "dt must be finite and >= 0";
            return false;
        }
        if (!pending_state_.empty()) {
            const MusicState* target = find_state(pending_state_);
            if (target != nullptr && target->transition_s > 0.0) {
                fade_progress_ += dt / target->transition_s;
                if (fade_progress_ >= 1.0) {
                    fade_progress_ = 1.0;
                    gains_ = fade_to_;
                    current_state_ = pending_state_;
                    pending_state_.clear();
                    events_.push_back(MusicEvent{MusicEventKind::StateChanged,
                                                 current_state_, "", 0.0});
                }
            }
        }
        errorOut.clear();
        return true;
    }

    double layer_gain(const std::string& layer) const override {
        if (!pending_state_.empty()) {
            const double from = fade_from_.count(layer) ? fade_from_.at(layer) : 0.0;
            const double to = fade_to_.count(layer) ? fade_to_.at(layer) : 0.0;
            return (from + (to - from) * fade_progress_) * intensity_;
        }
        const auto it = gains_.find(layer);
        return it == gains_.end() ? 0.0 : it->second * intensity_;
    }

    std::vector<MusicEvent> drain_events() override {
        std::vector<MusicEvent> out = std::move(events_);
        events_.clear();
        return out;
    }

    std::string serialize_state() const override {
        std::ostringstream out;
        out << std::setprecision(9);
        out << "{\"current_state\":\"" << json_escape(current_state_)
            << "\",\"intensity\":" << intensity_ << ",\"gains\":{";
        bool first = true;
        for (const auto& kv : gains_) {
            if (!first) out << ",";
            first = false;
            out << "\"" << json_escape(kv.first) << "\":" << kv.second;
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
            errorOut = "adaptive music state must be an object";
            return false;
        }
        const sdk::JsonValue* stateField = doc.field("current_state");
        if (stateField == nullptr || stateField->kind != sdk::JsonValue::Kind::String) {
            errorOut = "state must contain a current_state string";
            return false;
        }
        if (stateField->string.empty() || find_state(stateField->string) == nullptr) {
            errorOut = "current_state must reference a known state";
            return false;
        }
        double intensity = 1.0;
        if (!number_field(doc, "intensity", intensity, false, errorOut))
            return false;
        if (!finite(intensity) || intensity < 0.0 || intensity > 1.0) {
            errorOut = "intensity must be in [0,1]";
            return false;
        }
        const sdk::JsonValue* gainsField = doc.field("gains");
        if (gainsField == nullptr || !gainsField->is_object()) {
            errorOut = "state must contain a gains object";
            return false;
        }
        // all-or-nothing: valida antes de mutar.
        std::map<std::string, double> ngains;
        for (const auto& kv : gainsField->object) {
            if (kv.second.kind != sdk::JsonValue::Kind::Number ||
                !finite(kv.second.number) || kv.second.number < 0.0 ||
                kv.second.number > 1.0) {
                errorOut = "gain values must be finite numbers in [0,1]";
                return false;
            }
            if (!gains_.count(kv.first)) {
                errorOut = "gains references unknown layer \"" + kv.first + "\"";
                return false;
            }
            ngains[kv.first] = kv.second.number;
        }
        current_state_ = stateField->string;
        intensity_ = intensity;
        gains_ = ngains;
        pending_state_.clear();
        fade_progress_ = 0.0;
        events_.clear();
        errorOut.clear();
        return true;
    }

private:
    const MusicState* find_state(const std::string& id) const {
        for (const auto& s : spec_.states) {
            if (s.id == id) {
                return &s;
            }
        }
        return nullptr;
    }

    std::map<std::string, double> gains_of(const MusicState& state) const {
        std::map<std::string, double> out;
        for (const auto& l : spec_.layers) {
            out[l.id] = 0.0;
        }
        for (const auto& lg : state.layer_gains) {
            out[lg.first] = lg.second;
        }
        return out;
    }

    AdaptiveMusicSpec spec_;
    std::string current_state_;
    std::string pending_state_;
    double fade_progress_ = 0.0;
    double intensity_ = 1.0;
    std::map<std::string, double> gains_;
    std::map<std::string, double> fade_from_;
    std::map<std::string, double> fade_to_;
    std::vector<MusicEvent> events_;
};

}  // namespace

std::unique_ptr<IAdaptiveMusic> create_adaptive_music() {
    return std::make_unique<AdaptiveMusic>();
}

}  // namespace audio
}  // namespace engine
