#include "engine/audio/ISpatialAudio.hpp"

#include "RegistryJson.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>

namespace engine {
namespace audio {
namespace {

bool finite(double v) {
    return std::isfinite(v);
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

bool is_uint64(const sdk::JsonValue& v) {
    return v.kind == sdk::JsonValue::Kind::Number && v.number >= 0.0 &&
           v.number == std::floor(v.number);
}

bool number_field(const sdk::JsonValue& obj, const char* key, double& out,
                  bool required, std::string& errorOut) {
    const sdk::JsonValue* f = obj.field(key);
    if (f == nullptr) {
        if (required) {
            errorOut = std::string("missing field ") + key;
            return false;
        }
        return true;
    }
    if (f->kind != sdk::JsonValue::Kind::Number) {
        errorOut = std::string(key) + " must be a number";
        return false;
    }
    out = f->number;
    return true;
}

bool string_field(const sdk::JsonValue& obj, const char* key, std::string& out,
                  bool required, std::string& errorOut) {
    const sdk::JsonValue* f = obj.field(key);
    if (f == nullptr) {
        if (required) {
            errorOut = std::string("missing field ") + key;
            return false;
        }
        return true;
    }
    if (f->kind != sdk::JsonValue::Kind::String) {
        errorOut = std::string(key) + " must be a string";
        return false;
    }
    out = f->string;
    return true;
}

bool bool_field(const sdk::JsonValue& obj, const char* key, bool& out,
                bool required, std::string& errorOut) {
    const sdk::JsonValue* f = obj.field(key);
    if (f == nullptr) {
        if (required) {
            errorOut = std::string("missing field ") + key;
            return false;
        }
        return true;
    }
    if (f->kind != sdk::JsonValue::Kind::Bool) {
        errorOut = std::string(key) + " must be a bool";
        return false;
    }
    out = f->boolean;
    return true;
}

bool vec3_field(const sdk::JsonValue& obj, const char* key, Vec3& out,
                bool required, std::string& errorOut) {
    const sdk::JsonValue* f = obj.field(key);
    if (f == nullptr) {
        if (required) {
            errorOut = std::string("missing field ") + key;
            return false;
        }
        return true;
    }
    if (!f->is_array() || f->array.size() != 3) {
        errorOut = std::string(key) + " must be a [x,y,z] array";
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        if (f->array[i].kind != sdk::JsonValue::Kind::Number) {
            errorOut = std::string(key) + " entries must be numbers";
            return false;
        }
    }
    out = {f->array[0].number, f->array[1].number, f->array[2].number};
    return true;
}

double db_to_linear_local(double db) {
    return std::pow(10.0, db / 20.0);
}

double clamp01(double v) {
    return std::max(0.0, std::min(1.0, v));
}

const double kPi = 3.14159265358979323846;

}  // namespace

bool AudioSpatialSpec::validate(std::string& errorOut) const {
    if (!finite(min_distance) || min_distance <= 0.0) {
        errorOut = "min_distance must be finite and > 0";
        return false;
    }
    if (!finite(max_distance) || max_distance <= min_distance) {
        errorOut = "max_distance must be finite and > min_distance";
        return false;
    }
    if (!finite(master_gain_db)) {
        errorOut = "master_gain_db must be finite";
        return false;
    }
    std::set<std::string> ids;
    for (const auto& z : zones) {
        if (z.id.empty()) {
            errorOut = "zone id must be non-empty";
            return false;
        }
        if (ids.count(z.id)) {
            errorOut = "duplicate zone id \"" + z.id + "\"";
            return false;
        }
        ids.insert(z.id);
        if (!finite(z.center.x) || !finite(z.center.y) || !finite(z.center.z) ||
            !finite(z.half_extents.x) || !finite(z.half_extents.y) ||
            !finite(z.half_extents.z) || z.half_extents.x <= 0.0 ||
            z.half_extents.y <= 0.0 || z.half_extents.z <= 0.0) {
            errorOut = "zone half-extents must be finite and > 0";
            return false;
        }
        if (!finite(z.wet) || z.wet < 0.0 || z.wet > 1.0) {
            errorOut = "zone wet must be finite and in [0,1]";
            return false;
        }
    }
    errorOut.clear();
    return true;
}

bool AudioSpatialSpec::load_from_json(const std::string& json,
                                      std::string& errorOut) {
    sdk::JsonValue doc;
    if (!sdk::json_parse(json, doc, errorOut)) {
        return false;
    }
    if (!doc.is_object()) {
        errorOut = "audio spatial spec must be an object";
        return false;
    }
    const sdk::JsonValue* version = doc.field("version");
    if (version != nullptr &&
        (!is_uint64(*version) || static_cast<int>(version->number) != 1)) {
        errorOut = "unsupported audio spatial spec version";
        return false;
    }
    AudioSpatialSpec candidate;
    if (!number_field(doc, "min_distance", candidate.min_distance, false, errorOut))
        return false;
    if (!number_field(doc, "max_distance", candidate.max_distance, false, errorOut))
        return false;
    const sdk::JsonValue* rolloffField = doc.field("rolloff");
    if (rolloffField != nullptr) {
        if (rolloffField->kind != sdk::JsonValue::Kind::String) {
            errorOut = "rolloff must be a string";
            return false;
        }
        if (rolloffField->string == "linear") {
            candidate.rolloff = RolloffModel::Linear;
        } else if (rolloffField->string == "inverse") {
            candidate.rolloff = RolloffModel::Inverse;
        } else if (rolloffField->string == "inverse_square") {
            candidate.rolloff = RolloffModel::InverseSquare;
        } else {
            errorOut = "unknown rolloff \"" + rolloffField->string + "\"";
            return false;
        }
    }
    if (!number_field(doc, "master_gain_db", candidate.master_gain_db, false,
                      errorOut))
        return false;
    const sdk::JsonValue* zonesField = doc.field("zones");
    if (zonesField != nullptr) {
        if (!zonesField->is_array()) {
            errorOut = "zones must be an array";
            return false;
        }
        for (const auto& item : zonesField->array) {
            if (!item.is_object()) {
                errorOut = "zone entries must be objects";
                return false;
            }
            AudioReverbZone z;
            if (!string_field(item, "id", z.id, true, errorOut)) return false;
            if (!vec3_field(item, "center", z.center, true, errorOut)) return false;
            if (!vec3_field(item, "half_extents", z.half_extents, true, errorOut))
                return false;
            if (!number_field(item, "wet", z.wet, false, errorOut)) return false;
            if (!number_field(item, "decay", z.decay, false, errorOut)) return false;
            candidate.zones.push_back(z);
        }
    }
    if (!candidate.validate(errorOut)) {
        return false;
    }
    *this = std::move(candidate);
    return true;
}

std::string AudioSpatialSpec::to_json() const {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "{\"version\":1,\"min_distance\":" << min_distance
        << ",\"max_distance\":" << max_distance << ",\"rolloff\":\"";
    switch (rolloff) {
        case RolloffModel::Linear: out << "linear"; break;
        case RolloffModel::Inverse: out << "inverse"; break;
        case RolloffModel::InverseSquare: out << "inverse_square"; break;
    }
    out << "\",\"master_gain_db\":" << master_gain_db << ",\"zones\":[";
    for (std::size_t i = 0; i < zones.size(); ++i) {
        if (i) out << ",";
        const auto& z = zones[i];
        out << "{\"id\":\"" << json_escape(z.id) << "\",\"center\":[" << z.center.x
            << "," << z.center.y << "," << z.center.z << "],\"half_extents\":["
            << z.half_extents.x << "," << z.half_extents.y << "," << z.half_extents.z
            << "],\"wet\":" << z.wet << ",\"decay\":" << z.decay << "}";
    }
    out << "]}";
    return out.str();
}

namespace {

double attenuate(RolloffModel model, double distance, double minD, double maxD) {
    if (!(distance > minD)) {
        return 1.0;  // dentro do raio de ganho pleno
    }
    if (distance >= maxD) {
        return 0.0;  // além do alcance
    }
    switch (model) {
        case RolloffModel::Linear: {
            const double t = (distance - minD) / (maxD - minD);
            return 1.0 - t;
        }
        case RolloffModel::Inverse:
            return minD / distance;
        case RolloffModel::InverseSquare: {
            const double r = minD / distance;
            return r * r;
        }
    }
    return 1.0;
}

// Panning equal-power a partir do azimute (projeção horizontal).
// pan = θ/π ∈ [-1,1]; L = cos((pan+1)π/4), R = sin((pan+1)π/4), L²+R²=1.
void pan_gains(const Vec3& to_source, const Vec3& fwd_xz, double& panL,
               double& panR) {
    const Vec3 dir_xz = to_source.normalized_xz();
    const Vec3 f = fwd_xz.normalized_xz();
    if (dir_xz.length_sq() <= 0.0 || f.length_sq() <= 0.0) {
        const double c = std::sqrt(0.5);
        panL = c;
        panR = c;
        return;
    }
    const double dot =
        std::max(-1.0, std::min(1.0, dir_xz.x * f.x + dir_xz.z * f.z));
    const double cross = f.x * dir_xz.z - f.z * dir_xz.x;
    const double theta = std::atan2(cross, dot);  // ∈ [-π, π]
    const double pan = theta / kPi;               // ∈ [-1, 1]
    panL = std::cos((pan + 1.0) * kPi / 4.0);
    panR = std::sin((pan + 1.0) * kPi / 4.0);
}

// Cobertura de um ponto numa AABB: 1 no centro → 0 na borda (min dos eixos).
double zone_coverage(const Vec3& point, const AudioReverbZone& zone) {
    double cov = 1.0;
    const double dx = std::fabs(point.x - zone.center.x) / zone.half_extents.x;
    const double dy = std::fabs(point.y - zone.center.y) / zone.half_extents.y;
    const double dz = std::fabs(point.z - zone.center.z) / zone.half_extents.z;
    cov = std::min(cov, 1.0 - clamp01(dx));
    cov = std::min(cov, 1.0 - clamp01(dy));
    cov = std::min(cov, 1.0 - clamp01(dz));
    return cov;
}

bool validate_source_input(const AudioSourceInput& input, std::string& errorOut) {
    if (!finite(input.position.x) || !finite(input.position.y) ||
        !finite(input.position.z)) {
        errorOut = "source position must be finite";
        return false;
    }
    if (!finite(input.gain_db)) {
        errorOut = "source gain_db must be finite";
        return false;
    }
    if (!finite(input.priority) || input.priority < 0.0 || input.priority > 1.0) {
        errorOut = "source priority must be finite and in [0,1]";
        return false;
    }
    if (!finite(input.occlusion) || input.occlusion < 0.0 ||
        input.occlusion > 1.0) {
        errorOut = "source occlusion must be finite and in [0,1]";
        return false;
    }
    errorOut.clear();
    return true;
}

class SpatialAudio final : public ISpatialAudio {
public:
    bool configure(const AudioSpatialSpec& spec, std::string& errorOut) override {
        if (!spec.validate(errorOut)) {
            return false;
        }
        spec_ = spec;
        sources_.clear();
        results_.clear();
        virtualized_.clear();
        errorOut.clear();
        return true;
    }

    bool set_listener(const Vec3& position, const Vec3& forward,
                      std::string& errorOut) override {
        if (!finite(position.x) || !finite(position.y) || !finite(position.z) ||
            !finite(forward.x) || !finite(forward.y) || !finite(forward.z)) {
            errorOut = "listener position/forward must be finite";
            return false;
        }
        if (forward.normalized_xz().length_sq() <= 0.0) {
            errorOut = "listener forward needs a non-zero horizontal projection";
            return false;
        }
        listener_pos_ = position;
        listener_fwd_ = forward;
        errorOut.clear();
        return true;
    }

    bool set_source(const std::string& id, const AudioSourceInput& input,
                    std::string& errorOut) override {
        if (id.empty()) {
            errorOut = "source id must be non-empty";
            return false;
        }
        if (!validate_source_input(input, errorOut)) {
            return false;
        }
        sources_[id] = input;
        results_[id] = AudioSourceResult{};
        errorOut.clear();
        return true;
    }

    bool remove_source(const std::string& id, std::string& errorOut) override {
        if (!sources_.count(id)) {
            errorOut = "unknown source \"" + id + "\"";
            return false;
        }
        sources_.erase(id);
        results_.erase(id);
        errorOut.clear();
        return true;
    }

    bool update(std::string& errorOut) override {
        (void)errorOut;
        // 1) Computa o resultado base de cada fonte (ordem sorted por id —
        //    iteração de std::map é determinística).
        for (auto& kv : sources_) {
            results_[kv.first] = compute(kv.first, kv.second);
        }
        // 2) Virtualização: fontes 3D além do budget, prioridade DESC, id ASC.
        std::vector<std::string> three_d;
        for (const auto& kv : sources_) {
            if (kv.second.is_3d) {
                three_d.push_back(kv.first);
            }
        }
        std::sort(three_d.begin(), three_d.end(),
                  [this](const std::string& a, const std::string& b) {
                      const double pa = sources_.at(a).priority;
                      const double pb = sources_.at(b).priority;
                      if (pa != pb) {
                          return pa > pb;  // prioridade DESC
                      }
                      return a < b;  // id ASC (desempate determinístico)
                  });
        virtualized_.clear();
        int active = 0;
        for (const auto& id : three_d) {
            if (active >= max_voices_) {
                virtualized_.push_back(id);
                results_[id] = AudioSourceResult{0.0, 0.0, 0.0, true};
            } else {
                ++active;
            }
        }
        errorOut.clear();
        return true;
    }

    AudioSourceResult source_result(const std::string& id) const override {
        const auto it = results_.find(id);
        return it == results_.end() ? AudioSourceResult{} : it->second;
    }

    bool source_active(const std::string& id) const override {
        return sources_.count(id) != 0;
    }

    bool set_max_voices(int maxVoices, std::string& errorOut) override {
        if (maxVoices < 1) {
            errorOut = "max_voices must be >= 1";
            return false;
        }
        max_voices_ = maxVoices;
        errorOut.clear();
        return true;
    }

    std::vector<std::string> virtualized_sources() const override {
        return virtualized_;
    }

    std::string serialize_state() const override {
        std::ostringstream out;
        out << std::setprecision(9);
        out << "{\"listener\":{\"px\":" << listener_pos_.x << ",\"py\":"
            << listener_pos_.y << ",\"pz\":" << listener_pos_.z << ",\"fx\":"
            << listener_fwd_.x << ",\"fy\":" << listener_fwd_.y << ",\"fz\":"
            << listener_fwd_.z << "},\"max_voices\":" << max_voices_
            << ",\"sources\":{";
        bool first = true;
        for (const auto& kv : sources_) {
            if (!first) out << ",";
            first = false;
            const auto& s = kv.second;
            out << "\"" << json_escape(kv.first) << "\":{\"px\":" << s.position.x
                << ",\"py\":" << s.position.y << ",\"pz\":" << s.position.z
                << ",\"gain_db\":" << s.gain_db << ",\"priority\":"
                << s.priority << ",\"occlusion\":" << s.occlusion
                << ",\"is_3d\":" << (s.is_3d ? "true" : "false") << "}";
        }
        out << "}}";
        return out.str();
    }

    bool deserialize_state(const std::string& json,
                           std::string& errorOut) override {
        sdk::JsonValue doc;
        if (!sdk::json_parse(json, doc, errorOut)) {
            return false;
        }
        if (!doc.is_object()) {
            errorOut = "audio spatial state must be an object";
            return false;
        }
        const sdk::JsonValue* listenerField = doc.field("listener");
        if (listenerField == nullptr || !listenerField->is_object()) {
            errorOut = "state must contain a listener object";
            return false;
        }
        Vec3 pos, fwd;
        if (!number_field(*listenerField, "px", pos.x, true, errorOut)) return false;
        if (!number_field(*listenerField, "py", pos.y, true, errorOut)) return false;
        if (!number_field(*listenerField, "pz", pos.z, true, errorOut)) return false;
        if (!number_field(*listenerField, "fx", fwd.x, true, errorOut)) return false;
        if (!number_field(*listenerField, "fy", fwd.y, true, errorOut)) return false;
        if (!number_field(*listenerField, "fz", fwd.z, true, errorOut)) return false;
        if (fwd.normalized_xz().length_sq() <= 0.0) {
            errorOut = "state listener forward has zero horizontal projection";
            return false;
        }
        int maxVoices = -1;
        const sdk::JsonValue* voicesField = doc.field("max_voices");
        if (voicesField != nullptr) {
            if (!is_uint64(*voicesField)) {
                errorOut = "max_voices must be a non-negative integer";
                return false;
            }
            maxVoices = static_cast<int>(voicesField->number);
            if (maxVoices < 1) {
                errorOut = "max_voices must be >= 1";
                return false;
            }
        }
        const sdk::JsonValue* sourcesField = doc.field("sources");
        if (sourcesField == nullptr || !sourcesField->is_object()) {
            errorOut = "state must contain a sources object";
            return false;
        }
        // all-or-nothing: valida tudo antes de mutar.
        std::map<std::string, AudioSourceInput> nsources;
        for (const auto& kv : sourcesField->object) {
            if (kv.first.empty()) {
                errorOut = "source id must be non-empty";
                return false;
            }
            if (!kv.second.is_object()) {
                errorOut = "source entries must be objects";
                return false;
            }
            AudioSourceInput s;
            if (!number_field(kv.second, "px", s.position.x, true, errorOut)) return false;
            if (!number_field(kv.second, "py", s.position.y, true, errorOut)) return false;
            if (!number_field(kv.second, "pz", s.position.z, true, errorOut)) return false;
            if (!number_field(kv.second, "gain_db", s.gain_db, false, errorOut)) return false;
            if (!number_field(kv.second, "priority", s.priority, false, errorOut)) return false;
            if (!number_field(kv.second, "occlusion", s.occlusion, false, errorOut)) return false;
            if (!bool_field(kv.second, "is_3d", s.is_3d, false, errorOut)) return false;
            if (!validate_source_input(s, errorOut)) {
                return false;
            }
            nsources[kv.first] = s;
        }
        // commit
        listener_pos_ = pos;
        listener_fwd_ = fwd;
        if (maxVoices >= 1) {
            max_voices_ = maxVoices;
        }
        sources_ = std::move(nsources);
        results_.clear();
        for (const auto& kv : sources_) {
            results_[kv.first] = AudioSourceResult{};
        }
        virtualized_.clear();
        errorOut.clear();
        return true;
    }

private:
    AudioSourceResult compute(const std::string& id,
                              const AudioSourceInput& input) const {
        (void)id;
        if (!input.is_3d) {
            const double total = db_to_linear_local(spec_.master_gain_db +
                                                    input.gain_db);
            const double c = std::sqrt(0.5);
            return AudioSourceResult{total * c, total * c, 0.0, false};
        }
        const Vec3 to_source = input.position - listener_pos_;
        const double dist = to_source.length();
        const double attenu = attenuate(spec_.rolloff, dist, spec_.min_distance,
                                        spec_.max_distance);
        double panL = 0.0, panR = 0.0;
        pan_gains(to_source, listener_fwd_, panL, panR);
        const double occ = 1.0 - input.occlusion;
        const double total = db_to_linear_local(spec_.master_gain_db +
                                                input.gain_db) *
                             attenu * occ;
        double wet = 0.0;
        for (const auto& z : spec_.zones) {
            wet = std::max(wet, z.wet * zone_coverage(input.position, z));
        }
        return AudioSourceResult{total * panL, total * panR, wet, false};
    }

    AudioSpatialSpec spec_;
    Vec3 listener_pos_{0.0, 0.0, 0.0};
    Vec3 listener_fwd_{1.0, 0.0, 0.0};
    int max_voices_ = 8;
    std::map<std::string, AudioSourceInput> sources_;
    std::map<std::string, AudioSourceResult> results_;
    std::vector<std::string> virtualized_;
};

}  // namespace

std::unique_ptr<ISpatialAudio> create_spatial_audio() {
    return std::make_unique<SpatialAudio>();
}

}  // namespace audio
}  // namespace engine
