// IShaderCompiler — Agente 1 (task_plan B.2/C.15): the PUBLIC shader compilation
// contract. Headless, deterministic, self-contained std — no Vulkan device required.
//
// Compile GLSL source to SPIR-V binary via glslc (Vulkan SDK).
// Validate the output with spirv-val.
// All-or-nothing: any invalid input → error + all outputs unchanged.
// JSON round-trip: config → JSON → config is bit-exact.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vc::rendering {

// Shader stage bitmask (matches Vulkan VkShaderStageFlagBits values).
enum class ShaderStage : uint32_t {
    Vertex   = 0x00000001,
    Fragment = 0x00000004,
    Compute  = 0x00000020,
    Task     = 0x00000040,   // VK_SHADER_STAGE_TASK_BIT_EXT (GL_EXT_mesh_shader)
    Mesh     = 0x00000080,   // VK_SHADER_STAGE_MESH_BIT_EXT (GL_EXT_mesh_shader)
};

// Compilation configuration — all-or-nothing (NaN/limits refused).
struct ShaderCompilerConfig {
    // Path to glslc executable (empty = auto-detect from VULKAN_SDK env).
    std::string glslcPath;
    // Path to spirv-val executable (empty = auto-detect from VULKAN_SDK env).
    std::string spirvValPath;
    // Target SPIR-V version (e.g. "1.5" for Vulkan 1.3). Empty = default.
    std::string targetEnv;
    // Optimization level: 0 = none, 1 = size, 2 = speed.
    int optLevel = 0;
    // Define macros (e.g. {"MAX_LIGHTS=64"}).
    std::vector<std::string> defines;

    // All-or-nothing validation. Returns true if config is valid.
    bool validate() const;

    // JSON round-trip.
    std::string toJson() const;
    static ShaderCompilerConfig fromJson(const std::string& json, std::string& errorOut);
};

// Compilation result.
struct ShaderCompileResult {
    bool success = false;
    std::vector<uint32_t> spirv;         // SPIR-V binary (words).
    std::string errorLog;                 // Compiler/validation errors.
    uint32_t stageMask = 0;              // Detected stages in the binary.
    size_t sourceSize = 0;               // Original GLSL size (bytes).
    size_t spirvSize = 0;                // SPIR-V size (bytes).
};

// The public contract.
class IShaderCompiler {
public:
    virtual ~IShaderCompiler() = default;

    // Compile a single GLSL source to SPIR-V.
    // source: GLSL source code (null-terminated).
    // stage: shader stage hint for glslc.
    // config: compilation configuration.
    // errorOut: populated on failure.
    // Returns: compiled SPIR-V binary (empty on failure).
    virtual std::vector<uint32_t> compile(
        const char* source,
        ShaderStage stage,
        const ShaderCompilerConfig& config,
        std::string& errorOut) = 0;

    // Compile and validate (spirv-val) in one step.
    virtual ShaderCompileResult compileAndValidate(
        const char* source,
        ShaderStage stage,
        const ShaderCompilerConfig& config) = 0;

    // Validate an existing SPIR-V binary.
    virtual bool validate(
        const uint32_t* spirv,
        size_t wordCount,
        const ShaderCompilerConfig& config,
        std::string& errorOut) = 0;

    // Optimize SPIR-V (wraps spirv-opt).
    virtual std::vector<uint32_t> optimize(
        const uint32_t* spirv,
        size_t wordCount,
        int optLevel,
        std::string& errorOut) = 0;

    // Get the list of supported optimization passes.
    virtual std::vector<std::string> availablePasses() const = 0;
};

// Factory.
std::unique_ptr<IShaderCompiler> create_shader_compiler(std::string& errorOut);

} // namespace vc::rendering
