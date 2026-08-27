#pragma once
// IAbilityEffects.hpp — Headless ability effects: particle/audio/animation/lighting hooks
// Defines the logic and scheduling of ability VFX without rendering.
// No GPU, no audio device, no animation rigging required.

#include <memory>
#include <cstdint>
#include <string>
#include <vector>

namespace vc::rendering {

enum class EffectType : uint8_t {
    Particle = 0,
    Audio = 1,
    Animation = 2,
    Light = 3,
    ScreenShake = 4,
    Custom = 255
};

struct EffectEvent {
    uint64_t abilityId = 0;
    uint32_t effectIndex = 0;
    EffectType type = EffectType::Particle;
    float time = 0.0f;           // trigger time relative to ability start
    float duration = 1.0f;       // effect duration
    float intensity = 1.0f;      // 0..1 strength
    float param0 = 0.0f;         // type-specific parameter
    float param1 = 0.0f;
    float param2 = 0.0f;
    bool loop = false;
};

struct AbilityEffectDef {
    uint64_t abilityId = 0;
    std::vector<EffectEvent> events;
};

struct AbilityEffectState {
    uint64_t abilityId = 0;
    float elapsed = 0.0f;
    int activeEffects = 0;
    bool finished = false;
};

struct AbilityConfig {
    int maxActiveAbilities = 16;
    int maxEventsPerAbility = 32;

    bool validate() const { return maxActiveAbilities > 0 && maxEventsPerAbility > 0; }
    std::string toJson() const {
        return "{\"maxActiveAbilities\":" + std::to_string(maxActiveAbilities)
            + ",\"maxEventsPerAbility\":" + std::to_string(maxEventsPerAbility) + "}";
    }
    static AbilityConfig fromJson(const std::string& s) {
        AbilityConfig c;
        auto p = s.find("\"maxActiveAbilities\":");
        if (p != std::string::npos) c.maxActiveAbilities = std::stoi(s.substr(p + 22));
        p = s.find("\"maxEventsPerAbility\":");
        if (p != std::string::npos) c.maxEventsPerAbility = std::stoi(s.substr(p + 22));
        return c;
    }
};

class IAbilityEffects {
public:
    virtual ~IAbilityEffects() = default;

    // Register an ability definition
    virtual bool registerAbility(const AbilityEffectDef& def) = 0;

    // Start an ability
    virtual bool startAbility(uint64_t abilityId, AbilityEffectState& out) = 0;

    // Tick all active abilities, return triggered events
    virtual std::vector<EffectEvent> tick(float dt) = 0;

    // Stop an ability
    virtual void stopAbility(uint64_t abilityId) = 0;

    // Get count of active abilities
    virtual int activeCount() const = 0;

    // Get config
    virtual AbilityConfig getConfig() const = 0;
};

std::unique_ptr<IAbilityEffects> create_ability_effects(const AbilityConfig& config, std::string& errorOut);

} // namespace vc::rendering
