// UiDocTests — headless coverage for the public UI-composition contract
// (engine/ui/IUiDoc.hpp, adapter UiDoc.cpp): one versioned document that
// composes layout + widgets + viewport + confirmations, delegating JSON to
// each sub-contract. Covers validate, bit-exact round-trip, all-or-nothing
// refusals (a bad sub-document never mutates the target), the optional
// confirmations field, and cross-instance determinism. Standalone main()
// with CHECK (pattern: UiWidgetsTests).

#include "engine/ui/IUiDoc.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

using namespace engine::ui;

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "UiDocTests failure at line " << __LINE__ << ": " #condition "\n"; \
    return false; } } while (false)

namespace {

UiDoc make_doc() {
    UiDoc doc;

    doc.layout.version = 1;
    doc.layout.root = "screen";
    doc.layout.tree.id = "screen";
    LayoutNode content;
    content.id = "content";
    content.weight = 1.0;
    doc.layout.tree.children.push_back(content);

    UiBarSpec hp;
    hp.id = "hp";
    hp.value_binding = "$hp";
    hp.min = 0.0;
    hp.max = 100.0;
    doc.widgets.bars.push_back(hp);

    UiModalSpec modal;
    modal.id = "confirm_buy";
    modal.title_binding = "\"Buy sword?\"";
    modal.visible_binding = "$buy_open";
    modal.on_confirm = "server:buy:42";
    doc.widgets.modals.push_back(modal);

    UiFocusSpec focus;
    focus.id = "menu";
    focus.ids = { "hp", "confirm_buy" };
    focus.cols = 2;
    doc.widgets.focus.push_back(focus);

    doc.viewport.version = 1;
    doc.viewport.reference_width = 1920.0;
    doc.viewport.reference_height = 1080.0;
    doc.viewport.scale_mode = UiScaleMode::Fit;
    doc.viewport.safe_area.left = 20.0;
    doc.viewport.safe_area.top = 40.0;
    doc.viewport.text_scale = 1.25;

    ConfirmActionSpec buy;
    buy.id = "buy_sword";
    buy.title = "Buy sword";
    buy.severity = ConfirmSeverity::Danger;
    buy.payload = "{\"item\":\"sword\",\"price\":10}";
    buy.on_confirm = "server:buy:42";
    doc.confirmations.push_back(buy);

    return doc;
}

bool run_all() {
    std::string err;

    // ---- Validate a composed doc -----------------------------------------
    {
        const UiDoc doc = make_doc();
        CHECK(doc.validate(err));
    }

    // ---- Bit-exact round-trip --------------------------------------------
    {
        const UiDoc doc = make_doc();
        const std::string jsonText = doc.to_json();

        UiDoc back;
        CHECK(back.load_from_json(jsonText, err));
        CHECK(back.to_json() == jsonText);  // bit-exact re-emit
        // Data identical: reloading the re-emitted text yields the same text.
        UiDoc again;
        CHECK(again.load_from_json(back.to_json(), err));
        CHECK(again.to_json() == jsonText);
    }

    // ---- Optional confirmations field ------------------------------------
    {
        UiDoc doc = make_doc();
        doc.confirmations.clear();
        const std::string jsonText = doc.to_json();

        UiDoc back;
        CHECK(back.load_from_json(jsonText, err));
        CHECK(back.confirmations.empty());
        CHECK(back.to_json() == jsonText);
    }

    // ---- All-or-nothing refusals -----------------------------------------
    {
        // Malformed top-level JSON.
        UiDoc untouched = make_doc();
        CHECK(!untouched.load_from_json("{not json", err));
        CHECK(untouched.to_json() == make_doc().to_json());

        // Missing required layout field.
        const std::string noLayout =
            "{\"version\":1,\"widgets\":{\"version\":1,\"bars\":[],"
            "\"modals\":[],\"focus\":[]},\"viewport\":{\"version\":1,"
            "\"reference_width\":1920,\"reference_height\":1080,"
            "\"scale_mode\":\"fit\"},\"confirmations\":[]}";
        CHECK(!untouched.load_from_json(noLayout, err));
        CHECK(untouched.to_json() == make_doc().to_json());

        // Invalid viewport (reference_width <= 0) — candidate never commits.
        const std::string badViewport =
            "{\"version\":1,\"layout\":{\"version\":1,\"root\":\"screen\","
            "\"tree\":{\"id\":\"screen\"}},\"widgets\":{\"version\":1,"
            "\"bars\":[],\"modals\":[],\"focus\":[]},\"viewport\":{\"version\":1,"
            "\"reference_width\":0,\"reference_height\":1080,"
            "\"scale_mode\":\"fit\"}}";
        CHECK(!untouched.load_from_json(badViewport, err));
        CHECK(untouched.to_json() == make_doc().to_json());

        // Invalid confirmation (empty id) — candidate never commits.
        const std::string badAction =
            "{\"version\":1,\"layout\":{\"version\":1,\"root\":\"screen\","
            "\"tree\":{\"id\":\"screen\"}},\"widgets\":{\"version\":1,"
            "\"bars\":[],\"modals\":[],\"focus\":[]},\"viewport\":{\"version\":1,"
            "\"reference_width\":1920,\"reference_height\":1080,"
            "\"scale_mode\":\"fit\"},\"confirmations\":[{\"id\":\"\","
            "\"title\":\"t\"}]}";
        CHECK(!untouched.load_from_json(badAction, err));
        CHECK(untouched.to_json() == make_doc().to_json());

        // Unsupported version.
        CHECK(!untouched.load_from_json("{\"version\":99}", err));
        CHECK(untouched.to_json() == make_doc().to_json());
    }

    // ---- validate() detects broken sub-contracts --------------------------
    {
        UiDoc broken = make_doc();
        broken.viewport.reference_width = 0.0;
        CHECK(!broken.validate(err));
        CHECK(!err.empty());
    }

    // ---- Determinism cross-instance --------------------------------------
    {
        const UiDoc doc = make_doc();
        const std::string jsonText = doc.to_json();
        for (int i = 0; i < 2; ++i) {
            UiDoc back;
            CHECK(back.load_from_json(jsonText, err));
            CHECK(back.to_json() == jsonText);
        }
    }

    std::cout << "UiDocTests: all checks passed\n";
    return true;
}

}  // namespace

int main() {
    if (!run_all()) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
