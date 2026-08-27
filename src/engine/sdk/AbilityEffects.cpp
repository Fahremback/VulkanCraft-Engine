// AbilityEffects.cpp — Ability effects scheduling (simplified, no vectors in structs)
#include "engine/rendering/IAbilityEffects.hpp"
#include <algorithm>
#include <vector>

namespace vc::rendering {

// Internal: flat event storage per ability
struct InternalAbilityDef {
    uint64_t abilityId = 0;
    int eventCount = 0;
    EffectEvent events[32]; // maxEventsPerAbility = 32
};

struct ActiveEntry {
    uint64_t abilityId = 0;
    float elapsed = 0.0f;
    int activeEffects = 0;
    bool finished = false;
    uint8_t fired[32];
};

class AbilityEffectsImpl : public IAbilityEffects {
public:
    explicit AbilityEffectsImpl(const AbilityConfig& cfg) : cfg_(cfg) {}

    bool registerAbility(const AbilityEffectDef& def) override {
        if (defCount_ >= 32) return false;
        for (int i = 0; i < defCount_; i++) {
            if (defs_[i].abilityId == def.abilityId) return false;
        }
        InternalAbilityDef& d = defs_[defCount_++];
        d.abilityId = def.abilityId;
        d.eventCount = (int)def.events.size();
        if (d.eventCount > 32) d.eventCount = 32;
        for (int i = 0; i < d.eventCount; i++) d.events[i] = def.events[i];
        return true;
    }

    bool startAbility(uint64_t abilityId, AbilityEffectState& out) override {
        if (activeCount_ >= cfg_.maxActiveAbilities) return false;
        const InternalAbilityDef* found = nullptr;
        for (int i = 0; i < defCount_; i++) {
            if (defs_[i].abilityId == abilityId) { found = &defs_[i]; break; }
        }
        if (!found) return false;
        ActiveEntry& a = active_[activeCount_++];
        a.abilityId = abilityId;
        a.elapsed = 0;
        a.activeEffects = 0;
        a.finished = false;
        for (int i = 0; i < found->eventCount; i++) a.fired[i] = 0;
        out.abilityId = abilityId;
        out.elapsed = 0;
        out.activeEffects = 0;
        out.finished = false;
        return true;
    }

    std::vector<EffectEvent> tick(float dt) override {
        std::vector<EffectEvent> triggered;
        for (int i = 0; i < activeCount_; ) {
            ActiveEntry& a = active_[i];
            a.elapsed += dt;
            const InternalAbilityDef* def = nullptr;
            for (int j = 0; j < defCount_; j++) {
                if (defs_[j].abilityId == a.abilityId) { def = &defs_[j]; break; }
            }
            if (!def) { i++; continue; }
            for (int j = 0; j < def->eventCount; j++) {
                if (a.fired[j]) continue;
                if (a.elapsed >= def->events[j].time) {
                    a.fired[j] = 1;
                    a.activeEffects++;
                    triggered.push_back(def->events[j]);
                }
            }
            float maxDur = 0;
            bool allFired = true;
            for (int j = 0; j < def->eventCount; j++) {
                if (!def->events[j].loop) {
                    if (!a.fired[j]) allFired = false;
                    float end = def->events[j].time + def->events[j].duration;
                    if (end > maxDur) maxDur = end;
                }
            }
            if (allFired && a.elapsed >= maxDur) {
                a.finished = true;
                active_[i] = active_[--activeCount_];
            } else {
                i++;
            }
        }
        return triggered;
    }

    void stopAbility(uint64_t abilityId) override {
        for (int i = 0; i < activeCount_; i++) {
            if (active_[i].abilityId == abilityId) {
                active_[i] = active_[--activeCount_];
                return;
            }
        }
    }

    int activeCount() const override { return activeCount_; }
    AbilityConfig getConfig() const override { return cfg_; }

private:
    AbilityConfig cfg_;
    InternalAbilityDef defs_[32];
    int defCount_ = 0;
    ActiveEntry active_[16];
    int activeCount_ = 0;
};

std::unique_ptr<IAbilityEffects> create_ability_effects(const AbilityConfig& config, std::string& errorOut) {
    if (!config.validate()) { errorOut = "invalid config"; return nullptr; }
    return std::make_unique<AbilityEffectsImpl>(config);
}

} // namespace vc::rendering
