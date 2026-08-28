#include "GameApplication.hpp"

#include "VulkanEngineApp.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>

int GameApplication::run() {
    VulkanEngineApp engine;
    try {
        engine.init();
        engine.run();
        engine.cleanup();
    } catch (const std::exception& error) {
        std::cerr << "[Fatal Error] " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
