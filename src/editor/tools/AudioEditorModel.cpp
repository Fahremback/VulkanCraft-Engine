#include "AudioEditorModel.hpp"

#include <fstream>
#include <sstream>

#include "../../engine/core/serialization/JsonMini.hpp"

namespace Engine::Editor {

namespace {

Json::Value vec3_to_json(const glm::vec3& v) {
    Json::Value out = Json::Value::make_array();
    out.push(static_cast<double>(v.x));
    out.push(static_cast<double>(v.y));
    out.push(static_cast<double>(v.z));
    return out;
}

glm::vec3 vec3_from_json(const Json::Value& v, glm::vec3 fallback = {0.0f, 0.0f, 0.0f}) {
    if (!v.is_array() || v.size() != 3) return fallback;
    const Json::Value* x = v.at(0);
    const Json::Value* y = v.at(1);
    const Json::Value* z = v.at(2);
    if (!x || !y || !z || !x->is_number() || !y->is_number() || !z->is_number()) return fallback;
    return {static_cast<float>(x->as_number()), static_cast<float>(y->as_number()),
            static_cast<float>(z->as_number())};
}

Json::Value variation_to_json(const AudioVariationModel& v) {
    Json::Value out = Json::Value::make_object();
    out["clip"] = v.clip.to_string();
    out["weight"] = static_cast<double>(v.weight);
    out["volume"] = static_cast<double>(v.volume);
    out["pitch"] = static_cast<double>(v.pitch);
    return out;
}

bool variation_from_json(const Json::Value& v, AudioVariationModel& out) {
    const Json::Value* clip = v.find("clip");
    if (!clip || !clip->is_string()) return false;
    out.clip = UUID::from_string(clip->as_string());
    out.weight = static_cast<float>(v.find("weight") ? v.find("weight")->as_number() : 1.0);
    out.volume = static_cast<float>(v.find("volume") ? v.find("volume")->as_number() : 1.0);
    out.pitch = static_cast<float>(v.find("pitch") ? v.find("pitch")->as_number() : 1.0);
    return true;
}

Json::Value bus_to_json(const AudioBusModel& bus) {
    Json::Value out = Json::Value::make_object();
    out["id"] = static_cast<int64_t>(bus.id);
    out["name"] = bus.name;
    out["parent"] = static_cast<int64_t>(bus.parent);
    out["gain"] = static_cast<double>(bus.gain);
    out["muted"] = bus.muted;
    Json::Value effects = Json::Value::make_array();
    for (const AudioBusEffectModel& effect : bus.effects) {
        Json::Value entry = Json::Value::make_object();
        entry["kind"] = static_cast<int64_t>(effect.kind);
        entry["amount"] = static_cast<double>(effect.amount);
        effects.push(std::move(entry));
    }
    out["effects"] = std::move(effects);
    return out;
}

bool bus_from_json(const Json::Value& v, AudioBusModel& out) {
    const Json::Value* id = v.find("id");
    if (!id || !id->is_number()) return false;
    out.id = static_cast<uint32_t>(id->as_int());
    out.name = v.find("name") ? v.find("name")->as_string() : std::string();
    out.parent = static_cast<uint32_t>(v.find("parent") ? v.find("parent")->as_int() : 0);
    out.gain = static_cast<float>(v.find("gain") ? v.find("gain")->as_number() : 1.0);
    out.muted = v.find("muted") ? v.find("muted")->as_bool() : false;
    const Json::Value* effects = v.find("effects");
    if (effects && effects->is_array()) {
        for (const Json::Value& entry : effects->array()) {
            AudioBusEffectModel effect;
            effect.kind = static_cast<AudioBusEffectKind>(
                entry.find("kind") ? entry.find("kind")->as_int() : 0);
            effect.amount = static_cast<float>(
                entry.find("amount") ? entry.find("amount")->as_number() : 0.0);
            out.effects.push_back(effect);
        }
    }
    return true;
}

Json::Value reverb_zone_to_json(const AudioReverbZoneModel& zone) {
    Json::Value out = Json::Value::make_object();
    out["name"] = zone.name;
    out["center"] = vec3_to_json(zone.center);
    out["halfExtents"] = vec3_to_json(zone.halfExtents);
    out["wet"] = static_cast<double>(zone.wet);
    out["decay"] = static_cast<double>(zone.decay);
    out["preDelay"] = static_cast<double>(zone.preDelay);
    return out;
}

bool reverb_zone_from_json(const Json::Value& v, AudioReverbZoneModel& out) {
    out.name = v.find("name") ? v.find("name")->as_string() : std::string("ReverbZone");
    out.center = vec3_from_json(v.find("center") ? *v.find("center") : Json::Value());
    out.halfExtents = vec3_from_json(v.find("halfExtents") ? *v.find("halfExtents") : Json::Value(),
                                     {5.0f, 5.0f, 5.0f});
    out.wet = static_cast<float>(v.find("wet") ? v.find("wet")->as_number() : 0.3);
    out.decay = static_cast<float>(v.find("decay") ? v.find("decay")->as_number() : 0.35);
    out.preDelay = static_cast<float>(v.find("preDelay") ? v.find("preDelay")->as_number() : 0.02);
    return true;
}

Json::Value ambient_zone_to_json(const AudioAmbientZoneModel& zone) {
    Json::Value out = Json::Value::make_object();
    out["name"] = zone.name;
    out["center"] = vec3_to_json(zone.center);
    out["halfExtents"] = vec3_to_json(zone.halfExtents);
    out["clip"] = zone.clipPath;
    out["gain"] = static_cast<double>(zone.gain);
    return out;
}

bool ambient_zone_from_json(const Json::Value& v, AudioAmbientZoneModel& out) {
    out.name = v.find("name") ? v.find("name")->as_string() : std::string("AmbientZone");
    out.center = vec3_from_json(v.find("center") ? *v.find("center") : Json::Value());
    out.halfExtents = vec3_from_json(v.find("halfExtents") ? *v.find("halfExtents") : Json::Value(),
                                     {10.0f, 10.0f, 10.0f});
    out.clipPath = v.find("clip") ? v.find("clip")->as_string() : std::string();
    out.gain = static_cast<float>(v.find("gain") ? v.find("gain")->as_number() : 0.7);
    return true;
}

} // namespace

std::string AudioEditorModel::to_json() const {
    Json::Value root = Json::Value::make_object();
    root["format"] = "VulkanEngine.AudioEditor";
    root["version"] = 1;
    root["name"] = name;
    root["bus"] = bus;
    root["volume"] = static_cast<double>(volume);
    root["minPitch"] = static_cast<double>(minPitch);
    root["maxPitch"] = static_cast<double>(maxPitch);
    root["minDistance"] = static_cast<double>(minDistance);
    root["maxDistance"] = static_cast<double>(maxDistance);
    root["spatial"] = spatial;
    root["looping"] = looping;

    Json::Value variationsJson = Json::Value::make_array();
    for (const AudioVariationModel& v : variations) variationsJson.push(variation_to_json(v));
    root["variations"] = std::move(variationsJson);

    Json::Value busesJson = Json::Value::make_array();
    for (const AudioBusModel& b : buses) busesJson.push(bus_to_json(b));
    root["buses"] = std::move(busesJson);

    Json::Value reverbJson = Json::Value::make_array();
    for (const AudioReverbZoneModel& z : reverbZones) reverbJson.push(reverb_zone_to_json(z));
    root["reverbZones"] = std::move(reverbJson);

    Json::Value ambientJson = Json::Value::make_array();
    for (const AudioAmbientZoneModel& z : ambientZones) ambientJson.push(ambient_zone_to_json(z));
    root["ambientZones"] = std::move(ambientJson);

    return Json::stringify(root, 2);
}

bool AudioEditorModel::load_from_json_text(const std::string& document) {
    std::string error;
    const Json::Value root = Json::parse(document, &error);
    if (!root.is_object()) return false;
    const Json::Value* format = root.find("format");
    if (!format || format->as_string() != "VulkanEngine.AudioEditor") return false;

    AudioEditorModel loaded;
    loaded.name = root.find("name") ? root.find("name")->as_string("Untitled Audio Event")
                                    : std::string("Untitled Audio Event");
    loaded.bus = root.find("bus") ? root.find("bus")->as_string("Master") : std::string("Master");
    loaded.volume = static_cast<float>(root.find("volume") ? root.find("volume")->as_number() : 1.0);
    loaded.minPitch = static_cast<float>(root.find("minPitch") ? root.find("minPitch")->as_number() : 1.0);
    loaded.maxPitch = static_cast<float>(root.find("maxPitch") ? root.find("maxPitch")->as_number() : 1.0);
    loaded.minDistance = static_cast<float>(root.find("minDistance") ? root.find("minDistance")->as_number() : 1.0);
    loaded.maxDistance = static_cast<float>(root.find("maxDistance") ? root.find("maxDistance")->as_number() : 100.0);
    loaded.spatial = root.find("spatial") ? root.find("spatial")->as_bool() : true;
    loaded.looping = root.find("looping") ? root.find("looping")->as_bool() : false;

    const Json::Value* variationsJson = root.find("variations");
    if (variationsJson && variationsJson->is_array()) {
        for (const Json::Value& entry : variationsJson->array()) {
            AudioVariationModel v;
            if (variation_from_json(entry, v)) loaded.variations.push_back(v);
        }
    }
    const Json::Value* busesJson = root.find("buses");
    if (busesJson && busesJson->is_array()) {
        for (const Json::Value& entry : busesJson->array()) {
            AudioBusModel bus;
            if (bus_from_json(entry, bus)) loaded.buses.push_back(bus);
        }
    }
    const Json::Value* reverbJson = root.find("reverbZones");
    if (reverbJson && reverbJson->is_array()) {
        for (const Json::Value& entry : reverbJson->array()) {
            AudioReverbZoneModel zone;
            if (reverb_zone_from_json(entry, zone)) loaded.reverbZones.push_back(zone);
        }
    }
    const Json::Value* ambientJson = root.find("ambientZones");
    if (ambientJson && ambientJson->is_array()) {
        for (const Json::Value& entry : ambientJson->array()) {
            AudioAmbientZoneModel zone;
            if (ambient_zone_from_json(entry, zone)) loaded.ambientZones.push_back(zone);
        }
    }

    // Commit: replace this document with the loaded one.
    name = std::move(loaded.name);
    bus = std::move(loaded.bus);
    volume = loaded.volume;
    minPitch = loaded.minPitch;
    maxPitch = loaded.maxPitch;
    minDistance = loaded.minDistance;
    maxDistance = loaded.maxDistance;
    spatial = loaded.spatial;
    looping = loaded.looping;
    variations = std::move(loaded.variations);
    buses = std::move(loaded.buses);
    reverbZones = std::move(loaded.reverbZones);
    ambientZones = std::move(loaded.ambientZones);
    clear_undo();
    mark_saved();
    return true;
}

bool AudioEditorModel::save_to_file(const std::filesystem::path& path) const {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << to_json() << "\n";
    return static_cast<bool>(out);
}

bool AudioEditorModel::load_from_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return load_from_json_text(buffer.str());
}

} // namespace Engine::Editor
