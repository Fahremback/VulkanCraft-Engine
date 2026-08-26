// AiEventBusTests — gate do contrato público do bus de eventos de IA (agente 4
// §3 item 1). Prova que o log de eventos é determinístico, all-or-nothing no
// load, bit-exact no round-trip, e que a ordem/capacidade/drenagem se
// comportam como documentado.

#include "engine/ai/IAiEventBus.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failures;
        std::cout << "FAIL: " << message << "\n";
    }
}

using engine::ai::AiEvent;
using engine::ai::AiEventBusSpec;
using engine::ai::create_ai_event_bus;

void test_validate_and_spec() {
    AiEventBusSpec spec;
    std::string err;
    check(spec.validate(err) && err.empty(), "spec default aceita");

    AiEventBusSpec bad;
    bad.max_events = -1;
    check(!bad.validate(err) && !err.empty(), "max_events negativo recusa");

    AiEventBusSpec loaded;
    check(loaded.load_from_json("{\"max_events\":64}", err) && err.empty(),
          "spec JSON carrega");
    check(loaded.to_json() == "{\"max_events\":64}", "spec round-trip bit-exact");
    check(!loaded.load_from_json("{bad", err) && !err.empty(), "spec JSON inválido recusa");
    check(loaded.to_json() == "{\"max_events\":64}", "recusa não muta");
}

void test_order_and_drain() {
    auto b = create_ai_event_bus();
    std::string err;
    check(b->configure(AiEventBusSpec{}, err), "configure");
    b->emit(0, "fsm", "state_changed", "{\"to\":\"chase\"}");
    b->emit(1, "perception", "threat_seen", "id=9");
    b->emit(1, "planner", "plan", "[\"gather\",\"craft\"]");

    const auto all = b->peek();
    check(all.size() == 3, "peek mantém todos");
    check(all[0].tick == 0 && all[0].source == "fsm" &&
              all[0].kind == "state_changed" && all[0].payload == "{\"to\":\"chase\"}",
          "evento 1 em ordem com payload íntegro");
    check(all[1].source == "perception" && all[2].source == "planner",
          "ordem de emissão preservada");

    const auto drained = b->drain();
    check(drained.size() == 3, "drain devolve tudo");
    check(b->peek().empty(), "drain limpa o log");
}

void test_capacity_fifo() {
    AiEventBusSpec spec;
    spec.max_events = 2;
    auto b = create_ai_event_bus();
    std::string err;
    check(b->configure(spec, err), "configure com cap 2");
    b->emit(0, "a", "k", "p0");
    b->emit(1, "a", "k", "p1");
    b->emit(2, "a", "k", "p2");  // estoura → descarta p0

    const auto all = b->peek();
    check(all.size() == 2, "cap mantém no máximo 2");
    check(all[0].payload == "p1" && all[1].payload == "p2",
          "FIFO: o mais antigo é descartado");
}

void test_serialize_roundtrip() {
    auto b = create_ai_event_bus();
    std::string err;
    check(b->configure(AiEventBusSpec{}, err), "configure");
    b->emit(0, "fsm", "state_changed", "{\"to\":\"chase\"}");
    b->emit(7, "utility", "selected", "attack");
    b->emit(7, "perception", "threat_seen", "id=\"9\"");

    const std::string state = b->serialize();
    auto c = create_ai_event_bus();
    check(c->configure(AiEventBusSpec{}, err), "configure c");
    check(c->deserialize(state, err) && err.empty(), "deserialize");
    check(c->serialize() == state, "round-trip bit-exact");
    check(c->peek() == b->peek(), "log restaurado idêntico");

    auto d = create_ai_event_bus();
    check(!d->deserialize("{bad", err) && !err.empty(), "estado inválido recusa");
    check(!d->deserialize("{\"events\":[{\"tick\":-1}]}", err) && !err.empty(),
          "tick negativo recusa");
    check(d->peek().empty(), "recusa não muta");
}

void test_determinism() {
    auto a = create_ai_event_bus();
    auto b = create_ai_event_bus();
    std::string err;
    check(a->configure(AiEventBusSpec{}, err), "configure a");
    check(b->configure(AiEventBusSpec{}, err), "configure b");
    for (std::uint64_t t = 0; t < 10; ++t) {
        a->emit(t, "fsm", "tick", "state=" + std::to_string(t % 3));
        b->emit(t, "fsm", "tick", "state=" + std::to_string(t % 3));
    }
    check(a->serialize() == b->serialize(), "determinismo: estados bit-exatos");
    check(a->peek() == b->peek(), "determinismo: logs idênticos");
}

}  // namespace

int main() {
    test_validate_and_spec();
    test_order_and_drain();
    test_capacity_fifo();
    test_serialize_roundtrip();
    test_determinism();

    if (g_failures == 0) {
        std::cout << "ai_event_bus_tests: all checks passed\n";
    } else {
        std::cout << "ai_event_bus_tests: " << g_failures << " failure(s)\n";
    }
    return g_failures == 0 ? 0 : 1;
}
