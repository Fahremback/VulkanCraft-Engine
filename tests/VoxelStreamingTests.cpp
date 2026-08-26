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
        lastMesh[key(result.chunk.coord.x, result.chunk.coord.z)] = std::move(result);
    }

    // Latest mesh per chunk (additive: existing members unchanged). Lets
    // scenarios assert vertex content produced through the REAL dispatch path
    // (world -> snapshot -> mesher), e.g. block-light shading.
    std::unordered_map<uint64_t, ChunkMeshResult> lastMesh;

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
    World world{ 4 };  // small worker pool: deterministic, no oversubscription
    MockBridge bridge;
    glm::vec3 player{ 8.0f, 40.0f, 8.0f };  // inside chunk (0,0)

    explicit TestWorld(int budget = 16) {
        world.set_chunk_budget(budget);
        // Deterministic runs: no structures, so the only block edits come
        // from the test itself. (Mob spawning moved to the entity layer —
        // FALTANTES item 11; the cow-in-lava equivalence lives in
        // voxel_sdk_tests as test_mob_behavior.)
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
// (The data-driven fluid-damage scenario moved to voxel_sdk_tests as
// test_mob_behavior — mobs are IEntityWorld entities now, FALTANTES item 11.)
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

// FALTANTES item 8/9: the mesher shades vertex colors by the captured block
// light (0..15). A snapshot with hasLightData scales RGB by light/15 (alpha
// untouched); a snapshot WITHOUT light data keeps the legacy full-bright
// material color.
bool scenario_mesher_consumes_light() {
    const RuntimeBlockId stoneId = runtime_id(BlockType::Stone);

    auto make_snapshot = [&](bool hasLight, uint8_t lightLevel, uint32_t id) {
        ChunkSnapshot snapshot;
        snapshot.id = { { 0, 0 }, id };
        snapshot.revision = 1;
        snapshot.verticalExtent = 1;
        snapshot.layers = { 130 };
        snapshot.hasLightData = hasLight;
        auto& layer = snapshot.center[130];
        layer.blocks.fill(kRuntimeAirId);
        layer.blocks[8 * CHUNK_SIZE_X + 8] = stoneId;
        if (hasLight) layer.light.fill(lightLevel);
        return snapshot;
    };

    const ChunkMeshResult full = VoxelMesher::build(make_snapshot(true, 15, 1));
    const ChunkMeshResult half = VoxelMesher::build(make_snapshot(true, 8, 2));
    const ChunkMeshResult dark = VoxelMesher::build(make_snapshot(true, 0, 3));
    // No light capture: legacy full-bright material color (brightness 1.0).
    const ChunkMeshResult unlit = VoxelMesher::build(make_snapshot(false, 0, 4));
    CHECK(full.valid);
    CHECK(half.valid);
    CHECK(dark.valid);
    CHECK(unlit.valid);
    CHECK(!full.mesh.meshVertices.empty());
    CHECK(!half.mesh.meshVertices.empty());
    CHECK(!dark.mesh.meshVertices.empty());
    CHECK(!unlit.mesh.meshVertices.empty());

    const glm::vec4 fullColor = full.mesh.meshVertices.front().color;
    const glm::vec4 halfColor = half.mesh.meshVertices.front().color;
    const glm::vec4 darkColor = dark.mesh.meshVertices.front().color;
    const glm::vec4 baseColor = unlit.mesh.meshVertices.front().color;

    // light=15 == the unlit material color exactly (and alpha untouched).
    for (int c = 0; c < 3; ++c) {
        CHECK(std::abs(fullColor[c] - baseColor[c]) < 1e-3f);
        CHECK(std::abs(halfColor[c] - baseColor[c] * (8.0f / 15.0f)) < 1e-3f);
        CHECK(std::abs(darkColor[c]) < 1e-3f);
    }
    CHECK(fullColor.a == baseColor.a);
    CHECK(halfColor.a == baseColor.a);
    CHECK(darkColor.a == baseColor.a);
    // Ordering: full > half > dark.
    CHECK(fullColor.r > halfColor.r);
    CHECK(halfColor.r > darkColor.r);

    // Sky fallback for cells with no occupied layer (open air above the
    // surface): the accessor uses the column's occlusion height.
    const ChunkSnapshot& skySnapshot = make_snapshot(true, 0, 5);
    CHECK(skySnapshot.light(8, 131, 8) == 0);  // layer 131 missing, no sky data
    auto skyVisible = skySnapshot;
    skyVisible.skyOcclusionTop[8 * CHUNK_SIZE_X + 8] = 130;
    CHECK(skyVisible.light(8, 131, 8) == 15);  // above the occluding column
    auto skyBlocked = skyVisible;
    skyBlocked.skyOcclusionTop[8 * CHUNK_SIZE_X + 8] = 255;
    CHECK(skyBlocked.light(8, 131, 8) == 0);  // column occluded above the cell

    std::cout << "[test] mesher shades vertex colors by block light ("
              << fullColor.r << " > " << halfColor.r << " > " << darkColor.r
              << ", alpha preserved, no-light = full bright)\n";
    return g_failures == 0;
}

// FALTANTES item 8/9, real dispatch path: a Glowstone emitter (builtin
// emission 15) inside an underground air tunnel lights the tunnel; the
// snapshot captured at meshing carries the light, and the uploaded mesh
// shades a stone block facing the emitter at full brightness while a stone
// block at the far end of the tunnel is notably darker (light decay).
bool scenario_mesher_light_capture() {
    TestWorld test;
    CHECK(test.boot_world());

    // Underground (below the terrain occlusion line, so sky light = 0): an
    // air tunnel along +x inside chunk (0,0). Glowstone emits 15 by default
    // (builtin light table); stone probes border the tunnel.
    const RuntimeBlockId airId = kRuntimeAirId;
    const RuntimeBlockId stoneId = runtime_id(BlockType::Stone);
    const RuntimeBlockId emitterId = runtime_id(BlockType::Glowstone);
    const int y = 40;
    const glm::vec3 emitter(8.0f, static_cast<float>(y), 8.0f);
    const glm::vec3 nearBlock(9.0f, static_cast<float>(y), 8.0f);
    const glm::vec3 farBlock(0.0f, static_cast<float>(y), 8.0f);
    for (int x = 2; x <= 7; ++x) {
        test.world.set_block_at(
            glm::vec3(static_cast<float>(x), static_cast<float>(y), 8.0f), airId);
    }
    test.world.set_block_at(emitter, emitterId);
    test.world.set_block_at(nearBlock, stoneId);
    test.world.set_block_at(farBlock, stoneId);
    CHECK(test.world.get_block_at(emitter) == emitterId);

    // Wait until the light field settles: the emitter cell is 15, the far
    // tunnel air cell (1,y,8) has decayed to 8 (7 steps through air).
    const glm::vec3 farAir(1.0f, static_cast<float>(y), 8.0f);
    const bool lit = test.run_until([&] {
        const uint8_t far = test.world.get_block_light(farAir);
        return test.world.get_block_light(emitter) == 15 && far >= 8 && far <= 10;
    });
    std::cout << "[dbg] light emitter="
              << static_cast<int>(test.world.get_block_light(emitter))
              << " farAir=" << static_cast<int>(test.world.get_block_light(farAir))
              << " lit=" << lit << "\n";
    CHECK(lit);

    // Meshing runs one frame BEFORE the light pass in update(), so the last
    // uploaded mesh may predate the settled light. Force one more remesh now
    // that the light is final, then wait for a newer mesh revision.
    const auto key = MockBridge::key_of(emitter);
    const uint64_t before = test.bridge.lastMesh.count(key)
        ? test.bridge.lastMesh.at(key).sourceRevision : 0;
    test.world.set_block_at(farBlock, stoneId);  // same value: bump + remesh
    const bool remeshed = test.run_until([&] {
        const auto found = test.bridge.lastMesh.find(key);
        return found != test.bridge.lastMesh.end() &&
               found->second.sourceRevision > before;
    });
    CHECK(remeshed);

    const ChunkMeshResult& mesh = test.bridge.lastMesh.at(key);
    CHECK(mesh.valid);
    const auto face_color_at = [&](const glm::vec3& blockPos) {
        // Average the vertices of the ONE drawn face (the one looking into
        // the tunnel); world positions, block occupies [blockPos, +1).
        float r = 0.0f, g = 0.0f, b = 0.0f;
        int n = 0;
        for (const VoxelVertex& v : mesh.mesh.meshVertices) {
            if (v.position.x >= blockPos.x && v.position.x <= blockPos.x + 1.0f &&
                v.position.y >= blockPos.y && v.position.y <= blockPos.y + 1.0f &&
                v.position.z >= blockPos.z && v.position.z <= blockPos.z + 1.0f) {
                r += v.color.r; g += v.color.g; b += v.color.b; ++n;
            }
        }
        return std::make_tuple(n, r, g, b);
    };
    const auto [nNear, rN, gN, bN] = face_color_at(nearBlock);
    const auto [nFar, rF, gF, bF] = face_color_at(farBlock);
    CHECK(nNear > 0);
    CHECK(nFar > 0);
    // near faces the emitter cell (light 15); far faces tunnel air at ~8
    // (8/15 = 0.53, so far < 0.7 * near is a safe margin).
    CHECK(rF < rN * 0.7f);
    CHECK(gF < gN * 0.7f);
    CHECK(bF < bN * 0.7f);

    std::cout << "[test] real dispatch: near-emitter stone lit (" << rN
              << ',' << gN << ',' << bN << ") vs far stone dark (" << rF
              << ',' << gF << ',' << bF << ") OK\n";
    return g_failures == 0;
}

// Per-face materials + occlusion (FALTANTES §14): a dynamic block with
// faceTop/faceSide overrides meshes each face with its own color, unset
// faces fall back to the base color; occlusion=false draws the shared face
// against an opaque neighbor while two opaque blocks skip it.
bool scenario_face_materials_and_occlusion() {
    const RuntimeBlockId dynamicId = static_cast<RuntimeBlockId>(BlockType::Count) + 8;

    ChunkSnapshot snapshot;
    snapshot.id = { { 0, 0 }, 1 };
    snapshot.revision = 1;
    snapshot.verticalExtent = 1;
    snapshot.layers = { 130 };
    auto& layer = snapshot.center[130];
    layer.blocks.fill(kRuntimeAirId);
    layer.blocks[8 * CHUNK_SIZE_X + 8] = dynamicId;

    RuntimeBlockInfo info;
    info.uuid = "00000000-0000-0000-0000-0000000000ac";
    info.color = glm::vec4(0.9f, 0.1f, 0.1f, 1.0f);
    info.faceTop = glm::vec4(0.2f, 0.8f, 0.2f, 1.0f);
    info.faceSide = glm::vec4(0.5f, 0.35f, 0.2f, 1.0f);
    info.faceTopSet = true;
    info.faceSideSet = true;
    info.solid = true;
    snapshot.runtimeBlocks.emplace_back(dynamicId, info);

    const ChunkMeshResult result = VoxelMesher::build(snapshot);
    CHECK(result.valid);
    int topCount = 0, sideCount = 0, bottomCount = 0;
    for (const VoxelVertex& vertex : result.mesh.meshVertices) {
        if (vertex.normal.y > 0.5f) {
            ++topCount;
            CHECK(std::abs(vertex.color.g - 0.8f) < 1e-3f);  // faceTop
            CHECK(std::abs(vertex.color.r - 0.2f) < 1e-3f);
        } else if (vertex.normal.y < -0.5f) {
            ++bottomCount;
            CHECK(std::abs(vertex.color.r - 0.9f) < 1e-3f);  // base color (unset)
            CHECK(std::abs(vertex.color.g - 0.1f) < 1e-3f);
        } else {
            ++sideCount;
            CHECK(std::abs(vertex.color.r - 0.5f) < 1e-3f);  // faceSide
            CHECK(std::abs(vertex.color.g - 0.35f) < 1e-3f);
        }
    }
    CHECK(topCount == 6 && sideCount == 24 && bottomCount == 6);

    // Occlusion: two adjacent opaque dynamic blocks skip the shared face (10
    // faces = 60 vertices); with occlusion=false on one side both draw (12
    // faces = 72 vertices).
    const RuntimeBlockId aId = static_cast<RuntimeBlockId>(BlockType::Count) + 9;
    const RuntimeBlockId bId = static_cast<RuntimeBlockId>(BlockType::Count) + 10;
    auto make_pair = [&](bool aOccludes, bool bOccludes) {
        ChunkSnapshot pair;
        pair.id = { { 0, 0 }, 1 };
        pair.revision = 1;
        pair.verticalExtent = 1;
        pair.layers = { 130 };
        auto& pl = pair.center[130];
        pl.blocks.fill(kRuntimeAirId);
        pl.blocks[8 * CHUNK_SIZE_X + 8] = aId;
        pl.blocks[9 * CHUNK_SIZE_X + 8] = bId;
        RuntimeBlockInfo ai;
        ai.uuid = "00000000-0000-0000-0000-0000000000ad";
        ai.color = glm::vec4(0.3f, 0.3f, 0.3f, 1.0f);
        ai.solid = true;
        ai.occludes = aOccludes;
        RuntimeBlockInfo bi = ai;
        bi.uuid = "00000000-0000-0000-0000-0000000000ae";
        bi.occludes = bOccludes;
        pair.runtimeBlocks.emplace_back(aId, ai);
        pair.runtimeBlocks.emplace_back(bId, bi);
        return VoxelMesher::build(pair);
    };
    const ChunkMeshResult opaquePair = make_pair(true, true);
    CHECK(opaquePair.valid);
    int opaqueZ = 0;
    for (const VoxelVertex& vertex : opaquePair.mesh.meshVertices) {
        if (std::abs(vertex.normal.z) > 0.5f) ++opaqueZ;
    }
    CHECK(opaqueZ == 12);  // 2 outer north/south faces, shared face skipped
    const ChunkMeshResult openPair = make_pair(false, true);
    CHECK(openPair.valid);
    int openZ = 0;
    for (const VoxelVertex& vertex : openPair.mesh.meshVertices) {
        if (std::abs(vertex.normal.z) > 0.5f) ++openZ;
    }
    CHECK(openZ == 24);  // shared faces drawn on both sides (occlusion off)

    std::cout << "[test] per-face materials + occlusion (top=" << topCount
              << " side=" << sideCount << " bottom=" << bottomCount
              << ", opaquePairZ=" << opaqueZ << " openPairZ=" << openZ << ")\n";
    return g_failures == 0;
}

// FALTANTES §8 item 167: um fluido de PROJETO (id dinâmico, classe fluid)
// mesha como FLUIDO — mesh de água com altura por nível (waterMeshVertices),
// não um cubo sólido opaco. O mesher roteia pelo flag `fluid` do
// RuntimeBlockInfo (não pelo enum builtin Water).
bool scenario_dynamic_fluid_meshes_as_fluid() {
    const RuntimeBlockId dynamicFluid = static_cast<RuntimeBlockId>(BlockType::Count) + 20;

    ChunkSnapshot snapshot;
    snapshot.id = { { 0, 0 }, 1 };
    snapshot.revision = 1;
    snapshot.verticalExtent = 1;
    snapshot.layers = { 130 };
    auto& layer = snapshot.center[130];
    layer.blocks.fill(kRuntimeAirId);
    layer.blocks[8 * CHUNK_SIZE_X + 8] = dynamicFluid;
    layer.water[8 * CHUNK_SIZE_X + 8] = 2;  // level 2: thinning fluid

    RuntimeBlockInfo info;
    info.uuid = "00000000-0000-0000-0000-0000000000b0";
    info.color = glm::vec4(0.3f, 1.0f, 0.2f, 0.7f);
    info.solid = false;
    info.fluid = true;  // project fluid (block class "fluid")
    snapshot.runtimeBlocks.emplace_back(dynamicFluid, info);

    const ChunkMeshResult result = VoxelMesher::build(snapshot);
    CHECK(result.valid);
    // O cubo sólido fica VAZIO: a célula de fluido só emite na mesh de água.
    CHECK(result.mesh.meshVertices.empty());
    CHECK(!result.mesh.waterMeshVertices.empty());
    // A face de topo segue a altura por nível (topo elevado, não cubo cheio).
    bool sawElevatedTop = false;
    for (const VoxelVertex& vertex : result.mesh.waterMeshVertices) {
        if (vertex.normal.y > 0.5f && vertex.position.y > 0.1f) sawElevatedTop = true;
    }
    CHECK(sawElevatedTop);

    // Contraste: o MESMO bloco sem o flag fluid mesha como sólido opaco.
    ChunkSnapshot solid;
    solid.id = { { 0, 0 }, 2 };
    solid.revision = 2;
    solid.verticalExtent = 1;
    solid.layers = { 130 };
    auto& solidLayer = solid.center[130];
    solidLayer.blocks.fill(kRuntimeAirId);
    solidLayer.blocks[8 * CHUNK_SIZE_X + 8] = dynamicFluid;
    RuntimeBlockInfo solidInfo = info;
    solidInfo.fluid = false;
    solid.runtimeBlocks.emplace_back(dynamicFluid, solidInfo);
    const ChunkMeshResult solidResult = VoxelMesher::build(solid);
    CHECK(solidResult.valid);
    CHECK(!solidResult.mesh.meshVertices.empty());
    CHECK(solidResult.mesh.waterMeshVertices.empty());

    std::cout << "[test] project-defined fluid meshes as fluid (water mesh by "
                 "level, not a solid cube) OK\n";
    return g_failures == 0;
}

// State-aware materials (FALTANTES item 5): a dynamic block carrying named
// states meshes with its DEFAULT state (states[0]) through the ordinary path,
// and VoxelMesher::resolve_state_material addresses every state directly
// (index 0 = states[0], k = states[k], out-of-range clamps to default).
bool scenario_state_materials() {
    const RuntimeBlockId dynamicId = static_cast<RuntimeBlockId>(BlockType::Count) + 11;

    ChunkSnapshot snapshot;
    snapshot.id = { { 0, 0 }, 1 };
    snapshot.revision = 1;
    snapshot.verticalExtent = 1;
    snapshot.layers = { 130 };
    auto& layer = snapshot.center[130];
    layer.blocks.fill(kRuntimeAirId);
    layer.blocks[8 * CHUNK_SIZE_X + 8] = dynamicId;

    RuntimeBlockInfo info;
    info.uuid = "00000000-0000-0000-0000-0000000000af";
    info.color = glm::vec4(0.1f, 0.1f, 0.1f, 1.0f);
    info.faceSide = glm::vec4(0.2f, 0.2f, 0.2f, 1.0f);
    info.faceSideSet = true;
    info.solid = true;
    RuntimeBlockInfo::RuntimeBlockState lit;
    lit.name = "lit";
    lit.color = glm::vec4(1.0f, 0.6f, 0.1f, 1.0f);
    lit.faceTop = glm::vec4(0.9f, 0.9f, 0.9f, 1.0f);
    lit.faceTopSet = true;
    RuntimeBlockInfo::RuntimeBlockState dim;
    dim.name = "dim";
    dim.color = glm::vec4(0.3f, 0.2f, 0.1f, 1.0f);
    info.states = { lit, dim };
    snapshot.runtimeBlocks.emplace_back(dynamicId, info);

    // Default state (states[0]) drives the ordinary mesh: top faces use the
    // lit state's override, sides fall back to its base color.
    const ChunkMeshResult result = VoxelMesher::build(snapshot);
    CHECK(result.valid);
    int topWithOverride = 0;
    int sideWithBase = 0;
    for (const VoxelVertex& vertex : result.mesh.meshVertices) {
        if (vertex.normal.y > 0.5f) {
            ++topWithOverride;
            CHECK(vertex.color == lit.faceTop);
        } else if (std::abs(vertex.normal.y) < 0.5f) {
            ++sideWithBase;
            CHECK(vertex.color == lit.color);
        }
    }
    CHECK(topWithOverride == 6);
    CHECK(sideWithBase == 24);

    // Direct state addressing: 0 = states[0] (default), k = states[k],
    // out-of-range clamps to default; without states, base per-face material.
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const glm::vec3 side(1.0f, 0.0f, 0.0f);
    CHECK(VoxelMesher::resolve_state_material(info, 0, up) == lit.faceTop);
    CHECK(VoxelMesher::resolve_state_material(info, 0, side) == lit.color);
    CHECK(VoxelMesher::resolve_state_material(info, 1, up) == dim.color);
    CHECK(VoxelMesher::resolve_state_material(info, 1, side) == dim.color);
    CHECK(VoxelMesher::resolve_state_material(info, 99, up) == lit.faceTop);
    CHECK(VoxelMesher::resolve_state_material(info, -1, up) == lit.faceTop);
    RuntimeBlockInfo plain;
    plain.color = glm::vec4(0.9f, 0.1f, 0.1f, 1.0f);
    plain.faceSide = glm::vec4(0.5f, 0.35f, 0.2f, 1.0f);
    plain.faceSideSet = true;
    CHECK(VoxelMesher::resolve_state_material(plain, 0, up) == plain.color);
    CHECK(VoxelMesher::resolve_state_material(plain, 0, side) == plain.faceSide);
    CHECK(VoxelMesher::resolve_state_material(plain, 7, up) == plain.color);

    std::cout << "[test] state materials: default mesh = states[0], index "
                 "addressing + clamping OK\n";
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
    // Mesher-level checks (no world needed): dynamic ids resolve through the
    // snapshot's runtime table; per-face materials and occlusion follow §14.
    scenario_dynamic_block_meshes();
    scenario_mesher_consumes_light();
    scenario_mesher_light_capture();
    scenario_face_materials_and_occlusion();
    scenario_dynamic_fluid_meshes_as_fluid();
    scenario_state_materials();

    if (g_failures != 0) {
        std::cerr << "[voxel_streaming_tests] " << g_failures << " failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "[voxel_streaming_tests] all checks passed\n";
    return EXIT_SUCCESS;
}
