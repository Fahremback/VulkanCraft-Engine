#pragma once
// ILuauSandbox — runtime de scripting sandboxed com budgets, para o domínio
// `engine/scripting/` (§3 item 6 — "Integrar runtime Luau sandboxed opcional
// com bindings gerados e budgets de CPU/memória"). O backend Luau REAL está
// vendido e provado utilizável (findings #302-luau-probe); este contrato é a
// POLÍTICA que qualquer runtime de scripting (Luau, WASM, ...) deve
// respeitar para ser sandboxed.
//
// Divisão honesta de responsabilidades (mesmo espírito de ISink em
// observability e ISignatureVerifier em packaging):
//   - ILuauSandbox = política: sandbox (allowlist de globals, io/require
//     proibidos), budgets (teto determinístico de instruções, guarda de
//     profundidade de chamada), shaping de resultado (JSON bit-exact com
//     chaves ordenadas), contadores e persistência all-or-nothing.
//   - IScriptRunner = o MOTOR substituível (binding para um runtime real).
//     Testes plugam um runner determinístico; produção pluga o Luau vendido.
// O contrato NÃO conhece o motor: o runner recebe a fonte + entrada + teto e
// devolve um ScriptResult opaco (JSON). A política decide se o que o runner
// reportou é aceitável (budget estourado → erro determinístico, nada muda).
//
// Self-contained (std only), headless, determinístico.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine::scripting {

// Política de sandbox. Todos os campos são validados all-or-nothing no
// `configure`: máximos < 1 são recusados (nada muda).
struct SandboxPolicy {
    // Teto determinístico de instruções por execução (>= 1). Estourou → o
    // ScriptResult vem com ok=false e `error` contendo "budget" — o estado do
    // sandbox NÃO muda (all-or-nothing).
    std::uint32_t max_instructions{ 10000 };

    // Guarda de profundidade de chamada (>= 1). Chamada mais funda que o teto
    // → erro determinístico, nada muda.
    std::uint32_t max_call_depth{ 64 };

    // Sandbox: IO e require são proibidos por padrão. `allow_io`/`allow_require`
    // NÃO liberam nada sozinhos — o runner decide o que expõe; a política só
    // registra a intenção e valida consistência (allow_require sem lista de
    // módulos permitidos é recusado).
    bool allow_io{ false };
    bool allow_require{ false };

    // Allowlist de globals expostos ao script (vazio = apenas a stdlib segura
    // do motor). Nomes vazios são recusados no configure.
    std::vector<std::string> allowed_globals;
};

// Resultado de uma execução: `value` é JSON serializado do valor de retorno
// (bit-exact, chaves ordenadas) quando ok; `error` carrega a mensagem quando
// não (sempre contém uma tag estável: "budget", "sandbox", "runtime",
// "compile"). `instructions_used` é o que o runner reportou (<= teto quando
// ok; > teto quando estourou).
struct ScriptResult {
    bool ok{ false };
    std::string value;              // JSON bit-exact (chaves ordenadas)
    std::string error;              // tag estável + detalhe opaco
    std::uint64_t instructions_used{ 0 };
};

// O motor executável (binding para um runtime de scripting). Substituível em
// runtime via `attach_runner`. O runner DEVE:
//   - respeitar `instruction_budget` (estourou → ok=false, error contendo
//     "budget");
//   - devolver `value` como JSON bit-exact (chaves ordenadas) em sucesso;
//   - não violar a sandbox do contrato (io/require desligados, só globals da
//     allowlist) — a política confia no runner para ENFORCEAR; o contrato só
//     VALIDA o que voltou.
struct IScriptRunner {
    virtual ~IScriptRunner() = default;

    // Executa `source` (fonte do script) chamando `entry` com `args_json`
    // (JSON). `depth` é a profundidade de chamada corrente (1 = chamada raiz).
    virtual ScriptResult run(const std::string& source,
                             const std::string& entry,
                             const std::string& args_json,
                             std::uint32_t instruction_budget,
                             std::uint32_t depth,
                             std::string& errorOut) = 0;
};

class ILuauSandbox {
public:
    virtual ~ILuauSandbox() = default;

    // Identificador fixo da instância.
    virtual const std::string& sandbox_id() const = 0;

    // Troca o motor em runtime (substituível — o chamador pluga o backend que
    // quiser). Runner nulo → false (nada muda). A política corrente é mantida.
    virtual bool attach_runner(IScriptRunner* runner, std::string& errorOut) = 0;

    // Configura a política (all-or-nothing: inválida → false, estado intacto).
    // `allow_require=true` exige `allowed_modules` não-vazia (consistência).
    virtual bool configure(const SandboxPolicy& policy, std::string& errorOut) = 0;
    virtual const SandboxPolicy& policy() const = 0;

    // Executa `source` (fonte) chamando `entry`. Sem runner anexado → false
    // (nada muda). Resultado/erro sempre determinístico para a mesma entrada.
    virtual ScriptResult evaluate(const std::string& source,
                                  const std::string& entry,
                                  std::string& errorOut) = 0;

    // Executa `source` chamando `entry` com `args_json` (JSON de argumentos).
    // `args_json` inválido (não-JSON) → false, nada muda.
    virtual ScriptResult call(const std::string& source,
                              const std::string& entry,
                              const std::string& args_json,
                              std::string& errorOut) = 0;

    // Contador de execuções bem-sucedidas (observabilidade da política).
    virtual std::uint64_t executions() const = 0;

    // Descarta o estado acumulado (execuções) mantendo política e runner.
    // Sempre ok.
    virtual bool reset(std::string& errorOut) = 0;

    // --- Persistência (bit-exact, all-or-nothing) ---
    // Serializa sandbox_id + política + contador de execuções. `load` rejeita
    // id mismatch, política inválida, contador não-crescente, campos
    // desconhecidos e trailing — em todas as rejeições o estado fica intacto.
    virtual bool load_from_json(const std::string& json, std::string& errorOut) = 0;
    virtual std::string serialize_state() const = 0;
};

// Cria um sandbox. `sandboxId` deve ser não-vazio; `runner` pode ser nulo
// (anexe depois via attach_runner) — avaliação sem runner é recusada até lá.
// `policy` inválida → nullptr + errorOut nomeado (all-or-nothing).
std::unique_ptr<ILuauSandbox> create_luau_sandbox(const std::string& sandboxId,
                                                  IScriptRunner* runner,
                                                  const SandboxPolicy& policy,
                                                  std::string& errorOut);

}  // namespace engine::scripting
