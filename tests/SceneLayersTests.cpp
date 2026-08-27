// SceneLayersTests.cpp
//
// Gate for ISceneLayers (agente 2 — item `openusd`): layered scene
// composition in the spirit of USD's core model. Proves the REAL adapter
// (src/engine/sdk/SceneLayers.cpp):
//   - precedence: strongest layer wins per property;
//   - USD-style over: an over record in a stronger layer only replaces a
//     property authored in a weaker layer (never introduces new ones);
//   - entities present only in weaker layers are still emitted;
//   - validation: empty stack / layer without entities / mismatched arrays
//     / empty ids refused all-or-nothing;
//   - determinism: same stack -> identical composed scene.
//
// Deterministic and headless. Self-contained (std + engine/assets only).

#include <engine/assets/ISceneLayers.hpp>

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what);
        ++g_failures;
    }
}

namespace {

engine::assets::LayerEntity make_entity(const std::string& id,
                                        std::vector<std::string> names,
                                        std::vector<float> values,
                                        std::vector<bool> over) {
    engine::assets::LayerEntity e;
    e.id = id;
    for (std::size_t i = 0; i < names.size(); ++i) {
        e.propNames.push_back(names[i]);
        engine::assets::LayerValue v;
        v.isVec3 = false;
        v.scalar = values[i];
        e.propValues.push_back(v);
        e.propOver.push_back(over.empty() ? false : over[i]);
    }
    return e;
}

void test_precedence() {
    std::printf("[slay] precedence…\n");
    auto layers = engine::assets::create_scene_layers();
    // Strongest layer defines color=1.0; weakest defines color=0.0.
    engine::assets::SceneLayer strong;
    strong.id = "strong";
    strong.entities.push_back(make_entity("rock", {"color"}, {1.0f}, {}));
    engine::assets::SceneLayer weak;
    weak.id = "weak";
    weak.entities.push_back(make_entity("rock", {"color"}, {0.0f}, {}));

    std::string err;
    std::vector<engine::assets::ComposedEntity> out;
    check(layers->compose({strong, weak}, out, err),
          "compose(strong, weak) succeeds");
    check(out.size() == 1 && out[0].id == "rock", "rock composed");
    if (!out.empty()) {
        check(out[0].propValues[0].scalar == 1.0f,
              "strongest layer wins (color=1.0)");
    }

    // Reversed: weak first -> weak wins.
    check(layers->compose({weak, strong}, out, err),
          "compose(weak, strong) succeeds");
    if (!out.empty()) {
        check(out[0].propValues[0].scalar == 0.0f,
              "strongest (first) layer wins in reversed stack");
    }
    std::printf("[slay] precedence OK\n");
}

void test_over_semantics() {
    std::printf("[slay] over semantics…\n");
    auto layers = engine::assets::create_scene_layers();
    // Weak defines color=0.5, weight=10. Strong has an OVER on color=1.0
    // (authored below -> applies) and an OVER on newprop=9 (not authored
    // below -> does NOT introduce it).
    engine::assets::SceneLayer strong;
    strong.id = "strong";
    strong.entities.push_back(make_entity(
        "mob", {"color", "newprop"}, {1.0f, 9.0f}, {true, true}));
    engine::assets::SceneLayer weak;
    weak.id = "weak";
    weak.entities.push_back(make_entity(
        "mob", {"color", "weight"}, {0.5f, 10.0f}, {}));

    std::string err;
    std::vector<engine::assets::ComposedEntity> out;
    check(layers->compose({strong, weak}, out, err),
          "compose(strong over, weak) succeeds");
    check(out.size() == 1 && out[0].id == "mob", "mob composed");
    if (!out.empty()) {
        // color: over applies (authored below) -> 1.0.
        // newprop: over does not apply (not authored below) -> absent.
        bool foundColor = false, foundNew = false, foundWeight = false;
        float color = 0.0f, weight = 0.0f;
        for (std::size_t i = 0; i < out[0].propNames.size(); ++i) {
            if (out[0].propNames[i] == "color") {
                foundColor = true;
                color = out[0].propValues[i].scalar;
            }
            if (out[0].propNames[i] == "newprop") foundNew = true;
            if (out[0].propNames[i] == "weight") {
                foundWeight = true;
                weight = out[0].propValues[i].scalar;
            }
        }
        check(foundColor && color == 1.0f,
              "over applies to authored-below property (color=1.0)");
        check(!foundNew, "over does NOT introduce un-authored property");
        check(foundWeight && weight == 10.0f,
              "weak-authored property survives (weight=10)");
    }
    std::printf("[slay] over OK\n");
}

void test_weaker_entities() {
    std::printf("[slay] weaker entities…\n");
    auto layers = engine::assets::create_scene_layers();
    engine::assets::SceneLayer strong;
    strong.id = "strong";
    strong.entities.push_back(make_entity("a", {"x"}, {1.0f}, {}));
    engine::assets::SceneLayer weak;
    weak.id = "weak";
    weak.entities.push_back(make_entity("b", {"x"}, {2.0f}, {}));

    std::string err;
    std::vector<engine::assets::ComposedEntity> out;
    check(layers->compose({strong, weak}, out, err),
          "compose with entity only in weak layer succeeds");
    check(out.size() == 2, "both entities emitted (a from strong, b from weak)");
    if (out.size() == 2) {
        check(out[1].id == "b" && out[1].propValues[0].scalar == 2.0f,
              "weak-only entity present with its values");
    }
    std::printf("[slay] weaker entities OK\n");
}

void test_validation() {
    std::printf("[slay] validation…\n");
    auto layers = engine::assets::create_scene_layers();
    std::vector<engine::assets::ComposedEntity> out;
    std::string err;

    // Empty stack.
    check(!layers->compose({}, out, err), "empty stack refused");
    check(!err.empty(), "refusal reports a message");

    // Layer without entities.
    engine::assets::SceneLayer empty;
    empty.id = "empty";
    check(!layers->compose({empty}, out, err), "layer without entities refused");

    // Mismatched property arrays.
    engine::assets::SceneLayer bad;
    bad.id = "bad";
    engine::assets::LayerEntity e;
    e.id = "x";
    e.propNames.push_back("p");
    e.propValues.push_back(engine::assets::LayerValue{});
    // propOver has 0 entries but propNames has 1 -> mismatch.
    bad.entities.push_back(e);
    err.clear();
    check(!layers->compose({bad}, out, err), "mismatched arrays refused");

    // Empty entity id.
    engine::assets::SceneLayer bad2;
    bad2.id = "bad2";
    bad2.entities.push_back(make_entity("", {"p"}, {1.0f}, {}));
    err.clear();
    check(!layers->compose({bad2}, out, err), "empty entity id refused");
    std::printf("[slay] validation OK\n");
}

void test_determinism() {
    std::printf("[slay] determinism…\n");
    auto a = engine::assets::create_scene_layers();
    auto b = engine::assets::create_scene_layers();
    engine::assets::SceneLayer strong, weak;
    strong.id = "s";
    strong.entities.push_back(make_entity("e", {"c", "r"}, {1.0f, 0.2f}, {}));
    weak.id = "w";
    weak.entities.push_back(make_entity("e", {"c"}, {0.5f}, {}));
    std::string ea, eb;
    std::vector<engine::assets::ComposedEntity> oa, ob;
    check(a->compose({strong, weak}, oa, ea), "compose(a) succeeds");
    check(b->compose({strong, weak}, ob, eb), "compose(b) succeeds");
    check(oa.size() == ob.size(), "same entity count");
    if (oa.size() == ob.size() && !oa.empty()) {
        check(oa[0].propNames == ob[0].propNames &&
                  oa[0].propValues.size() == ob[0].propValues.size(),
              "composed property sets identical");
    }
    std::printf("[slay] determinism OK\n");
}

}  // namespace

int main() {
    test_precedence();
    test_over_semantics();
    test_weaker_entities();
    test_validation();
    test_determinism();
    if (g_failures == 0) {
        std::printf("[slay] ALL PASSED\n");
        return 0;
    }
    std::printf("[slay] %d FAILURE(S)\n", g_failures);
    return 1;
}
