// IAiDebugInfo — gravador de SNAPSHOT da execução de IA para visualização e
// profiling. Componente CORE do §3 item 40 ("expor authoring, visualização,
// profiling e breakpoints da IA no editor"): o runtime de IA (IBehaviorTree/
// IUtilityAi/...) alimenta o gravador a cada tick; o editor/CLI/MCP consulta
// o snapshot do último tick COMPLETO (nós visitados com status/profundidade
// + blackboard) — sem acoplar. Puro, determinístico (blackboard ordenado por
// chave; nós na ordem de visita); o autor do tick é o chamador.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace ai {

struct AiDebugNode {
    std::string id;
    std::string status;   // running/succeeded/failed/aborted
    int depth{ 0 };
    std::string detail;   // ex.: nome do nó, valor retornado
};

struct AiDebugBlackboard {
    std::string key;
    std::string value;
};

struct AiDebugSnapshot {
    std::uint64_t agentId{ 0 };
    std::string treeName;
    std::uint64_t tick{ 0 };
    std::vector<AiDebugNode> nodes;            // ordem de visita
    std::vector<AiDebugBlackboard> blackboard; // ordem crescente de chave
};

class IAiDebugRecorder {
public:
    virtual ~IAiDebugRecorder() = default;

    // Abre um novo tick (descarta o snapshot em construção anterior).
    virtual void begin_tick(std::uint64_t agentId, const std::string& treeName,
                            std::uint64_t tick) = 0;

    // Registra a visita a um nó no tick corrente. Id duplicado no mesmo tick
    // recusa (all-or-nothing, sem mutar).
    virtual bool node_visit(const std::string& id, const std::string& status,
                            int depth, const std::string& detail) = 0;

    // Registra um valor de blackboard no tick corrente. Chave duplicada
    // SUBSTITUI (o snapshot mostra o último valor do tick).
    virtual bool blackboard_set(const std::string& key, const std::string& value) = 0;

    // Snapshot do último tick COMPLETO (nullptr se nenhum tick foi fechado).
    virtual const AiDebugSnapshot* snapshot() const = 0;

    // Snapshot em JSON: {"agentId":..,"treeName":..,"tick":..,"nodes":[...],
    // "blackboard":[...]}.
    virtual std::string to_json() const = 0;

    virtual void clear() = 0;
};

std::unique_ptr<IAiDebugRecorder> create_ai_debug_recorder();

}  // namespace ai
}  // namespace engine
