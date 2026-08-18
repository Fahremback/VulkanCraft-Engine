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

// ---------------------------------------------------------------------------
// Wicked-port component set (frontend; PORTS.md). Each struct is authored in
// its dedicated editor panel. Runtime integration status is noted per struct:
// where the play world does not simulate the feature yet, the panel carries an
// explicit TODO(frontend-port) comment and the data is still serialized so a
// future runtime pass can consume it.
// ---------------------------------------------------------------------------

// Collision shape authored in the Collider panel. The play world maps this to
// a physics shape (box/sphere/capsule) on the rigidbody.
enum class ColliderShape : uint8_t { Box = 0, Sphere = 1, Capsule = 2 };
struct ColliderComponent {
    ColliderShape shape{ ColliderShape::Box };
    glm::vec3 size{ 1.0f, 1.0f, 1.0f };
    float radius{ 0.5f };
    float height{ 1.0f };
    glm::vec3 offset{ 0.0f, 0.0f, 0.0f };
    bool isTrigger{ false };
    bool enabled{ true };
};

// Physics joint authored in the Constraint panel. Runtime integration:
// TODO(frontend-port) — map to a Jolt/Bullet constraint in the play world.
enum class ConstraintType : uint8_t { Fixed = 0, Hinge = 1, Spring = 2, Point = 3 };
struct ConstraintComponent {
    ConstraintType type{ ConstraintType::Fixed };
    UUID otherEntity{ 0, 0 };
    glm::vec3 anchor{ 0.0f, 0.0f, 0.0f };
    glm::vec3 axis{ 0.0f, 1.0f, 0.0f };
    float breakForce{ 0.0f };          // 0 = unbreakable
    bool enabled{ true };
};

// Cloth/soft body authored in the Soft Body panel. Runtime integration:
// TODO(frontend-port) — soft body solver in the physics backend.
struct SoftBodyComponent {
    uint32_t detail{ 8 };
    float mass{ 1.0f };
    float friction{ 0.5f };
    float restitution{ 0.1f };
    float pressure{ 0.0f };
    float vertexRadius{ 0.05f };
    bool wind{ true };
    bool enabled{ true };
};

// Spring dynamics authored in the Spring panel. Runtime integration:
// TODO(frontend-port) — spring/constraint solver in the physics backend.
struct SpringComponent {
    float stiffness{ 0.5f };
    float drag{ 0.1f };
    float wind{ 0.0f };
    float gravity{ 1.0f };
    float hitRadius{ 0.5f };
    bool disabled{ false };
    bool useGravity{ true };
    bool enabled{ true };
};

// Projected texture authored in the Decal panel. Runtime integration:
// TODO(frontend-port) — decal pass in the deferred/forward renderer.
struct DecalComponent {
    std::string texturePath;
    glm::vec3 color{ 1.0f, 1.0f, 1.0f };
    float slopeBlendPower{ 1.0f };
    bool projectOnStatic{ true };
    bool onlyAlpha{ false };
    bool enabled{ true };
};

// Catmull-Rom path authored in the Spline panel. Runtime integration:
// TODO(frontend-port) — spline follower/mesh generator in the play world.
struct SplineComponent {
    std::vector<glm::vec3> points{ { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 2.0f, 0.0f, 0.0f } };
    bool looped{ false };
    bool filled{ false };
    float width{ 1.0f };
    float rotation{ 0.0f };
    uint32_t subdiv{ 16 };
    bool enabled{ true };
};

// Physics zone authored in the Force Field panel. Runtime integration:
// TODO(frontend-port) — force evaluation against play physics bodies.
enum class ForceFieldType : uint8_t { Gravity = 0, Push = 1, Wind = 2, Vortex = 3 };
struct ForceFieldComponent {
    ForceFieldType type{ ForceFieldType::Gravity };
    float strength{ 1.0f };
    float range{ 10.0f };
    bool enabled{ true };
};

// Reflection probe authored in the Env Probe panel. Runtime integration:
// TODO(frontend-port) — cubemap capture pass in the renderer.
struct EnvProbeComponent {
    bool realTime{ false };
    float viewDistance{ 100.0f };
    uint32_t resolution{ 256 };
    bool enabled{ true };
};

// Global weather/sky authored in the Weather panel. Runtime integration:
// TODO(frontend-port) — sky/fog/atmosphere pass in the renderer.
struct WeatherComponent {
    glm::vec3 sunColor{ 1.0f, 0.95f, 0.85f };
    float fogDensity{ 0.001f };
    float fogStart{ 100.0f };
    float skyExposure{ 1.0f };
    float skyRotation{ 0.0f };
    float windSpeed{ 5.0f };
    float rainAmount{ 0.0f };
    float rainLength{ 1.0f };
    bool heightFog{ false };
    bool enabled{ true };
};

// Hair/strand system authored in the Hair panel. Runtime integration:
// TODO(frontend-port) — strand rendering in the renderer.
struct HairParticleComponent {
    std::string meshPath;
    uint32_t count{ 1000 };
    float length{ 0.3f };
    float width{ 0.01f };
    float stiffness{ 0.5f };
    float drag{ 0.1f };
    float gravityPower{ 1.0f };
    float randomness{ 0.2f };
    uint32_t segments{ 8 };
    uint32_t seed{ 0 };
    bool enabled{ true };
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
