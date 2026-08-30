#pragma once

// IParticleDrawData — Agente 1 (task_plan F.22), the PUBLIC core that turns
// alive particle instances into REAL vertex + indirect draw data consumed by
// the particle GPU pass (not just a count in a UBO). Before this core the
// particle path only published an instance counter to the feature contract;
// this builder always emits the indirect command AND the packed interleaved
// vertex buffer a real vkCmdDrawIndirect pass stages.
//
// Each particle becomes a camera-facing quad (two triangles = 6 vertices) with
// position + size + color per vertex. `build()` produces:
//   out.vertexCount   = 6 * instanceCount   (== VkDrawIndirectCommand.vertexCount)
//   out.instanceCount = 1                    (single multi-vertex draw)
//   out.firstVertex   = 0
//   out.firstInstance = 0
// and a flat vertex buffer of `count * 6` ParticleDrawVertex (8 floats each:
// px py pz | size | r g b a) that the pass uploads and binds.
//
// Self-contained (std + glm); headless and deterministic (no RNG, no GPU
// state, no external headers). All-or-nothing: build() on zero instances is a
// no-op that yields vertexCount == 0 (an empty draw), and an out-of-range
// size/color is refused, never clamped.

#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>

namespace Engine::Rendering {

// One camera-facing quad vertex (position, world-space size scale, RGBA color).
struct ParticleDrawVertex {
    float px{ 0.0f };
    float py{ 0.0f };
    float pz{ 0.0f };
    float size{ 1.0f };   // world-size scale; in (0, 1024]
    float r{ 1.0f };
    float g{ 1.0f };
    float b{ 1.0f };
    float a{ 1.0f };
};

// One live particle instance submitted to the batch (spawn position, size,
// color). All-or-nothing: size <= 0 or a color channel outside [0, 1] refuses.
struct ParticleInstance {
    glm::vec3 position{ 0.0f, 0.0f, 0.0f };
    float size{ 1.0f };
    glm::vec4 color{ 1.0f, 1.0f, 1.0f, 1.0f };
};

// A VkDrawIndirectCommand (field-compatible layout: vertexCount, instanceCount,
// firstVertex, firstInstance) for the particle pass.
struct IndirectDrawCommand {
    std::uint32_t vertexCount{ 0 };
    std::uint32_t instanceCount{ 1 };
    std::uint32_t firstVertex{ 0 };
    std::uint32_t firstInstance{ 0 };
};

class IParticleDrawData {
public:
    virtual ~IParticleDrawData() = default;

    // Submits one live particle instance to the batch (all-or-nothing on an
    // invalid size/color; never clamps).
    virtual bool push(const ParticleInstance& instance, std::string& errorOut) = 0;

    // Number of instances currently batched.
    virtual std::size_t count() const = 0;

    // Empties the batch (start of a new frame).
    virtual void clear() = 0;

    // Builds the REAL indirect command + packed vertex buffer (8 floats per
    // vertex, count*6 vertices). Returns false with a diagnostic when an
    // instance in the batch is invalid; on success `out` carries the command a
    // vkCmdDrawIndirect pass stages and `vertexBuffer` the interleaved data it
    // uploads+binds. Zero instances -> vertexCount 0, empty buffer (legal no-op).
    virtual bool build(IndirectDrawCommand& out,
                       std::vector<float>& vertexBuffer,
                       std::string& errorOut) = 0;
};

// Creates the particle draw-data builder (the only TU implementing it).
std::unique_ptr<IParticleDrawData> create_particle_draw_data();

}  // namespace Engine::Rendering