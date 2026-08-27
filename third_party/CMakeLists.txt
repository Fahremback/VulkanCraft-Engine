# Vendored production libraries, compiled as static libs.
# Jolt Physics: canonical source list from Jolt.cmake (optional subsystems like
# Renderer/Shaders/Compute compile out to stubs without their defines).
# Bullet: the official amalgamated translation units (3 TUs).

# ---- Jolt Physics ----
set(PHYSICS_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/jolt)
include(${CMAKE_CURRENT_SOURCE_DIR}/jolt/Jolt/Jolt.cmake)
# The vendored Jolt.cmake creates a `Jolt` target that the engine does NOT
# link (the engine builds Jolt once as its own target below, with its own
# compile definitions). Left as-is it would (a) write Jolt.lib racing the
# engine's target and (b) generate Jolt.vcxproj whose name case-collides with
# the engine target's project in the .slnx (MSB4025). Nothing links Jolt::Jolt,
# so keep the vendored target out of the default build and give it a distinct
# output name; the engine target remains the sole producer of Jolt.lib.
set_target_properties(Jolt PROPERTIES
    OUTPUT_NAME JoltVendored
    EXCLUDE_FROM_ALL TRUE
)
set(JOLT_CPP_SOURCES)
foreach(source IN LISTS JOLT_PHYSICS_SRC_FILES)
    if(source MATCHES "\\.cpp$")
        list(APPEND JOLT_CPP_SOURCES ${source})
    endif()
endforeach()
add_library(vulkan_jolt STATIC ${JOLT_CPP_SOURCES})
target_include_directories(vulkan_jolt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/jolt)
target_compile_features(vulkan_jolt PRIVATE cxx_std_17)
# Jolt checks JPH_ENABLE_ASSERTS with #ifdef, so defining it to 0 still
# enables asserts (and changes FixedSizeFreeList layout -> ABI mismatch with
# consumers that compile without the define). Match Jolt's own Jolt.cmake
# convention: define it (no value) for Debug only, leave it undefined for
# non-Debug so asserts are off and the layout matches consumers.
target_compile_definitions(vulkan_jolt PRIVATE
    $<$<CONFIG:Debug>:JPH_ENABLE_ASSERTS>
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
