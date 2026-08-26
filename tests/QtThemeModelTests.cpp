// Gate for engine::ui::IQtThemeModel (agente 2 §B — porte Qt) — the Wicked
// charcoal theme in Qt-native form (QPalette roles + QSS). Headless: no Qt,
// no window, no clock.

#include "engine/ui/qt/IQtThemeModel.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                            \
    do {                                                            \
        if (!(cond)) {                                              \
            std::printf("FAIL %s:%d — %s\n", __FILE__, __LINE__, msg); \
            ++g_failures;                                           \
        }                                                           \
    } while (0)

void expect_refused(bool ok, const char* what) {
    if (ok) {
        std::printf("FAIL %s:%d — %s should have been refused\n", __FILE__, __LINE__, what);
        ++g_failures;
    }
}

engine::ui::QtRgba rgba(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return engine::ui::QtRgba{ r, g, b, 255 };
}

// The editor's live charcoal base colors (same values as WickedToolsPanel).
const engine::ui::QtRgba kBg = rgba(26, 28, 36);      // 0.10/0.11/0.14
const engine::ui::QtRgba kPanel = rgba(51, 51, 51);   // 0.20
const engine::ui::QtRgba kText = rgba(217, 217, 217); // 0.85

void test_derive_charcoal_palette() {
    const auto palette = engine::ui::derive_charcoal_palette(kBg, kPanel, kText);
    CHECK(palette.size() == 12, "12 roles");
    auto find = [&palette](engine::ui::QtPaletteRole role) -> engine::ui::QtRgba {
        for (const auto& [r, c] : palette) {
            if (r == role) return c;
        }
        return engine::ui::QtRgba{ 0, 0, 0, 255 };
    };
    CHECK(find(engine::ui::QtPaletteRole::Window) == rgba(26, 28, 36),
          "Window = bg (#1A1C24)");
    CHECK(find(engine::ui::QtPaletteRole::Base) == rgba(51, 51, 51),
          "Base = panel (#333333)");
    // 51 + 0.08*255 = 51 + 20.4 -> 71 (#474747).
    CHECK(find(engine::ui::QtPaletteRole::Button) == rgba(71, 71, 71),
          "Button = panel lifted 8% (#474747)");
    CHECK(find(engine::ui::QtPaletteRole::Text) == rgba(217, 217, 217),
          "Text (#D9D9D9)");
    CHECK(find(engine::ui::QtPaletteRole::Highlight) == rgba(96, 160, 255),
          "Highlight = accent (#60A0FF)");
    CHECK(find(engine::ui::QtPaletteRole::HighlightedText) == rgba(255, 255, 255),
          "HighlightedText = white");
    // 51 + 0.04*255 = 51 + 10.2 -> 61 (#3D3D3D).
    CHECK(find(engine::ui::QtPaletteRole::AlternateBase) == rgba(61, 61, 61),
          "AlternateBase = panel lifted 4% (#3D3D3D)");
    // 217 * 0.55 = 119.35 -> 119 (#777777).
    CHECK(find(engine::ui::QtPaletteRole::PlaceholderText) == rgba(119, 119, 119),
          "PlaceholderText = text at 55% (#777777)");
    // bg is (26,28,36) — NOT gray; each channel lifts independently:
    // 26+38=64 (0x40), 28+38=66 (0x42), 36+38=74 (0x4A) -> #42424A.
    CHECK(find(engine::ui::QtPaletteRole::ToolTipBase) == rgba(64, 66, 74),
          "ToolTipBase = bg lifted 15% (#42424A)");
}

void test_hex_format() {
    CHECK(engine::ui::rgba_to_hex(rgba(26, 28, 36)) == "#1A1C24",
          "hex uppercase #RRGGBB");
    CHECK(engine::ui::rgba_to_hex(rgba(96, 160, 255)) == "#60A0FF",
          "hex accent");
    CHECK(engine::ui::rgba_to_hex(rgba(255, 255, 255)) == "#FFFFFF",
          "hex white");
}

void test_qss_from_palette() {
    const auto palette = engine::ui::derive_charcoal_palette(kBg, kPanel, kText);
    const auto qss = engine::ui::qss_from_palette(palette);
    CHECK(qss.size() == 7, "7 selectors");
    CHECK(qss[0].selector == "QMainWindow", "first selector QMainWindow");
    CHECK(qss[0].declarations == "background:#1A1C24;", "main window bg hex");
    CHECK(qss[1].selector == "QDockWidget", "dock selector");
    CHECK(qss[1].declarations == "color:#D9D9D9;", "dock text color");
    CHECK(qss[2].selector == "QTreeView, QListView, QTableView",
          "views selector");
    CHECK(qss[2].declarations.find("background:#333333;") != std::string::npos,
          "views base");
    CHECK(qss[2].declarations.find("selection-background-color:#60A0FF;") != std::string::npos,
          "views highlight");
    CHECK(qss[3].selector.find("QPushButton") != std::string::npos,
          "buttons selector");
    CHECK(qss[3].declarations.find("background-color:#474747;") != std::string::npos,
          "button tone");
    CHECK(qss[5].selector == "QLineEdit, QTextEdit, QPlainTextEdit",
          "editors selector");
    CHECK(qss[6].selector == "QToolTip", "tooltip selector");
    CHECK(qss[6].declarations.find("background:#40424A;") != std::string::npos,
          "tooltip base");
}

void test_set_theme_and_all_or_nothing() {
    auto theme = engine::ui::create_qt_theme_model();
    std::string err;

    engine::ui::QtThemeSnapshot snap;
    snap.name = "charcoal";
    snap.palette = engine::ui::derive_charcoal_palette(kBg, kPanel, kText);
    snap.qss = engine::ui::qss_from_palette(snap.palette);
    CHECK(theme->set_theme(snap, err), "set_theme accepted");

    // Missing name refused.
    engine::ui::QtThemeSnapshot bad = snap;
    bad.name.clear();
    expect_refused(theme->set_theme(bad, err), "empty name");
    // Duplicate role refused.
    bad = snap;
    bad.palette.push_back({ engine::ui::QtPaletteRole::Window, rgba(1, 1, 1) });
    expect_refused(theme->set_theme(bad, err), "duplicate role");
    // Missing core role (Highlight) refused.
    bad = snap;
    bad.palette.erase(bad.palette.begin() + 7);
    expect_refused(theme->set_theme(bad, err), "missing Highlight");
    // Duplicate qss selector refused.
    bad = snap;
    bad.qss.push_back(bad.qss[0]);
    expect_refused(theme->set_theme(bad, err), "duplicate selector");

    // Theme untouched after refusals.
    CHECK(theme->snapshot().name == "charcoal", "theme untouched after refusals");
}

void test_set_qss_and_accessors() {
    auto theme = engine::ui::create_qt_theme_model();
    std::string err;
    engine::ui::QtThemeSnapshot snap;
    snap.name = "charcoal";
    snap.palette = engine::ui::derive_charcoal_palette(kBg, kPanel, kText);
    snap.qss = engine::ui::qss_from_palette(snap.palette);
    CHECK(theme->set_theme(snap, err), "set_theme accepted");

    // Replace QSS all-or-nothing.
    engine::ui::QtQssRule r;
    r.selector = "QMainWindow";
    r.declarations = "background:#111111;";
    CHECK(theme->set_qss({ r }, err), "set_qss accepted");
    expect_refused(theme->set_qss({ { "", "x" } }, err), "empty selector refused");
    expect_refused(theme->set_qss({ { "A", "x" }, { "A", "y" } }, err),
                   "duplicate selector refused");
    CHECK(theme->qss_rule("QMainWindow") != nullptr &&
          theme->qss_rule("QMainWindow")->declarations == "background:#111111;",
          "qss replaced");
    CHECK(theme->qss_rule("QTreeView") == nullptr, "old rule gone");

    const auto* color = theme->color(engine::ui::QtPaletteRole::Window);
    CHECK(color != nullptr && *color == rgba(26, 28, 36), "color accessor");
    CHECK(theme->color(static_cast<engine::ui::QtPaletteRole>(99)) == nullptr,
          "unknown role is nullptr");
}

void test_json_deterministic() {
    auto theme = engine::ui::create_qt_theme_model();
    std::string err;
    engine::ui::QtThemeSnapshot snap;
    snap.name = "charcoal";
    snap.palette = engine::ui::derive_charcoal_palette(kBg, kPanel, kText);
    snap.qss = engine::ui::qss_from_palette(snap.palette);
    CHECK(theme->set_theme(snap, err), "set_theme accepted");
    const std::string json = theme->to_json();
    CHECK(json == theme->to_json(), "to_json deterministic");
    CHECK(json.find("\"name\":\"charcoal\"") != std::string::npos, "json name");
    CHECK(json.find("\"Window\":\"#1A1C24\"") != std::string::npos, "json Window hex");
    CHECK(json.find("\"Highlight\":\"#60A0FF\"") != std::string::npos, "json Highlight hex");
    CHECK(json.find("\"selector\":\"QMainWindow\"") != std::string::npos, "json selector");
    CHECK(json.find("\"declarations\":\"background:#1A1C24;\"") != std::string::npos,
          "json declarations");
}

}  // namespace

int main() {
    test_derive_charcoal_palette();
    test_hex_format();
    test_qss_from_palette();
    test_set_theme_and_all_or_nothing();
    test_set_qss_and_accessors();
    test_json_deterministic();

    if (g_failures == 0) {
        std::printf("ALL PASSED — qt_theme_model_tests\n");
        return 0;
    }
    std::printf("%d failure(s) — qt_theme_model_tests\n", g_failures);
    return 1;
}
