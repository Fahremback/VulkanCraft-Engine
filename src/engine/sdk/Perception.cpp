#include "engine/ai/IPerception.hpp"

#include "RegistryJson.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>

namespace engine {
namespace ai {
namespace {

constexpr float kPi = 3.14159265358979323846f;

float deg_to_rad(float deg) {
    return deg * (kPi / 180.0f);
}

bool finite(float v) {
    return std::isfinite(v);
}

// Ordenação determinística de detecções: distância asc, depois id asc.
bool detection_less(const Detection& a, const Detection& b) {
    if (a.distance != b.distance) {
        return a.distance < b.distance;
    }
    return a.id < b.id;
}

std::string json_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char ch : text) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += ch; break;
        }
    }
    return out;
}

// Lê um campo numérico opcional; ausente mantém o default, presente exige
// Number (all-or-nothing: tipo errado recusa).
bool read_float_field(const sdk::JsonValue& doc, const char* key, float& out,
                      std::string& errorOut) {
    const sdk::JsonValue* f = doc.field(key);
    if (f == nullptr) {
        return true;
    }
    if (f->kind != sdk::JsonValue::Kind::Number) {
        errorOut = std::string(key) + " must be a number";
        return false;
    }
    out = static_cast<float>(f->number);
    return true;
}

// Uma entrada de memória: estímulo visto há `age` segundos (0 = agora).
struct MemoryEntry {
    uint32_t id = 0;
    float age = 0.0f;
    std::string kind;
};

}  // namespace

bool PerceptionSpec::validate(std::string& errorOut) const {
    if (!finite(vision_range) || vision_range < 0.0f) {
        errorOut = "vision_range must be finite and >= 0";
        return false;
    }
    if (!finite(vision_half_angle_deg) || vision_half_angle_deg <= 0.0f ||
        vision_half_angle_deg > 90.0f) {
        errorOut = "vision_half_angle_deg must be in (0, 90]";
        return false;
    }
    if (!finite(hearing_range) || hearing_range < 0.0f) {
        errorOut = "hearing_range must be finite and >= 0";
        return false;
    }
    if (!finite(proximity_range) || proximity_range < 0.0f) {
        errorOut = "proximity_range must be finite and >= 0";
        return false;
    }
    if (!finite(memory_ttl) || memory_ttl <= 0.0f) {
        errorOut = "memory_ttl must be finite and > 0";
        return false;
    }
    if (!finite(max_range) || max_range <= 0.0f) {
        errorOut = "max_range must be finite and > 0";
        return false;
    }
    errorOut.clear();
    return true;
}

bool PerceptionSpec::load_from_json(const std::string& json,
                                    std::string& errorOut) {
    sdk::JsonValue doc;
    if (!sdk::json_parse(json, doc, errorOut)) {
        return false;
    }
    if (!doc.is_object()) {
        errorOut = "perception spec must be an object";
        return false;
    }
    PerceptionSpec candidate;
    if (!read_float_field(doc, "vision_range", candidate.vision_range, errorOut)) {
        return false;
    }
    if (!read_float_field(doc, "vision_half_angle_deg",
                          candidate.vision_half_angle_deg, errorOut)) {
        return false;
    }
    if (!read_float_field(doc, "hearing_range", candidate.hearing_range, errorOut)) {
        return false;
    }
    if (!read_float_field(doc, "proximity_range", candidate.proximity_range,
                          errorOut)) {
        return false;
    }
    if (!read_float_field(doc, "memory_ttl", candidate.memory_ttl, errorOut)) {
        return false;
    }
    if (!read_float_field(doc, "max_range", candidate.max_range, errorOut)) {
        return false;
    }
    if (!candidate.validate(errorOut)) {
        return false;
    }
    *this = candidate;
    return true;
}

std::string PerceptionSpec::to_json() const {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "{\"vision_range\":" << vision_range
        << ",\"vision_half_angle_deg\":" << vision_half_angle_deg
        << ",\"hearing_range\":" << hearing_range
        << ",\"proximity_range\":" << proximity_range
        << ",\"memory_ttl\":" << memory_ttl
        << ",\"max_range\":" << max_range << "}";
    return out.str();
}

namespace {

class Perception final : public IPerception {
public:
    bool configure(const PerceptionSpec& spec, std::string& errorOut) override {
        if (!spec.validate(errorOut)) {
            return false;
        }
        spec_ = spec;
        return true;
    }

    bool update(const Vec3& agent_pos, const Vec3& agent_forward,
                const std::vector<PerceptionStimulus>& stimuli,
                float dt, std::string& errorOut) override {
        if (!finite(dt) || dt < 0.0f) {
            errorOut = "dt must be finite and >= 0";
            return false;
        }
        const Vec3 forward = agent_forward.normalized();
        const float cos_half = std::cos(deg_to_rad(spec_.vision_half_angle_deg));

        detections_.clear();
        std::vector<uint32_t> seen;

        for (const auto& s : stimuli) {
            const Vec3 rel = s.position - agent_pos;
            const float dist = rel.length();
            if (dist > spec_.max_range) {
                continue;  // teto global
            }
            Detection d;
            d.id = s.id;
            d.position = s.position;
            d.hostile = s.hostile;
            d.kind = s.kind;
            d.faction = s.faction;   // A2-114: faction chega aos sensores
            d.damage = s.damage;     // A2-114: dano/threat chega aos sensores
            d.distance = dist;

            if (dist <= spec_.proximity_range) {
                d.via_proximity = true;
            }
            if (dist <= spec_.hearing_range * std::max(0.0f, s.loudness)) {
                d.via_hearing = true;
            }
            if (spec_.vision_range > 0.0f && dist <= spec_.vision_range &&
                dist > 0.0f) {
                const float dot = rel.normalized().dot(forward);
                if (dot >= cos_half) {
                    d.via_vision = true;
                }
            }

            if (d.via_vision || d.via_hearing || d.via_proximity) {
                detections_.push_back(d);
                seen.push_back(s.id);
            }
        }

        std::sort(detections_.begin(), detections_.end(), detection_less);
        std::sort(seen.begin(), seen.end());

        // Avança a memória: age += dt; rejuvenesce os vistos; esquece vencidos.
        for (auto& e : memory_) {
            e.age += dt;
        }
        for (const auto id : seen) {
            auto it = std::find_if(memory_.begin(), memory_.end(),
                                   [id](const MemoryEntry& e) { return e.id == id; });
            if (it != memory_.end()) {
                it->age = 0.0f;
            } else {
                memory_.push_back(MemoryEntry{id, 0.0f, ""});
            }
        }
        memory_.erase(std::remove_if(memory_.begin(), memory_.end(),
                                     [this](const MemoryEntry& e) {
                                         return e.age > spec_.memory_ttl;
                                     }),
                      memory_.end());

        errorOut.clear();
        return true;
    }

    std::vector<Detection> detections() const override { return detections_; }

    bool nearest_threat(Detection& out) const override {
        const Detection* best = nullptr;
        for (const auto& d : detections_) {
            if (!d.hostile) {
                continue;
            }
            if (best == nullptr || d.distance < best->distance ||
                (d.distance == best->distance && d.id < best->id)) {
                best = &d;
            }
        }
        if (best == nullptr) {
            return false;
        }
        out = *best;
        return true;
    }

    std::vector<uint32_t> remembered_ids() const override {
        std::vector<uint32_t> ids;
        ids.reserve(memory_.size());
        for (const auto& e : memory_) {
            ids.push_back(e.id);
        }
        std::sort(ids.begin(), ids.end());
        return ids;
    }

    std::string serialize_state() const override {
        std::ostringstream out;
        out << std::setprecision(9);
        out << "{\"memory\":[";
        for (std::size_t i = 0; i < memory_.size(); ++i) {
            if (i) out << ",";
            out << "{\"id\":" << memory_[i].id << ",\"age\":" << memory_[i].age
                << "}";
        }
        out << "]}";
        return out.str();
    }

    bool deserialize_state(const std::string& json, std::string& errorOut) override {
        sdk::JsonValue doc;
        if (!sdk::json_parse(json, doc, errorOut)) {
            return false;
        }
        if (!doc.is_object()) {
            errorOut = "perception state must be an object";
            return false;
        }
        const sdk::JsonValue* mem = doc.field("memory");
        if (mem == nullptr || !mem->is_array()) {
            errorOut = "state must contain a memory array";
            return false;
        }
        std::vector<MemoryEntry> candidate;
        for (const auto& item : mem->array) {
            if (!item.is_object()) {
                errorOut = "memory entries must be objects";
                return false;
            }
            const sdk::JsonValue* idField = item.field("id");
            const sdk::JsonValue* ageField = item.field("age");
            if (idField == nullptr || idField->kind != sdk::JsonValue::Kind::Number ||
                ageField == nullptr || ageField->kind != sdk::JsonValue::Kind::Number) {
                errorOut = "memory entry needs numeric id/age";
                return false;
            }
            const double idv = idField->number;
            const double agev = ageField->number;
            if (idv < 0.0 || idv != std::floor(idv) || idv > 4294967295.0) {
                errorOut = "memory id must be a non-negative integer";
                return false;
            }
            if (!std::isfinite(agev) || agev < 0.0) {
                errorOut = "memory age must be finite and >= 0";
                return false;
            }
            candidate.push_back(MemoryEntry{static_cast<uint32_t>(idv),
                                            static_cast<float>(agev), ""});
        }
        memory_ = std::move(candidate);
        errorOut.clear();
        return true;
    }

private:
    PerceptionSpec spec_;
    std::vector<Detection> detections_;
    std::vector<MemoryEntry> memory_;
};

}  // namespace

std::unique_ptr<IPerception> create_perception() {
    return std::make_unique<Perception>();
}

}  // namespace ai
}  // namespace engine
