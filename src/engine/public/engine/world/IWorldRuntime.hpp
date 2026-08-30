#pragma once

// IWorldRuntime — the canonical game/world composition owner (Agente 2 block A).
//
// The engine already exposes per-domain public contracts (IEntityWorld,
// IPhysicsWorld/IGameplayRuntime, IWorldManager, IGameplaySystemWiring,
// IGameplayIntegration, IPortalSystem, ITimelinePolicy, IAudioEventMapper,
// INavigationProvider, etc.). The product executables (game, editor play mode,
// dedicated server) do NOT yet agree on a single ownership/composition step —
// the game instantiates an isolated IEntityWorld while the canonical wiring
// contracts are left unconsumed. IWorldRuntime is the small, deterministic
// composition host that (a) owns every service the world needs via a typed
// context, (b) enforces one explicit lifecycle order so game, play mode and
// server advance the same systems, and (c) is renderer-independent (self-
// contained, no Vulkan, no GLFW).
//
// Determinism: no globals; bootstrap/order are explicit; null services are
// allowed where a subsystem is optional for an executable (server has no
// render snapshot; play mode has no network authoring), but the fixed tick is
// the simulation authority for every executable that reports gameplay.
//
// The only implementation is src/engine/sdk/WorldRuntime.cpp.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {

// Forward-declare the services the runtime owns through type-erased holders.
namespace entity { class IEntityWorld; class IMobBehavior; }
namespace gameplay { class IGameplayRuntime; class IGameplayIntegration; class IGameplaySystemWiring; class IGameplayEventRouter; class IGameplayBindings; class IDayNightCycle; }
namespace world { class IWorldManager; class IPortalSystem; class ITimelinePolicy; }
namespace navigation { class INavigationProvider; class IAsyncQueryScheduler; }
namespace voxel { class IVoxelWorld; }
namespace audio { class IAudioEventMapper; }

// Typed service context: the runtime owns each service and hands out stable
// raw pointers (never transferring ownership). Null means the subsystem is not
// enabled for this executable configuration.
struct WorldServiceContext {
    entity::IEntityWorld* ecs{ nullptr };
    entity::IMobBehavior* mobBehavior{ nullptr };
    voxel::IVoxelWorld* voxel{ nullptr };
    gameplay::IGameplayRuntime* physicsGameplay{ nullptr };
    gameplay::IGameplayIntegration* integration{ nullptr };
    gameplay::IGameplaySystemWiring* wiring{ nullptr };
    gameplay::IGameplayEventRouter* eventRouter{ nullptr };
    gameplay::IGameplayBindings* bindings{ nullptr };
    world::IWorldManager* worlds{ nullptr };
    world::IPortalSystem* portals{ nullptr };
    world::ITimelinePolicy* timelinePolicy{ nullptr };
    navigation::INavigationProvider* navigation{ nullptr };
    navigation::IAsyncQueryScheduler* navigationQueries{ nullptr };
    audio::IAudioEventMapper* audio{ nullptr };
    // Deterministic day/night clock (G.95): advanced by the canonical runtime
    // on the variable frame so the game, play mode and server share ONE clock.
    // Systems read time_of_day()/sun_altitude()/daylight_factor() from it;
    // null disables the subsystem for an executable configuration.
    gameplay::IDayNightCycle* dayNight{ nullptr };
};

// Explicit, ordered phases of the canonical world frame. game/play-mode/server
// all run these in the same order; an executable skips a phase by not binding
// its service, never by reordering.
enum class WorldLifecyclePhase : std::uint8_t {
    Bootstrap,        // services created + wired into context
    FixedTick,        // simulation authority (fixed-step gameplay/physics)
    VariableUpdate,   // per-frame variable-rate updates (animation, AI LOD)
    LateUpdate,       // post-update (IK, pose warp, constraints finalization)
    RenderSnapshot,   // read simulation state into the render partition
    Persistence,      // autosave/journal, async, honours this frame's budget
    Shutdown          // ordered teardown (physics claims, persistence flush)
};

struct WorldRuntimeUpdateResult {
    std::uint64_t tick{ 0 };          // fixed ticks run this frame
    std::size_t entities{ 0 };        // entities on the ECS after this frame
    std::size_t worlds{ 0 };          // worlds managed by IWorldManager
    double simulationSeconds{ 0.0 };
    bool bootstrapComplete{ false };
    bool integrationComplete{ false };
};

class IWorldRuntime {
public:
    virtual ~IWorldRuntime() = default;

    // ---- Composition -------------------------------------------------------
    // Binds the runtime to services the OWNER of the runtime has created. This
    // keeps IWorldRuntime a pure orchestrator: it never constructs physics/
    // voxel/network backends itself, so jogo/play/server reuse the same
    // composition with their own backend choices.
    virtual bool bind(const WorldServiceContext& context, std::string& errorOut) = 0;

    // ---- Lifecycle ---------------------------------------------------------
    // Runs Bootstrap: wires the context into the canonical integration/wiring
    // and marks every applicable lifecycle phase bound. Idempotent.
    virtual bool bootstrap(std::string& errorOut) = 0;
    // Advances ONE frame in the canonical order (FixedTick -> VariableUpdate ->
    // LateUpdate -> RenderSnapshot -> Persistence). `deltaSeconds` is the
    // variable frame time; fixed tick uses the configured fixedDelta.
    virtual WorldRuntimeUpdateResult advance(float deltaSeconds) = 0;
    // Ordered teardown (reverse of bootstrap) with persistence flush.
    virtual void shutdown() = 0;

    // ---- Configuration -----------------------------------------------------
    virtual bool set_fixed_delta(float seconds) = 0;
    virtual bool set_autosave_budget_ms(float milliseconds) = 0;
    // Sets the world-streaming focus the canonical integration uses when it
    // advances every attached IWorldManager world. Without this the worlds
    // stream around (0,0,0) and evict the real play/server focus (fixed-focus
    // defect, Agente 2 2026-08-29). Default (0,0,0) for compatibility.
    virtual bool set_world_focus(float x, float y, float z) = 0;
    virtual float fixed_delta() const = 0;

    // ---- Introspection -----------------------------------------------------
    virtual const WorldServiceContext& services() const = 0;
    virtual std::vector<WorldLifecyclePhase> enabled_phases() const = 0;
    virtual std::string to_json() const = 0;
};

// The only implementation of IWorldRuntime (src/engine/sdk/WorldRuntime.cpp).
std::unique_ptr<IWorldRuntime> create_world_runtime();

}  // namespace engine