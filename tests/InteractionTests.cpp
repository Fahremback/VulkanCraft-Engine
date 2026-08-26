// InteractionTests — gate do contrato IInteraction (§1 item 26, componente de
// interação CORE): prova config all-or-nothing (nome vazio/duplicado, raio <= 0,
// cooldown < 0, prompt vazio), avaliação por raio/facing/cooldown determinística,
// activate com cooldown, advance (dt negativo recusa) e round-trip JSON bit-exact.

#include "engine/gameplay/IInteraction.hpp"

#include <cmath>
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

bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

void test_config() {
    auto interaction = engine::gameplay::create_interaction();
    std::string error;

    std::vector<engine::gameplay::InteractionDef> defs;
    defs.push_back({ "open", "[E] Abrir", 2.0f, false, 1.0f });
    defs.push_back({ "talk", "[F] Falar", 3.0f, true, 0.5f });
    check(interaction->configure(defs, error), "configure 2 interações");
    check(interaction->count() == 2, "count 2");

    std::vector<engine::gameplay::InteractionDef> bad;
    bad.push_back({ "", "x", 1.0f, false, 0.0f });
    check(!interaction->configure(bad, error), "nome vazio recusa");
    bad.clear();
    bad.push_back({ "a", "x", 1.0f, false, 0.0f });
    bad.push_back({ "a", "y", 1.0f, false, 0.0f });
    check(!interaction->configure(bad, error), "nome duplicado recusa");
    bad.clear();
    bad.push_back({ "b", "x", 0.0f, false, 0.0f });
    check(!interaction->configure(bad, error), "raio 0 recusa");
    bad.clear();
    bad.push_back({ "c", "x", 1.0f, false, -1.0f });
    check(!interaction->configure(bad, error), "cooldown negativo recusa");
    bad.clear();
    bad.push_back({ "d", "", 1.0f, false, 0.0f });
    check(!interaction->configure(bad, error), "prompt vazio recusa");
    check(interaction->count() == 2, "estado intacto após recusas");
}

void test_evaluate() {
    auto interaction = engine::gameplay::create_interaction();
    std::string error;
    std::vector<engine::gameplay::InteractionDef> defs;
    defs.push_back({ "open", "[E] Abrir", 2.0f, false, 0.0f });
    defs.push_back({ "door", "[E] Porta", 2.0f, true, 0.0f });
    check(interaction->configure(defs, error), "configure p/ evaluate");

    // O mapa ordena por nome: states[0] = "door", states[1] = "open".
    // Entidade em (5,0) olhando +X (facing 0); interator em (5.9,0) olhando
    // para a entidade (facing π ≈ -X) → door disponível.
    std::vector<engine::gameplay::InteractionState> states =
        interaction->evaluate(5.0f, 0.0f, 0.0f, 5.9f, 0.0f, 3.14159f);
    check(states.size() == 2, "2 estados");
    check(states[0].name == "door" && states[0].available,
          "door disponível (interator de frente)");
    check(states[1].name == "open" && states[1].available, "open disponível no raio");
    check(near(states[1].distance, 0.9f), "distância 0.9");

    // Interator olhando +X (facing 0) → olha PARA LONGE da entidade → door
    // indisponível por facing (open segue disponível: não exige facing).
    states = interaction->evaluate(5.0f, 0.0f, 0.0f, 5.9f, 0.0f, 0.0f);
    check(!states[0].available, "door indisponível (facing errado)");
    check(states[1].available, "open segue disponível (sem facing)");

    // Fora do raio → ambos indisponíveis.
    states = interaction->evaluate(5.0f, 0.0f, 0.0f, 10.0f, 0.0f, 0.0f);
    check(!states[1].available, "open indisponível fora do raio");
}

void test_cooldown() {
    auto interaction = engine::gameplay::create_interaction();
    std::string error;
    std::vector<engine::gameplay::InteractionDef> defs;
    defs.push_back({ "lever", "Alavanca", 2.0f, false, 2.0f });
    check(interaction->configure(defs, error), "configure cooldown");

    check(interaction->activate("lever"), "activate lever");
    std::vector<engine::gameplay::InteractionState> states =
        interaction->evaluate(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    check(!states[0].available, "em cooldown → indisponível");
    check(near(states[0].remainingCooldown, 2.0f), "cooldown restante 2.0");

    check(!interaction->activate("ghost"), "activate desconhecido → false");
    check(!interaction->advance(-1.0f), "advance negativo recusa");

    check(interaction->advance(1.0f), "advance 1.0");
    states = interaction->evaluate(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    check(!states[0].available, "ainda em cooldown");
    check(interaction->advance(1.0f), "advance 1.0 (fim)");
    states = interaction->evaluate(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    check(states[0].available && near(states[0].remainingCooldown, 0.0f),
          "cooldown expirado → disponível");
}

void test_json() {
    auto interaction = engine::gameplay::create_interaction();
    std::string error;
    std::vector<engine::gameplay::InteractionDef> defs;
    defs.push_back({ "open", "[E] Abrir", 2.0f, false, 1.0f });
    defs.push_back({ "talk", "[F] Falar", 3.0f, true, 0.5f });
    check(interaction->configure(defs, error), "configure p/ json");

    std::string json;
    check(interaction->to_json(json), "to_json");
    auto restored = engine::gameplay::create_interaction();
    check(restored->from_json(json, error), "from_json");
    check(restored->count() == 2, "count após round-trip");

    std::string json2;
    check(restored->to_json(json2), "to_json do restaurado");
    check(json == json2, "round-trip bit-exact");

    check(!restored->from_json("{broken", error), "JSON inválido recusa");
    check(!restored->from_json("{\"version\":2,\"interactions\":[]}", error),
          "versão não suportada recusa");
    check(restored->count() == 2, "estado intacto após recusas JSON");
}

}  // namespace

int main() {
    test_config();
    test_evaluate();
    test_cooldown();
    test_json();

    if (failures == 0) {
        std::printf("interaction_tests: all checks passed\n");
        return 0;
    }
    std::printf("interaction_tests: %d failure(s)\n", failures);
    return 1;
}
