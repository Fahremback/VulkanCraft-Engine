# VulkanCraft Engine — Inventário da API pública (SDK)

Gerado automaticamente por `node tools/sdk/sdk-check.mjs --write-manifest docs/SDK_API_INVENTORY.md` — NÃO editar à mão.
Fonte única: o walk do filesystem em `src/engine/public/` (único include dir exposto aos consumidores).

Total: **207 headers públicos** em 36 domínios.

## Domínios

### ai (10)

- `engine/ai/IAiDebugInfo.hpp`
- `engine/ai/IAiEventBus.hpp`
- `engine/ai/IAiLod.hpp`
- `engine/ai/IBehaviorTree.hpp`
- `engine/ai/ICrowdSimulation.hpp`
- `engine/ai/IFsm.hpp`
- `engine/ai/IPerception.hpp`
- `engine/ai/IPlanner.hpp`
- `engine/ai/ISteering.hpp`
- `engine/ai/IUtilityAi.hpp`

### animation (22)

- `engine/animation/IAiGraphValidation.hpp`
- `engine/animation/IAnimAdditive.hpp`
- `engine/animation/IAnimBudget.hpp`
- `engine/animation/IAnimCore.hpp`
- `engine/animation/IAnimEvents.hpp`
- `engine/animation/IAnimMask.hpp`
- `engine/animation/IAnimStateMachine.hpp`
- `engine/animation/IAnimationLod.hpp`
- `engine/animation/IConstraints.hpp`
- `engine/animation/IFootPlacement.hpp`
- `engine/animation/IGaitPlanner.hpp`
- `engine/animation/IIkSolver.hpp`
- `engine/animation/IInertializer.hpp`
- `engine/animation/IMotionDatabase.hpp`
- `engine/animation/IMotionMatcher.hpp`
- `engine/animation/IPoseWarper.hpp`
- `engine/animation/IProceduralAnimationPipeline.hpp`
- `engine/animation/IProceduralLegs.hpp`
- `engine/animation/IRetargeting.hpp`
- `engine/animation/IRootMotion.hpp`
- `engine/animation/ISkinning.hpp`
- `engine/animation/ITerrainAdaptation.hpp`

### assets (2)

- `engine/assets/IAssetPipeline.hpp`
- `engine/assets/ISceneLayers.hpp`

### audio (4)

- `engine/audio/IAdaptiveMusic.hpp`
- `engine/audio/IAudioEventMapper.hpp`
- `engine/audio/IAudioMixer.hpp`
- `engine/audio/ISpatialAudio.hpp`

### compiler (1)

- `engine/compiler/IEpisodeCompiler.hpp`

### compression (1)

- `engine/compression/ICompressionProvider.hpp`

### deformable (2)

- `engine/deformable/IDeformableProvider.hpp`
- `engine/deformable/ITetraMeshCooking.hpp`

### director (1)

- `engine/director/IWorldDirector.hpp`

### editor (18)

- `engine/editor/IAnimationTimelineEditor.hpp`
- `engine/editor/ICommandSearch.hpp`
- `engine/editor/IContentBrowser.hpp`
- `engine/editor/IEditorCamera.hpp`
- `engine/editor/IFileChangeDebounce.hpp`
- `engine/editor/IFileWatcher.hpp`
- `engine/editor/IGizmoController.hpp`
- `engine/editor/IInspectorDoc.hpp`
- `engine/editor/IMessageCatalog.hpp`
- `engine/editor/IOnboardingTour.hpp`
- `engine/editor/IPlayMode.hpp`
- `engine/editor/IProjectLauncher.hpp`
- `engine/editor/IPublishPipeline.hpp`
- `engine/editor/IRetargeting.hpp`
- `engine/editor/ISceneHierarchy.hpp`
- `engine/editor/IShortcutDoc.hpp`
- `engine/editor/IUndoHistory.hpp`
- `engine/editor/IWindowMode.hpp`

### entity (6)

- `engine/entity/IEntityArchetype.hpp`
- `engine/entity/IEntityLifecycle.hpp`
- `engine/entity/IEntityWorld.hpp`
- `engine/entity/IMobBehavior.hpp`
- `engine/entity/IReflection.hpp`
- `engine/entity/ISpatialIndex.hpp`

### farm (1)

- `engine/farm/IOfflineFarm.hpp`

### gameplay (16)

- `engine/gameplay/IAbilityEffects.hpp`
- `engine/gameplay/IAbilitySystem.hpp`
- `engine/gameplay/IBalance.hpp`
- `engine/gameplay/ICharacterController.hpp`
- `engine/gameplay/IDayNightCycle.hpp`
- `engine/gameplay/IEffectStacks.hpp`
- `engine/gameplay/IFaction.hpp`
- `engine/gameplay/IGameplayEventRouter.hpp`
- `engine/gameplay/IGameplayEvents.hpp`
- `engine/gameplay/IGameplayMetrics.hpp`
- `engine/gameplay/IGameplayRuntime.hpp`
- `engine/gameplay/IHitReaction.hpp`
- `engine/gameplay/IInteraction.hpp`
- `engine/gameplay/IMissionAsset.hpp`
- `engine/gameplay/IRagdollAsset.hpp`
- `engine/gameplay/IReplay.hpp`

### hair (1)

- `engine/hair/IHairProvider.hpp`

### hashing (1)

- `engine/hashing/IHashProvider.hpp`

### input (1)

- `engine/input/IActionMap.hpp`

### navigation (8)

- `engine/navigation/IAgentCapabilities.hpp`
- `engine/navigation/IAsyncQueryScheduler.hpp`
- `engine/navigation/IHierarchicalPath.hpp`
- `engine/navigation/INavInvalidation.hpp`
- `engine/navigation/INavStreaming.hpp`
- `engine/navigation/INavigationProvider.hpp`
- `engine/navigation/INavigationSchedulerBridge.hpp`
- `engine/navigation/VoxelNavigation.hpp`

### networking (4)

- `engine/networking/INetworkDiscovery.hpp`
- `engine/networking/INetworkInterest.hpp`
- `engine/networking/INetworkReplication.hpp`
- `engine/networking/INetworkRpc.hpp`

### observability (2)

- `engine/observability/IObservability.hpp`
- `engine/observability/OtlpExporter.hpp`

### packaging (1)

- `engine/packaging/IPackageManager.hpp`

### physics (3)

- `engine/physics/ICSGOperation.hpp`
- `engine/physics/IConvexDecomposition.hpp`
- `engine/physics/IExplosion.hpp`

### plugins (2)

- `engine/plugins/IPluginSandbox.hpp`
- `engine/plugins/IPluginTypeRegistry.hpp`

### procgen (15)

- `engine/procgen/IClimateBiome.hpp`
- `engine/procgen/IHeightmapErosion.hpp`
- `engine/procgen/IJobRunner.hpp`
- `engine/procgen/ILodTerrain.hpp`
- `engine/procgen/IMeshCooking.hpp`
- `engine/procgen/IMeshGeometryProcessing.hpp`
- `engine/procgen/IMultiScaleStreaming.hpp`
- `engine/procgen/INoiseGraph.hpp`
- `engine/procgen/IParcellation.hpp`
- `engine/procgen/IProcgenPreview.hpp`
- `engine/procgen/IShapeGrammar.hpp`
- `engine/procgen/IStructureGenerator.hpp`
- `engine/procgen/IStructurePlacement.hpp`
- `engine/procgen/IWorldFeatures.hpp`
- `engine/procgen/IWorldProfile.hpp`

### profiling (1)

- `engine/profiling/IFrameProfiler.hpp`

### registry (8)

- `engine/registry/BlockRegistry.hpp`
- `engine/registry/FluidRegistry.hpp`
- `engine/registry/IEquipment.hpp`
- `engine/registry/ILootTable.hpp`
- `engine/registry/Inventory.hpp`
- `engine/registry/ItemRegistry.hpp`
- `engine/registry/ItemStack.hpp`
- `engine/registry/RecipeRegistry.hpp`

### rendering (36)

- `engine/rendering/IAbilityEffects.hpp`
- `engine/rendering/IAtmosphereScattering.hpp`
- `engine/rendering/IBlockMaterialResolver.hpp`
- `engine/rendering/ICasSharpening.hpp`
- `engine/rendering/IDiffuseGlobalIllumination.hpp`
- `engine/rendering/IEllipsoidMath.hpp`
- `engine/rendering/IFftCore.hpp`
- `engine/rendering/IFluidSimulation.hpp`
- `engine/rendering/IGlobalIlluminationProvider.hpp`
- `engine/rendering/IHairPhysics.hpp`
- `engine/rendering/IKtx2Transcoder.hpp`
- `engine/rendering/ILumenScene.hpp`
- `engine/rendering/IMaterialShading.hpp`
- `engine/rendering/IParticleSystem.hpp`
- `engine/rendering/IProbeGrid.hpp`
- `engine/rendering/IRayTracer.hpp`
- `engine/rendering/IReSTIRDI.hpp`
- `engine/rendering/IReflectionModel.hpp`
- `engine/rendering/IReflectionProvider.hpp`
- `engine/rendering/IRenderGraph.hpp`
- `engine/rendering/IRenderPassMetrics.hpp`
- `engine/rendering/IRenderingDebugView.hpp`
- `engine/rendering/IRenderingPresets.hpp`
- `engine/rendering/ISceneCulling.hpp`
- `engine/rendering/IScreenSpaceTracer.hpp`
- `engine/rendering/IShaderCompiler.hpp`
- `engine/rendering/ISoftwareTracer.hpp`
- `engine/rendering/ISparseVolumeGrid.hpp`
- `engine/rendering/ISpatialUpscaler.hpp`
- `engine/rendering/ISpirvReflection.hpp`
- `engine/rendering/ISurfaceCacheCapture.hpp`
- `engine/rendering/ISwapchainManager.hpp`
- `engine/rendering/ITemporalDenoiser.hpp`
- `engine/rendering/IToneMapping.hpp`
- `engine/rendering/IVolumeClouds.hpp`
- `engine/rendering/IXrMath.hpp`

### scripting (2)

- `engine/scripting/ILuauSandbox.hpp`
- `engine/scripting/IVisualScriptGraph.hpp`

### semantic (1)

- `engine/semantic/ISemanticApi.hpp`

### simfarm (1)

- `engine/simfarm/ISimulationFarm.hpp`

### simulation (3)

- `engine/simulation/IFixedTickSim.hpp`
- `engine/simulation/IMacroMicroReconciler.hpp`
- `engine/simulation/ISimulationLod.hpp`

### storage (1)

- `engine/storage/IChunkStoreFactory.hpp`

### timeline (2)

- `engine/timeline/ICausalResolver.hpp`
- `engine/timeline/ITimelineGraph.hpp`

### ui (11)

- `engine/ui/IConfirmation.hpp`
- `engine/ui/ICrafting.hpp`
- `engine/ui/IInventoryGrid.hpp`
- `engine/ui/IJsonSchema.hpp`
- `engine/ui/ILayout.hpp`
- `engine/ui/ITextShaper.hpp`
- `engine/ui/IUiDoc.hpp`
- `engine/ui/IViewport.hpp`
- `engine/ui/IWidgets.hpp`
- `engine/ui/qt/IQtEditorDoc.hpp`
- `engine/ui/qt/IQtThemeModel.hpp`

### vehicles (5)

- `engine/vehicles/IBeamGraphAsset.hpp`
- `engine/vehicles/IVehicleAsset.hpp`
- `engine/vehicles/IVehicleDamage.hpp`
- `engine/vehicles/IVehicleProvider.hpp`
- `engine/vehicles/IVehicleReplication.hpp`

### version.hpp (1)

- `engine/version.hpp`

### voxel (6)

- `engine/voxel/IBlockEntityScripting.hpp`
- `engine/voxel/IVoxelBlockEntity.hpp`
- `engine/voxel/IVoxelReplication.hpp`
- `engine/voxel/IVoxelServices.hpp`
- `engine/voxel/IVoxelStreaming.hpp`
- `engine/voxel/IVoxelWorld.hpp`

### world (7)

- `engine/world/ILocalSpace.hpp`
- `engine/world/IOriginRebase.hpp`
- `engine/world/IPortalSystem.hpp`
- `engine/world/ITimeTravel.hpp`
- `engine/world/ITimelinePolicy.hpp`
- `engine/world/IWorldManager.hpp`
- `engine/world/IWorldReplication.hpp`
