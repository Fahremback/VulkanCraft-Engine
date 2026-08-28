#pragma once

#include "engine/gameplay/IGameplayBindings.hpp"
#include "engine/gameplay/IGameplayEventRouter.hpp"
#include "engine/gameplay/IAbilitySystem.hpp"
#include "engine/gameplay/IEffectStacks.hpp"
#include "engine/gameplay/IAbilityEffects.hpp"
#include "engine/gameplay/IDayNightCycle.hpp"
#include "engine/gameplay/IHitReaction.hpp"
#include "engine/gameplay/IRagdollAsset.hpp"
#include "engine/gameplay/IReplay.hpp"
#include "engine/animation/IAnimationLod.hpp"
#include "engine/animation/IAnimBudget.hpp"
#include "engine/animation/IAnimCore.hpp"
#include "engine/animation/ISkinning.hpp"
#include "engine/entity/IEntityWorld.hpp"
#include "engine/entity/IEntityArchetype.hpp"
#include "engine/entity/IEntityLifecycle.hpp"
#include "engine/entity/ISpatialIndex.hpp"
#include "engine/navigation/INavigationProvider.hpp"
#include "engine/navigation/INavInvalidation.hpp"
#include "engine/navigation/INavStreaming.hpp"
#include "engine/navigation/IAsyncQueryScheduler.hpp"
#include "engine/gameplay/IGameplayRuntime.hpp"
#include "engine/voxel/IVoxelWorld.hpp"
#include "engine/networking/INetworkReplication.hpp"
#include "engine/rendering/IRenderingDebugView.hpp"
#include "engine/scripting/IVisualScriptRuntime.hpp"
#include "engine/semantic/ISemanticApi.hpp"
#include "engine/audio/IAudioEventMapper.hpp"
#include "engine/world/IWorldManager.hpp"
#include "engine/world/IPortalSystem.hpp"
#include "engine/world/ITimelinePolicy.hpp"

#include <memory>
#include <string>

namespace engine::gameplay {

struct GameplaySystemWiring {
    entity::IEntityWorld* ecs{nullptr};
    entity::IEntityArchetypeRegistry* archetypes{nullptr};
    entity::IEntityLifecycle* lifecycle{nullptr};
    entity::ISpatialIndex* spatialIndex{nullptr};
    navigation::INavigationProvider* navigation{nullptr};
    navigation::INavInvalidation* navigationInvalidation{nullptr};
    navigation::INavStreaming* navigationStreaming{nullptr};
    navigation::IAsyncQueryScheduler* navigationQueries{nullptr};
    IGameplayRuntime* physicsGameplay{nullptr};
    IAbilitySystem* abilities{nullptr};
    IEffectStacks* effectStacks{nullptr};
    IAbilityEffects* abilityEffects{nullptr};
    IDayNightCycle* dayNight{nullptr};
    IHitReaction* hitReaction{nullptr};
    animation::IAnimationLod* animationLod{nullptr};
    animation::IAnimBudget* animationBudget{nullptr};
    animation::IAnimCore* animationCore{nullptr};
    animation::ISkinning* skinning{nullptr};
    const RagdollAsset* ragdollAsset{nullptr};
    voxel::IVoxelWorld* voxel{nullptr};
    networking::INetworkReplication* multiplayer{nullptr};
    const Engine::Rendering::IRenderingDebugView* renderer{nullptr};
    scripting::IVisualScriptRuntime* scriptingRuntime{nullptr};
    semantic::ISemanticApi* semantic{nullptr};
    audio::IAudioEventMapper* audio{nullptr};
    IGameplayEventRouter* eventRouter{nullptr};
    world::IWorldManager* worlds{nullptr};
    world::IPortalSystem* portals{nullptr};
    world::ITimelinePolicy* timelinePolicy{nullptr};
    IReplay* replay{nullptr};
};

class IGameplaySystemWiring {
public:
    virtual ~IGameplaySystemWiring() = default;
    virtual bool attach(const GameplaySystemWiring& wiring, std::string& errorOut) = 0;
    virtual bool attach_bindings(IGameplayBindings* bindings) = 0;
    virtual bool complete(std::string& errorOut) const = 0;
    virtual const GameplaySystemWiring& wiring() const = 0;
    virtual std::string to_json() const = 0;
};

std::unique_ptr<IGameplaySystemWiring> create_gameplay_system_wiring();

} // namespace engine::gameplay
