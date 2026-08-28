#include "engine/gameplay/IGameplayIntegration.hpp"
#include "engine/gameplay/IGameplayBindings.hpp"
#include "engine/gameplay/IGameplaySystemWiring.hpp"
#include "engine/gameplay/IGameplayDebugSurface.hpp"
#include "engine/gameplay/IGameplayEvents.hpp"
#include "engine/gameplay/IGameplayMetrics.hpp"
#include "engine/gameplay/IGameplayEventRouter.hpp"
#include "engine/entity/IEntityWorld.hpp"
#include "engine/entity/IEntityArchetype.hpp"
#include "engine/entity/IEntityLifecycle.hpp"
#include "engine/entity/ISpatialIndex.hpp"
#include "engine/navigation/INavigationProvider.hpp"
#include "engine/navigation/INavInvalidation.hpp"
#include "engine/navigation/INavStreaming.hpp"
#include "engine/navigation/IAsyncQueryScheduler.hpp"
#include "engine/gameplay/IGameplayRuntime.hpp"
#include "engine/gameplay/IAbilitySystem.hpp"
#include "engine/gameplay/IEffectStacks.hpp"
#include "engine/gameplay/IAbilityEffects.hpp"
#include "engine/gameplay/IDayNightCycle.hpp"
#include "engine/gameplay/IHitReaction.hpp"
#include "engine/gameplay/IRagdollAsset.hpp"
#include "engine/animation/IAnimCore.hpp"
#include "engine/animation/IAnimationLod.hpp"
#include "engine/animation/IAnimBudget.hpp"
#include "engine/animation/ISkinning.hpp"
#include "engine/voxel/IVoxelWorld.hpp"
#include "engine/networking/INetworkReplication.hpp"
#include "engine/rendering/IRenderingDebugView.hpp"
#include "engine/scripting/IVisualScriptGraph.hpp"
#include "engine/scripting/IVisualScriptRuntime.hpp"
#include "engine/semantic/ISemanticApi.hpp"
#include "engine/audio/IAudioEventMapper.hpp"
#include "engine/world/IWorldManager.hpp"
#include "engine/world/IPortalSystem.hpp"
#include "engine/world/ITimelinePolicy.hpp"
#include "engine/gameplay/IReplay.hpp"
#include <cstdio>
#include <string>
#include <vector>

int main() {
    std::string error;
    auto events = engine::gameplay::create_gameplay_events();
    auto metrics = engine::gameplay::create_gameplay_metrics();
    auto audio = engine::audio::create_audio_event_mapper();
    auto router = engine::gameplay::create_gameplay_event_router(events.get(), audio.get(), metrics.get());
    auto integration = engine::gameplay::create_gameplay_integration();
    auto bindings = engine::gameplay::create_gameplay_bindings();
    if (!integration || !bindings || !events || !metrics || !audio || !router ||
        !integration->configure(1.0f / 60.0f, 2, error)) return 1;

    const std::vector<engine::gameplay::GameplayDomain> domains = {
        engine::gameplay::GameplayDomain::Ecs, engine::gameplay::GameplayDomain::Navigation,
        engine::gameplay::GameplayDomain::Animation, engine::gameplay::GameplayDomain::Physics,
        engine::gameplay::GameplayDomain::Voxel, engine::gameplay::GameplayDomain::Renderer,
        engine::gameplay::GameplayDomain::Multiplayer, engine::gameplay::GameplayDomain::Editor,
        engine::gameplay::GameplayDomain::Scripting, engine::gameplay::GameplayDomain::Audio,
        engine::gameplay::GameplayDomain::Worlds, engine::gameplay::GameplayDomain::ExternalSolutions};
    std::vector<engine::gameplay::GameplayBinding> domainBindings;
    for (const auto domain : domains) {
        domainBindings.push_back({domain, "editor.producer", "editor.consumer",
                                  "editor.persistence", "editor.replication"});
    }
    if (!bindings->configure(domainBindings, error)) return 2;
    const char* solutions[] = {"behavior-tree-cpp", "ceres-solver", "deepmimic", "minecraft-spider",
        "motion-matching", "mujoco", "mujoco-mpc", "ozz-animation", "or-tools", "opus",
        "recast-navigation", "steam-audio", "acl"};
    std::vector<engine::gameplay::GameplayExternalBinding> external;
    for (const auto* id : solutions) external.push_back({id, engine::gameplay::GameplayDomain::ExternalSolutions,
                                                           std::string("editor.adapter.") + id,
                                                           std::string("editor.contract.") + id, true});
    if (!bindings->configure_external(external, error) || !bindings->complete(error)) return 3;
    if (!integration->attach_bindings(bindings.get())) return 4;

    engine::scripting::VisualScriptGraph graph;
    engine::scripting::NodeDef node;
    node.type_name = "gameplay.editor.debug";
    if (!graph.register_node_type(node, &error)) return 5;
    engine::scripting::NodeInstance instance;
    instance.type_name = node.type_name;
    if (graph.add_node(instance) == 0 || !graph.validate().valid()) return 6;
    auto scriptingRuntime = engine::scripting::create_visual_script_runtime();
    auto semantic = engine::semantic::create_semantic_api();
    engine::semantic::SemanticKind schema;
    schema.name = "gameplay.editor.debug";
    schema.fields.push_back({"tick", engine::semantic::SemanticFieldType::Int, true});
    if (!scriptingRuntime || !semantic || !semantic->register_kind(schema, error)) return 7;

    auto ecs = engine::entity::create_entity_world();
    auto archetypes = engine::entity::create_entity_archetype_registry();
    auto lifecycle = engine::entity::create_entity_lifecycle();
    auto spatial = engine::entity::create_spatial_index();
    auto nav = engine::navigation::create_recast_navigation_provider();
    auto invalidation = engine::navigation::create_nav_invalidation();
    auto streaming = engine::navigation::create_nav_streaming();
    auto queries = engine::navigation::create_async_query_scheduler();
    auto navigationBridge = engine::navigation::create_navigation_scheduler_bridge(queries.get(), nav.get());
    auto runtime = engine::gameplay::create_gameplay_runtime();
    auto abilities = engine::gameplay::create_ability_system();
    auto stacks = engine::gameplay::create_effect_stacks();
    auto effects = engine::gameplay::create_ability_effects();
    auto dayNight = engine::gameplay::create_day_night_cycle();
    auto reaction = engine::gameplay::create_hit_reaction();
    auto animCore = engine::animation::create_anim_core();
    auto animLod = engine::animation::create_animation_lod();
    auto animBudget = engine::animation::create_anim_budget();
    auto skinning = animCore ? engine::animation::create_skinning(*animCore) : nullptr;
    auto voxel = engine::voxel::create_default_voxel_world();
    std::string replicationError;
    auto multiplayer = engine::networking::create_network_replication("editor", replicationError);
    auto renderer = engine::Engine::Rendering::create_rendering_debug_view(error);
    auto worlds = engine::world::create_world_manager();
    auto portals = engine::world::create_portal_system();
    auto timeline = engine::world::create_timeline_policy();
    auto replay = engine::gameplay::create_replay();
    engine::gameplay::RagdollAsset ragdoll;
    ragdoll.name = "editor-ragdoll";
    engine::gameplay::RagdollJoint root;
    root.name = "root";
    ragdoll.joints.push_back(root);
    if (!ecs || !archetypes || !lifecycle || !spatial || !nav || !invalidation || !streaming || !queries ||
        !navigationBridge || !runtime || !abilities || !stacks || !effects || !dayNight || !reaction || !animCore || !animLod ||
        !animBudget || !skinning || !voxel || !multiplayer || !renderer || !worlds || !portals || !timeline ||
        !replay || !ragdoll.validate(error)) return 8;

    engine::gameplay::GameplaySystemWiring seams;
    seams.ecs = ecs.get(); seams.archetypes = archetypes.get(); seams.lifecycle = lifecycle.get();
    seams.spatialIndex = spatial.get(); seams.navigation = nav.get(); seams.navigationInvalidation = invalidation.get();
    seams.navigationStreaming = streaming.get(); seams.navigationQueries = queries.get(); seams.physicsGameplay = runtime.get();
    seams.abilities = abilities.get(); seams.effectStacks = stacks.get(); seams.abilityEffects = effects.get();
    seams.dayNight = dayNight.get(); seams.hitReaction = reaction.get(); seams.animationCore = animCore.get();
    seams.animationLod = animLod.get(); seams.animationBudget = animBudget.get(); seams.skinning = skinning.get();
    seams.ragdollAsset = &ragdoll; seams.voxel = voxel.get(); seams.multiplayer = multiplayer.get();
    seams.renderer = renderer.get(); seams.scriptingRuntime = scriptingRuntime.get(); seams.semantic = semantic.get();
    seams.audio = audio.get(); seams.eventRouter = router.get(); seams.worlds = worlds.get(); seams.portals = portals.get();
    seams.timelinePolicy = timeline.get(); seams.replay = replay.get();
    auto wiring = engine::gameplay::create_gameplay_system_wiring();
    if (!wiring->attach(seams, error) || !wiring->attach_bindings(bindings.get()) || !wiring->complete(error)) return 9;
    if (!integration->attach_system_wiring(wiring.get())) return 10;
    if (!integration->attach_events(events.get()) || !integration->attach_metrics(metrics.get()) ||
        !integration->attach_audio_mapper(audio.get()) || !integration->attach_entity_world(ecs.get()) ||
        !integration->attach_queries(queries.get()) || !integration->attach_navigation_bridge(navigationBridge.get()) ||
        !integration->attach_runtime(runtime.get()) ||
        !integration->attach_world_manager(worlds.get()) || !integration->attach_event_router(router.get())) return 11;
    integration->advance(1.0f / 60.0f);
    auto debug = engine::gameplay::create_gameplay_debug_surface();
    debug->bind_integration(integration.get());
    if (!debug || !debug->snapshot().integration.bindingsComplete ||
        !debug->snapshot().integration.runtimeWiringComplete) return 12;
    std::puts("editor-gameplay-consumer-ok concrete-wiring");
    std::puts("editor-gameplay-consumer-ok authoring");
    return 0;
}
