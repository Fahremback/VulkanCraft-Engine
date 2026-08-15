#pragma once

#include <glm/glm.hpp>

#include <any>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Generic gameplay abstractions (README section 26): Event, Timer, Trigger,
// Interaction, Damage, Inventory, Objective. Everything lives in
// Engine::Gameplay so it never conflicts with Engine::MissionRuntime and
// friends in GameplayVisual.hpp.
//
// Entity ids are plain uint64_t to keep the framework self-contained.

namespace Engine::Gameplay {

// ---------------------------------------------------------------------------
// Event: named events with std::any payloads, priority-ordered subscriptions
// and cancellation.
// ---------------------------------------------------------------------------

using EventPayload = std::any;
// Handlers return true to keep dispatching, false to cancel (stop
// propagation to the remaining lower-priority handlers).
using EventHandler = std::function<bool(const EventPayload&)>;

struct EmitResult {
    size_t handlers_called{0};
    bool canceled{false};
};

class EventBus final {
public:
    // Returns a subscription id (0 if handler is empty).
    uint64_t subscribe(const std::string& name, EventHandler handler, int priority = 0);
    bool unsubscribe(uint64_t subscriptionId);
    EmitResult emit(const std::string& name, const EventPayload& payload = {});
    size_t handler_count(const std::string& name) const;
    void clear();

private:
    struct Entry {
        uint64_t id{0};
        EventHandler handler;
        int priority{0};
    };
    std::unordered_map<std::string, std::vector<Entry>> handlers_;
    uint64_t nextId_{1};
};

// ---------------------------------------------------------------------------
// Timer: named timers with delay, optional looping, cancellation and
// remaining-time queries.
// ---------------------------------------------------------------------------

class TimerSystem final {
public:
    // Schedules a named timer. loop=false fires once; loop=true repeats every
    // `interval` seconds (defaults to `delay`) until canceled.
    uint64_t schedule(const std::string& name, float delay, std::function<void()> callback,
                      bool loop = false, float interval = -1.0f);
    bool cancel(uint64_t timerId);
    size_t cancel_named(const std::string& name);
    void update(float deltaSeconds);
    float remaining(uint64_t timerId) const;                 // -1.0f if unknown
    float remaining_named(const std::string& name) const;    // -1.0f if none
    bool is_active(uint64_t timerId) const;
    size_t active_count() const;

private:
    struct Timer {
        uint64_t id{0};
        std::string name;
        float remaining{0.0f};
        float interval{0.0f};
        bool loop{false};
        bool alive{true};
        std::function<void()> callback;
    };
    std::vector<Timer> timers_;
    uint64_t nextId_{1};
};

// ---------------------------------------------------------------------------
// Trigger: 3D box volume with point/ray tests and enter/stay/exit callbacks.
// ---------------------------------------------------------------------------

class TriggerVolume final {
public:
    TriggerVolume() = default;
    TriggerVolume(glm::vec3 center, glm::vec3 halfExtents, uint32_t layerMask = ~0u);

    void set_center(glm::vec3 center);
    void set_half_extents(glm::vec3 halfExtents);
    void set_layer_mask(uint32_t layerMask);
    const glm::vec3& center() const noexcept { return center_; }
    const glm::vec3& half_extents() const noexcept { return halfExtents_; }
    uint32_t layer_mask() const noexcept { return layerMask_; }

    bool contains(const glm::vec3& point) const;
    bool intersects_ray(const glm::vec3& origin, const glm::vec3& direction,
                        float maxDistance, float& outDistance) const;

    void on_enter(std::function<void(uint64_t)> callback);
    void on_stay(std::function<void(uint64_t)> callback);
    void on_exit(std::function<void(uint64_t)> callback);

    // Tracks `entity`; fires enter/stay/exit based on membership.
    void update(uint64_t entity, const glm::vec3& position, uint32_t layer = 1);
    bool is_inside(uint64_t entity) const noexcept { return inside_.count(entity) > 0; }
    size_t inside_count() const noexcept { return inside_.size(); }
    void reset();

private:
    glm::vec3 center_{0.0f};
    glm::vec3 halfExtents_{0.5f};
    uint32_t layerMask_{~0u};
    std::function<void(uint64_t)> onEnter_;
    std::function<void(uint64_t)> onStay_;
    std::function<void(uint64_t)> onExit_;
    std::unordered_set<uint64_t> inside_;
};

// Payload emitted by TriggerManager on enter/exit.
struct TriggerEvent {
    std::string volume;
    uint64_t entity{0};
};

// TriggerManager: named volumes plus automatic EventBus integration. Volumes
// emit "TriggerEnter" / "TriggerExit" events carrying a TriggerEvent payload
// whenever an entity crosses a boundary.
class TriggerManager final {
public:
    bool add(const std::string& name, TriggerVolume volume);
    bool remove(const std::string& name);
    void clear();
    TriggerVolume* volume(const std::string& name);
    const TriggerVolume* volume(const std::string& name) const;
    void bind_event_bus(EventBus* bus) { bus_ = bus; }
    void update(uint64_t entity, const glm::vec3& position, uint32_t layer = 1);
    size_t count() const noexcept { return volumes_.size(); }

private:
    std::unordered_map<std::string, TriggerVolume> volumes_;
    EventBus* bus_{nullptr};
};

// ---------------------------------------------------------------------------
// Interaction: named, ranged, availability-gated interactions.
// ---------------------------------------------------------------------------

struct Interaction {
    uint64_t id{0};
    std::string name;
    glm::vec3 position{0.0f};
    float radius{1.0f};
    bool available{true};
    std::function<void(uint64_t)> action;   // (instigator)
};

class InteractionSystem final {
public:
    uint64_t register_interaction(std::string name, glm::vec3 position, float radius,
                                  std::function<void(uint64_t)> action);
    bool unregister(uint64_t id);
    bool set_available(uint64_t id, bool available);
    const Interaction* find(uint64_t id) const;
    // Available interactions within `maxDistance` of position, sorted by distance.
    std::vector<uint64_t> query(const glm::vec3& position, float maxDistance) const;
    bool interact(uint64_t id, uint64_t instigator, const glm::vec3& position,
                  float maxDistance = std::numeric_limits<float>::max());
    size_t count() const noexcept { return interactions_.size(); }

private:
    std::unordered_map<uint64_t, Interaction> interactions_;
    uint64_t nextId_{1};
};

// ---------------------------------------------------------------------------
// Damage: typed damage, modifiers, shields, health with callbacks.
// ---------------------------------------------------------------------------

enum class DamageType : uint8_t { Physical, Fire, Ice, Poison, Electric, True };

struct DamageModifier {
    DamageType type{DamageType::Physical};
    float multiplier{1.0f};
    float flat{0.0f};
};

struct DamageInstance {
    float amount{0.0f};
    DamageType type{DamageType::Physical};
    uint64_t source{0};
    uint64_t target{0};
};

struct Shield {
    float amount{0.0f};
    DamageType type{DamageType::Physical};  // DamageType::True = absorbs any type
};

class Damageable final {
public:
    explicit Damageable(float maxHealth = 100.0f);

    void set_max_health(float value);
    float max_health() const noexcept { return maxHealth_; }
    float health() const noexcept { return health_; }
    float shield_amount(DamageType type) const;
    bool dead() const noexcept { return health_ <= 0.0f; }

    void heal(float amount);
    void add_modifier(DamageModifier modifier);
    bool remove_modifier(DamageType type);
    void add_shield(Shield shield);

    // Applies damage; returns the total effective damage dealt
    // (shield absorption + health loss). True damage bypasses shields.
    float apply(const DamageInstance& damage);
    float apply_damage(float amount, DamageType type = DamageType::Physical, uint64_t source = 0);

    void on_damaged(std::function<void(const DamageInstance&, float dealt)> callback);
    void on_died(std::function<void(const DamageInstance&)> callback);

private:
    float maxHealth_{100.0f};
    float health_{100.0f};
    std::vector<DamageModifier> modifiers_;
    std::vector<Shield> shields_;
    std::function<void(const DamageInstance&, float)> onDamaged_;
    std::function<void(const DamageInstance&)> onDied_;
    bool diedNotified_{false};
};

// ---------------------------------------------------------------------------
// Inventory: items with id / quantity / stack, add / remove / query / transfer.
// ---------------------------------------------------------------------------

struct ItemStack {
    uint64_t id{0};
    std::string name;
    uint32_t quantity{0};
    uint32_t maxStack{64};
};

class Inventory final {
public:
    explicit Inventory(size_t capacity = 32);
    void set_capacity(size_t capacity) { capacity_ = capacity; }
    size_t capacity() const noexcept { return capacity_; }
    size_t used_slots() const noexcept { return stacks_.size(); }

    bool add(const ItemStack& stack);   // false if nothing could be added
    uint32_t add(uint64_t id, uint32_t quantity, const std::string& name = "",
                 uint32_t maxStack = 64);
    uint32_t remove(uint64_t id, uint32_t quantity);   // returns removed count
    uint32_t count(uint64_t id) const;
    bool contains(uint64_t id) const { return count(id) > 0; }
    const ItemStack* find(uint64_t id) const;
    uint32_t transfer_to(Inventory& other, uint64_t id, uint32_t quantity);
    std::vector<ItemStack> items() const;
    void clear();

private:
    size_t capacity_{32};
    std::vector<ItemStack> stacks_;
};

// ---------------------------------------------------------------------------
// Objective: state machine (inactive/active/completed/failed), progress and
// required conditions.
// ---------------------------------------------------------------------------

enum class ObjectiveState : uint8_t { Inactive, Active, Completed, Failed };

class Objective final {
public:
    Objective() = default;
    Objective(std::string id, std::string description, uint32_t targetProgress = 1,
              bool autoComplete = true);

    const std::string& id() const noexcept { return id_; }
    const std::string& description() const noexcept { return description_; }
    ObjectiveState state() const noexcept { return state_; }
    uint32_t current_progress() const noexcept { return currentProgress_; }
    uint32_t target_progress() const noexcept { return targetProgress_; }
    float progress_ratio() const;
    bool completed() const noexcept { return state_ == ObjectiveState::Completed; }

    void activate();
    void deactivate();
    void complete();
    void fail();
    void add_progress(uint32_t amount = 1);
    void set_progress(uint32_t value);

    void add_required_condition(const std::string& name);
    void set_condition(const std::string& name, bool value);
    bool condition(const std::string& name) const;
    bool conditions_met() const;

    void on_state_changed(std::function<void(ObjectiveState)> callback);

private:
    void set_state(ObjectiveState state);

    std::string id_;
    std::string description_;
    uint32_t targetProgress_{1};
    uint32_t currentProgress_{0};
    bool autoComplete_{true};
    ObjectiveState state_{ObjectiveState::Inactive};
    std::vector<std::string> requiredConditions_;
    std::unordered_map<std::string, bool> conditionValues_;
    std::function<void(ObjectiveState)> onStateChanged_;
};

class ObjectiveTracker final {
public:
    bool add(Objective objective);
    bool remove(const std::string& id);
    void clear();
    Objective* objective(const std::string& id);
    const Objective* objective(const std::string& id) const;
    bool activate(const std::string& id);
    bool deactivate(const std::string& id);
    bool complete(const std::string& id);
    bool fail(const std::string& id);
    bool add_progress(const std::string& id, uint32_t amount = 1);
    std::vector<std::string> active_objectives() const;
    size_t count() const noexcept { return objectives_.size(); }

private:
    std::unordered_map<std::string, Objective> objectives_;
};

} // namespace Engine::Gameplay
