#pragma once
// IObservability — observabilidade opt-in (logging, tracing, telemetry e
// contexto de crash) por interfaces substituíveis, para o domínio
// `engine/observability/` (§6 item 6 — "Publicar logging/tracing/crash
// reporting/telemetry opt-in por interfaces substituíveis").
//
// Núcleo headless da observabilidade: sinks são registrados/removidos em
// runtime (substituíveis — o chamador pluga o backend que quiser: stdout,
// arquivo, rede, Sentry, OTLP); log/trace/telemetry só são roteados para os
// sinks ATIVOS (opt-in — sem sink, tudo é no-op barato); um buffer circular
// retém os N últimos eventos + contadores para o contexto de crash; spans de
// trace têm ordem determinística (id global estritamente crescente, sem
// relógio de parede — a ordenação é por sequência, não por timestamp). O
// contrato NÃO conhece backends: sinks recebem linhas de texto opacas
// (formato do chamador). Mesmo espírito dos contratos de networking/assets:
// dados opacos, ordem determinística, persistência JSON bit-exact e
// all-or-nothing.
//
// Self-contained (std only), headless, determinístico.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine::observability {

// Níveis de log (ordem canônica crescente).
enum class LogLevel {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
};

// Um evento de log no buffer circular.
struct LogEvent {
    std::uint64_t sequence{ 0 };  // sequência global estritamente crescente
    LogLevel level{ LogLevel::Info };
    std::string category;  // opaco: "network", "assets", ...
    std::string message;   // linha já formatada pelo chamador (opaca)
};

// Um span de trace (segmento nomeado com início e fim determinísticos).
struct TraceSpan {
    std::uint64_t span_id{ 0 };   // id global estritamente crescente
    std::uint64_t parent_id{ 0 }; // 0 = raiz
    std::string name;             // opaco: "asset.cook", "rpc.drain", ...
    std::uint64_t begin_seq{ 0 }; // sequência do begin
    std::uint64_t end_seq{ 0 };   // sequência do end (0 = ainda aberto)
    bool closed{ false };
};

// Snapshot do estado para contexto de crash.
struct CrashContext {
    std::vector<LogEvent> recent_logs;    // últimos eventos (mais recentes primeiro)
    std::vector<TraceSpan> open_spans;    // spans ainda abertos (ordem de id)
    std::vector<std::pair<std::string, std::int64_t>> counters;  // ordenado por nome
    std::uint64_t total_logs{ 0 };
    std::uint64_t total_spans{ 0 };
};

// Sink: recebe linhas de texto opacas (log, span open/close, contador).
// `line` é uma string já serializada pelo contrato no formato do chamador —
// o sink decide o que fazer (persistir, enviar, descartar).
struct ISink {
    virtual ~ISink() = default;
    virtual void emit(const std::string& line) = 0;
};

class IObservability {
public:
    virtual ~IObservability() = default;

    // Identificador fixo da sessão de observabilidade.
    virtual const std::string& session_id() const = 0;

    // Registra/atualiza um sink. `sinkId` vazio ou `sink` nulo → false e NADA
    // muda (all-or-nothing). Atualização sobrescreve sem duplicar.
    virtual bool register_sink(const std::string& sinkId, ISink* sink,
                               std::string& errorOut) = 0;

    // Remove um sink. Ausente = no-op. Com o último sink removido, o roteamento
    // volta a ser no-op (opt-in desligado) — mas o buffer continua gravando.
    virtual void remove_sink(const std::string& sinkId) = 0;

    // Desliga/liga o roteamento para sinks. `enabled=false` → sinks não
    // recebem nada (mas logs/spans/contadores continuam acumulando no buffer,
    // para o contexto de crash). Default: habilitado.
    virtual void set_enabled(bool enabled) = 0;
    virtual bool enabled() const = 0;

    // Registra um log: roteia para os sinks ativos e acumula no buffer
    // circular. `category` vazio → false (all-or-nothing: nada muda).
    virtual bool log(LogLevel level, const std::string& category,
                     const std::string& message, std::string& errorOut) = 0;

    // Abre um span de trace. `name` vazio → false (nada muda). Retorna o id
    // do span (estritamente crescente).
    virtual bool begin_span(const std::string& name, std::uint64_t parent_id,
                            std::uint64_t& span_idOut, std::string& errorOut) = 0;

    // Fecha um span aberto. Span desconhecido/já fechado → false (nada muda).
    virtual bool end_span(std::uint64_t span_id, std::string& errorOut) = 0;

    // Telemetria: incrementa um contador ou define um gauge (valor absoluto).
    // Nome vazio → false (nada muda). Ordenação por nome no snapshot.
    virtual bool increment_counter(const std::string& name, std::int64_t by,
                                   std::string& errorOut) = 0;
    virtual bool set_gauge(const std::string& name, std::int64_t value,
                           std::string& errorOut) = 0;

    // Snapshot para crash reporting: últimos eventos + spans abertos +
    // contadores. Nunca falha.
    virtual CrashContext crash_context() const = 0;

    // Contagem total de eventos (observabilidade da observabilidade).
    virtual std::uint64_t total_logs() const = 0;
    virtual std::uint64_t total_spans() const = 0;

    // Descarta tudo (nova sessão). Sempre ok.
    virtual bool reset(std::string& errorOut) = 0;

    // --- Persistência (bit-exact, all-or-nothing) ---
    virtual bool load_from_json(const std::string& json, std::string& errorOut) = 0;
    virtual std::string serialize_state() const = 0;
};

// Cria uma sessão de observabilidade. `sessionId` deve ser não-vazio
// (all-or-nothing). O buffer circular retém os últimos `history` eventos
// (>= 1; valores < 1 são clampados para 1).
std::unique_ptr<IObservability> create_observability(const std::string& sessionId,
                                                     std::size_t history,
                                                     std::string& errorOut);

}  // namespace engine::observability
