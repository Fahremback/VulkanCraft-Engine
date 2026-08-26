#include "engine/audio/IAudioMixer.hpp"

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

}  // namespace

double linear_to_db(double linear) {
    if (!(linear > 0.0)) {
        return -std::numeric_limits<double>::infinity();
    }
    return 20.0 * std::log10(linear);
}

double db_to_linear(double db) {
    return std::pow(10.0, db / 20.0);
}

bool AudioMixerSpec::validate(std::string& errorOut) const {
    std::map<std::string, bool> ids;
    for (const auto& b : buses) {
        if (b.id.empty()) {
            errorOut = "bus id must be non-empty";
            return false;
        }
        if (ids.count(b.id)) {
            errorOut = "duplicate bus id \"" + b.id + "\"";
            return false;
        }
        ids[b.id] = true;
        if (!finite(b.gain_db)) {
            errorOut = "bus gain_db must be finite";
            return false;
        }
    }
    for (const auto& b : buses) {
        if (!b.parent.empty() && !ids.count(b.parent)) {
            errorOut = "bus \"" + b.id + "\" references unknown parent \"" + b.parent + "\"";
            return false;
        }
    }
    for (const auto& sc : sidechains) {
        if (!ids.count(sc.source) || !ids.count(sc.target)) {
            errorOut = "sidechain references unknown bus";
            return false;
        }
        if (!finite(sc.threshold) || sc.threshold < 0.0 || sc.threshold > 1.0) {
            errorOut = "threshold must be finite and in [0,1]";
            return false;
        }
        if (!finite(sc.duck_db) || !finite(sc.attack_s) || !finite(sc.release_s) ||
            sc.attack_s < 0.0 || sc.release_s < 0.0) {
            errorOut = "sidechain duck_db/attack/release must be finite (attack/release >= 0)";
            return false;
        }
    }
    std::set<std::string> snap_names;
    for (const auto& s : snapshots) {
        if (s.name.empty()) {
            errorOut = "snapshot name must be non-empty";
            return false;
        }
        if (snap_names.count(s.name)) {
            errorOut = "duplicate snapshot \"" + s.name + "\"";
            return false;
        }
        snap_names.insert(s.name);
        for (const auto& g : s.gains) {
            if (!ids.count(g.bus)) {
                errorOut = "snapshot references unknown bus \"" + g.bus + "\"";
                return false;
            }
            if (!finite(g.gain_db)) {
                errorOut = "snapshot gain_db must be finite";
                return false;
            }
        }
    }
    // Sem ciclos na árvore de roteamento (parent chain).
    for (const auto& b : buses) {
        std::set<std::string> seen;
        std::string cur = b.id;
        while (!cur.empty()) {
            if (!seen.insert(cur).second) {
                errorOut = "routing cycle detected at bus \"" + b.id + "\"";
                return false;
            }
            bool found = false;
            for (const auto& p : buses) {
                if (p.id == cur) {
                    cur = p.parent;
                    found = true;
                    break;
                }
            }
            if (!found) {
                break;
            }
        }
    }
    errorOut.clear();
    return true;
}

bool AudioMixerSpec::load_from_json(const std::string& json,
                                    std::string& errorOut) {
    sdk::JsonValue doc;
    if (!sdk::json_parse(json, doc, errorOut)) {
        return false;
    }
    if (!doc.is_object()) {
        errorOut = "audio mixer spec must be an object";
        return false;
    }
    const sdk::JsonValue* version = doc.field("version");
    if (version != nullptr &&
        (!is_uint64(*version) || static_cast<int>(version->number) != 1)) {
        errorOut = "unsupported audio mixer spec version";
        return false;
    }
    AudioMixerSpec candidate;
    const sdk::JsonValue* busesField = doc.field("buses");
    if (busesField != nullptr) {
        if (!busesField->is_array()) {
            errorOut = "buses must be an array";
            return false;
        }
        for (const auto& item : busesField->array) {
            if (!item.is_object()) {
                errorOut = "bus entries must be objects";
                return false;
            }
            AudioBus b;
            if (!string_field(item, "id", b.id, true, errorOut)) return false;
            if (!number_field(item, "gain_db", b.gain_db, false, errorOut)) return false;
            if (!string_field(item, "parent", b.parent, false, errorOut)) return false;
            candidate.buses.push_back(b);
        }
    }
    const sdk::JsonValue* scField = doc.field("sidechains");
    if (scField != nullptr) {
        if (!scField->is_array()) {
            errorOut = "sidechains must be an array";
            return false;
        }
        for (const auto& item : scField->array) {
            if (!item.is_object()) {
                errorOut = "sidechain entries must be objects";
                return false;
            }
            AudioSidechain sc;
            if (!string_field(item, "source", sc.source, true, errorOut)) return false;
            if (!string_field(item, "target", sc.target, true, errorOut)) return false;
            if (!number_field(item, "threshold", sc.threshold, false, errorOut)) return false;
            if (!number_field(item, "duck_db", sc.duck_db, false, errorOut)) return false;
            if (!number_field(item, "attack_s", sc.attack_s, false, errorOut)) return false;
            if (!number_field(item, "release_s", sc.release_s, false, errorOut)) return false;
            candidate.sidechains.push_back(sc);
        }
    }
    const sdk::JsonValue* snapField = doc.field("snapshots");
    if (snapField != nullptr) {
        if (!snapField->is_array()) {
            errorOut = "snapshots must be an array";
            return false;
        }
        for (const auto& item : snapField->array) {
            if (!item.is_object()) {
                errorOut = "snapshot entries must be objects";
                return false;
            }
            AudioSnapshot s;
            if (!string_field(item, "name", s.name, true, errorOut)) return false;
            const sdk::JsonValue* gains = item.field("gains");
            if (gains != nullptr) {
                if (!gains->is_array()) {
                    errorOut = "snapshot gains must be an array";
                    return false;
                }
                for (const auto& g : gains->array) {
                    if (!g.is_object()) {
                        errorOut = "snapshot gain entries must be objects";
                        return false;
                    }
                    AudioSnapshot::Gain gain;
                    if (!string_field(g, "bus", gain.bus, true, errorOut)) return false;
                    if (!number_field(g, "gain_db", gain.gain_db, false, errorOut)) return false;
                    s.gains.push_back(gain);
                }
            }
            candidate.snapshots.push_back(s);
        }
    }
    if (!candidate.validate(errorOut)) {
        return false;
    }
    *this = std::move(candidate);
    return true;
}

std::string AudioMixerSpec::to_json() const {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "{\"version\":1,\"buses\":[";
    for (std::size_t i = 0; i < buses.size(); ++i) {
        if (i) out << ",";
        out << "{\"id\":\"" << json_escape(buses[i].id) << "\",\"gain_db\":"
            << buses[i].gain_db;
        if (!buses[i].parent.empty()) {
            out << ",\"parent\":\"" << json_escape(buses[i].parent) << "\"";
        }
        out << "}";
    }
    out << "],\"sidechains\":[";
    for (std::size_t i = 0; i < sidechains.size(); ++i) {
        if (i) out << ",";
        out << "{\"source\":\"" << json_escape(sidechains[i].source)
            << "\",\"target\":\"" << json_escape(sidechains[i].target)
            << "\",\"threshold\":" << sidechains[i].threshold
            << ",\"duck_db\":" << sidechains[i].duck_db
            << ",\"attack_s\":" << sidechains[i].attack_s
            << ",\"release_s\":" << sidechains[i].release_s << "}";
    }
    out << "],\"snapshots\":[";
    for (std::size_t i = 0; i < snapshots.size(); ++i) {
        if (i) out << ",";
        out << "{\"name\":\"" << json_escape(snapshots[i].name) << "\",\"gains\":[";
        for (std::size_t j = 0; j < snapshots[i].gains.size(); ++j) {
            if (j) out << ",";
            out << "{\"bus\":\"" << json_escape(snapshots[i].gains[j].bus)
                << "\",\"gain_db\":" << snapshots[i].gains[j].gain_db << "}";
        }
        out << "]}";
    }
    out << "]}";
    return out.str();
}

namespace {

struct BusRuntime {
    double base_gain_db = 0.0;
    double duck_db = 0.0;  // envelope de sidechain (negativo = atenua)
};

class AudioMixer final : public IAudioMixer {
public:
    bool configure(const AudioMixerSpec& spec, std::string& errorOut) override {
        if (!spec.validate(errorOut)) {
            return false;
        }
        spec_ = spec;
        inputs_.clear();
        duck_.clear();
        buses_.clear();
        for (const auto& b : spec_.buses) {
            buses_[b.id] = BusRuntime{b.gain_db, 0.0};
            inputs_[b.id] = 0.0;
            duck_[b.id] = 0.0;
        }
        return true;
    }

    bool set_input(const std::string& bus, double level, std::string& errorOut) override {
        if (!buses_.count(bus)) {
            errorOut = "set_input references unknown bus";
            return false;
        }
        inputs_[bus] = std::max(0.0, std::min(1.0, level));  // clamp [0,1]
        errorOut.clear();
        return true;
    }

    bool tick(double dt, std::string& errorOut) override {
        if (!finite(dt) || dt < 0.0) {
            errorOut = "dt must be finite and >= 0";
            return false;
        }
        // Para cada sidechain, atualiza o envelope do alvo em direção ao duck
        // (se o source está acima do threshold) ou de volta a 0.
        for (const auto& sc : spec_.sidechains) {
            const double source_level = pre_gain_level(sc.source);
            const bool ducking = source_level > sc.threshold;
            const double target_db = ducking ? sc.duck_db : 0.0;
            const double rate = ducking ? sc.attack_s : sc.release_s;
            double& current = duck_[sc.target];
            if (rate <= 0.0) {
                current = target_db;  // rampa instantânea
            } else {
                // Rampa linear determinística: completa |duck_db| ao longo de
                // `rate` segundos (ataque = duck, release = volta a 0).
                const double max_delta = (std::fabs(sc.duck_db) / rate) * dt;
                if (current < target_db) {
                    current = std::min(current + max_delta, target_db);
                } else if (current > target_db) {
                    current = std::max(current - max_delta, target_db);
                }
            }
        }
        errorOut.clear();
        return true;
    }

    double gain_db(const std::string& bus) const override {
        const auto it = buses_.find(bus);
        if (it == buses_.end()) {
            return 0.0;
        }
        return it->second.base_gain_db + duck_value(bus);
    }

    double bus_level(const std::string& bus) const override {
        const auto it = buses_.find(bus);
        if (it == buses_.end()) {
            return 0.0;
        }
        return pre_gain_level(bus) * db_to_linear(gain_db(bus));
    }

    double master_level() const override {
        // Master = soma do nível pós-gain de todos os buses-raiz.
        double total = 0.0;
        for (const auto& b : spec_.buses) {
            if (b.parent.empty()) {
                total += bus_level(b.id);
            }
        }
        return std::max(0.0, std::min(1.0, total));
    }

    bool apply_snapshot(const std::string& name, std::string& errorOut) override {
        const AudioSnapshot* snap = nullptr;
        for (const auto& s : spec_.snapshots) {
            if (s.name == name) {
                snap = &s;
                break;
            }
        }
        if (snap == nullptr) {
            errorOut = "unknown snapshot \"" + name + "\"";
            return false;
        }
        // all-or-nothing: valida os buses antes de mutar.
        for (const auto& g : snap->gains) {
            if (!buses_.count(g.bus)) {
                errorOut = "snapshot references unknown bus";
                return false;
            }
        }
        for (const auto& g : snap->gains) {
            buses_[g.bus].base_gain_db = g.gain_db;
        }
        errorOut.clear();
        return true;
    }

    std::string serialize_state() const override {
        std::ostringstream out;
        out << std::setprecision(9);
        out << "{\"duck\":{";
        bool first = true;
        for (const auto& kv : duck_) {
            if (!first) out << ",";
            first = false;
            out << "\"" << json_escape(kv.first) << "\":" << kv.second;
        }
        out << "},\"gains\":{";
        first = true;
        for (const auto& kv : buses_) {
            if (!first) out << ",";
            first = false;
            out << "\"" << json_escape(kv.first) << "\":" << kv.second.base_gain_db;
        }
        out << "}}";
        return out.str();
    }

    bool deserialize_state(const std::string& json, std::string& errorOut) override {
        sdk::JsonValue doc;
        if (!sdk::json_parse(json, doc, errorOut)) {
            return false;
        }
        if (!doc.is_object()) {
            errorOut = "audio mixer state must be an object";
            return false;
        }
        const sdk::JsonValue* duckField = doc.field("duck");
        if (duckField == nullptr || !duckField->is_object()) {
            errorOut = "state must contain a duck object";
            return false;
        }
        const sdk::JsonValue* gainsField = doc.field("gains");
        if (gainsField == nullptr || !gainsField->is_object()) {
            errorOut = "state must contain a gains object";
            return false;
        }
        // all-or-nothing: parse em temporário.
        std::map<std::string, double> nduck, ngains;
        for (const auto& kv : duckField->object) {
            if (kv.second.kind != sdk::JsonValue::Kind::Number ||
                !finite(kv.second.number)) {
                errorOut = "duck values must be finite numbers";
                return false;
            }
            if (!buses_.count(kv.first)) {
                errorOut = "state references unknown bus \"" + kv.first + "\"";
                return false;
            }
            nduck[kv.first] = kv.second.number;
        }
        for (const auto& kv : gainsField->object) {
            if (kv.second.kind != sdk::JsonValue::Kind::Number ||
                !finite(kv.second.number)) {
                errorOut = "gain values must be finite numbers";
                return false;
            }
            if (!buses_.count(kv.first)) {
                errorOut = "state references unknown bus \"" + kv.first + "\"";
                return false;
            }
            ngains[kv.first] = kv.second.number;
        }
        for (const auto& kv : nduck) duck_[kv.first] = kv.second;
        for (const auto& kv : ngains) buses_[kv.first].base_gain_db = kv.second;
        errorOut.clear();
        return true;
    }

private:
    double duck_value(const std::string& bus) const {
        const auto it = duck_.find(bus);
        return it == duck_.end() ? 0.0 : it->second;
    }

    // Nível PRÉ-gain = input + Σ(pós-gain dos filhos).
    double pre_gain_level(const std::string& bus) const {
        double level = inputs_.count(bus) ? inputs_.at(bus) : 0.0;
        for (const auto& b : spec_.buses) {
            if (b.parent == bus) {
                level += bus_level(b.id);
            }
        }
        return std::max(0.0, std::min(1.0, level));
    }

    AudioMixerSpec spec_;
    std::map<std::string, double> inputs_;
    std::map<std::string, double> duck_;
    std::map<std::string, BusRuntime> buses_;
};

}  // namespace

std::unique_ptr<IAudioMixer> create_audio_mixer() {
    return std::make_unique<AudioMixer>();
}

}  // namespace audio
}  // namespace engine
