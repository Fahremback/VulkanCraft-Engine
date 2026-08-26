// GizmoControllerTests — gate headless do contrato engine/editor IGizmoController.
//
// Verifica a matemática dos gizmos: distância ponto→segmento (hit-test),
// translate/scale delta com snap, e rotação assinada com snap. Casos de
// fronteira: segmento degenerado, vetor no eixo (ângulo indefinido → 0),
// snap desligado (<= 0), direções ortogonais.

#include "engine/editor/IGizmoController.hpp"

#include <cmath>
#include <cstdio>

using engine::editor::create_gizmo_controller;
using engine::editor::GizVec2;
using engine::editor::GizVec3;

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

void test_dist_point_segment() {
    auto g = create_gizmo_controller();
    // ponto no meio do segmento (0,0)-(10,0), p=(5,3) → 3
    check(near(g->dist_point_segment(GizVec2{5, 3}, GizVec2{0, 0}, GizVec2{10, 0}), 3.0f),
          "dist ao meio do segmento");
    // ponto além da ponta → dist ao endpoint (15,0) → 5
    check(near(g->dist_point_segment(GizVec2{15, 0}, GizVec2{0, 0}, GizVec2{10, 0}), 5.0f),
          "dist além da ponta = dist ao endpoint");
    // ponto antes do início → dist ao início
    check(near(g->dist_point_segment(GizVec2{-3, 4}, GizVec2{0, 0}, GizVec2{10, 0}), 5.0f),
          "dist antes do início = dist ao início");
    // segmento degenerado (a == b) → dist ponto→a
    check(near(g->dist_point_segment(GizVec2{3, 4}, GizVec2{0, 0}, GizVec2{0, 0}), 5.0f),
          "segmento degenerado = dist ao ponto");
    // ponto EM cima do segmento → 0
    check(near(g->dist_point_segment(GizVec2{7, 0}, GizVec2{0, 0}, GizVec2{10, 0}), 0.0f),
          "ponto no segmento → 0");
}

void test_translate_delta() {
    auto g = create_gizmo_controller();
    const GizVec3 axis_x{1, 0, 0};
    const GizVec3 axis_y{0, 1, 0};
    // deslocamento puramente no eixo X
    check(near(g->translate_delta(GizVec3{3, 0, 0}, axis_x, 0.0f), 3.0f),
          "delta puro no eixo");
    // componente perpendicular é ignorada
    check(near(g->translate_delta(GizVec3{3, 100, 0}, axis_x, 0.0f), 3.0f),
          "componente perpendicular ignorada");
    // eixo Y não pega deslocamento X
    check(near(g->translate_delta(GizVec3{3, 0, 0}, axis_y, 0.0f), 0.0f),
          "eixo errado → 0");
    // snap 1.0: 3.7 → 4; 3.2 → 3
    check(near(g->translate_delta(GizVec3{3.7f, 0, 0}, axis_x, 1.0f), 4.0f),
          "snap arredonda 3.7 → 4");
    check(near(g->translate_delta(GizVec3{3.2f, 0, 0}, axis_x, 1.0f), 3.0f),
          "snap arredonda 3.2 → 3");
    // snap <= 0 = desligado (nunca arredonda)
    check(near(g->translate_delta(GizVec3{3.7f, 0, 0}, axis_x, -1.0f), 3.7f),
          "snap negativo = desligado");
}

void test_scale_delta() {
    auto g = create_gizmo_controller();
    // escala é a mesma projeção do translate
    check(near(g->scale_delta(GizVec3{0, 2.5f, 0}, GizVec3{0, 1, 0}, 0.0f), 2.5f),
          "scale delta puro");
    check(near(g->scale_delta(GizVec3{0, 2.3f, 0}, GizVec3{0, 1, 0}, 0.5f), 2.5f),
          "scale com snap 0.5 → 2.5");
}

void test_rotate_delta() {
    auto g = create_gizmo_controller();
    const GizVec3 axis_y{0, 1, 0};
    // Right-handed: +X rotacionado +90° ao redor de +Y vai para -Z.
    // Logo ref=+X → v=+Z é rotação de -90° (mesma fórmula do editor).
    const float angle = g->rotate_delta(axis_y, GizVec3{1, 0, 0}, GizVec3{0, 0, 1}, 0.0f);
    check(near(angle, -90.0f), "rotacao -90 ao redor de Y (ref +X → v +Z)");
    // v = -Z → +90
    const float pos = g->rotate_delta(axis_y, GizVec3{1, 0, 0}, GizVec3{0, 0, -1}, 0.0f);
    check(near(pos, 90.0f), "rotacao +90 ao redor de Y (ref +X → v -Z)");
    // v = ref → 0
    check(near(g->rotate_delta(axis_y, GizVec3{1, 0, 0}, GizVec3{1, 0, 0}, 0.0f), 0.0f),
          "v == ref → 0");
    // snap 45: ângulo -37 → -45 (não está na grade)
    const float c37 = std::cos(37.0f * 3.14159265358979323846f / 180.0f);
    const float s37 = std::sin(37.0f * 3.14159265358979323846f / 180.0f);
    const float snapped = g->rotate_delta(axis_y, GizVec3{1, 0, 0},
                                          GizVec3{c37, 0, s37}, 45.0f);
    check(near(snapped, -45.0f), "snap 45: -37 → -45");
    // vetor no eixo (indefinido) → 0 (NUNCA NaN)
    const float on_axis = g->rotate_delta(axis_y, GizVec3{1, 0, 0}, GizVec3{0, 5, 0}, 0.0f);
    check(std::isfinite(on_axis) && near(on_axis, 0.0f), "v no eixo → 0 (sem NaN)");
}

}  // namespace

int main() {
    test_dist_point_segment();
    test_translate_delta();
    test_scale_delta();
    test_rotate_delta();

    if (g_failures == 0) {
        std::printf("ALL PASSED\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
