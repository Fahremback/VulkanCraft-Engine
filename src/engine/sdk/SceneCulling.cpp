// SceneCulling.cpp — Agente 1 (task_plan B.8), the deterministic scene
// culling core behind the public ISceneCulling contract.
//
// Self-contained (std + glm): Gribb-Hartmann frustum extraction, AABB/sphere
// visibility, distance LOD with hysteresis, conservative screen-space
// occlusion and instance grouping. Bit-exact for the same inputs on every
// machine. No clock. The GPU pipeline integration (draw calls, meshlet
// streams) is provider-side, as for the other pure cores.

#include "engine/rendering/ISceneCulling.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace Engine::Rendering {

namespace {

glm::vec4 normalizePlane(glm::vec4 p) {
    const float len = glm::length(glm::vec3(p.x, p.y, p.z));
    if (len <= 1e-9f) {
        return p;
    }
    return p / len;
}

// Clip-space box projection for the occlusion test: NDC rect + clip depth
// (z/w, monotonic along the view for points in front of the near plane).
struct ProjectedBox {
    float minX, minY, maxX, maxY;
    float minDepth, maxDepth;
};

class SceneCulling final : public ISceneCulling {
public:
    SceneCulling() : config_(SceneCullingConfig{}) {}

    bool configure(const SceneCullingConfig& config,
                   std::string& errorOut) override {
        if (!config.valid(errorOut)) {
            return false;
        }
        config_ = config;
        return true;
    }

    const SceneCullingConfig& config() const noexcept override { return config_; }

    bool configure_json(const std::string& jsonText,
                        std::string& errorOut) override {
        SceneCullingConfig parsed;
        if (!parseJson(jsonText, parsed, errorOut)) {
            return false;
        }
        return configure(parsed, errorOut);
    }

    std::string config_to_json() const override {
        std::ostringstream o;
        o << "{ \"version\": 1, \"lod0Distance\": " << config_.lod0Distance
          << ", \"lodHysteresis\": " << config_.lodHysteresis
          << ", \"maxInstances\": " << config_.maxInstances << " }";
        return o.str();
    }

    Frustum extractFrustum(const glm::mat4& viewProj) const noexcept override {
        Frustum f;
        // glm is column-major: mat[col][row]. Gribb-Hartmann needs the ROWS
        // of the combined matrix (clip = M * v), so rebuild them explicitly.
        const glm::vec4 r0(viewProj[0][0], viewProj[1][0], viewProj[2][0],
                           viewProj[3][0]);
        const glm::vec4 r1(viewProj[0][1], viewProj[1][1], viewProj[2][1],
                           viewProj[3][1]);
        const glm::vec4 r2(viewProj[0][2], viewProj[1][2], viewProj[2][2],
                           viewProj[3][2]);
        const glm::vec4 r3(viewProj[0][3], viewProj[1][3], viewProj[2][3],
                           viewProj[3][3]);
        f.planes[0] = normalizePlane(r3 + r0);   // left
        f.planes[1] = normalizePlane(r3 - r0);   // right
        f.planes[2] = normalizePlane(r3 - r1);   // top
        f.planes[3] = normalizePlane(r3 + r1);   // bottom
        f.planes[4] = normalizePlane(r3 + r2);   // near
        f.planes[5] = normalizePlane(r3 - r2);   // far
        return f;
    }

    bool sphereVisible(const Frustum& frustum, const glm::vec3& center,
                       float radius) const noexcept override {
        for (int i = 0; i < 6; ++i) {
            const glm::vec4& p = frustum.planes[i];
            const float d = p.x * center.x + p.y * center.y + p.z * center.z + p.w;
            if (d < -radius) {
                return false;
            }
        }
        return true;
    }

    bool aabbVisible(const Frustum& frustum, const glm::vec3& min,
                     const glm::vec3& max) const noexcept override {
        for (int i = 0; i < 6; ++i) {
            const glm::vec4& p = frustum.planes[i];
            // Positive vertex: the corner farthest along the plane normal.
            const glm::vec3 pv(
                (p.x >= 0.0f) ? max.x : min.x,
                (p.y >= 0.0f) ? max.y : min.y,
                (p.z >= 0.0f) ? max.z : min.z);
            const float d = p.x * pv.x + p.y * pv.y + p.z * pv.z + p.w;
            if (d < 0.0f) {
                return false;
            }
        }
        return true;
    }

    std::uint32_t selectLod(float distance) const noexcept override {
        const float d = std::max(0.0f, distance);
        float threshold = config_.lod0Distance;
        std::uint32_t lod = 0;
        while (d >= threshold && lod < 3) {
            ++lod;
            threshold *= 2.0f;
        }
        return lod;
    }

    std::uint32_t selectLodHysteretic(
        float distance, std::uint32_t currentLod) const noexcept override {
        const float d = std::max(0.0f, distance);
        const float h = config_.lodHysteresis;
        const std::uint32_t target = selectLod(d);
        if (target == currentLod) {
            return currentLod;
        }
        // The boundary between the two levels sits at the LOWER level's
        // threshold; the hysteresis band widens it on both sides.
        const float boundary = config_.lod0Distance *
                               std::pow(2.0f, static_cast<float>(
                                                  std::min(target, currentLod)));
        if (target > currentLod) {
            // Moving out: switch coarser only past the LEAVE threshold.
            return (d >= boundary * (1.0f + h)) ? currentLod + 1 : currentLod;
        }
        // Moving in: switch finer only past the ENTER threshold.
        return (d < boundary * (1.0f - h)) ? currentLod - 1 : currentLod;
    }

    bool occluded(const glm::mat4& viewProj, const glm::vec3& occMin,
                  const glm::vec3& occMax, const glm::vec3& candMin,
                  const glm::vec3& candMax) const noexcept override {
        // Extract the view matrix from viewProj is not possible directly; we
        // approximate depth ordering with the projected clip depth (z/w) which
        // is monotonic along the view direction for points in front of the
        // near plane.
        const ProjectedBox occ = projectClipBox(viewProj, occMin, occMax);
        const ProjectedBox cand = projectClipBox(viewProj, candMin, candMax);
        if (occ.maxX == -1e30f || cand.maxX == -1e30f) {
            return false;  // a box behind the camera is never an occluder
        }
        // Candidate rect fully inside the occluder rect (with epsilon).
        const float eps = 1e-4f;
        if (cand.minX < occ.minX - eps || cand.maxX > occ.maxX + eps ||
            cand.minY < occ.minY - eps || cand.maxY > occ.maxY + eps) {
            return false;
        }
        // Candidate's NEAREST depth beyond the occluder's FARTHEST depth.
        return cand.minDepth > occ.maxDepth + eps;
    }

    bool buildInstanceGroups(const std::vector<SceneInstance>& instances,
                             std::vector<InstanceGroup>& groups,
                             std::string& errorOut) const override {
        if (instances.size() > config_.maxInstances) {
            errorOut = "SceneCulling: instance stream exceeds maxInstances";
            return false;
        }
        // Deterministic grouping: sorted by (mesh, material) then position.
        std::vector<SceneInstance> sorted = instances;
        std::sort(sorted.begin(), sorted.end(),
                  [](const SceneInstance& a, const SceneInstance& b) {
                      if (a.mesh != b.mesh) return a.mesh < b.mesh;
                      if (a.material != b.material) return a.material < b.material;
                      if (a.position.x != b.position.x)
                          return a.position.x < b.position.x;
                      if (a.position.y != b.position.y)
                          return a.position.y < b.position.y;
                      if (a.position.z != b.position.z)
                          return a.position.z < b.position.z;
                      if (a.scale.x != b.scale.x) return a.scale.x < b.scale.x;
                      if (a.scale.y != b.scale.y) return a.scale.y < b.scale.y;
                      return a.scale.z < b.scale.z;
                  });

        groups.clear();
        for (const SceneInstance& inst : sorted) {
            if (groups.empty() || groups.back().mesh != inst.mesh ||
                groups.back().material != inst.material) {
                InstanceGroup g;
                g.mesh = inst.mesh;
                g.material = inst.material;
                g.count = 1;
                g.aabbMin = inst.position - inst.scale;
                g.aabbMax = inst.position + inst.scale;
                groups.push_back(g);
            } else {
                InstanceGroup& g = groups.back();
                ++g.count;
                g.aabbMin = glm::min(g.aabbMin, inst.position - inst.scale);
                g.aabbMax = glm::max(g.aabbMax, inst.position + inst.scale);
            }
        }
        return true;
    }

private:
    // Clip-space box projection for the occlusion test (no view matrix).
    static ProjectedBox projectClipBox(const glm::mat4& viewProj,
                                       const glm::vec3& min,
                                       const glm::vec3& max) {
        ProjectedBox out;
        out.minX = 1e30f;
        out.minY = 1e30f;
        out.maxX = -1e30f;
        out.maxY = -1e30f;
        out.minDepth = 1e30f;
        out.maxDepth = -1e30f;
        for (int ci = 0; ci < 8; ++ci) {
            const glm::vec3 corner(
                (ci & 1) ? max.x : min.x,
                (ci & 2) ? max.y : min.y,
                (ci & 4) ? max.z : min.z);
            const glm::vec4 clip = viewProj * glm::vec4(corner, 1.0f);
            const float w = clip.w;
            if (std::fabs(w) < 1e-9f) {
                continue;
            }
            const float nx = clip.x / w;
            const float ny = clip.y / w;
            const float nd = clip.z / w;  // monotonic along the view
            out.minX = std::min(out.minX, nx);
            out.minY = std::min(out.minY, ny);
            out.maxX = std::max(out.maxX, nx);
            out.maxY = std::max(out.maxY, ny);
            out.minDepth = std::min(out.minDepth, nd);
            out.maxDepth = std::max(out.maxDepth, nd);
        }
        return out;
    }

    static bool parseJson(const std::string& text, SceneCullingConfig& out,
                          std::string& errorOut) {
        struct Pair {
            std::string key;
            std::string value;
        };
        std::vector<Pair> pairs;
        {
            std::size_t i = 0;
            auto skipWs = [&]() {
                while (i < text.size() &&
                       (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' ||
                        text[i] == '\r')) {
                    ++i;
                }
            };
            skipWs();
            if (i >= text.size() || text[i] != '{') {
                errorOut = "SceneCulling config: expected '{'";
                return false;
            }
            ++i;
            skipWs();
            if (i < text.size() && text[i] == '}') return true;
            while (i < text.size()) {
                skipWs();
                if (i >= text.size() || text[i] != '"') {
                    errorOut = "SceneCulling config: expected key";
                    return false;
                }
                ++i;
                std::string key;
                while (i < text.size() && text[i] != '"') key.push_back(text[i++]);
                if (i >= text.size()) {
                    errorOut = "SceneCulling config: unterminated key";
                    return false;
                }
                ++i;
                skipWs();
                if (i >= text.size() || text[i] != ':') {
                    errorOut = "SceneCulling config: expected ':'";
                    return false;
                }
                ++i;
                skipWs();
                std::string value;
                if (i < text.size() && text[i] == '"') {
                    ++i;
                    while (i < text.size() && text[i] != '"') value.push_back(text[i++]);
                    if (i >= text.size()) {
                        errorOut = "SceneCulling config: unterminated string";
                        return false;
                    }
                    ++i;
                } else {
                    while (i < text.size() && text[i] != ',' && text[i] != '}') {
                        value.push_back(text[i++]);
                    }
                }
                pairs.push_back({key, value});
                skipWs();
                if (i < text.size() && text[i] == ',') {
                    ++i;
                    continue;
                }
                if (i < text.size() && text[i] == '}') {
                    ++i;
                    break;
                }
                errorOut = "SceneCulling config: expected ',' or '}'";
                return false;
            }
        }

        SceneCullingConfig parsed = out;
        bool sawVersion = false;
        for (const Pair& p : pairs) {
            if (p.key == "version") {
                if (p.value != "1") {
                    errorOut = "SceneCulling config: unsupported version";
                    return false;
                }
                sawVersion = true;
            } else if (p.key == "lod0Distance") {
                parsed.lod0Distance = std::stof(p.value);
            } else if (p.key == "lodHysteresis") {
                parsed.lodHysteresis = std::stof(p.value);
            } else if (p.key == "maxInstances") {
                parsed.maxInstances =
                    static_cast<std::uint32_t>(std::stoul(p.value));
            } else {
                errorOut = "SceneCulling config: unknown key '" + p.key + "'";
                return false;
            }
        }
        if (!sawVersion) {
            errorOut = "SceneCulling config: missing version";
            return false;
        }
        std::string validityError;
        if (!parsed.valid(validityError)) {
            errorOut = "SceneCulling config: " + validityError;
            return false;
        }
        out = parsed;
        return true;
    }

    SceneCullingConfig config_{};
};

}  // namespace

bool SceneCullingConfig::valid(std::string& errorOut) const {
    if (!(lod0Distance > 0.0f) || !std::isfinite(lod0Distance)) {
        errorOut = "lod0Distance must be finite and > 0";
        return false;
    }
    if (!(lodHysteresis >= 0.0f && lodHysteresis <= 0.5f)) {
        errorOut = "lodHysteresis must be in [0, 0.5]";
        return false;
    }
    if (!(maxInstances >= 1 && maxInstances <= (1u << 20))) {
        errorOut = "maxInstances must be in [1, 1<<20]";
        return false;
    }
    return true;
}

std::unique_ptr<ISceneCulling> create_scene_culling(std::string& errorOut) {
    auto impl = std::make_unique<SceneCulling>();
    if (!impl) {
        errorOut = "SceneCulling: allocation failed";
        return nullptr;
    }
    return impl;
}

std::unique_ptr<ISceneCulling> create_scene_culling_json(
    const std::string& jsonText, std::string& errorOut) {
    auto impl = std::make_unique<SceneCulling>();
    if (!impl->configure_json(jsonText, errorOut)) {
        return nullptr;
    }
    return impl;
}

}  // namespace Engine::Rendering
