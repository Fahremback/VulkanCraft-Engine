// Gate for engine::editor::IProjectLauncher (agente 2 §C l.66) — the
// deterministic project-launcher session model (open project/scene flow).
// Headless: no editor, no window, no clock.

#include "engine/editor/IProjectLauncher.hpp"

#include <cstdio>
#include <string>

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

void test_initial_state() {
    auto l = engine::editor::create_project_launcher();
    const auto snap = l->snapshot();
    CHECK(snap.in_launcher_mode && snap.project.empty() && snap.scene.empty() &&
          !snap.dirty && snap.recents.empty(), "fresh session is hub mode, all defaults");
}

void test_recents_dedupe_and_order() {
    auto l = engine::editor::create_project_launcher();
    std::string err;
    expect_refused(l->add_recent("", err), "empty recent path");
    CHECK(l->add_recent("/p/a", err), "add recent a");
    CHECK(l->add_recent("/p/b", err), "add recent b");
    CHECK(l->snapshot().recents.size() == 2, "two recents");
    CHECK(l->snapshot().recents[0] == "/p/b", "most recent first");
    CHECK(l->add_recent("/p/a", err), "re-add recent a (move to front)");
    CHECK(l->snapshot().recents.size() == 2, "no duplicate");
    CHECK(l->snapshot().recents[0] == "/p/a", "a moved to front");
}

void test_open_project_rules() {
    auto l = engine::editor::create_project_launcher();
    std::string err;
    expect_refused(l->open_project("", err), "empty project path");
    expect_refused(l->open_project("/unknown", err), "unknown project refused");
    CHECK(l->snapshot().in_launcher_mode, "still hub after refused open");

    CHECK(l->add_recent("/p/known", err), "register known project");
    CHECK(l->open_project("/p/known", err), "open known project");
    const auto snap = l->snapshot();
    CHECK(!snap.in_launcher_mode, "left the hub");
    CHECK(snap.project == "/p/known", "project set");
    CHECK(snap.scene.empty() && !snap.dirty, "no scene, clean");
    CHECK(snap.recents[0] == "/p/known", "project added to recents");
}

void test_open_scene_rules() {
    auto l = engine::editor::create_project_launcher();
    std::string err;
    expect_refused(l->open_scene("/s.scene", err), "open scene while hub showing");

    CHECK(l->add_recent("/p", err), "register project");
    CHECK(l->open_project("/p", err), "open project");
    CHECK(l->open_scene("/p/assets/scenes/w.scene", err), "open scene");
    CHECK(l->snapshot().scene == "/p/assets/scenes/w.scene", "scene set");

    expect_refused(l->open_scene("/p/other.scene", err), "second scene refused");

    l->set_dirty(true);
    CHECK(l->snapshot().dirty, "dirty set");
    l->set_dirty(false);
    CHECK(!l->snapshot().dirty, "dirty cleared");

    CHECK(l->close_scene(err), "close scene");
    CHECK(l->snapshot().scene.empty() && !l->snapshot().dirty, "scene closed, clean");
    CHECK(!l->snapshot().in_launcher_mode, "still in editor mode after close_scene");
    expect_refused(l->close_scene(err), "close scene twice refused");
}

void test_back_to_launcher() {
    auto l = engine::editor::create_project_launcher();
    std::string err;
    CHECK(l->add_recent("/p", err), "register project");
    CHECK(l->open_project("/p", err), "open project");
    CHECK(l->open_scene("/s.scene", err), "open scene");
    l->set_dirty(true);
    l->back_to_launcher();
    const auto snap = l->snapshot();
    CHECK(snap.in_launcher_mode && snap.project.empty() && snap.scene.empty() &&
          !snap.dirty, "back to clean hub");
    CHECK(snap.recents.size() == 1, "recents preserved");
}

void test_to_json() {
    auto l = engine::editor::create_project_launcher();
    std::string err;
    CHECK(l->add_recent("/p", err), "register project");
    CHECK(l->open_project("/p", err), "open project");
    CHECK(l->open_scene("/s.scene", err), "open scene");
    l->set_dirty(true);
    const std::string json = l->to_json();
    CHECK(json.find("\"mode\":\"editor\"") != std::string::npos, "mode in json");
    CHECK(json.find("\"project\":\"/p\"") != std::string::npos, "project in json");
    CHECK(json.find("\"scene\":\"/s.scene\"") != std::string::npos, "scene in json");
    CHECK(json.find("\"dirty\":true") != std::string::npos, "dirty in json");
    CHECK(l->to_json() == json, "json is deterministic");
}

}  // namespace

int main() {
    test_initial_state();
    test_recents_dedupe_and_order();
    test_open_project_rules();
    test_open_scene_rules();
    test_back_to_launcher();
    test_to_json();
    if (g_failures == 0) {
        std::printf("project_launcher_tests: ALL PASSED\n");
        return 0;
    }
    std::printf("project_launcher_tests: %d FAILURE(S)\n", g_failures);
    return 1;
}
