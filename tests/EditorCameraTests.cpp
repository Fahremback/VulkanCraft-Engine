// EditorCameraTests — gate headless do contrato engine/editor IEditorCamera.
//
// Verifica a semântica documentada da câmera de órbita: clamps (pitch ±89,
// distância 0.5..5000), yaw livre, pan/zoom/fly determinísticos, posição
// sempre derivada (target − dir·dist) e JSON estável. Testes red-green:
// cada expectativa descreve um comportamento observável.

#include "engine/editor/IEditorCamera.hpp"

#include <cmath>
#include <cstdio>

using engine::editor::CamVec3;
using engine::editor::create_editor_camera;
using engine::editor::EditorCameraState;

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

bool near_v(const CamVec3& a, const CamVec3& b, float eps = 1e-4f) {
    return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
}

void test_orbit_clamps_pitch() {
    // Convenção do editor: pitch -= delta (mouse para baixo diminui o pitch).
    auto cam = create_editor_camera();
    // pitch default -15; delta -30 (mouse para cima) → -15+30 = +15 (limite ok)
    cam->orbit(0.0f, -30.0f);
    check(near(cam->state().pitch, 15.0f), "orbit sobe pitch dentro do limite");
    // delta -200 → +185 → clampado em +89
    cam->orbit(0.0f, -200.0f);
    check(near(cam->state().pitch, 89.0f), "orbit clamp pitch em +89");
    // delta +200 → -215 → clampado em -89
    cam->orbit(0.0f, 200.0f);
    check(near(cam->state().pitch, -89.0f), "orbit clamp pitch em -89");
}

void test_yaw_accumulates_free() {
    auto cam = create_editor_camera();
    const float base = cam->state().yaw;
    cam->orbit(45.0f, 0.0f);
    check(near(cam->state().yaw, base + 45.0f), "yaw acumula sem clamp");
    cam->orbit(-90.0f, 0.0f);
    check(near(cam->state().yaw, base - 45.0f), "yaw negativo acumula");
}

void test_position_derived() {
    auto cam = create_editor_camera();
    cam->orbit(0.0f, 0.0f);
    const float dist = cam->state().distance;
    const CamVec3 front = cam->front();
    const CamVec3 expected = cam->state().target - front * dist;
    check(near_v(cam->position(), expected), "position = target - front*dist");
    // posição nunca é NaN
    check(std::isfinite(cam->position().x) && std::isfinite(cam->position().y) &&
              std::isfinite(cam->position().z),
          "position finita");
}

void test_dolly_clamps() {
    auto cam = create_editor_camera();
    // dolly para muito perto → clamp 0.5
    cam->dolly(1.0f);  // dist *= (1 - 1) = 0 → clamp 0.5
    check(near(cam->state().distance, 0.5f), "dolly clamp min 0.5");
    // câmera nova, dist default 15; dolly -1000 → dist *= 1001 = 15015 → clamp 5000
    auto far = create_editor_camera();
    far->dolly(-1000.0f);
    check(near(far->state().distance, 5000.0f), "dolly clamp max 5000");
}

void test_pan_scale_by_distance() {
    // pan com distância pequena move pouco; com distância grande move muito.
    auto close_cam = create_editor_camera();
    auto far_cam = create_editor_camera();
    EditorCameraState far_state = far_cam->state();
    far_state.distance = 1000.0f;
    far_cam = create_editor_camera(far_state);

    const CamVec3 close_before = close_cam->state().target;
    const CamVec3 far_before = far_cam->state().target;
    close_cam->pan(100.0f, 0.0f);
    far_cam->pan(100.0f, 0.0f);

    const float close_delta = (close_cam->state().target - close_before).length();
    const float far_delta = (far_cam->state().target - far_before).length();
    check(far_delta > close_delta * 10.0f, "pan escala com a distância");
    check(far_delta > 0.0f && close_delta > 0.0f, "pan move o alvo");
}

void test_fly_zero_move_is_noop() {
    auto cam = create_editor_camera();
    const CamVec3 before = cam->state().target;
    cam->fly(CamVec3{}, 10.0f, 1.0f);
    check(near_v(cam->state().target, before), "fly com vetor zero é no-op");
}

void test_fly_moves_along_front() {
    auto cam = create_editor_camera();
    const CamVec3 before = cam->state().target;
    cam->fly(cam->front(), 2.0f, 1.0f);
    const CamVec3 delta = cam->state().target - before;
    check(near(delta.length(), 2.0f), "fly avança speed*dt na direção");
    check(near_v(delta.normalized(), cam->front()), "fly direção = front");
}

void test_json_deterministic() {
    auto a = create_editor_camera();
    auto b = create_editor_camera();
    check(a->to_json() == b->to_json(), "JSON idêntico entre instâncias");
    a->orbit(10.0f, 5.0f);
    const std::string j = a->to_json();
    check(j.find("\"yaw\"") != std::string::npos, "JSON contém yaw");
    check(j.find("\"position\"") != std::string::npos, "JSON contém position");
    // determinismo: mesma sequência de comandos → mesmo JSON
    auto c = create_editor_camera();
    c->orbit(10.0f, 5.0f);
    check(c->to_json() == j, "JSON determinístico para a mesma sequência");
}

}  // namespace

int main() {
    test_orbit_clamps_pitch();
    test_yaw_accumulates_free();
    test_position_derived();
    test_dolly_clamps();
    test_pan_scale_by_distance();
    test_fly_zero_move_is_noop();
    test_fly_moves_along_front();
    test_json_deterministic();

    if (g_failures == 0) {
        std::printf("ALL PASSED\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
