#include "BulletPhysicsBackend.hpp"

#include <glm/gtc/quaternion.hpp>

#include <btBulletDynamicsCommon.h>
#include <BulletCollision/BroadphaseCollision/btOverlappingPairCache.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <set>
#include <unordered_map>

namespace Engine::Physics {

namespace {

btVector3 to_bt(const glm::vec3& v) { return btVector3(v.x, v.y, v.z); }
btQuaternion to_bt(const glm::quat& q) { return btQuaternion(q.x, q.y, q.z, q.w); }
glm::vec3 to_glm(const btVector3& v) { return {v.x(), v.y(), v.z()}; }
glm::quat to_glm(const btQuaternion& q) { return {q.w(), q.x(), q.y(), q.z()}; }

struct BulletBodyRecord {
    btRigidBody* rigid{nullptr};              // solid bodies and sensors alike
    btDefaultMotionState* motionState{nullptr};
    std::vector<btCollisionShape*> shapes;    // owned (base + compound children)
    CollisionFilter filter;
    bool dynamic{false};
    bool sensor{false};
};

std::uint64_t trigger_pair_key(BodyHandle a, BodyHandle b) {
    return a < b ? (std::uint64_t(a) << 32) | b : (std::uint64_t(b) << 32) | a;
}

} // namespace

struct BulletPhysicsBackend::Impl {
    Impl(const WorldSettings& settings) : settings(settings) {
        collisionConfig = std::make_unique<btDefaultCollisionConfiguration>();
        dispatcher = std::make_unique<btCollisionDispatcher>(collisionConfig.get());
        broadphase = std::make_unique<btDbvtBroadphase>();
        solver = std::make_unique<btSequentialImpulseConstraintSolver>();
        world = std::make_unique<btDiscreteDynamicsWorld>(dispatcher.get(), broadphase.get(), solver.get(),
                                                          collisionConfig.get());
        world->setGravity(to_bt(settings.gravity));
        // Note: this Bullet version's internal ghost pair callback is dead code
        // in the pair cache and its handler returns null for non-ghost pairs,
        // which would drop every collision pair. Sensors are handled as rigid
        // bodies with CF_NO_CONTACT_RESPONSE and triggers are read from the
        // broadphase pair array instead.
    }

    WorldSettings settings;
    std::unique_ptr<btDefaultCollisionConfiguration> collisionConfig;
    std::unique_ptr<btCollisionDispatcher> dispatcher;
    std::unique_ptr<btDbvtBroadphase> broadphase;
    std::unique_ptr<btSequentialImpulseConstraintSolver> solver;
    std::unique_ptr<btDiscreteDynamicsWorld> world;

    std::unordered_map<BodyHandle, BulletBodyRecord> bodies_;
    BodyHandle nextBodyHandle_{1};

    struct ConstraintRecord {
        btTypedConstraint* ptr{nullptr};
        BodyHandle bodyA{InvalidBody};
        BodyHandle bodyB{InvalidBody};
    };
    std::unordered_map<ConstraintHandle, ConstraintRecord> constraints_;
    ConstraintHandle nextConstraintHandle_{1};

    std::set<std::uint64_t> previousTriggerPairs_;
    std::vector<Contact> contacts_;
    std::vector<TriggerEvent> triggerEvents_;

    BodyHandle handle_of(const btCollisionObject* object) const {
        const std::uintptr_t value = reinterpret_cast<std::uintptr_t>(object->getUserPointer());
        return value != 0u ? static_cast<BodyHandle>(value) : InvalidBody;
    }
};

BulletPhysicsBackend::BulletPhysicsBackend(const WorldSettings& settings) : impl_(std::make_unique<Impl>(settings)) {}
BulletPhysicsBackend::~BulletPhysicsBackend() = default;

void BulletPhysicsBackend::set_gravity(const glm::vec3& gravity) {
    impl_->world->setGravity(to_bt(gravity));
}

void BulletPhysicsBackend::step(float deltaTime) {
    if (deltaTime <= 0.0f) return;
    const float frame = std::min(deltaTime, 0.25f);
    const int maxSubSteps = std::min(static_cast<int>(std::max(1u, impl_->settings.maxCcdSubsteps)),
                                     std::max(1, static_cast<int>(std::ceil(frame / std::max(impl_->settings.fixedStep, 1.0e-6f)))));
    impl_->world->stepSimulation(frame, maxSubSteps, impl_->settings.fixedStep);

    // Contacts from the persistent manifolds.
    impl_->contacts_.clear();
    const int numManifolds = impl_->dispatcher->getNumManifolds();
    for (int i = 0; i < numManifolds; ++i) {
        btPersistentManifold* manifold = impl_->dispatcher->getManifoldByIndexInternal(i);
        if (manifold == nullptr || manifold->getNumContacts() == 0) continue;
        const btCollisionObject* a = manifold->getBody0();
        const btCollisionObject* b = manifold->getBody1();
        if (a->getCollisionFlags() & btCollisionObject::CF_NO_CONTACT_RESPONSE) continue;
        if (b->getCollisionFlags() & btCollisionObject::CF_NO_CONTACT_RESPONSE) continue;
        const BodyHandle handleA = impl_->handle_of(a);
        const BodyHandle handleB = impl_->handle_of(b);
        if (handleA == InvalidBody || handleB == InvalidBody) continue;
        const btManifoldPoint& point = manifold->getContactPoint(0);
        Contact contact;
        contact.bodyA = handleA;
        contact.bodyB = handleB;
        contact.point = to_glm(point.m_positionWorldOnB);
        contact.normal = to_glm(point.m_normalWorldOnB);
        contact.penetration = std::max(0.0f, -point.m_distance1);
        contact.restitution = point.m_combinedRestitution;
        contact.friction = point.m_combinedFriction;
        impl_->contacts_.push_back(contact);
    }

    // Trigger diff: current overlapping pairs involving sensors vs previous.
    std::set<std::uint64_t> current;
    const btBroadphasePairArray& pairs =
        impl_->broadphase->getOverlappingPairCache()->getOverlappingPairArray();
    for (int i = 0; i < pairs.size(); ++i) {
        const btBroadphasePair& pair = pairs[i];
        const btCollisionObject* a = static_cast<const btCollisionObject*>(pair.m_pProxy0->m_clientObject);
        const btCollisionObject* b = static_cast<const btCollisionObject*>(pair.m_pProxy1->m_clientObject);
        const BodyHandle handleA = impl_->handle_of(a);
        const BodyHandle handleB = impl_->handle_of(b);
        if (handleA == InvalidBody || handleB == InvalidBody) continue;
        const auto itA = impl_->bodies_.find(handleA);
        const auto itB = impl_->bodies_.find(handleB);
        if (itA == impl_->bodies_.end() || itB == impl_->bodies_.end()) continue;
        if (!itA->second.sensor && !itB->second.sensor) continue;
        current.insert(trigger_pair_key(handleA, handleB));
    }
    impl_->triggerEvents_.clear();
    const auto emitEnterOrStay = [&](const std::uint64_t pair, bool existed) {
        const BodyHandle a = static_cast<BodyHandle>(pair >> 32);
        const BodyHandle b = static_cast<BodyHandle>(pair & 0xffffffffu);
        const auto itA = impl_->bodies_.find(a);
        const auto itB = impl_->bodies_.find(b);
        if (itA == impl_->bodies_.end() || itB == impl_->bodies_.end()) return;
        const bool sensorA = itA->second.sensor;
        const bool sensorB = itB->second.sensor;
        const TriggerEvent::Type type = existed ? TriggerEvent::Type::Stay : TriggerEvent::Type::Enter;
        if (sensorA) impl_->triggerEvents_.push_back({type, a, b});
        if (sensorB) impl_->triggerEvents_.push_back({type, b, a});
    };
    for (const std::uint64_t pair : current) emitEnterOrStay(pair, impl_->previousTriggerPairs_.count(pair) != 0);
    for (const std::uint64_t pair : impl_->previousTriggerPairs_) {
        if (current.count(pair) != 0) continue;
        const BodyHandle a = static_cast<BodyHandle>(pair >> 32);
        const BodyHandle b = static_cast<BodyHandle>(pair & 0xffffffffu);
        const auto itA = impl_->bodies_.find(a);
        const auto itB = impl_->bodies_.find(b);
        if (itA == impl_->bodies_.end() || itB == impl_->bodies_.end()) continue;
        if (itA->second.sensor) impl_->triggerEvents_.push_back({TriggerEvent::Type::Exit, a, b});
        if (itB->second.sensor) impl_->triggerEvents_.push_back({TriggerEvent::Type::Exit, b, a});
    }
    impl_->previousTriggerPairs_ = std::move(current);
}

BodyHandle BulletPhysicsBackend::create_body(const BodyDesc& description) {
    btCollisionShape* shape = nullptr;
    std::vector<btCollisionShape*> owned;

    const Collider& collider = description.collider;
    std::visit([&](const auto& concrete) {
        using T = std::decay_t<decltype(concrete)>;
        if constexpr (std::is_same_v<T, SphereShape>) {
            shape = new btSphereShape(std::max(0.01f, concrete.radius));
        } else if constexpr (std::is_same_v<T, BoxShape>) {
            shape = new btBoxShape(to_bt(glm::max(concrete.halfExtents, glm::vec3(0.01f))));
        } else if constexpr (std::is_same_v<T, CapsuleShape>) {
            // Bullet capsule: radius + total height of the cylinder part.
            shape = new btCapsuleShape(std::max(0.01f, concrete.radius), std::max(0.02f, 2.0f * concrete.halfHeight));
        }
    }, collider.shape);
    if (shape == nullptr) return InvalidBody;
    owned.push_back(shape);

    const glm::quat localRotation = glm::normalize(collider.localRotation);
    const bool translated = glm::dot(collider.localPosition, collider.localPosition) > 1.0e-9f ||
                            glm::abs(glm::dot(localRotation, glm::quat(1.0f, 0.0f, 0.0f, 0.0f))) < 0.999999f;
    if (translated) {
        auto* compound = new btCompoundShape();
        owned.push_back(compound);
        btTransform child;
        child.setIdentity();
        child.setOrigin(to_bt(collider.localPosition));
        child.setRotation(to_bt(localRotation));
        compound->addChildShape(child, shape);
        shape = compound;
    }

    const BodyHandle handle = impl_->nextBodyHandle_++;
    const bool sensor = collider.trigger;

    btTransform transform;
    transform.setIdentity();
    transform.setOrigin(to_bt(description.position));
    transform.setRotation(to_bt(glm::normalize(description.rotation)));

    const int layerBit = 1 << ((collider.filter.layer - 1) & 30);
    const int mask = static_cast<int>(collider.filter.mask);

    BulletBodyRecord record;
    record.shapes = std::move(owned);
    record.filter = collider.filter;
    record.dynamic = description.motion == MotionType::Dynamic;
    record.sensor = sensor;

    const btScalar mass = description.motion == MotionType::Dynamic && !sensor ? std::max(0.0f, description.mass) : 0.0f;
    btVector3 inertia(0.0f, 0.0f, 0.0f);
    if (mass > 0.0f) shape->calculateLocalInertia(mass, inertia);
    auto* motionState = new btDefaultMotionState(transform);
    btRigidBody::btRigidBodyConstructionInfo info(mass, motionState, shape, inertia);
    info.m_friction = std::max(0.0f, collider.friction);
    info.m_restitution = std::max(0.0f, collider.restitution);
    info.m_linearDamping = std::max(0.0f, description.linearDamping);
    info.m_angularDamping = std::max(0.0f, description.angularDamping);
    auto* body = new btRigidBody(info);
    if (sensor) {
        body->setCollisionFlags(body->getCollisionFlags() | btCollisionObject::CF_NO_CONTACT_RESPONSE);
    } else if (description.motion == MotionType::Dynamic) {
        body->setGravity(to_bt(impl_->settings.gravity * description.gravityScale));
        if (description.continuous) {
            btVector3 minExtent, maxExtent;
            shape->getAabb(btTransform::getIdentity(), minExtent, maxExtent);
            const btVector3 extents = maxExtent - minExtent;
            // CCD swept sphere must be smaller than the shape's smallest half extent.
            const btScalar radius = std::max(0.05f, 0.45f * 0.5f * std::min(extents.x(), std::min(extents.y(), extents.z())));
            body->setCcdMotionThreshold(1.0f);
            body->setCcdSweptSphereRadius(radius);
        }
    }
    body->setLinearVelocity(to_bt(description.linearVelocity));
    body->setAngularVelocity(to_bt(description.angularVelocity));
    body->setUserPointer(reinterpret_cast<void*>(static_cast<std::uintptr_t>(handle)));
    impl_->world->addRigidBody(body, layerBit, mask);
    record.rigid = body;
    record.motionState = motionState;

    impl_->bodies_[handle] = std::move(record);
    return handle;
}

bool BulletPhysicsBackend::destroy_body(BodyHandle body) {
    const auto it = impl_->bodies_.find(body);
    if (it == impl_->bodies_.end()) return false;

    std::vector<ConstraintHandle> toRemove;
    for (const auto& [handle, record] : impl_->constraints_) {
        if (record.bodyA == body || record.bodyB == body) toRemove.push_back(handle);
    }
    for (const ConstraintHandle handle : toRemove) destroy_constraint(handle);

    BulletBodyRecord& record = it->second;
    impl_->world->removeRigidBody(record.rigid);
    delete record.rigid->getMotionState();
    delete record.rigid;
    for (btCollisionShape* shape : record.shapes) delete shape;
    impl_->bodies_.erase(it);
    return true;
}

void BulletPhysicsBackend::set_transform(BodyHandle body, const glm::vec3& position, const glm::quat& rotation) {
    const auto it = impl_->bodies_.find(body);
    if (it == impl_->bodies_.end() || it->second.rigid == nullptr) return;
    btTransform transform;
    transform.setIdentity();
    transform.setOrigin(to_bt(position));
    transform.setRotation(to_bt(glm::normalize(rotation)));
    it->second.rigid->setWorldTransform(transform);
    it->second.rigid->activate();
}

bool BulletPhysicsBackend::get_state(BodyHandle body, glm::vec3& position, glm::quat& rotation,
                                     glm::vec3& linearVelocity, glm::vec3& angularVelocity, bool& sleeping) const {
    const auto it = impl_->bodies_.find(body);
    if (it == impl_->bodies_.end() || it->second.rigid == nullptr) return false;
    const btRigidBody* rigid = it->second.rigid;
    const btTransform& transform = rigid->getWorldTransform();
    position = to_glm(transform.getOrigin());
    rotation = to_glm(transform.getRotation());
    linearVelocity = to_glm(rigid->getLinearVelocity());
    angularVelocity = to_glm(rigid->getAngularVelocity());
    sleeping = rigid->getActivationState() == ISLAND_SLEEPING;
    return true;
}

void BulletPhysicsBackend::set_linear_velocity(BodyHandle body, const glm::vec3& velocity) {
    const auto it = impl_->bodies_.find(body);
    if (it == impl_->bodies_.end() || it->second.rigid == nullptr) return;
    it->second.rigid->setLinearVelocity(to_bt(velocity));
    it->second.rigid->activate();
}

void BulletPhysicsBackend::set_gravity_scale(BodyHandle body, float scale) {
    const auto it = impl_->bodies_.find(body);
    if (it == impl_->bodies_.end() || it->second.rigid == nullptr) return;
    it->second.rigid->setGravity(to_bt(impl_->settings.gravity * scale));
}

void BulletPhysicsBackend::set_linear_damping(BodyHandle body, float damping) {
    const auto it = impl_->bodies_.find(body);
    if (it == impl_->bodies_.end() || it->second.rigid == nullptr) return;
    it->second.rigid->setDamping(damping, it->second.rigid->getAngularDamping());
}

void BulletPhysicsBackend::add_force(BodyHandle body, const glm::vec3& force) {
    const auto it = impl_->bodies_.find(body);
    if (it == impl_->bodies_.end() || it->second.rigid == nullptr) return;
    it->second.rigid->applyCentralForce(to_bt(force));
    it->second.rigid->activate();
}

void BulletPhysicsBackend::apply_impulse(BodyHandle body, const glm::vec3& impulse) {
    const auto it = impl_->bodies_.find(body);
    if (it == impl_->bodies_.end() || it->second.rigid == nullptr) return;
    it->second.rigid->applyCentralImpulse(to_bt(impulse));
    it->second.rigid->activate();
}

void BulletPhysicsBackend::apply_impulse_at_point(BodyHandle body, const glm::vec3& impulse, const glm::vec3& worldPoint) {
    const auto it = impl_->bodies_.find(body);
    if (it == impl_->bodies_.end() || it->second.rigid == nullptr) return;
    const btVector3 relative = to_bt(worldPoint) - it->second.rigid->getCenterOfMassPosition();
    it->second.rigid->applyImpulse(to_bt(impulse), relative);
    it->second.rigid->activate();
}

void BulletPhysicsBackend::wake(BodyHandle body) {
    const auto it = impl_->bodies_.find(body);
    if (it == impl_->bodies_.end() || it->second.rigid == nullptr) return;
    it->second.rigid->activate();
}

ConstraintHandle BulletPhysicsBackend::create_distance_constraint(const DistanceConstraintDesc& description) {
    const auto itA = impl_->bodies_.find(description.bodyA);
    const auto itB = impl_->bodies_.find(description.bodyB);
    if (itA == impl_->bodies_.end() || itB == impl_->bodies_.end()) return InvalidConstraint;
    if (itA->second.rigid == nullptr || itB->second.rigid == nullptr) return InvalidConstraint;

    // Distance joint semantics: keep the two local anchors at their current
    // world separation (the engine's restLength). btPoint2PointConstraint drives
    // its pivots together, so the pivot on B is placed at the world position of
    // A's anchor — the pair then preserves the initial anchor distance.
    const btTransform& transformA = itA->second.rigid->getWorldTransform();
    const btTransform& transformB = itB->second.rigid->getWorldTransform();
    const btVector3 anchorAWorld = transformA * to_bt(description.localAnchorA);
    const btVector3 pivotB = transformB.inverse() * anchorAWorld;
    auto* constraint = new btPoint2PointConstraint(*itA->second.rigid, *itB->second.rigid,
                                                   to_bt(description.localAnchorA), pivotB);
    impl_->world->addConstraint(constraint, true);
    const ConstraintHandle handle = impl_->nextConstraintHandle_++;
    impl_->constraints_[handle] = {constraint, description.bodyA, description.bodyB};
    return handle;
}

bool BulletPhysicsBackend::destroy_constraint(ConstraintHandle constraint) {
    const auto it = impl_->constraints_.find(constraint);
    if (it == impl_->constraints_.end()) return false;
    impl_->world->removeConstraint(it->second.ptr);
    delete it->second.ptr;
    impl_->constraints_.erase(it);
    return true;
}

std::optional<RaycastHit> BulletPhysicsBackend::raycast(const glm::vec3& origin, const glm::vec3& direction,
                                                        float maxDistance, std::uint32_t layerMask,
                                                        BodyHandle ignoredBody) const {
    if (maxDistance <= 0.0f) return std::nullopt;
    const glm::vec3 ray = glm::normalize(direction);
    if (glm::dot(ray, ray) < 1.0e-12f) return std::nullopt;

    btCollisionWorld::ClosestRayResultCallback callback(to_bt(origin), to_bt(origin + ray * maxDistance));
    callback.m_collisionFilterGroup = ~0;
    callback.m_collisionFilterMask = static_cast<int>(layerMask);
    impl_->world->rayTest(callback.m_rayFromWorld, callback.m_rayToWorld, callback);
    if (!callback.hasHit() || callback.m_collisionObject == nullptr) return std::nullopt;

    const BodyHandle handle = impl_->handle_of(callback.m_collisionObject);
    if (handle == InvalidBody || handle == ignoredBody) return std::nullopt;
    const float distance = callback.m_closestHitFraction * maxDistance;
    return RaycastHit{handle, to_glm(callback.m_hitPointWorld), to_glm(callback.m_hitNormalWorld), distance};
}

std::vector<BodyHandle> BulletPhysicsBackend::overlap_aabb(const Aabb& bounds, std::uint32_t layerMask) const {
    std::vector<BodyHandle> result;
    const btVector3 queryMin = to_bt(bounds.minimum);
    const btVector3 queryMax = to_bt(bounds.maximum);
    for (int i = 0; i < impl_->world->getNumCollisionObjects(); ++i) {
        const btCollisionObject* object = impl_->world->getCollisionObjectArray()[i];
        const BodyHandle handle = impl_->handle_of(object);
        if (handle == InvalidBody) continue;
        const auto it = impl_->bodies_.find(handle);
        if (it == impl_->bodies_.end()) continue;
        if ((it->second.filter.layer & layerMask) == 0u) continue;
        btVector3 minExtent, maxExtent;
        object->getCollisionShape()->getAabb(object->getWorldTransform(), minExtent, maxExtent);
        if (minExtent.x() <= queryMax.x() && maxExtent.x() >= queryMin.x() &&
            minExtent.y() <= queryMax.y() && maxExtent.y() >= queryMin.y() &&
            minExtent.z() <= queryMax.z() && maxExtent.z() >= queryMin.z()) {
            result.push_back(handle);
        }
    }
    return result;
}

std::vector<Contact> BulletPhysicsBackend::contacts() { return impl_->contacts_; }

std::vector<TriggerEvent> BulletPhysicsBackend::trigger_events() { return std::move(impl_->triggerEvents_); }

std::vector<DebugLine> BulletPhysicsBackend::debug_geometry() const {
    std::vector<DebugLine> lines;
    for (const auto& [handle, record] : impl_->bodies_) {
        const btCollisionObject* object = record.rigid;
        const glm::vec4 color = record.sensor ? glm::vec4(1.0f, 0.75f, 0.15f, 1.0f)
                                : record.dynamic ? glm::vec4(0.9f, 0.3f, 0.25f, 1.0f)
                                                 : glm::vec4(0.35f, 0.9f, 0.4f, 1.0f);
        btVector3 minExtent, maxExtent;
        object->getCollisionShape()->getAabb(object->getWorldTransform(), minExtent, maxExtent);
        const glm::vec3 mn = to_glm(minExtent);
        const glm::vec3 mx = to_glm(maxExtent);
        const glm::vec3 p[8] = {
            {mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}, {mx.x, mx.y, mn.z}, {mn.x, mx.y, mn.z},
            {mn.x, mn.y, mx.z}, {mx.x, mn.y, mx.z}, {mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z}};
        constexpr int edges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (const auto& edge : edges) lines.push_back({p[edge[0]], p[edge[1]], color});
    }
    return lines;
}

} // namespace Engine::Physics
