// WorldReplicationTests.cpp
//
// Evidence for FALTANTES §19 "replicação e interesse por mundo"
// (IWorldReplication — the world-scoped replication service over
// IWorldManager):
//   - interest carries the world identity: two worlds with OVERLAPPING
//     coordinates stream their own content to their own observers with
//     zero leakage between connections;
//   - edits are routed BY WORLD: server_submit_edit mutates only the world
//     the connection observes, and server_broadcast_edits("A") reaches only
//     the connections registered on A;
//   - a portal crossing is an interest transition: server_set_interest with
//     a new world atomically re-binds the connection (its streams now come
//     from the destination world);
//   - client_bind selects the ONE local world the client side acts on;
//   - deterministic per (world, interest) region packs; all-or-nothing
//     validation (unknown connection/world, no leak on refusal).

#include <engine/world/IWorldReplication.hpp>

#include <glm/glm.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace engine::world;
using engine::voxel::IVoxelWorld;
using engine::voxel::ReplicationConnectionId;

namespace {

constexpr ReplicationConnectionId kConnA = 1;
constexpr ReplicationConnectionId kConnB = 2;
constexpr ReplicationConnectionId kConnC = 3;

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

// Owns the manager + replication under test (keeps the unique_ptr alive — a
// temporary unique_ptr would dangle the reference).
struct Harness {
    std::unique_ptr<IWorldManager> manager = create_world_manager();
};

// Boots a manager world (registers the generator, then streams chunk (0,0)
// via update_world until it loads — the same pattern the world_manager test
// uses, with the streaming budget).
bool boot_world(IWorldManager& manager, const std::string& name,
                const glm::vec3& player, int budget) {
    IVoxelWorld* world = manager.world(name);
    if (world == nullptr) return false;
    // The default world is all-Air without a generator (external-gate
    // finding); register a flat terrain so chunks carry solid surface.
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

int surface_y(IWorldManager& manager, const std::string& name, int x, int z) {
    IVoxelWorld* world = manager.world(name);
    if (world == nullptr) return -1;
    for (int y = 200; y >= 0; --y) {
        if (world->get_block(x, y, z) != 0) return y;
    }
    return -1;
}

// Returns the block id at (x, y, z) inside the LAST snapshot of chunk (0,0),
// or -1 when the chunk is absent. The LAST one is used because a dirty chunk
// re-streams a NEWER snapshot that supersedes the earlier one (the SDK
// replication test does the same for its wall).
int snapshot_block(const std::vector<engine::voxel::ChunkReplicationSnapshot>&
                       snapshots,
                   int x, int y, int z) {
    const engine::voxel::ChunkReplicationSnapshot* last = nullptr;
    for (const auto& snapshot : snapshots) {
        if (snapshot.chunkX == 0 && snapshot.chunkZ == 0) last = &snapshot;
    }
    if (last == nullptr) return -1;
    const std::size_t index =
        (static_cast<std::size_t>(y) * engine::voxel::kReplicationChunkSize +
         z) *
            engine::voxel::kReplicationChunkSize +
        x;
    if (index >= last->blocks.size()) return -1;
    return static_cast<int>(last->blocks[index]);
}

bool regions_equal(const engine::voxel::RegionReplicationSnapshot& a,
                   const engine::voxel::RegionReplicationSnapshot& b) {
    // The region's CONTENT is deterministic per (world, interest); the
    // sequence field is a monotonic packet counter (advances with every
    // pack call), so it is deliberately excluded from the content compare.
    if (a.origin != b.origin || a.chunkRadius != b.chunkRadius ||
        a.chunks.size() != b.chunks.size() ||
        a.blockEntities.size() != b.blockEntities.size() ||
        a.cells.size() != b.cells.size() ||
        a.entities.size() != b.entities.size())
        return false;
    for (std::size_t i = 0; i < a.chunks.size(); ++i) {
        if (a.chunks[i].chunkX != b.chunks[i].chunkX ||
            a.chunks[i].chunkZ != b.chunks[i].chunkZ ||
            a.chunks[i].minY != b.chunks[i].minY ||
            a.chunks[i].height != b.chunks[i].height ||
            a.chunks[i].blocks != b.chunks[i].blocks)
            return false;
    }
    return true;
}

// 1. Interest BY WORLD: two worlds with the SAME coordinates hold different
//    blocks; each connection observes only its own world — no leakage.
void test_interest_by_world() {
    Harness harness;
    IWorldManager& manager = *harness.manager;
    std::string error;

    WorldSpec overworld;
    overworld.name = "overworld";
    overworld.seed = 11;
    WorldSpec nether;
    nether.name = "nether";
    nether.seed = 22;
    check(manager.create_world(overworld, error), "overworld created");
    check(manager.create_world(nether, error), "nether created");
    check(boot_world(manager, "overworld", glm::vec3(8.0f, 200.0f, 8.0f), 2),
          "overworld boots");
    check(boot_world(manager, "nether", glm::vec3(8.0f, 200.0f, 8.0f), 2),
          "nether boots");

    const int surface = surface_y(manager, "overworld", 8, 8);
    check(surface > 0, "surface found");

    auto replication = create_world_replication(manager);
    check(replication != nullptr, "world replication created");

    // Connection A observes the OVERWORLD, connection B the NETHER — both at
    // the same position, same chunk radius.
    WorldReplicationInterest overInterest;
    overInterest.worldName = "overworld";
    overInterest.interest.position = glm::ivec3(8, surface, 8);
    overInterest.interest.chunkRadius = 1;
    WorldReplicationInterest nehInterest;
    nehInterest.worldName = "nether";
    nehInterest.interest.position = glm::ivec3(8, surface, 8);
    nehInterest.interest.chunkRadius = 1;

    check(replication->server_register_connection(kConnA, overInterest, error),
          "connection A registered on overworld");
    check(replication->server_register_connection(kConnB, nehInterest, error),
          "connection B registered on nether");
    // The snapshot window must cover the test cells above the terrain (the
    // default [0,127] would cut a surface+1 at 128). The per-world services
    // are created lazily on first registration, so set the window AFTER the
    // connections exist (same as the SDK replication test, which widens it).
    replication->server_set_snapshot_window(0, surface + 8);
    replication->server_update();
    replication->server_update();

    // Place the test blocks THROUGH the connections (server_submit_edit): a
    // direct set_block would not mark the replication view's dirty chunks, so
    // the already-streamed chunk would never re-stream (the SDK replication
    // test builds its wall with submit_edit for the same reason). Each edit
    // also proves the routing: A's edit lands in the OVERWORLD, B's in the
    // NETHER — at the SAME coordinates. The server enforces a per-connection
    // edit cooldown, so advance the tick between submits (SDK pattern).
    auto wallA = replication->server_submit_edit(kConnA, 8, surface + 1, 8, 3);
    replication->server_update();
    replication->server_update();
    auto glassB = replication->server_submit_edit(kConnB, 8, surface + 1, 8, 20);
    replication->server_update();
    replication->server_update();
    auto dirtB = replication->server_submit_edit(kConnB, 10, surface + 1, 10, 4);
    replication->server_update();
    replication->server_update();
    check(wallA.accepted && glassB.accepted && dirtB.accepted,
          "test edits accepted (routed per world)");

    const auto overPacks = replication->server_pack_interest(kConnA);
    const auto nehPacks = replication->server_pack_interest(kConnB);
    check(snapshot_block(overPacks, 8, surface + 1, 8) == 3,
          "A streams stone from the OVERWORLD");
    check(snapshot_block(nehPacks, 8, surface + 1, 8) == 20,
          "B streams glass from the NETHER (same coords, no leak)");
    check(snapshot_block(overPacks, 10, surface + 1, 10) != 4,
          "A never sees the nether-only dirt");
    check(snapshot_block(nehPacks, 10, surface + 1, 10) == 4,
          "B sees the nether-only dirt");

    // Region packs are deterministic per (world, interest).
    engine::voxel::RegionReplicationSnapshot r1, r2;
    check(replication->server_pack_region(kConnA, r1, error),
          "region pack A ok");
    check(replication->server_pack_region(kConnA, r2, error),
          "region pack A again ok");
    check(regions_equal(r1, r2), "region pack deterministic (A)");

    std::printf("[world-repl] interest by world: same coords, per-world "
                "streams, zero leak, deterministic region OK\n");
}

// 2. Edits are routed BY WORLD: server_submit_edit mutates only the world the
//    connection observes, and server_broadcast_edits("overworld") reaches
//    only the connections on the overworld.
void test_edit_routing() {
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
    const int surface = surface_y(manager, "A", 8, 8);
    check(surface > 0, "surface found");

    auto replication = create_world_replication(manager);
    WorldReplicationInterest interestA;
    interestA.worldName = "A";
    interestA.interest.position = glm::ivec3(8, surface, 8);
    interestA.interest.chunkRadius = 1;
    check(replication->server_register_connection(kConnA, interestA, error),
          "conn A registered on world A");
    replication->server_update();
    replication->server_update();

    // Edit through the connection: it lands in world A, never world B.
    auto result = replication->server_submit_edit(kConnA, 8, surface + 1, 8, 3);
    check(result.accepted && result.error.empty(), "edit accepted");
    check(manager.world("A")->get_block(8, surface + 1, 8) == 3,
          "edit applied to world A");
    check(manager.world("B")->get_block(8, surface + 1, 8) == 0,
          "world B untouched (routing)");

    // Broadcast of a world-A commit reaches A's observer...
    engine::voxel::BlockEdit edit;
    edit.position = glm::ivec3(9, surface + 1, 9);
    edit.blockId = 4;
    edit.previousBlockId = 0;
    std::vector<engine::voxel::BlockEdit> edits = { edit };
    replication->server_broadcast_edits("A", edits);
    replication->server_update();
    auto batch = replication->server_pack_batch(kConnA);
    bool delivered = false;
    for (const auto& delta : batch.deltas) {
        if (delta.position == edit.position && delta.blockId == 4) delivered = true;
    }
    check(delivered, "A's broadcast reaches A's observer");

    // ... and a broadcast of world B reaches NOBODY (no observer on B).
    // Register a second connection on A and confirm a B broadcast is a no-op
    // for everyone observing A.
    WorldReplicationInterest interestB;
    interestB.worldName = "A";
    interestB.interest.position = glm::ivec3(8, surface, 8);
    interestB.interest.chunkRadius = 1;
    check(replication->server_register_connection(kConnB, interestB, error),
          "conn B registered on world A too");
    engine::voxel::BlockEdit editB;
    editB.position = glm::ivec3(11, surface + 1, 11);
    editB.blockId = 5;
    std::vector<engine::voxel::BlockEdit> editsB = { editB };
    replication->server_broadcast_edits("B", editsB);
    replication->server_update();
    auto batchB = replication->server_pack_batch(kConnB);
    bool leaked = false;
    for (const auto& delta : batchB.deltas) {
        if (delta.position == editB.position) leaked = true;
    }
    check(!leaked, "world B broadcast never reaches A's observers");

    std::printf("[world-repl] edit routing: submit lands in the observed "
                "world, broadcast scoped to the world OK\n");
}

// 3. Portal crossing = interest transition: re-binding a connection to
//    another world atomically switches its streams to the destination world.
void test_portal_transition() {
    Harness harness;
    IWorldManager& manager = *harness.manager;
    std::string error;
    WorldSpec over, neh;
    over.name = "overworld";
    over.seed = 11;
    neh.name = "nether";
    neh.seed = 22;
    check(manager.create_world(over, error) && manager.create_world(neh, error),
          "two worlds created");
    check(boot_world(manager, "overworld", glm::vec3(8.0f, 200.0f, 8.0f), 2) &&
              boot_world(manager, "nether", glm::vec3(8.0f, 200.0f, 8.0f), 2),
          "both boot");
    const int surface = surface_y(manager, "overworld", 8, 8);
    check(surface > 0, "surface found");

    auto replication = create_world_replication(manager);
    WorldReplicationInterest overInterest;
    overInterest.worldName = "overworld";
    overInterest.interest.position = glm::ivec3(8, surface, 8);
    overInterest.interest.chunkRadius = 1;
    check(replication->server_register_connection(kConnA, overInterest, error),
          "conn A on overworld");
    // The window is sticky: it applies to every service, including ones
    // created later (the per-world services are lazy).
    replication->server_set_snapshot_window(0, surface + 8);
    replication->server_update();
    replication->server_update();
    // Distinct content at the SAME coordinates per world (submitted through
    // each world's service so the dirty chunks re-stream).
    WorldReplicationInterest nehServiceInterest;
    nehServiceInterest.worldName = "nether";
    nehServiceInterest.interest.position = glm::ivec3(8, surface, 8);
    nehServiceInterest.interest.chunkRadius = 1;
    std::string nehError;
    check(replication->server_register_connection(kConnB, nehServiceInterest,
                                                  nehError),
          "conn B on nether (content setup)");
    auto stoneOver =
        replication->server_submit_edit(kConnA, 8, surface + 1, 8, 3);
    replication->server_update();
    replication->server_update();
    auto glassNeh =
        replication->server_submit_edit(kConnB, 8, surface + 1, 8, 20);
    replication->server_update();
    replication->server_update();
    check(stoneOver.accepted && glassNeh.accepted,
          "content edits accepted (stone overworld, glass nether)");
    check(snapshot_block(replication->server_pack_interest(kConnA),
                         8, surface + 1, 8) == 3,
          "A sees overworld stone before travel");

    // Travel: the connection's interest moves to the nether (portal
    // crossing). After the transition its streams come from the nether.
    WorldReplicationInterest nehInterest;
    nehInterest.worldName = "nether";
    nehInterest.interest.position = glm::ivec3(8, surface, 8);
    nehInterest.interest.chunkRadius = 1;
    check(replication->server_set_interest(kConnA, nehInterest, error),
          "interest transition to nether");
    check(replication->server_interest(kConnA).worldName == "nether",
          "connection now observes the nether");
    replication->server_update();
    replication->server_update();
    check(snapshot_block(replication->server_pack_interest(kConnA),
                         8, surface + 1, 8) == 20,
          "A streams nether glass after the transition");

    // The old world no longer delivers to the connection: an overworld
    // broadcast never reaches A now.
    engine::voxel::BlockEdit edit;
    edit.position = glm::ivec3(9, surface + 1, 9);
    edit.blockId = 4;
    std::vector<engine::voxel::BlockEdit> edits = { edit };
    replication->server_broadcast_edits("overworld", edits);
    replication->server_update();
    auto batch = replication->server_pack_batch(kConnA);
    bool leaked = false;
    for (const auto& delta : batch.deltas) {
        if (delta.position == edit.position) leaked = true;
    }
    check(!leaked, "post-travel A ignores overworld broadcasts");

    // Unknown world on transition is refused and the connection stays put.
    WorldReplicationInterest bad;
    bad.worldName = "missing";
    std::string transitionError;
    check(!replication->server_set_interest(kConnA, bad, transitionError) &&
              !transitionError.empty(),
          "transition to unknown world refused");
    check(replication->server_interest(kConnA).worldName == "nether",
          "refused transition leaves the connection on the nether");

    std::printf("[world-repl] portal transition: interest re-bind switches "
                "world streams atomically OK\n");
}

// 4. Client side binds to ONE world: predict/apply act on the bound world's
//    local IVoxelWorld, and re-binding switches the target.
void test_client_bind() {
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
    const int surface = surface_y(manager, "A", 8, 8);
    check(surface > 0, "surface found");

    auto replication = create_world_replication(manager);
    check(replication->client_bind("A", error), "client binds world A");
    check(replication->client_world() == "A", "client world is A");

    // Predict acts on world A.
    check(replication->client_predict(8, surface + 1, 8, 3), "predict in A");
    check(manager.world("A")->get_block(8, surface + 1, 8) == 3,
          "predict applied to local world A");
    check(manager.world("B")->get_block(8, surface + 1, 8) == 0,
          "world B untouched by client on A");

    // Re-bind to B: predict now acts on world B.
    check(replication->client_bind("B", error), "client binds world B");
    check(replication->client_world() == "B", "client world is B");
    check(replication->client_predict(8, surface + 1, 8, 4), "predict in B");
    check(manager.world("B")->get_block(8, surface + 1, 8) == 4,
          "predict applied to local world B after re-bind");
    check(manager.world("A")->get_block(8, surface + 1, 8) == 3,
          "world A keeps its earlier block");

    // Unknown world refused (all-or-nothing).
    std::string bindError;
    check(!replication->client_bind("missing", bindError) &&
              !bindError.empty(),
          "bind to unknown world refused");
    check(replication->client_world() == "B", "refused bind keeps world B");

    std::printf("[world-repl] client bind: one bound world, re-bind switches "
                "predict/apply target OK\n");
}

// 5. Validation: unknown connection/world never mutate anything.
void test_validation() {
    Harness harness;
    IWorldManager& manager = *harness.manager;
    std::string error;
    WorldSpec a;
    a.name = "A";
    a.seed = 1;
    check(manager.create_world(a, error), "world A created");
    check(boot_world(manager, "A", glm::vec3(8.0f, 200.0f, 8.0f), 2),
          "world A boots");
    const int surface = surface_y(manager, "A", 8, 8);

    auto replication = create_world_replication(manager);

    // Register with an unknown world: refused, nothing registered.
    WorldReplicationInterest bad;
    bad.worldName = "missing";
    bad.interest.position = glm::ivec3(8, surface, 8);
    bad.interest.chunkRadius = 1;
    std::string regError;
    check(!replication->server_register_connection(kConnA, bad, regError) &&
              !regError.empty(),
          "register on unknown world refused");
    check(replication->server_interest(kConnA).worldName.empty(),
          "failed registration leaves no interest");

    WorldReplicationInterest good;
    good.worldName = "A";
    good.interest.position = glm::ivec3(8, surface, 8);
    good.interest.chunkRadius = 1;
    check(replication->server_register_connection(kConnA, good, error),
          "register on world A ok");

    // Unknown connection: submit/pack refused without touching the world.
    const int before = manager.world("A")->get_block(8, surface + 1, 8);
    auto result = replication->server_submit_edit(999, 8, surface + 1, 8, 3);
    check(!result.accepted && !result.error.empty(), "unknown conn edit refused");
    check(manager.world("A")->get_block(8, surface + 1, 8) == before,
          "unknown conn edit did not mutate");
    engine::voxel::RegionReplicationSnapshot region;
    std::string packError;
    check(!replication->server_pack_region(999, region, packError) &&
              !packError.empty(),
          "unknown conn region pack refused");

    // Duplicate registration refused.
    std::string dupError;
    check(!replication->server_register_connection(kConnA, good, dupError) &&
              !dupError.empty(),
          "duplicate registration refused");

    // Unregister: the connection stops receiving; edits to the world no
    // longer reach anyone.
    replication->server_unregister_connection(kConnA);
    check(replication->server_interest(kConnA).worldName.empty(),
          "unregistered connection has no interest");

    std::printf("[world-repl] validation: unknown world/connection and "
                "duplicate registration refused, no mutation OK\n");
}

}  // namespace

int main() {
    test_interest_by_world();
    test_edit_routing();
    test_portal_transition();
    test_client_bind();
    test_validation();
    if (g_failures == 0) {
        std::printf("[world-repl] ALL PASSED\n");
        return 0;
    }
    std::printf("[world-repl] %d FAILURE(S)\n", g_failures);
    return 1;
}
