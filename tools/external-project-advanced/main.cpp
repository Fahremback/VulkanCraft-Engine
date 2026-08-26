// External "advanced" consumer smoke (FALTANTES item 11 / S24 -- matrix): a
// project that composes the higher-level public contracts -- multiple worlds
// with independent state, a portal mapping between them (IWorldManager), and
// voxel destruction (IGameplayRuntime). Compiles and links ONLY against the
// installed SDK; no engine-tree reference anywhere.
//
// Exit code 0 + "advanced-consumer-ok" markers = the installed SDK is
// self-sufficient for an advanced composition.
//
// Reference: tests/VoxelSdkTests.cpp test_world_manager (portal offset (5,3,0)
// rotated 90 deg -> (0,3,5) -> target (1000, 73, 1005)) and the gameplay
// destruction scenario in the representative consumer.

#include <engine/entity/IEntityWorld.hpp>
#include <engine/gameplay/IGameplayRuntime.hpp>
#include <engine/voxel/IVoxelWorld.hpp>
#include <engine/world/IWorldManager.hpp>
#include <engine/registry/BlockRegistry.hpp>
#include <engine/procgen/IWorldProfile.hpp>
#include <engine/procgen/IStructurePlacement.hpp>
#include <engine/registry/ItemRegistry.hpp>
#include <engine/registry/Inventory.hpp>
#include <engine/registry/RecipeRegistry.hpp>
#include <engine/voxel/IVoxelBlockEntity.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "advanced consumer failure: " #condition "\n"; return 1; } } while (false)

namespace {

constexpr uint32_t kAir = 0;
constexpr uint32_t kStone = 3;

bool boot_world(engine::voxel::IVoxelWorld* world, const glm::vec3& player) {
    world->set_chunk_budget(16);
    const auto start = std::chrono::steady_clock::now();
    while (!world->is_chunk_loaded(0, 0)) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count() > 20000) {
            return false;
        }
        world->update(player, 1.0f / 60.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

// --- Block entities: register, attach, tick, serialize ---
class SimpleCounterEntity : public engine::voxel::IVoxelBlockEntity {
public:
    std::string type_id() const override { return "ext:counter"; }
    void on_tick(uint64_t) override { ++count_; }
    uint32_t data_version() const override { return 1; }
    std::vector<uint8_t> serialize_state() const override {
        std::vector<uint8_t> blob(4);
        blob[0] = static_cast<uint8_t>(count_ & 0xFF);
        blob[1] = static_cast<uint8_t>((count_ >> 8) & 0xFF);
        blob[2] = static_cast<uint8_t>((count_ >> 16) & 0xFF);
        blob[3] = static_cast<uint8_t>((count_ >> 24) & 0xFF);
        return blob;
    }
    bool deserialize_state(const std::vector<uint8_t>& data, uint32_t) override {
        if (data.size() < 4) return false;
        count_ = static_cast<uint32_t>(data[0])
               | (static_cast<uint32_t>(data[1]) << 8)
               | (static_cast<uint32_t>(data[2]) << 16)
               | (static_cast<uint32_t>(data[3]) << 24);
        return true;
    }
    uint32_t count() const { return count_; }
private:
    uint32_t count_ = 0;
};

int test_worlds_and_portal() {
    using namespace engine::world;

    std::unique_ptr<IWorldManager> manager = create_world_manager();
    CHECK(manager != nullptr);

    std::string error;
    WorldSpec overworld;
    overworld.name = "overworld";
    overworld.seed = 12345;
    CHECK(manager->create_world(overworld, error));
    WorldSpec nether;
    nether.name = "nether";
    nether.seed = 99999;
    CHECK(manager->create_world(nether, error));
    CHECK(manager->world_count() == 2);

    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
    engine::voxel::IVoxelWorld* over = manager->world("overworld");
    engine::voxel::IVoxelWorld* neh = manager->world("nether");
    CHECK(over != nullptr && neh != nullptr);
    CHECK(boot_world(over, player));
    CHECK(boot_world(neh, player));

    // Worlds are independent: editing overworld must not touch nether.
    const uint32_t netherBefore = neh->get_block(4, 130, 4);
    over->set_block(4, 130, 4, kStone);
    CHECK(over->get_block(4, 130, 4) == kStone);
    CHECK(neh->get_block(4, 130, 4) == netherBefore);
    std::cout << "advanced-consumer-ok worlds\n";

    // Portal: overworld (100,64,100) -> nether (1000,70,1000), yaw 90.
    PortalSpec portal;
    portal.fromWorld = "overworld";
    portal.fromX = 100.0f;
    portal.fromY = 64.0f;
    portal.fromZ = 100.0f;
    portal.toWorld = "nether";
    portal.toX = 1000.0f;
    portal.toY = 70.0f;
    portal.toZ = 1000.0f;
    portal.yawDegrees = 90.0f;
    const uint32_t portalId = manager->create_portal(portal, error);
    CHECK(portalId != 0);
    CHECK(manager->portals().size() == 1);

    // Cross: local offset (5,3,0) rotated 90 -> (0,3,5) -> (1000,73,1005).
    auto overEntities = over->entity_world();
    CHECK(overEntities != nullptr);
    const engine::entity::EntityId wanderer = overEntities->spawn(
        "vulkancraft:player", engine::entity::Position{ 105.0f, 67.0f, 100.0f },
        error);
    CHECK(wanderer.valid());
    const engine::entity::EntityId crossed =
        manager->transfer_via_portal("overworld", wanderer, portalId, error);
    CHECK(crossed.valid());
    CHECK(!overEntities->alive(wanderer));
    auto nehEntities = neh->entity_world();
    CHECK(nehEntities != nullptr);
    engine::entity::Position moved;
    CHECK(nehEntities->get_position(crossed, moved));
    CHECK(std::abs(moved.x - 1000.0f) < 1e-3f);
    CHECK(std::abs(moved.y - 73.0f) < 1e-3f);
    CHECK(std::abs(moved.z - 1005.0f) < 1e-3f);
    CHECK(nehEntities->alive(crossed));
    std::cout << "advanced-consumer-ok portal\n";
    return 0;
}

int test_gameplay() {
    using namespace engine::gameplay;
    auto runtime = create_gameplay_runtime();
    CHECK(runtime != nullptr);

    DestructionSpec spec;
    spec.position = { 0.0f, 0.0f, 0.0f };
    for (int i = 0; i < 4; ++i) {
        DestructionChunk chunk;
        chunk.localPosition =
            glm::vec3((i % 2 == 0) ? -0.75f : 0.75f,
                      (i < 2) ? -0.75f : 0.75f, 0.0f);
        chunk.halfExtents = { 0.25f, 0.25f, 0.25f };
        chunk.health = 25.0f;
        spec.chunks.push_back(chunk);
    }
    auto destructible = runtime->create_destruction(spec);
    CHECK(destructible != nullptr);
    CHECK(destructible->chunk_count() == 4);
    for (int i = 0; i < 60; ++i) runtime->step(1.0f / 60.0f);
    const auto events = destructible->apply_radial_damage(
        { 0.0f, 0.0f, 0.0f }, 3.0f, 100.0f, 5.0f);
    CHECK(!events.empty());
    CHECK(destructible->fully_destroyed());
    std::size_t detached = 0;
    for (std::size_t i = 0; i < destructible->chunk_count(); ++i) {
        if (destructible->chunk_detached(i)) ++detached;
        CHECK(destructible->chunk_body(i).valid());
    }
    CHECK(detached == 4);
    std::cout << "advanced-consumer-ok gameplay\n";
    return 0;
}

int test_block_entities() {
    using namespace engine::voxel;
    auto world = create_default_voxel_world();
    CHECK(world != nullptr);
    world->register_block_entity_type("ext:counter",
        [] { return std::make_shared<SimpleCounterEntity>(); });

    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
    CHECK(boot_world(world.get(), player));

    // Place a block and attach an entity.
    world->set_block(4, 130, 4, kStone);
    auto entity = std::make_shared<SimpleCounterEntity>();
    std::string ae;
    CHECK(world->attach_block_entity(4, 130, 4, entity, ae));
    auto retrieved = world->block_entity_at(4, 130, 4);
    CHECK(retrieved != nullptr);
    CHECK(retrieved->type_id() == "ext:counter");

    // Tick the entity a few times.
    for (int i = 0; i < 5; ++i) {
        world->update(player, 1.0f / 60.0f);
    }
    auto afterTick = world->block_entity_at(4, 130, 4);
    CHECK(afterTick != nullptr);
    const auto blob = afterTick->serialize_state();
    CHECK(blob.size() == 4);
    uint32_t count = static_cast<uint32_t>(blob[0])
                   | (static_cast<uint32_t>(blob[1]) << 8)
                   | (static_cast<uint32_t>(blob[2]) << 16)
                   | (static_cast<uint32_t>(blob[3]) << 24);
    CHECK(count > 0);

    std::cout << "advanced-consumer-ok block-entities\n";
    return 0;
}

int test_save_load_roundtrip() {
    using namespace engine::voxel;
    namespace fs = std::filesystem;

    const std::string savePath = (fs::temp_directory_path() / "vc_ext_advanced_save.vcwld").string();

    // Create world A, edit blocks, save.
    {
        auto a = create_default_voxel_world();
        CHECK(a != nullptr);
        const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
        CHECK(boot_world(a.get(), player));
        a->set_block(4, 130, 4, kStone);
        a->set_block(5, 130, 4, kStone);
        a->set_block(6, 130, 4, kStone);
        std::string error;
        if (!a->save_world(savePath, error)) {
            std::cerr << "save failed: " << error << "\n";
            return 1;
        }
    }

    // Create world B, load, verify.
    {
        auto b = create_default_voxel_world();
        CHECK(b != nullptr);
        const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
        CHECK(boot_world(b.get(), player));
        std::string error;
        if (!b->load_world(savePath, error)) {
            std::cerr << "load failed: " << error << "\n";
            return 1;
        }
        CHECK(b->get_block(4, 130, 4) == kStone);
        CHECK(b->get_block(5, 130, 4) == kStone);
        CHECK(b->get_block(6, 130, 4) == kStone);
    }

    fs::remove_all(savePath);
    std::cout << "advanced-consumer-ok save-load\n";
    return 0;
}

int test_per_voxel_state() {
    using namespace engine::voxel;

    auto world = create_default_voxel_world();
    CHECK(world != nullptr);
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
    CHECK(boot_world(world.get(), player));

    // Place Air then Stone to guarantee the block change resets state to 0.
    world->set_block(4, 130, 4, kAir);
    world->set_block(4, 130, 4, kStone);
    CHECK(world->get_block_state(4, 130, 4) == 0);
    world->set_block_state(4, 130, 4, 3);
    CHECK(world->get_block_state(4, 130, 4) == 3);

    // State 7 is max (3-bit).
    world->set_block_state(4, 130, 4, 7);
    CHECK(world->get_block_state(4, 130, 4) == 7);
    // Set state back to 0.
    world->set_block_state(4, 130, 4, 0);
    CHECK(world->get_block_state(4, 130, 4) == 0);
    // Changing block to Air resets state.
    world->set_block(4, 130, 4, kAir);
    CHECK(world->get_block_state(4, 130, 4) == 0);

    std::cout << "advanced-consumer-ok per-voxel-state\n";
    return 0;
}

// --- Block and item registries from JSON (data-driven assets) ---
int test_registries() {
    // Block registry: load builtins + add project-defined blocks from JSON.
    engine::registry::BlockRegistry blocks;
    CHECK(blocks.size() > 40);  // builtins already registered

    const engine::registry::BlockDefinition* stone = blocks.find_by_name("vulkancraft:stone");
    CHECK(stone != nullptr);
    CHECK(stone->builtinId == 3);

    std::string error;
    CHECK(blocks.load_from_json(
        R"([{"name":"ruby","namespace":"test","class":"solid","hardness":3.0,"lightEmission":0.4,"tags":["gem"],"drops":["test:ruby"],"faceTop":[0.2,0.8,0.2]}])",
        error));
    CHECK(error.empty());
    const engine::registry::BlockDefinition* ruby = blocks.find_by_name("test:ruby");
    CHECK(ruby != nullptr);
    CHECK(ruby->hardness > 2.9f && ruby->hardness < 3.1f);
    CHECK(ruby->lightEmission > 0.3f);
    CHECK(ruby->tags.size() == 1 && ruby->tags[0] == "gem");

    // Item registry: load from JSON.
    engine::registry::ItemRegistry items;
    CHECK(items.load_from_json(
        R"([
          {"namespace":"test","name":"ruby","maxStack":64,"tags":["gem"]},
          {"namespace":"test","name":"iron_ingot","maxStack":64},
          {"namespace":"test","name":"pickaxe","maxStack":1,"durability":250,"tags":["tool","pickaxe"]}
        ])",
        error));
    CHECK(error.empty());
    CHECK(items.size() == 3);
    const engine::registry::ItemDefinition* rubyItem = items.find_by_name("test:ruby");
    CHECK(rubyItem != nullptr);
    CHECK(rubyItem->maxStack == 64);
    CHECK(rubyItem->tags.size() == 1 && rubyItem->tags[0] == "gem");

    std::cout << "advanced-consumer-ok registries\n";
    return 0;
}

// --- Inventory and crafting (authoritative, atomic, data-driven) ---
int test_inventory_crafting() {
    using engine::registry::Inventory;
    using engine::registry::ItemStack;
    using engine::registry::SlotFilter;

    // Load items from JSON.
    engine::registry::ItemRegistry items;
    std::string error;
    CHECK(items.load_from_json(
        R"([
          {"namespace":"test","name":"cobblestone","maxStack":64,"tags":["stone"]},
          {"namespace":"test","name":"stick","maxStack":64,"tags":["stick"]},
          {"namespace":"test","name":"log","maxStack":64},
          {"namespace":"test","name":"plank","maxStack":64},
          {"namespace":"test","name":"iron_pickaxe","maxStack":1,"durability":250}
        ])",
        error));
    CHECK(error.empty());

    // Load recipes from JSON.
    engine::registry::RecipeRegistry recipes(&items);
    CHECK(recipes.load_from_json(
        R"({
          "namespace":"test",
          "recipes":[
            {"name":"plank_from_log","inputs":[{"item":"test:log","count":1}],
             "outputs":[{"item":"test:plank","count":4}]},
            {"name":"stick_from_plank","inputs":[{"item":"test:plank","count":2}],
             "outputs":[{"item":"test:stick","count":4}]},
            {"name":"pickaxe","station":"test:crafting_table",
             "inputs":[{"item":"test:stick","count":2},{"item":"test:cobblestone","count":3}],
             "outputs":[{"item":"test:iron_pickaxe","count":1}]}
          ]
        })",
        error));
    CHECK(error.empty());
    CHECK(recipes.size() == 3);

    // Set up inventory with crafting materials.
    Inventory inv(9);
    inv.set_filter(0, SlotFilter{ .allowAny = true });  // input
    inv.set_filter(1, SlotFilter{ .allowAny = true });  // input
    inv.set_filter(8, SlotFilter{ .allowAny = true });  // output

    CHECK(inv.set(0, ItemStack{ "test:log", 8 }, items, error));
    CHECK(error.empty());
    CHECK(inv.count_of("test:log") == 8);

    // Craft: 1 log -> 4 planks (no station = empty string).
    const engine::registry::RecipeDefinition* logRecipe = recipes.find_by_name("test:plank_from_log");
    CHECK(logRecipe != nullptr);
    engine::registry::CraftResult result = recipes.craft(inv, *logRecipe, "", items, 42);
    CHECK(result.ok);
    CHECK(result.outputs.size() == 1);
    CHECK(result.outputs[0].item == "test:plank");
    CHECK(result.outputs[0].count == 4);
    CHECK(inv.count_of("test:log") == 7);  // consumed 1

    // Add crafted planks back to inventory.
    ItemStack plankStack; plankStack.item = result.outputs[0].item; plankStack.count = result.outputs[0].count;
    ItemStack remainder = inv.add(plankStack, items, error);
    CHECK(error.empty());
    CHECK(remainder.count == 0);  // all fit
    CHECK(inv.count_of("test:plank") == 4);

    // Transfer: move planks from slot 0 to slot 1 (if any remain there).
    // Actually, let's verify atomicity: craft with insufficient inputs should fail.
    const engine::registry::RecipeDefinition* pickRecipe = recipes.find_by_name("test:pickaxe");
    CHECK(pickRecipe != nullptr);
    // Need 2 sticks + 3 cobblestone. We have 0 sticks.
    engine::registry::CraftResult failResult = recipes.craft(inv, *pickRecipe, "test:crafting_table", items, 1);
    CHECK(!failResult.ok);  // insufficient inputs
    // Inventory must be untouched (atomicity).
    CHECK(inv.count_of("test:plank") == 4);  // unchanged

    std::cout << "advanced-consumer-ok inventory-crafting\n";
    return 0;
}

// --- World profile + structure placement (data-driven biomes/structures) ---
int test_world_profile() {
    using namespace engine::procgen;

    // Create a world profile from JSON — height, climate, caves, ores, structures.
    std::string error;
    auto profile = create_world_profile_from_json(
        R"({
          "version": 1,
          "height": {"noiseType":"simplex","seed":42,"frequency":0.01},
          "baseHeight": 131, "amplitude": 4,
          "climate": {
            "temperature": {"noiseType":"simplex","seed":43,"frequency":0.005},
            "moisture": {"noiseType":"simplex","seed":44,"frequency":0.005}
          },
          "biomes": {
            "biomes": [
              {"id":"plains","temperature":[0.3,0.7],"moisture":[0.2,0.6],"surface":3,"under":2},
              {"id":"desert","temperature":[0.7,1.0],"moisture":[0.0,0.3],"surface":12,"under":2}
            ]
          },
          "caves": {"density":{"noiseType":"simplex","seed":45,"frequency":0.02},"scale":1.0,"offset":0.0},
          "ores": {"density":{"noiseType":"simplex","seed":46,"frequency":0.03},"scale":1.0,"offset":0.0,
                   "table":{"ores":[{"block":18,"minY":0,"maxY":64,"threshold":0.7,"noise":"density"}]}},
          "structures": {
            "definitions": [{
              "id":"test:house",
              "spec":{"sampleWidth":4,"sampleHeight":3,"patternSize":2,"seed":99,
                      "sample":[1,1,1,1,1,2,2,1,1,1,1,1],
                      "profiles":[{"block":3,"layers":[3,3,3]},{"block":5,"layers":[5]}]},
              "sockets":[{"name":"door","position":[0,1,0],"facing":0,"connectTag":"door"}]
            }],
            "rules": [{"structureId":"test:house","biomes":["plains"],"minSurfaceHeight":128,"maxSurfaceHeight":140,"density":1.0,"spacing":8,"yOffset":1}]
          }
        })", error);
    CHECK(profile != nullptr);
    CHECK(error.empty());

    // Profile composes a generator and a structure placement system.
    auto gen = profile->generator();
    CHECK(gen != nullptr);
    auto placement = profile->structure_placement();
    CHECK(placement != nullptr);
    CHECK(placement->definition("test:house") != nullptr);

    // Deterministic round-trip: serialize -> parse -> re-serialize is byte-identical.
    std::string json1;
    CHECK(profile->serialize(json1));
    CHECK(!json1.empty());
    auto roundtrip = create_world_profile_from_json(json1, error);
    CHECK(roundtrip != nullptr && error.empty());
    std::string json2;
    CHECK(roundtrip->serialize(json2));
    CHECK(json1 == json2);
    // Plan region using flat-world functions (surface 131, biome "plains").
    const auto surfaceAt = [](int, int) { return 131; };
    const auto biomeAt = [](int, int) { return std::string("plains"); };
    std::vector<StructurePlacement> plan;
    CHECK(placement->plan_region(
        { placement->rules()[0] }, 0, 0, 2, 2,
        surfaceAt, biomeAt, 12345, plan, error));
    CHECK(error.empty());
    CHECK(plan.size() > 0);
    CHECK(plan[0].structureId == "test:house");
    CHECK(plan[0].origin.y > 0);

    // place_structure writes blocks atomically into a world.
    auto world = engine::voxel::create_default_voxel_world();
    CHECK(world != nullptr);
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
    world->set_chunk_budget(16);
    const auto bstart = std::chrono::steady_clock::now();
    while (!world->is_chunk_loaded(0, 0)) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - bstart).count() > 20000) {
            std::cerr << "world profile: boot timed out
";
            return 1;
        }
        world->update(player, 1.0f / 60.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::string placeErr;
    CHECK(engine::procgen::place_structure(*world, plan[0], placeErr));
    CHECK(placeErr.empty());
    const auto& origin = plan[0].origin;
    CHECK(world->get_block(origin.x, origin.y, origin.z) != 0);
    CHECK(world->get_block(origin.x, origin.y, origin.z) != 0);

    std::cout << "advanced-consumer-ok world-profile\n";
    return 0;
}

}  // namespace

int main() {
    if (test_worlds_and_portal() != 0) return 1;
    if (test_gameplay() != 0) return 1;
    if (test_block_entities() != 0) return 1;
    if (test_save_load_roundtrip() != 0) return 1;
    if (test_per_voxel_state() != 0) return 1;
    if (test_registries() != 0) return 1;
    if (test_inventory_crafting() != 0) return 1;
    if (test_world_profile() != 0) return 1;
    std::cout << "advanced-consumer-ok all\n";
    return 0;
}
