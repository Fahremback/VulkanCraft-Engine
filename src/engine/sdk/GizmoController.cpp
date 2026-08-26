// GizmoController — adapter do contrato engine/editor IGizmoController.
//
// As fórmulas de arrasto dos gizmos do editor (translate/rotate/scale com
// snap + distância ponto→segmento do hit-test) agora têm fonte única aqui.
// O editor delega a matemática; a mutação da cena + undo continua no editor.
// Determinístico: sem RNG, sem relógio, sem estado global.

#include "engine/editor/IGizmoController.hpp"

namespace engine::editor {

namespace {

constexpr float kPi = 3.14159265358979323846f;

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

float snap_value(float v, float snap) {
    if (snap <= 0.0f) {
        return v;
    }
    return std::round(v / snap) * snap;
}

float rad_to_deg(float r) { return r * 180.0f / kPi; }

class GizmoControllerImpl : public IGizmoController {
public:
    float dist_point_segment(const GizVec2& p, const GizVec2& a,
                             const GizVec2& b) const override {
        const GizVec2 ab = b - a;
        const float len2 = ab.x * ab.x + ab.y * ab.y;
        if (len2 < 1e-8f) {
            const float dx = p.x - a.x;
            const float dy = p.y - a.y;
            return std::sqrt(dx * dx + dy * dy);
        }
        const float t = clamp01(((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / len2);
        const float qx = a.x + ab.x * t;
        const float qy = a.y + ab.y * t;
        const float dx = p.x - qx;
        const float dy = p.y - qy;
        return std::sqrt(dx * dx + dy * dy);
    }

    float translate_delta(const GizVec3& world_delta, const GizVec3& axis_world,
                          float snap) const override {
        return snap_value(world_delta.dot(axis_world), snap);
    }

    float rotate_delta(const GizVec3& axis_world, const GizVec3& ref,
                       const GizVec3& v, float snap_deg) const override {
        const float len = v.length();
        if (len <= 1e-5f) {
            return 0.0f;  // ponto no eixo → ângulo indefinido → delta 0
        }
        const GizVec3 vn = v * (1.0f / len);
        const float angle = rad_to_deg(std::atan2(
            ref.cross(vn).dot(axis_world), ref.dot(vn)));
        return snap_value(angle, snap_deg);
    }

    float scale_delta(const GizVec3& world_delta, const GizVec3& axis_world,
                      float snap) const override {
        return snap_value(world_delta.dot(axis_world), snap);
    }
};

}  // namespace

std::unique_ptr<IGizmoController> create_gizmo_controller() {
    return std::make_unique<GizmoControllerImpl>();
}

}  // namespace engine::editor
