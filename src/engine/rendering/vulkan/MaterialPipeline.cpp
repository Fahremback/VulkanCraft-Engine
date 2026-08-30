#include "engine/rendering/vulkan/MaterialPipeline.hpp"

#include "engine/rendering/IShaderCompiler.hpp"

#include <array>
#include <cstring>
#include <fstream>
#include <sstream>

namespace Engine::Rendering {

// ─── GLSL generation ───
namespace {

std::string_view glsl_type(MaterialValueType type) {
    switch (type) {
        case MaterialValueType::Bool: return "bool";
        case MaterialValueType::Float: return "float";
        case MaterialValueType::Vec2: return "vec2";
        case MaterialValueType::Vec3: return "vec3";
        case MaterialValueType::Vec4: return "vec4";
        case MaterialValueType::Texture2D: return "sampler2D";
    }
    return "float";
}

std::string glsl_literal(const MaterialValue& value) {
    if (std::holds_alternative<bool>(value))
        return std::get<bool>(value) ? "true" : "false";
    if (std::holds_alternative<float>(value))
        return std::to_string(std::get<float>(value)) + "f";
    if (std::holds_alternative<glm::vec2>(value)) {
        const glm::vec2 v = std::get<glm::vec2>(value);
        return "vec2(" + std::to_string(v.x) + "f, " + std::to_string(v.y) + "f)";
    }
    if (std::holds_alternative<glm::vec3>(value)) {
        const glm::vec3 v = std::get<glm::vec3>(value);
        return "vec3(" + std::to_string(v.x) + "f, " + std::to_string(v.y) + "f, " + std::to_string(v.z) + "f)";
    }
    if (std::holds_alternative<glm::vec4>(value)) {
        const glm::vec4 v = std::get<glm::vec4>(value);
        return "vec4(" + std::to_string(v.x) + "f, " + std::to_string(v.y) + "f, " +
               std::to_string(v.z) + "f, " + std::to_string(v.w) + "f)";
    }
    return "vec4(1.0f)";
}

// Map output semantic names to known material outputs.
struct OutputBinding {
    std::string semantic;
    MaterialValueType type{MaterialValueType::Vec4};
};

const std::vector<OutputBinding>& output_bindings() {
    static const std::vector<OutputBinding> bindings = {
        {"BaseColor", MaterialValueType::Vec4},
        {"Roughness", MaterialValueType::Float},
        {"Metallic", MaterialValueType::Float},
        {"Emissive", MaterialValueType::Vec4},
        {"Normal", MaterialValueType::Vec3},
        {"Opacity", MaterialValueType::Float},
    };
    return bindings;
}

} // namespace

GlslGenerationResult material_graph_to_glsl(const MaterialGraph& graph) {
    GlslGenerationResult result;
    const MaterialCompileResult compiled = graph.compile();
    if (!compiled) {
        result.errors = compiled.errors;
        return result;
    }

    std::ostringstream out;
    out << "#version 450\n";
    out << "layout(location = 0) in vec2 vUv;\n";
    out << "layout(location = 1) in vec3 vWorldPos;\n";
    out << "layout(location = 2) in vec3 vNormal;\n";
    out << "layout(location = 0) out vec4 outColor;\n\n";
    out << "layout(binding = 0) uniform MaterialParams {\n";

    // Collect exposed parameters as UBO members.
    const std::vector<MaterialParameter> params = graph.parameters();
    for (const MaterialParameter& p : params) {
        if (!p.exposed) continue;
        out << "    " << glsl_type(p.type) << " " << p.name << ";\n";
        result.uniformNames.push_back(p.name);
        result.uniformTypes.push_back(p.type);
    }
    if (result.uniformNames.empty()) {
        // glslc rejects empty uniform blocks; keep a harmless member.
        out << "    float _placeholder;\n";
    }
    out << "} params;\n\n";

    // Texture samplers (one per TextureSample node, bindings 1..N).
    std::vector<const MaterialNode*> textureNodes;
    std::unordered_map<MaterialNodeId, std::string> samplerNames;
    for (const auto& node : graph.nodes()) {
        if (node.kind == MaterialNodeKind::TextureSample) textureNodes.push_back(&node);
    }
    for (size_t i = 0; i < textureNodes.size(); ++i) {
        const std::string name = "tex" + std::to_string(i);
        samplerNames[textureNodes[i]->id] = name;
        out << "layout(binding = " << (i + 1) << ") uniform sampler2D " << name << ";\n";
    }

    // LightParams UBO (std140): directional sun + shadow + point lights,
    // declared after the samplers so the binding never collides with them.
    // Matches LightUboData in MaterialPipeline.hpp — keep both layouts in sync.
    result.lightUboBinding = static_cast<uint32_t>(textureNodes.size()) + 1;
    result.shadowSamplerBinding = result.lightUboBinding + 1;
    out << "layout(binding = " << result.lightUboBinding << ") uniform LightParams {\n";
    out << "    vec4 cameraPosition;\n";
    out << "    vec4 sunDirection;\n";
    out << "    vec4 sunColor;\n";
    out << "    mat4 sunViewProj;\n";
    out << "    vec4 shadowParams;\n";
    out << "    vec4 pointLightPos[" << kMaxPointLights << "];\n";
    out << "    vec4 pointLightColor[" << kMaxPointLights << "];\n";
    out << "    vec4 spotLightPos[" << kMaxSpotLights << "];\n";
    out << "    vec4 spotLightDir[" << kMaxSpotLights << "];\n";
    out << "    vec4 spotLightParams[" << kMaxSpotLights << "];\n";
    out << "    vec4 spotLightColor[" << kMaxSpotLights << "];\n";
    out << "    vec4 areaLightPos[" << kMaxAreaLights << "];\n";
    out << "    vec4 areaLightNormal[" << kMaxAreaLights << "];\n";
    out << "    vec4 areaLightHalf[" << kMaxAreaLights << "];\n";
    out << "    vec4 areaLightColor[" << kMaxAreaLights << "];\n";
    out << "    mat4 sunCascadeVP[" << kShadowCascadeCount << "];\n";
    out << "    vec4 sunCascadeSplits;\n";
    out << "    vec4 cameraForward;\n";
    out << "} lights;\n\n";
    out << "layout(binding = " << result.shadowSamplerBinding
        << ") uniform sampler2D shadowMap;\n\n";

    // Shadow sampling with optional shadow cascades: when shadowParams.z > 1
    // the sun shadow map is a 2x2 atlas (one 1024^2 tile per cascade), the
    // cascade is picked by view-space depth against sunCascadeSplits and each
    // cascade projects with its own sunCascadeVP; otherwise the legacy
    // single-map path (sunViewProj) is used (editor dummy shadow, etc).
    out << "\nfloat computeShadow(vec3 worldPos) {\n";
    out << "    if (lights.shadowParams.x <= 0.5) return 1.0;\n";
    out << "    int c = 0;\n";
    out << "    vec4 sc;\n";
    out << "    if (lights.shadowParams.z > 1.5) {\n";
    out << "        float viewDepth = dot(worldPos - lights.cameraPosition.xyz, lights.cameraForward.xyz);\n";
    out << "        c = " << (kShadowCascadeCount - 1) << ";\n";
    out << "        if (viewDepth < lights.sunCascadeSplits.x) c = 0;\n";
    out << "        else if (viewDepth < lights.sunCascadeSplits.y) c = 1;\n";
    out << "        else if (viewDepth < lights.sunCascadeSplits.z) c = 2;\n";
    out << "        sc = lights.sunCascadeVP[c] * vec4(worldPos, 1.0);\n";
    out << "    } else {\n";
    out << "        sc = lights.sunViewProj * vec4(worldPos, 1.0);\n";
    out << "    }\n";
    out << "    sc.xyz /= sc.w;\n";
    out << "    vec2 suv = sc.xy * 0.5 + 0.5;\n";
    out << "    if (lights.shadowParams.z > 1.5) {\n";
    out << "        vec2 tileOff = vec2(float(c % 2), float(c / 2)) * 0.5;\n";
    out << "        suv = suv * 0.5 + tileOff;\n";
    out << "    }\n";
    out << "    if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0 || sc.z < 0.0 || sc.z > 1.0) return 1.0;\n";
    out << "    float d = texture(shadowMap, suv).r;\n";
    out << "    return (d < sc.z - lights.shadowParams.y) ? 0.0 : 1.0;\n";
    out << "}\n\n";
    out << "void main() {\n";
    out << "    vec3 baseColor = vec3(0.8, 0.8, 0.8);\n";
    out << "    float roughness = 0.5;\n";
    out << "    float metallic = 0.0;\n";
    out << "    vec3 emissive = vec3(0.0);\n";
    out << "    float opacity = 1.0;\n";
    // The Normal output defaults to the interpolated world normal, so graphs
    // that never drive it render exactly as before; a graph that connects a
    // texture/expression to the Normal output overrides it (world-space
    // normal override — the pipeline has no tangents for TBN).
    out << "    vec3 normal = normalize(vNormal);\n\n";

    // Emit per-instruction GLSL from the IR. IR operands reference registers
    // (MaterialIRInstruction::result), not instruction indices — StoreOutput
    // produces no register, so the two numbering schemes diverge. Map registers
    // to their assigned variable names.
    std::unordered_map<uint32_t, std::string> regNames;
    std::unordered_map<uint32_t, MaterialValueType> regTypes;
    for (size_t i = 0; i < compiled.ir.instructions.size(); ++i) {
        const MaterialIRInstruction& ins = compiled.ir.instructions[i];
        std::string var = "t" + std::to_string(ins.result);
        if (ins.result != 0) {
            regNames[ins.result] = var;
            regTypes[ins.result] = ins.type;
        }
        auto reg = [&](size_t index) -> const std::string& {
            const auto it = regNames.find(ins.operands[index]);
            return it != regNames.end() ? it->second : var;
        };
        switch (ins.op) {
            case MaterialIROp::Constant:
                out << "    " << glsl_type(ins.type) << " " << var << " = " << glsl_literal(ins.literal) << ";\n";
                break;
            case MaterialIROp::LoadParameter: {
                const std::string pname = ins.symbol.empty() ? std::string("param") : ins.symbol;
                out << "    " << glsl_type(ins.type) << " " << var << " = params." << pname << ";\n";
                break;
            }
            case MaterialIROp::Add:
                out << "    " << glsl_type(ins.type) << " " << var << " = " << reg(0)
                    << " + " << reg(1) << ";\n";
                break;
            case MaterialIROp::Multiply:
                out << "    " << glsl_type(ins.type) << " " << var << " = " << reg(0)
                    << " * " << reg(1) << ";\n";
                break;
            case MaterialIROp::Lerp:
                out << "    " << glsl_type(ins.type) << " " << var << " = mix(" << reg(0)
                    << ", " << reg(1) << ", " << reg(2) << ");\n";
                break;
            case MaterialIROp::TextureSample: {
                // Sampler selection by originating node id (IR operands hold
                // register numbers, which cannot identify the texture node).
                const auto it = samplerNames.find(ins.nodeId);
                const std::string sampler = (it != samplerNames.end()) ? it->second : "tex0";
                out << "    vec4 " << var << " = texture(" << sampler << ", vUv);\n";
                break;
            }
            case MaterialIROp::StoreOutput: {
                const std::string src = reg(0);
                if (ins.symbol == "BaseColor") out << "    baseColor = " << src << ".rgb;\n";
                else if (ins.symbol == "Roughness" || ins.symbol == "Metallic") {
                    // Float sinks fed directly from a texture sample (vec4)
                    // take a single channel (.r) — mirrors the Opacity .a
                    // handling; a float expression stays as-is.
                    const auto tit = regTypes.find(ins.operands[0]);
                    const bool fromTex = tit != regTypes.end() &&
                                         tit->second == MaterialValueType::Vec4;
                    out << "    " << (ins.symbol == "Roughness" ? "roughness" : "metallic")
                        << " = " << src << (fromTex ? ".r" : "") << ";\n";
                }
                else if (ins.symbol == "Emissive") out << "    emissive = " << src << ".rgb;\n";
                else if (ins.symbol == "Opacity") {
                    // Opacity fed directly from a texture sample is a vec4:
                    // take its alpha channel (skin/leaf cutout).
                    const auto tit = regTypes.find(ins.operands[0]);
                    const bool fromTex = tit != regTypes.end() &&
                                         tit->second == MaterialValueType::Vec4;
                    out << "    opacity = " << src << (fromTex ? ".a" : "") << ";\n";
                }
                else if (ins.symbol == "Normal") {
                    // .rgb swizzle mirrors BaseColor: valid for both Vec4
                    // texture samples and Vec3 expressions driving the output.
                    out << "    normal = normalize(" << src << ".rgb);\n";
                }
                break;
            }
        }
    }

    // Alpha cutout: fully transparent texels (skins, leaves, glass) are
    // discarded instead of rendering as opaque white/black. Default opacity
    // is 1.0, so materials that never drive Opacity are unaffected.
    out << "    if (opacity < 0.05) discard;\n";

    // Real lighting: directional sun + point lights from LightComponents,
    // written into the LightParams UBO by the caller every frame. The graph's
    // Normal output (when driven) replaces the interpolated world normal.
    out << "\n    vec3 n = normal;\n";
    out << "    vec3 lightAccum = vec3(0.0);\n";
    out << "    if (lights.sunDirection.w > 0.5) {\n";
    out << "        float ndl = max(dot(n, -lights.sunDirection.xyz), 0.0);\n";
    out << "        lightAccum += ndl * lights.sunColor.rgb * computeShadow(vWorldPos);\n";
    out << "    }\n";
    out << "    for (int i = 0; i < " << kMaxPointLights << "; ++i) {\n";
    out << "        if (lights.pointLightColor[i].w <= 0.5) continue;\n";
    out << "        vec3 toLight = lights.pointLightPos[i].xyz - vWorldPos;\n";
    out << "        float dist = length(toLight);\n";
    out << "        float range = max(lights.pointLightPos[i].w, 0.01);\n";
    out << "        float att = clamp(1.0 - dist / range, 0.0, 1.0);\n";
    out << "        att *= att;\n";
    out << "        float ndl = max(dot(n, toLight / max(dist, 0.0001)), 0.0);\n";
    out << "        lightAccum += ndl * att * lights.pointLightColor[i].rgb;\n";
    out << "    }\n";
    out << "    for (int i = 0; i < " << kMaxSpotLights << "; ++i) {\n";
    out << "        if (lights.spotLightDir[i].w <= 0.5) continue;\n";
    out << "        vec3 toLight = lights.spotLightPos[i].xyz - vWorldPos;\n";
    out << "        float dist = length(toLight);\n";
    out << "        float range = max(lights.spotLightPos[i].w, 0.01);\n";
    out << "        float att = clamp(1.0 - dist / range, 0.0, 1.0);\n";
    out << "        att *= att;\n";
    out << "        vec3 L = toLight / max(dist, 0.0001);\n";
    out << "        float spot = smoothstep(lights.spotLightParams[i].y, lights.spotLightParams[i].x, dot(-L, lights.spotLightDir[i].xyz));\n";
    out << "        float ndl = max(dot(n, L), 0.0);\n";
    out << "        lightAccum += ndl * att * spot * lights.spotLightColor[i].rgb;\n";
    out << "    }\n";
    out << "    for (int i = 0; i < " << kMaxAreaLights << "; ++i) {\n";
    out << "        if (lights.areaLightPos[i].w <= 0.5) continue;\n";
    out << "        vec3 toLight = lights.areaLightPos[i].xyz - vWorldPos;\n";
    out << "        float dist = max(length(toLight), 0.0001);\n";
    out << "        float reach = max(lights.areaLightHalf[i].x + lights.areaLightHalf[i].y, 0.01);\n";
    out << "        float att = clamp(1.0 - dist / reach, 0.0, 1.0);\n";
    out << "        att *= att;\n";
    out << "        vec3 L = toLight / dist;\n";
    out << "        float facing = max(dot(lights.areaLightNormal[i].xyz, -L), 0.0);\n";
    out << "        float ndl = max(dot(n, L), 0.0);\n";
    out << "        lightAccum += ndl * att * facing * lights.areaLightColor[i].rgb;\n";
    out << "    }\n";
    // C.40: shared PBR lighting model. roughness/metallic (driven by the
    // graph's Roughness/Metallic outputs or PBR params) were previously dead
    // in a pure-Lambert out = baseColor*(0.22+0.78*lightAccum). Now a real
    // Cook-Torrance specular (GGX D, Smith G, Schlick F) consumes them for the
    // sun (the dominant real light), and the diffuse term is weighted by
    // (1 - metallic): the split the MeshAsset PBR params already encode. This
    // keeps mesh/personagem/primitive materials on a single shared model.
    // Defaults (roughness .5, metallic 0) add a subtle dielectric specular and
    // keep the ambient floor + approximate brightness of the prior Lambert path.
    out << "    vec3 V = normalize(lights.cameraPosition.xyz - vWorldPos);\n";
    out << "    vec3 H = normalize(-lights.sunDirection.xyz + V);\n";
    out << "    float NdotH = max(dot(n, H), 0.0);\n";
    out << "    float NdotV = max(dot(n, V), 0.0001);\n";
    out << "    float NdotLsun = max(dot(n, -lights.sunDirection.xyz), 0.0);\n";
    out << "    float rPbr = clamp(roughness, 0.04, 1.0);\n";
    out << "    float rPbr2 = rPbr * rPbr;\n";
    out << "    float D = rPbr2 / (3.14159 * pow(max(NdotH * NdotH * (rPbr2 - 1.0) + 1.0, 1e-4), 2.0));\n";
    out << "    float k = (rPbr + 1.0) * (rPbr + 1.0) / 8.0;\n";
    out << "    float G = (NdotLsun / (NdotLsun * (1.0 - k) + k)) * (NdotV / (NdotV * (1.0 - k) + k));\n";
    out << "    vec3 F0 = mix(vec3(0.04), baseColor, metallic);\n";
    out << "    float VdotH = max(dot(V, H), 0.0);\n";
    out << "    vec3 F = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);\n";
    out << "    // Specular is gated on the sun being enabled (same flag as the\n";
    out << "    // Lambert block above), so a disabled sun adds no specular.\n";
    out << "    vec3 spec = vec3(0.0);\n";
    out << "    if (lights.sunDirection.w > 0.5)\n";
    out << "        spec = (D * G * F) / (4.0 * NdotV * NdotLsun + 1e-4) * lights.sunColor.rgb * computeShadow(vWorldPos);\n";
    out << "    vec3 diff = baseColor * (1.0 - metallic) * (0.78 * lightAccum);\n";
    out << "    vec3 lit = baseColor * 0.22 + diff + spec;\n";
    out << "    outColor = vec4(lit + emissive, opacity);\n";
    out << "}\n";
    result.source = out.str();
    return result;
}

MaterialGraph material_graph_from_pbr(const MaterialAsset& material) {
    MaterialGraph graph;
    graph.define_parameter({ "Albedo", MaterialValueType::Vec3, material.albedo, true });
    graph.define_parameter({ "Roughness", MaterialValueType::Float, material.roughness, true });
    graph.define_parameter({ "Metallic", MaterialValueType::Float, material.metallic, true });
    graph.define_parameter({ "Emissive", MaterialValueType::Vec3,
                             material.emissiveColor * material.emissiveIntensity, true });
    const auto albedo = graph.add_parameter("Albedo");
    const auto roughness = graph.add_parameter("Roughness");
    const auto metallic = graph.add_parameter("Metallic");
    const auto emissive = graph.add_parameter("Emissive");
    const auto baseOut = graph.add_output("BaseColor", MaterialValueType::Vec3);
    const auto roughOut = graph.add_output("Roughness", MaterialValueType::Float);
    const auto metalOut = graph.add_output("Metallic", MaterialValueType::Float);
    const auto emisOut = graph.add_output("Emissive", MaterialValueType::Vec3);
    (void)graph.connect(albedo, baseOut, 0);
    (void)graph.connect(roughness, roughOut, 0);
    (void)graph.connect(metallic, metalOut, 0);
    (void)graph.connect(emissive, emisOut, 0);
    return graph;
}

std::vector<uint32_t> compile_glsl_to_spirv(const std::string& source, VkShaderStageFlagBits stage) {
    // The slang contract (C.15 IShaderCompiler) is the PRODUCT's shader
    // compilation path: every material-graph shader the engine compiles at
    // runtime goes through this public core (glslc + spirv-val), instead of a
    // private system() call. Falls back to the legacy temp-file glslc path if
    // the toolchain is unavailable, so the game still boots on minimal setups.
    {
        std::string compilerError;
        std::unique_ptr<vc::rendering::IShaderCompiler> compiler =
            vc::rendering::create_shader_compiler(compilerError);
        if (compiler) {
            vc::rendering::ShaderCompilerConfig config;
            std::string compileError;
            vc::rendering::ShaderStage shaderStage =
                (stage == VK_SHADER_STAGE_VERTEX_BIT)
                    ? vc::rendering::ShaderStage::Vertex
                    : vc::rendering::ShaderStage::Fragment;
            std::vector<uint32_t> spirv = compiler->compile(
                source.c_str(), shaderStage, config, compileError);
            if (!spirv.empty()) return spirv;
        }
    }
    // Legacy fallback: write the source to a temp file and invoke glslc.
    const std::filesystem::path tmp = std::filesystem::temp_directory_path() / "vc_material_tmp";
    const std::string stageArg = (stage == VK_SHADER_STAGE_VERTEX_BIT) ? "vert" : "frag";
    const std::filesystem::path srcFile = std::filesystem::path(tmp.string() + "." + stageArg);
    const std::filesystem::path spvFile = std::filesystem::path(tmp.string() + ".spv");
    {
        std::ofstream out(srcFile, std::ios::binary);
        out << source;
    }
    const std::string cmd = "glslc \"" + srcFile.string() + "\" -fshader-stage=" + stageArg +
                            " -o \"" + spvFile.string() + "\" 2>nul";
    const int rc = std::system(cmd.c_str());
    std::vector<uint32_t> spirv;
    if (rc == 0) {
        std::ifstream in(spvFile, std::ios::binary);
        if (in) {
            in.seekg(0, std::ios::end);
            const std::streamsize size = in.tellg();
            in.seekg(0, std::ios::beg);
            if (size > 0 && size % 4 == 0) {
                spirv.resize(static_cast<size_t>(size) / 4);
                in.read(reinterpret_cast<char*>(spirv.data()), size);
            }
        }
    }
    std::error_code ec;
    std::filesystem::remove(srcFile, ec);
    std::filesystem::remove(spvFile, ec);
    return spirv;
}

// ─── VulkanRenderGraphExecutor ───
bool VulkanRenderGraphExecutor::initialize(VkDevice device, const RenderGraph& graph, std::string* error) {
    if (device == VK_NULL_HANDLE) {
        lastError_ = "initialize requires a valid VkDevice";
        if (error) *error = lastError_;
        return false;
    }
    device_ = device;
    passFrames_.clear();
    compileResult_ = graph.compile();
    if (!compileResult_) {
        lastError_ = "render graph compile failed: " +
                     (compileResult_.errors.empty() ? "unknown" : compileResult_.errors[0]);
        if (error) *error = lastError_;
        return false;
    }
    initialized_ = true;
    return true;
}

void VulkanRenderGraphExecutor::shutdown() {
    initialized_ = false;
    compileResult_ = {};
    passFrames_.clear();
}

void VulkanRenderGraphExecutor::register_pass(RenderPassId pass, PassFrame frame) {
    if (frame.renderPass == VK_NULL_HANDLE) return;
    passFrames_[pass] = std::move(frame);
}

void VulkanRenderGraphExecutor::unregister_pass(RenderPassId pass) {
    passFrames_.erase(pass);
}

void VulkanRenderGraphExecutor::record(VkCommandBuffer commandBuffer, uint32_t swapImageIndex,
                                       VkExtent2D extent) const {
    if (!initialized_) return;
    executedPassCount_ = compileResult_.order.size();
    totalBarriers_ = 0;

    // Group the compiled barriers by the pass they must precede.
    std::unordered_map<RenderPassId, std::vector<const RenderBarrier*>> incoming;
    for (const RenderBarrier& barrier : compileResult_.barriers)
        incoming[barrier.destinationPass].push_back(&barrier);

    for (const RenderPassId passId : compileResult_.order) {
        // Emit the barriers targeting this pass before it begins: previous
        // pass writes must be visible before this pass reads/writes. Layout
        // transitions are handled by the render passes themselves (each
        // attachment declares initial/final layout), so a memory + execution
        // dependency is the correct barrier here.
        const auto barrierIt = incoming.find(passId);
        if (barrierIt != incoming.end() && !barrierIt->second.empty()) {
            VkMemoryBarrier memory{};
            memory.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            memory.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            memory.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
                                 VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 1, &memory,
                                 0, nullptr, 0, nullptr);
            totalBarriers_ += barrierIt->second.size();
        }

        const auto frameIt = passFrames_.find(passId);
        if (frameIt == passFrames_.end()) continue; // pass not registered: skip
        const PassFrame& frame = frameIt->second;
        const VkFramebuffer framebuffer = swapImageIndex < frame.framebuffers.size()
            ? frame.framebuffers[swapImageIndex]
            : (frame.framebuffers.empty() ? VK_NULL_HANDLE : frame.framebuffers.front());
        if (framebuffer == VK_NULL_HANDLE) continue;

        VkRenderPassBeginInfo rpBegin{};
        rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpBegin.renderPass = frame.renderPass;
        rpBegin.framebuffer = framebuffer;
        rpBegin.renderArea.offset = { 0, 0 };
        rpBegin.renderArea.extent = extent;
        rpBegin.clearValueCount = static_cast<uint32_t>(frame.clearValues.size());
        rpBegin.pClearValues = frame.clearValues.empty() ? nullptr : frame.clearValues.data();
        vkCmdBeginRenderPass(commandBuffer, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
        if (frame.draw) frame.draw(commandBuffer);
        vkCmdEndRenderPass(commandBuffer);
    }
}

// ─── FileWatcher ───
FileWatcher::FileWatcher(std::chrono::milliseconds interval) : interval_(interval) {}

void FileWatcher::watch(const std::filesystem::path& path) {
    std::error_code ec;
    const auto modified = std::filesystem::last_write_time(path, ec);
    const auto size = ec ? 0 : std::filesystem::file_size(path, ec);
    stamps_[path] = FileStamp{path, ec ? 0 : size, modified};
}

void FileWatcher::unwatch(const std::filesystem::path& path) {
    stamps_.erase(path);
}

std::vector<std::filesystem::path> FileWatcher::poll() {
    std::vector<std::filesystem::path> changed;
    for (auto it = stamps_.begin(); it != stamps_.end();) {
        std::error_code ec;
        const auto modified = std::filesystem::last_write_time(it->first, ec);
        if (ec) {
            changed.push_back(it->first);
            it = stamps_.erase(it);
            continue;
        }
        const auto size = std::filesystem::file_size(it->first, ec);
        if (size != it->second.size || modified != it->second.modified) {
            it->second.size = size;
            it->second.modified = modified;
            changed.push_back(it->first);
        }
        ++it;
    }
    return changed;
}

} // namespace Engine::Rendering
