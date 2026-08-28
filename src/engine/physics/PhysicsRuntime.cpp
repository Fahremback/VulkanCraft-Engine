#include "PhysicsRuntime.hpp"

#include "PhysicsBackend.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace Engine::Physics {
namespace {
constexpr float Epsilon = 1.0e-6f;

float safe_length(const glm::vec3& value) { return std::sqrt(std::max(0.0f, glm::dot(value, value))); }

glm::vec3 safe_normalize(const glm::vec3& value, const glm::vec3& fallback = {0.0f, 1.0f, 0.0f}) {
    const float length = safe_length(value);
    return length > Epsilon ? value / length : fallback;
}

float shape_min_extent(const ColliderShape& shape) {
    return std::visit([](const auto& concrete) {
        using T = std::decay_t<decltype(concrete)>;
        if constexpr (std::is_same_v<T, SphereShape>) {
            return std::max(0.01f, concrete.radius);
        } else if constexpr (std::is_same_v<T, CapsuleShape>) {
            return std::max(0.01f, concrete.radius);
        } else {
            return std::max(0.01f, std::min({concrete.halfExtents.x, concrete.halfExtents.y, concrete.halfExtents.z}));
        }
    }, shape);
}

glm::vec3 shape_half_extents(const ColliderShape& shape, const glm::quat& rotation) {
    return std::visit([&](const auto& concrete) -> glm::vec3 {
        using T = std::decay_t<decltype(concrete)>;
        if constexpr (std::is_same_v<T, SphereShape>) {
            return glm::vec3(std::max(0.0f, concrete.radius));
        } else if constexpr (std::is_same_v<T, CapsuleShape>) {
            const glm::vec3 axis = glm::abs(rotation * glm::vec3(0.0f, 1.0f, 0.0f));
            return glm::vec3(std::max(0.0f, concrete.radius)) + axis * std::max(0.0f, concrete.halfHeight);
        } else {
            const glm::mat3 basis = glm::mat3_cast(rotation);
            const glm::vec3 h = glm::max(concrete.halfExtents, glm::vec3(0.0f));
            return glm::abs(basis[0]) * h.x + glm::abs(basis[1]) * h.y + glm::abs(basis[2]) * h.z;
        }
    }, shape);
}

void append_box(std::vector<DebugLine>& lines, const Aabb& box, const glm::vec4& color) {
    const glm::vec3 mn = box.minimum;
    const glm::vec3 mx = box.maximum;
    const glm::vec3 p[8] = {
        {mn.x,mn.y,mn.z}, {mx.x,mn.y,mn.z}, {mx.x,mx.y,mn.z}, {mn.x,mx.y,mn.z},
        {mn.x,mn.y,mx.z}, {mx.x,mn.y,mx.z}, {mx.x,mx.y,mx.z}, {mn.x,mx.y,mx.z}
    };
    constexpr int edges[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
    for (const auto& edge : edges) lines.push_back({p[edge[0]], p[edge[1]], color});
}
} // namespace

bool Aabb::overlaps(const Aabb& other) const noexcept {
    return minimum.x <= other.maximum.x && maximum.x >= other.minimum.x &&
           minimum.y <= other.maximum.y && maximum.y >= other.minimum.y &&
           minimum.z <= other.maximum.z && maximum.z >= other.minimum.z;
}

bool Aabb::contains(const glm::vec3& point) const noexcept {
    return point.x >= minimum.x && point.x <= maximum.x && point.y >= minimum.y &&
           point.y <= maximum.y && point.z >= minimum.z && point.z <= maximum.z;
}

PhysicsRuntime::PhysicsRuntime(WorldSettings settings, PhysicsBackendKind backend)
    : settings_(settings), backendKind_(backend) {
    external_ = create_backend(backend, settings);
}

PhysicsRuntime::~PhysicsRuntime() = default;

const char* PhysicsRuntime::backend_name() const noexcept {
    return external_ ? external_->name() : "builtin";
}

void PhysicsRuntime::set_settings(const WorldSettings& settings) {
    settings_ = settings;
    if (external_) external_->set_gravity(settings.gravity);
}

BodyHandle PhysicsRuntime::to_engine_handle(BodyHandle backendHandle) const {
    const auto it = backendToEngine_.find(backendHandle);
    return it != backendToEngine_.end() ? it->second : InvalidBody;
}

BodyHandle PhysicsRuntime::to_backend_handle(BodyHandle engineHandle) const {
    return engineHandle < backendBodies_.size() ? backendBodies_[engineHandle] : InvalidBody;
}

BodyHandle PhysicsRuntime::create_body(const BodyDesc& description) {
    BodyHandle handle;
    if (!freeBodies_.empty()) {
        handle = freeBodies_.back();
        freeBodies_.pop_back();
    } else {
        handle = static_cast<BodyHandle>(bodies_.size());
        bodies_.emplace_back();
    }
    RigidBody value;
    value.handle = handle;
    value.motion = description.motion;
    value.position = value.previousPosition = description.position;
    value.rotation = glm::normalize(description.rotation);
    value.linearVelocity = description.linearVelocity;
    value.angularVelocity = description.angularVelocity;
    value.inverseMass = description.motion == MotionType::Dynamic && description.mass > Epsilon ? 1.0f / description.mass : 0.0f;
    value.linearDamping = std::max(0.0f, description.linearDamping);
    value.angularDamping = std::max(0.0f, description.angularDamping);
    value.gravityScale = description.gravityScale;
    value.continuous = description.continuous;
    value.allowSleep = description.allowSleep;
    value.collider = description.collider;
    value.userData = description.userData;
    if (external_) {
        backendBodies_.resize(bodies_.size(), InvalidBody);
        const BodyHandle backendHandle = external_->create_body(description);
        backendBodies_[handle] = backendHandle;
        if (backendHandle != InvalidBody) backendToEngine_[backendHandle] = handle;
    }
    bodies_[handle] = value;
    return handle;
}

bool PhysicsRuntime::destroy_body(BodyHandle handle) {
    if (handle == InvalidBody || handle >= bodies_.size() || !bodies_[handle]) return false;
    if (external_) {
        const BodyHandle backendHandle = to_backend_handle(handle);
        if (backendHandle != InvalidBody) {
            external_->destroy_body(backendHandle);
            backendToEngine_.erase(backendHandle);
            backendBodies_[handle] = InvalidBody;
        }
    }
    bodies_[handle].reset();
    freeBodies_.push_back(handle);
    for (auto& constraint : constraints_) {
        if (constraint && (constraint->bodyA == handle || constraint->bodyB == handle)) constraint->broken = true;
    }
    for (auto& constraint : swingTwistConstraints_) {
        if (constraint && (constraint->bodyA == handle || constraint->bodyB == handle)) constraint->broken = true;
    }
    triggerPairs_.erase(std::remove_if(triggerPairs_.begin(), triggerPairs_.end(), [handle](const auto& p) {
        return p.trigger == handle || p.other == handle;
    }), triggerPairs_.end());
    return true;
}

RigidBody* PhysicsRuntime::body(BodyHandle handle) {
    return handle < bodies_.size() && bodies_[handle] ? &*bodies_[handle] : nullptr;
}

const RigidBody* PhysicsRuntime::body(BodyHandle handle) const {
    return handle < bodies_.size() && bodies_[handle] ? &*bodies_[handle] : nullptr;
}

void PhysicsRuntime::set_transform(BodyHandle handle, const glm::vec3& position, const glm::quat& rotation) {
    RigidBody* value = body(handle);
    if (!value) return;
    value->position = position;
    value->rotation = glm::normalize(rotation);
    value->sleeping = false;
    value->sleepTimer = 0.0f;
    value->previousPosition = position;
    if (external_) {
        const BodyHandle backend = to_backend_handle(handle);
        if (backend != InvalidBody) external_->set_transform(backend, position, value->rotation);
    }
}

void PhysicsRuntime::set_velocity(BodyHandle handle, const glm::vec3& linearVelocity,
                                  const glm::vec3& angularVelocity) {
    RigidBody* value = body(handle);
    if (!value) return;
    value->linearVelocity = linearVelocity;
    value->angularVelocity = angularVelocity;
    value->sleeping = false;
    value->sleepTimer = 0.0f;
    if (external_) {
        const BodyHandle backend = to_backend_handle(handle);
        if (backend != InvalidBody) external_->set_velocity(backend, linearVelocity, angularVelocity);
    }
}

ConstraintHandle PhysicsRuntime::create_distance_constraint(const DistanceConstraintDesc& description) {
    if (!body(description.bodyA) || !body(description.bodyB) || description.bodyA == description.bodyB) return InvalidConstraint;
    ConstraintHandle handle;
    if (!freeConstraints_.empty()) {
        handle = freeConstraints_.back(); freeConstraints_.pop_back();
    } else {
        handle = static_cast<ConstraintHandle>(constraints_.size()); constraints_.emplace_back();
    }
    DistanceConstraint value;
    static_cast<DistanceConstraintDesc&>(value) = description;
    value.handle = handle;
    value.restLength = std::max(0.0f, value.restLength);
    value.stiffness = glm::clamp(value.stiffness, 0.0f, 1.0f);
    value.damping = std::max(0.0f, value.damping);
    if (external_) {
        backendConstraints_.resize(constraints_.size(), InvalidConstraint);
        backendConstraints_[handle] = external_->create_distance_constraint(description);
    }
    constraints_[handle] = value;
    return handle;
}

ConstraintHandle PhysicsRuntime::create_swing_twist_constraint(const SwingTwistConstraintDesc& description) {
    if (!external_ || !external_->supports_swing_twist()) return InvalidConstraint;
    if (!body(description.bodyA) || !body(description.bodyB) || description.bodyA == description.bodyB) return InvalidConstraint;
    // Separate handle space (high offset) so swing-twist handles never collide
    // with distance-constraint handles (which are dense indices).
    ConstraintHandle index;
    if (!freeSwingTwist_.empty()) {
        index = freeSwingTwist_.back(); freeSwingTwist_.pop_back();
    } else {
        index = static_cast<ConstraintHandle>(swingTwistConstraints_.size());
        swingTwistConstraints_.emplace_back();
        backendSwingTwist_.push_back(InvalidConstraint);
    }
    SwingTwistConstraint value;
    static_cast<SwingTwistConstraintDesc&>(value) = description;
    value.handle = kSwingTwistHandleOffset + index;
    value.normalHalfConeAngle = glm::clamp(value.normalHalfConeAngle, 0.0f, glm::pi<float>());
    value.planeHalfConeAngle = glm::clamp(value.planeHalfConeAngle, 0.0f, glm::pi<float>());
    value.twistMinAngle = glm::clamp(value.twistMinAngle, -glm::pi<float>(), 0.0f);
    value.twistMaxAngle = glm::clamp(value.twistMaxAngle, 0.0f, glm::pi<float>());
    backendSwingTwist_[index] = external_->create_swing_twist_constraint(description);
    if (backendSwingTwist_[index] == InvalidConstraint) {
        swingTwistConstraints_[index].reset();
        freeSwingTwist_.push_back(index);
        return InvalidConstraint;
    }
    swingTwistConstraints_[index] = value;
    return value.handle;
}

bool PhysicsRuntime::set_swing_twist_motor(ConstraintHandle constraint,
                                           bool motorOn, float frequency,
                                           float damping,
                                           const glm::quat& target) {
    if (!external_ || !external_->supports_swing_twist()) return false;
    if (constraint < kSwingTwistHandleOffset) return false;  // distance space
    const std::size_t index =
        static_cast<std::size_t>(constraint - kSwingTwistHandleOffset);
    if (index >= swingTwistConstraints_.size() ||
        !swingTwistConstraints_[index]) {
        return false;
    }
    if (index >= backendSwingTwist_.size() ||
        backendSwingTwist_[index] == InvalidConstraint) {
        return false;
    }
    SwingTwistConstraint& value = *swingTwistConstraints_[index];
    value.motorOn = motorOn;
    value.motorFrequency = frequency;
    value.motorDamping = damping;
    value.motorTarget = target;
    return external_->set_swing_twist_motor(backendSwingTwist_[index], motorOn,
                                            frequency, damping, target);
}

bool PhysicsRuntime::supports_swing_twist() const noexcept {
    return external_ != nullptr && external_->supports_swing_twist();
}

bool PhysicsRuntime::supports_vehicles() const noexcept {
    return external_ != nullptr && external_->supports_vehicles();
}

VehicleHandle PhysicsRuntime::create_vehicle(const VehicleDesc& description) {
    if (!external_ || !external_->supports_vehicles()) return InvalidVehicle;
    if (description.chassis == InvalidBody || !body(description.chassis)) return InvalidVehicle;
    if (description.wheels.empty()) return InvalidVehicle;
    VehicleHandle handle;
    if (!freeVehicles_.empty()) {
        handle = freeVehicles_.back();
        freeVehicles_.pop_back();
    } else {
        handle = static_cast<VehicleHandle>(backendVehicles_.size() + 1);
        backendVehicles_.push_back(InvalidVehicle);
    }
    const VehicleHandle backendHandle = external_->create_vehicle(description);
    if (backendHandle == InvalidVehicle) {
        backendVehicles_[handle - 1] = InvalidVehicle;
        freeVehicles_.push_back(handle);
        return InvalidVehicle;
    }
    backendVehicles_[handle - 1] = backendHandle;
    return handle;
}

bool PhysicsRuntime::destroy_vehicle(VehicleHandle handle) {
    if (handle == InvalidVehicle || handle > backendVehicles_.size()) return false;
    const VehicleHandle backendHandle = backendVehicles_[handle - 1];
    if (backendHandle == InvalidVehicle) return false;
    if (!external_ || !external_->destroy_vehicle(backendHandle)) return false;
    backendVehicles_[handle - 1] = InvalidVehicle;
    freeVehicles_.push_back(handle);
    return true;
}

bool PhysicsRuntime::set_vehicle_input(VehicleHandle handle, const VehicleInput& input) {
    if (handle == InvalidVehicle || handle > backendVehicles_.size()) return false;
    const VehicleHandle backendHandle = backendVehicles_[handle - 1];
    if (backendHandle == InvalidVehicle || !external_) return false;
    return external_->set_vehicle_input(backendHandle, input);
}

bool PhysicsRuntime::vehicle_wheel_state(VehicleHandle handle, std::size_t index, WheelState& out) const {
    if (handle == InvalidVehicle || handle > backendVehicles_.size()) return false;
    const VehicleHandle backendHandle = backendVehicles_[handle - 1];
    if (backendHandle == InvalidVehicle || !external_) return false;
    return external_->vehicle_wheel_state(backendHandle, index, out);
}

bool PhysicsRuntime::destroy_constraint(ConstraintHandle handle) {
    if (handle == InvalidConstraint) return false;
    if (handle >= kSwingTwistHandleOffset) {
        const ConstraintHandle index = handle - kSwingTwistHandleOffset;
        if (index >= swingTwistConstraints_.size() || !swingTwistConstraints_[index]) return false;
        if (external_ && index < backendSwingTwist_.size() && backendSwingTwist_[index] != InvalidConstraint) {
            external_->destroy_constraint(backendSwingTwist_[index]);
            backendSwingTwist_[index] = InvalidConstraint;
        }
        swingTwistConstraints_[index].reset();
        freeSwingTwist_.push_back(index);
        return true;
    }
    if (handle >= constraints_.size() || !constraints_[handle]) return false;
    if (external_ && handle < backendConstraints_.size() && backendConstraints_[handle] != InvalidConstraint) {
        external_->destroy_constraint(backendConstraints_[handle]);
        backendConstraints_[handle] = InvalidConstraint;
    }
    constraints_[handle].reset(); freeConstraints_.push_back(handle); return true;
}

void PhysicsRuntime::wake(BodyHandle handle) {
    if (RigidBody* value = body(handle); value && value->dynamic()) {
        if (external_) external_->wake(to_backend_handle(handle));
        value->sleeping = false; value->sleepTimer = 0.0f;
    }
}

void PhysicsRuntime::set_motion(BodyHandle handle, MotionType motion, float mass) {
    RigidBody* value = body(handle);
    if (!value) return;
    if (external_) external_->set_motion_type(to_backend_handle(handle), motion, mass);
    value->motion = motion;
    value->inverseMass = (motion == MotionType::Dynamic && mass > 0.0f)
                             ? 1.0f / mass
                             : 0.0f;
    value->sleeping = false;
    value->sleepTimer = 0.0f;
}

void PhysicsRuntime::add_force(BodyHandle handle, const glm::vec3& force) {
    if (RigidBody* value = body(handle); value && value->dynamic()) {
        if (external_) external_->add_force(to_backend_handle(handle), force);
        value->accumulatedForce += force; wake(handle);
    }
}

void PhysicsRuntime::add_torque(BodyHandle handle, const glm::vec3& torque) {
    if (RigidBody* value = body(handle); value && value->dynamic()) {
        if (external_) external_->add_torque(to_backend_handle(handle), torque);
        value->accumulatedTorque += torque; wake(handle);
    }
}

void PhysicsRuntime::apply_impulse(BodyHandle handle, const glm::vec3& impulse) {
    if (RigidBody* value = body(handle); value && value->dynamic()) {
        if (external_) external_->apply_impulse(to_backend_handle(handle), impulse);
        value->linearVelocity += impulse * value->inverseMass; wake(handle);
    }
}

void PhysicsRuntime::apply_impulse_at_point(BodyHandle handle, const glm::vec3& impulse, const glm::vec3& worldPoint) {
    if (RigidBody* value = body(handle); value && value->dynamic()) {
        if (external_) external_->apply_impulse_at_point(to_backend_handle(handle), impulse, worldPoint);
        value->linearVelocity += impulse * value->inverseMass;
        // NOTE: Angular impulse uses inverseMass as a simplification. For proper
        // non-uniform inertia, the backend (Jolt/Bullet) handles the real tensor.
        // The builtin solver approximates with scalar inverseMass.
        value->angularVelocity += glm::cross(worldPoint - value->position, impulse) * value->inverseMass;
        wake(handle);
    }
}

Aabb PhysicsRuntime::compute_aabb(const RigidBody& value, bool swept) const {
    const glm::quat colliderRotation = value.rotation * value.collider.localRotation;
    const glm::vec3 center = value.position + value.rotation * value.collider.localPosition;
    const glm::vec3 extents = shape_half_extents(value.collider.shape, colliderRotation);
    Aabb result{center - extents, center + extents};
    if (swept && value.continuous) {
        const glm::vec3 oldCenter = value.previousPosition + value.rotation * value.collider.localPosition;
        result.minimum = glm::min(result.minimum, oldCenter - extents);
        result.maximum = glm::max(result.maximum, oldCenter + extents);
    }
    return result;
}

std::vector<PhysicsRuntime::Pair> PhysicsRuntime::broadphase() const {
    struct Proxy { BodyHandle body; Aabb bounds; };
    std::vector<Proxy> proxies;
    proxies.reserve(bodies_.size());
    for (BodyHandle i = 1; i < bodies_.size(); ++i) if (bodies_[i]) proxies.push_back({i, compute_aabb(*bodies_[i], true)});
    std::sort(proxies.begin(), proxies.end(), [](const Proxy& a, const Proxy& b) { return a.bounds.minimum.x < b.bounds.minimum.x; });
    std::vector<Pair> pairs;
    for (std::size_t i = 0; i < proxies.size(); ++i) {
        const RigidBody& a = *bodies_[proxies[i].body];
        for (std::size_t j = i + 1; j < proxies.size() && proxies[j].bounds.minimum.x <= proxies[i].bounds.maximum.x; ++j) {
            const RigidBody& b = *bodies_[proxies[j].body];
            if ((a.motion == MotionType::Static && b.motion == MotionType::Static) || !a.collider.filter.allows(b.collider.filter)) continue;
            if (proxies[i].bounds.overlaps(proxies[j].bounds)) pairs.push_back({a.handle, b.handle});
        }
    }
    return pairs;
}

std::optional<Contact> PhysicsRuntime::generate_contact(const RigidBody& a, const RigidBody& b) const {
    const glm::vec3 centerA = a.position + a.rotation * a.collider.localPosition;
    const glm::vec3 centerB = b.position + b.rotation * b.collider.localPosition;
    Contact contact;
    contact.bodyA = a.handle; contact.bodyB = b.handle;
    contact.restitution = std::max(a.collider.restitution, b.collider.restitution);
    contact.friction = std::sqrt(std::max(0.0f, a.collider.friction * b.collider.friction));

    const auto* sphereA = std::get_if<SphereShape>(&a.collider.shape);
    const auto* sphereB = std::get_if<SphereShape>(&b.collider.shape);
    if (sphereA && sphereB) {
        const glm::vec3 delta = centerB - centerA;
        const float distance = safe_length(delta);
        const float radii = sphereA->radius + sphereB->radius;
        if (distance >= radii) return std::nullopt;
        contact.normal = safe_normalize(delta);
        contact.penetration = radii - distance;
        contact.point = centerA + contact.normal * (sphereA->radius - contact.penetration * 0.5f);
        return contact;
    }

    auto sphere_box = [&](const RigidBody& sphereBody, const SphereShape& sphere, const RigidBody& boxBody,
                          bool sphereIsA) -> std::optional<Contact> {
        const glm::vec3 sphereCenter = sphereBody.position + sphereBody.rotation * sphereBody.collider.localPosition;
        const Aabb box = compute_aabb(boxBody);
        const glm::vec3 closest = glm::clamp(sphereCenter, box.minimum, box.maximum);
        const glm::vec3 boxToSphere = sphereCenter - closest;
        const float distance = safe_length(boxToSphere);
        if (distance >= sphere.radius) return std::nullopt;
        glm::vec3 boxOut = safe_normalize(boxToSphere, safe_normalize(sphereCenter - (box.minimum + box.maximum) * 0.5f));
        Contact result = contact;
        result.normal = sphereIsA ? -boxOut : boxOut;
        result.penetration = sphere.radius - distance;
        result.point = closest;
        return result;
    };
    if (sphereA) return sphere_box(a, *sphereA, b, true);
    if (sphereB) return sphere_box(b, *sphereB, a, false);

    const Aabb boundsA = compute_aabb(a);
    const Aabb boundsB = compute_aabb(b);
    if (!boundsA.overlaps(boundsB)) return std::nullopt;
    const glm::vec3 overlaps = glm::min(boundsA.maximum, boundsB.maximum) - glm::max(boundsA.minimum, boundsB.minimum);
    int axis = 0;
    if (overlaps.y < overlaps.x) axis = 1;
    if (overlaps.z < overlaps[axis]) axis = 2;
    contact.normal = glm::vec3(0.0f);
    contact.normal[axis] = centerB[axis] >= centerA[axis] ? 1.0f : -1.0f;
    contact.penetration = overlaps[axis];
    contact.point = (glm::max(boundsA.minimum, boundsB.minimum) + glm::min(boundsA.maximum, boundsB.maximum)) * 0.5f;
    return contact;
}

void PhysicsRuntime::integrate(float dt) {
    for (auto& slot : bodies_) if (slot) {
        RigidBody& value = *slot;
        value.previousPosition = value.position;
        if (!value.dynamic() || value.sleeping) { value.accumulatedForce = {}; value.accumulatedTorque = {}; continue; }
        value.linearVelocity += (settings_.gravity * value.gravityScale + value.accumulatedForce * value.inverseMass) * dt;
        value.angularVelocity += value.accumulatedTorque * value.inverseMass * dt;
        value.linearVelocity *= 1.0f / (1.0f + value.linearDamping * dt);
        value.angularVelocity *= 1.0f / (1.0f + value.angularDamping * dt);
        value.position += value.linearVelocity * dt;
        const glm::vec3 spin = value.angularVelocity * dt;
        const float angle = safe_length(spin);
        if (angle > Epsilon) value.rotation = glm::normalize(glm::angleAxis(angle, spin / angle) * value.rotation);
        value.accumulatedForce = {}; value.accumulatedTorque = {};
    }
}

void PhysicsRuntime::solve_constraints(float dt) {
    if (dt <= Epsilon) return;
    for (auto& slot : constraints_) if (slot && !slot->broken) {
        DistanceConstraint& c = *slot;
        RigidBody* a = body(c.bodyA); RigidBody* b = body(c.bodyB);
        if (!a || !b) { c.broken = true; continue; }
        const glm::vec3 anchorA = a->position + a->rotation * c.localAnchorA;
        const glm::vec3 anchorB = b->position + b->rotation * c.localAnchorB;
        const glm::vec3 delta = anchorB - anchorA;
        const float distance = safe_length(delta);
        const glm::vec3 normal = safe_normalize(delta);
        const float invMass = a->inverseMass + b->inverseMass;
        if (invMass <= Epsilon) continue;
        const float relativeSpeed = glm::dot(b->linearVelocity - a->linearVelocity, normal);
        const float positionalSpeed = (distance - c.restLength) * c.stiffness / dt;
        const float impulseMagnitude = (positionalSpeed + relativeSpeed * c.damping) / invMass;
        c.lastImpulse = std::abs(impulseMagnitude);
        if (c.breakImpulse > 0.0f && c.lastImpulse > c.breakImpulse) { c.broken = true; continue; }
        const glm::vec3 impulse = normal * impulseMagnitude;
        if (a->dynamic()) a->linearVelocity += impulse * a->inverseMass;
        if (b->dynamic()) b->linearVelocity -= impulse * b->inverseMass;
    }
}

void PhysicsRuntime::solve_contacts(float dt) {
    (void)dt;
    for (std::uint32_t iteration = 0; iteration < settings_.solverIterations; ++iteration) {
        for (const Contact& contact : contacts_) {
            RigidBody* a = body(contact.bodyA); RigidBody* b = body(contact.bodyB);
            if (!a || !b) continue;
            const float invMass = a->inverseMass + b->inverseMass;
            if (invMass <= Epsilon) continue;
            const glm::vec3 relativeVelocity = b->linearVelocity - a->linearVelocity;
            const float velocityAlongNormal = glm::dot(relativeVelocity, contact.normal);
            if (velocityAlongNormal < 0.0f) {
                const float magnitude = -(1.0f + contact.restitution) * velocityAlongNormal / invMass;
                const glm::vec3 impulse = contact.normal * magnitude;
                if (a->dynamic()) a->linearVelocity -= impulse * a->inverseMass;
                if (b->dynamic()) b->linearVelocity += impulse * b->inverseMass;
                glm::vec3 tangent = relativeVelocity - contact.normal * velocityAlongNormal;
                const float tangentLength = safe_length(tangent);
                if (tangentLength > Epsilon) {
                    tangent /= tangentLength;
                    float tangentImpulse = -glm::dot(relativeVelocity, tangent) / invMass;
                    tangentImpulse = glm::clamp(tangentImpulse, -magnitude * contact.friction, magnitude * contact.friction);
                    const glm::vec3 frictionImpulse = tangent * tangentImpulse;
                    if (a->dynamic()) a->linearVelocity -= frictionImpulse * a->inverseMass;
                    if (b->dynamic()) b->linearVelocity += frictionImpulse * b->inverseMass;
                }
            }
            if (iteration == 0) {
                const float correctionDepth = std::max(0.0f, contact.penetration - settings_.contactSlop);
                const glm::vec3 correction = contact.normal * (correctionDepth * 0.65f / invMass);
                if (a->dynamic()) a->position -= correction * a->inverseMass;
                if (b->dynamic()) b->position += correction * b->inverseMass;
            }
            wake(a->handle); wake(b->handle);
        }
    }
}

void PhysicsRuntime::update_sleep(float dt) {
    for (auto& slot : bodies_) if (slot && slot->dynamic()) {
        RigidBody& value = *slot;
        if (!value.allowSleep) { value.sleeping = false; value.sleepTimer = 0.0f; continue; }
        if (safe_length(value.linearVelocity) < settings_.sleepLinearThreshold && safe_length(value.angularVelocity) < settings_.sleepAngularThreshold) {
            value.sleepTimer += dt;
            if (value.sleepTimer >= settings_.sleepDelay) { value.sleeping = true; value.linearVelocity = {}; value.angularVelocity = {}; }
        } else { value.sleepTimer = 0.0f; value.sleeping = false; }
    }
}

void PhysicsRuntime::update_triggers(const std::vector<TriggerPair>& currentInput) {
    std::vector<TriggerPair> current = currentInput;
    auto order = [](const TriggerPair& a, const TriggerPair& b) { return a.trigger == b.trigger ? a.other < b.other : a.trigger < b.trigger; };
    std::sort(current.begin(), current.end(), order);
    current.erase(std::unique(current.begin(), current.end(), [](const TriggerPair& a, const TriggerPair& b) {
        return a.trigger == b.trigger && a.other == b.other;
    }), current.end());
    triggerEvents_.clear();
    for (const auto& pair : current) {
        const bool existed = std::binary_search(triggerPairs_.begin(), triggerPairs_.end(), pair, order);
        triggerEvents_.push_back({existed ? TriggerEvent::Type::Stay : TriggerEvent::Type::Enter, pair.trigger, pair.other});
    }
    for (const auto& pair : triggerPairs_) if (!std::binary_search(current.begin(), current.end(), pair, order)) {
        triggerEvents_.push_back({TriggerEvent::Type::Exit, pair.trigger, pair.other});
    }
    triggerPairs_ = std::move(current);
}

void PhysicsRuntime::step_substep(float dt) {
    integrate(dt);
    contacts_.clear();
    std::vector<TriggerPair> triggers;
    for (const Pair pair : broadphase()) {
        RigidBody* a = body(pair.a); RigidBody* b = body(pair.b);
        if (!a || !b) continue;
        const auto contact = generate_contact(*a, *b);
        if (!contact) continue;
        if (a->collider.trigger || b->collider.trigger) {
            if (a->collider.trigger) triggers.push_back({a->handle, b->handle});
            if (b->collider.trigger) triggers.push_back({b->handle, a->handle});
        } else contacts_.push_back(*contact);
    }
    update_triggers(triggers);
    // The distance constraint solver applies a positional impulse proportional to
    // the current error; running it once per substep (not solverIterations times)
    // keeps it a stable damped correction. Iterating it without re-evaluating
    // positions multiplies the gain by solverIterations and blows up in free fall.
    solve_constraints(dt);
    solve_contacts(dt);
    update_sleep(dt);
}

void PhysicsRuntime::step(float deltaTime) {
    if (deltaTime <= 0.0f) return;
    if (external_) {
        external_->step(deltaTime);
        // Pull the simulated state back into the RigidBody records so the
        // existing consumers (ragdoll pose, weapons, gameplay runtimes) work
        // unchanged on the external backend.
        for (std::size_t i = 1; i < bodies_.size(); ++i) {
            if (!bodies_[i]) continue;
            const BodyHandle backendHandle = to_backend_handle(static_cast<BodyHandle>(i));
            if (backendHandle == InvalidBody) continue;
            glm::vec3 position, linearVelocity, angularVelocity;
            glm::quat rotation;
            bool sleeping = false;
            if (external_->get_state(backendHandle, position, rotation, linearVelocity, angularVelocity, sleeping)) {
                RigidBody& value = *bodies_[i];
                value.previousPosition = value.position;
                value.position = position;
                value.rotation = glm::normalize(rotation);
                value.linearVelocity = linearVelocity;
                value.angularVelocity = angularVelocity;
                value.sleeping = sleeping;
                value.accumulatedForce = {};
                value.accumulatedTorque = {};
            }
        }
        contacts_ = external_->contacts();
        triggerEvents_ = external_->trigger_events();
        return;
    }
    const float frame = std::min(deltaTime, 0.25f);
    std::uint32_t substeps = std::max(1u, static_cast<std::uint32_t>(std::ceil(frame / std::max(settings_.fixedStep, Epsilon))));
    for (const auto& slot : bodies_) if (slot && slot->dynamic() && slot->continuous) {
        const float travel = safe_length(slot->linearVelocity) * frame;
        substeps = std::max(substeps, static_cast<std::uint32_t>(std::ceil(travel / shape_min_extent(slot->collider.shape))));
    }
    substeps = std::min(std::max(1u, settings_.maxCcdSubsteps), substeps);
    const float dt = frame / static_cast<float>(substeps);
    for (std::uint32_t i = 0; i < substeps; ++i) step_substep(dt);
}

std::optional<RaycastHit> PhysicsRuntime::raycast(const glm::vec3& origin, const glm::vec3& direction,
                                                   float maxDistance, std::uint32_t layerMask, BodyHandle ignoredBody) const {
    if (maxDistance <= 0.0f || safe_length(direction) <= Epsilon) return std::nullopt;
    if (external_) {
        auto hit = external_->raycast(origin, direction, maxDistance, layerMask, ignoredBody);
        if (hit) hit->body = to_engine_handle(hit->body);
        return hit;
    }
    const glm::vec3 ray = glm::normalize(direction);
    std::optional<RaycastHit> closest;
    float closestDistance = maxDistance;
    for (BodyHandle handle = 1; handle < bodies_.size(); ++handle) if (bodies_[handle] && handle != ignoredBody) {
        const RigidBody& value = *bodies_[handle];
        if ((value.collider.filter.layer & layerMask) == 0u) continue;
        const Aabb box = compute_aabb(value);
        float nearTime = 0.0f, farTime = closestDistance;
        glm::vec3 nearNormal(0.0f);
        bool hit = true;
        for (int axis = 0; axis < 3; ++axis) {
            if (std::abs(ray[axis]) < Epsilon) { if (origin[axis] < box.minimum[axis] || origin[axis] > box.maximum[axis]) { hit = false; break; } continue; }
            float t1 = (box.minimum[axis] - origin[axis]) / ray[axis];
            float t2 = (box.maximum[axis] - origin[axis]) / ray[axis];
            float sign = -1.0f;
            if (t1 > t2) { std::swap(t1, t2); sign = 1.0f; }
            if (t1 > nearTime) { nearTime = t1; nearNormal = {}; nearNormal[axis] = sign; }
            farTime = std::min(farTime, t2);
            if (nearTime > farTime) { hit = false; break; }
        }
        if (hit && nearTime >= 0.0f && nearTime < closestDistance) {
            closestDistance = nearTime;
            closest = RaycastHit{handle, origin + ray * nearTime, nearNormal, nearTime};
        }
    }
    return closest;
}

std::vector<BodyHandle> PhysicsRuntime::overlap_aabb(const Aabb& bounds, std::uint32_t layerMask) const {
    if (external_) {
        std::vector<BodyHandle> result = external_->overlap_aabb(bounds, layerMask);
        for (BodyHandle& handle : result) handle = to_engine_handle(handle);
        return result;
    }
    std::vector<BodyHandle> result;
    for (BodyHandle handle = 1; handle < bodies_.size(); ++handle) if (bodies_[handle]) {
        const RigidBody& value = *bodies_[handle];
        if ((value.collider.filter.layer & layerMask) != 0u && bounds.overlaps(compute_aabb(value))) result.push_back(handle);
    }
    return result;
}

std::vector<DebugLine> PhysicsRuntime::debug_geometry(bool includeSleeping) const {
    if (external_) return external_->debug_geometry();
    std::vector<DebugLine> lines;
    for (const auto& slot : bodies_) if (slot && (includeSleeping || !slot->sleeping)) {
        glm::vec4 color = slot->collider.trigger ? glm::vec4(1.0f, 0.75f, 0.15f, 1.0f) :
                          slot->sleeping ? glm::vec4(0.3f, 0.55f, 0.85f, 1.0f) :
                          slot->motion == MotionType::Static ? glm::vec4(0.35f, 0.9f, 0.4f, 1.0f) : glm::vec4(0.9f, 0.3f, 0.25f, 1.0f);
        append_box(lines, compute_aabb(*slot), color);
    }
    for (const Contact& contact : contacts_) {
        lines.push_back({contact.point, contact.point + contact.normal * std::max(0.1f, contact.penetration), {1.0f,1.0f,0.1f,1.0f}});
    }
    for (const auto& slot : constraints_) if (slot && !slot->broken) {
        const RigidBody* a = body(slot->bodyA); const RigidBody* b = body(slot->bodyB);
        if (a && b) lines.push_back({a->position + a->rotation * slot->localAnchorA, b->position + b->rotation * slot->localAnchorB, {0.8f,0.2f,1.0f,1.0f}});
    }
    return lines;
}

} // namespace Engine::Physics
