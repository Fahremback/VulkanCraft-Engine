// OriginRebaseTests.cpp
//
// Evidence for FALTANTES §15 "origin rebasing ou coordenadas de alta
// precisão" (META §19):
//   - the core problem: a float32 position at |x| ~ 1e9 cannot even
//     distinguish adjacent blocks (ulp = 64 > 1), so raw float positions lose
//     sub-block resolution exactly there;
//   - precision: rebasing the origin to the far focus makes absolute
//     positions authorable at full double precision — to_absolute_d(to_local)
//     round-trips bit-exactly for content near the origin;
//   - invariance: a rebase translates every entity in every world by -delta,
//     preserving absolute positions (and containing blocks) bit-exactly for
//     content near the new origin — including successive rebases that follow
//     a moving focus;
//   - integer snap: update(focus, threshold, snapToVoxel) snaps the origin to
//     the voxel grid so floor(absolute) is integer-exact;
//   - threshold: a focus still in range is a no-op (nothing translated);
//   - validation: unknown world / dead handle / negative threshold /
//     non-finite delta refused all-or-nothing with diagnostics;
//   - determinism: identical managers rebased identically produce bit-identical
//     positions.

#include <engine/world/IOriginRebase.hpp>

#include <glm/glm.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace engine::world;
using engine::entity::EntityId;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what);
        ++g_failures;
    }
}

bool dvec_eq(const glm::dvec3& a, const glm::dvec3& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

// Real float32 rounding. The project builds with /fp:fast, under which the
// compiler keeps float conversions in registers (double) and a direct float
// comparison would be performed in double; `volatile` forces a memory store
// with true float32 rounding, matching the stored Position semantics.
float f32(double v) {
    volatile float f = static_cast<float>(v);
    return f;
}

glm::ivec3 block_of(const glm::dvec3& absolute) {
    return glm::ivec3(static_cast<long long>(std::floor(absolute.x)),
                      static_cast<long long>(std::floor(absolute.y)),
                      static_cast<long long>(std::floor(absolute.z)));
}

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

struct Harness {
    std::unique_ptr<IWorldManager> manager = create_world_manager();
    std::unique_ptr<IOriginRebase> rebase =
        create_origin_rebase(*manager);
};

bool boot_world(IWorldManager& manager, const std::string& name,
                const glm::vec3& player, int budget) {
    engine::voxel::IVoxelWorld* world = manager.world(name);
    if (world == nullptr) return false;
    world->register_generator(std::make_shared<FlatGenerator>(96));
    world->set_chunk_budget(budget);
    const auto start = std::chrono::steady_clock::now();
    while (!world->is_chunk_loaded(0, 0)) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count() > 8000) {
            return false;
        }
        manager.update_world(name, player, 1.0f / 60.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

// 1. The core point: raw float32 cannot resolve blocks at 1e9; rebasing the
//    origin there makes absolute positions authorable at full precision.
void test_precision_at_distance() {
    Harness harness;
    std::string error;

    // Premise: float32 at 1e9 has ulp 64 — adjacent blocks are identical.
    check(f32(1000000001.0) == f32(1000000000.0),
          "premise: raw float32 cannot distinguish blocks at 1e9");

    // With the origin at 0, a far absolute position degrades immediately:
    // the sub-block offsets are lost in the stored float local (the round
    // trip no longer reproduces the input).
    const glm::dvec3 far(1000000000.25, 1000000073.5, 1000001005.75);
    const glm::vec3 degraded = harness.rebase->to_local(far);
    check(!dvec_eq(harness.rebase->to_absolute_d(degraded), far),
          "without rebase the far offset is lost in float local");
    check(degraded.x == f32(1000000000.0),
          "the 0.25 sub-block offset is rounded away");

    // Rebase the origin to the integer-snapped far focus FIRST (the intended
    // usage: the origin follows the content), then author at full precision —
    // the stored local is exactly the sub-block offset raw float32 loses.
    const glm::dvec3 snappedOrigin(std::floor(far.x), std::floor(far.y),
                                   std::floor(far.z));
    RebaseResult r = harness.rebase->rebase(snappedOrigin, error);
    check(r.rebased && error.empty(), "rebase to far focus succeeds");
    check(dvec_eq(harness.rebase->origin(), snappedOrigin),
          "origin is the integer-snapped far focus");
    check(r.translatedEntities == 0, "no entities translated (empty worlds)");

    WorldSpec spec;
    spec.name = "far";
    spec.seed = 1;
    check(harness.manager->create_world(spec, error), "world created");
    check(boot_world(*harness.manager, "far", glm::vec3(0.0f, 0.0f, 0.0f), 2),
          "world boots");

    const EntityId mob =
        harness.rebase->spawn_at("far", "test.mob", far, error);
    check(mob.valid() && error.empty(), "spawn at absolute position");

    glm::dvec3 back;
    check(harness.rebase->absolute_position("far", mob, back, error),
          "absolute position readable");
    check(dvec_eq(back, far),
          "absolute position round-trips BIT-EXACT after rebase");
    // The stored local is the FRACTIONAL part (0.25, 0.5, 0.75): the integer
    // part (1e9 + 73 + 1005) lives in the double origin, and the fractional
    // part is exactly the resolution raw float32 at 1e9 cannot represent.
    engine::entity::Position stored;
    check(harness.manager->world("far")->entity_world()->get_position(mob,
                                                                      stored),
          "stored local readable");
    check(stored.x == 0.25f && stored.y == 0.5f && stored.z == 0.75f,
          "stored local carries the exact fractional offsets");

    // The containing block is integer-exact: floor(origin + local) equals
    // floor(absolute) computed directly.
    const glm::ivec3 blockFromFrame = block_of(back);
    check(blockFromFrame == block_of(far),
          "containing block matches floor(absolute)");

    std::printf("[origin-rebase] precision at 1e9: raw float32 loses blocks, "
                "rebased frame round-trips bit-exact OK\n");
}

// 2. Invariance: a rebase translates every entity in every world by -delta,
//    preserving absolute positions, health and containing blocks bit-exactly
//    for content near the new origin — and rebasing back restores the originals.
void test_invariance_under_rebase() {
    Harness harness;
    IWorldManager& manager = *harness.manager;
    std::string error;
    WorldSpec a, b;
    a.name = "A";
    a.seed = 1;
    b.name = "B";
    b.seed = 2;
    check(manager.create_world(a, error) && manager.create_world(b, error),
          "two worlds created");
    check(boot_world(manager, "A", glm::vec3(8.0f, 200.0f, 8.0f), 2) &&
              boot_world(manager, "B", glm::vec3(8.0f, 200.0f, 8.0f), 2),
          "both boot");

    const EntityId e1 = harness.rebase->spawn_at("A", "test.mob",
                                                 glm::dvec3(8.0, 130.0, 8.0),
                                                 error);
    const EntityId e2 = harness.rebase->spawn_at("A", "test.mob",
                                                 glm::dvec3(10.0, 140.0, 10.0),
                                                 error);
    const EntityId e3 = harness.rebase->spawn_at("B", "test.mob",
                                                 glm::dvec3(1000.0, 73.0, 1005.0),
                                                 error);
    check(e1.valid() && e2.valid() && e3.valid(), "entities spawned");
    manager.world("A")->entity_world()->set_health(e1, { 17.5f, 20.0f });

    // Capture absolute positions + blocks before the rebase.
    glm::dvec3 p1, p2, p3;
    check(harness.rebase->absolute_position("A", e1, p1, error) &&
              harness.rebase->absolute_position("A", e2, p2, error) &&
              harness.rebase->absolute_position("B", e3, p3, error),
          "pre-rebase absolute positions");
    const glm::ivec3 b1 = block_of(p1);
    const glm::ivec3 b3 = block_of(p3);

    // Rebase to the focus (near e1). Delta is small => every local stays
    // float-exact => absolute positions are preserved bit-exactly.
    RebaseResult r = harness.rebase->rebase(glm::dvec3(8.0, 130.0, 8.0), error);
    check(r.rebased && error.empty(), "rebase to focus succeeds");
    check(r.translatedEntities == 3, "all 3 entities translated");

    glm::dvec3 q1, q2, q3;
    check(harness.rebase->absolute_position("A", e1, q1, error) &&
              harness.rebase->absolute_position("A", e2, q2, error) &&
              harness.rebase->absolute_position("B", e3, q3, error),
          "post-rebase absolute positions");
    check(dvec_eq(q1, p1) && dvec_eq(q2, p2) && dvec_eq(q3, p3),
          "absolute positions INVARIANT under rebase (bit-exact)");
    check(block_of(q1) == b1 && block_of(q3) == b3,
          "containing blocks invariant");
    engine::entity::Health h;
    check(manager.world("A")->entity_world()->get_health(e1, h) &&
              h.value == 17.5f,
          "health untouched by the rebase");

    // Rebase back to the origin: the original locals are restored exactly.
    RebaseResult back = harness.rebase->rebase(glm::dvec3(0.0, 0.0, 0.0), error);
    check(back.rebased && back.translatedEntities == 3,
          "rebase back translates all entities");
    glm::dvec3 s1;
    check(harness.rebase->absolute_position("A", e1, s1, error) &&
              dvec_eq(s1, p1),
          "rebasing back restores the exact original positions");

    std::printf("[origin-rebase] invariance: rebase shifts every entity, "
                "absolute positions/health/blocks preserved bit-exact OK\n");
}

// 3. Focus-following loop: successive rebases keep precision as the focus
//    moves (content authored near an earlier origin stays exact).
void test_following_focus_loop() {
    Harness harness;
    std::string error;
    WorldSpec spec;
    spec.name = "W";
    spec.seed = 3;
    check(harness.manager->create_world(spec, error), "world created");
    check(boot_world(*harness.manager, "W", glm::vec3(0.0f, 0.0f, 0.0f), 2),
          "world boots");

    // First rebase to a far focus, then author content there.
    const glm::dvec3 focus1(1000000000.0, 1000000000.0, 1000000000.0);
    check(harness.rebase->rebase(focus1, error).rebased,
          "rebase to far focus");
    const glm::dvec3 content(focus1.x + 8.0, focus1.y + 130.0, focus1.z + 8.0);
    const EntityId mob = harness.rebase->spawn_at("W", "test.mob", content, error);
    check(mob.valid(), "content spawned at the far focus");

    // The focus moves 10 units; the loop rebases again. Content near the new
    // origin stays bit-exact.
    const glm::dvec3 focus2 = focus1 + glm::dvec3(10.0, 0.0, 10.0);
    RebaseResult r = harness.rebase->update(focus2, 4.0, true, error);
    check(r.rebased && error.empty(), "focus drift triggers a rebase");
    check(dvec_eq(harness.rebase->origin(), glm::dvec3(
              std::floor(focus2.x), std::floor(focus2.y), std::floor(focus2.z))),
          "origin snapped to the integer voxel grid");

    glm::dvec3 back;
    check(harness.rebase->absolute_position("W", mob, back, error),
          "content position readable after the second rebase");
    check(dvec_eq(back, content),
          "content absolute position preserved across successive rebases");
    check(block_of(back) == block_of(content),
          "content block exact with integer-snapped origin");

    std::printf("[origin-rebase] focus-following loop: successive rebases keep "
                "content bit-exact with integer snap OK\n");
}

// 4. Threshold semantics: a focus still in range is a no-op; a negative
//    threshold is refused.
void test_threshold() {
    Harness harness;
    std::string error;
    WorldSpec spec;
    spec.name = "W";
    spec.seed = 4;
    check(harness.manager->create_world(spec, error), "world created");
    check(boot_world(*harness.manager, "W", glm::vec3(0.0f, 0.0f, 0.0f), 2),
          "world boots");
    const EntityId mob = harness.rebase->spawn_at("W", "test.mob",
                                                  glm::dvec3(8.0, 130.0, 8.0),
                                                  error);
    check(mob.valid(), "entity spawned");

    RebaseResult noop = harness.rebase->update(
        glm::dvec3(2.0, 0.0, 0.0), 10.0, true, error);
    check(!noop.rebased && noop.translatedEntities == 0,
          "focus within threshold is a no-op");
    check(dvec_eq(harness.rebase->origin(), glm::dvec3(0.0, 0.0, 0.0)),
          "origin unchanged by the no-op");

    std::string negError;
    RebaseResult neg = harness.rebase->update(
        glm::dvec3(100.0, 100.0, 100.0), -1.0, true, negError);
    check(!neg.rebased && !negError.empty(),
          "negative threshold refused with a diagnostic");

    std::printf("[origin-rebase] threshold: in-range no-op, negative refused "
                "OK\n");
}

// 5. Lossless conversions near the origin (the frame the service maintains).
void test_roundtrip_near_origin() {
    Harness harness;
    std::string error;
    WorldSpec spec;
    spec.name = "W";
    spec.seed = 5;
    check(harness.manager->create_world(spec, error), "world created");
    check(harness.rebase->rebase(glm::dvec3(2048.5, 4096.25, 8192.75), error)
              .rebased,
          "origin set");

    bool allExact = true;
    for (int i = 0; i < 8; ++i) {
        const glm::dvec3 a(2048.5 + i * 0.25, 4096.25 + i * 0.5,
                           8192.75 - i * 0.125);
        const glm::dvec3 roundTrip = harness.rebase->to_absolute_d(
            harness.rebase->to_local(a));
        if (!dvec_eq(roundTrip, a)) allExact = false;
    }
    check(allExact, "to_absolute_d(to_local(a)) == a bit-exact near origin");

    std::printf("[origin-rebase] lossless conversions near the origin OK\n");
}

// 6. Validation: unknown world / dead handle / non-finite delta refused.
void test_validation() {
    Harness harness;
    std::string error;
    WorldSpec spec;
    spec.name = "W";
    spec.seed = 6;
    check(harness.manager->create_world(spec, error), "world created");

    std::string e1;
    check(!harness.rebase->spawn_at("missing", "test.mob",
                                    glm::dvec3(1.0, 2.0, 3.0), e1).valid() &&
              !e1.empty(),
          "spawn_at on an unknown world refused");

    std::string e2;
    glm::dvec3 out;
    check(!harness.rebase->absolute_position("W", EntityId{ 7, 0 }, out, e2) &&
              !e2.empty(),
          "absolute_position of a dead handle refused");

    std::string e3;
    RebaseResult bad = harness.rebase->rebase(
        glm::dvec3(std::nan(""), 0.0, 0.0), e3);
    check(!bad.rebased && !e3.empty(),
          "non-finite origin delta refused");

    std::printf("[origin-rebase] validation: unknown world / dead handle / "
                "non-finite delta refused OK\n");
}

// 7. Determinism: identical managers + identical rebase => bit-identical
//    translated positions.
void test_determinism() {
    auto run = [](const glm::dvec3& delta, glm::dvec3& out) {
        Harness harness;
        std::string error;
        WorldSpec spec;
        spec.name = "W";
        spec.seed = 7;
        check(harness.manager->create_world(spec, error), "world created");
        check(boot_world(*harness.manager, "W", glm::vec3(0.0f, 0.0f, 0.0f), 2),
              "world boots");
        const EntityId mob =
            harness.rebase->spawn_at("W", "test.mob",
                                     glm::dvec3(8.0, 130.0, 8.0), error);
        check(mob.valid(), "entity spawned");
        check(harness.rebase->rebase(delta, error).rebased, "rebase applied");
        harness.rebase->absolute_position("W", mob, out, error);
    };

    glm::dvec3 first, second;
    run(glm::dvec3(1000000000.0, 1000000000.0, 1000000000.0), first);
    run(glm::dvec3(1000000000.0, 1000000000.0, 1000000000.0), second);
    check(dvec_eq(first, second),
          "identical rebase across identical managers is bit-identical");

    std::printf("[origin-rebase] determinism: bit-identical across instances "
                "OK\n");
}

}  // namespace

int main() {
    test_precision_at_distance();
    test_invariance_under_rebase();
    test_following_focus_loop();
    test_threshold();
    test_roundtrip_near_origin();
    test_validation();
    test_determinism();
    if (g_failures == 0) {
        std::printf("[origin-rebase] ALL PASSED\n");
        return 0;
    }
    std::printf("[origin-rebase] %d FAILURE(S)\n", g_failures);
    return 1;
}
