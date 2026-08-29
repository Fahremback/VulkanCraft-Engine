// AbilityEffectsFacadeTests.cpp — testa o contrato público IAbilityEffects
// (engine::gameplay): configure all-or-nothing, emit publicando evento tipado
// no IGameplayEvents (payload serializado), query ordenada por id.
#include "engine/gameplay/IAbilityEffects.hpp"
#include "engine/gameplay/IGameplayEvents.hpp"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

static int g_failures = 0;
#define CHECK(cond)                                          \
    do {                                                     \
        if (!(cond)) {                                       \
            ++g_failures;                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                    \
    } while (0)

int main() {
    auto effects = engine::gameplay::create_ability_effects();
    auto events = engine::gameplay::create_gameplay_events();

    if (!effects || !events) {
        std::printf("FAIL: factories returned null\n");
        return 1;
    }

    // Empty configure is valid (0 specs).
    std::string err;
    CHECK(effects->configure({}, err));
    CHECK(effects->count() == 0);

    // Valid specs.
    using engine::gameplay::AbilityEffectKind;
    engine::gameplay::AbilityEffectSpec force;
    force.id = "b";
    force.kind = AbilityEffectKind::ForceImpulse;
    force.magnitude = 100.0f;
    engine::gameplay::AbilityEffectSpec teleport;
    teleport.id = "a";
    teleport.kind = AbilityEffectKind::Teleport;
    teleport.target = glm::vec3(1.0f, 2.0f, 3.0f);
    engine::gameplay::AbilityEffectSpec block;
    block.id = "c";
    block.kind = AbilityEffectKind::CreateBlock;
    block.blockId = "stone";
    std::vector<engine::gameplay::AbilityEffectSpec> specs{ force, teleport, block };
    CHECK(effects->configure(specs, err));
    CHECK(effects->count() == 3);
    // ids() must be ascending.
    const auto ids = effects->ids();
    CHECK(ids.size() == 3);
    CHECK(ids[0] == "a" && ids[1] == "b" && ids[2] == "c");

    // Duplicate id rejected all-or-nothing.
    std::string dupErr;
    std::vector<engine::gameplay::AbilityEffectSpec> dup{ force, force };
    CHECK(!effects->configure(dup, dupErr));
    CHECK(!dupErr.empty());
    // Failed configure did not clobber existing state.
    CHECK(effects->count() == 3);

    // Invalid per-kind params rejected.
    engine::gameplay::AbilityEffectSpec badField;
    badField.id = "f";
    badField.kind = AbilityEffectKind::Field;
    badField.radius = 0.0f;
    std::string badErr;
    CHECK(!effects->configure({ badField }, badErr));
    CHECK(!badErr.empty());

    // Query single spec.
    CHECK(effects->spec("a") != nullptr);
    CHECK(effects->spec("a")->kind == AbilityEffectKind::Teleport);
    CHECK(effects->spec("missing") == nullptr);

    // emit publishes a typed event with serialized payload.
    CHECK(effects->emit(*events, "b", 42, err));
    CHECK(events->pending_count() == 1);
    auto drained = events->drain();
    CHECK(drained.size() == 1);
    CHECK(drained[0].kind == 1);            // ForceImpulse
    CHECK(drained[0].tick == 42);
    CHECK(!drained[0].payload.empty());     // serialized spec

    // Unknown emit rejected, nothing published.
    std::string unkErr;
    CHECK(!effects->emit(*events, "nope", 1, unkErr));
    CHECK(events->pending_count() == 0);

    // Clear.
    effects->clear();
    CHECK(effects->count() == 0);
    CHECK(effects->spec("a") == nullptr);

    if (g_failures == 0) {
        std::printf("ALL PASSED\n");
        return 0;
    }
    std::printf("FAILURES DETECTED: %d\n", g_failures);
    return 1;
}