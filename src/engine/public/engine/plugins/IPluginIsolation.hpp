#pragma once
// IPluginIsolation — isolamento de falhas e hot reload de plugins
// (§3 HANDOFF AGENT-6 linha 204 — "Completar sistema de plugins com
// manifesto, dependências, versionamento, ABI, isolamento de falhas,
// hot reload e empacotamento").
//
// O contrato define como o host isola plugins com falhas e como
// hot reload é executado sem derrubar o runtime, editor ou servidor.
//
// Padrão: contrato puro C++17, self-contained (std only), headless,
// determinístico. Mesmo espírito de IPluginSandbox/ILuauSandbox.

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::plugins {

/// Severidade de uma falha de plugin.
enum class FailureSeverity : std::uint8_t {
    Warning,        ///< Falha não-crítica (log, mas plugin continua)
    Recoverable,    ///< Plugin pode ser reiniciado
    Critical,       ///< Plugin deve ser descarregado
    Fatal,          ///< Plugin corrompeu estado — rollback necessário
};

/// Informações sobre uma falha de plugin.
struct PluginFailure {
    std::string plugin_name;        ///< qual plugin falhou
    FailureSeverity severity{ FailureSeverity::Warning };
    std::string error_type;         ///< tag estável: "crash", "timeout", "memory", "abort"
    std::string error_message;      ///< descrição da falha
    std::uint64_t timestamp_ms{ 0 }; ///< quando a falha ocorreu
    std::string stack_trace;        ///< stack trace (se disponível)
    bool corrupted_state{ false };  ///< se o plugin corrompeu estado compartilhado
};

/// Callback para tratamento de falhas de plugins.
using FailureHandler = std::function<void(const PluginFailure&)>;

/// Estado preservado durante hot reload.
struct HotReloadState {
    std::string plugin_name;
    std::string serialized_state;   ///< JSON bit-exact do estado a preservar
    std::uint64_t checkpoint_ms{ 0 }; ///< quando o checkpoint foi feito
    bool valid{ false };
};

/// Resultado de uma operação de hot reload.
enum class HotReloadResult : std::uint8_t {
    Success,        ///< Reload completo com sucesso
    Rollback,       ///< Reload falhou, estado restaurado
    Deferred,       ///< Reload adiado (plugin ocupado)
    Failed,         ///< Reload falhou irrecuperavelmente
};

/// Interface para isolamento de falhas de plugins. O host cria uma
/// instância por plugin e monitora execuções.
///
/// Headless-testable: simular falhas, verificar isolamento, testar
/// recovery — sem GPU, sem filesystem, sem rede.
class IPluginIsolation {
public:
    virtual ~IPluginIsolation() = default;

    /// Nome do plugin isolado.
    [[nodiscard]] virtual const std::string& plugin_name() const = 0;

    /// Número de falhas registradas.
    [[nodiscard]] virtual std::uint32_t failure_count() const = 0;

    /// Última falha registrada (se houver).
    [[nodiscard]] virtual const PluginFailure* last_failure() const = 0;

    /// Verifica se o plugin está em estado saudável (sem falhas críticas).
    [[nodiscard]] virtual bool is_healthy() const = 0;

    /// Registra uma falha. O isolamento decide a ação (log, restart,
    /// descarte) baseado na severidade.
    virtual void record_failure(const PluginFailure& failure) = 0;

    /// Tenta recuperar o plugin (restart). Retorna true se bem-sucedido.
    virtual bool try_recovery(std::string& error) = 0;

    /// Descarta o plugin (unload forçado). Estado compartilhado é
    /// revertido se `corrupted_state` foi flagado.
    virtual bool force_unload(bool revert_corrupted, std::string& error) = 0;

    /// Reseta contadores de falha (após recovery bem-sucedido).
    virtual void reset_failure_count() = 0;

    /// Registra um handler de falhas. Chamado toda vez que uma falha
    /// é registrada.
    virtual void on_failure(FailureHandler handler) = 0;
};

/// Interface para hot reload de plugins. O host cria uma instância
/// global e gerencia o ciclo de vida de reload.
///
/// Hot reload é executado em 3 fases:
///   1. Checkpoint: serializa estado do plugin
///   2. Swap: descarrega versão antiga, carrega nova
///   3. Restore: desserializa estado no novo plugin
///
/// Se qualquer fase falhar, o plugin é restaurado à versão anterior
/// (rollback atômico).
class IPluginHotReload {
public:
    virtual ~IPluginHotReload() = default;

    /// Verifica se hot reload é suportado para um plugin.
    /// Plugins C++ estáticos NÃO suportam hot reload.
    [[nodiscard]] virtual bool is_supported(const std::string& plugin_name) const = 0;

    /// Faz checkpoint do estado de um plugin. Retorna um token de
    /// checkpoint que deve ser passado ao reload.
    [[nodiscard]] virtual HotReloadState checkpoint(const std::string& plugin_name) = 0;

    /// Executa hot reload de um plugin. `new_library_path` é o caminho
    /// da nova versão (pode ser o mesmo path se o arquivo foi substituído).
    /// `checkpoint` é o estado preservado (pode ser inválido para reload
    /// sem preservação).
    virtual HotReloadResult reload(
        const std::string& plugin_name,
        const std::string& new_library_path,
        const HotReloadState& checkpoint,
        std::string& error) = 0;

    /// Rollback manual para a versão anterior.
    virtual HotReloadResult rollback(
        const std::string& plugin_name,
        const HotReloadState& checkpoint,
        std::string& error) = 0;

    /// Histórico de reloads (mais recente primeiro).
    [[nodiscard]] virtual std::vector<HotReloadState> history(
        const std::string& plugin_name) const = 0;

    /// Limpa o histórico de reloads.
    virtual void clear_history(const std::string& plugin_name) = 0;
};

/// Interface para timeout de execução de plugins. O host configura
/// timeouts por plugin e cancela execuções que excedem o limite.
class IPluginTimeout {
public:
    virtual ~IPluginTimeout() = default;

    /// Define o timeout máximo (em ms) para execuções de um plugin.
    /// 0 = sem timeout.
    virtual void set_timeout_ms(const std::string& plugin_name,
                                std::uint64_t timeout_ms) = 0;

    /// Obtém o timeout configurado.
    [[nodiscard]] virtual std::uint64_t get_timeout_ms(
        const std::string& plugin_name) const = 0;

    /// Verifica se uma execução está excedendo o timeout.
    /// Retorna true se o timeout foi atingido.
    [[nodiscard]] virtual bool check_timeout(
        const std::string& plugin_name,
        std::uint64_t elapsed_ms) const = 0;

    /// Cancela execuções pendentes de um plugin.
    virtual void cancel(const std::string& plugin_name) = 0;
};

/// Interface para memória de plugins. O host monitora e limita
/// o consumo de memória por plugin.
class IPluginMemory {
public:
    virtual ~IPluginMemory() = default;

    /// Define o limite de memória (em bytes) para um plugin.
    /// 0 = sem limite.
    virtual void set_limit(const std::string& plugin_name,
                           std::uint64_t limit_bytes) = 0;

    /// Obtém o limite configurado.
    [[nodiscard]] virtual std::uint64_t get_limit(
        const std::string& plugin_name) const = 0;

    /// Obtém o consumo atual de memória de um plugin.
    [[nodiscard]] virtual std::uint64_t get_usage(
        const std::string& plugin_name) const = 0;

    /// Verifica se o plugin excedeu o limite.
    [[nodiscard]] virtual bool is_over_limit(
        const std::string& plugin_name) const = 0;

    /// Reseta o contador de memória (após GC ou unload).
    virtual void reset_usage(const std::string& plugin_name) = 0;
};

}  // namespace engine::plugins
