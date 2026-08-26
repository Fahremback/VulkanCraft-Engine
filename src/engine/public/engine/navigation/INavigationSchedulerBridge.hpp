// INavigationSchedulerBridge — integração real entre o escalonador de
// consultas assíncronas (IAsyncQueryScheduler, item 27) e um provedor de
// navegação (INavigationProvider, FALTANTES item 12). Componente de WIRING
// do §2: enfileira consultas de path com prioridade no scheduler e, a cada
// frame, despacha o lote do orçamento para begin_async_path do provider,
// coletando os resultados concluídos. Determinístico e headless: o provider
// é injetado (qualquer implementação do contrato), então o gate usa um mock
// determinístico — a política (prioridade/timeout/orçamento/join de cancel)
// fica no bridge e é provada sem navmesh real.
//
// Sem RNG, sem estado global. O relógio de timeout é o do scheduler
// (avançado via tick). O cancel de uma query ainda enfileirada remove do
// scheduler; o cancel de uma em voo propaga para o provider (join).

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace navigation {

class INavigationProvider;
class IAsyncQueryScheduler;

struct NavQueryResult {
    std::uint64_t queryId{ 0 };
    bool succeeded = false;
    bool cancelled = false;
    std::string pathJson;   // resultado serializado (JSON opaco do provider)
    std::string error;      // diagnóstico em Failed
};

class INavigationSchedulerBridge {
public:
    virtual ~INavigationSchedulerBridge() = default;

    // Configura capacidade da fila e orçamento máximo por frame (ambos
    // all-or-nothing: 0 rejeita). O orçamento padrão vem do scheduler.
    virtual bool configure(std::size_t maxQueued, float maxBudgetSeconds,
                           std::string& errorOut) = 0;

    // Enfileira uma consulta de path com prioridade/timeout/custo estimado.
    // `start`/`goal` são opacos ao scheduler — o bridge os guarda e os passa
    // ao provider no despacho. All-or-nothing (id duplicado, fila cheia,
    // pontos não-finitos → rejeita sem mutar nada).
    virtual bool enqueue_path(std::uint64_t queryId, float priority,
                              float timeoutSeconds, float estimatedCost,
                              float startX, float startY, float startZ,
                              float goalX, float goalY, float goalZ,
                              std::string& errorOut) = 0;

    // Cancela: remove da fila se ainda enfileirada; propaga ao provider se
    // já despachada (join). false para id desconhecido.
    virtual bool cancel(std::uint64_t queryId) = 0;

    // Avança o relógio do scheduler (timeouts) e retorna ids expirados.
    virtual std::vector<std::uint64_t> tick(float dt) = 0;

    // Frame: despacha o lote do orçamento para o provider e coleta os
    // resultados concluídos (Succeeded/Failed/Cancelled) desde o último
    // frame. Determinístico na ordem dos resultados (por id crescente).
    virtual std::vector<NavQueryResult> frame() = 0;

    virtual std::size_t queued_count() const = 0;
    virtual std::size_t in_flight_count() const = 0;
    virtual void reset() = 0;
};

// Cria o bridge com o scheduler e o provider fornecidos (o bridge NÃO é
// dono deles — o chamador gerencia o ciclo de vida; nullptr em qualquer um
// retorna nullptr). O orçamento padrão por frame é 1.0s.
std::unique_ptr<INavigationSchedulerBridge> create_navigation_scheduler_bridge(
    IAsyncQueryScheduler* scheduler, INavigationProvider* provider);

}  // namespace navigation
}  // namespace engine
