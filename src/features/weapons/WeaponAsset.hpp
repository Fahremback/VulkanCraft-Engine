#pragma once

#include <string>
#include <glm/glm.hpp>
#include "../../engine/core/uuid/UUID.hpp"
#include "../../engine/core/reflection/Reflection.hpp"

namespace Engine {

enum class FireMode {
    Single,
    Burst,
    Automatic
};

class WeaponAsset {
public:
    UUID id;
    std::string name{ "Rifle" };
    FireMode fireMode{ FireMode::Automatic };

    float rateOfFire{ 600.0f }; // rounds per minute
    float baseDamage{ 35.0f };
    float bulletVelocity{ 800.0f }; // m/s
    float recoilKickback{ 0.15f };
    int magazineCapacity{ 30 };
    float reloadTimeSeconds{ 2.2f };

    UUID meshAssetID;
    UUID fireSoundAssetID;
    UUID reloadSoundAssetID;
};

REFLECT_BEGIN(WeaponAsset)
    REFLECT_FIELD_RANGE(baseDamage, FieldType::Float, "Base Damage", 1.0f, 500.0f)
    REFLECT_FIELD_RANGE(rateOfFire, FieldType::Float, "Rate of Fire (RPM)", 60.0f, 2000.0f)
    REFLECT_FIELD_RANGE(bulletVelocity, FieldType::Float, "Muzzle Velocity (m/s)", 10.0f, 3000.0f)
    REFLECT_FIELD_RANGE(recoilKickback, FieldType::Float, "Recoil Impulse", 0.0f, 2.0f)
    REFLECT_FIELD_RANGE(reloadTimeSeconds, FieldType::Float, "Reload Time (s)", 0.1f, 10.0f)
REFLECT_END(WeaponAsset)

} // namespace Engine
