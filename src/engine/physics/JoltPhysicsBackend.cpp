#include "JoltPhysicsBackend.hpp"

#include <glm/gtc/quaternion.hpp>

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/Reference.h>
#include <Jolt/Physics/EActivation.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseQuery.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/CollisionCollector.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/Constraints/MotorSettings.h>
#include <Jolt/Physics/Constraints/ConstraintManager.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/VehicleCollisionTester.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>
#include <Jolt/Physics/Vehicle/MotorcycleController.h>
#include <Jolt/Physics/Vehicle/TrackedVehicleController.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <set>
#include <unordered_map>

namespace Engine::Physics {

namespace {

std::once_flag g_joltOnce;
void ensure_jolt_registered() {
    std::call_once(g_joltOnce, [] {
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    });
}

JPH::Vec3 to_jolt(const glm::vec3& v) { return JPH::Vec3(v.x, v.y, v.z); }
JPH::RVec3 to_rjolt(const glm::vec3& v) { return JPH::RVec3(v.x, v.y, v.z); }
JPH::Quat to_jolt(const glm::quat& q) { return JPH::Quat(q.x, q.y, q.z, q.w); }
glm::vec3 to_glm(const JPH::Vec3& v) { return {v.GetX(), v.GetY(), v.GetZ()}; }
// Note: this build of Jolt is single-precision (no JPH_DOUBLE_PRECISION), so
// JPH::RVec3 is an alias of JPH::Vec3 and needs no separate overload.
glm::quat to_glm(const JPH::Quat& q) { return {q.GetW(), q.GetX(), q.GetY(), q.GetZ()}; }

std::uint64_t pair_key(std::uint32_t a, std::uint32_t b) {
    return a < b ? (std::uint64_t(a) << 32) | b : (std::uint64_t(b) << 32) | a;
}

// Maps each unique engine (layer, mask) combo to a Jolt object layer so the
// engine CollisionFilter semantics (mask & other.layer both ways) hold.
class FilterRegistry {
public:
    static constexpr std::uint16_t kMaxLayers = 63;

    FilterRegistry() { combos_.push_back({1u, ~0u}); } // layer 1 = wildcard

    std::uint16_t layer_for(const CollisionFilter& filter) {
        const std::uint64_t key = (std::uint64_t(filter.layer) << 32) | filter.mask;
        const auto it = byKey_.find(key);
        if (it != byKey_.end()) return it->second;
        if (combos_.size() < kMaxLayers) {
            combos_.push_back({filter.layer, filter.mask});
            const std::uint16_t id = static_cast<std::uint16_t>(combos_.size());
            byKey_[key] = id;
            return id;
        }
        return 1; // budget exhausted: fall back to the wildcard layer
    }

    bool should_collide(std::uint16_t a, std::uint16_t b) const {
        if (a == 0 || b == 0 || a > combos_.size() || b > combos_.size()) return true;
        const FilterCombo& fa = combos_[a - 1];
        const FilterCombo& fb = combos_[b - 1];
        return (fa.mask & fb.layer) != 0u && (fb.mask & fa.layer) != 0u;
    }

private:
    struct FilterCombo { std::uint32_t layer; std::uint32_t mask; };
    std::vector<FilterCombo> combos_;
    std::unordered_map<std::uint64_t, std::uint16_t> byKey_;
};

class EngineBroadPhaseLayerInterface final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return 1; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer) const override { return JPH::BroadPhaseLayer(0); }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer) const override { return "Default"; }
#endif
};

class EngineObjectVsBroadPhaseLayerFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer, JPH::BroadPhaseLayer) const override { return true; }
};

class EngineObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    explicit EngineObjectLayerPairFilter(const FilterRegistry& registry) : registry_(registry) {}
    bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::ObjectLayer inLayer2) const override {
        return registry_.should_collide(inLayer1, inLayer2);
    }
private:
    const FilterRegistry& registry_;
};

struct JoltBodyRecord {
    JPH::BodyID id;
    CollisionFilter filter;
    bool sensor{false};
};

struct RawContact {
    std::uint32_t body1{0};
    std::uint32_t body2{0};
    glm::vec3 point{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    float penetration{0.0f};
    float restitution{0.0f};
    float friction{0.0f};
};

class EngineContactListener final : public JPH::ContactListener {
public:
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        rawContacts_.clear();
        pairsThisStep_.clear();
    }

    JPH::ValidateResult OnContactValidate(const JPH::Body&, const JPH::Body&, JPH::RVec3Arg,
                                          const JPH::CollideShapeResult&) override {
        return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
    }

    void OnContactAdded(const JPH::Body& inBody1, const JPH::Body& inBody2, const JPH::ContactManifold& inManifold,
                        JPH::ContactSettings& ioSettings) override {
        record(inBody1, inBody2, inManifold, ioSettings);
    }

    void OnContactPersisted(const JPH::Body& inBody1, const JPH::Body& inBody2, const JPH::ContactManifold& inManifold,
                            JPH::ContactSettings& ioSettings) override {
        record(inBody1, inBody2, inManifold, ioSettings);
    }

    // Contact callbacks run on Jolt's job threads during Update(); the main
    // thread clears (before Update) and reads (after Update) these containers.
    // Return copies under the lock so no reference escapes into job-thread
    // storage; reads happen after all jobs are joined, so this only serializes
    // the concurrent record() writers during the step.
    std::vector<RawContact> raw_contacts() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return rawContacts_;
    }

    std::set<std::uint64_t> pairs_this_step() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pairsThisStep_;
    }

private:
    void record(const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold& manifold,
                const JPH::ContactSettings& settings) {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::uint32_t id1 = b1.GetID().GetIndexAndSequenceNumber();
        const std::uint32_t id2 = b2.GetID().GetIndexAndSequenceNumber();
        pairsThisStep_.insert(pair_key(id1, id2));
        if (b1.IsSensor() || b2.IsSensor()) return; // sensors feed triggers, not contact points
        RawContact contact;
        contact.body1 = id1;
        contact.body2 = id2;
        if (!manifold.mRelativeContactPointsOn2.empty()) {
            const JPH::Vec3 relative = manifold.mRelativeContactPointsOn2[0];
            contact.point = to_glm(manifold.mBaseOffset + relative);
        } else {
            contact.point = to_glm(manifold.mBaseOffset);
        }
        contact.normal = to_glm(manifold.mWorldSpaceNormal);
        contact.penetration = manifold.mPenetrationDepth;
        contact.restitution = settings.mCombinedRestitution;
        contact.friction = settings.mCombinedFriction;
        rawContacts_.push_back(contact);
    }

    mutable std::mutex mutex_;
    std::vector<RawContact> rawContacts_;
    std::set<std::uint64_t> pairsThisStep_;
};

class EngineRayCollector final : public JPH::CastRayCollector {
public:
    EngineRayCollector(const std::unordered_map<std::uint32_t, BodyHandle>& bodyToHandle,
                       const std::unordered_map<BodyHandle, JoltBodyRecord>& bodies,
                       std::uint32_t layerMask, BodyHandle ignoredBody)
        : bodyToHandle_(bodyToHandle), bodies_(bodies), layerMask_(layerMask), ignoredBody_(ignoredBody) {}

    void AddHit(const JPH::RayCastResult& inResult) override {
        if (inResult.mFraction > GetEarlyOutFraction()) return;
        const auto it = bodyToHandle_.find(inResult.mBodyID.GetIndexAndSequenceNumber());
        if (it == bodyToHandle_.end() || it->second == ignoredBody_) return;
        const auto fit = bodies_.find(it->second);
        if (fit != bodies_.end() && (fit->second.filter.layer & layerMask_) == 0u) return;
        hit_ = inResult;
        UpdateEarlyOutFraction(inResult.mFraction);
    }

    std::optional<JPH::RayCastResult> hit_;

private:
    const std::unordered_map<std::uint32_t, BodyHandle>& bodyToHandle_;
    const std::unordered_map<BodyHandle, JoltBodyRecord>& bodies_;
    std::uint32_t layerMask_;
    BodyHandle ignoredBody_;
};

class EngineBodyCollector final : public JPH::CollideShapeBodyCollector {
public:
    void AddHit(const JPH::BodyID& inBodyID) override { ids.push_back(inBodyID); }
    std::vector<JPH::BodyID> ids;
};

JPH::EMotionType motion_type(MotionType motion) {
    switch (motion) {
        case MotionType::Static: return JPH::EMotionType::Static;
        case MotionType::Kinematic: return JPH::EMotionType::Kinematic;
        case MotionType::Dynamic: break;
    }
    return JPH::EMotionType::Dynamic;
}

JPH::RefConst<JPH::Shape> make_shape(const Collider& collider) {
    JPH::RefConst<JPH::Shape> inner;
    std::visit([&](const auto& concrete) {
        using T = std::decay_t<decltype(concrete)>;
        if constexpr (std::is_same_v<T, SphereShape>) {
            inner = new JPH::SphereShape(std::max(0.01f, concrete.radius));
        } else if constexpr (std::is_same_v<T, BoxShape>) {
            inner = new JPH::BoxShape(to_jolt(glm::max(concrete.halfExtents, glm::vec3(0.01f))));
        } else if constexpr (std::is_same_v<T, CapsuleShape>) {
            // Jolt capsule: radius + half height of the cylinder part.
            inner = new JPH::CapsuleShape(std::max(0.01f, concrete.radius), std::max(0.01f, concrete.halfHeight));
        }
    }, collider.shape);
    if (!inner) return nullptr;
    const glm::quat localRotation = glm::normalize(collider.localRotation);
    const bool translated = glm::dot(collider.localPosition, collider.localPosition) > 1.0e-9f ||
                            glm::abs(glm::dot(localRotation, glm::quat(1.0f, 0.0f, 0.0f, 0.0f))) < 0.999999f;
    if (!translated) return inner;
    return new JPH::RotatedTranslatedShape(to_jolt(collider.localPosition), to_jolt(localRotation), inner);
}

} // namespace

struct JoltPhysicsBackend::Impl {
    Impl(const WorldSettings& settings) : settings(settings) {
        ensure_jolt_registered();

        broadPhaseInterface = std::make_unique<EngineBroadPhaseLayerInterface>();
        objectVsBroadPhaseFilter = std::make_unique<EngineObjectVsBroadPhaseLayerFilter>();
        objectLayerPairFilter = std::make_unique<EngineObjectLayerPairFilter>(registry);
        contactListener = std::make_unique<EngineContactListener>();
        jobSystem = std::make_unique<JPH::JobSystemThreadPool>(1024, 8, 4);
        tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(16u * 1024u * 1024u);

        physicsSystem = std::make_unique<JPH::PhysicsSystem>();
        physicsSystem->Init(4096, 0, 65536, 20480, *broadPhaseInterface, *objectVsBroadPhaseFilter,
                            *objectLayerPairFilter);
        physicsSystem->SetGravity(to_jolt(settings.gravity));
        physicsSystem->SetContactListener(contactListener.get());
    }

    WorldSettings settings;
    FilterRegistry registry;
    std::unique_ptr<EngineBroadPhaseLayerInterface> broadPhaseInterface;
    std::unique_ptr<EngineObjectVsBroadPhaseLayerFilter> objectVsBroadPhaseFilter;
    std::unique_ptr<EngineObjectLayerPairFilter> objectLayerPairFilter;
    std::unique_ptr<EngineContactListener> contactListener;
    std::unique_ptr<JPH::JobSystemThreadPool> jobSystem;
    std::unique_ptr<JPH::TempAllocatorImpl> tempAllocator;
    std::unique_ptr<JPH::PhysicsSystem> physicsSystem;

    std::unordered_map<BodyHandle, JoltBodyRecord> bodies_;
    std::unordered_map<std::uint32_t, BodyHandle> bodyToHandle_;
    BodyHandle nextBodyHandle_{1};

    struct ConstraintRecord {
        JPH::Constraint* ptr{nullptr};
        BodyHandle bodyA{InvalidBody};
        BodyHandle bodyB{InvalidBody};
    };
    std::unordered_map<ConstraintHandle, ConstraintRecord> constraints_;
    ConstraintHandle nextConstraintHandle_{1};

    struct VehicleRecord {
        JPH::Ref<JPH::VehicleConstraint> constraint;
        BodyHandle chassis{InvalidBody};
        std::vector<WheelDesc> wheels;
        VehicleKind kind{ VehicleKind::Wheeled };
    };
    std::unordered_map<VehicleHandle, VehicleRecord> vehicles_;
    VehicleHandle nextVehicleHandle_{1};

    std::set<std::uint64_t> previousPairs_;
    std::vector<TriggerEvent> triggerEvents_;

    BodyHandle to_engine_handle(std::uint32_t joltId) const {
        const auto it = bodyToHandle_.find(joltId);
        return it != bodyToHandle_.end() ? it->second : InvalidBody;
    }
};

JoltPhysicsBackend::JoltPhysicsBackend(const WorldSettings& settings) : impl_(std::make_unique<Impl>(settings)) {}

JoltPhysicsBackend::~JoltPhysicsBackend() {
    // Vehicles are registered as step listeners and constraints in the physics
    // system; they must be removed before the physics system dies (the Impl
    // holds the system).
    for (const auto& [handle, record] : impl_->vehicles_) {
        (void)handle;
        impl_->physicsSystem->RemoveStepListener(record.constraint);
        impl_->physicsSystem->RemoveConstraint(record.constraint);
    }
    impl_->vehicles_.clear();
}

void JoltPhysicsBackend::set_gravity(const glm::vec3& gravity) {
    impl_->physicsSystem->SetGravity(to_jolt(gravity));
}

void JoltPhysicsBackend::step(float deltaTime) {
    if (deltaTime <= 0.0f) return;
    impl_->contactListener->clear();

    const float frame = std::min(deltaTime, 0.25f);
    int collisionSteps = std::max(1, static_cast<int>(std::ceil(frame / std::max(impl_->settings.fixedStep, 1.0e-6f))));
    collisionSteps = std::min(static_cast<int>(std::max(1u, impl_->settings.maxCcdSubsteps)), collisionSteps);
    impl_->physicsSystem->Update(frame, collisionSteps, impl_->tempAllocator.get(), impl_->jobSystem.get());

    // Trigger diff: current overlap pairs vs previous.
    const std::set<std::uint64_t> current = impl_->contactListener->pairs_this_step();
    const std::set<std::uint64_t>& previous = impl_->previousPairs_;
    impl_->triggerEvents_.clear();
    for (const std::uint64_t pair : current) {
        const std::uint32_t idA = static_cast<std::uint32_t>(pair >> 32);
        const std::uint32_t idB = static_cast<std::uint32_t>(pair & 0xffffffffu);
        const BodyHandle a = impl_->to_engine_handle(idA);
        const BodyHandle b = impl_->to_engine_handle(idB);
        if (a == InvalidBody || b == InvalidBody) continue;
        const auto itA = impl_->bodies_.find(a);
        const auto itB = impl_->bodies_.find(b);
        if (itA == impl_->bodies_.end() || itB == impl_->bodies_.end()) continue;
        const bool sensorA = itA->second.sensor;
        const bool sensorB = itB->second.sensor;
        if (!sensorA && !sensorB) continue;
        const bool existed = previous.count(pair) != 0;
        if (sensorA) impl_->triggerEvents_.push_back({existed ? TriggerEvent::Type::Stay : TriggerEvent::Type::Enter, a, b});
        if (sensorB) impl_->triggerEvents_.push_back({existed ? TriggerEvent::Type::Stay : TriggerEvent::Type::Enter, b, a});
    }
    for (const std::uint64_t pair : previous) {
        if (current.count(pair) != 0) continue;
        const std::uint32_t idA = static_cast<std::uint32_t>(pair >> 32);
        const std::uint32_t idB = static_cast<std::uint32_t>(pair & 0xffffffffu);
        const BodyHandle a = impl_->to_engine_handle(idA);
        const BodyHandle b = impl_->to_engine_handle(idB);
        if (a == InvalidBody || b == InvalidBody) continue;
        const auto itA = impl_->bodies_.find(a);
        const auto itB = impl_->bodies_.find(b);
        if (itA == impl_->bodies_.end() || itB == impl_->bodies_.end()) continue;
        if (itA->second.sensor) impl_->triggerEvents_.push_back({TriggerEvent::Type::Exit, a, b});
        if (itB->second.sensor) impl_->triggerEvents_.push_back({TriggerEvent::Type::Exit, b, a});
    }
    impl_->previousPairs_ = current;
}

BodyHandle JoltPhysicsBackend::create_body(const BodyDesc& description) {
    JPH::RefConst<JPH::Shape> shape = make_shape(description.collider);
    if (!shape) return InvalidBody;

    JPH::BodyCreationSettings creation(shape, to_rjolt(description.position), to_jolt(glm::normalize(description.rotation)),
                                       motion_type(description.motion),
                                       impl_->registry.layer_for(description.collider.filter));
    creation.mFriction = std::max(0.0f, description.collider.friction);
    creation.mRestitution = std::max(0.0f, description.collider.restitution);
    creation.mLinearDamping = std::max(0.0f, description.linearDamping);
    creation.mAngularDamping = std::max(0.0f, description.angularDamping);
    creation.mGravityFactor = description.gravityScale;
    creation.mAllowSleeping = description.allowSleep;
    creation.mIsSensor = description.collider.trigger;
    creation.mMotionQuality = description.continuous ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
    creation.mUserData = description.userData;
    if (description.motion == MotionType::Dynamic && description.mass > 0.0f) {
        JPH::MassProperties massProperties = shape->GetMassProperties();
        if (massProperties.mMass > 1.0e-6f) {
            const float scale = description.mass / massProperties.mMass;
            massProperties.mMass = description.mass;
            massProperties.mInertia = massProperties.mInertia * scale;
            creation.mMassPropertiesOverride = massProperties;
            creation.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        }
    }

    JPH::BodyInterface& bodyInterface = impl_->physicsSystem->GetBodyInterface();
    JPH::Body* body = bodyInterface.CreateBody(creation);
    if (!body) return InvalidBody;
    bodyInterface.AddBody(body->GetID(), JPH::EActivation::Activate);
    // BodyDesc.linearVelocity/angularVelocity must survive into Jolt (initial
    // state of a spawned body — the builtin backend keeps them on the
    // RigidBody, so the Jolt path was silently dropping them).
    bodyInterface.SetLinearVelocity(body->GetID(), to_jolt(description.linearVelocity));
    bodyInterface.SetAngularVelocity(body->GetID(), to_jolt(description.angularVelocity));

    const BodyHandle handle = impl_->nextBodyHandle_++;
    impl_->bodies_[handle] = {body->GetID(), description.collider.filter, description.collider.trigger};
    impl_->bodyToHandle_[body->GetID().GetIndexAndSequenceNumber()] = handle;
    return handle;
}

bool JoltPhysicsBackend::destroy_body(BodyHandle body) {
    const auto it = impl_->bodies_.find(body);
    if (it == impl_->bodies_.end()) return false;

    // Remove constraints attached to this body first.
    std::vector<ConstraintHandle> toRemove;
    for (const auto& [handle, record] : impl_->constraints_) {
        if (record.bodyA == body || record.bodyB == body) toRemove.push_back(handle);
    }
    for (const ConstraintHandle handle : toRemove) destroy_constraint(handle);

    // Remove vehicles whose chassis is this body (a constraint referencing a
    // destroyed body leaves a dangling pointer in the constraint manager).
    std::vector<VehicleHandle> vehiclesToRemove;
    for (const auto& [handle, record] : impl_->vehicles_) {
        if (record.chassis == body) vehiclesToRemove.push_back(handle);
    }
    for (const VehicleHandle handle : vehiclesToRemove) destroy_vehicle(handle);

    // Jolt's DestroyBody REQUIRES the body to be inactive and out of the
    // broadphase first (BodyManager::RemoveBodyInternal asserts it; with
    // JPH_ENABLE_ASSERTS off the assert vanishes and the body is FREED while
    // still in the active list — the next Update() then dereferences a
    // dangling body in JobApplyGravity). RemoveBody must precede DestroyBody.
    JPH::BodyInterface& bodyInterface = impl_->physicsSystem->GetBodyInterface();
    bodyInterface.RemoveBody(it->second.id);
    bodyInterface.DestroyBody(it->second.id);
    impl_->bodyToHandle_.erase(it->second.id.GetIndexAndSequenceNumber());
    impl_->bodies_.erase(it);
    return true;
}

bool JoltPhysicsBackend::set_motion_type(BodyHandle body, MotionType motion,
                                         float mass) {
    const auto it = impl_->bodies_.find(body);
    if (it == impl_->bodies_.end()) return false;
    // Kinematic -> Dynamic flip (fracture/destruction debris): the Jolt body
    // must actually change motion type, or gravity/impulses are ignored and
    // the detached chunk floats in place (the engine-side mirror alone is
    // invisible to the backend). Activate so the newly-dynamic body wakes.
    JPH::BodyInterface& bodyInterface = impl_->physicsSystem->GetBodyInterface();
    bodyInterface.SetMotionType(it->second.id, motion_type(motion),
                                JPH::EActivation::Activate);
    if (motion == MotionType::Dynamic && mass > 0.0f) {
        // Body::SetMotionType does NOT restore mass: a kinematic body keeps
        // zero inverse mass, so gravity would produce no acceleration and the
        // debris would float forever. Scale the shape's mass properties to the
        // caller's mass (same normalization as create_body) and apply them
        // through the body lock interface (the sanctioned direct path).
        JPH::MassProperties massProperties =
            bodyInterface.GetShape(it->second.id)->GetMassProperties();
        if (massProperties.mMass > 1.0e-6f) {
            const float scale = mass / massProperties.mMass;
            massProperties.mMass = mass;
            massProperties.mInertia = massProperties.mInertia * scale;
        }
        JPH::BodyLockWrite lock(impl_->physicsSystem->GetBodyLockInterface(),
                                it->second.id);
        if (lock.Succeeded() && lock.GetBody().GetMotionProperties() != nullptr) {
            lock.GetBody().GetMotionProperties()->SetMassProperties(
                JPH::EAllowedDOFs::All, massProperties);
        }
    }
    return true;
}

void JoltPhysicsBackend::set_transform(BodyHandle body, const glm::vec3& position, const glm::quat& rotation) {
    const auto it = impl_->bodies_.find(body);
    if (it == impl_->bodies_.end()) return;
    impl_->physicsSystem->GetBodyInterface().SetPositionAndRotation(it->second.id, to_rjolt(position),
                                                                    to_jolt(glm::normalize(rotation)),
                                                                    JPH::EActivation::Activate);
}

bool JoltPhysicsBackend::get_state(BodyHandle body, glm::vec3& position, glm::quat& rotation,
                                   glm::vec3& linearVelocity, glm::vec3& angularVelocity, bool& sleeping) const {
    const auto it = impl_->bodies_.find(body);
    if (it == impl_->bodies_.end()) return false;
    const JPH::BodyInterface& bi = impl_->physicsSystem->GetBodyInterface();
    const JPH::BodyID id = it->second.id;
    position = to_glm(bi.GetPosition(id));
    rotation = to_glm(bi.GetRotation(id));
    linearVelocity = to_glm(bi.GetLinearVelocity(id));
    angularVelocity = to_glm(bi.GetAngularVelocity(id));
    sleeping = !bi.IsActive(id);
    return true;
}

void JoltPhysicsBackend::set_linear_velocity(BodyHandle body, const glm::vec3& velocity) {
    const auto it = impl_->bodies_.find(body);
    if (it == impl_->bodies_.end()) return;
    impl_->physicsSystem->GetBodyInterface().SetLinearVelocity(it->second.id, to_jolt(velocity));
}

void JoltPhysicsBackend::set_velocity(BodyHandle body, const glm::vec3& linearVelocity,
                                      const glm::vec3& angularVelocity) {
    const auto it = impl_->bodies_.find(body);
    if (it == impl_->bodies_.end()) return;
    impl_->physicsSystem->GetBodyInterface().SetLinearAndAngularVelocity(
        it->second.id, to_jolt(linearVelocity), to_jolt(angularVelocity));
}

void JoltPhysicsBackend::set_gravity_scale(BodyHandle body, float scale) {
    const auto it = impl_->bodies_.find(body);
    if (it == impl_->bodies_.end()) return;
    impl_->physicsSystem->GetBodyInterface().SetGravityFactor(it->second.id, scale);
}

void JoltPhysicsBackend::set_linear_damping(BodyHandle body, float damping) {
    const auto it = impl_->bodies_.find(body);
    if (it == impl_->bodies_.end()) return;
    JPH::BodyLockWrite lock(impl_->physicsSystem->GetBodyLockInterface(), it->second.id);
    if (lock.Succeeded()) lock.GetBody().GetMotionProperties()->SetLinearDamping(damping);
}

void JoltPhysicsBackend::add_force(BodyHandle body, const glm::vec3& force) {
    const auto it = impl_->bodies_.find(body);
    if (it == impl_->bodies_.end()) return;
    impl_->physicsSystem->GetBodyInterface().AddForce(it->second.id, to_jolt(force));
}

void JoltPhysicsBackend::apply_impulse(BodyHandle body, const glm::vec3& impulse) {
    const auto it = impl_->bodies_.find(body);
    if (it == impl_->bodies_.end()) return;
    impl_->physicsSystem->GetBodyInterface().AddImpulse(it->second.id, to_jolt(impulse));
}

void JoltPhysicsBackend::apply_impulse_at_point(BodyHandle body, const glm::vec3& impulse, const glm::vec3& worldPoint) {
    const auto it = impl_->bodies_.find(body);
    if (it == impl_->bodies_.end()) return;
    impl_->physicsSystem->GetBodyInterface().AddImpulse(it->second.id, to_jolt(impulse), to_rjolt(worldPoint));
}

void JoltPhysicsBackend::wake(BodyHandle body) {
    const auto it = impl_->bodies_.find(body);
    if (it == impl_->bodies_.end()) return;
    impl_->physicsSystem->GetBodyInterface().ActivateBody(it->second.id);
}

ConstraintHandle JoltPhysicsBackend::create_distance_constraint(const DistanceConstraintDesc& description) {
    const auto itA = impl_->bodies_.find(description.bodyA);
    const auto itB = impl_->bodies_.find(description.bodyB);
    if (itA == impl_->bodies_.end() || itB == impl_->bodies_.end()) return InvalidConstraint;

    JPH::BodyInterface& bi = impl_->physicsSystem->GetBodyInterface();
    const JPH::BodyLockWrite lockA(impl_->physicsSystem->GetBodyLockInterface(), itA->second.id);
    const JPH::BodyLockWrite lockB(impl_->physicsSystem->GetBodyLockInterface(), itB->second.id);
    if (!lockA.Succeeded() || !lockB.Succeeded()) return InvalidConstraint;

    const JPH::RVec3 anchorA = lockA.GetBody().GetCenterOfMassPosition() + lockA.GetBody().GetRotation() * to_jolt(description.localAnchorA);
    const JPH::RVec3 anchorB = lockB.GetBody().GetCenterOfMassPosition() + lockB.GetBody().GetRotation() * to_jolt(description.localAnchorB);

    auto settings = std::make_unique<JPH::DistanceConstraintSettings>();
    settings->mPoint1 = anchorA;
    settings->mPoint2 = anchorB;
    settings->mMinDistance = std::max(0.0f, description.restLength);
    settings->mMaxDistance = std::max(0.0f, description.restLength);

    JPH::Constraint* constraint = settings->Create(lockA.GetBody(), lockB.GetBody());
    if (!constraint) return InvalidConstraint;

    impl_->physicsSystem->AddConstraint(constraint);

    const ConstraintHandle handle = impl_->nextConstraintHandle_++;
    impl_->constraints_[handle] = {constraint, description.bodyA, description.bodyB};
    return handle;
}

ConstraintHandle JoltPhysicsBackend::create_swing_twist_constraint(const SwingTwistConstraintDesc& description) {
    const auto itA = impl_->bodies_.find(description.bodyA);
    const auto itB = impl_->bodies_.find(description.bodyB);
    if (itA == impl_->bodies_.end() || itB == impl_->bodies_.end()) return InvalidConstraint;

    JPH::BodyLockWrite lockA(impl_->physicsSystem->GetBodyLockInterface(), itA->second.id);
    JPH::BodyLockWrite lockB(impl_->physicsSystem->GetBodyLockInterface(), itB->second.id);
    if (!lockA.Succeeded() || !lockB.Succeeded()) return InvalidConstraint;

    auto settings = std::make_unique<JPH::SwingTwistConstraintSettings>();
    settings->mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
    settings->mPosition1 = to_jolt(description.localAnchorA);
    settings->mPosition2 = to_jolt(description.localAnchorB);
    settings->mTwistAxis1 = to_jolt(glm::normalize(description.twistAxisA));
    settings->mPlaneAxis1 = to_jolt(glm::normalize(description.planeAxisA));
    settings->mTwistAxis2 = to_jolt(glm::normalize(description.twistAxisB));
    settings->mPlaneAxis2 = to_jolt(glm::normalize(description.planeAxisB));
    settings->mNormalHalfConeAngle = glm::clamp(description.normalHalfConeAngle, 0.0f, glm::pi<float>());
    settings->mPlaneHalfConeAngle = glm::clamp(description.planeHalfConeAngle, 0.0f, glm::pi<float>());
    settings->mTwistMinAngle = glm::clamp(description.twistMinAngle, -glm::pi<float>(), 0.0f);
    settings->mTwistMaxAngle = glm::clamp(description.twistMaxAngle, 0.0f, glm::pi<float>());

    JPH::SwingTwistConstraint* constraint = nullptr;
    if (description.motorOn) {
        settings->mSwingMotorSettings.mSpringSettings =
            JPH::SpringSettings(JPH::ESpringMode::FrequencyAndDamping, description.motorFrequency, description.motorDamping);
        settings->mTwistMotorSettings.mSpringSettings =
            JPH::SpringSettings(JPH::ESpringMode::FrequencyAndDamping, description.motorFrequency, description.motorDamping);
        constraint = static_cast<JPH::SwingTwistConstraint*>(
            settings->Create(lockA.GetBody(), lockB.GetBody()));
        if (constraint == nullptr) return InvalidConstraint;
        constraint->SetSwingMotorState(JPH::EMotorState::Position);
        constraint->SetTwistMotorState(JPH::EMotorState::Position);
        // SetTargetOrientationBS: the desc's motorTarget is the RELATIVE
        // orientation of body B in body A's frame (R2 = R1 * motorTarget).
        // Passing it raw to SetTargetOrientationCS would conjugate it by the
        // constraint-frame rotation (c1^-1 * q * c2), silently rotating the
        // target into the twist axis for non-identity frames (findings #100).
        constraint->SetTargetOrientationBS(to_jolt(description.motorTarget));
    } else {
        constraint = static_cast<JPH::SwingTwistConstraint*>(
            settings->Create(lockA.GetBody(), lockB.GetBody()));
        if (constraint == nullptr) return InvalidConstraint;
    }

    impl_->physicsSystem->AddConstraint(constraint);

    const ConstraintHandle handle = impl_->nextConstraintHandle_++;
    impl_->constraints_[handle] = {constraint, description.bodyA, description.bodyB};
    return handle;
}

bool JoltPhysicsBackend::set_swing_twist_motor(ConstraintHandle constraint,
                                               bool motorOn, float frequency,
                                               float damping,
                                               const glm::quat& target) {
    const auto it = impl_->constraints_.find(constraint);
    if (it == impl_->constraints_.end()) return false;
    JPH::SwingTwistConstraint* joint =
        static_cast<JPH::SwingTwistConstraint*>(it->second.ptr);
    if (joint == nullptr) return false;
    const JPH::SpringSettings spring(
        JPH::ESpringMode::FrequencyAndDamping,
        glm::clamp(frequency, 0.01f, 1000.0f),
        glm::clamp(damping, 0.01f, 1000.0f));
    joint->GetSwingMotorSettings().mSpringSettings = spring;
    joint->GetTwistMotorSettings().mSpringSettings = spring;
    joint->SetSwingMotorState(motorOn ? JPH::EMotorState::Position
                                      : JPH::EMotorState::Off);
    joint->SetTwistMotorState(motorOn ? JPH::EMotorState::Position
                                      : JPH::EMotorState::Off);
    // Same convention as create: target is the relative orientation of body B
    // in body A's frame (see findings #100 — CS would rotate it by the frame).
    joint->SetTargetOrientationBS(to_jolt(target));
    return true;
}

bool JoltPhysicsBackend::destroy_constraint(ConstraintHandle constraint) {
    const auto it = impl_->constraints_.find(constraint);
    if (it == impl_->constraints_.end()) return false;
    impl_->physicsSystem->RemoveConstraint(it->second.ptr);
    impl_->constraints_.erase(it);
    return true;
}

VehicleHandle JoltPhysicsBackend::create_vehicle(const VehicleDesc& description) {
    const auto chassisIt = impl_->bodies_.find(description.chassis);
    if (chassisIt == impl_->bodies_.end() || description.wheels.empty()) return InvalidVehicle;
    // Tracked vehicles need two tracks (left/right).
    if (description.kind == VehicleKind::Tracked &&
        (description.tracks.empty() || description.tracks.size() != 2)) {
        return InvalidVehicle;
    }

    // VehicleConstraint needs the chassis Body& to bind the constraint.
    const JPH::BodyLockWrite lock(impl_->physicsSystem->GetBodyLockInterface(), chassisIt->second.id);
    if (!lock.Succeeded()) return InvalidVehicle;

    auto settings = std::make_unique<JPH::VehicleConstraintSettings>();
    settings->mUp = to_jolt(glm::normalize(description.up));
    settings->mForward = to_jolt(glm::normalize(description.forward));

    // Tracked wheels are friction-only (WheelSettingsTV); wheeled/motorcycle
    // wheels carry suspension + tire curves (WheelSettingsWV).
    const bool tracked = description.kind == VehicleKind::Tracked;
    for (std::size_t wi = 0; wi < description.wheels.size(); ++wi) {
        const WheelDesc& wheel = description.wheels[wi];
        if (tracked) {
            auto ws = new JPH::WheelSettingsTV();
            ws->mPosition = to_jolt(wheel.localPosition);
            ws->mRadius = std::max(0.05f, wheel.radius);
            ws->mWheelForward = to_jolt(glm::normalize(description.forward));
            // Suspension is handled by the track (the wheels ride the track);
            // keep a small fixed drop so the tester ray reaches the ground.
            ws->mSuspensionMinLength = 0.05f;
            ws->mSuspensionMaxLength = 0.05f;
            ws->mLongitudinalFriction = 2.0f;
            ws->mLateralFriction = 1.5f;
            settings->mWheels.push_back(ws);
            continue;
        }
        auto ws = new JPH::WheelSettingsWV();
        ws->mPosition = to_jolt(wheel.localPosition);
        // The tire pushes along the WHEEL's forward (default +Z), not the
        // constraint's mForward — set it to the engine forward (-Z) so the
        // vehicle drives the direction the engine's speed() reports.
        ws->mWheelForward = to_jolt(glm::normalize(description.forward));
        ws->mRadius = std::max(0.05f, wheel.radius);
        ws->mSuspensionMinLength = std::max(0.0f, wheel.suspensionRestLength - wheel.suspensionTravel);
        ws->mSuspensionMaxLength = wheel.suspensionRestLength + wheel.suspensionTravel;
        // The legacy raycast model's spring/damper coefficients map directly
        // to Jolt's stiffness/damping suspension spring.
        ws->mSuspensionSpring = JPH::SpringSettings(JPH::ESpringMode::StiffnessAndDamping,
                                                    std::max(0.0f, wheel.springStrength),
                                                    std::max(0.0f, wheel.damperStrength));
        ws->mMaxSteerAngle = wheel.steering
            ? glm::clamp(std::abs(wheel.maxSteerAngle), 0.0f, JPH::DegreesToRadians(75.0f))
            : 0.0f;
        const float brakeTorque = wheel.maxBrakeForce * ws->mRadius;
        ws->mMaxBrakeTorque = brakeTorque;
        ws->mMaxHandBrakeTorque = brakeTorque;
        // Flat-ish tire friction curves centered on the grip coefficient.
        ws->mLongitudinalFriction.AddPoint(0.0f, wheel.tireGrip);
        ws->mLongitudinalFriction.AddPoint(0.3f, wheel.tireGrip * 0.98f);
        ws->mLongitudinalFriction.AddPoint(1.0f, wheel.tireGrip * 0.9f);
        ws->mLateralFriction.AddPoint(0.0f, wheel.tireGrip);
        ws->mLateralFriction.AddPoint(30.0f, wheel.tireGrip * 0.8f);
        ws->mLateralFriction.AddPoint(90.0f, wheel.tireGrip * 0.5f);
        settings->mWheels.push_back(ws);
    }

    if (tracked) {
        // Tracked: one engine + transmission driving two tracks; each track
        // lists its wheels (the driven wheel is the one with max drive force).
        auto controller = std::make_unique<JPH::TrackedVehicleControllerSettings>();
        controller->mEngine.mMaxTorque = std::max(1.0f, description.engineMaxTorque);
        controller->mEngine.mMinRPM = std::max(1.0f, description.engineMinRPM);
        controller->mEngine.mMaxRPM = std::max(controller->mEngine.mMinRPM + 1.0f, description.engineMaxRPM);
        controller->mEngine.mNormalizedTorque.AddPoint(0.0f, 1.0f);
        controller->mEngine.mNormalizedTorque.AddPoint(0.25f, 0.95f);
        controller->mEngine.mNormalizedTorque.AddPoint(0.7f, 0.85f);
        controller->mEngine.mNormalizedTorque.AddPoint(1.0f, 0.65f);
        controller->mTransmission.mMode = JPH::ETransmissionMode::Auto;
        controller->mTransmission.mGearRatios.clear();
        for (const float ratio : description.gearRatios) {
            if (ratio > 0.0f) controller->mTransmission.mGearRatios.push_back(ratio);
        }
        if (controller->mTransmission.mGearRatios.empty()) {
            controller->mTransmission.mGearRatios.push_back(1.0f);
        }
        for (std::size_t side = 0; side < 2; ++side) {
            const VehicleDesc::TrackSide& track = description.tracks[side];
            if (track.wheelIndices.empty()) {
                return InvalidVehicle;
            }
            JPH::VehicleTrackSettings ts;
            ts.mWheels.clear();
            std::size_t driven = track.wheelIndices.front();
            float maxForce = -1.0f;
            for (const std::size_t idx : track.wheelIndices) {
                if (idx >= description.wheels.size()) return InvalidVehicle;
                ts.mWheels.push_back(static_cast<JPH::uint>(idx));
                if (description.wheels[idx].maxDriveForce > maxForce) {
                    maxForce = description.wheels[idx].maxDriveForce;
                    driven = idx;
                }
            }
            ts.mDrivenWheel = static_cast<JPH::uint>(driven);
            ts.mDifferentialRatio = std::max(0.01f, description.differentialRatio);
            ts.mInertia = 8.0f;
            ts.mAngularDamping = 0.5f;
            const float radius = std::max(0.05f, description.wheels[driven].radius);
            ts.mMaxBrakeTorque = std::max(1.0f, description.wheels[driven].maxBrakeForce) * radius;
            controller->mTracks[side] = ts;
        }
        settings->mController = controller.release();
    } else {
        // Wheeled / Motorcycle: engine + transmission + one differential per
        // driven axle (left/right pair), torque split evenly across axles.
        std::unique_ptr<JPH::WheeledVehicleControllerSettings> controller;
        if (description.kind == VehicleKind::Motorcycle) {
            auto moto = std::make_unique<JPH::MotorcycleControllerSettings>();
            moto->mMaxLeanAngle = JPH::DegreesToRadians(35.0f);
            moto->mLeanSpringConstant = 5000.0f;
            moto->mLeanSpringDamping = 1000.0f;
            controller = std::move(moto);
        } else {
            controller = std::make_unique<JPH::WheeledVehicleControllerSettings>();
        }
        controller->mEngine.mMaxTorque = std::max(1.0f, description.engineMaxTorque);
        controller->mEngine.mMinRPM = std::max(1.0f, description.engineMinRPM);
        controller->mEngine.mMaxRPM = std::max(controller->mEngine.mMinRPM + 1.0f, description.engineMaxRPM);
        controller->mEngine.mNormalizedTorque.AddPoint(0.0f, 1.0f);
        controller->mEngine.mNormalizedTorque.AddPoint(0.25f, 0.95f);
        controller->mEngine.mNormalizedTorque.AddPoint(0.7f, 0.85f);
        controller->mEngine.mNormalizedTorque.AddPoint(1.0f, 0.65f);
        controller->mTransmission.mMode = JPH::ETransmissionMode::Auto;
        controller->mTransmission.mGearRatios.clear();
        for (const float ratio : description.gearRatios) {
            if (ratio > 0.0f) controller->mTransmission.mGearRatios.push_back(ratio);
        }
        if (controller->mTransmission.mGearRatios.empty()) {
            controller->mTransmission.mGearRatios.push_back(1.0f);
        }

        std::vector<std::size_t> driven;
        for (std::size_t i = 0; i < description.wheels.size(); ++i) {
            if (description.wheels[i].driven) driven.push_back(i);
        }
        std::stable_sort(driven.begin(), driven.end(), [&](std::size_t a, std::size_t b) {
            const float za = description.wheels[a].localPosition.z;
            const float zb = description.wheels[b].localPosition.z;
            if (za != zb) return za < zb;
            return description.wheels[a].localPosition.x < description.wheels[b].localPosition.x;
        });
        std::size_t i = 0;
        while (i < driven.size()) {
            std::size_t j = i + 1;
            while (j < driven.size() &&
                   description.wheels[driven[j]].localPosition.z ==
                       description.wheels[driven[i]].localPosition.z) {
                ++j;
            }
            std::size_t left = driven[i], right = driven[i];
            for (std::size_t k = i; k < j; ++k) {
                if (description.wheels[driven[k]].localPosition.x <
                    description.wheels[left].localPosition.x) {
                    left = driven[k];
                }
                if (description.wheels[driven[k]].localPosition.x >
                    description.wheels[right].localPosition.x) {
                    right = driven[k];
                }
            }
            JPH::VehicleDifferentialSettings diff;
            diff.mLeftWheel = static_cast<int>(left);
            diff.mRightWheel = right == left ? -1 : static_cast<int>(right);
            diff.mDifferentialRatio = std::max(0.01f, description.differentialRatio);
            diff.mLeftRightSplit = 0.5f;
            controller->mDifferentials.push_back(diff);
            i = j;
        }
        const float perAxle = controller->mDifferentials.empty()
            ? 1.0f
            : 1.0f / static_cast<float>(controller->mDifferentials.size());
        for (JPH::VehicleDifferentialSettings& diff : controller->mDifferentials) {
            diff.mEngineTorqueRatio = perAxle;
        }
        settings->mController = controller.release();
    }

    JPH::VehicleConstraint* vehicle = new JPH::VehicleConstraint(lock.GetBody(), *settings);
    if (vehicle == nullptr) return InvalidVehicle;
    // Without a collision tester the constraint's wheel queries dereference a
    // null Ref in OnStep (the samples always install one). Layer 1 is the
    // engine wildcard combo ({layer 1, mask ~0}) that collides with everything
    // — the default drivable semantics (drivableLayers == ~0).
    vehicle->SetVehicleCollisionTester(new JPH::VehicleCollisionTesterRay(1));

    impl_->physicsSystem->AddStepListener(vehicle);
    impl_->physicsSystem->AddConstraint(vehicle);

    const VehicleHandle handle = impl_->nextVehicleHandle_++;
    impl_->vehicles_[handle] = {vehicle, description.chassis, description.wheels, description.kind};
    return handle;
}

bool JoltPhysicsBackend::destroy_vehicle(VehicleHandle vehicle) {
    const auto it = impl_->vehicles_.find(vehicle);
    if (it == impl_->vehicles_.end()) return false;
    impl_->physicsSystem->RemoveStepListener(it->second.constraint);
    impl_->physicsSystem->RemoveConstraint(it->second.constraint);
    impl_->vehicles_.erase(it);
    return true;
}

bool JoltPhysicsBackend::set_vehicle_input(VehicleHandle vehicle, const VehicleInput& input) {
    const auto it = impl_->vehicles_.find(vehicle);
    if (it == impl_->vehicles_.end()) return false;

    const float throttle = glm::clamp(input.throttle, -1.0f, 1.0f);
    const float steering = glm::clamp(input.steering, -1.0f, 1.0f);
    const float brake = glm::clamp(input.brake, 0.0f, 1.0f);
    const float handbrake = glm::clamp(input.handbrake, 0.0f, 1.0f);

    if (it->second.kind == VehicleKind::Tracked) {
        // Tank steering: the tracks turn at differential ratios. Full lock
        // (steering == +-1) spins one track forward and the other backward
        // (pivot); smaller steering drives one side faster than the other.
        auto* controller =
            static_cast<JPH::TrackedVehicleController*>(it->second.constraint->GetController());
        if (controller == nullptr) return false;
        const float left = 1.0f + steering;
        const float right = 1.0f - steering;
        // JPH asserts the ratios are non-zero (pivot uses opposite signs).
        controller->SetDriverInput(throttle,
                                   left == 0.0f ? 0.01f : left,
                                   right == 0.0f ? 0.01f : right,
                                   brake);
    } else {
        auto* controller =
            static_cast<JPH::WheeledVehicleController*>(it->second.constraint->GetController());
        if (controller == nullptr) return false;
        // Motorcycle reuses the wheeled input; the lean spring keeps the
        // chassis upright (no extra input needed).
        controller->SetDriverInput(throttle, steering, brake, handbrake);
    }

    // A settled chassis may be asleep (parked car); driver input must wake it
    // or the throttle/brake have no effect (the old raycast model woke on
    // every impulse).
    if (input.throttle != 0.0f || input.steering != 0.0f || input.brake != 0.0f ||
        input.handbrake != 0.0f) {
        const auto chassis = impl_->bodies_.find(it->second.chassis);
        if (chassis != impl_->bodies_.end()) {
            impl_->physicsSystem->GetBodyInterface().ActivateBody(chassis->second.id);
        }
    }
    return true;
}

bool JoltPhysicsBackend::vehicle_wheel_state(VehicleHandle vehicle, std::size_t index, WheelState& out) const {
    const auto it = impl_->vehicles_.find(vehicle);
    if (it == impl_->vehicles_.end() || index >= it->second.wheels.size()) return false;
    const JPH::Wheel* wheel = it->second.constraint->GetWheel(static_cast<JPH::uint>(index));
    if (wheel == nullptr) return false;
    out.grounded = wheel->HasContact();
    out.contactPoint = out.grounded ? to_glm(wheel->GetContactPosition()) : glm::vec3(0.0f);
    out.contactNormal = out.grounded ? to_glm(wheel->GetContactNormal()) : glm::vec3(0.0f, 1.0f, 0.0f);
    out.suspensionLength = wheel->GetSuspensionLength();
    const WheelDesc& desc = it->second.wheels[index];
    out.compression = (desc.suspensionRestLength + desc.suspensionTravel) - out.suspensionLength;
    out.rotation = wheel->GetRotationAngle();
    out.angularSpeed = wheel->GetAngularVelocity();
    out.steerAngle = wheel->GetSteerAngle();
    out.groundBody = out.grounded
        ? impl_->to_engine_handle(wheel->GetContactBodyID().GetIndexAndSequenceNumber())
        : InvalidBody;
    return true;
}

std::optional<RaycastHit> JoltPhysicsBackend::raycast(const glm::vec3& origin, const glm::vec3& direction,
                                                      float maxDistance, std::uint32_t layerMask,
                                                      BodyHandle ignoredBody) const {
    if (maxDistance <= 0.0f) return std::nullopt;
    const JPH::Vec3 dir = to_jolt(direction);
    if (dir.IsNearZero()) return std::nullopt;
    const JPH::RVec3 originR = to_rjolt(origin);

    JPH::RayCastSettings raySettings;
    EngineRayCollector collector(impl_->bodyToHandle_, impl_->bodies_, layerMask, ignoredBody);
    // Build a filter map from the body records (they hold the CollisionFilter).
    impl_->physicsSystem->GetNarrowPhaseQuery().CastRay(
        JPH::RRayCast(originR, dir * maxDistance), raySettings, collector);
    if (!collector.hit_) return std::nullopt;

    const float distance = collector.hit_->mFraction * maxDistance;
    const BodyHandle handle = impl_->to_engine_handle(collector.hit_->mBodyID.GetIndexAndSequenceNumber());
    if (handle == InvalidBody) return std::nullopt;
    return RaycastHit{handle, origin + glm::normalize(direction) * distance, glm::vec3(0.0f, 1.0f, 0.0f), distance};
}

std::vector<BodyHandle> JoltPhysicsBackend::overlap_aabb(const Aabb& bounds, std::uint32_t layerMask) const {
    std::vector<BodyHandle> result;
    JPH::AABox box(to_jolt(bounds.minimum), to_jolt(bounds.maximum));
    EngineBodyCollector collector;
    impl_->physicsSystem->GetBroadPhaseQuery().CollideAABox(box, collector);
    result.reserve(collector.ids.size());
    for (const JPH::BodyID& id : collector.ids) {
        const BodyHandle handle = impl_->to_engine_handle(id.GetIndexAndSequenceNumber());
        if (handle == InvalidBody) continue;
        const auto it = impl_->bodies_.find(handle);
        if (it != impl_->bodies_.end() && (it->second.filter.layer & layerMask) != 0u) result.push_back(handle);
    }
    return result;
}

std::vector<Contact> JoltPhysicsBackend::contacts() {
    std::vector<Contact> result;
    const std::vector<RawContact> rawContacts = impl_->contactListener->raw_contacts();
    result.reserve(rawContacts.size());
    for (const RawContact& raw : rawContacts) {
        const BodyHandle a = impl_->to_engine_handle(raw.body1);
        const BodyHandle b = impl_->to_engine_handle(raw.body2);
        if (a == InvalidBody || b == InvalidBody) continue;
        result.push_back(Contact{a, b, raw.point, raw.normal, raw.penetration, raw.restitution, raw.friction});
    }
    return result;
}

std::vector<TriggerEvent> JoltPhysicsBackend::trigger_events() {
    return std::move(impl_->triggerEvents_);
}

std::vector<DebugLine> JoltPhysicsBackend::debug_geometry() const {
    std::vector<DebugLine> lines;
    const JPH::BodyInterface& bi = impl_->physicsSystem->GetBodyInterface();
    for (const auto& [handle, record] : impl_->bodies_) {
        const glm::vec4 color = record.sensor ? glm::vec4(1.0f, 0.75f, 0.15f, 1.0f)
                                : record.filter.layer == 1u && record.filter.mask == ~0u ? glm::vec4(0.9f, 0.3f, 0.25f, 1.0f)
                                                                                        : glm::vec4(0.35f, 0.9f, 0.4f, 1.0f);
        const JPH::Shape* shape = bi.GetShape(record.id);
        if (!shape) continue;
        const JPH::AABox local = shape->GetLocalBounds();
        const JPH::RMat44 transform = bi.GetWorldTransform(record.id);        const JPH::Vec3 corners[8] = {
            local.mMin, JPH::Vec3(local.mMax.GetX(), local.mMin.GetY(), local.mMin.GetZ()),
            JPH::Vec3(local.mMax.GetX(), local.mMax.GetY(), local.mMin.GetZ()),
            JPH::Vec3(local.mMin.GetX(), local.mMax.GetY(), local.mMin.GetZ()),
            JPH::Vec3(local.mMin.GetX(), local.mMin.GetY(), local.mMax.GetZ()),
            JPH::Vec3(local.mMax.GetX(), local.mMin.GetY(), local.mMax.GetZ()),
            local.mMax, JPH::Vec3(local.mMin.GetX(), local.mMax.GetY(), local.mMax.GetZ())};
        glm::vec3 mn(1e30f), mx(-1e30f);
        for (const JPH::Vec3& corner : corners) {
            const glm::vec3 p = to_glm(transform * corner);
            mn = glm::min(mn, p);
            mx = glm::max(mx, p);
        }
        const glm::vec3 p[8] = {
            {mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}, {mx.x, mx.y, mn.z}, {mn.x, mx.y, mn.z},
            {mn.x, mn.y, mx.z}, {mx.x, mn.y, mx.z}, {mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z}};
        constexpr int edges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (const auto& edge : edges) lines.push_back({p[edge[0]], p[edge[1]], color});
    }
    return lines;
}

} // namespace Engine::Physics
