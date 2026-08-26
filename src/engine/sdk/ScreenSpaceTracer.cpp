// ScreenSpaceTracer.cpp — Agente 1 (task_plan A.6): the HEADLESS screen-space
// ray march. Deterministic march against an injected depth sampler, with
// off-screen detection, binary refinement, and history helpers (reprojection
// and disocclusion). Self-contained (std + glm), no GPU.

#include "engine/rendering/IScreenSpaceTracer.hpp"

#include <cmath>

namespace Engine::Rendering {
namespace {

constexpr float kEps = 1.0e-6f;

glm::vec3 normalize_dir(const glm::vec3& dir) {
    const float lenSq = glm::dot(dir, dir);
    if (lenSq <= kEps) return glm::vec3(0.0f, 0.0f, -1.0f);
    return dir * glm::inversesqrt(lenSq);
}

class ScreenSpaceTracer final : public IScreenSpaceTracer {
public:
    ScreenSpaceTracer() = default;

    bool configure(const ScreenTraceConfig& requested, std::string& errorOut) override {
        if (requested.maxSteps < 1) {
            errorOut = "screen_trace: maxSteps must be >= 1";
            return false;
        }
        if (!std::isfinite(requested.stepSize) || requested.stepSize <= 0.0f) {
            errorOut = "screen_trace: stepSize must be > 0";
            return false;
        }
        if (!std::isfinite(requested.depthBias) || requested.depthBias <= 0.0f) {
            errorOut = "screen_trace: depthBias must be > 0";
            return false;
        }
        if (!std::isfinite(requested.refineSteps) || requested.refineSteps < 0.0f) {
            errorOut = "screen_trace: refineSteps must be >= 0";
            return false;
        }
        if (requested.viewportWidth < 1 || requested.viewportHeight < 1) {
            errorOut = "screen_trace: viewport must be >= 1x1";
            return false;
        }
        config_ = requested;
        errorOut.clear();
        return true;
    }

    const ScreenTraceConfig& config() const noexcept override { return config_; }

    ScreenTraceHit trace(const glm::vec3& viewOrigin, const glm::vec3& viewDir,
                         const DepthSampler& depth) const override {
        ScreenTraceHit result;
        if (!depth) return result;
        const glm::vec3 dir = normalize_dir(viewDir);

        // Screen projection: x_ndc = x * f / -z, y_ndc = y * f / -z where f is a
        // focal constant derived from the viewport aspect (a canonical 90°-ish
        // perspective). The sampler is UV-based, so we pick f = 1.0 and map NDC
        // -> UV; the depth sampler returns linear depth (>= 0) in the SAME view
        // space, so the crossing test is `-p.z > depth(uv) + bias`.
        const float f = 1.0f;
        const auto project = [&](const glm::vec3& p, glm::vec2& uv, float& rayDepth,
                                 bool& inView) {
            const float w = -p.z;  // camera looks down -Z
            if (w <= kEps) {       // behind the camera
                inView = false;
                return;
            }
            const float nx = (p.x * f) / w;
            const float ny = (p.y * f) / w;
            uv = glm::vec2(nx * 0.5f + 0.5f, ny * 0.5f + 0.5f);
            rayDepth = w;
            inView = uv.x >= 0.0f && uv.x <= 1.0f && uv.y >= 0.0f && uv.y <= 1.0f;
        };

        // Start one step off the camera (the camera itself is at w == 0).
        float t = config_.stepSize;
        float prevT = 0.0f;
        glm::vec3 prevPoint = viewOrigin;

        for (std::uint32_t step = 0; step < config_.maxSteps; ++step) {
            result.steps = step + 1;
            const glm::vec3 p = viewOrigin + dir * t;
            glm::vec2 uv{};
            float rayDepth = 0.0f;
            bool inView = false;
            project(p, uv, rayDepth, inView);
            if (!inView) {
                result.offscreen = true;
                return result;
            }
            const float surfaceDepth = depth(uv);
            const bool behind = rayDepth > surfaceDepth + config_.depthBias;
            if (behind) {
                // Binary refine between prevPoint (in front) and p (behind).
                float lo = prevT;
                float hi = t;
                for (int r = 0; r < static_cast<int>(config_.refineSteps); ++r) {
                    const float mid = 0.5f * (lo + hi);
                    const glm::vec3 mp = viewOrigin + dir * mid;
                    glm::vec2 muv{};
                    float mDepth = 0.0f;
                    bool mIn = false;
                    project(mp, muv, mDepth, mIn);
                    if (!mIn || mDepth > depth(muv) + config_.depthBias) {
                        hi = mid;
                    } else {
                        lo = mid;
                    }
                }
                const float hitT = 0.5f * (lo + hi);
                const glm::vec3 hitPoint = viewOrigin + dir * hitT;
                glm::vec2 hitUv{};
                float hitDepth = 0.0f;
                bool hitIn = false;
                project(hitPoint, hitUv, hitDepth, hitIn);
                result.hit = true;
                result.t = hitT;
                result.uv = hitUv;
                result.position = hitPoint;
                return result;
            }
            prevT = t;
            prevPoint = p;
            t += config_.stepSize;
        }
        result.t = config_.stepSize * config_.maxSteps;
        return result;
    }

    ReprojectResult reproject(const glm::vec3& viewPoint,
                              const glm::mat4& prevViewProjection,
                              const glm::mat4& currViewProjection) const override {
        ReprojectResult out;
        // World position implied by the CURRENT view (assume the point is in
        // current view space); invert the current VP to world, then project into
        // the previous VP. If the matrices are identity (degenerate), fall back
        // to the identity mapping so the helper is usable in tests with simple
        // matrices.
        const glm::vec4 world = glm::inverse(currViewProjection) * glm::vec4(viewPoint, 1.0f);
        const glm::vec4 prevClip = prevViewProjection * world;
        const float w = prevClip.w;
        if (w <= kEps) return out;
        const glm::vec3 ndc = glm::vec3(prevClip) / w;
        // Valid = in front of the previous camera AND inside its screen
        // rectangle. Depth is NOT range-gated (an unbounded/reverse-Z
        // projection legitimately yields |z| > 1); the consumer uses `depth`
        // for the disocclusion test only.
        const bool inside =
            ndc.x >= -1.0f && ndc.x <= 1.0f && ndc.y >= -1.0f && ndc.y <= 1.0f;
        out.valid = inside;
        out.uv = glm::vec2(ndc.x * 0.5f + 0.5f, ndc.y * 0.5f + 0.5f);
        out.depth = ndc.z;
        return out;
    }

    bool disoccluded(float reprojectedDepth, float currentDepth,
                     float threshold) const override {
        return std::fabs(reprojectedDepth - currentDepth) > threshold;
    }

private:
    ScreenTraceConfig config_{};
};

}  // namespace

std::unique_ptr<IScreenSpaceTracer> create_screen_space_tracer(
    std::string& errorOut) {
    auto tracer = std::make_unique<ScreenSpaceTracer>();
    ScreenTraceConfig defaults;
    if (!tracer->configure(defaults, errorOut)) return nullptr;
    return tracer;
}

}  // namespace Engine::Rendering
