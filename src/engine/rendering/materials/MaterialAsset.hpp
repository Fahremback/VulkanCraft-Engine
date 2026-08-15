#pragma once

#include <glm/glm.hpp>
#include <string>
#include "../../core/uuid/UUID.hpp"
#include "../../core/reflection/Reflection.hpp"

namespace Engine {

struct PhysicsMaterial {
    float friction{ 0.6f };
    float restitution{ 0.1f };
    float density{ 1500.0f }; // kg/m3
    float hardness{ 5.0f };
    std::string impactSoundEvent{ "event:/physics/impact_stone" };
    std::string footstepSoundEvent{ "event:/physics/footstep_stone" };
};

class MaterialAsset {
public:
    UUID id;
    std::string name{ "PBR Material" };

    glm::vec4 baseColor{ 1.0f, 1.0f, 1.0f, 1.0f };
    float roughness{ 0.5f };
    float metallic{ 0.0f };
    glm::vec3 emission{ 0.0f, 0.0f, 0.0f };

    UUID albedoTextureID;
    UUID normalMapTextureID;
    UUID roughnessTextureID;

    PhysicsMaterial physicsMaterial;
};

REFLECT_BEGIN(MaterialAsset)
    REFLECT_FIELD(baseColor, FieldType::Color, "Base Color")
    REFLECT_FIELD_RANGE(roughness, FieldType::Float, "Roughness", 0.0f, 1.0f)
    REFLECT_FIELD_RANGE(metallic, FieldType::Float, "Metallic", 0.0f, 1.0f)
    REFLECT_FIELD(emission, FieldType::Vec3, "Emission Color")
REFLECT_END(MaterialAsset)

} // namespace Engine
