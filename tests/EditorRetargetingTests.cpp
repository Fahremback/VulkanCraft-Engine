// Gate for engine::editor::IRetargeting (agente 2 §B l.43) — the
// deterministic animation-retargeting editor document model. Headless: no
// editor, no window, no clock.

#include "engine/editor/IRetargeting.hpp"

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
    auto rt = engine::editor::create_retargeting();
    const auto snap = rt->snapshot();
    CHECK(snap.sourceSkeleton.empty() && snap.targetSkeleton.empty(),
          "fresh document has no skeletons");
    CHECK(snap.preserveRootMotion, "preserve root motion defaults true");
    CHECK(snap.mapping.empty(), "fresh document has no mapping");
    const auto issues = rt->validate();
    CHECK(issues.size() == 2, "fresh document reports missing skeletons + no mapping");
}

void test_set_skeletons_refuses_empty() {
    auto rt = engine::editor::create_retargeting();
    std::string err;
    expect_refused(rt->set_skeletons("", "t", err), "empty source skeleton");
    expect_refused(rt->set_skeletons("s", "", err), "empty target skeleton");
    CHECK(rt->snapshot().sourceSkeleton.empty(), "skeletons untouched after refusals");
    CHECK(rt->set_skeletons("skeleton_a", "skeleton_b", err), "set skeletons");
    const auto snap = rt->snapshot();
    CHECK(snap.sourceSkeleton == "skeleton_a" && snap.targetSkeleton == "skeleton_b",
          "skeletons preserved");
}

void test_map_rules() {
    auto rt = engine::editor::create_retargeting();
    std::string err;
    engine::editor::RetargetBoneMapDef m;
    m.sourceBone = "";
    m.targetBone = "Hand.L";
    expect_refused(rt->map(m, err), "empty source bone");
    m.sourceBone = "Hand.L";
    m.targetBone = "";
    expect_refused(rt->map(m, err), "empty target bone");
    m.targetBone = "Hand.L";
    m.translationScale = 0.0f;
    expect_refused(rt->map(m, err), "zero translation scale");
    m.translationScale = -1.0f;
    expect_refused(rt->map(m, err), "negative translation scale");
    m.translationScale = 1.0f;
    CHECK(rt->map(m, err), "map Hand.L -> Hand.L");
    CHECK(rt->snapshot().mapping.size() == 1, "one mapping");
    // Upsert by sourceBone replaces the previous entry.
    m.targetBone = "Hand.R";
    m.rotationOffsetY = 1.5f;
    CHECK(rt->map(m, err), "upsert Hand.L -> Hand.R");
    CHECK(rt->snapshot().mapping.size() == 1, "upsert keeps one entry");
    CHECK(rt->snapshot().mapping[0].targetBone == "Hand.R",
          "upsert replaced the target");
    CHECK(rt->snapshot().mapping[0].rotationOffsetY == 1.5f,
          "upsert replaced the offset");
}

void test_unmap_and_clear() {
    auto rt = engine::editor::create_retargeting();
    std::string err;
    engine::editor::RetargetBoneMapDef m;
    m.sourceBone = "A";
    m.targetBone = "B";
    CHECK(rt->map(m, err), "map A -> B");
    expect_refused(rt->unmap("ghost", err), "unmap unknown source bone");
    CHECK(rt->unmap("A", err), "unmap A");
    CHECK(rt->snapshot().mapping.empty(), "mapping empty after unmap");
    CHECK(rt->map(m, err), "re-map A -> B");
    rt->clear_mapping();
    CHECK(rt->snapshot().mapping.empty(), "mapping empty after clear");
}

void test_preserve_root_motion_flag() {
    auto rt = engine::editor::create_retargeting();
    rt->set_preserve_root_motion(false);
    CHECK(!rt->snapshot().preserveRootMotion, "flag flipped");
    rt->set_preserve_root_motion(true);
    CHECK(rt->snapshot().preserveRootMotion, "flag restored");
}

void test_validation_scenarios() {
    auto rt = engine::editor::create_retargeting();
    std::string err;
    engine::editor::RetargetBoneMapDef a;
    a.sourceBone = "Hand.L";
    a.targetBone = "Hand.L";
    engine::editor::RetargetBoneMapDef b;
    b.sourceBone = "Hand.R";
    b.targetBone = "Hand.R";
    engine::editor::RetargetBoneMapDef c;
    c.sourceBone = "Arm.L";
    c.targetBone = "Hand.L";  // collides with a's target
    rt->set_skeletons("s", "t", err);
    CHECK(rt->map(a, err), "map Hand.L self");
    auto issues = rt->validate();
    bool hasSelf = false;
    for (const auto& i : issues)
        if (i.find("self mapping") != std::string::npos) hasSelf = true;
    CHECK(hasSelf, "self mapping reported as info");
    CHECK(rt->map(b, err), "map Hand.R self");
    CHECK(rt->map(c, err), "map Arm.L -> Hand.L (duplicate target)");
    issues = rt->validate();
    bool hasDup = false;
    for (const auto& i : issues)
        if (i.find("duplicate target") != std::string::npos) hasDup = true;
    CHECK(hasDup, "duplicate target bone reported");
}

void test_to_json() {
    auto rt = engine::editor::create_retargeting();
    std::string err;
    rt->set_skeletons("src", "dst", err);
    engine::editor::RetargetBoneMapDef m;
    m.sourceBone = "Hip";
    m.targetBone = "Pelvis";
    m.translationScale = 1.25f;
    rt->map(m, err);
    rt->set_preserve_root_motion(false);
    const std::string json = rt->to_json();
    CHECK(json.find("\"sourceSkeleton\":\"src\"") != std::string::npos,
          "source skeleton in json");
    CHECK(json.find("\"targetSkeleton\":\"dst\"") != std::string::npos,
          "target skeleton in json");
    CHECK(json.find("\"preserveRootMotion\":false") != std::string::npos,
          "preserve flag in json");
    CHECK(json.find("\"sourceBone\":\"Hip\"") != std::string::npos,
          "source bone in json");
    CHECK(json.find("\"translationScale\":1.25") != std::string::npos,
          "scale in json");
    CHECK(json.find("\"rotationOffset\":[0,0,0]") != std::string::npos,
          "rotation offset in json");
    CHECK(rt->to_json() == json, "json is deterministic");
}

}  // namespace

int main() {
    test_initial_state();
    test_set_skeletons_refuses_empty();
    test_map_rules();
    test_unmap_and_clear();
    test_preserve_root_motion_flag();
    test_validation_scenarios();
    test_to_json();
    if (g_failures == 0) {
        std::printf("retargeting_tests: ALL PASSED\n");
        return 0;
    }
    std::printf("retargeting_tests: %d FAILURE(S)\n", g_failures);
    return 1;
}
