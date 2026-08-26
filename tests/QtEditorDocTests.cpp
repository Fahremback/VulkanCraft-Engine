// Gate for engine::ui::IQtEditorDoc (agente 2 §B — porte Qt) — the
// deterministic Qt editor shell document model (docks/actions/menus/toolbars/
// status). Headless: no Qt, no window, no clock.

#include "engine/ui/qt/IQtEditorDoc.hpp"

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

engine::ui::QtEditorDocSnapshot sample_doc() {
    engine::ui::QtEditorDocSnapshot doc;
    doc.version = "qt-editor-doc-1";
    engine::ui::QtDockSpec hierarchy;
    hierarchy.objectName = "hierarchy";
    hierarchy.title = "Hierarchy";
    hierarchy.category = "scene";
    hierarchy.area = engine::ui::QtDockArea::Left;
    hierarchy.visible = true;
    engine::ui::QtDockSpec inspector;
    inspector.objectName = "inspector";
    inspector.title = "Inspector";
    inspector.category = "scene";
    inspector.area = engine::ui::QtDockArea::Right;
    inspector.visible = true;
    inspector.tabified = true;
    doc.docks = { hierarchy, inspector };

    engine::ui::QtActionSpec save;
    save.id = "scene.save";
    save.text = "Save Scene";
    save.shortcut = "Ctrl+S";
    save.category = "Scene";
    save.action = "scene.save";
    engine::ui::QtActionSpec play;
    play.id = "play.toggle";
    play.text = "Play";
    play.category = "Play";
    play.action = "play";
    play.checkable = true;
    doc.actions = { save, play };

    engine::ui::QtMenuSpec fileMenu;
    fileMenu.id = "file";
    fileMenu.title = "File";
    fileMenu.actionIds = { "scene.save" };
    doc.menus = { fileMenu };

    engine::ui::QtToolbarSpec mainToolbar;
    mainToolbar.id = "main";
    mainToolbar.actionIds = { "scene.save", "play.toggle" };
    doc.toolbars = { mainToolbar };

    doc.status.state = "edit";
    doc.status.sceneName = "scene1";
    doc.status.entityCount = 3;
    doc.status.frameMillis = 16;
    return doc;
}

void test_set_doc_success_and_accessors() {
    auto docModel = engine::ui::create_qt_editor_doc();
    std::string err;
    CHECK(docModel->set_doc(sample_doc(), err), "set_doc accepted");
    const auto* dock = docModel->dock("hierarchy");
    CHECK(dock != nullptr && dock->title == "Hierarchy" &&
          dock->area == engine::ui::QtDockArea::Left && dock->visible,
          "dock accessor returns the spec");
    CHECK(docModel->dock("unknown") == nullptr, "unknown dock is nullptr");
    const auto* action = docModel->action("play.toggle");
    CHECK(action != nullptr && action->checkable,
          "action accessor returns the spec");
    CHECK(action->enabled, "play action enabled by default");
    CHECK(docModel->action("nope") == nullptr, "unknown action is nullptr");
}

void test_set_doc_all_or_nothing() {
    auto docModel = engine::ui::create_qt_editor_doc();
    std::string err;
    // Empty version.
    auto doc = sample_doc();
    doc.version.clear();
    expect_refused(docModel->set_doc(doc, err), "empty version");
    // No docks.
    doc = sample_doc();
    doc.docks.clear();
    expect_refused(docModel->set_doc(doc, err), "no docks");
    // No actions.
    doc = sample_doc();
    doc.actions.clear();
    expect_refused(docModel->set_doc(doc, err), "no actions");
    // Duplicate dock objectName.
    doc = sample_doc();
    doc.docks[1].objectName = "hierarchy";
    expect_refused(docModel->set_doc(doc, err), "duplicate dock");
    // Duplicate action id.
    doc = sample_doc();
    doc.actions[1].id = "scene.save";
    expect_refused(docModel->set_doc(doc, err), "duplicate action");
    // Menu referencing unknown action.
    doc = sample_doc();
    doc.menus[0].actionIds = { "ghost" };
    expect_refused(docModel->set_doc(doc, err), "menu unknown action");
    // Toolbar referencing unknown action.
    doc = sample_doc();
    doc.toolbars[0].actionIds = { "ghost" };
    expect_refused(docModel->set_doc(doc, err), "toolbar unknown action");
    // Document untouched after every refusal.
    CHECK(docModel->snapshot().docks.empty(), "docks empty after refusals");
    CHECK(docModel->snapshot().actions.empty(), "actions empty after refusals");
}

void test_mutations_all_or_nothing() {
    auto docModel = engine::ui::create_qt_editor_doc();
    std::string err;
    CHECK(docModel->set_doc(sample_doc(), err), "set_doc accepted");

    CHECK(docModel->set_dock_area("inspector", engine::ui::QtDockArea::Bottom),
          "move inspector to Bottom");
    CHECK(docModel->dock("inspector")->area == engine::ui::QtDockArea::Bottom,
          "inspector area is Bottom");
    expect_refused(docModel->set_dock_area("ghost", engine::ui::QtDockArea::Top),
                   "set_dock_area unknown dock");
    CHECK(docModel->dock("inspector")->area == engine::ui::QtDockArea::Bottom,
          "area untouched after refused move");

    CHECK(docModel->set_dock_visible("hierarchy", false), "hide hierarchy");
    CHECK(!docModel->dock("hierarchy")->visible, "hierarchy hidden");
    expect_refused(docModel->set_dock_visible("ghost", true),
                   "set_dock_visible unknown dock");

    CHECK(docModel->set_action_enabled("scene.save", false), "disable save");
    CHECK(!docModel->action("scene.save")->enabled, "save disabled");
    expect_refused(docModel->set_action_enabled("ghost", false),
                   "set_action_enabled unknown action");

    // play.toggle is checkable -> checked flips; scene.save is not.
    CHECK(docModel->set_action_checked("play.toggle", true), "check play");
    expect_refused(docModel->set_action_checked("scene.save", true),
                   "set_action_checked non-checkable action refused");
    expect_refused(docModel->set_action_checked("ghost", true),
                   "set_action_checked unknown action");
}

void test_status_and_snapshot_equality() {
    auto docModel = engine::ui::create_qt_editor_doc();
    std::string err;
    CHECK(docModel->set_doc(sample_doc(), err), "set_doc accepted");
    engine::ui::QtStatusSpec status;
    status.state = "play";
    status.sceneName = "scene2";
    status.entityCount = 42;
    status.frameMillis = 33;
    docModel->set_status(status);
    const auto snap = docModel->snapshot();
    CHECK(snap.status.state == "play" && snap.status.entityCount == 42 &&
          snap.status.frameMillis == 33, "status updated");
    CHECK(snap.version == "qt-editor-doc-1" && snap.docks.size() == 2 &&
          snap.actions.size() == 2 && snap.menus.size() == 1 &&
          snap.toolbars.size() == 1, "snapshot keeps full structure");
}

void test_json_deterministic_and_content() {
    auto docModel = engine::ui::create_qt_editor_doc();
    std::string err;
    CHECK(docModel->set_doc(sample_doc(), err), "set_doc accepted");
    const std::string json = docModel->to_json();
    CHECK(json == docModel->to_json(), "to_json deterministic");
    CHECK(json.find("\"version\":\"qt-editor-doc-1\"") != std::string::npos,
          "json has version");
    CHECK(json.find("\"objectName\":\"hierarchy\"") != std::string::npos,
          "json has hierarchy dock");
    CHECK(json.find("\"area\":\"Left\"") != std::string::npos, "json has Left area");
    CHECK(json.find("\"tabified\":true") != std::string::npos,
          "json has tabified inspector");
    CHECK(json.find("\"id\":\"scene.save\"") != std::string::npos,
          "json has save action");
    CHECK(json.find("\"shortcut\":\"Ctrl+S\"") != std::string::npos,
          "json has shortcut");
    CHECK(json.find("\"checked\":true") != std::string::npos ||
          json.find("\"checked\":false") != std::string::npos,
          "json has checked state");
    CHECK(json.find("\"menus\":[{\"id\":\"file\"") != std::string::npos,
          "json has file menu");
    CHECK(json.find("\"toolbars\":[{\"id\":\"main\"") != std::string::npos,
          "json has main toolbar");
    CHECK(json.find("\"status\":{\"state\":\"edit\",\"scene\":\"scene1\"") != std::string::npos,
          "json has status");
    CHECK(json.find("\"frameMillis\":16") != std::string::npos,
          "json has frameMillis (no floats)");
}

}  // namespace

int main() {
    test_set_doc_success_and_accessors();
    test_set_doc_all_or_nothing();
    test_mutations_all_or_nothing();
    test_status_and_snapshot_equality();
    test_json_deterministic_and_content();

    if (g_failures == 0) {
        std::printf("ALL PASSED — qt_editor_doc_tests\n");
        return 0;
    }
    std::printf("%d failure(s) — qt_editor_doc_tests\n", g_failures);
    return 1;
}
