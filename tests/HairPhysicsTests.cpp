// HairPhysicsTests.cpp — Gate for IHairPhysics (C.16 tressfx — Verlet + spring-mass)
#include <cstdio>
#include <cmath>
#include <cstring>
#include "engine/rendering/IHairPhysics.hpp"

static int g_passed = 0, g_failed = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("  FAIL: %s\n", msg); g_failed++; } \
    else { g_passed++; } \
} while(0)

#define CHECK_NEAR(a, b, eps, msg) do { \
    float _diff = std::fabs((a)-(b)); \
    if (_diff > (eps)) { std::printf("  FAIL: %s (got %f, expected %f, diff %f)\n", msg, (float)(a), (float)(b), _diff); g_failed++; } \
    else { g_passed++; } \
} while(0)

using namespace vc::rendering;

int main() {
    std::printf("[hair] ALL tests starting\n");

    // 1. Config validation
    std::printf("[hair] test config\n");
    {
        HairConfig c;
        CHECK(c.validate(), "default config valid");
        HairConfig bad; bad.damping = -1;
        CHECK(!bad.validate(), "negative damping invalid");
        HairConfig bad2; bad2.stiffness = 2.0f;
        CHECK(!bad2.validate(), "stiffness > 1 invalid");
        HairConfig bad3; bad3.dt = 0;
        CHECK(!bad3.validate(), "dt=0 invalid");
    }

    // 2. JSON round-trip
    std::printf("[hair] test JSON\n");
    {
        HairConfig c; c.gravity = -10; c.damping = 0.95f; c.stiffness = 0.8f;
        std::string json = c.toJson();
        auto c2 = HairConfig::fromJson(json);
        CHECK_NEAR(c2.gravity, -10.0f, 0.01f, "JSON gravity");
        CHECK_NEAR(c2.damping, 0.95f, 0.001f, "JSON damping");
        CHECK_NEAR(c2.stiffness, 0.8f, 0.001f, "JSON stiffness");
    }

    // 3. Create strand (root pinned)
    std::printf("[hair] test create strand\n");
    {
        std::string err;
        auto hp = create_hair_physics(HairConfig{}, err);
        std::vector<Vec3> positions = {{0,0,0}, {0,-1,0}, {0,-2,0}, {0,-3,0}};
        auto strand = hp->createStrand(positions);
        CHECK(hp->particleCount(strand) == 4, "4 particles");
        CHECK(strand.particles[0].invMass == 0, "root pinned");
        CHECK(strand.particles[1].invMass == 1, "tip free");
    }

    // 4. Verlet: single step gravity pulls free particles down
    std::printf("[hair] test verlet gravity\n");
    {
        std::string err;
        HairConfig cfg; cfg.gravity = -10; cfg.damping = 1.0f;
        auto hp = create_hair_physics(cfg, err);
        std::vector<Vec3> positions = {{0,0,0}, {0,-1,0}};
        auto strand = hp->createStrand(positions);
        hp->simulate(strand, cfg);
        // Root stays, second particle moves down by gravity*dt²
        CHECK_NEAR(strand.particles[0].position.x, 0.0f, 0.001f, "root stays x");
        CHECK_NEAR(strand.particles[0].position.y, 0.0f, 0.001f, "root stays y");
        CHECK(strand.particles[1].position.y < strand.particles[1].prevPosition.y, "tip moves down");
    }

    // 5. Root stays pinned after multiple steps
    std::printf("[hair] test root pinned\n");
    {
        std::string err;
        HairConfig cfg; cfg.gravity = -10;
        auto hp = create_hair_physics(cfg, err);
        std::vector<Vec3> positions = {{5,10,3}, {5,9,3}};
        auto strand = hp->createStrand(positions);
        for (int i = 0; i < 10; i++) hp->simulate(strand, cfg);
        CHECK_NEAR(strand.particles[0].position.x, 5.0f, 0.001f, "pinned x");
        CHECK_NEAR(strand.particles[0].position.y, 10.0f, 0.001f, "pinned y");
        CHECK_NEAR(strand.particles[0].position.z, 3.0f, 0.001f, "pinned z");
    }

    // 6. Damping: strand stays stable with different damping values
    std::printf("[hair] test damping\n");
    {
        std::string err;
        HairConfig cfg; cfg.gravity = -10; cfg.damping = 0.5f;
        auto hp = create_hair_physics(cfg, err);
        std::vector<Vec3> positions = {{0,0,0}, {0,-1,0}, {0,-2,0}, {0,-3,0}};
        auto strand = hp->createStrand(positions);
        for (int i = 0; i < 20; i++) hp->simulate(strand, cfg);
        // Root stays pinned
        CHECK_NEAR(strand.particles[0].position.y, 0.0f, 0.001f, "damping root pinned");
        // Tip doesn't explode (stays within reasonable bounds)
        CHECK(strand.particles[3].position.y < 0, "damping tip below root");
        CHECK(strand.particles[3].position.y > -100, "damping tip doesn't explode");
        // Monotonic: each particle is below the previous
        CHECK(strand.particles[1].position.y < strand.particles[0].position.y, "p1 below p0");
        CHECK(strand.particles[2].position.y < strand.particles[1].position.y, "p2 below p1");
        CHECK(strand.particles[3].position.y < strand.particles[2].position.y, "p3 below p2");
    }

    // 7. Distance constraint maintains rest length
    std::printf("[hair] test distance constraint\n");
    {
        std::string err;
        auto hp = create_hair_physics(HairConfig{}, err);
        HairParticle a, b;
        a.position = {0,0,0}; a.invMass = 1;
        b.position = {3,0,0}; b.invMass = 1;
        hp->applyDistanceConstraint(a, b, 1.0f, 1.0f);
        float dist = std::sqrt(
            (b.position.x-a.position.x)*(b.position.x-a.position.x) +
            (b.position.y-a.position.y)*(b.position.y-a.position.y) +
            (b.position.z-a.position.z)*(b.position.z-a.position.z));
        CHECK_NEAR(dist, 1.0f, 0.01f, "constraint pulls to rest length");
    }

    // 8. Pinned particle doesn't move under constraint
    std::printf("[hair] test pinned constraint\n");
    {
        std::string err;
        auto hp = create_hair_physics(HairConfig{}, err);
        HairParticle a, b;
        a.position = {0,0,0}; a.invMass = 0; // pinned
        b.position = {5,0,0}; b.invMass = 1;
        hp->applyDistanceConstraint(a, b, 1.0f, 1.0f);
        CHECK_NEAR(a.position.x, 0.0f, 0.001f, "pinned a doesn't move");
        float dist = std::sqrt(
            (b.position.x-a.position.x)*(b.position.x-a.position.x) +
            (b.position.y-a.position.y)*(b.position.y-a.position.y) +
            (b.position.z-a.position.z)*(b.position.z-a.position.z));
        CHECK(dist < 5.0f, "b moves toward a");
    }

    // 9. Stiffness = 0 means no correction
    std::printf("[hair] test zero stiffness\n");
    {
        std::string err;
        auto hp = create_hair_physics(HairConfig{}, err);
        HairParticle a, b;
        a.position = {0,0,0}; a.invMass = 1;
        b.position = {3,0,0}; b.invMass = 1;
        float bx = b.position.x;
        hp->applyDistanceConstraint(a, b, 1.0f, 0.0f);
        CHECK_NEAR(b.position.x, bx, 0.001f, "zero stiffness = no move");
    }

    // 10. Stiffness = 1 is maximum correction
    std::printf("[hair] test full stiffness\n");
    {
        std::string err;
        auto hp = create_hair_physics(HairConfig{}, err);
        HairParticle a, b;
        a.position = {0,0,0}; a.invMass = 1;
        b.position = {3,0,0}; b.invMass = 1;
        hp->applyDistanceConstraint(a, b, 1.0f, 1.0f);
        // With equal masses, each moves half the correction
        // Equal masses: each moves half the correction = 1.0 each direction
        CHECK_NEAR(a.position.x, 1.0f, 0.01f, "full stiffness a");
        CHECK_NEAR(b.position.x, 2.0f, 0.01f, "full stiffness b");
    }

    // 11. Convergence: long strand settles under gravity
    std::printf("[hair] test convergence\n");
    {
        std::string err;
        HairConfig cfg; cfg.gravity = -10; cfg.damping = 0.9f; cfg.stiffness = 0.9f;
        cfg.localIterations = 5; cfg.lengthIterations = 5;
        auto hp = create_hair_physics(cfg, err);
        std::vector<Vec3> positions;
        for (int i = 0; i < 10; i++) positions.push_back({0, 0, 0});
        auto strand = hp->createStrand(positions);
        // Simulate many steps
        for (int i = 0; i < 100; i++) hp->simulate(strand, cfg);
        // All particles should have y < 0 (hanging down)
        bool allBelow = true;
        for (size_t i = 1; i < strand.particles.size(); i++) {
            if (strand.particles[i].position.y >= 0) allBelow = false;
        }
        CHECK(allBelow, "strand hangs below root");
        // Root stays pinned
        CHECK_NEAR(strand.particles[0].position.y, 0.0f, 0.001f, "root stays");
    }

    // 12. Determinism
    std::printf("[hair] test determinism\n");
    {
        std::string err;
        HairConfig cfg; cfg.gravity = -10; cfg.damping = 0.9f;
        auto hp = create_hair_physics(cfg, err);
        std::vector<Vec3> positions = {{0,0,0}, {0,-1,0}, {0,-2,0}};
        auto s1 = hp->createStrand(positions);
        auto s2 = hp->createStrand(positions);
        hp->simulate(s1, cfg);
        hp->simulate(s2, cfg);
        CHECK(s1.particles[1].position.x == s2.particles[1].position.x, "deterministic x");
        CHECK(s1.particles[1].position.y == s2.particles[1].position.y, "deterministic y");
        CHECK(s1.particles[1].position.z == s2.particles[1].position.z, "deterministic z");
    }

    // 13. Multi-step determinism
    std::printf("[hair] test multi-step determinism\n");
    {
        std::string err;
        HairConfig cfg; cfg.gravity = -10; cfg.damping = 0.85f;
        auto hp = create_hair_physics(cfg, err);
        std::vector<Vec3> positions = {{0,0,0}, {0,-1,0}};
        auto s1 = hp->createStrand(positions);
        auto s2 = hp->createStrand(positions);
        for (int i = 0; i < 20; i++) { hp->simulate(s1, cfg); hp->simulate(s2, cfg); }
        CHECK(s1.particles[1].position.x == s2.particles[1].position.x, "multi-step deterministic x");
        CHECK(s1.particles[1].position.y == s2.particles[1].position.y, "multi-step deterministic y");
    }

    // 14. Refusals (invalid config)
    std::printf("[hair] test refusals\n");
    {
        HairConfig bad; bad.damping = -1;
        std::string err;
        auto p = create_hair_physics(bad, err);
        CHECK(p == nullptr, "bad config refused");
    }

    // 15. Config getter
    std::printf("[hair] test config getter\n");
    {
        std::string err;
        HairConfig cfg; cfg.gravity = -15; cfg.stiffness = 0.5f;
        auto hp = create_hair_physics(cfg, err);
        HairConfig got = hp->getConfig();
        CHECK_NEAR(got.gravity, -15.0f, 0.001f, "config gravity");
        CHECK_NEAR(got.stiffness, 0.5f, 0.001f, "config stiffness");
    }

    // 16. Wind affects trajectory
    std::printf("[hair] test wind\n");
    {
        std::string err;
        HairConfig cfg; cfg.gravity = 0; cfg.damping = 1.0f; cfg.windStrength = 5.0f;
        cfg.windDirection = {1,0,0};
        auto hp = create_hair_physics(cfg, err);
        std::vector<Vec3> positions = {{0,0,0}, {0,-1,0}};
        auto strand = hp->createStrand(positions);
        hp->simulate(strand, cfg);
        CHECK(strand.particles[1].position.x > 0, "wind pushes in X");
    }

    std::printf("\n[hair] Results: %d passed, %d failed\n", g_passed, g_failed);
    if (g_failed == 0) std::printf("[hair] ALL PASSED\n");
    return g_failed == 0 ? 0 : 1;
}
