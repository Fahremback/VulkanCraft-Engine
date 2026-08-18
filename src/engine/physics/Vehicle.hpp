#pragma once

#include "PhysicsRuntime.hpp"

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace Engine::Physics {

// VehicleHandle/InvalidVehicle live in PhysicsRuntime.hpp next to the other
// opaque handles. The descriptor structs below are the canonical vehicle
// types used by the backend seam.

// Vehicle kind (FALTANTES §17 item 3): the Jolt controller family the backend
// assembles. Wheeled is the classic car (WheeledVehicleController); Motorcycle
// adds the lean-spring balance controller (2 wheels); Tracked builds a tank
// with two tracks (TrackedVehicleController) steered by left/right ratios.
// Builtin/bullet keep the raycast fallback for every kind (the kind only
// selects the Jolt controller; the fallback ignores it).
enum class VehicleKind : std::uint8_t { Wheeled, Motorcycle, Tracked };

// Propulsion module (FALTANTES §17 item 3): a force generator attached to the
// chassis at a local position. Wing produces lift from forward speed,
// Thruster pushes along an axis by throttle, Buoyancy floats the chassis when
// its local position goes below the water level. These are pure force
// modules — they apply forces through the same seam as any other gameplay
// force and work on every backend (no solver-specific code).
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


// Wheel description shared by the legacy raycast model and the Jolt Vehicles
// adapter. springStrength is the linear stiffness k (N/m) and damperStrength
// the linear damping c (N*s/m) of the suspension: Jolt's suspension spring
// accepts them directly through ESpringMode::StiffnessAndDamping.
struct WheelDesc {
    glm::vec3 localPosition{0.0f};
    float radius{0.36f};
    float suspensionRestLength{0.45f};
    float suspensionTravel{0.18f};
    float springStrength{26000.0f};
    float damperStrength{3200.0f};
    float tireGrip{1.35f};
    float maxDriveForce{4200.0f};
    float maxBrakeForce{6000.0f};
    float maxSteerAngle{0.55f};
    bool steering{false};
    bool driven{true};
};

struct WheelState {
    bool grounded{false};
    BodyHandle groundBody{InvalidBody};
    glm::vec3 contactPoint{0.0f};
    glm::vec3 contactNormal{0.0f, 1.0f, 0.0f};
    float suspensionLength{0.0f};
    float compression{0.0f};
    float rotation{0.0f};
    float angularSpeed{0.0f};
    float steerAngle{0.0f};
};

struct VehicleInput {
    float throttle{0.0f};
    float steering{0.0f};
    float brake{0.0f};
    float handbrake{0.0f};
};

// Fuel system (FALTANTES §17 item 7): an internal-combustion tank the engine
// burns by throttle. capacity == 0 disables the system (no fuel constraint —
// the legacy behavior); with a capacity, the engine burns burnPerSecond
// liters/second at full throttle (scaled by |throttle|) plus an idle burn,
// and CUTS OUT when the level falls below minLevelToRun * capacity (the
// runtime stops delivering drive — the vehicle coasts to a stop).
struct FuelSystem {
    float capacity{ 0.0f };          // liters; 0 = no fuel system
    float initialLevel{ 1.0f };      // 0..1 fraction of capacity at assembly
    float burnPerSecond{ 0.05f };    // L/s at full throttle
    float idleBurnPerSecond{ 0.0f }; // L/s while the engine is on (no throttle)
    float minLevelToRun{ 0.0f };     // 0..1; below this the engine cuts out
};

// Energy system (FALTANTES §17 item 7): a battery/electric drive the motor
// draws by throttle, recharged by regenerative braking (regen while braking).
// capacity == 0 disables the system (no energy constraint). With a capacity,
// the motor draws drawPerSecond energy/second at full throttle, regenerates
// regenPerSecond while braking, and cuts out below minChargeToRun * capacity.
struct EnergySystem {
    float capacity{ 0.0f };           // energy units; 0 = no energy system
    float initialCharge{ 1.0f };      // 0..1 fraction of capacity at assembly
    float drawPerSecond{ 0.0f };      // units/s at full throttle
    float regenPerSecond{ 0.0f };     // units/s while braking (regen)
    float minChargeToRun{ 0.0f };     // 0..1; below this the motor cuts out
};

// Control mapping (FALTANTES §17 item 7): transforms the RAW input (joystick /
// keys) into the vehicle input before the physics clamp. Each axis applies a
// deadzone (inputs within it snap to 0), a sensitivity curve (sign(x) * |x|^s
// — s > 1 sharpens the center, s < 1 softens it) and an optional inversion.
// Defaults are the identity mapping (no change to legacy behavior).
struct ControlMapping {
    float throttleDeadzone{ 0.0f };   // 0..1
    float throttleSensitivity{ 1.0f }; // curve exponent > 0
    bool throttleInvert{ false };
    float steeringDeadzone{ 0.0f };
    float steeringSensitivity{ 1.0f };
    bool steeringInvert{ false };
    float brakeDeadzone{ 0.0f };
    float brakeSensitivity{ 1.0f };
};

// The vehicle's power/controls configuration (FALTANTES §17 item 7): the fuel
// tank + battery + input mapping, data-driven from the assets. The runtime
// keeps the LIVE levels (fuelLevel/chargeLevel start at the initial fractions
// and move with burn/draw/regen/refuel/recharge).
struct VehiclePower {
    FuelSystem fuel;
    EnergySystem energy;
    ControlMapping controls;
    // Live levels (0..1 fractions of capacity).
    float fuelLevel{ 1.0f };
    float chargeLevel{ 1.0f };
};

// Applies the control mapping to a raw input (the runtime's set_input):
// deadzone -> sensitivity curve -> inversion -> clamp. Pure and deterministic
// (inline so both runtimes and every consumer share the exact same code).
inline VehicleInput map_controls(const ControlMapping& mapping,
                                 const VehicleInput& raw) {
    auto apply_axis = [](float value, float deadzone, float sensitivity,
                         bool invert) {
        const float magnitude = std::abs(value);
        if (magnitude <= deadzone) value = 0.0f;
        else if (sensitivity != 1.0f && sensitivity > 0.0f) {
            // Rescale the post-deadzone range so the curve applies to 0..1.
            const float range = 1.0f - deadzone;
            const float scaled = (magnitude - deadzone) / range;
            value = std::copysign(std::pow(scaled, sensitivity), value);
        }
        if (invert) value = -value;
        return value;
    };
    VehicleInput out;
    out.throttle = apply_axis(raw.throttle, mapping.throttleDeadzone,
                              mapping.throttleSensitivity, mapping.throttleInvert);
    out.steering = apply_axis(raw.steering, mapping.steeringDeadzone,
                              mapping.steeringSensitivity, mapping.steeringInvert);
    out.brake = apply_axis(raw.brake, mapping.brakeDeadzone,
                           mapping.brakeSensitivity, false);
    out.handbrake = raw.handbrake;
    return out;
}

// Vehicle seat (FALTANTES §17 item 8): an occupant position on the vehicle.
// `localPosition` is the seat point in chassis-local space (the ride pose is
// derived from it every update); `exitOffset` is the world-space offset from
// the chassis origin where the occupant spawns on exit.
struct VehicleSeat {
    std::string name;
    glm::vec3 localPosition{ 0.0f };
    glm::vec3 exitOffset{ 0.0f, 1.2f, 1.0f };
};

// Damage part kinds (FALTANTES §17 item 9): parts are AUTO-DERIVED from the
// vehicle's components — Chassis + Drivetrain for every vehicle, Wheel per
// wheel, Beam per beam (beam chassis only). componentIndex names the wheel /
// beam for the Wheel/Beam kinds (0 for Chassis/Drivetrain).
enum class VehiclePartKind : std::uint8_t { Chassis, Drivetrain, Wheel, Beam };

struct VehiclePartInfo {
    std::string name;
    VehiclePartKind kind{ VehiclePartKind::Chassis };
    float maxHealth{ 100.0f };
    float health{ 100.0f };
    bool separated{ false };        // health reached 0
    std::size_t componentIndex{ 0 };
};

// Description of a wheeled vehicle for the Jolt Vehicles adapter. The chassis
// is a normal dynamic body created through the same runtime. `forward` follows
// the engine's convention (the legacy model drives along local -Z); `up` is
// the chassis local up. The drivetrain is a single engine + automatic
// transmission driving one differential per driven axle (left/right pair).
// `kind` selects the Jolt controller family (Wheeled/Motorcycle/Tracked).
struct VehicleDesc {
    BodyHandle chassis{InvalidBody};
    std::vector<WheelDesc> wheels;

    glm::vec3 up{0.0f, 1.0f, 0.0f};
    glm::vec3 forward{0.0f, 0.0f, -1.0f};

    VehicleKind kind{ VehicleKind::Wheeled };

    // Tracked vehicles (kind == Tracked): the two track sides, each with its
    // own set of track wheels (defined by `wheelIndices`). The engine/brake
    // torques come from the wheel descriptors of each track's wheels.
    struct TrackSide {
        std::vector<std::size_t> wheelIndices;  // wheels of this track
        float friction{ 2.0f };                 // track-ground friction
    };
    std::vector<TrackSide> tracks;  // 2 entries for Tracked; ignored otherwise

    // Propulsion modules (asas/propulsão/flutuação): pure force generators
    // applied to the chassis each update (see VehicleRuntime).
    std::vector<PropulsionModule> propulsion;

    float engineMaxTorque{300.0f};  // N*m
    float engineMinRPM{1000.0f};
    float engineMaxRPM{6000.0f};
    float differentialRatio{3.42f};
    std::vector<float> gearRatios{2.66f, 1.78f, 1.3f, 1.0f, 0.74f};
};

} // namespace Engine::Physics
