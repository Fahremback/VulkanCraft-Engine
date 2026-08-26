# vulkan_craft_sdk — relocatable package config (FALTANTES item 11 / §24).
#
# Every path resolves relative to this file's install location, so the staged
# prefix can move to another directory or machine (same OS/toolchain) without
# re-editing. No absolute paths from the engine build are embedded.
#
# Usage in an external project:
#   find_package(vulkan_craft_sdk CONFIG REQUIRED)
#   target_link_libraries(my_app PRIVATE vulkan_craft_sdk)
#
# The imported target exposes include/ (engine/ + glm/) and the full static
# link set (vc_sdk archive + promoted dependency libs), mirroring exactly what
# voxel_sdk_tests links in-tree.

# This file lives at <prefix>/lib/cmake/vulkan_craft_sdk/ — three levels up
# is the prefix itself.
get_filename_component(VULKAN_CRAFT_SDK_PREFIX
    "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

set(VULKAN_CRAFT_SDK_INCLUDE_DIRS "${VULKAN_CRAFT_SDK_PREFIX}/include")

# The archive first, then every static dependency lib (names substituted at
# engine configure time; paths resolved against the prefix at find time so the
# prefix is relocatable).
set(VULKAN_CRAFT_SDK_LIBRARIES
  "${VULKAN_CRAFT_SDK_PREFIX}/lib/vc_sdk.lib")
foreach(_vc_dep vc_zstd.lib;vc_blake3.lib;vc_navigation.lib;vc_fastwfc.lib;vc_meshoptimizer.lib;vc_xatlas.lib;flatbuffers.lib;rocksdb.lib;Jolt.lib;bullet.lib)
    list(APPEND VULKAN_CRAFT_SDK_LIBRARIES "${VULKAN_CRAFT_SDK_PREFIX}/lib/${_vc_dep}")
endforeach()

if(NOT TARGET vulkan_craft_sdk)
    add_library(vulkan_craft_sdk STATIC IMPORTED)
    set_target_properties(vulkan_craft_sdk PROPERTIES
        IMPORTED_LOCATION "${VULKAN_CRAFT_SDK_PREFIX}/lib/vc_sdk.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${VULKAN_CRAFT_SDK_INCLUDE_DIRS}"
        INTERFACE_LINK_LIBRARIES "${VULKAN_CRAFT_SDK_LIBRARIES}")
endif()

set(VULKAN_CRAFT_SDK_FOUND TRUE)
