// CrowdSimulationTests — gate do contrato ICrowdSimulation (agente 4 §10
// l.176 "Integrar multidões com LOD de simulação, sleeping, agregação distante
// e retomada determinística"): prova spec all-or-nothing, classificação por
// tier determinística (Full/Reduced/Aggregate/Dormant), sleeping/acordar,
// budget de ticks por frame, agregação distante por tipo e round-trip JSON
// bit-exact all-or-nothing.

#include "engine/ai/ICrowdSimulation.hpp"

#include <cmath>
#include <cstdio>
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

void test_configure() {
    auto crowd = engine::ai::create_crowd_simulation();
    std::string error;

    engine::ai::CrowdSpec spec;
    check(crowd->configure(spec, error), "configure default spec");

    engine::ai::CrowdSpec bad;
    bad.reduced_radius = 8.0;  // < full_radius (16)
    check(!crowd->configure(bad, error), "reduced < full recusa");
    bad = engine::ai::CrowdSpec();
    bad.aggregate_radius = 32.0;  // < reduced (64)
    check(!crowd->configure(bad, error), "aggregate < reduced recusa");
    bad = engine::ai::CrowdSpec();
    bad.reduced_interval = 0.5;
    check(!crowd->configure(bad, error), "reduced_interval < 1 recusa");
    bad = engine::ai::CrowdSpec();
    bad.max_agents = 0;
    check(!crowd->configure(bad, error), "max_agents 0 recusa");
    bad = engine::ai::CrowdSpec();
    bad.full_radius = -1.0;
    check(!crowd->configure(bad, error), "full_radius negativo recusa");

    // JSON round-trip bit-exact
    engine::ai::CrowdSpec a;
    a.full_radius = 20.0;
    a.reduced_radius = 80.0;
    a.aggregate_radius = 300.0;
    a.reduced_interval = 6.0;
    a.max_agents = 500;
    const std::string j1 = a.to_json();
    engine::ai::CrowdSpec b;
    check(b.load_from_json(j1, error), "load json");
    check(b.to_json() == j1, "round-trip bit-exact");
    check(b.full_radius == 20.0 && b.reduced_interval == 6.0 && b.max_agents == 500,
          "campos restaurados");
    check(!b.load_from_json("{\"full_radius\": -3}", error), "load inválido recusa");
}

void test_tiers() {
    auto crowd = engine::ai::create_crowd_simulation();
    std::string error;
    engine::ai::CrowdSpec spec;
    spec.full_radius = 10.0;
    spec.reduced_radius = 40.0;
    spec.aggregate_radius = 100.0;
    spec.reduced_interval = 2.0;
    check(crowd->configure(spec, error), "configure p/ tiers");

    std::vector<engine::ai::CrowdAgent> agents;
    agents.push_back({ 1, { 1, 0, 0 }, "villager" });    // dist 1 → Full
    agents.push_back({ 2, { 20, 0, 0 }, "villager" });   // dist 20 → Reduced
    agents.push_back({ 3, { 60, 0, 0 }, "guard" });      // dist 60 → Aggregate
    agents.push_back({ 4, { 200, 0, 0 }, "guard" });     // dist 200 → Dormant
    check(crowd->set_agents(agents, error), "set 4 agentes");

    const engine::ai::Vec3 focus{ 0, 0, 0 };
    engine::ai::CrowdFrameResult r = crowd->advance(focus, 1, error);
    check(r.agent_states.size() == 4, "4 estados");

    engine::ai::CrowdAgentState st;
    check(crowd->agent_state(1, st) && st.tier == engine::ai::CrowdTier::Full,
          "id 1 Full");
    check(crowd->agent_state(2, st) && st.tier == engine::ai::CrowdTier::Reduced,
          "id 2 Reduced");
    check(crowd->agent_state(3, st) && st.tier == engine::ai::CrowdTier::Aggregate,
          "id 3 Aggregate");
    check(crowd->agent_state(4, st) && st.tier == engine::ai::CrowdTier::Dormant,
          "id 4 Dormant");

    // Full e Reduced (interval 2, tick 0 → ticka) tickam; Aggregate/Dormant não
    check(r.agent_states[0].tick_this_frame, "Full ticka");
    check(r.agent_states[1].tick_this_frame, "Reduced ticka no frame 0");
    check(!r.agent_states[2].tick_this_frame, "Aggregate não ticka");
    check(!r.agent_states[3].tick_this_frame, "Dormant não ticka");
}

void test_sleeping_wake() {
    auto crowd = engine::ai::create_crowd_simulation();
    std::string error;
    engine::ai::CrowdSpec spec;
    spec.full_radius = 5.0;
    spec.reduced_radius = 20.0;
    spec.aggregate_radius = 50.0;
    spec.reduced_interval = 2.0;
    check(crowd->configure(spec, error), "configure p/ sleeping");

    std::vector<engine::ai::CrowdAgent> agents;
    agents.push_back({ 1, { 100, 0, 0 }, "villager" });  // longe → Dormant
    check(crowd->set_agents(agents, error), "set 1 agente");

    const engine::ai::Vec3 farFocus{ 0, 0, 0 };
    engine::ai::CrowdFrameResult r = crowd->advance(farFocus, 3, error);
    engine::ai::CrowdAgentState st;
    check(crowd->agent_state(1, st) && st.tier == engine::ai::CrowdTier::Dormant,
          "fica Dormant longe");
    check(st.idle_ticks == 3, "idle acumula 3");

    // Foco se aproxima → acorda (volta a Full)
    const engine::ai::Vec3 nearFocus{ 100, 0, 0 };
    r = crowd->advance(nearFocus, 1, error);
    check(r.woke_any, "woke_any ao se aproximar");
    check(crowd->agent_state(1, st) && st.tier == engine::ai::CrowdTier::Full,
          "acordou para Full");
    check(st.idle_ticks == 0, "idle zerado ao acordar");
}

void test_budget() {
    auto crowd = engine::ai::create_crowd_simulation();
    std::string error;
    engine::ai::CrowdSpec spec;
    spec.full_radius = 1000.0;  // tudo Full
    spec.reduced_radius = 2000.0;
    spec.aggregate_radius = 3000.0;
    spec.max_ticks_per_frame = 2;
    check(crowd->configure(spec, error), "configure p/ budget");

    std::vector<engine::ai::CrowdAgent> agents;
    agents.push_back({ 1, { 1, 0, 0 }, "a" });
    agents.push_back({ 2, { 2, 0, 0 }, "a" });
    agents.push_back({ 3, { 3, 0, 0 }, "a" });
    agents.push_back({ 4, { 4, 0, 0 }, "a" });
    check(crowd->set_agents(agents, error), "set 4 agentes");

    const engine::ai::Vec3 focus{ 0, 0, 0 };
    engine::ai::CrowdFrameResult r = crowd->advance(focus, 1, error);
    // Budget 2 → só os 2 MAIS PRÓXIMOS tickam (ids 1 e 2)
    int ticked = 0;
    for (const auto& s : r.agent_states) {
        if (s.tick_this_frame) {
            ++ticked;
            check(s.id == 1 || s.id == 2, "os mais próximos tickam");
        }
    }
    check(ticked == 2, "budget 2 respeitado");
}

void test_aggregate() {
    auto crowd = engine::ai::create_crowd_simulation();
    std::string error;
    engine::ai::CrowdSpec spec;
    spec.full_radius = 1.0;
    spec.reduced_radius = 2.0;
    spec.aggregate_radius = 100.0;
    spec.reduced_interval = 2.0;
    check(crowd->configure(spec, error), "configure p/ agregado");

    std::vector<engine::ai::CrowdAgent> agents;
    agents.push_back({ 1, { 50, 0, 0 }, "villager" });
    agents.push_back({ 2, { 60, 0, 0 }, "villager" });
    agents.push_back({ 3, { 70, 0, 0 }, "guard" });
    check(crowd->set_agents(agents, error), "set 3 agentes");

    const engine::ai::Vec3 focus{ 0, 0, 0 };
    engine::ai::CrowdFrameResult r = crowd->advance(focus, 1, error);
    check(r.aggregates.size() == 2, "2 grupos agregados (villager, guard)");
    for (const auto& agg : r.aggregates) {
        if (agg.type == "villager") check(agg.count == 2, "villager count 2");
        if (agg.type == "guard") check(agg.count == 1, "guard count 1");
    }
}

void test_persistence() {
    auto crowd = engine::ai::create_crowd_simulation();
    std::string error;
    engine::ai::CrowdSpec spec;
    spec.full_radius = 10.0;
    spec.reduced_radius = 40.0;
    spec.aggregate_radius = 100.0;
    spec.reduced_interval = 2.0;
    check(crowd->configure(spec, error), "configure p/ persistência");

    std::vector<engine::ai::CrowdAgent> agents;
    agents.push_back({ 1, { 1, 0, 0 }, "villager" });
    agents.push_back({ 2, { 60, 0, 0 }, "guard" });
    check(crowd->set_agents(agents, error), "set 2 agentes");

    const engine::ai::Vec3 focus{ 0, 0, 0 };
    crowd->advance(focus, 3, error);
    const std::string state = crowd->serialize_state();

    // Restaura em instância nova e compara bit-exact
    auto crowd2 = engine::ai::create_crowd_simulation();
    check(crowd2->configure(spec, error), "configure instância 2");
    check(crowd2->deserialize_state(state, error), "deserialize");
    check(crowd2->serialize_state() == state, "round-trip bit-exact");

    // All-or-nothing: json inválido rejeita com estado intacto
    check(!crowd2->deserialize_state("{\"version\":2}", error), "version 2 recusa");
    check(crowd2->serialize_state() == state, "estado intacto após recusa");
    check(!crowd2->deserialize_state("{\"version\":1,\"tick\":0}", error),
          "sem agents recusa");
    check(crowd2->serialize_state() == state, "estado intacto após recusa 2");

    // Determinismo cross-instance
    auto crowd3 = engine::ai::create_crowd_simulation();
    check(crowd3->configure(spec, error), "configure instância 3");
    check(crowd3->deserialize_state(state, error), "deserialize 3");
    engine::ai::CrowdFrameResult r2 = crowd2->advance(focus, 5, error);
    engine::ai::CrowdFrameResult r3 = crowd3->advance(focus, 5, error);
    check(crowd2->serialize_state() == crowd3->serialize_state(),
          "determinismo bit-exact cross-instance");
}

}  // namespace

int main() {
    std::printf("CrowdSimulationTests\n");
    test_configure();
    test_tiers();
    test_sleeping_wake();
    test_budget();
    test_aggregate();
    test_persistence();
    if (failures == 0) {
        std::printf("ALL PASSED\n");
        return 0;
    }
    std::printf("%d failure(s)\n", failures);
    return 1;
}
