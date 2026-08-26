// AgentCapabilities.cpp — adapter do contrato IAgentCapabilities.
// Escada determinística de capacidades: tenta Walk, depois Jump, Climb,
// Swim, Fly — retorna a MENOR que torna o trecho possível. Cada capacidade
// checa exatamente a geometria que lhe diz respeito; se nenhuma vence,
// possible=false com o motivo do gargalo.

#include "engine/navigation/IAgentCapabilities.hpp"

#include <cmath>

namespace engine::navigation {

namespace {

bool feasible_walk(const AgentProfile& p, const TraversalGeometry& g) {
    if (g.horizontalGap > 0.01f) return false;                      // vão
    if (g.stepUp > p.maxClimb + 1.0e-5f) return false;              // degrau alto
    if (g.slopeDegrees > p.maxSlopeDegrees + 1.0e-5f) return false; // íngreme
    if (g.waterDepth > 0.01f) return false;                         // água
    if (g.ceilingClearance > 0.0f && g.ceilingClearance < p.height) return false;
    return true;
}

bool feasible_jump(const AgentProfile& p, const TraversalGeometry& g) {
    if (!p.canJump) return false;
    if (g.horizontalGap > p.jumpDistance + 1.0e-5f) return false;
    if (g.stepUp > p.jumpHeight + 1.0e-5f) return false;
    // Salto não pousa em rampa íngreme (sem área de pouso andável).
    if (g.slopeDegrees > p.maxSlopeDegrees + 1.0e-5f) return false;
    if (g.waterDepth > 0.01f) return false;  // salto não atravessa água funda
    if (g.ceilingClearance > 0.0f && g.ceilingClearance < p.height) return false;
    return true;
}

bool feasible_climb(const AgentProfile& p, const TraversalGeometry& g) {
    if (!p.canClimb) return false;
    if (g.horizontalGap > p.jumpDistance + 1.0e-5f) return false;  // escalada não cruza vão
    if (g.stepUp > p.climbHeight + 1.0e-5f) return false;          // parede própria
    if (g.waterDepth > 0.01f) return false;
    if (g.ceilingClearance > 0.0f && g.ceilingClearance < p.height) return false;
    return true;
}

bool feasible_swim(const AgentProfile& p, const TraversalGeometry& g) {
    if (!p.canSwim) return false;
    if (g.waterDepth <= 0.01f) return false;  // nadar exige água
    if (g.waterDepth > p.swimDepth + 1.0e-5f) return false;
    return true;  // água cobre gap/degrau/inclinação (flutua)
}

bool feasible_fly(const AgentProfile& p, const TraversalGeometry& g) {
    if (!p.canFly) return false;
    (void)g;  // voo ignora geometria de superfície
    return true;
}

}  // namespace

class AgentCapabilitiesImpl final : public IAgentCapabilities {
public:
    void set_profile(const AgentProfile& profile) override { profile_ = profile; }
    AgentProfile profile() const override { return profile_; }

    TraversalResult can_traverse(const TraversalGeometry& geometry) const override {
        TraversalResult result;
        if (feasible_walk(profile_, geometry)) {
            result.possible = true;
            result.capability = MoveCapability::Walk;
            result.reason = "walk";
            return result;
        }
        if (feasible_jump(profile_, geometry)) {
            result.possible = true;
            result.capability = MoveCapability::Jump;
            result.reason = "jump";
            return result;
        }
        if (feasible_climb(profile_, geometry)) {
            result.possible = true;
            result.capability = MoveCapability::Climb;
            result.reason = "climb";
            return result;
        }
        if (feasible_swim(profile_, geometry)) {
            result.possible = true;
            result.capability = MoveCapability::Swim;
            result.reason = "swim";
            return result;
        }
        if (feasible_fly(profile_, geometry)) {
            result.possible = true;
            result.capability = MoveCapability::Fly;
            result.reason = "fly";
            return result;
        }
        result.possible = false;
        result.reason = diagnose(geometry);
        return result;
    }

private:
    const char* diagnose(const TraversalGeometry& g) const {
        if (g.horizontalGap > profile_.jumpDistance + 1.0e-5f)
            return "gap além do salto";
        if (g.stepUp > profile_.jumpHeight + 1.0e-5f)
            return "degrau além do salto";
        if (g.waterDepth > 0.01f && g.waterDepth > profile_.swimDepth + 1.0e-5f)
            return "água além da natação";
        if (g.stepUp > profile_.climbHeight + 1.0e-5f && !profile_.canFly)
            return "parede além da escalada";
        if (g.slopeDegrees > profile_.maxSlopeDegrees + 1.0e-5f && !profile_.canClimb)
            return "inclinação além do limite (sem escalada)";
        if (g.waterDepth > 0.01f && !profile_.canSwim && !profile_.canFly)
            return "água sem capacidade de nadar/voar";
        if (g.ceilingClearance > 0.0f && g.ceilingClearance < profile_.height)
            return "teto abaixo da altura do agente";
        return "sem capacidade de movimento";
    }

    AgentProfile profile_;
};

std::unique_ptr<IAgentCapabilities> create_agent_capabilities() {
    return std::make_unique<AgentCapabilitiesImpl>();
}

}  // namespace engine::navigation
