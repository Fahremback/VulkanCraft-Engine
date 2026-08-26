// GameplayEventRouterTests — gate do contrato IGameplayEventRouter (§5 item
// 75 + §2 item 30, wiring events→áudio/métricas): prova que o router drena
// FIFO, traduz kind→eventKind, emite triggers na ordem e registra counters
// por eventKind; mapping all-or-nothing.

#include "engine/gameplay/IGameplayEventRouter.hpp"

#include "engine/audio/IAudioEventMapper.hpp"
#include "engine/gameplay/IGameplayEvents.hpp"
#include "engine/gameplay/IGameplayMetrics.hpp"

#include <cstdio>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what);
        ++failures;
    }
}

std::vector<std::uint8_t> payload() {
    return std::vector<std::uint8_t>{ 0x01, 0x02 };
}

void test_routing() {
    auto events = engine::gameplay::create_gameplay_events();
    auto audio = engine::audio::create_audio_event_mapper();
    auto metrics = engine::gameplay::create_gameplay_metrics();
    std::string error;

    std::vector<engine::audio::AudioTrigger> triggers;
    triggers.push_back({ "explosion", "boom_01", 0.9f, 1.0f });
    triggers.push_back({ "footstep", "step_01", 0.4f, 1.2f });
    check(audio->configure(triggers, error), "audio configure");

    auto router = engine::gameplay::create_gameplay_event_router(
        events.get(), audio.get(), metrics.get());
    check(router != nullptr, "router criado");

    std::vector<std::pair<std::uint16_t, std::string>> mapping;
    mapping.push_back({ 1, "explosion" });
    mapping.push_back({ 2, "footstep" });
    check(router->configure_mapping(mapping, error), "mapping ok");

    // Sem eventos → nada.
    check(router->route().empty(), "sem eventos → vazio");

    events->publish(2, 10, payload());   // footstep
    events->publish(1, 11, payload());   // explosion
    events->publish(3, 12, payload());   // não mapeado → ignorado
    const std::vector<engine::gameplay::AudioTriggerRequest> requests =
        router->route();
    check(requests.size() == 2, "2 requisições (ordem FIFO)");
    check(requests[0].eventKind == "footstep" &&
              requests[0].soundId == "step_01" && requests[0].volume == 0.4f,
          "1ª = footstep (FIFO)");
    check(requests[1].eventKind == "explosion" &&
              requests[1].soundId == "boom_01",
          "2ª = explosion");
    check(router->routed_count() == 3, "3 eventos roteados (incl. não mapeado)");
    check(events->pending_count() == 0, "fila drenada");

    // Métricas: counters por eventKind.
    std::vector<engine::gameplay::GameplayMetric> snapshot = metrics->snapshot();
    check(snapshot.size() == 2, "2 métricas criadas");
    check(snapshot[0].name == "events.explosion" &&
              snapshot[0].value == 1.0,
          "counter explosion = 1");
    check(snapshot[1].name == "events.footstep" &&
              snapshot[1].value == 1.0,
          "counter footstep = 1");
}

void test_mapping_refusals() {
    auto events = engine::gameplay::create_gameplay_events();
    auto audio = engine::audio::create_audio_event_mapper();
    auto metrics = engine::gameplay::create_gameplay_metrics();
    std::string error;

    auto router = engine::gameplay::create_gameplay_event_router(
        events.get(), audio.get(), metrics.get());
    std::vector<std::pair<std::uint16_t, std::string>> mapping;
    mapping.push_back({ 1, "a" });
    mapping.push_back({ 1, "b" });
    check(!router->configure_mapping(mapping, error), "kind duplicado recusa");
    mapping.clear();
    mapping.push_back({ 1, "" });
    check(!router->configure_mapping(mapping, error), "eventKind vazio recusa");
    mapping.clear();
    mapping.push_back({ 1, "a" });
    check(router->configure_mapping(mapping, error), "mapping válido aceito");
}

void test_null_components() {
    auto audio = engine::audio::create_audio_event_mapper();
    auto metrics = engine::gameplay::create_gameplay_metrics();
    auto bad = engine::gameplay::create_gameplay_event_router(
        nullptr, audio.get(), metrics.get());
    check(bad == nullptr, "events nulo → nullptr");
}

}  // namespace

int main() {
    test_routing();
    test_mapping_refusals();
    test_null_components();

    if (failures == 0) {
        std::printf("gameplay_event_router_tests: all checks passed\n");
        return 0;
    }
    std::printf("gameplay_event_router_tests: %d failure(s)\n", failures);
    return 1;
}
