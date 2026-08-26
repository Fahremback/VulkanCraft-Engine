// SceneCullingTests.cpp — Agente 1 (task_plan B.8): headless gate for the
// PUBLIC scene culling contract (ISceneCulling). Proves the deterministic
// pure core: frustum extraction + AABB/sphere visibility, distance LOD with
// hysteresis, conservative occlusion and instance grouping — no GPU required.

#include "engine/rendering/ISceneCulling.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::printf("FAIL: %s\n", message);
        ++g_failures;
    }
}

void check(bool condition, const std::string& message) {
    check(condition, message.c_str());
}

bool near(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps;
}

using Engine::Rendering::ISceneCulling;
using Engine::Rendering::SceneCullingConfig;
using Engine::Rendering::SceneInstance;
using Engine::Rendering::InstanceGroup;
using Engine::Rendering::Frustum;
using Engine::Rendering::create_scene_culling;
using Engine::Rendering::create_scene_culling_json;

// A camera at the origin looking down -Z with a 60-degree vertical FOV.
glm::mat4 testViewProj() {
    const glm::mat4 proj = glm::perspective(
        60.0f * 3.14159265f / 180.0f, 1.3333f, 0.1f, 1000.0f);
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f),
                                       glm::vec3(0.0f, 0.0f, -1.0f),
                                       glm::vec3(0.0f, 1.0f, 0.0f));
    return proj * view;
}

}  // namespace

int main() {
    using namespace Engine::Rendering;

    // ---- 1. default config + all-or-nothing refusal ----
    {
        std::string error;
        auto c = create_scene_culling(error);
        check(c != nullptr, "default scene culling created");
        check(error.empty(), "default config diagnostic empty");

        SceneCullingConfig bad = c->config();
        bad.lod0Distance = 0.0f;
        check(!c->configure(bad, error) && !error.empty(),
              "lod0Distance 0 refused");
        bad = c->config();
        bad.lod0Distance = std::numeric_limits<float>::quiet_NaN();
        check(!c->configure(bad, error) && !error.empty(), "NaN refused");
        bad = c->config();
        bad.lodHysteresis = 0.75f;
        check(!c->configure(bad, error) && !error.empty(),
              "lodHysteresis 0.75 refused");
        bad = c->config();
        bad.maxInstances = 0;
        check(!c->configure(bad, error) && !error.empty(),
              "maxInstances 0 refused");
        check(c->config().lod0Distance == 32.0f,
              "config unchanged after refusals (never clamps)");
    }

    // ---- 2. JSON round-trip (bit-exact) + refusals ----
    {
        std::string error;
        auto a = create_scene_culling(error);
        SceneCullingConfig cfg = a->config();
        cfg.lod0Distance = 64.0f;
        cfg.lodHysteresis = 0.2f;
        cfg.maxInstances = 2048;
        check(a->configure(cfg, error), "custom config applied");

        auto b = create_scene_culling_json(a->config_to_json(), error);
        check(b != nullptr && error.empty(), "json round-trip creates");
        check(b->config().lod0Distance == 64.0f &&
                  b->config().lodHysteresis == 0.2f &&
                  b->config().maxInstances == 2048,
              "json round-trip bit-exact");

        check(create_scene_culling_json("{ \"version\": 1, \"bogus\": 1 }",
                                        error) == nullptr,
              "unknown key refused");
        check(create_scene_culling_json("{ \"version\": 2 }", error) == nullptr,
              "unsupported version refused");
        check(create_scene_culling_json(
                  "{ \"version\": 1, \"lodHysteresis\": 2.0 }", error) == nullptr,
              "invalid json config refused all-or-nothing");
    }

    // ---- 3. frustum extraction + visibility ----
    {
        std::string error;
        auto c = create_scene_culling(error);
        const glm::mat4 vp = testViewProj();
        const Frustum f = c->extractFrustum(vp);

        // A point straight ahead is inside.
        check(c->sphereVisible(f, glm::vec3(0.0f, 0.0f, -10.0f), 0.5f),
              "sphere in front is visible");
        // Behind the camera is outside.
        check(!c->sphereVisible(f, glm::vec3(0.0f, 0.0f, 10.0f), 0.5f),
              "sphere behind the camera is culled");
        // Far to the side (beyond the 60-degree FOV at 10m) is outside.
        const float halfW = 10.0f * std::tan(30.0f * 3.14159265f / 180.0f) *
                            1.3333f;
        check(!c->sphereVisible(f, glm::vec3(halfW + 0.2f, 0.0f, -10.0f), 0.1f),
              "sphere beyond the horizontal FOV is culled");
        // A sphere straddling the boundary with radius is still visible.
        check(c->sphereVisible(f, glm::vec3(halfW + 0.05f, 0.0f, -10.0f), 0.2f),
              "sphere straddling the frustum boundary stays visible");

        // AABB in front: visible; fully to the side: culled; behind: culled.
        check(c->aabbVisible(f, glm::vec3(-1.0f, -1.0f, -11.0f),
                             glm::vec3(1.0f, 1.0f, -9.0f)),
              "aabb in front is visible");
        check(!c->aabbVisible(f, glm::vec3(halfW + 1.0f, -1.0f, -11.0f),
                              glm::vec3(halfW + 3.0f, 1.0f, -9.0f)),
              "aabb beyond the FOV is culled");
        check(!c->aabbVisible(f, glm::vec3(-1.0f, -1.0f, 9.0f),
                              glm::vec3(1.0f, 1.0f, 11.0f)),
              "aabb behind the camera is culled");
    }

    // ---- 4. distance LOD ----
    {
        std::string error;
        auto c = create_scene_culling(error);
        check(c->selectLod(0.0f) == 0 && c->selectLod(10.0f) == 0,
              "lod 0 inside the first band");
        check(c->selectLod(32.0f) == 1 && c->selectLod(40.0f) == 1,
              "lod 1 at [32, 64)");
        check(c->selectLod(64.0f) == 2 && c->selectLod(100.0f) == 2,
              "lod 2 at [64, 128)");
        check(c->selectLod(128.0f) == 3 && c->selectLod(10000.0f) == 3,
              "lod 3 beyond 128");
        check(c->selectLod(-5.0f) == 0, "negative distance clamps to lod 0");
    }

    // ---- 5. LOD hysteresis: no popping at the boundary ----
    {
        std::string error;
        auto c = create_scene_culling(error);
        // lod0Distance 32, hysteresis 0.15: enter 27.2, leave 36.8.
        check(c->selectLodHysteretic(10.0f, 0) == 0, "near, lod 0 stays 0");
        // Moving out: at 33 the plain selector says 1, but the leave threshold
        // is 36.8, so with hysteresis we stay at 0.
        check(c->selectLod(33.0f) == 1, "plain selector: 33 -> lod 1");
        check(c->selectLodHysteretic(33.0f, 0) == 0,
              "hysteresis: 33 with current lod 0 stays 0 (no pop)");
        check(c->selectLodHysteretic(37.0f, 0) == 1,
              "hysteresis: 37 crosses the leave threshold -> lod 1");
        // Moving back in: at 30 the plain selector says 0, but the enter
        // threshold for lod 1 is 27.2, so we stay at 1.
        check(c->selectLod(30.0f) == 0, "plain selector: 30 -> lod 0");
        check(c->selectLodHysteretic(30.0f, 1) == 1,
              "hysteresis: 30 with current lod 1 stays 1 (no pop)");
        check(c->selectLodHysteretic(26.0f, 1) == 0,
              "hysteresis: 26 crosses the enter threshold -> lod 0");
    }

    // ---- 6. conservative occlusion ----
    {
        std::string error;
        auto c = create_scene_culling(error);
        const glm::mat4 vp = testViewProj();

        // A big occluder straight ahead; a small box fully behind it.
        const glm::vec3 occMin(-2.0f, -2.0f, -15.0f);
        const glm::vec3 occMax(2.0f, 2.0f, -13.0f);
        check(c->occluded(vp, occMin, occMax, glm::vec3(-0.5f, -0.5f, -20.0f),
                          glm::vec3(0.5f, 0.5f, -19.0f)),
              "box fully behind a larger occluder is occluded");

        // A box only partially behind (sticks out sideways enough to escape
        // the occluder's solid angle) is not occluded. Note: a box at 2.5m
        // world-x at 20m still projects inside the occluder at 15m (perspective
        // foreshortening) and its rays cross the occluder -> conservative
        // culling keeps it hidden; 3.0m+ truly escapes.
        check(!c->occluded(vp, occMin, occMax, glm::vec3(3.0f, -0.5f, -20.0f),
                           glm::vec3(4.0f, 0.5f, -19.0f)),
              "box sticking out of the occluder solid angle is not occluded");
        check(c->occluded(vp, occMin, occMax, glm::vec3(1.5f, -0.5f, -20.0f),
                          glm::vec3(2.5f, 0.5f, -19.0f)),
              "box behind the occluder solid angle stays occluded (conservative)");

        // A box in FRONT of the occluder is not occluded (nearest depth closer).
        check(!c->occluded(vp, occMin, occMax, glm::vec3(-0.5f, -0.5f, -12.0f),
                           glm::vec3(0.5f, 0.5f, -11.0f)),
              "box in front of the occluder is not occluded");
    }

    // ---- 7. instance grouping (instancing / meshlet stream) ----
    {
        std::string error;
        auto c = create_scene_culling(error);
        std::vector<SceneInstance> insts;
        // two trees (mesh 1, material 1), one rock (mesh 2, material 2), two
        // rocks (mesh 2, material 3) -> 3 groups with counts 2/1/2
        SceneInstance a;
        a.mesh = 1; a.material = 1; a.position = glm::vec3(0.0f, 0.0f, -5.0f);
        a.scale = glm::vec3(1.0f, 2.0f, 1.0f);
        insts.push_back(a);
        SceneInstance b = a;
        b.position = glm::vec3(4.0f, 0.0f, -5.0f);
        insts.push_back(b);
        SceneInstance r;
        r.mesh = 2; r.material = 2; r.position = glm::vec3(0.0f, 0.0f, -8.0f);
        r.scale = glm::vec3(0.5f, 0.5f, 0.5f);
        insts.push_back(r);
        SceneInstance r2 = r;
        r2.material = 3; r2.position = glm::vec3(2.0f, 0.0f, -8.0f);
        insts.push_back(r2);
        SceneInstance r3 = r2;
        r3.position = glm::vec3(2.0f, 0.0f, -9.0f);
        insts.push_back(r3);

        std::vector<InstanceGroup> groups;
        check(c->buildInstanceGroups(insts, groups, error),
              "instance grouping runs");
        check(groups.size() == 3, "three distinct (mesh, material) groups");
        check(groups[0].count == 2 && groups[0].mesh == 1 &&
                  groups[0].material == 1,
              "two trees merged into one group of 2");
        check(groups[1].count == 1 && groups[1].mesh == 2 &&
                  groups[1].material == 2,
              "single rock (material 2) is its own group");
        check(groups[2].count == 2 && groups[2].material == 3,
              "two rocks (material 3) merged");
        // Combined AABB of the tree group covers both trees.
        check(near(groups[0].aabbMin.x, -1.0f) && near(groups[0].aabbMax.x, 5.0f),
              "group AABB covers all merged instances");
        // Deterministic: same input, same groups bit-exact.
        std::vector<InstanceGroup> groups2;
        check(c->buildInstanceGroups(insts, groups2, error), "regroup runs");
        check(groups.size() == groups2.size() &&
                  std::memcmp(groups.data(), groups2.data(),
                              groups.size() * sizeof(InstanceGroup)) == 0,
              "instance grouping is deterministic bit-exact");

        // Refusal: stream over maxInstances.
        SceneCullingConfig cfg = c->config();
        cfg.maxInstances = 3;
        check(c->configure(cfg, error), "small budget applied");
        std::vector<InstanceGroup> emptyG;
        check(!c->buildInstanceGroups(insts, emptyG, error) && !error.empty() &&
                  emptyG.empty(),
              "stream over maxInstances refused all-or-nothing");
    }

    // ---- 8. determinism (bit-exact) ----
    {
        std::string error;
        auto c = create_scene_culling(error);
        const glm::mat4 vp = testViewProj();
        const Frustum a = c->extractFrustum(vp);
        const Frustum b = c->extractFrustum(vp);
        check(std::memcmp(&a, &b, sizeof(Frustum)) == 0,
              "frustum extraction is deterministic bit-exact");
        const bool v1 = c->sphereVisible(a, glm::vec3(0.3f, 0.2f, -7.0f), 0.7f);
        const bool v2 = c->sphereVisible(a, glm::vec3(0.3f, 0.2f, -7.0f), 0.7f);
        check(v1 == v2, "visibility tests are deterministic");
    }

    if (g_failures == 0) {
        std::printf("[scene-culling] ALL PASSED\n");
        return 0;
    }
    std::printf("[scene-culling] %d FAILURE(S)\n", g_failures);
    return 1;
}
