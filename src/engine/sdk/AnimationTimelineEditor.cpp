// AnimationTimelineEditor.cpp — the ONLY TU with the animation-timeline
// editor behavior (agente 2 §B l.33). Pure document model: duration +
// playhead + loop + named tracks with ordered keyframes. All mutations are
// all-or-nothing (refused with a reason, document untouched). No
// clocks/RNG/globals. Deterministic JSON (%.6g floats).

#include "engine/editor/IAnimationTimelineEditor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace engine {
namespace editor {

namespace {

bool nearly_equal(float a, float b) {
    return std::fabs(a - b) < 1e-6f;
}

std::string fmt(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    return buf;
}

std::string json_escape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

const char* kind_name(TimelineTrackKind kind) {
    switch (kind) {
        case TimelineTrackKind::Animation: return "animation";
        case TimelineTrackKind::Audio: return "audio";
        case TimelineTrackKind::Event: return "event";
        case TimelineTrackKind::Camera: return "camera";
        case TimelineTrackKind::Property: return "property";
    }
    return "animation";
}

class AnimationTimelineEditorImpl final : public IAnimationTimelineEditor {
public:
    AnimationTimelineEditorImpl() = default;

    AnimationTimelineSnapshot snapshot() const override {
        return { duration_, playhead_, loop_, tracks_ };
    }

    bool reset(float duration, bool loop, std::string& errorOut) override {
        errorOut.clear();
        if (!(duration > 0.0f) || !std::isfinite(duration)) {
            errorOut = "timeline duration must be positive and finite";
            return false;
        }
        duration_ = duration;
        loop_ = loop;
        playhead_ = 0.0f;
        tracks_.clear();
        return true;
    }

    bool add_track(const std::string& name, TimelineTrackKind kind,
                   std::string& errorOut) override {
        errorOut.clear();
        if (name.empty()) {
            errorOut = "track name must not be empty";
            return false;
        }
        for (const auto& t : tracks_) {
            if (t.name == name) {
                errorOut = "track already exists: " + name;
                return false;
            }
        }
        TimelineTrackDef t;
        t.name = name;
        t.kind = kind;
        tracks_.push_back(std::move(t));
        return true;
    }

    bool remove_track(const std::string& name, std::string& errorOut) override {
        errorOut.clear();
        const auto it = std::find_if(tracks_.begin(), tracks_.end(),
                                     [&](const TimelineTrackDef& t) {
                                         return t.name == name;
                                     });
        if (it == tracks_.end()) {
            errorOut = "track does not exist: " + name;
            return false;
        }
        tracks_.erase(it);
        return true;
    }

    bool set_muted(const std::string& name, bool muted,
                   std::string& errorOut) override {
        errorOut.clear();
        for (auto& t : tracks_) {
            if (t.name == name) {
                t.muted = muted;
                return true;
            }
        }
        errorOut = "track does not exist: " + name;
        return false;
    }

    bool add_key(const std::string& track, float time, const std::string& value,
                 std::string& errorOut) override {
        errorOut.clear();
        if (!std::isfinite(time) || time < 0.0f || time > duration_) {
            errorOut = "key time outside [0, duration]: " + fmt(time);
            return false;
        }
        for (auto& t : tracks_) {
            if (t.name != track) continue;
            for (const auto& k : t.keys) {
                if (nearly_equal(k.time, time)) {
                    errorOut = "key already exists at time " + fmt(time);
                    return false;
                }
            }
            TimelineKeyDef k;
            k.time = time;
            k.value = value;
            t.keys.push_back(std::move(k));
            std::stable_sort(t.keys.begin(), t.keys.end(),
                             [](const TimelineKeyDef& a, const TimelineKeyDef& b) {
                                 return a.time < b.time;
                             });
            return true;
        }
        errorOut = "track does not exist: " + track;
        return false;
    }

    bool remove_key(const std::string& track, float time,
                    std::string& errorOut) override {
        errorOut.clear();
        for (auto& t : tracks_) {
            if (t.name != track) continue;
            const auto it = std::find_if(t.keys.begin(), t.keys.end(),
                                         [&](const TimelineKeyDef& k) {
                                             return nearly_equal(k.time, time);
                                         });
            if (it == t.keys.end()) {
                errorOut = "key does not exist at time " + fmt(time);
                return false;
            }
            t.keys.erase(it);
            return true;
        }
        errorOut = "track does not exist: " + track;
        return false;
    }

    void seek(float time) override {
        if (!std::isfinite(time)) return;
        playhead_ = std::max(0.0f, std::min(time, duration_));
    }

    std::vector<std::string> validate() const override {
        std::vector<std::string> issues;
        if (!(duration_ > 0.0f) || !std::isfinite(duration_)) {
            issues.push_back("duration must be positive");
        }
        for (const auto& t : tracks_) {
            for (const auto& k : t.keys) {
                if (k.time < 0.0f || k.time > duration_) {
                    issues.push_back("key outside timeline: " + t.name);
                }
            }
            for (const auto& other : tracks_) {
                if (&other != &t && other.name == t.name) {
                    issues.push_back("duplicate track name: " + t.name);
                    break;
                }
            }
        }
        return issues;
    }

    std::string to_json() const override {
        std::ostringstream out;
        out << "{\"duration\":" << fmt(duration_)
            << ",\"playhead\":" << fmt(playhead_)
            << ",\"loop\":" << (loop_ ? "true" : "false")
            << ",\"tracks\":[";
        for (size_t i = 0; i < tracks_.size(); ++i) {
            if (i) out << ",";
            const auto& t = tracks_[i];
            out << "{\"name\":\"" << json_escape(t.name)
                << "\",\"kind\":\"" << kind_name(t.kind)
                << "\",\"muted\":" << (t.muted ? "true" : "false")
                << ",\"keys\":[";
            for (size_t j = 0; j < t.keys.size(); ++j) {
                if (j) out << ",";
                const auto& k = t.keys[j];
                out << "{\"time\":" << fmt(k.time)
                    << ",\"value\":\"" << json_escape(k.value) << "\"}";
            }
            out << "]}";
        }
        out << "]}";
        return out.str();
    }

private:
    float duration_{ 1.0f };
    float playhead_{ 0.0f };
    bool loop_{ false };
    std::vector<TimelineTrackDef> tracks_;
};

}  // namespace

std::unique_ptr<IAnimationTimelineEditor> create_animation_timeline_editor() {
    return std::make_unique<AnimationTimelineEditorImpl>();
}

}  // namespace editor
}  // namespace engine
