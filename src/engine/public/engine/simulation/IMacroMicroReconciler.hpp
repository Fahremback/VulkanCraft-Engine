#pragma once

// IMacroMicroReconciler — FALTANTES differential "MacroMicroReconciler":
// DETERMINISTIC MATERIALIZATION OF AGGREGATE CONSEQUENCES (META section 32 —
// engine-own code; nothing in external/solutions resolves it).
//
// Simulation LOD (ISimulationLod, §20) evolves DISTANT regions analytically:
// a region in the Aggregate tier advances population/resources counters
// through a deterministic seasonal model instead of ticking entities. When
// the region reactivates (Aggregate -> Full/Coarse), those MACRO counters
// must become CONCRETE MICRO consequences — entities to spawn, resources to
// drop, growth stages to apply, entities to remove — and the mapping must be
// reproducible: the same macro state materializes the SAME concrete effects,
// bit-exact, on every machine and every run.
//
// This reconciler is the pure decision engine for that mapping:
//   - MATERIALIZE (pure): macro state (aggregate counters + per-region seed +
//     seasonal growth + tags) -> an ORDERED, DETERMINISTIC list of concrete
//     effects. Counts come from the integer parts of the counters; positions
//     are derived from splitmix64(seed, index) mapped into the region cell;
//     archetype/item/block selection comes from data-driven RULES (first
//     rule whose tag matches the region; tag "" = default). No RNG, fixed
//     arithmetic order — bit-exact across instances.
//   - RECONCILE (budget + continuation): the world materializes consequences
//     over time, not in one frame. reconcile() emits at most
//     maxEffectsPerTick effects per call and persists a cursor, so a large
//     consequence set spans ticks. If the macro state CHANGES while effects
//     are still pending, the pending batch is INVALIDATED and re-derived from
//     the newest macro — exactly the affected-descendants invalidation of
//     ICausalResolver applied to temporal consequences.
//   - MERGE_AND_RESOLVE (pure): when two regions' consequences target the
//     same world slot, ONE effect survives — higher kind priority wins, ties
//     break by lower region seed. Deterministic.
//   - PERSISTENCE: serialize_state/deserialize_state round-trips the
//     reconciliation state (cursor, pending, fingerprint) bit-exactly —
//     a partially-materialized consequence survives save/load/replication.
//
// The runtime is PURE and DETERMINISTIC — it never touches a concrete world;
// it only DECIDES and REPORTS effects, the project applies. State is
// caller-owned and explicit (the IAnimationLod / ISimulationLod pattern).
// All refusals are all-or-nothing with a diagnostic. Self-contained (std).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace simulation {

// The MACRO input: the aggregate state of one region (the handoff shape
// ISimulationLod emits on Aggregate -> Full/Coarse transitions and on
// AggregateUpdate). Caller-owned; validated all-or-nothing.
struct ReconcilerMacroState {
    int version{ 1 };
    std::int64_t cellX{ 0 };
    std::int64_t cellZ{ 0 };
    // Grid convention (same as ISimulationLod): cell = floor(world / cellSize).
    // Must be finite and > 0.
    float cellSize{ 16.0f };
    // Current aggregate population counter (>= 0, finite).
    float population{ 0.0f };
    // The counter BEFORE the latest aggregate evolution (>= 0, finite). A drop
    // (floor(prev) > floor(cur)) materializes RemoveEntity consequences.
    float previousPopulation{ 0.0f };
    // Current aggregate resources counter (>= 0, finite).
    float resources{ 0.0f };
    // Seasonal growing factor in [0, 1] (the climate model's growth; the
    // season itself does not need to be passed — growth already encodes it).
    float growth{ 0.0f };
    // Per-region deterministic seed (the splitmix64 base for every derived
    // position/handle — same seed + same counters = same consequences).
    std::uint64_t seed{ 0 };
    // Region tags ("forest", "arid", ...) selecting the materialization rules.
    std::vector<std::string> tags;
};

// One data-driven materialization rule (JSON versioned, all-or-nothing,
// bit-exact round-trip). The first rule whose tag is present in the region's
// tags wins; a rule with an EMPTY tag ("") is the default and matches any
// region without an explicit rule.
struct ReconcilerRule {
    std::string tag;             // "" = default rule
    std::string archetypeId;     // SpawnEntity (population units)
    std::string itemId;          // ResourceDrop (resource units)
    std::string blockId;         // GrowthStage (growth-advanced blocks)
    // Growth stages 1..maxGrowthStages. Must be >= 1.
    int maxGrowthStages{ 3 };
    // Growth consequences per unit of growth (>= 0): floor(growth * density).
    float growthDensity{ 2.0f };
};

// The emission budget (JSON versioned, all-or-nothing). 0 = unlimited.
struct ReconcilerBudget {
    int version{ 1 };
    int maxEffectsPerTick{ 0 };  // >= 0
};

// One CONCRETE micro consequence the project applies to its world. Each
// effect is ONE action (one entity, one drop, one block). The kind order also
// defines the conflict priority (see merge_and_resolve).
struct MaterializedEffect {
    enum class Kind : std::uint8_t {
        SpawnEntity,   // lowest conflict priority
        ResourceDrop,
        GrowthStage,
        RemoveEntity,  // highest conflict priority
    };
    Kind kind{ Kind::SpawnEntity };
    // SpawnEntity: the archetype from the matching rule.
    std::string archetypeId;
    // RemoveEntity: a synthetic deterministic handle "<cellX>,<cellZ>:decline:<index>"
    // the project maps to its own entity ids.
    std::string handle;
    std::string reason;  // RemoveEntity ("population_decline")
    // ResourceDrop: the item from the matching rule.
    std::string itemId;
    // GrowthStage: the block from the matching rule.
    std::string blockId;
    // SpawnEntity / ResourceDrop: world position inside the region cell
    // ((cell + [0.15, 0.85]) * cellSize — derived, never on the border).
    float positionX{ 0.0f };
    float positionZ{ 0.0f };
    // GrowthStage: integer block coordinates (y = 0 surface by convention —
    // the project applies the real surface height).
    int blockX{ 0 };
    int blockY{ 0 };
    int blockZ{ 0 };
    int growthStage{ 1 };  // 1..maxGrowthStages
};

// The caller-owned reconciliation state (explicit; persisted bit-exactly).
struct ReconcilerState {
    int version{ 1 };
    std::int64_t cellX{ 0 };
    std::int64_t cellZ{ 0 };
    // Fingerprint of the macro state that produced `pending` (0 = none yet).
    // A macro change invalidates the pending batch (see reconcile).
    std::uint64_t macroFingerprint{ 0 };
    // Consequences already materialized (derived from the current macro) but
    // not yet emitted to the project.
    std::vector<MaterializedEffect> pending;
    // Index of the next effect to emit within `pending`.
    std::size_t cursor{ 0 };
    // How many times a batch was (re)materialized (persisted diagnostics).
    std::uint64_t materializationCount{ 0 };
    // True when every pending effect has been emitted.
    bool complete{ true };
};

class IMacroMicroReconciler {
public:
    virtual ~IMacroMicroReconciler() = default;

    // ---- configuration (data-driven, all-or-nothing) ----
    // Validates the rules (never clamps): duplicate tags (including a second
    // default), maxGrowthStages < 1, growthDensity < 0, non-finite density
    // refuse the whole list. An EMPTY list is valid — materialize/reconcile
    // then refuse when a consequence would need a missing id (all-or-nothing:
    // never emit an effect with an empty archetype/item/block).
    virtual bool set_rules(const std::vector<ReconcilerRule>& rules,
                           std::string& errorOut) = 0;
    // Loads {version:1, rules:[{tag, archetypeId, itemId, blockId,
    // maxGrowthStages, growthDensity}]} — version != 1 or a malformed rule
    // refuses all-or-nothing (the active rules stay untouched).
    virtual bool set_rules_json(const std::string& jsonText,
                                std::string& errorOut) = 0;
    // Canonical deterministic emit of the active rules (the bit-exact
    // round-trip counterpart of set_rules_json; floats as %.9g, std::map
    // ordering). Empty until set_rules/set_rules_json succeeded.
    virtual std::string rules_to_json() const = 0;
    virtual bool set_budget(const ReconcilerBudget& budget,
                            std::string& errorOut) = 0;
    virtual const std::vector<ReconcilerRule>* rules() const = 0;
    virtual const ReconcilerBudget* budget() const = 0;

    // ---- pure materialization ----
    // The deterministic macro -> effects mapping (see the file comment for
    // the exact rules). `effectsOut` is cleared at entry; on refusal it is
    // left empty. Order is fixed: spawns, drops, growth, removals. Same macro
    // -> bit-identical list, on every instance. Refuses (all-or-nothing) an
    // invalid macro (bad version, non-finite/negative counters, cellSize <= 0,
    // growth outside [0,1]) or a consequence category whose matching rule
    // misses the needed id.
    virtual bool materialize(const ReconcilerMacroState& macro,
                             std::vector<MaterializedEffect>& effectsOut,
                             std::string& errorOut) const = 0;

    // ---- reconciliation step (budget + continuation + invalidation) ----
    // Emits up to maxEffectsPerTick effects from the region's consequence
    // set, persisting the cursor across calls (a large set spans ticks).
    // If the macro fingerprint changed while effects were pending, the
    // pending batch is INVALIDATED and re-derived from the NEW macro (only
    // the affected pending consequences are dropped — the ICausalResolver
    // semantics). `effectsOut` is cleared at entry. Refuses all-or-nothing
    // (nothing in `state` is touched) on an invalid macro/state/budget.
    virtual bool reconcile(ReconcilerState& state,
                           const ReconcilerMacroState& macro,
                           std::vector<MaterializedEffect>& effectsOut,
                           std::string& errorOut) = 0;

    // ---- conflict resolution (pure, deterministic) ----
    // Merges two consequence batches (two regions materializing into the same
    // world) keeping ONE effect per target slot. A target slot is the block
    // (GrowthStage), the position (SpawnEntity/ResourceDrop) or the handle
    // (RemoveEntity). Higher kind priority wins (RemoveEntity > GrowthStage >
    // ResourceDrop > SpawnEntity); a tie breaks by LOWER region seed. The
    // output order is deterministic (input order, winners in place). Refuses
    // all-or-nothing when either batch contains an invalid effect.
    virtual bool merge_and_resolve(const std::vector<MaterializedEffect>& a,
                                   const std::vector<MaterializedEffect>& b,
                                   std::uint64_t seedA, std::uint64_t seedB,
                                   std::vector<MaterializedEffect>& out,
                                   std::string& errorOut) const = 0;

    // ---- persistence (bit-exact %.9g, all-or-nothing) ----
    // Serializes the full reconciliation state (fingerprint, cursor, pending
    // effects, counters). deserialize is all-or-nothing: a malformed document
    // leaves `out` untouched.
    virtual bool serialize_state(const ReconcilerState& state,
                                 std::string& out,
                                 std::string& errorOut) const = 0;
    virtual bool deserialize_state(const std::string& data,
                                   ReconcilerState& out,
                                   std::string& errorOut) const = 0;
};

// The only implementation (src/engine/sdk/MacroMicroReconciler.cpp).
std::unique_ptr<IMacroMicroReconciler> create_macro_micro_reconciler();

}  // namespace simulation
}  // namespace engine
