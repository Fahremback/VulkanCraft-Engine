#include "PhysicsBackend.hpp"

#include "BulletPhysicsBackend.hpp"
#include "JoltPhysicsBackend.hpp"

namespace Engine::Physics {

PhysicsBackendKind backend_kind_from_string(const std::string& name) {
    if (name == "jolt") return PhysicsBackendKind::Jolt;
    if (name == "bullet") return PhysicsBackendKind::Bullet;
    return PhysicsBackendKind::Builtin;
}

std::string backend_kind_to_string(PhysicsBackendKind kind) {
    switch (kind) {
        case PhysicsBackendKind::Jolt: return "jolt";
        case PhysicsBackendKind::Bullet: return "bullet";
        case PhysicsBackendKind::Builtin: break;
    }
    return "builtin";
}

std::unique_ptr<PhysicsBackend> create_backend(PhysicsBackendKind kind, const WorldSettings& settings) {
    switch (kind) {
        case PhysicsBackendKind::Jolt: return std::make_unique<JoltPhysicsBackend>(settings);
        case PhysicsBackendKind::Bullet: return std::make_unique<BulletPhysicsBackend>(settings);
        case PhysicsBackendKind::Builtin: break;
    }
    return nullptr;
}

} // namespace Engine::Physics
