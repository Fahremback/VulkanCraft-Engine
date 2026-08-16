#include "MobRenderer.hpp"

#include "Voxel.hpp"
#include "../../sdk/RegistryJson.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>

struct MobPushData {
    glm::mat4 mvp;
    glm::vec4 cameraPos;
    glm::vec4 sunDirection;
    glm::vec4 sunColor;
    glm::vec4 environment;
};

LimbMesh MobRenderer::build_box_limb(VkDevice device, VmaAllocator allocator,
                                   int texU, int texV, int texW, int texH,
                                   float minX, float minY, float minZ,
                                   float dx, float dy, float dz,
                                   glm::vec3 pivot, float layerIdx, glm::vec4 baseColor) {
    LimbMesh mesh;
    mesh.pivotOffset = pivot;

    std::vector<VoxelVertex> vertices;

    // Algoritmo Oficial de UV Mappings de Caixas do Minecraft (ModelBox.java)
    float u0 = (float)texU / (float)texW;
    float v0 = (float)texV / (float)texH;
    float u1 = (float)(texU + dz) / (float)texW;
    float u2 = (float)(texU + dz + dx) / (float)texW;
    float u3 = (float)(texU + dz + dx + dx) / (float)texW;
    float u4 = (float)(texU + dz + dx + dz + dx) / (float)texW;

    float v1 = (float)(texV + dz) / (float)texH;
    float v2 = (float)(texV + dz + dy) / (float)texH;

    glm::vec3 p0(minX, minY, minZ + dz);
    glm::vec3 p1(minX + dx, minY, minZ + dz);
    glm::vec3 p2(minX + dx, minY + dy, minZ + dz);
    glm::vec3 p3(minX, minY + dy, minZ + dz);

    glm::vec3 p4(minX + dx, minY, minZ);
    glm::vec3 p5(minX, minY, minZ);
    glm::vec3 p6(minX, minY + dy, minZ);
    glm::vec3 p7(minX + dx, minY + dy, minZ);

    auto add_face = [&](glm::vec3 va, glm::vec3 vb, glm::vec3 vc, glm::vec3 vd,
                        float uA, float vA, float uB, float vB, glm::vec3 norm) {
        vertices.push_back({ va - pivot, norm, baseColor, glm::vec3(uA, vB, layerIdx) });
        vertices.push_back({ vb - pivot, norm, baseColor, glm::vec3(uB, vB, layerIdx) });
        vertices.push_back({ vc - pivot, norm, baseColor, glm::vec3(uB, vA, layerIdx) });
        vertices.push_back({ va - pivot, norm, baseColor, glm::vec3(uA, vB, layerIdx) });
        vertices.push_back({ vc - pivot, norm, baseColor, glm::vec3(uB, vA, layerIdx) });
        vertices.push_back({ vd - pivot, norm, baseColor, glm::vec3(uA, vA, layerIdx) });
    };

    // Front (Z+)
    add_face(p0, p1, p2, p3, u1, v1, u2, v2, glm::vec3(0, 0, 1));
    // Back (Z-)
    add_face(p4, p5, p6, p7, u3, v1, u4, v2, glm::vec3(0, 0, -1));
    // Right (X+)
    add_face(p1, p4, p7, p2, u2, v1, u3, v2, glm::vec3(1, 0, 0));
    // Left (X-)
    add_face(p5, p0, p3, p6, u0, v1, u1, v2, glm::vec3(-1, 0, 0));
    // Top (Y+)
    add_face(p3, p2, p7, p6, u1, u0, u2, v1, glm::vec3(0, 1, 0));
    // Bottom (Y-)
    add_face(p5, p4, p1, p0, u2, u0, u3, v1, glm::vec3(0, -1, 0));

    mesh.vertexCount = static_cast<uint32_t>(vertices.size());
    VkDeviceSize bufferSize = mesh.vertexCount * sizeof(VoxelVertex);

    VkBufferCreateInfo bufferInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = bufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

    VmaAllocationCreateInfo allocInfo = {};
    allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;

    vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &mesh.buffer.buffer, &mesh.buffer.allocation, nullptr);

    void* data;
    vmaMapMemory(allocator, mesh.buffer.allocation, &data);
    memcpy(data, vertices.data(), bufferSize);
    vmaUnmapMemory(allocator, mesh.buffer.allocation);

    return mesh;
}

void MobRenderer::build_mob_limb_meshes(VkDevice device, VmaAllocator allocator) {
    // 0: Zombie
    glm::vec4 zCol(0.25f, 0.6f, 0.35f, 1.0f);
    sharedLimbMeshes_[0][0] = build_box_limb(device, allocator, 0, 0, 64, 64, -0.25f, 1.25f, -0.25f, 0.5f, 0.5f, 0.5f, glm::vec3(0, 1.25f, 0), 2.0f, zCol);
    sharedLimbMeshes_[0][1] = build_box_limb(device, allocator, 16, 16, 64, 64, -0.25f, 0.5f, -0.125f, 0.5f, 0.75f, 0.25f, glm::vec3(0, 1.25f, 0), 2.0f, zCol);
    sharedLimbMeshes_[0][2] = build_box_limb(device, allocator, 40, 16, 64, 64, -0.45f, 0.5f, -0.125f, 0.2f, 0.75f, 0.25f, glm::vec3(-0.35f, 1.2f, 0), 2.0f, zCol);
    sharedLimbMeshes_[0][3] = build_box_limb(device, allocator, 40, 16, 64, 64, 0.25f, 0.5f, -0.125f, 0.2f, 0.75f, 0.25f, glm::vec3(0.35f, 1.2f, 0), 2.0f, zCol);
    sharedLimbMeshes_[0][4] = build_box_limb(device, allocator, 0, 16, 64, 64, -0.2f, 0.0f, -0.125f, 0.2f, 0.5f, 0.25f, glm::vec3(-0.1f, 0.5f, 0), 2.0f, zCol);
    sharedLimbMeshes_[0][5] = build_box_limb(device, allocator, 0, 16, 64, 64, 0.0f, 0.0f, -0.125f, 0.2f, 0.5f, 0.25f, glm::vec3(0.1f, 0.5f, 0), 2.0f, zCol);

    // 1: Skeleton
    glm::vec4 sCol(0.9f, 0.9f, 0.9f, 1.0f);
    sharedLimbMeshes_[1][0] = build_box_limb(device, allocator, 0, 0, 64, 32, -0.25f, 1.25f, -0.25f, 0.5f, 0.5f, 0.5f, glm::vec3(0, 1.25f, 0), 3.0f, sCol);
    sharedLimbMeshes_[1][1] = build_box_limb(device, allocator, 16, 16, 64, 32, -0.25f, 0.5f, -0.125f, 0.5f, 0.75f, 0.25f, glm::vec3(0, 1.25f, 0), 3.0f, sCol);
    sharedLimbMeshes_[1][2] = build_box_limb(device, allocator, 40, 16, 64, 32, -0.4f, 0.5f, -0.06f, 0.12f, 0.75f, 0.12f, glm::vec3(-0.3f, 1.2f, 0), 3.0f, sCol);
    sharedLimbMeshes_[1][3] = build_box_limb(device, allocator, 40, 16, 64, 32, 0.28f, 0.5f, -0.06f, 0.12f, 0.75f, 0.12f, glm::vec3(0.3f, 1.2f, 0), 3.0f, sCol);
    sharedLimbMeshes_[1][4] = build_box_limb(device, allocator, 0, 16, 64, 32, -0.18f, 0.0f, -0.06f, 0.12f, 0.5f, 0.12f, glm::vec3(-0.1f, 0.5f, 0), 3.0f, sCol);
    sharedLimbMeshes_[1][5] = build_box_limb(device, allocator, 0, 16, 64, 32, 0.06f, 0.0f, -0.06f, 0.12f, 0.5f, 0.12f, glm::vec3(0.1f, 0.5f, 0), 3.0f, sCol);

    // 2: Creeper
    glm::vec4 cCol(0.2f, 0.85f, 0.3f, 1.0f);
    sharedLimbMeshes_[2][0] = build_box_limb(device, allocator, 0, 0, 64, 32, -0.25f, 1.0f, -0.25f, 0.5f, 0.5f, 0.5f, glm::vec3(0, 1.0f, 0), 2.0f, cCol);
    sharedLimbMeshes_[2][1] = build_box_limb(device, allocator, 16, 16, 64, 32, -0.25f, 0.35f, -0.125f, 0.5f, 0.65f, 0.25f, glm::vec3(0, 1.0f, 0), 2.0f, cCol);
    sharedLimbMeshes_[2][2] = build_box_limb(device, allocator, 0, 16, 64, 32, -0.25f, 0.0f, -0.35f, 0.25f, 0.35f, 0.25f, glm::vec3(-0.125f, 0.35f, -0.2f), 2.0f, cCol);
    sharedLimbMeshes_[2][3] = build_box_limb(device, allocator, 0, 16, 64, 32, 0.0f, 0.0f, -0.35f, 0.25f, 0.35f, 0.25f, glm::vec3(0.125f, 0.35f, -0.2f), 2.0f, cCol);
    sharedLimbMeshes_[2][4] = build_box_limb(device, allocator, 0, 16, 64, 32, -0.25f, 0.0f, 0.1f, 0.25f, 0.35f, 0.25f, glm::vec3(-0.125f, 0.35f, 0.2f), 2.0f, cCol);
    sharedLimbMeshes_[2][5] = build_box_limb(device, allocator, 0, 16, 64, 32, 0.0f, 0.0f, 0.1f, 0.25f, 0.35f, 0.25f, glm::vec3(0.125f, 0.35f, 0.2f), 2.0f, cCol);

    // 3: Cow (Quadrupede)
    glm::vec4 cowCol(0.5f, 0.35f, 0.25f, 1.0f);
    sharedLimbMeshes_[3][0] = build_box_limb(device, allocator, 0, 0, 64, 32, -0.25f, 0.8f, -0.6f, 0.5f, 0.5f, 0.5f, glm::vec3(0, 0.8f, -0.35f), 2.0f, cowCol);
    sharedLimbMeshes_[3][1] = build_box_limb(device, allocator, 18, 4, 64, 32, -0.35f, 0.5f, -0.35f, 0.7f, 0.5f, 0.9f, glm::vec3(0, 0.75f, 0), 2.0f, cowCol);
    sharedLimbMeshes_[3][2] = build_box_limb(device, allocator, 0, 16, 64, 32, -0.3f, 0.0f, -0.3f, 0.25f, 0.5f, 0.25f, glm::vec3(-0.2f, 0.5f, -0.2f), 2.0f, cowCol);
    sharedLimbMeshes_[3][3] = build_box_limb(device, allocator, 0, 16, 64, 32, 0.05f, 0.0f, -0.3f, 0.25f, 0.5f, 0.25f, glm::vec3(0.2f, 0.5f, -0.2f), 2.0f, cowCol);
    sharedLimbMeshes_[3][4] = build_box_limb(device, allocator, 0, 16, 64, 32, -0.3f, 0.0f, 0.1f, 0.25f, 0.5f, 0.25f, glm::vec3(-0.2f, 0.5f, 0.2f), 2.0f, cowCol);
    sharedLimbMeshes_[3][5] = build_box_limb(device, allocator, 0, 16, 64, 32, 0.05f, 0.0f, 0.1f, 0.25f, 0.5f, 0.25f, glm::vec3(0.2f, 0.5f, 0.2f), 2.0f, cowCol);

    // 4: Pig
    glm::vec4 pigCol(0.95f, 0.65f, 0.7f, 1.0f);
    sharedLimbMeshes_[4][0] = build_box_limb(device, allocator, 0, 0, 64, 32, -0.25f, 0.5f, -0.5f, 0.5f, 0.5f, 0.5f, glm::vec3(0, 0.5f, -0.25f), 2.0f, pigCol);
    sharedLimbMeshes_[4][1] = build_box_limb(device, allocator, 28, 8, 64, 32, -0.3f, 0.35f, -0.35f, 0.6f, 0.45f, 0.8f, glm::vec3(0, 0.5f, 0), 2.0f, pigCol);
    sharedLimbMeshes_[4][2] = build_box_limb(device, allocator, 0, 16, 64, 32, -0.25f, 0.0f, -0.3f, 0.2f, 0.35f, 0.2f, glm::vec3(-0.15f, 0.35f, -0.2f), 2.0f, pigCol);
    sharedLimbMeshes_[4][3] = build_box_limb(device, allocator, 0, 16, 64, 32, 0.05f, 0.0f, -0.3f, 0.2f, 0.35f, 0.2f, glm::vec3(0.15f, 0.35f, -0.2f), 2.0f, pigCol);
    sharedLimbMeshes_[4][4] = build_box_limb(device, allocator, 0, 16, 64, 32, -0.25f, 0.0f, 0.1f, 0.2f, 0.35f, 0.2f, glm::vec3(-0.15f, 0.35f, 0.2f), 2.0f, pigCol);
    sharedLimbMeshes_[4][5] = build_box_limb(device, allocator, 0, 16, 64, 32, 0.05f, 0.0f, 0.1f, 0.2f, 0.35f, 0.2f, glm::vec3(0.15f, 0.35f, 0.2f), 2.0f, pigCol);

    // 5: Sheep
    glm::vec4 shCol(0.95f, 0.95f, 0.95f, 1.0f);
    sharedLimbMeshes_[5][0] = build_box_limb(device, allocator, 0, 0, 64, 32, -0.22f, 0.7f, -0.5f, 0.44f, 0.44f, 0.44f, glm::vec3(0, 0.7f, -0.28f), 2.0f, shCol);
    sharedLimbMeshes_[5][1] = build_box_limb(device, allocator, 28, 8, 64, 32, -0.32f, 0.45f, -0.35f, 0.64f, 0.5f, 0.85f, glm::vec3(0, 0.7f, 0), 2.0f, shCol);
    sharedLimbMeshes_[5][2] = build_box_limb(device, allocator, 0, 16, 64, 32, -0.25f, 0.0f, -0.3f, 0.2f, 0.45f, 0.2f, glm::vec3(-0.15f, 0.45f, -0.2f), 2.0f, shCol);
    sharedLimbMeshes_[5][3] = build_box_limb(device, allocator, 0, 16, 64, 32, 0.05f, 0.0f, -0.3f, 0.2f, 0.45f, 0.2f, glm::vec3(0.15f, 0.45f, -0.2f), 2.0f, shCol);
    sharedLimbMeshes_[5][4] = build_box_limb(device, allocator, 0, 16, 64, 32, -0.25f, 0.0f, 0.1f, 0.2f, 0.45f, 0.2f, glm::vec3(-0.15f, 0.45f, 0.2f), 2.0f, shCol);
    sharedLimbMeshes_[5][5] = build_box_limb(device, allocator, 0, 16, 64, 32, 0.05f, 0.0f, 0.1f, 0.2f, 0.45f, 0.2f, glm::vec3(0.15f, 0.45f, 0.2f), 2.0f, shCol);
}

void MobRenderer::init(VkDevice device, VmaAllocator allocator) {
    build_mob_limb_meshes(device, allocator);
    std::cout << "[MobRenderer] Entity-layer mob rendering initialized (legacy MobManager track removed)\n";
}

// Parses the renderer-relevant fields of a mob component blob (JSON MobSpec).
// The behavior owns the document; the renderer only reads presentation state.
bool parse_mob_draw_state(const std::string& blob, uint32_t& typeIndexOut,
                          float& yawOut, float& walkOut, float& fuseOut) {
    engine::sdk::JsonValue root;
    std::string error;
    if (!engine::sdk::json_parse(blob, root, error) || !root.is_object()) {
        return false;
    }
    typeIndexOut = static_cast<uint32_t>(
        std::lround(engine::sdk::json_number(root, "typeIndex", 3.0)));
    yawOut = static_cast<float>(engine::sdk::json_number(root, "yaw", 0.0));
    walkOut = static_cast<float>(
        engine::sdk::json_number(root, "walkAnimProgress", 0.0));
    fuseOut = static_cast<float>(engine::sdk::json_number(root, "fuseTimer", 0.0));
    return true;
}

void MobRenderer::draw(const engine::entity::IEntityWorld& entities, VkCommandBuffer cmd, VkPipelineLayout pipelineLayout, const glm::mat4& viewProj,
                           const Frustum& frustum, const glm::vec3& cameraPosition,
                           const glm::vec3& sunDirection, const glm::vec3& sunColor,
                           const glm::vec4& environment) {
    entities.for_each_entity([&](engine::entity::EntityId id) {
        engine::entity::ComponentData mob;
        if (!entities.get_component(id, engine::entity::kMobComponentType, mob)) {
            return;
        }
        engine::entity::Health health;
        if (!entities.get_health(id, health) || health.value <= 0.0f) return;
        engine::entity::Position position;
        if (!entities.get_position(id, position)) return;

        uint32_t typeIdx = 3;
        float yaw = 0.0f;
        float walkAnimProgress = 0.0f;
        float creeperFuseTimer = 0.0f;
        if (!parse_mob_draw_state(mob.blob, typeIdx, yaw, walkAnimProgress,
                                  creeperFuseTimer)) {
            return;
        }
        if (typeIdx > 5) return;

        const glm::vec3 mobPosition(position.x, position.y, position.z);
        if (!frustum.is_box_visible(mobPosition - glm::vec3(1.0f), mobPosition + glm::vec3(1.0f, 2.0f, 1.0f))) {
            return;
        }

        glm::mat4 baseModel = glm::translate(glm::mat4(1.0f), mobPosition);
        baseModel = glm::rotate(baseModel, yaw, glm::vec3(0, 1, 0));

        // Efeito de inchaço do Creeper ao explodir
        if (typeIdx == 2 && creeperFuseTimer > 0.0f) {
            float swell = 1.0f + std::sin(creeperFuseTimer * 20.0f) * 0.2f;
            baseModel = glm::scale(baseModel, glm::vec3(swell, swell, swell));
        }

        // Matrizes de Articulação de Membros (Física e Animação de Caminhada)
        float swing = std::sin(walkAnimProgress) * 0.6f;
        float swingOpp = -swing;

        std::array<glm::mat4, 6> limbTransforms;
        limbTransforms[0] = baseModel; // Head
        limbTransforms[1] = baseModel; // Body

        if (typeIdx == 0) {
            // Braços de Zumbi sempre levantados para a frente
            limbTransforms[2] = glm::rotate(baseModel, -1.4f + swing * 0.2f, glm::vec3(1, 0, 0));
            limbTransforms[3] = glm::rotate(baseModel, -1.4f + swingOpp * 0.2f, glm::vec3(1, 0, 0));
            limbTransforms[4] = glm::rotate(baseModel, swing, glm::vec3(1, 0, 0));
            limbTransforms[5] = glm::rotate(baseModel, swingOpp, glm::vec3(1, 0, 0));
        } else if (typeIdx == 1) {
            limbTransforms[2] = glm::rotate(baseModel, swingOpp, glm::vec3(1, 0, 0));
            limbTransforms[3] = glm::rotate(baseModel, swing, glm::vec3(1, 0, 0));
            limbTransforms[4] = glm::rotate(baseModel, swing, glm::vec3(1, 0, 0));
            limbTransforms[5] = glm::rotate(baseModel, swingOpp, glm::vec3(1, 0, 0));
        } else {
            // Quadrupede (Vaca, Porco, Ovelha, Creeper)
            limbTransforms[2] = glm::rotate(baseModel, swing, glm::vec3(1, 0, 0));
            limbTransforms[3] = glm::rotate(baseModel, swingOpp, glm::vec3(1, 0, 0));
            limbTransforms[4] = glm::rotate(baseModel, swingOpp, glm::vec3(1, 0, 0));
            limbTransforms[5] = glm::rotate(baseModel, swing, glm::vec3(1, 0, 0));
        }

        // Desenhar os 6 membros articulados da malha 3D
        for (int i = 0; i < 6; ++i) {
            const auto& mesh = sharedLimbMeshes_[typeIdx][i];
            if (mesh.vertexCount == 0 || mesh.buffer.buffer == VK_NULL_HANDLE) continue;

            glm::mat4 mvp = viewProj * limbTransforms[i];

            MobPushData pushData;
            pushData.mvp = mvp;
            pushData.cameraPos = glm::vec4(cameraPosition, 1.0f);
            pushData.sunDirection = glm::vec4(sunDirection, 0.0f);
            pushData.sunColor = glm::vec4(sunColor, 1.0f);
            pushData.environment = environment;

            vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(MobPushData), &pushData);

            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.buffer.buffer, &offset);
            vkCmdDraw(cmd, mesh.vertexCount, 1, 0, 0);
        }
    });
}

void MobRenderer::cleanup(VkDevice device, VmaAllocator allocator) {
    for (int t = 0; t < 6; ++t) {
        for (int i = 0; i < 6; ++i) {
            if (sharedLimbMeshes_[t][i].buffer.buffer != VK_NULL_HANDLE) {
                vmaDestroyBuffer(allocator, sharedLimbMeshes_[t][i].buffer.buffer, sharedLimbMeshes_[t][i].buffer.allocation);
            }
        }
    }
}
