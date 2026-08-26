#pragma once

// ILayout (agente 2 §A item 1): the PUBLIC data-driven UI layout + data
// binding contract. Gameplay/editor UI must NOT hardcode pixel rectangles — a
// widget tree describes RELATIVE layout (weights + min/max + direction) and
// BINDS its visible/text properties to a typed value store; the engine
// resolves the tree into absolute rects deterministically. The engine has NO
// UI surface today (grep engine/ui in src/engine/public = zero); this contract
// turns UI layout into DATA + a pure resolution runtime:
//   - LAYOUT: a tree of LayoutNode. A node stacks its children along a
//     direction (row = horizontal, column = vertical); each child's main-axis
//     size is its `weight` share of the available space, its cross-axis size
//     fills the parent (minus margins), and min_w/max_w/min_h/max_h CLAMP the
//     result — so the same tree reflows responsively when the container
//     resizes (a panel with max_w=400 stops growing; a spacer with weight=1
//     absorbs leftover space).
//   - DATA BINDING: a node's `text_binding`/`visible_binding` are EXPRESSIONS
//     over a typed store (numbers/bools/strings keyed by name). `visible`
//     bound to a false value hides the node AND its subtree; `text` bound to a
//     value renders that value. Expressions support literals, `$key` store
//     references, and + - * / over numbers (full precedence).
//   - RESOLUTION: layout(width, height, store, out) walks the tree once in
//     pre-order and emits a flat vector of LayoutRect (absolute x/y/w/h +
//     resolved text + visible). PURE: the resolver never reads a window/GPU;
//     it only computes rects — the project draws them.
//   - PERSISTENCE: JSON versioned, bit-exact round-trip (load_from_json/
//     to_json), all-or-nothing on malformed input.
//
// Deterministic and headless: same tree + store + container size -> identical
// rects, bit-exact. Self-contained (std only). The SDK adapter
// (src/engine/sdk/UiLayout.cpp) is the ONLY TU with behavior.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace ui {

// A typed value (the data-binding vocabulary). Numbers are doubles; booleans
// and strings are first-class (no implicit conversion).
struct UiValue {
    enum class Kind : std::uint8_t { Number, Bool, String };
    Kind kind{ Kind::Number };
    double number{ 0.0 };
    bool boolean{ false };
    std::string string;

    bool operator==(const UiValue& other) const {
        if (kind != other.kind) return false;
        switch (kind) {
            case Kind::Number: return number == other.number;
            case Kind::Bool: return boolean == other.boolean;
            case Kind::String: return string == other.string;
        }
        return false;
    }
    bool operator!=(const UiValue& other) const { return !(*this == other); }
};

// The data-binding store: name -> typed value. Caller-owned plain data (a
// std::map keeps it header-only and deterministic — keys iterate sorted).
using UiStore = std::map<std::string, UiValue>;

// How a node stacks its children.
enum class LayoutDirection : std::uint8_t { Column, Row };

// One node of the widget tree. Relative, not absolute: absolute rects are the
// RESULT of resolution, never the input.
struct LayoutNode {
    std::string id;
    LayoutDirection direction{ LayoutDirection::Column };
    // Main-axis flex share relative to siblings. Must be > 0.
    double weight{ 1.0 };
    // Clamps (responsive). A value < 0 means "unbounded".
    double min_w{ 0.0 };
    double max_w{ -1.0 };
    double min_h{ 0.0 };
    double max_h{ -1.0 };
    // Uniform outer margin (spacing between siblings / around the node).
    double margin{ 0.0 };
    // Uniform inner padding (the content area children are laid into).
    double padding{ 0.0 };
    // Data bindings (expressions over the store). Empty = not bound.
    std::string text_binding;    // resolves to string/number/bool for display
    std::string visible_binding; // resolves to bool; false hides the subtree
    std::vector<LayoutNode> children;
};

// The full data-driven layout, validated all-or-nothing (never clamped).
struct UiLayoutSpec {
    int version{ 1 };
    std::string root; // id of the root node (must exist, exactly one)
    LayoutNode tree;

    bool validate(std::string& errorOut) const;
    bool load_from_json(const std::string& jsonText, std::string& errorOut);
    std::string to_json() const;
};

// A resolved rectangle (absolute, in the container's coordinate space).
struct LayoutRect {
    std::string id;
    double x{ 0.0 };
    double y{ 0.0 };
    double w{ 0.0 };
    double h{ 0.0 };
    bool visible{ true };
    std::string text;
};

// Evaluates a data-binding expression against a store. Grammar (full
// precedence): expr := add; add := mul (('+'|'-') mul)*; mul := unary
// (('*'|'/') unary)*; unary := '-' unary | primary; primary := number | 'true'
// | 'false' | '"'string'"' | '$'ident | '(' expr ')'. Numbers produce Number,
// bools Bool, strings String; arithmetic requires Number operands.
bool eval_binding(const std::string& expr, const UiStore& store, UiValue& out,
                  std::string& errorOut);

// The deterministic layout runtime. PURE — it computes rects, never draws.
class IUiLayout {
public:
    virtual ~IUiLayout() = default;

    // Resolves the tree into absolute rects. `store` supplies the data
    // bindings. Emits in pre-order (root first, then children, left-to-right).
    // `out` is cleared before filling. Returns false (and leaves `out` empty)
    // when a binding fails to evaluate.
    virtual bool layout(double width, double height, const UiStore& store,
                        std::vector<LayoutRect>& out,
                        std::string& errorOut) = 0;

    virtual const UiLayoutSpec& spec() const = 0;
};

// Parses+validates a spec and compiles it (rejected -> nullptr + errorOut).
std::unique_ptr<IUiLayout> create_ui_layout(const UiLayoutSpec& spec,
                                            std::string& errorOut);

}  // namespace ui
}  // namespace engine
