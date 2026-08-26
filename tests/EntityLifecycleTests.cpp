// EntityLifecycleTests — gate do contrato IEntityLifecycle (§1 item 14,
// ciclo de vida CORE): prova a máquina Despawned/Active/Sleeping, pooling
// (capacidade limita spawn), transições inválidas recusadas, flags de
// persistência, dirty/checkpoint e round-trip JSON all-or-nothing.

#include "engine/entity/IEntityLifecycle.hpp"

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
    bool ok = actual.size() == expected.size();
    if (ok) {
        for (std::size_t i = 0; i < actual.size(); ++i) {
            if (actual[i] != expected[i]) ok = false;
        }
    }
    check(ok, what);
    return ok;
}

void test_state_machine() {
    auto lifecycle = engine::entity::create_entity_lifecycle();
    std::string error;
    check(lifecycle->configure(4, error), "configure pool 4");

    check(lifecycle->register_entity(10, error) && lifecycle->register_entity(20, error) &&
              lifecycle->register_entity(30, error),
          "register 10/20/30");
    check(lifecycle->state(10) == engine::entity::LifecycleState::Despawned,
          "recém-registrada: Despawned");

    check(lifecycle->spawn(10), "spawn 10");
    check(lifecycle->spawn(20), "spawn 20");
    check(lifecycle->state(10) == engine::entity::LifecycleState::Active, "10 Active");
    check(lifecycle->pool_used() == 2, "pool usado 2/4");

    check(lifecycle->sleep(10), "sleep 10");
    check(lifecycle->state(10) == engine::entity::LifecycleState::Sleeping, "10 Sleeping");
    check(!lifecycle->sleep(10), "sleep de Sleeping recusa");
    check(lifecycle->wake(10), "wake 10");
    check(lifecycle->state(10) == engine::entity::LifecycleState::Active, "10 Active de novo");
    check(!lifecycle->wake(10), "wake de Active recusa");

    check(lifecycle->despawn(10), "despawn 10");
    check(lifecycle->state(10) == engine::entity::LifecycleState::Despawned, "10 Despawned");
    check(!lifecycle->despawn(10), "despawn de Despawned recusa");
    check(lifecycle->pool_used() == 1, "pool liberado (1/4)");

    // Transições de não-registrada recusam.
    check(!lifecycle->spawn(99), "spawn de não-registrada recusa");
    check(!lifecycle->sleep(99) && !lifecycle->wake(99) && !lifecycle->despawn(99),
          "transições de não-registrada recusam");
}

void test_pooling() {
    auto lifecycle = engine::entity::create_entity_lifecycle();
    std::string error;
    check(lifecycle->configure(2, error), "configure pool 2");
    check(lifecycle->register_entity(1, error) && lifecycle->register_entity(2, error) &&
              lifecycle->register_entity(3, error),
          "register 1/2/3");
    check(lifecycle->spawn(1) && lifecycle->spawn(2), "spawn 1 e 2 (pool cheio)");
    check(!lifecycle->spawn(3), "spawn 3 recusa (pool cheio)");
    check(lifecycle->state(3) == engine::entity::LifecycleState::Despawned,
          "3 continua Despawned");
    check(lifecycle->despawn(1), "despawn 1");
    check(lifecycle->spawn(3), "spawn 3 (slot liberado)");
    check(lifecycle->pool_used() == 2, "pool 2/2 de novo");

    ids_equal(lifecycle->by_state(engine::entity::LifecycleState::Active), { 2, 3 },
              "by_state Active → {2,3} ordenado");
}

void test_persistence_dirty() {
    auto lifecycle = engine::entity::create_entity_lifecycle();
    std::string error;
    check(lifecycle->configure(4, error), "configure");
    check(lifecycle->register_entity(5, error) && lifecycle->register_entity(6, error),
          "register 5/6");
    check(lifecycle->spawn(5), "spawn 5");

    // Registrar e spawn são mudanças (dirty).
    ids_equal(lifecycle->dirty(), { 5, 6 }, "dirty após registros");
    lifecycle->mark_checkpoint();
    check(lifecycle->dirty().empty(), "checkpoint limpa dirty");

    check(lifecycle->set_persistent(5, true), "set_persistent 5");
    ids_equal(lifecycle->dirty(), { 5 }, "mudança de flag → dirty");
    lifecycle->mark_checkpoint();

    check(lifecycle->set_persistent(5, true), "set_persistent 5 (mesmo valor)");
    check(lifecycle->dirty().empty(), "flag inalterada não suja");
    check(lifecycle->is_persistent(5) && !lifecycle->is_persistent(6),
          "flags de persistência");

    // Persistência sobrevive ao despawn.
    check(lifecycle->despawn(5), "despawn 5");
    ids_equal(lifecycle->dirty(), { 5 }, "despawn suja");
    check(lifecycle->is_persistent(5), "flag persistente preservada no despawn");
}

void test_json() {
    auto a = engine::entity::create_entity_lifecycle();
    auto b = engine::entity::create_entity_lifecycle();
    std::string error;
    check(a->configure(3, error), "configure A pool 3");
    check(a->register_entity(1, error) && a->register_entity(2, error), "register A");
    check(a->spawn(1), "spawn A 1");
    check(a->sleep(1), "sleep A 1");
    check(a->set_persistent(2, true), "persistent A 2");

    check(b->load_from_json(a->to_json(), error), "load B do estado de A");
    check(b->to_json() == a->to_json(), "round-trip bit-exact");
    check(b->state(1) == engine::entity::LifecycleState::Sleeping &&
              b->is_persistent(2) && b->pool_capacity() == 3,
          "estado restaurado (sleeping, persistent, capacidade)");
    check(b->pool_used() == 1, "pool usado restaurado");

    check(!b->load_from_json(R"({"version":2,"poolCapacity":3,"entities":[]})", error),
          "versão 2 recusa");
    check(!b->load_from_json(R"({"version":1,"poolCapacity":3,"entities":[{"id":1,"state":"ghost"}]})", error),
          "estado desconhecido recusa");
    check(!b->load_from_json(R"({"version":1,"poolCapacity":1,"entities":[{"id":7,"state":"active"},{"id":8,"state":"active"}]})", error),
          "estado excede a capacidade do pool recusa");
    check(b->to_json() == a->to_json(), "estado intacto após recusas JSON");

    auto bad = engine::entity::create_entity_lifecycle();
    check(!bad->configure(0, error), "configure 0 recusa");
}

}  // namespace

int main() {
    test_state_machine();
    test_pooling();
    test_persistence_dirty();
    test_json();

    if (failures == 0) {
        std::printf("entity_lifecycle_tests: all checks passed\n");
        return 0;
    }
    std::printf("entity_lifecycle_tests: %d failure(s)\n", failures);
    return 1;
}
