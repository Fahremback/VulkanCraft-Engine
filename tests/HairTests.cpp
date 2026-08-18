// HairTests (FALTANTES §18 item 10): proves the public hair/fur contract
// `IHairProvider` — the StrandSolver plugin (position-based strand dynamics,
// the XPBD family, deterministic + headless) with LOD by relevance (frozen
// strands are not stepped), and the TressFX plugin seam (an opt-in GPU plugin
// NOT vendored — REFUSED with a diagnostic, never a silent fallback, the
// deformable/FEMFX pattern).
#include "engine/hair/IHairProvider.hpp"

#include <glm/glm.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace Engine::Hair;

namespace {

int g_failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::printf("[hair] FAIL: %s\n", message);
        ++g_failures;
    }
}

void checkClose(float a, float b, float eps, const char* message) {
    if (std::fabs(a - b) > eps) {
        std::printf("[hair] FAIL: %s (%.6f vs %.6f)\n", message, a, b);
        ++g_failures;
    }
}

// A horizontal strand: root at (0, y, 0), `count` points along +X spaced
// `spacing` apart (for the gravity-sag test).
HairStrandDesc horizontal_strand(float y, int count, float spacing) {
    HairStrandDesc desc;
    std::vector<glm::vec3> strand;
    for (int i = 0; i < count; ++i) {
        strand.push_back(glm::vec3(static_cast<float>(i) * spacing, y, 0.0f));
    }
    desc.strands.push_back(strand);
    return desc;
}

// A vertical strand hanging down from (0, top, 0).
HairStrandDesc vertical_strand(float top, int count, float spacing) {
    HairStrandDesc desc;
    std::vector<glm::vec3> strand;
    for (int i = 0; i < count; ++i) {
        strand.push_back(glm::vec3(0.0f, top - static_cast<float>(i) * spacing, 0.0f));
    }
    desc.strands.push_back(strand);
    return desc;
}

void test_validation_and_seam() {
    std::string err;
    // Plugin seam: StrandSolver works, Tressfx is REFUSED with a diagnostic.
    auto solver = create_hair_provider(HairProviderKind::StrandSolver, HairConfig{}, err);
    check(solver != nullptr, "seam: StrandSolver provider created");
    check(solver->kind() == HairProviderKind::StrandSolver, "seam: kind");
    auto tressfx =
        create_hair_provider(HairProviderKind::Tressfx, HairConfig{}, err);
    check(tressfx == nullptr, "seam: Tressfx refused (plugin not vendored)");
    check(!err.empty(), "seam: refusal carries a diagnostic");

    // Invalid config refuses the provider.
    HairConfig bad;
    bad.stiffness = 0.0f;
    check(create_hair_provider(HairProviderKind::StrandSolver, bad, err) == nullptr,
          "validation: zero stiffness refuses the provider");
    bad = HairConfig();
    bad.strandMaxNodes = 1;
    check(create_hair_provider(HairProviderKind::StrandSolver, bad, err) == nullptr,
          "validation: strandMaxNodes < 2 refuses");

    // Body creation refusals.
    solver = create_hair_provider(HairProviderKind::StrandSolver, HairConfig{}, err);
    HairStrandDesc empty;
    check(solver->create_strand_body(empty, err) == InvalidHairBody,
          "validation: no strands refused");
    HairStrandDesc onePoint;
    onePoint.strands.push_back({glm::vec3(0)});
    check(solver->create_strand_body(onePoint, err) == InvalidHairBody,
          "validation: strand with a single point refused");
    HairStrandDesc degenerate;
    degenerate.strands.push_back({glm::vec3(0), glm::vec3(0)});
    check(solver->create_strand_body(degenerate, err) == InvalidHairBody,
          "validation: degenerate segment refused");
    HairConfig tiny;
    tiny.maxStrands = 2;
    auto small = create_hair_provider(HairProviderKind::StrandSolver, tiny, err);
    HairStrandDesc three;
    three.strands.push_back({glm::vec3(0), glm::vec3(0, 0, 1)});
    three.strands.push_back({glm::vec3(0), glm::vec3(0, 0, 1)});
    three.strands.push_back({glm::vec3(0), glm::vec3(0, 0, 1)});
    check(small->create_strand_body(three, err) == InvalidHairBody,
          "validation: strand count over the cap refused");
    HairConfig nodeCap;
    nodeCap.strandMaxNodes = 3;
    auto capped = create_hair_provider(HairProviderKind::StrandSolver, nodeCap, err);
    check(capped->create_strand_body(horizontal_strand(1.0f, 5, 0.25f), err) ==
              InvalidHairBody,
          "validation: strand over the node cap refused");

    // JSON config.
    HairConfig jsonCfg;
    check(jsonCfg.load_from_json(
              "{\"stiffness\": 0.9, \"damping\": 0.2, \"bounce\": 0.5, "
              "\"groundY\": -2.0, \"maxStrands\": 64, \"substeps\": 4, "
              "\"solverIterations\": 16, \"gravityY\": -5.0}",
              err),
          "validation: JSON config loads");
    checkClose(jsonCfg.stiffness, 0.9f, 1e-6f, "validation: stiffness read");
    checkClose(jsonCfg.gravity.y, -5.0f, 1e-6f, "validation: gravityY read");
    check(jsonCfg.groundCollision, "validation: groundCollision default true");
    check(!jsonCfg.load_from_json("{\"stiffness\": 0}", err),
          "validation: JSON zero stiffness refused");
    check(!jsonCfg.load_from_json("not json", err),
          "validation: JSON parse failure refused");
    std::printf("[hair] validation + plugin seam (Tressfx refused) OK\n");
}

void test_gravity_sag() {
    // A horizontal strand droops under gravity. The droop that distinguishes
    // hair comes from TWO constraints: the FOLLICLE pin (pinRootDirection —
    // the first segment keeps its rest direction; without it a straight chain
    // swings as a rigid pendulum with zero curvature and the droop can't tell
    // floppy from stiff) and the BENDING stiffness (resistance to curvature
    // along the strand). A floppy strand (bend 0) droops far below its rest
    // height 1.0; a stiff one (bend 0.95) keeps its rest shape and the tip
    // stays high. Ground is far below (groundY -5, collision off) so the
    // equilibrium is pure droop.
    HairConfig softCfg;
    softCfg.stiffness = 0.99f;
    softCfg.bendStiffness = 0.0f;   // perfectly flexible rope
    softCfg.pinRootDirection = true;
    softCfg.solverIterations = 24;
    softCfg.damping = 0.3f;
    softCfg.groundCollision = false;
    softCfg.groundY = -5.0f;
    HairConfig hardCfg = softCfg;
    hardCfg.bendStiffness = 0.95f;  // hair-like resistance to curvature
    std::string err;
    auto soft = create_hair_provider(HairProviderKind::StrandSolver, softCfg, err);
    auto hard = create_hair_provider(HairProviderKind::StrandSolver, hardCfg, err);
    const HairStrandDesc desc = horizontal_strand(1.0f, 6, 0.2f);  // root y=1, 5 segments
    const auto bodyS = soft->create_strand_body(desc, err);
    const auto bodyH = hard->create_strand_body(desc, err);
    check(bodyS != InvalidHairBody && bodyH != InvalidHairBody, "sag: bodies created");
    check(soft->node_count(bodyS, 0) == 6, "sag: six nodes per strand");

    for (int i = 0; i < 600; ++i) {
        soft->step(1.0f / 60.0f);
        hard->step(1.0f / 60.0f);
    }
    // The tip (node 5) droops below its rest height 1.0 in BOTH cases; the
    // stiffer strand droops LESS (probe: 0.160 vs 0.996 at equilibrium).
    const float softTip = soft->node_position(bodyS, 0, 5).y;
    const float hardTip = hard->node_position(bodyH, 0, 5).y;
    check(softTip < 0.5f, "sag: floppy strand droops low");
    check(hardTip > 0.9f, "sag: stiff strand keeps its shape");
    check(hardTip > softTip, "sag: stiffer strand droops LESS than the floppy");
    check(soft->constraint_error(bodyS) < 0.05f,
          "sag: floppy strand settled (segments hold)");
    check(hard->constraint_error(bodyH) < 0.05f,
          "sag: stiff strand settled (segments hold)");

    // The follicle pin keeps the FIRST segment on the rest direction: node 1
    // sits on the +X ray from the root at distance ~spacing, and its droop is
    // essentially zero (probe: 0.9999).
    const glm::vec3 node1 = soft->node_position(bodyS, 0, 1);
    check(node1.y > 0.995f, "sag: follicle pin keeps the root segment level");
    checkClose(glm::length(node1 - soft->node_position(bodyS, 0, 0)), 0.2f,
               1e-2f, "sag: first segment keeps its rest length");
    std::printf("[hair] gravity droop: floppy tip %.3f, stiff tip %.3f\n",
                softTip, hardTip);

    // The root never moves (fixed).
    const glm::vec3 root = soft->node_position(bodyS, 0, 0);
    checkClose(root.y, 1.0f, 1e-6f, "sag: root stays fixed");
}

void test_collision() {
    // A hanging strand drops onto the ground: no node ever goes below
    // groundY, and the tip ends resting on it.
    HairConfig cfg;
    cfg.groundCollision = true;
    cfg.groundY = 0.0f;
    cfg.bounce = 0.3f;
    cfg.damping = 0.3f;
    std::string err;
    auto provider = create_hair_provider(HairProviderKind::StrandSolver, cfg, err);
    const HairStrandDesc desc = vertical_strand(3.0f, 7, 0.5f);
    const auto body = provider->create_strand_body(desc, err);
    check(body != InvalidHairBody, "collision: body created");

    bool everBelow = false;
    for (int i = 0; i < 360; ++i) {
        provider->step(1.0f / 60.0f);
        for (std::uint32_t n = 0; n < provider->node_count(body, 0); ++n) {
            if (provider->node_position(body, 0, n).y < cfg.groundY - 1e-5f) {
                everBelow = true;
            }
        }
    }
    check(!everBelow, "collision: no node ever below the ground");
    const float tip = provider->node_position(body, 0, 6).y;
    check(tip >= cfg.groundY - 1e-4f, "collision: tip rests on the ground");
    check(tip < cfg.groundY + 0.3f, "collision: tip near the ground (not floating)");
    std::printf("[hair] ground collision: tip y=%.3f\n", tip);
}

void test_lod() {
    // 8 strands; relevance reduces the ACTIVE count and freezes the rest.
    HairConfig cfg;
    std::string err;
    auto provider = create_hair_provider(HairProviderKind::StrandSolver, cfg, err);
    HairStrandDesc desc;
    for (int s = 0; s < 8; ++s) {
        std::vector<glm::vec3> strand = {
            glm::vec3(static_cast<float>(s) * 0.1f, 1.0f, 0.0f),
            glm::vec3(static_cast<float>(s) * 0.1f, 1.0f, 0.2f),
            glm::vec3(static_cast<float>(s) * 0.1f, 1.0f, 0.4f),
        };
        desc.strands.push_back(strand);
    }
    const auto body = provider->create_strand_body(desc, err);
    check(body != InvalidHairBody, "lod: body created");
    check(provider->strand_count(body) == 8, "lod: 8 strands");
    check(provider->active_strand_count(body) == 8, "lod: all active at default");

    check(provider->set_lod(body, 1.0f, err), "lod: relevance 1 accepted");
    check(provider->active_strand_count(body) == 8, "lod: all active at 1.0");
    check(provider->set_lod(body, 0.5f, err), "lod: relevance 0.5 accepted");
    check(provider->active_strand_count(body) == 4, "lod: half active at 0.5");
    check(provider->set_lod(body, 0.25f, err), "lod: relevance 0.25 accepted");
    check(provider->active_strand_count(body) == 2, "lod: quarter active at 0.25");
    check(provider->set_lod(body, 0.0f, err), "lod: relevance 0 accepted");
    check(provider->active_strand_count(body) == 1, "lod: at least one active at 0");
    check(!provider->set_lod(body, 1.5f, err), "lod: relevance > 1 refused");
    check(!provider->set_lod(body, -0.1f, err), "lod: negative relevance refused");

    // A frozen strand is NOT stepped: its nodes stay bit-identical.
    check(provider->set_lod(body, 0.5f, err), "lod: back to half active");
    const glm::vec3 frozenBefore = provider->node_position(body, 7, 1);
    for (int i = 0; i < 60; ++i) provider->step(1.0f / 60.0f);
    const glm::vec3 frozenAfter = provider->node_position(body, 7, 1);
    check(frozenBefore == frozenAfter, "lod: frozen strand never moves");
    std::printf("[hair] LOD active counts 8/4/2/1, frozen strand static OK\n");
}

void test_determinism() {
    HairConfig cfg;
    cfg.substeps = 3;
    cfg.solverIterations = 16;
    cfg.stiffness = 0.6f;
    cfg.damping = 0.15f;
    std::string err;
    auto a = create_hair_provider(HairProviderKind::StrandSolver, cfg, err);
    auto b = create_hair_provider(HairProviderKind::StrandSolver, cfg, err);
    HairStrandDesc desc;
    for (int s = 0; s < 4; ++s) {
        std::vector<glm::vec3> strand;
        for (int i = 0; i < 6; ++i) {
            strand.push_back(glm::vec3(static_cast<float>(s) * 0.1f +
                                           static_cast<float>(i) * 0.15f,
                                       1.0f, 0.0f));
        }
        desc.strands.push_back(strand);
    }
    const auto bodyA = a->create_strand_body(desc, err);
    const auto bodyB = b->create_strand_body(desc, err);
    for (int i = 0; i < 120; ++i) {
        a->apply_force(bodyA, 0, 5, glm::vec3(0.0f, -0.5f, 0.2f));
        b->apply_force(bodyB, 0, 5, glm::vec3(0.0f, -0.5f, 0.2f));
        a->step(1.0f / 60.0f);
        b->step(1.0f / 60.0f);
    }
    bool identical = true;
    for (std::uint32_t s = 0; s < 4 && identical; ++s) {
        for (std::uint32_t n = 0; n < 6; ++n) {
            if (a->node_position(bodyA, s, n) != b->node_position(bodyB, s, n) ||
                a->node_velocity(bodyA, s, n) != b->node_velocity(bodyB, s, n)) {
                identical = false;
                break;
            }
        }
    }
    check(identical, "determinism: two providers, identical steps -> bit-identical");
    check(a->constraint_error(bodyA) == b->constraint_error(bodyB),
          "determinism: constraint error identical");
    std::printf("[hair] cross-instance determinism OK\n");
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    test_validation_and_seam();
    test_gravity_sag();
    test_collision();
    test_lod();
    test_determinism();
    if (g_failures == 0) {
        std::printf("[hair] ALL PASSED\n");
        return 0;
    }
    std::printf("[hair] %d FAILURE(S)\n", g_failures);
    return 1;
}
