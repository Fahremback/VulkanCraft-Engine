// PhysicsOwnershipTests.cpp
//
// FALTANTES §17 item 10: each physical entity (body) is simulated by EXACTLY
// ONE provider at a time. The public IPhysicsWorld exposes all-or-nothing
// provider claims (claim_provider / release_provider / provider_of) with a
// diagnostic — claiming a body another provider owns is refused, so a game
// never silently ends up with two simulators on one entity. The vehicle
// factories claim the chassis automatically (create_vehicle /
// create_vehicle_from_asset claim "vehicle:jolt"), so a second vehicle on the
// same chassis body is refused. These tests prove: claim/release/owner,
// conflicts refused with diagnostics, idempotent same-provider re-claim,
// non-owning release refused, the vehicle factory exclusivity (second vehicle
// on the same body refused, first still owns it), the asset factory claim,
// release-then-reclaim, invalid bodies refused, and determinism.

#include "engine/gameplay/IGameplayRuntime.hpp"
#include "engine/vehicles/IVehicleAsset.hpp"

#include <glm/glm.hpp>

#include <cstdio>
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

engine::gameplay::BodyId make_dynamic_body(engine::gameplay::IGameplayRuntime& runtime) {
    using namespace engine::gameplay;
    BodySpec spec;
    spec.motion = MotionType::Dynamic;
    spec.shape = BoxShape{{0.5f, 0.5f, 0.5f}};
    spec.position = {0.0f, 5.0f, 0.0f};
    return runtime.physics().create_body(spec);
}

engine::vehicles::VehicleAsset make_car_asset() {
    using namespace engine::vehicles;
    VehicleAsset asset;
    asset.name = "ownership-car";
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

// --- 1. Claim / release / owner ------------------------------------------------

void test_claim_release() {
    using namespace engine::gameplay;
    auto runtime = create_gameplay_runtime(PhysicsBackend::Jolt);
    BodyId body = make_dynamic_body(*runtime);

    std::string error;
    check(runtime->physics().provider_of(body).empty(),
          "claim: fresh body has no provider");
    check(runtime->physics().claim_provider(body, "ragdoll", error),
          "claim: first provider claims");
    check(runtime->physics().provider_of(body) == "ragdoll",
          "claim: provider_of reports the owner");
    check(!runtime->physics().claim_provider(body, "vehicle:jolt", error) &&
              !error.empty(),
          "claim: second provider refused with a diagnostic");
    check(runtime->physics().provider_of(body) == "ragdoll",
          "claim: refused claim leaves the owner unchanged");
    // Idempotent same-provider re-claim.
    check(runtime->physics().claim_provider(body, "ragdoll", error),
          "claim: same provider re-claim is idempotent");
    // Non-owning release refused; owning release works.
    check(!runtime->physics().release_provider(body, "vehicle:jolt"),
          "claim: non-owning release refused");
    check(runtime->physics().release_provider(body, "ragdoll"),
          "claim: owning release succeeds");
    check(runtime->physics().provider_of(body).empty(),
          "claim: body unclaimed after release");
    std::printf("[physics-ownership] claim/release/owner OK\n");
}

// --- 2. Invalid bodies refused -------------------------------------------------

void test_invalid_body() {
    using namespace engine::gameplay;
    auto runtime = create_gameplay_runtime(PhysicsBackend::Jolt);
    std::string error;
    check(!runtime->physics().claim_provider(BodyId{0}, "ragdoll", error) &&
              !error.empty(),
          "invalid: zero body id refused");
    check(!runtime->physics().claim_provider(BodyId{9999}, "ragdoll", error) &&
              !error.empty(),
          "invalid: unknown body refused");
    check(!runtime->physics().claim_provider(make_dynamic_body(*runtime), "",
                                              error) &&
              !error.empty(),
          "invalid: empty provider name refused");
    std::printf("[physics-ownership] invalid claims refused OK\n");
}

// --- 3. Vehicle factory exclusivity --------------------------------------------

void test_vehicle_exclusive() {
    using namespace engine::gameplay;
    auto runtime = create_gameplay_runtime(PhysicsBackend::Jolt);
    BodyId chassis = make_dynamic_body(*runtime);

    std::vector<WheelSpec> wheels;
    for (int i = 0; i < 4; ++i) {
        WheelSpec wheel;
        wheel.localPosition = {0.0f, -0.2f, i < 2 ? -1.0f : 1.0f};
        wheel.driven = true;
        wheels.push_back(wheel);
    }
    auto vehicle = runtime->create_vehicle(chassis, wheels);
    check(vehicle != nullptr && vehicle->valid(), "factory: first vehicle created");
    check(runtime->physics().provider_of(chassis) == "vehicle:jolt",
          "factory: chassis claimed by the vehicle provider");

    // A SECOND vehicle on the same chassis body is refused.
    auto second = runtime->create_vehicle(chassis, wheels);
    check(second == nullptr, "factory: second vehicle on the same body refused");
    check(runtime->physics().provider_of(chassis) == "vehicle:jolt",
          "factory: refusal leaves the first claim intact");

    // A manual claim from another provider is also refused while the vehicle
    // owns the body.
    std::string error;
    check(!runtime->physics().claim_provider(chassis, "ragdoll", error) &&
              !error.empty(),
          "factory: manual foreign claim refused on a vehicle chassis");
    std::printf("[physics-ownership] vehicle factory exclusivity OK\n");
}

// --- 4. Asset factory claims the assembled chassis -----------------------------

void test_asset_factory_claim() {
    using namespace engine::gameplay;
    auto runtime = create_gameplay_runtime(PhysicsBackend::Jolt);
    engine::vehicles::VehicleAsset asset = make_car_asset();
    auto vehicle = runtime->create_vehicle_from_asset(asset);
    check(vehicle != nullptr && vehicle->valid(), "asset: vehicle assembled");
    check(runtime->physics().provider_of(vehicle->chassis()) == "vehicle:jolt",
          "asset: assembled chassis claimed by the vehicle provider");

    // Releasing the claim then claiming with another provider works (the game
    // consciously hands the entity over).
    check(runtime->physics().release_provider(vehicle->chassis(), "vehicle:jolt"),
          "asset: vehicle releases its chassis");
    std::string error;
    check(runtime->physics().claim_provider(vehicle->chassis(), "ragdoll", error),
          "asset: another provider can claim after release");
    std::printf("[physics-ownership] asset factory claim OK\n");
}

// --- 5. Determinism ------------------------------------------------------------

void test_determinism() {
    using namespace engine::gameplay;
    auto run = [&](std::string& ownerA, std::string& ownerB) {
        auto runtime = create_gameplay_runtime(PhysicsBackend::Jolt);
        BodyId body = make_dynamic_body(*runtime);
        std::string error;
        runtime->physics().claim_provider(body, "ragdoll", error);
        ownerA = runtime->physics().provider_of(body);
        bool refused = runtime->physics().claim_provider(body, "vehicle:jolt", error);
        ownerB = runtime->physics().provider_of(body);
        check(!refused, "determinism: foreign claim refused");
    };
    std::string a1, a2, b1, b2;
    run(a1, a2);
    run(b1, b2);
    check(a1 == b1 && a2 == b2, "determinism: claims identical across runs");
    std::printf("[physics-ownership] determinism OK\n");
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    test_claim_release();
    test_invalid_body();
    test_vehicle_exclusive();
    test_asset_factory_claim();
    test_determinism();
    if (g_failures == 0) {
        std::printf("[physics-ownership] ALL PASSED\n");
        return 0;
    }
    std::printf("[physics-ownership] %d FAILURE(S)\n", g_failures);
    return 1;
}
