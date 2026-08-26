# External "differentials" consumer (FALTANTES item 11 / §24 — matrix): the
# ONLY dependency is the installed VulkanCraft SDK (find_package CONFIG
# resolves the relocatable config under <prefix>/lib/cmake/vulkan_craft_sdk).
# Nothing here references the engine source tree — configure with
# -DCMAKE_PREFIX_PATH=<prefix>.
#
# This project compiles + RUNS the META §32 content-pipeline differentials
# through their PUBLIC contracts only: TimelineGraph, CausalResolver,
# MacroMicroReconciler, WorldDirector, EpisodeCompiler, HeadlessSimulationFarm
# and SemanticEngineAPI (findings #132/#134/#138/#140/#142/#145). It proves
# the "self-contained std" claim of those contracts against the INSTALLED SDK
# (headers under <prefix>/include/engine/... + the vc_sdk archive).
cmake_minimum_required(VERSION 3.20)
project(vulkan_craft_differentials LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(vulkan_craft_sdk CONFIG REQUIRED)

add_executable(differentials_consumer main.cpp)
target_link_libraries(differentials_consumer PRIVATE vulkan_craft_sdk)
