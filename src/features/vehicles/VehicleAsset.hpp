#pragma once

#include <string>
#include <glm/glm.hpp>
#include "../../engine/core/uuid/UUID.hpp"
#include "../../engine/core/reflection/Reflection.hpp"

namespace Engine {

class VehicleAsset {
public:
    UUID id;
    std::string name{ "Offroad Vehicle" };

    float engineHorsepower{ 450.0f };
    float maxRPM{ 6500.0f };
    float vehicleMass{ 1800.0f }; // kg
    float brakeForce{ 3500.0f };
    float suspensionStiffness{ 35000.0f };
    float suspensionDamping{ 4500.0f };
    float wheelRadius{ 0.45f };

    UUID chassisMeshID;
    UUID engineSoundID;
};

REFLECT_BEGIN(VehicleAsset)
    REFLECT_FIELD_RANGE(engineHorsepower, FieldType::Float, "Horsepower", 10.0f, 3000.0f)
    REFLECT_FIELD_RANGE(vehicleMass, FieldType::Float, "Mass (kg)", 100.0f, 50000.0f)
    REFLECT_FIELD_RANGE(wheelRadius, FieldType::Float, "Wheel Radius (m)", 0.1f, 3.0f)
REFLECT_END(VehicleAsset)

} // namespace Engine
