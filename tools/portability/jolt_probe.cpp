// Deterministic-crash replica: floor + box + contacts read (8/8 crash before).
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <memory>

#include "engine/physics/PhysicsRuntime.hpp"

using namespace Engine::Physics;

BodyDesc make_box(const glm::vec3& position, float mass, MotionType motion = MotionType::Dynamic) {
    BodyDesc desc;
    desc.motion = motion;
    desc.position = position;
    desc.mass = mass;
    desc.collider.shape = BoxShape{glm::vec3(0.5f)};
    desc.collider.filter.layer = 1u;
    desc.collider.filter.mask = ~0u;
    return desc;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("== backend: jolt ==\n");
    PhysicsRuntime world({}, PhysicsBackendKind::Jolt);
    const BodyHandle floor = world.create_body(make_box({0.0f, -0.5f, 0.0f}, 0.0f, MotionType::Static));
    const BodyHandle box = world.create_body(make_box({0.0f, 10.0f, 0.0f}, 1.0f));

    int contactFrames = 0;
    for (int i = 0; i < 180; ++i) {
        world.step(1.0f / 60.0f);
        if (!world.contacts().empty()) ++contactFrames;
    }
    const RigidBody* rb = world.body(box);
    const glm::vec3 p = rb ? rb->position : glm::vec3(0.0f);
    const RigidBody* b = rb;
    std::printf("[probe] y=%.3f vy=%.3f contacts=%d\n", p.y, b ? b->linearVelocity.y : 0.0f, contactFrames);
    std::printf("[probe] PROBE_PASS\n");
    return 0;
}
