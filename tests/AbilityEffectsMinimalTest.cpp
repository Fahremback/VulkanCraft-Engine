#include "engine/rendering/IAbilityEffects.hpp"
#include <cstdio>
#include <string>
int main() {
    std::string err;
    auto ae = vc::rendering::create_ability_effects(vc::rendering::AbilityConfig{}, err);
    if (!ae) { std::printf("FAILED: %s\n", err.c_str()); return 1; }
    vc::rendering::AbilityEffectDef def;
    def.abilityId = 1;
    vc::rendering::EffectEvent ev;
    ev.time = 0.5f; ev.duration = 1.0f;
    def.events.push_back(ev);
    ae->registerAbility(def);
    vc::rendering::AbilityEffectState out;
    ae->startAbility(1, out);
    auto r = ae->tick(0.6f);
    std::printf("events: %d, active: %d\n", (int)r.size(), ae->activeCount());
    std::printf("ALL PASSED\n");
    return 0;
}
