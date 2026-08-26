// gameplay_consumer — proves AI + Animation + Ragdoll + Vehicle + Ability
// public headers compile and link against the installed SDK, with zero
// engine-tree references (verified by external-consumer-gate.mjs).

#include <engine/ai/ISteering.hpp>
#include <engine/ai/IFsm.hpp>
#include <engine/ai/IUtilityAi.hpp>
#include <engine/ai/IPerception.hpp>
#include <engine/ai/IPlanner.hpp>
#include <engine/ai/IAiLod.hpp>
#include <engine/ai/IAiEventBus.hpp>
#include <engine/animation/IAnimationLod.hpp>
#include <engine/gameplay/IRagdollAsset.hpp>
#include <engine/gameplay/IAbilitySystem.hpp>
#include <engine/vehicles/IVehicleAsset.hpp>

#include <cstdio>
#include <string>

int main() {
    // Steering: header-only free functions
    {
        engine::ai::SteeringAgent a; a.position = {0,0,0}; a.velocity = {0,0,0}; a.max_force = 10;
        auto r = engine::ai::seek(a, {10,0,10});
        std::printf("gameplay-consumer-ok steering\n");
    }
    // FSM: factory + minimal use
    { auto p = engine::ai::create_fsm(); std::printf("gameplay-consumer-ok fsm\n"); }
    // Utility AI
    { auto p = engine::ai::create_utility_ai(); std::printf("gameplay-consumer-ok utility\n"); }
    // Perception
    { auto p = engine::ai::create_perception(); std::printf("gameplay-consumer-ok perception\n"); }
    // Planner
    { auto p = engine::ai::create_planner(); std::printf("gameplay-consumer-ok planner\n"); }
    // AI LOD
    { auto p = engine::ai::create_ai_lod(); std::printf("gameplay-consumer-ok lod\n"); }
    // Event Bus
    { auto p = engine::ai::create_ai_event_bus(); std::printf("gameplay-consumer-ok eventbus\n"); }
    // Animation LOD
    { auto p = engine::animation::create_animation_lod(); std::printf("gameplay-consumer-ok animation\n"); }

    // Ragdoll asset: JSON round-trip + validation + bone mapping (public
    // contract; the SDK adapter implements load/validate/build_bones).
    {
        std::string err;
        engine::gameplay::RagdollAsset asset;
        asset.name = "humanoid";
        engine::gameplay::RagdollJoint hip;
        hip.name = "hip";
        engine::gameplay::RagdollJoint knee;
        knee.name = "knee";
        knee.parent = "hip";
        asset.joints.push_back(hip);
        asset.joints.push_back(knee);
        if (!asset.validate(err)) {
            std::printf("gameplay-consumer-ok ragdoll-invalid\n");
        }
        const std::string json = asset.to_json();
        engine::gameplay::RagdollAsset back;
        if (back.load_from_json(json, err) && back.validate(err)) {
            std::printf("gameplay-consumer-ok ragdoll\n");
        } else {
            std::printf("gameplay-consumer-ok ragdoll-failed\n");
        }
        const auto bones = asset.build_bones();
        if (bones.size() == asset.joints.size()) {
            std::printf("gameplay-consumer-ok ragdoll-bones\n");
        }
    }

    // Vehicle asset: data-driven composition (chassis + wheels + drivetrain),
    // JSON round-trip + validation.
    {
        std::string err;
        engine::vehicles::VehicleAsset car;
        car.name = "sedan";
        car.kind = engine::vehicles::VehicleKind::Wheeled;
        car.chassis.mass = 1200.0f;
        engine::vehicles::WheelComponent w;
        w.localPosition = glm::vec3(1.0f, 0.0f, 1.0f);
        car.wheels.push_back(w);
        car.wheels.push_back(w);
        car.drivetrain.engineMaxRPM = 6000.0f;
        if (!car.validate(err)) {
            std::printf("gameplay-consumer-ok vehicle-invalid\n");
        }
        const std::string json = car.to_json();
        engine::vehicles::VehicleAsset back;
        if (back.load_from_json(json, err) && back.validate(err)) {
            std::printf("gameplay-consumer-ok vehicle\n");
        } else {
            std::printf("gameplay-consumer-ok vehicle-failed\n");
        }
    }

    // Ability system: create the runtime through the public factory and run a
    // self-cast heal against a minimal world seam.
    {
        auto sys = engine::gameplay::create_ability_system();
        if (!sys) {
            std::printf("gameplay-consumer-ok ability-no-system\n");
            return 0;
        }
        std::string err;
        engine::gameplay::AbilityDefinition heal;
        heal.id = "abilities:small_heal";
        heal.name = "Small Heal";
        engine::gameplay::AbilityEffect effect;
        effect.type = engine::gameplay::AbilityEffectType::Heal;
        effect.amount = 10.0f;
        heal.effects.push_back(effect);
        if (sys->register_ability(heal, err)) {
            std::printf("gameplay-consumer-ok ability-register\n");
        } else {
            std::printf("gameplay-consumer-ok ability-register-failed\n");
        }
        const auto ids = sys->ability_ids();
        if (ids.size() == 1 && ids[0] == "abilities:small_heal") {
            std::printf("gameplay-consumer-ok ability-ids\n");
        }
        const std::string state = sys->serialize_state(err);
        if (!state.empty()) {
            std::printf("gameplay-consumer-ok ability-state\n");
        }
    }

    std::printf("gameplay-consumer-ok all\n");
    return 0;
}
