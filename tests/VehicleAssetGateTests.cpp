// VehicleAssetGateTests — equivalence gate for the MCP vehicle-assembly
// surface (FALTANTES §17 item 12). The MCP semantic layer
// (tools/mcp-server/game-authoring.mjs) authors versioned JSON vehicle
// assemblies under Content/Vehicles/<name>.json via author_vehicle_asset
// (kinds: vehicle = rigid VehicleAsset, beam = node/beam BeamGraphAsset); the
// JS validator mirrors the public contracts' rules. This gate mirrors the
// EXACT documents the MCP emits (see tools/mcp-server/protocol-smoke.mjs —
// the "Pickup" and "SoftCrawler" payloads) and proves they load through the
// PUBLIC factories unchanged: VehicleAsset::load_from_json and
// BeamGraphAsset::load_from_json (engine/vehicles/IVehicleAsset.hpp /
// IBeamGraphAsset.hpp, implemented by the SDK adapters).
//
// The test TU compiles against ONLY the public headers (src/engine/public)
// plus glm, like mcp_registry_gate_tests — the stand-in for an external
// project. The engine implementation comes from the vc_sdk_public OBJECT
// module (VehicleAsset.cpp / BeamGraphAsset.cpp / RegistryJson.cpp).
//
// No UI, no GPU. Build/run like the other engine tests (standalone main()
// with CHECK).

#include "engine/vehicles/IBeamGraphAsset.hpp"
#include "engine/vehicles/IVehicleAsset.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "VehicleAssetGateTests failure at line " << __LINE__ << ": " #condition "\n"; \
    return false; } } while (false)

namespace {

// ---------------------------------------------------------------------------
// Documents exactly as authored by the MCP smoke test. Keep these strings in
// sync with tools/mcp-server/protocol-smoke.mjs (author_vehicle_asset calls
// with the same args build these documents via buildVehicleDocument).
// ---------------------------------------------------------------------------

const char* kVehicleDocument =
    "{\"name\":\"Pickup\",\"version\":1,\"kind\":\"wheeled\","
    "\"position\":[0,0.5,0],\"rotation\":[0,0,0,1],"
    "\"chassis\":{\"shape\":\"box\",\"halfExtents\":[1.1,0.4,0.65],"
    "\"radius\":0.5,\"halfHeight\":0.65,\"mass\":1500,\"friction\":0.6,"
    "\"restitution\":0.05},"
    "\"wheels\":["
    "{\"localPosition\":[0.95,0,0.7],\"radius\":0.38,\"suspensionRestLength\":0.5,"
    "\"suspensionTravel\":0.18,\"springStrength\":26000,\"damperStrength\":3200,"
    "\"tireGrip\":1.35,\"maxDriveForce\":4200,\"maxBrakeForce\":6000,"
    "\"maxSteerAngle\":0.5,\"steering\":true,\"driven\":true},"
    "{\"localPosition\":[-0.95,0,0.7],\"radius\":0.38,\"suspensionRestLength\":0.5,"
    "\"suspensionTravel\":0.18,\"springStrength\":26000,\"damperStrength\":3200,"
    "\"tireGrip\":1.35,\"maxDriveForce\":4200,\"maxBrakeForce\":6000,"
    "\"maxSteerAngle\":0.5,\"steering\":true,\"driven\":true},"
    "{\"localPosition\":[0.95,0,-0.7],\"radius\":0.38,\"suspensionRestLength\":0.5,"
    "\"suspensionTravel\":0.18,\"springStrength\":26000,\"damperStrength\":3200,"
    "\"tireGrip\":1.35,\"maxDriveForce\":4200,\"maxBrakeForce\":6000,"
    "\"maxSteerAngle\":0.55,\"steering\":false,\"driven\":true},"
    "{\"localPosition\":[-0.95,0,-0.7],\"radius\":0.38,\"suspensionRestLength\":0.5,"
    "\"suspensionTravel\":0.18,\"springStrength\":26000,\"damperStrength\":3200,"
    "\"tireGrip\":1.35,\"maxDriveForce\":4200,\"maxBrakeForce\":6000,"
    "\"maxSteerAngle\":0.55,\"steering\":false,\"driven\":true}],"
    "\"drivetrain\":{\"engineMaxTorque\":1800,\"engineMinRPM\":900,"
    "\"engineMaxRPM\":6500,\"differentialRatio\":3.7,"
    "\"gearRatios\":[3.2,2.1,1.5,1.1,0.85]},"
    "\"propulsion\":[],"
    "\"seats\":[{\"name\":\"driver\",\"localPosition\":[0.2,0.5,0.3],"
    "\"exitOffset\":[0,1.1,1.2]}],"
    "\"power\":{"
    "\"fuel\":{\"capacity\":60,\"initialLevel\":0.8,\"burnPerSecond\":0.12,"
    "\"idleBurnPerSecond\":0,\"minLevelToRun\":0.05},"
    "\"energy\":{\"capacity\":0,\"initialCharge\":1,\"drawPerSecond\":0,"
    "\"regenPerSecond\":0,\"minChargeToRun\":0},"
    "\"controls\":{\"throttleDeadzone\":0.05,\"throttleSensitivity\":1.2,"
    "\"throttleInvert\":false,\"steeringDeadzone\":0,\"steeringSensitivity\":1,"
    "\"steeringInvert\":false,\"brakeDeadzone\":0,\"brakeSensitivity\":1}}}";

const char* kBeamDocument =
    "{\"name\":\"SoftCrawler\",\"version\":1,"
    "\"position\":[0,0,0],\"rotation\":[0,0,0,1],\"mass\":900,"
    "\"nodes\":["
    "{\"position\":[1.2,0,0.8],\"fixed\":false},"
    "{\"position\":[1.2,0,-0.8],\"fixed\":false},"
    "{\"position\":[-1.2,0,0.8],\"fixed\":false},"
    "{\"position\":[-1.2,0,-0.8],\"fixed\":false},"
    "{\"position\":[0,1,0],\"fixed\":true}],"
    "\"beams\":["
    "{\"a\":0,\"b\":1,\"stiffness\":0.95},"
    "{\"a\":1,\"b\":3,\"stiffness\":0.9},"
    "{\"a\":3,\"b\":2,\"stiffness\":0.95},"
    "{\"a\":2,\"b\":0,\"stiffness\":0.9},"
    "{\"a\":0,\"b\":4,\"stiffness\":0.9},"
    "{\"a\":1,\"b\":4,\"stiffness\":0.9},"
    "{\"a\":2,\"b\":4,\"stiffness\":0.9},"
    "{\"a\":3,\"b\":4,\"stiffness\":0.9}],"
    "\"wheels\":[{\"node\":0,\"steering\":true,\"driven\":true,"
    "\"wheel\":{\"localPosition\":[0,0,0],\"radius\":0.4,"
    "\"suspensionRestLength\":0.5,\"suspensionTravel\":0.18,"
    "\"springStrength\":26000,\"damperStrength\":3200,\"tireGrip\":1.35,"
    "\"maxDriveForce\":3200,\"maxBrakeForce\":6000,\"maxSteerAngle\":0.55,"
    "\"steering\":false,\"driven\":true}}],"
    "\"seats\":[],"
    "\"power\":{"
    "\"fuel\":{\"capacity\":0,\"initialLevel\":1,\"burnPerSecond\":0.05,"
    "\"idleBurnPerSecond\":0,\"minLevelToRun\":0},"
    "\"energy\":{\"capacity\":0,\"initialCharge\":1,\"drawPerSecond\":0,"
    "\"regenPerSecond\":0,\"minChargeToRun\":0},"
    "\"controls\":{\"throttleDeadzone\":0,\"throttleSensitivity\":1,"
    "\"throttleInvert\":false,\"steeringDeadzone\":0,\"steeringSensitivity\":1,"
    "\"steeringInvert\":false,\"brakeDeadzone\":0,\"brakeSensitivity\":1}},"
    "\"solver\":{\"substeps\":3,\"solverIterations\":12,\"stiffness\":0.85,"
    "\"damping\":0.15,\"gravity\":[0,-9.81,0]}}";

// The refusal cases the MCP mirror rejects — the C++ factories must agree
// (all-or-nothing: malformed documents are refused with a diagnostic, never
// clamped or partially applied).
const char* kBadVehicleWheelDocument =
    "{\"name\":\"BrokenWheel\",\"version\":1,\"kind\":\"wheeled\","
    "\"position\":[0,0,0],\"rotation\":[0,0,0,1],"
    "\"chassis\":{\"shape\":\"box\",\"halfExtents\":[1,0.4,0.6],"
    "\"radius\":0.5,\"halfHeight\":0.65,\"mass\":1000,\"friction\":0.55,"
    "\"restitution\":0.05},"
    "\"wheels\":[{\"localPosition\":[0,0,0],\"radius\":0,"
    "\"suspensionRestLength\":0.45,\"suspensionTravel\":0.18,"
    "\"springStrength\":26000,\"damperStrength\":3200,\"tireGrip\":1.35,"
    "\"maxDriveForce\":4200,\"maxBrakeForce\":6000,\"maxSteerAngle\":0.55,"
    "\"steering\":false,\"driven\":true}],"
    "\"drivetrain\":{\"engineMaxTorque\":0,\"engineMinRPM\":1000,"
    "\"engineMaxRPM\":6000,\"differentialRatio\":3.42,"
    "\"gearRatios\":[2.66]}}";

const char* kBadBeamDocument =
    "{\"name\":\"BrokenBeam\",\"version\":1,"
    "\"position\":[0,0,0],\"rotation\":[0,0,0,1],\"mass\":1200,"
    "\"nodes\":[{\"position\":[0,0,0],\"fixed\":false}],"
    "\"beams\":[{\"a\":0,\"b\":7,\"stiffness\":0.9}],"
    "\"solver\":{\"substeps\":2,\"solverIterations\":8,\"stiffness\":0.8,"
    "\"damping\":0.1,\"gravity\":[0,-9.81,0]}}";

bool test_vehicle_gate() {
    engine::vehicles::VehicleAsset asset;
    std::string error;
    CHECK(asset.load_from_json(kVehicleDocument, error));
    CHECK(error.empty());
    CHECK(asset.name == "Pickup");
    CHECK(asset.version == 1);
    CHECK(asset.kind == engine::vehicles::VehicleKind::Wheeled);
    CHECK(asset.wheels.size() == 4);
    CHECK(asset.chassis.mass == 1500.0f);
    CHECK(asset.chassis.shape == engine::vehicles::ChassisShape::Box);
    CHECK(asset.drivetrain.engineMaxTorque == 1800.0f);
    CHECK(asset.drivetrain.gearRatios.size() == 5);
    CHECK(std::fabs(asset.drivetrain.gearRatios[0] - 3.2f) < 1.0e-4f);
    CHECK(asset.seats.size() == 1);
    CHECK(asset.seats[0].name == "driver");
    CHECK(asset.power.fuel.capacity == 60.0f);
    CHECK(std::fabs(asset.power.fuel.initialLevel - 0.8f) < 1.0e-6f);
    CHECK(std::fabs(asset.power.controls.throttleSensitivity - 1.2f) < 1.0e-6f);
    // The front wheels steer; the rears do not.
    CHECK(asset.wheels[0].steering);
    CHECK(asset.wheels[2].steering == false);
    CHECK(asset.wheels[2].maxSteerAngle == 0.55f);

    // Bit-exact round trip: to_json() -> load_from_json() is stable.
    const std::string emitted = asset.to_json();
    engine::vehicles::VehicleAsset reloaded;
    std::string roundTripError;
    CHECK(reloaded.load_from_json(emitted, roundTripError));
    CHECK(roundTripError.empty());
    CHECK(reloaded.to_json() == emitted);

    std::cout << "[vehicle-gate] authored vehicle document loads through the "
                 "public factory; round-trip bit-exact OK\n";
    return true;
}

bool test_beam_gate() {
    engine::vehicles::BeamGraphAsset asset;
    std::string error;
    CHECK(asset.load_from_json(kBeamDocument, error));
    CHECK(error.empty());
    CHECK(asset.name == "SoftCrawler");
    CHECK(asset.version == 1);
    CHECK(asset.mass == 900.0f);
    CHECK(asset.nodes.size() == 5);
    CHECK(asset.nodes[4].fixed);
    CHECK(asset.beams.size() == 8);
    CHECK(asset.beams[0].a == 0 && asset.beams[0].b == 1);
    CHECK(std::fabs(asset.beams[0].stiffness - 0.95f) < 1.0e-6f);
    CHECK(asset.wheels.size() == 1);
    CHECK(asset.wheels[0].node == 0);
    CHECK(asset.wheels[0].steering);
    CHECK(asset.wheels[0].wheel.maxDriveForce == 3200.0f);
    CHECK(asset.solver.substeps == 3);
    CHECK(asset.solver.solverIterations == 12);
    CHECK(std::fabs(asset.solver.gravity.y - (-9.81f)) < 1.0e-6f);

    // Bit-exact round trip: to_json() -> load_from_json() is stable.
    const std::string emitted = asset.to_json();
    engine::vehicles::BeamGraphAsset reloaded;
    std::string roundTripError;
    CHECK(reloaded.load_from_json(emitted, roundTripError));
    CHECK(roundTripError.empty());
    CHECK(reloaded.to_json() == emitted);

    std::cout << "[vehicle-gate] authored beam document loads through the "
                 "public factory; round-trip bit-exact OK\n";
    return true;
}

bool test_refusal_gate() {
    // A wheel with radius <= 0 is refused with a diagnostic (all-or-nothing):
    // the C++ factory agrees with the MCP mirror.
    engine::vehicles::VehicleAsset vehicle;
    std::string vehicleError;
    CHECK(!vehicle.load_from_json(kBadVehicleWheelDocument, vehicleError));
    CHECK(!vehicleError.empty());

    // A beam referencing an invalid node is refused.
    engine::vehicles::BeamGraphAsset beam;
    std::string beamError;
    CHECK(!beam.load_from_json(kBadBeamDocument, beamError));
    CHECK(!beamError.empty());

    // Malformed JSON is refused with a diagnostic; the object stays usable
    // (a subsequent valid load succeeds — all-or-nothing, no partial state).
    engine::vehicles::VehicleAsset bad;
    std::string badError;
    CHECK(!bad.load_from_json("{not json", badError));
    CHECK(!badError.empty());
    std::string cleanError;
    CHECK(bad.load_from_json(kVehicleDocument, cleanError));
    CHECK(cleanError.empty());

    std::cout << "[vehicle-gate] refusal cases agree with the MCP mirror "
                 "(all-or-nothing) OK\n";
    return true;
}

bool run_all() {
    if (!test_vehicle_gate()) return false;
    if (!test_beam_gate()) return false;
    if (!test_refusal_gate()) return false;
    return true;
}

}  // namespace

int main() {
    if (run_all()) {
        std::cout << "VehicleAssetGateTests: all gates passed\n";
        return 0;
    }
    return 1;
}
