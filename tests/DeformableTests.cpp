// DeformableTests.cpp
//
// Evidence for FALTANTES §16 item 3: the public IDeformableProvider contract
// with XPBD (position-based dynamics) as the implemented specialized plugin
// and FEMFX as the opt-in seam:
//   - factory: Xpbd creates the solver; FEMFX is REFUSED with a diagnostic
//     (a missing plugin is never mistaken for a working deformable);
//   - validation: bad config / empty mesh / over-cap / bad edges refused;
//   - gravity: a hanging chain sags under gravity yet stays connected (the
//     distance constraints hold within a bounded error);
//   - force propagation: a lateral force on the free end drags its neighbors
//     through the constraints;
//   - elastic recovery: removing the force returns the chain toward rest
//     (the constraints are compliant springs, not plastic);
//   - ground collision: nodes never pass below the ground plane;
//   - convergence: more solver iterations yield smaller constraint error;
//   - determinism: identical provider + body + steps -> bit-identical nodes.

#include <engine/deformable/IDeformableProvider.hpp>

#include <glm/glm.hpp>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace Engine::Deformable;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what);
        ++g_failures;
    }
}

// A vertical chain: node 0 fixed at the top, 3 free nodes below it, one edge
// per consecutive pair. The canonical XPBD scenario.
DeformableMeshDesc make_chain() {
    DeformableMeshDesc desc;
    desc.nodes = { glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f),
                   glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.0f, 3.0f, 0.0f) };
    desc.edges = { { 0, 1 }, { 1, 2 }, { 2, 3 } };
    desc.fixed = { true, false, false, false };
    return desc;
}

// 1. Factory + plugin seam: Xpbd creates the solver; FEMFX is refused with a
//    diagnostic; invalid configs are refused (never clamped).
void test_factory_and_validation() {
    DeformableConfig config;
    std::string error;
    std::unique_ptr<IDeformableProvider> xpbd =
        create_deformable_provider(DeformableProviderKind::Xpbd, config, error);
    check(xpbd != nullptr && xpbd->kind() == DeformableProviderKind::Xpbd,
          "Xpbd provider created by the factory");
    std::unique_ptr<IDeformableProvider> femfx =
        create_deformable_provider(DeformableProviderKind::Femfx, config, error);
    check(femfx == nullptr && !error.empty(),
          "FEMFX refused with a diagnostic (specialized plugin seam)");

    // Config JSON validation (all-or-nothing).
    DeformableConfig loaded;
    check(!loaded.load_from_json(R"({"stiffness":0})", error),
          "stiffness 0 refused");
    check(!loaded.load_from_json(R"({"substeps":0})", error),
          "substeps 0 refused");
    check(!loaded.load_from_json(R"({"solverIterations":0})", error),
          "solverIterations 0 refused");
    check(!loaded.load_from_json(R"({"damping":1})", error),
          "damping 1 refused");
    check(!loaded.load_from_json(R"({"maxNodes":0})", error),
          "maxNodes 0 refused");
    check(loaded.load_from_json(
              R"({"substeps":3,"solverIterations":16,"stiffness":0.9,)"
              R"("damping":0.2,"gravityY":-5,"bounce":0.5})", error) &&
              loaded.substeps == 3 && loaded.solverIterations == 16 &&
              loaded.stiffness == 0.9f && loaded.damping == 0.2f &&
              loaded.gravity.y == -5.0f && loaded.bounce == 0.5f,
          "valid config loads and round-trips");

    // Body validation.
    DeformableConfig small;
    small.maxNodes = 2;
    std::unique_ptr<IDeformableProvider> provider =
        create_deformable_provider(DeformableProviderKind::Xpbd, small, error);
    check(provider != nullptr, "small-cap provider created");

    DeformableMeshDesc empty;
    check(provider->create_body(empty, error) == InvalidDeformableBody && !error.empty(),
          "empty mesh refused");

    DeformableMeshDesc overCap;
    overCap.nodes = { glm::vec3(0.0f), glm::vec3(1.0f), glm::vec3(2.0f) };
    check(provider->create_body(overCap, error) == InvalidDeformableBody,
          "mesh over maxNodes refused");

    DeformableMeshDesc badFixed;
    badFixed.nodes = { glm::vec3(0.0f), glm::vec3(1.0f) };
    badFixed.fixed = { true };
    check(provider->create_body(badFixed, error) == InvalidDeformableBody,
          "fixed flags mismatched with the nodes refused");

    DeformableMeshDesc badEdge;
    badEdge.nodes = { glm::vec3(0.0f), glm::vec3(1.0f) };
    badEdge.edges = { { 0, 5 } };
    check(provider->create_body(badEdge, error) == InvalidDeformableBody,
          "edge referencing an invalid node refused");

    DeformableConfig full;
    std::unique_ptr<IDeformableProvider> ok =
        create_deformable_provider(DeformableProviderKind::Xpbd, full, error);
    const DeformableBodyHandle body = ok->create_body(make_chain(), error);
    check(body != InvalidDeformableBody && ok->body_count() == 1 &&
              ok->node_count(body) == 4,
          "valid chain body created");
    std::printf("[deformable] factory/plugin seam/validation OK\n");
}

// 2. Gravity: the hanging chain sags (the free end drops) yet stays
//    connected — the distance constraints hold within a bounded error.
void test_gravity_sag() {
    DeformableConfig config;
    config.stiffness = 0.95f;
    config.groundCollision = false;  // the hanging chain floats in free space
    std::string err;
    std::unique_ptr<IDeformableProvider> provider =
        create_deformable_provider(DeformableProviderKind::Xpbd, config, err);
    std::string error;
    const DeformableBodyHandle body = provider->create_body(make_chain(), error);
    check(body != InvalidDeformableBody, "chain body created");

    for (int i = 0; i < 360; ++i) provider->step(1.0f / 60.0f);
    const float y3 = provider->node_position(body, 3).y;
    const float y2 = provider->node_position(body, 2).y;
    check(y3 < 2.95f && y3 > 2.0f,
          "free end sagged under gravity yet did not collapse");
    check(y3 > y2, "chain order preserved (node 3 below node 2)");
    check(provider->constraint_error(body) < 0.5f,
          "distance constraints hold within a bounded error");
    std::printf("[deformable] gravity sag (y3=%.3f, err=%.4f) OK\n",
                y3, provider->constraint_error(body));
}

// 3. Force propagation: a lateral force on the free end drags its neighbors
//    through the constraints (the chain stays connected).
void test_force_propagation() {
    DeformableConfig config;
    config.stiffness = 0.95f;
    config.groundCollision = false;
    std::string err;
    std::unique_ptr<IDeformableProvider> provider =
        create_deformable_provider(DeformableProviderKind::Xpbd, config, err);
    std::string error;
    const DeformableBodyHandle body = provider->create_body(make_chain(), error);

    for (int i = 0; i < 120; ++i) provider->step(1.0f / 60.0f);  // settle vertical
    provider->apply_force(body, 3, glm::vec3(60.0f, 0.0f, 0.0f));
    for (int i = 0; i < 60; ++i) provider->step(1.0f / 60.0f);

    const float x3 = provider->node_position(body, 3).x;
    const float x1 = provider->node_position(body, 1).x;
    check(x3 > 0.08f, "pulled node moved along the force");
    check(x1 > 0.03f,
          "force propagated to the neighbors through the constraints");
    check(x3 > x1,
          "the far end moves more than the near end (pendulum geometry)");
    check(provider->constraint_error(body) < 1.0f,
          "chain stayed connected under the pull");
    std::printf("[deformable] force propagation (x1=%.3f x3=%.3f, err=%.4f) OK\n",
                x1, x3, provider->constraint_error(body));
}

// 4. Elastic recovery: the compliant constraint is a SPRING, not plastic — a
//    single-edge body (the cleanest dynamic) pushed sideways swings back to
//    vertical after the force stops.
void test_elastic_recovery() {
    DeformableConfig config;
    config.stiffness = 0.9f;
    config.damping = 0.2f;
    config.groundCollision = false;
    std::string err;
    std::unique_ptr<IDeformableProvider> provider =
        create_deformable_provider(DeformableProviderKind::Xpbd, config, err);
    DeformableMeshDesc desc;
    desc.nodes = { glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f) };
    desc.edges = { { 0, 1 } };
    desc.fixed = { true, false };
    std::string error;
    const DeformableBodyHandle body = provider->create_body(desc, error);
    check(body != InvalidDeformableBody, "single-edge body created");

    for (int i = 0; i < 120; ++i) provider->step(1.0f / 60.0f);  // hang vertical
    provider->apply_force(body, 1, glm::vec3(100.0f, 0.0f, 0.0f));
    for (int i = 0; i < 600; ++i) provider->step(1.0f / 60.0f);  // settle the lean
    const float pushed = provider->node_position(body, 1).x;
    check(pushed > 0.5f, "force displaced the free end to a stable lean");

    for (int i = 0; i < 1500; ++i) provider->step(1.0f / 60.0f);  // release + settle
    const float recovered = provider->node_position(body, 1).x;
    check(recovered < pushed * 0.25f,
          "elastic recovery: the free end returned toward vertical");
    std::printf("[deformable] elastic recovery (pushed=%.3f recovered=%.3f) OK\n",
                pushed, recovered);
}

// 5. Ground collision: nodes never pass below the ground plane; with bounce
//    they reflect off it.
void test_ground_collision() {
    DeformableConfig config;
    config.groundCollision = true;
    config.groundY = 0.0f;
    config.bounce = 0.5f;
    std::string err;
    std::unique_ptr<IDeformableProvider> provider =
        create_deformable_provider(DeformableProviderKind::Xpbd, config, err);

    DeformableMeshDesc single;
    single.nodes = { glm::vec3(0.0f, 2.0f, 0.0f) };  // a lone falling node
    std::string error;
    const DeformableBodyHandle body = provider->create_body(single, error);
    check(body != InvalidDeformableBody, "single node body created");

    bool bounced = false;
    bool everBelow = false;
    for (int i = 0; i < 240; ++i) {
        provider->step(1.0f / 60.0f);
        const float y = provider->node_position(body, 0).y;
        if (y < -1.0e-4f) everBelow = true;
        if (provider->node_velocity(body, 0).y > 0.05f && y < 0.1f) bounced = true;
    }
    check(!everBelow, "node never went below the ground plane");
    check(bounced, "bounce 0.5 reflected the node off the ground");
    const float rest = provider->node_position(body, 0).y;
    check(rest >= -1.0e-4f && rest < 0.05f,
          "node settled on the ground plane");
    std::printf("[deformable] ground collision (rest y=%.4f, bounced=%d) OK\n",
                rest, bounced ? 1 : 0);
}

// 6. Convergence: more solver iterations yield a smaller constraint error
//    after the same simulation time.
void test_convergence() {
    // Compare the TRANSIENT (few steps), not the steady state: at rest the
    // violation is set by gravity+compliance regardless of iterations; more
    // iterations solve MORE of the initial violation per step.
    const auto run = [](int iterations) {
        DeformableConfig config;
        config.stiffness = 0.5f;  // compliant: iterations visibly matter
        config.solverIterations = iterations;
        config.groundCollision = false;
        std::string err;
        std::unique_ptr<IDeformableProvider> provider =
            create_deformable_provider(DeformableProviderKind::Xpbd, config, err);
        std::string error;
        const DeformableBodyHandle body = provider->create_body(make_chain(), error);
        for (int i = 0; i < 3; ++i) provider->step(1.0f / 60.0f);
        return provider->constraint_error(body);
    };
    const float error2 = run(2);
    const float error32 = run(32);
    check(error32 < error2,
          "32 iterations converge to a strictly smaller constraint error than 2");
    std::printf("[deformable] convergence (err[2]=%.6f err[32]=%.6f) OK\n",
                error2, error32);
}

// 7. Determinism: identical provider + body + steps -> bit-identical nodes.
void test_determinism() {
    const auto run = []() {
        DeformableConfig config;
        config.stiffness = 0.95f;
        config.groundCollision = false;
        std::string err;
        std::unique_ptr<IDeformableProvider> provider =
            create_deformable_provider(DeformableProviderKind::Xpbd, config, err);
        std::string error;
        const DeformableBodyHandle body = provider->create_body(make_chain(), error);
        provider->apply_force(body, 3, glm::vec3(20.0f, 0.0f, 5.0f));
        for (int i = 0; i < 200; ++i) provider->step(1.0f / 60.0f);
        std::vector<glm::vec3> positions;
        for (std::uint32_t n = 0; n < 4; ++n)
            positions.push_back(provider->node_position(body, n));
        return positions;
    };
    const std::vector<glm::vec3> a = run();
    const std::vector<glm::vec3> b = run();
    bool identical = a.size() == b.size();
    if (identical) {
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (a[i] != b[i]) {
                identical = false;
                break;
            }
        }
    }
    check(identical, "identical scenarios -> bit-identical node positions");
    std::printf("[deformable] determinism OK\n");
}

}  // namespace

int main() {
    test_factory_and_validation();
    test_gravity_sag();
    test_force_propagation();
    test_elastic_recovery();
    test_ground_collision();
    test_convergence();
    test_determinism();
    if (g_failures == 0) {
        std::printf("[deformable] ALL PASSED\n");
        return 0;
    }
    std::printf("[deformable] %d FAILURE(S)\n", g_failures);
    return 1;
}
