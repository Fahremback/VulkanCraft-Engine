// InspectorDocTests — gate headless do contrato engine/editor IInspectorDoc.
//
// Verifica o modelo do Inspector: entidade vazia (nada selecionado),
// agrupamento semântico dos componentes (Aparência/Física/Jogabilidade/
// Animação na ordem canônica do painel), descritores tipados e JSON
// determinístico.

#include "engine/editor/IInspectorDoc.hpp"

#include <cstdio>
#include <string>

using engine::editor::create_inspector_doc;
using engine::editor::InspectorGroup;
using engine::editor::InspectorProperty;
using engine::editor::PropertyType;

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

void test_no_entity() {
    auto d = create_inspector_doc();
    const auto doc = d->build("", false, true, true, false, false, false, false,
                              false, false, false, false, false, false, false, false);
    check(!doc.has_entity, "sem entidade → has_entity false");
    check(doc.components.empty(), "sem entidade → sem componentes");
    check(doc.entity_name.empty(), "sem entidade → nome vazio");
}

void test_groups_canonical_order() {
    auto d = create_inspector_doc();
    // entidade com um componente de cada grupo
    const auto doc = d->build("hero", true,
                              true,   // transform (Appearance)
                              true,   // mesh_renderer (Appearance)
                              true,   // rigidbody (Physics)
                              true,   // destruction (Physics)
                              true,   // weapon (Gameplay)
                              false,  // vehicle
                              true,   // ragdoll (Gameplay)
                              true,   // animation (Animation)
                              false,  // timeline
                              false,  // ik
                              false,  // retarget
                              false,  // mission
                              false,  // dialogue
                              false); // navigation
    check(doc.has_entity, "entidade presente");
    check(doc.entity_name == "hero", "nome preservado");
    // ordem canônica: transform, mesh_renderer, rigidbody, destruction,
    // weapon, ragdoll, animation
    check(doc.components.size() == 7, "7 componentes");
    check(doc.components[0].name == "transform" &&
              doc.components[0].group == InspectorGroup::Appearance,
          "transform primeiro (Appearance)");
    check(doc.components[2].name == "rigidbody" &&
              doc.components[2].group == InspectorGroup::Physics,
          "rigidbody em Physics");
    check(doc.components[4].name == "weapon" &&
              doc.components[4].group == InspectorGroup::Gameplay,
          "weapon em Gameplay");
    check(doc.components[6].name == "animation" &&
              doc.components[6].group == InspectorGroup::Animation,
          "animation em Animation (último)");
}

void test_transform_properties() {
    auto d = create_inspector_doc();
    const auto doc = d->build("e", true, true, false, false, false, false, false,
                              false, false, false, false, false, false, false, false);
    check(doc.components.size() == 1, "só transform");
    const auto& props = doc.components[0].properties;
    check(props.size() == 3, "transform tem 3 propriedades");
    check(props[0].name == "position" && props[0].type == PropertyType::Vec3,
          "position é vec3");
    check(props[2].name == "scale" && props[2].type == PropertyType::Vec3,
          "scale é vec3");
}

void test_asset_and_bool_types() {
    auto d = create_inspector_doc();
    const auto doc = d->build("e", true, true, true, false, false, false, false,
                              false, true, false, false, false, false, false, false);
    bool found_asset = false, found_bool = false, found_clip = false;
    for (const auto& c : doc.components) {
        for (const auto& p : c.properties) {
            if (p.type == PropertyType::Asset) found_asset = true;
            if (p.type == PropertyType::Bool) found_bool = true;
            if (p.name == "clip") found_clip = true;
        }
    }
    check(found_asset, "mesh_renderer tem propriedade asset (mesh)");
    check(found_bool, "mesh_renderer tem propriedade bool (cast_shadow)");
    check(found_clip, "animation tem propriedade clip");
}

void test_json_deterministic() {
    auto a = create_inspector_doc();
    auto b = create_inspector_doc();
    const auto doc = a->build("hero", true, true, true, true, false, false, false,
                              false, false, false, false, false, false, false, false);
    const std::string j1 = a->to_json(doc);
    check(j1.find("\"group\":\"appearance\"") != std::string::npos, "JSON tem group appearance");
    check(j1.find("\"group\":\"physics\"") != std::string::npos, "JSON tem group physics");
    // determinismo: mesma build → mesmo JSON
    const auto doc2 = b->build("hero", true, true, true, true, false, false, false,
                               false, false, false, false, false, false, false, false);
    check(b->to_json(doc2) == j1, "JSON determinístico para a mesma entrada");
}

}  // namespace

int main() {
    test_no_entity();
    test_groups_canonical_order();
    test_transform_properties();
    test_asset_and_bool_types();
    test_json_deterministic();

    if (g_failures == 0) {
        std::printf("ALL PASSED\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
