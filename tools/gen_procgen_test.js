const fs = require('fs');
const path = 'tools/external-project-advanced/main.cpp';
let c = fs.readFileSync(path, 'utf8');

// Add includes
c = c.replace(
    '#include <engine/registry/BlockRegistry.hpp>',
    '#include <engine/registry/BlockRegistry.hpp>\n#include <engine/procgen/IWorldProfile.hpp>\n#include <engine/procgen/IStructurePlacement.hpp>'
);

// Add test function before }  // namespace
const newTest = `// --- World profile + structure placement (data-driven biomes/structures) ---
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

    // Place structures in a world.
    auto world = engine::voxel::create_default_voxel_world();
    CHECK(world != nullptr);
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
    world->set_chunk_budget(16);
    const auto start = std::chrono::steady_clock::now();
    while (!world->is_chunk_loaded(0, 0)) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count() > 20000) {
            std::cerr << "world profile: boot timed out\\n";
            return 1;
        }
        world->update(player, 1.0f / 60.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // Plan region for chunks around origin — structures should be placed.
    std::vector<StructurePlacement> placements;
    placement->plan_region(*world, -2, -2, 2, 2, 12345, placements);
    // With density 1.0 in plains, we expect some placements.
    CHECK(placements.size() > 0);
    CHECK(placements[0].structureId == "test:house");
    CHECK(placements[0].origin.y > 0);

    // place_structure writes blocks atomically.
    std::string placeErr;
    CHECK(placement->place_structure(*world, placements[0], placeErr));
    CHECK(placeErr.empty());
    // Verify at least one block was placed.
    const auto& origin = placements[0].origin;
    CHECK(world->get_block(origin.x, origin.y, origin.z) != 0);

    std::cout << "advanced-consumer-ok world-profile\\n";
    return 0;
}

`;

c = c.replace('}  // namespace', newTest + '}  // namespace');

// Update main
c = c.replace(
    '    if (test_inventory_crafting() != 0) return 1;',
    '    if (test_inventory_crafting() != 0) return 1;\n    if (test_world_profile() != 0) return 1;'
);

fs.writeFileSync(path, c, 'utf8');
console.log('Added world_profile test, total lines:', c.split('\n').length);
