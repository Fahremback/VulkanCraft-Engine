// VehicleAsset.cpp — the only TU implementing the public vehicle asset
// contract (FALTANTES §17 item 2): composable components (chassis / wheel /
// drivetrain) serialized as versioned JSON. Load is all-or-nothing (refuses
// malformed documents with a diagnostic, never clamps); the emitter round-
// trips float32 exactly (%.9g), so to_json() -> load_from_json() is stable.
//
// Numeric validation uses BIT-LEVEL finite checks: the project compiles with
// /fp:fast (findings #79), which folds std::isfinite(NaN) to true.

#include "engine/vehicles/IVehicleAsset.hpp"

#include "RegistryJson.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace engine {
namespace vehicles {
namespace {

bool finite_float(float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float is 32-bit");
    std::memcpy(&bits, &value, sizeof(bits));
    // IEEE-754: exponent all-ones => NaN or infinity.
    return (bits & 0x7f800000u) != 0x7f800000u;
}

bool finite_vec3(const glm::vec3& value) {
    return finite_float(value.x) && finite_float(value.y) && finite_float(value.z);
}

bool finite_quat(const glm::quat& value) {
    return finite_float(value.x) && finite_float(value.y) &&
           finite_float(value.z) && finite_float(value.w);
}

std::string json_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char ch : text) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += ch; break;
        }
    }
    return out;
}

// --- emitters ---------------------------------------------------------------

std::string emit_vec3(const glm::vec3& v) {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "[" << v.x << "," << v.y << "," << v.z << "]";
    return out.str();
}

std::string emit_quat(const glm::quat& q) {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "[" << q.x << "," << q.y << "," << q.z << "," << q.w << "]";
    return out.str();
}

std::string emit_number_array(const std::vector<float>& values) {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out << ",";
        out << values[i];
    }
    out << "]";
    return out.str();
}

std::string emit_chassis(const ChassisComponent& chassis) {
    const char* shape =
        chassis.shape == ChassisShape::Sphere ? "sphere"
        : chassis.shape == ChassisShape::Capsule ? "capsule" : "box";
    std::ostringstream out;
    out << std::setprecision(9);
    out << "{\"shape\":\"" << shape
        << "\",\"halfExtents\":" << emit_vec3(chassis.halfExtents)
        << ",\"radius\":" << chassis.radius
        << ",\"halfHeight\":" << chassis.halfHeight
        << ",\"mass\":" << chassis.mass
        << ",\"friction\":" << chassis.friction
        << ",\"restitution\":" << chassis.restitution << '}';
    return out.str();
}

std::string emit_wheel(const WheelComponent& wheel) {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "{\"localPosition\":" << emit_vec3(wheel.localPosition)
        << ",\"radius\":" << wheel.radius
        << ",\"suspensionRestLength\":" << wheel.suspensionRestLength
        << ",\"suspensionTravel\":" << wheel.suspensionTravel
        << ",\"springStrength\":" << wheel.springStrength
        << ",\"damperStrength\":" << wheel.damperStrength
        << ",\"tireGrip\":" << wheel.tireGrip
        << ",\"maxDriveForce\":" << wheel.maxDriveForce
        << ",\"maxBrakeForce\":" << wheel.maxBrakeForce
        << ",\"maxSteerAngle\":" << wheel.maxSteerAngle
        << ",\"steering\":" << (wheel.steering ? "true" : "false")
        << ",\"driven\":" << (wheel.driven ? "true" : "false") << '}';
    return out.str();
}

std::string emit_drivetrain(const DrivetrainComponent& drivetrain) {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "{\"engineMaxTorque\":" << drivetrain.engineMaxTorque
        << ",\"engineMinRPM\":" << drivetrain.engineMinRPM
        << ",\"engineMaxRPM\":" << drivetrain.engineMaxRPM
        << ",\"differentialRatio\":" << drivetrain.differentialRatio
        << ",\"gearRatios\":" << emit_number_array(drivetrain.gearRatios) << '}';
    return out.str();
}

const char* propulsion_kind_name(PropulsionKind kind) {
    switch (kind) {
        case PropulsionKind::Wing: return "wing";
        case PropulsionKind::Buoyancy: return "buoyancy";
        case PropulsionKind::Thruster: break;
    }
    return "thruster";
}

std::string emit_propulsion(const PropulsionModule& module) {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "{\"kind\":\"" << propulsion_kind_name(module.kind)
        << "\",\"localPosition\":" << emit_vec3(module.localPosition)
        << ",\"axis\":" << emit_vec3(module.axis)
        << ",\"maxForce\":" << module.maxForce
        << ",\"area\":" << module.area
        << ",\"liftCoefficient\":" << module.liftCoefficient
        << ",\"fluidDensity\":" << module.fluidDensity
        << ",\"waterLevel\":" << module.waterLevel << '}';
    return out.str();
}

std::string emit_seat(const VehicleSeat& seat) {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "{\"name\":\"" << json_escape(seat.name)
        << "\",\"localPosition\":" << emit_vec3(seat.localPosition)
        << ",\"exitOffset\":" << emit_vec3(seat.exitOffset) << '}';
    return out.str();
}

std::string emit_fuel(const FuelSystem& fuel) {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "{\"capacity\":" << fuel.capacity
        << ",\"initialLevel\":" << fuel.initialLevel
        << ",\"burnPerSecond\":" << fuel.burnPerSecond
        << ",\"idleBurnPerSecond\":" << fuel.idleBurnPerSecond
        << ",\"minLevelToRun\":" << fuel.minLevelToRun << '}';
    return out.str();
}

std::string emit_energy(const EnergySystem& energy) {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "{\"capacity\":" << energy.capacity
        << ",\"initialCharge\":" << energy.initialCharge
        << ",\"drawPerSecond\":" << energy.drawPerSecond
        << ",\"regenPerSecond\":" << energy.regenPerSecond
        << ",\"minChargeToRun\":" << energy.minChargeToRun << '}';
    return out.str();
}

std::string emit_controls(const ControlMapping& controls) {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "{\"throttleDeadzone\":" << controls.throttleDeadzone
        << ",\"throttleSensitivity\":" << controls.throttleSensitivity
        << ",\"throttleInvert\":" << (controls.throttleInvert ? "true" : "false")
        << ",\"steeringDeadzone\":" << controls.steeringDeadzone
        << ",\"steeringSensitivity\":" << controls.steeringSensitivity
        << ",\"steeringInvert\":" << (controls.steeringInvert ? "true" : "false")
        << ",\"brakeDeadzone\":" << controls.brakeDeadzone
        << ",\"brakeSensitivity\":" << controls.brakeSensitivity << '}';
    return out.str();
}

std::string emit_power(const VehiclePower& power) {
    return "{\"fuel\":" + emit_fuel(power.fuel) +
           ",\"energy\":" + emit_energy(power.energy) +
           ",\"controls\":" + emit_controls(power.controls) + "}";
}

// --- readers ----------------------------------------------------------------

bool read_vec3(const sdk::JsonValue& object, const std::string& key,
               glm::vec3& out) {
    const sdk::JsonValue* value = object.field(key);
    if (value == nullptr || value->kind != sdk::JsonValue::Kind::Array ||
        value->array.size() != 3) {
        return false;
    }
    for (const sdk::JsonValue& component : value->array) {
        if (component.kind != sdk::JsonValue::Kind::Number) return false;
    }
    out = {static_cast<float>(value->array[0].number),
           static_cast<float>(value->array[1].number),
           static_cast<float>(value->array[2].number)};
    return finite_vec3(out);
}

bool read_quat(const sdk::JsonValue& object, const std::string& key,
               glm::quat& out) {
    const sdk::JsonValue* value = object.field(key);
    if (value == nullptr || value->kind != sdk::JsonValue::Kind::Array ||
        value->array.size() != 4) {
        return false;
    }
    for (const sdk::JsonValue& component : value->array) {
        if (component.kind != sdk::JsonValue::Kind::Number) return false;
    }
    const glm::quat q(
        static_cast<float>(value->array[3].number),   // w
        static_cast<float>(value->array[0].number),   // x
        static_cast<float>(value->array[1].number),   // y
        static_cast<float>(value->array[2].number));  // z
    out = glm::normalize(q);
    return finite_quat(q);
}

// --- power readers ------------------------------------------------------------

bool read_fuel(const sdk::JsonValue& object, FuelSystem& fuel) {
    fuel.capacity = static_cast<float>(sdk::json_number(object, "capacity", 0.0));
    fuel.initialLevel = static_cast<float>(sdk::json_number(object, "initialLevel", 1.0));
    fuel.burnPerSecond = static_cast<float>(sdk::json_number(object, "burnPerSecond", 0.05));
    fuel.idleBurnPerSecond = static_cast<float>(sdk::json_number(object, "idleBurnPerSecond", 0.0));
    fuel.minLevelToRun = static_cast<float>(sdk::json_number(object, "minLevelToRun", 0.0));
    return true;
}

bool read_energy(const sdk::JsonValue& object, EnergySystem& energy) {
    energy.capacity = static_cast<float>(sdk::json_number(object, "capacity", 0.0));
    energy.initialCharge = static_cast<float>(sdk::json_number(object, "initialCharge", 1.0));
    energy.drawPerSecond = static_cast<float>(sdk::json_number(object, "drawPerSecond", 0.0));
    energy.regenPerSecond = static_cast<float>(sdk::json_number(object, "regenPerSecond", 0.0));
    energy.minChargeToRun = static_cast<float>(sdk::json_number(object, "minChargeToRun", 0.0));
    return true;
}

bool read_controls(const sdk::JsonValue& object, ControlMapping& controls) {
    controls.throttleDeadzone = static_cast<float>(sdk::json_number(object, "throttleDeadzone", 0.0));
    controls.throttleSensitivity = static_cast<float>(sdk::json_number(object, "throttleSensitivity", 1.0));
    controls.throttleInvert = sdk::json_bool(object, "throttleInvert", false);
    controls.steeringDeadzone = static_cast<float>(sdk::json_number(object, "steeringDeadzone", 0.0));
    controls.steeringSensitivity = static_cast<float>(sdk::json_number(object, "steeringSensitivity", 1.0));
    controls.steeringInvert = sdk::json_bool(object, "steeringInvert", false);
    controls.brakeDeadzone = static_cast<float>(sdk::json_number(object, "brakeDeadzone", 0.0));
    controls.brakeSensitivity = static_cast<float>(sdk::json_number(object, "brakeSensitivity", 1.0));
    return true;
}

// --- validation ---------------------------------------------------------------

bool validate_power(const VehiclePower& power, std::string& errorOut) {
    auto fail = [&](const std::string& message) {
        errorOut = "vehicle asset: " + message;
        return false;
    };
    const FuelSystem& fuel = power.fuel;
    if (!finite_float(fuel.capacity) || !finite_float(fuel.initialLevel) ||
        !finite_float(fuel.burnPerSecond) || !finite_float(fuel.idleBurnPerSecond) ||
        !finite_float(fuel.minLevelToRun)) {
        return fail("fuel values must be finite");
    }
    if (fuel.capacity < 0.0f || fuel.burnPerSecond < 0.0f ||
        fuel.idleBurnPerSecond < 0.0f) {
        return fail("fuel capacity/rates must be >= 0");
    }
    if (fuel.initialLevel < 0.0f || fuel.initialLevel > 1.0f ||
        fuel.minLevelToRun < 0.0f || fuel.minLevelToRun > 1.0f) {
        return fail("fuel levels must be in [0, 1]");
    }
    const EnergySystem& energy = power.energy;
    if (!finite_float(energy.capacity) || !finite_float(energy.initialCharge) ||
        !finite_float(energy.drawPerSecond) || !finite_float(energy.regenPerSecond) ||
        !finite_float(energy.minChargeToRun)) {
        return fail("energy values must be finite");
    }
    if (energy.capacity < 0.0f || energy.drawPerSecond < 0.0f ||
        energy.regenPerSecond < 0.0f) {
        return fail("energy capacity/rates must be >= 0");
    }
    if (energy.initialCharge < 0.0f || energy.initialCharge > 1.0f ||
        energy.minChargeToRun < 0.0f || energy.minChargeToRun > 1.0f) {
        return fail("energy levels must be in [0, 1]");
    }
    const ControlMapping& controls = power.controls;
    if (!finite_float(controls.throttleDeadzone) ||
        !finite_float(controls.throttleSensitivity) ||
        !finite_float(controls.steeringDeadzone) ||
        !finite_float(controls.steeringSensitivity) ||
        !finite_float(controls.brakeDeadzone) ||
        !finite_float(controls.brakeSensitivity)) {
        return fail("control values must be finite");
    }
    if (controls.throttleDeadzone < 0.0f || controls.throttleDeadzone > 1.0f ||
        controls.steeringDeadzone < 0.0f || controls.steeringDeadzone > 1.0f ||
        controls.brakeDeadzone < 0.0f || controls.brakeDeadzone > 1.0f) {
        return fail("control deadzones must be in [0, 1]");
    }
    if (!(controls.throttleSensitivity > 0.0f) ||
        !(controls.steeringSensitivity > 0.0f) ||
        !(controls.brakeSensitivity > 0.0f)) {
        return fail("control sensitivities must be > 0");
    }
    return true;
}

bool validate_asset(const VehicleAsset& asset, std::string& errorOut) {
    auto fail = [&](const std::string& message) {
        errorOut = "vehicle asset: " + message;
        return false;
    };
    if (asset.name.empty()) return fail("name must not be empty");
    if (asset.version != 1) return fail("unsupported version");
    if (asset.wheels.empty()) return fail("at least one wheel is required");
    if (!finite_vec3(asset.position) || !finite_quat(asset.rotation)) {
        return fail("position/rotation must be finite");
    }
    const ChassisComponent& chassis = asset.chassis;
    if (!finite_vec3(chassis.halfExtents) || !finite_float(chassis.radius) ||
        !finite_float(chassis.halfHeight) || !finite_float(chassis.mass) ||
        !finite_float(chassis.friction) || !finite_float(chassis.restitution)) {
        return fail("chassis values must be finite");
    }
    if (!(chassis.mass > 0.0f)) return fail("chassis mass must be > 0");
    if (chassis.friction < 0.0f || chassis.restitution < 0.0f) {
        return fail("chassis friction/restitution must be >= 0");
    }
    if (chassis.shape == ChassisShape::Sphere || chassis.shape == ChassisShape::Capsule) {
        if (!(chassis.radius > 0.0f)) return fail("chassis radius must be > 0");
    }
    if (chassis.shape == ChassisShape::Capsule && !(chassis.halfHeight > 0.0f)) {
        return fail("chassis halfHeight must be > 0");
    }
    if (chassis.shape == ChassisShape::Box &&
        (!(chassis.halfExtents.x > 0.0f) || !(chassis.halfExtents.y > 0.0f) ||
         !(chassis.halfExtents.z > 0.0f))) {
        return fail("chassis halfExtents must be > 0");
    }
    for (std::size_t i = 0; i < asset.wheels.size(); ++i) {
        const WheelComponent& wheel = asset.wheels[i];
        if (!finite_vec3(wheel.localPosition) || !finite_float(wheel.radius) ||
            !finite_float(wheel.suspensionRestLength) ||
            !finite_float(wheel.suspensionTravel) ||
            !finite_float(wheel.springStrength) ||
            !finite_float(wheel.damperStrength) ||
            !finite_float(wheel.tireGrip) || !finite_float(wheel.maxDriveForce) ||
            !finite_float(wheel.maxBrakeForce) ||
            !finite_float(wheel.maxSteerAngle)) {
            return fail("wheel " + std::to_string(i) + " has non-finite values");
        }
        if (!(wheel.radius > 0.0f)) return fail("wheel radius must be > 0");
        if (!(wheel.suspensionRestLength > 0.0f)) {
            return fail("wheel suspensionRestLength must be > 0");
        }
        if (wheel.suspensionTravel < 0.0f || wheel.springStrength < 0.0f ||
            wheel.damperStrength < 0.0f || wheel.tireGrip < 0.0f ||
            wheel.maxDriveForce < 0.0f || wheel.maxBrakeForce < 0.0f ||
            wheel.maxSteerAngle < 0.0f) {
            return fail("wheel " + std::to_string(i) + " has a negative value");
        }
    }
    const DrivetrainComponent& drivetrain = asset.drivetrain;
    if (!finite_float(drivetrain.engineMaxTorque) ||
        !finite_float(drivetrain.engineMinRPM) ||
        !finite_float(drivetrain.engineMaxRPM) ||
        !finite_float(drivetrain.differentialRatio)) {
        return fail("drivetrain values must be finite");
    }
    if (drivetrain.engineMaxTorque < 0.0f) {
        return fail("drivetrain engineMaxTorque must be >= 0");
    }
    if (!(drivetrain.engineMinRPM > 0.0f)) {
        return fail("drivetrain engineMinRPM must be > 0");
    }
    if (!(drivetrain.engineMaxRPM > drivetrain.engineMinRPM)) {
        return fail("drivetrain engineMaxRPM must exceed engineMinRPM");
    }
    if (!(drivetrain.differentialRatio > 0.0f)) {
        return fail("drivetrain differentialRatio must be > 0");
    }
    if (drivetrain.gearRatios.empty()) return fail("drivetrain needs at least one gear");
    for (const float ratio : drivetrain.gearRatios) {
        if (!finite_float(ratio) || !(ratio > 0.0f)) {
            return fail("drivetrain gear ratios must be finite and > 0");
        }
    }
    for (std::size_t i = 0; i < asset.propulsion.size(); ++i) {
        const PropulsionModule& module = asset.propulsion[i];
        if (!finite_vec3(module.localPosition) || !finite_vec3(module.axis) ||
            !finite_float(module.maxForce) || !finite_float(module.area) ||
            !finite_float(module.liftCoefficient) ||
            !finite_float(module.fluidDensity) || !finite_float(module.waterLevel)) {
            return fail("propulsion module " + std::to_string(i) + " has non-finite values");
        }
        if (glm::dot(module.axis, module.axis) < 1.0e-8f) {
            return fail("propulsion module " + std::to_string(i) + " has a zero axis");
        }
        if (!(module.maxForce >= 0.0f) || !(module.area >= 0.0f) ||
            !(module.liftCoefficient >= 0.0f) || !(module.fluidDensity >= 0.0f)) {
            return fail("propulsion module " + std::to_string(i) + " has a negative value");
        }
    }
    for (std::size_t i = 0; i < asset.seats.size(); ++i) {
        const VehicleSeat& seat = asset.seats[i];
        if (!finite_vec3(seat.localPosition) || !finite_vec3(seat.exitOffset)) {
            return fail("seat " + std::to_string(i) + " has non-finite values");
        }
    }
    if (!validate_power(asset.power, errorOut)) return false;
    return true;
}

}  // namespace

bool VehicleAsset::load_from_json(const std::string& jsonText,
                                  std::string& errorOut) {
    sdk::JsonValue root;
    if (!sdk::json_parse(jsonText, root, errorOut) || !root.is_object()) {
        if (errorOut.empty()) errorOut = "vehicle asset: root must be an object";
        return false;
    }

    VehicleAsset parsed;
    parsed.name = sdk::json_string(root, "name", "");
    parsed.version = static_cast<int>(sdk::json_number(root, "version", 1));
    const std::string id = sdk::json_string(root, "id", "");
    parsed.id = sdk::uuid_or_derived(id, "vehicles:" + parsed.name);
    // Physics provider (FALTANTES §17 items 5/6): jolt|chrono|jsbsim. The
    // asset selects EXACTLY ONE provider; unknown names are refused
    // (all-or-nothing). The availability check happens at CREATION (an
    // asset requesting chrono/jsbsim is refused there — never a silent
    // fallback), not here: the document must round-trip regardless.
    const std::string providerName = sdk::json_string(root, "provider", "jolt");
    if (!parse_vehicle_provider(providerName, parsed.provider)) {
        errorOut = "vehicle asset: unknown provider '" + providerName + "'";
        return false;
    }
    if (!read_vec3(root, "position", parsed.position)) {
        errorOut = "vehicle asset: position must be a finite [x,y,z] array";
        return false;
    }
    if (!read_quat(root, "rotation", parsed.rotation)) {
        errorOut = "vehicle asset: rotation must be a finite [x,y,z,w] array";
        return false;
    }

    const sdk::JsonValue* chassisValue = root.field("chassis");
    if (chassisValue == nullptr || !chassisValue->is_object()) {
        errorOut = "vehicle asset: chassis must be an object";
        return false;
    }
    const sdk::JsonValue& chassis = *chassisValue;
    const std::string shape = sdk::json_string(chassis, "shape", "box");
    parsed.chassis.shape = shape == "sphere" ? ChassisShape::Sphere
                            : shape == "capsule" ? ChassisShape::Capsule
                                                 : ChassisShape::Box;
    if (shape != "box" && shape != "sphere" && shape != "capsule") {
        errorOut = "vehicle asset: unknown chassis shape '" + shape + "'";
        return false;
    }
    parsed.chassis.halfExtents = glm::vec3(0.9f, 0.35f, 0.56f);
    read_vec3(chassis, "halfExtents", parsed.chassis.halfExtents);
    parsed.chassis.radius = static_cast<float>(sdk::json_number(chassis, "radius", 0.5));
    parsed.chassis.halfHeight = static_cast<float>(sdk::json_number(chassis, "halfHeight", 0.65));
    parsed.chassis.mass = static_cast<float>(sdk::json_number(chassis, "mass", 1200.0));
    parsed.chassis.friction = static_cast<float>(sdk::json_number(chassis, "friction", 0.55));
    parsed.chassis.restitution = static_cast<float>(sdk::json_number(chassis, "restitution", 0.05));

    const sdk::JsonValue* wheelsValue = root.field("wheels");
    if (wheelsValue == nullptr || wheelsValue->kind != sdk::JsonValue::Kind::Array) {
        errorOut = "vehicle asset: wheels must be an array";
        return false;
    }
    parsed.wheels.reserve(wheelsValue->array.size());
    for (const sdk::JsonValue& wheel : wheelsValue->array) {
        if (!wheel.is_object()) {
            errorOut = "vehicle asset: each wheel must be an object";
            return false;
        }
        WheelComponent component;
        glm::vec3 position{0.0f};
        if (!read_vec3(wheel, "localPosition", position)) {
            errorOut = "vehicle asset: wheel localPosition must be a finite [x,y,z] array";
            return false;
        }
        component.localPosition = position;
        component.radius = static_cast<float>(sdk::json_number(wheel, "radius", 0.36));
        component.suspensionRestLength = static_cast<float>(sdk::json_number(wheel, "suspensionRestLength", 0.45));
        component.suspensionTravel = static_cast<float>(sdk::json_number(wheel, "suspensionTravel", 0.18));
        component.springStrength = static_cast<float>(sdk::json_number(wheel, "springStrength", 26000.0));
        component.damperStrength = static_cast<float>(sdk::json_number(wheel, "damperStrength", 3200.0));
        component.tireGrip = static_cast<float>(sdk::json_number(wheel, "tireGrip", 1.35));
        component.maxDriveForce = static_cast<float>(sdk::json_number(wheel, "maxDriveForce", 4200.0));
        component.maxBrakeForce = static_cast<float>(sdk::json_number(wheel, "maxBrakeForce", 6000.0));
        component.maxSteerAngle = static_cast<float>(sdk::json_number(wheel, "maxSteerAngle", 0.55));
        component.steering = sdk::json_bool(wheel, "steering", false);
        component.driven = sdk::json_bool(wheel, "driven", true);
        parsed.wheels.push_back(component);
    }

    const sdk::JsonValue* drivetrainValue = root.field("drivetrain");
    if (drivetrainValue == nullptr || !drivetrainValue->is_object()) {
        errorOut = "vehicle asset: drivetrain must be an object";
        return false;
    }
    const sdk::JsonValue& drivetrain = *drivetrainValue;
    parsed.drivetrain.engineMaxTorque = static_cast<float>(sdk::json_number(drivetrain, "engineMaxTorque", 0.0));
    parsed.drivetrain.engineMinRPM = static_cast<float>(sdk::json_number(drivetrain, "engineMinRPM", 1000.0));
    parsed.drivetrain.engineMaxRPM = static_cast<float>(sdk::json_number(drivetrain, "engineMaxRPM", 6000.0));
    parsed.drivetrain.differentialRatio = static_cast<float>(sdk::json_number(drivetrain, "differentialRatio", 3.42));
    const std::vector<double> gears = sdk::json_number_array(drivetrain, "gearRatios");
    parsed.drivetrain.gearRatios.clear();
    parsed.drivetrain.gearRatios.reserve(gears.size());
    for (const double gear : gears) {
        parsed.drivetrain.gearRatios.push_back(static_cast<float>(gear));
    }

    // Kind: wheeled (default) / motorcycle / tracked.
    const std::string kind = sdk::json_string(root, "kind", "wheeled");
    if (kind == "wheeled") {
        parsed.kind = VehicleKind::Wheeled;
    } else if (kind == "motorcycle") {
        parsed.kind = VehicleKind::Motorcycle;
    } else if (kind == "tracked") {
        parsed.kind = VehicleKind::Tracked;
    } else {
        errorOut = "vehicle asset: unknown kind '" + kind + "'";
        return false;
    }

    // Propulsion modules (wing / thruster / buoyancy).
    const sdk::JsonValue* propulsionValue = root.field("propulsion");
    if (propulsionValue == nullptr) {
        // optional
    } else if (propulsionValue->kind != sdk::JsonValue::Kind::Array) {
        errorOut = "vehicle asset: propulsion must be an array";
        return false;
    } else {
        parsed.propulsion.clear();
        parsed.propulsion.reserve(propulsionValue->array.size());
        for (const sdk::JsonValue& moduleValue : propulsionValue->array) {
            if (!moduleValue.is_object()) {
                errorOut = "vehicle asset: each propulsion module must be an object";
                return false;
            }
            PropulsionModule module;
            const std::string mkind = sdk::json_string(moduleValue, "kind", "thruster");
            if (mkind == "wing") {
                module.kind = PropulsionKind::Wing;
            } else if (mkind == "buoyancy") {
                module.kind = PropulsionKind::Buoyancy;
            } else if (mkind == "thruster") {
                module.kind = PropulsionKind::Thruster;
            } else {
                errorOut = "vehicle asset: unknown propulsion kind '" + mkind + "'";
                return false;
            }
            glm::vec3 position{0.0f};
            if (!read_vec3(moduleValue, "localPosition", position)) {
                errorOut = "vehicle asset: propulsion localPosition must be a finite [x,y,z] array";
                return false;
            }
            module.localPosition = position;
            glm::vec3 axis{0.0f, 1.0f, 0.0f};
            if (!read_vec3(moduleValue, "axis", axis)) {
                errorOut = "vehicle asset: propulsion axis must be a finite [x,y,z] array";
                return false;
            }
            if (glm::dot(axis, axis) < 1.0e-8f) {
                errorOut = "vehicle asset: propulsion axis must be non-zero";
                return false;
            }
            module.axis = glm::normalize(axis);
            module.maxForce = static_cast<float>(sdk::json_number(moduleValue, "maxForce", 1000.0));
            module.area = static_cast<float>(sdk::json_number(moduleValue, "area", 4.0));
            module.liftCoefficient = static_cast<float>(sdk::json_number(moduleValue, "liftCoefficient", 0.8));
            module.fluidDensity = static_cast<float>(sdk::json_number(moduleValue, "fluidDensity", 1000.0));
            module.waterLevel = static_cast<float>(sdk::json_number(moduleValue, "waterLevel", 0.0));
            parsed.propulsion.push_back(module);
        }
    }

    // Occupant seats (FALTANTES §17 item 8).
    const sdk::JsonValue* seatsValue = root.field("seats");
    if (seatsValue == nullptr) {
        // optional
    } else if (seatsValue->kind != sdk::JsonValue::Kind::Array) {
        errorOut = "vehicle asset: seats must be an array";
        return false;
    } else {
        parsed.seats.clear();
        parsed.seats.reserve(seatsValue->array.size());
        for (const sdk::JsonValue& seatValue : seatsValue->array) {
            if (!seatValue.is_object()) {
                errorOut = "vehicle asset: each seat must be an object";
                return false;
            }
            VehicleSeat seat;
            seat.name = sdk::json_string(seatValue, "name", "");
            if (!read_vec3(seatValue, "localPosition", seat.localPosition)) {
                errorOut = "vehicle asset: seat localPosition must be a finite [x,y,z] array";
                return false;
            }
            if (!read_vec3(seatValue, "exitOffset", seat.exitOffset)) {
                errorOut = "vehicle asset: seat exitOffset must be a finite [x,y,z] array";
                return false;
            }
            parsed.seats.push_back(seat);
        }
    }

    // Power (fuel / energy / controls) — FALTANTES §17 item 7.
    const sdk::JsonValue* powerValue = root.field("power");
    if (powerValue != nullptr) {
        if (!powerValue->is_object()) {
            errorOut = "vehicle asset: power must be an object";
            return false;
        }
        const sdk::JsonValue* fuelValue = powerValue->field("fuel");
        if (fuelValue != nullptr) {
            if (!fuelValue->is_object() || !read_fuel(*fuelValue, parsed.power.fuel)) {
                errorOut = "vehicle asset: power.fuel must be an object";
                return false;
            }
        }
        const sdk::JsonValue* energyValue = powerValue->field("energy");
        if (energyValue != nullptr) {
            if (!energyValue->is_object() || !read_energy(*energyValue, parsed.power.energy)) {
                errorOut = "vehicle asset: power.energy must be an object";
                return false;
            }
        }
        const sdk::JsonValue* controlsValue = powerValue->field("controls");
        if (controlsValue != nullptr) {
            if (!controlsValue->is_object() || !read_controls(*controlsValue, parsed.power.controls)) {
                errorOut = "vehicle asset: power.controls must be an object";
                return false;
            }
        }
    }

    std::string validationError;
    if (!validate_asset(parsed, validationError)) {
        errorOut = validationError;
        return false;
    }
    *this = std::move(parsed);
    return true;
}

std::string VehicleAsset::to_json() const {
    std::ostringstream out;
    out << "{\"id\":\"" << json_escape(id) << "\",\"name\":\""
        << json_escape(name) << "\",\"version\":" << version
        << ",\"position\":" << emit_vec3(position)
        << ",\"rotation\":" << emit_quat(rotation)
        << ",\"chassis\":" << emit_chassis(chassis)
        << ",\"wheels\":[";
    for (std::size_t i = 0; i < wheels.size(); ++i) {
        if (i) out << ",";
        out << emit_wheel(wheels[i]);
    }
    out << "],\"drivetrain\":" << emit_drivetrain(drivetrain)
        << ",\"kind\":\"";
    switch (kind) {
        case VehicleKind::Motorcycle: out << "motorcycle"; break;
        case VehicleKind::Tracked: out << "tracked"; break;
        case VehicleKind::Wheeled: out << "wheeled"; break;
    }
    out << "\",\"propulsion\":[";
    for (std::size_t i = 0; i < propulsion.size(); ++i) {
        if (i) out << ",";
        out << emit_propulsion(propulsion[i]);
    }
    out << "],\"seats\":[";
    for (std::size_t i = 0; i < seats.size(); ++i) {
        if (i) out << ",";
        out << emit_seat(seats[i]);
    }
    out << "],\"power\":" << emit_power(power)
        << ",\"provider\":\"" << vehicle_provider_name(provider) << "\"}";
    return out.str();
}

bool VehicleAsset::validate(std::string& errorOut) const {
    return validate_asset(*this, errorOut);
}

}  // namespace vehicles
}  // namespace engine
