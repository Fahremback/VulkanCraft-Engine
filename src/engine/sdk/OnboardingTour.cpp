// IOnboardingTour adapter — the ONLY TU with behavior
// (engine::editor::IOnboardingTour). Pure step machine: no clocks, no RNG,
// no globals. Every command validates the current state and refuses invalid
// commands without mutating (all-or-nothing).
//
//   Idle    --start(tour)-->  Running   Running --next()--->  Running|Done
//   Running --skip()------->  Running   Running --complete()> Done
//   Running --reset()------>  Running   any     --dismiss()-> Idle
//   Done    --dismiss()---->  Idle
//
// Invalid commands return false + a reason and leave the machine untouched.

#include "engine/editor/IOnboardingTour.hpp"

#include <sstream>

namespace engine {
namespace editor {

namespace {

const char* state_name(TourState s) {
    switch (s) {
        case TourState::Idle: return "idle";
        case TourState::Running: return "running";
        case TourState::Done: return "done";
    }
    return "idle";
}

}  // namespace

namespace {

class OnboardingTourImpl final : public IOnboardingTour {
public:
    TourState state() const override { return m_state; }

    OnboardingSnapshot snapshot() const override {
        OnboardingSnapshot snap;
        snap.state = m_state;
        snap.tour = m_tourId;
        snap.cursor = m_cursor;
        snap.done = m_done;
        snap.skipped = m_skipped;
        snap.total = static_cast<std::uint64_t>(m_steps.size());
        if (m_state == TourState::Running && !m_steps.empty() &&
            m_cursor >= 1 && m_cursor <= m_steps.size()) {
            snap.currentStep = m_steps[m_cursor - 1].id;
        }
        return snap;
    }

    bool start(const std::string& tourId,
               const std::vector<TourStepDef>& steps) override {
        if (m_state != TourState::Idle) {
            m_lastReason = "start requires Idle state (current: " +
                           std::string(state_name(m_state)) + ")";
            return false;
        }
        if (steps.empty()) {
            m_lastReason = "start requires at least one step";
            return false;
        }
        m_tourId = tourId;
        m_steps = steps;
        m_cursor = 1;
        m_done = 0;
        m_skipped = 0;
        m_state = TourState::Running;
        m_lastReason.clear();
        return true;
    }

    bool next() override {
        if (m_state != TourState::Running) {
            m_lastReason = "next requires Running state (current: " +
                           std::string(state_name(m_state)) + ")";
            return false;
        }
        ++m_done;
        return advance();
    }

    bool skip() override {
        if (m_state != TourState::Running) {
            m_lastReason = "skip requires Running state (current: " +
                           std::string(state_name(m_state)) + ")";
            return false;
        }
        ++m_skipped;
        return advance();
    }

    bool complete() override {
        if (m_state != TourState::Running) {
            m_lastReason = "complete requires Running state (current: " +
                           std::string(state_name(m_state)) + ")";
            return false;
        }
        // Mark any remaining steps as skipped, finish the tour.
        const std::uint64_t remaining = m_steps.size() - m_cursor + 1;
        m_skipped += remaining;
        m_cursor = static_cast<std::uint64_t>(m_steps.size());
        m_state = TourState::Done;
        m_lastReason.clear();
        return true;
    }

    bool reset() override {
        if (m_state != TourState::Running) {
            m_lastReason = "reset requires Running state (current: " +
                           std::string(state_name(m_state)) + ")";
            return false;
        }
        m_cursor = 1;
        m_lastReason.clear();
        return true;
    }

    void dismiss() override {
        m_tourId.clear();
        m_steps.clear();
        m_cursor = 0;
        m_done = 0;
        m_skipped = 0;
        m_state = TourState::Idle;
        m_lastReason.clear();
    }

    std::string to_json() const override {
        std::ostringstream os;
        os << "{\"state\":\"" << state_name(m_state) << "\",\"tour\":\""
           << m_tourId << "\",\"cursor\":" << m_cursor << ",\"done\":" << m_done
           << ",\"skipped\":" << m_skipped << ",\"total\":"
           << m_steps.size() << ",\"current\":\"" << snapshot().currentStep
           << "\"}";
        return os.str();
    }

private:
    // Advance the cursor; finishes the tour when past the last step.
    // Returns true (the command itself succeeded) in both cases.
    bool advance() {
        if (m_cursor < m_steps.size()) {
            ++m_cursor;
            m_lastReason.clear();
            return true;
        }
        m_state = TourState::Done;
        m_lastReason.clear();
        return true;
    }

    TourState m_state{ TourState::Idle };
    std::string m_tourId;
    std::vector<TourStepDef> m_steps;
    std::uint64_t m_cursor{ 0 };
    std::uint64_t m_done{ 0 };
    std::uint64_t m_skipped{ 0 };
    std::string m_lastReason;
};

}  // namespace

std::unique_ptr<IOnboardingTour> create_onboarding_tour() {
    return std::make_unique<OnboardingTourImpl>();
}

}  // namespace editor
}  // namespace engine
