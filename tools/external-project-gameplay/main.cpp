// gameplay_consumer — proves AI + Animation public headers compile and link

#include <engine/ai/ISteering.hpp>
#include <engine/ai/IFsm.hpp>
#include <engine/ai/IUtilityAi.hpp>
#include <engine/ai/IPerception.hpp>
#include <engine/ai/IPlanner.hpp>
#include <engine/ai/IAiLod.hpp>
#include <engine/ai/IAiEventBus.hpp>
#include <engine/animation/IAnimationLod.hpp>

#include <cstdio>

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

    std::printf("gameplay-consumer-ok all\n");
    return 0;
}
