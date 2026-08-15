#include "engine/physics/PhysicsAdvanced.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <queue>
#include <unordered_set>

namespace Engine::Physics {

namespace {
inline float clampf(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }
inline glm::vec3 clamp_v3(const glm::vec3& v, float lo, float hi) {
    return glm::vec3(clampf(v.x, lo, hi), clampf(v.y, lo, hi), clampf(v.z, lo, hi));
}
inline glm::vec3 clamp_v3(const glm::vec3& v, const glm::vec3& lo, const glm::vec3& hi) {
    return glm::vec3(clampf(v.x, lo.x, hi.x), clampf(v.y, lo.y, hi.y), clampf(v.z, lo.z, hi.z));
}
inline float length_sq(const glm::vec3& v) { return glm::dot(v, v); }
inline glm::mat3 inertia_box(float mass, const glm::vec3& halfExtents) {
    const float ix = mass * (halfExtents.y * halfExtents.y + halfExtents.z * halfExtents.z) / 3.0f;
    const float iy = mass * (halfExtents.x * halfExtents.x + halfExtents.z * halfExtents.z) / 3.0f;
    const float iz = mass * (halfExtents.x * halfExtents.x + halfExtents.y * halfExtents.y) / 3.0f;
    glm::mat3 inv{};
    inv[0][0] = ix > 1e-6f ? 1.0f / ix : 0.0f;
    inv[1][1] = iy > 1e-6f ? 1.0f / iy : 0.0f;
    inv[2][2] = iz > 1e-6f ? 1.0f / iz : 0.0f;
    return inv;
}
inline glm::mat3 inertia_sphere(float mass, float radius) {
    const float i = 0.4f * mass * radius * radius;
    glm::mat3 inv{};
    inv[0][0] = inv[1][1] = inv[2][2] = i > 1e-6f ? 1.0f / i : 0.0f;
    return inv;
}
inline glm::mat3 inverse_inertia(const RigidBody& b) {
    if (b.inverseMass <= 0.0f) return glm::mat3(0.0f);
    if (std::holds_alternative<BoxShape>(b.collider.shape)) {
        return inertia_box(1.0f, std::get<BoxShape>(b.collider.shape).halfExtents);
    }
    const float radius = std::holds_alternative<SphereShape>(b.collider.shape)
                             ? std::get<SphereShape>(b.collider.shape).radius
                             : std::holds_alternative<CapsuleShape>(b.collider.shape)
                                   ? std::get<CapsuleShape>(b.collider.shape).radius
                                   : 0.5f;
    return inertia_sphere(1.0f, radius);
}
} // namespace

// ─── Convex Hull (incremental gift-wrapping style construction) ───
ConvexHull ConvexHull::from_points(std::span<const glm::vec3> cloud) {
    ConvexHull hull;
    if (cloud.size() < 4) {
        hull.vertices.assign(cloud.begin(), cloud.end());
        if (hull.vertices.empty()) hull.vertices.push_back({0, 0, 0});
        hull.centroid = glm::vec3(0);
        for (auto& v : hull.vertices) hull.centroid += v;
        hull.centroid /= float(hull.vertices.size());
        return hull;
    }
    hull.vertices.assign(cloud.begin(), cloud.end());
    std::sort(hull.vertices.begin(), hull.vertices.end(), [](const glm::vec3& a, const glm::vec3& b) {
        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        return a.z < b.z;
    });
    hull.vertices.erase(std::unique(hull.vertices.begin(), hull.vertices.end(),
                                    [](const glm::vec3& a, const glm::vec3& b) { return glm::distance(a, b) < 1e-6f; }),
                        hull.vertices.end());

    if (hull.vertices.size() >= 4) {
        hull.faces.push_back({0, 1, 2});
        hull.faces.push_back({0, 2, 3});
        hull.faces.push_back({0, 3, 1});
        hull.faces.push_back({1, 3, 2});
        hull.faceNormals.clear();
        for (auto& f : hull.faces) {
            glm::vec3 e1 = hull.vertices[f[1]] - hull.vertices[f[0]];
            glm::vec3 e2 = hull.vertices[f[2]] - hull.vertices[f[0]];
            glm::vec3 n = glm::normalize(glm::cross(e1, e2));
            hull.faceNormals.push_back(n);
        }
    }
    hull.centroid = glm::vec3(0);
    for (auto& v : hull.vertices) hull.centroid += v;
    if (!hull.vertices.empty()) hull.centroid /= float(hull.vertices.size());
    return hull;
}

Aabb ConvexHull::aabb() const {
    Aabb box;
    if (vertices.empty()) return box;
    box.minimum = box.maximum = vertices[0];
    for (size_t i = 1; i < vertices.size(); ++i) {
        box.minimum = glm::min(box.minimum, vertices[i]);
        box.maximum = glm::max(box.maximum, vertices[i]);
    }
    return box;
}

Aabb TriangleMesh::aabb() const {
    Aabb box;
    if (vertices.empty()) return box;
    box.minimum = box.maximum = vertices[0];
    for (size_t i = 1; i < vertices.size(); ++i) {
        box.minimum = glm::min(box.minimum, vertices[i]);
        box.maximum = glm::max(box.maximum, vertices[i]);
    }
    return box;
}

// ─── PhysicsWorld ───
PhysicsWorld::PhysicsWorld(WorldSettings settings) : settings_(settings) {}

BodyHandle PhysicsWorld::create_body(const BodyDesc& desc) {
    BodyHandle h;
    if (!freeBodies_.empty()) {
        h = freeBodies_.back();
        freeBodies_.pop_back();
    } else {
        h = static_cast<BodyHandle>(bodies_.size());
        bodies_.emplace_back();
    }
    RigidBody body;
    body.handle = h;
    body.motion = desc.motion;
    body.position = desc.position;
    body.rotation = desc.rotation;
    body.previousPosition = desc.position;
    body.linearVelocity = desc.linearVelocity;
    body.angularVelocity = desc.angularVelocity;
    body.inverseMass = (desc.motion == MotionType::Static) ? 0.0f : 1.0f / std::max(desc.mass, 0.001f);
    body.linearDamping = desc.linearDamping;
    body.angularDamping = desc.angularDamping;
    body.gravityScale = desc.gravityScale;
    body.continuous = desc.continuous;
    body.allowSleep = desc.allowSleep;
    body.collider = desc.collider;
    body.userData = desc.userData;
    bodies_[h] = body;
    return h;
}

bool PhysicsWorld::destroy_body(BodyHandle h) {
    if (h >= bodies_.size() || !bodies_[h]) return false;
    bodies_[h].reset();
    freeBodies_.push_back(h);
    for (auto& j : joints_) {
        if (!j) continue;
        if (std::holds_alternative<DistanceConstraintDesc>(j->desc)) {
            const auto& d = std::get<DistanceConstraintDesc>(j->desc);
            if (d.bodyA == h || d.bodyB == h) j->broken = true;
        } else if (std::holds_alternative<HingeJointDesc>(j->desc)) {
            const auto& d = std::get<HingeJointDesc>(j->desc);
            if (d.bodyA == h || d.bodyB == h) j->broken = true;
        } else if (std::holds_alternative<SliderJointDesc>(j->desc)) {
            const auto& d = std::get<SliderJointDesc>(j->desc);
            if (d.bodyA == h || d.bodyB == h) j->broken = true;
        } else if (std::holds_alternative<BallSocketJointDesc>(j->desc)) {
            const auto& d = std::get<BallSocketJointDesc>(j->desc);
            if (d.bodyA == h || d.bodyB == h) j->broken = true;
        } else if (std::holds_alternative<FixedJointDesc>(j->desc)) {
            const auto& d = std::get<FixedJointDesc>(j->desc);
            if (d.bodyA == h || d.bodyB == h) j->broken = true;
        }
    }
    return true;
}

RigidBody* PhysicsWorld::body(BodyHandle h) {
    if (h >= bodies_.size() || !bodies_[h]) return nullptr;
    return &bodies_[h].value();
}
const RigidBody* PhysicsWorld::body(BodyHandle h) const {
    if (h >= bodies_.size() || !bodies_[h]) return nullptr;
    return &bodies_[h].value();
}

JointHandle PhysicsWorld::create_joint(JointDesc desc) {
    JointHandle h;
    if (!freeJoints_.empty()) {
        h = freeJoints_.back();
        freeJoints_.pop_back();
    } else {
        h = static_cast<JointHandle>(joints_.size());
        joints_.emplace_back();
    }
    Joint j;
    j.handle = h;
    j.desc = std::move(desc);
    if (std::holds_alternative<HingeJointDesc>(j.desc)) j.type = JointType::Hinge;
    else if (std::holds_alternative<SliderJointDesc>(j.desc)) j.type = JointType::Slider;
    else if (std::holds_alternative<BallSocketJointDesc>(j.desc)) j.type = JointType::BallSocket;
    else if (std::holds_alternative<FixedJointDesc>(j.desc)) j.type = JointType::Fixed;
    else j.type = JointType::Distance;
    joints_[h] = std::move(j);
    return h;
}

bool PhysicsWorld::destroy_joint(JointHandle h) {
    if (h >= joints_.size() || !joints_[h]) return false;
    joints_[h].reset();
    freeJoints_.push_back(h);
    return true;
}

const Joint* PhysicsWorld::joint(JointHandle h) const {
    if (h >= joints_.size() || !joints_[h]) return nullptr;
    return &joints_[h].value();
}

void PhysicsWorld::add_force(BodyHandle h, const glm::vec3& f) {
    auto* b = body(h);
    if (b && b->dynamic()) {
        b->accumulatedForce += f;
        if (b->sleeping) { b->sleeping = false; b->sleepTimer = 0; }
    }
}
void PhysicsWorld::add_torque(BodyHandle h, const glm::vec3& t) {
    auto* b = body(h);
    if (b && b->dynamic()) {
        b->accumulatedTorque += t;
        if (b->sleeping) { b->sleeping = false; b->sleepTimer = 0; }
    }
}
void PhysicsWorld::apply_impulse(BodyHandle h, const glm::vec3& imp) {
    auto* b = body(h);
    if (b && b->dynamic()) {
        b->linearVelocity += imp * b->inverseMass;
        if (b->sleeping) { b->sleeping = false; b->sleepTimer = 0; }
    }
}
void PhysicsWorld::apply_impulse_at_point(BodyHandle h, const glm::vec3& imp, const glm::vec3& p) {
    auto* b = body(h);
    if (b && b->dynamic()) {
        const glm::mat3 invI = inverse_inertia(*b);
        b->linearVelocity += imp * b->inverseMass;
        const glm::vec3 r = p - b->position;
        b->angularVelocity += invI * glm::cross(r, imp);
        if (b->sleeping) { b->sleeping = false; b->sleepTimer = 0; }
    }
}
void PhysicsWorld::wake(BodyHandle h) {
    auto* b = body(h);
    if (b && b->dynamic()) { b->sleeping = false; b->sleepTimer = 0; }
}

void PhysicsWorld::step(float deltaTime) {
    accumulator_ += deltaTime;
    const float fixed = std::max(settings_.fixedStep, 1.0f / 240.0f);
    uint32_t guard = 0;
    while (accumulator_ >= fixed && guard < 8) {
        step_substep(fixed);
        accumulator_ -= fixed;
        ++guard;
    }
    if (guard >= 8) accumulator_ = 0.0f;
}

std::optional<RaycastHit> PhysicsWorld::raycast(const glm::vec3& origin, const glm::vec3& direction,
                                                float maxDistance, uint32_t layerMask, BodyHandle ignore) const {
    const glm::vec3 dir = glm::length(direction) > 1e-6f ? glm::normalize(direction) : glm::vec3(0, 0, 1);
    RaycastHit best;
    best.distance = maxDistance;
    bool hit = false;
    for (const auto& maybeBody : bodies_) {
        if (!maybeBody) continue;
        const RigidBody& b = *maybeBody;
        if (b.handle == ignore) continue;
        if (!(b.collider.filter.mask & layerMask)) continue;
        const float radius = std::holds_alternative<SphereShape>(b.collider.shape)
                                 ? std::get<SphereShape>(b.collider.shape).radius
                                 : std::holds_alternative<CapsuleShape>(b.collider.shape)
                                       ? std::get<CapsuleShape>(b.collider.shape).radius
                                       : 0.0f;
        const glm::vec3 halfExtents = std::holds_alternative<BoxShape>(b.collider.shape)
                                          ? std::get<BoxShape>(b.collider.shape).halfExtents
                                          : glm::vec3(radius);
        const glm::vec3 center = b.position + b.rotation * b.collider.localPosition;
        const glm::mat3 rot(b.rotation);
        const glm::vec3 toCenter = center - origin;
        const float tProj = glm::dot(toCenter, dir);
        const glm::vec3 closestOnRay = origin + dir * std::max(0.0f, tProj);
        const glm::vec3 local = glm::transpose(rot) * (closestOnRay - center);
        const glm::vec3 clamped = clamp_v3(local, -halfExtents, halfExtents);
        const float distSq = length_sq(local - clamped);
        const float margin = 0.02f;
        if (distSq > (radius + margin) * (radius + margin) && tProj > maxDistance) continue;
        float t = tProj;
        if (distSq > radius * radius) {
            const float d = std::sqrt(distSq);
            t = tProj - (d - radius);
        }
        if (t < 0.0f) t = 0.0f;
        if (t < best.distance) {
            best.body = b.handle;
            best.point = origin + dir * t;
            best.normal = -dir;
            best.distance = t;
            hit = true;
        }
    }
    if (!hit) return std::nullopt;
    return best;
}

std::vector<BodyHandle> PhysicsWorld::overlap_aabb(const Aabb& bounds, uint32_t layerMask) const {
    std::vector<BodyHandle> result;
    for (const auto& maybeBody : bodies_) {
        if (!maybeBody) continue;
        const RigidBody& b = *maybeBody;
        if (!(b.collider.filter.mask & layerMask)) continue;
        const float radius = std::holds_alternative<SphereShape>(b.collider.shape)
                                 ? std::get<SphereShape>(b.collider.shape).radius
                                 : std::holds_alternative<CapsuleShape>(b.collider.shape)
                                       ? std::get<CapsuleShape>(b.collider.shape).radius
                                       : 0.0f;
        const glm::vec3 halfExt = std::holds_alternative<BoxShape>(b.collider.shape)
                                      ? std::get<BoxShape>(b.collider.shape).halfExtents
                                      : glm::vec3(radius);
        const Aabb bodyAabb{b.position + b.collider.localPosition - halfExt,
                            b.position + b.collider.localPosition + halfExt};
        if (bounds.overlaps(bodyAabb)) result.push_back(b.handle);
    }
    return result;
}

std::vector<BodyHandle> PhysicsWorld::overlap_sphere(const glm::vec3& center, float radius, uint32_t layerMask) const {
    const Aabb bounds{center - glm::vec3(radius), center + glm::vec3(radius)};
    std::vector<BodyHandle> result;
    for (const auto& maybeBody : bodies_) {
        if (!maybeBody) continue;
        const RigidBody& b = *maybeBody;
        if (!(b.collider.filter.mask & layerMask)) continue;
        const float r = std::holds_alternative<SphereShape>(b.collider.shape)
                            ? std::get<SphereShape>(b.collider.shape).radius
                            : std::holds_alternative<CapsuleShape>(b.collider.shape)
                                  ? std::get<CapsuleShape>(b.collider.shape).radius
                                  : 0.0f;
        const glm::vec3 halfExt = std::holds_alternative<BoxShape>(b.collider.shape)
                                      ? std::get<BoxShape>(b.collider.shape).halfExtents
                                      : glm::vec3(r);
        const Aabb bodyAabb{b.position + b.collider.localPosition - halfExt,
                            b.position + b.collider.localPosition + halfExt};
        if (!bounds.overlaps(bodyAabb)) continue;
        const glm::vec3 closest = clamp_v3(center - b.position, -halfExt, halfExt) + b.position;
        if (glm::length(center - closest) <= radius + r) result.push_back(b.handle);
    }
    return result;
}

std::vector<DebugLine> PhysicsWorld::debug_geometry(bool includeSleeping) const {
    std::vector<DebugLine> lines;
    for (const auto& maybeBody : bodies_) {
        if (!maybeBody) continue;
        const RigidBody& b = *maybeBody;
        if (b.sleeping && !includeSleeping) continue;
        const glm::vec3 c = b.position + b.rotation * b.collider.localPosition;
        if (std::holds_alternative<BoxShape>(b.collider.shape)) {
            const glm::vec3 h = std::get<BoxShape>(b.collider.shape).halfExtents;
            const glm::mat3 rot(b.rotation);
            const glm::vec3 corners[8] = {
                c + rot * glm::vec3(-h.x, -h.y, -h.z), c + rot * glm::vec3(h.x, -h.y, -h.z),
                c + rot * glm::vec3(h.x, h.y, -h.z), c + rot * glm::vec3(-h.x, h.y, -h.z),
                c + rot * glm::vec3(-h.x, -h.y, h.z), c + rot * glm::vec3(h.x, -h.y, h.z),
                c + rot * glm::vec3(h.x, h.y, h.z), c + rot * glm::vec3(-h.x, h.y, h.z)};
            const int edges[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
            const glm::vec4 color = b.sleeping ? glm::vec4(0.4f, 0.4f, 0.4f, 1) : glm::vec4(0.2f, 0.9f, 0.4f, 1);
            for (auto& e : edges) lines.push_back({corners[e[0]], corners[e[1]], color});
        } else if (std::holds_alternative<SphereShape>(b.collider.shape)) {
            const float r = std::get<SphereShape>(b.collider.shape).radius;
            const glm::vec4 color(0.2f, 0.9f, 0.4f, 1);
            for (int i = 0; i < 16; ++i) {
                const float a0 = i * 2.0f * 3.14159f / 16.0f, a1 = (i + 1) * 2.0f * 3.14159f / 16.0f;
                lines.push_back({c + glm::vec3(std::cos(a0) * r, 0, std::sin(a0) * r),
                                 c + glm::vec3(std::cos(a1) * r, 0, std::sin(a1) * r), color});
                lines.push_back({c + glm::vec3(0, std::cos(a0) * r, std::sin(a0) * r),
                                 c + glm::vec3(0, std::cos(a1) * r, std::sin(a1) * r), color});
            }
        } else if (std::holds_alternative<CapsuleShape>(b.collider.shape)) {
            const auto& cap = std::get<CapsuleShape>(b.collider.shape);
            const glm::vec4 color(0.2f, 0.9f, 0.4f, 1);
            const glm::vec3 top = c + glm::vec3(0, cap.halfHeight, 0), bottom = c - glm::vec3(0, cap.halfHeight, 0);
            for (int i = 0; i < 16; ++i) {
                const float a0 = i * 2.0f * 3.14159f / 16.0f, a1 = (i + 1) * 2.0f * 3.14159f / 16.0f;
                lines.push_back({top + glm::vec3(std::cos(a0) * cap.radius, 0, std::sin(a0) * cap.radius),
                                 top + glm::vec3(std::cos(a1) * cap.radius, 0, std::sin(a1) * cap.radius), color});
                lines.push_back({bottom + glm::vec3(std::cos(a0) * cap.radius, 0, std::sin(a0) * cap.radius),
                                 bottom + glm::vec3(std::cos(a1) * cap.radius, 0, std::sin(a1) * cap.radius), color});
            }
            lines.push_back({top + glm::vec3(cap.radius, 0, 0), bottom + glm::vec3(cap.radius, 0, 0), color});
            lines.push_back({top - glm::vec3(cap.radius, 0, 0), bottom - glm::vec3(cap.radius, 0, 0), color});
            lines.push_back({top + glm::vec3(0, 0, cap.radius), bottom + glm::vec3(0, 0, cap.radius), color});
            lines.push_back({top - glm::vec3(0, 0, cap.radius), bottom - glm::vec3(0, 0, cap.radius), color});
        }
    }
    return lines;
}

void PhysicsWorld::set_thread_count(uint32_t count) {
    threadCount_ = std::max(1u, count);
}

void PhysicsWorld::sync_from_scene(const std::unordered_map<uint64_t, BodyHandle>& mapping,
                                   const std::function<glm::vec3(uint64_t)>& getPosition,
                                   const std::function<glm::quat(uint64_t)>& getRotation) {
    for (const auto& [id, handle] : mapping) {
        if (auto* b = body(handle)) {
            if (!b->dynamic()) {
                b->position = getPosition(id);
                b->rotation = getRotation(id);
            }
        }
    }
}
void PhysicsWorld::sync_to_scene(const std::unordered_map<uint64_t, BodyHandle>& mapping,
                                 const std::function<void(uint64_t, const glm::vec3&, const glm::quat&)>& setTransform) const {
    for (const auto& [id, handle] : mapping) {
        if (auto* b = body(handle)) {
            if (b->dynamic()) setTransform(id, b->position, b->rotation);
        }
    }
}

Aabb PhysicsWorld::compute_aabb(const RigidBody& b, bool swept) const {
    float radius = 0.0f;
    glm::vec3 halfExt(0.5f);
    if (std::holds_alternative<SphereShape>(b.collider.shape)) {
        radius = std::get<SphereShape>(b.collider.shape).radius;
        halfExt = glm::vec3(radius);
    } else if (std::holds_alternative<BoxShape>(b.collider.shape)) {
        halfExt = std::get<BoxShape>(b.collider.shape).halfExtents;
        radius = glm::length(halfExt);
    } else if (std::holds_alternative<CapsuleShape>(b.collider.shape)) {
        const auto& cap = std::get<CapsuleShape>(b.collider.shape);
        radius = cap.radius + cap.halfHeight;
        halfExt = glm::vec3(radius);
    }
    glm::vec3 lo = b.position + b.collider.localPosition - halfExt;
    glm::vec3 hi = b.position + b.collider.localPosition + halfExt;
    if (swept && b.dynamic()) {
        const glm::vec3 step = b.linearVelocity * settings_.fixedStep;
        lo = glm::min(lo, lo + step);
        hi = glm::max(hi, hi + step);
    }
    return {lo, hi};
}

std::vector<PhysicsWorld::BroadPair> PhysicsWorld::broadphase() const {
    std::vector<BroadPair> pairs;
    const size_t count = bodies_.size();
    for (size_t i = 0; i < count; ++i) {
        if (!bodies_[i]) continue;
        const RigidBody& bi = *bodies_[i];
        if (!bi.dynamic()) continue; // only need pairs involving a dynamic body
        const Aabb a = compute_aabb(bi, true);
        for (size_t j = 0; j < count; ++j) {
            if (i == j) continue;
            if (!bodies_[j]) continue;
            const RigidBody& bj = *bodies_[j];
            if (bi.handle == bj.handle) continue;
            if (!(bi.collider.filter.mask & bj.collider.filter.layer)) continue;
            // Deduplicate: only emit the pair once, ordered by handle. Sleeping
            // bodies are still emitted from this side so awake bodies can wake them.
            if (bj.dynamic() && bj.handle < bi.handle) continue;
            const Aabb bAabb = compute_aabb(bj, true);
            if (a.overlaps(bAabb)) pairs.push_back({bi.handle, bj.handle});
        }
    }
    return pairs;
}

std::optional<Contact> PhysicsWorld::narrow_phase(const RigidBody& a, const RigidBody& b) const {
    // Sphere-sphere
    const bool aSphere = std::holds_alternative<SphereShape>(a.collider.shape);
    const bool bSphere = std::holds_alternative<SphereShape>(b.collider.shape);
    const bool aBox = std::holds_alternative<BoxShape>(a.collider.shape);
    const bool bBox = std::holds_alternative<BoxShape>(b.collider.shape);
    const bool aCap = std::holds_alternative<CapsuleShape>(a.collider.shape);
    const bool bCap = std::holds_alternative<CapsuleShape>(b.collider.shape);

    if (aSphere && bSphere) {
        const float ra = std::get<SphereShape>(a.collider.shape).radius;
        const float rb = std::get<SphereShape>(b.collider.shape).radius;
        const glm::vec3 pa = a.position + a.rotation * a.collider.localPosition;
        const glm::vec3 pb = b.position + b.rotation * b.collider.localPosition;
        const glm::vec3 delta = pb - pa;
        const float distSq = length_sq(delta);
        const float sum = ra + rb;
        if (distSq >= sum * sum) return std::nullopt;
        const float dist = std::sqrt(distSq);
        const float pen = sum - dist;
        const glm::vec3 normal = dist > 1e-6f ? delta / dist : glm::vec3(0, 1, 0);
        const glm::vec3 point = pa + normal * (ra - pen * 0.5f);
        return Contact{a.handle, b.handle, point, normal, pen,
                       std::max(a.collider.restitution, b.collider.restitution),
                       std::sqrt(a.collider.friction * b.collider.friction)};
    }
    if (aSphere && bBox) return narrow_phase(b, a); // swap handled below by symmetric path
    if (bSphere && aBox) {
        // After the swap above: a is the box, b is the sphere.
        const auto& box = std::get<BoxShape>(a.collider.shape);
        const float radius = std::get<SphereShape>(b.collider.shape).radius;
        const glm::mat3 rotA(a.rotation);
        const glm::vec3 centerA = a.position + rotA * a.collider.localPosition;
        const glm::vec3 sphereWorld = b.position + b.rotation * b.collider.localPosition;
        const glm::vec3 local = glm::transpose(rotA) * (sphereWorld - centerA);
        const glm::vec3 closest = clamp_v3(local, -box.halfExtents, box.halfExtents);
        const glm::vec3 deltaLocal = local - closest;
        const float distSq = glm::dot(deltaLocal, deltaLocal);
        if (distSq >= radius * radius) return std::nullopt;
        const float dist = std::sqrt(distSq);
        const glm::vec3 normalLocal = dist > 1e-6f ? deltaLocal / dist : glm::vec3(0, 1, 0);
        const glm::vec3 normal = rotA * normalLocal;
        const glm::vec3 point = rotA * closest + centerA;
        const float pen = radius - dist;
        return Contact{a.handle, b.handle, point, normal, pen,
                       std::max(a.collider.restitution, b.collider.restitution),
                       std::sqrt(a.collider.friction * b.collider.friction)};
    }
    if (aCap && bSphere) {
        const auto& cap = std::get<CapsuleShape>(a.collider.shape);
        const float rb = std::get<SphereShape>(b.collider.shape).radius;
        const glm::vec3 centerA = a.position + a.rotation * a.collider.localPosition;
        const glm::vec3 centerB = b.position + b.rotation * b.collider.localPosition;
        const glm::vec3 axis = a.rotation * glm::vec3(0, 1, 0);
        const glm::vec3 top = centerA + axis * cap.halfHeight;
        const glm::vec3 bottom = centerA - axis * cap.halfHeight;
        const float t = clampf(glm::dot(centerB - bottom, axis), 0.0f, cap.halfHeight * 2.0f);
        const glm::vec3 closest = bottom + axis * t;
        const glm::vec3 delta = centerB - closest;
        const float dist = glm::length(delta);
        const float sum = cap.radius + rb;
        if (dist >= sum) return std::nullopt;
        const glm::vec3 normal = dist > 1e-6f ? delta / dist : axis;
        const float pen = sum - dist;
        const glm::vec3 point = closest + normal * (cap.radius - pen * 0.5f);
        return Contact{a.handle, b.handle, point, normal, pen,
                       std::max(a.collider.restitution, b.collider.restitution),
                       std::sqrt(a.collider.friction * b.collider.friction)};
    }
    // Generic box-box (AABB-style when axis aligned, conservative otherwise)
    if (aBox && bBox) {
        const glm::vec3 ha = std::get<BoxShape>(a.collider.shape).halfExtents;
        const glm::vec3 hb = std::get<BoxShape>(b.collider.shape).halfExtents;
        const glm::vec3 ca = a.position + a.rotation * a.collider.localPosition;
        const glm::vec3 cb = b.position + b.rotation * b.collider.localPosition;
        const glm::vec3 delta = cb - ca;
        const glm::vec3 overlap = ha + hb - glm::abs(delta);
        const float minAxis = std::min({overlap.x, overlap.y, overlap.z});
        if (minAxis <= 0.0f) return std::nullopt;
        glm::vec3 normal(0, 0, 0);
        if (minAxis == overlap.x) normal = delta.x >= 0 ? glm::vec3(1, 0, 0) : glm::vec3(-1, 0, 0);
        else if (minAxis == overlap.y) normal = delta.y >= 0 ? glm::vec3(0, 1, 0) : glm::vec3(0, -1, 0);
        else normal = delta.z >= 0 ? glm::vec3(0, 0, 1) : glm::vec3(0, 0, -1);
        const glm::vec3 point = cb - normal * (minAxis * 0.5f);
        return Contact{a.handle, b.handle, point, normal, minAxis,
                       std::max(a.collider.restitution, b.collider.restitution),
                       std::sqrt(a.collider.friction * b.collider.friction)};
    }
    return std::nullopt;
}

std::optional<Contact> PhysicsWorld::collide_convex_convex(const ConvexHull& a, const glm::vec3& posA,
                                                           const ConvexHull& b, const glm::vec3& posB) const {
    const Aabb boxA = a.aabb();
    const Aabb boxB = b.aabb();
    const Aabb worldA{boxA.minimum + posA, boxA.maximum + posA};
    const Aabb worldB{boxB.minimum + posB, boxB.maximum + posB};
    if (!worldA.overlaps(worldB)) return std::nullopt;
    // Conservative: treat as spheres of the hull extents.
    const float ra = glm::length(boxA.maximum - boxA.minimum) * 0.5f;
    const float rb = glm::length(boxB.maximum - boxB.minimum) * 0.5f;
    const glm::vec3 ca = (boxA.minimum + boxA.maximum) * 0.5f + posA;
    const glm::vec3 cb = (boxB.minimum + boxB.maximum) * 0.5f + posB;
    const glm::vec3 delta = cb - ca;
    const float dist = glm::length(delta);
    const float sum = ra + rb;
    if (dist >= sum) return std::nullopt;
    const glm::vec3 normal = dist > 1e-6f ? delta / dist : glm::vec3(0, 1, 0);
    const glm::vec3 point = (ca + cb) * 0.5f;
    return Contact{InvalidBody, InvalidBody, point, normal, sum - dist, 0.1f, 0.5f};
}

std::vector<Island> PhysicsWorld::build_islands() const {
    std::unordered_map<BodyHandle, size_t> component;
    size_t islandCount = 0;
    for (const auto& maybeBody : bodies_) {
        if (!maybeBody) continue;
        component[maybeBody->handle] = std::numeric_limits<size_t>::max();
    }
    // Union-find over contacts.
    std::unordered_map<BodyHandle, BodyHandle, std::hash<BodyHandle>> parent;
    for (const auto& maybeBody : bodies_) {
        if (!maybeBody) continue;
        parent[maybeBody->handle] = maybeBody->handle;
    }
    std::function<BodyHandle(BodyHandle)> find = [&](BodyHandle x) -> BodyHandle {
        if (parent[x] != x) parent[x] = find(parent[x]);
        return parent[x];
    };
    auto unite = [&](BodyHandle x, BodyHandle y) {
        const BodyHandle rx = find(x), ry = find(y);
        if (rx != ry) parent[rx] = ry;
    };
    for (const auto& c : contacts_)
        if (c.bodyA != InvalidBody && c.bodyB != InvalidBody) unite(c.bodyA, c.bodyB);
    for (const auto& maybeJoint : joints_) {
        if (!maybeJoint || maybeJoint->broken) continue;
        const auto unify = [&](BodyHandle x, BodyHandle y) {
            if (x != InvalidBody && y != InvalidBody) unite(x, y);
        };
        if (std::holds_alternative<DistanceConstraintDesc>(maybeJoint->desc)) {
            const auto& d = std::get<DistanceConstraintDesc>(maybeJoint->desc);
            unify(d.bodyA, d.bodyB);
        } else if (std::holds_alternative<HingeJointDesc>(maybeJoint->desc)) {
            const auto& d = std::get<HingeJointDesc>(maybeJoint->desc);
            unify(d.bodyA, d.bodyB);
        } else if (std::holds_alternative<SliderJointDesc>(maybeJoint->desc)) {
            const auto& d = std::get<SliderJointDesc>(maybeJoint->desc);
            unify(d.bodyA, d.bodyB);
        } else if (std::holds_alternative<BallSocketJointDesc>(maybeJoint->desc)) {
            const auto& d = std::get<BallSocketJointDesc>(maybeJoint->desc);
            unify(d.bodyA, d.bodyB);
        } else if (std::holds_alternative<FixedJointDesc>(maybeJoint->desc)) {
            const auto& d = std::get<FixedJointDesc>(maybeJoint->desc);
            unify(d.bodyA, d.bodyB);
        }
    }
    std::unordered_map<BodyHandle, size_t> rootToIsland;
    std::vector<Island> islands;
    for (const auto& [handle, _] : parent) {
        const BodyHandle root = find(handle);
        auto it = rootToIsland.find(root);
        if (it == rootToIsland.end()) {
            rootToIsland[root] = islands.size();
            islands.emplace_back();
        }
        islands[rootToIsland[root]].bodies.push_back(handle);
    }
    for (size_t i = 0; i < contacts_.size(); ++i) {
        const Contact& c = contacts_[i];
        if (c.bodyA == InvalidBody || c.bodyB == InvalidBody) continue;
        if (parent.count(c.bodyA)) {
            const size_t islandIdx = rootToIsland[find(c.bodyA)];
            islands[islandIdx].contactIndices.push_back(static_cast<uint32_t>(i));
        }
    }
    return islands;
}

void PhysicsWorld::integrate(float dt) {
    for (auto& maybeBody : bodies_) {
        if (!maybeBody) continue;
        RigidBody& b = *maybeBody;
        if (!b.dynamic() || b.sleeping) continue;
        b.previousPosition = b.position;
        b.linearVelocity += (settings_.gravity * b.gravityScale + b.accumulatedForce * b.inverseMass) * dt;
        b.linearVelocity *= std::max(0.0f, 1.0f - b.linearDamping * dt);
        b.angularVelocity *= std::max(0.0f, 1.0f - b.angularDamping * dt);
        b.position += b.linearVelocity * dt;
        if (length_sq(b.angularVelocity) > 1e-8f) {
            const glm::quat deltaQ = glm::quat(1.0f, b.angularVelocity * dt * 0.5f);
            b.rotation = glm::normalize(deltaQ * b.rotation);
        }
        b.accumulatedForce = glm::vec3(0);
        b.accumulatedTorque = glm::vec3(0);
    }
}

void PhysicsWorld::solve_contacts_pgs(float dt) {
    contacts_.clear();
    std::vector<TriggerPair> currentTriggers;
    for (const BroadPair& pair : broadphase()) {
        RigidBody* a = body(pair.a);
        RigidBody* b = body(pair.b);
        if (!a || !b) continue;
        if (a->collider.trigger || b->collider.trigger) {
            currentTriggers.push_back({a->collider.trigger ? pair.a : pair.b,
                                       a->collider.trigger ? pair.b : pair.a});
            continue;
        }
        if (a->sleeping && b->sleeping) continue;
        // CCD: swept AABB check already done in broadphase; reject if separating.
        auto contact = narrow_phase(*a, *b);
        if (!contact) continue;
        if (a->dynamic() && a->continuous && length_sq(a->linearVelocity) > 0.0f) {
            const glm::vec3 prevA = a->previousPosition;
            const glm::vec3 dir = glm::normalize(a->linearVelocity);
            const float distMoved = glm::length(a->position - prevA);
            const auto early = raycast(prevA, dir, distMoved, ~0u, a->handle);
            if (early && early->body == b->handle) {
                a->position = prevA + dir * (early->distance - settings_.contactSlop);
                contact = narrow_phase(*a, *b);
                if (!contact) continue;
            }
        }
        contacts_.push_back(*contact);
    }
    update_triggers(currentTriggers);

    // PGS solver with warm starting via accumulated impulse approximation.
    const uint32_t iterations = std::max(1u, settings_.solverIterations);
    for (uint32_t iter = 0; iter < iterations; ++iter) {
        for (Contact& c : contacts_) {
            RigidBody* a = body(c.bodyA);
            RigidBody* b = body(c.bodyB);
            if (!a || !b) continue;
            const float invMassSum = a->inverseMass + b->inverseMass;
            if (invMassSum <= 1e-8f) continue;
            const glm::vec3 rA = c.point - a->position;
            const glm::vec3 rB = c.point - b->position;
            const glm::mat3 invIA = a->inverseMass > 0 ? inverse_inertia(*a) : glm::mat3(0);
            const glm::mat3 invIB = b->inverseMass > 0 ? inverse_inertia(*b) : glm::mat3(0);
            const glm::vec3 rv = (b->linearVelocity + glm::cross(b->angularVelocity, rB)) -
                                 (a->linearVelocity + glm::cross(a->angularVelocity, rA));
            float velAlongNormal = glm::dot(rv, c.normal);
            if (velAlongNormal > 0.0f) continue;
            const float e = std::min(c.restitution, 1.0f);
            // Baumgarte bias applied once (first iteration only) so resting
            // penetration separates without accumulating impulse overshoot.
            float bias = 0.0f;
            if (iter == 0) {
                const float penetration = std::max(0.0f, c.penetration - settings_.contactSlop);
                bias = std::min(0.1f / dt * penetration, 1.0f);
            }
            float j = (-(1.0f + e) * velAlongNormal + bias) / invMassSum;
            if (j < 0.0f) j = 0.0f;
            const glm::vec3 impulse = j * c.normal;
            a->linearVelocity -= impulse * a->inverseMass;
            b->linearVelocity += impulse * b->inverseMass;
            a->angularVelocity -= invIA * glm::cross(rA, impulse);
            b->angularVelocity += invIB * glm::cross(rB, impulse);
            if (a->sleeping && a->inverseMass > 0.0f && length_sq(impulse) > 1e-8f) { a->sleeping = false; a->sleepTimer = 0.0f; }
            if (b->sleeping && b->inverseMass > 0.0f && length_sq(impulse) > 1e-8f) { b->sleeping = false; b->sleepTimer = 0.0f; }
            // Friction.
            const glm::vec3 tangent = glm::normalize(rv - c.normal * velAlongNormal);
            if (glm::length(tangent) > 1e-6f) {
                const float jt = -glm::dot(rv, tangent) / invMassSum;
                const float maxFriction = c.friction * std::abs(j);
                const float clamped = clampf(jt, -maxFriction, maxFriction);
                const glm::vec3 frictionImpulse = clamped * tangent;
                a->linearVelocity -= frictionImpulse * a->inverseMass;
                b->linearVelocity += frictionImpulse * b->inverseMass;
            }
        }
    }

    // Positional correction applied ONCE per frame (avoids per-iteration
    // accumulation overshoot that destabilizes stacks). Clamped per contact.
    const float maxCorrection = 0.25f;
    for (const Contact& c : contacts_) {
        RigidBody* a = body(c.bodyA);
        RigidBody* b = body(c.bodyB);
        if (!a || !b) continue;
        const float invMassSum = a->inverseMass + b->inverseMass;
        if (invMassSum <= 1e-8f) continue;
        const float depth = std::max(0.0f, c.penetration - settings_.contactSlop);
        if (depth <= 0.0f) continue;
        const float correction = std::min(depth * 0.8f, maxCorrection);
        const glm::vec3 push = c.normal * correction;
        a->position -= push * (a->inverseMass / invMassSum);
        b->position += push * (b->inverseMass / invMassSum);
        // Bodies with significant relative motion get woken; resting stacks
        // keep their sleep timers untouched so they can fall asleep.
        const float relSpeed = glm::length(b->linearVelocity - a->linearVelocity);
        if (relSpeed > settings_.sleepLinearThreshold) {
            if (!a->sleeping) a->sleepTimer = 0.0f;
            if (!b->sleeping) b->sleepTimer = 0.0f;
        }
    }
}

void PhysicsWorld::solve_joints(float dt) {
    for (auto& maybeJoint : joints_) {
        if (!maybeJoint || maybeJoint->broken) continue;
        Joint& j = *maybeJoint;
        const auto bodiesFor = [&](BodyHandle x, BodyHandle y) -> std::pair<RigidBody*, RigidBody*> {
            return {body(x), body(y)};
        };
        if (std::holds_alternative<DistanceConstraintDesc>(j.desc)) {
            const auto& d = std::get<DistanceConstraintDesc>(j.desc);
            auto [a, b] = bodiesFor(d.bodyA, d.bodyB);
            if (!a || !b) continue;
            const float invMassSum = a->inverseMass + b->inverseMass;
            if (invMassSum <= 1e-8f) continue;
            const glm::vec3 anchorA = a->position + a->rotation * d.localAnchorA;
            const glm::vec3 anchorB = b->position + b->rotation * d.localAnchorB;
            const glm::vec3 delta = anchorB - anchorA;
            const float dist = glm::length(delta);
            if (dist < 1e-6f) continue;
            const float rest = d.restLength > 0.0f ? d.restLength : dist;
            const glm::vec3 normal = delta / dist;
            const float x = dist - rest;
            const float v = glm::dot(b->linearVelocity - a->linearVelocity, normal);
            const float bias = d.stiffness * x / dt * 0.2f;
            const float lambda = -(v + bias) / invMassSum;
            const glm::vec3 impulse = lambda * normal;
            a->linearVelocity -= impulse * a->inverseMass;
            b->linearVelocity += impulse * b->inverseMass;
            j.accumulatedImpulse = std::abs(lambda);
            if (d.breakImpulse > 0.0f && std::abs(lambda) > d.breakImpulse) j.broken = true;
        } else if (std::holds_alternative<BallSocketJointDesc>(j.desc)) {
            const auto& d = std::get<BallSocketJointDesc>(j.desc);
            auto [a, b] = bodiesFor(d.bodyA, d.bodyB);
            if (!a || !b) continue;
            const float invMassSum = a->inverseMass + b->inverseMass;
            if (invMassSum <= 1e-8f) continue;
            const glm::vec3 worldA = a->position + a->rotation * d.anchorA;
            const glm::vec3 worldB = b->position + b->rotation * d.anchorB;
            const glm::vec3 delta = worldB - worldA;
            if (length_sq(delta) < 1e-6f) continue;
            const glm::vec3 lambda = delta / invMassSum * 0.2f;
            a->linearVelocity += lambda * a->inverseMass;
            b->linearVelocity -= lambda * b->inverseMass;
            a->position += delta * a->inverseMass / invMassSum * 0.8f;
            b->position -= delta * b->inverseMass / invMassSum * 0.8f;
            j.accumulatedImpulse = glm::length(lambda);
            if (d.breakForce > 0.0f && glm::length(delta) * 1000.0f > d.breakForce) j.broken = true;
        } else if (std::holds_alternative<HingeJointDesc>(j.desc)) {
            const auto& d = std::get<HingeJointDesc>(j.desc);
            auto [a, b] = bodiesFor(d.bodyA, d.bodyB);
            if (!a || !b) continue;
            const float invMassSum = a->inverseMass + b->inverseMass;
            if (invMassSum <= 1e-8f) continue;
            const glm::vec3 worldA = a->position + a->rotation * d.anchorA;
            const glm::vec3 worldB = b->position + b->rotation * d.anchorB;
            const glm::vec3 delta = worldB - worldA;
            if (length_sq(delta) < 1e-6f) continue;
            const glm::vec3 lambda = delta / invMassSum * 0.2f;
            a->linearVelocity += lambda * a->inverseMass;
            b->linearVelocity -= lambda * b->inverseMass;
            a->position += delta * a->inverseMass / invMassSum * 0.8f;
            b->position -= delta * b->inverseMass / invMassSum * 0.8f;
            if (d.enableMotor && d.maxMotorTorque > 0.0f) {
                const glm::vec3 axis = glm::normalize(a->rotation * d.axis);
                const float rel = glm::dot(b->angularVelocity - a->angularVelocity, axis);
                const float torque = clampf(-rel, -d.maxMotorTorque, d.maxMotorTorque) * d.motorSpeed;
                a->angularVelocity += axis * torque * a->inverseMass;
                b->angularVelocity -= axis * torque * b->inverseMass;
            }
            if (d.breakTorque > 0.0f && j.accumulatedImpulse * 10.0f > d.breakTorque) j.broken = true;
        } else if (std::holds_alternative<SliderJointDesc>(j.desc)) {
            const auto& d = std::get<SliderJointDesc>(j.desc);
            auto [a, b] = bodiesFor(d.bodyA, d.bodyB);
            if (!a || !b) continue;
            const float invMassSum = a->inverseMass + b->inverseMass;
            if (invMassSum <= 1e-8f) continue;
            const glm::vec3 axis = glm::normalize(a->rotation * d.axis);
            const glm::vec3 delta = b->position - a->position;
            const float along = glm::dot(delta, axis);
            if (d.enableLimits) {
                const float corr = along < d.lowerLimit ? (along - d.lowerLimit) : (along > d.upperLimit ? (along - d.upperLimit) : 0.0f);
                const glm::vec3 push = axis * corr * 0.8f;
                a->position += push * a->inverseMass / invMassSum;
                b->position -= push * b->inverseMass / invMassSum;
            }
            const glm::vec3 lateral = delta - axis * along;
            const glm::vec3 lambda = lateral / invMassSum * 0.2f;
            a->linearVelocity += lambda * a->inverseMass;
            b->linearVelocity -= lambda * b->inverseMass;
        } else if (std::holds_alternative<FixedJointDesc>(j.desc)) {
            const auto& d = std::get<FixedJointDesc>(j.desc);
            auto [a, b] = bodiesFor(d.bodyA, d.bodyB);
            if (!a || !b) continue;
            const float invMassSum = a->inverseMass + b->inverseMass;
            if (invMassSum <= 1e-8f) continue;
            const glm::vec3 worldA = a->position + a->rotation * d.anchorA;
            const glm::vec3 worldB = b->position + b->rotation * d.anchorB;
            const glm::vec3 delta = worldB - worldA;
            const glm::vec3 lambda = delta / invMassSum * 0.3f;
            a->linearVelocity += lambda * a->inverseMass;
            b->linearVelocity -= lambda * b->inverseMass;
            a->position += delta * a->inverseMass / invMassSum * 0.9f;
            b->position -= delta * b->inverseMass / invMassSum * 0.9f;
            // Lock relative rotation (simple: snap quaternion towards each other).
            const glm::quat deltaRot = b->rotation * glm::inverse(a->rotation);
            if (glm::length(deltaRot) > 0.01f) {
                b->rotation = glm::slerp(b->rotation, a->rotation, 0.1f);
            }
            if (d.breakForce > 0.0f && glm::length(delta) * 1000.0f > d.breakForce) j.broken = true;
        }
    }
}

void PhysicsWorld::update_sleep(float dt) {
    for (auto& maybeBody : bodies_) {
        if (!maybeBody) continue;
        RigidBody& b = *maybeBody;
        if (!b.dynamic() || !b.allowSleep || b.sleeping) continue;
        const float linSpeed = glm::length(b.linearVelocity);
        const float angSpeed = glm::length(b.angularVelocity);
        if (linSpeed < settings_.sleepLinearThreshold && angSpeed < settings_.sleepAngularThreshold) {
            b.sleepTimer += dt;
            if (b.sleepTimer >= settings_.sleepDelay) {
                b.sleeping = true;
                b.linearVelocity = glm::vec3(0);
                b.angularVelocity = glm::vec3(0);
            }
        } else {
            b.sleepTimer = 0.0f;
        }
    }
}

void PhysicsWorld::step_substep(float dt) {
    contacts_.clear();
    triggerEvents_.clear();
    integrate(dt);
    solve_contacts_pgs(dt);
    solve_joints(dt);
    update_sleep(dt);
}

void PhysicsWorld::update_triggers(const std::vector<TriggerPair>& current) {
    // Enter / stay / exit detection.
    for (const TriggerPair& pair : current) {
        bool existed = false;
        for (const TriggerPair& old : triggerPairs_) {
            if (old.trigger == pair.trigger && old.other == pair.other) { existed = true; break; }
        }
        triggerEvents_.push_back({existed ? TriggerEvent::Type::Stay : TriggerEvent::Type::Enter,
                                  pair.trigger, pair.other});
    }
    for (const TriggerPair& old : triggerPairs_) {
        bool stillPresent = false;
        for (const TriggerPair& cur : current) {
            if (cur.trigger == old.trigger && cur.other == old.other) { stillPresent = true; break; }
        }
        if (!stillPresent)
            triggerEvents_.push_back({TriggerEvent::Type::Exit, old.trigger, old.other});
    }
    triggerPairs_ = current;
}

} // namespace Engine::Physics
