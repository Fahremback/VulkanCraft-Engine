// BeamGraphAsset.cpp — the only TU implementing the public node/beam chassis
// contract (FALTANTES §17 item 4): nodes + beams (with per-beam stiffness) +
// wheel mounts serialized as versioned JSON. Load is all-or-nothing (refuses
// malformed documents with a diagnostic, never clamps); the emitter round-
// trips float32 exactly (%.9g), so to_json() -> load_from_json() is stable.
//
// Numeric validation uses BIT-LEVEL finite checks (/fp:fast folds
// std::isfinite — findings #79).

#include "engine/vehicles/IBeamGraphAsset.hpp"

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

// --- emitters ---------------------------------------------------------------

std::string emit_node(const BeamNode& node) {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "{\"position\":" << emit_vec3(node.position)
        << ",\"fixed\":" << (node.fixed ? "true" : "false") << '}';
    return out.str();
}

std::string emit_beam(const Beam& beam) {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "{\"a\":" << beam.a << ",\"b\":" << beam.b
        << ",\"stiffness\":" << beam.stiffness << '}';
    return out.str();
}

std::string emit_wheel(const BeamWheelMount& mount) {
    std::ostringstream out;
    out << std::setprecision(9);
    const WheelComponent& wheel = mount.wheel;
    out << "{\"node\":" << mount.node
        << ",\"steering\":" << (mount.steering ? "true" : "false")
        << ",\"driven\":" << (mount.driven ? "true" : "false")
        << ",\"wheel\":{\"localPosition\":" << emit_vec3(wheel.localPosition)
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
        << ",\"driven\":" << (wheel.driven ? "true" : "false") << "}}";
    return out.str();
}

std::string emit_solver(const BeamSolverConfig& solver) {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "{\"substeps\":" << solver.substeps
        << ",\"solverIterations\":" << solver.solverIterations
        << ",\"stiffness\":" << solver.stiffness
        << ",\"damping\":" << solver.damping
        << ",\"gravity\":" << emit_vec3(solver.gravity) << '}';
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

bool validate_power(const VehiclePower& power, std::string& errorOut) {
    auto fail = [&](const std::string& message) {
        errorOut = "beam asset: " + message;
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

// --- validation ---------------------------------------------------------------

bool validate_wheel(const WheelComponent& wheel, std::string& errorOut) {
    if (!finite_vec3(wheel.localPosition) || !finite_float(wheel.radius) ||
        !finite_float(wheel.suspensionRestLength) ||
        !finite_float(wheel.suspensionTravel) ||
        !finite_float(wheel.springStrength) ||
        !finite_float(wheel.damperStrength) || !finite_float(wheel.tireGrip) ||
        !finite_float(wheel.maxDriveForce) || !finite_float(wheel.maxBrakeForce) ||
        !finite_float(wheel.maxSteerAngle)) {
        errorOut = "beam asset: wheel has non-finite values";
        return false;
    }
    if (!(wheel.radius > 0.0f)) { errorOut = "beam asset: wheel radius must be > 0"; return false; }
    if (!(wheel.suspensionRestLength > 0.0f)) {
        errorOut = "beam asset: wheel suspensionRestLength must be > 0";
        return false;
    }
    if (wheel.suspensionTravel < 0.0f || wheel.springStrength < 0.0f ||
        wheel.damperStrength < 0.0f || wheel.tireGrip < 0.0f ||
        wheel.maxDriveForce < 0.0f || wheel.maxBrakeForce < 0.0f ||
        wheel.maxSteerAngle < 0.0f) {
        errorOut = "beam asset: wheel has a negative value";
        return false;
    }
    return true;
}

bool validate_asset(const BeamGraphAsset& asset, std::string& errorOut) {
    auto fail = [&](const std::string& message) {
        errorOut = "beam asset: " + message;
        return false;
    };
    if (asset.name.empty()) return fail("name must not be empty");
    if (asset.version != 1) return fail("unsupported version");
    if (asset.nodes.empty()) return fail("at least one node is required");
    if (asset.nodes.size() > 4096) return fail("node count exceeds 4096");
    if (asset.beams.empty()) return fail("at least one beam is required");
    if (!finite_vec3(asset.position) || !finite_quat(asset.rotation)) {
        return fail("position/rotation must be finite");
    }
    if (!finite_float(asset.mass) || !(asset.mass > 0.0f)) {
        return fail("mass must be finite and > 0");
    }
    for (std::size_t i = 0; i < asset.nodes.size(); ++i) {
        if (!finite_vec3(asset.nodes[i].position)) {
            return fail("node " + std::to_string(i) + " has a non-finite position");
        }
    }
    for (std::size_t i = 0; i < asset.beams.size(); ++i) {
        const Beam& beam = asset.beams[i];
        if (beam.a >= asset.nodes.size() || beam.b >= asset.nodes.size()) {
            return fail("beam " + std::to_string(i) + " references an invalid node");
        }
        if (!finite_float(beam.stiffness) || !(beam.stiffness > 0.0f) ||
            beam.stiffness > 1.0f) {
            return fail("beam " + std::to_string(i) + " stiffness must be in (0, 1]");
        }
    }
    for (std::size_t i = 0; i < asset.wheels.size(); ++i) {
        const BeamWheelMount& mount = asset.wheels[i];
        if (mount.node >= asset.nodes.size()) {
            return fail("wheel " + std::to_string(i) + " mounts an invalid node");
        }
        if (!validate_wheel(mount.wheel, errorOut)) return false;
    }
    for (std::size_t i = 0; i < asset.seats.size(); ++i) {
        const VehicleSeat& seat = asset.seats[i];
        if (!finite_vec3(seat.localPosition) || !finite_vec3(seat.exitOffset)) {
            return fail("seat " + std::to_string(i) + " has non-finite values");
        }
    }
    if (!validate_power(asset.power, errorOut)) return false;
    const BeamSolverConfig& solver = asset.solver;
    if (solver.substeps < 1 || solver.substeps > 16) {
        return fail("solver substeps must be in [1, 16]");
    }
    if (solver.solverIterations < 1 || solver.solverIterations > 64) {
        return fail("solver solverIterations must be in [1, 64]");
    }
    if (!finite_float(solver.stiffness) || !(solver.stiffness > 0.0f) ||
        solver.stiffness > 1.0f) {
        return fail("solver stiffness must be in (0, 1]");
    }
    if (!finite_float(solver.damping) || solver.damping < 0.0f ||
        solver.damping >= 1.0f) {
        return fail("solver damping must be in [0, 1)");
    }
    if (!finite_vec3(solver.gravity) || glm::length(solver.gravity) > 1000.0f) {
        return fail("solver gravity must be finite and below 1000 magnitude");
    }
    return true;
}

}  // namespace

bool BeamGraphAsset::load_from_json(const std::string& jsonText,
                                    std::string& errorOut) {
    sdk::JsonValue root;
    if (!sdk::json_parse(jsonText, root, errorOut) || !root.is_object()) {
        if (errorOut.empty()) errorOut = "beam asset: root must be an object";
        return false;
    }

    BeamGraphAsset parsed;
    parsed.name = sdk::json_string(root, "name", "");
    parsed.version = static_cast<int>(sdk::json_number(root, "version", 1));
    const std::string id = sdk::json_string(root, "id", "");
    parsed.id = sdk::uuid_or_derived(id, "beams:" + parsed.name);
    // Physics provider (FALTANTES §17 items 5/6): jolt|chrono|jsbsim. Same
    // contract as VehicleAsset — unknown names refused (all-or-nothing),
    // availability checked at creation, never a silent fallback.
    const std::string providerName = sdk::json_string(root, "provider", "jolt");
    if (!parse_vehicle_provider(providerName, parsed.provider)) {
        errorOut = "beam asset: unknown provider '" + providerName + "'";
        return false;
    }
    if (!read_vec3(root, "position", parsed.position)) {
        errorOut = "beam asset: position must be a finite [x,y,z] array";
        return false;
    }
    if (!read_quat(root, "rotation", parsed.rotation)) {
        errorOut = "beam asset: rotation must be a finite [x,y,z,w] array";
        return false;
    }
    parsed.mass = static_cast<float>(sdk::json_number(root, "mass", 1200.0));

    const sdk::JsonValue* nodesValue = root.field("nodes");
    if (nodesValue == nullptr || nodesValue->kind != sdk::JsonValue::Kind::Array) {
        errorOut = "beam asset: nodes must be an array";
        return false;
    }
    parsed.nodes.reserve(nodesValue->array.size());
    for (const sdk::JsonValue& node : nodesValue->array) {
        if (!node.is_object()) {
            errorOut = "beam asset: each node must be an object";
            return false;
        }
        BeamNode beamNode;
        if (!read_vec3(node, "position", beamNode.position)) {
            errorOut = "beam asset: node position must be a finite [x,y,z] array";
            return false;
        }
        beamNode.fixed = sdk::json_bool(node, "fixed", false);
        parsed.nodes.push_back(beamNode);
    }

    const sdk::JsonValue* beamsValue = root.field("beams");
    if (beamsValue == nullptr || beamsValue->kind != sdk::JsonValue::Kind::Array) {
        errorOut = "beam asset: beams must be an array";
        return false;
    }
    parsed.beams.reserve(beamsValue->array.size());
    for (const sdk::JsonValue& beamValue : beamsValue->array) {
        if (!beamValue.is_object()) {
            errorOut = "beam asset: each beam must be an object";
            return false;
        }
        Beam beam;
        beam.a = static_cast<std::uint32_t>(
            sdk::json_number(beamValue, "a", 0));
        beam.b = static_cast<std::uint32_t>(
            sdk::json_number(beamValue, "b", 0));
        beam.stiffness = static_cast<float>(
            sdk::json_number(beamValue, "stiffness", 0.9));
        parsed.beams.push_back(beam);
    }

    const sdk::JsonValue* wheelsValue = root.field("wheels");
    if (wheelsValue == nullptr) {
        // optional — a non-wheeled beam chassis (trailer/crane) is valid
    } else if (wheelsValue->kind != sdk::JsonValue::Kind::Array) {
        errorOut = "beam asset: wheels must be an array";
        return false;
    } else {
        parsed.wheels.reserve(wheelsValue->array.size());
        for (const sdk::JsonValue& mountValue : wheelsValue->array) {
            if (!mountValue.is_object()) {
                errorOut = "beam asset: each wheel mount must be an object";
                return false;
            }
            BeamWheelMount mount;
            mount.node = static_cast<std::uint32_t>(
                sdk::json_number(mountValue, "node", 0));
            mount.steering = sdk::json_bool(mountValue, "steering", true);
            mount.driven = sdk::json_bool(mountValue, "driven", true);
            const sdk::JsonValue* wheelValue = mountValue.field("wheel");
            if (wheelValue == nullptr || !wheelValue->is_object()) {
                errorOut = "beam asset: each wheel mount needs a wheel object";
                return false;
            }
            glm::vec3 position{0.0f};
            if (!read_vec3(*wheelValue, "localPosition", position)) {
                errorOut = "beam asset: wheel localPosition must be a finite [x,y,z] array";
                return false;
            }
            mount.wheel.localPosition = position;
            mount.wheel.radius = static_cast<float>(
                sdk::json_number(*wheelValue, "radius", 0.36));
            mount.wheel.suspensionRestLength = static_cast<float>(
                sdk::json_number(*wheelValue, "suspensionRestLength", 0.45));
            mount.wheel.suspensionTravel = static_cast<float>(
                sdk::json_number(*wheelValue, "suspensionTravel", 0.18));
            mount.wheel.springStrength = static_cast<float>(
                sdk::json_number(*wheelValue, "springStrength", 26000.0));
            mount.wheel.damperStrength = static_cast<float>(
                sdk::json_number(*wheelValue, "damperStrength", 3200.0));
            mount.wheel.tireGrip = static_cast<float>(
                sdk::json_number(*wheelValue, "tireGrip", 1.35));
            mount.wheel.maxDriveForce = static_cast<float>(
                sdk::json_number(*wheelValue, "maxDriveForce", 4200.0));
            mount.wheel.maxBrakeForce = static_cast<float>(
                sdk::json_number(*wheelValue, "maxBrakeForce", 6000.0));
            mount.wheel.maxSteerAngle = static_cast<float>(
                sdk::json_number(*wheelValue, "maxSteerAngle", 0.55));
            mount.wheel.steering = sdk::json_bool(*wheelValue, "steering", false);
            mount.wheel.driven = sdk::json_bool(*wheelValue, "driven", true);
            parsed.wheels.push_back(mount);
        }
    }

    // Occupant seats (FALTANTES §17 item 8).
    const sdk::JsonValue* seatsValue = root.field("seats");
    if (seatsValue == nullptr) {
        // optional
    } else if (seatsValue->kind != sdk::JsonValue::Kind::Array) {
        errorOut = "beam asset: seats must be an array";
        return false;
    } else {
        parsed.seats.clear();
        parsed.seats.reserve(seatsValue->array.size());
        for (const sdk::JsonValue& seatValue : seatsValue->array) {
            if (!seatValue.is_object()) {
                errorOut = "beam asset: each seat must be an object";
                return false;
            }
            VehicleSeat seat;
            seat.name = sdk::json_string(seatValue, "name", "");
            if (!read_vec3(seatValue, "localPosition", seat.localPosition)) {
                errorOut = "beam asset: seat localPosition must be a finite [x,y,z] array";
                return false;
            }
            if (!read_vec3(seatValue, "exitOffset", seat.exitOffset)) {
                errorOut = "beam asset: seat exitOffset must be a finite [x,y,z] array";
                return false;
            }
            parsed.seats.push_back(seat);
        }
    }

    // Power (fuel / energy / controls) — FALTANTES §17 item 7.
    const sdk::JsonValue* powerValue = root.field("power");
    if (powerValue != nullptr) {
        if (!powerValue->is_object()) {
            errorOut = "beam asset: power must be an object";
            return false;
        }
        const sdk::JsonValue* fuelValue = powerValue->field("fuel");
        if (fuelValue != nullptr) {
            if (!fuelValue->is_object() || !read_fuel(*fuelValue, parsed.power.fuel)) {
                errorOut = "beam asset: power.fuel must be an object";
                return false;
            }
        }
        const sdk::JsonValue* energyValue = powerValue->field("energy");
        if (energyValue != nullptr) {
            if (!energyValue->is_object() || !read_energy(*energyValue, parsed.power.energy)) {
                errorOut = "beam asset: power.energy must be an object";
                return false;
            }
        }
        const sdk::JsonValue* controlsValue = powerValue->field("controls");
        if (controlsValue != nullptr) {
            if (!controlsValue->is_object() || !read_controls(*controlsValue, parsed.power.controls)) {
                errorOut = "beam asset: power.controls must be an object";
                return false;
            }
        }
    }

    const sdk::JsonValue* solverValue = root.field("solver");
    if (solverValue != nullptr && solverValue->is_object()) {
        parsed.solver.substeps = static_cast<int>(
            sdk::json_number(*solverValue, "substeps", 2));
        parsed.solver.solverIterations = static_cast<int>(
            sdk::json_number(*solverValue, "solverIterations", 8));
        parsed.solver.stiffness = static_cast<float>(
            sdk::json_number(*solverValue, "stiffness", 0.8));
        parsed.solver.damping = static_cast<float>(
            sdk::json_number(*solverValue, "damping", 0.1));
        glm::vec3 gravity{0.0f, -9.81f, 0.0f};
        if (!read_vec3(*solverValue, "gravity", gravity)) {
            errorOut = "beam asset: solver gravity must be a finite [x,y,z] array";
            return false;
        }
        parsed.solver.gravity = gravity;
    }

    std::string validationError;
    if (!validate_asset(parsed, validationError)) {
        errorOut = validationError;
        return false;
    }
    *this = std::move(parsed);
    return true;
}

std::string BeamGraphAsset::to_json() const {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "{\"id\":\"" << json_escape(id) << "\",\"name\":\""
        << json_escape(name) << "\",\"version\":" << version
        << ",\"position\":" << emit_vec3(position)
        << ",\"rotation\":" << emit_quat(rotation)
        << ",\"mass\":" << mass
        << ",\"nodes\":[";
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (i) out << ",";
        out << emit_node(nodes[i]);
    }
    out << "],\"beams\":[";
    for (std::size_t i = 0; i < beams.size(); ++i) {
        if (i) out << ",";
        out << emit_beam(beams[i]);
    }
    out << "],\"wheels\":[";
    for (std::size_t i = 0; i < wheels.size(); ++i) {
        if (i) out << ",";
        out << emit_wheel(wheels[i]);
    }
    out << "],\"seats\":[";
    for (std::size_t i = 0; i < seats.size(); ++i) {
        if (i) out << ",";
        out << emit_seat(seats[i]);
    }
    out << "],\"power\":" << emit_power(power)
        << ",\"solver\":" << emit_solver(solver)
        << ",\"provider\":\"" << vehicle_provider_name(provider) << "\"}";
    return out.str();
}

bool BeamGraphAsset::validate(std::string& errorOut) const {
    return validate_asset(*this, errorOut);
}

}  // namespace vehicles
}  // namespace engine
