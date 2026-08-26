// UiViewport.cpp — the ONLY TU with the viewport/scale/safe-area runtime
// (agente 2 §A item 4). Pure and deterministic: resolve() maps the reference
// space onto the usable area with a uniform scale and centered offsets. No
// window/GPU. JSON parse/emit uses the shared RegistryJson helpers.

#include "engine/ui/IViewport.hpp"

#include "RegistryJson.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace engine {
namespace ui {

namespace {

bool is_finite(double v) { return std::isfinite(v); }

const char* scale_mode_name(UiScaleMode mode) {
    switch (mode) {
        case UiScaleMode::None: return "none";
        case UiScaleMode::Fit: return "fit";
        case UiScaleMode::Fill: break;
    }
    return "fill";
}

bool scale_mode_from_name(const std::string& name, UiScaleMode& out) {
    if (name == "none") { out = UiScaleMode::None; return true; }
    if (name == "fit") { out = UiScaleMode::Fit; return true; }
    if (name == "fill") { out = UiScaleMode::Fill; return true; }
    return false;
}

}  // namespace

bool ViewportSpec::validate(std::string& errorOut) const {
    errorOut.clear();
    if (version != 1) {
        errorOut = "unsupported viewport spec version";
        return false;
    }
    if (!is_finite(reference_width) || reference_width <= 0.0) {
        errorOut = "viewport reference_width must be finite and > 0";
        return false;
    }
    if (!is_finite(reference_height) || reference_height <= 0.0) {
        errorOut = "viewport reference_height must be finite and > 0";
        return false;
    }
    for (const double inset : { safe_area.left, safe_area.right,
                                safe_area.top, safe_area.bottom }) {
        if (!is_finite(inset) || inset < 0.0) {
            errorOut = "viewport safe area insets must be finite and >= 0";
            return false;
        }
    }
    if (!is_finite(text_scale) || text_scale < 1.0) {
        errorOut = "viewport text_scale must be finite and >= 1";
        return false;
    }
    return true;
}

std::string ViewportSpec::to_json() const {
    std::ostringstream out;
    out.precision(9);
    out << "{\"version\":1,\"reference_width\":" << reference_width
        << ",\"reference_height\":" << reference_height
        << ",\"scale_mode\":\"" << scale_mode_name(scale_mode)
        << "\",\"safe_area\":{\"left\":" << safe_area.left
        << ",\"right\":" << safe_area.right << ",\"top\":" << safe_area.top
        << ",\"bottom\":" << safe_area.bottom
        << "},\"text_scale\":" << text_scale
        << ",\"high_contrast\":" << (high_contrast ? "true" : "false") << "}";
    return out.str();
}

bool ViewportSpec::load_from_json(const std::string& jsonText,
                                  std::string& errorOut) {
    sdk::JsonValue doc;
    if (!sdk::json_parse(jsonText, doc, errorOut)) return false;
    if (!doc.is_object()) {
        errorOut = "viewport spec document must be an object";
        return false;
    }
    const int version = static_cast<int>(sdk::json_number(doc, "version", 1));
    if (version != 1) {
        errorOut = "unsupported viewport spec version";
        return false;
    }
    ViewportSpec candidate;
    candidate.version = version;
    candidate.reference_width = sdk::json_number(doc, "reference_width", 1920.0);
    candidate.reference_height = sdk::json_number(doc, "reference_height", 1080.0);
    const std::string mode = sdk::json_string(doc, "scale_mode", "fit");
    if (!scale_mode_from_name(mode, candidate.scale_mode)) {
        errorOut = "unknown viewport scale_mode: " + mode;
        return false;
    }
    if (const sdk::JsonValue* safe = doc.field("safe_area")) {
        if (safe->is_object()) {
            candidate.safe_area.left = sdk::json_number(*safe, "left", 0.0);
            candidate.safe_area.right = sdk::json_number(*safe, "right", 0.0);
            candidate.safe_area.top = sdk::json_number(*safe, "top", 0.0);
            candidate.safe_area.bottom = sdk::json_number(*safe, "bottom", 0.0);
        }
    }
    candidate.text_scale = sdk::json_number(doc, "text_scale", 1.0);
    candidate.high_contrast = sdk::json_bool(doc, "high_contrast", false);
    if (!candidate.validate(errorOut)) return false;
    *this = std::move(candidate);
    return true;
}

// ---------------------------------------------------------------------------
// Runtime
// ---------------------------------------------------------------------------

namespace {

class UiViewportRuntime final : public IUiViewport {
public:
    explicit UiViewportRuntime(const ViewportSpec& spec) : spec_(spec) {}

    ViewportState resolve(double viewportWidth, double viewportHeight,
                          std::string& errorOut) const override {
        errorOut.clear();
        if (!is_finite(viewportWidth) || viewportWidth <= 0.0 ||
            !is_finite(viewportHeight) || viewportHeight <= 0.0) {
            errorOut = "viewport size must be finite and > 0";
            return {};
        }

        ViewportState state;
        state.text_scale = spec_.text_scale;
        state.high_contrast = spec_.high_contrast;

        // Usable area = viewport minus safe-area insets.
        const double usableW = std::max(0.0, viewportWidth - spec_.safe_area.left -
                                                 spec_.safe_area.right);
        const double usableH = std::max(0.0, viewportHeight - spec_.safe_area.top -
                                                 spec_.safe_area.bottom);
        state.usable_x = spec_.safe_area.left;
        state.usable_y = spec_.safe_area.top;
        state.usable_w = usableW;
        state.usable_h = usableH;

        double scale = 1.0;
        switch (spec_.scale_mode) {
            case UiScaleMode::None:
                scale = 1.0;
                break;
            case UiScaleMode::Fit: {
                const double sx = usableW / spec_.reference_width;
                const double sy = usableH / spec_.reference_height;
                scale = std::min(sx, sy);
                break;
            }
            case UiScaleMode::Fill: {
                const double sx = usableW / spec_.reference_width;
                const double sy = usableH / spec_.reference_height;
                scale = std::max(sx, sy);
                break;
            }
        }

        state.scale = scale;
        if (spec_.scale_mode == UiScaleMode::None) {
            // 1:1, top-left anchored in the usable area.
            state.offset_x = state.usable_x;
            state.offset_y = state.usable_y;
        } else {
            // Center the scaled reference rect in the usable area.
            state.offset_x = state.usable_x + (usableW - spec_.reference_width * scale) * 0.5;
            state.offset_y = state.usable_y + (usableH - spec_.reference_height * scale) * 0.5;
        }
        return state;
    }

    const ViewportSpec& spec() const override { return spec_; }

private:
    ViewportSpec spec_;
};

}  // namespace

std::unique_ptr<IUiViewport> create_ui_viewport(const ViewportSpec& spec,
                                                std::string& errorOut) {
    errorOut.clear();
    if (!spec.validate(errorOut)) return nullptr;
    return std::make_unique<UiViewportRuntime>(spec);
}

}  // namespace ui
}  // namespace engine
