// ExplosionTests.cpp
//
// Evidence for FALTANTES §16 item 10: integrated explosions combining
// materials, heat, pressure, impulse and terrain. The test drives the PUBLIC
// world surface (IVoxelWorld) + PhysicsRuntime + ExplosionRuntime:
//   - terrain + materials: the blast carves a crater, but a block whose
//     blast resistance beats the local pressure survives in place;
//   - heat: a flammable block outside the blast radius burns away (fireball)
//     while a non-flammable block at the same distance survives;
//   - impulse + density: the pressure wave pushes dynamic bodies; the light
//     one (mass from density) is accelerated far more than the heavy one, and
//     static bodies are skipped;
//   - terrain -> connectivity: the blast's affected voxel box feeds the
//     connectivity layer (item 9), so a pillar carved by the explosion
//     detaches its platform, which becomes a dynamic Jolt body and falls;
//   - determinism: identical world + blast -> bit-identical results.

#include <engine/voxel/IVoxelWorld.hpp>
#include <engine/voxel/IVoxelReplication.hpp>
#include <engine/registry/BlockRegistry.hpp>

#include "engine/gameplay/ExplosionRuntime.hpp"
#include "engine/gameplay/IAbilitySystem.hpp"
#include "engine/physics/PhysicsRuntime.hpp"
#include "engine/physics/PhysicsStreamingBridge.hpp"
#include "engine/physics/VoxelConnectivity.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define VC_EXPLOSION_GETPID _getpid
#else
#include <unistd.h>
#define VC_EXPLOSION_GETPID getpid
#endif

using namespace Engine::Physics;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what);
        ++g_failures;
    }
}

// Deterministic flat world (mirrors the voxel_sdk_tests generator).
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

constexpr int kGroundTop = 130;  // FlatGenerator(130)

// World + registry with the material test blocks for the explosion axes.
struct TestWorld {
    std::shared_ptr<engine::registry::BlockRegistry> registry;
    std::unique_ptr<engine::voxel::IVoxelWorld> world;
    std::uint32_t obsidianId{ 0 };
    std::uint32_t woodId{ 0 };
    bool ok{ false };
};

TestWorld make_test_world() {
    TestWorld out;
    std::string error;
    out.registry = std::make_shared<engine::registry::BlockRegistry>();
    out.ok = out.registry->load_from_json(
        R"({"name":"obsidian","namespace":"test","resistance":100.0,"density":3.0})", error) &&
             out.registry->load_from_json(
        R"({"name":"wood","namespace":"test","resistance":0.0,"flammability":1.0,"density":0.6})", error) &&
             out.registry->load_from_json(
        R"({"name":"glass","namespace":"test","resistance":0.0,"flammability":0.0,"density":2.5})", error);
    out.world = engine::voxel::create_default_voxel_world();
    out.world->set_block_registry(out.registry);
    out.world->register_generator(std::make_shared<FlatGenerator>(kGroundTop));
    out.ok = out.ok &&
             boot_world(*out.world, glm::vec3(8.0f, 200.0f, 8.0f), 2) &&
             out.world->resolve_block_id("test:obsidian", out.obsidianId, error) &&
             out.world->resolve_block_id("test:wood", out.woodId, error);
    return out;
}

template <typename Pred>
bool settle(engine::voxel::IVoxelWorld& world, PhysicsRuntime& physics,
            PhysicsStreamingBridge& bridge, const glm::vec3& focus, Pred predicate,
            int maxMs = 30000) {
    const auto start = std::chrono::steady_clock::now();
    constexpr int kMaxSteps = 60 * 300;
    for (int step = 0; step < kMaxSteps; ++step) {
        if (predicate()) return true;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count() > maxMs) {
            return false;
        }
        world.update(focus, 1.0f / 60.0f);
        bridge.sync(focus);
        physics.step(1.0f / 60.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
}

// 1. Terrain + materials: the blast carves a crater in the stone ground but
//    the reinforced block inside the blast radius survives (resistance beats
//    the local pressure).
void test_terrain_materials() {
    TestWorld t = make_test_world();
    check(t.ok, "test world boots with material blocks");

    // Reinforced column inside the blast radius, stone everywhere else.
    t.world->set_block(8, kGroundTop, 8, t.obsidianId);
    check(t.world->get_block(8, kGroundTop, 8) == t.obsidianId,
          "obsidian column placed at the epicenter");

    PhysicsRuntime physics(WorldSettings{}, PhysicsBackendKind::Jolt);
    Engine::Gameplay::ExplosionConfig config;
    config.blastRadius = 4.0f;
    config.maxPressure = 40.0f;
    config.heatRadius = 0.0f;  // no heat in this scenario
    const Engine::Gameplay::ExplosionResult result = Engine::Gameplay::apply_explosion(
        *t.world, physics, glm::vec3(8.0f, kGroundTop + 0.5f, 8.0f), config);

    check(result.blocksRemoved > 20, "blast carved a crater (many stone cells)");
    check(result.blocksIgnited == 0, "no heat in this scenario");
    check(t.world->get_block(8, kGroundTop, 8) == t.obsidianId,
          "reinforced block survived the blast (resistance >= pressure)");
    check(t.world->get_block(7, kGroundTop, 8) == 0 &&
              t.world->get_block(8, kGroundTop, 7) == 0 &&
              t.world->get_block(10, kGroundTop, 8) == 0,
          "surrounding stone crumbled (resistance 0 < pressure)");
    check(result.affected_any(), "affected voxel box reported for connectivity");
    std::printf("[explosion] terrain x materials: crater + survivor OK\n");
}

// 2. Heat: a flammable block outside the blast radius burns away while a
//    non-flammable block at the same distance survives the fireball.
void test_heat_flammability() {
    TestWorld t = make_test_world();
    check(t.ok, "test world boots with material blocks");

    t.world->set_block(8, kGroundTop, 5, t.woodId);    // d=3 -> burns (heat only)
    t.world->set_block(8, kGroundTop, 13, t.woodId);   // d=5 -> outside heat (control)
    t.world->set_block(8, kGroundTop, 10, t.obsidianId);  // d=2, non-flammable -> survives

    PhysicsRuntime physics(WorldSettings{}, PhysicsBackendKind::Jolt);
    Engine::Gameplay::ExplosionConfig config;
    config.blastRadius = 2.0f;      // heat-only zone at d > 2
    config.heatRadius = 4.0f;
    config.heatDamage = 1.0f;
    config.ignitionThreshold = 1.0f;
    config.maxPressure = 40.0f;
    const Engine::Gameplay::ExplosionResult result = Engine::Gameplay::apply_explosion(
        *t.world, physics, glm::vec3(8.0f, kGroundTop + 0.5f, 8.0f), config);

    check(result.blocksIgnited == 1,
          "exactly the flammable cell outside the blast radius burned");
    check(t.world->get_block(8, kGroundTop, 5) == 0,
          "wood (flammability 1) burned by the fireball");
    check(t.world->get_block(8, kGroundTop, 13) == t.woodId,
          "wood control untouched (outside the heat radius)");
    check(t.world->get_block(8, kGroundTop, 10) == t.obsidianId,
          "non-flammable block survives the heat (flammability 0)");
    std::printf("[explosion] heat x materials: only flammables burn OK\n");
}

// 3. Impulse + density: the pressure wave pushes dynamic bodies; the light
//    one accelerates far more than the heavy one (mass from density), and
//    static bodies are skipped.
void test_impulse_density() {
    TestWorld t = make_test_world();
    check(t.ok, "test world boots");

    PhysicsRuntime physics(WorldSettings{}, PhysicsBackendKind::Jolt);
    PhysicsStreamingBridge bridge(*t.world, physics);
    bridge.sync(glm::vec3(8.0f, 200.0f, 8.0f));

    BodyDesc light;
    light.motion = MotionType::Dynamic;
    light.mass = 1.0f;
    light.position = glm::vec3(8.0f, 133.0f, 8.0f);
    light.collider.shape = BoxShape{ glm::vec3(0.5f) };
    const BodyHandle lightBody = bridge.spawn_dynamic(light);

    BodyDesc heavy = light;
    heavy.mass = 10.0f;
    heavy.position = glm::vec3(16.0f, 133.0f, 8.0f);
    const BodyHandle heavyBody = bridge.spawn_dynamic(heavy);

    BodyDesc still = light;
    still.motion = MotionType::Static;
    still.position = glm::vec3(12.0f, 134.0f, 8.0f);
    const BodyHandle stillBody = bridge.spawn_dynamic(still);

    Engine::Gameplay::ExplosionConfig config;
    config.blastRadius = 6.0f;
    config.maxPressure = 40.0f;
    config.heatRadius = 0.0f;
    config.impulseScale = 2.0f;
    const Engine::Gameplay::ExplosionResult result = Engine::Gameplay::apply_explosion(
        *t.world, physics, glm::vec3(12.0f, kGroundTop + 0.5f, 8.0f), config);

    check(result.bodiesImpulsed == 2,
          "pressure wave pushed the two dynamic bodies only");
    physics.step(1.0f / 60.0f);

    const RigidBody* lightRb = physics.body(lightBody);
    const RigidBody* heavyRb = physics.body(heavyBody);
    const RigidBody* stillRb = physics.body(stillBody);
    if (lightRb && heavyRb && stillRb) {
        const float lightSpeed = std::fabs(lightRb->linearVelocity.x);
        const float heavySpeed = std::fabs(heavyRb->linearVelocity.x);
        check(lightSpeed > 1.0f, "light debris accelerated by the wave");
        check(lightSpeed > heavySpeed * 3.0f,
              "heavy debris moves far less (mass from density scales the response)");
        check(std::fabs(stillRb->linearVelocity.x) < 1.0e-4f &&
                  std::fabs(stillRb->linearVelocity.y) < 1.0e-4f,
              "static body not pushed by the wave");
    } else {
        check(false, "impulse bodies alive after the blast");
    }
    std::printf("[explosion] impulse x density: light flies, heavy crawls, static stays OK\n");
}

// 4. Terrain -> connectivity: the blast carves the support pillar, and the
//    connectivity layer (item 9) turns the detached platform into a dynamic
//    Jolt body that falls and rests.
void test_explosion_connectivity() {
    TestWorld t = make_test_world();
    check(t.ok, "test world boots");

    // Platform (3x1x3, stone) supported by a 1x1 stone pillar to the ground.
    const glm::ivec3 pillarMin{ 12, kGroundTop + 1, 12 };
    const glm::ivec3 pillarMax{ 12, 140, 12 };
    for (int y = pillarMin.y; y <= pillarMax.y; ++y)
        t.world->set_block(pillarMin.x, y, pillarMin.z, 3);
    for (int x = 11; x <= 13; ++x)
        for (int z = 11; z <= 13; ++z)
            t.world->set_block(x, 141, z, 3);

    PhysicsRuntime physics(WorldSettings{}, PhysicsBackendKind::Jolt);
    PhysicsStreamingBridge bridge(*t.world, physics);
    bridge.sync(glm::vec3(8.0f, 200.0f, 8.0f));

    // Blast at the pillar midpoint: carves the WHOLE pillar (d<=5) plus a
    // ground crater, but must leave the platform intact (d=6 > 5.5).
    Engine::Gameplay::ExplosionConfig config;
    config.blastRadius = 5.5f;
    config.maxPressure = 40.0f;
    config.heatRadius = 0.0f;
    const glm::vec3 epicenter(12.5f, 135.5f, 12.5f);
    const Engine::Gameplay::ExplosionResult result =
        Engine::Gameplay::apply_explosion(*t.world, physics, epicenter, config);
    check(result.blocksRemoved > 5, "blast carved the pillar base + ground");
    check(t.world->get_block(12, 141, 12) == 3,
          "platform untouched by the blast (outside blast radius)");

    // Connectivity: the carved region detached the platform island.
    VoxelConnectivity connectivity;
    ConnectivitySettings settings;
    connectivity.note_edit(result.affectedMin, result.affectedMax);
    const auto isSolid = [](std::uint32_t id) { return id != 0u; };
    const std::vector<VoxelIsland> islands =
        connectivity.sync(*t.world, settings, isSolid);
    check(islands.size() == 1, "blast detached exactly one island (the platform)");
    if (!islands.empty()) {
        check(islands[0].minimum == glm::ivec3(11, 141, 11) &&
                  islands[0].maximum == glm::ivec3(13, 141, 13),
              "island is the 3x1x3 platform");
    }
    check(bridge.sync_detached_islands(connectivity, settings, isSolid) == 1,
          "island became a dynamic Jolt body");

    const BodyHandle islandBody = bridge.island_body_at(0);
    bool fell = false;
    settle(*t.world, physics, bridge, glm::vec3(8.0f, 200.0f, 8.0f), [&]() {
        const RigidBody* rb = physics.body(islandBody);
        if (rb == nullptr) return false;
        fell = fell || rb->position.y < 141.0f - 5.0f;
        return rb->position.y < 132.0f;
    });
    check(fell, "platform island fell under gravity after the explosion");
    const RigidBody* rest = physics.body(islandBody);
    if (rest != nullptr) {
        check(std::fabs(rest->position.y - (kGroundTop + 0.5f)) < 1.5f,
              "platform island rested on the streamed terrain");
    }
    std::printf("[explosion] terrain -> connectivity: blast detaches island -> falls OK\n");
}

// 5. Determinism: identical worlds + identical blasts -> bit-identical
//    results (removed/ignited counts and affected box).
void test_determinism() {
    const auto run = []() {
        TestWorld t = make_test_world();
        t.world->set_block(8, kGroundTop, 8, t.obsidianId);
        t.world->set_block(8, kGroundTop, 5, t.woodId);
        PhysicsRuntime physics(WorldSettings{}, PhysicsBackendKind::Jolt);
        Engine::Gameplay::ExplosionConfig config;
        config.blastRadius = 3.0f;
        config.heatRadius = 5.0f;
        config.heatDamage = 1.0f;
        config.maxPressure = 40.0f;
        const Engine::Gameplay::ExplosionResult result = Engine::Gameplay::apply_explosion(
            *t.world, physics, glm::vec3(8.0f, kGroundTop + 0.5f, 8.0f), config);
        return result;
    };

    const Engine::Gameplay::ExplosionResult a = run();
    const Engine::Gameplay::ExplosionResult b = run();
    check(a.blocksRemoved == b.blocksRemoved && a.blocksIgnited == b.blocksIgnited &&
              a.bodiesImpulsed == b.bodiesImpulsed &&
              a.affectedMin == b.affectedMin && a.affectedMax == b.affectedMax,
          "identical explosions -> bit-identical results");
    std::printf("[explosion] determinism OK\n");
}

// 6. Config validation: out-of-range values are refused with a diagnostic.
void test_config_validation() {
    Engine::Gameplay::ExplosionConfig config;
    std::string error;
    check(!config.load_from_json(R"({"blastRadius":0})", error),
          "blastRadius 0 refused");
    check(!config.load_from_json(R"({"blastRadius":100})", error),
          "blastRadius 100 refused");
    check(!config.load_from_json(R"({"heatDamage":-1})", error),
          "negative heatDamage refused");
    check(!config.load_from_json(R"({"ignitionThreshold":0})", error),
          "ignitionThreshold 0 refused");
    check(!config.load_from_json(R"({"pressureDecay":0})", error),
          "pressureDecay 0 refused");
    Engine::Gameplay::ExplosionConfig valid;
    check(valid.load_from_json(
              R"({"blastRadius":5,"maxPressure":60,"heatRadius":7,"impulseScale":3})", error) &&
              valid.blastRadius == 5.0f && valid.maxPressure == 60.0f &&
              valid.heatRadius == 7.0f && valid.impulseScale == 3.0f,
          "valid config loads and round-trips");
    std::printf("[explosion] config validation OK\n");
}

// FALTANTES item 27 sub-2 — expanded-vision proof: an explosion ALTERS the
// terrain, the alteration SAVES, LOADs back in a fresh world, and REPLICATES
// to a fresh client. Composes three already-green systems through their
// public contracts (ExplosionRuntime carve, IVoxelWorld save_world/load_world,
// IVoxelReplication server-negotiated palette + region state-sync) — no
// recompilation, no internal headers.
void test_expanded_vision_explosion_save_replicate() {
    TestWorld t = make_test_world();
    check(t.ok, "proof world boots");

    // A JSON-only block (test:obsidian, resistance 100) at the epicenter
    // survives the blast and must travel to the client by DYNAMIC id via the
    // server-negotiated palette. A stone cell outside the blast is the intact
    // control (id captured, never a magic number).
    t.world->set_block(8, kGroundTop, 8, t.obsidianId);
    // Control at (8, kGroundTop, 2): cell center (8.5, kGroundTop+0.5, 2.5) is
    // ~5.5 units from the epicenter — outside the strict blastRadius 4 carve
    // (the carve measures distance to the CELL CENTER: (8,130,4) sits at
    // d=3.54 and gets carved, so it cannot be the control).
    const std::uint32_t intactBefore = t.world->get_block(8, kGroundTop, 2);
    check(intactBefore != 0, "intact control is terrain (not air)");

    PhysicsRuntime physics(WorldSettings{}, PhysicsBackendKind::Jolt);
    Engine::Gameplay::ExplosionConfig config;
    config.blastRadius = 4.0f;
    config.maxPressure = 40.0f;
    config.heatRadius = 0.0f;
    const Engine::Gameplay::ExplosionResult result = Engine::Gameplay::apply_explosion(
        *t.world, physics, glm::vec3(8.0f, kGroundTop + 0.5f, 8.0f), config);
    check(result.blocksRemoved > 20, "blast carved a crater in the terrain");
    check(t.world->get_block(7, kGroundTop, 8) == 0, "crater cell is air after blast");
    check(t.world->get_block(8, kGroundTop, 8) == t.obsidianId,
          "reinforced survivor kept at epicenter");
    check(t.world->get_block(8, kGroundTop, 2) == intactBefore, "intact control untouched");

    // SAVE: the altered terrain (crater + survivor) is persisted (versioned,
    // checksummed, WAL-backed). Unique-under-temp path so concurrent
    // instances never collide (same pattern as voxel_sdk_tests::scratch_dir).
    const std::string proofDir = (std::filesystem::temp_directory_path() /
        ("vc_explosion_proof_" + std::to_string(VC_EXPLOSION_GETPID()))).string();
    std::error_code ec;
    std::filesystem::remove_all(proofDir, ec);
    std::filesystem::create_directories(proofDir);
    const std::string savePath = proofDir + "/altered.vcwld";
    std::string error;
    check(t.world->save_world(savePath, error), "altered world saves");
    check(error.empty(), "save error empty");

    // LOAD into a FRESH world (same registry attached first; the save's own
    // palette restores the dynamic id by UUID — no recompilation).
    std::unique_ptr<engine::voxel::IVoxelWorld> loaded =
        engine::voxel::create_default_voxel_world();
    loaded->set_block_registry(t.registry);
    loaded->register_generator(std::make_shared<FlatGenerator>(kGroundTop));
    check(boot_world(*loaded, glm::vec3(8.0f, 200.0f, 8.0f), 16),
          "fresh world boots for load");
    check(loaded->load_world(savePath, error), "altered world loads");
    check(error.empty(), "load error empty");
    std::uint32_t loadedObsidian = 0;
    check(loaded->resolve_block_id("test:obsidian", loadedObsidian, error),
          "dynamic block re-resolved after load");
    check(loadedObsidian == t.obsidianId, "same UUID -> same dynamic id after load");
    check(loaded->get_block(7, kGroundTop, 8) == 0, "crater survived save/load");
    check(loaded->get_block(8, kGroundTop, 8) == loadedObsidian,
          "survivor survived save/load");
    check(loaded->get_block(8, kGroundTop, 2) == intactBefore, "intact terrain survived");

    // REPLICATE: the loaded world is the AUTHORITY; a FRESH client (no
    // registry, no knowledge of test:obsidian) receives the server-negotiated
    // palette, then the region snapshot with the crater + survivor and
    // converges on the dynamic id.
    std::unique_ptr<engine::voxel::IVoxelWorld> client =
        engine::voxel::create_default_voxel_world();
    client->register_generator(std::make_shared<FlatGenerator>(kGroundTop));
    check(boot_world(*client, glm::vec3(8.0f, 200.0f, 8.0f), 16),
          "fresh client world boots");
    auto srv = engine::voxel::create_voxel_replication(*loaded);
    auto cli = engine::voxel::create_voxel_replication(*client);
    constexpr engine::voxel::ReplicationConnectionId kProofConn = 1;
    srv->server_register_connection(kProofConn);
    srv->server_set_interest(kProofConn, {{8, kGroundTop, 8}, 2});

    engine::voxel::ReplicationPalette palette;
    check(srv->server_pack_palette(kProofConn, palette, error), "server packs palette");
    check(error.empty(), "palette error empty");
    check(palette.entries.size() == 3, "palette carries the 3 JSON-only blocks");
    check(cli->client_apply_palette(palette, error), "client applies palette");
    check(error.empty(), "client palette error empty");
    std::uint32_t clientObsidian = 0;
    check(client->resolve_block_id("test:obsidian", clientObsidian, error),
          "client resolves dynamic id from palette");
    check(clientObsidian == t.obsidianId, "client id parity with server");

    srv->server_set_snapshot_window(0, kGroundTop + 8);
    srv->server_update();
    engine::voxel::RegionReplicationSnapshot region;
    check(srv->server_pack_region(kProofConn, region, error), "server packs region");
    check(error.empty(), "region error empty");
    check(cli->client_apply_region(region, error), "client applies region");
    check(error.empty(), "client region error empty");
    check(client->get_block(7, kGroundTop, 8) == 0, "crater replicated to client");
    check(client->get_block(8, kGroundTop, 8) == clientObsidian,
          "survivor replicated by dynamic id");
    check(client->get_block(8, kGroundTop, 2) == intactBefore, "intact terrain replicated");

    std::error_code rmEc;
    std::filesystem::remove_all(proofDir, rmEc);
    std::printf("[explosion] proof 27.2: terrain altered -> saved -> loaded -> "
                "replicated OK\n");
}

// ---- Item 27 sub-4 proof seam: IAbilityWorld over the REAL voxel world ----
// BlockEdit writes route through the authoritative public path
// (IVoxelWorld::set_block). Physics/health methods are inert: this scenario
// is terrain-only, so the runtime never exercises them.
class VoxelAbilityWorld final : public engine::gameplay::IAbilityWorld {
public:
    explicit VoxelAbilityWorld(engine::voxel::IVoxelWorld& world) : world_(world) {}

    void add_caster(std::uint32_t id, const glm::vec3& position) {
        casters_[id] = position;
    }

    std::uint32_t block_at(int x, int y, int z) const override {
        return world_.get_block(x, y, z);
    }
    bool set_block(int x, int y, int z, std::uint32_t blockId) override {
        world_.set_block(x, y, z, blockId);  // public surface is void; write issued
        return true;
    }
    bool body_state(const engine::gameplay::AbilityBodyId& body,
                    engine::gameplay::AbilityBodyState& out) const override {
        const auto found = casters_.find(body.id);
        if (found == casters_.end()) return false;
        out.position = found->second;
        out.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        out.linearVelocity = glm::vec3(0.0f);
        out.angularVelocity = glm::vec3(0.0f);
        return true;
    }
    bool apply_impulse(const engine::gameplay::AbilityBodyId&, const glm::vec3&) override { return true; }
    bool add_force(const engine::gameplay::AbilityBodyId&, const glm::vec3&) override { return true; }
    bool set_transform(const engine::gameplay::AbilityBodyId&, const glm::vec3&,
                       const glm::quat&) override { return true; }
    bool raycast(const glm::vec3&, const glm::vec3&, float,
                 engine::gameplay::AbilityRaycastHit&) const override { return false; }
    float attribute(const engine::gameplay::AbilityBodyId&, const std::string&) const override { return 0.0f; }
    engine::gameplay::AbilityTagList tags(
        const engine::gameplay::AbilityBodyId&) const override { return {}; }
    bool spend_cost(const engine::gameplay::AbilityBodyId&, const std::string&, float) override { return true; }
    bool health(const engine::gameplay::AbilityBodyId&, float& out) const override { out = 100.0f; return true; }
    bool damage(const engine::gameplay::AbilityBodyId&, float) override { return true; }
    bool heal(const engine::gameplay::AbilityBodyId&, float) override { return true; }

private:
    engine::voxel::IVoxelWorld& world_;
    std::map<std::uint32_t, glm::vec3> casters_;
};

// FALTANTES item 27 sub-4 — expanded-vision proof: an ABILITY modifies the
// scene. A data-driven definition (the JSON document is the bit-exact
// round-trip contract of the SDK adapter) is cast through the public
// IAbilitySystem onto a REAL voxel world: BlockEdit writes a box of a
// JSON-only block (dynamic id) into the terrain. No recompilation, no
// internal headers; deterministic across identical worlds.
void test_expanded_vision_ability_alters_scene() {
    using namespace engine::gameplay;

    TestWorld t = make_test_world();
    check(t.ok, "proof world boots");

    // Data-driven definition: build -> to_json() -> load_from_json() is the
    // document contract the MCP/CLI author (findings #120).
    AbilityDefinition terraform;
    terraform.name = "terraform";
    terraform.id = "abilities:terraform";
    terraform.targeting.mode = AbilityTargetMode::Point;
    AbilityEffect edit;
    edit.type = AbilityEffectType::BlockEdit;
    edit.min = {-2, 0, -2};
    edit.max = {2, 1, 2};
    edit.blockId = t.obsidianId;  // JSON-only block (dynamic id)
    edit.relative = true;
    terraform.effects.push_back(edit);
    const std::string json = terraform.to_json();
    check(json.find("blockEdit") != std::string::npos,
          "definition document is JSON (data-driven)");

    AbilityDefinition loaded;
    std::string error;
    check(loaded.load_from_json(json, error), "data-driven definition loads");
    check(error.empty(), "definition load error empty");
    auto system = create_ability_system();
    check(system->register_ability(loaded, error), "ability registered from JSON");
    check(error.empty(), "register error empty");

    // Cast at a point above the terrain: the box (relative to the point) is
    // written through the world seam -> the scene is altered.
    VoxelAbilityWorld seam(*t.world);
    AbilityBodyId caster;
    caster.id = 1;
    seam.add_caster(caster.id, glm::vec3(8.0f, kGroundTop + 0.5f, 8.0f));
    AbilityTarget target;
    target.mode = AbilityTargetMode::Point;
    target.point = {8.0f, kGroundTop + 1.0f, 8.0f};
    const CastResult result = system->cast(loaded.id, caster, target, seam);
    check(result.accepted, "terraform cast accepted on the real world");
    check(t.world->get_block(6, kGroundTop + 1, 6) == t.obsidianId,
          "box min corner written (JSON-only block)");
    check(t.world->get_block(10, kGroundTop + 2, 10) == t.obsidianId,
          "box max corner written");
    check(t.world->get_block(8, kGroundTop + 1, 8) == t.obsidianId,
          "origin written");
    check(t.world->get_block(11, kGroundTop + 1, 8) == 0,
          "outside the box untouched");
    check(t.world->get_block(8, kGroundTop, 8) != t.obsidianId,
          "below the box untouched (terrain kept)");

    // Determinism: an identical world + the same cast writes the same box.
    TestWorld t2 = make_test_world();
    check(t2.ok, "second proof world boots");
    VoxelAbilityWorld seam2(*t2.world);
    AbilityBodyId caster2;
    caster2.id = 1;
    seam2.add_caster(caster2.id, glm::vec3(8.0f, kGroundTop + 0.5f, 8.0f));
    const CastResult result2 = system->cast(loaded.id, caster2, target, seam2);
    check(result2.accepted, "second cast accepted");
    check(t2.world->get_block(6, kGroundTop + 1, 6) == t2.obsidianId,
          "deterministic box min");
    check(t2.world->get_block(10, kGroundTop + 2, 10) == t2.obsidianId,
          "deterministic box max");
    check(t2.world->get_block(11, kGroundTop + 1, 8) == 0,
          "deterministic untouched cell");

    std::printf("[explosion] proof 27.4: data-driven ability altered the real "
                "scene (JSON-only block) OK\n");
}

}  // namespace

int main() {
    test_terrain_materials();
    test_heat_flammability();
    test_impulse_density();
    test_explosion_connectivity();
    test_determinism();
    test_config_validation();
    test_expanded_vision_explosion_save_replicate();
    test_expanded_vision_ability_alters_scene();
    if (g_failures == 0) {
        std::printf("[explosion] ALL PASSED\n");
        return 0;
    }
    std::printf("[explosion] %d FAILURE(S)\n", g_failures);
    return 1;
}
