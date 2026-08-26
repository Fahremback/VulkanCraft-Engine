#pragma once

// IViewport (agente 2 §A item 4): the PUBLIC viewport/scale/safe-area
// contract. A UI is designed against a REFERENCE size; at runtime the project
// feeds the actual viewport (any resolution/aspect/DPI) and this contract
// decides deterministically how to map the reference space onto the usable
// area:
//   - SAFE AREA: insets (left/right/top/bottom) subtracted from the viewport;
//     the UI is confined to the resulting usable rect (notches, taskbars,
//     TV overscan).
//   - SCALE MODE:
//       * None  — 1:1, top-left anchored (no scaling).
//       * Fit   — scale = min(usableW/refW, usableH/refH); the reference
//                 rect fits entirely (letterboxing), centered in the usable
//                 area.
//       * Fill  — scale = max(usableW/refW, usableH/refH); the reference
//                 rect covers the usable area (overflow is cropped),
//                 centered.
//     The resolved `scale` is a single uniform factor (no aspect distortion).
//   - ACCESSIBILITY: `text_scale` (>= 1 multiplies the base scale for text)
//     and `high_contrast` (a data flag the renderer uses to switch themes).
//
// Deterministic and headless: same spec + viewport -> identical
// scale/offset/usable rect, bit-exact. Self-contained (std only). The SDK
// adapter (src/engine/sdk/UiViewport.cpp) is the ONLY TU with behavior.

#include <cstdint>
#include <memory>
#include <string>

namespace engine {
namespace ui {

enum class UiScaleMode : std::uint8_t { None, Fit, Fill };

// Viewport insets (safe area), in viewport pixels.
struct UiSafeArea {
    double left{ 0.0 };
    double right{ 0.0 };
    double top{ 0.0 };
    double bottom{ 0.0 };
};

// The data-driven viewport spec, validated all-or-nothing.
struct ViewportSpec {
    int version{ 1 };
    double reference_width{ 1920.0 };
    double reference_height{ 1080.0 };
    UiScaleMode scale_mode{ UiScaleMode::Fit };
    UiSafeArea safe_area;
    double text_scale{ 1.0 };  // >= 1 (accessibility)
    bool high_contrast{ false }; // accessibility flag (renderer-owned theme)

    bool validate(std::string& errorOut) const;
    bool load_from_json(const std::string& jsonText, std::string& errorOut);
    std::string to_json() const;
};

// The resolved viewport mapping for one frame.
struct ViewportState {
    double scale{ 1.0 };       // uniform scale factor
    double offset_x{ 0.0 };    // where the reference origin maps (viewport px)
    double offset_y{ 0.0 };
    double usable_x{ 0.0 };    // usable rect (viewport minus safe area)
    double usable_y{ 0.0 };
    double usable_w{ 0.0 };
    double usable_h{ 0.0 };
    double text_scale{ 1.0 };  // a11y text multiplier
    bool high_contrast{ false };
};

class IUiViewport {
public:
    virtual ~IUiViewport() = default;

    // Resolves the spec against the current viewport size. The viewport must
    // be finite and > 0 (a zero-sized window is refused with a diagnostic).
    virtual ViewportState resolve(double viewportWidth, double viewportHeight,
                                  std::string& errorOut) const = 0;

    virtual const ViewportSpec& spec() const = 0;
};

// Parses+validates a spec and compiles it (rejected -> nullptr + errorOut).
std::unique_ptr<IUiViewport> create_ui_viewport(const ViewportSpec& spec,
                                                std::string& errorOut);

}  // namespace ui
}  // namespace engine
