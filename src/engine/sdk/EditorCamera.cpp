// EditorCamera — adapter do contrato engine/editor IEditorCamera.
//
// A matemática de órbita/pan/zoom/fly do editor agora tem fonte única aqui
// (antes duplicada entre EditorApplication.cpp e EditorApplication_PlayMode.cpp
// com os mesmos clamps copiados). Determinístico: yaw livre, pitch clampada,
// dolly multiplicativo clampado, posição sempre derivada. Sem RNG/relógio.

#include "engine/editor/IEditorCamera.hpp"

#include <sstream>

namespace engine::editor {

namespace {

constexpr float kPi = 3.14159265358979323846f;

float deg_to_rad(float d) { return d * kPi / 180.0f; }

// Direção frontal a partir de yaw/pitch (graus) — o mesmo euler_direction
// que o editor usa, aqui como fonte única.
CamVec3 euler_direction(float yaw, float pitch) {
    const float y = deg_to_rad(yaw);
    const float p = deg_to_rad(pitch);
    const float cp = std::cos(p);
    return CamVec3{std::cos(y) * cp, std::sin(p), std::sin(y) * cp};
}

}  // namespace

namespace {

class EditorCameraImpl : public IEditorCamera {
public:
    explicit EditorCameraImpl(EditorCameraState initial) : m_state(initial) {}

    EditorCameraState state() const override { return m_state; }

    bool orbit(float yaw_delta_deg, float pitch_delta_deg) override {
        m_state.yaw += yaw_delta_deg;
        const float limit = m_state.pitch_limit;
        const float p = m_state.pitch - pitch_delta_deg;
        m_state.pitch = p > limit ? limit : (p < -limit ? -limit : p);
        return true;
    }

    void pan(float dx, float dy) override {
        const float scale = m_state.distance * 0.0016f;
        m_state.target = m_state.target + (-right() * dx + up() * dy) * scale;
    }

    void dolly(float amount) override {
        const float d = m_state.distance * (1.0f - amount);
        m_state.distance = d < m_state.min_distance ? m_state.min_distance
                         : (d > m_state.max_distance ? m_state.max_distance : d);
    }

    void fly(const CamVec3& move, float speed, float dt) override {
        const float len = move.length();
        if (len <= 0.0f) {
            return;  // nada a mover (zero → zero)
        }
        const CamVec3 dir = move * (1.0f / len);
        m_state.target = m_state.target + (dir * speed * dt);
    }

    CamVec3 front() const override {
        return euler_direction(m_state.yaw, m_state.pitch);
    }

    CamVec3 right() const override {
        const CamVec3 f = front();
        // up do mundo = +Y; right = normalize(cross(front, (0,1,0)))
        return CamVec3{-f.z, 0.0f, f.x}.normalized();
    }

    CamVec3 up() const override {
        const CamVec3 r = right();
        const CamVec3 f = front();
        // cross(right, front)
        return CamVec3{r.y * f.z - r.z * f.y,
                       r.z * f.x - r.x * f.z,
                       r.x * f.y - r.y * f.x}.normalized();
    }

    CamVec3 position() const override {
        return m_state.target - euler_direction(m_state.yaw, m_state.pitch) * m_state.distance;
    }

    std::string to_json() const override {
        const CamVec3 pos = position();
        std::ostringstream out;
        out << "{\"yaw\":" << m_state.yaw
            << ",\"pitch\":" << m_state.pitch
            << ",\"distance\":" << m_state.distance
            << ",\"target\":{\"x\":" << m_state.target.x
            << ",\"y\":" << m_state.target.y
            << ",\"z\":" << m_state.target.z
            << "},\"position\":{\"x\":" << pos.x
            << ",\"y\":" << pos.y
            << ",\"z\":" << pos.z << "}}";
        return out.str();
    }

private:
    EditorCameraState m_state;
};

}  // namespace

std::unique_ptr<IEditorCamera> create_editor_camera(EditorCameraState initial) {
    return std::make_unique<EditorCameraImpl>(initial);
}

}  // namespace engine::editor
