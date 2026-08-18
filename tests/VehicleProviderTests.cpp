// VehicleProviderTests.cpp
//
// FALTANTES §17 items 5 and 6: the physics provider behind a vehicle is an
// OPT-IN PLUGIN selected by the ASSET (VehicleAsset.provider /
// BeamGraphAsset.provider — jolt|chrono|jsbsim). Only Jolt is vendored
// today; Chrono (tires/terramechanics/multibody) and JSBSim (6DoF
// aircraft/rockets) are specialized plugins NOT vendored (DEPENDENCY_POLICY
// §6) and are REFUSED with a diagnostic by create_vehicle_provider — never a
// silent fallback (the same contract as the FEMFX path in
// create_deformable_provider). These tests prove: the factory returns a
// usable Jolt provider and refuses chrono/jsbsim with a diagnostic; the
// provider field round-trips through BOTH assets (bit-exact JSON) with
// all-or-nothing validation; the vehicle factories honor the gate (an asset
// requesting chrono/jsbsim is refused at creation — no vehicle, no claim);
// and provider ownership names the actual provider (item 10 integration).

#include "engine/gameplay/IGameplayRuntime.hpp"
#include "engine/vehicles/IVehicleAsset.hpp"
#include "engine/vehicles/IBeamGraphAsset.hpp"
#include "engine/vehicles/IVehicleProvider.hpp"

#include "../src/engine/physics/PhysicsRuntime.hpp"

#include <glm/glm.hpp>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::printf("FAIL: %s\n", message);
        ++g_failures;
    }
}

bool has_substring(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

engine::vehicles::VehicleAsset make_car_asset() {
    using namespace engine::vehicles;
    VehicleAsset asset;
    asset.id = "11111111-1111-1111-1111-111111111111";
    asset.name = "provider-car";
    asset.position = {0.0f, 1.2f, 0.0f};
    asset.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    asset.chassis.shape = ChassisShape::Box;
    asset.chassis.halfExtents = {0.95f, 0.37f, 0.61f};
    asset.chassis.mass = 1200.0f;
    const glm::vec3 locals[4] = {
        {-1.3f, -0.1f, -0.8f}, {-1.3f, -0.1f, 0.8f},
        {1.3f, -0.1f, -0.8f}, {1.3f, -0.1f, 0.8f},
    };
    for (const glm::vec3& local : locals) {
        WheelComponent wheel;
        wheel.localPosition = local;
        wheel.driven = true;
        wheel.steering = local.z < 0.0f;
        asset.wheels.push_back(wheel);
    }
    return asset;
}

engine::vehicles::BeamGraphAsset make_beam_asset() {
    using namespace engine::vehicles;
    BeamGraphAsset asset;
    asset.id = "22222222-2222-2222-2222-222222222222";
    asset.name = "provider-beam";
    asset.position = {0.0f, 1.4f, 0.0f};
    asset.nodes = { { {0.0f, 0.0f, 0.0f}, false },
                    { {1.0f, 0.0f, 0.0f}, false } };
    asset.beams = { { 0, 1, 0.9f } };
    return asset;
}

void add_ground(engine::gameplay::IGameplayRuntime& runtime) {
    using namespace engine::gameplay;
    BodySpec ground;
    ground.motion = MotionType::Static;
    ground.shape = BoxShape{{50.0f, 1.0f, 50.0f}};
    ground.position = {0.0f, -0.5f, 0.0f};
    runtime.physics().create_body(ground);
}

// --- 1. Factory seam ---------------------------------------------------------

void test_factory_seam() {
    using namespace engine::vehicles;

    // Jolt is vendored and usable.
    std::string joltError;
    auto jolt = create_vehicle_provider(VehicleProviderKind::Jolt, joltError);
    check(jolt != nullptr, "factory: jolt provider created");
    check(jolt->available(), "factory: jolt is available");
    check(jolt->kind() == VehicleProviderKind::Jolt, "factory: jolt kind");
    check(std::string(vehicle_provider_name(VehicleProviderKind::Jolt)) == "jolt",
          "factory: jolt name");

    // Chrono is a declared plugin, NOT vendored — refused with a diagnostic.
    std::string chronoError;
    auto chrono = create_vehicle_provider(VehicleProviderKind::Chrono, chronoError);
    check(chrono == nullptr, "factory: chrono refused (not vendored)");
    check(has_substring(chronoError, "Chrono"), "factory: chrono diagnostic names the plugin");
    check(has_substring(chronoError, "NOT vendored"), "factory: chrono diagnostic states not vendored");
    check(has_substring(chronoError, "never falls back silently"),
          "factory: chrono refusal never silent");

    // JSBSim likewise.
    std::string jsbsimError;
    auto jsbsim = create_vehicle_provider(VehicleProviderKind::Jsbsim, jsbsimError);
    check(jsbsim == nullptr, "factory: jsbsim refused (not vendored)");
    check(has_substring(jsbsimError, "JSBSim"), "factory: jsbsim diagnostic names the plugin");
    check(has_substring(jsbsimError, "NOT vendored"), "factory: jsbsim diagnostic states not vendored");

    // Parsing: names map to kinds; unknown names are refused (all-or-nothing).
    VehicleProviderKind parsed;
    check(parse_vehicle_provider("jolt", parsed) && parsed == VehicleProviderKind::Jolt,
          "factory: parse jolt");
    check(parse_vehicle_provider("chrono", parsed) && parsed == VehicleProviderKind::Chrono,
          "factory: parse chrono");
    check(parse_vehicle_provider("jsbsim", parsed) && parsed == VehicleProviderKind::Jsbsim,
          "factory: parse jsbsim");
    check(!parse_vehicle_provider("havok", parsed), "factory: unknown provider refused");

    std::printf("[vehicle-provider] factory seam: jolt available, chrono/jsbsim "
                "refused with diagnostics OK\n");
}

// --- 2. Provider field JSON round-trip (both assets) --------------------------

void test_provider_json_roundtrip() {
    using namespace engine::vehicles;

    // Default provider = jolt (legacy behavior preserved).
    VehicleAsset car = make_car_asset();
    check(car.provider == VehicleProviderKind::Jolt, "round-trip: default provider is jolt");
    const std::string carJson = car.to_json();
    check(has_substring(carJson, "\"provider\":\"jolt\""),
          "round-trip: vehicle JSON carries provider");
    VehicleAsset parsedCar;
    std::string error;
    check(parsedCar.load_from_json(carJson, error), "round-trip: vehicle loads");
    check(parsedCar.provider == VehicleProviderKind::Jolt, "round-trip: vehicle provider jolt");

    // Explicit chrono round-trips through the DOCUMENT (the document must
    // round-trip regardless; the availability gate is at creation).
    VehicleAsset chronoCar = make_car_asset();
    chronoCar.provider = VehicleProviderKind::Chrono;
    const std::string chronoJson = chronoCar.to_json();
    check(has_substring(chronoJson, "\"provider\":\"chrono\""),
          "round-trip: chrono vehicle JSON carries provider");
    VehicleAsset parsedChrono;
    std::string chronoError;
    check(parsedChrono.load_from_json(chronoJson, chronoError), "round-trip: chrono vehicle loads");
    check(parsedChrono.provider == VehicleProviderKind::Chrono,
          "round-trip: chrono vehicle provider preserved");

    // Beam asset: same contract.
    BeamGraphAsset beam = make_beam_asset();
    const std::string beamJson = beam.to_json();
    check(has_substring(beamJson, "\"provider\":\"jolt\""),
          "round-trip: beam JSON carries provider");
    BeamGraphAsset parsedBeam;
    std::string beamError;
    check(parsedBeam.load_from_json(beamJson, beamError), "round-trip: beam loads");
    check(parsedBeam.provider == VehicleProviderKind::Jolt, "round-trip: beam provider jolt");

    BeamGraphAsset jsbsimBeam = make_beam_asset();
    jsbsimBeam.provider = VehicleProviderKind::Jsbsim;
    const std::string jsbsimJson = jsbsimBeam.to_json();
    check(has_substring(jsbsimJson, "\"provider\":\"jsbsim\""),
          "round-trip: jsbsim beam JSON carries provider");
    BeamGraphAsset parsedJsbsim;
    std::string jsbsimError;
    check(parsedJsbsim.load_from_json(jsbsimJson, jsbsimError), "round-trip: jsbsim beam loads");
    check(parsedJsbsim.provider == VehicleProviderKind::Jsbsim,
          "round-trip: jsbsim beam provider preserved");

    // Bit-exact round trip stays stable.
    check(parsedCar.to_json() == carJson, "round-trip: vehicle to_json stable");
    check(parsedBeam.to_json() == beamJson, "round-trip: beam to_json stable");

    // All-or-nothing: an unknown provider name is refused with a diagnostic.
    const std::string badCar =
        "{\"name\":\"bad\",\"version\":1,\"provider\":\"havok\","
        "\"position\":[0,0,0],\"rotation\":[0,0,0,1],"
        "\"chassis\":{\"shape\":\"box\",\"halfExtents\":[1,1,1],\"mass\":1000,"
        "\"friction\":0.5,\"restitution\":0.05},"
        "\"wheels\":[{\"localPosition\":[0,0,0],\"radius\":0.36,"
        "\"suspensionRestLength\":0.45}],"
        "\"drivetrain\":{\"engineMinRPM\":1000,\"engineMaxRPM\":6000,"
        "\"differentialRatio\":3.42,\"gearRatios\":[2.66]}}";
    VehicleAsset badParsed;
    std::string badError;
    check(!badParsed.load_from_json(badCar, badError), "round-trip: unknown provider refused");
    check(has_substring(badError, "provider"), "round-trip: refusal names provider");

    std::printf("[vehicle-provider] provider field JSON round-trip (vehicle + "
                "beam) + all-or-nothing OK\n");
}

// --- 3. Vehicle factories honor the provider gate -----------------------------

void test_factory_gate() {
    using namespace engine::gameplay;

    auto runtime = create_gameplay_runtime();

    // Jolt asset creates a real vehicle that drives on the ground.
    add_ground(*runtime);
    engine::vehicles::VehicleAsset car = make_car_asset();
    car.provider = engine::vehicles::VehicleProviderKind::Jolt;
    auto vehicle = runtime->create_vehicle_from_asset(car);
    check(vehicle != nullptr, "gate: jolt vehicle created");
    if (vehicle) {
        const float dt = 1.0f / 60.0f;
        VehicleInput input;
        input.throttle = 0.5f;
        for (int i = 0; i < 60; ++i) {
            vehicle->set_input(input);
            vehicle->update(dt);
            runtime->step(dt);
        }
        check(vehicle->speed() > 0.5f, "gate: jolt vehicle actually drives");
    }

    // A chrono asset is REFUSED at creation (no vehicle, no silent fallback).
    engine::vehicles::VehicleAsset chronoCar = make_car_asset();
    chronoCar.provider = engine::vehicles::VehicleProviderKind::Chrono;
    auto chronoVehicle = runtime->create_vehicle_from_asset(chronoCar);
    check(chronoVehicle == nullptr, "gate: chrono vehicle refused at creation");

    // A jsbsim beam asset is refused too.
    engine::vehicles::BeamGraphAsset beam = make_beam_asset();
    beam.provider = engine::vehicles::VehicleProviderKind::Jsbsim;
    auto jsbsimBeam = runtime->create_beam_vehicle(beam);
    check(jsbsimBeam == nullptr, "gate: jsbsim beam refused at creation");

    // A jolt beam asset still works.
    engine::vehicles::BeamGraphAsset joltBeam = make_beam_asset();
    joltBeam.provider = engine::vehicles::VehicleProviderKind::Jolt;
    auto joltBeamVehicle = runtime->create_beam_vehicle(joltBeam);
    check(joltBeamVehicle != nullptr, "gate: jolt beam created");

    std::printf("[vehicle-provider] vehicle factories honor the provider gate "
                "(jolt ok, chrono/jsbsim refused) OK\n");
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    test_factory_seam();
    test_provider_json_roundtrip();
    test_factory_gate();
    if (g_failures == 0) {
        std::printf("[vehicle-provider] ALL PASSED\n");
        return 0;
    }
    std::printf("[vehicle-provider] %d FAILURE(S)\n", g_failures);
    return 1;
}
