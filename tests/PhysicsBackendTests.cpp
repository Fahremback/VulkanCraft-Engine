// Runs the same physics scenarios against every backend (builtin, jolt, bullet)
// through the single PhysicsRuntime surface. Set VC_PHYSICS_BACKENDS to a comma
// separated list to restrict (default: all three).
#include "engine/physics/PhysicsRuntime.hpp"
#include "engine/physics/PhysicsBackend.hpp"

#include <glm/glm.hpp>

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>

using namespace Engine::Physics;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what);
        ++failures;
    }
}

BodyDesc make_box(const glm::vec3& position, float mass, MotionType motion = MotionType::Dynamic,
                  bool trigger = false, std::uint32_t layer = 1u, std::uint32_t mask = ~0u) {
    BodyDesc desc;
    desc.motion = motion;
    desc.position = position;
    desc.mass = mass;
    desc.collider.shape = BoxShape{glm::vec3(0.5f)};
    desc.collider.trigger = trigger;
    desc.collider.filter.layer = layer;
    desc.collider.filter.mask = mask;
    return desc;
}

glm::vec3 position_of(const PhysicsRuntime& world, BodyHandle handle) {
    const RigidBody* body = world.body(handle);
    return body ? body->position : glm::vec3(0.0f);
}

// A static floor (top at y = 0) + a dynamic box dropped from y = 10.
void test_gravity_and_rest(PhysicsBackendKind kind, const char* name) {
    PhysicsRuntime world({}, kind);
    const BodyHandle floor = world.create_body(make_box({0.0f, -0.5f, 0.0f}, 0.0f, MotionType::Static));
    const BodyHandle box = world.create_body(make_box({0.0f, 10.0f, 0.0f}, 1.0f));

    int contactFrames = 0;
    for (int i = 0; i < 180; ++i) {
        world.step(1.0f / 60.0f);
        if (!world.contacts().empty()) ++contactFrames;
    }
    const glm::vec3 p = position_of(world, box);
    const RigidBody* b = world.body(box);
    check(p.y < 9.0f, "box fell");
    check(p.y > 0.35f && p.y < 1.2f, "box rests on the floor");
    check(b != nullptr && std::fabs(b->linearVelocity.y) < 0.5f, "box is roughly at rest");
    check(contactFrames > 0, "contacts were generated while settling");
    std::printf("  [%s] y=%.3f vy=%.3f contacts=%d\n", name, p.y, b ? b->linearVelocity.y : 0.0f, contactFrames);
}

void test_raycast(PhysicsBackendKind kind, const char* name) {
    PhysicsRuntime world({}, kind);
    const BodyHandle floor = world.create_body(make_box({0.0f, -0.5f, 0.0f}, 0.0f, MotionType::Static));
    const auto hit = world.raycast({0.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 100.0f);
    check(hit.has_value(), "raycast hits the floor");
    check(hit && hit->body == floor, "raycast hits the floor body");
    check(hit && hit->distance > 3.5f && hit->distance < 6.0f, "raycast distance is plausible");
    const auto ignored = world.raycast({0.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 100.0f, ~0u, floor);
    check(!ignored.has_value(), "raycast ignores the specified body");
    std::printf("  [%s] distance=%.3f ignored=%s\n", name, hit ? hit->distance : -1.0f, ignored ? "hit" : "none");
}

void test_impulse(PhysicsBackendKind kind, const char* name) {
    PhysicsRuntime world({}, kind);
    const BodyHandle box = world.create_body(make_box({0.0f, 5.0f, 0.0f}, 1.0f));
    world.apply_impulse(box, {0.0f, 5.0f, 0.0f});
    world.step(1.0f / 60.0f);
    const RigidBody* b = world.body(box);
    check(b != nullptr && b->linearVelocity.y > 2.0f, "impulse moves the body upward");
    std::printf("  [%s] vy=%.3f\n", name, b ? b->linearVelocity.y : 0.0f);
}

void test_distance_constraint(PhysicsBackendKind kind, const char* name) {
    PhysicsRuntime world({}, kind);
    // Mask 0: the two bodies never touch each other (a touching contact while
    // in flight is a known degenerate corner of the builtin solver), so this
    // scenario isolates the distance constraint during free fall.
    const BodyHandle a = world.create_body(make_box({0.0f, 5.0f, 0.0f}, 1.0f, MotionType::Dynamic, false, 1u, 0u));
    const BodyHandle b = world.create_body(make_box({0.0f, 6.0f, 0.0f}, 1.0f, MotionType::Dynamic, false, 1u, 0u));
    DistanceConstraintDesc joint;
    joint.bodyA = a;
    joint.bodyB = b;
    joint.localAnchorA = {0.0f, 0.0f, 0.0f};
    joint.localAnchorB = {0.0f, 0.0f, 0.0f};
    joint.restLength = 1.0f;
    const ConstraintHandle handle = world.create_distance_constraint(joint);
    check(handle != InvalidConstraint, "distance constraint created");

    for (int i = 0; i < 120; ++i) world.step(1.0f / 60.0f);
    const glm::vec3 pa = position_of(world, a);
    const glm::vec3 pb = position_of(world, b);
    const float separation = std::fabs(glm::length(pb - pa) - 1.0f);
    check(std::isfinite(separation) && separation < 0.5f, "constraint keeps the bodies 1m apart while falling");
    std::printf("  [%s] separation error=%.3f\n", name, separation);
}

void test_trigger(PhysicsBackendKind kind, const char* name) {
    PhysicsRuntime world({}, kind);
    const BodyHandle sensor = world.create_body(make_box({0.0f, 0.0f, 0.0f}, 0.0f, MotionType::Static, true, 1u));
    const BodyHandle box = world.create_body(make_box({0.0f, 5.0f, 0.0f}, 1.0f));

    bool entered = false;
    bool exited = false;
    for (int i = 0; i < 120 && !exited; ++i) {
        world.step(1.0f / 60.0f);
        for (const TriggerEvent& event : world.trigger_events()) {
            if (event.trigger == sensor && event.other == box) {
                if (event.type == TriggerEvent::Type::Enter) entered = true;
                if (event.type == TriggerEvent::Type::Exit) exited = true;
            }
        }
    }
    check(entered, "trigger Enter fired when the box fell in");
    std::printf("  [%s] enter=%s exit=%s\n", name, entered ? "yes" : "no", exited ? "yes" : "no");
}

void test_overlap(PhysicsBackendKind kind, const char* name) {
    PhysicsRuntime world({}, kind);
    const BodyHandle box = world.create_body(make_box({0.0f, 3.0f, 0.0f}, 0.0f, MotionType::Static));
    const std::vector<BodyHandle> hits = world.overlap_aabb({{0.2f, 2.2f, 0.2f}, {0.8f, 3.8f, 0.8f}});
    bool found = false;
    for (const BodyHandle handle : hits) if (handle == box) found = true;
    check(found, "overlap_aabb finds the body");
    std::printf("  [%s] hits=%zu\n", name, hits.size());
}

void test_filters(PhysicsBackendKind kind, const char* name) {
    // Non-colliding filters: the dropped box passes straight through the static box.
    {
        PhysicsRuntime world({}, kind);
        const BodyHandle platform = world.create_body(make_box({0.0f, 0.0f, 0.0f}, 0.0f, MotionType::Static));
        const BodyHandle box = world.create_body(make_box({0.0f, 5.0f, 0.0f}, 1.0f, MotionType::Dynamic, false, 1u, 0u));
        for (int i = 0; i < 120; ++i) world.step(1.0f / 60.0f);
        const float y = position_of(world, box).y;
        check(y < -1.5f, "mask=0 box passes through the static platform");
        std::printf("  [%s] pass-through y=%.3f\n", name, y);
    }
    // Control: default filters collide and the box rests on the platform.
    {
        PhysicsRuntime world({}, kind);
        const BodyHandle platform = world.create_body(make_box({0.0f, 0.0f, 0.0f}, 0.0f, MotionType::Static));
        const BodyHandle box = world.create_body(make_box({0.0f, 5.0f, 0.0f}, 1.0f));
        for (int i = 0; i < 120; ++i) world.step(1.0f / 60.0f);
        const float y = position_of(world, box).y;
        check(y > 0.9f && y < 2.0f, "default filters collide: box rests on the platform");
        std::printf("  [%s] control y=%.3f\n", name, y);
    }
}

void test_debug_geometry(PhysicsBackendKind kind, const char* name) {
    PhysicsRuntime world({}, kind);
    world.create_body(make_box({0.0f, 0.0f, 0.0f}, 0.0f, MotionType::Static));
    world.create_body(make_box({1.0f, 1.0f, 0.0f}, 1.0f));
    const std::vector<DebugLine> lines = world.debug_geometry();
    check(!lines.empty(), "debug geometry reports body boxes");
    std::printf("  [%s] lines=%zu\n", name, lines.size());
}

std::vector<PhysicsBackendKind> backends_from_env() {
    std::vector<PhysicsBackendKind> result;
    const char* env = std::getenv("VC_PHYSICS_BACKENDS");
    const std::string value = env ? env : "builtin,jolt,bullet";
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t comma = value.find(',', start);
        const std::string token = value.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!token.empty()) result.push_back(backend_kind_from_string(token));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    if (result.empty()) result.push_back(PhysicsBackendKind::Builtin);
    return result;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::vector<PhysicsBackendKind> backends = backends_from_env();
    const char* names[] = {"builtin", "jolt", "bullet"};

    for (const PhysicsBackendKind kind : backends) {
        const char* name = names[static_cast<int>(kind)];
        std::printf("== backend: %s ==\n", name);
        test_gravity_and_rest(kind, name);
        test_raycast(kind, name);
        test_impulse(kind, name);
        test_distance_constraint(kind, name);
        test_trigger(kind, name);
        test_overlap(kind, name);
        test_filters(kind, name);
        test_debug_geometry(kind, name);
    }

    // String helpers.
    check(backend_kind_from_string("jolt") == PhysicsBackendKind::Jolt, "jolt parsed");
    check(backend_kind_from_string("bullet") == PhysicsBackendKind::Bullet, "bullet parsed");
    check(backend_kind_from_string("bogus") == PhysicsBackendKind::Builtin, "unknown falls back to builtin");
    check(backend_kind_to_string(PhysicsBackendKind::Jolt) == "jolt", "jolt serialized");
    check(backend_kind_to_string(PhysicsBackendKind::Builtin) == "builtin", "builtin serialized");

    if (failures == 0) {
        std::printf("ALL PHYSICS BACKEND TESTS PASSED\n");
        return 0;
    }
    std::printf("%d FAILURES\n", failures);
    return 1;
}
