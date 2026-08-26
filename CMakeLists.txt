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

# ozz-animation (Guillaume Blanc, MIT) — skeleton/clip runtime, sampling,
# blending and skinning authority (FALTANTES §18 item 1, promoted 2026-08-17).
# Sources compiled directly from the pinned clone (base + animation/runtime +
# animation/offline builders); the upstream CMake is not used. The public
# contract engine/animation/* never includes ozz; only
# src/engine/sdk/MotionDatabase.cpp does (adapter rule).
set(VC_OZZ_BASE_SOURCES
    ${VC_SOLUTIONS_DIR}/ozz-animation/src/base/memory/allocator.cc
    ${VC_SOLUTIONS_DIR}/ozz-animation/src/base/platform.cc
    ${VC_SOLUTIONS_DIR}/ozz-animation/src/base/log.cc
    ${VC_SOLUTIONS_DIR}/ozz-animation/src/base/containers/string_archive.cc
    ${VC_SOLUTIONS_DIR}/ozz-animation/src/base/encode/group_varint.cc
    ${VC_SOLUTIONS_DIR}/ozz-animation/src/base/io/archive.cc
    ${VC_SOLUTIONS_DIR}/ozz-animation/src/base/io/stream.cc
    ${VC_SOLUTIONS_DIR}/ozz-animation/src/base/maths/box.cc
    ${VC_SOLUTIONS_DIR}/ozz-animation/src/base/maths/math_archive.cc
    ${VC_SOLUTIONS_DIR}/ozz-animation/src/base/maths/simd_math.cc
    ${VC_SOLUTIONS_DIR}/ozz-animation/src/base/maths/simd_math_archive.cc
    ${VC_SOLUTIONS_DIR}/ozz-animation/src/base/maths/soa_math_archive.cc
)
set(VC_OZZ_RUNTIME_SOURCES
    ${VC_SOLUTIONS_DIR}/ozz-animation/src/animation/runtime/animation.cc
    ${VC_SOLUTIONS_DIR}/ozz-animation/src/animation/runtime/animation_utils.cc
    ${VC_SOLUTIONS_DIR}/ozz-animation/src/animation/runtime/blending_job.cc
    ${VC_SOLUTIONS_DIR}/ozz-animation/src/animation/runtime/ik_aim_job.cc
    ${VC_SOLUTIONS_DIR}/ozz-animation/src/animation/runtime/ik_two_bone_job.cc
    ${VC_SOLUTIONS_DIR}/ozz-animation/src/animation/runtime/local_to_model_job.cc
    ${VC_SOLUTIONS_DIR}/ozz-animation/src/animation/runtime/motion_blending_job.cc
    ${VC_SOLUTIONS_DIR}/ozz-animation/src/animation/runtime/sampling_job.cc
    ${VC_SOLUTIONS_DIR}/ozz-animation/src/animation/runtime/skeleton.cc
    ${VC_SOLUTIONS_DIR}/ozz-animation/src/animation/runtime/skeleton_utils.cc
    ${VC_SOLUTIONS_DIR}/ozz-animation/src/animation/runtime/track.cc
    ${VC_SOLUTIONS_DIR}/ozz-animation/src/animation/runtime/track_sampling_job.cc
    ${VC_SOLUTIONS_DIR}/ozz-animation/src/animation/runtime/track_triggering_job.cc
)
set(VC_OZZ_OFFLINE_SOURCES
    ${VC_SOLUTIONS_DIR}/ozz-animation/src/animation/offline/raw_animation.cc
    ${VC_SOLUTIONS_DIR}/ozz-animation/src/animation/offline/raw_animation_archive.cc
    ${VC_SOLUTIONS_DIR}/ozz-animation/src/animation/offline/raw_animation_utils.cc
    ${VC_SOLUTIONS_DIR}/ozz-animation/src/animation/offline/animation_builder.cc
    ${VC_SOLUTIONS_DIR}/ozz-animation/src/animation/offline/raw_skeleton.cc
    ${VC_SOLUTIONS_DIR}/ozz-animation/src/animation/offline/raw_skeleton_archive.cc
    ${VC_SOLUTIONS_DIR}/ozz-animation/src/animation/offline/skeleton_builder.cc
    ${VC_SOLUTIONS_DIR}/ozz-animation/src/animation/offline/track_builder.cc
    ${VC_SOLUTIONS_DIR}/ozz-animation/src/animation/offline/track_optimizer.cc
    ${VC_SOLUTIONS_DIR}/ozz-animation/src/animation/offline/raw_track.cc
    ${VC_SOLUTIONS_DIR}/ozz-animation/src/animation/offline/raw_track_utils.cc
)
add_library(vc_ozz STATIC
    ${VC_OZZ_BASE_SOURCES}
    ${VC_OZZ_RUNTIME_SOURCES}
    ${VC_OZZ_OFFLINE_SOURCES}
)
target_include_directories(vc_ozz PUBLIC
    ${VC_SOLUTIONS_DIR}/ozz-animation/include
)
target_include_directories(vc_ozz PRIVATE ${VC_SOLUTIONS_DIR}/ozz-animation/src)

# Animation Compression Library (Nicholas Frechette, MIT) — clip compression
# and decompression (FALTANTES §18 item 1, promoted 2026-08-17). Header-only
# (the .impl.h files are included by the public headers); the upstream CMake
# is not used. RTM (Realtime Math, Nicholas Frechette, MIT) is its math layer
# and is vendored at the exact pin ACL 2.1.99 requires (v2.2.0 — see
# SNAPSHOT.md). Only src/engine/sdk/MotionDatabase.cpp includes ACL (adapter
# rule).
add_library(vc_acl INTERFACE)
target_include_directories(vc_acl INTERFACE
    ${VC_SOLUTIONS_DIR}/acl/includes
    ${VC_SOLUTIONS_DIR}/acl/external/rtm/includes
)

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
    src/engine/sdk/MultiScaleStreaming.cpp
    src/engine/sdk/XpbdDeformable.cpp
    src/engine/sdk/TetraMeshCooking.cpp
    src/engine/sdk/MobBehavior.cpp
    src/engine/sdk/WorldManager.cpp
    src/engine/sdk/WorldReplication.cpp
    src/engine/sdk/OriginRebase.cpp
    src/engine/sdk/LocalSpace.cpp
    src/engine/sdk/TimeTravel.cpp
    src/engine/sdk/VehicleAsset.cpp
    src/engine/sdk/BeamGraphAsset.cpp
    src/engine/sdk/VehicleReplication.cpp
    src/engine/sdk/VehicleProvider.cpp
    src/engine/sdk/AbilitySystem.cpp
    src/engine/sdk/MotionDatabase.cpp
    src/engine/sdk/GaitPlanner.cpp
    src/engine/sdk/FootPlacement.cpp
    src/engine/sdk/PoseWarp.cpp
    src/engine/sdk/ProceduralLegs.cpp
    src/engine/sdk/MotionMatcher.cpp
    src/engine/sdk/OfflineFarm.cpp
    src/engine/sdk/AnimationLod.cpp
    src/engine/sdk/AiGraphValidation.cpp
    src/engine/sdk/StrandHair.cpp
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
    ${VC_SOLUTIONS_DIR}/ozz-animation/include
    ${VC_SOLUTIONS_DIR}/acl/includes
    ${VC_SOLUTIONS_DIR}/acl/external/rtm/includes
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
    src/engine/physics/SkeletonPoseMapper.cpp
    src/engine/physics/PhysicsStreamingBridge.cpp
    src/engine/physics/VoxelConnectivity.cpp
    src/engine/physics/VoxelBoxMerger.cpp
    src/engine/gameplay/WeaponSystem.cpp
    src/engine/gameplay/VehicleRuntime.cpp
    src/engine/gameplay/BeamChassisRuntime.cpp
    src/engine/gameplay/DestructionRuntime.cpp
    src/engine/gameplay/FractureConfig.cpp
    src/engine/gameplay/ExplosionRuntime.cpp
    src/engine/gameplay/DebrisRuntime.cpp
    src/engine/gameplay/DestructionHistoryRuntime.cpp
    src/engine/gameplay/DestructionBudget.cpp
    src/engine/gameplay/DestructionTelemetry.cpp
    src/engine/core/uuid/UUID.cpp
)
# RegistryJson stays owned by vc_sdk_public (the canonical copy every consumer
# links); the gameplay TUs reference its json_* helpers, resolved at link time.
target_include_directories(vc_gameplay_sdk PRIVATE src)
vc_object_module(vc_advanced_runtime
    src/engine/physics/PhysicsRuntime.cpp
    src/engine/physics/PhysicsAdvanced.cpp
    src/engine/physics/Ragdoll.cpp
    src/engine/physics/SkeletonPoseMapper.cpp
    src/engine/physics/PhysicsStreamingBridge.cpp
    src/engine/physics/VoxelConnectivity.cpp
    src/engine/physics/VoxelBoxMerger.cpp
    src/engine/physics/PhysicsBackend.cpp
    src/engine/physics/JoltPhysicsBackend.cpp
    src/engine/physics/BulletPhysicsBackend.cpp
    src/engine/gameplay/GameplayVisual.cpp
    src/engine/gameplay/WeaponSystem.cpp
    src/engine/gameplay/VehicleRuntime.cpp
    src/engine/gameplay/BeamChassisRuntime.cpp
    src/engine/gameplay/DestructionRuntime.cpp
    src/engine/gameplay/FractureConfig.cpp
    src/engine/gameplay/ExplosionRuntime.cpp
    src/engine/gameplay/DebrisRuntime.cpp
    src/engine/gameplay/DestructionHistoryRuntime.cpp
    src/engine/gameplay/DestructionBudget.cpp
    src/engine/gameplay/DestructionTelemetry.cpp
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
    editor_grid.vert editor_grid.frag editor_splat.vert editor_splat.frag
    editor_envsphere.vert editor_envsphere.frag block.vert block.frag
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
    # RegistryJson lives in vc_sdk_public; these app targets link vc_advanced_runtime
    # (which compiles FractureConfig using the json helpers) without sdk_public, so
    # they compile the sdk TU directly — same pattern as vulkan_craft below.
    # BeamChassisRuntime (vc_gameplay_sdk) references the public beam-chassis and
    # XPBD solver contracts, so those two sdk TUs are compiled here too.
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/BeamGraphAsset.cpp
    src/engine/sdk/VehicleProvider.cpp
    src/engine/sdk/MotionDatabase.cpp
    src/engine/sdk/XpbdDeformable.cpp
    $<TARGET_OBJECTS:vc_core_runtime>
    $<TARGET_OBJECTS:vc_advanced_runtime>
)
target_compile_features(VulkanEngineGame PRIVATE cxx_std_20)
target_include_directories(VulkanEngineGame PRIVATE src ${glm_SOURCE_DIR})
target_link_libraries(VulkanEngineGame PRIVATE vc_build_options glfw glm vk-bootstrap Vulkan::Vulkan vc_material_pipeline vc_ozz vc_acl)

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
    # BeamChassisRuntime (vc_gameplay_sdk) references the public beam-chassis
    # and XPBD solver contracts; compile those sdk TUs directly like the rest.
    src/engine/sdk/BeamGraphAsset.cpp
    src/engine/sdk/VehicleProvider.cpp
    src/engine/sdk/MotionDatabase.cpp
    src/engine/sdk/XpbdDeformable.cpp
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
    vc_build_options glfw glm vk-bootstrap Vulkan::Vulkan windowscodecs ole32 vc_ozz vc_acl
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
    # Loopback HTTP control API (curl 127.0.0.1:8321/{play,pause,resume,stop,step,state})
    src/editor/EditorControlApi.cpp
    src/engine/editor/ui/EditorGUI.cpp
    src/editor/tools/SpecializedEditorsPanel.cpp
    # Wicked-port tool panels (frontend; PORTS.md): windows wired to the
    # VulkanCraft scene/components, with TODO(frontend-port) markers where the
    # runtime feature still needs implementing.
    src/editor/tools/WickedToolsPanel.cpp
    # Forge design system (light theme + reusable editor widgets).
    src/editor/frontend/ForgeTheme.cpp
    src/editor/frontend/ForgeWidgets.cpp
    # Public navigation provider (FALTANTES item 12): the editor's play nav
    # bakes via Recast + Detour — the legacy grid track was removed.
    src/engine/sdk/RecastNavigation.cpp
    # RegistryJson lives in vc_sdk_public; this target links vc_advanced_runtime
    # (which compiles FractureConfig using the json helpers) without sdk_public,
    # so it compiles the sdk TU directly — same pattern as vulkan_craft below.
    # BeamChassisRuntime (vc_gameplay_sdk) references the public beam-chassis and
    # XPBD solver contracts, so those two sdk TUs are compiled here too.
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/BeamGraphAsset.cpp
    src/engine/sdk/VehicleProvider.cpp
    src/engine/sdk/MotionDatabase.cpp
    src/engine/sdk/XpbdDeformable.cpp
    $<$<BOOL:${VC_ENABLE_VOXEL_PLUGIN}>:src/engine/audio/SoundEngine.cpp>
    $<$<BOOL:${VC_ENABLE_VOXEL_PLUGIN}>:src/plugins/voxel/VoxelPlugin.cpp>
    # Frontend port (Wicked Editor, MIT): the UI base font (Liberation Sans) is
    # embedded zstd-compressed; the adapter TU decompresses it through the
    # public ICompressionProvider — same pattern as the sdk TUs above.
    $<$<BOOL:${VC_ENABLE_ZSTD}>:src/engine/core/compression/ZstdCompression.cpp>
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
    # Frontend port (Wicked GUI, MIT): base layer of the ported wiGUI module.
    src/editor/frontend/wicked
    ${VC_SOLUTIONS_DIR}/recast-navigation/Recast/Include
    ${VC_SOLUTIONS_DIR}/recast-navigation/Detour/Include
)
target_link_libraries(VulkanEngineEditor PRIVATE
    vc_build_options glfw glm vk-bootstrap Vulkan::Vulkan windowscodecs ole32 vc_material_pipeline vc_navigation vc_ozz vc_acl
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
)
add_dependencies(VulkanEngineEditor compile_shaders)

add_executable(VulkanEngineServer
    src/server/main_server.cpp
    $<TARGET_OBJECTS:vc_core_runtime>
    $<TARGET_OBJECTS:vc_advanced_runtime>
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
)
target_compile_features(VulkanEngineServer PRIVATE cxx_std_20)
target_include_directories(VulkanEngineServer PRIVATE
    src
    src/engine/public
    ${glm_SOURCE_DIR}
)
target_link_libraries(VulkanEngineServer PRIVATE
    vc_build_options
    vc_ozz vc_acl
    glm jolt bullet
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
    vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
)
if(TARGET vc_generate_world_save)
    add_dependencies(VulkanEngineServer vc_generate_world_save)
endif()

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

add_executable(voxel_box_merger_tests
    tests/VoxelBoxMergerTests.cpp
    src/engine/physics/VoxelBoxMerger.cpp
)
target_link_libraries(voxel_box_merger_tests PRIVATE vc_build_options glm)
add_test(NAME voxel_box_merger_tests COMMAND voxel_box_merger_tests)

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
    src/engine/physics/SkeletonPoseMapper.cpp
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
    src/engine/sdk/MotionDatabase.cpp
)
target_link_libraries(empty_project_tests PRIVATE vc_build_options glm vc_ozz vc_acl)
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
    src/engine/sdk/MotionDatabase.cpp
)
target_link_libraries(engine_foundation_tests PRIVATE vc_build_options vc_ozz vc_acl)
add_test(NAME engine_foundation_tests COMMAND engine_foundation_tests)

# Runs the same physics scenarios against every backend (builtin, jolt, bullet).
add_executable(physics_backend_tests
    tests/PhysicsBackendTests.cpp
    src/engine/physics/PhysicsRuntime.cpp
    src/engine/physics/PhysicsBackend.cpp
    src/engine/physics/JoltPhysicsBackend.cpp
    src/engine/physics/BulletPhysicsBackend.cpp
    src/engine/physics/Ragdoll.cpp
    src/engine/physics/SkeletonPoseMapper.cpp
)
target_compile_features(physics_backend_tests PRIVATE cxx_std_20)
target_include_directories(physics_backend_tests PRIVATE src)
target_link_libraries(physics_backend_tests PRIVATE vc_build_options glm)
add_test(NAME physics_backend_tests COMMAND physics_backend_tests)

# Jolt SkeletonMapper adapter (FALTANTES item 2): maps poses between the
# animation skeleton and the ragdoll bodies through Jolt's SkeletonMapper.
add_executable(skeleton_mapper_tests
    tests/SkeletonMapperTests.cpp
    src/engine/physics/PhysicsRuntime.cpp
    src/engine/physics/PhysicsBackend.cpp
    src/engine/physics/JoltPhysicsBackend.cpp
    src/engine/physics/BulletPhysicsBackend.cpp
    src/engine/physics/Ragdoll.cpp
    src/engine/physics/SkeletonPoseMapper.cpp
    src/engine/core/uuid/UUID.cpp
)
target_compile_features(skeleton_mapper_tests PRIVATE cxx_std_20)
target_include_directories(skeleton_mapper_tests PRIVATE src)
target_link_libraries(skeleton_mapper_tests PRIVATE vc_build_options glm)
add_test(NAME skeleton_mapper_tests COMMAND skeleton_mapper_tests)

# FALTANTES §18 item 8: active/partial ragdoll with recovery driven by the
# Jolt swing-twist constraint motors (built on the §16 item 2 Ragdoll +
# SkeletonPoseMapper). Needs the physics stack, so it follows the
# skeleton_mapper_tests pattern (physics TUs compiled directly, Jolt backend).
add_executable(active_ragdoll_tests
    tests/ActiveRagdollTests.cpp
    src/engine/physics/PhysicsRuntime.cpp
    src/engine/physics/PhysicsBackend.cpp
    src/engine/physics/JoltPhysicsBackend.cpp
    src/engine/physics/BulletPhysicsBackend.cpp
    src/engine/physics/Ragdoll.cpp
    src/engine/physics/SkeletonPoseMapper.cpp
    src/engine/physics/ActiveRagdoll.cpp
    src/engine/core/uuid/UUID.cpp
)
target_compile_features(active_ragdoll_tests PRIVATE cxx_std_20)
target_include_directories(active_ragdoll_tests PRIVATE src)
target_link_libraries(active_ragdoll_tests PRIVATE vc_build_options glm)
add_test(NAME active_ragdoll_tests COMMAND active_ragdoll_tests)

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
    src/engine/sdk/MotionDatabase.cpp
)
target_include_directories(advanced_systems_tests PRIVATE
    ${VC_SOLUTIONS_DIR}/recast-navigation/Recast/Include
    ${VC_SOLUTIONS_DIR}/recast-navigation/Detour/Include
)
target_link_libraries(advanced_systems_tests PRIVATE vc_build_options glm vc_navigation vc_ozz vc_acl)
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
    src/engine/gameplay/BeamChassisRuntime.cpp
    src/engine/gameplay/DestructionRuntime.cpp
    src/engine/audio/AudioRuntime.cpp
    src/engine/sdk/RecastNavigation.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/BeamGraphAsset.cpp
    src/engine/sdk/VehicleProvider.cpp
    src/engine/sdk/MotionDatabase.cpp
    src/engine/sdk/XpbdDeformable.cpp
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
target_link_libraries(gameplay_framework_tests PRIVATE vc_build_options glm vc_navigation vc_ozz vc_acl)
add_test(NAME gameplay_framework_tests COMMAND gameplay_framework_tests)

# Public gameplay runtime (FALTANTES item 9 / META section 20): the contract
# compiles only with public SDK headers; the equivalence gates drive the same
# scenarios through the INTERNAL runtimes (the internal layers never leak into
# the public surface).
add_executable(gameplay_runtime_tests
    tests/GameplayRuntimeTests.cpp
    # vc_gameplay_sdk compiles FractureConfig (json helpers); RegistryJson and
    # VehicleAsset live in vc_sdk_public, so this light target compiles the
    # sdk TUs directly (VehicleAsset.cpp: create_vehicle_from_asset validates
    # through the public asset contract). BeamChassisRuntime also references
    # the XPBD solver factory, compiled here as well.
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/VehicleAsset.cpp
    src/engine/sdk/BeamGraphAsset.cpp
    src/engine/sdk/VehicleProvider.cpp
    src/engine/sdk/MotionDatabase.cpp
    src/engine/sdk/XpbdDeformable.cpp
    $<TARGET_OBJECTS:vc_gameplay_sdk>
)
target_include_directories(gameplay_runtime_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(gameplay_runtime_tests PRIVATE vc_build_options glm vc_ozz vc_acl)
add_test(NAME gameplay_runtime_tests COMMAND gameplay_runtime_tests)

# FALTANTES item 19: generic abilities/powers as data-driven assets (attributes,
# tags, costs, cooldowns, conditions; targeting + composable effects; voxel /
# physics / particles / audio / animation integration through the public world
# seam; cancel/interrupt/periodic; persistence + authority + prediction; event
# hooks). The contract compiles with public SDK headers only; the gate drives a
# deterministic mock IAbilityWorld (no engine internal is touched — the "without
# core code" proof).
add_executable(ability_system_tests
    tests/AbilitySystemTests.cpp
    # AbilitySystem.cpp uses the shared RegistryJson helpers; both live in
    # vc_sdk_public. This light target compiles the TUs directly (the
    # gameplay_runtime_tests pattern) so the gate needs no other SDK module.
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/AbilitySystem.cpp
)
target_include_directories(ability_system_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(ability_system_tests PRIVATE vc_build_options glm)
add_test(NAME ability_system_tests COMMAND ability_system_tests)

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
    src/engine/sdk/MotionDatabase.cpp
    third_party/tinyexr.cc
    third_party/miniz.c
    third_party/ufbx.c
    src/plugins/PluginContract.cpp
    src/plugins/PluginEditorExtensions.cpp
    src/plugins/voxel/VoxelPlugin.cpp
    src/plugins/voxel/VoxelStructure.cpp
    src/simulation/voxel/tools/VoxelTools.cpp
)
target_link_libraries(voxel_tools_tests PRIVATE vc_build_options glm vc_ozz vc_acl)
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
    vc_ozz vc_acl
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

# Physics x streaming x server authority (FALTANTES §16 item 6): the bridge
# owns the physics bodies mirroring the streaming world — loaded chunks get
# static terrain slabs, dynamic bodies rest and sleep on them, wake_region
# wakes them, and unloaded chunks despawn their contained bodies (no orphans).
add_executable(physics_streaming_tests
    tests/PhysicsStreamingTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/networking/NetworkRuntime.cpp
)
target_compile_features(physics_streaming_tests PRIVATE cxx_std_20)
target_include_directories(physics_streaming_tests PRIVATE
    src/engine/public
    src
    ${glm_SOURCE_DIR}
)
target_link_libraries(physics_streaming_tests PRIVATE
    vc_build_options
    vc_ozz vc_acl
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
    vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
)
if(TARGET vc_generate_world_save)
    add_dependencies(physics_streaming_tests vc_generate_world_save)
endif()
add_test(NAME physics_streaming_tests COMMAND physics_streaming_tests)

# Incremental voxel connectivity (FALTANTES §16 item 9): only voxel islands
# that LOST their connection to the anchored mass become dynamic Jolt bodies.
# VoxelConnectivity does the bounded flood-fill island detection over the
# PUBLIC world surface; the bridge turns each detached island into a dynamic
# body (mass from block density) that falls and rests on the streamed terrain.
add_executable(connectivity_tests
    tests/ConnectivityTests.cpp
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/networking/NetworkRuntime.cpp
)
target_compile_features(connectivity_tests PRIVATE cxx_std_20)
target_include_directories(connectivity_tests PRIVATE
    src/engine/public
    src
    ${glm_SOURCE_DIR}
)
target_link_libraries(connectivity_tests PRIVATE
    vc_build_options
    vc_ozz vc_acl
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
    vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
)
if(TARGET vc_generate_world_save)
    add_dependencies(connectivity_tests vc_generate_world_save)
endif()
add_test(NAME connectivity_tests COMMAND connectivity_tests)

# Integrated explosions (FALTANTES §16 item 10): the ExplosionRuntime combines
# materials (blast resistance + flammability from the registry JSON), heat
# (fireball burns flammables), pressure (falloff drives the carve), impulse
# (dynamic bodies pushed by the wave) and terrain (the affected voxel box
# feeds the connectivity layer — detached islands become dynamic bodies).
add_executable(explosion_tests
    tests/ExplosionTests.cpp
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/networking/NetworkRuntime.cpp
)
target_compile_features(explosion_tests PRIVATE cxx_std_20)
target_include_directories(explosion_tests PRIVATE
    src/engine/public
    src
    ${glm_SOURCE_DIR}
)
target_link_libraries(explosion_tests PRIVATE
    vc_build_options
    vc_ozz vc_acl
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
    vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
)
if(TARGET vc_generate_world_save)
    add_dependencies(explosion_tests vc_generate_world_save)
endif()
add_test(NAME explosion_tests COMMAND explosion_tests)

# Production debris (FALTANTES §16 item 11): DebrisRuntime owns the debris
# lifecycle — pooling (parked bodies reused, not recreated), LOD (cull beyond
# the focus radius, re-enter on return), selective persistence (only resting,
# in-radius, above-min-mass debris snapshot/restore) and replication (drained
# lifecycle events).
add_executable(debris_tests
    tests/DebrisTests.cpp
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/networking/NetworkRuntime.cpp
)
target_compile_features(debris_tests PRIVATE cxx_std_20)
target_include_directories(debris_tests PRIVATE
    src/engine/public
    src
    ${glm_SOURCE_DIR}
)
target_link_libraries(debris_tests PRIVATE
    vc_build_options
    vc_ozz vc_acl
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
    vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
)
if(TARGET vc_generate_world_save)
    add_dependencies(debris_tests vc_generate_world_save)
endif()
add_test(NAME debris_tests COMMAND debris_tests)

# Revoxelize sleeping debris (FALTANTES §16 item 12): debris that fell and
# SLEEPS is written back into the voxel world as terrain per the asset policy
# (settle delay, material filter, footprint caps), then despawned into the
# pool — the destruction pipeline converges back to solid terrain.
add_executable(revoxelize_tests
    tests/RevoxelizeTests.cpp
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/networking/NetworkRuntime.cpp
)
target_compile_features(revoxelize_tests PRIVATE cxx_std_20)
target_include_directories(revoxelize_tests PRIVATE
    src/engine/public
    src
    ${glm_SOURCE_DIR}
)
target_link_libraries(revoxelize_tests PRIVATE
    vc_build_options
    vc_ozz vc_acl
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
    vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
)
if(TARGET vc_generate_world_save)
    add_dependencies(revoxelize_tests vc_generate_world_save)
endif()
add_test(NAME revoxelize_tests COMMAND revoxelize_tests)

# Destruction history (FALTANTES §16 item 13): ONE generic capture/restore
# mechanism reconstructs a region's destruction state (voxel cells + debris)
# regardless of the damage source — explosion carve, fracture detach, manual
# carve, or debris revoxelization. No per-source special cases.
add_executable(history_tests
    tests/HistoryTests.cpp
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/networking/NetworkRuntime.cpp
)
target_compile_features(history_tests PRIVATE cxx_std_20)
target_include_directories(history_tests PRIVATE
    src/engine/public
    src
    ${glm_SOURCE_DIR}
)
target_link_libraries(history_tests PRIVATE
    vc_build_options
    vc_ozz vc_acl
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
    vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
)
if(TARGET vc_generate_world_save)
    add_dependencies(history_tests vc_generate_world_save)
endif()
add_test(NAME history_tests COMMAND history_tests)

# Destruction budgets + telemetry (FALTANTES §16 item 14): ONE data-driven
# budget bounds every pipeline stage per step (explosion carve/burn/impulse,
# connectivity island promotion, debris spawns, revoxelization, history
# restore) and telemetry makes the whole destruction step observable — done
# work AND budget spills, step-scoped with cumulative totals.
add_executable(destruction_tests
    tests/DestructionTests.cpp
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/networking/NetworkRuntime.cpp
)
target_compile_features(destruction_tests PRIVATE cxx_std_20)
target_include_directories(destruction_tests PRIVATE
    src/engine/public
    src
    ${glm_SOURCE_DIR}
)
target_link_libraries(destruction_tests PRIVATE
    vc_build_options
    vc_ozz vc_acl
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
    vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
)
if(TARGET vc_generate_world_save)
    add_dependencies(destruction_tests vc_generate_world_save)
endif()
add_test(NAME destruction_tests COMMAND destruction_tests)

# Deformable providers (FALTANTES §16 item 3): the public IDeformableProvider
# contract with XPBD (position-based dynamics) as the implemented specialized
# plugin and FEMFX as the opt-in seam (refused, never silently falling back).
# Self-contained, deterministic, headless.
add_executable(deformable_tests
    tests/DeformableTests.cpp
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/networking/NetworkRuntime.cpp
)
target_compile_features(deformable_tests PRIVATE cxx_std_20)
target_include_directories(deformable_tests PRIVATE
    src/engine/public
    src
    ${glm_SOURCE_DIR}
)
target_link_libraries(deformable_tests PRIVATE
    vc_build_options
    vc_ozz vc_acl
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
    vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
)
if(TARGET vc_generate_world_save)
    add_dependencies(deformable_tests vc_generate_world_save)
endif()
add_test(NAME deformable_tests COMMAND deformable_tests)

# Tetra mesh cooking (FALTANTES §16 items 4-5): cooks tetrahedral SIMULATION
# meshes keeping collider/render/simulation separate (deterministic, headless;
# fTetWild stays a specialized offline cooker) and documents per-provider
# determinism guarantees (docs/DETERMINISMO_PROVIDERS.md).
add_executable(tetra_cooking_tests
    tests/TetraCookingTests.cpp
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/networking/NetworkRuntime.cpp
)
target_compile_features(tetra_cooking_tests PRIVATE cxx_std_20)
target_include_directories(tetra_cooking_tests PRIVATE
    src/engine/public
    src
    ${glm_SOURCE_DIR}
)
target_link_libraries(tetra_cooking_tests PRIVATE
    vc_build_options
    vc_ozz vc_acl
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
    vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
)
if(TARGET vc_generate_world_save)
    add_dependencies(tetra_cooking_tests vc_generate_world_save)
endif()
add_test(NAME tetra_cooking_tests COMMAND tetra_cooking_tests)

# World-scoped replication (META §19 / FALTANTES "replicação e interesse por
# mundo"): per-world interest, world-routed edits, portal crossings as
# interest transitions, per-world client binding.
add_executable(world_replication_tests
    tests/WorldReplicationTests.cpp
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/networking/NetworkRuntime.cpp
)
target_compile_features(world_replication_tests PRIVATE cxx_std_20)
target_include_directories(world_replication_tests PRIVATE
    src/engine/public
    src
    ${glm_SOURCE_DIR}
)
target_link_libraries(world_replication_tests PRIVATE
    vc_build_options
    vc_ozz vc_acl
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
    vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
)
if(TARGET vc_generate_world_save)
    add_dependencies(world_replication_tests vc_generate_world_save)
endif()
add_test(NAME world_replication_tests COMMAND world_replication_tests)

# Persistent references between worlds (META §19 / FALTANTES "referências
# persistentes entre mundos"): stable entity ids that survive save/load +
# cross-world references stored as a reserved component on the source entity.
add_executable(world_ref_tests
    tests/WorldRefTests.cpp
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/networking/NetworkRuntime.cpp
)
target_compile_features(world_ref_tests PRIVATE cxx_std_20)
target_include_directories(world_ref_tests PRIVATE
    src/engine/public
    src
    ${glm_SOURCE_DIR}
)
target_link_libraries(world_ref_tests PRIVATE
    vc_build_options
    vc_ozz vc_acl
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
    vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
)
if(TARGET vc_generate_world_save)
    add_dependencies(world_ref_tests vc_generate_world_save)
endif()
add_test(NAME world_ref_tests COMMAND world_ref_tests)

# Origin rebasing / high-precision coordinates (META §19 / FALTANTES §15
# "origin rebasing ou coordenadas de alta precisão"): the local frame origin
# (double) follows the gameplay focus so entity positions stay small and the
# stored float32 is exact at sub-block resolution; a rebase shifts the origin
# and translates every entity in every world, leaving absolute positions
# invariant.
add_executable(origin_rebase_tests
    tests/OriginRebaseTests.cpp
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/networking/NetworkRuntime.cpp
)
target_compile_features(origin_rebase_tests PRIVATE cxx_std_20)
target_include_directories(origin_rebase_tests PRIVATE
    src/engine/public
    src
    ${glm_SOURCE_DIR}
)
target_link_libraries(origin_rebase_tests PRIVATE
    vc_build_options
    vc_ozz vc_acl
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
    vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
)
if(TARGET vc_generate_world_save)
    add_dependencies(origin_rebase_tests vc_generate_world_save)
endif()
add_test(NAME origin_rebase_tests COMMAND origin_rebase_tests)

# Hierarchical local spaces (META §19 / FALTANTES §15 "espaços locais
# hierárquicos para planetas e veículos grandes"): a tree of local frames
# (planet -> vehicle -> seat) with rigid transforms (position + yaw, double);
# entities bind to a space via a reserved component (engine.space_ref), so a
# moving frame carries its occupants for free and the binding persists with
# the world save / replication.
add_executable(local_space_tests
    tests/LocalSpaceTests.cpp
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/networking/NetworkRuntime.cpp
)
target_compile_features(local_space_tests PRIVATE cxx_std_20)
target_include_directories(local_space_tests PRIVATE
    src/engine/public
    src
    ${glm_SOURCE_DIR}
)
target_link_libraries(local_space_tests PRIVATE
    vc_build_options
    vc_ozz vc_acl
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
    vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
)
if(TARGET vc_generate_world_save)
    add_dependencies(local_space_tests vc_generate_world_save)
endif()
add_test(NAME local_space_tests COMMAND local_space_tests)

# Persistent temporal states / branches for time travel (META §19 /
# FALTANTES §15 "branches/estados temporais persistentes para viagem no
# tempo"): full world snapshots (save v5) registered on a timeline, branches
# forked by copying snapshots, and travel_to rewinding the live world
# transactionally (rollback on failure).
add_executable(timeline_tests
    tests/TimelineTests.cpp
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/networking/NetworkRuntime.cpp
)
target_compile_features(timeline_tests PRIVATE cxx_std_20)
target_include_directories(timeline_tests PRIVATE
    src/engine/public
    src
    ${glm_SOURCE_DIR}
)
target_link_libraries(timeline_tests PRIVATE
    vc_build_options
    vc_ozz vc_acl
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
    vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
)
if(TARGET vc_generate_world_save)
    add_dependencies(timeline_tests vc_generate_world_save)
endif()
add_test(NAME timeline_tests COMMAND timeline_tests)

# Multi-scale streaming (META §19 / FALTANTES §15 vision "streaming
# multi-escala", Fase 5 checkbox): the world function streamed at several
# scales around a focus — fine voxel detail near, coarser height-fields far —
# all sampling the SAME generator, with power-of-two levels sharing anchors
# (cross-level coherent, lossless path-independent transition).
add_executable(multiscale_streaming_tests
    tests/MultiScaleStreamingTests.cpp
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/networking/NetworkRuntime.cpp
)
target_compile_features(multiscale_streaming_tests PRIVATE cxx_std_20)
target_include_directories(multiscale_streaming_tests PRIVATE
    src/engine/public
    src
    ${glm_SOURCE_DIR}
)
target_link_libraries(multiscale_streaming_tests PRIVATE
    vc_build_options
    vc_ozz vc_acl
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
    vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
)
if(TARGET vc_generate_world_save)
    add_dependencies(multiscale_streaming_tests vc_generate_world_save)
endif()
add_test(NAME multiscale_streaming_tests COMMAND multiscale_streaming_tests)

# Jolt Vehicles adapter (FALTANTES §17 item 1): replaces the hand-rolled
# raycast vehicle with a real Jolt VehicleConstraint on worlds with a vehicle
# solver; builtin/bullet keep the raycast fallback.
add_executable(vehicle_adapter_tests
    tests/VehicleAdapterTests.cpp
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/networking/NetworkRuntime.cpp
)
target_compile_features(vehicle_adapter_tests PRIVATE cxx_std_20)
target_include_directories(vehicle_adapter_tests PRIVATE
    src/engine/public
    src
    ${glm_SOURCE_DIR}
)
target_link_libraries(vehicle_adapter_tests PRIVATE
    vc_build_options
    vc_ozz vc_acl
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
    vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
)
if(TARGET vc_generate_world_save)
    add_dependencies(vehicle_adapter_tests vc_generate_world_save)
endif()
add_test(NAME vehicle_adapter_tests COMMAND vehicle_adapter_tests)

# VehicleAsset public contract (FALTANTES §17 item 2): the runtime is exposed
# as a data-driven asset (chassis/wheels/drivetrain JSON) with public,
# combinable component types; create_vehicle_from_asset assembles it on the
# physics world.
add_executable(vehicle_asset_tests
    tests/VehicleAssetTests.cpp
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/networking/NetworkRuntime.cpp
)
target_compile_features(vehicle_asset_tests PRIVATE cxx_std_20)
target_include_directories(vehicle_asset_tests PRIVATE
    src/engine/public
    src
    ${glm_SOURCE_DIR}
)
target_link_libraries(vehicle_asset_tests PRIVATE
    vc_build_options
    vc_ozz vc_acl
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
    vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
)
if(TARGET vc_generate_world_save)
    add_dependencies(vehicle_asset_tests vc_generate_world_save)
endif()
add_test(NAME vehicle_asset_tests COMMAND vehicle_asset_tests)

# Vehicle kinds + propulsion modules (FALTANTES §17 item 3): car / motorcycle /
# tank through the Jolt controllers (Wheeled / Motorcycle / Tracked) and the
# public wing/thruster/buoyancy propulsion modules.
add_executable(vehicle_kinds_tests
    tests/VehicleKindsTests.cpp
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/networking/NetworkRuntime.cpp
)
target_compile_features(vehicle_kinds_tests PRIVATE cxx_std_20)
target_include_directories(vehicle_kinds_tests PRIVATE
    src/engine/public
    src
    ${glm_SOURCE_DIR}
)
target_link_libraries(vehicle_kinds_tests PRIVATE
    vc_build_options
    vc_ozz vc_acl
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
    vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
)
if(TARGET vc_generate_world_save)
    add_dependencies(vehicle_kinds_tests vc_generate_world_save)
endif()
add_test(NAME vehicle_kinds_tests COMMAND vehicle_kinds_tests)

# Node/beam deformable chassis (FALTANTES §17 item 4): BeamGraphAsset + XPBD
# (the public IDeformableProvider solver) + wheel mounts on nodes.
add_executable(beam_vehicle_tests
    tests/BeamVehicleTests.cpp
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/networking/NetworkRuntime.cpp
)
target_compile_features(beam_vehicle_tests PRIVATE cxx_std_20)
target_include_directories(beam_vehicle_tests PRIVATE
    src/engine/public
    src
    ${glm_SOURCE_DIR}
)
target_link_libraries(beam_vehicle_tests PRIVATE
    vc_build_options
    vc_ozz vc_acl
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
    vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
)
if(TARGET vc_generate_world_save)
    add_dependencies(beam_vehicle_tests vc_generate_world_save)
endif()
add_test(NAME beam_vehicle_tests COMMAND beam_vehicle_tests)

# Vehicle lifecycle (FALTANTES §17 items 8-9): occupants (seats, entry, exit,
# kinematic ride) and damage/part separation (chassis/drivetrain/wheels for
# rigid vehicles; + beams for the XPBD chassis). Same object modules as the
# beam chassis tests.
add_executable(vehicle_lifecycle_tests
    tests/VehicleLifecycleTests.cpp
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/networking/NetworkRuntime.cpp
)
target_compile_features(vehicle_lifecycle_tests PRIVATE cxx_std_20)
target_include_directories(vehicle_lifecycle_tests PRIVATE
    src/engine/public
    src
    ${glm_SOURCE_DIR}
)
target_link_libraries(vehicle_lifecycle_tests PRIVATE
    vc_build_options
    vc_ozz vc_acl
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
    vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
)
if(TARGET vc_generate_world_save)
    add_dependencies(vehicle_lifecycle_tests vc_generate_world_save)
endif()
add_test(NAME vehicle_lifecycle_tests COMMAND vehicle_lifecycle_tests)

# Vehicle energy/fuel/controls (FALTANTES §17 item 7): data-driven fuel tank,
# battery and control mapping — the runtime burns fuel by throttle, draws /
# regens energy, and cuts the drive when a system is empty. Same object
# modules as the other vehicle tests.
add_executable(vehicle_power_tests
    tests/VehiclePowerTests.cpp

    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/networking/NetworkRuntime.cpp
)
target_compile_features(vehicle_power_tests PRIVATE cxx_std_20)
target_include_directories(vehicle_power_tests PRIVATE
    src/engine/public
    src
    ${glm_SOURCE_DIR}
)
target_link_libraries(vehicle_power_tests PRIVATE
    vc_build_options
    vc_ozz vc_acl
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
    vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
)
if(TARGET vc_generate_world_save)
    add_dependencies(vehicle_power_tests vc_generate_world_save)
endif()
add_test(NAME vehicle_power_tests COMMAND vehicle_power_tests)

# Provider ownership (FALTANTES §17 item 10): each physical entity (body) is
# simulated by EXACTLY ONE provider — claims are all-or-nothing with a
# diagnostic, and the vehicle factories claim the chassis automatically so a
# second vehicle on the same body is refused. Same object modules as the
# other gameplay tests.
add_executable(physics_ownership_tests
    tests/PhysicsOwnershipTests.cpp
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/networking/NetworkRuntime.cpp
)
target_compile_features(physics_ownership_tests PRIVATE cxx_std_20)
target_include_directories(physics_ownership_tests PRIVATE
    src/engine/public
    src
    ${glm_SOURCE_DIR}
)
target_link_libraries(physics_ownership_tests PRIVATE
    vc_build_options
    vc_ozz vc_acl
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
    vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
)
if(TARGET vc_generate_world_save)
    add_dependencies(physics_ownership_tests vc_generate_world_save)
endif()
add_test(NAME physics_ownership_tests COMMAND physics_ownership_tests)

# Vehicle network authority + prediction (FALTANTES §17 item 11): the server
# owns the simulation (steps vehicles with client input, broadcasts
# authoritative states); the client predicts locally and reconciles. Same
# object modules as the other vehicle tests.
add_executable(vehicle_replication_tests
    tests/VehicleReplicationTests.cpp
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/networking/NetworkRuntime.cpp
)
target_compile_features(vehicle_replication_tests PRIVATE cxx_std_20)
target_include_directories(vehicle_replication_tests PRIVATE
    src/engine/public
    src
    ${glm_SOURCE_DIR}
)
target_link_libraries(vehicle_replication_tests PRIVATE
    vc_build_options
    vc_ozz vc_acl
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
    vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
)
if(TARGET vc_generate_world_save)
    add_dependencies(vehicle_replication_tests vc_generate_world_save)
endif()
add_test(NAME vehicle_replication_tests COMMAND vehicle_replication_tests)

# Configurable voxel fracture (FALTANTES §16 item 8): the data-driven
# FractureConfig turns a solid voxel region into a destructible whose chunks
# detach as dynamic debris (mass from block density); the Jolt motion-type
# flip makes the debris actually fall in the standard world.
add_executable(fracture_tests
    tests/FractureTests.cpp
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/networking/NetworkRuntime.cpp
)
target_compile_features(fracture_tests PRIVATE cxx_std_20)
target_include_directories(fracture_tests PRIVATE src/engine/public src ${glm_SOURCE_DIR})
target_link_libraries(fracture_tests PRIVATE
    vc_build_options glm
    vc_ozz vc_acl
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
    vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
)
if(TARGET vc_generate_world_save)
    add_dependencies(fracture_tests vc_generate_world_save)
endif()
add_test(NAME fracture_tests COMMAND fracture_tests)

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
    vc_ozz vc_acl
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

# Vehicle-assembly gate (FALTANTES §17 item 12): the versioned JSON documents
# the MCP semantic layer authors (Content/Vehicles/*.json via
# author_vehicle_asset — rigid VehicleAsset / beam BeamGraphAsset) must load
# through the PUBLIC factories unchanged. The test TU compiles against only
# the public headers (like mcp_registry_gate_tests) and mirrors the exact
# documents tools/mcp-server/protocol-smoke.mjs emits.
add_executable(vehicle_asset_gate_tests
    tests/VehicleAssetGateTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/networking/NetworkRuntime.cpp
)
target_compile_features(vehicle_asset_gate_tests PRIVATE cxx_std_20)
target_include_directories(vehicle_asset_gate_tests PRIVATE
    src/engine/public
    src
    ${glm_SOURCE_DIR}
)
target_link_libraries(vehicle_asset_gate_tests PRIVATE
    vc_build_options
    vc_ozz vc_acl
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
    add_dependencies(vehicle_asset_gate_tests vc_generate_world_save)
endif()
add_test(NAME vehicle_asset_gate_tests COMMAND vehicle_asset_gate_tests)

# Vehicle provider seam (FALTANTES §17 items 5/6): the asset selects EXACTLY
# ONE physics provider; only jolt is vendored (chrono/jsbsim are refused with
# a diagnostic — never a silent fallback). The gate proves the factory refuses
# non-vendored providers and that the provider field round-trips through both
# assets.
add_executable(vehicle_provider_tests
    tests/VehicleProviderTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/networking/NetworkRuntime.cpp
)
target_compile_features(vehicle_provider_tests PRIVATE cxx_std_20)
target_include_directories(vehicle_provider_tests PRIVATE
    src/engine/public
    src
    ${glm_SOURCE_DIR}
)
target_link_libraries(vehicle_provider_tests PRIVATE
    vc_build_options
    vc_ozz vc_acl
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
    add_dependencies(vehicle_provider_tests vc_generate_world_save)
endif()
add_test(NAME vehicle_provider_tests COMMAND vehicle_provider_tests)
endif()

add_executable(motion_database_tests
    tests/MotionDatabaseTests.cpp
    # Light target (pattern of gameplay_runtime_tests): the motion-database
    # adapter is self-contained (glm + ozz + acl only), so compile it directly
    # instead of pulling every vc_sdk_public object.
    src/engine/sdk/MotionDatabase.cpp
)
target_compile_features(motion_database_tests PRIVATE cxx_std_20)
target_include_directories(motion_database_tests PRIVATE
    src/engine/public
    src
    ${glm_SOURCE_DIR}
    ${VC_SOLUTIONS_DIR}/ozz-animation/include
    ${VC_SOLUTIONS_DIR}/acl/includes
    ${VC_SOLUTIONS_DIR}/acl/external/rtm/includes
)
target_link_libraries(motion_database_tests PRIVATE
    vc_build_options
    vc_ozz vc_acl
)
add_test(NAME motion_database_tests COMMAND motion_database_tests)

# FALTANTES §18 item 4: ContactPlanner + GaitAsset + leg-chain assets. The
# gait adapter is pure logic (glm only); MotionDatabase.cpp is compiled here
# for the IK-integration gate (a planted foot target solved by the item-3
# ik_two_bone).
add_executable(gait_planner_tests
    tests/GaitPlannerTests.cpp
    src/engine/sdk/GaitPlanner.cpp
    src/engine/sdk/MotionDatabase.cpp
)
target_compile_features(gait_planner_tests PRIVATE cxx_std_20)
target_include_directories(gait_planner_tests PRIVATE
    src/engine/public
    src
    ${glm_SOURCE_DIR}
    ${VC_SOLUTIONS_DIR}/ozz-animation/include
    ${VC_SOLUTIONS_DIR}/acl/includes
    ${VC_SOLUTIONS_DIR}/acl/external/rtm/includes
)
target_link_libraries(gait_planner_tests PRIVATE
    vc_build_options
    vc_ozz vc_acl
)
add_test(NAME gait_planner_tests COMMAND gait_planner_tests)

# FALTANTES §18 item 7: spider-style procedural locomotion (minecraft-spider
# concepts as REFERENCE only — never compiled, like shape-ml). The driver is
# pure (glm + the public animation headers); the integration gate (legs ->
# placement -> IK) needs the motion database, so the target compiles the SDK
# adapters directly (the light pattern, like gait_planner_tests). Terrain is
# sampled through the IFootTerrainSampler seam (item 5) — the gate uses
# in-test samplers; the voxel sampler itself is proven by foot_placement_tests.
add_executable(procedural_legs_tests
    tests/ProceduralLegsTests.cpp
    src/engine/sdk/ProceduralLegs.cpp
    src/engine/sdk/GaitPlanner.cpp
    src/engine/sdk/MotionDatabase.cpp
)
target_compile_features(procedural_legs_tests PRIVATE cxx_std_20)
target_include_directories(procedural_legs_tests PRIVATE
    src/engine/public
    src
    ${glm_SOURCE_DIR}
    ${VC_SOLUTIONS_DIR}/ozz-animation/include
    ${VC_SOLUTIONS_DIR}/acl/includes
    ${VC_SOLUTIONS_DIR}/acl/external/rtm/includes
)
target_link_libraries(procedural_legs_tests PRIVATE
    vc_build_options
    vc_ozz vc_acl
)
add_test(NAME procedural_legs_tests COMMAND procedural_legs_tests)

# FALTANTES §18 item 9: motion matching (feature vector + exhaustive search,
# pure glm — the ozz sample_motion_matching is an MIT reference only). The
# adapter is self-contained, so the target follows the light pattern (compile
# the SDK TU directly; no physics/voxel dependencies).
add_executable(motion_matcher_tests
    tests/MotionMatcherTests.cpp
    src/engine/sdk/MotionMatcher.cpp
    src/engine/sdk/MotionDatabase.cpp
)
target_compile_features(motion_matcher_tests PRIVATE cxx_std_20)
target_include_directories(motion_matcher_tests PRIVATE
    src/engine/public
    src
    ${glm_SOURCE_DIR}
    ${VC_SOLUTIONS_DIR}/ozz-animation/include
    ${VC_SOLUTIONS_DIR}/acl/includes
    ${VC_SOLUTIONS_DIR}/acl/external/rtm/includes
)
target_link_libraries(motion_matcher_tests PRIVATE
    vc_build_options
    vc_ozz vc_acl
)
add_test(NAME motion_matcher_tests COMMAND motion_matcher_tests)

# FALTANTES §18 item 10: hair/fur as an optional plugin with LOD. The
# StrandSolver is self-contained (glm + the engine's own JSON parser); TressFX
# is the opt-in GPU seam (refused, never a silent fallback — the deformable
# pattern). Light target: compile the adapter + RegistryJson directly.
add_executable(hair_tests
    tests/HairTests.cpp
    src/engine/sdk/StrandHair.cpp
    src/engine/sdk/RegistryJson.cpp
)
target_compile_features(hair_tests PRIVATE cxx_std_20)
target_include_directories(hair_tests PRIVATE
    src/engine/public
    src
    ${glm_SOURCE_DIR}
)
target_link_libraries(hair_tests PRIVATE
    vc_build_options
)
add_test(NAME hair_tests COMMAND hair_tests)

# FALTANTES §18 item 11: the OFFLINE physics/learning farm seam. MuJoCo /
# MuJoCo MPC / DeepMimic generate/validate motion assets OUTSIDE the game
# (reference only — never compiled, the shape-ml/minecraft-spider pattern);
# the runtime consumes COOKED + SIGNED assets through IFarmCooker, and this
# gate proves the RuntimeCooker: deterministic FNV-1a 64 signing over canonical
# payload bytes, all-or-nothing validation, and the seam that REFUSES the
# training backends with a diagnostic.
add_executable(offline_farm_tests
    tests/OfflineFarmTests.cpp
    src/engine/sdk/OfflineFarm.cpp
)
target_compile_features(offline_farm_tests PRIVATE cxx_std_20)
target_include_directories(offline_farm_tests PRIVATE
    src/engine/public
    src
    ${glm_SOURCE_DIR}
)
target_link_libraries(offline_farm_tests PRIVATE
    vc_build_options
)
add_test(NAME offline_farm_tests COMMAND offline_farm_tests)

# FALTANTES §18 item 12: animation LOD by relevance (update frequency + bone
# subset). The adapter is PURE (it only decides budgets, never samples), so
# the gate is a light target: no ozz/ACL needed — the MotionPose struct comes
# from the public IMotionDatabase header.
add_executable(animation_lod_tests
    tests/AnimationLodTests.cpp
    src/engine/sdk/AnimationLod.cpp
)
target_compile_features(animation_lod_tests PRIVATE cxx_std_20)
target_include_directories(animation_lod_tests PRIVATE
    src/engine/public
    src
    ${glm_SOURCE_DIR}
)
target_link_libraries(animation_lod_tests PRIVATE
    vc_build_options
)
add_test(NAME animation_lod_tests COMMAND animation_lod_tests)

# FALTANTES §18 item 13: AI clip/graph validation via the public API. The
# runtime side (validate + sign, pure) compiles the AiGraphValidation adapter;
# MotionClip::validate lives in MotionDatabase.cpp, so the light target
# compiles it too (the gait_planner_tests pattern).
add_executable(ai_graph_validation_tests
    tests/AiGraphValidationTests.cpp
    src/engine/sdk/AiGraphValidation.cpp
    src/engine/sdk/MotionDatabase.cpp
)
target_compile_features(ai_graph_validation_tests PRIVATE cxx_std_20)
target_include_directories(ai_graph_validation_tests PRIVATE
    src/engine/public
    src
    ${glm_SOURCE_DIR}
    ${VC_SOLUTIONS_DIR}/ozz-animation/include
    ${VC_SOLUTIONS_DIR}/acl/includes
    ${VC_SOLUTIONS_DIR}/acl/external/rtm/includes
)
target_link_libraries(ai_graph_validation_tests PRIVATE
    vc_build_options
    vc_ozz vc_acl
)
add_test(NAME ai_graph_validation_tests COMMAND ai_graph_validation_tests)

# FALTANTES §18 item 5: foot placement on dynamic voxel terrain and moving
# surfaces. The placer is pure (glm only); the voxel sampler couples to the
# public IVoxelWorld. The integration gate (planner -> placement -> IK)
# needs the voxel world facade, so this follows the connectivity_tests
# object-library pattern (the SDK adapters come from vc_sdk_public objects).
add_executable(foot_placement_tests
    tests/FootPlacementTests.cpp
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/networking/NetworkRuntime.cpp
)
target_compile_features(foot_placement_tests PRIVATE cxx_std_20)
target_include_directories(foot_placement_tests PRIVATE
    src/engine/public
    src
    ${glm_SOURCE_DIR}
)
target_link_libraries(foot_placement_tests PRIVATE
    vc_build_options
    vc_ozz vc_acl
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
    vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
)
if(TARGET vc_generate_world_save)
    add_dependencies(foot_placement_tests vc_generate_world_save)
endif()
add_test(NAME foot_placement_tests COMMAND foot_placement_tests)

# FALTANTES §18 item 6: pose warping and locomotion adaptation. The warper
# is pure (glm only); GaitPlanner/FootPlacement/MotionDatabase are compiled
# here for the planner -> placement -> warp -> IK composition gate.
add_executable(pose_warp_tests
    tests/PoseWarpTests.cpp
    src/engine/sdk/PoseWarp.cpp
    src/engine/sdk/GaitPlanner.cpp
    src/engine/sdk/FootPlacement.cpp
    src/engine/sdk/MotionDatabase.cpp
)
target_compile_features(pose_warp_tests PRIVATE cxx_std_20)
target_include_directories(pose_warp_tests PRIVATE
    src/engine/public
    src
    ${glm_SOURCE_DIR}
    ${VC_SOLUTIONS_DIR}/ozz-animation/include
    ${VC_SOLUTIONS_DIR}/acl/includes
    ${VC_SOLUTIONS_DIR}/acl/external/rtm/includes
)
target_link_libraries(pose_warp_tests PRIVATE
    vc_build_options
    vc_ozz vc_acl
)
add_test(NAME pose_warp_tests COMMAND pose_warp_tests)






add_executable(VulkanEngineCooker
    src/tools/main_cooker.cpp
    src/tools/AssetCooker.cpp
    # AnimationRuntime (vc_core_runtime) routes through the public motion
    # database — compile the adapter TU directly (same pattern as the app
    # targets; vc_sdk_public is not linked here).
    src/engine/sdk/MotionDatabase.cpp
    $<TARGET_OBJECTS:vc_core_runtime>
)
target_link_libraries(VulkanEngineCooker PRIVATE vc_build_options vc_ozz vc_acl)

add_executable(VulkanShaderCompiler src/tools/main_shader_compiler.cpp src/tools/BuildTools.cpp)
target_link_libraries(VulkanShaderCompiler PRIVATE vc_build_options)
add_executable(VulkanProjectGenerator src/tools/main_project_generator.cpp src/tools/BuildTools.cpp)
target_link_libraries(VulkanProjectGenerator PRIVATE vc_build_options)
add_executable(VulkanPackageBuilder src/tools/main_package_builder.cpp src/tools/BuildTools.cpp)
target_link_libraries(VulkanPackageBuilder PRIVATE vc_build_options vc_ozz vc_acl)

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
