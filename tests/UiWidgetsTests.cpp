// UiWidgetsTests — headless coverage for the public interactive-widget
// contract (engine/ui/IWidgets.hpp, adapter UiWidgets.cpp): bars (value
// binding + fraction clamp + label), modals (visible/title via store), and
// deterministic grid focus navigation (4 directions, wrap on/off, short last
// row). Standalone main() with CHECK (pattern: UiLayoutTests).

#include "engine/ui/IWidgets.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

using namespace engine::ui;

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "UiWidgetsTests failure at line " << __LINE__ << ": " #condition "\n"; \
    return false; } } while (false)

namespace {

bool run_all() {
    std::string err;

    // ---- Document: validate + JSON round-trip ---------------------------
    WidgetsDoc doc;
    {
        UiBarSpec hp;
        hp.id = "hp";
        hp.value_binding = "$hp";
        hp.min = 0.0;
        hp.max = 100.0;
        hp.horizontal = true;
        doc.bars.push_back(hp);

        UiBarSpec cooldown;
        cooldown.id = "cooldown";
        cooldown.value_binding = "$cd";
        cooldown.label_binding = "\"ready\"";
        cooldown.min = 0.0;
        cooldown.max = 1.0;
        doc.bars.push_back(cooldown);

        UiModalSpec pause;
        pause.id = "pause_menu";
        pause.title_binding = "\"Paused\"";
        pause.visible_binding = "$paused";
        pause.confirm_label = "Resume";
        pause.cancel_label = "Quit";
        pause.on_confirm = "resume";
        pause.on_cancel = "quit";
        doc.modals.push_back(pause);

        UiFocusSpec menu;
        menu.id = "main_menu";
        menu.ids = { "btn_new", "btn_load", "btn_options", "btn_quit" };
        menu.cols = 2;
        menu.wrap = true;
        doc.focus.push_back(menu);
    }
    CHECK(doc.validate(err));

    const std::string json = doc.to_json();
    WidgetsDoc back;
    CHECK(back.load_from_json(json, err));
    CHECK(back.bars.size() == 2);
    CHECK(back.bars[0].id == "hp");
    CHECK(back.bars[0].min == 0.0 && back.bars[0].max == 100.0);
    CHECK(back.bars[1].id == "cooldown");
    CHECK(back.bars[1].label_binding == "\"ready\"");
    CHECK(back.modals.size() == 1);
    CHECK(back.modals[0].id == "pause_menu");
    CHECK(back.modals[0].confirm_label == "Resume");
    CHECK(back.modals[0].on_confirm == "resume");
    CHECK(back.focus.size() == 1);
    CHECK(back.focus[0].ids == std::vector<std::string>(
        { "btn_new", "btn_load", "btn_options", "btn_quit" }));
    CHECK(back.focus[0].cols == 2);
    CHECK(back.to_json() == json); // bit-exact

    // Malformed refused all-or-nothing (target untouched).
    WidgetsDoc keep = back;
    CHECK(!back.load_from_json("{bad", err));
    CHECK(back.to_json() == keep.to_json());

    // ---- Validation refusals --------------------------------------------
    WidgetsDoc bad;
    bad.version = 2;
    CHECK(!bad.validate(err));
    bad.version = 1;

    WidgetsDoc badBar = doc;
    badBar.bars[0].value_binding.clear();
    CHECK(!badBar.validate(err));
    badBar = doc;
    badBar.bars[0].max = 0.0; // max <= min
    CHECK(!badBar.validate(err));
    badBar = doc;
    badBar.bars[0].id.clear();
    CHECK(!badBar.validate(err));

    WidgetsDoc badModal = doc;
    badModal.modals[0].visible_binding.clear();
    CHECK(!badModal.validate(err));
    badModal = doc;
    badModal.modals[0].title_binding.clear();
    CHECK(!badModal.validate(err));

    WidgetsDoc badFocus = doc;
    badFocus.focus[0].ids.clear();
    CHECK(!badFocus.validate(err));
    badFocus = doc;
    badFocus.focus[0].ids[1] = "btn_new"; // duplicate
    CHECK(!badFocus.validate(err));
    badFocus = doc;
    badFocus.focus[0].cols = 0;
    CHECK(!badFocus.validate(err));

    // ---- Runtime ---------------------------------------------------------
    auto widgets = create_ui_widgets(doc, err);
    CHECK(widgets != nullptr);

    UiStore store;
    store["hp"] = UiValue{ UiValue::Kind::Number, 25.0, false, {} };
    store["cd"] = UiValue{ UiValue::Kind::Number, 0.5, false, {} };
    store["paused"] = UiValue{ UiValue::Kind::Bool, 0.0, true, {} };

    // Bars: fraction + label.
    const std::vector<UiBarState> bars = widgets->resolve_bars(store, err);
    CHECK(bars.size() == 2);
    CHECK(bars[0].id == "hp");
    CHECK(bars[0].value == 25.0);
    CHECK(bars[0].fraction == 0.25);
    CHECK(bars[1].label == "ready");

    // Bar clamp: value out of range.
    store["hp"] = UiValue{ UiValue::Kind::Number, 200.0, false, {} };
    const std::vector<UiBarState> clamped = widgets->resolve_bars(store, err);
    CHECK(clamped[0].fraction == 1.0);
    store["hp"] = UiValue{ UiValue::Kind::Number, -10.0, false, {} };
    const std::vector<UiBarState> clampedLow = widgets->resolve_bars(store, err);
    CHECK(clampedLow[0].fraction == 0.0);

    // Bar binding failure -> empty (all-or-nothing).
    UiStore missing;
    CHECK(widgets->resolve_bars(missing, err).empty());
    CHECK(!err.empty());

    // Modals: visible + title + labels.
    const std::vector<UiModalState> modals = widgets->resolve_modals(store, err);
    CHECK(modals.size() == 1);
    CHECK(modals[0].visible); // paused=true
    CHECK(modals[0].title == "Paused");
    CHECK(modals[0].confirm_label == "Resume");
    CHECK(modals[0].cancel_label == "Quit");

    UiStore unpaused = store;
    unpaused["paused"] = UiValue{ UiValue::Kind::Bool, 0.0, false, {} };
    const std::vector<UiModalState> hidden = widgets->resolve_modals(unpaused, err);
    CHECK(!hidden[0].visible);

    // ---- Focus navigation (2 cols, 4 ids: grid = [0,1; 2,3]) ------------
    // "" starts at the first id.
    UiFocusMove mv = widgets->move_focus("main_menu", "", FocusDirection::Down, err);
    CHECK(mv.moved && mv.to == "btn_new");

    // btn_new (0,0) -> Right -> btn_load (0,1); Down -> btn_options (1,0).
    mv = widgets->move_focus("main_menu", "btn_new", FocusDirection::Right, err);
    CHECK(mv.moved && mv.to == "btn_load");
    mv = widgets->move_focus("main_menu", "btn_new", FocusDirection::Down, err);
    CHECK(mv.moved && mv.to == "btn_options");
    // btn_load (0,1) -> Down -> btn_quit (1,1); Left -> btn_new.
    mv = widgets->move_focus("main_menu", "btn_load", FocusDirection::Down, err);
    CHECK(mv.moved && mv.to == "btn_quit");
    mv = widgets->move_focus("main_menu", "btn_load", FocusDirection::Left, err);
    CHECK(mv.moved && mv.to == "btn_new");
    // Up from btn_quit (1,1) -> btn_load.
    mv = widgets->move_focus("main_menu", "btn_quit", FocusDirection::Up, err);
    CHECK(mv.moved && mv.to == "btn_load");

    // Wrap: Right from btn_load (0,1) wraps to btn_new (0,0); Down from
    // btn_quit (1,1) wraps to btn_load (0,1).
    mv = widgets->move_focus("main_menu", "btn_load", FocusDirection::Right, err);
    CHECK(mv.moved && mv.to == "btn_new");
    mv = widgets->move_focus("main_menu", "btn_quit", FocusDirection::Down, err);
    CHECK(mv.moved && mv.to == "btn_load");

    // Unknown grid / unknown from -> diagnostic.
    CHECK(!widgets->move_focus("nope", "", FocusDirection::Down, err).moved);
    CHECK(!err.empty());
    err.clear();
    CHECK(!widgets->move_focus("main_menu", "btn_missing", FocusDirection::Down, err).moved);
    CHECK(!err.empty());

    // ---- Non-wrap grid ---------------------------------------------------
    WidgetsDoc nowrapDoc = doc;
    nowrapDoc.focus[0].wrap = false;
    auto nw = create_ui_widgets(nowrapDoc, err);
    CHECK(nw != nullptr);
    // Right from btn_load (0,1) at the right edge: blocked, stays.
    mv = nw->move_focus("main_menu", "btn_load", FocusDirection::Right, err);
    CHECK(!mv.moved && mv.to == "btn_load");
    // Down from btn_quit (1,1) at the bottom: blocked.
    mv = nw->move_focus("main_menu", "btn_quit", FocusDirection::Down, err);
    CHECK(!mv.moved && mv.to == "btn_quit");
    // Internal moves still work.
    mv = nw->move_focus("main_menu", "btn_new", FocusDirection::Right, err);
    CHECK(mv.moved && mv.to == "btn_load");

    // ---- Short last row (5 ids, 3 cols: [0,1,2; 3,4]) --------------------
    WidgetsDoc shortDoc = doc;
    shortDoc.focus[0].ids = { "a", "b", "c", "d", "e" };
    shortDoc.focus[0].cols = 3;
    auto sd = create_ui_widgets(shortDoc, err);
    CHECK(sd != nullptr);
    // d (1,0) -> Right -> e (1,1); e (1,1) -> Right -> phantom (1,2): blocked.
    mv = sd->move_focus("main_menu", "d", FocusDirection::Right, err);
    CHECK(mv.moved && mv.to == "e");
    mv = sd->move_focus("main_menu", "e", FocusDirection::Right, err);
    CHECK(!mv.moved && mv.to == "e");
    // b (0,1) -> Down -> e (1,1).
    mv = sd->move_focus("main_menu", "b", FocusDirection::Down, err);
    CHECK(mv.moved && mv.to == "e");

    // ---- Determinism cross-instance --------------------------------------
    auto w1 = create_ui_widgets(doc, err);
    auto w2 = create_ui_widgets(doc, err);
    const std::vector<UiBarState> b1 = w1->resolve_bars(store, err);
    const std::vector<UiBarState> b2 = w2->resolve_bars(store, err);
    CHECK(b1.size() == b2.size());
    for (std::size_t i = 0; i < b1.size(); ++i) {
        CHECK(b1[i].id == b2[i].id);
        CHECK(b1[i].value == b2[i].value);
        CHECK(b1[i].fraction == b2[i].fraction);
        CHECK(b1[i].label == b2[i].label);
    }
    const UiFocusMove f1 = w1->move_focus("main_menu", "btn_new", FocusDirection::Down, err);
    const UiFocusMove f2 = w2->move_focus("main_menu", "btn_new", FocusDirection::Down, err);
    CHECK(f1.to == f2.to && f1.moved == f2.moved);

    std::cout << "UiWidgetsTests: all checks passed\n";
    return true;
}

}  // namespace

int main() {
    if (!run_all()) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
