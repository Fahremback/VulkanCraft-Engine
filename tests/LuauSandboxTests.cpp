// LuauSandboxTests — gate do contrato ILuauSandbox (engine::scripting,
// §3 item 6 — "Integrar runtime Luau sandboxed opcional com bindings gerados
// e budgets de CPU/memória"). O motor real (Luau vendido) é provado à parte
// (#302-luau-probe); aqui o contrato é exercitado com um runner determinístico
// (mesmo espírito do ISignatureVerifier fake nos testes de packaging).
// Prova: criação all-or-nothing (id vazio / política inválida), attach_runner
// nulo recusado, avaliação sem runner recusada sem mutar, configure
// all-or-nothing (máximos < 1 / allow_require recusados com estado intacto),
// budget enforceado (runner que estoura o teto → erro com tag "budget" e
// contador de execuções NÃO incrementa), args_json inválido recusado, erro de
// runtime com tag estável, valor de sucesso validado como JSON, chamada de
// função com argumentos JSON, determinismo cross-instance, reset, JSON
// round-trip bit-exact e rejeições all-or-nothing com estado intacto
// (sandbox_id mismatch / política inválida / contador não-crescente / campo
// desconhecido / trailing).

#include "engine/scripting/ILuauSandbox.hpp"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what);
        ++failures;
    }
}

// Runner determinístico: o "custo" de cada script é declarado no próprio
// source ("{n}" no início). Simula sucesso (devolve o valor que vier em
// "=>"), estouro de budget (custo > teto → ok=false, tag "budget"), erro de
// runtime ("!! msg" → ok=false, tag "runtime") e violação de sandbox
// ("io." no source → ok=false, tag "sandbox").
struct DeterministicRunner final : engine::scripting::IScriptRunner {
    engine::scripting::ScriptResult run(const std::string& source,
                                        const std::string& entry,
                                        const std::string& args_json,
                                        std::uint32_t budget,
                                        std::uint32_t depth,
                                        std::string& errorOut) override {
        engine::scripting::ScriptResult r;
        std::uint64_t cost = 1;
        if (source.size() >= 2 && source[0] == '{') {
            const std::size_t close = source.find('}');
            if (close != std::string::npos) {
                cost = static_cast<std::uint64_t>(std::stoull(source.substr(1, close - 1)));
            }
        }
        r.instructions_used = cost;
        if (source.find("io.") != std::string::npos) {
            r.ok = false;
            r.error = "sandbox: io is not allowed";
            errorOut.clear();
            return r;
        }
        if (cost > budget) {
            r.ok = false;
            r.error = "budget exceeded";
            errorOut.clear();
            return r;
        }
        const std::size_t arrow = source.find("=>");
        if (arrow != std::string::npos) {
            r.ok = true;
            r.value = source.substr(arrow + 2);
        } else if (source.find("!!") != std::string::npos) {
            r.ok = false;
            r.error = "runtime: boom";
            errorOut.clear();
            return r;
        } else {
            r.ok = true;
            r.value = "{}";
        }
        errorOut.clear();
        return r;
    }
};

void test_creation() {
    std::printf("[creation]\n");
    std::string err;
    DeterministicRunner runner;
    engine::scripting::SandboxPolicy policy;
    auto sb = engine::scripting::create_luau_sandbox("sb-1", &runner, policy, err);
    check(sb != nullptr, "sandbox criada com id + runner + política default");
    check(sb->sandbox_id() == "sb-1", "sandbox_id preservado");
    check(sb->executions() == 0, "nasce zerada");

    auto empty = engine::scripting::create_luau_sandbox("", &runner, policy, err);
    check(empty == nullptr, "id vazio recusado (all-or-nothing)");
    check(!err.empty(), "erro nomeado no id vazio");

    engine::scripting::SandboxPolicy bad;
    bad.max_instructions = 0;
    auto bad_policy = engine::scripting::create_luau_sandbox("sb-x", &runner, bad, err);
    check(bad_policy == nullptr, "política com max_instructions=0 recusada");
    bad = engine::scripting::SandboxPolicy{};
    bad.max_call_depth = 0;
    auto bad_depth = engine::scripting::create_luau_sandbox("sb-x", &runner, bad, err);
    check(bad_depth == nullptr, "política com max_call_depth=0 recusada");
    bad = engine::scripting::SandboxPolicy{};
    bad.allow_require = true;
    auto bad_require = engine::scripting::create_luau_sandbox("sb-x", &runner, bad, err);
    check(bad_require == nullptr, "allow_require sem allowlist de módulos recusada");
    bad = engine::scripting::SandboxPolicy{};
    bad.allowed_globals = { "" };
    auto bad_global = engine::scripting::create_luau_sandbox("sb-x", &runner, bad, err);
    check(bad_global == nullptr, "global vazio recusado");

    auto no_runner = engine::scripting::create_luau_sandbox("sb-2", nullptr, policy, err);
    check(no_runner != nullptr, "sandbox sem runner nasce ok (anexe depois)");
    auto r = no_runner->evaluate("1=>{\"a\":1}", "main", err);
    check(!r.ok && !err.empty(), "avaliação sem runner recusada com erro nomeado");
    check(no_runner->executions() == 0, "nada conta sem runner");
}

void test_attach_configure() {
    std::printf("[attach/configure]\n");
    std::string err;
    DeterministicRunner runner;
    engine::scripting::SandboxPolicy policy;
    auto sb = engine::scripting::create_luau_sandbox("sb-3", nullptr, policy, err);

    check(!sb->attach_runner(nullptr, err), "attach_runner nulo recusado");

    engine::scripting::SandboxPolicy p2;
    p2.max_instructions = 0;
    check(!sb->configure(p2, err), "configure com max_instructions=0 recusado");
    check(sb->policy().max_instructions == 10000, "estado intacto após configure inválido");

    p2 = engine::scripting::SandboxPolicy{};
    p2.allow_require = true;
    check(!sb->configure(p2, err), "configure com allow_require recusado");
    check(!sb->policy().allow_require, "estado intacto após configure inválido 2");

    p2 = engine::scripting::SandboxPolicy{};
    p2.max_instructions = 50;
    p2.allowed_globals = { "math", "table" };
    check(sb->configure(p2, err), "configure válido aplica");
    check(sb->policy().max_instructions == 50, "max_instructions aplicado");
    check(sb->policy().allowed_globals.size() == 2, "allowlist aplicada");

    check(sb->attach_runner(&runner, err), "attach_runner com runner válido");
    auto r = sb->evaluate("5=>{\"v\":1}", "main", err);
    check(r.ok && r.value == "{\"v\":1}", "avaliação funciona após attach");
}

void test_budget() {
    std::printf("[budget]\n");
    std::string err;
    DeterministicRunner runner;
    engine::scripting::SandboxPolicy policy;
    policy.max_instructions = 10;
    auto sb = engine::scripting::create_luau_sandbox("sb-4", &runner, policy, err);

    auto ok = sb->evaluate("{5}=>{\"a\":1}", "main", err);
    check(ok.ok && ok.instructions_used == 5, "script dentro do teto executa");
    check(sb->executions() == 1, "execução contada");

    auto over = sb->evaluate("{999}!! boom", "main", err);
    check(!over.ok, "custo > teto → falha");
    check(over.error.find("budget") != std::string::npos, "erro tem tag 'budget'");
    check(sb->executions() == 1, "execução estourada NÃO conta (all-or-nothing)");

    // Runner que mente: reporta ok=true mas custo acima do teto → a política
    // detecta e transforma em erro de budget.
    struct LyingRunner final : engine::scripting::IScriptRunner {
        engine::scripting::ScriptResult run(const std::string&, const std::string&,
                                            const std::string&, std::uint32_t,
                                            std::uint32_t, std::string& errorOut) override {
            errorOut.clear();
            return { true, "{}", "", 999 };
        }
    };
    LyingRunner liar;
    auto sb2 = engine::scripting::create_luau_sandbox("sb-4b", &liar, policy, err);
    auto lied = sb2->evaluate("x", "main", err);
    check(!lied.ok, "runner mentiroso (ok + custo > teto) pego pela política");
    check(lied.error.find("budget") != std::string::npos, "erro com tag 'budget'");
    check(sb2->executions() == 0, "nada conta");
}

void test_sandbox() {
    std::printf("[sandbox]\n");
    std::string err;
    DeterministicRunner runner;
    engine::scripting::SandboxPolicy policy;
    auto sb = engine::scripting::create_luau_sandbox("sb-5", &runner, policy, err);

    auto io = sb->evaluate("io.write('x')", "main", err);
    check(!io.ok, "acesso a io recusado (sandbox)");
    check(io.error.find("sandbox") != std::string::npos, "erro com tag 'sandbox'");
    check(sb->executions() == 0, "violação não conta");
}

void test_call_and_args() {
    std::printf("[call/args]\n");
    std::string err;
    DeterministicRunner runner;
    engine::scripting::SandboxPolicy policy;
    auto sb = engine::scripting::create_luau_sandbox("sb-6", &runner, policy, err);

    auto bad_args = sb->call("x=>{}", "main", "not json", err);
    check(!bad_args.ok && !err.empty(), "args_json inválido recusado antes do runner");
    check(sb->executions() == 0, "nada conta com args inválidos");

    auto good = sb->call("x=>{\"sum\":2}", "add", "{\"a\":1,\"b\":1}", err);
    check(good.ok && good.value == "{\"sum\":2}", "call com args JSON funciona");
    check(sb->executions() == 1, "call conta execução");

    auto rt = sb->evaluate("!! boom", "main", err);
    check(!rt.ok && rt.error.find("runtime") != std::string::npos, "erro de runtime com tag");

    auto empty_src = sb->evaluate("", "main", err);
    check(!empty_src.ok && !err.empty(), "source vazio recusado");
    auto empty_entry = sb->evaluate("x", "", err);
    check(!empty_entry.ok && !err.empty(), "entry vazio recusado");
}

void test_persistence() {
    std::printf("[persistence]\n");
    std::string err;
    DeterministicRunner runner;
    engine::scripting::SandboxPolicy policy;
    policy.max_instructions = 42;
    policy.allowed_globals = { "math" };
    auto sb = engine::scripting::create_luau_sandbox("sb-7", &runner, policy, err);
    sb->evaluate("5=>{}", "main", err);
    const std::string snap = sb->serialize_state();

    auto sb2 = engine::scripting::create_luau_sandbox("sb-7", nullptr, policy, err);
    check(sb2->load_from_json(snap, err), "load aceita o próprio snapshot");
    check(sb2->serialize_state() == snap, "round-trip bit-exact");
    check(sb2->executions() == 1, "contador preservado");
    check(sb2->policy().max_instructions == 42, "política preservada");

    // Rejeições all-or-nothing (estado intacto).
    check(!sb2->load_from_json("{}", err), "documento sem sandbox_id rejeitado");
    check(sb2->serialize_state() == snap, "estado intacto após rejeição 1");

    std::string bad2 = snap;
    const std::size_t pos = bad2.find("sb-7");
    bad2.replace(pos, 4, "sb-X");
    check(!sb2->load_from_json(bad2, err), "sandbox_id mismatch rejeitado");
    check(sb2->serialize_state() == snap, "estado intacto após rejeição 2");

    std::string bad3 = snap;
    const std::size_t pos3 = bad3.find("\"executions\":1");
    bad3.replace(pos3, 15, "\"executions\":0");
    check(!sb2->load_from_json(bad3, err), "contador não-crescente rejeitado");
    check(sb2->serialize_state() == snap, "estado intacto após rejeição 3");

    std::string bad4 = snap + ",\"bogus\":1}";
    check(!sb2->load_from_json(bad4, err), "campo desconhecido rejeitado");
    check(sb2->serialize_state() == snap, "estado intacto após rejeição 4");

    std::string bad5 = snap;
    const std::size_t pos5 = bad5.find("\"max_instructions\":42");
    bad5.replace(pos5, 23, "\"max_instructions\":0");
    check(!sb2->load_from_json(bad5, err), "política inválida rejeitada");
    check(sb2->serialize_state() == snap, "estado intacto após rejeição 5");
}

void test_determinism_reset() {
    std::printf("[determinism/reset]\n");
    std::string err;
    DeterministicRunner runner;
    engine::scripting::SandboxPolicy policy;
    auto p1 = engine::scripting::create_luau_sandbox("sb-8", &runner, policy, err);
    auto p2 = engine::scripting::create_luau_sandbox("sb-8", &runner, policy, err);
    p1->evaluate("3=>{\"a\":1}", "main", err);
    p2->evaluate("3=>{\"a\":1}", "main", err);
    check(p1->serialize_state() == p2->serialize_state(),
          "snapshot determinístico cross-instance");

    check(p1->reset(err), "reset ok");
    check(p1->executions() == 0, "reset zera o contador");
    check(p1->policy().max_instructions == 10000, "reset mantém a política");
    auto r = p1->evaluate("2=>{}", "main", err);
    check(r.ok && p1->executions() == 1, "sandbox reutilizável após reset");
}

}  // namespace

int main() {
    test_creation();
    test_attach_configure();
    test_budget();
    test_sandbox();
    test_call_and_args();
    test_persistence();
    test_determinism_reset();
    if (failures == 0) {
        std::printf("LuauSandboxTests: ALL PASSED\n");
        return 0;
    }
    std::printf("LuauSandboxTests: %d FAILURE(S)\n", failures);
    return 1;
}
