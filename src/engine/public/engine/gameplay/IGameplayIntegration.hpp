#pragma once

#include "engine/entity/IEntityWorld.hpp"
#include "engine/gameplay/IGameplayBindings.hpp"
#include "engine/gameplay/IGameplayEvents.hpp"
#include "engine/gameplay/IGameplayMetrics.hpp"
#include "engine/navigation/IAsyncQueryScheduler.hpp"
#include "engine/navigation/INavigationSchedulerBridge.hpp"
#include "engine/gameplay/IGameplayRuntime.hpp"
#include "engine/audio/IAudioEventMapper.hpp"
#include "engine/world/IWorldManager.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace engine::gameplay {
class IGameplayEventRouter;
class IGameplaySystemWiring;

struct GameplayIntegrationSnapshot {
    std::uint64_t tick{0};
    std::size_t entities{0};
    std::size_t pendingEvents{0};
    std::size_t queuedQueries{0};
    std::size_t mappedAudioEvents{0};
    std::uint64_t routedEvents{0};
    double simulationSeconds{0.0};
    bool bindingsComplete{false};
    bool runtimeWiringComplete{false};
};

class IGameplayIntegration {
public:
    virtual ~IGameplayIntegration() = default;
    virtual bool configure(float fixedDelta, std::size_t queryBudget, std::string& errorOut) = 0;
    virtual bool attach_bindings(IGameplayBindings* bindings) = 0;
    virtual bool attach_system_wiring(IGameplaySystemWiring* wiring) = 0;
    virtual bool attach_entity_world(entity::IEntityWorld* world) = 0;
    virtual bool attach_events(IGameplayEvents* events) = 0;
    virtual bool attach_metrics(IGameplayMetrics* metrics) = 0;
    virtual bool attach_queries(navigation::IAsyncQueryScheduler* queries) = 0;
    virtual bool attach_navigation_bridge(navigation::INavigationSchedulerBridge* bridge) = 0;
    virtual bool attach_runtime(IGameplayRuntime* runtime) = 0;
    virtual bool attach_world_manager(world::IWorldManager* worlds) = 0;
    virtual bool attach_audio_mapper(audio::IAudioEventMapper* audio) = 0;
    virtual bool attach_event_router(IGameplayEventRouter* router) = 0;
    virtual bool attach_tick_source(std::uint64_t tick) = 0;
    // Sets the world-streaming focus used when the integration advances every
    // attached IWorldManager world (fixed focus was a real defect: a consumer
    // binding `worlds` got its chunks streamed around (0,0,0), evicting the
    // actual play/server focus). Default remains (0,0,0) for compatibility;
    // jogo/play-mode/servidor must set the real focus before binding worlds.
    virtual bool set_world_focus(float x, float y, float z) = 0;
    virtual void advance(float deltaSeconds) = 0;
    virtual GameplayIntegrationSnapshot snapshot() const = 0;
    virtual void reset() = 0;
};

std::unique_ptr<IGameplayIntegration> create_gameplay_integration();

} // namespace engine::gameplay
