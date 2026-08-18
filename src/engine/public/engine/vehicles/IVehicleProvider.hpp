#pragma once

// Vehicle provider seam (FALTANTES §17 items 5 and 6): the PHYSICS provider
// behind a vehicle is an opt-in plugin selected by the ASSET, never inferred.
// The asset (VehicleAsset / BeamGraphAsset) carries a `provider` field
// (jolt|chrono|jsbsim); the vehicle factory routes through
// create_vehicle_provider, and the runtime refuses a provider that cannot
// simulate the vehicle (never a silent fallback — DEPENDENCY_POLICY §6).
//
// Today ONLY Jolt is vendored. Chrono (tires/terramechanics/multibody) and
// JSBSim (6DoF aircraft/rockets) are specialized opt-in plugins NOT vendored;
// create_vehicle_provider REFUSES them with a diagnostic, exactly like the
// FEMFX path in create_deformable_provider. Implementing one = writing a
// provider behind this seam; the contract does not change.
//
// Provider ownership (item 10) still applies: a vehicle created with a
// future Chrono/JSBSim provider would claim its chassis as that provider's
// name — the same "exactly one simulator per body" rule.

#include <cstdint>
#include <memory>
#include <string>

namespace engine {
namespace vehicles {

// The physics provider simulating a vehicle. Jolt is the vendored default
// (JPH::VehicleConstraint); Chrono and JSBSim are opt-in plugins not vendored
// (refused with a diagnostic by create_vehicle_provider).
enum class VehicleProviderKind : std::uint8_t {
    Jolt,     // default — vendored Jolt Vehicles adapter
    Chrono,   // opt-in plugin, NOT vendored (refused with a diagnostic)
    Jsbsim    // opt-in plugin, NOT vendored (refused with a diagnostic)
};

// Stable name of a provider kind (used by provider_of / claim_provider).
const char* vehicle_provider_name(VehicleProviderKind kind) noexcept;

// The vehicle provider surface: the kind + whether it can actually simulate
// vehicles in this build. A provider with `available() == false` is a
// declared-but-not-vendored plugin (Chrono/JSBSim): the factory refuses it.
class IVehicleProvider {
public:
    virtual ~IVehicleProvider() = default;

    virtual VehicleProviderKind kind() const noexcept = 0;
    virtual bool available() const noexcept = 0;
    virtual const char* description() const noexcept = 0;
};

// The factory: the ONLY way a vehicle asset picks its physics provider. Jolt
// returns a usable provider; Chrono/JSBSim are refused with a diagnostic
// (never a silent fallback — the caller must handle the refusal, and the
// vehicle factories do: an asset requesting an unavailable provider is
// refused at creation).
std::unique_ptr<IVehicleProvider> create_vehicle_provider(
    VehicleProviderKind kind, std::string& errorOut);

// Parses a provider name ("jolt" | "chrono" | "jsbsim"). Returns false for
// unknown names (validation mirrors the asset JSON all-or-nothing rule).
bool parse_vehicle_provider(const std::string& name,
                            VehicleProviderKind& outKind);

}  // namespace vehicles
}  // namespace engine
