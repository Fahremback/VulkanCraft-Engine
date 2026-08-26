// ShaderCompilerTests.cpp — gate for IShaderCompiler (B.2/C.15)
// Headless: uses glslc/spirv-val/spirv-opt from Vulkan SDK.

#include "engine/rendering/IShaderCompiler.hpp"

#include <cstdio>
#include <cstring>
#include <string>

using namespace vc::rendering;

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("  FAIL: %s\n", msg); g_failed++; } \
    else { g_passed++; } \
} while(0)

// Minimal valid GLSL shaders (all have layout locations for SPIR-V).
static const char* kVert = R"(
#version 450
layout(location=0) in vec3 pos;
layout(location=1) in vec3 normal;
layout(location=0) out vec3 vNormal;
void main() {
    vNormal = normal;
    gl_Position = vec4(pos, 1.0);
}
)";

static const char* kFrag = R"(
#version 450
layout(location=0) in vec3 vNormal;
layout(location=0) out vec4 fragColor;
void main() {
    fragColor = vec4(normalize(vNormal), 1.0);
}
)";

static const char* kComp = R"(
#version 450
layout(local_size_x=16) in;
layout(set=0, binding=0, rgba8) uniform writeonly image2D outImg;
void main() {
    imageStore(outImg, ivec2(gl_GlobalInvocationID.xy), vec4(1.0));
}
)";

int main() {
    std::printf("[shader-compiler] ALL tests starting\n");

    // 1. Config all-or-nothing.
    std::printf("[shader-compiler] test config all-or-nothing\n");
    { ShaderCompilerConfig c; c.optLevel = 5; CHECK(!c.validate(), "optLevel=5 invalid"); }
    { ShaderCompilerConfig c; c.targetEnv = "abc"; CHECK(!c.validate(), "targetEnv=abc invalid"); }
    { ShaderCompilerConfig c; CHECK(c.validate(), "default valid"); }
    { ShaderCompilerConfig c; c.optLevel = 2; c.targetEnv = "1.5"; c.defines = {"A=1"};
      CHECK(c.validate(), "full config valid"); }

    // 2. JSON round-trip.
    std::printf("[shader-compiler] test JSON round-trip\n");
    {
        ShaderCompilerConfig c; c.targetEnv = "1.5"; c.optLevel = 1; c.defines = {"X","Y"};
        std::string json = c.toJson();
        std::string err;
        auto r = ShaderCompilerConfig::fromJson(json, err);
        CHECK(err.empty(), "no parse error");
        CHECK(r.targetEnv == "1.5", "targetEnv round-trip");
        CHECK(r.optLevel == 1, "optLevel round-trip");
        CHECK(r.defines.size() == 2, "defines size");
    }

    // Create compiler.
    std::string err;
    auto compiler = create_shader_compiler(err);
    if (!compiler) {
        std::printf("  SKIP all compile tests (glslc not available: %s)\n", err.c_str());
        std::printf("\n[shader-compiler] Results: %d passed, %d failed\n", g_passed, g_failed);
        return g_failed > 0 ? 1 : 0;
    }

    // 3. Compile vertex.
    std::printf("[shader-compiler] test compile vertex\n");
    {
        ShaderCompilerConfig cfg;
        auto spirv = compiler->compile(kVert, ShaderStage::Vertex, cfg, err);
        if (spirv.empty()) std::printf("  DEBUG: %s\n", err.c_str());
        CHECK(!spirv.empty(), "vertex not empty");
        if (!spirv.empty()) CHECK(spirv[0] == 0x07230203, "vertex magic");
    }

    // 4. Compile + validate fragment.
    std::printf("[shader-compiler] test compile+validate fragment\n");
    {
        ShaderCompilerConfig cfg;
        auto r = compiler->compileAndValidate(kFrag, ShaderStage::Fragment, cfg);
        CHECK(r.success, "fragment success");
        CHECK(!r.spirv.empty(), "fragment SPIR-V not empty");
        CHECK(r.sourceSize > 0, "sourceSize set");
        CHECK(r.spirvSize > 0, "spirvSize set");
        CHECK(r.stageMask != 0, "stageMask detected");
    }

    // 5. Compile compute.
    std::printf("[shader-compiler] test compile compute\n");
    {
        ShaderCompilerConfig cfg;
        auto r = compiler->compileAndValidate(kComp, ShaderStage::Compute, cfg);
        CHECK(r.success, "compute success");
    }

    // 6. Invalid GLSL fails.
    std::printf("[shader-compiler] test invalid GLSL\n");
    {
        ShaderCompilerConfig cfg;
        auto spirv = compiler->compile("not valid GLSL!!!", ShaderStage::Vertex, cfg, err);
        CHECK(spirv.empty(), "invalid GLSL -> empty");
        CHECK(!err.empty(), "invalid GLSL -> error");
    }

    // 7. Determinism.
    std::printf("[shader-compiler] test determinism\n");
    {
        ShaderCompilerConfig cfg;
        auto a = compiler->compile(kVert, ShaderStage::Vertex, cfg, err);
        auto b = compiler->compile(kVert, ShaderStage::Vertex, cfg, err);
        CHECK(a == b, "same input -> same SPIR-V");
    }

    // 8. Optimize.
    std::printf("[shader-compiler] test optimize\n");
    {
        ShaderCompilerConfig cfg;
        auto spirv = compiler->compile(kVert, ShaderStage::Vertex, cfg, err);
        CHECK(!spirv.empty(), "compile for optimize");
        if (!spirv.empty()) {
            auto opt = compiler->optimize(spirv.data(), spirv.size(), 0, err);
            CHECK(!opt.empty(), "optimized not empty");
            if (!opt.empty()) CHECK(opt[0] == 0x07230203, "optimized magic");
        }
    }

    // 9. Available passes.
    std::printf("[shader-compiler] test available passes\n");
    {
        auto passes = compiler->availablePasses();
        CHECK(passes.size() > 0, "has passes");
    }

    std::printf("\n[shader-compiler] Results: %d passed, %d failed\n", g_passed, g_failed);
    if (g_failed > 0) { std::printf("[shader-compiler] FAILED\n"); return 1; }
    std::printf("[shader-compiler] ALL PASSED\n");
    return 0;
}
