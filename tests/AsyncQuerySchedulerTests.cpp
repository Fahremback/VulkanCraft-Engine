// AsyncQuerySchedulerTests — gate do contrato IAsyncQueryScheduler (§2 item
// 27, consultas assíncronas CORE): prova prioridade (maior vence) com FIFO
// entre iguais, timeout no relógio do scheduler, despacho por frame com
// orçamento (greedy: custo maior que o restante é pulado, não bloqueia),
// cancel, recusas all-or-nothing sem mutar a fila e determinismo.

#include "engine/navigation/IAsyncQueryScheduler.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what);
        ++failures;
    }
}

bool ids_equal(const std::vector<std::uint64_t>& actual,
               const std::vector<std::uint64_t>& expected, const char* what) {
    if (actual.size() != expected.size()) {
        check(false, what);
        return false;
    }
    bool ok = true;
    for (std::size_t i = 0; i < actual.size(); ++i) {
        if (actual[i] != expected[i]) ok = false;
    }
    check(ok, what);
    return ok;
}

void test_priority_and_fifo() {
    auto sched = engine::navigation::create_async_query_scheduler();
    std::string error;
    engine::navigation::AsyncQuerySpec spec;
    spec.queryId = 1; spec.priority = 1.0f; spec.estimatedCost = 1.0f;
    check(sched->enqueue(spec, error), "enqueue A (pri 1)");
    spec.queryId = 2; spec.priority = 3.0f;
    check(sched->enqueue(spec, error), "enqueue B (pri 3)");
    spec.queryId = 3; spec.priority = 2.0f;
    check(sched->enqueue(spec, error), "enqueue C (pri 2)");
    spec.queryId = 4; spec.priority = 3.0f;
    check(sched->enqueue(spec, error), "enqueue D (pri 3, depois de B)");

    // Prioridade desc; empate B antes de D (FIFO).
    ids_equal(sched->dispatch(100.0f), { 2, 4, 3, 1 },
              "dispatch: [B, D, C, A] (prioridade + FIFO)");
    check(sched->queued_count() == 0, "fila vazia após dispatch total");
}

void test_budget_greedy() {
    auto sched = engine::navigation::create_async_query_scheduler();
    std::string error;
    engine::navigation::AsyncQuerySpec spec;
    spec.queryId = 1; spec.priority = 1.0f; spec.estimatedCost = 1.0f;
    check(sched->enqueue(spec, error), "A cost 1.0");
    spec.queryId = 2; spec.priority = 2.0f; spec.estimatedCost = 2.0f;
    check(sched->enqueue(spec, error), "B cost 2.0");
    spec.queryId = 3; spec.priority = 3.0f; spec.estimatedCost = 0.5f;
    check(sched->enqueue(spec, error), "C cost 0.5");

    // Ordem: C(0.5) → B(2.0) = 2.5; A(1.0) não cabe nos 2.5 restantes (0) → pula.
    ids_equal(sched->dispatch(2.5f), { 3, 2 },
              "budget 2.5: [C, B], A pulado");
    check(sched->queued_count() == 1 && sched->is_queued(1),
          "A permanece na fila (custo > orçamento restante)");

    // Orçamento maior pega tudo.
    ids_equal(sched->dispatch(100.0f), { 1 }, "budget total pega A");
}

void test_timeout() {
    auto sched = engine::navigation::create_async_query_scheduler();
    std::string error;
    engine::navigation::AsyncQuerySpec spec;
    spec.queryId = 1; spec.timeoutSeconds = 2.0f;
    check(sched->enqueue(spec, error), "enqueue com timeout 2.0");
    spec.queryId = 2; spec.timeoutSeconds = 0.0f;
    check(sched->enqueue(spec, error), "enqueue sem timeout");
    spec.queryId = 3; spec.timeoutSeconds = 1.0f;
    check(sched->enqueue(spec, error), "enqueue com timeout 1.0");

    check(sched->tick(0.5f).empty(), "tick 0.5: nada expira");
    ids_equal(sched->tick(0.5f), { 3 }, "tick +0.5 (=1.0): id 3 expira");
    check(sched->is_queued(1) && sched->is_queued(2), "1 e 2 seguem na fila");
    ids_equal(sched->tick(1.0f), { 1 }, "tick +1.0 (=2.0): id 1 expira");
    check(sched->is_queued(2), "id 2 (timeout 0) nunca expira");
    check(sched->tick(50.0f).empty(), "id 2 segue vivo após ticks grandes");
    check(sched->queued_count() == 1, "só id 2 na fila");
}

void test_cancel_and_refusals() {
    auto sched = engine::navigation::create_async_query_scheduler();
    std::string error;
    check(sched->configure(2, error), "configure maxQueued 2");
    engine::navigation::AsyncQuerySpec spec;
    spec.queryId = 1;
    check(sched->enqueue(spec, error), "enqueue 1");
    check(sched->cancel(1), "cancel 1");
    check(!sched->is_queued(1) && sched->queued_count() == 0, "fila vazia");
    check(!sched->cancel(1), "cancel de id desconhecido → false");

    // Refusos all-or-nothing: fila intacta após cada um.
    spec.queryId = 1;
    check(sched->enqueue(spec, error), "enqueue 1 (de novo)");
    spec.queryId = 2;
    check(sched->enqueue(spec, error), "enqueue 2");
    spec.queryId = 3;
    check(!sched->enqueue(spec, error), "fila cheia (3º) recusa");
    check(sched->queued_count() == 2, "fila intacta após recusa de cheia");

    spec.queryId = 2;
    check(!sched->enqueue(spec, error), "id duplicado recusa");
    spec.queryId = 4; spec.estimatedCost = 0.0f;
    check(!sched->enqueue(spec, error), "cost 0 recusa");
    spec.estimatedCost = -1.0f;
    check(!sched->enqueue(spec, error), "cost negativo recusa");
    spec.estimatedCost = 1.0f; spec.timeoutSeconds = -0.5f;
    check(!sched->enqueue(spec, error), "timeout negativo recusa");
    spec.timeoutSeconds = 0.0f; spec.queryId = 0;
    check(!sched->enqueue(spec, error), "queryId 0 recusa");
    spec.queryId = 5;
    check(sched->enqueue(spec, error) == false, "fila cheia recusa de novo");
    check(sched->queued_count() == 2, "fila intacta após todas as recusas");

    auto bad = engine::navigation::create_async_query_scheduler();
    check(!bad->configure(0, error), "configure 0 recusa");
}

void test_determinism() {
    auto a = engine::navigation::create_async_query_scheduler();
    auto b = engine::navigation::create_async_query_scheduler();
    std::string error;
    engine::navigation::AsyncQuerySpec spec;
    const std::uint64_t ids[] = { 10, 20, 30, 40, 50 };
    const float pr[] = { 1.0f, 3.0f, 2.0f, 3.0f, 0.5f };
    const float cost[] = { 1.0f, 2.0f, 0.5f, 1.5f, 0.25f };
    for (int i = 0; i < 5; ++i) {
        spec.queryId = ids[i]; spec.priority = pr[i]; spec.estimatedCost = cost[i];
        check(a->enqueue(spec, error) && b->enqueue(spec, error), "enqueue duplo");
    }
    check(a->tick(0.4f) == b->tick(0.4f), "tick idêntico (timeout 0: vazio)");
    ids_equal(a->dispatch(2.0f), b->dispatch(2.0f), "dispatch determinístico");
    check(a->queued_count() == b->queued_count(), "estado convergente");
    ids_equal(a->dispatch(100.0f), b->dispatch(100.0f), "restante determinístico");
}

}  // namespace

int main() {
    test_priority_and_fifo();
    test_budget_greedy();
    test_timeout();
    test_cancel_and_refusals();
    test_determinism();

    if (failures == 0) {
        std::printf("async_query_scheduler_tests: all checks passed\n");
        return 0;
    }
    std::printf("async_query_scheduler_tests: %d failure(s)\n", failures);
    return 1;
}
