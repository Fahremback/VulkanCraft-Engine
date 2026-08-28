#include "EditorApplication.hpp"
#include "EditorInternalHelpers.hpp"
// Importação de textura via WIC (iwicimagingfactory/ComPtr) — includes do
// monólito original que o split havia perdido.
#include <wincodec.h>
#include <wrl/client.h>

namespace Engine {

namespace {
// Cluster restaurado do monólito (git 408c2d3 4460-4524): geometria de skin
// de personagem usada pela importação de avatar — o split havia perdido.
struct CharacterUVRect { float u0, v0, u1, v1; };

void append_character_face(std::vector<EditorVertex>& verts, std::vector<uint32_t>& indices,
                           const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
                           const glm::vec3& d, const glm::vec2& ta, const glm::vec2& tb,
                           const glm::vec2& tc, const glm::vec2& td, const glm::vec3& normal) {
    const uint32_t base = static_cast<uint32_t>(verts.size());
    const glm::vec3 p[4] = { a, b, c, d };
    const glm::vec2 t[4] = { ta, tb, tc, td };
    for (int i = 0; i < 4; ++i) {
        EditorVertex v;
        v.pos = p[i];
        v.normal = normal;
        v.color = glm::vec3(1.0f);
        v.uv = t[i];
        verts.push_back(v);
    }
    indices.push_back(base);
    indices.push_back(base + 1);
    indices.push_back(base + 2);
    indices.push_back(base);
    indices.push_back(base + 2);
    indices.push_back(base + 3);
}

// Adds the six faces of a box. `right`/`left` map the +X/-X faces,
// `front`/`back` the +Z/-Z (back flipped so the layout reads correctly),
// `top`/`bottom` the +Y/-Y. UVs come from the 64-unit layout grid and are
// normalized by `skinHeight` (64x64 or legacy 64x32).
void append_character_box(std::vector<EditorVertex>& verts, std::vector<uint32_t>& indices,
                          float x0, float y0, float z0, float x1, float y1, float z1,
                          float skinHeight, const CharacterUVRect& right,
                          const CharacterUVRect& left, const CharacterUVRect& front,
                          const CharacterUVRect& back, const CharacterUVRect& top,
                          const CharacterUVRect& bottom) {
    const auto uv = [&](const CharacterUVRect& r, float u, float v) {
        return glm::vec2((r.u0 + (r.u1 - r.u0) * u) / 64.0f,
                         (r.v0 + (r.v1 - r.v0) * v) / skinHeight);
    };
    // +Z front
    append_character_face(verts, indices, { x0, y0, z1 }, { x1, y0, z1 }, { x1, y1, z1 },
                          { x0, y1, z1 }, uv(front, 0, 1), uv(front, 1, 1), uv(front, 1, 0),
                          uv(front, 0, 0), { 0, 0, 1 });
    // -Z back (u flipped)
    append_character_face(verts, indices, { x1, y0, z0 }, { x0, y0, z0 }, { x0, y1, z0 },
                          { x1, y1, z0 }, uv(back, 1, 1), uv(back, 0, 1), uv(back, 0, 0),
                          uv(back, 1, 0), { 0, 0, -1 });
    // +X right
    append_character_face(verts, indices, { x1, y0, z0 }, { x1, y0, z1 }, { x1, y1, z1 },
                          { x1, y1, z0 }, uv(right, 0, 1), uv(right, 1, 1), uv(right, 1, 0),
                          uv(right, 0, 0), { 1, 0, 0 });
    // -X left (u flipped)
    append_character_face(verts, indices, { x0, y0, z1 }, { x0, y0, z0 }, { x0, y1, z0 },
                          { x0, y1, z1 }, uv(left, 1, 1), uv(left, 0, 1), uv(left, 0, 0),
                          uv(left, 1, 0), { -1, 0, 0 });
    // +Y top (z1 -> v0, the front edge of the top rect)
    append_character_face(verts, indices, { x0, y1, z1 }, { x1, y1, z1 }, { x1, y1, z0 },
                          { x0, y1, z0 }, uv(top, 0, 0), uv(top, 1, 0), uv(top, 1, 1),
                          uv(top, 0, 1), { 0, 1, 0 });
    // -Y bottom
    append_character_face(verts, indices, { x0, y0, z0 }, { x1, y0, z0 }, { x1, y0, z1 },
                          { x0, y0, z1 }, uv(bottom, 0, 1), uv(bottom, 1, 1),
                          uv(bottom, 1, 0), uv(bottom, 0, 0), { 0, -1, 0 });
}
} // namespace

void build_character_geometry(float skinHeight, std::vector<EditorVertex>& verts,
                              std::vector<uint32_t>& indices) {
    // Skin layout rects in the 64x64 coordinate grid (authoritative: the
    // reference implementation used by mineatar.io). v is normalized by
    // skinHeight so legacy 64x32 skins work too.
    const CharacterUVRect headTop{ 8, 0, 16, 8 }, headBottom{ 16, 0, 24, 8 },
        headRight{ 0, 8, 8, 16 }, headFront{ 8, 8, 16, 16 },
        headLeft{ 16, 8, 24, 16 }, headBack{ 24, 8, 32, 16 };
    const CharacterUVRect bodyTop{ 20, 16, 28, 20 }, bodyBottom{ 28, 16, 36, 20 },
        bodyRight{ 16, 20, 20, 32 }, bodyFront{ 20, 20, 28, 32 },
        bodyLeft{ 28, 20, 32, 32 }, bodyBack{ 32, 20, 40, 32 };
    const CharacterUVRect rightArmTop{ 44, 16, 48, 20 }, rightArmBottom{ 48, 16, 52, 20 },
        rightArmRight{ 40, 20, 44, 32 }, rightArmFront{ 44, 20, 48, 32 },
        rightArmLeft{ 48, 20, 52, 32 }, rightArmBack{ 52, 20, 56, 32 };
    const CharacterUVRect rightLegTop{ 4, 16, 8, 20 }, rightLegBottom{ 8, 16, 12, 20 },
        rightLegRight{ 0, 20, 4, 32 }, rightLegFront{ 4, 20, 8, 32 },
        rightLegLeft{ 8, 20, 12, 32 }, rightLegBack{ 12, 20, 16, 32 };
    CharacterUVRect leftArmTop = rightArmTop, leftArmBottom = rightArmBottom,
        leftArmRight = rightArmRight, leftArmFront = rightArmFront,
        leftArmLeft = rightArmLeft, leftArmBack = rightArmBack;
    CharacterUVRect leftLegTop = rightLegTop, leftLegBottom = rightLegBottom,
        leftLegRight = rightLegRight, leftLegFront = rightLegFront,
        leftLegLeft = rightLegLeft, leftLegBack = rightLegBack;
    if (skinHeight > 32.5f) {
        // 64x64: dedicated left arm/leg regions.
        leftArmTop = { 36, 48, 40, 52 }; leftArmBottom = { 40, 48, 44, 52 };
        leftArmRight = { 32, 52, 36, 64 }; leftArmFront = { 36, 52, 40, 64 };
        leftArmLeft = { 40, 52, 44, 64 }; leftArmBack = { 44, 52, 48, 64 };
        leftLegTop = { 20, 48, 24, 52 }; leftLegBottom = { 24, 48, 28, 52 };
        leftLegRight = { 16, 52, 20, 64 }; leftLegFront = { 20, 52, 24, 64 };
        leftLegLeft = { 24, 52, 28, 64 }; leftLegBack = { 28, 52, 32, 64 };
    }
    verts.clear();
    indices.clear();
    // Right leg (+X), left leg (-X): 0.25 x 0.75 x 0.25 m.
    append_character_box(verts, indices, 0.0f, 0.0f, -0.125f, 0.25f, 0.75f, 0.125f, skinHeight,
                         rightLegRight, rightLegLeft, rightLegFront, rightLegBack,
                         rightLegTop, rightLegBottom);
    append_character_box(verts, indices, -0.25f, 0.0f, -0.125f, 0.0f, 0.75f, 0.125f, skinHeight,
                         leftLegRight, leftLegLeft, leftLegFront, leftLegBack,
                         leftLegTop, leftLegBottom);
    // Body: 0.5 x 0.75 x 0.25 m.
    append_character_box(verts, indices, -0.25f, 0.75f, -0.125f, 0.25f, 1.5f, 0.125f, skinHeight,
                         bodyRight, bodyLeft, bodyFront, bodyBack, bodyTop, bodyBottom);
    // Right arm (+X), left arm (-X): 0.25 x 0.75 x 0.25 m.
    append_character_box(verts, indices, 0.25f, 0.75f, -0.125f, 0.5f, 1.5f, 0.125f, skinHeight,
                         rightArmRight, rightArmLeft, rightArmFront, rightArmBack,
                         rightArmTop, rightArmBottom);
    append_character_box(verts, indices, -0.5f, 0.75f, -0.125f, -0.25f, 1.5f, 0.125f, skinHeight,
                         leftArmRight, leftArmLeft, leftArmFront, leftArmBack,
                         leftArmTop, leftArmBottom);
    // Head: 0.5 x 0.5 x 0.5 m.
    append_character_box(verts, indices, -0.25f, 1.5f, -0.25f, 0.25f, 2.0f, 0.25f, skinHeight,
                         headRight, headLeft, headFront, headBack, headTop, headBottom);
}

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
    // Per-face atlas UVs: the block texture is a 3-wide [top|side|bottom]
    // atlas. build_cube face order: 0=-Z,1=+Z,2=-X,3=+X (side), 4=-Y
    // (bottom), 5=+Y (top) — remap each face's u into its atlas region.
    if (verts.size() >= 24) {
        for (uint32_t f = 0; f < 6; ++f) {
            float u0 = 1.0f / 3.0f, u1 = 2.0f / 3.0f; // sides
            if (f == 5) { u0 = 0.0f; u1 = 1.0f / 3.0f; }        // +Y top
            else if (f == 4) { u0 = 2.0f / 3.0f; u1 = 1.0f; }   // -Y bottom
            for (int c = 0; c < 4; ++c) {
                EditorVertex& v = verts[f * 4 + static_cast<uint32_t>(c)];
                v.uv.x = u0 + v.uv.x * (u1 - u0);
            }
        }
    }
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
    // #19: a failed upload must NOT leave a "valid" mesh with uninitialized
    // buffers (rendering garbage). Abort and keep the resource invalid.
    if (!safe_map_and_copy(m_device, cube.vb.memory, 0, vbSize, verts.data()) ||
        !safe_map_and_copy(m_device, cube.ib.memory, 0, ibSize, indices.data())) {
        if (cube.vb.buffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, cube.vb.buffer, nullptr);
        if (cube.vb.memory != VK_NULL_HANDLE) vkFreeMemory(m_device, cube.vb.memory, nullptr);
        if (cube.ib.buffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, cube.ib.buffer, nullptr);
        if (cube.ib.memory != VK_NULL_HANDLE) vkFreeMemory(m_device, cube.ib.memory, nullptr);
        return;
    }
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
    float skinLayoutHeight = 64.0f;
    if (const auto meta = m_assetRegistry.find(texId); meta && meta->width > 0 && meta->height > 0) {
        // Minecraft HD skins scale the whole 64x64 layout (128x128, 256x256...)
        // and therefore keep the same normalized UVs. Only legacy skins use a
        // genuinely different 64x32 layout.
        skinLayoutHeight = meta->height * 2u == meta->width ? 32.0f : 64.0f;
    }
    std::vector<EditorVertex> verts;
    std::vector<uint32_t> indices;
    build_character_geometry(skinLayoutHeight, verts, indices);
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
    if (!safe_map_and_copy(m_device, character.vb.memory, 0, vbSize, verts.data()) ||
        !safe_map_and_copy(m_device, character.ib.memory, 0, ibSize, indices.data())) {
        if (character.vb.buffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, character.vb.buffer, nullptr);
        if (character.vb.memory != VK_NULL_HANDLE) vkFreeMemory(m_device, character.vb.memory, nullptr);
        if (character.ib.buffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, character.ib.buffer, nullptr);
        if (character.ib.memory != VK_NULL_HANDLE) vkFreeMemory(m_device, character.ib.memory, nullptr);
        return;
    }
    character.valid = true;
    m_meshResources[texId] = std::move(character);
}

// A Minecraft character/mob skin becomes a humanoid entity in the scene: the
// MeshRenderer references the texture asset directly (the renderer builds the
// humanoid mesh + skin pipeline on demand), so there is no sidecar file and no
// duplicate asset in the browser.
void EditorApplication::spawn_character_entity(const UUID& texId, const glm::vec3& position) {
    if (!m_editorScene) {
        std::cerr << "[Editor] spawn_character_entity: m_editorScene is null" << std::endl;
        return;
    }
    const auto meta = m_assetRegistry.find(texId);
    if (!meta || meta->type != AssetType::Texture) {
        std::cerr << "[Editor] spawn_character_entity: texture not found " << texId.to_string() << std::endl;
        return;
    }
    Entity e = m_editorScene->create_entity(meta->sourcePath.stem().string());
    m_editorScene->transformComponents[e.get_id()].position = position;
    m_editorScene->meshRendererComponents[e.get_id()] =
        MeshRendererComponent{ texId, UUID{ 0, 0 }, true, true };
    m_selectedEntity = e;
    m_editorGui.select_entity(e);
    mark_scene_dirty();
}

// Shared material-graph pipeline that samples one texture (block faces and
// character skins both land here). Cached per texture UUID so two blocks that
// share a texture reuse the same pipeline; rebuilt when the graph hash changes.
EditorApplication::GraphMaterialPipeline* EditorApplication::ensure_texture_pipeline(
    const UUID& texId, std::unordered_map<UUID, GraphMaterialPipeline>& cache, bool withAlpha) {
    if (!texId.is_valid()) return nullptr;
    auto it = cache.find(texId);
    const auto sampledMeta = m_assetRegistry.find(texId);
    const bool isBlockAtlas = sampledMeta && sampledMeta->type == AssetType::Block;
    // REGRESSION GUARD (BUG-EDITOR-ASSET-TEXTURES-003): block assets are
    // dynamically composed [top|side|bottom] atlases and may contain cutout
    // texels.  Never let a caller silently build them as opaque: that makes
    // transparent pixels render black and lets a stale opaque graph cache
    // survive until the next editor restart.  This policy belongs here so a
    // future voxel/entity call site cannot reintroduce the regression.
    const bool requiresAlpha = withAlpha || isBlockAtlas;
    Rendering::MaterialGraph graph;
    const auto texNode = graph.add_texture_sample("Texture");
    if (auto* node = graph.find_node(texNode)) node->value = texId.to_string();
    const auto baseOut = graph.add_output("BaseColor", Rendering::MaterialValueType::Vec3);
    (void)graph.connect(texNode, baseOut, 0);
    if (requiresAlpha) {
        // Alpha cutout: skins/decals sample the texture's alpha into Opacity
        // so fully transparent texels are discarded by the generated shader
        // (no more white/black sides from ignored alpha).
        const auto opacityOut = graph.add_output("Opacity", Rendering::MaterialValueType::Float);
        (void)graph.connect(texNode, opacityOut, 0);
    }
    const uint64_t graphHash = hash_material_graph(graph);
    // Rebuild when the sampled texture's content changed (hot reload) — the
    // graph hash alone cannot see that, so pipelines kept stale GPU copies.
    uint64_t contentHash = 0;
    if (sampledMeta) contentHash = sampledMeta->contentHash;
    if (it == cache.end() || !it->second.valid || it->second.graphHash != graphHash ||
        it->second.textureContentHash != contentHash) {
        if (it != cache.end()) destroy_graph_pipeline(it->second);
        GraphMaterialPipeline built;
        built.graphHash = graphHash;
        if (!build_graph_pipeline(graph, built)) {
            std::cerr << "[Editor] Texture pipeline: " << built.lastError << std::endl;
        }
        built.textureContentHash = contentHash;
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
    mark_scene_dirty();
}void EditorApplication::draw_mesh_resource(VkCommandBuffer cmd, const glm::mat4& mvp, const glm::vec4& color,
                                           const EditorMeshResource& resource,
                                           const glm::mat4& model) {
    if (!resource.valid || resource.vb.buffer == VK_NULL_HANDLE) return;
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &resource.vb.buffer, &offset);
    if (resource.ib.buffer != VK_NULL_HANDLE) {
        vkCmdBindIndexBuffer(cmd, resource.ib.buffer, 0, VK_INDEX_TYPE_UINT32);
    }
    push_constants(cmd, m_scenePipelineLayout, mvp, color, model);
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
    // Content-hash invalidation: a reimported texture (hot reload) must not
    // keep showing its stale flat thumbnail.
    const auto hashIt = m_assetThumbnailHashes.find(asset.id);
    if (hashIt != m_assetThumbnailHashes.end() && hashIt->second != asset.contentHash) {
        const auto thumbIt = m_assetThumbnails.find(asset.id);
        if (thumbIt != m_assetThumbnails.end()) {
            if (thumbIt->second.imguiId != VK_NULL_HANDLE)
                ImGui_ImplVulkan_RemoveTexture(thumbIt->second.imguiId);
            if (thumbIt->second.view != VK_NULL_HANDLE)
                vkDestroyImageView(m_device, thumbIt->second.view, nullptr);
            if (thumbIt->second.image != VK_NULL_HANDLE)
                vkDestroyImage(m_device, thumbIt->second.image, nullptr);
            if (thumbIt->second.memory != VK_NULL_HANDLE)
                vkFreeMemory(m_device, thumbIt->second.memory, nullptr);
            m_assetThumbnails.erase(thumbIt);
        }
        m_assetThumbnailFailed.erase(asset.id);
        m_assetThumbnailHashes.erase(hashIt);
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
// screenful lazily and the rest stays virtualized until scrolled into
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
        // Remember the content this preview was made from, so a reimport
        // invalidates it (success and failure alike — a fixed file retries).
        if (const auto meta = m_assetRegistry.find(ready.assetId))
            m_assetThumbnailHashes[ready.assetId] = meta->contentHash;
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
        if (meta && !meta->sourcePath.empty()) {
            if (m_audioDecodeThread.joinable()) m_audioDecodeThread.join();
            // Decode the SOURCE file (wav/ogg/flac/mp3), not the cooked
            // .vcaudio: the cooked file has a custom "VCAUDIO" header that
            // miniaudio does not understand, so previewing used to fail for
            // every audio asset. miniaudio sniffs the container, so files
            // whose extension does not match their content still work.
            m_audioDecodeThread = std::thread([this, id, path = meta->sourcePath]() {
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
    m_assetThumbnailHashes.clear();
    for (auto& [id, desc] : m_asset3dThumbnails) {
        (void)id;
        if (desc != VK_NULL_HANDLE) ImGui_ImplVulkan_RemoveTexture(desc);
    }
    m_asset3dThumbnails.clear();
    for (auto& [id, thumb] : m_asset3dThumbnailImages) {
        (void)id;
        if (thumb.view != VK_NULL_HANDLE) vkDestroyImageView(m_device, thumb.view, nullptr);
        if (thumb.image != VK_NULL_HANDLE) vkDestroyImage(m_device, thumb.image, nullptr);
        if (thumb.memory != VK_NULL_HANDLE) vkFreeMemory(m_device, thumb.memory, nullptr);
    }
    m_asset3dThumbnailImages.clear();
    m_asset3dThumbnailHashes.clear();
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
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
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
    VkCommandBuffer cmd = begin_single_time_commands();
    transition_image_layout(cmd, m_thumbImage, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    end_single_time_commands(cmd);
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
    // Per-face atlas UVs: remap the shared thumbnail cube into the 3-wide
    // [top|side|bottom] atlas regions. Face groups: 0-3=+Z, 4-7=-Z, 8-11=+X,
    // 12-15=-X (sides), 16-19=+Y (top), 20-23=-Y (bottom).
    BlockVert cube[24];
    std::memcpy(cube, verts, sizeof(cube));
    for (int i = 0; i < 24; ++i) {
        float u0 = 1.0f / 3.0f, u1 = 2.0f / 3.0f; // sides
        if (i >= 16 && i < 20) { u0 = 0.0f; u1 = 1.0f / 3.0f; }     // +Y top
        else if (i >= 20) { u0 = 2.0f / 3.0f; u1 = 1.0f; }          // -Y bottom
        cube[i].uv.x = u0 + cube[i].uv.x * (u1 - u0);
    }
    const VkDeviceSize vbSize = sizeof(BlockVert) * 24;
    const VkDeviceSize ibSize = sizeof(uint32_t) * 36;
    create_buffer(vbSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  m_blockCubeVB.buffer, m_blockCubeVB.memory);
    create_buffer(ibSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  m_blockCubeIB.buffer, m_blockCubeIB.memory);
    if (!safe_map_and_copy(m_device, m_blockCubeVB.memory, 0, vbSize, cube) ||
        !safe_map_and_copy(m_device, m_blockCubeIB.memory, 0, ibSize, indices)) {
        std::cerr << "[Editor] block cube upload failed" << std::endl;
    }
    m_blockCubeIndexCount = 36;

    // Pixel-art sampler: Minecraft blocks/skins are nearest-filtered, no
    // mipmap bleeding. (PBR/HD textures keep the trilinear sampler elsewhere.)
    VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f; // the atlas is a single mip
    vkCreateSampler(m_device, &samplerInfo, nullptr, &m_blockSampler);

    // Same pixel-art filtering for the block atlases sampled by the
    // material-graph pipelines (voxel volumes + scene block cubes): NEAREST,
    // no mipmap bleeding — the crisp Minecraft look instead of LINEAR blur.
    VkSamplerCreateInfo blockDrawInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    blockDrawInfo.magFilter = VK_FILTER_NEAREST;
    blockDrawInfo.minFilter = VK_FILTER_NEAREST;
    blockDrawInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    blockDrawInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    blockDrawInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    blockDrawInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    blockDrawInfo.minLod = 0.0f;
    blockDrawInfo.maxLod = 0.0f; // the block atlas is a single mip
    vkCreateSampler(m_device, &blockDrawInfo, nullptr, &m_blockDrawSampler);

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo descLayoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    descLayoutInfo.bindingCount = 1;
    descLayoutInfo.pBindings = &binding;
    vkCreateDescriptorSetLayout(m_device, &descLayoutInfo, nullptr, &m_blockDescSetLayout);

    // Block descriptors come from their OWN pool, not the ImGui pool:
    // ImGui resets its descriptor pools every frame, which invalidated these
    // sets right after allocation — draws with the reset sets corrupted the
    // whole frame (viewport went blank; occasionally the device faulted).
    // A dedicated pool keeps the sets alive for the block pipeline's lifetime.
    VkDescriptorPoolSize blockPoolSize{};
    blockPoolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    blockPoolSize.descriptorCount = 2048;
    VkDescriptorPoolCreateInfo blockPoolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    blockPoolInfo.maxSets = 1024;
    blockPoolInfo.poolSizeCount = 1;
    blockPoolInfo.pPoolSizes = &blockPoolSize;
    vkCreateDescriptorPool(m_device, &blockPoolInfo, nullptr, &m_blockDescPool);

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
    m_blockTextureHashes.clear();
    if (m_blockPipeline != VK_NULL_HANDLE) { vkDestroyPipeline(m_device, m_blockPipeline, nullptr); m_blockPipeline = VK_NULL_HANDLE; }
    if (m_blockPipelineLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(m_device, m_blockPipelineLayout, nullptr); m_blockPipelineLayout = VK_NULL_HANDLE; }
    if (m_blockDescSetLayout != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(m_device, m_blockDescSetLayout, nullptr); m_blockDescSetLayout = VK_NULL_HANDLE; }
    if (m_blockDescPool != VK_NULL_HANDLE) { vkDestroyDescriptorPool(m_device, m_blockDescPool, nullptr); m_blockDescPool = VK_NULL_HANDLE; }
    if (m_blockSampler != VK_NULL_HANDLE) { vkDestroySampler(m_device, m_blockSampler, nullptr); m_blockSampler = VK_NULL_HANDLE; }
    if (m_blockDrawSampler != VK_NULL_HANDLE) { vkDestroySampler(m_device, m_blockDrawSampler, nullptr); m_blockDrawSampler = VK_NULL_HANDLE; }
    // Per-face atlas textures (same lifecycle as the block cube).
    for (auto& [uuid, gt] : m_blockAtlasTextures) {
        (void)uuid;
        destroy_graph_texture(gt);
    }
    m_blockAtlasTextures.clear();
    m_blockAtlasHashes.clear();
    if (m_blockVertShader != VK_NULL_HANDLE) { vkDestroyShaderModule(m_device, m_blockVertShader, nullptr); m_blockVertShader = VK_NULL_HANDLE; }
    if (m_blockFragShader != VK_NULL_HANDLE) { vkDestroyShaderModule(m_device, m_blockFragShader, nullptr); m_blockFragShader = VK_NULL_HANDLE; }
    if (m_blockCubeVB.buffer != VK_NULL_HANDLE) { vkDestroyBuffer(m_device, m_blockCubeVB.buffer, nullptr); vkFreeMemory(m_device, m_blockCubeVB.memory, nullptr); m_blockCubeVB = GPUBuffer{}; }
    if (m_blockCubeIB.buffer != VK_NULL_HANDLE) { vkDestroyBuffer(m_device, m_blockCubeIB.buffer, nullptr); vkFreeMemory(m_device, m_blockCubeIB.memory, nullptr); m_blockCubeIB = GPUBuffer{}; }
}

// Lazy descriptor set for a block texture (my layout, allocated from the ImGui
// descriptor pool which carries COMBINED_IMAGE_SAMPLER).
VkDescriptorSet EditorApplication::get_block_descriptor(const UUID& textureAsset) {
    // Block assets resolve to the per-face atlas, which is OWNED by
    // m_blockAtlasTextures (not m_blockTextures) — avoid double-owning/destroying.
    const bool isAtlas = [&] {
        const auto meta = m_assetRegistry.find(textureAsset);
        return meta && meta->type == AssetType::Block;
    }();
    // Content-hash invalidation: a reimported texture (hot reload) must not
    // keep its stale GPU copy (thumbnails and scene block faces share it).
    uint64_t contentHash = 0;
    if (const auto meta = m_assetRegistry.find(textureAsset)) contentHash = meta->contentHash;
    const auto hashIt = m_blockTextureHashes.find(textureAsset);
    if (hashIt != m_blockTextureHashes.end() && hashIt->second != contentHash) {
        const auto descIt = m_blockDescriptors.find(textureAsset);
        if (descIt != m_blockDescriptors.end()) {
            if (descIt->second != VK_NULL_HANDLE && m_blockDescPool != VK_NULL_HANDLE)
                vkFreeDescriptorSets(m_device, m_blockDescPool, 1, &descIt->second);
            m_blockDescriptors.erase(descIt);
        }
        if (!isAtlas) {
            const auto texIt = m_blockTextures.find(textureAsset);
            if (texIt != m_blockTextures.end()) { destroy_graph_texture(texIt->second); m_blockTextures.erase(texIt); }
        }
        m_blockTextureHashes.erase(hashIt);
    }
    const auto cached = m_blockDescriptors.find(textureAsset);
    if (cached != m_blockDescriptors.end()) return cached->second;
    if (m_blockDescSetLayout == VK_NULL_HANDLE || m_blockSampler == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    GraphTexture gt;
    std::string error;
    if (!load_viewport_texture(textureAsset, gt, error, 192)) return VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = m_blockDescPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &m_blockDescSetLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(m_device, &ai, &set) != VK_SUCCESS) {
        if (!isAtlas) destroy_graph_texture(gt); // atlas is owned by its cache
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
    if (!isAtlas) m_blockTextures[textureAsset] = gt; // atlas kept alive by its cache
    m_blockDescriptors[textureAsset] = set;
    m_blockTextureHashes[textureAsset] = contentHash;
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
        // Content-hash invalidation: a reimported asset (hot reload) must not
        // keep showing its stale 3D thumbnail.
        const auto hashIt = m_asset3dThumbnailHashes.find(id);
        if (hashIt != m_asset3dThumbnailHashes.end() && hashIt->second != meta->contentHash) {
            destroy_3d_thumbnail(id);
            m_asset3dThumbnailHashes.erase(hashIt);
        }
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
            // The cube samples a 3-wide [top|side|bottom] atlas. Feeding the
            // original single PNG made each face sample only one third of it.
            const UUID blockId = create_block_asset(*meta);
            const VkDescriptorSet desc = blockId.is_valid()
                                       ? get_block_descriptor(blockId) : VK_NULL_HANDLE;
            if (desc == VK_NULL_HANDLE) { m_assetThumbnailFailed.insert(id); continue; }
            render_block_thumbnail(id, desc);
        } else if (meta->type == AssetType::Texture && is_character_texture(*meta)) {
            // The PNG is the character: its card shows the humanoid mesh with
            // the skin applied (same pipeline the viewport uses), not the flat
            // skin atlas.
            const EditorMeshResource* mesh = get_mesh_resource(id);
            if (!mesh || !mesh->valid) { m_assetThumbnailFailed.insert(id); continue; }
            render_character_thumbnail(id, *mesh);
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
    snapshot_rendered_thumbnail(cmd, assetId);
    end_single_time_commands(cmd);
    if (const auto meta = m_assetRegistry.find(assetId))
        m_asset3dThumbnailHashes[assetId] = meta->contentHash;
}

// Same, but a textured unit cube: the Minecraft-style block assembled from its
// PNG face texture (block pipeline).
void EditorApplication::render_block_thumbnail(const UUID& assetId, VkDescriptorSet textureDesc) {
    if (m_blockPipeline == VK_NULL_HANDLE) return;
    // Unit block centered at the origin. The old character camera looked one
    // metre above it from 4.5 m away, making every block a tiny cyan speck.
    const glm::vec3 blockCenter(0.0f);
    const glm::mat4 proj = glm::perspective(glm::radians(45.0f), 1.0f, 0.01f, 50.0f);
    const glm::mat4 view = glm::lookAt(glm::vec3(1.45f, 1.15f, 1.65f),
                                      blockCenter, glm::vec3(0, 1, 0));
    const glm::mat4 mvp = proj * view;

    VkCommandBuffer cmd = begin_single_time_commands();
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
    snapshot_rendered_thumbnail(cmd, assetId);
    end_single_time_commands(cmd);
    if (const auto meta = m_assetRegistry.find(assetId))
        m_asset3dThumbnailHashes[assetId] = meta->contentHash;
}

// A Minecraft character/mob skin rendered as the humanoid mesh with the skin
// applied (material-graph texture pipeline, the exact path the viewport uses
// for character entities) — the card shows the 3D character instead of the
// flat skin atlas PNG.
void EditorApplication::render_character_thumbnail(const UUID& assetId, const EditorMeshResource& mesh) {
    GraphMaterialPipeline* gmp = ensure_texture_pipeline(assetId, m_skinGraphPipelines, true);
    if (!gmp) { m_assetThumbnailFailed.insert(assetId); return; }
    write_material_ubo(*gmp, nullptr, nullptr);
    write_light_ubo(*gmp, m_editorScene.get(), m_editorCamera.position);
    // Frame the humanoid: the model spans y 0..2 with the feet at the origin.
    const glm::vec3 center(0.0f, 1.0f, 0.0f);
    const glm::mat4 proj = glm::perspective(glm::radians(45.0f), 1.0f, 0.01f, 100.0f);
    const glm::mat4 view = glm::lookAt(center + glm::vec3(0.85f, 0.55f, 1.10f) * 3.0f,
                                       center, glm::vec3(0, 1, 0));
    const Rendering::MaterialPushConstants pc{ proj * view, glm::mat4(1.0f) };

    VkCommandBuffer cmd = begin_single_time_commands();
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
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gmp->pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gmp->layout,
                            0, 1, &gmp->descriptorSet, 0, nullptr);
    vkCmdPushConstants(cmd, gmp->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
    const VkDeviceSize vertexOffset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.vb.buffer, &vertexOffset);
    if (mesh.ib.buffer != VK_NULL_HANDLE)
        vkCmdBindIndexBuffer(cmd, mesh.ib.buffer, 0, VK_INDEX_TYPE_UINT32);
    for (const EditorMeshResource::DrawRange& range : mesh.ranges) {
        if (range.indexed)
            vkCmdDrawIndexed(cmd, range.indexCount, 1, range.firstIndex, 0, 0);
        else
            vkCmdDraw(cmd, range.indexCount, 1, range.vertexOffset, 0);
    }
    vkCmdEndRenderPass(cmd);
    snapshot_rendered_thumbnail(cmd, assetId);
    end_single_time_commands(cmd);
    if (const auto meta = m_assetRegistry.find(assetId))
        m_asset3dThumbnailHashes[assetId] = meta->contentHash;
}

void EditorApplication::destroy_3d_thumbnail(const UUID& assetId) {
    const auto descriptor = m_asset3dThumbnails.find(assetId);
    if (descriptor != m_asset3dThumbnails.end()) {
        if (descriptor->second != VK_NULL_HANDLE) ImGui_ImplVulkan_RemoveTexture(descriptor->second);
        m_asset3dThumbnails.erase(descriptor);
    }
    const auto image = m_asset3dThumbnailImages.find(assetId);
    if (image != m_asset3dThumbnailImages.end()) {
        if (image->second.view != VK_NULL_HANDLE) vkDestroyImageView(m_device, image->second.view, nullptr);
        if (image->second.image != VK_NULL_HANDLE) vkDestroyImage(m_device, image->second.image, nullptr);
        if (image->second.memory != VK_NULL_HANDLE) vkFreeMemory(m_device, image->second.memory, nullptr);
        m_asset3dThumbnailImages.erase(image);
    }
}

void EditorApplication::snapshot_rendered_thumbnail(VkCommandBuffer cmd, const UUID& assetId) {
    destroy_3d_thumbnail(assetId);
    AssetThumbnail snapshot;
    create_image(m_thumbSize, m_thumbSize, VK_FORMAT_R8G8B8A8_UNORM,
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, snapshot.image, snapshot.memory);
    snapshot.view = create_image_view(snapshot.image, VK_FORMAT_R8G8B8A8_UNORM,
                                      VK_IMAGE_ASPECT_COLOR_BIT);
    transition_image_layout(cmd, m_thumbImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    transition_image_layout(cmd, snapshot.image, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageCopy copy{};
    copy.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.extent = { m_thumbSize, m_thumbSize, 1 };
    vkCmdCopyImage(cmd, m_thumbImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   snapshot.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    transition_image_layout(cmd, snapshot.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    transition_image_layout(cmd, m_thumbImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    snapshot.imguiId = ImGui_ImplVulkan_AddTexture(snapshot.view,
                                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    m_asset3dThumbnails[assetId] = snapshot.imguiId;
    m_asset3dThumbnailImages[assetId] = snapshot;
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
        "/skin/", "/skins/",
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
// Auxiliary material maps follow the classic <base>_<suffix> naming
// (andesite_n.png = normal, _s = specular, _h = height, _e = emissive, …).
// They are material inputs for a block, never a block face themselves — the
// heuristic used to classify every square POT texture as a Block, flooding the
// browser with fake "blocks" that were really normal/specular maps.
bool EditorApplication::is_aux_map_texture(const AssetMetadata& meta) const {
    if (meta.type != AssetType::Texture || meta.sourcePath.empty()) return false;
    std::string stem = meta.sourcePath.stem().string();
    std::transform(stem.begin(), stem.end(), stem.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (stem.size() < 3) return false;
    static const char* kAuxSuffixes[] = {
        "_n", "_s", "_h", "_e", "_bump", "_bumpmap", "_normal", "_normalmap",
        "_spec", "_specular", "_specmap", "_height", "_heightmap", "_emissive",
        "_emission", "_glow", "_ao", "_ambientocclusion", "_rough", "_roughness",
        "_metal", "_metallic", "_metalness", "_disp", "_displacement", "_mask",
        "_detail", "_overlay", "_gloss", "_glossmap",
    };
    for (const char* suffix : kAuxSuffixes) {
        const size_t n = std::strlen(suffix);
        if (stem.size() > n && stem.compare(stem.size() - n, n, suffix) == 0) return true;
    }
    return false;
}

bool EditorApplication::looks_like_block_texture(const AssetMetadata& meta) const {
    if (meta.type != AssetType::Texture || meta.width == 0 || meta.height == 0) return false;
    if (meta.width != meta.height) return false;
    const uint32_t s = meta.width;
    if (s < 8 || s > 256) return false;
    if ((s & (s - 1)) != 0) return false;
    if (is_aux_map_texture(meta)) return false;
    std::string p = meta.sourcePath.generic_string();
    std::transform(p.begin(), p.end(), p.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (p.find("/block/") != std::string::npos || p.find("/blocks/") != std::string::npos ||
        p.find("/tile/") != std::string::npos) {
        return true;
    }
    if (is_character_texture(meta)) return false;
    // Non-block decorative/UI textures: even when square POT, particle
    // atlases, icons, GUI sprites, noise maps and similar are not block
    // faces. Only the fallback path (no /block/ folder) is filtered — a
    // texture explicitly inside a block folder always wins above.
    std::string stem = meta.sourcePath.stem().string();
    std::transform(stem.begin(), stem.end(), stem.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const char* kNonBlockStems[] = {
        "particle", "particles", "icon", "icons", "noise", "noises",
        "gui", "font", "fontsheet", "cursor", "logo", "splash",
        "toolbar", "badge", "badges", "emoji", "emojis", "widget",
        "widgets", "button", "buttons", "panel", "panels", "frame",
        "frames", "loading", "menu", "menus", "title", "options",
        "settings", "achievement", "achievements", "recipe", "recipes",
        "inventory", "hotbar", "crosshair", "hud", "map", "maps",
        "book", "books", "painting", "paintings", "slot", "slots",
        "container", "containers", "banner", "banners", "arrow",
        "arrows", "experience", "xp", "effect", "effects",
    };
    for (const char* marker : kNonBlockStems) {
        if (stem == marker) return false;
    }
    static const char* kNonBlockSuffixes[] = {
        "_icon", "_icons", "_particle", "_particles", "_noise", "_sprite",
        "_sprites", "_gui", "_ui", "_hud",
    };
    for (const char* suffix : kNonBlockSuffixes) {
        const size_t n = std::strlen(suffix);
        if (stem.size() > n && stem.compare(stem.size() - n, n, suffix) == 0) return false;
    }
    static const char* kNonBlockPrefixes[] = {
        "gui_", "icon_", "particle_", "noise_", "ui_", "hud_", "menu_", "title_",
    };
    for (const char* prefix : kNonBlockPrefixes) {
        const size_t n = std::strlen(prefix);
        if (stem.size() > n && stem.compare(0, n, prefix) == 0) return false;
    }
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
    if (is_aux_map_texture(meta)) {
        // Aux maps are never blocks. Heal registries created before this rule
        // existed (their .vblock sidecar made andesite_n.png a "Block").
        if (!m_auxBlockHealed.contains(meta.id)) {
            m_auxBlockHealed.insert(meta.id);
            heal_aux_block_sidecars(meta);
        }
        return false;
    }
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

// Removes .vblock sidecars that were auto-created for an auxiliary map (a
// sidecar whose main face IS the aux texture, e.g. andesite_n.vblock). Blocks
// that merely reference the aux texture as their normal/specular are kept.
// Runs once per texture UUID (see is_block_texture).
void EditorApplication::heal_aux_block_sidecars(const AssetMetadata& textureMeta) {
    if (!textureMeta.id.is_valid()) return;
    AssetBrowserModel browser(m_assetRegistry);
    bool changed = false;
    for (const AssetMetadata& candidate : m_assetRegistry.snapshot()) {
        if (candidate.type != AssetType::Block) continue;
        BlockAssetData data;
        if (!load_block_asset(candidate.id, data)) continue;
        const bool isMainFace = data.texture == textureMeta.id || data.top == textureMeta.id ||
                                data.side == textureMeta.id || data.bottom == textureMeta.id;
        if (!isMainFace) continue;
        const AssetFileOperationResult removed = browser.delete_asset(candidate.id);
        if (!removed) {
            std::cerr << "[ContentBrowser] Could not remove aux-map block: " << removed.error << std::endl;
        } else {
            m_blockAssetCache.erase(candidate.id);
            m_blockAssetFailed.erase(candidate.id);
            changed = true;
        }
    }
    if (changed) {
        const auto registryPath =
            std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "AssetRegistry.db";
        if (!m_assetRegistry.save(registryPath))
            std::cerr << "[AssetRegistry] Could not persist aux-map block cleanup" << std::endl;
        std::cout << "[ContentBrowser] Removed block sidecar(s) for aux map '"
                  << textureMeta.sourcePath.filename().string() << "'" << std::endl;
    }
}

// One-time pass (after the Content Browser indexes): base blocks whose sidecar
// predates material-map grouping get their sibling _n/_s textures recorded as
// normal/specular, so andesite.vblock owns the whole material set even when it
// was created in an older session.
void EditorApplication::enrich_block_material_maps() {
    bool changed = false;
    for (const AssetMetadata& candidate : m_assetRegistry.snapshot()) {
        if (candidate.type != AssetType::Block || candidate.sourcePath.empty()) continue;
        BlockAssetData data;
        if (!load_block_asset(candidate.id, data)) continue;
        if (data.normal.is_valid() && data.specular.is_valid()) continue;
        if (!data.texture.is_valid()) continue;
        const auto texMeta = m_assetRegistry.find(data.texture);
        if (!texMeta || texMeta->sourcePath.empty()) continue;
        UUID normalId = data.normal, specularId = data.specular;
        std::string baseLower = texMeta->sourcePath.stem().string();
        std::transform(baseLower.begin(), baseLower.end(), baseLower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (const AssetMetadata& cand : m_assetRegistry.snapshot()) {
            if (cand.type != AssetType::Texture || cand.sourcePath.empty()) continue;
            std::string cStem = cand.sourcePath.stem().string();
            std::transform(cStem.begin(), cStem.end(), cStem.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (cStem == baseLower + "_n" || cStem == baseLower + "_normal" ||
                cStem == baseLower + "_normalmap") {
                normalId = cand.id;
            } else if (cStem == baseLower + "_s" || cStem == baseLower + "_spec" ||
                       cStem == baseLower + "_specular") {
                specularId = cand.id;
            }
        }
        if (normalId == data.normal && specularId == data.specular) continue;
        data.normal = normalId;
        data.specular = specularId;
        m_blockAssetCache[candidate.id] = data;
        std::ofstream out(candidate.sourcePath);
        out << "{\"texture\":\"" << data.texture.to_string() << "\"";
        if (data.top.is_valid()) out << ",\"top\":\"" << data.top.to_string() << "\"";
        if (data.bottom.is_valid()) out << ",\"bottom\":\"" << data.bottom.to_string() << "\"";
        if (data.side.is_valid()) out << ",\"side\":\"" << data.side.to_string() << "\"";
        if (normalId.is_valid()) out << ",\"normal\":\"" << normalId.to_string() << "\"";
        if (specularId.is_valid()) out << ",\"specular\":\"" << specularId.to_string() << "\"";
        out << "}";
        changed = true;
    }
    if (changed) {
        std::cout << "[ContentBrowser] Enriched block sidecars with material maps" << std::endl;
    }
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
    const auto lowerStem = [](const std::filesystem::path& p) {
        std::string s = p.stem().string();
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    const std::string baseLower = lowerStem(textureMeta.sourcePath);
    // Minecraft-style face textures (<base>_top/_side/_bottom) have no
    // <base>.png of their own (grass_block = grass_block_top + grass_block_side
    // + dirt). Every face import (re)assembles the parent <base>.vblock so the
    // block renders with the correct per-face atlas — converges as the pack
    // import visits the sibling faces in any order.
    static const char* kFaceSuffixes[] = { "_top", "_up", "_side", "_bottom", "_down" };
    std::string parentLower = baseLower;
    for (const char* suffix : kFaceSuffixes) {
        const size_t n = std::strlen(suffix);
        if (parentLower.size() > n && parentLower.compare(parentLower.size() - n, n, suffix) == 0) {
            parentLower.resize(parentLower.size() - n);
            break;
        }
    }
    if (!parentLower.empty() && parentLower != baseLower) {
        UUID pBase, pTop, pSide, pBottom, dirtTex;
        for (const AssetMetadata& candidate : m_assetRegistry.snapshot()) {
            if (candidate.type != AssetType::Texture || candidate.sourcePath.empty()) continue;
            const std::string cStem = lowerStem(candidate.sourcePath);
            if (cStem == parentLower) pBase = candidate.id;
            else if (cStem == parentLower + "_top" || cStem == parentLower + "_up") pTop = candidate.id;
            else if (cStem == parentLower + "_side") pSide = candidate.id;
            else if (cStem == parentLower + "_bottom" || cStem == parentLower + "_down") pBottom = candidate.id;
            else if (cStem == "dirt") dirtTex = candidate.id;
        }
        const UUID mainTex = pBase.is_valid() ? pBase
                           : (pTop.is_valid() ? pTop : (pSide.is_valid() ? pSide : pBottom));
        if (mainTex.is_valid() && (pTop.is_valid() || pSide.is_valid() || pBottom.is_valid())) {
            const UUID faceTop = pTop.is_valid() ? pTop : mainTex;
            const UUID faceSide = pSide.is_valid() ? pSide : mainTex;
            // Minecraft convention: no dedicated bottom → dirt (grass_block);
            // otherwise the block's own texture (stone) or the top (logs).
            const UUID faceBottom = pBottom.is_valid() ? pBottom
                                  : (pBase.is_valid() ? pBase
                                  : (dirtTex.is_valid() ? dirtTex : faceTop));
            const std::filesystem::path parentPath = textureMeta.sourcePath.parent_path() /
                (parentLower + ".vblock");
            UUID parentId{ 0, 0 };
            if (const auto existing = m_assetRegistry.find_id(parentPath)) parentId = *existing;
            else parentId = UUID();
            {
                std::ofstream out(parentPath);
                out << "{\"texture\":\"" << mainTex.to_string() << "\"";
                out << ",\"top\":\"" << faceTop.to_string() << "\"";
                out << ",\"side\":\"" << faceSide.to_string() << "\"";
                if (faceBottom.is_valid()) out << ",\"bottom\":\"" << faceBottom.to_string() << "\"";
                out << "}";
            }
            AssetMetadata pmeta;
            pmeta.id = parentId;
            pmeta.type = AssetType::Block;
            pmeta.sourcePath = parentPath;
            pmeta.cookedPath = parentPath;
            pmeta.isCooked = true;
            pmeta.contentHash = textureMeta.contentHash;
            if (m_assetRegistry.register_asset(pmeta)) {
                m_blockAssetCache[parentId] = BlockAssetData{ mainTex, faceTop, faceBottom, faceSide,
                                                              UUID{ 0, 0 }, UUID{ 0, 0 } };
                m_blockAssetFailed.erase(parentId);
                const auto stale = m_blockAtlasTextures.find(parentId);
                if (stale != m_blockAtlasTextures.end()) {
                    destroy_graph_texture(stale->second);
                    m_blockAtlasTextures.erase(stale);
                }
                m_blockAtlasHashes.erase(parentId);
            }
        }
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
    // Group the block's material set: sibling <base>_n / <base>_s textures
    // (normal/specular maps, already registered as plain textures) are recorded
    // in the sidecar so the block asset owns its maps, not just the albedo
    // face — andesite_n.png is no longer a separate "Block". Per-face sibling
    // textures (<base>_top/_side/_bottom) are recorded too, so the renderer's
    // per-face atlas shows grass_block_top on +Y, grass_block_side on the
    // sides and dirt on -Y instead of one texture everywhere.
    UUID normalId, specularId, topId, sideId, bottomId;
    for (const AssetMetadata& candidate : m_assetRegistry.snapshot()) {
        if (candidate.type != AssetType::Texture || candidate.sourcePath.empty()) continue;
        const std::string cStem = lowerStem(candidate.sourcePath);
        if (cStem == baseLower + "_n" || cStem == baseLower + "_normal" ||
            cStem == baseLower + "_normalmap") {
            normalId = candidate.id;
        } else if (cStem == baseLower + "_s" || cStem == baseLower + "_spec" ||
                   cStem == baseLower + "_specular") {
            specularId = candidate.id;
        } else if (cStem == baseLower + "_top" || cStem == baseLower + "_up") {
            topId = candidate.id;
        } else if (cStem == baseLower + "_side") {
            sideId = candidate.id;
        } else if (cStem == baseLower + "_bottom" || cStem == baseLower + "_down") {
            bottomId = candidate.id;
        }
    }
    {
        std::ofstream out(blockPath);
        out << "{\"texture\":\"" << textureMeta.id.to_string() << "\"";
        if (topId.is_valid()) out << ",\"top\":\"" << topId.to_string() << "\"";
        if (sideId.is_valid()) out << ",\"side\":\"" << sideId.to_string() << "\"";
        if (bottomId.is_valid()) out << ",\"bottom\":\"" << bottomId.to_string() << "\"";
        if (normalId.is_valid()) out << ",\"normal\":\"" << normalId.to_string() << "\"";
        if (specularId.is_valid()) out << ",\"specular\":\"" << specularId.to_string() << "\"";
        out << "}";
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
    m_blockAssetCache[meta.id] =
        BlockAssetData{ textureMeta.id,
                        topId.is_valid() ? topId : textureMeta.id,
                        bottomId.is_valid() ? bottomId : textureMeta.id,
                        sideId.is_valid() ? sideId : textureMeta.id,
                        normalId, specularId };
    m_blockAssetFailed.erase(meta.id);
    const auto registryPath = std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "AssetRegistry.db";
    if (!m_assetRegistry.save(registryPath)) {
        std::cerr << "[AssetRegistry] Could not persist block asset" << std::endl;
    }
    return meta.id;
}

bool EditorApplication::set_block_faces(const UUID& blockId, const UUID& top,
                                        const UUID& side, const UUID& bottom) {
    const auto meta = m_assetRegistry.find(blockId);
    if (!meta || meta->type != AssetType::Block || meta->sourcePath.empty()) {
        std::cerr << "[BlockFaces] block asset not found" << std::endl;
        return false;
    }
    BlockAssetData data;
    if (!load_block_asset(blockId, data)) return false;
    const auto isTex = [&](const UUID& id) {
        if (!id.is_valid()) return true;
        const auto tm = m_assetRegistry.find(id);
        return tm && tm->type == AssetType::Texture;
    };
    if (!isTex(top) || !isTex(side) || !isTex(bottom)) {
        std::cerr << "[BlockFaces] a face UUID is not a registered texture" << std::endl;
        return false;
    }
    const UUID newTop = top.is_valid() ? top : (data.top.is_valid() ? data.top : data.texture);
    const UUID newSide = side.is_valid() ? side : (data.side.is_valid() ? data.side : data.texture);
    const UUID newBottom = bottom.is_valid() ? bottom : (data.bottom.is_valid() ? data.bottom : data.texture);
    data.top = newTop;
    data.side = newSide;
    data.bottom = newBottom;
    {
        std::ofstream out(meta->sourcePath);
        out << "{\"texture\":\"" << data.texture.to_string() << "\"";
        if (newTop.is_valid()) out << ",\"top\":\"" << newTop.to_string() << "\"";
        if (newSide.is_valid()) out << ",\"side\":\"" << newSide.to_string() << "\"";
        if (newBottom.is_valid()) out << ",\"bottom\":\"" << newBottom.to_string() << "\"";
        if (data.normal.is_valid()) out << ",\"normal\":\"" << data.normal.to_string() << "\"";
        if (data.specular.is_valid()) out << ",\"specular\":\"" << data.specular.to_string() << "\"";
        out << "}";
    }
    m_blockAssetCache[blockId] = data;
    m_blockAssetFailed.erase(blockId);
    // Invalidate the per-face atlas so the next render composites the new faces.
    const auto staleIt = m_blockAtlasTextures.find(blockId);
    if (staleIt != m_blockAtlasTextures.end()) {
        destroy_graph_texture(staleIt->second);
        m_blockAtlasTextures.erase(staleIt);
    }
    m_blockAtlasHashes.erase(blockId);
    std::cout << "[BlockFaces] updated " << blockId.to_string() << std::endl;
    return true;
}

UUID EditorApplication::create_block_from_faces(const UUID& base, const UUID& top,
                                                const UUID& side, const UUID& bottom,
                                                const std::string& name) {
    const auto isTex = [&](const UUID& id) {
        if (!id.is_valid()) return false;
        const auto tm = m_assetRegistry.find(id);
        return tm && tm->type == AssetType::Texture;
    };
    if (!isTex(base) && !isTex(top) && !isTex(side) && !isTex(bottom)) {
        std::cerr << "[BlockModelFaces] at least one face must be a registered texture" << std::endl;
        return UUID{ 0, 0 };
    }
    const UUID fallback = base.is_valid() ? base : (side.is_valid() ? side : top);
    std::filesystem::path blockPath;
    if (const auto bm = m_assetRegistry.find(fallback); bm && !bm->sourcePath.empty()) {
        blockPath = bm->sourcePath.parent_path();
    } else {
        blockPath = std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "assets" / "textures";
    }
    std::string stem = name;
    if (stem.empty()) stem = "block";
    std::string sanitized;
    for (char c : stem) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') sanitized += c;
    }
    if (sanitized.empty()) sanitized = "block";
    blockPath /= sanitized + ".vblock";
    unsigned suffix = 2;
    while (std::filesystem::exists(blockPath)) {
        blockPath = blockPath.parent_path() /
            (sanitized + "_" + std::to_string(suffix++) + ".vblock");
    }
    {
        std::ofstream out(blockPath);
        out << "{\"texture\":\"" << fallback.to_string() << "\"";
        if (top.is_valid()) out << ",\"top\":\"" << top.to_string() << "\"";
        if (side.is_valid()) out << ",\"side\":\"" << side.to_string() << "\"";
        if (bottom.is_valid()) out << ",\"bottom\":\"" << bottom.to_string() << "\"";
        out << "}";
    }
    AssetMetadata meta;
    meta.id = UUID();
    meta.type = AssetType::Block;
    meta.sourcePath = blockPath;
    meta.cookedPath = blockPath;
    meta.isCooked = true;
    meta.contentHash = 0;
    if (const auto bm = m_assetRegistry.find(fallback); bm) meta.contentHash = bm->contentHash;
    if (!m_assetRegistry.register_asset(meta)) {
        std::cerr << "[BlockModelFaces] failed to register block asset " << blockPath.string() << std::endl;
        return UUID{ 0, 0 };
    }
    m_blockAssetCache[meta.id] = BlockAssetData{ fallback, top, bottom, side, UUID{ 0, 0 }, UUID{ 0, 0 } };
    m_blockAssetFailed.erase(meta.id);
    const auto registryPath = std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "AssetRegistry.db";
    if (!m_assetRegistry.save(registryPath)) {
        std::cerr << "[AssetRegistry] Could not persist block asset" << std::endl;
    }
    std::cout << "[BlockModelFaces] created " << meta.id.to_string() << " ("
              << blockPath.filename().string() << ")" << std::endl;
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
    data.normal = grab("normal");
    data.specular = grab("specular");
    if (!data.texture.is_valid() && !data.top.is_valid() && !data.side.is_valid() && !data.bottom.is_valid()) {
        m_blockAssetFailed.insert(blockAssetId);
        return false;
    }
    m_blockAssetCache[blockAssetId] = data;
    return true;
}

UUID EditorApplication::resolve_block_texture(const UUID& blockAssetId) {
    // Block assets now resolve to themselves: load_viewport_texture hooks them
    // to the per-face atlas [top|side|bottom], so the renderer samples the
    // right face per side instead of one texture on the whole cube.
    if (const auto meta = m_assetRegistry.find(blockAssetId);
        meta && meta->type == AssetType::Block) {
        return blockAssetId;
    }
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
// kVoxelSizeX/Y/Z live in the anonymous namespace above setup_play_runtime()
// (shared with play-mode real world collision).
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

UUID EditorApplication::resolve_voxel_type_block(uint16_t type) {
    // Explicit agent override (API `voxel-block <type> <uuid>`) wins.
    const auto overrideIt = m_voxelTypeBlocks.find(type);
    if (overrideIt != m_voxelTypeBlocks.end()) return overrideIt->second;
    // Name-based defaults from the BlockRegistry: 1=dirt, 2=grass, 3=stone,
    // 4=water. A texture pack import creates .vblock assets for these, so the
    // voxel volume picks them up automatically.
    static const char* keywords[5] = { nullptr, "dirt", "grass", "stone", "water" };
    if (type >= 5 || keywords[type] == nullptr) return UUID{ 0, 0 };
    const std::string keyword = keywords[type];
    const auto assets = m_assetRegistry.snapshot();
    // Prefer blocks with a real per-face map (top/side/bottom differ): the
    // assembled grass_block (top + side + dirt) must win over the single-face
    // grass_block_top/grass_block_side sidecars a pack import also produces.
    const auto faceScore = [&](const AssetMetadata& meta) {
        BlockAssetData data;
        if (!load_block_asset(meta.id, data)) return 0;
        int score = 0;
        if (data.top.is_valid() && data.top != data.side) ++score;
        if (data.bottom.is_valid() && data.bottom != data.side) ++score;
        return score;
    };
    const AssetMetadata* best = nullptr;
    int bestFaces = -1;
    bool bestHasBlock = false;
    for (const AssetMetadata& meta : assets) {
        if (meta.type != AssetType::Block) continue;
        std::string stem = meta.sourcePath.stem().string();
        std::transform(stem.begin(), stem.end(), stem.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (stem.find(keyword) == std::string::npos) continue;
        const int faces = faceScore(meta);
        const bool hasBlock = stem.find("block") != std::string::npos;
        // Face-composited block > explicitly-named block > first match.
        if (!best || faces > bestFaces ||
            (faces == bestFaces && hasBlock && !bestHasBlock)) {
            best = &meta;
            bestFaces = faces;
            bestHasBlock = hasBlock;
        }
    }
    if (best) {
        std::cerr << "[Editor] voxel type " << static_cast<int>(type) << " -> block "
                  << best->id.to_string() << " (" << best->sourcePath.filename().string() << ")" << std::endl;
    } else {
        std::cerr << "[Editor] voxel type " << static_cast<int>(type)
                  << " -> no block match (registry " << m_assetRegistry.size() << ")" << std::endl;
    }
    return best ? best->id : UUID{ 0, 0 };
}

void EditorApplication::rebuild_voxel_mesh(const UUID& entityId) {
    const auto gridIt = m_voxelStructures.find(entityId);
    if (gridIt == m_voxelStructures.end()) return;
    const Engine::Voxel::VoxelStructure& grid = *gridIt->second;
    const auto trIt = m_editorScene->transformComponents.find(entityId);
    const glm::vec3 origin = (trIt != m_editorScene->transformComponents.end())
                                 ? trIt->second.position
                                 : glm::vec3(0.0f);

    auto& mesh = m_voxelMeshes[entityId];
    if (mesh.valid) {
        // Same in-flight hazard as terrain regeneration: wait for the GPU
        // before freeing buffers the previous frame may still read.
        if (mesh.vb.buffer != VK_NULL_HANDLE || mesh.ib.buffer != VK_NULL_HANDLE)
            vkDeviceWaitIdle(m_device);
        if (mesh.vb.buffer != VK_NULL_HANDLE) destroy_buffer(mesh.vb);
        if (mesh.ib.buffer != VK_NULL_HANDLE) destroy_buffer(mesh.ib);
        mesh = EditorVoxelMesh{};
    }

    // Surface-only meshing: emit a face only when the neighbouring voxel is
    // air (or out of bounds). This renders the visible shell with per-face
    // normals (correct shading) instead of every solid cell as an up-shaded
    // box — the old version looked shapeless and wasted ~6x the geometry on
    // internal faces that were never visible.
    const auto solid = [&](int x, int y, int z) -> bool {
        if (x < 0 || y < 0 || z < 0 ||
            x >= kVoxelSizeX || y >= kVoxelSizeY || z >= kVoxelSizeZ) return false;
        return !grid.get(Engine::Voxel::Int3{ x, y, z }).empty();
    };
    struct Face { glm::vec3 n; glm::vec3 c[4]; };
    const Face faces[6] = {
        { { 0, 0,-1 }, { {0,0,0},{1,0,0},{1,1,0},{0,1,0} } }, // -Z
        { { 0, 0, 1 }, { {1,0,1},{0,0,1},{0,1,1},{1,1,1} } }, // +Z
        { {-1, 0, 0 }, { {0,0,1},{0,0,0},{0,1,0},{0,1,1} } }, // -X
        { { 1, 0, 0 }, { {1,0,0},{1,0,1},{1,1,1},{1,1,0} } }, // +X
        { { 0,-1, 0 }, { {0,0,0},{1,0,0},{1,0,1},{0,0,1} } }, // -Y
        { { 0, 1, 0 }, { {0,1,1},{1,1,1},{1,1,0},{0,1,0} } }, // +Y
    };
    const int noff[6][3] = {
        { 0, 0,-1 }, { 0, 0, 1 }, {-1, 0, 0 }, { 1, 0, 0 }, { 0,-1, 0 }, { 0, 1, 0 },
    };
    // Per voxel type: its own vertex/index group so each type can be drawn
    // with a different block atlas pipeline. The block atlas layout is the
    // same for every block ([top|side|bottom], 3 wide), so the UV mapping is
    // identical across types — only the sampled texture differs.
    std::map<uint16_t, std::vector<EditorVertex>> typeVerts;
    std::map<uint16_t, std::vector<uint32_t>> typeIndices;
    std::unordered_map<uint16_t, UUID> typeBlock;
    std::unordered_set<uint16_t> typeResolved;
    const glm::vec2 cornerUv[4] = { {0,0},{1,0},{1,1},{0,1} };
    for (int x = 0; x < kVoxelSizeX; ++x) {
        for (int y = 0; y < kVoxelSizeY; ++y) {
            for (int z = 0; z < kVoxelSizeZ; ++z) {
                const Engine::Voxel::VoxelValue v = grid.get(Engine::Voxel::Int3{ x, y, z });
                if (v.empty()) continue;
                const glm::vec3 base = origin + glm::vec3(x - kVoxelSizeX / 2, y, z - kVoxelSizeZ / 2);
                auto& tv = typeVerts[v.type];
                auto& ti = typeIndices[v.type];
                // Resolve the block once per type. NOTE: UUID's default ctor is
                // RANDOM, so we must track resolution explicitly — comparing
                // typeBlock[type] against UUID{0,0} would never match.
                if (!typeResolved.contains(v.type)) {
                    typeResolved.insert(v.type);
                    typeBlock[v.type] = resolve_voxel_type_block(v.type);
                }
                const UUID block = typeBlock[v.type];
                // Vertex color is only used by the untextured fallback path;
                // textured types sample white so the albedo comes from the atlas.
                const glm::vec3 color = block.is_valid() ? glm::vec3(1.0f) : voxel_type_color(v.type);
                for (int f = 0; f < 6; ++f) {
                    if (solid(x + noff[f][0], y + noff[f][1], z + noff[f][2])) continue;
                    // Atlas regions: +Y top [0,1/3], sides [1/3,2/3], -Y bottom [2/3,1].
                    float u0 = 1.0f / 3.0f, u1 = 2.0f / 3.0f;
                    if (f == 5) { u0 = 0.0f; u1 = 1.0f / 3.0f; }
                    else if (f == 4) { u0 = 2.0f / 3.0f; u1 = 1.0f; }
                    const uint32_t first = static_cast<uint32_t>(tv.size());
                    for (int c = 0; c < 4; ++c) {
                        EditorVertex ev;
                        ev.pos = base + faces[f].c[c];
                        ev.normal = faces[f].n;
                        ev.color = color;
                        ev.uv = glm::vec2(u0 + cornerUv[c].x * (u1 - u0), cornerUv[c].y);
                        tv.push_back(ev);
                    }
                    ti.push_back(first);
                    ti.push_back(first + 1);
                    ti.push_back(first + 2);
                    ti.push_back(first);
                    ti.push_back(first + 2);
                    ti.push_back(first + 3);
                }
            }
        }
    }
    if (typeVerts.empty()) return;
    // Concatenate the per-type groups into one VB/IB, recording a range per
    // type (indices are rebased onto the shared vertex buffer).
    std::vector<EditorVertex> verts;
    std::vector<uint32_t> indices;
    verts.reserve(32768);
    indices.reserve(49152);
    size_t vertBase = 0;
    for (auto& [type, tv] : typeVerts) {
        auto& ti = typeIndices[type];
        const uint32_t firstIndex = static_cast<uint32_t>(indices.size());
        for (const uint32_t idx : ti) indices.push_back(idx + static_cast<uint32_t>(vertBase));
        verts.insert(verts.end(), tv.begin(), tv.end());
        vertBase += tv.size();
        mesh.ranges.push_back(EditorVoxelRange{
            firstIndex, static_cast<uint32_t>(ti.size()), type, typeBlock[type] });
    }
    const VkDeviceSize vbSize = sizeof(EditorVertex) * verts.size();
    const VkDeviceSize ibSize = sizeof(uint32_t) * indices.size();
    create_buffer(vbSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  mesh.vb.buffer, mesh.vb.memory);
    create_buffer(ibSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  mesh.ib.buffer, mesh.ib.memory);
    safe_map_and_copy(m_device, mesh.vb.memory, 0, vbSize, verts.data());
    safe_map_and_copy(m_device, mesh.ib.memory, 0, ibSize, indices.data());
    mesh.indexCount = static_cast<uint32_t>(indices.size());
    mesh.valid = true;
}

void EditorApplication::ensure_voxel_pipelines() {
    // Rebuild dirty meshes and pre-build the block atlas pipelines OUTSIDE the
    // render pass (see the draw-site comment: building GPU resources mid-pass
    // hung the device). Runs every frame; the pipeline cache makes it a no-op
    // after the first successful build, and a failed build is retried.
    Scene* renderScene = m_playMode.get_active_scene();
    if (!renderScene) renderScene = m_editorScene.get();
    if (!renderScene) return;
    for (const auto& [id, vol] : renderScene->voxelVolumeComponents) {
        (void)vol;
        if (!renderScene->transformComponents.contains(id)) continue;
        if (m_voxelMeshesDirty.erase(id) != 0 || !m_voxelMeshes[id].valid) {
            ensure_voxel_volume(id, renderScene->voxelVolumeComponents[id].seed,
                                renderScene->voxelVolumeComponents[id].seaLevel);
            rebuild_voxel_mesh(id);
        }
        const auto& mesh = m_voxelMeshes[id];
        if (!mesh.valid) continue;
        for (const EditorVoxelRange& range : mesh.ranges) {
            if (range.blockId.is_valid()) {
                // Explicitly alpha-aware as well as guarded inside
                // ensure_texture_pipeline: blocks commonly use cutout pixels.
                ensure_texture_pipeline(range.blockId, m_blockGraphPipelines, true);
            }
        }
    }
}

void EditorApplication::draw_voxel_volumes(VkCommandBuffer cmd, const glm::mat4& viewProj, Scene* scene) {
    if (!scene || m_device == VK_NULL_HANDLE) return;
    for (const auto& [id, vol] : scene->voxelVolumeComponents) {
        (void)vol;
        if (!scene->transformComponents.contains(id)) continue;
        // Meshes are rebuilt and pipelines pre-built by ensure_voxel_pipelines()
        // in the main loop — never create GPU resources while a render pass is
        // being recorded (that hung the device). A volume that is not ready
        // yet simply skips this frame.
        const auto& mesh = m_voxelMeshes[id];
        if (!mesh.valid || mesh.vb.buffer == VK_NULL_HANDLE) continue;
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.vb.buffer, &offset);
        vkCmdBindIndexBuffer(cmd, mesh.ib.buffer, 0, VK_INDEX_TYPE_UINT32);
        for (const EditorVoxelRange& range : mesh.ranges) {
            // BlockRegistry-backed types render textured (per-face atlas
            // [top|side|bottom], sampled by a material-graph pipeline); types
            // without a matching block fall back to the vertex-color pipeline.
            GraphMaterialPipeline* gmp = nullptr;
            if (range.blockId.is_valid()) {
                gmp = ensure_texture_pipeline(range.blockId, m_blockGraphPipelines, true);
                if (gmp) {
                    write_material_ubo(*gmp, nullptr, nullptr);
                    write_light_ubo(*gmp, scene, m_editorCamera.position);
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gmp->pipeline);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gmp->layout,
                                            0, 1, &gmp->descriptorSet, 0, nullptr);
                    const Rendering::MaterialPushConstants pc{ viewProj, glm::mat4(1.0f) };
                    vkCmdPushConstants(cmd, gmp->layout, VK_SHADER_STAGE_VERTEX_BIT,
                                       0, sizeof(pc), &pc);
                }
            }
            if (!gmp) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_scenePipeline);
                push_constants(cmd, m_scenePipelineLayout, viewProj, glm::vec4(1.0f));
            }
            vkCmdDrawIndexed(cmd, range.indexCount, 1, range.firstIndex, 0, 0);
        }
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

// Builds (or returns cached) the per-face atlas for a Block asset: a 3-wide
// image [top | side | bottom] composited from the .vblock face maps, with the
// main texture as fallback for missing faces. Blocks without face maps still
// get the 3-region atlas (all regions = the main texture), so the cube UVs
// stay uniform across every block.
bool EditorApplication::ensure_block_atlas(const UUID& blockId, GraphTexture& out) {
    const auto meta = m_assetRegistry.find(blockId);
    if (!meta || meta->type != AssetType::Block) return false;
    BlockAssetData data;
    if (!load_block_asset(blockId, data)) return false;

    const auto validTexture = [&](const UUID& id) -> UUID {
        if (!id.is_valid()) return UUID{ 0, 0 };
        const auto candidate = m_assetRegistry.find(id);
        return candidate && candidate->type == AssetType::Texture && !candidate->cookedPath.empty()
             ? id : UUID{ 0, 0 };
    };
    const auto lowerStem = [](const std::filesystem::path& path) {
        std::string stem = path.stem().string();
        std::transform(stem.begin(), stem.end(), stem.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return stem;
    };

    // REGRESSION GUARD (BUG-EDITOR-ASSET-TEXTURES-003): sidecars survive a
    // registry rebuild, so their UUIDs can be *valid but wrong* after assets
    // move or are re-imported.  Resolve semantic Minecraft face names against
    // the current registry FIRST.  Selecting data.texture first is unsafe: a
    // stale image then becomes the atlas fallback and produces black/wrong
    // block faces despite top/side/bottom textures still existing correctly.
    const std::string blockStem = lowerStem(meta->sourcePath);
    std::unordered_map<std::string, UUID> textureByStem;
    for (const AssetMetadata& candidate : m_assetRegistry.snapshot()) {
        if (candidate.type == AssetType::Texture && !candidate.sourcePath.empty() &&
            !candidate.cookedPath.empty()) {
            textureByStem.emplace(lowerStem(candidate.sourcePath), candidate.id);
        }
    }
    const auto named = [&](std::initializer_list<std::string> names) -> UUID {
        for (const std::string& name : names) {
            const auto found = textureByStem.find(name);
            if (found != textureByStem.end()) return found->second;
        }
        return UUID{ 0, 0 };
    };
    const UUID exact = named({ blockStem });
    const UUID namedTop = named({ blockStem + "_top", blockStem + "_up" });
    const UUID namedSide = named({ blockStem + "_side" });
    const UUID namedBottom = named({ blockStem + "_bottom", blockStem + "_down" });
    UUID mainTex = exact;
    if (!mainTex.is_valid()) mainTex = namedSide;
    if (!mainTex.is_valid()) mainTex = namedTop;
    if (!mainTex.is_valid()) mainTex = namedBottom;
    if (!mainTex.is_valid()) mainTex = validTexture(data.texture);
    if (!mainTex.is_valid()) mainTex = validTexture(data.side);
    if (!mainTex.is_valid()) mainTex = validTexture(data.top);
    if (!mainTex.is_valid()) mainTex = validTexture(data.bottom);
    if (!mainTex.is_valid()) return false;
    const auto texMeta = m_assetRegistry.find(mainTex);
    if (!texMeta || texMeta->type != AssetType::Texture || texMeta->cookedPath.empty()) return false;

    UUID topId = namedTop.is_valid() ? namedTop : validTexture(data.top);
    if (!topId.is_valid()) topId = mainTex;
    UUID sideId = namedSide.is_valid() ? namedSide : validTexture(data.side);
    if (!sideId.is_valid()) sideId = mainTex;
    UUID bottomId = namedBottom.is_valid() ? namedBottom : validTexture(data.bottom);
    if (!bottomId.is_valid() && blockStem == "grass_block") bottomId = named({ "dirt" });
    if (!bottomId.is_valid() && blockStem.ends_with("_log")) bottomId = topId;
    if (!bottomId.is_valid()) bottomId = mainTex;

    const auto textureHash = [&](const UUID& id) -> std::uint64_t {
        const auto candidate = m_assetRegistry.find(id);
        return candidate ? candidate->contentHash : 0u;
    };
    std::uint64_t atlasHash = meta->contentHash;
    for (const UUID id : { mainTex, topId, sideId, bottomId }) {
        atlasHash ^= textureHash(id) + 0x9e3779b97f4a7c15ull + (atlasHash << 6u) + (atlasHash >> 2u);
    }
    const auto hashIt = m_blockAtlasHashes.find(blockId);
    if (hashIt != m_blockAtlasHashes.end()) {
        if (hashIt->second == atlasHash) {
            const auto texIt = m_blockAtlasTextures.find(blockId);
            if (texIt != m_blockAtlasTextures.end() && texIt->second.image != VK_NULL_HANDLE) {
                out = texIt->second;
                return true;
            }
        }
        const auto staleIt = m_blockAtlasTextures.find(blockId);
        if (staleIt != m_blockAtlasTextures.end()) {
            destroy_graph_texture(staleIt->second);
            m_blockAtlasTextures.erase(staleIt);
        }
        m_blockAtlasHashes.erase(hashIt);
    }

    std::string err;
    DecodedTexturePixels base;
    if (!decode_cooked_texture_pixels(texMeta->cookedPath, 256, base, err)) return false;
    const uint32_t w = base.width, h = base.height;
    if (w == 0 || h == 0) return false;
    const auto facePixels = [&](const UUID& faceId) {
        Editor::BlockFacePixels result{ w, h, base.rgba };
        const auto fm = m_assetRegistry.find(faceId);
        if (faceId.is_valid() && fm && fm->type == AssetType::Texture && !fm->cookedPath.empty()) {
            DecodedTexturePixels px;
            if (decode_cooked_texture_pixels(fm->cookedPath, 256, px, err) &&
                px.width == w && px.height == h) {
                result.rgba = std::move(px.rgba);
            }
        }
        return result;
    };
    const Editor::BlockFacePixels top = facePixels(topId);
    const Editor::BlockFacePixels side = facePixels(sideId);
    const Editor::BlockFacePixels bottom = facePixels(bottomId);
    Editor::BlockFacePixels atlas;
    if (!Editor::compose_horizontal_block_atlas(top, side, bottom, atlas, &err)) return false;
    // Minecraft stores foliage albedo as a grayscale mask and applies the
    // biome tint at render time.  The editor material graph has no biome
    // context, so bake the neutral forest tint into this small atlas while
    // preserving alpha (the cutout is handled by the material pipeline).
    if (blockStem.find("leaves") != std::string::npos ||
        blockStem.find("foliage") != std::string::npos) {
        for (std::size_t i = 0; i + 3 < atlas.rgba.size(); i += 4) {
            const float luma = (0.2126f * atlas.rgba[i] +
                                0.7152f * atlas.rgba[i + 1] +
                                0.0722f * atlas.rgba[i + 2]) / 255.0f;
            const float value = std::clamp(luma * 1.75f + 0.28f, 0.42f, 1.18f);
            atlas.rgba[i] = static_cast<std::uint8_t>(std::clamp(0.25f * value, 0.0f, 1.0f) * 255.0f);
            atlas.rgba[i + 1] = static_cast<std::uint8_t>(std::clamp(0.67f * value, 0.0f, 1.0f) * 255.0f);
            atlas.rgba[i + 2] = static_cast<std::uint8_t>(std::clamp(0.19f * value, 0.0f, 1.0f) * 255.0f);
        }
    }
    GraphTexture atlasTex;
    if (!upload_texture_pixels(atlas.width, atlas.height, atlas.rgba, 1, base.srgb, atlasTex, err)) return false;
    m_blockAtlasTextures[blockId] = atlasTex;
    m_blockAtlasHashes[blockId] = atlasHash;
    out = atlasTex;
    return true;
}

bool EditorApplication::load_viewport_texture(const UUID& assetId, GraphTexture& out, std::string& error,
                                              uint32_t maxDim) {
    const auto metaOpt = m_assetRegistry.find(assetId);
    if (!metaOpt) {
        error = "texture asset not found in registry";
        std::cerr << "[Editor] load_viewport_texture: missing asset " << assetId.to_string()
                  << " (registry size " << m_assetRegistry.size() << ")" << std::endl;
        return false;
    }
    const AssetMetadata& meta = *metaOpt;
    // Block assets sample their per-face atlas [top|side|bottom] instead of a
    // single texture on all six faces. This hook serves BOTH the scene
    // material-graph path and the block thumbnail pipeline.
    if (meta.type == AssetType::Block) {
        return ensure_block_atlas(assetId, out);
    }
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
    safe_map_and_copy(m_device, stagingMemory, 0, imageSize, rgba.data());
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
    safe_map_and_copy(m_device, stagingMemory, 0, imageSize, halfRgba.data());
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
            // Only destroy textures the pipeline owns (atlas textures are
            // borrowed from m_blockAtlasTextures).
            for (size_t i = 0; i < textures.size(); ++i) {
                if (i < out.textureIsAtlas.size() && out.textureIsAtlas[i]) continue;
                destroy_graph_texture(textures[i]);
            }
            out.textureIsAtlas.clear();
            return false;
        }
        // Block assets sample their per-face atlas, which is OWNED by
        // m_blockAtlasTextures — the pipeline only references it, so record
        // that so destroy_graph_pipeline doesn't free it twice.
        const auto meta = m_assetRegistry.find(id);
        out.textureIsAtlas.push_back(meta && meta->type == AssetType::Block);
        textures.push_back(std::move(tex));
    }
    out.textures = std::move(textures);
    const Rendering::GlslGenerationResult gen = material_graph_to_glsl(graph);
    if (!gen) {
        out.lastError = gen.errors.empty() ? "material graph compile failed" : gen.errors[0].message;
        return false;
    }
    // VC_EDITOR_DUMP_MATERIAL_GLSL=1: log the generated fragment source (useful
    // to debug alpha cutout, BRDF and sampler wiring without instrumenting
    // the generator).
    if (std::getenv("VC_EDITOR_DUMP_MATERIAL_GLSL") != nullptr) {
        std::cout << "[MaterialGLSL] --- generated fragment source ---\n"
                  << gen.source << "\n[MaterialGLSL] --- end ---" << std::endl;
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
        // Block/pixel-art textures use NEAREST filtering (Minecraft look);
        // skins, decals and PBR textures keep trilinear + anisotropic.
        const bool isBlockAtlas = i < out.textureIsAtlas.size() && out.textureIsAtlas[i];
        imageInfo.sampler = isBlockAtlas ? m_blockDrawSampler : m_offscreen.sampler;
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
    for (size_t i = 0; i < p.textures.size(); ++i) {
        // Block atlases are owned by m_blockAtlasTextures — reference only.
        if (i < p.textureIsAtlas.size() && p.textureIsAtlas[i]) continue;
        destroy_graph_texture(p.textures[i]);
    }
    p.textures.clear();
    p.textureIsAtlas.clear();
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

// ---------------------------------------------------------------------------
// BUG-EDITOR-LIGHTS-001: shared light-entry collection for every
// LightUboData consumer. Previously ONLY the material-graph pipelines
// (blocks, voxels, skins) received scene lights; the basic mesh path shaded
// with a hardcoded light direction, so placing point/spot/area lights
// changed nothing on screen. Both paths now share this code.
// ---------------------------------------------------------------------------
static void fill_scene_light_entries(Rendering::LightUboData& data, const Scene* scene) {
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
            // Editor lights use lux-like intensity (default sun = 10000) but the
            // MaterialPipeline Lambert term adds lightColor.rgb straight into
            // lightAccum (lit = baseColor*(0.22+0.78*lightAccum)), which expects
            // ~1.0 for a full-strength light. Normalize to the default-sun
            // reference so material-graph lit objects (blocks, voxels, skins,
            // decals) get balanced light instead of white-out or ambient-only.
            const glm::vec3 colorIntensity = light.color * (light.intensity / 10000.0f);
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
}

void EditorApplication::init_scene_light_resources() {
    // Descriptor set layout (BUG-EDITOR-SHADOWS-002 / BUG-EDITOR-GI-001):
    //   0: SceneLights UBO (LightUboData)
    //   1: sun shadow map        (sampler2DShadow)
    //   2: spot shadow atlas     (sampler2DShadow, 4 tiles)
    //   3: point shadow atlas    (sampler2DShadow, 6 face tiles, linear depth)
    //   4: EditorShadowUbo       (spot VPs, point slot-0, probe irradiance grid)
    VkDescriptorSetLayoutBinding bindings[5]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    for (int i = 1; i <= 3; ++i) {
        bindings[i].binding = static_cast<uint32_t>(i);
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    bindings[4].binding = 4;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo layoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    layoutInfo.bindingCount = 5;
    layoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr,
                                    &m_sceneLightSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create scene light set layout");
    }

    create_buffer(sizeof(Rendering::LightUboData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  m_sceneLightBuffer, m_sceneLightMemory);
    create_buffer(sizeof(EditorShadowUbo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  m_editorShadowUbo, m_editorShadowUboMemory);

    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = 2;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = 3;
    VkDescriptorPoolCreateInfo poolInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_sceneLightPool) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create scene light descriptor pool");
    }
    VkDescriptorSetAllocateInfo allocInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    allocInfo.descriptorPool = m_sceneLightPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_sceneLightSetLayout;
    if (vkAllocateDescriptorSets(m_device, &allocInfo, &m_sceneLightSet) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate scene light descriptor set");
    }
    VkDescriptorBufferInfo bufferInfo{ m_sceneLightBuffer, 0, sizeof(Rendering::LightUboData) };
    VkDescriptorBufferInfo shadowBufferInfo{ m_editorShadowUbo.buffer, 0, sizeof(EditorShadowUbo) };
    VkDescriptorImageInfo sunShadowInfo{ m_shadowMap.sampler, m_shadowMap.view,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo spotShadowInfo{ m_spotShadow.sampler, m_spotShadow.view,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo pointShadowInfo{ m_pointShadow.sampler, m_pointShadow.view,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet writes[5]{};
    writes[0].dstSet = m_sceneLightSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo = &bufferInfo;
    writes[1].dstSet = m_sceneLightSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &sunShadowInfo;
    writes[2].dstSet = m_sceneLightSet;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[2].pImageInfo = &spotShadowInfo;
    writes[3].dstSet = m_sceneLightSet;
    writes[3].dstBinding = 3;
    writes[3].descriptorCount = 1;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[3].pImageInfo = &pointShadowInfo;
    writes[4].dstSet = m_sceneLightSet;
    writes[4].dstBinding = 4;
    writes[4].descriptorCount = 1;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[4].pBufferInfo = &shadowBufferInfo;
    vkUpdateDescriptorSets(m_device, 5, writes, 0, nullptr);

    // BUG-EDITOR-GI-001: the headless probe grid core (Agente 1) behind the
    // viewport's indirect ambient.
    init_gi_probes();
}

void EditorApplication::refresh_shadow_descriptors() {
    // Re-writes bindings 1-3 after the shadow targets were (re)created — a
    // resize destroys and rebuilds the samplers/views, which would otherwise
    // leave the scene light set pointing at dead objects.
    if (m_sceneLightSet == VK_NULL_HANDLE) return;
    if (m_shadowMap.sampler == VK_NULL_HANDLE || m_spotShadow.sampler == VK_NULL_HANDLE ||
        m_pointShadow.sampler == VK_NULL_HANDLE) {
        return;
    }
    VkDescriptorImageInfo infos[3]{
        { m_shadowMap.sampler, m_shadowMap.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { m_spotShadow.sampler, m_spotShadow.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { m_pointShadow.sampler, m_pointShadow.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
    };
    VkWriteDescriptorSet writes[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        writes[i].dstSet = m_sceneLightSet;
        writes[i].dstBinding = 1 + i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &infos[i];
    }
    vkUpdateDescriptorSets(m_device, 3, writes, 0, nullptr);
}

void EditorApplication::destroy_scene_light_resources() {
    if (m_sceneLightBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, m_sceneLightBuffer, nullptr);
        m_sceneLightBuffer = VK_NULL_HANDLE;
    }
    if (m_sceneLightMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_sceneLightMemory, nullptr);
        m_sceneLightMemory = VK_NULL_HANDLE;
    }
    if (m_editorShadowUbo.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, m_editorShadowUbo.buffer, nullptr);
        m_editorShadowUbo = GPUBuffer{};
    }
    if (m_editorShadowUboMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_editorShadowUboMemory, nullptr);
        m_editorShadowUboMemory = VK_NULL_HANDLE;
    }
    if (m_sceneLightPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_sceneLightPool, nullptr);
        m_sceneLightPool = VK_NULL_HANDLE;
    }
    if (m_sceneLightSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_device, m_sceneLightSetLayout, nullptr);
        m_sceneLightSetLayout = VK_NULL_HANDLE;
    }
    m_sceneLightSet = VK_NULL_HANDLE;
}

void EditorApplication::update_scene_light_ubo(const Scene* scene) {
    if (m_sceneLightMemory == VK_NULL_HANDLE) return;
    Rendering::LightUboData data{};
    data.cameraPosition = glm::vec4(m_editorCamera.position, 1.0f);
    // BUG-EDITOR-SHADOWS-001: the basic mesh path now samples the real sun
    // shadow map (binding 1). w = 1/shadowMapSize for the PCF texel offsets.
    data.sunViewProj = m_shadowMap.viewProj;
    data.shadowParams = glm::vec4(m_shadowMap.enabled ? 1.0f : 0.0f, 0.0006f, 1.0f,
                                  1.0f / float(m_shadowMap.size));
    const glm::mat4 view = m_editorCamera.get_view_matrix();
    data.cameraForward = glm::vec4(
        glm::normalize(glm::vec3(-view[2][0], -view[2][1], -view[2][2])), 0.0f);
    fill_scene_light_entries(data, scene);
    void* mapped = nullptr;
    if (vkMapMemory(m_device, m_sceneLightMemory, 0, sizeof(data), 0, &mapped) != VK_SUCCESS) return;
    std::memcpy(mapped, &data, sizeof(data));
    vkUnmapMemory(m_device, m_sceneLightMemory);
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
    fill_scene_light_entries(data, scene);
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
        destroy_buffer(resource.vb);
        destroy_buffer(resource.ib);
    }
    m_meshResources.clear();
}

} // namespace Engine
