// EditorLayoutTests — headless coverage for the editor shell layout model
// (src/editor/EditorLayout.hpp, adapter EditorLayout.cpp): registry-derived
// defaults, visibility toggling, bit-exact JSON round-trip, all-or-nothing
// (malformed) vs tolerant (unknown ids) snapshot application, safe reset, and
// cross-instance determinism. Standalone main() with CHECK.

#include "EditorLayout.hpp"

#include "EditorPlugin.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

using namespace Engine::Editor;

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "EditorLayoutTests failure at line " << __LINE__ << ": " #condition "\n"; \
    return false; } } while (false)

namespace {

std::vector<EditorPanelSpec> make_registry() {
    std::vector<EditorPanelSpec> panels;
    const auto add = [&](const char* id, bool defaultOpen) {
        EditorPanelSpec spec;
        spec.id = id;
        spec.title = id;
        spec.category = "test";
        spec.toggleable = true;
        spec.default_open = defaultOpen;
        panels.push_back(spec);
    };
    add("hierarchy", true);
    add("inspector", true);
    add("content_browser", false);
    add("profiler", false);
    return panels;
}

bool run_all() {
    std::string err;

    // ---- Registry-derived defaults (safe reset) --------------------------
    {
        EditorLayoutModel model;
        model.reset_from_registry(make_registry());
        CHECK(model.panel_ids().size() == 4);
        CHECK(model.is_visible("hierarchy"));
        CHECK(model.is_visible("inspector"));
        CHECK(!model.is_visible("content_browser"));
        CHECK(!model.is_visible("profiler"));
        const std::vector<std::string> visible = model.visible_panels();
        CHECK(visible.size() == 2);
        CHECK(visible[0] == "hierarchy");
        CHECK(visible[1] == "inspector");
    }

    // ---- Visibility toggling ----------------------------------------------
    {
        EditorLayoutModel model;
        model.reset_from_registry(make_registry());
        CHECK(model.set_visible("content_browser", true));
        CHECK(model.is_visible("content_browser"));
        CHECK(!model.set_visible("nope", true));  // unknown id refused
        CHECK(model.set_visible("hierarchy", false));
        CHECK(!model.is_visible("hierarchy"));
        CHECK(model.visible_panels().size() == 2);  // inspector + content_browser
    }

    // ---- Snapshot round-trip (bit-exact) ----------------------------------
    {
        EditorLayoutModel model;
        model.reset_from_registry(make_registry());
        model.set_visible("content_browser", true);
        model.set_active_panel("inspector");
        model.set_gizmo_mode("rotate");
        model.set_viewport_first(false);

        const EditorLayoutSnapshot snap = model.snapshot();
        CHECK(snap.validate(err));
        const std::string json = snap.to_json();

        EditorLayoutSnapshot back;
        CHECK(back.load_from_json(json, err));
        CHECK(back.viewport_first == snap.viewport_first);
        CHECK(back.active_panel == snap.active_panel);
        CHECK(back.gizmo_mode == snap.gizmo_mode);
        CHECK(back.panels.size() == snap.panels.size());
        CHECK(back.to_json() == json);  // bit-exact
    }

    // ---- apply_snapshot: known applies, unknown ignored -------------------
    {
        EditorLayoutModel model;
        model.reset_from_registry(make_registry());
        model.set_visible("content_browser", true);

        EditorLayoutSnapshot snap = model.snapshot();
        snap.panels.push_back({"newer_panel", true});  // from a newer editor
        snap.panels[2].visible = false;                // hide content_browser

        CHECK(model.apply_snapshot(snap, err));
        CHECK(!model.is_visible("content_browser"));   // known -> applied
        CHECK(!model.is_visible("newer_panel"));       // unknown -> ignored
        CHECK(model.is_visible("hierarchy"));          // untouched
    }

    // ---- Malformed snapshots do not mutate --------------------------------
    {
        EditorLayoutModel model;
        model.reset_from_registry(make_registry());
        model.set_visible("content_browser", true);
        const std::string before = model.snapshot().to_json();

        EditorLayoutSnapshot bad;
        CHECK(!bad.load_from_json("{not json", err));
        CHECK(model.snapshot().to_json() == before);

        EditorLayoutSnapshot badVersion;
        CHECK(!badVersion.load_from_json("{\"version\":99}", err));
        CHECK(model.snapshot().to_json() == before);

        EditorLayoutSnapshot badGizmo;
        CHECK(!badGizmo.load_from_json(
            "{\"version\":1,\"gizmo_mode\":\"fly\"}", err));
        CHECK(model.snapshot().to_json() == before);

        // apply_snapshot of an invalid gizmo is refused (no mutation).
        EditorLayoutSnapshot invalid;
        invalid.version = 1;
        invalid.gizmo_mode = "fly";
        CHECK(!model.apply_snapshot(invalid, err));
        CHECK(model.snapshot().to_json() == before);
    }

    // ---- Safe reset returns to defaults -----------------------------------
    {
        EditorLayoutModel model;
        model.reset_from_registry(make_registry());
        model.set_visible("profiler", true);
        model.set_visible("hierarchy", false);
        model.set_active_panel("profiler");
        model.set_gizmo_mode("scale");
        model.set_viewport_first(false);

        model.reset_from_registry(make_registry());
        CHECK(model.is_visible("hierarchy"));       // back to default
        CHECK(model.is_visible("inspector"));
        CHECK(!model.is_visible("profiler"));
        CHECK(model.active_panel().empty());
        CHECK(model.gizmo_mode().empty());
        CHECK(model.viewport_first());
    }

    // ---- Determinism cross-instance ---------------------------------------
    {
        const auto registry = make_registry();
        EditorLayoutModel a;
        EditorLayoutModel b;
        a.reset_from_registry(registry);
        b.reset_from_registry(registry);
        a.set_visible("content_browser", true);
        b.set_visible("content_browser", true);
        CHECK(a.snapshot().to_json() == b.snapshot().to_json());
    }

    std::cout << "EditorLayoutTests: all checks passed\n";
    return true;
}

}  // namespace

int main() {
    if (!run_all()) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
