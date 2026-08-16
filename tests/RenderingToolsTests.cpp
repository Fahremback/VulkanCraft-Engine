#include "../src/engine/rendering/MaterialGraph.hpp"
#include "../src/engine/rendering/RenderGraph.hpp"
#include "../src/engine/rendering/RenderingFoundation.hpp"
#include "../src/engine/rendering/vulkan/MaterialPipeline.hpp"
#include "../src/editor/tools/RenderingToolModels.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <process.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>

using namespace Engine::Rendering;

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "RenderingToolsTests failure at line " << __LINE__ << ": " #condition "\n"; \
    return EXIT_FAILURE; } } while (false)

int main() {
    // Typed material graph -> deterministic IR and material instance overrides.
    auto material = std::make_shared<MaterialDefinition>("Paint");
    CHECK(material->graph().define_parameter({"baseColor", MaterialValueType::Vec3, glm::vec3(0.8f, 0.2f, 0.1f), true}));
    CHECK(material->graph().define_parameter({"roughness", MaterialValueType::Float, 0.65f, true}));
    const MaterialNodeId colorParameter = material->graph().add_parameter("baseColor");
    const MaterialNodeId roughnessParameter = material->graph().add_parameter("roughness");
    const MaterialNodeId roughnessScale = material->graph().add_constant("Scale", 0.5f);
    const MaterialNodeId multiply = material->graph().add_operation(MaterialNodeKind::Multiply, MaterialValueType::Float);
    const MaterialNodeId colorOutput = material->graph().add_output("BaseColor", MaterialValueType::Vec3);
    const MaterialNodeId roughnessOutput = material->graph().add_output("Roughness", MaterialValueType::Float);
    CHECK(material->graph().connect(roughnessParameter, multiply, 0));
    CHECK(material->graph().connect(roughnessScale, multiply, 1));
    CHECK(material->graph().connect(colorParameter, colorOutput, 0));
    CHECK(material->graph().connect(multiply, roughnessOutput, 0));
    const MaterialCompileResult materialIR = material->compile();
    CHECK(materialIR);
    CHECK(materialIR.ir.outputs.size() == 2);
    CHECK(materialIR.ir.instructions.size() == 6);

    const MaterialNodeId wrongType = material->graph().add_constant("Wrong", glm::vec2(1.0f));
    std::string typeError;
    CHECK(!material->graph().connect(wrongType, roughnessOutput, 0, &typeError));
    CHECK(!typeError.empty());
    CHECK(material->graph().remove_node(wrongType));

    MaterialInstance instance(material);
    CHECK(instance.set_parameter("roughness", 0.2f));
    CHECK(instance.has_override("roughness"));
    CHECK(std::abs(std::get<float>(*instance.parameter("roughness")) - 0.2f) < 0.0001f);
    CHECK(instance.clear_parameter("roughness"));
    CHECK(std::abs(std::get<float>(*instance.parameter("roughness")) - 0.65f) < 0.0001f);
    CHECK(!instance.set_parameter("roughness", glm::vec3(1.0f)));

    ShaderPermutation permutationA;
    ShaderPermutation permutationB;
    CHECK(permutationA.set("USE_NORMAL_MAP"));
    CHECK(permutationA.set("ALPHA_MODE", "MASK"));
    CHECK(permutationB.set("ALPHA_MODE", "MASK"));
    CHECK(permutationB.set("USE_NORMAL_MAP"));
    CHECK(permutationA.hash() == permutationB.hash());
    CHECK(permutationA.canonical_key() == "ALPHA_MODE=MASK;USE_NORMAL_MAP=1;");
    ShaderCacheMetadata shaderCache;
    shaderCache.upsert({permutationA.hash(), 11, 22, 3, "vulkan-spirv1.6", "paint.spv", {"pbr.glsl", "common.glsl"}});
    CHECK(shaderCache.find(permutationA.hash(), "vulkan-spirv1.6"));
    CHECK(!shaderCache.is_stale(permutationA.hash(), "vulkan-spirv1.6", 11, 3, {"common.glsl", "pbr.glsl"}));
    CHECK(shaderCache.is_stale(permutationA.hash(), "vulkan-spirv1.6", 12, 3, {"common.glsl", "pbr.glsl"}));

    // Render graph dependencies, topological schedule, barriers and transient lifetimes.
    RenderGraph graph;
    const RenderResourceId shadowMap = graph.add_resource({"ShadowAtlas", RenderResourceKind::Image, 0, 2048, 2048, 1, false, true,
                                                            RenderResourceState::ShaderRead});
    const RenderResourceId depth = graph.add_resource({"Depth", RenderResourceKind::Image, 0, 1920, 1080});
    const RenderResourceId hdr = graph.add_resource({"HDR", RenderResourceKind::Image, 0, 1920, 1080});
    const RenderPassId shadowPass = graph.add_pass({"Shadow", RenderQueue::Graphics,
        {{shadowMap, RenderAccess::Write, RenderResourceState::DepthAttachment}}});
    const RenderPassId gbufferPass = graph.add_pass({"GBuffer", RenderQueue::Graphics,
        {{depth, RenderAccess::Write, RenderResourceState::DepthAttachment}}});
    const RenderPassId lightingPass = graph.add_pass({"Lighting", RenderQueue::Compute,
        {{shadowMap, RenderAccess::Read, RenderResourceState::ShaderRead},
         {depth, RenderAccess::Read, RenderResourceState::ShaderRead},
         {hdr, RenderAccess::Write, RenderResourceState::General}}});
    CHECK(graph.add_dependency(shadowPass, gbufferPass));
    CHECK(graph.add_dependency(gbufferPass, lightingPass));
    const RenderGraphCompileResult compiledGraph = graph.compile();
    CHECK(compiledGraph);
    CHECK(compiledGraph.order == std::vector<RenderPassId>({shadowPass, gbufferPass, lightingPass}));
    CHECK(compiledGraph.lifetimes.size() == 3);
    CHECK(compiledGraph.barriers.size() >= 4);

    RenderGraph cyclic;
    const RenderPassId first = cyclic.add_pass({"First"});
    const RenderPassId second = cyclic.add_pass({"Second"});
    CHECK(cyclic.add_dependency(first, second));
    CHECK(cyclic.add_dependency(second, first));
    CHECK(!cyclic.compile());

    // Lighting math, atlas reuse/allocation and render/debug statistics.
    DirectionalLight sun;
    sun.direction = {2.0f, -2.0f, 0.0f};
    sun.sanitize();
    CHECK(sun.valid());
    CHECK(std::abs(glm::length(sun.direction) - 1.0f) < 0.0001f);
    PointLight point;
    point.range = 8.0f;
    CHECK(point.valid());
    CHECK(point.attenuation(1.0f) > point.attenuation(4.0f));
    CHECK(point.attenuation(9.0f) == 0.0f);
    SpotLight spot;
    spot.position = {0.0f, 0.0f, 0.0f};
    spot.direction = {0.0f, 0.0f, -1.0f};
    spot.sanitize();
    CHECK(spot.valid());
    CHECK(spot.attenuation({0.0f, 0.0f, -2.0f}) > spot.attenuation({2.0f, 0.0f, -2.0f}));

    ShadowAtlasAllocator atlas(1024, 1024, 128);
    const auto tileA = atlas.allocate(100, 300);
    const auto tileB = atlas.allocate(200, 128);
    CHECK(tileA && tileA->width == 512);
    CHECK(tileB && tileB->width == 128);
    CHECK(atlas.allocate(100, 128)->id == tileA->id);
    CHECK(atlas.utilization() > 0.0f);
    CHECK(atlas.release(tileA->id));
    CHECK(!atlas.find(tileA->id));
    CHECK(atlas.release_owner(200) == 1);
    CHECK(atlas.utilization() == 0.0f);

    RenderStatsCollector stats;
    stats.begin_frame(42);
    stats.record_pass({"GBuffer", 1.0, 0.8, 12, 0, 2048});
    stats.record_pass({"Lighting", 0.4, 1.2, 0, 2, 0});
    stats.record_visibility(25, 75);
    stats.record_barriers(compiledGraph.barriers.size());
    stats.record_shader_cache(true);
    stats.record_shader_cache(false);
    stats.end_frame(2.0, 2.5);
    const RenderStats snapshot = stats.snapshot();
    CHECK(snapshot.frameIndex == 42 && snapshot.drawCalls == 12 && snapshot.dispatchCalls == 2);
    CHECK(snapshot.visibleObjects == 25 && snapshot.culledObjects == 75);
    CHECK(snapshot.shaderCacheHits == 1 && snapshot.shaderCacheMisses == 1);

    RenderDebugStats debug(2);
    debug.push({RenderDebugMessage::Severity::Info, "Graph", "Compiled", 41});
    debug.push({RenderDebugMessage::Severity::Warning, "Shadow", "Atlas nearly full", 42});
    debug.push({RenderDebugMessage::Severity::Error, "Shader", "Permutation failed", 42});
    CHECK(debug.messages().size() == 2);
    CHECK(debug.count(RenderDebugMessage::Severity::Warning) == 1);
    CHECK(debug.count(RenderDebugMessage::Severity::Error) == 1);

    // UI-independent editor models expose graph state without mutating renderer internals.
    Engine::Editor::MaterialEditorModel materialEditor(&material->graph());
    CHECK(materialEditor.select(multiply));
    CHECK(materialEditor.nodes("rough").size() >= 2);
    CHECK(materialEditor.compile_preview());
    CHECK(!materialEditor.dirty());
    CHECK(materialEditor.connect(roughnessScale, multiply, 1));
    CHECK(materialEditor.dirty() && materialEditor.revision() == 1);
    materialEditor.mark_saved();
    CHECK(!materialEditor.dirty());

    Engine::Editor::RenderGraphViewerModel graphViewer;
    graphViewer.rebuild(graph);
    CHECK(graphViewer.compilation());
    CHECK(graphViewer.passes().size() == 3);
    CHECK(graphViewer.resources("depth").size() == 1);
    CHECK(graphViewer.select_pass(lightingPass));
    CHECK(!graphViewer.barriers_for_pass(lightingPass).empty());
    CHECK(graphViewer.select_resource(depth));
    CHECK(graphViewer.selected_resource() == depth && !graphViewer.selected_pass());

    // Material graph → real GLSL: parameters appear as UBO members and the
    // compiled IR drives expressions that reference them.
    {
        const GlslGenerationResult glsl = material_graph_to_glsl(material->graph());
        CHECK(glsl);
        CHECK(!glsl.source.empty());
        CHECK(glsl.source.find("#version 450") != std::string::npos);
        CHECK(glsl.source.find("uniform MaterialParams") != std::string::npos);
        CHECK(glsl.source.find("baseColor") != std::string::npos);
        // Exposed parameters are exposed as UBO members.
        CHECK(glsl.source.find("baseColor;") != std::string::npos ||
              glsl.source.find("baseColor ") != std::string::npos);
        // The LightParams block carries the spot/area arrays and the lighting
        // code evaluates their cones/facing (LightComponent Spot/Area support).
        CHECK(glsl.source.find("spotLightPos[4]") != std::string::npos);
        CHECK(glsl.source.find("spotLightParams[4]") != std::string::npos);
        CHECK(glsl.source.find("areaLightHalf[4]") != std::string::npos);
        CHECK(glsl.source.find("smoothstep(lights.spotLightParams[i].y") != std::string::npos);
        CHECK(glsl.source.find("dot(lights.areaLightNormal[i].xyz, -L)") != std::string::npos);
        // Shadow cascades: the generated shader exposes the per-cascade VPs,
        // the split points and a computeShadow helper with cascade selection.
        CHECK(glsl.source.find("mat4 sunCascadeVP[4]") != std::string::npos);
        CHECK(glsl.source.find("vec4 sunCascadeSplits") != std::string::npos);
        CHECK(glsl.source.find("float computeShadow(vec3 worldPos)") != std::string::npos);
        CHECK(glsl.source.find("lights.sunCascadeVP[c]") != std::string::npos);
        CHECK(glsl.source.find("computeShadow(vWorldPos)") != std::string::npos);
        CHECK(!glsl.uniformNames.empty());
        // The generated shader must be syntactically plausible: balanced braces.
        int depthBraces = 0;
        for (const char c : glsl.source) {
            if (c == '{') ++depthBraces;
            else if (c == '}') --depthBraces;
        }
        CHECK(depthBraces == 0);
        CHECK(glsl.source.find("void main()") != std::string::npos);
    }

    // The generated GLSL must compile to SPIR-V with glslc — the editor viewport
    // compiles material-graph shaders at runtime through exactly this path.
    const auto compileWithGlslc = [](const std::string& source, const std::string& tag) {
        // pid suffix: concurrent instances must not share temp paths.
        const std::filesystem::path tmp = std::filesystem::temp_directory_path() /
            ("vc_test_mat_" + std::to_string(_getpid()) + "_" + tag);
        const std::filesystem::path srcFile = std::filesystem::path(tmp.string() + ".frag");
        const std::filesystem::path spvFile = std::filesystem::path(tmp.string() + ".spv");
        {
            std::ofstream out(srcFile, std::ios::binary);
            out << source;
        }
        const std::string cmd = "glslc \"" + srcFile.string() + "\" -fshader-stage=frag -o \"" +
                                spvFile.string() + "\" 2>nul";
        const int rc = std::system(cmd.c_str());
        bool valid = false;
        if (rc == 0) {
            std::ifstream in(spvFile, std::ios::binary);
            if (in) {
                uint32_t magic = 0;
                in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
                valid = magic == 0x07230203u;
            }
        }
        std::error_code ec;
        std::filesystem::remove(srcFile, ec);
        std::filesystem::remove(spvFile, ec);
        return valid;
    };
    {
        const GlslGenerationResult glsl = material_graph_to_glsl(material->graph());
        CHECK(glsl);
        CHECK(compileWithGlslc(glsl.source, "paint"));
    }
    {
        // A graph with no exposed parameters must still produce valid GLSL
        // (the generator emits a placeholder UBO member).
        MaterialGraph plain;
        const MaterialNodeId constant = plain.add_constant("Color", glm::vec3(1.0f, 0.0f, 0.0f));
        const MaterialNodeId output = plain.add_output("BaseColor", MaterialValueType::Vec3);
        (void)plain.connect(constant, output, 0);
        const GlslGenerationResult glsl = material_graph_to_glsl(plain);
        CHECK(glsl);
        CHECK(glsl.source.find("_placeholder") != std::string::npos);
        CHECK(compileWithGlslc(glsl.source, "plain"));
    }

    // RenderGraph → executor: compiling a real graph produces ordered passes
    // with barriers and lifetimes, and the executor consumes them.
    {
        RenderGraph executorGraph;
        const RenderResourceId color = executorGraph.add_resource({"color", RenderResourceKind::Image, 0, 1024, 768, 1, true, false, RenderResourceState::Undefined});
        const RenderResourceId depthRes = executorGraph.add_resource({"depth", RenderResourceKind::Image, 0, 1024, 768, 1, true, false, RenderResourceState::Undefined});
        const RenderPassId clearPass = executorGraph.add_pass({"Clear", RenderQueue::Graphics, {{color, RenderAccess::Write, RenderResourceState::ColorAttachment}, {depthRes, RenderAccess::Write, RenderResourceState::DepthAttachment}}, true});
        const RenderPassId lightPass = executorGraph.add_pass({"Lighting", RenderQueue::Graphics, {{color, RenderAccess::Read, RenderResourceState::ShaderRead}, {depthRes, RenderAccess::Read, RenderResourceState::ShaderRead}}, true});
        CHECK(executorGraph.add_dependency(clearPass, lightPass));
        const RenderGraphCompileResult compiled = executorGraph.compile();
        CHECK(compiled);
        CHECK(compiled.order.size() == 2);
        CHECK(compiled.barriers.size() >= 1);
        CHECK(!compiled.lifetimes.empty());

        // VulkanRenderGraphExecutor: without a real VkDevice, initialize fails
        // gracefully and reports an error (device-dependent path is validated
        // in the editor/game with a live device).
        VulkanRenderGraphExecutor executor;
        std::string execError;
        const bool initOk = executor.initialize(VK_NULL_HANDLE, executorGraph, &execError);
        CHECK(!initOk);
        CHECK(!execError.empty());
    }

    // FileWatcher: detects content changes for hot reload.
    {
        const std::filesystem::path tmpDir = std::filesystem::temp_directory_path() /
            ("vc_watcher_test_" + std::to_string(_getpid()));
        std::filesystem::remove_all(tmpDir);
        std::filesystem::create_directories(tmpDir);
        const std::filesystem::path file = tmpDir / "material.txt";
        {
            std::ofstream out(file);
            out << "version-one-content";
        }
        FileWatcher watcher(std::chrono::milliseconds(10));
        watcher.watch(file);
        CHECK(watcher.watched_count() == 1);
        CHECK(watcher.poll().empty());
        // Touch the file with different content → poll reports it.
        {
            std::ofstream out(file);
            out << "version-two-content-with-more-bytes";
        }
        const auto changed = watcher.poll();
        CHECK(changed.size() == 1 && changed[0] == file);
        CHECK(watcher.poll().empty()); // stable again
        watcher.unwatch(file);
        CHECK(watcher.watched_count() == 0);
        std::filesystem::remove_all(tmpDir);
    }

    return EXIT_SUCCESS;
}
