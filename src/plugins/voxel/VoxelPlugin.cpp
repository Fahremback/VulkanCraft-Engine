#include "VoxelPlugin.hpp"

#include "../../editor/tools/EditorRegistry.hpp"

#include <memory>

namespace Engine {

void VoxelPlugin::register_types(TypeRegistry& registry) {
    Plugins::register_plugin_type(registry, "VoxelStructure");
    Plugins::register_plugin_type(registry, "VoxelBrushOperation");
    Plugins::register_plugin_type(registry, "VoxelSelection");
}

void VoxelPlugin::register_assets(AssetRegistry& registry) {
    (void)registry;
    if (pipeline_ && !importerRegistered_) {
        pipeline_->add_importer(std::make_unique<Voxel::VoxelStructureImporter>());
        importerRegistered_ = true;
    }
}

void VoxelPlugin::register_editor_tools(Editor::EditorRegistry& registry) {
    Plugins::register_asset_tool(registry, "VoxelStructure", "Tools/Voxel", get_name());
    Plugins::register_viewport_tool(registry, "Voxel Brush", "Tools/Voxel", get_name());
    Plugins::register_viewport_tool(registry, "Voxel Selection", "Tools/Voxel", get_name());
    Plugins::register_viewport_tool(registry, "Voxel Preview", "Tools/Voxel", get_name());
}

Voxel::VoxelDiff VoxelPlugin::apply_brush(Voxel::VoxelStructure& target,
                                           const VoxelBrushOperation& operation) const {
    return Voxel::VoxelTools::apply(target, operation);
}

} // namespace Engine
