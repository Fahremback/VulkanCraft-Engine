# External "advanced" consumer (FALTANTES item 11 / §24 — matrix): the ONLY
# dependency is the installed VulkanCraft SDK (find_package CONFIG resolves the
# relocatable config under <prefix>/lib/cmake/vulkan_craft_sdk). Nothing here
# references the engine source tree — configure with -DCMAKE_PREFIX_PATH=<prefix>.
#
# This project composes the higher-level public contracts: multi-world +
# portals (IWorldManager) and gameplay destruction (IGameplayRuntime).
cmake_minimum_required(VERSION 3.20)
project(vulkan_craft_advanced LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(vulkan_craft_sdk CONFIG REQUIRED)

add_executable(advanced_consumer main.cpp)
target_link_libraries(advanced_consumer PRIVATE vulkan_craft_sdk)
