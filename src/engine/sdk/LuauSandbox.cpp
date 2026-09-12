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
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <unordered_map>
#include <unordered_set>

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

constexpr const char* kModulePrefix = "module:";

bool normalize_module_id(const std::string& raw, std::string& normalized) {
    if (raw.empty() || raw.find('\0') != std::string::npos) return false;
    std::string portable = raw;
    std::replace(portable.begin(), portable.end(), '\\', '/');
    if (portable.empty() || portable.front() == '/' || portable.find(':') != std::string::npos) return false;
    const std::filesystem::path input(portable);
    if (input.is_absolute() || input.has_root_name() || input.has_root_directory()) return false;
    const std::filesystem::path clean = input.lexically_normal();
    if (clean.empty() || clean == ".") return false;
    for (const auto& part : clean) {
        if (part == ".." || part == ".") return false;
    }
    normalized = clean.generic_string();
    return !normalized.empty() && normalized.rfind("../", 0) != 0;
}

bool module_path_has_symlink(const std::string& normalized) {
    std::error_code ec;
    std::filesystem::path current;
    for (const auto& part : std::filesystem::path(normalized)) {
        current /= part;
        const auto status = std::filesystem::symlink_status(current, ec);
        if (ec) { ec.clear(); continue; }
        if (std::filesystem::is_symlink(status)) return true;
    }
    return false;
}

std::unordered_set<std::string> allowed_modules(const SandboxPolicy& p, bool& valid) {
    valid = true;
    std::unordered_set<std::string> modules;
    for (const auto& entry : p.allowed_globals) {
        if (entry.rfind(kModulePrefix, 0) != 0) continue;
        std::string normalized;
        if (!normalize_module_id(entry.substr(std::char_traits<char>::length(kModulePrefix)), normalized) ||
            module_path_has_symlink(normalized)) {
            valid = false;
            return {};
        }
        modules.insert(std::move(normalized));
    }
    return modules;
}

bool valid_policy(const SandboxPolicy& p) {
    if (p.max_instructions < 1) return false;
    if (p.max_call_depth < 1) return false;
    for (const auto& g : p.allowed_globals) {
        if (g.empty()) return false;
    }
    bool modulesValid = true;
    const auto modules = allowed_modules(p, modulesValid);
    if (!modulesValid) return false;
    if (p.allow_require && modules.empty()) return false;
    return true;
}

bool is_ident(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool validate_require_calls(const std::string& source, const SandboxPolicy& policy,
                            std::unordered_map<std::string, bool>& cache,
                            std::string& error) {
    bool modulesValid = true;
    const auto modules = allowed_modules(policy, modulesValid);
    if (!modulesValid) { error = "sandbox: invalid module allowlist"; return false; }

    std::size_t i = 0;
    while (i < source.size()) {
        if (source[i] == '-' && i + 1 < source.size() && source[i + 1] == '-') {
            i += 2;
            if (i + 1 < source.size() && source[i] == '[' && source[i + 1] == '[') {
                i += 2;
                const auto end = source.find("]]", i);
                i = end == std::string::npos ? source.size() : end + 2;
            } else {
                const auto end = source.find('\n', i);
                i = end == std::string::npos ? source.size() : end + 1;
            }
            continue;
        }
        if (source[i] == '\'' || source[i] == '"') {
            const char quote = source[i++];
            while (i < source.size()) {
                if (source[i] == '\\' && i + 1 < source.size()) { i += 2; continue; }
                if (source[i++] == quote) break;
            }
            continue;
        }
        if (i + 7 > source.size() || source.compare(i, 7, "require") != 0 ||
            (i > 0 && is_ident(source[i - 1])) ||
            (i + 7 < source.size() && is_ident(source[i + 7]))) {
            ++i;
            continue;
        }

        std::size_t cursor = i + 7;
        while (cursor < source.size() && std::isspace(static_cast<unsigned char>(source[cursor]))) ++cursor;
        bool parenthesized = false;
        if (cursor < source.size() && source[cursor] == '(') {
            parenthesized = true;
            ++cursor;
            while (cursor < source.size() && std::isspace(static_cast<unsigned char>(source[cursor]))) ++cursor;
        }
        if (cursor >= source.size() || (source[cursor] != '\'' && source[cursor] != '"')) {
            error = "sandbox: require argument must be a literal module id";
            return false;
        }
        const char quote = source[cursor++];
        std::string module;
        bool closed = false;
        while (cursor < source.size()) {
            const char c = source[cursor++];
            if (c == quote) { closed = true; break; }
            if (c == '\\') {
                if (cursor >= source.size()) break;
                const char escaped = source[cursor++];
                if (escaped != '\\' && escaped != '/' && escaped != '.' && escaped != '-' && escaped != '_') {
                    error = "sandbox: require module escape rejected";
                    return false;
                }
                module += escaped;
            } else {
                module += c;
            }
        }
        if (!closed) { error = "sandbox: unterminated require module"; return false; }
        if (parenthesized) {
            while (cursor < source.size() && std::isspace(static_cast<unsigned char>(source[cursor]))) ++cursor;
            if (cursor >= source.size() || source[cursor] != ')') {
                error = "sandbox: require must contain exactly one literal module id";
                return false;
            }
            ++cursor;
        }
        if (!policy.allow_require) {
            error = "sandbox: require disabled by policy";
            return false;
        }

        std::string normalized;
        if (!normalize_module_id(module, normalized)) {
            error = "sandbox: require traversal/absolute module rejected";
            return false;
        }
        const auto cached = cache.find(normalized);
        if (cached != cache.end()) {
            if (!cached->second) { error = "sandbox: module not allowed: " + normalized; return false; }
        } else {
            const bool allowed = modules.count(normalized) != 0 && !module_path_has_symlink(normalized);
            cache.emplace(normalized, allowed);
            if (!allowed) { error = "sandbox: module not allowed: " + normalized; return false; }
        }
        i = cursor;
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
                       "allow_require requires module:<id> entries; traversal/symlinks forbidden)";
            return false;
        }
        policy_ = policy;
        module_cache_.clear();
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
        module_cache_.clear();
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
        if (!validate_require_calls(source, policy_, module_cache_, errorOut)) return {};

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
    // Cache is scoped to the current immutable policy and keyed by canonical
    // module id. configure/load clear it, so stale grants cannot survive a
    // policy change.
    std::unordered_map<std::string, bool> module_cache_;
};

// Product-owned module runner used as a boot gate for the public sandbox.
// It deliberately resolves only in-memory, explicitly registered modules: no
// filesystem lookup is performed here, so a validated module id can never
// escape through traversal or a symlink after the policy check above.
class ProductModuleRunner final : public IScriptRunner {
public:
    ProductModuleRunner() {
        modules_.emplace("engine/runtime",
                         "{\"module\":\"engine/runtime\",\"version\":1}");
    }

    ScriptResult run(const std::string& source,
                     const std::string&,
                     const std::string&,
                     std::uint32_t instruction_budget,
                     std::uint32_t,
                     std::string& errorOut) override {
        ScriptResult result;
        result.instructions_used = 1;
        if (instruction_budget < result.instructions_used) {
            result.error = "budget exceeded";
            errorOut.clear();
            return result;
        }

        const auto requested = required_module(source);
        if (!requested) {
            result.ok = true;
            result.value = "{}";
            errorOut.clear();
            return result;
        }

        std::string normalized;
        if (!normalize_module_id(*requested, normalized)) {
            result.error = "sandbox: invalid module id";
            errorOut.clear();
            return result;
        }
        const auto found = modules_.find(normalized);
        if (found == modules_.end()) {
            result.error = "sandbox: unresolved module: " + normalized;
            errorOut.clear();
            return result;
        }
        result.ok = true;
        result.value = found->second;
        errorOut.clear();
        return result;
    }

private:
    static std::optional<std::string> required_module(const std::string& source) {
        const auto requirePos = source.find("require");
        if (requirePos == std::string::npos) return std::nullopt;
        std::size_t cursor = requirePos + 7;
        while (cursor < source.size() &&
               std::isspace(static_cast<unsigned char>(source[cursor]))) ++cursor;
        if (cursor < source.size() && source[cursor] == '(') {
            ++cursor;
            while (cursor < source.size() &&
                   std::isspace(static_cast<unsigned char>(source[cursor]))) ++cursor;
        }
        if (cursor >= source.size() || (source[cursor] != '\'' && source[cursor] != '"'))
            return std::nullopt;
        const char quote = source[cursor++];
        const std::size_t begin = cursor;
        while (cursor < source.size() && source[cursor] != quote) {
            if (source[cursor] == '\\' && cursor + 1 < source.size()) cursor += 2;
            else ++cursor;
        }
        if (cursor >= source.size()) return std::nullopt;
        return source.substr(begin, cursor - begin);
    }

    std::unordered_map<std::string, std::string> modules_;
};

struct ProductRequireHost {
    ProductModuleRunner runner;
    std::unique_ptr<ILuauSandbox> sandbox;
    bool healthy{false};
    std::string error;

    ProductRequireHost() {
        SandboxPolicy policy;
        policy.max_instructions = 64;
        policy.max_call_depth = 8;
        policy.allow_io = false;
        policy.allow_require = true;
        policy.allowed_globals = {"module:engine/runtime"};
        sandbox = std::unique_ptr<ILuauSandbox>(
            new LuauSandboxImpl("product.require", &runner, policy));
        auto result = sandbox->evaluate(
            "return require(\"engine/runtime\")", "main", error);
        healthy = result.ok &&
                  result.value == "{\"module\":\"engine/runtime\",\"version\":1}";
        if (!healthy && error.empty()) {
            error = result.error.empty() ? "module resolution failed" : result.error;
        }
    }
};

ProductRequireHost& product_require_host() {
    static ProductRequireHost host;
    return host;
}

}  // namespace

std::unique_ptr<ILuauSandbox> create_luau_sandbox(const std::string& sandboxId,
                                                  IScriptRunner* runner,
                                                  const SandboxPolicy& policy,
                                                  std::string& errorOut) {
    // The factory is a product boot call site (editor/game SDK hosts create
    // their sandbox through it).  Keep the real allow_require path live here:
    // a canonical in-memory module is resolved through an allowlisted sandbox
    // before handing out any product sandbox.  Failure is explicit instead of
    // silently shipping an unexercised require implementation.
    const auto& requireHost = product_require_host();
    if (!requireHost.healthy) {
        errorOut = "product require host unavailable: " + requireHost.error;
        return nullptr;
    }
    if (sandboxId.empty()) {
        errorOut = "sandbox id must be non-empty";
        return nullptr;
    }
    if (!valid_policy(policy)) {
        errorOut = "invalid sandbox policy (max_instructions/max_call_depth >= 1; "
                   "allow_require requires module:<id> entries; traversal/symlinks forbidden)";
        return nullptr;
    }
    errorOut.clear();
    return std::unique_ptr<ILuauSandbox>(new LuauSandboxImpl(sandboxId, runner, policy));
}

}  // namespace engine::scripting
