#pragma once

// IAnimationTimelineEditor (agente 2 §B l.33): the PUBLIC, deterministic
// model of the animation-timeline editor. The visual TimelineEditorModel in
// the specialized-editors panel must never drift from the authored data: a
// timeline is a duration + playhead + loop flag + named tracks, each track
// holding ordered keyframes (time + opaque value string). The contract is a
// pure model over that document:
//   - UNEQUIVOCAL: add_track/add_key/seek/remove_track/mute validate every
//     input; invalid commands are REFUSED with a reason and leave the
//     document untouched (all-or-nothing).
//   - DETERMINISM: keys are always kept sorted by time (stable), no
//     clocks/RNG/globals; same sequence of commands -> identical state.
//   - OBSERVABLE: to_json() serializes {duration, playhead, loop, tracks,
//     keys} deterministically (editor exposes it via the Control API, e.g.
//     GET /timeline-editor).
//
// Self-contained (std only). The SDK adapter
// (src/engine/sdk/AnimationTimelineEditor.cpp) is the ONLY TU with behavior.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace editor {

// Track kinds mirror the play-world TimelineComponent (0..4).
enum class TimelineTrackKind : std::uint8_t {
    Animation = 0,
    Audio = 1,
    Event = 2,
    Camera = 3,
    Property = 4
};

struct TimelineKeyDef {
    float time{ 0.0f };
    std::string value;  // opaque; the runtime interprets it per track kind
    bool operator==(const TimelineKeyDef& other) const {
        return time == other.time && value == other.value;
    }
};

struct TimelineTrackDef {
    std::string name;
    TimelineTrackKind kind{ TimelineTrackKind::Animation };
    bool muted{ false };
    std::vector<TimelineKeyDef> keys;  // always sorted by time (stable)
    bool operator==(const TimelineTrackDef& other) const {
        return name == other.name && kind == other.kind &&
               muted == other.muted && keys == other.keys;
    }
};

struct AnimationTimelineSnapshot {
    float duration{ 1.0f };
    float playhead{ 0.0f };
    bool loop{ false };
    std::vector<TimelineTrackDef> tracks;

    bool operator==(const AnimationTimelineSnapshot& other) const {
        return duration == other.duration && playhead == other.playhead &&
               loop == other.loop && tracks == other.tracks;
    }
    bool operator!=(const AnimationTimelineSnapshot& other) const {
        return !(*this == other);
    }
};

class IAnimationTimelineEditor {
public:
    virtual ~IAnimationTimelineEditor() = default;

    virtual AnimationTimelineSnapshot snapshot() const = 0;

    // Resets the document (all-or-nothing: refuses non-positive duration).
    virtual bool reset(float duration, bool loop, std::string& errorOut) = 0;

    // Adds a track. REFUSED when the name is empty or a track with the same
    // name already exists.
    virtual bool add_track(const std::string& name, TimelineTrackKind kind,
                           std::string& errorOut) = 0;

    // Removes a track by name. REFUSED when it does not exist.
    virtual bool remove_track(const std::string& name,
                              std::string& errorOut) = 0;

    // Toggles mute. REFUSED when the track does not exist.
    virtual bool set_muted(const std::string& name, bool muted,
                           std::string& errorOut) = 0;

    // Adds a keyframe to a track. REFUSED when the track does not exist, the
    // time is negative or beyond the duration, or a key with the exact same
    // time already exists. Keys stay sorted by time.
    virtual bool add_key(const std::string& track, float time,
                         const std::string& value, std::string& errorOut) = 0;

    // Removes the keyframe at exactly `time` on a track. REFUSED when the
    // track or key does not exist.
    virtual bool remove_key(const std::string& track, float time,
                            std::string& errorOut) = 0;

    // Seeks the playhead (clamped to [0, duration]).
    virtual void seek(float time) = 0;

    // Validation issues: duration positive, keys within [0, duration], track
    // names unique. Empty when the document is valid.
    virtual std::vector<std::string> validate() const = 0;

    // Deterministic JSON of the snapshot (bit-exact via %.6g floats).
    virtual std::string to_json() const = 0;
};

// Factory: the SDK adapter is the only TU with behavior.
std::unique_ptr<IAnimationTimelineEditor> create_animation_timeline_editor();

}  // namespace editor
}  // namespace engine
