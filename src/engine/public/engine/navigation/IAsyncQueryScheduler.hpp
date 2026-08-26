// IAsyncQueryScheduler — escalonador de consultas assíncronas com prioridade,
// cancelamento, timeout e orçamento por frame. Componente CORE do §2 item 27
// ("consultas assíncronas com prioridade, cancelamento, timeout e orçamento
// por frame"): o INavigationProvider já expõe async FIFO + cancel (FALTANTES
// item 12); este contrato adiciona a camada de POLÍTICA determinística que
// ele (ou qualquer sistema de queries) pode usar por cima — sem depender de
// navmesh, física ou threads (o chamador decide onde o lote despachado roda).
//
// Semântica: fila de queries com prioridade (maior vence; empate = FIFO por
// ordem de enqueue), timeout medido no RELÓGIO DO PRÓPRIO SCHEDULER (soma dos
// tick(dt) desde o enqueue; 0 = sem timeout) e despacho por FRAME com
// orçamento de tempo: dispatch(budgetSeconds) seleciona, na ordem de
// prioridade, toda query cujo estimatedCost caiba no orçamento restante
// (greedy determinístico). Sem RNG, sem estado global; o estado é carregado
// no próprio adapter.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace navigation {

// Spec de uma query assíncrona enfileirada.
struct AsyncQuerySpec {
    std::uint64_t queryId{ 0 };        // id opaco do chamador (único na fila)
    float priority{ 0.0f };            // maior vence; empate = FIFO
    float timeoutSeconds{ 0.0f };      // 0 = sem timeout (relógio do scheduler)
    float estimatedCost{ 1.0f };       // > 0; unidade do orçamento por frame
};

class IAsyncQueryScheduler {
public:
    virtual ~IAsyncQueryScheduler() = default;

    // Define a capacidade da fila (substitui a anterior; fila não-vazia é
    // preservada — só o teto muda). maxQueued == 0 rejeita (all-or-nothing).
    virtual bool configure(std::size_t maxQueued, std::string& errorOut) = 0;

    // All-or-nothing: id duplicado, fila cheia, cost <= 0, timeout < 0,
    // priority não-finita (guard /fp:fast) → rejeita SEM mutar a fila.
    virtual bool enqueue(const AsyncQuerySpec& spec, std::string& errorOut) = 0;

    // Remove a query da fila; false para id desconhecido.
    virtual bool cancel(std::uint64_t queryId) = 0;

    // Avança o relógio do scheduler: expira toda query cujo tempo de espera
    // (desde o enqueue) >= timeoutSeconds e a REMOVE, retornando os ids
    // expirados em ordem crescente. dt <= 0 é no-op. Timeout 0 nunca expira.
    virtual std::vector<std::uint64_t> tick(float dt) = 0;

    // Despacha o lote do frame: percorre as queries pendentes na ordem
    // (prioridade desc, FIFO entre iguais) e seleciona toda query cujo
    // estimatedCost caiba no orçamento restante (greedy: cost > restante é
    // PULADA, não bloqueia as seguintes); remove as selecionadas e retorna
    // os ids na ordem de despacho. budget <= 0 → lote vazio. Determinístico.
    virtual std::vector<std::uint64_t> dispatch(float budgetSeconds) = 0;

    virtual std::size_t queued_count() const = 0;
    virtual bool is_queued(std::uint64_t queryId) const = 0;
    virtual void reset() = 0;
};

std::unique_ptr<IAsyncQueryScheduler> create_async_query_scheduler();

}  // namespace navigation
}  // namespace engine
