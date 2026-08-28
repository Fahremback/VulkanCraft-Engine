#include "VulkanGame.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>

int main() {
    try {
        VulkanGame game;
        game.run();
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
