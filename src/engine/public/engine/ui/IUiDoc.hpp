#pragma once

// IUiDoc (agente 2 §A item 6): the PUBLIC UI-composition contract — ONE
// versioned JSON document that composes every UI contract of the engine/ui
// domain (ILayout #177 layout tree, IWidgets #183 bars/modals/focus,
// IViewport #187 scale/safe-area/a11y, IConfirmation #188 actions) so a
// screen can be described, validated, persisted and shipped as a single
// artifact. This is the data surface for reflection/scripting/MCP tooling:
// any tool that can read/write JSON can author or inspect a full screen.
//   - COMPOSITION: a UiDoc holds one layout spec, one widgets doc, one
//     viewport spec and N confirmation action specs. Validation and JSON
//     round-trip DELEGATE to each sub-contract's own all-or-nothing
//     validate()/load_from_json()/to_json() — no gameplay logic here.
//   - ALL-OR-NOTHING: load_from_json never partially applies — a malformed
//     sub-document leaves the target doc untouched.
//   - BIT-EXACT: to_json()/load_from_json() round-trip identically (same
//     %.9g number formatting and string escaping the sub-contracts use).
//
// Self-contained (std + engine/ui only). The SDK adapter
// (src/engine/sdk/UiDoc.cpp) is the ONLY TU with behavior — it re-emits the
// extracted JSON fields with a small local emitter (the shared RegistryJson
// parser has no emit path) and hands each to the matching sub-contract.

#include <string>
#include <vector>

#include "engine/ui/IConfirmation.hpp"
#include "engine/ui/ILayout.hpp"
#include "engine/ui/IViewport.hpp"
#include "engine/ui/IWidgets.hpp"

namespace engine {
namespace ui {

// The full screen composition. Pure data — validated all-or-nothing.
struct UiDoc {
    int version{ 1 };
    UiLayoutSpec layout;                  // required: the widget tree
    WidgetsDoc widgets;                   // required: bars/modals/focus
    ViewportSpec viewport;                // required: scale/safe-area/a11y
    std::vector<ConfirmActionSpec> confirmations;  // optional action list

    // Validates every sub-contract, in order (layout, widgets, viewport,
    // each confirmation). Clears errorOut on success.
    bool validate(std::string& errorOut) const;

    // All-or-nothing: a parse error or an invalid sub-document leaves the
    // target unchanged. `confirmations` may be absent (empty list).
    bool load_from_json(const std::string& jsonText, std::string& errorOut);

    // Composes the sub-contracts' own to_json() output. Bit-exact with
    // load_from_json().
    std::string to_json() const;
};

}  // namespace ui
}  // namespace engine
