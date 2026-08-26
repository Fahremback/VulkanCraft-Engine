// AssetPipeline.cpp — adapter do contrato IAssetPipeline
// (engine::assets, §6 item 1 — "pipeline público import→validate→cook→cache→
// package com operações incrementais e determinísticas").
//
// Implementação determinística: fontes em mapa ordenado por nome; kinds
// embutidos self-contained (`raw` passthrough, `json` canônico compacto com
// chaves ordenadas, `text` com EOL normalizado); cache content-addressado
// por hash FNV-1a da (fonte, versão) — re-cook da mesma fonte+versão é cache
// hit sem recomputar; package ordenado por nome; JSON bit-exact
// all-or-nothing (load só comita se TODOS os registros forem válidos).
// Self-contained (std only) — mesma convenção dos demais adapters do sdk.

#include "engine/assets/IAssetPipeline.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <vector>

namespace engine::assets {

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

// FNV-1a 64 — hash determinístico usado para fontes e artefatos.
std::uint64_t fnv1a(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t h = 1469598103934665603ULL;
    for (const std::uint8_t b : bytes) {
        h ^= b;
        h *= 1099511628211ULL;
    }
    return h;
}

std::uint64_t fnv1a_str(const std::string& s) {
    std::vector<std::uint8_t> v(s.begin(), s.end());
    return fnv1a(v);
}

// ---- Validação e cook por kind ----

bool bytes_as_text(const std::vector<std::uint8_t>& bytes, std::string& out) {
    out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
}

// JSON canônico compacto com chaves ordenadas. Retorna false se malformado.
bool json_canonical(const std::string& in, std::string& out, std::string& err) {
    std::size_t i = 0;
    // parse_value: retorna true e escreve o canônico em out.
    // Implementa um parser recursivo mínimo (objetos/arrays/strings/números/
    // true/false/null) suficiente para validação e re-emissão determinística.
    struct Parser {
        const std::string& s;
        std::size_t i;
        std::string err;
        explicit Parser(const std::string& str) : s(str), i(0) {}

        bool value(std::string& out) {
            if (!skip_ws(s, i)) { err = "unexpected end"; return false; }
            const char c = s[i];
            if (c == '{') return object(out);
            if (c == '[') return array(out);
            if (c == '"') return str(out);
            if (c == 't') return literal("true", out);
            if (c == 'f') return literal("false", out);
            if (c == 'n') return literal("null", out);
            return number(out);
        }

        bool literal(const char* lit, std::string& out) {
            const std::size_t n = std::string(lit).size();
            if (s.compare(i, n, lit) != 0) { err = "bad literal"; return false; }
            i += n;
            out += lit;
            return true;
        }

        bool raw_string(std::string& raw) {
            ++i;  // abre aspas
            raw.clear();
            while (i < s.size() && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < s.size()) {
                    const char e = s[i + 1];
                    switch (e) {
                    case '"': raw += '"'; break;
                    case '\\': raw += '\\'; break;
                    case '/': raw += '/'; break;
                    case 'n': raw += '\n'; break;
                    case 'r': raw += '\r'; break;
                    case 't': raw += '\t'; break;
                    case 'u':
                        if (i + 5 >= s.size()) { err = "bad \\u"; return false; }
                        raw += static_cast<char>(std::strtol(s.substr(i + 2, 4).c_str(), nullptr, 16) & 0xFF);
                        i += 4;
                        break;
                    default: err = "bad escape"; return false;
                    }
                    i += 2;
                } else {
                    raw += s[i++];
                }
            }
            if (i >= s.size()) { err = "unterminated string"; return false; }
            ++i;  // fecha aspas
            return true;
        }

        bool str(std::string& out) {
            std::string raw;
            if (!raw_string(raw)) return false;
            out += '"';
            out += json_escape(raw);
            out += '"';
            return true;
        }

        bool number(std::string& out) {
            const std::size_t start = i;
            if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
            bool digits = false;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') { digits = true; ++i; }
            if (i < s.size() && s[i] == '.') {
                ++i;
                while (i < s.size() && s[i] >= '0' && s[i] <= '9') { digits = true; ++i; }
            }
            if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
                ++i;
                if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
                while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
            }
            if (!digits) { err = "bad number"; return false; }
            out.append(s, start, i - start);
            return true;
        }

        bool array(std::string& out) {
            ++i;  // '['
            out += '[';
            bool first = true;
            for (;;) {
                if (!skip_ws(s, i)) { err = "unterminated array"; return false; }
                if (s[i] == ']') { ++i; break; }
                if (!first) {
                    if (s[i] != ',') { err = "expected ','"; return false; }
                    ++i;
                    out += ',';
                }
                first = false;
                std::string item;
                if (!value(item)) return false;
                out += item;
            }
            out += ']';
            return true;
        }

        bool object(std::string& out) {
            ++i;  // '{'
            out += '{';
            std::map<std::string, std::string> members;  // chave → valor canônico
            bool first = true;
            for (;;) {
                if (!skip_ws(s, i)) { err = "unterminated object"; return false; }
                if (s[i] == '}') { ++i; break; }
                if (!first) {
                    if (s[i] != ',') { err = "expected ','"; return false; }
                    ++i;
                }
                first = false;
                if (!skip_ws(s, i) || s[i] != '"') { err = "expected string key"; return false; }
                std::string key_raw;
                if (!raw_string(key_raw)) return false;
                if (!skip_ws(s, i) || s[i] != ':') { err = "expected ':'"; return false; }
                ++i;
                std::string val;
                if (!value(val)) return false;
                members[key_raw] = val;  // duplicatas: última vence (determinístico)
            }
            bool first_member = true;
            for (const auto& kv : members) {
                if (!first_member) out += ',';
                first_member = false;
                out += '"';
                out += json_escape(kv.first);
                out += "\":";
                out += kv.second;
            }
            out += '}';
            return true;
        }
    };

    Parser p(in);
    if (!p.value(out)) {
        err = p.err.empty() ? "malformed json" : p.err;
        return false;
    }
    // Garante que não sobrou lixo após o valor raiz.
    if (skip_ws(in, p.i) && p.i < in.size()) {
        err = "trailing content";
        return false;
    }
    return true;
}

}  // namespace

namespace {

struct CachedArtifact {
    std::vector<std::uint8_t> artifact;
    std::uint64_t artifact_hash{ 0 };
};

class AssetPipelineImpl final : public IAssetPipeline {
public:
    explicit AssetPipelineImpl(const std::string& seed) : seed_(seed) {}

    bool import_source(const AssetSource& source, std::string& errorOut) override {
        if (source.name.empty()) { errorOut = "empty name"; return false; }
        if (!known_kind(source.kind)) { errorOut = "unknown kind: " + source.kind; return false; }
        std::string text;
        if (!kind_bytes_valid(source.kind, source.bytes, text, errorOut)) return false;

        const std::uint64_t src_hash = fnv1a(source.bytes);
        const bool same_source = sources_.count(source.name) &&
                                 sources_[source.name].version == source.version &&
                                 fnv1a(sources_[source.name].bytes) == src_hash;
        if (!same_source) {
            // Fonte ou versão mudou: invalida o artefato anterior.
            cooked_.erase(source.name);
        }
        sources_[source.name] = { source.kind, source.version, source.bytes, src_hash };
        return true;
    }

    AssetValidation validate(const std::string& name) const override {
        AssetValidation v;
        const auto it = sources_.find(name);
        if (it == sources_.end()) { v.error = "unknown asset: " + name; return v; }
        std::string text;
        if (!kind_bytes_valid(it->second.kind, it->second.bytes, text, v.error)) return v;
        v.valid = true;
        return v;
    }

    AssetCookResult cook(const std::string& name) override {
        AssetCookResult r;
        const auto it = sources_.find(name);
        if (it == sources_.end()) { r.error = "unknown asset: " + name; return r; }

        // Valida primeiro (all-or-nothing: inválida não muta).
        std::string text;
        if (!kind_bytes_valid(it->second.kind, it->second.bytes, text, r.error)) return r;

        // Cache content-addressado por (kind, versão, hash da fonte).
        const std::string cache_key = it->second.kind + "|" + it->second.version + "|" +
                                      std::to_string(it->second.source_hash);
        const auto cit = cache_.find(cache_key);
        if (cit != cache_.end()) {
            r.ok = true;
            r.artifact = cit->second.artifact;
            r.artifact_hash = cit->second.artifact_hash;
            r.cache_hit = true;
            ++cache_hits_;
            cooked_[name] = cit->second.artifact_hash;
            return r;
        }

        // Cook determinístico por kind.
        std::vector<std::uint8_t> artifact;
        if (it->second.kind == "json") {
            std::string canonical, jerr;
            if (!json_canonical(text, canonical, jerr)) {
                r.error = "json cook failed: " + jerr;
                return r;
            }
            artifact.assign(canonical.begin(), canonical.end());
        } else if (it->second.kind == "text") {
            std::string normalized;
            for (std::size_t idx = 0; idx < text.size(); ++idx) {
                if (text[idx] == '\r') {
                    if (idx + 1 < text.size() && text[idx + 1] == '\n') continue;  // \r\n → \n
                    normalized += '\n';
                } else {
                    normalized += text[idx];
                }
            }
            artifact.assign(normalized.begin(), normalized.end());
        } else {  // raw
            artifact = it->second.bytes;
        }

        const std::uint64_t art_hash = fnv1a(artifact);
        cache_[cache_key] = { artifact, art_hash };
        r.ok = true;
        r.artifact = artifact;
        r.artifact_hash = art_hash;
        r.cache_hit = false;
        cooked_[name] = art_hash;
        return r;
    }

    void remove(const std::string& name) override {
        sources_.erase(name);
        cooked_.erase(name);
    }

    std::vector<AssetState> states() const override {
        std::vector<AssetState> out;
        out.reserve(sources_.size());
        for (const auto& kv : sources_) {
            AssetState s;
            s.name = kv.first;
            s.kind = kv.second.kind;
            s.version = kv.second.version;
            s.source_hash = kv.second.source_hash;
            s.validated = validate(kv.first).valid;
            const auto cit = cooked_.find(kv.first);
            s.cooked = cit != cooked_.end();
            if (s.cooked) s.artifact_hash = cit->second;
            out.push_back(s);
        }
        return out;
    }

    AssetManifest package() const override {
        AssetManifest m;
        m.assets = states();
        std::sort(m.assets.begin(), m.assets.end(),
                  [](const AssetState& a, const AssetState& b) { return a.name < b.name; });
        return m;
    }

    std::size_t cache_hits() const override { return cache_hits_; }

    bool reset(std::string& errorOut) override {
        sources_.clear();
        cooked_.clear();
        cache_.clear();
        cache_hits_ = 0;
        errorOut.clear();
        return true;
    }

    bool load_from_json(const std::string& json, std::string& errorOut) override {
        // Documento: {"seed": "...", "assets": [ {name, kind, version, bytes_b64},
        // ...], "cooked": {name: hash}, "cache": [{key, hash}], "cache_hits": N}
        // All-or-nothing: qualquer campo inválido rejeita o documento inteiro.
        AssetPipelineImpl fresh(seed_);
        std::string text;
        std::size_t i = 0;
        if (!skip_ws(json, i) || json[i] != '{') { errorOut = "not an object"; return false; }
        ++i;
        std::set<std::string> seen;
        for (;;) {
            if (!skip_ws(json, i)) { errorOut = "unterminated"; return false; }
            if (json[i] == '}') { ++i; break; }
            if (!seen.empty() && (json[i] != ',' || !skip_ws(json, ++i))) { errorOut = "bad comma"; return false; }
            if (json[i] != '"') { errorOut = "expected key"; return false; }
            // lê key simples (sem escape) — chaves internas são fixas.
            const std::size_t ks = ++i;
            while (i < json.size() && json[i] != '"') ++i;
            if (i >= json.size()) { errorOut = "unterminated key"; return false; }
            const std::string key = json.substr(ks, i - ks);
            ++i;
            if (!skip_ws(json, i) || json[i] != ':') { errorOut = "expected ':'"; return false; }
            ++i;
            if (key == "seed") {
                if (!read_string(json, i, text) || text != seed_) { errorOut = "seed mismatch"; return false; }
            } else if (key == "assets") {
                if (!read_assets(json, i, fresh)) { errorOut = "bad assets"; return false; }
            } else if (key == "cooked") {
                if (!read_cooked(json, i, fresh)) { errorOut = "bad cooked"; return false; }
            } else if (key == "cache") {
                if (!read_cache(json, i, fresh)) { errorOut = "bad cache"; return false; }
            } else if (key == "cache_hits") {
                if (!read_size(json, i, fresh.cache_hits_)) { errorOut = "bad cache_hits"; return false; }
            } else {
                errorOut = "unknown field: " + key;
                return false;
            }
            seen.insert(key);
        }
        if (!seen.count("seed")) { errorOut = "missing seed"; return false; }
        // Rejeita conteúdo após o fechamento do objeto raiz (all-or-nothing).
        if (skip_ws(json, i) && i < json.size()) { errorOut = "trailing content"; return false; }
        // Só comita no final (all-or-nothing).
        sources_ = std::move(fresh.sources_);
        cooked_ = std::move(fresh.cooked_);
        cache_ = std::move(fresh.cache_);
        cache_hits_ = fresh.cache_hits_;
        return true;
    }

    std::string serialize_state() const override {
        std::string out = "{\"seed\":\"";
        out += json_escape(seed_);
        out += "\",\"assets\":[";
        bool first = true;
        for (const auto& kv : sources_) {
            if (!first) out += ',';
            first = false;
            out += "{\"name\":\"" + json_escape(kv.first) +
                   "\",\"kind\":\"" + json_escape(kv.second.kind) +
                   "\",\"version\":\"" + json_escape(kv.second.version) +
                   "\",\"bytes\":\"";
            // bytes em base64 (subset seguro) — determinístico.
            out += to_base64(kv.second.bytes);
            out += "\",\"source_hash\":" + std::to_string(kv.second.source_hash) + "}";
        }
        out += "],\"cooked\":{";
        first = true;
        for (const auto& kv : cooked_) {
            if (!first) out += ',';
            first = false;
            out += "\"" + json_escape(kv.first) + "\":" + std::to_string(kv.second);
        }
        out += "},\"cache\":{";
        first = true;
        for (const auto& kv : cache_) {
            if (!first) out += ',';
            first = false;
            out += "\"" + json_escape(kv.first) + "\":\"" + to_base64(kv.second.artifact) + "\"";
        }
        out += "},\"cache_hits\":" + std::to_string(cache_hits_) + "}";
        return out;
    }

private:
    struct Source {
        std::string kind;
        std::string version;
        std::vector<std::uint8_t> bytes;
        std::uint64_t source_hash{ 0 };
    };

    static bool known_kind(const std::string& kind) {
        return kind == "raw" || kind == "json" || kind == "text";
    }

    static bool kind_bytes_valid(const std::string& kind,
                                 const std::vector<std::uint8_t>& bytes,
                                 std::string& textOut, std::string& errorOut) {
        bytes_as_text(bytes, textOut);
        if (kind == "json") {
            std::string canonical, jerr;
            if (!json_canonical(textOut, canonical, jerr)) {
                errorOut = "invalid json: " + jerr;
                return false;
            }
        } else if (kind == "text") {
            // Rejeita NUL e sequências UTF-8 malformadas simples (bytes altos
            // sem continuação válida) — validação honesta de texto.
            for (std::size_t idx = 0; idx < bytes.size(); ++idx) {
                const std::uint8_t b = bytes[idx];
                if (b == 0) { errorOut = "text contains NUL"; return false; }
                if (b >= 0x80) {
                    std::size_t cont = 0;
                    if (b >= 0xC0 && b < 0xE0) cont = 1;
                    else if (b >= 0xE0 && b < 0xF0) cont = 2;
                    else if (b >= 0xF0 && b < 0xF8) cont = 3;
                    else { errorOut = "invalid utf-8 lead byte"; return false; }
                    for (std::size_t c = 0; c < cont; ++c) {
                        if (idx + 1 + c >= bytes.size() ||
                            (bytes[idx + 1 + c] & 0xC0) != 0x80) {
                            errorOut = "invalid utf-8 continuation";
                            return false;
                        }
                    }
                    idx += cont;
                }
            }
        }
        return true;
    }

    static std::string to_base64(const std::vector<std::uint8_t>& bytes) {
        static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve(((bytes.size() + 2) / 3) * 4);
        for (std::size_t i = 0; i < bytes.size(); i += 3) {
            const std::uint32_t n = (std::uint32_t(bytes[i]) << 16) |
                                    (i + 1 < bytes.size() ? std::uint32_t(bytes[i + 1]) << 8 : 0) |
                                    (i + 2 < bytes.size() ? std::uint32_t(bytes[i + 2]) : 0);
            out += T[(n >> 18) & 63];
            out += T[(n >> 12) & 63];
            out += (i + 1 < bytes.size()) ? T[(n >> 6) & 63] : '=';
            out += (i + 2 < bytes.size()) ? T[n & 63] : '=';
        }
        return out;
    }

    static bool from_base64(const std::string& in, std::vector<std::uint8_t>& out) {
        static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        if (in.size() % 4 != 0) return false;
        out.clear();
        for (std::size_t i = 0; i < in.size(); i += 4) {
            std::uint32_t n = 0;
            for (std::size_t j = 0; j < 4; ++j) {
                const char c = in[i + j];
                if (c == '=') { n <<= 6; continue; }
                const char* p = std::strchr(T, c);
                if (!p) return false;
                n = (n << 6) | static_cast<std::uint32_t>(p - T);
            }
            out.push_back(static_cast<std::uint8_t>((n >> 16) & 0xFF));
            if (i + 2 < in.size() && in[i + 2] != '=') out.push_back(static_cast<std::uint8_t>((n >> 8) & 0xFF));
            if (i + 3 < in.size() && in[i + 3] != '=') out.push_back(static_cast<std::uint8_t>(n & 0xFF));
        }
        return true;
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

    static bool read_size(const std::string& s, std::size_t& i, std::size_t& out) {
        if (!skip_ws(s, i)) return false;
        if (s[i] == '"') {  // aceita string numérica por robustez
            ++i;
            out = 0;
            bool any = false;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') { out = out * 10 + (s[i] - '0'); any = true; ++i; }
            if (!any || i >= s.size() || s[i] != '"') return false;
            ++i;
            return true;
        }
        out = 0;
        bool any = false;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') { out = out * 10 + (s[i] - '0'); any = true; ++i; }
        return any;
    }

    bool read_assets(const std::string& s, std::size_t& i, AssetPipelineImpl& fresh) {
        if (!skip_ws(s, i) || s[i] != '[') return false;
        ++i;
        bool first = true;
        for (;;) {
            if (!skip_ws(s, i)) return false;
            if (s[i] == ']') { ++i; return true; }
            if (!first) {
                if (s[i] != ',') return false;
                ++i;
            }
            first = false;
            if (!skip_ws(s, i) || s[i] != '{') return false;
            ++i;
            AssetSource src;
            std::string bytes_b64;
            std::set<std::string> fseen;
            for (;;) {
                if (!skip_ws(s, i)) return false;
                if (s[i] == '}') { ++i; break; }
                if (!fseen.empty() && (s[i] != ',' || !skip_ws(s, ++i))) return false;
                if (s[i] != '"') return false;
                const std::size_t ks = ++i;
                while (i < s.size() && s[i] != '"') ++i;
                if (i >= s.size()) return false;
                const std::string fk = s.substr(ks, i - ks);
                ++i;
                if (!skip_ws(s, i) || s[i] != ':') return false;
                ++i;
                if (fk == "name") { if (!read_string(s, i, src.name)) return false; }
                else if (fk == "kind") { if (!read_string(s, i, src.kind)) return false; }
                else if (fk == "version") { if (!read_string(s, i, src.version)) return false; }
                else if (fk == "bytes") { if (!read_string(s, i, bytes_b64)) return false; }
                else if (fk == "source_hash") {  // derivado; só exige número válido
                    std::size_t v;
                    if (!read_size(s, i, v)) return false;
                }
                else return false;
                fseen.insert(fk);
            }
            if (src.name.empty()) return false;
            if (!from_base64(bytes_b64, src.bytes)) return false;
            std::string err;
            if (!fresh.import_source(src, err)) return false;
            // O hash da fonte é derivado (recomputado no import) — o campo
            // serializado é informacional e validado como número acima;
            // consistência do cache vem do re-import.
        }
    }

    bool read_cooked(const std::string& s, std::size_t& i, AssetPipelineImpl& fresh) {
        if (!skip_ws(s, i) || s[i] != '{') return false;
        ++i;
        bool first = true;
        for (;;) {
            if (!skip_ws(s, i)) return false;
            if (s[i] == '}') { ++i; return true; }
            if (!first) {
                if (s[i] != ',') return false;
                ++i;
            }
            first = false;
            std::string name;
            if (!read_string(s, i, name)) return false;
            if (!skip_ws(s, i) || s[i] != ':') return false;
            ++i;
            std::size_t hash;
            if (!read_size(s, i, hash)) return false;
            if (!fresh.sources_.count(name)) return false;  // cooked de asset ausente
            fresh.cooked_[name] = hash;
        }
    }

    bool read_cache(const std::string& s, std::size_t& i, AssetPipelineImpl& fresh) {
        if (!skip_ws(s, i) || s[i] != '{') return false;
        ++i;
        bool first = true;
        for (;;) {
            if (!skip_ws(s, i)) return false;
            if (s[i] == '}') { ++i; return true; }
            if (!first) {
                if (s[i] != ',') return false;
                ++i;
            }
            first = false;
            std::string key;
            if (!read_string(s, i, key)) return false;
            if (!skip_ws(s, i) || s[i] != ':') return false;
            ++i;
            std::string b64;
            if (!read_string(s, i, b64)) return false;
            CachedArtifact ca;
            if (!from_base64(b64, ca.artifact)) return false;
            ca.artifact_hash = fnv1a(ca.artifact);
            fresh.cache_[key] = ca;
        }
    }

    std::string seed_;
    std::map<std::string, Source> sources_;
    std::map<std::string, std::uint64_t> cooked_;
    std::map<std::string, CachedArtifact> cache_;
    std::size_t cache_hits_{ 0 };
};

}  // namespace

std::unique_ptr<IAssetPipeline> create_asset_pipeline(const std::string& seed,
                                                      std::string& errorOut) {
    if (seed.empty()) {
        errorOut = "seed must be non-empty";
        return nullptr;
    }
    errorOut.clear();
    return std::unique_ptr<IAssetPipeline>(new AssetPipelineImpl(seed));
}

}  // namespace engine::assets
