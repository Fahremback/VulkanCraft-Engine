// sentry-sink.hpp — SentrySink: binding REAL do ISink do contrato
// IObservability (engine::observability, #294) sobre o sentry-native vendido
// (#306). Compilado APENAS pelo gate E2E (observability-sentry-e2e) contra a
// lib estática do external/solutions/sentry-native — não faz parte do build
// da engine.
//
// Cada linha opaca emitida pelo contrato (log/span/counter) vira um evento
// sentry de mensagem com nível mapeado; um transport custom captura o
// envelope SEM rede (determinístico). Prova o wiring: o sink substituível do
// contrato conectado ao crash reporting real.
#pragma once

#include <cstdio>
#include <cstring>
#include <string>

#include "engine/observability/IObservability.hpp"

#include <sentry.h>

namespace observability_e2e {

// Transport custom: conta eventos de mensagem capturados (sem rede).
struct SentryCapture {
    int event_count{ 0 };
    char last_event[2048]{ 0 };
};

static SentryCapture g_capture;

static void capture_transport(sentry_envelope_t* envelope, void* state) {
    (void)state;
    size_t len = 0;
    char* raw = sentry_envelope_serialize(envelope, &len);
    if (raw) {
        if (strstr(raw, "event") != NULL || strstr(raw, "message") != NULL ||
            strstr(raw, "sentry-probe-e2e") != NULL) {
            g_capture.event_count++;
            strncpy(g_capture.last_event, raw, sizeof(g_capture.last_event) - 1);
        }
        sentry_free(raw);
    }
}

struct SentrySink final : engine::observability::ISink {
    // Linha opaca do contrato → evento de mensagem no Sentry real.
    void emit(const std::string& line) override {
        sentry_level_t level = SENTRY_LEVEL_INFO;
        if (line.find("[log warn") != std::string::npos) level = SENTRY_LEVEL_WARNING;
        else if (line.find("[log error") != std::string::npos) level = SENTRY_LEVEL_ERROR;
        else if (line.find("[log trace") != std::string::npos) level = SENTRY_LEVEL_DEBUG;

        sentry_value_t event = sentry_value_new_message_event(
            level, "engine", line.c_str());
        sentry_value_t tags = sentry_value_new_object();
        sentry_value_set_by_key(tags, "sink", sentry_value_new_string("sentry-e2e"));
        sentry_value_set_by_key(event, "tags", tags);
        sentry_capture_event(event);
    }
};

}  // namespace observability_e2e
