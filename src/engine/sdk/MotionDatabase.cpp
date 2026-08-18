// MotionDatabase (FALTANTES §18 item 1): the ONLY TU that includes ozz/acl
// headers (adapter rule). Cooks canonical skeleton/clip data through the
// `ufbx/fastgltf -> ozz -> ACL -> motion database` pipeline: RawSkeleton/
// RawAnimation -> SkeletonBuilder/AnimationBuilder -> runtime Skeleton/
// Animation, and compresses clips with ACL (compressed_tracks). Sampling
// equivalence with the ozz path is proved by MotionDatabaseTests.
#include "engine/animation/IMotionDatabase.hpp"

// ozz
#include "ozz/animation/offline/raw_skeleton.h"
#include "ozz/animation/offline/raw_animation.h"
#include "ozz/animation/offline/skeleton_builder.h"
#include "ozz/animation/offline/animation_builder.h"
#include "ozz/animation/runtime/skeleton.h"
#include "ozz/animation/runtime/animation.h"
#include "ozz/animation/runtime/sampling_job.h"
#include "ozz/animation/runtime/blending_job.h"
#include "ozz/animation/runtime/ik_two_bone_job.h"
#include "ozz/animation/runtime/ik_aim_job.h"
#include "ozz/base/maths/simd_quaternion.h"
#include "ozz/base/maths/soa_transform.h"

// ACL
#include "acl/compression/compress.h"
#include "acl/compression/compression_settings.h"
#include "acl/compression/track.h"
#include "acl/compression/track_array.h"
#include "acl/core/ansi_allocator.h"
#include "acl/core/compressed_tracks.h"
#include "acl/core/error_result.h"
#include "acl/core/track_writer.h"
#include "acl/decompression/decompress.h"
#include "acl/decompression/decompression_settings.h"

// rtm (ACL math)
#include <rtm/qvvf.h>
#include <rtm/quatf.h>
#include <rtm/vector4f.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

namespace engine {
namespace animation {
namespace {

// The uniform sample rate (Hz) used to resample canonical clips for ACL
// compression. ACL stores uniform tracks; the resample is deterministic
// (same keyframe interpolation as the sampling gate), so the compressed
// clip reproduces the same motion.
constexpr float kAclSampleRate = 30.0f;

// Interpolates one canonical track at `time` using the same blend rules the
// gate uses to compare poses (linear translation/scale, shortest-path slerp
// rotation). Clamped at the track's ends.
glm::vec3 sampleTrackTranslation(const MotionTrack& track, float time) {
    const auto& keys = track.keyframes;
    if (keys.empty()) {
        return glm::vec3(0.0f);
    }
    if (time <= keys.front().time) {
        return keys.front().translation;
    }
    if (time >= keys.back().time) {
        return keys.back().translation;
    }
    for (std::size_t i = 1; i < keys.size(); ++i) {
        if (time <= keys[i].time) {
            const float span = keys[i].time - keys[i - 1].time;
            const float t = span > 0.0f ? (time - keys[i - 1].time) / span : 0.0f;
            return glm::mix(keys[i - 1].translation, keys[i].translation, t);
        }
    }
    return keys.back().translation;
}

glm::quat sampleTrackRotation(const MotionTrack& track, float time) {
    const auto& keys = track.keyframes;
    if (keys.empty()) {
        return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    }
    if (time <= keys.front().time) {
        return keys.front().rotation;
    }
    if (time >= keys.back().time) {
        return keys.back().rotation;
    }
    for (std::size_t i = 1; i < keys.size(); ++i) {
        if (time <= keys[i].time) {
            const float span = keys[i].time - keys[i - 1].time;
            const float t = span > 0.0f ? (time - keys[i - 1].time) / span : 0.0f;
            return glm::slerp(keys[i - 1].rotation, keys[i].rotation, t);
        }
    }
    return keys.back().rotation;
}

glm::vec3 sampleTrackScale(const MotionTrack& track, float time) {
    const auto& keys = track.keyframes;
    if (keys.empty()) {
        return glm::vec3(1.0f);
    }
    if (time <= keys.front().time) {
        return keys.front().scale;
    }
    if (time >= keys.back().time) {
        return keys.back().scale;
    }
    for (std::size_t i = 1; i < keys.size(); ++i) {
        if (time <= keys[i].time) {
            const float span = keys[i].time - keys[i - 1].time;
            const float t = span > 0.0f ? (time - keys[i - 1].time) / span : 0.0f;
            return glm::mix(keys[i - 1].scale, keys[i].scale, t);
        }
    }
    return keys.back().scale;
}

// Builds the RawSkeleton tree from the flat canonical bone list (children
// always come after their parents). Returns false if a parent reference is
// out of order.
bool buildRawSkeleton(const MotionSkeleton& skeleton,
                      ozz::animation::offline::RawSkeleton& out,
                      std::string& errorOut) {
    // Node pool: raw pointers into it stay stable (unique_ptr owns).
    struct Node {
        std::string name;
        ozz::math::Transform transform;
        std::vector<Node*> children;
    };
    std::vector<std::unique_ptr<Node>> pool;
    pool.reserve(skeleton.bones.size());
    for (const MotionBone& b : skeleton.bones) {
        pool.push_back(std::make_unique<Node>());
        pool.back()->name = b.name;
        pool.back()->transform.translation = ozz::math::Float3(
            b.localTranslation.x, b.localTranslation.y, b.localTranslation.z);
        pool.back()->transform.rotation = ozz::math::Quaternion(
            b.localRotation.x, b.localRotation.y, b.localRotation.z,
            b.localRotation.w);
        pool.back()->transform.scale =
            ozz::math::Float3(b.localScale.x, b.localScale.y, b.localScale.z);
    }
    for (std::size_t i = 0; i < skeleton.bones.size(); ++i) {
        const int parent = skeleton.bones[i].parent;
        if (parent < 0) {
            continue;
        }
        if (parent >= static_cast<int>(i)) {
            errorOut = "motion skeleton parent " + std::to_string(parent) +
                       " of bone '" + skeleton.bones[i].name +
                       "' is not before it (children must come after parents)";
            return false;
        }
        pool[static_cast<std::size_t>(parent)]->children.push_back(pool[i].get());
    }

    // Deep-copy the tree into the RawSkeleton.
    std::function<void(const Node*, ozz::animation::offline::RawSkeleton::Joint::Children&)> convert;
    convert = [&convert](const Node* node,
                         ozz::animation::offline::RawSkeleton::Joint::Children& children) {
        children.emplace_back();
        ozz::animation::offline::RawSkeleton::Joint& joint = children.back();
        joint.name = ozz::string(node->name.c_str(), node->name.size());
        joint.transform = node->transform;
        joint.children.reserve(node->children.size());
        for (const Node* child : node->children) {
            convert(child, joint.children);
        }
    };
    out.roots.reserve(pool.size());
    for (std::size_t i = 0; i < skeleton.bones.size(); ++i) {
        if (skeleton.bones[i].parent < 0) {
            convert(pool[i].get(), out.roots);
        }
    }
    return true;
}

// Packs an AoS MotionPose into ozz SoA buffers (num_soa = ceil(count/4)
// groups). The last group's unused lanes are padded with the identity
// transform so the BlendingJob always processes finite data.
void packPose(const MotionPose& pose, std::size_t count,
              std::vector<ozz::math::SoaTransform>& out) {
    const std::size_t numSoa = (count + 3) / 4;
    out.resize(numSoa);
    for (std::size_t g = 0; g < numSoa; ++g) {
        float tx[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float ty[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float tz[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float rx[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float ry[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float rz[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float rw[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        float sx[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        float sy[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        float sz[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        for (int lane = 0; lane < 4; ++lane) {
            const std::size_t b = g * 4 + static_cast<std::size_t>(lane);
            if (b >= count) break;
            tx[lane] = pose.translations[b].x;
            ty[lane] = pose.translations[b].y;
            tz[lane] = pose.translations[b].z;
            rx[lane] = pose.rotations[b].x;
            ry[lane] = pose.rotations[b].y;
            rz[lane] = pose.rotations[b].z;
            rw[lane] = pose.rotations[b].w;
            sx[lane] = pose.scales[b].x;
            sy[lane] = pose.scales[b].y;
            sz[lane] = pose.scales[b].z;
        }
        ozz::math::SoaTransform& t = out[g];
        t.translation = ozz::math::SoaFloat3::Load(
            ozz::math::simd_float4::Load(tx[0], tx[1], tx[2], tx[3]),
            ozz::math::simd_float4::Load(ty[0], ty[1], ty[2], ty[3]),
            ozz::math::simd_float4::Load(tz[0], tz[1], tz[2], tz[3]));
        t.rotation = ozz::math::SoaQuaternion::Load(
            ozz::math::simd_float4::Load(rx[0], rx[1], rx[2], rx[3]),
            ozz::math::simd_float4::Load(ry[0], ry[1], ry[2], ry[3]),
            ozz::math::simd_float4::Load(rz[0], rz[1], rz[2], rz[3]),
            ozz::math::simd_float4::Load(rw[0], rw[1], rw[2], rw[3]));
        t.scale = ozz::math::SoaFloat3::Load(
            ozz::math::simd_float4::Load(sx[0], sx[1], sx[2], sx[3]),
            ozz::math::simd_float4::Load(sy[0], sy[1], sy[2], sy[3]),
            ozz::math::simd_float4::Load(sz[0], sz[1], sz[2], sz[3]));
    }
}

// Unpacks ozz SoA transform buffers back into an AoS MotionPose (only the
// first `count` lanes — the padding lane of the last group is dropped).
void unpackPose(const std::vector<ozz::math::SoaTransform>& soa,
                std::size_t count, MotionPose& out) {
    out.translations.resize(count);
    out.rotations.resize(count);
    out.scales.resize(count);
    for (std::size_t b = 0; b < count; ++b) {
        const ozz::math::SoaTransform& st = soa[b / 4];
        const int lane = static_cast<int>(b % 4);
        float x = 0.0f, y = 0.0f, z = 0.0f;
        switch (lane) {
            case 0:
                x = ozz::math::GetX(st.translation.x);
                y = ozz::math::GetX(st.translation.y);
                z = ozz::math::GetX(st.translation.z);
                break;
            case 1:
                x = ozz::math::GetY(st.translation.x);
                y = ozz::math::GetY(st.translation.y);
                z = ozz::math::GetY(st.translation.z);
                break;
            case 2:
                x = ozz::math::GetZ(st.translation.x);
                y = ozz::math::GetZ(st.translation.y);
                z = ozz::math::GetZ(st.translation.z);
                break;
            default:
                x = ozz::math::GetW(st.translation.x);
                y = ozz::math::GetW(st.translation.y);
                z = ozz::math::GetW(st.translation.z);
                break;
        }
        out.translations[b] = glm::vec3(x, y, z);
        float rx = 0.0f, ry = 0.0f, rz = 0.0f, rw = 1.0f;
        switch (lane) {
            case 0:
                rx = ozz::math::GetX(st.rotation.x);
                ry = ozz::math::GetX(st.rotation.y);
                rz = ozz::math::GetX(st.rotation.z);
                rw = ozz::math::GetX(st.rotation.w);
                break;
            case 1:
                rx = ozz::math::GetY(st.rotation.x);
                ry = ozz::math::GetY(st.rotation.y);
                rz = ozz::math::GetY(st.rotation.z);
                rw = ozz::math::GetY(st.rotation.w);
                break;
            case 2:
                rx = ozz::math::GetZ(st.rotation.x);
                ry = ozz::math::GetZ(st.rotation.y);
                rz = ozz::math::GetZ(st.rotation.z);
                rw = ozz::math::GetZ(st.rotation.w);
                break;
            default:
                rx = ozz::math::GetW(st.rotation.x);
                ry = ozz::math::GetW(st.rotation.y);
                rz = ozz::math::GetW(st.rotation.z);
                rw = ozz::math::GetW(st.rotation.w);
                break;
        }
        out.rotations[b] = glm::quat(rw, rx, ry, rz);
        float sx = 1.0f, sy = 1.0f, sz = 1.0f;
        switch (lane) {
            case 0:
                sx = ozz::math::GetX(st.scale.x);
                sy = ozz::math::GetX(st.scale.y);
                sz = ozz::math::GetX(st.scale.z);
                break;
            case 1:
                sx = ozz::math::GetY(st.scale.x);
                sy = ozz::math::GetY(st.scale.y);
                sz = ozz::math::GetY(st.scale.z);
                break;
            case 2:
                sx = ozz::math::GetZ(st.scale.x);
                sy = ozz::math::GetZ(st.scale.y);
                sz = ozz::math::GetZ(st.scale.z);
                break;
            default:
                sx = ozz::math::GetW(st.scale.x);
                sy = ozz::math::GetW(st.scale.y);
                sz = ozz::math::GetW(st.scale.z);
                break;
        }
        out.scales[b] = glm::vec3(sx, sy, sz);
    }
}

// ── IK support (FALTANTES §18 item 3) ─────────────────────────────────────
// The ozz IK jobs work on MODEL-space joint matrices and output LOCAL-space
// correction quaternions. The canonical pose + the canonical parent list are
// enough (no BFS permutation needed — the correction targets canonical
// indices directly).

// Computes model-space matrices for every bone from a local-space pose
// (children after parents; `parents[i]` is the canonical parent index).
std::vector<glm::mat4> localToModel(const MotionPose& pose,
                                    const std::vector<int>& parents) {
    std::vector<glm::mat4> model(pose.translations.size(), glm::mat4(1.0f));
    for (std::size_t i = 0; i < pose.translations.size(); ++i) {
        const glm::mat4 local =
            glm::translate(glm::mat4(1.0f), pose.translations[i]) *
            glm::mat4_cast(pose.rotations[i]) *
            glm::scale(glm::mat4(1.0f), pose.scales[i]);
        const int p = parents[i];
        model[i] = p >= 0 ? model[static_cast<std::size_t>(p)] * local : local;
    }
    return model;
}

// glm::mat4 and ozz::math::Float4x4 are both 16 consecutive floats in
// column-major order — the layout is interchangeable.
ozz::math::Float4x4 toOzz(const glm::mat4& m) {
    ozz::math::Float4x4 r;
    std::memcpy(r.cols, &m, sizeof(r.cols));
    return r;
}

ozz::math::SimdFloat4 toOzzVec4(const glm::vec3& v) {
    return ozz::math::simd_float4::Load(v.x, v.y, v.z, 0.0f);
}

glm::quat toGlmQuat(const ozz::math::SimdQuaternion& q) {
    float xyzw[4];
    ozz::math::StorePtrU(q.xyzw, xyzw);
    return glm::quat(xyzw[3], xyzw[0], xyzw[1], xyzw[2]);
}

// Walks the parent chain of `node` upward; returns true when `ancestor` is
// found on the way (a node is its own ancestor for this purpose).
bool isDescendant(int ancestor, int node, const std::vector<int>& parents) {
    for (int n = node; n >= 0; n = parents[static_cast<std::size_t>(n)]) {
        if (n == ancestor) return true;
    }
    return false;
}

// ACL decompression writer that fills a MotionPose. The base track_writer
// methods are NOT virtual (duck-typed through the templated decompressor), so
// the overrides must match the exact signature AND the RTM_SIMD_CALL calling
// convention — no `override` keyword.
struct PoseWriter : acl::track_writer {
    MotionPose* pose{ nullptr };

    void RTM_SIMD_CALL write_rotation(std::uint32_t trackIndex,
                                      rtm::quatf_arg0 rotation) {
        pose->rotations[trackIndex] =
            glm::quat(rtm::quat_get_w(rotation), rtm::quat_get_x(rotation),
                      rtm::quat_get_y(rotation), rtm::quat_get_z(rotation));
    }
    void RTM_SIMD_CALL write_translation(std::uint32_t trackIndex,
                                         rtm::vector4f_arg0 translation) {
        pose->translations[trackIndex] =
            glm::vec3(rtm::vector_get_x(translation), rtm::vector_get_y(translation),
                      rtm::vector_get_z(translation));
    }
    void RTM_SIMD_CALL write_scale(std::uint32_t trackIndex,
                                   rtm::vector4f_arg0 scale) {
        pose->scales[trackIndex] =
            glm::vec3(rtm::vector_get_x(scale), rtm::vector_get_y(scale),
                      rtm::vector_get_z(scale));
    }
};

}  // namespace

bool MotionSkeleton::validate(std::string& errorOut) const {
    if (bones.empty()) {
        errorOut = "motion skeleton '" + name + "' has no bones";
        return false;
    }
    for (std::size_t i = 0; i < bones.size(); ++i) {
        const int parent = bones[i].parent;
        if (parent >= static_cast<int>(i)) {
            errorOut = "motion skeleton bone '" + bones[i].name +
                       "' parent must precede it (children after parents)";
            return false;
        }
    }
    return true;
}

bool MotionClip::validate(const MotionSkeleton& skeleton,
                          std::string& errorOut) const {
    if (duration <= 0.0f) {
        errorOut = "motion clip '" + name + "' duration must be > 0";
        return false;
    }
    for (const MotionTrack& track : tracks) {
        if (track.boneIndex < 0 ||
            track.boneIndex >= static_cast<int>(skeleton.bones.size())) {
            errorOut = "motion clip '" + name + "' track references bone " +
                       std::to_string(track.boneIndex) + " out of range";
            return false;
        }
        float prev = -1.0f;
        for (const MotionKeyframe& k : track.keyframes) {
            if (k.time < 0.0f || k.time > duration) {
                errorOut = "motion clip '" + name + "' keyframe time " +
                           std::to_string(k.time) + " out of [0, duration]";
                return false;
            }
            if (k.time <= prev) {
                errorOut = "motion clip '" + name + "' keyframe times not "
                           "strictly increasing";
                return false;
            }
            prev = k.time;
        }
    }
    return true;
}

namespace {

// One cooked clip slot. The public CookedMotion is stored by value inside the
// slot; `cooked()` returns a stable pointer (the slot is owned by a
// unique_ptr in a vector, never moved).
// Deleter for ACL compressed tracks (allocated by the compressor through the
// allocator; freed with the same allocator by size).
struct AclTracksDeleter {
    acl::ansi_allocator* allocator{ nullptr };
    void operator()(acl::compressed_tracks* tracks) const {
        if (tracks != nullptr && allocator != nullptr) {
            allocator->deallocate(tracks, tracks->get_size());
        }
    }
};

struct CookedSlot {
    CookedMotion meta;
    // ozz::unique_ptr carries ozz's own Deleter; must match exactly.
    ozz::unique_ptr<ozz::animation::Animation> ozzAnimation;
    std::unique_ptr<acl::compressed_tracks, AclTracksDeleter> aclTracks;
};

class MotionDatabaseImpl : public IMotionDatabase {
public:
    bool cook_skeleton(const MotionSkeleton& skeleton,
                       std::string& errorOut) override {
        std::string err;
        if (!skeleton.validate(err)) {
            errorOut = err;
            return false;
        }
        ozz::animation::offline::RawSkeleton raw;
        if (!buildRawSkeleton(skeleton, raw, err)) {
            errorOut = err;
            return false;
        }
        ozz::animation::offline::SkeletonBuilder builder;
        auto built = builder(raw);
        if (!built) {
            errorOut = "ozz SkeletonBuilder failed for '" + skeleton.name + "'";
            return false;
        }
        ozzSkeleton_ = std::move(built);
        canonicalSkeleton_ = skeleton;
        boneCount_ = skeleton.bones.size();
        parents_.clear();
        parents_.reserve(skeleton.bones.size());
        for (const MotionBone& b : skeleton.bones) {
            parents_.push_back(b.parent);
        }
        // The SkeletonBuilder reorders joints breadth-first; record the
        // canonical -> runtime joint index map so sample() can permute the
        // output back to canonical order.
        canonicalToRuntime_.clear();
        canonicalToRuntime_.reserve(skeleton.bones.size());
        const auto runtimeNames = ozzSkeleton_->joint_names();
        for (const MotionBone& b : skeleton.bones) {
            int runtimeIndex = -1;
            for (std::size_t r = 0; r < runtimeNames.size(); ++r) {
                if (b.name == runtimeNames[r]) {
                    runtimeIndex = static_cast<int>(r);
                    break;
                }
            }
            if (runtimeIndex < 0) {
                errorOut = "ozz skeleton lost bone '" + b.name + "'";
                return false;
            }
            canonicalToRuntime_.push_back(runtimeIndex);
        }
        return true;
    }

    bool cook_clip(const MotionClip& clip, std::string& errorOut) override {
        if (!ozzSkeleton_) {
            errorOut = "cook_clip requires a cooked skeleton first";
            return false;
        }
        std::string err;
        if (!clip.validate(canonicalSkeleton_, err)) {
            errorOut = err;
            return false;
        }
        // Build tracks in RUNTIME joint order: RawAnimation track R animates
        // runtime joint R (the SkeletonBuilder's breadth-first order), so the
        // canonical track of bone C goes into the track of its runtime index.
        std::vector<int> runtimeToCanonical(static_cast<std::size_t>(boneCount_),
                                            -1);
        for (std::size_t c = 0; c < canonicalToRuntime_.size(); ++c) {
            const int rt = canonicalToRuntime_[c];
            if (rt >= 0 && rt < boneCount_) {
                runtimeToCanonical[static_cast<std::size_t>(rt)] =
                    static_cast<int>(c);
            }
        }
        ozz::animation::offline::RawAnimation raw;
        raw.duration = clip.duration;
        raw.tracks.resize(static_cast<std::size_t>(boneCount_));
        for (std::size_t r = 0; r < static_cast<std::size_t>(boneCount_); ++r) {
            ozz::animation::offline::RawAnimation::JointTrack& joint =
                raw.tracks[r];
            // Find the canonical track for this runtime joint, if any.
            const MotionTrack* canonical = nullptr;
            for (const MotionTrack& t : clip.tracks) {
                if (t.boneIndex == runtimeToCanonical[r]) {
                    canonical = &t;
                    break;
                }
            }
            if (canonical == nullptr) {
                // No canonical track: the bone holds its rest transform. ozz
                // requires EVERY track to carry keys (an empty track corrupts
                // the sampler's key cache and can read out of bounds), so
                // emit a constant identity track spanning the clip.
                ozz::animation::offline::RawAnimation::TranslationKey tk0;
                tk0.time = 0.0f;
                tk0.value = ozz::math::Float3::zero();
                joint.translations.push_back(tk0);
                ozz::animation::offline::RawAnimation::RotationKey rk0;
                rk0.time = 0.0f;
                rk0.value = ozz::math::Quaternion::identity();
                joint.rotations.push_back(rk0);
                ozz::animation::offline::RawAnimation::ScaleKey sk0;
                sk0.time = 0.0f;
                sk0.value = ozz::math::Float3::one();
                joint.scales.push_back(sk0);
                ozz::animation::offline::RawAnimation::TranslationKey tk1;
                tk1.time = clip.duration;
                tk1.value = ozz::math::Float3::zero();
                joint.translations.push_back(tk1);
                ozz::animation::offline::RawAnimation::RotationKey rk1;
                rk1.time = clip.duration;
                rk1.value = ozz::math::Quaternion::identity();
                joint.rotations.push_back(rk1);
                ozz::animation::offline::RawAnimation::ScaleKey sk1;
                sk1.time = clip.duration;
                sk1.value = ozz::math::Float3::one();
                joint.scales.push_back(sk1);
                continue;
            }
            joint.translations.reserve(canonical->keyframes.size());
            joint.rotations.reserve(canonical->keyframes.size());
            joint.scales.reserve(canonical->keyframes.size());
            for (const MotionKeyframe& k : canonical->keyframes) {
                ozz::animation::offline::RawAnimation::TranslationKey tk;
                tk.time = k.time;
                tk.value = ozz::math::Float3(k.translation.x, k.translation.y,
                                             k.translation.z);
                joint.translations.push_back(tk);
                ozz::animation::offline::RawAnimation::RotationKey rk;
                rk.time = k.time;
                rk.value = ozz::math::Quaternion(
                    k.rotation.x, k.rotation.y, k.rotation.z, k.rotation.w);
                joint.rotations.push_back(rk);
                ozz::animation::offline::RawAnimation::ScaleKey sk;
                sk.time = k.time;
                sk.value =
                    ozz::math::Float3(k.scale.x, k.scale.y, k.scale.z);
                joint.scales.push_back(sk);
            }
        }
        if (!raw.Validate()) {
            errorOut = "ozz RawAnimation validation failed for '" + clip.name +
                       "'";
            return false;
        }
        ozz::animation::offline::AnimationBuilder builder;
        builder.iframe_interval = 0.0f;
        auto built = builder(raw);
        if (!built) {
            errorOut = "ozz AnimationBuilder failed for '" + clip.name + "'";
            return false;
        }
        auto slot = std::make_unique<CookedSlot>();
        slot->meta.kind = CookedMotion::Kind::OzzAnimation;
        slot->meta.clipName = clip.name;
        slot->meta.duration = clip.duration;
        slot->meta.bytes = built->size();
        slot->ozzAnimation = std::move(built);
        slot->meta.handle = static_cast<std::uint64_t>(slots_.size());
        slots_.push_back(std::move(slot));
        return true;
    }

    bool compress_clip(const MotionClip& clip, std::string& errorOut) override {
        if (!ozzSkeleton_) {
            errorOut = "compress_clip requires a cooked skeleton first";
            return false;
        }
        std::string err;
        if (!clip.validate(canonicalSkeleton_, err)) {
            errorOut = err;
            return false;
        }
        const std::size_t numBones = static_cast<std::size_t>(boneCount_);
        // Uniform resample rate: enough samples to cover the densest track.
        std::size_t maxKeys = 1;
        for (const MotionTrack& t : clip.tracks) {
            maxKeys = std::max(maxKeys, t.keyframes.size());
        }
        const float rate =
            std::max(kAclSampleRate,
                     std::ceil(static_cast<float>(maxKeys - 1) /
                               std::max(clip.duration, 0.0001f)));
        const std::uint32_t numSamples = static_cast<std::uint32_t>(
            std::floor(clip.duration * rate) + 1);

        acl::track_array_qvvf trackList(allocator_, static_cast<std::uint32_t>(numBones));
        for (std::size_t b = 0; b < numBones; ++b) {
            const MotionTrack* canonical = nullptr;
            for (const MotionTrack& t : clip.tracks) {
                if (t.boneIndex == static_cast<int>(b)) {
                    canonical = &t;
                    break;
                }
            }
            // ACL make_owner TAKES OWNERSHIP of the sample buffer (the track
            // frees it through the allocator on destruction), so the buffer
            // must be allocated with the ACL allocator — never a local
            // std::vector (dangling-pointer crash).
            rtm::qvvf* samples = static_cast<rtm::qvvf*>(allocator_.allocate(
                static_cast<std::size_t>(numSamples) * sizeof(rtm::qvvf),
                alignof(rtm::qvvf)));
            for (std::uint32_t s = 0; s < numSamples; ++s) {
                const float t = static_cast<float>(s) / rate;
                glm::vec3 translation(0.0f);
                glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
                glm::vec3 scale(1.0f);
                if (canonical != nullptr) {
                    translation = sampleTrackTranslation(*canonical, t);
                    rotation = sampleTrackRotation(*canonical, t);
                    scale = sampleTrackScale(*canonical, t);
                }
                samples[s] = rtm::qvv_set(
                    rtm::quat_set(rotation.x, rotation.y, rotation.z,
                                  rotation.w),
                    rtm::vector_set(translation.x, translation.y,
                                    translation.z),
                    rtm::vector_set(scale.x, scale.y, scale.z));
            }
            acl::track_desc_transformf desc;
            desc.output_index = static_cast<std::uint32_t>(b);
            desc.parent_index = parents_[b] >= 0
                                    ? static_cast<std::uint32_t>(parents_[b])
                                    : acl::k_invalid_track_index;
            desc.precision = 1e-4F;
            trackList[b] = acl::track_qvvf::make_owner(
                desc, allocator_, samples, numSamples, rate);
        }

        acl::compression_settings settings = acl::get_default_compression_settings();
        settings.level = acl::compression_level8::medium;
        // The dev-snapshot default leaves error_metric null; the qvvf metric
        // measures object-space error with qvvf arithmetic (scale-aware).
        acl::qvvf_transform_error_metric errorMetric;
        settings.error_metric = &errorMetric;
        acl::compressed_tracks* compressed = nullptr;
        acl::output_stats stats;
        acl::error_result result =
            acl::compress_track_list(allocator_, trackList, settings, compressed,
                                     stats);
        if (result.any()) {
            errorOut = std::string("ACL compression failed: ") + result.c_str();
            return false;
        }
        auto slot = std::make_unique<CookedSlot>();
        slot->meta.kind = CookedMotion::Kind::AclCompressed;
        slot->meta.clipName = clip.name;
        slot->meta.duration = clip.duration;
        slot->meta.bytes = compressed->get_size();
        slot->aclTracks.reset(compressed);
        slot->aclTracks.get_deleter().allocator = &allocator_;
        slot->meta.handle = static_cast<std::uint64_t>(slots_.size());
        slots_.push_back(std::move(slot));
        return true;
    }

    bool sample(const CookedMotion& motion, float time,
                MotionPose& outPose) const override {
        if (!ozzSkeleton_ || boneCount_ == 0) {
            return false;
        }
        if (motion.handle >= slots_.size()) {
            return false;
        }
        const CookedSlot& slot = *slots_[motion.handle];
        const float t = std::max(0.0f, std::min(time, slot.meta.duration));
        outPose.translations.resize(static_cast<std::size_t>(boneCount_));
        outPose.rotations.resize(static_cast<std::size_t>(boneCount_));
        outPose.scales.resize(static_cast<std::size_t>(boneCount_));
        if (slot.meta.kind == CookedMotion::Kind::OzzAnimation) {
            if (!slot.ozzAnimation) {
                return false;
            }
            if (canonicalToRuntime_.size() !=
                static_cast<std::size_t>(boneCount_)) {
                return false;
            }
            const int numSoa = (boneCount_ + 3) / 4;
            std::vector<ozz::math::SoaTransform> soa(static_cast<std::size_t>(numSoa));
            if (!samplingContext_ ||
                samplingContext_->max_tracks() < boneCount_) {
                samplingContext_ =
                    std::make_unique<ozz::animation::SamplingJob::Context>(
                        boneCount_);
            }
            // Random-access sampling: the context cache assumes sequential
            // playback and produces wrong poses on large ratio jumps (e.g.
            // 0 -> 1.0 with a fresh cache). Invalidate forces a full
            // re-initialization for the requested time — correctness first.
            samplingContext_->Invalidate();
            ozz::animation::SamplingJob job;
            job.animation = slot.ozzAnimation.get();
            job.context = samplingContext_.get();
            job.ratio = slot.meta.duration > 0.0f
                            ? t / slot.meta.duration
                            : 0.0f;
            job.output = ozz::span<ozz::math::SoaTransform>(soa.data(), soa.size());
            if (!job.Run()) {
                return false;
            }
            // Sampled output is indexed by RUNTIME joint order (BFS);
            // permute back to canonical bone order.
            std::vector<glm::vec3> rawT(static_cast<std::size_t>(boneCount_));
            std::vector<glm::quat> rawR(static_cast<std::size_t>(boneCount_));
            std::vector<glm::vec3> rawS(static_cast<std::size_t>(boneCount_));
            for (int b = 0; b < boneCount_; ++b) {
                const ozz::math::SoaTransform& st = soa[static_cast<std::size_t>(b / 4)];
                const int lane = b % 4;
                float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
                switch (lane) {
                    case 0:
                        x = ozz::math::GetX(st.translation.x);
                        y = ozz::math::GetX(st.translation.y);
                        z = ozz::math::GetX(st.translation.z);
                        break;
                    case 1:
                        x = ozz::math::GetY(st.translation.x);
                        y = ozz::math::GetY(st.translation.y);
                        z = ozz::math::GetY(st.translation.z);
                        break;
                    case 2:
                        x = ozz::math::GetZ(st.translation.x);
                        y = ozz::math::GetZ(st.translation.y);
                        z = ozz::math::GetZ(st.translation.z);
                        break;
                    default:
                        x = ozz::math::GetW(st.translation.x);
                        y = ozz::math::GetW(st.translation.y);
                        z = ozz::math::GetW(st.translation.z);
                        break;
                }
                rawT[static_cast<std::size_t>(b)] = glm::vec3(x, y, z);
                float rx = 0.0f, ry = 0.0f, rz = 0.0f, rw = 1.0f;
                switch (lane) {
                    case 0:
                        rx = ozz::math::GetX(st.rotation.x);
                        ry = ozz::math::GetX(st.rotation.y);
                        rz = ozz::math::GetX(st.rotation.z);
                        rw = ozz::math::GetX(st.rotation.w);
                        break;
                    case 1:
                        rx = ozz::math::GetY(st.rotation.x);
                        ry = ozz::math::GetY(st.rotation.y);
                        rz = ozz::math::GetY(st.rotation.z);
                        rw = ozz::math::GetY(st.rotation.w);
                        break;
                    case 2:
                        rx = ozz::math::GetZ(st.rotation.x);
                        ry = ozz::math::GetZ(st.rotation.y);
                        rz = ozz::math::GetZ(st.rotation.z);
                        rw = ozz::math::GetZ(st.rotation.w);
                        break;
                    default:
                        rx = ozz::math::GetW(st.rotation.x);
                        ry = ozz::math::GetW(st.rotation.y);
                        rz = ozz::math::GetW(st.rotation.z);
                        rw = ozz::math::GetW(st.rotation.w);
                        break;
                }
                rawR[static_cast<std::size_t>(b)] = glm::quat(rw, rx, ry, rz);
                float sx = 1.0f, sy = 1.0f, sz = 1.0f;
                switch (lane) {
                    case 0:
                        sx = ozz::math::GetX(st.scale.x);
                        sy = ozz::math::GetX(st.scale.y);
                        sz = ozz::math::GetX(st.scale.z);
                        break;
                    case 1:
                        sx = ozz::math::GetY(st.scale.x);
                        sy = ozz::math::GetY(st.scale.y);
                        sz = ozz::math::GetY(st.scale.z);
                        break;
                    case 2:
                        sx = ozz::math::GetZ(st.scale.x);
                        sy = ozz::math::GetZ(st.scale.y);
                        sz = ozz::math::GetZ(st.scale.z);
                        break;
                    default:
                        sx = ozz::math::GetW(st.scale.x);
                        sy = ozz::math::GetW(st.scale.y);
                        sz = ozz::math::GetW(st.scale.z);
                        break;
                }
                rawS[static_cast<std::size_t>(b)] = glm::vec3(sx, sy, sz);
            }
            for (int b = 0; b < boneCount_; ++b) {
                const int rt = canonicalToRuntime_[static_cast<std::size_t>(b)];
                outPose.translations[static_cast<std::size_t>(b)] =
                    rawT[static_cast<std::size_t>(rt)];
                outPose.rotations[static_cast<std::size_t>(b)] =
                    rawR[static_cast<std::size_t>(rt)];
                outPose.scales[static_cast<std::size_t>(b)] =
                    rawS[static_cast<std::size_t>(rt)];
            }
            return true;
        }
        // ACL-compressed.
        if (!slot.aclTracks) {
            return false;
        }
        acl::decompression_context<acl::default_transform_decompression_settings>
            context;
        if (!context.initialize(*slot.aclTracks)) {
            return false;
        }
        context.seek(t, acl::sample_rounding_policy::none);
        PoseWriter writer;
        writer.pose = &outPose;
        context.decompress_tracks(writer);
        return true;
    }

    bool blend_poses(const MotionPose& a, const MotionPose& b, float weight,
                     MotionPose& out) const override {
        const std::size_t count = a.translations.size();
        if (count == 0 || a.rotations.size() != count || a.scales.size() != count ||
            b.translations.size() != count || b.rotations.size() != count ||
            b.scales.size() != count) {
            return false;
        }
        const float w = std::max(0.0f, std::min(weight, 1.0f));
        const std::size_t numSoa = (count + 3) / 4;
        // The rest pose is only read when the accumulated layer weight for a
        // bone falls below the threshold; identity of the right size keeps
        // Validate() happy and the blending numerically stable.
        std::vector<ozz::math::SoaTransform> rest(
            numSoa, ozz::math::SoaTransform::identity());
        std::vector<ozz::math::SoaTransform> soaA;
        std::vector<ozz::math::SoaTransform> soaB;
        std::vector<ozz::math::SoaTransform> soaOut(numSoa);
        packPose(a, count, soaA);
        packPose(b, count, soaB);
        ozz::animation::BlendingJob::Layer layers[2];
        layers[0].weight = 1.0f - w;
        layers[0].transform =
            ozz::span<const ozz::math::SoaTransform>(soaA.data(), soaA.size());
        layers[1].weight = w;
        layers[1].transform =
            ozz::span<const ozz::math::SoaTransform>(soaB.data(), soaB.size());
        ozz::animation::BlendingJob job;
        job.layers =
            ozz::span<const ozz::animation::BlendingJob::Layer>(layers, 2);
        job.rest_pose =
            ozz::span<const ozz::math::SoaTransform>(rest.data(), rest.size());
        job.output =
            ozz::span<ozz::math::SoaTransform>(soaOut.data(), soaOut.size());
        if (!job.Run()) {
            return false;
        }
        unpackPose(soaOut, count, out);
        return true;
    }

    bool ik_two_bone(const MotionPose& pose, int root, int mid, int end,
                     const glm::vec3& target, const glm::vec3& poleVector,
                     float weight, MotionPose& out) const override {
        if (!ozzSkeleton_ || boneCount_ == 0) {
            return false;
        }
        const std::size_t count = pose.translations.size();
        if (count != static_cast<std::size_t>(boneCount_) ||
            pose.rotations.size() != count || pose.scales.size() != count) {
            return false;
        }
        if (root < 0 || root >= boneCount_ || mid < 0 || mid >= boneCount_ ||
            end < 0 || end >= boneCount_ || root == mid || mid == end) {
            return false;
        }
        if (weight < 0.0f || weight > 1.0f ||
            !isDescendant(root, mid, parents_) ||
            !isDescendant(mid, end, parents_)) {
            return false;
        }
        const std::vector<glm::mat4> model = localToModel(pose, parents_);
        const ozz::math::Float4x4 startM =
            toOzz(model[static_cast<std::size_t>(root)]);
        const ozz::math::Float4x4 midM =
            toOzz(model[static_cast<std::size_t>(mid)]);
        const ozz::math::Float4x4 endM =
            toOzz(model[static_cast<std::size_t>(end)]);
        ozz::animation::IKTwoBoneJob job;
        job.target = toOzzVec4(target);
        job.pole_vector = toOzzVec4(poleVector);
        job.weight = weight;
        // Soften 1 (the ozz default): da = chain_len, ds = 0, so the
        // exponential SoftenTarget never applies — the end joint reaches the
        // target exactly when reachable (the gate proves it with weight 1).
        job.soften = 1.0f;
        job.start_joint = &startM;
        job.mid_joint = &midM;
        job.end_joint = &endM;
        ozz::math::SimdQuaternion startCorrection;
        ozz::math::SimdQuaternion midCorrection;
        job.start_joint_correction = &startCorrection;
        job.mid_joint_correction = &midCorrection;
        if (!job.Run()) {
            return false;
        }
        out = pose;
        // Apply corrections to the LOCAL rotations of root and mid — the ozz
        // sample multiplies `local * correction`.
        out.rotations[static_cast<std::size_t>(root)] =
            out.rotations[static_cast<std::size_t>(root)] *
            toGlmQuat(startCorrection);
        out.rotations[static_cast<std::size_t>(mid)] =
            out.rotations[static_cast<std::size_t>(mid)] *
            toGlmQuat(midCorrection);
        return true;
    }

    bool ik_aim(const MotionPose& pose, int joint, const glm::vec3& target,
                const glm::vec3& forward, const glm::vec3& up,
                const glm::vec3& poleVector, float weight,
                MotionPose& out) const override {
        if (!ozzSkeleton_ || boneCount_ == 0) {
            return false;
        }
        const std::size_t count = pose.translations.size();
        if (count != static_cast<std::size_t>(boneCount_) ||
            pose.rotations.size() != count || pose.scales.size() != count) {
            return false;
        }
        if (joint < 0 || joint >= boneCount_ || weight < 0.0f || weight > 1.0f) {
            return false;
        }
        if (glm::length(forward) < 1e-6f || glm::length(up) < 1e-6f) {
            return false;
        }
        const std::vector<glm::mat4> model = localToModel(pose, parents_);
        const ozz::math::Float4x4 jointM =
            toOzz(model[static_cast<std::size_t>(joint)]);
        ozz::animation::IKAimJob job;
        job.target = toOzzVec4(target);
        job.forward = ozz::math::Normalize3(toOzzVec4(forward));
        job.up = ozz::math::Normalize3(toOzzVec4(up));
        job.pole_vector = toOzzVec4(poleVector);
        job.weight = weight;
        job.joint = &jointM;
        ozz::math::SimdQuaternion correction;
        job.joint_correction = &correction;
        if (!job.Run()) {
            return false;
        }
        out = pose;
        out.rotations[static_cast<std::size_t>(joint)] =
            out.rotations[static_cast<std::size_t>(joint)] *
            toGlmQuat(correction);
        return true;
    }

    // Returns the MOST RECENTLY cooked version of the clip (the
    // ACL-compressed one when compress_clip ran after cook_clip), so callers
    // can compare the two pipeline paths through the same name.
    const CookedMotion* cooked(const std::string& clipName) const override {
        for (auto it = slots_.rbegin(); it != slots_.rend(); ++it) {
            if ((*it)->meta.clipName == clipName) {
                return &(*it)->meta;
            }
        }
        return nullptr;
    }

    std::size_t bone_count() const noexcept override {
        return static_cast<std::size_t>(boneCount_);
    }

    void clear() noexcept override {
        ozzSkeleton_.reset();
        canonicalSkeleton_ = MotionSkeleton{};
        samplingContext_.reset();
        slots_.clear();
        parents_.clear();
        canonicalToRuntime_.clear();
        boneCount_ = 0;
    }

private:
    acl::ansi_allocator allocator_;
    // The canonical skeleton kept for clip validation (the ozz runtime
    // skeleton is a derived representation).
    MotionSkeleton canonicalSkeleton_;
    ozz::unique_ptr<ozz::animation::Skeleton> ozzSkeleton_;
    // Reusable sampling context (lazily grown); mutated by the const
    // `sample()` for the ozz path.
    mutable std::unique_ptr<ozz::animation::SamplingJob::Context> samplingContext_;
    std::vector<std::unique_ptr<CookedSlot>> slots_;
    std::vector<int> parents_;
    std::vector<int> canonicalToRuntime_;
    int boneCount_{ 0 };
};

}  // namespace

std::unique_ptr<IMotionDatabase> create_motion_database() {
    return std::make_unique<MotionDatabaseImpl>();
}

}  // namespace animation
}  // namespace engine
