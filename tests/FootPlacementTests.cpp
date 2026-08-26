// FootPlacementTests (FALTANTES §18 item 5): proves the public foot-
// placement contract. The placer re-anchors a gait plan to the LIVE terrain:
// planted feet rest ON the surface (voxel world top-down scan), dynamic
// voxel edits move the surface under a planted foot, moving surfaces carry
// the foot, vertical steps beyond maxStepHeight are clamped and reported
// (never teleported), unknown terrain (unloaded chunk) keeps the plan's own
// y, the next-stance landing is re-anchored, and the whole thing is
// deterministic. The gate also proves the composition planner -> placement ->
// IK: a placed foot target is solved by IMotionDatabase::ik_two_bone and the
// foot lands exactly on the terrain surface.
#include "engine/animation/IFootPlacement.hpp"
#include "engine/animation/IGaitPlanner.hpp"
#include "engine/animation/IMotionDatabase.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <chrono>
#include <cmath>
#include <thread>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::printf("[foot] FAIL: %s\n", message);
        ++g_failures;
    }
}

void checkClose(float a, float b, float eps, const char* message) {
    if (std::fabs(a - b) > eps) {
        std::printf("[foot] FAIL: %s (%.6f vs %.6f)\n", message, a, b);
        ++g_failures;
    }
}

void checkVec(const glm::vec3& a, const glm::vec3& b, float eps,
              const char* message) {
    if (glm::distance(a, b) > eps) {
        std::printf("[foot] FAIL: %s (%.4f,%.4f,%.4f vs %.4f,%.4f,%.4f)\n",
                    message, a.x, a.y, a.z, b.x, b.y, b.z);
        ++g_failures;
    }
}

// Deterministic flat world (mirrors voxel_sdk_tests / connectivity_tests).
class FlatGenerator final : public engine::voxel::IVoxelGenerator {
public:
    explicit FlatGenerator(int height) : height_(height) {}
    engine::voxel::TerrainPoint sample(float, float) const override {
        engine::voxel::TerrainPoint point;
        point.height = height_;
        point.temperature = 0.5f;
        point.moisture = 0.5f;
        point.slope = 0.0f;
        return point;
    }
    float cave_density(float, float, float) const override { return -1.0f; }
    float ore_density(float, float, float) const override { return -1.0f; }

private:
    int height_;
};

bool boot_world(engine::voxel::IVoxelWorld& world, const glm::vec3& player,
                int budget, int maxBudgetMs = 8000) {
    world.set_chunk_budget(budget);
    const auto start = std::chrono::steady_clock::now();
    while (!world.is_chunk_loaded(0, 0)) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count() > maxBudgetMs) {
            return false;
        }
        world.update(player, 1.0f / 60.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

constexpr int kGroundTop = 130;  // FlatGenerator(130): top face at y=130

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
    gait.legPhases = {0.0f, 0.5f, 0.25f, 0.75f};
    return gait;
}

// A single-leg skeleton (root -> hip -> knee -> foot) with the SAME geometry
// as the quadruped's FL leg chain (hip 0.3,0.8,0.5; knee/foot -0.9 below).
engine::animation::MotionSkeleton make_single_leg_skeleton() {
    engine::animation::MotionSkeleton sk;
    sk.name = "leg";
    auto add = [&sk](const std::string& name, int parent, glm::vec3 t) {
        engine::animation::MotionBone b;
        b.name = name;
        b.parent = parent;
        b.localTranslation = t;
        sk.bones.push_back(b);
    };
    add("root", -1, glm::vec3(0));
    add("hip", 0, glm::vec3(0.3f, 0.8f, 0.5f));
    add("knee", 1, glm::vec3(0, -0.9f, 0));
    add("foot", 2, glm::vec3(0, -0.9f, 0));
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

// 1. The voxel sampler: top-down surface scan, known=false on unloaded chunk.
void test_voxel_sampler() {
    auto world = engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(kGroundTop));
    check(boot_world(*world, glm::vec3(8.0f, 200.0f, 8.0f), 4),
          "sampler: world boots");
    auto sampler = engine::animation::create_voxel_foot_terrain_sampler(
        *world, 200.0f, 0.0f);
    const engine::animation::SurfaceSample s =
        sampler->sample(8.0f, 8.0f);
    check(s.known, "sampler: column known on loaded chunk");
    // The world fills y <= height, so the top face is height + 1.
    checkClose(s.height, static_cast<float>(kGroundTop + 1), 1e-4f,
               "sampler: surface == ground top face");
    // Unloaded chunk (far away, budget 4) -> unknown, never a guessed height.
    const engine::animation::SurfaceSample far =
        sampler->sample(512.0f, 512.0f);
    check(!far.known, "sampler: unloaded chunk reports unknown");
    std::printf("[foot] voxel sampler OK\n");
}

// 2. Placement on flat terrain: planted feet rest ON the surface, swinging
//    feet keep their arc, landing is re-anchored.
void test_flat_placement() {
    auto world = engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(kGroundTop));
    check(boot_world(*world, glm::vec3(8.0f, 200.0f, 8.0f), 4),
          "flat: world boots");
    auto sampler = engine::animation::create_voxel_foot_terrain_sampler(
        *world, 200.0f, 0.0f);

    // BUG-022: boot_world only waits for chunk(0,0); FR/BL/BR foot columns
    // sit in ring-1 chunks which can still be Generating when the sampler
    // samples (sampler reports known=false -> surface -0.8 -> flaky FAIL).
    // Wait until all four foot columns are readable before planting.
    {
        const float kFootXz[4][2] = {
            {0.3f, 0.5f}, {-0.3f, 0.5f}, {0.3f, -0.5f}, {-0.3f, -0.5f}};
        const auto start = std::chrono::steady_clock::now();
        bool ready = false;
        while (!ready && std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start).count() < 8000) {
            ready = true;
            for (const auto& c : kFootXz) {
                const auto s = sampler->sample(c[0], c[1]);
                if (!s.known) { ready = false; break; }
            }
            if (!ready) {
                world->update(glm::vec3(8.0f, 200.0f, 8.0f), 1.0f / 60.0f);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        check(ready, "flat: all four foot columns readable before planting");
    }

    auto planner = engine::animation::create_contact_planner();
    auto placer = engine::animation::create_foot_placer();

    engine::animation::GaitPlan plan;
    std::string err;
    // Body at ground level, walking forward at 1 m/s.
    const glm::vec3 body(0.0f, 0.0f, 0.0f);
    check(planner->plan(make_quadruped(), 0.0f, body, 0.0f,
                        glm::vec2(0.0f, 1.0f), plan, err),
          "flat: plan succeeds");
    engine::animation::FootPlacementSpec spec;
    engine::animation::FootPlacementResult placed, prev;
    check(placer->place(spec, *sampler, plan, prev, placed, err),
          "flat: placement succeeds");
    check(placed.feet.size() == 4, "flat: all feet placed");
    for (std::size_t i = 0; i < placed.feet.size(); ++i) {
        check(placed.feet[i].surfaceKnown, "flat: surface known");    checkClose(placed.feet[i].surfaceHeight,
               static_cast<float>(kGroundTop + 1), 1e-4f,
               "flat: surface height == ground top face");
        if (placed.feet[i].stance) {
            checkClose(placed.feet[i].targetWorld.y,
                       static_cast<float>(kGroundTop + 1) +
                           spec.footRestHeight,
                       1e-4f, "flat: planted foot ON the surface");
        } else {
            // Swing foot keeps its arc above the surface.
            check(placed.feet[i].targetWorld.y >=
                      static_cast<float>(kGroundTop + 1) + 1e-3f,
                  "flat: swing foot arcs above the surface");
        }
        checkClose(placed.feet[i].landing.y,
                   static_cast<float>(kGroundTop + 1) + spec.footRestHeight,
                   1e-4f, "flat: landing re-anchored to the surface");
        check(!placed.feet[i].stepLimited, "flat: no step limit");
    }
    std::printf("[foot] flat placement OK\n");
}

// 3. Dynamic voxel terrain: digging under a planted foot moves the surface
//    DOWN (within the step window) on the next placement; placing a block
//    raises it.
void test_dynamic_terrain() {
    auto world = engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(kGroundTop));
    check(boot_world(*world, glm::vec3(8.0f, 200.0f, 8.0f), 4),
          "dynamic: world boots");
    auto sampler = engine::animation::create_voxel_foot_terrain_sampler(
        *world, 200.0f, 0.0f);
    auto planner = engine::animation::create_contact_planner();
    auto placer = engine::animation::create_foot_placer();
    engine::animation::FootPlacementSpec spec;
    spec.maxStepHeight = 5.0f;  // permissive: the dig is a 1-block drop

    engine::animation::GaitPlan plan;
    std::string err;
    const glm::vec3 body(0.0f, 0.0f, 0.0f);
    check(planner->plan(make_quadruped(), 0.0f, body, 0.0f,
                        glm::vec2(0.0f, 1.0f), plan, err),
          "dynamic: plan succeeds");
    engine::animation::FootPlacementResult a, b, empty;
    check(placer->place(spec, *sampler, plan, empty, a, err),
          "dynamic: first placement");
    const float before = a.feet[0].targetWorld.y;
    checkClose(before, static_cast<float>(kGroundTop + 1), 1e-4f,
               "dynamic: foot on the surface before the dig");

    // Dig the top block (y = kGroundTop) under FL's column: the surface
    // drops from 131 to 130.
    const int fx = static_cast<int>(std::floor(plan.feet[0].targetWorld.x));
    const int fz = static_cast<int>(std::floor(plan.feet[0].targetWorld.z));
    world->set_block(fx, kGroundTop, fz, 0);  // air
    check(placer->place(spec, *sampler, plan, a, b, err),
          "dynamic: placement after dig");
    checkClose(b.feet[0].targetWorld.y, static_cast<float>(kGroundTop), 1e-4f,
               "dynamic: foot follows the dug surface down");
    check(!b.feet[0].stepLimited, "dynamic: 1-block drop within step window");

    // Fill the hole back up (surface back to 131).
    world->set_block(fx, kGroundTop, fz, 3);
    engine::animation::FootPlacementResult c;
    check(placer->place(spec, *sampler, plan, b, c, err),
          "dynamic: placement after refill");
    checkClose(c.feet[0].targetWorld.y, static_cast<float>(kGroundTop + 1),
               1e-4f, "dynamic: foot follows the raised surface back up");
    std::printf("[foot] dynamic voxel terrain OK\n");
}

// 4. Moving surfaces: a platform that rises between placements carries the
//    planted foot (within the step window); a jump beyond maxStepHeight is
//    clamped and reported as stepLimited.
class MovingPlatformSampler final : public engine::animation::IFootTerrainSampler {
public:
    explicit MovingPlatformSampler(float baseHeight, float platformTop,
                                   float platformX, float platformZ,
                                   float halfSize)
        : base_(baseHeight), top_(platformTop), px_(platformX), pz_(platformZ),
          half_(halfSize) {}
    void set_platform_top(float top) { top_ = top; }
    engine::animation::SurfaceSample sample(float x, float z) const override {
        engine::animation::SurfaceSample s;
        if (std::fabs(x - px_) <= half_ && std::fabs(z - pz_) <= half_) {
            s.known = true;
            s.height = top_;
        } else {
            s.known = true;
            s.height = base_;
        }
        return s;
    }

private:
    float base_, top_, px_, pz_, half_;
};

void test_moving_surface() {
    auto planner = engine::animation::create_contact_planner();
    auto placer = engine::animation::create_foot_placer();
    engine::animation::FootPlacementSpec spec;
    spec.maxStepHeight = 1.0f;

    // FL's planted column sits on a 0.5-wide platform centered at (0, 0.5).
    MovingPlatformSampler terrain(static_cast<float>(kGroundTop + 1),
                                  static_cast<float>(kGroundTop + 1), 0.0f,
                                  0.5f, 0.5f);
    engine::animation::GaitPlan plan;
    std::string err;
    const glm::vec3 body(0.0f, 0.0f, 0.0f);
    check(planner->plan(make_quadruped(), 0.0f, body, 0.0f,
                        glm::vec2(0.0f, 1.0f), plan, err),
          "moving: plan succeeds");
    engine::animation::FootPlacementResult a, b, c, empty;
    check(placer->place(spec, terrain, plan, empty, a, err),
          "moving: first placement");
    checkClose(a.feet[0].targetWorld.y, static_cast<float>(kGroundTop + 1),
               1e-4f, "moving: foot on the platform at rest height");

    // Platform rises 0.4 m (within the 1.0 step window) -> foot follows.
    terrain.set_platform_top(static_cast<float>(kGroundTop + 1) + 0.4f);
    check(placer->place(spec, terrain, plan, a, b, err),
          "moving: placement after platform rise");
    checkClose(b.feet[0].targetWorld.y,
               static_cast<float>(kGroundTop + 1) + 0.4f, 1e-4f,
               "moving: foot carried up by the platform");
    check(!b.feet[0].stepLimited, "moving: 0.4 m rise within step window");

    // Platform drops 3 m (> maxStepHeight) -> clamped, stepLimited reported.
    terrain.set_platform_top(static_cast<float>(kGroundTop + 1) - 2.6f);
    check(placer->place(spec, terrain, plan, b, c, err),
          "moving: placement after platform drop");
    checkClose(c.feet[0].targetWorld.y, b.feet[0].targetWorld.y - 1.0f, 1e-4f,
               "moving: foot clamped to the step window");
    check(c.feet[0].stepLimited, "moving: big drop reported step-limited");
    std::printf("[foot] moving surfaces OK\n");
}

// 5. Unknown terrain keeps the plan's own y (never invented); determinism
//    between instances.
void test_unknown_and_determinism() {
    // A sampler that is unknown over the whole world.
    class UnknownSampler final : public engine::animation::IFootTerrainSampler {
    public:
        engine::animation::SurfaceSample sample(float, float) const override {
            return engine::animation::SurfaceSample{};  // known=false
        }
    } unknown;

    auto planner = engine::animation::create_contact_planner();
    auto placerA = engine::animation::create_foot_placer();
    auto placerB = engine::animation::create_foot_placer();
    engine::animation::FootPlacementSpec spec;
    engine::animation::GaitPlan plan;
    std::string err;
    const glm::vec3 body(0.0f, 0.0f, 0.0f);
    check(planner->plan(make_quadruped(), 0.0f, body, 0.0f,
                        glm::vec2(0.0f, 1.0f), plan, err),
          "unknown: plan succeeds");
    engine::animation::FootPlacementResult a, b, empty;
    check(placerA->place(spec, unknown, plan, empty, a, err) &&
              placerB->place(spec, unknown, plan, empty, b, err),
          "unknown: placements succeed");
    bool same = true;
    for (std::size_t i = 0; i < a.feet.size(); ++i) {
        if (a.feet[i].surfaceKnown != b.feet[i].surfaceKnown ||
            a.feet[i].targetWorld != b.feet[i].targetWorld ||
            a.feet[i].landing != b.feet[i].landing) {
            same = false;
        }
        // Unknown terrain: the plan's own y is preserved, not zeroed.
        checkClose(a.feet[i].targetWorld.y, plan.feet[i].targetWorld.y, 1e-6f,
                   "unknown: plan y preserved");
    }
    check(same, "unknown: deterministic between instances");
    std::printf("[foot] unknown terrain + determinism OK\n");
}

// 6. Planner -> placement -> IK: the placed foot target (on the terrain
//    surface) is solved by ik_two_bone and the foot lands on the surface.
void test_ik_integration() {
    auto world = engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(kGroundTop));
    check(boot_world(*world, glm::vec3(8.0f, 200.0f, 8.0f), 4),
          "ik: world boots");
    auto sampler = engine::animation::create_voxel_foot_terrain_sampler(
        *world, 200.0f, 0.0f);

    // BUG-022: boot_world only waits for chunk(0,0); the FR/BL/BR foot columns
    // sit in ring-1 chunks which can still be Generating when the placer
    // samples (sampler reports known=false -> surface -0.8 -> flaky FAIL).
    // Wait until all four foot columns are readable before planting.
    {
        const float kFootXz[4][2] = {
            {0.3f, 0.5f}, {-0.3f, 0.5f}, {0.3f, -0.5f}, {-0.3f, -0.5f}};
        const auto start = std::chrono::steady_clock::now();
        bool ready = false;
        while (!ready && std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start).count() < 8000) {
            ready = true;
            for (const auto& c : kFootXz) {
                const auto s = sampler->sample(c[0], c[1]);
                if (!s.known) { ready = false; break; }
            }
            if (!ready) {
                world->update(glm::vec3(8.0f, 200.0f, 8.0f), 1.0f / 60.0f);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        check(ready, "ik: all four foot columns readable before planting");
    }

    auto planner = engine::animation::create_contact_planner();
    auto placer = engine::animation::create_foot_placer();
    auto db = engine::animation::create_motion_database();
    std::string err;
    check(db->cook_skeleton(make_single_leg_skeleton(), err),
          "ik: single-leg skeleton cooks");

    engine::animation::GaitPlan plan;
    const glm::vec3 body(0.0f, 0.0f, 0.0f);
    check(planner->plan(make_quadruped(), 0.2f, body, 0.0f,
                        glm::vec2(0.0f, 1.0f), plan, err),
          "ik: plan succeeds");
    engine::animation::FootPlacementSpec spec;
    engine::animation::FootPlacementResult placed, empty;
    check(placer->place(spec, *sampler, plan, empty, placed, err),
          "ik: placement succeeds");
    const engine::animation::PlacedFoot& fl = placed.feet[0];
    check(fl.stance && fl.surfaceKnown, "ik: FL planted with known surface");
    checkClose(fl.targetWorld.y, static_cast<float>(kGroundTop + 1), 1e-4f,
               "ik: placed target on the surface");

    // Rest pose of the single-leg skeleton, then solve the chain to the
    // placed target.
    const engine::animation::MotionSkeleton sk = make_single_leg_skeleton();
    // Rest pose: the body root sits at terrain height so the hip (root +
    // hipOffset) is level with the surface — the chain must REACH the placed
    // foot target on the terrain.
    engine::animation::MotionPose pose;
    pose.translations.resize(sk.bones.size(), glm::vec3(0.0f));
    pose.rotations.resize(sk.bones.size(), glm::quat(1.0f, 0, 0, 0));
    pose.scales.resize(sk.bones.size(), glm::vec3(1.0f));
    pose.translations[0] =
        glm::vec3(0.0f, static_cast<float>(kGroundTop + 1) - 0.8f, 0.0f);
    pose.translations[1] = glm::vec3(0.3f, 0.8f, 0.5f);   // hip
    pose.translations[2] = glm::vec3(0.0f, -0.9f, 0.0f);  // knee
    pose.translations[3] = glm::vec3(0.0f, -0.9f, 0.0f);  // foot

    engine::animation::MotionPose solved;
    check(db->ik_two_bone(pose, 1, 2, 3, fl.targetWorld, glm::vec3(0, 0, 1),
                          1.0f, solved),
          "ik: leg chain solves the placed target");
    checkVec(modelPosition(solved, sk, 3), fl.targetWorld, 0.02f,
             "ik: foot lands on the placed terrain target");
    std::printf("[foot] planner -> placement -> IK OK\n");
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    test_voxel_sampler();
    test_flat_placement();
    test_dynamic_terrain();
    test_moving_surface();
    test_unknown_and_determinism();
    test_ik_integration();
    if (g_failures == 0) {
        std::printf("[foot] ALL PASSED\n");
        return 0;
    }
    std::printf("[foot] %d FAILURE(S)\n", g_failures);
    return 1;
}
