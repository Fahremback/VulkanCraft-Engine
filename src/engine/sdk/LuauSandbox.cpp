// LuauSandbox.cpp — adapter do contrato ILuauSandbox (engine::scripting,
// §3 item 6 — "Integrar runtime Luau sandboxed opcional com bindings gerados
// e budgets de CPU/memória").
//
// Implementação determinística da POLÍTICA de sandbox: runner substituível
// (IScriptRunner — testes usam um runner determinístico; produção pluga o
// Luau vendido, provado utilizável em #302-luau-probe); configure
// all-or-nothing (política inválida → false, estado intacto); avaliação exige
// runner anexado (sem runner → false, nada muda); budget de instruções
// enforceado sobre o que o runner reporta (estourou → ScriptResult com tag
// "budget", contador de execuções NÃO incrementa — all-or-nothing); guarda de
// profundidade de chamada; args_json validado como JSON antes de chegar ao
// runner; JSON bit-exact all-or-nothing na persistência (id mismatch /
// política inválida / contador não-crescente / campo desconhecido / trailing
// rejeitam o documento inteiro e deixam o estado anterior intacto).
// Self-contained (std only) — mesma convenção dos demais adapters do sdk.

#include "engine/scripting/ILuauSandbox.hpp"

#include <algorithm>
#include <cstdio>
#include <map>

namespace engine::scripting {

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

// Valida que `json` é um documento JSON bem-formado (parse estrutural leve,
// sem semântica): objeto/array/string/número/bool/null. Usado para recusar
// `args_json` e documentos de persistência malformados antes de qualquer
// efeito (all-or-nothing).
bool json_well_formed(const std::string& s, std::size_t& i) {
    if (!skip_ws(s, i)) return false;
    if (s[i] == '{') {
        ++i;
        if (!skip_ws(s, i)) return false;
        if (s[i] == '}') { ++i; return true; }
        bool first = true;
        for (;;) {
            if (!first) {
                if (!skip_ws(s, i) || s[i] != ',') return false;
                ++i;
            }
            first = false;
            if (!skip_ws(s, i) || s[i] != '"') return false;
            ++i;
            while (i < s.size() && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < s.size()) i += 2; else ++i;
            }
            if (i >= s.size()) return false;
            ++i;
            if (!skip_ws(s, i) || s[i] != ':') return false;
            ++i;
            if (!json_well_formed(s, i)) return false;
            if (!skip_ws(s, i)) return false;
            if (s[i] == '}') { ++i; return true; }
        }
    }
    if (s[i] == '[') {
        ++i;
        if (!skip_ws(s, i)) return false;
        if (s[i] == ']') { ++i; return true; }
        bool first = true;
        for (;;) {
            if (!first) {
                if (!skip_ws(s, i) || s[i] != ',') return false;
                ++i;
            }
            first = false;
            if (!json_well_formed(s, i)) return false;
            if (!skip_ws(s, i)) return false;
            if (s[i] == ']') { ++i; return true; }
        }
    }
    if (s[i] == '"') {
        ++i;
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size()) i += 2; else ++i;
        }
        if (i >= s.size()) return false;
        ++i;
        return true;
    }
    if (s.compare(i, 4, "true") == 0) { i += 4; return true; }
    if (s.compare(i, 5, "false") == 0) { i += 5; return true; }
    if (s.compare(i, 4, "null") == 0) { i += 4; return true; }
    // número
    std::size_t j = i;
    if (j < s.size() && (s[j] == '-' || s[j] == '+')) ++j;
    bool digits = false;
    while (j < s.size() && s[j] >= '0' && s[j] <= '9') { ++j; digits = true; }
    if (j < s.size() && s[j] == '.') {
        ++j;
        while (j < s.size() && s[j] >= '0' && s[j] <= '9') ++j;
    }
    if (j < s.size() && (s[j] == 'e' || s[j] == 'E')) {
        ++j;
        if (j < s.size() && (s[j] == '-' || s[j] == '+')) ++j;
        while (j < s.size() && s[j] >= '0' && s[j] <= '9') ++j;
    }
    if (!digits || (j < s.size() && s[j] != ',' && s[j] != '}' && s[j] != ']')) return false;
    i = j;
    return true;
}

bool valid_policy(const SandboxPolicy& p) {
    if (p.max_instructions < 1) return false;
    if (p.max_call_depth < 1) return false;
    if (p.allow_require) return false;  // require exige allowlist de módulos (não implementada)
    for (const auto& g : p.allowed_globals) {
        if (g.empty()) return false;
    }
    return true;
}

bool policies_equal(const SandboxPolicy& a, const SandboxPolicy& b) {
    return a.max_instructions == b.max_instructions &&
           a.max_call_depth == b.max_call_depth &&
           a.allow_io == b.allow_io &&
           a.allow_require == b.allow_require &&
           a.allowed_globals == b.allowed_globals;
}

std::string policy_json(const SandboxPolicy& p) {
    std::string out = "{\"max_instructions\":" + std::to_string(p.max_instructions);
    out += ",\"max_call_depth\":" + std::to_string(p.max_call_depth);
    out += ",\"allow_io\":" + std::string(p.allow_io ? "true" : "false");
    out += ",\"allow_require\":" + std::string(p.allow_require ? "true" : "false");
    out += ",\"allowed_globals\":[";
    bool first = true;
    for (const auto& g : p.allowed_globals) {
        if (!first) out += ',';
        first = false;
        out += "\"" + json_escape(g) + "\"";
    }
    out += "]}";
    return out;
}

}  // namespace

namespace {

class LuauSandboxImpl final : public ILuauSandbox {
public:
    LuauSandboxImpl(const std::string& sandboxId, IScriptRunner* runner,
                    const SandboxPolicy& policy)
        : sandbox_id_(sandboxId), runner_(runner), policy_(policy) {}

    const std::string& sandbox_id() const override { return sandbox_id_; }

    bool attach_runner(IScriptRunner* runner, std::string& errorOut) override {
        if (runner == nullptr) { errorOut = "null runner"; return false; }
        runner_ = runner;
        errorOut.clear();
        return true;
    }

    bool configure(const SandboxPolicy& policy, std::string& errorOut) override {
        if (!valid_policy(policy)) {
            errorOut = "invalid sandbox policy (max_instructions/max_call_depth >= 1; "
                       "allow_require requires module allowlist)";
            return false;
        }
        policy_ = policy;
        errorOut.clear();
        return true;
    }

    const SandboxPolicy& policy() const override { return policy_; }

    ScriptResult evaluate(const std::string& source,
                          const std::string& entry,
                          std::string& errorOut) override {
        return run(source, entry, "{}", errorOut);
    }

    ScriptResult call(const std::string& source,
                      const std::string& entry,
                      const std::string& args_json,
                      std::string& errorOut) override {
        return run(source, entry, args_json, errorOut);
    }

    std::uint64_t executions() const override { return executions_; }

    bool reset(std::string& errorOut) override {
        executions_ = 0;
        errorOut.clear();
        return true;
    }

    bool load_from_json(const std::string& json, std::string& errorOut) override {
        // Documento: {"sandbox_id": "...", "executions": N,
        //  "policy": {max_instructions, max_call_depth, allow_io,
        //             allow_require, allowed_globals: [...]}}
        std::string text;
        std::size_t i = 0;
        if (!skip_ws(json, i) || json[i] != '{') { errorOut = "not an object"; return false; }
        ++i;
        bool have_id = false, have_exec = false, have_policy = false;
        std::uint64_t parsed_exec = 0;
        SandboxPolicy parsed_policy = policy_;
        bool first_field = true;
        for (;;) {
            if (!skip_ws(json, i)) { errorOut = "unterminated"; return false; }
            if (json[i] == '}') { ++i; break; }
            if (!first_field) {
                if (json[i] != ',') { errorOut = "bad comma"; return false; }
                ++i;
            }
            first_field = false;
            if (!skip_ws(json, i) || json[i] != '"') { errorOut = "expected key"; return false; }
            const std::size_t ks = ++i;
            while (i < json.size() && json[i] != '"') ++i;
            if (i >= json.size()) { errorOut = "unterminated key"; return false; }
            const std::string key = json.substr(ks, i - ks);
            ++i;
            if (!skip_ws(json, i) || json[i] != ':') { errorOut = "expected ':'"; return false; }
            ++i;
            if (key == "sandbox_id") {
                if (!read_string(json, i, text) || text != sandbox_id_) {
                    errorOut = "sandbox_id mismatch";
                    return false;
                }
                have_id = true;
            } else if (key == "executions") {
                if (!read_u64(json, i, parsed_exec)) { errorOut = "bad executions"; return false; }
                have_exec = true;
            } else if (key == "policy") {
                if (!read_policy(json, i, parsed_policy)) { errorOut = "bad policy"; return false; }
                have_policy = true;
            } else {
                errorOut = "unknown field: " + key;
                return false;
            }
        }
        if (!have_id) { errorOut = "missing sandbox_id"; return false; }
        if (!have_exec) { errorOut = "missing executions"; return false; }
        if (!have_policy) { errorOut = "missing policy"; return false; }
        if (!valid_policy(parsed_policy)) { errorOut = "invalid policy"; return false; }
        // Rejeita conteúdo após o fechamento do objeto raiz (all-or-nothing).
        if (skip_ws(json, i) && i < json.size()) { errorOut = "trailing content"; return false; }
        if (parsed_exec < executions_) {
            errorOut = "executions not increasing";
            return false;
        }
        // Só comita no final (all-or-nothing).
        executions_ = parsed_exec;
        policy_ = parsed_policy;
        return true;
    }

    std::string serialize_state() const override {
        std::string out = "{\"sandbox_id\":\"";
        out += json_escape(sandbox_id_);
        out += "\",\"executions\":" + std::to_string(executions_);
        out += ",\"policy\":" + policy_json(policy_);
        out += "}";
        return out;
    }

private:
    ScriptResult run(const std::string& source,
                     const std::string& entry,
                     const std::string& args_json,
                     std::string& errorOut) {
        if (runner_ == nullptr) { errorOut = "no runner attached"; return {}; }
        std::size_t probe = 0;
        if (!json_well_formed(args_json, probe)) {
            errorOut = "args_json is not valid JSON";
            return {};
        }
        if (source.empty()) { errorOut = "empty source"; return {}; }
        if (entry.empty()) { errorOut = "empty entry"; return {}; }

        ScriptResult r = runner_->run(source, entry, args_json,
                                      policy_.max_instructions, 1, errorOut);
        // Política: runner violou o teto → erro determinístico, nada muda.
        if (r.ok && r.instructions_used > policy_.max_instructions) {
            ScriptResult out;
            out.ok = false;
            out.error = "budget exceeded (" + std::to_string(r.instructions_used) +
                        " instructions > " + std::to_string(policy_.max_instructions) + ")";
            out.instructions_used = r.instructions_used;
            errorOut.clear();
            return out;
        }
        if (r.ok) {
            // Sucesso: valor deve ser JSON bem-formado (bit-exact do runner).
            std::size_t check = 0;
            if (!json_well_formed(r.value, check) || check < r.value.size()) {
                ScriptResult out;
                out.ok = false;
                out.error = "runner returned malformed value JSON";
                out.instructions_used = r.instructions_used;
                errorOut.clear();
                return out;
            }
            ++executions_;
        } else {
            // Tag estável obrigatória: budget/sandbox/runtime/compile.
            if (r.error.find("budget") == std::string::npos &&
                r.error.find("sandbox") == std::string::npos &&
                r.error.find("runtime") == std::string::npos &&
                r.error.find("compile") == std::string::npos) {
                r.error = "runtime: " + r.error;
            }
        }
        errorOut.clear();
        return r;
    }

    static bool read_string(const std::string& s, std::size_t& i, std::string& out) {
        if (!skip_ws(s, i) || s[i] != '"') return false;
        ++i;
        out.clear();
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size()) {
                const char e = s[i + 1];
                out += (e == 'n') ? '\n' : (e == 'r') ? '\r' : (e == 't') ? '\t' : e;
                i += 2;
            } else {
                out += s[i++];
            }
        }
        if (i >= s.size()) return false;
        ++i;
        return true;
    }

    static bool read_u64(const std::string& s, std::size_t& i, std::uint64_t& out) {
        if (!skip_ws(s, i)) return false;
        out = 0;
        bool any = false;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') { out = out * 10 + (s[i] - '0'); any = true; ++i; }
        return any;
    }

    static bool read_u32(const std::string& s, std::size_t& i, std::uint32_t& out) {
        std::uint64_t v;
        if (!read_u64(s, i, v) || v > 0xFFFFFFFFull) return false;
        out = static_cast<std::uint32_t>(v);
        return true;
    }

    static bool read_bool(const std::string& s, std::size_t& i, bool& out) {
        if (!skip_ws(s, i)) return false;
        if (s.compare(i, 4, "true") == 0) { out = true; i += 4; return true; }
        if (s.compare(i, 5, "false") == 0) { out = false; i += 5; return true; }
        return false;
    }

    bool read_policy(const std::string& s, std::size_t& i, SandboxPolicy& out) {
        if (!skip_ws(s, i) || s[i] != '{') return false;
        ++i;
        bool have_mi = false, have_md = false, have_io = false, have_req = false, have_glob = false;
        bool first_field = true;
        for (;;) {
            if (!skip_ws(s, i)) return false;
            if (s[i] == '}') { ++i; break; }
            if (!first_field) {
                if (s[i] != ',') return false;
                ++i;
            }
            first_field = false;
            if (!skip_ws(s, i) || s[i] != '"') return false;
            const std::size_t ks = ++i;
            while (i < s.size() && s[i] != '"') ++i;
            if (i >= s.size()) return false;
            const std::string k = s.substr(ks, i - ks);
            ++i;
            if (!skip_ws(s, i) || s[i] != ':') return false;
            ++i;
            // Dispatch por chave ANTES de ler o valor: o array allowed_globals
            // precisa de parse próprio (read_key_value consumiria só o '[').
            if (k == "allowed_globals") {
                if (!read_string_array(s, i, out.allowed_globals)) return false;
                have_glob = true;
            } else {
                std::string v;
                if (!read_key_value_at(s, i, v)) return false;
                std::uint64_t nv64;
                if (k == "max_instructions") {
                    if (!read_u64_from(v, nv64) || nv64 > 0xFFFFFFFFull) return false;
                    out.max_instructions = static_cast<std::uint32_t>(nv64);
                    have_mi = true;
                } else if (k == "max_call_depth") {
                    if (!read_u64_from(v, nv64) || nv64 > 0xFFFFFFFFull) return false;
                    out.max_call_depth = static_cast<std::uint32_t>(nv64);
                    have_md = true;
                } else if (k == "allow_io") {
                    if (v == "true") { out.allow_io = true; have_io = true; }
                    else if (v == "false") { out.allow_io = false; have_io = true; }
                    else return false;
                } else if (k == "allow_require") {
                    if (v == "true") { out.allow_require = true; have_req = true; }
                    else if (v == "false") { out.allow_require = false; have_req = true; }
                    else return false;
                } else {
                    return false;
                }
            }
        }
        return have_mi && have_md && have_io && have_req && have_glob;
    }

    static bool read_string_array(const std::string& s, std::size_t& i,
                                  std::vector<std::string>& out) {
        if (!skip_ws(s, i) || s[i] != '[') return false;
        ++i;
        out.clear();
        bool first = true;
        for (;;) {
            if (!skip_ws(s, i)) return false;
            if (s[i] == ']') { ++i; return true; }
            if (!first) {
                if (s[i] != ',') return false;
                ++i;
            }
            first = false;
            std::string v;
            if (!read_string(s, i, v)) return false;
            out.push_back(v);
        }
    }

    // Lê o VALOR no ponto atual como STRING (número ou string entre aspas).
    static bool read_key_value_at(const std::string& s, std::size_t& i,
                                  std::string& value) {
        if (!skip_ws(s, i)) return false;
        if (s[i] == '"') {
            return read_string(s, i, value);
        }
        const std::size_t vs = i;
        while (i < s.size() && s[i] != ',' && s[i] != '}') ++i;
        value = s.substr(vs, i - vs);
        return true;
    }

    static bool read_u64_from(const std::string& v, std::uint64_t& out) {
        out = 0;
        bool any = false;
        for (const char c : v) {
            if (c < '0' || c > '9') return false;
            out = out * 10 + (c - '0');
            any = true;
        }
        return any;
    }

    std::string sandbox_id_;
    IScriptRunner* runner_{ nullptr };
    SandboxPolicy policy_;
    std::uint64_t executions_{ 0 };
};

}  // namespace

std::unique_ptr<ILuauSandbox> create_luau_sandbox(const std::string& sandboxId,
                                                  IScriptRunner* runner,
                                                  const SandboxPolicy& policy,
                                                  std::string& errorOut) {
    if (sandboxId.empty()) {
        errorOut = "sandbox id must be non-empty";
        return nullptr;
    }
    if (!valid_policy(policy)) {
        errorOut = "invalid sandbox policy (max_instructions/max_call_depth >= 1; "
                   "allow_require requires module allowlist)";
        return nullptr;
    }
    errorOut.clear();
    return std::unique_ptr<ILuauSandbox>(new LuauSandboxImpl(sandboxId, runner, policy));
}

}  // namespace engine::scripting
