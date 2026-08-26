// NetworkReplicationTests — gate do contrato INetworkReplication
// (engine::networking, §1 item 3 — interfaces estáveis; §6 item 4 — snapshots).
// Prova: aplicação de frames exige ticks estritamente crescentes e ids únicos
// (all-or-nothing), último estado por entidade, ids ordenados, JSON round-trip
// bit-exact, rejeição de documento inválido com estado intacto e determinismo
// cross-instance.

#include "engine/networking/INetworkReplication.hpp"

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

std::vector<std::uint8_t> bytes(std::initializer_list<int> values) {
    std::vector<std::uint8_t> out;
    for (const int value : values) out.push_back(static_cast<std::uint8_t>(value));
    return out;
}

engine::networking::NetworkEntityState state(std::uint64_t id, const char* kind,
                                             std::vector<std::uint8_t> data) {
    engine::networking::NetworkEntityState st;
    st.entity_id = id;
    st.kind = kind;
    st.data = std::move(data);
    return st;
}

void test_apply_and_query() {
    std::printf("[apply/query]\n");
    std::string err;
    auto session = engine::networking::create_network_replication("match-1", err);
    check(session != nullptr, "sessão criada com id válido");
    check(session->session_id() == "match-1", "session_id preservado");

    engine::networking::ReplicationFrame f1;
    f1.tick = 10;
    f1.states = { state(1, "unit", bytes({ 1, 2, 3 })), state(2, "unit", bytes({ 9 })) };
    check(session->apply_frame(f1, err), "frame tick 10 aplicado");
    check(err.empty(), "sem erro no sucesso");

    const auto* s1 = session->state(1);
    check(s1 != nullptr && s1->kind == "unit" && s1->data == bytes({ 1, 2, 3 }), "estado da entidade 1");
    const auto* s2 = session->state(2);
    check(s2 != nullptr && s2->data == bytes({ 9 }), "estado da entidade 2");
    check(session->state(99) == nullptr, "entidade desconhecida → nullptr");
    check(session->tick_count() == 1, "tick_count 1");
    check(session->last_tick() == 10, "last_tick 10");
    check(session->entity_ids() == std::vector<std::uint64_t>({ 1, 2 }), "ids ordenados");

    // Substituição: mesmo id em tick maior sobrescreve.
    engine::networking::ReplicationFrame f2;
    f2.tick = 11;
    f2.states = { state(1, "unit", bytes({ 7 })) };
    check(session->apply_frame(f2, err), "frame tick 11 aplicado");
    check(session->state(1)->data == bytes({ 7 }), "entidade 1 sobrescrita");
    check(session->state(2)->data == bytes({ 9 }), "entidade 2 intacta");
    check(session->tick_count() == 2 && session->last_tick() == 11, "tick_count/last_tick após 2 frames");
}

void test_all_or_nothing() {
    std::printf("[all-or-nothing]\n");
    std::string err;
    auto session = engine::networking::create_network_replication("match-2", err);
    engine::networking::ReplicationFrame f1;
    f1.tick = 5;
    f1.states = { state(1, "unit", bytes({ 1 })) };
    check(session->apply_frame(f1, err), "frame base aplicado");

    // Tick não-estritamente crescente rejeitado, nada muda.
    engine::networking::ReplicationFrame bad;
    bad.tick = 5;  // igual ao último
    bad.states = { state(3, "unit", bytes({ 3 })) };
    check(!session->apply_frame(bad, err), "tick repetido rejeitado");
    check(err.find("ordem") != std::string::npos, "erro nomeia a ordem");
    check(session->state(3) == nullptr && session->tick_count() == 1, "estado intacto após rejeição");

    bad.tick = 4;  // decrescente
    check(!session->apply_frame(bad, err), "tick decrescente rejeitado");
    check(session->tick_count() == 1, "estado intacto (decrescente)");

    // Id duplicado no MESMO frame rejeitado all-or-nothing.
    engine::networking::ReplicationFrame dup;
    dup.tick = 6;
    dup.states = { state(1, "unit", bytes({ 1 })), state(1, "unit", bytes({ 2 })) };
    check(!session->apply_frame(dup, err), "id duplicado no frame rejeitado");
    check(err.find("duplicado") != std::string::npos, "erro nomeia a duplicação");
    check(session->state(1)->data == bytes({ 1 }) && session->last_tick() == 5, "nenhum estado do frame duplicado aplicado");

    // Kind vazio rejeitado.
    engine::networking::ReplicationFrame nokind;
    nokind.tick = 6;
    nokind.states = { state(4, "", bytes({ 1 })) };
    check(!session->apply_frame(nokind, err), "kind vazio rejeitado");
    check(session->state(4) == nullptr, "entidade de kind vazio não aplicada");
}

void test_session_creation_and_reset() {
    std::printf("[session/reset]\n");
    std::string err;
    check(engine::networking::create_network_replication("", err) == nullptr, "session_id vazio rejeitado");
    check(err.find("vazio") != std::string::npos, "erro nomeia o id vazio");

    auto session = engine::networking::create_network_replication("match-3", err);
    engine::networking::ReplicationFrame f1;
    f1.tick = 1;
    f1.states = { state(1, "unit", bytes({ 1 })) };
    check(session->apply_frame(f1, err), "frame aplicado");
    check(session->reset(err), "reset ok");
    check(session->entity_ids().empty() && session->tick_count() == 0 && session->last_tick() == 0,
          "reset limpa estado e ticks");
}

void test_json_round_trip() {
    std::printf("[json]\n");
    std::string err;
    auto a = engine::networking::create_network_replication("match-4", err);
    engine::networking::ReplicationFrame f1;
    f1.tick = 100;
    f1.states = { state(1, "unit", bytes({ 1, 2, 3 })),
                  state(2, "player", bytes({ 250, 0, 255 })) };
    check(a->apply_frame(f1, err), "frame aplicado");
    engine::networking::ReplicationFrame f2;
    f2.tick = 101;
    f2.states = { state(3, "unit", bytes({ 0 })) };
    check(a->apply_frame(f2, err), "frame 2 aplicado");

    const std::string json = a->serialize_state();
    check(json.find("\"session_id\":\"match-4\"") != std::string::npos, "session_id no JSON");
    check(json.find("\"last_tick\":101") != std::string::npos, "last_tick no JSON");
    check(json.find("\"data\":[250,0,255]") != std::string::npos, "bytes bit-exact no JSON");

    auto b = engine::networking::create_network_replication("other", err);
    check(b->load_from_json(json, err), "load ok");
    check(b->session_id() == "match-4", "session_id restaurado");
    check(b->last_tick() == 101 && b->tick_count() == 1, "last_tick/tick_count restaurados");
    check(b->state(1)->data == bytes({ 1, 2, 3 }) && b->state(2)->data == bytes({ 250, 0, 255 }),
          "estados restaurados bit-exact");
    check(b->state(3)->data == bytes({ 0 }), "entidade do frame 2 restaurada");
    check(b->serialize_state() == json, "round-trip byte-a-byte (serialize == serialize)");

    // Cross-instance determinismo: outra instância com os mesmos frames.
    auto c = engine::networking::create_network_replication("match-4", err);
    check(c->apply_frame(f1, err) && c->apply_frame(f2, err), "mesmos frames em c");
    check(c->serialize_state() == json, "determinismo cross-instance bit-exact");

    // Documento malformado rejeitado com estado intacto.
    auto d = engine::networking::create_network_replication("match-4", err);
    check(d->apply_frame(f1, err), "d com frame base");
    check(!d->load_from_json("{\"version\":1,\"session_id\":\"match-4\",\"last_tick\":101,\"entities\":[{\"id\":1,\"kind\":\"unit\",\"data\":[1]},{\"id\":1,\"kind\":\"unit\",\"data\":[2]}]}", err),
          "id duplicado no documento rejeitado");
    check(d->state(1)->data == bytes({ 1, 2, 3 }), "estado intacto após documento inválido");
    check(!d->load_from_json("{\"version\":1,\"last_tick\":1,\"entities\":[]}", err),
          "session_id ausente rejeitado");
    check(d->session_id() == "match-4", "session_id intacto após falha");
    check(!d->load_from_json("{\"version\":1,\"session_id\":\"x\",\"last_tick\":1,\"entities\":[{\"id\":1,\"kind\":\"\",\"data\":[]}]}", err),
          "kind vazio no documento rejeitado");
    check(d->state(1)->data == bytes({ 1, 2, 3 }), "estado intacto (kind vazio)");
    check(!d->load_from_json("{\"version\":2,\"session_id\":\"x\",\"last_tick\":1,\"entities\":[]}", err),
          "versão diferente rejeitada");
}

}  // namespace

int main() {
    test_apply_and_query();
    test_all_or_nothing();
    test_session_creation_and_reset();
    test_json_round_trip();

    if (failures == 0) {
        std::printf("network_replication_tests: all checks passed\n");
        return 0;
    }
    std::printf("network_replication_tests: %d failure(s)\n", failures);
    return 1;
}
