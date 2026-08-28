#include "engine/networking/NetworkRuntime.hpp"
#include <algorithm>

namespace Engine::Networking {

ConnectionId ConnectionRegistry::add(std::string endpoint, double now) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Skip ID 0 (reserved as invalid) and avoid reusing active IDs on wrap-around.
    ConnectionId id{++nextId_};
    while (id.value == 0 || connections_.count(id) > 0) {
        id = ConnectionId{++nextId_};
    }
    connections_[id] = ConnectionInfo{id, ConnectionStatus::Connecting, std::move(endpoint), now, 0};
    return id;
}

bool ConnectionRegistry::remove(ConnectionId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return connections_.erase(id) > 0;
}

bool ConnectionRegistry::set_status(ConnectionId id, ConnectionStatus status) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = connections_.find(id); it != connections_.end()) {
        it->second.status = status;
        return true;
    }
    return false;
}

bool ConnectionRegistry::touch(ConnectionId id, double now, std::uint32_t roundTripMilliseconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = connections_.find(id); it != connections_.end()) {
        it->second.lastReceiveTime = now;
        it->second.roundTripMilliseconds = roundTripMilliseconds;
        return true;
    }
    return false;
}

std::optional<ConnectionInfo> ConnectionRegistry::get(ConnectionId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = connections_.find(id); it != connections_.end()) return it->second;
    return std::nullopt;
}

std::vector<ConnectionInfo> ConnectionRegistry::all() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ConnectionInfo> res;
    res.reserve(connections_.size());
    for (const auto& [id, info] : connections_) res.push_back(info);
    return res;
}

bool OwnershipRegistry::assign(NetEntityId entity, ConnectionId owner) {
    std::lock_guard<std::mutex> lock(mutex_);
    owners_[entity] = owner;
    return true;
}

bool OwnershipRegistry::release(NetEntityId entity, ConnectionId expectedOwner) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = owners_.find(entity);
    if (it != owners_.end()) {
        if (!expectedOwner.valid() || it->second == expectedOwner) {
            owners_.erase(it);
            return true;
        }
    }
    return false;
}

void OwnershipRegistry::release_all(ConnectionId owner) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = owners_.begin(); it != owners_.end(); ) {
        if (it->second == owner) it = owners_.erase(it);
        else ++it;
    }
}

ConnectionId OwnershipRegistry::owner_of(NetEntityId entity) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = owners_.find(entity);
    return it != owners_.end() ? it->second : ConnectionId{};
}

bool OwnershipRegistry::is_owner(ConnectionId connection, NetEntityId entity) const {
    return owner_of(entity) == connection;
}

bool ReplicatedProperties::set_bytes(NetEntityId entity, PropertyId property, ByteBuffer value) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& ent = entities_[entity];
    ent.revision++;
    ent.properties[property] = Value{std::move(value), ent.revision};
    return true;
}

std::optional<ByteBuffer> ReplicatedProperties::get_bytes(NetEntityId entity, PropertyId property) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entities_.find(entity);
    if (it != entities_.end()) {
        auto pit = it->second.properties.find(property);
        if (pit != it->second.properties.end()) return pit->second.bytes;
    }
    return std::nullopt;
}

std::vector<PropertyDelta> ReplicatedProperties::collect_since(
    const std::unordered_map<NetEntityId, std::uint32_t, NetEntityIdHash>& acknowledged,
    std::span<const NetEntityId> relevantEntities) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PropertyDelta> deltas;
    for (auto ent : relevantEntities) {
        auto it = entities_.find(ent);
        if (it == entities_.end()) continue;
        
        std::uint32_t ackRev = 0;
        auto ackIt = acknowledged.find(ent);
        if (ackIt != acknowledged.end()) ackRev = ackIt->second;
        
        if (it->second.revision > ackRev) {
            for (const auto& [propId, val] : it->second.properties) {
                if (val.revision > ackRev) {
                    deltas.push_back(PropertyDelta{ent, propId, val.revision, val.bytes});
                }
            }
        }
    }
    return deltas;
}

void ReplicatedProperties::apply(std::span<const PropertyDelta> deltas) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& delta : deltas) {
        auto& ent = entities_[delta.entity];
        auto& prop = ent.properties[delta.property];
        if (delta.revision > prop.revision) {
            prop.bytes = delta.value;
            prop.revision = delta.revision;
            ent.revision = std::max(ent.revision, delta.revision);
        }
    }
}

bool ReplicatedProperties::erase_entity(NetEntityId entity) {
    std::lock_guard<std::mutex> lock(mutex_);
    return entities_.erase(entity) > 0;
}

std::uint32_t ReplicatedProperties::entity_revision(NetEntityId entity) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entities_.find(entity);
    return it != entities_.end() ? it->second.revision : 0;
}

SnapshotBuffer::SnapshotBuffer(std::size_t capacity) : capacity_(capacity) {}

void SnapshotBuffer::push(Snapshot snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = std::lower_bound(snapshots_.begin(), snapshots_.end(), snapshot,
        [](const Snapshot& a, const Snapshot& b) { return a.tick < b.tick; });
        
    if (it != snapshots_.end() && it->tick == snapshot.tick) {
        *it = std::move(snapshot);
    } else {
        snapshots_.insert(it, std::move(snapshot));
    }
    
    while (snapshots_.size() > capacity_) {
        snapshots_.pop_front();
    }
}

std::optional<Snapshot> SnapshotBuffer::latest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshots_.empty()) return std::nullopt;
    return snapshots_.back();
}

void SnapshotBuffer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshots_.clear();
}

std::vector<InterpolatedEntity> SnapshotBuffer::sample(double renderTime, double maxExtrapolation) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<InterpolatedEntity> result;
    if (snapshots_.empty()) return result;
    
    // Find surrounding snapshots
    const Snapshot* older = nullptr;
    const Snapshot* newer = nullptr;
    
    for (auto it = snapshots_.rbegin(); it != snapshots_.rend(); ++it) {
        if (it->serverTime <= renderTime) {
            older = &(*it);
            if (it != snapshots_.rbegin()) {
                newer = &(*(it - 1));
            }
            break;
        }
    }
    
    if (!older) {
        older = &snapshots_.front();
        newer = older;
    }
    if (!newer) newer = older;
    
    float alpha = 0.0f;
    if (newer->serverTime > older->serverTime) {
        alpha = static_cast<float>((renderTime - older->serverTime) / (newer->serverTime - older->serverTime));
    }
    
    // Interpolate entities
    for (const auto& entOld : older->entities) {
        InterpolatedEntity interp;
        interp.entity = entOld.entity;
        interp.olderTick = older->tick;
        interp.newerTick = newer->tick;
        interp.alpha = alpha;
        interp.transform = entOld.transform;
        
        if (older != newer) {
            auto itNew = std::find_if(newer->entities.begin(), newer->entities.end(),
                [&](const EntitySnapshot& e) { return e.entity == entOld.entity; });
            if (itNew != newer->entities.end()) {
                interp.transform.position = glm::mix(entOld.transform.position, itNew->transform.position, alpha);
                interp.transform.rotation = glm::slerp(entOld.transform.rotation, itNew->transform.rotation, alpha);
                interp.transform.linearVelocity = glm::mix(entOld.transform.linearVelocity, itNew->transform.linearVelocity, alpha);
            } else if (renderTime > older->serverTime) { // Extrapolate briefly
                float dt = static_cast<float>(renderTime - older->serverTime);
                if (dt <= maxExtrapolation) {
                    interp.transform.position += entOld.transform.linearVelocity * dt;
                }
            }
        }
        result.push_back(interp);
    }
    
    return result;
}

void RelevancySystem::upsert(RelevancyRecord record) {
    std::lock_guard<std::mutex> lock(mutex_);
    records_[record.entity] = std::move(record);
}

bool RelevancySystem::remove(NetEntityId entity) {
    std::lock_guard<std::mutex> lock(mutex_);
    return records_.erase(entity) > 0;
}

std::vector<NetEntityId> RelevancySystem::query(ConnectionId viewer, const glm::vec3& viewpoint, float viewDistance, const OwnershipRegistry& ownership) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<NetEntityId> result;
    for (const auto& [id, record] : records_) {
        bool relevant = false;
        if (record.mode == RelevancyMode::Always) relevant = true;
        else if (record.mode == RelevancyMode::OwnerOnly) relevant = ownership.is_owner(viewer, id);
        else if (record.mode == RelevancyMode::Distance) {
            relevant = glm::distance(record.position, viewpoint) <= (viewDistance + record.radius);
        }
        if (relevant) result.push_back(id);
    }
    return result;
}

RpcQueue::RpcQueue(std::size_t capacity) : capacity_(capacity) {}

bool RpcQueue::enqueue(std::deque<RpcMessage>& queue, RpcMessage message, bool assignSequence) {
    if (queue.size() >= capacity_) {
        dropped_++;
        return false;
    }
    if (assignSequence) message.sequence = nextSequence_++;
    queue.push_back(std::move(message));
    return true;
}

bool RpcQueue::enqueue_outgoing(RpcMessage message) {
    std::lock_guard<std::mutex> lock(mutex_);
    return enqueue(outgoing_, std::move(message), true);
}

bool RpcQueue::enqueue_incoming(RpcMessage message) {
    std::lock_guard<std::mutex> lock(mutex_);
    return enqueue(incoming_, std::move(message), false);
}

std::vector<RpcMessage> RpcQueue::drain_outgoing(ConnectionId destination, std::size_t maximum) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RpcMessage> res;
    for (auto it = outgoing_.begin(); it != outgoing_.end() && res.size() < maximum;) {
        if (!destination.valid() || it->destination == destination) {
            res.push_back(std::move(*it));
            it = outgoing_.erase(it);
        } else {
            ++it;
        }
    }
    return res;
}

std::vector<RpcMessage> RpcQueue::drain_incoming(std::size_t maximum) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RpcMessage> res;
    while (!incoming_.empty() && res.size() < maximum) {
        res.push_back(std::move(incoming_.front()));
        incoming_.pop_front();
    }
    return res;
}

std::size_t RpcQueue::dropped() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
}

ClientPredictor::ClientPredictor(PredictionStep step, float correctionThreshold)
    : step_(std::move(step)), threshold_(correctionThreshold) {}

PredictionInput ClientPredictor::predict(float deltaTime, const glm::vec3& movement, ByteBuffer userData) {
    PredictionInput input{nextSequence_++, deltaTime, movement, std::move(userData)};
    pending_.push_back(input);
    step_(state_, input);
    return input;
}

ReconciliationResult ClientPredictor::reconcile(const PredictedState& authoritative, std::uint32_t acknowledgedInput) {
    ReconciliationResult result{};
    
    // Remove acknowledged inputs
    while (!pending_.empty() && pending_.front().sequence <= acknowledgedInput) {
        pending_.pop_front();
    }
    
    // Check error
    float err = glm::distance(state_.position, authoritative.position);
    result.positionError = err;
    
    if (err > threshold_) {
        result.corrected = true;
        result.visualCorrection = state_.position - authoritative.position; // For smooth visual catch-up
        visualError_ = result.visualCorrection;
        
        state_ = authoritative;
        for (const auto& input : pending_) {
            step_(state_, input);
            result.replayedInputs++;
        }
    }
    return result;
}

void ClientPredictor::set_state(PredictedState state) {
    state_ = std::move(state);
    pending_.clear();
    visualError_ = glm::vec3(0.0f);
}

PredictedState ClientPredictor::visual_state(float deltaTime, float sharpness) {
    if (glm::length(visualError_) > 0.001f) {
        visualError_ = glm::mix(visualError_, glm::vec3(0.0f), std::min(1.0f, deltaTime * sharpness));
    }
    PredictedState vis = state_;
    vis.position -= visualError_;
    return vis;
}

void NetworkingRuntime::publish(NetEntityId entity, NetworkTransform transform) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    states_[entity] = transform;
}

bool NetworkingRuntime::unpublish(NetEntityId entity) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return states_.erase(entity) > 0;
}

Snapshot NetworkingRuntime::make_snapshot(Tick tick, double serverTime,
                                         std::span<const NetEntityId> relevantEntities,
                                         const std::unordered_map<NetEntityId, std::uint32_t, NetEntityIdHash>& acknowledgedPropertyRevisions) const {
    Snapshot snap;
    snap.tick = tick;
    snap.serverTime = serverTime;
    
    auto deltas = properties_.collect_since(acknowledgedPropertyRevisions, relevantEntities);
    std::unordered_map<NetEntityId, std::vector<PropertyDelta>, NetEntityIdHash> mappedDeltas;
    for (auto& d : deltas) mappedDeltas[d.entity].push_back(std::move(d));
    
    std::lock_guard<std::mutex> lock(stateMutex_);
    for (auto ent : relevantEntities) {
        auto it = states_.find(ent);
        if (it != states_.end()) {
            EntitySnapshot es;
            es.entity = ent;
            es.transform = it->second;
            es.properties = std::move(mappedDeltas[ent]);
            snap.entities.push_back(std::move(es));
        }
    }
    return snap;
}

} // namespace Engine::Networking
