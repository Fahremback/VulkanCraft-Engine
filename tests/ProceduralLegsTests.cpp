// ProceduralLegsTests (FALTANTES §18 item 7): proves the public procedural
// locomotion contract `IProceduralLocomotion` — spider-style leg locomotion
// inspired by the minecraft-spider CONCEPTS (reference only, never compiled,
// like shape-ml). The distinguishing idea vs the phase-locked gait planner
// (item 4): EMERGENT leg timing — each leg plants, then independently
// re-targets when the body carries the planted foot beyond reach *
// retargetFactor; there is no gait clock or per-leg phase. The driver samples
// the terrain through the item-5 seam (in-test samplers here — the voxel
// sampler is proven by foot_placement_tests) and is pure + deterministic.
// The gate also proves the composition with the item-3 IK: a planted leg
// target is solved by IMotionDatabase::ik_two_bone, landing the foot on it.
#include "engine/animation/IProceduralLegs.hpp"
#include "engine/animation/IMotionDatabase.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::printf("[legs] FAIL: %s\n", message);
        ++g_failures;
    }
}

void checkClose(float a, float b, float eps, const char* message) {
    if (std::fabs(a - b) > eps) {
        std::printf("[legs] FAIL: %s (%.6f vs %.6f)\n", message, a, b);
        ++g_failures;
    }
}

void checkVec(const glm::vec3& a, const glm::vec3& b, float eps,
              const char* message) {
    if (glm::distance(a, b) > eps) {
        std::printf("[legs] FAIL: %s (%.4f,%.4f,%.4f vs %.4f,%.4f,%.4f)\n",
                    message, a.x, a.y, a.z, b.x, b.y, b.z);
        ++g_failures;
    }
}

// ---------------------------------------------------------------------------
// In-test terrain samplers (the item-5 seam accepts any implementation).
// ---------------------------------------------------------------------------

// Flat terrain at height 0.
class FlatTerrain final : public engine::animation::IFootTerrainSampler {
public:
    engine::animation::SurfaceSample sample(float, float) const override {
        return engine::animation::SurfaceSample{true, 0.0f};
    }
};

// A step: height 0.5 for worldX > 0.25, 0 otherwise.
class StepTerrain final : public engine::animation::IFootTerrainSampler {
public:
    engine::animation::SurfaceSample sample(float x, float) const override {
        return engine::animation::SurfaceSample{true, x > 0.25f ? 0.5f : 0.0f};
    }
};

// Never known (unloaded chunks / missing heightmap).
class UnknownTerrain final : public engine::animation::IFootTerrainSampler {
public:
    engine::animation::SurfaceSample sample(float, float) const override {
        return engine::animation::SurfaceSample{false, 0.0f};
    }
};

// ---------------------------------------------------------------------------
// The spider: 8 hip-anchored two-bone legs (upper 1.0 + lower 1.0, reach 2.0)
// at body height 0.6. Four per side at z = -0.9 .. 0.9, each leg's rest
// offset directly below its hip (foot drops 0.8) so the initial hip-to-target
// distance is 0 and retargeting is triggered purely by body travel.
// ---------------------------------------------------------------------------
engine::animation::ProceduralLocomotionSpec make_spider() {
    engine::animation::ProceduralLocomotionSpec spec;
    spec.bodyHeight = 0.6f;
    spec.retargetFactor = 0.75f;   // retarget at 2.0 * 0.75 = 1.5 m travel
    spec.swingDuration = 0.35f;
    spec.stepHeight = 0.2f;
    spec.landingDistancePerSpeed = 0.15f;
    spec.minLandingDistance = 0.3f;
    const float xs[2] = {0.6f, -0.6f};
    for (int side = 0; side < 2; ++side) {
        for (int i = 0; i < 4; ++i) {
            const float z = -0.9f + static_cast<float>(i) * 0.6f;
            engine::animation::LegChainAsset leg;
            leg.name = std::string(side == 0 ? "R" : "L") +
                       std::to_string(i + 1);
            leg.hipOffset = glm::vec3(xs[side], 0.6f, z);
            leg.upperLength = 1.0f;
            leg.lowerLength = 1.0f;
            // Rest directly below the hip: initial hip-to-target = 0.
            leg.restOffset = leg.hipOffset + glm::vec3(0.0f, -0.8f, 0.0f);
            const int base = 1 + (side * 4 + i) * 3;
            leg.hipBone = base;
            leg.kneeBone = base + 1;
            leg.footBone = base + 2;
            spec.legs.push_back(leg);
        }
    }
    return spec;
}

// The same creature as a canonical MotionSkeleton (children after parents):
// root + 8 chains (hip at hipOffset, knee/foot below).
engine::animation::MotionSkeleton make_spider_skeleton() {
    engine::animation::MotionSkeleton sk;
    sk.name = "spider";
    auto add = [&sk](const std::string& name, int parent, glm::vec3 t) {
        engine::animation::MotionBone b;
        b.name = name;
        b.parent = parent;
        b.localTranslation = t;
        sk.bones.push_back(b);
    };
    add("root", -1, glm::vec3(0));
    const float xs[2] = {0.6f, -0.6f};
    for (int side = 0; side < 2; ++side) {
        for (int i = 0; i < 4; ++i) {
            const float z = -0.9f + static_cast<float>(i) * 0.6f;
            const int base = 1 + (side * 4 + i) * 3;
            add("hip" + std::to_string(side * 4 + i), 0,
                glm::vec3(xs[side], 0.6f, z));
            add("knee" + std::to_string(side * 4 + i), base,
                glm::vec3(0, -0.9f, 0));
            add("foot" + std::to_string(side * 4 + i), base + 1,
                glm::vec3(0, -0.9f, 0));
        }
    }
    return sk;
}

glm::vec3 modelPosition(const engine::animation::MotionPose& pose,
                        const engine::animation::MotionSkeleton& sk,
                        int bone) {
    std::vector<glm::mat4> models(sk.bones.size(), glm::mat4(1.0f));
    for (std::size_t i = 0; i < sk.bones.size(); ++i) {
        const glm::mat4 local =
            glm::translate(glm::mat4(1.0f), pose.translations[i]) *
            glm::mat4_cast(pose.rotations[i]) *
            glm::scale(glm::mat4(1.0f), pose.scales[i]);
        const int p = sk.bones[i].parent;
        models[i] =
            p >= 0 ? models[static_cast<std::size_t>(p)] * local : local;
    }
    return glm::vec3(models[static_cast<std::size_t>(bone)][3]);
}

// Runs one step and returns the driver result.
bool step(engine::animation::IProceduralLocomotion& driver,
          const engine::animation::ProceduralLocomotionSpec& spec,
          const engine::animation::IFootTerrainSampler& terrain, float time,
          const glm::vec3& body, float yaw, const glm::vec2& vel,
          const std::vector<engine::animation::ProceduralLegState>& prev,
          engine::animation::ProceduralLocomotionResult& out,
          std::string& err) {
    return driver.step(spec, terrain, time, body, yaw, vel, prev, out, err);
}

// ---------------------------------------------------------------------------
// Validation: all-or-nothing, never silent.
// ---------------------------------------------------------------------------
void test_validation() {
    const engine::animation::ProceduralLocomotionSpec spider = make_spider();
    std::string err;
    engine::animation::ProceduralLocomotionSpec s;

    check(!s.validate(err), "validation: empty legs refused");
    s = spider;
    s.bodyHeight = 0.0f;
    check(!s.validate(err), "validation: zero bodyHeight refused");
    s = spider;
    s.retargetFactor = 0.0f;
    check(!s.validate(err), "validation: zero retargetFactor refused");
    s = spider;
    s.swingDuration = -1.0f;
    check(!s.validate(err), "validation: negative swingDuration refused");
    s = spider;
    s.stepHeight = -0.1f;
    check(!s.validate(err), "validation: negative stepHeight refused");
    s = spider;
    s.landingDistancePerSpeed = -1.0f;
    check(!s.validate(err), "validation: negative landing distance refused");
    s = spider;
    s.minLandingDistance = -1.0f;
    check(!s.validate(err), "validation: negative min landing refused");
    s = spider;
    s.legs[0].upperLength = 0.0f;
    check(!s.validate(err), "validation: bad leg propagates refusal");
    check(spider.validate(err), "validation: spider spec is valid");

    // Driver-level refusals.
    auto driver = engine::animation::create_procedural_locomotion();
    const FlatTerrain flat;
    engine::animation::ProceduralLocomotionResult out;
    std::vector<engine::animation::ProceduralLegState> prev;

    check(!step(*driver, spider, flat, -0.5f, glm::vec3(0), 0.0f,
                glm::vec2(0, 1), prev, out, err),
          "validation: negative time refused");
    check(!step(*driver, spider, flat, 0.0f,
                glm::vec3(NAN, 0, 0), 0.0f, glm::vec2(0, 1), prev, out, err),
          "validation: non-finite body refused");

    prev.resize(1);  // one leg against an 8-leg spec
    check(!step(*driver, spider, flat, 0.0f, glm::vec3(0), 0.0f,
                glm::vec2(0, 1), prev, out, err),
          "validation: leg state size mismatch refused");
    std::printf("[legs] validation all-or-nothing OK\n");
}

// ---------------------------------------------------------------------------
// Emergent timing: all 8 legs plant, stay planted while in reach, and swing
// together only when the body carries them past reach * retargetFactor.
// ---------------------------------------------------------------------------
void test_plant_and_retarget() {
    const engine::animation::ProceduralLocomotionSpec spider = make_spider();
    const FlatTerrain flat;
    auto driver = engine::animation::create_procedural_locomotion();
    std::string err;
    engine::animation::ProceduralLocomotionResult out;
    std::vector<engine::animation::ProceduralLegState> prev;

    // Step 1 (t=0, body at origin, moving +Z at 1 m/s): every leg plants at
    // its rest position ON the terrain (y = 0), the body rides the ground.
    check(step(*driver, spider, flat, 0.0f, glm::vec3(0), 0.0f,
               glm::vec2(0, 1), prev, out, err),
          "plant: first step succeeds");
    check(out.surfaceKnown, "plant: flat terrain is known");
    checkVec(out.bodyPosition, glm::vec3(0, 0.6f, 0), 1e-6f,
             "plant: body rides the ground at bodyHeight");
    check(out.legs.size() == 8, "plant: 8 legs");
    for (int i = 0; i < 8; ++i) {
        const auto& leg = spider.legs[static_cast<std::size_t>(i)];
        check(!out.legs[static_cast<std::size_t>(i)].swinging,
              "plant: leg planted on first step");
        const glm::vec3 expect(leg.restOffset.x, 0.0f, leg.restOffset.z);
        checkVec(out.legs[static_cast<std::size_t>(i)].targetWorld, expect,
                 1e-6f, "plant: foot planted at rest on the terrain");
    }
    prev = out.legs;

    // Step 2 (t=0.1, body +0.1): still within reach (0.1 < 1.5) — planted
    // feet stay FIXED in world while the hips travel ahead.
    check(step(*driver, spider, flat, 0.1f, glm::vec3(0, 0, 0.1f), 0.0f,
               glm::vec2(0, 1), prev, out, err),
          "plant: second step succeeds");
    checkVec(out.legs[0].targetWorld, glm::vec3(0.6f, 0.0f, -0.9f), 1e-6f,
             "plant: planted foot does not move with the body");
    check(!out.legs[0].swinging, "plant: still planted inside reach");
    prev = out.legs;

    // Step 3 (t=0.5, body +1.6): hip travel 1.6 > 1.5 = reach*0.75 — every
    // leg independently swings to a landing ahead (+Z by velocity-scaled
    // distance and minLandingDistance).
    check(step(*driver, spider, flat, 0.5f, glm::vec3(0, 0, 1.6f), 0.0f,
               glm::vec2(0, 1), prev, out, err),
          "plant: retarget step succeeds");
    for (int i = 0; i < 8; ++i) {
        check(out.legs[static_cast<std::size_t>(i)].swinging,
              "plant: out-of-reach leg swings");
    }
    // Leg 0 (R1, hip x=+0.6, planted at (0.6, 0, -0.9)): landing = planted +
    // velocity * 0.15 + forward(+Z) * 0.3 = (0.6, 0, -0.45).
    checkVec(out.legs[0].landing, glm::vec3(0.6f, 0.0f, -0.45f), 1e-6f,
             "plant: landing placed ahead along the velocity");
    checkClose(out.legs[0].swingProgress, 0.0f, 1e-6f,
               "plant: swing starts at progress 0");
    prev = out.legs;

    // Step 4 (t=0.675 = 0.5 + half the swing): deterministic progress 0.5,
    // foot arcs between planted and landing with the step-height lift.
    check(step(*driver, spider, flat, 0.675f, glm::vec3(0, 0, 1.6f), 0.0f,
               glm::vec2(0, 1), prev, out, err),
          "plant: mid-swing step succeeds");
    checkClose(out.legs[0].swingProgress, 0.5f, 1e-5f,
               "plant: swing progress is deterministic from the start time");
    checkVec(out.legs[0].targetWorld,
             glm::vec3(0.6f, 0.2f, (-0.9f - 0.45f) / 2.0f),
             1e-4f, "plant: mid-swing arc with the step-height lift");
    prev = out.legs;

    // Step 5 (t=0.85 = 0.5 + swing duration): the leg re-plants at the
    // landing; the target is the sampled landing (y = 0).
    check(step(*driver, spider, flat, 0.85f, glm::vec3(0, 0, 1.6f), 0.0f,
               glm::vec2(0, 1), prev, out, err),
          "plant: landing step succeeds");
    check(!out.legs[0].swinging, "plant: leg re-planted after the swing");
    checkVec(out.legs[0].targetWorld, glm::vec3(0.6f, 0.0f, -0.45f), 1e-6f,
             "plant: foot re-planted at the landing");
    std::printf("[legs] emergent plant + retarget OK\n");
}

// ---------------------------------------------------------------------------
// Stationary body: no retargeting ever (0 hip travel < 1.5) — legs stay
// planted; the body keeps riding the terrain.
// ---------------------------------------------------------------------------
void test_stationary() {
    const engine::animation::ProceduralLocomotionSpec spider = make_spider();
    const FlatTerrain flat;
    auto driver = engine::animation::create_procedural_locomotion();
    std::string err;
    engine::animation::ProceduralLocomotionResult out;
    std::vector<engine::animation::ProceduralLegState> prev;
    check(step(*driver, spider, flat, 0.0f, glm::vec3(0), 0.0f,
               glm::vec2(0, 0), prev, out, err),
          "stationary: first step succeeds");
    for (int t = 1; t <= 8; ++t) {
        prev = out.legs;
        check(step(*driver, spider, flat, static_cast<float>(t),
                   glm::vec3(0), 0.0f, glm::vec2(0, 0), prev, out, err),
              "stationary: step succeeds");
        check(!out.legs[0].swinging, "stationary: no leg ever swings");
        checkVec(out.legs[0].targetWorld, glm::vec3(0.6f, 0.0f, -0.9f),
                 1e-6f, "stationary: planted foot never moves");
        checkVec(out.bodyPosition, glm::vec3(0, 0.6f, 0), 1e-6f,
                 "stationary: body keeps riding the terrain");
    }
    std::printf("[legs] stationary body stays planted OK\n");
}

// ---------------------------------------------------------------------------
// Terrain-aware: the seam's surface drives both the plant height and the
// landing height per column (right legs on the raised step, left on flat);
// the body rides the surface under the body.
// ---------------------------------------------------------------------------
void test_step_terrain() {
    const engine::animation::ProceduralLocomotionSpec spider = make_spider();
    const StepTerrain stepT;
    auto driver = engine::animation::create_procedural_locomotion();
    std::string err;
    engine::animation::ProceduralLocomotionResult out;
    std::vector<engine::animation::ProceduralLegState> prev;

    // First plant: right legs (x = +0.6 > 0.25) rest at y = 0.5; left legs
    // (x = -0.6) at y = 0; the body surface (x = 0) is 0 -> body y = 0.6.
    check(step(*driver, spider, stepT, 0.0f, glm::vec3(0), 0.0f,
               glm::vec2(0, 1), prev, out, err),
          "terrain: first step succeeds");
    checkVec(out.bodyPosition, glm::vec3(0, 0.6f, 0), 1e-6f,
             "terrain: body rides the surface under the body");
    checkVec(out.legs[0].targetWorld, glm::vec3(0.6f, 0.5f, -0.9f), 1e-6f,
             "terrain: right foot rests on the raised step");
    checkVec(out.legs[4].targetWorld, glm::vec3(-0.6f, 0.0f, -0.9f), 1e-6f,
             "terrain: left foot rests on the flat part");
    prev = out.legs;

    // Retarget: the swing landing is sampled at ITS OWN column (right legs
    // keep x = +0.6 -> landing y = 0.5).
    check(step(*driver, spider, stepT, 0.5f, glm::vec3(0, 0, 1.6f), 0.0f,
               glm::vec2(0, 1), prev, out, err),
          "terrain: retarget step succeeds");
    check(out.legs[0].swinging, "terrain: right leg swings");
    checkVec(out.legs[0].landing, glm::vec3(0.6f, 0.5f, -0.45f), 1e-6f,
             "terrain: landing snapped to the raised step");
    checkVec(out.legs[4].landing, glm::vec3(-0.6f, 0.0f, -0.45f), 1e-6f,
             "terrain: left landing snapped to the flat part");
    std::printf("[legs] step-terrain placement OK\n");
}

// ---------------------------------------------------------------------------
// Unknown terrain: nothing is invented — the body keeps its given y + height
// and the feet keep their body-space rest y (never snapped).
// ---------------------------------------------------------------------------
void test_unknown_terrain() {
    const engine::animation::ProceduralLocomotionSpec spider = make_spider();
    const UnknownTerrain unknown;
    auto driver = engine::animation::create_procedural_locomotion();
    std::string err;
    engine::animation::ProceduralLocomotionResult out;
    std::vector<engine::animation::ProceduralLegState> prev;
    check(step(*driver, spider, unknown, 0.0f, glm::vec3(0, 5.0f, 0), 0.0f,
               glm::vec2(0, 1), prev, out, err),
          "unknown: first step succeeds");
    check(!out.surfaceKnown, "unknown: surface reported unknown");
    checkVec(out.bodyPosition, glm::vec3(0, 5.6f, 0), 1e-6f,
             "unknown: body keeps the given y + bodyHeight");
    checkVec(out.legs[0].targetWorld, glm::vec3(0.6f, 4.8f, -0.9f), 1e-6f,
             "unknown: foot keeps the body-space rest y (no invented height)");
    std::printf("[legs] unknown terrain never invents height OK\n");
}

// ---------------------------------------------------------------------------
// Determinism: two independent drivers over the same sequence produce
// bit-exact leg states and body state.
// ---------------------------------------------------------------------------
void test_determinism() {
    const engine::animation::ProceduralLocomotionSpec spider = make_spider();
    const StepTerrain stepT;
    auto a = engine::animation::create_procedural_locomotion();
    auto b = engine::animation::create_procedural_locomotion();
    std::string errA, errB;
    engine::animation::ProceduralLocomotionResult outA, outB;
    std::vector<engine::animation::ProceduralLegState> prevA, prevB;
    const glm::vec3 body0(0);
    const glm::vec2 vel(0, 1);
    const float times[4] = {0.0f, 0.1f, 0.5f, 0.675f};
    const glm::vec3 bodies[4] = {glm::vec3(0), glm::vec3(0, 0, 0.1f),
                                 glm::vec3(0, 0, 1.6f), glm::vec3(0, 0, 1.6f)};
    for (int i = 0; i < 4; ++i) {
        check(step(*a, spider, stepT, times[i], bodies[i], 0.0f, vel, prevA,
                   outA, errA) &&
                  step(*b, spider, stepT, times[i], bodies[i], 0.0f, vel,
                       prevB, outB, errB),
              "determinism: both drivers step");
        check(outA.bodyPosition == outB.bodyPosition,
              "determinism: body position bit-exact");
        check(outA.surfaceKnown == outB.surfaceKnown,
              "determinism: surface known bit-exact");
        for (std::size_t l = 0; l < outA.legs.size(); ++l) {
            const auto& la = outA.legs[l];
            const auto& lb = outB.legs[l];
            check(la.targetWorld == lb.targetWorld &&
                      la.swinging == lb.swinging &&
                      la.swingProgress == lb.swingProgress &&
                      la.swingStartTime == lb.swingStartTime &&
                      la.landing == lb.landing &&
                      la.surfaceHeight == lb.surfaceHeight &&
                      la.surfaceKnown == lb.surfaceKnown,
                  "determinism: leg state bit-exact");
        }
        prevA = outA.legs;
        prevB = outB.legs;
    }
    std::printf("[legs] cross-instance determinism OK\n");
}

// ---------------------------------------------------------------------------
// IK integration: a planted leg target produced by the driver is solved by
// the §18 item 3 IK — the foot model position lands exactly on the target.
// ---------------------------------------------------------------------------
void test_ik_integration() {
    const engine::animation::ProceduralLocomotionSpec spider = make_spider();
    const FlatTerrain flat;
    auto driver = engine::animation::create_procedural_locomotion();
    auto db = engine::animation::create_motion_database();
    std::string err;
    check(db->cook_skeleton(make_spider_skeleton(), err),
          "ik: spider skeleton cooks");

    engine::animation::ProceduralLocomotionResult out;
    std::vector<engine::animation::ProceduralLegState> prev;
    check(step(*driver, spider, flat, 0.0f, glm::vec3(0), 0.0f,
               glm::vec2(0, 1), prev, out, err),
          "ik: driver step succeeds");
    const glm::vec3 planted = out.legs[0].targetWorld;

    // Rest pose: the root at the body origin; hip locals at their offsets,
    // knee/foot below (the driver's hipWorld is the hip model position).
    const engine::animation::MotionSkeleton sk = make_spider_skeleton();
    engine::animation::MotionPose pose;
    pose.translations.resize(sk.bones.size(), glm::vec3(0.0f));
    pose.rotations.resize(sk.bones.size(), glm::quat(1.0f, 0, 0, 0));
    pose.scales.resize(sk.bones.size(), glm::vec3(1.0f));
    for (std::size_t i = 1; i < sk.bones.size(); ++i) {
        pose.translations[i] = sk.bones[i].localTranslation;
    }
    engine::animation::MotionPose solved;
    check(db->ik_two_bone(pose, 1, 2, 3, planted, glm::vec3(0, 0, 1), 1.0f,
                          solved),
          "ik: leg chain solves the planted target");
    checkVec(modelPosition(solved, sk, 3), planted, 0.02f,
             "ik: foot lands on the driver's planted target");
    std::printf("[legs] IK integration (procedural legs -> ik_two_bone) OK\n");
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    test_validation();
    test_plant_and_retarget();
    test_stationary();
    test_step_terrain();
    test_unknown_terrain();
    test_determinism();
    test_ik_integration();
    if (g_failures == 0) {
        std::printf("[legs] ALL PASSED\n");
        return 0;
    }
    std::printf("[legs] %d FAILURE(S)\n", g_failures);
    return 1;
}
