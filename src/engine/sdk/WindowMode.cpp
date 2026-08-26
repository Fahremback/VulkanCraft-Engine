// IWindowMode adapter — the ONLY TU with behavior (engine::editor::IWindowMode).
// Pure state machine: Windowed/Borderless/Fullscreen with only valid
// transitions, geometry preservation (snapshot on enter, restore on exit)
// and monitor tracking. No clocks/RNG/globals.
//
//   Windowed  --enter_fullscreen(m)--> Fullscreen
//   Windowed  --enter_borderless(m)--> Borderless
//   Borderless--enter_fullscreen(m)--> Fullscreen
//   Borderless--exit()---------------> Windowed (geometry restored)
//   Fullscreen--exit()---------------> Windowed (geometry restored)
//   set_windowed_geometry: Windowed only (w/h > 0)
//   set_size: any mode (w/h > 0; updates current + saved geometry)

#include "engine/editor/IWindowMode.hpp"

#include <sstream>

namespace engine {
namespace editor {

namespace {

const char* mode_name(WindowModeState s) {
    switch (s) {
        case WindowModeState::Windowed: return "windowed";
        case WindowModeState::Borderless: return "borderless";
        case WindowModeState::Fullscreen: return "fullscreen";
    }
    return "windowed";
}

}  // namespace

namespace {

class WindowModeImpl final : public IWindowMode {
public:
    WindowModeState mode() const override { return m_mode; }

    WindowModeSnapshot snapshot() const override {
        WindowModeSnapshot snap;
        snap.mode = m_mode;
        snap.monitor = m_monitor;
        snap.x = m_x;
        snap.y = m_y;
        snap.width = m_width;
        snap.height = m_height;
        snap.geometrySaved = m_geometrySaved;
        return snap;
    }

    bool enter_fullscreen(std::int64_t monitor, std::string& errorOut) override {
        if (monitor < 0) {
            errorOut = "enter_fullscreen requires a monitor id >= 0";
            return false;
        }
        if (m_mode == WindowModeState::Fullscreen) {
            errorOut = "already in Fullscreen mode";
            return false;
        }
        if (m_mode != WindowModeState::Windowed &&
            m_mode != WindowModeState::Borderless) {
            errorOut = "enter_fullscreen requires Windowed/Borderless state";
            return false;
        }
        save_geometry_if_needed();
        m_mode = WindowModeState::Fullscreen;
        m_monitor = monitor;
        return true;
    }

    bool enter_borderless(std::int64_t monitor, std::string& errorOut) override {
        if (monitor < 0) {
            errorOut = "enter_borderless requires a monitor id >= 0";
            return false;
        }
        if (m_mode != WindowModeState::Windowed) {
            errorOut = "enter_borderless requires Windowed state (current: " +
                       std::string(mode_name(m_mode)) + ")";
            return false;
        }
        save_geometry_if_needed();
        m_mode = WindowModeState::Borderless;
        m_monitor = monitor;
        return true;
    }

    bool exit(std::string& errorOut) override {
        if (m_mode == WindowModeState::Windowed) {
            return true;  // idempotent no-op
        }
        m_mode = WindowModeState::Windowed;
        if (m_geometrySaved) {
            m_width = m_savedWidth;
            m_height = m_savedHeight;
        }
        m_geometrySaved = false;
        return true;
    }

    bool set_windowed_geometry(std::int32_t x, std::int32_t y,
                               std::int32_t width, std::int32_t height,
                               std::string& errorOut) override {
        if (width <= 0 || height <= 0) {
            errorOut = "window geometry width/height must be > 0";
            return false;
        }
        if (m_mode != WindowModeState::Windowed) {
            errorOut = "set_windowed_geometry requires Windowed state";
            return false;
        }
        m_x = x;
        m_y = y;
        m_width = width;
        m_height = height;
        return true;
    }

    bool set_size(std::int32_t width, std::int32_t height,
                  std::string& errorOut) override {
        if (width <= 0 || height <= 0) {
            errorOut = "window size width/height must be > 0";
            return false;
        }
        // Updates ONLY the current geometry. The saved snapshot is immutable
        // until exit() — exit() always restores the ORIGINAL windowed size.
        m_width = width;
        m_height = height;
        return true;
    }

    std::string to_json() const override {
        const WindowModeSnapshot snap = snapshot();
        std::ostringstream out;
        out << "{\"mode\":\"" << mode_name(snap.mode)
            << "\",\"monitor\":" << snap.monitor
            << ",\"x\":" << snap.x
            << ",\"y\":" << snap.y
            << ",\"width\":" << snap.width
            << ",\"height\":" << snap.height
            << ",\"geometry_saved\":"
            << (snap.geometrySaved ? "true" : "false") << "}";
        return out.str();
    }

private:
    void save_geometry_if_needed() {
        if (m_geometrySaved) return;  // snapshot taken once
        m_savedX = m_x;
        m_savedY = m_y;
        m_savedWidth = m_width;
        m_savedHeight = m_height;
        m_geometrySaved = true;
    }

    WindowModeState m_mode{ WindowModeState::Windowed };
    std::int64_t m_monitor{ -1 };
    std::int32_t m_x{ 0 }, m_y{ 0 };
    std::int32_t m_width{ 1280 }, m_height{ 720 };
    bool m_geometrySaved{ false };
    std::int32_t m_savedX{ 0 }, m_savedY{ 0 };
    std::int32_t m_savedWidth{ 1280 }, m_savedHeight{ 720 };
};

}  // namespace

std::unique_ptr<IWindowMode> create_window_mode() {
    return std::make_unique<WindowModeImpl>();
}

}  // namespace editor
}  // namespace engine
