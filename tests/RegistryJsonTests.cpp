// RegistryJsonTests.cpp — AGENT-5-DOCS-20260825 (claim 01:30, 2026-08-26)
//
// Gate do contrato "json_parse limpa errorOut na entrada" (convenção dos
// adapters do SDK — clear na entrada, nunca deixar resíduo stale).
//
// MOTIVAÇÃO (discovery com evidência): `json_parse` (RegistryJson.cpp) violava
// a convenção: o `fail` lambda só ESCREVE errorOut em erro, e o caminho de
// sucesso nunca limpa o buffer. Um caller que REUSA o mesmo `std::string
// errorOut` entre chamadas — e checa `!errorOut.empty()` após um parse
// bem-sucedido (padrão real em FastNoiseGraph.cpp:428, VoxelWorldFacade.cpp:2347
// e nas seams do EpisodeCompiler) — recebe um FALSO-NEGATIVO: o resíduo de uma
// falha anterior sobrevive ao sucesso. Esta é a mesma classe de bug corrigida
// 3x nos meus adapters (#140 errorOut stale no pipeline do EpisodeCompiler,
// #142 SimulationFarm, #145 SemanticApi — todos `errorOut.clear()` na entrada).

#include "RegistryJson.hpp"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    } else {
        std::printf("ok:   %s\n", what);
    }
}

}  // namespace

int main() {
    using engine::sdk::JsonValue;
    using engine::sdk::json_parse;

    // 1. Parse malformed primeiro — errorOut ganha diagnóstico.
    {
        JsonValue out;
        std::string errorOut;
        const bool ok = json_parse("{not json", out, errorOut);
        check(!ok, "parse malformado falha (baseline)");
        check(!errorOut.empty(), "parse malformado preenche errorOut (baseline)");
    }

    // 2. O CONTRATO QUEBRADO: reuso do MESMO errorOut.
    //    Falha (errorOut = resíduo) -> sucesso (errorOut DEVE ficar vazio).
    {
        JsonValue out;
        std::string errorOut;
        json_parse("{not json", out, errorOut);            // falha -> resíduo
        check(!errorOut.empty(), "resíduo presente após falha");
        const bool ok = json_parse("{\"name\":\"emerald\",\"count\":3}", out, errorOut);
        check(ok, "parse válido após falha sucede");
        check(out.is_object(), "saída é objeto");
        check(errorOut.empty(),
              "errorOut VAZIO após sucesso (resíduo stale NÃO contamina)");
    }

    // 3. Sequência sucesso->sucesso mantém vazio.
    {
        JsonValue out;
        std::string errorOut;
        check(json_parse("{\"a\":1}", out, errorOut), "primeiro parse válido");
        check(errorOut.empty(), "errorOut vazio após 1o sucesso");
        check(json_parse("{\"b\":2}", out, errorOut), "segundo parse válido");
        check(errorOut.empty(), "errorOut vazio após 2o sucesso");
    }

    // 4. Falha após sucesso volta a preencher (sem regressão no caminho de erro).
    {
        JsonValue out;
        std::string errorOut;
        check(json_parse("{\"a\":1}", out, errorOut), "parse válido antes de falha");
        check(errorOut.empty(), "errorOut vazio antes da falha");
        const bool ok = json_parse("{\"a\":}", out, errorOut);
        check(!ok, "parse malformado após sucesso falha");
        check(!errorOut.empty(), "falha volta a preencher errorOut");
    }

    // 5. ErrorOut pré-poluído de FORA (caller reusa buffer de outra API).
    {
        JsonValue out;
        std::string errorOut = "resíduo de outra chamada qualquer";
        check(json_parse("{\"ok\":true}", out, errorOut), "parse válido com errorOut pré-poluído");
        check(errorOut.empty(), "errorOut limpo na entrada apaga pré-poluição");
    }

    if (g_failures == 0) {
        std::printf("ALL PASSED\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
