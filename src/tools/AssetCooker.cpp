#include "AssetCooker.hpp"
#include "../engine/assets/AssetRegistry.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace Engine {

int run_asset_cooker(const std::vector<std::string>& arguments) {
    if (arguments.size() < 3) {
        std::cerr << "Usage: VulkanEngineCooker <registry.db> <root-uuid> [root-uuid ...] <output-directory>\n";
        return EXIT_FAILURE;
    }

    AssetRegistry registry;
    if (!registry.load(arguments.front())) {
        std::cerr << "[Cooker] Cannot load asset registry: " << arguments.front() << '\n';
        return EXIT_FAILURE;
    }

    std::vector<UUID> roots;
    roots.reserve(arguments.size() - 2);
    try {
        for (size_t index = 1; index + 1 < arguments.size(); ++index) {
            UUID root = UUID::from_string(arguments[index]);
            if (!root.is_valid() || !registry.find(root)) {
                std::cerr << "[Cooker] Invalid or unknown root UUID: " << arguments[index] << '\n';
                return EXIT_FAILURE;
            }
            roots.push_back(root);
        }
    } catch (const std::exception& error) {
        std::cerr << "[Cooker] Invalid root UUID: " << error.what() << '\n';
        return EXIT_FAILURE;
    }

    const AssetPackageResult result = AssetPackager::package(registry, roots, arguments.back());
    if (!result) {
        std::cerr << "[Cooker] " << result.error << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "[Cooker] Packaged " << result.assets.size() << " asset(s)\n"
              << "[Cooker] Manifest: " << result.manifestPath << '\n';
    return EXIT_SUCCESS;
}

} // namespace Engine
