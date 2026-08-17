#include "engine/gameplay/DebrisRuntime.hpp"

#include "engine/sdk/RegistryJson.hpp"

#include <algorithm>
#include <cmath>

namespace Engine::Gameplay {

namespace {
float speed_squared(const glm::vec3& velocity) { return glm::dot(velocity, velocity); }
}

bool DebrisConfig::load_from_json(const std::string& json,
                                  std::string& errorOut) {
    engine::sdk::JsonValue document;
    if (!engine::sdk::json_parse(json, document, errorOut)) return false;
    if (!document.is_object()) {
        errorOut = "debris config must be a JSON object";
        return false;
    }
    // NOTE: the defaults must read the MEMBERS (this->...) — an unqualified
    // name inside its own initializer resolves to the local being declared
    // (uninitialized, MSVC C4700), which would make omitted JSON keys read
    // garbage.
    const double poolSize = engine::sdk::json_number(document, "poolSize", static_cast<double>(this->poolSize));
    const double cullRadius = engine::sdk::json_number(document, "cullRadius", static_cast<double>(this->cullRadius));
    const double persistRadius = engine::sdk::json_number(document, "persistRadius", static_cast<double>(this->persistRadius));
    const double restSpeed = engine::sdk::json_number(document, "restSpeed", static_cast<double>(this->restSpeed));
    const double minPersistMass = engine::sdk::json_number(document, "minPersistMass", static_cast<double>(this->minPersistMass));

    if (poolSize < 1.0 || poolSize > 1024.0) {
        errorOut = "debris config: poolSize must be in [1, 1024]";
        return false;
    }
    if (cullRadius <= 0.0) {
        errorOut = "debris config: cullRadius must be positive";
        return false;
    }
    if (persistRadius < 0.0) {
        errorOut = "debris config: persistRadius cannot be negative";
        return false;
    }
    if (restSpeed < 0.0) {
        errorOut = "debris config: restSpeed cannot be negative";
        return false;
    }
    if (minPersistMass < 0.0) {
        errorOut = "debris config: minPersistMass cannot be negative";
        return false;
    }

    this->poolSize = static_cast<std::size_t>(poolSize);
    this->cullRadius = static_cast<float>(cullRadius);
    this->persistRadius = static_cast<float>(persistRadius);
    this->restSpeed = static_cast<float>(restSpeed);
    this->minPersistMass = static_cast<float>(minPersistMass);
    return true;
}

bool RevoxelizePolicy::load_from_json(const std::string& json,
                                      std::string& errorOut) {
    engine::sdk::JsonValue document;
    if (!engine::sdk::json_parse(json, document, errorOut)) return false;
    if (!document.is_object()) {
        errorOut = "revoxelize policy must be a JSON object";
        return false;
    }
    // Same member-default discipline as DebrisConfig::load_from_json (see
    // the note there): `this->` so omitted keys default to the initialized
    // member, not the uninitialized local.
    const double settleDelay = engine::sdk::json_number(document, "settleDelay", static_cast<double>(this->settleDelay));
    const double materialFilter = engine::sdk::json_number(document, "materialFilter", static_cast<double>(this->materialFilter));
    const double maxBlocksPerDebris = engine::sdk::json_number(document, "maxBlocksPerDebris", static_cast<double>(this->maxBlocksPerDebris));
    const double maxDebrisPerPass = engine::sdk::json_number(document, "maxDebrisPerPass", static_cast<double>(this->maxDebrisPerPass));

    if (settleDelay < 0.0) {
        errorOut = "revoxelize policy: settleDelay cannot be negative";
        return false;
    }
    if (materialFilter < 0.0) {
        errorOut = "revoxelize policy: materialFilter cannot be negative";
        return false;
    }
    if (maxBlocksPerDebris < 1.0 || maxBlocksPerDebris > 4096.0) {
        errorOut = "revoxelize policy: maxBlocksPerDebris must be in [1, 4096]";
        return false;
    }
    if (maxDebrisPerPass < 1.0 || maxDebrisPerPass > 1024.0) {
        errorOut = "revoxelize policy: maxDebrisPerPass must be in [1, 1024]";
        return false;
    }

    this->settleDelay = static_cast<float>(settleDelay);
    this->materialFilter = static_cast<std::uint32_t>(materialFilter);
    this->maxBlocksPerDebris = static_cast<int>(maxBlocksPerDebris);
    this->maxDebrisPerPass = static_cast<int>(maxDebrisPerPass);
    enabled = engine::sdk::json_bool(document, "enabled", enabled);
    removeAfter = engine::sdk::json_bool(document, "removeAfter", removeAfter);
    return true;
}

DebrisRuntime::DebrisRuntime(Physics::PhysicsRuntime& physics,
                             DebrisConfig config)
    : physics_(physics), config_(config) {}

DebrisRuntime::Debris* DebrisRuntime::find(Physics::BodyHandle body) {
    const auto it = std::find_if(active_.begin(), active_.end(),
                                 [body](const Debris& d) { return d.body == body; });
    return it != active_.end() ? &*it : nullptr;
}

const DebrisRuntime::Debris* DebrisRuntime::find(Physics::BodyHandle body) const {
    const auto it = std::find_if(active_.begin(), active_.end(),
                                 [body](const Debris& d) { return d.body == body; });
    return it != active_.end() ? &*it : nullptr;
}

void DebrisRuntime::emit(Physics::BodyHandle body,
                         DebrisReplicationEvent::Type type) {
    const Debris* debris = find(body);
    if (debris == nullptr) return;
    DebrisReplicationEvent event;
    event.type = type;
    event.id = debris->id;
    event.position = debris->position;
    event.rotation = debris->rotation;
    event.linearVelocity = glm::vec3(0.0f);
    event.mass = debris->mass;
    event.materialIndex = debris->materialIndex;
    if (const Physics::RigidBody* rb = physics_.body(body)) {
        event.position = rb->position;
        event.rotation = rb->rotation;
        event.linearVelocity = rb->linearVelocity;
    }
    events_.push_back(event);
}

Physics::BodyHandle DebrisRuntime::spawn(const Physics::BodyDesc& description,
                                         std::uint32_t materialIndex) {
    Physics::BodyHandle body = Physics::InvalidBody;
    if (!parked_.empty()) {
        // Pool reuse: re-arm the parked body (kinematic -> dynamic with mass,
        // move to the spawn spot, restore the spawn velocity via impulse).
        body = parked_.back();
        parked_.pop_back();
        physics_.set_motion(body, Physics::MotionType::Dynamic, description.mass);
        physics_.set_transform(body, description.position, description.rotation);
        physics_.apply_impulse(body, description.mass * description.linearVelocity);
        physics_.wake(body);
        ++reusedCount_;
    } else {
        body = physics_.create_body(description);
        if (body == Physics::InvalidBody) return body;
        ++spawnedTotal_;
    }
    Debris debris;
    debris.body = body;
    debris.id = nextId_++;
    debris.position = description.position;
    debris.rotation = description.rotation;
    debris.shape = description.collider.shape;
    debris.mass = description.mass;
    debris.materialIndex = materialIndex;
    active_.push_back(std::move(debris));
    emit(body, DebrisReplicationEvent::Type::Spawned);
    return body;
}

void DebrisRuntime::park(Physics::BodyHandle body) {
    // Kinematic + far corner: the parked body no longer simulates and cannot
    // interact with gameplay (nothing lives at the pool corner).
    physics_.set_motion(body, Physics::MotionType::Kinematic, 1.0f);
    physics_.set_transform(body, config_.poolCorner, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
}

bool DebrisRuntime::despawn(Physics::BodyHandle body) {
    const auto it = std::find_if(active_.begin(), active_.end(),
                                 [body](const Debris& d) { return d.body == body; });
    if (it == active_.end()) return false;
    const std::uint64_t id = it->id;
    active_.erase(it);
    if (parked_.size() < config_.poolSize) {
        park(body);
        parked_.push_back(body);
    } else {
        physics_.destroy_body(body);
    }
    DebrisReplicationEvent event;
    event.type = DebrisReplicationEvent::Type::Despawned;
    event.id = id;
    events_.push_back(event);
    return true;
}

void DebrisRuntime::update(const glm::vec3& focus, float deltaTime) {
    // 1. Re-entry: culled debris back within the cull radius re-enter the
    //    simulation (from the pool) at their last known transform.
    std::vector<Debris> reentered;
    for (const Debris& record : culled_) {
        const glm::vec3 toFocus = record.position - focus;
        if (glm::dot(toFocus, toFocus) > config_.cullRadius * config_.cullRadius)
            continue;
        Physics::BodyDesc description;
        description.motion = Physics::MotionType::Dynamic;
        description.position = record.position;
        description.rotation = record.rotation;
        description.mass = record.mass;
        description.collider.shape = record.shape;
        Physics::BodyHandle body = Physics::InvalidBody;
        if (!parked_.empty()) {
            body = parked_.back();
            parked_.pop_back();
            physics_.set_motion(body, Physics::MotionType::Dynamic, description.mass);
            physics_.set_transform(body, description.position, description.rotation);
            physics_.wake(body);
            ++reusedCount_;
        } else {
            body = physics_.create_body(description);
            if (body != Physics::InvalidBody) ++spawnedTotal_;
        }
        if (body == Physics::InvalidBody) continue;
        Debris debris = record;
        debris.body = body;
        reentered.push_back(std::move(debris));
        ++cullReenter_;
    }
    culled_.erase(std::remove_if(culled_.begin(), culled_.end(),
                                 [&](const Debris& record) {
                                     const glm::vec3 toFocus = record.position - focus;
                                     return glm::dot(toFocus, toFocus) <=
                                            config_.cullRadius * config_.cullRadius;
                                 }),
                  culled_.end());
    for (Debris& debris : reentered) active_.push_back(std::move(debris));
    // Emit AFTER the push: emit() resolves the debris through the active list.
    for (const Debris& debris : reentered)
        emit(debris.body, DebrisReplicationEvent::Type::Spawned);

    // 2. LOD cull + resting tracking on the live debris.
    const float cullSq = config_.cullRadius * config_.cullRadius;
    const float restSq = config_.restSpeed * config_.restSpeed;
    for (auto it = active_.begin(); it != active_.end();) {
        Debris& debris = *it;
        Physics::RigidBody* rb = physics_.body(debris.body);
        if (rb == nullptr) {  // externally destroyed: drop the record
            it = active_.erase(it);
            continue;
        }
        debris.position = rb->position;
        debris.rotation = rb->rotation;
        const glm::vec3 toFocus = debris.position - focus;
        if (glm::dot(toFocus, toFocus) > cullSq) {
            culled_.push_back(debris);
            physics_.destroy_body(debris.body);
            ++cullEnter_;
            emit(debris.body, DebrisReplicationEvent::Type::Despawned);
            it = active_.erase(it);
            continue;
        }
        const bool resting = rb->sleeping || speed_squared(rb->linearVelocity) <= restSq;
        debris.resting = resting;
        if (resting) {
            debris.restTimer += deltaTime;
            if (!debris.settled) {
                debris.settled = true;
                emit(debris.body, DebrisReplicationEvent::Type::Settled);
            }
        } else {
            debris.restTimer = 0.0f;
            debris.settled = false;
        }
        ++it;
    }
}

std::vector<DebrisPersistRecord> DebrisRuntime::snapshot_persistable(
    const glm::vec3& focus) const {
    std::vector<DebrisPersistRecord> records;
    const float persistSq = config_.persistRadius * config_.persistRadius;
    for (const Debris& debris : active_) {
        if (!debris.resting) continue;
        if (debris.mass < config_.minPersistMass) continue;
        const glm::vec3 toFocus = debris.position - focus;
        if (glm::dot(toFocus, toFocus) > persistSq) continue;
        DebrisPersistRecord record;
        record.id = debris.id;
        record.position = debris.position;
        record.rotation = debris.rotation;
        record.shape = debris.shape;
        record.mass = debris.mass;
        record.materialIndex = debris.materialIndex;
        record.sleeping = true;
        records.push_back(record);
    }
    std::sort(records.begin(), records.end(),
              [](const DebrisPersistRecord& a, const DebrisPersistRecord& b) {
                  return a.id < b.id;
              });
    return records;
}

std::vector<DebrisPersistRecord> DebrisRuntime::debris_in_box(
    const glm::vec3& minimum, const glm::vec3& maximum) const {
    std::vector<DebrisPersistRecord> records;
    for (const Debris& debris : active_) {
        if (debris.position.x < minimum.x || debris.position.x > maximum.x ||
            debris.position.y < minimum.y || debris.position.y > maximum.y ||
            debris.position.z < minimum.z || debris.position.z > maximum.z) {
            continue;
        }
        DebrisPersistRecord record;
        record.id = debris.id;
        record.position = debris.position;
        record.rotation = debris.rotation;
        record.shape = debris.shape;
        record.mass = debris.mass;
        record.materialIndex = debris.materialIndex;
        record.sleeping = debris.resting;
        records.push_back(record);
    }
    return records;
}

std::size_t DebrisRuntime::despawn_in_box(const glm::vec3& minimum,
                                          const glm::vec3& maximum) {
    std::size_t despawned = 0;
    for (auto it = active_.begin(); it != active_.end();) {
        const Debris& debris = *it;
        if (debris.position.x >= minimum.x && debris.position.x <= maximum.x &&
            debris.position.y >= minimum.y && debris.position.y <= maximum.y &&
            debris.position.z >= minimum.z && debris.position.z <= maximum.z) {
            const Physics::BodyHandle body = it->body;
            const std::uint64_t id = it->id;
            it = active_.erase(it);
            if (parked_.size() < config_.poolSize) {
                park(body);
                parked_.push_back(body);
            } else {
                physics_.destroy_body(body);
            }
            DebrisReplicationEvent event;
            event.type = DebrisReplicationEvent::Type::Despawned;
            event.id = id;
            events_.push_back(event);
            ++despawned;
        } else {
            ++it;
        }
    }
    return despawned;
}

std::size_t DebrisRuntime::restore(const std::vector<DebrisPersistRecord>& records) {
    std::size_t restored = 0;
    for (const DebrisPersistRecord& record : records) {
        Physics::BodyDesc description;
        description.motion = Physics::MotionType::Dynamic;
        description.position = record.position;
        description.rotation = record.rotation;
        description.mass = record.mass;
        description.collider.shape = record.shape;

        Physics::BodyHandle body = Physics::InvalidBody;
        if (!parked_.empty()) {
            body = parked_.back();
            parked_.pop_back();
            physics_.set_motion(body, Physics::MotionType::Dynamic, description.mass);
            physics_.set_transform(body, description.position, description.rotation);
            physics_.wake(body);
            ++reusedCount_;
        } else {
            body = physics_.create_body(description);
            if (body != Physics::InvalidBody) ++spawnedTotal_;
        }
        if (body == Physics::InvalidBody) continue;
        Debris debris;
        debris.body = body;
        debris.id = record.id;  // identity survives the round-trip
        debris.position = record.position;
        debris.rotation = record.rotation;
        debris.mass = record.mass;
        debris.materialIndex = record.materialIndex;
        debris.resting = true;
        debris.settled = true;
        active_.push_back(std::move(debris));
        emit(body, DebrisReplicationEvent::Type::Spawned);
        ++restored;
    }
    return restored;
}

std::vector<DebrisReplicationEvent> DebrisRuntime::drain_replication_events() {
    std::vector<DebrisReplicationEvent> out = std::move(events_);
    events_.clear();
    return out;
}

namespace {

// Voxel footprint of a debris shape at a resting position: every cell whose
// CENTER lies inside the shape, in fixed (y,z,x) scan order. `callback` is
// invoked once per cell and may return false to stop (budget cap).
template <typename Callback>
void for_each_footprint_cell(const Physics::ColliderShape& shape,
                             const glm::vec3& position, Callback&& callback) {
    glm::vec3 minimum = position;
    glm::vec3 maximum = position;
    if (const auto* box = std::get_if<Physics::BoxShape>(&shape)) {
        minimum -= box->halfExtents;
        maximum += box->halfExtents;
    } else if (const auto* sphere = std::get_if<Physics::SphereShape>(&shape)) {
        const glm::vec3 r(sphere->radius);
        minimum -= r;
        maximum += r;
    } else if (const auto* capsule = std::get_if<Physics::CapsuleShape>(&shape)) {
        const glm::vec3 r(capsule->radius);
        const glm::vec3 top(0.0f, capsule->halfHeight, 0.0f);
        minimum -= r + top;
        maximum += r + top;
    }
    const auto inside = [&](const glm::vec3& center) {
        if (const auto* box = std::get_if<Physics::BoxShape>(&shape)) {
            return center.x >= position.x - box->halfExtents.x &&
                   center.x <= position.x + box->halfExtents.x &&
                   center.y >= position.y - box->halfExtents.y &&
                   center.y <= position.y + box->halfExtents.y &&
                   center.z >= position.z - box->halfExtents.z &&
                   center.z <= position.z + box->halfExtents.z;
        }
        if (const auto* sphere = std::get_if<Physics::SphereShape>(&shape)) {
            return glm::dot(center - position, center - position) <=
                   sphere->radius * sphere->radius;
        }
        if (const auto* capsule = std::get_if<Physics::CapsuleShape>(&shape)) {
            const glm::vec3 top(0.0f, capsule->halfHeight, 0.0f);
            const float y = std::clamp(center.y - position.y,
                                       -capsule->halfHeight, capsule->halfHeight);
            const glm::vec3 closest = position + glm::vec3(0.0f, y, 0.0f);
            return glm::dot(center - closest, center - closest) <=
                   capsule->radius * capsule->radius;
        }
        return false;
    };
    const int minX = static_cast<int>(std::floor(minimum.x));
    const int maxX = static_cast<int>(std::floor(maximum.x));
    const int minY = static_cast<int>(std::floor(minimum.y));
    const int maxY = static_cast<int>(std::floor(maximum.y));
    const int minZ = static_cast<int>(std::floor(minimum.z));
    const int maxZ = static_cast<int>(std::floor(maximum.z));
    for (int y = minY; y <= maxY; ++y) {
        for (int z = minZ; z <= maxZ; ++z) {
            for (int x = minX; x <= maxX; ++x) {
                const glm::vec3 center(x + 0.5f, y + 0.5f, z + 0.5f);
                if (!inside(center)) continue;
                if (!callback(x, y, z)) return;
            }
        }
    }
}

}  // namespace

RevoxelizeResult DebrisRuntime::revoxelize_sleeping(
    engine::voxel::IVoxelWorld& world, const RevoxelizePolicy& policy,
    const std::function<std::uint32_t(std::uint32_t)>& blockOf) {
    RevoxelizeResult result;
    if (!policy.enabled || !blockOf) return result;

    std::size_t pass = 0;
    for (auto it = active_.begin(); it != active_.end() && pass < static_cast<std::size_t>(policy.maxDebrisPerPass);) {
        Debris& debris = *it;
        Physics::RigidBody* rb = physics_.body(debris.body);
        // "Sleeping" is the POLICY's settle delay: the debris must have been
        // resting (below restSpeed) for at least settleDelay seconds. The Jolt
        // solver's own sleep flag is a solver artifact (it lags sustained rest
        // behind its own wake threshold and is engine-config dependent), so it
        // is NOT required here — restTimer is the deterministic sleep
        // criterion.
        const bool settled = debris.resting && debris.restTimer >= policy.settleDelay;
        const bool materialOk = policy.materialFilter == 0u ||
                                debris.materialIndex == policy.materialFilter;
        if (!settled || !materialOk) {
            ++it;
            continue;
        }
        const std::uint32_t blockId = blockOf(debris.materialIndex);
        if (blockId == 0u) {  // no terrain for this material: keep the debris
            ++result.debrisSkipped;
            ++it;
            continue;
        }

        std::size_t written = 0;
        glm::vec3 resting = debris.position;
        if (rb != nullptr) resting = rb->position;
        for_each_footprint_cell(debris.shape, resting,
                                [&](int x, int y, int z) {
                                    if (written >= static_cast<std::size_t>(policy.maxBlocksPerDebris))
                                        return false;  // budget cap per debris
                                    if (world.get_block(x, y, z) == 0u) {
                                        world.set_block(x, y, z, blockId);
                                        ++written;
                                    }
                                    return true;
                                });
        result.blocksWritten += written;
        ++result.debrisRevoxelized;
        ++pass;
        if (policy.removeAfter) {
            const Physics::BodyHandle body = debris.body;
            const std::uint64_t id = debris.id;
            it = active_.erase(it);
            if (parked_.size() < config_.poolSize) {
                park(body);
                parked_.push_back(body);
            } else {
                physics_.destroy_body(body);
            }
            DebrisReplicationEvent event;
            event.type = DebrisReplicationEvent::Type::Despawned;
            event.id = id;
            events_.push_back(event);
        } else {
            ++it;
        }
    }
    return result;
}

}  // namespace Engine::Gameplay
