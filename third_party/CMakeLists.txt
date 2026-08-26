# Vendored production libraries, compiled as static libs.
# Jolt Physics: canonical source list from Jolt.cmake (optional subsystems like
# Renderer/Shaders/Compute compile out to stubs without their defines).
# Bullet: the official amalgamated translation units (3 TUs).

# ---- Jolt Physics ----
set(PHYSICS_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/jolt)
include(${CMAKE_CURRENT_SOURCE_DIR}/jolt/Jolt/Jolt.cmake)
set(JOLT_CPP_SOURCES)
foreach(source IN LISTS JOLT_PHYSICS_SRC_FILES)
    if(source MATCHES "\\.cpp$")
        list(APPEND JOLT_CPP_SOURCES ${source})
    endif()
endforeach()
# The target is named vulkan_jolt (NOT jolt): the vendored Jolt.cmake above
# creates add_library(Jolt ...) which on Windows generates third_party/Jolt.vcxproj;
# a target named jolt would generate third_party/jolt.vcxproj — case-insensitive
# collision in the .slnx (MSB4025 duplicate item). The engine target is distinct;
# OUTPUT_NAME stays Jolt so the installed archive remains lib/Jolt.lib.
add_library(vulkan_jolt STATIC ${JOLT_CPP_SOURCES})
target_include_directories(vulkan_jolt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/jolt)
target_compile_features(vulkan_jolt PRIVATE cxx_std_17)
target_compile_definitions(vulkan_jolt PRIVATE
    $<$<CONFIG:Debug>:JPH_ENABLE_ASSERTS=1>
    $<$<NOT:$<CONFIG:Debug>>:JPH_ENABLE_ASSERTS=0>
)
if(MSVC)
    target_compile_options(vulkan_jolt PRIVATE /w)
endif()

# ---- Bullet (amalgamated) ----
add_library(bullet STATIC
    bullet/src/btLinearMathAll.cpp
    bullet/src/btBulletCollisionAll.cpp
    bullet/src/btBulletDynamicsAll.cpp
)
target_include_directories(bullet PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/bullet/src)
target_compile_features(bullet PRIVATE cxx_std_17)
# On MSVC Bullet auto-enables BT_USE_SSE but leaves BT_USE_SSE_IN_API off, which
# breaks the __m128 conversion operators in btVector3/btMatrix3x3/btQuaternion.
target_compile_definitions(bullet PRIVATE BT_USE_SSE_IN_API=1)
if(MSVC)
    # /fp:fast breaks the Bullet solver on MSVC (NaN explosions on contact);
    # keep the library on precise semantics like its reference builds.
    target_compile_options(bullet PRIVATE /w /fp:precise)
endif()
