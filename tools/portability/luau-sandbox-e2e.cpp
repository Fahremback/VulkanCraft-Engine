// luau-sandbox-e2e.cpp — E2E do contrato ILuauSandbox com o backend REAL:
// compila o adapter (src/engine/sdk/LuauSandbox.cpp) + o LuauRunner (binding
// sobre o Luau vendido, #302) e prova o fluxo completo: attach do runner
// real, avaliação de script Luau real (compile→bytecode→VM→run), valor de
// retorno JSON real de volta pelo contrato, erro de runtime propagado com
// tag estável, sandbox por construção (io/os/require removidos), persistência
// bit-exact do contrato com o estado real. O enforcement de budget é do
// contrato (ctest, determinístico); aqui prova-se o WIRING real.
//
// Compilado APENAS pelo gate (luau-sandbox-e2e-gate.mjs) contra as libs
// estáticas do external/solutions/luau — não faz parte do build da engine.

#include <cstdio>
#include <string>

#include "engine/scripting/ILuauSandbox.hpp"
#include "luau-runner.hpp"

#include "../../src/engine/sdk/LuauSandbox.cpp"

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what.c_str());
        ++failures;
    }
}

}  // namespace

int main() {
    std::printf("luau-sandbox-e2e: vendored Luau through the ILuauSandbox contract\n");

    std::string err;
    luau_e2e::LuauRunner runner;
    engine::scripting::SandboxPolicy policy;
    auto sb = engine::scripting::create_luau_sandbox("e2e-1", &runner, policy, err);
    if (sb == nullptr) std::printf("  create error: %s\n", err.c_str());
    check(sb != nullptr, "sandbox criada com runner REAL");

    // Script Luau real: soma determinística.
    auto r = sb->evaluate(
        "local a = 40\n"
        "local b = 2\n"
        "return a + b\n", "main", err);
    check(r.ok, "script Luau real executou pelo contrato");
    check(r.value == "42", "retorno real 42 em JSON (got: " + r.value + ")");

    // Script com string real.
    auto r2 = sb->evaluate("return 'olá mundo'\n", "main", err);
    check(r2.ok && r2.value.find("ol") != std::string::npos,
          "string real retornada em JSON (got: " + r2.value + ")");

    // Erro de runtime real propagado com tag estável.
    auto r3 = sb->evaluate("error('boom')\n", "main", err);
    check(!r3.ok, "erro de runtime real falha");
    check(r3.error.find("runtime") != std::string::npos,
          "tag 'runtime' presente (got: " + r3.error + ")");

    // Sandbox por construção: io/os/require não existem no ambiente.
    auto r4 = sb->evaluate("return io\n", "main", err);
    check(r4.ok && r4.value == "null", "io ausente no ambiente sandboxed (got: " + r4.value + ")");
    auto r5 = sb->evaluate("return require\n", "main", err);
    check(r5.ok && r5.value == "null", "require ausente (got: " + r5.value + ")");

    // Contador de execuções + persistência bit-exact com o backend real.
    check(sb->executions() >= 4, "execuções contadas");
    const std::string snap = sb->serialize_state();
    auto sb2 = engine::scripting::create_luau_sandbox("e2e-1", &runner, policy, err);
    check(sb2->load_from_json(snap, err), "load do snapshot com estado real");
    check(sb2->serialize_state() == snap, "round-trip bit-exact");
    check(sb2->executions() == sb->executions(), "contador preservado");

    // call com args JSON (o runner recebe args_json como campo da tabela).
    auto r6 = sb->call("return 7\n", "main", "{\"a\":1}", err);
    check(r6.ok && r6.value == "7", "call com args JSON funciona (got: " + r6.value + ")");
    auto r7 = sb->call("return 1\n", "main", "not-json", err);
    check(!r7.ok && !err.empty(), "args inválido recusado antes do runner real");

    if (failures == 0) {
        std::printf("luau-sandbox-e2e: ALL PASSED (real Luau wired through ILuauSandbox)\n");
        return 0;
    }
    std::printf("luau-sandbox-e2e: %d FAILURE(S)\n", failures);
    return 1;
}
