// NetworkRpcTests — gate do contrato INetworkRpc
// (engine::networking, §6 item 4 — "RPC" da rede pública).
// Prova: registro de procedimentos all-or-nothing (vazio/duplicado/nullptr),
// enqueue recusando procedure desconhecida/payload vazio sem mutar, drain
// destrutivo na ordem de enqueue com ack por sucesso, procedures ordenadas,
// JSON round-trip bit-exact, rejeição de documento inválido com estado
// intacto e determinismo cross-instance.

#include "engine/networking/INetworkRpc.hpp"

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

engine::networking::RpcResult ok_result(std::initializer_list<int> data) {
    engine::networking::RpcResult r;
    r.ok = true;
    for (const int value : data) r.data.push_back(static_cast<std::uint8_t>(value));
    return r;
}

engine::networking::RpcResult fail_result(const char* error) {
    engine::networking::RpcResult r;
    r.ok = false;
    r.error = error;
    return r;
}

void test_registration() {
    std::printf("[registration]\n");
    std::string err;
    auto session = engine::networking::create_network_rpc("rpc-1", err);
    check(session != nullptr, "sessão criada com id válido");
    check(session->session_id() == "rpc-1", "session_id preservado");

    check(session->register_procedure("ping", [](const std::vector<std::uint8_t>& p) {
        return ok_result({ static_cast<int>(p.size()) });
    }, err), "ping registrado");
    check(err.empty(), "sem erro no sucesso");
    check(session->procedures() == std::vector<std::string>({ "ping" }), "procedures listado");

    // All-or-nothing: vazio, duplicado, handler nulo — recusados sem mutar.
    check(!session->register_procedure("", [](const auto&) { return ok_result({}); }, err),
        "nome vazio recusado");
    check(session->procedures() == std::vector<std::string>({ "ping" }), "registro intacto após recusa vazio");
    check(!session->register_procedure("ping", [](const auto&) { return ok_result({}); }, err),
        "duplicado recusado");
    check(session->procedures() == std::vector<std::string>({ "ping" }), "registro intacto após duplicado");
    check(!session->register_procedure("nul", nullptr, err), "handler nulo recusado");

    session->unregister_procedure("ping");
    check(session->procedures().empty(), "unregister remove");
    session->unregister_procedure("ping");  // no-op, não crasha
    check(session->procedures().empty(), "unregister ausente é no-op");
}

void test_enqueue_drain() {
    std::printf("[enqueue/drain]\n");
    std::string err;
    auto session = engine::networking::create_network_rpc("rpc-2", err);
    session->register_procedure("add", [](const std::vector<std::uint8_t>& p) {
        std::uint8_t sum = 0;
        for (const std::uint8_t b : p) sum += b;
        return ok_result({ static_cast<int>(sum) });
    }, err);
    session->register_procedure("boom", [](const std::vector<std::uint8_t>&) {
        return fail_result("kaboom");
    }, err);

    // Recusas all-or-nothing: nada enfileira.
    check(!session->enqueue_call("nope", bytes({ 1 }), err), "procedure desconhecida recusada");
    check(session->pending_calls().empty(), "fila intacta após recusa");
    check(!session->enqueue_call("add", {}, err), "payload vazio recusado");
    check(session->pending_calls().empty(), "fila intacta após payload vazio");

    check(session->enqueue_call("add", bytes({ 1, 2 }), err), "call 1 enfileirada");
    check(session->enqueue_call("add", bytes({ 10 }), err), "call 2 enfileirada");
    check(session->enqueue_call("boom", bytes({ 5 }), err), "call 3 enfileirada (falha no handler)");
    check(session->pending_calls().size() == 3, "3 chamadas pendentes");
    check(session->pending_calls()[0].call_id == 1, "call_id 1 (sequência global)");
    check(session->pending_calls()[2].call_id == 3, "call_id 3");
    check(session->next_call_id() == 4, "next_call_id avança");

    auto results = session->drain(err);
    check(results.size() == 3, "drain devolve 3 resultados na ordem");
    check(results[0].ok && results[0].data == bytes({ 3 }), "add 1+2 = 3");
    check(results[1].ok && results[1].data == bytes({ 10 }), "add 10 = 10");
    check(!results[2].ok && results[2].error == "kaboom", "boom falha com erro");
    check(session->pending_calls().empty(), "drain é destrutivo (fila esvazia)");
    check(session->drain(err).empty(), "drain sem chamadas → vazio");
}

void test_persistence() {
    std::printf("[persistence]\n");
    std::string err;
    auto session = engine::networking::create_network_rpc("rpc-3", err);
    session->register_procedure("echo", [](const std::vector<std::uint8_t>& p) {
        return ok_result({ static_cast<int>(p[0]) });
    }, err);
    session->enqueue_call("echo", bytes({ 7 }), err);
    session->enqueue_call("echo", bytes({ 8 }), err);
    const std::string json = session->serialize_state();

    auto restored = engine::networking::create_network_rpc("rpc-3", err);
    check(restored->load_from_json(json, err), "load bit-exact");
    check(restored->serialize_state() == json, "re-serialize igual (round-trip bit-exact)");
    check(restored->pending_calls().size() == 2, "2 chamadas restauradas");
    check(restored->pending_calls()[0].call_id == 1 && restored->pending_calls()[1].call_id == 2,
        "ids restaurados na ordem");
    check(restored->next_call_id() == 3, "next_call_id restaurado");

    // Rejeições all-or-nothing: documento inválido deixa o estado anterior intacto.
    auto broken = engine::networking::create_network_rpc("rpc-3", err);
    check(!broken->load_from_json("not json", err), "not-json recusado");
    check(broken->pending_calls().empty(), "recusa sem estado anterior");
    // Duplicata de call_id dentro do documento → recusa.
    std::string dup = "{\"version\":1,\"session\":\"rpc-3\",\"next_call_id\":3,\"calls\":["
        "{\"call_id\":1,\"procedure\":\"echo\",\"payload\":[7],\"acked\":false},"
        "{\"call_id\":1,\"procedure\":\"echo\",\"payload\":[8],\"acked\":false}]}";
    check(!broken->load_from_json(dup, err), "call_id duplicado recusado");
    // Session mismatch → recusa.
    std::string other = "{\"version\":1,\"session\":\"other\",\"next_call_id\":2,\"calls\":[]}";
    check(!broken->load_from_json(other, err), "session mismatch recusado");
    // next_call_id ≤ último call → recusa.
    std::string badNext = "{\"version\":1,\"session\":\"rpc-3\",\"next_call_id\":1,\"calls\":["
        "{\"call_id\":1,\"procedure\":\"echo\",\"payload\":[7],\"acked\":false}]}";
    check(!broken->load_from_json(badNext, err), "next_call_id inválido recusado");
}

void test_determinism() {
    std::printf("[determinism]\n");
    std::string err;
    auto a = engine::networking::create_network_rpc("d", err);
    auto b = engine::networking::create_network_rpc("d", err);
    for (auto* s : { a.get(), b.get() }) {
        s->register_procedure("ping", [](const std::vector<std::uint8_t>&) { return ok_result({ 1 }); }, err);
        s->enqueue_call("ping", bytes({ 1 }), err);
        s->enqueue_call("ping", bytes({ 2 }), err);
    }
    check(a->serialize_state() == b->serialize_state(), "estado idêntico cross-instance");
    auto ra = a->drain(err);
    auto rb = b->drain(err);
    check(ra.size() == rb.size() && ra[0].ok == rb[0].ok && ra[1].data == rb[1].data,
        "drain idêntico cross-instance");
}

void test_reset() {
    std::printf("[reset]\n");
    std::string err;
    auto session = engine::networking::create_network_rpc("rpc-4", err);
    session->register_procedure("ping", [](const std::vector<std::uint8_t>&) { return ok_result({}); }, err);
    session->enqueue_call("ping", bytes({ 1 }), err);
    check(session->reset(err), "reset ok");
    check(session->procedures().empty() && session->pending_calls().empty(), "reset limpa tudo");
    check(session->next_call_id() == 1, "sequência reinicia");
}

}  // namespace

int main() {
    test_registration();
    test_enqueue_drain();
    test_persistence();
    test_determinism();
    test_reset();
    if (failures == 0) {
        std::printf("NetworkRpcTests: ALL PASSED\n");
        return 0;
    }
    std::printf("NetworkRpcTests: %d FAILURE(S)\n", failures);
    return 1;
}
