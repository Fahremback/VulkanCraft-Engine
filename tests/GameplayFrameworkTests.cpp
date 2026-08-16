#include "../src/engine/gameplay/GameplayFramework.hpp"
#include "../src/engine/gameplay/MissionSystem.hpp"
#include "../src/engine/gameplay/DialogueSystem.hpp"
#include "../src/engine/gameplay/WeaponSystem.hpp"
#include "../src/engine/gameplay/ParticleSimulation.hpp"
#include "../src/engine/gameplay/VehicleRuntime.hpp"
#include "../src/engine/gameplay/DestructionRuntime.hpp"
#include "../src/engine/audio/AudioRuntime.hpp"
#include "engine/navigation/INavigationProvider.hpp"
#include "../src/engine/physics/PhysicsRuntime.hpp"

#include <any>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

using namespace Engine::Gameplay;

namespace {

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::cerr << "Failure at line " << __LINE__ << ": " #cond << '\n';       \
            return false;                                                            \
        }                                                                            \
    } while (0)

bool near(float a, float b, float eps = 0.01f) { return std::abs(a - b) <= eps; }

// ---------------------------------------------------------------------------
// Event
// ---------------------------------------------------------------------------

bool test_event_bus() {
    EventBus bus;

    // Priority ordering: higher priority handlers run first.
    std::vector<std::string> order;
    bus.subscribe("boom", [&](const EventPayload&) {
        order.push_back("low");
        return true;
    }, 0);
    bus.subscribe("boom", [&](const EventPayload&) {
        order.push_back("high");
        return true;
    }, 100);
    const EmitResult boomResult = bus.emit("boom");
    CHECK(boomResult.handlers_called == 2 && !boomResult.canceled);
    CHECK(order.size() == 2 && order[0] == "high" && order[1] == "low");

    // std::any payload delivery.
    bool payloadOk = false;
    bus.subscribe("damage", [&](const EventPayload& payload) {
        payloadOk = std::any_cast<float>(payload) == 42.5f;
        return true;
    });
    bus.emit("damage", std::make_any<float>(42.5f));
    CHECK(payloadOk);

    // A handler returning false cancels the remaining lower-priority handlers.
    int cancelerCalls = 0;
    int lowerCalls = 0;
    bus.subscribe("cancel", [&](const EventPayload&) {
        ++cancelerCalls;
        return false;
    }, 10);
    bus.subscribe("cancel", [&](const EventPayload&) {
        ++lowerCalls;
        return true;
    }, 0);
    const EmitResult cancelResult = bus.emit("cancel");
    CHECK(cancelResult.canceled);
    CHECK(cancelResult.handlers_called == 1);
    CHECK(cancelerCalls == 1 && lowerCalls == 0);

    // Unsubscribe removes handlers.
    const uint64_t id = bus.subscribe("x", [](const EventPayload&) { return true; });
    CHECK(bus.handler_count("x") == 1);
    CHECK(bus.unsubscribe(id));
    CHECK(bus.handler_count("x") == 0);
    CHECK(!bus.unsubscribe(id));

    // Unknown events are a no-op.
    const EmitResult missing = bus.emit("does_not_exist");
    CHECK(missing.handlers_called == 0 && !missing.canceled);
    return true;
}

// ---------------------------------------------------------------------------
// Timer
// ---------------------------------------------------------------------------

bool test_timers() {
    TimerSystem timers;
    int onceFires = 0;
    int loopFires = 0;

    const uint64_t onceId = timers.schedule("once", 0.1f, [&] { ++onceFires; });
    CHECK(timers.is_active(onceId));
    CHECK(near(timers.remaining(onceId), 0.1f));

    timers.update(0.05f);
    CHECK(onceFires == 0);
    CHECK(near(timers.remaining(onceId), 0.05f));

    timers.update(0.06f);
    CHECK(onceFires == 1);
    CHECK(!timers.is_active(onceId));
    CHECK(timers.remaining(onceId) < 0.0f);   // unknown after firing

    // Looping timer fires repeatedly until canceled.
    const uint64_t loopId = timers.schedule("loop", 0.1f, [&] { ++loopFires; }, true, 0.1f);
    timers.update(0.1f);
    CHECK(loopFires == 1);
    CHECK(timers.is_active(loopId));
    timers.update(0.1f);
    CHECK(loopFires == 2);
    CHECK(timers.cancel(loopId));
    timers.update(1.0f);
    CHECK(loopFires == 2);
    CHECK(timers.active_count() == 0);

    // Named cancellation removes all timers with that name.
    timers.schedule("grp", 1.0f, [] {});
    timers.schedule("grp", 1.0f, [] {});
    CHECK(timers.cancel_named("grp") == 2);
    CHECK(timers.active_count() == 0);

    // remaining_named returns the earliest pending time.
    timers.schedule("a", 0.3f, [] {});
    timers.schedule("a", 0.1f, [] {});
    CHECK(near(timers.remaining_named("a"), 0.1f));
    CHECK(timers.remaining_named("missing") < 0.0f);
    return true;
}

// ---------------------------------------------------------------------------
// Trigger
// ---------------------------------------------------------------------------

bool test_triggers() {
    TriggerVolume volume({0.0f, 0.0f, 0.0f}, {2.0f, 1.0f, 2.0f});
    CHECK(volume.contains({1.0f, 0.5f, -1.0f}));
    CHECK(!volume.contains({3.0f, 0.0f, 0.0f}));

    // Ray test: entry face at x = -2 -> distance 3 from origin (-5,0,0).
    float dist = -1.0f;
    CHECK(volume.intersects_ray({-5.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 100.0f, dist));
    CHECK(near(dist, 3.0f));
    CHECK(!volume.intersects_ray({-5.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 2.0f, dist));
    CHECK(!volume.intersects_ray({10.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 100.0f, dist));

    // Enter / stay / exit callbacks driven by update().
    int enters = 0, stays = 0, exits = 0;
    uint64_t lastEntered = 0, lastExited = 0;
    volume.on_enter([&](uint64_t e) { lastEntered = e; ++enters; });
    volume.on_stay([&](uint64_t) { ++stays; });
    volume.on_exit([&](uint64_t e) { lastExited = e; ++exits; });

    volume.update(7, {0.0f, 0.0f, 0.0f});   // enter
    CHECK(lastEntered == 7 && enters == 1 && stays == 0 && exits == 0);
    CHECK(volume.is_inside(7) && volume.inside_count() == 1);

    volume.update(7, {1.0f, 0.0f, 0.0f});   // stay
    CHECK(enters == 1 && stays == 1);

    volume.update(7, {5.0f, 0.0f, 0.0f});   // exit
    CHECK(lastExited == 7 && exits == 1 && !volume.is_inside(7));

    volume.update(7, {0.0f, 0.0f, 0.0f});   // re-enter
    CHECK(enters == 2);

    // Layer mask: entities outside the mask are ignored entirely.
    const int entersBefore = enters;
    const int exitsBefore = exits;
    volume.set_layer_mask(0x2u);
    volume.update(7, {5.0f, 0.0f, 0.0f}, 0x1u);   // layer 1 not in mask -> no-op
    CHECK(volume.is_inside(7) && enters == entersBefore);
    volume.update(7, {5.0f, 0.0f, 0.0f}, 0x2u);   // layer 2 in mask -> exit fires
    CHECK(!volume.is_inside(7) && exits == exitsBefore + 1);
    return true;
}

// ---------------------------------------------------------------------------
// Trigger volumes wired into the event bus (README integration: volumes emit
// events when entities enter/leave).
// ---------------------------------------------------------------------------

bool test_trigger_manager_events() {
    EventBus bus;
    TriggerManager manager;
    manager.bind_event_bus(&bus);
    CHECK(manager.add("bridge_area", TriggerVolume({0.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 2.0f})));
    CHECK(manager.volume("bridge_area") != nullptr);
    CHECK(manager.count() == 1);

    int enterEvents = 0, exitEvents = 0;
    std::string enteredVolume, exitedVolume;
    uint64_t enteredEntity = 0, exitedEntity = 0;
    bus.subscribe("TriggerEnter", [&](const EventPayload& payload) {
        const TriggerEvent event = std::any_cast<TriggerEvent>(payload);
        enteredVolume = event.volume;
        enteredEntity = event.entity;
        ++enterEvents;
        return true;
    });
    bus.subscribe("TriggerExit", [&](const EventPayload& payload) {
        const TriggerEvent event = std::any_cast<TriggerEvent>(payload);
        exitedVolume = event.volume;
        exitedEntity = event.entity;
        ++exitEvents;
        return true;
    });

    manager.update(11, {1.0f, 0.0f, 0.0f});
    CHECK(enterEvents == 1 && enteredVolume == "bridge_area" && enteredEntity == 11);
    CHECK(manager.volume("bridge_area")->is_inside(11));   // inside now

    manager.update(11, {3.0f, 0.0f, 0.0f});
    CHECK(exitEvents == 1 && exitedVolume == "bridge_area" && exitedEntity == 11);

    // Same entity staying inside emits no second enter/exit pair.
    manager.update(11, {1.0f, 0.0f, 0.0f});
    manager.update(11, {1.2f, 0.0f, 0.0f});
    CHECK(enterEvents == 2 && exitEvents == 1);
    return true;
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------

bool test_interactions() {
    InteractionSystem system;
    int doorOpens = 0;
    uint64_t doorInstigator = 0;
    const uint64_t doorId = system.register_interaction("open_door", {0.0f, 0.0f, 0.0f}, 2.0f,
                                                        [&](uint64_t instigator) {
                                                            doorInstigator = instigator;
                                                            ++doorOpens;
                                                        });
    const uint64_t chestId = system.register_interaction("open_chest", {10.0f, 0.0f, 0.0f}, 1.0f,
                                                         [](uint64_t) {});
    CHECK(doorId != 0 && chestId != 0);

    // Query returns available interactions in range, sorted by distance.
    const std::vector<uint64_t> nearDoor = system.query({1.0f, 0.0f, 0.0f}, 5.0f);
    CHECK(nearDoor.size() == 1 && nearDoor[0] == doorId);
    CHECK(system.query({0.0f, 0.0f, 0.0f}, 100.0f).size() == 2);
    CHECK(system.query({50.0f, 0.0f, 0.0f}, 1.0f).empty());

    // Interact executes only when in range and available.
    CHECK(system.interact(doorId, 5, {0.0f, 0.0f, 0.0f}));
    CHECK(doorInstigator == 5 && doorOpens == 1);
    CHECK(!system.interact(doorId, 5, {5.0f, 0.0f, 0.0f}));   // out of range
    CHECK(doorOpens == 1);

    CHECK(system.set_available(doorId, false));
    CHECK(!system.interact(doorId, 5, {0.0f, 0.0f, 0.0f}));
    CHECK(system.query({0.0f, 0.0f, 0.0f}, 5.0f).empty());

    CHECK(system.unregister(doorId));
    CHECK(system.count() == 1);
    CHECK(system.find(doorId) == nullptr);
    return true;
}

// ---------------------------------------------------------------------------
// Damage
// ---------------------------------------------------------------------------

bool test_damage() {
    Damageable target(100.0f);
    int damagedEvents = 0;
    int diedEvents = 0;
    float lastDealt = 0.0f;
    target.on_damaged([&](const DamageInstance&, float dealt) {
        lastDealt = dealt;
        ++damagedEvents;
    });
    target.on_died([&](const DamageInstance&) { ++diedEvents; });

    // Plain physical damage.
    CHECK(near(target.apply_damage(30.0f, DamageType::Physical), 30.0f));
    CHECK(near(target.health(), 70.0f));
    CHECK(damagedEvents == 1 && diedEvents == 0);

    // Modifiers: fire damage x2 plus flat 5 -> 10*2+5 = 25.
    target.add_modifier({DamageType::Fire, 2.0f, 5.0f});
    CHECK(near(target.apply_damage(10.0f, DamageType::Fire), 25.0f));
    CHECK(near(target.health(), 45.0f));
    CHECK(target.remove_modifier(DamageType::Fire));
    CHECK(!target.remove_modifier(DamageType::Fire));   // already removed

    // Shield absorbs typed damage first.
    Damageable shielded(100.0f);
    shielded.add_shield({50.0f, DamageType::Physical});
    CHECK(near(shielded.apply_damage(30.0f, DamageType::Physical), 30.0f));   // fully absorbed
    CHECK(near(shielded.health(), 100.0f));
    CHECK(near(shielded.shield_amount(DamageType::Physical), 20.0f));
    CHECK(near(shielded.apply_damage(30.0f, DamageType::Physical), 30.0f));   // 20 absorbed + 10 health
    CHECK(near(shielded.health(), 90.0f));
    CHECK(near(shielded.shield_amount(DamageType::Physical), 0.0f));

    // True damage bypasses shields.
    Damageable bypassed(100.0f);
    bypassed.add_shield({100.0f, DamageType::Physical});
    CHECK(near(bypassed.apply_damage(40.0f, DamageType::True), 40.0f));
    CHECK(near(bypassed.health(), 60.0f));
    CHECK(near(bypassed.shield_amount(DamageType::Physical), 100.0f));

    // Death fires exactly once and dead targets take no damage.
    Damageable fragile(10.0f);
    fragile.on_died([&](const DamageInstance&) { ++diedEvents; });
    fragile.apply_damage(6.0f);
    fragile.apply_damage(6.0f);
    CHECK(fragile.dead());
    CHECK(diedEvents == 1);
    CHECK(near(fragile.apply_damage(5.0f), 0.0f));

    // Healing and max-health clamping.
    Damageable healer(100.0f);
    healer.apply_damage(40.0f);
    healer.heal(15.0f);
    CHECK(near(healer.health(), 75.0f));
    healer.set_max_health(50.0f);
    CHECK(near(healer.max_health(), 50.0f));
    CHECK(near(healer.health(), 50.0f));
    return true;
}

// ---------------------------------------------------------------------------
// Inventory
// ---------------------------------------------------------------------------

bool test_inventory() {
    Inventory inventory(5);

    // Stacking: id 1 maxStack 64.
    CHECK(inventory.add(1, 10, "arrow", 64) == 10);
    CHECK(inventory.count(1) == 10);
    CHECK(inventory.add(1, 60, "arrow", 64) == 60);   // fills slot to 64, 6 overflow into new slot
    CHECK(inventory.count(1) == 70);
    CHECK(inventory.used_slots() == 2);

    CHECK(inventory.add(2, 5, "potion", 16) == 5);
    CHECK(inventory.contains(2));
    const ItemStack* potion = inventory.find(2);
    CHECK(potion != nullptr && potion->name == "potion" && potion->quantity == 5);

    // Capacity: only 5 slots; the sixth distinct item is rejected.
    CHECK(inventory.add(3, 1, "gem", 16) == 1);
    CHECK(inventory.add(4, 1, "ring", 16) == 1);
    CHECK(inventory.add(5, 1, "boots", 16) == 0);
    CHECK(inventory.used_slots() == 5);

    // Remove (partial and wholesale).
    CHECK(inventory.remove(1, 20) == 20);
    CHECK(inventory.count(1) == 50);
    CHECK(inventory.remove(1, 1000) == 50);
    CHECK(inventory.count(1) == 0 && !inventory.contains(1));

    // Stack-based add.
    CHECK(inventory.add(ItemStack{9, "sword", 1, 1}));
    CHECK(inventory.count(9) == 1);

    // Transfer between inventories.
    Inventory source(8), sink(8);
    source.add(7, 5, "gold", 99);
    CHECK(source.transfer_to(sink, 7, 3) == 3);
    CHECK(source.count(7) == 2 && sink.count(7) == 3);

    // Transfer limited by the destination slot's maxStack; leftover returns.
    Inventory partialSink(1);
    partialSink.add(7, 98, "gold", 99);
    Inventory partialSource(4);
    partialSource.add(7, 10, "gold", 99);
    CHECK(partialSource.transfer_to(partialSink, 7, 5) == 1);
    CHECK(partialSource.count(7) == 9 && partialSink.count(7) == 99);

    // Transfer into a full destination transfers nothing.
    CHECK(partialSource.transfer_to(partialSink, 7, 1000) == 0);
    CHECK(partialSource.count(7) == 9);
    return true;
}

// ---------------------------------------------------------------------------
// Objective
// ---------------------------------------------------------------------------

bool test_objectives() {
    int stateChanges = 0;
    ObjectiveState lastState = ObjectiveState::Inactive;
    Objective objective("destroy_bridge", "Destroy the bridge", 3, true);
    objective.on_state_changed([&](ObjectiveState state) {
        lastState = state;
        ++stateChanges;
    });

    CHECK(objective.state() == ObjectiveState::Inactive);
    CHECK(near(objective.progress_ratio(), 0.0f));
    objective.activate();
    CHECK(objective.state() == ObjectiveState::Active);
    CHECK(stateChanges == 1 && lastState == ObjectiveState::Active);

    objective.add_progress(2);
    CHECK(objective.current_progress() == 2);
    CHECK(objective.state() == ObjectiveState::Active);
    objective.add_progress(1);
    CHECK(objective.state() == ObjectiveState::Completed);
    CHECK(objective.completed());
    CHECK(stateChanges == 2 && lastState == ObjectiveState::Completed);

    // Progress clamps at the target and auto-completes.
    Objective clamped("kills", "Kill goblins", 5, true);
    clamped.activate();
    clamped.add_progress(99);
    CHECK(clamped.current_progress() == 5 && clamped.completed());

    // Failed objectives no longer accept progress.
    Objective timed("escort", "Escort the cart", 1, true);
    timed.activate();
    timed.fail();
    CHECK(timed.state() == ObjectiveState::Failed);
    timed.add_progress(1);
    CHECK(timed.state() == ObjectiveState::Failed && timed.current_progress() == 0);

    // Required conditions gate completion even at full progress.
    Objective gated("gate", "Open the gate", 1, true);
    gated.add_required_condition("hasKey");
    gated.activate();
    gated.add_progress(1);
    CHECK(gated.state() == ObjectiveState::Active);
    gated.set_condition("hasKey", true);
    CHECK(gated.state() == ObjectiveState::Completed);

    // Tracker aggregates and queries objectives.
    ObjectiveTracker tracker;
    CHECK(tracker.add(Objective("a", "Objective A", 1, true)));
    CHECK(tracker.add(Objective("b", "Objective B", 1, true)));
    CHECK(!tracker.add(Objective("a", "duplicate", 1, true)));   // id exists -> replaced
    CHECK(tracker.count() == 2);
    CHECK(tracker.activate("a") && tracker.add_progress("a"));
    CHECK(tracker.objective("a")->completed());
    CHECK(tracker.active_objectives().empty());   // b was never activated

    CHECK(tracker.activate("b"));
    const std::vector<std::string> active = tracker.active_objectives();
    CHECK(active.size() == 1 && active[0] == "b");
    CHECK(tracker.complete("b"));
    CHECK(tracker.active_objectives().empty());
    return true;
}

// ---------------------------------------------------------------------------
// Mission: linear graph from README section 27 (vehicle -> bridge -> destroy).
// ---------------------------------------------------------------------------

bool test_mission_linear() {
    MissionSystem missions;
    ObjectiveTracker objectives;

    Mission mission("intro", "First mission", {
        start_node("start", "obj1"),
        set_objective_node("obj1", "enter_vehicle", "Enter the vehicle", 1, "wait_vehicle"),
        wait_for_event_node("wait_vehicle", "VehicleEntered", 1, "obj2"),
        set_objective_node("obj2", "reach_bridge", "Reach the bridge", 1, "wait_bridge"),
        wait_for_event_node("wait_bridge", "AreaEntered", 1, "obj3"),
        set_objective_node("obj3", "destroy_bridge", "Destroy the bridge", 1, "wait_destroy"),
        wait_for_event_node("wait_destroy", "StructureDestroyed", 1, "complete"),
        complete_mission_node("complete"),
    });
    mission.on_objective([&](const std::string& id, const std::string& text, uint32_t target) {
        objectives.add(Objective(id, text, target));
        objectives.activate(id);
    });
    int completedCalls = 0;
    int failedCalls = 0;
    mission.on_completed([&] { ++completedCalls; });
    mission.on_failed([&] { ++failedCalls; });
    CHECK(missions.register_mission(std::move(mission)));

    CHECK(missions.start("intro"));
    Mission* active = missions.mission("intro");
    CHECK(active != nullptr && active->state() == MissionState::Active);
    CHECK(active->current_node() != nullptr && *active->current_node() == "wait_vehicle");
    const Objective* firstObjective = objectives.objective("enter_vehicle");
    CHECK(firstObjective != nullptr && firstObjective->state() == ObjectiveState::Active);

    // Unrelated events are ignored; the mission keeps waiting.
    missions.dispatch_event("SomethingElse");
    CHECK(active->state() == MissionState::Active);

    // Vehicle entered -> advance to the bridge objective.
    missions.dispatch_event("VehicleEntered", std::make_any<uint64_t>(42u));
    const Objective* secondObjective = objectives.objective("reach_bridge");
    CHECK(secondObjective != nullptr && secondObjective->state() == ObjectiveState::Active);
    CHECK(active->current_node() != nullptr && *active->current_node() == "wait_bridge");

    // A trigger volume firing on enter dispatches the game event the mission
    // is waiting for (volume -> event -> mission).
    TriggerVolume bridge({0.0f, 0.0f, 0.0f}, {3.0f, 3.0f, 3.0f});
    bridge.on_enter([&](uint64_t) { missions.dispatch_event("AreaEntered"); });
    bridge.update(9, {1.0f, 0.0f, 0.0f});
    const Objective* thirdObjective = objectives.objective("destroy_bridge");
    CHECK(thirdObjective != nullptr && thirdObjective->state() == ObjectiveState::Active);
    CHECK(active->current_node() != nullptr && *active->current_node() == "wait_destroy");

    missions.dispatch_event("StructureDestroyed");
    CHECK(active->state() == MissionState::Completed);
    CHECK(completedCalls == 1 && failedCalls == 0);
    CHECK(missions.active_missions().empty());
    return true;
}

// ---------------------------------------------------------------------------
// Mission: variables, branching, spawn and item nodes.
// ---------------------------------------------------------------------------

bool test_mission_branch_and_nodes() {
    MissionSystem missions;
    std::vector<std::string> spawned;
    std::vector<std::pair<std::string, uint32_t>> itemsGiven;
    int failedCalls = 0;

    Mission branchMission("branch_demo", "Branch demo", {
        start_node("start", "set_has_key"),
        set_variable_node("set_has_key", "hasKey", MissionValue{true}, "branch"),
        branch_node("branch", "hasKey", MissionCompare::Equal, MissionValue{true},
                    "spawn", "fail"),
        spawn_entity_node("spawn", "goblin", 3, "give"),
        give_item_node("give", "sword", 1, "complete"),
        complete_mission_node("complete"),
        fail_mission_node("fail"),
    });
    branchMission.on_spawn([&](const std::string& type, uint32_t count) {
        spawned.push_back(type + ":" + std::to_string(count));
    });
    branchMission.on_give_item([&](const std::string& id, uint32_t qty) {
        itemsGiven.emplace_back(id, qty);
    });
    branchMission.on_failed([&] { ++failedCalls; });
    CHECK(missions.register_mission(std::move(branchMission)));

    CHECK(missions.start("branch_demo"));
    Mission* mission = missions.mission("branch_demo");
    CHECK(mission != nullptr && mission->state() == MissionState::Completed);
    CHECK(spawned.size() == 1 && spawned[0] == "goblin:3");
    CHECK(itemsGiven.size() == 1 && itemsGiven[0].first == "sword" &&
          itemsGiven[0].second == 1);
    CHECK(failedCalls == 0);
    CHECK(mission->variable_bool("hasKey"));

    // False branch leads to FailMission.
    MissionSystem second;
    int failed2 = 0;
    Mission losing("branch_false", "Branch false", {
        start_node("start", "set_score"),
        set_variable_node("set_score", "score", MissionValue{int64_t(2)}, "branch"),
        branch_node("branch", "score", MissionCompare::GreaterEqual, MissionValue{int64_t(5)},
                    "win", "lose"),
        complete_mission_node("win"),
        fail_mission_node("lose"),
    });
    losing.on_failed([&] { ++failed2; });
    CHECK(second.register_mission(std::move(losing)));
    CHECK(second.start("branch_false"));
    CHECK(second.mission("branch_false")->state() == MissionState::Failed);
    CHECK(failed2 == 1);

    // String variables and query helpers.
    Mission vars("vars", "Vars", {
        start_node("start", "set"),
        set_variable_node("set", "name", MissionValue{std::string("hero")}, "complete"),
        complete_mission_node("complete"),
    });
    CHECK(second.register_mission(std::move(vars)));
    CHECK(second.start("vars"));
    const MissionValue* name = second.mission("vars")->variable("name");
    CHECK(name != nullptr && std::holds_alternative<std::string>(*name));
    CHECK(second.mission("vars")->variable_string("name") == "hero");

    // Explicit failure API.
    MissionSystem third;
    Mission timed("timed", "Timed", {
        start_node("start", "wait"),
        wait_for_event_node("wait", "Never", 1, "complete"),
        complete_mission_node("complete"),
    });
    int timedFailed = 0;
    timed.on_failed([&] { ++timedFailed; });
    CHECK(third.register_mission(std::move(timed)));
    CHECK(third.start("timed"));
    CHECK(third.fail("timed"));
    CHECK(third.mission("timed")->state() == MissionState::Failed);
    CHECK(timedFailed == 1);
    return true;
}

// ---------------------------------------------------------------------------
// Dialogue: lines, condition-gated choices and branching.
// ---------------------------------------------------------------------------

bool test_dialogue() {
    DialogueGraph greeting;
    greeting.id = "greeting";
    greeting.entry = "greet";
    greeting.nodes = {
        {"greet", {"Mayor", "Welcome, hero!", "mayor_greet.wav", "wave", "cam_wide"}, {
            {"Accept Mission", "accepted", ""},
            {"Ask Question", "question", "metMayor"},
            {"Leave", "end", ""},
        }},
        {"accepted", {"Mayor", "Excellent! The mission is yours.", "", "", ""}, {}},
        {"question", {"Mayor", "The bridge was old anyway.", "", "", ""}, {}},
        {"end", {"Mayor", "Farewell.", "", "", ""}, {}},
    };

    DialogueSystem system;
    CHECK(system.register_graph(greeting));
    CHECK(system.graph("greeting") != nullptr);

    int started = 0, finished = 0, lines = 0, choices = 0;
    std::string startedId, finishedId, choiceDlg, choiceText;
    system.on_dialogue_started([&](const std::string& id) { startedId = id; ++started; });
    system.on_dialogue_finished([&](const std::string& id) { finishedId = id; ++finished; });
    system.on_line_spoken([&](const std::string&, const DialogueLine& line) {
        if (!line.text.empty()) {
            ++lines;
        }
    });
    system.on_choice_made([&](const std::string& id, const std::string& text) {
        choiceDlg = id;
        choiceText = text;
        ++choices;
    });

    CHECK(system.play("greeting"));
    CHECK(system.is_playing());
    CHECK(started == 1 && startedId == "greeting");
    CHECK(system.active_dialogue() != nullptr);
    CHECK(system.active_dialogue()->current_text() == "Welcome, hero!");
    CHECK(system.active_dialogue()->current_character() == "Mayor");
    CHECK(system.active_dialogue()->current_audio() == "mayor_greet.wav");
    CHECK(system.active_dialogue()->current_animation() == "wave");
    CHECK(system.active_dialogue()->current_camera() == "cam_wide");
    CHECK(system.active_dialogue_id() != nullptr && *system.active_dialogue_id() == "greeting");

    // Condition-gated choice is hidden until its condition is set.
    const auto available = [&] {
        return system.active_dialogue()->available_choices(
            [&](const std::string& name) { return system.condition(name); });
    };
    CHECK(available().size() == 2);
    CHECK(!system.choose("Ask Question"));   // not available yet
    system.set_condition("metMayor", true);
    CHECK(available().size() == 3);

    // Leave -> end node (no choices) -> dialogue finished, line stays readable.
    CHECK(system.choose("Leave"));
    CHECK(!system.is_playing());
    CHECK(finished == 1 && finishedId == "greeting");
    CHECK(system.active_dialogue()->current_text() == "Farewell.");
    CHECK(choices == 1 && choiceDlg == "greeting" && choiceText == "Leave");

    // Replay and branch to a follow-up node (also choice-less -> terminal).
    CHECK(system.play("greeting"));
    CHECK(system.choose("Ask Question"));
    CHECK(!system.is_playing());
    CHECK(system.active_dialogue()->current_text() == "The bridge was old anyway.");
    system.stop();   // stopping an already-finished dialogue is a no-op
    CHECK(!system.is_playing());
    CHECK(finished == 2);

    // Unknown dialogues / choices are rejected.
    CHECK(!system.play("missing"));
    CHECK(!system.choose("nope"));
    return true;
}

// ---------------------------------------------------------------------------
// Mission + Dialogue integration without circular dependency.
// ---------------------------------------------------------------------------

bool test_mission_dialogue_integration() {
    DialogueGraph quest;
    quest.id = "quest_giver";
    quest.entry = "greet";
    quest.nodes = {
        {"greet", {"Elder", "I need a hero.", "elder.wav", "talk", ""}, {
            {"Accept Mission", "accepted", ""},
            {"Leave", "end", ""},
        }},
        {"accepted", {"Elder", "You have my blessing.", "", "", ""}, {}},
        {"end", {"Elder", "Come back anytime.", "", "", ""}, {}},
    };

    DialogueSystem dialogues;
    CHECK(dialogues.register_graph(quest));

    MissionSystem missions;
    ObjectiveTracker tracker;
    Inventory inventory(8);
    std::string givenItem;
    int completed = 0;
    int failed = 0;

    Mission mission("quest", "The elder's quest", {
        start_node("start", "obj"),
        set_objective_node("obj", "talk_elder", "Talk to the elder", 1, "play"),
        play_dialogue_node("play", "quest_giver", "wait_choice"),
        wait_for_event_node("wait_choice", "DialogueChoice", 1, "give"),
        give_item_node("give", "reward", 1, "complete"),
        complete_mission_node("complete"),
    });
    mission.on_objective([&](const std::string& id, const std::string& text, uint32_t target) {
        tracker.add(Objective(id, text, target));
        tracker.activate(id);
    });
    // Mission notifies the DialogueSystem through a callback (no coupling).
    mission.on_play_dialogue([&](const std::string& dialogueId) { dialogues.play(dialogueId); });
    mission.on_give_item([&](const std::string& itemId, uint32_t qty) {
        givenItem = itemId + ":" + std::to_string(qty);
        inventory.add(1, qty, itemId, 99);
    });
    mission.on_completed([&] { ++completed; });
    mission.on_failed([&] { ++failed; });
    CHECK(missions.register_mission(std::move(mission)));

    // Dialogue choices feed events back into the mission.
    dialogues.on_choice_made([&](const std::string& dialogueId, const std::string& choiceText) {
        if (dialogueId == "quest_giver" && choiceText == "Accept Mission") {
            missions.dispatch_event("DialogueChoice");
        }
    });

    CHECK(missions.start("quest"));
    Mission* active = missions.mission("quest");
    CHECK(active != nullptr && active->state() == MissionState::Active);
    CHECK(dialogues.is_playing());   // PlayDialogue node fired the dialogue
    CHECK(dialogues.active_dialogue()->current_text() == "I need a hero.");
    const Objective* objective = tracker.objective("talk_elder");
    CHECK(objective != nullptr && objective->state() == ObjectiveState::Active);

    // A wrong choice leaves the mission waiting for the right one.
    CHECK(dialogues.choose("Leave"));
    CHECK(active->state() == MissionState::Active);
    CHECK(!dialogues.is_playing());
    CHECK(completed == 0);

    // The elder retries the conversation; accepting completes the flow.
    CHECK(dialogues.play("quest_giver"));
    CHECK(dialogues.choose("Accept Mission"));
    CHECK(dialogues.active_dialogue()->current_text() == "You have my blessing.");
    CHECK(active->state() == MissionState::Completed);
    CHECK(completed == 1 && failed == 0);
    CHECK(givenItem == "reward:1");
    CHECK(inventory.count(1) == 1);
    return true;
}

// ── Weapon runtime: hitscan raycast, ammo/reserve, reload, fire modes ──
bool test_weapon_hitscan() {
    using namespace Engine;

    // Single shot with a raycast callback that always hits at 10 units.
    WeaponDefinition def;
    def.id = UUID{ 0, 99 };
    def.name = "Rifle";
    def.fireMode = FireMode::Single;
    def.magazineSize = 5;
    def.reserveAmmo = 10;
    def.damage = 25.0f;
    def.range = 100.0f;
    WeaponRuntime weapon(std::move(def));
    weapon.set_raycast([](const glm::vec3& o, const glm::vec3& d, float maxDist)
                           -> std::optional<WeaponHit> {
        WeaponHit h;
        h.entity = UUID{ 0, 7 };
        h.position = o + d * 10.0f;
        h.normal = glm::vec3(0, 1, 0);
        h.distance = 10.0f;
        return h;
    });
    CHECK(weapon.ammo() == 5);
    CHECK(weapon.reserve() == 10);
    const glm::vec3 origin(0, 0, 0), dir(0, 0, -1);

    // trigger_pressed fires; the hit is recorded with damage; ammo drops.
    CHECK(weapon.trigger_pressed(origin, dir));
    CHECK(weapon.ammo() == 4);
    CHECK(weapon.hits().size() == 1);
    CHECK(weapon.hits().back().damage == 25.0f);
    // Spread (1°) perturbs the ray slightly, so use a small tolerance.
    CHECK(std::abs(weapon.hits().back().distance - 10.0f) < 0.05f);
    CHECK(std::abs(weapon.hits().back().position.z + 10.0f) < 0.05f);
    weapon.trigger_released();

    // Cooldown (RPM) blocks an instant second shot, then allows it after time.
    CHECK(!weapon.trigger_pressed(origin, dir));
    weapon.update(0.1f, origin, dir);
    CHECK(weapon.trigger_pressed(origin, dir));
    weapon.trigger_released();
    CHECK(weapon.ammo() == 3);

    // Empty the magazine (ammo was 3): fire the rest, then reload from reserve.
    for (int i = 0; i < 3; ++i) {
        weapon.update(0.11f, origin, dir); // clear cooldown
        CHECK(weapon.trigger_pressed(origin, dir));
        weapon.trigger_released();
    }
    CHECK(weapon.ammo() == 0);
    CHECK(!weapon.trigger_pressed(origin, dir));
    CHECK(weapon.reload());
    CHECK(weapon.reloading());
    CHECK(weapon.ammo() == 0);
    weapon.update(3.0f, origin, dir);
    CHECK(!weapon.reloading());
    CHECK(weapon.ammo() == 5);
    CHECK(weapon.reserve() == 5);

    // Automatic: holding the trigger fires once per cooldown window.
    WeaponDefinition autoDef;
    autoDef.fireMode = FireMode::Automatic;
    autoDef.roundsPerMinute = 600.0f; // 0.1s between shots
    autoDef.magazineSize = 30;
    autoDef.reserveAmmo = 0;
    WeaponRuntime autoWeapon(std::move(autoDef));
    const uint32_t before = autoWeapon.ammo();
    CHECK(autoWeapon.trigger_pressed(origin, dir));   // shot 1
    autoWeapon.update(0.25f, origin, dir);            // held: shot 2 (one per frame)
    CHECK(autoWeapon.ammo() == before - 2);
    autoWeapon.update(0.11f, origin, dir);            // held: shot 3
    CHECK(autoWeapon.ammo() == before - 3);
    autoWeapon.trigger_released();
    return true;
}

bool test_vehicle_runtime() {
    using namespace Engine;
    Physics::PhysicsRuntime world;
    Physics::BodyDesc ground;
    ground.motion = Physics::MotionType::Static;
    ground.collider.shape = Physics::BoxShape{{50.0f, 1.0f, 50.0f}};
    const auto groundBody = world.create_body(ground);
    (void)groundBody;

    Physics::BodyDesc chassis;
    chassis.motion = Physics::MotionType::Dynamic;
    chassis.mass = 1200.0f;
    chassis.position = {0.0f, 1.2f, 0.0f};
    chassis.collider.shape = Physics::BoxShape{{0.9f, 0.35f, 0.56f}};
    const auto body = world.create_body(chassis);
    CHECK(body != Physics::InvalidBody);

    std::vector<WheelDesc> wheels(4);
    const glm::vec3 locals[4] = {
        {-1.3f, -0.1f, -0.8f}, {-1.3f, -0.1f, 0.8f},
        {1.3f, -0.1f, -0.8f},  {1.3f, -0.1f, 0.8f},
    };
    for (int i = 0; i < 4; ++i) {
        wheels[i].localPosition = locals[i];
        wheels[i].steering = i < 2;
        wheels[i].driven = true;
    }
    VehicleRuntime vehicle(body, std::move(wheels));
    CHECK(vehicle.valid(world));

    // Let the suspension settle on the ground; the chassis must not be moving.
    VehicleInput idle;
    for (int i = 0; i < 180; ++i) { vehicle.update(world, 1.0f / 60.0f); }
    CHECK(vehicle.speed(world) < 1.0f);

    // Full throttle for a second: the chassis accelerates forward.
    VehicleInput drive;
    drive.throttle = 1.0f;
    for (int i = 0; i < 60; ++i) { vehicle.set_input(drive); vehicle.update(world, 1.0f / 60.0f); }
    const float driveSpeed = vehicle.speed(world);
    CHECK(driveSpeed > 2.0f);

    // Brake: forward speed drops.
    VehicleInput stop;
    stop.brake = 1.0f;
    for (int i = 0; i < 90; ++i) { vehicle.set_input(stop); vehicle.update(world, 1.0f / 60.0f); }
    CHECK(vehicle.speed(world) < driveSpeed);
    return true;
}

bool test_particle_simulation() {
    using namespace Engine;
    // An emitting emitter spawns particles at its rate; a burst adds exactly
    // the requested count; removal stops emission.
    ParticleSimulation sim(256, 12345);
    ParticleEmitterDesc desc;
    desc.rate = 20.0f;
    desc.lifetimeMin = desc.lifetimeMax = 1.0f;
    const std::size_t emitter = sim.add_emitter(desc);
    CHECK(emitter != std::size_t(-1));
    sim.emit_burst(emitter, 50);
    CHECK(sim.alive_count() == 50);
    // dt is clamped to 0.1s internally; 20 steps of 0.05s = 1.0s of emission
    // (20 more particles). The burst (1.0s life) has expired by the last step;
    // a trailing window of the rate-spawned ones is still alive.
    for (int i = 0; i < 20; ++i) sim.update(0.05f, nullptr);
    CHECK(sim.alive_count() >= 1 && sim.alive_count() <= 30);
    CHECK(sim.remove_emitter(emitter));
    const std::size_t aliveAtRemove = sim.alive_count();
    CHECK(aliveAtRemove > 0);
    for (int i = 0; i < 30; ++i) sim.update(0.1f, nullptr);  // all expire, no new spawns
    CHECK(sim.alive_count() == 0);

    // Collision: particles with collide=true bounce off a static floor.
    Physics::PhysicsRuntime world;
    Physics::BodyDesc ground;
    ground.motion = Physics::MotionType::Static;
    ground.collider.shape = Physics::BoxShape{{10.0f, 1.0f, 10.0f}};
    const auto groundBody = world.create_body(ground);
    (void)groundBody;
    ParticleSimulation colliding(64, 7);
    ParticleEmitterDesc burstDesc;
    burstDesc.rate = 0.0f;
    burstDesc.emitting = false;  // pure burst: only the 8 explicit particles
    burstDesc.speedMin = burstDesc.speedMax = 0.0f;
    burstDesc.lifetimeMin = burstDesc.lifetimeMax = 10.0f;
    burstDesc.collide = true;
    burstDesc.restitution = 0.0f;
    burstDesc.position = {0.0f, 1.0f, 0.0f};
    const std::size_t burstEmitter = colliding.add_emitter(burstDesc);
    colliding.emit_burst(burstEmitter, 8);
    for (int i = 0; i < 120; ++i) colliding.update(1.0f / 60.0f, &world);
    int resting = 0;
    for (const auto& p : colliding.particles()) {
        if (p.alive && std::abs(p.position.y - 1.0f) < 0.3f) ++resting;  // on the floor
    }
    CHECK(resting == 8);  // none fell through the static floor
    return true;
}

// Editor play-mode integration (Fase 8): destructibles take radial damage and
// fully destroy; the navigation grid routes around blocked cells and the agent
// moves toward its waypoints; the audio mixer plays a clip and drops the voice
// when the non-looping clip ends (is_active tracks it).
bool test_play_world_advanced() {
    using namespace Engine;

    // --- DestructibleRuntime ---
    Physics::PhysicsRuntime world;
    std::vector<Gameplay::DestructionChunkDesc> chunks;
    for (int i = 0; i < 4; ++i) {
        Gameplay::DestructionChunkDesc c;
        c.localPosition = glm::vec3((i % 2 == 0) ? -0.75f : 0.75f, (i < 2) ? -0.75f : 0.75f, 0.0f);
        c.halfExtents = {0.25f, 0.25f, 0.25f};
        c.health = 25.0f;
        chunks.push_back(c);
    }
    Gameplay::DestructibleRuntime destructible;
    CHECK(destructible.create(world, {0, 0, 0}, glm::quat(1, 0, 0, 0), chunks));
    CHECK(destructible.chunks().size() == 4);
    CHECK(!destructible.fully_destroyed());
    const auto events = destructible.apply_radial_damage(world, {0, 0, 0}, 3.0f, 100.0f, 5.0f);
    CHECK(!events.empty());          // chunks detached with an impulse
    CHECK(destructible.fully_destroyed());
    destructible.destroy(world);

    // --- Navigation provider (FALTANTES item 12: the legacy grid track was
    // removed — the public Recast provider is the authority) ---
    auto provider = engine::navigation::create_recast_navigation_provider();
    engine::navigation::NavmeshConfig config;
    config.boundsMinX = -1.0f;
    config.boundsMaxX = 10.0f;
    config.boundsMinZ = -1.0f;
    config.boundsMaxZ = 10.0f;
    config.cellSize = 0.5f;
    config.cellHeight = 0.2f;
    config.agentRadius = 0.4f;
    config.agentHeight = 1.8f;
    config.agentMaxClimb = 1.0f;
    std::vector<engine::navigation::VoxelColumn> columns;
    for (int gx = 0; gx < 20; ++gx) {
        for (int gz = 0; gz < 20; ++gz) {
            const float cx = -1.0f + (gx + 0.5f) * 0.5f;
            const float cz = -1.0f + (gz + 0.5f) * 0.5f;
            // Wall 2 units thick so a probe at its center (x=4) is outside
            // the walkable search extents (agentRadius*2 = 0.8).
            const bool wall = (cx >= 3.0f && cx <= 5.0f) && (cz >= 2.0f && cz <= 8.0f);
            if (wall) continue;  // omitted cells = blocked
            columns.push_back({ cx, cz, 0.0f, 1.0f, true });
        }
    }
    std::string navError;
    CHECK(provider->build(config, columns, navError));
    engine::navigation::PathResult path;
    CHECK(provider->find_path(0.5f, 1.0f, 0.5f, 9.5f, 1.0f, 9.5f, path));
    CHECK(path.found);
    CHECK(path.waypoints.size() >= 6);  // detours around the wall
    CHECK(provider->is_walkable(0.5f, 1.0f, 0.5f));
    CHECK(!provider->is_walkable(4.0f, 1.0f, 5.0f));  // inside the wall footprint

    // --- Audio Mixer voice lifecycle ---
    Audio::Mixer mixer;
    auto clip = std::make_shared<Audio::AudioClip>("tone");
    Audio::AudioBuffer buffer;
    buffer.sampleRate = 48000;
    buffer.channels = 1;
    buffer.samples.assign(480, 0.5f);   // 10 ms of tone
    clip->hot_swap(std::move(buffer));
    Audio::VoiceDescription desc;
    desc.clip = std::move(clip);
    desc.bus = mixer.master_bus();
    const Audio::VoiceId voice = mixer.play(std::move(desc));
    CHECK(mixer.is_active(voice));
    const auto mix = mixer.render(480);
    CHECK(mix.size() == 480 * 2);       // interleaved stereo out
    mixer.render(480);                   // next block hits the clip end
    CHECK(!mixer.is_active(voice));      // non-looping clip finished -> dropped
    return true;
}

} // namespace

int main() {
    if (!test_event_bus()) { return EXIT_FAILURE; }
    if (!test_weapon_hitscan()) { return EXIT_FAILURE; }
    if (!test_vehicle_runtime()) { return EXIT_FAILURE; }
    if (!test_particle_simulation()) { return EXIT_FAILURE; }
    if (!test_timers()) { return EXIT_FAILURE; }
    if (!test_triggers()) { return EXIT_FAILURE; }
    if (!test_trigger_manager_events()) { return EXIT_FAILURE; }
    if (!test_interactions()) { return EXIT_FAILURE; }
    if (!test_damage()) { return EXIT_FAILURE; }
    if (!test_inventory()) { return EXIT_FAILURE; }
    if (!test_objectives()) { return EXIT_FAILURE; }
    if (!test_mission_linear()) { return EXIT_FAILURE; }
    if (!test_mission_branch_and_nodes()) { return EXIT_FAILURE; }
    if (!test_dialogue()) { return EXIT_FAILURE; }
    if (!test_mission_dialogue_integration()) { return EXIT_FAILURE; }
    if (!test_play_world_advanced()) { return EXIT_FAILURE; }
    std::cout << "GameplayFrameworkTests: all tests passed\n";
    return EXIT_SUCCESS;
}
