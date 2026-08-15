// VoxelSdkTests.cpp
//
// Evidence for the public SDK milestone (META section 6 + 7):
//  - This translation unit compiles using ONLY public SDK headers
//    (<engine/...>) plus glm — no engine internals, no Vulkan. It stands in
//    for an external project consuming the engine as a library.
//  - The voxel world operates headless (no renderer) through IVoxelWorld:
//    generation, meshing and edits run on the simulation side.
//  - A registered generator replaces the builtin terrain (plug-in generation).
//  - Block/item registries are data-driven: JSON assets load at runtime,
//    identities are persistent UUIDs (stable across load order), invalid
//    assets fail with a clear diagnostic and a safe fallback exists.

#include <engine/version.hpp>
#include <engine/voxel/IVoxelWorld.hpp>
#include <engine/voxel/IVoxelServices.hpp>
#include <engine/voxel/IVoxelBlockEntity.hpp>
#include <engine/registry/BlockRegistry.hpp>
#include <engine/registry/FluidRegistry.hpp>
#include <engine/registry/ItemRegistry.hpp>
#include <engine/registry/ItemStack.hpp>
#include <engine/registry/Inventory.hpp>
#include <engine/registry/RecipeRegistry.hpp>
#include <engine/entity/IEntityWorld.hpp>
#include <engine/navigation/INavigationProvider.hpp>
#include <engine/navigation/VoxelNavigation.hpp>
#include <engine/compression/ICompressionProvider.hpp>
#include <engine/hashing/IHashProvider.hpp>
#include <engine/storage/IChunkStoreFactory.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
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

constexpr int kBlockAir = 0;
constexpr int kBlockStone = 3;

// Deterministic generator: a flat world at `height` with no caves/ores.
// Exercise for the public IVoxelGenerator contract (register_generator).
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

// ---- Project-owned block entities (META section 8 examples) ----
// The engine stores/ticks/persists; the project owns behavior and state. The
// blob is opaque to the engine; the project migrates its own versions.

// A counter machine: persisted counter + runtime tick count.
class CounterMachine final : public engine::voxel::IVoxelBlockEntity {
public:
    std::string type_id() const override { return "project:counter_machine"; }
    void on_tick(uint64_t worldTick) override {
        ++ticks_;
        lastWorldTick_ = worldTick;
    }
    uint32_t data_version() const override { return 1; }
    std::vector<uint8_t> serialize_state() const override {
        std::vector<uint8_t> blob(4);
        for (int i = 0; i < 4; ++i) {
            blob[static_cast<std::size_t>(i)] =
                static_cast<uint8_t>((counter_ >> (8 * i)) & 0xFFu);
        }
        return blob;
    }
    bool deserialize_state(const std::vector<uint8_t>& data,
                           uint32_t version) override {
        if (version != 1 || data.size() < 4) return false;
        counter_ = 0;
        for (int i = 0; i < 4; ++i) {
            counter_ |= static_cast<uint32_t>(data[static_cast<std::size_t>(i)])
                        << (8 * i);
        }
        return true;
    }
    void on_created() override { created_ = true; }
    void on_destroyed() override { destroyed_ = true; }

    uint32_t counter_{ 0 };        // persisted project state
    uint64_t ticks_{ 0 };          // runtime only (not persisted)
    uint64_t lastWorldTick_{ 0 };
    bool created_{ false };
    bool destroyed_{ false };
};

// A machine that refuses data it cannot interpret (version migration demo).
class StrictMachine final : public engine::voxel::IVoxelBlockEntity {
public:
    std::string type_id() const override { return "project:strict_machine"; }
    void on_tick(uint64_t) override {}
    uint32_t data_version() const override { return 2; }
    std::vector<uint8_t> serialize_state() const override { return {}; }
    bool deserialize_state(const std::vector<uint8_t>&, uint32_t version) override {
        return version == 2;  // refuses older/newer data
    }
};

// Boots a world headless: drive update() with real-time pacing until the
// center chunk is loaded or the wall-clock budget runs out.
bool boot_world(engine::voxel::IVoxelWorld& world, const glm::vec3& player,
                int budget, int maxBudgetMs = 8000) {
    world.set_chunk_budget(budget);
    world.set_mob_spawning(false);
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

// Runs update() until `predicate` holds or the wall-clock budget runs out.
// Light settles asynchronously (budgeted relight pass), so assertions poll.
template <typename Pred>
bool settle(engine::voxel::IVoxelWorld& world, const glm::vec3& player,
            Pred predicate, int maxMs = 4000) {
    const auto start = std::chrono::steady_clock::now();
    while (!predicate()) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count() > maxMs) {
            return false;
        }
        world.update(player, 1.0f / 60.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

void test_version() {
    const engine::VersionInfo version = engine::engine_version();
    CHECK(version.major >= 1);
    CHECK(version.abi != nullptr && version.abi[0] != '\0');
    CHECK(std::string(engine::engine_sdk_abi()) == "engine-voxel-1");
    std::cout << "[sdk] version " << version.major << '.' << version.minor
              << '.' << version.patch << " abi=" << version.abi << "\n";
}

// Headless world through the public interface: boot, edit roundtrip and
// voxel raycast against a flat world produced by a registered generator.
void test_world_headless() {
    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    CHECK(world != nullptr);

    // Register the generator BEFORE boot so generation uses it.
    world->register_generator(std::make_shared<FlatGenerator>(96));
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
    CHECK(boot_world(*world, player, 16));

    // Flat world: solid surface at y=96, water 97..127, air from 128 up.
    CHECK(world->get_block(0, 96, 0) != kBlockAir);
    CHECK(world->get_block(0, 128, 0) == kBlockAir);

    // Downward raycast hits the surface top face (before any edits).
    const engine::voxel::VoxelRaycastHit down =
        world->raycast(glm::vec3(8.0f, 160.0f, 8.0f), glm::vec3(0.0f, -1.0f, 0.0f), 200.0f);
    CHECK(down.hit);
    CHECK(down.block.y == 96);
    CHECK(down.normal.y > 0.5f);
    CHECK(down.chunk.x == 0 && down.chunk.y == 0);

    // Upward ray from above the water misses (nothing solid up there).
    const engine::voxel::VoxelRaycastHit up =
        world->raycast(glm::vec3(8.0f, 130.0f, 8.0f), glm::vec3(0.0f, 1.0f, 0.0f), 100.0f);
    CHECK(!up.hit);

    // Edit roundtrip on a loaded chunk.
    world->set_block(8, 120, 8, kBlockStone);
    CHECK(world->get_block(8, 120, 8) == kBlockStone);

    std::cout << "[sdk] headless world: flat height 96 booted, edits and "
                 "raycast OK\n";
}

// Transactional edits (META section 11): atomic multi-edit commit, full
// rollback on invalid input, undo/redo, single-edit convenience routed through
// the same path, event log and listener.
void test_transactions() {
    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*world, glm::vec3(8.0f, 200.0f, 8.0f), 16));

    int committed = 0, undone = 0, redone = 0, rolledBack = 0;
    world->set_transaction_listener(
        [&](const engine::voxel::TransactionEvent& event) {
            switch (event.kind) {
            case engine::voxel::TransactionEvent::Kind::Committed: ++committed; break;
            case engine::voxel::TransactionEvent::Kind::Undone: ++undone; break;
            case engine::voxel::TransactionEvent::Kind::Redone: ++redone; break;
            case engine::voxel::TransactionEvent::Kind::RolledBack: ++rolledBack; break;
            }
        });

    // 1. A multi-edit transaction applies atomically (y=130 is above the flat
    // surface, so previous state is Air everywhere).
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = world->begin_transaction();
        CHECK(tx != nullptr);
        CHECK(tx->edit_count() == 0);
        tx->set_block(3, 130, 3, kBlockStone);
        tx->set_block(4, 130, 4, kBlockStone);
        tx->set_block(5, 130, 5, kBlockStone);
        CHECK(tx->edit_count() == 3);
        std::string error;
        CHECK(tx->commit(error));
        CHECK(error.empty());
        CHECK(world->get_block(3, 130, 3) == kBlockStone);
        CHECK(world->get_block(4, 130, 4) == kBlockStone);
        CHECK(world->get_block(5, 130, 5) == kBlockStone);
        CHECK(world->undo_depth() == 1);
        CHECK(world->edit_log_count() == 3);
    }

    // 2. Undo restores the previous state; redo reapplies it.
    CHECK(world->undo_last_transaction());
    CHECK(world->get_block(3, 130, 3) == kBlockAir);
    CHECK(world->get_block(4, 130, 4) == kBlockAir);
    CHECK(world->undo_depth() == 0);
    CHECK(world->redo_last_transaction());
    CHECK(world->get_block(3, 130, 3) == kBlockStone);
    CHECK(world->get_block(5, 130, 5) == kBlockStone);
    CHECK(world->undo_depth() == 1);

    // 3. Invalid block id: commit fails, nothing changes, event logged.
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = world->begin_transaction();
        tx->set_block(6, 130, 6, 999'999);
        std::string error;
        CHECK(!tx->commit(error));
        CHECK(!error.empty());
        CHECK(world->get_block(6, 130, 6) == kBlockAir);
        CHECK(world->undo_depth() == 1);   // valid transaction still undoable
        CHECK(world->edit_log_count() == 3);  // rejected edit not logged
    }

    // 4. remove_block + the single-edit convenience API are transactional and
    // undoable (no parallel mutation path).
    world->set_block(9, 130, 9, kBlockStone);          // implicit transaction
    CHECK(world->get_block(9, 130, 9) == kBlockStone);
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> rm = world->begin_transaction();
        rm->remove_block(9, 130, 9);
        std::string error;
        CHECK(rm->commit(error));
        CHECK(world->get_block(9, 130, 9) == kBlockAir);
    }
    CHECK(world->undo_last_transaction());             // undo remove -> stone
    CHECK(world->get_block(9, 130, 9) == kBlockStone);
    CHECK(world->undo_last_transaction());             // undo implicit set -> air
    CHECK(world->get_block(9, 130, 9) == kBlockAir);
    CHECK(world->undo_depth() == 1);                   // multi-edit tx still there
    CHECK(world->undo_last_transaction());             // undo multi-edit tx
    CHECK(world->get_block(3, 130, 3) == kBlockAir);
    CHECK(world->get_block(5, 130, 5) == kBlockAir);
    CHECK(world->undo_depth() == 0);
    CHECK(!world->undo_last_transaction());            // empty undo stack

    // 5. Event counters match the outcome sequence.
    CHECK(committed == 3);   // multi-edit, implicit set, remove
    CHECK(undone == 4);      // multi-edit undo + redo pair, remove, implicit, multi-edit
    CHECK(redone == 1);      // multi-edit redo
    CHECK(rolledBack == 1);  // invalid id

    std::cout << "[sdk] transactions: atomic commit, rollback, undo/redo, "
                 "event log OK\n";
}

// Persistence (META section 10): versioned save/load with semantic identity,
// idempotent re-serialization, corruption/bad-magic/bad-version refusal, and a
// file roundtrip.
void test_persistence() {
    // World A: flat terrain + a transactional edit, then serialize.
    std::unique_ptr<engine::voxel::IVoxelWorld> a =
        engine::voxel::create_default_voxel_world();
    a->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*a, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = a->begin_transaction();
        tx->set_block(3, 130, 3, kBlockStone);
        tx->set_block(5, 130, 5, kBlockStone);
        std::string error;
        CHECK(tx->commit(error));
        CHECK(error.empty());
    }
    std::string errorA;
    const std::string bytesA = a->serialize_world(errorA);
    CHECK(errorA.empty());
    CHECK(bytesA.size() > 100);

    // World B: same generator, restore the save, compare a sample grid.
    std::unique_ptr<engine::voxel::IVoxelWorld> b =
        engine::voxel::create_default_voxel_world();
    b->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*b, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    std::string errorB;
    CHECK(b->deserialize_world(bytesA, errorB));
    CHECK(errorB.empty());

    const int xs[] = { 0, 3, 5, 8, 15 };
    const int zs[] = { 0, 8, 15 };
    const int ys[] = { 1, 50, 96, 100, 120, 127, 128, 130, 200 };
    bool identical = true;
    for (const int x : xs) {
        for (const int z : zs) {
            for (const int y : ys) {
                if (a->get_block(x, y, z) != b->get_block(x, y, z)) identical = false;
            }
        }
    }
    CHECK(identical);

    // Idempotency: serializing the restored world yields the same bytes.
    std::string errorC;
    const std::string bytesB = b->serialize_world(errorC);
    CHECK(errorC.empty());
    CHECK(bytesB == bytesA);

    // Corruption: a flipped byte is refused with a clear diagnostic.
    std::string corrupted = bytesA;
    corrupted[bytesA.size() / 2] ^= 0xFF;
    std::string errorD;
    CHECK(!b->deserialize_world(corrupted, errorD));
    CHECK(!errorD.empty());

    // Bad magic and unsupported schema version are refused too.
    std::string badMagic = bytesA;
    badMagic[0] = 'X';
    std::string errorE;
    CHECK(!b->deserialize_world(badMagic, errorE));
    CHECK(!errorE.empty());
    std::string badVersion = bytesA;
    badVersion[5] = 9;  // schema version field (u32 LE at offset 5)
    std::string errorF;
    CHECK(!b->deserialize_world(badVersion, errorF));
    CHECK(!errorF.empty());

    // File roundtrip: save to disk, load into a fresh world.
    const char* path = "vc_test_world.vcwld";
    std::string errorG;
    CHECK(a->save_world(path, errorG));
    CHECK(errorG.empty());
    std::unique_ptr<engine::voxel::IVoxelWorld> d =
        engine::voxel::create_default_voxel_world();
    d->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*d, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    std::string errorH;
    CHECK(d->load_world(path, errorH));
    CHECK(errorH.empty());
    CHECK(d->get_block(3, 130, 3) == kBlockStone);
    CHECK(d->get_block(5, 130, 5) == kBlockStone);

    // Repeated saves to the SAME slot must replace the previous save (Windows
    // std::rename semantics must not silently fail on an existing target).
    {
        // A second transaction so the new save differs from the first.
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = a->begin_transaction();
        tx->set_block(7, 130, 7, kBlockStone);
        std::string error;
        CHECK(tx->commit(error));
        std::string saveError;
        CHECK(a->save_world(path, saveError));
        CHECK(saveError.empty());
    }
    {
        std::unique_ptr<engine::voxel::IVoxelWorld> e =
            engine::voxel::create_default_voxel_world();
        e->register_generator(std::make_shared<FlatGenerator>(96));
        CHECK(boot_world(*e, glm::vec3(8.0f, 200.0f, 8.0f), 16));
        std::string loadError;
        CHECK(e->load_world(path, loadError));
        CHECK(loadError.empty());
        CHECK(e->get_block(7, 130, 7) == kBlockStone);  // second save won
        CHECK(e->get_block(3, 130, 3) == kBlockStone);  // first content kept
        CHECK(e->get_block(9, 130, 9) == kBlockAir);     // nothing stale
    }
    std::remove(path);

    std::cout << "[sdk] persistence: versioned save/load, idempotency, "
                 "corruption & file roundtrip OK\n";
}

// Promoted solution (META section 32): Zstandard adapter round-trips byte
// strings, detects its own frames and fails cleanly on foreign input.
void test_compression_provider() {
    std::shared_ptr<engine::compression::ICompressionProvider> z =
        engine::compression::create_zstd_compression_provider();
    CHECK(z != nullptr);

    // Round-trip: empty, tiny, repetitive (compressible) and random data.
    const std::string inputs[] = {
        std::string(),
        "x",
        "hello hello hello hello hello\n",
        std::string(200000, 'a'),
    };
    for (const std::string& in : inputs) {
        const std::string frame = z->compress(in);
        CHECK(!frame.empty());          // even empty input yields a real frame
        CHECK(z->is_compressed(frame));
        CHECK(z->decompress(frame) == in);
    }
    // Compressible input actually shrinks.
    CHECK(z->compress(std::string(200000, 'a')).size() < 2000u);

    // Foreign/garbage input is not a frame and decompression fails cleanly.
    CHECK(!z->is_compressed("not a zstd frame"));
    CHECK(z->decompress("not a zstd frame").empty());
    // A truncated frame fails cleanly too (no crash).
    const std::string frame = z->compress("payload");
    CHECK(!frame.empty());
    CHECK(z->decompress(frame.substr(0, frame.size() / 2)).empty());

    std::cout << "[sdk] compression: zstd round-trip, frame detection, "
                 "clean failure OK\n";
}

// Promoted solution (META section 32): BLAKE3 adapter matches the official
// test vector (BLAKE3-256 of empty input) and is deterministic.
void test_hash_provider() {
    std::shared_ptr<engine::hashing::IHashProvider> h =
        engine::hashing::create_blake3_hash_provider();
    CHECK(h != nullptr);
    CHECK(h->digest_size() == 32u);

    // Official BLAKE3-256 of the empty string (BLAKE3 test_vectors.json).
    const std::string emptyHex =
        "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262";
    CHECK(h->hash_hex(std::string()) == emptyHex);
    CHECK(h->hash(std::string()).size() == 32u);

    // Determinism and sensitivity.
    CHECK(h->hash_hex("abc") == h->hash_hex("abc"));
    CHECK(h->hash_hex("abc") != h->hash_hex("abd"));
    CHECK(h->hash_hex("abc").size() == 64u);
    {   // hex-escape trap: \x consumes following hex digits, so build chars
        // explicitly to test to_hex with 0x00, 0x01, 0xff.
        std::string raw;
        raw.push_back(static_cast<char>(0x00));
        raw.push_back(static_cast<char>(0x01));
        raw.push_back(static_cast<char>(0xFF));
        CHECK(engine::hashing::to_hex(raw) == "0001ff");
    }

    std::cout << "[sdk] hash: BLAKE3 official vector + determinism OK\n";
}

// v4 keeps loading legacy v1-v3 saves: build a v3 save by hand (FNV-1a
// checksum, no entity section) and confirm it loads; also confirm the v4 file
// round-trip through the zstd-compressed file layer preserves the world.
void test_save_v4_legacy() {
    // Minimal v3 save: magic + version 3 + palette count 0 + chunk count 0 +
    // entity count 0 + FNV-1a (offset/prime from the documented format).
    std::string body = "VCWLD";
    auto appendU32 = [&body](uint32_t v) {
        body.push_back(static_cast<char>(v & 0xFFu));
        body.push_back(static_cast<char>((v >> 8) & 0xFFu));
        body.push_back(static_cast<char>((v >> 16) & 0xFFu));
        body.push_back(static_cast<char>((v >> 24) & 0xFFu));
    };
    appendU32(3);  // schema version 3
    appendU32(0);  // palette count
    appendU32(0);  // chunk count
    appendU32(0);  // block entity count (v3 section)
    uint64_t hash = 1469598103934665603ull;
    for (const unsigned char c : body) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    for (int i = 0; i < 8; ++i) {
        body.push_back(static_cast<char>((hash >> (8 * i)) & 0xFFu));
    }

    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*world, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    std::string error;
    CHECK(world->deserialize_world(body, error));
    CHECK(error.empty());

    // A v3 save with a corrupted FNV checksum is still refused.
    std::string corrupted = body;
    corrupted[body.size() - 1] ^= 0x01;
    std::string corruptError;
    CHECK(!world->deserialize_world(corrupted, corruptError));
    CHECK(!corruptError.empty());

    // v4 file round-trip: save compresses with zstd, load decompresses, and
    // the compressed file is smaller than the serialized body for the
    // (highly compressible, mostly-air) flat world.
    std::unique_ptr<engine::voxel::IVoxelWorld> a =
        engine::voxel::create_default_voxel_world();
    a->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*a, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = a->begin_transaction();
        tx->set_block(3, 130, 3, kBlockStone);
        std::string txError;
        CHECK(tx->commit(txError));
        CHECK(txError.empty());
    }
    std::string serError;
    const std::string serialized = a->serialize_world(serError);
    CHECK(serError.empty());
    CHECK(!serialized.empty());
    // v5: "VCWLD" + u32 version(5) + FlatBuffers container with identifier
    // "WLD5" (root uoffset u32, then the 4-char identifier, then the table).
    CHECK(serialized.size() >= 5u + 4u + 8u);
    CHECK(serialized.compare(0, 5, "VCWLD") == 0);
    const uint32_t v5Version =
        static_cast<uint32_t>(static_cast<unsigned char>(serialized[5])) |
        (static_cast<uint32_t>(static_cast<unsigned char>(serialized[6])) << 8) |
        (static_cast<uint32_t>(static_cast<unsigned char>(serialized[7])) << 16) |
        (static_cast<uint32_t>(static_cast<unsigned char>(serialized[8])) << 24);
    CHECK(v5Version == 5u);
    CHECK(serialized.compare(13, 4, "WLD5") == 0);

    // A v4 save (manual binary body + BLAKE3 checksum, the previous format)
    // must still load through the legacy parser: empty palette/chunks/entities
    // plus the BLAKE3-256 digest over the body.
    {
        std::string v4 = "VCWLD";
        auto appendU32v4 = [&v4](uint32_t v) {
            v4.push_back(static_cast<char>(v & 0xFFu));
            v4.push_back(static_cast<char>((v >> 8) & 0xFFu));
            v4.push_back(static_cast<char>((v >> 16) & 0xFFu));
            v4.push_back(static_cast<char>((v >> 24) & 0xFFu));
        };
        appendU32v4(4);  // schema version 4
        appendU32v4(0);  // palette count
        appendU32v4(0);  // chunk count
        appendU32v4(0);  // block entity count
        const std::shared_ptr<engine::hashing::IHashProvider> h4 =
            engine::hashing::create_blake3_hash_provider();
        v4 += h4->hash(v4);
        std::unique_ptr<engine::voxel::IVoxelWorld> legacy =
            engine::voxel::create_default_voxel_world();
        legacy->register_generator(std::make_shared<FlatGenerator>(96));
        CHECK(boot_world(*legacy, glm::vec3(8.0f, 200.0f, 8.0f), 16));
        std::string v4Error;
        CHECK(legacy->deserialize_world(v4, v4Error));
        CHECK(v4Error.empty());
        std::string badV4 = v4;
        badV4[badV4.size() - 1] ^= 0x01;
        std::string badV4Error;
        CHECK(!legacy->deserialize_world(badV4, badV4Error));
        CHECK(!badV4Error.empty());
    }

    const char* path = "vc_test_world_v4.vcwld";
    std::string saveError;
    CHECK(a->save_world(path, saveError));
    CHECK(saveError.empty());

    std::string fileBytes;
    {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream buf;
        buf << in.rdbuf();
        fileBytes = buf.str();
    }  // close before std::remove (Windows refuses to delete open files)
    // The file layer writes a zstd frame: magic 28 B5 2F FD.
    CHECK(fileBytes.size() >= 4u);
    CHECK(static_cast<unsigned char>(fileBytes[0]) == 0x28u);
    CHECK(static_cast<unsigned char>(fileBytes[1]) == 0xB5u);
    CHECK(static_cast<unsigned char>(fileBytes[2]) == 0x2Fu);
    CHECK(static_cast<unsigned char>(fileBytes[3]) == 0xFDu);
    CHECK(fileBytes.size() < serialized.size());

    std::unique_ptr<engine::voxel::IVoxelWorld> b =
        engine::voxel::create_default_voxel_world();
    b->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*b, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    std::string loadError;
    CHECK(b->load_world(path, loadError));
    CHECK(loadError.empty());
    CHECK(b->get_block(3, 130, 3) == kBlockStone);
    std::remove(path);

    std::cout << "[sdk] persistence v4: legacy v3 loads, zstd file layer "
                 "(smaller than raw) round-trips OK\n";
}

// Promoted solution (META section 32): the RocksDB chunk store persists world
// blobs in a real embedded key-value database, content-addressed by BLAKE3,
// and the world really delegates persistence to a registered service.
void test_rocksdb_storage() {
    const char* dbDir = "vc_test_rocksdb";

    // World A: flat + edits, serialize (v5 flatbuffer + BLAKE3 body).
    std::unique_ptr<engine::voxel::IVoxelWorld> a =
        engine::voxel::create_default_voxel_world();
    a->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*a, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = a->begin_transaction();
        tx->set_block(3, 130, 3, kBlockStone);
        tx->set_block(5, 130, 5, kBlockStone);
        std::string error;
        CHECK(tx->commit(error));
        CHECK(error.empty());
    }
    std::string serError;
    const std::string blob = a->serialize_world(serError);
    CHECK(serError.empty());
    CHECK(!blob.empty());

    // Save into a fresh RocksDB database: deserialize (caches + persists the
    // blob content-addressed), then save_world materializes the DB on disk.
    std::shared_ptr<engine::voxel::IChunkStorage> store =
        engine::storage::create_rocksdb_chunk_storage();
    CHECK(store != nullptr);
    std::string storeError;
    CHECK(store->deserialize_world(blob, storeError));
    CHECK(storeError.empty());
    CHECK(store->save_world(dbDir, storeError));
    CHECK(storeError.empty());
    store.reset();  // close the DB

    // Load into a NEW store: the world blob survives the database round-trip.
    std::shared_ptr<engine::voxel::IChunkStorage> store2 =
        engine::storage::create_rocksdb_chunk_storage();
    std::string loadError;
    CHECK(store2->load_world(dbDir, loadError));
    CHECK(loadError.empty());
    std::string outError;
    const std::string restored = store2->serialize_world(outError);
    CHECK(outError.empty());
    CHECK(restored == blob);

    // The restored blob loads into a fresh world with the same content.
    std::unique_ptr<engine::voxel::IVoxelWorld> b =
        engine::voxel::create_default_voxel_world();
    b->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*b, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    std::string bError;
    CHECK(b->deserialize_world(restored, bError));
    CHECK(bError.empty());
    CHECK(b->get_block(3, 130, 3) == kBlockStone);
    CHECK(b->get_block(5, 130, 5) == kBlockStone);

    // The world really delegates save/load to the registered service.
    b->register_storage(store2);
    const std::vector<std::string> services = b->registered_services();
    bool hasStorage = false;
    for (const std::string& name : services) {
        if (name == "storage") hasStorage = true;
    }
    CHECK(hasStorage);

    // Loading into an empty database is a clear diagnostic, not a crash.
    std::shared_ptr<engine::voxel::IChunkStorage> store3 =
        engine::storage::create_rocksdb_chunk_storage();
    std::string emptyError;
    CHECK(!store3->load_world("vc_test_rocksdb_empty", emptyError));
    CHECK(!emptyError.empty());

    // Release the database handles BEFORE removing the directories: on
    // Windows a directory holding an open file cannot be deleted. The world
    // `b` owns the registered store and the test locals store2/store3 still
    // hold DB handles (dbDir and the empty database respectively).
    b.reset();
    store2.reset();
    store3.reset();
    std::filesystem::remove_all(dbDir);
    std::filesystem::remove_all("vc_test_rocksdb_empty");
    std::cout << "[sdk] storage: RocksDB round-trip (BLAKE3-addressed), "
                 "world delegation, empty-db diagnostic OK\n";
}

// A project-supplied persistence backend (IChunkStorage). Proves the world
// really delegates save/load to a registered service instead of the builtin
// path — the storage service wiring is observable, not decorative.
class RecordingStorage final : public engine::voxel::IChunkStorage {
public:
    int serialized = 0, deserialized = 0, saved = 0, loaded = 0;
    std::string lastPayload;

    std::string serialize_world(std::string& errorOut) override {
        ++serialized;
        errorOut.clear();
        return "custom-bytes";
    }
    bool deserialize_world(const std::string& data, std::string& errorOut) override {
        ++deserialized;
        lastPayload = data;
        errorOut.clear();
        return true;
    }
    bool save_world(const std::string&, std::string& errorOut) override {
        ++saved;
        errorOut.clear();
        return true;
    }
    bool load_world(const std::string&, std::string& errorOut) override {
        ++loaded;
        errorOut.clear();
        return true;
    }
};

// IChunkStorage wiring (META section 6): a registered storage backend takes
// over every persistence entry point, and registered_services() reports it.
void test_storage_service() {
    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    auto storage = std::make_shared<RecordingStorage>();
    world->register_storage(storage);

    const std::vector<std::string> names = world->registered_services();
    CHECK(std::find(names.begin(), names.end(), "storage") != names.end());

    std::string error;
    CHECK(world->serialize_world(error) == "custom-bytes");
    CHECK(error.empty());
    CHECK(storage->serialized == 1);

    CHECK(world->deserialize_world("custom-bytes", error));
    CHECK(error.empty());
    CHECK(storage->deserialized == 1);
    CHECK(storage->lastPayload == "custom-bytes");

    CHECK(world->save_world("ignored-path", error));
    CHECK(error.empty());
    CHECK(storage->saved == 1);

    CHECK(world->load_world("ignored-path", error));
    CHECK(error.empty());
    CHECK(storage->loaded == 1);

    std::cout << "[sdk] storage service: persistence delegates to a "
                 "registered IChunkStorage\n";
}

// A JSON-only block survives save/load through the UUID palette: the palette
// pins each dynamic runtime id to its persistent UUID, so a world saved with
// the registry loaded as [ruby, sapphire] restores byte-identical ids in a
// registry loaded as [sapphire, ruby] — ids never depend on load order.
void test_dynamic_block_persistence() {
    // World A: registry [ruby, sapphire], place ruby, save.
    std::unique_ptr<engine::voxel::IVoxelWorld> a =
        engine::voxel::create_default_voxel_world();
    auto registryA = std::make_shared<engine::registry::BlockRegistry>();
    std::string error;
    CHECK(registryA->load_from_json(
        R"([{"name":"ruby","namespace":"test","color":[0.9,0.1,0.1]},)"
        R"({"name":"sapphire","namespace":"test","color":[0.1,0.1,0.9]}])",
        error));
    a->set_block_registry(registryA);
    CHECK(boot_world(*a, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    uint32_t rubyIdA = 0;
    CHECK(a->resolve_block_id("test:ruby", rubyIdA, error));
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = a->begin_transaction();
        tx->set_block(3, 130, 3, rubyIdA);
        tx->set_block(7, 130, 7, rubyIdA);
        std::string txError;
        CHECK(tx->commit(txError));
        CHECK(txError.empty());
    }
    std::string saveError;
    const std::string bytes = a->serialize_world(saveError);
    CHECK(saveError.empty());

    // World B: registry loaded in the REVERSED order — ids must match.
    std::unique_ptr<engine::voxel::IVoxelWorld> b =
        engine::voxel::create_default_voxel_world();
    auto registryB = std::make_shared<engine::registry::BlockRegistry>();
    CHECK(registryB->load_from_json(
        R"([{"name":"sapphire","namespace":"test","color":[0.1,0.1,0.9]},)"
        R"({"name":"ruby","namespace":"test","color":[0.9,0.1,0.1]}])",
        error));
    b->set_block_registry(registryB);
    CHECK(boot_world(*b, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    uint32_t rubyIdB = 0;
    CHECK(b->resolve_block_id("test:ruby", rubyIdB, error));
    CHECK(rubyIdB == rubyIdA);  // UUID-sorted allocation: order-independent

    std::string loadError;
    CHECK(b->deserialize_world(bytes, loadError));
    CHECK(loadError.empty());
    CHECK(b->get_block(3, 130, 3) == rubyIdB);
    CHECK(b->get_block(7, 130, 7) == rubyIdB);
    CHECK(b->get_block(4, 130, 4) == kBlockAir);

    // Idempotency through the palette: re-serializing B yields the same bytes.
    std::string reError;
    CHECK(b->serialize_world(reError) == bytes);

    std::cout << "[sdk] dynamic block persistence: UUID palette survives load "
                 "order, byte-identical reserialization\n";
}

// Block entities (META section 8): lifecycle, deterministic ticking through
// the world scheduler and atomic destroy with the block. All through the
// public contract, in the external-project TU.
void test_block_entity_lifecycle() {
    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    CHECK(world != nullptr);
    world->register_generator(std::make_shared<FlatGenerator>(96));
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
    CHECK(boot_world(*world, player, 16));

    world->register_block_entity_type("project:counter_machine",
        [] { return std::make_shared<CounterMachine>(); });

    std::vector<engine::voxel::BlockEntityEvent> events;
    world->set_block_entity_listener(
        [&](const engine::voxel::BlockEntityEvent& event) { events.push_back(event); });

    // Attach requires a loaded non-empty block; (5,96,5) is the flat surface.
    auto machine = std::make_shared<CounterMachine>();
    std::string error;
    CHECK(world->attach_block_entity(5, 96, 5, machine, error));
    CHECK(error.empty());
    CHECK(world->block_entity_count() == 1);
    CHECK(world->block_entity_at(5, 96, 5) != nullptr);
    CHECK(world->block_entity_at(5, 96, 5)->type_id() == "project:counter_machine");
    CHECK(machine->created_);
    CHECK(events.size() == 1 && events[0].kind == engine::voxel::BlockEntityEvent::Kind::Attached);

    // Duplicate attach is refused; so is attaching to empty air or a type
    // without a registered factory.
    CHECK(!world->attach_block_entity(5, 96, 5, std::make_shared<CounterMachine>(), error));
    CHECK(!world->attach_block_entity(5, 130, 5, std::make_shared<CounterMachine>(), error));
    CHECK(!error.empty());
    CHECK(!world->attach_block_entity(6, 96, 6, std::make_shared<StrictMachine>(), error));

    // Ticking: update() drives the scheduler's BlockTick phase. Every update
    // is >= the 80ms fixed step, so each fires at least one tick (the
    // scheduler carries any leftover from boot, so exact counts are not
    // asserted — monotonic progress is).
    CHECK(machine->ticks_ == 0);
    world->update(player, 0.09f);
    world->update(player, 0.09f);
    world->update(player, 0.09f);
    CHECK(machine->ticks_ >= 3);
    CHECK(machine->lastWorldTick_ > 0);

    // Explicit detach: gone, Detached event, destroyed hook.
    CHECK(world->remove_block_entity(5, 96, 5));
    CHECK(world->block_entity_count() == 0);
    CHECK(world->block_entity_at(5, 96, 5) == nullptr);
    CHECK(machine->destroyed_);
    CHECK(events.size() == 2 && events[1].kind == engine::voxel::BlockEntityEvent::Kind::Detached);

    std::cout << "[sdk] block entities: lifecycle + scheduler ticking OK\n";
}

void test_block_entity_atomic_destroy() {
    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(96));
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
    CHECK(boot_world(*world, player, 16));
    world->register_block_entity_type("project:counter_machine",
        [] { return std::make_shared<CounterMachine>(); });

    auto machine = std::make_shared<CounterMachine>();
    std::string error;
    CHECK(world->attach_block_entity(6, 96, 6, machine, error));

    // Removing the block (transaction -> Air) destroys the entity atomically.
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = world->begin_transaction();
        tx->remove_block(6, 96, 6);
        std::string txError;
        CHECK(tx->commit(txError));
    }
    CHECK(world->block_entity_count() == 0);
    CHECK(machine->destroyed_);

    // The single-edit convenience path behaves identically (Air removes).
    auto second = std::make_shared<CounterMachine>();
    CHECK(world->attach_block_entity(7, 96, 7, second, error));
    world->set_block(7, 96, 7, kBlockAir);
    CHECK(world->block_entity_count() == 0);
    CHECK(second->destroyed_);

    // A non-Air replacement keeps the entity (project decides compatibility).
    auto third = std::make_shared<CounterMachine>();
    CHECK(world->attach_block_entity(8, 96, 8, third, error));
    world->set_block(8, 96, 8, kBlockStone);
    CHECK(world->block_entity_count() == 1);
    CHECK(world->block_entity_at(8, 96, 8) != nullptr);

    std::cout << "[sdk] block entities: atomic destroy with the block OK\n";
}

void test_block_entity_persistence() {
    // World A: attach a machine with project state, save.
    std::unique_ptr<engine::voxel::IVoxelWorld> a =
        engine::voxel::create_default_voxel_world();
    a->register_generator(std::make_shared<FlatGenerator>(96));
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
    CHECK(boot_world(*a, player, 16));
    a->register_block_entity_type("project:counter_machine",
        [] { return std::make_shared<CounterMachine>(); });
    auto machine = std::make_shared<CounterMachine>();
    machine->counter_ = 7;  // project state to survive the roundtrip
    std::string error;
    CHECK(a->attach_block_entity(8, 96, 8, machine, error));
    std::string saveError;
    const std::string bytes = a->serialize_world(saveError);
    CHECK(saveError.empty());
    // Deterministic entity section: re-serializing yields identical bytes.
    CHECK(a->serialize_world(saveError) == bytes);

    // World B without the factory: the save is refused with a diagnostic.
    std::unique_ptr<engine::voxel::IVoxelWorld> b =
        engine::voxel::create_default_voxel_world();
    b->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*b, player, 16));
    std::string loadError;
    CHECK(!b->deserialize_world(bytes, loadError));
    CHECK(loadError.find("project:counter_machine") != std::string::npos);

    // World C with the factory: entity reconstructed with its state and ticks.
    std::unique_ptr<engine::voxel::IVoxelWorld> c =
        engine::voxel::create_default_voxel_world();
    c->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*c, player, 16));
    c->register_block_entity_type("project:counter_machine",
        [] { return std::make_shared<CounterMachine>(); });
    CHECK(c->deserialize_world(bytes, loadError));
    CHECK(loadError.empty());
    auto restored = std::dynamic_pointer_cast<CounterMachine>(c->block_entity_at(8, 96, 8));
    CHECK(restored != nullptr);
    CHECK(restored->counter_ == 7);   // opaque project state restored
    CHECK(restored->ticks_ == 0);     // runtime-only state is NOT persisted
    CHECK(c->block_entity_count() == 1);
    c->update(player, 0.09f);
    c->update(player, 0.09f);
    CHECK(restored->ticks_ >= 2);     // the restored entity ticks again

    // An entity that refuses its data fails the load (never silently drops):
    // register a factory whose entity rejects the version written by the save.
    std::unique_ptr<engine::voxel::IVoxelWorld> d =
        engine::voxel::create_default_voxel_world();
    d->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*d, player, 16));
    d->register_block_entity_type("project:counter_machine",
        [] { return std::make_shared<StrictMachine>(); });  // lying factory
    std::string refuseError;
    CHECK(!d->deserialize_world(bytes, refuseError));
    CHECK(refuseError.find("refused") != std::string::npos);

    std::cout << "[sdk] block entities: versioned persistence roundtrip OK\n";
}

// Discrete world lighting (META section 12), through the public contract:
// skylight from column occlusion, block light from data-driven emitters,
// attenuation through air, opaque blocking, chunk borders and determinism.
void test_light_skylight() {
    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(96));
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
    CHECK(boot_world(*world, player, 16));

    // Sky settles asynchronously; poll until the surface column is occluded.
    CHECK(settle(*world, player, [&] {
        return world->get_sky_light(8, 96, 8) == 0 &&
               world->get_sky_light(8, 200, 8) == 15;
    }));
    CHECK(world->get_sky_light(8, 200, 8) == 15);  // open air: full sun
    CHECK(world->get_sky_light(8, 97, 8) == 15);   // water surface: sunlit
    CHECK(world->get_sky_light(8, 96, 8) == 0);    // stone occludes
    CHECK(world->get_sky_light(8, 90, 8) == 0);    // underground: dark

    std::cout << "[sdk] lighting: skylight from column occlusion OK\n";
}

void test_light_block_emitter() {
    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(96));
    auto registry = std::make_shared<engine::registry::BlockRegistry>();
    std::string error;
    CHECK(registry->load_from_json(
        R"([{"name":"lantern","namespace":"test","color":[1.0,0.9,0.3],"lightEmission":1.0}])",
        error));
    world->set_block_registry(registry);
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
    CHECK(boot_world(*world, player, 16));
    uint32_t lanternId = 0;
    CHECK(world->resolve_block_id("test:lantern", lanternId, error));

    // Place a light emitter in open air above the water line.
    world->set_block(8, 130, 8, lanternId);
    CHECK(settle(*world, player, [&] { return world->get_block_light(8, 130, 8) == 15; }));
    CHECK(world->get_block_light(8, 130, 8) == 15);  // emitter cell
    CHECK(world->get_block_light(8, 129, 8) == 14);  // one air step
    CHECK(world->get_block_light(8, 131, 8) == 14);
    CHECK(world->get_block_light(8, 128, 8) == 13);
    CHECK(world->get_block_light(8, 140, 8) == 5);   // 10 air steps: 15 - 10

    // Opaque blocks stop light: the stone's own cell goes dark even though
    // the emitter below is at full strength. (Light still wraps around the
    // stone through the neighboring column, so the cell directly above is
    // dimmer than the 13 it had before — never brighter.)
    world->set_block(8, 131, 8, kBlockStone);
    CHECK(settle(*world, player, [&] { return world->get_block_light(8, 131, 8) == 0; }));
    CHECK(world->get_block_light(8, 130, 8) == 15);  // emitter unaffected
    CHECK(world->get_block_light(8, 132, 8) < 13);   // direct beam blocked

    // Incremental removal: breaking the emitter decays the field.
    world->set_block(8, 130, 8, kBlockAir);
    CHECK(settle(*world, player, [&] {
        return world->get_block_light(8, 130, 8) == 0 &&
               world->get_block_light(8, 129, 8) == 0;
    }));
    CHECK(world->get_block_light(8, 130, 8) == 0);
    CHECK(world->get_block_light(8, 129, 8) == 0);

    std::cout << "[sdk] lighting: data-driven emitter, attenuation and removal OK\n";
}

void test_light_chunk_boundary() {
    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(96));
    auto registry = std::make_shared<engine::registry::BlockRegistry>();
    std::string error;
    CHECK(registry->load_from_json(
        R"([{"name":"lantern","namespace":"test","color":[1.0,0.9,0.3],"lightEmission":1.0}])",
        error));
    world->set_block_registry(registry);
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
    CHECK(boot_world(*world, player, 16));
    uint32_t lanternId = 0;
    CHECK(world->resolve_block_id("test:lantern", lanternId, error));

    // Emitter on the LAST column of chunk (0,0): light must cross into chunk
    // (1,0), which is loaded (render distance for budget 16 covers ring 2).
    world->set_block(15, 130, 8, lanternId);
    CHECK(settle(*world, player, [&] { return world->get_block_light(15, 130, 8) == 15; }));
    CHECK(settle(*world, player, [&] { return world->get_block_light(16, 130, 8) == 14; }));
    CHECK(world->get_block_light(14, 130, 8) == 14);   // same chunk
    CHECK(world->get_block_light(13, 130, 8) == 13);
    CHECK(world->get_block_light(17, 130, 8) == 13);   // deeper into chunk 1
    CHECK(world->get_block_light(24, 130, 8) == 6);    // 15 - 9 steps

    std::cout << "[sdk] lighting: block light crosses chunk borders OK\n";
}

void test_light_determinism() {
    const auto build = [&](const glm::vec3& player) {
        auto world = engine::voxel::create_default_voxel_world();
        world->register_generator(std::make_shared<FlatGenerator>(96));
        auto registry = std::make_shared<engine::registry::BlockRegistry>();
        std::string error;
        CHECK(registry->load_from_json(
            R"([{"name":"lantern","namespace":"test","color":[1.0,0.9,0.3],"lightEmission":1.0}])",
            error));
        world->set_block_registry(registry);
        CHECK(boot_world(*world, player, 16));
        uint32_t lanternId = 0;
        CHECK(world->resolve_block_id("test:lantern", lanternId, error));
        world->set_block(7, 130, 7, lanternId);
        world->set_block(12, 130, 12, lanternId);
        CHECK(settle(*world, player, [&] { return world->get_block_light(7, 130, 7) == 15; }));
        CHECK(settle(*world, player, [&] { return world->get_block_light(12, 130, 12) == 15; }));
        return world;
    };
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
    std::unique_ptr<engine::voxel::IVoxelWorld> a = build(player);
    std::unique_ptr<engine::voxel::IVoxelWorld> b = build(player);
    // Same blocks => same light, on any machine/pass order.
    for (int z = 6; z <= 13; ++z) {
        for (int x = 6; x <= 13; ++x) {
            CHECK(a->get_block_light(x, 130, z) == b->get_block_light(x, 130, z));
            CHECK(a->get_sky_light(x, 130, z) == b->get_sky_light(x, 130, z));
        }
    }

    std::cout << "[sdk] lighting: deterministic across instances OK\n";
}

// The block registry is the source of truth for settable ids on the world
// (META section 7 + Prioridade 0 item 1): a builtin block resolves to its
// engine id; a catalog-only JSON block gets a DYNAMIC runtime id (>= Count,
// allocated by UUID — order-independent) and can actually be placed, read,
// raycast against (solid/collidable semantics from the definition) and removed.
// A fabricated id that is not registered is still rejected instead of silently
// aliasing Air.
void test_world_registry_source_of_truth() {
    std::unique_ptr<engine::voxel::IVoxelWorld> world =
        engine::voxel::create_default_voxel_world();

    auto registry = std::make_shared<engine::registry::BlockRegistry>();
    std::string error;
    // ruby: solid/collidable red block. spirit: non-collidable (ray passes
    // through) green block. Both have NO builtinId — catalog-only.
    CHECK(registry->load_from_json(
        R"([{"name":"ruby","namespace":"test","class":"solid","hardness":3.0,"color":[0.9,0.1,0.1]},)"
        R"({"name":"spirit","namespace":"test","class":"solid","collidable":false,"color":[0.1,0.9,0.1]}])",
        error));
    world->set_block_registry(registry);
    // Flat world (surface at y=96) so the floating test blocks at y=130 are
    // in open air and the rays behave deterministically.
    world->register_generator(std::make_shared<FlatGenerator>(96));

    // Builtin block resolves to its engine id.
    uint32_t id = 0;
    CHECK(world->resolve_block_id("vulkancraft:stone", id, error));
    CHECK(error.empty());
    CHECK(id == kBlockStone);

    // Catalog-only blocks now resolve to DYNAMIC ids >= BlockType::Count.
    uint32_t rubyId = 0;
    CHECK(world->resolve_block_id("test:ruby", rubyId, error));
    CHECK(error.empty());
    CHECK(rubyId >= 51u);  // BlockType::Count
    uint32_t spiritId = 0;
    CHECK(world->resolve_block_id("test:spirit", spiritId, error));
    CHECK(error.empty());
    CHECK(spiritId != rubyId);

    CHECK(boot_world(*world, glm::vec3(8.0f, 200.0f, 8.0f), 16));

    // A dynamic block can be placed and read back through the transaction path.
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = world->begin_transaction();
        tx->set_block(3, 130, 3, rubyId);
        std::string txError;
        CHECK(tx->commit(txError));
        CHECK(txError.empty());
        CHECK(world->get_block(3, 130, 3) == rubyId);
    }

    // A fabricated id that is not registered is rejected (never Air silently).
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = world->begin_transaction();
        tx->set_block(6, 130, 6, 500);  // not in the runtime table
        std::string txError;
        CHECK(!tx->commit(txError));
        CHECK(!txError.empty());
        CHECK(world->get_block(6, 130, 6) == kBlockAir);  // nothing written
    }

    // Solid semantics come from the definition: a downward ray hits the
    // collidable ruby block, and passes THROUGH the non-collidable spirit.
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> tx = world->begin_transaction();
        tx->set_block(8, 130, 8, spiritId);
        std::string txError;
        CHECK(tx->commit(txError));
        CHECK(txError.empty());
    }
    const engine::voxel::VoxelRaycastHit hitRuby =
        world->raycast(glm::vec3(3.5f, 200.0f, 3.5f), glm::vec3(0.0f, -1.0f, 0.0f), 200.0f);
    CHECK(hitRuby.hit);
    CHECK(hitRuby.block.x == 3 && hitRuby.block.y == 130 && hitRuby.block.z == 3);
    const engine::voxel::VoxelRaycastHit throughSpirit =
        world->raycast(glm::vec3(8.5f, 200.0f, 8.5f), glm::vec3(0.0f, -1.0f, 0.0f), 200.0f);
    CHECK(throughSpirit.hit);                       // hits the flat ground below
    CHECK(throughSpirit.block.y < 130);             // spirit did NOT stop the ray

    // Air (id 0) remains settable: removal keeps working, and the dynamic
    // block is undoable through the same stack.
    {
        std::unique_ptr<engine::voxel::IVoxelTransaction> rm = world->begin_transaction();
        rm->remove_block(3, 130, 3);
        std::string rmError;
        CHECK(rm->commit(rmError));
        CHECK(rmError.empty());
        CHECK(world->get_block(3, 130, 3) == kBlockAir);
    }

    std::cout << "[sdk] world registry: dynamic ids for catalog-only blocks "
                 "(place/raycast/remove), fabricated ids rejected\n";
}

// Data-driven fluids (META section 13): the registry is the project's way to
// declare what a fluid IS (viscosity, range, cadence, source/falling/
// evaporation, damage); the engine runs the simulation.
void test_fluid_registry() {
    engine::registry::FluidRegistry fluids;
    std::string error;
    CHECK(fluids.load_from_json(
        R"([{"block":"test:sludge","viscosity":0.0,"range":4,"evaporation":false},)"
        R"({"block":"test:tar","viscosity":1.0,"range":2,"tickInterval":0.16,"damagePerTick":3.0}])",
        error));
    CHECK(error.empty());
    CHECK(fluids.size() == 2);

    const engine::registry::FluidDefinition* sludge = fluids.find_by_block("test:sludge");
    CHECK(sludge != nullptr);
    CHECK(sludge->viscosity == 0.0f);
    CHECK(sludge->range == 4);
    CHECK(!sludge->evaporation);
    CHECK(!sludge->uuid.empty());          // derived from the block name
    CHECK(fluids.find_by_uuid(sludge->uuid) != nullptr);
    const engine::registry::FluidDefinition* tar = fluids.find_by_block("test:tar");
    CHECK(tar != nullptr);
    CHECK(tar->damagePerTick == 3.0f);
    CHECK(tar->tickInterval == 0.16f);

    // Deterministic order: same UUID sequence regardless of load order.
    const std::vector<engine::registry::FluidDefinition> first = fluids.all_definitions();
    engine::registry::FluidRegistry reversed;
    CHECK(reversed.load_from_json(
        R"([{"block":"test:tar","viscosity":1.0,"range":2},)"
        R"({"block":"test:sludge","viscosity":0.0,"range":4}])",
        error));
    const std::vector<engine::registry::FluidDefinition> second = reversed.all_definitions();
    CHECK(first.size() == second.size());
    for (std::size_t i = 0; i < first.size(); ++i) CHECK(first[i].uuid == second[i].uuid);

    // Invalid definitions fail with a diagnostic (never a guessed fluid).
    engine::registry::FluidRegistry strict;
    CHECK(!strict.load_from_json(R"([{"viscosity":0.5}])", error));   // no block
    CHECK(error.find("block") != std::string::npos);
    CHECK(strict.size() == 0);

    std::cout << "[sdk] fluid registry: JSON, UUID identity, order-independent, "
                 "validation OK\n";
}

// The engine runs the project's fluid parameters: thin fluids spread 2 levels
// per step (rings at 2, 4), thick fluids 1 level per step (rings at 1, 2), and
// the spread budget (range) caps both.
void test_fluid_generalized() {
    const auto build = [&](const std::string& fluidJson, uint32_t& idOut,
                           std::unique_ptr<engine::voxel::IVoxelWorld>& worldOut,
                           const glm::vec3& player) {
        auto world = engine::voxel::create_default_voxel_world();
        world->register_generator(std::make_shared<FlatGenerator>(96));
        auto blocks = std::make_shared<engine::registry::BlockRegistry>();
        std::string error;
        CHECK(blocks->load_from_json(
            R"([{"name":"goo","namespace":"test","class":"fluid","color":[0.2,0.9,0.3]}])",
            error));
        world->set_block_registry(blocks);
        auto fluids = std::make_shared<engine::registry::FluidRegistry>();
        CHECK(fluids->load_from_json(fluidJson, error));
        CHECK(world->set_fluid_registry(fluids, error));
        CHECK(error.empty());
        CHECK(boot_world(*world, player, 16));
        CHECK(world->resolve_block_id("test:goo", idOut, error));
        worldOut = std::move(world);
    };
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };

    // Thin goo (viscosity 0 -> 2 levels/tick), range 4: rings at 2 and 4.
    std::unique_ptr<engine::voxel::IVoxelWorld> thin;
    uint32_t thinId = 0;
    build(R"([{"block":"test:goo","viscosity":0.0,"range":4,"falling":false}])",
          thinId, thin, player);
    // y=128 sits just above the flat world's water line (below is water, not
    // air) so the pool spreads sideways instead of falling/draining.
    thin->set_block(8, 128, 8, static_cast<uint32_t>(thinId));
    CHECK(settle(*thin, player, [&] { return thin->get_fluid_level(9, 128, 8) == 2; }));
    CHECK(thin->get_fluid_level(8, 128, 8) == 0);   // source
    CHECK(thin->get_fluid_level(9, 128, 8) == 2);   // thin: 2 levels per step
    CHECK(settle(*thin, player, [&] { return thin->get_fluid_level(10, 128, 8) == 4; }));
    CHECK(thin->get_block(11, 128, 8) == kBlockAir);  // range 4 caps the spread

    // Thick goo (viscosity 1 -> 1 level/tick), range 2: rings at 1 and 2.
    std::unique_ptr<engine::voxel::IVoxelWorld> thick;
    uint32_t thickId = 0;
    build(R"([{"block":"test:goo","viscosity":1.0,"range":2,"falling":false}])",
          thickId, thick, player);
    thick->set_block(8, 128, 8, static_cast<uint32_t>(thickId));
    CHECK(settle(*thick, player, [&] { return thick->get_fluid_level(9, 128, 8) == 1; }));
    CHECK(thick->get_fluid_level(9, 128, 8) == 1);
    CHECK(settle(*thick, player, [&] { return thick->get_fluid_level(10, 128, 8) == 2; }));
    CHECK(thick->get_block(11, 128, 8) == kBlockAir);  // range 2 caps it

    std::cout << "[sdk] fluids: data-driven spread (thin 2/tick, thick 1/tick, "
                 "range caps) OK\n";
}

// evaporation=false keeps an unfed cell (pooled); evaporation=true decays it.
// Floating pools (below = air) are never fed, so the flag is directly visible.
void test_fluid_evaporation() {
    const auto build = [&](const std::string& fluidJson, uint32_t& idOut,
                           std::unique_ptr<engine::voxel::IVoxelWorld>& worldOut,
                           const glm::vec3& player) {
        auto world = engine::voxel::create_default_voxel_world();
        world->register_generator(std::make_shared<FlatGenerator>(96));
        auto blocks = std::make_shared<engine::registry::BlockRegistry>();
        std::string error;
        CHECK(blocks->load_from_json(
            R"([{"name":"goo","namespace":"test","class":"fluid","color":[0.2,0.9,0.3]}])",
            error));
        world->set_block_registry(blocks);
        auto fluids = std::make_shared<engine::registry::FluidRegistry>();
        CHECK(fluids->load_from_json(fluidJson, error));
        CHECK(world->set_fluid_registry(fluids, error));
        CHECK(boot_world(*world, player, 16));
        CHECK(world->resolve_block_id("test:goo", idOut, error));
        worldOut = std::move(world);
    };
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };

    // Pooled goo: evaporation=false -> the spread ring SURVIVES its unfed
    // step (the source has air below, so it can never feed the ring sideways;
    // an unfed cell also stops spreading, so the pool is exactly one ring).
    std::unique_ptr<engine::voxel::IVoxelWorld> pooled;
    uint32_t pooledId = 0;
    build(R"([{"block":"test:goo","viscosity":1.0,"range":2,"falling":false,"evaporation":false}])",
          pooledId, pooled, player);
    pooled->set_block(8, 130, 8, static_cast<uint32_t>(pooledId));
    CHECK(settle(*pooled, player, [&] { return pooled->get_fluid_level(9, 130, 8) == 1; }));
    CHECK(pooled->get_block(9, 130, 8) == static_cast<uint32_t>(pooledId));
    CHECK(pooled->get_block(10, 130, 8) == kBlockAir);  // unfed cells don't spread

    // Evaporating goo: same setup with evaporation=true -> the ring decays to
    // air once it is processed (only the source cell remains).
    std::unique_ptr<engine::voxel::IVoxelWorld> evaporating;
    uint32_t evaporatingId = 0;
    build(R"([{"block":"test:goo","viscosity":1.0,"range":2,"falling":false,"evaporation":true}])",
          evaporatingId, evaporating, player);
    evaporating->set_block(8, 130, 8, static_cast<uint32_t>(evaporatingId));
    CHECK(settle(*evaporating, player, [&] {
        return evaporating->get_block(9, 130, 8) == kBlockAir &&
               evaporating->get_block(10, 130, 8) == kBlockAir;
    }));
    CHECK(evaporating->get_block(8, 130, 8) == static_cast<uint32_t>(evaporatingId));

    std::cout << "[sdk] fluids: evaporation flag (pooled vs decayed) OK\n";
}

// Per-fluid cadence (tickInterval): a slow fluid steps every N world ticks, so
// it visibly lags a fast fluid with the same range and viscosity.
void test_fluid_tick_cadence() {
    auto world = engine::voxel::create_default_voxel_world();
    world->register_generator(std::make_shared<FlatGenerator>(96));
    auto blocks = std::make_shared<engine::registry::BlockRegistry>();
    std::string error;
    CHECK(blocks->load_from_json(
        R"([{"name":"goo","namespace":"test","class":"fluid","color":[0.2,0.9,0.3]},)"
        R"({"name":"ooze","namespace":"test","class":"fluid","color":[0.5,0.2,0.6]}])",
        error));
    world->set_block_registry(blocks);
    auto fluids = std::make_shared<engine::registry::FluidRegistry>();
    // Same range/viscosity; only the cadence differs (0.08 vs 0.64 seconds).
    CHECK(fluids->load_from_json(
        R"([{"block":"test:goo","viscosity":1.0,"range":4,"falling":false,"evaporation":false,"tickInterval":0.08},)"
        R"({"block":"test:ooze","viscosity":1.0,"range":4,"falling":false,"evaporation":false,"tickInterval":0.64}])",
        error));
    CHECK(world->set_fluid_registry(fluids, error));
    CHECK(error.empty());
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
    CHECK(boot_world(*world, player, 16));
    uint32_t gooId = 0, oozeId = 0;
    CHECK(world->resolve_block_id("test:goo", gooId, error));
    CHECK(world->resolve_block_id("test:ooze", oozeId, error));
    world->set_block(2, 128, 2, gooId);
    world->set_block(12, 128, 12, oozeId);

    // Fast goo reaches its full range (level 4 at distance 4: level = distance
    // for a 1-level/tick fluid)...
    CHECK(settle(*world, player, [&] { return world->get_fluid_level(6, 128, 2) == 4; }));
    // ...while the slow ooze (8x cadence) has barely moved.
    CHECK(world->get_fluid_level(13, 128, 12) < 4);
    // It catches up eventually: cadence gates speed, not reach.
    CHECK(settle(*world, player, [&] { return world->get_fluid_level(16, 128, 12) == 4; }));

    std::cout << "[sdk] fluids: per-fluid tick cadence (slow lags, catches up) OK\n";
}

// Fluid levels are part of the world save: a data-driven fluid's spread
// survives serialize/deserialize and re-serializes byte-identically.
void test_fluid_persistence() {
    const auto build = [&](uint32_t& idOut,
                           std::unique_ptr<engine::voxel::IVoxelWorld>& worldOut,
                           const glm::vec3& player) {
        auto world = engine::voxel::create_default_voxel_world();
        world->register_generator(std::make_shared<FlatGenerator>(96));
        auto blocks = std::make_shared<engine::registry::BlockRegistry>();
        std::string error;
        CHECK(blocks->load_from_json(
            R"([{"name":"goo","namespace":"test","class":"fluid","color":[0.2,0.9,0.3]}])",
            error));
        world->set_block_registry(blocks);
        auto fluids = std::make_shared<engine::registry::FluidRegistry>();
        CHECK(fluids->load_from_json(
            R"([{"block":"test:goo","viscosity":0.0,"range":4,"falling":false,"evaporation":false}])",
            error));
        CHECK(world->set_fluid_registry(fluids, error));
        CHECK(boot_world(*world, player, 16));
        CHECK(world->resolve_block_id("test:goo", idOut, error));
        worldOut = std::move(world);
    };
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };

    std::unique_ptr<engine::voxel::IVoxelWorld> a;
    uint32_t gooId = 0;
    build(gooId, a, player);
    a->set_block(8, 128, 8, static_cast<uint32_t>(gooId));
    CHECK(settle(*a, player, [&] { return a->get_fluid_level(10, 128, 8) == 4; }));
    std::string saveError;
    const std::string bytes = a->serialize_world(saveError);
    CHECK(saveError.empty());
    CHECK(a->serialize_world(saveError) == bytes);  // deterministic section

    std::unique_ptr<engine::voxel::IVoxelWorld> b;
    uint32_t gooIdB = 0;
    build(gooIdB, b, player);
    CHECK(gooIdB == gooId);  // same registry -> same dynamic id
    std::string loadError;
    CHECK(b->deserialize_world(bytes, loadError));
    CHECK(loadError.empty());
    CHECK(b->get_fluid_level(8, 128, 8) == 0);   // source restored
    CHECK(b->get_fluid_level(9, 128, 8) == 2);   // ring levels restored
    CHECK(b->get_fluid_level(10, 128, 8) == 4);
    CHECK(b->get_block(9, 128, 8) == static_cast<uint32_t>(gooIdB));
    std::string reError;
    CHECK(b->serialize_world(reError) == bytes);  // byte-identical roundtrip

    std::cout << "[sdk] fluids: levels persist through save/load, byte-identical "
                 "roundtrip OK\n";
}

void test_block_registry() {
    engine::registry::BlockRegistry blocks;
    CHECK(blocks.size() > 40);  // builtin table registered

    const engine::registry::BlockDefinition* stone =
        blocks.find_by_name("vulkancraft:stone");
    CHECK(stone != nullptr);
    CHECK(stone->builtinId == 3);
    CHECK(stone->hardness > 1.0f);
    CHECK(stone->uuid.size() == 36);

    // Stable derived id: same namespaced name -> same uuid in a fresh registry.
    const std::string stoneUuid = stone->uuid;
    engine::registry::BlockRegistry again;
    const engine::registry::BlockDefinition* stoneAgain =
        again.find_by_name("vulkancraft:stone");
    CHECK(stoneAgain != nullptr);
    CHECK(stoneAgain->uuid == stoneUuid);

    // JSON asset loads at runtime (no recompile): a project-defined block.
    const char* jsonAsset =
        R"([{"name":"ruby","namespace":"test","class":"solid","hardness":3.0,"lightEmission":0.4,"tags":["gem"],"drops":["test:ruby"]},)"
        R"({"name":"sapphire","namespace":"test","class":"solid","hardness":4.0}])";
    std::string error;
    CHECK(blocks.load_from_json(jsonAsset, error));
    const engine::registry::BlockDefinition* ruby = blocks.find_by_name("test:ruby");
    CHECK(ruby != nullptr);
    CHECK(ruby->hardness > 2.9f && ruby->hardness < 3.1f);
    CHECK(ruby->lightEmission > 0.3f);
    CHECK(ruby->tags.size() == 1 && ruby->tags[0] == "gem");

    // ID stability across load order: identical definitions in a different
    // order produce the same UUIDs (reorder-proof ids).
    engine::registry::BlockRegistry reordered;
    std::string errA;
    CHECK(reordered.load_from_json(
        R"([{"name":"sapphire","namespace":"test"},{"name":"ruby","namespace":"test","id":")" + ruby->uuid + "\"}]", errA));
    const engine::registry::BlockDefinition* rubyReordered =
        reordered.find_by_name("test:ruby");
    CHECK(rubyReordered != nullptr);
    CHECK(rubyReordered->uuid == ruby->uuid);

    // Invalid asset: clear diagnostic, nothing registered, safe fallback.
    engine::registry::BlockRegistry broken;
    std::string brokenError;
    const bool loaded = broken.load_from_json(R"({"name":""})", brokenError);
    CHECK(!loaded);
    CHECK(!brokenError.empty());
    CHECK(broken.find_by_name("test:ruby") == nullptr);
    CHECK(broken.find_by_uuid("00000000-0000-0000-0000-000000000000") == nullptr);
    CHECK(broken.fallback().name == "unknown");
    CHECK(!broken.fallback().collidable);

    std::cout << "[sdk] block registry: " << blocks.size()
              << " blocks, JSON load, id stability and fallback OK\n";
}

void test_item_registry() {
    engine::registry::ItemRegistry items;

    engine::registry::ItemDefinition pickaxe;
    pickaxe.ns = "vulkancraft";
    pickaxe.name = "stone_pickaxe";
    pickaxe.maxStack = 1;
    pickaxe.durability = 250;
    pickaxe.tags = { "tool", "pickaxe" };
    std::string error;
    CHECK(items.register_item(pickaxe, error));
    CHECK(error.empty());

    const engine::registry::ItemDefinition* found =
        items.find_by_name("vulkancraft:stone_pickaxe");
    CHECK(found != nullptr);
    CHECK(found->maxStack == 1);
    CHECK(found->durability == 250);
    CHECK(found->uuid.size() == 36);

    // Invalid maxStack is rejected with a diagnostic.
    engine::registry::ItemDefinition bad;
    bad.ns = "test";
    bad.name = "bad_stack";
    bad.maxStack = 100;
    std::string badError;
    CHECK(!items.register_item(bad, badError));
    CHECK(!badError.empty());

    // JSON load + stable derived id (same name in a fresh registry that also
    // loads the asset produces the same uuid — id stability across instances).
    engine::registry::ItemRegistry more;
    std::string jsonError;
    CHECK(more.load_from_json(
        R"({"name":"copper_ingot","namespace":"test","maxStack":64,"tags":["material"]})",
        jsonError));
    const engine::registry::ItemDefinition* ingot = more.find_by_name("test:copper_ingot");
    CHECK(ingot != nullptr);
    CHECK(ingot->maxStack == 64);
    engine::registry::ItemRegistry another;
    CHECK(another.load_from_json(
        R"({"name":"copper_ingot","namespace":"test"})", jsonError));
    const engine::registry::ItemDefinition* ingotAnother =
        another.find_by_name("test:copper_ingot");
    CHECK(ingotAnother != nullptr);
    CHECK(ingotAnother->uuid == ingot->uuid);

    CHECK(more.fallback().name == "unknown");

    std::cout << "[sdk] item registry: " << items.size()
              << " items, validation and fallback OK\n";
}

// META section 14: authoritative item stacks and slot inventories — typed
// filters, transfer/split/merge/swap, change events, lossless add (remainder)
// and data-driven serialization (namespaced ids, never numeric).
void test_item_stack_inventory() {
    using engine::registry::Inventory;
    using engine::registry::ItemStack;
    using engine::registry::SlotFilter;

    engine::registry::ItemRegistry items;
    std::string error;
    {
        engine::registry::ItemDefinition def;
        def.ns = "vulkancraft";
        def.name = "cobblestone";
        def.maxStack = 64;
        def.tags = { "stone", "material" };
        CHECK(items.register_item(def, error));
        def = {};
        def.ns = "vulkancraft";
        def.name = "stone_pickaxe";
        def.maxStack = 1;
        def.durability = 250;
        def.tags = { "tool", "pickaxe" };
        CHECK(items.register_item(def, error));
        def = {};
        def.ns = "vulkancraft";
        def.name = "coal";
        def.maxStack = 64;
        def.tags = { "fuel" };
        CHECK(items.register_item(def, error));
        def = {};
        def.ns = "vulkancraft";
        def.name = "iron_ingot";
        def.maxStack = 64;
        CHECK(items.register_item(def, error));
    }

    // ---- ItemStack semantics ----
    ItemStack a;
    a.item = "vulkancraft:cobblestone";
    a.count = 10;
    CHECK(a.capacity_left(64) == 54);
    CHECK(a.add(100, 64) == 54);
    CHECK(a.count == 64);
    ItemStack b = a.split(20);
    CHECK(b.count == 20);
    CHECK(a.count == 44);
    ItemStack damaged1, damaged2;
    damaged1.item = "vulkancraft:stone_pickaxe";
    damaged1.count = 1;
    damaged1.damage = 5;
    damaged2 = damaged1;
    CHECK(ItemStack::can_merge(damaged1, damaged2));
    damaged2.damage = 6;
    CHECK(!ItemStack::can_merge(damaged1, damaged2));

    // ---- Inventory: typed filters + authoritative set/add/remove ----
    Inventory inv(6);
    CHECK(inv.slot_count() == 6);
    const uint64_t v0 = inv.version();
    ItemStack stone;
    stone.item = "vulkancraft:cobblestone";
    stone.count = 1;
    CHECK(!inv.set(0, stone, items, error));  // slot 0 locked (no filter)
    CHECK(!error.empty());
    SlotFilter toolFilter;
    toolFilter.allowItems = { "vulkancraft:stone_pickaxe" };
    inv.set_filter(1, toolFilter);
    error.clear();
    CHECK(!inv.set(1, stone, items, error));  // cobblestone rejected by slot 1
    ItemStack pick;
    pick.item = "vulkancraft:stone_pickaxe";
    pick.count = 1;
    CHECK(inv.set(1, pick, items, error));
    SlotFilter tagFilter;
    tagFilter.allowTags = { "stone" };
    inv.set_filter(2, tagFilter);
    CHECK(inv.set(2, stone, items, error));  // accepted via tag expansion
    SlotFilter any;
    any.allowAny = true;
    for (int s = 3; s < 6; ++s) inv.set_filter(s, any);
    error.clear();

    // Lossless add: fits (slots 2..5, 64 each) -> no remainder.
    ItemStack big;
    big.item = "vulkancraft:cobblestone";
    big.count = 200;
    ItemStack rem = inv.add(big, items, error);
    CHECK(error.empty());
    CHECK(rem.empty());
    CHECK(inv.count_of("vulkancraft:cobblestone") == 201);
    CHECK(inv.version() > v0);

    // Overflow is returned as remainder — nothing is lost or truncated.
    big.count = 1000;
    rem = inv.add(big, items, error);
    CHECK(rem.count == 945);
    CHECK(inv.count_of("vulkancraft:cobblestone") == 256);

    // Unknown item refused, never guessed.
    ItemStack ghost;
    ghost.item = "vulkancraft:ghost";
    ghost.count = 1;
    error.clear();
    rem = inv.add(ghost, items, error);
    CHECK(!error.empty());
    CHECK(rem.count == 1);

    CHECK(inv.remove("vulkancraft:cobblestone", 10, items, error) == 10);
    CHECK(inv.count_of("vulkancraft:cobblestone") == 246);
    CHECK(inv.consume(2, 5) == 5);
    CHECK(inv.count_of("vulkancraft:cobblestone") == 241);

    // ---- Transfer (validation on both sides, partial move) ----
    Inventory src(4), dst(4);
    for (int s = 0; s < 4; ++s) {
        src.set_filter(s, any);
        dst.set_filter(s, any);
    }
    stone.count = 30;
    CHECK(src.set(0, stone, items, error));
    ItemStack coal;
    coal.item = "vulkancraft:coal";
    coal.count = 1;
    CHECK(dst.set(0, coal, items, error));
    error.clear();
    // Destination holds a different item -> authoritative refusal.
    CHECK(Inventory::transfer(src, 0, dst, 0, 10, items, error) == 0);
    CHECK(!error.empty());
    // Filter rejection on the destination.
    dst.set_filter(2, toolFilter);
    error.clear();
    CHECK(Inventory::transfer(src, 0, dst, 2, 5, items, error) == 0);
    CHECK(!error.empty());
    // Clean move.
    error.clear();
    CHECK(Inventory::transfer(src, 0, dst, 1, 10, items, error) == 10);
    CHECK(src.count_of("vulkancraft:cobblestone") == 20);
    CHECK(Inventory::transfer(src, 0, dst, 1, 20, items, error) == 20);
    CHECK(src.count_of("vulkancraft:cobblestone") == 0);

    // ---- Swap validates both filters ----
    ItemStack ingot;
    ingot.item = "vulkancraft:iron_ingot";
    ingot.count = 3;
    CHECK(src.set(1, ingot, items, error));
    CHECK(Inventory::swap(src, 0, 1, items, error));
    CHECK(src.get(0).item == "vulkancraft:iron_ingot");
    src.set_filter(2, toolFilter);
    error.clear();
    CHECK(!Inventory::swap(src, 2, 0, items, error));  // slot 2 rejects ingot
    CHECK(!error.empty());

    // ---- Change events: version + callback fire on success only ----
    Inventory eventsInv(3);  // slot 0 locked, slots 1-2 unrestricted
    for (int s = 1; s < 3; ++s) eventsInv.set_filter(s, any);
    int changes = 0;
    eventsInv.set_change_callback([&](const Inventory&) { ++changes; });
    const uint64_t before = eventsInv.version();
    coal.count = 1;
    CHECK(eventsInv.add(coal, items, error).empty());
    CHECK(changes == 1);
    CHECK(eventsInv.version() == before + 1);
    error.clear();
    CHECK(!eventsInv.set(0, coal, items, error));  // locked slot: no event
    CHECK(changes == 1);
    CHECK(eventsInv.version() == before + 1);

    // ---- Serialization: namespaced ids, damage + data round-trip ----
    Inventory saveInv(4);
    for (int s = 0; s < 4; ++s) saveInv.set_filter(s, any);
    pick.damage = 7;
    pick.data = "sharpness:2";
    CHECK(saveInv.set(0, pick, items, error));
    stone.count = 5;
    CHECK(saveInv.set(1, stone, items, error));
    const std::string json = saveInv.serialize_json();
    Inventory loaded(4);
    for (int s = 0; s < 4; ++s) loaded.set_filter(s, any);
    CHECK(loaded.deserialize_json(json, items, error));
    CHECK(loaded.get(0).damage == 7);
    CHECK(loaded.get(0).data == "sharpness:2");
    CHECK(loaded.get(1).count == 5);
    CHECK(loaded.serialize_json() == json);
    // Corrupt payloads are rejected all-or-nothing (version unchanged).
    const uint64_t vLoaded = loaded.version();
    error.clear();
    CHECK(!loaded.deserialize_json(
        R"({"slots":[{"item":"vulkancraft:ghost","count":1},null,null,null]})",
        items, error));
    CHECK(!error.empty());
    CHECK(loaded.version() == vLoaded);
    error.clear();
    CHECK(!loaded.deserialize_json(
        R"({"slots":[{"item":"vulkancraft:coal","count":100},null,null,null]})",
        items, error));
    CHECK(!error.empty());
    CHECK(loaded.version() == vLoaded);
    // Unknown format version is refused, never guessed.
    error.clear();
    CHECK(!loaded.deserialize_json(
        R"({"version":2,"slots":[null,null,null,null]})", items, error));
    CHECK(!error.empty());
    CHECK(loaded.version() == vLoaded);

    std::cout << "[sdk] inventory: typed filters, transfer/split/merge/swap, "
                 "events and JSON round-trip OK\n";
}

// META section 14: data-driven recipe graph — inputs (item/tag/alternatives),
// station/time/energy/fuel/conditions, outputs + byproducts, authoritative
// atomic craft and dependency edges with cycle detection.
void test_recipe_graph() {
    using engine::registry::CraftResult;
    using engine::registry::Inventory;
    using engine::registry::ItemStack;
    using engine::registry::SlotFilter;

    engine::registry::ItemRegistry items;
    std::string itemError;
    CHECK(items.load_from_json(
        R"([
          {"namespace":"vulkancraft","name":"cobblestone","maxStack":64,"tags":["stone"]},
          {"namespace":"vulkancraft","name":"stick","maxStack":64,"tags":["stick"]},
          {"namespace":"vulkancraft","name":"log","maxStack":64,"tags":["log"]},
          {"namespace":"vulkancraft","name":"plank","maxStack":64},
          {"namespace":"vulkancraft","name":"stone_pickaxe","maxStack":1,"durability":250},
          {"namespace":"vulkancraft","name":"iron_ore","maxStack":64},
          {"namespace":"vulkancraft","name":"iron_ingot","maxStack":64},
          {"namespace":"vulkancraft","name":"coal","maxStack":64,"tags":["fuel"]},
          {"namespace":"vulkancraft","name":"crafting_table","maxStack":64},
          {"namespace":"vulkancraft","name":"furnace","maxStack":64},
          {"namespace":"vulkancraft","name":"slag","maxStack":64},
          {"namespace":"vulkancraft","name":"gizmo","maxStack":64},
          {"namespace":"vulkancraft","name":"widget","maxStack":64}
        ])",
        itemError));
    CHECK(itemError.empty());

    engine::registry::RecipeRegistry recipes(&items);
    std::string recipeError;
    CHECK(recipes.load_from_json(
        R"({
          "namespace":"vulkancraft",
          "recipes":[
            {"name":"stone_pickaxe","station":"vulkancraft:crafting_table","time":2.0,
             "inputs":[{"item":"vulkancraft:cobblestone","count":3},{"tag":"stick","count":2}],
             "outputs":[{"item":"vulkancraft:stone_pickaxe","count":1}]},
            {"name":"plank_from_log","inputs":[{"item":"vulkancraft:log","count":1}],
             "outputs":[{"item":"vulkancraft:plank","count":4}]},
            {"name":"stick_from_plank","inputs":[{"item":"vulkancraft:plank","count":2}],
             "outputs":[{"item":"vulkancraft:stick","count":2}]},
            {"name":"smelt_iron","station":"vulkancraft:furnace","energy":1.0,
             "fuel":"vulkancraft:coal","conditions":["unlocked:iron_age"],
             "inputs":[{"item":"vulkancraft:iron_ore","count":1}],
             "outputs":[{"item":"vulkancraft:iron_ingot","count":1}],
             "byproducts":[{"item":"vulkancraft:slag","count":1,"chance":0.5}]}
          ]
        })",
        recipeError));
    CHECK(recipeError.empty());
    CHECK(recipes.size() == 4);

    const engine::registry::RecipeDefinition* pick =
        recipes.find_by_name("vulkancraft:stone_pickaxe");
    CHECK(pick != nullptr);
    CHECK(pick->station == "vulkancraft:crafting_table");
    CHECK(pick->time == 2.0);
    CHECK(pick->inputs.size() == 2);
    CHECK(pick->inputs[1].tag == "stick");
    CHECK(pick->outputs.size() == 1);
    CHECK(pick->uuid.size() == 36);
    CHECK(recipes.find_by_uuid(pick->uuid) != nullptr);
    CHECK(recipes.find_by_uuid(pick->uuid)->namespaced() == pick->namespaced());

    const engine::registry::RecipeDefinition* smelt =
        recipes.find_by_name("vulkancraft:smelt_iron");
    CHECK(smelt->energy == 1.0);
    CHECK(smelt->fuel == "vulkancraft:coal");
    CHECK(smelt->conditions.size() == 1);
    CHECK(smelt->outputs.size() == 2);
    CHECK(smelt->outputs[0].item == "vulkancraft:iron_ingot");
    CHECK(smelt->outputs[1].byproduct);
    CHECK(smelt->outputs[1].chance == 0.5);

    // Unknown item / unknown tag refused at registration (never guessed).
    engine::registry::RecipeRegistry strict(&items);
    std::string badError;
    CHECK(!strict.load_from_json(
        R"({"recipes":[{"name":"bad","inputs":[{"item":"vulkancraft:nope"}],"outputs":[{"item":"vulkancraft:coal"}]}]})",
        badError));
    CHECK(!badError.empty());
    badError.clear();
    CHECK(!strict.load_from_json(
        R"({"recipes":[{"name":"bad2","inputs":[{"tag":"ghost_tag"}],"outputs":[{"item":"vulkancraft:coal"}]}]})",
        badError));
    CHECK(!badError.empty());

    // ---- Craft: station gating, atomic consumption, outputs/byproducts ----
    SlotFilter any;
    any.allowAny = true;
    Inventory inv(9);
    for (int s = 0; s < 9; ++s) inv.set_filter(s, any);
    std::string error;
    ItemStack c;
    c.item = "vulkancraft:cobblestone";
    c.count = 3;
    CHECK(inv.add(c, items, error).empty());
    ItemStack st;
    st.item = "vulkancraft:stick";
    st.count = 2;
    CHECK(inv.add(st, items, error).empty());

    // Wrong station -> refused, nothing consumed.
    CraftResult noStation = recipes.craft(inv, *pick, "", items);
    CHECK(!noStation.ok);
    CHECK(!noStation.error.empty());
    CHECK(inv.count_of("vulkancraft:cobblestone") == 3);
    CHECK(inv.count_of("vulkancraft:stick") == 2);

    CraftResult crafted =
        recipes.craft(inv, *pick, "vulkancraft:crafting_table", items);
    CHECK(crafted.ok);
    CHECK(crafted.outputs.size() == 1);
    CHECK(crafted.outputs[0].item == "vulkancraft:stone_pickaxe");
    CHECK(crafted.time == 2.0);
    CHECK(inv.count_of("vulkancraft:cobblestone") == 0);
    CHECK(inv.count_of("vulkancraft:stick") == 0);

    // Insufficient input -> atomic failure (nothing consumed).
    ItemStack ore;
    ore.item = "vulkancraft:iron_ore";
    ore.count = 1;
    CHECK(inv.add(ore, items, error).empty());
    CraftResult smelted = recipes.craft(inv, *smelt, "vulkancraft:furnace", items);
    CHECK(smelted.ok);
    CHECK(smelted.outputs.size() == 1);
    CHECK(smelted.outputs[0].item == "vulkancraft:iron_ingot");
    CHECK(inv.count_of("vulkancraft:iron_ore") == 0);
    // Byproduct rolls are deterministic per seed: seed 1 includes slag,
    // seed 2 does not.
    ItemStack ore2;
    ore2.item = "vulkancraft:iron_ore";
    ore2.count = 1;
    CHECK(inv.add(ore2, items, error).empty());
    CraftResult withSlag = recipes.craft(inv, *smelt, "vulkancraft:furnace", items, 1);
    CHECK(withSlag.ok);
    CHECK(withSlag.byproducts.size() == 1);
    CHECK(withSlag.byproducts[0].item == "vulkancraft:slag");
    ItemStack ore3;
    ore3.item = "vulkancraft:iron_ore";
    ore3.count = 1;
    CHECK(inv.add(ore3, items, error).empty());
    CraftResult noSlag = recipes.craft(inv, *smelt, "vulkancraft:furnace", items, 2);
    CHECK(noSlag.ok);
    CHECK(noSlag.byproducts.empty());

    // ---- recipes_for: station filtering + tag/alternative expansion ----
    Inventory mats(9);
    for (int s = 0; s < 9; ++s) mats.set_filter(s, any);
    c.count = 3;
    CHECK(mats.add(c, items, error).empty());
    st.count = 2;
    CHECK(mats.add(st, items, error).empty());
    ItemStack log;
    log.item = "vulkancraft:log";
    log.count = 1;
    CHECK(mats.add(log, items, error).empty());
    const auto anywhere = recipes.recipes_for(mats, "", items);
    bool hasPlank = false, hasPick = false, hasStick = false, hasSmelt = false;
    for (const auto* r : anywhere) {
        if (r->name == "plank_from_log") hasPlank = true;
        if (r->name == "stone_pickaxe") hasPick = true;
        if (r->name == "stick_from_plank") hasStick = true;
        if (r->name == "smelt_iron") hasSmelt = true;
    }
    CHECK(hasPlank);
    CHECK(!hasStick);  // stick_from_plank needs 2 planks, which mats lacks
    CHECK(!hasPick);   // pickaxe requires the crafting table
    CHECK(!hasSmelt);  // smelt requires the furnace (and there is no ore here)

    // ---- Dependency edges + cycle detection ----
    const auto edges = recipes.dependency_edges();
    bool stickToPlank = false, pickToStick = false;
    for (const auto& [from, to] : edges) {
        if (from == "vulkancraft:stick_from_plank" &&
            to == "vulkancraft:plank_from_log")
            stickToPlank = true;
        if (from == "vulkancraft:stone_pickaxe" &&
            to == "vulkancraft:stick_from_plank")
            pickToStick = true;
    }
    CHECK(stickToPlank);
    CHECK(pickToStick);
    std::string cyclePath;
    CHECK(!recipes.has_cycle(cyclePath));
    CHECK(cyclePath.empty());

    engine::registry::RecipeRegistry cyclic(&items);
    CHECK(cyclic.load_from_json(
        R"({"recipes":[
          {"name":"gizmo_from_widget","inputs":[{"item":"vulkancraft:widget"}],"outputs":[{"item":"vulkancraft:gizmo"}]},
          {"name":"widget_from_gizmo","inputs":[{"item":"vulkancraft:gizmo"}],"outputs":[{"item":"vulkancraft:widget"}]}
        ]})",
        recipeError));
    CHECK(cyclic.has_cycle(cyclePath));
    CHECK(!cyclePath.empty());

    std::cout << "[sdk] recipe graph: " << recipes.size()
              << " recipes, station/time/energy/conditions, atomic craft, "
                 "byproducts and cycle detection OK\n";
}

// META section 15 / FALTANTES item 11: EnTT-backed entity world — generational
// handles with pooling, builtin + project components, headless tick with
// per-entity sleeping, chunk spatial index and versioned persistence.
void test_entity_world() {
    using engine::entity::ComponentData;
    using engine::entity::EntityId;
    using engine::entity::Health;
    using engine::entity::Position;

    std::unique_ptr<engine::entity::IEntityWorld> world =
        engine::entity::create_entity_world();
    CHECK(world != nullptr);

    // ---- Generational handles: stale handle never aliases a reused id ----
    std::string error;
    EntityId cow =
        world->spawn("vulkancraft:cow", Position{ 8.0f, 40.0f, 8.0f }, error);
    CHECK(cow.valid());
    CHECK(error.empty());
    CHECK(world->alive(cow));
    CHECK(world->type_of(cow) == "vulkancraft:cow");
    CHECK(world->size() == 1);
    EntityId zombie = world->spawn("vulkancraft:zombie",
                                   Position{ 100.0f, 40.0f, 100.0f }, error);
    CHECK(zombie.valid());
    CHECK(world->size() == 2);

    CHECK(world->despawn(cow));
    CHECK(!world->alive(cow));  // stale handle
    CHECK(world->size() == 1);
    // Re-spawn reuses the pooled id with a bumped generation.
    EntityId cow2 =
        world->spawn("vulkancraft:cow", Position{ 9.0f, 40.0f, 9.0f }, error);
    CHECK(cow2.valid());
    CHECK(cow2 != cow);
    CHECK(world->alive(cow2));
    CHECK(!world->alive(cow));  // still stale even though the id was reused
    error.clear();
    CHECK(!world->spawn("", Position{}, error).valid());
    CHECK(!error.empty());

    // ---- Builtin components ----
    Health health{ 10.0f, 20.0f };
    CHECK(world->set_health(cow2, health));
    Health out;
    CHECK(world->get_health(cow2, out));
    CHECK(out.value == 10.0f);
    CHECK(out.max == 20.0f);
    Position pos;
    CHECK(world->get_position(cow2, pos));
    CHECK(pos.x == 9.0f);
    CHECK(world->set_position(cow2, Position{ 200.0f, 40.0f, 200.0f }));
    CHECK(world->get_position(cow2, pos));
    CHECK(pos.x == 200.0f);
    // Stale/unknown handles refuse everything.
    Health ignored;
    CHECK(!world->get_health(cow, ignored));
    CHECK(!world->set_health(EntityId{ 9999, 0 }, health));
    CHECK(world->type_of(EntityId{ 9999, 0 }).empty());

    // ---- Project components (versioned opaque blobs) ----
    ComponentData comp;
    comp.type = "vulkancraft:attributes";
    comp.version = 3;
    comp.blob = "strength:2";
    CHECK(world->set_component(cow2, comp));
    ComponentData got;
    CHECK(world->get_component(cow2, "vulkancraft:attributes", got));
    CHECK(got.version == 3);
    CHECK(got.blob == "strength:2");
    CHECK(!world->get_component(cow2, "vulkancraft:none", got));

    // ---- Headless tick + sleeping (per-entity tick policy) ----
    std::unique_ptr<engine::entity::IEntityWorld> sim =
        engine::entity::create_entity_world();
    EntityId fast = sim->spawn("vulkancraft:fast", Position{ 0, 0, 0 }, error);
    EntityId slow = sim->spawn("vulkancraft:slow", Position{ 1, 0, 1 }, error);
    CHECK(sim->set_tick_interval(slow, 0.5f));
    int fastTicks = 0, slowTicks = 0;
    float slowDt = 0.0f;
    const auto tickAll = [&](float dt) {
        sim->tick(dt, [&](EntityId handle, float dtValue) {
            if (handle == slow) {
                ++slowTicks;
                slowDt = dtValue;
            } else {
                ++fastTicks;
            }
        });
    };
    tickAll(0.1f);
    tickAll(0.1f);
    tickAll(0.1f);  // slow accumulated 0.3 < 0.5
    CHECK(fastTicks == 3);
    CHECK(slowTicks == 0);
    CHECK(sim->sleeping_count() == 1);
    tickAll(0.1f);
    tickAll(0.1f);  // slow accumulated 0.5 >= 0.5 -> ticks once
    CHECK(fastTicks == 5);
    CHECK(slowTicks == 1);
    CHECK(slowDt > 0.49f && slowDt <= 0.51f);  // effective dt = accumulated

    // ---- Spatial index by chunk (16x16) ----
    std::unique_ptr<engine::entity::IEntityWorld> spatial =
        engine::entity::create_entity_world();
    EntityId e1 = spatial->spawn("vulkancraft:a", Position{ 8, 40, 8 }, error);
    EntityId e2 = spatial->spawn("vulkancraft:b", Position{ 15, 40, 15 }, error);
    EntityId e3 = spatial->spawn("vulkancraft:c", Position{ 20, 40, 20 }, error);
    CHECK(spatial->entities_in_chunk(0, 0).size() == 2);
    CHECK(spatial->entities_in_chunk(1, 1).size() == 1);
    CHECK(spatial->entities_in_chunk(2, 2).empty());
    CHECK(spatial->set_position(e2, Position{ 20, 40, 20 }));  // move chunk
    CHECK(spatial->entities_in_chunk(0, 0).size() == 1);
    CHECK(spatial->entities_in_chunk(1, 1).size() == 2);
    CHECK(spatial->entities_in_aabb(0, 0, 0, 24, 80, 24).size() == 3);
    CHECK(spatial->entities_in_aabb(19, 0, 19, 21, 80, 21).size() == 2);
    CHECK(spatial->despawn(e1));
    CHECK(spatial->entities_in_chunk(0, 0).empty());

    // ---- Persistence: versioned snapshots, all-or-nothing restore ----
    std::unique_ptr<engine::entity::IEntityWorld> save0 =
        engine::entity::create_entity_world();
    EntityId pa = save0->spawn("vulkancraft:cow", Position{ 8, 40, 8 }, error);
    CHECK(save0->set_health(pa, Health{ 5.0f, 20.0f }));
    CHECK(save0->set_tick_interval(pa, 0.25f));
    comp.type = "vulkancraft:attributes";
    comp.version = 2;
    comp.blob = "speed:3";
    CHECK(save0->set_component(pa, comp));
    CHECK(save0->spawn("vulkancraft:zombie", Position{ 100, 40, 100 }, error).valid());
    const auto snapshots = save0->serialize_entities();
    CHECK(snapshots.size() == 2);

    std::unique_ptr<engine::entity::IEntityWorld> load0 =
        engine::entity::create_entity_world();
    CHECK(load0->deserialize_entities(snapshots, error));
    CHECK(load0->size() == 2);
    const auto reloaded = load0->serialize_entities();
    CHECK(reloaded.size() == 2);
    CHECK(reloaded[0].type == "vulkancraft:cow");
    CHECK(reloaded[0].position.x == 8.0f);
    CHECK(reloaded[0].health.value == 5.0f);
    CHECK(reloaded[0].tickInterval == 0.25f);
    CHECK(reloaded[0].components.size() == 1);
    CHECK(reloaded[0].components[0].blob == "speed:3");
    CHECK(reloaded[1].type == "vulkancraft:zombie");

    // All-or-nothing: a bad snapshot leaves the population untouched.
    std::vector<engine::entity::EntitySnapshot> bad = snapshots;
    bad[0].type.clear();
    error.clear();
    CHECK(!load0->deserialize_entities(bad, error));
    CHECK(!error.empty());
    CHECK(load0->size() == 2);

    std::cout << "[sdk] entity world: generational handles, tick/sleeping, "
                 "chunk index and versioned persistence OK\n";
}

// META section 15: the world's entity layer persists through the world save
// (v5 world_entities) — spawn entities, save, load into a fresh world, and the
// population (health, components, position) comes back intact.
void test_entity_world_save() {
    std::unique_ptr<engine::voxel::IVoxelWorld> a =
        engine::voxel::create_default_voxel_world();
    a->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*a, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    auto entities = a->entity_world();
    CHECK(entities != nullptr);
    std::string error;
    engine::entity::EntityId cow = entities->spawn(
        "vulkancraft:cow", engine::entity::Position{ 8.0f, 100.0f, 8.0f }, error);
    CHECK(cow.valid());
    CHECK(entities->set_health(cow, engine::entity::Health{ 7.0f, 20.0f }));
    engine::entity::ComponentData comp;
    comp.type = "vulkancraft:loot";
    comp.version = 1;
    comp.blob = "leather";
    CHECK(entities->set_component(cow, comp));

    std::string serError;
    const std::string blob = a->serialize_world(serError);
    CHECK(serError.empty());

    std::unique_ptr<engine::voxel::IVoxelWorld> b =
        engine::voxel::create_default_voxel_world();
    b->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*b, glm::vec3(8.0f, 200.0f, 8.0f), 16));
    std::string bError;
    CHECK(b->deserialize_world(blob, bError));
    CHECK(bError.empty());
    auto bEntities = b->entity_world();
    CHECK(bEntities != nullptr);
    CHECK(bEntities->size() == 1);
    const auto inChunk = bEntities->entities_in_chunk(0, 0);
    CHECK(inChunk.size() == 1);
    engine::entity::Health health;
    CHECK(bEntities->get_health(inChunk[0], health));
    CHECK(health.value == 7.0f);
    engine::entity::ComponentData got;
    CHECK(bEntities->get_component(inChunk[0], "vulkancraft:loot", got));
    CHECK(got.blob == "leather");

    // Idempotency with entities: a second serialization is byte-identical.
    std::string bSer;
    const std::string blobB = b->serialize_world(bSer);
    CHECK(bSer.empty());
    CHECK(blobB == blob);

    std::cout << "[sdk] entity world save: v5 world_entities round-trip "
                 "and idempotency OK\n";
}

// META section 16 / FALTANTES item 12: Recast/Detour navigation provider —
// bake from voxel columns, path around walls, maxClimb steps, revision
// invalidation and determinism.
void test_navigation_provider() {
    using engine::navigation::NavmeshConfig;
    using engine::navigation::PathResult;
    using engine::navigation::VoxelColumn;

    std::cerr << "[nav-probe] provider create\n";
    std::unique_ptr<engine::navigation::INavigationProvider> nav =
        engine::navigation::create_recast_navigation_provider();
    std::cerr << "[nav-probe] provider ok\n";
    CHECK(nav != nullptr);
    CHECK(!nav->valid());

    const float step = 0.5f;
    const float minCoord = 0.0f, maxCoord = 32.0f;

    const auto ground_columns = [&]() {
        std::vector<VoxelColumn> columns;
        for (float x = minCoord; x <= maxCoord; x += step) {
            for (float z = minCoord; z <= maxCoord; z += step) {
                VoxelColumn column;
                column.x = x;
                column.z = z;
                column.solidMinY = 0.0f;
                column.solidMaxY = 1.0f;
                column.solid = true;
                columns.push_back(column);
            }
        }
        return columns;
    };
    const auto wall_columns = [&](std::vector<VoxelColumn> columns) {
        // A 3-block-high wall at x = 16, z in [8, 24] (walkable top at y=3).
        for (VoxelColumn& column : columns) {
            if (std::fabs(column.x - 16.0f) <= step / 2.0f &&
                column.z >= 8.0f && column.z <= 24.0f) {
                column.solidMinY = 0.0f;
                column.solidMaxY = 3.0f;
            }
        }
        return columns;
    };

    NavmeshConfig config;
    config.boundsMinX = minCoord;
    config.boundsMaxX = maxCoord;
    config.boundsMinZ = minCoord;
    config.boundsMaxZ = maxCoord;
    config.cellSize = step;

    // ---- Flat terrain: straight path, walkability, revision ----
    std::string error;
    std::cerr << "[nav-probe] build start (columns="
              << ground_columns().size() << ")\n";
    CHECK(nav->build(config, ground_columns(), error));
    std::cerr << "[nav-probe] build done rev=" << nav->revision() << "\n";
    CHECK(error.empty());
    CHECK(nav->valid());
    const uint64_t r0 = nav->revision();
    CHECK(r0 > 0);
    PathResult flat;
    CHECK(nav->find_path(2.0f, 1.5f, 2.0f, 30.0f, 1.5f, 30.0f, flat));
    {
        std::cerr << "[nav-probe] flat: found=" << flat.found
                  << " len=" << flat.totalLength << " wp=" << flat.waypoints.size()
                  << "\n";
        for (std::size_t i = 0; i + 2 < flat.waypoints.size(); i += 3) {
            std::cerr << "  p" << i / 3 << "=(" << flat.waypoints[i] << ","
                      << flat.waypoints[i + 1] << "," << flat.waypoints[i + 2]
                      << ")\n";
        }
        std::cerr << "[nav-probe] walkable(8,1.5,8)=" << nav->is_walkable(8.0f, 1.5f, 8.0f)
                  << " walkable(8,12,8)=" << nav->is_walkable(8.0f, 12.0f, 8.0f) << "\n";
        std::cerr << "[nav-probe] first wp=(" << flat.waypoints[0] << ","
                  << flat.waypoints[1] << "," << flat.waypoints[2] << ") last=("
                  << flat.waypoints[flat.waypoints.size() - 3] << ","
                  << flat.waypoints[flat.waypoints.size() - 2] << ","
                  << flat.waypoints[flat.waypoints.size() - 1] << ")\n";
    }
    CHECK(flat.found);
    const float straight = std::sqrt(28.0f * 28.0f * 2.0f);
    CHECK(flat.totalLength > straight - 1.0f);
    CHECK(flat.totalLength < straight + 3.0f);
    CHECK(flat.revision == r0);
    CHECK(flat.waypoints.size() >= 6);  // start + end + intermediate points
    CHECK(nav->is_walkable(8.0f, 1.5f, 8.0f));
    CHECK(!nav->is_walkable(8.0f, 12.0f, 8.0f));  // high above the surface
    // Rebuild bumps the revision (consumers invalidate cached paths).
    CHECK(nav->build(config, ground_columns(), error));
    CHECK(nav->revision() > r0);

    // ---- Wall: the path must deviate around it ----
    CHECK(nav->build(config, wall_columns(ground_columns()), error));
    CHECK(error.empty());
    PathResult around;
    CHECK(nav->find_path(8.0f, 1.5f, 8.0f, 24.0f, 1.5f, 8.0f, around));
    std::cerr << "[nav-probe] around: found=" << around.found
              << " len=" << around.totalLength << " wp=" << around.waypoints.size()
              << "\n";
    for (std::size_t i = 0; i + 2 < around.waypoints.size(); i += 3) {
        std::cerr << "  a" << i / 3 << "=(" << around.waypoints[i] << ","
                  << around.waypoints[i + 1] << "," << around.waypoints[i + 2]
                  << ")\n";
    }
    CHECK(around.found);
    CHECK(around.totalLength > 16.0f + 1.5f);  // longer than the straight 16

    // ---- maxClimb: a 1-block step is climbable at 1.0, blocked at 0.2 ----
    const auto stepped_columns = [&]() {
        std::vector<VoxelColumn> columns = ground_columns();
        for (VoxelColumn& column : columns) {
            if (column.x >= 20.0f && column.x <= 28.0f &&
                column.z >= 20.0f && column.z <= 28.0f) {
                // 1-block-high platform (walkable surface at y=2).
                column.solidMinY = 1.0f;
                column.solidMaxY = 2.0f;
            }
        }
        return columns;
    };
    NavmeshConfig climbConfig = config;
    climbConfig.agentMaxClimb = 1.0f;
    CHECK(nav->build(climbConfig, stepped_columns(), error));
    PathResult ontoPlatform;
    CHECK(nav->find_path(10.0f, 1.5f, 10.0f, 24.0f, 2.5f, 24.0f,
                         ontoPlatform));
    std::cerr << "[nav-probe] climb1: found=" << ontoPlatform.found
              << " len=" << ontoPlatform.totalLength << " wp="
              << ontoPlatform.waypoints.size() << "\n";
    for (std::size_t i = 0; i + 2 < ontoPlatform.waypoints.size(); i += 3) {
        std::cerr << "  c" << i / 3 << "=(" << ontoPlatform.waypoints[i] << ","
                  << ontoPlatform.waypoints[i + 1] << "," << ontoPlatform.waypoints[i + 2]
                  << ")\n";
    }
    CHECK(ontoPlatform.found);
    NavmeshConfig noClimb = climbConfig;
    noClimb.agentMaxClimb = 0.2f;
    CHECK(nav->build(noClimb, stepped_columns(), error));
    PathResult blockedStep;
    const bool blockedCall = nav->find_path(10.0f, 1.5f, 10.0f, 24.0f, 2.5f,
                                            24.0f, blockedStep);
    std::cerr << "[nav-probe] noClimb: call=" << blockedCall
              << " found=" << blockedStep.found << " len=" << blockedStep.totalLength
              << " wp=" << blockedStep.waypoints.size() << "\n";
    for (std::size_t i = 0; i + 2 < blockedStep.waypoints.size(); i += 3) {
        std::cerr << "  b" << i / 3 << "=(" << blockedStep.waypoints[i] << ","
                  << blockedStep.waypoints[i + 1] << "," << blockedStep.waypoints[i + 2]
                  << ")\n";
    }
    CHECK(!blockedCall);
    CHECK(!blockedStep.found);

    // ---- Determinism: same build + query -> same path ----
    CHECK(nav->build(config, ground_columns(), error));
    PathResult first, second;
    CHECK(nav->find_path(2.0f, 1.5f, 2.0f, 30.0f, 1.5f, 30.0f, first));
    CHECK(nav->find_path(2.0f, 1.5f, 2.0f, 30.0f, 1.5f, 30.0f, second));
    CHECK(first.totalLength == second.totalLength);

    std::cout << "[sdk] navigation: bake, wall detour, maxClimb, revision "
                 "and determinism OK\n";
}

// META section 16: navigation integrated with a real voxel world — the sampler
// reads the world's walkable surface and the navmesh routes around a wall the
// test builds with normal block edits.
void test_navigation_voxel_world() {
    using engine::navigation::NavmeshConfig;
    using engine::navigation::PathResult;

    const auto surface_y = [](engine::voxel::IVoxelWorld& world, int x, int z) {
        for (int y = 120; y >= 0; --y) {
            if (world.get_block(x, y, z) != 0) return y;
        }
        return -1;
    };

    NavmeshConfig config;
    config.boundsMinX = 0.0f;
    config.boundsMaxX = 32.0f;
    config.boundsMinZ = 0.0f;
    config.boundsMaxZ = 32.0f;
    config.boundsMinY = 0.0f;
    config.boundsMaxY = 110.0f;
    config.cellSize = 0.5f;

    // World A: open terrain.
    std::unique_ptr<engine::voxel::IVoxelWorld> open =
        engine::voxel::create_default_voxel_world();
    open->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*open, glm::vec3(16.0f, 200.0f, 16.0f), 24));
    const int surface = surface_y(*open, 8, 8);
    CHECK(surface > 0);
    std::string error;
    auto columns =
        engine::navigation::sample_voxel_columns(*open, config, error);
    CHECK(error.empty());
    CHECK(columns.size() > 1000);
    std::unique_ptr<engine::navigation::INavigationProvider> nav =
        engine::navigation::create_recast_navigation_provider();
    CHECK(nav->build(config, columns, error));
    CHECK(error.empty());
    const float agentY = static_cast<float>(surface) + 1.5f;
    PathResult openPath;
    CHECK(nav->find_path(8.0f, agentY, 8.0f, 24.0f, agentY, 8.0f, openPath));
    CHECK(openPath.found);
    CHECK(openPath.totalLength < 17.5f);  // essentially straight (16)

    // World B: same terrain plus a 3-block stone wall across the path.
    std::unique_ptr<engine::voxel::IVoxelWorld> walled =
        engine::voxel::create_default_voxel_world();
    walled->register_generator(std::make_shared<FlatGenerator>(96));
    CHECK(boot_world(*walled, glm::vec3(16.0f, 200.0f, 16.0f), 24));
    for (int z = 8; z <= 24; ++z) {
        for (int y = surface + 1; y <= surface + 3; ++y) {
            walled->set_block(16, y, z, kBlockStone);
        }
    }
    const auto wallColumns =
        engine::navigation::sample_voxel_columns(*walled, config, error);
    CHECK(error.empty());
    CHECK(nav->build(config, wallColumns, error));
    CHECK(error.empty());
    PathResult walledPath;
    CHECK(nav->find_path(8.0f, agentY, 8.0f, 24.0f, agentY, 8.0f, walledPath));
    CHECK(walledPath.found);
    CHECK(walledPath.totalLength > openPath.totalLength + 1.5f);  // detour

    std::cout << "[sdk] navigation voxel: sampler + wall detour on a real "
                 "world OK\n";
}

}  // namespace

int main() {
    try {
        test_version();
        test_world_headless();
        test_transactions();
        test_persistence();
        test_compression_provider();
        test_hash_provider();
        test_save_v4_legacy();
        test_rocksdb_storage();
        test_storage_service();
        test_world_registry_source_of_truth();
        test_dynamic_block_persistence();
        test_block_entity_lifecycle();
        test_block_entity_atomic_destroy();
        test_block_entity_persistence();
        test_light_skylight();
        test_light_block_emitter();
        test_light_chunk_boundary();
        test_light_determinism();
        test_fluid_registry();
        test_fluid_generalized();
        test_fluid_evaporation();
        test_fluid_tick_cadence();
        test_fluid_persistence();
        test_block_registry();
        test_item_registry();
        test_item_stack_inventory();
        test_recipe_graph();
        test_entity_world();
        test_entity_world_save();
        test_navigation_provider();
        test_navigation_voxel_world();
    } catch (const std::exception& e) {
        std::cerr << "[voxel_sdk_tests] uncaught exception: " << e.what()
                  << "\n";
        return EXIT_FAILURE;
    } catch (...) {
        std::cerr << "[voxel_sdk_tests] uncaught unknown exception\n";
        return EXIT_FAILURE;
    }

    if (g_failures != 0) {
        std::cerr << "[voxel_sdk_tests] " << g_failures << " failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "[voxel_sdk_tests] all checks passed\n";
    return EXIT_SUCCESS;
}
