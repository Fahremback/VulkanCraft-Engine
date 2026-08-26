// HitReaction.cpp — adapter do contrato IHitReaction (engine::gameplay).
// Máquina de estados determinística: Normal → Stagger (impacto) → Down
// (knockdown, quando o knockback vence a gravidade/limiar) → Recovering
// (após downDuration no chão) → Normal. Knockback decai exponencialmente.

#include "engine/gameplay/IHitReaction.hpp"

#include <cmath>

namespace engine::gameplay {

namespace {

// Fração mínima da velocidade inicial que caracteriza "ainda em movimento"
// durante o primeiro tick do stagger (usada para não derrubar por um
// impacto residual já quase decaído).
constexpr float kMinKnockbackFraction = 0.5f;

}  // namespace

class HitReactionImpl final : public IHitReaction {
public:
    void set_config(const HitReactionConfig& config) override { config_ = config; }
    HitReactionConfig config() const override { return config_; }

    void apply_impact(float strength) override {
        const float clamped = strength < 0.0f ? 0.0f : (strength > 1.0f ? 1.0f : strength);
        state_.knockbackVelocity = config_.knockbackInitial * clamped;
        pendingKnockdown_ = clamped >= config_.knockdownStrength;
        if (state_.state == HitState::Normal || state_.state == HitState::Recovering) {
            state_.state = HitState::Stagger;
            state_.timeInState = 0.0f;
        } else if (state_.state == HitState::Down) {
            state_.timeInState = 0.0f;  // impacto prolonga o chão
        }
        // Stagger já ativo: impacto novo apenas reinicia o timer (sem
        // empilhar velocidade — o decaimento já cuidou do resto).
    }

    HitReactionState update(float dt, bool grounded) override {
        if (dt < 0.0f) dt = 0.0f;
        state_.grounded = grounded;
        state_.timeInState += dt;

        switch (state_.state) {
        case HitState::Normal:
            state_.knockbackVelocity = 0.0f;
            break;
        case HitState::Stagger: {
            // Decaimento exponencial do knockback.
            state_.knockbackVelocity *= std::exp(-config_.knockbackDecay * dt);
            // Knockdown: impacto forte e impulso ainda significativo.
            const bool knockedDown =
                pendingKnockdown_ &&
                state_.knockbackVelocity >= config_.knockbackInitial * kMinKnockbackFraction;
            pendingKnockdown_ = false;
            if (knockedDown) {
                state_.state = HitState::Down;
                state_.timeInState = 0.0f;
            } else if (state_.timeInState >= config_.staggerDuration) {
                state_.state = HitState::Normal;
                state_.timeInState = 0.0f;
                state_.knockbackVelocity = 0.0f;
            }
            break;
        }
        case HitState::Down:
            state_.knockbackVelocity = 0.0f;
            if (grounded && state_.timeInState >= config_.downDuration) {
                state_.state = HitState::Recovering;
                state_.timeInState = 0.0f;
            }
            break;
        case HitState::Recovering:
            state_.knockbackVelocity = 0.0f;
            if (state_.timeInState >= config_.recoverDuration) {
                state_.state = HitState::Normal;
                state_.timeInState = 0.0f;
            }
            break;
        }
        return state_;
    }

    HitReactionState state() const override { return state_; }
    void reset() override {
        state_ = HitReactionState{};
        pendingKnockdown_ = false;
    }

private:
    HitReactionConfig config_;
    HitReactionState state_;
    bool pendingKnockdown_{ false };
};

std::unique_ptr<IHitReaction> create_hit_reaction() {
    return std::make_unique<HitReactionImpl>();
}

}  // namespace engine::gameplay
