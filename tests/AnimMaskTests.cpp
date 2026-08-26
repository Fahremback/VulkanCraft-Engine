// AnimMaskTests — gate do contrato público de máscaras de animação (agente 4
// §4 item 1, unidade "masks"). Prova pesos 0..1 por osso (ausente = 0), a
// aplicação determinística sobre deltas aditivos (#212): pos·w, rot =
// slerp(identidade, rot, w), scale = lerp(1, scale, w); e o round-trip JSON
// bit-exact all-or-nothing.

#include "engine/animation/IAnimMask.hpp"

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

bool approx(double a, double b, double eps = 1e-6) {
    return std::fabs(a - b) <= eps;
}

bool approx_q(const engine::animation::AnimQuat& a,
              const engine::animation::AnimQuat& b, double eps = 1e-6) {
    return approx(a.x, b.x, eps) && approx(a.y, b.y, eps) &&
           approx(a.z, b.z, eps) && approx(a.w, b.w, eps);
}

using engine::animation::AdditiveDelta;
using engine::animation::AnimMaskEntry;
using engine::animation::AnimQuat;
using engine::animation::AnimTransform;
using engine::animation::AnimVec3;
using engine::animation::IAnimMask;
using engine::animation::create_anim_mask;

// Deltas típicos: thigh com 90° yaw + translação, hips com translação.
std::vector<AdditiveDelta> make_deltas() {
    return {
        {"hips", {{1, 0, 0}, AnimQuat{}, {1, 1, 1}}},
        {"thigh", {{0, 0.5, 0}, {0, 0.707106781, 0, 0.707106781}, {1, 1, 1}}},
    };
}

void test_add_mask() {
    auto mask = create_anim_mask();
    std::string err;

    check(mask->add_mask("upper", {{"thigh", 1.0}, {"hips", 0.5}}, err) &&
              err.empty(),
          "máscara válida aceita");
    check(mask->has_mask("upper"), "has_mask upper");
    check(mask->mask_ids().size() == 1 && mask->mask_ids()[0] == "upper",
          "mask_ids");
    check(approx(mask->weight("upper", "thigh"), 1.0, 1e-12),
          "weight thigh = 1.0");
    check(approx(mask->weight("upper", "hips"), 0.5, 1e-12),
          "weight hips = 0.5");
    check(approx(mask->weight("upper", "spine"), 0.0, 1e-12),
          "weight ausente = 0");
    check(approx(mask->weight("ghost", "thigh"), 0.0, 1e-12),
          "weight de máscara inexistente = 0");

    check(!mask->add_mask("", {{"thigh", 1.0}}, err), "id vazio rejeitado");
    check(!mask->add_mask("upper", {{"thigh", 1.0}}, err),
          "id duplicado rejeitado");
    check(!mask->add_mask("m1", {{"", 1.0}}, err), "osso vazio rejeitado");
    check(!mask->add_mask("m1", {{"thigh", 1.5}}, err),
          "peso > 1 rejeitado");
    check(!mask->add_mask("m1", {{"thigh", -0.1}}, err),
          "peso < 0 rejeitado");
    check(!mask->add_mask("m1", {{"thigh", 1.0}, {"thigh", 0.5}}, err),
          "osso duplicado na mesma máscara rejeitado");
    // Falha NÃO corrompe: máscara original intacta.
    check(mask->mask_ids().size() == 1, "estado intacto após recusas");
}

void test_mask_deltas() {
    auto mask = create_anim_mask();
    std::string err;
    check(mask->add_mask("full", {{"thigh", 1.0}, {"hips", 1.0}}, err) &&
              err.empty(),
          "máscara full");
    check(mask->add_mask("half", {{"thigh", 0.5}}, err) && err.empty(),
          "máscara half (só thigh 0.5)");

    const std::vector<AdditiveDelta> deltas = make_deltas();

    // full: deltas inalterados.
    const std::vector<AdditiveDelta> full =
        mask->mask_deltas("full", deltas, err);
    check(err.empty() && full.size() == 2, "mask_deltas full ok");
    if (full.size() == 2) {
        check(approx(full[0].local.position.x, 1.0, 1e-9) &&
                  approx_q(full[0].local.rotation, AnimQuat{}),
              "full: hips inalterado");
        check(approx_q(full[1].local.rotation,
                       AnimQuat{0, 0.707106781, 0, 0.707106781}),
              "full: thigh 90° mantido");
    }

    // half: thigh 90°→45° (slerp(identidade, 90°, 0.5)); pos·0.5.
    const std::vector<AdditiveDelta> half =
        mask->mask_deltas("half", deltas, err);
    check(err.empty() && half.size() == 2, "mask_deltas half ok");
    if (half.size() == 2) {
        check(approx_q(half[1].local.rotation,
                       AnimQuat{0, 0.382683432, 0, 0.923879533}),
              "half: thigh 45° (slerp 0.5)");
        check(approx(half[1].local.position.y, 0.25, 1e-9),
              "half: pos·0.5");
        // hips NÃO listado → peso 0 → identidade.
        check(approx(half[0].local.position.x, 0.0, 1e-9) &&
                  approx_q(half[0].local.rotation, AnimQuat{}),
              "half: hips (ausente) = identidade");
    }

    // Máscara desconhecida → erro all-or-nothing.
    const std::vector<AdditiveDelta> bad =
        mask->mask_deltas("ghost", deltas, err);
    check(bad.empty() && err.find("unknown mask") != std::string::npos,
          "máscara desconhecida → erro");
}

void test_state() {
    auto mask = create_anim_mask();
    std::string err;
    check(mask->add_mask("upper", {{"thigh", 1.0}, {"hips", 0.5}}, err) &&
              err.empty(),
          "add upper");

    const std::string s1 = mask->serialize_state();
    check(!s1.empty(), "serialize não vazio");

    auto mask2 = create_anim_mask();
    check(mask2->deserialize_state(s1, err) && err.empty(),
          "deserialize ok");
    check(mask2->serialize_state() == s1, "round-trip bit-exact");
    check(approx(mask2->weight("upper", "hips"), 0.5, 1e-12),
          "weight restaurado");

    check(!mask2->deserialize_state("{\"m\":{\"thigh\":1.5}}", err),
          "peso fora de [0,1] rejeitado no restore");
    check(!mask2->deserialize_state("{\"m\":[1,2]}", err),
          "máscara não-objeto rejeitada");
    check(!mask2->deserialize_state("[]", err),
          "estado não-objeto rejeitado");
    // Falha NÃO corrompe o estado anterior.
    check(mask2->serialize_state() == s1, "estado intacto após falha");
}

}  // namespace

int main() {
    test_add_mask();
    test_mask_deltas();
    test_state();

    if (g_failures == 0) {
        std::cout << "anim_mask_tests: all checks passed\n";
        return 0;
    }
    std::cout << "anim_mask_tests: " << g_failures << " failure(s)\n";
    return 1;
}
