#include "FbxImporter.hpp"

#include "ufbx.h"

#include <glm/gtc/quaternion.hpp>

#include <cstring>
#include <unordered_map>

namespace Engine {

namespace {

glm::vec3 to_vec3(ufbx_vec3 value) {
    return { static_cast<float>(value.x), static_cast<float>(value.y), static_cast<float>(value.z) };
}

glm::vec2 to_vec2(ufbx_vec2 value) {
    return { static_cast<float>(value.x), static_cast<float>(value.y) };
}

glm::mat4 to_mat4(const ufbx_matrix& value) {
    // ufbx_matrix é row-major (4 vetores de 3 + col 4) — transposto para glm.
    glm::mat4 result(1.0f);
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            result[col][row] = static_cast<float>(value.cols[col].v[row]);
        }
    }
    for (int row = 0; row < 3; ++row) {
        result[3][row] = static_cast<float>(value.cols[3].v[row]);
    }
    return result;
}

} // namespace

bool import_fbx_geometry(std::span<const uint8_t> bytes, GltfGeometryResult& out, std::string* error) {
    if (bytes.empty()) {
        if (error) *error = "empty FBX input";
        return false;
    }

    ufbx_load_opts options = {};
    options.generate_missing_normals = true;

    ufbx_error loadError;
    ufbx_scene* scene = ufbx_load_memory(bytes.data(), bytes.size(), &options, &loadError);
    if (!scene) {
        if (error) *error = loadError.description.data;
        return false;
    }

    bool anyPrimitive = false;

    for (size_t meshIndex = 0; meshIndex < scene->meshes.count; ++meshIndex) {
        const ufbx_mesh* mesh = scene->meshes.data[meshIndex];
        if (!mesh || mesh->num_vertices == 0 || mesh->num_indices == 0) continue;

        GltfMeshPrimitive primitive;
        primitive.positions.reserve(mesh->num_vertices);
        for (size_t i = 0; i < mesh->num_vertices; ++i) {
            primitive.positions.push_back(to_vec3(mesh->vertex_position[i]));
        }
        primitive.normals.reserve(mesh->num_vertices);
        for (size_t i = 0; i < mesh->num_vertices; ++i) {
            primitive.normals.push_back(to_vec3(mesh->vertex_normal[i]));
        }
        const bool hasUvs = mesh->vertex_uv.indices.count > 0;
        if (hasUvs) {
            primitive.uvs.reserve(mesh->num_vertices);
            for (size_t i = 0; i < mesh->num_vertices; ++i) {
                primitive.uvs.push_back(to_vec2(mesh->vertex_uv[i]));
            }
        }

        // Triangulação por leque por face (ufbx carrega polígonos).
        for (size_t faceIndex = 0; faceIndex < mesh->faces.count; ++faceIndex) {
            const ufbx_face face = mesh->faces.data[faceIndex];
            if (face.num_indices < 3) continue;
            const uint32_t first = mesh->vertex_indices.data[face.index_begin];
            for (uint32_t offset = 1; offset + 1 < face.num_indices; ++offset) {
                primitive.indices.push_back(first);
                primitive.indices.push_back(mesh->vertex_indices.data[face.index_begin + offset]);
                primitive.indices.push_back(mesh->vertex_indices.data[face.index_begin + offset + 1]);
            }
        }
        if (primitive.indices.empty()) continue;
        primitive.indexed = true;

        // Skinning: clusters de cada skin deformer → JOINTS_0/WEIGHTS_0.
        std::unordered_map<const ufbx_node*, int32_t> nodeToBone;
        for (size_t d = 0; d < mesh->skin_deformers.count; ++d) {
            const ufbx_skin_deformer* deformer = mesh->skin_deformers.data[d];
            if (!deformer || deformer->clusters.count == 0) continue;

            std::vector<const ufbx_node*> bones;
            std::vector<glm::mat4> inverseBinds;
            for (size_t c = 0; c < deformer->clusters.count; ++c) {
                const ufbx_skin_cluster* cluster = deformer->clusters.data[c];
                if (!cluster->bone_node) continue;
                if (!nodeToBone.contains(cluster->bone_node)) {
                    nodeToBone[cluster->bone_node] = static_cast<int32_t>(bones.size());
                    bones.push_back(cluster->bone_node);
                    inverseBinds.push_back(to_mat4(cluster->geometry_to_bone));
                }
            }
            if (bones.empty()) continue;

            const size_t vertexCount = primitive.positions.size();
            std::vector<glm::uvec4> jointStorage(vertexCount, glm::uvec4(0));
            std::vector<glm::vec4> weightStorage(vertexCount, glm::vec4(0.0f));
            for (size_t c = 0; c < deformer->clusters.count; ++c) {
                const ufbx_skin_cluster* cluster = deformer->clusters.data[c];
                const auto found = nodeToBone.find(cluster->bone_node);
                if (found == nodeToBone.end()) continue;
                const uint32_t boneIndex = static_cast<uint32_t>(found->second);
                for (size_t w = 0; w < cluster->num_weights; ++w) {
                    const uint32_t vertexIndex = cluster->vertices.data[w];
                    if (vertexIndex >= vertexCount) continue;
                    const float weight = static_cast<float>(cluster->weights.data[w]);
                    for (int slot = 0; slot < 4; ++slot) {
                        if (weightStorage[vertexIndex][slot] == 0.0f && weight > 0.0f) {
                            jointStorage[vertexIndex][slot] = boneIndex;
                            weightStorage[vertexIndex][slot] = weight;
                            break;
                        }
                    }
                }
            }
            for (size_t v = 0; v < vertexCount; ++v) {
                const float total = weightStorage[v].x + weightStorage[v].y +
                                    weightStorage[v].z + weightStorage[v].w;
                if (total > 0.0f) {
                    primitive.joints.push_back(jointStorage[v]);
                    primitive.weights.push_back(weightStorage[v] / total);
                }
            }
            if (primitive.joints.size() == vertexCount) {
                GltfGeometrySkin geometrySkin;
                geometrySkin.name = mesh->name.data;
                for (size_t b = 0; b < bones.size(); ++b) {
                    geometrySkin.jointNames.push_back(bones[b]->name.data);
                    geometrySkin.inverseBindMatrices.push_back(inverseBinds[b]);
                    const ufbx_node* parent = bones[b]->parent;
                    geometrySkin.jointParents.push_back(
                        parent && nodeToBone.contains(parent) ? nodeToBone[parent] : -1);
                }
                out.skins.push_back(std::move(geometrySkin));
            } else {
                primitive.joints.clear();
                primitive.weights.clear();
            }
        }

        out.primitives.push_back(std::move(primitive));
        anyPrimitive = true;
    }

    if (!anyPrimitive) {
        if (error) *error = "FBX contains no usable meshes";
        ufbx_free_scene(scene);
        return false;
    }

    for (const GltfMeshPrimitive& primitive : out.primitives) {
        out.vertexCount += static_cast<uint32_t>(primitive.positions.size());
        out.indexCount += static_cast<uint32_t>(primitive.indices.size());
    }
    out.success = true;
    ufbx_free_scene(scene);
    return true;
}

} // namespace Engine
