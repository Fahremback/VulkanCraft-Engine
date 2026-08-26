#pragma once
// ISteering — contrato público de steering behaviors (agente 4 §3 item 3).
//
// Forças de direção de Reynolds puras e determinísticas: SEM RNG, SEM relógio
// de parede, SEM estado global — as mesmas entradas produzem a mesma saída
// bit-exata entre instâncias. Self-contained (std apenas); nenhuma dependência
// de glm ou de qualquer outro contrato do engine.

#include <cmath>
#include <vector>

namespace engine::ai {

// Vetor 3D mínimo self-contained (sem dependência externa).
struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    Vec3 operator+(const Vec3& o) const { return Vec3{x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return Vec3{x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return Vec3{x * s, y * s, z * s}; }
    Vec3 operator/(float s) const { return Vec3{x / s, y / s, z / s}; }

    float dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    float length_sq() const { return dot(*this); }
    float length() const { return std::sqrt(length_sq()); }
    // vetor zero → vetor zero (NUNCA NaN)
    Vec3 normalized() const {
        const float len = length();
        if (len <= 0.0f) {
            return Vec3{};
        }
        return *this * (1.0f / len);
    }
    // |d| <= eps em cada eixo
    bool approx(const Vec3& o, float eps) const {
        const Vec3 d = *this - o;
        return std::fabs(d.x) <= eps && std::fabs(d.y) <= eps &&
               std::fabs(d.z) <= eps;
    }
};

inline Vec3 operator*(float s, const Vec3& v) { return v * s; }

// Estado cinemático + limites de um agente de steering.
struct SteeringAgent {
    Vec3 position;
    Vec3 velocity;
    float max_speed = 5.0f;   // limite da velocidade desejada
    float max_force = 10.0f;  // limite da força de steering (usado em blend)
};

// Vizinho (posição + velocidade) para flocking.
struct SteeringNeighbor {
    Vec3 position;
    Vec3 velocity;
};

// Obstáculo esférico para avoidance.
struct SteeringObstacle {
    Vec3 center;
    float radius = 1.0f;
};

// --- behaviors (todos puros/determinísticos) -------------------------------

// seek: velocidade desejada até o alvo, limitada a max_speed.
Vec3 seek(const SteeringAgent& agent, const Vec3& target);

// flee: oposto de seek (foge da ameaça).
Vec3 flee(const SteeringAgent& agent, const Vec3& threat);

// arrive: seek com desaceleração linear dentro de slowing_radius (0 no alvo).
Vec3 arrive(const SteeringAgent& agent, const Vec3& target, float slowing_radius);

// pursue: seek da posição futura prevista (target_pos + target_vel * lookahead).
Vec3 pursue(const SteeringAgent& agent, const Vec3& target_pos,
            const Vec3& target_vel, float lookahead);

// separation: afasta dos vizinhos dentro de radius (peso 1/distância, ordem de
// entrada estável — determinístico).
Vec3 separation(const SteeringAgent& agent,
                const std::vector<SteeringNeighbor>& neighbors, float radius);

// alignment: média das velocidades dos vizinhos dentro de radius.
Vec3 alignment(const SteeringAgent& agent,
               const std::vector<SteeringNeighbor>& neighbors, float radius);

// cohesion: direção ao centro de massa dos vizinhos dentro de radius.
Vec3 cohesion(const SteeringAgent& agent,
              const std::vector<SteeringNeighbor>& neighbors, float radius);

// obstacle_avoidance: desvia do obstáculo mais próximo cuja superfície está
// dentro de `lookahead` à frente do agente; força inversamente proporcional à
// distância da superfície.
Vec3 obstacle_avoidance(const SteeringAgent& agent,
                        const std::vector<SteeringObstacle>& obstacles,
                        float lookahead);

// blend: soma ponderada determinística (ordem de entrada), limitada a max_force.
// Se forces.size() != weights.size(), o menor comprimento define o número de
// termos considerados (nenhuma leitura fora de bounds).
Vec3 blend(const std::vector<Vec3>& forces,
           const std::vector<float>& weights, float max_force);

}  // namespace engine::ai
