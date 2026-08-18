// OfflineFarmTests (FALTANTES §18 item 11): proves the OFFLINE physics/
// learning farm seam. MuJoCo / MuJoCo MPC / DeepMimic generate and validate
// motion assets OUTSIDE the game (reference only — never compiled, the
// shape-ml / minecraft-spider pattern; DEPENDENCY_POLICY "treinamento, não
// dependência do jogo"); the runtime consumes COOKED + SIGNED assets through
// `IFarmCooker`. This gate proves the RuntimeCooker: (1) deterministic signing
// (FNV-1a 64 over canonical payload bytes — same payload, same signature,
// bit-exact, across instances), (2) verify() accepts the exact signed asset
// and rejects ANY tampered byte, (3) all-or-nothing validation (empty /
// mismatched / non-finite / non-increasing / duplicate payloads are REFUSED
// with a diagnostic, never clamped), (4) the seam: the training backends
// (MuJoCo, MuJoCo MPC, DeepMimic) are REFUSED with a diagnostic — a missing
// training backend must never look like a working runtime simulator.
#include "engine/farm/IOfflineFarm.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstdio>
#include <string>

using namespace Engine::Farm;

namespace {

int g_failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::printf("[farm] FAIL: %s\n", message);
        ++g_failures;
    }
}

ClipPayload make_clip(std::size_t joints = 3, std::size_t frames = 4,
                      float frameRate = 30.0f) {
    ClipPayload clip;
    clip.jointCount = joints;
    clip.frameCount = frames;
    clip.frameRate = frameRate;
    for (std::size_t f = 0; f < frames; ++f) {
        clip.rootPositions.push_back(
            glm::vec3(static_cast<float>(f) * 0.5f, 1.0f, 0.0f));
        clip.rootOrientations.push_back(glm::quat(
            1.0f, 0.0f, static_cast<float>(f) * 0.1f, 0.0f));
    }
    for (std::size_t j = 0; j < joints; ++j) {
        for (std::size_t f = 0; f < frames; ++f) {
            clip.jointRotations.push_back(glm::quat(
                1.0f, static_cast<float>(j) * 0.01f,
                static_cast<float>(f) * 0.02f, 0.0f));
        }
    }
    return clip;
}

TrajectoryPayload make_trajectory() {
    TrajectoryPayload t;
    t.times.push_back(glm::vec3(0.0f));
    t.rootVelocities.push_back(glm::vec3(1.0f, 0.0f, 0.0f));
    t.turnRates.push_back(0.0f);
    t.times.push_back(glm::vec3(0.5f));
    t.rootVelocities.push_back(glm::vec3(1.1f, 0.0f, 0.0f));
    t.turnRates.push_back(0.2f);
    t.times.push_back(glm::vec3(1.0f));
    t.rootVelocities.push_back(glm::vec3(1.2f, 0.0f, 0.0f));
    t.turnRates.push_back(-0.1f);
    return t;
}

ParameterPayload make_parameters() {
    ParameterPayload p;
    p.names = {"gaitCycleTime", "contactGain", "swingHeight"};
    p.values = {0.8f, 12.0f, 0.15f};
    return p;
}

void test_cook_and_verify() {
    std::string err;
    auto cooker = create_farm_cooker(FarmKind::RuntimeCooker, err);
    check(cooker != nullptr, "cook: RuntimeCooker created");
    check(cooker->kind() == FarmKind::RuntimeCooker, "cook: kind");

    // Motion clip: cook + verify round-trip.
    CookedFarmAsset clipAsset;
    check(cooker->cook(CookedAssetKind::MotionClip, make_clip(),
                       TrajectoryPayload{}, ParameterPayload{}, clipAsset, err),
          "cook: motion clip accepted");
    check(clipAsset.kind == CookedAssetKind::MotionClip, "cook: kind stored");
    check(clipAsset.signature != 0, "cook: signature non-zero");
    check(cooker->verify(clipAsset, err), "verify: exact asset passes");

    // Trajectory policy and parameter table.
    CookedFarmAsset trajAsset;
    check(cooker->cook(CookedAssetKind::TrajectoryPolicy, ClipPayload{},
                       make_trajectory(), ParameterPayload{}, trajAsset, err),
          "cook: trajectory accepted");
    check(cooker->verify(trajAsset, err), "verify: trajectory passes");
    CookedFarmAsset paramAsset;
    check(cooker->cook(CookedAssetKind::ParameterTable, ClipPayload{},
                       TrajectoryPayload{}, make_parameters(), paramAsset, err),
          "cook: parameter table accepted");
    check(cooker->verify(paramAsset, err), "verify: parameter table passes");

    // Cross-kind payloads are refused (exactly ONE payload family per kind).
    check(!cooker->cook(CookedAssetKind::TrajectoryPolicy, make_clip(),
                        make_trajectory(), ParameterPayload{}, trajAsset, err),
          "cook: clip payload refused for trajectory kind");
    check(!err.empty(), "cook: refusal carries a diagnostic");
    std::printf("[farm] cook + verify round-trips OK\n");
}

void test_signature_determinism() {
    std::string err;
    auto a = create_farm_cooker(FarmKind::RuntimeCooker, err);
    auto b = create_farm_cooker(FarmKind::RuntimeCooker, err);
    CookedFarmAsset assetA, assetB;
    check(a->cook(CookedAssetKind::MotionClip, make_clip(), TrajectoryPayload{},
                  ParameterPayload{}, assetA, err),
          "det: cook A");
    check(b->cook(CookedAssetKind::MotionClip, make_clip(), TrajectoryPayload{},
                  ParameterPayload{}, assetB, err),
          "det: cook B");
    check(assetA.signature == assetB.signature,
          "det: identical payloads -> identical signatures (cross-instance)");
    check(a->verify(assetA, err) && b->verify(assetA, err),
          "det: one instance verifies the other's asset");

    // A single changed float changes the signature (no hash collisions in the
    // signed domain).
    CookedFarmAsset tampered;
    ClipPayload clip = make_clip();
    clip.jointRotations[0].x += 1e-4f;
    check(a->cook(CookedAssetKind::MotionClip, clip, TrajectoryPayload{},
                  ParameterPayload{}, tampered, err),
          "det: tampered clip cooks (still valid structurally)");
    check(tampered.signature != assetA.signature,
          "det: any payload change changes the signature");
    std::printf("[farm] deterministic signing (cross-instance) OK\n");
}

void test_verify_rejects_tampering() {
    std::string err;
    auto cooker = create_farm_cooker(FarmKind::RuntimeCooker, err);
    CookedFarmAsset asset;
    check(cooker->cook(CookedAssetKind::MotionClip, make_clip(),
                       TrajectoryPayload{}, ParameterPayload{}, asset, err),
          "tamper: cook");
    const std::uint64_t originalSignature = asset.signature;

    // Corrupt a payload byte (the signature no longer matches).
    CookedFarmAsset corrupt = asset;
    corrupt.clip.rootPositions[0].y += 0.5f;
    check(!cooker->verify(corrupt, err), "tamper: moved root position rejected");
    check(!err.empty(), "tamper: diagnostic present");

    // Wrong kind header for the payload.
    CookedFarmAsset wrongKind = asset;
    wrongKind.kind = CookedAssetKind::ParameterTable;
    check(!cooker->verify(wrongKind, err),
          "tamper: kind swapped for clip payload rejected");

    // Verify returns the ORIGINAL asset to the exact signed state.
    check(cooker->verify(asset, err), "tamper: original still verifies");
    check(asset.signature == originalSignature, "tamper: original intact");
    std::printf("[farm] verify rejects tampering, original intact OK\n");
}

void test_validation_refusals() {
    std::string err;
    auto cooker = create_farm_cooker(FarmKind::RuntimeCooker, err);

    // Clip refusals.
    CookedFarmAsset out;
    check(!cooker->cook(CookedAssetKind::MotionClip, ClipPayload{},
                        TrajectoryPayload{}, ParameterPayload{}, out, err),
          "refusal: empty clip refused");
    ClipPayload noJoints = make_clip(0, 4, 30.0f);
    check(!cooker->cook(CookedAssetKind::MotionClip, noJoints,
                        TrajectoryPayload{}, ParameterPayload{}, out, err),
          "refusal: zero-joint clip refused");
    ClipPayload badRate = make_clip(3, 4, 0.0f);
    check(!cooker->cook(CookedAssetKind::MotionClip, badRate,
                        TrajectoryPayload{}, ParameterPayload{}, out, err),
          "refusal: non-positive frameRate refused");
    ClipPayload badRootSize = make_clip(3, 4, 30.0f);
    badRootSize.rootPositions.pop_back();
    check(!cooker->cook(CookedAssetKind::MotionClip, badRootSize,
                        TrajectoryPayload{}, ParameterPayload{}, out, err),
          "refusal: root array size mismatch refused");
    ClipPayload badJointSize = make_clip(3, 4, 30.0f);
    badJointSize.jointRotations.pop_back();
    check(!cooker->cook(CookedAssetKind::MotionClip, badJointSize,
                        TrajectoryPayload{}, ParameterPayload{}, out, err),
          "refusal: joint array size mismatch refused");
    ClipPayload nanRoot = make_clip(3, 4, 30.0f);
    nanRoot.rootPositions[1].y = std::nanf("");
    check(!cooker->cook(CookedAssetKind::MotionClip, nanRoot,
                        TrajectoryPayload{}, ParameterPayload{}, out, err),
          "refusal: non-finite root position refused");

    // Trajectory refusals.
    TrajectoryPayload flatTimes = make_trajectory();
    flatTimes.times[2] = flatTimes.times[1];  // not strictly increasing
    check(!cooker->cook(CookedAssetKind::TrajectoryPolicy, ClipPayload{},
                        flatTimes, ParameterPayload{}, out, err),
          "refusal: non-increasing trajectory times refused");
    TrajectoryPayload sized = make_trajectory();
    sized.rootVelocities.pop_back();
    check(!cooker->cook(CookedAssetKind::TrajectoryPolicy, ClipPayload{},
                        sized, ParameterPayload{}, out, err),
          "refusal: trajectory array size mismatch refused");

    // Parameter refusals.
    ParameterPayload dupNames = make_parameters();
    dupNames.names[1] = dupNames.names[0];
    check(!cooker->cook(CookedAssetKind::ParameterTable, ClipPayload{},
                        TrajectoryPayload{}, dupNames, out, err),
          "refusal: duplicate parameter name refused");
    ParameterPayload nanValue = make_parameters();
    nanValue.values[0] = std::nanf("");
    check(!cooker->cook(CookedAssetKind::ParameterTable, ClipPayload{},
                        TrajectoryPayload{}, nanValue, out, err),
          "refusal: non-finite parameter value refused");
    ParameterPayload emptyName = make_parameters();
    emptyName.names[0].clear();
    check(!cooker->cook(CookedAssetKind::ParameterTable, ClipPayload{},
                        TrajectoryPayload{}, emptyName, out, err),
          "refusal: empty parameter name refused");
    std::printf("[farm] all-or-nothing validation refusals OK\n");
}

void test_offline_seam() {
    std::string err;
    // The training backends belong to the OFFLINE farm: REFUSED with a
    // diagnostic, never a silent fallback (a missing training backend must
    // never look like a working runtime simulator).
    check(create_farm_cooker(FarmKind::Mujoco, err) == nullptr,
          "seam: MuJoCo refused in the runtime");
    check(!err.empty(), "seam: MuJoCo refusal carries a diagnostic");
    err.clear();
    check(create_farm_cooker(FarmKind::MujocoMpc, err) == nullptr,
          "seam: MuJoCo MPC refused");
    err.clear();
    check(create_farm_cooker(FarmKind::DeepMimic, err) == nullptr,
          "seam: DeepMimic refused");
    check(!err.empty(), "seam: DeepMimic refusal carries a diagnostic");
    std::printf("[farm] offline seam: training backends refused OK\n");
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    test_cook_and_verify();
    test_signature_determinism();
    test_verify_rejects_tampering();
    test_validation_refusals();
    test_offline_seam();
    if (g_failures == 0) {
        std::printf("[farm] ALL PASSED\n");
        return 0;
    }
    std::printf("[farm] %d FAILURE(S)\n", g_failures);
    return 1;
}
