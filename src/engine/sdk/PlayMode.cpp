// IPlayMode adapter — the ONLY TU with behavior (engine::editor::IPlayMode).
// Pure state machine: no clocks, no RNG, no globals. Every command validates
// the current state and refuses invalid transitions without mutating.
//
//   Edit --play()--> Play    Play --pause()--> Pause   Pause --step()--> Pause
//   Edit --simulate()> Sim   Play --stop()-->  Edit    Pause --pause()--> Play
//   Sim  --pause()--> Pause  Pause--resume()-->Play    Pause --stop()--> Edit
//   Sim  --stop()--> Edit    Pause--stop()---> Edit    Sim/Play all others refused
//
// Invalid transitions return false + a reason and leave the state untouched.

#include "engine/editor/IPlayMode.hpp"

#include <sstream>

namespace engine {
namespace editor {

namespace {

const char* state_name(PlayModeState s) {
    switch (s) {
        case PlayModeState::Edit: return "edit";
        case PlayModeState::Play: return "play";
        case PlayModeState::Pause: return "pause";
        case PlayModeState::Simulate: return "simulate";
    }
    return "edit";
}

}  // namespace

namespace {

class PlayModeImpl final : public IPlayMode {
public:
    PlayModeState state() const override { return m_state; }

    PlayModeSnapshot snapshot() const override {
        PlayModeSnapshot snap;
        snap.state = m_state;
        snap.runtime = (m_state == PlayModeState::Play ||
                        m_state == PlayModeState::Pause ||
                        m_state == PlayModeState::Simulate);
        snap.paused = (m_state == PlayModeState::Pause);
        snap.simulating = (m_state == PlayModeState::Simulate);
        snap.steps = m_steps;
        return snap;
    }

    bool is_runtime() const override {
        return m_state != PlayModeState::Edit;
    }

    bool play(std::string& errorOut) override {
        if (m_state != PlayModeState::Edit) {
            errorOut = "play requires Edit state (current: " +
                       std::string(state_name(m_state)) + ")";
            return false;
        }
        m_state = PlayModeState::Play;
        return true;
    }

    bool simulate(std::string& errorOut) override {
        if (m_state != PlayModeState::Edit) {
            errorOut = "simulate requires Edit state (current: " +
                       std::string(state_name(m_state)) + ")";
            return false;
        }
        m_state = PlayModeState::Simulate;
        return true;
    }

    bool pause(std::string& errorOut) override {
        if (m_state == PlayModeState::Play || m_state == PlayModeState::Simulate) {
            m_state = PlayModeState::Pause;
            return true;
        }
        if (m_state == PlayModeState::Pause) {
            // pause() while paused is the classic play/pause toggle; keep it
            // unambiguous by resuming (matches the editor's toggle button).
            m_state = PlayModeState::Play;
            return true;
        }
        errorOut = "pause requires Play/Simulate state (current: " +
                   std::string(state_name(m_state)) + ")";
        return false;
    }

    bool resume(std::string& errorOut) override {
        if (m_state != PlayModeState::Pause) {
            errorOut = "resume requires Pause state (current: " +
                       std::string(state_name(m_state)) + ")";
            return false;
        }
        m_state = PlayModeState::Play;
        return true;
    }

    bool stop(std::string& errorOut) override {
        if (m_state == PlayModeState::Edit) {
            return true;  // idempotent no-op in Edit
        }
        m_state = PlayModeState::Edit;
        m_steps = 0;
        return true;
    }

    bool step(std::string& errorOut) override {
        if (m_state != PlayModeState::Pause) {
            errorOut = "step requires Pause state (current: " +
                       std::string(state_name(m_state)) + ")";
            return false;
        }
        ++m_steps;
        return true;
    }

    std::string to_json() const override {
        const PlayModeSnapshot snap = snapshot();
        std::ostringstream out;
        out << "{\"state\":\"" << state_name(snap.state) << "\","
            << "\"runtime\":" << (snap.runtime ? "true" : "false") << ","
            << "\"paused\":" << (snap.paused ? "true" : "false") << ","
            << "\"simulating\":" << (snap.simulating ? "true" : "false") << ","
            << "\"steps\":" << snap.steps << "}";
        return out.str();
    }

private:
    PlayModeState m_state{ PlayModeState::Edit };
    std::uint64_t m_steps{ 0 };
};

}  // namespace

std::unique_ptr<IPlayMode> create_play_mode() {
    return std::make_unique<PlayModeImpl>();
}

}  // namespace editor
}  // namespace engine
