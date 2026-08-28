#include "engine/gameplay/IGameplayIntegration.hpp"
#include "engine/entity/IEntityWorld.hpp"
#include "engine/gameplay/IGameplayEvents.hpp"
#include "engine/gameplay/IGameplayMetrics.hpp"
#include "engine/navigation/IAsyncQueryScheduler.hpp"
#include <cstdio>
#include <string>

int main() {
    auto integration = engine::gameplay::create_gameplay_integration();
    auto entities = engine::entity::create_entity_world();
    auto events = engine::gameplay::create_gameplay_events();
    auto metrics = engine::gameplay::create_gameplay_metrics();
    auto queries = engine::navigation::create_async_query_scheduler();
    std::string error;
    if (!integration->configure(1.0f / 60.0f, 1, error)) return 1;
    if (!integration->attach_entity_world(entities.get())) return 2;
    if (!integration->attach_events(events.get())) return 3;
    if (!integration->attach_metrics(metrics.get())) return 4;
    if (!integration->attach_queries(queries.get())) return 5;
    if (!entities->spawn("player", {}, error).valid()) return 6;
    integration->advance(1.0f / 30.0f);
    const auto snapshot = integration->snapshot();
    if (snapshot.tick != 2 || snapshot.entities != 1 || snapshot.simulationSeconds <= 0.0) return 7;
    if (metrics->snapshot().empty()) return 8;
    std::printf("gameplay_integration_coordinator_tests: all checks passed\n");
    return 0;
}
