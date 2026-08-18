#pragma once

// Public locomotion contract (FALTANTES §18 item 4): `ContactPlanner` +
// `GaitAsset` + leg-chain assets for ARBITRARY creatures. A creature is a set
// of leg chains (hip-anchored two-bone chains hip -> knee -> foot) plus a
// data-driven gait (cycle timing + per-leg phase offsets). The contact
// planner maps the body state + the gait clock to per-foot targets (planted
// feet stay fixed in world space during stance; swinging feet arc toward the
// next landing). The caller solves each chain with the animation IK
// (IMotionDatabase::ik_two_bone) — this contract composes with it.
//
// The planner is PURE and DETERMINISTIC: identical inputs produce identical
// plans (no hidden state). It assumes constant horizontal velocity and yaw
// over the stance period (the foot is back-projected to the hip position at
// the start of the stance); terrain-aware placement is FALTANTES §18 item 5.
//
// Self-contained (glm only). The ONLY TU that implements the contract is the
// SDK adapter (src/engine/sdk/GaitPlanner.cpp) — the adapter rule.

#include <glm/glm.hpp>

#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace animation {

// One leg of an arbitrary creature: a hip-anchored two-bone chain
// (hip -> knee -> foot) plus the foot's rest position. The bone indices bind
// the chain to the creature skeleton (canonical order); -1 = unbound.
struct LegChainAsset {
    std::string name;
    // Hip joint offset in BODY space (the creature origin / root joint).
    glm::vec3 hipOffset{0.0f};
    // Upper (hip->knee) and lower (knee->foot) bone lengths.
    float upperLength{0.5f};
    float lowerLength{0.5f};
    // Foot rest offset in body space (where the foot stands at rest).
    glm::vec3 restOffset{0.0f, -1.0f, 0.0f};
    // Max horizontal reach from the hip; 0 = auto (upper + lower).
    float maxReach{0.0f};
    // Canonical skeleton bone indices (hip, knee, foot); -1 = unbound.
    int hipBone{-1};
    int kneeBone{-1};
    int footBone{-1};

    // All-or-nothing: bone lengths > 0, finite offsets/reach (reach >= 0),
    // distinct bone indices when set.
    bool validate(std::string& errorOut) const;
};

// A data-driven gait: the cycle timing and per-leg phase offsets that define
// how an arbitrary creature walks/trots/ambles.
struct GaitAsset {
    std::string name;
    // Duration of one full stride cycle (seconds); > 0.
    float cycleDuration{1.0f};
    // Stance fraction of the cycle (0..1); the rest is swing. In (0, 1).
    float stanceFraction{0.6f};
    // Vertical foot lift during swing (>= 0).
    float stepHeight{0.25f};
    // Max horizontal stride per step; > 0.
    float maxStride{0.5f};
    // Per-leg phase offset (fraction of the cycle, [0, 1)); size == legs.
    std::vector<float> legPhases;
    // The leg chains, one per leg; non-empty.
    std::vector<LegChainAsset> legs;

    // All-or-nothing: cycle > 0, stanceFraction in (0, 1), stepHeight >= 0,
    // maxStride > 0, non-empty legs, phases sized to match and in [0, 1),
    // every leg validates.
    bool validate(std::string& errorOut) const;
};

// One foot's plan at the current time.
struct FootPlan {
    int legIndex{0};
    // Phase within the cycle [0, 1).
    float phase{0.0f};
    // True when the foot is planted (stance), false when swinging.
    bool stance{false};
    // The foot's desired WORLD position now: planted feet stay fixed on the
    // ground; swinging feet arc toward the landing point.
    glm::vec3 targetWorld{0.0f};
    // Vertical lift applied during swing (0 during stance).
    float lift{0.0f};
    // The predicted landing point (next stance position), world space.
    glm::vec3 landing{0.0f};
    // True when the target is within the leg's reach (|hip - target| <=
    // reach), so the leg chain can actually be solved to it.
    bool withinReach{true};
};

struct GaitPlan {
    std::vector<FootPlan> feet;
};

// The contact planner: maps the creature's body state + gait clock to
// per-foot targets. Pure and deterministic (no internal state).
class IContactPlanner {
public:
    virtual ~IContactPlanner() = default;

    // Computes the foot plan for the current gait phase. `time` is the gait
    // clock (seconds, >= 0). `bodyPosition` is the body origin in world space
    // (Y-up), `bodyYaw` the heading (radians about Y), `velocity` the
    // horizontal body velocity (m/s, drives the stride length). All-or-
    // nothing: an invalid gait or non-finite input is refused with a
    // diagnostic and `outPlan` is cleared.
    virtual bool plan(const GaitAsset& gait, float time,
                      const glm::vec3& bodyPosition, float bodyYaw,
                      const glm::vec2& velocity, GaitPlan& outPlan,
                      std::string& errorOut) = 0;
};

// The factory: builds the contact planner (the ONLY adapter TU).
std::unique_ptr<IContactPlanner> create_contact_planner();

}  // namespace animation
}  // namespace engine
