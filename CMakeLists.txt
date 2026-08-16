cmake_minimum_required(VERSION 3.20)
project(vulkan_craft_1_5 LANGUAGES C CXX)

enable_testing()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(VC_ENABLE_VOXEL_PLUGIN "Build the optional voxel plugin and voxel game targets" ON)
# Promoted solutions (META section 32 / DEPENDENCY_POLICY): Zstandard backs the
# persistent world save (compression) and BLAKE3 backs content hashing (save
# integrity, future content addressing). Sources compile directly from the
# pinned clones in external/solutions; external headers never reach the public
# API — consumers see only engine/compression and engine/hashing interfaces.
option(VC_ENABLE_ZSTD "Promote Zstandard into the build" ON)
option(VC_ENABLE_BLAKE3 "Promote BLAKE3 into the build" ON)
option(VC_ENABLE_FLATBUFFERS "Promote FlatBuffers into the build" ON)
option(VC_ENABLE_ROCKSDB "Promote RocksDB into the build" ON)
set(VC_SOLUTIONS_DIR ${CMAKE_SOURCE_DIR}/external/solutions)
set(VC_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)

if(VC_ENABLE_ZSTD)
    file(GLOB VC_ZSTD_COMMON ${VC_SOLUTIONS_DIR}/zstd/lib/common/*.c)
    file(GLOB VC_ZSTD_COMPRESS ${VC_SOLUTIONS_DIR}/zstd/lib/compress/*.c)
    file(GLOB VC_ZSTD_DECOMPRESS ${VC_SOLUTIONS_DIR}/zstd/lib/decompress/*.c)
    add_library(vc_zstd STATIC ${VC_ZSTD_COMMON} ${VC_ZSTD_COMPRESS} ${VC_ZSTD_DECOMPRESS})
    target_include_directories(vc_zstd PUBLIC ${VC_SOLUTIONS_DIR}/zstd/lib)
    if(MSVC)
        # Matches upstream: the assembly path is disabled for MSVC.
        target_compile_definitions(vc_zstd PRIVATE ZSTD_DISABLE_ASM=1)
    endif()
endif()

if(VC_ENABLE_BLAKE3)
    add_library(vc_blake3 STATIC
        ${VC_SOLUTIONS_DIR}/blake3/c/blake3.c
        ${VC_SOLUTIONS_DIR}/blake3/c/blake3_dispatch.c
        ${VC_SOLUTIONS_DIR}/blake3/c/blake3_portable.c
    )
    target_include_directories(vc_blake3 PUBLIC ${VC_SOLUTIONS_DIR}/blake3/c)
    # Portable path only: no assembly/SIMD sources, deterministic everywhere.
    target_compile_definitions(vc_blake3 PRIVATE
        BLAKE3_NO_SSE2=1 BLAKE3_NO_SSE41=1 BLAKE3_NO_AVX2=1 BLAKE3_NO_AVX512=1)
endif()

if(VC_ENABLE_FLATBUFFERS)
    # FlatBuffers promotes with its own CMake (flatc compiler + runtime lib).
    # Tests/benchmarks/install are all off; we only need the compiler to turn
    # our schema into C++ and the small static runtime to link it.
    set(FLATBUFFERS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(FLATBUFFERS_BUILD_FLATHASH OFF CACHE BOOL "" FORCE)
    set(FLATBUFFERS_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
    set(FLATBUFFERS_BUILD_GRPCTEST OFF CACHE BOOL "" FORCE)
    set(FLATBUFFERS_BUILD_SHAREDLIB OFF CACHE BOOL "" FORCE)
    set(FLATBUFFERS_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory(${VC_SOLUTIONS_DIR}/flatbuffers flatbuffers_build)
    add_library(vc_flatbuffers ALIAS flatbuffers)

    # Schema → C++ generation (v5 world save container).
    set(VC_WORLD_SAVE_FBS ${CMAKE_SOURCE_DIR}/src/engine/sdk/world_save.fbs)
    set(VC_WORLD_SAVE_GENERATED ${VC_GENERATED_DIR}/world_save_generated.h)
    add_custom_command(
        OUTPUT ${VC_WORLD_SAVE_GENERATED}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${VC_GENERATED_DIR}
        COMMAND $<TARGET_FILE:flatc> --cpp -o ${VC_GENERATED_DIR} ${VC_WORLD_SAVE_FBS}
        DEPENDS flatc ${VC_WORLD_SAVE_FBS}
        COMMENT "flatc: world_save.fbs -> world_save_generated.h"
    )
    add_custom_target(vc_generate_world_save DEPENDS ${VC_WORLD_SAVE_GENERATED})
endif()

if(VC_ENABLE_ROCKSDB)
    # RocksDB promotes with its own CMake; trim everything except the static
    # library (no shared, no tests/tools/benchmarks, no optional compression
    # backends — zstd stays OURS via vc_zstd). WIN_CI mirrors the official CI
    # flags (/MP, lighter debug info) and FAIL_ON_WARNINGS must be off for MSVC.
    set(ROCKSDB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(WITH_TESTS OFF CACHE BOOL "" FORCE)
    set(WITH_TOOLS OFF CACHE BOOL "" FORCE)
    set(WITH_BENCHMARK_TOOLS OFF CACHE BOOL "" FORCE)
    set(WITH_BENCHMARK OFF CACHE BOOL "" FORCE)
    set(WITH_JNI OFF CACHE BOOL "" FORCE)
    set(WITH_GFLAGS OFF CACHE BOOL "" FORCE)
    set(WITH_JEMALLOC OFF CACHE BOOL "" FORCE)
    set(WITH_SNAPPY OFF CACHE BOOL "" FORCE)
    set(WITH_LZ4 OFF CACHE BOOL "" FORCE)
    set(WITH_ZLIB OFF CACHE BOOL "" FORCE)
    set(WITH_ZSTD OFF CACHE BOOL "" FORCE)
    set(WITH_BZ2 OFF CACHE BOOL "" FORCE)
    set(WITH_XPRESS OFF CACHE BOOL "" FORCE)
    set(WITH_LIBURING OFF CACHE BOOL "" FORCE)
    set(FAIL_ON_WARNINGS OFF CACHE BOOL "" FORCE)
    set(WIN_CI ON CACHE BOOL "" FORCE)
    set(ROCKSDB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    add_subdirectory(${VC_SOLUTIONS_DIR}/rocksdb rocksdb_build)
    add_library(vc_rocksdb ALIAS rocksdb)
endif()

# EnTT — ECS storage authority (DEPENDENCY_POLICY), promoted 2026-08-14.
# Header-only (single_include), MIT, pinned commit 85c6bba014049b5de8fad49d25424df2f1f6a8c1.
# Hard dependency (like Jolt): no VC_ENABLE_* option. The public contract
# engine/entity/IEntityWorld.hpp never includes EnTT; only src/engine/sdk/
# EntityWorld.cpp does (adapter rule).
add_library(vc_entt INTERFACE)
target_include_directories(vc_entt INTERFACE
    ${VC_SOLUTIONS_DIR}/entt/single_include)

# Recast + Detour — navigation authority (DEPENDENCY_POLICY), promoted 2026-08-14.
# Zlib license, pinned commit 9f4ce64458dfae86e1239c525ddc219c4e9e06f1. Sources
# compiled directly from the pinned clone (Recast/Source + Detour/Source); the
# upstream CMake is not used. The public contract engine/navigation/* never
# includes Recast; only src/engine/sdk/RecastNavigation.cpp does (adapter rule).
file(GLOB VC_RECAST_SOURCES ${VC_SOLUTIONS_DIR}/recast-navigation/Recast/Source/*.cpp)
file(GLOB VC_DETOUR_SOURCES ${VC_SOLUTIONS_DIR}/recast-navigation/Detour/Source/*.cpp)
add_library(vc_navigation STATIC ${VC_RECAST_SOURCES} ${VC_DETOUR_SOURCES})
target_include_directories(vc_navigation PUBLIC
    ${VC_SOLUTIONS_DIR}/recast-navigation/Recast/Include
    ${VC_SOLUTIONS_DIR}/recast-navigation/Detour/Include)

# fast-wfc (Mathieu Fehr & Nathanaël Courant, MIT) — deterministic structures/
# interiors via wave function collapse. Sources compiled directly from the
# pinned clone (src/lib + src/include); the upstream CMake is not used. The
# public contract engine/procgen/IStructureGenerator.hpp never includes
# fast-wfc; only src/engine/sdk/FastWfcStructures.cpp does (adapter rule).
set(VC_FASTWFC_SOURCES
    ${VC_SOLUTIONS_DIR}/fast-wfc/src/lib/wave.cpp
    ${VC_SOLUTIONS_DIR}/fast-wfc/src/lib/propagator.cpp
    ${VC_SOLUTIONS_DIR}/fast-wfc/src/lib/wfc.cpp)
add_library(vc_fastwfc STATIC ${VC_FASTWFC_SOURCES})
target_include_directories(vc_fastwfc PUBLIC ${VC_SOLUTIONS_DIR}/fast-wfc/src/include)

# delaunator (Volodymyr Bilonenko, MIT) + earcut.hpp (Mapbox, ISC) — parceling/
# road graph via Delaunay triangulation and polygon triangulation. Header-only,
# used directly from the pinned clones; the upstream CMake is not used. The
# public contract engine/procgen/IParcellation.hpp never includes them; only
# src/engine/sdk/Parcellation.cpp does (adapter rule).
add_library(vc_delaunator INTERFACE)
target_include_directories(vc_delaunator INTERFACE ${VC_SOLUTIONS_DIR}/delaunator-cpp/include)
add_library(vc_earcut INTERFACE)
target_include_directories(vc_earcut INTERFACE ${VC_SOLUTIONS_DIR}/earcut-hpp/include)

# meshoptimizer (Arseny Kapoulkine, MIT) + xatlas (Jonathan Young, MIT) —
# headless mesh cooking (unwrap/optimize/simplify/analyze). Sources compiled
# directly from the pinned clones; upstream CMake is not used. The public
# contract engine/procgen/IMeshCooking.hpp never includes them; only
# src/engine/sdk/MeshCooking.cpp does (adapter rule). manifold (TBB) and
# geometry-central (Eigen) are BLOCKED at the gate — deps not vendored (see
# DEPENDENCY_POLICY).
file(GLOB VC_MESHOPTIMIZER_SOURCES ${VC_SOLUTIONS_DIR}/meshoptimizer/src/*.cpp)
add_library(vc_meshoptimizer STATIC ${VC_MESHOPTIMIZER_SOURCES})
target_include_directories(vc_meshoptimizer PUBLIC ${VC_SOLUTIONS_DIR}/meshoptimizer/src)
add_library(vc_xatlas STATIC ${VC_SOLUTIONS_DIR}/xatlas/source/xatlas/xatlas.cpp)
target_include_directories(vc_xatlas PUBLIC ${VC_SOLUTIONS_DIR}/xatlas/source/xatlas)

# FastNoise2 (catalog authority for "noise graph") is BLOCKED at the promotion
# gate: this pinned clone requires FastSIMD (include/FastNoise/Utility/Config.h
# -> <FastSIMD/DispatchClass.h>) which is not vendored in the clone nor in the
# catalog. The procgen contract (engine/procgen/INoiseGraph.hpp) is implemented
# with the engine's in-project noise authority (FastNoiseLite, already used by
# TerrainGenerator/World) behind the adapter; swapping to FastNoise2 only needs
# the adapter once FastSIMD is vendored (see DEPENDENCY_POLICY).

if(CMAKE_CONFIGURATION_TYPES)
    list(APPEND CMAKE_CONFIGURATION_TYPES Shipping)
    list(REMOVE_DUPLICATES CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "${CMAKE_CONFIGURATION_TYPES}" CACHE STRING "Available build configurations" FORCE)
    foreach(lang C CXX)
        set(CMAKE_${lang}_FLAGS_SHIPPING "${CMAKE_${lang}_FLAGS_RELEASE}" CACHE STRING "Shipping flags" FORCE)
    endforeach()
    set(CMAKE_EXE_LINKER_FLAGS_SHIPPING "${CMAKE_EXE_LINKER_FLAGS_RELEASE}" CACHE STRING "Shipping linker flags" FORCE)
endif()

if(MSVC)
    add_compile_options(/O2 /fp:fast /arch:AVX2)
endif()

include(FetchContent)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4)
FetchContent_Declare(glm GIT_REPOSITORY https://github.com/g-truc/glm.git GIT_TAG 1.0.1)
FetchContent_Declare(vk-bootstrap GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap.git GIT_TAG v0.7)
FetchContent_Declare(vma GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git GIT_TAG v3.1.0)
FetchContent_Declare(miniaudio GIT_REPOSITORY https://github.com/mackron/miniaudio.git GIT_TAG 0.11.21)
# Pin the docking branch to an immutable commit. Mutable branch names make
# otherwise identical builds resolve to different source trees over time.
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG 83f668625ad45364de71d385aeb6a5dd04bee02e)
FetchContent_MakeAvailable(glfw glm vk-bootstrap vma miniaudio imgui)
find_package(Vulkan REQUIRED)

# Vendored production libraries (Jolt Physics, Bullet).
add_subdirectory(third_party)

set(VC_SOURCE_DIRS
    src
    src/engine/public
    src/app
    src/engine/core/uuid
    src/engine/core/reflection
    src/engine/core/serialization
    src/engine/core/plugin
    src/engine/scene
    src/engine/editor/undo
    src/engine/editor/play_mode
    src/engine/editor/ui
    src/engine/assets
    src/engine/animation
    src/engine/scripting
    src/engine/jobs
    src/engine/audio
    src/engine/rendering/renderer
    src/engine/rendering/materials
    src/engine/rendering/lighting
    src/engine/rendering/vulkan
    src/plugins/voxel
    src/simulation/voxel/core
    src/simulation/voxel/storage
    src/simulation/voxel/generation
    src/simulation/voxel/meshing
    src/simulation/voxel/lighting
    src/simulation/voxel/simulation
    src/simulation/voxel/streaming
    src/simulation/voxel/structure
    src/simulation/entities
    src/features/player
    src/features/inventory
    src/features/weapons
    src/features/vehicles
)

add_library(vc_build_options INTERFACE)
target_include_directories(vc_build_options INTERFACE
    ${VC_SOURCE_DIRS}
    ${Vulkan_INCLUDE_DIRS}
    ${vma_SOURCE_DIR}/include
    ${miniaudio_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/third_party/jolt
    ${CMAKE_SOURCE_DIR}/third_party/bullet/src
)
target_compile_definitions(vc_build_options INTERFACE
    VULKANCRAFT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    VULKANCRAFT_SHADER_DIR="${CMAKE_BINARY_DIR}/shaders"
    $<$<CONFIG:Shipping>:VC_SHIPPING=1>
    $<$<NOT:$<CONFIG:Shipping>>:VC_SHIPPING=0>
    VC_ENABLE_VOXEL_PLUGIN=$<BOOL:${VC_ENABLE_VOXEL_PLUGIN}>
    VC_ENABLE_FLATBUFFERS=$<BOOL:${VC_ENABLE_FLATBUFFERS}>
)
target_link_libraries(vc_build_options INTERFACE
    glfw
    glm
    vk-bootstrap
    Vulkan::Vulkan
    jolt
    bullet
)

function(vc_object_module target)
    add_library(${target} OBJECT ${ARGN})
    target_link_libraries(${target} PRIVATE vc_build_options)
endfunction()

vc_object_module(vc_core_runtime
    src/engine/core/uuid/UUID.cpp
    src/engine/core/plugin/PluginRuntime.cpp
    src/engine/core/serialization/Serializer.cpp
    src/engine/assets/AssetRegistry.cpp
    src/engine/assets/GltfGeometry.cpp
    src/engine/assets/GltfAssets.cpp
    src/engine/assets/ExrDecoder.cpp
    src/engine/assets/FbxImporter.cpp
    src/engine/assets/ThumbnailCache.cpp
    src/engine/assets/RuntimePackage.cpp
    src/engine/audio/OggDecoder.cpp
    src/engine/audio/MiniaudioImpl.cpp
    src/engine/animation/AnimationRuntime.cpp
    src/engine/animation/AnimationAssets.cpp
    src/engine/scene/Scene.cpp
    src/engine/scene/Prefab.cpp
    src/engine/scene/ComponentRegistry.cpp
    src/engine/scene/SceneComponentIntegration.cpp
    src/engine/rendering/materials/Material.cpp
    third_party/tinyexr.cc
    third_party/miniz.c
    third_party/ufbx.c
)
vc_object_module(vc_voxel_generation
    src/simulation/voxel/generation/TerrainGenerator.cpp
    src/simulation/voxel/structure/Structures.cpp
)
vc_object_module(vc_voxel_runtime
    src/simulation/voxel/storage/Chunk.cpp
    src/simulation/voxel/meshing/ChunkSnapshot.cpp
    src/simulation/voxel/meshing/VoxelMesher.cpp
    src/simulation/voxel/lighting/ChunkLighting.cpp
    src/simulation/voxel/simulation/WorldScheduler.cpp
    src/simulation/voxel/streaming/World.cpp
    src/simulation/voxel/tools/VoxelTools.cpp
    src/plugins/voxel/VoxelStructure.cpp
)
vc_object_module(vc_sdk_public
    src/engine/sdk/VoxelWorldFacade.cpp
    src/engine/sdk/BlockRegistry.cpp
    src/engine/sdk/FluidRegistry.cpp
    src/engine/sdk/ItemRegistry.cpp
    src/engine/sdk/Inventory.cpp
    src/engine/sdk/RecipeRegistry.cpp
    src/engine/sdk/EntityWorld.cpp
    src/engine/sdk/RecastNavigation.cpp
    src/engine/sdk/VoxelReplication.cpp
    src/engine/sdk/FastNoiseGraph.cpp
    src/engine/sdk/ClimateBiome.cpp
    src/engine/sdk/WorldFeatures.cpp
    src/engine/sdk/FastWfcStructures.cpp
    src/engine/sdk/Parcellation.cpp
    src/engine/sdk/ShapeGrammar.cpp
    src/engine/sdk/HeightmapErosion.cpp
    src/engine/sdk/MeshCooking.cpp
    src/engine/sdk/ProcgenPreview.cpp
    src/engine/sdk/ProcgenJobs.cpp
    src/engine/sdk/LodTerrainSampler.cpp
    src/engine/sdk/MobBehavior.cpp
    src/engine/sdk/WorldManager.cpp
    src/engine/sdk/RegistryJson.cpp

    src/engine/core/compression/ZstdCompression.cpp
    src/engine/core/hashing/Blake3Hash.cpp
    src/engine/core/storage/RocksDbChunkStorage.cpp
    src/engine/core/storage/RegionChunkStorage.cpp
)
# Promoted-solution adapters: these TUs are the ONLY ones that include the
# external headers; the include dirs stay private to this module.
target_include_directories(vc_sdk_public PRIVATE
    ${VC_SOLUTIONS_DIR}/entt/single_include
    ${VC_SOLUTIONS_DIR}/recast-navigation/Recast/Include
    ${VC_SOLUTIONS_DIR}/recast-navigation/Detour/Include
    ${VC_SOLUTIONS_DIR}/fast-wfc/src/include
    ${VC_SOLUTIONS_DIR}/delaunator-cpp/include
    ${VC_SOLUTIONS_DIR}/earcut-hpp/include
    ${VC_SOLUTIONS_DIR}/meshoptimizer/src
    ${VC_SOLUTIONS_DIR}/xatlas/source/xatlas
)
if(VC_ENABLE_ZSTD OR VC_ENABLE_BLAKE3 OR VC_ENABLE_FLATBUFFERS OR VC_ENABLE_ROCKSDB)
    target_include_directories(vc_sdk_public PRIVATE
        $<$<BOOL:${VC_ENABLE_ZSTD}>:${VC_SOLUTIONS_DIR}/zstd/lib>
        $<$<BOOL:${VC_ENABLE_BLAKE3}>:${VC_SOLUTIONS_DIR}/blake3/c>
        $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:${VC_SOLUTIONS_DIR}/flatbuffers/include>
        $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:${VC_GENERATED_DIR}>
        $<$<BOOL:${VC_ENABLE_ROCKSDB}>:${VC_SOLUTIONS_DIR}/rocksdb/include>
    )
endif()
if(VC_ENABLE_FLATBUFFERS)
    # The generated header is produced by flatc before this module compiles.
    add_dependencies(vc_sdk_public vc_generate_world_save)
endif()
# Public gameplay runtime (FALTANTES item 9 / META section 20): the adapter
# src/engine/sdk/GameplayRuntime.cpp is the ONLY TU that crosses from the
# public surface into the internal physics/gameplay layers. The internal TUs
# are compiled into this module so external consumers (tests, projects) link
# them together with the public contract.
vc_object_module(vc_gameplay_sdk
    src/engine/sdk/GameplayRuntime.cpp
    src/engine/physics/PhysicsRuntime.cpp
    src/engine/physics/PhysicsBackend.cpp
    src/engine/physics/JoltPhysicsBackend.cpp
    src/engine/physics/BulletPhysicsBackend.cpp
    src/engine/physics/Ragdoll.cpp
    src/engine/gameplay/WeaponSystem.cpp
    src/engine/gameplay/VehicleRuntime.cpp
    src/engine/gameplay/DestructionRuntime.cpp
    src/engine/core/uuid/UUID.cpp
)
target_include_directories(vc_gameplay_sdk PRIVATE src)
vc_object_module(vc_advanced_runtime
    src/engine/physics/PhysicsRuntime.cpp
    src/engine/physics/PhysicsAdvanced.cpp
    src/engine/physics/Ragdoll.cpp
    src/engine/physics/PhysicsBackend.cpp
    src/engine/physics/JoltPhysicsBackend.cpp
    src/engine/physics/BulletPhysicsBackend.cpp
    src/engine/gameplay/GameplayVisual.cpp
    src/engine/gameplay/WeaponSystem.cpp
    src/engine/gameplay/VehicleRuntime.cpp
    src/engine/gameplay/DestructionRuntime.cpp
    src/engine/gameplay/ParticleSimulation.cpp
    src/engine/gameplay/MissionSystem.cpp
    src/engine/gameplay/DialogueSystem.cpp
    src/engine/audio/AudioRuntime.cpp
    src/engine/networking/NetworkRuntime.cpp
    src/engine/scripting/ScriptRuntime.cpp
    src/engine/scripting/ScriptDebugger.cpp
    src/engine/scripting/VisualScriptCanvas.cpp
    src/engine/scripting/ScriptGraphBridge.cpp
    src/engine/world/WorldPartition.cpp
    src/engine/world/WorldPartitionProfiler.cpp
    src/engine/world/WorldRegionPackage.cpp
    src/engine/rendering/MaterialGraph.cpp
    src/engine/rendering/RenderGraph.cpp
    src/engine/rendering/RenderingFoundation.cpp
)
# Material pipeline is Vulkan-only: the headless server must not link it.
vc_object_module(vc_material_pipeline src/engine/rendering/vulkan/MaterialPipeline.cpp)
vc_object_module(vc_editor_extensions
    src/editor/tools/EditorRegistry.cpp
    src/editor/tools/RenderingToolModels.cpp
    src/plugins/PluginContract.cpp
    src/plugins/PluginEditorExtensions.cpp
    src/plugins/vehicles/VehiclesPlugin.cpp
    src/plugins/weapons/WeaponsPlugin.cpp
    src/plugins/missions/MissionsPlugin.cpp
    src/plugins/procedural_animation/ProceduralAnimationPlugin.cpp
    src/plugins/advanced_physics/AdvancedPhysicsPlugin.cpp
    src/plugins/networking/NetworkingPlugin.cpp
)
vc_object_module(vc_feature_player src/features/player/Player.cpp)
vc_object_module(vc_engine_runtime
    src/engine/audio/SoundEngine.cpp
    src/engine/audio/AudioEvent.cpp
    src/engine/physics/Physics.cpp
    src/engine/scripting/VisualScriptGraph.cpp
    src/engine/rendering/renderer/ChunkRenderer.cpp
    src/engine/rendering/renderer/WorldRenderer.cpp
    src/engine/rendering/renderer/FarTerrain.cpp
    src/engine/rendering/renderer/MobRenderer.cpp
    src/engine/rendering/materials/TextureManager.cpp
    src/engine/rendering/lighting/RadianceCache.cpp
)
vc_object_module(vc_app_runtime
    src/app/Engine.cpp
    src/app/EngineLifecycle.cpp
    src/app/GameApplication.cpp
    src/platform/window/EngineWindow.cpp
)

set(VC_SHADER_SOURCE_DIR "${CMAKE_SOURCE_DIR}/shaders/active")
set(VC_SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${VC_SHADER_OUTPUT_DIR}")
find_program(GLSLC_EXECUTABLE NAMES glslc HINTS "$ENV{VULKAN_SDK}/Bin" REQUIRED)
set(VC_SHADERS
    voxel.vert far_surface.vert voxel.frag grass.vert foliage.vert
    sky.vert sky.frag post.vert post.frag shadow.vert
    shadow_far_surface.vert shadow.frag shadow_foliage.vert shadow_grass.vert
    editor_viewport.vert editor_viewport.frag editor_pick.frag editor_material.vert
)
set(VC_SHADER_OUTPUTS)
foreach(shader IN LISTS VC_SHADERS)
    set(output "${VC_SHADER_OUTPUT_DIR}/${shader}.spv")
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${GLSLC_EXECUTABLE}" -I "${VC_SHADER_SOURCE_DIR}"
                "${VC_SHADER_SOURCE_DIR}/${shader}" -o "${output}"
        DEPENDS "${VC_SHADER_SOURCE_DIR}/${shader}" "${VC_SHADER_SOURCE_DIR}/radiance_cache.glsl"
        VERBATIM
    )
    list(APPEND VC_SHADER_OUTPUTS "${output}")
endforeach()
add_custom_target(compile_shaders ALL DEPENDS ${VC_SHADER_OUTPUTS})

set(VC_REQUIRED_ASSETS
    assets/audio
    assets/skins
    assets/textures/minecraft/assets/minecraft/textures/block
    assets/textures/grass/assets/minecraft/textures/block
)
foreach(asset_dir IN LISTS VC_REQUIRED_ASSETS)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${asset_dir}")
        message(FATAL_ERROR "Runtime asset directory is missing: ${asset_dir}")
    endif()
endforeach()

add_executable(VulkanEngineGame
    src/app/main_game.cpp
    $<TARGET_OBJECTS:vc_core_runtime>
    $<TARGET_OBJECTS:vc_advanced_runtime>
)
target_compile_features(VulkanEngineGame PRIVATE cxx_std_20)
target_include_directories(VulkanEngineGame PRIVATE src ${glm_SOURCE_DIR})
target_link_libraries(VulkanEngineGame PRIVATE vc_build_options glfw glm vk-bootstrap Vulkan::Vulkan vc_material_pipeline)

if(VC_ENABLE_VOXEL_PLUGIN)
add_executable(vulkan_craft
    src/app/main.cpp
    src/plugins/voxel/VoxelPlugin.cpp
    # Public entity layer + mob behavior (FALTANTES item 11): the legacy
    # Mob/MobManager track was removed; the app hosts mobs as IEntityWorld
    # entities advanced by the public IMobBehavior.
    src/engine/sdk/EntityWorld.cpp
    src/engine/sdk/MobBehavior.cpp
    src/engine/sdk/RegistryJson.cpp
    $<TARGET_OBJECTS:vc_core_runtime>
    $<$<BOOL:${VC_ENABLE_VOXEL_PLUGIN}>:$<TARGET_OBJECTS:vc_voxel_generation>>
    $<$<BOOL:${VC_ENABLE_VOXEL_PLUGIN}>:$<TARGET_OBJECTS:vc_voxel_runtime>>
    $<TARGET_OBJECTS:vc_advanced_runtime>
    $<TARGET_OBJECTS:vc_editor_extensions>
    $<TARGET_OBJECTS:vc_feature_player>
    $<TARGET_OBJECTS:vc_engine_runtime>
    $<TARGET_OBJECTS:vc_app_runtime>
)
add_dependencies(vulkan_craft compile_shaders)
target_include_directories(vulkan_craft PRIVATE
    ${VC_SOLUTIONS_DIR}/entt/single_include
)
target_link_libraries(vulkan_craft PRIVATE
    vc_build_options glfw glm vk-bootstrap Vulkan::Vulkan windowscodecs ole32
)
endif()

set(IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)

add_executable(VulkanEngineEditor
    src/editor/main_editor.cpp
    src/editor/EditorApplication.cpp
    src/engine/editor/ui/EditorGUI.cpp
    src/editor/tools/SpecializedEditorsPanel.cpp
    # Public navigation provider (FALTANTES item 12): the editor's play nav
    # bakes via Recast + Detour — the legacy grid track was removed.
    src/engine/sdk/RecastNavigation.cpp
    $<$<BOOL:${VC_ENABLE_VOXEL_PLUGIN}>:src/engine/audio/SoundEngine.cpp>
    $<$<BOOL:${VC_ENABLE_VOXEL_PLUGIN}>:src/plugins/voxel/VoxelPlugin.cpp>
    ${IMGUI_SOURCES}
    $<TARGET_OBJECTS:vc_core_runtime>
    $<$<BOOL:${VC_ENABLE_VOXEL_PLUGIN}>:$<TARGET_OBJECTS:vc_voxel_generation>>
    $<$<BOOL:${VC_ENABLE_VOXEL_PLUGIN}>:$<TARGET_OBJECTS:vc_voxel_runtime>>
    $<TARGET_OBJECTS:vc_advanced_runtime>
    $<TARGET_OBJECTS:vc_editor_extensions>
)
target_include_directories(VulkanEngineEditor PRIVATE
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    src/editor
    ${VC_SOLUTIONS_DIR}/recast-navigation/Recast/Include
    ${VC_SOLUTIONS_DIR}/recast-navigation/Detour/Include
)
target_link_libraries(VulkanEngineEditor PRIVATE
    vc_build_options glfw glm vk-bootstrap Vulkan::Vulkan windowscodecs ole32 vc_material_pipeline vc_navigation
)
add_dependencies(VulkanEngineEditor compile_shaders)

add_executable(VulkanEngineServer
    src/server/main_server.cpp
    $<TARGET_OBJECTS:vc_core_runtime>
    $<TARGET_OBJECTS:vc_advanced_runtime>
)
target_compile_features(VulkanEngineServer PRIVATE cxx_std_20)
target_include_directories(VulkanEngineServer PRIVATE src ${glm_SOURCE_DIR})
target_link_libraries(VulkanEngineServer PRIVATE glm jolt bullet)

add_executable(terrain_generator_tests
    tests/TerrainGeneratorTests.cpp
    src/simulation/voxel/generation/TerrainGenerator.cpp
)
target_link_libraries(terrain_generator_tests PRIVATE vc_build_options)
add_test(NAME terrain_generator_tests COMMAND terrain_generator_tests)

add_executable(thread_pool_tests tests/ThreadPoolTests.cpp)
target_link_libraries(thread_pool_tests PRIVATE vc_build_options)
add_test(NAME thread_pool_tests COMMAND thread_pool_tests)

add_executable(gltf_geometry_tests
    tests/GltfGeometryTests.cpp
    src/engine/assets/GltfGeometry.cpp
)
target_link_libraries(gltf_geometry_tests PRIVATE vc_build_options glm)
add_test(NAME gltf_geometry_tests COMMAND gltf_geometry_tests)

add_executable(radiance_cache_math_tests tests/RadianceCacheMathTests.cpp)
target_link_libraries(radiance_cache_math_tests PRIVATE vc_build_options glm)
add_test(NAME radiance_cache_math_tests COMMAND radiance_cache_math_tests)

add_executable(architecture_boundary_tests tests/ArchitectureBoundaryTests.cpp)
target_link_libraries(architecture_boundary_tests PRIVATE vc_build_options)
add_test(NAME architecture_boundary_tests COMMAND architecture_boundary_tests)

add_executable(empty_project_tests
    tests/EmptyProjectTests.cpp
    src/tools/BuildTools.cpp
    src/tools/ProjectConfig.cpp
    src/tools/BuildPipeline.cpp
    src/engine/core/uuid/UUID.cpp
    src/engine/core/serialization/Serializer.cpp
    src/engine/assets/AssetRegistry.cpp
    src/engine/assets/GltfGeometry.cpp
    src/engine/assets/RuntimePackage.cpp
    src/engine/audio/OggDecoder.cpp
    src/engine/audio/MiniaudioImpl.cpp
    src/engine/animation/AnimationRuntime.cpp
    src/engine/animation/AnimationAssets.cpp
    src/engine/scripting/ScriptRuntime.cpp
    src/engine/scripting/VisualScriptCanvas.cpp
    src/engine/scripting/ScriptGraphBridge.cpp
    src/engine/physics/PhysicsRuntime.cpp
    src/engine/physics/PhysicsBackend.cpp
    src/engine/physics/JoltPhysicsBackend.cpp
    src/engine/physics/BulletPhysicsBackend.cpp
    src/engine/physics/Ragdoll.cpp
    src/engine/scene/Scene.cpp
    src/engine/scene/Prefab.cpp
    src/engine/scene/ComponentRegistry.cpp
    src/engine/scene/SceneComponentIntegration.cpp
    src/engine/rendering/materials/Material.cpp
    src/engine/audio/AudioEvent.cpp
    src/engine/physics/Physics.cpp
    src/engine/scripting/VisualScriptGraph.cpp
    src/engine/assets/ExrDecoder.cpp
    src/engine/assets/FbxImporter.cpp
    third_party/tinyexr.cc
    third_party/miniz.c
    third_party/ufbx.c
)
target_link_libraries(empty_project_tests PRIVATE vc_build_options glm)
add_test(NAME empty_project_tests COMMAND empty_project_tests)

add_executable(engine_foundation_tests
    tests/EngineFoundationTests.cpp
    src/tools/AssetCooker.cpp
    src/engine/core/uuid/UUID.cpp
    src/engine/core/plugin/PluginRuntime.cpp
    src/engine/core/serialization/Serializer.cpp
    src/engine/assets/AssetRegistry.cpp
    src/engine/assets/GltfGeometry.cpp
    src/engine/assets/GltfAssets.cpp
    src/engine/assets/ExrDecoder.cpp
    src/engine/assets/FbxImporter.cpp
    src/engine/assets/ThumbnailCache.cpp
    src/engine/assets/RuntimePackage.cpp
    src/engine/audio/OggDecoder.cpp
    src/engine/audio/MiniaudioImpl.cpp
    src/engine/animation/AnimationRuntime.cpp
    src/engine/animation/AnimationAssets.cpp
    src/engine/scene/Scene.cpp
    src/engine/scene/Prefab.cpp
    src/engine/scene/ComponentRegistry.cpp
    src/engine/scene/SceneComponentIntegration.cpp
    src/engine/rendering/materials/Material.cpp
    src/engine/audio/AudioEvent.cpp
    src/engine/physics/Physics.cpp
    src/engine/scripting/VisualScriptGraph.cpp
    src/engine/scripting/ScriptRuntime.cpp
    third_party/tinyexr.cc
    third_party/miniz.c
    third_party/ufbx.c
)
target_link_libraries(engine_foundation_tests PRIVATE vc_build_options)
add_test(NAME engine_foundation_tests COMMAND engine_foundation_tests)

# Runs the same physics scenarios against every backend (builtin, jolt, bullet).
add_executable(physics_backend_tests
    tests/PhysicsBackendTests.cpp
    src/engine/physics/PhysicsRuntime.cpp
    src/engine/physics/PhysicsBackend.cpp
    src/engine/physics/JoltPhysicsBackend.cpp
    src/engine/physics/BulletPhysicsBackend.cpp
)
target_compile_features(physics_backend_tests PRIVATE cxx_std_20)
target_include_directories(physics_backend_tests PRIVATE src)
target_link_libraries(physics_backend_tests PRIVATE vc_build_options glm)
add_test(NAME physics_backend_tests COMMAND physics_backend_tests)

add_executable(advanced_systems_tests
    tests/AdvancedSystemsTests.cpp
    src/engine/core/uuid/UUID.cpp
    src/engine/animation/AnimationRuntime.cpp
    src/engine/animation/AnimationAssets.cpp
    src/engine/scripting/ScriptRuntime.cpp
    src/engine/scripting/VisualScripting.cpp
    src/engine/core/serialization/Serializer.cpp
    src/engine/scene/Scene.cpp
    src/engine/scene/Prefab.cpp
    src/engine/scene/ComponentRegistry.cpp
    src/engine/scene/SceneComponentIntegration.cpp
    src/engine/gameplay/GameplayVisual.cpp
    src/engine/gameplay/WeaponSystem.cpp
    src/engine/sdk/RecastNavigation.cpp
    src/engine/audio/OggDecoder.cpp
    src/engine/audio/MiniaudioImpl.cpp
    src/engine/audio/AudioZones.cpp
    src/engine/audio/AudioRuntime.cpp
    src/engine/physics/PhysicsAdvanced.cpp
    src/engine/physics/PhysicsRuntime.cpp
    src/engine/physics/PhysicsBackend.cpp
    src/engine/physics/JoltPhysicsBackend.cpp
    src/engine/physics/BulletPhysicsBackend.cpp
    src/engine/networking/SocketTransport.cpp
    src/engine/networking/ReliableTransport.cpp
    src/engine/world/WorldPartition.cpp
    src/engine/world/WorldPartitionProfiler.cpp
    src/engine/world/WorldRegionPackage.cpp
    src/tools/BuildTools.cpp
)
target_include_directories(advanced_systems_tests PRIVATE
    ${VC_SOLUTIONS_DIR}/recast-navigation/Recast/Include
    ${VC_SOLUTIONS_DIR}/recast-navigation/Detour/Include
)
target_link_libraries(advanced_systems_tests PRIVATE vc_build_options glm vc_navigation)
add_test(NAME advanced_systems_tests COMMAND advanced_systems_tests)

add_executable(rendering_tools_tests
    tests/RenderingToolsTests.cpp
    src/engine/core/uuid/UUID.cpp
    src/engine/rendering/MaterialGraph.cpp
    src/engine/rendering/RenderGraph.cpp
    src/engine/rendering/RenderingFoundation.cpp
    src/engine/rendering/vulkan/MaterialPipeline.cpp
    src/editor/tools/RenderingToolModels.cpp
)
target_link_libraries(rendering_tools_tests PRIVATE vc_build_options glm)
add_test(NAME rendering_tools_tests COMMAND rendering_tools_tests)

add_executable(gameplay_framework_tests
    tests/GameplayFrameworkTests.cpp
    src/engine/gameplay/GameplayFramework.cpp
    src/engine/gameplay/MissionSystem.cpp
    src/engine/gameplay/DialogueSystem.cpp
    src/engine/gameplay/WeaponSystem.cpp
    src/engine/gameplay/ParticleSimulation.cpp
    src/engine/gameplay/VehicleRuntime.cpp
    src/engine/gameplay/DestructionRuntime.cpp
    src/engine/audio/AudioRuntime.cpp
    src/engine/sdk/RecastNavigation.cpp
    src/engine/physics/PhysicsRuntime.cpp
    src/engine/physics/PhysicsBackend.cpp
    src/engine/physics/JoltPhysicsBackend.cpp
    src/engine/physics/BulletPhysicsBackend.cpp
    src/engine/core/uuid/UUID.cpp
)

target_include_directories(gameplay_framework_tests PRIVATE
    ${VC_SOLUTIONS_DIR}/recast-navigation/Recast/Include
    ${VC_SOLUTIONS_DIR}/recast-navigation/Detour/Include
)
target_link_libraries(gameplay_framework_tests PRIVATE vc_build_options glm vc_navigation)
add_test(NAME gameplay_framework_tests COMMAND gameplay_framework_tests)

# Public gameplay runtime (FALTANTES item 9 / META section 20): the contract
# compiles only with public SDK headers; the equivalence gates drive the same
# scenarios through the INTERNAL runtimes (the internal layers never leak into
# the public surface).
add_executable(gameplay_runtime_tests
    tests/GameplayRuntimeTests.cpp
    $<TARGET_OBJECTS:vc_gameplay_sdk>
)
target_include_directories(gameplay_runtime_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(gameplay_runtime_tests PRIVATE vc_build_options glm)
add_test(NAME gameplay_runtime_tests COMMAND gameplay_runtime_tests)

add_executable(visual_authoring_tests
    tests/VisualAuthoringTests.cpp
    src/engine/scripting/VisualScriptCanvas.cpp
    src/engine/scripting/ScriptGraphBridge.cpp
    src/engine/scripting/ScriptRuntime.cpp
    src/engine/scripting/ScriptDebugger.cpp
    src/editor/tools/AudioEditorModel.cpp
    src/engine/core/uuid/UUID.cpp
)
target_link_libraries(visual_authoring_tests PRIVATE vc_build_options glm)
add_test(NAME visual_authoring_tests COMMAND visual_authoring_tests)

add_executable(script_debugger_tests
    tests/ScriptDebuggerTests.cpp
    src/engine/scripting/ScriptDebugger.cpp
    src/engine/scripting/ScriptHotReload.cpp
    src/editor/EditorRuntimeBridge.cpp
    src/engine/scripting/ScriptRuntime.cpp
    src/engine/core/uuid/UUID.cpp
    src/engine/scene/Scene.cpp
    src/engine/scene/Prefab.cpp
    src/engine/scene/ComponentRegistry.cpp
    src/engine/core/serialization/Serializer.cpp
)
target_link_libraries(script_debugger_tests PRIVATE vc_build_options glm)
add_test(NAME script_debugger_tests COMMAND script_debugger_tests)

if(VC_ENABLE_VOXEL_PLUGIN)
add_executable(voxel_tools_tests
    tests/VoxelToolsTests.cpp
    src/engine/core/uuid/UUID.cpp
    src/engine/assets/AssetRegistry.cpp
    src/engine/assets/GltfGeometry.cpp
    src/engine/assets/ExrDecoder.cpp
    src/engine/assets/FbxImporter.cpp
    src/engine/audio/OggDecoder.cpp
    src/engine/audio/MiniaudioImpl.cpp
    src/engine/core/serialization/Serializer.cpp
    src/engine/scene/Scene.cpp
    src/engine/scene/Prefab.cpp
    src/engine/scene/ComponentRegistry.cpp
    src/engine/scene/SceneComponentIntegration.cpp
    src/engine/rendering/materials/Material.cpp
    src/engine/audio/AudioEvent.cpp
    src/engine/physics/Physics.cpp
    src/engine/scripting/VisualScriptGraph.cpp
    src/engine/animation/AnimationRuntime.cpp
    src/engine/animation/AnimationAssets.cpp
    src/editor/tools/EditorRegistry.cpp
    third_party/tinyexr.cc
    third_party/miniz.c
    third_party/ufbx.c
    src/plugins/PluginContract.cpp
    src/plugins/PluginEditorExtensions.cpp
    src/plugins/voxel/VoxelPlugin.cpp
    src/plugins/voxel/VoxelStructure.cpp
    src/simulation/voxel/tools/VoxelTools.cpp
)
target_link_libraries(voxel_tools_tests PRIVATE vc_build_options glm)
add_test(NAME voxel_tools_tests COMMAND voxel_tools_tests)

# Voxel streaming pipeline: MeshReady->Uploaded transition, async remesh on
# edit, border propagation and no remesh ping-pong (headless, no Vulkan).
add_executable(voxel_streaming_tests
    tests/VoxelStreamingTests.cpp
    src/engine/core/uuid/UUID.cpp
    src/simulation/voxel/storage/Chunk.cpp
    src/simulation/voxel/meshing/ChunkSnapshot.cpp
    src/simulation/voxel/meshing/VoxelMesher.cpp
    src/simulation/voxel/lighting/ChunkLighting.cpp
    src/simulation/voxel/simulation/WorldScheduler.cpp
    src/simulation/voxel/streaming/World.cpp
    src/simulation/voxel/generation/TerrainGenerator.cpp
    src/simulation/voxel/structure/Structures.cpp
    src/plugins/voxel/VoxelStructure.cpp
    src/engine/audio/SoundEngine.cpp
    src/engine/audio/MiniaudioImpl.cpp
)
target_link_libraries(voxel_streaming_tests PRIVATE vc_build_options)
add_test(NAME voxel_streaming_tests COMMAND voxel_streaming_tests)

# Deterministic world tick scheduler (META section 9): fixed-tick clock, phase
# ordering, budgets with carry-over, dedup/cancel, active regions and
# seed-driven random ticks (headless, std-only, no Vulkan).
add_executable(voxel_scheduler_tests
    tests/VoxelSchedulerTests.cpp
    src/simulation/voxel/simulation/WorldScheduler.cpp
)
target_link_libraries(voxel_scheduler_tests PRIVATE vc_build_options)
add_test(NAME voxel_scheduler_tests COMMAND voxel_scheduler_tests)

# SDK evidence target: the test TU compiles against ONLY the public headers
# (src/engine/public) plus glm — the stand-in for an external project. The
# engine implementation comes from the SDK/voxel OBJECT modules.
add_executable(voxel_sdk_tests
    tests/VoxelSdkTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/networking/NetworkRuntime.cpp
)
target_include_directories(voxel_sdk_tests PRIVATE src/engine/public ${glm_SOURCE_DIR})
target_link_libraries(voxel_sdk_tests PRIVATE
    vc_build_options
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
    vc_navigation
    vc_fastwfc
    vc_meshoptimizer
    vc_xatlas
)
if(TARGET vc_generate_world_save)
    add_dependencies(voxel_sdk_tests vc_generate_world_save)
endif()
add_test(NAME voxel_sdk_tests COMMAND voxel_sdk_tests)

# MCP registry-asset gate (FALTANTES item 23 / prioridade 5): the versioned
# JSON documents the MCP semantic layer authors (Content/Registry/<kind>/*.json)
# must load through the PUBLIC factories unchanged. The test TU compiles against
# only the public headers (like voxel_sdk_tests) and mirrors the exact documents
# tools/mcp-server/protocol-smoke.mjs emits.
add_executable(mcp_registry_gate_tests
    tests/McpRegistryGateTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/networking/NetworkRuntime.cpp
)
target_include_directories(mcp_registry_gate_tests PRIVATE src/engine/public ${glm_SOURCE_DIR})
target_link_libraries(mcp_registry_gate_tests PRIVATE
    vc_build_options
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
    vc_navigation
    vc_fastwfc
    vc_meshoptimizer
    vc_xatlas
)
if(TARGET vc_generate_world_save)
    add_dependencies(mcp_registry_gate_tests vc_generate_world_save)
endif()
add_test(NAME mcp_registry_gate_tests COMMAND mcp_registry_gate_tests)
endif()

add_executable(VulkanEngineCooker
    src/tools/main_cooker.cpp
    src/tools/AssetCooker.cpp
    $<TARGET_OBJECTS:vc_core_runtime>
)
target_link_libraries(VulkanEngineCooker PRIVATE vc_build_options)

add_executable(VulkanShaderCompiler src/tools/main_shader_compiler.cpp src/tools/BuildTools.cpp)
target_link_libraries(VulkanShaderCompiler PRIVATE vc_build_options)
add_executable(VulkanProjectGenerator src/tools/main_project_generator.cpp src/tools/BuildTools.cpp)
target_link_libraries(VulkanProjectGenerator PRIVATE vc_build_options)
add_executable(VulkanPackageBuilder src/tools/main_package_builder.cpp src/tools/BuildTools.cpp)
target_link_libraries(VulkanPackageBuilder PRIVATE vc_build_options)

# The built tool/game executables, staged when present (a partial build skips
# the ones it did not produce). The documented flow is Release; the engine's
# verification commands all use --config Release.
foreach(vc_exe VulkanEngineGame VulkanEngineServer VulkanEngineCooker
        VulkanShaderCompiler VulkanProjectGenerator VulkanPackageBuilder
        VulkanEngineEditor)
    install(FILES "${CMAKE_BINARY_DIR}/Release/${vc_exe}.exe" DESTINATION bin OPTIONAL)
endforeach()
install(DIRECTORY assets/ DESTINATION assets OPTIONAL)

# FALTANTES item 11 / §24 — redistributable SDK install: only the public
# headers, the MCP server, the built tools/binaries, and (Windows) the runtime
# DLLs. No src/, no build/, no workspace paths. The staged tree is verified by
# tools/portability/clean-machine-gate.mjs.
install(DIRECTORY src/engine/public/ DESTINATION include FILES_MATCHING PATTERN "*.hpp")
install(DIRECTORY tools/mcp-server DESTINATION tools
    FILES_MATCHING PATTERN "*.mjs" PATTERN "package.json" PATTERN "README.md")
install(FILES SDK.md DESTINATION .)
if(WIN32)
    install(DIRECTORY "${CMAKE_BINARY_DIR}/Release/" DESTINATION bin OPTIONAL
        FILES_MATCHING PATTERN "*.dll")
endif()

# FALTANTES item 11 / §24 — consumable SDK archive: ONE static library packing
# the public registries + procgen (vc_sdk_public), the public gameplay runtime
# (vc_gameplay_sdk), the voxel runtime/generation and the network/sound
# adapters — the exact object set voxel_sdk_tests links. External projects
# consume ONLY this archive + the installed public headers + glm; the
# relocatable package config below resolves every dependency relative to the
# install prefix (verified by tools/portability/external-consumer-gate.mjs).
# The object modules already compiled with vc_build_options; only the three
# direct TUs compile here (same flags via vc_build_options).
add_library(vc_sdk STATIC
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/networking/NetworkRuntime.cpp
)
target_link_libraries(vc_sdk PRIVATE vc_build_options)
if(TARGET vc_generate_world_save)
    add_dependencies(vc_sdk vc_generate_world_save)
endif()

# The engine's jolt target shares its OUTPUT_NAME with the Jolt target created
# by Jolt.cmake; pin it so the installed archive name is deterministic on every
# filesystem (the build already writes Jolt.lib via case-insensitive collision).
set_target_properties(jolt PROPERTIES OUTPUT_NAME Jolt)

# The vc_sdk archive links vc_sdk_public, which includes the storage TUs
# (zstd/blake3/rocksdb/flatbuffers) unconditionally — it cannot be built when
# those optional deps are OFF (e.g. the clean-machine gate's reduced configure).
# Install it OPTIONAL so partial builds stage everything else and skip this lib
# silently; the workspace build always produces it.
install(FILES "$<TARGET_FILE:vc_sdk>" DESTINATION lib OPTIONAL)
foreach(vc_lib vc_zstd vc_blake3 vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas)
    if(TARGET ${vc_lib})
        # $<TARGET_FILE:...> resolves the real archive path (subdirectory
        # builds like rocksdb/flatbuffers land outside the top-level Release
        # dir); OPTIONAL skips silently when a partial build defined the
        # target but did not build its archive (clean-machine gate, deps OFF).
        install(FILES "$<TARGET_FILE:${vc_lib}>" DESTINATION lib OPTIONAL)
    endif()
endforeach()
foreach(vc_lib flatbuffers rocksdb jolt bullet)
    if(TARGET ${vc_lib})
        install(FILES "$<TARGET_FILE:${vc_lib}>" DESTINATION lib OPTIONAL)
    endif()
endforeach()
# glm headers: the public contracts include <glm/...>, so the consumer needs
# the same header-only library voxel_sdk_tests compiles against.
install(DIRECTORY ${glm_SOURCE_DIR}/glm DESTINATION include)

# Relocatable package config: every path resolves from this file's location, so
# the staged prefix can move or be consumed from another checkout/machine.
set(VULKAN_CRAFT_SDK_LINK_LIBS "")
foreach(vc_lib vc_zstd vc_blake3 vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas flatbuffers rocksdb jolt bullet)
    if(TARGET ${vc_lib})
        get_target_property(_vc_on ${vc_lib} OUTPUT_NAME)
        if(NOT _vc_on)
            set(_vc_on ${vc_lib})
        endif()
        list(APPEND VULKAN_CRAFT_SDK_LINK_LIBS "${_vc_on}.lib")
    endif()
endforeach()
string(JOIN ";" VULKAN_CRAFT_SDK_LINK_LIBS_CMAKE ${VULKAN_CRAFT_SDK_LINK_LIBS})
configure_file(cmake/vulkan_craft_sdk-config.cmake.in
    ${CMAKE_BINARY_DIR}/vulkan_craft_sdk-config.cmake @ONLY)
install(FILES ${CMAKE_BINARY_DIR}/vulkan_craft_sdk-config.cmake
    DESTINATION lib/cmake/vulkan_craft_sdk)

add_executable(far_terrain_sampling_benchmark
    tests/FarTerrainSamplingBenchmark.cpp
    src/simulation/voxel/generation/TerrainGenerator.cpp
)
target_link_libraries(far_terrain_sampling_benchmark PRIVATE vc_build_options)
add_test(NAME far_terrain_sampling_benchmark COMMAND far_terrain_sampling_benchmark)
set_tests_properties(far_terrain_sampling_benchmark PROPERTIES LABELS "benchmark;far-terrain")
