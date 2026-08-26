// AudioEventMapper.cpp — the only TU implementing the public audio event
// mapper (Agente 4 §7 item 76 CORE): gameplay event kinds → audio triggers.
// Pure std; triggers kept in std::map (order by eventKind, deterministic).

#include "engine/audio/IAudioEventMapper.hpp"

#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

namespace engine {
namespace audio {
namespace {

bool finite_float(float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float is 32-bit");
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000u) != 0x7f800000u;
}

class AudioEventMapper final : public IAudioEventMapper {
public:
    AudioEventMapper() = default;

    bool configure(const std::vector<AudioTrigger>& triggers,
                   std::string& errorOut) override {
        std::map<std::string, AudioTrigger> parsed;
        for (const AudioTrigger& trigger : triggers) {
            if (trigger.eventKind.empty()) {
                errorOut = "audio event mapper: eventKind must be non-empty";
                return false;
            }
            if (trigger.soundId.empty()) {
                errorOut = "audio event mapper: soundId for '" + trigger.eventKind +
                           "' must be non-empty";
                return false;
            }
            if (!finite_float(trigger.volume) || trigger.volume < 0.0f ||
                trigger.volume > 1.0f) {
                errorOut = "audio event mapper: volume for '" + trigger.eventKind +
                           "' must be in [0,1]";
                return false;
            }
            if (!finite_float(trigger.pitch) || trigger.pitch <= 0.0f) {
                errorOut = "audio event mapper: pitch for '" + trigger.eventKind +
                           "' must be > 0";
                return false;
            }
            if (parsed.count(trigger.eventKind) != 0) {
                errorOut = "audio event mapper: duplicate eventKind '" +
                           trigger.eventKind + "'";
                return false;
            }
            parsed[trigger.eventKind] = trigger;
        }
        triggers_ = std::move(parsed);
        return true;
    }

    const AudioTrigger* trigger_for(const std::string& eventKind) const override {
        const auto found = triggers_.find(eventKind);
        return found == triggers_.end() ? nullptr : &found->second;
    }

    std::vector<AudioTrigger> triggers() const override {
        std::vector<AudioTrigger> out;
        out.reserve(triggers_.size());
        for (const auto& entry : triggers_) out.push_back(entry.second);
        return out;
    }

    std::size_t count() const override { return triggers_.size(); }
    void clear() override { triggers_.clear(); }

private:
    std::map<std::string, AudioTrigger> triggers_;
};

}  // namespace

std::unique_ptr<IAudioEventMapper> create_audio_event_mapper() {
    return std::make_unique<AudioEventMapper>();
}

}  // namespace audio
}  // namespace engine
