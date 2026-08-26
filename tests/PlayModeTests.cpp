// Gate for engine::editor::IPlayMode (agente 2 §B) — the unambiguous
// play-state machine. Headless: no editor, no scene, no clock.

#include "engine/editor/IPlayMode.hpp"

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
    auto pm = engine::editor::create_play_mode();
    CHECK(pm->state() == engine::editor::PlayModeState::Edit, "starts in Edit");
    CHECK(!pm->is_runtime(), "Edit is not runtime");

    const auto snap = pm->snapshot();
    CHECK(!snap.runtime && !snap.paused && !snap.simulating && snap.steps == 0,
          "fresh snapshot is all-defaults");
}

void test_valid_transitions() {
    auto pm = engine::editor::create_play_mode();
    std::string err;

    // Edit -> Play -> Pause -> Play -> Pause -> Stop -> Edit
    CHECK(pm->play(err), "play from Edit");
    CHECK(pm->state() == engine::editor::PlayModeState::Play, "state is Play");
    CHECK(pm->is_runtime(), "Play is runtime");
    CHECK(pm->pause(err), "pause from Play");
    CHECK(pm->state() == engine::editor::PlayModeState::Pause, "state is Pause");
    CHECK(pm->snapshot().paused, "snapshot says paused");
    CHECK(pm->resume(err), "resume from Pause");
    CHECK(pm->state() == engine::editor::PlayModeState::Play, "resumed to Play");
    CHECK(pm->pause(err), "pause again");
    CHECK(pm->stop(err), "stop from Pause");
    CHECK(pm->state() == engine::editor::PlayModeState::Edit, "stopped to Edit");
    CHECK(!pm->is_runtime(), "not runtime after stop");

    // Edit -> Simulate -> Pause -> Play (pause-toggle) -> Stop
    CHECK(pm->simulate(err), "simulate from Edit");
    CHECK(pm->state() == engine::editor::PlayModeState::Simulate, "state is Simulate");
    CHECK(pm->snapshot().simulating, "snapshot says simulating");
    CHECK(pm->pause(err), "pause from Simulate");
    CHECK(pm->snapshot().paused, "paused while simulating");
    CHECK(pm->pause(err), "pause-toggle while paused resumes");
    CHECK(pm->state() == engine::editor::PlayModeState::Play, "pause-toggle resumed to Play");
    CHECK(pm->stop(err), "stop from Play");
    CHECK(pm->state() == engine::editor::PlayModeState::Edit, "back to Edit");
}

void test_step_frame() {
    auto pm = engine::editor::create_play_mode();
    std::string err;

    // step() in Edit must be refused
    expect_refused(pm->step(err), err, "step in Edit");

    CHECK(pm->play(err), "play");
    expect_refused(pm->step(err), err, "step in Play");

    CHECK(pm->pause(err), "pause");
    CHECK(pm->step(err), "step in Pause");
    CHECK(pm->step(err), "step again in Pause");
    CHECK(pm->snapshot().steps == 2, "two steps consumed");
    CHECK(pm->state() == engine::editor::PlayModeState::Pause, "still Pause after steps");

    CHECK(pm->stop(err), "stop");
    CHECK(pm->snapshot().steps == 0, "stop resets step counter");
}

void test_invalid_transitions_refused_without_mutation() {
    auto pm = engine::editor::create_play_mode();
    std::string err;

    // stop() in Edit is a valid no-op (idempotent).
    CHECK(pm->stop(err), "stop in Edit is a no-op");

    // pause/resume/simulate/step from Edit
    expect_refused(pm->pause(err), err, "pause in Edit");
    expect_refused(pm->resume(err), err, "resume in Edit");
    CHECK(pm->state() == engine::editor::PlayModeState::Edit, "state untouched after refusals");

    // from Play: simulate/resume/play refused
    CHECK(pm->play(err), "play");
    expect_refused(pm->simulate(err), err, "simulate in Play");
    expect_refused(pm->resume(err), err, "resume in Play");
    expect_refused(pm->play(err), err, "double play");
    CHECK(pm->state() == engine::editor::PlayModeState::Play, "still Play after refusals");

    // from Pause: play/simulate refused
    CHECK(pm->pause(err), "pause");
    expect_refused(pm->play(err), err, "play in Pause");
    expect_refused(pm->simulate(err), err, "simulate in Pause");
    CHECK(pm->state() == engine::editor::PlayModeState::Pause, "still Pause after refusals");
}

void test_json_snapshot() {
    auto pm = engine::editor::create_play_mode();
    std::string err;

    CHECK(pm->to_json() == "{\"state\":\"edit\",\"runtime\":false,\"paused\":false,\"simulating\":false,\"steps\":0}",
          "edit JSON exact");

    CHECK(pm->play(err), "play");
    CHECK(pm->to_json() == "{\"state\":\"play\",\"runtime\":true,\"paused\":false,\"simulating\":false,\"steps\":0}",
          "play JSON exact");

    CHECK(pm->pause(err), "pause");
    CHECK(pm->to_json() == "{\"state\":\"pause\",\"runtime\":true,\"paused\":true,\"simulating\":false,\"steps\":0}",
          "pause JSON exact");

    CHECK(pm->stop(err), "stop");
    CHECK(pm->simulate(err), "simulate");
    CHECK(pm->to_json() == "{\"state\":\"simulate\",\"runtime\":true,\"paused\":false,\"simulating\":true,\"steps\":0}",
          "simulate JSON exact");
}

void test_determinism() {
    std::string errA, errB;
    auto a = engine::editor::create_play_mode();
    auto b = engine::editor::create_play_mode();

    const char* cmds[] = {"play", "pause", "resume", "pause", "stop",
                          "simulate", "pause", "pause", "stop", "play", "pause",
                          "step", "step", "step"};
    for (const char* c : cmds) {
        std::string e1, e2;
        if (std::string(c) == "play") { a->play(e1); b->play(e2); }
        else if (std::string(c) == "simulate") { a->simulate(e1); b->simulate(e2); }
        else if (std::string(c) == "pause") { a->pause(e1); b->pause(e2); }
        else if (std::string(c) == "resume") { a->resume(e1); b->resume(e2); }
        else if (std::string(c) == "stop") { a->stop(e1); b->stop(e2); }
        else if (std::string(c) == "step") { a->step(e1); b->step(e2); }
        CHECK(e1 == e2, "error parity across instances");
    }
    CHECK(a->to_json() == b->to_json(), "bit-exact determinism cross-instance");
    CHECK(a->snapshot() == b->snapshot(), "snapshot equality cross-instance");
}

}  // namespace

int main() {
    test_initial_state();
    test_valid_transitions();
    test_step_frame();
    test_invalid_transitions_refused_without_mutation();
    test_json_snapshot();
    test_determinism();

    if (g_failures == 0) {
        std::printf("ALL PASSED\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
