// Gate for engine::editor::IWindowMode (agente 2 §B) — the unambiguous
// window-mode model. Headless: no window, no monitor, no clock.

#include "engine/editor/IWindowMode.hpp"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d — %s\n", __FILE__, __LINE__, msg); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

void expect_refused(bool ok, const std::string& err, const char* what) {
    if (ok) {
        std::printf("FAIL %s:%d — %s should have been refused\n", __FILE__, __LINE__, what);
        ++g_failures;
    } else if (err.empty()) {
        std::printf("FAIL %s:%d — %s refusal must carry a reason\n", __FILE__, __LINE__, what);
        ++g_failures;
    }
}

void test_initial_state() {
    auto wm = engine::editor::create_window_mode();
    CHECK(wm->mode() == engine::editor::WindowModeState::Windowed, "starts Windowed");
    const auto snap = wm->snapshot();
    CHECK(snap.width == 1280 && snap.height == 720, "default 1280x720");
    CHECK(snap.monitor == -1, "no monitor in Windowed");
    CHECK(!snap.geometrySaved, "no geometry saved initially");
}

void test_valid_transitions() {
    auto wm = engine::editor::create_window_mode();
    std::string err;

    CHECK(wm->set_windowed_geometry(100, 50, 1600, 900, err), "set windowed geometry");
    CHECK(wm->enter_fullscreen(0, err), "Windowed -> Fullscreen");
    CHECK(wm->mode() == engine::editor::WindowModeState::Fullscreen, "mode Fullscreen");
    CHECK(wm->snapshot().monitor == 0, "monitor tracked");
    CHECK(wm->snapshot().geometrySaved, "geometry saved on enter");

    CHECK(wm->exit(err), "Fullscreen -> Windowed");
    CHECK(wm->mode() == engine::editor::WindowModeState::Windowed, "mode Windowed");
    CHECK(wm->snapshot().width == 1600 && wm->snapshot().height == 900,
          "windowed geometry restored");
    CHECK(wm->snapshot().x == 100 && wm->snapshot().y == 50, "position restored");

    CHECK(wm->enter_borderless(1, err), "Windowed -> Borderless");
    CHECK(wm->mode() == engine::editor::WindowModeState::Borderless, "mode Borderless");
    CHECK(wm->snapshot().monitor == 1, "borderless monitor tracked");

    CHECK(wm->enter_fullscreen(2, err), "Borderless -> Fullscreen");
    CHECK(wm->mode() == engine::editor::WindowModeState::Fullscreen, "mode Fullscreen");
    CHECK(wm->snapshot().monitor == 2, "fullscreen monitor tracked");

    CHECK(wm->exit(err), "Fullscreen -> Windowed");
    CHECK(wm->snapshot().width == 1600, "geometry restored again");
}

void test_geometry_snapshot_once() {
    auto wm = engine::editor::create_window_mode();
    std::string err;

    CHECK(wm->set_windowed_geometry(10, 20, 1280, 720, err), "set geometry");
    CHECK(wm->enter_fullscreen(0, err), "fullscreen");
    // Resize while fullscreen changes the CURRENT geometry only; the saved
    // snapshot is immutable — exit restores the ORIGINAL windowed geometry.
    CHECK(wm->set_size(1920, 1080, err), "resize fullscreen");
    CHECK(wm->snapshot().width == 1920 && wm->snapshot().height == 1080,
          "current geometry updated while fullscreen");
    CHECK(wm->exit(err), "exit");
    CHECK(wm->snapshot().width == 1280 && wm->snapshot().height == 720,
          "exit restores the ORIGINAL windowed geometry");
    CHECK(wm->snapshot().x == 10 && wm->snapshot().y == 20, "position restored");

    // Snapshot is taken ONCE: re-entering after borderless keeps the ORIGINAL
    // windowed geometry (immutable snapshot).
    CHECK(wm->set_windowed_geometry(5, 5, 800, 600, err), "reset geometry");
    CHECK(wm->enter_fullscreen(0, err), "fullscreen again");
    CHECK(wm->set_size(2000, 1000, err), "grow fullscreen");
    CHECK(wm->exit(err), "exit");
    CHECK(wm->snapshot().width == 800 && wm->snapshot().height == 600,
          "snapshot taken once (original geometry kept)");
}

void test_invalid_transitions_refused() {
    auto wm = engine::editor::create_window_mode();
    std::string err;

    // exit() in Windowed is a no-op.
    CHECK(wm->exit(err), "exit in Windowed no-op");

    // enter_borderless in Fullscreen / enter_fullscreen in Fullscreen.
    CHECK(wm->enter_fullscreen(0, err), "fullscreen");
    expect_refused(wm->enter_fullscreen(1, err), err, "double fullscreen");
    expect_refused(wm->enter_borderless(1, err), err, "borderless from fullscreen");
    CHECK(wm->mode() == engine::editor::WindowModeState::Fullscreen, "still fullscreen");

    // set_windowed_geometry refused outside Windowed.
    expect_refused(wm->set_windowed_geometry(0, 0, 800, 600, err), err,
                   "set_windowed_geometry in Fullscreen");

    // monitor < 0 refused.
    CHECK(wm->exit(err), "back to windowed");
    expect_refused(wm->enter_fullscreen(-1, err), err, "negative monitor");
    expect_refused(wm->enter_borderless(-1, err), err, "negative monitor borderless");

    // invalid sizes refused without mutation.
    expect_refused(wm->set_windowed_geometry(0, 0, 0, 600, err), err, "zero width");
    expect_refused(wm->set_windowed_geometry(0, 0, 800, -5, err), err, "negative height");
    expect_refused(wm->set_size(0, 720, err), err, "zero size");
    CHECK(wm->snapshot().width == 1280 && wm->snapshot().height == 720,
          "refused sizes do not mutate");
}

void test_json() {
    auto wm = engine::editor::create_window_mode();
    std::string err;
    CHECK(wm->to_json() ==
              "{\"mode\":\"windowed\",\"monitor\":-1,\"x\":0,\"y\":0,"
              "\"width\":1280,\"height\":720,\"geometry_saved\":false}",
          "windowed JSON exact");

    CHECK(wm->enter_fullscreen(3, err), "fullscreen");
    CHECK(wm->to_json() ==
              "{\"mode\":\"fullscreen\",\"monitor\":3,\"x\":0,\"y\":0,"
              "\"width\":1280,\"height\":720,\"geometry_saved\":true}",
          "fullscreen JSON exact");
}

void test_determinism() {
    std::string errA, errB;
    auto a = engine::editor::create_window_mode();
    auto b = engine::editor::create_window_mode();

    a->set_windowed_geometry(10, 20, 1280, 720, errA);
    b->set_windowed_geometry(10, 20, 1280, 720, errB);
    a->enter_fullscreen(0, errA); b->enter_fullscreen(0, errB);
    a->set_size(1920, 1080, errA); b->set_size(1920, 1080, errB);
    a->exit(errA); b->exit(errB);
    a->enter_borderless(2, errA); b->enter_borderless(2, errB);

    CHECK(a->to_json() == b->to_json(), "JSON determinism cross-instance");
    CHECK(a->snapshot() == b->snapshot(), "snapshot equality cross-instance");
    CHECK(errA == errB, "error parity");
}

}  // namespace

int main() {
    test_initial_state();
    test_valid_transitions();
    test_geometry_snapshot_once();
    test_invalid_transitions_refused();
    test_json();
    test_determinism();

    if (g_failures == 0) {
        std::printf("ALL PASSED\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
