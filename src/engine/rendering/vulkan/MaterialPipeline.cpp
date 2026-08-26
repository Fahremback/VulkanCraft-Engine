#include "engine/rendering/vulkan/MaterialPipeline.hpp"

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
    out << "    vec3 normal = vec3(0.0, 0.0, 1.0);\n\n";

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
                else if (ins.symbol == "Roughness") out << "    roughness = " << src << ";\n";
                else if (ins.symbol == "Metallic") out << "    metallic = " << src << ";\n";
                else if (ins.symbol == "Emissive") out << "    emissive = " << src << ".rgb;\n";
                else if (ins.symbol == "Opacity") {
                    // Opacity fed directly from a texture sample is a vec4:
                    // take its alpha channel (skin/leaf cutout).
                    const auto tit = regTypes.find(ins.operands[0]);
                    const bool fromTex = tit != regTypes.end() &&
                                         tit->second == MaterialValueType::Vec4;
                    out << "    opacity = " << src << (fromTex ? ".a" : "") << ";\n";
                }
                else if (ins.symbol == "Normal") out << "    normal = normalize(" << src << ");\n";
                break;
            }
        }
    }

    // Alpha cutout: fully transparent texels (skins, leaves, glass) are
    // discarded instead of rendering as opaque white/black. Default opacity
    // is 1.0, so materials that never drive Opacity are unaffected.
    out << "    if (opacity < 0.05) discard;\n";

    // Real lighting: directional sun + point lights from LightComponents,
    // written into the LightParams UBO by the caller every frame.
    out << "\n    vec3 n = normalize(vNormal);\n";
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
    out << "    vec3 lit = baseColor * (0.22 + 0.78 * lightAccum);\n";
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

// ─── VulkanMaterialPipeline ───
namespace {

VkShaderModule create_module(VkDevice device, const std::vector<uint32_t>& spirv) {
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = spirv.size() * sizeof(uint32_t);
    info.pCode = spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &info, nullptr, &module);
    return module;
}

} // namespace

std::vector<uint32_t> compile_glsl_to_spirv(const std::string& source, VkShaderStageFlagBits stage) {
    // Write the source to a temp file and invoke glslc.
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

VulkanMaterialPipeline::~VulkanMaterialPipeline() {
    destroy();
}

bool VulkanMaterialPipeline::create(VkDevice device, VkFormat colorFormat, VkFormat depthFormat,
                                    const MaterialGraph& graph, std::string* error) {
    destroy();
    device_ = device;
    colorFormat_ = colorFormat;
    depthFormat_ = depthFormat;

    const GlslGenerationResult gen = material_graph_to_glsl(graph);
    if (!gen) {
        lastError_ = "material graph GLSL generation failed";
        if (error) *error = lastError_;
        return false;
    }
    glsl_ = gen.source;
    uniformNames_ = gen.uniformNames;
    uniformTypes_ = gen.uniformTypes;
    sourceHash_ = std::hash<std::string>{}(glsl_);

    // Vertex shader (fixed: fullscreen triangle with UVs).
    const std::string vertSrc = R"(
#version 450
layout(location = 0) out vec2 vUv;
void main() {
    vec2 pos[3] = vec2[3](vec2(-1,-1), vec2(3,-1), vec2(-1,3));
    vUv = pos[gl_VertexIndex] * 0.5 + 0.5;
    gl_Position = vec4(pos[gl_VertexIndex], 0.0, 1.0);
}
)";
    std::vector<uint32_t> vertSpv = compile_glsl_to_spirv(vertSrc, VK_SHADER_STAGE_VERTEX_BIT);
    std::vector<uint32_t> fragSpv = compile_glsl_to_spirv(glsl_, VK_SHADER_STAGE_FRAGMENT_BIT);
    if (vertSpv.empty() || fragSpv.empty()) {
        lastError_ = "glslc compile failed (is glslc on PATH?)";
        if (error) *error = lastError_;
        return false;
    }
    vertModule_ = create_module(device_, vertSpv);
    fragModule_ = create_module(device_, fragSpv);
    if (vertModule_ == VK_NULL_HANDLE || fragModule_ == VK_NULL_HANDLE) {
        lastError_ = "VkShaderModule creation failed";
        if (error) *error = lastError_;
        return false;
    }

    // Descriptor set layout: binding 0 = material params UBO.
    VkDescriptorSetLayoutBinding uboBinding{};
    uboBinding.binding = 0;
    uboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboBinding.descriptorCount = 1;
    uboBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dslInfo{};
    dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 1;
    dslInfo.pBindings = &uboBinding;
    if (vkCreateDescriptorSetLayout(device_, &dslInfo, nullptr, &descriptorSetLayout_) != VK_SUCCESS) {
        lastError_ = "descriptor set layout creation failed";
        if (error) *error = lastError_;
        return false;
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = 16;

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &descriptorSetLayout_;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &layout_) != VK_SUCCESS) {
        lastError_ = "pipeline layout creation failed";
        if (error) *error = lastError_;
        return false;
    }

    // Shaders + layout ready; the graphics pipeline itself requires the real
    // render pass and is created by build_pipeline().
    lastError_.clear();
    return true;
}

bool VulkanMaterialPipeline::build_pipeline(VkRenderPass renderPass, uint32_t subpass) {
    if (renderPass == VK_NULL_HANDLE) {
        lastError_ = "build_pipeline requires a real VkRenderPass";
        return false;
    }
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule_;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 0xF;
    blend.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blend;

    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &assembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depth;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamic;
    pipelineInfo.layout = layout_;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = subpass;

    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_) != VK_SUCCESS) {
        lastError_ = "vkCreateGraphicsPipelines failed";
        return false;
    }
    lastError_.clear();
    return true;
}

void VulkanMaterialPipeline::destroy() {
    if (device_ == VK_NULL_HANDLE) return;
    if (pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, layout_, nullptr);
    if (descriptorSetLayout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
    if (vertModule_ != VK_NULL_HANDLE) vkDestroyShaderModule(device_, vertModule_, nullptr);
    if (fragModule_ != VK_NULL_HANDLE) vkDestroyShaderModule(device_, fragModule_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    descriptorSetLayout_ = VK_NULL_HANDLE;
    vertModule_ = VK_NULL_HANDLE;
    fragModule_ = VK_NULL_HANDLE;
}

void VulkanMaterialPipeline::write_parameters(std::byte* uboMemory, std::size_t capacity) const {
    std::size_t offset = 0;
    for (size_t i = 0; i < uniformNames_.size() && offset + 16 <= capacity; ++i) {
        // Defaults are written as zeroed floats; callers override with real values.
        std::memset(uboMemory + offset, 0, 16);
        offset += 16;
    }
}

bool VulkanMaterialPipeline::poll_reload() {
    const std::size_t currentHash = std::hash<std::string>{}(glsl_);
    if (currentHash != sourceHash_) {
        sourceHash_ = currentHash;
        return reload();
    }
    return false;
}

bool VulkanMaterialPipeline::reload() {
    // Recompile the fragment shader module from the stored GLSL source.
    ++buildId_;
    if (device_ == VK_NULL_HANDLE || glsl_.empty()) {
        lastError_ = "reload requires an existing pipeline with GLSL source";
        return false;
    }
    std::vector<uint32_t> fragSpv = compile_glsl_to_spirv(glsl_, VK_SHADER_STAGE_FRAGMENT_BIT);
    if (fragSpv.empty()) {
        lastError_ = "glslc recompile failed";
        return false;
    }
    VkShaderModule newModule = create_module(device_, fragSpv);
    if (newModule == VK_NULL_HANDLE) {
        lastError_ = "VkShaderModule rebuild failed";
        return false;
    }
    // Swap module (pipeline rebuild requires the caller's render pass; the
    // module swap is the hot-reload primitive available at this layer).
    if (fragModule_ != VK_NULL_HANDLE) vkDestroyShaderModule(device_, fragModule_, nullptr);
    fragModule_ = newModule;
    sourceHash_ = std::hash<std::string>{}(glsl_);
    lastError_.clear();
    return true;
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
