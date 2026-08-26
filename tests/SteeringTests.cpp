// SteeringTests — gate do contrato público de steering (agente 4 §3 item 3).
// Prova que os behaviors de Reynolds (seek/flee/arrive/pursue/separation/
// alignment/cohesion/obstacle_avoidance/blend) são puros, determinísticos,
// sem NaN, e que o Vec3 self-contained se comporta como documentado.

#include "engine/ai/ISteering.hpp"

#include <cmath>
#include <iostream>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failures;
        std::cout << "FAIL: " << message << "\n";
    }
}

using engine::ai::SteeringAgent;
using engine::ai::SteeringNeighbor;
using engine::ai::SteeringObstacle;
using engine::ai::Vec3;

SteeringAgent agent_at_origin() {
    SteeringAgent a;
    a.position = Vec3{};
    a.velocity = Vec3{};
    a.max_speed = 5.0f;
    a.max_force = 10.0f;
    return a;
}

void test_vec3_basics() {
    const Vec3 a{3.0f, 4.0f, 0.0f};
    check(a.length() == 5.0f, "Vec3::length 3-4-5");
    const Vec3 n = a.normalized();
    check(n.approx(Vec3{0.6f, 0.8f, 0.0f}, 1e-6f), "Vec3::normalized 3-4-5");
    check(a.dot(Vec3{1.0f, 0.0f, 0.0f}) == 3.0f, "Vec3::dot");
    const Vec3 z{};
    check(z.normalized().approx(Vec3{}, 0.0f), "zero.normalized() == zero (sem NaN)");
    check(std::isnan(z.normalized().x) == false, "normalized nunca produz NaN");
}

void test_seek_flee() {
    const SteeringAgent a = agent_at_origin();
    check(seek(a, Vec3{0, 0, 10}).approx(Vec3{0, 0, 5}, 1e-6f), "seek aponta ao alvo com max_speed");
    check(flee(a, Vec3{0, 0, 10}).approx(Vec3{0, 0, -5}, 1e-6f), "flee é o oposto de seek");
}

void test_arrive() {
    const SteeringAgent a = agent_at_origin();
    // longe → velocidade cheia
    check(arrive(a, Vec3{0, 0, 100}, 10.0f).approx(Vec3{0, 0, 5}, 1e-6f),
          "arrive longe = max_speed");
    // dentro do raio → desaceleração linear (5/10 * max_speed)
    check(arrive(a, Vec3{0, 0, 5}, 10.0f).approx(Vec3{0, 0, 2.5f}, 1e-6f),
          "arrive dentro do slowing_radius desacelera");
    // no alvo → zero
    check(arrive(a, Vec3{0, 0, 0}, 10.0f).approx(Vec3{0, 0, 0}, 1e-6f),
          "arrive no alvo = zero");
}

void test_pursue() {
    const SteeringAgent a = agent_at_origin();
    // alvo em (0,0,10) movendo +X a 2, lookahead 1s → previsto (2,0,10)
    const Vec3 out = pursue(a, Vec3{0, 0, 10}, Vec3{2, 0, 0}, 1.0f);
    check(out.approx(Vec3{0.9806f, 0, 4.9029f}, 1e-3f), "pursue mira a posição prevista");
}

void test_separation() {
    const SteeringAgent a = agent_at_origin();
    const std::vector<SteeringNeighbor> neighbors{
        {{0, 1, 0}, {}},
    };
    check(separation(a, neighbors, 2.0f).approx(Vec3{0, -1, 0}, 1e-6f),
          "separation empurra para longe do vizinho");
}

void test_alignment() {
    const SteeringAgent a = agent_at_origin();
    const std::vector<SteeringNeighbor> neighbors{
        {{0, 0, 1}, {3, 0, 0}},
    };
    check(alignment(a, neighbors, 10.0f).approx(Vec3{3, 0, 0}, 1e-6f),
          "alignment iguala a velocidade média dos vizinhos");
}

void test_cohesion() {
    const SteeringAgent a = agent_at_origin();
    const std::vector<SteeringNeighbor> neighbors{
        {{10, 0, 0}, {}},
        {{0, 0, 10}, {}},
    };
    // centro de massa (5,0,5) → seek normalizado * 5
    check(cohesion(a, neighbors, 20.0f).approx(Vec3{3.5355f, 0, 3.5355f}, 1e-3f),
          "cohesion dirige ao centro de massa");
}

void test_obstacle_avoidance() {
    SteeringAgent a = agent_at_origin();
    a.velocity = Vec3{0, 0, 1};
    // obstáculo à frente → força com componente -Z (foge)
    const std::vector<SteeringObstacle> ahead{{Vec3{0, 0, 4}, 1.0f}};
    const Vec3 out = obstacle_avoidance(a, ahead, 10.0f);
    check(out.z < 0.0f && out.x == 0.0f && out.y == 0.0f,
          "obstacle_avoidance foge do obstáculo à frente");

    // obstáculo atrás → ignorado
    const std::vector<SteeringObstacle> behind{{Vec3{0, 0, -4}, 1.0f}};
    check(obstacle_avoidance(a, behind, 10.0f).approx(Vec3{}, 1e-6f),
          "obstacle_avoidance ignora obstáculo atrás");
}

void test_blend() {
    const std::vector<Vec3> forces{{10, 0, 0}, {0, 10, 0}};
    const std::vector<float> weights{1.0f, 1.0f};
    // soma (10,10,0) de comprimento 14.14 → clampado a max_force 5
    check(blend(forces, weights, 5.0f).approx(Vec3{3.5355f, 3.5355f, 0}, 1e-3f),
          "blend soma ponderada clampada a max_force");
}

void test_determinism() {
    const SteeringAgent a = agent_at_origin();
    const std::vector<SteeringNeighbor> neighbors{
        {{10, 0, 0}, {3, 0, 0}},
        {{0, 0, 10}, {0, 3, 0}},
    };
    const std::vector<SteeringObstacle> obstacles{{Vec3{0, 0, 4}, 1.0f}};

    const Vec3 s1 = seek(a, Vec3{1, 2, 3});
    const Vec3 s2 = seek(a, Vec3{1, 2, 3});
    const Vec3 c1 = cohesion(a, neighbors, 20.0f);
    const Vec3 c2 = cohesion(a, neighbors, 20.0f);
    const Vec3 o1 = obstacle_avoidance(a, obstacles, 10.0f);
    const Vec3 o2 = obstacle_avoidance(a, obstacles, 10.0f);

    check(s1.approx(s2, 0.0f), "seek bit-exato entre chamadas");
    check(c1.approx(c2, 0.0f), "cohesion bit-exato entre chamadas");
    check(o1.approx(o2, 0.0f), "obstacle_avoidance bit-exato entre chamadas");
}

}  // namespace

int main() {
    test_vec3_basics();
    test_seek_flee();
    test_arrive();
    test_pursue();
    test_separation();
    test_alignment();
    test_cohesion();
    test_obstacle_avoidance();
    test_blend();
    test_determinism();

    if (g_failures == 0) {
        std::cout << "steering_tests: all checks passed\n";
    } else {
        std::cout << "steering_tests: " << g_failures << " failure(s)\n";
    }
    return g_failures == 0 ? 0 : 1;
}
