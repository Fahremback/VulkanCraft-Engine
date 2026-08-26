const fs = require('fs');
const file = 'tools/external-project-advanced/main.cpp';
let c = fs.readFileSync(file, 'utf8');
const startMarker = '// --- World profile + structure placement';
const endMarker = '}  // namespace';
const startIdx = c.indexOf(startMarker);
const endIdx = c.indexOf(endMarker);
if (startIdx === -1 || endIdx === -1) { console.log('markers not found'); process.exit(1); }
const newFunc = `// --- World profile + structure placement (data-driven biomes/structures) ---
int test_world_profile() {
    using namespace engine::procgen;
    std::string error;
    auto profile = create_world_profile_from_json(
        R"({
  "version": 1,
  "height": {"version":1,"seed":2026,"root":1,"nodes":[
    {"type":"perlin","params":[0.05,0.0],"sources":[]},
    {"type":"fbm","params":[4,0.5,2.0,0.0],"sources":[0]}]},
  "baseHeight": 131, "amplitude": 4,
  "climate": {
    "temperature": {"version":1,"seed":43,"root":0,"nodes":[{"type":"constant","params":[0.5],"sources":[]}]},
    "moisture": {"version":1,"seed":44,"root":0,"nodes":[{"type":"constant","params":[0.3],"sources":[]}]}},
  "biomes": {"version":1,"biomes":[
    {"name":"plains_rule","engineBiomeIndex":0,
     "climate":{"temperature":[0.25,0.75],"moisture":[0.0,0.6]},
     "surface":[{"blockId":3,"minDepth":0,"maxDepth":0}]},
    {"name":"desert_rule","engineBiomeIndex":1,
     "climate":{"temperature":[0.75,1.0],"moisture":[-1.0,0.3]},
     "surface":[{"blockId":12,"minDepth":0,"maxDepth":0}]}]},
  "caves": {"density":{"version":1,"seed":45,"root":0,"nodes":[{"type":"constant","params":[0.0],"sources":[]}]}},
  "ores": {"density":{"version":1,"seed":46,"root":0,"nodes":[{"type":"constant","params":[0.0],"sources":[]}]},
           "table":{"version":1,"rules":[{"blockId":18,"minDensity":0.7,"maxDensity":0.8,"minY":10,"maxY":120}]}},
  "carver": {"version":1,"fluidMaxY":60,"fluidBlockId":12},
  "decorators": {"version":1,"decorators":[]},
  "structures": {"version":1,"definitions":[
    {"id":"test:house","outputWidth":8,"outputHeight":6,
     "spec":{"version":1,"sampleWidth":4,"sampleHeight":3,"patternSize":2,
             "symmetry":0,"periodicOutput":false,"ground":false,"seed":99,
             "sample":[1,1,1,1,1,2,2,1,1,1,1,1],
             "profiles":[{"blockId":3,"layers":[3,3,3]},{"blockId":5,"layers":[5]}]},
     "sockets":[{"name":"door","position":[0,1,0],"facing":0,"connectTag":"door"}]}],
   "rules":[{"structureId":"test:house","biomes":["plains_rule"],
             "density":1.0,"spacing":8,"yOffset":1,"seedOffset":0}]}
})", error);
    if (!profile) {
        std::cerr << "PROFILE CREATE ERROR: " << error << "\n";
        return 1;
    }
    auto gen = profile->generator();
    CHECK(gen != nullptr);
    auto placement = profile->structure_placement();
    CHECK(placement != nullptr);
    CHECK(placement->definition("test:house") != nullptr);
    CHECK(placement->definition("test:house")->sockets.size() == 1);
    CHECK(placement->rules().size() == 1);
    std::string json1;
    CHECK(profile->serialize(json1));
    CHECK(!json1.empty());
    auto roundtrip = create_world_profile_from_json(json1, error);
    CHECK(roundtrip != nullptr && error.empty());
    std::string json2;
    CHECK(roundtrip->serialize(json2));
    CHECK(json1 == json2);
    const auto surfaceAt = [](int, int) { return 131; };
    const auto biomeAt = [](int, int) { return std::string("plains_rule"); };
    std::vector<StructurePlacement> plan;
    CHECK(placement->plan_region(
        { placement->rules()[0] }, 0, 0, 2, 2,
        surfaceAt, biomeAt, 12345, plan, error));
    CHECK(error.empty());
    CHECK(plan.size() > 0);
    CHECK(plan[0].structureId == "test:house");
    CHECK(plan[0].origin.y > 0);
    auto world = engine::voxel::create_default_voxel_world();
    CHECK(world != nullptr);
    const glm::vec3 player{ 8.0f, 200.0f, 8.0f };
    world->set_chunk_budget(16);
    const auto bstart = std::chrono::steady_clock::now();
    while (!world->is_chunk_loaded(0, 0)) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - bstart).count() > 20000) {
            std::cerr << "world profile: boot timed out\n";
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
    std::cout << "advanced-consumer-ok world-profile\n";
    return 0;
}

`;
const result = c.substring(0, startIdx) + newFunc + c.substring(endIdx);
fs.writeFileSync(file, result, 'utf8');
console.log('Done. Lines:', result.split('\n').length);
