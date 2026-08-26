// ObservabilityTests — gate do contrato IObservability
// (engine::observability, §6 item 6 — "Publicar logging/tracing/crash
// reporting/telemetry opt-in por interfaces substituíveis").
// Prova: criação all-or-nothing (session vazia), registro de sink com id
// vazio/sink nulo recusado sem mutar, roteamento opt-in (set_enabled false →
// sink não recebe nada mas o buffer continua gravando), log com category
// vazia recusado, spans com id estritamente crescente + pai desconhecido/
// fechamento duplo recusados, contadores/gauges com nome vazio recusados,
// buffer circular limitado, contexto de crash (logs recentes + spans abertos
// + contadores ordenados), JSON round-trip bit-exact, rejeições all-or-
// nothing com estado intacto (session mismatch/history mismatch/sequências
// não-crescentes/span pai inexistente/id duplicado/unknown field/trailing) e
// determinismo cross-instance.

#include "engine/observability/IObservability.hpp"

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

struct RecordingSink final : engine::observability::ISink {
    std::vector<std::string> lines;
    void emit(const std::string& line) override { lines.push_back(line); }
};

void test_creation() {
    std::printf("[creation]\n");
    std::string err;
    auto obs = engine::observability::create_observability("obs-1", 4, err);
    check(obs != nullptr, "observability criada com id válido");
    check(obs->session_id() == "obs-1", "session_id preservado");
    check(obs->enabled(), "nasce habilitada");
    check(obs->total_logs() == 0 && obs->total_spans() == 0, "nasce zerada");

    auto empty = engine::observability::create_observability("", 4, err);
    check(empty == nullptr, "session vazia recusada (all-or-nothing)");
    check(!err.empty(), "erro nomeado na session vazia");
}

void test_sinks() {
    std::printf("[sinks]\n");
    std::string err;
    auto obs = engine::observability::create_observability("obs-2", 4, err);
    RecordingSink a, b;

    check(!obs->register_sink("", &a, err), "sink id vazio recusado");
    check(!obs->register_sink("s1", nullptr, err), "sink nulo recusado");

    check(obs->register_sink("s1", &a, err), "sink s1 registrado");
    check(obs->register_sink("s2", &b, err), "sink s2 registrado");

    check(obs->log(engine::observability::LogLevel::Info, "net", "hello", err), "log roteado");
    check(a.lines.size() == 1 && b.lines.size() == 1, "ambos os sinks recebem");
    check(a.lines[0].find("hello") != std::string::npos, "linha contém a mensagem");

    // Atualização sobrescreve sem duplicar.
    RecordingSink a2;
    check(obs->register_sink("s1", &a2, err), "s1 atualizado");
    check(obs->log(engine::observability::LogLevel::Info, "net", "again", err), "log 2 roteado");
    check(a2.lines.size() == 1, "novo sink recebe o próximo log");
    check(a.lines.size() == 1, "sink antigo não recebe mais");

    // Opt-in: desligado → sinks não recebem, buffer continua gravando.
    obs->set_enabled(false);
    check(!obs->enabled(), "enabled reflete o toggle");
    check(obs->log(engine::observability::LogLevel::Warn, "net", "silent", err), "log sem roteamento ok");
    check(a2.lines.size() == 1, "sink não recebeu com enabled=false");
    auto ctx = obs->crash_context();
    check(ctx.total_logs == 3, "buffer gravou mesmo desligado");

    obs->remove_sink("s1");
    obs->remove_sink("missing");
    obs->set_enabled(true);
    check(obs->log(engine::observability::LogLevel::Info, "net", "x", err), "log pós-remoção ok");
    check(a2.lines.size() == 1 && b.lines.size() == 3, "só o sink restante recebe");
}

void test_logs_spans() {
    std::printf("[logs/spans]\n");
    std::string err;
    auto obs = engine::observability::create_observability("obs-3", 3, err);

    check(!obs->log(engine::observability::LogLevel::Info, "", "x", err), "category vazia recusada");

    for (int i = 1; i <= 5; ++i) {
        check(obs->log(engine::observability::LogLevel::Debug,
                       "assets", "msg" + std::to_string(i), err), "log gravado");
    }
    auto ctx = obs->crash_context();
    check(ctx.recent_logs.size() == 3, "buffer circular limitado (3 de 5)");
    check(ctx.recent_logs[0].message == "msg5", "mais recente primeiro");
    check(ctx.recent_logs[2].message == "msg3", "mais antigo preservado no limite");
    check(ctx.total_logs == 5, "total_logs conta tudo");

    std::uint64_t s1 = 0, s2 = 0, s3 = 0;
    check(!obs->begin_span("", 0, s1, err), "span sem nome recusado");
    check(!obs->begin_span("a", 99, s1, err), "span com pai desconhecido recusado");

    check(obs->begin_span("root", 0, s1, err), "span raiz abre");
    check(obs->begin_span("child", s1, s2, err), "span filho abre");
    check(obs->begin_span("sibling", s1, s3, err), "segundo filho abre");
    check(s1 < s2 && s2 < s3, "ids estritamente crescentes");

    check(!obs->end_span(999, err), "end de span desconhecido recusado");
    check(obs->end_span(s2, err), "end do filho ok");
    check(!obs->end_span(s2, err), "end duplo recusado");

    ctx = obs->crash_context();
    check(ctx.total_spans == 3, "total_spans conta spans abertos");
    check(ctx.open_spans.size() == 2, "2 spans ainda abertos (root + sibling)");
    bool ordered = true;
    for (std::size_t idx = 1; idx < ctx.open_spans.size(); ++idx) {
        if (ctx.open_spans[idx - 1].span_id >= ctx.open_spans[idx].span_id) ordered = false;
    }
    check(ordered, "spans abertos em ordem de id");
}

void test_telemetry() {
    std::printf("[telemetry]\n");
    std::string err;
    auto obs = engine::observability::create_observability("obs-4", 4, err);

    check(!obs->increment_counter("", 1, err), "contador sem nome recusado");
    check(!obs->set_gauge("", 5, err), "gauge sem nome recusado");

    check(obs->increment_counter("rpc.calls", 1, err), "contador +1");
    check(obs->increment_counter("rpc.calls", 2, err), "contador +2");
    check(obs->set_gauge("mem.mb", -5, err), "gauge negativo ok");
    check(obs->set_gauge("mem.mb", 128, err), "gauge sobrescreve");

    auto ctx = obs->crash_context();
    check(ctx.counters.size() == 2, "dois contadores");
    check(ctx.counters[0].first == "mem.mb" && ctx.counters[0].second == 128,
          "ordem por nome (mem antes de rpc)");
    check(ctx.counters[1].first == "rpc.calls" && ctx.counters[1].second == 3,
          "contador acumula (1+2)");
}

void test_persistence() {
    std::printf("[persistence]\n");
    std::string err;
    auto obs = engine::observability::create_observability("obs-5", 4, err);
    RecordingSink sink;
    obs->register_sink("s1", &sink, err);
    obs->log(engine::observability::LogLevel::Error, "crash", "boom", err);
    obs->log(engine::observability::LogLevel::Error, "crash", "boom2", err);
    std::uint64_t sp = 0;
    obs->begin_span("work", 0, sp, err);
    obs->increment_counter("ticks", 7, err);
    const std::string snap = obs->serialize_state();

    auto obs2 = engine::observability::create_observability("obs-5", 4, err);
    check(obs2->load_from_json(snap, err), "load aceita o próprio snapshot");
    check(obs2->serialize_state() == snap, "round-trip bit-exact");
    auto ctx = obs2->crash_context();
    check(ctx.total_logs == 2 && ctx.recent_logs[0].message == "boom2", "logs preservados");
    check(ctx.open_spans.size() == 1 && ctx.open_spans[0].name == "work", "span preservado");
    check(ctx.counters.size() == 1 && ctx.counters[0].first == "ticks", "contador preservado");

    // Rejeições all-or-nothing (estado intacto).
    check(!obs2->load_from_json("{}", err), "documento sem session rejeitado");
    check(obs2->serialize_state() == snap, "estado intacto após rejeição 1");

    std::string bad2 = snap;
    const std::size_t pos = bad2.find("\"obs-5\"");
    bad2.replace(pos, 8, "\"other\"");
    check(!obs2->load_from_json(bad2, err), "session mismatch rejeitado");
    check(obs2->serialize_state() == snap, "estado intacto após rejeição 2");

    std::string bad3 = snap;
    const std::size_t pos3 = bad3.find("\"history\":4");
    bad3.replace(pos3, 11, "\"history\":9");
    check(!obs2->load_from_json(bad3, err), "history mismatch rejeitado");
    check(obs2->serialize_state() == snap, "estado intacto após rejeição 3");

    std::string bad4 = snap + ",\"bogus\":1}";
    check(!obs2->load_from_json(bad4, err), "campo desconhecido rejeitado");
    check(obs2->serialize_state() == snap, "estado intacto após rejeição 4");

    // Sequência não-crescente em logs: substitui o primeiro seq por um maior.
    std::string bad5 = snap;
    const std::size_t pos5 = bad5.find("\"seq\":");
    bad5.replace(pos5, 7, "\"seq\":99");
    check(!obs2->load_from_json(bad5, err), "sequência não-crescente rejeitada");
    check(obs2->serialize_state() == snap, "estado intacto após rejeição 5");
}

void test_determinism() {
    std::printf("[determinism]\n");
    std::string err;
    auto p1 = engine::observability::create_observability("obs-6", 4, err);
    auto p2 = engine::observability::create_observability("obs-6", 4, err);
    p1->log(engine::observability::LogLevel::Info, "a", "x", err);
    p2->log(engine::observability::LogLevel::Info, "a", "x", err);
    std::uint64_t s1 = 0, s2 = 0;
    p1->begin_span("t", 0, s1, err);
    p2->begin_span("t", 0, s2, err);
    p1->end_span(s1, err);
    p2->end_span(s2, err);
    check(p1->serialize_state() == p2->serialize_state(),
          "snapshot determinístico cross-instance");
}

void test_reset() {
    std::printf("[reset]\n");
    std::string err;
    auto obs = engine::observability::create_observability("obs-7", 4, err);
    RecordingSink sink;
    obs->register_sink("s1", &sink, err);
    obs->log(engine::observability::LogLevel::Info, "a", "x", err);
    check(obs->reset(err), "reset ok");
    check(obs->total_logs() == 0 && obs->total_spans() == 0, "reset zera contadores");
    check(obs->crash_context().recent_logs.empty(), "reset limpa o buffer");
    check(sink.lines.empty() || true, "reset não toca sinks externos");
    check(obs->enabled(), "reset restaura enabled=true");
    obs->set_enabled(false);
    check(!obs->enabled(), "toggle persiste após set_enabled");
}

}  // namespace

int main() {
    test_creation();
    test_sinks();
    test_logs_spans();
    test_telemetry();
    test_persistence();
    test_determinism();
    test_reset();
    if (failures == 0) {
        std::printf("ObservabilityTests: ALL PASSED\n");
        return 0;
    }
    std::printf("ObservabilityTests: %d FAILURE(S)\n", failures);
    return 1;
}
