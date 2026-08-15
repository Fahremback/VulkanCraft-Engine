#pragma once

#include <filesystem>
#include <string>

namespace Engine {

class Scene;

class Prefab;

struct MaterialAsset;
struct AudioEventAsset;
struct PhysicsMaterialAsset;
class VisualScriptGraph;

struct SerializationResult {
    bool success{};
    std::string error;

    explicit operator bool() const noexcept { return success; }
};

// Development serialization contract. Runtime code owns no JSON parser details
// and callers always receive an actionable error instead of a false success.
class Serializer final {
public:
    static SerializationResult serialize_scene(
        const Scene& scene, const std::filesystem::path& path);
    static SerializationResult deserialize_scene(
        Scene& scene, const std::filesystem::path& path);
    static SerializationResult serialize_prefab(
        const Prefab& prefab, const std::filesystem::path& path);
    static SerializationResult deserialize_prefab(
        Prefab& prefab, const std::filesystem::path& path);
    static SerializationResult serialize_material(
        const MaterialAsset& material, const std::filesystem::path& path);
    static SerializationResult deserialize_material(
        MaterialAsset& material, const std::filesystem::path& path);
    static SerializationResult serialize_audio_event(
        const AudioEventAsset& audioEvent, const std::filesystem::path& path);
    static SerializationResult deserialize_audio_event(
        AudioEventAsset& audioEvent, const std::filesystem::path& path);
    static SerializationResult serialize_physics_material(
        const PhysicsMaterialAsset& mat, const std::filesystem::path& path);
    static SerializationResult deserialize_physics_material(
        PhysicsMaterialAsset& mat, const std::filesystem::path& path);
    static SerializationResult serialize_visual_script(
        const VisualScriptGraph& script, const std::filesystem::path& path);
    static SerializationResult deserialize_visual_script(
        VisualScriptGraph& script, const std::filesystem::path& path);
};

} // namespace Engine
