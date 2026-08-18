#pragma once

// Public motion-database contract (FALTANTES §18 item 1): the animation
// pipeline `ufbx/fastgltf -> ozz -> ACL -> motion database` fixed behind this
// façade. The project feeds CANONICAL skeleton + clip data (engine-neutral
// keyframe format, the same shapes the legacy sampler consumes); the database
// cooks it through ozz (RawSkeleton/RawAnimation -> SkeletonBuilder/
// AnimationBuilder -> runtime Skeleton/Animation), compresses clips with ACL
// (compressed_tracks), and samples back through the ozz runtime. Sampling
// equivalence with the legacy path is proved by MotionDatabaseTests.
//
// Self-contained (glm only). The ONLY TU that includes ozz/acl headers is the
// SDK adapter (src/engine/sdk/MotionDatabase.cpp) — the adapter rule. This
// header never references external types.

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace animation {

// One bone of the canonical skeleton. `parent` is -1 for the root.
struct MotionBone {
    std::string name;
    int parent{ -1 };
    glm::vec3 localTranslation{ 0.0f };
    glm::quat localRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    glm::vec3 localScale{ 1.0f };
    // Inverse bind matrix (column-major, world-space rest). Identity when
    // unknown — skinning uses it to bring vertices into bone space.
    glm::mat4 inverseBindMatrix{ 1.0f };
};

// The canonical skeleton: ordered bones (children after parents), root at 0.
struct MotionSkeleton {
    std::string name;
    std::vector<MotionBone> bones;

    // All-or-nothing: refuses empty/misordered skeletons with a diagnostic.
    bool validate(std::string& errorOut) const;
};

// One keyframe of a canonical track.
struct MotionKeyframe {
    float time{ 0.0f };
    glm::vec3 translation{ 0.0f };
    glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    glm::vec3 scale{ 1.0f };
};

// One canonical track (a bone's animation over time).
struct MotionTrack {
    int boneIndex{ 0 };
    std::vector<MotionKeyframe> keyframes;
};

// The canonical clip: named, timed, per-bone keyframes.
struct MotionClip {
    std::string name;
    float duration{ 1.0f };
    std::vector<MotionTrack> tracks;

    // All-or-nothing: refuses empty/negative-duration clips, tracks with
    // non-monotonic or negative keyframe times, and out-of-range bone refs.
    bool validate(const MotionSkeleton& skeleton, std::string& errorOut) const;
};

// A sampled local pose: per-bone local transforms (bone order matches the
// skeleton).
struct MotionPose {
    std::vector<glm::vec3> translations;
    std::vector<glm::quat> rotations;
    std::vector<glm::vec3> scales;
};

// A cooked animation object (opaque). `kind` reports whether the clip is the
// raw ozz runtime animation or an ACL-compressed one.
struct CookedMotion {
    enum class Kind : std::uint8_t { OzzAnimation, AclCompressed };
    Kind kind{ Kind::OzzAnimation };
    // Opaque handle owned by the database (never dereferenced by callers).
    std::uint64_t handle{ 0 };
    std::string clipName;
    float duration{ 1.0f };
    // Size of the underlying cooked data (bytes) — the compression win is
    // observable through cooked_clip_bytes() vs source keyframe bytes.
    std::size_t bytes{ 0 };
};

// The motion database: cooks canonical skeletons/clips through the ozz->ACL
// pipeline and samples them back. Transport- and render-free — pure animation
// data.
class IMotionDatabase {
public:
    virtual ~IMotionDatabase() = default;

    // Cooks a canonical skeleton into the ozz runtime skeleton. All-or-
    // nothing (invalid skeleton refused with a diagnostic).
    virtual bool cook_skeleton(const MotionSkeleton& skeleton,
                               std::string& errorOut) = 0;

    // Cooks a canonical clip into an ozz runtime Animation. Requires a
    // cooked skeleton first. All-or-nothing.
    virtual bool cook_clip(const MotionClip& clip, std::string& errorOut) = 0;

    // Compresses a canonical clip with ACL (header-only library), producing
    // a decompressible compressed_tracks (uniform-resampled). Requires a
    // cooked skeleton. Returns false (with diagnostic) when compression is
    // not possible.
    virtual bool compress_clip(const MotionClip& clip, std::string& errorOut) = 0;

    // Samples a cooked (ozz or ACL-compressed) clip at `time` seconds into
    // `outPose`. Requires a cooked skeleton. Returns false on unknown
    // clip/kind or an empty pose buffer.
    virtual bool sample(const CookedMotion& motion, float time,
                        MotionPose& outPose) const = 0;

    // Blends two local-space poses (same bone count) with `weight` of `b`
    // into `a`, writing the result to `out`. Blending is performed by the
    // ozz BlendingJob (SoA): linear interpolation for translations/scales and
    // normalized-lerp (nlerp) for rotations, followed by normalization.
    // Refuses empty or size-mismatched poses (all-or-nothing). This is the
    // FALTANTES §18 item 2 path: the engine's own pose blending is replaced
    // by ozz blending behind the runtime facades.
    virtual bool blend_poses(const MotionPose& a, const MotionPose& b,
                             float weight, MotionPose& out) const = 0;

    // Two-bone IK (FALTANTES §18 item 3): solves the chain root -> mid -> end
    // (canonical bone indices; mid and end must be descendants of root/mid
    // respectively, not necessarily direct) toward `target` (model-space),
    // with the chain bending toward `poleVector` (model-space), via the ozz
    // IKTwoBoneJob. The correction quaternions are applied to the LOCAL
    // rotations of root and mid (order `local * correction`, the ozz sample
    // order). The job is analytic — with weight 1 the end joint's model-space
    // position reaches `target` (when reachable). All-or-nothing: refuses
    // without a cooked skeleton, an empty/size-mismatched pose, out-of-range
    // or non-ancestor chain indices, or weight outside [0, 1].
    virtual bool ik_two_bone(const MotionPose& pose, int root, int mid, int end,
                             const glm::vec3& target,
                             const glm::vec3& poleVector, float weight,
                             MotionPose& out) const = 0;

    // Aim IK (FALTANTES §18 item 3): rotates `joint` (canonical index) so its
    // local `forward` axis points at `target` (model-space), keeping the
    // local `up` axis oriented toward `poleVector` (model-space), via the ozz
    // IKAimJob. The correction is applied to the joint's LOCAL rotation
    // (order `local * correction`). All-or-nothing: refuses without a cooked
    // skeleton, an empty/size-mismatched pose, an out-of-range joint, weight
    // outside [0, 1], or a degenerate (zero-length) forward/up axis.
    virtual bool ik_aim(const MotionPose& pose, int joint,
                        const glm::vec3& target, const glm::vec3& forward,
                        const glm::vec3& up, const glm::vec3& poleVector,
                        float weight, MotionPose& out) const = 0;

    // Cooked object accessor. Returns the most recently cooked version of the
    // clip (ACL-compressed when compress_clip ran after cook_clip), or
    // nullptr when the clip is unknown.
    virtual const CookedMotion* cooked(const std::string& clipName) const = 0;
    // The number of bones of the cooked skeleton (0 = not cooked).
    virtual std::size_t bone_count() const noexcept = 0;

    // Removes every cooked object (skeleton + clips) — a clean slate for
    // restart-safety.
    virtual void clear() noexcept = 0;
};

// The factory: builds the ozz+ACL-backed motion database (the ONLY adapter
// TU that touches external headers).
std::unique_ptr<IMotionDatabase> create_motion_database();

}  // namespace animation
}  // namespace engine
