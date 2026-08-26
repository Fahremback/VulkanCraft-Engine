#pragma once

// IWidgets (agente 2 §A item 3): the PUBLIC interactive-widget contract —
// bars, modals and grid focus navigation (keyboard/controller). ILayout
// (#177) resolves RECTS; this contract resolves the interactive STATE of
// controls: a bar's value fraction, a modal's visibility/title, and
// deterministic focus movement across a grid of navigable ids. Everything is
// data-driven through the SAME UiStore binding grammar (eval_binding from
// ILayout.hpp) and PURE — no window/GPU/input device; the project feeds
// direction/activation intent, the contract decides.
//   - BAR: a value bound to the store (expression), clamped to [min, max];
//     `fraction` is the 0..1 fill ratio (for rendering), `label` the resolved
//     display text. min/max are data (health bars, cooldowns, ammo…).
//   - MODAL: visibility and title are store bindings; confirm/cancel labels
//     are spec literals and on_confirm/on_cancel are ACTION STRINGS the
//     project interprets (authority/confirmation stays with the project).
//   - FOCUS NAVIGATION: a grid of navigable ids (row-major over `cols`
//     columns) with directional moves Up/Down/Left/Right. Movement is
//     deterministic and wraps at the grid edges when `wrap` is true (the
//     default); with wrap=false a move that would leave the grid is refused
//     (moved=false, focus stays). Starting from "" focuses the FIRST id.
//
// Deterministic and headless: same doc + store -> identical bar/modal states
// and focus moves, bit-exact. Self-contained (std + engine/ui only). The SDK
// adapter (src/engine/sdk/UiWidgets.cpp) is the ONLY TU with behavior.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/ui/ILayout.hpp" // UiStore / UiValue (shared binding grammar)

namespace engine {
namespace ui {

// ---- Bars ---------------------------------------------------------------

struct UiBarSpec {
    std::string id;
    std::string value_binding; // expr -> number (required)
    std::string label_binding; // expr -> string/number/bool (optional)
    double min{ 0.0 };
    double max{ 1.0 };
    bool horizontal{ true };

    bool operator==(const UiBarSpec& other) const {
        return id == other.id && value_binding == other.value_binding &&
               label_binding == other.label_binding && min == other.min &&
               max == other.max && horizontal == other.horizontal;
    }
    bool operator!=(const UiBarSpec& other) const { return !(*this == other); }
};

// ---- Modals -------------------------------------------------------------

struct UiModalSpec {
    std::string id;
    std::string title_binding;    // expr -> string/number/bool (required)
    std::string visible_binding;  // expr -> bool (required)
    std::string confirm_label{ "Confirm" };
    std::string cancel_label{ "Cancel" };
    std::string on_confirm;       // action string (project-owned)
    std::string on_cancel;        // action string (project-owned)

    bool operator==(const UiModalSpec& other) const {
        return id == other.id && title_binding == other.title_binding &&
               visible_binding == other.visible_binding &&
               confirm_label == other.confirm_label &&
               cancel_label == other.cancel_label &&
               on_confirm == other.on_confirm && on_cancel == other.on_cancel;
    }
    bool operator!=(const UiModalSpec& other) const { return !(*this == other); }
};

// ---- Focus navigation ---------------------------------------------------

enum class FocusDirection : std::uint8_t { Up, Down, Left, Right };

struct UiFocusSpec {
    std::string id;
    std::vector<std::string> ids; // navigable ids, grid order (row-major)
    int cols{ 1 };
    bool wrap{ true };

    bool operator==(const UiFocusSpec& other) const {
        return id == other.id && ids == other.ids && cols == other.cols &&
               wrap == other.wrap;
    }
    bool operator!=(const UiFocusSpec& other) const { return !(*this == other); }
};

// ---- Document -----------------------------------------------------------

// One JSON document describing ALL interactive widgets. Validated
// all-or-nothing (never clamped); JSON versioned, bit-exact round-trip.
struct WidgetsDoc {
    int version{ 1 };
    std::vector<UiBarSpec> bars;
    std::vector<UiModalSpec> modals;
    std::vector<UiFocusSpec> focus;

    bool validate(std::string& errorOut) const;
    bool load_from_json(const std::string& jsonText, std::string& errorOut);
    std::string to_json() const;
};

// ---- Resolved state -----------------------------------------------------

struct UiBarState {
    std::string id;
    double value{ 0.0 };
    double fraction{ 0.0 }; // clamp((value-min)/(max-min), 0, 1)
    std::string label;
};

struct UiModalState {
    std::string id;
    bool visible{ false };
    std::string title;
    std::string confirm_label;
    std::string cancel_label;
};

struct UiFocusMove {
    std::string from;  // "" when starting
    std::string to;    // the focused id after the move
    bool moved{ false }; // false when from is unknown or the edge blocks
};

class IUiWidgets {
public:
    virtual ~IUiWidgets() = default;

    // Resolves every bar against the store, in document order.
    virtual std::vector<UiBarState> resolve_bars(
        const UiStore& store, std::string& errorOut) const = 0;

    // Resolves every modal against the store, in document order.
    virtual std::vector<UiModalState> resolve_modals(
        const UiStore& store, std::string& errorOut) const = 0;

    // Deterministic focus movement over a named grid. `from` "" starts at the
    // first id. Returns moved=false with a diagnostic when the grid id is
    // unknown or a non-wrap move would leave the grid.
    virtual UiFocusMove move_focus(const std::string& focusId,
                                   const std::string& from,
                                   FocusDirection direction,
                                   std::string& errorOut) const = 0;

    virtual const WidgetsDoc& spec() const = 0;
};

// Parses+validates a doc and compiles it (rejected -> nullptr + errorOut).
std::unique_ptr<IUiWidgets> create_ui_widgets(const WidgetsDoc& doc,
                                              std::string& errorOut);

}  // namespace ui
}  // namespace engine
