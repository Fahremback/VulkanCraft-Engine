// EditorInternalHelpers.cpp
// Shared implementations extracted from the monolithic EditorApplication.cpp split.
// All functions were originally file-scope anonymous-namespace helpers; now they
// live here with external linkage under namespace Engine.

#include "EditorInternalHelpers.hpp"
#include "BlockTextureAtlas.hpp"

// build_cube is implemented in EditorApplicationPanels.cpp; this declaration
// is exposed by EditorInternalHelpers.hpp so the current split stays modular.
#include <cmath>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <system_error>

namespace Engine {

// ── Transform ─────────────────────────────────────────────────────────────

glm::mat4 model_from_transform(const TransformComponent& t) {
    const auto finite = [](float v) { return std::isfinite(v); };
    if (!finite(t.position.x) || !finite(t.position.y) || !finite(t.position.z) ||
        !finite(t.rotation.x) || !finite(t.rotation.y) || !finite(t.rotation.z) ||
        !finite(t.scale.x) || !finite(t.scale.y) || !finite(t.scale.z)) {
        return glm::mat4(1.0f);
    }
    glm::mat4 model(1.0f);
    model = glm::translate(model, t.position);
    model = glm::rotate(model, glm::radians(t.rotation.z), glm::vec3(0, 0, 1));
    model = glm::rotate(model, glm::radians(t.rotation.y), glm::vec3(0, 1, 0));
    model = glm::rotate(model, glm::radians(t.rotation.x), glm::vec3(1, 0, 0));
    model = glm::scale(model, t.scale);
    return model;
}

glm::mat3 rotation_axis_from_y(const glm::vec3& axis) {
    const glm::vec3 y(0, 1, 0);
    if (glm::length(glm::cross(y, axis)) < 1e-5f) {
        return axis.y > 0 ? glm::mat3(1.0f) : glm::mat3(glm::vec3(1, 0, 0), glm::vec3(0, -1, 0), glm::vec3(0, 0, 1));
    }
    const float angle = std::acos(glm::clamp(glm::dot(y, axis), -1.0f, 1.0f));
    return glm::mat3(glm::rotate(glm::mat4(1.0f), angle, glm::normalize(glm::cross(y, axis))));
}

// ── Vulkan helpers ────────────────────────────────────────────────────────

bool safe_vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
                      VkDeviceSize size, VkFlags flags, void** ppData) {
    *ppData = nullptr;
    const VkResult result = vkMapMemory(device, memory, offset, size, flags, ppData);
    if (result != VK_SUCCESS) {
        std::cerr << "[Vulkan] vkMapMemory failed (" << result << ")" << std::endl;
        return false;
    }
    return true;
}

bool safe_map_and_copy(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
                       VkDeviceSize size, const void* source) {
    void* data = nullptr;
    if (!safe_vkMapMemory(device, memory, offset, size, 0, &data)) return false;
    std::memcpy(data, source, static_cast<size_t>(size));
    vkUnmapMemory(device, memory);
    return true;
}

void push_constants(VkCommandBuffer cmd, VkPipelineLayout layout, const glm::mat4& mvp,
                    const glm::vec4& color, const glm::mat4& model) {
    const ScenePushConstants pc{ mvp, color, g_fogParams, g_fogColor, model };
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       static_cast<uint32_t>(sizeof(pc)), &pc);
}

void set_viewport_scissor(VkCommandBuffer cmd, uint32_t w, uint32_t h) {
    VkViewport viewport{ 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{ { 0, 0 }, { w, h } };
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

void set_viewport_scissor_offset(VkCommandBuffer cmd, float offsetX, float offsetY, uint32_t w, uint32_t h) {
    VkViewport viewport{ offsetX, offsetY, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{ { static_cast<int32_t>(offsetX), static_cast<int32_t>(offsetY) }, { w, h } };
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

void draw_indexed_cube(VkCommandBuffer cmd, VkPipelineLayout layout, const VkBuffer& vb, const VkBuffer& ib,
                       uint32_t indexCount, const glm::mat4& mvp, const glm::vec4& color,
                       const glm::mat4& model) {
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
    vkCmdBindIndexBuffer(cmd, ib, 0, VK_INDEX_TYPE_UINT32);
    push_constants(cmd, layout, mvp, color, model);
    vkCmdDrawIndexed(cmd, indexCount, 1, 0, 0, 0);
}

void draw_indexed_editor_mesh(VkCommandBuffer cmd, VkPipelineLayout layout, const VkBuffer& vb,
                              const VkBuffer& ib, uint32_t indexCount, const glm::mat4& mvp,
                              const glm::vec4& color, const glm::mat4& model) {
    draw_indexed_cube(cmd, layout, vb, ib, indexCount, mvp, color, model);
}

// ── Gizmo geometry builders ───────────────────────────────────────────────

std::pair<uint32_t, uint32_t> append_transformed_cube(std::vector<EditorVertex>& verts,
                                                      std::vector<uint32_t>& indices,
                                                      const glm::mat4& model, const glm::vec3& color) {
    std::vector<EditorVertex> cubeVerts;
    std::vector<uint32_t> cubeIndices;
    build_cube(cubeVerts, cubeIndices);
    const uint32_t base = static_cast<uint32_t>(verts.size());
    for (const EditorVertex& v : cubeVerts) {
        EditorVertex out;
        out.pos = glm::vec3(model * glm::vec4(v.pos, 1.0f));
        out.normal = glm::normalize(glm::mat3(model) * v.normal);
        out.color = color;
        verts.push_back(out);
    }
    const uint32_t indexBase = static_cast<uint32_t>(indices.size());
    for (uint32_t i : cubeIndices) indices.push_back(base + i);
    return { indexBase, static_cast<uint32_t>(cubeIndices.size()) };
}

std::pair<uint32_t, uint32_t> append_cone(std::vector<EditorVertex>& verts, std::vector<uint32_t>& indices,
                                          float baseY, float tipY, float radius, int segments,
                                          const glm::mat3& rot, const glm::vec3& color) {
    const uint32_t base = static_cast<uint32_t>(verts.size());
    const float step = glm::two_pi<float>() / static_cast<float>(segments);
    EditorVertex apex;
    apex.pos = glm::vec3(rot * glm::vec3(0.0f, tipY, 0.0f));
    apex.normal = glm::normalize(rot * glm::vec3(0.0f, 1.0f, 0.0f));
    apex.color = color;
    verts.push_back(apex);
    std::vector<uint32_t> ring;
    for (int s = 0; s < segments; ++s) {
        const float a = step * static_cast<float>(s);
        const glm::vec3 local(std::cos(a) * radius, baseY, std::sin(a) * radius);
        EditorVertex v;
        v.pos = glm::vec3(rot * local);
        const glm::vec3 toApex = glm::normalize(glm::vec3(0.0f, tipY, 0.0f) - local);
        const glm::vec3 tangent(std::sin(a), 0.0f, -std::cos(a));
        v.normal = glm::normalize(rot * glm::normalize(glm::cross(tangent, toApex)));
        v.color = color;
        verts.push_back(v);
        ring.push_back(static_cast<uint32_t>(verts.size()) - 1);
    }
    const uint32_t indexBase = static_cast<uint32_t>(indices.size());
    for (int s = 0; s < segments; ++s) {
        const uint32_t next = ring[(s + 1) % segments];
        indices.push_back(base);
        indices.push_back(ring[s]);
        indices.push_back(next);
    }
    return { indexBase, static_cast<uint32_t>(segments) * 3u };
}

std::pair<uint32_t, uint32_t> append_ring(std::vector<EditorVertex>& verts, std::vector<uint32_t>& indices,
                                          const glm::vec3& axis, float radius, int segments,
                                          const glm::vec3& color) {
    const uint32_t base = static_cast<uint32_t>(verts.size());
    glm::vec3 u = glm::normalize(glm::cross(axis, glm::abs(axis.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0)));
    glm::vec3 v = glm::normalize(glm::cross(axis, u));
    for (int s = 0; s < segments; ++s) {
        const float a = glm::two_pi<float>() * static_cast<float>(s) / static_cast<float>(segments);
        EditorVertex vert;
        vert.pos = u * (std::cos(a) * radius) + v * (std::sin(a) * radius);
        vert.normal = axis;
        vert.color = color;
        verts.push_back(vert);
    }
    const uint32_t indexBase = static_cast<uint32_t>(indices.size());
    for (int s = 0; s < segments; ++s) {
        indices.push_back(base + static_cast<uint32_t>(s));
        indices.push_back(base + static_cast<uint32_t>((s + 1) % segments));
    }
    return { indexBase, static_cast<uint32_t>(segments) * 2u };
}

// ── Material UBO layout ───────────────────────────────────────────────────

size_t material_std140_size(Rendering::MaterialValueType type) {
    switch (type) {
        case Rendering::MaterialValueType::Bool: return 4;
        case Rendering::MaterialValueType::Float: return 4;
        case Rendering::MaterialValueType::Vec2: return 8;
        case Rendering::MaterialValueType::Vec3: return 12;
        case Rendering::MaterialValueType::Vec4: return 16;
        case Rendering::MaterialValueType::Texture2D: return 16;
    }
    return 4;
}

size_t material_std140_alignment(Rendering::MaterialValueType type) {
    switch (type) {
        case Rendering::MaterialValueType::Bool: return 4;
        case Rendering::MaterialValueType::Float: return 4;
        case Rendering::MaterialValueType::Vec2: return 8;
        case Rendering::MaterialValueType::Vec3: return 16;
        case Rendering::MaterialValueType::Vec4: return 16;
        case Rendering::MaterialValueType::Texture2D: return 16;
    }
    return 4;
}

size_t align_material_offset(size_t offset, size_t alignment) {
    return (offset + alignment - 1) / alignment * alignment;
}

void write_ubo_value(std::byte* dst, Rendering::MaterialValueType type, const Rendering::MaterialValue& value) {
    std::memset(dst, 0, material_std140_size(type));
    switch (type) {
        case Rendering::MaterialValueType::Bool:
            *reinterpret_cast<uint32_t*>(dst) = std::holds_alternative<bool>(value) && std::get<bool>(value) ? 1u : 0u;
            break;
        case Rendering::MaterialValueType::Float:
            if (std::holds_alternative<float>(value)) *reinterpret_cast<float*>(dst) = std::get<float>(value);
            break;
        case Rendering::MaterialValueType::Vec2:
            if (std::holds_alternative<glm::vec2>(value)) {
                const glm::vec2 v = std::get<glm::vec2>(value);
                std::memcpy(dst, &v, sizeof(v));
            }
            break;
        case Rendering::MaterialValueType::Vec3:
            if (std::holds_alternative<glm::vec3>(value)) {
                const glm::vec3 v = std::get<glm::vec3>(value);
                std::memcpy(dst, &v, sizeof(v));
            }
            break;
        case Rendering::MaterialValueType::Vec4:
            if (std::holds_alternative<glm::vec4>(value)) {
                const glm::vec4 v = std::get<glm::vec4>(value);
                std::memcpy(dst, &v, sizeof(v));
            }
            break;
        case Rendering::MaterialValueType::Texture2D:
            break;
    }
}

// ── Material-graph helpers ────────────────────────────────────────────────

uint64_t hash_material_graph(const Rendering::MaterialGraph& graph) {
    uint64_t h = 14695981039346656037ull;
    const auto mix = [&h](const void* data, size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; ++i) {
            h ^= bytes[i];
            h *= 1099511628211ull;
        }
    };
    for (const auto& node : graph.nodes()) {
        mix(&node.id, sizeof(node.id));
        const auto kind = static_cast<uint8_t>(node.kind);
        mix(&kind, 1);
        const auto outputType = static_cast<uint8_t>(node.outputType);
        mix(&outputType, 1);
        mix(node.label.data(), node.label.size());
        mix(node.parameter.data(), node.parameter.size());
        std::visit([&](const auto& v) {
            if constexpr (std::is_same_v<std::decay_t<decltype(v)>, std::string>) {
                mix(v.data(), v.size());
            } else {
                mix(&v, sizeof(v));
            }
        }, node.value);
    }
    for (const auto& p : graph.parameters()) {
        mix(p.name.data(), p.name.size());
        const auto type = static_cast<uint8_t>(p.type);
        mix(&type, 1);
        mix(&p.exposed, 1);
    }
    return h;
}

Rendering::MaterialGraph material_graph_from_asset(const MaterialAsset& mat) {
    Rendering::MaterialGraph graph;
    graph.define_parameter({ "Albedo", Rendering::MaterialValueType::Vec3, mat.albedo, true });
    graph.define_parameter({ "Roughness", Rendering::MaterialValueType::Float, mat.roughness, true });
    graph.define_parameter({ "Metallic", Rendering::MaterialValueType::Float, mat.metallic, true });
    graph.define_parameter({ "Emissive", Rendering::MaterialValueType::Vec3,
                             mat.emissiveColor * mat.emissiveIntensity, true });
    const auto roughness = graph.add_parameter("Roughness");
    const auto metallic = graph.add_parameter("Metallic");
    const auto emissive = graph.add_parameter("Emissive");
    const auto baseOut = graph.add_output("BaseColor", Rendering::MaterialValueType::Vec3);
    const auto roughOut = graph.add_output("Roughness", Rendering::MaterialValueType::Float);
    const auto metalOut = graph.add_output("Metallic", Rendering::MaterialValueType::Float);
    const auto emisOut = graph.add_output("Emissive", Rendering::MaterialValueType::Vec3);
    if (mat.albedoMapID.is_valid()) {
        const auto tex = graph.add_texture_sample("Albedo Map");
        if (auto* node = graph.find_node(tex)) node->value = mat.albedoMapID.to_string();
        (void)graph.connect(tex, baseOut, 0);
    } else {
        const auto albedo = graph.add_parameter("Albedo");
        (void)graph.connect(albedo, baseOut, 0);
    }
    (void)graph.connect(roughness, roughOut, 0);
    (void)graph.connect(metallic, metalOut, 0);
    (void)graph.connect(emissive, emisOut, 0);
    return graph;
}

} // namespace Engine
