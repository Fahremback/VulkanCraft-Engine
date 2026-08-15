#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Engine::Networking {

using Tick = std::uint32_t;
using PropertyId = std::uint32_t;
using RpcId = std::uint32_t;

struct ConnectionId {
    std::uint32_t value{};
    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
    auto operator<=>(const ConnectionId&) const = default;
};

struct NetEntityId {
    std::uint64_t value{};
    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
    auto operator<=>(const NetEntityId&) const = default;
};

struct ConnectionIdHash {
    std::size_t operator()(ConnectionId id) const noexcept { return std::hash<std::uint32_t>{}(id.value); }
};
struct NetEntityIdHash {
    std::size_t operator()(NetEntityId id) const noexcept { return std::hash<std::uint64_t>{}(id.value); }
};

enum class ConnectionStatus { Connecting, Connected, Disconnecting, Disconnected };

struct ConnectionInfo {
    ConnectionId id;
    ConnectionStatus status{ConnectionStatus::Connecting};
    std::string endpoint;
    double lastReceiveTime{};
    std::uint32_t roundTripMilliseconds{};
};

class ConnectionRegistry final {
public:
    [[nodiscard]] ConnectionId add(std::string endpoint, double now);
    bool remove(ConnectionId id);
    bool set_status(ConnectionId id, ConnectionStatus status);
    bool touch(ConnectionId id, double now, std::uint32_t roundTripMilliseconds);
    [[nodiscard]] std::optional<ConnectionInfo> get(ConnectionId id) const;
    [[nodiscard]] std::vector<ConnectionInfo> all() const;
private:
    mutable std::mutex mutex_;
    std::uint32_t nextId_{1};
    std::unordered_map<ConnectionId, ConnectionInfo, ConnectionIdHash> connections_;
};

class OwnershipRegistry final {
public:
    bool assign(NetEntityId entity, ConnectionId owner);
    bool release(NetEntityId entity, ConnectionId expectedOwner = {});
    void release_all(ConnectionId owner);
    [[nodiscard]] ConnectionId owner_of(NetEntityId entity) const;
    [[nodiscard]] bool is_owner(ConnectionId connection, NetEntityId entity) const;
private:
    mutable std::mutex mutex_;
    std::unordered_map<NetEntityId, ConnectionId, NetEntityIdHash> owners_;
};

using ByteBuffer = std::vector<std::byte>;

struct PropertyDelta {
    NetEntityId entity;
    PropertyId property{};
    std::uint32_t revision{};
    ByteBuffer value;
};

class ReplicatedProperties final {
public:
    template<class T>
    bool set(NetEntityId entity, PropertyId property, const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        ByteBuffer bytes(sizeof(T));
        std::memcpy(bytes.data(), &value, sizeof(T));
        return set_bytes(entity, property, std::move(bytes));
    }

    bool set_bytes(NetEntityId entity, PropertyId property, ByteBuffer value);
    [[nodiscard]] std::optional<ByteBuffer> get_bytes(NetEntityId entity, PropertyId property) const;

    template<class T>
    [[nodiscard]] std::optional<T> get(NetEntityId entity, PropertyId property) const {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto bytes = get_bytes(entity, property);
        if (!bytes || bytes->size() != sizeof(T)) return std::nullopt;
        T value;
        std::memcpy(&value, bytes->data(), sizeof(T));
        return value;
    }

    [[nodiscard]] std::vector<PropertyDelta> collect_since(
        const std::unordered_map<NetEntityId, std::uint32_t, NetEntityIdHash>& acknowledged,
        std::span<const NetEntityId> relevantEntities) const;
    void apply(std::span<const PropertyDelta> deltas);
    bool erase_entity(NetEntityId entity);
    [[nodiscard]] std::uint32_t entity_revision(NetEntityId entity) const;
private:
    struct Value { ByteBuffer bytes; std::uint32_t revision{}; };
    struct EntityValues {
        std::uint32_t revision{};
        std::unordered_map<PropertyId, Value> properties;
    };
    mutable std::mutex mutex_;
    std::unordered_map<NetEntityId, EntityValues, NetEntityIdHash> entities_;
};

struct NetworkTransform {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 linearVelocity{0.0f};
};

struct EntitySnapshot {
    NetEntityId entity;
    NetworkTransform transform;
    std::vector<PropertyDelta> properties;
};

struct Snapshot {
    Tick tick{};
    double serverTime{};
    std::vector<EntitySnapshot> entities;
};

struct InterpolatedEntity {
    NetEntityId entity;
    NetworkTransform transform;
    float alpha{};
    Tick olderTick{};
    Tick newerTick{};
};

// Ordered snapshot history. Late snapshots are inserted in server-time order and
// duplicates by tick are replaced. Sampling can briefly extrapolate velocity.
class SnapshotBuffer final {
public:
    explicit SnapshotBuffer(std::size_t capacity = 32);
    void push(Snapshot snapshot);
    [[nodiscard]] std::vector<InterpolatedEntity> sample(double renderTime,
                                                          double maxExtrapolation = 0.1) const;
    [[nodiscard]] std::optional<Snapshot> latest() const;
    void clear();
private:
    std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<Snapshot> snapshots_;
};

enum class RelevancyMode { Distance, Always, OwnerOnly };

struct RelevancyRecord {
    NetEntityId entity;
    glm::vec3 position{0.0f};
    float radius{0.0f};
    RelevancyMode mode{RelevancyMode::Distance};
};

class RelevancySystem final {
public:
    void upsert(RelevancyRecord record);
    bool remove(NetEntityId entity);
    [[nodiscard]] std::vector<NetEntityId> query(ConnectionId viewer,
                                                  const glm::vec3& viewpoint,
                                                  float viewDistance,
                                                  const OwnershipRegistry& ownership) const;
private:
    mutable std::mutex mutex_;
    std::unordered_map<NetEntityId, RelevancyRecord, NetEntityIdHash> records_;
};

enum class RpcReliability { Unreliable, ReliableOrdered };

struct RpcMessage {
    RpcId rpc{};
    ConnectionId source;
    ConnectionId destination;
    NetEntityId target;
    RpcReliability reliability{RpcReliability::ReliableOrdered};
    std::uint32_t sequence{};
    ByteBuffer payload;
};

class RpcQueue final {
public:
    explicit RpcQueue(std::size_t capacity = 1024);
    bool enqueue_outgoing(RpcMessage message);
    bool enqueue_incoming(RpcMessage message);
    [[nodiscard]] std::vector<RpcMessage> drain_outgoing(ConnectionId destination,
                                                         std::size_t maximum = std::numeric_limits<std::size_t>::max());
    [[nodiscard]] std::vector<RpcMessage> drain_incoming(
        std::size_t maximum = std::numeric_limits<std::size_t>::max());
    [[nodiscard]] std::size_t dropped() const noexcept;
private:
    bool enqueue(std::deque<RpcMessage>& queue, RpcMessage message, bool assignSequence);
    std::size_t capacity_;
    std::uint32_t nextSequence_{1};
    std::size_t dropped_{};
    std::deque<RpcMessage> outgoing_;
    std::deque<RpcMessage> incoming_;
    mutable std::mutex mutex_;
};

struct PredictionInput {
    std::uint32_t sequence{};
    float deltaTime{};
    glm::vec3 movement{0.0f};
    ByteBuffer userData;
};

struct PredictedState {
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

using PredictionStep = std::function<void(PredictedState&, const PredictionInput&)>;

struct ReconciliationResult {
    bool corrected{};
    float positionError{};
    std::size_t replayedInputs{};
    glm::vec3 visualCorrection{0.0f};
};

class ClientPredictor final {
public:
    explicit ClientPredictor(PredictionStep step, float correctionThreshold = 0.05f);
    [[nodiscard]] PredictionInput predict(float deltaTime, const glm::vec3& movement,
                                           ByteBuffer userData = {});
    [[nodiscard]] ReconciliationResult reconcile(const PredictedState& authoritative,
                                                  std::uint32_t acknowledgedInput);
    void set_state(PredictedState state);
    [[nodiscard]] const PredictedState& state() const noexcept { return state_; }
    // Exponentially consumes reconciliation error for a non-snapping render pose.
    [[nodiscard]] PredictedState visual_state(float deltaTime, float sharpness = 12.0f);
    [[nodiscard]] std::size_t pending_input_count() const noexcept { return pending_.size(); }
private:
    PredictionStep step_;
    float threshold_;
    std::uint32_t nextSequence_{1};
    PredictedState state_;
    glm::vec3 visualError_{0.0f};
    std::deque<PredictionInput> pending_;
};

// Composes transport-independent runtime services. Socket serialization is kept
// outside this layer; snapshots and RPCs are plain deterministic data objects.
class NetworkingRuntime final {
public:
    [[nodiscard]] ConnectionRegistry& connections() noexcept { return connections_; }
    [[nodiscard]] OwnershipRegistry& ownership() noexcept { return ownership_; }
    [[nodiscard]] ReplicatedProperties& properties() noexcept { return properties_; }
    [[nodiscard]] RelevancySystem& relevancy() noexcept { return relevancy_; }
    [[nodiscard]] RpcQueue& rpcs() noexcept { return rpcs_; }
    [[nodiscard]] SnapshotBuffer& snapshots() noexcept { return snapshots_; }

    void publish(NetEntityId entity, NetworkTransform transform);
    bool unpublish(NetEntityId entity);
    [[nodiscard]] Snapshot make_snapshot(Tick tick, double serverTime,
                                         std::span<const NetEntityId> relevantEntities,
                                         const std::unordered_map<NetEntityId, std::uint32_t, NetEntityIdHash>&
                                             acknowledgedPropertyRevisions = {}) const;
private:
    ConnectionRegistry connections_;
    OwnershipRegistry ownership_;
    ReplicatedProperties properties_;
    RelevancySystem relevancy_;
    RpcQueue rpcs_;
    SnapshotBuffer snapshots_;
    mutable std::mutex stateMutex_;
    std::unordered_map<NetEntityId, NetworkTransform, NetEntityIdHash> states_;
};

} // namespace Engine::Networking
