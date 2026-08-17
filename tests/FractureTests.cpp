// FractureTests.cpp
//
// Evidence for FALTANTES §16 item 8 — configurable fracture for voxels
// (Blast/FEMFX are mesh-fracture middleware NOT vendored; the voxel path is
// the one applicable here). The data-driven FractureConfig turns a solid
// voxel region into a destructible whose chunks detach as dynamic debris on
// damage, with mass from the block density (item 7 chain). The kinematic ->
// dynamic flip goes through the runtime so the debris ACTUALLY FALLS under
// Jolt (the standard world) — the mirror-only flip silently left chunks
// floating in place.

#include "engine/gameplay/DestructionRuntime.hpp"
#include "engine/gameplay/FractureConfig.hpp"
#include "engine/physics/PhysicsRuntime.hpp"

#include <glm/glm.hpp>

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using namespace Engine;
using namespace Engine::Gameplay;
using namespace Engine::Physics;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what);
        ++g_failures;
    }
}

// Fills a solid x*y*z region with one block id.
std::vector<VoxelCell> solid_region(int width, int height, int depth,
                                    std::uint32_t blockId) {
    std::vector<VoxelCell> cells;
    cells.reserve(static_cast<std::size_t>(width * height * depth));
    for (int y = 0; y < height; ++y)
        for (int z = 0; z < depth; ++z)
            for (int x = 0; x < width; ++x)
                cells.push_back({ glm::ivec3(x, y, z), blockId });
    return cells;
}

bool same_chunks(const std::vector<DestructionChunkDesc>& a,
                 const std::vector<DestructionChunkDesc>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const DestructionChunkDesc& ca = a[i];
        const DestructionChunkDesc& cb = b[i];
        if (glm::any(glm::notEqual(ca.localPosition, cb.localPosition)) ||
            glm::any(glm::notEqual(ca.halfExtents, cb.halfExtents)) ||
            ca.mass != cb.mass || ca.health != cb.health ||
            ca.damageResistance != cb.damageResistance ||
            ca.materialIndex != cb.materialIndex) {
            return false;
        }
    }
    return true;
}

// 1. Data-driven config: JSON round-trip + strict validation.
void test_config_json() {
    FractureConfig config;
    std::string error;
    check(config.load_from_json(
              R"({"enabled":true,"indestructible":false,"chunkSize":3,"chunkHealth":40,"damageResistance":2,"massScale":1.5,"materialIndex":7})",
              error),
          "valid config loads");
    check(config.chunkSize == 3 && config.chunkHealth == 40.0f &&
              config.damageResistance == 2.0f && config.massScale == 1.5f &&
              config.materialIndex == 7 && config.enabled && !config.indestructible,
          "fields round-trip");

    FractureConfig defaults;
    check(defaults.load_from_json(R"({"chunkSize":4})", error) &&
              defaults.chunkSize == 4 && defaults.chunkHealth == 25.0f &&
              defaults.massScale == 1.0f && defaults.enabled,
          "missing fields keep defaults");

    auto refuse = [&](const std::string& json, const char* what) {
        FractureConfig c;
        std::string err;
        check(!c.load_from_json(json, err) && !err.empty(), what);
    };
    refuse(R"({"chunkSize":0})", "chunkSize 0 refused");
    refuse(R"({"chunkSize":17})", "chunkSize 17 refused");
    refuse(R"({"chunkHealth":0})", "chunkHealth 0 refused");
    refuse(R"({"damageResistance":-1})", "negative resistance refused");
    refuse(R"({"massScale":0})", "massScale 0 refused");
    refuse(R"({"materialIndex":256})", "materialIndex 256 refused");
    refuse("{not json", "malformed JSON refused");

    std::printf("[fracture] config JSON: round-trip + refusals OK\n");
}

// 2. Deterministic voxel -> chunk generation with mass from density.
void test_chunk_generation() {
    const std::vector<VoxelCell> region = solid_region(4, 4, 4, 42);
    const std::function<float(std::uint32_t)> densityOf =
        [](std::uint32_t id) { return id == 42 ? 2.0f : 1.0f; };

    FractureConfig config;
    std::string error;
    check(config.load_from_json(R"({"chunkSize":2,"massScale":1})", error),
          "config loads");

    const auto chunks = generate_voxel_fracture_chunks(region, { 0, 0, 0 }, densityOf, config);
    check(chunks.size() == 8, "4x4x4 / chunkSize 2 -> 8 chunks");
    if (chunks.size() == 8) {
        // Deterministic layout: half extents 1, mass = 2.0 * 8 cells = 16.
        check(glm::all(glm::equal(chunks[0].halfExtents, glm::vec3(1.0f))),
              "chunk half extents = chunkSize/2");
        for (const auto& chunk : chunks) {
            check(chunk.mass == 16.0f, "mass = density * 8 cells (16)");
            check(chunk.health == 25.0f && chunk.damageResistance == 0.0f,
                  "health/resistance from config");
        }
        // Scan order (y,z,x): first chunk at (0,0,0), then (2,0,0).
        check(chunks[0].localPosition == glm::vec3(0.0f, 0.0f, 0.0f) &&
                  chunks[1].localPosition == glm::vec3(2.0f, 0.0f, 0.0f),
              "bucket positions follow scan order");
    }

    // Bit-exact determinism: same input + config twice -> identical list.
    const auto again = generate_voxel_fracture_chunks(region, { 0, 0, 0 }, densityOf, config);
    check(same_chunks(chunks, again), "generation is deterministic (bit-exact)");

    // chunkSize 4 -> one chunk (whole region); chunkSize 1 -> 64 chunks.
    FractureConfig big;
    check(big.load_from_json(R"({"chunkSize":4})", error), "config loads");
    check(generate_voxel_fracture_chunks(region, { 0, 0, 0 }, densityOf, big).size() == 1,
          "4x4x4 / chunkSize 4 -> 1 chunk");
    FractureConfig fine;
    check(fine.load_from_json(R"({"chunkSize":1,"massScale":0.5})", error),
          "config loads");
    const auto fineChunks =
        generate_voxel_fracture_chunks(region, { 0, 0, 0 }, densityOf, fine);
    check(fineChunks.size() == 64, "4x4x4 / chunkSize 1 -> 64 chunks");
    if (fineChunks.size() == 64)
        check(fineChunks[0].mass == 1.0f, "mass = density * 1 cell * 0.5");

    // Root offset lands in localPosition.
    FractureConfig offset = config;
    const auto offsetChunks =
        generate_voxel_fracture_chunks(region, { 4, 4, 4 }, densityOf, config);
    check(offsetChunks.size() == 8 && offsetChunks[0].localPosition == glm::vec3(-4.0f, -4.0f, -4.0f),
          "localPosition is relative to the root");

    // enabled=false / indestructible=true -> no chunks (nothing to fracture).
    FractureConfig disabled;
    check(disabled.load_from_json(R"({"enabled":false})", error), "config loads");
    check(generate_voxel_fracture_chunks(region, { 0, 0, 0 }, densityOf, disabled).empty(),
          "enabled=false generates no chunks");
    FractureConfig indestructible;
    check(indestructible.load_from_json(R"({"indestructible":true})", error),
          "config loads");
    check(generate_voxel_fracture_chunks(region, { 0, 0, 0 }, densityOf, indestructible).empty(),
          "indestructible generates no chunks");

    std::printf("[fracture] generation: deterministic chunks + mass from density OK\n");
}

// 3. Voxel fracture lifecycle in the STANDARD WORLD (Jolt): damage detaches
//    chunks, the kinematic -> dynamic flip reaches Jolt, and the debris
//    actually falls under gravity (the mirror-only flip left it floating).
void test_voxel_fracture_lifecycle() {
    const std::vector<VoxelCell> region = solid_region(4, 4, 4, 42);
    const std::function<float(std::uint32_t)> densityOf = [](std::uint32_t) { return 2.0f; };
    FractureConfig config;
    std::string error;
    check(config.load_from_json(R"({"chunkSize":2})", error), "config loads");
    const auto chunks =
        generate_voxel_fracture_chunks(region, { 0, 0, 0 }, densityOf, config);
    check(chunks.size() == 8, "8 fracture chunks");

    // Destructible floats at y=20 (nothing below: pure gravity fall proof).
    PhysicsRuntime world(WorldSettings{}, PhysicsBackendKind::Jolt);
    DestructibleRuntime destructible;
    check(destructible.create(world, glm::vec3(0.0f, 20.0f, 0.0f),
                              glm::quat(1.0f, 0.0f, 0.0f, 0.0f), chunks),
          "destructible created from fracture chunks");
    for (int i = 0; i < 30; ++i) world.step(1.0f / 60.0f);

    // Small damage: nothing detaches.
    const auto small =
        destructible.apply_radial_damage(world, glm::vec3(2.0f, 22.0f, 2.0f), 2.5f, 5.0f, 0.0f);
    check(small.empty(), "small damage below health does not fracture");

    // Big damage at the center: the 4 chunks within radius 2.5 detach
    // (damage 200, falloff 0.2 at distance 2 -> 40 > health 25).
    const auto events = destructible.apply_radial_damage(
        world, glm::vec3(2.0f, 22.0f, 2.0f), 2.5f, 200.0f, 0.0f);
    check(events.size() == 4, "center blast detaches 4 of 8 chunks");

    // Chunk 7 = bucket (2,2,2) (detached, falls); chunk 0 = (0,0,0) (attached).
    const BodyHandle fallen = destructible.chunks()[7].body;
    const BodyHandle anchored = destructible.chunks()[0].body;
    check(destructible.chunks()[7].detached && !destructible.chunks()[0].detached,
          "chunk 7 detached, chunk 0 attached");
    const RigidBody* falling = world.body(fallen);
    if (falling != nullptr) {
        check(falling->motion == MotionType::Dynamic,
              "detached chunk is dynamic (flip went through the runtime)");
        const float startY = falling->position.y;  // 22
        for (int i = 0; i < 90; ++i) world.step(1.0f / 60.0f);  // 1.5s
        check(falling->position.y < startY - 5.0f,
              "detached debris FALLS under gravity (Jolt motion flip works)");
    }
    const RigidBody* staying = world.body(anchored);
    if (staying != nullptr) {
        check(staying->motion == MotionType::Kinematic &&
                  staying->position.y == 20.0f,
              "attached chunk stays kinematic in place");
    }

    // A full blast at huge radius destroys everything.
    const auto all = destructible.apply_radial_damage(
        world, glm::vec3(0.0f, 20.0f, 0.0f), 100.0f, 1000.0f, 0.0f);
    check(!all.empty() && destructible.fully_destroyed(),
          "full blast destroys the whole destructible");

    std::printf("[fracture] lifecycle: debris detaches and FALLS under Jolt OK\n");
}

}  // namespace

int main() {
    std::printf("[fracture] configurable voxel fracture tests\n");
    test_config_json();
    test_chunk_generation();
    test_voxel_fracture_lifecycle();
    if (g_failures != 0) {
        std::printf("[fracture] %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("[fracture] ALL PASSED\n");
    return 0;
}
