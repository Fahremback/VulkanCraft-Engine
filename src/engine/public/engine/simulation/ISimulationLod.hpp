#pragma once

// ISimulationLod (FALTANTES §20 — Simulation LOD e ecossistemas): the PUBLIC
// simulation LOD contract. A world cannot afford to tick every entity/system
// every frame at full fidelity — Simulation LOD turns DISTANCE/RELEVANCE into
// a simulation budget per REGION:
//   - REGIONS: the world is divided into a grid of cells (cellSize). A region
//     is one cell; its simulation tier derives from its distance to the FOCUS
//     (the camera/player). The grid is ALIGNED to the origin (same convention
//     as IMultiScaleStreaming), so a world position maps to ONE canonical
//     region, deterministically.
//   - TIERS (data-driven, JSON versioned, all-or-nothing): each tier has a
//     mode (Full/Coarse/Aggregate/Sleeping), an update interval (the
//     frequency IS the LOD), a max-region budget (overflow falls to the next
//     cheaper tier) and, for Aggregate tiers, an aggregate interval.
//   - FREQUENCY: near regions tick every update; far regions tick at their
//     tier interval (elapsed-since-tick gating). This is the per-region
//     tick-rate reduction.
//   - SLEEPING: a region whose selected tier is Sleeping stops ticking
//     entirely (idleSeconds accumulates; the project parks its entities).
//   - AGGREGATE: distant regions evolve through a DETERMINISTIC analytic
//     model (population/resources counters driven by a seasonal climate
//     function) instead of full simulation. The aggregate update is a pure
//     function of (counters, dt, per-region seed, season) — no RNG, fixed
//     arithmetic order, bit-exact across instances.
//   - COHERENT TRANSITIONS: when a region returns from Aggregate to
//     Full/Coarse, the emitted RegionTierChanged event carries the aggregate
//     counters — the exact handoff the project applies so the region resumes
//     where the analytic model left it.
//   - WORLD CLOCK / CLIMATE / SEASONS: the state owns a deterministic clock
//     (tick, totalSeconds, timeOfDay, day, season). climate() derives a
//     per-region seasonal temperature/moisture/growth (splitmix64 hash of
//     region cell + season + tags — pure, deterministic), which drives the
//     aggregate evolution.
//   - SCHEDULED EVENTS: the project schedules events (tick + cell + payload);
//     the runtime fires them at the exact tick (persisted in the state).
//   - BUDGETS: maxRegions per tier caps CPU; the dirty-region set (regions
//     that ticked/changed/aggregate-updated this frame) is the network
//     budget; the serialized state is the memory budget (bit-exact %.9g).
//   - PERSISTENCE: serialize_state/deserialize_state round-trips the whole
//     state bit-exactly (clock, regions, scheduled events) — save/load and
//     replication of a distant world's evolution.
//
// The runtime is PURE and DETERMINISTIC — it never touches a concrete world;
// it only DECIDES and REPORTS (events + dirty set), the project applies. The
// state is caller-owned and explicit (the IAnimationLod / IProceduralLegs
// pattern): the caller keeps SimulationLodState and reads the emitted events;
// the adapter never hides state. Same spec + same (focus, dt) sequence ->
// identical state and identical event streams, bit-exact, across instances.
//
// Self-contained (std + glm). Deterministic. Headless. load_from_json /
// to_json / validate of the spec and serialize_state / deserialize_state of
// the state are implemented by the SDK adapter (src/engine/sdk/SimulationLod.cpp).

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace simulation {

// How a region is simulated at its tier.
enum class SimulationLodMode : std::uint8_t {
    Full,       // every update, full fidelity
    Coarse,     // reduced frequency
    Aggregate,  // analytic/aggregate evolution (distant)
    Sleeping,   // parked entirely
};

// One simulation budget tier (data-driven). Pure data — the same contract
// shape as AbilityDefinition/VehicleAsset: versioned JSON, all-or-nothing,
// bit-exact round-trip.
struct SimulationLodTier {
    // Unique tier name ("full", "coarse", "aggregate", "sleep").
    std::string name;
    SimulationLodMode mode{ SimulationLodMode::Full };
    // The lowest relevance that selects this tier. Tiers must be sorted by
    // minRelevance DESCENDING (the first tier with minRelevance <= relevance
    // wins; a relevance below every minRelevance uses the last/cheapest tier).
    float minRelevance{ 0.0f };
    // Seconds between region ticks at this tier. 0 = every update. Lower
    // relevance tiers get LONGER intervals (fewer ticks per second).
    float updateInterval{ 0.0f };
    // Seconds a region may stay in this tier before the runtime considers it
    // idle (informational — the accumulated idleSeconds is persisted; the
    // project may park entities). Must be >= 0.
    float sleepAfterIdle{ 0.0f };
    // CPU budget: max regions at this tier (0 = unlimited). When the tier is
    // over budget, the FARTHEST excess regions fall to the next cheaper tier.
    int maxRegions{ 0 };
    // Seconds between aggregate evolutions for Aggregate-tier regions
    // (0 = every tick of the tier). Must be >= 0.
    float aggregateInterval{ 0.0f };
};

// The full LOD configuration, validated all-or-nothing (never clamped).
struct SimulationLodSpec {
    int version{ 1 };
    // Region cell size in world units. Must be finite and > 0.
    float cellSize{ 16.0f };
    // Distance within which relevance == 1 (full fidelity). Must be finite
    // and >= 0.
    float fullRadius{ 48.0f };
    // Distance at which relevance == 0 (nothing beyond is simulated). Must
    // be finite and > fullRadius. Relevance falls linearly between the two.
    float falloffRadius{ 320.0f };
    // Length of a world day in seconds (world clock). Must be finite and > 0.
    float dayLengthSeconds{ 240.0f };
    // Days per season (4 seasons: Spring/Summer/Autumn/Winter). Must be >= 1.
    int daysPerSeason{ 30 };

    std::vector<SimulationLodTier> tiers;

    // All-or-nothing: refuses bad version, non-positive cellSize, inverted
    // radii, non-positive day length, empty tiers, empty/duplicate tier
    // names, non-descending minRelevance order, non-finite/negative
    // intervals, negative budgets.
    bool validate(std::string& errorOut) const;
    bool load_from_json(const std::string& jsonText, std::string& errorOut);
    std::string to_json() const;
};

// A world region (one grid cell) and its runtime LOD state. Caller-owned and
// explicit: the caller creates regions (add_region), the runtime advances the
// tier/frequency/aggregate fields through update().
struct SimulationRegionState {
    std::int64_t cellX{ 0 };
    std::int64_t cellZ{ 0 };
    // Current tier index (into the spec's tiers).
    std::size_t tier{ 0 };
    // Continuous seconds this region has spent in the Sleeping tier (reset on
    // wake). Informational and persisted.
    float idleSeconds{ 0.0f };
    // Seconds accumulated since the last region tick (frequency gate).
    float elapsedSinceTick{ 0.0f };
    // Seconds accumulated toward the aggregate interval.
    float aggregateAccumulator{ 0.0f };
    // Deterministic aggregate evolution state (Aggregate tier). Evolved by a
    // pure seasonal model in update(); handed over on Aggregate -> Full/Coarse
    // transitions so the region resumes where the analytic model left it.
    float population{ 0.0f };
    float resources{ 0.0f };
    // Region tags ("forest", "arid", "mountain") — affect the seasonal
    // climate model (moisture/temperature offsets). The runtime never
    // interprets tag meaning.
    std::vector<std::string> tags;
};

// Deterministic world clock. tick and totalSeconds advance every update;
// timeOfDay/day/season are derived from totalSeconds/dayLengthSeconds/
// daysPerSeason. All fields are persisted verbatim (bit-exact round-trip).
struct SimulationWorldClock {
    std::uint64_t tick{ 0 };
    float totalSeconds{ 0.0f };
    float timeOfDay{ 0.0f };  // [0, 1) — fraction of the day
    std::uint64_t day{ 0 };
    int season{ 0 };          // 0 = Spring, 1 = Summer, 2 = Autumn, 3 = Winter
};

// A scheduled world event (persisted). The runtime fires it at the exact
// tick, emits EventDue and marks it fired.
struct SimulationScheduledEvent {
    std::uint64_t tickIndex{ 0 };
    std::int64_t cellX{ 0 };
    std::int64_t cellZ{ 0 };
    std::string payload;  // project-defined
    bool fired{ false };
};

// The caller-owned simulation LOD state (explicit; never hidden in the
// adapter). Persisted bit-exactly by serialize_state/deserialize_state.
struct SimulationLodState {
    int version{ 1 };
    SimulationWorldClock clock;
    std::vector<SimulationRegionState> regions;
    std::vector<SimulationScheduledEvent> scheduledEvents;
    // Network budget: region keys ("cellX,cellZ") that ticked, changed tier or
    // aggregate-updated this frame. Recomputed every update; not persisted.
    std::vector<std::string> dirtyRegionKeys;
};

// An emitted decision the project applies to its world.
struct SimulationLodEvent {
    enum class Kind : std::uint8_t {
        RegionTierChanged,  // fromTier -> toTier (carries aggregate handoff)
        RegionSlept,        // entered the Sleeping tier (idleSeconds starts)
        RegionWoken,        // left the Sleeping tier (idleSeconds resets)
        RegionTick,         // this region should tick this frame (Full/Coarse)
        AggregateUpdate,    // aggregate evolution ran (population/resources)
        EventDue,           // a scheduled event fired (payload)
    };
    Kind kind{ Kind::RegionTierChanged };
    std::int64_t cellX{ 0 };
    std::int64_t cellZ{ 0 };
    std::size_t fromTier{ 0 };
    std::size_t toTier{ 0 };
    // Aggregate handoff (RegionTierChanged into/out of Aggregate, and
    // AggregateUpdate): the current counters the project applies.
    float population{ 0.0f };
    float resources{ 0.0f };
    std::string payload;  // EventDue
};

// Seasonal climate of a region (deterministic pure function of the world
// clock + region cell + tags). growth drives the aggregate evolution.
struct SimulationClimate {
    float temperature{ 0.0f };
    float moisture{ 0.0f };
    float growth{ 0.0f };  // 0..1 — seasonal growing factor
};

class ISimulationLod {
public:
    virtual ~ISimulationLod() = default;

    // ---- configuration (data-driven, all-or-nothing) ----
    // Validates the spec (never clamps) and makes it the active configuration.
    // update()/select_tier()/relevance()/climate() refuse while no valid spec
    // is set.
    virtual bool set_spec(const SimulationLodSpec& spec, std::string& errorOut) = 0;
    virtual const SimulationLodSpec* spec() const = 0;

    // ---- pure queries (from the active spec) ----
    // Maps relevance in [0, 1] to a tier index: the first tier (descending
    // minRelevance) with minRelevance <= relevance; the last tier below every
    // threshold. Refuses relevance outside [0, 1] (all-or-nothing).
    virtual bool select_tier(float relevance, std::size_t& tierIndexOut,
                             std::string& errorOut) const = 0;
    // The aligned region cell of a world position: floor(world / cellSize) —
    // the same canonical cell for the same position, grid aligned to the
    // origin (the IMultiScaleStreaming convention).
    virtual void region_cell(float worldX, float worldZ,
                             std::int64_t& cellX, std::int64_t& cellZ) const = 0;
    // Relevance of a distance: 1 inside fullRadius, 0 at/beyond falloffRadius,
    // linear between. Deterministic pure function of the spec.
    virtual float relevance(float distance) const = 0;
    // Stable region key "cellX,cellZ" (the dirty-set / network identity).
    virtual bool region_key(std::int64_t cellX, std::int64_t cellZ,
                            std::string& out) const = 0;

    // ---- state management (caller-owned) ----
    // Creates a region at the given cell (tier seeded to the cheapest/fullest
    // default and immediately re-tiered by the next update). Refuses a
    // duplicate cell (all-or-nothing).
    virtual bool add_region(SimulationLodState& state, std::int64_t cellX,
                            std::int64_t cellZ,
                            const std::vector<std::string>& tags,
                            std::string& errorOut) = 0;
    // Removes a region and its scheduled events.
    virtual bool remove_region(SimulationLodState& state, std::int64_t cellX,
                               std::int64_t cellZ) = 0;
    // Schedules an event to fire at tickIndex (must be >= the current clock
    // tick — the past cannot be scheduled). Payload must be non-empty.
    virtual bool schedule_event(SimulationLodState& state,
                                std::uint64_t tickIndex, std::int64_t cellX,
                                std::int64_t cellZ, const std::string& payload,
                                std::string& errorOut) = 0;

    // ---- the simulation step ----
    // Advances the world clock (tick/totalSeconds -> timeOfDay/day/season),
    // re-tiers every region by its distance to the focus, ticks regions whose
    // tier interval elapsed (Full/Coarse), sleeps/wakes regions crossing the
    // Sleeping tier, evolves Aggregate regions through the deterministic
    // seasonal model, fires due scheduled events, enforces the per-tier
    // region budgets (overflow falls to the next cheaper tier) and rebuilds
    // the dirty-region set. Emits every decision into `events` (cleared at
    // entry; deterministic order — regions iterated by (cellX, cellZ)). Same
    // state + same (focus, dt) sequence reproduces bit-exactly. Refuses
    // non-finite/negative dt or an invalid state version (all-or-nothing).
    virtual bool update(SimulationLodState& state, float focusX, float focusZ,
                        float dtSeconds, std::vector<SimulationLodEvent>& events,
                        std::string& errorOut) = 0;

    // Deterministic seasonal climate of a region: temperature/moisture from
    // the season + a splitmix64 hash of (cellX, cellZ, season, tags), growth
    // from temperature and moisture. Pure — no state mutation.
    virtual bool climate(const SimulationLodState& state,
                         const SimulationRegionState& region,
                         SimulationClimate& out) const = 0;

    // ---- persistence / replication (bit-exact %.9g, all-or-nothing) ----
    // Serializes the full state (clock, regions, scheduled events). Refuses
    // an invalid state version. deserialize is all-or-nothing: a malformed
    // document leaves `out` untouched.
    virtual bool serialize_state(const SimulationLodState& state,
                                 std::string& out, std::string& errorOut) const = 0;
    virtual bool deserialize_state(const std::string& data,
                                   SimulationLodState& out,
                                   std::string& errorOut) const = 0;
};

// The only implementation (src/engine/sdk/SimulationLod.cpp).
std::unique_ptr<ISimulationLod> create_simulation_lod();

}  // namespace simulation
}  // namespace engine
