// VulkanEngineGame — the SECOND game executable. A3/A1-A-DUALPATH: the game
// binaries share ONE rendering path — this executable boots the SAME canonical
// runtime as `vulkan_craft` (GameApplication → VulkanEngineApp → frame graph
// + feature contract). The legacy monolithic VulkanGame renderer is no longer
// compiled into any shipped executable (its sources stay in the tree as
// reference only), so no object type is rendered twice with incompatible
// lighting/material.
#include "GameApplication.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>

int main() {
    try {
        GameApplication application;
        return application.run();
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        return EXIT_FAILURE;
    }
}
