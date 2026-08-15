#include "Serializer.hpp"

#include "../../scene/Scene.hpp"
#include "../../scene/Prefab.hpp"
#include "../../rendering/materials/Material.hpp"
#include "../../audio/AudioEvent.hpp"
#include "../../physics/Physics.hpp"
#include "../../scripting/VisualScriptGraph.hpp"

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
            << ",\"castShadows\":" << (v.castShadows ? "true" : "false") << '}';
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
                << static_cast<int>(value.type) << "}";
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
