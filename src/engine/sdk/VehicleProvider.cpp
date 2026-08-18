// VehicleProvider.cpp — the ONLY TU implementing the public vehicle provider
// seam (FALTANTES §17 items 5 and 6). The provider behind a vehicle is an
// opt-in plugin selected by the ASSET (`provider` field in VehicleAsset /
// BeamGraphAsset); the factory here is the single routing point.
//
// Today ONLY Jolt is vendored. Chrono and JSBSim are specialized opt-in
// plugins NOT vendored (DEPENDENCY_POLICY §6 — project-chrono is a vehicle/
// engineering plugin, never global authority); create_vehicle_provider
// REFUSES them with a diagnostic, exactly like the FEMFX path in
// create_deformable_provider. Never a silent fallback: an asset requesting an
// unavailable provider is refused at creation (the vehicle factories check
// available() before assembling).

#include "engine/vehicles/IVehicleProvider.hpp"

namespace engine {
namespace vehicles {

namespace {

class JoltVehicleProvider final : public IVehicleProvider {
public:
    VehicleProviderKind kind() const noexcept override {
        return VehicleProviderKind::Jolt;
    }
    bool available() const noexcept override { return true; }
    const char* description() const noexcept override {
        return "Jolt Vehicles adapter (vendored): Wheeled/Motorcycle/Tracked "
               "controllers via JPH::VehicleConstraint";
    }
};

}  // namespace

const char* vehicle_provider_name(VehicleProviderKind kind) noexcept {
    switch (kind) {
        case VehicleProviderKind::Jolt: return "jolt";
        case VehicleProviderKind::Chrono: return "chrono";
        case VehicleProviderKind::Jsbsim: return "jsbsim";
    }
    return "unknown";
}

bool parse_vehicle_provider(const std::string& name,
                            VehicleProviderKind& outKind) {
    if (name == "jolt") {
        outKind = VehicleProviderKind::Jolt;
        return true;
    }
    if (name == "chrono") {
        outKind = VehicleProviderKind::Chrono;
        return true;
    }
    if (name == "jsbsim") {
        outKind = VehicleProviderKind::Jsbsim;
        return true;
    }
    return false;
}

std::unique_ptr<IVehicleProvider> create_vehicle_provider(
    VehicleProviderKind kind, std::string& errorOut) {
    switch (kind) {
        case VehicleProviderKind::Jolt:
            return std::make_unique<JoltVehicleProvider>();
        case VehicleProviderKind::Chrono:
            errorOut =
                "vehicle provider: Chrono is a specialized opt-in plugin "
                "(tires/terramechanics/multibody) NOT vendored "
                "(DEPENDENCY_POLICY); implement the IVehicleProvider seam to "
                "add it — the asset never falls back silently";
            return nullptr;
        case VehicleProviderKind::Jsbsim:
            errorOut =
                "vehicle provider: JSBSim is a specialized opt-in plugin "
                "(6DoF aircraft/rockets) NOT vendored (DEPENDENCY_POLICY); "
                "implement the IVehicleProvider seam to add it — the asset "
                "never falls back silently";
            return nullptr;
    }
    errorOut = "vehicle provider: unknown provider kind";
    return nullptr;
}

}  // namespace vehicles
}  // namespace engine
