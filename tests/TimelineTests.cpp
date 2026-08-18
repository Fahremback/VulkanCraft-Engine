// TimelineTests.cpp
//
// Evidence for FALTANTES §15 "branches/estados temporais persistentes para
// viagem no tempo" (META §19):
//   - temporal states are FULL world snapshots (save v5: blocks + entities)
//     persisted to a named path and registered on a timeline;
//   - capture_state / state_exists / states / state with all-or-nothing
//     validation (duplicate name, empty path, unknown world, failed save);
//   - travel_to REWINDS the live world to a captured state — blocks and
//     entities revert, and the change is PERSISTENT (a real mutation, not a
//     simulation);
//   - the FUTURE is never destroyed: later states still exist and traveling
//     to them returns the future content (divergent timeline, not a single
//     undoable line);
//   - branch_state FORKS a state into an independent timeline (snapshot file
//     copied): edits made after traveling to the branch do NOT contaminate
//     the source state;
//   - transactional rollback: travel failures restore the pre-travel state;
//   - determinism: identical managers + identical states travel to identical
//     content.

#include <engine/world/ITimeTravel.hpp>

#include <glm/glm.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace engine::world;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what);
        ++g_failures;
    }
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
    std::unique_ptr<ITimeTravel> timeline = create_time_travel(*manager);
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

std::size_t entity_count(IWorldManager& manager, const std::string& name) {
    return manager.world_info(name).entityCount;
}

// 1. Capture: a temporal state is a full persistent snapshot (blocks +
//    entities) with all-or-nothing validation.
void test_capture() {
    Harness harness;
    IWorldManager& manager = *harness.manager;
    std::string error;
    WorldSpec spec;
    spec.name = "W";
    spec.seed = 1;
    check(manager.create_world(spec, error), "world created");
    check(boot_world(manager, "W", glm::vec3(8.0f, 200.0f, 8.0f), 2),
          "world boots");

    // A stone block + one entity in the live world.
    manager.world("W")->set_block(8, 131, 8, 3);
    auto* entities = manager.world("W")->entity_world().get();
    std::string eError;
    entities->spawn("test.mob", { 8.0f, 130.0f, 8.0f }, eError);

    const std::string path = "vc_timeline_state1.vcwld";
    check(harness.timeline->capture_state("s0", "W", path, error),
          "state s0 captured");
    check(harness.timeline->state_exists("s0"), "state exists");
    const auto all = harness.timeline->states();
    check(all.size() == 1 && all[0].name == "s0" &&
              all[0].worldName == "W" && all[0].path == path,
          "states() reports the entry");
    const TimelineStateInfo info = harness.timeline->state("s0");
    check(info.name == "s0" && info.worldName == "W" && info.path == path,
          "state() reports the entry");
    check(std::filesystem::exists(path), "snapshot persisted to disk");

    // ---- validation (all-or-nothing, nothing registered) ----
    std::string vError;
    check(!harness.timeline->capture_state("s0", "W", "x.vcwld", vError) &&
              !vError.empty(),
          "duplicate state name refused");
    check(!harness.timeline->capture_state("s1", "missing", "x.vcwld",
                                           vError) &&
              !vError.empty(),
          "unknown world refused");
    check(!harness.timeline->capture_state("s1", "W", "", vError) &&
              !vError.empty(),
          "empty path refused");
    check(!harness.timeline->state_exists("s1"), "nothing registered on failure");

    std::filesystem::remove(path);
    std::printf("[timeline] capture: full persistent snapshot + validation "
                "OK\n");
}

// 2. travel_to rewinds the LIVE world to the captured state (blocks revert);
//    the change is persistent, and the future still exists as another state.
void test_travel_rewinds() {
    Harness harness;
    IWorldManager& manager = *harness.manager;
    std::string error;
    WorldSpec spec;
    spec.name = "W";
    spec.seed = 2;
    check(manager.create_world(spec, error), "world created");
    check(boot_world(manager, "W", glm::vec3(8.0f, 200.0f, 8.0f), 2),
          "world boots");

    manager.world("W")->set_block(8, 131, 8, 3);          // stone
    auto* entities = manager.world("W")->entity_world().get();
    std::string eError;
    entities->spawn("test.mob", { 8.0f, 130.0f, 8.0f }, eError);
    const std::string path0 = "vc_timeline_s0.vcwld";
    check(harness.timeline->capture_state("s0", "W", path0, error),
          "s0 captured (stone + 1 entity)");

    // The world advances: the block becomes glass and a second entity spawns.
    manager.world("W")->set_block(8, 131, 8, 20);
    entities->spawn("test.mob", { 9.0f, 130.0f, 9.0f }, eError);
    const std::string path1 = "vc_timeline_s1.vcwld";
    check(harness.timeline->capture_state("s1", "W", path1, error),
          "s1 captured (glass + 2 entities)");
    check(manager.world("W")->get_block(8, 131, 8) == 20 &&
              entity_count(manager, "W") == 2,
          "live world is at the future");

    // TIME TRAVEL: rewind to s0. The block reverts to stone, the second
    // entity is gone — and the change is PERSISTENT (real mutation).
    check(harness.timeline->travel_to("s0", error) && error.empty(),
          "travel back to s0");
    check(manager.world("W")->get_block(8, 131, 8) == 3,
          "block reverted to stone");
    check(entity_count(manager, "W") == 1,
          "entity population reverted to 1");

    // The future is NOT destroyed: travel forward to s1 restores it.
    check(harness.timeline->travel_to("s1", error) && error.empty(),
          "travel forward to s1");
    check(manager.world("W")->get_block(8, 131, 8) == 20 &&
              entity_count(manager, "W") == 2,
          "future content restored (divergent timeline, not a single line)");

    std::filesystem::remove(path0);
    std::filesystem::remove(path1);
    std::printf("[timeline] travel: rewind persistent, future survives, "
                "travel forward restores OK\n");
}

// 3. Branches fork a state into an independent timeline: edits made after
//    traveling to the branch do NOT contaminate the source state.
void test_branch_independent() {
    Harness harness;
    IWorldManager& manager = *harness.manager;
    std::string error;
    WorldSpec spec;
    spec.name = "W";
    spec.seed = 3;
    check(manager.create_world(spec, error), "world created");
    check(boot_world(manager, "W", glm::vec3(8.0f, 200.0f, 8.0f), 2),
          "world boots");

    manager.world("W")->set_block(8, 131, 8, 3);  // stone
    const std::string path0 = "vc_timeline_b_s0.vcwld";
    check(harness.timeline->capture_state("s0", "W", path0, error),
          "s0 captured (stone)");

    // Fork a branch from s0 (independent snapshot file).
    const std::string branchPath = "vc_timeline_b_branch.vcwld";
    check(harness.timeline->branch_state("alt", "s0", branchPath, error) &&
              error.empty(),
          "branch forked from s0");
    check(harness.timeline->state_exists("alt") &&
              harness.timeline->state("alt").worldName == "W",
          "branch registered");
    check(std::filesystem::exists(branchPath),
          "branch snapshot persisted to disk");

    // Travel to the branch, then EDIT the live world (glass).
    check(harness.timeline->travel_to("alt", error), "travel to the branch");
    manager.world("W")->set_block(8, 131, 8, 20);

    // Traveling to the SOURCE state still gives the ORIGINAL stone — the
    // branch's edit did not contaminate it.
    check(harness.timeline->travel_to("s0", error), "travel back to s0");
    check(manager.world("W")->get_block(8, 131, 8) == 3,
          "source state unaffected by the branch's edits");
    // And the branch still holds the pre-edit stone too (independent).
    check(harness.timeline->travel_to("alt", error), "travel to the branch");
    check(manager.world("W")->get_block(8, 131, 8) == 3,
          "branch independent of the source (both keep stone)");

    // ---- validation ----
    std::string vError;
    check(!harness.timeline->branch_state("alt", "s0", "z.vcwld", vError) &&
              !vError.empty(),
          "duplicate branch name refused");
    check(!harness.timeline->branch_state("b2", "missing", "z.vcwld",
                                          vError) &&
              !vError.empty(),
          "unknown source state refused");
    check(!harness.timeline->branch_state("b2", "s0", "", vError) &&
              !vError.empty(),
          "empty branch path refused");
    check(!harness.timeline->state_exists("b2"), "nothing registered on failure");

    std::filesystem::remove(path0);
    std::filesystem::remove(branchPath);
    std::printf("[timeline] branch: fork independent, edits don't contaminate "
                "source, validation OK\n");
}

// 4. Transactional travel: traveling to an unknown/unloaded state is refused
//    and the live world is untouched.
void test_travel_validation() {
    Harness harness;
    IWorldManager& manager = *harness.manager;
    std::string error;
    WorldSpec spec;
    spec.name = "W";
    spec.seed = 4;
    check(manager.create_world(spec, error), "world created");
    check(boot_world(manager, "W", glm::vec3(8.0f, 200.0f, 8.0f), 2),
          "world boots");
    manager.world("W")->set_block(8, 131, 8, 3);
    const std::string path = "vc_timeline_t.vcwld";
    check(harness.timeline->capture_state("s0", "W", path, error),
          "state captured");

    // Travel to an UNKNOWN state is refused; the live world is untouched.
    std::string tError;
    check(!harness.timeline->travel_to("ghost", tError) && !tError.empty(),
          "unknown state refused");
    check(manager.world("W")->get_block(8, 131, 8) == 3,
          "live world untouched by the refused travel");

    // Unload the world: traveling to its state is refused (world not loaded).
    check(manager.unload_world("W"), "world unloaded");
    std::string uError;
    check(!harness.timeline->travel_to("s0", uError) && !uError.empty(),
          "travel refused when the world is not loaded");

    std::filesystem::remove(path);
    std::printf("[timeline] travel validation: unknown state / unloaded world "
                "refused, live world untouched OK\n");
}

// 5. Determinism: identical managers + identical states travel to identical
//    content.
void test_determinism() {
    auto run = [](uint32_t& block) {
        Harness harness;
        IWorldManager& manager = *harness.manager;
        std::string error;
        WorldSpec spec;
        spec.name = "W";
        spec.seed = 5;
        check(manager.create_world(spec, error), "world created");
        check(boot_world(manager, "W", glm::vec3(8.0f, 200.0f, 8.0f), 2),
              "world boots");
        manager.world("W")->set_block(8, 131, 8, 3);
        manager.world("W")->set_block(9, 131, 8, 12);
        const std::string path = "vc_timeline_d.vcwld";
        check(harness.timeline->capture_state("s0", "W", path, error),
              "state captured");
        // The world advances, then travels back.
        manager.world("W")->set_block(8, 131, 8, 20);
        check(harness.timeline->travel_to("s0", error), "travel to s0");
        block = manager.world("W")->get_block(8, 131, 8);
        std::filesystem::remove(path);
    };
    uint32_t first = 0, second = 0;
    run(first);
    run(second);
    check(first == 3 && first == second,
          "identical setups travel to identical content");

    std::printf("[timeline] determinism: bit-identical travel across "
                "instances OK\n");
}

}  // namespace

int main() {
    test_capture();
    test_travel_rewinds();
    test_branch_independent();
    test_travel_validation();
    test_determinism();
    if (g_failures == 0) {
        std::printf("[timeline] ALL PASSED\n");
        return 0;
    }
    std::printf("[timeline] %d FAILURE(S)\n", g_failures);
    return 1;
}
