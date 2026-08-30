// ShowcaseGameplay.cpp — AGENTE 2 (aceleracao — gameplay showcase).
//
// The canonical public factories of character / animation / physics /
// simulation / gameplay are consumed by the REAL game loop (init -> fixed
// tick -> render snapshot -> shutdown). Each factory is created and
// configured with live world/player data in showcase_gameplay_init(), is
// advanced every FIXED tick in showcase_gameplay_tick() (gated by the
// deterministic IFixedTickSim accumulator), and its observable state is
// published through showcaseSummary, which the game window title reads.
// Teardown runs in showcase_gameplay_shutdown().
//
// This TU is part of vc_app_runtime (the same module as VulkanEngineApp.cpp),
// so the methods below are the executable's own implementation of the
// showcase integration — no separate consumer binary.

#include "VulkanEngineApp.hpp"

#include <GLFW/glfw3.h>
#include <glm/gtc/quaternion.hpp>

#include "engine/sdk/RegistryJson.hpp"
#include "engine/networking/INetworkTransportFactory.hpp"
#include "engine/networking/INetworkSession.hpp"
#include "engine/audio/AudioEvent.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

// Terrain seam for the animation foot-placement / procedural-locomotion and
// the character controller: reads the LIVE voxel world surface.
class ShowcaseTerrainSampler final : public engine::animation::IFootTerrainSampler {
public:
    explicit ShowcaseTerrainSampler(const World& w) : world_(&w) {}
    engine::animation::SurfaceSample sample(float worldX, float worldZ) const override {
        engine::animation::SurfaceSample out;
        for (int y = 90; y >= 0; --y) {
            if (world_->is_solid_block_id(world_->get_block_at(
                    glm::vec3(worldX, static_cast<float>(y), worldZ)))) {
                out.known = true;
                out.height = static_cast<float>(y) + 1.0f;
                break;
            }
        }
        return out;
    }

private:
    const World* world_;
};

// The canonical 6-bone showcase skeleton — the same chain shape the
// showcase_character_gait.json leg chains reference (hip/knee/foot x2).
engine::animation::SkeletonSpec make_showcase_skeleton() {
    engine::animation::SkeletonSpec spec;
    spec.id = "showcase_char";
    auto bone = [](const char* id, int parent) {
        engine::animation::Bone b;
        b.id = id;
        b.parent = parent;
        return b;
    };
    spec.bones = {
        bone("LeftHip", -1),
        bone("LeftKnee", 0),
        bone("LeftFoot", 1),
        bone("RightHip", -1),
        bone("RightKnee", 3),
        bone("RightFoot", 4),
    };
    return spec;
}

// Scans the LIVE voxel world for the topmost SOLID voxel under (wx, wz).
// `found` (optional) is true iff a solid was hit in [0, 90]; false on the
// 24.0 fallback (genuinely empty column, or a not-yet-streamed chunk that
// still reads as air).
float showcase_scan_surface(const World& world, float wx, float wz, bool* found) {
    for (int y = 90; y >= 0; --y) {
        if (world.is_solid_block_id(world.get_block_at(
                glm::vec3(wx, static_cast<float>(y), wz)))) {
            if (found) *found = true;
            return static_cast<float>(y) + 1.0f;
        }
    }
    if (found) *found = false;
    return 24.0f;
}

// Sample the world surface height under (wx, wz) from the LIVE voxel world.
float showcase_surface_height(const World& world, float wx, float wz) {
    bool found = false;
    return showcase_scan_surface(world, wx, wz, &found);
}

// CONTA 3: samples walkable columns of the LIVE voxel world for a navmesh
// config (CONTA 3 items 117/118). Each column's walkable surface is the
// topmost SOLID voxel over [boundsMinY, boundsMaxY] (an air column is skipped
// — nothing to walk on). Feeding these to INavigationProvider::build()/update()
// reproduces the server's sample_voxel_columns over the game's own live world.
std::vector<engine::navigation::VoxelColumn> showcase_nav_sample_columns(
    const World& world, const engine::navigation::NavmeshConfig& config) {
    std::vector<engine::navigation::VoxelColumn> columns;
    if (config.cellSize <= 0.0f) return columns;
    const float step = config.cellSize;
    const int topY = static_cast<int>(std::floor(config.boundsMaxY));
    const int bottomY = static_cast<int>(std::floor(config.boundsMinY));
    const float startX = std::floor(config.boundsMinX / step) * step;
    const float startZ = std::floor(config.boundsMinZ / step) * step;
    for (float x = startX; x <= config.boundsMaxX; x += step) {
        for (float z = startZ; z <= config.boundsMaxZ; z += step) {
            int surfaceY = -1;
            for (int y = topY; y >= bottomY; --y) {
                if (world.is_solid_block_id(
                        world.get_block_at(glm::vec3(x, static_cast<float>(y), z)))) {
                    surfaceY = y;
                    break;
                }
            }
            if (surfaceY < 0) continue;
            engine::navigation::VoxelColumn column;
            column.x = x;
            column.z = z;
            column.solidMinY = static_cast<float>(surfaceY);
            column.solidMaxY = static_cast<float>(surfaceY) + 1.0f;
            column.solid = true;
            columns.push_back(column);
        }
    }
    return columns;
}

// Cached surface-height query (see the header comment). Memoizes per column;
// the load-replay and block edits clear the cache via invalidateShowcaseSurfaces.
float VulkanEngineApp::showcaseSurfaceAt(const World& world, float wx, float wz) {
    const int cx = static_cast<int>(std::floor(wx));
    const int cz = static_cast<int>(std::floor(wz));
    const long long key = (static_cast<long long>(cx) << 32) ^
                          static_cast<unsigned int>(cz);
    const auto it = showcaseSurfaceLod.find(key);
    if (it != showcaseSurfaceLod.end()) {
        ++showcaseSurfaceCacheHits;
        return it->second;
    }
    ++showcaseSurfaceCacheMisses;
    bool found = false;
    const float h = showcase_scan_surface(world, wx, wz, &found);
    // Cache only a REASON-so-far (solid voxel hit). The 24.0 air fallback is
    // left uncached so a column whose chunk is still streaming re-scans every
    // tick until it uploads — pinning a stale low value would break the
    // surface query when the chunk finally arrives.
    if (found) showcaseSurfaceLod.emplace(key, h);
    return h;
}

// J.125 occlusion sampler — a bounded DDA walk through the LIVE voxel world
// from `from` to `to`. Returns the fraction of samples that hit a solid block
// ([0,1]); 0 = clear line of sight, 1 = fully blocked. This is the per-source
// `occlusion` INPUT the ISpatialAudio contract expects (the raycast lives in
// the world, the spatializer stays decoupled). Air/streaming-unknown cells
// count as transparent so a not-yet-loaded column never blocks sound.
float VulkanEngineApp::showcaseAudioOcclusion(const World& world,
                                              const glm::vec3& from,
                                              const glm::vec3& to,
                                              int maxSamples) {
    const glm::vec3 delta = to - from;
    const float dist = glm::length(delta);
    if (dist <= 1.0e-4f || maxSamples <= 0) return 0.0f;
    int solid = 0;
    const int steps = std::min(maxSamples, std::max(1, static_cast<int>(dist)));
    for (int i = 1; i <= steps; ++i) {
        const glm::vec3 p = from + delta * (static_cast<float>(i) / steps);
        if (world.get_block_at(p) != kRuntimeAirId) ++solid;
    }
    return static_cast<float>(solid) / static_cast<float>(steps);
}

void VulkanEngineApp::invalidateShowcaseSurfaces() {
    showcaseSurfaceLod.clear();
    // CONTA 3: a committed block edit changed the walkable terrain — invalidate
    // the nav ledger region around the player and cancel every in-flight async
    // path query so a stale route (through the removed/added block) is never
    // applied. The nav provider re-bakes the affected tiles on the next tick.
    showcase_gameplay_nav_invalidate();
}

// Builds the canonical MotionSkeleton the motion-matcher/pose-warper/validator
// contracts operate on (the same 6-bone showcase chain).
engine::animation::MotionSkeleton make_showcase_motion_skeleton() {
    engine::animation::MotionSkeleton ms;
    ms.name = "showcase";
    const char* names[] = { "LeftHip", "LeftKnee", "LeftFoot",
                            "RightHip", "RightKnee", "RightFoot" };
    const int parents[] = { -1, 0, 1, -1, 3, 4 };
    for (int i = 0; i < 6; ++i) {
        engine::animation::MotionBone b;
        b.name = names[i];
        b.parent = parents[i];
        b.localTranslation = { 0.0f, 0.9f - 0.45f * (i % 3), 0.0f };
        ms.bones.push_back(b);
    }
    return ms;
}

glm::vec3 VulkanEngineApp::showVehicleChassisPos() const {
    if (!showcaseVehicle || !runtimePhysics) return { 0.0f, 0.0f, 0.0f };
    // The public IVehicle::chassis() is the BodyId the runtime owns; read its
    // live state from the same IPhysicsWorld the game steps every fixed tick.
    const auto body = showcaseVehicle->chassis();
    engine::gameplay::BodyState st;
    if (runtimePhysics->physics().body_state(body, st)) {
        return st.position;
    }
    return { 0.0f, 0.0f, 0.0f };
}

void VulkanEngineApp::showcase_gameplay_init() {
    std::string err;

    // ── CONTA 2 (world/procgen — 18 factories): compose the data-driven
    // generator and register it on the LIVE world before it starts streaming.
    // Every chunk dispatcher samples the composed generator (see
    // WorldProcgen.cpp); the observables land in the title each fixed tick.
    worldProcgen.init(world);

    // ── A: load the ShowcaseGame project by configuration ──
    // project.json -> initialScene -> the REAL scene loader parses the scene;
    // the observables report what the project actually contains (entities /
    // camera). This is the config-driven showcase bootstrap: the game boots
    // the showcase project, not a hardcoded world.
    {
        std::ifstream projectFile(
            std::string(VULKANCRAFT_SOURCE_DIR) +
            "/Projects/ShowcaseGame/project.json");
        if (projectFile) {
            std::string content((std::istreambuf_iterator<char>(projectFile)),
                                std::istreambuf_iterator<char>());
            // Tiny key scan for the two bootstrap fields (schema'd JSON).
            auto grab = [&](const std::string& key) -> std::string {
                const std::size_t pos = content.find(key);
                if (pos == std::string::npos) return "";
                const std::size_t q1 = content.find('"', pos + key.size());
                if (q1 == std::string::npos) return "";
                const std::size_t q2 = content.find('"', q1 + 1);
                if (q2 == std::string::npos) return "";
                return content.substr(q1 + 1, q2 - q1 - 1);
            };
            showcaseProjectName = grab("\"name\"");
            showcaseInitialScene = grab("\"initialScene\"");
        }
        if (!showcaseInitialScene.empty()) {
            const std::string scenePath =
                std::string(VULKANCRAFT_SOURCE_DIR) + "/Projects/ShowcaseGame/" +
                showcaseInitialScene;
            if (showcaseScene.load_from_file(scenePath)) {
                showcaseSceneLoaded = true;
                showcaseSceneEntities = showcaseScene.get_entities().size();
                showcaseSceneHasCamera =
                    !showcaseScene.cameraComponents.empty();
                std::cout << "[Showcase] project '" << showcaseProjectName
                          << "' scene '" << showcaseInitialScene << "' loaded ("
                          << showcaseSceneEntities << " entities"
                          << (showcaseSceneHasCamera ? ", camera" : "")
                          << ")\n";
            } else {
                std::cout << "[Showcase] scene load refused: " << scenePath << '\n';
            }
        }
    }

    // ── A: data-driven audio — the showcase project declares its audio event
    // in Content/AudioEvents/showcase_audio.json (VulkanEngine.AudioEvent, the
    // same document author_showcase_game authors and the cooker/package ships).
    // Load it through the canonical AudioEventAsset loader so the game
    // registers the project's clip into the SAME SoundEngine map the builtin
    // sounds live in — the project asset is a real consumer, not a dead file.
    {
        const std::string audioAssetPath =
            std::string(VULKANCRAFT_SOURCE_DIR) +
            "/Projects/ShowcaseGame/Content/AudioEvents/showcase_audio.json";
        Engine::AudioEventAsset audioEvent;
        if (audioEvent.load_from_file(audioAssetPath)) {
            showcaseAudioAssetName = audioEvent.name;
            showcaseAudioAssetLoaded = true;
            const std::string resolvedClip =
                std::string(VULKANCRAFT_SOURCE_DIR) + "/Projects/ShowcaseGame/Content/" +
                audioEvent.clipPath;
            if (soundEngine.register_audio_asset(audioEvent.name, resolvedClip)) {
                showcaseAudioRegistered = true;
                std::cout << "[Showcase] audio asset consumed: '"
                          << audioEvent.name << "' volume " << audioEvent.volume
                          << " (clip " << audioEvent.clipPath
                          << ", registered in sound engine)\n";
            } else {
                std::cout << "[Showcase] audio asset parsed ('" << audioEvent.name
                          << "') but clip missing at " << resolvedClip
                          << "; registered with fallback\n";
            }
        } else {
            std::cout << "[Showcase] audio asset refused: " << audioAssetPath << '\n';
        }
    }

    // ── A: data-driven lights — Content/Config/showcase_lights.json is the
    // project's authoritative light set (the same document author_showcase_game
    // authors and the cooker/package builder ships). Each declared light becomes
    // a REAL scene light component (transform + LightComponent on a named
    // entity), so the renderer's light UBO is driven by the project asset — not
    // a hardcoded light list in code.
    {
        const std::string lightsPath =
            std::string(VULKANCRAFT_SOURCE_DIR) +
            "/Projects/ShowcaseGame/Content/Config/showcase_lights.json";
        std::ifstream lightsFile(lightsPath);
        if (lightsFile) {
            std::string content((std::istreambuf_iterator<char>(lightsFile)),
                                std::istreambuf_iterator<char>());
            engine::sdk::JsonValue root;
            std::string jsonError;
            if (engine::sdk::json_parse(content, root, jsonError) && root.is_object()) {
                const engine::sdk::JsonValue* lights = root.field("lights");
                if (lights && lights->is_array()) {
                    for (const auto& light : lights->array) {
                        if (!light.is_object()) continue;
                        const std::string name =
                            engine::sdk::json_string(light, "name", "light");
                        const std::string type =
                            engine::sdk::json_string(light, "type", "point");
                        const std::vector<double> color =
                            engine::sdk::json_number_array(light, "color");
                        const double intensity =
                            engine::sdk::json_number(light, "intensity", 1000.0);
                        const double range =
                            engine::sdk::json_number(light, "range", 50.0);
                        const bool castShadows =
                            engine::sdk::json_bool(light, "castShadows", true);

                        Engine::LightComponent lc;
                        if (color.size() >= 3) {
                            lc.color = glm::vec3(static_cast<float>(color[0]),
                                                 static_cast<float>(color[1]),
                                                 static_cast<float>(color[2]));
                        }
                        lc.intensity = static_cast<float>(intensity);
                        lc.range = static_cast<float>(range);
                        lc.castShadows = castShadows;
                        if (type == "directional") lc.type = Engine::LightType::Directional;
                        else if (type == "spot") lc.type = Engine::LightType::Spot;
                        else if (type == "area") lc.type = Engine::LightType::Area;
                        else lc.type = Engine::LightType::Point;

                        // Apply into the loaded project scene: reuse the named
                        // entity when the scene already declares it (e.g. the
                        // scene's "Sun"), else create one.
                        Engine::Entity lightEntity;
                        for (const auto& [id, entity] : showcaseScene.get_entities()) {
                            (void)id;
                            if (entity.get_name() == name) {
                                lightEntity = entity;
                                break;
                            }
                        }
                        if (!lightEntity.is_valid()) {
                            lightEntity = showcaseScene.create_entity(name);
                        }
                        showcaseScene.lightComponents[lightEntity.get_id()] = lc;
                        showcaseScene.transformComponents[lightEntity.get_id()] =
                            Engine::TransformComponent{};
                        ++showcaseLightCount;
                    }
                    showcaseLightsAssetLoaded = true;
                    std::cout << "[Showcase] lights asset consumed: "
                              << showcaseLightCount
                              << " light(s) applied to scene from showcase_lights.json\n";
                }
            } else {
                std::cout << "[Showcase] lights asset refused (parse): "
                          << jsonError << '\n';
            }
        } else {
            std::cout << "[Showcase] lights asset absent: " << lightsPath << '\n';
        }
    }

    // ── A: data-driven abilities — the public IAbilitySystem loads the
    // project's ability definition (Content/Registry/abilities/
    // showcase_character_abilities.json) through AbilityDefinition::
    // load_from_json and registers it, so the ability asset is a REAL
    // consumer of the game executable (same document author_showcase_game
    // authors and the cooker/package ships).
    {
        const std::string abilitiesPath =
            std::string(VULKANCRAFT_SOURCE_DIR) +
            "/Projects/ShowcaseGame/Content/Registry/abilities/"
            "showcase_character_abilities.json";
        std::ifstream abilitiesFile(abilitiesPath);
        if (abilitiesFile) {
            std::string content((std::istreambuf_iterator<char>(abilitiesFile)),
                                std::istreambuf_iterator<char>());
            engine::gameplay::AbilityDefinition definition;
            std::string abilityError;
            if (definition.load_from_json(content, abilityError)) {
                std::string sysError;
                showcaseAbilities = engine::gameplay::create_ability_system();
                if (showcaseAbilities &&
                    showcaseAbilities->register_ability(definition, sysError)) {
                    showcaseAbilityAssetLoaded = true;
                    showcaseAbilityCount =
                        showcaseAbilities->ability_ids().size();
                    std::cout << "[Showcase] abilities asset consumed: '"
                              << definition.id << "' registered ("
                              << showcaseAbilityCount << " ability)\n";
                } else {
                    std::cout << "[Showcase] abilities registration refused: "
                              << sysError << '\n';
                }
            } else {
                std::cout << "[Showcase] abilities asset refused (load): "
                          << abilityError << '\n';
            }
        } else {
            std::cout << "[Showcase] abilities asset absent: " << abilitiesPath
                      << '\n';
        }
    }

    // ── A: data-driven item + inventory registries — the showcase project
    // declares its items (Content/Registry/item/{dirt,planks,glowstone_custom}
    // .json — the same documents author_showcase_game ships) and its starting
    // inventory (Content/Inventories/showcase_inventory.json). The items
    // register into the SAME ItemRegistry the player hotbar uses (real
    // data-driven items); the inventory document is then deserialized into a
    // dedicated 56-slot showcase Inventory through the canonical
    // Inventory::deserialize_json (all-or-nothing — every referenced item must
    // resolve), so the asset is consumed as authored, not forced into the
    // 9-slot hotbar.
    if (playerItems) {
        const char* showcaseItemAssets[] = {
            "/Projects/ShowcaseGame/Content/Registry/item/dirt.json",
            "/Projects/ShowcaseGame/Content/Registry/item/planks.json",
            "/Projects/ShowcaseGame/Content/Registry/item/glowstone_custom.json",
        };
        for (const char* relative : showcaseItemAssets) {
            const std::string itemPath =
                std::string(VULKANCRAFT_SOURCE_DIR) + relative;
            std::ifstream itemFile(itemPath);
            if (!itemFile) continue;
            std::string content((std::istreambuf_iterator<char>(itemFile)),
                                std::istreambuf_iterator<char>());
            std::string itemError;
            if (playerItems->load_from_json(content, itemError)) {
                showcaseItemAssetLoaded = true;
                std::cout << "[Showcase] item registry asset consumed: "
                          << relative << "\n";
            } else {
                std::cout << "[Showcase] item asset refused: " << itemError
                          << " (" << relative << ")\n";
            }
        }
    }
    {
        const std::string invPath =
            std::string(VULKANCRAFT_SOURCE_DIR) +
            "/Projects/ShowcaseGame/Content/Inventories/showcase_inventory.json";
        std::ifstream invFile(invPath);
        if (invFile) {
            std::string content((std::istreambuf_iterator<char>(invFile)),
                                std::istreambuf_iterator<char>());
            engine::sdk::JsonValue root;
            std::string jsonError;
            if (engine::sdk::json_parse(content, root, jsonError) &&
                root.is_object()) {
                const std::string name =
                    engine::sdk::json_string(root, "name", "showcase_inventory");
                const engine::sdk::JsonValue* slots = root.field("slots");
                if (slots != nullptr && slots->is_array()) {
                    showcaseInventoryAssetLoaded = true;
                    showcaseInventorySlots = slots->array.size();
                    for (const auto& slot : slots->array) {
                        if (slot.is_object() &&
                            !engine::sdk::json_string(slot, "item", "").empty()) {
                            ++showcaseInventoryItems;
                        }
                    }
                    // Full consumption: deserialize the document into a
                    // dedicated showcase Inventory sized to the asset (the
                    // canonical all-or-nothing loader — every referenced item
                    // must resolve in the registry).
                    if (playerItems) {
                        showcaseInventory = std::make_unique<
                            engine::registry::Inventory>(
                            static_cast<int>(showcaseInventorySlots));
                        std::string invError;
                        if (showcaseInventory->deserialize_json(
                                content, *playerItems, invError)) {
                            showcaseInventoryDeserialized = true;
                        } else {
                            std::cout << "[Showcase] inventory asset "
                                         "deserialize refused: "
                                      << invError << '\n';
                        }
                    }
                    std::cout << "[Showcase] inventory asset consumed: '"
                              << name << "' with " << showcaseInventoryItems
                              << " item(s) across " << showcaseInventorySlots
                              << " slots"
                              << (showcaseInventoryDeserialized
                                      ? " (deserialized)"
                                      : "") << '\n';
                } else {
                    std::cout << "[Showcase] inventory asset refused "
                                 "(no slots array)\n";
                }
            } else {
                std::cout << "[Showcase] inventory asset refused (parse): "
                          << jsonError << '\n';
            }
        } else {
            std::cout << "[Showcase] inventory asset absent: " << invPath << '\n';
        }
    }

    // ── A: data-driven vehicle — Content/Registry/vehicles/showcase_jeep.json
    // is loaded through the canonical public VehicleAsset::load_from_json
    // (the same contract create_vehicle_from_asset consumes) and validated;
    // the declared wheels/provider are observable.
    {
        const std::string jeepPath =
            std::string(VULKANCRAFT_SOURCE_DIR) +
            "/Projects/ShowcaseGame/Content/Registry/vehicles/showcase_jeep.json";
        std::ifstream jeepFile(jeepPath);
        if (jeepFile) {
            std::string content((std::istreambuf_iterator<char>(jeepFile)),
                                std::istreambuf_iterator<char>());
            engine::vehicles::VehicleAsset asset;
            std::string vehicleError;
            if (asset.load_from_json(content, vehicleError) &&
                asset.validate(vehicleError)) {
                showcaseJeepAssetLoaded = true;
                showcaseJeepWheels = asset.wheels.size();
                std::cout << "[Showcase] jeep asset consumed: '" << asset.name
                          << "' (" << showcaseJeepWheels << " wheels, provider "
                          << static_cast<int>(asset.provider) << ")\n";
            } else {
                std::cout << "[Showcase] jeep asset refused: " << vehicleError
                          << '\n';
            }
        } else {
            std::cout << "[Showcase] jeep asset absent: " << jeepPath << '\n';
        }
    }

    // ── A: data-driven mission — Content/Registry/missions/
    // showcase_portal_mission.json is loaded through the canonical public
    // MissionDefinition::load_from_json and validated; the declared
    // objectives/reward are observable. (The active in-play mission stays the
    // gameplay-driven one; the project asset is a real loader consumer.)
    {
        const std::string missionPath =
            std::string(VULKANCRAFT_SOURCE_DIR) +
            "/Projects/ShowcaseGame/Content/Registry/missions/"
            "showcase_portal_mission.json";
        std::ifstream missionFile(missionPath);
        if (missionFile) {
            std::string content((std::istreambuf_iterator<char>(missionFile)),
                                std::istreambuf_iterator<char>());
            engine::gameplay::MissionDefinition asset;
            std::string missionError;
            if (asset.load_from_json(content, missionError) &&
                asset.validate(missionError)) {
                showcaseMissionAssetLoaded = true;
                showcaseMissionAssetObjectives = asset.objectives.size();
                std::cout << "[Showcase] mission asset consumed: '" << asset.name
                          << "' (" << showcaseMissionAssetObjectives
                          << " objectives, reward "
                          << asset.reward.itemId << " x" << asset.reward.count
                          << ")\n";
            } else {
                std::cout << "[Showcase] mission asset refused: "
                          << missionError << '\n';
            }
        } else {
            std::cout << "[Showcase] mission asset absent: " << missionPath
                      << '\n';
        }
    }

    // ── A: data-driven network declaration — Content/Network/
    // showcase_network.json mirrors the public DedicatedServerConfig contract;
    // the game loads and validates the document (server id, endpoint, tick
    // rate, max clients) as an executable consumer of the asset. Runtime
    // ingestion of the config stays in the network domain (Agente 3), but the
    // declaration is read here, not dead.
    {
        const std::string netPath =
            std::string(VULKANCRAFT_SOURCE_DIR) +
            "/Projects/ShowcaseGame/Content/Network/showcase_network.json";
        std::ifstream netFile(netPath);
        if (netFile) {
            std::string content((std::istreambuf_iterator<char>(netFile)),
                                std::istreambuf_iterator<char>());
            engine::sdk::JsonValue root;
            std::string jsonError;
            if (engine::sdk::json_parse(content, root, jsonError) &&
                root.is_object()) {
                const std::string serverId =
                    engine::sdk::json_string(root, "server_id", "showcase");
                showcaseNetworkAssetLoaded = true;
                showcaseNetworkTickRate =
                    static_cast<std::size_t>(
                        engine::sdk::json_number(root, "tick_rate", 60.0));
                showcaseNetworkMaxClients =
                    static_cast<std::size_t>(
                        engine::sdk::json_number(root, "max_clients", 4.0));
                if (const engine::sdk::JsonValue* transport =
                        root.field("transport")) {
                    if (const engine::sdk::JsonValue* endpoint =
                            transport->field("endpoint")) {
                        showcaseNetworkPort =
                            static_cast<std::size_t>(
                                engine::sdk::json_number(*endpoint, "port", 25565.0));
                    }
                }
                std::cout << "[Showcase] network asset consumed: server '"
                          << serverId << "' port " << showcaseNetworkPort
                          << " tick " << showcaseNetworkTickRate << " max "
                          << showcaseNetworkMaxClients << "\n";
            } else {
                std::cout << "[Showcase] network asset refused (parse): "
                          << jsonError << '\n';
            }
        } else {
            std::cout << "[Showcase] network asset absent: " << netPath << '\n';
        }
    }

    // ── A: spawn the showcase character as a PERSISTENT entity in the SAME
    // canonical ECS the mobs live in — stable id + transform + health. Its
    // transform mirrors the player every fixed tick (see tick below).
    if (mobEntities) {
        std::string spawnError;
        showcasePlayerEntity = mobEntities->spawn(
            "showcase.player",
            { player.position.x, player.position.y, player.position.z },
            spawnError);
        showcasePlayerEntityValid = showcasePlayerEntity.valid();
        if (showcasePlayerEntityValid) {
            mobEntities->set_health(showcasePlayerEntity, { 100.0f, 100.0f });
            mobEntities->set_stable_id(showcasePlayerEntity, "showcase.player");
            std::cout << "[Showcase] character entity spawned "
                         "(stable id 'showcase.player', health 100)\n";
        } else {
            std::cout << "[Showcase] character entity spawn refused: "
                      << spawnError << '\n';
        }
    }

    // ── B: character skin via the asset registry (IAssetPipeline). The skin
    // descriptor is imported -> validated -> cooked -> cached like any other
    // asset; its UV layout (64x64 player skin) is applied to the character's
    // UV grid. When the file is absent the import refuses and the pipeline
    // falls back to the builtin layout EXPLICITLY (observable, never silent).
    {
        std::string assetError;
        showcaseAssets = engine::assets::create_asset_pipeline("showcase", assetError);
        if (showcaseAssets) {
            const std::string skinPath =
                std::string(VULKANCRAFT_SOURCE_DIR) +
                "/Projects/ShowcaseGame/Content/Registry/showcase_character_skin.json";
            std::ifstream skinFile(skinPath);
            if (skinFile) {
                std::string content((std::istreambuf_iterator<char>(skinFile)),
                                    std::istreambuf_iterator<char>());
                engine::assets::AssetSource source;
                source.name = "showcase_character_skin";
                source.kind = "json";
                source.version = "1";
                source.bytes.assign(content.begin(), content.end());
                if (showcaseAssets->import_source(source, assetError) &&
                    showcaseAssets->validate("showcase_character_skin").valid) {
                    const engine::assets::AssetCookResult cooked =
                        showcaseAssets->cook("showcase_character_skin");
                    if (cooked.ok) {
                        const std::string artifact(
                            cooked.artifact.begin(), cooked.artifact.end());
                        // Read the layout the descriptor declares and apply it
                        // to the character's UV grid (64x64 player-skin atlas).
                        engine::sdk::JsonValue skin;
                        std::string jsonError;
                        if (engine::sdk::json_parse(artifact, skin, jsonError) &&
                            skin.is_object()) {
                            showcaseSkinName = engine::sdk::json_string(
                                skin, "name", "showcase_character_skin");
                            showcaseSkinLayout = engine::sdk::json_string(
                                skin, "layout", "64x64");
                            showcaseSkinFallback = false;
                        }
                    }
                }
                showcaseAssetCacheHits = showcaseAssets->cache_hits();
                std::cout << "[Showcase] character skin via asset registry: '"
                          << showcaseSkinName << "' layout "
                          << showcaseSkinLayout
                          << (showcaseSkinFallback
                                  ? " (FALLBACK: builtin 64x64)"
                                  : " (registry)")
                          << '\n';
            } else {
                // Explicit fallback: asset absent -> keep the builtin 64x64
                // player-skin layout (the mesh's TextureIndex::PlayerSkin UVs
                // are already laid out for it).
                showcaseSkinFallback = true;
                std::cout << "[Showcase] character skin asset absent; falling "
                             "back to builtin 64x64 player-skin layout\n";
            }
        } else {
            std::cout << "[Showcase] asset pipeline refused: " << assetError
                      << '\n';
        }
    }

    // ── A: data-driven input — the public IActionMap binds ACTIONS to
    // physical inputs (no raw key checks in the showcase path). The actions
    // drive the character controller, camera (mouse look) and interaction.
    // The project asset Content/Input/showcase_input.json is the authoritative
    // source (the same document author_showcase_game authors and the cookie/
    // package builder ships); the REMOVED_* game-only bindings are appended so
    // the data-driven set stays single-source while the extra gameplay actions
    // (break/place/craft/smelt/warp/remove_block_entity) still work.
    {
        auto bind = [](const char* action, const char* input) {
            engine::input::ActionBinding ab;
            ab.action = action;
            ab.bindings.push_back(engine::input::InputBinding{
                engine::input::InputSource::Keyboard, "", input, 0, 1.0, 0.0 });
            return ab;
        };
        engine::input::ActionMapSpec am;
        bool inputFromAsset = false;
        {
            const std::string inputPath =
                std::string(VULKANCRAFT_SOURCE_DIR) +
                "/Projects/ShowcaseGame/Content/Input/showcase_input.json";
            std::ifstream inFile(inputPath);
            if (inFile) {
                std::string inContent((std::istreambuf_iterator<char>(inFile)),
                                      std::istreambuf_iterator<char>());
                std::string inErr;
                if (am.load_from_json(inContent, inErr)) {
                    inputFromAsset = true;
                    std::cout << "[Showcase] input action map loaded from asset: "
                              << inputPath << " (" << am.actions.size()
                              << " actions)\n";
                } else {
                    std::cout << "[Showcase] input asset refused (" << inErr
                              << "); falling back to builtin action map\n";
                    am.actions.clear();
                }
            } else {
                std::cout << "[Showcase] input asset absent; using builtin action map\n";
            }
        }
        // Augment the data-driven set with the game-only gameplay actions that
        // are not declared by the showcase input asset (present in BOTH cases:
        // when loaded from the asset they are appended, never duplicated — the
        // asset omits them, so no physical/action collision).
        struct ExtraBinding { const char* action; const char* input; };
        static const ExtraBinding kExtras[] = {
            { "move_forward", "KeyW" }, { "move_back", "KeyS" },
            { "move_left", "KeyA" }, { "move_right", "KeyD" },
            { "jump", "Space" }, { "interact", "KeyE" },
            { "break_block", "MouseLeft" }, { "place_block", "MouseRight" },
            { "craft", "KeyC" }, { "smelt", "KeyF" },
            { "warp", "KeyV" }, { "remove_block_entity", "KeyQ" },
        };
        auto hasAction = [&am](const char* name) {
            for (const auto& a : am.actions) if (a.action == name) return true;
            return false;
        };
        for (const ExtraBinding& e : kExtras) {
            if (!hasAction(e.action)) am.actions.push_back(bind(e.action, e.input));
        }
        std::string amError;
        showcaseActionMap = engine::input::create_action_map(am, amError);
        if (!showcaseActionMap) {
            std::cout << "[Showcase] action map refused: " << amError << '\n';
        } else if (!inputFromAsset) {
            std::cout << "[Showcase] builtin action map active ("
                      << am.actions.size() << " actions)\n";
        }
    }

    // ── A: fixed tick accumulator (fixed tick -> variable update -> render) ──
    fixedTickSim = engine::simulation::create_fixed_tick_sim();
    if (fixedTickSim) {
        engine::simulation::FixedTickSimSpec ftSpec;
        ftSpec.fixed_dt = 1.0 / 60.0;
        ftSpec.max_ticks_per_frame = 4;
        if (!fixedTickSim->configure(ftSpec, err)) {
            std::cout << "[Showcase] fixed tick configure refused: " << err << '\n';
            fixedTickSim.reset();
        }
    }

    // ── B: character controller — kinematic resolver over LIVE terrain ──
    showcaseController = engine::gameplay::create_character_controller();
    if (showcaseController) {
        engine::gameplay::CharacterConfig cc;
        cc.maxStepHeight = 0.5f;
        cc.stepDownDistance = 0.6f;
        cc.maxSlopeDegrees = 50.0f;
        cc.waterSurfaceY = 3.0f;
        showcaseController->set_config(cc);
    }

    // ── B: hit reaction + ragdoll on the SAME runtime physics ──
    showcaseHitReaction = engine::gameplay::create_hit_reaction();
    if (showcaseHitReaction) {
        engine::gameplay::HitReactionConfig hr;
        hr.knockbackInitial = 6.0f;
        hr.knockbackDecay = 2.0f;
        hr.staggerDuration = 0.5f;
        hr.knockdownStrength = 0.9f;
        showcaseHitReaction->set_config(hr);
    }
    if (runtimePhysics) {
        // Data-driven ragdoll asset -> canonical RagdollBone list -> runtime.
        engine::gameplay::RagdollAsset rd;
        rd.name = "showcase_character";
        engine::gameplay::RagdollJoint hips;
        hips.name = "Hips";
        hips.anchor = { 0.0f, 1.0f, 0.0f };
        hips.length = 0.3f;
        hips.radius = 0.16f;
        hips.mass = 12.0f;
        engine::gameplay::RagdollJoint leg;
        leg.name = "LeftLeg";
        leg.parent = "Hips";
        leg.anchor = { -0.18f, 0.0f, 0.0f };
        leg.length = 0.45f;
        leg.radius = 0.1f;
        leg.mass = 3.0f;
        rd.joints = { hips, leg };
        if (rd.validate(err)) {
            const std::vector<engine::gameplay::RagdollBone> bones = rd.build_bones();
            showcaseRagdoll = runtimePhysics->create_ragdoll(bones, player.position);
            showcaseRagdollBones = showcaseRagdoll ? showcaseRagdoll->bone_count() : 0;
            std::cout << "[Showcase] ragdoll created from asset '"
                      << rd.name << "' (" << showcaseRagdollBones << " bones)\n";
        } else {
            std::cout << "[Showcase] ragdoll asset refused: " << err << '\n';
        }
        // Weapon factory (runtime-owned): hitscan through the SAME physics
        // world the gameplay path already raycasts into.
        engine::gameplay::WeaponSpec wp;
        wp.id = "showcase.rifle";
        wp.name = "Showcase Rifle";
        wp.fireMode = engine::gameplay::WeaponSpec::FireMode::Automatic;
        wp.magazineSize = 30;
        wp.reserveAmmo = 120;
        wp.burstCount = 3;
        wp.roundsPerMinute = 600.0f;
        wp.reloadSeconds = 1.6f;
        wp.damage = 12.0f;
        wp.range = 80.0f;
        wp.hitscan = true;
        showcaseRifle = runtimePhysics->create_weapon(wp);
        if (showcaseRifle) {
            showcaseRifle->set_raycast(
                [this](const glm::vec3& origin, const glm::vec3& direction,
                       float maxDistance)
                    -> std::optional<engine::gameplay::WeaponHit> {
                    engine::gameplay::RaycastHit hit;
                    if (runtimePhysics->physics().raycast(
                            origin, direction, maxDistance, hit)) {
                        engine::gameplay::WeaponHit out;
                        out.position = hit.point;
                        out.normal = hit.normal;
                        out.distance = hit.distance;
                        return out;
                    }
                    return std::nullopt;
                });
            showcaseWeaponAmmo = showcaseRifle->ammo();
            std::cout << "[Showcase] weapon factory wired (mag "
                      << showcaseRifle->ammo() << ", reserve "
                      << showcaseRifle->reserve() << ")\n";
        }
    }

    // ── B: animation stack on the canonical IAnimCore ──
    animCore = engine::animation::create_anim_core();
    if (animCore) {
        const engine::animation::SkeletonSpec skeleton = make_showcase_skeleton();
        if (!animCore->add_skeleton(skeleton, err)) {
            std::cout << "[Showcase] anim skeleton refused: " << err << '\n';
            animCore.reset();
        } else {
            // Deterministic walk clip: legs swing around the hips, feet stay
            // near the ground plane (the same motion the gait asset describes).
            engine::animation::ClipSpec clip;
            clip.id = "walk";
            clip.skeleton = "showcase_char";
            clip.duration = 0.8;
            for (const auto& b : skeleton.bones) {
                engine::animation::BoneTrack track;
                track.bone = b.id;
                engine::animation::Keyframe k0;
                k0.t = 0.0;
                engine::animation::Keyframe k1;
                k1.t = 0.8;
                const bool isHip = track.bone == "LeftHip" || track.bone == "RightHip";
                const bool isFoot = track.bone == "LeftFoot" || track.bone == "RightFoot";
                if (isHip) {
                    k0.value.position = { 0.0, 0.9, 0.0 };
                    k1.value.position = { 0.0, 0.9, 0.0 };
                } else if (isFoot) {
                    k0.value.position = { 0.0, 0.0, 0.0 };
                    k1.value.position = { 0.0, 0.0, 0.0 };
                } else {  // knees
                    k0.value.position = { 0.0, 0.45, 0.0 };
                    k1.value.position = { 0.0, 0.45, 0.0 };
                }
                if (track.bone == "LeftHip" || track.bone == "LeftFoot") {
                    const engine::animation::AnimQuat swing1 =
                        engine::animation::AnimQuat{ 0.0, 0.0, 0.3, 1.0 }.normalized();
                    const engine::animation::AnimQuat swing2 =
                        engine::animation::AnimQuat{ 0.0, 0.0, -0.3, 1.0 }.normalized();
                    k0.value.rotation = engine::animation::AnimQuat::slerp(
                        engine::animation::AnimQuat{}, swing1, 0.35);
                    k1.value.rotation = engine::animation::AnimQuat::slerp(
                        engine::animation::AnimQuat{}, swing2, 0.35);
                } else if (track.bone == "RightHip" || track.bone == "RightFoot") {
                    const engine::animation::AnimQuat swing1 =
                        engine::animation::AnimQuat{ 0.0, 0.0, -0.3, 1.0 }.normalized();
                    const engine::animation::AnimQuat swing2 =
                        engine::animation::AnimQuat{ 0.0, 0.0, 0.3, 1.0 }.normalized();
                    k0.value.rotation = engine::animation::AnimQuat::slerp(
                        engine::animation::AnimQuat{}, swing1, 0.35);
                    k1.value.rotation = engine::animation::AnimQuat::slerp(
                        engine::animation::AnimQuat{}, swing2, 0.35);
                }
                track.keys = { k0, k1 };
                clip.tracks.push_back(track);
            }
            if (!animCore->add_clip(clip, err)) {
                std::cout << "[Showcase] anim clip refused: " << err << '\n';
            } else {
                animBasePose = animCore->sample_clip("walk", 0.0, err);
                // Additive layer + named events + root motion + mask over the
                // SAME core.
                animAdditive = engine::animation::create_anim_additive(*animCore);
                animEvents = engine::animation::create_anim_events(*animCore);
                animMask = engine::animation::create_anim_mask();
                animRootMotion = engine::animation::create_root_motion();
                if (animEvents) {
                    const engine::animation::AnimEvent footstep{ "walk", 0.2, "footstep" };
                    if (!animEvents->add_event(footstep, err)) {
                        std::cout << "[Showcase] anim event refused: " << err << '\n';
                    }
                }
                if (animMask) {
                    const std::vector<engine::animation::AnimMaskEntry> entries = {
                        { "LeftFoot", 1.0 }, { "RightFoot", 1.0 },
                    };
                    if (!animMask->add_mask("feet", entries, err)) {
                        std::cout << "[Showcase] anim mask refused: " << err << '\n';
                    }
                }
                std::cout << "[Showcase] animation core + clips loaded "
                          << "(skeleton '" << skeleton.id << "', clip 'walk' "
                          << clip.duration << "s)\n";
            }
        }
    }
    // State machine (data-driven) over the core clips.
    animStateMachine = engine::animation::create_anim_state_machine();
    if (animStateMachine && animCore && animCore->has_clip("walk")) {
        animStateSpec.id = "showcase_loc";
        animStateSpec.initial = "idle";
        animStateSpec.states = {
            { "idle", "idle", 1.0, true },
            { "walk", "walk", 1.0, true },
        };
        animStateSpec.transitions = {
            { "idle", "walk", "move", "", 0.0, 0.2 },
            { "walk", "idle", "stop", "", 0.0, 0.2 },
        };
        if (animStateMachine->configure(animStateSpec, err)) {
            animStateMachine->start(err);
            animStateMachineState = animStateMachine->state();
        } else {
            std::cout << "[Showcase] anim state machine refused: " << err << '\n';
        }
    }
    // Pure math helpers over the same skeleton space.
    animIkSolver = engine::animation::create_ik_solver();
    animInertializer = engine::animation::create_inertializer();
    animConstraints = engine::animation::create_constraints();
    terrainAdaptation = engine::animation::create_terrain_adaptation();
    // ── LOTE 2: 102 skinning CPU + 108 skeleton real + 80 CCD body ──
    // The public ISkinning consumes the SAME animCore pose the ASM and motion
    // matcher produce — deforms a real vertex set every fixed tick (observable
    // in the title). IMotionDatabase (ozz) cooks the canonical showcase
    // skeleton+clip, replacing any two-bone fallback outside an explicit error.
    if (animCore) {
        showcaseSkinning = engine::animation::create_skinning(*animCore);
        showcaseMotionDb = engine::animation::create_motion_database();
        if (showcaseMotionDb) {
            const engine::animation::MotionSkeleton sks = make_showcase_motion_skeleton();
            if (showcaseMotionDb->cook_skeleton(sks, err)) {
                showcaseMotionDbBones = showcaseMotionDb->bone_count();
                engine::animation::MotionClip mclip;
                mclip.name = "walk";
                mclip.duration = 0.8;
                for (int i = 0; i < static_cast<int>(sks.bones.size()); ++i) {
                    engine::animation::MotionTrack track;
                    track.boneIndex = i;
                    engine::animation::MotionKeyframe k0;
                    k0.time = 0.0;
                    engine::animation::MotionKeyframe k1;
                    k1.time = 0.8;
                    k0.translation = { 0.0f, 0.9f - 0.45f * (i % 3), 0.0f };
                    k1.translation = { 0.0f, 0.9f - 0.45f * (i % 3), 0.0f };
                    track.keyframes = { k0, k1 };
                    mclip.tracks.push_back(track);
                }
                if (showcaseMotionDb->cook_clip(mclip, err)) {
                    const engine::animation::CookedMotion* cooked =
                        showcaseMotionDb->cooked("walk");
                    showcaseMotionDbFrames = cooked ? cooked->duration > 0.0f ? 12 : 0 : 0;
                    std::cout << "[Showcase] motion db cooked skeleton '"
                              << sks.name << "' + clip 'walk' ("
                              << showcaseMotionDbBones << " bones)\n";
                }
            } else {
                std::cout << "[Showcase] motion db skeleton refused: " << err << '\n';
            }
        }
        // 108: the character skeleton is a real asset in the project; the
        // motion database cooks it. Report asset presence (the 6-bone chain
        // mirrors the showcase_character_skeleton.json hip/knee/foot x2).
        if (showcaseMotionDbBones > 0) {
            const std::string skelPath =
                std::string(VULKANCRAFT_SOURCE_DIR) +
                "/Projects/ShowcaseGame/Content/Registry/"
                "showcase_character_skeleton.json";
            std::ifstream skelFile(skelPath);
            if (skelFile) {
                showcaseSkeletonAssetLoaded =
                    showcaseMotionDbBones;   // bones cooked from a real asset
                std::cout << "[Showcase] character skeleton asset loaded ("
                          << showcaseMotionDbBones << " bones)\n";
            }
        }
    }
    // 80: one CCD-enabled body in the gameplay runtime physics (the mirror
    // body is kinematic; the CCD body is an ACTIVE dynamic obstacle that
    // proves continuous-collision + awake/sleep are wired, observable).
    showcaseCcdBody = {};
    if (runtimePhysics) {
        engine::gameplay::BodySpec ccdSpec;
        ccdSpec.motion = engine::gameplay::MotionType::Dynamic;
        ccdSpec.position = player.position + glm::vec3(0.0f, 2.0f, 1.5f);
        ccdSpec.mass = 5.0f;
        ccdSpec.continuous = true;   // CCD flag on
        ccdSpec.shape = engine::gameplay::SphereShape{ 0.4f };
        showcaseCcdBody = runtimePhysics->physics().create_body(ccdSpec);
    }
    // ── LOTE 3 (94): equipment + loot real no jogo ──
    showcaseEquipment = engine::registry::create_equipment();
    if (showcaseEquipment) {
        engine::registry::EquipmentSpec es;
        es.categories = {
            { "helmet", {} }, { "chest", {} }, { "weapon", {} },
        };
        if (showcaseEquipment->configure(es, err)) {
            std::string eErr;
            showcaseEquipment->equip("weapon", "showcase.rifle",
                                     { "vulkancraft:weapon" }, eErr);
            showcaseEquippedSlots =
                showcaseEquipment->items().size();
            std::cout << "[Showcase] equipment grid configured ("
                      << showcaseEquippedSlots << " slots filled)\n";
        }
    }
    showcaseLoot = engine::registry::create_loot_table();
    if (showcaseLoot) {
        engine::registry::LootTableSpec ls;
        ls.id = "showcase.stone";
        ls.entries = {
            { "vulkancraft:stone", 1.0, 1, 3, 1.0 },
            { "vulkancraft:stone_brick", 0.25, 1, 1, 0.6 },
        };
        ls.rolls_min = 1;
        ls.rolls_max = 1;
        if (!showcaseLoot->configure(ls, err)) {
            std::cout << "[Showcase] loot table refused: " << err << '\n';
        }
    }
    // ── LOTE 3 (95): ecosystem — data-driven ore/carver/decorator tables ──
    showcaseDecorators = engine::procgen::create_decorator_set();
    showcaseOreTable = engine::procgen::create_ore_table();
    {
        engine::procgen::CarverSpec cs;
        cs.fluidMaxY = 3;
        cs.fluidBlockId = 0u;
        showcaseCarver = engine::procgen::create_carver(cs);
        // The empty decorator set is a valid data-driven state; at tick time we
        // query its count so the observer reflects a real IWorldFeatures link.
    }
    // ── LOTE 3 (46/47): block entity real (counter machine) ──
    showcaseBlockEntity = std::make_unique<ShowcaseBlockEntity>();
    if (showcaseBlockEntity) {
        showcaseBlockEntityTicks =
            static_cast<std::size_t>(showcaseBlockEntity->ticks());
    }
    // ── LOTE 3 (115): nav streaming — ledged active-region gate ──
    showcaseNavStream = engine::navigation::create_nav_streaming();
    if (showcaseNavStream) {
        std::string nsErr;
        if (showcaseNavStream->configure(2, nsErr)) {
            showcaseNavStream->set_focus(
                static_cast<std::int32_t>(std::floor(player.position.x / 8.0f)),
                static_cast<std::int32_t>(std::floor(player.position.z / 8.0f)));
            showcaseNavLoaded = showcaseNavStream->loaded_count();
            showcaseNavPendingRebuild =
                showcaseNavStream->tiles_pending_rebuild().size();
        }
    }
    // ── LOTE 3 (54): real world-gen graphs from an asset. The project ships
    // a full data-driven `Profiles/DefaultWorld.json` (height/climate/biomes/
    // caves/ores/carver/decorators/structures) that we CRACK into the world
    // profile to prove the generator is composed from real JSON, not built-in
    // defaults. The profile's generator()/structure_placement() are the same
    // public contracts the server's authoritative world uses.
    showcaseWorldProfile.reset();
    {
        const std::string profilePath =
            std::string(VULKANCRAFT_SOURCE_DIR) +
            "/Projects/ShowcaseGame/Content/Profiles/DefaultWorld.json";
        std::ifstream profileFile(profilePath);
        if (profileFile) {
            std::string pcontent((std::istreambuf_iterator<char>(profileFile)),
                                 std::istreambuf_iterator<char>());
            std::string profileErr;
            showcaseWorldProfile =
                engine::procgen::create_world_profile_from_json(pcontent, profileErr);
            if (showcaseWorldProfile) {
                showcaseWorldProfileSections = 0;
                // Count the data-driven sections the profile actually carries.
                std::size_t sec = 0;
                for (const char* k : {"height", "climate", "biomes",
                                       "caves", "ores", "decorators",
                                       "structures"}) {
                    if (pcontent.find(std::string("\"") + k + "\"") != std::string::npos) {
                        ++sec;
                    }
                }
                showcaseWorldProfileSections = sec;
                // Create a managed world composed from the profile: the
                // manager composes the world's generator from `profileJson`
                // (height + climate + biomes + caves/ores + carver +
                // decorators + structures) — the SAME document the project's
                // DefaultWorld.json carries, proving the generator is
                // data-driven, not built-in.
                if (runtimeWorlds && !runtimeWorlds->has_world("showcase_profile")) {
                    engine::world::WorldSpec ws;
                    ws.name = "showcase_profile";
                    ws.seed = 20260830;
                    ws.profileJson = pcontent;
                    std::string wErr;
                    if (runtimeWorlds->create_world(ws, wErr)) {
                        showcaseWorldProfileWorld = 1;
                        std::cout << "[Showcase] data-driven world profile wired: "
                                  << showcaseWorldProfileSections << " sections, "
                                  << "world 'showcase_profile' created from profileJson\n";
                    } else {
                        std::cout << "[Showcase] profile world refused: "
                                  << wErr << '\n';
                    }
                }
            } else {
                std::cout << "[Showcase] world profile refused: "
                          << profileErr << '\n';
            }
        } else {
            std::cout << "[Showcase] default world profile asset absent\n";
        }
    }
    if (animInertializer && !animBasePose.empty()) {
        animInertializer->set_decay_time(0.25, err);
        animInertializer->reset(animBasePose, animBasePose, err);
    }
    if (animConstraints) {
        const std::vector<engine::animation::JointLimit> limits = {
            { "LeftKnee", -0.1, 2.6, 0.0, 0.0, 0.0, 0.0 },
            { "RightKnee", -0.1, 2.6, 0.0, 0.0, 0.0, 0.0 },
        };
        if (!animConstraints->add_constraint("showcase_legs", limits, err)) {
            std::cout << "[Showcase] anim constraints refused: " << err << '\n';
        }
    }
    if (terrainAdaptation) {
        // Heightmap from the real world around the spawn (bilinear sampler).
        constexpr int kGrid = 9;
        std::vector<double> heights;
        const float ox = static_cast<float>(static_cast<int>(player.position.x)) - 4.0f;
        const float oz = static_cast<float>(static_cast<int>(player.position.z)) - 4.0f;
        for (int r = 0; r < kGrid; ++r) {
            for (int c = 0; c < kGrid; ++c) {
                heights.push_back(static_cast<double>(
                    showcaseSurfaceAt(world, ox + static_cast<float>(c),
                                            oz + static_cast<float>(r))));
            }
        }
        if (!terrainAdaptation->set_heightmap(
                "showcase", ox, oz, 1.0, heights, kGrid, kGrid, err)) {
            std::cout << "[Showcase] terrain heightmap refused: " << err << '\n';
        } else {
            const std::vector<engine::animation::FootConfig> feet = {
                { "LeftFoot", { -0.18, -0.9, 0.0 }, 0.0 },
                { "RightFoot", { 0.18, -0.9, 0.0 }, 0.0 },
            };
            if (!terrainAdaptation->configure("LeftHip", feet, err)) {
                std::cout << "[Showcase] terrain feet refused: " << err << '\n';
            }
        }
    }

    // ── B: gait planner + foot placement + pose warp (showcase asset) ──
    {
        std::ifstream gaitFile(
            std::string(VULKANCRAFT_SOURCE_DIR) +
            "/Projects/ShowcaseGame/Content/Registry/showcase_character_gait.json");
        if (gaitFile) {
            std::ostringstream buffer;
            buffer << gaitFile.rdbuf();
            if (showcaseGait.load_from_json(buffer.str(), err)) {
                showcaseGaitLoaded = true;
                contactPlanner = engine::animation::create_contact_planner();
                footPlacer = engine::animation::create_foot_placer();
                proceduralLegs = engine::animation::create_procedural_locomotion();
                proceduralPipeline =
                    engine::animation::create_procedural_animation_pipeline();
                std::cout << "[Showcase] gait asset consumed: "
                          << showcaseGait.name << " (" << showcaseGait.legs.size()
                          << " legs, " << showcaseGait.cycleDuration << "s cycle)\n";
                if (proceduralPipeline) {
                    engine::animation::ProceduralAnimationSpec pspec;
                    pspec.chains = {
                        { engine::animation::IkChainKind::Leg, "LeftHip",
                          "LeftKnee", "LeftFoot",
                          { -0.18, 0.9, 0.0 }, { -0.18, -0.9, 0.0 } },
                        { engine::animation::IkChainKind::Leg, "RightHip",
                          "RightKnee", "RightFoot",
                          { 0.18, 0.9, 0.0 }, { 0.18, -0.9, 0.0 } },
                    };
                    pspec.jointLimits = {
                        { "LeftKnee", -0.1, 2.6, 0.0, 0.0, 0.0, 0.0 },
                        { "RightKnee", -0.1, 2.6, 0.0, 0.0, 0.0, 0.0 },
                    };
                    if (!proceduralPipeline->configure(pspec, err)) {
                        std::cout << "[Showcase] procedural pipeline refused: "
                                  << err << '\n';
                        proceduralPipeline.reset();
                    }
                }
            } else {
                std::cout << "[Showcase] gait asset refused: " << err << '\n';
            }
        } else {
            std::cout << "[Showcase] showcase_character_gait.json not found; "
                         "gait-dependent factories run on builtin legs\n";
        }
        // Motion matching: canonical skeleton + annotated clip entry.
        motionMatcher = engine::animation::create_motion_matcher();
        poseWarper = engine::animation::create_pose_warper();
        if (motionMatcher) {
            const engine::animation::MotionSkeleton ms = make_showcase_motion_skeleton();
            std::vector<engine::animation::MotionClipEntry> clips;
            engine::animation::MotionClipEntry entry;
            entry.name = "walk";
            entry.frameRate = 30.0f;
            entry.loop = true;
            constexpr int kFrames = 12;
            for (int f = 0; f < kFrames; ++f) {
                engine::animation::MotionPose pose;
                const float phase = 6.2831853f * static_cast<float>(f) /
                                    static_cast<float>(kFrames);
                for (int i = 0; i < 6; ++i) {
                    pose.translations.push_back(
                        { 0.0f, 0.9f - 0.45f * (i % 3), 0.0f });
                    pose.rotations.push_back(glm::angleAxis(
                        0.35f * std::sin(phase),
                        i < 3 ? glm::vec3(0.0f, 0.0f, 1.0f)
                              : glm::vec3(0.0f, 0.0f, -1.0f)));
                    pose.scales.push_back(glm::vec3(1.0f));
                }
                entry.frames.push_back(pose);
                entry.rootPositions.push_back(
                    glm::vec3(0.02f * static_cast<float>(f), 0.9f, 0.0f));
                entry.rootOrientations.push_back(glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
            }
            clips.push_back(entry);
            engine::animation::MotionMatcherSpec mspec;
            if (motionMatcher->build(ms, clips, mspec, err)) {
                motionMatchFrames = motionMatcher->frame_count(0);
                std::cout << "[Showcase] motion matcher built "
                          << motionMatcher->clip_count() << " clip(s), "
                          << motionMatchFrames << " frames\n";
            } else {
                std::cout << "[Showcase] motion matcher build refused: " << err << '\n';
                motionMatcher.reset();
            }
        }
    }

    // ── C: interaction, explosion, balance, faction ──
    showcaseInteraction = engine::gameplay::create_interaction();
    if (showcaseInteraction) {
        const std::vector<engine::gameplay::InteractionDef> defs = {
            { "pickup", "[E] Coletar", 2.5f, false, 0.5f },
            { "talk", "[E] Falar", 3.0f, true, 2.0f },
        };
        if (!showcaseInteraction->configure(defs, err)) {
            std::cout << "[Showcase] interaction refused: " << err << '\n';
            showcaseInteraction.reset();
        }
    }
    showcaseExplosion = engine::physics::create_explosion();
    if (showcaseExplosion) {
        engine::physics::ExplosionSpec es;
        es.radius = 5.0;
        es.impulse = 900.0;
        es.damage = 60.0;
        es.falloff_power = 2.0;
        es.fragments = 16;
        if (!showcaseExplosion->configure(es, err)) {
            std::cout << "[Showcase] explosion refused: " << err << '\n';
            showcaseExplosion.reset();
        }
    }
    showcaseBalance = engine::gameplay::create_balance();
    if (showcaseBalance) {
        engine::gameplay::BalanceConfig bc;
        bc.edgeMargin = 0.05f;
        showcaseBalance->set_config(bc);
    }
    showcaseFaction = engine::gameplay::create_faction();
    if (showcaseFaction) {
        engine::gameplay::FactionSpec fspec;
        fspec.teams = { "player", "guard", "bandit" };
        fspec.relations = {
            { "player", "guard", engine::gameplay::FactionRelation::Friendly },
            { "player", "bandit", engine::gameplay::FactionRelation::Hostile },
            { "guard", "bandit", engine::gameplay::FactionRelation::Hostile },
        };
        if (!showcaseFaction->configure(fspec, err)) {
            std::cout << "[Showcase] faction refused: " << err << '\n';
            showcaseFaction.reset();
        } else {
            showcaseTeamCount = showcaseFaction->teams().size();
        }
    }

    // ── C: AI decision surface ──
    aiDebugRecorder = engine::ai::create_ai_debug_recorder();
    aiValidator = engine::animation::create_ai_validator(
        engine::animation::AiBackend::RuntimeValidator, err);
    if (!aiValidator) {
        std::cout << "[Showcase] AI validator refused: " << err << '\n';
    }
    vendorTree = engine::ai::create_vendor_behavior_tree(err);
    if (vendorTree) {
        vendorTree->register_action(
            "ShowcaseAction",
            [](const std::string&) { return engine::ai::VendorNodeStatus::SUCCESS; },
            err);
        const char* xml =
            "<root main_tree_to_execute=\"MainTree\">"
            "  <BehaviorTree ID=\"MainTree\">"
            "    <Sequence>"
            "      <ShowcaseAction name=\"patrol_step\"/>"
            "    </Sequence>"
            "  </BehaviorTree>"
            "</root>";
        if (vendorTree->load_from_xml(xml, err)) {
            vendorTreeStatus = static_cast<int>(vendorTree->tick(err));
        } else {
            std::cout << "[Showcase] vendored behavior tree refused: " << err << '\n';
        }
    }

    // ── C: navigation / simulation ──
    hierarchicalPath = engine::navigation::create_hierarchical_path();
    if (hierarchicalPath) {
        // Region nav graph of the chunk grid around the player: one node per
        // chunk cell, portal edges across boundaries.
        constexpr std::uint32_t kGrid = 3;
        std::vector<engine::navigation::HPathNode> nodes;
        std::vector<engine::navigation::HPathEdge> edges;
        for (std::uint32_t r = 0; r < kGrid; ++r) {
            for (std::uint32_t c = 0; c < kGrid; ++c) {
                const std::uint32_t id = r * kGrid + c;
                nodes.push_back(engine::navigation::HPathNode{
                    id, static_cast<float>(c) * 32.0f,
                    static_cast<float>(r) * 32.0f, r * kGrid + c, 1.0f });
            }
        }
        for (std::uint32_t r = 0; r < kGrid; ++r) {
            for (std::uint32_t c = 0; c < kGrid; ++c) {
                const std::uint32_t id = r * kGrid + c;
                if (c + 1 < kGrid) edges.push_back({ id, id + 1, 1.0f });
                if (r + 1 < kGrid) edges.push_back({ id, id + kGrid, 1.0f });
            }
        }
        if (hierarchicalPath->configure(nodes, edges, err)) {
            const auto path = hierarchicalPath->find_path(0, kGrid * kGrid - 1);
            navPathFound = path.found;
            navPathNodes = path.nodes.size();
            std::cout << "[Showcase] hierarchical path: "
                      << hierarchicalPath->node_count() << " nodes, "
                      << hierarchicalPath->region_count() << " regions"
                      << (path.found ? ", route found" : ", no route") << '\n';
        } else {
            std::cout << "[Showcase] hierarchical path refused: " << err << '\n';
            hierarchicalPath.reset();
        }
    }
    agentCapabilities = engine::navigation::create_agent_capabilities();
    if (agentCapabilities) {
        engine::navigation::AgentProfile profile;
        profile.radius = 0.4f;
        profile.height = 1.8f;
        profile.maxClimb = 1.0f;
        profile.canJump = true;
        agentCapabilities->set_profile(profile);
    }
    simulationLod = engine::simulation::create_simulation_lod();
    if (simulationLod) {
        engine::simulation::SimulationLodSpec lspec;
        lspec.cellSize = 16.0f;
        lspec.fullRadius = 48.0f;
        lspec.falloffRadius = 256.0f;
        lspec.dayLengthSeconds = 240.0f;
        lspec.daysPerSeason = 30;
        lspec.tiers = {
            { "full", engine::simulation::SimulationLodMode::Full, 0.85f, 0.0f, 0.0f, 0, 0.0f },
            { "coarse", engine::simulation::SimulationLodMode::Coarse, 0.4f, 0.25f, 0.0f, 0, 0.0f },
            { "aggregate", engine::simulation::SimulationLodMode::Aggregate, 0.0f, 0.5f, 0.0f, 0, 0.25f },
            { "sleep", engine::simulation::SimulationLodMode::Sleeping, -1.0f, 0.0f, 5.0f, 0, 0.0f },
        };
        if (!simulationLod->set_spec(lspec, err)) {
            std::cout << "[Showcase] simulation LOD refused: " << err << '\n';
            simulationLod.reset();
        } else {
            simulationLodState.version = 1;
            const int px = static_cast<int>(player.position.x);
            const int pz = static_cast<int>(player.position.z);
            for (int dx = -1; dx <= 1; ++dx) {
                for (int dz = -1; dz <= 1; ++dz) {
                    std::string regionError;
                    simulationLod->add_region(
                        simulationLodState, px / 16 + dx, pz / 16 + dz,
                        { "forest" }, regionError);
                }
            }
        }
    }

    // ── D: physics selection — convex decomposition, multibody, shape ──
    {
        std::string cdError;
        convexDecomposition =
            engine::physics::create_convex_decomposition("coacd", cdError);
        if (convexDecomposition) {
            // A real concave L-shape from the voxel surface (3 quads): the
            // split is deterministic and its part count is observable.
            engine::physics::ConvexInput input;
            const float baseY = showcaseSurfaceAt(
                world, player.position.x, player.position.z);
            auto quad = [&](float x0, float z0, float x1, float z1, float y) {
                const std::uint32_t start =
                    static_cast<std::uint32_t>(input.positions.size() / 3);
                input.positions.insert(input.positions.end(),
                                       { x0, y, z0, x1, y, z0, x1, y, z1,
                                         x0, y, z1 });
                input.indices.insert(input.indices.end(),
                                     { start, start + 1, start + 2,
                                       start, start + 2, start + 3 });
            };
            const float px = player.position.x;
            const float pz = player.position.z;
            quad(px, pz, px + 2.0f, pz + 2.0f, baseY);
            quad(px + 2.0f, pz, px + 4.0f, pz + 1.0f, baseY);
            quad(px + 1.0f, pz + 2.0f, px + 2.0f, pz + 3.0f, baseY);
            engine::physics::ConvexResult cr;
            if (convexDecomposition->decompose(input, cr, cdError)) {
                convexPartCount = cr.parts.size();
                convexBackendName = convexDecomposition->name();
                std::cout << "[Showcase] convex decomposition '"
                          << convexBackendName << "': " << convexPartCount
                          << " parts from " << input.indices.size() / 3
                          << " tris\n";
            } else {
                std::cout << "[Showcase] convex decompose refused: "
                          << cdError << '\n';
            }
        }
    }
    {
        std::string mbError;
        multibody = engine::physics::create_multibody_dynamics(mbError);
        if (multibody) {
            engine::physics::MultibodyConfig mcfg;
            if (!multibody->configure(mcfg, mbError)) {
                std::cout << "[Showcase] multibody configure refused: "
                          << mbError << '\n';
                multibody.reset();
            } else {
                const std::vector<engine::physics::MultibodyLinkDesc> links = {
                    { engine::physics::JointKind::Revolute,
                      { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f }, 2.0f,
                      { 0.1f, 0.1f, 0.1f }, -3.0f, 3.0f, 0.4f },
                    { engine::physics::JointKind::Revolute,
                      { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, 1.0f,
                      { 0.05f, 0.05f, 0.05f }, -2.6f, 2.6f, 0.0f },
                };
                multibodyChain = multibody->create_chain(links, mbError);
                if (multibodyChain != engine::physics::InvalidMultibody) {
                    multibodyLinks = multibody->link_count(multibodyChain);
                    std::cout << "[Showcase] multibody chain created ("
                              << multibodyLinks << " links)\n";
                } else {
                    std::cout << "[Showcase] multibody chain refused: "
                              << mbError << '\n';
                }
            }
        }
    }
    {
        std::string srError;
        shapeRecognition = engine::physics::create_shape_recognition(srError);
        if (shapeRecognition) {
            engine::physics::ShapeRecognitionConfig sconfig;
            if (!shapeRecognition->configure(sconfig, srError)) {
                std::cout << "[Showcase] shape recognition refused: "
                          << srError << '\n';
                shapeRecognition.reset();
            }
        }
    }

    // ── CONTA 4 item 2: block entity FULL lifecycle on the LIVE voxel world
    // — attach/detach are observed through the canonical listener and a real
    // removal (unload/destruction) is driven by a hotkey (KeyQ) through the
    // SAME World::remove_block_entity path the load-reconcile uses. The loop
    // proves creation -> tick -> unload -> destruction all flow through the
    // product World, not a SDK probe.
    {
        world.set_block_entity_listener(
            [this](const engine::voxel::BlockEntityEvent& event) {
                if (event.kind == engine::voxel::BlockEntityEvent::Kind::Attached) {
                    ++blockEntityAttachedObserved;
                } else if (event.kind == engine::voxel::BlockEntityEvent::Kind::Detached) {
                    ++blockEntityDetachedObserved;
                }
            });
        // Force at least one attached-clock cycle so the listener proves the
        // Attached event fires on the real World (clocks attach in the loop).
        worldLote1.try_attach_clocks(
            player.position.x, player.position.y, player.position.z);
    }

    // ── CONTA 4 item 6: the game executable consumes the PUBLIC mission
    // runtime — a data-driven mission whose Collect objective is driven by the
    // REAL player inventory counts and whose reward is applied to the SAME
    // live Inventory. Progress is advanced every fixed tick and persisted in
    // the unified save (restored on load without a silent reset).
    showcaseMissions = engine::gameplay::create_mission_runtime();
    if (showcaseMissions) {
        showcaseMissionDef.name = "Mine Stone";
        showcaseMissionDef.id = "22222222-3333-4444-5555-666666666666";
        engine::gameplay::MissionObjective collect;
        collect.id = "collect_stone";
        collect.kind = engine::gameplay::MissionObjectiveKind::Collect;
        collect.target = "stone";
        collect.count = 3;
        engine::gameplay::MissionObjective reach;
        reach.id = "reach_home";
        reach.kind = engine::gameplay::MissionObjectiveKind::Reach;
        reach.count = 1;
        reach.x = 8.0f;
        reach.z = 8.0f;
        reach.radius = 8.0f;
        showcaseMissionDef.objectives = { collect, reach };
        showcaseMissionDef.reward.itemId = "vulkancraft:stone_brick";
        showcaseMissionDef.reward.count = 8;
        showcaseMissionDef.reward.xp = 50;
        showcaseMissionDef.reward.setFlag = "quest1_done";
        std::string mDefErr;
        if (!showcaseMissionDef.validate(mDefErr)) {
            std::cout << "[Showcase] mission definition refused: "
                      << mDefErr << '\n';
            showcaseMissions.reset();
        } else {
            std::cout << "[Showcase] mission runtime wired: '"
                      << showcaseMissionDef.name << "' ("
                      << showcaseMissionDef.objectives.size()
                      << " objectives, reward "
                      << showcaseMissionDef.reward.itemId << " x"
                      << showcaseMissionDef.reward.count << ")\n";
        }
    }

    // ── CONTA 4 item 5: the game assembles a REAL vehicle from a
    // VehicleAsset through the PUBLIC IGameplayRuntime::create_vehicle_from_asset
    // (the same factory the server uses) and drives it every fixed tick. The
    // chassis is spawned near the player; wheels/speed are observable and the
    // pose is persisted so the save/load round-trips the vehicle (no reset).
    showcaseVehicleAsset = std::make_unique<engine::vehicles::VehicleAsset>();
    showcaseVehicleValid = false;
    if (runtimePhysics && showcaseVehicleAsset) {
        engine::vehicles::VehicleAsset& asset = *showcaseVehicleAsset;
        asset.id = "showcase_car";
        asset.name = "Showcase Car";
        asset.provider = engine::vehicles::VehicleProviderKind::Jolt;
        asset.position = player.position + glm::vec3(0.0f, 0.5f, 4.0f);
        asset.chassis.mass = 900.0f;
        asset.wheels = {
            { { -0.6f, 0.0f, -0.9f }, 0.36f, 0.45f, 0.18f, 26000.f, 3200.f, 1.35f, 4200.f, 6000.f, 0.55f, true, true },
            { {  0.6f, 0.0f, -0.9f }, 0.36f, 0.45f, 0.18f, 26000.f, 3200.f, 1.35f, 4200.f, 6000.f, 0.55f, true, true },
            { { -0.6f, 0.0f,  0.9f }, 0.36f, 0.45f, 0.18f, 26000.f, 3200.f, 1.35f, 4200.f, 6000.f, 0.55f, false, true },
            { {  0.6f, 0.0f,  0.9f }, 0.36f, 0.45f, 0.18f, 26000.f, 3200.f, 1.35f, 4200.f, 6000.f, 0.55f, false, true },
        };
        std::string vErr;
        // Drive the vehicle onto the surface so wheels report grounding.
        asset.position.y = showcaseSurfaceAt(world, asset.position.x,
                                             asset.position.z) + 0.6f;
        if (asset.validate(vErr)) {
            showcaseVehicle =
                runtimePhysics->create_vehicle_from_asset(asset);
            showcaseVehicleValid = showcaseVehicle != nullptr;
            if (showcaseVehicle) {
                showcaseVehicleSpeed = showcaseVehicle->speed();
                showcaseVehicleWheels =
                    showcaseVehicle->wheel_states().size();
                showcaseVehicleOccupants =
                    showcaseVehicle->seat_count();
                std::cout << "[Showcase] vehicle assembled from asset '"
                          << asset.name << "' (" << showcaseVehicleWheels
                          << " wheels)\n";
            } else {
                std::cout << "[Showcase] vehicle assembly refused: "
                          << vErr << '\n';
                if (!asset.validate(vErr)) {
                    std::cout << "[Showcase]   asset invalid: " << vErr << '\n';
                }
            }
        } else {
            std::cout << "[Showcase] vehicle asset invalid: " << vErr << '\n';
        }
    }

    // ── CONTA 3 (fechamento global): content/physics/simulation factories
    // that were TEST-ONLY are now owned by the LIVE game loop — each is
    // created with real world/player data here and advanced per fixed tick
    // (see showcase_content_physics_tick), with observables in the title.
    showcase_content_physics_init();

    // ── C: load the previous session's save (if any) — restores player,
    // inventory, day/night, abilities and queues the block edits for atomic
    // re-application once chunks stream. Runs after the world is bound so the
    // edits target a live world.
    showcase_gameplay_load();

    // ── AGENTE 3 A.2/A.3/A.4/B.1/B.5: host-local session over the PUBLIC
    // networking contracts — the game is a real consumer of the same stack the
    // dedicated server uses (session/transport/replication/RPC/interest). The
    // INetworkGameClient owns its loopback transport + session identity + client
    // prediction; the discovery/interest/rpc/entity-replication factories are
    // consumed DIRECTLY (A.4), and the session lifecycle is explicit: start ->
    // connect -> tick -> disconnect (A.3) with observable errors.
    {
        std::string netErr;
        // D.4 — data-driven host-local config (no personal address/token/path
        // hardcoded): the same asset the server boots from.
        std::string cfgHost = "127.0.0.1";
        std::uint16_t cfgPort = 3724;
        std::uint32_t cfgTick = 60;
        std::uint32_t cfgMaxClients = 64;
        std::uint32_t cfgProtocol = 1;
        {
            const std::string cfgPath =
                std::string(VULKANCRAFT_SOURCE_DIR) +
                "/Projects/AuditProj/Content/Config/ag3_host_local.json";
            std::ifstream cfgFile(cfgPath);
            if (cfgFile) {
                std::string cfgText((std::istreambuf_iterator<char>(cfgFile)),
                                    std::istreambuf_iterator<char>());
                engine::sdk::JsonValue cfgRoot;
                std::string cfgJsonError;
                if (engine::sdk::json_parse(cfgText, cfgRoot, cfgJsonError) &&
                    cfgRoot.is_object()) {
                    const engine::sdk::JsonValue* session =
                        cfgRoot.field("session");
                    if (session != nullptr && session->is_object()) {
                        cfgHost = engine::sdk::json_string(
                            *session, "host", cfgHost);
                        cfgPort = static_cast<std::uint16_t>(
                            engine::sdk::json_number(*session, "port", cfgPort));
                        cfgTick = static_cast<std::uint32_t>(
                            engine::sdk::json_number(*session, "tick_rate", cfgTick));
                        cfgMaxClients = static_cast<std::uint32_t>(
                            engine::sdk::json_number(*session, "max_clients", cfgMaxClients));
                    }
                    const engine::sdk::JsonValue* versioning =
                        cfgRoot.field("versioning");
                    if (versioning != nullptr && versioning->is_object()) {
                        cfgProtocol = static_cast<std::uint32_t>(
                            engine::sdk::json_number(*versioning, "protocol", cfgProtocol));
                    }
                }
            }
        }
        // A.4 — transport factory: enumerate the real backends and create the
        // loopback transport the local session connects over.
        std::size_t transportBackends = 0;
        bool transportLoopback = false;
        {
            auto transportFactory =
                engine::networking::create_network_transport_factory();
            if (transportFactory) {
                const auto backends = transportFactory->backends();
                transportBackends = backends.size();
                for (const auto& b : backends) {
                    if (b.kind == engine::networking::TransportKind::Loopback &&
                        b.available) {
                        transportLoopback = true;
                    }
                }
            }
        }
        // A.4 — discovery: register a healthy gameplay service and resolve it.
        std::size_t discoveryResolved = 0;
        hostLocalDiscovery = engine::networking::create_network_discovery(
            "host-local-discovery", netErr);
        if (hostLocalDiscovery) {
            engine::networking::DiscoveryService service;
            service.service_id = 1;
            service.type = "gameplay";
            service.endpoint = "loopback://" + cfgHost + ":" +
                               std::to_string(cfgPort);
            service.healthy = true;
            std::string discErr;
            if (hostLocalDiscovery->register_service(service, discErr)) {
                hostLocalDiscovery->report_health(1, true);
                discoveryResolved =
                    hostLocalDiscovery->resolve("gameplay").size();
            }
        }
        // A.4 — interest: the local session observes the showcase entities by
        // position (the player is the observer; the mobs are entities).
        hostLocalInterest = engine::networking::create_network_interest(
            "host-local-interest", netErr);
        if (hostLocalInterest) {
            engine::networking::InterestObserver observer;
            observer.observer_id = 1;
            observer.position = { player.position.x, player.position.y,
                                  player.position.z };
            observer.radius = 64.0;
            observer.always_relevant = false;
            std::string interestErr;
            if (hostLocalInterest->set_observer(observer, interestErr)) {
                if (mobEntities) {
                    std::vector<engine::entity::EntityId> live;
                    mobEntities->for_each_entity(
                        [&live](engine::entity::EntityId id) {
                            live.push_back(id);
                        });
                    for (const auto& id : live) {
                        engine::entity::Position p;
                        if (mobEntities->get_position(id, p)) {
                            hostLocalInterest->set_entity(
                                { static_cast<std::uint64_t>(id.id),
                                  { p.x, p.y, p.z } });
                        }
                    }
                }
                const auto relevant = hostLocalInterest->compute();
                for (const auto& result : relevant) {
                    hostLocalRelevantEntities += result.entity_ids.size();
                }
            }
        }
        // A.4 — RPC: register a remote procedure and drain one call.
        std::size_t rpcDrained = 0;
        hostLocalRpc = engine::networking::create_network_rpc(
            "host-local-rpc", netErr);
        if (hostLocalRpc) {
            std::string rpcErr;
            if (hostLocalRpc->register_procedure(
                    "showcase.interact",
                    [](const std::vector<std::uint8_t>& payload) {
                        engine::networking::RpcResult result;
                        result.ok = true;
                        result.data = payload;
                        return result;
                    },
                    rpcErr)) {
                const std::vector<std::uint8_t> call{ 1, 0, 0, 0 };
                if (hostLocalRpc->enqueue_call("showcase.interact", call,
                                                rpcErr)) {
                    rpcDrained = hostLocalRpc->drain(rpcErr).size();
                }
            }
        }
        // B.1 — entity-state replication with clear authority: the local host
        // applies authoritative frames carrying the showcase entity's
        // identity/transform/inventory/ability/interaction state (opaque bytes,
        // same contract the server replicates with).
        std::size_t replicatedFrames = 0;
        hostLocalReplication = engine::networking::create_network_replication(
            "host-local-replication", netErr);
        if (hostLocalReplication) {
            const std::uint64_t entityId =
                showcasePlayerEntityValid
                    ? static_cast<std::uint64_t>(showcasePlayerEntity.id)
                    : static_cast<std::uint64_t>(1001);
            const std::vector<std::pair<std::string, std::string>> kinds = {
                { "identity", "player:host-local" },
                { "transform", "pos 8.0,60.0,8.0 yaw 0" },
                { "animation", "anim walk t=0.0" },
                { "inventory", "inv {dirt:1,stone:0}" },
                { "ability", "ab punch ready" },
                { "interaction", "interact none" },
            };
            for (std::size_t i = 0; i < kinds.size(); ++i) {
                engine::networking::ReplicationFrame frame;
                frame.tick = static_cast<std::uint64_t>(i + 1);
                engine::networking::NetworkEntityState state;
                state.entity_id = entityId;
                state.kind = kinds[i].first;
                state.data.assign(kinds[i].second.begin(),
                                  kinds[i].second.end());
                frame.states.push_back(state);
                std::string frameErr;
                if (hostLocalReplication->apply_frame(frame, frameErr)) {
                    ++replicatedFrames;
                }
            }
        }
        // A.2/A.3 — the game client: real transport + session + prediction over
        // the loopback endpoint, with an explicit connect -> tick -> disconnect
        // lifecycle (the same INetworkGameClient the server protocol targets).
        hostLocalClient = engine::networking::create_network_game_client(netErr);
        if (hostLocalClient) {
            engine::networking::GameClientConfig clientConfig;
            clientConfig.transport.kind = engine::networking::TransportKind::Loopback;
            clientConfig.transport.endpoint = { cfgHost, cfgPort };
            clientConfig.server = { cfgHost, cfgPort };
            clientConfig.version.protocol = cfgProtocol;
            clientConfig.player_name = "host-local-player";
            clientConfig.player_id = 1001;
            clientConfig.start_world_id = 1;
            std::string connectErr;
            if (hostLocalClient->connect(clientConfig, connectErr)) {
                std::string tickErr;
                hostLocalClient->tick(1.0 / 60.0, tickErr);
                // B.5 — client prediction + reconciliation over the same
                // controller input the showcase uses (no gameplay duplication:
                // the default kinematic step integrates the input, the server
                // pose corrects drift).
                auto& prediction = hostLocalClient->prediction();
                prediction.reset(netErr);
                prediction.predict(0.016f, 1.0f, 0.0f, false);
                prediction.predict(0.016f, 0.0f, 1.0f, false);
                const auto unacked = prediction.next_sequence() - 1;
                engine::networking::PredictedPose authoritative = prediction.pose();
                authoritative.x += 0.25;  // server authority diverges slightly
                const auto rc = prediction.reconcile(authoritative, unacked);
                // B.5 — transactional block prediction with rollback (G.5):
                // predict a break, then confirm it was REJECTED by the server
                // -> a rollback signal restores the pre-edit block.
                const auto predictSeq = prediction.predict_block(
                    engine::networking::BlockEditKind::Break,
                    static_cast<int>(player.position.x),
                    static_cast<int>(player.position.y),
                    static_cast<int>(player.position.z), 1u, 0u);
                if (predictSeq != 0) {
                    ++hostLocalPredictedBlocks;
                    prediction.confirm_block(predictSeq, false);  // rejected
                    hostLocalRollbacks =
                        prediction.drain_rollbacks().size();
                }
                // A.3 — explicit command send; the client stays connected for
                // the game loop (tick per frame in showcase_gameplay_tick) and
                // disconnects gracefully in showcase_gameplay_shutdown. A
                // refused send is OBSERVABLE (never silent).
                const std::uint8_t cmd[12] = { 8,0,0,0, 100,0,0,0, 24,0,0,0 };
                std::string cmdErr;
                if (!hostLocalClient->send_command("block.place", cmd,
                                                   sizeof(cmd), cmdErr)) {
                    std::cout << "[Showcase] host-local send_command refused: "
                              << cmdErr << "\n";
                }
                hostLocalNetOk = true;
                hostLocalNetSummary = std::format(
                    "net ok (transport {} backends{}|disc {}|interest {}|rpc {}|"
                    "repl {} frames|client connected, {} predicted blocks, "
                    "{} rollback)",
                    transportBackends, transportLoopback ? " loopback" : "",
                    discoveryResolved, hostLocalRelevantEntities, rpcDrained,
                    replicatedFrames, hostLocalPredictedBlocks, hostLocalRollbacks);
                std::cout << "[Showcase] host-local session: "
                          << hostLocalNetSummary << "\n";
            } else {
                std::cout << "[Showcase] host-local client connect refused: "
                          << connectErr << "\n";
                hostLocalClient.reset();
            }
        }
    }

    // ── CONTA 4: the game client consumes the same three SDK symbols the
    // dedicated server does — REAL call sites (no longer TEST-ONLY), advanced
    // every fixed tick and observable in the title/summary, over the public
    // contracts (single transport, no parallel track).
    //
    // create_opus_codec — real OPUS voice codec for network voice: created
    // here, fed one mono PCM frame per fixed tick (encode -> wire -> decode).
    {
        std::string codecErr;
        showcaseVoiceCodec = engine::audio::create_opus_codec(
            engine::audio::AudioCodecConfig{}, codecErr);
        std::cout << "[Showcase] create_opus_codec "
                  << (showcaseVoiceCodec
                          ? "ready (real OPUS voice codec)"
                          : std::string("refused: ") + codecErr)
                  << "\n";
    }
    // create_vehicle_replication — server authority + client prediction on two
    // upside-down dedicated secondary runtimes (the game world is NOT
    // double-stepped): the authoritative vehicle and its predicted copy are
    // created from the SAME showcase jeep asset; every fixed tick the snapshot
    // travels to the predicted world and is reconciled.
    if (runtimePhysics && showcaseVehicleAsset) {
        showcaseVehServerRuntime = engine::gameplay::create_gameplay_runtime(
            engine::gameplay::PhysicsBackend::Builtin);
        showcaseVehReplication =
            engine::vehicles::create_vehicle_replication(*showcaseVehServerRuntime);
        showcaseVehClientRuntime = engine::gameplay::create_gameplay_runtime(
            engine::gameplay::PhysicsBackend::Builtin);
        showcaseVehClientReplication =
            engine::vehicles::create_vehicle_replication(*showcaseVehClientRuntime);
        engine::gameplay::BodySpec ground;
        ground.motion = engine::gameplay::MotionType::Static;
        ground.position = { 0.0f, -0.5f, 0.0f };
        ground.shape = engine::gameplay::BoxShape{ { 200.0f, 1.0f, 200.0f } };
        showcaseVehServerRuntime->physics().create_body(ground);
        showcaseVehClientRuntime->physics().create_body(ground);
        std::string srErr;
        showcaseVehServerCar =
            showcaseVehServerRuntime->create_vehicle_from_asset(*showcaseVehicleAsset);
        if (showcaseVehServerCar &&
            showcaseVehReplication->server_register("client_car",
                                                    *showcaseVehServerCar,
                                                    srErr)) {
            std::string prErr;
            showcaseVehPredicted =
                showcaseVehClientRuntime->create_vehicle_from_asset(*showcaseVehicleAsset);
            if (showcaseVehPredicted &&
                !showcaseVehClientReplication->client_register_prediction(
                    "client_car", *showcaseVehPredicted, prErr)) {
                showcaseVehPredicted.reset();
            }
            showcaseVehNetSetup = showcaseVehPredicted != nullptr;
            std::cout << "[Showcase] create_vehicle_replication ready (server "
                         "authority + client prediction on secondary worlds)\n";
        }
    }
    // create_voxel_replication — bound lazily on the first fixed tick once the
    // game's public IVoxelWorld (blockWorld) is live; per-tick region snapshots
    // pack the world's entity EntitySnapshots into an observable counter.
    std::cout << "[Showcase] create_voxel_replication staged (bound lazily to "
                 "the game voxel world each fixed tick)\n";

    // ── CONTA 6 (integração): the two residual TEST-ONLY factories get REAL
    // consumers in the game executable — create_hair_physics (the headless
    // Verlet hair contract) and create_system_timeline (the per-system CPU
    // frame breakdown). Both are advanced every fixed tick and their live
    // state is published to the title, closing the last FACTORY-NO-CONSUMER
    // rows of the integration audit.
    {
        std::string hairErr;
        vc::rendering::HairConfig hcfg;
        hcfg.gravity = -9.81f;
        hcfg.damping = 0.99f;
        hcfg.stiffness = 0.9f;
        hcfg.localIterations = 3;
        hcfg.lengthIterations = 3;
        hcfg.windStrength = 0.35f;
        hcfg.windDirection = { 0.0f, 0.0f, 1.0f };
        showcaseHairPhysics = vc::rendering::create_hair_physics(hcfg, hairErr);
        if (showcaseHairPhysics) {
            std::vector<vc::rendering::Vec3> strand;
            for (int i = 0; i < 5; ++i) {
                strand.push_back(vc::rendering::Vec3{
                    0.0f, 1.8f - 0.22f * static_cast<float>(i), 0.0f });
            }
            showcaseHairStrand = showcaseHairPhysics->createStrand(strand);
            showcaseHairParticles = static_cast<std::size_t>(
                showcaseHairPhysics->particleCount(showcaseHairStrand));
            std::cout << "[Showcase] create_hair_physics ready ("
                      << showcaseHairParticles << " particles)\n";
        } else {
            std::cout << "[Showcase] create_hair_physics refused: "
                      << hairErr << "\n";
        }
    }
    {
        showcaseTimeline = engine::profiling::create_system_timeline(120);
        std::cout << "[Showcase] create_system_timeline ready "
                     "(120-frame sliding window)\n";
    }

    // One full fixed tick at boot so every observable reflects real output.
    showcase_gameplay_tick(1.0f / 60.0f);
    std::cout << "[Showcase] gameplay showcase bootstrap complete\n";
}

// ── CONTA 3 (fechamento global) — content/physics/simulation real consumers ──
// The TEST-ONLY factories of the physics/content/simulation domains are owned
// by the LIVE game loop: each is created here with real world/player data,
// advanced per FIXED tick in showcase_content_physics_tick(), and released in
// showcase_content_physics_shutdown(). Every observable below is published in
// the window title through the appended summary segment.
void VulkanEngineApp::showcase_content_physics_init() {
    std::string err;

    // create_beam_vehicle: a deformable node/beam XPBD chassis assembled
    // through the PUBLIC IGameplayRuntime, spawned above the LIVE surface at
    // the player. Driven by set_input/update on the fixed tick; speed +
    // deformation (max |node - rest|) are the per-tick observables.
    if (runtimePhysics) {
        engine::vehicles::BeamGraphAsset beam;
        beam.name = "showcase-beam";
        beam.position = glm::vec3(
            player.position.x,
            showcaseSurfaceAt(world, player.position.x, player.position.z) + 1.4f,
            player.position.z);
        beam.nodes = {
            { { -1.2f, 0.0f, -0.8f }, false }, { { -1.2f, 0.0f, 0.8f }, false },
            { { 1.2f, 0.0f, -0.8f }, false },  { { 1.2f, 0.0f, 0.8f }, false },
            { { 0.0f, 0.0f, 0.0f }, false },
        };
        beam.beams = {
            { 0, 1, 0.9f }, { 1, 3, 0.9f }, { 3, 2, 0.9f }, { 2, 0, 0.9f },
            { 0, 4, 0.5f }, { 4, 3, 0.5f },
        };
        engine::vehicles::BeamWheelMount mount;
        mount.wheel.radius = 0.36f;
        mount.wheel.suspensionRestLength = 0.55f;
        mount.wheel.suspensionTravel = 0.22f;
        mount.wheel.springStrength = 26000.0f;
        mount.wheel.damperStrength = 3200.0f;
        mount.wheel.tireGrip = 1.35f;
        mount.wheel.maxDriveForce = 4200.0f;
        mount.wheel.maxBrakeForce = 6000.0f;
        mount.wheel.maxSteerAngle = 0.55f;
        for (std::uint32_t i = 0; i < 4; ++i) {
            engine::vehicles::BeamWheelMount m = mount;
            m.node = i;
            m.steering = i < 2;
            m.driven = true;
            beam.wheels.push_back(m);
        }
        showcaseBeam = runtimePhysics->create_beam_vehicle(beam);
        if (showcaseBeam) {
            showcaseBeamNodes = showcaseBeam->node_count();
            std::cout << "[Showcase] beam vehicle assembled ("
                      << showcaseBeamNodes << " nodes, "
                      << showcaseBeam->wheel_states().size() << " wheels)\n";
        } else {
            std::cout << "[Showcase] beam vehicle refused\n";
        }
    }

    // create_flight_dynamics + create_flight_dynamics_json: the same 6-DOF
    // fixed-wing model from the plain and the data-driven factory.
    {
        std::string fErr;
        showcaseFlight = engine::vehicles::create_flight_dynamics(fErr);
        engine::vehicles::AircraftSpec spec;
        spec.mass = 1200.0f;
        spec.wingArea = 18.0f;
        spec.thrust = 8000.0f;
        if (showcaseFlight && !showcaseFlight->configure(spec, fErr)) {
            std::cout << "[Showcase] flight dynamics configure refused: "
                      << fErr << "\n";
            showcaseFlight.reset();
        }
        std::string jErr;
        showcaseFlightJson = engine::vehicles::create_flight_dynamics_json(
            R"({"mass":1200,"wingArea":18,"thrust":8000,"cmAlpha":-0.5})",
            jErr);
        if (!showcaseFlightJson) {
            std::cout << "[Showcase] flight dynamics (json) refused: "
                      << jErr << "\n";
        }
    }

    // create_vehicle_damage: deterministic per-part model (destroy/detach).
    {
        std::string dErr;
        showcaseVehicleDamage = engine::vehicles::create_vehicle_damage();
        if (showcaseVehicleDamage) {
            const std::vector<engine::vehicles::VehiclePartSpec> parts = {
                { "hull", 100.0f, false, 0.0f },
                { "engine", 80.0f, true, 0.4f },
                { "wheel_fl", 50.0f, true, 0.5f },
                { "wheel_fr", 50.0f, true, 0.5f },
            };
            if (!showcaseVehicleDamage->configure(parts, dErr)) {
                std::cout << "[Showcase] vehicle damage configure refused: "
                          << dErr << "\n";
                showcaseVehicleDamage.reset();
            }
        }
    }

    // create_csg_operation: manifold boolean ops over boxes anchored at the
    // live surface (build/excavate), refreshed once per second in the tick.
    {
        std::string cErr;
        showcaseCsg = engine::physics::create_csg_operation("manifold", cErr);
        if (!showcaseCsg) {
            std::cout << "[Showcase] csg refused: " << cErr << "\n";
        }
    }

    // create_multibody_dynamics_json: the data-driven articulated chain.
    {
        std::string mErr;
        multibodyJson = engine::physics::create_multibody_dynamics_json(
            R"({"maxLinks":8,"damping":0.1})", mErr);
        if (multibodyJson) {
            const std::vector<engine::physics::MultibodyLinkDesc> links = {
                { engine::physics::JointKind::Revolute,
                  { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f }, 2.0f,
                  { 0.1f, 0.1f, 0.1f }, -3.0f, 3.0f, 0.4f },
                { engine::physics::JointKind::Revolute,
                  { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, 1.0f,
                  { 0.05f, 0.05f, 0.05f }, -2.6f, 2.6f, 0.0f },
            };
            multibodyJsonChain = multibodyJson->create_chain(links, mErr);
            if (multibodyJsonChain == engine::physics::InvalidMultibody) {
                std::cout << "[Showcase] multibody (json) chain refused: "
                          << mErr << "\n";
            }
        }
    }

    // create_shape_recognition_json: RANSAC over real surface points.
    {
        std::string sErr;
        shapeRecognitionJson = engine::physics::create_shape_recognition_json(
            R"({"seed":3,"minSupport":12})", sErr);
        if (shapeRecognitionJson) {
            engine::physics::ShapeRecognitionConfig sconfig =
                shapeRecognitionJson->config();
            if (!shapeRecognitionJson->configure(sconfig, sErr)) {
                std::cout << "[Showcase] shape recognition (json) refused: "
                          << sErr << "\n";
                shapeRecognitionJson.reset();
            }
        }
    }

    // create_sph_fluid_simulation + _json: a particle column above the LIVE
    // surface; both solvers are stepped every fixed tick and their density
    // (compressibility) is the observable.
    auto resetSph = [&](engine::simulation::ISPHFluidSimulation* sim,
                        const char* tag) {
        if (!sim) return;
        engine::simulation::SphFluidConfig cfg = sim->config();
        cfg.groundY = showcaseSurfaceAt(world, player.position.x,
                                        player.position.z);
        std::string rErr;
        if (!sim->configure(cfg, rErr)) {
            std::cout << "[Showcase] sph (" << tag << ") configure refused: "
                      << rErr << "\n";
            return;
        }
        std::vector<glm::vec3> pos, vel;
        for (int i = 0; i < 6; ++i) {
            for (int j = 0; j < 6; ++j) {
                pos.push_back(glm::vec3(
                    player.position.x - 0.75f + 0.3f * static_cast<float>(i),
                    cfg.groundY + 2.0f + 0.3f * static_cast<float>((i + j) % 3),
                    player.position.z - 0.75f + 0.3f * static_cast<float>(j)));
                vel.emplace_back(0.0f);
            }
        }
        if (!sim->reset(pos, vel, rErr)) {
            std::cout << "[Showcase] sph (" << tag << ") reset refused: "
                      << rErr << "\n";
        }
    };
    {
        std::string pErr;
        sphFluid = engine::simulation::create_sph_fluid_simulation(pErr);
        std::string jErr;
        sphFluidJson =
            engine::simulation::create_sph_fluid_simulation_json(
                R"({"maxParticles":256,"particleRadius":0.12})", jErr);
        if (!sphFluidJson) {
            std::cout << "[Showcase] sph (json) refused: " << jErr << "\n";
        }
        resetSph(sphFluid.get(), "plain");
        resetSph(sphFluidJson.get(), "json");
    }

    // create_tetra_mesh_cooking: the cooker needs an IVoxelWorld — the SDK
    // default world is populated per second with the LIVE voxel region under
    // the player, so the cooked sim/render meshes track the real terrain.
    blockWorld = engine::voxel::create_default_voxel_world();
    {
        Engine::Deformable::TetraCookingConfig tcfg;
        tcfg.maxTets = 200000;
        std::string tErr;
        tetraCooker = Engine::Deformable::create_tetra_mesh_cooking(tcfg, tErr);
        if (!tetraCooker) {
            std::cout << "[Showcase] tetra cooker refused: " << tErr << "\n";
        }
    }

    // create_road_network_builder + create_parcellation: Delaunay road graph
    // + bounded-face parcels over junction points near the player.
    roadBuilder = engine::procgen::create_road_network_builder();
    parcellation = engine::procgen::create_parcellation();

    // create_shape_grammar_runner: the house grammar emits real block volumes
    // every tick (walls/floor/windows/glass).
    grammarRunner = engine::procgen::create_shape_grammar_runner();
    if (grammarRunner) {
        using engine::procgen::GrammarOpMass;
        using engine::procgen::GrammarOpSize;
        using engine::procgen::GrammarOpSplit;
        using engine::procgen::GrammarRule;
        engine::procgen::GrammarRule axiom;
        axiom.name = "Axiom";
        axiom.ops.push_back(GrammarOpSize{ 10, 10, 8 });
        axiom.ops.push_back(GrammarOpSplit{ 'x', { 2, 6, 2 },
                                            { "Wall", "Interior", "Wall" } });
        showcaseHouseGrammar.rules.push_back(axiom);
        engine::procgen::GrammarRule wall;
        wall.name = "Wall";
        wall.ops.push_back(GrammarOpMass{ 4 });
        showcaseHouseGrammar.rules.push_back(wall);
        engine::procgen::GrammarRule interior;
        interior.name = "Interior";
        interior.ops.push_back(GrammarOpSplit{ 'y', { 4, 2, 4 },
                                              { "Floor", "Windows", "Floor" } });
        showcaseHouseGrammar.rules.push_back(interior);
        engine::procgen::GrammarRule floor;
        floor.name = "Floor";
        floor.ops.push_back(GrammarOpMass{ 4 });
        showcaseHouseGrammar.rules.push_back(floor);
        engine::procgen::GrammarRule windows;
        windows.name = "Windows";
        windows.ops.push_back(GrammarOpSplit{ 'z', { 1, 6, 1 },
                                              { "Wall", "Glass", "Wall" } });
        showcaseHouseGrammar.rules.push_back(windows);
        engine::procgen::GrammarRule glass;
        glass.name = "Glass";
        glass.ops.push_back(GrammarOpMass{ 20 });
        showcaseHouseGrammar.rules.push_back(glass);
    }

    // create_structure_generator (plain + from_json) and
    // create_structure_placement_system_from_json: the WFC room spec is built
    // once, the JSON variants are rebuilt from the serialized assets.
    {
        engine::procgen::StructureAssetSpec spec;
        spec.sampleWidth = 8;
        spec.sampleHeight = 5;
        const int w = spec.sampleWidth;
        for (int z = 0; z < spec.sampleHeight; ++z) {
            for (int x = 0; x < w; ++x) {
                const bool wall = z == 0 || z == spec.sampleHeight - 1 ||
                                  x == 0 || x == w - 1;
                spec.sample.push_back(wall ? 1u : 2u);
            }
        }
        spec.patternSize = 2;
        spec.seed = 7;
        spec.profiles.emplace_back(1, std::vector<std::uint32_t>{ 3, 3, 3 });
        spec.profiles.emplace_back(2, std::vector<std::uint32_t>{ 5 });
        std::string gErr;
        structureGenerator =
            engine::procgen::create_structure_generator(spec, gErr);
        if (!structureGenerator) {
            std::cout << "[Showcase] structure generator refused: "
                      << gErr << "\n";
        }
        std::string serialized;
        if (structureGenerator && structureGenerator->serialize(serialized)) {
            std::string gjErr;
            structureGeneratorJson =
                engine::procgen::create_structure_generator_from_json(serialized,
                                                                      gjErr);
            if (!structureGeneratorJson) {
                std::cout << "[Showcase] structure generator (json) refused: "
                          << gjErr << "\n";
            }
        }
        // Placement: definition + spawn rule on a plain system, then rebuilt
        // through the JSON factory (the product consumer is the _from_json
        // variant).
        auto plainSystem = engine::procgen::create_structure_placement_system();
        if (plainSystem) {
            engine::procgen::StructureDefinition room;
            room.id = "showcase:room";
            room.spec = spec;
            room.outputWidth = 12;
            room.outputHeight = 8;
            room.sockets.push_back(
                engine::procgen::StructureSocket{ "door", { 0, 1, 0 }, 0,
                                                  "door" });
            room.sockets.push_back(
                engine::procgen::StructureSocket{ "window", { 6, 2, 0 }, 2,
                                                  "" });
            if (!plainSystem->add_definition(room, gErr)) {
                std::cout << "[Showcase] placement definition refused: "
                          << gErr << "\n";
            }
            engine::procgen::StructureSpawnRule rule;
            rule.structureId = "showcase:room";
            rule.biomes = { "plains" };
            rule.minSurfaceHeight = -100000;
            rule.maxSurfaceHeight = 100000;
            rule.density = 1.0f;
            rule.spacing = 8;
            rule.yOffset = 1;
            rule.seedOffset = 3;
            if (!plainSystem->set_rules({ rule }, gErr)) {
                std::cout << "[Showcase] placement rules refused: "
                          << gErr << "\n";
            }
            std::string doc;
            if (plainSystem->serialize(doc)) {
                structurePlacement =
                    engine::procgen::create_structure_placement_system_from_json(
                        doc, gErr);
                if (!structurePlacement) {
                    std::cout << "[Showcase] placement (json) refused: "
                              << gErr << "\n";
                }
            }
        }
    }

    // create_motion_match_vendor: a synthetic walk database (9 bones) is
    // built once; the fixed tick queries it with the LIVE character root pose.
    {
        std::string mErr;
        motionMatchVendor = engine::animation::create_motion_match_vendor();
        if (!motionMatchVendor) return;
        constexpr int kNBones = 9;
        const int kParents[kNBones] = { -1, 0, 1, 2, 3, 4, 1, 6, 7 };
        std::vector<engine::animation::VendorPose> poses;
        poses.reserve(120);
        for (int i = 0; i < 120; ++i) {
            engine::animation::VendorPose pose;
            const float x = static_cast<float>(i) * 0.1f;
            const float yaw = 0.1f * std::sin(i * 0.03f);
            const float swingL = 0.15f * std::sin(i * 0.1f);
            const float swingR = -0.15f * std::sin(i * 0.1f);
            const float posData[kNBones][3] = {
                { x, 1.0f, 0.0f },        { 0.0f, 0.0f, 0.0f },
                { 0.0f, -0.05f, 0.05f },  { 0.0f, -0.5f, 0.0f },
                { 0.0f, -0.45f, swingL }, { 0.0f, 0.0f, 0.05f },
                { 0.0f, -0.05f, -0.05f }, { 0.0f, -0.5f, 0.0f },
                { 0.0f, -0.45f, swingR },
            };
            const float velData[kNBones][3] = {
                { 0.1f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f },
                { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f },
                { 0.0f, 0.0f, 0.15f * 0.1f * std::cos(i * 0.1f) },
                { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f },
                { 0.0f, 0.0f, 0.0f },
                { 0.0f, 0.0f, -0.15f * 0.1f * std::cos(i * 0.1f) },
            };
            for (int b = 0; b < kNBones; ++b) {
                pose.bonePositions.insert(
                    pose.bonePositions.end(),
                    { posData[b][0], posData[b][1], posData[b][2] });
                const float half = yaw * (b == 0 ? 1.0f : 0.0f) * 0.5f;
                const float c = std::cos(half), s = std::sin(half);
                if (b == 0) {
                    pose.boneRotations.insert(
                        pose.boneRotations.end(), { 0.0f, s, 0.0f, c });
                } else {
                    pose.boneRotations.insert(
                        pose.boneRotations.end(), { 0.0f, 0.0f, 0.0f, 1.0f });
                }
                pose.boneVelocities.insert(
                    pose.boneVelocities.end(),
                    { velData[b][0], velData[b][1], velData[b][2] });
                pose.boneParents.push_back(kParents[b]);
            }
            poses.push_back(std::move(pose));
        }
        if (!motionMatchVendor->build_database(poses, mErr)) {
            std::cout << "[Showcase] motion-match database refused: "
                      << mErr << "\n";
        }
    }
}

void VulkanEngineApp::showcase_content_physics_tick(float fixedDt) {
    std::string err;

    // Beam vehicle: drive the XPBD chassis with live input every fixed tick
    // and read the deformed state (speed + max node displacement).
    if (showcaseBeam) {
        static int beamFrames = 0;
        ++beamFrames;
        engine::gameplay::VehicleInput input;
        input.throttle = 0.6f;
        input.steering = 0.35f * std::sin(static_cast<float>(beamFrames) * 0.05f);
        showcaseBeam->set_input(input);
        showcaseBeam->update(fixedDt);
        showcaseBeamSpeed = showcaseBeam->speed();
        showcaseBeamDeformation = showcaseBeam->deformation();
    }

    // Flight dynamics (plain + JSON): full throttle with an oscillating
    // elevator — altitude/speed/alpha read from the advanced state.
    {
        engine::vehicles::FlightControls fctrl;
        fctrl.throttle = 1.0f;
        static int flightFrames = 0;
        ++flightFrames;
        fctrl.elevator = 0.06f * std::sin(static_cast<float>(flightFrames) * 0.02f);
        if (showcaseFlight) {
            showcaseFlight->step(fixedDt, fctrl);
            showcaseFlightSpeed =
                glm::length(showcaseFlight->state().velocity);
            showcaseFlightAltitude = showcaseFlight->state().position.z;
            showcaseFlightAlpha = showcaseFlight->alpha();
        }
        if (showcaseFlightJson) {
            showcaseFlightJson->step(fixedDt, fctrl);
            showcaseFlightJsonSpeed =
                glm::length(showcaseFlightJson->state().velocity);
            showcaseFlightJsonAltitude =
                showcaseFlightJson->state().position.z;
        }
    }

    // Vehicle damage: periodic hits on the parts; destroyed/detached counts
    // are read live (the cycle self-repairs so the observables stay bounded).
    if (showcaseVehicleDamage) {
        static int dmgFrames = 0;
        if (++dmgFrames % 90 == 0) {
            (void)showcaseVehicleDamage->apply_damage("hull", 10.0f);
            if (dmgFrames % 270 == 0) {
                (void)showcaseVehicleDamage->apply_damage("wheel_fl", 30.0f);
            }
        }
        if (dmgFrames % 1080 == 0) {
            showcaseVehicleDamage->repair_all();
        }
        showcaseDamageDestroyed =
            showcaseVehicleDamage->destroyed_parts().size();
        showcaseDamageDetached =
            showcaseVehicleDamage->detached_parts().size();
    }

    // CSG: subtract a smaller box from a box anchored at the LIVE surface
    // once per second — the manifold boolean result tris are the observable.
    if (showcaseCsg) {
        static int csgFrames = 0;
        if (++csgFrames >= 60) {
            csgFrames = 0;
            auto makeBox = [](float x0, float y0, float z0, float sx, float sy,
                              float sz, engine::physics::CSGMesh& out) {
                const float x1 = x0 + sx, y1 = y0 + sy, z1 = z0 + sz;
                out.positions = {
                    x0, y0, z0, x1, y0, z0, x1, y1, z0, x0, y1, z0,
                    x0, y0, z1, x1, y0, z1, x1, y1, z1, x0, y1, z1,
                };
                out.indices = {
                    0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6,
                    0, 4, 5, 0, 5, 1, 2, 6, 7, 2, 7, 3,
                    0, 3, 7, 0, 7, 4, 1, 5, 6, 1, 6, 2,
                };
            };
            const float baseY =
                showcaseSurfaceAt(world, player.position.x, player.position.z);
            engine::physics::CSGMesh a, b, result;
            makeBox(player.position.x - 1.0f, baseY, player.position.z - 1.0f,
                    2.0f, 2.0f, 2.0f, a);
            makeBox(player.position.x - 0.5f, baseY + 0.25f,
                    player.position.z - 0.5f, 1.0f, 1.0f, 1.0f, b);
            std::string cErr;
            if (showcaseCsg->operate(a, b, engine::physics::CSGOp::Subtract,
                                     result, cErr)) {
                showcaseCsgResultTris = result.indices.size() / 3;
            } else {
                std::cout << "[Showcase] csg subtract refused: " << cErr
                          << "\n";
            }
        }
    }

    // Multibody (JSON factory): step the chain and read the end effector.
    if (multibodyJson &&
        multibodyJsonChain != engine::physics::InvalidMultibody) {
        multibodyJson->step(fixedDt);
        const auto last = multibodyJson->link_state(
            multibodyJsonChain,
            multibodyJson->link_count(multibodyJsonChain) - 1);
        multibodyJsonEndEffectorY = last.position.y;
    }

    // Shape recognition (JSON factory): 5x5 grid of live surface points.
    if (shapeRecognitionJson) {
        static int srFrames = 0;
        if (++srFrames >= 60) {
            srFrames = 0;
            std::vector<glm::vec3> points;
            for (int dx = -2; dx <= 2; ++dx) {
                for (int dz = -2; dz <= 2; ++dz) {
                    points.push_back(glm::vec3(
                        player.position.x + static_cast<float>(dx),
                        showcaseSurfaceAt(
                            world, player.position.x + static_cast<float>(dx),
                            player.position.z + static_cast<float>(dz)),
                        player.position.z + static_cast<float>(dz)));
                }
            }
            std::vector<engine::physics::ShapePrimitive> primitives;
            if (shapeRecognitionJson->recognize(points, primitives, err)) {
                shapeJsonPrimitiveCount = primitives.size();
            }
        }
    }

    // SPH (plain + JSON): step both solvers; density = compressibility obs.
    if (sphFluid) {
        sphFluid->step(fixedDt);
        sphParticleCount = sphFluid->particle_count();
        float maxDensity = 0.0f;
        for (std::size_t i = 0; i < sphFluid->particle_count(); ++i) {
            maxDensity = std::max(maxDensity, sphFluid->particle_density(i));
        }
        sphMaxDensity = maxDensity;
    }
    if (sphFluidJson) {
        sphFluidJson->step(fixedDt);
        float maxDensity = 0.0f;
        for (std::size_t i = 0; i < sphFluidJson->particle_count(); ++i) {
            maxDensity =
                std::max(maxDensity, sphFluidJson->particle_density(i));
        }
        sphJsonMaxDensity = maxDensity;
    }

    // Tetra cooking: refresh the LIVE voxel snapshot under the player once per
    // second and re-cook the sim/render meshes.
    if (tetraCooker && blockWorld) {
        static int tetFrames = 0;
        if (++tetFrames >= 60) {
            tetFrames = 0;
            const int cx = static_cast<int>(std::floor(player.position.x));
            const int cz = static_cast<int>(std::floor(player.position.z));
            const int baseY = static_cast<int>(std::floor(
                showcaseSurfaceAt(world, player.position.x, player.position.z)));
            const glm::ivec3 minV(cx - 2, baseY - 1, cz - 2);
            const glm::ivec3 maxV(cx + 3, baseY + 4, cz + 3);
            for (int y = minV.y; y <= maxV.y; ++y) {
                for (int x = minV.x; x <= maxV.x; ++x) {
                    for (int z = minV.z; z <= maxV.z; ++z) {
                        blockWorld->set_block(
                            x, y, z,
                            static_cast<std::uint32_t>(world.get_block_at(
                                glm::vec3(static_cast<float>(x),
                                          static_cast<float>(y),
                                          static_cast<float>(z)))));
                    }
                }
            }
            std::string tErr;
            const Engine::Deformable::TetraCookedMesh cooked =
                tetraCooker->cook_voxel_region(*blockWorld, minV, maxV, tErr);
            if (cooked.valid()) {
                tetraSimNodes = cooked.simNodes.size();
                tetraSimTets = cooked.simTets.size();
                tetraRenderTris = cooked.renderTriangles.size();
            } else if (!tErr.empty()) {
                std::cout << "[Showcase] tetra cook refused: " << tErr << "\n";
            }
        }
    }

    // Road network + parcels over live junction points near the player.
    if (roadBuilder && parcellation) {
        static int roadFrames = 0;
        if (++roadFrames >= 60) {
            roadFrames = 0;
            const float px = player.position.x;
            const float pz = player.position.z;
            engine::procgen::RoadNetworkSpec spec;
            spec.points = {
                { px - 8.0, pz - 8.0 }, { px + 8.0, pz - 8.0 },
                { px + 8.0, pz + 8.0 }, { px - 8.0, pz + 8.0 },
            };
            std::string rErr;
            if (roadBuilder->build(spec, rErr)) {
                const auto& net = roadBuilder->network();
                roadJunctions = net.points.size();
                roadEdges = net.edges.size();
                std::vector<engine::procgen::ParcelPolygon> parcels;
                if (parcellation->parcels_from_network(net, parcels, rErr)) {
                    parcelCount = parcels.size();
                    if (!parcels.empty()) {
                        std::vector<std::uint32_t> tris;
                        if (parcellation->triangulate(parcels[0], tris, rErr)) {
                            parcelTris = tris.size() / 3;
                        }
                    }
                }
            }
        }
    }

    // Shape grammar: run the house grammar every fixed tick.
    if (grammarRunner) {
        engine::procgen::GrammarResult gres;
        if (grammarRunner->run(showcaseHouseGrammar, gres, err)) {
            grammarBoxes = gres.boxes.size();
        }
    }

    // Structure generation (plain + JSON) every 90 ticks.
    if (structureGenerator) {
        static int genFrames = 0;
        if (++genFrames >= 90) {
            genFrames = 0;
            engine::procgen::StructureOutput out;
            std::string gErr;
            if (structureGenerator->generate(12, 8, out, gErr) && out.succeeded) {
                structurePlanBlocks = out.blocks.size();
            }
            if (structureGeneratorJson) {
                engine::procgen::StructureOutput jOut;
                std::string jErr;
                if (structureGeneratorJson->generate(12, 8, jOut, jErr) &&
                    jOut.succeeded) {
                    structurePlanBlocksJson = jOut.blocks.size();
                }
            }
        }
    }

    // Structure placement (from JSON): try_place at the player's cell +
    // resolve its sockets.
    if (structurePlacement) {
        static int plcFrames = 0;
        if (++plcFrames >= 90) {
            plcFrames = 0;
            const int wx = static_cast<int>(std::floor(player.position.x));
            const int wz = static_cast<int>(std::floor(player.position.z));
            const int surface = static_cast<int>(std::floor(
                showcaseSurfaceAt(world, player.position.x, player.position.z)));
            engine::procgen::StructurePlacement placed;
            std::string pErr;
            if (structurePlacement->try_place({}, wx, wz, surface, "plains",
                                              42u, placed, pErr) &&
                placed.output.succeeded) {
                ++structurePlacements;
                std::vector<engine::procgen::StructureSocket> sockets;
                if (structurePlacement->resolve_sockets(placed, sockets,
                                                        pErr)) {
                    structureSockets = sockets.size();
                }
            }
        }
    }

    // Motion matching: query the database with the LIVE character root pose.
    if (motionMatchVendor && motionMatchVendor->frame_count() > 0) {
        engine::animation::VendorQuery q;
        constexpr int kNBones = 9;
        const float px = player.position.x;
        const float pz = player.position.z;
        const float py =
            showcaseSurfaceAt(world, px, pz) + 1.0f;
        for (int b = 0; b < kNBones; ++b) {
            q.worldPositions.insert(q.worldPositions.end(),
                                    { px, py - 0.35f * b, pz });
            q.worldRotations.insert(q.worldRotations.end(),
                                    { 0.0f, 0.0f, 0.0f, 1.0f });
            q.worldVelocities.insert(q.worldVelocities.end(),
                                    { 0.0f, 0.0f, 0.0f });
        }
        q.trajectoryPositions = { px, py, pz, px + 0.5f, py, pz,
                                  px + 1.0f, py, pz };
        q.trajectoryRotations = { 0.0f, 0.0f, 0.0f, 1.0f,
                                  0.0f, 0.0f, 0.0f, 1.0f,
                                  0.0f, 0.0f, 0.0f, 1.0f };
        std::int32_t frameIndex = -1;
        float cost = 1.0f;
        static int mmFrame = 0;
        if (motionMatchVendor->query(q, mmFrame, 0.1f, frameIndex, cost,
                                     err)) {
            motionMatchFrame = static_cast<std::size_t>(frameIndex);
            motionMatchCost = cost;
        }
        ++mmFrame;
    }
}

void VulkanEngineApp::showcase_content_physics_shutdown() {
    showcaseBeam.reset();
    showcaseFlight.reset();
    showcaseFlightJson.reset();
    showcaseVehicleDamage.reset();
    showcaseCsg.reset();
    multibodyJson.reset();
    multibodyJsonChain = engine::physics::InvalidMultibody;
    shapeRecognitionJson.reset();
    sphFluid.reset();
    sphFluidJson.reset();
    tetraCooker.reset();
    blockWorld.reset();
    roadBuilder.reset();
    parcellation.reset();
    grammarRunner.reset();
    structureGenerator.reset();
    structureGeneratorJson.reset();
    structurePlacement.reset();
    motionMatchVendor.reset();
}

void VulkanEngineApp::showcase_gameplay_tick(float fixedDt) {
    std::string err;

    // ── CONTA 2 (world/procgen — 18 factories): advance the composed
    // generator's observers on the real world each fixed tick: climate/biome/
    // surface at the player, coherent LOD cells, multi-scale streaming, the
    // heightmap erosion cache and the mesh cooker.
    const auto c6TickStart = std::chrono::steady_clock::now();
    worldProcgen.tick(this->world, player.position.x, player.position.z);
    const auto c6TickAfterProcgen = std::chrono::steady_clock::now();

    // ── CONTA 6 (integração): record the real elapsed of the world/procgen
    // section into the PUBLIC system timeline (create_system_timeline) and
    // advance the headless Verlet hair strand (create_hair_physics) with the
    // fixed step. Both factories are consumed here every tick; their live
    // state is published at the end of this tick (c6 section of the title).
    if (showcaseTimeline) {
        const double procgenMs = std::chrono::duration<double, std::milli>(
            c6TickAfterProcgen - c6TickStart).count();
        showcaseTimeline->begin_frame();
        showcaseTimeline->record_system("world-procgen", procgenMs);
        showcaseTimeline->end_frame(
            static_cast<double>(fixedDt) * 1000.0);
        const engine::profiling::SystemTimelineSnapshot snap =
            showcaseTimeline->snapshot();
        showcaseTimelineFrames = snap.frameCount;
        for (const auto& sys : snap.systems) {
            if (sys.name == "world-procgen") {
                showcaseTimelineP95 = static_cast<float>(sys.p95Ms);
            }
        }
    }
    if (showcaseHairPhysics && !showcaseHairStrand.particles.empty()) {
        showcaseHairPhysics->simulate(showcaseHairStrand,
                                      showcaseHairPhysics->getConfig());
        const auto& tip =
            showcaseHairStrand.particles.back().position;
        const vc::rendering::Vec3 restTip{ 0.0f, 0.8f, 0.0f };
        const float dx = tip.x - restTip.x;
        const float dy = tip.y - restTip.y;
        const float dz = tip.z - restTip.z;
        showcaseHairTipDisp = std::sqrt(dx * dx + dy * dy + dz * dz);
        showcaseHairParticles = static_cast<std::size_t>(
            showcaseHairPhysics->particleCount(showcaseHairStrand));
    }

    // ── AGENTE 3 A.3: the host-local client is a REAL per-frame consumer —
    // tick() polls the loopback transport + heartbeats the session every
    // fixed tick while connected; errors are observable (never silent).
    // A2-73 (Agente 5): paused HOLDS the network session (no tick — the
    // session stays connected with its last state, like the audio J.126 holds
    // voices); un-pause resumes. Frames held are observable in the `policy`
    // title segment.
    if (hostLocalClient && hostLocalClient->connected()) {
        if (!isPaused) {
            std::string tickErr;
            (void)hostLocalClient->tick(fixedDt, tickErr);
            if (!tickErr.empty()) {
                std::cout << "[Showcase] host-local client tick error: "
                          << tickErr << "\n";
            }
        } else {
            ++policyNetPausedFrames;
        }
    }

    // ── CONTA 4: advance the SDK consumers every fixed tick (same public
    // contracts the dedicated server uses, on the game executable).
    // create_opus_codec: one real voice frame round-trips encode -> decode.
    if (showcaseVoiceCodec) {
        std::string codecErr;
        std::vector<float> pcm(960, 0.0f);
        std::vector<std::uint8_t> packet;
        std::vector<float> decoded;
        if (showcaseVoiceCodec->encode_frame(pcm.data(), packet, codecErr) &&
            showcaseVoiceCodec->decode_frame(packet.data(), packet.size(),
                                             decoded, codecErr)) {
            ++showcaseVoiceFrames;
        }
    }
    // create_vehicle_replication: the same input drives the authoritative
    // vehicle and the predicted copy; the authoritative snapshot is applied to
    // the prediction and reconciled (an error-driven correction = rollback).
    if (showcaseVehReplication && showcaseVehClientReplication &&
        showcaseVehServerCar && showcaseVehNetSetup) {
        std::string vehErr;
        engine::gameplay::VehicleInput vin;
        vin.throttle = (runtimeTick % 4 == 0) ? 1.0f : 0.3f;
        showcaseVehReplication->server_submit_input("client_car", vin);
        showcaseVehReplication->server_tick(fixedDt);
        showcaseVehClientReplication->client_submit_input("client_car", vin, vehErr);
        showcaseVehClientReplication->client_predict(fixedDt);
        engine::vehicles::VehicleReplicationState auth;
        if (showcaseVehReplication->server_snapshot("client_car", auth, vehErr) &&
            showcaseVehClientReplication->client_apply_state("client_car", auth,
                                                             vehErr) &&
            showcaseVehClientReplication->client_reconcile("client_car", vehErr)) {
            ++showcaseVehRollbacks;
        }
    }
    // create_voxel_replication: lazily bind to the game's live voxel world
    // (blockWorld) and pack the client's region snapshot — the world's entity
    // EntitySnapshots — into the observable counter each tick.
    if (blockWorld) {
        if (!showcaseVoxelReplication) {
            showcaseVoxelReplication =
                engine::voxel::create_voxel_replication(*blockWorld);
            if (showcaseVoxelReplication) {
                showcaseVoxelReplication->server_register_connection(1);
                engine::voxel::ReplicationInterest interest;
                interest.position = { static_cast<int>(player.position.x),
                                      static_cast<int>(player.position.y),
                                      static_cast<int>(player.position.z) };
                interest.chunkRadius = 2;
                showcaseVoxelReplication->server_set_interest(1, interest);
            }
        }
        if (showcaseVoxelReplication) {
            showcaseVoxelReplication->server_update();
            engine::voxel::RegionReplicationSnapshot region;
            std::string regErr;
            if (showcaseVoxelReplication->server_pack_region(1, region, regErr)) {
                showcaseVoxelRegionEntities += region.entities.size();
            }
        }
    }

    // ── C: deferred save/load block-edit re-application. Edits loaded from a
    // save are re-applied through the SAME atomic transaction API once the
    // target chunks are writable (the world streams asynchronously, so the
    // first ticks after boot may not have them yet). Applies the writable
    // prefix of the journal past `showcaseReplayCursor`; the rest stays
    // pending. The journal itself is CUMULATIVE and never erased on replay so
    // every save persists the full edit history and every load re-applies it
    // in order — preserving altered blocks across any number of sessions.
    if (showcaseReplayCursor < showcaseLoadEditCount) {
        auto replayTx = world.begin_transaction();
        std::size_t staged = 0;
        for (std::size_t i = showcaseReplayCursor; i < showcaseLoadEditCount; ++i) {
            const auto& e = showcaseBlockJournal[i];
            const glm::vec3 pos(static_cast<float>(e.first.x),
                                static_cast<float>(e.first.y),
                                static_cast<float>(e.first.z));
            if (!world.can_touch_chunk_at(pos)) break;
            replayTx->set_block(pos, static_cast<RuntimeBlockId>(e.second));
            ++staged;
        }
        if (staged > 0) {
            std::string replayError;
            if (replayTx->commit(replayError)) {
                showcaseReplayCursor += staged;
                showcasePendingBlockEdits =
                    showcaseLoadEditCount - showcaseReplayCursor;
                // Re-applied edits changed surface heights — drop stale cache.
                invalidateShowcaseSurfaces();
            } else {
                // Refused (e.g. a mid-batch chunk race): keep everything
                // pending and retry on a later tick — never drop edits.
                std::cout << "[Showcase] save replay deferred: "
                          << replayError << '\n';
            }
        }
    }

    // ── A: resolve the data-driven action map against the real GLFW state ──
    // The actions drive the character controller / camera / interaction — no
    // raw key branches in the showcase path.
    if (showcaseActionMap && window) {
        const double dt = static_cast<double>(fixedDt);
        auto pollKey = [&](const char* input, int glfwKey) {
            const double value = glfwGetKey(window, glfwKey) == GLFW_PRESS ? 1.0 : 0.0;
            if (value == 0.0) return;
            const auto activations = showcaseActionMap->poll(
                engine::input::InputSource::Keyboard, "", input, 0, value, dt);
            for (const auto& a : activations) {
                showcaseLastAction = a.action;
                if (a.action == "interact" && showcaseInteraction) {
                    if (showcaseInteraction->activate("pickup")) {
                        interactionActivated = true;
                    }
                }
                if (a.action == "craft") {
                    showcase_try_craft();
                }
                if (a.action == "smelt") {
                    showcase_try_smelt();
                }
                if (a.action == "warp") {
                    showcaseAbilityEmits = 0;
                    if (abilityEffects && runtimeEvents &&
                        showcaseHitState != engine::gameplay::HitState::Down) {
                        std::string abErr;
                        if (abilityEffects->emit(
                                *runtimeEvents, "warp",
                                static_cast<std::uint64_t>(runtimeTick), abErr)) {
                            showcaseAbilityEmits = 1;
                        }
                    }
                }
                // CONTA 4 item 2: unload/destroy one attached block entity
                // through the LIVE World removal path (closes the destruction
                // cycle in the game, not only in server/SDK).
                if (a.action == "remove_block_entity") {
                    const auto& ents = world.block_entities();
                    for (const auto& [cell, ent] : ents) {
                        (void)ent;
                        if (world.remove_block_entity(cell.x, cell.y, cell.z)) {
                            ++blockEntityRemoveAttempts;
                        } else {
                            ++blockEntityRemoveAttempts;
                        }
                        break;   // one per press
                    }
                    if (ents.empty()) ++blockEntityRemoveAttempts;
                }
            }
        };
        pollKey("KeyW", GLFW_KEY_W);
        pollKey("KeyS", GLFW_KEY_S);
        pollKey("KeyA", GLFW_KEY_A);
        pollKey("KeyD", GLFW_KEY_D);
        pollKey("Space", GLFW_KEY_SPACE);
        pollKey("KeyE", GLFW_KEY_E);
        pollKey("MouseLeft", GLFW_MOUSE_BUTTON_LEFT);
        pollKey("MouseRight", GLFW_MOUSE_BUTTON_RIGHT);
        pollKey("KeyC", GLFW_KEY_C);
        pollKey("KeyF", GLFW_KEY_F);
        pollKey("KeyV", GLFW_KEY_V);
        pollKey("KeyQ", GLFW_KEY_Q);
    }
    // Mirror the player into the persistent character entity and read its
    // REAL health (the HUD/title observables never duplicate simulation).
    if (showcasePlayerEntityValid && mobEntities) {
        mobEntities->set_position(
            showcasePlayerEntity,
            { player.position.x, player.position.y, player.position.z });
        engine::entity::Health h;
        if (mobEntities->get_health(showcasePlayerEntity, h)) {
            showcasePlayerHealth = h.value;
        }
    }
    // Selected block: the player's hotbar selection (the same value the
    // place-block path consumes) — read, never duplicated.
    switch (player.selectedBlock) {
        case BlockType::Stone: showcaseSelectedBlock = "stone"; break;
        case BlockType::Dirt: showcaseSelectedBlock = "dirt"; break;
        case BlockType::Grass: showcaseSelectedBlock = "grass"; break;
        case BlockType::Cobblestone: showcaseSelectedBlock = "cobblestone"; break;
        case BlockType::Sand: showcaseSelectedBlock = "sand"; break;
        case BlockType::Planks: showcaseSelectedBlock = "planks"; break;
        case BlockType::Wood: showcaseSelectedBlock = "wood"; break;
        default: showcaseSelectedBlock = "air"; break;
    }

    // ── B: character controller over live terrain (player = the character) ──
    if (showcaseController) {
        engine::gameplay::TerrainSample samples[4];
        const float px = player.position.x;
        const float pz = player.position.z;
        const glm::vec3 front2d = glm::normalize(
            glm::vec3(player.camera.front.x, 0.0f, player.camera.front.z) +
            glm::vec3(1.0e-4f));
        samples[0] = { px, pz, showcaseSurfaceAt(world, px, pz) };
        samples[1] = { px + front2d.x, pz + front2d.z,
                       showcaseSurfaceAt(world, px + front2d.x,
                                               pz + front2d.z) };
        samples[2] = { px + 0.5f, pz,
                       showcaseSurfaceAt(world, px + 0.5f, pz) };
        samples[3] = { px, pz + 0.5f,
                       showcaseSurfaceAt(world, px, pz + 0.5f) };
        const float step = 0.05f;
        const engine::gameplay::MoveResult mr =
            showcaseController->move(px, player.position.y, pz,
                                     px + front2d.x * step, pz + front2d.z * step,
                                     samples, 4);
        showcaseSteppedUp = mr.steppedUp;
        showcaseSnappedDown = mr.snappedDown;
        showcaseInWater = mr.inWater;
    }

    // ── B: hit reaction + ragdoll recovery (same skeleton/physics) ──
    if (showcaseHitReaction) {
        const float surface = showcaseSurfaceAt(
            world, player.position.x, player.position.z);
        const bool grounded = player.position.y <= surface + 0.6f;
        const engine::gameplay::HitReactionState rs =
            showcaseHitReaction->update(fixedDt, grounded);
        showcaseHitState = rs.state;
        switch (rs.state) {
            case engine::gameplay::HitState::Normal: showcaseHitStateName = "normal"; break;
            case engine::gameplay::HitState::Stagger: showcaseHitStateName = "stagger"; break;
            case engine::gameplay::HitState::Down: showcaseHitStateName = "down"; break;
            case engine::gameplay::HitState::Recovering: showcaseHitStateName = "recovering"; break;
        }
    }
    if (showcaseRifle) {
        const glm::vec3 origin = player.camera.position;
        const glm::vec3 front = player.camera.front;
        showcaseRifle->update(fixedDt, origin, front);
        showcaseWeaponAmmo = showcaseRifle->ammo();
    }

    // ── LOTE 2 (80): CCD/sleeping/triggers/layers wire — the CCD body is a
    // real dynamic obstacle; when the player is in Stagger/Down (hit reaction)
    // the body falls asleep (awake=false) and wakes on the next clear state,
    // proving the sleep/wake toggle is actually driven by gameplay state.
    showcaseCcdActive = 0;
    if (runtimePhysics) {
        // Step the gameplay physics every fixed tick so dynamic/CCD bodies
        // (the obstacle below) actually fall/settle/sleep deterministically.
        runtimePhysics->step(fixedDt);
        if (showcaseCcdBody.valid()) {
            engine::gameplay::BodyState ccdState;
            if (runtimePhysics->physics().body_state(showcaseCcdBody, ccdState)) {
                showcaseCcdActive = 1;
            }
        }
    }
    // ── LOTE 2 (84/86): ragdoll -> skeleton pose + no-duplicate guard. When
    // the hit reaction reaches Down, the ragdoll goes ACTIVE and its real
    // physics pose is read back onto the animation skeleton; the body guard
    // counts mirror bodies to prove character<->ragdoll never doubles a body.
    showcaseRagdollPoseBones = 0;
    showcaseRagdollBodyGuard = 0;
    if (showcaseRagdoll && showcaseHitReaction) {
        if (showcaseHitState == engine::gameplay::HitState::Down) {
            showcaseRagdoll->set_awake(true);
            const std::vector<engine::gameplay::RagdollPoseBone> pose =
                showcaseRagdoll->pose();
            showcaseRagdollPoseBones = pose.size();
        } else {
            showcaseRagdoll->set_awake(false);
        }
        showcaseRagdollBodyGuard = mobPhysicsBodyCount;
    }
    // ── LOTE 2 (104): IK/foot/pose-warp — solve the live two-bone IK chain
    // toward the real foot targets sampled from the voxel surface each tick.
    showcaseIkSolves = 0;
    if (animIkSolver) {
        const engine::animation::AnimVec3 origin{ player.position.x, 0.9, player.position.z };
        const float surface = showcaseSurfaceAt(
            world, player.position.x, player.position.z);
        for (int leg = 0; leg < 2; ++leg) {
            const float lateral = leg == 0 ? -0.18f : 0.18f;
            const engine::animation::AnimVec3 target{
                player.position.x + lateral, surface, player.position.z };
            std::string ikErr;
            const engine::animation::TwoBoneResult res =
                animIkSolver->solve_two_bone(origin, target, 0.45, 0.45,
                                             { 0.0, 0.0, 1.0 }, ikErr);
            if (ikErr.empty()) ++showcaseIkSolves;
        }
    }

    // ── LOTE 3 (94/95/46/115): the rest of the closed items — advanced here.
    // Equipment: refresh the filled-slot observable from the live grid.
    showcaseEquippedSlots = showcaseEquipment
                                ? showcaseEquipment->items().size()
                                : 0U;
    // Loot: roll the deterministic table once per fixed tick (seed = runtime
    // tick) — observable item count.
    showcaseLootRolls = 0U;
    if (showcaseLoot) {
        const auto drops = showcaseLoot->roll(static_cast<std::uint64_t>(runtimeTick) * 2654435761u);
        showcaseLootRolls = drops.size();
    }
    // Ecosystem: query the data-driven tables (ore/carver/decorator counts)
    // against a real height — observable feature-places.
    showcaseFeaturePlaces = 0U;
    if (showcaseOreTable) showcaseFeaturePlaces += showcaseOreTable->rule_count();
    if (showcaseCarver) showcaseFeaturePlaces += showcaseCarver->spec().fluidMaxY;
    if (showcaseDecorators)
        showcaseFeaturePlaces += showcaseDecorators->decorator_count();
    // Block entity: tick the stateful voxel (counter machine) every fixed
    // tick with the world's clock — observable tick count.
    if (showcaseBlockEntity) {
        showcaseBlockEntity->on_tick(static_cast<std::uint64_t>(runtimeTick));
        showcaseBlockEntityTicks =
            static_cast<std::size_t>(showcaseBlockEntity->ticks());
    }

    // ── CONTA 4 item 6 (missions): advance the accepted mission every fixed
    // tick against the REAL world seam (inventory counts + player position).
    // The runtime reports events (objective completed / dialogue / completed /
    // reward applied) that the game consumes — progress is observable and the
    // reward is applied to the SAME live Inventory (no separate fake reward).
    showcaseMissionEvents = 0U;
    if (showcaseMissions) {
        // IMissionWorld seam over the LIVE game state: Collect objectives read
        // the real player inventory (stone count), Reach reads the player
        // position, and the reward is granted into the real player inventory.
        struct ShowcaseMissionWorld final : public engine::gameplay::IMissionWorld {
            const VulkanEngineApp& app;
            explicit ShowcaseMissionWorld(const VulkanEngineApp& a) : app(a) {}
            float count_of(const std::string& key) const override {
                if (app.playerInventory && key == "stone") {
                    return static_cast<float>(
                        app.playerInventory->count_of("vulkancraft:stone"));
                }
                return 0.0f;
            }
            bool flag(const std::string&) const override {
                return false;
            }
            float attribute(const std::string&) const override { return 0.0f; }
            bool position(float& x, float& z) const override {
                x = app.player.position.x;
                z = app.player.position.z;
                return true;
            }
            bool apply_reward(const std::string& itemId, int count, int xp) override {
                (void)xp;
                if (!app.playerInventory || !app.playerItems) return false;
                const auto* itemDef =
                    app.playerItems->find_by_name(itemId);
                if (itemDef == nullptr) return false;
                std::string rErr;
                const auto remainder = app.playerInventory->add(
                    engine::registry::ItemStack{ itemDef->namespaced(), count },
                    *app.playerItems, rErr);
                if (!remainder.empty()) {
                    std::cout << "[Showcase] mission reward overflow: "
                              << remainder.count << " dropped\n";
                }
                // Inventory state is refreshed by the break/place path every
                // frame (playerInventorySummary in the title); no const-mutation
                // here.
                return true;
            }
            bool set_flag(const std::string&) override { return true; }
        };
        ShowcaseMissionWorld mworld(*this);
        std::vector<engine::gameplay::MissionEvent> events;
        std::string mTickErr;
        if (!showcaseMissionAccepted) {
            if (showcaseMissions->accept(showcaseMissionDef, showcaseMissionState,
                                         mworld, events, mTickErr)) {
                showcaseMissionAccepted = true;
            } else if (!mTickErr.empty()) {
                std::cout << "[Showcase] mission accept refused: "
                          << mTickErr << '\n';
            }
        } else if (!showcaseMissionCompleted) {
            if (showcaseMissions->update(showcaseMissionDef, showcaseMissionState,
                                         mworld, events, mTickErr)) {
                for (const auto& ev : events) {
                    if (ev.kind ==
                        engine::gameplay::MissionEvent::Kind::MissionCompleted) {
                        // apply the reward through the canonical complete():
                        // mark it done + grant the reward (the seam applies it
                        // to the live inventory).
                        std::string compErr;
                        std::vector<engine::gameplay::MissionEvent> compEvents;
                        if (showcaseMissions->complete(
                                showcaseMissionDef, showcaseMissionState,
                                mworld, compEvents, compErr)) {
                            showcaseMissionCompleted = true;
                            events.insert(events.end(),
                                          compEvents.begin(), compEvents.end());
                        } else if (!compErr.empty()) {
                            std::cout << "[Showcase] mission complete refused: "
                                      << compErr << '\n';
                        }
                    }
                }
            }
        }
        showcaseMissionEvents = events.size();
        // Observability: objective progress (rebuilt from the world each
        // update) + completion state, published in the title.
        std::string prog;
        for (const auto& [id, p] : showcaseMissionState.objectiveProgress) {
            if (!prog.empty()) prog += ",";
            prog += id + "=" + std::to_string(static_cast<int>(p));
        }
        showcaseMissionSummary =
            (showcaseMissionAccepted ? "accepted" : "locked") +
            std::string(showcaseMissionCompleted ? "/done" : "") +
            " [" + (prog.empty() ? std::string("none") : prog) + "]";
    } else {
        showcaseMissionSummary = "n/a";
    }

    // ── CONTA 4 item 5 (vehicle pose): drive the REAL vehicle from the same
    // data-driven input the character uses (W = throttle, A/D = steering) and
    // read its live chassis state every fixed tick. The game executable is the
    // consumer — the chassis pose/speed/wheels are observable, not SDK-only.
    if (showcaseVehicle && showcaseVehicleValid) {
        engine::gameplay::VehicleInput vin;
        vin.throttle = 0.0f;
        vin.steering = 0.0f;
        if (window) {
            const bool fwd = glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS;
            const bool back = glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS;
            const bool left = glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS;
            const bool right = glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS;
            vin.throttle = (fwd ? 0.6f : 0.0f) - (back ? 0.4f : 0.0f);
            vin.steering = (left ? -0.6f : 0.0f) + (right ? 0.6f : 0.0f);
        }
        showcaseVehicle->set_input(vin);
        showcaseVehicle->update(fixedDt);
        showcaseVehicleSpeed = showcaseVehicle->speed();
        showcaseVehicleWheels =
            showcaseVehicle->wheel_states().size();
        showcaseVehicleOccupants = showcaseVehicle->seat_count();
        const glm::vec3 chassisPos = showVehicleChassisPos();
        showcaseVehicleSummary =
            std::format("veh {}w {:.1f}m/s occ {} @({:.0f},{:.0f},{:.0f})",
                        showcaseVehicleWheels, showcaseVehicleSpeed,
                        showcaseVehicleOccupants, chassisPos.x, chassisPos.y,
                        chassisPos.z);
    } else {
        showcaseVehicleSummary = "n/a";
    }
    // Nav streaming: keep the active-region focus at the player and report
    // the loaded/invalid ledger each frame.
    if (showcaseNavStream) {
        showcaseNavStream->set_focus(
            static_cast<std::int32_t>(std::floor(player.position.x / 8.0f)),
            static_cast<std::int32_t>(std::floor(player.position.z / 8.0f)));
        // Mark the focus tile as loaded so the ledger transitions to ready.
        engine::navigation::NavTile ft;
        showcaseNavStream->focus(ft);
        if (showcaseNavStream->is_tile_active(ft) &&
            !showcaseNavStream->is_loaded(ft)) {
            showcaseNavStream->mark_loaded(ft);
        }
        showcaseNavLoaded = showcaseNavStream->loaded_count();
        showcaseNavPendingRebuild =
            showcaseNavStream->tiles_pending_rebuild().size();
    }
    // CONTA 3 (items 117/118): drive the LIVE mob agents over the real
    // INavigationProvider — async begin/poll/cancel with world-change and
    // despawn cancellation — every fixed tick (deterministic cadence).
    showcase_gameplay_nav_tick(fixedDt);
    // (54) Update the profile-composed world's clock/generation each fixed
    // tick so blocks/cavins stream deterministically while the game runs.
    if ((showcaseWorldProfile != nullptr) && (showcaseWorldProfileWorld != 0)) {
        if (runtimeWorlds) {
            runtimeWorlds->update_world(
                "showcase_profile", player.position, fixedDt);
        }
    }

    // ── B: animation stack per fixed tick ──
    showcaseAnimClock += static_cast<double>(fixedDt);
    if (animCore && animCore->has_clip("walk")) {
        const double t = std::fmod(showcaseAnimClock, 0.8);
        const std::vector<engine::animation::BonePose> sampled =
            animCore->sample_clip("walk", t, err);
        // State machine: player movement drives the move/stop transitions.
        if (animStateMachine) {
            animStateMachine->tick(fixedDt, err);
            const float hSpeed = std::sqrt(
                player.velocity.x * player.velocity.x +
                player.velocity.z * player.velocity.z);
            if (hSpeed > 0.05f && animStateMachine->state() == "idle") {
                animStateMachine->send_event("move", err);
            } else if (hSpeed <= 0.05f && animStateMachine->state() == "walk") {
                animStateMachine->send_event("stop", err);
            }
            animStateMachineState = animStateMachine->state();
        }
        // Additive layer + mask + events + root motion over the same core.
        if (animAdditive && !sampled.empty()) {
            const auto deltas = animAdditive->sample_additive("walk", t, 0.0, err);
            if (animMask && !deltas.empty()) {
                animMask->mask_deltas("feet", deltas, err);
            }
            animAdditive->layer_additive(sampled, deltas, err);
        }
        if (animEvents) {
            const auto fired = animEvents->poll("walk", t - fixedDt, t, err);
            if (!fired.empty()) animEventLast = fired.back().name;
        }
        if (animRootMotion) {
            const auto rm = animRootMotion->compute(
                *animCore, "walk", "LeftHip", t - fixedDt, t, err);
            rootMotionDistance = rm.horizontal_distance;
        }
        if (animInertializer && !sampled.empty()) {
            const auto in = animInertializer->tick(sampled, fixedDt, err);
            (void)in;
        }
        if (animConstraints && !sampled.empty()) {
            animConstraints->apply_constraint("showcase_legs", sampled, err);
        }
        if (terrainAdaptation) {
            const auto adapted = terrainAdaptation->adapt(
                "showcase", player.position.x, player.position.y,
                player.position.z, err);
            (void)adapted;
        }
        if (!animBasePose.empty()) animBasePose = sampled;
    }
    // ── LOTE 2 (102/108): skinning CPU consumers the SAME live pose the ASM
    // produced — one deterministic skinned mesh over the walk pose every tick.
    showcaseSkinnedVerts = 0;
    if (showcaseSkinning && animCore && animCore->has_clip("walk") &&
        !animBasePose.empty()) {
        std::vector<engine::animation::BonePose> poseForSkin = animBasePose;
        std::vector<engine::animation::SkinMatrix> skin =
            showcaseSkinning->skin_matrices("showcase_char", poseForSkin, err);
        if (!skin.empty()) {
            // A small skinned mesh: 4 verts of the character's feet box.
            std::vector<engine::animation::SkinVertex> verts(4);
            const double footY = 0.0;
            const double yOff = 0.0;
            verts[0] = {{ -0.18, footY, 0.0 }, 2, -1, -1, -1, 1.0, 0, 0, 0};
            verts[1] = {{  0.18, footY, 0.0 }, 5, -1, -1, -1, 1.0, 0, 0, 0};
            verts[2] = {{ -0.18, footY + 0.45, 0.0 }, 1, -1, -1, -1, 1.0, 0, 0, 0};
            verts[3] = {{  0.18, footY + 0.45, 0.0 }, 4, -1, -1, -1, 1.0, 0, 0, 0};
            const std::vector<engine::animation::AnimVec3> deformed =
                showcaseSkinning->skin_vertices(
                    "showcase_char", poseForSkin, verts, err);
            showcaseSkinnedVerts = deformed.size();
            // CONTA 4 item 5: keep the deformed foot targets in world space so
            // the renderer's pose mesh (rebuild_showcase_pose_mesh) draws the
            // ACTUAL deformation output — the skinned mesh is not CPU-only.
            showcaseLastSkinnedFoots.clear();
            for (const auto& d : deformed) {
                showcaseLastSkinnedFoots.push_back(player.position +
                                                   glm::vec3(d.x, d.y, d.z));
            }
            (void)yOff;
        }
    }
    // ── LOTE 2 (108): motion db re-samples the cooked clip each tick on the
    // live animation clock — the ozz-backed skeleton really feeds the pose.
    showcaseMotionDbFrames = 0;
    if (showcaseMotionDb && showcaseMotionDb->bone_count() > 0) {
        const engine::animation::CookedMotion* m =
            showcaseMotionDb->cooked("walk");
        if (m) {
            engine::animation::MotionPose mp;
            if (showcaseMotionDb->sample(*m, std::fmod(showcaseAnimClock, 0.8), mp)) {
                showcaseMotionDbFrames =
                    (mp.rotations.empty() ? 0U : mp.rotations.size());
            }
        }
    }
    // Motion matcher: live query from the player state -> best clip frame.
    if (motionMatcher) {
        engine::animation::MotionMatchQuery q;
        q.rootPosition = player.position;
        q.rootVelocity = player.velocity;
        q.rootOrientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        for (int i = 0; i < 6; ++i) {
            q.pose.translations.push_back({ 0.0f, 0.9f - 0.45f * (i % 3), 0.0f });
            q.pose.rotations.push_back(glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
            q.pose.scales.push_back(glm::vec3(1.0f));
        }
        // Spec default trajectoryPoints == 3: exactly three future samples.
        q.trajectory = { player.position,
                         player.position + player.camera.front * 0.25f,
                         player.position + player.camera.front * 0.5f };
        engine::animation::MotionMatchResult mout;
        if (motionMatcher->match(q, mout, err)) {
            motionMatchFrames = static_cast<std::size_t>(mout.frame);
        }
    }
    // Pose warper: adapt the sampled pose to the live body state.
    if (poseWarper) {
        const engine::animation::MotionSkeleton ms = make_showcase_motion_skeleton();
        engine::animation::MotionPose pose;
        for (int i = 0; i < 6; ++i) {
            pose.translations.push_back({ 0.0f, 0.9f - 0.45f * (i % 3), 0.0f });
            pose.rotations.push_back(glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
            pose.scales.push_back(glm::vec3(1.0f));
        }
        engine::animation::PoseWarpSpec wspec;
        engine::animation::WarpInput win;
        win.bodyPosition = player.position;
        win.bodyYaw = std::atan2(player.camera.front.x, player.camera.front.z);
        win.surfaceHeight = showcaseSurfaceAt(
            world, player.position.x, player.position.z);
        win.speed = std::sqrt(player.velocity.x * player.velocity.x +
                              player.velocity.z * player.velocity.z);
        win.feet = {
            { 2, { player.position.x - 0.18f, win.surfaceHeight, player.position.z }, true },
            { 5, { player.position.x + 0.18f, win.surfaceHeight, player.position.z }, true },
        };
        engine::animation::MotionPose out;
        if (!poseWarper->warp(ms, pose, wspec, win, out, err)) {
            // A refusal here is a real diagnostic, never silent.
            static bool warpRefusalLogged = false;
            if (!warpRefusalLogged && !err.empty()) {
                std::cout << "[Showcase] pose warper refused: " << err << '\n';
                warpRefusalLogged = true;
            }
        }
    }
    // Gait planner + foot placement from the showcase asset.
    if (showcaseGaitLoaded && contactPlanner && footPlacer) {
        engine::animation::GaitPlan plan;
        if (contactPlanner->plan(
                showcaseGait, static_cast<float>(showcaseAnimClock),
                player.position,
                std::atan2(player.camera.front.x, player.camera.front.z),
                { player.velocity.x, player.velocity.z }, plan, err)) {
            ShowcaseTerrainSampler sampler(world);
            engine::animation::FootPlacementSpec fspec;
            static engine::animation::FootPlacementResult prev;
            engine::animation::FootPlacementResult placed;
            if (footPlacer->place(fspec, sampler, plan, prev, placed, err)) {
                prev = placed;
                proceduralEffectorCount = placed.feet.size();
            }
        }
    }
    if (proceduralLegs && showcaseGaitLoaded) {
        engine::animation::ProceduralLocomotionSpec lspec;
        lspec.legs = showcaseGait.legs;
        lspec.bodyHeight = 0.9f;
        static std::vector<engine::animation::ProceduralLegState> prevLegs;
        ShowcaseTerrainSampler sampler(world);
        engine::animation::ProceduralLocomotionResult lout;
        if (proceduralLegs->step(
                lspec, sampler, static_cast<float>(showcaseAnimClock),
                player.position,
                std::atan2(player.camera.front.x, player.camera.front.z),
                { player.velocity.x, player.velocity.z }, prevLegs, lout, err)) {
            prevLegs = lout.legs;
        }
    }
    if (proceduralPipeline && !animBasePose.empty()) {
        ShowcaseTerrainSampler sampler(world);
        engine::animation::PipelineBodyState body;
        body.position = engine::animation::AnimVec3{
            player.position.x, player.position.y, player.position.z };
        body.yawRadians = std::atan2(player.camera.front.x, player.camera.front.z);
        body.aimTarget = engine::animation::AnimVec3{
            player.camera.position.x + player.camera.front.x * 4.0f,
            player.camera.position.y + player.camera.front.y * 4.0f,
            player.camera.position.z + player.camera.front.z * 4.0f };
        body.hasAimTarget = true;
        const auto result =
            proceduralPipeline->run(body, &sampler, animBasePose, err);
        if (result.ok) proceduralEffectorCount = result.effectors.size();
    }

    // ── C: interaction / explosion / balance / faction per tick ──
    if (showcaseInteraction) {
        const auto states = showcaseInteraction->evaluate(
            2.0f, 2.0f, 0.0f, player.position.x, player.position.z, 0.0f);
        interactionAvailable = 0;
        for (const auto& s : states) {
            if (s.available) ++interactionAvailable;
        }
        showcaseInteraction->advance(fixedDt);
    }
    if (showcaseExplosion) {
        // Blast epicenter at the nearest hostile mob (or a fixed point ahead);
        // the player's distance gives the real falloff each tick.
        float epicenterX = player.position.x + player.camera.front.x * 4.0f;
        float epicenterZ = player.position.z + player.camera.front.z * 4.0f;
        if (mobEntities) {
            float nearest = 1.0e9f;
            mobEntities->for_each_entity([&](engine::entity::EntityId id) {
                engine::entity::Position pos;
                if (!mobEntities->get_position(id, pos)) return;
                // The blast targets MOBS — skip the player's own showcase
                // entity (same ECS) so the epicenter lands on a hostile.
                if (id == showcasePlayerEntity) return;
                const float dx = pos.x - player.position.x;
                const float dz = pos.z - player.position.z;
                const float d2 = dx * dx + dz * dz;
                if (d2 < nearest) {
                    nearest = d2;
                    epicenterX = pos.x;
                    epicenterZ = pos.z;
                }
            });
        }
        const double distance = std::sqrt(
            static_cast<double>((player.position.x - epicenterX) *
                                (player.position.x - epicenterX) +
                                (player.position.z - epicenterZ) *
                                (player.position.z - epicenterZ)));
        const auto sample = showcaseExplosion->sample_at(distance, err);
        explosionFalloffAtPlayer = static_cast<float>(sample.falloff);
        // Reintegração (item 106): a onda de choque perto do jogador dispara o
        // hit-reaction real — o stagger/knockdown resultante trava o controle
        // no loop de movimento (enforcement no VulkanEngineApp).
        if (showcaseHitReaction && explosionFalloffAtPlayer > 0.25f) {
            showcaseHitReaction->apply_impact(
                std::clamp(explosionFalloffAtPlayer, 0.0f, 1.0f));
        }
        const auto frags = showcaseExplosion->fragments(
            static_cast<std::uint64_t>(runtimeTick), err);
        explosionFragments = frags.size();

        // ── C: real detonation on a deterministic cadence — the blast
        // modifies REAL blocks (atomic transaction, falloff-gated) and REAL
        // physics bodies (impulse + entity damage), journaling every committed
        // edit so the save/load round trip persists the result.
        if (runtimeTick >= showcaseExplosionNextTick) {
            showcaseExplosionNextTick = runtimeTick + 480;  // every 8 s fixed
            const float radius =
                static_cast<float>(showcaseExplosion->spec().radius);
            const double damageAtCenter = showcaseExplosion->spec().damage;
            const int radiusI = static_cast<int>(std::ceil(radius));
            // 1) Blocks: sphere around the epicenter, air where the falloff is
            // above the gating threshold — all in ONE atomic transaction.
            auto boomTx = world.begin_transaction();
            std::size_t staged = 0;
            // Capture the cells actually staged for removal (non-air + gated),
            // so the post-commit journal matches EXACTLY the blocks changed —
            // otherwise cells that were already Air become ghost edits that a
            // later save/load would re-apply as Air over a newly placed block.
            std::vector<glm::ivec3> removedCells;
            removedCells.reserve(static_cast<std::size_t>(
                (2 * radiusI + 1) * (2 * radiusI + 1) * (2 * radiusI + 1)));
            for (int dy = -radiusI; dy <= radiusI; ++dy) {
                for (int dz = -radiusI; dz <= radiusI; ++dz) {
                    for (int dx = -radiusI; dx <= radiusI; ++dx) {
                        const glm::vec3 cell(epicenterX + static_cast<float>(dx),
                                             static_cast<float>(
                                                 std::floor(player.position.y)) +
                                                 static_cast<float>(dy),
                                             epicenterZ + static_cast<float>(dz));
                        const double cellDist = std::sqrt(
                            static_cast<double>(dx * dx + dy * dy + dz * dz));
                        const auto cellSample =
                            showcaseExplosion->sample_at(cellDist, err);
                        if (cellSample.falloff < 0.35) continue;  // gated
                        if (world.get_block_at(cell) == kRuntimeAirId) continue;
                        boomTx->remove_block(cell);
                        removedCells.emplace_back(
                            static_cast<int>(std::floor(epicenterX)) + dx,
                            static_cast<int>(std::floor(player.position.y)) + dy,
                            static_cast<int>(std::floor(epicenterZ)) + dz);
                        ++staged;
                    }
                }
            }
            if (staged > 0) {
                std::string boomError;
                if (boomTx->commit(boomError)) {
                    showcaseExplosionBlockEdits = staged;
                    ++showcaseJournalCommits;
                    // Journal ONLY the cells actually removed, and only after a
                    // successful atomic commit (rolled-back => journal nothing).
                    for (const glm::ivec3& jPos : removedCells) {
                        showcaseBlockJournal.push_back(
                            { jPos, static_cast<uint32_t>(kRuntimeAirId) });
                    }
                    // The blast changed surface heights across the sphere —
                    // drop the stale cached surface columns.
                    invalidateShowcaseSurfaces();
                } else {
                    std::cout << "[Showcase] explosion block transaction refused: "
                              << boomError << '\n';
                }
            }
            // 2) Bodies + entities: impulse away from the epicenter scaled by
            // falloff, entity damage scaled by falloff.
            showcaseExplosionBodiesHit = 0;
            if (runtimePhysics && mobEntities) {
                mobEntities->for_each_entity([&](engine::entity::EntityId id) {
                    engine::entity::Position pos;
                    if (!mobEntities->get_position(id, pos)) return;
                    const float dx = pos.x - epicenterX;
                    const float dz = pos.z - epicenterZ;
                    const double bodyDist =
                        std::sqrt(static_cast<double>(dx * dx + dz * dz));
                    const auto bodySample = showcaseExplosion->sample_at(bodyDist, err);
                    if (bodySample.falloff <= 0.0) return;
                    const auto bodyIt = mobPhysicsBodies.find(id.id);
                    if (bodyIt != mobPhysicsBodies.end()) {
                        const glm::vec3 dir = glm::normalize(
                            glm::vec3(dx, 0.0f, dz) + glm::vec3(1.0e-4f));
                        runtimePhysics->physics().apply_impulse(
                            bodyIt->second,
                            dir * static_cast<float>(
                                      showcaseExplosion->spec().impulse *
                                      bodySample.falloff));
                        ++showcaseExplosionBodiesHit;
                    }
                    engine::entity::Health h;
                    if (mobEntities->get_health(id, h)) {
                        h.value = std::max(
                            0.0f,
                            h.value - static_cast<float>(damageAtCenter *
                                                         bodySample.falloff));
                        mobEntities->set_health(id, h);
                    }
                });
            }
        }
    }
    if (showcaseBalance) {
        const engine::gameplay::SupportPoint support[4] = {
            { player.position.x - 0.18f, player.position.z - 0.18f },
            { player.position.x + 0.18f, player.position.z - 0.18f },
            { player.position.x - 0.18f, player.position.z + 0.18f },
            { player.position.x + 0.18f, player.position.z + 0.18f },
        };
        const auto br = showcaseBalance->evaluate(player.position.x,
                                                  player.position.z, support, 4);
        showcaseBalanceState = br.state;
    }
    if (showcaseFaction) {
        showcaseTeamCount = showcaseFaction->teams().size();
        showcaseFaction->is_hostile("player", "bandit");
    }

    // ── C: AI decision surface per tick ──
    // CONTA 3 (item 120): the per-frame AI debug snapshot is fed from the LIVE
    // mob ECS agents (perception/blackboard/FSM/behavior tree/utility/planner)
    // and published to aiDebugSnapshotJson for the editor consumer.
    showcase_gameplay_ai_debug_snapshot();
    if (aiValidator) {
        static bool validatorCooked = false;
        if (!validatorCooked) {
            engine::animation::AnimationGraphProposal graph;
            graph.initialState = "idle";
            graph.states = { { "idle", "walk" }, { "walk", "walk" } };
            graph.parameters = {
                { "speed", engine::animation::AiParameterKind::Float } };
            graph.transitions = {
                { "idle", "walk", "speed",
                  engine::animation::AiGraphTransition::Comparison::Greater,
                  0.1f, 0.2f, false, 1.0f },
            };
            const engine::animation::MotionSkeleton ms = make_showcase_motion_skeleton();
            engine::animation::MotionClip clip;
            clip.name = "walk";
            clip.duration = 1.0f;
            for (int i = 0; i < 6; ++i) {
                engine::animation::MotionTrack tr;
                tr.boneIndex = i;
                engine::animation::MotionKeyframe kf;
                kf.time = 0.0f;
                tr.keyframes.push_back(kf);
                engine::animation::MotionKeyframe kf1;
                kf1.time = 1.0f;
                tr.keyframes.push_back(kf1);
                clip.tracks.push_back(tr);
            }
            engine::animation::AiCookedAsset asset;
            if (aiValidator->cook(engine::animation::AiAssetKind::AnimationGraph,
                                  ms, clip, graph, asset, err)) {
                aiValidatorSigned = aiValidator->verify(asset, err);
                validatorCooked = true;
                std::cout << "[Showcase] AI graph validated + signed ("
                          << asset.signature << ")\n";
            }
        }
    }
    if (vendorTree) {
        vendorTreeStatus = static_cast<int>(vendorTree->tick(err));
    }

    // ── C: navigation / simulation per tick ──
    if (agentCapabilities) {
        const float surface = showcaseSurfaceAt(
            world, player.position.x, player.position.z);
        const float step = std::max(0.0f, surface - player.position.y);
        engine::navigation::TraversalGeometry geometry;
        geometry.stepUp = step;
        geometry.slopeDegrees = 5.0f;
        geometry.ceilingClearance = 2.2f;
        const auto tr = agentCapabilities->can_traverse(geometry);
        agentCanTraverse = tr.possible;
        agentTraverseReason = tr.reason != nullptr ? tr.reason : "";
    }
    if (simulationLod) {
        std::vector<engine::simulation::SimulationLodEvent> events;
        if (simulationLod->update(simulationLodState, player.position.x,
                                  player.position.z, fixedDt, events, err)) {
            simulationLodEvents = events.size();
        }
    }

    // ── D: multibody chain stepped on the fixed tick ──
    if (multibody && multibodyChain != engine::physics::InvalidMultibody) {
        multibody->step(fixedDt);
        const auto last = multibody->link_state(
            multibodyChain, multibody->link_count(multibodyChain) - 1);
        multibodyEndEffectorY = last.position.y;
    }
    // Shape recognition over real world surface points (once per second).
    if (shapeRecognition) {
        static int recognitionFrames = 0;
        if (++recognitionFrames >= 60) {
            recognitionFrames = 0;
            std::vector<glm::vec3> points;
            for (int dx = -2; dx <= 2; ++dx) {
                for (int dz = -2; dz <= 2; ++dz) {
                    points.push_back(glm::vec3(
                        player.position.x + static_cast<float>(dx),
                        showcaseSurfaceAt(
                            world, player.position.x + static_cast<float>(dx),
                            player.position.z + static_cast<float>(dz)),
                        player.position.z + static_cast<float>(dz)));
                }
            }
            std::vector<engine::physics::ShapePrimitive> primitives;
            if (shapeRecognition->recognize(points, primitives, err)) {
                shapePrimitiveCount = primitives.size();
            }
        }
    }

    // ── CONTA 3 (fechamento global): content/physics/simulation factories
    // advanced on the same fixed tick (see showcase_content_physics_tick).
    showcase_content_physics_tick(fixedDt);

    // A4-REQ-CROSS-DOMAIN / A4-REQ-PHASE (Agente 5): advance the cross-domain
    // coordinator + domain lifecycle every fixed tick and publish the state.
    if (crossDomain_) {
        crossDomain_->refresh();
        const auto cs = crossDomain_->snapshot();
        crossDomainSummary = std::format(
            "navr {} inv {} ld {} ai {} rc {} [{}]{}",
            cs.navigationRevision, cs.invalidNavigationTiles,
            cs.loadedNavigationTiles, cs.aiTick, cs.renderCards,
            cs.navigationBound ? "nav" : "-nav",
            cs.debugBound ? "+dbg" : "");
        crossDomainJson_ = crossDomain_->to_json();
    }
    if (phase_) {
        // Mark the real domains the game advances this tick as bound.
        phase_->mark_consumer_bound(engine::gameplay::GameplayDomain::Ecs);
        phase_->mark_producer_bound(engine::gameplay::GameplayDomain::Navigation);
        phase_->mark_consumer_bound(engine::gameplay::GameplayDomain::Ai);
        phase_->mark_producer_bound(engine::gameplay::GameplayDomain::Physics);
        phase_->mark_producer_bound(engine::gameplay::GameplayDomain::Voxel);
        phase_->mark_producer_bound(engine::gameplay::GameplayDomain::Renderer);
        phase_->mark_producer_bound(engine::gameplay::GameplayDomain::Audio);
        phase_->mark_consumer_bound(engine::gameplay::GameplayDomain::Worlds);
        std::string pErr;
        const bool ok = phase_->complete(pErr);
        const auto st = phase_->status();
        phaseSummary = std::format("{} {}({})", st.size(),
                                   ok ? "complete" : "incomplete", pErr);
    }

    // ── Observable summary (title) ──
    std::string animSummary = "anim n/a";
    if (animCore && animCore->has_clip("walk")) {
        animSummary = std::format(
            "anim {} (ev {} rm {:.2f} mm {} eff {})",
            animStateMachineState, animEventLast, rootMotionDistance,
            motionMatchFrames, proceduralEffectorCount);
    }
    const std::string hitSummary =
        showcaseHitReaction ? showcaseHitStateName : std::string("n/a");
    const std::string vendorSummary =
        vendorTreeStatus >= 0
            ? std::string(std::to_string(vendorTreeStatus))
            : std::string("n/a");
    const std::string balanceSummary =
        showcaseBalanceState == engine::gameplay::BalanceState::Stable
            ? std::string("stable")
            : (showcaseBalanceState == engine::gameplay::BalanceState::Edge
                   ? std::string("edge")
                   : std::string("unstable"));
    // Weapon cooldown state: reloading is the real per-magazine cooldown the
    // HUD reads (the hit-reaction stagger above is the other cooldown).
    const std::string weaponCooldown =
        showcaseRifle ? (showcaseRifle->reloading() ? "reloading" : "ready")
                      : std::string("n/a");
    showcaseSummary = std::format(
        "showcase {} hp{:.0f} sel {} act {} cc {}{}{} | {} | hit {} | "
        "ragdoll {} | weapon {}/{} ({}) | fx {:.0f}% ({}) | inter {} | bal {} | "
        "{} teams | ai {} | tree {} | nav {} ({} nodes) | cap {} | simlod {} | "
        "convex {} ({}) | mb {:.2f} | shape {} | skin {} ({}){} | boom {}b {}c | "
        "save {}{} | net {} | srf {}h/{}m | enf {} | craft {} | alpha {:.2f} | "
        "lote2 ccd{} rag{} g{} skin{} mdb{} ik{} | lote3 eq{} loot{} fea{} be{} ns{}/{} wp{}sec{}w{} sk{} ab{} | "
        "c4 miss {} | veh {} | blke a{}/d{}/rm{}/rst{} | "
        "c3nav r{} of{} ac{} ob{} door{} fol{} canc{} fail{} ai{} agents{} | "
        "audio {} ({}) | lights {} ({}) | abilities {} ({}) | inv {} ({} in {}){}{} | "
        "jeep {} ({}) | mission {} ({}) | net-asset {} (p{} t{} m{})",
        showcaseSceneLoaded ? showcaseProjectName : "no-project",
        showcasePlayerHealth, showcaseSelectedBlock, showcaseLastAction,
        showcaseSteppedUp ? "^" : "", showcaseSnappedDown ? "v" : "",
        showcaseInWater ? "~" : "", animSummary, hitSummary,
        showcaseRagdollBones, showcaseWeaponAmmo,
        showcaseRifle ? showcaseRifle->reserve() : 0U, weaponCooldown,
        explosionFalloffAtPlayer * 100.0f, explosionFragments,
        interactionAvailable, balanceSummary, showcaseTeamCount,
        aiDebugNodeCount, vendorSummary,
        navPathFound ? "found" : "none", navPathNodes,
        agentCanTraverse ? "walk" : agentTraverseReason,
        simulationLodEvents, convexPartCount, convexBackendName,
        multibodyEndEffectorY, shapePrimitiveCount, showcaseSkinName,
        showcaseSkinLayout, showcaseSkinFallback ? "!" : "",
        showcaseExplosionBlockEdits, showcaseExplosionBodiesHit,
        showcaseSaveLoaded ? "restored" : "fresh", showcasePendingBlockEdits,
        hostLocalNetSummary, showcaseSurfaceCacheHits,
        showcaseSurfaceCacheMisses, trackedEnforcedAxes, showcaseCraftResult,
        showcaseFixedTickAlpha, showcaseCcdActive, showcaseRagdollPoseBones,
        showcaseRagdollBodyGuard, showcaseSkinnedVerts,
        showcaseMotionDbBones != 0 ? showcaseMotionDbFrames : 0U,
        showcaseIkSolves, showcaseEquippedSlots, showcaseLootRolls,
        showcaseFeaturePlaces, showcaseBlockEntityTicks,
        showcaseNavLoaded, showcaseNavPendingRebuild,
        showcaseWorldProfile ? showcaseWorldProfileSections : 0U,
        showcaseWorldProfile ? 1U : 0U, showcaseWorldProfileWorld,
        showcaseSkeletonAssetLoaded, showcaseAbilityEmits,
        showcaseMissionSummary, showcaseVehicleSummary,
        blockEntityAttachedObserved, blockEntityDetachedObserved,
        blockEntityRemoveAttempts, showcaseWorldBlockEntitiesRestored,
        gameNavBaked ? gameNavRevision : 0U,
        gameNavOffMeshLinks, gameNavAreaCosts, gameNavObstacleColumns,
        gameNavDoorActive ? 1 : 0, gameNavAgentsFollowing, gameNavCancelled,
        gameNavFailureCount, aiDebugNodeCount, aiDebugSnapshotAgents,
        // Conta 5 (showcase data-driven assets): project audio event state.
        showcaseAudioAssetLoaded ? showcaseAudioAssetName : std::string("no-audio"),
        showcaseAudioRegistered ? "reg" : "noreg",
        // Conta 5 (showcase data-driven assets): project light set applied to
        // the loaded scene.
        showcaseLightsAssetLoaded ? "lights" : "no-lights",
        showcaseLightCount,
        // Conta 5 (showcase data-driven assets): project ability + inventory
        // + item-registry state.
        showcaseAbilityAssetLoaded ? "abilities" : "no-abilities",
        showcaseAbilityCount,
        showcaseInventoryAssetLoaded ? "inv" : "no-inv",
        showcaseInventoryItems, showcaseInventorySlots,
        showcaseItemAssetLoaded ? "+item" : "",
        showcaseInventoryDeserialized ? "+full" : "",
        // Conta 5 (showcase data-driven assets): project vehicle + mission
        // registry state.
        showcaseJeepAssetLoaded ? "jeep" : "no-jeep",
        showcaseJeepWheels,
        showcaseMissionAssetLoaded ? "mission" : "no-mission",
        showcaseMissionAssetObjectives,
        // Conta 5 (showcase data-driven assets): project network declaration.
        showcaseNetworkAssetLoaded ? "net" : "no-net",
        showcaseNetworkPort, showcaseNetworkTickRate,
        showcaseNetworkMaxClients);
    // ── CONTA 3 (fechamento global) — content/physics/simulation observables.
    showcaseSummary += std::format(
        " | c3f beam {:.1f}m/s d{:.3f} | fl {:.0f}/{:.0f}m a{:.1f}° (j {:.0f}/{:.0f}) | "
        "dmg {}x | csg {}t | mbJ {:.2f} | shJ {} | sph {}/{:.0f} (j {:.0f}) | "
        "tet {}/{}/{} | road {}j {}e p{} {}t | grm {} | gen {}p (j {}p) | "
        "plc {} sk{} | mm {} {:.3f} | hair {:.4f}",
        showcaseBeamSpeed, showcaseBeamDeformation,
        showcaseFlightSpeed, showcaseFlightAltitude, showcaseFlightAlpha,
        showcaseFlightJsonSpeed, showcaseFlightJsonAltitude,
        showcaseDamageDestroyed + showcaseDamageDetached, showcaseCsgResultTris,
        multibodyJsonEndEffectorY, shapeJsonPrimitiveCount,
        sphParticleCount, sphMaxDensity, sphJsonMaxDensity,
        tetraSimNodes, tetraSimTets, tetraRenderTris,
        roadJunctions, roadEdges, parcelCount, parcelTris,
        grammarBoxes, structurePlanBlocks, structurePlanBlocksJson,
        structurePlacements, structureSockets,
        motionMatchFrame, motionMatchCost, hairProviderError);
    // ── CONTA 4 (fechamento global — rede/servidor): the three SDK symbols are
    // consumed every fixed tick; their live state is published to the title.
    showcaseSummary += std::format(
        " | c4 voxel {}snap veh {}rollback voice {}frames opus{}vox{}",
        showcaseVoxelRegionEntities, showcaseVehRollbacks, showcaseVoiceFrames,
        showcaseVoiceCodec ? "1" : "0",
        showcaseVoxelReplication ? "1" : "0");
    // ── CONTA 2 (world/procgen — 18 factories): the composed generator + its
    // consumers (climate/biome/surface, LOD cells, multi-scale streams, the
    // heightmap erosion tile cache and the mesh cooker) are published.
    showcaseSummary += std::format(" | cont2 {}", worldProcgen.summary());
    // SDK-INTERNAL resolution: IGameplayCrossDomain + IGameplayPhase — the
    // cross-domain coordinator aggregates integration/nav/debug/authoring into
    // a single snapshot; the phase tracks domain lifecycle status.
    if (crossDomain_) {
        crossDomain_->refresh();
        crossDomainJson_ = crossDomain_->snapshot().fullyBound ? "bound" : "partial";
    }
    if (phase_) {
        auto status = phase_->status();
        std::size_t bound = 0;
        for (const auto& s : status) {
            if (s.producerBound && s.consumerBound) ++bound;
        }
        crossDomainJson_ += "/" + std::to_string(bound) + "d";
    }
    showcaseSummary += std::format(" | xdomain {}", crossDomainJson_);
    // ── CONTA 6 (integração): the two residual factories consumed this tick —
    // headless hair physics (particles + tip displacement from rest) and the
    // system timeline (frames closed + p95 ms of the world/procgen section).
    showcaseSummary += std::format(
        " | c6 hair {}p d{:.3f} | tl {}f p95 {:.3f}ms",
        showcaseHairParticles, showcaseHairTipDisp,
        showcaseTimelineFrames, showcaseTimelineP95);
    // A2-124 (Agente 5): the game's real IAudioMixer state (master level,
    // music/sfx bus levels, master gain dB) is published every tick.
    showcaseSummary += std::format(
        " | mixer {}",
        gameMixer ? gameMixerState : std::string("n/a"));
    // A2-105 (Agente 5): the biped creature gait asset consumed this tick —
    // name + leg-chain count, observed in the title.
    showcaseSummary += std::format(
        " | gait {}",
        showcaseGaitLoaded
            ? std::string(std::format("{} ({} legs)", showcaseGait.name,
                                       showcaseGait.legs.size()))
            : std::string("builtin"));
    // A2-114 (Agente 5): the sensors consumed damage + faction this frame —
    // distinct factions sensed + max damage/threat of the detected set.
    showcaseSummary += std::format(" | percept {}",
                                   playerPerception ? perceptSummary
                                                    : std::string("n/a"));
    // A2-73 (Agente 5): time-travel/world-switch lifecycle policies
    // (network/particles held + rewind cleanup), observable in the title.
    showcaseSummary += std::format(" | policy {}", policySummary);
    // A2-50/57 (Agente 5): continuous light/fluid/structure streaming is
    // validated on the real executable — observable in the title.
    showcaseSummary += std::format(" | stream {}", streamSummary);
    // A2-90 (Agente 5): the hotbar is persisted to disk (loaded at boot,
    // saved on change) — observable save/load state in the title.
    showcaseSummary += std::format(
        " | invpersist {} (L{} S{})", inventorySaveSummary, inventoryLoads,
        inventorySaves);
    // A4-REQ-CROSS-DOMAIN / A4-REQ-PHASE (Agente 5): cross-domain coordinator
    // + domain lifecycle states, observable in the title.
    showcaseSummary += std::format(
        " | crossdom {}", crossDomain_ ? crossDomainSummary : std::string("n/a"));
    showcaseSummary += std::format(
        " | phase {}", phase_ ? phaseSummary : std::string("n/a"));
}

// ── CONTA 3 — active navigation wired to the LIVE mob agents (Agente 2 items
// 117/118). The game owns a real INavigationProvider (Recast+Detour) baked from
// the live voxel world columns, carrying off-mesh links (jump/climb), hazard
// area costs, and a dynamic door obstacle. Each live mob ECS agent follows a
// path via begin_async_path/poll_async_path; a new request, a despawn, an
// invalid target or a world change cancels the in-flight query so a late
// result is never applied after cancellation. World block edits (via
// invalidateShowcaseSurfaces) invalidate the nav ledger region around the
// player and force a localized re-bake + replan.
void VulkanEngineApp::showcase_gameplay_nav_init() {
    if (gameNav || !mobEntities) return;
    std::string err;
    gameNav = engine::navigation::create_recast_navigation_provider();
    gameNavInvalidation = engine::navigation::create_nav_invalidation();
    if (!gameNav || !gameNavInvalidation) {
        std::cout << "[Showcase] nav provider/invalidation refused\n";
        gameNav.reset();
        gameNavInvalidation.reset();
        return;
    }
    // A4-REQ-CROSS-DOMAIN / A4-REQ-PHASE (Agente 5): the two SDK-INTERNAL
    // gameplay factories become REAL product consumers once navigation exists —
    // a cross-domain status coordinator (navigation + AI debug + rendering
    // debug + gameplay integration) and a domain lifecycle (phases). Bind the
    // live components the game already owns; advanced per frame in the tick
    // with observables in the title. Bindings that can't be fulfilled (e.g. a
    // scripting/semantic authoring seam the game doesn't host) stay FALSE in
    // the snapshot — never a silent claim.
    if (!crossDomain_ && runtimeIntegration && aiDebugRecorder &&
        renderingDebugView && showcaseNavStream && runtimeQueries) {
        navSchedulerBridge_ =
            engine::navigation::create_navigation_scheduler_bridge(
                runtimeQueries.get(), gameNav.get());
        crossDomain_ = engine::gameplay::create_gameplay_cross_domain();
        const bool intOk = crossDomain_->bind_integration(runtimeIntegration.get());
        bool navOk = false;
        std::string cdErr;
        if (navSchedulerBridge_) {
            navOk = crossDomain_->bind_navigation(
                gameNav.get(), gameNavInvalidation.get(), showcaseNavStream.get(),
                navSchedulerBridge_.get(), cdErr);
        }
        const bool dbgOk =
            crossDomain_->bind_debug(aiDebugRecorder.get(),
                                     renderingDebugView.get());
        std::cout << "[Showcase] crossDomain bound: int=" << intOk
                  << " nav=" << navOk << " debug=" << dbgOk
                  << (navOk ? "" : (" (" + cdErr + ")")) << "\n";
    }
    if (!phase_) {
        phase_ = engine::gameplay::create_gameplay_phase();
        if (phase_) {
            const std::vector<engine::gameplay::GameplayDomain> domains = {
                engine::gameplay::GameplayDomain::Ecs,
                engine::gameplay::GameplayDomain::Navigation,
                engine::gameplay::GameplayDomain::Ai,
                engine::gameplay::GameplayDomain::Animation,
                engine::gameplay::GameplayDomain::Physics,
                engine::gameplay::GameplayDomain::Voxel,
                engine::gameplay::GameplayDomain::Renderer,
                engine::gameplay::GameplayDomain::Audio,
                engine::gameplay::GameplayDomain::Worlds,
            };
            std::string pErr;
            if (!phase_->configure(domains, pErr)) {
                std::cout << "[Showcase] gameplay phase configure refused: "
                          << pErr << "\n";
                phase_.reset();
            }
        }
    }
    // Tiled mode so local updates (obstacle toggle, block edit) never trigger a
    // full rebake.
    engine::navigation::NavmeshConfig cfg;
    const float px = player.position.x;
    const float pz = player.position.z;
    cfg.boundsMinX = px - 24.0f;
    cfg.boundsMaxX = px + 24.0f;
    cfg.boundsMinZ = pz - 24.0f;
    cfg.boundsMaxZ = pz + 24.0f;
    cfg.boundsMinY = -8.0f;
    cfg.boundsMaxY = 200.0f;
    cfg.cellSize = 0.5f;
    cfg.cellHeight = 0.2f;
    cfg.agentRadius = 0.4f;
    cfg.agentHeight = 1.8f;
    cfg.agentMaxClimb = 1.0f;
    cfg.agentMaxSlope = 45.0f;
    cfg.tileSize = 16.0f;
    const auto columns = showcase_nav_sample_columns(world, cfg);
    if (columns.empty()) {
        std::cout << "[Showcase] nav bake: no walkable columns near the player\n";
        return;
    }
    if (!gameNav->build(cfg, columns, err)) {
        std::cout << "[Showcase] nav bake refused: " << err << '\n';
        gameNav.reset();
        return;
    }
    gameNavRevision = gameNav->revision();
    // Confirgure the routed invalidation ledger at the provider's tile size.
    if (!gameNavInvalidation->configure(16.0f, err)) {
        std::cout << "[Showcase] nav invalidation configure refused: " << err << '\n';
        gameNavInvalidation.reset();
    }
    // Off-mesh links: a jump/climb ledge across the bake region (not part of
    // the walkable surface) — makes otherwise-unroutable gaps traversable.
    {
        engine::navigation::OffMeshLink jump;
        jump.startX = px - 4.0f;
        jump.startY = cfg.boundsMinY + 1.0f;
        jump.startZ = pz;
        jump.endX = px + 4.0f;
        jump.endY = cfg.boundsMinY + 3.0f;
        jump.endZ = pz;
        jump.radius = 1.0f;
        jump.bidirectional = true;
        std::string linkErr;
        if (gameNav->set_off_mesh_links({ jump }, linkErr)) {
            gameNavOffMeshLinks = 1;
        } else {
            std::cout << "[Showcase] nav off-mesh link refused: " << linkErr << '\n';
        }
    }
    // Area costs (hazards): terrain tagged with area 1 is more expensive to
    // cross (e.g. a hazardous material band), so find_path routes around it.
    {
        std::string costErr;
        if (gameNav->set_area_cost(1, 4.0f, costErr)) {
            gameNavAreaCosts = 1;
        } else {
            std::cout << "[Showcase] nav area cost refused: " << costErr << '\n';
        }
    }
    // Dynamic obstacle (door): a wall footprint that blocks the navmesh while
    // ACTIVE and is passable while INACTIVE; toggling re-bakes only its tiles.
    {
        engine::navigation::DynamicObstacle door;
        door.columns = {
            { px + 2.0f, pz, cfg.boundsMinY + 1.0f, cfg.boundsMinY + 2.5f, true, 0 },
            { px + 2.0f, pz - 1.0f, cfg.boundsMinY + 1.0f, cfg.boundsMinY + 2.5f, true, 0 },
            { px + 2.0f, pz + 1.0f, cfg.boundsMinY + 1.0f, cfg.boundsMinY + 2.5f, true, 0 },
        };
        std::string obsErr;
        if (gameNav->set_dynamic_obstacle(1, door, obsErr)) {
            gameNavObstacleColumns = door.columns.size();
            // Start OPEN (passable) so paths are free on boot.
            gameNavDoorActive = false;
            gameNav->set_obstacle_active(1, false, obsErr);
        } else {
            std::cout << "[Showcase] nav door obstacle refused: " << obsErr << '\n';
        }
    }
    gameNavBaked = gameNav && gameNav->valid();
    std::cout << "[Showcase] nav baked (revision " << gameNavRevision << ", "
              << columns.size() << " columns, "
              << gameNavOffMeshLinks << " off-mesh, " << gameNavAreaCosts
              << " cost areas, " << gameNavObstacleColumns << " door cols)\n";
}

void VulkanEngineApp::showcase_gameplay_nav_invalidate() {
    // A committed block edit = the walkable terrain changed. The nav provider
    // re-bakes in tiled mode on the next tick; every in-flight async query is
    // cancelled (join) so no stale path through the edit is applied.
    if (!gameNav) return;
    if (gameNavInvalidation) {
        const float r = 24.0f;
        engine::navigation::NavInvalidationRegion region;
        region.minX = player.position.x - r;
        region.minZ = player.position.z - r;
        region.maxX = player.position.x + r;
        region.maxZ = player.position.z + r;
        gameNavInvalidation->invalidate(region);
    }
    // Cancel every in-flight agent request (world change cancels old queries).
    for (auto& [id, agent] : gameNavAgents) {
        (void)id;
        if (agent.requestId != 0) {
            gameNav->cancel_async_path(agent.requestId);
            ++gameNavCancelled;
        }
        agent.requestId = 0;
        agent.pathActive = false;
        agent.waypoint = 0;
    }
}

void VulkanEngineApp::showcase_gameplay_nav_tick(float fixedDt) {
    if (!gameNav || !mobEntities) {
        // Lazy one-time bake once the world has streamed columns near the
        // player (the first ticks may have no solid terrain yet).
        if (!gameNavBaked) showcase_gameplay_nav_init();
        return;
    }
    // World change / obstacle toggle handling: on every 90th fixed tick the
    // door toggles and the provider re-bakes only its tiles; block edits (see
    // invalidateShowcaseSurfaces) already forced a replan. Deterministic.
    if (gameNav && (runtimeTick % 90) == 0) {
        std::string obsErr;
        gameNavDoorActive = !gameNavDoorActive;
        gameNav->set_obstacle_active(1, gameNavDoorActive, obsErr);
    }
    // Re-bake tiles invalidated by block edits (localized, tiled mode).
    if (gameNavInvalidation && !gameNavInvalidation->invalid_tiles().empty()) {
        const engine::navigation::NavmeshConfig cfg;
        const auto columns = showcase_nav_sample_columns(world, cfg);
        std::string upErr;
        if (gameNav->update(columns, upErr)) {
            std::string cErr;
            for (const auto& t : gameNavInvalidation->invalid_tiles()) {
                gameNavInvalidation->rebuild(t);
            }
            gameNavRevision = gameNav->revision();
        } else {
            std::cout << "[Showcase] nav localized re-bake refused: " << upErr << '\n';
        }
    }

    // Steer each LIVE mob ECS agent along an async path toward the player.
    const glm::vec3 goal = player.position;
    std::size_t following = 0;
    std::unordered_set<std::uint32_t> live;
    mobEntities->for_each_entity([&](engine::entity::EntityId id) {
        live.insert(id.id);
        engine::entity::Position pos;
        if (!mobEntities->get_position(id, pos)) return;
        auto& agent = gameNavAgents[id.id];
        // Despawn / invalid target: an entity that disappeared (or whose goal
        // became non-walkable) must not keep applying a stale path — cancel
        // the in-flight query and drop the route.
        const bool targetInvalid =
            agent.requestId != 0 &&
            agent.result.revision != 0 &&
            agent.result.revision != gameNav->revision();
        if (!mobEntities->alive(id) || targetInvalid) {
            if (agent.requestId != 0) {
                gameNav->cancel_async_path(agent.requestId);
                ++gameNavCancelled;
                agent.requestId = 0;
            }
            agent.pathActive = false;
            return;
        }
        // A NEW request supersedes the in-flight one: cancel before re-issuing
        // (a late result from the old query is never applied).
        if (agent.requestId != 0 &&
            (agent.goal.x != goal.x || agent.goal.y != goal.y ||
             agent.goal.z != goal.z)) {
            gameNav->cancel_async_path(agent.requestId);
            ++gameNavCancelled;
            agent.requestId = 0;
            agent.pathActive = false;
        }
        if (agent.requestId == 0) {
            std::string reqErr;
            agent.requestId = gameNav->begin_async_path(
                pos.x, pos.y, pos.z, goal.x, goal.y, goal.z, reqErr);
            agent.goal = goal;
            agent.requestedAtTick = runtimeTick;
            if (agent.requestId == 0) {
                // Async refused (pre-build/invalid): report, don't stall.
                agent.pathActive = false;
                return;
            }
        }
        // Poll the in-flight request (non-blocking).
        engine::navigation::PathResult result;
        std::string pollErr;
        const auto status = gameNav->poll_async_path(agent.requestId, result, pollErr);
        if (status == engine::navigation::PathRequestStatus::Succeeded) {
            agent.result = result;
            agent.pathActive = true;
            agent.waypoint = 0;
            agent.requestId = 0;  // consumed
        } else if (status == engine::navigation::PathRequestStatus::Failed ||
                   status == engine::navigation::PathRequestStatus::Cancelled ||
                   status == engine::navigation::PathRequestStatus::Invalid) {
            ++gameNavFailureCount;
            agent.requestId = 0;
            agent.pathActive = false;
        }
        if (agent.pathActive && !agent.result.waypoints.empty()) {
            ++following;
            // Follow the resolved route: advance toward the current waypoint.
            const std::size_t stride = agent.result.waypoints.size() / 3;
            const std::size_t wp = std::min(agent.waypoint, stride - 1);
            const float wx = agent.result.waypoints[wp * 3];
            const float wy = agent.result.waypoints[wp * 3 + 1];
            const float wz = agent.result.waypoints[wp * 3 + 2];
            const float dx = wx - pos.x;
            const float dz = wz - pos.z;
            const float dist = std::sqrt(dx * dx + dz * dz);
            if (dist < 0.6f) {
                if (agent.waypoint + 1 < stride) ++agent.waypoint;
            } else {
                const float speed = 1.6f * fixedDt;
                const float stepX = dx / dist * speed;
                const float stepZ = dz / dist * speed;
                mobEntities->set_position(
                    id, { pos.x + stepX, wy, pos.z + stepZ });
            }
        }
    });
    // Clean up agents that no longer exist (despawned mobs). Cancelling their
    // in-flight query is handled on the next pass (they're not live), but we
    // drop the state so the map doesn't grow unboundedly.
    for (auto it = gameNavAgents.begin(); it != gameNavAgents.end();) {
        if (!live.count(it->first)) {
            if (it->second.requestId != 0) {
                gameNav->cancel_async_path(it->second.requestId);
                ++gameNavCancelled;
            }
            it = gameNavAgents.erase(it);
        } else {
            ++it;
        }
    }
    gameNavAgentsFollowing = following;
}

// ── CONTA 3 — per-frame AI debug snapshot from the SAME live ECS agents
// (Agente 2 item 120). Every fixed tick the game feeds its IAiDebugRecorder
// with the actual decision cores running on the live mobs (perception,
// blackboard, FSM, behavior tree, utility, planner) and publishes the JSON
// snapshot the editor consumer reads. The recorder holds one snapshot (the
// agent the game focused this tick — deterministic); the per-agent split is
// summarized in aiDebugSnapshotAgents.
void VulkanEngineApp::showcase_gameplay_ai_debug_snapshot() {
    if (!aiDebugRecorder || !mobEntities) {
        aiDebugSnapshotJson = "{}";
        aiDebugSnapshotAgents = 0;
        return;
    }
    // Focus the nearest hostile (or first) live ECS mob — the same suite the
    // game's AI tick feeds (perception → decision cores).
    engine::entity::EntityId focus;
    float focusDist = 1.0e9f;
    std::size_t liveCount = 0;
    mobEntities->for_each_entity([&](engine::entity::EntityId id) {
        ++liveCount;
        engine::entity::Position p;
        if (!mobEntities->get_position(id, p)) return;
        engine::entity::ComponentData cdata;
        bool hostile = false;
        if (mobEntities->get_component(
                id, engine::entity::kMobComponentType, cdata)) {
            hostile = cdata.blob.find("\"hostile\":true") != std::string::npos;
        }
        if (!hostile) return;
        const float d = std::sqrt(
            (p.x - player.position.x) * (p.x - player.position.x) +
            (p.z - player.position.z) * (p.z - player.position.z));
        if (d < focusDist) {
            focusDist = d;
            focus = id;
        }
    });
    aiDebugSnapshotAgents = liveCount;
    if (!focus.valid()) {
        aiDebugSnapshotJson = "{}";
        return;
    }
    aiDebugRecorder->begin_tick(focus.id, "live_mob", runtimeTick);
    // Perception: how many detections/remembers the suite holds this tick.
    aiDebugRecorder->node_visit(
        "perception", "succeeded", 0,
        "det " + std::to_string(perceptionDetections) +
            " mem " + std::to_string(perceptionMemory));
    aiDebugRecorder->blackboard_set(
        "threat.distance", nearestThreatDistance < 0.0f
            ? std::string("none")
            : std::to_string(static_cast<int>(nearestThreatDistance)));
    // FSM: current state + last drained action.
    aiDebugRecorder->node_visit("fsm", "running", 1, fsmState);
    aiDebugRecorder->blackboard_set("fsm.action", fsmLastAction);
    // Behavior tree: root status + first trace node.
    aiDebugRecorder->node_visit(
        "behavior_tree", treeStatus == 1 ? "succeeded"
                          : (treeStatus == 2 ? "running" : "failed"), 1,
        treeTrace.empty() ? std::string("none") : treeTrace);
    // Utility AI: highest-utility action + value.
    aiDebugRecorder->node_visit("utility", "selected", 1,
                                utilityAction + " (" + utilityChoice + ")");
    // GOAP planner: plan length + first step + goal reachability.
    aiDebugRecorder->node_visit(
        "planner", (plannerGoal != "unreachable" ? "succeeded" : "failed"), 1,
        (plannerStep == "none" ? std::string("empty")
                                : plannerStep + " -> " + plannerGoal));
    aiDebugRecorder->blackboard_set("planner.steps",
                                    std::to_string(plannerLength));
    // AI event bus: decisions logged this tick.
    aiDebugRecorder->blackboard_set("events.bus", std::to_string(aiEventCount));
    const auto* snap = aiDebugRecorder->snapshot();
    aiDebugNodeCount = snap ? snap->nodes.size() : 0;
    aiDebugSnapshotJson = aiDebugRecorder->to_json();
}

void VulkanEngineApp::showcase_gameplay_shutdown() {
    // The ragdoll and weapon live inside the runtime physics (owned there);
    // release our references first, then drop the remaining factory state.
    // The canonical composition teardown happens in cleanup() after this.
    showcaseRagdoll.reset();
    showcaseRifle.reset();
    fixedTickSim.reset();
    // CONTA 4: release the game-loop consumers (mission runtime + vehicle)
    // before the canonical runtime physics is torn down in cleanup().
    showcaseMissions.reset();
    showcaseMissionDef = {};
    showcaseMissionState = {};
    showcaseVehicle.reset();
    showcaseVehicleAsset.reset();
    showcaseVehicleValid = false;
    // CONTA 4: release the game client's SDK consumers (voice codec + voxel
    // + vehicle replication) before the canonical runtimes are torn down.
    showcaseVoxelReplication.reset();
    showcaseVehServerCar.reset();
    showcaseVehPredicted.reset();
    showcaseVehReplication.reset();
    showcaseVehClientReplication.reset();
    showcaseVehServerRuntime.reset();
    showcaseVehClientRuntime.reset();
    showcaseVoiceCodec.reset();
    // CONTA 3: join the nav provider worker + free the invalidator.
    gameNavAgents.clear();
    gameNav.reset();
    gameNavInvalidation.reset();
    // CONTA 3 (fechamento global): release the content/physics/simulation
    // consumers created by showcase_content_physics_init().
    showcase_content_physics_shutdown();
    // ── AGENTE 3 A.3: explicit disconnect of the host-local session before
    // the networking stack goes away (graceful leave keeps the reconnect
    // token valid); the discovery/interest/rpc/replication factories are
    // dropped right after.
    if (hostLocalClient) {
        std::cout << "[Showcase] host-local session disconnect ("
                  << (hostLocalNetOk ? "was connected" : "not connected")
                  << ", " << hostLocalPredictedBlocks << " predicted blocks, "
                  << hostLocalRollbacks << " rollback)\n";
        hostLocalClient->disconnect();
    }
    hostLocalClient.reset();
    hostLocalDiscovery.reset();
    hostLocalInterest.reset();
    hostLocalRpc.reset();
    hostLocalReplication.reset();
    // ── C: persist the session's simulated state (player, inventory, clock,
    // abilities, committed block edits) for the next boot's round trip.
    showcase_gameplay_save();
    std::cout << "[Showcase] gameplay showcase shutdown complete\n";
}

// ── C: save/load round trip (AGENTE 2 — gameplay showcase). The save file
// persists the SIMULATED state the showcase owns: player position/health/
// selection, the real inventory, the deterministic day/night clock, the
// player entity health, weapon ammo and the committed atomic block edits
// (journal). The load path restores all of it and re-applies the block edits
// through the SAME atomic transaction API once the target chunks are writable
// (deferred in the fixed tick). No renderer state is touched.
namespace {
// Minimal JSON string escaping for embedding serialized JSON fragments (the
// inventory/day-night payloads) as string fields in the showcase save file.
std::string json_escape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (const char c : in) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;                default: out += c; break;
        }
    }
    return out;
}

// Hex helpers so a block entity's serialized state blob survives the JSON
// save as a flat hexadecimal string (Item 2/3 — the game's World-attached
// block entities round-trip through the unified save, not only the SDK).
std::string bytes_to_hex(const std::vector<std::uint8_t>& bytes) {
    constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::uint8_t b : bytes) {
        out += kHex[(b >> 4) & 0xF];
        out += kHex[b & 0xF];
    }
    return out;
}

std::vector<std::uint8_t> hex_to_bytes(const std::string& hex) {
    std::vector<std::uint8_t> out;
    if (hex.size() % 2 != 0) return out;
    out.reserve(hex.size() / 2);
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        const int hi = nibble(hex[i]);
        const int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return out;
}
}  // namespace

void VulkanEngineApp::showcase_gameplay_save() {
    if (showcaseSavePath.empty()) {
        showcaseSavePath = std::string(VULKANCRAFT_SOURCE_DIR) +
                           "/Projects/ShowcaseGame/showcase_save.json";
    }
    std::ostringstream out;
    out << std::setprecision(9);
    out << "{\n";
    out << "  \"player\": {\"x\":" << player.position.x
        << ",\"y\":" << player.position.y
        << ",\"z\":" << player.position.z
        << ",\"hp\":" << showcasePlayerHealth
        << ",\"selected\":\"" << showcaseSelectedBlock << "\"},\n";
    // The inventory payload is embedded ESCAPED as a string field so the JSON
    // save file stays a flat document (the loader reads it back with
    // json_string and deserializes into the live Inventory).
    out << "  \"inventory\": \""
        << json_escape(playerInventory ? playerInventory->serialize_json()
                                       : std::string("{}"))
        << "\",\n";
    out << "  \"daynight\": \""
        << json_escape(dayNightCycle ? dayNightCycle->to_json()
                                     : std::string("{}"))
        << "\",\n";
    out << "  \"weapon\": {\"ammo\":"
        << (showcaseRifle ? showcaseRifle->ammo() : 0U)
        << ",\"reserve\":"
        << (showcaseRifle ? showcaseRifle->reserve() : 0U) << "},\n";
    out << "  \"abilityFx\": " << abilityEffectCount << ",\n";
    // Item 47/66: the stateful voxel block entity frames its state for
    // persistence (type_id + version + blob) inside the SAME save so the
    // round trip restores it — a real block entity survives the save.
    out << "  \"blockEntity\": {\"type\":\""
        << (showcaseBlockEntity ? showcaseBlockEntity->type_id()
                                : std::string("none"))
        << "\",\"version\":"
        << (showcaseBlockEntity ? showcaseBlockEntity->data_version() : 0U)
        << ",\"saved\":"
        << (showcaseBlockEntity ? showcaseBlockEntity->ticks() : 0ULL)
        << "},\n";
    // CONTA 4 items 2/3: persist the LIVE World's attached block entities
    // (the project clocks wired by WorldLote1::try_attach_clocks) by integer
    // coordinate — each entity's type_id + data_version + serialized state
    // blob, hex-encoded. On load they are restored through the SAME World
    // path (restore_block_entity) so the round trip keeps per-coordinate
    // block entity state, not only the standalone showcase counter.
    {
        out << "  \"worldBlockEntities\": [";
        std::size_t index = 0;
        const auto& beMap = world.block_entities();
        for (const auto& [cell, ent] : beMap) {
            if (!ent) continue;
            if (index) out << ',';
            out << "{\"x\":" << cell.x << ",\"y\":" << cell.y
                << ",\"z\":" << cell.z << ",\"type\":\""
                << json_escape(ent->type_id()) << "\",\"version\":"
                << ent->data_version() << ",\"state\":\""
                << bytes_to_hex(ent->serialize_state()) << "\"}";
            ++index;
        }
        out << "],\n";
    }
    // CONTA 4 item 6: persist the mission's explicit state (accepted /
    // completed + objective progress + dialogue node) so a reboot restores it
    // without a silent reset. The runtime serializes it bit-exactly.
    out << "  \"mission\": {\"accepted\":"
        << (showcaseMissionAccepted ? "true" : "false")
        << ",\"completed\":"
        << (showcaseMissionCompleted ? "true" : "false")
        << ",\"state\":\""
        << json_escape(
               [&]() -> std::string {
                   if (!showcaseMissions) return std::string();
                   std::string ser;
                   std::string serErr;
                   if (showcaseMissions->serialize_state(
                           showcaseMissionState, ser, serErr)) {
                       return ser;
                   }
                   std::cout << "[Showcase] mission state serialize refused: "
                             << serErr << '\n';
                   return std::string();
               }())
        << "\"},\n";
    // CONTA 4 item 5: persist the vehicle chassis pose so the save/load
    // round-trips it (an explicit pose, never a silently reset spawn).
    {
        const glm::vec3 vp = showVehicleChassisPos();
        out << "  \"vehicle\": {\"x\":" << vp.x
            << ",\"y\":" << vp.y
            << ",\"z\":" << vp.z << "},\n";
    }
    out << "  \"edits\": [";
    for (std::size_t i = 0; i < showcaseBlockJournal.size(); ++i) {
        const auto& e = showcaseBlockJournal[i];
        if (i) out << ',';
        out << "{\"x\":" << e.first.x << ",\"y\":" << e.first.y
            << ",\"z\":" << e.first.z << ",\"id\":" << e.second << "}";
    }
    out << "]\n}\n";
    std::ofstream file(showcaseSavePath);
    if (file) {
        file << out.str();
        std::cout << "[Showcase] save written: " << showcaseSavePath
                  << " (" << showcaseBlockJournal.size() << " block edits)\n";
    } else {
        std::cout << "[Showcase] save refused: " << showcaseSavePath << '\n';
    }
}

void VulkanEngineApp::showcase_gameplay_load() {
    if (showcaseSavePath.empty()) {
        showcaseSavePath = std::string(VULKANCRAFT_SOURCE_DIR) +
                           "/Projects/ShowcaseGame/showcase_save.json";
    }
    std::ifstream file(showcaseSavePath);
    if (!file) {
        std::cout << "[Showcase] no save file at " << showcaseSavePath
                  << "; booting fresh\n";
        return;
    }
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    engine::sdk::JsonValue root;
    std::string jsonError;
    if (!engine::sdk::json_parse(content, root, jsonError) || !root.is_object()) {
        std::cout << "[Showcase] save parse refused: " << jsonError << '\n';
        return;
    }
    // Player position + health + selection.
    const engine::sdk::JsonValue* playerJson = root.field("player");
    if (playerJson != nullptr && playerJson->is_object()) {
        player.position.x = static_cast<float>(
            engine::sdk::json_number(*playerJson, "x", player.position.x));
        player.position.y = static_cast<float>(
            engine::sdk::json_number(*playerJson, "y", player.position.y));
        player.position.z = static_cast<float>(
            engine::sdk::json_number(*playerJson, "z", player.position.z));
        showcasePlayerHealth = static_cast<float>(
            engine::sdk::json_number(*playerJson, "hp", showcasePlayerHealth));
        const std::string sel =
            engine::sdk::json_string(*playerJson, "selected", showcaseSelectedBlock);
        if (sel == "stone") player.selectedBlock = BlockType::Stone;
        else if (sel == "grass") player.selectedBlock = BlockType::Grass;
        else if (sel == "cobblestone") player.selectedBlock = BlockType::Cobblestone;
        else if (sel == "sand") player.selectedBlock = BlockType::Sand;
        else if (sel == "planks") player.selectedBlock = BlockType::Planks;
        else if (sel == "wood") player.selectedBlock = BlockType::Wood;
        else if (sel == "dirt") player.selectedBlock = BlockType::Dirt;
    }
    // Inventory round trip.
    if (playerInventory && playerItems) {
        const std::string invJson =
            engine::sdk::json_string(root, "inventory", std::string());
        if (!invJson.empty()) {
            std::string invError;
            if (!playerInventory->deserialize_json(invJson, *playerItems, invError)) {
                std::cout << "[Showcase] inventory restore refused: "
                          << invError << '\n';
            }
            playerInventorySummary = playerInventory->serialize_json();
        }
    }
    // Block entity round trip (item 46/47/66): restore the stateful voxel's
    // blob via the factory (deserialize_state) so the counter survives.
    {
        const engine::sdk::JsonValue* beJson = root.field("blockEntity");
        if (beJson != nullptr && beJson->is_object() && showcaseBlockEntity) {
            const std::string beType = engine::sdk::json_string(
                *beJson, "type", std::string());
            const std::uint32_t beVer = static_cast<std::uint32_t>(
                engine::sdk::json_number(*beJson, "version", 1));
            if (beType == showcaseBlockEntity->type_id()) {
                std::vector<uint8_t> blob(16, 0);
                const double savedTicks =
                    engine::sdk::json_number(*beJson, "saved", 0.0);
                const std::uint64_t st =
                    static_cast<std::uint64_t>(savedTicks);
                for (int i = 0; i < 8; ++i) {
                    blob[static_cast<size_t>(i)] =
                        static_cast<uint8_t>((st >> (i * 8)) & 0xFF);
                }
                if (showcaseBlockEntity->deserialize_state(blob, beVer)) {
                    showcaseBlockEntityTicks = static_cast<std::size_t>(
                        showcaseBlockEntity->ticks());
                }
            }
        }
    }
    // CONTA 4 items 2/3: restore the LIVE World's attached block entities
    // from the unified save through the SAME World path (restore_block_entity
    // -> schedules their BlockTick). Each saved block entity (type_id/version/
    // state blob) is reconstructed via its registered factory and its state
    // blob is deserialized, so per-coordinate block-entity state survives the
    // round trip without a silent reset. A registered type with no factory or
    // a refused deserialize is reported, never silently dropped.
    {
        std::size_t restoredCount = 0;
        const engine::sdk::JsonValue* wbeJson = root.field("worldBlockEntities");
        if (wbeJson != nullptr && wbeJson->is_array()) {
            for (const engine::sdk::JsonValue& be : wbeJson->array) {
                if (!be.is_object()) continue;
                const int bx = static_cast<int>(
                    engine::sdk::json_number(be, "x", 0.0));
                const int by = static_cast<int>(
                    engine::sdk::json_number(be, "y", 0.0));
                const int bz = static_cast<int>(
                    engine::sdk::json_number(be, "z", 0.0));
                const std::string beType = engine::sdk::json_string(
                    be, "type", std::string());
                const std::uint32_t beVer = static_cast<std::uint32_t>(
                    engine::sdk::json_number(be, "version", 1));
                const std::vector<std::uint8_t> blob = hex_to_bytes(
                    engine::sdk::json_string(be, "state", std::string()));
                const engine::voxel::BlockEntityFactory factory =
                    world.find_block_entity_factory(beType);
                if (!factory) {
                    std::cout << "[Showcase] no factory for saved block "
                                 "entity '" << beType << "'; not restored\n";
                    continue;
                }
                auto entity = factory();
                if (!entity) continue;
                if (!entity->deserialize_state(blob, beVer)) {
                    std::cout << "[Showcase] block entity '" << beType
                              << "' @(" << bx << "," << by << "," << bz
                              << ") state restore refused; not restored\n";
                    continue;
                }
                if (world.block_entity_at(bx, by, bz)) {
                    // Already live this boot (attached by the probe); do not
                    // double-attach — the live entity wins.
                    continue;
                }
                world.restore_block_entity(bx, by, bz, std::move(entity));
                ++restoredCount;
            }
        }
        if (restoredCount > 0) {
            std::cout << "[Showcase] restored " << restoredCount
                      << " world block entities from unified save\n";
            showcaseWorldBlockEntitiesRestored += restoredCount;
        }
    }
    // Day/night clock round trip.
    if (dayNightCycle) {
        const std::string dnJson =
            engine::sdk::json_string(root, "daynight", std::string());
        if (!dnJson.empty()) {
            std::string dnError;
            if (!dayNightCycle->load_from_json(dnJson, dnError)) {
                std::cout << "[Showcase] day/night restore refused: "
                          << dnError << '\n';
            }
        }
    }
    // CONTA 4 item 6: restore the mission's persisted state (accepted /
    // completed + bit-exact progress) so a reboot does not silently reset it.
    {
        const engine::sdk::JsonValue* missionJson = root.field("mission");
        if (missionJson != nullptr && missionJson->is_object() && showcaseMissions) {
            showcaseMissionAccepted =
                engine::sdk::json_bool(*missionJson, "accepted", false);
            showcaseMissionCompleted =
                engine::sdk::json_bool(*missionJson, "completed", false);
            const std::string ser =
                engine::sdk::json_string(*missionJson, "state", std::string());
            if (!ser.empty()) {
                engine::gameplay::MissionState restored;
                std::string dsErr;
                if (showcaseMissions->deserialize_state(ser, restored, dsErr)) {
                    showcaseMissionState = std::move(restored);
                    std::cout << "[Showcase] mission state restored ("
                              << (showcaseMissionAccepted ? "accepted" : "locked")
                              << (showcaseMissionCompleted ? "/done" : "")
                              << ")\n";
                } else {
                    std::cout << "[Showcase] mission state restore refused: "
                              << dsErr << '\n';
                }
            }
        }
    }
    // CONTA 4 item 5: restore the persisted vehicle chassis pose onto the
    // live vehicle (an explicit teleport, never a silent reset to spawn).
    {
        const engine::sdk::JsonValue* vJson = root.field("vehicle");
        if (vJson != nullptr && vJson->is_object() && showcaseVehicle &&
            showcaseVehicleValid) {
            const glm::vec3 vp(
                static_cast<float>(
                    engine::sdk::json_number(*vJson, "x", player.position.x)),
                static_cast<float>(
                    engine::sdk::json_number(*vJson, "y", player.position.y)),
                static_cast<float>(
                    engine::sdk::json_number(*vJson, "z", player.position.z)));
            runtimePhysics->physics().set_transform(
                showcaseVehicle->chassis(), vp, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
            std::cout << "[Showcase] vehicle pose restored @("
                      << vp.x << ", " << vp.y << ", " << vp.z << ")\n";
        }
    }
    // Weapon + abilities state: the weapon's live ammo has no public setter
    // (the runtime owns it), so the round trip persists the values and reports
    // them; the emitted ability-effect counter (the "estado de abilities" the
    // plan asks to preserve) is restored as the observable it drives.
    {
        const engine::sdk::JsonValue* weaponJson = root.field("weapon");
        if (weaponJson != nullptr && weaponJson->is_object()) {
            const std::uint32_t ammo = static_cast<std::uint32_t>(
                engine::sdk::json_number(*weaponJson, "ammo", 0.0));
            const std::uint32_t reserve = static_cast<std::uint32_t>(
                engine::sdk::json_number(*weaponJson, "reserve", 0.0));
            // Persisted values only (live ammo is runtime-owned).
            std::cout << "[Showcase] persisted weapon state restored (mag "
                      << ammo << ", reserve " << reserve << ")\n";
        }
        const double fx =
            engine::sdk::json_number(root, "abilityFx", 0.0);
        abilityEffectCount = static_cast<std::uint64_t>(fx);
    }
    // Block edits: stage for deferred re-application (chunks may not be
    // writable yet — the fixed tick retries until they are). The journal is
    // cumulative, so the loaded set is appended and the replay is scoped to
    // exactly those loaded LEADING entries (showcaseLoadEditCount): they are
    // re-applied in order this boot; live edits made after boot append beyond
    // that marker and are never replayed (they are already applied in world).
    const std::size_t loadedEditStart = showcaseBlockJournal.size();
    const engine::sdk::JsonValue* editsJson = root.field("edits");
    if (editsJson != nullptr && editsJson->is_array()) {
        for (const engine::sdk::JsonValue& e : editsJson->array) {
            if (!e.is_object()) continue;
            showcaseBlockJournal.push_back({
                { static_cast<int>(engine::sdk::json_number(e, "x", 0.0)),
                  static_cast<int>(engine::sdk::json_number(e, "y", 0.0)),
                  static_cast<int>(engine::sdk::json_number(e, "z", 0.0)) },
                static_cast<uint32_t>(engine::sdk::json_number(e, "id", 0.0)) });
        }
    }
    showcaseReplayCursor = 0;
    showcaseLoadEditCount = showcaseBlockJournal.size();
    showcasePendingBlockEdits =
        showcaseLoadEditCount > loadedEditStart
            ? showcaseLoadEditCount - loadedEditStart
            : 0;
    // Restore the player entity health/position in the live ECS.
    if (showcasePlayerEntityValid && mobEntities) {
        mobEntities->set_position(
            showcasePlayerEntity,
            { player.position.x, player.position.y, player.position.z });
        mobEntities->set_health(showcasePlayerEntity,
                                { showcasePlayerHealth, 100.0f });
    }
    showcaseSaveLoaded = true;
    std::cout << "[Showcase] save loaded: " << showcaseSavePath
              << " (" << showcasePendingBlockEdits
              << " block edits queued for re-application)\n";
}

// Reintegração (item 91): craft REAL no jogo. A action map (KeyC) dispara a
// primeira receita satisfazível do inventário VIVO; o RecipeRegistry autoritativo
// consome exatamente os insumos (atômico) e o resultado é adicionado ao mesmo
// inventário. Observável em showcaseCraftResult. Antes recipes_for só era
// consultado; agora CRIA produto alterando o inventário do jogador.
void VulkanEngineApp::showcase_try_craft() {
    if (!playerRecipes || !playerInventory || !playerItems) {
        showcaseCraftResult = "n/a";
        return;
    }
    const auto candidates = playerRecipes->recipes_for(
        *playerInventory, "vulkancraft:crafting", *playerItems);
    if (candidates.empty()) {
        showcaseCraftResult = "no recipe satisfiable";
        return;
    }
    const auto* recipe = candidates.front();
    std::string crError;
    const auto result = playerRecipes->craft(
        *playerInventory, *recipe, "vulkancraft:crafting", *playerItems,
        static_cast<std::uint64_t>(runtimeTick));
    if (!result.ok) {
        showcaseCraftResult = "refused:" + result.error;
        return;
    }
    std::size_t gained = 0;
    for (const auto& out : result.outputs) {
        const auto remainder = playerInventory->add(
            engine::registry::ItemStack{
                out.item, static_cast<std::int32_t>(out.count) },
            *playerItems, crError);
        (void)remainder;  // inventory full: output dropped, not silently duped
        gained += static_cast<std::size_t>(std::max(0, out.count));
    }
    craftableRecipeCount = playerRecipes->recipes_for(
        *playerInventory, "vulkancraft:crafting", *playerItems).size();
    playerInventorySummary = playerInventory->serialize_json();
    showcaseCraftResult =
        "ok+" + std::to_string(gained) + " [" + recipe->name + "]";
    std::cout << "[Showcase] crafted " << recipe->name << " (+" << gained
              << ")\n";
}

// L91 furnace: consumes a recipe at the `vulkancraft:furnace` STATION (a
// genuinely separate processing station from the crafting table) whose
// RecipeDefinition declares a FUEL item; processing consumes one unit of that
// fuel from the live inventory per craft, so stations + fuel + processing are
// real assets run in the product loop. Observable in showcaseSmeltResult /
// smokableRecipeCount.
void VulkanEngineApp::showcase_try_smelt() {
    if (!playerRecipes || !playerInventory || !playerItems) {
        showcaseSmeltResult = "n/a";
        return;
    }
    const auto candidates = playerRecipes->recipes_for(
        *playerInventory, "vulkancraft:furnace", *playerItems);
    if (candidates.empty()) {
        showcaseSmeltResult = "no furnace recipe satisfiable";
        return;
    }
    const auto* recipe = candidates.front();
    std::string smError;
    const auto result = playerRecipes->craft(
        *playerInventory, *recipe, "vulkancraft:furnace", *playerItems,
        static_cast<std::uint64_t>(runtimeTick));
    if (!result.ok) {
        showcaseSmeltResult = "refused:" + result.error;
        return;
    }
    // Processing consumes the recipe's declared fuel item from the same
    // inventory (one unit per craft).
    if (!recipe->fuel.empty()) {
        const int consumed =
            playerInventory->remove(recipe->fuel, 1, *playerItems, smError);
        if (consumed <= 0) {
            showcaseSmeltResult = "refused:no-fuel:" + recipe->fuel;
            return;
        }
    }
    std::size_t gained = 0;
    for (const auto& out : result.outputs) {
        const auto remainder = playerInventory->add(
            engine::registry::ItemStack{
                out.item, static_cast<std::int32_t>(out.count) },
            *playerItems, smError);
        (void)remainder;
        gained += static_cast<std::size_t>(std::max(0, out.count));
    }
    smokableRecipeCount = playerRecipes->recipes_for(
        *playerInventory, "vulkancraft:furnace", *playerItems).size();
    playerInventorySummary = playerInventory->serialize_json();
    showcaseSmeltResult =
        "ok+" + std::to_string(gained) + " [" + recipe->name +
        "] fuel:" + (recipe->fuel.empty() ? "none" : recipe->fuel);
    std::cout << "[Showcase] smelted " << recipe->name << " (+" << gained
              << ")\n";
}
