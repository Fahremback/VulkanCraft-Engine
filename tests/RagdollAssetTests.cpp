// RagdollAssetTests — gate do contrato IRagdollAsset (§4 item 55, ragdoll
// configurável — CORE do asset data-driven). Prova: round-trip JSON
// bit-exact (%.9g), load all-or-nothing (cada rejeição mantém o asset
// anterior intacto), validate estrutural/semântico (duplicatas, parents,
// ciclos, finitos, faixas, drives) e build_bones mapeando o skeleton
// público do runtime (RagdollBone) na ordem dos joints.

#include "engine/gameplay/IRagdollAsset.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what);
        ++failures;
    }
}

// Asset humanoide de referência: pelvis (raiz) -> spine -> head, com limits,
// drives (parcial: só spine dirigido), autoBalance e blend.
engine::gameplay::RagdollAsset make_asset() {
    engine::gameplay::RagdollAsset asset;
    asset.name = "humanoid";
    asset.autoBalance = true;
    asset.blend.recoverRate = 2.5f;

    engine::gameplay::RagdollJoint pelvis;
    pelvis.name = "pelvis";
    pelvis.parent = "";
    pelvis.anchor = { 0.0f, 1.0f, 0.0f };
    pelvis.length = 0.4f;
    pelvis.radius = 0.18f;
    pelvis.mass = 12.0f;
    pelvis.limits.swingLimitX = 0.6f;
    pelvis.limits.swingLimitY = 0.5f;
    pelvis.limits.twistLimit = 0.3f;

    engine::gameplay::RagdollJoint spine;
    spine.name = "spine";
    spine.parent = "pelvis";
    spine.anchor = { 0.0f, 0.3f, 0.0f };
    spine.rotation = { 0.7071068f, 0.0f, 0.0f, 0.7071068f };
    spine.length = 0.5f;
    spine.radius = 0.14f;
    spine.mass = 8.0f;
    spine.limits.swingLimitX = 0.4f;
    spine.limits.swingLimitY = 0.4f;
    spine.limits.twistLimit = 0.2f;
    spine.drive.enabled = true;
    spine.drive.frequency = 12.0f;
    spine.drive.damping = 1.5f;

    engine::gameplay::RagdollJoint head;
    head.name = "head";
    head.parent = "spine";
    head.anchor = { 0.0f, 0.5f, 0.0f };
    head.length = 0.25f;
    head.radius = 0.12f;
    head.mass = 4.0f;
    head.limits.swingLimitX = 0.8f;
    head.limits.swingLimitY = 0.8f;
    head.limits.twistLimit = 0.5f;

    asset.joints = { pelvis, spine, head };
    return asset;
}

void test_roundtrip() {
    const engine::gameplay::RagdollAsset asset = make_asset();
    std::string error;
    check(asset.validate(error), "asset de referência valida");

    const std::string json = asset.to_json();
    engine::gameplay::RagdollAsset loaded;
    check(loaded.load_from_json(json, error), "load do round-trip aceita");
    check(loaded.to_json() == json, "round-trip bit-exact (re-serializa igual)");
    check(loaded.name == "humanoid" && loaded.autoBalance &&
              loaded.blend.recoverRate == 2.5f,
          "campos de topo preservados");
    check(loaded.joints.size() == 3, "3 joints preservados");
    check(loaded.joints[0].name == "pelvis" && loaded.joints[0].parent.empty(),
          "raiz preservada");
    check(loaded.joints[1].drive.enabled &&
              loaded.joints[1].drive.frequency == 12.0f &&
              loaded.joints[1].drive.damping == 1.5f,
          "drive do spine preservado");
    check(loaded.joints[2].limits.swingLimitX == 0.8f &&
              loaded.joints[2].limits.twistLimit == 0.5f,
          "limits da head preservados");
    check(loaded.joints[1].rotation.w == 0.7071068f, "quat preservado (%.9g)");
}

void test_load_all_or_nothing() {
    engine::gameplay::RagdollAsset asset = make_asset();
    std::string error;
    check(asset.load_from_json(asset.to_json(), error), "baseline carrega");

    const std::string intact = asset.to_json();
    const struct {
        const char* json;
        const char* label;
    } bad[] = {
        { "{", "JSON malformado" },
        { R"({"version":2,"name":"x","joints":[]})", "versão 2" },
        { R"({"version":1,"name":"","joints":[{"name":"a"}]})", "nome vazio" },
        { R"({"version":1,"name":"x","joints":[]})", "joints vazio" },
        { R"({"version":1,"name":"x","joints":"nope"})", "joints não-array" },
        { R"({"version":1,"name":"x","joints":[{"name":"a"},{"name":"a"}]})",
          "nome duplicado" },
        { R"({"version":1,"name":"x","joints":[{"name":"a","parent":"ghost"}]})",
          "parent desconhecido" },
        { R"({"version":1,"name":"x","joints":[{"name":"a","parent":"b"},{"name":"b","parent":"a"}]})",
          "ciclo de parents" },
        { R"({"version":1,"name":"x","joints":[{"name":"a","length":0}]})",
          "length 0" },
        { R"({"version":1,"name":"x","joints":[{"name":"a","mass":-2}]})",
          "mass negativa" },
        { R"({"version":1,"name":"x","joints":[{"name":"a","anchor":[1,2,1e999]}]})",
          "anchor não-finita (inf)" },
        { R"({"version":1,"name":"x","joints":[{"name":"a","limits":{"swingLimitX":-1}}]})",
          "limit negativo" },
        { R"({"version":1,"name":"x","joints":[{"name":"a","drive":{"enabled":true,"frequency":0}}]})",
          "drive com frequency 0" },
        { R"({"version":1,"name":"x","joints":[{"name":"a"}],"blend":{"recoverRate":-0.5}})",
          "recoverRate negativa" },
        { R"({"version":1,"name":"x","joints":[{"name":"a","anchor":[1,2]}]})",
          "anchor com 2 componentes" },
    };
    for (const auto& entry : bad) {
        check(!asset.load_from_json(entry.json, error), entry.label);
        check(asset.to_json() == intact, "estado intacto após rejeição");
    }
}

void test_validate() {
    // Válido.
    engine::gameplay::RagdollAsset asset = make_asset();
    std::string error;
    check(asset.validate(error), "referência valida");

    // Duplicata.
    engine::gameplay::RagdollAsset dup = make_asset();
    dup.joints[0].name = dup.joints[1].name;
    check(!dup.validate(error), "nome duplicado rejeitado");

    // Parent desconhecido.
    engine::gameplay::RagdollAsset unknownParent = make_asset();
    unknownParent.joints[2].parent = "nope";
    check(!unknownParent.validate(error), "parent desconhecido rejeitado");

    // Ciclo.
    engine::gameplay::RagdollAsset cycle = make_asset();
    cycle.joints[0].parent = cycle.joints[2].name;
    cycle.joints[2].parent = cycle.joints[0].name;
    check(!cycle.validate(error), "ciclo rejeitado");

    // Não-finita via bit pattern (guard do /fp:fast, findings #79).
    engine::gameplay::RagdollAsset nanAsset = make_asset();
    const std::uint32_t qnan = 0x7fc00000u;
    std::memcpy(&nanAsset.joints[0].anchor.x, &qnan, sizeof(qnan));
    check(!nanAsset.validate(error), "anchor NaN rejeitada (bit-level)");

    // Massa zero.
    engine::gameplay::RagdollAsset zeroMass = make_asset();
    zeroMass.joints[0].mass = 0.0f;
    check(!zeroMass.validate(error), "mass 0 rejeitada");

    // Drives fora de faixa.
    engine::gameplay::RagdollAsset badDrive = make_asset();
    badDrive.joints[1].drive.enabled = true;
    badDrive.joints[1].drive.damping = -1.0f;
    check(!badDrive.validate(error), "damping negativo rejeitado");

    // Várias raízes são válidas.
    engine::gameplay::RagdollAsset multiRoot = make_asset();
    multiRoot.joints[2].parent = "";
    check(multiRoot.validate(error), "múltiplas raízes válidas");
}

void test_build_bones() {
    const engine::gameplay::RagdollAsset asset = make_asset();
    const std::vector<engine::gameplay::RagdollBone> bones = asset.build_bones();
    check(bones.size() == 3, "3 bones");
    check(bones[0].name == "pelvis" && bones[0].parent.empty(),
          "bone 0 = raiz, nome/parent");
    check(bones[0].position.x == 0.0f && bones[0].position.y == 1.0f &&
              bones[0].position.z == 0.0f,
          "bone 0 anchor → position");
    check(bones[0].length == 0.4f && bones[0].radius == 0.18f &&
              bones[0].mass == 12.0f,
          "bone 0 dimensões/massa");
    check(bones[1].parent == "pelvis", "bone 1 parent");
    check(bones[1].rotation.w == 0.7071068f, "bone 1 rotation preservada");
    check(bones[2].name == "head" && bones[2].parent == "spine",
          "bone 2 ordem preservada");
}

}  // namespace

int main() {
    test_roundtrip();
    test_load_all_or_nothing();
    test_validate();
    test_build_bones();

    if (failures == 0) {
        std::printf("ragdoll_asset_tests: all checks passed\n");
        return 0;
    }
    std::printf("ragdoll_asset_tests: %d failure(s)\n", failures);
    return 1;
}
