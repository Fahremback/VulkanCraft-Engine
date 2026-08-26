// AgentCapabilitiesTests — gate do contrato IAgentCapabilities (§4 itens
// 25/28, CORE). Prova: walk em superfície plana, degrau dentro/fora do
// limite, gap por salto, escalada, natação com profundidade, voo,
// impossível com diagnóstico, round-trip de perfil.

#include "engine/navigation/IAgentCapabilities.hpp"

#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what);
        ++failures;
    }
}

bool is_cap(engine::navigation::TraversalResult r, engine::navigation::MoveCapability c) {
    return r.possible && r.capability == c;
}

void test_walk() {
    auto ac = engine::navigation::create_agent_capabilities();
    engine::navigation::AgentProfile profile;
    profile.radius = 0.4f;
    profile.height = 1.8f;
    profile.maxClimb = 1.0f;
    profile.maxSlopeDegrees = 45.0f;
    profile.canJump = true;
    profile.canClimb = false;
    profile.canSwim = false;
    profile.canFly = false;
    ac->set_profile(profile);

    engine::navigation::TraversalGeometry flat;
    flat.slopeDegrees = 0.0f;
    engine::navigation::TraversalResult r = ac->can_traverse(flat);
    check(is_cap(r, engine::navigation::MoveCapability::Walk), "plano → Walk");

    engine::navigation::TraversalGeometry step;
    step.stepUp = 0.8f;  // <= maxClimb
    r = ac->can_traverse(step);
    check(is_cap(r, engine::navigation::MoveCapability::Walk), "degrau 0.8 → Walk");

    engine::navigation::TraversalGeometry steep;
    steep.slopeDegrees = 60.0f;  // > 45
    r = ac->can_traverse(steep);
    check(is_cap(r, engine::navigation::MoveCapability::Jump) == false &&
              r.possible == false,
          "inclinação 60° sem escalada → impossível");
    check(r.reason != nullptr && r.reason[0] != '\0', "diagnóstico não vazio");

    engine::navigation::TraversalGeometry ceiling;
    ceiling.ceilingClearance = 1.2f;  // < 1.8
    r = ac->can_traverse(ceiling);
    check(!r.possible, "teto baixo → impossível");
}

void test_jump_and_climb() {
    auto ac = engine::navigation::create_agent_capabilities();
    engine::navigation::AgentProfile profile;
    profile.maxClimb = 1.0f;
    profile.jumpDistance = 2.0f;
    profile.jumpHeight = 1.2f;
    profile.climbHeight = 3.0f;
    profile.canJump = true;
    profile.canClimb = true;
    profile.canSwim = true;
    profile.canFly = false;
    ac->set_profile(profile);

    engine::navigation::TraversalGeometry gap;
    gap.horizontalGap = 1.5f;  // <= jumpDistance
    engine::navigation::TraversalResult r = ac->can_traverse(gap);
    check(is_cap(r, engine::navigation::MoveCapability::Jump), "gap 1.5 → Jump");

    engine::navigation::TraversalGeometry bigGap;
    bigGap.horizontalGap = 3.0f;  // > jumpDistance
    r = ac->can_traverse(bigGap);
    check(!r.possible, "gap 3.0 além do salto → impossível");

    engine::navigation::TraversalGeometry wall;
    wall.stepUp = 1.8f;  // > jumpHeight, mas escalável
    r = ac->can_traverse(wall);
    check(is_cap(r, engine::navigation::MoveCapability::Climb), "parede escalável → Climb");

    engine::navigation::TraversalGeometry water;
    water.waterDepth = 1.5f;  // <= swimDepth
    r = ac->can_traverse(water);
    check(is_cap(r, engine::navigation::MoveCapability::Swim), "água 1.5 → Swim");

    engine::navigation::TraversalGeometry deep;
    deep.waterDepth = 5.0f;  // > swimDepth
    r = ac->can_traverse(deep);
    check(!r.possible, "água 5.0 além da natação → impossível");
}

void test_fly_and_profile() {
    auto ac = engine::navigation::create_agent_capabilities();
    engine::navigation::AgentProfile profile;
    profile.canFly = true;
    ac->set_profile(profile);

    engine::navigation::TraversalGeometry anything;
    anything.horizontalGap = 99.0f;
    anything.stepUp = 99.0f;
    anything.waterDepth = 99.0f;
    engine::navigation::TraversalResult r = ac->can_traverse(anything);
    check(is_cap(r, engine::navigation::MoveCapability::Fly), "voo ignora geometria → Fly");

    // Sem capacidades especiais: só walk.
    engine::navigation::AgentProfile base;
    base.canJump = false;
    base.canClimb = false;
    base.canSwim = false;
    base.canFly = false;
    ac->set_profile(base);
    engine::navigation::TraversalGeometry gap;
    gap.horizontalGap = 0.5f;
    r = ac->can_traverse(gap);
    check(!r.possible, "sem jump: gap pequeno → impossível");

    // Round-trip de perfil.
    engine::navigation::AgentProfile full;
    full.radius = 0.6f;
    full.height = 2.0f;
    full.maxClimb = 1.5f;
    full.maxSlopeDegrees = 50.0f;
    full.jumpDistance = 3.0f;
    full.jumpHeight = 1.5f;
    full.climbHeight = 4.0f;
    full.swimDepth = 3.0f;
    full.canJump = true;
    full.canClimb = true;
    full.canSwim = true;
    full.canFly = true;
    ac->set_profile(full);
    const engine::navigation::AgentProfile got = ac->profile();
    check(got.radius == 0.6f && got.height == 2.0f && got.maxClimb == 1.5f &&
              got.maxSlopeDegrees == 50.0f && got.jumpDistance == 3.0f &&
              got.jumpHeight == 1.5f && got.climbHeight == 4.0f &&
              got.swimDepth == 3.0f && got.canJump && got.canClimb &&
              got.canSwim && got.canFly,
          "perfil round-trip exato");
}

}  // namespace

int main() {
    test_walk();
    test_jump_and_climb();
    test_fly_and_profile();

    if (failures == 0) {
        std::printf("agent_capabilities_tests: all checks passed\n");
        return 0;
    }
    std::printf("agent_capabilities_tests: %d failure(s)\n", failures);
    return 1;
}
