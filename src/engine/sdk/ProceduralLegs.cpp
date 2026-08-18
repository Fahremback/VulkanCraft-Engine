// ProceduralLegs (FALTANTES §18 item 7): the ONLY TU implementing the public
// IProceduralLocomotion contract. Spider-style leg locomotion inspired by
// the minecraft-spider CONCEPTS (reference only, never compiled): emergent
// leg timing — each leg plants, then independently re-targets when the body
// carries the planted foot beyond reach*retargetFactor; there is NO gait
// clock or phase offset. The driver samples the terrain through the item-5
// seam and produces targets/landings for the item-3 IK / item-6 warper.
// Pure + deterministic (glm only); per-leg state is caller-owned.
#include "engine/animation/IProceduralLegs.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace engine {
namespace animation {

bool ProceduralLocomotionSpec::validate(std::string& errorOut) const {
    if (legs.empty()) {
        errorOut = "procedural locomotion refused: no legs";
        return false;
    }
    if (!std::isfinite(bodyHeight) || bodyHeight <= 0.0f) {
        errorOut = "procedural locomotion refused: bodyHeight must be > 0";
        return false;
    }
    if (!std::isfinite(retargetFactor) || retargetFactor <= 0.0f) {
        errorOut = "procedural locomotion refused: retargetFactor must be > 0";
        return false;
    }
    if (!std::isfinite(swingDuration) || swingDuration <= 0.0f) {
        errorOut = "procedural locomotion refused: swingDuration must be > 0";
        return false;
    }
    if (!std::isfinite(stepHeight) || stepHeight < 0.0f) {
        errorOut = "procedural locomotion refused: stepHeight must be >= 0";
        return false;
    }
    if (!std::isfinite(landingDistancePerSpeed) ||
        landingDistancePerSpeed < 0.0f ||
        !std::isfinite(minLandingDistance) || minLandingDistance < 0.0f) {
        errorOut = "procedural locomotion refused: landing distances must "
                   "be >= 0";
        return false;
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

class ProceduralLegsImpl final : public IProceduralLocomotion {
public:
    bool step(const ProceduralLocomotionSpec& spec,
              const IFootTerrainSampler& terrain, float time,
              const glm::vec3& bodyPosition, float bodyYaw,
              const glm::vec2& velocity,
              const std::vector<ProceduralLegState>& prevLegs,
              ProceduralLocomotionResult& out,
              std::string& errorOut) override {
        out.legs.clear();
        out.bodyPosition = glm::vec3(0.0f);
        out.surfaceKnown = false;
        std::string err;
        if (!spec.validate(err)) {
            errorOut = err;
            return false;
        }
        if (!std::isfinite(time) || time < 0.0f ||
            !std::isfinite(bodyPosition.x) || !std::isfinite(bodyPosition.y) ||
            !std::isfinite(bodyPosition.z) || !std::isfinite(bodyYaw) ||
            !std::isfinite(velocity.x) || !std::isfinite(velocity.y)) {
            errorOut = "procedural locomotion refused: non-finite input";
            return false;
        }
        if (!prevLegs.empty() && prevLegs.size() != spec.legs.size()) {
            errorOut = "procedural locomotion refused: leg state size "
                       "mismatch";
            return false;
        }
        const bool first = prevLegs.empty();

        // The body rides the terrain: y = surface under the body + height.
        {
            const SurfaceSample bodySurface =
                terrain.sample(bodyPosition.x, bodyPosition.z);
            out.surfaceKnown = bodySurface.known;
            out.bodyPosition = bodyPosition;
            out.bodyPosition.y =
                (bodySurface.known ? bodySurface.height : bodyPosition.y) +
                spec.bodyHeight;
        }

        out.legs.resize(spec.legs.size());
        const float speed = glm::length(velocity);

        for (std::size_t i = 0; i < spec.legs.size(); ++i) {
            const LegChainAsset& leg = spec.legs[i];
            ProceduralLegState& state = out.legs[i];
            const glm::vec3 hipWorld =
                bodyPosition + rotate_yaw(leg.hipOffset, bodyYaw);

            if (first) {
                // First step: plant every leg at its rest position ON the
                // terrain, and remember the hip-relative offset so the
                // planted target moves WITH the body until retargeting.
                const glm::vec3 restWorld =
                    bodyPosition + rotate_yaw(leg.restOffset, bodyYaw);
                const SurfaceSample s =
                    terrain.sample(restWorld.x, restWorld.z);
                state.targetWorld = restWorld;
                state.surfaceKnown = s.known;
                state.surfaceHeight = s.known ? s.height : 0.0f;
                if (s.known) state.targetWorld.y = s.height;
                state.swinging = false;
                state.swingProgress = 0.0f;
                state.swingStartTime = time;
                state.landing = state.targetWorld;
                continue;
            }

            const ProceduralLegState& prev = prevLegs[i];
            state = prev;

            // The hip-to-target horizontal distance decides retargeting.
            const glm::vec3 toTarget =
                state.targetWorld - hipWorld;
            const float horizDist = std::sqrt(toTarget.x * toTarget.x +
                                              toTarget.z * toTarget.z);
            const float reach = leg.maxReach > 0.0f
                                    ? leg.maxReach
                                    : leg.upperLength + leg.lowerLength;
            const float retargetDist = reach * spec.retargetFactor;

            if (state.swinging) {
                // Deterministic swing progress from the start time.
                state.swingProgress =
                    (time - state.swingStartTime) / spec.swingDuration;
                if (state.swingProgress >= 1.0f) {
                    state.targetWorld = state.landing;
                    state.swinging = false;
                    state.swingProgress = 0.0f;
                } else {
                    // Arc from the old planted position to the landing.
                    const float p = state.swingProgress;
                    const glm::vec3 oldPlanted = prev.targetWorld;
                    state.targetWorld =
                        glm::mix(oldPlanted, state.landing, p);
                    state.targetWorld.y +=
                        spec.stepHeight * std::sin(glm::pi<float>() * p);
                }
            } else if (horizDist > retargetDist) {
                // Out of reach: swing to a new landing ahead. The landing is
                // placed along the velocity direction (speed-scaled) plus a
                // minimum forward (+Z local); stationary bodies stride in
                // place (min only).
                const glm::vec3 forward = rotate_yaw(glm::vec3(0, 0, 1), bodyYaw);
                glm::vec3 landing =
                    state.targetWorld + glm::vec3(velocity.x, 0.0f, velocity.y) *
                                             spec.landingDistancePerSpeed +
                    forward * spec.minLandingDistance;
                const SurfaceSample ls =
                    terrain.sample(landing.x, landing.z);
                if (ls.known) landing.y = ls.height;
                state.landing = landing;
                state.swinging = true;
                state.swingProgress = 0.0f;
                state.swingStartTime = time;
                state.surfaceHeight = ls.known ? ls.height : 0.0f;
                state.surfaceKnown = ls.known;
            }
        }
        return true;
    }
};

}  // namespace

std::unique_ptr<IProceduralLocomotion> create_procedural_locomotion() {
    return std::make_unique<ProceduralLegsImpl>();
}

}  // namespace animation
}  // namespace engine
