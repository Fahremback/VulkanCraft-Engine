// AiGraphValidationTests (FALTANTES §18 item 13): proves the PUBLIC API for
// AI-generated/validated animation clips and graphs. The AI runs OFFLINE (the
// farm — no LLM or training environment is distributed with the game); the
// runtime side is `IAiGraphValidator` (RuntimeValidator): pure structural/
// semantic validation over the canonical formats + deterministic signing
// (FNV-1a 64 over canonical bytes). The gate proves: clip validation against a
// cooked skeleton, graph validation (unique states, existing initial state,
// transitions only between existing states, declared parameters, finite
// values), cook+verify round-trips, deterministic signatures (cross-instance),
// tamper rejection, all-or-nothing refusals, and the seam that REFUSES the
// LLM/training backends with a diagnostic.
#include "engine/animation/IAiGraphValidation.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace engine::animation;

namespace {

int g_failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::printf("[aigv] FAIL: %s\n", message);
        ++g_failures;
    }
}

MotionSkeleton make_skeleton(int bones = 3) {
    MotionSkeleton skeleton;
    skeleton.name = "ai-test-skeleton";
    for (int b = 0; b < bones; ++b) {
        MotionBone bone;
        bone.name = "bone" + std::to_string(b);
        bone.parent = b == 0 ? -1 : b - 1;
        skeleton.bones.push_back(bone);
    }
    return skeleton;
}

MotionClip make_clip(int bones = 3, std::size_t keyframes = 3) {
    MotionClip clip;
    clip.name = "ai-walk";
    clip.duration = 1.0f;
    for (int b = 0; b < bones; ++b) {
        MotionTrack track;
        track.boneIndex = b;
        for (std::size_t k = 0; k < keyframes; ++k) {
            MotionKeyframe key;
            key.time = static_cast<float>(k) / static_cast<float>(keyframes);
            key.translation = glm::vec3(static_cast<float>(k) * 0.1f, 0.0f,
                                        0.0f);
            key.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            key.scale = glm::vec3(1.0f);
            track.keyframes.push_back(key);
        }
        clip.tracks.push_back(track);
    }
    return clip;
}

AnimationGraphProposal make_graph() {
    AnimationGraphProposal graph;
    graph.initialState = "idle";
    graph.states.push_back({"idle", "idleClip"});
    graph.states.push_back({"walk", "walkClip"});
    graph.parameters.push_back({"speed", AiParameterKind::Float});
    AiGraphTransition t;
    t.from = "idle";
    t.to = "walk";
    t.parameter = "speed";
    t.comparison = AiGraphTransition::Comparison::Greater;
    t.threshold = 0.5f;
    t.blendDuration = 0.2f;
    graph.transitions.push_back(t);
    AiGraphTransition back;
    back.from = "walk";
    back.to = "idle";
    back.parameter = "speed";
    back.comparison = AiGraphTransition::Comparison::Less;
    back.threshold = 0.3f;
    back.blendDuration = 0.2f;
    graph.transitions.push_back(back);
    return graph;
}

void test_seam() {
    std::string err;
    auto validator = create_ai_validator(AiBackend::RuntimeValidator, err);
    check(validator != nullptr, "seam: RuntimeValidator created");
    check(validator->kind() == AiBackend::RuntimeValidator, "seam: kind");
    check(create_ai_validator(AiBackend::Llm, err) == nullptr,
          "seam: LLM refused in the runtime");
    check(!err.empty(), "seam: LLM refusal carries a diagnostic");
    err.clear();
    check(create_ai_validator(AiBackend::TrainingBackend, err) == nullptr,
          "seam: training backend refused");
    check(!err.empty(), "seam: training refusal carries a diagnostic");
    std::printf("[aigv] seam: LLM/training refused in the runtime OK\n");
}

void test_clip_validation() {
    std::string err;
    auto validator = create_ai_validator(AiBackend::RuntimeValidator, err);
    const MotionSkeleton skeleton = make_skeleton();
    check(validator->validate_clip(skeleton, make_clip(), err),
          "clip: valid clip against skeleton passes");
    // The same clip rules the motion database enforces.
    MotionClip outOfRange = make_clip();
    outOfRange.tracks[1].boneIndex = 99;  // unknown bone
    check(!validator->validate_clip(skeleton, outOfRange, err),
          "clip: out-of-range bone refused");
    MotionClip nonMonotonic = make_clip();
    nonMonotonic.tracks[0].keyframes[2].time = 0.1f;  // before keyframe 1
    check(!validator->validate_clip(skeleton, nonMonotonic, err),
          "clip: non-monotonic keyframes refused");
    MotionClip emptyClip;
    emptyClip.name = "empty";
    check(!validator->validate_clip(skeleton, emptyClip, err),
          "clip: empty clip refused");
    MotionSkeleton badSkeleton;
    check(!validator->validate_clip(badSkeleton, make_clip(), err),
          "clip: malformed skeleton refused (never assumed)");
    std::printf("[aigv] clip validation against the skeleton OK\n");
}

void test_graph_validation() {
    std::string err;
    auto validator = create_ai_validator(AiBackend::RuntimeValidator, err);
    check(validator->validate_graph(make_graph(), err),
          "graph: valid graph passes");
    check(make_graph().validate(err), "graph: proposal validate passes");

    AnimationGraphProposal noInitial = make_graph();
    noInitial.initialState = "nope";  // does not exist
    check(!validator->validate_graph(noInitial, err),
          "graph: unknown initial state refused");

    AnimationGraphProposal dupState = make_graph();
    dupState.states.push_back({"idle", "otherClip"});
    check(!validator->validate_graph(dupState, err),
          "graph: duplicate state name refused");

    AnimationGraphProposal badFrom = make_graph();
    badFrom.transitions[0].from = "flying";
    check(!validator->validate_graph(badFrom, err),
          "graph: transition from unknown state refused");

    AnimationGraphProposal badTo = make_graph();
    badTo.transitions[0].to = "flying";
    check(!validator->validate_graph(badTo, err),
          "graph: transition to unknown state refused");

    AnimationGraphProposal undeclaredParam = make_graph();
    undeclaredParam.transitions[0].parameter = "mana";
    check(!validator->validate_graph(undeclaredParam, err),
          "graph: transition referencing undeclared parameter refused");

    AnimationGraphProposal dupParam = make_graph();
    dupParam.parameters.push_back({"speed", AiParameterKind::Float});
    check(!validator->validate_graph(dupParam, err),
          "graph: duplicate parameter refused");

    AnimationGraphProposal negativeBlend = make_graph();
    negativeBlend.transitions[0].blendDuration = -0.5f;
    check(!validator->validate_graph(negativeBlend, err),
          "graph: negative blend duration refused");

    AnimationGraphProposal nanThreshold = make_graph();
    nanThreshold.transitions[0].threshold = std::nanf("");
    check(!validator->validate_graph(nanThreshold, err),
          "graph: NaN threshold refused");

    AnimationGraphProposal empty = make_graph();
    empty.states.clear();
    check(!validator->validate_graph(empty, err),
          "graph: empty graph refused");
    std::printf("[aigv] graph validation (structural rules) OK\n");
}

void test_cook_verify() {
    std::string err;
    auto validator = create_ai_validator(AiBackend::RuntimeValidator, err);
    const MotionSkeleton skeleton = make_skeleton();

    AiCookedAsset clipAsset;
    check(validator->cook(AiAssetKind::MotionClip, skeleton, make_clip(),
                          AnimationGraphProposal{}, clipAsset, err),
          "cook: motion clip accepted and signed");
    check(clipAsset.kind == AiAssetKind::MotionClip, "cook: kind stored");
    check(clipAsset.signature != 0, "cook: signature non-zero");
    check(validator->verify(clipAsset, err), "verify: exact clip passes");

    AiCookedAsset graphAsset;
    check(validator->cook(AiAssetKind::AnimationGraph, skeleton, MotionClip{},
                          make_graph(), graphAsset, err),
          "cook: animation graph accepted and signed");
    check(validator->verify(graphAsset, err), "verify: exact graph passes");

    // Deterministic signing across instances.
    auto other = create_ai_validator(AiBackend::RuntimeValidator, err);
    AiCookedAsset clipAsset2;
    check(other->cook(AiAssetKind::MotionClip, skeleton, make_clip(),
                      AnimationGraphProposal{}, clipAsset2, err),
          "cook: second instance cooks");
    check(clipAsset.signature == clipAsset2.signature,
          "cook: identical payloads -> identical signatures (cross-instance)");

    // A single changed keyframe changes the signature.
    AiCookedAsset tampered;
    MotionClip clip = make_clip();
    clip.tracks[0].keyframes[1].translation.x += 1e-4f;
    check(validator->cook(AiAssetKind::MotionClip, skeleton, clip,
                          AnimationGraphProposal{}, tampered, err),
          "cook: tampered clip still cooks (structurally valid)");
    check(tampered.signature != clipAsset.signature,
          "cook: any payload change changes the signature");
    std::printf("[aigv] cook + verify + deterministic signing OK\n");
}

void test_verify_rejects_tampering() {
    std::string err;
    auto validator = create_ai_validator(AiBackend::RuntimeValidator, err);
    const MotionSkeleton skeleton = make_skeleton();
    AiCookedAsset asset;
    check(validator->cook(AiAssetKind::AnimationGraph, skeleton, MotionClip{},
                          make_graph(), asset, err),
          "tamper: cook graph");

    AiCookedAsset corrupt = asset;
    corrupt.graph.states[0].clipName = "otherClip";  // signature no longer matches
    check(!validator->verify(corrupt, err), "tamper: modified state rejected");
    check(!err.empty(), "tamper: diagnostic present");

    AiCookedAsset wrongKind = asset;
    wrongKind.kind = AiAssetKind::MotionClip;
    check(!validator->verify(wrongKind, err),
          "tamper: kind swapped for graph payload rejected");

    check(validator->verify(asset, err), "tamper: original still verifies");
    std::printf("[aigv] verify rejects tampering, original intact OK\n");
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    test_seam();
    test_clip_validation();
    test_graph_validation();
    test_cook_verify();
    test_verify_rejects_tampering();
    if (g_failures == 0) {
        std::printf("[aigv] ALL PASSED\n");
        return 0;
    }
    std::printf("[aigv] %d FAILURE(S)\n", g_failures);
    return 1;
}
