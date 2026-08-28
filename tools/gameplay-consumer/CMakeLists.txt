cmake_minimum_required(VERSION 3.20)
project(gameplay_consumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
find_package(vulkan_craft_sdk CONFIG REQUIRED)
add_executable(gameplay_consumer main.cpp)
# Links the installed SDK's exported target. The package config exports the
# plain imported target `vulkan_craft_sdk` (see cmake/vulkan_craft_sdk-config.cmake),
# not a namespaced alias.
target_link_libraries(gameplay_consumer PRIVATE vulkan_craft_sdk)
