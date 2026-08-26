// NetworkDiscovery.cpp — adapter do contrato INetworkDiscovery
// (engine::networking). Implementação determinística: serviços em mapa
// ordenado, saúde derivada de falhas consecutivas, resolve por tipo retorna
// só os saudáveis em ordem crescente de id, JSON bit-exact all-or-nothing.
// Self-contained (std only) — mesma convenção dos demais adapters do sdk.

#include "engine/networking/INetworkDiscovery.hpp"

#include <algorithm>
#include <cstdio>
#include <map>

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

}  // namespace

namespace {

class NetworkDiscoveryImpl final : public INetworkDiscovery {
public:
    NetworkDiscoveryImpl(std::string sessionId, std::string& errorOut) : session_(std::move(sessionId)) {
        if (session_.empty()) {
            errorOut = "session id must be non-empty";
            valid_ = false;
        }
    }

    bool valid() const { return valid_; }

    const std::string& session_id() const override { return session_; }

    bool register_service(const DiscoveryService& service, std::string& errorOut) override {
        if (!valid_) { errorOut = "invalid session"; return false; }
        if (service.type.empty()) { errorOut = "type must be non-empty"; return false; }
        if (service.endpoint.empty()) { errorOut = "endpoint must be non-empty"; return false; }
        services_[service.service_id] = service;
        return true;
    }

    void unregister_service(std::uint64_t service_id) override {
        services_.erase(service_id);
    }

    void report_health(std::uint64_t service_id, bool ok) override {
        auto it = services_.find(service_id);
        if (it == services_.end()) return;
        if (ok) {
            it->second.consecutive_failures = 0;
        } else {
            it->second.consecutive_failures += 1;
        }
        it->second.healthy = (it->second.consecutive_failures == 0);
    }

    std::vector<DiscoveryService> resolve(const std::string& type) const override {
        std::vector<DiscoveryService> out;
        for (const auto& kv : services_) {
            if (kv.second.type == type && kv.second.healthy) {
                out.push_back(kv.second);
            }
        }
        return out;
    }

    std::vector<DiscoveryService> services() const override {
        std::vector<DiscoveryService> out;
        out.reserve(services_.size());
        for (const auto& kv : services_) out.push_back(kv.second);
        return out;
    }

    bool reset(std::string& errorOut) override {
        if (!valid_) { errorOut = "invalid session"; return false; }
        services_.clear();
        return true;
    }

    bool load_from_json(const std::string& json, std::string& errorOut) override {
        if (!valid_) { errorOut = "invalid session"; return false; }
        std::map<std::uint64_t, DiscoveryService> newServices;
        if (!parse_state(json, newServices, errorOut)) return false;
        services_ = std::move(newServices);
        return true;
    }

    std::string serialize_state() const override {
        std::string out = "{\"version\":1,\"session\":\"" + json_escape(session_) +
                          "\",\"services\":[";
        bool first = true;
        for (const auto& kv : services_) {
            if (!first) out += ",";
            first = false;
            const auto& s = kv.second;
            out += "{\"service_id\":" + std::to_string(s.service_id) +
                   ",\"type\":\"" + json_escape(s.type) +
                   "\",\"endpoint\":\"" + json_escape(s.endpoint) +
                   "\",\"consecutive_failures\":" + std::to_string(s.consecutive_failures) +
                   ",\"healthy\":" + (s.healthy ? "true" : "false") + "}";
        }
        out += "]}";
        return out;
    }

private:
    bool parse_state(const std::string& json,
                     std::map<std::uint64_t, DiscoveryService>& svcOut,
                     std::string& errorOut) const {
        std::size_t i = 0;
        if (!skip_ws(json, i) || json[i] != '{') { errorOut = "expected object"; return false; }
        ++i;
        int version = 0;
        std::string session;
        bool haveVersion = false, haveSession = false, haveServices = false;
        while (true) {
            std::string key;
            if (!json_field(json, i, key)) { errorOut = "bad field"; return false; }
            if (key == "version") {
                if (!json_uint64(json, i, *reinterpret_cast<std::uint64_t*>(&version))) { errorOut = "bad version"; return false; }
                haveVersion = true;
            } else if (key == "session") {
                if (!json_string(json, i, session)) { errorOut = "bad session"; return false; }
                haveSession = true;
            } else if (key == "services") {
                if (!skip_ws(json, i) || json[i] != '[') { errorOut = "bad services"; return false; }
                ++i;
                if (skip_ws(json, i) && json[i] == ']') { ++i; haveServices = true; }
                else {
                    while (true) {
                        if (!skip_ws(json, i) || json[i] != '{') { errorOut = "bad service"; return false; }
                        ++i;
                        DiscoveryService s;
                        bool haveId = false, haveType = false, haveEndpoint = false,
                             haveFailures = false, haveHealthy = false;
                        while (true) {
                            std::string sk;
                            if (!json_field(json, i, sk)) { errorOut = "bad service field"; return false; }
                            if (sk == "service_id") {
                                if (!json_uint64(json, i, s.service_id)) { errorOut = "bad service_id"; return false; }
                                haveId = true;
                            } else if (sk == "type") {
                                if (!json_string(json, i, s.type)) { errorOut = "bad type"; return false; }
                                haveType = true;
                            } else if (sk == "endpoint") {
                                if (!json_string(json, i, s.endpoint)) { errorOut = "bad endpoint"; return false; }
                                haveEndpoint = true;
                            } else if (sk == "consecutive_failures") {
                                if (!json_uint64(json, i, s.consecutive_failures)) { errorOut = "bad consecutive_failures"; return false; }
                                haveFailures = true;
                            } else if (sk == "healthy") {
                                if (!json_bool(json, i, s.healthy)) { errorOut = "bad healthy"; return false; }
                                haveHealthy = true;
                            } else { errorOut = "unknown service field: " + sk; return false; }
                            if (!skip_ws(json, i)) { errorOut = "bad service end"; return false; }
                            if (json[i] == '}') { ++i; break; }
                            if (json[i] != ',') { errorOut = "bad service separator"; return false; }
                            ++i;
                        }
                        if (!haveId || !haveType || !haveEndpoint || !haveFailures || !haveHealthy) {
                            errorOut = "incomplete service"; return false;
                        }
                        if (s.type.empty()) { errorOut = "type must be non-empty"; return false; }
                        if (s.endpoint.empty()) { errorOut = "endpoint must be non-empty"; return false; }
                        if (s.consecutive_failures > 0 && s.healthy) {
                            errorOut = "healthy with failures"; return false;
                        }
                        if (s.consecutive_failures == 0 && !s.healthy) {
                            errorOut = "unhealthy with zero failures"; return false;
                        }
                        if (svcOut.count(s.service_id)) { errorOut = "duplicate service_id"; return false; }
                        svcOut.emplace(s.service_id, s);
                        if (!skip_ws(json, i)) { errorOut = "bad services end"; return false; }
                        if (json[i] == ']') { ++i; haveServices = true; break; }
                        if (json[i] != ',') { errorOut = "bad services separator"; return false; }
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
        if (!haveServices) { errorOut = "missing services"; return false; }
        return true;
    }

    std::string session_;
    bool valid_{ true };
    std::map<std::uint64_t, DiscoveryService> services_;
};

}  // namespace

std::unique_ptr<INetworkDiscovery> create_network_discovery(const std::string& sessionId,
                                                            std::string& errorOut) {
    auto impl = std::make_unique<NetworkDiscoveryImpl>(sessionId, errorOut);
    if (!impl->valid()) return nullptr;
    return impl;
}

}  // namespace engine::networking
