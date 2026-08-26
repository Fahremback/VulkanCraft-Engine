#pragma once

// ISpirvReflection — Agente 1 (task_plan C1): the PUBLIC SPIR-V reflection
// contract. Parses a SPIR-V module and reports its resource layout — uniform
// buffers, storage buffers, sampled images, storage images, combined samplers,
// push constant blocks and descriptor sets — so SDK/MCP/editor (and the Render
// Graph from B2) can build pipelines/passes without hardcoding layouts. The
// backend (spirv-reflect) is CPU-only; this surface is self-contained std.
//
// Deterministic: the same module bytes reflect to the same layout. All-or-
// nothing: invalid SPIR-V refuses with a diagnostic and leaves `out` empty.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Engine::Rendering {

enum class SpirvDescriptorType : std::uint8_t {
    UniformBuffer,
    StorageBuffer,
    SampledImage,
    StorageImage,
    CombinedImageSampler,
    UniformTexelBuffer,
    StorageTexelBuffer,
    InputAttachment,
    AccelerationStructure,
    InlineUniformBlock,
    Other,
};

enum class SpirvShaderStage : std::uint8_t {
    Vertex,
    TessellationControl,
    TessellationEvaluation,
    Geometry,
    Fragment,
    Compute,
    RayGeneration,
    Intersection,
    AnyHit,
    ClosestHit,
    Miss,
    Callable,
    Mesh,
    Task,
    Unknown,
};

struct SpirvDescriptorBinding {
    std::string name;
    std::uint32_t set{0};
    std::uint32_t binding{0};
    SpirvDescriptorType type{SpirvDescriptorType::Other};
    std::uint32_t count{1};  // array size; 1 = scalar
    std::uint32_t accessed{0};
};

struct SpirvDescriptorSet {
    std::uint32_t set{0};
    std::vector<SpirvDescriptorBinding> bindings;
};

struct SpirvPushConstantBlock {
    std::string name;
    std::uint32_t size{0};
    std::uint32_t offset{0};
};

struct SpirvReflection {
    SpirvShaderStage stage{SpirvShaderStage::Unknown};
    std::string entryPoint;
    std::vector<SpirvDescriptorSet> sets;           // sorted by set id
    std::vector<SpirvPushConstantBlock> pushConstants;
    std::uint32_t descriptorBindingCount{0};        // total across sets
};

// Public factory. `module` is a pointer to the SPIR-V words (little-endian
// uint32), `wordCount` the number of words. Returns nullptr with a diagnostic
// in errorOut on invalid input (never partial reflection).
std::unique_ptr<SpirvReflection> reflect_spirv_module(
    const std::uint32_t* module, std::size_t wordCount, std::string& errorOut);

}  // namespace Engine::Rendering
