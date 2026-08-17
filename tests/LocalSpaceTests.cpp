// LocalSpaceTests.cpp
//
// Evidence for FALTANTES §15 "espaços locais hierárquicos para planetas e
// veículos grandes" (META §19):
//   - hierarchy: a tree of local frames (planet -> vehicle -> seat) rooted at
//     the world frame, each with a rigid transform (position + yaw, double);
//     create/query/remove with all-or-nothing validation;
//   - conversions: space_to_world / world_to_space round-trip BIT-EXACTLY
//     (cardinal yaw snaps cos/sin), nested chains compose, yaw rotates;
//   - moving frame carries contents: bind an entity to a vehicle space, move
//     the vehicle — the entity's ABSOLUTE position follows while its stored
//     position stays space-local;
//   - precision: a vehicle at absolute ~1e9 absorbs the offset; the bound
//     entity's float local stays small/exact and the absolute round-trips;
//   - persistence: the binding is a reserved component (engine.space_ref), so
//     it survives serialize/deserialize without a separate registry;
//   - composition with origin rebasing: a rebase SKIPS space-bound entities
//     (their position is space-local) and refuses to read them as world
//     positions;
//   - unbind converts back to the world frame (no teleport);
//   - determinism: identical setups produce bit-identical conversions.

#include <engine/world/ILocalSpace.hpp>
#include <engine/world/IOriginRebase.hpp>

#include <glm/glm.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace engine::world;
using engine::entity::EntityId;
using engine::entity::Position;

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
    std::unique_ptr<ILocalSpace> spaces = create_local_space(*manager);
    std::unique_ptr<IOriginRebase> rebase = create_origin_rebase(*manager);
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

EntityId spawn_entity(IWorldManager& manager, const std::string& worldName,
                      float x, float y, float z) {
    engine::voxel::IVoxelWorld* world = manager.world(worldName);
    if (world == nullptr) return {};
    auto* entities = world->entity_world().get();
    if (entities == nullptr) return {};
    std::string error;
    Position position{ x, y, z };
    return entities->spawn("test.mob", position, error);
}

// 1. Hierarchy: planet -> vehicle -> seat; create/query/remove + validation.
void test_hierarchy() {
    Harness harness;
    std::string error;

    check(harness.spaces->create_space("planet", "", { { 1000.0, 0.0, 500.0 }, 0.0f },
                                       error),
          "planet created on the world root");
    check(harness.spaces->create_space("vehicle", "planet",
                                       { { 20.0, 3.0, -4.0 }, 0.0f }, error),
          "vehicle created on the planet");
    check(harness.spaces->create_space("seat", "vehicle",
                                       { { 0.0, 1.2, 0.0 }, 0.0f }, error),
          "seat created on the vehicle");

    check(harness.spaces->space_exists("planet") &&
              harness.spaces->space_exists("vehicle") &&
              harness.spaces->space_exists("seat"),
          "all spaces exist");
    check(harness.spaces->space_parent("planet").empty(),
          "planet's parent is the world root");
    check(harness.spaces->space_parent("vehicle") == "planet",
          "vehicle's parent is the planet");
    check(harness.spaces->space_parent("seat") == "vehicle",
          "seat's parent is the vehicle");

    SpaceTransform transform;
    check(harness.spaces->space_transform("vehicle", transform) &&
              dvec_eq(transform.position, glm::dvec3(20.0, 3.0, -4.0)) &&
              transform.yawDegrees == 0.0f,
          "vehicle transform reads back");

    // ---- validation (all-or-nothing) ----
    std::string vError;
    check(!harness.spaces->create_space("", "planet", {}, vError) &&
              !vError.empty(),
          "empty space name refused");
    check(!harness.spaces->create_space("planet", "", {}, vError) &&
              !vError.empty(),
          "duplicate space name refused");
    check(!harness.spaces->create_space("ghost", "missing", {}, vError) &&
              !vError.empty(),
          "unknown parent refused");
    check(!harness.spaces->remove_space("vehicle", vError) && !vError.empty(),
          "removing a space with children refused");
    check(!harness.spaces->remove_space("ghost", vError) && !vError.empty(),
          "removing an unknown space refused");

    check(harness.spaces->remove_space("seat", vError),
          "leaf space removed");
    check(harness.spaces->remove_space("vehicle", vError),
          "vehicle removed after its leaf");
    check(harness.spaces->remove_space("planet", vError),
          "planet removed");

    std::printf("[local-space] hierarchy: tree create/query/remove + "
                "all-or-nothing validation OK\n");
}

// 2. Conversions: bit-exact round-trip, nested chain composition, yaw.
void test_conversions() {
    Harness harness;
    std::string error;
    harness.spaces->create_space("planet", "", { { 1000.0, 0.0, 500.0 }, 0.0f },
                                 error);
    harness.spaces->create_space("vehicle", "planet",
                                 { { 20.0, 3.0, -4.0 }, 90.0f }, error);
    harness.spaces->create_space("seat", "vehicle",
                                 { { 1.0, 1.5, 0.5 }, 0.0f }, error);

    // Round-trip: world -> space -> world is bit-exact.
    const glm::dvec3 world(1023.25, 5.5, 497.75);
    glm::dvec3 local, back;
    check(harness.spaces->world_to_space("vehicle", world, local, error) &&
              error.empty(),
          "world_to_space on the vehicle");
    check(harness.spaces->space_to_world("vehicle", local, back, error) &&
              dvec_eq(back, world),
          "round-trip world->vehicle->world is BIT-EXACT");

    // Nested chain: a seat-local point maps through seat -> vehicle -> planet.
    // seat at (1, 1.5, 0.5) relative to vehicle; vehicle at (20, 3, -4) yaw 90
    // relative to planet; planet at (1000, 0, 500). Seat-local origin in world
    // (all values exactly representable in binary -> bit-exact):
    //   seat frame: (0,0,0)
    //   seat -> vehicle: R(0)*(0,0,0) + (1,1.5,0.5)        = (1, 1.5, 0.5)
    //   vehicle -> planet: R(90)*(1,1.5,0.5) + (20,3,-4)
    //     R(90): (x*c+z*s, y, -x*s+z*c) = (0.5, 1.5, -1)   = (20.5, 4.5, -5)
    //   planet -> world: R(0)*(20.5,4.5,-5) + (1000,0,500) = (1020.5, 4.5, 495)
    glm::dvec3 seatWorld;
    check(harness.spaces->space_to_world("seat", glm::dvec3(0.0, 0.0, 0.0),
                                         seatWorld, error),
          "seat origin to world");
    check(dvec_eq(seatWorld, glm::dvec3(1020.5, 4.5, 495.0)),
          "seat origin composes through the chain");

    // Yaw rotates around Y: vehicle-local +X at yaw 90 maps to planet -Z
    // (rotation matrix: x' = x*cos + z*sin, z' = -x*sin + z*cos).
    //   vehicle frame: (2, 0, 0) -> R(90) -> (0, 0, -2); + vehicle pos
    //   (20,3,-4) -> (20, 3, -6); + planet pos -> (1020, 3, 494).
    glm::dvec3 rotated;
    harness.spaces->space_to_world("vehicle", glm::dvec3(2.0, 0.0, 0.0),
                                   rotated, error);
    check(dvec_eq(rotated, glm::dvec3(1020.0, 3.0, 494.0)),
          "yaw 90 rotates +X to -Z in the parent frame");

    // Inverse is consistent for the same point (bit-exact, cardinal yaw).
    glm::dvec3 localBack;
    check(harness.spaces->world_to_space("seat", seatWorld, localBack, error) &&
              dvec_eq(localBack, glm::dvec3(0.0, 0.0, 0.0)),
          "world->seat round-trip through the nested chain");

    std::printf("[local-space] conversions: bit-exact round-trip, nested "
                "chain composition, yaw OK\n");
}

// 3. Moving a frame carries its occupants: the entity's absolute position
//    follows the vehicle while its stored position stays space-local.
void test_move_carries_contents() {
    Harness harness;
    IWorldManager& manager = *harness.manager;
    std::string error;
    WorldSpec spec;
    spec.name = "W";
    spec.seed = 1;
    check(manager.create_world(spec, error), "world created");
    check(boot_world(manager, "W", glm::vec3(8.0f, 200.0f, 8.0f), 2),
          "world boots");

    harness.spaces->create_space("planet", "", { { 0.0, 0.0, 0.0 }, 0.0f },
                                 error);
    harness.spaces->create_space("vehicle", "planet",
                                 { { 100.0, 10.0, -50.0 }, 0.0f }, error);
    harness.spaces->create_space("seat", "vehicle",
                                 { { 0.0, 1.0, 0.0 }, 0.0f }, error);

    const EntityId passenger = spawn_entity(manager, "W", 0.0f, 0.0f, 0.0f);
    check(passenger.valid(), "passenger spawned");
    check(harness.spaces->bind_entity("W", passenger, "seat",
                                      glm::vec3(0.5f, 1.25f, 0.25f), error),
          "passenger bound to the seat");

    glm::dvec3 before;
    check(harness.spaces->entity_world_position("W", passenger, before, error),
          "absolute position before the move");
    // seat-local (0.5,1.25,0.25) -> seat + (0,1,0) -> vehicle + (100,10,-50)
    // -> planet (0,0,0): all exactly representable in float32/double.
    check(dvec_eq(before, glm::dvec3(100.5, 12.25, -49.75)),
          "bound entity sits at seat + vehicle + planet");

    // The vehicle drives forward (+X, world direction) by 30 units.
    check(harness.spaces->move_space("vehicle", glm::dvec3(30.0, 0.0, 0.0)),
          "vehicle moved");
    glm::dvec3 after;
    check(harness.spaces->entity_world_position("W", passenger, after, error),
          "absolute position after the move");
    check(dvec_eq(after, glm::dvec3(130.5, 12.25, -49.75)),
          "passenger follows the vehicle (+30 in X)");

    // The stored position is UNCHANGED (still seat-local).
    Position stored;
    manager.world("W")->entity_world()->get_position(passenger, stored);
    check(stored.x == 0.5f && stored.y == 1.25f && stored.z == 0.25f,
          "stored position stays seat-local through the move");

    // The seat (a descendant) inherits the move: seat origin moved too.
    glm::dvec3 seatWorld;
    harness.spaces->space_to_world("seat", glm::dvec3(0.0, 0.0, 0.0), seatWorld,
                                   error);
    check(dvec_eq(seatWorld, glm::dvec3(130.0, 11.0, -50.0)),
          "descendant frames inherit the parent move");

    std::printf("[local-space] move carries contents: absolute follows, "
                "stored stays space-local, descendants inherit OK\n");
}

// 4. Precision: a vehicle far from the world origin absorbs the offset; the
//    bound entity's float local stays small and the absolute round-trips.
void test_precision_at_distance() {
    Harness harness;
    IWorldManager& manager = *harness.manager;
    std::string error;
    WorldSpec spec;
    spec.name = "W";
    spec.seed = 2;
    check(manager.create_world(spec, error), "world created");
    check(boot_world(manager, "W", glm::vec3(0.0f, 0.0f, 0.0f), 2),
          "world boots");

    // Vehicle at absolute ~1e9.
    harness.spaces->create_space("vehicle", "",
                                 { { 1000000000.0, 1000000000.0, 1000000000.0 },
                                   0.0f },
                                 error);
    const EntityId passenger = spawn_entity(manager, "W", 0.0f, 0.0f, 0.0f);
    check(harness.spaces->bind_entity("W", passenger, "vehicle",
                                      glm::vec3(0.25f, 1.5f, 0.75f), error),
          "passenger bound at a sub-block offset");

    glm::dvec3 absolute;
    check(harness.spaces->entity_world_position("W", passenger, absolute,
                                                error),
          "absolute position at 1e9");
    check(dvec_eq(absolute,
                  glm::dvec3(1000000000.25, 1000000001.5, 1000000000.75)),
          "absolute round-trips BIT-EXACT with the vehicle absorbing 1e9");

    // The stored float local is the small exact part (what raw float32 at 1e9
    // cannot represent — ulp 64).
    Position stored;
    manager.world("W")->entity_world()->get_position(passenger, stored);
    check(stored.x == 0.25f && stored.y == 1.5f && stored.z == 0.75f,
          "stored local carries the exact sub-block offsets");

    std::printf("[local-space] precision: vehicle at 1e9 absorbs the offset, "
                "bound local stays exact OK\n");
}

// 5. Persistence: the binding is a reserved component, so serialize/
//    deserialize into a fresh world keeps it (no separate registry).
void test_binding_persists() {
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

    harness.spaces->create_space("vehicle", "", { { 50.0, 2.0, 0.0 }, 0.0f },
                                 error);
    const EntityId passenger = spawn_entity(manager, "A", 0.0f, 0.0f, 0.0f);
    check(harness.spaces->bind_entity("A", passenger, "vehicle",
                                      glm::vec3(1.0f, 1.0f, 1.0f), error),
          "passenger bound");

    // Serialize from A, deserialize into B (same manager, same space tree).
    auto* entitiesA = manager.world("A")->entity_world().get();
    const auto snapshots = entitiesA->serialize_entities();
    auto* entitiesB = manager.world("B")->entity_world().get();
    check(entitiesB->deserialize_entities(snapshots, error),
          "entities deserialized into world B");

    // The binding survives: entity_space reads the reserved component.
    bool foundBound = false;
    entitiesB->for_each_entity([&](EntityId handle) {
        if (harness.spaces->entity_space("B", handle) == "vehicle") {
            foundBound = true;
        }
    });
    check(foundBound, "binding survives serialize/deserialize via the "
                      "reserved component");

    glm::dvec3 absolute;
    entitiesB->for_each_entity([&](EntityId handle) {
        glm::dvec3 p;
        if (harness.spaces->entity_world_position("B", handle, p, error)) {
            absolute = p;
        }
    });
    check(dvec_eq(absolute, glm::dvec3(51.0, 3.0, 1.0)),
          "restored entity's absolute position uses the space chain");

    std::printf("[local-space] binding persists through serialize/deserialize "
                "(reserved component) OK\n");
}

// 6. Composition with origin rebasing: a rebase SKIPS space-bound entities
//    (space-local position) and refuses to read them as world positions.
void test_rebase_composition() {
    Harness harness;
    IWorldManager& manager = *harness.manager;
    std::string error;
    WorldSpec spec;
    spec.name = "W";
    spec.seed = 3;
    check(manager.create_world(spec, error), "world created");
    check(boot_world(manager, "W", glm::vec3(8.0f, 200.0f, 8.0f), 2),
          "world boots");

    harness.spaces->create_space("vehicle", "", { { 100.0, 10.0, 0.0 }, 0.0f },
                                 error);
    const EntityId bound = spawn_entity(manager, "W", 0.0f, 0.0f, 0.0f);
    const EntityId free = spawn_entity(manager, "W", 8.0f, 130.0f, 8.0f);
    check(harness.spaces->bind_entity("W", bound, "vehicle",
                                      glm::vec3(1.0f, 1.0f, 1.0f), error),
          "bound entity");
    check(free.valid(), "free entity");

    // Absolute positions before the rebase.
    glm::dvec3 boundBefore, freeBefore;
    harness.spaces->entity_world_position("W", bound, boundBefore, error);
    harness.rebase->absolute_position("W", free, freeBefore, error);

    // Rebase to a focus NEAR the free entity (the documented float32-exact
    // zone) — the bound entity must be SKIPPED (its position is space-local).
    RebaseResult r = harness.rebase->rebase(glm::dvec3(8.0, 130.0, 8.0), error);
    check(r.rebased && error.empty(), "rebase applied");
    check(r.translatedEntities == 1, "only the FREE entity is translated "
                                     "(bound entity skipped)");

    // The bound entity's stored position is UNTOUCHED (space-local).
    Position stored;
    manager.world("W")->entity_world()->get_position(bound, stored);
    check(stored.x == 1.0f && stored.y == 1.0f && stored.z == 1.0f,
          "bound entity's stored position untouched by the rebase");

    // Its absolute position (via the space chain) is unchanged.
    glm::dvec3 boundAfter;
    harness.spaces->entity_world_position("W", bound, boundAfter, error);
    check(dvec_eq(boundAfter, boundBefore),
          "bound entity's absolute position invariant under the rebase");

    // The free entity's absolute position (via the rebase service) is
    // unchanged, and the rebase service REFUSES to read the bound entity.
    glm::dvec3 freeAfter;
    check(harness.rebase->absolute_position("W", free, freeAfter, error) &&
              dvec_eq(freeAfter, freeBefore),
          "free entity's absolute position invariant under the rebase");
    std::string refError;
    glm::dvec3 ignored;
    check(!harness.rebase->absolute_position("W", bound, ignored, refError) &&
              !refError.empty(),
          "rebase service refuses to read a space-bound entity as world "
          "position (use ILocalSpace)");

    std::printf("[local-space] rebase composition: bound entities skipped, "
                "absolutes invariant, world-read refused OK\n");
}

// 7. Unbind converts back to the world frame (no teleport).
void test_unbind() {
    Harness harness;
    IWorldManager& manager = *harness.manager;
    std::string error;
    WorldSpec spec;
    spec.name = "W";
    spec.seed = 4;
    check(manager.create_world(spec, error), "world created");
    check(boot_world(manager, "W", glm::vec3(8.0f, 200.0f, 8.0f), 2),
          "world boots");

    harness.spaces->create_space("vehicle", "", { { 100.0, 10.0, -50.0 }, 0.0f },
                                 error);
    const EntityId passenger = spawn_entity(manager, "W", 0.0f, 0.0f, 0.0f);
    check(harness.spaces->bind_entity("W", passenger, "vehicle",
                                      glm::vec3(0.5f, 1.2f, 0.25f), error),
          "passenger bound");
    harness.spaces->move_space("vehicle", glm::dvec3(30.0, 0.0, 0.0));

    check(harness.spaces->unbind_entity("W", passenger), "unbound");
    check(harness.spaces->entity_space("W", passenger).empty(),
          "binding gone after unbind");
    // Position converted to the world frame: (100+30+0.5, 10+1.2, -50+0.25).
    Position stored;
    manager.world("W")->entity_world()->get_position(passenger, stored);
    check(stored.x == 130.5f && stored.y == 11.2f && stored.z == -49.75f,
          "unbind converts to the world frame (no teleport)");
    check(harness.spaces->unbind_entity("W", passenger),
          "unbinding an unbound entity is a no-op success");

    std::printf("[local-space] unbind: converts to world frame, no teleport, "
                "no-op when unbound OK\n");
}

// 8. Determinism: identical setups produce bit-identical conversions.
void test_determinism() {
    auto run = [](glm::dvec3& seatWorld, glm::dvec3& roundTrip) {
        Harness harness;
        std::string error;
        harness.spaces->create_space("planet", "", { { 1000.0, 0.0, 500.0 },
                                                      0.0f },
                                     error);
        harness.spaces->create_space("vehicle", "planet",
                                     { { 20.0, 3.0, -4.0 }, 90.0f }, error);
        harness.spaces->create_space("seat", "vehicle",
                                     { { 1.0, 1.2, 0.5 }, 0.0f }, error);
        harness.spaces->space_to_world("seat", glm::dvec3(0.0, 0.0, 0.0),
                                       seatWorld, error);
        harness.spaces->world_to_space("seat", seatWorld, roundTrip, error);
    };
    glm::dvec3 w1, r1, w2, r2;
    run(w1, r1);
    run(w2, r2);
    check(dvec_eq(w1, w2) && dvec_eq(r1, r2),
          "identical setups produce bit-identical conversions");

    std::printf("[local-space] determinism: bit-identical across instances "
                "OK\n");
}

}  // namespace

int main() {
    test_hierarchy();
    test_conversions();
    test_move_carries_contents();
    test_precision_at_distance();
    test_binding_persists();
    test_rebase_composition();
    test_unbind();
    test_determinism();
    if (g_failures == 0) {
        std::printf("[local-space] ALL PASSED\n");
        return 0;
    }
    std::printf("[local-space] %d FAILURE(S)\n", g_failures);
    return 1;
}
