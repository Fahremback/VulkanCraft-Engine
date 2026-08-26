#include "engine/ai/ISteering.hpp"

#include <cmath>

namespace engine::ai {

namespace {

// Limita um vetor a um comprimento máximo (zero → zero).
Vec3 limit(const Vec3& v, float max_len) {
    const float l2 = v.length_sq();
    if (l2 > max_len * max_len) {
        return v * (max_len / std::sqrt(l2));
    }
    return v;
}

}  // namespace

Vec3 seek(const SteeringAgent& agent, const Vec3& target) {
    const Vec3 desired = limit(target - agent.position, agent.max_speed);
    return desired - agent.velocity;
}

Vec3 flee(const SteeringAgent& agent, const Vec3& threat) {
    const Vec3 desired = limit(agent.position - threat, agent.max_speed);
    return desired - agent.velocity;
}

Vec3 arrive(const SteeringAgent& agent, const Vec3& target, float slowing_radius) {
    const Vec3 to_target = target - agent.position;
    const float dist = to_target.length();
    if (dist <= 0.0f) {
        return Vec3{} - agent.velocity;  // chegou: freia totalmente
    }
    float speed = agent.max_speed;
    if (dist < slowing_radius) {
        speed = agent.max_speed * (dist / slowing_radius);
    }
    const Vec3 desired = to_target.normalized() * speed;
    return desired - agent.velocity;
}

Vec3 pursue(const SteeringAgent& agent, const Vec3& target_pos,
            const Vec3& target_vel, float lookahead) {
    const Vec3 predicted = target_pos + target_vel * lookahead;
    return seek(agent, predicted);
}

Vec3 separation(const SteeringAgent& agent,
                const std::vector<SteeringNeighbor>& neighbors, float radius) {
    Vec3 force{};
    for (const auto& n : neighbors) {
        const Vec3 diff = agent.position - n.position;
        const float d = diff.length();
        if (d > 0.0f && d < radius) {
            force = force + diff.normalized() * (1.0f / d);
        }
    }
    return force;
}

Vec3 alignment(const SteeringAgent& agent,
               const std::vector<SteeringNeighbor>& neighbors, float radius) {
    Vec3 avg{};
    int count = 0;
    for (const auto& n : neighbors) {
        if ((n.position - agent.position).length() < radius) {
            avg = avg + n.velocity;
            ++count;
        }
    }
    if (count == 0) {
        return Vec3{};
    }
    const Vec3 desired =
        limit(avg * (1.0f / static_cast<float>(count)), agent.max_speed);
    return desired - agent.velocity;
}

Vec3 cohesion(const SteeringAgent& agent,
              const std::vector<SteeringNeighbor>& neighbors, float radius) {
    Vec3 center{};
    int count = 0;
    for (const auto& n : neighbors) {
        if ((n.position - agent.position).length() < radius) {
            center = center + n.position;
            ++count;
        }
    }
    if (count == 0) {
        return Vec3{};
    }
    return seek(agent, center * (1.0f / static_cast<float>(count)));
}

Vec3 obstacle_avoidance(const SteeringAgent& agent,
                        const std::vector<SteeringObstacle>& obstacles,
                        float lookahead) {
    const Vec3 vel_dir = agent.velocity.normalized();

    Vec3 force{};
    float nearest = lookahead;  // menor distância de superfície encontrada

    for (const auto& o : obstacles) {
        const Vec3 rel = o.center - agent.position;
        const float dist = rel.length();
        if (dist <= 0.0f) {
            continue;  // obstáculo exatamente sobre o agente — sem direção útil
        }
        const float surface_dist = dist - o.radius;
        if (surface_dist >= nearest) {
            continue;  // superfície longe demais (ou atrás de outro mais próximo)
        }
        if (rel.normalized().dot(vel_dir) < 0.0f) {
            continue;  // atrás do agente
        }
        nearest = surface_dist;
        const Vec3 away = rel.normalized() * -1.0f;  // longe do centro
        force = away * (1.0f / (nearest + 0.001f));
    }
    return force;
}

Vec3 blend(const std::vector<Vec3>& forces,
           const std::vector<float>& weights, float max_force) {
    const std::size_t n = forces.size() < weights.size() ? forces.size()
                                                          : weights.size();
    Vec3 sum{};
    for (std::size_t i = 0; i < n; ++i) {
        sum = sum + forces[i] * weights[i];
    }
    return limit(sum, max_force);
}

}  // namespace engine::ai
