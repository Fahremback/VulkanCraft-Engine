// NetworkDiscoveryTests — gate do contrato INetworkDiscovery
// (engine::networking, §6 item 4 — "discovery" da rede pública).
// Prova: criação all-or-nothing (session vazia), register com tipo/endpoint
// vazio recusado sem mutar, saúde derivada de falhas consecutivas (probe ok
// zera, falha incrementa, healthy só com 0 falhas), resolve por tipo retorna
// SÓ os saudáveis em ordem crescente, unregister no-op, JSON round-trip
// bit-exact, rejeição de documento inválido com estado intacto
// (duplicata/mismatch/healthy↔failures inconsistente) e determinismo
// cross-instance.

#include "engine/networking/INetworkDiscovery.hpp"

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

engine::networking::DiscoveryService svc(std::uint64_t id, const char* type, const char* endpoint) {
    engine::networking::DiscoveryService s;
    s.service_id = id;
    s.type = type;
    s.endpoint = endpoint;
    s.consecutive_failures = 0;
    s.healthy = true;
    return s;
}

void test_creation() {
    std::printf("[creation]\n");
    std::string err;
    auto session = engine::networking::create_network_discovery("disc-1", err);
    check(session != nullptr, "sessão criada com id válido");
    check(session->session_id() == "disc-1", "session_id preservado");

    auto empty = engine::networking::create_network_discovery("", err);
    check(empty == nullptr && !err.empty(), "session vazia recusada (all-or-nothing)");
}

void test_register() {
    std::printf("[register]\n");
    std::string err;
    auto session = engine::networking::create_network_discovery("disc-2", err);
    check(session->register_service(svc(1, "gameplay", "tcp://10.0.0.1:7001"), err),
        "serviço 1 registrado");
    check(err.empty(), "sem erro no sucesso");
    check(session->register_service(svc(5, "replication", "tcp://10.0.0.5:7002"), err),
        "serviço 5 registrado");

    // All-or-nothing: tipo/endpoint vazio recusados sem mutar.
    auto bad = svc(9, "", "tcp://x:1");
    check(!session->register_service(bad, err), "tipo vazio recusado");
    bad.type = "gameplay";
    bad.endpoint = "";
    check(!session->register_service(bad, err), "endpoint vazio recusado");
    check(session->services().size() == 2, "registro intacto após recusas");

    check(session->services()[0].service_id == 1, "services ordenado por id (1 primeiro)");
    check(session->services()[1].service_id == 5, "services ordenado por id (5 depois)");

    // Atualização (mesmo id) sobrescreve sem duplicar.
    auto updated = svc(1, "gameplay", "tcp://10.0.0.1:7101");
    check(session->register_service(updated, err), "atualização de serviço");
    check(session->services().size() == 2, "atualização não duplica");
    check(session->services()[0].endpoint == "tcp://10.0.0.1:7101", "endpoint atualizado");

    session->unregister_service(5);
    check(session->services().size() == 1, "unregister remove");
    session->unregister_service(999);
    check(session->services().size() == 1, "unregister ausente é no-op");
}

void test_health() {
    std::printf("[health]\n");
    std::string err;
    auto session = engine::networking::create_network_discovery("disc-3", err);
    session->register_service(svc(1, "gameplay", "tcp://a:1"), err);
    session->register_service(svc(2, "gameplay", "tcp://b:2"), err);
    session->register_service(svc(3, "replication", "tcp://c:3"), err);

    // Tudo saudável no início → resolve retorna todos do tipo, ordenados.
    auto all = session->resolve("gameplay");
    check(all.size() == 2 && all[0].service_id == 1 && all[1].service_id == 2,
        "resolve retorna os 2 gameplay saudáveis em ordem");

    // Falha no probe → incrementa falhas consecutivas e derruba healthy.
    session->report_health(1, false);
    auto afterFail = session->resolve("gameplay");
    check(afterFail.size() == 1 && afterFail[0].service_id == 2,
        "resolve exclui o serviço com falha consecutiva");
    check(session->services()[0].consecutive_failures == 1 && !session->services()[0].healthy,
        "falha incrementa contador e derruba healthy");

    // Falha de novo → contador sobe; serviço 3 (outro tipo) intocado.
    session->report_health(1, false);
    check(session->services()[0].consecutive_failures == 2, "2ª falha incrementa de novo");
    check(session->services()[2].healthy, "outro tipo continua saudável");

    // Probe ok → zera contador e restaura healthy; resolve volta a incluí-lo.
    session->report_health(1, true);
    check(session->services()[0].consecutive_failures == 0 && session->services()[0].healthy,
        "probe ok zera contador e restaura healthy");
    auto restored = session->resolve("gameplay");
    check(restored.size() == 2, "resolve volta a incluir o serviço recuperado");

    // Tipo desconhecido → vazio.
    check(session->resolve("nope").empty(), "tipo desconhecido → vazio");
    // report_health em serviço desconhecido é no-op (não crasha).
    session->report_health(999, false);
    check(session->services().size() == 3, "probe de serviço desconhecido é no-op");
}

void test_persistence() {
    std::printf("[persistence]\n");
    std::string err;
    auto session = engine::networking::create_network_discovery("disc-4", err);
    session->register_service(svc(1, "gameplay", "tcp://a:1"), err);
    session->register_service(svc(2, "replication", "tcp://b:2"), err);
    session->report_health(2, false);
    const std::string json = session->serialize_state();

    auto restored = engine::networking::create_network_discovery("disc-4", err);
    check(restored->load_from_json(json, err), "load bit-exact");
    check(restored->serialize_state() == json, "re-serialize igual (round-trip bit-exact)");
    check(restored->services().size() == 2, "estado restaurado completo");
    check(restored->services()[1].consecutive_failures == 1 && !restored->services()[1].healthy,
        "saúde restaurada (falha + unhealthy)");

    // Rejeições all-or-nothing: documento inválido deixa o estado anterior intacto.
    auto broken = engine::networking::create_network_discovery("disc-4", err);
    check(!broken->load_from_json("not json", err), "not-json recusado");
    check(broken->services().empty(), "recusa sem estado anterior");
    // Session mismatch → recusa.
    std::string other = "{\"version\":1,\"session\":\"other\",\"services\":[]}";
    check(!broken->load_from_json(other, err), "session mismatch recusado");
    // healthy=true com falhas > 0 → inconsistência recusada.
    std::string badHealth = "{\"version\":1,\"session\":\"disc-4\",\"services\":[{\"service_id\":1,"
        "\"type\":\"gameplay\",\"endpoint\":\"tcp://a:1\",\"consecutive_failures\":2,\"healthy\":true}]}";
    check(!broken->load_from_json(badHealth, err), "healthy com falhas recusado");
    // healthy=false com 0 falhas → inconsistência recusada.
    std::string badHealth2 = "{\"version\":1,\"session\":\"disc-4\",\"services\":[{\"service_id\":1,"
        "\"type\":\"gameplay\",\"endpoint\":\"tcp://a:1\",\"consecutive_failures\":0,\"healthy\":false}]}";
    check(!broken->load_from_json(badHealth2, err), "unhealthy com zero falhas recusado");
    // Duplicata de service_id dentro do documento → recusa.
    std::string dup = "{\"version\":1,\"session\":\"disc-4\",\"services\":[{\"service_id\":1,"
        "\"type\":\"gameplay\",\"endpoint\":\"tcp://a:1\",\"consecutive_failures\":0,\"healthy\":true},"
        "{\"service_id\":1,\"type\":\"gameplay\",\"endpoint\":\"tcp://a:1\",\"consecutive_failures\":0,\"healthy\":true}]}";
    check(!broken->load_from_json(dup, err), "service_id duplicado recusado");
    // Faltando campo obrigatório → recusa.
    std::string missing = "{\"version\":1,\"session\":\"disc-4\",\"services\":[{\"service_id\":1,"
        "\"type\":\"gameplay\",\"consecutive_failures\":0,\"healthy\":true}]}";
    check(!broken->load_from_json(missing, err), "serviço incompleto recusado");
}

void test_determinism() {
    std::printf("[determinism]\n");
    std::string err;
    auto a = engine::networking::create_network_discovery("d", err);
    auto b = engine::networking::create_network_discovery("d", err);
    for (auto* s : { a.get(), b.get() }) {
        s->register_service(svc(2, "gameplay", "tcp://b:2"), err);
        s->register_service(svc(1, "replication", "tcp://a:1"), err);
        s->register_service(svc(3, "gameplay", "tcp://c:3"), err);
        s->report_health(3, false);
    }
    check(a->serialize_state() == b->serialize_state(), "estado idêntico cross-instance");
    auto ra = a->resolve("gameplay");
    auto rb = b->resolve("gameplay");
    check(ra.size() == rb.size() && ra[0].service_id == rb[0].service_id,
        "resolve idêntico cross-instance");
}

void test_reset() {
    std::printf("[reset]\n");
    std::string err;
    auto session = engine::networking::create_network_discovery("disc-5", err);
    session->register_service(svc(1, "gameplay", "tcp://a:1"), err);
    check(session->reset(err), "reset ok");
    check(session->services().empty(), "reset limpa tudo");
    check(session->resolve("gameplay").empty(), "resolve vazio após reset");
}

}  // namespace

int main() {
    test_creation();
    test_register();
    test_health();
    test_persistence();
    test_determinism();
    test_reset();
    if (failures == 0) {
        std::printf("NetworkDiscoveryTests: ALL PASSED\n");
        return 0;
    }
    std::printf("NetworkDiscoveryTests: %d FAILURE(S)\n", failures);
    return 1;
}
