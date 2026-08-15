#include "PluginContract.hpp"
#include "../editor/tools/EditorRegistry.hpp"

namespace Engine::Plugins {
namespace {
class HeadlessAssetEditor final : public Editor::IAssetEditor {
public:
    explicit HeadlessAssetEditor(std::string type) : type_(std::move(type)) {}
    void open_asset(UUID asset) override { asset_ = asset; }
private:
    std::string type_; UUID asset_{0,0};
};
class HeadlessViewportTool final : public Editor::IViewportTool {
public:
    explicit HeadlessViewportTool(std::string id) : id_(std::move(id)) {}
    std::string_view id() const noexcept override { return id_; }
    void activate() override { active_ = true; }
    void deactivate() override { active_ = false; }
private:
    std::string id_; bool active_{};
};
}
void register_asset_tool(Editor::EditorRegistry& registry, std::string assetType,
                         std::string menuPath, std::string owner) {
    const std::string id = "asset." + assetType;
    registry.register_asset_editor(assetType, [assetType] { return std::make_unique<HeadlessAssetEditor>(assetType); }, owner);
    registry.register_menu_item({id, std::move(menuPath), "Open " + assetType + " Editor", {}, 0, {}, [] {}}, owner);
}
void register_viewport_tool(Editor::EditorRegistry& registry, std::string toolId,
                            std::string menuPath, std::string owner) {
    const std::string copy = toolId;
    registry.register_viewport_tool(toolId, [copy] { return std::make_unique<HeadlessViewportTool>(copy); }, owner);
    registry.register_menu_item({"tool." + toolId, std::move(menuPath), toolId, {}, 0, {}, [] {}}, owner);
}
} // namespace Engine::Plugins
