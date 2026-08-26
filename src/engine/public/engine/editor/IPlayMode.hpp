#pragma once

// IPlayMode (agente 2 §B): the PUBLIC, unambiguous editor play-state machine.
// The editor's play/stop header must never accept an invalid transition:
//   Edit  --play()-->   Play       Play   --pause()-->  Pause
//   Edit  --simulate()-> Simulate  Pause  --resume()--> Play
//   Play  --stop()-->   Edit       Pause  --stop()-----> Edit
//   Simulate--pause()-> Pause      Pause  --step()-----> (stays Pause, one
//   Simulate--stop()--> Edit              --pause()---->  frame consumed)
//   Pause  --stop()-->  Edit
// Everything else is REFUSED with a reason and leaves the state untouched
// (all-or-nothing). Editor and runtime stay SEPARATE: the machine only owns
// the state; the editor decides what runs in Play vs Edit (scene cloning,
// input routing, ticking) — this contract guarantees the mode bookkeeping is
// never ambiguous and is fully testable headless.
//   - UNEQUIVOCAL: each command validates the current state; invalid
//     transitions are refused (no silent no-ops, no illegal jumps).
//   - SEPARATION: state() (Edit vs runtime) and is_runtime() are the single
//     source of truth for "is the editor playing right now?".
//   - DETERMINISM: pure state machine, no clocks/RNG/globals; same sequence
//     of commands -> identical state, bit-exact.
//   - OBSERVABLE: to_json() serializes {state, runtime, step} deterministically
//     (editor exposes it via the Control API, e.g. GET /play-mode).
//
// Self-contained (std only). The SDK adapter
// (src/engine/sdk/PlayMode.cpp) is the ONLY TU with behavior.

#include <cstdint>
#include <memory>
#include <string>

namespace engine {
namespace editor {

enum class PlayModeState : std::uint8_t { Edit, Play, Pause, Simulate };

// The play-mode snapshot (observable via the editor's Control API).
struct PlayModeSnapshot {
    PlayModeState state{ PlayModeState::Edit };
    bool runtime{ false };  // true while Play/Simulate (runtime world active)
    bool paused{ false };
    bool simulating{ false };
    std::uint64_t steps{ 0 };  // frames advanced via step() while paused

    bool operator==(const PlayModeSnapshot& other) const {
        return state == other.state && runtime == other.runtime &&
               paused == other.paused && simulating == other.simulating &&
               steps == other.steps;
    }
    bool operator!=(const PlayModeSnapshot& other) const {
        return !(*this == other);
    }
};

class IPlayMode {
public:
    virtual ~IPlayMode() = default;

    virtual PlayModeState state() const = 0;
    virtual PlayModeSnapshot snapshot() const = 0;

    // true while a runtime world is active (Play or Simulate).
    virtual bool is_runtime() const = 0;

    // Command set — every command validates the current state and REFUSES
    // invalid transitions (returns false + reason, state untouched).
    //   play():     Edit -> Play            (refused anywhere else)
    //   simulate(): Edit -> Simulate        (refused anywhere else)
    //   pause():    Play|Simulate -> Pause  (refused otherwise)
    //   resume():   Pause -> Play           (refused otherwise)
    //   stop():     any runtime state -> Edit (no-op in Edit)
    //   step():     Pause only — consumes one frame (steps++), stays Pause
    virtual bool play(std::string& errorOut) = 0;
    virtual bool simulate(std::string& errorOut) = 0;
    virtual bool pause(std::string& errorOut) = 0;
    virtual bool resume(std::string& errorOut) = 0;
    virtual bool stop(std::string& errorOut) = 0;
    virtual bool step(std::string& errorOut) = 0;

    // Deterministic JSON snapshot ({"state":"play","runtime":true,...}).
    virtual std::string to_json() const = 0;
};

// Creates the machine (always Edit at birth). Never returns nullptr.
std::unique_ptr<IPlayMode> create_play_mode();

}  // namespace editor
}  // namespace engine
