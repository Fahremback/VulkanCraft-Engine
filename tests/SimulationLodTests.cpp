// SimulationLodTests.cpp
//
// FALTANTES §20: "Simulation LOD e ecossistemas" — regions with per-tier
// simulation frequency by distance/relevance, distant systems sleeping,
// deterministic analytic/aggregate evolution for distant regions, persisted
// scheduled events and world evolution, coherent transitions between
// aggregate and detailed state, world clock / climate / seasons, CPU/memory/
// network budgets, and coherent reactivation after long-distance simulation.
//
// The runtime is a PURE deterministic decision engine over the caller-owned
// SimulationLodState (the IAnimationLod pattern): it never touches a concrete
// world — it emits tier/tick/sleep/aggregate/event decisions the project
// applies. Every scenario runs headless against the public contract only.
//
// Proves, headless and text-only:
//   - spec validation all-or-nothing + JSON round-trip bit-exact;
//   - relevance falloff and tier selection boundaries;
//   - aligned region cells (floor semantics, negatives included);
//   - per-tier update frequency (the interval IS the LOD);
//   - sleeping and waking by distance;
//   - deterministic seasonal aggregate evolution (summer > winter growth);
//   - world clock (tick/day/season) and seasonal climate;
//   - scheduled events fired at the exact tick and persisted;
//   - per-tier region budgets (overflow falls to the next cheaper tier);
//   - coherent Aggregate -> Full handoff on reactivation;
//   - state serialization bit-exact + all-or-nothing deserialization;
//   - determinism across fresh instances.

#include "engine/simulation/ISimulationLod.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::printf("FAIL: %s\n", message);
        ++g_failures;
    }
}

bool bit_equal(float a, float b) {
    return std::memcmp(&a, &b, sizeof(float)) == 0;
}

// The reference configuration: full fidelity within 48 u, coarse to 211.2 u
// (rel >= 0.4), aggregate to 292.8 u (rel >= 0.1), sleeping beyond. Short day
// (10 s) with 2 days/season so the clock cycles fast in the gate.
engine::simulation::SimulationLodSpec make_spec() {
    engine::simulation::SimulationLodSpec spec;
    spec.cellSize = 16.0f;
    spec.fullRadius = 48.0f;
    spec.falloffRadius = 320.0f;
    spec.dayLengthSeconds = 10.0f;
    spec.daysPerSeason = 2;
    engine::simulation::SimulationLodTier full;
    full.name = "full";
    full.mode = engine::simulation::SimulationLodMode::Full;
    full.minRelevance = 1.0f;
    full.updateInterval = 0.0f;
    engine::simulation::SimulationLodTier coarse;
    coarse.name = "coarse";
    coarse.mode = engine::simulation::SimulationLodMode::Coarse;
    coarse.minRelevance = 0.4f;
    coarse.updateInterval = 0.25f;
    engine::simulation::SimulationLodTier aggregate;
    aggregate.name = "aggregate";
    aggregate.mode = engine::simulation::SimulationLodMode::Aggregate;
    aggregate.minRelevance = 0.1f;
    aggregate.updateInterval = 0.5f;
    aggregate.aggregateInterval = 0.5f;
    engine::simulation::SimulationLodTier sleeping;
    sleeping.name = "sleeping";
    sleeping.mode = engine::simulation::SimulationLodMode::Sleeping;
    sleeping.minRelevance = 0.0f;
    sleeping.updateInterval = 0.0f;
    spec.tiers = { full, coarse, aggregate, sleeping };
    return spec;
}

std::unique_ptr<engine::simulation::ISimulationLod> make_lod(
    const engine::simulation::SimulationLodSpec& spec = make_spec()) {
    auto lod = engine::simulation::create_simulation_lod();
    std::string error;
    const bool ok = lod->set_spec(spec, error);
    check(ok, "set_spec with the reference spec succeeds");
    return lod;
}

void test_spec_validation() {
    std::string error;
    const engine::simulation::SimulationLodSpec base = make_spec();
    check(base.validate(error), "reference spec validates");

    auto bad = base;
    bad.version = 2;
    check(!bad.validate(error), "version 2 refused");
    bad = base;
    bad.cellSize = 0.0f;
    check(!bad.validate(error), "cellSize 0 refused");
    bad = base;
    bad.cellSize = -1.0f;
    check(!bad.validate(error), "cellSize negative refused");
    bad = base;
    bad.fullRadius = 400.0f;
    check(!bad.validate(error), "fullRadius >= falloffRadius refused");
    bad = base;
    bad.dayLengthSeconds = 0.0f;
    check(!bad.validate(error), "dayLengthSeconds 0 refused");
    bad = base;
    bad.daysPerSeason = 0;
    check(!bad.validate(error), "daysPerSeason 0 refused");
    bad = base;
    bad.tiers.clear();
    check(!bad.validate(error), "empty tiers refused");
    bad = base;
    bad.tiers[0].name = "";
    check(!bad.validate(error), "empty tier name refused");
    bad = base;
    bad.tiers[2].name = "full";
    check(!bad.validate(error), "duplicate tier name refused");
    bad = base;
    std::swap(bad.tiers[0], bad.tiers[1]);
    check(!bad.validate(error), "non-descending tiers refused");
    bad = base;
    bad.tiers[1].minRelevance = bad.tiers[0].minRelevance;  // equal thresholds
    check(!bad.validate(error), "equal minRelevance refused (must be strictly descending)");
    bad = base;
    bad.tiers[1].updateInterval = -0.1f;
    check(!bad.validate(error), "negative updateInterval refused");
    bad = base;
    bad.tiers[2].aggregateInterval = -1.0f;
    check(!bad.validate(error), "negative aggregateInterval refused");
    bad = base;
    bad.tiers[3].maxRegions = -1;
    check(!bad.validate(error), "negative maxRegions refused");
    bad = base;
    bad.tiers[3].sleepAfterIdle = -1.0f;
    check(!bad.validate(error), "negative sleepAfterIdle refused");
    bad = base;
    bad.tiers[0].minRelevance = 1.5f;
    check(!bad.validate(error), "minRelevance > 1 refused");
}

void test_spec_round_trip() {
    const engine::simulation::SimulationLodSpec spec = make_spec();
    const std::string json = spec.to_json();
    engine::simulation::SimulationLodSpec loaded;
    std::string error;
    check(loaded.load_from_json(json, error), "spec JSON loads");
    check(loaded.to_json() == json, "spec JSON round-trips bit-exact");
    check(loaded.cellSize == spec.cellSize && loaded.fullRadius == spec.fullRadius &&
              loaded.falloffRadius == spec.falloffRadius &&
              loaded.dayLengthSeconds == spec.dayLengthSeconds &&
              loaded.daysPerSeason == spec.daysPerSeason && loaded.tiers.size() == 4,
          "spec fields survive the round-trip");
    check(loaded.tiers[0].name == "full" && loaded.tiers[1].name == "coarse" &&
              loaded.tiers[2].name == "aggregate" && loaded.tiers[3].name == "sleeping",
          "tier names survive the round-trip");
    check(bit_equal(loaded.tiers[1].updateInterval, 0.25f) &&
              loaded.tiers[2].aggregateInterval == 0.5f && loaded.tiers[3].maxRegions == 0,
          "tier numbers survive the round-trip");
    check(!loaded.load_from_json("not json", error), "malformed spec JSON refused");
    check(!loaded.load_from_json("{\"version\":1,\"tiers\":[]}", error),
          "spec with empty tiers refused at load");
}

void test_relevance_and_tiers() {
    auto lod = make_lod();
    const engine::simulation::SimulationLodSpec* spec = lod->spec();
    check(spec != nullptr, "spec is queryable");

    check(lod->relevance(0.0f) == 1.0f, "relevance 1 inside fullRadius");
    check(lod->relevance(spec->fullRadius) == 1.0f, "relevance 1 at fullRadius");
    check(lod->relevance(spec->falloffRadius) == 0.0f, "relevance 0 at falloffRadius");
    check(lod->relevance(spec->falloffRadius + 50.0f) == 0.0f, "relevance 0 beyond falloff");
    const float mid = lod->relevance((spec->fullRadius + spec->falloffRadius) * 0.5f);
    check(mid > 0.4f && mid < 0.6f, "relevance linear between the radii");

    std::size_t tier = 99;
    std::string error;
    check(lod->select_tier(1.0f, tier, error) && tier == 0, "relevance 1 -> full tier");
    check(lod->select_tier(0.5f, tier, error) && tier == 1, "relevance 0.5 -> coarse tier");
    check(lod->select_tier(0.2f, tier, error) && tier == 2, "relevance 0.2 -> aggregate tier");
    check(lod->select_tier(0.0f, tier, error) && tier == 3, "relevance 0 -> sleeping tier");
    check(lod->select_tier(0.05f, tier, error) && tier == 3, "relevance below every threshold -> last tier");
    check(!lod->select_tier(1.5f, tier, error), "relevance > 1 refused");
    check(!lod->select_tier(-0.1f, tier, error), "relevance < 0 refused");
}

void test_region_cells() {
    auto lod = make_lod();
    std::int64_t x = 0, z = 0;
    lod->region_cell(100.5f, -200.25f, x, z);
    check(x == 6 && z == -13, "region_cell floors world/cellSize (100.5/16=6, -200.25/16=-13)");
    lod->region_cell(15.9f, 0.0f, x, z);
    check(x == 0 && z == 0, "region_cell 15.9 -> cell 0");
    lod->region_cell(16.0f, 16.0f, x, z);
    check(x == 1 && z == 1, "region_cell 16.0 -> cell 1");
    std::string key;
    lod->region_key(-3, 7, key);
    check(key == "-3,7", "region_key format cellX,cellZ");
}

void test_frequency() {
    auto lod = make_lod();
    engine::simulation::SimulationLodState state;
    std::string error;
    check(lod->add_region(state, 0, 0, {}, error), "near region added");     // full tier
    check(lod->add_region(state, 9, 0, {}, error), "mid region added");      // coarse tier (rel ~0.62)
    std::vector<engine::simulation::SimulationLodEvent> events;
    int fullTicks = 0;
    int coarseTicks = 0;
    for (int i = 0; i < 100; ++i) {
        events.clear();
        check(lod->update(state, 0.0f, 0.0f, 0.01f, events, error), "update ok");
        for (const auto& event : events) {
            if (event.kind == engine::simulation::SimulationLodEvent::Kind::RegionTick) {
                if (event.cellX == 0 && event.cellZ == 0) ++fullTicks;
                if (event.cellX == 9 && event.cellZ == 0) ++coarseTicks;
            }
        }
    }
    check(fullTicks == 100, "full tier ticks every update (100/100)");
    check(coarseTicks >= 3 && coarseTicks <= 5,
          "coarse tier ticks at its interval (~4/s at 0.25s)");
}

void test_sleeping_and_waking() {
    auto lod = make_lod();
    engine::simulation::SimulationLodState state;
    std::string error;
    // Cell (20, 0) center (328, 8) — beyond falloff: sleeping.
    check(lod->add_region(state, 20, 0, {}, error), "far region added");
    std::vector<engine::simulation::SimulationLodEvent> events;
    bool slept = false;
    bool woken = false;
    // 10 updates far away: the region sleeps and stays asleep (no ticks).
    int farTicks = 0;
    for (int i = 0; i < 10; ++i) {
        events.clear();
        check(lod->update(state, 0.0f, 0.0f, 0.01f, events, error), "far update ok");
        for (const auto& event : events) {
            if (event.kind == engine::simulation::SimulationLodEvent::Kind::RegionSlept) slept = true;
            if (event.kind == engine::simulation::SimulationLodEvent::Kind::RegionTick) ++farTicks;
        }
    }
    check(slept, "far region sleeps");
    check(farTicks == 0, "sleeping region never ticks");
    check(state.regions[0].tier == 3, "far region is in the sleeping tier");
    check(state.regions[0].idleSeconds > 0.0f, "idleSeconds accumulates while asleep");

    // Approach the region: it wakes, returns to full, and ticks again.
    int wakeTicks = 0;
    bool wakeCarriesTier = false;
    for (int i = 0; i < 5; ++i) {
        events.clear();
        check(lod->update(state, 330.0f, 8.0f, 0.01f, events, error), "near update ok");
        for (const auto& event : events) {
            if (event.kind == engine::simulation::SimulationLodEvent::Kind::RegionWoken) {
                woken = true;
                wakeCarriesTier = event.fromTier == 3 && event.toTier == 0;
            }
            if (event.kind == engine::simulation::SimulationLodEvent::Kind::RegionTick) ++wakeTicks;
        }
    }
    check(woken, "approaching region wakes");
    check(wakeCarriesTier, "the wake event carries the sleeping -> full tier transition");
    check(state.regions[0].tier == 0, "woken region returns to the full tier");
    check(state.regions[0].idleSeconds == 0.0f, "idleSeconds resets on wake");
    check(wakeTicks == 5, "woken region ticks again (5/5)");
}

void test_aggregate_evolution_and_determinism() {
    auto lodA = make_lod();
    auto lodB = make_lod();
    engine::simulation::SimulationLodState stateA;
    engine::simulation::SimulationLodState stateB;
    std::string error;
    // Cell (13, 0) center (216, 8) — aggregate tier (rel ~0.38).
    check(lodA->add_region(stateA, 13, 0, {"forest"}, error), "aggregate region A added");
    check(lodB->add_region(stateB, 13, 0, {"forest"}, error), "aggregate region B added");
    std::vector<engine::simulation::SimulationLodEvent> eventsA;
    std::vector<engine::simulation::SimulationLodEvent> eventsB;
    const float kStartPopulation = 100.0f;
    stateA.regions[0].population = kStartPopulation;
    stateB.regions[0].population = kStartPopulation;
    std::string streamA;
    std::string streamB;
    for (int i = 0; i < 400; ++i) {
        eventsA.clear();
        eventsB.clear();
        check(lodA->update(stateA, 0.0f, 0.0f, 0.01f, eventsA, error), "A update ok");
        check(lodB->update(stateB, 0.0f, 0.0f, 0.01f, eventsB, error), "B update ok");
        for (const auto& event : eventsA) {
            streamA += static_cast<char>('0' + static_cast<int>(event.kind));
        }
        for (const auto& event : eventsB) {
            streamB += static_cast<char>('0' + static_cast<int>(event.kind));
        }
    }
    // Determinism: identical event streams and identical aggregate counters.
    check(streamA == streamB, "identical event streams across instances");
    check(bit_equal(stateA.regions[0].population, stateB.regions[0].population),
          "identical aggregate population across instances");
    check(bit_equal(stateA.regions[0].resources, stateB.regions[0].resources),
          "identical aggregate resources across instances");
    check(stateA.regions[0].population > kStartPopulation, "distant region evolved (population grew)");
    check(stateA.regions[0].tier == 2, "distant region stays in the aggregate tier");
}

void test_clock_climate_seasons() {
    auto lod = make_lod();
    engine::simulation::SimulationLodState state;
    std::string error;
    check(lod->add_region(state, 0, 0, {"forest"}, error), "region added");
    // 100 updates at dt 0.01 = 1 s with dayLengthSeconds 10 -> day 0, season 0.
    std::vector<engine::simulation::SimulationLodEvent> events;
    for (int i = 0; i < 100; ++i) {
        events.clear();
        check(lod->update(state, 0.0f, 0.0f, 0.01f, events, error), "update ok");
    }
    check(state.clock.tick == 100, "clock tick advances");
    check(state.clock.day == 0 && state.clock.season == 0, "day 0 season 0 after 1 s (10 s day)");
    // 1000 more updates -> 11 s -> day 1, season 0.
    for (int i = 0; i < 1000; ++i) {
        events.clear();
        check(lod->update(state, 0.0f, 0.0f, 0.01f, events, error), "update ok");
    }
    check(state.clock.day == 1, "day 1 after 11 s");
    check(state.clock.timeOfDay >= 0.0f && state.clock.timeOfDay < 1.0f, "timeOfDay in [0, 1)");

    // Seasonal climate: the same region is colder in winter than in summer.
    engine::simulation::SimulationClimate summer;
    engine::simulation::SimulationClimate winter;
    state.clock.season = 1;  // Summer
    check(lod->climate(state, state.regions[0], summer), "summer climate queried");
    state.clock.season = 3;  // Winter
    check(lod->climate(state, state.regions[0], winter), "winter climate queried");
    check(summer.temperature > winter.temperature, "summer warmer than winter");
    check(summer.growth > winter.growth, "summer growth greater than winter growth");
    check(summer.growth >= 0.0f && summer.growth <= 1.0f, "growth in [0, 1]");
    // Deterministic: same season + same region -> same climate.
    engine::simulation::SimulationClimate repeat;
    state.clock.season = 1;
    check(lod->climate(state, state.regions[0], repeat), "climate re-queried");
    check(bit_equal(repeat.temperature, summer.temperature) &&
              bit_equal(repeat.moisture, summer.moisture) && bit_equal(repeat.growth, summer.growth),
          "climate is deterministic for the same (region, season)");
}

void test_scheduled_events() {
    auto lod = make_lod();
    engine::simulation::SimulationLodState state;
    std::string error;
    check(lod->add_region(state, 0, 0, {}, error), "region added");
    // Advance the clock so the "past" case is a REAL past (at tick 0, tick-1
    // would underflow the unsigned counter).
    std::vector<engine::simulation::SimulationLodEvent> events;
    for (int i = 0; i < 3; ++i) {
        events.clear();
        check(lod->update(state, 0.0f, 0.0f, 0.01f, events, error), "pre-update ok");
    }
    check(state.clock.tick == 3, "clock advanced to tick 3");
    const std::uint64_t fireTick = state.clock.tick + 7;  // tick 10
    check(lod->schedule_event(state, fireTick, 0, 0, "meteor", error),
          "event scheduled for the future");
    check(!lod->schedule_event(state, state.clock.tick - 1, 0, 0, "past", error),
          "past event refused");
    check(!lod->schedule_event(state, fireTick, 0, 0, "", error), "empty payload refused");
    bool fired = false;
    for (std::uint64_t i = 0; i < 8; ++i) {
        events.clear();
        check(lod->update(state, 0.0f, 0.0f, 0.01f, events, error), "update ok");
        for (const auto& event : events) {
            if (event.kind == engine::simulation::SimulationLodEvent::Kind::EventDue) {
                fired = true;
                check(event.payload == "meteor", "EventDue carries the payload");
                check(event.cellX == 0 && event.cellZ == 0, "EventDue carries the cell");
            }
        }
    }
    check(fired, "event fired at its tick");
    check(state.scheduledEvents.size() == 1 && state.scheduledEvents[0].fired,
          "fired flag persisted");
    // Persisted across serialize/deserialize.
    std::string json;
    check(lod->serialize_state(state, json, error), "state serialized");
    engine::simulation::SimulationLodState loaded;
    check(lod->deserialize_state(json, loaded, error), "state deserialized");
    check(loaded.scheduledEvents.size() == 1 && loaded.scheduledEvents[0].fired &&
              loaded.scheduledEvents[0].payload == "meteor",
          "fired event survives the round-trip");
}

void test_budgets() {
    engine::simulation::SimulationLodSpec spec = make_spec();
    spec.tiers[0].maxRegions = 2;  // full tier capped at 2 regions
    auto lod = make_lod(spec);
    engine::simulation::SimulationLodState state;
    std::string error;
    check(lod->add_region(state, 0, 0, {}, error), "region 1 added");
    check(lod->add_region(state, 0, 1, {}, error), "region 2 added");
    check(lod->add_region(state, 0, 2, {}, error), "region 3 added");
    std::vector<engine::simulation::SimulationLodEvent> events;
    check(lod->update(state, 0.0f, 0.0f, 0.01f, events, error), "update ok");
    int demoted = 0;
    int keptFull = 0;
    for (const auto& event : events) {
        if (event.kind == engine::simulation::SimulationLodEvent::Kind::RegionTierChanged &&
            event.fromTier == 0 && event.toTier == 1) {
            ++demoted;
            // The demoted region must be the farthest: cell (0, 2).
            check(event.cellX == 0 && event.cellZ == 2, "farthest region demoted");
        }
    }
    for (const auto& region : state.regions) {
        if (region.tier == 0) ++keptFull;
    }
    check(demoted == 1, "one region demoted over the full-tier budget");
    check(keptFull == 2, "two regions kept in the full tier");

    // Without a cap nothing is demoted.
    auto lodUnlimited = make_lod();
    engine::simulation::SimulationLodState stateU;
    check(lodUnlimited->add_region(stateU, 0, 0, {}, error), "u region 1");
    check(lodUnlimited->add_region(stateU, 0, 1, {}, error), "u region 2");
    check(lodUnlimited->add_region(stateU, 0, 2, {}, error), "u region 3");
    std::vector<engine::simulation::SimulationLodEvent> eventsU;
    check(lodUnlimited->update(stateU, 0.0f, 0.0f, 0.01f, eventsU, error), "unlimited update ok");
    int demotedU = 0;
    for (const auto& event : eventsU) {
        if (event.kind == engine::simulation::SimulationLodEvent::Kind::RegionTierChanged &&
            event.fromTier == 0) ++demotedU;
    }
    check(demotedU == 0, "no demotion without a budget cap");
}

void test_transition_handoff_and_reactivation() {
    auto lod = make_lod();
    engine::simulation::SimulationLodState state;
    std::string error;
    // Aggregate region far away; focus is far.
    check(lod->add_region(state, 13, 0, {"forest"}, error), "region added");
    state.regions[0].population = 100.0f;
    std::vector<engine::simulation::SimulationLodEvent> events;
    for (int i = 0; i < 300; ++i) {
        events.clear();
        check(lod->update(state, 0.0f, 0.0f, 0.01f, events, error), "far update ok");
    }
    const float evolvedPopulation = state.regions[0].population;
    const float evolvedResources = state.regions[0].resources;
    check(evolvedPopulation > 100.0f, "aggregate evolved while away");

    // The focus returns: the region leaves Aggregate for Full carrying the
    // exact counters the analytic model produced (coherent reactivation).
    float handoffPopulation = -1.0f;
    float handoffResources = -1.0f;
    for (int i = 0; i < 3; ++i) {
        events.clear();
        check(lod->update(state, 230.0f, 8.0f, 0.01f, events, error), "near update ok");
        for (const auto& event : events) {
            if (event.kind == engine::simulation::SimulationLodEvent::Kind::RegionTierChanged &&
                event.fromTier == 2 && event.toTier == 0) {
                handoffPopulation = event.population;
                handoffResources = event.resources;
            }
        }
    }
    check(handoffPopulation >= 0.0f, "Aggregate -> Full handoff emitted");
    check(bit_equal(handoffPopulation, evolvedPopulation) &&
              bit_equal(handoffResources, evolvedResources),
          "handoff carries the exact aggregate counters (coherent reactivation)");
    check(state.regions[0].tier == 0, "region resumed at the full tier");
    check(bit_equal(state.regions[0].population, evolvedPopulation),
          "region state keeps the evolved counters after reactivation");
}

void test_state_serialization() {
    auto lod = make_lod();
    engine::simulation::SimulationLodState state;
    std::string error;
    check(lod->add_region(state, 13, 0, {"forest", "arid"}, error), "region added");
    check(lod->add_region(state, 20, 5, {}, error), "second region added");
    check(lod->schedule_event(state, state.clock.tick + 3, 13, 0, "storm", error),
          "event scheduled");
    std::vector<engine::simulation::SimulationLodEvent> events;
    for (int i = 0; i < 60; ++i) {
        events.clear();
        check(lod->update(state, 0.0f, 0.0f, 0.01f, events, error), "update ok");
    }
    std::string json;
    check(lod->serialize_state(state, json, error), "state serialized");
    engine::simulation::SimulationLodState loaded;
    check(lod->deserialize_state(json, loaded, error), "state deserialized");
    std::string rejson;
    check(lod->serialize_state(loaded, rejson, error), "loaded state serialized");
    check(rejson == json, "state round-trips bit-exact");
    check(loaded.regions.size() == 2 && loaded.clock.tick == state.clock.tick,
          "clock and regions survive");
    check(loaded.regions[0].tags.size() == 2 && loaded.regions[0].tags[0] == "forest",
          "region tags survive");
    // All-or-nothing: malformed documents leave the target untouched.
    engine::simulation::SimulationLodState untouched;
    untouched.clock.tick = 42;
    check(!lod->deserialize_state("not json", untouched, error), "malformed state refused");
    check(untouched.clock.tick == 42, "target untouched on refusal");
    check(!lod->deserialize_state("{\"version\":2}", untouched, error),
          "bad version refused");
    check(!lod->deserialize_state("{\"version\":1,\"regions\":{}}", untouched, error),
          "regions must be an array");
    // Bad tier index in a loaded state is refused at update time.
    engine::simulation::SimulationLodState badTier;
    check(lod->deserialize_state(json, badTier, error), "valid state loaded for mutation");
    badTier.regions[0].tier = 99;
    events.clear();
    check(!lod->update(badTier, 0.0f, 0.0f, 0.01f, events, error),
          "update refuses a state with an out-of-spec tier");
}

void test_add_remove_and_guards() {
    auto lod = make_lod();
    engine::simulation::SimulationLodState state;
    std::string error;
    check(lod->add_region(state, 0, 0, {}, error), "region added");
    check(!lod->add_region(state, 0, 0, {}, error), "duplicate region refused");
    check(lod->remove_region(state, 0, 0), "region removed");
    check(!lod->remove_region(state, 0, 0), "missing region removal refused");
    // update refuses non-finite / negative dt.
    std::vector<engine::simulation::SimulationLodEvent> events;
    check(!lod->update(state, 0.0f, 0.0f, -0.01f, events, error), "negative dt refused");
    const float nanValue = std::nanf("");
    check(!lod->update(state, 0.0f, 0.0f, nanValue, events, error), "NaN dt refused");
    // A system without a spec refuses.
    auto bare = engine::simulation::create_simulation_lod();
    std::size_t dummyTier = 0;
    check(!bare->select_tier(0.5f, dummyTier, error), "select_tier without a spec refused");
    // Set-spec is all-or-nothing: a bad spec leaves the previous one active.
    auto lod2 = make_lod();
    engine::simulation::SimulationLodSpec bad = make_spec();
    bad.tiers[0].updateInterval = -1.0f;
    check(!lod2->set_spec(bad, error), "bad spec refused at set_spec");
    check(lod2->spec() != nullptr && lod2->spec()->tiers.size() == 4,
          "previous spec remains active after a refused set_spec");
}

void test_determinism_full_sequence() {
    auto lodA = make_lod();
    auto lodB = make_lod();
    engine::simulation::SimulationLodState stateA;
    engine::simulation::SimulationLodState stateB;
    std::string error;
    // Identical region sets, identical schedules.
    for (const auto& cell : std::vector<std::pair<std::int64_t, std::int64_t>>{
             {0, 0}, {13, 0}, {20, 5}, {-4, -2}, {9, 1}}) {
        check(lodA->add_region(stateA, cell.first, cell.second, {"forest"}, error),
              "A region added");
        check(lodB->add_region(stateB, cell.first, cell.second, {"forest"}, error),
              "B region added");
    }
    check(lodA->schedule_event(stateA, 3, 13, 0, "meteor", error), "A event scheduled");
    check(lodB->schedule_event(stateB, 3, 13, 0, "meteor", error), "B event scheduled");
    // A moving focus: approach, dwell, retreat.
    const std::vector<std::pair<float, float>> focuses = {
        {0.0f, 0.0f}, {200.0f, 0.0f}, {200.0f, 0.0f}, {0.0f, 0.0f}, {330.0f, 330.0f},
        {0.0f, 0.0f}, {0.0f, 0.0f}, {100.0f, 50.0f}};
    std::string eventsA;
    std::string eventsB;
    for (int i = 0; i < 500; ++i) {
        const auto& focus = focuses[i % focuses.size()];
        std::vector<engine::simulation::SimulationLodEvent> ea;
        std::vector<engine::simulation::SimulationLodEvent> eb;
        check(lodA->update(stateA, focus.first, focus.second, 0.01f, ea, error), "A update");
        check(lodB->update(stateB, focus.first, focus.second, 0.01f, eb, error), "B update");
        for (const auto& event : ea) {
            eventsA += std::to_string(static_cast<int>(event.kind)) + "," +
                       std::to_string(event.cellX) + "," + std::to_string(event.cellZ) + ";";
        }
        for (const auto& event : eb) {
            eventsB += std::to_string(static_cast<int>(event.kind)) + "," +
                       std::to_string(event.cellX) + "," + std::to_string(event.cellZ) + ";";
        }
    }
    check(eventsA == eventsB, "bit-identical event streams over a long moving-focus run");
    std::string jsonA;
    std::string jsonB;
    check(lodA->serialize_state(stateA, jsonA, error), "A serialized");
    check(lodB->serialize_state(stateB, jsonB, error), "B serialized");
    check(jsonA == jsonB, "bit-identical final states across instances");
}

}  // namespace

int main() {
    test_spec_validation();
    test_spec_round_trip();
    test_relevance_and_tiers();
    test_region_cells();
    test_frequency();
    test_sleeping_and_waking();
    test_aggregate_evolution_and_determinism();
    test_clock_climate_seasons();
    test_scheduled_events();
    test_budgets();
    test_transition_handoff_and_reactivation();
    test_state_serialization();
    test_add_remove_and_guards();
    test_determinism_full_sequence();
    if (g_failures == 0) {
        std::printf("[simulation-lod] ALL PASSED\n");
        return 0;
    }
    std::printf("[simulation-lod] %d FAILURE(S)\n", g_failures);
    return 1;
}
