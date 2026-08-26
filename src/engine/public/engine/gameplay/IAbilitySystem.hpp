#pragma once

// Public ability/powers contract (FALTANTES §19 / META section 20 — abilities
// data-driven). A generic system of abilities/powers defined as data-driven
// assets: attributes, tags, costs, cooldowns and conditions; targeting and
// COMPOSABLE effects; effects that integrate with voxel, physics, particles,
// audio and animation (through a minimal world seam + event callbacks);
// cancel/interrupt/periodic casts; persistence, authority and client
// prediction; and the MCP/CLI authoring surface.
//
// An AbilityDefinition is PURE DATA (JSON, versioned, all-or-nothing, bit-exact
// round-trip — the same contract shape as VehicleAsset). The runtime applies
// it through IAbilityWorld — a minimal seam the project (or the engine's voxel
// world + gameplay runtime) implements — so no core code needs to change to
// add an ability: the proofs (telekinesis, flight, a scene-altering block
// edit) run entirely on public contracts.
//
// This header is self-contained (glm only). load_from_json / to_json /
// validate are implemented by the SDK adapter (src/engine/sdk/AbilitySystem.cpp).

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace engine {
namespace gameplay {

// Body id/state and raycast hit are shared with the gameplay runtime
// (IGameplayRuntime.hpp); the ability world seam only needs these physics
// primitives, not the full runtime.
struct AbilityBodyId {
    std::uint32_t id{ 0 };
    bool valid() const { return id != 0; }
};

struct AbilityBodyState {
    glm::vec3 position{ 0.0f };
    glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    glm::vec3 linearVelocity{ 0.0f };
    glm::vec3 angularVelocity{ 0.0f };
};

struct AbilityRaycastHit {
    AbilityBodyId body;
    glm::vec3 point{ 0.0f };
    glm::vec3 normal{ 0.0f, 1.0f, 0.0f };
    float distance{ 0.0f };
};

// ---- Attributes / tags / cost / cooldown / conditions -----------------------

// A named numeric attribute (e.g. "level", "strength", "range"). Abilities may
// read attributes of the caster and/or of the targeted body via the world
// seam (attribute() below); a condition may gate on them.
struct AbilityAttribute {
    std::string name;
    float value{ 0.0f };
};

// Named tag strings ("movement", "fire", "ultimate"). Conditions test for
// presence on the caster/target; the runtime never interprets tag meaning.
using AbilityTagList = std::vector<std::string>;

// Resource cost: spends `amount` of the named resource from the caster when
// the cast starts (the world seam's spend_cost()). An empty resource name or
// amount <= 0 means "no cost".
struct AbilityCost {
    std::string resource;
    float amount{ 0.0f };
};

// Condition kinds, evaluated against the caster/target through the world seam
// before a cast is accepted. All conditions must pass (AND). Unknown kinds are
// refused at load time (all-or-nothing), never evaluated.
enum class AbilityConditionKind : std::uint8_t {
    OwnerTag,        // caster carries `tag`
    TargetTag,       // target carries `tag` (fails for a non-body target)
    OwnerAttribute,  // caster attribute `attribute` >= minValue
    TargetAttribute, // target attribute `attribute` >= minValue
    Distance,        // |caster - target point| <= maxDistance
};

struct AbilityCondition {
    AbilityConditionKind kind{ AbilityConditionKind::OwnerTag };
    std::string tag;
    std::string attribute;
    float minValue{ 0.0f };
    float maxDistance{ 0.0f };
};

// ---- Targeting --------------------------------------------------------------

// How the cast resolves its target. Self needs nothing; Direction provides a
// unit direction; Point a world position; Body a physics body id.
enum class AbilityTargetMode : std::uint8_t { Self, Direction, Point, Body };

struct AbilityTargeting {
    AbilityTargetMode mode{ AbilityTargetMode::Self };
    float range{ 0.0f };   // max distance (0 = unlimited) for Direction/Point
    float radius{ 0.0f };  // AoE radius for effects that scale by proximity
};

// The resolved cast target (runtime).
struct AbilityTarget {
    AbilityTargetMode mode{ AbilityTargetMode::Self };
    glm::vec3 point{ 0.0f };
    glm::vec3 direction{ 0.0f, 0.0f, -1.0f };
    AbilityBodyId body;
};

// ---- Effects (composable) ---------------------------------------------------

enum class AbilityEffectType : std::uint8_t {
    Damage,     // subtracts health from the target body (world seam damage())
    Heal,       // adds health to the target body
    Impulse,    // impulse to the target body along direction * force
    Telekinesis,// sustained: holds the target body near a point (caster + offset)
    Flight,     // sustained: applies upward thrust to the caster body
    BlockEdit,  // scene-altering: writes blocks in a box region (voxel seam)
    Periodic,   // wrapper: repeats a sub-effect at interval for `ticks` times
};

struct AbilityEffect {
    AbilityEffectType type{ AbilityEffectType::Damage };

    // Damage / Heal
    float amount{ 0.0f };

    // Impulse
    float force{ 0.0f };

    // Telekinesis — sustained. Hold offset relative to the CASTER position;
    // grabForce is the spring stiffness pulling the body to the hold point;
    // durationSeconds > 0 ends the hold automatically, 0 = until cancel.
    float holdOffsetX{ 0.0f };
    float holdOffsetY{ 1.5f };
    float holdOffsetZ{ 0.0f };
    float grabForce{ 240.0f };
    float durationSeconds{ 0.0f };

    // Flight — sustained. Upward thrust applied to the caster body each
    // update while active; durationSeconds > 0 ends it automatically.
    float thrust{ 320.0f };

    // BlockEdit — scene-altering. min/max are the box corners RELATIVE to the
    // target point (relative=true) or absolute world coordinates; blockId is
    // the block to write (0 = air). Written through the world seam
    // set_block(), which the seam may route through the voxel transaction
    // path. The box volume is validated at load (<= kMaxBlockEditVolume).
    glm::ivec3 min{ -1, -1, -1 };
    glm::ivec3 max{ 1, 1, 1 };
    std::uint32_t blockId{ 1 };
    bool relative{ true };

    // Periodic — wrapper. Held by shared_ptr so the recursive type stays
    // copyable AND compiles on MSVC (std::optional<AbilityEffect> inside
    // AbilityEffect would need a complete type for optional's storage). Null
    // == no sub-effect.
    float intervalSeconds{ 0.5f };
    int ticks{ 4 };
    std::shared_ptr<AbilityEffect> subEffect;

    // Presentation hooks (particles/audio/animation): named references the
    // PROJECT resolves through the event sink (see AbilityEvent below) — the
    // runtime never interprets them. Empty = no hook.
    std::string castAnimation;
    std::string particleEffect;
    std::string soundEffect;
};

// ---- The data-driven definition ---------------------------------------------

struct AbilityDefinition {
    std::string id;    // stable project id; derived from "abilities:<name>"
    std::string name;
    int version{ 1 };

    std::vector<AbilityAttribute> attributes;
    AbilityTagList tags;
    AbilityCost cost;
    float cooldownSeconds{ 0.0f };

    std::vector<AbilityCondition> conditions;
    AbilityTargeting targeting;

    // Effects are applied in declaration order at cast; sustained effects
    // (Telekinesis/Flight) keep ticking through update() until their duration
    // ends or the cast is cancelled/interrupted.
    std::vector<AbilityEffect> effects;

    bool cancelable{ true };
    bool interruptible{ true };

    // All-or-nothing: refuses malformed documents with a diagnostic (never
    // clamps or partially applies). Implemented by the SDK adapter.
    bool load_from_json(const std::string& jsonText, std::string& errorOut);
    // Bit-exact: to_json() round-trips every field (%.9g float emission).
    std::string to_json() const;
    bool validate(std::string& errorOut) const;
};

// ---- World seam --------------------------------------------------------------

// The minimal world an ability runtime needs. The project implements this over
// its own world (the engine's voxel world + gameplay runtime qualify); the
// runtime never couples to any concrete world type. Every method is
// deterministic and may be called from the headless tick.
class IAbilityWorld {
public:
    virtual ~IAbilityWorld() = default;

    // ---- voxel (BlockEdit) ----
    // Block id at the cell (0 = air). Used to implement "an ability that
    // alters the scene": BlockEdit writes through set_block.
    virtual std::uint32_t block_at(int x, int y, int z) const = 0;
    // Writes a block. The implementation routes this through its own
    // authoritative mutation path (e.g. the voxel transaction); the runtime
    // only issues writes. Returns false when the write is refused.
    virtual bool set_block(int x, int y, int z, std::uint32_t blockId) = 0;

    // ---- physics ----
    virtual bool body_state(const AbilityBodyId& body, AbilityBodyState& out) const = 0;
    virtual bool apply_impulse(const AbilityBodyId& body,
                               const glm::vec3& impulse) = 0;
    virtual bool add_force(const AbilityBodyId& body, const glm::vec3& force) = 0;
    virtual bool set_transform(const AbilityBodyId& body,
                               const glm::vec3& position,
                               const glm::quat& rotation) = 0;
    virtual bool raycast(const glm::vec3& origin, const glm::vec3& direction,
                         float maxDistance, AbilityRaycastHit& out) const = 0;

    // ---- attributes / cost / health (caster & target) ----
    // Attribute value of the caster (the body that cast) or the target body,
    // or 0 when absent. The project decides what attributes exist; the
    // runtime only reads the names the definition declares.
    virtual float attribute(const AbilityBodyId& body, const std::string& name) const = 0;
    // Tags carried by the caster / target body. Empty when absent.
    virtual AbilityTagList tags(const AbilityBodyId& body) const = 0;
    // Spends `amount` of `resource` from the caster. Returns false (and
    // refuses the cast) when the caster cannot afford it.
    virtual bool spend_cost(const AbilityBodyId& caster,
                            const std::string& resource, float amount) = 0;
    // Health operations for Damage/Heal. health() returns the current value;
    // damage()/heal() clamp at 0 and the project's maximum. Returns false for
    // an unknown body.
    virtual bool health(const AbilityBodyId& body, float& out) const = 0;
    virtual bool damage(const AbilityBodyId& body, float amount) = 0;
    virtual bool heal(const AbilityBodyId& body, float amount) = 0;
};

// ---- Events (particles / audio / animation hooks) ---------------------------

// Fired by the runtime for presentation hooks. The project wires these to its
// particle/audio/animation systems; the runtime only reports. Text-only
// validation marks the visual/audio side as HUMAN-VISUAL-PENDING — the event
// sequence itself is deterministic and tested.
struct AbilityEvent {
    enum class Kind : std::uint8_t { Cast, EffectTick, Cancelled, Interrupted, Finished };
    Kind kind{ Kind::Cast };
    std::string abilityId;
    std::string animation;     // castAnimation / sub-effect hook (empty = none)
    std::string particle;      // particleEffect hook
    std::string sound;         // soundEffect hook
    glm::vec3 position{ 0.0f };
};

// ---- Runtime -----------------------------------------------------------------

// Result of a cast attempt (all-or-nothing: a rejected cast applies nothing).
struct CastResult {
    bool accepted{ false };
    std::string error;
    std::size_t castIndex{ 0 };   // handle for cancel()/interrupt(), valid when accepted
    // Immediate effects applied in this cast (declaration order). Sustained
    // casts (Telekinesis/Flight/Periodic) stay active and are advanced by
    // update().
    std::size_t effectCount{ 0 };
};

// A live cast: what update() advances and cancel()/interrupt() stops.
struct ActiveCastInfo {
    std::size_t castIndex{ 0 };
    std::string abilityId;
    AbilityBodyId caster;
    AbilityTarget target;
    float elapsedSeconds{ 0.0f };
    float durationSeconds{ 0.0f };  // 0 = indefinite (until cancel)
    bool cancelled{ false };
    bool interrupted{ false };
};

// Authority/prediction state (FALTANTES §19 — persistence, authority and
// prediction of the network). The SERVER is the authority: it owns the
// cooldowns and the accepted casts. The CLIENT may predict a cast locally
// (immediate gameplay) and reconcile when the authoritative state arrives.
// Serialized bit-exactly (%.9g) so it travels in saves and replication.
struct AbilityStateSnapshot {
    std::size_t nextCastIndex{ 0 };
    // Cooldown remaining per ability id (0 = ready).
    std::map<std::string, float> cooldowns;
    std::vector<ActiveCastInfo> activeCasts;
};

class IAbilitySystem {
public:
    virtual ~IAbilitySystem() = default;

    // ---- definitions (data-driven) ----
    virtual bool register_ability(const AbilityDefinition& definition,
                                  std::string& errorOut) = 0;
    virtual bool unregister_ability(const std::string& id) = 0;
    virtual const AbilityDefinition* ability(const std::string& id) const = 0;
    virtual std::vector<std::string> ability_ids() const = 0;

    // ---- casting ----
    // Casts `abilityId` from `caster` at `target`. Checks (in order): the
    // ability exists, the caster body is valid, all conditions pass, the
    // ability is not on cooldown, and the cost is affordable (spent through
    // the world seam). On acceptance the cost is spent, the cooldown starts,
    // and the effects apply through `world` (sustained effects become active
    // casts). All-or-nothing on rejection: a refused cast applies nothing and
    // spends nothing. Deterministic: the same sequence of casts + updates on
    // the same world reproduces bit-exactly.
    virtual CastResult cast(const std::string& abilityId,
                            const AbilityBodyId& caster,
                            const AbilityTarget& target,
                            IAbilityWorld& world) = 0;
    // Advances cooldowns and active casts (Telekinesis hold, Flight thrust,
    // Periodic ticks, durations). Deterministic: the same sequence of casts +
    // updates reproduces bit-exactly.
    virtual void update(float deltaTime, IAbilityWorld& world) = 0;

    // ---- cancel / interrupt ----
    // Cancels a sustained cast: sustained effects stop and their cleanup runs
    // (e.g. telekinesis releases the held body). Returns false when the cast
    // index is unknown/already finished.
    virtual bool cancel(std::size_t castIndex, IAbilityWorld& world,
                        std::string& errorOut) = 0;
    // Interrupt is cancel for abilities that are interruptible; an
    // uninterruptible ability refuses (diagnostic) and keeps running.
    virtual bool interrupt(std::size_t castIndex, IAbilityWorld& world,
                           std::string& errorOut) = 0;

    // ---- queries ----
    virtual float cooldown_remaining(const std::string& abilityId) const = 0;
    virtual bool on_cooldown(const std::string& abilityId) const = 0;
    virtual std::size_t active_cast_count() const = 0;
    virtual ActiveCastInfo active_cast(std::size_t castIndex) const = 0;

    // ---- persistence / authority / prediction ----
    // Captures the full runtime state (cooldowns + active casts). The SERVER
    // serializes this to send to clients and to persist in saves.
    virtual AbilityStateSnapshot snapshot() const = 0;
    // Restores a snapshot (all-or-nothing; refuses malformed state). Used by
    // the client reconcile and by save/load.
    virtual bool apply_snapshot(const AbilityStateSnapshot& snapshot,
                                std::string& errorOut) = 0;
    virtual std::string serialize_state(std::string& errorOut) const = 0;
    virtual bool deserialize_state(const std::string& data,
                                   std::string& errorOut) = 0;

    // ---- events (particles/audio/animation hooks) ----
    using EventSink = std::function<void(const AbilityEvent&)>;
    virtual void set_event_sink(EventSink sink) = 0;
};

// The only implementation (src/engine/sdk/AbilitySystem.cpp).
std::unique_ptr<IAbilitySystem> create_ability_system();

// Serializes a state snapshot as a versioned JSON document (bit-exact
// round-trip via %.9g). Exposed so projects can persist/send it directly.
std::string serialize_ability_state(const AbilityStateSnapshot& snapshot,
                                    std::string& errorOut);
bool deserialize_ability_state(const std::string& data,
                               AbilityStateSnapshot& out,
                               std::string& errorOut);

}  // namespace gameplay
}  // namespace engine
