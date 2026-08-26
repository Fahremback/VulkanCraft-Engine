// UiViewportTests — headless coverage for the public viewport/scale/safe-area
// contract (engine/ui/IViewport.hpp, adapter UiViewport.cpp). Pure and
// deterministic: None/Fit/Fill scale modes, safe-area insets, text_scale and
// high_contrast. Standalone main() with CHECK (pattern: UiWidgetsTests).

#include "engine/ui/IViewport.hpp"

#include <cstdlib>
#include <cmath>
#include <iostream>
#include <string>

using namespace engine::ui;

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "UiViewportTests failure at line " << __LINE__ << ": " #condition "\n"; \
    return false; } } while (false)

namespace {

bool near(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) <= eps;
}

bool run_all() {
    std::string err;

    // ---- Spec validate + JSON round-trip --------------------------------
    ViewportSpec spec;
    spec.reference_width = 1920.0;
    spec.reference_height = 1080.0;
    spec.scale_mode = UiScaleMode::Fit;
    spec.safe_area.left = 20.0;
    spec.safe_area.right = 20.0;
    spec.safe_area.top = 40.0;
    spec.safe_area.bottom = 0.0;
    spec.text_scale = 1.25;
    spec.high_contrast = true;
    CHECK(spec.validate(err));

    const std::string json = spec.to_json();
    ViewportSpec back;
    CHECK(back.load_from_json(json, err));
    CHECK(back.reference_width == 1920.0);
    CHECK(back.reference_height == 1080.0);
    CHECK(back.scale_mode == UiScaleMode::Fit);
    CHECK(back.safe_area.left == 20.0);
    CHECK(back.safe_area.right == 20.0);
    CHECK(back.safe_area.top == 40.0);
    CHECK(back.safe_area.bottom == 0.0);
    CHECK(back.text_scale == 1.25);
    CHECK(back.high_contrast);
    CHECK(back.to_json() == json); // bit-exact

    // Malformed refused all-or-nothing (target untouched).
    ViewportSpec keep = back;
    CHECK(!back.load_from_json("{bad", err));
    CHECK(back.to_json() == keep.to_json());

    // Validation refusals.
    ViewportSpec bad = spec;
    bad.reference_width = 0.0;
    CHECK(!bad.validate(err));
    bad = spec;
    bad.reference_height = -1.0;
    CHECK(!bad.validate(err));
    bad = spec;
    bad.safe_area.top = -5.0;
    CHECK(!bad.validate(err));
    bad = spec;
    bad.text_scale = 0.5;
    CHECK(!bad.validate(err));
    bad = spec;
    bad.version = 2;
    CHECK(!bad.validate(err));
    // Unknown scale_mode refused on load.
    CHECK(!back.load_from_json("{\"version\":1,\"reference_width\":1920,\"reference_height\":1080,\"scale_mode\":\"stretch\",\"safe_area\":{\"left\":0,\"right\":0,\"top\":0,\"bottom\":0},\"text_scale\":1,\"high_contrast\":false}", err));

    // ---- Fit mode ---------------------------------------------------------
    auto viewport = create_ui_viewport(spec, err);
    CHECK(viewport != nullptr);

    // 1920x1080 viewport, safe area (20,20,40,0) -> usable 1880x1040 at (20,40).
    // Fit: scale = min(1880/1920, 1040/1080) = min(0.97917, 0.96296) = 0.96296.
    ViewportState st = viewport->resolve(1920.0, 1080.0, err);
    CHECK(st.usable_x == 20.0);
    CHECK(st.usable_y == 40.0);
    CHECK(near(st.usable_w, 1880.0));
    CHECK(near(st.usable_h, 1040.0));
    CHECK(near(st.scale, 1040.0 / 1080.0)); // width-limited by height
    // Centered: offset_x = 20 + (1880 - 1920*scale)/2.
    CHECK(near(st.offset_x, 20.0 + (1880.0 - 1920.0 * st.scale) * 0.5));
    CHECK(near(st.offset_y, 40.0)); // reference aspect matches usable height

    // Wider viewport -> scale limited by width (landscape).
    st = viewport->resolve(3840.0, 1080.0, err);
    // usable 3800x1040; scale = min(3800/1920, 1040/1080) = 0.96296.
    CHECK(near(st.scale, 1040.0 / 1080.0));

    // Taller viewport -> scale limited by height.
    st = viewport->resolve(1920.0, 2160.0, err);
    // usable 1880x2120; scale = min(1880/1920, 2120/1080) = 0.97917.
    CHECK(near(st.scale, 1880.0 / 1920.0));

    // ---- None mode --------------------------------------------------------
    ViewportSpec none = spec;
    none.scale_mode = UiScaleMode::None;
    auto nv = create_ui_viewport(none, err);
    CHECK(nv != nullptr);
    st = nv->resolve(1920.0, 1080.0, err);
    CHECK(near(st.scale, 1.0));
    CHECK(near(st.offset_x, 20.0)); // top-left anchored in usable area
    CHECK(near(st.offset_y, 40.0));

    // ---- Fill mode --------------------------------------------------------
    ViewportSpec fill = spec;
    fill.scale_mode = UiScaleMode::Fill;
    auto fv = create_ui_viewport(fill, err);
    CHECK(fv != nullptr);
    st = fv->resolve(1920.0, 1080.0, err);
    // usable 1880x1040; scale = max(1880/1920, 1040/1080) = 0.97917 (crops).
    CHECK(near(st.scale, 1880.0 / 1920.0));

    // ---- Accessibility flags survive resolve ------------------------------
    st = viewport->resolve(1920.0, 1080.0, err);
    CHECK(near(st.text_scale, 1.25));
    CHECK(st.high_contrast);

    // ---- Refusals ---------------------------------------------------------
    ViewportSpec zero = spec;
    auto zv = create_ui_viewport(zero, err);
    CHECK(zv != nullptr);
    CHECK(zv->resolve(0.0, 1080.0, err).usable_w <= 0.0 || !err.empty());
    err.clear();
    CHECK(zv->resolve(-5.0, 1080.0, err).usable_w <= 0.0 || !err.empty());
    err.clear();
    CHECK(zv->resolve(1920.0, 1080.0, err).scale > 0.0); // sanity: valid resolve
    err.clear();

    // ---- Determinism cross-instance --------------------------------------
    auto v1 = create_ui_viewport(spec, err);
    auto v2 = create_ui_viewport(spec, err);
    const ViewportState s1 = v1->resolve(2560.0, 1440.0, err);
    const ViewportState s2 = v2->resolve(2560.0, 1440.0, err);
    CHECK(s1.scale == s2.scale);
    CHECK(s1.offset_x == s2.offset_x && s1.offset_y == s2.offset_y);
    CHECK(s1.usable_w == s2.usable_w && s1.usable_h == s2.usable_h);
    CHECK(s1.text_scale == s2.text_scale && s1.high_contrast == s2.high_contrast);

    std::cout << "UiViewportTests: all checks passed\n";
    return true;
}

}  // namespace

int main() {
    if (!run_all()) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
