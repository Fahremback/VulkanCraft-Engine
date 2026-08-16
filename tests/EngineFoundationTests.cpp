#include "AssetRegistry.hpp"
#include "Scene.hpp"
#include "Serializer.hpp"
#include "UndoSystem.hpp"
#include "Prefab.hpp"
#include "GltfAssets.hpp"
#include "GltfGeometry.hpp"
#include "ExrDecoder.hpp"
#include "FbxImporter.hpp"
#include "ThumbnailCache.hpp"
#include "PluginRuntime.hpp"
#include "tinyexr.h"
#include "../src/engine/editor/play_mode/PlayMode.hpp"
#include "../src/engine/rendering/materials/Material.hpp"
#include "../src/engine/audio/AudioEvent.hpp"
#include "../src/engine/physics/Physics.hpp"
#include "../src/engine/scripting/VisualScriptGraph.hpp"
#include "../src/engine/scripting/ScriptRuntime.hpp"
#include <process.h>
#include "../src/engine/core/plugin/Plugin.hpp"
#include "../src/tools/AssetCooker.hpp"

#include <array>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>

namespace {
bool near(float a, float b) {
    return std::abs(a - b) < 0.0001f;
}

void append_f32(std::vector<uint8_t>& out, float v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    out.insert(out.end(), p, p + 4);
}

std::string base64_encode(const std::vector<uint8_t>& data) {
    static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < data.size(); i += 3) {
        const uint32_t a = data[i];
        const uint32_t b = i + 1 < data.size() ? data[i + 1] : 0;
        const uint32_t c = i + 2 < data.size() ? data[i + 2] : 0;
        const uint32_t triple = (a << 16) | (b << 8) | c;
        out.push_back(alphabet[(triple >> 18) & 0x3F]);
        out.push_back(alphabet[(triple >> 12) & 0x3F]);
        out.push_back(i + 1 < data.size() ? alphabet[(triple >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < data.size() ? alphabet[triple & 0x3F] : '=');
    }
    return out;
}
}

int main() {
    using namespace Engine;

    Scene reflectionProbe("Reflection Probe");
    if (!TypeRegistry::get().find_class("TransformComponent") ||
        !TypeRegistry::get().find_class("LightComponent") ||
        !TypeRegistry::get().find_class("CameraComponent") ||
        !TypeRegistry::get().find_class("RigidbodyComponent")) return EXIT_FAILURE;

    const std::filesystem::path temporary =
        std::filesystem::temp_directory_path() / ("vulkan_engine_test_" + UUID().to_string());
    std::filesystem::create_directories(temporary);

    Scene source("Roundtrip \"Scene\"");
    const Entity entity = source.create_entity("Key Light");
    source.transformComponents.at(entity.get_id()).position = {12.5f, -4.0f, 99.25f};
    source.lightComponents[entity.get_id()] = {
        {0.25f, 0.5f, 0.75f}, 4200.0f, 80.0f, false};
    const std::filesystem::path sceneFile = temporary / "roundtrip.scene";
    if (!Serializer::serialize_scene(source, sceneFile)) return EXIT_FAILURE;

    Scene loaded;
    if (!Serializer::deserialize_scene(loaded, sceneFile)) return EXIT_FAILURE;

    Scene playCopy = loaded.clone_for_play();
    if (playCopy.get_name() != loaded.get_name() + " [PLAY]") return EXIT_FAILURE;
    if (playCopy.get_entities().size() != loaded.get_entities().size()) return EXIT_FAILURE;
    if (playCopy.find_entity_by_id(entity.get_id()).get_name() != entity.get_name()) return EXIT_FAILURE;
    playCopy.transformComponents.at(entity.get_id()).position.x = -100.0f;
    if (!near(loaded.transformComponents.at(entity.get_id()).position.x, 12.5f)) return EXIT_FAILURE;
    if (loaded.get_id() != source.get_id() || loaded.get_name() != source.get_name()) return EXIT_FAILURE;
    const Entity loadedEntity = loaded.find_entity_by_id(entity.get_id());
    if (!loadedEntity.is_valid() || loadedEntity.get_scene() != &loaded ||
        loadedEntity.get_name() != entity.get_name()) return EXIT_FAILURE;
    const auto& transform = loaded.transformComponents.at(entity.get_id());
    if (!near(transform.position.x, 12.5f) || !near(transform.position.z, 99.25f)) return EXIT_FAILURE;
    const auto& light = loaded.lightComponents.at(entity.get_id());
    if (!near(light.intensity, 4200.0f) || light.castShadows) return EXIT_FAILURE;

    Prefab lightPrefab;
    lightPrefab.capture(loaded, entity.get_id());
    Scene prefabScene("Prefab Instances");
    const Entity instance = lightPrefab.instantiate(&prefabScene, "Light Instance");
    if (!instance.is_valid() || instance.get_name() != "Light Instance") return EXIT_FAILURE;
    if (!prefabScene.transformComponents.contains(instance.get_id())) return EXIT_FAILURE;
    if (!near(prefabScene.transformComponents.at(instance.get_id()).position.x, 12.5f)) return EXIT_FAILURE;
    lightPrefab.set_override({"LightComponent", "intensity", 9000.0f});
    const Entity overridden = lightPrefab.instantiate(&prefabScene, "Overridden Light");
    if (!near(prefabScene.lightComponents.at(overridden.get_id()).intensity, 9000.0f)) return EXIT_FAILURE;

    // Multi-entity prefabs retain stable local identities and remap hierarchy per instance.
    Scene vehicleSource("Vehicle Authoring");
    const Entity chassis = vehicleSource.create_entity("Chassis");
    const Entity wheel = vehicleSource.create_entity("Wheel");
    const Entity lamp = vehicleSource.create_entity("Lamp");
    const Entity unrelated = vehicleSource.create_entity("Unrelated");
    vehicleSource.set_parent(wheel.get_id(), chassis.get_id());
    vehicleSource.set_parent(lamp.get_id(), chassis.get_id());
    vehicleSource.rigidbodyComponents[chassis.get_id()].mass = 1200.0f;
    vehicleSource.meshRendererComponents[wheel.get_id()] = MeshRendererComponent{};
    vehicleSource.lightComponents[lamp.get_id()].intensity = 500.0f;

    Prefab vehiclePrefab(UUID(), "Vehicle");
    if (!vehiclePrefab.capture_hierarchy(vehicleSource, chassis.get_id()) || vehiclePrefab.entity_count() != 3)
        return EXIT_FAILURE;
    const UUID chassisLocal = vehiclePrefab.local_id_for_source(chassis.get_id());
    const UUID wheelLocal = vehiclePrefab.local_id_for_source(wheel.get_id());
    const UUID lampLocal = vehiclePrefab.local_id_for_source(lamp.get_id());
    if (!chassisLocal.is_valid() || !wheelLocal.is_valid() || !lampLocal.is_valid() ||
        vehiclePrefab.local_id_for_source(unrelated.get_id()).is_valid()) return EXIT_FAILURE;
    if (!vehiclePrefab.capture_hierarchy(vehicleSource, chassis.get_id()) ||
        vehiclePrefab.local_id_for_source(wheel.get_id()) != wheelLocal) return EXIT_FAILURE;

    Scene vehicleInstances("Vehicle Instances");
    const PrefabInstantiation firstVehicle = vehiclePrefab.instantiate_instance(&vehicleInstances, "Vehicle A");
    const PrefabInstantiation secondVehicle = vehiclePrefab.instantiate_instance(&vehicleInstances, "Vehicle B");
    if (!firstVehicle || !secondVehicle || firstVehicle.instanceID == secondVehicle.instanceID ||
        firstVehicle.entities.size() != 3 || secondVehicle.entities.size() != 3) return EXIT_FAILURE;
    const UUID firstChassis = firstVehicle.entities.at(chassisLocal);
    const UUID firstWheel = firstVehicle.entities.at(wheelLocal);
    const UUID secondLamp = secondVehicle.entities.at(lampLocal);
    if (firstChassis == secondVehicle.entities.at(chassisLocal) ||
        vehicleInstances.get_parent(firstWheel) != firstChassis ||
        vehicleInstances.get_parent(secondLamp) != secondVehicle.entities.at(chassisLocal)) return EXIT_FAILURE;
    if (vehiclePrefab.instance_id_for_entity(firstWheel) != firstVehicle.instanceID) return EXIT_FAILURE;

    // Property overrides are addressed by local entity/component/property and survive propagation.
    if (!vehiclePrefab.set_instance_override(vehicleInstances, firstVehicle.instanceID,
            {"LightComponent", "intensity", 700.0f, lampLocal})) return EXIT_FAILURE;
    if (!near(vehicleInstances.lightComponents.at(firstVehicle.entities.at(lampLocal)).intensity, 700.0f))
        return EXIT_FAILURE;
    if (!vehiclePrefab.set_prefab_property(lampLocal, "LightComponent", "intensity", 550.0f) ||
        !vehiclePrefab.propagate_all_instances()) return EXIT_FAILURE;
    if (!near(vehicleInstances.lightComponents.at(firstVehicle.entities.at(lampLocal)).intensity, 700.0f) ||
        !near(vehicleInstances.lightComponents.at(secondLamp).intensity, 550.0f)) return EXIT_FAILURE;

    // Component and entity overrides are independent from property overrides.
    if (!vehiclePrefab.set_component_removed(vehicleInstances, firstVehicle.instanceID,
            wheelLocal, "MeshRendererComponent") ||
        vehicleInstances.meshRendererComponents.contains(firstWheel)) return EXIT_FAILURE;
    if (!vehiclePrefab.revert_component_override(vehicleInstances, firstVehicle.instanceID,
            wheelLocal, "MeshRendererComponent") ||
        !vehicleInstances.meshRendererComponents.contains(firstWheel) ||
        !vehiclePrefab.set_component_removed(vehicleInstances, firstVehicle.instanceID,
            wheelLocal, "MeshRendererComponent")) return EXIT_FAILURE;
    if (!vehiclePrefab.set_entity_removed(vehicleInstances, firstVehicle.instanceID, lampLocal) ||
        vehicleInstances.find_entity_by_id(firstVehicle.entities.at(lampLocal)).is_valid()) return EXIT_FAILURE;
    const UUID antennaLocal = vehiclePrefab.add_instance_entity(
        vehicleInstances, firstVehicle.instanceID, chassisLocal, "Antenna");
    if (!antennaLocal.is_valid()) return EXIT_FAILURE;
    const PrefabInstance* firstState = vehiclePrefab.find_instance(firstVehicle.instanceID);
    if (!firstState || !firstState->entities.contains(antennaLocal) ||
        vehicleInstances.get_parent(firstState->entities.at(antennaLocal)) != firstChassis) return EXIT_FAILURE;
    if (!vehiclePrefab.propagate_instance(vehicleInstances, firstVehicle.instanceID) ||
        vehicleInstances.meshRendererComponents.contains(firstWheel) ||
        vehicleInstances.find_entity_by_id(firstVehicle.entities.at(lampLocal)).is_valid() ||
        !vehicleInstances.find_entity_by_id(firstState->entities.at(antennaLocal)).is_valid()) return EXIT_FAILURE;

    // Revert restores the asset value; apply promotes an override and updates peer instances.
    if (!vehiclePrefab.revert_override(vehicleInstances, firstVehicle.instanceID,
            lampLocal, "LightComponent", "intensity")) { std::cerr << "revert property failed\n"; return EXIT_FAILURE; }
    if (!vehiclePrefab.revert_entity_override(vehicleInstances, firstVehicle.instanceID, lampLocal)) { std::cerr << "revert entity failed\n"; return EXIT_FAILURE; }
    if (!near(vehicleInstances.lightComponents.at(firstVehicle.entities.at(lampLocal)).intensity, 550.0f)) { std::cerr << "reverted value wrong\n"; return EXIT_FAILURE; }
    if (!vehiclePrefab.set_instance_override(vehicleInstances, firstVehicle.instanceID,
            {"LightComponent", "intensity", 725.0f, lampLocal}) ||
        !vehiclePrefab.apply_override(vehicleInstances, firstVehicle.instanceID,
            lampLocal, "LightComponent", "intensity")) return EXIT_FAILURE;
    if (!near(vehicleInstances.lightComponents.at(secondLamp).intensity, 725.0f) ||
        vehiclePrefab.has_override(firstVehicle.instanceID, lampLocal, "LightComponent", "intensity"))
        return EXIT_FAILURE;

    // Nested prefabs receive their own identity, compose hierarchy, and reject dependency cycles.
    Scene tireSource("Tire Source");
    const Entity tireRoot = tireSource.create_entity("Tire Root");
    const Entity tireChild = tireSource.create_entity("Tire Child");
    tireSource.set_parent(tireChild.get_id(), tireRoot.get_id());
    Prefab tirePrefab(UUID(), "Tire");
    if (!tirePrefab.capture_hierarchy(tireSource, tireRoot.get_id())) return EXIT_FAILURE;
    if (!vehiclePrefab.add_nested_prefab(wheelLocal, tirePrefab) ||
        tirePrefab.add_nested_prefab(tirePrefab.root_local_id(), vehiclePrefab)) return EXIT_FAILURE;
    Scene nestedScene("Nested Instances");
    const PrefabInstantiation nestedVehicle = vehiclePrefab.instantiate_instance(&nestedScene);
    if (!nestedVehicle || nestedVehicle.nestedInstanceIDs.size() != 1 || nestedVehicle.entities.size() != 5)
        return EXIT_FAILURE;
    const UUID nestedTireRoot = nestedVehicle.entities.at(tirePrefab.root_local_id());
    if (nestedScene.get_parent(nestedTireRoot) != nestedVehicle.entities.at(wheelLocal)) return EXIT_FAILURE;
    const UUID tireChildLocal = tirePrefab.local_id_for_source(tireChild.get_id());
    if (!tirePrefab.set_prefab_property(tireChildLocal, "TransformComponent", "position",
            glm::vec3(3.0f, 4.0f, 5.0f)) ||
        !near(nestedScene.transformComponents.at(nestedVehicle.entities.at(tireChildLocal)).position.y, 4.0f))
        return EXIT_FAILURE;

    // Failed edits and cyclic/corrupt instantiation are transactional: no partial scene mutation.
    const size_t beforeFailedEdit = nestedScene.get_entities().size();
    if (vehiclePrefab.set_instance_override(nestedScene, nestedVehicle.instanceID,
            {"LightComponent", "intensity", std::string("wrong type"), lampLocal}) ||
        nestedScene.get_entities().size() != beforeFailedEdit) return EXIT_FAILURE;

    const std::filesystem::path sourceAsset = temporary / "source.png";
    const auto writePngFixture = [&](const std::filesystem::path& path, uint32_t width, uint32_t height, uint8_t marker) {
        const std::array<uint8_t, 26> bytes{
            0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A,
            0x00, 0x00, 0x00, 0x0D, 'I', 'H', 'D', 'R',
            static_cast<uint8_t>(width >> 24), static_cast<uint8_t>(width >> 16),
            static_cast<uint8_t>(width >> 8), static_cast<uint8_t>(width),
            static_cast<uint8_t>(height >> 24), static_cast<uint8_t>(height >> 16),
            static_cast<uint8_t>(height >> 8), static_cast<uint8_t>(height), marker, 6};
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    };
    writePngFixture(sourceAsset, 64, 32, 8);
    AssetRegistry registry;
    AssetPipeline pipeline(registry);
    pipeline.add_importer(std::make_unique<TextureImporter>());
    const ImportResult imported = pipeline.import({
        .source = temporary / "source.png",
        .cookedDirectory = temporary / "cache",
        .importerVersion = 3});
    if (!imported || !imported.asset.isCooked || imported.asset.width != 64 ||
        imported.asset.height != 32 || imported.asset.channels != 4 ||
        imported.asset.cookedPath.extension() != ".vctex" ||
        !std::filesystem::is_regular_file(imported.asset.cookedPath) ||
        registry.size() != 1 || !registry.find(imported.asset.id)) return EXIT_FAILURE;
    {
        std::ofstream invalidTexture(temporary / "invalid.png", std::ios::binary);
        invalidTexture << "not a png";
        if (pipeline.import({temporary / "invalid.png", temporary / "cache", 3}) || registry.size() != 1)
            return EXIT_FAILURE;
    }
    {
        std::ifstream cooked(imported.asset.cookedPath, std::ios::binary);
        std::array<char, 5> magic{};
        cooked.read(magic.data(), magic.size());
        if (!cooked || std::string_view(magic.data(), magic.size()) != "VCTEX") return EXIT_FAILURE;
    }

    // Import settings participate in cooking identity while preserving the asset UUID.
    ImportSettings textureSettings;
    textureSettings.generateMipmaps = false;
    textureSettings.srgb = false;
    textureSettings.textureQuality = 42;
    const ImportResult settingsReimport = pipeline.import({
        .source = sourceAsset,
        .cookedDirectory = temporary / "cache",
        .importerVersion = 3,
        .settings = textureSettings});
    if (!settingsReimport || settingsReimport.asset.id != imported.asset.id ||
        settingsReimport.asset.contentHash == imported.asset.contentHash ||
        settingsReimport.asset.settingsHash == 0) return EXIT_FAILURE;
    const std::filesystem::path settingsDatabase = temporary / "SettingsRegistry.db";
    AssetRegistry restoredSettingsRegistry;
    if (!registry.save(settingsDatabase) || !restoredSettingsRegistry.load(settingsDatabase)) return EXIT_FAILURE;
    const auto restoredSettings = restoredSettingsRegistry.find(imported.asset.id);
    if (!restoredSettings || restoredSettings->settingsHash != settingsReimport.asset.settingsHash ||
        restoredSettings->importSettings.generateMipmaps || restoredSettings->importSettings.srgb ||
        restoredSettings->importSettings.textureQuality != 42) return EXIT_FAILURE;

    // Structured mesh import validates glTF structure and records render metadata.
    const std::filesystem::path meshSource = temporary / "triangle.gltf";
    const std::filesystem::path meshTextureSource = temporary / "albedo.png";
    writePngFixture(meshTextureSource, 16, 16, 8);
    {
        std::ofstream mesh(meshSource, std::ios::binary);
        mesh << R"({"asset":{"version":"2.0"},"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}],"accessors":[{"count":3},{"count":3}],"images":[{"uri":"albedo.png"}]})";
    }
    AssetRegistry meshRegistry;
    AssetPipeline meshPipeline(meshRegistry);
    meshPipeline.add_importer(std::make_unique<MeshImporter>());
    meshPipeline.add_importer(std::make_unique<TextureImporter>());
    const ImportResult importedMesh = meshPipeline.import({meshSource, temporary / "mesh-cache", 2});
    if (!importedMesh || importedMesh.asset.type != AssetType::Mesh ||
        importedMesh.asset.primitiveCount != 1 || importedMesh.asset.vertexCount != 3 ||
        importedMesh.asset.indexCount != 3 || importedMesh.asset.cookedPath.extension() != ".vcmesh")
        return EXIT_FAILURE;
    const auto importedMeshTexture = meshRegistry.find_id(meshTextureSource);
    if (!importedMeshTexture || meshRegistry.dependencies_of(importedMesh.asset.id) !=
        std::vector<UUID>{*importedMeshTexture}) return EXIT_FAILURE;
    {
        std::ifstream cookedMesh(importedMesh.asset.cookedPath, std::ios::binary);
        std::array<char, 6> magic{};
        cookedMesh.read(magic.data(), magic.size());
        if (!cookedMesh || std::string_view(magic.data(), magic.size()) != "VCMESH") return EXIT_FAILURE;
    }
    const std::filesystem::path meshDatabase = temporary / "MeshRegistry.db";
    AssetRegistry restoredMeshRegistry;
    if (!meshRegistry.save(meshDatabase) || !restoredMeshRegistry.load(meshDatabase)) return EXIT_FAILURE;
    const auto restoredMesh = restoredMeshRegistry.find(importedMesh.asset.id);
    if (!restoredMesh || restoredMesh->primitiveCount != 1 || restoredMesh->vertexCount != 3 ||
        restoredMesh->indexCount != 3) return EXIT_FAILURE;

    const std::filesystem::path glbSource = temporary / "triangle.glb";
    {
        std::string json = R"({"asset":{"version":"2.0"},"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}],"accessors":[{"count":3},{"count":3}]})";
        while (json.size() % 4) json.push_back(' ');
        std::ofstream glb(glbSource, std::ios::binary);
        const auto writeU32 = [&](uint32_t value) { glb.write(reinterpret_cast<const char*>(&value), sizeof(value)); };
        glb.write("glTF", 4);
        writeU32(2);
        writeU32(static_cast<uint32_t>(20 + json.size()));
        writeU32(static_cast<uint32_t>(json.size()));
        writeU32(0x4E4F534A);
        glb.write(json.data(), static_cast<std::streamsize>(json.size()));
    }
    AssetRegistry glbRegistry;
    AssetPipeline glbPipeline(glbRegistry);
    glbPipeline.add_importer(std::make_unique<MeshImporter>());
    const ImportResult importedGlb = glbPipeline.import({glbSource, temporary / "mesh-cache", 2});
    if (!importedGlb || importedGlb.asset.vertexCount != 3 || importedGlb.asset.indexCount != 3)
        return EXIT_FAILURE;
    {
        std::ofstream invalidMesh(temporary / "invalid.gltf");
        invalidMesh << R"({"asset":{"version":"2.0"}})";
    }
    if (meshPipeline.import({temporary / "invalid.gltf", temporary / "mesh-cache", 2}) ||
        meshRegistry.size() != 2) return EXIT_FAILURE;

    // Structured WAV import validates RIFF chunks and records playback metadata.
    const std::filesystem::path wavSource = temporary / "tone.wav";
    {
        std::ofstream wav(wavSource, std::ios::binary);
        const auto u16 = [&](uint16_t value) { wav.write(reinterpret_cast<const char*>(&value), sizeof(value)); };
        const auto u32 = [&](uint32_t value) { wav.write(reinterpret_cast<const char*>(&value), sizeof(value)); };
        wav.write("RIFF", 4); u32(40); wav.write("WAVE", 4);
        wav.write("fmt ", 4); u32(16); u16(1); u16(2); u32(48000); u32(192000); u16(4); u16(16);
        wav.write("data", 4); u32(4); u32(0);
    }
    AssetRegistry audioRegistry;
    AssetPipeline audioPipeline(audioRegistry);
    audioPipeline.add_importer(std::make_unique<AudioImporter>());
    const ImportResult importedAudio = audioPipeline.import({wavSource, temporary / "audio-cache", 1});
    if (!importedAudio || importedAudio.asset.sampleRate != 48000 ||
        importedAudio.asset.audioChannels != 2 || importedAudio.asset.durationSeconds <= 0.0f ||
        importedAudio.asset.cookedPath.extension() != ".vcaudio") return EXIT_FAILURE;
    {
        std::ifstream cookedAudio(importedAudio.asset.cookedPath, std::ios::binary);
        std::array<char, 7> magic{};
        cookedAudio.read(magic.data(), magic.size());
        if (!cookedAudio || std::string_view(magic.data(), magic.size()) != "VCAUDIO") return EXIT_FAILURE;
    }
    const std::filesystem::path audioDatabase = temporary / "AudioRegistry.db";
    AssetRegistry restoredAudioRegistry;
    if (!audioRegistry.save(audioDatabase) || !restoredAudioRegistry.load(audioDatabase)) return EXIT_FAILURE;
    const auto restoredAudio = restoredAudioRegistry.find(importedAudio.asset.id);
    if (!restoredAudio || restoredAudio->sampleRate != 48000 || restoredAudio->audioChannels != 2 ||
        restoredAudio->durationSeconds <= 0.0f) return EXIT_FAILURE;
    {
        std::ofstream invalidAudio(temporary / "invalid.wav", std::ios::binary);
        invalidAudio << "not wave";
    }
    if (audioPipeline.import({temporary / "invalid.wav", temporary / "audio-cache", 1}) ||
        audioRegistry.size() != 1) return EXIT_FAILURE;

    // Reimport must preserve the persistent asset UUID while refreshing cooked data.
    const UUID importedTextureID = imported.asset.id;
    const uint64_t originalTextureHash = imported.asset.contentHash;
    writePngFixture(sourceAsset, 128, 32, 8);
    const ImportResult reimported = pipeline.import({
        .source = temporary / "source.png",
        .cookedDirectory = temporary / "cache",
        .importerVersion = 3});
    if (!reimported || reimported.asset.id != importedTextureID ||
        reimported.asset.contentHash == originalTextureHash || registry.size() != 1) return EXIT_FAILURE;

    // Hot reload must detect a changed source, reimport it, preserve UUID and notify listeners.
    AssetHotReloadService hotReload(pipeline, registry, temporary / "cache");
    int reloadNotifications = 0;
    hotReload.set_reload_callback([&](const AssetMetadata& metadata) {
        if (metadata.id == importedTextureID) ++reloadNotifications;
    });
    hotReload.watch_registered_assets();
    writePngFixture(sourceAsset, 128, 64, 8);
    const auto futureWrite = std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2);
    std::filesystem::last_write_time(temporary / "source.png", futureWrite);
    const std::vector<AssetMetadata> reloadedAssets = hotReload.poll();
    if (reloadedAssets.size() != 1 || reloadedAssets[0].id != importedTextureID || reloadNotifications != 1)
        return EXIT_FAILURE;

    // Content Browser queries must support type filters and case-insensitive path search.
    AssetBrowserModel browser(registry);
    auto textureResults = browser.query("SOURCE", AssetType::Texture);
    if (textureResults.size() != 1 || textureResults[0].id != importedTextureID) return EXIT_FAILURE;
    if (!browser.query("missing", AssetType::Texture).empty()) return EXIT_FAILURE;
    if (!browser.query("source", AssetType::Mesh).empty()) return EXIT_FAILURE;

    // Rename/move in Content Browser must keep UUID references stable and update the registry.
    const std::filesystem::path renamedTexture = temporary / "Textures" / "renamed_source.png";
    const AssetFileOperationResult renamed = browser.move_asset(importedTextureID, renamedTexture);
    if (!renamed.success || renamed.asset.id != importedTextureID ||
        !std::filesystem::is_regular_file(renamedTexture) ||
        registry.find_id(temporary / "source.png").has_value() ||
        registry.find_id(renamedTexture) != importedTextureID) return EXIT_FAILURE;

    // Duplicate creates an independent persistent asset identity.
    const std::filesystem::path duplicatedTexture = temporary / "Textures" / "renamed_source_copy.png";
    const AssetFileOperationResult duplicated = browser.duplicate_asset(importedTextureID, duplicatedTexture);
    if (!duplicated.success || duplicated.asset.id == importedTextureID ||
        !std::filesystem::is_regular_file(duplicatedTexture) ||
        registry.find_id(duplicatedTexture) != duplicated.asset.id || registry.size() != 2)
        return EXIT_FAILURE;

    // Dependency graph must support forward/reverse queries and persist across editor sessions.
    if (!registry.set_dependencies(duplicated.asset.id, {importedTextureID})) return EXIT_FAILURE;
    const auto directDependencies = registry.dependencies_of(duplicated.asset.id);
    const auto reverseReferences = registry.referencers_of(importedTextureID);
    if (directDependencies.size() != 1 || directDependencies[0] != importedTextureID ||
        reverseReferences.size() != 1 || reverseReferences[0] != duplicated.asset.id)
        return EXIT_FAILURE;
    if (!registry.unused_assets({duplicated.asset.id}).empty()) return EXIT_FAILURE;
    const auto unusedFromTextureRoot = registry.unused_assets({importedTextureID});
    if (unusedFromTextureRoot.size() != 1 || unusedFromTextureRoot[0] != duplicated.asset.id)
        return EXIT_FAILURE;

    // Packaging follows the dependency closure and includes only cooked reachable assets.
    const std::filesystem::path packageDirectory = temporary / "PackagedGame";
    const AssetPackageResult packageResult = AssetPackager::package(
        registry, {importedTextureID}, packageDirectory);
    if (!packageResult.success || packageResult.assets.size() != 1 ||
        packageResult.assets[0] != importedTextureID ||
        !std::filesystem::is_regular_file(packageResult.manifestPath) ||
        !std::filesystem::is_regular_file(packageDirectory / "Content" /
            importedTextureID.to_string() / renamed.asset.cookedPath.filename()))
        return EXIT_FAILURE;

    // Asset database persistence must preserve stable identities, cooked metadata, and dependencies.
    const std::filesystem::path databaseFile = temporary / "AssetRegistry.db";
    if (!registry.save(databaseFile)) return EXIT_FAILURE;
    AssetRegistry restoredRegistry;
    if (!restoredRegistry.load(databaseFile)) return EXIT_FAILURE;
    const auto restoredTexture = restoredRegistry.find(importedTextureID);
    if (!restoredTexture || restoredTexture->sourcePath != renamed.asset.sourcePath ||
        restoredTexture->contentHash != renamed.asset.contentHash ||
        restoredTexture->width != 128 || restoredTexture->height != 64 ||
        restoredTexture->channels != 4 ||
        restoredRegistry.find_id(renamed.asset.sourcePath) != importedTextureID ||
        restoredRegistry.dependencies_of(duplicated.asset.id) != directDependencies ||
        restoredRegistry.referencers_of(importedTextureID) != reverseReferences)
        return EXIT_FAILURE;

    // Standalone cooker entry point loads the database and packages selected roots.
    const std::filesystem::path cliPackageDirectory = temporary / "CookerPackage";
    std::cerr << "[CP] before cooker\n";
    if (run_asset_cooker({databaseFile.string(), importedTextureID.to_string(),
                          cliPackageDirectory.string()}) != EXIT_SUCCESS ||
        !std::filesystem::is_regular_file(cliPackageDirectory / "AssetManifest.txt"))
        return EXIT_FAILURE;
    if (run_asset_cooker({}) == EXIT_SUCCESS) return EXIT_FAILURE;
    std::cerr << "[CP] after cooker\n";

    // Safe deletion rejects referenced assets and removes unreferenced source/cooked files plus registry metadata.
    const AssetFileOperationResult blockedDelete = browser.delete_asset(importedTextureID);
    if (blockedDelete.success || !std::filesystem::is_regular_file(renamedTexture) ||
        !registry.find(importedTextureID)) return EXIT_FAILURE;
    const AssetFileOperationResult deletedDuplicate = browser.delete_asset(duplicated.asset.id);
    if (!deletedDuplicate.success || std::filesystem::exists(duplicatedTexture) ||
        registry.find(duplicated.asset.id) || !registry.referencers_of(importedTextureID).empty())
        return EXIT_FAILURE;

    UndoSystem undo;
    std::cerr << "[CP] undo start\n";
    undo.execute_command(std::make_unique<MoveEntityCommand>(
        &loaded, entity.get_id(), transform.position, glm::vec3(1.0f, 2.0f, 3.0f)));
    if (!near(loaded.transformComponents.at(entity.get_id()).position.y, 2.0f)) return EXIT_FAILURE;
    undo.undo();
    if (!near(loaded.transformComponents.at(entity.get_id()).position.x, 12.5f)) return EXIT_FAILURE;
    undo.redo();
    if (!near(loaded.transformComponents.at(entity.get_id()).position.z, 3.0f)) return EXIT_FAILURE;

    const float oldIntensity = loaded.lightComponents.at(entity.get_id()).intensity;
    const float newIntensity = 7777.0f;
    undo.execute_command(std::make_unique<PropertyChangeCommand>(
        "LightComponent.Display Intensity",
        [&loaded, id = entity.get_id(), newIntensity] {
            loaded.lightComponents.at(id).intensity = newIntensity;
        },
        [&loaded, id = entity.get_id(), oldIntensity] {
            loaded.lightComponents.at(id).intensity = oldIntensity;
        }));
    if (!near(loaded.lightComponents.at(entity.get_id()).intensity, newIntensity)) return EXIT_FAILURE;
    undo.undo();
    if (!near(loaded.lightComponents.at(entity.get_id()).intensity, oldIntensity)) return EXIT_FAILURE;
    undo.redo();
    if (!near(loaded.lightComponents.at(entity.get_id()).intensity, newIntensity)) return EXIT_FAILURE;

    const float mergedStart = loaded.lightComponents.at(entity.get_id()).intensity;
    const float mergedMiddle = 8000.0f;
    const float mergedFinal = 9000.0f;
    undo.clear();
    undo.execute_or_merge_property(
        "LightComponent.Display Intensity",
        [&loaded, id = entity.get_id(), mergedMiddle] {
            loaded.lightComponents.at(id).intensity = mergedMiddle;
        },
        [&loaded, id = entity.get_id(), mergedStart] {
            loaded.lightComponents.at(id).intensity = mergedStart;
        });
    undo.execute_or_merge_property(
        "LightComponent.Display Intensity",
        [&loaded, id = entity.get_id(), mergedFinal] {
            loaded.lightComponents.at(id).intensity = mergedFinal;
        },
        [&loaded, id = entity.get_id(), mergedMiddle] {
            loaded.lightComponents.at(id).intensity = mergedMiddle;
        });
    if (!near(loaded.lightComponents.at(entity.get_id()).intensity, mergedFinal)) return EXIT_FAILURE;
    undo.undo();
    if (!near(loaded.lightComponents.at(entity.get_id()).intensity, mergedStart)) return EXIT_FAILURE;
    undo.redo();
    if (!near(loaded.lightComponents.at(entity.get_id()).intensity, mergedFinal)) return EXIT_FAILURE;

    const UUID componentEntityID = loaded.create_entity("Component Command Target").get_id();
    undo.clear();
    undo.execute_command(std::make_unique<AddComponentCommand>(
        "Add LightComponent",
        [&loaded, componentEntityID] {
            loaded.lightComponents[componentEntityID] = LightComponent{};
        },
        [&loaded, componentEntityID] {
            loaded.lightComponents.erase(componentEntityID);
        }));
    if (!loaded.lightComponents.contains(componentEntityID)) return EXIT_FAILURE;
    undo.undo();
    if (loaded.lightComponents.contains(componentEntityID)) return EXIT_FAILURE;
    undo.redo();
    if (!loaded.lightComponents.contains(componentEntityID)) return EXIT_FAILURE;

    loaded.lightComponents.at(componentEntityID).intensity = 1234.0f;
    const LightComponent removedComponent = loaded.lightComponents.at(componentEntityID);
    undo.execute_command(std::make_unique<RemoveComponentCommand>(
        "Remove LightComponent",
        [&loaded, componentEntityID] {
            loaded.lightComponents.erase(componentEntityID);
        },
        [&loaded, componentEntityID, removedComponent] {
            loaded.lightComponents[componentEntityID] = removedComponent;
        }));
    if (loaded.lightComponents.contains(componentEntityID)) return EXIT_FAILURE;
    undo.undo();
    if (!loaded.lightComponents.contains(componentEntityID) ||
        !near(loaded.lightComponents.at(componentEntityID).intensity, 1234.0f)) return EXIT_FAILURE;
    undo.redo();
    if (loaded.lightComponents.contains(componentEntityID)) return EXIT_FAILURE;

    // Test CreateEntityCommand, RenameEntityCommand, and DeleteEntityCommand with component snapshot
    undo.clear();
    undo.execute_command(std::make_unique<CreateEntityCommand>(&loaded, "Test Entity Commands"));
    UUID testEntID{0, 0};
    for (const auto& [id, ent] : loaded.get_entities()) {
        if (ent.get_name() == "Test Entity Commands") {
            testEntID = id;
            break;
        }
    }
    const Entity createdEnt = loaded.find_entity_by_id(testEntID);
    if (!createdEnt.is_valid() || createdEnt.get_name() != "Test Entity Commands") return EXIT_FAILURE;
    
    loaded.lightComponents[testEntID] = LightComponent{{1.0f, 0.0f, 0.0f}, 500.0f, 10.0f, true};

    undo.execute_command(std::make_unique<RenameEntityCommand>(&loaded, testEntID, "Test Entity Commands", "Renamed Entity"));
    if (loaded.find_entity_by_id(testEntID).get_name() != "Renamed Entity") return EXIT_FAILURE;
    undo.undo();
    if (loaded.find_entity_by_id(testEntID).get_name() != "Test Entity Commands") return EXIT_FAILURE;
    undo.redo();
    if (loaded.find_entity_by_id(testEntID).get_name() != "Renamed Entity") return EXIT_FAILURE;

    undo.execute_command(std::make_unique<DeleteEntityCommand>(&loaded, testEntID));
    if (loaded.find_entity_by_id(testEntID).is_valid()) return EXIT_FAILURE;
    if (loaded.lightComponents.contains(testEntID)) return EXIT_FAILURE;

    undo.undo();
    const Entity restoredEnt = loaded.find_entity_by_id(testEntID);
    if (!restoredEnt.is_valid() || restoredEnt.get_name() != "Renamed Entity") return EXIT_FAILURE;
    if (!loaded.lightComponents.contains(testEntID) || !near(loaded.lightComponents.at(testEntID).intensity, 500.0f)) return EXIT_FAILURE;

    undo.redo();
    if (loaded.find_entity_by_id(testEntID).is_valid()) return EXIT_FAILURE;
    if (loaded.lightComponents.contains(testEntID)) return EXIT_FAILURE;

    // Test Entity Hierarchy and Reparenting
    undo.clear();
    const Entity parentEnt = loaded.create_entity("Parent Entity");
    const Entity childEnt = loaded.create_entity("Child Entity");
    loaded.set_parent(childEnt.get_id(), parentEnt.get_id());
    if (loaded.get_parent(childEnt.get_id()) != parentEnt.get_id()) return EXIT_FAILURE;
    if (loaded.get_children(parentEnt.get_id()).size() != 1 || loaded.get_children(parentEnt.get_id())[0] != childEnt.get_id()) return EXIT_FAILURE;

    undo.execute_command(std::make_unique<ReparentEntityCommand>(&loaded, childEnt.get_id(), parentEnt.get_id(), UUID{0, 0}));
    if (loaded.get_parent(childEnt.get_id()).is_valid()) return EXIT_FAILURE;
    if (!loaded.get_children(parentEnt.get_id()).empty()) return EXIT_FAILURE;

    undo.undo();
    if (loaded.get_parent(childEnt.get_id()) != parentEnt.get_id()) return EXIT_FAILURE;

    // Test Hierarchy Serialization
    const std::filesystem::path hierarchySceneFile = temporary / "hierarchy.scene";
    if (!Serializer::serialize_scene(loaded, hierarchySceneFile)) return EXIT_FAILURE;
    Scene loadedHierarchy;
    if (!Serializer::deserialize_scene(loadedHierarchy, hierarchySceneFile)) return EXIT_FAILURE;
    if (loadedHierarchy.get_parent(childEnt.get_id()) != parentEnt.get_id()) return EXIT_FAILURE;
    if (loadedHierarchy.get_children(parentEnt.get_id()).size() != 1) return EXIT_FAILURE;

    // Test Prefab File Persistence (.prefab)
    const std::filesystem::path prefabFile = temporary / "light_test.prefab";
    if (!lightPrefab.save_to_file(prefabFile.string())) return EXIT_FAILURE;
    Prefab loadedPrefab;
    if (!loadedPrefab.load_from_file(prefabFile.string())) return EXIT_FAILURE;
    if (loadedPrefab.get_name() != lightPrefab.get_name()) return EXIT_FAILURE;
    Scene loadedPrefabScene("Loaded Prefab Scene");
    const Entity loadedPrefabInst = loadedPrefab.instantiate(&loadedPrefabScene, "Saved Prefab Instance");
    if (!loadedPrefabInst.is_valid() || !loadedPrefabScene.lightComponents.contains(loadedPrefabInst.get_id())) return EXIT_FAILURE;

    // Version 2 persists stable local IDs, hierarchy, every component family, nested references and defaults.
    const std::filesystem::path vehiclePrefabFile = temporary / "vehicle.prefab";
    vehiclePrefab.set_override({"RigidbodyComponent", "mass", 1400.0f, chassisLocal});
    if (!vehiclePrefab.save_to_file(vehiclePrefabFile.string())) return EXIT_FAILURE;
    Prefab restoredVehicle;
    if (!restoredVehicle.load_from_file(vehiclePrefabFile.string()) ||
        restoredVehicle.get_id() != vehiclePrefab.get_id() || restoredVehicle.entity_count() != 3 ||
        restoredVehicle.root_local_id() != chassisLocal ||
        restoredVehicle.local_id_for_source(wheel.get_id()) != wheelLocal ||
        restoredVehicle.get_nested_prefabs().size() != 1) return EXIT_FAILURE;
    Scene unresolvedNestedScene("Unresolved Nested");
    const size_t unresolvedBefore = unresolvedNestedScene.get_entities().size();
    if (restoredVehicle.instantiate_instance(&unresolvedNestedScene) ||
        unresolvedNestedScene.get_entities().size() != unresolvedBefore) return EXIT_FAILURE;
    if (!restoredVehicle.bind_nested_prefab(tirePrefab.get_id(), tirePrefab)) return EXIT_FAILURE;
    const PrefabInstantiation restoredVehicleInstance = restoredVehicle.instantiate_instance(&unresolvedNestedScene);
    if (!restoredVehicleInstance || restoredVehicleInstance.entities.size() != 5 ||
        unresolvedNestedScene.get_parent(restoredVehicleInstance.entities.at(wheelLocal)) !=
            restoredVehicleInstance.entities.at(chassisLocal) ||
        !near(unresolvedNestedScene.rigidbodyComponents.at(
            restoredVehicleInstance.entities.at(chassisLocal)).mass, 1400.0f)) return EXIT_FAILURE;

    // Version 1 files remain readable and are promoted to a one-entity prefab.
    const std::filesystem::path legacyPrefabFile = temporary / "legacy.prefab";
    {
        std::ofstream legacy(legacyPrefabFile);
        legacy << "{\"format\":\"VulkanEngine.Prefab\",\"version\":1,"
               << "\"prefab_id\":\"" << UUID().to_string() << "\",\"name\":\"Legacy\","
               << "\"captured\":true,\"components\":{\"Light\":{\"intensity\":321}}}";
    }
    Prefab legacyPrefab;
    if (!legacyPrefab.load_from_file(legacyPrefabFile.string()) || legacyPrefab.entity_count() != 1) return EXIT_FAILURE;
    Scene legacyScene("Legacy Prefab");
    const Entity legacyInstance = legacyPrefab.instantiate(&legacyScene);
    if (!legacyInstance.is_valid() ||
        !near(legacyScene.lightComponents.at(legacyInstance.get_id()).intensity, 321.0f)) return EXIT_FAILURE;

    // Test PlayModeManager (Edit, Play, Pause, Step Frame, Simulate, Stop)
    PlayModeManager playMode;
    if (playMode.get_state() != PlayState::Edit) return EXIT_FAILURE;

    playMode.start_play(&loaded);
    if (playMode.get_state() != PlayState::Play) return EXIT_FAILURE;
    Scene* activePlayScene = playMode.get_active_scene();
    if (!activePlayScene || activePlayScene == &loaded) return EXIT_FAILURE;

    playMode.pause_play();
    if (playMode.get_state() != PlayState::Pause) return EXIT_FAILURE;

    bool stepped = false;
    playMode.step_frame(0.016f, [&](Scene* s, float dt) {
        stepped = (s == activePlayScene && near(dt, 0.016f));
    });
    if (!stepped) return EXIT_FAILURE;

    playMode.stop_play();
    if (playMode.get_state() != PlayState::Edit) return EXIT_FAILURE;

    playMode.start_simulate(&loaded);
    if (playMode.get_state() != PlayState::Simulate) return EXIT_FAILURE;
    playMode.stop_play();
    if (playMode.get_state() != PlayState::Edit) return EXIT_FAILURE;

    // Test Material Asset Persistence (.material)
    MaterialAsset matAsset;
    matAsset.id = UUID();
    matAsset.name = "Gold PBR Material";
    matAsset.albedo = {1.0f, 0.85f, 0.57f};
    matAsset.roughness = 0.2f;
    matAsset.metallic = 1.0f;
    const std::filesystem::path matFile = temporary / "gold.material";
    if (!matAsset.save_to_file(matFile)) return EXIT_FAILURE;
    MaterialAsset loadedMat;
    if (!loadedMat.load_from_file(matFile)) return EXIT_FAILURE;
    if (loadedMat.name != "Gold PBR Material" || !near(loadedMat.roughness, 0.2f) || !near(loadedMat.metallic, 1.0f)) return EXIT_FAILURE;
    if (!near(loadedMat.albedo.r, 1.0f) || !near(loadedMat.albedo.g, 0.85f)) return EXIT_FAILURE;

    // Test TextMaterialImporter in AssetPipeline
    pipeline.add_importer(std::make_unique<TextMaterialImporter>());
    const ImportResult matImported = pipeline.import({
        .source = matFile,
        .cookedDirectory = temporary / "cache",
        .importerVersion = 1});
    if (!matImported || !matImported.asset.isCooked ||
        !std::filesystem::is_regular_file(matImported.asset.cookedPath) ||
        registry.size() != 2 || !registry.find(matImported.asset.id)) return EXIT_FAILURE;

    // Test AudioEventAsset Persistence (.audioevent)
    AudioEventAsset audioEvt;
    audioEvt.id = UUID();
    audioEvt.name = "Explosion Spatial Sound";
    audioEvt.clipPath = "assets/audio/explosion.wav";
    audioEvt.volume = 0.95f;
    audioEvt.minPitch = 0.85f;
    audioEvt.maxPitch = 1.15f;
    audioEvt.is3D = true;
    audioEvt.isLooping = false;
    const std::filesystem::path audioEvtFile = temporary / "explosion.audioevent";
    if (!audioEvt.save_to_file(audioEvtFile)) return EXIT_FAILURE;
    AudioEventAsset loadedAudioEvt;
    if (!loadedAudioEvt.load_from_file(audioEvtFile)) return EXIT_FAILURE;
    if (loadedAudioEvt.name != "Explosion Spatial Sound" || loadedAudioEvt.clipPath != "assets/audio/explosion.wav") return EXIT_FAILURE;
    if (!near(loadedAudioEvt.volume, 0.95f) || !near(loadedAudioEvt.minPitch, 0.85f) || !loadedAudioEvt.is3D) return EXIT_FAILURE;

    // Test PhysicsWorld and PhysicsMaterialAsset (.physicsmaterial)
    PhysicsMaterialAsset physMat;
    physMat.id = UUID();
    physMat.name = "Rubber Friction Material";
    physMat.friction = 0.9f;
    physMat.restitution = 0.8f;
    const std::filesystem::path physMatFile = temporary / "rubber.physicsmaterial";
    if (!physMat.save_to_file(physMatFile)) return EXIT_FAILURE;
    PhysicsMaterialAsset loadedPhysMat;
    if (!loadedPhysMat.load_from_file(physMatFile)) return EXIT_FAILURE;
    if (loadedPhysMat.name != "Rubber Friction Material" || !near(loadedPhysMat.friction, 0.9f) || !near(loadedPhysMat.restitution, 0.8f)) return EXIT_FAILURE;

    PhysicsWorld physWorld;
    const Entity fallingEnt = loaded.create_entity("Falling Physics Entity");
    loaded.transformComponents[fallingEnt.get_id()].position = {0.0f, 10.0f, 0.0f};
    loaded.rigidbodyComponents[fallingEnt.get_id()] = RigidbodyComponent{10.0f, 0.5f, 0.1f, false, true};
    physWorld.step(loaded, 0.1f);
    if (! (loaded.transformComponents.at(fallingEnt.get_id()).position.y < 10.0f)) return EXIT_FAILURE;

    const RaycastHit hit = physWorld.raycast(loaded, {0.0f, 20.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 100.0f);
    if (!hit.hit || hit.entityID != fallingEnt.get_id()) return EXIT_FAILURE;

    // Test VisualScriptGraph Execution and Persistence (.script)
    VisualScriptGraph scriptGraph;
    scriptGraph.id = UUID();
    scriptGraph.name = "Health Regeneration Graph";
    
    bool scriptEventExecuted = false;
    ScriptNode startNode;
    startNode.id = UUID();
    startNode.title = "OnStart";
    startNode.executionCallback = [&](Scene* s, Entity instigator) {
        scriptEventExecuted = true;
    };
    scriptGraph.add_node(startNode);

    scriptGraph.execute_event("OnStart", &loaded, fallingEnt);
    if (!scriptEventExecuted) return EXIT_FAILURE;

    const std::filesystem::path scriptFile = temporary / "regen.script";
    if (!scriptGraph.save_to_file(scriptFile)) return EXIT_FAILURE;
    VisualScriptGraph loadedScript;
    if (!loadedScript.load_from_file(scriptFile)) return EXIT_FAILURE;
    if (loadedScript.name != "Health Regeneration Graph" || loadedScript.nodes.size() != 1) return EXIT_FAILURE;

    // Test PluginRegistry (Plugin discovery, registration, lifecycle, enable/disable)
    class DummyTestPlugin : public Plugin {
    public:
        std::string get_name() const override { return "TestDummyPlugin"; }
        std::string get_version() const override { return "1.0.0"; }
        void on_load() override { loadedCount++; }
        void on_unload() override { unloadedCount++; }
        int loadedCount{ 0 };
        int unloadedCount{ 0 };
    };

    auto dummyPlugin = std::make_shared<DummyTestPlugin>();
    PluginRegistry::get().register_plugin(dummyPlugin);
    if (!PluginRegistry::get().is_plugin_enabled("TestDummyPlugin") || dummyPlugin->loadedCount != 1) return EXIT_FAILURE;

    PluginRegistry::get().set_plugin_enabled("TestDummyPlugin", false);
    if (PluginRegistry::get().is_plugin_enabled("TestDummyPlugin") || dummyPlugin->unloadedCount != 1) return EXIT_FAILURE;

    PluginRegistry::get().set_plugin_enabled("TestDummyPlugin", true);
    if (!PluginRegistry::get().is_plugin_enabled("TestDummyPlugin") || dummyPlugin->loadedCount != 2) return EXIT_FAILURE;

    // Test DerivedDataCache (hash/settings/importer/platform keying, invalidation, stats, persistence)
    {
        DerivedDataCache cache;
        const std::filesystem::path ddcRoot = temporary / "ddc";
        std::filesystem::create_directories(ddcRoot);
        const std::filesystem::path ddcDb = ddcRoot / "ddc.bin";
        const std::filesystem::path cookedAsset = ddcRoot / "cooked.vctex";
        { std::ofstream(cookedAsset, std::ios::binary) << "payload"; }

        DerivedDataKey keyA;
        keyA.source = ddcRoot / "a.png";
        keyA.sourceHash = 100;
        keyA.settingsHash = 200;
        keyA.importerVersion = 3;
        keyA.platform = "win64";
        DerivedDataEntry entryA;
        entryA.cookedPath = cookedAsset;
        entryA.contentHash = 12345;
        entryA.sourceHash = keyA.sourceHash;
        entryA.settingsHash = keyA.settingsHash;
        entryA.importerVersion = 3;
        entryA.platform = "win64";

        // Miss first, hit after store
        if (cache.find(keyA)) return EXIT_FAILURE;
        cache.store(keyA, entryA);
        const auto hit = cache.find(keyA);
        if (!hit || hit->contentHash != 12345 || hit->cookedPath != cookedAsset) return EXIT_FAILURE;

        // Different platform or importer version must miss
        DerivedDataKey keyB = keyA;
        keyB.platform = "linux";
        if (cache.find(keyB)) return EXIT_FAILURE;
        DerivedDataKey keyC = keyA;
        keyC.importerVersion = 4;
        if (cache.find(keyC)) return EXIT_FAILURE;

        // Stats recorded
        const DerivedDataStats stats = cache.stats();
        if (stats.hits < 1 || stats.misses < 3) return EXIT_FAILURE;

        // Persistence roundtrip
        if (!cache.save(ddcDb)) return EXIT_FAILURE;
        DerivedDataCache restored;
        if (!restored.load(ddcDb)) return EXIT_FAILURE;
        const auto restoredHit = restored.find(keyA);
        if (!restoredHit || restoredHit->contentHash != 12345) return EXIT_FAILURE;

        // Invalidation removes entries for the source
        if (!restored.invalidate_source(ddcRoot / "a.png")) return EXIT_FAILURE;
        if (restored.find(keyA)) return EXIT_FAILURE;

        // Clear wipes everything
        cache.clear();
        if (cache.find(keyA)) return EXIT_FAILURE;
        const auto emptyStats = cache.stats();
        if (emptyStats.hits < 1) return EXIT_FAILURE;
    }

    // Test real TGA and Radiance HDR decoding through the texture importer.
    {
        const std::filesystem::path texRoot = temporary / "textures";
        std::filesystem::create_directories(texRoot);
        AssetRegistry texRegistry;
        AssetPipeline texPipeline(texRegistry);
        texPipeline.add_importer(std::make_unique<TextureImporter>());

        // Synthetic 2x2 24-bit uncompressed TGA (bottom-up, BGR).
        const std::filesystem::path tgaPath = texRoot / "tiny.tga";
        {
            std::vector<uint8_t> tga;
            tga.resize(18, 0);
            tga[2] = 2;                       // uncompressed true-color
            tga[12] = 2; tga[13] = 0;         // width = 2
            tga[14] = 2; tga[15] = 0;         // height = 2
            tga[16] = 24;                     // 24-bit
            tga[17] = 0;
            // Pixel data bottom-up: row1 (B,G,R), row0 (B,G,R)
            const uint8_t pixels[12] = {
                0x10, 0x20, 0x30,  0x40, 0x50, 0x60,   // bottom row
                0x70, 0x80, 0x90,  0xA0, 0xB0, 0xC0    // top row
            };
            tga.insert(tga.end(), pixels, pixels + sizeof(pixels));
            std::ofstream out(tgaPath, std::ios::binary);
            out.write(reinterpret_cast<const char*>(tga.data()), static_cast<std::streamsize>(tga.size()));
        }
        const ImportResult tgaResult = texPipeline.import({tgaPath, texRoot / "cooked", 1});
        if (!tgaResult) { std::cerr << "[DBG-tga] import failed: " << tgaResult.error << "\n"; return EXIT_FAILURE; }
        if (tgaResult.asset.width != 2 || tgaResult.asset.height != 2 || tgaResult.asset.channels != 3) {
            std::cerr << "[DBG-tga] meta " << tgaResult.asset.width << "x" << tgaResult.asset.height
                      << " ch=" << tgaResult.asset.channels << "\n";
            return EXIT_FAILURE;
        }
        // Read back cooked payload and verify top-down RGB ordering.
        {
            std::ifstream cooked(tgaResult.asset.cookedPath, std::ios::binary);
            std::vector<uint8_t> cookedBytes((std::istreambuf_iterator<char>(cooked)), {});
            if (cookedBytes.size() < 5 + 4 + 4 + 4 + 4 + 1 + 4 + 1 + 8 + 12) { std::cerr << "[DBG-tga] short payload " << cookedBytes.size() << "\n"; return EXIT_FAILURE; }
            const size_t payloadOffset = 5 + 4 + 4 + 4 + 4 + 1 + 4 + 1 + 8;
            if (cookedBytes[payloadOffset] != 0x90 || cookedBytes[payloadOffset + 1] != 0x80 ||
                cookedBytes[payloadOffset + 2] != 0x70) {
                std::cerr << "[DBG-tga] pixel " << static_cast<int>(cookedBytes[payloadOffset]) << ","
                          << static_cast<int>(cookedBytes[payloadOffset + 1]) << ","
                          << static_cast<int>(cookedBytes[payloadOffset + 2]) << "\n";
                return EXIT_FAILURE;
            }
        }

        // Synthetic 1x1 Radiance HDR (RGBE) with header.
        const std::filesystem::path hdrPath = texRoot / "tiny.hdr";
        {
            std::vector<uint8_t> hdr;
            const std::string header =
                "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 1\n";
            hdr.assign(header.begin(), header.end());
            const uint8_t rgbe[4] = { 0x80, 0x40, 0x20, 0x80 }; // (0.5, 0.25, 0.125) * 1
            hdr.insert(hdr.end(), rgbe, rgbe + 4);
            std::ofstream out(hdrPath, std::ios::binary);
            out.write(reinterpret_cast<const char*>(hdr.data()), static_cast<std::streamsize>(hdr.size()));
        }
        const ImportResult hdrResult = texPipeline.import({hdrPath, texRoot / "cooked", 1});
        if (!hdrResult) { std::cerr << "[DBG-hdr] import failed: " << hdrResult.error << "\n"; return EXIT_FAILURE; }
        if (hdrResult.asset.width != 1 || hdrResult.asset.height != 1 || hdrResult.asset.channels != 4) {
            std::cerr << "[DBG-hdr] meta " << hdrResult.asset.width << "x" << hdrResult.asset.height
                      << " ch=" << hdrResult.asset.channels << "\n";
            return EXIT_FAILURE;
        }
        {
            std::ifstream cooked(hdrResult.asset.cookedPath, std::ios::binary);
            std::vector<uint8_t> cookedBytes((std::istreambuf_iterator<char>(cooked)), {});
            if (cookedBytes.size() < 5 + 4 + 4 + 4 + 4 + 1 + 4 + 1 + 8 + 8) { std::cerr << "[DBG-hdr] short payload " << cookedBytes.size() << "\n"; return EXIT_FAILURE; }
            const size_t payloadOffset = 5 + 4 + 4 + 4 + 4 + 1 + 4 + 1 + 8;
            uint16_t halfR{};
            std::memcpy(&halfR, cookedBytes.data() + payloadOffset, 2);
            // 0.5 -> 0x3800 half.
            if (halfR != 0x3800) { std::cerr << "[DBG-hdr] halfR=" << std::hex << halfR << std::dec << "\n"; return EXIT_FAILURE; }
        }
    }

    std::error_code ignored;
    std::filesystem::remove_all(temporary, ignored);

    // glTF import: JSON parser extracts skeletons, skins and animation clips;
    // GPU skinning buffer produces bone matrices and a skinned vertex shader.
    {
        const std::string gltfJson = R"json(
{
  "asset": {"version": "2.0"},
  "nodes": [
    {"name": "Root", "children": [1]},
    {"name": "Hips", "translation": [0.0, 1.0, 0.0]},
    {"name": "LeftArm", "rotation": [0.0, 0.0, 0.7071, 0.7071]}
  ],
  "skins": [
    {"name": "Humanoid", "joints": [0, 1, 2]}
  ],
  "animations": [
    {
      "name": "Walk",
      "samplers": [
        {"times": [0.0, 1.0], "output": [0.0, 1.0, 0.0, 0.0, 2.0, 0.0]}
      ],
      "channels": [
        {"sampler": 0, "target": {"node": 1, "path": "translation"}}
      ]
    }
  ]
}
)json";
        GltfParser parser(gltfJson);
        std::string gltfError;
        if (!parser.parse(&gltfError)) { std::cerr << "Failure at line " << __LINE__ << ": " << gltfError << '\n'; return EXIT_FAILURE; }
        if (!parser.has_skins()) { std::cerr << "Failure at line " << __LINE__ << '\n'; return EXIT_FAILURE; }
        if (parser.node_names().size() != 3 || parser.node_names()[1] != "Hips") { std::cerr << "Failure at line " << __LINE__ << '\n'; return EXIT_FAILURE; }
        if (parser.clips().size() != 1) { std::cerr << "Failure at line " << __LINE__ << '\n'; return EXIT_FAILURE; }
        const auto& clip = parser.clips()[0];
        if (clip.name != "Walk" || clip.channels.size() != 1 || clip.duration < 0.99f) { std::cerr << "Failure at line " << __LINE__ << '\n'; return EXIT_FAILURE; }
        const auto& channel = clip.channels[0];
        if (channel.nodeName != "Hips" || channel.times.size() != 2) { std::cerr << "Failure at line " << __LINE__ << '\n'; return EXIT_FAILURE; }
        if (channel.translations.size() != 2) { std::cerr << "Failure at line " << __LINE__ << '\n'; return EXIT_FAILURE; }
        if (std::abs(channel.translations[1].y - 2.0f) > 1e-4f) { std::cerr << "Failure at line " << __LINE__ << '\n'; return EXIT_FAILURE; }

        // Skeleton from skin.
        const SkeletonAsset skeleton = parser.make_skeleton();
        if (skeleton.bones.size() != 3 || skeleton.bones[1].name != "Hips") { std::cerr << "Failure at line " << __LINE__ << '\n'; return EXIT_FAILURE; }
        if (skeleton.find_bone_index("LeftArm") != 2) { std::cerr << "Failure at line " << __LINE__ << '\n'; return EXIT_FAILURE; }

        // Clip as AnimationClip keyframes.
        const AnimationClip animationClip = parser.make_clip();
        if (animationClip.tracks.empty() || animationClip.tracks[0].keyFrames.size() != 2) { std::cerr << "Failure at line " << __LINE__ << '\n'; return EXIT_FAILURE; }
        if (std::abs(animationClip.tracks[0].keyFrames[1].position.y - 2.0f) > 1e-4f) { std::cerr << "Failure at line " << __LINE__ << '\n'; return EXIT_FAILURE; }

        // GPU skinning buffer: bind pose bone matrices (identity chain) and
        // a skinned vertex shader with the right bone count.
        const Pose bindPose = AnimationSampler::bind_pose(skeleton);
        const std::vector<glm::mat4> boneMatrices = GpuSkinningBuffer::compute_bone_matrices(skeleton, bindPose);
        if (boneMatrices.size() != 3) { std::cerr << "Failure at line " << __LINE__ << '\n'; return EXIT_FAILURE; }
        const std::vector<float> packed = GpuSkinningBuffer::pack(boneMatrices);
        if (packed.size() != 3 * 16) { std::cerr << "Failure at line " << __LINE__ << '\n'; return EXIT_FAILURE; }
        const std::string shader = GpuSkinningBuffer::skinned_vertex_shader(3);
        if (shader.find("mat4 bones[3]") == std::string::npos) { std::cerr << "Failure at line " << __LINE__ << '\n'; return EXIT_FAILURE; }
        if (shader.find("inJoints") == std::string::npos || shader.find("inWeights") == std::string::npos) { std::cerr << "Failure at line " << __LINE__ << '\n'; return EXIT_FAILURE; }
    }

    // Audio importer: compressed formats decode to float PCM with real
    // sample rate / channel metadata in the cooked asset.
    {
        const std::filesystem::path audioRoot = temporary / "audio_ogg";
        std::error_code aec;
        std::filesystem::remove_all(audioRoot, aec);
        std::filesystem::create_directories(audioRoot / "cooked", aec);
        // Synthetic WAV (miniaudio decodes WAV through the same path used for OGG).
        const std::filesystem::path wavPath = audioRoot / "tone.wav";
        const uint32_t sampleRate = 8000, dataSize = sampleRate * 2;
        std::vector<uint8_t> wav;
        const auto appendStr = [&](const std::string& s) { wav.insert(wav.end(), s.begin(), s.end()); };
        const auto append32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) wav.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF)); };
        const auto append16 = [&](uint16_t v) { for (int i = 0; i < 2; ++i) wav.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF)); };
        appendStr("RIFF"); append32(36 + dataSize); appendStr("WAVE");
        appendStr("fmt "); append32(16); append16(1); append16(1); append32(sampleRate);
        append32(sampleRate * 2); append16(2); append16(16);
        appendStr("data"); append32(dataSize);
        for (uint32_t i = 0; i < sampleRate; ++i) { const int16_t s = static_cast<int16_t>(std::sin(i * 0.05) * 8000); append16(static_cast<uint16_t>(s)); }
        {
            std::ofstream out(wavPath, std::ios::binary);
            out.write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
        }
        // Route through the AudioImporter with the compressed path by renaming
        // the source extension to .ogg (decoder is format-agnostic here).
        AssetRegistry audioReg;
        AssetPipeline audioPipeline(audioReg);
        audioPipeline.add_importer(std::make_unique<AudioImporter>());
        const std::filesystem::path oggPath = audioRoot / "tone.ogg";
        std::filesystem::copy_file(wavPath, oggPath, std::filesystem::copy_options::overwrite_existing, aec);
        const ImportResult audioResult = audioPipeline.import({oggPath, audioRoot / "cooked", 1});
        if (!audioResult) { std::cerr << "Failure at line " << __LINE__ << ": " << audioResult.error << '\n'; return EXIT_FAILURE; }
        if (audioResult.asset.sampleRate != sampleRate || audioResult.asset.audioChannels != 1) { std::cerr << "Failure at line " << __LINE__ << '\n'; return EXIT_FAILURE; }
        if (audioResult.asset.durationSeconds < 0.9f || audioResult.asset.durationSeconds > 1.1f) { std::cerr << "Failure at line " << __LINE__ << " dur=" << audioResult.asset.durationSeconds << '\n'; return EXIT_FAILURE; }
        std::ifstream cookedAudio(audioResult.asset.cookedPath, std::ios::binary);
        const std::vector<uint8_t> cookedBytes((std::istreambuf_iterator<char>(cookedAudio)), {});
        if (cookedBytes.size() < 7 + 4 + 4 + 2 + 2 + 4 + 8) { std::cerr << "Failure at line " << __LINE__ << '\n'; return EXIT_FAILURE; }
        // formatVersion == 2 (float PCM).
        uint32_t cookedFormat{};
        std::memcpy(&cookedFormat, cookedBytes.data() + 7, 4);
        if (cookedFormat != 2) { std::cerr << "Failure at line " << __LINE__ << " fmt=" << cookedFormat << '\n'; return EXIT_FAILURE; }
        std::filesystem::remove_all(audioRoot, aec);
    }

    // Asset Registry migration: an old v3/v5 database loads and re-saves as v7.
    std::cerr << "[CP] migration start\n";
    {
        const std::filesystem::path legacyRoot =
            std::filesystem::temp_directory_path() /
            ("legacy_assets_migration_" + std::to_string(_getpid()));
        std::error_code lec;
        std::filesystem::remove_all(legacyRoot, lec);
        std::filesystem::create_directories(legacyRoot);

        const UUID legacyId(0x1111, 0x2222);
        const std::filesystem::path legacyDb = legacyRoot / "AssetRegistry.db";
        {
            std::ofstream out(legacyDb, std::ios::trunc);
            // v5 line format: id type source cooked hash importerVersion cooked
            // boneCount animationTrackCount animationKeyframeCount  (v5 = up to bone fields,
            // before settingsHash/mipmaps/srgb — textureQuality/meshScale also absent).
            out << "VulkanEngine.AssetRegistry 5\n";
            out << std::quoted(legacyId.to_string()) << " 1 "
                << std::quoted("Assets/hero.fbx") << " "
                << std::quoted("Intermediate/hero.vcmesh") << " "
                << 12345ull << " 3 1 "
                // width height channels primitiveCount | vertexCount indexCount |
                // sampleRate audioChannels durationSeconds | boneCount animationTrackCount animationKeyframeCount | dependencyCount
                << "64 64 4 1 1000 2000 44100 2 1.5 42 3 120 0\n";
            out << std::quoted(UUID(0x3333, 0x4444).to_string()) << " 2 "
                << std::quoted("Assets/dirt.tga") << " "
                << std::quoted("Intermediate/dirt.vctex") << " "
                << 99ull << " 1 1 "
                << "128 128 3 1 0 0 0 0 0 0 0 0 0\n";
        }
        AssetRegistry legacy;
        if (!legacy.load(legacyDb)) { std::cerr << "Failure at line " << __LINE__ << " v5 load\n"; return EXIT_FAILURE; }
        const auto legacyAssets = legacy.snapshot();
        if (legacyAssets.size() != 2) { std::cerr << "Failure at line " << __LINE__ << " count=" << legacyAssets.size() << '\n'; return EXIT_FAILURE; }
        // Migrated metadata: defaults filled in for missing v6/v7 fields.
        for (const auto& asset : legacyAssets) {
            if (asset.importSettings.textureQuality != 100 || asset.importSettings.meshScale != 1.0f) {
                std::cerr << "Failure at line " << __LINE__ << " defaults\n";
                return EXIT_FAILURE;
            }
        }
        // Re-save migrates the file to v7.
        const std::filesystem::path migratedDb = legacyRoot / "Migrated.db";
        if (!legacy.save(migratedDb)) { std::cerr << "Failure at line " << __LINE__ << " save\n"; return EXIT_FAILURE; }
        std::ifstream headerIn(migratedDb);
        std::string headerLine;
        std::getline(headerIn, headerLine);
        if (headerLine != "VulkanEngine.AssetRegistry 7") { std::cerr << "Failure at line " << __LINE__ << " header='" << headerLine << "'\n"; return EXIT_FAILURE; }
        AssetRegistry reloaded;
        if (!reloaded.load(migratedDb)) { std::cerr << "Failure at line " << __LINE__ << " v7 reload\n"; return EXIT_FAILURE; }
        if (reloaded.snapshot().size() != 2) { std::cerr << "Failure at line " << __LINE__ << " reload count\n"; return EXIT_FAILURE; }
        // Unsupported version still rejected.
        {
            std::ofstream out(legacyRoot / "Bad.db", std::ios::trunc);
            out << "VulkanEngine.AssetRegistry 99\n";
        }
        AssetRegistry bad;
        if (bad.load(legacyRoot / "Bad.db")) { std::cerr << "Failure at line " << __LINE__ << " bad version accepted\n"; return EXIT_FAILURE; }

        std::filesystem::remove_all(legacyRoot, lec);
    }

    // Thumbnail cache: generation on miss, memory reuse, disk persistence and
    // invalidation by source hash.
    std::cerr << "[CP] thumbnail start\n";
    {
        using namespace Engine::Assets;
        const std::filesystem::path thumbRoot =
            std::filesystem::temp_directory_path() /
            ("thumbnail_cache_tests_" + std::to_string(_getpid()));
        std::error_code tec;
        std::filesystem::remove_all(thumbRoot, tec);

        ThumbnailCache::Options topts;
        topts.size = 32;
        topts.cacheDirectory = thumbRoot;
        topts.persist = true;
        ThumbnailCache cache(topts);

        int generations = 0;
        auto generator = [&](std::uint32_t w, std::uint32_t h, std::vector<std::uint8_t>& rgba) {
            ++generations;
            ThumbnailCache::generate_checkerboard(w, rgba);
            rgba.resize(static_cast<std::size_t>(w) * h * 4);
        };

        // First get generates and caches.
        auto thumb = cache.get("Assets/hero.png", 1001, generator);
        if (!thumb || !thumb->valid() || thumb->width != 32 || thumb->height != 32) { std::cerr << "Failure at line " << __LINE__ << " generate\n"; return EXIT_FAILURE; }
        if (generations != 1) { std::cerr << "Failure at line " << __LINE__ << " gen count\n"; return EXIT_FAILURE; }
        // Same hash → served from memory, no regeneration.
        auto again = cache.get("Assets/hero.png", 1001, generator);
        if (!again || generations != 1) { std::cerr << "Failure at line " << __LINE__ << " cache hit\n"; return EXIT_FAILURE; }
        // Content hash changed → regenerated.
        auto stale = cache.get("Assets/hero.png", 1002, generator);
        if (!stale || generations != 2) { std::cerr << "Failure at line " << __LINE__ << " invalidation\n"; return EXIT_FAILURE; }

        // Fresh cache instance loads from disk (no regeneration).
        int diskGens = 0;
        auto diskGenerator = [&](std::uint32_t w, std::uint32_t h, std::vector<std::uint8_t>& rgba) {
            ++diskGens;
            rgba.assign(static_cast<std::size_t>(w) * h * 4, 0);
        };
        ThumbnailCache diskCache(topts);
        auto fromDisk = diskCache.get("Assets/hero.png", 1002, diskGenerator);
        if (!fromDisk || diskGens != 0) { std::cerr << "Failure at line " << __LINE__ << " disk load (gens=" << diskGens << ")\n"; return EXIT_FAILURE; }
        if (fromDisk->rgba != stale->rgba) { std::cerr << "Failure at line " << __LINE__ << " disk bytes\n"; return EXIT_FAILURE; }

        // Memory accounting.
        if (cache.memory_entries() != 1 || cache.memory_bytes() == 0) { std::cerr << "Failure at line " << __LINE__ << " accounting\n"; return EXIT_FAILURE; }
        cache.clear_memory();
        if (cache.memory_entries() != 0) { std::cerr << "Failure at line " << __LINE__ << " clear\n"; return EXIT_FAILURE; }

        std::filesystem::remove_all(thumbRoot, tec);
    }

    // Plugin runtime: manifest validation, factory loading, dependency-ordered
    // startup, API-version rejection, reverse-order unload and hot reload.
    std::cerr << "[CP] plugin start\n";
    {
        using namespace Engine::Plugins;
        PluginRuntime runtime;

        struct SpyPlugin final : IPluginRuntime {
            explicit SpyPlugin(std::string n) : name_(std::move(n)) {}
            std::string name_;
            int loads{0}, unloads{0}, updates{0};
            const std::string& name() const override { return name_; }
            const std::string& version() const override { static const std::string v = "1.0"; return v; }
            bool on_load(std::string&) override { ++loads; return true; }
            void on_unload() override { ++unloads; }
            void update(double) override { ++updates; }
        };

        runtime.register_factory("Base", [](const PluginManifest&) {
            return std::shared_ptr<IPluginRuntime>(new SpyPlugin("Base"));
        });
        runtime.register_factory("Dep", [](const PluginManifest&) {
            return std::shared_ptr<IPluginRuntime>(new SpyPlugin("Dep"));
        });
        runtime.register_factory("App", [](const PluginManifest&) {
            return std::shared_ptr<IPluginRuntime>(new SpyPlugin("App"));
        });

        PluginManifest base;
        base.name = "Base";
        base.apiVersion = 1;
        PluginManifest dep;
        dep.name = "Dep";
        dep.dependencies = {"Base"};
        PluginManifest app;
        app.name = "App";
        app.dependencies = {"Dep"};

        std::string error;
        std::cerr << "[CP] p1\n";
        if (!runtime.load_manifest(base, &error)) { std::cerr << "Failure at line " << __LINE__ << " base: " << error << '\n'; return EXIT_FAILURE; }
        std::cerr << "[CP] p2\n";
        if (!runtime.load_manifest(dep, &error)) { std::cerr << "Failure at line " << __LINE__ << " dep: " << error << '\n'; return EXIT_FAILURE; }
        std::cerr << "[CP] p3\n";
        if (!runtime.load_manifest(app, &error)) { std::cerr << "Failure at line " << __LINE__ << " app: " << error << '\n'; return EXIT_FAILURE; }
        std::cerr << "[CP] p4 count=" << runtime.count() << "\n";
        if (runtime.count() != 3 || !runtime.is_loaded("Base") || !runtime.is_loaded("Dep") || !runtime.is_loaded("App")) {
            std::cerr << "Failure at line " << __LINE__ << " loaded state\n"; return EXIT_FAILURE;
        }
        std::cerr << "[CP] p5\n";
        // Dependency order: Base before Dep before App.
        std::vector<std::string> order;
        std::cerr << "[CP] p6\n";
        if (!runtime.compute_load_order(order, &error)) { std::cerr << "Failure at line " << __LINE__ << " order: " << error << '\n'; return EXIT_FAILURE; }
        std::cerr << "[CP] p7\n";
        auto posOf = [&](const std::string& n) {
            return static_cast<std::ptrdiff_t>(std::find(order.begin(), order.end(), n) - order.begin());
        };
        if (posOf("Base") >= posOf("Dep") || posOf("Dep") >= posOf("App")) {
            std::cerr << "Failure at line " << __LINE__ << " order wrong\n"; return EXIT_FAILURE;
        }
        std::cerr << "[CP] p8\n";

        // update_all ticks each loaded plugin.
        runtime.update_all(0.016);
        std::cerr << "[CP] p9\n";
        const LoadedPlugin* appPlugin = runtime.plugin("App");
        std::cerr << "[CP] p10\n";
        if (!appPlugin) { std::cerr << "Failure at line " << __LINE__ << '\n'; return EXIT_FAILURE; }
        // Avoid RTTI dynamic_cast on the function-local spy type (MSVC quirk);
        // verify via the runtime's public name() instead.
        if (appPlugin->runtime->name() != "App") { std::cerr << "Failure at line " << __LINE__ << " name\n"; return EXIT_FAILURE; }
        std::cerr << "[CP] p11\n";

        // API version too new → rejected.
        PluginManifest future;
        future.name = "Future";
        future.apiVersion = 999;
        if (runtime.load_manifest(future, &error)) { std::cerr << "Failure at line " << __LINE__ << " future accepted\n"; return EXIT_FAILURE; }
        std::cerr << "[CP] p12\n";

        // Unload App → Dep still loaded; unload Dep → Base still loaded.
        if (!runtime.unload("App", &error)) { std::cerr << "Failure at line " << __LINE__ << ": " << error << '\n'; return EXIT_FAILURE; }
        std::cerr << "[CP] p13\n";
        if (runtime.is_loaded("App") || !runtime.is_loaded("Dep")) { std::cerr << "Failure at line " << __LINE__ << " unload app\n"; return EXIT_FAILURE; }
        std::cerr << "[CP] p14\n";

        // Hot reload App keeps identity and re-runs on_load.
        if (!runtime.reload("App", &error)) { std::cerr << "Failure at line " << __LINE__ << ": " << error << '\n'; return EXIT_FAILURE; }
        std::cerr << "[CP] p15\n";
        const LoadedPlugin* reloaded = runtime.plugin("App");
        if (!reloaded || reloaded->runtime->name() != "App") { std::cerr << "Failure at line " << __LINE__ << '\n'; return EXIT_FAILURE; }
        std::cerr << "[CP] p16\n";

        // Cycle rejection.
        PluginRuntime cyclic;
        cyclic.register_factory("A", [](const PluginManifest&) { return std::make_shared<SpyPlugin>("A"); });
        cyclic.register_factory("B", [](const PluginManifest&) { return std::make_shared<SpyPlugin>("B"); });
        PluginManifest a, b;
        a.name = "A"; a.dependencies = {"B"};
        b.name = "B"; b.dependencies = {"A"};
        std::cerr << "[CP] p17\n";
        if (cyclic.load_manifest(a, &error)) { std::cerr << "Failure at line " << __LINE__ << " a loaded\n"; return EXIT_FAILURE; }
        std::cerr << "[CP] p18\n";
        if (cyclic.load_manifest(b, &error)) { std::cerr << "Failure at line " << __LINE__ << " b loaded\n"; return EXIT_FAILURE; }
        std::cerr << "[CP] p19\n";
        std::vector<std::string> cyclicOrder;
        if (cyclic.compute_load_order(cyclicOrder, &error)) { std::cerr << "Failure at line " << __LINE__ << " cycle not detected\n"; return EXIT_FAILURE; }

        // Manifest file roundtrip.
        const std::filesystem::path pluginRoot = std::filesystem::temp_directory_path() /
            ("plugin_runtime_tests_" + std::to_string(_getpid()));
        std::error_code pec;
        std::filesystem::remove_all(pluginRoot, pec);
        PluginManifest fileManifest;
        fileManifest.name = "FilePlugin";
        fileManifest.version = "2.1.0";
        fileManifest.apiVersion = 1;
        fileManifest.dependencies = {"Base"};
        fileManifest.editorOnly = true;
        const auto manifestPath = pluginRoot / "plugin.mf";
        if (!fileManifest.save_to_file(manifestPath)) { std::cerr << "Failure at line " << __LINE__ << " manifest save\n"; return EXIT_FAILURE; }
        PluginManifest loadedManifest;
        if (!loadedManifest.load_from_file(manifestPath)) { std::cerr << "Failure at line " << __LINE__ << " manifest load\n"; return EXIT_FAILURE; }
        if (loadedManifest.name != "FilePlugin" || loadedManifest.version != "2.1.0" ||
            loadedManifest.dependencies.size() != 1 || !loadedManifest.editorOnly) {
            std::cerr << "Failure at line " << __LINE__ << " manifest roundtrip\n"; return EXIT_FAILURE;
        }
        std::filesystem::remove_all(pluginRoot, pec);
    }

    // ── glTF animation extraction: accessor-based TRS channels → clip ──
    {
        // Data-URI glTF with a rotation channel on node "BoneB": times
        // [0, 1] (SCALAR) and quaternion outputs [identity, 90° around Z]
        // (VEC4) stored in a binary accessor.
        std::vector<uint8_t> buffer;
        append_f32(buffer, 0.0f);
        append_f32(buffer, 1.0f);
        // glTF quaternions are (x, y, z, w): identity = (0,0,0,1), Rz(90) =
        // (0, 0, sin45, cos45) = (0, 0, 0.7071, 0.7071).
        append_f32(buffer, 0.0f); append_f32(buffer, 0.0f); append_f32(buffer, 0.0f); append_f32(buffer, 1.0f);
        append_f32(buffer, 0.0f); append_f32(buffer, 0.0f); append_f32(buffer, 0.7071068f); append_f32(buffer, 0.7071068f);
        const std::string b64 = base64_encode(buffer);
        const std::string json = R"({
          "asset": {"version": "2.0"},
          "buffers": [{"byteLength": 40, "uri": "data:application/octet-stream;base64,)" + b64 + R"("}],
          "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 8},
            {"buffer": 0, "byteOffset": 8, "byteLength": 32}
          ],
          "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 2, "type": "SCALAR"},
            {"bufferView": 1, "componentType": 5126, "count": 2, "type": "VEC4"}
          ],
          "nodes": [
            {"name": "BoneA", "children": [1]},
            {"name": "BoneB"}
          ],
          "skins": [{"name": "TestSkin", "joints": [0, 1]}],
          "animations": [{
            "name": "Wave",
            "samplers": [{"input": 0, "output": 1}],
            "channels": [{"sampler": 0, "target": {"node": 1, "path": "rotation"}}]
          }]
        })";
        GltfParser parser(json, {});
        std::string error;
        if (!parser.parse(&error)) {
            std::cerr << "Failure at line " << __LINE__ << " parser: " << error << "\n";
            return EXIT_FAILURE;
        }
        const std::vector<GltfAnimationClip>& clips = parser.clips();
        if (clips.size() != 1 || clips[0].channels.size() != 1) {
            std::cerr << "Failure at line " << __LINE__ << " clip/channel count\n";
            return EXIT_FAILURE;
        }
        const GltfAnimationChannel& channel = clips[0].channels[0];
        if (channel.nodeName != "BoneB" || channel.times.size() != 2 ||
            !near(channel.times[1], 1.0f) || channel.rotations.size() != 2) {
            std::cerr << "Failure at line " << __LINE__ << " channel data\n";
            return EXIT_FAILURE;
        }
        const glm::vec3 rotated = glm::mat3_cast(channel.rotations[1]) * glm::vec3(1.0f, 0.0f, 0.0f);
        if (!near(rotated.x, 0.0f) || !near(rotated.y, 1.0f)) {
            std::cerr << "Failure at line " << __LINE__ << " rotation output ("
                      << rotated.x << ", " << rotated.y << ")\n";
            return EXIT_FAILURE;
        }

        // make_clip maps the channel to the skeleton bone and samples halfway:
        // slerp(identity, Rz90, 0.5) = Rz45 → (1,0,0) becomes (√2/2, √2/2, 0).
        const SkeletonAsset animSkeleton = parser.make_skeleton();
        const AnimationClip animClip = parser.make_clip();
        const Pose sampled = AnimationSampler::sample(animSkeleton, animClip, 0.5f);
        if (sampled.local.size() != 2) {
            std::cerr << "Failure at line " << __LINE__ << " sampled pose size\n";
            return EXIT_FAILURE;
        }
        const glm::vec3 halfway = glm::mat3_cast(sampled.local[1].rotation) * glm::vec3(1.0f, 0.0f, 0.0f);
        if (!near(halfway.x, 0.7071f) || !near(halfway.y, 0.7071f)) {
            std::cerr << "Failure at line " << __LINE__ << " sampled rotation ("
                      << halfway.x << ", " << halfway.y << ")\n";
            return EXIT_FAILURE;
        }
    }

    // ── GpuSkinningBuffer: bone matrices → packed UBO → generated shader ──
    {
        SkeletonAsset skeleton;
        skeleton.name = "Flag";
        BoneNode root;
        root.name = "Root";
        root.parentIndex = -1;
        root.localTransform = glm::mat4(1.0f);
        root.inverseBindMatrix = glm::inverse(root.localTransform);
        skeleton.bones.push_back(root);
        BoneNode tip;
        tip.name = "Tip";
        tip.parentIndex = 0;
        tip.localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        tip.inverseBindMatrix = glm::inverse(tip.localTransform);
        skeleton.bones.push_back(tip);

        // Bind pose: bone 0 = identity, bone 1 = parent * local = T(1,0,0).
        const Pose bindPose = AnimationSampler::bind_pose(skeleton);
        const std::vector<glm::mat4> bind = GpuSkinningBuffer::compute_bone_matrices(skeleton, bindPose);
        if (bind.size() != 2) { std::cerr << "Failure at line " << __LINE__ << " bind matrix count\n"; return EXIT_FAILURE; }
        if (!near(bind[0][3][0], 0.0f) || !near(bind[0][3][1], 0.0f)) {
            std::cerr << "Failure at line " << __LINE__ << " root bind offset\n"; return EXIT_FAILURE;
        }
        if (!near(bind[1][3][0], 1.0f) || !near(bind[1][3][1], 0.0f)) {
            std::cerr << "Failure at line " << __LINE__ << " tip bind offset\n"; return EXIT_FAILURE;
        }

        // Animated pose: the tip rotates 90° around Z. The pose overrides the
        // bone's local transform (translation 0), so bone 1 pivots at the
        // origin and its local X axis maps to world +Y.
        Pose pose = bindPose;
        pose.local[1].rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
        const std::vector<glm::mat4> animated = GpuSkinningBuffer::compute_bone_matrices(skeleton, pose);
        if (!near(animated[1][0][0], 0.0f) || !near(animated[1][0][1], 1.0f)) {
            std::cerr << "Failure at line " << __LINE__ << " animated bone X axis ("
                      << animated[1][0][0] << ", " << animated[1][0][1] << ")\n";
            return EXIT_FAILURE;
        }

        // Pack: 16 column-major floats per bone; column 0 of an identity bone
        // is (1,0,0,0).
        const std::vector<float> packed = GpuSkinningBuffer::pack(bind);
        if (packed.size() != 32) { std::cerr << "Failure at line " << __LINE__ << " pack size\n"; return EXIT_FAILURE; }
        if (!near(packed[0], 1.0f) || !near(packed[1], 0.0f) || !near(packed[3], 0.0f)) {
            std::cerr << "Failure at line " << __LINE__ << " pack identity column\n"; return EXIT_FAILURE;
        }
        if (!near(packed[16 + 12], 1.0f) || !near(packed[16 + 13], 0.0f)) {
            std::cerr << "Failure at line " << __LINE__ << " pack tip translation\n"; return EXIT_FAILURE;
        }

        // Generated shader: skin blend + world normal output for lighting.
        const std::string shader = GpuSkinningBuffer::skinned_vertex_shader(64);
        if (shader.find("mat4 bones[64]") == std::string::npos ||
            shader.find("inJoints") == std::string::npos ||
            shader.find("vWorldPos") == std::string::npos ||
            shader.find("vNormal = mat3(skin) * inNormal") == std::string::npos) {
            std::cerr << "Failure at line " << __LINE__ << " generated shader contents\n";
            return EXIT_FAILURE;
        }
    }

    // ── Visual scripting: save/load JSON, FunctionCall, scopes, hot reload ──
    {
        // ScriptGraphAsset -> JSON -> back (roundtrip preserves kind/literal).
        ScriptGraphAsset graph;
        graph.name = "Test Graph";
        graph.nodes.push_back(TypedScriptNode{ UUID(), ScriptNodeKind::Event, "OnStart" });
        graph.nodes.push_back(TypedScriptNode{ UUID(), ScriptNodeKind::ConstantFloat, "", "", 3.5 });
        graph.nodes.push_back(TypedScriptNode{ UUID(), ScriptNodeKind::SetVariable, "", "value" });
        graph.nodes.push_back(TypedScriptNode{ UUID(), ScriptNodeKind::Function, "Helper" });
        graph.nodes.push_back(TypedScriptNode{ UUID(), ScriptNodeKind::Log, "", "hello", 1.0 });
        const std::filesystem::path scriptFile = temporary / "graph.script";
        if (!graph.save(scriptFile)) { std::cerr << "line " << __LINE__ << " script save\n"; return EXIT_FAILURE; }
        ScriptGraphAsset loadedGraph;
        if (!loadedGraph.load(scriptFile) || loadedGraph.name != "Test Graph" ||
            loadedGraph.nodes.size() != 5 || loadedGraph.nodes[1].kind != ScriptNodeKind::ConstantFloat ||
            !std::holds_alternative<double>(loadedGraph.nodes[1].literal) ||
            std::abs(std::get<double>(loadedGraph.nodes[1].literal) - 3.5) > 1e-9 ||
            loadedGraph.nodes[3].kind != ScriptNodeKind::Function ||
            loadedGraph.nodes[3].event != "Helper") {
            std::cerr << "line " << __LINE__ << " script roundtrip\n"; return EXIT_FAILURE;
        }

        // FunctionCall: the caller pushes a return address; execution resumes
        // after the call and the function body is reachable by name.
        ScriptGraphAsset fnGraph;
        fnGraph.nodes.push_back(TypedScriptNode{ UUID(), ScriptNodeKind::Event, "OnStart" });
        fnGraph.nodes.push_back(TypedScriptNode{ UUID(), ScriptNodeKind::FunctionCall, "Init" });
        fnGraph.nodes.push_back(TypedScriptNode{ UUID(), ScriptNodeKind::ConstantFloat, "", "", 9.0 });
        fnGraph.nodes.push_back(TypedScriptNode{ UUID(), ScriptNodeKind::SetVariable, "", "after" });
        fnGraph.nodes.push_back(TypedScriptNode{ UUID(), ScriptNodeKind::Function, "Init" });
        fnGraph.nodes.push_back(TypedScriptNode{ UUID(), ScriptNodeKind::ConstantFloat, "", "", 7.0 });
        fnGraph.nodes.push_back(TypedScriptNode{ UUID(), ScriptNodeKind::SetVariable, "", "n" });
        fnGraph.nodes.push_back(TypedScriptNode{ UUID(), ScriptNodeKind::Return });
        const auto fnCompiled = ScriptCompiler::compile(fnGraph);
        if (!fnCompiled) { std::cerr << "line " << __LINE__ << " function compile\n"; return EXIT_FAILURE; }
        ScriptVM fnVm;
        fnVm.load(std::move(fnCompiled.program));
        fnVm.start_event("OnStart");
        fnVm.run(0.0f, 10000);
        if (fnVm.status() != VMStatus::Completed ||
            !near(static_cast<float>(fnVm.float_variable("n")), 7.0f) ||
            !near(static_cast<float>(fnVm.float_variable("after")), 9.0f)) {
            std::cerr << "line " << __LINE__ << " function call semantics (n="
                      << fnVm.float_variable("n") << ", after=" << fnVm.float_variable("after") << ")\n";
            return EXIT_FAILURE;
        }

        // Scopes: a variable stored inside a scope frame does not leak out;
        // a later global store wins after the frame is popped.
        ScriptGraphAsset scopeGraph;
        scopeGraph.nodes.push_back(TypedScriptNode{ UUID(), ScriptNodeKind::Event, "OnStart" });
        scopeGraph.nodes.push_back(TypedScriptNode{ UUID(), ScriptNodeKind::Scope });
        scopeGraph.nodes.push_back(TypedScriptNode{ UUID(), ScriptNodeKind::ConstantFloat, "", "", 3.0 });
        scopeGraph.nodes.push_back(TypedScriptNode{ UUID(), ScriptNodeKind::SetVariable, "", "v" });
        scopeGraph.nodes.push_back(TypedScriptNode{ UUID(), ScriptNodeKind::ScopeEnd });
        scopeGraph.nodes.push_back(TypedScriptNode{ UUID(), ScriptNodeKind::ConstantFloat, "", "", 9.0 });
        scopeGraph.nodes.push_back(TypedScriptNode{ UUID(), ScriptNodeKind::SetVariable, "", "v" });
        const auto scopeCompiled = ScriptCompiler::compile(scopeGraph);
        if (!scopeCompiled) { std::cerr << "line " << __LINE__ << " scope compile\n"; return EXIT_FAILURE; }
        ScriptVM scopeVm;
        scopeVm.load(std::move(scopeCompiled.program));
        scopeVm.start_event("OnStart");
        scopeVm.run(0.0f, 10000);
        if (!near(static_cast<float>(scopeVm.float_variable("v")), 9.0f)) {
            std::cerr << "line " << __LINE__ << " scope semantics\n"; return EXIT_FAILURE;
        }

        // Hot reload: watch a .script, rewrite it, reload the program — the
        // variable map survives the swap and the new program takes over.
        ScriptVM hotVm;
        ScriptGraphAsset hotA;
        hotA.nodes.push_back(TypedScriptNode{ UUID(), ScriptNodeKind::Event, "OnStart" });
        hotA.nodes.push_back(TypedScriptNode{ UUID(), ScriptNodeKind::ConstantFloat, "", "", 1.0 });
        hotA.nodes.push_back(TypedScriptNode{ UUID(), ScriptNodeKind::SetVariable, "", "x" });
        const auto hotCompiled = ScriptCompiler::compile(hotA);
        if (!hotCompiled) { std::cerr << "line " << __LINE__ << " hot compile\n"; return EXIT_FAILURE; }
        hotVm.load(std::move(hotCompiled.program));
        hotVm.start_event("OnStart");
        hotVm.run(0.0f, 10000);
        if (!near(static_cast<float>(hotVm.float_variable("x")), 1.0f)) { std::cerr << "line " << __LINE__ << " hot init\n"; return EXIT_FAILURE; }
        const std::filesystem::path hotFile = temporary / "hot.script";
        ScriptHotReloader reloader;
        if (!reloader.watch(hotFile)) { std::cerr << "line " << __LINE__ << " hot watch\n"; return EXIT_FAILURE; }
        hotVm.set_variable("x", 42.0);
        ScriptGraphAsset hotB;
        hotB.nodes.push_back(TypedScriptNode{ UUID(), ScriptNodeKind::Event, "OnStart" });
        hotB.nodes.push_back(TypedScriptNode{ UUID(), ScriptNodeKind::ConstantFloat, "", "", 5.0 });
        hotB.nodes.push_back(TypedScriptNode{ UUID(), ScriptNodeKind::SetVariable, "", "x" });
        if (!hotB.save(hotFile)) { std::cerr << "line " << __LINE__ << " hot save\n"; return EXIT_FAILURE; }
        std::string hotError;
        if (!reloader.reload_if_changed(hotVm, &hotError)) { std::cerr << "line " << __LINE__ << " hot reload: " << hotError << "\n"; return EXIT_FAILURE; }
        if (!near(static_cast<float>(hotVm.float_variable("x")), 42.0f)) { std::cerr << "line " << __LINE__ << " hot vars\n"; return EXIT_FAILURE; }
        hotVm.start_event("OnStart");
        hotVm.run(0.0f, 10000);
        if (!near(static_cast<float>(hotVm.float_variable("x")), 5.0f)) { std::cerr << "line " << __LINE__ << " hot program\n"; return EXIT_FAILURE; }
    }

    // ── VM debugger hooks: breakpoint pauses run(); run() resumes from
    // Paused (Continue); program() exposes the bytecode for the panel. ──
    {
        ScriptGraphAsset bpGraph;
        bpGraph.nodes.push_back(TypedScriptNode{ UUID(), ScriptNodeKind::Event, "OnStart" });
        bpGraph.nodes.push_back(TypedScriptNode{ UUID(), ScriptNodeKind::ConstantFloat, "", "", 1.0 });
        bpGraph.nodes.push_back(TypedScriptNode{ UUID(), ScriptNodeKind::SetVariable, "", "x" });
        bpGraph.nodes.push_back(TypedScriptNode{ UUID(), ScriptNodeKind::ConstantFloat, "", "", 2.0 });
        bpGraph.nodes.push_back(TypedScriptNode{ UUID(), ScriptNodeKind::SetVariable, "", "y" });
        const auto bpCompiled = ScriptCompiler::compile(bpGraph);
        if (!bpCompiled) { std::cerr << "line " << __LINE__ << " bp compile\n"; return EXIT_FAILURE; }
        ScriptVM bpVm;
        bpVm.load(std::move(bpCompiled.program));
        if (bpVm.program().instructions.empty() || bpVm.program().eventEntries.empty()) {
            std::cerr << "line " << __LINE__ << " bp program access\n"; return EXIT_FAILURE;
        }
        // Breakpoint on the PushFloat 2 (index 3): the VM pauses right after
        // it executes, with x stored and y still unset, then Continue resumes.
        const size_t last = bpVm.program().instructions.size() - 1; // Return
        bpVm.add_breakpoint(last - 2);
        if (!bpVm.start_event("OnStart")) { std::cerr << "line " << __LINE__ << " bp start\n"; return EXIT_FAILURE; }
        const VMStatus pausedAt = bpVm.run(0.0f, 10000);
        if (pausedAt != VMStatus::Paused || bpVm.instruction_pointer() != last - 1) {
            std::cerr << "line " << __LINE__ << " bp pause (status="
                      << static_cast<int>(pausedAt) << " ip=" << bpVm.instruction_pointer() << ")\n";
            return EXIT_FAILURE;
        }
        if (!near(static_cast<float>(bpVm.float_variable("x")), 1.0f) ||
            !near(static_cast<float>(bpVm.float_variable("y")), 0.0f)) {
            std::cerr << "line " << __LINE__ << " bp x/y at pause\n"; return EXIT_FAILURE;
        }
        const VMStatus resumed = bpVm.run(0.0f, 10000); // Continue resumes from Paused
        if (resumed != VMStatus::Completed ||
            !near(static_cast<float>(bpVm.float_variable("y")), 2.0f)) {
            std::cerr << "line " << __LINE__ << " bp resume (status="
                      << static_cast<int>(resumed) << " y=" << bpVm.float_variable("y") << ")\n";
            return EXIT_FAILURE;
        }
        if (std::string_view(script_opcode_name(OpCode::Call)) != "Call" ||
            std::string_view(script_node_kind_name(ScriptNodeKind::FunctionCall)) != "FunctionCall") {
            std::cerr << "line " << __LINE__ << " opcode names\n"; return EXIT_FAILURE;
        }
    }

    // ── JPEG baseline decoder via TextureImporter (8x8 gray, DC-only) ──
    {
        // Minimal valid baseline JPEG: uniform gray 128. One 1-bit DC code for
        // category 0 and one 1-bit AC code for EOB make the entropy data two
        // zero bits; all coefficients are zero, so every pixel decodes to 128.
        std::vector<uint8_t> jpg;
        const auto push = [&](std::initializer_list<uint8_t> bytes) {
            jpg.insert(jpg.end(), bytes.begin(), bytes.end());
        };
        push({ 0xFF, 0xD8 });                                    // SOI
        push({ 0xFF, 0xDB, 0x00, 0x43, 0x00 });                  // DQT, table 0
        for (int i = 0; i < 64; ++i) jpg.push_back(16);
        push({ 0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00, 0x08, 0x00, 0x08, 0x01, 0x01, 0x11, 0x00 }); // SOF0
        // info=00, counts[0]=01 (one 1-bit code), counts[1..15]=0, one symbol 00.
        // length 0x14 = 2 len + 1 info + 16 counts + 1 symbol.
        push({ 0xFF, 0xC4, 0x00, 0x14, 0x00, 0x01,               // DHT DC id 0
               0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00 });
        push({ 0xFF, 0xC4, 0x00, 0x14, 0x10, 0x01,               // DHT AC id 0 (EOB)
               0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00 });
        push({ 0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3F, 0x00 }); // SOS
        jpg.push_back(0x00);                                     // DC '0' + AC EOB '0'
        push({ 0xFF, 0xD9 });                                    // EOI

        const std::filesystem::path jpgPath = temporary / "gray.jpg";
        {
            std::ofstream f(jpgPath, std::ios::binary);
            f.write(reinterpret_cast<const char*>(jpg.data()), static_cast<std::streamsize>(jpg.size()));
        }
        AssetRegistry jpegRegistry;
        AssetPipeline jpegPipeline(jpegRegistry);
        jpegPipeline.add_importer(std::make_unique<TextureImporter>());
        const auto imported = jpegPipeline.import({ jpgPath, temporary / "cooked", 1 });
        if (!imported || imported.asset.type != AssetType::Texture ||
            imported.asset.width != 8 || imported.asset.height != 8) {
            std::cerr << "line " << __LINE__ << " jpeg import\n"; return EXIT_FAILURE;
        }
        // Cooked payload: VCTEX v3 header (5+4+4+4+4+1+4+1+8 = 35 bytes) +
        // the mip chain (generateMipmaps default): 8x8 + 4x4 + 2x2 + 1x1 RGB.
        std::ifstream cookedFile(imported.asset.cookedPath, std::ios::binary);
        std::vector<uint8_t> cooked((std::istreambuf_iterator<char>(cookedFile)), {});
        const size_t jpegChain = static_cast<size_t>(8) * 8 * 3 + 4 * 4 * 3 + 2 * 2 * 3 + 1 * 1 * 3;
        if (cooked.size() != 35 + jpegChain) {
            std::cerr << "line " << __LINE__ << " jpeg payload size " << cooked.size() << " (expected " << 35 + jpegChain << ")\n";
            return EXIT_FAILURE;
        }
        for (size_t i = 35; i < cooked.size(); ++i) {
            if (cooked[i] != 128) {
                std::cerr << "line " << __LINE__ << " jpeg pixel " << i << " = " << static_cast<int>(cooked[i]) << "\n";
                return EXIT_FAILURE;
            }
        }
    }

    // ── Import settings applied at cook time (Fase 2) ──
    {
        const std::filesystem::path settingsRoot = temporary / "settings";
        std::filesystem::create_directories(settingsRoot);
        const auto makeTga = [](const std::filesystem::path& path, uint32_t w, uint32_t h, uint8_t v) {
            std::vector<uint8_t> tga(18, 0);
            tga[2] = 2;
            tga[12] = static_cast<uint8_t>(w & 0xFF); tga[13] = static_cast<uint8_t>(w >> 8);
            tga[14] = static_cast<uint8_t>(h & 0xFF); tga[15] = static_cast<uint8_t>(h >> 8);
            tga[16] = 24;
            for (uint32_t i = 0; i < w * h; ++i) {
                tga.push_back(v); tga.push_back(v); tga.push_back(v);
            }
            std::ofstream out(path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(tga.data()), static_cast<std::streamsize>(tga.size()));
        };
        const auto readHeader = [](const std::filesystem::path& path, uint32_t& w, uint32_t& h,
                                   uint32_t& mipCount, uint8_t& flags, uint64_t& payloadSize) {
            std::ifstream in(path, std::ios::binary);
            std::array<char, 5> magic{};
            in.read(magic.data(), magic.size());
            uint32_t version = 0;
            in.read(reinterpret_cast<char*>(&version), sizeof(version));
            in.read(reinterpret_cast<char*>(&w), sizeof(w));
            in.read(reinterpret_cast<char*>(&h), sizeof(h));
            uint32_t channels = 0;
            in.read(reinterpret_cast<char*>(&channels), sizeof(channels));
            uint8_t bitDepth = 0;
            in.read(reinterpret_cast<char*>(&bitDepth), sizeof(bitDepth));
            in.read(reinterpret_cast<char*>(&mipCount), sizeof(mipCount));
            in.read(reinterpret_cast<char*>(&flags), sizeof(flags));
            in.read(reinterpret_cast<char*>(&payloadSize), sizeof(payloadSize));
            return magic == std::array<char, 5>{'V', 'C', 'T', 'E', 'X'} && version == 3;
        };

        // Defaults (mips on, srgb on, quality 80): 2x2 -> level0 + 1x1 chain,
        // srgb flag bit set.
        AssetRegistry defRegistry;
        AssetPipeline defPipeline(defRegistry);
        defPipeline.add_importer(std::make_unique<TextureImporter>());
        const auto defPath = settingsRoot / "def.tga";
        makeTga(defPath, 2, 2, 60);
        const auto defImport = defPipeline.import({ defPath, settingsRoot / "cooked", 1 });
        if (!defImport) { std::cerr << "line " << __LINE__ << " settings def import\n"; return EXIT_FAILURE; }
        uint32_t dw = 0, dh = 0, dmips = 0;
        uint8_t dflags = 0;
        uint64_t dpayload = 0;
        if (!readHeader(defImport.asset.cookedPath, dw, dh, dmips, dflags, dpayload)) {
            std::cerr << "line " << __LINE__ << " settings def header\n"; return EXIT_FAILURE;
        }
        if (dw != 2 || dh != 2 || dmips != 2 || (dflags & 1u) == 0 ||
            dpayload != static_cast<uint64_t>(2 * 2 * 3 + 1 * 1 * 3)) {
            std::cerr << "line " << __LINE__ << " settings def values " << dw << "x" << dh
                      << " mips=" << dmips << " flags=" << static_cast<int>(dflags)
                      << " payload=" << dpayload << "\n";
            return EXIT_FAILURE;
        }

        // Quality 40 + srgb off + mips off on a 4x4: box-downscale halves it to
        // 2x2, single level, no srgb flag.
        AssetRegistry qRegistry;
        AssetPipeline qPipeline(qRegistry);
        qPipeline.add_importer(std::make_unique<TextureImporter>());
        const auto qPath = settingsRoot / "q.tga";
        makeTga(qPath, 4, 4, 80);
        ImportSettings qSettings;
        qSettings.generateMipmaps = false;
        qSettings.srgb = false;
        qSettings.textureQuality = 40;
        const auto qImport = qPipeline.import({ qPath, settingsRoot / "cooked", 1, qSettings });
        if (!qImport) { std::cerr << "line " << __LINE__ << " settings q import\n"; return EXIT_FAILURE; }
        uint32_t qw = 0, qh = 0, qmips = 0;
        uint8_t qflags = 0;
        uint64_t qpayload = 0;
        if (!readHeader(qImport.asset.cookedPath, qw, qh, qmips, qflags, qpayload)) {
            std::cerr << "line " << __LINE__ << " settings q header\n"; return EXIT_FAILURE;
        }
        if (qw != 2 || qh != 2 || qmips != 1 || (qflags & 1u) != 0 ||
            qpayload != static_cast<uint64_t>(2 * 2 * 3)) {
            std::cerr << "line " << __LINE__ << " settings q values " << qw << "x" << qh
                      << " mips=" << qmips << " flags=" << static_cast<int>(qflags)
                      << " payload=" << qpayload << "\n";
            return EXIT_FAILURE;
        }

        // meshScale at cook: a triangle imported with scale 2 ships doubled
        // positions (the runtime never re-scales). A base64 data-URI buffer
        // keeps the import real (positions 0,0,0 / 1,0,0 / 0,1,0).
        const auto b64 = [](const std::vector<uint8_t>& bytes) {
            static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string out;
            for (size_t i = 0; i < bytes.size(); i += 3) {
                const uint32_t b0 = bytes[i];
                const uint32_t b1 = i + 1 < bytes.size() ? bytes[i + 1] : 0;
                const uint32_t b2 = i + 2 < bytes.size() ? bytes[i + 2] : 0;
                const uint32_t n = (b0 << 16) | (b1 << 8) | b2;
                out.push_back(table[(n >> 18) & 63]);
                out.push_back(table[(n >> 12) & 63]);
                out.push_back(i + 1 < bytes.size() ? table[(n >> 6) & 63] : '=');
                out.push_back(i + 2 < bytes.size() ? table[n & 63] : '=');
            }
            return out;
        };
        std::vector<uint8_t> positions;
        const auto pushFloat = [&](float value) {
            uint32_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            for (int b = 0; b < 4; ++b) positions.push_back(static_cast<uint8_t>((bits >> (b * 8)) & 0xFF));
        };
        pushFloat(0.0f); pushFloat(0.0f); pushFloat(0.0f);
        pushFloat(1.0f); pushFloat(0.0f); pushFloat(0.0f);
        pushFloat(0.0f); pushFloat(1.0f); pushFloat(0.0f);
        const std::string gltfJson =
            std::string("{\"asset\":{\"version\":\"2.0\"},") +
            "\"buffers\":[{\"byteLength\":36,\"uri\":\"data:application/octet-stream;base64," +
            b64(positions) + "\"}]," +
            "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}]," +
            "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}]," +
            "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}]}";
        const auto meshPath = settingsRoot / "tri.gltf";
        {
            std::ofstream f(meshPath);
            f << gltfJson;
        }

        const auto importMesh = [&](float scale) {
            AssetRegistry registry;
            AssetPipeline pipeline(registry);
            pipeline.add_importer(std::make_unique<MeshImporter>());
            ImportSettings settings;
            settings.meshScale = scale;
            auto result = pipeline.import({ meshPath, settingsRoot / "cooked", 1, settings });
            std::string error;
            return std::make_pair(result, GltfGeometryParser::parse_vcmesh(result.asset.cookedPath, &error));
        };
        const auto [scaledImport, scaled] = importMesh(2.0f);
        const auto [unitImport, unit] = importMesh(1.0f);
        if (!scaledImport) { std::cerr << "line " << __LINE__ << " scaled import: " << scaledImport.error << "\n"; return EXIT_FAILURE; }
        if (!unitImport) { std::cerr << "line " << __LINE__ << " unit import: " << unitImport.error << "\n"; return EXIT_FAILURE; }
        if (!scaled.success) { std::cerr << "line " << __LINE__ << " scaled parse: " << scaled.error << "\n"; return EXIT_FAILURE; }
        if (!unit.success) { std::cerr << "line " << __LINE__ << " unit parse: " << unit.error << "\n"; return EXIT_FAILURE; }

        const auto& sp = scaled.primitives.front().positions;
        const auto& up = unit.primitives.front().positions;
        if (std::abs(up[0].x) > 1e-4f || std::abs(up[1].x - 1.0f) > 1e-4f || std::abs(up[2].y - 1.0f) > 1e-4f ||
            std::abs(sp[1].x - 2.0f) > 1e-4f || std::abs(sp[2].y - 2.0f) > 1e-4f ||
            std::abs(sp[1].x - up[1].x * 2.0f) > 1e-4f || std::abs(sp[2].y - up[2].y * 2.0f) > 1e-4f) {
            std::cerr << "line " << __LINE__ << " settings mesh scale mismatch (unit=" << up[1].x
                      << "," << up[2].y << " scaled=" << sp[1].x << "," << sp[2].y << ")\n";
            return EXIT_FAILURE;
        }
    }

    // EXR (tinyexr — biblioteca externa): SaveEXRToMemory 2x2 → decode_exr →
    // RGBA16F com os valores esperados.
    {
        const float pixels[2 * 2 * 4] = {
            4.0f, 0.5f, 0.25f, 1.0f,
            1.0f, 2.0f, 3.0f, 0.5f,
            0.0f, 0.0f, 0.0f, 0.0f,
            0.75f, 0.125f, 8.0f, 1.0f
        };
        unsigned char* exrBytes = nullptr;
        const char* saveError = nullptr;
        const int exrSize = SaveEXRToMemory(pixels, 2, 2, 4, /*save_as_fp16=*/0, &exrBytes, &saveError);
        if (exrSize <= 0 || !exrBytes) {
            std::cerr << "line " << __LINE__ << " SaveEXRToMemory failed: " << (saveError ? saveError : "?") << "\n";
            return EXIT_FAILURE;
        }
        const std::vector<uint8_t> exrData(exrBytes, exrBytes + static_cast<size_t>(exrSize));
        std::free(exrBytes);

        DecodedExr decoded;
        std::string decodeError;
        if (!decode_exr(exrData, decoded, &decodeError)) {
            std::cerr << "line " << __LINE__ << " decode_exr: " << decodeError << "\n";
            return EXIT_FAILURE;
        }
        if (decoded.width != 2 || decoded.height != 2 || decoded.rgba16f.size() != 2 * 2 * 8) {
            std::cerr << "line " << __LINE__ << " EXR dims wrong: " << decoded.width << "x" << decoded.height
                      << " bytes=" << decoded.rgba16f.size() << "\n";
            return EXIT_FAILURE;
        }
        // Pixel (0,0) = (4.0, 0.5, 0.25, 1.0) → halves 0x4400/0x3800/0x3400/0x3C00.
        uint16_t r = 0, g = 0, b = 0, a = 0;
        std::memcpy(&r, decoded.rgba16f.data() + 0, 2);
        std::memcpy(&g, decoded.rgba16f.data() + 2, 2);
        std::memcpy(&b, decoded.rgba16f.data() + 4, 2);
        std::memcpy(&a, decoded.rgba16f.data() + 6, 2);
        if (r != 0x4400 || g != 0x3800 || b != 0x3400 || a != 0x3C00) {
            std::cerr << "line " << __LINE__ << " EXR pixel(0,0) halves: " << std::hex
                      << r << "," << g << "," << b << "," << a << std::dec << "\n";
            return EXIT_FAILURE;
        }
        // Pixel (1,1) = (0.75, 0.125, 8.0, 1.0).
        std::memcpy(&r, decoded.rgba16f.data() + 3 * 8 + 0, 2);
        std::memcpy(&g, decoded.rgba16f.data() + 3 * 8 + 2, 2);
        std::memcpy(&b, decoded.rgba16f.data() + 3 * 8 + 4, 2);
        if (r != 0x3A00 || g != 0x3000 || b != 0x4800) {
            std::cerr << "line " << __LINE__ << " EXR pixel(1,1) halves: " << std::hex
                      << r << "," << g << "," << b << std::dec << "\n";
            return EXIT_FAILURE;
        }
    }

    // FBX (ufbx — biblioteca externa): quad ASCII → import_fbx_geometry → 4
    // vértices, 2 triângulos, normais presentes.
    {
        const std::string fbxSource = R"FBX(
; FBX 7.4.0 project file
FBXHeaderExtension:  {
    FBXHeaderVersion: 1003
    FBXVersion: 7400
}
GlobalSettings:  {
    Version: 1000
    Properties70:  {
        P: "UpAxis", "int", "Integer", "", 1
        P: "UpAxisSign", "int", "Integer", "", 1
        P: "FrontAxis", "int", "Integer", "", 2
        P: "FrontAxisSign", "int", "Integer", "", 1
        P: "CoordAxis", "int", "Integer", "", 0
        P: "CoordAxisSign", "int", "Integer", "", 1
        P: "UnitScaleFactor", "double", "Number", "", 1
    }
}
Objects:  {
    Geometry: 100, "Geometry::Quad", "Mesh" {
        Vertices: *12 {
            a: 0,0,0,1,0,0,1,1,0,0,1,0
        }
        PolygonVertexIndex: *4 {
            a: 0,1,2,-4
        }
        GeometryVersion: 124
        LayerElementNormal: 0 {
            Version: 101
            Name: ""
            MappingInformationType: "ByPolygonVertex"
            ReferenceInformationType: "Direct"
            Normals: *12 {
                a: 0,0,1,0,0,1,0,0,1,0,0,1
            }
        }
        Layer: 0 {
            Version: 100
            LayerElement:  {
                Type: "LayerElementNormal"
                TypedIndex: 0
            }
        }
    }
    Model: 200, "Model::Quad", "Mesh" {
        Version: 232
        Shading: T
        Culling: "CullingOff"
    }
}
Connections:  {
    C: "OO", 200, 0
}
)FBX";
        GltfGeometryResult geometry;
        std::string fbxError;
        const std::vector<uint8_t> fbxBytes(fbxSource.begin(), fbxSource.end());
        if (!import_fbx_geometry(fbxBytes, geometry, &fbxError)) {
            std::cerr << "line " << __LINE__ << " fbx import: " << fbxError << "\n";
            return EXIT_FAILURE;
        }
        if (geometry.primitives.size() != 1) {
            std::cerr << "line " << __LINE__ << " fbx primitives=" << geometry.primitives.size() << "\n";
            return EXIT_FAILURE;
        }
        const GltfMeshPrimitive& quad = geometry.primitives.front();
        if (quad.positions.size() != 4 || quad.indices.size() != 6 || !quad.indexed) {
            std::cerr << "line " << __LINE__ << " fbx quad: verts=" << quad.positions.size()
                      << " idx=" << quad.indices.size() << " indexed=" << quad.indexed << "\n";
            return EXIT_FAILURE;
        }
        // Cantos do quad (0,0,0),(1,0,0),(1,1,0),(0,1,0); normais (0,0,1).
        if (std::abs(quad.positions[0].x) > 1e-4f || std::abs(quad.positions[0].y) > 1e-4f ||
            std::abs(quad.positions[1].x - 1.0f) > 1e-4f || std::abs(quad.positions[2].y - 1.0f) > 1e-4f ||
            std::abs(quad.positions[3].x) > 1e-4f || std::abs(quad.positions[3].y - 1.0f) > 1e-4f) {
            std::cerr << "line " << __LINE__ << " fbx quad positions wrong\n";
            return EXIT_FAILURE;
        }
        for (const glm::vec3& normal : quad.normals) {
            if (std::abs(normal.x) > 1e-4f || std::abs(normal.y) > 1e-4f || std::abs(normal.z - 1.0f) > 1e-4f) {
                std::cerr << "line " << __LINE__ << " fbx quad normal wrong\n";
                return EXIT_FAILURE;
            }
        }
    }

    return EXIT_SUCCESS;
}
