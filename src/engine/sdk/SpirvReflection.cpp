// SpirvReflection.cpp — Agente 1 (task_plan C1): the ONLY TU that includes
// spirv-reflect headers. Maps the promoted backend (external/solutions/
// spirv-reflect, Apache-2.0, commit 3954c1e…) to the public contract
// engine/rendering/ISpirvReflection.hpp. Compiles the clone's spirv_reflect.c
// as C++ via the upstream spirv_reflect.cpp shim (target vc_spirv_reflect).
// Pure CPU, deterministic, all-or-nothing.

#include "engine/rendering/ISpirvReflection.hpp"

#include "spirv_reflect.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace Engine::Rendering {
namespace {

SpirvDescriptorType map_descriptor_type(SpvReflectDescriptorType type) noexcept {
    switch (type) {
    case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
    case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
        return SpirvDescriptorType::UniformBuffer;
    case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER:
    case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
        return SpirvDescriptorType::StorageBuffer;
    case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        return SpirvDescriptorType::SampledImage;
    case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        return SpirvDescriptorType::StorageImage;
    case SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        return SpirvDescriptorType::CombinedImageSampler;
    case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        return SpirvDescriptorType::UniformTexelBuffer;
    case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
        return SpirvDescriptorType::StorageTexelBuffer;
    case SPV_REFLECT_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
        return SpirvDescriptorType::InputAttachment;
    case SPV_REFLECT_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
        return SpirvDescriptorType::AccelerationStructure;
    default:
        return SpirvDescriptorType::Other;
    }
}

SpirvShaderStage map_stage(SpvReflectShaderStageFlagBits stage) noexcept {
    switch (stage) {
    case SPV_REFLECT_SHADER_STAGE_VERTEX_BIT: return SpirvShaderStage::Vertex;
    case SPV_REFLECT_SHADER_STAGE_TESSELLATION_CONTROL_BIT: return SpirvShaderStage::TessellationControl;
    case SPV_REFLECT_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: return SpirvShaderStage::TessellationEvaluation;
    case SPV_REFLECT_SHADER_STAGE_GEOMETRY_BIT: return SpirvShaderStage::Geometry;
    case SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT: return SpirvShaderStage::Fragment;
    case SPV_REFLECT_SHADER_STAGE_COMPUTE_BIT: return SpirvShaderStage::Compute;
    case SPV_REFLECT_SHADER_STAGE_RAYGEN_BIT_KHR: return SpirvShaderStage::RayGeneration;
    case SPV_REFLECT_SHADER_STAGE_INTERSECTION_BIT_KHR: return SpirvShaderStage::Intersection;
    case SPV_REFLECT_SHADER_STAGE_ANY_HIT_BIT_KHR: return SpirvShaderStage::AnyHit;
    case SPV_REFLECT_SHADER_STAGE_CLOSEST_HIT_BIT_KHR: return SpirvShaderStage::ClosestHit;
    case SPV_REFLECT_SHADER_STAGE_MISS_BIT_KHR: return SpirvShaderStage::Miss;
    case SPV_REFLECT_SHADER_STAGE_CALLABLE_BIT_KHR: return SpirvShaderStage::Callable;
    case SPV_REFLECT_SHADER_STAGE_MESH_BIT_EXT: return SpirvShaderStage::Mesh;
    case SPV_REFLECT_SHADER_STAGE_TASK_BIT_EXT: return SpirvShaderStage::Task;
    default: return SpirvShaderStage::Unknown;
    }
}

}  // namespace

std::unique_ptr<SpirvReflection> reflect_spirv_module(
    const std::uint32_t* module, std::size_t wordCount, std::string& errorOut) {
    errorOut.clear();
    if (module == nullptr || wordCount == 0) {
        errorOut = "empty SPIR-V module";
        return nullptr;
    }

    SpvReflectShaderModule spvModule;
    const SpvReflectResult createResult = spvReflectCreateShaderModule(
        static_cast<size_t>(wordCount) * sizeof(std::uint32_t),
        static_cast<const void*>(module), &spvModule);
    if (createResult != SPV_REFLECT_RESULT_SUCCESS) {
        errorOut = "spirv-reflect: create failed (invalid module)";
        return nullptr;
    }

    auto result = std::make_unique<SpirvReflection>();
    result->stage = map_stage(spvModule.shader_stage);
    if (spvModule.entry_point_name != nullptr) result->entryPoint = spvModule.entry_point_name;

    std::uint32_t setCount = 0;
    if (spvReflectEnumerateDescriptorSets(&spvModule, &setCount, nullptr) == SPV_REFLECT_RESULT_SUCCESS &&
        setCount > 0) {
        std::vector<SpvReflectDescriptorSet*> sets(setCount);
        const SpvReflectResult setsResult = spvReflectEnumerateDescriptorSets(
            &spvModule, &setCount, sets.data());
        if (setsResult == SPV_REFLECT_RESULT_SUCCESS) {
            for (std::uint32_t i = 0; i < setCount; ++i) {
                const SpvReflectDescriptorSet* set = sets[i];
                SpirvDescriptorSet outSet;
                outSet.set = set->set;
                for (std::uint32_t b = 0; b < set->binding_count; ++b) {
                    const SpvReflectDescriptorBinding* binding = set->bindings[b];
                    SpirvDescriptorBinding outBinding;
                    if (binding->name != nullptr) outBinding.name = binding->name;
                    outBinding.set = binding->set;
                    outBinding.binding = binding->binding;
                    outBinding.type = map_descriptor_type(binding->descriptor_type);
                    outBinding.count = binding->count;
                    outBinding.accessed = binding->accessed;
                    outSet.bindings.push_back(std::move(outBinding));
                }
                std::sort(outSet.bindings.begin(), outSet.bindings.end(),
                          [](const SpirvDescriptorBinding& a, const SpirvDescriptorBinding& b) {
                              return a.binding < b.binding;
                          });
                result->descriptorBindingCount += static_cast<std::uint32_t>(outSet.bindings.size());
                result->sets.push_back(std::move(outSet));
            }
        }
    }
    std::sort(result->sets.begin(), result->sets.end(),
              [](const SpirvDescriptorSet& a, const SpirvDescriptorSet& b) { return a.set < b.set; });

    std::uint32_t pushCount = 0;
    if (spvReflectEnumeratePushConstantBlocks(&spvModule, &pushCount, nullptr) == SPV_REFLECT_RESULT_SUCCESS &&
        pushCount > 0) {
        std::vector<SpvReflectBlockVariable*> pushBlocks(pushCount);
        const SpvReflectResult pushResult = spvReflectEnumeratePushConstantBlocks(
            &spvModule, &pushCount, pushBlocks.data());
        if (pushResult == SPV_REFLECT_RESULT_SUCCESS) {
            for (std::uint32_t i = 0; i < pushCount; ++i) {
                const SpvReflectBlockVariable* block = pushBlocks[i];
                SpirvPushConstantBlock outBlock;
                if (block->name != nullptr) outBlock.name = block->name;
                outBlock.size = static_cast<std::uint32_t>(block->size);
                outBlock.offset = block->offset;
                result->pushConstants.push_back(std::move(outBlock));
            }
        }
    }
    std::sort(result->pushConstants.begin(), result->pushConstants.end(),
              [](const SpirvPushConstantBlock& a, const SpirvPushConstantBlock& b) {
                  return a.offset < b.offset;
              });

    spvReflectDestroyShaderModule(&spvModule);
    return result;
}

}  // namespace Engine::Rendering
