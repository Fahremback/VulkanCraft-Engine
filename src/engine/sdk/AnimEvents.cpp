// AnimEvents.cpp — adapter único de IAnimEvents (engine::animation).
// Armazena eventos por clip; ordenação canônica (tempo, inserção) na query;
// polling meio-aberto (t0, t1]; serialização JSON all-or-nothing bit-exact
// (convenção do projeto: setprecision(9), helpers locais de campo).

#include "engine/animation/IAnimEvents.hpp"

#include "engine/sdk/RegistryJson.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <map>
#include <sstream>
#include <utility>

namespace engine::animation {
namespace {

std::string json_escape(const std::string& text) {
    std::ostringstream out;
    for (char c : text) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out << "\\u" << std::hex << std::setw(4)
                        << std::setfill('0')
                        << static_cast<int>(static_cast<unsigned char>(c))
                        << std::dec;
                } else {
                    out << c;
                }
        }
    }
    return out.str();
}

bool string_field(const sdk::JsonValue& obj, const char* key, std::string& out,
                  bool required, std::string& errorOut) {
    const sdk::JsonValue* f = obj.field(key);
    if (f == nullptr) {
        if (required) {
            errorOut = std::string("missing required field \"") + key + "\"";
            return false;
        }
        out.clear();
        return true;
    }
    if (f->kind != sdk::JsonValue::Kind::String) {
        errorOut = std::string("field \"") + key + "\" must be a string";
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
            errorOut = std::string("missing required field \"") + key + "\"";
            return false;
        }
        out = 0.0;
        return true;
    }
    if (f->kind != sdk::JsonValue::Kind::Number) {
        errorOut = std::string("field \"") + key + "\" must be a number";
        return false;
    }
    out = f->number;
    return true;
}

class AnimEvents final : public IAnimEvents {
public:
    explicit AnimEvents(IAnimCore& core) : core_(core) {}

    bool add_event(const AnimEvent& ev, std::string& errorOut) override {
        if (ev.name.empty()) {
            errorOut = "event name must not be empty";
            return false;
        }
        if (!std::isfinite(ev.time) || ev.time < 0.0) {
            errorOut = "event time must be finite and >= 0";
            return false;
        }
        std::string clipErr;
        const double dur = core_.clip_duration(ev.clip, clipErr);
        if (!clipErr.empty()) {
            errorOut = clipErr;
            return false;
        }
        if (ev.time > dur) {
            errorOut = "event time exceeds clip duration " +
                       std::to_string(dur);
            return false;
        }
        auto& vec = events_[ev.clip];
        for (const AnimEvent& e : vec) {
            if (e.time == ev.time && e.name == ev.name) {
                errorOut = "duplicate event \"" + ev.name + "\" at time " +
                           std::to_string(ev.time);
                return false;
            }
        }
        vec.push_back(ev);
        errorOut.clear();
        return true;
    }

    bool remove_event(const std::string& clip, double time,
                      const std::string& name) override {
        auto it = events_.find(clip);
        if (it == events_.end()) return false;
        auto& vec = it->second;
        for (std::size_t i = 0; i < vec.size(); ++i) {
            if (vec[i].time == time && vec[i].name == name) {
                vec.erase(vec.begin() + static_cast<std::ptrdiff_t>(i));
                return true;
            }
        }
        return false;
    }

    std::vector<AnimEvent> events_for(const std::string& clip,
                                      std::string& errorOut) const override {
        const auto it = events_.find(clip);
        if (it == events_.end()) {
            if (!core_.has_clip(clip)) {
                errorOut = "unknown clip \"" + clip + "\"";
                return {};
            }
            return {};
        }
        std::vector<AnimEvent> out = it->second;
        std::stable_sort(out.begin(), out.end(),
                         [](const AnimEvent& a, const AnimEvent& b) {
                             return a.time < b.time;
                         });
        errorOut.clear();
        return out;
    }

    std::vector<AnimEvent> poll(const std::string& clip, double t0, double t1,
                                std::string& errorOut) const override {
        if (!std::isfinite(t0) || !std::isfinite(t1)) {
            errorOut = "poll times must be finite";
            return {};
        }
        const auto it = events_.find(clip);
        if (it == events_.end()) {
            if (!core_.has_clip(clip)) {
                errorOut = "unknown clip \"" + clip + "\"";
                return {};
            }
            return {};
        }
        std::vector<AnimEvent> out;
        for (const AnimEvent& e : it->second) {
            if (e.time > t0 && e.time <= t1) {
                out.push_back(e);
            }
        }
        std::stable_sort(out.begin(), out.end(),
                         [](const AnimEvent& a, const AnimEvent& b) {
                             return a.time < b.time;
                         });
        errorOut.clear();
        return out;
    }

    std::string serialize_state() const override {
        std::ostringstream out;
        out << std::setprecision(9);
        out << "[";
        bool first = true;
        for (const auto& kv : events_) {
            for (const AnimEvent& e : kv.second) {
                if (!first) out << ",";
                first = false;
                out << "{\"clip\":\"" << json_escape(kv.first)
                    << "\",\"time\":" << e.time
                    << ",\"name\":\"" << json_escape(e.name) << "\"}";
            }
        }
        out << "]";
        return out.str();
    }

    bool deserialize_state(const std::string& json,
                           std::string& errorOut) override {
        sdk::JsonValue doc;
        if (!sdk::json_parse(json, doc, errorOut)) return false;
        if (!doc.is_array()) {
            errorOut = "anim events state must be a JSON array";
            return false;
        }
        std::map<std::string, std::vector<AnimEvent>> parsed;
        for (const sdk::JsonValue& item : doc.array) {
            if (!item.is_object()) {
                errorOut = "event entry must be an object";
                return false;
            }
            AnimEvent ev;
            if (!string_field(item, "clip", ev.clip, true, errorOut)) return false;
            if (!number_field(item, "time", ev.time, true, errorOut)) return false;
            if (!string_field(item, "name", ev.name, true, errorOut)) return false;
            if (!std::isfinite(ev.time) || ev.time < 0.0) {
                errorOut = "event time must be finite and >= 0";
                return false;
            }
            parsed[ev.clip].push_back(ev);
        }
        // Validação: clips existem, tempos dentro da duração, sem duplicatas.
        for (const auto& kv : parsed) {
            std::string clipErr;
            const double dur = core_.clip_duration(kv.first, clipErr);
            if (!clipErr.empty()) {
                errorOut = clipErr;
                return false;
            }
            const auto& vec = kv.second;
            for (const AnimEvent& e : vec) {
                if (e.time > dur) {
                    errorOut = "event time exceeds clip duration";
                    return false;
                }
            }
            for (std::size_t i = 0; i < vec.size(); ++i) {
                for (std::size_t j = i + 1; j < vec.size(); ++j) {
                    if (vec[i].time == vec[j].time &&
                        vec[i].name == vec[j].name) {
                        errorOut = "duplicate event in state";
                        return false;
                    }
                }
            }
        }
        events_ = std::move(parsed);
        errorOut.clear();
        return true;
    }

private:
    IAnimCore& core_;
    std::map<std::string, std::vector<AnimEvent>> events_;
};

}  // namespace

std::unique_ptr<IAnimEvents> create_anim_events(IAnimCore& core) {
    return std::unique_ptr<IAnimEvents>(new AnimEvents(core));
}

}  // namespace engine::animation
