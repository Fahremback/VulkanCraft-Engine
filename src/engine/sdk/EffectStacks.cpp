// EffectStacks.cpp — adapter do contrato IEffectStacks.
// Mapa determinístico efeito → (stacks, duração restante). apply adiciona
// até maxStacks e renova se refreshOnApply; tick decreta a duração e remove
// o efeito inteiro ao zerar; estados ordenados por id.

#include "engine/gameplay/IEffectStacks.hpp"

#include <algorithm>
#include <unordered_map>

namespace engine::gameplay {

namespace {

struct Effect {
    EffectStackSpec spec;
    std::uint32_t stacks{ 0 };
    float remaining{ 0.0f };
};

}  // namespace

class EffectStacksImpl final : public IEffectStacks {
public:
    bool configure(const std::vector<EffectStackSpec>& specs,
                   std::string& errorOut) override {
        std::unordered_map<std::uint16_t, Effect> parsed;
        for (const EffectStackSpec& spec : specs) {
            if (spec.maxStacks == 0) {
                errorOut = "effect_stacks: maxStacks == 0";
                return false;
            }
            if (parsed.count(spec.effectId)) {
                errorOut = "effect_stacks: efeito duplicado";
                return false;
            }
            Effect effect;
            effect.spec = spec;
            parsed[spec.effectId] = effect;
        }
        effects_ = std::move(parsed);
        return true;
    }

    std::uint32_t apply(std::uint16_t effectId) override {
        auto found = effects_.find(effectId);
        if (found == effects_.end()) return 0;
        Effect& effect = found->second;
        if (effect.stacks < effect.spec.maxStacks) ++effect.stacks;
        if (effect.spec.refreshOnApply) {
            effect.remaining = effect.spec.durationSeconds;
        } else if (effect.remaining < effect.spec.durationSeconds) {
            effect.remaining = effect.spec.durationSeconds;
        }
        return effect.stacks;
    }

    bool refresh(std::uint16_t effectId) override {
        auto found = effects_.find(effectId);
        if (found == effects_.end()) return false;
        if (found->second.stacks == 0) return false;
        found->second.remaining = found->second.spec.durationSeconds;
        return true;
    }

    bool clear(std::uint16_t effectId) override {
        auto found = effects_.find(effectId);
        if (found == effects_.end()) return false;
        found->second.stacks = 0;
        found->second.remaining = 0.0f;
        return true;
    }

    std::vector<std::uint16_t> tick(float dt) override {
        if (dt < 0.0f) dt = 0.0f;
        std::vector<std::uint16_t> removed;
        for (auto& [id, effect] : effects_) {
            if (effect.stacks == 0) continue;
            effect.remaining -= dt;
            if (effect.remaining <= 0.0f) {
                effect.stacks = 0;
                effect.remaining = 0.0f;
                removed.push_back(id);
            }
        }
        std::sort(removed.begin(), removed.end());
        return removed;
    }

    std::vector<EffectStackState> states() const override {
        std::vector<EffectStackState> out;
        for (const auto& [id, effect] : effects_) {
            if (effect.stacks == 0) continue;
            EffectStackState state;
            state.effectId = id;
            state.stacks = effect.stacks;
            state.remainingSeconds = effect.remaining;
            out.push_back(state);
        }
        std::sort(out.begin(), out.end(),
                  [](const EffectStackState& a, const EffectStackState& b) {
                      return a.effectId < b.effectId;
                  });
        return out;
    }

    std::uint32_t stack_count(std::uint16_t effectId) const override {
        const auto found = effects_.find(effectId);
        return found == effects_.end() ? 0 : found->second.stacks;
    }

    void reset() override {
        for (auto& [id, effect] : effects_) {
            (void)id;
            effect.stacks = 0;
            effect.remaining = 0.0f;
        }
    }

private:
    std::unordered_map<std::uint16_t, Effect> effects_;
};

std::unique_ptr<IEffectStacks> create_effect_stacks() {
    return std::make_unique<EffectStacksImpl>();
}

}  // namespace engine::gameplay
