// IGameplayMetrics — registro de métricas de gameplay NOMEADAS (contadores,
// gauges e amostras) com snapshot determinístico e saída JSON. Componente
// CORE do §2 item 30 ("expor ... métricas e consultas públicas para editor,
// scripting, CLI e MCP"): sistemas (explosões, hit reactions, abilities,
// ragdolls, queries) registram e reportam; editor/CLI/MCP consultam o
// snapshot — sem acoplar. O IFrameProfiler (engine/profiling) cobre timing
// de frame; este contrato cobre métricas SEMÂNTICAS de gameplay.
//
// Semântica: `register_metric(name, kind)` cria a métrica (duplicata recusa,
// all-or-nothing); `record(name, value)` — Counter SOMA, Gauge seta o último,
// Sample acumula soma/min/max e expõe a MÉDIA (value = soma/count);
// `snapshot()` devolve tudo em ordem crescente de nome (determinístico);
// `reset`/`reset_all` zeram; `to_json` emite o snapshot completo. Valores
// não-finitos recusados (guard /fp:fast, findings #79). Sem RNG, sem estado
// global; o estado é carregado no próprio adapter.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace gameplay {

enum class GameplayMetricKind : std::uint8_t { Counter, Gauge, Sample };

// Estado observável de uma métrica num instante.
struct GameplayMetric {
    std::string name;
    GameplayMetricKind kind{ GameplayMetricKind::Counter };
    double value{ 0.0 };        // counter: soma; gauge: último; sample: média
    std::uint64_t count{ 0 };   // nº de record() recebidos
    double minValue{ 0.0 };
    double maxValue{ 0.0 };
};

class IGameplayMetrics {
public:
    virtual ~IGameplayMetrics() = default;

    // Cria a métrica. All-or-nothing: nome vazio ou duplicado rejeita.
    virtual bool register_metric(const std::string& name,
                                 GameplayMetricKind kind,
                                 std::string& errorOut) = 0;

    // Registra um valor. Métrica não registrada ou valor não-finito → false
    // (sem mutar nada). Counter soma; Gauge seta; Sample acumula.
    virtual bool record(const std::string& name, double value,
                        std::string& errorOut) = 0;

    // Estado de TODAS as métricas em ordem crescente de nome (determinístico).
    virtual std::vector<GameplayMetric> snapshot() const = 0;

    // Zera uma métrica (0 = nome desconhecido). Zera todas com reset_all().
    virtual bool reset(const std::string& name) = 0;
    virtual void reset_all() = 0;

    // Snapshot em JSON: {"metrics":[{name,kind,value,count,min,max}, ...]}.
    virtual std::string to_json() const = 0;
};

std::unique_ptr<IGameplayMetrics> create_gameplay_metrics();

}  // namespace gameplay
}  // namespace engine
