// EditorPluginTests — headless coverage for the editor panel plugin registry
// (EditorPlugin.hpp/cpp). Mirrors the ezEngine "tools as replaceable plugins"
// pillar: panels register stable metadata, the shell derives menus/palette
// deterministically. No UI, no GPU. Standalone main() with CHECK, same
// pattern as VisualAuthoringTests.
//
// Covered: registration + size/find/contains; duplicate and empty id refused
// without mutation; stable insertion order (panels + categories first-seen);
// category filtering; unregister (+ unknown id refused); clear.

#include "../src/editor/EditorPlugin.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace Engine::Editor;

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "EditorPluginTests failure at line " << __LINE__ << ": " #condition "\n"; \
    return false; } } while (false)

namespace {

EditorPanelSpec make_panel(std::string id, std::string title, std::string category,
                           bool toggleable = true, bool default_open = false) {
    EditorPanelSpec spec;
    spec.id = std::move(id);
    spec.title = std::move(title);
    spec.category = std::move(category);
    spec.toggleable = toggleable;
    spec.default_open = default_open;
    return spec;
}

bool run_all() {
    EditorPluginRegistry registry;

    // Registration + lookups.
    CHECK(registry.size() == 0);
    CHECK(registry.register_panel(make_panel("hierarchy", "Hierarchy", "scene", true, true)));
    CHECK(registry.register_panel(make_panel("inspector", "Inspector", "scene", true, true)));
    CHECK(registry.register_panel(make_panel("content_browser", "Content Browser", "assets", true, false)));
    CHECK(registry.register_panel(make_panel("profiler", "Profiler", "debug", false, false)));
    CHECK(registry.size() == 4);
    CHECK(registry.contains("hierarchy"));
    CHECK(registry.contains("profiler"));
    CHECK(!registry.contains("missing"));
    CHECK(registry.find("inspector") != nullptr);
    CHECK(registry.find("inspector")->title == "Inspector");
    CHECK(registry.find("inspector")->category == "scene");
    CHECK(registry.find("nope") == nullptr);

    // Duplicate id refused, registry unchanged.
    const std::size_t before = registry.size();
    CHECK(!registry.register_panel(make_panel("hierarchy", "Other", "other")));
    CHECK(registry.size() == before);
    CHECK(registry.find("hierarchy")->title == "Hierarchy");

    // Empty id refused.
    CHECK(!registry.register_panel(make_panel("", "Nameless", "scene")));
    CHECK(registry.size() == before);

    // Stable insertion order (panels).
    const std::vector<EditorPanelSpec> all = registry.panels();
    CHECK(all.size() == 4);
    CHECK(all[0].id == "hierarchy");
    CHECK(all[1].id == "inspector");
    CHECK(all[2].id == "content_browser");
    CHECK(all[3].id == "profiler");

    // Categories: distinct, first-seen order.
    const std::vector<std::string> cats = registry.categories();
    CHECK(cats.size() == 3);
    CHECK(cats[0] == "scene");
    CHECK(cats[1] == "assets");
    CHECK(cats[2] == "debug");

    // Category filtering, insertion order within the category.
    const std::vector<EditorPanelSpec> scenePanels = registry.panels_in_category("scene");
    CHECK(scenePanels.size() == 2);
    CHECK(scenePanels[0].id == "hierarchy");
    CHECK(scenePanels[1].id == "inspector");
    CHECK(registry.panels_in_category("assets").size() == 1);
    CHECK(registry.panels_in_category("debug").size() == 1);
    CHECK(registry.panels_in_category("empty").empty());

    // Unregister: removes from map AND order; unknown id refused.
    CHECK(registry.unregister_panel("content_browser"));
    CHECK(!registry.contains("content_browser"));
    CHECK(registry.size() == 3);
    const std::vector<EditorPanelSpec> afterRemove = registry.panels();
    CHECK(afterRemove.size() == 3);
    CHECK(afterRemove[0].id == "hierarchy");
    CHECK(afterRemove[1].id == "inspector");
    CHECK(afterRemove[2].id == "profiler");
    CHECK(!registry.unregister_panel("content_browser")); // already gone
    CHECK(registry.size() == 3);

    // Re-register after unregister succeeds.
    CHECK(registry.register_panel(make_panel("content_browser", "Content Browser", "assets", true, false)));
    CHECK(registry.size() == 4);

    // Clear.
    registry.clear();
    CHECK(registry.size() == 0);
    CHECK(registry.panels().empty());
    CHECK(registry.categories().empty());

    std::cout << "EditorPluginTests: all checks passed\n";
    return true;
}

} // namespace

int main() {
    if (!run_all()) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
