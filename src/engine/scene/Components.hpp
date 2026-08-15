#pragma once

#include <glm/glm.hpp>
#include <string>
#include <mutex>
#include "../core/uuid/UUID.hpp"
#include "../core/reflection/Reflection.hpp"

namespace Engine {

struct TransformComponent {
    glm::vec3 position{ 0.0f, 0.0f, 0.0f };
    glm::vec3 rotation{ 0.0f, 0.0f, 0.0f }; // Euler angles in degrees
    glm::vec3 scale{ 1.0f, 1.0f, 1.0f };
};

struct MeshRendererComponent {
    UUID meshAssetID;
    UUID materialAssetID;
    bool isVisible{ true };
    bool castShadows{ true };
};

// Explicit light type. Directional keeps the legacy range>=50 heuristic so
// scenes authored before the type field behave exactly as before (a
// Directional light with range < 50 falls back to point-light behavior in the
// renderers' light collection). Spot/Area are honored explicitly by the
// LightParams UBO writers (game + editor).
enum class LightType : uint8_t {
    Directional = 0,
    Point = 1,
    Spot = 2,
    Area = 3,
};

struct LightComponent {
    glm::vec3 color{ 1.0f, 1.0f, 1.0f };
    float intensity{ 1000.0f };
    float range{ 50.0f };
    bool castShadows{ true };
    LightType type{ LightType::Directional };
};

// True when the light acts as the directional sun (drives the shadow map and
// the volumetric shafts): an explicit Directional type with range >= 50 (the
// legacy heuristic). Never true for Spot/Area.
inline bool is_directional_sun(const LightComponent& light) noexcept {
    return light.type == LightType::Directional && light.range >= 50.0f;
}

struct CameraComponent {
    float fov{ 70.0f };
    float nearPlane{ 0.1f };
    float farPlane{ 1000.0f };
    bool isPrimary{ true };
};

struct RigidbodyComponent {
    float mass{ 1.0f };
    float friction{ 0.5f };
    float restitution{ 0.1f };
    bool isKinematic{ false };
    bool useGravity{ true };
};

// Hitscan weapon authored in the Weapon panel (Fase 8): the play world
// instantiates a WeaponRuntime per entity with these parameters and fires
// with the camera ray against the play physics.
struct WeaponComponent {
    float damage{ 25.0f };
    float roundsPerMinute{ 600.0f };
    uint32_t magazineSize{ 30 };
    uint32_t reserveAmmo{ 90 };
    bool automatic{ true };
    float spreadDegrees{ 1.5f };
    bool hitscan{ true };
};

// Particle emitter authored in the Particle panel (Fase 8): mirrors
// Gameplay::ParticleEmitterDesc; the play world instantiates a
// ParticleSimulation emitter per entity at the world transform.
struct ParticleEmitterComponent {
    glm::vec3 position{ 0.0f, 0.0f, 0.0f };   // local offset from entity origin
    glm::vec3 direction{ 0.0f, 1.0f, 0.0f };
    float coneAngle{ 0.4f };
    float rate{ 20.0f };
    float speedMin{ 1.0f };
    float speedMax{ 3.0f };
    float lifetimeMin{ 0.5f };
    float lifetimeMax{ 1.5f };
    float sizeStart{ 0.12f };
    float sizeEnd{ 0.0f };
    glm::vec4 colorStart{ 1.0f, 1.0f, 1.0f, 1.0f };
    glm::vec4 colorEnd{ 1.0f, 1.0f, 1.0f, 0.0f };
    glm::vec3 acceleration{ 0.0f, -9.81f, 0.0f };
    float drag{ 0.05f };
    float turbulence{ 0.0f };
    float restitution{ 0.35f };
    uint32_t burstCount{ 0 };  // >0: emit this many on play start
    bool collide{ false };
    bool emitting{ true };
};

// Vehicle authored in the Vehicle panel (Fase 8): the play world builds a
// chassis body plus four wheels (positions derived from wheelBase/trackWidth)
// and drives it with a VehicleRuntime.
struct VehicleComponent {
    float enginePower{ 4200.0f };
    float maxSteerAngle{ 0.55f };
    float brakeForce{ 6000.0f };
    float wheelRadius{ 0.36f };
    float suspensionRest{ 0.45f };
    float wheelBase{ 2.6f };     // front-rear axle distance
    float trackWidth{ 1.6f };    // left-right wheel distance
    float mass{ 1200.0f };
    bool frontWheelDrive{ true };
    bool enabled{ true };
};

// Ragdoll authored in the Ragdoll panel (Fase 6): the play world builds
// physics bodies per bone (from a real skeleton when fromSkeleton is set,
// else a two-bone fallback) and blends the physics pose into the pose.
struct RagdollComponent {
    bool enabled{ true };
    float blendWeight{ 0.8f };
    bool fromSkeleton{ false };   // build bones from the entity's skin skeleton
    float massPerBone{ 1.0f };
    glm::vec3 spawnOffset{ 0.0f, 0.0f, 0.0f };
};

// Mission authored in the Mission panel (Fase 8): the play world registers a
// Mission (Start -> SetObjective -> WaitForEvent -> CompleteMission) in its
// MissionSystem; the completeEvent (a script EmitEvent name) finishes it.
struct MissionComponent {
    std::string missionId{ "Mission" };
    std::string objectiveText{ "Complete the mission" };
    uint32_t objectiveTarget{ 1 };
    std::string completeEvent{ "MissionComplete" };
    bool autoStart{ true };
    bool active{ false };   // live play state
};

// Dialogue authored in the Dialogue panel (Fase 8): the play world registers a
// one-node DialogueGraph (line + one choice) and plays it on start; the choice
// can chain to another dialogue by id.
struct DialogueComponent {
    std::string dialogueId{ "Dialogue" };
    std::string character{ "NPC" };
    std::string line{ "Hello!" };
    std::string choiceText{ "Continue" };
    std::string nextDialogueId;   // empty = end after the choice
    bool playOnStart{ false };
    bool playing{ false };        // live play state
};

// Destructible authored in the Destruction panel (Fase 8): the play world
// builds a DestructibleRuntime of chunkCount boxes at the entity transform;
// weapon hits within damageRadius detach chunks with an impulse.
struct DestructionComponent {
    glm::vec3 chunkSize{ 0.5f };
    uint32_t chunkCount{ 6 };
    float chunkHealth{ 25.0f };
    float damageRadius{ 3.0f };
    float damageImpulse{ 8.0f };
    bool enabled{ true };
    bool destroyed{ false };      // live play state
};

// Navigation authored in the Navigation panel (Fase 8): the play world bakes a
// NavigationGrid at the entity transform (blockers from play physics bodies)
// and drives a NavigationAgent toward the primary camera entity each frame.
struct NavigationComponent {
    int gridWidth{ 32 };
    int gridHeight{ 32 };
    float cellSize{ 1.0f };
    float agentSpeed{ 3.0f };
    bool enabled{ true };
};

// Audio source authored in the Audio panel (Fase 8): the play world decodes
// the referenced .ogg (via the project's asset registry), plays it through the
// Audio::Mixer (spatial against the camera listener) and tracks playing state.
struct AudioComponent {
    std::string clipPath;         // source .ogg path (registry lookup)
    float volume{ 1.0f };
    float pitch{ 1.0f };
    bool spatial{ false };
    bool looping{ false };
    bool playOnStart{ true };
    bool playing{ false };        // live play state
};

struct MaterialComponent {
    glm::vec3 albedo{ 1.0f, 1.0f, 1.0f };
    float roughness{ 0.5f };
    float metallic{ 0.0f };
    glm::vec3 emissiveColor{ 0.0f, 0.0f, 0.0f };
    float emissiveIntensity{ 0.0f };
};

struct HierarchyComponent {
    UUID parentID{ 0, 0 };
    std::vector<UUID> childrenIDs;
};

struct VoxelVolumeComponent {
    int chunkBudget{ 1024 };
    int seed{ 1337 };
    float seaLevel{ 26.0f };
    bool enableFarLod{ true };
};

REFLECT_BEGIN(TransformComponent)
    REFLECT_FIELD(position, FieldType::Vec3, "Position")
    REFLECT_FIELD(rotation, FieldType::Vec3, "Rotation")
    REFLECT_FIELD(scale, FieldType::Vec3, "Scale")
REFLECT_END(TransformComponent)

REFLECT_BEGIN(LightComponent)
    REFLECT_FIELD(color, FieldType::Color, "Light Color")
    REFLECT_FIELD_RANGE(intensity, FieldType::Float, "Intensity", 0.0f, 100000.0f)
    REFLECT_FIELD_RANGE(range, FieldType::Float, "Range", 0.1f, 2000.0f)
    REFLECT_FIELD(castShadows, FieldType::Bool, "Cast Shadows")
    REFLECT_FIELD_RANGE(type, FieldType::Int, "Type (0 Dir, 1 Point, 2 Spot, 3 Area)", 0.0f, 3.0f)
REFLECT_END(LightComponent)

REFLECT_BEGIN(CameraComponent)
    REFLECT_FIELD_RANGE(fov, FieldType::Float, "FOV", 10.0f, 160.0f)
    REFLECT_FIELD(nearPlane, FieldType::Float, "Near Plane")
    REFLECT_FIELD(farPlane, FieldType::Float, "Far Plane")
    REFLECT_FIELD(isPrimary, FieldType::Bool, "Is Primary")
REFLECT_END(CameraComponent)

REFLECT_BEGIN(RigidbodyComponent)
    REFLECT_FIELD_RANGE(mass, FieldType::Float, "Mass (kg)", 0.01f, 10000.0f)
    REFLECT_FIELD_RANGE(friction, FieldType::Float, "Friction", 0.0f, 1.0f)
    REFLECT_FIELD_RANGE(restitution, FieldType::Float, "Restitution", 0.0f, 1.0f)
    REFLECT_FIELD(isKinematic, FieldType::Bool, "Is Kinematic")
    REFLECT_FIELD(useGravity, FieldType::Bool, "Use Gravity")
REFLECT_END(RigidbodyComponent)

REFLECT_BEGIN(MaterialComponent)
    REFLECT_FIELD(albedo, FieldType::Color, "Albedo")
    REFLECT_FIELD_RANGE(roughness, FieldType::Float, "Roughness", 0.0f, 1.0f)
    REFLECT_FIELD_RANGE(metallic, FieldType::Float, "Metallic", 0.0f, 1.0f)
    REFLECT_FIELD(emissiveColor, FieldType::Color, "Emissive Color")
    REFLECT_FIELD_RANGE(emissiveIntensity, FieldType::Float, "Emissive Intensity", 0.0f, 10000.0f)
REFLECT_END(MaterialComponent)

inline void register_builtin_component_reflection() {
    static std::once_flag registered;
    std::call_once(registered, [] {
        reflect_TransformComponent();
        reflect_LightComponent();
        reflect_CameraComponent();
        reflect_RigidbodyComponent();
        reflect_MaterialComponent();
    });
}

} // namespace Engine
