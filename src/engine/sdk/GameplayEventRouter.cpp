// GameplayEventRouter.cpp — the only TU implementing the public gameplay
// event router (Agente 4 §5 item 75 + §2 item 30 WIRING): drains gameplay
// events FIFO, translates kind → eventKind, emits audio trigger requests
// and records per-kind counters into metrics. Pure std; components injected.

#include "engine/gameplay/IGameplayEventRouter.hpp"

#include "engine/audio/IAudioEventMapper.hpp"
#include "engine/gameplay/IGameplayEvents.hpp"
#include "engine/gameplay/IGameplayMetrics.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace engine {
namespace gameplay {
namespace {

class GameplayEventRouter final : public IGameplayEventRouter {
public:
    GameplayEventRouter(IGameplayEvents* events,
                        engine::audio::IAudioEventMapper* audio,
                        IGameplayMetrics* metrics)
        : events_(events), audio_(audio), metrics_(metrics) {}

    bool configure_mapping(
        const std::vector<std::pair<std::uint16_t, std::string>>& mapping,
        std::string& errorOut) override {
        std::map<std::uint16_t, std::string> parsed;
        for (const auto& entry : mapping) {
            if (entry.second.empty()) {
                errorOut = "gameplay event router: eventKind must be non-empty";
                return false;
            }
            if (parsed.count(entry.first) != 0) {
                errorOut = "gameplay event router: duplicate kind";
                return false;
            }
            parsed[entry.first] = entry.second;
        }
        mapping_ = std::move(parsed);
        return true;
    }

    std::vector<AudioTriggerRequest> route(std::size_t maxCount) override {
        std::vector<AudioTriggerRequest> out;
        const std::vector<GameplayEvent> drained = events_->drain(maxCount);
        for (const GameplayEvent& event : drained) {
            ++routed_;
            const auto found = mapping_.find(event.kind);
            if (found == mapping_.end()) continue;   // kind não mapeado
            const std::string& eventKind = found->second;
            if (metrics_ != nullptr) {
                std::string metricError;
                const std::string metricName = "events." + eventKind;
                // Counter por eventKind: registra na primeira ocorrência e
                // depois conta. Falha de registro/record é silenciosa (a
                // métrica é observável — erro não deve derrubar o frame).
                metrics_->register_metric(
                    metricName, GameplayMetricKind::Counter, metricError);
                metrics_->record(metricName, 1.0, metricError);
            }
            if (audio_ == nullptr) continue;
            const engine::audio::AudioTrigger* trigger =
                audio_->trigger_for(eventKind);
            if (trigger == nullptr) continue;   // sem trigger configurado
            AudioTriggerRequest request;
            request.eventKind = eventKind;
            request.soundId = trigger->soundId;
            request.volume = trigger->volume;
            request.pitch = trigger->pitch;
            out.push_back(request);
        }
        return out;
    }

    std::uint64_t routed_count() const override { return routed_; }

    void reset() override {
        routed_ = 0;
        mapping_.clear();
    }

private:
    IGameplayEvents* events_{ nullptr };
    engine::audio::IAudioEventMapper* audio_{ nullptr };
    IGameplayMetrics* metrics_{ nullptr };
    std::map<std::uint16_t, std::string> mapping_;
    std::uint64_t routed_{ 0 };
};

}  // namespace

std::unique_ptr<IGameplayEventRouter> create_gameplay_event_router(
    IGameplayEvents* events, engine::audio::IAudioEventMapper* audioMapper,
    IGameplayMetrics* metrics) {
    if (events == nullptr || audioMapper == nullptr || metrics == nullptr) {
        return nullptr;
    }
    return std::make_unique<GameplayEventRouter>(events, audioMapper, metrics);
}

}  // namespace gameplay
}  // namespace engine
