#include "GameApplication.hpp"

#include "Engine.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>

int GameApplication::run() {
    Engine engine;
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
