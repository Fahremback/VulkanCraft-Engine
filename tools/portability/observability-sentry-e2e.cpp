// observability-sentry-e2e.cpp — E2E do contrato IObservability com o backend
// REAL: compila o adapter (src/engine/sdk/Observability.cpp) + o SentrySink
// (binding sobre o sentry-native vendido, #306) e prova o fluxo completo:
// sink substituível conectado ao crash reporting real, log/spans/contadores
// roteados como eventos sentry, opt-in (enabled=false → nada capturado),
// crash context do contrato preservado, persistência bit-exact. O
// enforcement determinístico é do contrato (ctest); aqui prova-se o WIRING
// real. Mesmo padrão do luau-sandbox-e2e (#304) e package-manager-sodium-e2e
// (#299).
//
// Compilado APENAS pelo gate (observability-sentry-e2e-gate.mjs) contra a lib
// estática do external/solutions/sentry-native — não faz parte do build da
// engine.

#include <cstdio>
#include <string>

#include "engine/observability/IObservability.hpp"
#include "sentry-sink.hpp"

#include "../../src/engine/sdk/Observability.cpp"

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what.c_str());
        ++failures;
    }
}

}  // namespace

int main() {
    std::printf("observability-sentry-e2e: IObservability wired to real sentry-native\n");

    // Sentry real: init com transport custom (sem rede, determinístico).
    sentry_options_t* options = sentry_options_new();
    sentry_options_set_dsn(options, "https://probe@example.com/1");
    sentry_options_set_release(options, "vulkan-craft@1.0.0");
    sentry_transport_t* transport = sentry_transport_new(observability_e2e::capture_transport);
    sentry_options_set_transport(options, transport);
    check(sentry_init(options) == 0, "sentry_init ok (inproc + custom transport)");

    std::string err;
    observability_e2e::SentrySink sink;
    auto obs = engine::observability::create_observability("e2e-obs", 8, err);
    check(obs != nullptr, "IObservability criada");
    check(obs->register_sink("sentry", &sink, err), "SentrySink anexado (substituível)");

    // Logs/spans/contadores roteados para o Sentry real.
    obs->log(engine::observability::LogLevel::Info, "net", "client connected", err);
    obs->log(engine::observability::LogLevel::Error, "crash", "shader compile failed", err);
    std::uint64_t sp = 0;
    obs->begin_span("asset.cook", 0, sp, err);
    obs->end_span(sp, err);
    obs->increment_counter("ticks", 42, err);

    check(observability_e2e::g_capture.event_count >= 4,
          "4+ linhas roteadas viram eventos sentry (got " +
              std::to_string(observability_e2e::g_capture.event_count) + ")");
    check(strstr(observability_e2e::g_capture.last_event, "sentry-e2e") != NULL,
          "evento sentry carrega a tag do sink");

    // Opt-in: desligado → nada chega ao Sentry.
    obs->set_enabled(false);
    const int before = observability_e2e::g_capture.event_count;
    obs->log(engine::observability::LogLevel::Warn, "net", "silent line", err);
    check(observability_e2e::g_capture.event_count == before,
          "opt-in: enabled=false → nada capturado");
    auto ctx = obs->crash_context();
    check(ctx.total_logs >= 3, "buffer do contrato continua gravando desligado (2+1)");
    obs->set_enabled(true);

    // Persistência bit-exact do contrato (independente do backend).
    const std::string snap = obs->serialize_state();
    auto obs2 = engine::observability::create_observability("e2e-obs", 8, err);
    check(obs2->load_from_json(snap, err), "load do snapshot");
    check(obs2->serialize_state() == snap, "round-trip bit-exact");

    sentry_shutdown();
    if (failures == 0) {
        std::printf("observability-sentry-e2e: ALL PASSED (real sentry wired through IObservability)\n");
        return 0;
    }
    std::printf("observability-sentry-e2e: %d FAILURE(S)\n", failures);
    return 1;
}
