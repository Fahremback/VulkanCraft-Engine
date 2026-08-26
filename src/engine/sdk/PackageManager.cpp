// PackageManager.cpp — adapter do contrato IPackageManager
// (engine::packaging, §6 item 5 — "Implementar packages/mods assináveis,
// manifestos, resolução de dependências e atualizações seguras").
//
// Implementação determinística: manifestos em mapa ordenado por nome;
// restrições de versão opacas (`*`, `==X`, `>=X`, versão nua = exata) com
// comparação por segmentos numéricos; resolução em ordem topológica com
// ciclo/restrição violada/versão ausente recusados; verificação de assinatura
// plugável via ISignatureVerifier; gate de instalação all-or-nothing
// (resolve ok + assinatura válida → comita; senão nada muda); JSON bit-exact
// all-or-nothing (load só comita se TUDO válido). Self-contained (std only).

#include "engine/packaging/IPackageManager.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <vector>

namespace engine::packaging {

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

// Comparação de versões por segmentos numéricos ("1.2.10" > "1.2.9").
// Não-numérico é tratado como 0. Retorna: -1, 0, +1.
int compare_versions(const std::string& a, const std::string& b) {
    std::size_t ia = 0, ib = 0;
    for (;;) {
        // Extrai o próximo segmento numérico de cada lado.
        std::uint64_t na = 0, nb = 0;
        bool da = false, db = false;
        while (ia < a.size() && a[ia] != '.') {
            if (a[ia] >= '0' && a[ia] <= '9') { na = na * 10 + (a[ia] - '0'); da = true; }
            ++ia;
        }
        while (ib < b.size() && b[ib] != '.') {
            if (b[ib] >= '0' && b[ib] <= '9') { nb = nb * 10 + (b[ib] - '0'); db = true; }
            ++ib;
        }
        if (na != nb) return na < nb ? -1 : 1;
        if (!da && db) return -1;
        if (da && !db) return 1;
        const bool ea = ia >= a.size();
        const bool eb = ib >= b.size();
        if (ea && eb) return 0;
        if (ea) return -1;
        if (eb) return 1;
        ++ia;
        ++ib;
    }
}

// Satisfaz a restrição? `*` = qualquer; `==X` = exata; `>=X` = maior/igual;
// versão nua = exata (mesmo que ==).
bool satisfies(const std::string& version, const std::string& constraint) {
    if (constraint == "*") return true;
    std::string op, ver;
    if (constraint.size() >= 2 && constraint[0] == '=' && constraint[1] == '=') {
        op = "==";
        ver = constraint.substr(2);
    } else if (constraint.size() >= 2 && constraint[0] == '>' && constraint[1] == '=') {
        op = ">=";
        ver = constraint.substr(2);
    } else {
        op = "==";
        ver = constraint;
    }
    if (ver.empty()) return false;
    const int cmp = compare_versions(version, ver);
    if (op == "==") return cmp == 0;
    return cmp >= 0;
}

}  // namespace

namespace {

class PackageManagerImpl final : public IPackageManager {
public:
    explicit PackageManagerImpl(const std::string& sessionId) : session_id_(sessionId) {}

    const std::string& session_id() const override { return session_id_; }

    bool register_manifest(const PackageManifest& manifest,
                           std::string& errorOut) override {
        if (manifest.name.empty()) { errorOut = "empty name"; return false; }
        if (manifest.version.empty()) { errorOut = "empty version"; return false; }
        if (manifest.content_hash.empty()) { errorOut = "empty content_hash"; return false; }
        for (const auto& dep : manifest.dependencies) {
            if (dep.name.empty()) { errorOut = "empty dependency name"; return false; }
            if (dep.constraint.empty()) { errorOut = "empty dependency constraint"; return false; }
        }
        const auto it = manifests_.find(manifest.name);
        if (it != manifests_.end() && it->second.installed &&
            it->second.manifest.content_hash != manifest.content_hash) {
            errorOut = "installed package content changed; uninstall first";
            return false;
        }
        ManifestEntry entry;
        entry.manifest = manifest;
        entry.signature_valid = false;
        if (it != manifests_.end()) entry.installed = it->second.installed;
        manifests_[manifest.name] = entry;
        errorOut.clear();
        return true;
    }

    void set_verifier(ISignatureVerifier* verifier) override { verifier_ = verifier; }

    bool verify_signature(const std::string& name, const std::string& signature,
                          std::string& errorOut) override {
        const auto it = manifests_.find(name);
        if (it == manifests_.end()) { errorOut = "unknown package: " + name; return false; }
        if (verifier_ == nullptr) { errorOut = "no verifier installed"; return false; }
        const bool ok = verifier_->verify(name, it->second.manifest.content_hash, signature);
        it->second.signature_valid = ok;
        errorOut.clear();
        return ok;
    }

    ResolutionResult resolve(const std::string& name) const override {
        ResolutionResult r;
        const auto root = manifests_.find(name);
        if (root == manifests_.end()) { r.error = "unknown package: " + name; return r; }

        // Fechamento transitivo com detecção de ciclo (DFS determinística em
        // ordem de declaração).
        std::vector<std::string> order;      // resultado (deps primeiro)
        std::set<std::string> visited;       // visitados (para ciclo)
        std::set<std::string> done;          // concluídos (em order)
        std::string cycle_error;

        // DFS iterativa para evitar recursão profunda.
        struct Frame { const ManifestEntry* entry; std::size_t dep_index; };
        std::vector<Frame> stack;
        const auto push_entry = [&](const std::string& pkg_name) -> bool {
            const auto it = manifests_.find(pkg_name);
            if (it == manifests_.end()) {
                cycle_error = "missing dependency: " + pkg_name;
                return false;
            }
            if (done.count(pkg_name)) return true;  // já resolvido
            if (visited.count(pkg_name)) {
                cycle_error = "dependency cycle at: " + pkg_name;
                return false;
            }
            visited.insert(pkg_name);
            stack.push_back({ &it->second, 0 });
            return true;
        };

        if (!push_entry(name)) { r.error = cycle_error; return r; }

        while (!stack.empty()) {
            Frame& top = stack.back();
            const ManifestEntry& entry = *top.entry;
            if (top.dep_index >= entry.manifest.dependencies.size()) {
                // Todos os deps processados: conclui.
                const std::string pkg_name = entry.manifest.name;
                visited.erase(pkg_name);
                done.insert(pkg_name);
                order.push_back(pkg_name);
                stack.pop_back();
                continue;
            }
            const PackageDependency& dep = entry.manifest.dependencies[top.dep_index++];
            const auto dit = manifests_.find(dep.name);
            if (dit == manifests_.end()) {
                r.error = "missing dependency: " + dep.name;
                return r;
            }
            if (!satisfies(dit->second.manifest.version, dep.constraint)) {
                r.error = "constraint violated: " + dep.name + " " + dep.constraint +
                          " (have " + dit->second.manifest.version + ")";
                return r;
            }
            if (done.count(dep.name)) continue;
            if (visited.count(dep.name)) {
                r.error = "dependency cycle at: " + dep.name;
                return r;
            }
            visited.insert(dep.name);
            stack.push_back({ &dit->second, 0 });
        }

        r.ok = true;
        r.order = order;
        return r;
    }

    bool install(const std::string& name, const std::string& signature,
                 std::string& errorOut) override {
        const auto it = manifests_.find(name);
        if (it == manifests_.end()) { errorOut = "unknown package: " + name; return false; }
        if (it->second.installed) { errorOut.clear(); return true; }  // no-op
        if (verifier_ == nullptr) { errorOut = "no verifier — signature required"; return false; }
        if (!verifier_->verify(name, it->second.manifest.content_hash, signature)) {
            errorOut = "invalid signature";
            return false;
        }

        // Gate: resolve tudo antes de comitar.
        ResolutionResult res = resolve(name);
        if (!res.ok) { errorOut = res.error; return false; }

        // Instala em ordem topológica (deps primeiro). All-or-nothing: só
        // comita se TODAS as deps não-instaladas já tiverem sido verificadas
        // explicitamente (verify_signature prévia — cada dep tem o seu
        // content_hash e a sua própria assinatura; a signature do install
        // vale só para o ROOT). Sem verificação explícita da dep → recusa.
        std::vector<std::string> to_install;
        for (const std::string& pkg : res.order) {
            const auto pit = manifests_.find(pkg);
            if (pit->second.installed) continue;
            if (pkg != name && !pit->second.signature_valid) {
                errorOut = "dependency not verified: " + pkg +
                           " (call verify_signature first)";
                return false;
            }
            to_install.push_back(pkg);
        }

        for (const std::string& pkg : to_install) {
            auto& entry = manifests_[pkg];
            entry.installed = true;
            entry.signature_valid = true;
            entry.install_seq = ++install_seq_;
        }
        errorOut.clear();
        return true;
    }

    bool uninstall(const std::string& name, std::string& errorOut) override {
        const auto it = manifests_.find(name);
        if (it == manifests_.end()) { errorOut.clear(); return true; }  // no-op
        if (!it->second.installed) { errorOut.clear(); return true; }
        // Dependentes instalados → recusa.
        for (const auto& kv : manifests_) {
            if (!kv.second.installed || kv.first == name) continue;
            for (const auto& dep : kv.second.manifest.dependencies) {
                if (dep.name == name) {
                    errorOut = "still depended on by: " + kv.first;
                    return false;
                }
            }
        }
        it->second.installed = false;
        it->second.signature_valid = false;
        it->second.install_seq = 0;
        errorOut.clear();
        return true;
    }

    std::vector<PackageState> states() const override {
        std::vector<PackageState> out;
        out.reserve(manifests_.size());
        for (const auto& kv : manifests_) {
            PackageState s;
            s.name = kv.first;
            s.version = kv.second.manifest.version;
            s.dependencies = kv.second.manifest.dependencies;
            s.content_hash = kv.second.manifest.content_hash;
            s.installed = kv.second.installed;
            s.signature_valid = kv.second.signature_valid;
            s.install_seq = kv.second.install_seq;
            out.push_back(s);
        }
        return out;
    }

    std::vector<PackageState> installed() const override {
        std::vector<PackageState> out;
        for (const auto& kv : manifests_) {
            if (kv.second.installed) {
                PackageState s;
                s.name = kv.first;
                s.version = kv.second.manifest.version;
                s.dependencies = kv.second.manifest.dependencies;
                s.content_hash = kv.second.manifest.content_hash;
                s.installed = true;
                s.signature_valid = kv.second.signature_valid;
                s.install_seq = kv.second.install_seq;
                out.push_back(s);
            }
        }
        std::sort(out.begin(), out.end(),
                  [](const PackageState& a, const PackageState& b) {
                      return a.install_seq < b.install_seq;
                  });
        return out;
    }

    bool reset(std::string& errorOut) override {
        manifests_.clear();
        install_seq_ = 0;
        verifier_ = nullptr;
        errorOut.clear();
        return true;
    }

    bool load_from_json(const std::string& json, std::string& errorOut) override {
        // Documento: {"session": "...", "install_seq": N,
        //  "packages": [ {name, version, content_hash, installed,
        //                 signature_valid, install_seq,
        //                 deps: [ {name, constraint}, ... ]}, ... ] }
        PackageManagerImpl fresh(session_id_);
        std::string text;
        std::size_t i = 0;
        if (!skip_ws(json, i) || json[i] != '{') { errorOut = "not an object"; return false; }
        ++i;
        bool have_session = false;
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
            if (key == "session") {
                if (!read_string(json, i, text) || text != session_id_) {
                    errorOut = "session mismatch";
                    return false;
                }
                have_session = true;
            } else if (key == "install_seq") {
                if (!read_u64(json, i, fresh.install_seq_)) { errorOut = "bad install_seq"; return false; }
            } else if (key == "packages") {
                if (!read_packages(json, i, fresh)) { errorOut = "bad packages"; return false; }
            } else {
                errorOut = "unknown field: " + key;
                return false;
            }
        }
        if (!have_session) { errorOut = "missing session"; return false; }
        if (skip_ws(json, i) && i < json.size()) { errorOut = "trailing content"; return false; }
        // Só comita no final (all-or-nothing).
        manifests_ = std::move(fresh.manifests_);
        install_seq_ = fresh.install_seq_;
        return true;
    }

    std::string serialize_state() const override {
        std::string out = "{\"session\":\"";
        out += json_escape(session_id_);
        out += "\",\"install_seq\":" + std::to_string(install_seq_);
        out += ",\"packages\":[";
        bool first = true;
        for (const auto& kv : manifests_) {
            if (!first) out += ',';
            first = false;
            const auto& m = kv.second.manifest;
            out += "{\"name\":\"" + json_escape(kv.first) + "\"";
            out += ",\"version\":\"" + json_escape(m.version) + "\"";
            out += ",\"content_hash\":\"" + json_escape(m.content_hash) + "\"";
            out += ",\"installed\":" + std::string(kv.second.installed ? "true" : "false");
            out += ",\"signature_valid\":" + std::string(kv.second.signature_valid ? "true" : "false");
            out += ",\"install_seq\":" + std::to_string(kv.second.install_seq);
            out += ",\"deps\":[";
            bool dfirst = true;
            for (const auto& dep : m.dependencies) {
                if (!dfirst) out += ',';
                dfirst = false;
                out += "{\"name\":\"" + json_escape(dep.name) + "\"";
                out += ",\"constraint\":\"" + json_escape(dep.constraint) + "\"}";
            }
            out += "]}";
        }
        out += "]}";
        return out;
    }

private:
    struct ManifestEntry {
        PackageManifest manifest;
        bool installed{ false };
        bool signature_valid{ false };
        std::uint64_t install_seq{ 0 };
    };

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

    static bool read_bool(const std::string& s, std::size_t& i, bool& out) {
        if (!skip_ws(s, i)) return false;
        if (s.compare(i, 4, "true") == 0) { out = true; i += 4; return true; }
        if (s.compare(i, 5, "false") == 0) { out = false; i += 5; return true; }
        return false;
    }

    static bool read_key_value(const std::string& s, std::size_t& i,
                               std::string& key, std::string& value) {
        if (!skip_ws(s, i) || s[i] != '"') return false;
        const std::size_t ks = ++i;
        while (i < s.size() && s[i] != '"') ++i;
        if (i >= s.size()) return false;
        key = s.substr(ks, i - ks);
        ++i;
        if (!skip_ws(s, i) || s[i] != ':') return false;
        ++i;
        if (!skip_ws(s, i)) return false;
        if (s[i] == '"') return read_string(s, i, value);
        const std::size_t vs = i;
        while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']') ++i;
        value = s.substr(vs, i - vs);
        return true;
    }

    bool read_packages(const std::string& s, std::size_t& i, PackageManagerImpl& fresh) {
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
            PackageManifest m;
            ManifestEntry entry;
            bool first_member = true;
            for (;;) {
                if (!skip_ws(s, i)) return false;
                if (s[i] == '}') { ++i; break; }
                if (!first_member) {
                    if (s[i] != ',') return false;
                    ++i;
                }
                first_member = false;
                if (!skip_ws(s, i) || s[i] != '"') return false;
                const std::size_t kstart = ++i;
                while (i < s.size() && s[i] != '"') ++i;
                if (i >= s.size()) return false;
                const std::string k = s.substr(kstart, i - kstart);
                ++i;
                if (!skip_ws(s, i) || s[i] != ':') return false;
                ++i;
                if (k == "deps") {
                    if (!read_deps(s, i, m.dependencies)) return false;
                    continue;
                }
                std::string v;
                if (!read_value(s, i, v)) return false;
                std::uint64_t nv;
                if (k == "name") { m.name = v; }
                else if (k == "version") { m.version = v; }
                else if (k == "content_hash") { m.content_hash = v; }
                else if (k == "installed") {
                    if (v == "true") entry.installed = true;
                    else if (v == "false") entry.installed = false;
                    else return false;
                } else if (k == "signature_valid") {
                    if (v == "true") entry.signature_valid = true;
                    else if (v == "false") entry.signature_valid = false;
                    else return false;
                } else if (k == "install_seq") {
                    if (!read_u64_from(v, nv)) return false;
                    entry.install_seq = nv;
                } else {
                    return false;
                }
            }
            if (m.name.empty() || m.version.empty() || m.content_hash.empty()) return false;
            entry.manifest = m;
            if (fresh.manifests_.count(m.name)) return false;  // duplicata
            fresh.manifests_[m.name] = entry;
        }
    }

    static bool read_deps(const std::string& s, std::size_t& i,
                          std::vector<PackageDependency>& deps) {
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
            PackageDependency dep;
            bool first_member = true;
            for (;;) {
                if (!skip_ws(s, i)) return false;
                if (s[i] == '}') { ++i; break; }
                if (!first_member) {
                    if (s[i] != ',') return false;
                    ++i;
                }
                first_member = false;
                std::string k, v;
                if (!read_key_value(s, i, k, v)) return false;
                if (k == "name") dep.name = v;
                else if (k == "constraint") dep.constraint = v;
                else return false;
            }
            if (dep.name.empty() || dep.constraint.empty()) return false;
            deps.push_back(dep);
        }
    }

    static bool read_value(const std::string& s, std::size_t& i, std::string& out) {
        if (!skip_ws(s, i)) return false;
        if (s[i] == '"') return read_string(s, i, out);
        const std::size_t vs = i;
        while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']') ++i;
        out = s.substr(vs, i - vs);
        return !out.empty();
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

    std::string session_id_;
    std::map<std::string, ManifestEntry> manifests_;
    std::uint64_t install_seq_{ 0 };
    ISignatureVerifier* verifier_{ nullptr };
};

}  // namespace

std::unique_ptr<IPackageManager> create_package_manager(const std::string& sessionId,
                                                        std::string& errorOut) {
    if (sessionId.empty()) {
        errorOut = "session id must be non-empty";
        return nullptr;
    }
    errorOut.clear();
    return std::unique_ptr<IPackageManager>(new PackageManagerImpl(sessionId));
}

}  // namespace engine::packaging
