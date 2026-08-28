#include "engine/gameplay/IGameplayIntegration.hpp"
#include "engine/gameplay/IGameplayCrossDomain.hpp"
#include "engine/gameplay/IGameplayDebugSurface.hpp"
#include "engine/gameplay/IGameplayPhase.hpp"
#include "engine/gameplay/IGameplayBindings.hpp"
#include "engine/gameplay/IGameplaySystemWiring.hpp"
#include "engine/gameplay/IGameplayEvents.hpp"
#include "engine/gameplay/IGameplayMetrics.hpp"
#include "engine/entity/IEntityArchetype.hpp"
#include "engine/entity/IEntityLifecycle.hpp"
#include "engine/entity/ISpatialIndex.hpp"
#include "engine/world/IPortalSystem.hpp"
#include "engine/world/ITimelinePolicy.hpp"
#include "engine/gameplay/IReplay.hpp"
#include "engine/animation/IAnimationLod.hpp"
#include "engine/animation/IAnimBudget.hpp"
#include "engine/animation/IAnimCore.hpp"
#include "engine/animation/ISkinning.hpp"
#include "engine/gameplay/IAbilitySystem.hpp"
#include "engine/gameplay/IEffectStacks.hpp"
#include "engine/gameplay/IAbilityEffects.hpp"
#include "engine/gameplay/IDayNightCycle.hpp"
#include "engine/gameplay/IHitReaction.hpp"
#include "engine/gameplay/IRagdollAsset.hpp"
#include "engine/entity/IEntityWorld.hpp"
#include "engine/navigation/INavInvalidation.hpp"
#include "engine/navigation/INavStreaming.hpp"
#include "engine/navigation/IAsyncQueryScheduler.hpp"
#include "engine/networking/INetworkReplication.hpp"
#include "engine/voxel/IVoxelWorld.hpp"
#include "engine/scripting/IVisualScriptRuntime.hpp"
#include "engine/semantic/ISemanticApi.hpp"
#include "engine/audio/IAudioEventMapper.hpp"
#include "engine/world/IWorldManager.hpp"
#include <cstdio>
#include <string>
#include <vector>

int main() {
    std::string error;
    auto integration = engine::gameplay::create_gameplay_integration();
    auto events = engine::gameplay::create_gameplay_events();
    auto metrics = engine::gameplay::create_gameplay_metrics();
    if (!integration->configure(1.0f / 60.0f, 1, error)) return 1;
    auto bindings = engine::gameplay::create_gameplay_bindings();
    const std::vector<engine::gameplay::GameplayDomain> domains = {
        engine::gameplay::GameplayDomain::Ecs, engine::gameplay::GameplayDomain::Navigation,
        engine::gameplay::GameplayDomain::Animation, engine::gameplay::GameplayDomain::Physics,
        engine::gameplay::GameplayDomain::Voxel, engine::gameplay::GameplayDomain::Renderer,
        engine::gameplay::GameplayDomain::Multiplayer, engine::gameplay::GameplayDomain::Editor,
        engine::gameplay::GameplayDomain::Scripting, engine::gameplay::GameplayDomain::Audio,
        engine::gameplay::GameplayDomain::Worlds, engine::gameplay::GameplayDomain::ExternalSolutions};
    std::vector<engine::gameplay::GameplayBinding> manifests;
    for (const auto domain : domains) {
        manifests.push_back({domain, "producer." + std::to_string(static_cast<unsigned>(domain)),
                              "consumer." + std::to_string(static_cast<unsigned>(domain)),
                              "persist." + std::to_string(static_cast<unsigned>(domain)),
                              "replicate." + std::to_string(static_cast<unsigned>(domain))});
    }
    if (!bindings->configure(manifests, error)) return 2;
    const std::vector<const char*> solutions = {
        "behavior-tree-cpp", "ceres-solver", "deepmimic", "minecraft-spider",
        "motion-matching", "mujoco", "mujoco-mpc", "ozz-animation", "or-tools",
        "opus", "recast-navigation", "steam-audio", "acl"};
    std::vector<engine::gameplay::GameplayExternalBinding> external;
    for (const auto* solution : solutions) {
        external.push_back({solution, engine::gameplay::GameplayDomain::ExternalSolutions,
                            std::string("adapter.") + solution,
                            std::string("contract.") + solution, true});
    }
    if (!bindings->configure_external(external, error) || !bindings->complete(error)) return 3;
    if (!integration->attach_bindings(bindings.get())) return 4;

    auto ecs = engine::entity::create_entity_world();
    auto archetypes = engine::entity::create_entity_archetype_registry();
    auto lifecycle = engine::entity::create_entity_lifecycle();
    auto spatialIndex = engine::entity::create_spatial_index();
    auto nav = engine::navigation::create_recast_navigation_provider();
    auto invalidation = engine::navigation::create_nav_invalidation();
    auto streaming = engine::navigation::create_nav_streaming();
    auto queries = engine::navigation::create_async_query_scheduler();
    auto navigationBridge = engine::navigation::create_navigation_scheduler_bridge(queries.get(), nav.get());
    auto runtime = engine::gameplay::create_gameplay_runtime();
    auto abilities = engine::gameplay::create_ability_system();
    auto effectStacks = engine::gameplay::create_effect_stacks();
    auto abilityEffects = engine::gameplay::create_ability_effects();
    auto dayNight = engine::gameplay::create_day_night_cycle();
    auto hitReaction = engine::gameplay::create_hit_reaction();
    auto animationCore = engine::animation::create_anim_core();
    auto animationLod = engine::animation::create_animation_lod();
    auto animationBudget = engine::animation::create_anim_budget();
    auto skinning = animationCore ? engine::animation::create_skinning(*animationCore) : nullptr;
    engine::gameplay::RagdollAsset ragdollAsset;
    ragdollAsset.name = "consumer-ragdoll";
    engine::gameplay::RagdollJoint root;
    root.name = "root";
    ragdollAsset.joints.push_back(root);
    auto renderer = Engine::Rendering::create_rendering_debug_view(error);
    auto voxel = engine::voxel::create_default_voxel_world();
    std::string replicationError;
    auto multiplayer = engine::networking::create_network_replication("gameplay-consumer", replicationError);
    auto scripting = engine::scripting::create_visual_script_runtime();
    auto semantic = engine::semantic::create_semantic_api();
    auto audio = engine::audio::create_audio_event_mapper();
    if (events && metrics && audio) {
        integration->attach_events(events.get());
        integration->attach_metrics(metrics.get());
        integration->attach_audio_mapper(audio.get());
    }
    auto worlds = engine::world::create_world_manager();
    auto portals = engine::world::create_portal_system();
    auto timelinePolicy = engine::world::create_timeline_policy();
    auto replay = engine::gameplay::create_replay();
    auto eventRouter = engine::gameplay::create_gameplay_event_router(events.get(), audio.get(), metrics.get());
    if (!ecs || !nav || !invalidation || !streaming || !queries || !runtime || !voxel ||
        !navigationBridge || !multiplayer || !archetypes || !lifecycle || !spatialIndex || !portals ||
        !timelinePolicy || !replay || !eventRouter || !events || !metrics || !abilities || !effectStacks || !abilityEffects || !dayNight ||
        !hitReaction || !animationCore || !animationLod || !animationBudget || !skinning ||
        !renderer || !scripting || !semantic || !audio || !worlds || !ragdollAsset.validate(error)) return 5;

    auto wiring = engine::gameplay::create_gameplay_system_wiring();
    engine::gameplay::GameplaySystemWiring seams;
    seams.ecs = ecs.get(); seams.archetypes = archetypes.get(); seams.lifecycle = lifecycle.get();
    seams.spatialIndex = spatialIndex.get(); seams.navigation = nav.get();
    seams.navigationInvalidation = invalidation.get(); seams.navigationStreaming = streaming.get();
    seams.navigationQueries = queries.get();    seams.physicsGameplay = runtime.get(); seams.abilities = abilities.get();
    seams.effectStacks = effectStacks.get(); seams.abilityEffects = abilityEffects.get();
    seams.dayNight = dayNight.get(); seams.hitReaction = hitReaction.get();
    seams.animationCore = animationCore.get(); seams.animationLod = animationLod.get();
    seams.animationBudget = animationBudget.get(); seams.skinning = skinning.get();
    seams.ragdollAsset = &ragdollAsset;

    seams.voxel = voxel.get(); seams.multiplayer = multiplayer.get(); seams.renderer = renderer.get();
    seams.scriptingRuntime = scripting.get();
    seams.semantic = semantic.get(); seams.audio = audio.get(); seams.eventRouter = eventRouter.get();
    seams.worlds = worlds.get(); seams.portals = portals.get(); seams.timelinePolicy = timelinePolicy.get();
    seams.replay = replay.get();
    if (!wiring->attach(seams, error) || !wiring->attach_bindings(bindings.get()) || !wiring->complete(error)) return 6;
    if (!integration->attach_system_wiring(wiring.get())) return 7;
    if (        !integration->attach_entity_world(ecs.get()) || !integration->attach_queries(queries.get()) ||
        !integration->attach_navigation_bridge(navigationBridge.get()) ||

        !integration->attach_runtime(runtime.get()) || !integration->attach_world_manager(worlds.get()) ||
        !integration->attach_event_router(eventRouter.get()) || !integration->snapshot().runtimeWiringComplete) return 8;

    auto debug = engine::gameplay::create_gameplay_debug_surface();
    debug->bind_integration(integration.get());
    integration->advance(1.0f / 60.0f);
    if (debug->snapshot().integration.tick != 1 || !debug->snapshot().integration.bindingsComplete ||
        !debug->snapshot().integration.runtimeWiringComplete) return 9;

    std::puts("gameplay-consumer-ok concrete-wiring");
    std::puts("gameplay-consumer-ok all");
    return 0;
}
