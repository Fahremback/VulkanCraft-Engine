// VoxelStreamingTests.cpp
//
// Regression tests for the voxel streaming pipeline (World::update):
//   1. The MeshReady -> GPU upload -> Uploaded transition actually happens
//      (World::update calls the render bridge's upload_chunk and publishes
//      chunks as Uploaded — previously the pipeline stalled at MeshReady).
//   2. A block edit dirties the chunk and produces a real re-mesh + re-upload.
//   3. Border edits propagate to the neighboring chunk (halo gap-fill).
//   4. Upload-driven neighbor dirtying is gated by revisions: with no further
//      edits the world settles and uploads stop (no remesh ping-pong).
//
// Generation/meshing run on the world's real worker threads, so booting and
// settling poll in wall-clock time (sleep between frames) instead of counting
// synthetic frames. A world that fails to boot within the budget is discarded
// and retried with a fresh one.
#include "World.hpp"
#include "WorldRenderBridge.hpp"
#include "Chunk.hpp"
#include "VoxelMesher.hpp"
#include "Mob.hpp"
#include "SoundEngine.hpp"
#include "Voxel.hpp"

#include <glm/glm.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #cond  \
                      << "\n";                                                 \
        }                                                                      \
    } while (0)

// Minimal bridge: records every upload without touching any GPU.
struct MockBridge final : WorldRenderBridge {
    int beginFrames{ 0 };
    std::vector<ChunkId> uploaded;
    std::unordered_map<uint64_t, int> uploadsPerChunk;

    static uint64_t key(int x, int z) {
        const uint64_t ux = static_cast<uint64_t>(static_cast<int64_t>(x) + 1'000'000);
        const uint64_t uz = static_cast<uint64_t>(static_cast<int64_t>(z) + 1'000'000);
        return (ux << 32) ^ uz;
    }
    static uint64_t key_of(const glm::vec3& p) {
        return key(static_cast<int>(std::floor(p.x / CHUNK_SIZE_X)),
                   static_cast<int>(std::floor(p.z / CHUNK_SIZE_Z)));
    }

    void begin_frame() override { ++beginFrames; }
    void request_far_terrain(int, int, int, float) override {}
    void retire_chunk(ChunkId) override {}
    void upload_chunk(ChunkMeshResult result) override {
        uploaded.push_back(result.chunk);
        ++uploadsPerChunk[key(result.chunk.coord.x, result.chunk.coord.z)];
    }

    int uploads_for(int cx, int cz) const {
        const auto found = uploadsPerChunk.find(key(cx, cz));
        return found == uploadsPerChunk.end() ? 0 : found->second;
    }
};

int total_uploads(const std::unordered_map<uint64_t, int>& counts) {
    int total = 0;
    for (const auto& [unused, count] : counts) total += count;
    return total;
}

struct TestWorld {
    SoundEngine audio;
    MobManager mobs{ audio };
    World world{ mobs, 4 };  // small worker pool: deterministic, no oversubscription
    MockBridge bridge;
    glm::vec3 player{ 8.0f, 40.0f, 8.0f };  // inside chunk (0,0)

    explicit TestWorld(int budget = 16) {
        world.set_chunk_budget(budget);
        // Deterministic runs: no mobs (creeper explosions edit blocks) and no
        // structures, so the only block edits come from the test itself.
        world.mobSpawningEnabled = false;
        world.structureSpawningEnabled = false;
    }

    ~TestWorld() {
        // Drain the worker pool before any member state is destroyed (same
        // guarantee World's destructor provides; explicit here so the mock
        // bridge is also quiesced while the world still exists).
        world.cleanup();
    }

    void run_frames(int frames) {
        for (int i = 0; i < frames; ++i) world.update(player, bridge, 1.0f / 60.0f);
    }

    // Drive update() until the center chunk is Uploaded or the real-time budget
    // runs out. Generation runs on real worker threads, so the poll sleeps
    // between frames to give them wall-clock time; a wedged pool (no boot
    // within the budget) is reported as a stall instead of hanging forever.
    bool boot_world(int maxBudgetMs = 8000) {
        return run_until([this] { return world.is_chunk_loaded_at(player); }, maxBudgetMs);
    }

    // Drive update() with real-time pacing until `predicate` becomes true or
    // the wall-clock budget expires. Meshing jobs run on real worker threads
    // (~ms each), so fast main-thread frame loops would race them; the sleep
    // between frames gives workers wall-clock time to finish and apply.
    template <typename Pred>
    bool run_until(Pred&& predicate, int maxBudgetMs = 8000) {
        const auto start = std::chrono::steady_clock::now();
        while (!predicate()) {
            if (std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start).count() > maxBudgetMs) {
                return false;
            }
            world.update(player, bridge, 1.0f / 60.0f);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    }

    // Real-time settling window: enough wall-clock frames for queued mesh jobs
    // to finish, apply, and any gap-fill chains to re-trigger and re-settle.
    void settle_real_time(int frames, int msPerFrame = 2) {
        for (int i = 0; i < frames; ++i) {
            world.update(player, bridge, 1.0f / 60.0f);
            std::this_thread::sleep_for(std::chrono::milliseconds(msPerFrame));
        }
    }
};

// Runs `scenario` with fresh worlds until the world boots (no generation
// stall) and the scenario passes, or the attempt budget runs out.
template <typename Scenario>
void run_with_retry(const char* name, Scenario&& scenario) {
    for (int attempt = 1; attempt <= 6; ++attempt) {
        TestWorld test;
        if (!test.boot_world()) {
            std::cerr << "[test] " << name << ": generation stall, retry " << attempt << "\n";
            continue;  // fresh world
        }
        // Let it settle (mesh + upload + gap-fill chains) in real time.
        test.run_until([&test] { return test.world.pendingTasks.load() == 0; });
        test.settle_real_time(60);
        const bool ok = scenario(test);
        if (ok) return;
        // The scenario itself failed (not the stall): report and give up.
        std::cerr << "[test] " << name << ": failed after boot\n";
        return;
    }
    std::cerr << "[test] " << name << ": gave up after retries\n";
    ++g_failures;
}

// The MeshReady -> Uploaded transition must actually happen: World::update has
// to hand the CPU mesh to the render bridge and publish the chunk.
bool scenario_pipeline(TestWorld& test) {
    CHECK(test.world.is_chunk_loaded_at(test.player));
    CHECK(!test.bridge.uploaded.empty());
    CHECK(test.bridge.uploads_for(0, 0) > 0);
    std::cout << "[test] pipeline: center Uploaded, " << test.bridge.uploaded.size()
              << " uploads, center chunk uploaded " << test.bridge.uploads_for(0, 0) << "x\n";
    return test.world.is_chunk_loaded_at(test.player) &&
           !test.bridge.uploaded.empty() && test.bridge.uploads_for(0, 0) > 0;
}

// A block edit must dirty the chunk, re-mesh it and re-upload it (the full
// Uploaded -> dirty -> MeshReady -> Uploaded cycle).
bool scenario_edit(TestWorld& test) {
    CHECK(test.world.is_chunk_loaded_at(test.player));
    const int before = test.bridge.uploads_for(0, 0);
    if (before <= 0) return false;

    // Replace the first solid surface block above the chunk center.
    glm::vec3 edit{ 8.0f, 0.0f, 8.0f };
    for (int y = 150; y >= 40; --y) {
        edit.y = static_cast<float>(y);
        if (test.world.get_block_at(edit) != kRuntimeAirId) break;
    }
    CHECK(test.world.get_block_at(edit) != kRuntimeAirId);
    test.world.set_block_at(edit, runtime_id(BlockType::Stone));
    CHECK(test.world.get_block_at(edit) == runtime_id(BlockType::Stone));

    const bool uploaded = test.run_until(
        [&test, before] { return test.bridge.uploads_for(0, 0) > before; });
    const int after = test.bridge.uploads_for(0, 0);
    CHECK(uploaded);
    CHECK(after > before);
    std::cout << "[test] edit: surface block -> Stone at y=" << edit.y
              << "; center uploads " << before << " -> " << after << "\n";
    return uploaded && after > before;
}

// Editing a block at a chunk border must dirty the neighboring chunk too and
// re-upload it (halo border propagation across the chunk seam).
bool scenario_border(TestWorld& test) {
    const glm::vec3 neighborPoint{ -8.0f, 40.0f, 8.0f };  // chunk (-1,0)
    CHECK(test.run_until([&test, &neighborPoint] {
        return test.world.is_chunk_loaded_at(neighborPoint);
    }));

    const int centerBefore = test.bridge.uploads_for(0, 0);
    const int neighborBefore = test.bridge.uploads_for(-1, 0);
    if (centerBefore <= 0 || neighborBefore <= 0) return false;

    // World x=0.5 lands on local x=0 of chunk (0,0) — a border column.
    glm::vec3 border{ 0.5f, 0.0f, 8.0f };
    for (int y = 150; y >= 40; --y) {
        border.y = static_cast<float>(y);
        if (test.world.get_block_at(border) != kRuntimeAirId) break;
    }
    test.world.set_block_at(border, runtime_id(BlockType::Stone));

    const bool both = test.run_until([&test, centerBefore, neighborBefore] {
        return test.bridge.uploads_for(0, 0) > centerBefore &&
               test.bridge.uploads_for(-1, 0) > neighborBefore;
    });
    CHECK(both);
    CHECK(test.bridge.uploads_for(0, 0) > centerBefore);
    CHECK(test.bridge.uploads_for(-1, 0) > neighborBefore);
    std::cout << "[test] border: center " << centerBefore << " -> "
              << test.bridge.uploads_for(0, 0) << ", neighbor(-1,0) "
              << neighborBefore << " -> " << test.bridge.uploads_for(-1, 0) << "\n";
    return both;
}

// A registry-defined (JSON-only) block meshes with its data-driven color: the
// snapshot carries the dynamic runtime table and the mesher resolves ids >=
// BlockType::Count through it (color-only, no engine texture layer).
// Data-driven fluid damage (META section 13): a fluid with damagePerTick in
// the world's table hurts entities inside it. The cow sinks through the lava
// (fluids are never ground) and takes 4 damage/s until the check fires.
bool scenario_fluid_damage(TestWorld& test) {
    CHECK(test.world.is_chunk_loaded_at(test.player));

    // Lava as a real fluid with damage, no falling (the pool stays put).
    std::unordered_map<RuntimeBlockId, FluidParams> fluids;
    FluidParams lava;
    lava.viscosity = 1.0f;
    lava.density = 2.0f;
    lava.maxLevel = 1;
    lava.levelsPerTick = 1;
    lava.source = true;
    lava.falling = false;
    lava.evaporation = false;
    lava.damagePerTick = 4.0f;
    fluids[runtime_id(BlockType::Lava)] = lava;
    test.world.set_fluid_table(std::move(fluids));

    // Carve a basin over the whole chunk area (0..15 x 0..15): stone floor,
    // 3-tall lava lake, and everything above cleared to air. The terrain is
    // uneven (the surface scan below found a single column), so carving the
    // full volume guarantees the cow falls INTO lava instead of landing on a
    // ledge above a buried pool.
    glm::vec3 surface{ 8.0f, 0.0f, 8.0f };
    for (int y = 150; y >= 40; --y) {
        surface.y = static_cast<float>(y);
        if (test.world.get_block_at(surface) != kRuntimeAirId) break;
    }
    CHECK(test.world.get_block_at(surface) != kRuntimeAirId);
    const int platformY = static_cast<int>(surface.y);
    for (int x = 0; x < CHUNK_SIZE_X; ++x) {
        for (int z = 0; z < CHUNK_SIZE_Z; ++z) {
            test.world.set_block_at(glm::vec3(x, platformY, z), runtime_id(BlockType::Stone));
            for (int dy = 1; dy <= 3; ++dy) {
                test.world.set_block_at(glm::vec3(x, platformY + dy, z),
                                        runtime_id(BlockType::Lava));
            }
            for (int dy = 4; dy <= 15; ++dy) {
                test.world.set_block_at(glm::vec3(x, platformY + dy, z), kRuntimeAirId);
            }
        }
    }

    // The cow falls into the lake, sinks through the lava (fluids are not
    // ground) and rests with its feet inside it: damage accumulates until the
    // check fires (4/s -> 1.5 HP lost in ~0.4s of contact).
    test.mobs.spawn_mob(MobType::Cow, glm::vec3(8.5f, platformY + 5.5f, 8.5f));
    const bool damaged = test.run_until([&test] {
        return test.mobs.mobs.size() == 1 &&
               test.mobs.mobs[0].health <= 10.0f - 1.5f;
    }, 10000);
    CHECK(damaged);
    CHECK(test.mobs.mobs.size() == 1);
    CHECK(test.mobs.mobs[0].health < 10.0f);
    std::cout << "[test] fluid damage: cow in lava lost "
              << (10.0f - test.mobs.mobs[0].health) << " HP (4/s)\n";
    return damaged;
}

bool scenario_dynamic_block_meshes() {
    const RuntimeBlockId dynamicId = static_cast<RuntimeBlockId>(BlockType::Count) + 7;

    ChunkSnapshot snapshot;
    snapshot.id = { { 0, 0 }, 1 };
    snapshot.revision = 1;
    snapshot.verticalExtent = 1;
    snapshot.layers = { 130 };
    auto& layer = snapshot.center[130];
    layer.blocks.fill(kRuntimeAirId);
    layer.blocks[8 * CHUNK_SIZE_X + 8] = dynamicId;

    RuntimeBlockInfo info;
    info.uuid = "00000000-0000-0000-0000-0000000000ab";
    info.color = glm::vec4(0.9f, 0.1f, 0.1f, 1.0f);
    info.solid = true;
    snapshot.runtimeBlocks.emplace_back(dynamicId, info);

    const ChunkMeshResult result = VoxelMesher::build(snapshot);
    CHECK(result.valid);
    CHECK(!result.mesh.meshVertices.empty());
    bool allDynamicColor = true;
    for (const VoxelVertex& vertex : result.mesh.meshVertices) {
        if (std::abs(vertex.color.r - 0.9f) > 1e-3f ||
            std::abs(vertex.color.g - 0.1f) > 1e-3f) allDynamicColor = false;
        if (vertex.uv.z != -1.0f) allDynamicColor = false;  // color-only
    }
    CHECK(allDynamicColor);

    // Contrast: a builtin block still resolves through the engine material
    // table (textured layer), not the runtime table.
    ChunkSnapshot builtin;
    builtin.id = { { 0, 0 }, 2 };
    builtin.revision = 2;
    builtin.verticalExtent = 1;
    builtin.layers = { 130 };
    auto& bLayer = builtin.center[130];
    bLayer.blocks.fill(kRuntimeAirId);
    bLayer.blocks[8 * CHUNK_SIZE_X + 8] = runtime_id(BlockType::Stone);
    const ChunkMeshResult stoneResult = VoxelMesher::build(builtin);
    CHECK(stoneResult.valid);
    CHECK(!stoneResult.mesh.meshVertices.empty());
    CHECK(stoneResult.mesh.meshVertices.front().uv.z >= 0.0f);

    std::cout << "[test] dynamic block meshes with data-driven color ("
              << result.mesh.meshVertices.size() << " vertices)\n";
    return g_failures == 0;
}

// Without further edits the world must settle: the upload-driven neighbor
// dirtying is gated by chunk revisions, so there is no remesh ping-pong.
bool scenario_no_pingpong(TestWorld& test) {
    CHECK(test.world.is_chunk_loaded_at(test.player));
    // Drain every gap-fill chain in real time (workers + apply + re-trigger).
    test.run_until([&test] { return test.world.pendingTasks.load() == 0; });
    test.settle_real_time(120);

    const int settled = total_uploads(test.bridge.uploadsPerChunk);
    test.settle_real_time(150);
    const int extra = total_uploads(test.bridge.uploadsPerChunk) - settled;
    CHECK(extra == 0);
    std::cout << "[test] idle: " << settled << " uploads at settle, "
              << extra << " extra over 300ms idle (expect 0)\n";
    return extra == 0;
}

} // namespace

int main() {
    run_with_retry("pipeline->uploaded", scenario_pipeline);
    run_with_retry("edit->remesh->upload", scenario_edit);
    run_with_retry("border propagation", scenario_border);
    run_with_retry("no ping-pong", scenario_no_pingpong);
    run_with_retry("fluid damage", scenario_fluid_damage);
    // Mesher-level check (no world needed): dynamic ids resolve through the
    // snapshot's runtime table.
    scenario_dynamic_block_meshes();

    if (g_failures != 0) {
        std::cerr << "[voxel_streaming_tests] " << g_failures << " failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "[voxel_streaming_tests] all checks passed\n";
    return EXIT_SUCCESS;
}
