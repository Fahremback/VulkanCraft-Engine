// Gate for engine::editor::IAnimationTimelineEditor (agente 2 §B l.33) — the
// deterministic animation-timeline editor document model. Headless: no
// editor, no window, no clock.

#include "engine/editor/IAnimationTimelineEditor.hpp"

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

void test_initial_state() {
    auto ed = engine::editor::create_animation_timeline_editor();
    const auto snap = ed->snapshot();
    CHECK(snap.duration == 1.0f && snap.playhead == 0.0f && !snap.loop &&
          snap.tracks.empty(), "fresh document is all-defaults");
    CHECK(ed->validate().empty(), "fresh document has no issues");
}

void test_reset_refuses_non_positive() {
    auto ed = engine::editor::create_animation_timeline_editor();
    std::string err;
    expect_refused(ed->reset(0.0f, false, err), "reset with zero duration");
    expect_refused(ed->reset(-1.0f, false, err), "reset with negative duration");
    const auto snap = ed->snapshot();
    CHECK(snap.duration == 1.0f, "duration untouched after refused reset");
}

void test_add_track_rules() {
    auto ed = engine::editor::create_animation_timeline_editor();
    std::string err;
    expect_refused(ed->add_track("", engine::editor::TimelineTrackKind::Animation, err),
                   "empty track name");
    CHECK(ed->add_track("walk", engine::editor::TimelineTrackKind::Animation, err),
          "add walk track");
    expect_refused(ed->add_track("walk", engine::editor::TimelineTrackKind::Audio, err),
                   "duplicate track name");
    CHECK(ed->snapshot().tracks.size() == 1, "exactly one track");
    CHECK(ed->snapshot().tracks[0].name == "walk", "track name preserved");
}

void test_keys_sorted_and_unique() {
    auto ed = engine::editor::create_animation_timeline_editor();
    std::string err;
    CHECK(ed->reset(10.0f, true, err), "reset 10s loop");
    CHECK(ed->add_track("t", engine::editor::TimelineTrackKind::Event, err), "add track");
    CHECK(ed->add_key("t", 3.0f, "a", err), "key at 3");
    CHECK(ed->add_key("t", 1.0f, "b", err), "key at 1");
    CHECK(ed->add_key("t", 2.0f, "c", err), "key at 2");
    expect_refused(ed->add_key("t", 2.0f, "dup", err), "duplicate key time");
    const auto keys = ed->snapshot().tracks[0].keys;
    CHECK(keys.size() == 3, "three keys");
    CHECK(keys[0].time == 1.0f && keys[1].time == 2.0f && keys[2].time == 3.0f,
          "keys sorted by time");
    CHECK(keys[0].value == "b" && keys[1].value == "c" && keys[2].value == "a",
          "values follow their keyframes");
}

void test_key_bounds_and_unknown_track() {
    auto ed = engine::editor::create_animation_timeline_editor();
    std::string err;
    CHECK(ed->reset(5.0f, false, err), "reset 5s");
    CHECK(ed->add_track("t", engine::editor::TimelineTrackKind::Property, err), "add track");
    expect_refused(ed->add_key("t", -1.0f, "x", err), "negative key time");
    expect_refused(ed->add_key("t", 6.0f, "x", err), "key beyond duration");
    expect_refused(ed->add_key("ghost", 1.0f, "x", err), "unknown track");
    expect_refused(ed->remove_key("t", 1.0f, err), "remove missing key");
    expect_refused(ed->remove_key("ghost", 1.0f, err), "remove from unknown track");
    expect_refused(ed->remove_track("ghost", err), "remove unknown track");
    expect_refused(ed->set_muted("ghost", true, err), "mute unknown track");
}

void test_seek_and_mute() {
    auto ed = engine::editor::create_animation_timeline_editor();
    std::string err;
    CHECK(ed->reset(10.0f, false, err), "reset 10s");
    CHECK(ed->add_track("t", engine::editor::TimelineTrackKind::Audio, err), "add track");
    ed->seek(5.0f);
    CHECK(ed->snapshot().playhead == 5.0f, "seek 5");
    ed->seek(999.0f);
    CHECK(ed->snapshot().playhead == 10.0f, "seek clamps to duration");
    ed->seek(-5.0f);
    CHECK(ed->snapshot().playhead == 0.0f, "seek clamps to zero");
    CHECK(ed->set_muted("t", true, err), "mute track");
    CHECK(ed->snapshot().tracks[0].muted, "track muted");
    CHECK(ed->remove_track("t", err), "remove track");
    CHECK(ed->snapshot().tracks.empty(), "track gone");
}

void test_validation() {
    auto ed = engine::editor::create_animation_timeline_editor();
    std::string err;
    CHECK(ed->reset(4.0f, false, err), "reset 4s");
    CHECK(ed->add_track("t", engine::editor::TimelineTrackKind::Camera, err), "add track");
    // Force an out-of-bounds key by shrinking duration after adding a key.
    CHECK(ed->add_key("t", 3.0f, "x", err), "key at 3");
    CHECK(ed->reset(2.0f, false, err), "reset 2s (drops the key with the doc)");
    CHECK(ed->add_track("t", engine::editor::TimelineTrackKind::Camera, err), "re-add track");
    CHECK(ed->add_key("t", 3.0f, "x", err) == false, "key at 3 refused on 2s doc");
    CHECK(ed->validate().empty(), "document stays valid");
}

void test_to_json() {
    auto ed = engine::editor::create_animation_timeline_editor();
    std::string err;
    CHECK(ed->reset(2.5f, true, err), "reset 2.5 loop");
    CHECK(ed->add_track("t", engine::editor::TimelineTrackKind::Animation, err), "add track");
    CHECK(ed->add_key("t", 0.5f, "v", err), "key");
    const std::string json = ed->to_json();
    CHECK(json.find("\"duration\":2.5") != std::string::npos, "duration in json");
    CHECK(json.find("\"loop\":true") != std::string::npos, "loop in json");
    CHECK(json.find("\"time\":0.5") != std::string::npos, "key time in json");
    CHECK(json.find("\"value\":\"v\"") != std::string::npos, "key value in json");
    // Deterministic: two serializations are identical.
    CHECK(ed->to_json() == json, "json is deterministic");
}

}  // namespace

int main() {
    test_initial_state();
    test_reset_refuses_non_positive();
    test_add_track_rules();
    test_keys_sorted_and_unique();
    test_key_bounds_and_unknown_track();
    test_seek_and_mute();
    test_validation();
    test_to_json();
    if (g_failures == 0) {
        std::printf("animation_timeline_editor_tests: ALL PASSED\n");
        return 0;
    }
    std::printf("animation_timeline_editor_tests: %d FAILURE(S)\n", g_failures);
    return 1;
}
