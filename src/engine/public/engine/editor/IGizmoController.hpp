#pragma once
// IGizmoController — contrato público de matemática de gizmos do editor
// (agente 2 §B, "câmera e gizmos move/rotate/scale").
//
// As fórmulas de arrasto dos gizmos do editor (translate/rotate/scale com
// snap, e a distância ponto→segmento do hit-test) agora têm fonte única aqui
// — antes embutidas em EditorApplication.cpp com a mesma matemática. A mutação
// da cena + undo continua no editor; este contrato só faz a MATEMÁTICA pura,
// determinística (sem RNG/relógio/estado global). Self-contained (std apenas).

#include <cmath>
#include <memory>

namespace engine::editor {

// Vetor 2D mínimo self-contained.
struct GizVec2 {
    float x = 0.0f;
    float y = 0.0f;

    GizVec2() = default;
    GizVec2(float x_, float y_) : x(x_), y(y_) {}

    GizVec2 operator-(const GizVec2& o) const { return GizVec2{x - o.x, y - o.y}; }
    GizVec2 operator+(const GizVec2& o) const { return GizVec2{x + o.x, y + o.y}; }
    GizVec2 operator*(float s) const { return GizVec2{x * s, y * s}; }
};

// Vetor 3D mínimo self-contained (mesmo padrão do CamVec3 do IEditorCamera).
struct GizVec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    GizVec3() = default;
    GizVec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    GizVec3 operator+(const GizVec3& o) const { return GizVec3{x + o.x, y + o.y, z + o.z}; }
    GizVec3 operator-(const GizVec3& o) const { return GizVec3{x - o.x, y - o.y, z - o.z}; }
    GizVec3 operator-() const { return GizVec3{-x, -y, -z}; }
    GizVec3 operator*(float s) const { return GizVec3{x * s, y * s, z * s}; }

    float dot(const GizVec3& o) const { return x * o.x + y * o.y + z * o.z; }
    float length_sq() const { return dot(*this); }
    float length() const { return std::sqrt(length_sq()); }
    // vetor zero → vetor zero (NUNCA NaN)
    GizVec3 normalized() const {
        const float len = length();
        if (len <= 0.0f) {
            return GizVec3{};
        }
        return *this * (1.0f / len);
    }
    // cross product (usado em rotação e planos)
    GizVec3 cross(const GizVec3& o) const {
        return GizVec3{y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
};

// Modo do gizmo — as três operações de transform do editor.
enum class GizmoMode { Translate, Rotate, Scale };

// Contrato da matemática de gizmos.
struct IGizmoController {
    virtual ~IGizmoController() = default;

    // Distância ponto→segmento em espaço de tela (a MESMA fórmula que o
    // hit-test do editor usa, com t clampado em [0,1]).
    virtual float dist_point_segment(const GizVec2& p, const GizVec2& a,
                                     const GizVec2& b) const = 0;

    // Delta de translate: projeção do deslocamento de mundo no eixo do gizmo,
    // com snap opcional (snap > 0 → round(delta/snap)*snap; snap <= 0 → sem).
    virtual float translate_delta(const GizVec3& world_delta,
                                  const GizVec3& axis_world, float snap) const = 0;

    // Delta de rotação: ângulo assinado (graus) de v ao redor do eixo, medido
    // a partir do vetor de referência ref, com snap opcional em graus.
    // (A MESMA fórmula do editor: atan2(dot(cross(ref,v),axis), dot(ref,v)).)
    virtual float rotate_delta(const GizVec3& axis_world, const GizVec3& ref,
                               const GizVec3& v, float snap_deg) const = 0;

    // Delta de escala: projeção do deslocamento de mundo no eixo do gizmo,
    // com snap opcional.
    virtual float scale_delta(const GizVec3& world_delta,
                              const GizVec3& axis_world, float snap) const = 0;
};

// Factory do adapter (implementada em src/engine/sdk/GizmoController.cpp).
std::unique_ptr<IGizmoController> create_gizmo_controller();

}  // namespace engine::editor
