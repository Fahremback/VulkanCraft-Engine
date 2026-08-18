#pragma once

// Public vehicle asset contract (FALTANTES §17 item 2 / META section 20):
// the vehicle runtime is assembled from COMPOSABLE PUBLIC COMPONENTS —
// chassis, wheels and drivetrain — and serialized as a data-driven
// VehicleAsset (JSON, versioned, all-or-nothing). A project builds any
// wheeled vehicle (car, motorcycle, truck) by composing components and
// loading them from JSON; the runtime is created through
// IGameplayRuntime::create_vehicle_from_asset.
//
// This header is self-contained (glm only). load_from_json / to_json /
// validate are implemented by the SDK adapter (src/engine/sdk/VehicleAsset.cpp).
// The asset is PURE DATA — it never touches physics or rendering.

#include "IVehicleProvider.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace engine {
namespace vehicles {

// Vehicle kind (FALTANTES §17 item 3): selects the Jolt controller family.
// Wheeled = car (WheeledVehicleController); Motorcycle = two-wheeler with the
// lean-spring balance controller; Tracked = tank with two tracks steered by
// differential track ratios. The runtime/backend maps the kind onto the
// solver; the project composes any wheeled/steered vehicle from the
// components below.
enum class VehicleKind : std::uint8_t { Wheeled, Motorcycle, Tracked };

// Propulsion module (FALTANTES §17 item 3): a force generator attached to the
// chassis at a local position. Wing produces lift from forward speed,
// Thruster pushes along `axis` scaled by throttle, Buoyancy floats the
// chassis when its local position goes below `waterLevel`. Pure force
// modules — backend-independent (applied through the physics seam).
enum class PropulsionKind : std::uint8_t { Wing, Thruster, Buoyancy };

struct PropulsionModule {
    PropulsionKind kind{ PropulsionKind::Thruster };
    glm::vec3 localPosition{ 0.0f };
    glm::vec3 axis{ 0.0f, 1.0f, 0.0f };  // thrust direction / wing lift normal
    float maxForce{ 1000.0f };            // thruster force (N)
    float area{ 4.0f };                   // wing area (m^2)
    float liftCoefficient{ 0.8f };        // wing lift coefficient
    float fluidDensity{ 1000.0f };        // buoyancy fluid density (kg/m^3)
    float waterLevel{ 0.0f };             // buoyancy water surface Y
};

enum class ChassisShape : std::uint8_t { Box, Sphere, Capsule };

// Chassis component: the body the vehicle is assembled on. halfExtents is the
// box extents; radius/halfHeight describe sphere/capsule bodies (the capsule
// axis is local Y). mass > 0, friction/restitution >= 0.
struct ChassisComponent {
    ChassisShape shape{ ChassisShape::Box };
    glm::vec3 halfExtents{ 0.9f, 0.35f, 0.56f };
    float radius{ 0.5f };
    float halfHeight{ 0.65f };
    float mass{ 1200.0f };
    float friction{ 0.55f };
    float restitution{ 0.05f };
};

// Wheel component: one wheel of the vehicle (suspension + tire). springStrength
// is the suspension stiffness k (N/m) and damperStrength the damping c (N*s/m)
// — the same coefficients the physics layer feeds Jolt's suspension spring.
struct WheelComponent {
    glm::vec3 localPosition{ 0.0f };
    float radius{ 0.36f };
    float suspensionRestLength{ 0.45f };
    float suspensionTravel{ 0.18f };
    float springStrength{ 26000.0f };
    float damperStrength{ 3200.0f };
    float tireGrip{ 1.35f };
    float maxDriveForce{ 4200.0f };
    float maxBrakeForce{ 6000.0f };
    float maxSteerAngle{ 0.55f };
    bool steering{ false };
    bool driven{ true };
};

// Drivetrain component: engine + automatic transmission + differential.
// engineMaxTorque == 0 derives the torque so first gear delivers the wheels'
// maxDriveForce (the legacy model's acceleration); an explicit value overrides.
struct DrivetrainComponent {
    float engineMaxTorque{ 0.0f };
    float engineMinRPM{ 1000.0f };
    float engineMaxRPM{ 6000.0f };
    float differentialRatio{ 3.42f };
    std::vector<float> gearRatios{ 2.66f, 1.78f, 1.3f, 1.0f, 0.74f };
};

// Vehicle seat (FALTANTES §17 item 8): an occupant position on the vehicle.
// `localPosition` is the seat point in chassis-local space (the ride pose is
// derived from it every update); `exitOffset` is the WORLD-space offset from
// the chassis origin where the occupant spawns on exit (entry/exit point).
struct VehicleSeat {
    std::string name;
    glm::vec3 localPosition{ 0.0f };
    glm::vec3 exitOffset{ 0.0f, 1.2f, 1.0f };
};

// Fuel system (FALTANTES §17 item 7): an internal-combustion tank the engine
// burns by throttle. capacity == 0 disables the system (no fuel constraint —
// the legacy behavior); with a capacity, the engine burns burnPerSecond
// liters/second at full throttle (scaled by |throttle|) plus an idle burn,
// and CUTS OUT below minLevelToRun * capacity (the vehicle coasts to a stop).
struct FuelSystem {
    float capacity{ 0.0f };          // liters; 0 = no fuel system
    float initialLevel{ 1.0f };      // 0..1 fraction of capacity at assembly
    float burnPerSecond{ 0.05f };    // L/s at full throttle
    float idleBurnPerSecond{ 0.0f }; // L/s while the engine is on
    float minLevelToRun{ 0.0f };     // 0..1; below this the engine cuts out
};

// Energy system (FALTANTES §17 item 7): a battery the motor draws by throttle,
// recharged by regenerative braking. capacity == 0 disables the system (no
// energy constraint); with a capacity, the motor draws drawPerSecond per
// second at full throttle, regenerates regenPerSecond while braking, and cuts
// out below minChargeToRun * capacity.
struct EnergySystem {
    float capacity{ 0.0f };           // energy units; 0 = no energy system
    float initialCharge{ 1.0f };      // 0..1 fraction at assembly
    float drawPerSecond{ 0.0f };      // units/s at full throttle
    float regenPerSecond{ 0.0f };     // units/s while braking (regen)
    float minChargeToRun{ 0.0f };     // 0..1; below this the motor cuts out
};

// Control mapping (FALTANTES §17 item 7): transforms the RAW input (joystick /
// keys) into the vehicle input — deadzone (inputs within it snap to 0),
// sensitivity curve (sign(x)*|x|^s) and inversion per axis. Defaults are the
// identity mapping.
struct ControlMapping {
    float throttleDeadzone{ 0.0f };
    float throttleSensitivity{ 1.0f };  // curve exponent > 0
    bool throttleInvert{ false };
    float steeringDeadzone{ 0.0f };
    float steeringSensitivity{ 1.0f };
    bool steeringInvert{ false };
    float brakeDeadzone{ 0.0f };
    float brakeSensitivity{ 1.0f };
};

// The vehicle's power/controls configuration (FALTANTES §17 item 7): fuel
// tank + battery + control mapping, data-driven from JSON. The runtime keeps
// the LIVE levels (initial fractions at assembly, moved by burn/draw/regen/
// refuel/recharge).
struct VehiclePower {
    FuelSystem fuel;
    EnergySystem energy;
    ControlMapping controls;
};

// A wheeled vehicle definition: the composition of the public components.
// Position/rotation are the assembly (spawn) transform of the chassis body.
struct VehicleAsset {
    std::string id;   // stable project id; derived from name when absent
    std::string name;
    int version{ 1 };

    glm::vec3 position{ 0.0f };
    glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };

    VehicleKind kind{ VehicleKind::Wheeled };
    // Physics provider behind this vehicle (FALTANTES §17 items 5/6): the
    // asset selects EXACTLY ONE provider (jolt|chrono|jsbsim). Only jolt is
    // vendored today; chrono/jsbsim are refused with a diagnostic at
    // creation (never a silent fallback). Defaults to Jolt (legacy behavior).
    VehicleProviderKind provider{ VehicleProviderKind::Jolt };
    ChassisComponent chassis;
    std::vector<WheelComponent> wheels;
    DrivetrainComponent drivetrain;
    // Propulsion modules (wings/thrusters/buoyancy). Empty for a plain car.
    std::vector<PropulsionModule> propulsion;
    // Occupant seats (FALTANTES §17 item 8). Empty = no occupants.
    std::vector<VehicleSeat> seats;
    // Energy/fuel/controls (FALTANTES §17 item 7). Defaults disable the
    // systems (no consumption) and use the identity control mapping.
    VehiclePower power;

    // All-or-nothing: refuses malformed documents with a diagnostic (never
    // clamps or partially applies). Implemented by the SDK adapter.
    bool load_from_json(const std::string& jsonText, std::string& errorOut);
    // Bit-exact: to_json() round-trips every field (%.9g float emission).
    std::string to_json() const;
    bool validate(std::string& errorOut) const;
};

}  // namespace vehicles
}  // namespace engine
