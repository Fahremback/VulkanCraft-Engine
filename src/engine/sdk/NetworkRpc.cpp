// NetworkRpc.cpp — adapter do contrato INetworkRpc (engine::networking).
// Implementação determinística: registro de procedimentos por nome, fila de
// chamadas com id global estritamente crescente, drain destrutivo em ordem de
// enqueue com ack por sucesso, JSON bit-exact all-or-nothing.
// Self-contained (std only) — mesma convenção dos demais adapters do sdk.

#include "engine/networking/INetworkRpc.hpp"

#include <algorithm>
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
    ++i;  // fecha aspas
    if (!skip_ws(s, i) || s[i] != ':') return false;
    ++i;  // ':'
    return true;
}

// Lê uma string JSON ("...") no cursor.
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

// Lê um array JSON de bytes ([1,2,3]) no cursor.
bool json_bytes(const std::string& s, std::size_t& i, std::vector<std::uint8_t>& out) {
    if (!skip_ws(s, i) || s[i] != '[') return false;
    ++i;
    out.clear();
    if (!skip_ws(s, i)) return false;
    if (s[i] == ']') { ++i; return true; }
    while (true) {
        if (!skip_ws(s, i)) return false;
        std::size_t start = i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
        if (i == start) return false;
        long v = 0;
        for (std::size_t k = start; k < i; ++k) v = v * 10 + (s[k] - '0');
        if (v < 0 || v > 255) return false;
        out.push_back(static_cast<std::uint8_t>(v));
        if (!skip_ws(s, i)) return false;
        if (s[i] == ']') { ++i; return true; }
        if (s[i] != ',') return false;
        ++i;
    }
}

std::string bytes_json(const std::vector<std::uint8_t>& bytes) {
    std::string out = "[";
    for (std::size_t k = 0; k < bytes.size(); ++k) {
        if (k) out += ",";
        out += std::to_string(bytes[k]);
    }
    out += "]";
    return out;
}

}  // namespace

namespace {

class NetworkRpcImpl final : public INetworkRpc {
public:
    NetworkRpcImpl(std::string sessionId, std::string& errorOut) : session_(std::move(sessionId)) {
        if (session_.empty()) {
            errorOut = "session id must be non-empty";
            valid_ = false;
        }
    }

    bool valid() const { return valid_; }

    const std::string& session_id() const override { return session_; }

    bool register_procedure(const std::string& procedure, RpcHandler handler,
                            std::string& errorOut) override {
        if (!valid_) { errorOut = "invalid session"; return false; }
        if (procedure.empty()) { errorOut = "procedure must be non-empty"; return false; }
        if (!handler) { errorOut = "handler must be non-null"; return false; }
        if (procedures_.count(procedure)) { errorOut = "procedure already registered: " + procedure; return false; }
        procedures_.emplace(procedure, std::move(handler));
        return true;
    }

    void unregister_procedure(const std::string& procedure) override {
        procedures_.erase(procedure);
    }

    bool enqueue_call(const std::string& procedure,
                      const std::vector<std::uint8_t>& payload,
                      std::string& errorOut) override {
        if (!valid_) { errorOut = "invalid session"; return false; }
        if (!procedures_.count(procedure)) { errorOut = "unknown procedure: " + procedure; return false; }
        if (payload.empty()) { errorOut = "payload must be non-empty"; return false; }
        RpcCall call;
        call.call_id = next_call_id_++;
        call.procedure = procedure;
        call.payload = payload;
        queue_.push_back(std::move(call));
        return true;
    }

    std::vector<RpcResult> drain(std::string& errorOut) override {
        std::vector<RpcResult> results;
        if (!valid_) { errorOut = "invalid session"; return results; }
        results.reserve(queue_.size());
        for (auto& call : queue_) {
            RpcResult result;
            auto it = procedures_.find(call.procedure);
            if (it == procedures_.end()) {
                result.ok = false;
                result.error = "unknown procedure: " + call.procedure;
            } else {
                result = it->second(call.payload);
                if (result.ok) call.acked = true;
            }
            results.push_back(std::move(result));
        }
        queue_.clear();
        return results;
    }

    std::vector<RpcCall> pending_calls() const override {
        std::vector<RpcCall> out;
        out.reserve(queue_.size());
        for (const auto& call : queue_) out.push_back(call);
        return out;
    }

    std::vector<std::string> procedures() const override {
        std::vector<std::string> out;
        out.reserve(procedures_.size());
        for (const auto& kv : procedures_) out.push_back(kv.first);
        return out;
    }

    std::uint64_t next_call_id() const override { return next_call_id_; }

    bool reset(std::string& errorOut) override {
        if (!valid_) { errorOut = "invalid session"; return false; }
        procedures_.clear();
        queue_.clear();
        next_call_id_ = 1;
        return true;
    }

    bool load_from_json(const std::string& json, std::string& errorOut) override {
        if (!valid_) { errorOut = "invalid session"; return false; }
        std::string err;
        std::uint64_t nextId = 0;
        std::vector<RpcCall> calls;
        if (!parse_state(json, nextId, calls, err)) {
            errorOut = err;
            return false;  // all-or-nothing: estado anterior intacto
        }
        next_call_id_ = nextId;
        queue_ = std::move(calls);
        return true;
    }

    std::string serialize_state() const override {
        std::string out = "{\"version\":1,\"session\":\"" + json_escape(session_) +
                          "\",\"next_call_id\":" + std::to_string(next_call_id_) + ",\"calls\":[";
        for (std::size_t k = 0; k < queue_.size(); ++k) {
            if (k) out += ",";
            const auto& call = queue_[k];
            out += "{\"call_id\":" + std::to_string(call.call_id) +
                   ",\"procedure\":\"" + json_escape(call.procedure) +
                   "\",\"payload\":" + bytes_json(call.payload) +
                   ",\"acked\":" + (call.acked ? "true" : "false") + "}";
        }
        out += "]}";
        return out;
    }

private:
    bool parse_state(const std::string& json, std::uint64_t& nextId,
                     std::vector<RpcCall>& calls, std::string& errorOut) const {
        std::size_t i = 0;
        if (!skip_ws(json, i) || json[i] != '{') { errorOut = "expected object"; return false; }
        ++i;
        int version = 0;
        std::string session;
        bool haveVersion = false, haveSession = false, haveNext = false, haveCalls = false;
        while (true) {
            std::string key;
            if (!json_field(json, i, key)) { errorOut = "bad field"; return false; }
            if (key == "version") {
                if (!skip_ws(json, i) || json[i] < '0' || json[i] > '9') { errorOut = "bad version"; return false; }
                std::size_t start = i;
                while (i < json.size() && json[i] >= '0' && json[i] <= '9') ++i;
                version = std::stoi(json.substr(start, i - start));
                haveVersion = true;
            } else if (key == "session") {
                if (!json_string(json, i, session)) { errorOut = "bad session"; return false; }
                haveSession = true;
            } else if (key == "next_call_id") {
                if (!skip_ws(json, i) || json[i] < '0' || json[i] > '9') { errorOut = "bad next_call_id"; return false; }
                std::size_t start = i;
                while (i < json.size() && json[i] >= '0' && json[i] <= '9') ++i;
                nextId = std::stoull(json.substr(start, i - start));
                haveNext = true;
            } else if (key == "calls") {
                if (!skip_ws(json, i) || json[i] != '[') { errorOut = "bad calls"; return false; }
                ++i;
                if (skip_ws(json, i) && json[i] == ']') { ++i; haveCalls = true; }
                else {
                    while (true) {
                        if (!skip_ws(json, i) || json[i] != '{') { errorOut = "bad call"; return false; }
                        ++i;
                        RpcCall call;
                        bool haveId = false, haveProc = false, havePayload = false, haveAcked = false;
                        while (true) {
                            std::string ck;
                            if (!json_field(json, i, ck)) { errorOut = "bad call field"; return false; }
                            if (ck == "call_id") {
                                if (!skip_ws(json, i) || json[i] < '0' || json[i] > '9') { errorOut = "bad call_id"; return false; }
                                std::size_t start = i;
                                while (i < json.size() && json[i] >= '0' && json[i] <= '9') ++i;
                                call.call_id = std::stoull(json.substr(start, i - start));
                                haveId = true;
                            } else if (ck == "procedure") {
                                if (!json_string(json, i, call.procedure)) { errorOut = "bad procedure"; return false; }
                                haveProc = true;
                            } else if (ck == "payload") {
                                if (!json_bytes(json, i, call.payload)) { errorOut = "bad payload"; return false; }
                                havePayload = true;
                            } else if (ck == "acked") {
                                if (!skip_ws(json, i)) { errorOut = "bad acked"; return false; }
                                if (json.compare(i, 4, "true") == 0) { call.acked = true; i += 4; }
                                else if (json.compare(i, 5, "false") == 0) { call.acked = false; i += 5; }
                                else { errorOut = "bad acked"; return false; }
                                haveAcked = true;
                            } else { errorOut = "unknown call field: " + ck; return false; }
                            if (!skip_ws(json, i)) { errorOut = "bad call end"; return false; }
                            if (json[i] == '}') { ++i; break; }
                            if (json[i] != ',') { errorOut = "bad call separator"; return false; }
                            ++i;
                        }
                        if (!haveId || !haveProc || !havePayload || !haveAcked) { errorOut = "incomplete call"; return false; }
                        if (call.call_id == 0) { errorOut = "call_id must be non-zero"; return false; }
                        if (call.procedure.empty()) { errorOut = "procedure must be non-empty"; return false; }
                        if (call.payload.empty()) { errorOut = "payload must be non-empty"; return false; }
                        calls.push_back(std::move(call));
                        if (!skip_ws(json, i)) { errorOut = "bad calls end"; return false; }
                        if (json[i] == ']') { ++i; haveCalls = true; break; }
                        if (json[i] != ',') { errorOut = "bad calls separator"; return false; }
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
        if (!haveNext || !haveCalls) { errorOut = "missing next_call_id/calls"; return false; }
        // Valida a ordem: ids estritamente crescentes.
        for (std::size_t k = 1; k < calls.size(); ++k) {
            if (calls[k].call_id <= calls[k - 1].call_id) { errorOut = "call ids not strictly increasing"; return false; }
        }
        if (nextId <= (calls.empty() ? 0 : calls.back().call_id)) { errorOut = "next_call_id must exceed last call"; return false; }
        return true;
    }

    std::string session_;
    bool valid_{ true };
    std::uint64_t next_call_id_{ 1 };
    std::map<std::string, RpcHandler> procedures_;
    std::vector<RpcCall> queue_;
};

}  // namespace

std::unique_ptr<INetworkRpc> create_network_rpc(const std::string& sessionId,
                                                std::string& errorOut) {
    auto impl = std::make_unique<NetworkRpcImpl>(sessionId, errorOut);
    if (!impl->valid()) return nullptr;
    return impl;
}

}  // namespace engine::networking
