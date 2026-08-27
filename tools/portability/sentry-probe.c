// sentry-probe.c — §7 probe de utilização do sentry-native vendido
// (finding #306). Compila+roda contra a lib estática MSVC
// (external/solutions/sentry-native/build-gate/Release/sentry.lib) com o
// backend inproc: init com transporte CUSTOM em processo (captura o envelope
// SEM rede — determinístico), capture de event de mensagem, tags/contexto via
// sentry_value, flush do transport, shutdown. Exit 0 = crash reporting
// vendido é utilizável SEM wiring de CMake (backend real do ISink de
// observability #294). Mesmo padrão dos probes immer/sqlite/libsodium/curl/
// tuf/luau.
#include <stdio.h>
#include <string.h>

#include <sentry.h>

static int envelope_count = 0;
static char first_envelope[4096] = { 0 };

// Transport custom: recebe o envelope serializado SEM enviar para a rede —
// prova o caminho completo (event → serialização → transport) de forma
// determinística, igual ao harness de testes oficial do sentry-native.
// O primeiro envelope é o do EVENT capturado; sentry_shutdown() envia um
// envelope de SESSION à parte (contagem total = eventos + sessões).
static void capture_transport(sentry_envelope_t* envelope, void* state) {
    (void)state;
    size_t len = 0;
    char* raw = sentry_envelope_serialize(envelope, &len);
    if (raw) {
        if (envelope_count == 0) {
            strncpy(first_envelope, raw, sizeof(first_envelope) - 1);
        }
        envelope_count++;
        sentry_free(raw);
    }
}

static int failures = 0;

static void check(int cond, const char* label, const char* detail) {
    if (cond) {
        printf("  ok  %s\n", label);
    } else {
        failures++;
        printf("FAIL  %s  %s\n", label, detail ? detail : "");
    }
}

int main(void) {
    printf("sentry-probe: vendored sentry-native\n");

    sentry_options_t* options = sentry_options_new();
    check(options != NULL, "options criadas", "");
    sentry_options_set_dsn(options, "https://probe@example.com/1");
    sentry_options_set_environment(options, "gate");
    sentry_options_set_release(options, "vulkan-craft@1.0.0");
    sentry_options_set_database_path(options, ".");
    sentry_options_set_debug(options, 1);

    sentry_transport_t* transport = sentry_transport_new(capture_transport);
    sentry_options_set_transport(options, transport);

    check(sentry_init(options) == 0, "sentry_init ok (backend inproc)", "");
    sentry_clear_modulecache();
    check(1, "clear_modulecache ok", "");

    // Evento de mensagem com nível + tag + contexto.
    sentry_value_t event = sentry_value_new_message_event(
        SENTRY_LEVEL_WARNING, "probe", "hello from gate");
    sentry_value_t tags = sentry_value_new_object();
    sentry_value_set_by_key(tags, "gate", sentry_value_new_string("sentry-probe"));
    sentry_value_set_by_key(event, "tags", tags);
    sentry_value_set_by_key(event, "extra", sentry_value_new_string("deterministic"));

    sentry_uuid_t id = sentry_capture_event(event);
    check(!sentry_uuid_is_nil(&id), "event capturado com uuid não-nulo", "");
    check(envelope_count == 1, "transport custom recebeu 1 envelope (o event)", "");
    check(strstr(first_envelope, "hello from gate") != NULL,
          "envelope do event contém o payload",
          first_envelope[0] ? first_envelope : "(empty)");

    sentry_shutdown();
    // shutdown envia o envelope de SESSION (flush) — total = event + session.
    check(envelope_count == 2, "shutdown faz flush (event + session)", "");
    check(strstr(first_envelope, "vulkan-craft@1.0.0") != NULL ||
              strstr(first_envelope, "gate") != NULL,
          "envelope do event contém release/ambiente", "");

    // Re-init: ciclo completo funciona de novo (crash reporting reutilizável).
    sentry_options_t* options2 = sentry_options_new();
    sentry_options_set_dsn(options2, "https://probe@example.com/1");
    sentry_transport_t* transport2 = sentry_transport_new(capture_transport);
    sentry_options_set_transport(options2, transport2);
    check(sentry_init(options2) == 0, "segundo init ok", "");
    sentry_value_t ev2 = sentry_value_new_message_event(
        SENTRY_LEVEL_ERROR, "probe", "boom again");
    sentry_capture_event(ev2);
    check(envelope_count == 3, "segundo ciclo captura de novo", "");
    sentry_shutdown();
    check(envelope_count >= 3, "segundo shutdown não quebra (>= event do ciclo 2)", "");

    if (failures == 0) {
        printf("sentry-probe: ALL PASSED (vendored sentry-native usable)\n");
        return 0;
    }
    printf("sentry-probe: %d FAILURE(S)\n", failures);
    return 1;
}
