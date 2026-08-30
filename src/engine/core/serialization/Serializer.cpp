#include "Serializer.hpp"

#include "../../scene/Scene.hpp"
#include "../../scene/Prefab.hpp"
#include "../../rendering/materials/Material.hpp"
#include "../../audio/AudioEvent.hpp"
#include "../../physics/Physics.hpp"
#include "../../scripting/VisualScriptGraph.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Engine {
namespace {

std::string escape_json(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char c : value) {
        switch (c) {
        case '\\': escaped += "\\\\"; break;
        case '"': escaped += "\\\""; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default: escaped += c; break;
        }
    }
    return escaped;
}

std::optional<std::string> string_field(std::string_view object, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    size_t cursor = object.find(needle);
    if (cursor == std::string_view::npos) return std::nullopt;
    cursor = object.find(':', cursor + needle.size());
    if (cursor == std::string_view::npos) return std::nullopt;
    cursor = object.find('"', cursor + 1);
    if (cursor == std::string_view::npos) return std::nullopt;
    ++cursor;
    std::string value;
    bool escaped = false;
    for (; cursor < object.size(); ++cursor) {
        const char c = object[cursor];
        if (escaped) {
            switch (c) {
            case 'n': value += '\n'; break;
            case 'r': value += '\r'; break;
            case 't': value += '\t'; break;
            default: value += c; break;
            }
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            return value;
        } else {
            value += c;
        }
    }
    return std::nullopt;
}

std::optional<double> number_field(std::string_view object, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    size_t cursor = object.find(needle);
    if (cursor == std::string_view::npos) return std::nullopt;
    cursor = object.find(':', cursor + needle.size());
    if (cursor == std::string_view::npos) return std::nullopt;
    ++cursor;
    while (cursor < object.size() && std::isspace(static_cast<unsigned char>(object[cursor]))) ++cursor;
    const size_t begin = cursor;
    while (cursor < object.size() &&
           (std::isdigit(static_cast<unsigned char>(object[cursor])) ||
            object[cursor] == '-' || object[cursor] == '+' ||
            object[cursor] == '.' || object[cursor] == 'e' || object[cursor] == 'E')) ++cursor;
    if (begin == cursor) return std::nullopt;
    try {
        return std::stod(std::string(object.substr(begin, cursor - begin)));
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<bool> bool_field(std::string_view object, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    size_t cursor = object.find(needle);
    if (cursor == std::string_view::npos) return std::nullopt;
    cursor = object.find(':', cursor + needle.size());
    if (cursor == std::string_view::npos) return std::nullopt;
    ++cursor;
    while (cursor < object.size() && std::isspace(static_cast<unsigned char>(object[cursor]))) ++cursor;
    if (object.substr(cursor, 4) == "true") return true;
    if (object.substr(cursor, 5) == "false") return false;
    return std::nullopt;
}

std::optional<std::string_view> object_field(std::string_view object, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    size_t cursor = object.find(needle);
    if (cursor == std::string_view::npos) return std::nullopt;
    cursor = object.find('{', cursor + needle.size());
    if (cursor == std::string_view::npos) return std::nullopt;
    const size_t begin = cursor;
    int depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (; cursor < object.size(); ++cursor) {
        const char c = object[cursor];
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
            continue;
        }
        if (c == '"') quoted = true;
        else if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) return object.substr(begin, cursor - begin + 1);
    }
    return std::nullopt;
}

std::vector<std::string_view> array_objects(std::string_view document, std::string_view key) {
    std::vector<std::string_view> objects;
    const std::string needle = "\"" + std::string(key) + "\"";
    size_t cursor = document.find(needle);
    if (cursor == std::string_view::npos) return objects;
    cursor = document.find('[', cursor + needle.size());
    if (cursor == std::string_view::npos) return objects;
    ++cursor;
    while (cursor < document.size()) {
        cursor = document.find_first_not_of(" \t\r\n,", cursor);
        if (cursor == std::string_view::npos || document[cursor] == ']') break;
        if (document[cursor] != '{') return {};
        const size_t begin = cursor;
        int depth = 0;
        bool quoted = false;
        bool escaped = false;
        for (; cursor < document.size(); ++cursor) {
            const char c = document[cursor];
            if (quoted) {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') quoted = false;
                continue;
            }
            if (c == '"') quoted = true;
            else if (c == '{') ++depth;
            else if (c == '}' && --depth == 0) {
                objects.push_back(document.substr(begin, cursor - begin + 1));
                ++cursor;
                break;
            }
        }
    }
    return objects;
}

void write_transform(std::ostream& out, const TransformComponent& value) {
    out << "      \"Transform\": {"
        << "\"px\":" << value.position.x << ",\"py\":" << value.position.y << ",\"pz\":" << value.position.z
        << ",\"rx\":" << value.rotation.x << ",\"ry\":" << value.rotation.y << ",\"rz\":" << value.rotation.z
        << ",\"sx\":" << value.scale.x << ",\"sy\":" << value.scale.y << ",\"sz\":" << value.scale.z << "}";
}

void write_prefab_components(std::ostream& out, const PrefabEntityData& entity) {
    bool comma = false;
    const auto separator = [&] { if (comma) out << ','; comma = true; };
    if (entity.hasTransform) {
        separator(); const auto& v = entity.transform;
        out << "\"Transform\":{\"px\":" << v.position.x << ",\"py\":" << v.position.y << ",\"pz\":" << v.position.z
            << ",\"rx\":" << v.rotation.x << ",\"ry\":" << v.rotation.y << ",\"rz\":" << v.rotation.z
            << ",\"sx\":" << v.scale.x << ",\"sy\":" << v.scale.y << ",\"sz\":" << v.scale.z << '}';
    }
    if (entity.hasMeshRenderer) {
        separator(); const auto& v = entity.meshRenderer;
        out << "\"MeshRenderer\":{\"mesh\":\"" << v.meshAssetID.to_string()
            << "\",\"material\":\"" << v.materialAssetID.to_string()
            << "\",\"visible\":" << (v.isVisible ? "true" : "false")
            << ",\"castShadows\":" << (v.castShadows ? "true" : "false") << '}';
    }
    if (entity.hasLight) {
        separator(); const auto& v = entity.light;
        out << "\"Light\":{\"r\":" << v.color.r << ",\"g\":" << v.color.g << ",\"b\":" << v.color.b
            << ",\"intensity\":" << v.intensity << ",\"range\":" << v.range
            << ",\"castShadows\":" << (v.castShadows ? "true" : "false")
            << ",\"coneAngle\":" << v.coneAngle << '}';
    }
    if (entity.hasCamera) {
        separator(); const auto& v = entity.camera;
        out << "\"Camera\":{\"fov\":" << v.fov << ",\"near\":" << v.nearPlane << ",\"far\":" << v.farPlane
            << ",\"primary\":" << (v.isPrimary ? "true" : "false") << '}';
    }
    if (entity.hasRigidbody) {
        separator(); const auto& v = entity.rigidbody;
        out << "\"Rigidbody\":{\"mass\":" << v.mass << ",\"friction\":" << v.friction
            << ",\"restitution\":" << v.restitution << ",\"kinematic\":" << (v.isKinematic ? "true" : "false")
            << ",\"gravity\":" << (v.useGravity ? "true" : "false") << '}';
    }
    if (entity.hasMaterial) {
        separator(); const auto& v = entity.material;
        out << "\"Material\":{\"ar\":" << v.albedo.r << ",\"ag\":" << v.albedo.g << ",\"ab\":" << v.albedo.b
            << ",\"roughness\":" << v.roughness << ",\"metallic\":" << v.metallic
            << ",\"er\":" << v.emissiveColor.r << ",\"eg\":" << v.emissiveColor.g << ",\"eb\":" << v.emissiveColor.b
            << ",\"emissiveIntensity\":" << v.emissiveIntensity << '}';
    }
    if (entity.hasVoxelVolume) {
        separator(); const auto& v = entity.voxelVolume;
        out << "\"VoxelVolume\":{\"chunkBudget\":" << v.chunkBudget << ",\"seed\":" << v.seed
            << ",\"seaLevel\":" << v.seaLevel << ",\"farLod\":" << (v.enableFarLod ? "true" : "false") << '}';
    }
}

void read_prefab_components(std::string_view object, PrefabEntityData& entity) {
    const auto components = object_field(object, "components");
    if (!components) return;
    if (const auto value = object_field(*components, "Transform")) {
        entity.hasTransform = true;
        entity.transform.position = {static_cast<float>(number_field(*value, "px").value_or(0.0)), static_cast<float>(number_field(*value, "py").value_or(0.0)), static_cast<float>(number_field(*value, "pz").value_or(0.0))};
        entity.transform.rotation = {static_cast<float>(number_field(*value, "rx").value_or(0.0)), static_cast<float>(number_field(*value, "ry").value_or(0.0)), static_cast<float>(number_field(*value, "rz").value_or(0.0))};
        entity.transform.scale = {static_cast<float>(number_field(*value, "sx").value_or(1.0)), static_cast<float>(number_field(*value, "sy").value_or(1.0)), static_cast<float>(number_field(*value, "sz").value_or(1.0))};
    }
    if (const auto value = object_field(*components, "MeshRenderer")) {
        entity.hasMeshRenderer = true;
        entity.meshRenderer.meshAssetID = UUID::from_string(string_field(*value, "mesh").value_or(""));
        entity.meshRenderer.materialAssetID = UUID::from_string(string_field(*value, "material").value_or(""));
        entity.meshRenderer.isVisible = bool_field(*value, "visible").value_or(true);
        entity.meshRenderer.castShadows = bool_field(*value, "castShadows").value_or(true);
    }
    if (const auto value = object_field(*components, "Light")) {
        entity.hasLight = true;
        entity.light.color = {static_cast<float>(number_field(*value, "r").value_or(1.0)), static_cast<float>(number_field(*value, "g").value_or(1.0)), static_cast<float>(number_field(*value, "b").value_or(1.0))};
        entity.light.intensity = static_cast<float>(number_field(*value, "intensity").value_or(1000.0));
        entity.light.range = static_cast<float>(number_field(*value, "range").value_or(50.0));
        entity.light.castShadows = bool_field(*value, "castShadows").value_or(true);
        entity.light.coneAngle = static_cast<float>(
            number_field(*value, "coneAngle").value_or(0.78539816339));
    }
    if (const auto value = object_field(*components, "Camera")) {
        entity.hasCamera = true;
        entity.camera.fov = static_cast<float>(number_field(*value, "fov").value_or(70.0));
        entity.camera.nearPlane = static_cast<float>(number_field(*value, "near").value_or(0.1));
        entity.camera.farPlane = static_cast<float>(number_field(*value, "far").value_or(1000.0));
        entity.camera.isPrimary = bool_field(*value, "primary").value_or(true);
    }
    if (const auto value = object_field(*components, "Rigidbody")) {
        entity.hasRigidbody = true;
        entity.rigidbody.mass = static_cast<float>(number_field(*value, "mass").value_or(1.0));
        entity.rigidbody.friction = static_cast<float>(number_field(*value, "friction").value_or(0.5));
        entity.rigidbody.restitution = static_cast<float>(number_field(*value, "restitution").value_or(0.1));
        entity.rigidbody.isKinematic = bool_field(*value, "kinematic").value_or(false);
        entity.rigidbody.useGravity = bool_field(*value, "gravity").value_or(true);
    }
    if (const auto value = object_field(*components, "Material")) {
        entity.hasMaterial = true;
        entity.material.albedo = {static_cast<float>(number_field(*value, "ar").value_or(1.0)), static_cast<float>(number_field(*value, "ag").value_or(1.0)), static_cast<float>(number_field(*value, "ab").value_or(1.0))};
        entity.material.roughness = static_cast<float>(number_field(*value, "roughness").value_or(0.5));
        entity.material.metallic = static_cast<float>(number_field(*value, "metallic").value_or(0.0));
        entity.material.emissiveColor = {static_cast<float>(number_field(*value, "er").value_or(0.0)), static_cast<float>(number_field(*value, "eg").value_or(0.0)), static_cast<float>(number_field(*value, "eb").value_or(0.0))};
        entity.material.emissiveIntensity = static_cast<float>(number_field(*value, "emissiveIntensity").value_or(0.0));
    }
    if (const auto value = object_field(*components, "VoxelVolume")) {
        entity.hasVoxelVolume = true;
        entity.voxelVolume.chunkBudget = static_cast<int>(number_field(*value, "chunkBudget").value_or(1024.0));
        entity.voxelVolume.seed = static_cast<int>(number_field(*value, "seed").value_or(1337.0));
        entity.voxelVolume.seaLevel = static_cast<float>(number_field(*value, "seaLevel").value_or(26.0));
        entity.voxelVolume.enableFarLod = bool_field(*value, "farLod").value_or(true);
    }
}

std::string override_type(const std::any& value) {
    if (value.type() == typeid(float)) return "float";
    if (value.type() == typeid(int)) return "int";
    if (value.type() == typeid(bool)) return "bool";
    if (value.type() == typeid(glm::vec3)) return "vec3";
    if (value.type() == typeid(UUID)) return "uuid";
    if (value.type() == typeid(std::string)) return "string";
    return {};
}

void write_override_value(std::ostream& out, const std::any& value, const std::string& type) {
    if (type == "float") out << std::any_cast<float>(value);
    else if (type == "int") out << std::any_cast<int>(value);
    else if (type == "bool") out << (std::any_cast<bool>(value) ? "true" : "false");
    else if (type == "vec3") { const auto v = std::any_cast<glm::vec3>(value); out << "{\"x\":" << v.x << ",\"y\":" << v.y << ",\"z\":" << v.z << '}'; }
    else if (type == "uuid") out << "\"" << std::any_cast<UUID>(value).to_string() << "\"";
    else if (type == "string") out << "\"" << escape_json(std::any_cast<std::string>(value)) << "\"";
    else out << "null";
}

std::any read_override_value(std::string_view object, const std::string& type) {
    if (type == "float") return static_cast<float>(number_field(object, "value").value_or(0.0));
    if (type == "int") return static_cast<int>(number_field(object, "value").value_or(0.0));
    if (type == "bool") return bool_field(object, "value").value_or(false);
    if (type == "uuid") return UUID::from_string(string_field(object, "value").value_or(""));
    if (type == "string") return string_field(object, "value").value_or("");
    if (type == "vec3") {
        if (const auto value = object_field(object, "value")) return glm::vec3{
            static_cast<float>(number_field(*value, "x").value_or(0.0)),
            static_cast<float>(number_field(*value, "y").value_or(0.0)),
            static_cast<float>(number_field(*value, "z").value_or(0.0))};
    }
    return {};
}

} // namespace

SerializationResult Serializer::serialize_scene(
    const Scene& scene, const std::filesystem::path& path) {
    std::error_code error;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return {false, "Cannot open scene for writing: " + path.string()};
    out << std::setprecision(9);
    out << "{\n  \"format\":\"VulkanEngine.Scene\",\n  \"version\":1,\n";
    out << "  \"scene_id\":\"" << scene.m_id.to_string() << "\",\n";
    out << "  \"name\":\"" << escape_json(scene.m_name) << "\",\n  \"entities\":[\n";
    size_t emitted = 0;
    for (const auto& [id, entity] : scene.m_entities) {
        out << "    {\n      \"id\":\"" << id.to_string() << "\",\n";
        out << "      \"name\":\"" << escape_json(entity.get_name()) << "\",\n";
        auto transform = scene.transformComponents.find(id);
        if (transform != scene.transformComponents.end()) write_transform(out, transform->second);
        if (auto light = scene.lightComponents.find(id); light != scene.lightComponents.end()) {
            const auto& value = light->second;
            out << ",\n      \"Light\":{\"r\":" << value.color.r << ",\"g\":" << value.color.g
                << ",\"b\":" << value.color.b << ",\"intensity\":" << value.intensity
                << ",\"range\":" << value.range << ",\"castShadows\":"
                << (value.castShadows ? "true" : "false") << ",\"type\":"
                << static_cast<int>(value.type) << ",\"coneAngle\":" << value.coneAngle << "}";
        }
        if (auto mesh = scene.meshRendererComponents.find(id); mesh != scene.meshRendererComponents.end()) {
            const auto& v=mesh->second;out<<",\n      \"MeshRenderer\":{\"mesh\":\""<<v.meshAssetID.to_string()<<"\",\"material\":\""<<v.materialAssetID.to_string()<<"\",\"visible\":"<<(v.isVisible?"true":"false")<<",\"castShadows\":"<<(v.castShadows?"true":"false")<<"}";
        }
        if (auto camera = scene.cameraComponents.find(id); camera != scene.cameraComponents.end()) {
            const auto& v=camera->second;out<<",\n      \"Camera\":{\"fov\":"<<v.fov<<",\"near\":"<<v.nearPlane<<",\"far\":"<<v.farPlane<<",\"primary\":"<<(v.isPrimary?"true":"false")<<"}";
        }
        if (auto body = scene.rigidbodyComponents.find(id); body != scene.rigidbodyComponents.end()) {
            const auto& v=body->second;out<<",\n      \"Rigidbody\":{\"mass\":"<<v.mass<<",\"friction\":"<<v.friction<<",\"restitution\":"<<v.restitution<<",\"kinematic\":"<<(v.isKinematic?"true":"false")<<",\"gravity\":"<<(v.useGravity?"true":"false")<<"}";
        }
        if (auto wpn = scene.weaponComponents.find(id); wpn != scene.weaponComponents.end()) {
            const auto& v=wpn->second;out<<",\n      \"Weapon\":{\"damage\":"<<v.damage<<",\"rpm\":"<<v.roundsPerMinute<<",\"magazine\":"<<v.magazineSize<<",\"reserve\":"<<v.reserveAmmo<<",\"automatic\":"<<(v.automatic?"true":"false")<<",\"spread\":"<<v.spreadDegrees<<",\"hitscan\":"<<(v.hitscan?"true":"false")<<"}";
        }
        if (auto pe = scene.particleEmitterComponents.find(id); pe != scene.particleEmitterComponents.end()) {
            const auto& v=pe->second;out<<",\n      \"ParticleEmitter\":{\"px\":"<<v.position.x<<",\"py\":"<<v.position.y<<",\"pz\":"<<v.position.z<<",\"dx\":"<<v.direction.x<<",\"dy\":"<<v.direction.y<<",\"dz\":"<<v.direction.z<<",\"cone\":"<<v.coneAngle<<",\"rate\":"<<v.rate<<",\"speedMin\":"<<v.speedMin<<",\"speedMax\":"<<v.speedMax<<",\"lifeMin\":"<<v.lifetimeMin<<",\"lifeMax\":"<<v.lifetimeMax<<",\"sizeStart\":"<<v.sizeStart<<",\"sizeEnd\":"<<v.sizeEnd<<",\"cr\":"<<v.colorStart.r<<",\"cg\":"<<v.colorStart.g<<",\"cb\":"<<v.colorStart.b<<",\"ca\":"<<v.colorStart.a<<",\"er\":"<<v.colorEnd.r<<",\"eg\":"<<v.colorEnd.g<<",\"eb\":"<<v.colorEnd.b<<",\"ea\":"<<v.colorEnd.a<<",\"ax\":"<<v.acceleration.x<<",\"ay\":"<<v.acceleration.y<<",\"az\":"<<v.acceleration.z<<",\"drag\":"<<v.drag<<",\"turbulence\":"<<v.turbulence<<",\"restitution\":"<<v.restitution<<",\"burst\":"<<v.burstCount<<",\"collide\":"<<(v.collide?"true":"false")<<",\"emitting\":"<<(v.emitting?"true":"false")<<"}";
        }
        if (auto veh = scene.vehicleComponents.find(id); veh != scene.vehicleComponents.end()) {
            const auto& v=veh->second;out<<",\n      \"Vehicle\":{\"enginePower\":"<<v.enginePower<<",\"maxSteer\":"<<v.maxSteerAngle<<",\"brake\":"<<v.brakeForce<<",\"wheelRadius\":"<<v.wheelRadius<<",\"suspRest\":"<<v.suspensionRest<<",\"wheelBase\":"<<v.wheelBase<<",\"track\":"<<v.trackWidth<<",\"mass\":"<<v.mass<<",\"fwd\":"<<(v.frontWheelDrive?"true":"false")<<",\"enabled\":"<<(v.enabled?"true":"false")<<"}";
        }
        if (auto rg = scene.ragdollComponents.find(id); rg != scene.ragdollComponents.end()) {
            const auto& v=rg->second;out<<",\n      \"Ragdoll\":{\"enabled\":"<<(v.enabled?"true":"false")<<",\"blend\":"<<v.blendWeight<<",\"fromSkeleton\":"<<(v.fromSkeleton?"true":"false")<<",\"massPerBone\":"<<v.massPerBone<<",\"ox\":"<<v.spawnOffset.x<<",\"oy\":"<<v.spawnOffset.y<<",\"oz\":"<<v.spawnOffset.z<<"}";
        }
        if (auto ms = scene.missionComponents.find(id); ms != scene.missionComponents.end()) {
            const auto& v=ms->second;out<<",\n      \"Mission\":{\"id\":\""<<escape_json(v.missionId)<<"\",\"objective\":\""<<escape_json(v.objectiveText)<<"\",\"target\":"<<v.objectiveTarget<<",\"completeEvent\":\""<<escape_json(v.completeEvent)<<"\",\"autoStart\":"<<(v.autoStart?"true":"false")<<",\"active\":"<<(v.active?"true":"false")<<"}";
        }
        if (auto dg = scene.dialogueComponents.find(id); dg != scene.dialogueComponents.end()) {
            const auto& v=dg->second;out<<",\n      \"Dialogue\":{\"id\":\""<<escape_json(v.dialogueId)<<"\",\"character\":\""<<escape_json(v.character)<<"\",\"line\":\""<<escape_json(v.line)<<"\",\"choice\":\""<<escape_json(v.choiceText)<<"\",\"next\":\""<<escape_json(v.nextDialogueId)<<"\",\"playOnStart\":"<<(v.playOnStart?"true":"false")<<",\"playing\":"<<(v.playing?"true":"false")<<"}";
        }
        if (auto ds = scene.destructionComponents.find(id); ds != scene.destructionComponents.end()) {
            const auto& v=ds->second;out<<",\n      \"Destruction\":{\"csx\":"<<v.chunkSize.x<<",\"csy\":"<<v.chunkSize.y<<",\"csz\":"<<v.chunkSize.z<<",\"chunks\":"<<v.chunkCount<<",\"health\":"<<v.chunkHealth<<",\"radius\":"<<v.damageRadius<<",\"impulse\":"<<v.damageImpulse<<",\"enabled\":"<<(v.enabled?"true":"false")<<",\"destroyed\":"<<(v.destroyed?"true":"false")<<"}";
        }
        if (auto nav = scene.navigationComponents.find(id); nav != scene.navigationComponents.end()) {
            const auto& v=nav->second;out<<",\n      \"Navigation\":{\"gw\":"<<v.gridWidth<<",\"gh\":"<<v.gridHeight<<",\"cell\":"<<v.cellSize<<",\"speed\":"<<v.agentSpeed<<",\"enabled\":"<<(v.enabled?"true":"false")<<"}";
        }
        if (auto au = scene.audioComponents.find(id); au != scene.audioComponents.end()) {
            const auto& v=au->second;out<<",\n      \"Audio\":{\"clip\":\""<<escape_json(v.clipPath)<<"\",\"volume\":"<<v.volume<<",\"pitch\":"<<v.pitch<<",\"spatial\":"<<(v.spatial?"true":"false")<<",\"looping\":"<<(v.looping?"true":"false")<<",\"playOnStart\":"<<(v.playOnStart?"true":"false")<<",\"playing\":"<<(v.playing?"true":"false")<<"}";
        }
        if (auto material = scene.materialComponents.find(id); material != scene.materialComponents.end()) {
            const auto& v=material->second;out<<",\n      \"Material\":{\"ar\":"<<v.albedo.r<<",\"ag\":"<<v.albedo.g<<",\"ab\":"<<v.albedo.b<<",\"roughness\":"<<v.roughness<<",\"metallic\":"<<v.metallic<<",\"er\":"<<v.emissiveColor.r<<",\"eg\":"<<v.emissiveColor.g<<",\"eb\":"<<v.emissiveColor.b<<",\"emissiveIntensity\":"<<v.emissiveIntensity<<"}";
        }
        if (auto voxel = scene.voxelVolumeComponents.find(id); voxel != scene.voxelVolumeComponents.end()) {
            const auto& v=voxel->second;out<<",\n      \"VoxelVolume\":{\"chunkBudget\":"<<v.chunkBudget<<",\"seed\":"<<v.seed<<",\"seaLevel\":"<<v.seaLevel<<",\"enableFarLod\":"<<(v.enableFarLod?"true":"false")<<"}";
        }
        if (auto hier = scene.hierarchyComponents.find(id); hier != scene.hierarchyComponents.end() && hier->second.parentID.is_valid()) {
            out << ",\n      \"Hierarchy\":{\"parent_id\":\"" << hier->second.parentID.to_string() << "\"}";
        }
        // Wicked-port components (frontend; PORTS.md). Compact field names keep
        // the scene file small; the read side mirrors them.
        if (auto cl = scene.colliderComponents.find(id); cl != scene.colliderComponents.end()) {
            const auto& v=cl->second;out<<",\n      \"Collider\":{\"shape\":"<<static_cast<int>(v.shape)<<",\"sx\":"<<v.size.x<<",\"sy\":"<<v.size.y<<",\"sz\":"<<v.size.z<<",\"radius\":"<<v.radius<<",\"height\":"<<v.height<<",\"ox\":"<<v.offset.x<<",\"oy\":"<<v.offset.y<<",\"oz\":"<<v.offset.z<<",\"trigger\":"<<(v.isTrigger?"true":"false")<<",\"enabled\":"<<(v.enabled?"true":"false")<<"}";
        }
        if (auto cn = scene.constraintComponents.find(id); cn != scene.constraintComponents.end()) {
            const auto& v=cn->second;out<<",\n      \"Constraint\":{\"type\":"<<static_cast<int>(v.type)<<",\"other\":\""<<v.otherEntity.to_string()<<"\",\"ax\":"<<v.anchor.x<<",\"ay\":"<<v.anchor.y<<",\"az\":"<<v.anchor.z<<",\"dx\":"<<v.axis.x<<",\"dy\":"<<v.axis.y<<",\"dz\":"<<v.axis.z<<",\"breakForce\":"<<v.breakForce<<",\"enabled\":"<<(v.enabled?"true":"false")<<"}";
        }
        if (auto sb = scene.softBodyComponents.find(id); sb != scene.softBodyComponents.end()) {
            const auto& v=sb->second;out<<",\n      \"SoftBody\":{\"detail\":"<<v.detail<<",\"mass\":"<<v.mass<<",\"friction\":"<<v.friction<<",\"restitution\":"<<v.restitution<<",\"pressure\":"<<v.pressure<<",\"vertexRadius\":"<<v.vertexRadius<<",\"wind\":"<<(v.wind?"true":"false")<<",\"enabled\":"<<(v.enabled?"true":"false")<<"}";
        }
        if (auto sp = scene.springComponents.find(id); sp != scene.springComponents.end()) {
            const auto& v=sp->second;out<<",\n      \"Spring\":{\"stiffness\":"<<v.stiffness<<",\"drag\":"<<v.drag<<",\"wind\":"<<v.wind<<",\"gravity\":"<<v.gravity<<",\"hitRadius\":"<<v.hitRadius<<",\"disabled\":"<<(v.disabled?"true":"false")<<",\"useGravity\":"<<(v.useGravity?"true":"false")<<",\"enabled\":"<<(v.enabled?"true":"false")<<"}";
        }
        if (auto dc = scene.decalComponents.find(id); dc != scene.decalComponents.end()) {
            const auto& v=dc->second;out<<",\n      \"Decal\":{\"texture\":\""<<escape_json(v.texturePath)<<"\",\"r\":"<<v.color.r<<",\"g\":"<<v.color.g<<",\"b\":"<<v.color.b<<",\"slope\":"<<v.slopeBlendPower<<",\"static\":"<<(v.projectOnStatic?"true":"false")<<",\"onlyAlpha\":"<<(v.onlyAlpha?"true":"false")<<",\"enabled\":"<<(v.enabled?"true":"false")<<"}";
        }
        if (auto sl = scene.splineComponents.find(id); sl != scene.splineComponents.end()) {
            const auto& v=sl->second;out<<",\n      \"Spline\":{\"looped\":"<<(v.looped?"true":"false")<<",\"filled\":"<<(v.filled?"true":"false")<<",\"width\":"<<v.width<<",\"rot\":"<<v.rotation<<",\"subdiv\":"<<v.subdiv<<",\"points\":[";
            for (size_t pi = 0; pi < v.points.size(); ++pi) {
                out << (pi ? "," : "") << "[" << v.points[pi].x << "," << v.points[pi].y << "," << v.points[pi].z << "]";
            }
            out << "]}";
        }
        if (auto ff = scene.forceFieldComponents.find(id); ff != scene.forceFieldComponents.end()) {
            const auto& v=ff->second;out<<",\n      \"ForceField\":{\"type\":"<<static_cast<int>(v.type)<<",\"strength\":"<<v.strength<<",\"range\":"<<v.range<<",\"enabled\":"<<(v.enabled?"true":"false")<<"}";
        }
        if (auto ep = scene.envProbeComponents.find(id); ep != scene.envProbeComponents.end()) {
            const auto& v=ep->second;out<<",\n      \"EnvProbe\":{\"realtime\":"<<(v.realTime?"true":"false")<<",\"viewDistance\":"<<v.viewDistance<<",\"resolution\":"<<v.resolution<<",\"enabled\":"<<(v.enabled?"true":"false")<<"}";
        }
        if (auto wt = scene.weatherComponents.find(id); wt != scene.weatherComponents.end()) {
            const auto& v=wt->second;out<<",\n      \"Weather\":{\"sr\":"<<v.sunColor.r<<",\"sg\":"<<v.sunColor.g<<",\"sb\":"<<v.sunColor.b<<",\"fogDensity\":"<<v.fogDensity<<",\"fogStart\":"<<v.fogStart<<",\"skyExposure\":"<<v.skyExposure<<",\"skyRotation\":"<<v.skyRotation<<",\"windSpeed\":"<<v.windSpeed<<",\"rainAmount\":"<<v.rainAmount<<",\"rainLength\":"<<v.rainLength<<",\"heightFog\":"<<(v.heightFog?"true":"false")<<",\"enabled\":"<<(v.enabled?"true":"false")<<"}";
        }
        if (auto hp = scene.hairParticleComponents.find(id); hp != scene.hairParticleComponents.end()) {
            const auto& v=hp->second;out<<",\n      \"HairParticle\":{\"mesh\":\""<<escape_json(v.meshPath)<<"\",\"count\":"<<v.count<<",\"length\":"<<v.length<<",\"width\":"<<v.width<<",\"stiffness\":"<<v.stiffness<<",\"drag\":"<<v.drag<<",\"gravityPower\":"<<v.gravityPower<<",\"randomness\":"<<v.randomness<<",\"segments\":"<<v.segments<<",\"seed\":"<<v.seed<<",\"cr\":"<<v.color.r<<",\"cg\":"<<v.color.g<<",\"cb\":"<<v.color.b<<",\"enabled\":"<<(v.enabled?"true":"false")<<"}";
        }
        if (auto ly = scene.layerComponents.find(id); ly != scene.layerComponents.end()) {
            const auto& v=ly->second;out<<",\n      \"Layer\":{\"name\":\""<<escape_json(v.name)<<"\",\"index\":"<<v.index<<",\"visible\":"<<(v.visible?"true":"false")<<",\"locked\":"<<(v.locked?"true":"false")<<",\"enabled\":"<<(v.enabled?"true":"false")<<"}";
        }
        if (auto pc = scene.paintComponents.find(id); pc != scene.paintComponents.end()) {
            const auto& v=pc->second;
            out << ",\n      \"Paint\":{\"r\":" << v.brushColor.r << ",\"g\":" << v.brushColor.g
                << ",\"b\":" << v.brushColor.b << ",\"brush\":" << v.brushSize
                << ",\"opacity\":" << v.opacity << ",\"mode\":" << (v.paintMode ? "true" : "false")
                << ",\"colors\":[";
            for (size_t ci = 0; ci < v.vertexColors.size(); ++ci) {
                out << (ci ? "," : "") << "[" << v.vertexColors[ci].x << "," << v.vertexColors[ci].y
                    << "," << v.vertexColors[ci].z << "," << v.vertexColors[ci].w << "]";
            }
            out << "]}";
        }
        if (auto vc = scene.videoComponents.find(id); vc != scene.videoComponents.end()) {
            const auto& v=vc->second;
            out << ",\n      \"Video\":{\"fps\":" << v.fps << ",\"frame\":" << v.currentFrame
                << ",\"time\":" << v.time << ",\"playing\":" << (v.playing ? "true" : "false")
                << ",\"loop\":" << (v.loop ? "true" : "false")
                << ",\"enabled\":" << (v.enabled ? "true" : "false") << ",\"frames\":[";
            for (size_t fi = 0; fi < v.framePaths.size(); ++fi) {
                out << (fi ? "," : "") << "\"" << escape_json(v.framePaths[fi]) << "\"";
            }
            out << "]}";
        }
        if (auto gs = scene.gaussianSplatComponents.find(id); gs != scene.gaussianSplatComponents.end()) {
            const auto& v=gs->second;out<<",\n      \"GaussianSplat\":{\"count\":"<<v.count<<",\"scale\":"<<v.scale<<",\"pointSize\":"<<v.pointSize<<",\"opacity\":"<<v.opacity<<",\"seed\":"<<v.seed<<",\"enabled\":"<<(v.enabled?"true":"false")<<"}";
        }
        if (auto ex = scene.expressionComponents.find(id); ex != scene.expressionComponents.end()) {
            const auto& v=ex->second;out<<",\n      \"Expression\":{\"head\":\""<<v.headEntity.to_string()<<"\",\"bsx\":"<<v.baseScale.x<<",\"bsy\":"<<v.baseScale.y<<",\"bsz\":"<<v.baseScale.z<<",\"smile\":"<<v.smile<<",\"frown\":"<<v.frown<<",\"blink\":"<<v.blink<<",\"surprised\":"<<v.surprised<<",\"anger\":"<<v.anger<<",\"enabled\":"<<(v.enabled?"true":"false")<<"}";
        }
        if (auto an = scene.animationComponents.find(id); an != scene.animationComponents.end()) {
            const auto& v=an->second;
            out << ",\n      \"Animation\":{\"playing\":" << (v.playing ? "true" : "false")
                << ",\"entry\":\"" << escape_json(v.entryState) << "\",\"states\":[";
            for (size_t i = 0; i < v.states.size(); ++i) {
                const auto& s = v.states[i];
                if (i) out << ',';
                out << "{\"id\":\"" << escape_json(s.id) << "\",\"clip\":\"" << s.clip.to_string()
                    << "\",\"loop\":" << (s.loop ? "true" : "false") << ",\"speed\":" << s.speed << "}";
            }
            out << "],\"transitions\":[";
            for (size_t i = 0; i < v.transitions.size(); ++i) {
                const auto& t = v.transitions[i];
                if (i) out << ',';
                out << "{\"from\":\"" << escape_json(t.from) << "\",\"to\":\"" << escape_json(t.to)
                    << "\",\"condition\":\"" << escape_json(t.condition)
                    << "\",\"blend\":" << t.blendSeconds << "}";
            }
            out << "]}";
        }
        if (auto tl = scene.timelineComponents.find(id); tl != scene.timelineComponents.end()) {
            const auto& v = tl->second;
            out << ",\n      \"Timeline\":{\"duration\":" << v.duration << ",\"loop\":"
                << (v.loop ? "true" : "false") << ",\"playhead\":" << v.playhead << ",\"tracks\":[";
            for (size_t i = 0; i < v.tracks.size(); ++i) {
                const auto& t = v.tracks[i];
                if (i) out << ',';
                out << "{\"name\":\"" << escape_json(t.name) << "\",\"type\":"
                    << static_cast<int>(t.type) << ",\"muted\":" << (t.muted ? "true" : "false")
                    << ",\"keys\":[";
                for (size_t k = 0; k < t.keys.size(); ++k) {
                    const auto& key = t.keys[k];
                    if (k) out << ',';
                    out << "{\"time\":" << key.time << ",\"value\":\""
                        << escape_json(key.value) << "\"}";
                }
                out << "]}";
            }
            out << "]}";
        }
        if (auto ik = scene.ikComponents.find(id); ik != scene.ikComponents.end()) {
            const auto& v = ik->second;
            out << ",\n      \"IK\":{\"root\":\"" << v.rootEntity.to_string() << "\",\"mid\":\""
                << v.midEntity.to_string() << "\",\"end\":\"" << v.endEntity.to_string()
                << "\",\"target\":\"" << v.targetEntity.to_string() << "\",\"px\":" << v.poleVector.x
                << ",\"py\":" << v.poleVector.y << ",\"pz\":" << v.poleVector.z
                << ",\"weight\":" << v.weight << ",\"iterations\":" << v.iterations
                << ",\"enabled\":" << (v.enabled ? "true" : "false") << "}";
        }
        if (auto rt = scene.retargetComponents.find(id); rt != scene.retargetComponents.end()) {
            const auto& v = rt->second;
            out << ",\n      \"Retarget\":{\"source\":\"" << v.sourceSkeleton.to_string()
                << "\",\"target\":\"" << v.targetSkeleton.to_string()
                << "\",\"rootMotion\":" << (v.preserveRootMotion ? "true" : "false")
                << ",\"mapping\":[";
            for (size_t i = 0; i < v.mapping.size(); ++i) {
                const auto& m = v.mapping[i];
                if (i) out << ',';
                out << "{\"source\":\"" << escape_json(m.sourceBone)
                    << "\",\"target\":\"" << escape_json(m.targetBone)
                    << "\",\"scale\":" << m.translationScale << ",\"ox\":" << m.rotationOffset.x
                    << ",\"oy\":" << m.rotationOffset.y << ",\"oz\":" << m.rotationOffset.z << "}";
            }
            out << "]}";
        }
        out << "\n    }" << (++emitted < scene.m_entities.size() ? "," : "") << '\n';
    }
    out << "  ]\n}\n";
    if (!out) return {false, "Failed while writing scene: " + path.string()};
    return {true, {}};
}

SerializationResult Serializer::deserialize_scene(
    Scene& scene, const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {false, "Cannot open scene for reading: " + path.string()};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string document = buffer.str();
    if (string_field(document, "format") != "VulkanEngine.Scene")
        return {false, "Unsupported scene format"};
    const auto idText = string_field(document, "scene_id");
    const auto name = string_field(document, "name");
    if (!idText || !name) return {false, "Scene header is incomplete"};
    const UUID sceneId = UUID::from_string(*idText);
    if (!sceneId.is_valid()) return {false, "Scene UUID is invalid"};
    const auto entities = array_objects(document, "entities");

    Scene loaded(*name);
    loaded.m_id = sceneId;
    for (const std::string_view object : entities) {
        const auto entityIdText = string_field(object, "id");
        const auto entityName = string_field(object, "name");
        if (!entityIdText || !entityName) return {false, "Entity header is incomplete"};
        const UUID entityId = UUID::from_string(*entityIdText);
        if (!entityId.is_valid() || loaded.m_entities.contains(entityId))
            return {false, "Entity UUID is invalid or duplicated"};
        loaded.create_entity_with_id(entityId, *entityName);
        if (const auto transform = object_field(object, "Transform")) {
            auto& value = loaded.transformComponents.at(entityId);
            value.position = {
                static_cast<float>(number_field(*transform, "px").value_or(0.0)),
                static_cast<float>(number_field(*transform, "py").value_or(0.0)),
                static_cast<float>(number_field(*transform, "pz").value_or(0.0))};
            value.rotation = {
                static_cast<float>(number_field(*transform, "rx").value_or(0.0)),
                static_cast<float>(number_field(*transform, "ry").value_or(0.0)),
                static_cast<float>(number_field(*transform, "rz").value_or(0.0))};
            value.scale = {
                static_cast<float>(number_field(*transform, "sx").value_or(1.0)),
                static_cast<float>(number_field(*transform, "sy").value_or(1.0)),
                static_cast<float>(number_field(*transform, "sz").value_or(1.0))};
        }
        if (const auto light = object_field(object, "Light")) {
            LightComponent value;
            value.color = {
                static_cast<float>(number_field(*light, "r").value_or(1.0)),
                static_cast<float>(number_field(*light, "g").value_or(1.0)),
                static_cast<float>(number_field(*light, "b").value_or(1.0))};
            value.intensity = static_cast<float>(number_field(*light, "intensity").value_or(1000.0));
            value.range = static_cast<float>(number_field(*light, "range").value_or(50.0));
            value.castShadows = bool_field(*light, "castShadows").value_or(true);
            // Type is optional (legacy scenes default to Directional → the
            // range>=50 sun heuristic applies unchanged).
            value.type = static_cast<LightType>(
                static_cast<int>(number_field(*light, "type").value_or(0.0)));
            // coneAngle optional: legacy scenes default to 45° (π/4), exactly
            // the cone the editor/game previously hardcoded — old scenes load
            // with identical spot behavior.
            value.coneAngle = static_cast<float>(
                number_field(*light, "coneAngle").value_or(0.78539816339));
            loaded.lightComponents[entityId] = value;
        }
        if (const auto mesh = object_field(object, "MeshRenderer")) {
            MeshRendererComponent v;v.meshAssetID=UUID::from_string(string_field(*mesh,"mesh").value_or(""));v.materialAssetID=UUID::from_string(string_field(*mesh,"material").value_or(""));v.isVisible=bool_field(*mesh,"visible").value_or(true);v.castShadows=bool_field(*mesh,"castShadows").value_or(true);loaded.meshRendererComponents[entityId]=v;
        }
        if (const auto camera = object_field(object, "Camera")) {
            CameraComponent v;v.fov=static_cast<float>(number_field(*camera,"fov").value_or(60));v.nearPlane=static_cast<float>(number_field(*camera,"near").value_or(.1));v.farPlane=static_cast<float>(number_field(*camera,"far").value_or(1000));v.isPrimary=bool_field(*camera,"primary").value_or(false);loaded.cameraComponents[entityId]=v;
        }
        if (const auto body = object_field(object, "Rigidbody")) {
            RigidbodyComponent v;v.mass=static_cast<float>(number_field(*body,"mass").value_or(1));v.friction=static_cast<float>(number_field(*body,"friction").value_or(.5));v.restitution=static_cast<float>(number_field(*body,"restitution").value_or(.1));v.isKinematic=bool_field(*body,"kinematic").value_or(false);v.useGravity=bool_field(*body,"gravity").value_or(true);loaded.rigidbodyComponents[entityId]=v;
        }
        if (const auto wpn = object_field(object, "Weapon")) {
            WeaponComponent v;v.damage=static_cast<float>(number_field(*wpn,"damage").value_or(25));v.roundsPerMinute=static_cast<float>(number_field(*wpn,"rpm").value_or(600));v.magazineSize=static_cast<uint32_t>(number_field(*wpn,"magazine").value_or(30));v.reserveAmmo=static_cast<uint32_t>(number_field(*wpn,"reserve").value_or(90));v.automatic=bool_field(*wpn,"automatic").value_or(true);v.spreadDegrees=static_cast<float>(number_field(*wpn,"spread").value_or(1.5));v.hitscan=bool_field(*wpn,"hitscan").value_or(true);loaded.weaponComponents[entityId]=v;
        }
        if (const auto pe = object_field(object, "ParticleEmitter")) {
            ParticleEmitterComponent v;v.position={static_cast<float>(number_field(*pe,"px").value_or(0)),static_cast<float>(number_field(*pe,"py").value_or(0)),static_cast<float>(number_field(*pe,"pz").value_or(0))};v.direction={static_cast<float>(number_field(*pe,"dx").value_or(0)),static_cast<float>(number_field(*pe,"dy").value_or(1)),static_cast<float>(number_field(*pe,"dz").value_or(0))};v.coneAngle=static_cast<float>(number_field(*pe,"cone").value_or(.4));v.rate=static_cast<float>(number_field(*pe,"rate").value_or(20));v.speedMin=static_cast<float>(number_field(*pe,"speedMin").value_or(1));v.speedMax=static_cast<float>(number_field(*pe,"speedMax").value_or(3));v.lifetimeMin=static_cast<float>(number_field(*pe,"lifeMin").value_or(.5));v.lifetimeMax=static_cast<float>(number_field(*pe,"lifeMax").value_or(1.5));v.sizeStart=static_cast<float>(number_field(*pe,"sizeStart").value_or(.12));v.sizeEnd=static_cast<float>(number_field(*pe,"sizeEnd").value_or(0));v.colorStart={static_cast<float>(number_field(*pe,"cr").value_or(1)),static_cast<float>(number_field(*pe,"cg").value_or(1)),static_cast<float>(number_field(*pe,"cb").value_or(1)),static_cast<float>(number_field(*pe,"ca").value_or(1))};v.colorEnd={static_cast<float>(number_field(*pe,"er").value_or(1)),static_cast<float>(number_field(*pe,"eg").value_or(1)),static_cast<float>(number_field(*pe,"eb").value_or(1)),static_cast<float>(number_field(*pe,"ea").value_or(0))};v.acceleration={static_cast<float>(number_field(*pe,"ax").value_or(0)),static_cast<float>(number_field(*pe,"ay").value_or(-9.81f)),static_cast<float>(number_field(*pe,"az").value_or(0))};v.drag=static_cast<float>(number_field(*pe,"drag").value_or(.05));v.turbulence=static_cast<float>(number_field(*pe,"turbulence").value_or(0));v.restitution=static_cast<float>(number_field(*pe,"restitution").value_or(.35));v.burstCount=static_cast<uint32_t>(number_field(*pe,"burst").value_or(0));v.collide=bool_field(*pe,"collide").value_or(false);v.emitting=bool_field(*pe,"emitting").value_or(true);loaded.particleEmitterComponents[entityId]=v;
        }
        if (const auto veh = object_field(object, "Vehicle")) {
            VehicleComponent v;v.enginePower=static_cast<float>(number_field(*veh,"enginePower").value_or(4200));v.maxSteerAngle=static_cast<float>(number_field(*veh,"maxSteer").value_or(.55));v.brakeForce=static_cast<float>(number_field(*veh,"brake").value_or(6000));v.wheelRadius=static_cast<float>(number_field(*veh,"wheelRadius").value_or(.36));v.suspensionRest=static_cast<float>(number_field(*veh,"suspRest").value_or(.45));v.wheelBase=static_cast<float>(number_field(*veh,"wheelBase").value_or(2.6f));v.trackWidth=static_cast<float>(number_field(*veh,"track").value_or(1.6f));v.mass=static_cast<float>(number_field(*veh,"mass").value_or(1200));v.frontWheelDrive=bool_field(*veh,"fwd").value_or(true);v.enabled=bool_field(*veh,"enabled").value_or(true);loaded.vehicleComponents[entityId]=v;
        }
        if (const auto rg = object_field(object, "Ragdoll")) {
            RagdollComponent v;v.enabled=bool_field(*rg,"enabled").value_or(true);v.blendWeight=static_cast<float>(number_field(*rg,"blend").value_or(.8));v.fromSkeleton=bool_field(*rg,"fromSkeleton").value_or(false);v.massPerBone=static_cast<float>(number_field(*rg,"massPerBone").value_or(1));v.spawnOffset={static_cast<float>(number_field(*rg,"ox").value_or(0)),static_cast<float>(number_field(*rg,"oy").value_or(0)),static_cast<float>(number_field(*rg,"oz").value_or(0))};loaded.ragdollComponents[entityId]=v;
        }
        if (const auto ms = object_field(object, "Mission")) {
            MissionComponent v;v.missionId=string_field(*ms,"id").value_or("Mission");v.objectiveText=string_field(*ms,"objective").value_or("Complete the mission");v.objectiveTarget=static_cast<uint32_t>(number_field(*ms,"target").value_or(1));v.completeEvent=string_field(*ms,"completeEvent").value_or("MissionComplete");v.autoStart=bool_field(*ms,"autoStart").value_or(true);v.active=bool_field(*ms,"active").value_or(false);loaded.missionComponents[entityId]=v;
        }
        if (const auto dg = object_field(object, "Dialogue")) {
            DialogueComponent v;v.dialogueId=string_field(*dg,"id").value_or("Dialogue");v.character=string_field(*dg,"character").value_or("NPC");v.line=string_field(*dg,"line").value_or("Hello!");v.choiceText=string_field(*dg,"choice").value_or("Continue");v.nextDialogueId=string_field(*dg,"next").value_or("");v.playOnStart=bool_field(*dg,"playOnStart").value_or(false);v.playing=bool_field(*dg,"playing").value_or(false);loaded.dialogueComponents[entityId]=v;
        }
        if (const auto ds = object_field(object, "Destruction")) {
            DestructionComponent v;v.chunkSize={static_cast<float>(number_field(*ds,"csx").value_or(.5)),static_cast<float>(number_field(*ds,"csy").value_or(.5)),static_cast<float>(number_field(*ds,"csz").value_or(.5))};v.chunkCount=static_cast<uint32_t>(number_field(*ds,"chunks").value_or(6));v.chunkHealth=static_cast<float>(number_field(*ds,"health").value_or(25));v.damageRadius=static_cast<float>(number_field(*ds,"radius").value_or(3));v.damageImpulse=static_cast<float>(number_field(*ds,"impulse").value_or(8));v.enabled=bool_field(*ds,"enabled").value_or(true);v.destroyed=bool_field(*ds,"destroyed").value_or(false);loaded.destructionComponents[entityId]=v;
        }
        if (const auto nav = object_field(object, "Navigation")) {
            NavigationComponent v;v.gridWidth=static_cast<int>(number_field(*nav,"gw").value_or(32));v.gridHeight=static_cast<int>(number_field(*nav,"gh").value_or(32));v.cellSize=static_cast<float>(number_field(*nav,"cell").value_or(1));v.agentSpeed=static_cast<float>(number_field(*nav,"speed").value_or(3));v.enabled=bool_field(*nav,"enabled").value_or(true);loaded.navigationComponents[entityId]=v;
        }
        if (const auto au = object_field(object, "Audio")) {
            AudioComponent v;v.clipPath=string_field(*au,"clip").value_or("");v.volume=static_cast<float>(number_field(*au,"volume").value_or(1));v.pitch=static_cast<float>(number_field(*au,"pitch").value_or(1));v.spatial=bool_field(*au,"spatial").value_or(false);v.looping=bool_field(*au,"looping").value_or(false);v.playOnStart=bool_field(*au,"playOnStart").value_or(true);v.playing=bool_field(*au,"playing").value_or(false);loaded.audioComponents[entityId]=v;
        }
        if (const auto material = object_field(object, "Material")) {
            MaterialComponent v;v.albedo={static_cast<float>(number_field(*material,"ar").value_or(1)),static_cast<float>(number_field(*material,"ag").value_or(1)),static_cast<float>(number_field(*material,"ab").value_or(1))};v.roughness=static_cast<float>(number_field(*material,"roughness").value_or(.5));v.metallic=static_cast<float>(number_field(*material,"metallic").value_or(0));v.emissiveColor={static_cast<float>(number_field(*material,"er").value_or(0)),static_cast<float>(number_field(*material,"eg").value_or(0)),static_cast<float>(number_field(*material,"eb").value_or(0))};v.emissiveIntensity=static_cast<float>(number_field(*material,"emissiveIntensity").value_or(0));loaded.materialComponents[entityId]=v;
        }
        if (const auto voxel = object_field(object, "VoxelVolume")) {
            VoxelVolumeComponent v;v.chunkBudget=static_cast<int>(number_field(*voxel,"chunkBudget").value_or(1024));v.seed=static_cast<int>(number_field(*voxel,"seed").value_or(1337));v.seaLevel=static_cast<float>(number_field(*voxel,"seaLevel").value_or(26));v.enableFarLod=bool_field(*voxel,"enableFarLod").value_or(true);loaded.voxelVolumeComponents[entityId]=v;
        }
        // Wicked-port components (frontend; PORTS.md) — mirror of the writer.
        if (const auto cl = object_field(object, "Collider")) {
            ColliderComponent v;v.shape=static_cast<ColliderShape>(static_cast<int>(number_field(*cl,"shape").value_or(0)));v.size={static_cast<float>(number_field(*cl,"sx").value_or(1)),static_cast<float>(number_field(*cl,"sy").value_or(1)),static_cast<float>(number_field(*cl,"sz").value_or(1))};v.radius=static_cast<float>(number_field(*cl,"radius").value_or(.5));v.height=static_cast<float>(number_field(*cl,"height").value_or(1));v.offset={static_cast<float>(number_field(*cl,"ox").value_or(0)),static_cast<float>(number_field(*cl,"oy").value_or(0)),static_cast<float>(number_field(*cl,"oz").value_or(0))};v.isTrigger=bool_field(*cl,"trigger").value_or(false);v.enabled=bool_field(*cl,"enabled").value_or(true);loaded.colliderComponents[entityId]=v;
        }
        if (const auto cn = object_field(object, "Constraint")) {
            ConstraintComponent v;v.type=static_cast<ConstraintType>(static_cast<int>(number_field(*cn,"type").value_or(0)));v.otherEntity=UUID::from_string(string_field(*cn,"other").value_or(""));v.anchor={static_cast<float>(number_field(*cn,"ax").value_or(0)),static_cast<float>(number_field(*cn,"ay").value_or(0)),static_cast<float>(number_field(*cn,"az").value_or(0))};v.axis={static_cast<float>(number_field(*cn,"dx").value_or(0)),static_cast<float>(number_field(*cn,"dy").value_or(1)),static_cast<float>(number_field(*cn,"dz").value_or(0))};v.breakForce=static_cast<float>(number_field(*cn,"breakForce").value_or(0));v.enabled=bool_field(*cn,"enabled").value_or(true);loaded.constraintComponents[entityId]=v;
        }
        if (const auto sb = object_field(object, "SoftBody")) {
            SoftBodyComponent v;v.detail=static_cast<uint32_t>(number_field(*sb,"detail").value_or(8));v.mass=static_cast<float>(number_field(*sb,"mass").value_or(1));v.friction=static_cast<float>(number_field(*sb,"friction").value_or(.5));v.restitution=static_cast<float>(number_field(*sb,"restitution").value_or(.1));v.pressure=static_cast<float>(number_field(*sb,"pressure").value_or(0));v.vertexRadius=static_cast<float>(number_field(*sb,"vertexRadius").value_or(.05));v.wind=bool_field(*sb,"wind").value_or(true);v.enabled=bool_field(*sb,"enabled").value_or(true);loaded.softBodyComponents[entityId]=v;
        }
        if (const auto sp = object_field(object, "Spring")) {
            SpringComponent v;v.stiffness=static_cast<float>(number_field(*sp,"stiffness").value_or(.5));v.drag=static_cast<float>(number_field(*sp,"drag").value_or(.1));v.wind=static_cast<float>(number_field(*sp,"wind").value_or(0));v.gravity=static_cast<float>(number_field(*sp,"gravity").value_or(1));v.hitRadius=static_cast<float>(number_field(*sp,"hitRadius").value_or(.5));v.disabled=bool_field(*sp,"disabled").value_or(false);v.useGravity=bool_field(*sp,"useGravity").value_or(true);v.enabled=bool_field(*sp,"enabled").value_or(true);loaded.springComponents[entityId]=v;
        }
        if (const auto dc = object_field(object, "Decal")) {
            DecalComponent v;v.texturePath=string_field(*dc,"texture").value_or("");v.color={static_cast<float>(number_field(*dc,"r").value_or(1)),static_cast<float>(number_field(*dc,"g").value_or(1)),static_cast<float>(number_field(*dc,"b").value_or(1))};v.slopeBlendPower=static_cast<float>(number_field(*dc,"slope").value_or(1));v.projectOnStatic=bool_field(*dc,"static").value_or(true);v.onlyAlpha=bool_field(*dc,"onlyAlpha").value_or(false);v.enabled=bool_field(*dc,"enabled").value_or(true);loaded.decalComponents[entityId]=v;
        }
        if (const auto sl = object_field(object, "Spline")) {
            SplineComponent v;v.looped=bool_field(*sl,"looped").value_or(false);v.filled=bool_field(*sl,"filled").value_or(false);v.width=static_cast<float>(number_field(*sl,"width").value_or(1));v.rotation=static_cast<float>(number_field(*sl,"rot").value_or(0));v.subdiv=static_cast<uint32_t>(number_field(*sl,"subdiv").value_or(16));v.points.clear();
            // "points":[[x,y,z],...] — minimal parser (JsonMini has no array helper).
            const std::string_view src = *sl;
            const auto arrBegin = src.find("points\"");
            const auto colon = (arrBegin != std::string_view::npos) ? src.find(':', arrBegin) : std::string_view::npos;
            if (colon != std::string_view::npos) {
                size_t pos = src.find('[', colon);
                while (pos != std::string_view::npos) {
                    const auto open = src.find('[', pos + 1);
                    if (open == std::string_view::npos) break;
                    const auto close = src.find(']', open);
                    if (close == std::string_view::npos) break;
                    const std::string_view triple = src.substr(open + 1, close - open - 1);
                    float x = 0.0f, y = 0.0f, z = 0.0f;
                    if (std::sscanf(std::string(triple).c_str(), "%f,%f,%f", &x, &y, &z) == 3) {
                        v.points.push_back({x, y, z});
                    }
                    pos = close;
                }
            }
            loaded.splineComponents[entityId]=v;
        }
        if (const auto ff = object_field(object, "ForceField")) {
            ForceFieldComponent v;v.type=static_cast<ForceFieldType>(static_cast<int>(number_field(*ff,"type").value_or(0)));v.strength=static_cast<float>(number_field(*ff,"strength").value_or(1));v.range=static_cast<float>(number_field(*ff,"range").value_or(10));v.enabled=bool_field(*ff,"enabled").value_or(true);loaded.forceFieldComponents[entityId]=v;
        }
        if (const auto ep = object_field(object, "EnvProbe")) {
            EnvProbeComponent v;v.realTime=bool_field(*ep,"realtime").value_or(false);v.viewDistance=static_cast<float>(number_field(*ep,"viewDistance").value_or(100));v.resolution=static_cast<uint32_t>(number_field(*ep,"resolution").value_or(256));v.enabled=bool_field(*ep,"enabled").value_or(true);loaded.envProbeComponents[entityId]=v;
        }
        if (const auto wt = object_field(object, "Weather")) {
            WeatherComponent v;v.sunColor={static_cast<float>(number_field(*wt,"sr").value_or(1)),static_cast<float>(number_field(*wt,"sg").value_or(.95)),static_cast<float>(number_field(*wt,"sb").value_or(.85))};v.fogDensity=static_cast<float>(number_field(*wt,"fogDensity").value_or(.001));v.fogStart=static_cast<float>(number_field(*wt,"fogStart").value_or(100));v.skyExposure=static_cast<float>(number_field(*wt,"skyExposure").value_or(1));v.skyRotation=static_cast<float>(number_field(*wt,"skyRotation").value_or(0));v.windSpeed=static_cast<float>(number_field(*wt,"windSpeed").value_or(5));v.rainAmount=static_cast<float>(number_field(*wt,"rainAmount").value_or(0));v.rainLength=static_cast<float>(number_field(*wt,"rainLength").value_or(1));v.heightFog=bool_field(*wt,"heightFog").value_or(false);v.enabled=bool_field(*wt,"enabled").value_or(true);loaded.weatherComponents[entityId]=v;
        }
        if (const auto hp = object_field(object, "HairParticle")) {
            HairParticleComponent v;v.meshPath=string_field(*hp,"mesh").value_or("");v.count=static_cast<uint32_t>(number_field(*hp,"count").value_or(1000));v.length=static_cast<float>(number_field(*hp,"length").value_or(.3));v.width=static_cast<float>(number_field(*hp,"width").value_or(.01));v.stiffness=static_cast<float>(number_field(*hp,"stiffness").value_or(.5));v.drag=static_cast<float>(number_field(*hp,"drag").value_or(.1));v.gravityPower=static_cast<float>(number_field(*hp,"gravityPower").value_or(1));v.randomness=static_cast<float>(number_field(*hp,"randomness").value_or(.2));v.segments=static_cast<uint32_t>(number_field(*hp,"segments").value_or(8));v.seed=static_cast<uint32_t>(number_field(*hp,"seed").value_or(0));v.color={static_cast<float>(number_field(*hp,"cr").value_or(.95)),static_cast<float>(number_field(*hp,"cg").value_or(.88)),static_cast<float>(number_field(*hp,"cb").value_or(.78))};v.enabled=bool_field(*hp,"enabled").value_or(true);loaded.hairParticleComponents[entityId]=v;
        }
        if (const auto ly = object_field(object, "Layer")) {
            LayerComponent v;v.name=string_field(*ly,"name").value_or("Default");v.index=static_cast<int>(number_field(*ly,"index").value_or(0));v.visible=bool_field(*ly,"visible").value_or(true);v.locked=bool_field(*ly,"locked").value_or(false);v.enabled=bool_field(*ly,"enabled").value_or(true);loaded.layerComponents[entityId]=v;
        }
        if (const auto pc = object_field(object, "Paint")) {
            PaintComponent v;v.brushColor={static_cast<float>(number_field(*pc,"r").value_or(1)),static_cast<float>(number_field(*pc,"g").value_or(.3)),static_cast<float>(number_field(*pc,"b").value_or(.22))};v.brushSize=static_cast<float>(number_field(*pc,"brush").value_or(.5));v.opacity=static_cast<float>(number_field(*pc,"opacity").value_or(1));v.paintMode=bool_field(*pc,"mode").value_or(false);
            const std::string_view src = *pc;
            const auto arrBegin = src.find("colors\"");
            const auto colon = (arrBegin != std::string_view::npos) ? src.find(':', arrBegin) : std::string_view::npos;
            if (colon != std::string_view::npos) {
                size_t pos = src.find('[', colon);
                while (pos != std::string_view::npos) {
                    const auto open = src.find('[', pos + 1);
                    if (open == std::string_view::npos) break;
                    const auto close = src.find(']', open);
                    if (close == std::string_view::npos) break;
                    const std::string_view quad = src.substr(open + 1, close - open - 1);
                    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
                    if (std::sscanf(std::string(quad).c_str(), "%f,%f,%f,%f", &x, &y, &z, &w) == 4) {
                        v.vertexColors.push_back({x, y, z, w});
                    }
                    pos = close;
                }
            }
            loaded.paintComponents[entityId]=v;
        }
        if (const auto vc = object_field(object, "Video")) {
            VideoComponent v;v.fps=static_cast<float>(number_field(*vc,"fps").value_or(24));v.currentFrame=static_cast<int>(number_field(*vc,"frame").value_or(0));v.time=static_cast<float>(number_field(*vc,"time").value_or(0));v.playing=bool_field(*vc,"playing").value_or(true);v.loop=bool_field(*vc,"loop").value_or(true);v.enabled=bool_field(*vc,"enabled").value_or(true);
            const std::string_view src = *vc;
            const auto arrBegin = src.find("frames\"");
            const auto colon = (arrBegin != std::string_view::npos) ? src.find(':', arrBegin) : std::string_view::npos;
            if (colon != std::string_view::npos) {
                size_t pos = src.find('[', colon);
                while (pos != std::string_view::npos) {
                    const auto q = src.find('"', pos + 1);
                    if (q == std::string_view::npos) break;
                    const auto qe = src.find('"', q + 1);
                    if (qe == std::string_view::npos) break;
                    v.framePaths.push_back(std::string(src.substr(q + 1, qe - q - 1)));
                    pos = qe;
                }
            }
            loaded.videoComponents[entityId]=v;
        }
        if (const auto gs = object_field(object, "GaussianSplat")) {
            GaussianSplatComponent v;v.count=static_cast<uint32_t>(number_field(*gs,"count").value_or(8000));v.scale=static_cast<float>(number_field(*gs,"scale").value_or(3));v.pointSize=static_cast<float>(number_field(*gs,"pointSize").value_or(6));v.opacity=static_cast<float>(number_field(*gs,"opacity").value_or(.6));v.seed=static_cast<uint32_t>(number_field(*gs,"seed").value_or(1));v.enabled=bool_field(*gs,"enabled").value_or(true);loaded.gaussianSplatComponents[entityId]=v;
        }
        if (const auto ex = object_field(object, "Expression")) {
            ExpressionComponent v;v.headEntity=UUID::from_string(string_field(*ex,"head").value_or(""));v.baseScale={static_cast<float>(number_field(*ex,"bsx").value_or(1)),static_cast<float>(number_field(*ex,"bsy").value_or(1)),static_cast<float>(number_field(*ex,"bsz").value_or(1))};v.smile=static_cast<float>(number_field(*ex,"smile").value_or(0));v.frown=static_cast<float>(number_field(*ex,"frown").value_or(0));v.blink=static_cast<float>(number_field(*ex,"blink").value_or(0));v.surprised=static_cast<float>(number_field(*ex,"surprised").value_or(0));v.anger=static_cast<float>(number_field(*ex,"anger").value_or(0));v.enabled=bool_field(*ex,"enabled").value_or(true);loaded.expressionComponents[entityId]=v;
        }
        if (const auto an = object_field(object, "Animation")) {
            AnimationComponent v;
            v.playing = bool_field(*an, "playing").value_or(true);
            v.entryState = string_field(*an, "entry").value_or("");
            for (const auto st : array_objects(*an, "states")) {
                AnimationStateDef s;
                s.id = string_field(st, "id").value_or("");
                s.clip = UUID::from_string(string_field(st, "clip").value_or(""));
                s.loop = bool_field(st, "loop").value_or(true);
                s.speed = static_cast<float>(number_field(st, "speed").value_or(1.0));
                v.states.push_back(s);
            }
            for (const auto tr : array_objects(*an, "transitions")) {
                AnimationTransitionDef t;
                t.from = string_field(tr, "from").value_or("");
                t.to = string_field(tr, "to").value_or("");
                t.condition = string_field(tr, "condition").value_or("");
                t.blendSeconds = static_cast<float>(number_field(tr, "blend").value_or(0.2));
                v.transitions.push_back(t);
            }
            loaded.animationComponents[entityId] = v;
        }
        if (const auto tl = object_field(object, "Timeline")) {
            TimelineComponent v;
            v.duration = static_cast<float>(number_field(*tl, "duration").value_or(1.0));
            v.loop = bool_field(*tl, "loop").value_or(false);
            v.playhead = static_cast<float>(number_field(*tl, "playhead").value_or(0.0));
            for (const auto tr : array_objects(*tl, "tracks")) {
                TimelineTrackDef t;
                t.name = string_field(tr, "name").value_or("");
                t.type = static_cast<uint8_t>(static_cast<int>(number_field(tr, "type").value_or(0)));
                t.muted = bool_field(tr, "muted").value_or(false);
                for (const auto k : array_objects(tr, "keys")) {
                    TimelineKeyDef key;
                    key.time = static_cast<float>(number_field(k, "time").value_or(0.0));
                    key.value = string_field(k, "value").value_or("");
                    t.keys.push_back(key);
                }
                v.tracks.push_back(t);
            }
            loaded.timelineComponents[entityId] = v;
        }
        if (const auto ik = object_field(object, "IK")) {
            IKComponent v;
            v.rootEntity = UUID::from_string(string_field(*ik, "root").value_or(""));
            v.midEntity = UUID::from_string(string_field(*ik, "mid").value_or(""));
            v.endEntity = UUID::from_string(string_field(*ik, "end").value_or(""));
            v.targetEntity = UUID::from_string(string_field(*ik, "target").value_or(""));
            v.poleVector = {static_cast<float>(number_field(*ik, "px").value_or(0)), static_cast<float>(number_field(*ik, "py").value_or(1)), static_cast<float>(number_field(*ik, "pz").value_or(0))};
            v.weight = static_cast<float>(number_field(*ik, "weight").value_or(1.0));
            v.iterations = static_cast<int>(number_field(*ik, "iterations").value_or(8));
            v.enabled = bool_field(*ik, "enabled").value_or(true);
            loaded.ikComponents[entityId] = v;
        }
        if (const auto rt = object_field(object, "Retarget")) {
            RetargetComponent v;
            v.sourceSkeleton = UUID::from_string(string_field(*rt, "source").value_or(""));
            v.targetSkeleton = UUID::from_string(string_field(*rt, "target").value_or(""));
            v.preserveRootMotion = bool_field(*rt, "rootMotion").value_or(true);
            for (const auto mp : array_objects(*rt, "mapping")) {
                RetargetBoneMapDef m;
                m.sourceBone = string_field(mp, "source").value_or("");
                m.targetBone = string_field(mp, "target").value_or("");
                m.translationScale = static_cast<float>(number_field(mp, "scale").value_or(1.0));
                m.rotationOffset = {static_cast<float>(number_field(mp, "ox").value_or(0)), static_cast<float>(number_field(mp, "oy").value_or(0)), static_cast<float>(number_field(mp, "oz").value_or(0))};
                v.mapping.push_back(m);
            }
            loaded.retargetComponents[entityId] = v;
        }
        if (const auto hier = object_field(object, "Hierarchy")) {
            if (const auto parentIdText = string_field(*hier, "parent_id")) {
                const UUID parentId = UUID::from_string(*parentIdText);
                if (parentId.is_valid()) {
                    loaded.hierarchyComponents[entityId].parentID = parentId;
                }
            }
        }
    }
    std::vector<std::pair<UUID, UUID>> parentLinks;
    for (const auto& [childId, hier] : loaded.hierarchyComponents) {
        if (hier.parentID.is_valid()) {
            parentLinks.emplace_back(childId, hier.parentID);
        }
    }
    for (const auto& [childId, parentId] : parentLinks) {
        if (loaded.m_entities.contains(parentId)) {
            auto& parentChildren = loaded.hierarchyComponents[parentId].childrenIDs;
            if (std::find(parentChildren.begin(), parentChildren.end(), childId) == parentChildren.end()) {
                parentChildren.push_back(childId);
            }
        }
    }
    scene = std::move(loaded);
    return {true, {}};
}

SerializationResult Serializer::serialize_prefab(
    const Prefab& prefab, const std::filesystem::path& path) {
    std::error_code error;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return {false, "Cannot open prefab for writing: " + path.string()};
    out << std::setprecision(9);
    out << "{\n  \"format\":\"VulkanEngine.Prefab\",\n  \"version\":2,\n";
    out << "  \"prefab_id\":\"" << prefab.m_id.to_string() << "\",\n";
    out << "  \"name\":\"" << escape_json(prefab.m_name) << "\",\n";
    out << "  \"captured\":" << (prefab.m_captured ? "true" : "false") << ",\n";
    out << "  \"root_local_id\":\"" << prefab.m_rootLocalID.to_string() << "\",\n";
    out << "  \"entities\":[\n";
    for (std::size_t i = 0; i < prefab.m_entities.size(); ++i) {
        const auto& entity = prefab.m_entities[i];
        out << "    {\"local_id\":\"" << entity.localID.to_string()
            << "\",\"source_id\":\"" << entity.sourceEntityID.to_string()
            << "\",\"parent_local_id\":\"" << entity.parentLocalID.to_string()
            << "\",\"name\":\"" << escape_json(entity.name) << "\",\"components\":{";
        write_prefab_components(out, entity);
        out << "}}" << (i + 1 < prefab.m_entities.size() ? "," : "") << '\n';
    }
    out << "  ],\n  \"nested_prefabs\":[\n";
    for (std::size_t i = 0; i < prefab.m_nestedPrefabs.size(); ++i) {
        const auto& nested = prefab.m_nestedPrefabs[i];
        out << "    {\"parent_local_id\":\"" << nested.parentLocalID.to_string()
            << "\",\"prefab_id\":\"" << nested.prefabID.to_string() << "\"}"
            << (i + 1 < prefab.m_nestedPrefabs.size() ? "," : "") << '\n';
    }
    out << "  ],\n  \"overrides\":[\n";
    std::size_t emitted = 0;
    for (const auto& item : prefab.m_overrides) {
        const std::string type = override_type(item.value);
        if (item.kind != PrefabOverrideKind::Property || type.empty()) continue;
        if (emitted++) out << ",\n";
        out << "    {\"local_id\":\"" << item.localEntityID.to_string()
            << "\",\"component\":\"" << escape_json(item.componentName)
            << "\",\"field\":\"" << escape_json(item.fieldName)
            << "\",\"type\":\"" << type << "\",\"value\":";
        write_override_value(out, item.value, type);
        out << '}';
    }
    if (emitted) out << '\n';
    out << "  ]\n}\n";
    if (!out) return {false, "Failed while writing prefab: " + path.string()};
    return {true, {}};
}

SerializationResult Serializer::deserialize_prefab(
    Prefab& prefab, const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {false, "Cannot open prefab for reading: " + path.string()};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string document = buffer.str();
    if (string_field(document, "format") != "VulkanEngine.Prefab")
        return {false, "Unsupported prefab format"};
    const auto idText = string_field(document, "prefab_id");
    const auto name = string_field(document, "name");
    if (!idText || !name) return {false, "Prefab header is incomplete"};
    const UUID prefabId = UUID::from_string(*idText);
    if (!prefabId.is_valid()) return {false, "Prefab UUID is invalid"};

    Prefab loaded(prefabId, *name);
    loaded.m_captured = bool_field(document, "captured").value_or(true);
    const bool version2 = number_field(document, "version").value_or(1.0) >= 2.0;
    const auto entityObjects = array_objects(document, "entities");
    if (version2) {
        loaded.m_rootLocalID = UUID::from_string(string_field(document, "root_local_id").value_or(""));
        std::unordered_set<UUID> localIDs;
        for (const auto object : entityObjects) {
            PrefabEntityData entity;
            entity.localID = UUID::from_string(string_field(object, "local_id").value_or(""));
            entity.sourceEntityID = UUID::from_string(string_field(object, "source_id").value_or(""));
            entity.parentLocalID = UUID::from_string(string_field(object, "parent_local_id").value_or(""));
            entity.name = string_field(object, "name").value_or("Entity");
            if (!entity.localID.is_valid() || !localIDs.insert(entity.localID).second)
                return {false, "Prefab entity local ID is invalid or duplicated"};
            read_prefab_components(object, entity);
            loaded.m_entities.push_back(std::move(entity));
        }
        if ((!loaded.m_entities.empty() || loaded.m_captured) &&
            (!loaded.m_rootLocalID.is_valid() || !localIDs.contains(loaded.m_rootLocalID)))
            return {false, "Prefab root local ID is invalid"};
        for (const auto& entity : loaded.m_entities)
            if (entity.parentLocalID.is_valid() && !localIDs.contains(entity.parentLocalID))
                return {false, "Prefab hierarchy references a missing local ID"};

        for (const auto object : array_objects(document, "nested_prefabs")) {
            NestedPrefabReference nested;
            nested.parentLocalID = UUID::from_string(string_field(object, "parent_local_id").value_or(""));
            nested.prefabID = UUID::from_string(string_field(object, "prefab_id").value_or(""));
            if (!localIDs.contains(nested.parentLocalID) || !nested.prefabID.is_valid())
                return {false, "Nested prefab reference is invalid"};
            loaded.m_nestedPrefabs.push_back(nested); // Resolved later through bind_nested_prefab().
        }
        for (const auto object : array_objects(document, "overrides")) {
            PropertyOverride item;
            item.localEntityID = UUID::from_string(string_field(object, "local_id").value_or(""));
            item.componentName = string_field(object, "component").value_or("");
            item.fieldName = string_field(object, "field").value_or("");
            const std::string type = string_field(object, "type").value_or("");
            item.value = read_override_value(object, type);
            if (!localIDs.contains(item.localEntityID) || item.componentName.empty() ||
                item.fieldName.empty() || !item.value.has_value())
                return {false, "Prefab override is invalid"};
            PrefabEntityData* target = loaded.find_entity(item.localEntityID);
            PrefabEntityData validation = target ? *target : PrefabEntityData{};
            if (!target || !loaded.apply_property(validation, item))
                return {false, "Prefab override type or property is invalid"};
            loaded.m_overrides.push_back(std::move(item));
        }
    } else {
        // Version 1 compatibility: promote the original single component bag to one local entity.
        PrefabEntityData root;
        root.localID = UUID();
        root.name = *name;
        root.sourceEntityID = UUID{0, 0};
        if (const auto comps = object_field(document, "components")) {
            const std::string wrapper = "{\"components\":" + std::string(*comps) + "}";
            read_prefab_components(wrapper, root);
        }
        loaded.m_rootLocalID = root.localID;
        loaded.m_entities.push_back(std::move(root));
    }
    if (loaded.m_captured && loaded.m_entities.empty()) return {false, "Captured prefab contains no entities"};
    prefab = std::move(loaded);
    return {true, {}};
}

SerializationResult Serializer::serialize_material(
    const MaterialAsset& material, const std::filesystem::path& path) {
    std::error_code error;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return {false, "Cannot open material for writing: " + path.string()};
    out << std::setprecision(9);
    out << "{\n  \"format\":\"VulkanEngine.Material\",\n  \"version\":1,\n";
    out << "  \"material_id\":\"" << material.id.to_string() << "\",\n";
    out << "  \"name\":\"" << escape_json(material.name) << "\",\n";
    out << "  \"albedo\":{\"r\":" << material.albedo.r << ",\"g\":" << material.albedo.g << ",\"b\":" << material.albedo.b << "},\n";
    out << "  \"roughness\":" << material.roughness << ",\n";
    out << "  \"metallic\":" << material.metallic << ",\n";
    out << "  \"emissiveColor\":{\"r\":" << material.emissiveColor.r << ",\"g\":" << material.emissiveColor.g << ",\"b\":" << material.emissiveColor.b << "},\n";
    out << "  \"emissiveIntensity\":" << material.emissiveIntensity << ",\n";
    out << "  \"albedoMapID\":\"" << material.albedoMapID.to_string() << "\",\n";
    out << "  \"normalMapID\":\"" << material.normalMapID.to_string() << "\",\n";
    out << "  \"roughnessMapID\":\"" << material.roughnessMapID.to_string() << "\",\n";
    out << "  \"metallicMapID\":\"" << material.metallicMapID.to_string() << "\"\n";
    out << "}\n";
    if (!out) return {false, "Failed while writing material: " + path.string()};
    return {true, {}};
}

SerializationResult Serializer::deserialize_material(
    MaterialAsset& material, const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {false, "Cannot open material for reading: " + path.string()};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string document = buffer.str();
    if (string_field(document, "format") != "VulkanEngine.Material")
        return {false, "Unsupported material format"};
    const auto idText = string_field(document, "material_id");
    const auto name = string_field(document, "name");
    if (!idText || !name) return {false, "Material header is incomplete"};
    const UUID matId = UUID::from_string(*idText);

    MaterialAsset loaded;
    loaded.id = matId.is_valid() ? matId : UUID();
    loaded.name = *name;
    loaded.roughness = static_cast<float>(number_field(document, "roughness").value_or(0.5));
    loaded.metallic = static_cast<float>(number_field(document, "metallic").value_or(0.0));
    loaded.emissiveIntensity = static_cast<float>(number_field(document, "emissiveIntensity").value_or(0.0));

    if (const auto alb = object_field(document, "albedo")) {
        loaded.albedo = {
            static_cast<float>(number_field(*alb, "r").value_or(1.0)),
            static_cast<float>(number_field(*alb, "g").value_or(1.0)),
            static_cast<float>(number_field(*alb, "b").value_or(1.0))};
    }
    if (const auto em = object_field(document, "emissiveColor")) {
        loaded.emissiveColor = {
            static_cast<float>(number_field(*em, "r").value_or(0.0)),
            static_cast<float>(number_field(*em, "g").value_or(0.0)),
            static_cast<float>(number_field(*em, "b").value_or(0.0))};
    }
    if (const auto albMap = string_field(document, "albedoMapID")) loaded.albedoMapID = UUID::from_string(*albMap);
    if (const auto normMap = string_field(document, "normalMapID")) loaded.normalMapID = UUID::from_string(*normMap);
    if (const auto roughMap = string_field(document, "roughnessMapID")) loaded.roughnessMapID = UUID::from_string(*roughMap);
    if (const auto metMap = string_field(document, "metallicMapID")) loaded.metallicMapID = UUID::from_string(*metMap);

    material = std::move(loaded);
    return {true, {}};
}

SerializationResult Serializer::serialize_audio_event(
    const AudioEventAsset& audioEvent, const std::filesystem::path& path) {
    std::error_code error;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return {false, "Cannot open audio event for writing: " + path.string()};
    out << std::setprecision(9);
    out << "{\n  \"format\":\"VulkanEngine.AudioEvent\",\n  \"version\":1,\n";
    out << "  \"audio_event_id\":\"" << audioEvent.id.to_string() << "\",\n";
    out << "  \"name\":\"" << escape_json(audioEvent.name) << "\",\n";
    out << "  \"clipPath\":\"" << escape_json(audioEvent.clipPath) << "\",\n";
    out << "  \"volume\":" << audioEvent.volume << ",\n";
    out << "  \"minPitch\":" << audioEvent.minPitch << ",\n";
    out << "  \"maxPitch\":" << audioEvent.maxPitch << ",\n";
    out << "  \"maxDistance\":" << audioEvent.maxDistance << ",\n";
    out << "  \"is3D\":" << (audioEvent.is3D ? "true" : "false") << ",\n";
    out << "  \"isLooping\":" << (audioEvent.isLooping ? "true" : "false") << "\n";
    out << "}\n";
    if (!out) return {false, "Failed while writing audio event: " + path.string()};
    return {true, {}};
}

SerializationResult Serializer::deserialize_audio_event(
    AudioEventAsset& audioEvent, const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {false, "Cannot open audio event for reading: " + path.string()};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string document = buffer.str();
    if (string_field(document, "format") != "VulkanEngine.AudioEvent")
        return {false, "Unsupported audio event format"};
    const auto idText = string_field(document, "audio_event_id");
    const auto name = string_field(document, "name");
    if (!idText || !name) return {false, "Audio event header is incomplete"};
    const UUID audioId = UUID::from_string(*idText);

    AudioEventAsset loaded;
    loaded.id = audioId.is_valid() ? audioId : UUID();
    loaded.name = *name;
    loaded.clipPath = string_field(document, "clipPath").value_or("");
    loaded.volume = static_cast<float>(number_field(document, "volume").value_or(1.0));
    loaded.minPitch = static_cast<float>(number_field(document, "minPitch").value_or(0.9));
    loaded.maxPitch = static_cast<float>(number_field(document, "maxPitch").value_or(1.1));
    loaded.maxDistance = static_cast<float>(number_field(document, "maxDistance").value_or(100.0));
    loaded.is3D = bool_field(document, "is3D").value_or(true);
    loaded.isLooping = bool_field(document, "isLooping").value_or(false);

    audioEvent = std::move(loaded);
    return {true, {}};
}

SerializationResult Serializer::serialize_physics_material(
    const PhysicsMaterialAsset& mat, const std::filesystem::path& path) {
    std::error_code error;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return {false, "Cannot open physics material for writing: " + path.string()};
    out << std::setprecision(9);
    out << "{\n  \"format\":\"VulkanEngine.PhysicsMaterial\",\n  \"version\":1,\n";
    out << "  \"physics_material_id\":\"" << mat.id.to_string() << "\",\n";
    out << "  \"name\":\"" << escape_json(mat.name) << "\",\n";
    out << "  \"friction\":" << mat.friction << ",\n";
    out << "  \"restitution\":" << mat.restitution << ",\n";
    out << "  \"density\":" << mat.density << ",\n";
    out << "  \"hardness\":" << mat.hardness << "\n";
    out << "}\n";
    if (!out) return {false, "Failed while writing physics material: " + path.string()};
    return {true, {}};
}

SerializationResult Serializer::deserialize_physics_material(
    PhysicsMaterialAsset& mat, const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {false, "Cannot open physics material for reading: " + path.string()};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string document = buffer.str();
    if (string_field(document, "format") != "VulkanEngine.PhysicsMaterial")
        return {false, "Unsupported physics material format"};
    const auto idText = string_field(document, "physics_material_id");
    const auto name = string_field(document, "name");
    if (!idText || !name) return {false, "Physics material header is incomplete"};
    const UUID matId = UUID::from_string(*idText);

    PhysicsMaterialAsset loaded;
    loaded.id = matId.is_valid() ? matId : UUID();
    loaded.name = *name;
    loaded.friction = static_cast<float>(number_field(document, "friction").value_or(0.5));
    loaded.restitution = static_cast<float>(number_field(document, "restitution").value_or(0.1));
    loaded.density = static_cast<float>(number_field(document, "density").value_or(1000.0));
    loaded.hardness = static_cast<float>(number_field(document, "hardness").value_or(0.5));

    mat = std::move(loaded);
    return {true, {}};
}

SerializationResult Serializer::serialize_visual_script(
    const VisualScriptGraph& script, const std::filesystem::path& path) {
    std::error_code error;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return {false, "Cannot open visual script for writing: " + path.string()};
    out << std::setprecision(9);
    out << "{\n  \"format\":\"VulkanEngine.VisualScript\",\n  \"version\":1,\n";
    out << "  \"script_id\":\"" << script.id.to_string() << "\",\n";
    out << "  \"name\":\"" << escape_json(script.name) << "\",\n";
    out << "  \"nodes\":[\n";
    for (size_t i = 0; i < script.nodes.size(); ++i) {
        const auto& node = script.nodes[i];
        out << "    {\"id\":\"" << node.id.to_string() << "\",\"title\":\"" << escape_json(node.title) << "\"}"
            << (i + 1 < script.nodes.size() ? ",\n" : "\n");
    }
    out << "  ],\n  \"connections\":[\n";
    for (size_t i = 0; i < script.connections.size(); ++i) {
        const auto& conn = script.connections[i];
        out << "    {\"from\":\"" << conn.fromPinID.to_string() << "\",\"to\":\"" << conn.toPinID.to_string() << "\"}"
            << (i + 1 < script.connections.size() ? ",\n" : "\n");
    }
    out << "  ]\n}\n";
    if (!out) return {false, "Failed while writing visual script: " + path.string()};
    return {true, {}};
}

SerializationResult Serializer::deserialize_visual_script(
    VisualScriptGraph& script, const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {false, "Cannot open visual script for reading: " + path.string()};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string document = buffer.str();
    if (string_field(document, "format") != "VulkanEngine.VisualScript")
        return {false, "Unsupported visual script format"};
    const auto idText = string_field(document, "script_id");
    const auto name = string_field(document, "name");
    if (!idText || !name) return {false, "Visual script header is incomplete"};
    const UUID scriptId = UUID::from_string(*idText);

    VisualScriptGraph loaded;
    loaded.id = scriptId.is_valid() ? scriptId : UUID();
    loaded.name = *name;

    const auto nodesArr = array_objects(document, "nodes");
    for (const auto nodeObj : nodesArr) {
        const auto nid = string_field(nodeObj, "id");
        const auto ntitle = string_field(nodeObj, "title");
        if (nid && ntitle) {
            ScriptNode node;
            node.id = UUID::from_string(*nid);
            node.title = *ntitle;
            loaded.add_node(node);
        }
    }
    const auto connsArr = array_objects(document, "connections");
    for (const auto connObj : connsArr) {
        const auto fromStr = string_field(connObj, "from");
        const auto toStr = string_field(connObj, "to");
        if (fromStr && toStr) {
            loaded.connect_pins(UUID::from_string(*fromStr), UUID::from_string(*toStr));
        }
    }

    script = std::move(loaded);
    return {true, {}};
}

} // namespace Engine
