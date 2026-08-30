// NetworkReplication.cpp — adapter do contrato INetworkReplication
// (engine::networking). Implementação determinística: aplica frames de
// snapshot em ordem estritamente crescente de tick, mantém o último estado
// por entidade (mapa ordenado), JSON bit-exact all-or-nothing.
// Self-contained (std only) — mesma convenção dos demais adapters do sdk.

#include "engine/networking/INetworkReplication.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <sstream>
#include <unordered_set>

namespace engine::networking {

namespace {

// Escapa uma string para JSON (mesma convenção dos demais adapters do sdk).
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

// Lê um campo `"key":` no cursor. Retorna false no fim do objeto (ou inválido).
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
    ++i;  // fecha aspas
    if (!skip_ws(s, i) || s[i] != ':') return false;
    ++i;
    return true;
}

bool json_comma(const std::string& s, std::size_t& i) {
    if (!skip_ws(s, i)) return false;
    if (s[i] != ',') return false;
    ++i;
    return true;
}

bool json_number(const std::string& s, std::size_t& i, double& out) {
    if (!skip_ws(s, i)) return false;
    std::size_t start = i;
    while (i < s.size() && (s[i] == '-' || s[i] == '+' || s[i] == '.' ||
                            (s[i] >= '0' && s[i] <= '9') || s[i] == 'e' || s[i] == 'E')) ++i;
    if (i == start) return false;
    try {
        out = std::stod(s.substr(start, i - start));
    } catch (...) {
        return false;
    }
    return true;
}

bool json_array_start(const std::string& s, std::size_t& i) {
    if (!skip_ws(s, i) || s[i] != '[') return false;
    ++i;
    return true;
}

// Bytes como array JSON de números (`[1,2,3]`) — bit-exact.
std::string bytes_json(const std::vector<std::uint8_t>& bytes) {
    std::string out = "[";
    for (std::size_t n = 0; n < bytes.size(); ++n) {
        if (n) out += ",";
        out += std::to_string(static_cast<unsigned>(bytes[n]));
    }
    out += "]";
    return out;
}

bool parse_bytes(const std::string& s, std::size_t& i, std::vector<std::uint8_t>& out) {
    if (!json_array_start(s, i)) return false;
    out.clear();
    while (true) {
        if (!skip_ws(s, i)) return false;
        if (s[i] == ']') { ++i; return true; }
        double value = 0.0;
        if (!json_number(s, i, value) || value < 0.0 || value > 255.0 || std::floor(value) != value) return false;
        out.push_back(static_cast<std::uint8_t>(value));
        if (!skip_ws(s, i)) return false;
        if (s[i] == ',') { ++i; continue; }
        if (s[i] == ']') { ++i; return true; }
        return false;
    }
}

}  // namespace

class NetworkReplicationImpl final : public INetworkReplication {
public:
    // Accessor público p/ a factory (fix P0 transitório do Agente 1: a factory
    // fora da classe precisa checar o estado de construção).
    bool isOk() const { return ok_; }

    NetworkReplicationImpl(std::string sessionId, std::string& errorOut) {
        if (sessionId.empty()) {
            errorOut = "network: session_id vazio";
            return;
        }
        sessionId_ = std::move(sessionId);
        ok_ = true;
    }

    bool apply_frame(const ReplicationFrame& frame, std::string& errorOut) override {
        if (!ok_) { errorOut = "network: sessão inválida"; return false; }
        if (frame.tick <= lastTick_) {
            errorOut = "network: tick fora de ordem (tick " + std::to_string(frame.tick) +
                       " <= último " + std::to_string(lastTick_) + ")";
            return false;
        }
        // Validação all-or-nothing: ids únicos no frame + kind não-vazio.
        // Duplicate detection via hash set (O(n)) instead of a linear find
        // per state (O(n^2)) for frames replicating many entities.
        std::unordered_set<std::uint64_t> seen;
        seen.reserve(frame.states.size());
        for (const NetworkEntityState& st : frame.states) {
            if (st.kind.empty()) {
                errorOut = "network: kind vazio na entidade " + std::to_string(st.entity_id);
                return false;
            }
            if (!seen.insert(st.entity_id).second) {
                errorOut = "network: entity_id duplicado no frame (" + std::to_string(st.entity_id) + ")";
                return false;
            }
        }
        // Aplica (só chega aqui se TUDO validou).
        for (const NetworkEntityState& st : frame.states) entities_[st.entity_id] = st;
        lastTick_ = frame.tick;
        ++tickCount_;
        return true;
    }

    const NetworkEntityState* state(std::uint64_t entity_id) const override {
        auto it = entities_.find(entity_id);
        return it == entities_.end() ? nullptr : &it->second;
    }

    std::vector<std::uint64_t> entity_ids() const override {
        std::vector<std::uint64_t> ids;
        ids.reserve(entities_.size());
        for (const auto& pair : entities_) ids.push_back(pair.first);
        return ids;
    }

    std::uint64_t tick_count() const override { return tickCount_; }
    std::uint64_t last_tick() const override { return lastTick_; }

    bool reset(std::string& errorOut) override {
        (void)errorOut;
        entities_.clear();
        lastTick_ = 0;
        tickCount_ = 0;
        return true;
    }

    bool load_from_json(const std::string& json, std::string& errorOut) override {
        std::string sessionId;
        std::uint64_t lastTick = 0;
        std::map<std::uint64_t, NetworkEntityState> parsed;
        std::size_t i = 0;
        if (!skip_ws(json, i) || json[i] != '{') { errorOut = "network: documento não é objeto"; return false; }
        ++i;

        bool sawSession = false, sawEntities = false;
        std::string key;
        while (true) {
            if (!json_field(json, i, key)) {
                if (skip_ws(json, i) && json[i] == '}') { ++i; break; }
                errorOut = "network: campo inválido";
                return false;
            }
            if (key == "version") {
                double value = 0.0;
                if (!json_number(json, i, value) || value != 1.0) { errorOut = "network: versão inválida"; return false; }
            } else if (key == "session_id") {
                std::string value;
                if (!json_string(json, i, value) || value.empty()) { errorOut = "network: session_id inválido"; return false; }
                sessionId = value;
                sawSession = true;
            } else if (key == "last_tick") {
                double value = 0.0;
                if (!json_number(json, i, value) || value < 0.0 || std::floor(value) != value) { errorOut = "network: last_tick inválido"; return false; }
                lastTick = static_cast<std::uint64_t>(value);
            } else if (key == "entities") {
                sawEntities = true;
                if (!json_array_start(json, i)) { errorOut = "network: entities não é array"; return false; }
                while (true) {
                    if (!skip_ws(json, i)) { errorOut = "network: entities malformado"; return false; }
                    if (json[i] == ']') { ++i; break; }
                    NetworkEntityState st;
                    if (!parse_entity(json, i, st, errorOut)) return false;
                    if (parsed.find(st.entity_id) != parsed.end()) {
                        errorOut = "network: entity_id duplicado no documento (" + std::to_string(st.entity_id) + ")";
                        return false;
                    }
                    parsed[st.entity_id] = std::move(st);
                    if (!skip_ws(json, i)) { errorOut = "network: entities malformado"; return false; }
                    if (json[i] == ',') { ++i; continue; }
                    if (json[i] == ']') { ++i; break; }
                    errorOut = "network: separador inválido em entities";
                    return false;
                }
            } else {
                errorOut = "network: campo desconhecido \"" + key + "\"";
                return false;
            }
            if (skip_ws(json, i) && json[i] == '}') { ++i; break; }
            if (!json_comma(json, i)) { errorOut = "network: separador inválido"; return false; }
        }
        if (!sawSession) { errorOut = "network: session_id ausente"; return false; }
        if (!sawEntities) { errorOut = "network: entities ausente"; return false; }

        sessionId_ = std::move(sessionId);
        lastTick_ = lastTick;
        tickCount_ = parsed.empty() ? 0 : 1;  // contagem derivada: 1 documento = 1 aplicação
        entities_ = std::move(parsed);
        ok_ = true;
        return true;
    }

    std::string serialize_state() const override {
        std::ostringstream out;
        out << "{\"version\":1,\"session_id\":\"" << json_escape(sessionId_)
            << "\",\"last_tick\":" << lastTick_
            << ",\"entities\":[";
        bool first = true;
        for (const auto& pair : entities_) {
            if (!first) out << ",";
            first = false;
            out << "{\"id\":" << pair.first
                << ",\"kind\":\"" << json_escape(pair.second.kind)
                << "\",\"data\":" << bytes_json(pair.second.data) << "}";
        }
        out << "]}";
        return out.str();
    }

    const std::string& session_id() const override { return sessionId_; }

private:
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

    bool parse_entity(const std::string& s, std::size_t& i, NetworkEntityState& out, std::string& errorOut) {
        if (!skip_ws(s, i) || s[i] != '{') { errorOut = "network: entidade não é objeto"; return false; }
        ++i;
        bool sawId = false, sawKind = false, sawData = false;
        std::string key;
        while (true) {
            if (!json_field(s, i, key)) {
                if (skip_ws(s, i) && s[i] == '}') { ++i; break; }
                errorOut = "network: campo de entidade inválido";
                return false;
            }
            if (key == "id") {
                double value = 0.0;
                if (!json_number(s, i, value) || value < 0.0 || std::floor(value) != value) { errorOut = "network: id inválido"; return false; }
                out.entity_id = static_cast<std::uint64_t>(value);
                sawId = true;
            } else if (key == "kind") {
                if (!json_string(s, i, out.kind) || out.kind.empty()) { errorOut = "network: kind inválido"; return false; }
                sawKind = true;
            } else if (key == "data") {
                if (!parse_bytes(s, i, out.data)) { errorOut = "network: data inválido"; return false; }
                sawData = true;
            } else {
                errorOut = "network: campo de entidade desconhecido \"" + key + "\"";
                return false;
            }
            if (skip_ws(s, i) && s[i] == '}') { ++i; break; }
            if (!json_comma(s, i)) { errorOut = "network: separador inválido em entidade"; return false; }
        }
        if (!sawId || !sawKind || !sawData) { errorOut = "network: entidade incompleta (id/kind/data)"; return false; }
        return true;
    }

    std::string sessionId_;
    std::map<std::uint64_t, NetworkEntityState> entities_;
    std::uint64_t lastTick_ = 0;
    std::uint64_t tickCount_ = 0;
    bool ok_ = false;
};

std::unique_ptr<INetworkReplication> create_network_replication(const std::string& sessionId,
                                                                std::string& errorOut) {
    auto impl = std::make_unique<NetworkReplicationImpl>(sessionId, errorOut);
    if (!impl->isOk()) return nullptr;
    return impl;
}

}  // namespace engine::networking
