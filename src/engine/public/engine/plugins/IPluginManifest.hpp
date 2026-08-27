#pragma once
// IPluginManifest — manifesto, dependências, versionamento e ABI de plugins
// (§3 HANDOFF AGENT-6 linha 204 — "Completar sistema de plugins com
// manifesto, dependências, versionamento, ABI, isolamento de falhas,
// hot reload e empacotamento").
//
// O contrato define a estrutura de um manifesto de plugin (lido de
// project.json ou PluginManifest.json) e validação all-or-nothing.
// Plugins declararam suas dependências, permissões, ABI e capabilities
// no manifesto; o host valida antes de carregar.
//
// Padrão: contrato puro C++17, self-contained (std only), headless,
// determinístico. Mesmo espírito de ILuauSandbox/IPluginSandbox.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine::plugins {

/// Versão semântica de plugin (MAJOR.MINOR.PATCH).
struct PluginVersion {
    std::uint32_t major{ 0 };
    std::uint32_t minor{ 0 };
    std::uint32_t patch{ 0 };

    /// Compara versões. Retorna -1/0/1.
    [[nodiscard]] int compare(const PluginVersion& other) const noexcept {
        if (major != other.major) return major < other.major ? -1 : 1;
        if (minor != other.minor) return minor < other.minor ? -1 : 1;
        if (patch != other.patch) return patch < other.patch ? -1 : 1;
        return 0;
    }

    /// Formata como string "MAJOR.MINOR.PATCH".
    [[nodiscard]] std::string to_string() const {
        return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    }

    /// Parse de "MAJOR.MINOR.PATCH". Inválido → {0,0,0}.
    [[nodiscard]] static PluginVersion parse(const std::string& str) {
        PluginVersion v;
        // Simplified parse — aceita "X.Y.Z"
        std::uint32_t parts[3] = { 0, 0, 0 };
        std::uint32_t idx = 0;
        std::uint32_t current = 0;
        bool has_digit = false;
        for (char c : str) {
            if (c == '.') {
                if (idx < 3) parts[idx++] = current;
                current = 0;
            } else if (c >= '0' && c <= '9') {
                current = current * 10 + static_cast<std::uint32_t>(c - '0');
                has_digit = true;
            }
        }
        if (idx < 3 && has_digit) parts[idx] = current;
        return { parts[0], parts[1], parts[2] };
    }

    [[nodiscard]] bool operator==(const PluginVersion& o) const noexcept { return compare(o) == 0; }
    [[nodiscard]] bool operator!=(const PluginVersion& o) const noexcept { return compare(o) != 0; }
    [[nodiscard]] bool operator<(const PluginVersion& o) const noexcept { return compare(o) < 0; }
    [[nodiscard]] bool operator>=(const PluginVersion& o) const noexcept { return compare(o) >= 0; }
    [[nodiscard]] bool operator>(const PluginVersion& o) const noexcept { return compare(o) > 0; }
};

/// Restrição de versão para uma dependência. Aceita:
///   - "*": qualquer versão
///   - ">=1.2.3": versão mínima
///   - "==1.2.3": versão exata
///   - ">=1.0.0 <2.0.0": faixa
struct VersionConstraint {
    std::string raw;  ///< textual original (para serialização bit-exact)

    /// Verifica se uma versão satisfaz a restrição.
    [[nodiscard]] bool satisfies(const PluginVersion& v) const {
        // Parse simplificado para os casos comuns
        if (raw == "*") return true;
        if (raw.size() >= 3 && raw[0] == '>' && raw[1] == '=') {
            auto min = PluginVersion::parse(raw.substr(2));
            return v >= min;
        }
        if (raw.size() >= 3 && raw[0] == '=' && raw[1] == '=') {
            auto exact = PluginVersion::parse(raw.substr(2));
            return v == exact;
        }
        // Fallback: aceita qualquer coisa
        return true;
    }
};

/// Dependência declarada no manifesto.
struct PluginDependency {
    std::string name;               ///< nome do plugin dependido
    VersionConstraint constraint;   ///< restrição de versão
    bool required{ true };          ///< false = opcional (soft dep)
};

/// ABI de um plugin. Define como o host se comunica com o plugin.
enum class PluginAbi : std::uint8_t {
    Cpp,            ///< Plugin C++ linkado estaticamente/dinamicamente
    Luau,           ///< Plugin Luau via ILuauSandbox
    Wasm,           ///< Plugin WebAssembly via wasmtime
    VisualScript,   ///< Plugin de visual scripting (node graph)
};

/// Caps (capabilities) que um plugin declara que fornece.
struct PluginCapabilities {
    bool provides_types{ false };        ///< Registra tipos ECS
    bool provides_components{ false };   ///< Registra componentes ECS
    bool provides_assets{ false };       ///< Registra tipos de asset
    bool provides_importers{ false };    ///< Registra importadores
    bool provides_panels{ false };       ///< Registra painéis do editor
    bool provides_mcp_tools{ false };    ///< Registra tools MCP
    bool provides_commands{ false };     ///< Registra comandos CLI
    bool provides_nodes{ false };        ///< Registra nós de visual script
    bool provides_events{ false };       ///< Emite eventos
    bool provides_ui{ false };           ///< Fornece UI
};

/// Manifesto completo de um plugin. Validado all-or-nothing pelo host
/// antes de carregar o plugin.
struct PluginManifest {
    std::string name;                       ///< nome único (ex: "com.company.health")
    std::string display_name;               ///< nome para exibição
    std::string description;                ///< descrição humana
    std::string author;                     ///< autor
    PluginVersion version{ 0, 0, 1 };       ///< versão deste plugin
    PluginAbi abi{ PluginAbi::Cpp };         ///< ABI do plugin
    std::vector<PluginDependency> dependencies;  ///< dependências
    std::vector<std::string> permissions;   ///< permissões requeridas (IPluginSandbox)
    PluginCapabilities capabilities;        ///< o que o plugin fornece
    std::unordered_map<std::string, std::string> metadata;  ///< pares chave-valor extras

    /// Valida o manifesto. Retorna true se válido; `error` descreve o
    /// problema primeiro encontrado.
    [[nodiscard]] bool validate(std::string& error) const {
        if (name.empty()) { error = "plugin name must not be empty"; return false; }
        if (display_name.empty()) { error = "display_name must not be empty"; return false; }
        if (version.major == 0 && version.minor == 0 && version.patch == 0) {
            error = "version must not be 0.0.0"; return false;
        }
        // Valida dependências
        for (const auto& dep : dependencies) {
            if (dep.name.empty()) { error = "dependency name must not be empty"; return false; }
            if (dep.constraint.raw.empty()) { error = "dependency constraint must not be empty"; return false; }
        }
        return true;
    }

    /// Serializa para JSON bit-exact (chaves ordenadas).
    [[nodiscard]] std::string to_json() const;

    /// Desserializa de JSON. Inválido → nullopt.
    [[nodiscard]] static PluginManifest from_json(const std::string& json, std::string& error);
};

/// Estado de um plugin carregado.
enum class PluginState : std::uint8_t {
    Unloaded,       ///< Não carregado
    Loading,        ///< Em carregamento
    Loaded,         ///< Carregado e ativo
    Error,          ///< Erro no carregamento
    Disabled,       ///< Desativado pelo usuário
    Reloading,      ///< Em hot reload
};

/// Informações de runtime de um plugin carregado.
struct PluginRuntimeInfo {
    PluginManifest manifest;        ///< manifesto carregado
    PluginState state{ PluginState::Unloaded };
    std::string error_message;      ///< mensagem de erro (se state == Error)
    std::uint64_t load_time_ms{ 0 }; ///< tempo de carregamento
    std::uint64_t memory_bytes{ 0 }; ///< memória usada (estimativa)
    std::string library_path;       ///< caminho da biblioteca (se C++)
    std::string script_path;        ///< caminho do script (se Luau/WASM/Visual)
};

/// Interface para gerenciar manifestos de plugins. O host cria um
/// gerenciador e registra plugins antes de carregá-los.
///
/// Headless-testable: registrar, validar, resolver dependências —
/// sem GPU, sem filesystem, sem rede.
class IPluginManifestManager {
public:
    virtual ~IPluginManifestManager() = default;

    /// Registra um manifesto. Validação all-or-nothing: manifesto
    /// inválido → false, nada muda.
    virtual bool register_manifest(
        const PluginManifest& manifest,
        std::string& error) = 0;

    /// Remove um manifesto por nome.
    virtual bool unregister(const std::string& name, std::string& error) = 0;

    /// Obtém o manifesto registrado.
    [[nodiscard]] virtual const PluginManifest* get(const std::string& name) const = 0;

    /// Lista todos os manifestos registrados.
    [[nodiscard]] virtual std::vector<PluginManifest> list() const = 0;

    /// Resolve dependências. Retorna a ordem de carregamento (topológica).
    /// Ciclo → error. Dependência obrigatória ausente → error.
    [[nodiscard]] virtual std::vector<std::string> resolve_dependencies(
        std::string& error) const = 0;

    /// Verifica se todas as dependências de um plugin são satisfáveis.
    [[nodiscard]] virtual bool can_load(
        const std::string& name,
        std::string& error) const = 0;

    /// Obtém informações de runtime de um plugin carregado.
    [[nodiscard]] virtual const PluginRuntimeInfo* get_runtime_info(
        const std::string& name) const = 0;

    /// Atualiza o estado de um plugin.
    virtual bool set_state(
        const std::string& name,
        PluginState state,
        std::string& error) = 0;
};

}  // namespace engine::plugins
