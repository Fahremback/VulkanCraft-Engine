// NavigationSchedulerBridgeTests — gate do contrato INavigationSchedulerBridge
// (§2 item 27, wiring scheduler↔provider): prova que o bridge enfileira com
// prioridade, despacha o lote do orçamento para o provider async, coleta
// resultados concluídos (Succeeded/Failed), propaga cancel em voo e mantém
// determinismo (resultados ordenados por id). Usa um MOCK provider
// determinístico (o navmesh real é do AGENT-6) — a política do bridge é o
// que está sob teste.

#include "engine/navigation/INavigationSchedulerBridge.hpp"

#include "engine/navigation/IAsyncQueryScheduler.hpp"
#include "engine/navigation/INavigationProvider.hpp"

#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what);
        ++failures;
    }
}

// Mock provider determinístico: completa cada request no frame seguinte com
// um waypoint derivado do id (nunca falha; começando em 1, Succeeded).
class MockProvider final : public engine::navigation::INavigationProvider {
public:
    bool build(const engine::navigation::NavmeshConfig&,
               const std::vector<engine::navigation::VoxelColumn>&,
               std::string&) override {
        return true;
    }
    bool update(const std::vector<engine::navigation::VoxelColumn>&,
                std::string&) override {
        return true;
    }
    bool find_path(float, float, float, float, float, float,
                   engine::navigation::PathResult& out) const override {
        out.found = true;
        out.waypoints = { 0.0f, 0.0f, 0.0f };
        return true;
    }
    bool is_walkable(float, float, float) const override { return true; }
    std::uint64_t revision() const override { return 1; }
    std::uint64_t tile_revision(float, float) const override { return 1; }
    bool valid() const override { return true; }
    bool set_dynamic_obstacle(std::uint64_t,
                              const engine::navigation::DynamicObstacle&,
                              std::string&) override {
        return true;
    }
    bool set_obstacle_active(std::uint64_t, bool, std::string&) override {
        return true;
    }
    bool set_area_cost(int, float, std::string&) override { return true; }
    float area_cost(int) const override { return 1.0f; }
    bool set_off_mesh_links(const std::vector<engine::navigation::OffMeshLink>&,
                            std::string&) override {
        return true;
    }

    // Async: cada request fica Running até o próximo poll (2 polls → done).
    std::uint64_t begin_async_path(float, float, float, float, float, float,
                                   std::string& errorOut) override {
        if (!ready_) {
            errorOut = "mock: not ready";
            return 0;
        }
        const std::uint64_t id = ++nextId_;
        polls_[id] = 0;
        return id;
    }
    engine::navigation::PathRequestStatus poll_async_path(
        std::uint64_t requestId, engine::navigation::PathResult& out,
        std::string&) override {
        const auto found = polls_.find(requestId);
        if (found == polls_.end()) {
            return engine::navigation::PathRequestStatus::Invalid;
        }
        ++found->second;
        if (found->second >= 2) {
            out.found = true;
            out.waypoints = { static_cast<float>(requestId), 0.0f, 0.0f };
            polls_.erase(found);
            return engine::navigation::PathRequestStatus::Succeeded;
        }
        return engine::navigation::PathRequestStatus::Running;
    }
    bool cancel_async_path(std::uint64_t requestId) override {
        const auto found = polls_.find(requestId);
        if (found == polls_.end()) return false;
        polls_.erase(found);
        cancelledIds_.push_back(requestId);
        return true;
    }

    void set_ready(bool ready) { ready_ = ready; }
    const std::vector<std::uint64_t>& cancelled_ids() const {
        return cancelledIds_;
    }

private:
    bool ready_{ true };
    std::uint64_t nextId_{ 0 };
    std::map<std::uint64_t, int> polls_;
    std::vector<std::uint64_t> cancelledIds_;
};

void test_enqueue_and_dispatch() {
    auto scheduler = engine::navigation::create_async_query_scheduler();
    MockProvider provider;
    auto bridge = engine::navigation::create_navigation_scheduler_bridge(
        scheduler.get(), &provider);
    check(bridge != nullptr, "bridge criado");
    std::string error;
    check(bridge->configure(8, 2.0f, error), "configure 8/2.0");

    // Prioridade: 2 > 1 → despacha 2 primeiro (orçamento cobre ambos).
    check(bridge->enqueue_path(101, 1.0f, 0.0f, 1.0f, 0, 0, 0, 5, 0, 0, error),
          "enqueue 101 (prio 1)");
    check(bridge->enqueue_path(102, 2.0f, 0.0f, 1.0f, 0, 0, 0, 5, 0, 0, error),
          "enqueue 102 (prio 2)");
    check(bridge->queued_count() == 2, "2 na fila");

    std::vector<engine::navigation::NavQueryResult> results = bridge->frame();
    check(results.empty(), "frame 1: nada concluído (ambos Running)");
    check(bridge->in_flight_count() == 2, "2 em voo");

    results = bridge->frame();
    check(results.size() == 2, "frame 2: 2 concluídos");
    check(results[0].queryId == 101 && results[1].queryId == 102,
          "resultados ordenados por id");
    check(results[0].succeeded && results[1].succeeded, "ambos succeeded");
    check(results[0].pathJson.find("\"found\":true") != std::string::npos &&
              results[0].pathJson.find("waypoints") != std::string::npos,
          "pathJson serializado (found + waypoints)");
    check(bridge->in_flight_count() == 0, "0 em voo após coleta");
}

void test_budget_limits_batch() {
    auto scheduler = engine::navigation::create_async_query_scheduler();
    MockProvider provider;
    auto bridge = engine::navigation::create_navigation_scheduler_bridge(
        scheduler.get(), &provider);
    std::string error;
    check(bridge->configure(8, 1.5f, error), "configure 8/1.5");

    check(bridge->enqueue_path(201, 1.0f, 0.0f, 1.0f, 0, 0, 0, 1, 0, 0, error),
          "enqueue 201 (custo 1)");
    check(bridge->enqueue_path(202, 1.0f, 0.0f, 1.0f, 0, 0, 0, 1, 0, 0, error),
          "enqueue 202 (custo 1)");
    // Custo 1 + 1 = 2 > 1.5 → só o primeiro cabe no orçamento do frame.
    std::vector<engine::navigation::NavQueryResult> results = bridge->frame();
    check(bridge->in_flight_count() == 1, "orçamento: 1 despachado");
    results = bridge->frame();
    check(results.size() == 1 && results[0].queryId == 201, "201 concluído");
    check(bridge->in_flight_count() == 1, "202 segue pendente (Running)");
}

void test_cancel_queued_and_inflight() {
    auto scheduler = engine::navigation::create_async_query_scheduler();
    MockProvider provider;
    auto bridge = engine::navigation::create_navigation_scheduler_bridge(
        scheduler.get(), &provider);
    std::string error;
    check(bridge->configure(8, 10.0f, error), "configure");

    check(bridge->enqueue_path(301, 1.0f, 0.0f, 1.0f, 0, 0, 0, 1, 0, 0, error),
          "enqueue 301");
    check(bridge->enqueue_path(302, 1.0f, 0.0f, 1.0f, 0, 0, 0, 1, 0, 0, error),
          "enqueue 302");
    check(bridge->cancel(301), "cancel 301 enfileirada");
    check(bridge->queued_count() == 1, "1 restante na fila");
    check(!bridge->cancel(999), "cancel desconhecido → false");

    // Despacha 302; cancela em voo → propaga para o provider (join).
    bridge->frame();
    check(bridge->in_flight_count() == 1, "302 em voo");
    check(bridge->cancel(302), "cancel 302 em voo");
    check(provider.cancelled_ids().size() == 1, "provider recebeu cancel");
    check(bridge->in_flight_count() == 0, "nada em voo após cancel");
}

void test_timeout_expires() {
    auto scheduler = engine::navigation::create_async_query_scheduler();
    MockProvider provider;
    auto bridge = engine::navigation::create_navigation_scheduler_bridge(
        scheduler.get(), &provider);
    std::string error;
    check(bridge->configure(8, 10.0f, error), "configure");

    check(bridge->enqueue_path(401, 1.0f, 2.0f, 1.0f, 0, 0, 0, 1, 0, 0, error),
          "enqueue 401 (timeout 2s)");
    const std::vector<std::uint64_t> expired = bridge->tick(3.0f);
    check(expired.size() == 1 && expired[0] == 401,
          "timeout expira 401 no relógio do scheduler");
    check(bridge->queued_count() == 0, "fila vazia após timeout");
}

void test_refusals() {
    auto scheduler = engine::navigation::create_async_query_scheduler();
    MockProvider provider;
    auto bridge = engine::navigation::create_navigation_scheduler_bridge(
        scheduler.get(), &provider);
    std::string error;
    check(bridge->configure(8, 10.0f, error), "configure");
    check(bridge->enqueue_path(501, 1.0f, 0.0f, 1.0f, 0, 0, 0, 1, 0, 0, error),
          "enqueue 501");
    check(!bridge->enqueue_path(501, 1.0f, 0.0f, 1.0f, 0, 0, 0, 1, 0, 0, error),
          "id duplicado recusa");
    check(!bridge->enqueue_path(502, 1.0f, 0.0f, 1.0f, 0, 0, 0,
                                std::numeric_limits<float>::infinity(),
                                0, 0, error),
          "ponto não-finito recusa");
    check(bridge->queued_count() == 1, "fila intacta após recusas");

    auto badBridge = engine::navigation::create_navigation_scheduler_bridge(
        nullptr, &provider);
    check(badBridge == nullptr, "scheduler nulo → nullptr");
}

}  // namespace

int main() {
    test_enqueue_and_dispatch();
    test_budget_limits_batch();
    test_cancel_queued_and_inflight();
    test_timeout_expires();
    test_refusals();

    if (failures == 0) {
        std::printf("navigation_scheduler_bridge_tests: all checks passed\n");
        return 0;
    }
    std::printf("navigation_scheduler_bridge_tests: %d failure(s)\n", failures);
    return 1;
}
