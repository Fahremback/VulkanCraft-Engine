// Gate for engine::editor::IOnboardingTour (agente 2 §C) — the deterministic
// onboarding/tutorial step machine. Headless: no editor, no window, no clock.

#include "engine/editor/IOnboardingTour.hpp"

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

engine::editor::TourStepDef step(const std::string& id, const std::string& target) {
    engine::editor::TourStepDef s;
    s.id = id;
    s.title = "T " + id;
    s.copy = "C " + id;
    s.target = target;
    return s;
}

void test_initial_state() {
    auto tour = engine::editor::create_onboarding_tour();
    CHECK(tour->state() == engine::editor::TourState::Idle, "starts Idle");
    const auto snap = tour->snapshot();
    CHECK(snap.tour.empty() && snap.cursor == 0 && snap.done == 0 &&
          snap.skipped == 0 && snap.total == 0 && snap.currentStep.empty(),
          "fresh snapshot is all-defaults");
}

void test_start_refuses_empty_and_while_running() {
    auto tour = engine::editor::create_onboarding_tour();
    std::vector<engine::editor::TourStepDef> steps{
        step("a", "panels/Inspector"), step("b", "command/publish"),
    };
    CHECK(!tour->start("t", {}), "start with zero steps refused");
    CHECK(tour->state() == engine::editor::TourState::Idle, "still Idle after refused start");

    CHECK(tour->start("t", steps), "start with 2 steps");
    CHECK(tour->state() == engine::editor::TourState::Running, "Running after start");
    CHECK(!tour->start("t2", steps), "start while Running refused");
    CHECK(tour->state() == engine::editor::TourState::Running, "still Running (same tour)");
    CHECK(tour->snapshot().tour == "t", "tour id unchanged by refused start");
}

void test_advance_to_done() {
    auto tour = engine::editor::create_onboarding_tour();
    std::vector<engine::editor::TourStepDef> steps{
        step("a", "panels/Inspector"), step("b", "command/publish"),
        step("c", "panels/Hierarchy"),
    };
    CHECK(tour->start("t", steps), "start");
    CHECK(tour->snapshot().cursor == 1 && tour->snapshot().total == 3,
          "cursor at 1 of 3");
    CHECK(tour->snapshot().currentStep == "a", "current step is a");

    CHECK(tour->next(), "next a->b");
    CHECK(tour->snapshot().cursor == 2 && tour->snapshot().done == 1,
          "cursor 2, done 1");
    CHECK(tour->snapshot().currentStep == "b", "current step is b");

    CHECK(tour->skip(), "skip b");
    CHECK(tour->snapshot().cursor == 3 && tour->snapshot().skipped == 1,
          "cursor 3, skipped 1");
    CHECK(tour->snapshot().currentStep == "c", "current step is c");

    CHECK(tour->next(), "next c -> finishes");
    CHECK(tour->state() == engine::editor::TourState::Done, "Done after last step");
    CHECK(tour->snapshot().done == 2 && tour->snapshot().skipped == 1,
          "done 2, skipped 1, total 3");

    // Commands on a finished tour are refused.
    expect_refused(tour->next(), "next after Done");
    expect_refused(tour->skip(), "skip after Done");
    expect_refused(tour->complete(), "complete after Done");
    expect_refused(tour->reset(), "reset after Done");
}

void test_complete_marks_remaining_skipped() {
    auto tour = engine::editor::create_onboarding_tour();
    std::vector<engine::editor::TourStepDef> steps{
        step("a", "p1"), step("b", "p2"), step("c", "p3"), step("d", "p4"),
    };
    CHECK(tour->start("t", steps), "start 4 steps");
    CHECK(tour->next(), "a done");
    CHECK(tour->next(), "b done");
    CHECK(tour->complete(), "complete at step 3");
    CHECK(tour->state() == engine::editor::TourState::Done, "Done after complete");
    const auto snap = tour->snapshot();
    CHECK(snap.done == 2 && snap.skipped == 2 && snap.total == 4,
          "complete marks remaining 2 as skipped");
}

void test_reset_keeps_counts_returns_to_first() {
    auto tour = engine::editor::create_onboarding_tour();
    std::vector<engine::editor::TourStepDef> steps{
        step("a", "p1"), step("b", "p2"),
    };
    CHECK(tour->start("t", steps), "start");
    CHECK(tour->next(), "a done");
    CHECK(tour->reset(), "reset");
    CHECK(tour->state() == engine::editor::TourState::Running, "still Running");
    CHECK(tour->snapshot().cursor == 1, "cursor back to 1");
    CHECK(tour->snapshot().done == 1, "done count kept after reset");
    CHECK(tour->snapshot().currentStep == "a", "current step is a again");
}

void test_dismiss_always_succeeds() {
    auto tour = engine::editor::create_onboarding_tour();
    std::vector<engine::editor::TourStepDef> steps{ step("a", "p1") };
    tour->dismiss();  // idempotent from Idle
    CHECK(tour->state() == engine::editor::TourState::Idle, "Idle after dismiss from Idle");

    CHECK(tour->start("t", steps), "start");
    tour->dismiss();
    CHECK(tour->state() == engine::editor::TourState::Idle, "Idle after dismiss from Running");
    const auto snap = tour->snapshot();
    CHECK(snap.tour.empty() && snap.total == 0 && snap.cursor == 0,
          "dismiss clears the tour");

    // Dismissed tour can start again.
    CHECK(tour->start("t2", steps), "restart after dismiss");
    CHECK(tour->state() == engine::editor::TourState::Running, "Running again");
}

void test_refusals_leave_state_untouched() {
    auto tour = engine::editor::create_onboarding_tour();
    // Commands in Idle (before any start) are refused.
    expect_refused(tour->next(), "next in Idle");
    expect_refused(tour->skip(), "skip in Idle");
    expect_refused(tour->complete(), "complete in Idle");
    expect_refused(tour->reset(), "reset in Idle");
    CHECK(tour->state() == engine::editor::TourState::Idle, "still Idle after refusals");
}

void test_json_deterministic() {
    auto tour = engine::editor::create_onboarding_tour();
    std::vector<engine::editor::TourStepDef> steps{
        step("a", "panels/Inspector"), step("b", "command/publish"),
    };
    CHECK(tour->start("t", steps), "start");
    tour->next();
    const std::string json = tour->to_json();
    const std::string again = tour->to_json();
    CHECK(json == again, "to_json deterministic");
    CHECK(json.find("\"state\":\"running\"") != std::string::npos, "json has running state");
    CHECK(json.find("\"tour\":\"t\"") != std::string::npos, "json has tour id");
    CHECK(json.find("\"cursor\":2") != std::string::npos, "json has cursor 2");
    CHECK(json.find("\"done\":1") != std::string::npos, "json has done 1");
    CHECK(json.find("\"total\":2") != std::string::npos, "json has total 2");
    CHECK(json.find("\"current\":\"b\"") != std::string::npos, "json has current step b");
}

}  // namespace

int main() {
    test_initial_state();
    test_start_refuses_empty_and_while_running();
    test_advance_to_done();
    test_complete_marks_remaining_skipped();
    test_reset_keeps_counts_returns_to_first();
    test_dismiss_always_succeeds();
    test_refusals_leave_state_untouched();
    test_json_deterministic();

    if (g_failures == 0) {
        std::printf("ALL PASSED — onboarding_tour_tests\n");
        return 0;
    }
    std::printf("%d failure(s) — onboarding_tour_tests\n", g_failures);
    return 1;
}
