#include "EditorApplication.hpp"
#include "frontend/FontAwesomeV6.h"
#include "frontend/IconsFontAwesome6.h"
#include "frontend/liberation_sans.h"
#include "frontend/ForgeTheme.hpp"
#include "frontend/ForgeWidgets.hpp"
#include "engine/compression/ICompressionProvider.hpp"
#include "../engine/assets/GltfGeometry.hpp"
#include "../engine/animation/AnimationAssets.hpp"
#include "../engine/rendering/vulkan/MaterialPipeline.hpp"
#include "../engine/audio/AudioRuntime.hpp"
#include "../engine/audio/OggDecoder.hpp"
#include "../engine/gameplay/DialogueSystem.hpp"
#include "../engine/gameplay/DestructionRuntime.hpp"
#include "../engine/gameplay/MissionSystem.hpp"
#include "engine/navigation/INavigationProvider.hpp"
#include <array>
#include <random>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <filesystem>
#include <iostream>
#include <type_traits>
#include <stdexcept>
#include <algorithm>
#include <functional>
#include <unordered_set>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <fstream>
#include <chrono>
#include <ctime>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <VkBootstrap.h>
#include <miniaudio.h>
#include <thread>

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

glm::mat4 model_from_transform(const Engine::TransformComponent& t) {
    glm::mat4 model(1.0f);
    model = glm::translate(model, t.position);
    model = glm::rotate(model, glm::radians(t.rotation.z), glm::vec3(0, 0, 1));
    model = glm::rotate(model, glm::radians(t.rotation.y), glm::vec3(0, 1, 0));
    model = glm::rotate(model, glm::radians(t.rotation.x), glm::vec3(1, 0, 0));
    model = glm::scale(model, t.scale);
    return model;
}
void push_constants(VkCommandBuffer cmd, VkPipelineLayout layout, const glm::mat4& mvp, const glm::vec4& color) {
    const Engine::ScenePushConstants pc{ mvp, color };
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       static_cast<uint32_t>(sizeof(pc)), &pc);
}
void set_viewport_scissor(VkCommandBuffer cmd, uint32_t w, uint32_t h) {
    VkViewport viewport{ 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{ { 0, 0 }, { w, h } };
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}
void draw_indexed_cube(VkCommandBuffer cmd, VkPipelineLayout layout, const VkBuffer& vb, const VkBuffer& ib,
                       uint32_t indexCount, const glm::mat4& mvp, const glm::vec4& color) {
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
    vkCmdBindIndexBuffer(cmd, ib, 0, VK_INDEX_TYPE_UINT32);
    push_constants(cmd, layout, mvp, color);
    vkCmdDrawIndexed(cmd, indexCount, 1, 0, 0, 0);
}
void draw_indexed_editor_mesh(VkCommandBuffer cmd, VkPipelineLayout layout, const VkBuffer& vb,
                              const VkBuffer& ib, uint32_t indexCount, const glm::mat4& mvp,
                              const glm::vec4& color) {
    draw_indexed_cube(cmd, layout, vb, ib, indexCount, mvp, color);
}
} // namespace

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <wincodec.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
#endif

namespace Engine {

// ===========================================================================
// Assets, Materials, Thumbnails, Block/Character (split from EditorApplication.cpp)
// ===========================================================================
bool EditorApplication::load_mesh_resource(const UUID& assetId) {
    const auto cached = m_meshResources.find(assetId);
    if (cached != m_meshResources.end()) return cached->second.valid;
    if (m_meshLoadFailed.contains(assetId)) return false;

    const auto found = m_assetRegistry.find(assetId);
    if (!found || found->type != AssetType::Mesh || !found->isCooked ||
        found->cookedPath.empty() || !std::filesystem::is_regular_file(found->cookedPath)) {
        m_meshLoadFailed.insert(assetId);
        return false;
    }
    std::string error;
    const GltfGeometryResult geometry = GltfGeometryParser::parse_vcmesh(found->cookedPath, &error);
    if (!geometry.success || geometry.primitives.empty()) {
        std::cerr << "[Editor] Cannot load mesh " << assetId.to_string() << ": " << error << std::endl;
        m_meshLoadFailed.insert(assetId);
        return false;
    }
    const float meshScale = found->importSettings.meshScale > 0.0f ? found->importSettings.meshScale : 1.0f;

    std::vector<EditorVertex> verts;
    std::vector<uint32_t> indices;
    EditorMeshResource resource;
    for (const GltfMeshPrimitive& primitive : geometry.primitives) {
        const uint32_t vertexOffset = static_cast<uint32_t>(verts.size());
        verts.reserve(verts.size() + primitive.positions.size());
        for (size_t i = 0; i < primitive.positions.size(); ++i) {
            EditorVertex v;
            v.pos = primitive.positions[i] * meshScale;
            v.normal = i < primitive.normals.size() ? primitive.normals[i] : glm::vec3(0.0f, 1.0f, 0.0f);
            v.color = glm::vec3(1.0f);
            v.uv = i < primitive.uvs.size() ? primitive.uvs[i] : glm::vec2(0.0f);
            verts.push_back(v);
            if (!resource.hasBounds) {
                resource.boundsMin = resource.boundsMax = v.pos;
                resource.hasBounds = true;
            } else {
                resource.boundsMin = glm::min(resource.boundsMin, v.pos);
                resource.boundsMax = glm::max(resource.boundsMax, v.pos);
            }
        }
        if (primitive.indexed) {
            const uint32_t firstIndex = static_cast<uint32_t>(indices.size());
            for (uint32_t index : primitive.indices) indices.push_back(index + vertexOffset);
            resource.ranges.push_back({ firstIndex, static_cast<uint32_t>(primitive.indices.size()), 0, true });
        } else {
            resource.ranges.push_back({ 0, static_cast<uint32_t>(primitive.positions.size()), vertexOffset, false });
        }
    }
    resource.vertexCount = static_cast<uint32_t>(verts.size());
    resource.valid = true;
    resource.cpuPositions.reserve(verts.size());
    for (const EditorVertex& v : verts) resource.cpuPositions.push_back(v.pos);

    const VkDeviceSize vbSize = sizeof(EditorVertex) * verts.size();
    const VkDeviceSize ibSize = indices.empty() ? 0 : sizeof(uint32_t) * indices.size();
    create_buffer(vbSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  resource.vb.buffer, resource.vb.memory);
    if (ibSize > 0) {
        create_buffer(ibSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      resource.ib.buffer, resource.ib.memory);
    }
    void* data = nullptr;
    vkMapMemory(m_device, resource.vb.memory, 0, vbSize, 0, &data);
    std::memcpy(data, verts.data(), static_cast<size_t>(vbSize));
    vkUnmapMemory(m_device, resource.vb.memory);
    if (ibSize > 0) {
        vkMapMemory(m_device, resource.ib.memory, 0, ibSize, 0, &data);
        std::memcpy(data, indices.data(), static_cast<size_t>(ibSize));
        vkUnmapMemory(m_device, resource.ib.memory);
    }
    resource.cpuIndices = std::move(indices);

    m_meshResources[assetId] = std::move(resource);
    return true;
}

const EditorApplication::EditorMeshResource* EditorApplication::get_mesh_resource(const UUID& assetId) {
    if (!assetId.is_valid()) return nullptr;
    // Block models: a Block asset is a textured cube — build the GPU mesh on
    // demand so spawned block entities survive restarts (no asset file, the
    // cube geometry is generated and uploaded here).
    if (const auto blockFound = m_assetRegistry.find(assetId);
        blockFound && blockFound->type == AssetType::Block) {
        ensure_block_cube_resource(assetId);
        const auto it = m_meshResources.find(assetId);
        return (it != m_meshResources.end() && it->second.valid) ? &it->second : nullptr;
    }
    // Minecraft character/mob skins: the texture IS the character. The
    // humanoid mesh is generated from the skin's UV layout (64x64 or legacy
    // 64x32) and cached per texture UUID, same on-demand pattern as blocks.
    if (const auto skinFound = m_assetRegistry.find(assetId);
        skinFound && skinFound->type == AssetType::Texture && is_character_texture(*skinFound)) {
        ensure_character_mesh_resource(assetId);
        const auto it = m_meshResources.find(assetId);
        return (it != m_meshResources.end() && it->second.valid) ? &it->second : nullptr;
    }
    if (!load_mesh_resource(assetId)) return nullptr;
    const auto found = m_meshResources.find(assetId);
    return (found != m_meshResources.end() && found->second.valid) ? &found->second : nullptr;
}

void EditorApplication::ensure_block_cube_resource(const UUID& blockId) {
    const auto cached = m_meshResources.find(blockId);
    if (cached != m_meshResources.end()) {
        if (cached->second.valid) return;
        if (cached->second.vb.buffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, cached->second.vb.buffer, nullptr);
        if (cached->second.vb.memory != VK_NULL_HANDLE) vkFreeMemory(m_device, cached->second.vb.memory, nullptr);
        if (cached->second.ib.buffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, cached->second.ib.buffer, nullptr);
        if (cached->second.ib.memory != VK_NULL_HANDLE) vkFreeMemory(m_device, cached->second.ib.memory, nullptr);
        m_meshResources.erase(cached);
    }
    std::vector<EditorVertex> verts;
    std::vector<uint32_t> indices;
    generate_cube_geometry(verts, indices);
    EditorMeshResource cube;
    cube.vertexCount = static_cast<uint32_t>(verts.size());
    cube.ranges.push_back({ 0, static_cast<uint32_t>(indices.size()), 0, true });
    const VkDeviceSize vbSize = sizeof(EditorVertex) * verts.size();
    const VkDeviceSize ibSize = sizeof(uint32_t) * indices.size();
    create_buffer(vbSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  cube.vb.buffer, cube.vb.memory);
    create_buffer(ibSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  cube.ib.buffer, cube.ib.memory);
    void* data = nullptr;
    vkMapMemory(m_device, cube.vb.memory, 0, vbSize, 0, &data);
    std::memcpy(data, verts.data(), static_cast<size_t>(vbSize));
    vkUnmapMemory(m_device, cube.vb.memory);
    vkMapMemory(m_device, cube.ib.memory, 0, ibSize, 0, &data);
    std::memcpy(data, indices.data(), static_cast<size_t>(ibSize));
    vkUnmapMemory(m_device, cube.ib.memory);
    cube.valid = true;
    m_meshResources[blockId] = std::move(cube);
}

// GPU mesh for a Minecraft-style character: the humanoid (head/body/arms/legs
// boxes UV-mapped to the standard skin layout) built from skinHeight (64 for
// 64x64/HD skins, 32 for legacy 64x32) and uploaded on demand.
void EditorApplication::ensure_character_mesh_resource(const UUID& texId) {
    const auto cached = m_meshResources.find(texId);
    if (cached != m_meshResources.end()) {
        if (cached->second.valid) return;
        if (cached->second.vb.buffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, cached->second.vb.buffer, nullptr);
        if (cached->second.vb.memory != VK_NULL_HANDLE) vkFreeMemory(m_device, cached->second.vb.memory, nullptr);
        if (cached->second.ib.buffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, cached->second.ib.buffer, nullptr);
        if (cached->second.ib.memory != VK_NULL_HANDLE) vkFreeMemory(m_device, cached->second.ib.memory, nullptr);
        m_meshResources.erase(cached);
    }
    float skinHeight = 64.0f;
    if (const auto meta = m_assetRegistry.find(texId); meta && meta->height > 0) {
        skinHeight = static_cast<float>(meta->height);
    }
    std::vector<EditorVertex> verts;
    std::vector<uint32_t> indices;
    build_character_geometry(skinHeight, verts, indices);
    EditorMeshResource character;
    character.vertexCount = static_cast<uint32_t>(verts.size());
    character.ranges.push_back({ 0, static_cast<uint32_t>(indices.size()), 0, true });
    const VkDeviceSize vbSize = sizeof(EditorVertex) * verts.size();
    const VkDeviceSize ibSize = sizeof(uint32_t) * indices.size();
    create_buffer(vbSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  character.vb.buffer, character.vb.memory);
    create_buffer(ibSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  character.ib.buffer, character.ib.memory);
    void* data = nullptr;
    vkMapMemory(m_device, character.vb.memory, 0, vbSize, 0, &data);
    std::memcpy(data, verts.data(), static_cast<size_t>(vbSize));
    vkUnmapMemory(m_device, character.vb.memory);
    vkMapMemory(m_device, character.ib.memory, 0, ibSize, 0, &data);
    std::memcpy(data, indices.data(), static_cast<size_t>(ibSize));
    vkUnmapMemory(m_device, character.ib.memory);
    character.valid = true;
    m_meshResources[texId] = std::move(character);
}

// A Minecraft character/mob skin becomes a humanoid entity in the scene: the
// MeshRenderer references the texture asset directly (the renderer builds the
// humanoid mesh + skin pipeline on demand), so there is no sidecar file and no
// duplicate asset in the browser.
void EditorApplication::spawn_character_entity(const UUID& texId, const glm::vec3& position) {
    if (!m_editorScene) return;
    const auto meta = m_assetRegistry.find(texId);
    if (!meta || meta->type != AssetType::Texture) return;
    Entity e = m_editorScene->create_entity(meta->sourcePath.stem().string());
    m_editorScene->transformComponents[e.get_id()].position = position;
    m_editorScene->meshRendererComponents[e.get_id()] =
        MeshRendererComponent{ texId, UUID{ 0, 0 }, true, true };
    m_selectedEntity = e;
    m_editorGui.select_entity(e);
}

// Shared material-graph pipeline that samples one texture (block faces and
// character skins both land here). Cached per texture UUID so two blocks that
// share a texture reuse the same pipeline; rebuilt when the graph hash changes.
EditorApplication::GraphMaterialPipeline* EditorApplication::ensure_texture_pipeline(
    const UUID& texId, std::unordered_map<UUID, GraphMaterialPipeline>& cache) {
    if (!texId.is_valid()) return nullptr;
    auto it = cache.find(texId);
    Rendering::MaterialGraph graph;
    const auto texNode = graph.add_texture_sample("Texture");
    if (auto* node = graph.find_node(texNode)) node->value = texId.to_string();
    const auto baseOut = graph.add_output("BaseColor", Rendering::MaterialValueType::Vec3);
    (void)graph.connect(texNode, baseOut, 0);
    const uint64_t graphHash = hash_material_graph(graph);
    if (it == cache.end() || !it->second.valid || it->second.graphHash != graphHash) {
        if (it != cache.end()) destroy_graph_pipeline(it->second);
        GraphMaterialPipeline built;
        built.graphHash = graphHash;
        if (!build_graph_pipeline(graph, built)) {
            std::cerr << "[Editor] Texture pipeline: " << built.lastError << std::endl;
        }
        it = cache.insert_or_assign(texId, std::move(built)).first;
    }
    return it->second.valid ? &it->second : nullptr;
}

void EditorApplication::spawn_block_entity(const UUID& blockId, const glm::vec3& position) {
    if (!m_editorScene) return;
    const auto meta = m_assetRegistry.find(blockId);
    if (!meta || meta->type != AssetType::Block) return;
    Entity e = m_editorScene->create_entity(meta->sourcePath.stem().string());
    m_editorScene->transformComponents[e.get_id()].position = position;
    // meshAssetID = the block asset: the renderer builds the textured cube on
    // demand (see get_mesh_resource / the block material branch in the mesh
    // draw loop). Persists with the scene; regenerated after restart.
    m_editorScene->meshRendererComponents[e.get_id()] =
        MeshRendererComponent{ blockId, UUID{ 0, 0 }, true, true };
    m_selectedEntity = e;
    m_editorGui.select_entity(e);
}void EditorApplication::draw_mesh_resource(VkCommandBuffer cmd, const glm::mat4& mvp, const glm::vec4& color,
                                           const EditorMeshResource& resource) {
    if (!resource.valid || resource.vb.buffer == VK_NULL_HANDLE) return;
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &resource.vb.buffer, &offset);
    if (resource.ib.buffer != VK_NULL_HANDLE) {
        vkCmdBindIndexBuffer(cmd, resource.ib.buffer, 0, VK_INDEX_TYPE_UINT32);
    }
    push_constants(cmd, m_scenePipelineLayout, mvp, color);
    for (const EditorMeshResource::DrawRange& range : resource.ranges) {
        if (range.indexed) {
            vkCmdDrawIndexed(cmd, range.indexCount, 1, range.firstIndex, 0, 0);
        } else {
            vkCmdDraw(cmd, range.indexCount, 1, range.vertexOffset, 0);
        }
    }
}

bool EditorApplication::load_material_asset(const UUID& assetId) {
    if (!assetId.is_valid()) return false;
    if (m_materialAssets.contains(assetId)) return true;
    if (m_materialLoadFailed.contains(assetId)) return false;
    const auto found = m_assetRegistry.find(assetId);
    if (!found || found->type != AssetType::Material || found->sourcePath.empty() ||
        !std::filesystem::is_regular_file(found->sourcePath)) {
        m_materialLoadFailed.insert(assetId);
        return false;
    }
    MaterialAsset mat;
    if (!mat.load_from_file(found->sourcePath)) {
        std::cerr << "[Editor] Cannot load material asset " << assetId.to_string() << std::endl;
        m_materialLoadFailed.insert(assetId);
        return false;
    }
    m_materialAssets[assetId] = std::move(mat);
    return true;
}

namespace {
// Decode a PNG payload via Windows Imaging Component into 8-bit RGBA.
bool decode_png_rgba(const std::vector<uint8_t>& png, std::vector<uint8_t>& rgba) {
    if (png.size() < 8) return false;
    static ComPtr<IWICImagingFactory> factory;
    if (!factory) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(factory.ReleaseAndGetAddressOf())))) return false;
    }
    HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, png.size());
    if (!hGlobal) return false;
    void* dst = GlobalLock(hGlobal);
    if (!dst) { GlobalFree(hGlobal); return false; }
    std::memcpy(dst, png.data(), png.size());
    GlobalUnlock(hGlobal);
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(hGlobal, TRUE, stream.ReleaseAndGetAddressOf()))) return false;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) ||
        FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
        return false;
    UINT width = 0, height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0) return false;
    rgba.resize(static_cast<size_t>(width) * height * 4);
    return SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(rgba.size()), rgba.data()));
}

// Box-downscale a single-level RGBA8 image so its longest side fits within
// maxDim (aspect preserved). When already small enough, dst is left untouched
// and the caller keeps the original (outW/outH are still set).
void downscale_rgba8(const uint8_t* src, uint32_t w, uint32_t h, uint32_t maxDim,
                     std::vector<uint8_t>& dst, uint32_t& outW, uint32_t& outH) {
    const uint32_t longest = std::max(w, h);
    if (longest <= maxDim) {
        outW = w;
        outH = h;
        return;
    }
    outW = std::max(1u, static_cast<uint32_t>((static_cast<uint64_t>(w) * maxDim) / longest));
    outH = std::max(1u, static_cast<uint32_t>((static_cast<uint64_t>(h) * maxDim) / longest));
    dst.assign(static_cast<size_t>(outW) * outH * 4, 0);
    for (uint32_t y = 0; y < outH; ++y) {
        const uint32_t y0 = static_cast<uint32_t>((static_cast<uint64_t>(y) * h) / outH);
        const uint32_t y1 = std::max(static_cast<uint32_t>((static_cast<uint64_t>(y + 1) * h) / outH), y0 + 1);
        for (uint32_t x = 0; x < outW; ++x) {
            const uint32_t x0 = static_cast<uint32_t>((static_cast<uint64_t>(x) * w) / outW);
            const uint32_t x1 = std::max(static_cast<uint32_t>((static_cast<uint64_t>(x + 1) * w) / outW), x0 + 1);
            uint64_t acc[4] = { 0, 0, 0, 0 };
            for (uint32_t sy = y0; sy < y1; ++sy) {
                const uint8_t* row = src + static_cast<size_t>(sy) * w * 4;
                for (uint32_t sx = x0; sx < x1; ++sx) {
                    const uint8_t* p = row + static_cast<size_t>(sx) * 4;
                    acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; acc[3] += p[3];
                }
            }
            const uint32_t n = (y1 - y0) * (x1 - x0);
            uint8_t* d = dst.data() + (static_cast<size_t>(y) * outW + x) * 4;
            d[0] = static_cast<uint8_t>(acc[0] / n); d[1] = static_cast<uint8_t>(acc[1] / n);
            d[2] = static_cast<uint8_t>(acc[2] / n); d[3] = static_cast<uint8_t>(acc[3] / n);
        }
    }
}

float half_to_float(uint16_t h) {
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1Fu;
    const uint32_t mant = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else {
            int e = -14;
            uint32_t m = mant;
            while ((m & 0x400u) == 0) { m <<= 1; --e; }
            m &= 0x3FFu;
            bits = sign | (static_cast<uint32_t>(e + 127) << 23) | (m << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

uint16_t float_to_half(float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const int32_t exp = static_cast<int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
    const uint32_t mant = bits & 0x7FFFFFu;
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7C00u);
    if (exp <= 0) {
        if (exp < -10) return static_cast<uint16_t>(sign);
        const uint32_t m = mant | 0x800000u;
        const uint32_t shift = static_cast<uint32_t>(14 - exp);
        const uint32_t rounded = (m >> shift) + 0x1FFu + ((m >> (shift + 1)) & 1u);
        return static_cast<uint16_t>(sign | (rounded >> 13));
    }
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) | (mant >> 13));
}

// Same box downscale for RGBA16F (HDR) thumbnails.
void downscale_half4(const uint8_t* src, uint32_t w, uint32_t h, uint32_t maxDim,
                     std::vector<uint8_t>& dst, uint32_t& outW, uint32_t& outH) {
    const uint32_t longest = std::max(w, h);
    if (longest <= maxDim) {
        outW = w;
        outH = h;
        return;
    }
    outW = std::max(1u, static_cast<uint32_t>((static_cast<uint64_t>(w) * maxDim) / longest));
    outH = std::max(1u, static_cast<uint32_t>((static_cast<uint64_t>(h) * maxDim) / longest));
    dst.assign(static_cast<size_t>(outW) * outH * 8, 0);
    for (uint32_t y = 0; y < outH; ++y) {
        const uint32_t y0 = static_cast<uint32_t>((static_cast<uint64_t>(y) * h) / outH);
        const uint32_t y1 = std::max(static_cast<uint32_t>((static_cast<uint64_t>(y + 1) * h) / outH), y0 + 1);
        for (uint32_t x = 0; x < outW; ++x) {
            const uint32_t x0 = static_cast<uint32_t>((static_cast<uint64_t>(x) * w) / outW);
            const uint32_t x1 = std::max(static_cast<uint32_t>((static_cast<uint64_t>(x + 1) * w) / outW), x0 + 1);
            float acc[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            for (uint32_t sy = y0; sy < y1; ++sy) {
                const uint16_t* row = reinterpret_cast<const uint16_t*>(src + static_cast<size_t>(sy) * w * 8);
                for (uint32_t sx = x0; sx < x1; ++sx) {
                    const uint16_t* p = row + static_cast<size_t>(sx) * 4;
                    acc[0] += half_to_float(p[0]); acc[1] += half_to_float(p[1]);
                    acc[2] += half_to_float(p[2]); acc[3] += half_to_float(p[3]);
                }
            }
            const uint32_t n = (y1 - y0) * (x1 - x0);
            uint16_t* d = reinterpret_cast<uint16_t*>(dst.data() + (static_cast<size_t>(y) * outW + x) * 8);
            const float inv = 1.0f / static_cast<float>(n);
            d[0] = float_to_half(acc[0] * inv); d[1] = float_to_half(acc[1] * inv);
            d[2] = float_to_half(acc[2] * inv); d[3] = float_to_half(acc[3] * inv);
        }
    }
}
} // namespace

// ---------------------------------------------------------------------------
// Cooked-texture CPU decode (shared by the viewport material path and the
// async Content Browser thumbnails). Pure file I/O + WIC decode + box
// downscale — safe to call from a worker thread.
// ---------------------------------------------------------------------------

struct DecodedTexturePixels {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipCount = 1;
    bool srgb = false;
    bool halfFloat = false; // true => rgba holds RGBA16F half-float pairs
    std::vector<uint8_t> rgba;
};

bool decode_cooked_texture_pixels(const std::filesystem::path& cookedPath, uint32_t maxDim,
                                  DecodedTexturePixels& out, std::string& error) {
    std::ifstream in(cookedPath, std::ios::binary);
    if (!in) {
        error = "cannot open cooked texture: " + cookedPath.string();
        return false;
    }
    std::array<char, 5> magic{};
    in.read(magic.data(), magic.size());
    uint32_t version = 0, width = 0, height = 0, channels = 0;
    uint8_t bitDepth = 0; // the importer stores bitDepth as a single byte
    uint32_t mipCount = 1; // v2 = single level; v3 reads mipCount + flags
    uint8_t flags = 0;
    uint64_t payloadSize = 0;
    in.read(reinterpret_cast<char*>(&version), sizeof(version));
    in.read(reinterpret_cast<char*>(&width), sizeof(width));
    in.read(reinterpret_cast<char*>(&height), sizeof(height));
    in.read(reinterpret_cast<char*>(&channels), sizeof(channels));
    in.read(reinterpret_cast<char*>(&bitDepth), sizeof(bitDepth));
    if (version == 2) {
        mipCount = 1;
        flags = 0;
    } else if (version == 3) {
        in.read(reinterpret_cast<char*>(&mipCount), sizeof(mipCount));
        in.read(reinterpret_cast<char*>(&flags), sizeof(flags));
    }
    in.read(reinterpret_cast<char*>(&payloadSize), sizeof(payloadSize));
    if (!in || std::string_view(magic.data(), magic.size()) != "VCTEX" ||
        (version != 2 && version != 3) || width == 0 || height == 0 || mipCount == 0 ||
        payloadSize == 0 || payloadSize > (1ull << 30)) {
        error = "invalid or unsupported VCTEX cooked texture (magic=" +
                std::string(magic.data(), magic.size()) + " version=" + std::to_string(version) +
                " size=" + std::to_string(width) + "x" + std::to_string(height) +
                " ch=" + std::to_string(channels) + " mips=" + std::to_string(mipCount) +
                " payload=" + std::to_string(payloadSize) +
                " path=" + cookedPath.string() + ")";
        return false;
    }
    std::vector<uint8_t> payload(static_cast<size_t>(payloadSize));
    in.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payloadSize));
    if (!in) {
        error = "truncated VCTEX payload";
        return false;
    }
    out.width = width;
    out.height = height;
    out.mipCount = mipCount;
    out.srgb = (flags & 1u) != 0;
    const bool isPng = payload.size() >= 8 &&
                       std::memcmp(payload.data(), "\x89PNG\r\n\x1a\n", 8) == 0;
    if (isPng) {
        if (!decode_png_rgba(payload, out.rgba)) {
            error = "PNG decode failed (WIC)";
            return false;
        }
        // PNG stays a single level (raw payload); srgb is still applied.
        // Thumbnails: box-downscale before upload so a full-res texture never
        // gets copied to VRAM just to be shown at 135x48 in the asset grid.
        out.mipCount = 1;
        if (maxDim > 0 && (out.width > maxDim || out.height > maxDim)) {
            std::vector<uint8_t> thumb;
            downscale_rgba8(out.rgba.data(), out.width, out.height, maxDim, thumb, out.width, out.height);
            out.rgba = std::move(thumb);
        }
        return true;
    }
    // TGA/HDR importers store decoded pixels in the payload. Radiance HDR
    // (bitDepth 32, channels 4) stores RGBA16F half-float pairs (w*h*8 bytes)
    // and is uploaded as an R16G16B16A16_SFLOAT image; TGA stores 8-bit
    // RGB/RGBA (w*h*3/4 bytes per level, mip chain when mipCount > 1).
    if (bitDepth == 32 && channels == 4 &&
        payload.size() == static_cast<size_t>(width) * height * 8) {
        out.halfFloat = true;
        out.mipCount = 1;
        if (maxDim > 0 && (width > maxDim || height > maxDim)) {
            uint32_t tw = width, th = height;
            downscale_half4(payload.data(), width, height, maxDim, out.rgba, tw, th);
            out.width = tw;
            out.height = th;
        } else {
            out.rgba = std::move(payload);
        }
        return true;
    }
    uint64_t expectedTotal = 0;
    for (uint32_t m = 0; m < mipCount; ++m) {
        const uint32_t mw = std::max(width >> m, 1u);
        const uint32_t mh = std::max(height >> m, 1u);
        expectedTotal += static_cast<uint64_t>(mw) * mh * channels;
    }
    if (payload.size() != expectedTotal) {
        error = "unsupported cooked texture payload layout (expected " +
                std::to_string(expectedTotal) + " bytes, got " + std::to_string(payload.size()) +
                " mips=" + std::to_string(mipCount) + ")";
        return false;
    }
    out.rgba.reserve(static_cast<size_t>(expectedTotal) / channels * 4);
    size_t offset = 0;
    for (uint32_t m = 0; m < mipCount; ++m) {
        const uint32_t mw = std::max(width >> m, 1u);
        const uint32_t mh = std::max(height >> m, 1u);
        const size_t levelBytes = static_cast<size_t>(mw) * mh * channels;
        const uint8_t* level = payload.data() + offset;
        if (channels == 4) {
            out.rgba.insert(out.rgba.end(), level, level + levelBytes);
        } else if (channels == 3) {
            for (size_t i = 0; i < levelBytes; i += 3) {
                out.rgba.push_back(level[i]);
                out.rgba.push_back(level[i + 1]);
                out.rgba.push_back(level[i + 2]);
                out.rgba.push_back(255);
            }
        } else {
            error = "unsupported cooked texture channel count";
            return false;
        }
        offset += levelBytes;
    }
    if (maxDim > 0 && (width > maxDim || height > maxDim)) {
        // Thumbnail: keep only level 0 (mip chain is irrelevant at 192 px) and
        // box-downscale it before the upload.
        std::vector<uint8_t> level0(out.rgba.begin(),
                                    out.rgba.begin() + static_cast<size_t>(width) * height * 4);
        std::vector<uint8_t> thumb;
        downscale_rgba8(level0.data(), width, height, maxDim, thumb, width, height);
        out.rgba = std::move(thumb);
        out.width = width;
        out.height = height;
        out.mipCount = 1;
    }
    return true;
}

void EditorApplication::destroy_graph_texture(GraphTexture& t) {
    if (m_device == VK_NULL_HANDLE) return;
    if (t.view != VK_NULL_HANDLE) vkDestroyImageView(m_device, t.view, nullptr);
    if (t.image != VK_NULL_HANDLE) vkDestroyImage(m_device, t.image, nullptr);
    if (t.memory != VK_NULL_HANDLE) vkFreeMemory(m_device, t.memory, nullptr);
    t = GraphTexture{};
}

// ---------------------------------------------------------------------------
// Asset previews (Content Browser)
// ---------------------------------------------------------------------------

// Lazy texture thumbnails, on demand: the grid only requests assets whose
// cards are visible; the decode runs on a worker thread and the main thread
// uploads one small image per frame (see pump_asset_thumbnail_decodes).
void EditorApplication::request_asset_thumbnail_decode(const AssetMetadata& asset) {
    if (asset.type != AssetType::Texture || asset.cookedPath.empty()) {
        m_assetThumbnailFailed.insert(asset.id);
        return;
    }
    if (m_assetThumbnails.contains(asset.id) || m_assetThumbnailFailed.contains(asset.id)) return;
    std::lock_guard<std::mutex> lock(m_thumbDecodeMutex);
    if (m_thumbDecodeRequested.contains(asset.id)) return;
    // Bound the queue: if the user scrolls very fast, drop the oldest pending
    // request (it is re-requested when that row scrolls back into view).
    if (m_thumbDecodeQueue.size() >= 256) m_thumbDecodeQueue.pop_front();
    m_thumbDecodeRequested.insert(asset.id);
    m_thumbDecodeQueue.push_back(asset.id);
}

// Consumes finished decodes on the main thread (one small GPU upload per
// frame — no multi-second stalls) and starts one worker decode at a time.
// The queue only ever contains visible assets, so a big folder loads the
// screenful lazily and the rest stays as placeholders until scrolled into
// view, exactly like lazy loading in a web UI.
void EditorApplication::pump_asset_thumbnail_decodes() {
    PendingThumbDecode ready;
    bool haveReady = false;
    {
        std::lock_guard<std::mutex> lock(m_thumbDecodeMutex);
        if (m_thumbDecodeReady) {
            ready = std::move(*m_thumbDecodeReady);
            m_thumbDecodeReady.reset();
            haveReady = true;
        }
    }
    if (haveReady) {
        if (!ready.rgba.empty()) {
            GraphTexture gt;
            std::string error;
            const bool ok = ready.halfFloat
                ? upload_texture_half_pixels(ready.width, ready.height, ready.rgba, gt, error)
                : upload_texture_pixels(ready.width, ready.height, ready.rgba, 1, ready.srgb, gt, error);
            if (ok) {
                const VkDescriptorSet imguiId =
                    ImGui_ImplVulkan_AddTexture(gt.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                m_assetThumbnails[ready.assetId] =
                    AssetThumbnail{ gt.image, gt.memory, gt.view, imguiId };
            } else {
                destroy_graph_texture(gt);
                m_assetThumbnailFailed.insert(ready.assetId);
            }
        } else {
            // Corrupt/undecodable cooked file: remember so we never retry it.
            m_assetThumbnailFailed.insert(ready.assetId);
        }
        std::lock_guard<std::mutex> lock(m_thumbDecodeMutex);
        m_thumbDecodeRequested.erase(ready.assetId);
    }
    UUID next;
    {
        std::lock_guard<std::mutex> lock(m_thumbDecodeMutex);
        if (m_thumbDecodeBusy.load() || m_thumbDecodeQueue.empty()) return;
        next = m_thumbDecodeQueue.front();
        m_thumbDecodeQueue.pop_front();
        m_thumbDecodeBusy.store(true);
    }
    const auto meta = m_assetRegistry.find(next);
    if (!meta || meta->type != AssetType::Texture || meta->cookedPath.empty()) {
        m_assetThumbnailFailed.insert(next);
        {
            std::lock_guard<std::mutex> lock(m_thumbDecodeMutex);
            m_thumbDecodeRequested.erase(next);
            m_thumbDecodeBusy.store(false);
        }
        return;
    }
    const std::filesystem::path cookedPath = meta->cookedPath;
    // Join previous worker before launching a new one (cheap: the previous
    // one already signaled m_thumbDecodeBusy=false by the time we get here).
    if (m_thumbDecodeThread.joinable()) m_thumbDecodeThread.join();
    m_thumbDecodeThread = std::thread([this, id = next, cookedPath]() {
        PendingThumbDecode pending;
        pending.assetId = id;
        DecodedTexturePixels px;
        std::string error;
        if (decode_cooked_texture_pixels(cookedPath, 192, px, error)) {
            pending.width = px.width;
            pending.height = px.height;
            pending.srgb = px.srgb;
            pending.halfFloat = px.halfFloat;
            pending.rgba = std::move(px.rgba);
        }
        {
            std::lock_guard<std::mutex> lock(m_thumbDecodeMutex);
            m_thumbDecodeReady = std::move(pending);
        }
        m_thumbDecodeBusy.store(false);
    });
}

// Single active audio preview: clicking ▶ on another asset stops the current
// voice first, like a professional content browser. The decode itself is async
// (worker thread + bounded LRU cache) so long clips never freeze the editor.
void EditorApplication::toggle_audio_preview(const AssetMetadata& asset) {
    const bool alreadyPlaying = m_audioPreviewAsset == asset.id && m_audioPreviewVoice != 0 &&
                                m_playAudio.is_active(m_audioPreviewVoice);
    if (alreadyPlaying) {
        m_playAudio.stop(m_audioPreviewVoice);
        m_audioPreviewVoice = 0;
        m_audioPreviewAsset = UUID{ 0, 0 };
        m_audioPreviewRequest = UUID{ 0, 0 };
        return;
    }
    // Clicking again while this asset is still decoding cancels the request.
    const bool pendingThis = m_audioPreviewRequest == asset.id && m_audioPreviewVoice == 0;
    if (m_audioPreviewVoice != 0) m_playAudio.stop(m_audioPreviewVoice);
    m_audioPreviewVoice = 0;
    m_audioPreviewAsset = UUID{ 0, 0 };
    if (pendingThis) {
        m_audioPreviewRequest = UUID{ 0, 0 };
        return;
    }
    m_audioPreviewRequest = UUID{ 0, 0 };
    if (asset.cookedPath.empty()) return;
    m_audioPreviewRequest = asset.id;

    const auto cached = m_audioPreviewCache.find(asset.id);
    if (cached != m_audioPreviewCache.end()) {
        start_preview_voice(asset.id);
        return;
    }
    // No cached decode: the per-frame pump (pump_audio_preview_decodes) sees
    // the request and kicks the worker thread, then plays when it finishes.
}

// ---------------------------------------------------------------------------
// Playback sink: a miniaudio pull-mode device whose data callback renders the
// play-in-editor Mixer. The callback runs on miniaudio's thread; the Mixer
// locks internally, and the main thread only touches it briefly (play/stop /
// set_listener), so contention just produces an occasional underrun, never a
// deadlock. If the device cannot open (no audio hardware / sandbox), the
// editor falls back to silent rendering as before.
// ---------------------------------------------------------------------------
namespace {

void editor_audio_data_callback(ma_device* device, void* pOutput, const void* pInput, ma_uint32 frameCount);

class EditorAudioSink final {
public:
    EditorAudioSink() = default;
    ~EditorAudioSink() { shutdown(); }

    bool init(Engine::Audio::Mixer* mixer) {
        mixer_ = mixer;
        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32;
        config.playback.channels = 2; // matches Mixer::outputChannels_ (default 2)
        config.sampleRate = 48000;    // matches Mixer::sampleRate_ (default 48000)
        config.dataCallback = editor_audio_data_callback;
        config.pUserData = this;
        device_ = new ma_device{};
        if (ma_device_init(nullptr, &config, device_) != MA_SUCCESS) {
            delete device_;
            device_ = nullptr;
            return false;
        }
        if (ma_device_start(device_) != MA_SUCCESS) {
            ma_device_uninit(device_);
            delete device_;
            device_ = nullptr;
            return false;
        }
        return true;
    }

    void shutdown() {
        if (device_ != nullptr) {
            ma_device_uninit(device_);
            delete device_;
            device_ = nullptr;
        }
    }

    void render_output(void* output, unsigned int frameCount) {
        const std::span<const float> samples = mixer_->render(frameCount);
        std::memcpy(output, samples.data(), static_cast<std::size_t>(frameCount) * 2 * sizeof(float));
    }

private:
    Engine::Audio::Mixer* mixer_{ nullptr };
    ma_device* device_{ nullptr };
};

void editor_audio_data_callback(ma_device* device, void* pOutput, const void*, ma_uint32 frameCount) {
    static_cast<EditorAudioSink*>(device->pUserData)->render_output(pOutput, frameCount);
}

} // namespace

void EditorApplication::init_audio_output() {
    if (m_audioDevice != nullptr) return;
    auto* sink = new EditorAudioSink{};
    if (!sink->init(&m_playAudio)) {
        std::cerr << "[Audio] No playback device available; play-in-editor audio stays silent." << std::endl;
        delete sink;
        return;
    }
    m_audioDevice = sink;
    m_audioDeviceStarted = true;
}

// Join any in-flight worker threads before we tear down Vulkan resources.
// Called at the very top of cleanup() so no detached thread is accessing
// member data while we destroy GPU buffers, images, and pipelines.
void EditorApplication::join_worker_threads() {
    m_thumbDecodeBusy.store(true);   // prevent new launches
    m_audioDecodeBusy.store(true);
    if (m_thumbDecodeThread.joinable()) m_thumbDecodeThread.join();
    if (m_audioDecodeThread.joinable()) m_audioDecodeThread.join();
}

void EditorApplication::shutdown_audio_output() {
    if (m_audioDevice != nullptr) {
        delete static_cast<EditorAudioSink*>(m_audioDevice);
        m_audioDevice = nullptr;
    }
    m_audioDeviceStarted = false;
}

// Picks up finished background decodes, plays the one that is still requested,
// and kicks off a decode for any outstanding request not yet cached.
void EditorApplication::pump_audio_preview_decodes() {
    PendingAudioDecode ready;
    bool haveReady = false;
    {
        std::lock_guard<std::mutex> lock(m_audioDecodeMutex);
        if (m_audioDecodeReady) {
            ready = std::move(*m_audioDecodeReady);
            m_audioDecodeReady.reset();
            haveReady = true;
        }
    }
    if (haveReady) {
        if (ready.buffer.valid()) {
            cache_audio_preview(ready.assetId, ready.buffer);
        } else {
            // Corrupt/undecodable file: remember so we never retry it.
            m_audioPreviewDecodeFailed.insert(ready.assetId);
        }
        if (ready.assetId == m_audioPreviewRequest && m_audioPreviewVoice == 0) {
            start_preview_voice(ready.assetId);
        }
    }
    if (m_audioPreviewRequest != UUID{ 0, 0 } && m_audioPreviewVoice == 0 &&
        !m_audioPreviewCache.contains(m_audioPreviewRequest) &&
        !m_audioPreviewDecodeFailed.contains(m_audioPreviewRequest) &&
        !m_audioDecodeBusy.exchange(true)) {
        const UUID id = m_audioPreviewRequest;
        const auto meta = m_assetRegistry.find(id);
        if (meta && !meta->cookedPath.empty()) {
            if (m_audioDecodeThread.joinable()) m_audioDecodeThread.join();
            m_audioDecodeThread = std::thread([this, id, path = meta->cookedPath]() {
                const auto decoded = Engine::Audio::OggDecoder::decode_file(path);
                PendingAudioDecode pending;
                pending.assetId = id;
                if (decoded && decoded->valid()) {
                    pending.buffer.sampleRate = decoded->sampleRate;
                    pending.buffer.channels = decoded->channels;
                    pending.buffer.samples = std::move(decoded->samples);
                }
                {
                    std::lock_guard<std::mutex> lock(m_audioDecodeMutex);
                    m_audioDecodeReady = std::move(pending);
                }
                m_audioDecodeBusy.store(false);
            });
        } else {
            m_audioDecodeBusy.store(false);
            m_audioPreviewRequest = UUID{ 0, 0 };
        }
    }
    // A failed asset must not keep a stale request alive.
    if (m_audioPreviewDecodeFailed.contains(m_audioPreviewRequest)) {
        m_audioPreviewRequest = UUID{ 0, 0 };
    }
}

void EditorApplication::start_preview_voice(const UUID& assetId) {
    const auto meta = m_assetRegistry.find(assetId);
    const auto cached = m_audioPreviewCache.find(assetId);
    if (!meta || cached == m_audioPreviewCache.end()) return;
    if (m_audioPreviewVoice != 0) m_playAudio.stop(m_audioPreviewVoice);
    m_audioPreviewVoice = 0;
    auto clip = std::make_shared<Engine::Audio::AudioClip>(meta->sourcePath.stem().string());
    Engine::Audio::AudioBuffer playable = *cached->second;
    clip->hot_swap(std::move(playable));
    Engine::Audio::VoiceDescription desc;
    desc.clip = std::move(clip);
    desc.bus = m_playAudio.master_bus();
    desc.gain = 1.0f;
    desc.looping = false;
    desc.spatial = false;
    m_audioPreviewVoice = m_playAudio.play(std::move(desc));
    m_audioPreviewAsset = assetId;
    m_audioPreviewRequest = UUID{ 0, 0 };
}

void EditorApplication::cache_audio_preview(const UUID& assetId, const Engine::Audio::AudioBuffer& buffer) {
    // Bounded LRU: ~60s of stereo @48 kHz worth of decoded previews in memory.
    constexpr std::size_t kMaxCachedFrames = 48000u * 60u * 2u;
    const auto existing = m_audioPreviewCache.find(assetId);
    if (existing != m_audioPreviewCache.end()) {
        m_audioPreviewCacheFrames -= existing->second->frame_count();
        m_audioPreviewCache.erase(existing);
        std::erase(m_audioPreviewCacheOrder, assetId);
    }
    m_audioPreviewCache[assetId] = std::make_shared<Engine::Audio::AudioBuffer>(buffer);
    m_audioPreviewCacheFrames += buffer.frame_count();
    m_audioPreviewCacheOrder.push_back(assetId);
    while (m_audioPreviewCacheFrames > kMaxCachedFrames && m_audioPreviewCacheOrder.size() > 1) {
        const UUID oldest = m_audioPreviewCacheOrder.front();
        m_audioPreviewCacheOrder.pop_front();
        const auto it = m_audioPreviewCache.find(oldest);
        if (it != m_audioPreviewCache.end()) {
            m_audioPreviewCacheFrames -= it->second->frame_count();
            m_audioPreviewCache.erase(it);
        }
    }
}

void EditorApplication::destroy_asset_thumbnails() {
    if (m_device == VK_NULL_HANDLE) return;
    if (m_audioPreviewVoice != 0)    m_playAudio.stop(m_audioPreviewVoice);
    m_audioPreviewVoice = 0;
    m_audioPreviewAsset = UUID{ 0, 0 };
    m_audioPreviewRequest = UUID{ 0, 0 };
    for (auto& [id, thumb] : m_assetThumbnails) {
        (void)id;
        if (thumb.imguiId != VK_NULL_HANDLE) ImGui_ImplVulkan_RemoveTexture(thumb.imguiId);
        if (thumb.view != VK_NULL_HANDLE) vkDestroyImageView(m_device, thumb.view, nullptr);
        if (thumb.image != VK_NULL_HANDLE) vkDestroyImage(m_device, thumb.image, nullptr);
        if (thumb.memory != VK_NULL_HANDLE) vkFreeMemory(m_device, thumb.memory, nullptr);
    }
    m_assetThumbnails.clear();
    for (auto& [id, desc] : m_asset3dThumbnails) {
        (void)id;
        if (desc != VK_NULL_HANDLE) ImGui_ImplVulkan_RemoveTexture(desc);
    }
    m_asset3dThumbnails.clear();
    m_assetThumbnailFailed.clear();
    {
        std::lock_guard<std::mutex> lock(m_thumbDecodeMutex);
        m_thumbDecodeQueue.clear();
        m_thumbDecodeRequested.clear();
        m_thumbDecodeReady.reset();
        m_thumbDecodeBusy.store(false);
    }
}

// ---------------------------------------------------------------------------
// 3D asset thumbnails (Content Browser)
// ---------------------------------------------------------------------------

// Small dedicated offscreen (thumbSize x thumbSize) that reuses the viewport
// MSAA render pass, so any viewport pipeline (scene / block) can render one
// asset into it. The result becomes an ImGui texture cached in m_asset3dThumbnails.
void EditorApplication::init_thumbnail_target() {
    if (m_device == VK_NULL_HANDLE || m_offscreen.renderPass == VK_NULL_HANDLE) return;
    if (m_thumbImage != VK_NULL_HANDLE) return;
    const VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
    const VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
    // Resolve target (1x) — what ImGui shows as the thumbnail.
    create_image(m_thumbSize, m_thumbSize, colorFormat,
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_thumbImage, m_thumbMemory);
    m_thumbView = create_image_view(m_thumbImage, colorFormat, VK_IMAGE_ASPECT_COLOR_BIT);
    // Multisampled color + depth (same render pass as the viewport).
    create_image(m_thumbSize, m_thumbSize, colorFormat,
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                 m_thumbMsaaImage, m_thumbMsaaMemory, 1, m_viewportSamples);
    m_thumbMsaaView = create_image_view(m_thumbMsaaImage, colorFormat, VK_IMAGE_ASPECT_COLOR_BIT);
    create_image(m_thumbSize, m_thumbSize, depthFormat,
                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                 m_thumbDepthImage, m_thumbDepthMemory, 1, m_viewportSamples);
    m_thumbDepthView = create_image_view(m_thumbDepthImage, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkImageView attachments[3] = { m_thumbMsaaView, m_thumbDepthView, m_thumbView };
    VkFramebufferCreateInfo fbInfo{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fbInfo.renderPass = m_offscreen.renderPass;
    fbInfo.attachmentCount = 3;
    fbInfo.pAttachments = attachments;
    fbInfo.width = m_thumbSize;
    fbInfo.height = m_thumbSize;
    fbInfo.layers = 1;
    vkCreateFramebuffer(m_device, &fbInfo, nullptr, &m_thumbFramebuffer);
}

void EditorApplication::destroy_thumbnail_target() {
    if (m_device == VK_NULL_HANDLE) return;
    if (m_thumbFramebuffer != VK_NULL_HANDLE) { vkDestroyFramebuffer(m_device, m_thumbFramebuffer, nullptr); m_thumbFramebuffer = VK_NULL_HANDLE; }
    if (m_thumbMsaaView != VK_NULL_HANDLE) { vkDestroyImageView(m_device, m_thumbMsaaView, nullptr); m_thumbMsaaView = VK_NULL_HANDLE; }
    if (m_thumbMsaaImage != VK_NULL_HANDLE) { vkDestroyImage(m_device, m_thumbMsaaImage, nullptr); m_thumbMsaaImage = VK_NULL_HANDLE; }
    if (m_thumbMsaaMemory != VK_NULL_HANDLE) { vkFreeMemory(m_device, m_thumbMsaaMemory, nullptr); m_thumbMsaaMemory = VK_NULL_HANDLE; }
    if (m_thumbView != VK_NULL_HANDLE) { vkDestroyImageView(m_device, m_thumbView, nullptr); m_thumbView = VK_NULL_HANDLE; }
    if (m_thumbImage != VK_NULL_HANDLE) { vkDestroyImage(m_device, m_thumbImage, nullptr); m_thumbImage = VK_NULL_HANDLE; }
    if (m_thumbMemory != VK_NULL_HANDLE) { vkFreeMemory(m_device, m_thumbMemory, nullptr); m_thumbMemory = VK_NULL_HANDLE; }
    if (m_thumbDepthView != VK_NULL_HANDLE) { vkDestroyImageView(m_device, m_thumbDepthView, nullptr); m_thumbDepthView = VK_NULL_HANDLE; }
    if (m_thumbDepthImage != VK_NULL_HANDLE) { vkDestroyImage(m_device, m_thumbDepthImage, nullptr); m_thumbDepthImage = VK_NULL_HANDLE; }
    if (m_thumbDepthMemory != VK_NULL_HANDLE) { vkFreeMemory(m_device, m_thumbDepthMemory, nullptr); m_thumbDepthMemory = VK_NULL_HANDLE; }
}

// Textured unit cube: the "block" pipeline used to assemble a Minecraft-style
// block model from a PNG texture (thumbnail preview + scene preview).
void EditorApplication::init_block_cube() {
    if (m_device == VK_NULL_HANDLE || m_blockPipeline != VK_NULL_HANDLE) return;

    // Unit cube with per-face UVs: 24 vertices / 36 indices.
    struct BlockVert { glm::vec3 pos; glm::vec2 uv; };
    const BlockVert verts[24] = {
        { { -0.5f, -0.5f,  0.5f }, { 0, 0 } }, { {  0.5f, -0.5f,  0.5f }, { 1, 0 } },
        { {  0.5f,  0.5f,  0.5f }, { 1, 1 } }, { { -0.5f,  0.5f,  0.5f }, { 0, 1 } }, // +Z
        { {  0.5f, -0.5f, -0.5f }, { 0, 0 } }, { { -0.5f, -0.5f, -0.5f }, { 1, 0 } },
        { { -0.5f,  0.5f, -0.5f }, { 1, 1 } }, { {  0.5f,  0.5f, -0.5f }, { 0, 1 } }, // -Z
        { {  0.5f, -0.5f,  0.5f }, { 0, 0 } }, { {  0.5f, -0.5f, -0.5f }, { 1, 0 } },
        { {  0.5f,  0.5f, -0.5f }, { 1, 1 } }, { {  0.5f,  0.5f,  0.5f }, { 0, 1 } }, // +X
        { { -0.5f, -0.5f, -0.5f }, { 0, 0 } }, { { -0.5f, -0.5f,  0.5f }, { 1, 0 } },
        { { -0.5f,  0.5f,  0.5f }, { 1, 1 } }, { { -0.5f,  0.5f, -0.5f }, { 0, 1 } }, // -X
        { { -0.5f,  0.5f,  0.5f }, { 0, 0 } }, { {  0.5f,  0.5f,  0.5f }, { 1, 0 } },
        { {  0.5f,  0.5f, -0.5f }, { 1, 1 } }, { { -0.5f,  0.5f, -0.5f }, { 0, 1 } }, // +Y
        { { -0.5f, -0.5f, -0.5f }, { 0, 0 } }, { {  0.5f, -0.5f, -0.5f }, { 1, 0 } },
        { {  0.5f, -0.5f,  0.5f }, { 1, 1 } }, { { -0.5f, -0.5f,  0.5f }, { 0, 1 } }, // -Y
    };
    const uint32_t indices[36] = {
        0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7, 8, 9, 10, 8, 10, 11,
        12, 13, 14, 12, 14, 15, 16, 17, 18, 16, 18, 19, 20, 21, 22, 20, 22, 23,
    };
    const VkDeviceSize vbSize = sizeof(BlockVert) * 24;
    const VkDeviceSize ibSize = sizeof(uint32_t) * 36;
    create_buffer(vbSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  m_blockCubeVB.buffer, m_blockCubeVB.memory);
    create_buffer(ibSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  m_blockCubeIB.buffer, m_blockCubeIB.memory);
    void* data = nullptr;
    vkMapMemory(m_device, m_blockCubeVB.memory, 0, vbSize, 0, &data);
    std::memcpy(data, verts, vbSize);
    vkUnmapMemory(m_device, m_blockCubeVB.memory);
    vkMapMemory(m_device, m_blockCubeIB.memory, 0, ibSize, 0, &data);
    std::memcpy(data, indices, ibSize);
    vkUnmapMemory(m_device, m_blockCubeIB.memory);
    m_blockCubeIndexCount = 36;

    VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    vkCreateSampler(m_device, &samplerInfo, nullptr, &m_blockSampler);

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo descLayoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    descLayoutInfo.bindingCount = 1;
    descLayoutInfo.pBindings = &binding;
    vkCreateDescriptorSetLayout(m_device, &descLayoutInfo, nullptr, &m_blockDescSetLayout);

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = 64; // mat4 mvp
    VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_blockDescSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_blockPipelineLayout);

    m_blockVertShader = make_module(m_device, read_spv("block.vert.spv"));
    m_blockFragShader = make_module(m_device, read_spv("block.frag.spv"));
    if (!m_blockVertShader || !m_blockFragShader) {
        std::cerr << "[Editor] block shaders missing (run compile_shaders)" << std::endl;
        return;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = m_blockVertShader;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = m_blockFragShader;
    stages[1].pName = "main";

    VkVertexInputBindingDescription bindings[2]{};
    bindings[0].binding = 0; bindings[0].stride = sizeof(BlockVert); bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0].location = 0; attrs[0].binding = 0; attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[0].offset = 0;
    attrs[1].location = 1; attrs[1].binding = 0; attrs[1].format = VK_FORMAT_R32G32_SFLOAT; attrs[1].offset = sizeof(glm::vec3);
    VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = bindings;
    vertexInput.vertexAttributeDescriptionCount = 2;
    vertexInput.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_BACK_BIT;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    multisample.rasterizationSamples = m_viewportSamples;
    multisample.alphaToCoverageEnable = VK_FALSE;
    VkPipelineDepthStencilStateCreateInfo depth{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo colorBlend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blend;
    VkPipelineViewportStateCreateInfo viewportState{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo pipelineInfo{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depth;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamic;
    pipelineInfo.layout = m_blockPipelineLayout;
    pipelineInfo.renderPass = m_offscreen.renderPass;
    pipelineInfo.subpass = 0;
    vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_blockPipeline);
}

void EditorApplication::destroy_block_cube() {
    if (m_device == VK_NULL_HANDLE) return;
    for (auto& [id, gt] : m_blockTextures) {
        (void)id;
        destroy_graph_texture(gt);
    }
    m_blockTextures.clear();
    m_blockDescriptors.clear();
    if (m_blockPipeline != VK_NULL_HANDLE) { vkDestroyPipeline(m_device, m_blockPipeline, nullptr); m_blockPipeline = VK_NULL_HANDLE; }
    if (m_blockPipelineLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(m_device, m_blockPipelineLayout, nullptr); m_blockPipelineLayout = VK_NULL_HANDLE; }
    if (m_blockDescSetLayout != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(m_device, m_blockDescSetLayout, nullptr); m_blockDescSetLayout = VK_NULL_HANDLE; }
    if (m_blockSampler != VK_NULL_HANDLE) { vkDestroySampler(m_device, m_blockSampler, nullptr); m_blockSampler = VK_NULL_HANDLE; }
    if (m_blockVertShader != VK_NULL_HANDLE) { vkDestroyShaderModule(m_device, m_blockVertShader, nullptr); m_blockVertShader = VK_NULL_HANDLE; }
    if (m_blockFragShader != VK_NULL_HANDLE) { vkDestroyShaderModule(m_device, m_blockFragShader, nullptr); m_blockFragShader = VK_NULL_HANDLE; }
    if (m_blockCubeVB.buffer != VK_NULL_HANDLE) { vkDestroyBuffer(m_device, m_blockCubeVB.buffer, nullptr); vkFreeMemory(m_device, m_blockCubeVB.memory, nullptr); m_blockCubeVB = GPUBuffer{}; }
    if (m_blockCubeIB.buffer != VK_NULL_HANDLE) { vkDestroyBuffer(m_device, m_blockCubeIB.buffer, nullptr); vkFreeMemory(m_device, m_blockCubeIB.memory, nullptr); m_blockCubeIB = GPUBuffer{}; }
}

// Lazy descriptor set for a block texture (my layout, allocated from the ImGui
// descriptor pool which carries COMBINED_IMAGE_SAMPLER).
VkDescriptorSet EditorApplication::get_block_descriptor(const UUID& textureAsset) {
    const auto cached = m_blockDescriptors.find(textureAsset);
    if (cached != m_blockDescriptors.end()) return cached->second;
    if (m_blockDescSetLayout == VK_NULL_HANDLE || m_blockSampler == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    GraphTexture gt;
    std::string error;
    if (!load_viewport_texture(textureAsset, gt, error, 192)) return VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = m_imguiDescriptorPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &m_blockDescSetLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(m_device, &ai, &set) != VK_SUCCESS) {
        destroy_graph_texture(gt);
        return VK_NULL_HANDLE;
    }
    VkDescriptorImageInfo imageInfo{ m_blockSampler, gt.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    m_blockTextures[textureAsset] = gt; // keeps image/view alive
    m_blockDescriptors[textureAsset] = set;
    return set;
}

void EditorApplication::request_3d_thumbnail(const UUID& assetId) {
    if (!assetId.is_valid()) return;
    if (m_assetThumbnails.contains(assetId) || m_asset3dThumbnails.contains(assetId) ||
        m_assetThumbnailFailed.contains(assetId) || m_thumbnailQueued.contains(assetId)) {
        return;
    }
    m_thumbnailQueued.insert(assetId);
    m_thumbnailQueue.push_back(assetId);
}

// Renders pending mesh/block thumbnails, a few per frame (each render is a
// submit + wait, so the budget keeps the editor responsive).
void EditorApplication::pump_asset_thumbnails(int budget) {
    if (m_thumbFramebuffer == VK_NULL_HANDLE || m_device == VK_NULL_HANDLE) return;
    while (budget-- > 0 && !m_thumbnailQueue.empty()) {
        const UUID id = m_thumbnailQueue.front();
        m_thumbnailQueue.pop_front();
        m_thumbnailQueued.erase(id);
        if (m_assetThumbnails.contains(id) || m_asset3dThumbnails.contains(id) ||
            m_assetThumbnailFailed.contains(id)) {
            continue;
        }
        const auto meta = m_assetRegistry.find(id);
        if (!meta) { m_assetThumbnailFailed.insert(id); continue; }
        if (meta->type == AssetType::Mesh) {
            if (!load_mesh_resource(id)) { m_assetThumbnailFailed.insert(id); continue; }
            const EditorMeshResource* mesh = get_mesh_resource(id);
            if (!mesh || !mesh->valid) { m_assetThumbnailFailed.insert(id); continue; }
            render_mesh_thumbnail(id, *mesh);
        } else if (meta->type == AssetType::Block) {
            const UUID tex = resolve_block_texture(id);
            if (!tex.is_valid()) { m_assetThumbnailFailed.insert(id); continue; }
            const VkDescriptorSet desc = get_block_descriptor(tex);
            if (desc == VK_NULL_HANDLE) { m_assetThumbnailFailed.insert(id); continue; }
            render_block_thumbnail(id, desc);
        } else if (meta->type == AssetType::Texture && is_block_texture(*meta)) {
            // The PNG is the block: its card shows the textured cube instead
            // of the flat image.
            const VkDescriptorSet desc = get_block_descriptor(meta->id);
            if (desc == VK_NULL_HANDLE) { m_assetThumbnailFailed.insert(id); continue; }
            render_block_thumbnail(id, desc);
        } else {
            m_assetThumbnailFailed.insert(id);
        }
    }
}

// Renders a cooked mesh into the thumbnail offscreen with the scene pipeline
// (neutral material color), framed from its bounds, and caches an ImGui
// texture. The color image itself stays owned by the thumbnail target.
void EditorApplication::render_mesh_thumbnail(const UUID& assetId, const EditorMeshResource& mesh) {
    if (m_scenePipeline == VK_NULL_HANDLE) return;
    const glm::vec3 center = (mesh.boundsMin + mesh.boundsMax) * 0.5f;
    const float radius = std::max(glm::length(mesh.boundsMax - mesh.boundsMin) * 0.5f, 1e-4f);
    const float camDist = radius * 2.6f;
    const glm::mat4 proj = glm::perspective(glm::radians(45.0f), 1.0f, 0.01f, camDist * 20.0f);
    const glm::mat4 view = glm::lookAt(center + glm::vec3(0.75f, 0.60f, 0.90f) * camDist, center, glm::vec3(0, 1, 0));

    VkCommandBuffer cmd = begin_single_time_commands();
    transition_image_layout(cmd, m_thumbImage, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    VkRenderPassBeginInfo rp{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rp.renderPass = m_offscreen.renderPass;
    rp.framebuffer = m_thumbFramebuffer;
    rp.renderArea = { { 0, 0 }, { m_thumbSize, m_thumbSize } };
    const VkClearValue clears[2] = {
        { { { 0.10f, 0.11f, 0.14f, 1.0f } } }, // surface background
        { { 1.0f, 0 } },
    };
    rp.clearValueCount = 2;
    rp.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport vp{ 0, 0, static_cast<float>(m_thumbSize), static_cast<float>(m_thumbSize), 0, 1 };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    const VkRect2D sc{ { 0, 0 }, { m_thumbSize, m_thumbSize } };
    vkCmdSetScissor(cmd, 0, 1, &sc);
    // The thumbnail shares the viewport's MSAA render pass, so the scene
    // pipeline (same samples) renders the mesh into it.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_scenePipeline);
    draw_mesh_resource(cmd, proj * view, glm::vec4(0.62f, 0.66f, 0.75f, 1.0f), mesh);
    vkCmdEndRenderPass(cmd);
    transition_image_layout(cmd, m_thumbImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    end_single_time_commands(cmd);

    m_asset3dThumbnails[assetId] =
        ImGui_ImplVulkan_AddTexture(m_thumbView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// Same, but a textured unit cube: the Minecraft-style block assembled from its
// PNG face texture (block pipeline).
void EditorApplication::render_block_thumbnail(const UUID& assetId, VkDescriptorSet textureDesc) {
    if (m_blockPipeline == VK_NULL_HANDLE) return;
    // Frame the full character: look at its vertical center (the model spans
    // y 0..2 with the feet at the origin) from a bit further out.
    const glm::vec3 charCenter(0.0f, 1.0f, 0.0f);
    const glm::mat4 proj = glm::perspective(glm::radians(45.0f), 1.0f, 0.01f, 50.0f);
    const glm::mat4 view = glm::lookAt(charCenter + glm::vec3(2.6f, 2.2f, 3.0f), charCenter, glm::vec3(0, 1, 0));
    const glm::mat4 mvp = proj * view;

    VkCommandBuffer cmd = begin_single_time_commands();
    transition_image_layout(cmd, m_thumbImage, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    VkRenderPassBeginInfo rp{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rp.renderPass = m_offscreen.renderPass;
    rp.framebuffer = m_thumbFramebuffer;
    rp.renderArea = { { 0, 0 }, { m_thumbSize, m_thumbSize } };
    const VkClearValue clears[2] = {
        { { { 0.10f, 0.11f, 0.14f, 1.0f } } },
        { { 1.0f, 0 } },
    };
    rp.clearValueCount = 2;
    rp.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport vp{ 0, 0, static_cast<float>(m_thumbSize), static_cast<float>(m_thumbSize), 0, 1 };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    const VkRect2D sc{ { 0, 0 }, { m_thumbSize, m_thumbSize } };
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_blockPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_blockPipelineLayout,
                            0, 1, &textureDesc, 0, nullptr);
    vkCmdPushConstants(cmd, m_blockPipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64, &mvp);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m_blockCubeVB.buffer, &offset);
    vkCmdBindIndexBuffer(cmd, m_blockCubeIB.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, m_blockCubeIndexCount, 1, 0, 0, 0);
    vkCmdEndRenderPass(cmd);
    transition_image_layout(cmd, m_thumbImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    end_single_time_commands(cmd);

    m_asset3dThumbnails[assetId] =
        ImGui_ImplVulkan_AddTexture(m_thumbView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// ---------------------------------------------------------------------------
// Minecraft-style block model assets
// ---------------------------------------------------------------------------

// Minecraft character/mob skins are square POT too (player 64x64, mobs
// 64x64...), so entity/mob path + filename signals classify them as MODELS.
// Resource-pack block folders ("/textures/block/", "/blocks/") always win —
// vanilla names like mob_spawner live there and ARE blocks.
bool EditorApplication::is_character_texture(const AssetMetadata& meta) const {
    if (meta.type != AssetType::Texture || meta.sourcePath.empty()) return false;
    std::string p = meta.sourcePath.generic_string();
    std::transform(p.begin(), p.end(), p.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const char* kSkinPathMarkers[] = {
        "/entity/", "/entities/", "/mob/", "/mobs/", "/char/", "/chars/",
        "/character/", "/characters/", "/player/", "/players/", "/actor/",
        "/actors/", "/humanoid/", "/creature/", "/creatures/", "/monster/",
        "/monsters/", "/npc/", "/npcs/", "/zombie/", "/villager/", "/village/",
    };
    for (const char* marker : kSkinPathMarkers) {
        if (p.find(marker) != std::string::npos) return true;
    }
    std::string stem = meta.sourcePath.stem().string();
    std::transform(stem.begin(), stem.end(), stem.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const char* kSkinNameMarkers[] = {
        "skin", "char", "player", "npc", "actor", "humanoid", "steve",
        "alex", "villager", "zombie", "creeper", "skeleton", "enderman",
        "spider", "chicken", "wolf", "horse", "rabbit", "squid", "slime",
        "ghast", "blaze", "witch", "wither", "dragon", "guardian",
        "shulker", "phantom", "drowned", "husk", "stray", "vex",
        "pillager", "ravager", "panda", "parrot", "turtle", "dolphin",
        "llama", "salmon", "pufferfish", "hoglin", "piglin", "zoglin",
        "strider", "trader", "golem", "silverfish", "magma", "sheep",
        "cow", "pig", "bee", "fox", "bat",
    };
    // Word-boundary match: "char_01" is a skin, but "charcoal" (a block) is
    // not — the marker must sit between non-alphanumeric separators.
    const auto hasMarker = [](const std::string& s, const char* marker) {
        const size_t pos = s.find(marker);
        if (pos == std::string::npos) return false;
        if (pos > 0 && std::isalnum(static_cast<unsigned char>(s[pos - 1]))) return false;
        const size_t end = pos + std::strlen(marker);
        if (end < s.size() && std::isalnum(static_cast<unsigned char>(s[end]))) return false;
        return true;
    };
    for (const char* marker : kSkinNameMarkers) {
        if (hasMarker(stem, marker)) return true;
    }
    return false;
}

// Heuristic: small square power-of-two textures are the classic Minecraft
// block face format (16/32/64/128/256). Character/mob skins are excluded
// (they are models, not blocks); block folders always win.
bool EditorApplication::looks_like_block_texture(const AssetMetadata& meta) const {
    if (meta.type != AssetType::Texture || meta.width == 0 || meta.height == 0) return false;
    if (meta.width != meta.height) return false;
    const uint32_t s = meta.width;
    if (s < 8 || s > 256) return false;
    if ((s & (s - 1)) != 0) return false;
    std::string p = meta.sourcePath.generic_string();
    std::transform(p.begin(), p.end(), p.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (p.find("/block/") != std::string::npos || p.find("/blocks/") != std::string::npos ||
        p.find("/tile/") != std::string::npos) {
        return true;
    }
    if (is_character_texture(meta)) return false;
    return true;
}

// The PNG is the block: a texture counts as a block when it looks like one
// (square POT 8-256, excluding character/mob skins) or when an existing
// .vblock sidecar references it (the explicit user mark). The registry scan
// is cached per texture UUID — it runs once, and sidecars are permanent once
// created.
bool EditorApplication::is_block_texture(const AssetMetadata& meta) {
    if (meta.type != AssetType::Texture || !meta.id.is_valid()) return false;
    // Explicit "not a block" (user override) wins over everything. The marker
    // file is stat()'d once per texture UUID (cached), so restarts honor it.
    if (!m_noblockChecked.contains(meta.id)) {
        m_noblockChecked.insert(meta.id);
        if (!meta.sourcePath.empty()) {
            std::error_code ec;
            if (std::filesystem::exists(meta.sourcePath.string() + ".noblock", ec)) {
                m_noblockTextures.insert(meta.id);
            }
        }
    }
    if (m_noblockTextures.contains(meta.id)) return false;
    if (looks_like_block_texture(meta)) return true;
    if (m_blockSidecarChecked.contains(meta.id)) return m_blockTextureSet.contains(meta.id);
    bool found = false;
    for (const AssetMetadata& candidate : m_assetRegistry.snapshot()) {
        if (candidate.type != AssetType::Block) continue;
        BlockAssetData data;
        if (load_block_asset(candidate.id, data) &&
            (data.texture == meta.id || data.top == meta.id ||
             data.side == meta.id || data.bottom == meta.id)) {
            found = true;
            break;
        }
    }
    m_blockSidecarChecked.insert(meta.id);
    if (found) m_blockTextureSet.insert(meta.id);
    return found;
}

// User override: delete the .vblock sidecar (file + registry entry) so a
// texture that was misclassified as a block (a character/mob skin) becomes a
// plain texture again. The texture card then shows the flat image and stops
// spawning cubes.
void EditorApplication::unmark_block_texture(const AssetMetadata& textureMeta) {
    if (!textureMeta.id.is_valid()) return;
    // Remove the .vblock sidecar if one exists (the positive block mark).
    for (const AssetMetadata& candidate : m_assetRegistry.snapshot()) {
        if (candidate.type != AssetType::Block) continue;
        BlockAssetData data;
        if (!load_block_asset(candidate.id, data)) continue;
        if (data.texture != textureMeta.id && data.top != textureMeta.id &&
            data.side != textureMeta.id && data.bottom != textureMeta.id) {
            continue;
        }
        AssetBrowserModel browser(m_assetRegistry);
        const AssetFileOperationResult removed = browser.delete_asset(candidate.id);
        if (!removed) {
            std::cerr << "[ContentBrowser] Could not unmark block: " << removed.error << std::endl;
        } else {
            m_blockAssetCache.erase(candidate.id);
            m_blockAssetFailed.erase(candidate.id);
            const auto registryPath =
                std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "AssetRegistry.db";
            if (!m_assetRegistry.save(registryPath))
                std::cerr << "[AssetRegistry] Could not persist unmarked block" << std::endl;
        }
        break;
    }
    // Negative marker: even a heuristic block (e.g. inside a /block/ folder)
    // stops being one. The file is checked once per texture UUID (cached).
    if (!textureMeta.sourcePath.empty()) {
        const std::filesystem::path marker = textureMeta.sourcePath.string() + ".noblock";
        std::error_code ec;
        std::ofstream out(marker, std::ios::trunc);
        out << "noblock\n";
        out.close();
    }
    m_noblockTextures.insert(textureMeta.id);
    m_blockSidecarChecked.erase(textureMeta.id);
    m_blockTextureSet.erase(textureMeta.id);
    std::cout << "[ContentBrowser] Unmarked '" << textureMeta.sourcePath.filename().string()
              << "' as a block" << std::endl;
}

// Find-or-create the .vblock sidecar for a texture (JSON: texture UUID per
// face; all default to the source texture) and register it as AssetType::Block.
// The PNG itself IS the Minecraft-style block, so a texture that already has a
// sidecar referencing it is returned as-is instead of duplicating.
UUID EditorApplication::create_block_asset(const AssetMetadata& textureMeta) {
    if (!textureMeta.id.is_valid() || textureMeta.type != AssetType::Texture) return UUID{ 0, 0 };
    // "Marcar como Bloco" also clears a previous "noblock" override.
    if (m_noblockTextures.erase(textureMeta.id) > 0 && !textureMeta.sourcePath.empty()) {
        std::error_code ec;
        std::filesystem::remove(textureMeta.sourcePath.string() + ".noblock", ec);
    }
    // Reuse an existing sidecar that already references this texture (repeat
    // drops/clicks/API calls must not pile up grass_2.vblock, grass_3.vblock…).
    for (const AssetMetadata& candidate : m_assetRegistry.snapshot()) {
        if (candidate.type != AssetType::Block) continue;
        BlockAssetData data;
        if (load_block_asset(candidate.id, data) &&
            (data.texture == textureMeta.id || data.top == textureMeta.id ||
             data.side == textureMeta.id || data.bottom == textureMeta.id)) {
            return candidate.id;
        }
    }
    std::filesystem::path blockPath = textureMeta.sourcePath.parent_path() /
        (textureMeta.sourcePath.stem().string() + ".vblock");
    unsigned suffix = 2;
    while (std::filesystem::exists(blockPath)) {
        blockPath = textureMeta.sourcePath.parent_path() /
            (textureMeta.sourcePath.stem().string() + "_" + std::to_string(suffix++) + ".vblock");
    }
    {
        std::ofstream out(blockPath);
        out << "{\"texture\":\"" << textureMeta.id.to_string() << "\"}";
    }
    AssetMetadata meta;
    meta.id = UUID();
    meta.type = AssetType::Block;
    meta.sourcePath = blockPath;
    meta.cookedPath = blockPath;
    meta.isCooked = true;
    meta.contentHash = textureMeta.contentHash;
    if (!m_assetRegistry.register_asset(meta)) {
        std::cerr << "[ContentBrowser] Failed to register block asset " << blockPath.string() << std::endl;
        return UUID{ 0, 0 };
    }
    m_blockAssetCache[meta.id] = BlockAssetData{ textureMeta.id, textureMeta.id, textureMeta.id, textureMeta.id };
    m_blockAssetFailed.erase(meta.id);
    const auto registryPath = std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "AssetRegistry.db";
    if (!m_assetRegistry.save(registryPath)) {
        std::cerr << "[AssetRegistry] Could not persist block asset" << std::endl;
    }
    return meta.id;
}

// Parses the .vblock sidecar (JSON is simple enough for a targeted string
// scan — no JSON dependency needed).
bool EditorApplication::load_block_asset(const UUID& blockAssetId, BlockAssetData& out) {
    const auto cached = m_blockAssetCache.find(blockAssetId);
    if (cached != m_blockAssetCache.end()) { out = cached->second; return true; }
    if (m_blockAssetFailed.contains(blockAssetId)) return false;
    const auto meta = m_assetRegistry.find(blockAssetId);
    if (!meta || meta->type != AssetType::Block || meta->sourcePath.empty() ||
        !std::filesystem::is_regular_file(meta->sourcePath)) {
        m_blockAssetFailed.insert(blockAssetId);
        return false;
    }
    std::ifstream in(meta->sourcePath);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    const auto grab = [&](const char* key) -> UUID {
        const std::string needle = std::string("\"") + key + "\"";
        const size_t p = text.find(needle);
        if (p == std::string::npos) return UUID{ 0, 0 };
        const size_t q = text.find('"', p + needle.size());
        if (q == std::string::npos) return UUID{ 0, 0 };
        const size_t r = text.find('"', q + 1);
        if (r == std::string::npos) return UUID{ 0, 0 };
        return UUID::from_string(text.substr(q + 1, r - q - 1));
    };
    BlockAssetData data;
    data.texture = grab("texture");
    data.top = grab("top");
    data.bottom = grab("bottom");
    data.side = grab("side");
    if (!data.texture.is_valid() && !data.top.is_valid() && !data.side.is_valid() && !data.bottom.is_valid()) {
        m_blockAssetFailed.insert(blockAssetId);
        return false;
    }
    m_blockAssetCache[blockAssetId] = data;
    return true;
}

UUID EditorApplication::resolve_block_texture(const UUID& blockAssetId) {
    BlockAssetData data;
    if (!load_block_asset(blockAssetId, data)) return UUID{ 0, 0 };
    if (data.texture.is_valid()) return data.texture;
    if (data.side.is_valid()) return data.side;
    if (data.top.is_valid()) return data.top;
    return data.bottom;
}

// ---------------------------------------------------------------------------
// Voxel sculpting (Escultura de Blocos) — real grid, real rendering, real
// painting. Each VoxelVolumeComponent entity owns an editable
// Engine::Voxel::VoxelStructure (32x24x32 cells, 1 m each) rendered as colored
// cubes; the brush panel paints into it via Engine::Voxel::VoxelTools.
// ---------------------------------------------------------------------------
namespace {
constexpr int kVoxelSizeX = 32;
constexpr int kVoxelSizeY = 24;
constexpr int kVoxelSizeZ = 32;

uint32_t voxel_hash2(int x, int z, uint32_t seed) {
    uint32_t h = seed ^ (static_cast<uint32_t>(x) * 374761393u) ^ (static_cast<uint32_t>(z) * 668265263u);
    h = (h ^ (h >> 13)) * 1274126177u;
    return (h ^ (h >> 16)) & 0xFFFFu;
}

glm::vec3 voxel_type_color(uint16_t type) {
    switch (type) {
        case 1: return glm::vec3(0.55f, 0.42f, 0.30f); // terra
        case 2: return glm::vec3(0.30f, 0.72f, 0.30f); // grama
        case 3: return glm::vec3(0.55f, 0.55f, 0.58f); // pedra
        case 4: return glm::vec3(0.25f, 0.45f, 0.85f); // água
        default: return glm::vec3(0.62f, 0.66f, 0.75f);
    }
}
} // namespace

void EditorApplication::ensure_voxel_volume(const UUID& entityId, uint32_t seed, float seaLevel) {
    if (m_voxelStructures.contains(entityId)) return;
    auto grid = std::make_unique<Engine::Voxel::VoxelStructure>(
        Engine::Voxel::Int3{ kVoxelSizeX, kVoxelSizeY, kVoxelSizeZ }, "Voxel");
    // Deterministic terrain from the volume seed (noise height per column).
    const int sea = std::clamp(static_cast<int>(seaLevel), 0, kVoxelSizeY - 2);
    for (int x = 0; x < kVoxelSizeX; ++x) {
        for (int z = 0; z < kVoxelSizeZ; ++z) {
            const uint32_t n = voxel_hash2(x, z, seed);
            const float v = static_cast<float>(n) / 65535.0f;
            const float hills = 6.0f * std::sin(x * 0.35f + seed * 0.001f) * std::cos(z * 0.28f);
            const int height = std::clamp(static_cast<int>(8.0f + v * 9.0f + hills * 0.5f), 2, kVoxelSizeY - 1);
            for (int y = 0; y < height; ++y) {
                const uint16_t type = (y == height - 1) ? 2 : ((y > sea) ? 3 : 1);
                grid->set(Engine::Voxel::Int3{ x, y, z }, Engine::Voxel::VoxelValue{ type, 0, 255 });
            }
            if (height < sea) {
                for (int y = height; y < sea; ++y) {
                    grid->set(Engine::Voxel::Int3{ x, y, z }, Engine::Voxel::VoxelValue{ 4, 0, 255 });
                }
            }
        }
    }
    m_voxelStructures[entityId] = std::move(grid);
    m_voxelMeshesDirty.insert(entityId);
}

void EditorApplication::rebuild_voxel_mesh(const UUID& entityId) {
    const auto gridIt = m_voxelStructures.find(entityId);
    if (gridIt == m_voxelStructures.end()) return;
    const Engine::Voxel::VoxelStructure& grid = *gridIt->second;
    const auto trIt = m_editorScene->transformComponents.find(entityId);
    const glm::vec3 origin = (trIt != m_editorScene->transformComponents.end())
                                 ? trIt->second.position
                                 : glm::vec3(0.0f);

    std::vector<EditorVertex> verts;
    std::vector<uint32_t> indices;
    verts.reserve(32768);
    indices.reserve(49152);
    auto& mesh = m_voxelMeshes[entityId];
    if (mesh.valid) {
        if (mesh.vb.buffer != VK_NULL_HANDLE) destroy_buffer(mesh.vb);
        if (mesh.ib.buffer != VK_NULL_HANDLE) destroy_buffer(mesh.ib);
        mesh = EditorVoxelMesh{};
    }
    for (int x = 0; x < kVoxelSizeX; ++x) {
        for (int y = 0; y < kVoxelSizeY; ++y) {
            for (int z = 0; z < kVoxelSizeZ; ++z) {
                const Engine::Voxel::VoxelValue v = grid.get(Engine::Voxel::Int3{ x, y, z });
                if (v.empty()) continue;
                const glm::vec3 base = origin + glm::vec3(x - kVoxelSizeX / 2, y, z - kVoxelSizeZ / 2);
                const glm::vec3 color = voxel_type_color(v.type);
                const uint32_t first = static_cast<uint32_t>(verts.size());
                // 8 corners, 12 triangles (indexed cubes with per-vertex color).
                const glm::vec3 c[8] = {
                    base + glm::vec3(0, 0, 0), base + glm::vec3(1, 0, 0),
                    base + glm::vec3(1, 1, 0), base + glm::vec3(0, 1, 0),
                    base + glm::vec3(0, 0, 1), base + glm::vec3(1, 0, 1),
                    base + glm::vec3(1, 1, 1), base + glm::vec3(0, 1, 1),
                };
                for (const glm::vec3& p : c) {
                    EditorVertex ev;
                    ev.pos = p;
                    ev.normal = glm::vec3(0, 1, 0);
                    ev.color = color;
                    ev.uv = glm::vec2(0.0f);
                    verts.push_back(ev);
                }
                const uint32_t idx[36] = {
                    0,1,2, 0,2,3, 4,5,6, 4,6,7, 0,1,5, 0,5,4, 2,3,7, 2,7,6,
                    1,2,6, 1,6,5, 0,3,7, 0,7,4,
                };
                for (uint32_t i : idx) indices.push_back(first + i);
            }
        }
    }
    if (verts.empty()) return;
    const VkDeviceSize vbSize = sizeof(EditorVertex) * verts.size();
    const VkDeviceSize ibSize = sizeof(uint32_t) * indices.size();
    create_buffer(vbSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  mesh.vb.buffer, mesh.vb.memory);
    create_buffer(ibSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  mesh.ib.buffer, mesh.ib.memory);
    void* data = nullptr;
    vkMapMemory(m_device, mesh.vb.memory, 0, vbSize, 0, &data);
    std::memcpy(data, verts.data(), static_cast<size_t>(vbSize));
    vkUnmapMemory(m_device, mesh.vb.memory);
    vkMapMemory(m_device, mesh.ib.memory, 0, ibSize, 0, &data);
    std::memcpy(data, indices.data(), static_cast<size_t>(ibSize));
    vkUnmapMemory(m_device, mesh.ib.memory);
    mesh.indexCount = static_cast<uint32_t>(indices.size());
    mesh.valid = true;
}

void EditorApplication::draw_voxel_volumes(VkCommandBuffer cmd, const glm::mat4& viewProj, Scene* scene) {
    if (!scene || m_device == VK_NULL_HANDLE) return;
    for (const auto& [id, vol] : scene->voxelVolumeComponents) {
        (void)vol;
        if (!scene->transformComponents.contains(id)) continue;
        if (m_voxelMeshesDirty.erase(id) != 0 || !m_voxelMeshes[id].valid) {
            ensure_voxel_volume(id, scene->voxelVolumeComponents[id].seed,
                                scene->voxelVolumeComponents[id].seaLevel);
            rebuild_voxel_mesh(id);
        }
        const auto& mesh = m_voxelMeshes[id];
        if (!mesh.valid || mesh.vb.buffer == VK_NULL_HANDLE) continue;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_scenePipeline);
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.vb.buffer, &offset);
        vkCmdBindIndexBuffer(cmd, mesh.ib.buffer, 0, VK_INDEX_TYPE_UINT32);
        push_constants(cmd, m_scenePipelineLayout, viewProj, glm::vec4(1.0f));
        vkCmdDrawIndexed(cmd, mesh.indexCount, 1, 0, 0, 0);
    }
}

// Paints with the active brush along a world ray. The brush settings come from
// the sculpt panel (m_activeVoxelBrush); right-drag forces Remove mode.
void EditorApplication::paint_voxel_ray(const glm::vec3& origin, const glm::vec3& dir, bool remove) {
    if (!m_editorScene) return;
    Scene* scene = m_editorScene.get();
    // Prefer the selected volume; otherwise the first one the ray hits.
    UUID target{ 0, 0 };
    if (m_selectedEntity.is_valid() && scene->voxelVolumeComponents.contains(m_selectedEntity.get_id())) {
        target = m_selectedEntity.get_id();
    }
    const auto& vols = scene->voxelVolumeComponents;
    if (!target.is_valid()) {
        float bestT = 1e18f;
        for (const auto& [id, vol] : vols) {
            (void)vol;
            const auto tit = scene->transformComponents.find(id);
            if (tit == scene->transformComponents.end()) continue;
            const glm::vec3 min = tit->second.position + glm::vec3(-kVoxelSizeX / 2, 0, -kVoxelSizeZ / 2);
            const glm::vec3 max = tit->second.position + glm::vec3(kVoxelSizeX / 2, kVoxelSizeY, kVoxelSizeZ / 2);
            const glm::vec3 inv = 1.0f / glm::max(glm::abs(dir), glm::vec3(1e-6f)) * glm::sign(dir);
            float t0 = glm::dot((min - origin), inv);
            float t1 = glm::dot((max - origin), inv);
            if (t0 > t1) std::swap(t0, t1);
            if (t0 <= t1 && t1 > 0.0f && t0 < bestT) {
                bestT = std::max(t0, 0.0f);
                target = id;
            }
        }
    }
    if (!target.is_valid()) return;
    const auto gridIt = m_voxelStructures.find(target);
    if (gridIt == m_voxelStructures.end()) return;
    const auto tit = scene->transformComponents.find(target);
    if (tit == scene->transformComponents.end()) return;

    // Ray vs grid AABB (grid-local space).
    const glm::vec3 gridMin(-kVoxelSizeX / 2, 0, -kVoxelSizeZ / 2);
    const glm::vec3 gridMax(kVoxelSizeX / 2, kVoxelSizeY, kVoxelSizeZ / 2);
    const glm::vec3 inv = 1.0f / glm::max(glm::abs(dir), glm::vec3(1e-6f)) * glm::sign(dir);
    float t0 = glm::dot((gridMin - (origin - tit->second.position)), inv);
    float t1 = glm::dot((gridMax - (origin - tit->second.position)), inv);
    if (t0 > t1) std::swap(t0, t1);
    if (t1 < 0.0f) return;
    const float hitT = std::max(t0, 0.0f);
    const glm::vec3 hitLocal = (origin - tit->second.position) + dir * hitT;
    const int hx = std::clamp(static_cast<int>(std::floor(hitLocal.x + kVoxelSizeX / 2)), 0, kVoxelSizeX - 1);
    const int hy = std::clamp(static_cast<int>(std::floor(hitLocal.y)), 0, kVoxelSizeY - 1);
    const int hz = std::clamp(static_cast<int>(std::floor(hitLocal.z + kVoxelSizeZ / 2)), 0, kVoxelSizeZ - 1);

    VoxelBrushOperation op = m_activeVoxelBrush;
    op.position = glm::vec3(hx + 0.5f, hy + 0.5f, hz + 0.5f); // grid cell space
    op.radius = std::max(m_activeVoxelBrush.radius, 0.5f);
    if (remove) op.mode = VoxelBrushMode::Remove;
    Engine::Voxel::VoxelTools::apply(*gridIt->second, op);
    m_voxelMeshesDirty.insert(target);
}

void EditorApplication::destroy_voxel_editor_meshes() {
    if (m_device == VK_NULL_HANDLE) return;
    for (auto& [id, mesh] : m_voxelMeshes) {
        (void)id;
        if (mesh.vb.buffer != VK_NULL_HANDLE) destroy_buffer(mesh.vb);
        if (mesh.ib.buffer != VK_NULL_HANDLE) destroy_buffer(mesh.ib);
    }
    m_voxelMeshes.clear();
    m_voxelStructures.clear();
    m_voxelMeshesDirty.clear();
}

bool EditorApplication::load_viewport_texture(const UUID& assetId, GraphTexture& out, std::string& error,
                                              uint32_t maxDim) {
    const auto metaOpt = m_assetRegistry.find(assetId);
    if (!metaOpt) {
        error = "texture asset not found in registry";
        return false;
    }
    const AssetMetadata& meta = *metaOpt;
    if (meta.type != AssetType::Texture || meta.cookedPath.empty()) {
        error = "asset is not a cooked texture";
        return false;
    }
    DecodedTexturePixels px;
    if (!decode_cooked_texture_pixels(meta.cookedPath, maxDim, px, error)) return false;
    if (px.halfFloat) {
        return upload_texture_half_pixels(px.width, px.height, px.rgba, out, error);
    }
    return upload_texture_pixels(px.width, px.height, px.rgba, px.mipCount, px.srgb, out, error);
}

bool EditorApplication::upload_texture_pixels(uint32_t width, uint32_t height,
                                              const std::vector<uint8_t>& rgba,
                                              uint32_t mipCount, bool srgb,
                                              GraphTexture& out, std::string& error) {
    // Import settings applied here (Fase 2): srgb selects the SRGB image
    // format and mipCount uploads the cooked mip chain (level 0 first) into a
    // mip-mapped image + view, so mipmapped textures actually sample the chain.
    const VkFormat format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    out.format = format;
    const uint32_t mips = std::max(mipCount, 1u);
    VkDeviceSize imageSize = 0;
    for (uint32_t m = 0; m < mips; ++m) {
        const uint32_t mw = std::max(width >> m, 1u);
        const uint32_t mh = std::max(height >> m, 1u);
        imageSize += static_cast<VkDeviceSize>(mw) * mh * 4;
    }
    create_image(width, height, format, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out.image, out.memory, mips);
    if (out.image == VK_NULL_HANDLE) {
        error = "texture image allocation failed";
        return false;
    }
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    create_buffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  staging, stagingMemory);
    if (staging == VK_NULL_HANDLE) {
        destroy_graph_texture(out);
        error = "texture staging buffer allocation failed";
        return false;
    }
    void* data = nullptr;
    vkMapMemory(m_device, stagingMemory, 0, imageSize, 0, &data);
    std::memcpy(data, rgba.data(), static_cast<size_t>(imageSize));
    vkUnmapMemory(m_device, stagingMemory);
    std::vector<VkBufferImageCopy> regions;
    regions.reserve(mips);
    VkDeviceSize offset = 0;
    for (uint32_t m = 0; m < mips; ++m) {
        const uint32_t mw = std::max(width >> m, 1u);
        const uint32_t mh = std::max(height >> m, 1u);
        VkBufferImageCopy region{};
        region.bufferOffset = offset;
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1 };
        region.imageExtent = { mw, mh, 1 };
        regions.push_back(region);
        offset += static_cast<VkDeviceSize>(mw) * mh * 4;
    }
    VkCommandBuffer cmd = begin_single_time_commands();
    transition_image_layout(cmd, out.image, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, mips);
    vkCmdCopyBufferToImage(cmd, staging, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<uint32_t>(regions.size()), regions.data());
    transition_image_layout(cmd, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, mips);
    end_single_time_commands(cmd);
    vkDestroyBuffer(m_device, staging, nullptr);
    vkFreeMemory(m_device, stagingMemory, nullptr);
    out.view = create_image_view(out.image, format, VK_IMAGE_ASPECT_COLOR_BIT, mips);
    if (out.view == VK_NULL_HANDLE) {
        destroy_graph_texture(out);
        error = "texture image view creation failed";
        return false;
    }
    return true;
}

// Uploads an RGBA16F (half-float RGBA) payload as an R16G16B16A16_SFLOAT image
// — the HDR path of the material graph (Radiance .hdr cooks to this layout).
bool EditorApplication::upload_texture_half_pixels(uint32_t width, uint32_t height,
                                                   const std::vector<uint8_t>& halfRgba,
                                                   GraphTexture& out, std::string& error) {
    const VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;
    out.format = format;
    const VkDeviceSize imageSize = static_cast<VkDeviceSize>(width) * height * 8;
    create_image(width, height, format, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out.image, out.memory);
    if (out.image == VK_NULL_HANDLE) {
        error = "HDR texture image allocation failed";
        return false;
    }
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    create_buffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  staging, stagingMemory);
    if (staging == VK_NULL_HANDLE) {
        destroy_graph_texture(out);
        error = "HDR texture staging buffer allocation failed";
        return false;
    }
    void* data = nullptr;
    vkMapMemory(m_device, stagingMemory, 0, imageSize, 0, &data);
    std::memcpy(data, halfRgba.data(), static_cast<size_t>(imageSize));
    vkUnmapMemory(m_device, stagingMemory);
    VkCommandBuffer cmd = begin_single_time_commands();
    transition_image_layout(cmd, out.image, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { width, height, 1 };
    vkCmdCopyBufferToImage(cmd, staging, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    transition_image_layout(cmd, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    end_single_time_commands(cmd);
    vkDestroyBuffer(m_device, staging, nullptr);
    vkFreeMemory(m_device, stagingMemory, nullptr);
    out.view = create_image_view(out.image, format, VK_IMAGE_ASPECT_COLOR_BIT);
    if (out.view == VK_NULL_HANDLE) {
        destroy_graph_texture(out);
        error = "HDR texture image view creation failed";
        return false;
    }
    return true;
}

bool EditorApplication::build_graph_pipeline(const Rendering::MaterialGraph& graph, GraphMaterialPipeline& out) {
    // Preserve the caller's cache key: out is reset below and graphHash is the
    // rebuild-detection stamp used by the per-material cache.
    const uint64_t callerGraphHash = out.graphHash;
    out = GraphMaterialPipeline{};
    out.graphHash = callerGraphHash;
    // TextureSample nodes: the texture asset UUID lives in the node value
    // (string). Binding i+1 corresponds to the i-th TextureSample in node order.
    std::vector<UUID> textureIds;
    for (const auto& node : graph.nodes()) {
        if (node.kind != Rendering::MaterialNodeKind::TextureSample) continue;
        const auto* value = std::get_if<std::string>(&node.value);
        textureIds.push_back(value && !value->empty() ? UUID::from_string(*value) : UUID{});
    }
    std::vector<GraphTexture> textures;
    textures.reserve(textureIds.size());
    for (const UUID& id : textureIds) {
        GraphTexture tex;
        std::string texError;
        if (!id.is_valid() || !load_viewport_texture(id, tex, texError)) {
            out.lastError = texError.empty()
                ? "a TextureSample node has no texture asset assigned" : texError;
            for (auto& t : textures) destroy_graph_texture(t);
            return false;
        }
        textures.push_back(std::move(tex));
    }
    out.textures = std::move(textures);
    const Rendering::GlslGenerationResult gen = material_graph_to_glsl(graph);
    if (!gen) {
        out.lastError = gen.errors.empty() ? "material graph compile failed" : gen.errors[0].message;
        return false;
    }

    const std::vector<uint32_t> vertSpv = read_spv("editor_material.vert.spv");
    if (vertSpv.empty()) {
        out.lastError = "editor_material.vert.spv is missing (re-run compile_shaders)";
        return false;
    }
    const std::vector<uint32_t> fragSpv = compile_material_glsl(VK_SHADER_STAGE_FRAGMENT_BIT, gen.source);
    if (fragSpv.empty()) {
        out.lastError = "glslc failed to compile the generated material shader";
        return false;
    }
    VkShaderModule vertModule = make_module(m_device, vertSpv);
    VkShaderModule fragModule = make_module(m_device, fragSpv);
    if (vertModule == VK_NULL_HANDLE || fragModule == VK_NULL_HANDLE) {
        if (vertModule != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, vertModule, nullptr);
        if (fragModule != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, fragModule, nullptr);
        out.lastError = "VkShaderModule creation failed";
        return false;
    }

    // Descriptor set layout: binding 0 = material params UBO; bindings 1..N =
    // combined image samplers (one per TextureSample node, node order); then the
    // LightParams UBO and the shadow-map sampler at the bindings the generated
    // shader declared.
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    bindings.reserve(3 + out.textures.size());
    VkDescriptorSetLayoutBinding uboBinding{};
    uboBinding.binding = 0;
    uboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboBinding.descriptorCount = 1;
    uboBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings.push_back(uboBinding);
    for (size_t i = 0; i < out.textures.size(); ++i) {
        VkDescriptorSetLayoutBinding texBinding{};
        texBinding.binding = static_cast<uint32_t>(i + 1);
        texBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        texBinding.descriptorCount = 1;
        texBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings.push_back(texBinding);
    }
    out.lightUboBinding = gen.lightUboBinding;
    VkDescriptorSetLayoutBinding lightBinding{};
    lightBinding.binding = out.lightUboBinding;
    lightBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    lightBinding.descriptorCount = 1;
    lightBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings.push_back(lightBinding);

    // Shadow-map sampler (dummy 1x1 white texture; shadows disabled in the
    // editor viewport).
    out.shadowSamplerBinding = gen.shadowSamplerBinding;
    {
        std::string texError;
        if (!upload_texture_pixels(1, 1, { 255, 255, 255, 255 }, 1, false, out.shadowDummy, texError)) {
            destroy_graph_pipeline(out);
            out.lastError = "shadow dummy texture failed: " + texError;
            return false;
        }
    }
    VkDescriptorSetLayoutBinding shadowBinding{};
    shadowBinding.binding = out.shadowSamplerBinding;
    shadowBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    shadowBinding.descriptorCount = 1;
    shadowBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings.push_back(shadowBinding);
    VkDescriptorSetLayoutCreateInfo dslInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dslInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    dslInfo.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(m_device, &dslInfo, nullptr, &out.descriptorSetLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(m_device, vertModule, nullptr);
        vkDestroyShaderModule(m_device, fragModule, nullptr);
        destroy_graph_pipeline(out);
        out.lastError = "descriptor set layout creation failed";
        return false;
    }

    // Pipeline layout: MVP + model push constant (vertex stage) + material set
    // (params + textures + lights, all fragment).
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(Rendering::MaterialPushConstants);
    VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &out.descriptorSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &out.layout) != VK_SUCCESS) {
        destroy_graph_pipeline(out);
        vkDestroyShaderModule(m_device, vertModule, nullptr);
        vkDestroyShaderModule(m_device, fragModule, nullptr);
        out.lastError = "pipeline layout creation failed";
        return false;
    }

    // UBO sized with std140 offsets; capture parameter defaults for per-frame writes.
    out.uniformNames = gen.uniformNames;
    out.uniformTypes = gen.uniformTypes;
    out.uniformDefaults.reserve(gen.uniformNames.size());
    out.uboSize = 0;
    for (size_t i = 0; i < gen.uniformNames.size(); ++i) {
        const auto* parameter = graph.find_parameter(gen.uniformNames[i]);
        out.uniformDefaults.push_back(parameter ? parameter->defaultValue : Rendering::MaterialValue(0.0f));
        out.uboSize = align_material_offset(out.uboSize, material_std140_alignment(gen.uniformTypes[i]));
        out.uboSize += material_std140_size(gen.uniformTypes[i]);
    }
    out.uboSize = align_material_offset(out.uboSize, 16);
    if (out.uboSize == 0) out.uboSize = 16;
    create_buffer(out.uboSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  out.uboBuffer, out.uboMemory);
    create_buffer(sizeof(Rendering::LightUboData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  out.lightBuffer, out.lightMemory);

    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = 2;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = static_cast<uint32_t>(out.textures.size()) + 1;
    VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    bool poolOk = vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &out.pool) == VK_SUCCESS;
    if (poolOk) {
        VkDescriptorSetAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        allocInfo.descriptorPool = out.pool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &out.descriptorSetLayout;
        poolOk = vkAllocateDescriptorSets(m_device, &allocInfo, &out.descriptorSet) == VK_SUCCESS;
    }
    if (!poolOk) {
        out.lastError = "descriptor pool/set allocation failed";
        destroy_graph_pipeline(out);
        return false;
    }
    std::vector<VkDescriptorImageInfo> imageInfos;
    imageInfos.reserve(out.textures.size() + 2);
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(3 + out.textures.size());
    VkDescriptorBufferInfo bufferInfo{ out.uboBuffer, 0, out.uboSize };
    VkWriteDescriptorSet uboWrite{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    uboWrite.dstSet = out.descriptorSet;
    uboWrite.descriptorCount = 1;
    uboWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboWrite.pBufferInfo = &bufferInfo;
    writes.push_back(uboWrite);
    VkDescriptorBufferInfo lightBufferInfo{ out.lightBuffer, 0, sizeof(Rendering::LightUboData) };
    VkWriteDescriptorSet lightWrite{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    lightWrite.dstSet = out.descriptorSet;
    lightWrite.dstBinding = out.lightUboBinding;
    lightWrite.descriptorCount = 1;
    lightWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    lightWrite.pBufferInfo = &lightBufferInfo;
    writes.push_back(lightWrite);
    for (size_t i = 0; i < out.textures.size(); ++i) {
        VkDescriptorImageInfo imageInfo{};
        imageInfo.sampler = m_offscreen.sampler;
        imageInfo.imageView = out.textures[i].view;
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfos.push_back(imageInfo);
        VkWriteDescriptorSet texWrite{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        texWrite.dstSet = out.descriptorSet;
        texWrite.dstBinding = static_cast<uint32_t>(i + 1);
        texWrite.descriptorCount = 1;
        texWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        texWrite.pImageInfo = &imageInfos.back();
        writes.push_back(texWrite);
    }
    VkDescriptorImageInfo shadowImageInfo{};
    // Real sun shadow map when available; the 1x1 dummy otherwise (no sun).
    shadowImageInfo.sampler = m_shadowMap.sampler != VK_NULL_HANDLE
        ? m_shadowMap.sampler : m_offscreen.sampler;
    shadowImageInfo.imageView = m_shadowMap.view != VK_NULL_HANDLE
        ? m_shadowMap.view : out.shadowDummy.view;
    shadowImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos.push_back(shadowImageInfo);
    VkWriteDescriptorSet shadowWrite{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    shadowWrite.dstSet = out.descriptorSet;
    shadowWrite.dstBinding = out.shadowSamplerBinding;
    shadowWrite.descriptorCount = 1;
    shadowWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    shadowWrite.pImageInfo = &imageInfos.back();
    writes.push_back(shadowWrite);
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

    // Graphics pipeline: same EditorVertex layout, no culling (glTF winding varies).
    out.pipeline = create_scene_pipeline(m_device, m_offscreen.renderPass, out.layout,
                                         vertModule, fragModule, m_viewportSamples,
                                         false, true, false, true);
    vkDestroyShaderModule(m_device, vertModule, nullptr);
    vkDestroyShaderModule(m_device, fragModule, nullptr);
    if (out.pipeline == VK_NULL_HANDLE) {
        out.lastError = "vkCreateGraphicsPipelines failed";
        destroy_graph_pipeline(out);
        return false;
    }
    out.lastError.clear();
    out.valid = true;
    return true;
}

void EditorApplication::destroy_graph_pipeline(GraphMaterialPipeline& p) {
    if (m_device == VK_NULL_HANDLE) return;
    if (p.pipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, p.pipeline, nullptr);
    if (p.layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, p.layout, nullptr);
    if (p.descriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(m_device, p.descriptorSetLayout, nullptr);
    if (p.pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_device, p.pool, nullptr);
    if (p.uboBuffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, p.uboBuffer, nullptr);
    if (p.uboMemory != VK_NULL_HANDLE) vkFreeMemory(m_device, p.uboMemory, nullptr);
    if (p.lightBuffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, p.lightBuffer, nullptr);
    if (p.lightMemory != VK_NULL_HANDLE) vkFreeMemory(m_device, p.lightMemory, nullptr);
    destroy_graph_texture(p.shadowDummy);
    for (auto& t : p.textures) destroy_graph_texture(t);
    p.textures.clear();
    p = GraphMaterialPipeline{};
}

void EditorApplication::destroy_graph_material_pipelines() {
    for (auto& [id, p] : m_graphMaterialPipelines) {
        (void)id;
        destroy_graph_pipeline(p);
    }
    m_graphMaterialPipelines.clear();
    for (auto& [id, p] : m_blockGraphPipelines) {
        (void)id;
        destroy_graph_pipeline(p);
    }
    m_blockGraphPipelines.clear();
    for (auto& [id, p] : m_skinGraphPipelines) {
        (void)id;
        destroy_graph_pipeline(p);
    }
    m_skinGraphPipelines.clear();
    for (auto& [id, p] : m_videoGraphPipelines) {
        (void)id;
        destroy_graph_pipeline(p);
    }
    m_videoGraphPipelines.clear();
    destroy_graph_pipeline(m_liveGraphPipeline);
    m_liveGraphHash = 0;
}

void EditorApplication::write_light_ubo(GraphMaterialPipeline& p, const Scene* scene,
                                        const glm::vec3& cameraPos) {
    if (!p.valid || p.lightBuffer == VK_NULL_HANDLE) return;
    Rendering::LightUboData data{};
    data.cameraPosition = glm::vec4(cameraPos, 1.0f);
    // Real sun shadow map: VP + enabled flag + bias + single-map mode (z=1).
    data.sunViewProj = m_shadowMap.viewProj;
    data.shadowParams = glm::vec4(m_shadowMap.enabled ? 1.0f : 0.0f, 0.0006f,
                                  m_shadowMap.enabled ? 1.0f : 0.0f, 0.0f);
    const glm::mat4 view = m_editorCamera.get_view_matrix();
    data.cameraForward = glm::vec4(glm::normalize(glm::vec3(-view[2][0], -view[2][1], -view[2][2])), 0.0f);
    uint32_t pointCount = 0, spotCount = 0, areaCount = 0;
    if (scene) {
        for (const auto& [id, light] : scene->lightComponents) {
            glm::vec3 dir(0.0f, -1.0f, 0.0f);
            glm::vec3 position(0.0f);
            const auto tit = scene->transformComponents.find(id);
            if (tit != scene->transformComponents.end()) {
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
    }
    void* mapped = nullptr;
    if (vkMapMemory(m_device, p.lightMemory, 0, sizeof(data), 0, &mapped) != VK_SUCCESS) return;
    std::memcpy(mapped, &data, sizeof(data));
    vkUnmapMemory(m_device, p.lightMemory);
}

void EditorApplication::write_material_ubo(const GraphMaterialPipeline& p, const MaterialAsset* material,
                                           const MaterialComponent* component) {
    if (!p.valid || p.uboBuffer == VK_NULL_HANDLE || p.uboSize == 0) return;
    const glm::vec3 albedo = material ? material->albedo
                                      : (component ? component->albedo : glm::vec3(1.0f, 1.0f, 1.0f));
    const float roughness = material ? material->roughness : (component ? component->roughness : 0.5f);
    const float metallic = material ? material->metallic : (component ? component->metallic : 0.0f);
    const glm::vec3 emissive = material
        ? material->emissiveColor * material->emissiveIntensity
        : (component ? component->emissiveColor * component->emissiveIntensity : glm::vec3(0.0f));
    const float emissiveIntensity = material ? material->emissiveIntensity
                                             : (component ? component->emissiveIntensity : 0.0f);
    void* mapped = nullptr;
    if (vkMapMemory(m_device, p.uboMemory, 0, p.uboSize, 0, &mapped) != VK_SUCCESS) return;
    auto* bytes = static_cast<std::byte*>(mapped);
    size_t offset = 0;
    for (size_t i = 0; i < p.uniformNames.size(); ++i) {
        offset = align_material_offset(offset, material_std140_alignment(p.uniformTypes[i]));
        if (offset + 16 > p.uboSize) break;
        Rendering::MaterialValue value = p.uniformDefaults[i];
        const std::string& name = p.uniformNames[i];
        if (name == "Albedo" || name == "BaseColor") {
            value = (p.uniformTypes[i] == Rendering::MaterialValueType::Vec4)
                ? Rendering::MaterialValue(glm::vec4(albedo, 1.0f))
                : Rendering::MaterialValue(albedo);
        } else if (name == "Roughness") {
            value = roughness;
        } else if (name == "Metallic") {
            value = metallic;
        } else if (name == "Emissive") {
            value = (p.uniformTypes[i] == Rendering::MaterialValueType::Vec4)
                ? Rendering::MaterialValue(glm::vec4(emissive, 1.0f))
                : Rendering::MaterialValue(emissive);
        } else if (name == "EmissiveIntensity") {
            value = emissiveIntensity;
        } else if (name == "Opacity") {
            value = 1.0f;
        }
        write_ubo_value(bytes + offset, p.uniformTypes[i], value);
        offset += material_std140_size(p.uniformTypes[i]);
    }
    vkUnmapMemory(m_device, p.uboMemory);
}

void EditorApplication::destroy_mesh_resources() {
    for (auto& [id, resource] : m_meshResources) {
        (void)id;
        if (resource.vb.buffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, resource.vb.buffer, nullptr);
        if (resource.vb.memory != VK_NULL_HANDLE) vkFreeMemory(m_device, resource.vb.memory, nullptr);
        if (resource.ib.buffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, resource.ib.buffer, nullptr);
        if (resource.ib.memory != VK_NULL_HANDLE) vkFreeMemory(m_device, resource.ib.memory, nullptr);
    }
    m_meshResources.clear();
    m_meshLoadFailed.clear();
}

void EditorApplication::cleanup() {
    join_worker_threads();
    shutdown_audio_output();
    if (m_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device);

        destroy_mesh_resources();
        destroy_graph_material_pipelines();
        destroy_asset_thumbnails();
        destroy_block_cube();
        destroy_thumbnail_target();
        destroy_voxel_editor_meshes();
        cleanup_offscreen_target();

        if (m_offscreen.renderPass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(m_device, m_offscreen.renderPass, nullptr);
            m_offscreen.renderPass = VK_NULL_HANDLE;
        }
        if (m_offscreen.pickRenderPass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(m_device, m_offscreen.pickRenderPass, nullptr);
            m_offscreen.pickRenderPass = VK_NULL_HANDLE;
        }
        if (m_offscreen.sampler != VK_NULL_HANDLE) {
            vkDestroySampler(m_device, m_offscreen.sampler, nullptr);
            m_offscreen.sampler = VK_NULL_HANDLE;
        }

        destroy_buffer(m_cubeVB);
        destroy_buffer(m_cubeIB);
        destroy_buffer(m_lightIconVB);
        destroy_buffer(m_cameraIconVB);
        destroy_buffer(m_gizmoVB);
        destroy_buffer(m_gizmoIB);
        destroy_buffer(m_terrainVB);
        destroy_buffer(m_terrainIB);
        m_terrainValid = false;
        // Runtime-wired Wicked-port resources.
        destroy_buffer(m_envSphereVB);
        destroy_buffer(m_envSphereIB);
        destroy_buffer(m_decalVB);
        destroy_buffer(m_decalIB);
        for (auto& [id, sim] : m_hairs) { (void)id; destroy_buffer(sim.vb); }
        m_hairs.clear();
        for (auto& [id, sim] : m_softBodies) { (void)id; destroy_buffer(sim.vb); destroy_buffer(sim.ib); }
        m_softBodies.clear();
        for (auto& [id, cloud] : m_splatClouds) { (void)id; destroy_buffer(cloud.vb); }
        m_splatClouds.clear();
        for (auto& [id, pb] : m_paintBuffers) { (void)id; destroy_buffer(pb.vb); }
        m_paintBuffers.clear();
        if (m_envCapture.valid) {
            for (int i = 0; i < 6; ++i) {
                if (m_envCapture.framebuffers[i]) vkDestroyFramebuffer(m_device, m_envCapture.framebuffers[i], nullptr);
                if (m_envCapture.views[i]) vkDestroyImageView(m_device, m_envCapture.views[i], nullptr);
            }
            if (m_envCapture.image) vkDestroyImage(m_device, m_envCapture.image, nullptr);
            if (m_envCapture.memory) vkFreeMemory(m_device, m_envCapture.memory, nullptr);
            if (m_envCapture.renderPass) vkDestroyRenderPass(m_device, m_envCapture.renderPass, nullptr);
            if (m_envCapture.sampler) vkDestroySampler(m_device, m_envCapture.sampler, nullptr);
        }
        if (m_envCubeView) vkDestroyImageView(m_device, m_envCubeView, nullptr);
        if (m_envDepthView) vkDestroyImageView(m_device, m_envDepthView, nullptr);
        if (m_envDepthImage) vkDestroyImage(m_device, m_envDepthImage, nullptr);
        if (m_envDepthMemory) vkFreeMemory(m_device, m_envDepthMemory, nullptr);
        if (m_envSphereDescLayout) vkDestroyDescriptorSetLayout(m_device, m_envSphereDescLayout, nullptr);
        if (m_envSphereDescPool) vkDestroyDescriptorPool(m_device, m_envSphereDescPool, nullptr);
        if (m_envSpherePipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, m_envSpherePipeline, nullptr);
        if (m_envSpherePipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, m_envSpherePipelineLayout, nullptr);
        if (m_envSphereFragShader != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, m_envSphereFragShader, nullptr);
        if (m_envSphereVertShader != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, m_envSphereVertShader, nullptr);
        if (m_splatPipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, m_splatPipeline, nullptr);
        if (m_splatPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, m_splatPipelineLayout, nullptr);
        if (m_splatFragShader != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, m_splatFragShader, nullptr);
        if (m_splatVertShader != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, m_splatVertShader, nullptr);
        if (m_hairPipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, m_hairPipeline, nullptr);

        if (m_pickPipeline != VK_NULL_HANDLE) { vkDestroyPipeline(m_device, m_pickPipeline, nullptr); m_pickPipeline = VK_NULL_HANDLE; }
        if (m_gizmoPipeline != VK_NULL_HANDLE) { vkDestroyPipeline(m_device, m_gizmoPipeline, nullptr); m_gizmoPipeline = VK_NULL_HANDLE; }
        if (m_wireframePipeline != VK_NULL_HANDLE) { vkDestroyPipeline(m_device, m_wireframePipeline, nullptr); m_wireframePipeline = VK_NULL_HANDLE; }
        if (m_scenePipeline != VK_NULL_HANDLE) { vkDestroyPipeline(m_device, m_scenePipeline, nullptr); m_scenePipeline = VK_NULL_HANDLE; }
        if (m_scenePipelineLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(m_device, m_scenePipelineLayout, nullptr); m_scenePipelineLayout = VK_NULL_HANDLE; }
        if (m_gridPipeline != VK_NULL_HANDLE) { vkDestroyPipeline(m_device, m_gridPipeline, nullptr); m_gridPipeline = VK_NULL_HANDLE; }
        if (m_gridPipelineLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(m_device, m_gridPipelineLayout, nullptr); m_gridPipelineLayout = VK_NULL_HANDLE; }
        if (m_gridFragShader != VK_NULL_HANDLE) { vkDestroyShaderModule(m_device, m_gridFragShader, nullptr); m_gridFragShader = VK_NULL_HANDLE; }
        if (m_gridVertShader != VK_NULL_HANDLE) { vkDestroyShaderModule(m_device, m_gridVertShader, nullptr); m_gridVertShader = VK_NULL_HANDLE; }
        if (m_pickFragShader != VK_NULL_HANDLE) { vkDestroyShaderModule(m_device, m_pickFragShader, nullptr); m_pickFragShader = VK_NULL_HANDLE; }
        if (m_viewportFragShader != VK_NULL_HANDLE) { vkDestroyShaderModule(m_device, m_viewportFragShader, nullptr); m_viewportFragShader = VK_NULL_HANDLE; }
        if (m_viewportVertShader != VK_NULL_HANDLE) { vkDestroyShaderModule(m_device, m_viewportVertShader, nullptr); m_viewportVertShader = VK_NULL_HANDLE; }

        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();

        if (m_imguiDescriptorPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(m_device, m_imguiDescriptorPool, nullptr);
        }

        for (size_t i = 0; i < 2; i++) {
            vkDestroySemaphore(m_device, m_imageAvailableSemaphores[i], nullptr);
            vkDestroySemaphore(m_device, m_renderFinishedSemaphores[i], nullptr);
            vkDestroyFence(m_device, m_inFlightFences[i], nullptr);
        }

        if (m_commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(m_device, m_commandPool, nullptr);
        }

        for (auto fb : m_framebuffers) {
            vkDestroyFramebuffer(m_device, fb, nullptr);
        }

        if (m_renderPass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(m_device, m_renderPass, nullptr);
        }

        for (auto view : m_swapchainViews) {
            vkDestroyImageView(m_device, view, nullptr);
        }

        if (m_swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
        }

        vkDestroyDevice(m_device, nullptr);
        // run() calls cleanup() and the destructor calls it again — reset the
        // device so the second pass is a no-op. Without this, the second pass
        // called vkDeviceWaitIdle on the already-destroyed handle
        // ("vkDeviceWaitIdle: Invalid device" from the loader at shutdown).
        m_device = VK_NULL_HANDLE;
        vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
        vkDestroyInstance(m_instance, nullptr);

        if (m_window) {
            glfwDestroyWindow(m_window);
            glfwTerminate();
        }
    }
}

// ===========================================================================
// File/folder pickers + scene loading (Abrir Jogo / Procurar Pasta).
// ===========================================================================

bool EditorApplication::pick_file_dialog(std::string& outPath, const wchar_t* filter,
                                         const wchar_t* title, const wchar_t* defExt) {
    wchar_t buf[MAX_PATH]{ 0 };
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = glfwGetWin32Window(m_window);
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.lpstrDefExt = defExt;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return false;
    const int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return false;
    std::string path(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, path.data(), len, nullptr, nullptr);
    outPath = path;
    return true;
}

bool EditorApplication::pick_folder_dialog(std::string& outPath, const wchar_t* title) {
    wchar_t buf[MAX_PATH]{ 0 };
    BROWSEINFOW bi{};
    bi.hwndOwner = glfwGetWin32Window(m_window);
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return false;
    if (!SHGetPathFromIDListW(pidl, buf)) {
        CoTaskMemFree(pidl);
        return false;
    }
    CoTaskMemFree(pidl);
    const int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return false;
    std::string path(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, path.data(), len, nullptr, nullptr);
    outPath = path;
    return true;
}

void EditorApplication::load_scene_file(const std::string& path) {
    auto scene = std::make_unique<Scene>("Untitled Scene");
    if (!scene->load_from_file(path)) {
        std::cerr << "[Editor] Falha ao abrir cena: " << path << std::endl;
        return;
    }
    if (m_playMode.get_state() != PlayState::Edit) m_playMode.stop_play();
    m_editorScene = std::move(scene);
    m_activeScenePath = path;
    m_editorGui.init(m_editorScene.get(), &m_undo);
    m_editorGui.set_asset_registry(&m_assetRegistry);
    m_selectedEntity = Entity();
    m_editorGui.select_entity(m_selectedEntity);
    // Give the scene a camera if it lacks one, so the viewport is usable.
    bool hasCamera = false;
    for (const auto& [id, ent] : m_editorScene->get_entities()) {
        if (m_editorScene->cameraComponents.contains(id)) { hasCamera = true; break; }
    }
    if (!hasCamera) {
        Entity cam = m_editorScene->create_entity(tr("Câmera Principal", "Main Camera"));
        m_editorScene->transformComponents[cam.get_id()].position = glm::vec3(0.0f, 2.0f, 5.0f);
        m_editorScene->cameraComponents[cam.get_id()] = CameraComponent{ 70.0f, 0.1f, 2000.0f, true };
    }
    std::cout << "[Editor] Cena carregada: " << path << " ("
              << m_editorScene->get_entities().size() << " entidades)" << std::endl;
}

void EditorApplication::scan_projects(std::vector<LauncherProject>& out) const {
    const std::filesystem::path projectsDir =
        std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Projects";
    std::error_code ec;
    if (!std::filesystem::exists(projectsDir, ec)) return;
    for (const auto& entry : std::filesystem::directory_iterator(projectsDir, ec)) {
        if (!entry.is_directory()) continue;
        LauncherProject proj;
        proj.name = entry.path().filename().string();
        proj.path = entry.path().string();
        // Last write time of the folder tree, best-effort.
        auto ftime = std::filesystem::last_write_time(entry.path(), ec);
        if (!ec) {
            const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
            const std::time_t t = std::chrono::system_clock::to_time_t(sys);
            char buf[64]{ 0 };
            std::strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M", std::localtime(&t));
            proj.lastModified = buf;
        } else {
            proj.lastModified = "—";
        }
        // Does the project contain a .scene anywhere under it?
        std::error_code subEc;
        for (const auto& file : std::filesystem::recursive_directory_iterator(entry.path(), subEc)) {
            if (file.is_regular_file() && file.path().extension() == ".scene") {
                proj.hasScene = true;
                break;
            }
        }
        out.push_back(std::move(proj));
    }
    // Stable, predictable order.
    std::sort(out.begin(), out.end(),
              [](const LauncherProject& a, const LauncherProject& b) { return a.name < b.name; });
}

void EditorApplication::save_current_scene() {
    if (!m_editorScene) return;
    if (!m_activeScenePath.empty()) {
        if (!m_editorScene->save_to_file(m_activeScenePath)) {
            std::cerr << "[Editor] Falha ao salvar: " << m_activeScenePath << std::endl;
        } else {
            std::cout << "[Editor] Cena salva: " << m_activeScenePath << std::endl;
        }
        return;
    }
    // No path yet — behave like Salvar Como.
    save_scene_as();
}

void EditorApplication::save_scene_as() {
    if (!m_editorScene) return;
    std::string path;
    if (!pick_save_file_dialog(path, L"Cenas VulkanCraft (*.scene)\0*.scene\0Todos (*.*)\0*.*\0",
                               L"Salvar Cena Como", L"scene")) {
        return;
    }
    if (!m_editorScene->save_to_file(path)) {
        std::cerr << "[Editor] Falha ao salvar: " << path << std::endl;
        return;
    }
    m_activeScenePath = path;
    std::cout << "[Editor] Cena salva: " << path << std::endl;
}

void EditorApplication::create_new_scene() {
    // Stop the play world first so it doesn't keep ticking the old scene.
    if (m_playMode.get_state() != PlayState::Edit) {
        teardown_play_runtime();
        m_playMode.stop_play();
    }
    m_selectedEntity = Entity();
    m_editorGui.select_entity(m_selectedEntity);
    const std::string name = (m_newSceneName[0] != '\0') ? m_newSceneName : "Untitled Scene";
    m_editorScene = std::make_unique<Scene>(name);
    m_activeScenePath.clear();  // new scene has no file until Salvar
    init_default_scene();
    std::cout << "[Editor] Nova cena: " << name << std::endl;
}

bool EditorApplication::pick_save_file_dialog(std::string& outPath, const wchar_t* filter,
                                              const wchar_t* title, const wchar_t* defExt) {
    wchar_t buf[MAX_PATH]{ 0 };
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = glfwGetWin32Window(m_window);
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.lpstrDefExt = defExt;
    // Default folder: the engine's scenes folder (works from any cwd).
    const std::filesystem::path defaultDir =
        std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "assets" / "scenes";
    std::error_code ec;
    std::filesystem::create_directories(defaultDir, ec);
    std::wstring initialDir = defaultDir.wstring();
    ofn.lpstrInitialDir = initialDir.c_str();
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return false;
    const int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return false;
    std::string path(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, path.data(), len, nullptr, nullptr);
    outPath = path;
    return true;
}

// ===========================================================================
// Terreno (Terrain panel): procedural heightmap mesh + static play body.
// ===========================================================================
void EditorApplication::generate_terrain_mesh(const TerrainParams& params) {
    // Drop the previous GPU buffers before regenerating.
    if (m_terrainVB.buffer != VK_NULL_HANDLE) { destroy_buffer(m_terrainVB); m_terrainVB = GPUBuffer{}; }
    if (m_terrainIB.buffer != VK_NULL_HANDLE) { destroy_buffer(m_terrainIB); m_terrainIB = GPUBuffer{}; }
    m_terrainValid = false;
    m_terrainParams = params;
    m_terrainIndexCount = 0;

    // Hash-based value noise + fBm octaves (deterministic, no RNG state).
    const auto hash2 = [](int x, int z) -> float {
        uint32_t n = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(z) * 668265263u;
        n = (n ^ (n >> 13)) * 1274126177u;
        n ^= (n >> 16);
        return static_cast<float>(n & 0xFFFFu) / 65535.0f;
    };
    const auto smoothT = [](float t) { return t * t * (3.0f - 2.0f * t); };
    const auto valueNoise = [&](float x, float z) {
        const int xi = static_cast<int>(std::floor(x));
        const int zi = static_cast<int>(std::floor(z));
        const float xf = smoothT(x - std::floor(x));
        const float zf = smoothT(z - std::floor(z));
        const float a = hash2(xi, zi), b = hash2(xi + 1, zi);
        const float c = hash2(xi, zi + 1), d = hash2(xi + 1, zi + 1);
        return a + (b - a) * xf + (c - a) * zf + (a - b - c + d) * xf * zf;
    };
    const auto fbm = [&](float x, float z, int octaves) {
        float amp = 1.0f, freq = 1.0f, sum = 0.0f, norm = 0.0f;
        for (int o = 0; o < octaves; ++o) {
            sum += amp * valueNoise(x * freq, z * freq);
            norm += amp;
            amp *= 0.5f;
            freq *= 2.0f;
        }
        return sum / std::max(norm, 1e-6f);
    };

    const int segments = params.segments;
    const float half = params.halfExtent;
    const float step = (2.0f * half) / static_cast<float>(segments);

    std::vector<EditorVertex> verts;
    std::vector<uint32_t> indices;
    verts.reserve(static_cast<size_t>(segments + 1) * (segments + 1));

    // Height pass: y = fbm(x, z) with a radial falloff that pulls the border
    // back to 0 so the sheet blends with the infinite grid.
    const size_t cols = static_cast<size_t>(segments + 1);
    for (int zi = 0; zi <= segments; ++zi) {
        for (int xi = 0; xi <= segments; ++xi) {
            const float x = -half + static_cast<float>(xi) * step;
            const float z = -half + static_cast<float>(zi) * step;
            const float dist = std::sqrt(x * x + z * z);
            const float falloff = glm::clamp(1.0f - (dist / half) * params.falloff, 0.0f, 1.0f);
            const float h = (fbm(x / params.scale, z / params.scale, params.octaves) - 0.5f)
                            * 2.0f * params.amount * 20.0f * falloff;
            EditorVertex v;
            v.pos = glm::vec3(x, h, z);
            v.normal = glm::vec3(0.0f, 1.0f, 0.0f);
            v.color = glm::vec3(0.55f, 0.62f, 0.50f);
            v.uv = glm::vec2((x + half) / (2.0f * half), (z + half) / (2.0f * half));
            verts.push_back(v);
        }
    }
    // Indexed grid: two triangles per cell.
    for (int zi = 0; zi < segments; ++zi) {
        for (int xi = 0; xi < segments; ++xi) {
            const uint32_t a = static_cast<uint32_t>(zi) * static_cast<uint32_t>(cols) + static_cast<uint32_t>(xi);
            const uint32_t b = a + 1;
            const uint32_t c = a + static_cast<uint32_t>(cols);
            const uint32_t d = c + 1;
            indices.push_back(a); indices.push_back(c); indices.push_back(b);
            indices.push_back(b); indices.push_back(c); indices.push_back(d);
        }
    }
    // Smooth normals: area-weighted accumulation from the triangle faces.
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const glm::vec3& p0 = verts[indices[i]].pos;
        const glm::vec3& p1 = verts[indices[i + 1]].pos;
        const glm::vec3& p2 = verts[indices[i + 2]].pos;
        const glm::vec3 n = glm::cross(p1 - p0, p2 - p0);
        verts[indices[i]].normal += n;
        verts[indices[i + 1]].normal += n;
        verts[indices[i + 2]].normal += n;
    }
    for (EditorVertex& v : verts) {
        const float len = glm::length(v.normal);
        v.normal = len > 1e-8f ? v.normal / len : glm::vec3(0.0f, 1.0f, 0.0f);
    }

    // GPU buffers (host-visible, same as the other editor meshes).
    const VkDeviceSize vbSize = sizeof(EditorVertex) * verts.size();
    const VkDeviceSize ibSize = sizeof(uint32_t) * indices.size();
    create_buffer(vbSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  m_terrainVB.buffer, m_terrainVB.memory);
    create_buffer(ibSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  m_terrainIB.buffer, m_terrainIB.memory);
    m_terrainVB.size = vbSize;
    m_terrainIB.size = ibSize;
    void* data = nullptr;
    vkMapMemory(m_device, m_terrainVB.memory, 0, vbSize, 0, &data);
    std::memcpy(data, verts.data(), static_cast<size_t>(vbSize));
    vkUnmapMemory(m_device, m_terrainVB.memory);
    vkMapMemory(m_device, m_terrainIB.memory, 0, ibSize, 0, &data);
    std::memcpy(data, indices.data(), static_cast<size_t>(ibSize));
    vkUnmapMemory(m_device, m_terrainIB.memory);
    m_terrainIndexCount = static_cast<uint32_t>(indices.size());
    m_terrainValid = true;
    std::cout << "[Editor] Terreno gerado: " << cols * cols << " vértices, "
              << indices.size() / 3 << " triângulos" << std::endl;
}

// ===========================================================================
// Criador de Projetos (Project Creator panel): folder + empty scene on disk.
// ===========================================================================
std::string EditorApplication::create_project(const std::string& name, const std::string& folder) {
    if (name.empty()) return "Erro: nome do projeto vazio.";
    // Sanitize into a folder-safe slug.
    std::string slug = name;
    for (char& c : slug) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '_';
    }
    std::filesystem::path root;
    if (!folder.empty()) {
        root = std::filesystem::path(folder) / slug;
    } else {
        root = std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Projects" / slug;
    }
    std::error_code ec;
    if (std::filesystem::exists(root, ec)) return "Erro: a pasta já existe: " + root.string();

    const std::filesystem::path scenesDir = root / "assets" / "scenes";
    std::filesystem::create_directories(scenesDir, ec);
    if (ec) return "Erro: não foi possível criar " + root.string();

    // Fresh scene with the same defaults as the editor (camera + sun).
    Scene scene(name);
    Entity camera = scene.create_entity("Câmera Principal");
    scene.transformComponents[camera.get_id()].position = glm::vec3(0.0f, 2.0f, 5.0f);
    scene.cameraComponents[camera.get_id()] = CameraComponent{ 70.0f, 0.1f, 2000.0f, true };
    Entity sun = scene.create_entity("Luz Direcional");
    scene.lightComponents[sun.get_id()] = LightComponent{ glm::vec3(1.0f, 0.95f, 0.85f), 10000.0f, 1000.0f, true };
    scene.transformComponents[sun.get_id()].rotation = glm::vec3(-45.0f, 30.0f, 0.0f);
    const std::filesystem::path scenePath = scenesDir / "active_world.scene";
    if (!scene.save_to_file(scenePath.string())) {
        return "Erro: falha ao salvar a cena inicial.";
    }

    // Empty asset registry (Content Browser starts clean).
    AssetRegistry reg;
    const std::filesystem::path regPath = root / "Intermediate" / "AssetRegistry.db";
    std::filesystem::create_directories(regPath.parent_path(), ec);
    reg.save(regPath.string());

    // README marker so the folder reads as a project at a glance.
    std::ofstream readme(root / "README.md");
    readme << "# " << name << "\n\nProjeto criado pelo Criador de Projetos do editor.\n";
    readme.close();

    std::cout << "[Editor] Projeto criado: " << root.string() << std::endl;
    return "OK: " + root.string();
}

// ===========================================================================
// Configurações (Opções Gerais / Tema): settings.json persistence.
// ===========================================================================
void EditorApplication::save_settings() {
    if (m_settingsPath.empty()) {
        m_settingsPath = (std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "settings.json").string();
    }
    std::ofstream out(m_settingsPath, std::ios::trunc);
    if (!out) {
        std::cerr << "[Editor] Não foi possível salvar " << m_settingsPath << std::endl;
        return;
    }
    const glm::vec3 bg = m_wickedTools.theme_background();
    const glm::vec3 panel = m_wickedTools.theme_panel();
    out << "{\n";
    out << "  \"language\": \"" << (m_currentLanguage == EngineLanguage::PT_BR ? "pt" : "en") << "\",\n";
    out << "  \"vsync\": " << (m_vsyncEnabled ? "true" : "false") << ",\n";
    out << "  \"shadowQuality\": " << m_shadowQuality << ",\n";
    out << "  \"themeBg\": [" << bg.r << ", " << bg.g << ", " << bg.b << "],\n";
    out << "  \"themePanel\": [" << panel.r << ", " << panel.g << ", " << panel.b << "]\n";
    out << "}\n";
    std::cout << "[Editor] Configurações salvas: " << m_settingsPath << std::endl;
}

void EditorApplication::load_settings() {
    m_settingsPath = (std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "settings.json").string();
    std::ifstream in(m_settingsPath);
    if (!in) return;
    const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    // Tiny hand-rolled key/value reader for our own settings format.
    const auto findString = [&](const std::string& key) -> std::string {
        const std::string token = "\"" + key + "\"";
        const size_t pos = content.find(token);
        if (pos == std::string::npos) return {};
        const size_t colon = content.find(':', pos + token.size());
        if (colon == std::string::npos) return {};
        size_t start = colon + 1;
        while (start < content.size() && std::isspace(static_cast<unsigned char>(content[start]))) ++start;
        if (start >= content.size()) return {};
        if (content[start] == '"') {
            const size_t end = content.find('"', start + 1);
            return end == std::string::npos ? std::string() : content.substr(start + 1, end - start - 1);
        }
        const size_t end = content.find_first_of(",}\n", start);
        std::string val = content.substr(start, end == std::string::npos ? std::string::npos : end - start);
        while (!val.empty() && std::isspace(static_cast<unsigned char>(val.back()))) val.pop_back();
        return val;
    };
    const auto findVec = [&](const std::string& key) -> glm::vec3 {
        const std::string token = "\"" + key + "\"";
        const size_t pos = content.find(token);
        if (pos == std::string::npos) return glm::vec3(-1.0f);
        const size_t lb = content.find('[', pos);
        const size_t rb = lb == std::string::npos ? std::string::npos : content.find(']', lb);
        if (lb == std::string::npos || rb == std::string::npos) return glm::vec3(-1.0f);
        const std::string arr = content.substr(lb + 1, rb - lb - 1);
        glm::vec3 v(0.0f);
        size_t i = 0;
        for (int comp = 0; comp < 3; ++comp) {
            while (i < arr.size() && (arr[i] == ' ' || arr[i] == ',')) ++i;
            const size_t start = i;
            while (i < arr.size() && arr[i] != ',' && arr[i] != ' ') ++i;
            if (i > start) v[static_cast<size_t>(comp)] = std::stof(arr.substr(start, i - start));
        }
        return v;
    };

    const std::string lang = findString("language");
    if (lang == "en") m_currentLanguage = EngineLanguage::EN_US;
    else if (lang == "pt") m_currentLanguage = EngineLanguage::PT_BR;
    const std::string vsync = findString("vsync");
    if (vsync == "false") m_vsyncEnabled = false;
    else if (vsync == "true") m_vsyncEnabled = true;
    const std::string quality = findString("shadowQuality");
    if (!quality.empty()) m_shadowQuality = std::clamp(std::atoi(quality.c_str()), 1, 4);
    // Theme colors are parsed but NOT reapplied on boot: the Forge light
    // design system is the base theme, and the Theme Editor panel tunes the
    // live style during the session (persisting it is a TODO(frontend-port)).
    const glm::vec3 bg = findVec("themeBg");
    const glm::vec3 panel = findVec("themePanel");
    if (bg.x >= 0.0f && panel.x >= 0.0f) {
        m_wickedTools.set_theme(bg, panel);
    }
    std::cout << "[Editor] Configurações carregadas: " << m_settingsPath << std::endl;
}

// ===========================================================================
// Opções Gráficas: VSync (swapchain) + resolução do mapa de sombras.
// ===========================================================================
uint32_t EditorApplication::shadow_size_from_quality(int quality) const {
    switch (std::clamp(quality, 1, 4)) {
        case 1: return 512;
        case 2: return 1024;
        case 3: return 2048;
        default: return 4096;
    }
}

void EditorApplication::apply_graphics_settings(bool vsync, int quality) {
    const bool vsyncChanged = m_vsyncEnabled != vsync;
    m_vsyncEnabled = vsync;
    m_shadowQuality = std::clamp(quality, 1, 4);
    if (vsyncChanged) m_recreateSwapchain = true;
    m_recreateShadowMap = true;
    std::cout << "[Editor] Gráficas: vsync=" << (vsync ? "on" : "off")
              << ", sombras=" << shadow_size_from_quality(m_shadowQuality) << std::endl;
}

// ===========================================================================
// Malha (Mesh panel): recalc/flip normals on the selected entity's mesh asset.
// ===========================================================================
std::string EditorApplication::apply_mesh_normals(int mode) {
    if (!m_selectedEntity.is_valid()) return "Nenhum objeto selecionado.";
    Scene* scene = m_editorScene.get();
    if (!scene) return "Sem cena aberta.";
    const UUID id = m_selectedEntity.get_id();
    const auto meshIt = scene->meshRendererComponents.find(id);
    if (meshIt == scene->meshRendererComponents.end() || !meshIt->second.meshAssetID.is_valid()) {
        return "O objeto selecionado não tem malha.";
    }
    const UUID assetId = meshIt->second.meshAssetID;
    const auto found = m_assetRegistry.find(assetId);
    if (!found || found->type != AssetType::Mesh || found->cookedPath.empty() ||
        !std::filesystem::is_regular_file(found->cookedPath)) {
        return "Asset de malha não encontrado (o modelo precisa ser importado/cookado).";
    }

    std::string error;
    GltfGeometryResult geometry = GltfGeometryParser::parse_vcmesh(found->cookedPath, &error);
    if (!geometry.success) return "Falha ao ler a malha: " + error;

    for (GltfMeshPrimitive& prim : geometry.primitives) {
        if (mode == 1) {
            // Flip: negate the existing normals.
            for (glm::vec3& n : prim.normals) n = -n;
            continue;
        }
        // Recalc smooth: area-weighted accumulation per vertex.
        std::vector<glm::vec3> acc(prim.positions.size(), glm::vec3(0.0f));
        if (prim.indexed && prim.indices.size() >= 3) {
            for (size_t i = 0; i + 2 < prim.indices.size(); i += 3) {
                const uint32_t ia = prim.indices[i], ib = prim.indices[i + 1], ic = prim.indices[i + 2];
                if (ia >= prim.positions.size() || ib >= prim.positions.size() || ic >= prim.positions.size()) continue;
                const glm::vec3 n = glm::cross(prim.positions[ib] - prim.positions[ia],
                                               prim.positions[ic] - prim.positions[ia]);
                acc[ia] += n; acc[ib] += n; acc[ic] += n;
            }
        } else {
            for (size_t i = 0; i + 2 < prim.positions.size(); i += 3) {
                const glm::vec3 n = glm::cross(prim.positions[i + 1] - prim.positions[i],
                                               prim.positions[i + 2] - prim.positions[i]);
                acc[i] += n; acc[i + 1] += n; acc[i + 2] += n;
            }
        }
        prim.normals.resize(prim.positions.size());
        for (size_t i = 0; i < prim.positions.size(); ++i) {
            const float len = glm::length(acc[i]);
            prim.normals[i] = len > 1e-8f ? acc[i] / len : glm::vec3(0.0f, 1.0f, 0.0f);
        }
    }

    // Re-upload the GPU resource in place (same vertex layout as load_mesh_resource).
    const float meshScale = found->importSettings.meshScale > 0.0f ? found->importSettings.meshScale : 1.0f;
    if (const auto it = m_meshResources.find(assetId); it != m_meshResources.end() && it->second.valid) {
        std::vector<EditorVertex> verts;
        std::vector<uint32_t> indices;
        for (const GltfMeshPrimitive& primitive : geometry.primitives) {
            const uint32_t vertexOffset = static_cast<uint32_t>(verts.size());
            verts.reserve(verts.size() + primitive.positions.size());
            for (size_t i = 0; i < primitive.positions.size(); ++i) {
                EditorVertex v;
                v.pos = primitive.positions[i] * meshScale;
                v.normal = i < primitive.normals.size() ? primitive.normals[i] : glm::vec3(0.0f, 1.0f, 0.0f);
                v.color = glm::vec3(1.0f);
                v.uv = i < primitive.uvs.size() ? primitive.uvs[i] : glm::vec2(0.0f);
                verts.push_back(v);
            }
            if (primitive.indexed) {
                for (uint32_t index : primitive.indices) indices.push_back(index + vertexOffset);
            }
        }
        const VkDeviceSize vbSize = sizeof(EditorVertex) * verts.size();
        const VkDeviceSize expected = sizeof(EditorVertex) * it->second.vertexCount;
        if (vbSize == expected && it->second.vb.buffer != VK_NULL_HANDLE) {
            void* data = nullptr;
            vkMapMemory(m_device, it->second.vb.memory, 0, vbSize, 0, &data);
            std::memcpy(data, verts.data(), static_cast<size_t>(vbSize));
            vkUnmapMemory(m_device, it->second.vb.memory);
        }
    }

    // Persist: rewrite the cooked mesh so the change survives a restart.
    if (!GltfGeometryParser::write_cooked(found->cookedPath, geometry, &error)) {
        return std::string(mode == 0 ? "Normais recalculadas (somente em memória): "
                                     : "Normais invertidas (somente em memória): ")
               + error;
    }
    m_meshEdited = true;
    return mode == 0 ? "Normais recalculadas e salvas no cook."
                     : "Normais invertidas e salvas no cook.";
}

} // namespace Engine
