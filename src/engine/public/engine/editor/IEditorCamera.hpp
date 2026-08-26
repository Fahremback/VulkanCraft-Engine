#pragma once
// IEditorCamera — contrato público de câmera de editor em órbita (agente 2 §B).
//
// Modelo determinístico da câmera viewport-first do editor: órbita (yaw/pitch
// ao redor do alvo), pan (desloca o alvo no plano da câmera), dolly (zoom em
// distância) e fly (WASD no plano da câmera). SEM RNG, SEM relógio de parede,
// SEM estado global — as mesmas entradas produzem a mesma saída bit-exata
// entre instâncias. Self-contained (std apenas, Vec3 próprio — mesmo padrão
// do engine::ai::Vec3); nenhuma dependência de glm ou de outro contrato.
//
// A posição é SEMPRE derivada: `position = target - dir(yaw,pitch) * distance`.
// O editor delega a matemática espalhada (EditorApplication.cpp e
// EditorApplication_PlayMode.cpp tinham os mesmos clamps copiados) a este
// contrato; a GPU/drawing continua no shell.

#include <cmath>
#include <memory>
#include <string>

namespace engine::editor {

// Vetor 3D mínimo self-contained (sem dependência externa).
struct CamVec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    CamVec3() = default;
    CamVec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    CamVec3 operator+(const CamVec3& o) const { return CamVec3{x + o.x, y + o.y, z + o.z}; }
    CamVec3 operator-(const CamVec3& o) const { return CamVec3{x - o.x, y - o.y, z - o.z}; }
    CamVec3 operator-() const { return CamVec3{-x, -y, -z}; }
    CamVec3 operator*(float s) const { return CamVec3{x * s, y * s, z * s}; }
    CamVec3 operator/(float s) const { return CamVec3{x / s, y / s, z / s}; }

    float dot(const CamVec3& o) const { return x * o.x + y * o.y + z * o.z; }
    float length_sq() const { return dot(*this); }
    float length() const { return std::sqrt(length_sq()); }
    // vetor zero → vetor zero (NUNCA NaN)
    CamVec3 normalized() const {
        const float len = length();
        if (len <= 0.0f) {
            return CamVec3{};
        }
        return *this * (1.0f / len);
    }
};

inline CamVec3 operator*(float s, const CamVec3& v) { return v * s; }

// Estado completo da câmera de órbita.
struct EditorCameraState {
    float yaw = -90.0f;      // graus (livre, acumula)
    float pitch = -15.0f;    // graus (clampada em ±pitch_limit)
    float distance = 15.0f;  // distância alvo→câmera
    CamVec3 target{0.0f, 0.0f, 0.0f};  // ponto de órbita

    float fov = 60.0f;         // graus verticais
    float near_plane = 0.1f;
    float far_plane = 50000.0f;

    // Limites (os MESMOS do editor atual — fonte única, sem clamps copiados).
    float pitch_limit = 89.0f;
    float min_distance = 0.5f;
    float max_distance = 5000.0f;
};

// Contrato da câmera de editor.
struct IEditorCamera {
    virtual ~IEditorCamera() = default;

    virtual EditorCameraState state() const = 0;

    // Órbita: yaw livre (yaw += delta), pitch segue a CONVENÇÃO DO EDITOR
    // (pitch -= delta — mesma do frame loop: mouse para baixo diminui o
    // pitch), clampada em ±pitch_limit. all-or-nothing.
    virtual bool orbit(float yaw_delta_deg, float pitch_delta_deg) = 0;

    // Pan: desloca o alvo no plano da câmera. dx/dy em pixels de tela;
    // a escala usa a distância atual (pan mais rápido de longe).
    virtual void pan(float dx, float dy) = 0;

    // Dolly (zoom em distância): multiplicativo, clampado em [min,max].
    virtual void dolly(float amount) = 0;

    // Fly (WASD): move o alvo ao longo dos eixos da câmera.
    // move é um vetor de direção (não normalizado aqui), speed é a velocidade
    // em unidades/segundo e dt o passo em segundos.
    virtual void fly(const CamVec3& move, float speed, float dt) = 0;

    // Atalhos derivados (determinísticos, nunca NaN).
    virtual CamVec3 front() const = 0;
    virtual CamVec3 right() const = 0;
    virtual CamVec3 up() const = 0;

    // Posição derivada: target - dir(yaw,pitch) * distance.
    virtual CamVec3 position() const = 0;

    // JSON determinístico: {"yaw","pitch","distance","target":{x,y,z},
    // "position":{x,y,z}}.
    virtual std::string to_json() const = 0;
};

// Factory do adapter (implementada em src/engine/sdk/EditorCamera.cpp).
std::unique_ptr<IEditorCamera> create_editor_camera(EditorCameraState initial = EditorCameraState{});

}  // namespace engine::editor
