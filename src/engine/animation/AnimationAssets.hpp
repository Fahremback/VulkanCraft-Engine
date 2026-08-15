#pragma once
#include "AnimationRuntime.hpp"
#include <filesystem>
#include <span>

namespace Engine {
struct SkinnedVertex {
    glm::vec3 position{0};
    glm::vec3 normal{0,1,0};
    glm::vec2 uv{0};
    glm::uvec4 joints{0};
    glm::vec4 weights{1,0,0,0};
};

class AnimationAssetIO final {
public:
    static bool save_skeleton(const SkeletonAsset& skeleton, const std::filesystem::path& path);
    static bool load_skeleton(SkeletonAsset& skeleton, const std::filesystem::path& path);
    static bool save_clip(const AnimationClip& clip, const std::filesystem::path& path);
    static bool load_clip(AnimationClip& clip, const std::filesystem::path& path);
};

class SkinnedMeshRuntime final {
public:
    void set_skeleton(const SkeletonAsset* skeleton) noexcept { skeleton_ = skeleton; }
    void set_vertices(std::vector<SkinnedVertex> vertices) { source_ = std::move(vertices); skinned_ = source_; }
    void update(const Pose& pose);
    const std::vector<SkinnedVertex>& vertices() const noexcept { return skinned_; }
    const std::vector<glm::mat4>& skin_matrices() const noexcept { return skinMatrices_; }
private:
    const SkeletonAsset* skeleton_{};
    std::vector<SkinnedVertex> source_;
    std::vector<SkinnedVertex> skinned_;
    std::vector<glm::mat4> skinMatrices_;
};
} // namespace Engine
