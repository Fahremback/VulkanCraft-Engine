#pragma once

// IShortcutDoc (agente 2 §C "atalhos documentados"): the PUBLIC shortcut-
// documentation contract. The editor's Help menu / README / in-editor
// shortcuts panel renders the CURRENT bindings as a human-readable document
// — derived from the ActionMapSpec (engine/input IActionMap, the rebindable
// input map), never from hardcoded help text. After a rebind, the same
// document() call reflects the new binding.
//   - HUMANIZE: turns an InputBinding into a readable string ("W",
//     "Left Mouse", "Gamepad A", "Left Stick X (inverted)" …). Deterministic.
//   - DOCUMENT: renders one markdown block per action, in the map's
//     declaration order, listing every binding slot. Entry metadata (label,
//     description) comes from a ShortcutDoc spec; actions without an entry
//     fall back to the action name. Entries whose action is NOT in the map
//     are listed as unbound (visible, never dropped silently).
//   - ALL-OR-NOTHING: ShortcutDoc load_from_json refuses bad version /
//     empty action / duplicate action without mutating.
//   - DETERMINISM: same map + doc -> identical markdown, bit-exact.
//
// Self-contained (std + engine/input only). The SDK adapter
// (src/engine/sdk/ShortcutDoc.cpp) is the ONLY TU with behavior.

#include <memory>
#include <string>
#include <vector>

#include "engine/input/IActionMap.hpp"

namespace engine {
namespace editor {

// Display metadata for one action in the shortcut document.
struct ShortcutEntry {
    std::string action;      // must exist in the ActionMapSpec
    std::string label;       // display name ("" = fall back to action)
    std::string description; // one-line help (optional)

    bool operator==(const ShortcutEntry& other) const {
        return action == other.action && label == other.label &&
               description == other.description;
    }
    bool operator!=(const ShortcutEntry& other) const {
        return !(*this == other);
    }
};

// The doc spec: metadata only (bindings come from the map at document() time).
struct ShortcutDocSpec {
    int version{ 1 };
    std::string title;  // document title (optional)
    std::vector<ShortcutEntry> entries;

    bool validate(std::string& errorOut) const;
    bool load_from_json(const std::string& jsonText, std::string& errorOut);
    std::string to_json() const;
};

class IShortcutDoc {
public:
    virtual ~IShortcutDoc() = default;

    // Renders a readable string for a single binding ("W", "Ctrl+K",
    // "Left Mouse", "Gamepad A", "Left Stick X (inverted)"). Deterministic
    // and always non-empty.
    virtual std::string humanize(const engine::input::InputBinding& binding)
        const = 0;

    // Renders the full markdown document for the given action map. Actions
    // render in the map's declaration order; unbound actions (no bindings)
    // render with "- (no bindings)". Entries with an action missing from the
    // map render under "UNBOUND" so nothing disappears silently.
    virtual std::string document(
        const engine::input::ActionMapSpec& map,
        std::string& errorOut) const = 0;

    virtual const ShortcutDocSpec& spec() const = 0;
};

// Parses+validates the doc spec and compiles it (rejected -> nullptr +
// errorOut).
std::unique_ptr<IShortcutDoc> create_shortcut_doc(const ShortcutDocSpec& spec,
                                                  std::string& errorOut);

}  // namespace editor
}  // namespace engine
