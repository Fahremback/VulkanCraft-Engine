#include "engine/gameplay/IGameplayIntegration.hpp"
#include "engine/gameplay/IGameplayEventRouter.hpp"
#include "engine/gameplay/IGameplaySystemWiring.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace engine::gameplay {
namespace {
class GameplayIntegration final : public IGameplayIntegration {
public:
    bool configure(float fixedDelta, std::size_t queryBudget,
                   std::string& errorOut) override {
        if (!std::isfinite(fixedDelta) || fixedDelta <= 0.0f) {
            errorOut = "fixed delta must be positive and finite";
            return false;
        }
        if (queryBudget == 0) {
            errorOut = "query budget must be positive";
            return false;
        }
        fixedDelta_ = fixedDelta;
        queryBudget_ = queryBudget;
        accumulator_ = 0.0f;
        errorOut.clear();
        return true;
    }

    bool attach_bindings(IGameplayBindings* value) override {
        bindings_ = value;
        return value != nullptr;
    }
    bool attach_system_wiring(IGameplaySystemWiring* value) override {
        systemWiring_ = value;
        return value != nullptr;
    }
    bool attach_entity_world(entity::IEntityWorld* value) override { entityWorld_ = value; return value != nullptr; }
    bool attach_events(IGameplayEvents* value) override { events_ = value; return value != nullptr; }
    bool attach_metrics(IGameplayMetrics* value) override { metrics_ = value; return value != nullptr; }
    bool attach_queries(navigation::IAsyncQueryScheduler* value) override { queries_ = value; return value != nullptr; }
    bool attach_navigation_bridge(navigation::INavigationSchedulerBridge* value) override { navigationBridge_ = value; return value != nullptr; }
    bool attach_runtime(IGameplayRuntime* value) override { runtime_ = value; return value != nullptr; }
    bool attach_world_manager(world::IWorldManager* value) override { worlds_ = value; return value != nullptr; }
    bool attach_audio_mapper(audio::IAudioEventMapper* value) override { audio_ = value; return value != nullptr; }
    bool attach_event_router(IGameplayEventRouter* value) override { router_ = value; return value != nullptr; }

    bool attach_tick_source(std::uint64_t tick) override {
        if (tick < tick_) return false;
        tick_ = tick;
        return true;
    }

    bool set_world_focus(float x, float y, float z) override {
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
        worldFocus_ = glm::vec3(x, y, z);
        return true;
    }

    void advance(float deltaSeconds) override {
        if (!std::isfinite(deltaSeconds) || deltaSeconds <= 0.0f || fixedDelta_ <= 0.0f) return;
        accumulator_ = std::min(accumulator_ + deltaSeconds, fixedDelta_ * 8.0f);
        while (accumulator_ >= fixedDelta_) {
            accumulator_ -= fixedDelta_;
            ++tick_;
            simulationSeconds_ += fixedDelta_;
            if (entityWorld_) entityWorld_->tick(fixedDelta_, nullptr);
            if (runtime_) runtime_->step(fixedDelta_);
            if (queries_) {
                queries_->tick(fixedDelta_);
                queries_->dispatch(static_cast<float>(queryBudget_));
            }
            if (worlds_) {
                for (const auto& name : worlds_->world_names()) {
                    worlds_->update_world(name, worldFocus_, fixedDelta_);
                }
            }
            if (router_) {
                const auto requests = router_->route();
                mappedAudioEvents_ += requests.size();
                routedEvents_ = router_->routed_count();
            } else if (events_ && audio_) {
                for (const auto& event : events_->drain()) {
                    if (audio_->trigger_for(std::to_string(event.kind))) ++mappedAudioEvents_;
                }
            }
            if (metrics_) {
                std::string ignored;
                if (!metrics_->register_metric("gameplay.ticks", GameplayMetricKind::Counter, ignored)) ignored.clear();
                metrics_->record("gameplay.ticks", 1.0, ignored);
                if (!metrics_->register_metric("gameplay.audio_events", GameplayMetricKind::Counter, ignored)) ignored.clear();
                metrics_->record("gameplay.audio_events", static_cast<double>(mappedAudioEvents_), ignored);
            }
        }
    }

    GameplayIntegrationSnapshot snapshot() const override {
        bool complete = false;
        if (bindings_) {
            std::string ignored;
            complete = bindings_->complete(ignored);
        }
        bool wiringComplete = false;
        if (systemWiring_) {
            std::string wiringError;
            wiringComplete = systemWiring_->complete(wiringError);
        }
        return {tick_, entityWorld_ ? entityWorld_->size() : 0,
                events_ ? events_->pending_count() : 0,
                queries_ ? queries_->queued_count() : 0,
                mappedAudioEvents_, routedEvents_, simulationSeconds_, complete,
                wiringComplete && navigationBridge_ != nullptr};
    }

    void reset() override {
        tick_ = 0; accumulator_ = 0.0f; simulationSeconds_ = 0.0;
        mappedAudioEvents_ = 0; routedEvents_ = 0;
    }

private:
    IGameplayBindings* bindings_{nullptr};
    entity::IEntityWorld* entityWorld_{nullptr};
    IGameplayEvents* events_{nullptr};
    IGameplayMetrics* metrics_{nullptr};
    navigation::IAsyncQueryScheduler* queries_{nullptr};
    navigation::INavigationSchedulerBridge* navigationBridge_{nullptr};
    IGameplayRuntime* runtime_{nullptr};
    world::IWorldManager* worlds_{nullptr};
    audio::IAudioEventMapper* audio_{nullptr};
    IGameplayEventRouter* router_{nullptr};
    IGameplaySystemWiring* systemWiring_{nullptr};
    float fixedDelta_{1.0f / 60.0f};
    glm::vec3 worldFocus_{0.0f};
    std::size_t queryBudget_{1};
    float accumulator_{0.0f};
    std::uint64_t tick_{0};
    std::size_t mappedAudioEvents_{0};
    std::uint64_t routedEvents_{0};
    double simulationSeconds_{0.0};
};
}

std::unique_ptr<IGameplayIntegration> create_gameplay_integration() {
    return std::make_unique<GameplayIntegration>();
}
} // namespace engine::gameplay
