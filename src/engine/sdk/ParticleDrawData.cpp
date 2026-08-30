// ParticleDrawData.cpp — the only TU implementing IParticleDrawData
// (Agente 1 task_plan F.22): turns alive particle instances into REAL
// vertex + indirect draw data for the particle pass. Headless, deterministic
// (std + glm only). The indirect command is field-compatible with
// VkDrawIndirectCommand so a real vkCmdDrawIndirect pass stages `out` and
// binds the packed interleaved vertex buffer (`count * 6 * 8` floats).

#include "engine/rendering/IParticleDrawData.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace Engine::Rendering {

namespace {

// One camera-facing quad = two triangles (6 vertices). The size sits in the
// vertex's `size` attribute; a billboard shader expands px py pz by it.
constexpr std::uint32_t kVerticesPerParticle = 6;
constexpr float kMaxSize = 1024.0f;

class ParticleDrawData final : public IParticleDrawData {
public:
    bool push(const ParticleInstance& instance, std::string& errorOut) override {
        if (!(instance.size > 0.0f) || instance.size > kMaxSize) {
            errorOut = "particle size out of range";
            return false;
        }
        const glm::vec4& c = instance.color;
        if (c.r < 0.0f || c.r > 1.0f || c.g < 0.0f || c.g > 1.0f ||
            c.b < 0.0f || c.b > 1.0f || c.a < 0.0f || c.a > 1.0f) {
            errorOut = "particle color channel out of range";
            return false;
        }
        instances_.push_back(instance);
        return true;
    }

    std::size_t count() const override { return instances_.size(); }

    void clear() override { instances_.clear(); }

    bool build(IndirectDrawCommand& out, std::vector<float>& vertexBuffer,
               std::string& errorOut) override {
        // Re-validate the whole batch all-or-nothing before emitting anything.
        for (const ParticleInstance& p : instances_) {
            if (!(p.size > 0.0f) || p.size > kMaxSize) {
                errorOut = "particle size out of range";
                return false;
            }
            const glm::vec4& c = p.color;
            if (c.r < 0.0f || c.r > 1.0f || c.g < 0.0f || c.g > 1.0f ||
                c.b < 0.0f || c.b > 1.0f || c.a < 0.0f || c.a > 1.0f) {
                errorOut = "particle color channel out of range";
                return false;
            }
        }
        vertexBuffer.clear();
        vertexBuffer.reserve(instances_.size() * kVerticesPerParticle * 8u);
        for (const ParticleInstance& p : instances_) {
            const float base[8] = { p.position.x, p.position.y, p.position.z,
                                    p.size, p.color.r, p.color.g, p.color.b, p.color.a };
            for (std::uint32_t v = 0; v < kVerticesPerParticle; ++v) {
                vertexBuffer.insert(vertexBuffer.end(), base, base + 8);
            }
        }
        out.vertexCount = static_cast<std::uint32_t>(instances_.size()) * kVerticesPerParticle;
        out.instanceCount = 1;
        out.firstVertex = 0;
        out.firstInstance = 0;
        errorOut.clear();
        return true;
    }

private:
    std::vector<ParticleInstance> instances_;
};

}  // namespace

std::unique_ptr<IParticleDrawData> create_particle_draw_data() {
    return std::make_unique<ParticleDrawData>();
}

}  // namespace Engine::Rendering