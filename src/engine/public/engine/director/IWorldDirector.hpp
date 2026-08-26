#pragma once

// IWorldDirector — FALTANTES differential "WorldDirector": EVENT SELECTION BY
// RULES, UTILITY, COHERENCE AND DIVERSITY (META section 32 — engine-own code;
// nothing in external/solutions resolves it).
//
// The world always has MORE candidate events than it can run (quests, raids,
// weather fronts, spawn waves, scheduled content...). Something must decide
// WHICH event fires NEXT — and the decision must be reproducible: the same
// candidates, world state and clock produce the SAME selection, bit-exact, on
// every machine and every run.
//
// This director is the pure decision engine for that choice:
//   - RULES (data-driven, all-or-nothing): each candidate declares the tags
//     the world must have (requiresAll) and must NOT have (excludesAny), a
//     cooldown between fires, a concurrency cap, a daily fire limit and a
//     diversity category.
//   - UTILITY: an eligible candidate's score = baseUtility * weight *
//     (0.5 + 0.5 * urgency) - diversityPenalty * (1 - urgency), where urgency
//     is the fraction of the recency window elapsed since the last fire (1
//     for never-fired). Fixed arithmetic order — deterministic.
//   - COHERENCE: the world state (current tags + clock) gates eligibility
//     (requiresAll / excludesAny), and the concurrency cap stops the director
//     from stacking incoherent repeats of the same event.
//   - DIVERSITY: a candidate that just fired pays the diversity penalty
//     (its recency factor is 1); one that has not fired for a full window
//     pays none — the selection spreads across candidates instead of
//     repeating the same event.
//   - SELECT: eligible candidates are scored, sorted by (utility DESC, id
//     ASC — deterministic), and the top maxPerTick are chosen. The caller-
//     owned selection states are advanced deterministically for the chosen
//     events (lastFireTick, fireCount, firesThisDay, dayOfLastFire,
//     selectedCount). The chosen events are returned with their scores and
//     the eligibility reasons (deterministic diagnostics).
//
// The runtime is PURE and DETERMINISTIC — it never touches a concrete world;
// it only DECIDES and REPORTS (the project fires the returned events). State
// is caller-owned and explicit (the IAnimationLod / ISimulationLod /
// IMacroMicroReconciler pattern). Same spec + same (world, selections) ->
// identical selections and identical event streams, bit-exact, across
// instances. All refusals are all-or-nothing with a diagnostic.
// Self-contained (std).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace director {

// One candidate world event (data-driven; JSON versioned, all-or-nothing).
struct WorldEventCandidate {
    // Unique id ("raid", "storm", "festival", ...). Non-empty.
    std::string id;
    // Coherence: every tag here must be present in the world state.
    std::vector<std::string> requiresAll;
    // Coherence: NONE of these tags may be present in the world state.
    std::vector<std::string> excludesAny;
    // Base utility in [0, 1] (the intrinsic worth of this event).
    float baseUtility{ 0.5f };
    // Weight multiplier (>= 0, finite). 0 disables the candidate.
    float weight{ 1.0f };
    // Minimum ticks between two fires (>= 0). Never-fired candidates are
    // always eligible.
    std::uint64_t cooldownTicks{ 0 };
    // Max concurrent instances (>= 1). The caller owns activeCount in the
    // selection state; the director refuses candidates at the cap.
    std::uint64_t maxConcurrent{ 1 };
    // Max fires per world day (>= 0; 0 = unlimited).
    std::uint64_t maxPerDay{ 0 };
    // Diversity group ("combat", "weather", "social", ...). Informational —
    // the diversity mechanism works through the recency penalty, not groups.
    std::string category;
};

// The full director configuration (validated all-or-nothing, never clamped).
struct DirectorSpec {
    int version{ 1 };
    std::vector<WorldEventCandidate> candidates;
    // How many events may be selected per select() call (0 = all eligible).
    int maxPerTick{ 1 };
    // Diversity penalty subtracted per recent fire (recency factor 1).
    // In [0, 1].
    float diversityPenalty{ 0.25f };
    // The urgency window in ticks (>= 1): a candidate that has not fired for
    // this many ticks is fully urgent (recency factor 0, no penalty).
    std::uint64_t recencyWindow{ 1000 };
    // World ticks per day (>= 1) — drives the daily fire limit.
    std::uint64_t dayLengthTicks{ 2400 };

    // All-or-nothing: refuses bad version, empty/duplicate candidate ids,
    // baseUtility outside [0,1], negative/non-finite weight, negative
    // cooldown, maxConcurrent < 1, maxPerDay < 0, diversityPenalty outside
    // [0,1], recencyWindow < 1, dayLengthTicks < 1, maxPerTick < 0.
    bool validate(std::string& errorOut) const;
    bool load_from_json(const std::string& jsonText, std::string& errorOut);
    std::string to_json() const;
};

// The world the director reads: current clock + current world-state tags
// (the coherence surface). Caller-owned; read-only during select().
struct DirectorWorldState {
    int version{ 1 };
    std::uint64_t tick{ 0 };
    // Current world tags ("peace", "war", "storm", "festival", ...).
    std::vector<std::string> tags;
};

// Per-candidate runtime state (caller-owned; persisted bit-exactly). The
// caller advances activeCount as it starts/finishes event instances; every
// other field is advanced by select() deterministically.
struct EventSelectionState {
    std::string id;  // must match a spec candidate id
    std::uint64_t lastFireTick{ 0 };
    std::uint64_t fireCount{ 0 };
    std::uint64_t selectedCount{ 0 };  // total times selected (diagnostics)
    std::uint64_t firesThisDay{ 0 };   // fires in the current world day
    std::uint64_t dayOfLastFire{ 0 };  // day (tick / dayLengthTicks)
    std::uint64_t activeCount{ 0 };    // caller-owned concurrent instances
};

// One selection decision (returned by select()).
struct DirectorSelection {
    std::string eventId;
    float utility{ 0.0f };   // the computed score (bit-exact deterministic)
    // Deterministic diagnostic: "eligible" or the FIRST failing gate
    // ("missing_tags", "excluded_tag", "cooldown", "concurrency_limit",
    // "daily_limit", "disabled").
    std::string reason;
};

class IWorldDirector {
public:
    virtual ~IWorldDirector() = default;

    // ---- configuration (data-driven, all-or-nothing) ----
    // Validates the spec (never clamps) and makes it the active
    // configuration. select() refuses while no valid spec is set.
    virtual bool set_spec(const DirectorSpec& spec, std::string& errorOut) = 0;
    virtual bool set_spec_json(const std::string& jsonText,
                               std::string& errorOut) = 0;
    // Canonical deterministic emit (bit-exact round-trip counterpart of
    // set_spec_json; floats as %.9g, std::map ordering).
    virtual std::string spec_to_json() const = 0;
    virtual const DirectorSpec* spec() const = 0;

    // ---- pure queries (from the active spec) ----
    // Whether a candidate is eligible RIGHT NOW (all gates). Deterministic
    // pure function; refuses an unknown candidate id (all-or-nothing).
    virtual bool eligible(const WorldEventCandidate& candidate,
                          const DirectorWorldState& world,
                          const EventSelectionState& selection,
                          std::string& reasonOut) const = 0;
    // The deterministic utility score of a candidate (see the file comment
    // for the formula). Refuses an unknown id or a selection with a
    // mismatched id (all-or-nothing).
    virtual bool candidate_utility(const WorldEventCandidate& candidate,
                                   const DirectorWorldState& world,
                                   const EventSelectionState& selection,
                                   float& utilityOut,
                                   std::string& errorOut) const = 0;

    // ---- the decision ----
    // Scores every eligible candidate, sorts by (utility DESC, id ASC) and
    // selects the top maxPerTick. For each chosen event the caller-owned
    // selection state is advanced deterministically (lastFireTick = tick,
    // fireCount++, selectedCount++, firesThisDay++, dayOfLastFire = day).
    // `out` is cleared at entry and filled in selection order (deterministic).
    // Refuses all-or-nothing (nothing mutated) on an invalid world/selection
    // state or a selection id absent from the spec. Same inputs -> identical
    // `out` and identical state deltas, bit-exact.
    virtual bool select(DirectorWorldState& world,
                        std::vector<EventSelectionState>& selections,
                        std::vector<DirectorSelection>& out,
                        std::string& errorOut) = 0;

    // ---- persistence (bit-exact, all-or-nothing) ----
    // Serializes the selection states. deserialize is all-or-nothing: a
    // malformed document leaves `out` untouched.
    virtual bool serialize_selections(
        const std::vector<EventSelectionState>& selections,
        std::string& out, std::string& errorOut) const = 0;
    virtual bool deserialize_selections(
        const std::string& data, std::vector<EventSelectionState>& out,
        std::string& errorOut) const = 0;
};

// The only implementation (src/engine/sdk/WorldDirector.cpp).
std::unique_ptr<IWorldDirector> create_world_director();

}  // namespace director
}  // namespace engine
