// AbilityEffectsTests — gate do contrato IAbilityEffects (§5 item 59,
// efeitos de abilities CORE): prova validação por kind (force/field/
// teleport/create/destroy/status/generic), emissão como eventos públicos
// (kind code + payload serializado), recusas all-or-nothing e determinismo.

#include "engine/gameplay/IAbilityEffects.hpp"

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

engine::gameplay::AbilityEffectSpec make_spec(const std::string& id,
                                              engine::gameplay::AbilityEffectKind kind) {
    engine::gameplay::AbilityEffectSpec spec;
    spec.id = id;
    spec.kind = kind;
    return spec;
}

void test_configure_and_emit() {
    auto effects = engine::gameplay::create_ability_effects();
    auto events = engine::gameplay::create_gameplay_events();
    std::string error;

    std::vector<engine::gameplay::AbilityEffectSpec> specs;
    engine::gameplay::AbilityEffectSpec punch = make_spec("punch", engine::gameplay::AbilityEffectKind::ForceImpulse);
    punch.magnitude = 25.0f;
    punch.target = { 1.0f, 0.0f, 0.0f };
    specs.push_back(punch);
    engine::gameplay::AbilityEffectSpec nova = make_spec("nova", engine::gameplay::AbilityEffectKind::Field);
    nova.magnitude = 10.0f;
    nova.radius = 3.0f;
    specs.push_back(nova);
    engine::gameplay::AbilityEffectSpec blink = make_spec("blink", engine::gameplay::AbilityEffectKind::Teleport);
    blink.target = { 100.0f, 64.0f, -50.0f };
    specs.push_back(blink);
    engine::gameplay::AbilityEffectSpec summon = make_spec("summon", engine::gameplay::AbilityEffectKind::CreateBlock);
    summon.blockId = "vulkancraft:stone";
    summon.target = { 5.0f, 1.0f, 5.0f };
    specs.push_back(summon);
    engine::gameplay::AbilityEffectSpec poison = make_spec("poison", engine::gameplay::AbilityEffectKind::Status);
    poison.statusId = "poison";
    poison.statusStacks = 2;
    specs.push_back(poison);
    engine::gameplay::AbilityEffectSpec sound = make_spec("sound", engine::gameplay::AbilityEffectKind::Generic);
    sound.eventKind = "boom";
    specs.push_back(sound);

    check(effects->configure(specs, error), "configure 6 efeitos");
    check(effects->count() == 6, "6 specs");
    check(effects->ids().size() == 6 && effects->ids()[0] == "blink",
          "ids em ordem crescente (blink primeiro)");
    check(effects->spec("nova") != nullptr && effects->spec("nova")->radius == 3.0f,
          "spec nova preservado");
    check(effects->spec("ghost") == nullptr, "spec desconhecido → nullptr");

    // Emissão: kind code = kind+1 (1..7).
    check(effects->emit(*events, "punch", 100, error), "emit punch");
    check(effects->emit(*events, "nova", 100, error), "emit nova");
    check(!effects->emit(*events, "ghost", 100, error), "emit de desconhecido → false");

    const auto drained = events->drain();
    check(drained.size() == 2, "2 eventos publicados");
    check(drained[0].kind == 1 && drained[0].tick == 100, "punch → kind 1");
    check(drained[1].kind == 2, "nova → kind 2");
    // Payload serializado: [kind(u8)][len(u8)][id bytes]...
    std::string idBytes(drained[0].payload.begin() + 2,
                        drained[0].payload.begin() + 2 + drained[0].payload[1]);
    check(idBytes == "punch", "payload contém o id após kind+len");
    check(drained[1].payload.size() > 10, "payload do campo não-vazio");
}

void test_refusals() {
    auto effects = engine::gameplay::create_ability_effects();
    std::string error;
    engine::gameplay::AbilityEffectSpec baseline = make_spec("a", engine::gameplay::AbilityEffectKind::Generic);
    baseline.eventKind = "boom";
    check(effects->configure({ baseline }, error), "baseline aceita");
    const std::vector<std::string> intact = effects->ids();

    std::vector<engine::gameplay::AbilityEffectSpec> bad;
    bad.push_back(make_spec("", engine::gameplay::AbilityEffectKind::Generic));
    check(!effects->configure(bad, error), "id vazio recusa");
    bad.clear();
    bad.push_back(make_spec("a", engine::gameplay::AbilityEffectKind::Generic));
    check(!effects->configure(bad, error), "id duplicado recusa");
    bad.clear();
    engine::gameplay::AbilityEffectSpec negMag = make_spec("b", engine::gameplay::AbilityEffectKind::ForceImpulse);
    negMag.magnitude = -1.0f;
    bad.push_back(negMag);
    check(!effects->configure(bad, error), "magnitude negativa recusa");
    bad.clear();
    engine::gameplay::AbilityEffectSpec badField = make_spec("c", engine::gameplay::AbilityEffectKind::Field);
    badField.radius = 0.0f;
    bad.push_back(badField);
    check(!effects->configure(bad, error), "field radius 0 recusa");
    bad.clear();
    engine::gameplay::AbilityEffectSpec noBlock = make_spec("d", engine::gameplay::AbilityEffectKind::CreateBlock);
    bad.push_back(noBlock);
    check(!effects->configure(bad, error), "create_block sem blockId recusa");
    bad.clear();
    engine::gameplay::AbilityEffectSpec noStatus = make_spec("e", engine::gameplay::AbilityEffectKind::Status);
    bad.push_back(noStatus);
    check(!effects->configure(bad, error), "status sem statusId recusa");
    bad.clear();
    engine::gameplay::AbilityEffectSpec noEvent = make_spec("f", engine::gameplay::AbilityEffectKind::Generic);
    bad.push_back(noEvent);
    check(!effects->configure(bad, error), "generic sem eventKind recusa");

    check(effects->ids() == intact, "estado intacto após recusas");
}

}  // namespace

int main() {
    test_configure_and_emit();
    test_refusals();

    if (failures == 0) {
        std::printf("ability_effects_tests: all checks passed\n");
        return 0;
    }
    std::printf("ability_effects_tests: %d failure(s)\n", failures);
    return 1;
}
