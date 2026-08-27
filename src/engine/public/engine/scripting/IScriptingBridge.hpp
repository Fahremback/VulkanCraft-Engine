#pragma once
// IScriptingBridge — ponte segura entre scripting e ECS/reflection
// (§3 HANDOFF AGENT-6 linha 203 — "Criar bridge segura entre scripting e
// ECS/reflection sem ponteiros privados, singletons ou dependência direta
// do renderer").
//
// O contrato expõe uma API de leitura/escrita de componentes via handles
// estáveis (strings), sem ponteiros diretos, sem singletons e sem
// dependência do renderer. Scripts (Luau, WASM, visual) interagem com
// o mundo ECS exclusivamente através desta ponte.
//
// Padrão: mesmo espírito de ILuauSandbox/ISink/ISignatureVerifier —
// contrato puro C++17, self-contained (std only), headless, determinístico.
//
// Divisão honesta:
//   - IScriptingBridge = API de acesso ao mundo (ler/escrever componentes,
//     enviar eventos, consultar entidades). Validação all-or-nothing.
//   - O ECS real (AGENT-3/4) implementa a ponte. Scripts não conhecem
//     o ECS — só conhecem handles (strings) e JSON.

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::scripting {

/// Handle estável para uma entidade ECS. Strings opacas — scripts não
/// descompactam nem validam o formato interno.
using EntityHandle = std::string;

/// Handle estável para um componente. Formato: "entity_id:component_type".
using ComponentHandle = std::string;

/// Resultado de uma operação na ponte. `ok` indica sucesso; `error`
/// contém tag estável ("not_found", "permission", "invalid", "timeout")
/// + detalhe opaco. `data` é JSON bit-exact (chaves ordenadas) quando
/// a operação produz dados.
struct BridgeResult {
    bool ok{ false };
    std::string error;      // tag estável + detalhe
    std::string data;       // JSON bit-exact (chaves ordenadas)
};

/// Permissões de acesso ao mundo ECS por script. Declaradas no manifesto
/// do script/plugin e enforceadas pela ponte.
enum class BridgePermission : std::uint8_t {
    ReadComponents,     ///< Ler componentes de entidades
    WriteComponents,    ///< Modificar componentes de entidades
    SpawnEntities,      ///< Criar entidades
    DestroyEntities,    ///< Remover entidades
    SendEvents,         ///< Enviar eventos para o ECS
    QueryEntities,      ///< Consultar entidades por critério
};

/// Consulta ao mundo ECS. Define filtros para busca de entidades.
struct EntityQuery {
    /// Componentes que a entidade DEVE ter (AND).
    std::vector<std::string> required_components;

    /// Componentes que a entidade NÃO deve ter (NOT).
    std::vector<std::string> excluded_components;

    /// Limite máximo de resultados (0 = sem limite).
    std::uint32_t limit{ 0 };
};

/// A ponte entre scripts e o mundo ECS/reflection. O host cria uma
/// instância por contexto de script e a passa ao script no bootstrap.
///
/// Headless-testable: criar ponte com um mock ECS, ler/escrever
/// componentes, verificar validação — sem GPU, sem filesystem, sem rede.
class IScriptingBridge {
public:
    virtual ~IScriptingBridge() = default;

    /// Identificador do contexto (ex: "sandbox:game_logic").
    [[nodiscard]] virtual const std::string& context_id() const = 0;

    /// Verifica se uma permissão está concedida para este contexto.
    [[nodiscard]] virtual bool has_permission(BridgePermission perm) const = 0;

    // ── Leitura de componentes ──────────────────────────────────────

    /// Lê um componente como JSON. `entity` é o handle da entidade;
    /// `component_type` é o nome do tipo (ex: "Transform", "Health").
    /// Entidade ou componente não encontrados → ok=false, error="not_found".
    /// Sem permissão ReadComponents → ok=false, error="permission".
    virtual BridgeResult read_component(
        const EntityHandle& entity,
        const std::string& component_type) const = 0;

    /// Lista todos os componentes de uma entidade.
    virtual BridgeResult list_components(
        const EntityHandle& entity) const = 0;

    // ── Escrita de componentes ──────────────────────────────────────

    /// Cria ou atualiza um componente. `data` é JSON com os campos a
    /// definir (merge shallow — campos ausentes não são removidos).
    /// All-or-nothing: `data` inválido → nada muda.
    virtual BridgeResult write_component(
        const EntityHandle& entity,
        const std::string& component_type,
        const std::string& data) = 0;

    /// Remove um componente de uma entidade.
    virtual BridgeResult remove_component(
        const EntityHandle& entity,
        const std::string& component_type) = 0;

    // ── Entidades ───────────────────────────────────────────────────

    /// Cria uma entidade vazia. Retorna o handle.
    virtual BridgeResult spawn_entity() = 0;

    /// Remove uma entidade e todos os seus componentes.
    virtual BridgeResult destroy_entity(const EntityHandle& entity) = 0;

    /// Consulta entidades por critério.
    virtual BridgeResult query_entities(const EntityQuery& query) const = 0;

    // ── Eventos ─────────────────────────────────────────────────────

    /// Envia um evento ao ECS. `event_type` é o nome do evento;
    /// `data` é JSON com os campos do evento.
    virtual BridgeResult send_event(
        const std::string& event_type,
        const std::string& data) = 0;

    // ── Reflexão ────────────────────────────────────────────────────

    /// Lista tipos de componentes disponíveis.
    virtual BridgeResult list_component_types() const = 0;

    /// Obtém o schema JSON de um tipo de componente.
    virtual BridgeResult get_component_schema(
        const std::string& component_type) const = 0;
};

/// Resultado de uma operação batch na ponte. Cada operação é avaliada
/// independentemente, mas o batch é atômico (qualquer falha → nada muda).
struct BatchResult {
    bool all_ok{ true };
    std::vector<BridgeResult> results;  ///< resultado de cada operação
    std::string error;                  ///< primeiro erro encontrado
};

/// Operação batch: leitura/escrita de múltiplos componentes em
/// uma única transação atômica.
struct BatchOperation {
    enum class Kind {
        ReadComponent,
        WriteComponent,
        RemoveComponent,
        SpawnEntity,
        DestroyEntity,
        SendEvent,
    };

    Kind kind;
    EntityHandle entity;            ///< vazio para SpawnEntity
    std::string component_type;     ///< vazio para Spawn/Destroy/Event
    std::string data;               ///< JSON para Write, vazio para Read/Remove
    std::string event_type;         ///< preenchido apenas para SendEvent
};

/// Extensão da ponte com suporte a operações batch atômicas.
class IScriptingBridgeBatch {
public:
    virtual ~IScriptingBridgeBatch() = default;

    /// Executa um batch de operações. All-or-nothing: qualquer
    /// operação com erro → nenhuma mudança é aplicada.
    virtual BatchResult execute_batch(
        const std::vector<BatchOperation>& operations) = 0;
};

}  // namespace engine::scripting
