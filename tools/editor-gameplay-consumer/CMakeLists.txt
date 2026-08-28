cmake_minimum_required(VERSION 3.20)
project(editor_gameplay_consumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
find_package(vulkan_craft_sdk CONFIG REQUIRED)
add_executable(editor_gameplay_consumer main.cpp)
# The installed SDK exports the plain imported target `vulkan_craft_sdk`
# (cmake/vulkan_craft_sdk-config.cmake) — no namespaced alias.
target_link_libraries(editor_gameplay_consumer PRIVATE vulkan_craft_sdk)
