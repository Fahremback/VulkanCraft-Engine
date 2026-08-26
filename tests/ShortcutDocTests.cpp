// ShortcutDocTests — headless coverage for the public shortcut-documentation
// contract (engine/editor/IShortcutDoc.hpp, adapter ShortcutDoc.cpp):
// humanize() of InputBindings across sources, and document() rendering the
// CURRENT bindings of an ActionMapSpec as deterministic markdown (labels,
// unbound markers, UNBOUND section). Standalone main() with CHECK.

#include "engine/editor/IShortcutDoc.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

using namespace engine::editor;
using namespace engine::input;

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "ShortcutDocTests failure at line " << __LINE__ << ": " #condition "\n"; \
    return false; } } while (false)

namespace {

ShortcutDocSpec make_spec() {
    ShortcutDocSpec spec;
    spec.title = "VulkanCraft Editor Shortcuts";
    {
        ShortcutEntry e;
        e.action = "palette";
        e.label = "Command Palette";
        e.description = "Open the global command palette.";
        spec.entries.push_back(e);
    }
    {
        ShortcutEntry e;
        e.action = "gizmo.scale";
        e.label = "Scale Gizmo";
        spec.entries.push_back(e);
    }
    {
        ShortcutEntry e;
        e.action = "play";
        e.label = "Play";
        spec.entries.push_back(e);
    }
    {
        ShortcutEntry e;
        e.action = "obsolete.action";
        e.label = "Obsolete";
        spec.entries.push_back(e);  // not in the map below -> UNBOUND
    }
    return spec;
}

ActionMapSpec make_map() {
    ActionMapSpec map;

    ActionBinding palette;
    palette.action = "palette";
    {
        InputBinding b;
        b.source = InputSource::Keyboard;
        b.input = "KeyK";
        palette.bindings.push_back(b);
    }
    map.actions.push_back(palette);

    ActionBinding scale;
    scale.action = "gizmo.scale";
    {
        InputBinding b;
        b.source = InputSource::Keyboard;
        b.input = "KeyR";
        scale.bindings.push_back(b);
    }
    map.actions.push_back(scale);

    ActionBinding play;
    play.action = "play";
    {
        InputBinding b;
        b.source = InputSource::Mouse;
        b.input = "MouseLeft";
        play.bindings.push_back(b);
    }
    map.actions.push_back(play);

    return map;
}

bool run_all() {
    std::string err;

    // ---- Spec validate + JSON round-trip ----------------------------------
    {
        const ShortcutDocSpec spec = make_spec();
        CHECK(spec.validate(err));
        const std::string json = spec.to_json();
        ShortcutDocSpec back;
        CHECK(back.load_from_json(json, err));
        CHECK(back.title == "VulkanCraft Editor Shortcuts");
        CHECK(back.entries.size() == 4);
        CHECK(back.to_json() == json);  // bit-exact

        ShortcutDocSpec untouched = make_spec();
        CHECK(!untouched.load_from_json("{bad", err));
        CHECK(untouched.entries.size() == 4);
        CHECK(!untouched.load_from_json("{\"version\":99}", err));
        CHECK(untouched.entries.size() == 4);
        CHECK(!untouched.load_from_json(
            "{\"version\":1,\"entries\":[{\"action\":\"a\"},"
            "{\"action\":\"a\"}]}", err));  // duplicate refused
        CHECK(untouched.entries.size() == 4);
    }

    // ---- humanize across sources ------------------------------------------
    {
        auto doc = create_shortcut_doc(make_spec(), err);
        CHECK(doc != nullptr);

        InputBinding key;
        key.source = InputSource::Keyboard;
        key.input = "KeyW";
        CHECK(doc->humanize(key) == "W");

        key.input = "Space";
        CHECK(doc->humanize(key) == "Space");

        InputBinding mouse;
        mouse.source = InputSource::Mouse;
        mouse.input = "MouseLeft";
        CHECK(doc->humanize(mouse) == "Left Mouse");

        InputBinding pad;
        pad.source = InputSource::Gamepad;
        pad.input = "PadA";
        CHECK(doc->humanize(pad) == "Gamepad A");

        InputBinding axis;
        axis.source = InputSource::Gamepad;
        axis.input = "AxisLX";
        axis.axis = 0;
        axis.scale = -1.0;
        CHECK(doc->humanize(axis) == "Left Stick X (inverted)");

        axis.scale = 1.0;
        CHECK(doc->humanize(axis) == "Left Stick X");
    }

    // ---- document(): deterministic markdown of CURRENT bindings ----------
    {
        auto doc = create_shortcut_doc(make_spec(), err);
        CHECK(doc != nullptr);
        const ActionMapSpec map = make_map();
        CHECK(map.validate(err));

        const std::string text = doc->document(map, err);
        CHECK(!text.empty());
        // Title + the three actions with labels.
        CHECK(text.find("# VulkanCraft Editor Shortcuts") != std::string::npos);
        CHECK(text.find("## Command Palette") != std::string::npos);
        CHECK(text.find("Open the global command palette.") != std::string::npos);
        CHECK(text.find("- K") != std::string::npos);
        CHECK(text.find("## Scale Gizmo") != std::string::npos);
        CHECK(text.find("- R") != std::string::npos);
        CHECK(text.find("## Play") != std::string::npos);
        CHECK(text.find("- Left Mouse") != std::string::npos);
        // Unknown entry surfaced, never dropped.
        CHECK(text.find("## UNBOUND") != std::string::npos);
        CHECK(text.find("obsolete.action (not in the action map)") !=
              std::string::npos);
    }

    // ---- Fallbacks: no label -> action name -------------------------------
    {
        ShortcutDocSpec spec = make_spec();
        spec.entries.clear();  // no metadata: labels fall back to action names
        auto doc = create_shortcut_doc(spec, err);
        CHECK(doc != nullptr);

        ActionMapSpec map = make_map();
        ActionBinding extra;
        extra.action = "extra_action";
        InputBinding kb;
        kb.source = InputSource::Keyboard;
        kb.input = "KeyF";
        extra.bindings.push_back(kb);
        map.actions.push_back(extra);

        const std::string text = doc->document(map, err);
        CHECK(text.find("## palette") != std::string::npos);  // action name
        CHECK(text.find("## extra_action") != std::string::npos);
        CHECK(text.find("- F") != std::string::npos);
    }

    // ---- Determinism cross-instance --------------------------------------
    {
        auto a = create_shortcut_doc(make_spec(), err);
        auto b = create_shortcut_doc(make_spec(), err);
        CHECK(a != nullptr && b != nullptr);
        const ActionMapSpec map = make_map();
        CHECK(a->document(map, err) == b->document(map, err));
    }

    std::cout << "ShortcutDocTests: all checks passed\n";
    return true;
}

}  // namespace

int main() {
    if (!run_all()) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
