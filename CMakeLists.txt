cmake_minimum_required(VERSION 3.20)
project(vulkan_craft_1_5 LANGUAGES C CXX)
# Guard against in-source builds (Agent 6 §1.31)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not allowed. Use cmake --preset <name> or cmake -B build.")
endif()

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
option(VC_BUILD_TEST_LABELS "Apply optional labels to registered CTest tests" ON)
option(VC_ENABLE_PCH "Enable the shared precompiled header for object modules" ON)
option(VC_ENABLE_ROCKSDB "Promote RocksDB into the build" ON)
# Centralized logging (spdlog) and allocator (mimalloc) — opt-in via VC_USE_*.
# Both have fallback paths in their headers when disabled.
option(VC_USE_SPDLOG "Use spdlog for centralized structured logging" OFF)
option(VC_USE_MIMALLOC "Use mimalloc as global allocator" OFF)
set(VC_SOLUTIONS_DIR ${CMAKE_SOURCE_DIR}/external/solutions)
set(VC_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)

if(VC_ENABLE_ZSTD)
    file(GLOB VC_ZSTD_COMMON ${VC_SOLUTIONS_DIR}/zstd/lib/common/*.c)
    file(GLOB VC_ZSTD_COMPRESS ${VC_SOLUTIONS_DIR}/zstd/lib/compress/*.c)
    file(GLOB VC_ZSTD_DECOMPRESS ${VC_SOLUTIONS_DIR}/zstd/lib/decompress/*.c)
    add_library(vc_zstd STATIC ${VC_ZSTD_COMMON} ${VC_ZSTD_COMPRESS} ${VC_ZSTD_DECOMPRESS})
    target_include_directories(vc_zstd PUBLIC ${VC_SOLUTIONS_DIR}/zstd/lib)
    if(MSVC OR MINGW)
        # Keep the vendored C implementation portable; the assembly helpers
        # are not part of this source glob and otherwise remain unresolved.
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

# spirv-reflect (KhronosGroup, Apache-2.0) — SPIR-V reflection, promoted
# 2026-08-26 (Agente 1, task_plan C1). Pinned commit 3954c1e…; the clone's
# spirv_reflect.cpp shim compiles spirv_reflect.c as C++. The upstream CMake
# is not used. The public contract engine/rendering/ISpirvReflection.hpp never
# includes it; only src/engine/sdk/SpirvReflection.cpp does (adapter rule).
# BUG-010 fix: the shim is compiled INTO vc_sdk_public (see its sources) so the
# spvReflect* symbols travel with the adapter's objects — a standalone static
# lib here left every $<TARGET_OBJECTS:vc_sdk_public> consumer with unresolved
# externals (they link the objects, not the lib).

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
    # task_plan Agente 5 §1 item 6 (compatibilidade debug/release): /O2 is
    # Release-only — MSVC Debug adds /RTC1, which conflicts with /O2 (D8016)
    # and made the Debug config unbuildable across the whole tree (documented
    # in findings #130).  AVX2 belongs only to VulkanCraft targets below: a
    # global /arch:AVX2 changes feature macros in vendored Embree and makes
    # its SSE2-only Windows configuration instantiate AVX templates.
    add_compile_options($<$<NOT:$<CONFIG:Debug>>:/O2>)
endif()

include(FetchContent)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4)
FetchContent_Declare(glm GIT_REPOSITORY https://github.com/g-truc/glm.git GIT_TAG 1.0.1)
FetchContent_Declare(vk-bootstrap
    GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap.git
    GIT_TAG v0.7
)
FetchContent_Declare(vma GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git GIT_TAG v3.1.0)
FetchContent_Declare(miniaudio GIT_REPOSITORY https://github.com/mackron/miniaudio.git GIT_TAG 4a5b74be GIT_SHALLOW FALSE)
# Pin the docking branch to an immutable commit. Mutable branch names make
# otherwise identical builds resolve to different source trees over time.
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG 83f668625ad45364de71d385aeb6a5dd04bee02e)
FetchContent_MakeAvailable(glfw glm vk-bootstrap vma miniaudio imgui)
find_package(Vulkan REQUIRED)

# Eigen3 — header-only linear algebra library (vendored in external/solutions/eigen).
# Used by G.position-based-dynamics, G.splishsplash, G.geometry-central.
set(EIGEN3_INCLUDE_DIR "${VC_SOLUTIONS_DIR}/eigen")
if(NOT TARGET Eigen3::Eigen)
    add_library(Eigen3::Eigen INTERFACE IMPORTED)
    target_include_directories(Eigen3::Eigen INTERFACE "${EIGEN3_INCLUDE_DIR}")
endif()

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
if(MSVC)
    target_compile_options(vc_build_options INTERFACE /FS /fp:fast /arch:AVX2)
endif()
# MSVC parallel compilation (/MP) — uses all available cores
if(MSVC)
    target_compile_options(vc_build_options INTERFACE /MP)
    # Relocatable package (BUG-020): /pathmap strips the absolute build-machine
    # paths MSVC embeds in .obj/.lib/PDB records. Map the source and binary
    # dirs to neutral tokens so the installed SDK libs carry no checkout path.
    # /experimental:deterministic is REQUIRED on MSVC 19.5x or /pathmap is
    # silently ignored (D9007) — fix verified 2026-08-26.
    target_compile_options(vc_build_options INTERFACE
        /experimental:deterministic
        "/pathmap:${CMAKE_SOURCE_DIR}=."
        "/pathmap:${CMAKE_BINARY_DIR}=out"
    )
endif()
target_include_directories(vc_build_options INTERFACE
    ${VC_SOLUTIONS_DIR}/concurrentqueue
    ${VC_SOLUTIONS_DIR}/spdlog/include
    ${VC_SOLUTIONS_DIR}/mimalloc/include
    ${VC_SOURCE_DIRS}
    ${Vulkan_INCLUDE_DIRS}
    ${vma_SOURCE_DIR}/include
    ${miniaudio_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/third_party/jolt
    ${CMAKE_SOURCE_DIR}/third_party/bullet/src
    $<$<BOOL:${VC_USE_SPDLOG}>:${VC_SOLUTIONS_DIR}/spdlog/include>
)
target_compile_definitions(vc_build_options INTERFACE
    VULKANCRAFT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    VULKANCRAFT_SHADER_DIR="${CMAKE_BINARY_DIR}/shaders"
    $<$<CONFIG:Shipping>:VC_SHIPPING=1>
    $<$<NOT:$<CONFIG:Shipping>>:VC_SHIPPING=0>
    VC_ENABLE_VOXEL_PLUGIN=$<BOOL:${VC_ENABLE_VOXEL_PLUGIN}>
    VC_ENABLE_FLATBUFFERS=$<BOOL:${VC_ENABLE_FLATBUFFERS}>
    $<$<BOOL:${VC_USE_SPDLOG}>:VC_USE_SPDLOG=1>
    $<$<BOOL:${VC_USE_MIMALLOC}>:VC_USE_MIMALLOC=1>
)
target_link_libraries(vc_build_options INTERFACE
    glfw
    glm
    vk-bootstrap
    Vulkan::Vulkan
    vulkan_jolt
    bullet
)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_TEST_APP OFF CACHE BOOL "" FORCE)
set(EFSW_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(${VC_SOLUTIONS_DIR}/efsw efsw_build EXCLUDE_FROM_ALL)
if(TARGET efsw-static)
    add_library(vc_efsw ALIAS efsw-static)
    target_compile_options(efsw-static PRIVATE /FS /EHsc)
    target_compile_features(efsw-static PRIVATE cxx_std_20)
endif()

function(vc_object_module target)
    add_library(${target} OBJECT ${ARGN})
    target_link_libraries(${target} PRIVATE vc_build_options)
    if(VC_ENABLE_PCH)
        # The shared header is C++ only.  Applying it to the C language
        # compile pass makes MSVC generate cmake_pch.c and fail with
        # STL1003 (the C compiler cannot include the C++ STL headers).
        target_precompile_headers(${target} PRIVATE
            "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_SOURCE_DIR}/cmake/pch.hpp>"
        )
    endif()
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
# Build the vendored Embree through its own CMake project.  Its sources use
# per-ISA translation units and compiler-specific intrinsics; listing them in
# vc_sdk_public bypassed Embree's Windows configuration and made MSVC compile
# AVX code with incompatible feature macros.  The official target owns those
# flags and exposes the portable SSE2 static build to the engine.
set(EMBREE_TUTORIALS OFF CACHE BOOL "" FORCE)
set(EMBREE_ISPC_SUPPORT OFF CACHE BOOL "" FORCE)
set(EMBREE_TASKING_SYSTEM INTERNAL CACHE STRING "" FORCE)
set(EMBREE_MAX_ISA SSE2 CACHE STRING "" FORCE)
set(EMBREE_STATIC_LIB ON CACHE BOOL "" FORCE)
set(EMBREE_ZIP_MODE OFF CACHE BOOL "" FORCE)
add_subdirectory(${VC_SOLUTIONS_DIR}/embree ${CMAKE_BINARY_DIR}/embree-build EXCLUDE_FROM_ALL)
set_property(DIRECTORY ${VC_SOLUTIONS_DIR}/embree PROPERTY VS_GLOBAL_UseMultiToolTask false)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT ProgramDatabase)
target_link_libraries(vc_build_options INTERFACE embree)

set(VC_EMBREE_ADAPTER_TUS
    src/engine/sdk/EmbreeRayTracer.cpp
    src/engine/sdk/VkRayTracer.cpp
)
set(VC_EMBREE_TUS
    src/engine/sdk/EmbreeRayTracer.cpp
    src/engine/sdk/VkRayTracer.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/builders/primrefgen.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/bvh/bvh.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/bvh/bvh4_factory.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/bvh/bvh8_factory.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/bvh/bvh_builder.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/bvh/bvh_builder_hair.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/bvh/bvh_builder_hair_mb.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/bvh/bvh_builder_morton.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/bvh/bvh_builder_sah.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/bvh/bvh_builder_sah_mb.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/bvh/bvh_builder_sah_spatial.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/bvh/bvh_builder_subdiv.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/bvh/bvh_builder_twolevel.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/bvh/bvh_collider.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/bvh/bvh_intersector1.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/bvh/bvh_intersector1_bvh4.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/bvh/bvh_intersector_hybrid.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/bvh/bvh_intersector_hybrid4_bvh4.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/bvh/bvh_refit.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/bvh/bvh_rotate.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/bvh/bvh_statistics.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/common/acceln.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/common/accelset.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/common/alloc.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/common/device.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/common/geometry.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/common/motion_derivative.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/common/rtcore.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/common/rtcore_builder.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/common/scene.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/common/scene_curves.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/common/scene_grid_mesh.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/common/scene_instance.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/common/scene_instance_array.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/common/scene_line_segments.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/common/scene_points.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/common/scene_quad_mesh.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/common/scene_subdiv_mesh.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/common/scene_triangle_mesh.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/common/scene_user_geometry.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/common/scene_verify.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/common/stat.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/common/state.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/geometry/curve_intersector_virtual_4i.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/geometry/curve_intersector_virtual_4i_mb.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/geometry/curve_intersector_virtual_4v.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/geometry/curve_intersector_virtual_8i.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/geometry/curve_intersector_virtual_8i_mb.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/geometry/curve_intersector_virtual_8v.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/geometry/grid_soa.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/geometry/instance_array_intersector.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/geometry/instance_intersector.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/geometry/primitive4.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/subdiv/bezier_curve.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/subdiv/bspline_curve.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/subdiv/catmullclark_coefficients.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/subdiv/catmullrom_curve.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/subdiv/subdivpatch1base.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/subdiv/subdivpatch1base_eval.cpp
    ${VC_SOLUTIONS_DIR}/embree/kernels/subdiv/tessellation_cache.cpp
    ${VC_SOLUTIONS_DIR}/embree/common/lexers/stringstream.cpp
    ${VC_SOLUTIONS_DIR}/embree/common/lexers/tokenstream.cpp
    ${VC_SOLUTIONS_DIR}/embree/common/math/constants.cpp
    ${VC_SOLUTIONS_DIR}/embree/common/simd/sse.cpp
    ${VC_SOLUTIONS_DIR}/embree/common/sys/alloc.cpp
    ${VC_SOLUTIONS_DIR}/embree/common/sys/barrier.cpp
    ${VC_SOLUTIONS_DIR}/embree/common/sys/condition.cpp
    ${VC_SOLUTIONS_DIR}/embree/common/sys/estring.cpp
    ${VC_SOLUTIONS_DIR}/embree/common/sys/filename.cpp
    ${VC_SOLUTIONS_DIR}/embree/common/sys/library.cpp
    ${VC_SOLUTIONS_DIR}/embree/common/sys/mutex.cpp
    ${VC_SOLUTIONS_DIR}/embree/common/sys/regression.cpp
    ${VC_SOLUTIONS_DIR}/embree/common/sys/sysinfo.cpp
    ${VC_SOLUTIONS_DIR}/embree/common/sys/thread.cpp
    ${VC_SOLUTIONS_DIR}/embree/common/tasking/taskschedulerinternal.cpp
)

vc_object_module(vc_sdk_public
    src/engine/sdk/VoxelWorldFacade.cpp
    src/engine/sdk/FastNoise2Adapter.cpp
    # Codec/hash/storage providers — restaurados ao módulo (refactor alheio 07:36
    # os removeu e quebrou todo consumer de $<TARGET_OBJECTS:vc_sdk_public>: o
    # VoxelWorldFacade referencia create_zstd_compression_provider etc.).
    $<$<BOOL:${VC_ENABLE_ZSTD}>:src/engine/core/compression/ZstdCompression.cpp>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:src/engine/core/hashing/Blake3Hash.cpp>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:src/engine/core/storage/RocksDbChunkStorage.cpp>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:src/engine/core/storage/RegionChunkStorage.cpp>
    src/engine/sdk/RegistryJson.cpp
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
    src/engine/sdk/StructurePlacement.cpp
    src/engine/sdk/WorldProfile.cpp
    src/engine/sdk/Parcellation.cpp
    src/engine/sdk/ShapeGrammar.cpp
    src/engine/sdk/HeightmapErosion.cpp
    src/engine/sdk/MeshCooking.cpp
    src/engine/sdk/TextShaper.cpp
    src/engine/sdk/SceneLayers.cpp
    src/engine/sdk/JsonSchema.cpp
    src/engine/sdk/ProcgenPreview.cpp
    src/engine/sdk/ProcgenJobs.cpp
    src/engine/sdk/JobSystem.cpp
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
    src/engine/sdk/TimelineGraph.cpp
    src/engine/sdk/CausalResolver.cpp
    src/engine/sdk/MacroMicroReconciler.cpp
    src/engine/sdk/WorldDirector.cpp
    src/engine/sdk/EpisodeCompiler.cpp
    src/engine/sdk/SimulationFarm.cpp
    src/engine/sdk/SemanticApi.cpp
    src/engine/sdk/VehicleAsset.cpp
    src/engine/sdk/BeamGraphAsset.cpp
    src/engine/sdk/RagdollAsset.cpp
    src/engine/sdk/VehicleReplication.cpp
    src/engine/sdk/VehicleProvider.cpp
    src/engine/sdk/AbilitySystem.cpp
    src/engine/sdk/SimulationLod.cpp
    src/engine/sdk/MissionAsset.cpp
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
    src/engine/sdk/BehaviorTree.cpp
    src/engine/sdk/ActionMap.cpp
    src/engine/sdk/UiLayout.cpp
    src/engine/sdk/UiInventoryGrid.cpp
    src/engine/sdk/UiWidgets.cpp
    src/engine/sdk/UiViewport.cpp
    src/engine/sdk/UiConfirmation.cpp
    src/engine/sdk/UiDoc.cpp
    src/engine/sdk/UiCrafting.cpp
    src/engine/sdk/FileChangeDebounce.cpp
    src/engine/sdk/FileWatcher.cpp
    src/engine/sdk/MessageCatalog.cpp
    src/engine/sdk/ShortcutDoc.cpp
    src/engine/sdk/PlayMode.cpp
    src/engine/sdk/CommandSearch.cpp
    src/engine/sdk/FrameProfiler.cpp
    src/engine/sdk/UndoHistory.cpp
    src/engine/sdk/ContentBrowser.cpp
    src/engine/sdk/WindowMode.cpp
    src/engine/sdk/EditorCamera.cpp
    src/engine/sdk/GizmoController.cpp
    src/engine/sdk/PublishPipeline.cpp
    src/engine/sdk/InspectorDoc.cpp
    src/engine/sdk/SceneHierarchy.cpp
    src/engine/sdk/OnboardingTour.cpp
    src/engine/sdk/QtEditorDoc.cpp
    src/engine/sdk/QtThemeModel.cpp
    src/engine/sdk/AnimationTimelineEditor.cpp
    src/engine/sdk/ProjectLauncher.cpp
    src/engine/sdk/EditorRetargeting.cpp
    src/engine/sdk/Steering.cpp
    src/engine/sdk/Perception.cpp
    src/engine/sdk/Fsm.cpp
    src/engine/sdk/UtilityAi.cpp
    src/engine/sdk/Planner.cpp
    src/engine/sdk/AiLod.cpp
    src/engine/sdk/AiEventBus.cpp
    src/engine/sdk/FixedTickSim.cpp
    src/engine/sdk/Faction.cpp
    src/engine/sdk/AudioMixer.cpp
    src/engine/sdk/SpatialAudio.cpp
    src/engine/sdk/AdaptiveMusic.cpp
    src/engine/sdk/LootTable.cpp
    src/engine/sdk/Equipment.cpp
    src/engine/sdk/AnimCore.cpp
    src/engine/sdk/AnimStateMachine.cpp
    src/engine/sdk/RootMotion.cpp
    src/engine/sdk/AnimEvents.cpp
    src/engine/sdk/AnimAdditive.cpp
    src/engine/sdk/AnimMask.cpp
    src/engine/sdk/IkSolver.cpp
    src/engine/sdk/Retargeting.cpp
    src/engine/sdk/Constraints.cpp
    src/engine/sdk/TerrainAdaptation.cpp
    src/engine/sdk/Inertializer.cpp
    src/engine/sdk/Skinning.cpp
    src/engine/sdk/AnimBudget.cpp
    src/engine/sdk/Explosion.cpp
    src/engine/sdk/NetworkReplication.cpp
    src/engine/sdk/NetworkRpc.cpp
    src/engine/sdk/NetworkInterest.cpp
    src/engine/sdk/NetworkDiscovery.cpp
    src/engine/sdk/Transport.cpp
    src/engine/sdk/NetworkTransportFactory.cpp
    src/engine/sdk/NetworkServer.cpp
    src/engine/sdk/AssetFormats.cpp
    src/engine/sdk/AssetCooker.cpp
    src/engine/sdk/PluginLoader.cpp
    src/engine/sdk/PluginManifestCodec.cpp
    src/engine/sdk/PluginPermissions.cpp
    src/engine/sdk/PluginIsolationRuntime.cpp
    src/engine/sdk/JobService.cpp
    src/engine/sdk/CapabilityRegistry.cpp
    src/engine/sdk/VisualScriptRuntime.cpp
    src/engine/sdk/VisualScriptService.cpp
    src/engine/sdk/AssetPipeline.cpp
    src/engine/sdk/Observability.cpp
    src/engine/sdk/PackageManager.cpp
    # Agente 1 — rendering adapters (public contracts; self-contained std+glm).
    src/engine/sdk/GlobalIllumination.cpp
    src/engine/sdk/LumenScene.cpp
    src/engine/sdk/SurfaceCacheCapture.cpp
    src/engine/sdk/DiffuseGlobalIllumination.cpp
    src/engine/sdk/SoftwareTracer.cpp
    src/engine/sdk/RenderingPresets.cpp
    src/engine/sdk/ScreenSpaceTracer.cpp
    src/engine/sdk/RenderingDebugView.cpp
    src/engine/sdk/RenderGraphCore.cpp
    src/engine/sdk/RenderPassMetrics.cpp
    src/engine/sdk/SpirvReflection.cpp
    ${VC_SOLUTIONS_DIR}/spirv-reflect/spirv_reflect.cpp
    src/engine/sdk/AtmosphereScattering.cpp
    ${VC_SOLUTIONS_DIR}/atmospheric-scattering/atmosphere/reference/functions.cc
    src/engine/sdk/Ktx2Transcoder.cpp
    ${VC_SOLUTIONS_DIR}/ktx-software/external/basis_universal/transcoder/basisu_transcoder.cpp
    src/engine/sdk/EffekseerParticles.cpp
    src/engine/sdk/ReSTIRDI.cpp
    src/engine/sdk/ProbeGrid.cpp
    src/engine/sdk/TemporalDenoiser.cpp
    src/engine/sdk/SpatialUpscaler.cpp
    src/engine/sdk/ReflectionModel.cpp
    src/engine/sdk/MaterialShading.cpp
    src/engine/sdk/VolumeClouds.cpp
    src/engine/sdk/ToneMapping.cpp
    src/engine/sdk/FftCore.cpp
    src/engine/sdk/SceneCulling.cpp
    src/engine/sdk/BlockMaterialResolver.cpp
    src/engine/sdk/ShaderCompiler.cpp
    src/engine/sdk/SparseVolumeGrid.cpp
    src/engine/sdk/CasSharpening.cpp
    src/engine/sdk/EllipsoidMath.cpp
    # Effekseer CPU core (MIT, promoted) — compiled INTO the object module
    # (BUG-010 pattern) so the adapter's symbols travel with the objects.
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Effekseer.Client.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Effekseer.Color.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Effekseer.CurveLoader.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Effekseer.DefaultEffectLoader.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Effekseer.DefaultFile.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Effekseer.Effect.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Effekseer.EffectNode.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Effekseer.EffectNodeModel.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Effekseer.EffectNodeRibbon.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Effekseer.EffectNodeRing.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Effekseer.EffectNodeRoot.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Effekseer.EffectNodeSprite.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Effekseer.EffectNodeTrack.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Effekseer.FCurves.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Effekseer.Instance.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Effekseer.InstanceChunk.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Effekseer.InstanceContainer.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Effekseer.InstanceGlobal.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Effekseer.InstanceGroup.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Effekseer.InternalScript.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Effekseer.Manager.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Effekseer.Matrix43.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Effekseer.Matrix44.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Effekseer.Random.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Effekseer.RectF.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Effekseer.Resource.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Effekseer.ResourceManager.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Effekseer.Server.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Effekseer.Setting.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Effekseer.Socket.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Effekseer.Vector2D.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Effekseer.Vector3D.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Effekseer.WorkerThread.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/ForceField/ForceFields.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/IO/Effekseer.EfkEfcFactory.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Material/Effekseer.CompiledMaterial.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Material/Effekseer.MaterialCompiler.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Material/Effekseer.MaterialFile.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Model/Model.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Model/ModelLoader.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Model/ProceduralModelGenerator.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Model/SplineGenerator.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Noise/CurlNoise.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Parameter/Easing.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Parameter/Effekseer.Parameters.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Parameter/Rotation.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/SIMD/Mat43f.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/SIMD/Mat44f.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/SIMD/Utils.cpp
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer/Effekseer/Utils/Effekseer.CustomAllocator.cpp
    $<$<NOT:$<BOOL:${MINGW}>>:${VC_EMBREE_ADAPTER_TUS}>
)
target_compile_definitions(vc_sdk_public PRIVATE
    EMBREE_STATIC_LIB
    TASKING_INTERNAL
    EMBREE_LOWEST_ISA
    BASISD_SUPPORT_KTX2_ZSTD=0
    EMBREE_TARGET_SSE2
)
# The official Embree target above owns all ISA flags.  Do not attach source
# properties here: CMake properties are global per source path and would leak
# into Embree's own targets, bypassing its MSVC-safe SSE2 configuration.
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
    ${VC_SOLUTIONS_DIR}/spirv-reflect
    ${VC_SOLUTIONS_DIR}/atmospheric-scattering
    ${VC_SOLUTIONS_DIR}/atmospheric-scattering/external/dimensional_types
    ${VC_SOLUTIONS_DIR}/ktx-software/external/basis_universal/transcoder
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer
    ${VC_SOLUTIONS_DIR}/embree/include
    ${VC_SOLUTIONS_DIR}/embree/kernels
    ${VC_SOLUTIONS_DIR}/embree/common
    ${VC_SOLUTIONS_DIR}/godot-voxel/thirdparty/fast_noise_2
    ${VC_SOLUTIONS_DIR}/godot-voxel/thirdparty/fast_noise
    ${CMAKE_SOURCE_DIR}/third_party/fastnoise_lite
    ${VC_SOLUTIONS_DIR}/flatbuffers/include
    ${VC_SOLUTIONS_DIR}/ozz-animation/include
    ${VC_SOLUTIONS_DIR}/acl/includes
    ${VC_SOLUTIONS_DIR}/acl/external/rtm/includes
    ${VC_SOLUTIONS_DIR}/efsw/include
    ${VC_SOLUTIONS_DIR}/efsw/src
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
    add_dependencies(vc_sdk_public vc_generate_world_save)
endif()

# Restaurado (refactor alheio 07:36 removeu o módulo mas deixou 33 refs
# $<TARGET_OBJECTS:vc_gameplay_sdk> + vc_sdk STATIC dependendo dele — P0 de
# configure; fontes intactos no tree).
vc_object_module(vc_gameplay_sdk
    src/engine/sdk/GameplayRuntime.cpp
    src/engine/sdk/GameplayIntegration.cpp
    src/engine/sdk/GameplayEventRouter.cpp
    src/engine/sdk/GameplayCrossDomain.cpp
    src/engine/sdk/GameplayPhase.cpp
    src/engine/sdk/GameplayBindings.cpp
    src/engine/sdk/GameplaySystemWiring.cpp
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
    src/engine/rendering/MaterialGraph.cpp
    src/engine/rendering/RenderGraph.cpp
    src/engine/rendering/RenderingFoundation.cpp
    src/engine/physics/BulletPhysicsBackend.cpp
    src/engine/physics/JoltPhysicsBackend.cpp
    src/engine/physics/ActiveRagdoll.cpp
    # Serializer.cpp já pertence a vc_core_runtime e não pode ser duplicado.
    # MaterialPipeline é Vulkan-only (vc_material_pipeline): o server headless
    # linka gameplay_sdk e não pode linkar Vulkan — padrão HEAD.
)
target_include_directories(vc_gameplay_sdk PRIVATE src)

# G.fastnoise2: FastNoise2 adapter uses FastNoiseLite (single-header MIT) backend.
# The vendored godot-voxel FastNoise2 SIMD has MSVC 2026 template bugs (C2672).
# FastNoiseLite is the working fallback.

# G.coacd: Convex Approximate Convex Decomposition (CoACD) object library.
# MIT license, self-contained with Bullet convex hull backend.
set(VC_COACD_DIR ${VC_SOLUTIONS_DIR}/coacd)
vc_object_module(vc_coacd
    ${VC_COACD_DIR}/src/btConvexHull/btAlignedAllocator.cpp
    ${VC_COACD_DIR}/src/btConvexHull/btConvexHullComputer.cpp
    ${VC_COACD_DIR}/src/bvh.cpp
    ${VC_COACD_DIR}/src/clip.cpp
    ${VC_COACD_DIR}/src/cost.cpp
    ${VC_COACD_DIR}/src/io.cpp
    ${VC_COACD_DIR}/src/logger.cpp
    ${VC_COACD_DIR}/src/mcts.cpp
    ${VC_COACD_DIR}/src/model_obj.cpp
    ${VC_COACD_DIR}/src/process.cpp
    ${VC_COACD_DIR}/src/quickhull/QuickHull.cpp
    ${VC_COACD_DIR}/src/shape.cpp
    ${VC_COACD_DIR}/src/sobol.cpp
    ${VC_COACD_DIR}/public/coacd.cpp
    # NOTE: preprocess.cpp (OpenVDB remesh) is intentionally excluded: it is
    # only compiled when WITH_3RD_PARTY_LIBS is enabled, and it needs TBB
    # headers (not vendored). The core convex-decomposition pipeline does not
    # reference it. See tools/portability/_coacd_gate for the verified build.
)
# Include order matters: coacd/src must NOT be a global -I dir because it
# shadows the CRT's <io.h> (every #include <io.h> would resolve to
# coacd/src/io.h, which references Model before it is declared).
# CoACD's own headers use quoted includes ("shape.h", "./io.h"), which
# resolve relative to the including file, so no -I coacd/src is needed.
# spdlog uses its bundled fmt (SPDLOG_FMT_EXTERNAL must stay UNDEFINED,
# defining it to 0 still trips the #ifdef and breaks fmt resolution).
# clip.cpp needs the CDT headers (restored from the pinned vcpkg artifact
# artem-ogre/CDT 1.4.5 into 3rd/cdt).
# process.cpp needs /openmp; bundled fmt needs /utf-8 on MSVC.
target_include_directories(vc_coacd PRIVATE
    ${VC_COACD_DIR}/public
    ${VC_SOLUTIONS_DIR}/spdlog/include
    ${VC_COACD_DIR}/3rd/cdt/include
)
target_compile_definitions(vc_coacd PRIVATE COACD_STATIC)
target_compile_options(vc_coacd PRIVATE /utf-8 /openmp)

# G.manifold: Manifold CSG boolean operations (union/subtract/intersect).
# Apache 2.0, self-contained C++17, no external dependencies.
set(VC_MANIFOLD_DIR ${VC_SOLUTIONS_DIR}/manifold)
vc_object_module(vc_manifold
    ${VC_MANIFOLD_DIR}/src/boolean2.cpp
    ${VC_MANIFOLD_DIR}/src/boolean2_diagnostics.cpp
    ${VC_MANIFOLD_DIR}/src/boolean2_offset.cpp
    ${VC_MANIFOLD_DIR}/src/boolean2_predicates.cpp
    ${VC_MANIFOLD_DIR}/src/boolean2_sweep.cpp
    ${VC_MANIFOLD_DIR}/src/boolean3.cpp
    ${VC_MANIFOLD_DIR}/src/boolean_result.cpp
    ${VC_MANIFOLD_DIR}/src/constructors.cpp
    ${VC_MANIFOLD_DIR}/src/cross_section.cpp
    ${VC_MANIFOLD_DIR}/src/csg_tree.cpp
    ${VC_MANIFOLD_DIR}/src/edge_op.cpp
    ${VC_MANIFOLD_DIR}/src/execution_impl.cpp
    ${VC_MANIFOLD_DIR}/src/face_op.cpp
    ${VC_MANIFOLD_DIR}/src/impl.cpp
    ${VC_MANIFOLD_DIR}/src/manifold.cpp
    ${VC_MANIFOLD_DIR}/src/minkowski.cpp
    ${VC_MANIFOLD_DIR}/src/polygon.cpp
    ${VC_MANIFOLD_DIR}/src/properties.cpp
    ${VC_MANIFOLD_DIR}/src/quickhull.cpp
    ${VC_MANIFOLD_DIR}/src/sdf.cpp
    ${VC_MANIFOLD_DIR}/src/smoothing.cpp
    ${VC_MANIFOLD_DIR}/src/sort.cpp
    ${VC_MANIFOLD_DIR}/src/subdivision.cpp
    ${VC_MANIFOLD_DIR}/src/tree2d.cpp
)
target_include_directories(vc_manifold PRIVATE
    ${VC_MANIFOLD_DIR}/include
)
target_compile_definitions(vc_manifold PRIVATE MANIFOLD_STATIC MANIFOLD_PAR=-1)

# G.position-based-dynamics: XPBD cloth/rope/rod/beam simulation.
# MIT license, requires Eigen3 (vendored in external/solutions/eigen).
# NOTE: only the core solver (PositionBasedDynamics/) is built. The
# Simulation/ layer (SceneLoader, SimulationModel, TimeStep...) depends on the
# GenericParameters submodule (GenParam::ParameterObject) which is NOT vendored
# (submodule, absent from the tarball) — same pattern as coacd's preprocess.cpp
# (needs OpenVDB/TBB, excluded). Core solver is fully self-contained: Eigen +
# header-only Utils/Logger.h. Covered by pbd_solver_tests.
set(VC_PBD_DIR ${VC_SOLUTIONS_DIR}/position-based-dynamics)
vc_object_module(vc_pbd
    ${VC_PBD_DIR}/PositionBasedDynamics/MathFunctions.cpp
    ${VC_PBD_DIR}/PositionBasedDynamics/PositionBasedDynamics.cpp
    ${VC_PBD_DIR}/PositionBasedDynamics/PositionBasedElasticRods.cpp
    ${VC_PBD_DIR}/PositionBasedDynamics/PositionBasedFluids.cpp
    ${VC_PBD_DIR}/PositionBasedDynamics/PositionBasedRigidBodyDynamics.cpp
    ${VC_PBD_DIR}/PositionBasedDynamics/SPHKernels.cpp
    ${VC_PBD_DIR}/PositionBasedDynamics/TimeIntegration.cpp
    ${VC_PBD_DIR}/PositionBasedDynamics/XPBD.cpp
)
target_include_directories(vc_pbd PRIVATE
    ${VC_PBD_DIR}
    ${VC_PBD_DIR}/Common
    ${EIGEN3_INCLUDE_DIR}
)
target_compile_definitions(vc_pbd PRIVATE _USE_MATH_DEFINES)

# G.position-based-dynamics gate: exercises the vendored XPBD/PBD solver
# (distance, shape-matching and volume constraints) — deterministic, headless.
add_executable(pbd_solver_tests
    src/engine/core/uuid/UUID.cpp
    tests/PbdSolverTests.cpp
    $<TARGET_OBJECTS:vc_pbd>
)
target_compile_features(pbd_solver_tests PRIVATE cxx_std_20)
target_include_directories(pbd_solver_tests PRIVATE
    src/engine/public
    src
    ${VC_PBD_DIR}
    ${VC_PBD_DIR}/Common
    ${EIGEN3_INCLUDE_DIR}
)
target_link_libraries(pbd_solver_tests PRIVATE vc_build_options)
add_test(NAME pbd_solver_tests COMMAND pbd_solver_tests)

# G.geometry-central: computational geometry library (surface mesh processing,
# geodesics, heat method, parameterization). MIT license, header + source, requires Eigen3.
# Suitesparse disabled: NOT defined at all (upstream guards with #ifdef GC_HAVE_SUITESPARSE,
# so defining it to 0 would still enable it and pull SuiteSparseQR.hpp which is not vendored).
# nanort/nanoflann/happly are header-only in deps/. happly.h was restored (was empty in the
# vendored tarball; official v2 from nmwsharp/happly, MIT). unit_vector3.cpp is NOT compiled:
# it is absent from upstream's explicit SRCS list and its header never existed (dead file).
set(VC_GC_DIR ${VC_SOLUTIONS_DIR}/geometry-central)
set(VC_GC_SOURCES
    ${VC_GC_DIR}/src/surface/surface_mesh.cpp
    ${VC_GC_DIR}/src/surface/manifold_surface_mesh.cpp
    ${VC_GC_DIR}/src/surface/halfedge_factories.cpp
    ${VC_GC_DIR}/src/surface/surface_mesh_factories.cpp
    ${VC_GC_DIR}/src/surface/meshio.cpp
    ${VC_GC_DIR}/src/surface/simple_polygon_mesh.cpp
    ${VC_GC_DIR}/src/surface/mesh_ray_tracer.cpp
    ${VC_GC_DIR}/src/surface/rich_surface_mesh_data.cpp
    ${VC_GC_DIR}/src/surface/base_geometry_interface.cpp
    ${VC_GC_DIR}/src/surface/boundary_first_flattening.cpp
    ${VC_GC_DIR}/src/surface/intrinsic_geometry_interface.cpp
    ${VC_GC_DIR}/src/surface/extrinsic_geometry_interface.cpp
    ${VC_GC_DIR}/src/surface/exact_geodesic_helpers.cpp
    ${VC_GC_DIR}/src/surface/exact_geodesics.cpp
    ${VC_GC_DIR}/src/surface/embed_convex.cpp
    ${VC_GC_DIR}/src/surface/embedded_geometry_interface.cpp
    ${VC_GC_DIR}/src/surface/edge_length_geometry.cpp
    ${VC_GC_DIR}/src/surface/vertex_position_geometry.cpp
    ${VC_GC_DIR}/src/surface/marching_triangles.cpp
    ${VC_GC_DIR}/src/surface/mutation_manager.cpp
    ${VC_GC_DIR}/src/surface/mesh_graph_algorithms.cpp
    ${VC_GC_DIR}/src/surface/direction_fields.cpp
    ${VC_GC_DIR}/src/surface/heat_method_distance.cpp
    ${VC_GC_DIR}/src/surface/vector_heat_method.cpp
    ${VC_GC_DIR}/src/surface/signed_heat_method.cpp
    ${VC_GC_DIR}/src/surface/polygon_mesh_heat_solver.cpp
    ${VC_GC_DIR}/src/surface/geodesic_centroidal_voronoi_tessellation.cpp
    ${VC_GC_DIR}/src/surface/trace_geodesic.cpp
    ${VC_GC_DIR}/src/surface/normal_coordinates.cpp
    ${VC_GC_DIR}/src/surface/surface_centers.cpp
    ${VC_GC_DIR}/src/surface/simple_idt.cpp
    ${VC_GC_DIR}/src/surface/intrinsic_triangulation.cpp
    ${VC_GC_DIR}/src/surface/signpost_intrinsic_triangulation.cpp
    ${VC_GC_DIR}/src/surface/integer_coordinates_intrinsic_triangulation.cpp
    ${VC_GC_DIR}/src/surface/common_subdivision.cpp
    ${VC_GC_DIR}/src/surface/surface_point.cpp
    ${VC_GC_DIR}/src/surface/barycentric_vector.cpp
    ${VC_GC_DIR}/src/surface/fast_marching_method.cpp
    ${VC_GC_DIR}/src/surface/intersection.cpp
    ${VC_GC_DIR}/src/surface/uniformize.cpp
    ${VC_GC_DIR}/src/surface/parameterize.cpp
    ${VC_GC_DIR}/src/surface/remeshing.cpp
    ${VC_GC_DIR}/src/surface/surgery.cpp
    ${VC_GC_DIR}/src/surface/exact_polyhedral_geodesics.cpp
    ${VC_GC_DIR}/src/surface/intrinsic_mollification.cpp
    ${VC_GC_DIR}/src/surface/tufted_laplacian.cpp
    ${VC_GC_DIR}/src/surface/flip_geodesics.cpp
    ${VC_GC_DIR}/src/surface/stripe_patterns.cpp
    ${VC_GC_DIR}/src/surface/transfer_functions.cpp
    ${VC_GC_DIR}/src/surface/quadric_error_simplification.cpp
    ${VC_GC_DIR}/src/surface/subdivide.cpp
    ${VC_GC_DIR}/src/surface/poisson_disk_sampler.cpp
    ${VC_GC_DIR}/src/pointcloud/point_cloud.cpp
    ${VC_GC_DIR}/src/pointcloud/neighborhoods.cpp
    ${VC_GC_DIR}/src/pointcloud/point_position_geometry.cpp
    ${VC_GC_DIR}/src/pointcloud/point_position_normal_geometry.cpp
    ${VC_GC_DIR}/src/pointcloud/point_position_frame_geometry.cpp
    ${VC_GC_DIR}/src/pointcloud/point_cloud_io.cpp
    ${VC_GC_DIR}/src/pointcloud/sample_cloud.cpp
    ${VC_GC_DIR}/src/pointcloud/local_triangulation.cpp
    ${VC_GC_DIR}/src/pointcloud/point_cloud_heat_solver.cpp
    ${VC_GC_DIR}/src/numerical/linear_algebra_utilities.cpp
    # suitesparse_utilities.cpp excluded: entire file guarded by #ifdef GC_HAVE_SUITESPARSE
    ${VC_GC_DIR}/src/numerical/linear_solvers.cpp
    ${VC_GC_DIR}/src/numerical/eigenproblem_solvers.cpp
    ${VC_GC_DIR}/src/numerical/qr_solvers.cpp
    ${VC_GC_DIR}/src/numerical/square_solvers.cpp
    ${VC_GC_DIR}/src/numerical/positive_definite_solvers.cpp
    ${VC_GC_DIR}/src/utilities/utilities.cpp
    ${VC_GC_DIR}/src/utilities/quaternion.cpp
    ${VC_GC_DIR}/src/utilities/disjoint_sets.cpp
    ${VC_GC_DIR}/src/utilities/knn.cpp
    ${VC_GC_DIR}/src/utilities/elementary_geometry.cpp
    ${VC_GC_DIR}/src/utilities/tri_tri_intersect.cpp
)
vc_object_module(vc_geometry_central ${VC_GC_SOURCES})
target_include_directories(vc_geometry_central PRIVATE
    ${VC_GC_DIR}/include
    ${EIGEN3_INCLUDE_DIR}
    ${VC_GC_DIR}/deps/nanort/include
    ${VC_GC_DIR}/deps/nanoflann/include
    ${VC_GC_DIR}/deps/happly
)
target_compile_definitions(vc_geometry_central PRIVATE _USE_MATH_DEFINES)

# G.geometry-central gate: exercises the vendored repo directly (SurfaceMesh +
# geodesic distance via adapter) — deterministic, headless.
add_executable(geometry_central_tests
    src/engine/sdk/GeometryCentralAdapter.cpp
    tests/GeometryCentralTests.cpp
    $<TARGET_OBJECTS:vc_geometry_central>
)
target_compile_features(geometry_central_tests PRIVATE cxx_std_20)
target_include_directories(geometry_central_tests PRIVATE
    src/engine/public
    src
    ${VC_GC_DIR}/include
    ${EIGEN3_INCLUDE_DIR}
    ${VC_GC_DIR}/deps/nanort/include
    ${VC_GC_DIR}/deps/nanoflann/include
    ${VC_GC_DIR}/deps/happly
)
target_compile_definitions(geometry_central_tests PRIVATE _USE_MATH_DEFINES GC_NO_DEBUG)
target_link_libraries(geometry_central_tests PRIVATE vc_build_options)
add_test(NAME geometry_central_tests COMMAND geometry_central_tests)

# Centralized logging gate: tests that Log.hpp macros compile and execute.
add_executable(logging_tests
    tests/LoggingTests.cpp
)
target_compile_features(logging_tests PRIVATE cxx_std_20)
target_include_directories(logging_tests PRIVATE src/engine/public)
target_link_libraries(logging_tests PRIVATE vc_build_options)
add_test(NAME logging_tests COMMAND logging_tests)

# Optional allocator gate: tests that Allocator.hpp compiles and works.
add_executable(allocator_tests
    tests/AllocatorTests.cpp
)
target_compile_features(allocator_tests PRIVATE cxx_std_20)
target_include_directories(allocator_tests PRIVATE src/engine/public)
target_link_libraries(allocator_tests PRIVATE vc_build_options)
add_test(NAME allocator_tests COMMAND allocator_tests)

# Agent 6 §12 items 258-262: determinism and recovery tests
add_executable(determinism_recovery_tests
    tests/DeterminismRecoveryTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/core/uuid/UUID.cpp
    src/engine/scripting/ScriptRuntime.cpp
    src/engine/networking/NetworkRuntime.cpp
)
target_compile_features(determinism_recovery_tests PRIVATE cxx_std_20)
target_include_directories(determinism_recovery_tests PRIVATE
    src/engine/public
    src/engine
    ${glm_SOURCE_DIR}
)
target_link_libraries(determinism_recovery_tests PRIVATE
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
    vc_efsw shlwapi
)
if(TARGET vc_generate_world_save)
    add_dependencies(determinism_recovery_tests vc_generate_world_save)
endif()
add_test(NAME determinism_recovery_tests COMMAND determinism_recovery_tests)

# Agent 6 items 236-244: MCP game generation tests
add_executable(mcp_game_generation_tests
    tests/McpGameGenerationTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/core/uuid/UUID.cpp
    src/engine/scripting/ScriptRuntime.cpp
    src/engine/networking/NetworkRuntime.cpp
)
target_compile_features(mcp_game_generation_tests PRIVATE cxx_std_20)
target_include_directories(mcp_game_generation_tests PRIVATE
    src/engine/public
    src/engine
    ${glm_SOURCE_DIR}
)
target_link_libraries(mcp_game_generation_tests PRIVATE
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
    vc_efsw shlwapi
)
if(TARGET vc_generate_world_save)
    add_dependencies(mcp_game_generation_tests vc_generate_world_save)
endif()
add_test(NAME mcp_game_generation_tests COMMAND mcp_game_generation_tests)

# Agent 6 items 239, 249, 266, 268: integration and validation tests
add_executable(integration_validation_tests
    tests/IntegrationValidationTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/core/uuid/UUID.cpp
    src/engine/scripting/ScriptRuntime.cpp
    src/engine/networking/NetworkRuntime.cpp
)
target_compile_features(integration_validation_tests PRIVATE cxx_std_20)
target_include_directories(integration_validation_tests PRIVATE
    src/engine/public
    src/engine
    ${glm_SOURCE_DIR}
)
target_link_libraries(integration_validation_tests PRIVATE
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
    vc_efsw shlwapi
)
if(TARGET vc_generate_world_save)
    add_dependencies(integration_validation_tests vc_generate_world_save)
endif()
add_test(NAME integration_validation_tests COMMAND integration_validation_tests)

# Agent 6 items 267, 269: plugin and schema tests
add_executable(plugin_schema_tests
    tests/PluginSchemaTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/core/uuid/UUID.cpp
    src/engine/scripting/ScriptRuntime.cpp
    src/engine/networking/NetworkRuntime.cpp
)
target_compile_features(plugin_schema_tests PRIVATE cxx_std_20)
target_include_directories(plugin_schema_tests PRIVATE
    src/engine/public
    src/engine
    ${glm_SOURCE_DIR}
)
target_link_libraries(plugin_schema_tests PRIVATE
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
    vc_efsw shlwapi
)
if(TARGET vc_generate_world_save)
    add_dependencies(plugin_schema_tests vc_generate_world_save)
endif()
add_test(NAME plugin_schema_tests COMMAND plugin_schema_tests)

# Agent 6 items 248, 250-253, 274: multiplayer and performance tests
add_executable(multiplayer_stress_tests
    tests/MultiplayerStressTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/core/uuid/UUID.cpp
    src/engine/scripting/ScriptRuntime.cpp
    src/engine/networking/NetworkRuntime.cpp
)
target_compile_features(multiplayer_stress_tests PRIVATE cxx_std_20)
target_include_directories(multiplayer_stress_tests PRIVATE
    src/engine/public
    src/engine
    ${glm_SOURCE_DIR}
)
target_link_libraries(multiplayer_stress_tests PRIVATE
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
    vc_efsw shlwapi
)
if(TARGET vc_generate_world_save)
    add_dependencies(multiplayer_stress_tests vc_generate_world_save)
endif()
add_test(NAME multiplayer_stress_tests COMMAND multiplayer_stress_tests)

# Agent 6 items 207, 257, 278: replay and cross-platform tests
add_executable(replay_crossplatform_tests
    tests/ReplayCrossPlatformTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/core/uuid/UUID.cpp
    src/engine/scripting/ScriptRuntime.cpp
    src/engine/networking/NetworkRuntime.cpp
)
target_compile_features(replay_crossplatform_tests PRIVATE cxx_std_20)
target_include_directories(replay_crossplatform_tests PRIVATE
    src/engine/public
    src/engine
    ${glm_SOURCE_DIR}
)
target_link_libraries(replay_crossplatform_tests PRIVATE
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
    vc_efsw shlwapi
)
if(TARGET vc_generate_world_save)
    add_dependencies(replay_crossplatform_tests vc_generate_world_save)
endif()
add_test(NAME replay_crossplatform_tests COMMAND replay_crossplatform_tests)

add_executable(effekseer_particles_tests
    tests/EffekseerParticlesTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    src/engine/networking/NetworkRuntime.cpp
)
target_include_directories(effekseer_particles_tests PRIVATE
    src/engine/public
    src
    ${VC_SOLUTIONS_DIR}/effekseer/Dev/Cpp/Effekseer
)
target_link_libraries(effekseer_particles_tests PRIVATE vc_build_options
    $<TARGET_OBJECTS:vc_core_runtime>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    vc_ozz vc_acl vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
    vc_efsw shlwapi
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
)
add_test(NAME effekseer_particles_tests COMMAND effekseer_particles_tests)

# Agente 1 — gate restaurado (refactor alheio 07:36 removeu; fonte intacto).
add_executable(global_illumination_tests
    tests/GlobalIlluminationTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    src/engine/networking/NetworkRuntime.cpp
)
target_include_directories(global_illumination_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(global_illumination_tests PRIVATE vc_build_options
    $<TARGET_OBJECTS:vc_core_runtime>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    vc_ozz vc_acl vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
    vc_efsw shlwapi
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
)
add_test(NAME global_illumination_tests COMMAND global_illumination_tests)

# Agente 1 — gate restaurado (refactor alheio 07:36 removeu; fonte intacto).
add_executable(lumen_scene_tests
    tests/LumenSceneTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    src/engine/networking/NetworkRuntime.cpp
)
target_include_directories(lumen_scene_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(lumen_scene_tests PRIVATE vc_build_options
    $<TARGET_OBJECTS:vc_core_runtime>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    vc_ozz vc_acl vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
    vc_efsw shlwapi
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
)
add_test(NAME lumen_scene_tests COMMAND lumen_scene_tests)

# Agente 1 — gate restaurado (refactor alheio 07:36 removeu; fonte intacto).
add_executable(surface_cache_capture_tests
    tests/SurfaceCacheCaptureTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    src/engine/networking/NetworkRuntime.cpp
)
target_include_directories(surface_cache_capture_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(surface_cache_capture_tests PRIVATE vc_build_options
    $<TARGET_OBJECTS:vc_core_runtime>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    vc_ozz vc_acl vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
    vc_efsw shlwapi
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
)
add_test(NAME surface_cache_capture_tests COMMAND surface_cache_capture_tests)

# Agente 1 — gate restaurado (refactor alheio 07:36 removeu; fonte intacto).
add_executable(diffuse_global_illumination_tests
    tests/DiffuseGlobalIlluminationTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    src/engine/networking/NetworkRuntime.cpp
)
target_include_directories(diffuse_global_illumination_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(diffuse_global_illumination_tests PRIVATE vc_build_options
    $<TARGET_OBJECTS:vc_core_runtime>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    vc_ozz vc_acl vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
    vc_efsw shlwapi
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
)
add_test(NAME diffuse_global_illumination_tests COMMAND diffuse_global_illumination_tests)

# Agente 1 — gate restaurado (refactor alheio 07:36 removeu; fonte intacto).
add_executable(screen_space_tracer_tests
    tests/ScreenSpaceTracerTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    src/engine/networking/NetworkRuntime.cpp
)
target_include_directories(screen_space_tracer_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(screen_space_tracer_tests PRIVATE vc_build_options
    $<TARGET_OBJECTS:vc_core_runtime>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    vc_ozz vc_acl vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
    vc_efsw shlwapi
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
)
add_test(NAME screen_space_tracer_tests COMMAND screen_space_tracer_tests)

# Agente 1 — gate restaurado (refactor alheio 07:36 removeu; fonte intacto).
add_executable(software_tracer_tests
    tests/SoftwareTracerTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    src/engine/networking/NetworkRuntime.cpp
)
target_include_directories(software_tracer_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(software_tracer_tests PRIVATE vc_build_options
    $<TARGET_OBJECTS:vc_core_runtime>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    vc_ozz vc_acl vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
    vc_efsw shlwapi
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
)
add_test(NAME software_tracer_tests COMMAND software_tracer_tests)

# Agente 1 — gate restaurado (refactor alheio 07:36 removeu; fonte intacto).
add_executable(rendering_presets_tests
    tests/RenderingPresetsTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    src/engine/networking/NetworkRuntime.cpp
)
target_include_directories(rendering_presets_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(rendering_presets_tests PRIVATE vc_build_options
    $<TARGET_OBJECTS:vc_core_runtime>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    vc_ozz vc_acl vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
    vc_efsw shlwapi
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
)
add_test(NAME rendering_presets_tests COMMAND rendering_presets_tests)

# Agente 1 — gate restaurado (refactor alheio 07:36 removeu; fonte intacto).
add_executable(rendering_debug_view_tests
    tests/RenderingDebugViewTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    src/engine/networking/NetworkRuntime.cpp
)
target_include_directories(rendering_debug_view_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(rendering_debug_view_tests PRIVATE vc_build_options
    $<TARGET_OBJECTS:vc_core_runtime>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    vc_ozz vc_acl vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
    vc_efsw shlwapi
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
)
add_test(NAME rendering_debug_view_tests COMMAND rendering_debug_view_tests)

# Agente 1 — gate restaurado (refactor alheio 07:36 removeu; fonte intacto).
add_executable(render_graph_core_tests
    tests/RenderGraphCoreTests.cpp
    tests/RenderGraphInternalProbe.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    src/engine/networking/NetworkRuntime.cpp
)
target_include_directories(render_graph_core_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(render_graph_core_tests PRIVATE vc_build_options
    $<TARGET_OBJECTS:vc_core_runtime>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    vc_ozz vc_acl vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
    vc_efsw shlwapi
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
)
add_test(NAME render_graph_core_tests COMMAND render_graph_core_tests)

# Agente 1 — gate restaurado (refactor alheio 07:36 removeu; fonte intacto).
add_executable(render_pass_metrics_tests
    tests/RenderPassMetricsTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    src/engine/networking/NetworkRuntime.cpp
)
target_include_directories(render_pass_metrics_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(render_pass_metrics_tests PRIVATE vc_build_options
    $<TARGET_OBJECTS:vc_core_runtime>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    vc_ozz vc_acl vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
    vc_efsw shlwapi
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
)
add_test(NAME render_pass_metrics_tests COMMAND render_pass_metrics_tests)

# Agente 1 — gate restaurado (refactor alheio 07:36 removeu; fonte intacto).
add_executable(spirv_reflection_tests
    tests/SpirvReflectionTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    src/engine/networking/NetworkRuntime.cpp
)
target_include_directories(spirv_reflection_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(spirv_reflection_tests PRIVATE vc_build_options
    $<TARGET_OBJECTS:vc_core_runtime>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    vc_ozz vc_acl vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
    vc_efsw shlwapi
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
)
add_test(NAME spirv_reflection_tests COMMAND spirv_reflection_tests)

# Agente 1 — gate restaurado (refactor alheio 07:36 removeu; fonte intacto).
add_executable(atmosphere_scattering_tests
    tests/AtmosphereScatteringTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    src/engine/networking/NetworkRuntime.cpp
)
target_include_directories(atmosphere_scattering_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(atmosphere_scattering_tests PRIVATE vc_build_options
    $<TARGET_OBJECTS:vc_core_runtime>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    vc_ozz vc_acl vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
    vc_efsw shlwapi
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
)
add_test(NAME atmosphere_scattering_tests COMMAND atmosphere_scattering_tests)

# Agente 1 — gate restaurado (refactor alheio 07:36 removeu; fonte intacto).
add_executable(ktx2_transcoder_tests
    tests/Ktx2TranscoderTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    src/engine/networking/NetworkRuntime.cpp
)
target_include_directories(ktx2_transcoder_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(ktx2_transcoder_tests PRIVATE vc_build_options
    $<TARGET_OBJECTS:vc_core_runtime>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    vc_ozz vc_acl vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
    vc_efsw shlwapi
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
)
add_test(NAME ktx2_transcoder_tests COMMAND ktx2_transcoder_tests)

# Agente 1 — C5 embree: IRayTracer (Test #19) — ray tracing offline CPU.
add_executable(embree_ray_tracer_tests
    tests/EmbreeRayTracerTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    src/engine/networking/NetworkRuntime.cpp
)
target_include_directories(embree_ray_tracer_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(embree_ray_tracer_tests PRIVATE vc_build_options
    $<TARGET_OBJECTS:vc_core_runtime>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    vc_ozz vc_acl vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
    vc_efsw shlwapi
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
)
add_test(NAME embree_ray_tracer_tests COMMAND embree_ray_tracer_tests)

# Agente 1 — A.8 hardware ray tracing: IRayTracer backend GPU (Test #20).
# Vulkan RT (VK_KHR_ray_tracing_pipeline) — mesma API pública, seleção
# data-driven em create_ray_tracer_preferred(); SKIP honesto sem GPU RT.
add_executable(ray_tracer_gpu_tests
    tests/RayTracerGpuTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    src/engine/networking/NetworkRuntime.cpp
)
target_include_directories(ray_tracer_gpu_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(ray_tracer_gpu_tests PRIVATE vc_build_options
    $<TARGET_OBJECTS:vc_core_runtime>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    vc_ozz vc_acl vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
    vc_efsw shlwapi
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
)
add_test(NAME ray_tracer_gpu_tests COMMAND ray_tracer_gpu_tests)

# Agente 1 — A.9 ReSTIR DI: determinist headless core (IReSTIRDI — Test #21).
# Candidate generation, weighted-resampling reservoirs, temporal/spatial reuse,
# visibility seam and unbiased resolve; no GPU required.
add_executable(restir_di_tests
    tests/ReSTIRDiTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    src/engine/networking/NetworkRuntime.cpp
)
target_include_directories(restir_di_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(restir_di_tests PRIVATE vc_build_options
    $<TARGET_OBJECTS:vc_core_runtime>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    vc_ozz vc_acl vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
    vc_efsw shlwapi
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
)
add_test(NAME restir_di_tests COMMAND restir_di_tests)

# Agente 1 — A.10 DDGI/radiance probes: deterministic headless core (IProbeGrid
# — Test #18). Toroidal clipmap scroll + EMA history + relocation/classification
# under a per-frame budget; no GPU required.
add_executable(probe_grid_tests
    tests/ProbeGridTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    src/engine/networking/NetworkRuntime.cpp
)
target_include_directories(probe_grid_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(probe_grid_tests PRIVATE vc_build_options
    $<TARGET_OBJECTS:vc_core_runtime>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    vc_ozz vc_acl vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
    vc_efsw shlwapi
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
)
add_test(NAME probe_grid_tests COMMAND probe_grid_tests)

# Agente 1 — A.11 NRD-style temporal denoiser: deterministic headless core
# (ITemporalDenoiser — Test #19). Motion reprojection + depth/normal rejection
# + EMA accumulation + confidence; no GPU required.
add_executable(temporal_denoiser_tests
    tests/TemporalDenoiserTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    src/engine/networking/NetworkRuntime.cpp
)
target_include_directories(temporal_denoiser_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(temporal_denoiser_tests PRIVATE vc_build_options
    $<TARGET_OBJECTS:vc_core_runtime>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    vc_ozz vc_acl vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
    vc_efsw shlwapi
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
)
add_test(NAME temporal_denoiser_tests COMMAND temporal_denoiser_tests)

# Agente 1 — A.12 FSR-style spatial upscaler: deterministic headless core
# (ISpatialUpscaler — Test #20). Bilinear baseline + Lanczos-2 blended by edge
# magnitude; no GPU required.
add_executable(spatial_upscaler_tests
    tests/SpatialUpscalerTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    src/engine/networking/NetworkRuntime.cpp
)
target_include_directories(spatial_upscaler_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(spatial_upscaler_tests PRIVATE vc_build_options
    $<TARGET_OBJECTS:vc_core_runtime>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    vc_ozz vc_acl vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
    vc_efsw shlwapi
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
)
add_test(NAME spatial_upscaler_tests COMMAND spatial_upscaler_tests)

# Agente 1 — A.13 reflection reflectance core: deterministic headless core
# (IReflectionModel — Test #21). Fresnel (Frostbite roughness correction),
# GGX cone spread, clear coat, water (IOR + Beer-Lambert) and the
# screen/probe blend policy; no GPU required.
add_executable(reflection_model_tests
    tests/ReflectionModelTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    src/engine/networking/NetworkRuntime.cpp
)
target_include_directories(reflection_model_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(reflection_model_tests PRIVATE vc_build_options
    $<TARGET_OBJECTS:vc_core_runtime>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    vc_ozz vc_acl vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
    vc_efsw shlwapi
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
)
add_test(NAME reflection_model_tests COMMAND reflection_model_tests)

# Agente 1 — A.14 material shading core: deterministic headless core
# (IMaterialShading — Test #22). Two-sided foliage, wrapped diffuse, simple
# subsurface transmission and interior ambient decay; no GPU required.
add_executable(material_shading_tests
    tests/MaterialShadingTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    src/engine/networking/NetworkRuntime.cpp
)
target_include_directories(material_shading_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(material_shading_tests PRIVATE vc_build_options
    $<TARGET_OBJECTS:vc_core_runtime>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    vc_ozz vc_acl vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
    vc_efsw shlwapi
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
)
add_test(NAME material_shading_tests COMMAND material_shading_tests)

# Agente 1 — A.15 volumetric cloud core: deterministic headless core
# (IVolumeClouds — Test #23). Seeded FBM density field, HG phase and a
# single-scattering raymarch with Beer-Lambert; no GPU required.
add_executable(volume_clouds_tests
    tests/VolumeCloudsTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    src/engine/networking/NetworkRuntime.cpp
)
target_include_directories(volume_clouds_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(volume_clouds_tests PRIVATE vc_build_options
    $<TARGET_OBJECTS:vc_core_runtime>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    vc_ozz vc_acl vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
    vc_efsw shlwapi
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
)
add_test(NAME volume_clouds_tests COMMAND volume_clouds_tests)

# Agente 1 — B.7 HDR tone mapping core: deterministic headless core
# (IToneMapping — Test #24). EV/manual exposure, ACES/Reinhard/Filmic
# operators and exact linear <-> sRGB; no GPU required.
add_executable(tone_mapping_tests
    tests/ToneMappingTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    src/engine/networking/NetworkRuntime.cpp
)
target_include_directories(tone_mapping_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(tone_mapping_tests PRIVATE vc_build_options
    $<TARGET_OBJECTS:vc_core_runtime>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    vc_ozz vc_acl vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
    vc_efsw shlwapi
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
)
add_test(NAME tone_mapping_tests COMMAND tone_mapping_tests)

# Agente 1 — C.14 FFT nucleus: deterministic headless core (IFftCore —
# Test #25). Iterative radix-2 Cooley-Tukey, cross-checked against the naive
# DFT; no GPU required (vkfft is the vendor seam).
add_executable(fft_core_tests
    tests/FftCoreTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    src/engine/networking/NetworkRuntime.cpp
)
target_include_directories(fft_core_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(fft_core_tests PRIVATE vc_build_options
    $<TARGET_OBJECTS:vc_core_runtime>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    vc_ozz vc_acl vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
    vc_efsw shlwapi
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
)
add_test(NAME fft_core_tests COMMAND fft_core_tests)

# Agente 1 — B.8 scene culling core: deterministic headless core
# (ISceneCulling — Test #26). Frustum extraction, AABB/sphere visibility, LOD
# with hysteresis, conservative occlusion and instance grouping; no GPU
# required.
add_executable(scene_culling_tests
    tests/SceneCullingTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    src/engine/networking/NetworkRuntime.cpp
)
target_include_directories(scene_culling_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(scene_culling_tests PRIVATE vc_build_options
    $<TARGET_OBJECTS:vc_core_runtime>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    vc_ozz vc_acl vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
    vc_efsw shlwapi
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
)
add_test(NAME scene_culling_tests COMMAND scene_culling_tests)

# Agente 1 — B.1 per-face block material resolver: deterministic headless core
# (IBlockMaterialResolver — Test #27). Consumes the data-driven BlockRegistry;
# resolves face overrides + state precedence + variant keys; no GPU required.
add_executable(block_material_resolver_tests
    tests/BlockMaterialResolverTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    src/engine/networking/NetworkRuntime.cpp
)
target_include_directories(block_material_resolver_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(block_material_resolver_tests PRIVATE vc_build_options
    $<TARGET_OBJECTS:vc_core_runtime>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    vc_ozz vc_acl vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
    vc_efsw shlwapi
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
)
add_test(NAME block_material_resolver_tests COMMAND block_material_resolver_tests)

add_executable(rendering_integration_tests
    tests/RenderingIntegrationTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    src/engine/networking/NetworkRuntime.cpp
)
target_include_directories(rendering_integration_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(rendering_integration_tests PRIVATE vc_build_options
    $<TARGET_OBJECTS:vc_core_runtime>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    vc_ozz vc_acl vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
    vc_efsw shlwapi
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
)
add_test(NAME rendering_integration_tests COMMAND rendering_integration_tests)

# --- Agente 1: shader_compiler_tests (B.2/C.15 — GLSL→SPIRV compile/validate/optimize)
add_executable(shader_compiler_tests
    tests/ShaderCompilerTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    src/engine/networking/NetworkRuntime.cpp
)
target_include_directories(shader_compiler_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/engine/sdk
    ${CMAKE_SOURCE_DIR}/src/engine/public
    ${CMAKE_SOURCE_DIR}/src/engine/private
    ${CMAKE_SOURCE_DIR}/src/common
    ${CMAKE_SOURCE_DIR}/src/engine/audio
    ${GLM_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/third_party/Jolt/Jolt
    ${CMAKE_SOURCE_DIR}/third_party/Jolt
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/external/solutions/effekseer/Dev/Cpp/Effekseer
    ${CMAKE_SOURCE_DIR}/external/solutions/effekseer/Dev/Cpp/EffekseerMath
    ${CMAKE_SOURCE_DIR}/external/solutions/embree/include
    ${CMAKE_SOURCE_DIR}/external/solutions/ktx-software/include
    ${CMAKE_SOURCE_DIR}/external/solutions/spirv-reflect
    ${CMAKE_SOURCE_DIR}/external/solutions/atmospheric-scattering/src
)
target_link_libraries(shader_compiler_tests PRIVATE vc_build_options
    vc_core_runtime vc_voxel_generation vc_voxel_runtime
    vc_ozz vc_acl vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
    vc_efsw shlwapi
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
)
add_test(NAME shader_compiler_tests COMMAND shader_compiler_tests)

# --- Agente 1: sparse_volume_grid_tests (C.8 openvdb — sparse volume grid)
add_executable(sparse_volume_grid_tests
    tests/SparseVolumeGridTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    src/engine/networking/NetworkRuntime.cpp
)
target_include_directories(sparse_volume_grid_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/engine/sdk
    ${CMAKE_SOURCE_DIR}/src/engine/public
    ${CMAKE_SOURCE_DIR}/src/engine/private
    ${CMAKE_SOURCE_DIR}/src/common
    ${CMAKE_SOURCE_DIR}/src/engine/audio
    ${GLM_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/third_party/Jolt/Jolt
    ${CMAKE_SOURCE_DIR}/third_party/Jolt
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/external/solutions/effekseer/Dev/Cpp/Effekseer
    ${CMAKE_SOURCE_DIR}/external/solutions/effekseer/Dev/Cpp/EffekseerMath
    ${CMAKE_SOURCE_DIR}/external/solutions/embree/include
    ${CMAKE_SOURCE_DIR}/external/solutions/ktx-software/include
    ${CMAKE_SOURCE_DIR}/external/solutions/spirv-reflect
    ${CMAKE_SOURCE_DIR}/external/solutions/atmospheric-scattering/src
)
target_link_libraries(sparse_volume_grid_tests PRIVATE vc_build_options
    vc_core_runtime vc_voxel_generation vc_voxel_runtime
    vc_ozz vc_acl vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
    vc_efsw shlwapi
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
)
add_test(NAME sparse_volume_grid_tests COMMAND sparse_volume_grid_tests)

# --- Agente 1: cas_sharpening_tests (C.5 fidelityfx — Contrast Adaptive Sharpening)
add_executable(cas_sharpening_tests
    tests/CasSharpeningTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    src/engine/networking/NetworkRuntime.cpp
)
target_include_directories(cas_sharpening_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/engine/sdk
    ${CMAKE_SOURCE_DIR}/src/engine/public
    ${CMAKE_SOURCE_DIR}/src/engine/private
    ${CMAKE_SOURCE_DIR}/src/common
    ${CMAKE_SOURCE_DIR}/src/engine/audio
    ${GLM_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/third_party/Jolt/Jolt
    ${CMAKE_SOURCE_DIR}/third_party/Jolt
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/external/solutions/effekseer/Dev/Cpp/Effekseer
    ${CMAKE_SOURCE_DIR}/external/solutions/effekseer/Dev/Cpp/EffekseerMath
    ${CMAKE_SOURCE_DIR}/external/solutions/embree/include
    ${CMAKE_SOURCE_DIR}/external/solutions/ktx-software/include
    ${CMAKE_SOURCE_DIR}/external/solutions/spirv-reflect
    ${CMAKE_SOURCE_DIR}/external/solutions/atmospheric-scattering/src
)
target_link_libraries(cas_sharpening_tests PRIVATE vc_build_options
    vc_core_runtime vc_voxel_generation vc_voxel_runtime
    vc_ozz vc_acl vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
    vc_efsw shlwapi
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
)
add_test(NAME cas_sharpening_tests COMMAND cas_sharpening_tests)

# --- Agente 1: ellipsoid_math_tests (C.2 cesium-native — WGS84 ellipsoid math)
add_executable(ellipsoid_math_tests
    tests/EllipsoidMathTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    src/engine/networking/NetworkRuntime.cpp
)
target_include_directories(ellipsoid_math_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/engine/sdk
    ${CMAKE_SOURCE_DIR}/src/engine/public
    ${CMAKE_SOURCE_DIR}/src/engine/private
    ${CMAKE_SOURCE_DIR}/src/common
    ${CMAKE_SOURCE_DIR}/src/engine/audio
    ${GLM_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/third_party/Jolt/Jolt
    ${CMAKE_SOURCE_DIR}/third_party/Jolt
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/external/solutions/effekseer/Dev/Cpp/Effekseer
    ${CMAKE_SOURCE_DIR}/external/solutions/effekseer/Dev/Cpp/EffekseerMath
    ${CMAKE_SOURCE_DIR}/external/solutions/embree/include
    ${CMAKE_SOURCE_DIR}/external/solutions/ktx-software/include
    ${CMAKE_SOURCE_DIR}/external/solutions/spirv-reflect
    ${CMAKE_SOURCE_DIR}/external/solutions/atmospheric-scattering/src
)
target_link_libraries(ellipsoid_math_tests PRIVATE vc_build_options
    vc_core_runtime vc_voxel_generation vc_voxel_runtime
    vc_ozz vc_acl vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
    vc_efsw shlwapi
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
)
add_test(NAME ellipsoid_math_tests COMMAND ellipsoid_math_tests)

# --- Agente 1: xr_math_tests (C.9 openxr — quaternion/pose/matrix math)
add_executable(xr_math_tests
    tests/XrMathTests.cpp
    src/engine/sdk/XrMath.cpp
)
target_include_directories(xr_math_tests PRIVATE
    src/engine/public
    src/engine/sdk
)
target_link_libraries(xr_math_tests PRIVATE vc_build_options
    vc_efsw shlwapi
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
)
add_test(NAME xr_math_tests COMMAND xr_math_tests)

# --- Agente 1: hair_physics_tests (C.16 tressfx — Verlet + spring-mass)
add_executable(hair_physics_tests
    tests/HairPhysicsTests.cpp
    src/engine/sdk/HairPhysics.cpp
)
target_include_directories(hair_physics_tests PRIVATE
    src/engine/public
    src/engine/sdk
)
target_link_libraries(hair_physics_tests PRIVATE vc_build_options
    vc_efsw shlwapi
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
)
add_test(NAME hair_physics_tests COMMAND hair_physics_tests)

# --- Agente 1: fluid_simulation_tests (B.3 — shallow water equations)
add_executable(fluid_simulation_tests
    tests/FluidSimulationTests.cpp
    src/engine/sdk/FluidSimulation.cpp
)
target_include_directories(fluid_simulation_tests PRIVATE
    src/engine/public
    src/engine/sdk
)
target_link_libraries(fluid_simulation_tests PRIVATE vc_build_options
    vc_efsw shlwapi
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
)
add_test(NAME fluid_simulation_tests COMMAND fluid_simulation_tests)

# --- Agente 1: ability_effects_tests (B.4 — ability VFX hooks)
add_executable(ability_effects_tests
    tests/AbilityEffectsTests.cpp
    src/engine/sdk/AbilityEffects.cpp
)
target_include_directories(ability_effects_tests PRIVATE
    src/engine/public
    src/engine/sdk
)
target_link_libraries(ability_effects_tests PRIVATE vc_build_options)
add_test(NAME ability_effects_tests COMMAND ability_effects_tests)

# --- Agente 1: swapchain_manager_tests (B.6 — resize/swapchain lifecycle)
add_executable(swapchain_manager_tests
    tests/SwapchainManagerTests.cpp
    src/engine/sdk/SwapchainManager.cpp
)
target_include_directories(swapchain_manager_tests PRIVATE
    src/engine/public
    src/engine/sdk
)
target_link_libraries(swapchain_manager_tests PRIVATE vc_build_options
    vc_efsw shlwapi
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
)
add_test(NAME swapchain_manager_tests COMMAND swapchain_manager_tests)


# Restaurados (refactor alheio 07:36 os removeu; TestLabels os referencia).
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
    # UndoSystem (interno) delega ao contrato IUndoHistory (findings #216) —
    # o target precisa do adapter para linkar create_undo_history (discovery #236).
    src/engine/sdk/UndoHistory.cpp
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

# G.coacd: convex decomposition through the public IConvexDecomposition seam.
# Wires the vendored CoACD object module + CoACDAdapter.cpp into a real gate
# (factory, validation, convex box, concave L split, determinism).
add_executable(convex_decomposition_tests
    tests/ConvexDecompositionTests.cpp
    src/engine/sdk/CoACDAdapter.cpp
    $<TARGET_OBJECTS:vc_coacd>
)
target_compile_features(convex_decomposition_tests PRIVATE cxx_std_20)
target_include_directories(convex_decomposition_tests PRIVATE
    src
    src/engine/public
    ${VC_SOLUTIONS_DIR}/coacd/public
    ${VC_SOLUTIONS_DIR}/spdlog/include
    ${VC_COACD_DIR}/3rd/cdt/include
)
target_compile_options(convex_decomposition_tests PRIVATE /utf-8 /openmp)
target_link_libraries(convex_decomposition_tests PRIVATE vc_build_options glm)
add_test(NAME convex_decomposition_tests COMMAND convex_decomposition_tests)

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
    src/engine/rendering/vulkan/GpuRenderFeatures.cpp
    src/engine/rendering/vulkan/GpuFeaturePasses.cpp
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
    src/engine/core/uuid/UUID.cpp
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

# Integrated gameplay suite (task_plan §10): abilities, attributes, damage,
# effects, inventory (ItemRegistry + Inventory) and voxel world interaction
# exercised TOGETHER in one deterministic fight — the integration proof on top
# of the per-system unit suites. Uses the same light-target pattern: compile
# the SDK TUs directly, gate needs no other module.
add_executable(gameplay_integration_coordinator_tests
    tests/GameplayIntegrationCoordinatorTests.cpp
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_core_runtime>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/sdk/GameplayIntegration.cpp
    src/engine/sdk/EntityWorld.cpp
    src/engine/sdk/GameplayEvents.cpp
    src/engine/sdk/GameplayMetrics.cpp
    src/engine/sdk/AsyncQueryScheduler.cpp
    src/engine/sdk/GameplayRuntime.cpp
    src/engine/physics/PhysicsRuntime.cpp
    src/engine/physics/PhysicsBackend.cpp
    src/engine/physics/Physics.cpp
    src/engine/sdk/WorldManager.cpp
    src/engine/sdk/PortalSystem.cpp
    src/engine/sdk/AudioEventMapper.cpp
    src/engine/sdk/VoxelWorldFacade.cpp
    src/engine/sdk/FastNoiseGraph.cpp
    src/engine/sdk/ClimateBiome.cpp
    src/engine/sdk/ProcgenPreview.cpp
    src/engine/sdk/XpbdDeformable.cpp
    src/engine/core/serialization/Serializer.cpp
    src/engine/physics/JoltPhysicsBackend.cpp
    src/engine/physics/BulletPhysicsBackend.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/WorldProfile.cpp
    src/engine/sdk/VehicleProvider.cpp
    src/engine/sdk/VehicleAsset.cpp
    src/engine/sdk/BeamGraphAsset.cpp
    src/engine/sdk/NetworkReplication.cpp
    src/engine/core/uuid/UUID.cpp
    src/engine/physics/Ragdoll.cpp
    src/engine/gameplay/VehicleRuntime.cpp
    src/engine/gameplay/BeamChassisRuntime.cpp
    src/engine/gameplay/DestructionRuntime.cpp
    src/engine/gameplay/WeaponSystem.cpp
)
target_include_directories(gameplay_integration_coordinator_tests PRIVATE
    src/engine/public src ${CMAKE_BINARY_DIR}/generated
    ${VC_SOLUTIONS_DIR}/flatbuffers/include
    ${VC_SOLUTIONS_DIR}/godot-voxel/thirdparty/fast_noise
    ${VC_SOLUTIONS_DIR}/godot-voxel/thirdparty/fast_noise_2
)
target_link_libraries(gameplay_integration_coordinator_tests PRIVATE vc_build_options glm vc_entt)
add_test(NAME gameplay_integration_coordinator_tests COMMAND gameplay_integration_coordinator_tests)

if(NOT TARGET gameplay_debug_surface_tests)
    add_executable(gameplay_debug_surface_tests
        tests/GameplayDebugSurfaceTests.cpp
        src/engine/sdk/GameplayDebugSurface.cpp
    )
    target_include_directories(gameplay_debug_surface_tests PRIVATE src/engine/public src)
    target_link_libraries(gameplay_debug_surface_tests PRIVATE vc_build_options)
    add_test(NAME gameplay_debug_surface_tests COMMAND gameplay_debug_surface_tests)
endif()

add_executable(gameplay_integration_tests
    tests/GameplayIntegrationTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/AbilitySystem.cpp
    src/engine/sdk/Inventory.cpp
    src/engine/sdk/ItemRegistry.cpp
)
target_include_directories(gameplay_integration_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(gameplay_integration_tests PRIVATE vc_build_options glm)
add_test(NAME gameplay_integration_tests COMMAND gameplay_integration_tests)

# Simulation LOD (FALTANTES §20): regions + per-tier simulation frequency,
# sleeping, deterministic aggregate evolution, world clock/climate/seasons,
# scheduled events, budgets and coherent reactivation — a pure deterministic
# decision engine over the caller-owned state (the IAnimationLod pattern). The
# contract compiles with public SDK headers only; the gate drives it headless
# (no engine internal is touched).
add_executable(simulation_lod_tests
    tests/SimulationLodTests.cpp
    # SimulationLod.cpp uses the shared RegistryJson helpers; both live in
    # vc_sdk_public. This light target compiles the TUs directly (the
    # ability_system_tests pattern) so the gate needs no other SDK module.
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/SimulationLod.cpp
)
target_include_directories(simulation_lod_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(simulation_lod_tests PRIVATE vc_build_options glm)
add_test(NAME simulation_lod_tests COMMAND simulation_lod_tests)

# Reusable AI (agente 4 §3): blackboard tipado + behavior tree data-driven
# determinístico — composites/decorators/leaves/abort/service/cooldown/wait,
# JSON versionado all-or-nothing, round-trip bit-exact, headless. O contrato
# compila só com headers públicos; o gate dirige o runtime puro (o padrão
# simulation_lod_tests) — compila os TUs direto, sem outro módulo do SDK.
add_executable(behavior_tree_tests
    tests/BehaviorTreeTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/BehaviorTree.cpp
)
target_include_directories(behavior_tree_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(behavior_tree_tests PRIVATE vc_build_options glm)
add_test(NAME behavior_tree_tests COMMAND behavior_tree_tests)

# Input mapping (agente 4 §1 item 8): action map + rebinding + conflito +
# persistência + multi-dispositivo — contrato puro/determinístico, JSON
# versionado all-or-nothing, round-trip bit-exact, headless (o padrão
# behavior_tree_tests/simulation_lod_tests).
add_executable(action_map_tests
    tests/ActionMapTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/ActionMap.cpp
)
target_include_directories(action_map_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(action_map_tests PRIVATE vc_build_options glm)
add_test(NAME action_map_tests COMMAND action_map_tests)

# Data-driven UI layout + data binding (FALTANTES item 21 / plano agente 2 §A):
# a widget tree resolves into absolute rects deterministically; nodes bind
# visible/text to a typed store via expressions. Model-only — headless gate.
add_executable(ui_layout_tests
    tests/UiLayoutTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/UiLayout.cpp
)
target_include_directories(ui_layout_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(ui_layout_tests PRIVATE vc_build_options)
add_test(NAME ui_layout_tests COMMAND ui_layout_tests)

# Inventory-slot presentation (FALTANTES item 21 / plano agente 2 §A item 2):
# a slot grid resolves rects/icon/tooltip and drives drag-and-drop by
# DELEGATING to engine::registry::Inventory (transfer/swap — no gameplay logic
# duplication). Model-only headless gate. Links Inventory.cpp for the real
# authoritative mutation under test.
add_executable(ui_inventory_grid_tests
    tests/UiInventoryGridTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/UiInventoryGrid.cpp
    src/engine/sdk/Inventory.cpp
    src/engine/sdk/ItemRegistry.cpp
)
target_include_directories(ui_inventory_grid_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(ui_inventory_grid_tests PRIVATE vc_build_options)
add_test(NAME ui_inventory_grid_tests COMMAND ui_inventory_grid_tests)

# Interactive widgets (FALTANTES item 21 / plano agente 2 §A item 3): bars,
# modals and deterministic grid focus navigation — model-only headless gate.
# UiWidgets.cpp reuses eval_binding from UiLayout.cpp (shared grammar).
add_executable(ui_widgets_tests
    tests/UiWidgetsTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/UiLayout.cpp
    src/engine/sdk/UiWidgets.cpp
)
target_include_directories(ui_widgets_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(ui_widgets_tests PRIVATE vc_build_options)
add_test(NAME ui_widgets_tests COMMAND ui_widgets_tests)

# Viewport / scale / safe-area / accessibility (FALTANTES item 21 / plano
# agente 2 §A item 4): maps the reference space onto the usable area with a
# uniform scale (None/Fit/Fill) + safe-area insets + a11y flags. Headless gate.
add_executable(ui_viewport_tests
    tests/UiViewportTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/UiViewport.cpp
)
target_include_directories(ui_viewport_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(ui_viewport_tests PRIVATE vc_build_options)
add_test(NAME ui_viewport_tests COMMAND ui_viewport_tests)

# Action confirmation + server authority (FALTANTES item 21 / plano agente 2
# §A item 5): deterministic decision state machine (None -> Pending ->
# Confirmed | Cancelled | Rejected) with an injectable authority gate that
# runs on confirm() BEFORE the action executes. Headless gate.
add_executable(ui_confirmation_tests
    tests/UiConfirmationTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/UiConfirmation.cpp
)
target_include_directories(ui_confirmation_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(ui_confirmation_tests PRIVATE vc_build_options)
add_test(NAME ui_confirmation_tests COMMAND ui_confirmation_tests)

# UI composition document (FALTANTES item 21 / plano agente 2 §A item 6):
# one versioned JSON doc composing layout + widgets + viewport +
# confirmations — the data surface for reflection/scripting/MCP. Delegates
# JSON to each sub-contract's own loader. Headless gate.
add_executable(ui_doc_tests
    tests/UiDocTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/UiLayout.cpp
    src/engine/sdk/UiWidgets.cpp
    src/engine/sdk/UiViewport.cpp
    src/engine/sdk/UiConfirmation.cpp
    src/engine/sdk/UiDoc.cpp
)
target_include_directories(ui_doc_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(ui_doc_tests PRIVATE vc_build_options)
add_test(NAME ui_doc_tests COMMAND ui_doc_tests)

# Crafting UI session (FALTANTES item 21 / plano agente 2 §A item 7):
# presentation/decision layer that DELEGATES craftability + craft to the
# authoritative RecipeRegistry (atomic consume/produce). Headless gate.
add_executable(ui_crafting_tests
    tests/UiCraftingTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/UiCrafting.cpp
    src/engine/sdk/ItemRegistry.cpp
    src/engine/sdk/Inventory.cpp
    src/engine/sdk/RecipeRegistry.cpp
)
target_include_directories(ui_crafting_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(ui_crafting_tests PRIVATE vc_build_options)
add_test(NAME ui_crafting_tests COMMAND ui_crafting_tests)

# Editor shell layout persistence (plano agente 2 §B): registry-derived
# defaults + safe reset + tolerant snapshot apply. Model-only headless gate.
add_executable(editor_layout_tests
    tests/EditorLayoutTests.cpp
    src/editor/EditorLayout.cpp
    src/engine/sdk/RegistryJson.cpp
)
target_include_directories(editor_layout_tests PRIVATE
    src/editor
    src/engine/sdk
    src
)
target_link_libraries(editor_layout_tests PRIVATE vc_build_options)
add_test(NAME editor_layout_tests COMMAND editor_layout_tests)

# Hot-reload debounce (plano agente 2 §D "efsw watcher + debounce"): the pure
# coalescing core (injected clock -> deterministic). Native watcher wiring is a
# deferred pending task (FetchContent + OS-specific + global CMake).
add_executable(file_change_debounce_tests
    tests/FileChangeDebounceTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/FileChangeDebounce.cpp
)
target_include_directories(file_change_debounce_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(file_change_debounce_tests PRIVATE vc_build_options)
add_test(NAME file_change_debounce_tests COMMAND file_change_debounce_tests)

# Native watcher uses the single vendored efsw target declared above.

add_executable(file_watcher_tests
    tests/FileWatcherTests.cpp
    src/engine/sdk/FileWatcher.cpp
    src/engine/sdk/FileChangeDebounce.cpp
    src/engine/sdk/RegistryJson.cpp
)
target_include_directories(file_watcher_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(file_watcher_tests PRIVATE
    vc_build_options
    vc_efsw
)
add_test(NAME file_watcher_tests COMMAND file_watcher_tests)
if(MINGW)
    target_link_options(file_watcher_tests PRIVATE -static-libgcc -static-libstdc++)
endif()

# Message catalog (plano agente 2 §C "mensagens consistentes"): stable ids +
# severity + parameterized text ({0}..{n}) + actionable hint. Headless gate.
add_executable(message_catalog_tests
    tests/MessageCatalogTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/MessageCatalog.cpp
)
target_include_directories(message_catalog_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(message_catalog_tests PRIVATE vc_build_options)
add_test(NAME message_catalog_tests COMMAND message_catalog_tests)

# Shortcut documentation (plano agente 2 §C "atalhos documentados"): renders
# the CURRENT ActionMapSpec bindings as deterministic markdown (humanize +
# document). Headless gate.
add_executable(shortcut_doc_tests
    tests/ShortcutDocTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/ShortcutDoc.cpp
    src/engine/sdk/ActionMap.cpp
)
target_include_directories(shortcut_doc_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(shortcut_doc_tests PRIVATE vc_build_options)
add_test(NAME shortcut_doc_tests COMMAND shortcut_doc_tests)

# Play mode (plano agente 2 §B "Play/Simulate/Pause/Step/Stop inequívocos"):
# unambiguous play-state machine (Edit/Play/Pause/Simulate) that refuses
# invalid transitions. Headless gate.
add_executable(play_mode_tests
    tests/PlayModeTests.cpp
    src/engine/sdk/PlayMode.cpp
)
target_include_directories(play_mode_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(play_mode_tests PRIVATE vc_build_options)
add_test(NAME play_mode_tests COMMAND play_mode_tests)

# Command search (plano agente 2 §B "busca global/command palette"):
# data-driven ranked search over a command index — deterministic scores,
# bit-exact ordering. Headless gate.
add_executable(command_search_tests
    tests/CommandSearchTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/CommandSearch.cpp
)
target_include_directories(command_search_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(command_search_tests PRIVATE vc_build_options)
add_test(NAME command_search_tests COMMAND command_search_tests)

# Frame profiler (plano agente 2 §B "profiler de CPU/memória"): sliding-window
# frame-time/heap stats with deterministic percentiles and spike counting.
# Headless gate.
add_executable(frame_profiler_tests
    tests/FrameProfilerTests.cpp
    src/engine/sdk/FrameProfiler.cpp
)
target_include_directories(frame_profiler_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(frame_profiler_tests PRIVATE vc_build_options)
add_test(NAME frame_profiler_tests COMMAND frame_profiler_tests)

# Undo history (plano agente 2 §B "undo/redo"): generic undo/redo stack with
# depth cap (evicts oldest), adjacent merge, deterministic JSON. Headless gate.
add_executable(undo_history_tests
    tests/UndoHistoryTests.cpp
    src/engine/sdk/UndoHistory.cpp
)
target_include_directories(undo_history_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(undo_history_tests PRIVATE vc_build_options)
add_test(NAME undo_history_tests COMMAND undo_history_tests)

# Content browser (plano agente 2 §B "Content Browser"): deterministic asset
# index + folder tree + search/filter + selection. Headless gate.
add_executable(content_browser_tests
    tests/ContentBrowserTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/ContentBrowser.cpp
)
target_include_directories(content_browser_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(content_browser_tests PRIVATE vc_build_options)
add_test(NAME content_browser_tests COMMAND content_browser_tests)

# Window mode (plano agente 2 §B "DPI/monitores/resize/fullscreen"):
# unambiguous Windowed/Borderless/Fullscreen state machine with geometry
# preservation and monitor tracking. Headless gate.
add_executable(window_mode_tests
    tests/WindowModeTests.cpp
    src/engine/sdk/WindowMode.cpp
)
target_include_directories(window_mode_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(window_mode_tests PRIVATE vc_build_options)
add_test(NAME window_mode_tests COMMAND window_mode_tests)

# Editor camera (plano agente 2 §B): deterministic orbit/pan/zoom/fly model
# with the SAME clamps the editor used to duplicate. Headless gate.
add_executable(editor_camera_tests
    tests/EditorCameraTests.cpp
    src/engine/sdk/EditorCamera.cpp
)
target_include_directories(editor_camera_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(editor_camera_tests PRIVATE vc_build_options)
add_test(NAME editor_camera_tests COMMAND editor_camera_tests)

# Gizmo math (plano agente 2 §B): translate/rotate/scale drag deltas with snap
# and the point-to-segment distance used by the axis hit-test. Headless gate.
add_executable(gizmo_controller_tests
    tests/GizmoControllerTests.cpp
    src/engine/sdk/GizmoController.cpp
)
target_include_directories(gizmo_controller_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(gizmo_controller_tests PRIVATE vc_build_options)
add_test(NAME gizmo_controller_tests COMMAND gizmo_controller_tests)

# Publish pipeline (plano agente 2 §C): build stage machine with valid
# transitions only and deterministic JSON. Headless gate.
add_executable(publish_pipeline_tests
    tests/PublishPipelineTests.cpp
    src/engine/sdk/PublishPipeline.cpp
)
target_include_directories(publish_pipeline_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(publish_pipeline_tests PRIVATE vc_build_options)
add_test(NAME publish_pipeline_tests COMMAND publish_pipeline_tests)

# Inspector doc (plano agente 2 §C): semantic component/group model with
# typed property descriptors and deterministic JSON. Headless gate.
add_executable(inspector_doc_tests
    tests/InspectorDocTests.cpp
    src/engine/sdk/InspectorDoc.cpp
)
target_include_directories(inspector_doc_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(inspector_doc_tests PRIVATE vc_build_options)
add_test(NAME inspector_doc_tests COMMAND inspector_doc_tests)

# Scene hierarchy (plano agente 2 §B): deterministic flat scene tree with
# stable DFS order, depth and case-insensitive name search. Headless gate.
add_executable(scene_hierarchy_tests
    tests/SceneHierarchyTests.cpp
    src/engine/sdk/SceneHierarchy.cpp
)
target_include_directories(scene_hierarchy_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(scene_hierarchy_tests PRIVATE vc_build_options)
add_test(NAME scene_hierarchy_tests COMMAND scene_hierarchy_tests)

# Onboarding tour (plano agente 2 §C): deterministic tutorial step machine
# (start/next/skip/complete/reset/dismiss, all-or-nothing refusals). Headless gate.
add_executable(onboarding_tour_tests
    tests/OnboardingTourTests.cpp
    src/engine/sdk/OnboardingTour.cpp
)
target_include_directories(onboarding_tour_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(onboarding_tour_tests PRIVATE vc_build_options)
add_test(NAME onboarding_tour_tests COMMAND onboarding_tour_tests)

# Qt editor shell doc (plano agente 2 §B, porte Qt — decisão do usuário):
# deterministic model of the Qt editor shell (QDockWidget/QActions/QMenus/
# QToolBars/status). Headless gate.
add_executable(qt_editor_doc_tests
    tests/QtEditorDocTests.cpp
    src/engine/sdk/QtEditorDoc.cpp
)
target_include_directories(qt_editor_doc_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(qt_editor_doc_tests PRIVATE vc_build_options)
add_test(NAME qt_editor_doc_tests COMMAND qt_editor_doc_tests)

# Qt theme model (plano agente 2 §B, porte Qt): Wicked charcoal theme as
# QPalette roles + QSS, deterministic derivation. Headless gate.
add_executable(qt_theme_model_tests
    tests/QtThemeModelTests.cpp
    src/engine/sdk/QtThemeModel.cpp
)
target_include_directories(qt_theme_model_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(qt_theme_model_tests PRIVATE vc_build_options)
add_test(NAME qt_theme_model_tests COMMAND qt_theme_model_tests)

# Animation timeline editor (plano agente 2 §B l.33): deterministic timeline
# document model (duration/playhead/loop/tracks/keys, all-or-nothing refusals).
# Headless gate.
add_executable(animation_timeline_editor_tests
    tests/AnimationTimelineEditorTests.cpp
    src/engine/sdk/AnimationTimelineEditor.cpp
)
target_include_directories(animation_timeline_editor_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(animation_timeline_editor_tests PRIVATE vc_build_options)
add_test(NAME animation_timeline_editor_tests COMMAND animation_timeline_editor_tests)

# Project launcher (plano agente 2 §C l.66): deterministic project-launcher
# session model (hub mode / open project / open scene / dirty / recents).
# Headless gate.
add_executable(project_launcher_tests
    tests/ProjectLauncherTests.cpp
    src/engine/sdk/ProjectLauncher.cpp
)
target_include_directories(project_launcher_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(project_launcher_tests PRIVATE vc_build_options)
add_test(NAME project_launcher_tests COMMAND project_launcher_tests)

# Steering behaviors (agente 4 §3 item 3): forças de Reynolds puras e
# determinísticas (seek/flee/arrive/pursue/separation/alignment/cohesion/
# obstacle_avoidance/blend) — contrato self-contained std, gate headless
# (o padrão behavior_tree_tests/action_map_tests).
add_executable(steering_tests
    tests/SteeringTests.cpp
    src/engine/sdk/Steering.cpp
)
target_include_directories(steering_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(steering_tests PRIVATE vc_build_options glm)
add_test(NAME steering_tests COMMAND steering_tests)

# Perception/sensores (agente 4 §3 item 34): visão cone+FOV+alcance, audição
# por raio·loudness, proximidade, memória com TTL, ameaça mais próxima —
# contrato self-contained std, JSON all-or-nothing bit-exact, gate headless.
add_executable(perception_tests
    tests/PerceptionTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/Perception.cpp
)
target_include_directories(perception_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(perception_tests PRIVATE vc_build_options glm)
add_test(NAME perception_tests COMMAND perception_tests)

# FSM data-driven (agente 4 §3 item 3): transições por evento/condição/timer,
# actions enter/update/exit, terminal — contrato self-contained std, JSON
# versionado all-or-nothing bit-exact, gate headless.
add_executable(fsm_tests
    tests/FsmTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/Fsm.cpp
)
target_include_directories(fsm_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(fsm_tests PRIVATE vc_build_options glm)
add_test(NAME fsm_tests COMMAND fsm_tests)

# Utility AI (agente 4 §3 item 3): seleção por utilidade — curvas
# linear/inverse/step, pesos, remap min/max — puro/determinístico, JSON
# versionado all-or-nothing bit-exact, gate headless.
add_executable(utility_ai_tests
    tests/UtilityAiTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/UtilityAi.cpp
)
target_include_directories(utility_ai_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(utility_ai_tests PRIVATE vc_build_options glm)
add_test(NAME utility_ai_tests COMMAND utility_ai_tests)

# GOAP planner (agente 4 §3 item 3): planejamento por objetivo — átomos
# booleanos, preconditions/effects, custo, uniform-cost determinístico com
# limite de plano — JSON versionado all-or-nothing bit-exact, gate headless.
add_executable(planner_tests
    tests/PlannerTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/Planner.cpp
)
target_include_directories(planner_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(planner_tests PRIVATE vc_build_options glm)
add_test(NAME planner_tests COMMAND planner_tests)

# AI LOD por entidade (agente 4 §3 item 5): tiers full/reduced/aggregate/
# dormant por distância, intervalos determinísticos, budgets com rebaixamento
# do mais distante — contrato self-contained std, JSON versionado all-or-
# nothing bit-exact, gate headless.
add_executable(ai_lod_tests
    tests/AiLodTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/AiLod.cpp
)
target_include_directories(ai_lod_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(ai_lod_tests PRIVATE vc_build_options glm)
add_test(NAME ai_lod_tests COMMAND ai_lod_tests)

# AI event bus (agente 4 §3 item 1): log determinístico de eventos de IA —
# ordem preservada, drenagem por frame, cap FIFO, JSON all-or-nothing
# bit-exact, gate headless.
add_executable(ai_event_bus_tests
    tests/AiEventBusTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/AiEventBus.cpp
)
target_include_directories(ai_event_bus_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(ai_event_bus_tests PRIVATE vc_build_options glm)
add_test(NAME ai_event_bus_tests COMMAND ai_event_bus_tests)

# Fixed timestep (agente 4 §1 item 19): acumulador determinístico — ticks por
# frame com budget, alpha de interpolação, independente do FPS — JSON
# versionado all-or-nothing bit-exact, gate headless.
add_executable(fixed_tick_sim_tests
    tests/FixedTickSimTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/FixedTickSim.cpp
)
target_include_directories(fixed_tick_sim_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(fixed_tick_sim_tests PRIVATE vc_build_options glm)
add_test(NAME fixed_tick_sim_tests COMMAND fixed_tick_sim_tests)

# Factions/teams (agente 4 §1 item 16 "equipe"): relações simétricas
# friendly/hostile/neutral data-driven — contrato self-contained std, JSON
# versionado all-or-nothing bit-exact, gate headless.
add_executable(faction_tests
    tests/FactionTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/Faction.cpp
)
target_include_directories(faction_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(faction_tests PRIVATE vc_build_options glm)
add_test(NAME faction_tests COMMAND faction_tests)

# Audio mixer (agente 4 §7 item 73): mixer/buses/roteamento data-driven — ganho
# em dB, sidechain ducking (envelope de ataque/release), snapshots, nível de
# master — contrato self-contained std, JSON versionado all-or-nothing
# bit-exact, gate headless.
add_executable(audio_mixer_tests
    tests/AudioMixerTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/AudioMixer.cpp
)
target_include_directories(audio_mixer_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(audio_mixer_tests PRIVATE vc_build_options glm)
add_test(NAME audio_mixer_tests COMMAND audio_mixer_tests)

# Spatial audio (agente 4 §7 item 74): atenuação por distância, panning
# equal-power, oclusão (input do mundo), reverb zones AABB, prioridade/
# virtualização — contrato self-contained std, JSON versionado all-or-nothing
# bit-exact, gate headless.
add_executable(spatial_audio_tests
    tests/SpatialAudioTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/SpatialAudio.cpp
)
target_include_directories(spatial_audio_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(spatial_audio_tests PRIVATE vc_build_options glm)
add_test(NAME spatial_audio_tests COMMAND spatial_audio_tests)

# Adaptive music (agente 4 §7 item 75): camadas/estados com crossfade,
# intensidade global, stingers — contrato self-contained std, JSON versionado
# all-or-nothing bit-exact, gate headless.
add_executable(adaptive_music_tests
    tests/AdaptiveMusicTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/AdaptiveMusic.cpp
)
target_include_directories(adaptive_music_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(adaptive_music_tests PRIVATE vc_build_options glm)
add_test(NAME adaptive_music_tests COMMAND adaptive_music_tests)

# Loot tables (agente 4 §1 item 17): rolagem de drops data-driven por seed
# (splitmix64) — seleção ponderada, chance, count range, merge/ordem —
# contrato self-contained std, JSON versionado all-or-nothing bit-exact.
add_executable(loot_table_tests
    tests/LootTableTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/LootTable.cpp
)
target_include_directories(loot_table_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(loot_table_tests PRIVATE vc_build_options glm)
add_test(NAME loot_table_tests COMMAND loot_table_tests)

# Equipment slots (agente 4 §1 item 17 "equipamentos"): categorias com tags
# permitidas, equip/unequip all-or-nothing, estado bit-exact — contrato
# self-contained std, JSON versionado all-or-nothing, gate headless.
add_executable(equipment_tests
    tests/EquipmentTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/Equipment.cpp
)
target_include_directories(equipment_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(equipment_tests PRIVATE vc_build_options glm)
add_test(NAME equipment_tests COMMAND equipment_tests)

# Núcleo de animação (agente 4 §4 item 1): skeleton/clip/blend tree 1D —
# hierarquia de ossos, keyframes com lerp/slerp determinístico, local→world —
# contrato self-contained std, JSON versionado all-or-nothing bit-exact.
add_executable(anim_core_tests
    tests/AnimCoreTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/AnimCore.cpp
)
target_include_directories(anim_core_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(anim_core_tests PRIVATE vc_build_options glm)
add_test(NAME anim_core_tests COMMAND anim_core_tests)

# Animation state machine (agente 4 §4 item 1): estados com clip/speed/loop,
# transições por evento/condição/timer na ordem de declaração — contrato
# self-contained std, JSON versionado all-or-nothing bit-exact.
add_executable(anim_state_machine_tests
    tests/AnimStateMachineTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/AnimStateMachine.cpp
)
target_include_directories(anim_state_machine_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(anim_state_machine_tests PRIVATE vc_build_options glm)
add_test(NAME anim_state_machine_tests COMMAND anim_state_machine_tests)

# Root motion (agente 4 §4 item 1): delta do osso raiz de um clip entre dois
# tempos (posição/rotação/distância), sobre o IAnimCore — contrato self-
# contained std (consome o header público IAnimCore), determinístico.
add_executable(root_motion_tests
    tests/RootMotionTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/AnimCore.cpp
    src/engine/sdk/RootMotion.cpp
)
target_include_directories(root_motion_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(root_motion_tests PRIVATE vc_build_options glm)
add_test(NAME root_motion_tests COMMAND root_motion_tests)

# Animation events (agente 4 §4 item 1): eventos nomeados em timestamps de
# clips com polling meio-aberto determinístico (t0, t1], round-trip JSON
# bit-exact — contrato self-contained std sobre o IAnimCore (só validação).
add_executable(anim_events_tests
    tests/AnimEventsTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/AnimCore.cpp
    src/engine/sdk/AnimEvents.cpp
)
target_include_directories(anim_events_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(anim_events_tests PRIVATE vc_build_options glm)
add_test(NAME anim_events_tests COMMAND anim_events_tests)

# Animation additive (agente 4 §4 item 1): delta de pose relativo a uma
# referência + camada sobre pose base — contrato self-contained std sobre o
# IAnimCore (só amostragem), sem estado mutável.
add_executable(anim_additive_tests
    tests/AnimAdditiveTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/AnimCore.cpp
    src/engine/sdk/AnimAdditive.cpp
)
target_include_directories(anim_additive_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(anim_additive_tests PRIVATE vc_build_options glm)
add_test(NAME anim_additive_tests COMMAND anim_additive_tests)

# Animation masks (agente 4 §4 item 1 — ÚLTIMA unidade): máscaras de ossos
# com peso 0..1 (ausente = 0), aplicadas sobre deltas aditivos (#212) —
# contrato self-contained std, JSON bit-exact all-or-nothing.
add_executable(anim_mask_tests
    tests/AnimMaskTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/AnimMask.cpp
)
target_include_directories(anim_mask_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(anim_mask_tests PRIVATE vc_build_options glm)
add_test(NAME anim_mask_tests COMMAND anim_mask_tests)

# IK solver (agente 4 §4 item 2 — unidade "IK de mãos/pés/olhar"): 2-bone
# analítico + look-at determinístico — matemática pura self-contained std,
# sem estado mutável (estado {}).
add_executable(ik_solver_tests
    tests/IkSolverTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/IkSolver.cpp
)
target_include_directories(ik_solver_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(ik_solver_tests PRIVATE vc_build_options glm)
add_test(NAME ik_solver_tests COMMAND ik_solver_tests)

# Retargeting (agente 4 §4 item 2): mapeamento fonte→alvo entre skeletons com
# escala de posição, ossos sem mapeamento = bind da alvo — sobre o IAnimCore
# (usa bind_pose, acesso aditivo), JSON bit-exact all-or-nothing.
add_executable(retargeting_tests
    tests/RetargetingTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/AnimCore.cpp
    src/engine/sdk/Retargeting.cpp
)
target_include_directories(retargeting_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(retargeting_tests PRIVATE vc_build_options glm)
add_test(NAME retargeting_tests COMMAND retargeting_tests)

# Editor retargeting model (plano agente 2 §B l.43): deterministic
# animation-retargeting EDITOR document (skeletons / preserve root motion /
# bone mapping with scale and rotation offset / validation). Distinct from
# the runtime retargeting adapter engine/animation/IRetargeting (agente 4).
add_executable(editor_retargeting_tests
    tests/EditorRetargetingTests.cpp
    src/engine/sdk/EditorRetargeting.cpp
)
target_include_directories(editor_retargeting_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(editor_retargeting_tests PRIVATE vc_build_options)
add_test(NAME editor_retargeting_tests COMMAND editor_retargeting_tests)

# Constraints (agente 4 §4 item 2): limites de articulação por osso (Euler
# XYZ clamp + reconstrução Qx·Qy·Qz) — contrato self-contained std, JSON
# bit-exact all-or-nothing.
add_executable(constraints_tests
    tests/ConstraintsTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/Constraints.cpp
)
target_include_directories(constraints_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(constraints_tests PRIVATE vc_build_options glm)
add_test(NAME constraints_tests COMMAND constraints_tests)

# Terrain adaptation (agente 4 §4 item 2 — ÚLTIMA unidade): heightmap em
# grade + alinhamento de pés/root ao terreno (regra do menor ajuste) —
# contrato self-contained std, JSON bit-exact all-or-nothing.
add_executable(terrain_adaptation_tests
    tests/TerrainAdaptationTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/TerrainAdaptation.cpp
)
target_include_directories(terrain_adaptation_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(terrain_adaptation_tests PRIVATE vc_build_options glm)
add_test(NAME terrain_adaptation_tests COMMAND terrain_adaptation_tests)

# Inertializer (agente 4 §4 item 46, componente "inertialization"): resíduo
# current ⊖ target com envelope (1 + t/T)·e^(−t/T) — contrato self-contained
# std, JSON bit-exact all-or-nothing.
add_executable(inertializer_tests
    tests/InertializerTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/Inertializer.cpp
)
target_include_directories(inertializer_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(inertializer_tests PRIVATE vc_build_options glm)
add_test(NAME inertializer_tests COMMAND inertializer_tests)

# Skinning CPU (agente 4 §4 item 49): skin = world · invBind por osso + 4
# influências com pesos normalizados — contrato self-contained std sobre o
# IAnimCore (bind_pose + local_to_world), estado {}.
add_executable(skinning_tests
    tests/SkinningTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/AnimCore.cpp
    src/engine/sdk/Skinning.cpp
)
target_include_directories(skinning_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(skinning_tests PRIVATE vc_build_options glm)
add_test(NAME skinning_tests COMMAND skinning_tests)

# Animation budget (agente 4 §4 item 49): seleção determinística dentro do
# orçamento por frame com fairness anti-starvation — contrato self-contained
# std, JSON bit-exact all-or-nothing.
add_executable(anim_budget_tests
    tests/AnimBudgetTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/AnimBudget.cpp
)
target_include_directories(anim_budget_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(anim_budget_tests PRIVATE vc_build_options glm)
add_test(NAME anim_budget_tests COMMAND anim_budget_tests)

# Explosion CORE (agente 4 §5 item 58, blast determinístico): falloff
# (1 − d/r)^power + fragmentos por seed (xorshift32) — 1º contrato do domínio
# engine/physics, self-contained std, JSON bit-exact all-or-nothing.
# (NOTA: nome único explosion_core_tests — o target explosion_tests pertence
# à integração do AGENT-2 em tests/ExplosionTests.cpp.)
add_executable(explosion_core_tests
    tests/ExplosionCoreTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/Explosion.cpp
)
target_include_directories(explosion_core_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(explosion_core_tests PRIVATE vc_build_options glm)
add_test(NAME explosion_core_tests COMMAND explosion_core_tests)

# Replay determinístico (agente 4 §5 item 60): gravação de (tick, seed,
# inputs) por frame + reprodução bit-exact na ordem gravada, seek/trim,
# limite de frames e JSON bit-exact all-or-nothing — self-contained std.
add_executable(replay_tests
    tests/ReplayTests.cpp
    src/engine/sdk/Replay.cpp
)
target_include_directories(replay_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(replay_tests PRIVATE vc_build_options glm)
add_test(NAME replay_tests COMMAND replay_tests)

# Network replication (agente 5 §1 item 3): replicação de estado por snapshots
# determinística — frames com tick estritamente crescente, ids únicos e kind
# não-vazio (all-or-nothing), último estado por entidade, JSON bit-exact
# all-or-nothing — self-contained std (1º contrato do domínio engine/networking/).
add_executable(network_replication_tests
    tests/NetworkReplicationTests.cpp
    src/engine/sdk/NetworkReplication.cpp
)
target_include_directories(network_replication_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(network_replication_tests PRIVATE vc_build_options glm)
add_test(NAME network_replication_tests COMMAND network_replication_tests)

if(NOT TARGET network_server_tests)
    add_executable(network_server_tests tests/NetworkServerTests.cpp src/engine/sdk/NetworkServer.cpp src/engine/sdk/Transport.cpp src/engine/sdk/NetworkReplication.cpp src/engine/sdk/NetworkRpc.cpp src/engine/sdk/NetworkInterest.cpp src/engine/sdk/NetworkDiscovery.cpp)
    target_include_directories(network_server_tests PRIVATE src/engine/public src)
    target_link_libraries(network_server_tests PRIVATE vc_build_options)
    add_test(NAME network_server_tests COMMAND network_server_tests)
endif()


add_executable(transport_tests
    tests/TransportTests.cpp
    src/engine/sdk/Transport.cpp
)
target_include_directories(transport_tests PRIVATE src/engine/public src)
target_link_libraries(transport_tests PRIVATE vc_build_options)
add_test(NAME transport_tests COMMAND transport_tests)

# Network RPC (agente 5 §6 item 4): RPC determinístico — registro de
# procedimentos all-or-nothing, fila com id global estritamente crescente,
# drain destrutivo em ordem de enqueue com ack, JSON bit-exact —
# self-contained std (2º contrato do domínio engine/networking/).
add_executable(network_rpc_tests
    tests/NetworkRpcTests.cpp
    src/engine/sdk/NetworkRpc.cpp
)
target_include_directories(network_rpc_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(network_rpc_tests PRIVATE vc_build_options glm)
add_test(NAME network_rpc_tests COMMAND network_rpc_tests)

# Interest management (agente 5 §6 item 4 — "interest management" da rede
# pública): relevância determinística por distância <= raio (ou
# always_relevant), resultados ordenados, JSON bit-exact — self-contained
# std (3º contrato do domínio engine/networking/).
add_executable(network_interest_tests
    tests/NetworkInterestTests.cpp
    src/engine/sdk/NetworkInterest.cpp
)
target_include_directories(network_interest_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(network_interest_tests PRIVATE vc_build_options glm)
add_test(NAME network_interest_tests COMMAND network_interest_tests)

# Service discovery (agente 5 §6 item 4 — "discovery" da rede pública):
# registro de serviços por tipo+endpoint, saúde derivada de falhas
# consecutivas via probes, resolve por tipo retorna só os saudáveis em ordem
# determinística, JSON bit-exact — self-contained std (4º contrato do domínio
# engine/networking/).
add_executable(network_discovery_tests
    tests/NetworkDiscoveryTests.cpp
    src/engine/sdk/NetworkDiscovery.cpp
)
target_include_directories(network_discovery_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(network_discovery_tests PRIVATE vc_build_options glm)
add_test(NAME network_discovery_tests COMMAND network_discovery_tests)

# Package manager (agente 5 §6 item 5 — "Implementar packages/mods
# assináveis, manifestos, resolução de dependências e atualizações seguras"):
# manifestos com restrições de versão opacas, resolução topológica
# determinística (ciclo/restrição/ausente recusados), verificação de
# assinatura plugável via ISignatureVerifier, gate de instalação
# all-or-nothing, uninstall com dependente recusado, JSON bit-exact
# all-or-nothing — self-contained std (1º contrato do domínio
# engine/packaging/).
add_executable(package_manager_tests
    tests/PackageManagerTests.cpp
    src/engine/sdk/PackageManager.cpp
)
target_include_directories(package_manager_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(package_manager_tests PRIVATE vc_build_options glm)
add_test(NAME package_manager_tests COMMAND package_manager_tests)

# Observability (agente 5 §6 item 6 — "Publicar logging/tracing/crash
# reporting/telemetry opt-in por interfaces substituíveis"): sinks
# substituíveis em runtime, roteamento opt-in (enabled=false → no-op sem
# perder o buffer), buffer circular de logs, spans com id estritamente
# crescente, contadores/gauges ordenados, contexto de crash, JSON bit-exact
# all-or-nothing — self-contained std (1º contrato do domínio
# engine/observability/).
add_executable(observability_tests
    tests/ObservabilityTests.cpp
    src/engine/sdk/Observability.cpp
)
target_include_directories(observability_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(observability_tests PRIVATE vc_build_options glm)
add_test(NAME observability_tests COMMAND observability_tests)

# OTLP exporter: headless gate for OtlpExporter (ISink + OTLP JSON formatting).
add_executable(otlp_exporter_tests
    tests/OtlpExporterTests.cpp
    src/engine/sdk/Observability.cpp
)
target_include_directories(otlp_exporter_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(otlp_exporter_tests PRIVATE vc_build_options glm)
add_test(NAME otlp_exporter_tests COMMAND otlp_exporter_tests)

# Luau sandbox (agente 5 §3 item 6 — "Integrar runtime Luau sandboxed
# opcional com bindings gerados e budgets de CPU/memória"): política de
# sandbox (allowlist de globals, io/require proibidos), budgets
# (teto determinístico de instruções enforceado sobre o que o runner
# reporta — runner mentiroso pego pela política; guarda de profundidade),
# runner substituível em runtime (backend real = Luau vendido, provado em
# #302-luau-probe), resultado JSON bit-exact validado, contador de
# execuções all-or-nothing, JSON bit-exact all-or-nothing — self-contained
# std (1º contrato do domínio engine/scripting/).
add_executable(luau_sandbox_tests
    tests/LuauSandboxTests.cpp
    src/engine/sdk/LuauSandbox.cpp
)
target_include_directories(luau_sandbox_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(luau_sandbox_tests PRIVATE vc_build_options glm)
add_test(NAME luau_sandbox_tests COMMAND luau_sandbox_tests)

# Visual script graph (agente 5 §3 items 44-45 — "scripting visual"): headless
# node graph with typed pins, DAG validation, cycle detection, topological sort.
add_executable(visual_script_graph_tests
    tests/VisualScriptGraphTests.cpp
)
target_include_directories(visual_script_graph_tests PRIVATE
    src/engine/public
)
target_link_libraries(visual_script_graph_tests PRIVATE vc_build_options)
add_test(NAME visual_script_graph_tests COMMAND visual_script_graph_tests)

# Asset pipeline (agente 5 §6 item 1 — "pipeline público import→validate→
# cook→cache→package com operações incrementais e determinísticas"):
# validação/cook por kind (raw/json/text), cache content-addressado por
# (fonte, versão) com re-cook = cache hit, package ordenado, JSON bit-exact
# all-or-nothing — self-contained std (1º contrato do domínio engine/assets/).
add_executable(asset_pipeline_tests
    tests/AssetPipelineTests.cpp
    src/engine/sdk/AssetPipeline.cpp
)
target_include_directories(asset_pipeline_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(asset_pipeline_tests PRIVATE vc_build_options glm)
add_test(NAME asset_pipeline_tests COMMAND asset_pipeline_tests)

add_executable(asset_formats_tests tests/AssetFormatsTests.cpp src/engine/sdk/AssetFormats.cpp)
target_include_directories(asset_formats_tests PRIVATE src/engine/public src)
target_link_libraries(asset_formats_tests PRIVATE vc_build_options)
add_test(NAME asset_formats_tests COMMAND asset_formats_tests)

add_executable(plugin_loader_tests tests/PluginLoaderTests.cpp src/engine/sdk/PluginLoader.cpp src/engine/sdk/PluginPermissions.cpp)
target_include_directories(plugin_loader_tests PRIVATE src/engine/public src)
target_link_libraries(plugin_loader_tests PRIVATE vc_build_options)
add_test(NAME plugin_loader_tests COMMAND plugin_loader_tests)

add_executable(job_service_tests tests/JobServiceTests.cpp src/engine/sdk/JobService.cpp)
add_executable(capability_registry_tests tests/CapabilityRegistryTests.cpp src/engine/sdk/CapabilityRegistry.cpp)
target_include_directories(capability_registry_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/engine/public)
add_test(NAME capability_registry_tests COMMAND capability_registry_tests)
add_executable(plugin_isolation_runtime_tests tests/PluginIsolationRuntimeTests.cpp src/engine/sdk/PluginIsolationRuntime.cpp)
target_include_directories(plugin_isolation_runtime_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/engine/public)
add_test(NAME plugin_isolation_runtime_tests COMMAND plugin_isolation_runtime_tests)
add_executable(visual_script_service_tests tests/VisualScriptServiceTests.cpp src/engine/sdk/VisualScriptService.cpp src/engine/sdk/VisualScriptRuntime.cpp)
target_include_directories(visual_script_service_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/engine/public)
add_test(NAME visual_script_service_tests COMMAND visual_script_service_tests)
add_executable(asset_cooker_tests tests/AssetCookerTests.cpp src/engine/sdk/AssetCooker.cpp)
target_include_directories(asset_cooker_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/engine/public)
add_test(NAME asset_cooker_tests COMMAND asset_cooker_tests)
add_executable(plugin_manifest_codec_tests tests/PluginManifestCodecTests.cpp src/engine/sdk/PluginManifestCodec.cpp)
target_include_directories(plugin_manifest_codec_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/engine/public)
add_test(NAME plugin_manifest_codec_tests COMMAND plugin_manifest_codec_tests)
add_executable(network_transport_factory_tests tests/NetworkTransportFactoryTests.cpp src/engine/sdk/NetworkTransportFactory.cpp src/engine/sdk/Transport.cpp)
target_include_directories(network_transport_factory_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/engine/public)
add_test(NAME network_transport_factory_tests COMMAND network_transport_factory_tests)
target_include_directories(job_service_tests PRIVATE src/engine/public src)
target_link_libraries(job_service_tests PRIVATE vc_build_options)
add_test(NAME job_service_tests COMMAND job_service_tests)

add_executable(visual_script_runtime_tests tests/VisualScriptRuntimeTests.cpp src/engine/sdk/VisualScriptRuntime.cpp)
target_include_directories(visual_script_runtime_tests PRIVATE src/engine/public src)
target_link_libraries(visual_script_runtime_tests PRIVATE vc_build_options)
add_test(NAME visual_script_runtime_tests COMMAND visual_script_runtime_tests)

# Character controller (agente 4 §4 item 54, CORE): resolução cinemática de
# movimento sobre terreno — step-up/step-down/snap, projeção de inclinação,
# água (arrasto + flutuação) — self-contained std, determinístico.
add_executable(character_controller_tests
    tests/CharacterControllerTests.cpp
    src/engine/sdk/CharacterController.cpp
)
target_include_directories(character_controller_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(character_controller_tests PRIVATE vc_build_options glm)
add_test(NAME character_controller_tests COMMAND character_controller_tests)

# Hit reaction (agente 4 §4 item 48, CORE): modelo determinístico de
# knockback/stagger/down/recovery — self-contained std, sem física.
add_executable(hit_reaction_tests
    tests/HitReactionTests.cpp
    src/engine/sdk/HitReaction.cpp
)
target_include_directories(hit_reaction_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(hit_reaction_tests PRIVATE vc_build_options glm)
add_test(NAME hit_reaction_tests COMMAND hit_reaction_tests)

# Balance (agente 4 §4 item 47, equilíbrio): fecho convexo do suporte +
# CoM dentro/na borda/fora com correção tornozelo/quadril determinística —
# self-contained std.
add_executable(balance_tests
    tests/BalanceTests.cpp
    src/engine/sdk/Balance.cpp
)
target_include_directories(balance_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(balance_tests PRIVATE vc_build_options glm)
add_test(NAME balance_tests COMMAND balance_tests)

# Hierarchical pathfinding (agente 4 §4 item 26, CORE): A* de 2 níveis sobre
# regiões (abstrato + concreto nos extremos) — self-contained std.
add_executable(hierarchical_path_tests
    tests/HierarchicalPathTests.cpp
    src/engine/sdk/HierarchicalPath.cpp
)
target_include_directories(hierarchical_path_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(hierarchical_path_tests PRIVATE vc_build_options glm)
add_test(NAME hierarchical_path_tests COMMAND hierarchical_path_tests)

# Agent capabilities (agente 4 §4 itens 25/28, CORE): perfil de agente com
# capacidades walk/jump/climb/swim/fly + verificação determinística de
# trecho — self-contained std.
add_executable(agent_capabilities_tests
    tests/AgentCapabilitiesTests.cpp
    src/engine/sdk/AgentCapabilities.cpp
)
target_include_directories(agent_capabilities_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(agent_capabilities_tests PRIVATE vc_build_options glm)
add_test(NAME agent_capabilities_tests COMMAND agent_capabilities_tests)

# Gameplay events (agente 4 §5 item 65, CORE): fila FIFO determinística de
# eventos para hooks visuais/sonoros sem acoplar — self-contained std.
add_executable(gameplay_events_tests
    tests/GameplayEventsTests.cpp
    src/engine/sdk/GameplayEvents.cpp
)
target_include_directories(gameplay_events_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(gameplay_events_tests PRIVATE vc_build_options glm)
add_test(NAME gameplay_events_tests COMMAND gameplay_events_tests)

# Effect stacks (agente 4 §5 item 64, stacking): modelo determinístico de
# efeitos acumuláveis (apply/refresh/tick/clear) — self-contained std.
add_executable(effect_stacks_tests
    tests/EffectStacksTests.cpp
    src/engine/sdk/EffectStacks.cpp
)
target_include_directories(effect_stacks_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(effect_stacks_tests PRIVATE vc_build_options glm)
add_test(NAME effect_stacks_tests COMMAND effect_stacks_tests)

# Ragdoll asset (agente 4 §4 item 55, CORE do asset configurável): skeleton
# data-driven (joints/limits/drives/auto-balance/blend) em JSON versionado
# all-or-nothing bit-exact + build_bones p/ o RagdollBone público do runtime
# — self-contained std (RegistryJson p/ o parser).
add_executable(ragdoll_asset_tests
    tests/RagdollAssetTests.cpp
    src/engine/sdk/RagdollAsset.cpp
    src/engine/sdk/RegistryJson.cpp
)
target_include_directories(ragdoll_asset_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(ragdoll_asset_tests PRIVATE vc_build_options glm)
add_test(NAME ragdoll_asset_tests COMMAND ragdoll_asset_tests)

# Async query scheduler (agente 4 §2 item 27, CORE): fila de consultas
# assíncronas com prioridade (maior vence, FIFO em empate), timeout no
# relógio do scheduler e despacho por frame com orçamento (greedy) —
# self-contained std, sem navmesh/threads (o chamador roda o lote).
add_executable(async_query_scheduler_tests
    tests/AsyncQuerySchedulerTests.cpp
    src/engine/sdk/AsyncQueryScheduler.cpp
)
target_include_directories(async_query_scheduler_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(async_query_scheduler_tests PRIVATE vc_build_options)
add_test(NAME async_query_scheduler_tests COMMAND async_query_scheduler_tests)

# Gameplay metrics (agente 4 §2 item 30, CORE): registro de métricas de
# gameplay nomeadas (counter/gauge/sample) com snapshot determinístico e
# JSON — self-contained std, sem depender do profiler de frame.
add_executable(gameplay_metrics_tests
    tests/GameplayMetricsTests.cpp
    src/engine/sdk/GameplayMetrics.cpp
)
target_include_directories(gameplay_metrics_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(gameplay_metrics_tests PRIVATE vc_build_options)
add_test(NAME gameplay_metrics_tests COMMAND gameplay_metrics_tests)

# Portal system (agente 4 §6 item 66, CORE): links de portais entre mundos
# com find (mais próximo) e resolve (offset + rotação) — self-contained std
# (RegistryJson p/ o parser), sem depender do runtime de mundo.
add_executable(portal_system_tests
    tests/PortalSystemTests.cpp
    src/engine/sdk/PortalSystem.cpp
    src/engine/sdk/RegistryJson.cpp
)
target_include_directories(portal_system_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(portal_system_tests PRIVATE vc_build_options glm)
add_test(NAME portal_system_tests COMMAND portal_system_tests)

# Day/night cycle (agente 4 §3 item 37, CORE): relógio diurno/noturno
# determinístico (dt-driven, altitude do sol, daylight factor, estado
# serializável) — self-contained std (RegistryJson p/ o parser).
add_executable(day_night_cycle_tests
    tests/DayNightCycleTests.cpp
    src/engine/sdk/DayNightCycle.cpp
    src/engine/sdk/RegistryJson.cpp
)
target_include_directories(day_night_cycle_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(day_night_cycle_tests PRIVATE vc_build_options glm)
add_test(NAME day_night_cycle_tests COMMAND day_night_cycle_tests)

# Spatial index (agente 4 §1 item 15, CORE): partição espacial uniforme de
# entidades (insert/remove/move + query AABB/ponto determinística, ids
# ordenados) — self-contained std.
add_executable(spatial_index_tests
    tests/SpatialIndexTests.cpp
    src/engine/sdk/SpatialIndex.cpp
)
target_include_directories(spatial_index_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(spatial_index_tests PRIVATE vc_build_options glm)
if(TARGET vk-bootstrap)
    add_dependencies(spatial_index_tests vk-bootstrap)
endif()
add_test(NAME spatial_index_tests COMMAND spatial_index_tests)

# Entity archetype (agente 4 §1 item 13, CORE): templates data-driven de
# entidade por kind (player/mob/vehicle/projectile/interactive) com
# componentes (tipo + JSON opaco) — self-contained std (RegistryJson).
add_executable(entity_archetype_tests
    tests/EntityArchetypeTests.cpp
    src/engine/sdk/EntityArchetype.cpp
    src/engine/sdk/RegistryJson.cpp
)
target_include_directories(entity_archetype_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(entity_archetype_tests PRIVATE vc_build_options glm)
if(TARGET vk-bootstrap)
    add_dependencies(entity_archetype_tests vk-bootstrap)
endif()
add_test(NAME entity_archetype_tests COMMAND entity_archetype_tests)

# Entity lifecycle (agente 4 §1 item 14, CORE): máquina de estados
# spawn/despawn/sleep/wake com pooling, flags de persistência e dirty
# tracking — self-contained std (RegistryJson).
add_executable(entity_lifecycle_tests
    tests/EntityLifecycleTests.cpp
    src/engine/sdk/EntityLifecycle.cpp
    src/engine/sdk/RegistryJson.cpp
)
target_include_directories(entity_lifecycle_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(entity_lifecycle_tests PRIVATE vc_build_options glm)
if(TARGET vk-bootstrap)
    add_dependencies(entity_lifecycle_tests vk-bootstrap)
endif()
add_test(NAME entity_lifecycle_tests COMMAND entity_lifecycle_tests)

# Nav invalidation (agente 4 §2 item 24, CORE): ledger de invalidação
# localizada (região → tiles + versão + retomada) — self-contained std.
add_executable(nav_invalidation_tests
    tests/NavInvalidationTests.cpp
    src/engine/sdk/NavInvalidation.cpp
)
target_include_directories(nav_invalidation_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(nav_invalidation_tests PRIVATE vc_build_options)
if(TARGET vk-bootstrap)
    add_dependencies(nav_invalidation_tests vk-bootstrap)
endif()
add_test(NAME nav_invalidation_tests COMMAND nav_invalidation_tests)

# Nav streaming (agente 4 §2 item 29, CORE): gate de regiões ativas com
# load/unload e retomada segura pós-invalidação — self-contained std.
add_executable(nav_streaming_tests
    tests/NavStreamingTests.cpp
    src/engine/sdk/NavStreaming.cpp
)
target_include_directories(nav_streaming_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(nav_streaming_tests PRIVATE vc_build_options)
if(TARGET vk-bootstrap)
    add_dependencies(nav_streaming_tests vk-bootstrap)
endif()
add_test(NAME nav_streaming_tests COMMAND nav_streaming_tests)

# AI debug recorder (agente 4 §3 item 40, CORE): snapshot por tick da
# execução de IA (nós + blackboard) p/ editor/profiling — self-contained std.
add_executable(ai_debug_info_tests
    tests/AiDebugInfoTests.cpp
    src/engine/sdk/AiDebugInfo.cpp
)
target_include_directories(ai_debug_info_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(ai_debug_info_tests PRIVATE vc_build_options)
add_test(NAME ai_debug_info_tests COMMAND ai_debug_info_tests)

# Reflection (agente 4 §1 item 50, CORE): registry de tipos/campos p/
# editor/scripting/MCP — self-contained std (RegistryJson).
add_executable(reflection_tests
    tests/ReflectionTests.cpp
    src/engine/sdk/Reflection.cpp
    src/engine/sdk/RegistryJson.cpp
)
target_include_directories(reflection_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(reflection_tests PRIVATE vc_build_options glm)
add_test(NAME reflection_tests COMMAND reflection_tests)

# Vehicle damage (agente 4 §6 item 57, CORE): modelo determinístico de dano
# por peça (destruição + destacamento por limiar) — self-contained std
# (RegistryJson p/ o parser).
add_executable(vehicle_damage_tests
    tests/VehicleDamageTests.cpp
    src/engine/sdk/VehicleDamage.cpp
    src/engine/sdk/RegistryJson.cpp
)
target_include_directories(vehicle_damage_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(vehicle_damage_tests PRIVATE vc_build_options glm)
add_test(NAME vehicle_damage_tests COMMAND vehicle_damage_tests)

# (duplicata de ability_effects_tests removida 2026-08-26 AGENT-2 — conflito
# de target com a definição do AGENT-1 l.1685 que bloqueava o configure; a
# original do AGENT-1 foi preservada. Se o AGENT-4 precisa de GameplayEvents.cpp
# no target, deve mesclá-lo na definição original.)

# Audio event mapper (agente 4 §7 item 76, CORE): mapeia eventKind de
# gameplay para triggers de áudio — self-contained std.
add_executable(audio_event_mapper_tests
    tests/AudioEventMapperTests.cpp
    src/engine/sdk/AudioEventMapper.cpp
)
target_include_directories(audio_event_mapper_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(audio_event_mapper_tests PRIVATE vc_build_options)
add_test(NAME audio_event_mapper_tests COMMAND audio_event_mapper_tests)

# Timeline policy (agente 4 §6 item 68, CORE): orçamento de retenção e
# compactação determinísticos dos estados temporais — self-contained std.
add_executable(timeline_policy_tests
    tests/TimelinePolicyTests.cpp
    src/engine/sdk/TimelinePolicy.cpp
)
target_include_directories(timeline_policy_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(timeline_policy_tests PRIVATE vc_build_options)
add_test(NAME timeline_policy_tests COMMAND timeline_policy_tests)

# Interaction component (agente 4 §1 item 26, CORE): pontos de interação
# data-driven (raio/facing/cooldown/prompt) com avaliação determinística e
# JSON all-or-nothing — self-contained std (RegistryJson p/ o parser).
add_executable(interaction_tests
    tests/InteractionTests.cpp
    src/engine/sdk/Interaction.cpp
    src/engine/sdk/RegistryJson.cpp
)
target_include_directories(interaction_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(interaction_tests PRIVATE vc_build_options glm)
add_test(NAME interaction_tests COMMAND interaction_tests)

# Crowd simulation (agente 4 §10 l.176, WIRING): multidões determinísticas
# com LOD de simulação (Full/Reduced/Aggregate/Dormant), sleeping com
# acordar, agregação distante por tipo e retomada bit-exact — composição
# dos conceitos de IAiLod + ISimulationLod para populações; self-contained
# std (RegistryJson p/ o parser).
add_executable(crowd_simulation_tests
    tests/CrowdSimulationTests.cpp
    src/engine/sdk/CrowdSimulation.cpp
    src/engine/sdk/RegistryJson.cpp
)
target_include_directories(crowd_simulation_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(crowd_simulation_tests PRIVATE vc_build_options glm)
add_test(NAME crowd_simulation_tests COMMAND crowd_simulation_tests)

# Navigation scheduler bridge (agente 4 §2 item 27, WIRING): conecta o
# escalonador de consultas assíncronas ao provider de navegação (despacho
# do lote por orçamento, coleta de resultados, cancel em voo com join) —
# self-contained std; o gate usa um mock provider determinístico.
add_executable(navigation_scheduler_bridge_tests
    tests/NavigationSchedulerBridgeTests.cpp
    src/engine/sdk/NavigationSchedulerBridge.cpp
    src/engine/sdk/AsyncQueryScheduler.cpp
)
target_include_directories(navigation_scheduler_bridge_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(navigation_scheduler_bridge_tests PRIVATE vc_build_options)
add_test(NAME navigation_scheduler_bridge_tests COMMAND navigation_scheduler_bridge_tests)

# Gameplay event router (agente 4 §5 item 75 + §2 item 30, WIRING): drena
# eventos de gameplay e roteia para sinks de áudio (IAudioEventMapper) e
# métricas (IGameplayMetrics) — self-contained std.
add_executable(gameplay_event_router_tests
    tests/GameplayEventRouterTests.cpp
    src/engine/sdk/GameplayEventRouter.cpp
    src/engine/sdk/GameplayEvents.cpp
    src/engine/sdk/AudioEventMapper.cpp
    src/engine/sdk/GameplayMetrics.cpp
)
target_include_directories(gameplay_event_router_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(gameplay_event_router_tests PRIVATE vc_build_options)
add_test(NAME gameplay_event_router_tests COMMAND gameplay_event_router_tests)

# Mission/dialogue assets (FALTANTES item 23 "missões e diálogos"): a mission
# is a data-driven JSON asset (objectives, dialogue graph, unlock conditions,
# reward) applied through the public IMissionWorld seam — deterministic,
# caller-owned progress, bit-exact state serialization. The contract compiles
# with public SDK headers only; the gate drives a deterministic mock world
# (the ability_system_tests pattern).
add_executable(mission_asset_tests
    tests/MissionAssetTests.cpp
    # MissionAsset.cpp uses the shared RegistryJson helpers; both live in
    # vc_sdk_public. This light target compiles the TUs directly (the
    # ability_system_tests pattern) so the gate needs no other SDK module.
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/MissionAsset.cpp
)
target_include_directories(mission_asset_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(mission_asset_tests PRIVATE vc_build_options glm)
add_test(NAME mission_asset_tests COMMAND mission_asset_tests)

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

# Editor panel plugin registry (ezEngine "tools as replaceable plugins" pillar):
# panels register stable metadata; the shell derives menus/palette/layout from
# the registry. Model-only (no UI/GPU) — headless test below.
add_executable(editor_plugin_tests
    tests/EditorPluginTests.cpp
    src/editor/EditorPlugin.cpp
)
target_include_directories(editor_plugin_tests PRIVATE
    src
)
target_link_libraries(editor_plugin_tests PRIVATE vc_build_options)
add_test(NAME editor_plugin_tests COMMAND editor_plugin_tests)

# Plugin sandbox isolation + type registration: headless gate for IPluginSandbox and IPluginTypeRegistry.
add_executable(plugin_sandbox_tests
    tests/PluginSandboxTests.cpp
)
target_include_directories(plugin_sandbox_tests PRIVATE
    src/engine/public
)
target_link_libraries(plugin_sandbox_tests PRIVATE vc_build_options)
add_test(NAME plugin_sandbox_tests COMMAND plugin_sandbox_tests)

# Scripting bridge: headless gate for IScriptingBridge (scripting ↔ ECS/reflection bridge).
add_executable(scripting_bridge_tests
    tests/ScriptingBridgeTests.cpp
)
target_include_directories(scripting_bridge_tests PRIVATE
    src/engine/public
)
target_link_libraries(scripting_bridge_tests PRIVATE vc_build_options)
add_test(NAME scripting_bridge_tests COMMAND scripting_bridge_tests)

# Plugin manifest + isolation: headless gate for IPluginManifest, IPluginIsolation, IPluginHotReload.
add_executable(plugin_manifest_tests
    tests/PluginManifestTests.cpp
)
target_include_directories(plugin_manifest_tests PRIVATE
    src/engine/public
)
target_link_libraries(plugin_manifest_tests PRIVATE vc_build_options)
add_test(NAME plugin_manifest_tests COMMAND plugin_manifest_tests)

# Editor project templates (ezEngine "project templates" pillar): data-driven
# scaffold generation, all-or-nothing materialization — model-only headless gate.
add_executable(project_template_tests
    tests/ProjectTemplateTests.cpp
    src/editor/ProjectTemplate.cpp
)
target_include_directories(project_template_tests PRIVATE
    src
)
target_link_libraries(project_template_tests PRIVATE vc_build_options)
if(MINGW)
    target_link_options(project_template_tests PRIVATE -static-libgcc -static-libstdc++)
endif()
add_test(NAME project_template_tests COMMAND project_template_tests)

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
    src/engine/core/uuid/UUID.cpp
    src/engine/scripting/ScriptRuntime.cpp
    src/engine/sdk/BlockEntityScripting.cpp
    src/engine/sdk/ManifoldAdapter.cpp
    tests/VoxelSdkTests.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    $<TARGET_OBJECTS:vc_manifold>
    src/engine/networking/NetworkRuntime.cpp
)
target_include_directories(voxel_sdk_tests PRIVATE
    src/engine/public
    src/engine
    ${glm_SOURCE_DIR}
    third_party/fastnoise_lite
    ${VC_COACD_DIR}/public
    ${VC_MANIFOLD_DIR}/include
)
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
    vc_efsw shlwapi
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
    src/engine/core/uuid/UUID.cpp
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
    vc_efsw shlwapi
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
    src/engine/core/uuid/UUID.cpp
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
    vc_efsw shlwapi
)
if(TARGET vc_generate_world_save)
    add_dependencies(connectivity_tests vc_generate_world_save)
endif()
add_test(NAME connectivity_tests COMMAND connectivity_tests)

# Dynamic voxel navigation tests (Agent 6 §10 item 168): build, find_path,
# incremental update, dynamic obstacles, off-mesh links, area costs, async
# queries and determinism.
add_executable(navigation_dynamic_tests
    tests/NavigationDynamicTests.cpp
    src/engine/sdk/RecastNavigation.cpp
)
target_compile_features(navigation_dynamic_tests PRIVATE cxx_std_20)
target_include_directories(navigation_dynamic_tests PRIVATE
    src/engine/public
    src
    ${glm_SOURCE_DIR}
)
target_link_libraries(navigation_dynamic_tests PRIVATE
    vc_build_options
    vc_navigation
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
)
if(TARGET vc_generate_world_save)
    add_dependencies(navigation_dynamic_tests vc_generate_world_save)
endif()
add_test(NAME navigation_dynamic_tests COMMAND navigation_dynamic_tests)

# Integrated explosions (FALTANTES §16 item 10): the ExplosionRuntime combines
# materials (blast resistance + flammability from the registry JSON), heat
# (fireball burns flammables), pressure (falloff drives the carve), impulse
# (dynamic bodies pushed by the wave) and terrain (the affected voxel box
# feeds the connectivity layer — detached islands become dynamic bodies).
add_executable(explosion_tests
    src/engine/core/uuid/UUID.cpp
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
    vc_efsw shlwapi
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
    src/engine/core/uuid/UUID.cpp
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
    vc_efsw shlwapi
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
    src/engine/core/uuid/UUID.cpp
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
    vc_efsw shlwapi
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
    src/engine/core/uuid/UUID.cpp
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
    vc_efsw shlwapi
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
    src/engine/core/uuid/UUID.cpp
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
    vc_efsw shlwapi
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
    src/engine/core/uuid/UUID.cpp
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
    vc_efsw shlwapi
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
    src/engine/core/uuid/UUID.cpp
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
    vc_efsw shlwapi
)
if(TARGET vc_generate_world_save)
    add_dependencies(tetra_cooking_tests vc_generate_world_save)
endif()
add_test(NAME tetra_cooking_tests COMMAND tetra_cooking_tests)

# Mesh cooking (META §18 / FALTANTES item 14): headless unwrap/optimize/
# simplify via the promoted meshoptimizer + xatlas clones (src/engine/sdk/
# MeshCooking.cpp is the only TU including the backends). Proves unwrap
# generates valid [0,1] UVs, optimize improves vertex-cache, simplify meets
# LOD targets, full pipeline is bit-deterministic across instances, and
# degenerate input is rejected.
add_executable(mesh_cooking_tests
    src/engine/core/uuid/UUID.cpp
    tests/MeshCookingTests.cpp
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/networking/NetworkRuntime.cpp
)
target_compile_features(mesh_cooking_tests PRIVATE cxx_std_20)
target_include_directories(mesh_cooking_tests PRIVATE
    src/engine/public
    src
    ${glm_SOURCE_DIR}
)
target_link_libraries(mesh_cooking_tests PRIVATE
    vc_build_options
    vc_ozz vc_acl
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
    vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
    vc_efsw shlwapi
)
if(TARGET vc_generate_world_save)
    add_dependencies(mesh_cooking_tests vc_generate_world_save)
endif()
add_test(NAME mesh_cooking_tests COMMAND mesh_cooking_tests)

# Text shaping (agente 2 — item `harfbuzz`): Unicode shaping core — grapheme
# clusters (UAX#29 basics), per-cluster advances, bidi runs + visual
# reorder (UAX#9 basics). Adapter src/engine/sdk/TextShaper.cpp is the only
# TU with behavior; deterministic and headless (no font rasterizer needed).
add_executable(text_shaper_tests
    src/engine/core/uuid/UUID.cpp
    tests/TextShaperTests.cpp
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/networking/NetworkRuntime.cpp
)
target_compile_features(text_shaper_tests PRIVATE cxx_std_20)
target_include_directories(text_shaper_tests PRIVATE
    src/engine/public
    src
    ${glm_SOURCE_DIR}
)
target_link_libraries(text_shaper_tests PRIVATE
    vc_build_options
    vc_ozz vc_acl
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
    vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
    vc_efsw shlwapi
)
if(TARGET vc_generate_world_save)
    add_dependencies(text_shaper_tests vc_generate_world_save)
endif()
add_test(NAME text_shaper_tests COMMAND text_shaper_tests)
if(MINGW)
    target_link_options(text_shaper_tests PRIVATE -static-libgcc -static-libstdc++)
endif()

# Ported Wicked Engine GUI BASE layer (agente 2 — item `wicked-engine`):
# src/editor/frontend/wicked/*.h (wiColor/wiCanvas/wiMath — DirectX-compatible
# types on GLM, NO DirectX). Proves the ported base WORKS (lerp, hex, DPI
# scaling, logical<->physical) — deterministic, header-only.
add_executable(wicked_base_tests
    tests/WickedBaseTests.cpp
)
target_compile_features(wicked_base_tests PRIVATE cxx_std_20)
target_include_directories(wicked_base_tests PRIVATE
    src/editor/frontend/wicked
    src/editor
    src
    ${glm_SOURCE_DIR}
)
target_link_libraries(wicked_base_tests PRIVATE
    vc_build_options
)
add_test(NAME wicked_base_tests COMMAND wicked_base_tests)

# Layered scene interchange (agente 2 — item `openusd`): USD-style layer
# stack composition with precedence and over semantics. Adapter
# src/engine/sdk/SceneLayers.cpp is the only TU with behavior.
add_executable(scene_layers_tests
    src/engine/core/uuid/UUID.cpp
    tests/SceneLayersTests.cpp
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/networking/NetworkRuntime.cpp
)
target_compile_features(scene_layers_tests PRIVATE cxx_std_20)
target_include_directories(scene_layers_tests PRIVATE
    src/engine/public
    src
    ${glm_SOURCE_DIR}
)
target_link_libraries(scene_layers_tests PRIVATE
    vc_build_options
    vc_ozz vc_acl
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
    vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
    vc_efsw shlwapi
)
if(TARGET vc_generate_world_save)
    add_dependencies(scene_layers_tests vc_generate_world_save)
endif()
add_test(NAME scene_layers_tests COMMAND scene_layers_tests)

# JSON schema validation (agente 2 — item `glaze`): validates JSON documents
# against declared schemas on the SHARED JsonMini parser (consolidation
# authority). Adapter src/engine/sdk/JsonSchema.cpp is the only TU with
# behavior; all-or-nothing, deterministic.
add_executable(json_schema_tests
    src/engine/core/uuid/UUID.cpp
    tests/JsonSchemaTests.cpp
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/networking/NetworkRuntime.cpp
)
target_compile_features(json_schema_tests PRIVATE cxx_std_20)
target_include_directories(json_schema_tests PRIVATE
    src/engine/public
    src
    ${glm_SOURCE_DIR}
)
target_link_libraries(json_schema_tests PRIVATE
    vc_build_options
    vc_ozz vc_acl
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
    vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
    vc_efsw shlwapi
)
if(TARGET vc_generate_world_save)
    add_dependencies(json_schema_tests vc_generate_world_save)
endif()
add_test(NAME json_schema_tests COMMAND json_schema_tests)
if(MINGW)
    target_link_options(json_schema_tests PRIVATE -static-libgcc -static-libstdc++)
endif()

# World-scoped replication (META §19 / FALTANTES "replicação e interesse por
# mundo"): per-world interest, world-routed edits, portal crossings as
# interest transitions, per-world client binding.
add_executable(world_replication_tests
    src/engine/core/uuid/UUID.cpp
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
    vc_efsw shlwapi
)
if(TARGET vc_generate_world_save)
    add_dependencies(world_replication_tests vc_generate_world_save)
endif()
add_test(NAME world_replication_tests COMMAND world_replication_tests)

# Persistent references between worlds (META §19 / FALTANTES "referências
# persistentes entre mundos"): stable entity ids that survive save/load +
# cross-world references stored as a reserved component on the source entity.
add_executable(world_ref_tests
    src/engine/core/uuid/UUID.cpp
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
    vc_efsw shlwapi
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
    src/engine/core/uuid/UUID.cpp
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
    vc_efsw shlwapi
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
    src/engine/core/uuid/UUID.cpp
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
    vc_efsw shlwapi
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
    src/engine/core/uuid/UUID.cpp
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
    vc_efsw shlwapi
)
if(TARGET vc_generate_world_save)
    add_dependencies(timeline_tests vc_generate_world_save)
endif()
add_test(NAME timeline_tests COMMAND timeline_tests)

# Differential engines (META §32 / FALTANTES "diferenciais próprios"):
# TimelineGraph (branches copy-on-write + causal history — the persistent
# multiverse layer) and CausalResolver (invalidation/recomputation of ONLY
# the affected descendants). Engine-own code: nothing in external/solutions
# resolves them. The contract compiles with public SDK headers only; the gate
# drives both headless + their composition (timeline causality -> resolver
# invalidation), the ability_system_tests light pattern.
add_executable(timeline_causal_tests
    tests/TimelineCausalTests.cpp
    # Both differential adapters live in vc_sdk_public; this light target
    # compiles the TUs directly so the gate needs no other SDK module.
    src/engine/sdk/TimelineGraph.cpp
    src/engine/sdk/CausalResolver.cpp
)
target_include_directories(timeline_causal_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(timeline_causal_tests PRIVATE vc_build_options glm)
add_test(NAME timeline_causal_tests COMMAND timeline_causal_tests)

# Differential engine (META §32 / FALTANTES "diferenciais próprios"):
# MacroMicroReconciler — deterministic materialization of aggregate
# consequences (the Aggregate -> Full/Coarse handoff of ISimulationLod becomes
# CONCRETE micro effects: spawns, drops, growth, removals), with a budget/
# continuation cursor, macro-change invalidation of the pending batch (the
# ICausalResolver semantics) and deterministic conflict resolution. Engine-own
# code. The contract compiles with public SDK headers only; the gate drives it
# headless (the ability_system_tests light pattern — RegistryJson + this TU).
add_executable(macro_micro_reconciler_tests
    tests/MacroMicroReconcilerTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/MacroMicroReconciler.cpp
)
target_include_directories(macro_micro_reconciler_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(macro_micro_reconciler_tests PRIVATE vc_build_options glm)
add_test(NAME macro_micro_reconciler_tests COMMAND macro_micro_reconciler_tests)

# Differential engine (META §32 / FALTANTES "diferenciais próprios"):
# WorldDirector — event selection by RULES (data-driven gates: requiresAll /
# excludesAny / cooldown / concurrency / daily limit), UTILITY (deterministic
# scoring), COHERENCE (world-state tags) and DIVERSITY (recency penalty), all
# bit-exact deterministic. Engine-own code. The contract compiles with public
# SDK headers only; the gate drives it headless (the ability_system_tests
# light pattern — RegistryJson + this TU; independent of the WorldManager WIP).
add_executable(world_director_tests
    tests/WorldDirectorTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/WorldDirector.cpp
)
target_include_directories(world_director_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(world_director_tests PRIVATE vc_build_options glm)
add_test(NAME world_director_tests COMMAND world_director_tests)

# Differential engine (META §32 / FALTANTES "diferenciais próprios"):
# EpisodeCompiler — the deterministic, signed, all-or-nothing content
# pipeline: VALIDATE, SIMULATE, TEST, COMPRESS, SIGN and PUBLISH authored
# content into a self-describing package (kind-agnostic stage hooks; codec /
# hasher seams default to identity + a deterministic content digest —
# production wires Zstandard/BLAKE3). Engine-own code. The contract compiles
# with public SDK headers only; the gate drives it headless (the
# ability_system_tests light pattern — RegistryJson + this TU).
add_executable(episode_compiler_tests
    tests/EpisodeCompilerTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/EpisodeCompiler.cpp
)
target_include_directories(episode_compiler_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(episode_compiler_tests PRIVATE vc_build_options glm)
add_test(NAME episode_compiler_tests COMMAND episode_compiler_tests)

# Differential engine (META §32 / FALTANTES "diferenciais próprios"):
# HeadlessSimulationFarm — run thousands of seeds/profiles headlessly to find
# SOFTLOCKS (stalled progress) and REPETITION (state cycles), bit-exact
# deterministic, with a progress-oracle seam and all-or-nothing refusals.
# Composes EpisodeCompiler (compiled content driven headlessly in bulk).
# Engine-own code. The contract compiles with public SDK headers only; the
# gate drives it headless (the light pattern — this TU + EpisodeCompiler TU).
add_executable(simulation_farm_tests
    tests/SimulationFarmTests.cpp
    src/engine/sdk/SimulationFarm.cpp
    src/engine/sdk/EpisodeCompiler.cpp
    src/engine/sdk/RegistryJson.cpp
)
target_include_directories(simulation_farm_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(simulation_farm_tests PRIVATE vc_build_options glm)
add_test(NAME simulation_farm_tests COMMAND simulation_farm_tests)

# Differential engine (META §32 / FALTANTES "diferenciais próprios"):
# SemanticEngineAPI — ONE surface for C++, editor, scripting, CLI and MCP:
# the deterministic kind-agnostic semantic core (schema per kind + all-or-
# nothing validation + bit-exact canonicalization). Engine-own code. The
# contract compiles with public SDK headers only; the gate drives it headless
# and composes with EpisodeCompiler (canonical documents compile/verify).
add_executable(semantic_api_tests
    tests/SemanticApiTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/SemanticApi.cpp
    src/engine/sdk/EpisodeCompiler.cpp
)
target_include_directories(semantic_api_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(semantic_api_tests PRIVATE vc_build_options glm)
add_test(NAME semantic_api_tests COMMAND semantic_api_tests)

# Integration gate for the differentials (META §32): the THREE content-
# pipeline contracts chained end-to-end — SemanticEngineAPI canonicalizes
# authored docs -> EpisodeCompiler compiles+verifies them as an episode ->
# HeadlessSimulationFarm drives the COMPILED content headlessly and finds a
# softlock in a stalling entry. Proves the differentials compose into ONE
# deterministic pipeline (one surface, one pipeline, one detector).
add_executable(semantic_episode_farm_tests
    tests/SemanticEpisodeFarmTests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/SemanticApi.cpp
    src/engine/sdk/EpisodeCompiler.cpp
    src/engine/sdk/SimulationFarm.cpp
)
target_include_directories(semantic_episode_farm_tests PRIVATE
    src/engine/public
    src
)
target_link_libraries(semantic_episode_farm_tests PRIVATE vc_build_options glm)
add_test(NAME semantic_episode_farm_tests COMMAND semantic_episode_farm_tests)

# Production-wiring gate for the EpisodeCompiler differential (#140): the
# contract spec says the codec/hash SEAMS are production-wired through the
# promoted Zstandard (engine/compression) and BLAKE3 (engine/hashing)
# adapters. This light gate compiles the REAL provider TUs (the same ones
# backing the persistent world save) and drives the pipeline through them:
# zstd-shrunk payload, BLAKE3-256 signature, verify/unpack round-trip,
# tamper detection, empty-episode boundary.
add_executable(episode_compiler_zstd_blake3_tests
    tests/EpisodeCompilerZstdBlake3Tests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/EpisodeCompiler.cpp
    $<$<BOOL:${VC_ENABLE_ZSTD}>:src/engine/core/compression/ZstdCompression.cpp>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:src/engine/core/hashing/Blake3Hash.cpp>
)
target_include_directories(episode_compiler_zstd_blake3_tests PRIVATE
    src/engine/public
    src
    $<$<BOOL:${VC_ENABLE_ZSTD}>:${VC_SOLUTIONS_DIR}/zstd/lib>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:${VC_SOLUTIONS_DIR}/blake3/c>
)
target_link_libraries(episode_compiler_zstd_blake3_tests PRIVATE
    vc_build_options glm
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
)
add_test(NAME episode_compiler_zstd_blake3_tests COMMAND episode_compiler_zstd_blake3_tests)

# Full-chain production gate (AGENT-5, findings #155): semantic -> episode ->
# farm END-TO-END with the REAL promoted seams (zstd codec + blake3 hasher).
# #147 proved the chain with defaults, #151 proved the seams on the compiler
# alone — this gate proves the chain with production wiring together.
add_executable(semantic_episode_farm_zstd_blake3_tests
    tests/SemanticEpisodeFarmZstdBlake3Tests.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/SemanticApi.cpp
    src/engine/sdk/EpisodeCompiler.cpp
    src/engine/sdk/SimulationFarm.cpp
    $<$<BOOL:${VC_ENABLE_ZSTD}>:src/engine/core/compression/ZstdCompression.cpp>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:src/engine/core/hashing/Blake3Hash.cpp>
)
target_include_directories(semantic_episode_farm_zstd_blake3_tests PRIVATE
    src/engine/public
    src
    $<$<BOOL:${VC_ENABLE_ZSTD}>:${VC_SOLUTIONS_DIR}/zstd/lib>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:${VC_SOLUTIONS_DIR}/blake3/c>
)
target_link_libraries(semantic_episode_farm_zstd_blake3_tests PRIVATE
    vc_build_options glm
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
)
add_test(NAME semantic_episode_farm_zstd_blake3_tests COMMAND semantic_episode_farm_zstd_blake3_tests)

# RegistryJson — contrato "json_parse limpa errorOut na entrada" (AGENT-5,
# findings #152): convenção dos adapters do SDK (clear na entrada); o resíduo
# stale de um parse malformado anterior contaminava o buffer reusado pelo
# caller, gerando falso-negativo em quem checa `!errorOut.empty()` após
# sucesso (causa-raiz da classe de bug dos #140/#142/#145). Gate RED->GREEN.
add_executable(registry_json_tests
    tests/RegistryJsonTests.cpp
    src/engine/sdk/RegistryJson.cpp
)
target_include_directories(registry_json_tests PRIVATE
    src/engine/public
    src/engine/sdk
    src
)
target_link_libraries(registry_json_tests PRIVATE
    vc_build_options
)
add_test(NAME registry_json_tests COMMAND registry_json_tests)

# Multi-scale streaming (META §19 / FALTANTES §15 vision "streaming
# multi-escala", Fase 5 checkbox): the world function streamed at several
# scales around a focus — fine voxel detail near, coarser height-fields far —
# all sampling the SAME generator, with power-of-two levels sharing anchors
# (cross-level coherent, lossless path-independent transition).
add_executable(multiscale_streaming_tests
    src/engine/core/uuid/UUID.cpp
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
    vc_efsw shlwapi
)
if(TARGET vc_generate_world_save)
    add_dependencies(multiscale_streaming_tests vc_generate_world_save)
endif()
add_test(NAME multiscale_streaming_tests COMMAND multiscale_streaming_tests)

# Jolt Vehicles adapter (FALTANTES §17 item 1): replaces the hand-rolled
# raycast vehicle with a real Jolt VehicleConstraint on worlds with a vehicle
# solver; builtin/bullet keep the raycast fallback.
add_executable(vehicle_adapter_tests
    src/engine/core/uuid/UUID.cpp
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
    vc_efsw shlwapi
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
    src/engine/core/uuid/UUID.cpp
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
    vc_efsw shlwapi
)
if(TARGET vc_generate_world_save)
    add_dependencies(vehicle_asset_tests vc_generate_world_save)
endif()
add_test(NAME vehicle_asset_tests COMMAND vehicle_asset_tests)

# Vehicle kinds + propulsion modules (FALTANTES §17 item 3): car / motorcycle /
# tank through the Jolt controllers (Wheeled / Motorcycle / Tracked) and the
# public wing/thruster/buoyancy propulsion modules.
add_executable(vehicle_kinds_tests
    src/engine/core/uuid/UUID.cpp
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
    vc_efsw shlwapi
)
if(TARGET vc_generate_world_save)
    add_dependencies(vehicle_kinds_tests vc_generate_world_save)
endif()
add_test(NAME vehicle_kinds_tests COMMAND vehicle_kinds_tests)

# Node/beam deformable chassis (FALTANTES §17 item 4): BeamGraphAsset + XPBD
# (the public IDeformableProvider solver) + wheel mounts on nodes.
add_executable(beam_vehicle_tests
    src/engine/core/uuid/UUID.cpp
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
    vc_efsw shlwapi
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
    src/engine/core/uuid/UUID.cpp
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
    vc_efsw shlwapi
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
    src/engine/core/uuid/UUID.cpp
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
    vc_efsw shlwapi
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
    src/engine/core/uuid/UUID.cpp
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
    vc_efsw shlwapi
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
    src/engine/core/uuid/UUID.cpp
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
    vc_efsw shlwapi
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
    src/engine/core/uuid/UUID.cpp
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
    vc_efsw shlwapi
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
    src/engine/core/uuid/UUID.cpp
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
    vc_efsw shlwapi
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
    src/engine/core/uuid/UUID.cpp
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
    vc_efsw shlwapi
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
    src/engine/core/uuid/UUID.cpp
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
    vc_efsw shlwapi
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
    src/engine/sdk/RegistryJson.cpp
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
    src/engine/sdk/RegistryJson.cpp
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
    src/engine/core/uuid/UUID.cpp
    tests/FootPlacementTests.cpp
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_voxel_runtime>
    $<TARGET_OBJECTS:vc_voxel_generation>
    src/engine/networking/NetworkRuntime.cpp
)
target_link_libraries(foot_placement_tests PRIVATE vc_efsw shlwapi)  # vc_sdk_public carries FileWatcher.cpp (efsw) — BUG-025
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
    src/engine/sdk/RegistryJson.cpp
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






# ===========================================================================
# RESTAURADO (P0 2026-08-26 AGENT-2): os targets de app + módulos de
# app/engine/editor/feature/material foram removidos do CMakeLists pelo
# refactor alheio que o AGENT-1 consertou (#263 — ele restaurou os gates dele
# e consolidou vc_advanced_runtime → vc_gameplay_sdk, mas os apps e módulos
# abaixo ficaram de fora). Fontes intactos no tree; restaurados do HEAD com
# vc_advanced_runtime → vc_gameplay_sdk. Sem isso o próximo configure fresh
# perderia VulkanEngineGame/Editor/Server + vulkan_craft.
# ===========================================================================
vc_object_module(vc_material_pipeline
    ${CMAKE_SOURCE_DIR}/src/engine/rendering/vulkan/VmaImplementation.cpp
    src/engine/rendering/vulkan/MaterialPipeline.cpp
    src/engine/rendering/vulkan/GpuRenderFeatures.cpp
    src/engine/rendering/vulkan/GpuFeaturePasses.cpp
)
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

# Regression gate for the editor's world grid. It rejects perspective-driven
# decade switching, which creates visible density/color bands in the viewport.
find_program(VC_NODE_EXECUTABLE NAMES node node.exe)
if(VC_NODE_EXECUTABLE)
    add_test(NAME editor_grid_shader_tests
        COMMAND "${VC_NODE_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/EditorGridShaderTests.mjs")
    set_tests_properties(editor_grid_shader_tests PROPERTIES
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_test(NAME editor_viewport_regression_tests
        COMMAND "${VC_NODE_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/EditorViewportRegressionTests.mjs")
    set_tests_properties(editor_viewport_regression_tests PROPERTIES
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()

add_executable(block_texture_atlas_tests
    tests/BlockTextureAtlasTests.cpp
    src/editor/BlockTextureAtlas.cpp
)
target_compile_features(block_texture_atlas_tests PRIVATE cxx_std_20)
target_include_directories(block_texture_atlas_tests PRIVATE src)
add_test(NAME block_texture_atlas_tests COMMAND block_texture_atlas_tests)

add_executable(VulkanEngineGame
    src/app/main_game.cpp
    src/app/VulkanGameBootstrap.cpp
    src/app/VulkanGameWorld.cpp
    src/app/VulkanGameRender.cpp
    src/app/VulkanGameGameplay.cpp
    src/app/VulkanGamePresentation.cpp
    # Runtime TUs do gameplay (play mode usa ScriptVM/ScriptCompiler e o audio
    # callback usa Mixer) — restaurados com o target (rewrite 08:44).
    src/engine/audio/AudioRuntime.cpp
    src/engine/scripting/ScriptRuntime.cpp
    src/engine/scripting/ScriptDebugger.cpp
    src/engine/scripting/ScriptGraphBridge.cpp
    src/engine/scripting/VisualScriptCanvas.cpp
    src/engine/scripting/VisualScriptGraph.cpp
    src/engine/networking/NetworkRuntime.cpp
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_core_runtime>
    $<$<BOOL:${VC_ENABLE_VOXEL_PLUGIN}>:$<TARGET_OBJECTS:vc_voxel_generation>>
    $<$<BOOL:${VC_ENABLE_VOXEL_PLUGIN}>:$<TARGET_OBJECTS:vc_voxel_runtime>>
    $<TARGET_OBJECTS:vc_gameplay_sdk>
)
target_compile_features(VulkanEngineGame PRIVATE cxx_std_20)
target_include_directories(VulkanEngineGame PRIVATE src ${glm_SOURCE_DIR})
target_link_libraries(VulkanEngineGame PRIVATE
    vc_build_options glfw glm vk-bootstrap Vulkan::Vulkan windowscodecs ole32 ws2_32 vc_material_pipeline vc_navigation vc_ozz vc_acl
    vc_fastwfc vc_meshoptimizer vc_xatlas
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
    vc_efsw shlwapi
)

if(VC_ENABLE_VOXEL_PLUGIN)
add_executable(vulkan_craft
    src/app/main.cpp
    src/plugins/voxel/VoxelPlugin.cpp
    src/engine/sdk/EntityWorld.cpp
    src/engine/sdk/MobBehavior.cpp
    src/engine/sdk/RegistryJson.cpp
    src/engine/sdk/BeamGraphAsset.cpp
    src/engine/sdk/VehicleProvider.cpp
    src/engine/sdk/MotionDatabase.cpp
    src/engine/sdk/XpbdDeformable.cpp
    src/engine/sdk/VehicleAsset.cpp
    $<TARGET_OBJECTS:vc_core_runtime>
    $<$<BOOL:${VC_ENABLE_VOXEL_PLUGIN}>:$<TARGET_OBJECTS:vc_voxel_generation>>
    $<$<BOOL:${VC_ENABLE_VOXEL_PLUGIN}>:$<TARGET_OBJECTS:vc_voxel_runtime>>
    $<TARGET_OBJECTS:vc_gameplay_sdk>
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
    src/editor/EditorApplicationPanels.cpp
    src/editor/EditorApplicationVulkan.cpp
    src/editor/EditorApplicationPlay.cpp
    src/editor/EditorApplicationAssets.cpp
    src/editor/EditorApplicationBootstrap.cpp
    src/editor/EditorVisualPanels.cpp
    src/editor/BlockTextureAtlas.cpp
    src/editor/EditorControlApi.cpp
    src/editor/EditorLayout.cpp
    src/editor/EditorPlugin.cpp
    src/editor/ProjectTemplate.cpp
    src/engine/editor/ui/EditorGUI.cpp
    src/editor/tools/SpecializedEditorsPanel.cpp
    src/editor/tools/WickedToolsPanel.cpp
    src/editor/frontend/ForgeTheme.cpp
    src/editor/frontend/ForgeWidgets.cpp
    $<$<BOOL:${VC_ENABLE_VOXEL_PLUGIN}>:src/engine/audio/SoundEngine.cpp>
    $<$<BOOL:${VC_ENABLE_VOXEL_PLUGIN}>:src/plugins/voxel/VoxelPlugin.cpp>
    # Runtime TUs que o editor consome (play mode/scripting/audio/gameplay) —
    # restaurados com o target (rewrite 08:44 removeu os apps + módulos).
    src/engine/audio/AudioRuntime.cpp
    src/engine/gameplay/ParticleSimulation.cpp
    src/engine/gameplay/MissionSystem.cpp
    src/engine/gameplay/DialogueSystem.cpp
    src/engine/scripting/ScriptRuntime.cpp
    src/engine/scripting/ScriptDebugger.cpp
    src/engine/scripting/ScriptGraphBridge.cpp
    src/engine/scripting/VisualScriptCanvas.cpp
    src/engine/scripting/VisualScriptGraph.cpp
    src/engine/networking/NetworkRuntime.cpp
    ${IMGUI_SOURCES}
    $<TARGET_OBJECTS:vc_sdk_public>
    $<TARGET_OBJECTS:vc_core_runtime>
    $<$<BOOL:${VC_ENABLE_VOXEL_PLUGIN}>:$<TARGET_OBJECTS:vc_voxel_generation>>
    $<$<BOOL:${VC_ENABLE_VOXEL_PLUGIN}>:$<TARGET_OBJECTS:vc_voxel_runtime>>
    $<TARGET_OBJECTS:vc_gameplay_sdk>
    $<TARGET_OBJECTS:vc_editor_extensions>
)
target_include_directories(VulkanEngineEditor PRIVATE
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    src/editor
    src/editor/frontend/wicked
    ${VC_SOLUTIONS_DIR}/recast-navigation/Recast/Include
    ${VC_SOLUTIONS_DIR}/recast-navigation/Detour/Include
)
target_link_libraries(VulkanEngineEditor PRIVATE
    vc_build_options glfw glm vk-bootstrap Vulkan::Vulkan windowscodecs ole32 ws2_32 vc_material_pipeline vc_navigation vc_ozz vc_acl
    vc_efsw shlwapi
    vc_fastwfc vc_meshoptimizer vc_xatlas
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
)
# EditorApplication.cpp aggregates the complete editor surface and can exceed
# MSVC's default COFF section cap in optimized builds.
if(MSVC)
    target_compile_options(VulkanEngineEditor PRIVATE /bigobj)
endif()
add_dependencies(VulkanEngineEditor compile_shaders)

add_executable(VulkanEngineServer
    src/server/main_server.cpp
    src/engine/networking/NetworkRuntime.cpp
    $<TARGET_OBJECTS:vc_core_runtime>
    $<TARGET_OBJECTS:vc_gameplay_sdk>
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
    glm bullet
    $<$<BOOL:${VC_ENABLE_ZSTD}>:vc_zstd>
    $<$<BOOL:${VC_ENABLE_BLAKE3}>:vc_blake3>
    $<$<BOOL:${VC_ENABLE_FLATBUFFERS}>:vc_flatbuffers>
    $<$<BOOL:${VC_ENABLE_ROCKSDB}>:vc_rocksdb>
    vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas
)
if(TARGET vc_generate_world_save)
    add_dependencies(VulkanEngineServer vc_generate_world_save)
endif()

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
# FALTANTES item 11 / §24 + task_plan Agente 5 §1 — project TEMPLATES in the
# SDK package: the external-consumer scaffolds (tools/external-project*), each
# a self-contained consumer whose ONLY dependency is find_package(vulkan_craft_sdk
# CONFIG). Shipping them means a user can scaffold a new project straight from
# the INSTALLED SDK (no engine tree), which the moved-prefix gate verifies
# (tools/portability/moved-prefix-gate.mjs builds a consumer scaffolded from
# the installed copy, findings #183).
install(DIRECTORY tools/gameplay-consumer tools/editor-gameplay-consumer DESTINATION templates)
install(FILES src/engine/public/engine/gameplay/IGameplayIntegration.hpp
              src/engine/public/engine/gameplay/IGameplayDebugSurface.hpp
              src/engine/public/engine/gameplay/IGameplayCrossDomain.hpp
              src/engine/public/engine/gameplay/IGameplayPhase.hpp
              src/engine/public/engine/gameplay/IGameplayBindings.hpp
              src/engine/public/engine/gameplay/IGameplaySystemWiring.hpp
        DESTINATION include/engine/gameplay)
install(DIRECTORY tools/external-project tools/external-project-empty
    tools/external-project-advanced tools/external-project-differentials
    tools/external-project-sdk-surface
    DESTINATION templates FILES_MATCHING PATTERN "*")
install(FILES docs/SDK_MANIFEST.md DESTINATION . RENAME SDK.md)
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
    $<TARGET_OBJECTS:vc_core_runtime>
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

# The engine's jolt target is renamed vulkan_jolt (its project no longer
# case-collides with the vendored Jolt.vcxproj in the .slnx — MSB4025) and its
# OUTPUT_NAME is pinned to Jolt so the installed archive name stays the
# deterministic Jolt.lib consumers already link.
set_target_properties(vulkan_jolt PROPERTIES OUTPUT_NAME Jolt)

# The vc_sdk archive links vc_sdk_public, which includes the storage TUs
# (zstd/blake3/rocksdb/flatbuffers) unconditionally — it cannot be built when
# those optional deps are OFF (e.g. the clean-machine gate's reduced configure).
# Install it OPTIONAL so partial builds stage everything else and skip this lib
# silently; the workspace build always produces it.
#
# task_plan Agente 5 §1 item 6 (compatibilidade debug/release): every SDK
# archive is staged TWICE — non-Debug configs (Release default) into lib/ and
# Debug into lib/Debug/. The package config (vulkan_craft_sdk-config.cmake)
# selects per consumer config, so a Debug consumer links the /MDd archives and
# a Release consumer the /MD archives (MSVC runtime compatibility — the old
# single lib/ location silently mixed /MDd into /MD consumers and vice-versa).
macro(vc_install_sdk_lib vc_lib)
    if(TARGET ${vc_lib})
        # $<TARGET_FILE:...> resolves the real archive path (subdirectory
        # builds like rocksdb/flatbuffers land outside the top-level Release
        # dir); OPTIONAL skips silently when a partial build defined the
        # target but did not build its archive (clean-machine gate, deps OFF).
        install(FILES "$<TARGET_FILE:${vc_lib}>" DESTINATION lib OPTIONAL
            CONFIGURATIONS Release MinSizeRel RelWithDebInfo Shipping)
        install(FILES "$<TARGET_FILE:${vc_lib}>" DESTINATION lib/Debug OPTIONAL
            CONFIGURATIONS Debug)
    endif()
endmacro()

vc_install_sdk_lib(vc_sdk)
foreach(vc_lib vc_zstd vc_blake3 vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas)
    vc_install_sdk_lib(${vc_lib})
endforeach()
foreach(vc_lib flatbuffers rocksdb vulkan_jolt bullet)
    vc_install_sdk_lib(${vc_lib})
endforeach()
# glm headers: the public contracts include <glm/...>, so the consumer needs
# the same header-only library voxel_sdk_tests compiles against.
install(DIRECTORY ${glm_SOURCE_DIR}/glm DESTINATION include)

# Relocatable package config: every path resolves from this file's location, so
# the staged prefix can move or be consumed from another checkout/machine.
set(VULKAN_CRAFT_SDK_LINK_LIBS "")
foreach(vc_lib vc_zstd vc_blake3 vc_navigation vc_fastwfc vc_meshoptimizer vc_xatlas flatbuffers rocksdb vulkan_jolt bullet)
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
# Test labels for suite-based gating (Agent 6)
if(VC_BUILD_TEST_LABELS)
    include(cmake/TestLabels.cmake)
endif()

# GoogleTest smoke test — proves external solution integration
add_library(gtest_main STATIC
    ${VC_SOLUTIONS_DIR}/googletest/googletest/src/gtest-all.cc
    ${VC_SOLUTIONS_DIR}/googletest/googletest/src/gtest_main.cc)
target_include_directories(gtest_main PUBLIC
    ${VC_SOLUTIONS_DIR}/googletest/googletest/include
    ${VC_SOLUTIONS_DIR}/googletest/googletest)
target_compile_features(gtest_main PUBLIC cxx_std_14)

add_executable(gtest_smoke_test
    tests/GtestSmokeTest.cpp)
target_link_libraries(gtest_smoke_test PRIVATE
    vc_build_options
    gtest_main
    glm)
target_include_directories(gtest_smoke_test PRIVATE
    src/engine/public
    src)
include(GoogleTest)
gtest_discover_tests(gtest_smoke_test)

# Google Benchmark smoke test — proves external solution integration
file(GLOB GENCHMARK_SOURCES ${VC_SOLUTIONS_DIR}/google-benchmark/src/*.cc)
list(REMOVE_ITEM GENCHMARK_SOURCES
    ${VC_SOLUTIONS_DIR}/google-benchmark/src/benchmark_main.cc)
add_library(gbenchmark_main STATIC ${GENCHMARK_SOURCES})
target_include_directories(gbenchmark_main PUBLIC
    ${VC_SOLUTIONS_DIR}/google-benchmark/include
    ${VC_SOLUTIONS_DIR}/google-benchmark/src)
target_compile_definitions(gbenchmark_main PRIVATE
    Benchmark_ENABLE_TESTING=0
    Benchmark_ENABLE_GTEST_TESTS=0
    BENCHMARK_STATIC_DEFINE)
target_compile_features(gbenchmark_main PUBLIC cxx_std_14)

add_executable(benchmark_smoke_test
    tests/BenchmarkSmokeTest.cpp)
target_link_libraries(benchmark_smoke_test PRIVATE
    gbenchmark_main
    shlwapi)
target_compile_definitions(benchmark_smoke_test PRIVATE BENCHMARK_STATIC_DEFINE)
target_include_directories(benchmark_smoke_test PRIVATE
    src)
add_test(NAME benchmark_smoke_test COMMAND benchmark_smoke_test --benchmark_min_time=0.01s)

# Public job-system integration test. This is intentionally a small target so
# job API changes do not rebuild the application targets.
add_executable(job_system_tests tests/JobSystemTests.cpp)
target_link_libraries(job_system_tests PRIVATE vc_sdk_public vc_build_options)
target_include_directories(job_system_tests PRIVATE src/engine/public src)
add_test(NAME job_system_tests COMMAND job_system_tests)
if(VC_BUILD_TEST_LABELS)
    set_tests_properties(job_system_tests PROPERTIES LABELS "unit;integration;headless")
endif()

# Real engine benchmark against the public SDK (IHashProvider/BLAKE3) —
# §7/114 baseline pattern: consumer real + configuração + teste + evidência.
add_executable(benchmark_engine_test
    tests/BenchmarkEngineTest.cpp
    src/engine/core/hashing/Blake3Hash.cpp)
target_link_libraries(benchmark_engine_test PRIVATE
    gbenchmark_main
    vc_blake3
    shlwapi)
target_compile_definitions(benchmark_engine_test PRIVATE BENCHMARK_STATIC_DEFINE)
target_include_directories(benchmark_engine_test PRIVATE
    src
    src/engine/public)
add_test(NAME benchmark_engine_test COMMAND benchmark_engine_test --benchmark_min_time=0.01s)

# A/B gate (item 144) for §7/111: std::deque vs concurrentqueue on this HW.
add_executable(benchmark_queue_ab_test
    tests/BenchmarkQueueAbTest.cpp)
target_link_libraries(benchmark_queue_ab_test PRIVATE
    vc_build_options
    gbenchmark_main
    shlwapi)
target_compile_definitions(benchmark_queue_ab_test PRIVATE BENCHMARK_STATIC_DEFINE)
target_include_directories(benchmark_queue_ab_test PRIVATE
    ${VC_SOLUTIONS_DIR}/concurrentqueue)
add_test(NAME benchmark_queue_ab_test COMMAND benchmark_queue_ab_test --benchmark_min_time=0.01s)

# mimalloc static lib for the A/B allocator gate (§7/116, item 144).
# Core 18 TUs from the promoted solution; linked explicitly (mi_malloc/mi_free)
# — never overrides the process allocator, keeping the comparison isolated.
add_library(vc_mimalloc_ab STATIC
    ${VC_SOLUTIONS_DIR}/mimalloc/src/alloc-aligned.c
    ${VC_SOLUTIONS_DIR}/mimalloc/src/alloc-posix.c
    ${VC_SOLUTIONS_DIR}/mimalloc/src/alloc.c
    ${VC_SOLUTIONS_DIR}/mimalloc/src/arena-meta.c
    ${VC_SOLUTIONS_DIR}/mimalloc/src/arena.c
    ${VC_SOLUTIONS_DIR}/mimalloc/src/bitmap.c
    ${VC_SOLUTIONS_DIR}/mimalloc/src/heap.c
    ${VC_SOLUTIONS_DIR}/mimalloc/src/init.c
    ${VC_SOLUTIONS_DIR}/mimalloc/src/libc.c
    ${VC_SOLUTIONS_DIR}/mimalloc/src/options.c
    ${VC_SOLUTIONS_DIR}/mimalloc/src/os.c
    ${VC_SOLUTIONS_DIR}/mimalloc/src/page-map.c
    ${VC_SOLUTIONS_DIR}/mimalloc/src/page.c
    ${VC_SOLUTIONS_DIR}/mimalloc/src/random.c
    ${VC_SOLUTIONS_DIR}/mimalloc/src/static.c
    ${VC_SOLUTIONS_DIR}/mimalloc/src/stats.c
    ${VC_SOLUTIONS_DIR}/mimalloc/src/theap.c
    ${VC_SOLUTIONS_DIR}/mimalloc/src/threadlocal.c)
target_include_directories(vc_mimalloc_ab PUBLIC ${VC_SOLUTIONS_DIR}/mimalloc/include)
target_compile_definitions(vc_mimalloc_ab PRIVATE MI_STATIC_LIB)

add_executable(benchmark_alloc_ab_test
    tests/BenchmarkAllocAbTest.cpp)
target_link_libraries(benchmark_alloc_ab_test PRIVATE
    vc_build_options
    gbenchmark_main
    vc_mimalloc_ab
    shlwapi)
target_compile_definitions(benchmark_alloc_ab_test PRIVATE BENCHMARK_STATIC_DEFINE MI_STATIC_LIB)
target_include_directories(benchmark_alloc_ab_test PRIVATE
    ${VC_SOLUTIONS_DIR}/mimalloc/include)
add_test(NAME benchmark_alloc_ab_test COMMAND benchmark_alloc_ab_test --benchmark_min_time=0.01s)

# A/B gate (item 144) for §7/118: ad-hoc std::thread vs taskflow scheduler.
add_executable(benchmark_sched_ab_test
    tests/BenchmarkSchedAbTest.cpp)
target_link_libraries(benchmark_sched_ab_test PRIVATE
    vc_build_options
    gbenchmark_main
    shlwapi)
target_compile_definitions(benchmark_sched_ab_test PRIVATE BENCHMARK_STATIC_DEFINE)
target_include_directories(benchmark_sched_ab_test PRIVATE
    ${VC_SOLUTIONS_DIR}/taskflow)
add_test(NAME benchmark_sched_ab_test COMMAND benchmark_sched_ab_test --benchmark_min_time=0.01s)

# A/B gate (item 144) for §7/117: fprintf vs spdlog sync/async (header-only).
add_executable(benchmark_log_ab_test
    tests/BenchmarkLogAbTest.cpp)
target_link_libraries(benchmark_log_ab_test PRIVATE
    vc_build_options
    gbenchmark_main
    shlwapi)
target_compile_definitions(benchmark_log_ab_test PRIVATE BENCHMARK_STATIC_DEFINE SPDLOG_HEADER_ONLY)
target_compile_options(benchmark_log_ab_test PRIVATE /utf-8)  # spdlog bundled fmt requires it on MSVC
target_include_directories(benchmark_log_ab_test PRIVATE
    ${VC_SOLUTIONS_DIR}/spdlog/include)
add_test(NAME benchmark_log_ab_test COMMAND benchmark_log_ab_test --benchmark_min_time=0.01s)

# A/B gate (item 144) for §7/119: tracy per-zone overhead (client enabled,
# no profiler connected) — decides safe adoption of profiling zones.
add_library(vc_tracy_client STATIC
    ${VC_SOLUTIONS_DIR}/tracy/public/TracyClient.cpp)
target_include_directories(vc_tracy_client PUBLIC ${VC_SOLUTIONS_DIR}/tracy/public)
target_compile_definitions(vc_tracy_client PRIVATE TRACY_ENABLE)

add_executable(benchmark_tracy_ab_test
    tests/BenchmarkTracyAbTest.cpp)
target_link_libraries(benchmark_tracy_ab_test PRIVATE
    vc_build_options
    gbenchmark_main
    vc_tracy_client
    shlwapi)
target_compile_definitions(benchmark_tracy_ab_test PRIVATE BENCHMARK_STATIC_DEFINE TRACY_ENABLE)
target_include_directories(benchmark_tracy_ab_test PRIVATE
    ${VC_SOLUTIONS_DIR}/tracy/public)
add_test(NAME benchmark_tracy_ab_test COMMAND benchmark_tracy_ab_test --benchmark_min_time=0.01s)

# slerp diag
add_executable(xr_diag2 tests/xr_diag2.cpp src/engine/sdk/XrMath.cpp)
target_include_directories(xr_diag2 PRIVATE src/engine/public src/engine/sdk)
target_link_libraries(xr_diag2 PRIVATE vc_build_options)

# Gameplay integration manifest gate: validates every owned domain and all 13
# external adapters through the public SDK contract.
add_executable(gameplay_bindings_tests
    tests/GameplayBindingsTests.cpp
    $<TARGET_OBJECTS:vc_gameplay_sdk>)
target_include_directories(gameplay_bindings_tests PRIVATE src/engine/public src)
target_link_libraries(gameplay_bindings_tests PRIVATE vc_build_options)
add_test(NAME gameplay_bindings_tests COMMAND gameplay_bindings_tests)

add_executable(gameplay_system_wiring_tests
    tests/GameplaySystemWiringTests.cpp
    $<TARGET_OBJECTS:vc_gameplay_sdk>)
target_include_directories(gameplay_system_wiring_tests PRIVATE src/engine/public src)
target_link_libraries(gameplay_system_wiring_tests PRIVATE vc_build_options)
add_test(NAME gameplay_system_wiring_tests COMMAND gameplay_system_wiring_tests)
