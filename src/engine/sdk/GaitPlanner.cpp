// GaitPlanner (FALTANTES §18 item 4): the ONLY TU implementing the public
// IContactPlanner contract. Pure, deterministic locomotion planning —
// `ContactPlanner` + `GaitAsset` + leg-chain assets for arbitrary creatures.
// No external dependency (composição das fachadas públicas — no new vendored
// backend). The planner assumes constant horizontal velocity/yaw over the
// stance period (planted feet stay fixed in world space while the body
// advances); terrain-aware placement is FALTANTES §18 item 5.
#include "engine/animation/IGaitPlanner.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace engine {
namespace animation {

bool LegChainAsset::validate(std::string& errorOut) const {
    if (upperLength <= 0.0f || !std::isfinite(upperLength) ||
        lowerLength <= 0.0f || !std::isfinite(lowerLength)) {
        errorOut = "leg chain '" + name + "' bone lengths must be > 0";
        return false;
    }
    if (!std::isfinite(hipOffset.x) || !std::isfinite(hipOffset.y) ||
        !std::isfinite(hipOffset.z) || !std::isfinite(restOffset.x) ||
        !std::isfinite(restOffset.y) || !std::isfinite(restOffset.z) ||
        !std::isfinite(maxReach) || maxReach < 0.0f) {
        errorOut = "leg chain '" + name +
                   "' offsets must be finite and maxReach >= 0";
        return false;
    }
    if ((hipBone >= 0 && (hipBone == kneeBone || hipBone == footBone)) ||
        (kneeBone >= 0 && kneeBone == footBone)) {
        errorOut = "leg chain '" + name +
                   "' bone indices must be distinct when set";
        return false;
    }
    return true;
}

bool GaitAsset::validate(std::string& errorOut) const {
    if (cycleDuration <= 0.0f || !std::isfinite(cycleDuration)) {
        errorOut = "gait '" + name + "' cycleDuration must be > 0";
        return false;
    }
    if (stanceFraction <= 0.0f || stanceFraction >= 1.0f ||
        !std::isfinite(stanceFraction)) {
        errorOut = "gait '" + name + "' stanceFraction must be in (0, 1)";
        return false;
    }
    if (stepHeight < 0.0f || !std::isfinite(stepHeight)) {
        errorOut = "gait '" + name + "' stepHeight must be >= 0";
        return false;
    }
    if (maxStride <= 0.0f || !std::isfinite(maxStride)) {
        errorOut = "gait '" + name + "' maxStride must be > 0";
        return false;
    }
    if (legs.empty()) {
        errorOut = "gait '" + name + "' must define at least one leg";
        return false;
    }
    if (legPhases.size() != legs.size()) {
        errorOut = "gait '" + name + "' legPhases size (" +
                   std::to_string(legPhases.size()) + ") must match legs (" +
                   std::to_string(legs.size()) + ")";
        return false;
    }
    for (const float phase : legPhases) {
        if (!std::isfinite(phase) || phase < 0.0f || phase >= 1.0f) {
            errorOut = "gait '" + name + "' phase offsets must be in [0, 1)";
            return false;
        }
    }
    for (const LegChainAsset& leg : legs) {
        std::string err;
        if (!leg.validate(err)) {
            errorOut = err;
            return false;
        }
    }
    return true;
}

namespace {

// Rotates a body-space offset into world space for a yaw about +Y.
glm::vec3 rotate_yaw(const glm::vec3& v, float yaw) {
    const float c = std::cos(yaw);
    const float s = std::sin(yaw);
    return glm::vec3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

class ContactPlannerImpl final : public IContactPlanner {
public:
    bool plan(const GaitAsset& gait, float time, const glm::vec3& bodyPosition,
              float bodyYaw, const glm::vec2& velocity, GaitPlan& outPlan,
              std::string& errorOut) override {
        outPlan.feet.clear();
        std::string err;
        if (!gait.validate(err)) {
            errorOut = err;
            return false;
        }
        if (!std::isfinite(time) || time < 0.0f ||
            !std::isfinite(bodyPosition.x) || !std::isfinite(bodyPosition.y) ||
            !std::isfinite(bodyPosition.z) || !std::isfinite(bodyYaw) ||
            !std::isfinite(velocity.x) || !std::isfinite(velocity.y)) {
            errorOut = "gait plan refused: non-finite input (time, body, yaw "
                       "or velocity)";
            return false;
        }
        const float cycle = gait.cycleDuration;
        const float stanceTime = cycle * gait.stanceFraction;
        const float swingTime = cycle - stanceTime;
        const float speed = glm::length(velocity);
        // Stride per step: the distance the body covers in one cycle, capped
        // at the asset's maxStride. Direction = body velocity (a stationary
        // body strides in place).
        const glm::vec2 strideDir =
            speed > 1e-5f ? velocity / speed : glm::vec2(0.0f);
        const glm::vec2 strideVec = strideDir * std::min(speed * cycle, gait.maxStride);

        outPlan.feet.resize(gait.legs.size());
        for (std::size_t i = 0; i < gait.legs.size(); ++i) {
            const LegChainAsset& leg = gait.legs[i];
            float tau = std::fmod(time / cycle + gait.legPhases[i], 1.0f);
            if (tau < 0.0f) tau += 1.0f;
            const float cycleTime = tau * cycle;
            const bool stance = cycleTime < stanceTime;
            // The foot is planted at the hip's position at the START of the
            // current stance, back-projected by the elapsed stance time
            // (constant-velocity assumption) and the rest offset. It stays
            // fixed in world space for the whole stance.
            const float stanceElapsed = stance ? cycleTime : stanceTime;
            const glm::vec3 planted =
                bodyPosition -
                glm::vec3(velocity.x * stanceElapsed, 0.0f,
                          velocity.y * stanceElapsed) +
                rotate_yaw(leg.restOffset, bodyYaw);
            const glm::vec3 landing =
                planted + glm::vec3(strideVec.x, 0.0f, strideVec.y);

            FootPlan& f = outPlan.feet[i];
            f.legIndex = static_cast<int>(i);
            f.phase = tau;
            f.stance = stance;
            f.landing = landing;
            if (stance) {
                f.targetWorld = planted;
                f.lift = 0.0f;
            } else {
                const float progress =
                    std::min((cycleTime - stanceTime) / swingTime, 1.0f);
                f.targetWorld = glm::mix(planted, landing, progress);
                f.lift = gait.stepHeight * std::sin(glm::pi<float>() * progress);
                f.targetWorld.y += f.lift;
            }
            const glm::vec3 hipWorld =
                bodyPosition + rotate_yaw(leg.hipOffset, bodyYaw);
            const float reach = leg.maxReach > 0.0f
                                    ? leg.maxReach
                                    : leg.upperLength + leg.lowerLength;
            f.withinReach =
                glm::distance(hipWorld, f.targetWorld) <= reach + 1e-3f;
        }
        return true;
    }
};

}  // namespace

std::unique_ptr<IContactPlanner> create_contact_planner() {
    return std::make_unique<ContactPlannerImpl>();
}

}  // namespace animation
}  // namespace engine
