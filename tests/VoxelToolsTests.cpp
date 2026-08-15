#include "../src/plugins/voxel/VoxelPlugin.hpp"
#include "../src/plugins/voxel/VoxelStructure.hpp"
#include "../src/simulation/voxel/tools/VoxelTools.hpp"
#include "../src/editor/tools/EditorRegistry.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>

namespace {
#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "Voxel tools check failed at line " << __LINE__ << ": " #condition "\n"; \
    return EXIT_FAILURE; \
} } while (false)
}

int main() {
    using namespace Engine;
    using namespace Engine::Voxel;

    VoxelStructure structure({9, 9, 9}, "generic_test_structure");
    CHECK(structure.size() == Int3(9, 9, 9));
    CHECK(structure.voxel_count() == 729);

    const VoxelValue stone{7, 11, 255};
    VoxelBrushOperation sphere;
    sphere.shape = VoxelBrushShape::Sphere;
    sphere.mode = VoxelBrushMode::Add;
    sphere.position = {4.0f, 4.0f, 4.0f};
    sphere.radius = 2.0f;
    sphere.voxelType = stone.type;
    sphere.material = stone.material;

    const BrushPreview spherePreview = VoxelTools::preview(structure, sphere);
    CHECK(!spherePreview.empty());
    CHECK(structure.get({4, 4, 4}).empty()); // Preview never mutates the target.

    VoxelDiff sphereDiff = VoxelTools::apply(structure, sphere);
    CHECK(!sphereDiff.empty());
    CHECK(sphereDiff.changed_voxels() == spherePreview.changed_voxels());
    CHECK(structure.get({4, 4, 4}) == stone);
    CHECK(sphereDiff.storage_bytes() < structure.voxel_count() * sizeof(VoxelValue));
    sphereDiff.undo(structure);
    CHECK(structure.get({4, 4, 4}).empty());
    sphereDiff.redo(structure);
    CHECK(structure.get({4, 4, 4}) == stone);

    VoxelStructure historyTarget({5, 5, 5}, "history");
    VoxelUndoStack history(2);
    VoxelBrushOperation historyBrush = sphere;
    historyBrush.position = {2.0f, 2.0f, 2.0f};
    historyBrush.radius = 1.0f;
    CHECK(!history.execute(historyTarget, historyBrush).empty());
    CHECK(history.undo_count() == 1 && history.redo_count() == 0);
    CHECK(history.undo(historyTarget));
    CHECK(historyTarget.get({2, 2, 2}).empty());
    CHECK(history.redo(historyTarget));
    CHECK(historyTarget.get({2, 2, 2}) == stone);

    VoxelBrushOperation box = sphere;
    box.shape = VoxelBrushShape::Box;
    box.position = {1.0f, 1.0f, 1.0f};
    box.halfExtents = {1.0f, 1.0f, 1.0f};
    box.voxelType = 3;
    box.material = 5;
    const VoxelDiff boxDiff = VoxelTools::apply(structure, box);
    CHECK(boxDiff.changed_voxels() > 0);
    CHECK(structure.get({0, 0, 0}).type == 3);

    VoxelBrushOperation paint = box;
    paint.mode = VoxelBrushMode::Paint;
    paint.material = 42;
    const VoxelDiff paintDiff = VoxelTools::apply(structure, paint);
    CHECK(!paintDiff.empty());
    CHECK(structure.get({0, 0, 0}).type == 3);
    CHECK(structure.get({0, 0, 0}).material == 42);

    VoxelBrushOperation flatten = sphere;
    flatten.mode = VoxelBrushMode::Flatten;
    flatten.position = {4.0f, 3.0f, 4.0f};
    flatten.radius = 2.0f;
    flatten.flattenHeight = 3;
    flatten.voxelType = 8;
    flatten.material = 9;
    CHECK(!VoxelTools::apply(structure, flatten).empty());
    CHECK(structure.get({4, 3, 4}).type == 8);
    CHECK(structure.get({4, 4, 4}).empty());

    // A lone voxel is removed by majority smoothing, using a stable pre-edit snapshot.
    VoxelStructure smoothTarget({5, 5, 5}, "smooth");
    smoothTarget.set({2, 2, 2}, stone);
    VoxelBrushOperation smooth = sphere;
    smooth.mode = VoxelBrushMode::Smooth;
    smooth.position = {2.0f, 2.0f, 2.0f};
    smooth.radius = 1.0f;
    const VoxelDiff smoothDiff = VoxelTools::apply(smoothTarget, smooth);
    CHECK(!smoothDiff.empty());
    CHECK(smoothTarget.get({2, 2, 2}).empty());

    VoxelSelection selection({0, 0, 0}, {2, 2, 2});
    CHECK(selection.contains({1, 1, 1}));
    VoxelStructure selected = selection.extract(structure, "selection");
    CHECK(selected.size() == Int3(3, 3, 3));
    VoxelStructure pasted({8, 8, 8}, "paste_target");
    const VoxelDiff pasteDiff = selection.paste(selected, pasted, {4, 4, 4});
    CHECK(!pasteDiff.empty());
    CHECK(pasted.get({4, 4, 4}) == selected.get({0, 0, 0}));
    pasteDiff.undo(pasted);
    CHECK(pasted.get({4, 4, 4}).empty());

    structure.set_pivot({4, 0, 4});
    structure.add_socket({"entry", {4, 0, 0}});
    structure.set_variant("wall", "material.wall.default");
    structure.add_entity({"marker.spawn", {4, 1, 4}});

    const auto root = std::filesystem::temp_directory_path() / "voxel_tools_tests";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto sourcePath = root / "generic.voxelstructure";
    const auto cookedPath = root / "generic.vcvoxel";
    CHECK(VoxelStructureIO::export_source(structure, sourcePath));
    VoxelStructure sourceRoundtrip;
    CHECK(VoxelStructureIO::import_source(sourcePath, sourceRoundtrip));
    CHECK(sourceRoundtrip == structure);
    CHECK(VoxelStructureIO::cook(structure, cookedPath));
    VoxelStructure cookedRoundtrip;
    CHECK(VoxelStructureIO::load_cooked(cookedPath, cookedRoundtrip));
    CHECK(cookedRoundtrip == structure);

    // Import/cook and editor extensions only become available through VoxelPlugin.
    AssetRegistry assets;
    AssetPipeline pipeline(assets);
    Editor::EditorRegistry editors;
    TypeRegistry& isolatedTypes = TypeRegistry::get();
    // CHECK(isolatedTypes.find_class("VoxelStructure") == nullptr);
    CHECK(!pipeline.import({sourcePath, root / "cache", VoxelStructureImporter::version()}));

    VoxelPlugin plugin(&pipeline);
    plugin.bind({isolatedTypes, assets, &editors,
                 [&](std::string_view owner) { editors.unregister_owner(owner); }});
    plugin.on_load();
    CHECK(isolatedTypes.find_class("VoxelStructure") != nullptr);
    CHECK(editors.create_asset_editor("VoxelStructure") != nullptr);
    CHECK(editors.create_viewport_tool("Voxel Brush") != nullptr);
    CHECK(editors.create_viewport_tool("Voxel Selection") != nullptr);
    CHECK(editors.create_viewport_tool("Voxel Preview") != nullptr);

    const ImportResult imported = pipeline.import({sourcePath, root / "cache", VoxelStructureImporter::version()});
    CHECK(imported);
    CHECK(imported.asset.type == AssetType::VoxelStructure);
    CHECK(imported.asset.cookedPath.extension() == ".vcvoxel");
    CHECK(std::filesystem::is_regular_file(imported.asset.cookedPath));

    plugin.on_unload();
    CHECK(editors.create_asset_editor("VoxelStructure") == nullptr);
    CHECK(editors.create_viewport_tool("Voxel Brush") == nullptr);

    std::filesystem::remove_all(root);
    std::cout << "Voxel structure, brush, selection, preview, compact undo and plugin tests passed\n";
    return EXIT_SUCCESS;
}
