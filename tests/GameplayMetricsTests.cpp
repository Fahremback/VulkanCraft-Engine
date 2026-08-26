// GameplayMetricsTests — gate do contrato IGameplayMetrics (§2 item 30,
// métricas públicas CORE): prova Counter (soma), Gauge (último), Sample
// (média/min/max), snapshot em ordem crescente de nome, recusas
// all-or-nothing sem mutar, reset (preservando o kind), JSON determinístico
// e determinismo cross-instance.

#include "engine/gameplay/IGameplayMetrics.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

bool near(double a, double b, double eps = 1.0e-9) {
    return (a > b - eps) && (a < b + eps);
}

const engine::gameplay::GameplayMetric* find_metric(
    const std::vector<engine::gameplay::GameplayMetric>& metrics,
    const std::string& name) {
    for (const auto& metric : metrics) {
        if (metric.name == name) return &metric;
    }
    return nullptr;
}

void test_kinds() {
    auto metrics = engine::gameplay::create_gameplay_metrics();
    std::string error;
    check(metrics->register_metric("kills", engine::gameplay::GameplayMetricKind::Counter, error),
          "register counter 'kills'");
    check(metrics->register_metric("hp", engine::gameplay::GameplayMetricKind::Gauge, error),
          "register gauge 'hp'");
    check(metrics->register_metric("latency", engine::gameplay::GameplayMetricKind::Sample, error),
          "register sample 'latency'");

    check(metrics->record("kills", 1.0, error) && metrics->record("kills", 2.0, error) &&
              metrics->record("kills", 3.0, error),
          "counter: 3 records");
    check(metrics->record("hp", 80.0, error) && metrics->record("hp", 45.0, error) &&
              metrics->record("hp", 12.0, error),
          "gauge: 3 records");
    check(metrics->record("latency", 2.0, error) && metrics->record("latency", 4.0, error) &&
              metrics->record("latency", 6.0, error),
          "sample: 3 records");

    const auto snapshot = metrics->snapshot();
    const auto* kills = find_metric(snapshot, "kills");
    const auto* hp = find_metric(snapshot, "hp");
    const auto* latency = find_metric(snapshot, "latency");
    check(kills != nullptr && kills->value == 6.0 && kills->count == 3,
          "counter soma 1+2+3 = 6");
    check(hp != nullptr && hp->value == 12.0 && hp->count == 3,
          "gauge = último (12)");
    check(latency != nullptr && latency->count == 3 && near(latency->value, 4.0) &&
              latency->minValue == 2.0 && latency->maxValue == 6.0,
          "sample média 4, min 2, max 6");

    // Snapshot ordenado por nome (determinístico).
    bool sorted = true;
    for (std::size_t i = 1; i < snapshot.size(); ++i) {
        if (snapshot[i - 1].name > snapshot[i].name) sorted = false;
    }
    check(sorted, "snapshot em ordem crescente de nome");
}

void test_refusals() {
    auto metrics = engine::gameplay::create_gameplay_metrics();
    std::string error;
    check(!metrics->register_metric("", engine::gameplay::GameplayMetricKind::Counter, error),
          "nome vazio recusa");
    check(metrics->register_metric("a", engine::gameplay::GameplayMetricKind::Counter, error),
          "register 'a'");
    check(!metrics->register_metric("a", engine::gameplay::GameplayMetricKind::Gauge, error),
          "duplicata recusa");
    check(!metrics->record("ghost", 1.0, error), "record de não-registrada recusa");
    check(!metrics->record("a", std::nan(""), error), "valor NaN recusa");

    // Valor infinito via bit pattern (guard /fp:fast, findings #79).
    double inf = 0.0;
    const std::uint64_t posInf = 0x7ff0000000000000ull;
    std::memcpy(&inf, &posInf, sizeof(inf));
    check(!metrics->record("a", inf, error), "valor +inf recusa");
    check(metrics->snapshot().size() == 1, "fila intacta após recusas");
    check(metrics->snapshot()[0].value == 0.0 && metrics->snapshot()[0].count == 0,
          "métrica 'a' não mutada");
}

void test_reset_and_json() {
    auto metrics = engine::gameplay::create_gameplay_metrics();
    std::string error;
    check(metrics->register_metric("c", engine::gameplay::GameplayMetricKind::Counter, error) &&
              metrics->register_metric("g", engine::gameplay::GameplayMetricKind::Gauge, error) &&
              metrics->register_metric("s", engine::gameplay::GameplayMetricKind::Sample, error),
          "register c/g/s");
    metrics->record("c", 5.0, error);
    metrics->record("g", 7.0, error);
    metrics->record("s", 3.0, error);
    metrics->record("s", 5.0, error);

    check(metrics->reset("c"), "reset 'c'");
    check(!metrics->reset("ghost"), "reset de desconhecida → false");
    const auto afterReset = metrics->snapshot();
    const auto* c = find_metric(afterReset, "c");
    check(c != nullptr && c->value == 0.0 && c->count == 0 &&
              c->kind == engine::gameplay::GameplayMetricKind::Counter,
          "reset zera e PRESERVA o kind (counter)");
    const auto* s = find_metric(afterReset, "s");
    check(s != nullptr && s->count == 2, "sample segue com seus registros");

    metrics->record("s", 4.0, error);
    metrics->reset_all();
    const auto all = metrics->snapshot();
    check(all.size() == 3, "reset_all mantém as 3 métricas");
    bool zeroed = true;
    for (const auto& metric : all) {
        if (metric.count != 0 || metric.value != 0.0) zeroed = false;
    }
    check(zeroed, "reset_all zera valores e contagens");

    metrics->record("s", 4.0, error);
    const std::string json = metrics->to_json();
    check(json.find("\"name\":\"s\"") != std::string::npos &&
              json.find("\"kind\":\"sample\"") != std::string::npos,
          "JSON contém a métrica sample");
    check(json.find("\"count\":1") != std::string::npos, "JSON contém count");
}

void test_determinism() {
    auto a = engine::gameplay::create_gameplay_metrics();
    auto b = engine::gameplay::create_gameplay_metrics();
    std::string error;
    for (int i = 0; i < 3; ++i) {
        a->register_metric("m" + std::to_string(i),
                           engine::gameplay::GameplayMetricKind::Sample, error);
        b->register_metric("m" + std::to_string(i),
                           engine::gameplay::GameplayMetricKind::Sample, error);
    }
    // Registra em ordem DIFERENTE em b (map ordena de qualquer forma).
    b->record("m2", 1.0, error);
    b->record("m0", 2.0, error);
    b->record("m1", 3.0, error);
    a->record("m0", 2.0, error);
    a->record("m1", 3.0, error);
    a->record("m2", 1.0, error);
    check(a->to_json() == b->to_json(), "to_json determinístico (ordem de registro irrelevante)");
    const auto sa = a->snapshot();
    const auto sb = b->snapshot();
    check(sa.size() == sb.size() && sa[0].name == "m0" && sa[2].name == "m2",
          "snapshot ordenado por nome em ambos");
}

}  // namespace

int main() {
    test_kinds();
    test_refusals();
    test_reset_and_json();
    test_determinism();

    if (failures == 0) {
        std::printf("gameplay_metrics_tests: all checks passed\n");
        return 0;
    }
    std::printf("gameplay_metrics_tests: %d failure(s)\n", failures);
    return 1;
}
