#include "GameplayFramework.hpp"

#include <algorithm>
#include <utility>

namespace Engine::Gameplay {

// ---------------------------------------------------------------------------
// EventBus
// ---------------------------------------------------------------------------

uint64_t EventBus::subscribe(const std::string& name, EventHandler handler, int priority) {
    if (!handler) {
        return 0;
    }
    const uint64_t id = nextId_++;
    handlers_[name].push_back(Entry{id, std::move(handler), priority});
    return id;
}

bool EventBus::unsubscribe(uint64_t subscriptionId) {
    bool removed = false;
    for (auto& [name, entries] : handlers_) {
        (void)name;
        const size_t before = entries.size();
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [subscriptionId](const Entry& e) { return e.id == subscriptionId; }),
                      entries.end());
        removed = removed || entries.size() != before;
    }
    return removed;
}

EmitResult EventBus::emit(const std::string& name, const EventPayload& payload) {
    EmitResult result;
    const auto it = handlers_.find(name);
    if (it == handlers_.end()) {
        return result;
    }
    std::vector<Entry> entries = it->second;
    std::stable_sort(entries.begin(), entries.end(),
                     [](const Entry& a, const Entry& b) { return a.priority > b.priority; });
    for (const Entry& entry : entries) {
        if (!entry.handler) {
            continue;
        }
        ++result.handlers_called;
        if (!entry.handler(payload)) {
            result.canceled = true;
            break;
        }
    }
    return result;
}

size_t EventBus::handler_count(const std::string& name) const {
    const auto it = handlers_.find(name);
    return it == handlers_.end() ? 0 : it->second.size();
}

void EventBus::clear() {
    handlers_.clear();
}

// ---------------------------------------------------------------------------
// TimerSystem
// ---------------------------------------------------------------------------

uint64_t TimerSystem::schedule(const std::string& name, float delay,
                               std::function<void()> callback, bool loop, float interval) {
    if (!callback) {
        return 0;
    }
    if (delay < 0.0f) {
        delay = 0.0f;
    }
    if (interval < 0.0f) {
        interval = delay;
    }
    Timer timer;
    timer.id = nextId_++;
    timer.name = name;
    timer.remaining = delay;
    timer.interval = interval;
    timer.loop = loop;
    timer.callback = std::move(callback);
    timers_.push_back(std::move(timer));
    return timers_.back().id;
}

bool TimerSystem::cancel(uint64_t timerId) {
    for (Timer& timer : timers_) {
        if (timer.id == timerId && timer.alive) {
            timer.alive = false;
            return true;
        }
    }
    return false;
}

size_t TimerSystem::cancel_named(const std::string& name) {
    size_t canceled = 0;
    for (Timer& timer : timers_) {
        if (timer.alive && timer.name == name) {
            timer.alive = false;
            ++canceled;
        }
    }
    return canceled;
}

void TimerSystem::update(float deltaSeconds) {
    if (timers_.empty()) {
        return;
    }
    std::vector<uint64_t> due;
    for (Timer& timer : timers_) {
        if (!timer.alive) {
            continue;
        }
        timer.remaining -= deltaSeconds;
        if (timer.remaining <= 0.0f) {
            due.push_back(timer.id);
        }
    }
    for (const uint64_t id : due) {
        for (Timer& timer : timers_) {
            if (!timer.alive || timer.id != id) {
                continue;
            }
            if (timer.callback) {
                timer.callback();
            }
            if (timer.alive && timer.loop) {
                // Re-arm looping timers (interval <= 0 keeps firing each update).
                timer.remaining = timer.interval > 0.0f ? timer.interval : 0.0f;
            } else {
                timer.alive = false;  // one-shot, or canceled during the callback
            }
            break;
        }
    }
    timers_.erase(std::remove_if(timers_.begin(), timers_.end(),
                                 [](const Timer& t) { return !t.alive; }),
                  timers_.end());
}

float TimerSystem::remaining(uint64_t timerId) const {
    for (const Timer& timer : timers_) {
        if (timer.id == timerId && timer.alive) {
            return timer.remaining > 0.0f ? timer.remaining : 0.0f;
        }
    }
    return -1.0f;
}

float TimerSystem::remaining_named(const std::string& name) const {
    float best = -1.0f;
    for (const Timer& timer : timers_) {
        if (!timer.alive || timer.name != name) {
            continue;
        }
        const float value = timer.remaining > 0.0f ? timer.remaining : 0.0f;
        if (best < 0.0f || value < best) {
            best = value;
        }
    }
    return best;
}

bool TimerSystem::is_active(uint64_t timerId) const {
    for (const Timer& timer : timers_) {
        if (timer.id == timerId && timer.alive) {
            return true;
        }
    }
    return false;
}

size_t TimerSystem::active_count() const {
    size_t count = 0;
    for (const Timer& timer : timers_) {
        if (timer.alive) {
            ++count;
        }
    }
    return count;
}

// ---------------------------------------------------------------------------
// TriggerVolume
// ---------------------------------------------------------------------------

TriggerVolume::TriggerVolume(glm::vec3 center, glm::vec3 halfExtents, uint32_t layerMask)
    : center_(center), halfExtents_(halfExtents), layerMask_(layerMask) {}

void TriggerVolume::set_center(glm::vec3 center) { center_ = center; }

void TriggerVolume::set_half_extents(glm::vec3 halfExtents) { halfExtents_ = halfExtents; }

void TriggerVolume::set_layer_mask(uint32_t layerMask) { layerMask_ = layerMask; }

bool TriggerVolume::contains(const glm::vec3& point) const {
    const glm::vec3 min = center_ - halfExtents_;
    const glm::vec3 max = center_ + halfExtents_;
    return point.x >= min.x && point.x <= max.x &&
           point.y >= min.y && point.y <= max.y &&
           point.z >= min.z && point.z <= max.z;
}

bool TriggerVolume::intersects_ray(const glm::vec3& origin, const glm::vec3& direction,
                                   float maxDistance, float& outDistance) const {
    const glm::vec3 min = center_ - halfExtents_;
    const glm::vec3 max = center_ + halfExtents_;
    float tmin = 0.0f;
    float tmax = std::numeric_limits<float>::max();
    for (int axis = 0; axis < 3; ++axis) {
        const float o = origin[axis];
        const float d = direction[axis];
        if (std::abs(d) < 1e-9f) {
            if (o < min[axis] || o > max[axis]) {
                return false;
            }
            continue;
        }
        float t1 = (min[axis] - o) / d;
        float t2 = (max[axis] - o) / d;
        if (t1 > t2) {
            std::swap(t1, t2);
        }
        tmin = std::max(tmin, t1);
        tmax = std::min(tmax, t2);
        if (tmin > tmax) {
            return false;
        }
    }
    if (tmax < 0.0f) {
        return false;
    }
    const float hit = tmin > 0.0f ? tmin : tmax;
    if (hit > maxDistance) {
        return false;
    }
    outDistance = hit;
    return true;
}

void TriggerVolume::on_enter(std::function<void(uint64_t)> callback) {
    onEnter_ = std::move(callback);
}

void TriggerVolume::on_stay(std::function<void(uint64_t)> callback) {
    onStay_ = std::move(callback);
}

void TriggerVolume::on_exit(std::function<void(uint64_t)> callback) {
    onExit_ = std::move(callback);
}

void TriggerVolume::update(uint64_t entity, const glm::vec3& position, uint32_t layer) {
    if ((layer & layerMask_) == 0) {
        return;
    }
    const bool insideNow = contains(position);
    const bool wasInside = is_inside(entity);
    if (insideNow && !wasInside) {
        inside_.insert(entity);
        if (onEnter_) {
            onEnter_(entity);
        }
    } else if (insideNow) {
        if (onStay_) {
            onStay_(entity);
        }
    } else if (wasInside) {
        inside_.erase(entity);
        if (onExit_) {
            onExit_(entity);
        }
    }
}

void TriggerVolume::reset() {
    inside_.clear();
}

// ---------------------------------------------------------------------------
// TriggerManager
// ---------------------------------------------------------------------------

bool TriggerManager::add(const std::string& name, TriggerVolume volume) {
    if (name.empty()) {
        return false;
    }
    volumes_.insert_or_assign(name, std::move(volume));
    return true;
}

bool TriggerManager::remove(const std::string& name) {
    return volumes_.erase(name) > 0;
}

void TriggerManager::clear() {
    volumes_.clear();
}

TriggerVolume* TriggerManager::volume(const std::string& name) {
    const auto it = volumes_.find(name);
    return it == volumes_.end() ? nullptr : &it->second;
}

const TriggerVolume* TriggerManager::volume(const std::string& name) const {
    const auto it = volumes_.find(name);
    return it == volumes_.end() ? nullptr : &it->second;
}

void TriggerManager::update(uint64_t entity, const glm::vec3& position, uint32_t layer) {
    for (auto& [name, volume] : volumes_) {
        const bool wasInside = volume.is_inside(entity);
        volume.update(entity, position, layer);
        if (!bus_) {
            continue;
        }
        const bool nowInside = volume.is_inside(entity);
        if (nowInside && !wasInside) {
            bus_->emit("TriggerEnter", std::make_any<TriggerEvent>(TriggerEvent{name, entity}));
        } else if (wasInside && !nowInside) {
            bus_->emit("TriggerExit", std::make_any<TriggerEvent>(TriggerEvent{name, entity}));
        }
    }
}

// ---------------------------------------------------------------------------
// InteractionSystem
// ---------------------------------------------------------------------------

uint64_t InteractionSystem::register_interaction(std::string name, glm::vec3 position,
                                                 float radius,
                                                 std::function<void(uint64_t)> action) {
    if (name.empty() || radius < 0.0f) {
        return 0;
    }
    const uint64_t id = nextId_++;
    Interaction interaction;
    interaction.id = id;
    interaction.name = std::move(name);
    interaction.position = position;
    interaction.radius = radius;
    interaction.action = std::move(action);
    interactions_.emplace(id, std::move(interaction));
    return id;
}

bool InteractionSystem::unregister(uint64_t id) {
    return interactions_.erase(id) > 0;
}

bool InteractionSystem::set_available(uint64_t id, bool available) {
    const auto it = interactions_.find(id);
    if (it == interactions_.end()) {
        return false;
    }
    it->second.available = available;
    return true;
}

const Interaction* InteractionSystem::find(uint64_t id) const {
    const auto it = interactions_.find(id);
    return it == interactions_.end() ? nullptr : &it->second;
}

std::vector<uint64_t> InteractionSystem::query(const glm::vec3& position,
                                               float maxDistance) const {
    std::vector<std::pair<float, uint64_t>> sorted;
    for (const auto& [id, interaction] : interactions_) {
        if (!interaction.available) {
            continue;
        }
        const float dist = glm::distance(position, interaction.position);
        if (dist <= maxDistance) {
            sorted.emplace_back(dist, id);
        }
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<uint64_t> result;
    result.reserve(sorted.size());
    for (const auto& [dist, id] : sorted) {
        (void)dist;
        result.push_back(id);
    }
    return result;
}

bool InteractionSystem::interact(uint64_t id, uint64_t instigator, const glm::vec3& position,
                                 float maxDistance) {
    const auto it = interactions_.find(id);
    if (it == interactions_.end() || !it->second.available) {
        return false;
    }
    const Interaction& interaction = it->second;
    const float dist = glm::distance(position, interaction.position);
    if (dist > interaction.radius || dist > maxDistance) {
        return false;
    }
    if (interaction.action) {
        interaction.action(instigator);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Damageable
// ---------------------------------------------------------------------------

Damageable::Damageable(float maxHealth)
    : maxHealth_(maxHealth), health_(maxHealth) {}

void Damageable::set_max_health(float value) {
    maxHealth_ = value > 0.0f ? value : 0.0f;
    health_ = std::min(health_, maxHealth_);
}

void Damageable::heal(float amount) {
    if (amount <= 0.0f) {
        return;
    }
    health_ = std::min(maxHealth_, health_ + amount);
}

void Damageable::add_modifier(DamageModifier modifier) {
    modifiers_.push_back(modifier);
}

bool Damageable::remove_modifier(DamageType type) {
    const auto it = std::find_if(modifiers_.begin(), modifiers_.end(),
                                 [type](const DamageModifier& m) { return m.type == type; });
    if (it == modifiers_.end()) {
        return false;
    }
    modifiers_.erase(it);
    return true;
}

void Damageable::add_shield(Shield shield) {
    shields_.push_back(shield);
}

float Damageable::shield_amount(DamageType type) const {
    float total = 0.0f;
    for (const Shield& shield : shields_) {
        if (shield.type == DamageType::True || shield.type == type) {
            total += shield.amount;
        }
    }
    return total;
}

float Damageable::apply(const DamageInstance& damage) {
    if (dead() || damage.amount <= 0.0f) {
        return 0.0f;
    }
    float amount = damage.amount;
    for (const DamageModifier& modifier : modifiers_) {
        if (modifier.type != damage.type) {
            continue;
        }
        amount = amount * modifier.multiplier + modifier.flat;
    }
    if (amount <= 0.0f) {
        return 0.0f;
    }

    float dealt = 0.0f;
    // True damage bypasses shields entirely.
    if (damage.type != DamageType::True) {
        for (Shield& shield : shields_) {
            if (shield.amount <= 0.0f) {
                continue;
            }
            if (shield.type != DamageType::True && shield.type != damage.type) {
                continue;
            }
            const float absorbed = std::min(amount, shield.amount);
            shield.amount -= absorbed;
            amount -= absorbed;
            dealt += absorbed;
            if (amount <= 0.0f) {
                break;
            }
        }
    }
    if (amount > 0.0f) {
        const float healthLoss = std::min(health_, amount);
        health_ -= healthLoss;
        dealt += healthLoss;
    }
    if (onDamaged_) {
        onDamaged_(damage, dealt);
    }
    if (health_ <= 0.0f && !diedNotified_) {
        diedNotified_ = true;
        if (onDied_) {
            onDied_(damage);
        }
    }
    return dealt;
}

float Damageable::apply_damage(float amount, DamageType type, uint64_t source) {
    DamageInstance damage;
    damage.amount = amount;
    damage.type = type;
    damage.source = source;
    return apply(damage);
}

void Damageable::on_damaged(std::function<void(const DamageInstance&, float)> callback) {
    onDamaged_ = std::move(callback);
}

void Damageable::on_died(std::function<void(const DamageInstance&)> callback) {
    onDied_ = std::move(callback);
}

// ---------------------------------------------------------------------------
// Inventory
// ---------------------------------------------------------------------------

Inventory::Inventory(size_t capacity) : capacity_(capacity) {}

bool Inventory::add(const ItemStack& stack) {
    return add(stack.id, stack.quantity, stack.name, stack.maxStack) > 0;
}

uint32_t Inventory::add(uint64_t id, uint32_t quantity, const std::string& name,
                        uint32_t maxStack) {
    if (quantity == 0) {
        return 0;
    }
    if (maxStack == 0) {
        maxStack = 1;
    }
    uint32_t remaining = quantity;
    for (ItemStack& stack : stacks_) {
        if (stack.id != id || stack.quantity >= stack.maxStack) {
            continue;
        }
        const uint32_t room = stack.maxStack - stack.quantity;
        const uint32_t put = std::min(room, remaining);
        stack.quantity += put;
        remaining -= put;
        if (remaining == 0) {
            break;
        }
    }
    while (remaining > 0 && stacks_.size() < capacity_) {
        const uint32_t put = std::min(remaining, maxStack);
        stacks_.push_back(ItemStack{id, name.empty() ? "item_" + std::to_string(id) : name,
                                    put, maxStack});
        remaining -= put;
    }
    return quantity - remaining;
}

uint32_t Inventory::remove(uint64_t id, uint32_t quantity) {
    uint32_t removed = 0;
    for (auto it = stacks_.begin(); it != stacks_.end() && removed < quantity;) {
        if (it->id != id) {
            ++it;
            continue;
        }
        const uint32_t take = std::min(quantity - removed, it->quantity);
        it->quantity -= take;
        removed += take;
        if (it->quantity == 0) {
            it = stacks_.erase(it);
        } else {
            ++it;
        }
    }
    return removed;
}

uint32_t Inventory::count(uint64_t id) const {
    uint32_t total = 0;
    for (const ItemStack& stack : stacks_) {
        if (stack.id == id) {
            total += stack.quantity;
        }
    }
    return total;
}

const ItemStack* Inventory::find(uint64_t id) const {
    for (const ItemStack& stack : stacks_) {
        if (stack.id == id) {
            return &stack;
        }
    }
    return nullptr;
}

uint32_t Inventory::transfer_to(Inventory& other, uint64_t id, uint32_t quantity) {
    const ItemStack* source = find(id);
    if (!source) {
        return 0;
    }
    const std::string name = source->name;
    const uint32_t maxStack = source->maxStack;
    const uint32_t wanted = std::min(quantity, source->quantity);
    if (wanted == 0) {
        return 0;
    }
    remove(id, wanted);
    const uint32_t accepted = other.add(id, wanted, name, maxStack);
    if (accepted < wanted) {
        add(id, wanted - accepted, name, maxStack);
    }
    return accepted;
}

std::vector<ItemStack> Inventory::items() const {
    return stacks_;
}

void Inventory::clear() {
    stacks_.clear();
}

// ---------------------------------------------------------------------------
// Objective
// ---------------------------------------------------------------------------

Objective::Objective(std::string id, std::string description, uint32_t targetProgress,
                     bool autoComplete)
    : id_(std::move(id)),
      description_(std::move(description)),
      targetProgress_(targetProgress > 0 ? targetProgress : 1),
      autoComplete_(autoComplete) {}

float Objective::progress_ratio() const {
    return targetProgress_ > 0 ? static_cast<float>(currentProgress_) /
                                     static_cast<float>(targetProgress_)
                               : 0.0f;
}

void Objective::activate() {
    if (state_ == ObjectiveState::Active || state_ == ObjectiveState::Completed) {
        return;
    }
    currentProgress_ = 0;
    set_state(ObjectiveState::Active);
}

void Objective::deactivate() {
    if (state_ != ObjectiveState::Active) {
        return;
    }
    currentProgress_ = 0;
    set_state(ObjectiveState::Inactive);
}

void Objective::complete() {
    if (state_ != ObjectiveState::Active) {
        return;
    }
    currentProgress_ = targetProgress_;
    set_state(ObjectiveState::Completed);
}

void Objective::fail() {
    if (state_ == ObjectiveState::Completed) {
        return;
    }
    set_state(ObjectiveState::Failed);
}

void Objective::add_progress(uint32_t amount) {
    if (state_ != ObjectiveState::Active) {
        return;
    }
    set_progress(currentProgress_ + amount);
}

void Objective::set_progress(uint32_t value) {
    if (state_ != ObjectiveState::Active) {
        return;
    }
    currentProgress_ = std::min(value, targetProgress_);
    if (autoComplete_ && currentProgress_ >= targetProgress_ && conditions_met()) {
        complete();
    }
}

void Objective::add_required_condition(const std::string& name) {
    for (const std::string& existing : requiredConditions_) {
        if (existing == name) {
            return;
        }
    }
    requiredConditions_.push_back(name);
}

void Objective::set_condition(const std::string& name, bool value) {
    conditionValues_[name] = value;
    if (state_ == ObjectiveState::Active && autoComplete_ &&
        currentProgress_ >= targetProgress_ && conditions_met()) {
        complete();
    }
}

bool Objective::condition(const std::string& name) const {
    const auto it = conditionValues_.find(name);
    return it != conditionValues_.end() && it->second;
}

bool Objective::conditions_met() const {
    for (const std::string& name : requiredConditions_) {
        if (!condition(name)) {
            return false;
        }
    }
    return true;
}

void Objective::on_state_changed(std::function<void(ObjectiveState)> callback) {
    onStateChanged_ = std::move(callback);
}

void Objective::set_state(ObjectiveState state) {
    if (state_ == state) {
        return;
    }
    state_ = state;
    if (onStateChanged_) {
        onStateChanged_(state);
    }
}

// ---------------------------------------------------------------------------
// ObjectiveTracker
// ---------------------------------------------------------------------------

bool ObjectiveTracker::add(Objective objective) {
    if (objective.id().empty()) {
        return false;
    }
    return objectives_.emplace(objective.id(), std::move(objective)).second;
}

bool ObjectiveTracker::remove(const std::string& id) {
    return objectives_.erase(id) > 0;
}

void ObjectiveTracker::clear() {
    objectives_.clear();
}

Objective* ObjectiveTracker::objective(const std::string& id) {
    const auto it = objectives_.find(id);
    return it == objectives_.end() ? nullptr : &it->second;
}

const Objective* ObjectiveTracker::objective(const std::string& id) const {
    const auto it = objectives_.find(id);
    return it == objectives_.end() ? nullptr : &it->second;
}

bool ObjectiveTracker::activate(const std::string& id) {
    Objective* objective = this->objective(id);
    if (!objective) {
        return false;
    }
    objective->activate();
    return true;
}

bool ObjectiveTracker::deactivate(const std::string& id) {
    Objective* objective = this->objective(id);
    if (!objective) {
        return false;
    }
    objective->deactivate();
    return true;
}

bool ObjectiveTracker::complete(const std::string& id) {
    Objective* objective = this->objective(id);
    if (!objective) {
        return false;
    }
    objective->complete();
    return true;
}

bool ObjectiveTracker::fail(const std::string& id) {
    Objective* objective = this->objective(id);
    if (!objective) {
        return false;
    }
    objective->fail();
    return true;
}

bool ObjectiveTracker::add_progress(const std::string& id, uint32_t amount) {
    Objective* objective = this->objective(id);
    if (!objective) {
        return false;
    }
    objective->add_progress(amount);
    return true;
}

std::vector<std::string> ObjectiveTracker::active_objectives() const {
    std::vector<std::string> result;
    for (const auto& [id, objective] : objectives_) {
        (void)id;
        if (objective.state() == ObjectiveState::Active) {
            result.push_back(objective.id());
        }
    }
    return result;
}

} // namespace Engine::Gameplay
