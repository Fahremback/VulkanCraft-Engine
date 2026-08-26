// NetworkInterestTests — gate do contrato INetworkInterest
// (engine::networking, §6 item 4 — "interest management" da rede pública).
// Prova: criação all-or-nothing (session vazia), set_observer com raio
// inválido recusado sem mutar, relevância por distância euclidiana <= raio
// (ou always_relevant), entidades desconhecidas nunca relevantes, resultados
// ordenados e determinísticos, JSON round-trip bit-exact, rejeição de
// documento inválido com estado intacto (duplicata/mismatch/raio negativo) e
// determinismo cross-instance.

#include "engine/networking/INetworkInterest.hpp"

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

engine::networking::NetworkPosition pos(double x, double y, double z) {
    engine::networking::NetworkPosition p;
    p.x = x;
    p.y = y;
    p.z = z;
    return p;
}

engine::networking::InterestObserver observer(std::uint64_t id, double x, double y, double z,
                                              double radius, bool always = false) {
    engine::networking::InterestObserver o;
    o.observer_id = id;
    o.position = pos(x, y, z);
    o.radius = radius;
    o.always_relevant = always;
    return o;
}

engine::networking::InterestEntity entity(std::uint64_t id, double x, double y, double z) {
    engine::networking::InterestEntity e;
    e.entity_id = id;
    e.position = pos(x, y, z);
    return e;
}

bool contains(const std::vector<std::uint64_t>& ids, std::uint64_t id) {
    for (const std::uint64_t v : ids) {
        if (v == id) return true;
    }
    return false;
}

void test_creation() {
    std::printf("[creation]\n");
    std::string err;
    auto session = engine::networking::create_network_interest("iv-1", err);
    check(session != nullptr, "sessão criada com id válido");
    check(session->session_id() == "iv-1", "session_id preservado");

    auto empty = engine::networking::create_network_interest("", err);
    check(empty == nullptr && !err.empty(), "session vazia recusada (all-or-nothing)");
}

void test_observers() {
    std::printf("[observers]\n");
    std::string err;
    auto session = engine::networking::create_network_interest("iv-2", err);
    check(session->set_observer(observer(7, 0, 0, 0, 10.0), err), "observer registrado");
    check(err.empty(), "sem erro no sucesso");
    check(session->set_observer(observer(3, 0, 0, 0, 5.0), err), "observer 3 registrado");

    // All-or-nothing: raio negativo recusado sem mutar.
    check(!session->set_observer(observer(9, 0, 0, 0, -1.0), err), "raio negativo recusado");
    check(session->observers().size() == 2, "registro intacto após recusa");
    check(session->observers()[0].observer_id == 3, "observers ordenado por id (3 primeiro)");
    check(session->observers()[1].observer_id == 7, "observers ordenado por id (7 depois)");

    // Atualização (mesmo id) sobrescreve sem duplicar.
    check(session->set_observer(observer(3, 1, 1, 1, 20.0), err), "atualização de observer");
    check(session->observers().size() == 2, "atualização não duplica");

    session->remove_observer(7);
    check(session->observers().size() == 1, "remove_observer remove");
    session->remove_observer(999);
    check(session->observers().size() == 1, "remove ausente é no-op");
}

void test_relevance() {
    std::printf("[relevance]\n");
    std::string err;
    auto session = engine::networking::create_network_interest("iv-3", err);
    // Observador em (0,0,0) com raio 10 — vê tudo até distância 10.
    session->set_observer(observer(1, 0, 0, 0, 10.0), err);
    // Observador com always_relevant — vê tudo, sem raio.
    session->set_observer(observer(2, 100, 100, 100, 0.0, true), err);
    // Observador com raio 0 — só o que estiver exatamente em cima dele.
    session->set_observer(observer(3, 5, 5, 5, 0.0), err);

    session->set_entity(entity(10, 3, 4, 0));     // dist 5 do obs 1 → visível
    session->set_entity(entity(11, 0, 0, 12));    // dist 12 do obs 1 → fora
    session->set_entity(entity(12, 5, 5, 5));     // dist 0 do obs 3 → visível
    session->set_entity(entity(13, 100, 101, 100));  // dist ~1.73 do obs 2 (always) → visível

    auto results = session->compute();
    check(results.size() == 3, "1 resultado por observador");
    check(results[0].observer_id == 1, "resultados ordenados por observer_id (1)");
    check(results[1].observer_id == 2, "resultados ordenados por observer_id (2)");
    check(results[2].observer_id == 3, "resultados ordenados por observer_id (3)");

    check(results[0].entity_ids.size() == 2 && results[0].entity_ids[0] == 10 &&
            results[0].entity_ids[1] == 12,
        "obs 1 vê as entidades a dist 5 e ~8.66 (raio 10), não a dist 12");
    check(results[1].entity_ids.size() == 4 && contains(results[1].entity_ids, 13),
        "obs always_relevant vê todas");
    check(results[1].entity_ids[0] == 10 && results[1].entity_ids[3] == 13,
        "entity_ids em ordem crescente (obs always)");
    check(results[2].entity_ids.size() == 1 && results[2].entity_ids[0] == 12,
        "obs 3 (raio 0) vê só a que está em cima dele");

    // Entidade sem posição registrada nunca é relevante: removemos a 10 e ela
    // deixa de aparecer para o observador 1.
    session->remove_entity(10);
    auto results2 = session->compute();
    check(!contains(results2[0].entity_ids, 10) && contains(results2[0].entity_ids, 12),
        "entidade removida deixa de ser relevante");
}

void test_persistence() {
    std::printf("[persistence]\n");
    std::string err;
    auto session = engine::networking::create_network_interest("iv-4", err);
    session->set_observer(observer(1, 0, 0, 0, 2.5), err);
    session->set_observer(observer(2, 1, 1, 1, 0.0, true), err);
    session->set_entity(entity(50, 0.1, 0.2, 0.3));
    const std::string json = session->serialize_state();

    auto restored = engine::networking::create_network_interest("iv-4", err);
    check(restored->load_from_json(json, err), "load bit-exact");
    check(restored->serialize_state() == json, "re-serialize igual (round-trip bit-exact)");
    check(restored->observers().size() == 2 && restored->entities().size() == 1,
        "estado restaurado completo");
    check(restored->observers()[0].radius == 2.5, "raio restaurado com precisão");

    // Rejeições all-or-nothing: documento inválido deixa o estado anterior intacto.
    auto broken = engine::networking::create_network_interest("iv-4", err);
    check(!broken->load_from_json("not json", err), "not-json recusado");
    check(broken->observers().empty(), "recusa sem estado anterior");
    // Session mismatch → recusa.
    std::string other = "{\"version\":1,\"session\":\"other\",\"observers\":[],\"entities\":[]}";
    check(!broken->load_from_json(other, err), "session mismatch recusado");
    // Observer com raio negativo dentro do documento → recusa.
    std::string badRadius = "{\"version\":1,\"session\":\"iv-4\",\"observers\":[{\"observer_id\":1,"
        "\"position\":{\"x\":0,\"y\":0,\"z\":0},\"radius\":-3,\"always_relevant\":false}],\"entities\":[]}";
    check(!broken->load_from_json(badRadius, err), "raio negativo no documento recusado");
    // Duplicata de observer_id dentro do documento → recusa.
    std::string dup = "{\"version\":1,\"session\":\"iv-4\",\"observers\":[{\"observer_id\":1,"
        "\"position\":{\"x\":0,\"y\":0,\"z\":0},\"radius\":1,\"always_relevant\":false},"
        "{\"observer_id\":1,\"position\":{\"x\":0,\"y\":0,\"z\":0},\"radius\":1,\"always_relevant\":false}],"
        "\"entities\":[]}";
    check(!broken->load_from_json(dup, err), "observer_id duplicado recusado");
    // Faltando campo obrigatório → recusa.
    std::string missing = "{\"version\":1,\"session\":\"iv-4\",\"observers\":[{\"observer_id\":1,"
        "\"position\":{\"x\":0,\"y\":0,\"z\":0},\"radius\":1}],\"entities\":[]}";
    check(!broken->load_from_json(missing, err), "observer incompleto recusado");
}

void test_determinism() {
    std::printf("[determinism]\n");
    std::string err;
    auto a = engine::networking::create_network_interest("d", err);
    auto b = engine::networking::create_network_interest("d", err);
    for (auto* s : { a.get(), b.get() }) {
        s->set_observer(observer(2, 0, 0, 0, 5.0), err);
        s->set_observer(observer(1, 10, 10, 10, 5.0), err);
        s->set_entity(entity(3, 2, 0, 0));
        s->set_entity(entity(1, 100, 0, 0));
    }
    check(a->serialize_state() == b->serialize_state(), "estado idêntico cross-instance");
    auto ra = a->compute();
    auto rb = b->compute();
    check(ra.size() == rb.size(), "compute idêntico cross-instance (tamanho)");
    check(ra[0].observer_id == rb[0].observer_id && ra[0].entity_ids == rb[0].entity_ids,
        "compute idêntico cross-instance (conteúdo)");
}

void test_reset() {
    std::printf("[reset]\n");
    std::string err;
    auto session = engine::networking::create_network_interest("iv-5", err);
    session->set_observer(observer(1, 0, 0, 0, 5.0), err);
    session->set_entity(entity(1, 0, 0, 0));
    check(session->reset(err), "reset ok");
    check(session->observers().empty() && session->entities().empty(), "reset limpa tudo");
}

}  // namespace

int main() {
    test_creation();
    test_observers();
    test_relevance();
    test_persistence();
    test_determinism();
    test_reset();
    if (failures == 0) {
        std::printf("NetworkInterestTests: ALL PASSED\n");
        return 0;
    }
    std::printf("NetworkInterestTests: %d FAILURE(S)\n", failures);
    return 1;
}
