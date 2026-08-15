#pragma once

#include "../PluginContract.hpp"
#include "VoxelStructure.hpp"
#include "../../simulation/voxel/tools/VoxelTools.hpp"

namespace Engine {

// Voxel support is opt-in. Types, importer and editor extensions are contributed
// only while a VoxelPlugin is explicitly loaded by a project.
class VoxelPlugin final : public Plugins::EnginePlugin {
public:
    explicit VoxelPlugin(AssetPipeline* pipeline = nullptr) noexcept : pipeline_(pipeline) {}

    [[nodiscard]] std::string get_name() const override { return "Voxel"; }
    [[nodiscard]] std::string get_version() const override { return "2.0.0"; }

    [[nodiscard]] Voxel::VoxelDiff apply_brush(Voxel::VoxelStructure& target,
                                                const VoxelBrushOperation& operation) const;

protected:
    void register_types(TypeRegistry& registry) override;
    void register_assets(AssetRegistry& registry) override;
    void register_editor_tools(Editor::EditorRegistry& registry) override;

private:
    AssetPipeline* pipeline_{};
    bool importerRegistered_{};
};

} // namespace Engine
