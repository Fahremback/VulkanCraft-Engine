#pragma once
#include "engine/animation/AnimationRuntime.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Engine {

// Skinned vertex with bone indices + weights (JOINTS_0 / WEIGHTS_0).
struct GltfSkinnedVertex {
    glm::vec3 position{0};
    glm::vec3 normal{0, 1, 0};
    glm::vec2 uv{0};
    std::array<uint8_t, 4> joints{0, 0, 0, 0};
    std::array<float, 4> weights{1, 0, 0, 0};
};

struct GltfSkin {
    std::string name;
    std::vector<uint32_t> jointNodes;         // node indices
    std::vector<glm::mat4> inverseBindMatrices; // per joint
    std::vector<GltfSkinnedVertex> vertices;  // skinned mesh vertices
    std::vector<uint32_t> indices;
};

// One glTF animation channel: a single sampler targeting one node (path =
// translation/rotation/scale), keyed by parallel time/TRS arrays.
struct GltfAnimationChannel {
    std::string nodeName;
    int nodeIndex{ -1 };
    std::vector<float> times;
    std::vector<glm::vec3> translations; // optional
    std::vector<glm::quat> rotations;    // optional
    std::vector<glm::vec3> scales;       // optional
};

struct GltfAnimationClip {
    std::string name;
    std::vector<GltfAnimationChannel> channels;
    float duration{0.0f};
};

// Minimal JSON parser for glTF 2.0 assets (nodes, skins, animations, meshes).
// Animation samplers read their input/output through accessors (standard
// glTF) or inline JSON arrays; buffers resolve from data URIs or the GLB BIN
// chunk passed to the constructor.
class GltfParser final {
public:
    explicit GltfParser(std::string json, std::vector<uint8_t> bin = {});
    [[nodiscard]] bool parse(std::string* error = nullptr);

    [[nodiscard]] const std::vector<GltfSkin>& skins() const noexcept { return skins_; }
    [[nodiscard]] const std::vector<GltfAnimationClip>& clips() const noexcept { return clips_; }
    [[nodiscard]] const std::vector<std::string>& node_names() const noexcept { return nodeNames_; }
    [[nodiscard]] const std::vector<std::vector<uint32_t>>& node_children() const noexcept { return nodeChildren_; }
    [[nodiscard]] bool has_skins() const noexcept { return !skins_.empty(); }

    // Builds a SkeletonAsset from the first skin (bones named after nodes).
    [[nodiscard]] SkeletonAsset make_skeleton() const;
    // Builds an AnimationClip from the first clip.
    [[nodiscard]] AnimationClip make_clip() const;

private:
    std::string json_;
    std::vector<uint8_t> bin_;
    std::vector<GltfSkin> skins_;
    std::vector<GltfAnimationClip> clips_;
    std::vector<std::string> nodeNames_;
    std::vector<std::vector<uint32_t>> nodeChildren_;
    bool parsed_{false};
};

// Parses a .gltf (JSON) or .glb (binary container) file and extracts skeleton,
// skin and animation data. Returns false on malformed input.
[[nodiscard]] bool load_gltf_assets(const std::filesystem::path& source,
                                    std::vector<GltfSkin>& skins,
                                    std::vector<GltfAnimationClip>& clips,
                                    std::string* error = nullptr);

// GPU skinning support: produces the per-frame bone matrix buffer (joint
// matrices) a skinned vertex shader consumes, plus the skinned vertex layout.
class GpuSkinningBuffer final {
public:
    // Computes bone matrices: inverseBind * worldPose for every joint.
    static std::vector<glm::mat4> compute_bone_matrices(
        const SkeletonAsset& skeleton, const Pose& pose);
    // Returns a packed float buffer (16 floats per bone) suitable for a UBO.
    static std::vector<float> pack(const std::vector<glm::mat4>& boneMatrices);
    // Generates the GLSL vertex shader for skinned meshes.
    static std::string skinned_vertex_shader(uint32_t boneCount);
};

} // namespace Engine
