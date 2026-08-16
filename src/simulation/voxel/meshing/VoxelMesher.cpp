#include "VoxelMesher.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>

ChunkMeshResult VoxelMesher::build(const ChunkSnapshot& snapshot) {
    ChunkMeshResult result;
    result.chunk = snapshot.id;
    result.sourceRevision = snapshot.revision;
    result.verticalExtent = snapshot.verticalExtent;
    result.neighborSeen = snapshot.neighborSeen;
    auto& mesh = result.mesh;

    const RuntimeBlockId waterId = runtime_id(BlockType::Water);
    const RuntimeBlockId glassId = runtime_id(BlockType::Glass);
    const RuntimeBlockId grassId = runtime_id(BlockType::Grass);

    // Builtin ids resolve through the engine material table; dynamic
    // (registry-defined) ids resolve through the snapshot's runtime table.
    // Workers never touch the registry — the table is embedded at dispatch.
    auto material_for = [&](RuntimeBlockId id, const glm::vec3& normal) {
        glm::vec4 color(1.0f, 0.0f, 1.0f, 1.0f);
        float layer = -1.0f;
        if (is_builtin_block(id)) {
            const BlockType type = static_cast<BlockType>(id);
            color = get_block_color(type, normal);
            layer = get_block_texture_layer(type, normal);
        } else if (const RuntimeBlockInfo* info = snapshot.find_runtime_block(id)) {
            // State-aware material (FALTANTES item 5): state 0 is the default
            // (base per-face material), identical to the block-level rules.
            color = resolve_state_material(*info, 0, normal);
        }
        return std::pair<glm::vec4, float>{ color, layer };
    };

    // FALTANTES §8 item 167: a fluid is ANY block that drives the fluid
    // simulation — builtin Water/Lava, or a registry-defined (dynamic) block
    // whose class is Fluid. is_fluid feeds face culling (fluids never draw
    // shared faces).
    auto is_fluid = [&](RuntimeBlockId id) {
        if (is_builtin_block(id)) return is_fluid_block(static_cast<BlockType>(id));
        const RuntimeBlockInfo* info = snapshot.find_runtime_block(id);
        return info != nullptr && info->fluid;
    };
    // Which fluids get the FLUID MESH (transparent, height-sampled by level,
    // emitted into waterMeshVertices): builtin Water (Water texture layer) and
    // project fluids (dynamic, class "fluid", color-only). Builtin lava keeps
    // the opaque textured cube path — its material carries the Lava texture
    // layer, and the water pass material check is the hard geometry contract
    // that separates the two passes.
    auto is_fluid_mesh = [&](RuntimeBlockId id) {
        if (is_builtin_block(id)) return static_cast<BlockType>(id) == BlockType::Water;
        const RuntimeBlockInfo* info = snapshot.find_runtime_block(id);
        return info != nullptr && info->fluid;
    };


    float offsetX = static_cast<float>(snapshot.id.coord.x * CHUNK_SIZE_X);
    float offsetZ = static_cast<float>(snapshot.id.coord.z * CHUNK_SIZE_Z);

    auto add_face = [&](const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& p3,
                        const glm::vec3& normal, RuntimeBlockId type) {
        const auto [col, layer] = material_for(type, normal);

        glm::vec3 uv0(0.0f, 0.0f, layer);
        glm::vec3 uv1(1.0f, 0.0f, layer);
        glm::vec3 uv2(1.0f, 1.0f, layer);
        glm::vec3 uv3(0.0f, 1.0f, layer);
        auto& vertices = is_fluid_mesh(type) ? mesh.waterMeshVertices : mesh.meshVertices;
        vertices.push_back({ p0, normal, col, uv0 });
        vertices.push_back({ p1, normal, col, uv1 });
        vertices.push_back({ p2, normal, col, uv2 });
        vertices.push_back({ p0, normal, col, uv0 });
        vertices.push_back({ p2, normal, col, uv2 });
        vertices.push_back({ p3, normal, col, uv3 });
    };

    auto should_draw_face = [&](RuntimeBlockId current, RuntimeBlockId neighbor) {
        if (neighbor == kRuntimeAirId) return true;
        const bool currentFluid = is_fluid(current);
        const bool neighborFluid = is_fluid(neighbor);
        const bool currentLeaf = is_leaf_block(as_builtin_block(current));
        const bool neighborLeaf = is_leaf_block(as_builtin_block(neighbor));
        if (currentFluid && neighborFluid) return false;
        if (currentFluid && !neighborFluid) return true;
        if (!currentFluid && neighborFluid) return true;
        if (!currentLeaf && neighborLeaf) return true;
        if (current != glassId && neighbor == glassId) return true;
        // Dynamic (registry-defined) blocks: transparency and occlusion drive
        // face culling (FALTANTES §14). Transparent blocks always draw (glass
        // semantics); two opaque occluding blocks skip the shared face; a
        // non-occluding block (fence/plant) draws its face even against an
        // opaque neighbor so the shape reads.
        if (!is_builtin_block(current) || !is_builtin_block(neighbor)) {
            const RuntimeBlockInfo* curInfo = snapshot.find_runtime_block(current);
            const RuntimeBlockInfo* nbInfo = snapshot.find_runtime_block(neighbor);
            const bool curTransparent = curInfo && curInfo->transparent;
            const bool curOccludes = !curInfo || curInfo->occludes;
            const bool nbOccludes = !nbInfo || nbInfo->occludes;
            if (curTransparent) return true;
            if (curOccludes && nbOccludes) return false;
            return true;
        }
        return false;
    };

    const auto& meshLayers = snapshot.layers;

    for (int x = 0; x < CHUNK_SIZE_X; x++) {
        for (std::size_t layerIndex = 0; layerIndex < meshLayers.size(); ++layerIndex) {
            const int y = meshLayers[layerIndex];
            for (int z = 0; z < CHUNK_SIZE_Z; z++) {
                RuntimeBlockId type = snapshot.block(x, y, z);
                if (type == kRuntimeAirId) continue;

                glm::vec3 pos(offsetX + x, static_cast<float>(y), offsetZ + z);
                auto neighbor_block = [&](int dx, int dy, int dz) {
                    const int nx = x + dx, ny = y + dy, nz = z + dz;
                    if (nx >= 0 && nx < CHUNK_SIZE_X && ny >= 0 && ny < CHUNK_SIZE_Y && nz >= 0 && nz < CHUNK_SIZE_Z) {
                        return snapshot.block(nx, ny, nz);
                    }
                    // A frontier of streaming is unknown, not air. Returning the
                    // current material suppresses temporary 384-block-high walls
                    // of water/terrain. World::update dirties this chunk as soon as
                    // the horizontal neighbor is uploaded, revealing the real edge.
                    if (dy == 0 && (nx < 0 || nx >= CHUNK_SIZE_X || nz < 0 || nz >= CHUNK_SIZE_Z)) {
                        bool known = false;
                        const RuntimeBlockId neighbor = snapshot.halo_block(nx, ny, nz, known);
                        return known ? neighbor : type;
                    }
                    // The only remaining out-of-range case is below/above the
                    // logical column. It is air and needs no World lookup.
                    return kRuntimeAirId;
                };

                RuntimeBlockId topNeighbor = neighbor_block(0, 1, 0);
                RuntimeBlockId bottomNeighbor = neighbor_block(0, -1, 0);
                RuntimeBlockId eastNeighbor = neighbor_block(1, 0, 0);
                RuntimeBlockId westNeighbor = neighbor_block(-1, 0, 0);
                RuntimeBlockId northNeighbor = neighbor_block(0, 0, 1);
                RuntimeBlockId southNeighbor = neighbor_block(0, 0, -1);

                if (is_leaf_block(as_builtin_block(type))) {
                    const bool exposed = topNeighbor == kRuntimeAirId || bottomNeighbor == kRuntimeAirId ||
                                         eastNeighbor == kRuntimeAirId || westNeighbor == kRuntimeAirId ||
                                         northNeighbor == kRuntimeAirId || southNeighbor == kRuntimeAirId;
                    const float seed = std::sin(pos.x * 15.127f + pos.y * 37.719f + pos.z * 91.137f) * 43758.5453f;
                    const float random = seed - std::floor(seed);
                    if (exposed) {
                        const float sizeSeed = std::sin(seed * 0.0137f + 71.71f) * 24634.6345f;
                        const float sizeRandom = sizeSeed - std::floor(sizeSeed);
                        const float size = 1.08f + sizeRandom * 0.34f;
                        mesh.foliageInstances.push_back({ glm::vec4(pos.x + 0.5f, pos.y + 0.48f, pos.z + 0.5f, size) });
                    }
                    continue;
                }

                if (is_fluid_mesh(type)) {
                    auto sample_height = [&](int dx, int dz) {
                        const int nx = x + dx, nz = z + dz;
                        uint8_t level = WATER_LEVEL_NONE;
                        if (nx >= 0 && nx < CHUNK_SIZE_X && nz >= 0 && nz < CHUNK_SIZE_Z) {
                            level = snapshot.water_level(nx, y, nz);
                        } else {
                            bool known = false;
                            const uint8_t neighborLevel = snapshot.halo_water(nx, y, nz, known);
                            if (known) level = neighborLevel;
                        }
                        return level == WATER_LEVEL_NONE ? 0.0f : water_render_height(level);
                    };
                    auto corner_height = [&](int sx, int sz) {
                        const float samples[4] = {
                            sample_height(0, 0), sample_height(sx, 0),
                            sample_height(0, sz), sample_height(sx, sz)
                        };
                        float sum = 0.0f;
                        int count = 0;
                        for (float height : samples) {
                            if (height <= 0.0f) continue;
                            sum += height;
                            ++count;
                        }
                        return count > 0 ? sum / static_cast<float>(count) : 0.0f;
                    };

                    const float hSW = corner_height(-1, -1);
                    const float hSE = corner_height(1, -1);
                    const float hNE = corner_height(1, 1);
                    const float hNW = corner_height(-1, 1);

                    if (topNeighbor != type) {  // same fluid above: no top face
                        add_face(pos + glm::vec3(0, hNW, 1), pos + glm::vec3(1, hNE, 1),
                                 pos + glm::vec3(1, hSE, 0), pos + glm::vec3(0, hSW, 0),
                                 glm::vec3(0, 1, 0), type);
                    }
                    if (bottomNeighbor == kRuntimeAirId) {
                        add_face(pos + glm::vec3(0, 0, 0), pos + glm::vec3(1, 0, 0),
                                 pos + glm::vec3(1, 0, 1), pos + glm::vec3(0, 0, 1),
                                 glm::vec3(0, -1, 0), type);
                    }
                    if (eastNeighbor == kRuntimeAirId) {
                        add_face(pos + glm::vec3(1, 0, 0), pos + glm::vec3(1, hSE, 0),
                                 pos + glm::vec3(1, hNE, 1), pos + glm::vec3(1, 0, 1), glm::vec3(1, 0, 0), type);
                    }
                    if (westNeighbor == kRuntimeAirId) {
                        add_face(pos + glm::vec3(0, 0, 1), pos + glm::vec3(0, hNW, 1),
                                 pos + glm::vec3(0, hSW, 0), pos + glm::vec3(0, 0, 0), glm::vec3(-1, 0, 0), type);
                    }
                    if (northNeighbor == kRuntimeAirId) {
                        add_face(pos + glm::vec3(1, 0, 1), pos + glm::vec3(1, hNE, 1),
                                 pos + glm::vec3(0, hNW, 1), pos + glm::vec3(0, 0, 1), glm::vec3(0, 0, 1), type);
                    }
                    if (southNeighbor == kRuntimeAirId) {
                        add_face(pos + glm::vec3(0, 0, 0), pos + glm::vec3(0, hSW, 0),
                                 pos + glm::vec3(1, hSE, 0), pos + glm::vec3(1, 0, 0), glm::vec3(0, 0, -1), type);
                    }
                    continue;
                }

                // Top (+Y)
                if (should_draw_face(type, topNeighbor)) {
                    add_face(pos + glm::vec3(0, 1, 1), pos + glm::vec3(1, 1, 1), pos + glm::vec3(1, 1, 0), pos + glm::vec3(0, 1, 0),
                             glm::vec3(0, 1, 0), type);

                    if (type == grassId && topNeighbor == kRuntimeAirId) {
                        uint32_t edgeMask = 0;
                        if (westNeighbor != grassId) edgeMask |= 1u;
                        if (eastNeighbor != grassId) edgeMask |= 2u;
                        if (southNeighbor != grassId) edgeMask |= 4u;
                        if (northNeighbor != grassId) edgeMask |= 8u;
                        for (int bladeCard = 0; bladeCard < 128; ++bladeCard) {
                            const int gridX = bladeCard & 15;
                            const int gridZ = bladeCard >> 4;
                            const float seed = std::sin(pos.x * 12.9898f + pos.z * 78.233f + bladeCard * 37.719f) * 43758.5453f;
                            const float random = seed - std::floor(seed);
                            const float seed2 = std::sin(seed * 0.017f + 19.19f) * 24634.6345f;
                            const float random2 = seed2 - std::floor(seed2);
                            const float cellX = (static_cast<float>(gridX) + 0.15f + random * 0.70f) * 0.0625f;
                            const float cellZ = (static_cast<float>(gridZ) + 0.15f + random2 * 0.70f) * 0.125f;
                            const float localX = 0.025f + cellX * 0.95f;
                            const float localZ = 0.025f + cellZ * 0.95f;
                            mesh.grassInstances.push_back({ glm::vec4(pos.x + localX, pos.y + 1.002f, pos.z + localZ,
                                                                 static_cast<float>(edgeMask) + random) });
                        }
                    }
                }
                // Bottom (-Y)
                if (should_draw_face(type, bottomNeighbor)) {
                    add_face(pos + glm::vec3(0, 0, 0), pos + glm::vec3(1, 0, 0), pos + glm::vec3(1, 0, 1), pos + glm::vec3(0, 0, 1),
                             glm::vec3(0, -1, 0), type);
                }
                // East (+X)
                if (should_draw_face(type, eastNeighbor)) {
                    add_face(pos + glm::vec3(1, 0, 0), pos + glm::vec3(1, 1, 0), pos + glm::vec3(1, 1, 1), pos + glm::vec3(1, 0, 1),
                             glm::vec3(1, 0, 0), type);
                }
                // West (-X)
                if (should_draw_face(type, westNeighbor)) {
                    add_face(pos + glm::vec3(0, 0, 1), pos + glm::vec3(0, 1, 1), pos + glm::vec3(0, 1, 0), pos + glm::vec3(0, 0, 0),
                             glm::vec3(-1, 0, 0), type);
                }
                // North (+Z)
                if (should_draw_face(type, northNeighbor)) {
                    add_face(pos + glm::vec3(1, 0, 1), pos + glm::vec3(1, 1, 1), pos + glm::vec3(0, 1, 1), pos + glm::vec3(0, 0, 1),
                             glm::vec3(0, 0, 1), type);
                }
                // South (-Z)
                if (should_draw_face(type, southNeighbor)) {
                    add_face(pos + glm::vec3(0, 0, 0), pos + glm::vec3(0, 1, 0), pos + glm::vec3(1, 1, 0), pos + glm::vec3(1, 0, 0),
                             glm::vec3(0, 0, -1), type);
                }
            }
        }
    }

    mesh.pendingVertexCount = static_cast<uint32_t>(mesh.meshVertices.size());
    mesh.pendingWaterVertexCount = static_cast<uint32_t>(mesh.waterMeshVertices.size());
    mesh.pendingGrassInstanceCount = static_cast<uint32_t>(mesh.grassInstances.size());
    mesh.pendingFoliageInstanceCount = static_cast<uint32_t>(mesh.foliageInstances.size());

    // Hard geometry contract: a chunk may only publish vertices inside its own
    // 16x16 column and inside the logical world height. Effects can alter color
    // or normals later, but can never inject arbitrary world geometry.
    const float minX = offsetX - 0.001f;
    const float maxX = offsetX + CHUNK_SIZE_X + 0.001f;
    const float minZ = offsetZ - 0.001f;
    const float maxZ = offsetZ + CHUNK_SIZE_Z + 0.001f;
    auto valid_vertices = [&](const std::vector<VoxelVertex>& vertices, bool waterPass) {
        if (vertices.size() % 6 != 0) return false;
        for (const VoxelVertex& vertex : vertices) {
            const glm::vec3& p = vertex.position;
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
                p.x < minX || p.x > maxX || p.z < minZ || p.z > maxZ ||
                p.y < -0.001f || p.y > static_cast<float>(CHUNK_SIZE_Y) + 0.001f) return false;
            const bool waterMaterial = std::abs(vertex.uv.z - static_cast<float>(TextureIndex::Water)) < 0.25f;
            // uv.z == -1 is a color-only material (VoxelVertex contract). On
            // the water pass that is a PROJECT fluid (class "fluid", its own
            // color, no texture); on the solid pass it is a solid dynamic
            // block or the color-only clay builtin. The two passes must never
            // mix: water pass accepts only water-material or color-only
            // vertices, the solid pass rejects water-material vertices.
            if (waterPass) {
                if (!waterMaterial && !(vertex.uv.z < 0.0f)) return false;
            } else if (waterMaterial) {
                return false;
            }
        }
        return true;
    };
    if (!valid_vertices(mesh.meshVertices, false) || !valid_vertices(mesh.waterMeshVertices, true)) {
        std::cerr << "Rejected invalid chunk mesh at " << snapshot.id.coord.x << ',' << snapshot.id.coord.z << '\n';
        result.valid = false;
        result.mesh = {};
        return result;
    }
    // A mesh is a disposable snapshot. Never publish it if its voxel source
    // changed while it was being built.
    mesh.meshVersion = snapshot.revision;
    return result;
}

glm::vec4 VoxelMesher::resolve_state_material(const RuntimeBlockInfo& info,
                                              int stateIndex,
                                              const glm::vec3& normal) {
    const RuntimeBlockInfo::RuntimeBlockState* state = nullptr;
    if (!info.states.empty()) {
        // With states, index addresses states directly and 0 is the default
        // state; out-of-range clamps to the default (never the base material
        // of a block that declares states).
        const int index = stateIndex >= 0 &&
                                  stateIndex < static_cast<int>(info.states.size())
                              ? stateIndex
                              : 0;
        state = &info.states[static_cast<std::size_t>(index)];
    }
    const glm::vec4* base = state ? &state->color : &info.color;
    const glm::vec4* top = state ? &state->faceTop : &info.faceTop;
    const glm::vec4* bottom = state ? &state->faceBottom : &info.faceBottom;
    const glm::vec4* side = state ? &state->faceSide : &info.faceSide;
    const bool topSet = state ? state->faceTopSet : info.faceTopSet;
    const bool bottomSet = state ? state->faceBottomSet : info.faceBottomSet;
    const bool sideSet = state ? state->faceSideSet : info.faceSideSet;
    if (normal.y > 0.5f && topSet) return *top;
    if (normal.y < -0.5f && bottomSet) return *bottom;
    if (std::abs(normal.y) < 0.5f && sideSet) return *side;
    return *base;
}
