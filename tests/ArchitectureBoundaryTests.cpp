#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {
std::string read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream text;
    text << stream.rdbuf();
    return text.str();
}

bool contains_forbidden_rendering_type(const std::string& text) {
    return text.find("VulkanTypes") != std::string::npos ||
           text.find("VkBuffer") != std::string::npos ||
           text.find("VmaAllocation") != std::string::npos;
}
}

int main() {
    const std::filesystem::path root = VULKANCRAFT_SOURCE_DIR;
    const auto chunkData = read(root / "src/simulation/voxel/storage/ChunkData.hpp");
    const auto chunkMesh = read(root / "src/simulation/voxel/storage/ChunkMeshData.hpp");
    const auto voxelCore = read(root / "src/simulation/voxel/core/Voxel.hpp");
    const auto worldHeader = read(root / "src/simulation/voxel/streaming/World.hpp");
    const auto worldSource = read(root / "src/simulation/voxel/streaming/World.cpp");
    const auto playerHeader = read(root / "src/features/player/Player.hpp");
    const auto playerSource = read(root / "src/features/player/Player.cpp");
    const auto editorSource = read(root / "src/editor/EditorApplication.cpp");

#ifdef _WIN32
    if (editorSource.find("1.5/build/Release/vulkan_craft.exe") == std::string::npos ||
        editorSource.find("SetParent") == std::string::npos ||
        editorSource.find("MoveWindow") == std::string::npos ||
        editorSource.find("WS_CHILD") == std::string::npos ||
        editorSource.find("ScreenToClient") != std::string::npos) {
        std::cerr << "Windows editor play mode must embed the 1.5 game inside the scene viewport\n";
        return 1;
    }
#endif

    if (contains_forbidden_rendering_type(chunkData) ||
        contains_forbidden_rendering_type(chunkMesh) ||
        contains_forbidden_rendering_type(voxelCore) ||
        contains_forbidden_rendering_type(worldHeader) ||
        contains_forbidden_rendering_type(worldSource)) {
        std::cerr << "CPU voxel boundary depends on renderer/Vulkan\n";
        return 1;
    }
    if (playerHeader.find("GLFW") != std::string::npos ||
        playerSource.find("glfw") != std::string::npos) {
        std::cerr << "Player feature depends directly on GLFW\n";
        return 1;
    }
    // FALTANTES §8 item 166: simulation must never read the VISUAL material
    // table. World.cpp resolves builtin solidity/light through the simulation
    // table (get_block_sim / BLOCK_SIM_PROPS); get_block_material/BLOCK_MATERIALS
    // stay in the render path only.
    if (worldSource.find("get_block_material(") != std::string::npos ||
        worldSource.find("BLOCK_MATERIALS[") != std::string::npos) {
        std::cerr << "CPU simulation reads the visual material table\n";
        return 1;
    }
    // BlockMaterial must be appearance-only (color/textures). A solid/transparent
    // member there would let a visual edit silently change physics.
    const std::size_t materialStruct = voxelCore.find("struct BlockMaterial");
    const std::size_t materialEnd = voxelCore.find("};", materialStruct);
    if (materialStruct != std::string::npos && materialEnd != std::string::npos) {
        const std::string materialBody =
            voxelCore.substr(materialStruct, materialEnd - materialStruct);
        if (materialBody.find("bool solid") != std::string::npos ||
            materialBody.find("bool transparent") != std::string::npos) {
            std::cerr << "BlockMaterial carries simulation flags\n";
            return 1;
        }
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root / "src")) {
        if (!entry.is_regular_file()) continue;
        const std::string extension = entry.path().extension().string();
        if (extension != ".hpp" && extension != ".cpp") continue;
        const auto source = read(entry.path());
        if (source.find("::instance()") != std::string::npos ||
            source.find("static Engine* instance") != std::string::npos) {
            std::cerr << "Global service locator found in " << entry.path() << '\n';
            return 1;
        }
    }
    return 0;
}
