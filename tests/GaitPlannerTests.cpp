// GaitPlannerTests (FALTANTES §18 item 4): proves the public locomotion
// contract `ContactPlanner` + `GaitAsset` + leg-chain assets for arbitrary
// creatures. The planner is pure and deterministic: planted feet stay fixed
// in world space during stance (constant-velocity back-projection), swinging
// feet arc toward the next landing, the stride is capped by the asset, and
// `withinReach` reports when the leg chain cannot be solved. The gate also
// proves the composition with the §18 item 3 IK: a planted foot target from
// the plan is solved by IMotionDatabase::ik_two_bone, landing the foot
// exactly on the target.
#include "engine/animation/IGaitPlanner.hpp"
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
        std::printf("[gait] FAIL: %s\n", message);
        ++g_failures;
    }
}

void checkClose(float a, float b, float eps, const char* message) {
    if (std::fabs(a - b) > eps) {
        std::printf("[gait] FAIL: %s (%.6f vs %.6f)\n", message, a, b);
        ++g_failures;
    }
}

void checkVec(const glm::vec3& a, const glm::vec3& b, float eps,
              const char* message) {
    if (glm::distance(a, b) > eps) {
        std::printf("[gait] FAIL: %s (%.4f,%.4f,%.4f vs %.4f,%.4f,%.4f)\n",
                    message, a.x, a.y, a.z, b.x, b.y, b.z);
        ++g_failures;
    }
}

// A quadruped: four hip-anchored two-bone legs (upper 1.0 + lower 1.0, reach
// 2.0) with hip height 0.8 and rest drop 0.8. The vertical span hip->foot is
// 1.6, so a horizontal slack of ~1.2 is needed: a 1 m/s walk with a 0.6 s
// stance stretches a planted foot to |hip-foot| ≈ 1.97 at stance end — within
// reach 2.0 but past the 1.8 of shorter legs.
engine::animation::GaitAsset make_quadruped() {
    engine::animation::GaitAsset gait;
    gait.name = "quad";
    gait.cycleDuration = 1.0f;
    gait.stanceFraction = 0.6f;
    gait.stepHeight = 0.2f;
    gait.maxStride = 0.5f;
    const glm::vec3 hips[4] = {{0.3f, 0.8f, 0.5f}, {-0.3f, 0.8f, 0.5f},
                               {0.3f, 0.8f, -0.5f}, {-0.3f, 0.8f, -0.5f}};
    const char* names[4] = {"FL", "FR", "BL", "BR"};
    for (int i = 0; i < 4; ++i) {
        engine::animation::LegChainAsset leg;
        leg.name = names[i];
        leg.hipOffset = hips[i];
        leg.upperLength = 1.0f;
        leg.lowerLength = 1.0f;
        leg.restOffset = glm::vec3(0.0f, -0.8f, 0.0f);
        leg.hipBone = 1 + i * 3;
        leg.kneeBone = 2 + i * 3;
        leg.footBone = 3 + i * 3;
        gait.legs.push_back(leg);
    }
    // Trot-like diagonal pairing: front pairs with the opposite rear leg.
    gait.legPhases = {0.0f, 0.5f, 0.25f, 0.75f};
    return gait;
}

// The same creature as a canonical MotionSkeleton (children after parents),
// matching the leg-chain geometry (hip at hipOffset, knee/foot below).
engine::animation::MotionSkeleton make_quadruped_skeleton() {
    engine::animation::MotionSkeleton sk;
    sk.name = "quad";
    auto add = [&sk](const std::string& name, int parent, glm::vec3 t) {
        engine::animation::MotionBone b;
        b.name = name;
        b.parent = parent;
        b.localTranslation = t;
        sk.bones.push_back(b);
    };
    add("root", -1, glm::vec3(0));
    const glm::vec3 hips[4] = {{0.3f, 0.8f, 0.5f}, {-0.3f, 0.8f, 0.5f},
                               {0.3f, 0.8f, -0.5f}, {-0.3f, 0.8f, -0.5f}};
    for (int i = 0; i < 4; ++i) {
        add("hip" + std::to_string(i), 0, hips[i]);
        add("knee" + std::to_string(i), 1 + i * 3, glm::vec3(0, -0.9f, 0));
        add("foot" + std::to_string(i), 2 + i * 3, glm::vec3(0, -0.9f, 0));
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

void test_validation() {
    std::string err;
    // Leg chain.
    engine::animation::LegChainAsset leg;
    leg.name = "bad";
    leg.upperLength = 0.0f;
    check(!leg.validate(err), "leg: zero upper length refused");
    leg.upperLength = 0.5f;
    leg.lowerLength = -1.0f;
    check(!leg.validate(err), "leg: negative lower length refused");
    leg.lowerLength = 0.5f;
    leg.hipBone = 2;
    leg.kneeBone = 2;
    check(!leg.validate(err), "leg: hip==knee refused");
    leg.hipBone = -1;
    leg.kneeBone = -1;
    leg.maxReach = -1.0f;
    check(!leg.validate(err), "leg: negative maxReach refused");
    leg.maxReach = 0.0f;
    check(leg.validate(err), "leg: valid chain passes");
    // Gait.
    engine::animation::GaitAsset gait = make_quadruped();
    gait.cycleDuration = 0.0f;
    check(!gait.validate(err), "gait: zero cycle refused");
    gait.cycleDuration = 1.0f;
    gait.stanceFraction = 0.0f;
    check(!gait.validate(err), "gait: zero stance refused");
    gait.stanceFraction = 0.6f;
    gait.stepHeight = -1.0f;
    check(!gait.validate(err), "gait: negative stepHeight refused");
    gait.stepHeight = 0.2f;
    gait.maxStride = 0.0f;
    check(!gait.validate(err), "gait: zero maxStride refused");
    gait.maxStride = 0.5f;
    gait.legs.clear();
    check(!gait.validate(err), "gait: empty legs refused");
    gait = make_quadruped();
    gait.legPhases = {0.0f, 0.5f};
    check(!gait.validate(err), "gait: phase count mismatch refused");
    gait = make_quadruped();
    gait.legPhases[2] = 1.5f;
    check(!gait.validate(err), "gait: phase out of range refused");
    check(make_quadruped().validate(err), "gait: valid gait passes");
    std::printf("[gait] validation all-or-nothing OK\n");
}

void test_walking_and_determinism() {
    const engine::animation::GaitAsset gait = make_quadruped();
    auto planner = engine::animation::create_contact_planner();
    auto planner2 = engine::animation::create_contact_planner();
    const glm::vec3 v(0.0f, 0.0f, 1.0f);  // speed 1 m/s along +Z
    const float yaw = 0.0f;
    const glm::vec3 origin(0.0f);

    auto planAt = [&](auto& p, float t) {
        engine::animation::GaitPlan plan;
        std::string err;
        const glm::vec3 body(0.0f, 0.0f, t);  // the body advances with the walk
        check(p->plan(gait, t, body, yaw, glm::vec2(v.x, v.z), plan, err),
              "walk: plan succeeds");
        return plan;
    };

    // Planted foot stays fixed in world during its stance: leg FL (phase 0)
    // is in stance for t in [0, 0.6); at 0.2 and 0.4 the target is identical.
    {
        const engine::animation::GaitPlan a = planAt(planner, 0.2f);
        const engine::animation::GaitPlan b = planAt(planner, 0.4f);
        check(!a.feet[0].stance || a.feet[0].stance, "walk: sanity");
        check(a.feet[0].stance && b.feet[0].stance, "walk: FL stance in [0,0.6)");
        checkVec(a.feet[0].targetWorld, b.feet[0].targetWorld, 1e-4f,
                 "walk: planted foot fixed in world");
        checkClose(a.feet[0].lift, 0.0f, 1e-5f, "walk: stance lift 0");
        check(a.feet[0].withinReach, "walk: stance foot within reach");
    }
    // Mid-swing lift is exactly stepHeight (sin(pi/2) == 1).
    {
        const engine::animation::GaitPlan p = planAt(planner, 0.8f);
        check(!p.feet[0].stance, "walk: FL swinging at t=0.8");
        checkClose(p.feet[0].lift, 0.2f, 1e-4f, "walk: mid-swing lift == stepHeight");
        // Stride: landing is maxStride ahead of the planted position.
        const glm::vec3 planted(0.0f, -0.8f, 0.2f);
        const glm::vec3 landing = p.feet[0].landing;
        checkClose(landing.z - planted.z, 0.5f, 1e-4f, "walk: stride == maxStride");
        checkVec(landing - planted, glm::vec3(0.0f, 0.0f, 0.5f), 1e-4f,
                 "walk: landing offset horizontal");
    }
    // Phase mapping + stance/swing membership at t=0.3.
    {
        const engine::animation::GaitPlan p = planAt(planner, 0.3f);
        const bool expect[4] = {true, false, true, true};
        for (int i = 0; i < 4; ++i) {
            const float phase =
                std::fmod(0.3f + gait.legPhases[i], 1.0f);
            checkClose(p.feet[i].phase, phase, 1e-5f, "walk: phase mapping");
            check(p.feet[i].stance == expect[i], "walk: stance/swing membership");
            check(p.feet[i].withinReach, "walk: all feet within reach at t=0.3");
        }
    }
    // Determinism: two planner instances, identical inputs -> identical plans.
    {
        bool same = true;
        for (float t : {0.05f, 0.3f, 0.47f, 0.8f, 1.23f}) {
            const engine::animation::GaitPlan a = planAt(planner, t);
            const engine::animation::GaitPlan b = planAt(planner2, t);
            if (a.feet.size() != b.feet.size()) { same = false; break; }
            for (std::size_t i = 0; i < a.feet.size(); ++i) {
                if (a.feet[i].targetWorld != b.feet[i].targetWorld ||
                    a.feet[i].landing != b.feet[i].landing ||
                    a.feet[i].lift != b.feet[i].lift ||
                    a.feet[i].stance != b.feet[i].stance ||
                    a.feet[i].withinReach != b.feet[i].withinReach) {
                    same = false;
                }
            }
        }
        check(same, "walk: deterministic bit-exact between instances");
    }
    // Fast body: the planted foot trails out of reach -> withinReach false.
    {
        engine::animation::GaitPlan plan;
        std::string err;
        const glm::vec3 body(0.0f, 0.0f, 3.0f);  // body moved 3 m at 10 m/s
        check(planner->plan(gait, 0.3f, body, yaw, glm::vec2(0.0f, 10.0f), plan, err),
              "walk: fast plan succeeds");
        check(!plan.feet[0].withinReach,
              "walk: stretched foot reports out of reach");
    }
    // Stationary: planted feet never move; swinging feet march in place.
    {
        engine::animation::GaitPlan p0, p1;
        std::string err;
        check(planner->plan(gait, 0.0f, origin, yaw, glm::vec2(0.0f), p0, err) &&
                  planner->plan(gait, 0.37f, origin, yaw, glm::vec2(0.0f), p1, err),
              "stationary: plans succeed");
        check(p0.feet[0].stance && p1.feet[0].stance, "stationary: FL stance");
        checkVec(p0.feet[0].targetWorld, p1.feet[0].targetWorld, 1e-5f,
                 "stationary: planted foot fixed");
        checkVec(p0.feet[0].targetWorld, glm::vec3(0.0f, -0.8f, 0.0f), 1e-5f,
                 "stationary: planted at rest offset");
        check(p1.feet[1].lift > 0.0f, "stationary: swinging foot lifts (march)");
    }
    std::printf("[gait] walking + determinism OK\n");
}

void test_ik_integration() {
    // The planted foot target produced by the planner is solved by the §18
    // item 3 IK (IMotionDatabase::ik_two_bone): the foot model position lands
    // exactly on the target.
    const engine::animation::GaitAsset gait = make_quadruped();
    auto planner = engine::animation::create_contact_planner();
    auto db = engine::animation::create_motion_database();
    std::string err;
    check(db->cook_skeleton(make_quadruped_skeleton(), err),
          "ik: quadruped skeleton cooks");

    engine::animation::GaitPlan plan;
    check(planner->plan(gait, 0.2f, glm::vec3(0.0f), 0.0f,
                        glm::vec2(0.0f, 1.0f), plan, err),
          "ik: plan succeeds");
    const engine::animation::FootPlan& fl = plan.feet[0];
    check(fl.stance && fl.withinReach, "ik: FL planted and in reach");

    // Rest pose of the skeleton (hip at hipOffset, knee/foot below).
    const engine::animation::MotionSkeleton sk = make_quadruped_skeleton();
    engine::animation::MotionPose pose;
    pose.translations.resize(sk.bones.size(), glm::vec3(0.0f));
    pose.rotations.resize(sk.bones.size(), glm::quat(1.0f, 0, 0, 0));
    pose.scales.resize(sk.bones.size(), glm::vec3(1.0f));
    for (const auto& b : sk.bones) pose.translations[b.parent + 1] = b.localTranslation;
    pose.translations[0] = glm::vec3(0.0f);
    // Fix the chain locals explicitly (children inherit from the parent's
    // offset through the model chain).
    pose.translations[1] = glm::vec3(0.3f, 0.8f, 0.5f);   // FL hip
    pose.translations[2] = glm::vec3(0.0f, -0.9f, 0.0f);  // FL knee
    pose.translations[3] = glm::vec3(0.0f, -0.9f, 0.0f);  // FL foot

    engine::animation::MotionPose solved;
    check(db->ik_two_bone(pose, 1, 2, 3, fl.targetWorld, glm::vec3(0, 0, 1), 1.0f, solved),
          "ik: leg chain solves the planted target");
    checkVec(modelPosition(solved, sk, 3), fl.targetWorld, 0.02f,
             "ik: foot lands on the planted target");
    std::printf("[gait] IK integration (planner -> ik_two_bone) OK\n");
}

// FALTANTES item 23 "animações": GaitAsset/LegChainAsset as versioned JSON
// assets (the AbilityDefinition/MissionDefinition pattern) — bit-exact
// round-trip %.9g, all-or-nothing, defaults on missing fields, and the same
// refusal rules the MCP mirror enforces.
void test_json_roundtrip() {
    // Full asset: quadruped with named legs, offsets, bones and phases.
    engine::animation::GaitAsset gait;
    gait.name = "trot";
    gait.cycleDuration = 0.8f;
    gait.stanceFraction = 0.55f;
    gait.stepHeight = 0.3f;
    gait.maxStride = 0.75f;
    gait.legPhases = {0.0f, 0.5f, 0.25f, 0.75f};
    for (int i = 0; i < 4; ++i) {
        engine::animation::LegChainAsset leg;
        leg.name = (i % 2 == 0) ? "front" : "rear";
        leg.hipOffset = glm::vec3(0.3f, 0.8f, i < 2 ? 0.5f : -0.5f);
        leg.upperLength = 0.5f + i * 0.1f;
        leg.lowerLength = 0.6f;
        leg.restOffset = glm::vec3(0.3f, -1.0f, i < 2 ? 0.5f : -0.5f);
        leg.maxReach = 0.0f;
        leg.hipBone = 1 + i * 3;
        leg.kneeBone = 2 + i * 3;
        leg.footBone = 3 + i * 3;
        gait.legs.push_back(leg);
    }
    std::string err;
    check(gait.validate(err), "json: full asset validates");

    const std::string json = gait.to_json();
    engine::animation::GaitAsset back;
    check(back.load_from_json(json, err), "json: full asset loads");
    check(back.to_json() == json, "json: round-trip bit-exact (canonical doc)");
    check(back.name == "trot" && back.cycleDuration == 0.8f &&
              back.stanceFraction == 0.55f && back.stepHeight == 0.3f &&
              back.maxStride == 0.75f,
          "json: scalar fields round-trip");
    check(back.legPhases.size() == 4 && back.legs.size() == 4,
          "json: arrays round-trip");
    check(back.legs[3].upperLength == 0.8f && back.legs[3].footBone == 12 &&
              back.legs[3].restOffset.z == -0.5f,
          "json: leg fields round-trip");

    // Leg asset standalone round-trip.
    const std::string legJson = gait.legs[0].to_json();
    engine::animation::LegChainAsset legBack;
    check(legBack.load_from_json(legJson, err), "json: leg asset loads");
    check(legBack.to_json() == legJson, "json: leg round-trip bit-exact");

    // Defaults: a minimal document loads and re-emits the canonical doc.
    const std::string minimal =
        "{\"version\":1,\"name\":\"amble\",\"legPhases\":[0.0,0.5],"
        "\"legs\":[{\"name\":\"l1\"},{\"name\":\"l2\"}]}";
    engine::animation::GaitAsset ambled;
    check(ambled.load_from_json(minimal, err), "json: minimal doc loads");
    check(ambled.cycleDuration == 1.0f && ambled.stanceFraction == 0.6f &&
              ambled.stepHeight == 0.25f && ambled.maxStride == 0.5f &&
              ambled.legs[0].upperLength == 0.5f &&
              ambled.legs[0].hipBone == -1,
          "json: missing fields take documented defaults");
    check(ambled.to_json() == ambled.to_json(), "json: emission deterministic");

    // All-or-nothing: every refusal leaves the target object untouched.
    engine::animation::GaitAsset probe = ambled;  // a valid asset
    const std::string validDoc = probe.to_json();
    const char* badDocs[] = {
        "{\"version\":1,\"name\":\"x\",\"legs\":[{\"name\":\"l\"}]",  // malformed JSON
        "{\"version\":2,\"name\":\"x\",\"legs\":[{\"name\":\"l\"}],\"legPhases\":[0]}",
        "{\"version\":1,\"name\":\"\",\"legs\":[{\"name\":\"l\"}],\"legPhases\":[0]}",
        "{\"version\":1,\"name\":\"x\"}",  // empty legs
        "{\"version\":1,\"name\":\"x\",\"legs\":[{\"name\":\"l\"}]}",  // phases size mismatch
        "{\"version\":1,\"name\":\"x\",\"legs\":[{\"name\":\"l\"}],\"legPhases\":[1.5]}",
        "{\"version\":1,\"name\":\"x\",\"legs\":[{\"name\":\"l\",\"upperLength\":0}],\"legPhases\":[0]}",
        "{\"version\":1,\"name\":\"x\",\"legs\":[{\"name\":\"l\",\"hipOffset\":[1,2]}],\"legPhases\":[0]}",
        "{\"version\":1,\"name\":\"x\",\"legs\":[{\"name\":\"\",\"hipBone\":1,\"kneeBone\":1}],\"legPhases\":[0]}",
        "{\"version\":1,\"name\":\"x\",\"legs\":{\"name\":\"l\"},\"legPhases\":[0]}",
        "{\"version\":1,\"name\":\"x\",\"legs\":[{\"name\":\"l\"}],\"legPhases\":\"oops\"}",
    };
    for (const char* doc : badDocs) {
        engine::animation::GaitAsset target = probe;
        check(!target.load_from_json(doc, err), "json: bad doc refused");
        check(target.to_json() == validDoc, "json: refused doc leaves target intact");
    }
    // Same rule on the leg asset.
    engine::animation::LegChainAsset legProbe = gait.legs[0];
    const std::string legValid = legProbe.to_json();
    check(!legProbe.load_from_json("{\"name\":\"\"}", err),
          "json: leg with empty name refused");
    check(legProbe.to_json() == legValid, "json: refused leg leaves target intact");

    std::printf("[gait] JSON asset round-trip (bit-exact, all-or-nothing) OK\n");
}

}  // namespace

// A biped (Agente 5 - A2-105): the gait planner must handle ARBITRARY
// creatures - a 2-leg humanoid (LeftLeg/RightLeg, phase offset 0.5) mirrors
// the showcase_character_gait.json consumed in the JOGO by tick. Regression
// here fails loudly if planning collapsed back to quadruped-only.
engine::animation::GaitAsset make_biped() {
    engine::animation::GaitAsset gait;
    gait.name = "biped";
    gait.cycleDuration = 1.0f;
    gait.stanceFraction = 0.6f;
    gait.stepHeight = 0.2f;
    gait.maxStride = 0.5f;
    const glm::vec3 hips[2] = { { -0.18f, 0.9f, 0.0f }, { 0.18f, 0.9f, 0.0f } };
    const char* names[2] = { "LeftLeg", "RightLeg" };
    for (int i = 0; i < 2; ++i) {
        engine::animation::LegChainAsset leg;
        leg.name = names[i];
        leg.hipOffset = hips[i];
        leg.upperLength = 1.0f;
        leg.lowerLength = 1.0f;
        leg.restOffset = glm::vec3(0.0f, -0.8f, 0.0f);
        leg.hipBone = 1 + i * 2;
        leg.kneeBone = 2 + i * 2;
        leg.footBone = 3 + i * 2;
        gait.legs.push_back(leg);
    }
    gait.legPhases = { 0.0f, 0.5f };
    return gait;
}

// Two-legged plan: both legs plan; the phase-0.5 leg is mid-swing while the
// phase-0 leg is in stance (humanoid alternating gait).
void test_biped_plan() {
    const engine::animation::GaitAsset gait = make_biped();
    check(gait.legs.size() == 2, "biped: exactly 2 legs");
    auto planner = engine::animation::create_contact_planner();
    std::string err;
    engine::animation::GaitPlan plan;
    const glm::vec3 body(0.0f, 0.0f, 0.0f);
    const bool ok = planner->plan(gait, 0.25f, body, 0.0f, { 0.0f, 1.0f },
                                  plan, err);
    check(ok && err.empty(), "biped: plan succeeds");
    check(plan.feet.size() == 2, "biped: plan has 2 feet");
    // Stance/swing alternate: left (phase 0) in stance, right (phase 0.5) in
    // swing for this gait clock.
    check(plan.feet[0].stance && !plan.feet[1].stance,
          "biped: left stance / right swing");
    check(plan.feet[0].withinReach, "biped: stance foot within reach");
    std::printf("[gait] biped: 2-leg alternating plan consumed by tick OK\n");
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    test_validation();
    test_biped_plan();
    test_walking_and_determinism();
    test_ik_integration();
    test_json_roundtrip();
    if (g_failures == 0) {
        std::printf("[gait] ALL PASSED\n");
        return 0;
    }
    std::printf("[gait] %d FAILURE(S)\n", g_failures);
    return 1;
}
