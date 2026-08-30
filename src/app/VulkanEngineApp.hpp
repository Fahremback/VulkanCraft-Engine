#pragma once

#include "VulkanTypes.hpp"

#include "Player.hpp"
#include "World.hpp"
#include "WorldRenderer.hpp"
#include "TextureManager.hpp"
#include "MobRenderer.hpp"
#include "SoundEngine.hpp"
#include "engine/rendering/vulkan/GpuRenderFeatures.hpp"
#include "engine/rendering/vulkan/GpuFeaturePasses.hpp"
#include "WorldSystems.hpp"
#include "ShowcaseBlockEntity.hpp"
#include "engine/rendering/IRenderingDebugView.hpp"
#include "engine/rendering/IProbeGrid.hpp"
#include "engine/rendering/IReSTIRDI.hpp"
#include "engine/rendering/ITemporalDenoiser.hpp"
#include "engine/rendering/IFluidSimulation.hpp"
#include "engine/rendering/IBlockMaterialResolver.hpp"
#include "engine/rendering/IToneMapping.hpp"
#include "engine/rendering/IAtmosphereScattering.hpp"
#include "engine/rendering/IVolumeClouds.hpp"
#include "engine/rendering/IMaterialShading.hpp"
#include "engine/rendering/IParticleDrawData.hpp"
#include "engine/rendering/IParticleSystem.hpp"
#include "engine/rendering/IReSTIRDI.hpp"
#include "engine/rendering/IReflectionProvider.hpp"
#include "engine/rendering/IDiffuseGlobalIllumination.hpp"
#include "engine/rendering/IGlobalIlluminationProvider.hpp"
#include "engine/assets/ISceneLayers.hpp"
#include "engine/assets/IAssetPipeline.hpp"
#include "engine/rendering/ICasSharpening.hpp"
#include "engine/rendering/IKtx2Transcoder.hpp"
#include "engine/rendering/IXrMath.hpp"
#include "engine/rendering/IHairPhysics.hpp"
#include "engine/rendering/IShaderCompiler.hpp"
#include "engine/rendering/IRenderPassMetrics.hpp"
#include "engine/rendering/ISceneCulling.hpp"
#include "engine/rendering/IVulkanLoader.hpp"
#include "engine/rendering/ISwapchainManager.hpp"
#include "engine/rendering/IRenderGraph.hpp"
#include "engine/rendering/IRenderingPresets.hpp"
#include "engine/rendering/ISparseVolumeGrid.hpp"
#include "engine/rendering/IRayBakeMesh.hpp"
#include "engine/rendering/IEllipsoidMath.hpp"
#include "engine/world/IOriginRebase.hpp"
#include "engine/rendering/ILumenScene.hpp"
#include "engine/rendering/ISurfaceCacheCapture.hpp"
#include "engine/rendering/IRayTracer.hpp"
#include "engine/rendering/IRenderProviderRegistry.hpp"
#include "engine/rendering/IScreenSpaceTracer.hpp"
#include "engine/rendering/ISoftwareTracer.hpp"
#include "engine/rendering/IFftCore.hpp"
#include "engine/rendering/IFftOceanSurface.hpp"
#include "engine/rendering/ISpatialUpscaler.hpp"
#include "engine/rendering/IReflectionModel.hpp"
#include "engine/rendering/IRenderingPresets.hpp"
#include "engine/rendering/IEllipsoidMath.hpp"
#include "engine/rendering/ISparseVolumeGrid.hpp"
#include "engine/rendering/IRayBakeMesh.hpp"
#include "engine/rendering/RenderGraph.hpp"
#include "engine/rendering/lighting/RadianceCache.hpp"
#include "engine/registry/BlockRegistry.hpp"
#include "simulation/voxel/streaming/WorldRenderBridge.hpp"
#include "simulation/voxel/meshing/ChunkMeshResult.hpp"
#include "engine/entity/IEntityWorld.hpp"
#include "engine/entity/IMobBehavior.hpp"
#include "engine/world/IWorldRuntime.hpp"
#include "engine/world/IWorldManager.hpp"
#include "engine/gameplay/IGameplayRuntime.hpp"
#include "engine/vehicles/IVehicleAsset.hpp"
#include "engine/gameplay/IMissionAsset.hpp"
#include "engine/gameplay/IGameplayIntegration.hpp"
#include "engine/gameplay/IGameplayBindings.hpp"
#include "engine/gameplay/IGameplaySystemWiring.hpp"
#include "engine/gameplay/IGameplayEvents.hpp"
#include "engine/gameplay/IGameplayMetrics.hpp"
#include "engine/gameplay/IGameplayEventRouter.hpp"
#include "engine/gameplay/IDayNightCycle.hpp"
#include "engine/navigation/IAsyncQueryScheduler.hpp"
#include "engine/audio/IAudioEventMapper.hpp"
#include "engine/audio/ISpatialAudio.hpp"
#include "engine/audio/IAdaptiveMusic.hpp"
#include "engine/ai/ISteering.hpp"
#include "engine/ai/IPerception.hpp"
#include "engine/ai/IFsm.hpp"
#include "engine/ai/IBehaviorTree.hpp"
#include "engine/ai/IUtilityAi.hpp"
#include "engine/ai/IPlanner.hpp"
#include "engine/ai/IAiLod.hpp"
#include "engine/ai/IAiEventBus.hpp"
#include "engine/ai/ICrowdSimulation.hpp"
#include "engine/gameplay/IAbilityEffects.hpp"
#include "engine/gameplay/IAbilitySystem.hpp"
#include "engine/registry/RecipeRegistry.hpp"
#include "engine/registry/ItemRegistry.hpp"
#include "engine/registry/Inventory.hpp"
#include "engine/registry/ItemStack.hpp"
#include "engine/entity/ISpatialIndex.hpp"
#include "engine/input/IActionMap.hpp"
#include "engine/scene/Scene.hpp"
#include "engine/animation/IAnimationLod.hpp"
#include "engine/animation/IAnimCore.hpp"
#include "engine/animation/IAnimAdditive.hpp"
#include "engine/animation/IAnimEvents.hpp"
#include "engine/animation/IAnimMask.hpp"
#include "engine/animation/IAnimStateMachine.hpp"
#include "engine/animation/IRootMotion.hpp"
#include "engine/animation/IIkSolver.hpp"
#include "engine/animation/IInertializer.hpp"
#include "engine/animation/IMotionMatcher.hpp"
#include "engine/animation/IPoseWarper.hpp"
#include "engine/animation/IProceduralAnimationPipeline.hpp"
#include "engine/animation/IProceduralLegs.hpp"
#include "engine/animation/IConstraints.hpp"
#include "engine/animation/ITerrainAdaptation.hpp"
#include "engine/animation/IFootPlacement.hpp"
#include "engine/animation/IGaitPlanner.hpp"
#include "engine/animation/ISkinning.hpp"
#include "engine/animation/IMotionDatabase.hpp"
#include "engine/animation/IAiGraphValidation.hpp"
#include "engine/gameplay/ICharacterController.hpp"
#include "engine/gameplay/IInteraction.hpp"
#include "engine/gameplay/IBalance.hpp"
#include "engine/gameplay/IFaction.hpp"
#include "engine/gameplay/IHitReaction.hpp"
#include "engine/ai/IAiDebugInfo.hpp"
#include "engine/ai/IVendorBehaviorTree.hpp"
#include "engine/navigation/IHierarchicalPath.hpp"
#include "engine/navigation/IAgentCapabilities.hpp"
#include "engine/simulation/IFixedTickSim.hpp"
#include "engine/simulation/ISimulationLod.hpp"
#include "engine/networking/INetworkGameClient.hpp"
#include "engine/networking/INetworkDiscovery.hpp"
#include "engine/networking/INetworkInterest.hpp"
#include "engine/networking/INetworkRpc.hpp"
#include "engine/networking/INetworkReplication.hpp"
#include "engine/physics/IConvexDecomposition.hpp"
#include "engine/physics/IMultibodyDynamics.hpp"
#include "engine/physics/IShapeRecognition.hpp"
#include "engine/physics/IExplosion.hpp"
#include "engine/capabilities/ICapabilityRegistry.hpp"
#include "engine/director/IWorldDirector.hpp"
#include "engine/registry/IEquipment.hpp"
#include "engine/registry/ILootTable.hpp"
#include "engine/voxel/IVoxelBlockEntity.hpp"
#include "engine/navigation/INavStreaming.hpp"
#include "engine/navigation/INavigationProvider.hpp"
#include "engine/navigation/INavInvalidation.hpp"
#include "engine/navigation/INavigationSchedulerBridge.hpp"
#include "engine/procgen/IWorldFeatures.hpp"
#include "engine/procgen/IWorldProfile.hpp"
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Minimal world view the public mob behavior needs, adapted from the app's
// internal simulation World (FALTANTES item 11 — the entity layer never
// couples to the simulation World). Mobs are IEntityWorld entities advanced
// by IMobBehavior; the legacy Mob/MobManager track is gone.
class AppWorldMobQuery final : public engine::entity::IMobWorldQuery {
public:
    explicit AppWorldMobQuery(const World& world) : world_(world) {}
    uint32_t block_at(int x, int y, int z) const override {
        return static_cast<uint32_t>(world_.get_block_at(glm::vec3(
            static_cast<float>(x), static_cast<float>(y),
            static_cast<float>(z))));
    }
    bool is_fluid_block_at(int x, int y, int z) const override {
        return world_.is_fluid_block_at(glm::vec3(
            static_cast<float>(x), static_cast<float>(y),
            static_cast<float>(z)));
    }
    float fluid_damage_per_second_at(int x, int y, int z) const override {
        const FluidParams* params = world_.fluid_params_at(glm::ivec3(x, y, z));
        return params != nullptr ? params->damagePerTick : 0.0f;
    }

private:
    const World& world_;
};

struct FrameData {
    VkCommandPool commandPool;
    VkCommandBuffer mainCommandBuffer;
    VkFence renderFence;
    VkSemaphore swapchainSemaphore;
    // 2 queries per timed pass (frame-start + pass-start) — sized generously to
    // hold every per-pass boundary the frame records. Both the pool and the
    // readback buffer live per frame so the previous frame's query results are
    // still valid while the current frame is being recorded/submitted.
    VkQueryPool timestampPool{ VK_NULL_HANDLE };
};

inline constexpr std::uint32_t kFrameTimestampSlots = 16;  // 2 * 8 timed passes

// AGENTE 2 block G: maps a broken RuntimeBlockId to its data-driven item
// name (defined in VulkanEngineApp.cpp; used by EngineLifecycle.cpp's
// break-block drop logic and by VulkanEngineApp for the ItemRegistry grant).
const char* block_type_item_name(RuntimeBlockId id);

class VulkanEngineApp {
public:
    bool isInitialized{ false };
    bool isPaused{ false };
    bool escWasPressed{ false };
    bool showGraphicsMenu{ false };
    bool cinematicEffects{ true };
    bool depthOfFieldEnabled{ true };
    bool thirdPerson{ false };
    bool f5WasPressed{ false };
    bool fullscreenWasPressed{ false };
    bool framebufferResized{ false };
    bool fullscreen{ false };
    int windowedX{ 100 };
    int windowedY{ 100 };
    int windowedWidth{ 1280 };
    int windowedHeight{ 720 };
    bool lodQuality0WasPressed{ false };
    bool lodQuality9WasPressed{ false };
    double lodQualityHeldSince{ 0.0 };
    double lodQualityNextRepeat{ 0.0 };

    int frameNumber{ 0 };
    VkExtent2D windowExtent{ 1280, 720 };
    struct GLFWwindow* window{ nullptr };

    VkInstance instance;
    VkDebugUtilsMessengerEXT debugMessenger;
    VkPhysicalDevice physicalDevice;
    VkDevice device;
    VkSurfaceKHR surface;

    VkSwapchainKHR swapchain;
    VkFormat swapchainImageFormat;
    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> swapchainImageViews;
    VkExtent2D swapchainExtent;

    VkImage depthImage;
    VmaAllocation depthAllocation;
    VkImageView depthImageView;
    VkImage shadowImage{ VK_NULL_HANDLE };
    VmaAllocation shadowAllocation{};
    VkImageView shadowImageView{ VK_NULL_HANDLE };
    VkSampler shadowSampler{ VK_NULL_HANDLE };
    VkImage minimapImage{ VK_NULL_HANDLE };
    VmaAllocation minimapAllocation{};
    VkImageView minimapImageView{ VK_NULL_HANDLE };
    VkImage minimapDepthImage{ VK_NULL_HANDLE };
    VmaAllocation minimapDepthAllocation{};
    VkImageView minimapDepthView{ VK_NULL_HANDLE };
    VkSampler minimapSampler{ VK_NULL_HANDLE };

    VkImage hdrImage{ VK_NULL_HANDLE };
    VmaAllocation hdrAllocation{ VK_NULL_HANDLE };
    VkImageView hdrImageView{ VK_NULL_HANDLE };
    VkImage opaqueSceneImage{ VK_NULL_HANDLE };
    VmaAllocation opaqueSceneAllocation{ VK_NULL_HANDLE };
    VkImageView opaqueSceneImageView{ VK_NULL_HANDLE };
    VkImage opaqueDepthImage{ VK_NULL_HANDLE };
    VmaAllocation opaqueDepthAllocation{ VK_NULL_HANDLE };
    VkImageView opaqueDepthImageView{ VK_NULL_HANDLE };
    VkSampler waterSceneSampler{ VK_NULL_HANDLE };
    VkSampler postSampler{ VK_NULL_HANDLE };
    VkDescriptorSetLayout postDescriptorLayout{ VK_NULL_HANDLE };
    VkDescriptorPool postDescriptorPool{ VK_NULL_HANDLE };
    VkDescriptorSet postDescriptorSet{ VK_NULL_HANDLE };

    std::vector<VkSemaphore> renderSemaphores;

    FrameData frames[FRAME_OVERLAP];
    FrameData& get_current_frame() { return frames[frameNumber % FRAME_OVERLAP]; }

    VkQueue graphicsQueue;
    uint32_t graphicsQueueFamily;

    VmaAllocator allocator;

    VkPipelineLayout voxelPipelineLayout;
    VkPipeline voxelPipeline;
    VkPipeline farSurfacePipeline{ VK_NULL_HANDLE };
    VkPipeline waterPipeline{ VK_NULL_HANDLE };
    VkPipeline grassPipeline{ VK_NULL_HANDLE };
    VkPipeline foliagePipeline{ VK_NULL_HANDLE };
    VkPipeline skyPipeline{ VK_NULL_HANDLE };
    VkPipeline postPipeline{ VK_NULL_HANDLE };
    VkPipeline shadowPipeline{ VK_NULL_HANDLE };
    VkPipeline shadowFarSurfacePipeline{ VK_NULL_HANDLE };
    VkPipeline shadowFoliagePipeline{ VK_NULL_HANDLE };
    VkPipeline shadowGrassPipeline{ VK_NULL_HANDLE };
    VkPipelineLayout postPipelineLayout{ VK_NULL_HANDLE };

    glm::vec3 currentSunDirection{ 0.42f, 0.86f, 0.29f };
    glm::vec3 currentLightColor{ 1.18f, 1.10f, 0.94f };
    float currentDaylight{ 1.0f };
    float currentExposure{ 1.1f };
    // F.1/F.2: real atmosphere + cloud outputs consumed by the sky pass — the
    // IAtmosphereScattering spectral transmittance (sun elevation) and the
    // IVolumeClouds coverage, pushed into the sky shader's push constants.
    float skySunTransmittance{ 0.30f };
    float skyCloudCoverage{ 0.5f };
    float worldVisualTime{ 0.0f };
    glm::vec3 previousCameraFront{ 0.0f, 0.0f, -1.0f };
    // ── Conta 2 (L73 reaberto): SOMBRAS DE NUVEM REAIS + REPROJEÇÃO TEMPORAL ──
    // A cobertura estática do IVolumeClouds (config.coverage) alimenta apenas o
    // céu. Para as sombras no MUNDO, mantemos um campo de oclusão de nuvem no
    // espaço do mundo gerado a partir do núcleo real (march vertical por célula
    // do grid) e acumulado temporalmente com reprojeção pela translação da
    // câmera entre frames. O histórico é invalidado nos TRÊS eventos temporais
    // (resize, corte/teleport, origin rebase) no mesmo ponto em que os
    // denoiser/ReSTIR resetam — nunca reprojeta sobre descontinuidade. O campo
    // é publicado em gpuFeatures.atmosphere.z (cobertura de sombra média
    // estável) e temporal.yz (retroprojeção válida) e consumido por voxel.frag
    // como a sombra de nuvem do mundo (substituindo o padrão de seno puro).
    static constexpr std::uint32_t kCloudShadowGrid = 32u;          // 32x32 células
    static constexpr float kCloudShadowCellM = 24.0f;               // 768 m de cobertura
    static constexpr float kCloudShadowTopM = 300.0f;               // topo do band
    static constexpr float kCloudShadowBaseM = 150.0f;              // base do band
    // Campo acumulado (grid*grid, 1..0 = oclusão de nuvem por célula) e a origem
    // de mundo da célula [0][0] quando o campo é válido.
    std::vector<float> cloudShadowHistory_;
    bool cloudShadowFieldValid_{ false };
    glm::vec2 cloudShadowOrigin_{ 0.0f, 0.0f };
    float cloudShadowConfidence_{ 0.0f };   // 0..1: evolução temporal do campo
    glm::vec2 cloudShadowPrevCameraXZ_{ 0.0f, 0.0f };
    // Cobertura média estável publicada em gpuFeatures.atmosphere.z (a sombra
    // de nuvem real do frame, já temporalmente suavizada).
    float cloudShadowMeanCoverage_{ 0.0f };
    void invalidate_cloud_shadow_field();   // resize/cut/teleport/rebase -> zera histórico

    // Buffer da Malha do Braço do Personagem
    AllocatedBuffer armBuffer;
    uint32_t armVertexCount{ 0 };
    AllocatedBuffer heldBlockBuffer;
    uint32_t heldBlockVertexCount{ 0 };
    AllocatedBuffer characterBuffer;
    uint32_t characterVertexCount{ 0 };
    // L79 (reabertura): hair GPU render no jogo — o strand CPU (IHairPhysics,
    // simulado por frame) é renderizado como ribbon (VoxelVertex color-only,
    // uv.z=-1) pelo pipeline voxel existente na posição da cabeça do jogador.
    AllocatedBuffer hairBuffer;
    // L79: o hair buffer é criado UMA VEZ com capacidade fixa (o strand tem
    // até kMaxHairStrandNodes nós; cada segmento gera 6 vértices de ribbon).
    // Re-mapear/reescrever por frame evita destroy+create com FRAME_OVERLAP=2
    // (o frame anterior ainda pode estar em flight na GPU => use-after-free).
    static constexpr std::uint32_t kMaxHairStrandNodes = 64;
    std::uint32_t hairBufferCapacity{ 0u };
    std::uint32_t hairVertexCount{ 0 };
    // CONTA 4 item 5 (pose -> renderer): a pose-driven mesh buffer that the
    // renderer ACTUALLY draws every frame, rebuilt from the LIVE physics/
    // animation pose (ragdoll bone boxes + skinned foot targets + vehicle
    // chassis/boxes + debris) instead of a static CPU-only observable. Fixed
    // capacity allocated once (FRAME_OVERLAP=2 safe: map/rewrite per frame,
    // never destroy/recreate during flight).
    AllocatedBuffer showcasePoseBuffer;
    std::uint32_t showcasePoseVertexCount{ 0u };
    static constexpr std::uint32_t kShowcasePoseVertexCapacity = 1024u;
    // L39: samplers retirados pelo streaming de residência não podem ser
    // destruídos imediatamente (o frame anterior ainda pode estar em flight
    // usá-los com FRAME_OVERLAP=2). São destruídos no draw() depois do
    // vkWaitForFences garantir que o fence do frame antigo sinalizou.
    struct RetiredSampler { VkSampler sampler; std::uint64_t frame; };
    std::vector<RetiredSampler> retiredSamplers;

    SoundEngine soundEngine;
    MobRenderer mobRenderer;
    World world;
    WorldRenderer worldRenderer{world};
    AppWorldMobQuery mobQuery{world};
    // LOTE 1 — block entities/scheduler/noise/save/time-travel (A2 46-73).
    app::WorldLote1 worldLote1;
    std::string worldLote1GameShow;      // observável por frame (título)
    bool lote1ProbeDone{ false };        // save+travel rodam uma vez no boot
    int lote1AttachTries{ 0 };           // limite de tentativas do probe

    std::unique_ptr<engine::entity::IEntityWorld> mobEntities;
    std::unique_ptr<engine::entity::IMobBehavior> mobBehavior;
    // Canonical world composition (AGENTE 2 block A): the game executable is a
    // REAL consumer of the IWorldRuntime composition — same bind -> bootstrap
    // -> advance -> shutdown loop as play mode and server. The ECS is the SAME
    // mobEntities world the mobs live in; the gameplay integration drives its
    // fixed tick (physics + worlds + event router + navigation queries + audio
    // mapping) once per frame. Owned here; raw pointers are bound into the
    // WorldServiceContext below.
    std::unique_ptr<engine::IWorldRuntime> worldRuntime;
    std::unique_ptr<engine::gameplay::IGameplayRuntime> runtimePhysics;
    // AGENTE 2 block B: each LIVE mob ECS entity is mirrored by ONE kinematic
    // body in the canonical gameplay runtime (the ECS is the motion authority;
    // the body reflects its transform every frame so physics-queries/collisions
    // see the mob). Despawned mobs destroy their body (no orphan bodies).
    std::unordered_map<std::uint32_t, engine::gameplay::BodyId> mobPhysicsBodies;
    std::size_t mobPhysicsBodyCount{ 0 };
    // AGENTE 2 block I.112 (perception): the player's sensor suite (vision /
    // hearing / proximity) fed by the LIVE mob ECS every frame — hostiles are
    // threats. Detections + nearest hostile are observable in the title.
    std::unique_ptr<engine::ai::IPerception> playerPerception;
    std::size_t perceptionDetections{ 0 };
    std::size_t perceptionMemory{ 0 };
    float nearestThreatDistance{ -1.0f };
    // AGENTE 2 block I.114 (FSM): a deterministic combat state machine for a
    // mob — driven by the SAME perception suite (nearest threat). The FSM core
    // is pure: tick() emits action ids the game maps to observables. State /
    // drained action are observable in the title (never mutates the ECS).
    std::unique_ptr<engine::ai::IFsm> mobFsm;
    std::string fsmState{ "idle" };
    std::string fsmLastAction{ "none" };
    std::uint64_t fsmTicks{ 0 };
    // AGENTE 2 block I.116 (behavior tree): a data-driven decision tree for a
    // mob, fed by the SAME perception suite via a caller-owned Blackboard. The
    // runtime is pure/deterministic — tick(dt, bb) returns the root status and
    // debug_trace() exposes the ordered node visits. Status + a trace id are
    // observable in the title (never mutates the ECS).
    std::unique_ptr<engine::ai::IBehaviorTree> mobTree;
    int treeStatus{ -1 };
    std::string treeTrace{ "" };
    // AGENTE 2 block I.118 (utility AI): a tactical selection core for a mob,
    // fed by the perception suite + day/night. select() returns the highest
    // utility action id deterministically; the chosen id + its utility are
    // observable in the title (never mutates the ECS).
    std::unique_ptr<engine::ai::IUtilityAi> utilityAi;
    std::string utilityAction{ "" };
    std::string utilityChoice{ "none" };
    // AGENTE 2 block I.119 (GOAP planner): a goal-planning core for a mob —
    // set_atom() the perception-derived facts, set_goal() the current intent,
    // plan() returns the lowest-cost action sequence deterministically. The
    // plan length / first step are observable in the title (never mutates the
    // ECS).
    std::unique_ptr<engine::ai::IPlanner> mobPlanner;
    int plannerLength{ 0 };
    std::string plannerStep{ "none" };
    std::string plannerGoal{ "none" };
    // AGENTE 2 block I.117 (IAiLod): per-entity AI LOD — the live mob ECS is
    // classified by distance to the player and budgets applied. The active
    // (Full/Reduced) vs dormant split per tick is observable in the title
    // (pure classification; never mutates the ECS update schedule).
    std::unique_ptr<engine::ai::IAiLod> aiLod;
    std::uint64_t lodFull{ 0 };
    std::uint64_t lodReduced{ 0 };
    std::uint64_t lodDormant{ 0 };
    // AGENTE 2 block I.120 (AI event bus): deterministic log of the AI
    // decisions — every FSM transition emits an {tick, source, kind, payload}
    // event and the game drains them per frame. The drained event count is
    // observable in the title (closes the engine/ai decision domain).
    std::unique_ptr<engine::ai::IAiEventBus> aiEventBus;
    std::uint64_t aiEventCount{ 0 };
    // AGENTE 2 block I.120 (crowd simulation): advances the live mob ECS as a
    // crowd population — Full/Reduced/Aggregate/Dormant tiers by distance, wake
    // up/sleep, bounded ticks per frame. Stateful & deterministic. The
    // active/dormant/woken split is observable in the title (the ECS stays the
    // entity store; the sim classifies and ticks its population copy).
    std::unique_ptr<engine::ai::ICrowdSimulation> crowd;
    std::uint64_t crowdActive{ 0 };
    std::uint64_t crowdDormant{ 0 };
    std::uint64_t crowdWoken{ 0 };
    // AGENTE 2 block B.2 (spatial partition): the live mob ECS is mirrored in
    // a real ISpatialIndex (uniform cells, deterministic queries) — each mob's
    // AABB is inserted/moved every frame and the player position queries the
    // cell (near-candidates count observable in the title). The index is a
    // real consumer of the same entities the renderer/physics consume.
    std::unique_ptr<engine::entity::ISpatialIndex> mobSpatial;
    std::size_t spatialNearCount{ 0 };
    // AGENTE 2 block B.5 (sleeping by relevance): mobs classified Dormant/
    // Aggregate by the AI LOD are FROZEN in the physics mirror (the kinematic
    // body stops receiving set_transform, but is NOT destroyed) while the ECS
    // keeps their full state — sleeping by relevance without losing state.
    std::unordered_set<std::uint32_t> sleepingMobIds;
    std::size_t sleepingMobCount{ 0 };
    // AGENTE 2 block H.107 (animation LOD): the IAnimationLod core classes
    // each mob by distance-derived relevance (full vs far tier) and decides
    // per-tick re-samples; the sampled vs frozen split is observable in the
    // title. Pure/deterministic — the mob renderer stays the pose owner.
    std::unique_ptr<engine::animation::IAnimationLod> animLod;
    engine::animation::AnimationLodSpec animLodSpec;
    std::unordered_map<std::uint32_t, engine::animation::AnimationLodState>
        animLodStates;  // caller-owned per-mob LOD state
    std::uint64_t animLodSampled{ 0 };
    std::uint64_t animLodFrozen{ 0 };
    // AGENTE 2 block I (capabilities): the capability registry enumerates the
    // REAL capabilities the product's agents/player/vehicles carry (walk /
    // jump / swim / climb / drive / interact...). The registered count is
    // observable in the title and the boot log.
    std::unique_ptr<engine::capabilities::ICapabilityRegistry> capabilityReg;
    std::size_t capabilityCount{ 0 };
    // AGENTE 2 block G (world director + weather): the IWorldDirector core
    // selects deterministic world events (storm / raid / festival) from the
    // clock + perception tags every frame; the chosen event and a weather
    // state derived from the SAME day/night clock are observable in the title.
    std::unique_ptr<engine::director::IWorldDirector> worldDirector;
    std::vector<engine::director::EventSelectionState> directorSelections;
    std::string directorEvent{ "none" };
    std::string directorUtility{ "0.00" };
    std::string weatherState{ "clear" };
    // AGENTE 2 block G.92 (ability effects): a validated ability-effect table
    // (kinds: ForceImpulse / DestroyBlock / Generic) that EMITS real events
    // into the SAME canonical IGameplayEvents bus the game publishes break/
    // place events into — each block break also emits the configured effect.
    // The effect count is observable in the title.
    std::unique_ptr<engine::gameplay::IAbilityEffects> abilityEffects;
    std::uint64_t abilityEffectCount{ 0 };
    // L92 cooldown: the kick ability effect is gated by a fixed-tick cooldown
    // (so rapid block breaks don't spam the canonical bus every tick) — real
    // cooldown semantics observable alongside abilityEffectCount.
    std::uint64_t abilityLastKickTick{ 0 };
    static constexpr std::uint64_t kAbilityKickCooldownTicks{ 20 };
    std::size_t abilityCooldownBlocks{ 0 };  // emits held by the cooldown
    // AGENTE 2 block A (aceleracao — gameplay showcase): the game executable
    // runs ONE deterministic fixed-step accumulator (IFixedTickSim) that gates
    // how many canonical fixed ticks advance per frame; the render interpolation
    // alpha is observable. This is the "fixed tick" stage of the showcase loop.
    std::unique_ptr<engine::simulation::IFixedTickSim> fixedTickSim;
    double showcaseFixedTickAlpha{ 0.0 };
    long showcaseTicksAccumulated{ 0 };
    // AGENTE 3 (aceleracao — network/persistence): the game hosts a LOCAL
    // session over the SAME public networking contracts the dedicated server
    // uses (session/transport/replication/RPC/interest/discovery) — no
    // singleplayer-only bypass. The INetworkGameClient owns the local
    // ITransport + INetworkSession + IClientPrediction; the discovery/interest/
    // rpc/entity-replication factories are consumed directly. Lifecycle is
    // explicit: connect -> tick -> disconnect in init/tick/shutdown.
    std::unique_ptr<engine::networking::INetworkGameClient> hostLocalClient;
    std::unique_ptr<engine::networking::INetworkDiscovery> hostLocalDiscovery;
    std::unique_ptr<engine::networking::INetworkInterest> hostLocalInterest;
    std::unique_ptr<engine::networking::INetworkRpc> hostLocalRpc;
    std::unique_ptr<engine::networking::INetworkReplication> hostLocalReplication;
    bool hostLocalNetOk{ false };
    std::size_t hostLocalRelevantEntities{ 0 };
    std::uint32_t hostLocalPredictedBlocks{ 0 };
    std::size_t hostLocalRollbacks{ 0 };
    std::string hostLocalNetSummary{ "net n/a" };
    // AGENTE 2 block A (showcase bootstrap): the game loads the ShowcaseGame
    // project by configuration (project.json -> initialScene -> scene entities/
    // camera) and spawns the showcase character as a PERSISTENT entity in the
    // SAME canonical ECS the mobs live in (stable id + transform + health).
    // Input is data-driven through the public IActionMap (no raw key checks in
    // the showcase path) — WASD/jump/interact resolve to actions that drive the
    // character controller, camera and interaction system.
    std::string showcaseProjectName{ "n/a" };
    std::string showcaseInitialScene{ "n/a" };
    Engine::Scene showcaseScene;
    bool showcaseSceneLoaded{ false };
    std::size_t showcaseSceneEntities{ 0 };
    bool showcaseSceneHasCamera{ false };
    std::unique_ptr<engine::input::IActionMap> showcaseActionMap;
    std::string showcaseLastAction{ "none" };
    engine::entity::EntityId showcasePlayerEntity;
    bool showcasePlayerEntityValid{ false };
    float showcasePlayerHealth{ 20.0f };
    std::string showcaseSelectedBlock{ "dirt" };
    // AGENTE 2 block B (aceleracao — character): the player-controlled character
    // drives a real ICharacterController each fixed tick (terrain samples from
    // the LIVE voxel world) and a full deterministic animation stack built on a
    // canonical IAnimCore (skeleton + clips from the showcase gait asset):
    // additive layer, named events, state machine, root motion, inertializer,
    // motion matcher, pose warper, procedural pipeline + procedural legs + IK +
    // constraints + terrain adaptation. Ragdoll/hit-reaction run against the
    // SAME runtime physics. All are REAL product consumers, reported in title.
    std::unique_ptr<engine::animation::IAnimCore> animCore;
    std::unique_ptr<engine::animation::IAnimAdditive> animAdditive;
    std::unique_ptr<engine::animation::IAnimEvents> animEvents;
    std::unique_ptr<engine::animation::IAnimMask> animMask;
    std::unique_ptr<engine::animation::IAnimStateMachine> animStateMachine;
    std::unique_ptr<engine::animation::IRootMotion> animRootMotion;
    std::unique_ptr<engine::animation::IIkSolver> animIkSolver;
    std::unique_ptr<engine::animation::IInertializer> animInertializer;
    std::unique_ptr<engine::animation::IMotionMatcher> motionMatcher;
    std::unique_ptr<engine::animation::IPoseWarper> poseWarper;
    std::unique_ptr<engine::animation::IProceduralAnimationPipeline> proceduralPipeline;
    std::unique_ptr<engine::animation::IProceduralLocomotion> proceduralLegs;
    std::unique_ptr<engine::animation::IConstraints> animConstraints;
    std::unique_ptr<engine::animation::ITerrainAdaptation> terrainAdaptation;
    std::unique_ptr<engine::animation::IFootPlacer> footPlacer;
    std::unique_ptr<engine::animation::IContactPlanner> contactPlanner;
    std::vector<engine::animation::BonePose> animBasePose;
    double showcaseAnimClock{ 0.0 };
    engine::animation::AnimStateMachineSpec animStateSpec;
    std::string animStateMachineState{ "n/a" };
    std::string animEventLast{ "none" };
    double rootMotionDistance{ 0.0 };
    std::size_t motionMatchFrames{ 0 };
    std::size_t proceduralEffectorCount{ 0 };
    bool showcaseGaitLoaded{ false };
    engine::animation::GaitAsset showcaseGait;
    // AGENTE 2 block B (character controller): the kinematic character resolver
    // over live terrain + the combat hit-reaction model — observables in title.
    std::unique_ptr<engine::gameplay::ICharacterController> showcaseController;
    bool showcaseSteppedUp{ false };
    bool showcaseSnappedDown{ false };
    bool showcaseInWater{ false };
    std::unique_ptr<engine::gameplay::IHitReaction> showcaseHitReaction;
    engine::gameplay::HitState showcaseHitState{ engine::gameplay::HitState::Normal };
    std::string showcaseHitStateName{ "normal" };
    std::unique_ptr<engine::gameplay::IRagdoll> showcaseRagdoll;
    std::size_t showcaseRagdollBones{ 0 };
    // LOTE 2 sprint (física+animação): observables reais por item.
    // 84/86: ragdoll -> skeleton pose (read the live physics ragdoll pose and
    // feed it back to the animation core) + a body-count guard that proves the
    // personagem<->ragdoll transition never spawns a duplicate body (the claw:
    // the ECS mirror body is either kinematic OR replaced once by the ragdoll,
    // never both alive at the same time).
    std::size_t workshopRagdollPoseBones{ 0 };
    std::size_t showcaseRagdollPoseBones{ 0 };  // L2 ragdoll pose read-back
    std::size_t showcaseRagdollBodyGuard{ 0 };
    // 102 skinning CPU: public ISkinning consumes the SAME animCore pose the
    // ASM/motion-matcher produce — one skinned mesh over the live pose.
    std::unique_ptr<engine::animation::ISkinning> showcaseSkinning;
    std::size_t showcaseSkinnedVerts{ 0 };
    // CONTA 4 item 5: the last skinned/deformed foot targets (world space)
    // from the live pose. Kept on the app so the renderer's pose mesh draws
    // the actual deformation output, not a CPU-only number.
    std::vector<glm::vec3> showcaseLastSkinnedFoots;
    // 108 skeleton real: public IMotionDatabase cooks+samples the canonical
    // showcase skeleton/clip (ozz), replacing any two-bone fallback outside
    // an explicit error path.
    std::unique_ptr<engine::animation::IMotionDatabase> showcaseMotionDb;
    std::size_t showcaseMotionDbBones{ 0 };
    std::size_t showcaseMotionDbFrames{ 0 };
    // 80 CCD/sleeping/triggers/layers: one CCD-enabled collision body in the
    // gameplay runtime + an awake/sleep toggle observable per frame.
    engine::gameplay::BodyId showcaseCcdBody;
    std::size_t showcaseCcdActive{ 0 };
    // 104 IK/foot/pose-warp: solving the live two-bone IK chain each tick on
    // real feet targets sampled from the voxel surface.
    std::size_t showcaseIkSolves{ 0 };
    // 94 equipment + loot (the rest of the items brought into the game): a
    // real equipment grid (helmet/chest/weapon slots) and a deterministic
    // loot table rolled on real breaks — observable in the title.
    std::unique_ptr<engine::registry::IEquipment> showcaseEquipment;
    std::unique_ptr<engine::registry::ILootTable> showcaseLoot;
    std::size_t showcaseEquippedSlots{ 0 };
    std::size_t showcaseLootRolls{ 0 };
    // 95 ecosystem (IWorldFeatures): a data-driven decorator set that places
    // real blocks on live columns — observable by decorator application count.
    std::shared_ptr<engine::procgen::IOreTable> showcaseOreTable;
    std::shared_ptr<engine::procgen::ICarver> showcaseCarver;
    std::shared_ptr<engine::procgen::IDecoratorSet> showcaseDecorators;
    std::size_t showcaseFeaturePlaces{ 0 };
    // CONTA 4 item 2 (block entity full lifecycle): beyond creation/tick/save
    // (WorldLote1 + ShowcaseBlockEntity already in the game), the CLOSING
    // cycles are wired on the LIVE World — a block-entity listener observes
    // attach/detach on the real voxel world and the game BREAKS a block entity
    // with a real key (KeyB) so unload/destruction run through the canonical
    // World::remove_block_entity (the same path the load reconcile uses). The
    // observables are real: attached/detached counts + a removal attempt
    // counter.
    std::unique_ptr<class ShowcaseBlockEntity> showcaseBlockEntity;
    std::size_t showcaseBlockEntityTicks{ 0 };
    std::size_t blockEntityAttachedObserved{ 0 };
    std::size_t blockEntityDetachedObserved{ 0 };
    std::size_t blockEntityRemoveAttempts{ 0 };
    // CONTA 4 items 2/3: how many World-attached block entities the unified
    // save/load round trip restored on this boot (0 when none were persisted).
    std::size_t showcaseWorldBlockEntitiesRestored{ 0 };
    // CONTA 4 item 6 (missions): the PUBLIC IMissionRuntime is consumed by the
    // GAME executable — a data-driven mission (collect objective driven by the
    // REAL player inventory counts) is accepted, advanced, completed and its
    // reward applied to the SAME live Inventory every fixed tick. Progress is
    // observable in the title and the state is persisted in the unified save
    // (no silent reset). The IMissionWorld seam reads the real voxel world +
    // player position + inventory so the mission is coupled to the running
    // game, not a SDK-only probe.
    std::unique_ptr<engine::gameplay::IMissionRuntime> showcaseMissions;
    engine::gameplay::MissionDefinition showcaseMissionDef;
    engine::gameplay::MissionState showcaseMissionState;
    bool showcaseMissionAccepted{ false };
    bool showcaseMissionCompleted{ false };
    std::size_t showcaseMissionEvents{ 0 };
    std::string showcaseMissionSummary;
    // CONTA 4 item 5 (vehicle pose): the game assembles a REAL vehicle from a
    // VehicleAsset through IGameplayRuntime::create_vehicle_from_asset (the
    // same factory the server uses), drives it with real input every fixed
    // tick and reads its chassis pose — the observable proves the game
    // executable consumes the vehicle runtime, not just SDK/editor/server.
    std::unique_ptr<engine::gameplay::IVehicle> showcaseVehicle;
    std::unique_ptr<engine::vehicles::VehicleAsset> showcaseVehicleAsset;
    bool showcaseVehicleValid{ false };
    float showcaseVehicleSpeed{ 0.0f };
    std::size_t showcaseVehicleWheels{ 0 };
    std::size_t showcaseVehicleOccupants{ 0 };
    std::string showcaseVehicleSummary;
    // Reads the live chassis world position from the runtime physics (0,0,0
    // when the vehicle/body is unavailable — an explicit empty fallback, never
    // a guessed pose).
    glm::vec3 showVehicleChassisPos() const;
    // 115 nav streaming: the ledged active-region gate fed by the player's
    // focus tile each frame — observable loaded/invalid counts.
    std::unique_ptr<engine::navigation::INavStreaming> showcaseNavStream;
    std::size_t showcaseNavLoaded{ 0 };
    std::size_t showcaseNavPendingRebuild{ 0 };
    // CONTA 3 — navigation & AI integrados aos agentes vivos (items 117/118).
    // The GAME owns a real INavigationProvider (Recast+Detour) baked from the
    // live voxel world columns, carrying off-mesh links (jump/climb), area
    // costs (hazards), a dynamic door obstacle and async path queries. Each
    // live mob ECS agent is a path-following consumer: begin_async_path →
    // poll_async_path per frame, cancelling the in-flight request on any of
    // (new request, despawn, invalid target, world change) so a late result
    // is never applied after cancellation.
    std::unique_ptr<engine::navigation::INavigationProvider> gameNav;
    std::unique_ptr<engine::navigation::INavInvalidation> gameNavInvalidation;
    std::uint64_t gameNavRevision{ 0 };
    std::size_t gameNavOffMeshLinks{ 0 };
    std::size_t gameNavAreaCosts{ 0 };
    std::size_t gameNavObstacleColumns{ 0 };
    bool gameNavDoorActive{ false };
    bool gameNavBaked{ false };
    // Per-agent nav runtime state keyed by mob ECS entity id: the live
    // async request id (0 = none) + the last success/failure so the game can
    // steer mobs along the resolved route and cancel stale queries.
    struct GameNavAgent {
        std::uint64_t requestId{ 0 };
        engine::navigation::PathResult result;
        bool pathActive{ false };
        std::size_t waypoint{ 0 };
        glm::vec3 goal{ 0.0f, 0.0f, 0.0f };
        std::uint64_t requestedAtTick{ 0 };
    };
    std::unordered_map<std::uint32_t, GameNavAgent> gameNavAgents;
    std::size_t gameNavAgentsFollowing{ 0 };
    std::size_t gameNavCancelled{ 0 };
    std::size_t gameNavFailureCount{ 0 };
    // CONTA 3 — snapshot de debug de IA por frame (items 117/118/120): the
    // SAME live ECS mob agents feed an IAiDebugRecorder every fixed tick with
    // their perception/blackboard/FSM/behavior-tree/utility/planner state, and
    // the JSON snapshot is published for the editor consumer. The recorder is
    // bound to each agent serially (one snapshot per tick = the agent the game
    // focused, deterministic).
    std::string aiDebugSnapshotJson{ "{}" };
    std::size_t aiDebugSnapshotAgents{ 0 };
    void showcase_gameplay_nav_init();
    void showcase_gameplay_nav_tick(float fixedDt);
    void showcase_gameplay_nav_invalidate();
    void showcase_gameplay_ai_debug_snapshot();
    // 54 world-gen graphs de assets: the data-driven world profile (compone o
    // generator/structures do mundo) + quantas seções o DefaultWorld.json
    // realmente carrega + se um mundo gerenciado foi criado a partir dele.
    std::shared_ptr<engine::procgen::IWorldProfile> showcaseWorldProfile;
    std::size_t showcaseWorldProfileSections{ 0 };
    std::size_t showcaseWorldProfileWorld{ 0 };
    // 108 skeleton real de asset: the character skeleton asset JSON exists in
    // the project (Content/Registry/showcase_character_skeleton.json, 7 bones)
    // and the motion database cooks the canonical skeleton from it — this
    // observable proves the asset is present and loaded.
    std::size_t showcaseSkeletonAssetLoaded{ 0 };
    // 92 abilities in the tick: a Teleport ability is EMITTED on the game's
    // IGameplayEvents bus when the player presses a hotkey (and not staggered),
    // proving effect->event feedback with a real observable count.
    std::size_t showcaseAbilityEmits{ 0 };
    std::unique_ptr<engine::gameplay::IWeapon> showcaseRifle;
    std::uint32_t showcaseWeaponAmmo{ 0 };
    // AGENTE 2 block C (gameplay showcase): interaction defs evaluated against
    // the player, deterministic explosion blast (falloff + fragments) fed from
    // real world distance, faction relations, and the AI decision surface
    // (debug recorder + AI clip/graph validator + vendored behavior tree).
    std::unique_ptr<engine::gameplay::IInteraction> showcaseInteraction;
    std::size_t interactionAvailable{ 0 };
    bool interactionActivated{ false };
    std::unique_ptr<engine::physics::IExplosion> showcaseExplosion;
    std::size_t explosionFragments{ 0 };
    float explosionFalloffAtPlayer{ 1.0f };
    std::unique_ptr<engine::gameplay::IBalance> showcaseBalance;
    engine::gameplay::BalanceState showcaseBalanceState{ engine::gameplay::BalanceState::Stable };
    std::unique_ptr<engine::gameplay::IFaction> showcaseFaction;
    std::size_t showcaseTeamCount{ 0 };
    std::unique_ptr<engine::ai::IAiDebugRecorder> aiDebugRecorder;
    std::size_t aiDebugNodeCount{ 0 };
    std::unique_ptr<engine::animation::IAiGraphValidator> aiValidator;
    bool aiValidatorSigned{ false };
    std::unique_ptr<engine::ai::IVendorBehaviorTree> vendorTree;
    int vendorTreeStatus{ -1 };
    // AGENTE 2 block C (simulation): hierarchical path over a REAL region nav
    // graph of chunks, agent capability traversal on project terrain, and
    // simulation-LOD over live world regions near the player.
    std::unique_ptr<engine::navigation::IHierarchicalPath> hierarchicalPath;
    bool navPathFound{ false };
    std::size_t navPathNodes{ 0 };
    std::unique_ptr<engine::navigation::IAgentCapabilities> agentCapabilities;
    bool agentCanTraverse{ false };
    std::string agentTraverseReason{ "" };
    std::unique_ptr<engine::simulation::ISimulationLod> simulationLod;
    engine::simulation::SimulationLodState simulationLodState;
    std::size_t simulationLodEvents{ 0 };
    // AGENTE 2 block D (physics selection): convex decomposition of a REAL
    // voxel-built concave mesh, an articulated multibody chain stepped on the
    // fixed tick, and shape recognition over real world points. Only the three
    // deterministic cores consumed by the showcase are wired; the unowned
    // backend announcement is removed.
    std::unique_ptr<engine::physics::IConvexDecomposition> convexDecomposition;
    std::size_t convexPartCount{ 0 };
    std::string convexBackendName{ "n/a" };
    std::unique_ptr<engine::physics::IMultibodyDynamics> multibody;
    engine::physics::MultibodyHandle multibodyChain{ engine::physics::InvalidMultibody };
    std::size_t multibodyLinks{ 0 };
    float multibodyEndEffectorY{ 0.0f };
    std::unique_ptr<engine::physics::IShapeRecognition> shapeRecognition;
    std::size_t shapePrimitiveCount{ 0 };
    // AGENTE 2 block B (skin via asset registry): the character skin is a REAL
    // IAssetPipeline import -> validate -> cook -> cache round trip (the
    // registry path); the cooked skin layout (name + UV origin/size) is applied
    // to the character's 64x64 player-skin UV grid. When the asset file is
    // absent the import is refused and the pipeline falls back to the builtin
    // layout explicitly (observable, never silent).
    std::unique_ptr<engine::assets::IAssetPipeline> showcaseAssets;
    std::string showcaseSkinName{ "builtin" };
    std::string showcaseSkinLayout{ "64x64" };
    bool showcaseSkinFallback{ true };
    std::size_t showcaseAssetCacheHits{ 0 };
    // AGENTE 2 block C (voxel journal): every committed atomic transaction in
    // the showcase (break/place/explosion) is recorded as {position, blockId};
    // the save file persists it and the load path re-applies it through the
    // same transaction API once the target chunks are writable — the round
    // trip preserves the altered blocks, not a full-world dump.
    std::vector<std::pair<glm::ivec3, uint32_t>> showcaseBlockJournal;
    std::size_t showcaseJournalCommits{ 0 };
    // AGENTE 2 block C (explosion): deterministic periodic detonation at the
    // nearest hostile — the blast modifies REAL blocks (atomic transaction,
    // falloff-gated) and REAL physics bodies (impulse + entity damage), with
    // the committed edits journaled for persistence.
    std::uint64_t showcaseExplosionNextTick{ 180 };
    std::size_t showcaseExplosionBlockEdits{ 0 };
    std::size_t showcaseExplosionBodiesHit{ 0 };
    // AGENTE 2 block C (save/load): round-trip observables — the save file
    // path, whether a load actually restored state this boot, and how many
    // block edits are pending re-application (deferred until chunks stream).
    std::string showcaseSavePath;
    bool showcaseSaveLoaded{ false };
    std::size_t showcasePendingBlockEdits{ 0 };
    // Load-relative replay of the block-edit journal. On load, the cumulative
    // journal from the save is appended and `showcaseLoadEditCount` marks how
    // many LEADING entries are load-relative; the replay re-applies exactly
    // those in order (advancing `showcaseReplayCursor`) and then stops — live
    // edits made after boot append beyond that marker and are NEVER replayed
    // (they are already applied in the world). The journal is CUMULATIVE and
    // never erased, so each save persists the full history and every later
    // load re-applies it, preserving altered blocks across any number of
    // sessions (an erase-on-replay design would drop edits from earlier
    // sessions, reverting them to freshly generated terrain).
    std::size_t showcaseReplayCursor{ 0 };
    std::size_t showcaseLoadEditCount{ 0 };
    std::string showcaseSummary{ "" };
    void showcase_gameplay_init();
    void showcase_gameplay_tick(float fixedDt);

    // Cached surface-height query used by the per-tick systems (character
    // controller samples, agent capabilities, hit-reaction groundedness,
    // pose-warper feet and the shape-recognition grid). The old free function
    // re-scanned the whole voxel column (up to ~91 get_block_at calls, each
    // locking the world chunk mutex) on EVERY call — dozens per fixed tick.
    // This memoizes the topmost-solid height per column: only the FIRST query
    // per column per edit-epoch scans; repeats are O(1) cache hits.
    // Correctness: block edits and the save-load replay invalidate the cache;
    // columns that resolve to the air fallback (not-yet-streamed chunk) are
    // never cached, so they keep re-scanning until their chunk uploads.
    float showcaseSurfaceAt(const World& world, float wx, float wz);

    // J.125 occlusion sampler: raycasts the LIVE voxel world from `from` to
    // `to` and returns an occlusion value [0,1] (fraction of solid samples)
    // that feeds ISpatialAudio as the per-source occlusion INPUT. Deterministic
    // and cheap (a bounded DDA walk capped at maxSamples).
    static float showcaseAudioOcclusion(const World& world,
                                        const glm::vec3& from,
                                        const glm::vec3& to, int maxSamples = 24);
    void invalidateShowcaseSurfaces();
    std::unordered_map<long long, float> showcaseSurfaceLod;
    std::size_t showcaseSurfaceCacheHits{ 0 };
    std::size_t showcaseSurfaceCacheMisses{ 0 };
    // Reintegração (116+106): quanto dos eixos de input o enforcement de
    // capabilities + hit-reaction bloqueou no último frame (0 = permissivo).
    // Observable no título — prova que registry/hit-reaction MUDAM o movimento.
    std::size_t trackedEnforcedAxes{ 0 };
    void showcase_gameplay_shutdown();
    void showcase_gameplay_save();
    void showcase_gameplay_load();
    // Reintegração (91): ação de craft real no jogo — consome os insumos da
    // receita satisfazível do inventário VIVO e adiciona os produtos (RecipeRegistry
    // autoritativo, atômico). Disparada pela action map (KeyC). Observável.
    void showcase_try_craft();
    std::string showcaseCraftResult{ "n/a" };
    // L91 furnace path: the crafting table (KeyC) and the fuel-consuming
    // furnace smelting station (KeyF) are REAL separate stations wired to the
    // same RecipeRegistry/Inventory, so recipes/stations/fuel/processing are
    // consumed as generic assets in the running game.
    void showcase_try_smelt();
    std::string showcaseSmeltResult{ "n/a" };
    std::size_t smokableRecipeCount{ 0 };  // furnace-satisfiable recipes
    // AGENTE 2 block I (steering): per-frame aggregate steering force of the
    // LIVE mob ECS toward the player (seek) with flocking separation, plus the
    // resulting blend magnitude — observable in the title, purely computed
    // (never mutates the mob ECS, which stays the motion authority).
    float mobSteeringForce{ 0.0f };
    std::size_t steeringMobCount{ 0 };
    std::unique_ptr<engine::world::IWorldManager> runtimeWorlds;
    std::unique_ptr<engine::gameplay::IGameplayIntegration> runtimeIntegration;
    std::unique_ptr<engine::gameplay::IGameplayBindings> runtimeBindings;
    std::unique_ptr<engine::gameplay::IGameplaySystemWiring> runtimeWiring;
    std::unique_ptr<engine::gameplay::IGameplayEvents> runtimeEvents;
    std::unique_ptr<engine::gameplay::IGameplayMetrics> runtimeMetrics;
    std::unique_ptr<engine::gameplay::IGameplayEventRouter> runtimeRouter;
    std::unique_ptr<engine::navigation::IAsyncQueryScheduler> runtimeQueries;
    std::unique_ptr<engine::audio::IAudioEventMapper> runtimeAudio;
    // Conta 5 (showcase data-driven): the project's audio event asset
    // (Content/AudioEvents/showcase_audio.json) parsed + registered into the
    // SoundEngine — observables published in the window title.
    std::string showcaseAudioAssetName;
    bool showcaseAudioAssetLoaded{ false };
    bool showcaseAudioRegistered{ false };
    // Conta 5 (showcase data-driven): the project's light set
    // (Content/Config/showcase_lights.json) applied as REAL scene light
    // components — observables published in the window title.
    bool showcaseLightsAssetLoaded{ false };
    std::size_t showcaseLightCount{ 0 };
    // Conta 5 (showcase data-driven): the project's ability + inventory +
    // item-registry assets. The ability definition loads through the public
    // IAbilitySystem and is registered; the inventory document is loaded and
    // validated (non-empty slots read) and the showcase item asset registers
    // into the SAME ItemRegistry the hotbar uses.
    std::unique_ptr<engine::gameplay::IAbilitySystem> showcaseAbilities;
    bool showcaseAbilityAssetLoaded{ false };
    std::size_t showcaseAbilityCount{ 0 };
    bool showcaseItemAssetLoaded{ false };
    bool showcaseInventoryAssetLoaded{ false };
    std::size_t showcaseInventorySlots{ 0 };
    std::size_t showcaseInventoryItems{ 0 };
    // The showcase inventory document deserialized (all-or-nothing) into a
    // dedicated Inventory sized to the asset — the full canonical consumer.
    std::unique_ptr<engine::registry::Inventory> showcaseInventory;
    bool showcaseInventoryDeserialized{ false };
    // Conta 5 (showcase data-driven): the project's jeep + mission registry
    // assets loaded through the canonical public loaders (VehicleAsset::
    // load_from_json / MissionDefinition::load_from_json) with validation —
    // observables published in the window title.
    bool showcaseJeepAssetLoaded{ false };
    std::size_t showcaseJeepWheels{ 0 };
    bool showcaseMissionAssetLoaded{ false };
    std::size_t showcaseMissionAssetObjectives{ 0 };
    // Conta 5 (showcase data-driven): the project's network declaration
    // (Content/Network/showcase_network.json, mirroring the public
    // DedicatedServerConfig contract) loaded + validated by the game — the
    // runtime ingestion belongs to the network domain, but the executable
    // reads the asset document.
    bool showcaseNetworkAssetLoaded{ false };
    std::size_t showcaseNetworkPort{ 0 };
    std::size_t showcaseNetworkMaxClients{ 0 };
    std::size_t showcaseNetworkTickRate{ 0 };
    // AGENTE 2 block G (day/night): the deterministic IDayNightCycle advanced
    // by the canonical IWorldRuntime every frame is the game's ONE clock — the
    // sun direction, daylight factor and light color below derive from it (no
    // wall clock in the lighting path). The same cycle object is bound into the
    // WorldServiceContext so play mode and server share the identical clock.
    std::unique_ptr<engine::gameplay::IDayNightCycle> dayNightCycle;
    // AGENTE 2 block J (audio): the deterministic spatializer consumes the
    // REAL mob entity positions + player listener every frame (J.125); the
    // adaptive-music core is driven by the same day/night clock the lighting
    // uses (J.127 — day state vs night state). Both are std-only SDK cores;
    // their observable state (active/virtualized sources, music state + layer
    // gain) is published in the window title.
    std::unique_ptr<engine::audio::ISpatialAudio> spatialAudio;
    std::unique_ptr<engine::audio::IAdaptiveMusic> adaptiveMusic;
    std::size_t spatialActiveSources{ 0 };
    std::size_t spatialVirtualizedSources{ 0 };
    std::string adaptiveMusicState;
    std::uint64_t runtimeTick{ 0 };
    std::size_t runtimeEntities{ 0 };
    std::size_t runtimeWorldCount{ 0 };
    std::string runtimeBootError;
    // AGENTE 2 block G (data-driven items): the game owns a real ItemRegistry
    // (loaded from the project's item assets) and a hotbar Inventory bound to
    // the player's selected block. Breaking a block adds its item stack to the
    // inventory (the drops path the legacy code stubbed); inventory state is
    // observable in the window title. Both are std-only SDK cores.
    std::unique_ptr<engine::registry::ItemRegistry> playerItems;
    std::unique_ptr<engine::registry::Inventory> playerInventory;
    std::unique_ptr<engine::registry::RecipeRegistry> playerRecipes;
    std::size_t craftableRecipeCount{ 0 };
    std::size_t recipeCount{ 0 };
    std::string playerInventorySummary;
    Player player;
    TextureManager textureManager;
    RadianceCache radianceCache;
    bool radianceCacheReady{ false };
    // L39 (reabertura): mip streaming/residency por distância. textureResidencyMips
    // = níveis de mip residentes (1..10 para o atlas 512²); o sampler do
    // material set tem maxLod amarrado a esse budget, recriado quando a
    // distância real de streaming cruza o limiar (câmera alta = menos mips
    // residentes, perto do chão = cadeia completa). Observável real do streaming.
    std::uint32_t textureResidencyMips{ 0 };
    float textureResidencyDistance{ -1.0f };
    void update_texture_residency();
    // Destroi samplers retirados pelo streaming (L39) quando nenhum frame em
    // flight os referencia mais — chamado logo após vkWaitForFences no draw.
    void reap_retired_samplers();
    // Conta 2 (L73): avança o campo de sombra de nuvem por frame — reamostra o
    // núcleo IVolumeClouds real num grid ao redor da câmera, reprojeta o
    // histórico pela translação e publica a cobertura média estável. Chamado
    // de refresh_gpu_features() antes do upload do feature buffer.
    void update_cloud_shadow_field();
    Engine::Rendering::GpuFeatureBinding gpuFeatureBinding{};
    Engine::Rendering::GpuRenderFeatures gpuFeatures{};
    Engine::Rendering::GpuFeaturePasses gpuFeaturePasses{};
    std::unique_ptr<Engine::Rendering::IProbeGrid> probeGrid;
    std::unique_ptr<Engine::Rendering::IReSTIRDI> restirDi;
    std::unique_ptr<Engine::Rendering::ITemporalDenoiser> temporalDenoiser;
    std::unique_ptr<Engine::Rendering::IRenderingDebugView> renderingDebugView;
    // A.3/A.4/E.5/H: the Lumen-style surface cache, the material-card capture
    // and the Embree-backed software tracer are REAL product producers. The
    // scene is fed per chunk by WorldRenderer::upload_chunk (mesh->surface
    // pass); the capture core samples real world light (sky+block) per card;
    // the tracer is rebuilt from the surface cards and traces real camera rays
    // (software tracing against the streaming-updated scene structure).
    std::unique_ptr<Engine::Rendering::ILumenScene> lumenScene;
    std::unique_ptr<Engine::Rendering::ISurfaceCacheCapture> surfaceCacheCapture;
    std::unique_ptr<vc::rendering::IRayTracer> lumenRayTracer;
    // E.6/H.6: the tracer is selected by capability — hardware Vulkan RT
    // (BLAS/TLAS ray queries) when the device exposes it, Embree software
    // otherwise. The provider actually selected is recorded here and logged.
    std::string rayTracerProvider;
    std::uint32_t lumenTraceFrame{ 0U };
    std::uint32_t lumenTraceEpoch{ 0U };
    // C.2: the game world consumes a REAL BlockRegistry-derived runtime table
    // (the facade's deterministic builder, wired into the raw simulation World
    // here). The mesher/renderer read variant/face/state/emission data from
    // the registry instead of only builtin enum decisions, and emissive
    // catalog blocks actually emit light into the world.
    std::unique_ptr<engine::registry::BlockRegistry> blockRegistry;
    std::string blockRegistryError;
    std::uint32_t registryBlockCount{ 0 };
    std::uint32_t registryEmissiveCount{ 0 };
    bool registryBlocksPlaced{ false };
    // E.11: last camera position used for the camera-cut (teleport) history
    // reset; a > 64 m jump invalidates temporal histories.
    glm::vec3 lastTemporalCameraPosition{ 0.0f, 0.0f, 0.0f };
    // E.4: the screen-space tracer is a REAL frame producer — it traces the
    // same camera rays as the software tracer against a real scene-depth
    // field derived from the actual voxel world (reprojection + disocclusion
    // against the previous frame's view-projection). Hits land in the debug
    // view; off-screen rays fall back to the software tracer.
    std::unique_ptr<Engine::Rendering::IScreenSpaceTracer> screenSpaceTracer;
    std::uint32_t screenTraceRays{ 0 };
    std::uint32_t screenTraceHits{ 0 };
    std::uint32_t screenTraceFallbacks{ 0 };
    glm::mat4 previousViewProjection{ 1.0f };
    std::unique_ptr<vc::rendering::IFluidSimulation> fluidSimulation;
    // L75 (reabertura): fluxo/velocidade REAIS do simulador fluido alimentando a
    // água visível. waterFlowMean = magnitude do fluxo (média das velocidades
    // por célula, 0..1) e waterFlowDirXZ = eixo dominante normalizado — ambos
    // extraídos do FluidState vivo por frame e expostos em gpuFeatures.fluids.w
    // (magnitude) + observáveis do título/diagnóstico.
    float waterFlowMean{ 0.0f };
    glm::vec2 waterFlowDirXZ{ 0.0f, 0.0f };
    // Per-frame ReSTIR DI state (AGENT-1 A.9): the deterministic core is driven
    // every frame over a real pixel sample of the scene around the player; the
    // previous frame's reservoirs and an age/animation counter feed temporal
    // reuse so the ordinate is a genuine running estimator, not a presence gate.
    std::vector<Engine::Rendering::RestirReservoir> restirPrevReservoirs;
    std::uint32_t restirFrameIndex{ 0U };
    // Per-frame temporal-denoiser history (AGENT-1 A.11): the ReBLUR-style
    // history converges across frames; its real mean confidence (history
    // length) drives gi.w instead of a provider-availability gate.
    std::vector<Engine::Rendering::DenoiserHistory> denoiserHistories;
    float denoiserConfidence{ 0.0f };
    // Real ReSTIR DI build-up ordinate (mean effective M over the frame's
    // sample grid, saturated into 0..1) consumed by the feature contract.
    float restirBuildUp{ 0.0f };
    std::unique_ptr<Engine::Rendering::IToneMapping> toneMapping;
    std::unique_ptr<Engine::Rendering::IAtmosphereScattering> atmosphere;
    std::unique_ptr<Engine::Rendering::IVolumeClouds> volumeClouds;
    std::unique_ptr<Engine::Rendering::IMaterialShading> materialShading;
    // C.20/vkfft seam: the deterministic ocean FFT cores (IFftCore + the
    // Tessendorf ocean-surface synthesizer) are driven each frame over the
    // real world time and produce the actual height field that deforms the
    // ocean surface. No phantom — the synthesized vertices + normals tail the
    // observable contract.
    std::unique_ptr<Engine::Rendering::IFftCore> fftCore;
    std::unique_ptr<Engine::Rendering::IFftOceanSurface> fftOcean;
    std::uint32_t oceanFftVertices{ 0 };
    float oceanFftPeakHeight{ 0.0f };
    // A.12: FSR-style edge-adaptive spatial upscaler — a real frame producer:
    // each frame it upscales the sampled scene (via tone-mapped color grid)
    // and publishes the output size to the feature contract.
    std::unique_ptr<Engine::Rendering::ISpatialUpscaler> spatialUpscaler;
    std::uint32_t upscaleOutWidth{ 0 };
    std::uint32_t upscaleOutHeight{ 0 };
    std::uint32_t upscaleSrcPixels{ 0 };
    std::uint32_t upscaleEnergyDelta{ 0 };
    // A.13: roughness-dependent reflection reflectance core — evaluated each
    // frame for the water/clear-coat/material surfaces and publishes the
    // screen/probe blend weights (per-roughness) to the reflection contract.
    std::unique_ptr<Engine::Rendering::IReflectionModel> reflectionModel;
    std::uint32_t reflectionScreenWeightPct{ 0 };
    std::uint32_t reflectionProbeWeightPct{ 0 };
    float reflectionWaterFresnel{ 0.0f };
    // Aceleração 1 (visual showcase): the PUBLIC GI provider stack
    // (IGlobalIlluminationProvider + IGiCore + IDiffuseGlobalIllumination),
    // the reflection provider (create_reflection_provider) and the scene-lysis
    // core (create_scene_layers) are REAL product consumers of the framework:
    // they receive the scene's real geometry (voxel terrain sampler) and the
    // real lights (day/night sun + point lights) each frame and publish
    // irradiance / reflection policy / composed layers into the final
    // composition instead of staying SDK-only. Config is data-driven from the
    // showcase JSON assets.
    std::unique_ptr<Engine::Rendering::IGlobalIlluminationProvider> globalIllumination;
    std::unique_ptr<Engine::Rendering::IDiffuseGlobalIllumination> diffuseGi;
    std::unique_ptr<Engine::Rendering::IReflectionProvider> reflectionProvider;
    std::shared_ptr<engine::assets::ISceneLayers> sceneLayers;
    // Per-frame observable state of the real GI stack (fed to the title +
    // debug snapshot each frame).
    std::uint32_t giTotalProbes{ 0 };
    std::uint32_t giPendingProbes{ 0 };
    std::uint32_t giBakedPerFrame{ 0 };
    float giMeanOutgoing{ 0.0f };
    float giSkylightA{ 0.0f };
    std::uint32_t diffuseGiCardCount{ 0 };
    float diffuseGiBounceEnergy{ 0.0f };
    std::string diffuseGiSummary;
    std::string reflectionBackendName;
    std::uint32_t reflectionScreenSurfaces{ 0 };
    std::uint32_t reflectionProbeSurfaces{ 0 };
    std::uint32_t reflectionRayTracedSurfaces{ 0 };
    std::uint32_t reflectionScreenRaysSpent{ 0 };
    std::uint32_t sceneLayerEntityCount{ 0 };
    std::string sceneLayersSummary;
    // Data-driven showcase configs loaded from real assets and consumed by the
    // frame (ocean FFT + particle draw).
    std::string showcaseOceanJson;
    std::string showcaseParticlesJson;
    std::uint32_t showcaseOceanWindSpeedMs{ 0 };
    std::uint32_t showcaseOceanSize{ 0 };
    std::uint32_t showcaseParticleSpawn{ 42 };
    // Aceleração 1: the CPU offline tracer (create_ray_tracer) and the SDF
    // software tracer (create_software_tracer) were TEST-ONLY; here they are
    // REAL product consumers — rebuilt and traced per frame over the actual
    // voxel geometry (block soup / block-distance SDF) and observable.
    std::unique_ptr<vc::rendering::IRayTracer> offlineRayTracer;
    std::uint32_t offlineTraceRays{ 0 };
    std::uint32_t offlineTraceHits{ 0 };
    std::unique_ptr<Engine::Rendering::ISoftwareTracer> softwareTracer;
    std::uint32_t softwareTraceRays{ 0 };
    std::uint32_t softwareTraceHits{ 0 };
    float softwareTraceMeanDistance{ 0.0f };
    // L29 (reabertura): meshlets REAIS da geometria voxel (soup de blocos do
    // offline tracer) — agrupamento greedy com limites 64 vértices / 126
    // triângulos e bounding sphere real por grupo (centro médio + raio máximo;
    // o soup só tem posições, então cone axis por normal é inviável). O
    // stream de meshlets deixa de ser "só declarado"; o GPU path (mesh
    // shaders) fica para validação de build do Agente 5, com o fallback
    // indexado funcional mantido.
    std::uint32_t meshletCount{ 0 };
    std::uint32_t meshletMaxVerts{ 0 };
    std::uint32_t meshletMaxTris{ 0 };
    float meshletMaxSphereRadius{ 0.0f };  // maior raio de bounding sphere entre grupos
    // ── Conta 2 (item 1): submissão GPU REAL dos meshlets (VK_EXT_mesh_shader) ──
    // Quando a device expõe task/mesh shader, o stream de meshlets do soup de
    // blocos do mundo é compilado (task/mesh/fragment via glslc em runtime) e
    // enviado como buffers + pipeline mesh + vkCmdDrawMeshTasksEXT no scene
    // pass com culling por bounding sphere na task stage. Quando a capability
    // não existe, o caminho indexado/vertex atual permanece o de submissão
    // real (fallback visualmente equivalente). Tudo é capability-gated.
    struct MeshletStreamGpu {
        VkPipeline pipeline{VK_NULL_HANDLE};
        VkPipelineLayout layout{VK_NULL_HANDLE};
        VkDescriptorSetLayout setLayout{VK_NULL_HANDLE};
        VkDescriptorPool setPool{VK_NULL_HANDLE};
        VkDescriptorSet set{VK_NULL_HANDLE};
        AllocatedBuffer boundsBuffer;   // vec4(center.xyz, radius) por grupo
        AllocatedBuffer metaBuffer;     // vec4(firstTri, triCount, firstVtx, vtxCount)
        AllocatedBuffer posBuffer;      // posições mundo reais (vec4 por vértice)
        AllocatedBuffer trisBuffer;     // uvec4 por triângulo (3 índices)
        std::uint32_t groupCount{ 0 };
        std::uint32_t vertexCount{ 0 };
        std::uint32_t triangleCount{ 0 };
        bool valid{ false };
    } meshletGpu;
    // CPU-side capture of the REAL meshlet grouping built in the meshlet block
    // from the block soup. upload_meshlet_gpu() stages these into the four GPU
    // storage buffers (bounds / meta / positions / triangle connectivity) the
    // task+mesh shaders consume — the genuine vertex/triangle stream submitted
    // through vkCmdDrawMeshTasksEXT in the scene pass.
    std::vector<glm::vec4> meshletBoundsCpu_;
    std::vector<glm::vec4> meshletMetaCpu_;
    std::vector<glm::vec4> meshletPosCpu_;
    std::vector<glm::uvec4> meshletTrisCpu_;
    bool meshShaderCapable_{ false };
    PFN_vkCmdDrawMeshTasksEXT fpDrawMeshTasksExt_{ nullptr };
    void init_mesh_shader_path();
    void upload_meshlet_gpu();
    void destroy_mesh_shader_path();
    // Otimização: os tracejadores CPU (offline soup + SDF) são diagnóstico
    // observável, não entrada de render; rodam a cada N frames (cadência
    // 30, igual ao lumenRayTracer) em vez de todo frame. O buffer do soup
    // de blocos é reutilizado entre frames (clear + reuse) para evitar
    // realocação heap por frame. counterOfflineTrace é o contador de cadência.
    std::uint32_t offlineTraceCadence{ 0u };
    std::vector<vc::rendering::RayTracerTriangle> offlineSoupTris;
    // A.16: the quality-preset dial resolves the ACTIVE quality level into the
    // concrete budgets of every rendering contract each frame; no phantom.
    std::unique_ptr<Engine::Rendering::IRenderingPresets> renderingPresets;
    std::uint32_t presetGiClipmapCells{ 0 };
    std::uint32_t presetTraceRayBudget{ 0 };
    // C.2 (cesium-native): the WGS84 ellipsoid math drives the origin-rebasing
    // seam — the geodetic tile bounds the renderer uses to place the camera
    // frame relative to a large-coordinate world origin each frame.
    std::unique_ptr<vc::rendering::IEllipsoidMath> ellipsoidMath;
    double ellipsoidTileBoundsRad{ 0.0 };
    // C.8 (openvdb): sparse volume grid for fog-of-war / SDF density — a real
    // volumetric field driven near the player each frame; no phantom.
    std::unique_ptr<vc::rendering::ISparseVolumeGrid> sparseVolumeGrid;
    int sparseActiveBricks{ 0 };
    float sparseSdfAtPlayer{ 0.0f };
    // C.4 (embree): deterministic offline AO baking over a real triangle mesh
    // built from the spawn region; exposes baked occlusion per sample.
    std::unique_ptr<vc::rendering::IRayBakeMesh> rayBakeMesh;
    std::uint32_t bakeSampleCount{ 0 };
    float bakeMeanOcclusion{ 0.0f };
    // C.1/C.2: the deterministic per-face block material resolver runs EMBEDDED
    // at meshing dispatch (facade derives resolver-compatible variantKey/face
    // material into the RuntimeBlockInfo table the mesher consumes). No orphaned
    // IBlockMaterialResolver instance here — the app's raw World has no registry
    // to feed BlockDefinition; the real variant keys surface via
    // refresh_gpu_features() reading world.runtime_block_table().
    // C.5/C.15/vendor adoption: the deterministic SDK cores are instantiated by
    // the product frame (same pattern as atmosphere above) and publish into the
    // GPU feature contract consumed by post.frag.
    std::unique_ptr<Engine::Rendering::IParticleSystem> particleSystem;
    // F.22: REAL vertex + VkDrawIndirectCommand data for the particle pass,
    // built each frame from the alive particle effect (not a count-only UBO
    // read). The derived indirect/vertex tallies are observable each frame.
    std::unique_ptr<Engine::Rendering::IParticleDrawData> particleDrawData;
    std::uint32_t particleIndirectVertexCount{ 0 };
    std::uint32_t particleIndirectInstanceCount{ 0 };
    std::uint32_t particleIndirectBytes{ 0 };
    std::unique_ptr<vc::rendering::ICasSharpening> casSharpening;
    float casSharpness{ 0.4f };
    std::unique_ptr<Engine::Rendering::IKtx2Transcoder> ktx2Transcoder;
    std::unique_ptr<vc::rendering::IXrMath> xrMath;
    std::unique_ptr<vc::rendering::IHairPhysics> hairPhysics;
    vc::rendering::HairStrand hairStrand;
    std::unique_ptr<vc::rendering::IShaderCompiler> shaderCompiler;
    // vulkan-samples (B) + AGENT-4 2026-08-29: render-pass profiling pattern as
    // a real product producer — per-pass CPU+GPU timings recorded each frame.
    // The GPU timings come from a real per-frame VkQueryPool of timestamps
    // (vkCmdWriteTimestamp2 between pass boundaries), NOT the fabricated CPU
    // fractions that existed before. Guarded by renderPassMetrics != nullptr.
    std::unique_ptr<Engine::Rendering::IRenderPassMetrics> renderPassMetrics;
    std::chrono::steady_clock::time_point passFrameStart{};
    // B.4: the deterministic scene-culling core consumed by the real draw path
    // (WorldRenderer::set_scene_culling). Its per-draw observable counts
    // (visible/culled/occluded-detail/LOD split) are published in the title.
    std::unique_ptr<Engine::Rendering::ISceneCulling> sceneCulling;
    // AGENT-1 I.1: the seven public rendering factories that were ONLY
    // linked/tested in isolation are wired into the real game executable with
    // real data + observable state — see init() "RenderProviders" block.
    std::unique_ptr<Engine::Rendering::IVulkanLoader> vulkanLoader;
    std::unique_ptr<vc::rendering::ISwapchainManager> swapchainManager;
    std::unique_ptr<Engine::Rendering::IRenderGraph> publicRenderGraph;
    std::size_t renderGraphPassCount{ 0 };
    std::size_t renderGraphBarrierCount{ 0 };
    std::string presetName{ "n/a" };
    float rayBakeOpenMean{ 0.0f };
    float ellipsoidLat{ 0.0f };
    float ellipsoidLon{ 0.0f };
    // B.7: the SDK origin-rebase service keeps the render camera in the local
    // frame — the double-precision origin follows the camera focus and all
    // rendered coordinates stay small (no jitter at large world offsets).
    std::unique_ptr<engine::world::IOriginRebase> renderOriginRebase;
    std::uint64_t rebaseFiredCount{ 0 };
    // B.1/B.2: the immutable per-frame render snapshot. Built ONCE per frame
    // in draw() from the world/clock/player state, then consumed read-only by
    // the presentation path (title observables) — the renderer never reads
    // scattered mutable state for these values. Identity is the unified
    // camera/sun/tick/entity set, not per-system arrays.
    struct RenderFrameSnapshot {
        glm::vec3 cameraPosition{ 0.0f };
        glm::vec3 cameraFront{ 0.0f, 0.0f, -1.0f };
        glm::vec3 sunDirection{ 0.0f, 1.0f, 0.0f };
        glm::vec3 lightColor{ 1.0f };
        float daylight{ 1.0f };
        float exposure{ 1.0f };
        float timeOfDay{ 0.0f };
        std::uint64_t tick{ 0 };
        std::size_t entities{ 0 };
        std::size_t visibleChunks{ 0 };
        std::size_t mobs{ 0 };
        glm::dvec3 origin{ 0.0, 0.0, 0.0 };  // local-frame origin (B.7)
    };
    RenderFrameSnapshot frameSnapshot;
    // B.4: per-frame instance grouping of the LIVE mob ECS through
    // buildInstanceGroups — real positions/types from the entity layer become
    // InstanceGroups (mesh/material keys + merged AABB + count), and the
    // merged AABB is culled with the SDK frustum before the mob draw. The
    // group count and visible groups are published in the title.
    std::size_t mobInstanceGroups{ 0 };
    std::size_t mobVisibleGroups{ 0 };
    std::size_t mobInstanceCount{ 0 };
    // H.1/H.2/H.6: the real per-system provider registry — the game executable
    // records the provider actually selected for every rendering system (with
    // its call site, artifact and capability reason) and logs the full matrix
    // at boot. This is the runtime mirror of docs/SOLUCOES_E_DEPENDENCIAS.md.
    std::unique_ptr<Engine::Rendering::IRenderProviderRegistry> providerRegistry;
    // A.4/A.5/A.6: the REAL frame RenderGraph — one graph owns the pass order
    // and barriers of the executable's frame (minimap, shadow, scene,
    // sceneCopy, water, post, present) over the REAL images. Rebuilt when the
    // swapchain/screen targets change; the compiled barrier list drives
    // vkCmdPipelineBarrier2 in draw() (no hand-written duplicated sync).
    Engine::Rendering::RenderGraph frameGraph;
    Engine::Rendering::RenderGraphCompileResult frameGraphCompile;
    bool frameGraphValid{ false };
    std::unordered_map<Engine::Rendering::RenderResourceId, VkImage> frameGraphImages;
    std::unordered_map<Engine::Rendering::RenderResourceId, VkImageAspectFlags> frameGraphAspects;
    std::uint32_t currentSwapchainImageIndex{ 0 };
    std::uint32_t frameGraphPassCount{ 0 };
    std::uint32_t frameGraphBarrierCount{ 0 };
    std::string frameGraphErrors;
    void rebuild_frame_graph();
    void record_graph_barriers(VkCommandBuffer cmd, const char* destPass);
    // Time in ns between consecutive timestamp integer values (device limit).
    double timestampPeriodNs{ 1.0 };
    float particleSimAccumulator{ 0.0f };
    int32_t particleHandle{ -1 };
    bool ktx2AssetValidated{ false };
    std::vector<std::uint8_t> ktx2AssetBytes;
    bool gpuFeaturesReady{ false };
    void refresh_gpu_features();

    float deltaTime{ 0.0f };
    float lastFrameTime{ 0.0f };

    VulkanEngineApp() = default;
    ~VulkanEngineApp() = default;

    void init();
    void run();
    void cleanup();

private:
    void init_window();
    void init_vulkan();
    void init_swapchain();
    void create_swapchain(VkSwapchainKHR oldSwapchain);
    bool recreate_swapchain();
    void destroy_screen_targets();
    void initialize_screen_target_layouts();
    void update_screen_descriptors();
    void toggle_fullscreen();
    void init_depth_buffer();
    void init_shadow_map();
    void init_minimap();
    void init_hdr_target();
    void init_commands();
    void init_sync_structures();
    void init_pipeline();
    void init_gpu_feature_binding();
    void destroy_gpu_feature_binding();
    // C.2: builds the app's BlockRegistry catalog and pushes the derived
    // runtime table into the raw simulation World (facade-compatible builder).
    void init_block_registry();
    void init_gpu_feature_passes();
    void destroy_gpu_feature_passes();
    void init_timestamp_queries();
    void destroy_timestamp_queries();
    // Reads the PREVIOUS frame's completed timestamp queries and feeds the
    // per-pass GPU timings into renderPassMetrics. Returns false if the pool is
    // unavailable or results are not ready.
    bool publish_timestamp_metrics();
    void init_arm_mesh();
    void draw_arm(VkCommandBuffer cmd, const glm::mat4& view, const glm::mat4& proj);
    void draw_character(VkCommandBuffer cmd, const glm::mat4& view, const glm::mat4& proj);
    // CONTA 4 item 5 — rebuild the pose mesh the renderer draws from the LIVE
    // ragdoll/skinned/vehicle pose each frame and draw it (not CPU-only).
    void rebuild_showcase_pose_mesh();
    void draw_showcase_pose(VkCommandBuffer cmd, const glm::mat4& view,
                            const glm::mat4& proj);

    void draw();

    VkShaderModule load_shader_module(const std::string& filePath);

    static void mouse_callback(GLFWwindow* window, double xpos, double ypos);
    static void mouse_button_callback(GLFWwindow* window, int button, int action, int mods);
    static void framebuffer_size_callback(GLFWwindow* window, int width, int height);
};
