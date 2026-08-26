#pragma once

// IOnboardingTour (agente 2 §C): the PUBLIC, deterministic onboarding/tutorial
// step machine. The editor's "welcome / first-run / tutorial" surfaces must
// never drift: a tour is an ordered list of steps; each step is either
// completed, skipped, or (implicitly) pending. The machine is a pure state
// machine over {Idle, Running, Done} with the tour owning the step cursor:
//   Idle    --start(tour)-->  Running (cursor = first step)
//   Running --next()------->  Running (cursor advances; at last step -> Done)
//   Running --skip()------->  Running (marks current skipped, advances)
//   Running --complete()--->  Done    (marks current completed, finishes)
//   Running --reset()------>  Running (cursor back to first step)
//   any     --dismiss()---->  Idle    (tour abandoned, stays Idle)
// Everything else is REFUSED with a reason and leaves the machine untouched
// (all-or-nothing). This contract owns only the tour state; the visual shell
// decides what each step highlights (panels, dialogs) — the machine is fully
// testable headless and bit-exact.
//   - UNEQUIVOCAL: each command validates the current state; invalid commands
//     are refused (no silent no-ops).
//   - DETERMINISM: pure state machine, no clocks/RNG/globals; same sequence
//     of commands -> identical state, bit-exact.
//   - OBSERVABLE: to_json() serializes {state, tour, cursor, done, skipped,
//     total} deterministically (editor exposes it via the Control API, e.g.
//     GET /onboarding).
//
// Self-contained (std only). The SDK adapter
// (src/engine/sdk/OnboardingTour.cpp) is the ONLY TU with behavior.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace editor {

enum class TourState : std::uint8_t { Idle, Running, Done };

// One tutorial step (id stable, title/copy for the shell, optional target
// panel/action so the highlight can be data-driven).
struct TourStepDef {
    std::string id;
    std::string title;
    std::string copy;
    std::string target;  // e.g. "panels/Inspector", "command/publish" — free
                         // form; the shell maps it to a highlight.
    bool operator==(const TourStepDef& other) const {
        return id == other.id && title == other.title && copy == other.copy &&
               target == other.target;
    }
};

// The onboarding snapshot (observable via the editor's Control API).
struct OnboardingSnapshot {
    TourState state{ TourState::Idle };
    std::string tour;                 // id of the active/completed tour
    std::uint64_t cursor{ 0 };        // 1-based index of the current step
    std::uint64_t done{ 0 };          // steps marked completed
    std::uint64_t skipped{ 0 };       // steps marked skipped
    std::uint64_t total{ 0 };         // total steps in the tour
    std::string currentStep;          // id of the current step ("" when idle)

    bool operator==(const OnboardingSnapshot& other) const {
        return state == other.state && tour == other.tour &&
               cursor == other.cursor && done == other.done &&
               skipped == other.skipped && total == other.total &&
               currentStep == other.currentStep;
    }
    bool operator!=(const OnboardingSnapshot& other) const {
        return !(*this == other);
    }
};

class IOnboardingTour {
public:
    virtual ~IOnboardingTour() = default;

    virtual TourState state() const = 0;
    virtual OnboardingSnapshot snapshot() const = 0;

    // Starts a tour. REFUSED (returns false, machine untouched) when the
    // machine is not Idle or the tour has no steps. On success the cursor is
    // at step 1.
    virtual bool start(const std::string& tourId,
                       const std::vector<TourStepDef>& steps) = 0;

    // Marks the current step completed and advances. REFUSED when Idle or
    // Done. Advancing past the last step finishes the tour (state -> Done).
    virtual bool next() = 0;

    // Marks the current step skipped and advances. REFUSED when Idle or Done.
    virtual bool skip() = 0;

    // Finishes the tour immediately: marks any remaining steps as skipped and
    // moves to Done. REFUSED when Idle or Done.
    virtual bool complete() = 0;

    // Returns to the first step (skipped/completed counts kept). REFUSED when
    // Idle or Done.
    virtual bool reset() = 0;

    // Abandons the tour: back to Idle, cursor cleared. Always succeeds
    // (idempotent when already Idle).
    virtual void dismiss() = 0;

    // Deterministic JSON of the snapshot (bit-exact, no floats).
    virtual std::string to_json() const = 0;
};

// Factory: the SDK adapter is the only TU with behavior.
std::unique_ptr<IOnboardingTour> create_onboarding_tour();

}  // namespace editor
}  // namespace engine
