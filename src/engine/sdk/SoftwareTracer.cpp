// SoftwareTracer.cpp — Agente 1 (task_plan A.7): the HEADLESS software ray
// tracer (sphere tracing against a signed distance field). Deterministic
// march; analytic surface normals via central differences; shadow-ray
// occlusion. Self-contained (std + glm), no GPU.

#include "engine/rendering/ISoftwareTracer.hpp"

#include <cmath>

namespace Engine::Rendering {
namespace {

constexpr float kEps = 1.0e-6f;

glm::vec3 normalize_dir(const glm::vec3& dir) {
    const float lenSq = glm::dot(dir, dir);
    if (lenSq <= kEps) return glm::vec3(1.0f, 0.0f, 0.0f);
    return dir * glm::inversesqrt(lenSq);
}

class SoftwareTracer final : public ISoftwareTracer {
public:
    SoftwareTracer() = default;

    bool configure(const SoftwareTraceConfig& requested, std::string& errorOut) override {
        if (requested.maxSteps < 1) {
            errorOut = "software_trace: maxSteps must be >= 1";
            return false;
        }
        if (!std::isfinite(requested.maxDistance) || requested.maxDistance <= 0.0f) {
            errorOut = "software_trace: maxDistance must be > 0";
            return false;
        }
        if (!std::isfinite(requested.hitEpsilon) || requested.hitEpsilon <= 0.0f) {
            errorOut = "software_trace: hitEpsilon must be > 0";
            return false;
        }
        if (!std::isfinite(requested.normalEpsilon) || requested.normalEpsilon <= 0.0f) {
            errorOut = "software_trace: normalEpsilon must be > 0";
            return false;
        }
        config_ = requested;
        errorOut.clear();
        return true;
    }

    const SoftwareTraceConfig& config() const noexcept override { return config_; }

    SoftwareTraceHit trace(const glm::vec3& origin, const glm::vec3& dir,
                           const DistanceField& sdf) const override {
        SoftwareTraceHit result;
        if (!sdf) return result;
        const glm::vec3 d = normalize_dir(dir);
        float t = 0.0f;
        for (std::uint32_t step = 0; step < config_.maxSteps; ++step) {
            const glm::vec3 p = origin + d * t;
            const float distance = sdf(p);
            result.steps = step + 1;
            if (distance <= config_.hitEpsilon) {
                result.hit = true;
                result.distance = t;
                result.position = p;
                result.normal = surface_normal(p, sdf);
                return result;
            }
            t += std::max(distance, 0.0f);
            if (t > config_.maxDistance) break;
        }
        result.distance = config_.maxDistance;
        return result;
    }

    bool occluded(const glm::vec3& origin, const glm::vec3& to,
                  const DistanceField& sdf) const override {
        if (!sdf) return false;
        const glm::vec3 delta = to - origin;
        const float target = glm::length(delta);
        if (target <= kEps) return false;
        const glm::vec3 d = delta / target;
        float t = 0.0f;
        for (std::uint32_t step = 0; step < config_.maxSteps; ++step) {
            const float distance = sdf(origin + d * t);
            if (distance <= config_.hitEpsilon) {
                // A surface strictly before `to` (with margin) occludes.
                return t < target - config_.hitEpsilon;
            }
            t += std::max(distance, 0.0f);
            if (t >= target) break;
        }
        return false;
    }

    glm::vec3 surface_normal(const glm::vec3& p,
                             const DistanceField& sdf) const override {
        if (!sdf) return glm::vec3(0.0f, 1.0f, 0.0f);
        const float e = config_.normalEpsilon;
        const float dx = sdf(glm::vec3(p.x + e, p.y, p.z)) -
                         sdf(glm::vec3(p.x - e, p.y, p.z));
        const float dy = sdf(glm::vec3(p.x, p.y + e, p.z)) -
                         sdf(glm::vec3(p.x, p.y - e, p.z));
        const float dz = sdf(glm::vec3(p.x, p.y, p.z + e)) -
                         sdf(glm::vec3(p.x, p.y, p.z - e));
        const glm::vec3 g(dx, dy, dz);
        const float lenSq = glm::dot(g, g);
        return lenSq > kEps ? g * glm::inversesqrt(lenSq) : glm::vec3(0.0f, 1.0f, 0.0f);
    }

private:
    SoftwareTraceConfig config_{};
};

}  // namespace

std::unique_ptr<ISoftwareTracer> create_software_tracer(std::string& errorOut) {
    auto tracer = std::make_unique<SoftwareTracer>();
    SoftwareTraceConfig defaults;
    if (!tracer->configure(defaults, errorOut)) return nullptr;
    return tracer;
}

}  // namespace Engine::Rendering
