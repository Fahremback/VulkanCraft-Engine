# VulkanCraft Engine — Mapa canônico de namespaces (SDK)

Gerado automaticamente por `node tools/sdk/namespace-gate.mjs --write docs/NAMESPACE_CANONICAL.md` — NÃO editar à mão.
Fonte única: walk de `src/engine/public/` (mesma fonte do `SDK_API_INVENTORY.md`).

> **Invariante dura (bugs.md B-219-2, agora ENFORCED pelo gate)**: nenhum header público pode existir sem namespace.
> O objetivo de longo prazo (§1 item 5 / §2 codegen) é unificar os padrões ad-hoc (`engine{}`, `procgen{}`,
> `Engine::Rendering{}`, `animation{}`, `voxel{}`, ...) em `engine::<domain>` — breaking change global que deve
> ser dirigido pelo codegen do §2, não por edição manual de headers. Este mapa documenta o estado atual
> deterministicamente como insumo desse codegen.

## Headers por namespace top-level

| Header | Namespace(s) top-level |
|---|---|
| `src/engine/public/engine/ai/IAiDebugInfo.hpp` | `engine` · `ai` |
| `src/engine/public/engine/ai/IAiEventBus.hpp` | `engine::ai` |
| `src/engine/public/engine/ai/IAiLod.hpp` | `engine::ai` |
| `src/engine/public/engine/ai/IBehaviorTree.hpp` | `engine` · `ai` |
| `src/engine/public/engine/ai/ICrowdSimulation.hpp` | `engine::ai` |
| `src/engine/public/engine/ai/IFsm.hpp` | `engine::ai` |
| `src/engine/public/engine/ai/IPerception.hpp` | `engine::ai` |
| `src/engine/public/engine/ai/IPlanner.hpp` | `engine::ai` |
| `src/engine/public/engine/ai/ISteering.hpp` | `engine::ai` |
| `src/engine/public/engine/ai/IUtilityAi.hpp` | `engine::ai` |
| `src/engine/public/engine/animation/IAiGraphValidation.hpp` | `engine` · `animation` |
| `src/engine/public/engine/animation/IAnimAdditive.hpp` | `engine::animation` |
| `src/engine/public/engine/animation/IAnimBudget.hpp` | `engine::animation` |
| `src/engine/public/engine/animation/IAnimCore.hpp` | `engine::animation` |
| `src/engine/public/engine/animation/IAnimEvents.hpp` | `engine::animation` |
| `src/engine/public/engine/animation/IAnimMask.hpp` | `engine::animation` |
| `src/engine/public/engine/animation/IAnimStateMachine.hpp` | `engine::animation` |
| `src/engine/public/engine/animation/IAnimationLod.hpp` | `engine` · `animation` |
| `src/engine/public/engine/animation/IConstraints.hpp` | `engine::animation` |
| `src/engine/public/engine/animation/IFootPlacement.hpp` | `engine` · `animation` |
| `src/engine/public/engine/animation/IGaitPlanner.hpp` | `engine` · `animation` |
| `src/engine/public/engine/animation/IIkSolver.hpp` | `engine::animation` |
| `src/engine/public/engine/animation/IInertializer.hpp` | `engine::animation` |
| `src/engine/public/engine/animation/IMotionDatabase.hpp` | `engine` · `animation` |
| `src/engine/public/engine/animation/IMotionMatcher.hpp` | `engine` · `animation` |
| `src/engine/public/engine/animation/IPoseWarper.hpp` | `engine` · `animation` |
| `src/engine/public/engine/animation/IProceduralAnimationPipeline.hpp` | `engine::animation` |
| `src/engine/public/engine/animation/IProceduralLegs.hpp` | `engine` · `animation` |
| `src/engine/public/engine/animation/IRetargeting.hpp` | `engine::animation` |
| `src/engine/public/engine/animation/IRootMotion.hpp` | `engine::animation` |
| `src/engine/public/engine/animation/ISkinning.hpp` | `engine::animation` |
| `src/engine/public/engine/animation/ITerrainAdaptation.hpp` | `engine::animation` |
| `src/engine/public/engine/assets/IAssetPipeline.hpp` | `engine::assets` |
| `src/engine/public/engine/assets/ISceneLayers.hpp` | `engine` · `assets` |
| `src/engine/public/engine/audio/IAdaptiveMusic.hpp` | `engine::audio` |
| `src/engine/public/engine/audio/IAudioEventMapper.hpp` | `engine` · `audio` |
| `src/engine/public/engine/audio/IAudioMixer.hpp` | `engine::audio` |
| `src/engine/public/engine/audio/ISpatialAudio.hpp` | `engine::audio` |
| `src/engine/public/engine/compiler/IEpisodeCompiler.hpp` | `engine` · `compiler` |
| `src/engine/public/engine/compression/ICompressionProvider.hpp` | `engine` · `compression` |
| `src/engine/public/engine/deformable/IDeformableProvider.hpp` | `Engine::Deformable` |
| `src/engine/public/engine/deformable/ITetraMeshCooking.hpp` | `Engine::Deformable` |
| `src/engine/public/engine/director/IWorldDirector.hpp` | `engine` · `director` |
| `src/engine/public/engine/editor/IAnimationTimelineEditor.hpp` | `engine` · `editor` |
| `src/engine/public/engine/editor/ICommandSearch.hpp` | `engine` · `editor` |
| `src/engine/public/engine/editor/IContentBrowser.hpp` | `engine` · `editor` |
| `src/engine/public/engine/editor/IEditorCamera.hpp` | `engine::editor` |
| `src/engine/public/engine/editor/IFileChangeDebounce.hpp` | `engine` · `editor` |
| `src/engine/public/engine/editor/IFileWatcher.hpp` | `engine` · `editor` |
| `src/engine/public/engine/editor/IGizmoController.hpp` | `engine::editor` |
| `src/engine/public/engine/editor/IInspectorDoc.hpp` | `engine::editor` |
| `src/engine/public/engine/editor/IMessageCatalog.hpp` | `engine` · `editor` |
| `src/engine/public/engine/editor/IOnboardingTour.hpp` | `engine` · `editor` |
| `src/engine/public/engine/editor/IPlayMode.hpp` | `engine` · `editor` |
| `src/engine/public/engine/editor/IProjectLauncher.hpp` | `engine` · `editor` |
| `src/engine/public/engine/editor/IPublishPipeline.hpp` | `engine::editor` |
| `src/engine/public/engine/editor/IRetargeting.hpp` | `engine` · `editor` |
| `src/engine/public/engine/editor/ISceneHierarchy.hpp` | `engine::editor` |
| `src/engine/public/engine/editor/IShortcutDoc.hpp` | `engine` · `editor` |
| `src/engine/public/engine/editor/IUndoHistory.hpp` | `engine` · `editor` |
| `src/engine/public/engine/editor/IWindowMode.hpp` | `engine` · `editor` |
| `src/engine/public/engine/entity/IEntityArchetype.hpp` | `engine` · `entity` |
| `src/engine/public/engine/entity/IEntityLifecycle.hpp` | `engine` · `entity` |
| `src/engine/public/engine/entity/IEntityWorld.hpp` | `engine` · `entity` |
| `src/engine/public/engine/entity/IMobBehavior.hpp` | `engine` · `entity` |
| `src/engine/public/engine/entity/IReflection.hpp` | `engine` · `entity` |
| `src/engine/public/engine/entity/ISpatialIndex.hpp` | `engine` · `entity` |
| `src/engine/public/engine/farm/IOfflineFarm.hpp` | `Engine::Farm` |
| `src/engine/public/engine/gameplay/IAbilityEffects.hpp` | `engine` · `gameplay` |
| `src/engine/public/engine/gameplay/IAbilitySystem.hpp` | `engine` · `gameplay` |
| `src/engine/public/engine/gameplay/IBalance.hpp` | `engine::gameplay` |
| `src/engine/public/engine/gameplay/ICharacterController.hpp` | `engine::gameplay` |
| `src/engine/public/engine/gameplay/IDayNightCycle.hpp` | `engine` · `gameplay` |
| `src/engine/public/engine/gameplay/IEffectStacks.hpp` | `engine::gameplay` |
| `src/engine/public/engine/gameplay/IFaction.hpp` | `engine::gameplay` |
| `src/engine/public/engine/gameplay/IGameplayEventRouter.hpp` | `engine` · `audio` · `gameplay` |
| `src/engine/public/engine/gameplay/IGameplayEvents.hpp` | `engine::gameplay` |
| `src/engine/public/engine/gameplay/IGameplayMetrics.hpp` | `engine` · `gameplay` |
| `src/engine/public/engine/gameplay/IGameplayRuntime.hpp` | `engine` · `gameplay` |
| `src/engine/public/engine/gameplay/IHitReaction.hpp` | `engine::gameplay` |
| `src/engine/public/engine/gameplay/IInteraction.hpp` | `engine` · `gameplay` |
| `src/engine/public/engine/gameplay/IMissionAsset.hpp` | `engine` · `gameplay` |
| `src/engine/public/engine/gameplay/IRagdollAsset.hpp` | `engine` · `gameplay` |
| `src/engine/public/engine/gameplay/IReplay.hpp` | `engine::gameplay` |
| `src/engine/public/engine/hair/IHairProvider.hpp` | `Engine::Hair` |
| `src/engine/public/engine/hashing/IHashProvider.hpp` | `engine` · `hashing` |
| `src/engine/public/engine/input/IActionMap.hpp` | `engine` · `input` |
| `src/engine/public/engine/navigation/IAgentCapabilities.hpp` | `engine::navigation` |
| `src/engine/public/engine/navigation/IAsyncQueryScheduler.hpp` | `engine` · `navigation` |
| `src/engine/public/engine/navigation/IHierarchicalPath.hpp` | `engine::navigation` |
| `src/engine/public/engine/navigation/INavInvalidation.hpp` | `engine` · `navigation` |
| `src/engine/public/engine/navigation/INavStreaming.hpp` | `engine` · `navigation` |
| `src/engine/public/engine/navigation/INavigationProvider.hpp` | `engine` · `navigation` |
| `src/engine/public/engine/navigation/INavigationSchedulerBridge.hpp` | `engine` · `navigation` |
| `src/engine/public/engine/navigation/VoxelNavigation.hpp` | `engine` · `voxel` · `navigation` |
| `src/engine/public/engine/networking/INetworkDiscovery.hpp` | `engine::networking` |
| `src/engine/public/engine/networking/INetworkInterest.hpp` | `engine::networking` |
| `src/engine/public/engine/networking/INetworkReplication.hpp` | `engine::networking` |
| `src/engine/public/engine/networking/INetworkRpc.hpp` | `engine::networking` |
| `src/engine/public/engine/observability/IObservability.hpp` | `engine::observability` |
| `src/engine/public/engine/observability/OtlpExporter.hpp` | `engine::observability` |
| `src/engine/public/engine/packaging/IPackageManager.hpp` | `engine::packaging` |
| `src/engine/public/engine/physics/ICSGOperation.hpp` | `engine` · `physics` |
| `src/engine/public/engine/physics/IConvexDecomposition.hpp` | `engine` · `physics` |
| `src/engine/public/engine/physics/IExplosion.hpp` | `engine::physics` |
| `src/engine/public/engine/plugins/IPluginSandbox.hpp` | `Engine::Plugins` · `Permissions` |
| `src/engine/public/engine/plugins/IPluginTypeRegistry.hpp` | `Engine::Plugins` |
| `src/engine/public/engine/procgen/IClimateBiome.hpp` | `engine` · `procgen` |
| `src/engine/public/engine/procgen/IHeightmapErosion.hpp` | `engine` · `procgen` |
| `src/engine/public/engine/procgen/IJobRunner.hpp` | `engine` · `procgen` |
| `src/engine/public/engine/procgen/ILodTerrain.hpp` | `engine` · `procgen` |
| `src/engine/public/engine/procgen/IMeshCooking.hpp` | `engine` · `procgen` |
| `src/engine/public/engine/procgen/IMeshGeometryProcessing.hpp` | `Engine::Procgen` |
| `src/engine/public/engine/procgen/IMultiScaleStreaming.hpp` | `engine` · `procgen` |
| `src/engine/public/engine/procgen/INoiseGraph.hpp` | `engine` · `procgen` |
| `src/engine/public/engine/procgen/IParcellation.hpp` | `engine` · `procgen` |
| `src/engine/public/engine/procgen/IProcgenPreview.hpp` | `engine` · `procgen` |
| `src/engine/public/engine/procgen/IShapeGrammar.hpp` | `engine` · `procgen` |
| `src/engine/public/engine/procgen/IStructureGenerator.hpp` | `engine` · `procgen` |
| `src/engine/public/engine/procgen/IStructurePlacement.hpp` | `engine` · `procgen` |
| `src/engine/public/engine/procgen/IWorldFeatures.hpp` | `engine` · `procgen` |
| `src/engine/public/engine/procgen/IWorldProfile.hpp` | `engine` · `procgen` |
| `src/engine/public/engine/profiling/IFrameProfiler.hpp` | `engine` · `profiling` |
| `src/engine/public/engine/registry/BlockRegistry.hpp` | `engine` · `registry` |
| `src/engine/public/engine/registry/FluidRegistry.hpp` | `engine` · `registry` |
| `src/engine/public/engine/registry/IEquipment.hpp` | `engine::registry` |
| `src/engine/public/engine/registry/ILootTable.hpp` | `engine::registry` |
| `src/engine/public/engine/registry/Inventory.hpp` | `engine` · `registry` |
| `src/engine/public/engine/registry/ItemRegistry.hpp` | `engine` · `registry` |
| `src/engine/public/engine/registry/ItemStack.hpp` | `engine` · `registry` |
| `src/engine/public/engine/registry/RecipeRegistry.hpp` | `engine` · `registry` |
| `src/engine/public/engine/rendering/IAbilityEffects.hpp` | `vc::rendering` |
| `src/engine/public/engine/rendering/IAtmosphereScattering.hpp` | `Engine::Rendering` |
| `src/engine/public/engine/rendering/IBlockMaterialResolver.hpp` | `Engine::Rendering` |
| `src/engine/public/engine/rendering/ICasSharpening.hpp` | `vc::rendering` |
| `src/engine/public/engine/rendering/IDiffuseGlobalIllumination.hpp` | `Engine::Rendering` |
| `src/engine/public/engine/rendering/IEllipsoidMath.hpp` | `vc::rendering` |
| `src/engine/public/engine/rendering/IFftCore.hpp` | `Engine::Rendering` |
| `src/engine/public/engine/rendering/IFluidSimulation.hpp` | `vc::rendering` |
| `src/engine/public/engine/rendering/IGlobalIlluminationProvider.hpp` | `Engine::Rendering` |
| `src/engine/public/engine/rendering/IHairPhysics.hpp` | `vc::rendering` |
| `src/engine/public/engine/rendering/IKtx2Transcoder.hpp` | `Engine::Rendering` |
| `src/engine/public/engine/rendering/ILumenScene.hpp` | `Engine::Rendering` |
| `src/engine/public/engine/rendering/IMaterialShading.hpp` | `Engine::Rendering` |
| `src/engine/public/engine/rendering/IParticleSystem.hpp` | `Engine::Rendering` |
| `src/engine/public/engine/rendering/IProbeGrid.hpp` | `Engine::Rendering` |
| `src/engine/public/engine/rendering/IRayTracer.hpp` | `vc::rendering` |
| `src/engine/public/engine/rendering/IReSTIRDI.hpp` | `Engine::Rendering` |
| `src/engine/public/engine/rendering/IReflectionModel.hpp` | `Engine::Rendering` |
| `src/engine/public/engine/rendering/IReflectionProvider.hpp` | `Engine::Rendering` |
| `src/engine/public/engine/rendering/IRenderGraph.hpp` | `Engine::Rendering` |
| `src/engine/public/engine/rendering/IRenderPassMetrics.hpp` | `Engine::Rendering` |
| `src/engine/public/engine/rendering/IRenderingDebugView.hpp` | `Engine::Rendering` |
| `src/engine/public/engine/rendering/IRenderingPresets.hpp` | `Engine::Rendering` |
| `src/engine/public/engine/rendering/ISceneCulling.hpp` | `Engine::Rendering` |
| `src/engine/public/engine/rendering/IScreenSpaceTracer.hpp` | `Engine::Rendering` |
| `src/engine/public/engine/rendering/IShaderCompiler.hpp` | `vc::rendering` |
| `src/engine/public/engine/rendering/ISoftwareTracer.hpp` | `Engine::Rendering` |
| `src/engine/public/engine/rendering/ISparseVolumeGrid.hpp` | `vc::rendering` |
| `src/engine/public/engine/rendering/ISpatialUpscaler.hpp` | `Engine::Rendering` |
| `src/engine/public/engine/rendering/ISpirvReflection.hpp` | `Engine::Rendering` |
| `src/engine/public/engine/rendering/ISurfaceCacheCapture.hpp` | `Engine::Rendering` |
| `src/engine/public/engine/rendering/ISwapchainManager.hpp` | `vc::rendering` |
| `src/engine/public/engine/rendering/ITemporalDenoiser.hpp` | `Engine::Rendering` |
| `src/engine/public/engine/rendering/IToneMapping.hpp` | `Engine::Rendering` |
| `src/engine/public/engine/rendering/IVolumeClouds.hpp` | `Engine::Rendering` |
| `src/engine/public/engine/rendering/IXrMath.hpp` | `vc::rendering` |
| `src/engine/public/engine/scripting/ILuauSandbox.hpp` | `engine::scripting` |
| `src/engine/public/engine/scripting/IVisualScriptGraph.hpp` | `engine::scripting` |
| `src/engine/public/engine/semantic/ISemanticApi.hpp` | `engine` · `semantic` |
| `src/engine/public/engine/simfarm/ISimulationFarm.hpp` | `engine` · `simfarm` |
| `src/engine/public/engine/simulation/IFixedTickSim.hpp` | `engine::simulation` |
| `src/engine/public/engine/simulation/IMacroMicroReconciler.hpp` | `engine` · `simulation` |
| `src/engine/public/engine/simulation/ISimulationLod.hpp` | `engine` · `simulation` |
| `src/engine/public/engine/storage/IChunkStoreFactory.hpp` | `engine` · `voxel` · `storage` |
| `src/engine/public/engine/timeline/ICausalResolver.hpp` | `engine::timeline` |
| `src/engine/public/engine/timeline/ITimelineGraph.hpp` | `engine::timeline` |
| `src/engine/public/engine/ui/IConfirmation.hpp` | `engine` · `ui` |
| `src/engine/public/engine/ui/ICrafting.hpp` | `engine` · `ui` |
| `src/engine/public/engine/ui/IInventoryGrid.hpp` | `engine` · `ui` |
| `src/engine/public/engine/ui/IJsonSchema.hpp` | `engine` · `ui` |
| `src/engine/public/engine/ui/ILayout.hpp` | `engine` · `ui` |
| `src/engine/public/engine/ui/ITextShaper.hpp` | `engine` · `ui` |
| `src/engine/public/engine/ui/IUiDoc.hpp` | `engine` · `ui` |
| `src/engine/public/engine/ui/IViewport.hpp` | `engine` · `ui` |
| `src/engine/public/engine/ui/IWidgets.hpp` | `engine` · `ui` |
| `src/engine/public/engine/ui/qt/IQtEditorDoc.hpp` | `engine` · `ui` |
| `src/engine/public/engine/ui/qt/IQtThemeModel.hpp` | `engine` · `ui` |
| `src/engine/public/engine/vehicles/IBeamGraphAsset.hpp` | `engine` · `vehicles` |
| `src/engine/public/engine/vehicles/IVehicleAsset.hpp` | `engine` · `vehicles` |
| `src/engine/public/engine/vehicles/IVehicleDamage.hpp` | `engine` · `vehicles` |
| `src/engine/public/engine/vehicles/IVehicleProvider.hpp` | `engine` · `vehicles` |
| `src/engine/public/engine/vehicles/IVehicleReplication.hpp` | `engine` · `vehicles` |
| `src/engine/public/engine/version.hpp` | `engine` |
| `src/engine/public/engine/voxel/IBlockEntityScripting.hpp` | `engine` · `voxel` |
| `src/engine/public/engine/voxel/IVoxelBlockEntity.hpp` | `engine` · `voxel` |
| `src/engine/public/engine/voxel/IVoxelReplication.hpp` | `engine` · `voxel` |
| `src/engine/public/engine/voxel/IVoxelServices.hpp` | `engine` · `voxel` |
| `src/engine/public/engine/voxel/IVoxelStreaming.hpp` | `engine` · `voxel` |
| `src/engine/public/engine/voxel/IVoxelWorld.hpp` | `engine` · `registry` · `voxel` |
| `src/engine/public/engine/world/ILocalSpace.hpp` | `engine` · `world` |
| `src/engine/public/engine/world/IOriginRebase.hpp` | `engine` · `world` |
| `src/engine/public/engine/world/IPortalSystem.hpp` | `engine` · `world` |
| `src/engine/public/engine/world/ITimeTravel.hpp` | `engine` · `world` |
| `src/engine/public/engine/world/ITimelinePolicy.hpp` | `engine` · `world` |
| `src/engine/public/engine/world/IWorldManager.hpp` | `engine` · `world` |
| `src/engine/public/engine/world/IWorldReplication.hpp` | `engine` · `world` |
