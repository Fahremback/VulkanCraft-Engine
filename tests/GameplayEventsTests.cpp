// GameplayEventsTests — gate do contrato IGameplayEvents (§5 item 65,
// hooks visuais/sonoros — CORE). Prova: FIFO estrito na ordem de
// publicação, drain parcial/total, capacity com evicção do mais antigo +
// contagem de drops, reset, ilimitado.

#include "engine/gameplay/IGameplayEvents.hpp"

#include <cstdio>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what);
        ++failures;
    }
}

std::vector<std::uint8_t> bytes(std::initializer_list<int> values) {
    std::vector<std::uint8_t> out;
    for (const int v : values) out.push_back(static_cast<std::uint8_t>(v));
    return out;
}

void test_fifo_order() {
    auto events = engine::gameplay::create_gameplay_events();
    events->publish(1, 10, bytes({1, 2}));
    events->publish(2, 11, bytes({3}));
    events->publish(3, 12, bytes({}));
    check(events->pending_count() == 3, "3 eventos pendentes");

    // Drain total: FIFO estrito.
    std::vector<engine::gameplay::GameplayEvent> drained = events->drain();
    check(drained.size() == 3, "drain total esvazia");
    check(drained[0].kind == 1 && drained[0].tick == 10 &&
              drained[0].payload == bytes({1, 2}),
          "evento 1 (ordem exata)");
    check(drained[1].kind == 2 && drained[1].tick == 11 &&
              drained[1].payload == bytes({3}),
          "evento 2 (ordem exata)");
    check(drained[2].kind == 3 && drained[2].tick == 12 &&
              drained[2].payload.empty(),
          "evento 3 (ordem exata)");
    check(events->pending_count() == 0, "fila vazia após drain");

    // Drain parcial: pega só os primeiros N.
    events->publish(5, 20, bytes({}));
    events->publish(6, 21, bytes({}));
    events->publish(7, 22, bytes({}));
    drained = events->drain(2);
    check(drained.size() == 2 && drained[0].kind == 5 && drained[1].kind == 6,
          "drain parcial mantém ordem");
    check(events->pending_count() == 1, "resta 1 após drain parcial");
}

void test_capacity_and_drops() {
    // Capacity 2: publica 4 → os 2 mais antigos caem.
    auto events = engine::gameplay::create_gameplay_events(2);
    events->publish(1, 10, bytes({}));
    events->publish(2, 11, bytes({}));
    events->publish(3, 12, bytes({}));  // drop do 1
    events->publish(4, 13, bytes({}));  // drop do 2
    check(events->pending_count() == 2, "capacity 2: 2 pendentes");
    check(events->dropped_count() == 2, "2 drops contados");
    const std::vector<engine::gameplay::GameplayEvent> drained = events->drain();
    check(drained.size() == 2 && drained[0].kind == 3 && drained[1].kind == 4,
          "evicção do mais antigo (3 e 4 restam)");

    // Ilimitado (capacity 0): nunca descarta.
    auto unlimited = engine::gameplay::create_gameplay_events();
    for (int i = 0; i < 100; ++i) {
        unlimited->publish(static_cast<std::uint16_t>(i),
                           static_cast<std::uint64_t>(i), bytes({}));
    }
    check(unlimited->pending_count() == 100, "ilimitado: 100 pendentes");
    check(unlimited->dropped_count() == 0, "ilimitado: 0 drops");
}

void test_reset() {
    auto events = engine::gameplay::create_gameplay_events(3);
    events->publish(1, 10, bytes({}));
    events->publish(2, 11, bytes({}));
    events->publish(3, 12, bytes({}));
    events->publish(4, 13, bytes({}));  // drop do 1
    check(events->dropped_count() == 1, "1 drop antes do reset");
    events->reset();
    check(events->pending_count() == 0 && events->dropped_count() == 0,
          "reset limpa fila e drops");
}

}  // namespace

int main() {
    test_fifo_order();
    test_capacity_and_drops();
    test_reset();

    if (failures == 0) {
        std::printf("gameplay_events_tests: all checks passed\n");
        return 0;
    }
    std::printf("gameplay_events_tests: %d failure(s)\n", failures);
    return 1;
}
