// NetworkInterest.cpp — adapter do contrato INetworkInterest
// (engine::networking). Implementação determinística: observadores e entidades
// em mapas ordenados, relevância por distância euclidiana <= raio (ou
// always_relevant), resultados ordenados, JSON bit-exact all-or-nothing.
// Self-contained (std only) — mesma convenção dos demais adapters do sdk.

#include "engine/networking/INetworkInterest.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <sstream>

namespace engine::networking {

namespace {

std::string json_escape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (const char c : in) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out;
}

bool skip_ws(const std::string& s, std::size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    return i < s.size();
}

bool json_field(const std::string& s, std::size_t& i, std::string& key) {
    if (!skip_ws(s, i) || s[i] != '"') return false;
    ++i;
    key.clear();
    while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char c = s[i + 1];
            key += (c == 'n') ? '\n' : (c == 'r') ? '\r' : (c == 't') ? '\t' : c;
            i += 2;
        } else {
            key += s[i++];
        }
    }
    if (i >= s.size()) return false;
    ++i;
    if (!skip_ws(s, i) || s[i] != ':') return false;
    ++i;
    return true;
}

bool json_string(const std::string& s, std::size_t& i, std::string& out) {
    if (!skip_ws(s, i) || s[i] != '"') return false;
    ++i;
    out.clear();
    while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char c = s[i + 1];
            out += (c == 'n') ? '\n' : (c == 'r') ? '\r' : (c == 't') ? '\t' : c;
            i += 2;
        } else {
            out += s[i++];
        }
    }
    if (i >= s.size()) return false;
    ++i;
    return true;
}

// Lê um número JSON (inteiro ou double com precisão plena). Devemos preservar
// os bits do double para round-trip bit-exact — lemos a string crua e a
// convertemos com strtod, e ao serializar usamos %.17g.
bool json_number(const std::string& s, std::size_t& i, double& out) {
    if (!skip_ws(s, i)) return false;
    std::size_t start = i;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
    bool hasDigit = false;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') { ++i; hasDigit = true; }
    if (i < s.size() && s[i] == '.') {
        ++i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') { ++i; hasDigit = true; }
    }
    if (!hasDigit) return false;
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    }
    out = std::strtod(s.substr(start, i - start).c_str(), nullptr);
    return true;
}

std::string number_json(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return buf;
}

bool json_bool(const std::string& s, std::size_t& i, bool& out) {
    if (!skip_ws(s, i)) return false;
    if (s.compare(i, 4, "true") == 0) { out = true; i += 4; return true; }
    if (s.compare(i, 5, "false") == 0) { out = false; i += 5; return true; }
    return false;
}

bool json_uint64(const std::string& s, std::size_t& i, std::uint64_t& out) {
    if (!skip_ws(s, i)) return false;
    std::size_t start = i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    if (i == start) return false;
    out = std::stoull(s.substr(start, i - start));
    return true;
}

std::string position_json(const NetworkPosition& p) {
    return "{\"x\":" + number_json(p.x) + ",\"y\":" + number_json(p.y) +
           ",\"z\":" + number_json(p.z) + "}";
}

bool parse_position(const std::string& s, std::size_t& i, NetworkPosition& out) {
    if (!skip_ws(s, i) || s[i] != '{') return false;
    ++i;
    bool haveX = false, haveY = false, haveZ = false;
    while (true) {
        std::string key;
        if (!json_field(s, i, key)) return false;
        double v = 0.0;
        if (!json_number(s, i, v)) return false;
        if (key == "x") { out.x = v; haveX = true; }
        else if (key == "y") { out.y = v; haveY = true; }
        else if (key == "z") { out.z = v; haveZ = true; }
        else return false;
        if (!skip_ws(s, i)) return false;
        if (s[i] == '}') { ++i; break; }
        if (s[i] != ',') return false;
        ++i;
    }
    return haveX && haveY && haveZ;
}

double distance(const NetworkPosition& a, const NetworkPosition& b) {
    const double dx = a.x - b.x;
    const double dy = a.y - b.y;
    const double dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

}  // namespace

namespace {

class NetworkInterestImpl final : public INetworkInterest {
public:
    NetworkInterestImpl(std::string sessionId, std::string& errorOut) : session_(std::move(sessionId)) {
        if (session_.empty()) {
            errorOut = "session id must be non-empty";
            valid_ = false;
        }
    }

    bool valid() const { return valid_; }

    const std::string& session_id() const override { return session_; }

    bool set_observer(const InterestObserver& observer, std::string& errorOut) override {
        if (!valid_) { errorOut = "invalid session"; return false; }
        if (observer.radius < 0.0) { errorOut = "radius must be >= 0"; return false; }
        observers_[observer.observer_id] = observer;
        return true;
    }

    void remove_observer(std::uint64_t observer_id) override {
        observers_.erase(observer_id);
    }

    void set_entity(const InterestEntity& entity) override {
        entities_[entity.entity_id] = entity;
    }

    void remove_entity(std::uint64_t entity_id) override {
        entities_.erase(entity_id);
    }

    std::vector<InterestResult> compute() const override {
        std::vector<InterestResult> results;
        results.reserve(observers_.size());
        for (const auto& okv : observers_) {
            InterestResult r;
            r.observer_id = okv.first;
            for (const auto& ekv : entities_) {
                if (okv.second.always_relevant ||
                    distance(okv.second.position, ekv.second.position) <= okv.second.radius) {
                    r.entity_ids.push_back(ekv.first);
                }
            }
            results.push_back(std::move(r));
        }
        return results;
    }

    std::vector<InterestObserver> observers() const override {
        std::vector<InterestObserver> out;
        out.reserve(observers_.size());
        for (const auto& kv : observers_) out.push_back(kv.second);
        return out;
    }

    std::vector<InterestEntity> entities() const override {
        std::vector<InterestEntity> out;
        out.reserve(entities_.size());
        for (const auto& kv : entities_) out.push_back(kv.second);
        return out;
    }

    bool reset(std::string& errorOut) override {
        if (!valid_) { errorOut = "invalid session"; return false; }
        observers_.clear();
        entities_.clear();
        return true;
    }

    bool load_from_json(const std::string& json, std::string& errorOut) override {
        if (!valid_) { errorOut = "invalid session"; return false; }
        std::map<std::uint64_t, InterestObserver> newObservers;
        std::map<std::uint64_t, InterestEntity> newEntities;
        if (!parse_state(json, newObservers, newEntities, errorOut)) return false;
        observers_ = std::move(newObservers);
        entities_ = std::move(newEntities);
        return true;
    }

    std::string serialize_state() const override {
        std::string out = "{\"version\":1,\"session\":\"" + json_escape(session_) +
                          "\",\"observers\":[";
        bool firstObs = true;
        for (const auto& kv : observers_) {
            if (!firstObs) out += ",";
            firstObs = false;
            const auto& o = kv.second;
            out += "{\"observer_id\":" + std::to_string(o.observer_id) +
                   ",\"position\":" + position_json(o.position) +
                   ",\"radius\":" + number_json(o.radius) +
                   ",\"always_relevant\":" + (o.always_relevant ? "true" : "false") + "}";
        }
        out += "],\"entities\":[";
        bool firstEnt = true;
        for (const auto& kv : entities_) {
            if (!firstEnt) out += ",";
            firstEnt = false;
            const auto& e = kv.second;
            out += "{\"entity_id\":" + std::to_string(e.entity_id) +
                   ",\"position\":" + position_json(e.position) + "}";
        }
        out += "]}";
        return out;
    }

private:
    bool parse_state(const std::string& json,
                     std::map<std::uint64_t, InterestObserver>& obsOut,
                     std::map<std::uint64_t, InterestEntity>& entOut,
                     std::string& errorOut) const {
        std::size_t i = 0;
        if (!skip_ws(json, i) || json[i] != '{') { errorOut = "expected object"; return false; }
        ++i;
        int version = 0;
        std::string session;
        bool haveVersion = false, haveSession = false, haveObs = false, haveEnt = false;
        while (true) {
            std::string key;
            if (!json_field(json, i, key)) { errorOut = "bad field"; return false; }
            if (key == "version") {
                if (!json_uint64(json, i, *reinterpret_cast<std::uint64_t*>(&version))) { errorOut = "bad version"; return false; }
                haveVersion = true;
            } else if (key == "session") {
                if (!json_string(json, i, session)) { errorOut = "bad session"; return false; }
                haveSession = true;
            } else if (key == "observers") {
                if (!skip_ws(json, i) || json[i] != '[') { errorOut = "bad observers"; return false; }
                ++i;
                if (skip_ws(json, i) && json[i] == ']') { ++i; haveObs = true; }
                else {
                    while (true) {
                        if (!skip_ws(json, i) || json[i] != '{') { errorOut = "bad observer"; return false; }
                        ++i;
                        InterestObserver o;
                        bool haveId = false, havePos = false, haveRadius = false, haveAlways = false;
                        while (true) {
                            std::string ok;
                            if (!json_field(json, i, ok)) { errorOut = "bad observer field"; return false; }
                            if (ok == "observer_id") {
                                if (!json_uint64(json, i, o.observer_id)) { errorOut = "bad observer_id"; return false; }
                                haveId = true;
                            } else if (ok == "position") {
                                if (!parse_position(json, i, o.position)) { errorOut = "bad position"; return false; }
                                havePos = true;
                            } else if (ok == "radius") {
                                if (!json_number(json, i, o.radius)) { errorOut = "bad radius"; return false; }
                                haveRadius = true;
                            } else if (ok == "always_relevant") {
                                if (!json_bool(json, i, o.always_relevant)) { errorOut = "bad always_relevant"; return false; }
                                haveAlways = true;
                            } else { errorOut = "unknown observer field: " + ok; return false; }
                            if (!skip_ws(json, i)) { errorOut = "bad observer end"; return false; }
                            if (json[i] == '}') { ++i; break; }
                            if (json[i] != ',') { errorOut = "bad observer separator"; return false; }
                            ++i;
                        }
                        if (!haveId || !havePos || !haveRadius || !haveAlways) { errorOut = "incomplete observer"; return false; }
                        if (o.radius < 0.0) { errorOut = "radius must be >= 0"; return false; }
                        if (obsOut.count(o.observer_id)) { errorOut = "duplicate observer_id"; return false; }
                        obsOut.emplace(o.observer_id, o);
                        if (!skip_ws(json, i)) { errorOut = "bad observers end"; return false; }
                        if (json[i] == ']') { ++i; haveObs = true; break; }
                        if (json[i] != ',') { errorOut = "bad observers separator"; return false; }
                        ++i;
                    }
                }
            } else if (key == "entities") {
                if (!skip_ws(json, i) || json[i] != '[') { errorOut = "bad entities"; return false; }
                ++i;
                if (skip_ws(json, i) && json[i] == ']') { ++i; haveEnt = true; }
                else {
                    while (true) {
                        if (!skip_ws(json, i) || json[i] != '{') { errorOut = "bad entity"; return false; }
                        ++i;
                        InterestEntity e;
                        bool haveId = false, havePos = false;
                        while (true) {
                            std::string ek;
                            if (!json_field(json, i, ek)) { errorOut = "bad entity field"; return false; }
                            if (ek == "entity_id") {
                                if (!json_uint64(json, i, e.entity_id)) { errorOut = "bad entity_id"; return false; }
                                haveId = true;
                            } else if (ek == "position") {
                                if (!parse_position(json, i, e.position)) { errorOut = "bad position"; return false; }
                                havePos = true;
                            } else { errorOut = "unknown entity field: " + ek; return false; }
                            if (!skip_ws(json, i)) { errorOut = "bad entity end"; return false; }
                            if (json[i] == '}') { ++i; break; }
                            if (json[i] != ',') { errorOut = "bad entity separator"; return false; }
                            ++i;
                        }
                        if (!haveId || !havePos) { errorOut = "incomplete entity"; return false; }
                        if (entOut.count(e.entity_id)) { errorOut = "duplicate entity_id"; return false; }
                        entOut.emplace(e.entity_id, e);
                        if (!skip_ws(json, i)) { errorOut = "bad entities end"; return false; }
                        if (json[i] == ']') { ++i; haveEnt = true; break; }
                        if (json[i] != ',') { errorOut = "bad entities separator"; return false; }
                        ++i;
                    }
                }
            } else { errorOut = "unknown field: " + key; return false; }
            if (!skip_ws(json, i)) { errorOut = "bad object end"; return false; }
            if (json[i] == '}') { ++i; break; }
            if (json[i] != ',') { errorOut = "bad separator"; return false; }
            ++i;
        }
        if (!haveVersion || version != 1) { errorOut = "unsupported version"; return false; }
        if (!haveSession || session != session_) { errorOut = "session mismatch"; return false; }
        if (!haveObs || !haveEnt) { errorOut = "missing observers/entities"; return false; }
        return true;
    }

    std::string session_;
    bool valid_{ true };
    std::map<std::uint64_t, InterestObserver> observers_;
    std::map<std::uint64_t, InterestEntity> entities_;
};

}  // namespace

std::unique_ptr<INetworkInterest> create_network_interest(const std::string& sessionId,
                                                          std::string& errorOut) {
    auto impl = std::make_unique<NetworkInterestImpl>(sessionId, errorOut);
    if (!impl->valid()) return nullptr;
    return impl;
}

}  // namespace engine::networking
