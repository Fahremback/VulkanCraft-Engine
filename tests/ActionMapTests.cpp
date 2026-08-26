// ActionMapTests — gate do contrato público de input mapping (agente 4 §1
// item 8). Prova que o mapa de ações é data-driven, determinístico,
// all-or-nothing no load, bit-exact no round-trip JSON, e que resolução,
// rebinding, conflito e multi-dispositivo se comportam como documentado.

#include "engine/input/IActionMap.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failures;
        std::cout << "FAIL: " << message << "\n";
    }
}

using engine::input::ActionMapSpec;
using engine::input::InputBinding;
using engine::input::InputSource;
using engine::input::create_action_map;

// ---- Spec round-trip + validation -----------------------------------------

void test_spec_roundtrip() {
    const std::string json =
        "{\"version\":1,\"actions\":["
        "{\"action\":\"jump\",\"bindings\":["
        "{\"source\":\"keyboard\",\"device\":\"\",\"input\":\"Space\",\"axis\":0,\"scale\":1,\"deadzone\":0}]},"
        "{\"action\":\"move_forward\",\"bindings\":["
        "{\"source\":\"keyboard\",\"device\":\"\",\"input\":\"KeyW\",\"axis\":0,\"scale\":1,\"deadzone\":0},"
        "{\"source\":\"gamepad\",\"device\":\"pad1\",\"input\":\"AxisLY\",\"axis\":0,\"scale\":-1,\"deadzone\":0.1}]}]}";

    ActionMapSpec spec;
    std::string error;
    check(spec.load_from_json(json, error), "spec load ok");
    check(error.empty(), "spec load no error");
    check(spec.to_json() == json, "spec round-trip bit-exact");

    // Refusals.
    const char* bad[] = {
        "{\"version\":2,\"actions\":[]}",
        "{\"version\":1,\"actions\":[{\"action\":\"\",\"bindings\":[{\"source\":\"keyboard\",\"input\":\"Space\"}]}]}",
        "{\"version\":1,\"actions\":[{\"action\":\"a\",\"bindings\":[{\"source\":\"keyboard\",\"input\":\"\"}]}]}",
        "{\"version\":1,\"actions\":[{\"action\":\"a\",\"bindings\":[{\"source\":\"keyboard\",\"input\":\"Space\",\"deadzone\":2}]}]}",
        "{\"version\":1,\"actions\":["
        "{\"action\":\"a\",\"bindings\":[{\"source\":\"keyboard\",\"input\":\"Space\"}]},"
        "{\"action\":\"b\",\"bindings\":[{\"source\":\"keyboard\",\"input\":\"Space\"}]}]}",
    };
    for (const char* text : bad) {
        ActionMapSpec s;
        std::string err;
        check(!s.load_from_json(text, err), std::string("bad spec refused: ") + text);
        check(!err.empty(), "bad spec diagnostic non-empty");
    }

    std::cout << "  spec: round-trip bit-exact + refusals OK\n";
}

// ---- Resolution + multi-device --------------------------------------------

void test_resolution() {
    const std::string json =
        "{\"version\":1,\"actions\":["
        "{\"action\":\"jump\",\"bindings\":[{\"source\":\"keyboard\",\"device\":\"\",\"input\":\"Space\",\"axis\":0,\"scale\":1,\"deadzone\":0}]},"
        "{\"action\":\"move_forward\",\"bindings\":["
        "{\"source\":\"keyboard\",\"device\":\"\",\"input\":\"KeyW\",\"axis\":0,\"scale\":1,\"deadzone\":0},"
        "{\"source\":\"gamepad\",\"device\":\"pad1\",\"input\":\"AxisLY\",\"axis\":0,\"scale\":-1,\"deadzone\":0.1}]}]}";

    ActionMapSpec spec;
    std::string error;
    check(spec.load_from_json(json, error), "resolution spec load");
    auto map = create_action_map(spec, error);
    check(map != nullptr, "map created");

    // Keyboard jump.
    auto acts = map->poll(InputSource::Keyboard, "", "Space", 0, 1.0, 0.0);
    check(acts.size() == 1 && acts[0].action == "jump" && acts[0].value == 1.0,
          "keyboard jump resolves");

    // Gamepad axis with scale -1 + deadzone 0.1: value 1.0 -> -1.0.
    acts = map->poll(InputSource::Gamepad, "pad1", "AxisLY", 0, 1.0, 0.0);
    check(acts.size() == 1 && acts[0].action == "move_forward", "gamepad axis maps to move_forward");
    check(std::fabs(acts[0].value - (-1.0)) < 1e-9, "gamepad scale -1 applied");

    // Deadzone: |0.05| < 0.1 -> zeroed value.
    acts = map->poll(InputSource::Gamepad, "pad1", "AxisLY", 0, 0.05, 0.0);
    check(acts.size() == 1 && acts[0].value == 0.0, "deadzone zeroes small input");

    // Wrong device tag does NOT resolve (multi-device isolation).
    acts = map->poll(InputSource::Gamepad, "pad2", "AxisLY", 0, 1.0, 0.0);
    check(acts.empty(), "different device does not resolve first device's axis");

    std::cout << "  resolution: keyboard/gamepad/deadzone/multi-device OK\n";
}

// ---- Rebinding + conflicts ------------------------------------------------

void test_rebinding_and_conflicts() {
    const std::string json =
        "{\"version\":1,\"actions\":["
        "{\"action\":\"jump\",\"bindings\":[{\"source\":\"keyboard\",\"device\":\"\",\"input\":\"Space\",\"axis\":0,\"scale\":1,\"deadzone\":0}]},"
        "{\"action\":\"fire\",\"bindings\":[{\"source\":\"keyboard\",\"device\":\"\",\"input\":\"KeyF\",\"axis\":0,\"scale\":1,\"deadzone\":0}]}]}";

    ActionMapSpec spec;
    std::string error;
    check(spec.load_from_json(json, error), "rebind spec load");
    auto map = create_action_map(spec, error);
    check(map != nullptr, "rebind map created");

    // Clean rebind: jump -> KeyJ.
    InputBinding newJump;
    newJump.source = InputSource::Keyboard;
    newJump.input = "KeyJ";
    newJump.scale = 1.0;
    check(map->rebind("jump", 0, newJump, false, error), "clean rebind ok");
    check(error.empty(), "clean rebind no error");
    auto acts = map->poll(InputSource::Keyboard, "", "KeyJ", 0, 1.0, 0.0);
    check(acts.size() == 1 && acts[0].action == "jump", "rebound key resolves");
    acts = map->poll(InputSource::Keyboard, "", "Space", 0, 1.0, 0.0);
    check(acts.empty(), "old key no longer resolves");

    // Conflicting rebind (jump -> KeyF, already fire) refused without override.
    InputBinding conflict;
    conflict.source = InputSource::Keyboard;
    conflict.input = "KeyF";
    conflict.scale = 1.0;
    check(!map->rebind("jump", 0, conflict, false, error), "conflicting rebind refused");
    check(!error.empty(), "conflict diagnostic");
    // Map untouched: KeyJ still resolves.
    acts = map->poll(InputSource::Keyboard, "", "KeyJ", 0, 1.0, 0.0);
    check(acts.size() == 1 && acts[0].action == "jump", "refused rebind leaves map untouched");

    // With override, the rebind succeeds (project-owned conflict policy).
    check(map->rebind("jump", 0, conflict, true, error), "override rebind ok");

    // conflicts() lists the now-shared KeyF binding.
    bool hasJumpConflict = false;
    for (const auto& [actionName, slot] : map->conflicts()) {
        if (actionName == "jump" || actionName == "fire") hasJumpConflict = true;
    }
    check(hasJumpConflict, "conflicts() reports the shared binding");

    std::cout << "  rebinding: clean/conflict/override/conflicts() OK\n";
}

// ---- Determinism ----------------------------------------------------------

void test_determinism() {
    const std::string json =
        "{\"version\":1,\"actions\":["
        "{\"action\":\"strafe\",\"bindings\":[{\"source\":\"gamepad\",\"device\":\"pad1\",\"input\":\"AxisLX\",\"axis\":0,\"scale\":1,\"deadzone\":0.2}]}]}";

    ActionMapSpec specA, specB;
    std::string error;
    check(specA.load_from_json(json, error) && specB.load_from_json(json, error), "determinism spec loads");
    auto mapA = create_action_map(specA, error);
    auto mapB = create_action_map(specB, error);

    const double inputValues[] = { 0.0, 0.1, 0.5, -0.7, 0.0 };
    for (double v : inputValues) {
        const auto a = mapA->poll(InputSource::Gamepad, "pad1", "AxisLX", 0, v, 0.016);
        const auto b = mapB->poll(InputSource::Gamepad, "pad1", "AxisLX", 0, v, 0.016);
        check(a.size() == b.size(), "determinism activation count equal");
        for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
            check(a[i].action == b[i].action && a[i].value == b[i].value,
                  "determinism activation equal");
        }
    }
    check(mapA->spec().to_json() == mapB->spec().to_json(), "determinism spec bit-exact");

    std::cout << "  determinism: bit-exact across instances OK\n";
}

}  // namespace

int main() {
    std::cout << "[action_map_tests]\n";
    test_spec_roundtrip();
    test_resolution();
    test_rebinding_and_conflicts();
    test_determinism();

    if (g_failures == 0) {
        std::cout << "ALL ACTION MAP TESTS PASSED\n";
        return 0;
    }
    std::cout << g_failures << " FAILURE(S)\n";
    return 1;
}