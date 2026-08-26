#pragma once

// IActionMap (agente 4 §1 item 8): the PUBLIC input-mapping contract. Gameplay
// must NOT read raw keys/buttons — it binds ACTIONS to INPUTS through a
// data-driven, rebindable map that survives save/load and works across
// keyboard/mouse/gamepad/multi-device. The engine has NO input surface today
// (grep action_map/rebind/gamepad in src/engine/public = zero); this contract
// turns input into DATA + a pure deterministic resolution runtime:
//   - ACTIONS: a named gameplay action ("jump", "move_forward", "fire") with a
//     set of BINDINGS. A binding names an INPUT SOURCE (keyboard/mouse/gamepad/
//     touch), a physical input (a key/mouse button/gamepad button/axis name),
//     and a scale/axis index (so an analog axis maps to -1..+1 or a button to
//     0/1).
//   - RESOLUTION: poll(source, input, value, dt) turns a raw input event into
//     an action ACTIVATION (which actions became active and with what value).
//     The map decides, the project applies movement/ability. PURE: the map
//     only DECIDES (reports the resolved activation set), never reads a real
//     device.
//   - REBINDING: set_binding(action, slot, binding) rebinds at runtime with
//     all-or-nothing validation. A rebind that collides with another action's
//     binding is refused with a CONFLICT diagnostic (never silently
//     overwritten) unless `override` is true — the project owns the
//     conflict-resolution UI policy.
//   - CONFLICT DETECTION: conflicts(action) lists every binding shared with
//     another action (the editor/rebind UI consumes it).
//   - PERSISTENCE: JSON versioned, bit-exact round-trip (load_from_json/
//     to_json), all-or-nothing on malformed input.
//   - MULTI-DEVICE: a device-id field tags each binding's source, so two
//     gamepads / a keyboard and a pad coexist without a "last writer wins"
//     device singleton. The map is device-agnostic data.
//
// Deterministic and headless: same map + same (source, input, value, dt)
// sequence -> identical activation streams, bit-exact. Self-contained (std
// only). The SDK adapter (src/engine/sdk/ActionMap.cpp) is the ONLY TU with
// behavior.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace input {

// Where a binding comes from. The map is device-agnostic — a device id string
// ("keyboard-0", "gamepad-1") is a caller-owned tag, never a global singleton.
enum class InputSource : std::uint8_t { Keyboard, Mouse, Gamepad, Touch, Other };

// One physical input binding: a source + an input name + optional scale/axis.
struct InputBinding {
    InputSource source{ InputSource::Keyboard };
    // Device tag (caller-owned; "" = the default device of that source).
    std::string device;
    // Physical input name: key ("Space", "KeyW"), mouse button ("MouseLeft"),
    // gamepad button ("PadA") or axis ("AxisLX", "AxisLY").
    std::string input;
    // For analog axes: the axis index (a gamepad with two sticks). 0 = first.
    int axis{ 0 };
    // Multiplier applied to the raw value (-1 flips an axis; >1 amplifies).
    double scale{ 1.0 };
    // Deadzone applied to |raw value| below which the input reads as 0.
    double deadzone{ 0.0 };

    bool operator==(const InputBinding& other) const {
        return source == other.source && device == other.device &&
               input == other.input && axis == other.axis &&
               scale == other.scale && deadzone == other.deadzone;
    }
    bool operator!=(const InputBinding& other) const { return !(*this == other); }
};

// One action's bindings (an action fires when ANY of its bindings is active).
struct ActionBinding {
    std::string action;
    std::vector<InputBinding> bindings;
};

// The full data-driven map, validated all-or-nothing (never clamped).
struct ActionMapSpec {
    int version{ 1 };
    std::vector<ActionBinding> actions;

    // All-or-nothing: refuses bad version, empty/duplicate action names, a
    // binding with an empty input name, non-finite/negative scale or deadzone
    // > 1, or two actions sharing the SAME binding (hard conflict — the spec
    // is the canonical source; runtime rebinds may override).
    bool validate(std::string& errorOut) const;
    bool load_from_json(const std::string& jsonText, std::string& errorOut);
    std::string to_json() const;
};

// A resolved activation: which action fired and with what value.
struct ActionActivation {
    std::string action;
    // Final value after scale + deadzone (buttons 0/1; axes -1..+1).
    double value{ 0.0 };
    // The binding that produced this activation (for UI/debug).
    std::size_t bindingIndex{ 0 };
};

// Deterministic input-map runtime. PURE — it never reads a device; the
// project feeds raw events via poll() and applies the returned activations.
class IActionMap {
public:
    virtual ~IActionMap() = default;

    // Feeds one raw input event and returns the actions it activated. `value`
    // is the raw reading (buttons 0/1, axes -1..+1). `dt` is ignored by the
    // resolution (kept for future chord/hold semantics) — must be finite and
    // >= 0. Deterministic: the result depends only on (input, value, map).
    virtual std::vector<ActionActivation> poll(InputSource source,
                                               const std::string& device,
                                               const std::string& input,
                                               int axis, double value,
                                               double dt) = 0;

    // Rebinds one binding slot of an action (0-based). `override=false`
    // refuses a binding already used by ANOTHER action (conflict, never
    // silently overwritten). All-or-nothing: a refused rebind leaves the map
    // untouched.
    virtual bool rebind(const std::string& action, std::size_t slot,
                        const InputBinding& binding, bool overrideBinding,
                        std::string& errorOut) = 0;

    // Lists every (action, slot) whose binding is also used by another action.
    virtual std::vector<std::pair<std::string, std::size_t>> conflicts()
        const = 0;

    virtual const ActionMapSpec& spec() const = 0;
};

// Parses+validates a spec and compiles it (rejected -> nullptr + errorOut).
std::unique_ptr<IActionMap> create_action_map(const ActionMapSpec& spec,
                                              std::string& errorOut);

}  // namespace input
}  // namespace engine
