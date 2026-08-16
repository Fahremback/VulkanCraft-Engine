// External consumer smoke (FALTANTES item 11 / §24): this TU compiles and
// links ONLY against the installed SDK — <engine/...> headers from the install
// prefix and the vc_sdk archive + static deps. It exercises the same public
// contracts the in-tree gates prove (registry JSON documents mirroring the MCP
// assets of item 10, and the gameplay destruction scenario of item 9).
//
// Exit code 0 + "consumer-ok" markers = the installed SDK is self-sufficient.

#include <engine/gameplay/IGameplayRuntime.hpp>
#include <engine/registry/BlockRegistry.hpp>
#include <engine/registry/FluidRegistry.hpp>
#include <engine/registry/ItemRegistry.hpp>
#include <engine/registry/RecipeRegistry.hpp>

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "consumer failure: " #condition "\n"; return 1; } } while (false)

namespace {

const char* kBlockDocument =
    "{\"version\":1,\"namespace\":\"vulkancraft\",\"name\":\"Titanium\","
    "\"class\":\"solid\",\"hardness\":3.5,\"lightEmission\":0,"
    "\"lightAbsorption\":1,\"opaque\":true,\"collidable\":true,"
    "\"tags\":[\"metal\"],\"drops\":[]}";

const char* kItemDocument =
    "{\"version\":1,\"namespace\":\"vulkancraft\",\"name\":\"titanium_ingot\","
    "\"maxStack\":16,\"durability\":0,\"icon\":\"\",\"model\":\"\","
    "\"tags\":[\"metal\"]}";

const char* kInputItemDocument =
    "{\"version\":1,\"namespace\":\"vulkancraft\",\"name\":\"titanium\","
    "\"maxStack\":64,\"durability\":0,\"icon\":\"\",\"model\":\"\",\"tags\":[]}";

const char* kFluidDocument =
    "{\"version\":1,\"block\":\"vulkancraft:sludge\",\"viscosity\":0.8,"
    "\"density\":1,\"range\":7,\"tickInterval\":0.08,\"source\":true,"
    "\"falling\":true,\"evaporation\":true,\"damagePerTick\":2,"
    "\"compressible\":false}";

const char* kRecipeDocument =
    "{\"version\":1,\"namespace\":\"vulkancraft\",\"name\":\"TitaniumIngot\","
    "\"station\":\"vulkancraft:furnace\",\"time\":1,\"energy\":0,\"fuel\":\"\","
    "\"conditions\":[],\"tags\":[],"
    "\"inputs\":[{\"item\":\"vulkancraft:titanium\",\"count\":2}],"
    "\"outputs\":[{\"item\":\"vulkancraft:titanium_ingot\",\"count\":1}]}";

bool has_string(const std::vector<std::string>& values, const std::string& needle) {
    for (const std::string& value : values) {
        if (value == needle) return true;
    }
    return false;
}

int test_registries() {
    engine::registry::BlockRegistry blocks;
    std::string error;
    CHECK(blocks.load_from_json(kBlockDocument, error));
    CHECK(error.empty());
    const engine::registry::BlockDefinition* titanium =
        blocks.find_by_name("vulkancraft:Titanium");
    CHECK(titanium != nullptr);
    CHECK(titanium->hardness == 3.5f);
    CHECK(titanium->opaque);
    CHECK(has_string(titanium->tags, "metal"));
    CHECK(!titanium->hasBuiltinMapping);

    engine::registry::ItemRegistry items;
    CHECK(items.load_from_json(kInputItemDocument, error));
    CHECK(items.load_from_json(kItemDocument, error));
    CHECK(items.find_by_name("vulkancraft:titanium_ingot") != nullptr);

    engine::registry::FluidRegistry fluids;
    CHECK(fluids.load_from_json(kFluidDocument, error));
    CHECK(fluids.find_by_block("vulkancraft:sludge") != nullptr);

    engine::registry::RecipeRegistry recipes;
    CHECK(recipes.load_from_json(kRecipeDocument, error));
    CHECK(recipes.find_by_name("vulkancraft:TitaniumIngot") != nullptr);

    std::cout << "consumer-ok registries\n";
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

    std::cout << "consumer-ok gameplay\n";
    return 0;
}

}  // namespace

int main() {
    if (test_registries() != 0) return 1;
    if (test_gameplay() != 0) return 1;
    std::cout << "consumer-ok all\n";
    return 0;
}
