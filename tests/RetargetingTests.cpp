// Gate para engine::animation::IRetargeting (agente 4 §4 item 2) — retargeting
// determinístico de animação fonte → alvo sobre um IAnimCore. Headless: sem
// editor, sem janela, sem relógio.

#include "engine/animation/IRetargeting.hpp"
#include "engine/animation/IAnimCore.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                            \
    do {                                                            \
        if (!(cond)) {                                              \
            std::printf("FAIL: %s\n", msg);                         \
            ++g_failures;                                           \
        }                                                           \
    } while (0)

void expect_refused(bool ok, const char* what) {
    if (ok) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

void check_honest_error(const std::string& errorOut, const char* what) {
    if (errorOut.empty()) {
        std::printf("FAIL: %s (erro honesto)\n", what);
        ++g_failures;
    }
}

engine::animation::SkeletonSpec make_skeleton_src() {
    using namespace engine::animation;
    SkeletonSpec s;
    s.id = "human_s";
    s.bones.push_back(Bone{ "Hip", -1, AnimTransform{{0.0, 0.0, 0.0}, AnimQuat{}, {1.0, 1.0, 1.0}} });
    s.bones.push_back(Bone{ "Head", 0, AnimTransform{{0.0, 2.2, 0.0}, AnimQuat{}, {1.0, 1.0, 1.0}} });
    s.bones.push_back(Bone{ "HandL", 0, AnimTransform{{0.5, 1.2, 0.0}, AnimQuat{}, {1.0, 1.0, 1.0}} });
    return s;
}

engine::animation::SkeletonSpec make_skeleton_dst() {
    using namespace engine::animation;
    // Mesmos ossos da fonte + HandR extra: clip da alvo cobre 4 ossos e é
    // rejeitado como "clip de outra skeleton".
    SkeletonSpec s;
    s.id = "human_t";
    s.bones.push_back(Bone{ "Hip", -1, AnimTransform{{0.0, 0.0, 0.0}, AnimQuat{}, {1.0, 1.0, 1.0}} });
    s.bones.push_back(Bone{ "Head", 0, AnimTransform{{0.0, 2.2, 0.0}, AnimQuat{}, {1.0, 1.0, 1.0}} });
    s.bones.push_back(Bone{ "HandL", 0, AnimTransform{{0.5, 1.2, 0.0}, AnimQuat{}, {1.0, 1.0, 1.0}} });
    s.bones.push_back(Bone{ "HandR", 0, AnimTransform{{-0.5, 1.2, 0.0}, AnimQuat{}, {1.0, 1.0, 1.0}} });
    return s;
}

engine::animation::ClipSpec make_clip(const std::string& id, const std::string& skeleton) {
    using namespace engine::animation;
    ClipSpec c;
    c.id = id;
    c.skeleton = skeleton;
    c.duration = 1.0;
    BoneTrack hip;
    hip.bone = "Hip";
    hip.keys.push_back(Keyframe{ 0.0, AnimTransform{{1.0, 0.0, 0.0}, AnimQuat{}, {1.0, 1.0, 1.0}} });
    hip.keys.push_back(Keyframe{ 1.0, AnimTransform{{2.0, 0.0, 0.0}, AnimQuat{}, {1.0, 1.0, 1.0}} });
    c.tracks.push_back(std::move(hip));
    return c;
}

void test_retargeting() {
    using namespace engine::animation;

    auto core = create_anim_core();
    std::string err;

    // Skeletons e clips de base.
    auto src = make_skeleton_src();
    auto dst = make_skeleton_dst();
    CHECK(core->add_skeleton(src, err), "skeleton human_s aceito");
    CHECK(core->add_skeleton(dst, err), "skeleton human_t aceito");
    auto walk_s = make_clip("walk_s", "human_s");
    auto walk_t = make_clip("walk_t", "human_t");
    CHECK(core->add_clip(walk_s, err), "clip walk_s aceito");
    CHECK(core->add_clip(walk_t, err), "clip walk_t aceito");

    auto rt = create_retargeting(*core);

    // add_retarget válido.
    std::vector<RetargetMapping> mappings;
    mappings.push_back(RetargetMapping{ "Hip", "Hip", 1.0 });
    mappings.push_back(RetargetMapping{ "HandL", "HandL", 1.0 });
    CHECK(rt->add_retarget("s2t", "human_s", "human_t", mappings, err), "retarget s2t");
    CHECK(err.empty(), "retarget s2t sem erro residual");
    CHECK(rt->has_retarget("s2t"), "has_retarget s2t");
    CHECK(rt->retarget_ids().size() == 1, "retarget_ids = 1");

    // id duplicado.
    expect_refused(rt->add_retarget("s2t", "human_s", "human_t", mappings, err),
                   "id duplicado rejeitado");
    check_honest_error(err, "id duplicado rejeitado");

    // skeleton desconhecida.
    std::vector<RetargetMapping> one{ RetargetMapping{ "Hip", "Hip", 1.0 } };
    expect_refused(rt->add_retarget("ghost_skel", "ghost", "human_t", one, err),
                   "skeleton desconhecida rejeitado");
    check_honest_error(err, "skeleton desconhecida rejeitado");

    // osso fonte desconhecido.
    std::vector<RetargetMapping> badSrc{ RetargetMapping{ "Ghost", "Hip", 1.0 } };
    expect_refused(rt->add_retarget("bad_src", "human_s", "human_t", badSrc, err),
                   "osso fonte desconhecido rejeitado");
    check_honest_error(err, "osso fonte desconhecido rejeitado");

    // osso alvo desconhecido.
    std::vector<RetargetMapping> badDst{ RetargetMapping{ "Hip", "Ghost", 1.0 } };
    expect_refused(rt->add_retarget("bad_dst", "human_s", "human_t", badDst, err),
                   "osso alvo desconhecido rejeitado");
    check_honest_error(err, "osso alvo desconhecido rejeitado");

    // osso alvo duplicado (dois fontes → mesmo alvo).
    std::vector<RetargetMapping> dupDst{
        RetargetMapping{ "Hip", "HandL", 1.0 },
        RetargetMapping{ "Head", "HandL", 1.0 }
    };
    expect_refused(rt->add_retarget("dup_dst", "human_s", "human_t", dupDst, err),
                   "osso alvo duplicado rejeitado");
    check_honest_error(err, "osso alvo duplicado rejeitado");

    // scale 0.
    std::vector<RetargetMapping> zeroScale{ RetargetMapping{ "Hip", "Hip", 0.0 } };
    expect_refused(rt->add_retarget("zero_scale", "human_s", "human_t", zeroScale, err),
                   "scale 0 rejeitado");
    check_honest_error(err, "scale 0 rejeitado");

    // retarget_pose: osso mapeado usa o local da fonte × scale (1 → inalterado);
    // osso sem mapeamento (Head) = bind (0,2.2,0).
    const auto pose = rt->retarget_pose("s2t", "walk_s", 0.5, err);
    CHECK(err.empty(), "retarget_pose erro honesto");
    CHECK(pose.size() == 4, "pose na ordem da skeleton alvo (4 ossos)");
    bool hipOk = false;
    bool headOk = false;
    for (const auto& p : pose) {
        if (p.bone == "Hip") {
            hipOk = p.local.position.x == 1.5 && p.local.position.y == 0.0 &&
                    p.local.position.z == 0.0;
        } else if (p.bone == "Head") {
            headOk = p.local.position.x == 0.0 && p.local.position.y == 2.2 &&
                     p.local.position.z == 0.0;
        }
    }
    CHECK(hipOk, "retarget scale 1");
    CHECK(headOk, "head sem mapeamento = bind (0,2.2,0)");

    // retarget desconhecido.
    const auto p1 = rt->retarget_pose("ghost_rt", "walk_s", 0.5, err);
    CHECK(p1.empty(), "retarget desconhecido rejeitado");
    check_honest_error(err, "retarget desconhecido rejeitado");

    // clip desconhecido.
    const auto p2 = rt->retarget_pose("s2t", "ghost_clip", 0.5, err);
    CHECK(p2.empty(), "clip desconhecido rejeitado");
    check_honest_error(err, "clip desconhecido rejeitado");

    // clip de outra skeleton (walk_t está na human_t, 4 ossos).
    const auto p3 = rt->retarget_pose("s2t", "walk_t", 0.5, err);
    CHECK(p3.empty(), "clip de outra skeleton rejeitado");
    check_honest_error(err, "clip de outra skeleton rejeitado");

    // serialize → deserialize round-trip.
    const std::string state = rt->serialize_state();
    auto rt2 = create_retargeting(*core);
    CHECK(rt2->deserialize_state(state, err), "lido aceito");
    CHECK(err.empty(), "lido aceito sem erro residual");
    CHECK(rt2->has_retarget("s2t"), "has_retarget s2t após restore");
    const auto pose2 = rt2->retarget_pose("s2t", "walk_s", 0.5, err);
    CHECK(!pose2.empty() && pose2.size() == 4, "pose pós-restore");

    // restore com skeleton desconhecida.
    const std::string badState =
        "{\"version\":1,\"retargets\":{\"x\":{\"source\":\"ghost\",\"target\":\"human_t\",\"mappings\":[]}}}";
    expect_refused(rt2->deserialize_state(badState, err),
                   "restore com skeleton desconhecida rejeitado");
    check_honest_error(err, "restore com skeleton desconhecida rejeitado");
    CHECK(rt2->has_retarget("s2t"), "estado intocado após restore rejeitado");
}

}  // namespace

int main() {
    test_retargeting();
    if (g_failures == 0) {
        std::printf("retargeting_tests: all checks passed\n");
        return 0;
    }
    std::printf("retargeting_tests: %d failure(s)\n", g_failures);
    return 1;
}
