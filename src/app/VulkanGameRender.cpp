#include "VulkanGame.hpp"
#include "VulkanGameSupport.hpp"

glm::vec3 VulkanGame::cameraFront() const{
        const float yaw = glm::radians(camYaw);
        const float pitch = glm::radians(camPitch);
        return glm::normalize(glm::vec3(
            std::cos(yaw) * std::cos(pitch), std::sin(pitch), std::sin(yaw) * std::cos(pitch)));
    }

void VulkanGame::cursorPosCallback(GLFWwindow* window, double xpos, double ypos){
        auto app = reinterpret_cast<VulkanGame*>(glfwGetWindowUserPointer(window));
        if (app->cameraDragging || app->mouseLookEnabled) {
            const double dx = xpos - app->lastMouseX;
            const double dy = ypos - app->lastMouseY;
            app->camYaw += static_cast<float>(dx) * 0.15f;
            app->camPitch = glm::clamp(app->camPitch - static_cast<float>(dy) * 0.15f, -89.0f, 89.0f);
        }
        app->lastMouseX = xpos;
        app->lastMouseY = ypos;
    }

void VulkanGame::mouseButtonCallback(GLFWwindow* window, int button, int action, int /*mods*/){
        auto app = reinterpret_cast<VulkanGame*>(glfwGetWindowUserPointer(window));
        if (button == GLFW_MOUSE_BUTTON_RIGHT) {
            app->cameraDragging = action == GLFW_PRESS;
        } else if (button == GLFW_MOUSE_BUTTON_LEFT) {
            app->mouseLeftHeld = action == GLFW_PRESS;
        }
    }

void VulkanGame::framebufferResizeCallback(GLFWwindow* window, int width, int height){
        auto app = reinterpret_cast<VulkanGame*>(glfwGetWindowUserPointer(window));
        app->framebufferResized = true;
    }

void VulkanGame::initPipelines(){
        // Material-graph fragment shader: standard PBR graph → GLSL → SPIR-V
        // (same pipeline the editor viewport uses).
        MaterialAsset defaults;
        const MaterialGraph graph = material_graph_from_pbr(defaults);
        const GlslGenerationResult gen = material_graph_to_glsl(graph);
        if (!gen) throw std::runtime_error("material graph GLSL generation failed");
        const std::vector<uint32_t> fragSpv = compile_glsl_to_spirv(gen.source, VK_SHADER_STAGE_FRAGMENT_BIT);
        if (fragSpv.empty()) throw std::runtime_error("glslc failed to compile the material graph shader (is glslc on PATH?)");

        // Vertex shader: shared editor_material.vert (MVP push constant, vUv out).
        std::string vertPath = std::string(VULKANCRAFT_SHADER_DIR) + "/editor_material.vert.spv";
        std::ifstream probe(vertPath, std::ios::binary);
        if (!probe) vertPath = "Content/Shaders/editor_material.vert.spv";
        const VkShaderModule vertModule = createShaderModule(vertPath);
        const VkShaderModule fragModule = createModuleFromSpirv(fragSpv);

        // Descriptor set: binding 0 = material params UBO; the generated
        // shader's binding = LightParams UBO (lights from LightComponents);
        // then the shadow-map sampler.
        VkDescriptorSetLayoutBinding bindings[3]{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[1].binding = gen.lightUboBinding;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[2].binding = gen.shadowSamplerBinding;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo dslInfo{};
        dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslInfo.bindingCount = 3;
        dslInfo.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(device, &dslInfo, nullptr, &descriptorSetLayout) != VK_SUCCESS) {
            throw std::runtime_error("failed to create descriptor set layout");
        }

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pushRange.offset = 0;
        pushRange.size = sizeof(Rendering::MaterialPushConstants);
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &descriptorSetLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;
        if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
            throw std::runtime_error("failed to create pipeline layout");
        }

        // UBO: std140 layout of the PBR graph parameters (sorted by name):
        // Albedo vec3 @0, Emissive vec3 @16, Metallic float @32, Roughness @36 → 48 bytes.
        VkDeviceSize uboSize = 48;
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = uboSize;
        bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        if (vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &uboBuffer, &uboAllocation, nullptr) != VK_SUCCESS) {
            throw std::runtime_error("failed to create UBO");
        }

        VkDescriptorPoolSize poolSizes[2]{};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSizes[0].descriptorCount = 2;
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[1].descriptorCount = 1;
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = poolSizes;
        if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool) != VK_SUCCESS) {
            throw std::runtime_error("failed to create descriptor pool");
        }
        VkDescriptorSetAllocateInfo setAlloc{};
        setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setAlloc.descriptorPool = descriptorPool;
        setAlloc.descriptorSetCount = 1;
        setAlloc.pSetLayouts = &descriptorSetLayout;
        if (vkAllocateDescriptorSets(device, &setAlloc, &descriptorSet) != VK_SUCCESS) {
            throw std::runtime_error("failed to allocate descriptor set");
        }
        VkDescriptorBufferInfo descInfo{ uboBuffer, 0, uboSize };
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descriptorSet;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &descInfo;
        // LightParams UBO (second descriptor).
        VkBufferCreateInfo lightBufferInfo{};
        lightBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        lightBufferInfo.size = sizeof(Rendering::LightUboData);
        lightBufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        lightBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo lightAllocInfo{};
        lightAllocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        if (vmaCreateBuffer(allocator, &lightBufferInfo, &lightAllocInfo,
                            &lightBuffer, &lightAllocation, nullptr) != VK_SUCCESS) {
            throw std::runtime_error("failed to create light UBO");
        }
        VkDescriptorBufferInfo lightDescInfo{ lightBuffer, 0, sizeof(Rendering::LightUboData) };
        VkWriteDescriptorSet lightWrite{};
        lightWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        lightWrite.dstSet = descriptorSet;
        lightWrite.dstBinding = gen.lightUboBinding;
        lightWrite.descriptorCount = 1;
        lightWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        lightWrite.pBufferInfo = &lightDescInfo;
        VkDescriptorImageInfo shadowImageInfo{ shadowMapSampler, shadowMapView,
                                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet shadowWrite{};
        shadowWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        shadowWrite.dstSet = descriptorSet;
        shadowWrite.dstBinding = gen.shadowSamplerBinding;
        shadowWrite.descriptorCount = 1;
        shadowWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        shadowWrite.pImageInfo = &shadowImageInfo;
        VkWriteDescriptorSet writes[3] = { write, lightWrite, shadowWrite };
        vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);

        // Graphics pipeline.
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertModule;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragModule;
        stages[1].pName = "main";

        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(GameVertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription attrs[4]{};
        attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(GameVertex, pos)) };
        attrs[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(GameVertex, normal)) };
        attrs[2] = { 2, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(GameVertex, color)) };
        attrs[3] = { 3, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(GameVertex, uv)) };
        VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = 4;
        vertexInput.pVertexAttributeDescriptions = attrs;

        VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewportState{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depth{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp = VK_COMPARE_OP_LESS;

        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = 0xF;
        blend.blendEnable = VK_FALSE;
        VkPipelineColorBlendStateCreateInfo colorBlend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        colorBlend.attachmentCount = 1;
        colorBlend.pAttachments = &blend;

        VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamicStates;

        VkGraphicsPipelineCreateInfo pipelineInfo{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
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
        pipelineInfo.layout = pipelineLayout;
        pipelineInfo.renderPass = renderPass;
        pipelineInfo.subpass = 0;
        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &graphicsPipeline) != VK_SUCCESS) {
            throw std::runtime_error("failed to create graphics pipeline!");
        }
        vkDestroyShaderModule(device, vertModule, nullptr);
        vkDestroyShaderModule(device, fragModule, nullptr);

        buildCompositePipeline();
        buildShadowResources();
        buildVolumetricResources();
        buildSkinnedResources();
        buildRenderGraph();
    }

void VulkanGame::buildShadowResources(){
        // Depth-only render pass (final layout = shader-read for the Scene pass).
        VkAttachmentDescription depthAttachment{};
        depthAttachment.format = VK_FORMAT_D32_SFLOAT;
        depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        // One render pass for the whole cascade atlas: the CLEAR wipes all
        // four tiles at pass begin and each cascade renders into its own tile
        // (viewport/scissor + its light VP) inside drawShadowPass.
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depthAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentReference depthRef{ 0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.pDepthStencilAttachment = &depthRef;
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependency.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo rpInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rpInfo.attachmentCount = 1;
        rpInfo.pAttachments = &depthAttachment;
        rpInfo.subpassCount = 1;
        rpInfo.pSubpasses = &subpass;
        rpInfo.dependencyCount = 1;
        rpInfo.pDependencies = &dependency;
        if (vkCreateRenderPass(device, &rpInfo, nullptr, &shadowRenderPass) != VK_SUCCESS) {
            throw std::runtime_error("failed to create shadow render pass");
        }

        VkFramebufferCreateInfo fbInfo{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fbInfo.renderPass = shadowRenderPass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments = &shadowMapView;
        fbInfo.width = kShadowMapSize;
        fbInfo.height = kShadowMapSize;
        fbInfo.layers = 1;
        if (vkCreateFramebuffer(device, &fbInfo, nullptr, &shadowFramebuffer) != VK_SUCCESS) {
            throw std::runtime_error("failed to create shadow framebuffer");
        }

        // Depth-only vertex shader (same push constant as the material pass).
        const std::string vertSrc = R"(
#version 450
layout (location = 0) in vec3 inPosition;
layout (push_constant) uniform PushConstants {
    mat4 mvp;    // light view-projection * model
    mat4 model;
} push;
void main() {
    gl_Position = push.mvp * vec4(inPosition, 1.0);
}
)";
        const std::string fragSrc = "#version 450\nvoid main() {}\n";
        std::vector<uint32_t> vertSpv = Rendering::compile_glsl_to_spirv(vertSrc, VK_SHADER_STAGE_VERTEX_BIT);
        std::vector<uint32_t> fragSpv = Rendering::compile_glsl_to_spirv(fragSrc, VK_SHADER_STAGE_FRAGMENT_BIT);
        if (vertSpv.empty() || fragSpv.empty()) {
            throw std::runtime_error("glslc failed to compile the shadow shaders (is glslc on PATH?)");
        }
        const VkShaderModule vertModule = createModuleFromSpirv(vertSpv);
        const VkShaderModule fragModule = createModuleFromSpirv(fragSpv);

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pushRange.offset = 0;
        pushRange.size = sizeof(Rendering::MaterialPushConstants);
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;
        if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &shadowPipelineLayout) != VK_SUCCESS) {
            throw std::runtime_error("failed to create shadow pipeline layout");
        }

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertModule;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragModule;
        stages[1].pName = "main";

        // Depth-only shader consumes only position: declare a single attribute
        // (binding stride still matches GameVertex so the mesh buffers bind).
        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(GameVertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription attrs[1]{};
        attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(GameVertex, pos)) };
        VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = 1;
        vertexInput.pVertexAttributeDescriptions = attrs;
        VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewportState{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;
        raster.depthBiasEnable = VK_TRUE;
        raster.depthBiasConstantFactor = 1.5f;
        raster.depthBiasSlopeFactor = 1.5f;
        VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp = VK_COMPARE_OP_LESS;
        VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamicStates;

        VkGraphicsPipelineCreateInfo pipelineInfo{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = stages;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &assembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &raster;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pDepthStencilState = &depth;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = shadowPipelineLayout;
        pipelineInfo.renderPass = shadowRenderPass;
        pipelineInfo.subpass = 0;
        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &shadowPipeline) != VK_SUCCESS) {
            throw std::runtime_error("failed to create shadow pipeline!");
        }
        vkDestroyShaderModule(device, vertModule, nullptr);
        vkDestroyShaderModule(device, fragModule, nullptr);
    }

void VulkanGame::buildVolumetricResources(){
        const std::string vertSrc = R"(
#version 450
layout(location = 0) out vec2 vUv;
void main() {
    vec2 pos[3] = vec2[3](vec2(-1,-1), vec2(3,-1), vec2(-1,3));
    vUv = pos[gl_VertexIndex] * 0.5 + 0.5;
    gl_Position = vec4(pos[gl_VertexIndex], 0.0, 1.0);
}
)";
        const std::string fragSrc = R"(
#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
layout(binding = 0) uniform sampler2D sceneDepth;
layout(binding = 1) uniform sampler2D shadowMap;
layout(binding = 2) uniform VolumetricParams {
    vec4 cameraPosition;
    mat4 invViewProj;
    vec4 sunDirection;
    vec4 sunColor;
    mat4 sunViewProj;
    vec4 shadowParams;
    mat4 cameraViewProj;
    vec4 nearFar;
    mat4 sunCascadeVP[4];
    vec4 sunCascadeSplits;
    vec4 cameraForward;
} vol;
float sunShadowAt(vec3 worldPos) {
    if (vol.shadowParams.x <= 0.5) return 1.0;
    int c = 0;
    vec4 sc;
    if (vol.shadowParams.z > 1.5) {
        float viewDepth = dot(worldPos - vol.cameraPosition.xyz, vol.cameraForward.xyz);
        c = 3;
        if (viewDepth < vol.sunCascadeSplits.x) c = 0;
        else if (viewDepth < vol.sunCascadeSplits.y) c = 1;
        else if (viewDepth < vol.sunCascadeSplits.z) c = 2;
        sc = vol.sunCascadeVP[c] * vec4(worldPos, 1.0);
    } else {
        sc = vol.sunViewProj * vec4(worldPos, 1.0);
    }
    sc.xyz /= sc.w;
    vec2 suv = sc.xy * 0.5 + 0.5;
    if (vol.shadowParams.z > 1.5) {
        vec2 tileOff = vec2(float(c % 2), float(c / 2)) * 0.5;
        suv = suv * 0.5 + tileOff;
    }
    if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0 || sc.z < 0.0 || sc.z > 1.0) {
        return 1.0;
    }
    float d = texture(shadowMap, suv).r;
    return (d < sc.z - vol.shadowParams.y) ? 0.0 : 1.0;
}
void main() {
    float depth = texture(sceneDepth, vUv).r;
    vec4 ndc = vec4(vUv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 wp = vol.invViewProj * ndc;
    vec3 surfacePos = wp.xyz / wp.w;

    vec3 ray = surfacePos - vol.cameraPosition.xyz;
    float rayLen = length(ray);
    int steps = max(int(vol.nearFar.w), 2);
    vec3 stepVec = (rayLen > 1e-5) ? (ray / float(steps)) : vec3(0.0);
    float density = vol.nearFar.z;
    float sunActive = vol.sunDirection.w;
    float shadowActive = vol.shadowParams.x;
    float intensity = vol.sunColor.w;

    vec3 accum = vec3(0.0);
    float transmittance = 1.0;
    for (int i = 0; i < steps; ++i) {
        vec3 p = vol.cameraPosition.xyz + stepVec * (float(i) + 0.5);
        // Cut the shaft at occluders: if this sample is behind the scene
        // surface along its own screen direction, stop marching.
        vec4 sp = vol.cameraViewProj * vec4(p, 1.0);
        sp.xyz /= sp.w;
        vec2 suv = sp.xy * 0.5 + 0.5;
        if (suv.x >= 0.0 && suv.x <= 1.0 && suv.y >= 0.0 && suv.y <= 1.0) {
            float sd = texture(sceneDepth, suv).r;
            if (sp.z > sd * 2.0 - 1.0 + 0.0005) break;
        }
        float lit = 1.0;
        if (shadowActive > 0.5) lit = sunShadowAt(p);
        float scatter = transmittance * density * (rayLen / float(steps)) * lit;
        accum += vol.sunColor.rgb * (scatter * intensity * sunActive);
        transmittance *= exp(-density * (rayLen / float(steps)));
    }
    outColor = vec4(accum, 1.0);
}
)";
        std::vector<uint32_t> vertSpv = Rendering::compile_glsl_to_spirv(vertSrc, VK_SHADER_STAGE_VERTEX_BIT);
        std::vector<uint32_t> fragSpv = Rendering::compile_glsl_to_spirv(fragSrc, VK_SHADER_STAGE_FRAGMENT_BIT);
        if (vertSpv.empty() || fragSpv.empty()) {
            throw std::runtime_error("glslc failed to compile the volumetric shaders (is glslc on PATH?)");
        }
        const VkShaderModule vertModule = createModuleFromSpirv(vertSpv);
        const VkShaderModule fragModule = createModuleFromSpirv(fragSpv);

        // UBO buffer for the per-frame volumetric params.
        VkBufferCreateInfo uboInfo{};
        uboInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        uboInfo.size = sizeof(VolumetricParams);
        uboInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        uboInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo uboAllocInfo{};
        uboAllocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        if (vmaCreateBuffer(allocator, &uboInfo, &uboAllocInfo, &volumUboBuffer, &volumUboAllocation, nullptr) != VK_SUCCESS) {
            throw std::runtime_error("failed to create volumetric UBO buffer!");
        }

        VkDescriptorSetLayoutBinding bindings[3]{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[2].binding = 2;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo dslInfo{};
        dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslInfo.bindingCount = 3;
        dslInfo.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(device, &dslInfo, nullptr, &volumSetLayout) != VK_SUCCESS) {
            throw std::runtime_error("failed to create volumetric descriptor set layout");
        }

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &volumSetLayout;
        if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &volumPipelineLayout) != VK_SUCCESS) {
            throw std::runtime_error("failed to create volumetric pipeline layout");
        }

        VkDescriptorPoolSize poolSizes[2]{
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 }
        };
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = poolSizes;
        if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &volumDescriptorPool) != VK_SUCCESS) {
            throw std::runtime_error("failed to create volumetric descriptor pool");
        }
        VkDescriptorSetAllocateInfo setAlloc{};
        setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setAlloc.descriptorPool = volumDescriptorPool;
        setAlloc.descriptorSetCount = 1;
        setAlloc.pSetLayouts = &volumSetLayout;
        if (vkAllocateDescriptorSets(device, &setAlloc, &volumDescriptorSet) != VK_SUCCESS) {
            throw std::runtime_error("failed to allocate volumetric descriptor set");
        }

        VkDescriptorImageInfo depthInfo{ volumDepthSampler, depthImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo shadowInfo{ shadowMapSampler, shadowMapView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorBufferInfo uboDescInfo{ volumUboBuffer, 0, sizeof(VolumetricParams) };
        VkWriteDescriptorSet writes[3]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = volumDescriptorSet;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &depthInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = volumDescriptorSet;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = &shadowInfo;
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = volumDescriptorSet;
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[2].pBufferInfo = &uboDescInfo;
        vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertModule;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragModule;
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
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = 0xF;
        blend.blendEnable = VK_FALSE;
        VkPipelineColorBlendStateCreateInfo colorBlend{};
        colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlend.attachmentCount = 1;
        colorBlend.pAttachments = &blend;
        VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
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
        pipelineInfo.pColorBlendState = &colorBlend;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = volumPipelineLayout;
        pipelineInfo.renderPass = volumRenderPass;
        pipelineInfo.subpass = 0;
        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &volumPipeline) != VK_SUCCESS) {
            throw std::runtime_error("failed to create volumetric pipeline!");
        }
        vkDestroyShaderModule(device, vertModule, nullptr);
        vkDestroyShaderModule(device, fragModule, nullptr);
    }

void VulkanGame::buildSkinnedResources(){
        // 5x4 grid flag in the XY plane, x in [-1, 1], y in [0, 1.6].
        constexpr uint32_t cols = 5, rows = 4;
        std::vector<GameVertex> verts;
        verts.reserve(cols * rows);
        for (uint32_t r = 0; r < rows; ++r) {
            for (uint32_t c = 0; c < cols; ++c) {
                const float x = -1.0f + 2.0f * static_cast<float>(c) / static_cast<float>(cols - 1);
                const float y = 1.6f * static_cast<float>(r) / static_cast<float>(rows - 1);
                GameVertex v;
                v.pos = glm::vec3(x, y, 0.0f);
                v.normal = glm::vec3(0.0f, 0.0f, 1.0f);
                v.color = glm::vec3(1.0f);
                v.uv = glm::vec2(static_cast<float>(c) / static_cast<float>(cols - 1),
                                 static_cast<float>(r) / static_cast<float>(rows - 1));
                // Blend from bone 0 (left edge) to bone 1 (right edge).
                const float w1 = (x + 1.0f) * 0.5f;
                v.joints = glm::uvec4(0, 1, 0, 0);
                v.weights = glm::vec4(1.0f - w1, w1, 0.0f, 0.0f);
                verts.push_back(v);
            }
        }
        std::vector<uint32_t> indices;
        indices.reserve((cols - 1) * (rows - 1) * 6);
        for (uint32_t r = 0; r + 1 < rows; ++r) {
            for (uint32_t c = 0; c + 1 < cols; ++c) {
                const uint32_t a = r * cols + c;
                const uint32_t b = a + 1;
                const uint32_t d = (r + 1) * cols + c;
                const uint32_t e = d + 1;
                indices.insert(indices.end(), { a, b, d, b, e, d });
            }
        }
        skinnedVertexCount = static_cast<uint32_t>(verts.size());
        skinnedIndexCount = static_cast<uint32_t>(indices.size());
        createBuffer(sizeof(GameVertex) * verts.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                     verts.data(), skinnedVb, skinnedVbAllocation);
        createBuffer(sizeof(uint32_t) * indices.size(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                     indices.data(), skinnedIb, skinnedIbAllocation);

        // Skeleton: root at the flag base, tip joint at the right edge.
        skinnedSkeleton.name = "Flag";
        BoneNode root;
        root.name = "Root";
        root.parentIndex = -1;
        root.localTransform = glm::mat4(1.0f);
        root.inverseBindMatrix = glm::inverse(root.localTransform);
        skinnedSkeleton.bones.push_back(root);
        BoneNode tip;
        tip.name = "Tip";
        tip.parentIndex = 0;
        tip.localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        tip.inverseBindMatrix = glm::inverse(tip.localTransform);
        skinnedSkeleton.bones.push_back(tip);

        // A real AnimationClip driving the flag: the Tip joint swings between
        // 0 and 60 degrees over 2 seconds (loops) — sampled per frame through
        // AnimationSampler::sample (clip → pose → bone matrices → pipeline).
        skinnedClip.name = "FlagWave";
        skinnedClip.duration = 2.0f;
        skinnedClip.looping = true;
        skinnedClip.tracks.push_back(BoneTrack{ 1, {
            KeyFrame{ 0.0f, glm::vec3(0.0f), glm::angleAxis(0.0f, glm::vec3(0, 0, 1)), glm::vec3(1.0f) },
            KeyFrame{ 2.0f, glm::vec3(0.0f), glm::angleAxis(glm::radians(60.0f), glm::vec3(0, 0, 1)), glm::vec3(1.0f) },
        } });

        // Ragdoll: the same bones become physics bodies (root + tip capsules,
        // distance constraint between them) spawned above the camera view —
        // gravity + a per-frame impulse make the flag topple, and its pose is
        // blended with the clip to drive the skinned mesh.
        std::vector<Physics::RagdollBoneDesc> ragBones;
        ragBones.push_back(Physics::RagdollBoneDesc{
            "Root", "", glm::vec3(0.0f), glm::quat(1, 0, 0, 0), 0.6f, 0.12f, 1.0f, glm::vec3(0.0f) });
        ragBones.push_back(Physics::RagdollBoneDesc{
            "Tip", "Root", glm::vec3(1.0f, 0.0f, 0.0f), glm::quat(1, 0, 0, 0), 0.6f, 0.12f, 1.0f, glm::vec3(0.0f) });
        if (ragdoll.create(physics, ragBones, glm::vec3(0.0f, 3.0f, -2.5f))) {
            ragdollBuilt = true;
            std::cout << "[Game] Ragdoll active (bodies=" << ragBones.size()
                      << ", joints=1) — physics drives the skinned flag\n";
        }

        // Generated vertex shader (GpuSkinningBuffer) + dedicated fragment
        // shader consuming the shared LightParams UBO (matches LightUboData).
        const std::string fragSrc = R"(
#version 450
layout(location = 0) in vec2 vUv;
layout(location = 1) in vec3 vWorldPos;
layout(location = 2) in vec3 vNormal;
layout(location = 0) out vec4 outColor;
layout(binding = 2) uniform LightParams {
    vec4 cameraPosition;
    vec4 sunDirection;
    vec4 sunColor;
    mat4 sunViewProj;
    vec4 shadowParams;
    vec4 pointLightPos[8];
    vec4 pointLightColor[8];
    vec4 spotLightPos[4];
    vec4 spotLightDir[4];
    vec4 spotLightParams[4];
    vec4 spotLightColor[4];
    vec4 areaLightPos[4];
    vec4 areaLightNormal[4];
    vec4 areaLightHalf[4];
    vec4 areaLightColor[4];
    mat4 sunCascadeVP[4];
    vec4 sunCascadeSplits;
    vec4 cameraForward;
} lights;
void main() {
    vec3 n = normalize(vNormal);
    vec3 base = mix(vec3(0.30f, 0.38f, 0.58f), vec3(0.85f, 0.45f, 0.25f), vUv.x);
    vec3 lightAccum = vec3(0.18f);
    if (lights.sunDirection.w > 0.5f) {
        float ndl = max(dot(n, -lights.sunDirection.xyz), 0.0f);
        lightAccum += ndl * lights.sunColor.rgb * 0.25f;
    }
    for (int i = 0; i < 4; ++i) {
        if (lights.spotLightDir[i].w <= 0.5f) continue;
        vec3 toLight = lights.spotLightPos[i].xyz - vWorldPos;
        float dist = length(toLight);
        float range = max(lights.spotLightPos[i].w, 0.01f);
        float att = clamp(1.0f - dist / range, 0.0f, 1.0f);
        att *= att;
        vec3 L = toLight / max(dist, 0.0001f);
        float spot = smoothstep(lights.spotLightParams[i].y, lights.spotLightParams[i].x, dot(-L, lights.spotLightDir[i].xyz));
        float ndl = max(dot(n, L), 0.0f);
        lightAccum += ndl * att * spot * lights.spotLightColor[i].rgb;
    }
    for (int i = 0; i < 4; ++i) {
        if (lights.areaLightPos[i].w <= 0.5f) continue;
        vec3 toLight = lights.areaLightPos[i].xyz - vWorldPos;
        float dist = max(length(toLight), 0.0001f);
        float reach = max(lights.areaLightHalf[i].x + lights.areaLightHalf[i].y, 0.01f);
        float att = clamp(1.0f - dist / reach, 0.0f, 1.0f);
        att *= att;
        vec3 L = toLight / dist;
        float facing = max(dot(lights.areaLightNormal[i].xyz, -L), 0.0f);
        float ndl = max(dot(n, L), 0.0f);
        lightAccum += ndl * att * facing * lights.areaLightColor[i].rgb;
    }
    outColor = vec4(base * lightAccum, 1.0f);
}
)";
        std::vector<uint32_t> vertSpv = Rendering::compile_glsl_to_spirv(
            GpuSkinningBuffer::skinned_vertex_shader(kBoneCount), VK_SHADER_STAGE_VERTEX_BIT);
        std::vector<uint32_t> fragSpv = Rendering::compile_glsl_to_spirv(fragSrc, VK_SHADER_STAGE_FRAGMENT_BIT);
        if (vertSpv.empty() || fragSpv.empty()) {
            throw std::runtime_error("glslc failed to compile the skinned shaders (is glslc on PATH?)");
        }
        const VkShaderModule vertModule = createModuleFromSpirv(vertSpv);
        const VkShaderModule fragModule = createModuleFromSpirv(fragSpv);

        // Bone matrices + camera view-projection + light UBOs.
        const VkDeviceSize boneSize = static_cast<VkDeviceSize>(kBoneCount) * sizeof(glm::mat4);
        VkBufferCreateInfo uboInfo{};
        uboInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        uboInfo.size = boneSize;
        uboInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        uboInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo uboAllocInfo{};
        uboAllocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        if (vmaCreateBuffer(allocator, &uboInfo, &uboAllocInfo, &skinnedBoneBuffer, &skinnedBoneAllocation, nullptr) != VK_SUCCESS) {
            throw std::runtime_error("failed to create skinned bone UBO");
        }
        uboInfo.size = sizeof(glm::mat4);
        if (vmaCreateBuffer(allocator, &uboInfo, &uboAllocInfo, &skinnedCameraBuffer, &skinnedCameraAllocation, nullptr) != VK_SUCCESS) {
            throw std::runtime_error("failed to create skinned camera UBO");
        }

        VkDescriptorSetLayoutBinding bindings[3]{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        bindings[2].binding = 2;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo dslInfo{};
        dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslInfo.bindingCount = 3;
        dslInfo.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(device, &dslInfo, nullptr, &skinnedSetLayout) != VK_SUCCESS) {
            throw std::runtime_error("failed to create skinned descriptor set layout");
        }
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &skinnedSetLayout;
        if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &skinnedPipelineLayout) != VK_SUCCESS) {
            throw std::runtime_error("failed to create skinned pipeline layout");
        }

        VkDescriptorPoolSize poolSizes[1]{ { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3 } };
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = poolSizes;
        if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &skinnedDescriptorPool) != VK_SUCCESS) {
            throw std::runtime_error("failed to create skinned descriptor pool");
        }
        VkDescriptorSetAllocateInfo setAlloc{};
        setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setAlloc.descriptorPool = skinnedDescriptorPool;
        setAlloc.descriptorSetCount = 1;
        setAlloc.pSetLayouts = &skinnedSetLayout;
        if (vkAllocateDescriptorSets(device, &setAlloc, &skinnedDescriptorSet) != VK_SUCCESS) {
            throw std::runtime_error("failed to allocate skinned descriptor set");
        }
        VkDescriptorBufferInfo boneDesc{ skinnedBoneBuffer, 0, boneSize };
        VkDescriptorBufferInfo camDesc{ skinnedCameraBuffer, 0, sizeof(glm::mat4) };
        VkDescriptorBufferInfo lightDesc{ lightBuffer, 0, sizeof(Rendering::LightUboData) };
        VkWriteDescriptorSet writes[3]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = skinnedDescriptorSet;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &boneDesc;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = skinnedDescriptorSet;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[1].pBufferInfo = &camDesc;
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = skinnedDescriptorSet;
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[2].pBufferInfo = &lightDesc;
        vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertModule;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragModule;
        stages[1].pName = "main";

        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(GameVertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription attrs[5]{};
        attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(GameVertex, pos)) };
        attrs[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(GameVertex, normal)) };
        attrs[2] = { 2, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(GameVertex, uv)) };
        attrs[3] = { 3, 0, VK_FORMAT_R32G32B32A32_UINT, static_cast<uint32_t>(offsetof(GameVertex, joints)) };
        attrs[4] = { 4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, static_cast<uint32_t>(offsetof(GameVertex, weights)) };
        VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = 5;
        vertexInput.pVertexAttributeDescriptions = attrs;
        VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewportState{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp = VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = 0xF;
        blend.blendEnable = VK_FALSE;
        VkPipelineColorBlendStateCreateInfo colorBlend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        colorBlend.attachmentCount = 1;
        colorBlend.pAttachments = &blend;
        VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
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
        pipelineInfo.layout = skinnedPipelineLayout;
        pipelineInfo.renderPass = renderPass;
        pipelineInfo.subpass = 0;
        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &skinnedPipeline) != VK_SUCCESS) {
            throw std::runtime_error("failed to create skinned pipeline!");
        }
        vkDestroyShaderModule(device, vertModule, nullptr);
        vkDestroyShaderModule(device, fragModule, nullptr);
        skinnedBuilt = true;
        std::cout << "[Game] Skinned pipeline built (flag verts=" << skinnedVertexCount
                  << " indices=" << skinnedIndexCount << ", bones=" << skinnedSkeleton.bones.size() << ")\n";
    }

void VulkanGame::buildCompositePipeline(){
        const std::string vertSrc = R"(
#version 450
layout(location = 0) out vec2 vUv;
void main() {
    vec2 pos[3] = vec2[3](vec2(-1,-1), vec2(3,-1), vec2(-1,3));
    vUv = pos[gl_VertexIndex] * 0.5 + 0.5;
    gl_Position = vec4(pos[gl_VertexIndex], 0.0, 1.0);
}
)";
        const std::string fragSrc = R"(
#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
layout(binding = 0) uniform sampler2D sceneColor;
layout(binding = 1) uniform sampler2D volumetric;
void main() {
    outColor = vec4(texture(sceneColor, vUv).rgb + texture(volumetric, vUv).rgb, 1.0);
}
)";
        std::vector<uint32_t> vertSpv = Rendering::compile_glsl_to_spirv(vertSrc, VK_SHADER_STAGE_VERTEX_BIT);
        std::vector<uint32_t> fragSpv = Rendering::compile_glsl_to_spirv(fragSrc, VK_SHADER_STAGE_FRAGMENT_BIT);
        if (vertSpv.empty() || fragSpv.empty()) {
            throw std::runtime_error("glslc failed to compile the composite shaders (is glslc on PATH?)");
        }
        const VkShaderModule vertModule = createModuleFromSpirv(vertSpv);
        const VkShaderModule fragModule = createModuleFromSpirv(fragSpv);

        // Sampler for the HDR scene color.
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(device, &samplerInfo, nullptr, &compositeSampler) != VK_SUCCESS) {
            throw std::runtime_error("failed to create composite sampler");
        }

        VkDescriptorSetLayoutBinding samplerBindings[2]{};
        samplerBindings[0].binding = 0;
        samplerBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        samplerBindings[0].descriptorCount = 1;
        samplerBindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        samplerBindings[1].binding = 1;
        samplerBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        samplerBindings[1].descriptorCount = 1;
        samplerBindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo dslInfo{};
        dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslInfo.bindingCount = 2;
        dslInfo.pBindings = samplerBindings;
        if (vkCreateDescriptorSetLayout(device, &dslInfo, nullptr, &compositeSetLayout) != VK_SUCCESS) {
            throw std::runtime_error("failed to create composite descriptor set layout");
        }

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &compositeSetLayout;
        if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &compositePipelineLayout) != VK_SUCCESS) {
            throw std::runtime_error("failed to create composite pipeline layout");
        }

        VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 };
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &compositeDescriptorPool) != VK_SUCCESS) {
            throw std::runtime_error("failed to create composite descriptor pool");
        }
        VkDescriptorSetAllocateInfo setAlloc{};
        setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setAlloc.descriptorPool = compositeDescriptorPool;
        setAlloc.descriptorSetCount = 1;
        setAlloc.pSetLayouts = &compositeSetLayout;
        if (vkAllocateDescriptorSets(device, &setAlloc, &compositeDescriptorSet) != VK_SUCCESS) {
            throw std::runtime_error("failed to allocate composite descriptor set");
        }
        VkDescriptorImageInfo imageInfo{ compositeSampler, hdrImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo volumInfo{ compositeSampler, volumImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = compositeDescriptorSet;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &imageInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = compositeDescriptorSet;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = &volumInfo;
        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertModule;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragModule;
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
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = 0xF;
        blend.blendEnable = VK_FALSE;
        VkPipelineColorBlendStateCreateInfo colorBlend{};
        colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlend.attachmentCount = 1;
        colorBlend.pAttachments = &blend;
        VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
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
        pipelineInfo.pColorBlendState = &colorBlend;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = compositePipelineLayout;
        pipelineInfo.renderPass = compositeRenderPass;
        pipelineInfo.subpass = 0;
        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &compositePipeline) != VK_SUCCESS) {
            throw std::runtime_error("failed to create composite pipeline!");
        }
        vkDestroyShaderModule(device, vertModule, nullptr);
        vkDestroyShaderModule(device, fragModule, nullptr);
    }

void VulkanGame::buildRenderGraph(){
        if (renderGraphBuilt) return;
        using namespace Rendering;
        const auto hdrRes = renderGraph.add_resource({ "HDR Color", RenderResourceKind::Image, 0,
            swapChainExtent.width, swapChainExtent.height, 1, true, false, RenderResourceState::Undefined });
        const auto depthRes = renderGraph.add_resource({ "Scene Depth", RenderResourceKind::Image, 0,
            swapChainExtent.width, swapChainExtent.height, 1, true, false, RenderResourceState::Undefined });
        const auto swapRes = renderGraph.add_resource({ "Swapchain", RenderResourceKind::Image, 0,
            swapChainExtent.width, swapChainExtent.height, 1, false, true, RenderResourceState::Present });
        const auto shadowRes = renderGraph.add_resource({ "Shadow Map", RenderResourceKind::Image, 0,
            kShadowMapSize, kShadowMapSize, 1, true, false, RenderResourceState::Undefined });
        const auto volumRes = renderGraph.add_resource({ "Volumetric", RenderResourceKind::Image, 0,
            swapChainExtent.width, swapChainExtent.height, 1, true, false, RenderResourceState::Undefined });
        const auto shadowPass = renderGraph.add_pass({ "Shadow", RenderQueue::Graphics,
            { { shadowRes, RenderAccess::Write, RenderResourceState::DepthAttachment } }, true });
        const auto scenePass = renderGraph.add_pass({ "Scene", RenderQueue::Graphics,
            { { hdrRes, RenderAccess::Write, RenderResourceState::ColorAttachment },
              { depthRes, RenderAccess::Write, RenderResourceState::DepthAttachment },
              { shadowRes, RenderAccess::Read, RenderResourceState::ShaderRead } }, true });
        const auto volumPass = renderGraph.add_pass({ "Volumetric", RenderQueue::Graphics,
            { { depthRes, RenderAccess::Read, RenderResourceState::ShaderRead },
              { shadowRes, RenderAccess::Read, RenderResourceState::ShaderRead },
              { volumRes, RenderAccess::Write, RenderResourceState::ColorAttachment } }, true });
        const auto compositePass = renderGraph.add_pass({ "Composite", RenderQueue::Graphics,
            { { hdrRes, RenderAccess::Read, RenderResourceState::ShaderRead },
              { volumRes, RenderAccess::Read, RenderResourceState::ShaderRead },
              { swapRes, RenderAccess::Write, RenderResourceState::Present } }, true });
        (void)renderGraph.add_dependency(shadowPass, scenePass);
        (void)renderGraph.add_dependency(scenePass, volumPass);
        (void)renderGraph.add_dependency(volumPass, compositePass);

        std::string error;
        if (!renderGraphExecutor.initialize(device, renderGraph, &error)) {
            throw std::runtime_error("render graph init failed: " + error);
        }

        VulkanRenderGraphExecutor::PassFrame shadowFrame;
        shadowFrame.renderPass = shadowRenderPass;
        shadowFrame.framebuffers = { shadowFramebuffer };
        shadowFrame.clearValues.resize(1);
        shadowFrame.clearValues[0].depthStencil = { 1.0f, 0 };
        shadowFrame.draw = [this](VkCommandBuffer cb) { drawShadowPass(cb); };
        renderGraphExecutor.register_pass(shadowPass, std::move(shadowFrame));

        VulkanRenderGraphExecutor::PassFrame sceneFrame;
        sceneFrame.renderPass = renderPass;
        sceneFrame.framebuffers = { sceneFramebuffer };
        sceneFrame.clearValues.resize(2);
        sceneFrame.clearValues[0].color = { { 0.11f, 0.13f, 0.18f, 1.0f } };
        sceneFrame.clearValues[1].depthStencil = { 1.0f, 0 };
        sceneFrame.draw = [this](VkCommandBuffer cb) { drawScene(cb); };
        renderGraphExecutor.register_pass(scenePass, std::move(sceneFrame));

        VulkanRenderGraphExecutor::PassFrame volumFrame;
        volumFrame.renderPass = volumRenderPass;
        volumFrame.framebuffers = { volumFramebuffer };
        volumFrame.clearValues.resize(1);
        volumFrame.clearValues[0].color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
        volumFrame.draw = [this](VkCommandBuffer cb) { drawVolumetric(cb); };
        renderGraphExecutor.register_pass(volumPass, std::move(volumFrame));

        VulkanRenderGraphExecutor::PassFrame compositeFrame;
        compositeFrame.renderPass = compositeRenderPass;
        compositeFrame.framebuffers = compositeFramebuffers;
        compositeFrame.clearValues.resize(1);
        compositeFrame.clearValues[0].color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
        compositeFrame.draw = [this](VkCommandBuffer cb) { drawComposite(cb); };
        renderGraphExecutor.register_pass(compositePass, std::move(compositeFrame));

        renderGraphBuilt = true;
        std::cout << "[Game] Render graph frame wired ("
                  << renderGraphExecutor.compile_result().order.size() << " passes, "
                  << renderGraphExecutor.compile_result().barriers.size() << " barriers)\n";
    }

void VulkanGame::drawComposite(VkCommandBuffer cb){
        VkViewport viewport{ 0, 0, static_cast<float>(swapChainExtent.width),
                             static_cast<float>(swapChainExtent.height), 0.0f, 1.0f };
        vkCmdSetViewport(cb, 0, 1, &viewport);
        VkRect2D scissor{ { 0, 0 }, swapChainExtent };
        vkCmdSetScissor(cb, 0, 1, &scissor);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, compositePipeline);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, compositePipelineLayout,
                                0, 1, &compositeDescriptorSet, 0, nullptr);
        vkCmdDraw(cb, 3, 1, 0, 0);
    }

VkShaderModule VulkanGame::createModuleFromSpirv(const std::vector<uint32_t>& spirv){
        VkShaderModuleCreateInfo info{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        info.codeSize = spirv.size() * sizeof(uint32_t);
        info.pCode = spirv.data();
        VkShaderModule module;
        if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) {
            throw std::runtime_error("failed to create shader module from SPIR-V");
        }
        return module;
    }

GameMeshResource* VulkanGame::getMesh(const UUID& assetId){
        if (!assetId.is_valid()) return nullptr;
        const auto cached = meshes.find(assetId);
        if (cached != meshes.end()) return cached->second.valid ? &cached->second : nullptr;
        if (meshLoadFailed.contains(assetId)) return nullptr;

        std::filesystem::path cookedPath = content.absolute_path(assetId);
        std::string error;
        const GltfGeometryResult geometry = GltfGeometryParser::parse_vcmesh(cookedPath, &error);
        if (!geometry.success || geometry.primitives.empty()) {
            std::cerr << "[Game] Cannot load mesh " << assetId.to_string() << ": " << error << '\n';
            meshLoadFailed.insert(assetId);
            return nullptr;
        }
        GameMeshResource resource;
        std::vector<GameVertex> verts;
        std::vector<uint32_t> indices;
        for (const GltfMeshPrimitive& primitive : geometry.primitives) {
            const uint32_t vertexOffset = static_cast<uint32_t>(verts.size());
            verts.reserve(verts.size() + primitive.positions.size());
            const bool primitiveSkinned = !primitive.joints.empty() && primitive.joints.size() == primitive.positions.size() &&
                                          primitive.weights.size() == primitive.positions.size();
            if (primitiveSkinned) resource.skinned = true;
            for (size_t i = 0; i < primitive.positions.size(); ++i) {
                GameVertex v;
                v.pos = primitive.positions[i];
                v.normal = i < primitive.normals.size() ? primitive.normals[i] : glm::vec3(0.0f, 1.0f, 0.0f);
                v.color = glm::vec3(1.0f);
                v.uv = i < primitive.uvs.size() ? primitive.uvs[i] : glm::vec2(0.0f);
                if (primitiveSkinned) {
                    v.joints = primitive.joints[i];
                    v.weights = primitive.weights[i];
                }
                verts.push_back(v);
            }
            if (primitive.indexed) {
                const uint32_t firstIndex = static_cast<uint32_t>(indices.size());
                for (uint32_t index : primitive.indices) indices.push_back(index + vertexOffset);
                resource.ranges.push_back({ firstIndex, static_cast<uint32_t>(primitive.indices.size()), 0, true });
            } else {
                resource.ranges.push_back({ 0, static_cast<uint32_t>(primitive.positions.size()), vertexOffset, false });
            }
        }
        // Embed the first skin as the mesh skeleton (skinned draw path).
        if (resource.skinned && !geometry.skins.empty()) {
            const GltfGeometrySkin& source = geometry.skins.front();
            resource.skeleton.name = source.name.empty() ? "MeshSkeleton" : source.name;
            resource.skeleton.bones.resize(source.jointNames.size());
            for (size_t i = 0; i < source.jointNames.size(); ++i) {
                BoneNode& bone = resource.skeleton.bones[i];
                bone.name = source.jointNames[i];
                bone.parentIndex = i < source.jointParents.size() ? source.jointParents[i] : -1;
                bone.inverseBindMatrix = i < source.inverseBindMatrices.size()
                    ? source.inverseBindMatrices[i] : glm::mat4(1.0f);
                bone.localTransform = glm::inverse(bone.inverseBindMatrix);
            }
        }
        resource.valid = true;
        createBuffer(sizeof(GameVertex) * verts.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, verts.data(), resource.vb, resource.vbAlloc);
        if (!indices.empty()) {
            createBuffer(sizeof(uint32_t) * indices.size(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indices.data(), resource.ib, resource.ibAlloc);
        }
        meshes[assetId] = std::move(resource);
        return &meshes[assetId];
    }

GameMeshResource VulkanGame::buildCubeMesh(){
        std::vector<GameVertex> verts;
        std::vector<uint32_t> indices;
        const glm::vec3 n[6] = {
            { 0, 0, -1 }, { 0, 0, 1 }, { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 }, { 0, 1, 0 } };
        for (int f = 0; f < 6; ++f) {
            const uint32_t base = static_cast<uint32_t>(verts.size());
            const glm::vec3& axis = n[f];
            glm::vec3 u = glm::abs(axis.x) > 0.5f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
            glm::vec3 v = glm::normalize(glm::cross(axis, u));
            u = glm::normalize(glm::cross(v, axis));
            for (int c = 0; c < 4; ++c) {
                const glm::vec2 corner = glm::vec2((c & 1) ? 1 : 0, (c & 2) ? 1 : 0);
                glm::vec3 p = axis * 0.5f + u * (corner.x - 0.5f) + v * (corner.y - 0.5f);
                verts.push_back({ p, axis, glm::vec3(1.0f), corner });
            }
            indices.insert(indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
        }
        GameMeshResource resource;
        resource.valid = true;
        resource.ranges.push_back({ 0, static_cast<uint32_t>(indices.size()), 0, true });
        createBuffer(sizeof(GameVertex) * verts.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, verts.data(), resource.vb, resource.vbAlloc);
        createBuffer(sizeof(uint32_t) * indices.size(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indices.data(), resource.ib, resource.ibAlloc);
        return resource;
    }

void VulkanGame::writeMaterialUbo(const MaterialComponent& material){
        struct PbrUbo {
            glm::vec3 albedo;     // @0
            float pad0;           // @12
            glm::vec3 emissive;   // @16
            float pad1;           // @28
            float metallic;       // @32
            float roughness;      // @36
            float pad2, pad3;     // @40
        };
        PbrUbo ubo;
        ubo.albedo = material.albedo;
        ubo.emissive = material.emissiveColor * material.emissiveIntensity;
        ubo.metallic = material.metallic;
        ubo.roughness = material.roughness;
        ubo.pad0 = ubo.pad1 = ubo.pad2 = ubo.pad3 = 0.0f;
        void* mapped = nullptr;
        vmaMapMemory(allocator, uboAllocation, &mapped);
        std::memcpy(mapped, &ubo, sizeof(ubo));
        vmaUnmapMemory(allocator, uboAllocation);
    }

glm::mat4 VulkanGame::modelFromTransform(const TransformComponent& t) const{
        glm::mat4 model(1.0f);
        model = glm::translate(model, t.position);
        model = glm::rotate(model, glm::radians(t.rotation.z), glm::vec3(0, 0, 1));
        model = glm::rotate(model, glm::radians(t.rotation.y), glm::vec3(0, 1, 0));
        model = glm::rotate(model, glm::radians(t.rotation.x), glm::vec3(1, 0, 0));
        model = glm::scale(model, t.scale);
        return model;
    }

void VulkanGame::computeShadowCascades(const glm::mat4& lightView){
        const glm::vec3 front = cameraFront();
        const glm::vec3 up(0.0f, 1.0f, 0.0f);
        const glm::vec3 right = glm::normalize(glm::cross(front, up));
        const glm::vec3 camUp = glm::normalize(glm::cross(right, front));
        float aspect = static_cast<float>(swapChainExtent.width) / std::max(1u, swapChainExtent.height);
        float fov = 70.0f;
        for (const auto& [id, cam] : scene.cameraComponents) {
            (void)id;
            fov = cam.fov;
            break;
        }
        const float tanHalf = std::tan(glm::radians(fov) * 0.5f);
        constexpr float kNear = 0.1f, kFar = 1000.0f, kLambda = 0.5f;
        float splits[kGameShadowCascades + 1];
        splits[0] = kNear;
        for (uint32_t i = 1; i <= kGameShadowCascades; ++i) {
            const float p = static_cast<float>(i) / static_cast<float>(kGameShadowCascades);
            splits[i] = kLambda * kNear * std::pow(kFar / kNear, p) +
                        (1.0f - kLambda) * (kNear + (kFar - kNear) * p);
        }
        splits[kGameShadowCascades] = kFar;
        constexpr float kPad = 20.0f;
        for (uint32_t c = 0; c < kGameShadowCascades; ++c) {
            const float dNear = splits[c];
            const float dFar = splits[c + 1];
            const glm::vec3 cn = camPos + front * dNear;
            const glm::vec3 cf = camPos + front * dFar;
            const float hn = tanHalf * dNear, wn = hn * aspect;
            const float hf = tanHalf * dFar, wf = hf * aspect;
            const glm::vec3 corners[8] = {
                cn - right * wn - camUp * hn, cn + right * wn - camUp * hn,
                cn - right * wn + camUp * hn, cn + right * wn + camUp * hn,
                cf - right * wf - camUp * hf, cf + right * wf - camUp * hf,
                cf - right * wf + camUp * hf, cf + right * wf + camUp * hf,
            };
            float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f,
                  minZ = 1e30f, maxZ = -1e30f;
            for (const glm::vec3& corner : corners) {
                const glm::vec3 lc = glm::vec3(lightView * glm::vec4(corner, 1.0f));
                minX = std::min(minX, lc.x); maxX = std::max(maxX, lc.x);
                minY = std::min(minY, lc.y); maxY = std::max(maxY, lc.y);
                minZ = std::min(minZ, lc.z); maxZ = std::max(maxZ, lc.z);
            }
            const float halfX = (maxX - minX) * 0.5f + kPad;
            const float halfY = (maxY - minY) * 0.5f + kPad;
            glm::vec3 center((minX + maxX) * 0.5f, (minY + maxY) * 0.5f, (minZ + maxZ) * 0.5f);
            const float texelX = (halfX * 2.0f) / static_cast<float>(kShadowCascadeSize);
            const float texelY = (halfY * 2.0f) / static_cast<float>(kShadowCascadeSize);
            center.x = std::round(center.x / texelX) * texelX;
            center.y = std::round(center.y / texelY) * texelY;
            const glm::mat4 lightProj = glm::ortho(center.x - halfX, center.x + halfX,
                                                   center.y - halfY, center.y + halfY,
                                                   -maxZ - kPad, -minZ + kPad);
            // Remap NDC z ([-1,1] from glm::ortho) into [0,1] so computeShadow's
            // `sc.z < 0.0 || sc.z > 1.0` early-out and the `d < sc.z - bias`
            // compare match the depth actually stored in the shadow map.
            // Without this, the near half of the depth range is always lit.
            glm::mat4 depthRemap(1.0f);
            depthRemap[2][2] = 0.5f;
            depthRemap[2][3] = 0.5f;
            sunCascadeVP[c] = depthRemap * lightProj * lightView;
        }
        sunViewProj = sunCascadeVP[0];
        sunCascadeSplits = glm::vec4(splits[1], splits[2], splits[3],
                                     static_cast<float>(kGameShadowCascades));
    }

void VulkanGame::updateSunShadow(){
        sunEnabled = false;
        // O gate sunShadowStatusLogged loga o status das cascades uma única
        // vez; o reset por frame aqui anulava o gate (spam de 1 linha/frame).
        for (const auto& [id, light] : scene.lightComponents) {
            if (!is_directional_sun(light)) continue;
            glm::vec3 dir(0.0f, -1.0f, 0.0f);
            const auto tit = scene.transformComponents.find(id);
            if (tit != scene.transformComponents.end()) {
                const float yaw = glm::radians(tit->second.rotation.y);
                const float pitch = glm::radians(tit->second.rotation.x);
                dir = glm::normalize(glm::vec3(
                    std::cos(pitch) * std::sin(yaw), std::sin(pitch),
                    std::cos(pitch) * std::cos(yaw)));
            }
            const glm::vec3 target(0.0f, 1.0f, 0.0f);
            const glm::vec3 eye = target - dir * 60.0f;
            const glm::mat4 lightView = glm::lookAt(eye, target, glm::vec3(0, 1, 0));
            computeShadowCascades(lightView);
            sunEnabled = true;
            if (!sunShadowStatusLogged) {
                sunShadowStatusLogged = true;
                std::cout << "[Game] Sun shadow cascades active (atlas " << kShadowMapSize
                          << "x" << kShadowMapSize << " = " << kGameShadowCascades
                          << " x " << kShadowCascadeSize << "x" << kShadowCascadeSize << ")\n";
            }
            break;
        }
    }

void VulkanGame::drawShadowPass(VkCommandBuffer cb){
        if (!sunEnabled) return;
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline);
        for (uint32_t cascade = 0; cascade < kGameShadowCascades; ++cascade) {
            const uint32_t tileX = (cascade % 2) * kShadowCascadeSize;
            const uint32_t tileY = (cascade / 2) * kShadowCascadeSize;
            VkViewport viewport{ static_cast<float>(tileX), static_cast<float>(tileY),
                                 static_cast<float>(kShadowCascadeSize),
                                 static_cast<float>(kShadowCascadeSize), 0.0f, 1.0f };
            vkCmdSetViewport(cb, 0, 1, &viewport);
            VkRect2D scissor{ { static_cast<int32_t>(tileX), static_cast<int32_t>(tileY) },
                              { kShadowCascadeSize, kShadowCascadeSize } };
            vkCmdSetScissor(cb, 0, 1, &scissor);
            const glm::mat4 cascadeVP = sunCascadeVP[cascade];
            for (const auto& [id, mr] : scene.meshRendererComponents) {
                if (!mr.isVisible) continue;
                const auto tit = scene.transformComponents.find(id);
                if (tit == scene.transformComponents.end()) continue;
                const glm::mat4 model = modelFromTransform(tit->second);
                const Rendering::MaterialPushConstants pc{ cascadeVP * model, model };
                vkCmdPushConstants(cb, shadowPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
                const VkDeviceSize offset = 0;
                const GameMeshResource* mesh = getMesh(mr.meshAssetID);
                if (!mesh) {
                    if (!cubeBuilt) continue;
                    mesh = &cubeMesh;
                }
                vkCmdBindVertexBuffers(cb, 0, 1, &mesh->vb, &offset);
                if (mesh->ib != VK_NULL_HANDLE) vkCmdBindIndexBuffer(cb, mesh->ib, 0, VK_INDEX_TYPE_UINT32);
                for (const MeshRange& range : mesh->ranges) {
                    if (range.indexed) vkCmdDrawIndexed(cb, range.indexCount, 1, range.firstIndex, 0, 0);
                    else vkCmdDraw(cb, range.indexCount, 1, range.vertexOffset, 0);
                }
            }
        }
    }

void VulkanGame::writeLightUbo(const glm::vec3& camPos){
        Rendering::LightUboData data{};
        data.cameraPosition = glm::vec4(camPos, 1.0f);
        data.shadowParams = glm::vec4(sunEnabled ? 1.0f : 0.0f, shadowBias,
                                      sunEnabled ? static_cast<float>(kGameShadowCascades) : 0.0f, 0.0f);
        data.sunViewProj = sunCascadeVP[0];
        for (uint32_t i = 0; i < kGameShadowCascades; ++i) data.sunCascadeVP[i] = sunCascadeVP[i];
        data.sunCascadeSplits = sunCascadeSplits;
        data.cameraForward = glm::vec4(cameraFront(), 0.0f);
        uint32_t pointCount = 0, spotCount = 0, areaCount = 0;
        for (const auto& [id, light] : scene.lightComponents) {
            glm::vec3 dir(0.0f, -1.0f, 0.0f);
            glm::vec3 position(0.0f);
            const auto tit = scene.transformComponents.find(id);
            if (tit != scene.transformComponents.end()) {
                position = tit->second.position;
                const float yaw = glm::radians(tit->second.rotation.y);
                const float pitch = glm::radians(tit->second.rotation.x);
                dir = glm::normalize(glm::vec3(
                    std::cos(pitch) * std::sin(yaw), std::sin(pitch),
                    std::cos(pitch) * std::cos(yaw)));
            }
            const glm::vec3 colorIntensity = light.color * light.intensity;
            if (is_directional_sun(light)) {
                data.sunDirection = glm::vec4(dir, 1.0f);
                data.sunColor = glm::vec4(colorIntensity, 1.0f);
            } else if (light.type == LightType::Spot && spotCount < Rendering::kMaxSpotLights) {
                data.spotLightPos[spotCount] = glm::vec4(position, light.range);
                data.spotLightDir[spotCount] = glm::vec4(dir, 1.0f);
                data.spotLightParams[spotCount] = glm::vec4(
                    std::cos(glm::radians(25.0f)), std::cos(glm::radians(45.0f)), 0.0f, 0.0f);
                data.spotLightColor[spotCount] = glm::vec4(colorIntensity, 1.0f);
                ++spotCount;
            } else if (light.type == LightType::Area && areaCount < Rendering::kMaxAreaLights) {
                data.areaLightPos[areaCount] = glm::vec4(position, 1.0f);
                data.areaLightNormal[areaCount] = glm::vec4(dir, 1.0f);
                data.areaLightHalf[areaCount] = glm::vec4(2.0f, 1.0f, 0.0f, 0.0f);
                data.areaLightColor[areaCount] = glm::vec4(colorIntensity, 1.0f);
                ++areaCount;
            } else if (pointCount < Rendering::kMaxPointLights) {
                data.pointLightPos[pointCount] = glm::vec4(position, light.range);
                data.pointLightColor[pointCount] = glm::vec4(colorIntensity, 1.0f);
                ++pointCount;
            }
        }
        if (!lightStatusLogged) {
            lightStatusLogged = true;
            std::cout << "[Game] Lights in scene: " << (sunEnabled ? 1 : 0) << " sun, "
                      << pointCount << " point, " << spotCount << " spot, "
                      << areaCount << " area\n";
        }
        void* mapped = nullptr;
        vmaMapMemory(allocator, lightAllocation, &mapped);
        std::memcpy(mapped, &data, sizeof(data));
        vmaUnmapMemory(allocator, lightAllocation);
    }

glm::mat4 VulkanGame::computeCameraView() const{
        return glm::lookAt(camPos, camPos + cameraFront(), glm::vec3(0, 1, 0));
    }

glm::mat4 VulkanGame::computeCameraProj() const{
        float aspect = static_cast<float>(swapChainExtent.width) / std::max(1u, swapChainExtent.height);
        float fov = 70.0f;
        for (const auto& [id, cam] : scene.cameraComponents) {
            (void)id;
            fov = cam.fov;
            break;
        }
        glm::mat4 proj = glm::perspective(glm::radians(fov), aspect, 0.1f, 1000.0f);
        proj[1][1] *= -1.0f; // Vulkan clip space
        return proj;
    }

void VulkanGame::writeVolumetricUbo(){
        const glm::mat4 view = computeCameraView();
        const glm::mat4 proj = computeCameraProj();
        VolumetricParams data{};
        data.cameraPosition = glm::vec4(camPos, 1.0f);
        data.cameraViewProj = proj * view;
        data.invViewProj = glm::inverse(proj * view);
        data.sunViewProj = sunCascadeVP[0];
        data.shadowParams = glm::vec4(sunEnabled ? 1.0f : 0.0f, shadowBias,
                                      sunEnabled ? static_cast<float>(kGameShadowCascades) : 0.0f, 0.0f);
        data.nearFar = glm::vec4(0.1f, 1000.0f, kVolumetricDensity, kVolumetricSteps);
        for (uint32_t i = 0; i < kGameShadowCascades; ++i) data.sunCascadeVP[i] = sunCascadeVP[i];
        data.sunCascadeSplits = sunCascadeSplits;
        data.cameraForward = glm::vec4(cameraFront(), 0.0f);
        glm::vec3 sunDir(0.0f, -1.0f, 0.0f);
        glm::vec3 sunColor(1.0f);
        bool sunActive = false;
        for (const auto& [id, light] : scene.lightComponents) {
            if (!is_directional_sun(light)) continue;
            const auto tit = scene.transformComponents.find(id);
            if (tit != scene.transformComponents.end()) {
                const float yaw = glm::radians(tit->second.rotation.y);
                const float pitch = glm::radians(tit->second.rotation.x);
                sunDir = glm::normalize(glm::vec3(
                    std::cos(pitch) * std::sin(yaw), std::sin(pitch),
                    std::cos(pitch) * std::cos(yaw)));
            }
            sunColor = light.color;
            sunActive = true;
            break;
        }
        const float len = glm::length(sunColor);
        if (len > 1e-5f) sunColor /= len;
        data.sunDirection = glm::vec4(sunDir, sunActive ? 1.0f : 0.0f);
        data.sunColor = glm::vec4(sunColor * kVolumetricIntensity, kVolumetricIntensity);
        void* mapped = nullptr;
        vmaMapMemory(allocator, volumUboAllocation, &mapped);
        std::memcpy(mapped, &data, sizeof(data));
        vmaUnmapMemory(allocator, volumUboAllocation);
    }

void VulkanGame::drawVolumetric(VkCommandBuffer cb){
        VkViewport viewport{ 0, 0, static_cast<float>(swapChainExtent.width),
                             static_cast<float>(swapChainExtent.height), 0.0f, 1.0f };
        vkCmdSetViewport(cb, 0, 1, &viewport);
        VkRect2D scissor{ { 0, 0 }, swapChainExtent };
        vkCmdSetScissor(cb, 0, 1, &scissor);
        writeVolumetricUbo();
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, volumPipeline);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, volumPipelineLayout,
                                0, 1, &volumDescriptorSet, 0, nullptr);
        vkCmdDraw(cb, 3, 1, 0, 0);
    }

void VulkanGame::drawScene(VkCommandBuffer cb){
        VkViewport viewport{ 0, 0, static_cast<float>(swapChainExtent.width),
                             static_cast<float>(swapChainExtent.height), 0.0f, 1.0f };
        vkCmdSetViewport(cb, 0, 1, &viewport);
        VkRect2D scissor{ { 0, 0 }, swapChainExtent };
        vkCmdSetScissor(cb, 0, 1, &scissor);

        // Camera: free-fly state (initialized from the authored camera).
        const glm::mat4 view = computeCameraView();
        const glm::mat4 proj = computeCameraProj();

        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
        writeLightUbo(camPos);

        if (!cubeBuilt) {
            cubeMesh = buildCubeMesh();
            cubeBuilt = true;
        }
        for (const auto& [id, mr] : scene.meshRendererComponents) {
            if (!mr.isVisible) continue;
            const auto tit = scene.transformComponents.find(id);
            if (tit == scene.transformComponents.end()) continue;
            const TransformComponent& t = tit->second;

            const glm::mat4 model = modelFromTransform(t);

            const MaterialComponent* material = nullptr;
            const auto mit = scene.materialComponents.find(id);
            if (mit != scene.materialComponents.end()) material = &mit->second;
            const MaterialComponent fallback{ glm::vec3(0.8f, 0.8f, 0.8f), 0.5f, 0.0f, glm::vec3(0.0f), 0.0f };
            writeMaterialUbo(material ? *material : fallback);

            const glm::mat4 mvp = proj * view * model;
            const GameMeshResource* mesh = getMesh(mr.meshAssetID);
            if (mesh && mesh->skinned && skinnedBuilt && !mesh->skeleton.bones.empty()) {
                // Real skinned asset: animate the deepest joint and draw it
                // through the skinned pipeline (bind pose otherwise).
                Pose pose = AnimationSampler::bind_pose(mesh->skeleton);
                int32_t deepest = -1, bestDepth = -1;
                for (size_t i = 0; i < mesh->skeleton.bones.size(); ++i) {
                    int32_t depth = 0;
                    for (int32_t p = mesh->skeleton.bones[i].parentIndex; p >= 0; p = mesh->skeleton.bones[p].parentIndex) ++depth;
                    if (depth > bestDepth) { bestDepth = depth; deepest = static_cast<int32_t>(i); }
                }
                if (deepest >= 0) {
                    pose.local[static_cast<size_t>(deepest)].rotation = glm::angleAxis(
                        0.5f * std::sin(skinnedAnimTime * 2.0f), glm::vec3(0.0f, 0.0f, 1.0f));
                }
                drawSkinnedMesh(cb, mesh->vb, mesh->ib, mesh->ranges, mesh->skeleton, pose);
            } else if (mesh) {
                drawMeshResource(cb, *mesh, mvp, model);
            } else {
                drawMeshResource(cb, cubeMesh, mvp, model);
            }
        }

        drawSkinned(cb);
    }

void VulkanGame::drawSkinned(VkCommandBuffer cb){
        if (!skinnedBuilt) return;
        Pose pose = AnimationSampler::sample(skinnedSkeleton, skinnedClip, skinnedAnimTime);
        if (ragdollBuilt && !ragdoll.empty()) {
            std::vector<RagdollBody> bodies;
            for (const Physics::RagdollPoseBone& bone : ragdoll.pose(physics)) {
                const int boneIndex = skinnedSkeleton.find_bone_index(bone.name);
                if (boneIndex < 0) continue;
                const glm::mat4 world = glm::translate(glm::mat4(1.0f), bone.position) *
                                        glm::mat4_cast(bone.rotation);
                bodies.push_back(RagdollBody{ boneIndex, world, 1.0f });
            }
            if (!bodies.empty()) {
                pose = RagdollPoseBridge::blend_physics_pose(skinnedSkeleton, pose, bodies, ragdollBlendWeight);
            }
        } else {
            pose.local[0].translation = glm::vec3(0.0f, 3.0f, -2.5f);
        }
        const std::vector<MeshRange> ranges{ { 0, skinnedIndexCount, 0, true } };
        drawSkinnedMesh(cb, skinnedVb, skinnedIb, ranges, skinnedSkeleton, pose);
        if (!skinnedLogged) {
            skinnedLogged = true;
            std::cout << "[Game] Skinned mesh drawn (verts=" << skinnedVertexCount
                      << ", bones=" << skinnedSkeleton.bones.size()
                      << ") clip+ragdoll -> pose -> bone UBO\n";
        }
    }

void VulkanGame::destroyMeshResource(GameMeshResource& mesh){
        if (mesh.vb != VK_NULL_HANDLE) vmaDestroyBuffer(allocator, mesh.vb, mesh.vbAlloc);
        if (mesh.ib != VK_NULL_HANDLE) vmaDestroyBuffer(allocator, mesh.ib, mesh.ibAlloc);
        mesh = GameMeshResource{};
    }

void VulkanGame::updateHud(float dt){
        hudTimer -= dt;
        if (hudTimer > 0.0) return;
        hudTimer = 0.25;
        const int remaining = static_cast<int>(fpsTargets.size()) - targetsDestroyed;
        char title[192];
        std::snprintf(title, sizeof(title),
                      "VulkanCraft FPS | Ammo %u/%u%s | Targets %d/%d | %.0f FPS",
                      weapon.ammo(), weapon.reserve(),
                      weapon.reloading() ? " [RELOADING]" : "",
                      remaining, static_cast<int>(fpsTargets.size()),
                      1.0f / std::max(dt, 1e-5f));
        glfwSetWindowTitle(window, title);
    }

