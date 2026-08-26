#pragma once

// IWindowMode (agente 2 §B): the PUBLIC, unambiguous window-mode model for
// the editor shell — Windowed / Borderless / Fullscreen with ONLY valid
// transitions, geometry preservation and monitor bookkeeping. The editor
// currently has NO window-mode model (no fullscreen/borderless code at all);
// this contract is the deterministic core the shell will drive GLFW with:
//   Windowed   --enter_fullscreen(monitor)--> Fullscreen
//   Windowed   --enter_borderless(monitor)--> Borderless
//   Borderless --enter_fullscreen(monitor)--> Fullscreen
//   Borderless --exit()----------------------> Windowed  (geometry restored)
//   Fullscreen --exit()----------------------> Windowed  (geometry restored)
//   Borderless --exit()----------------------> Windowed
// Everything else REFUSED with a reason, state untouched (all-or-nothing).
//   - GEOMETRY: entering a full/borderless mode SNAPSHOTS the windowed
//     geometry (x, y, width, height); exiting restores it. The snapshot is
//     only taken once (re-entering fullscreen after borderless keeps the
//     ORIGINAL windowed geometry).
//   - MONITOR: full/borderless modes track a monitor id (>= 0); Windowed
//     mode has no monitor (tracking stays at the last used, "" none).
//   - RESIZE: set_windowed_geometry() only applies in Windowed mode (refused
//     elsewhere, no mutation); set_size() applies in any mode but must be
//     > 0 (refused otherwise) and updates the CURRENT geometry.
//   - DETERMINISM: pure state machine, no clocks/RNG/globals; same sequence
//     -> identical state and JSON, bit-exact.
//   - OBSERVABLE: to_json() serializes {mode, monitor, x, y, width, height}
//     deterministically (editor exposes it via the Control API, e.g.
//     GET /window-mode).
//
// Self-contained (std only). The SDK adapter
// (src/engine/sdk/WindowMode.cpp) is the ONLY TU with behavior.

#include <cstdint>
#include <memory>
#include <string>

namespace engine {
namespace editor {

enum class WindowModeState : std::uint8_t { Windowed, Borderless, Fullscreen };

// The window-mode snapshot (observable via the editor's Control API).
struct WindowModeSnapshot {
    WindowModeState mode{ WindowModeState::Windowed };
    std::int64_t monitor{ -1 };  // active monitor (>=0 in full/borderless)
    std::int32_t x{ 0 }, y{ 0 };             // windowed geometry
    std::int32_t width{ 1280 }, height{ 720 };
    bool geometrySaved{ false };  // a full/borderless snapshot exists

    bool operator==(const WindowModeSnapshot& other) const {
        return mode == other.mode && monitor == other.monitor &&
               x == other.x && y == other.y &&
               width == other.width && height == other.height &&
               geometrySaved == other.geometrySaved;
    }
    bool operator!=(const WindowModeSnapshot& other) const {
        return !(*this == other);
    }
};

class IWindowMode {
public:
    virtual ~IWindowMode() = default;

    virtual WindowModeState mode() const = 0;
    virtual WindowModeSnapshot snapshot() const = 0;

    // Command set — every command validates the current state and REFUSES
    // invalid transitions (returns false + reason, state untouched):
    //   enter_fullscreen(monitor):  Windowed|Borderless -> Fullscreen
    //   enter_borderless(monitor):  Windowed -> Borderless
    //   exit():                     Fullscreen|Borderless -> Windowed (restores
    //                               the SNAPSHOTTED windowed geometry; no-op
    //                               in Windowed)
    //   set_windowed_geometry(x,y,w,h):  Windowed only (refused elsewhere);
    //                               w/h must be > 0
    //   set_size(w,h):              any mode; w/h must be > 0 (updates ONLY
    //                               the current geometry; the saved snapshot
    //                               is immutable until exit — exit() always
    //                               restores the ORIGINAL windowed geometry)
    virtual bool enter_fullscreen(std::int64_t monitor,
                                  std::string& errorOut) = 0;
    virtual bool enter_borderless(std::int64_t monitor,
                                  std::string& errorOut) = 0;
    virtual bool exit(std::string& errorOut) = 0;
    virtual bool set_windowed_geometry(std::int32_t x, std::int32_t y,
                                       std::int32_t width,
                                       std::int32_t height,
                                       std::string& errorOut) = 0;
    virtual bool set_size(std::int32_t width, std::int32_t height,
                          std::string& errorOut) = 0;

    // Deterministic JSON snapshot ({"mode":"windowed","monitor":-1,...}).
    virtual std::string to_json() const = 0;
};

// Creates the machine (always Windowed at birth, 1280x720). Never returns
// nullptr.
std::unique_ptr<IWindowMode> create_window_mode();

}  // namespace editor
}  // namespace engine
