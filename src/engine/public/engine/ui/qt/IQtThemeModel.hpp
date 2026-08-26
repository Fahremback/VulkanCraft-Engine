#pragma once

// IQtThemeModel (agente 2 §B — porte Qt, decisão do usuário): the PUBLIC,
// deterministic model of the Qt editor theme — the Wicked charcoal theme
// expressed in Qt-native form (QPalette roles + Qt Style Sheet selectors).
// The current ImGui theme applies charcoal via ImGuiStyle; a Qt shell needs
// the same colors as QPalette roles (window/base/text/button/highlight) and
// QSS rules for the key widget types (QMainWindow, QDockWidget, views,
// buttons, menus, status bar, tool bar, editors, tooltips). The derivation
// from the live base colors (bg/panel) to the full palette is a deterministic
// function in the SDK adapter (same lift math the ImGui theme uses, so both
// UIs stay in sync by construction).
// UNEQUIVOCAL: set_palette/set_qss are all-or-nothing — missing core roles,
// unknown roles and duplicate selectors are refused with a reason, leaving
// the theme untouched. DETERMINISM: no clocks/RNG; colors are bytes, JSON is
// bit-exact (hex #RRGGBB, uppercase). OBSERVABLE: the editor exposes the
// theme via GET /qt-theme.
//
// Self-contained (std only). The SDK adapter (src/engine/sdk/QtThemeModel.cpp)
// is the ONLY TU with behavior.

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace engine {
namespace ui {

// QPalette::ColorRole analog (focused set the shell actually uses).
enum class QtPaletteRole : std::uint8_t {
    Window,
    WindowText,
    Base,
    AlternateBase,
    Text,
    Button,
    ButtonText,
    Highlight,
    HighlightedText,
    PlaceholderText,
    ToolTipBase,
    ToolTipText
};

const char* qt_palette_role_name(QtPaletteRole role);

// 8-bit RGBA color (bytes only — bit-exact JSON, no floats).
struct QtRgba {
    std::uint8_t r{ 0 };
    std::uint8_t g{ 0 };
    std::uint8_t b{ 0 };
    std::uint8_t a{ 255 };

    bool operator==(const QtRgba& other) const {
        return r == other.r && g == other.g && b == other.b && a == other.a;
    }
    bool operator!=(const QtRgba& other) const { return !(*this == other); }
};

// One Qt Style Sheet rule: a selector and its declarations, e.g.
// selector "QMainWindow", declarations "background:#1A1C24;".
struct QtQssRule {
    std::string selector;      // unique (e.g. "QTreeView, QListView, QTableView")
    std::string declarations;  // e.g. "background:#333333;color:#D9D9D9;"

    bool operator==(const QtQssRule& other) const {
        return selector == other.selector && declarations == other.declarations;
    }
    bool operator!=(const QtQssRule& other) const { return !(*this == other); }
};

// The full theme snapshot.
struct QtThemeSnapshot {
    std::string name;                                     // e.g. "charcoal"
    std::vector<std::pair<QtPaletteRole, QtRgba>> palette;  // role -> color
    std::vector<QtQssRule> qss;                           // ordered selectors

    bool operator==(const QtThemeSnapshot& other) const {
        return name == other.name && palette == other.palette &&
               qss == other.qss;
    }
    bool operator!=(const QtThemeSnapshot& other) const {
        return !(*this == other);
    }
};

// Derives the full charcoal palette from the live base colors. Deterministic:
//   Window        = bg
//   WindowText    = text
//   Base          = panel
//   AlternateBase = panel lifted 4%
//   Text          = text
//   Button        = panel lifted 8% (same lift the ImGui theme applies)
//   ButtonText    = text
//   Highlight     = accent (60,160,255) — documented constant
//   HighlightedText = white
//   PlaceholderText = text dimmed to 55%
//   ToolTipBase   = bg lifted 15%
//   ToolTipText   = text
std::vector<std::pair<QtPaletteRole, QtRgba>> derive_charcoal_palette(
    QtRgba bg, QtRgba panel, QtRgba text);

// Derives the ordered QSS rules from a derived palette (deterministic
// selectors, uppercase #RRGGBB). Selectors cover the widget types the editor
// shell hosts (main window, docks, views, buttons/inputs, menus/status/tool
// bar, editors, tooltips).
std::vector<QtQssRule> qss_from_palette(
    const std::vector<std::pair<QtPaletteRole, QtRgba>>& palette);

// #RRGGBB (uppercase) from a color (alpha ignored).
std::string rgba_to_hex(const QtRgba& color);

class IQtThemeModel {
public:
    virtual ~IQtThemeModel() = default;

    // Installs the whole theme (all-or-nothing). REFUSED (returns false,
    // theme untouched, reason in errorOut) when: the name is empty, a role
    // repeats, a role is unknown, or the palette misses the core roles
    // Window/Base/Text/Button/Highlight.
    virtual bool set_theme(const QtThemeSnapshot& theme,
                           std::string& errorOut) = 0;

    // Replaces the QSS rules (all-or-nothing: duplicate or empty selector
    // refused). The palette is kept; the shell re-derives QSS when colors
    // change and calls this.
    virtual bool set_qss(const std::vector<QtQssRule>& qss,
                         std::string& errorOut) = 0;

    // Const accessors (nullptr when unknown).
    virtual const QtRgba* color(QtPaletteRole role) const = 0;
    virtual const QtQssRule* qss_rule(const std::string& selector) const = 0;

    virtual QtThemeSnapshot snapshot() const = 0;

    // Deterministic JSON: {name, palette{role:#RRGGBB...}, qss[{selector,
    // declarations}]} — bit-exact.
    virtual std::string to_json() const = 0;
};

// Factory: the SDK adapter is the only TU with behavior.
std::unique_ptr<IQtThemeModel> create_qt_theme_model();

}  // namespace ui
}  // namespace engine
