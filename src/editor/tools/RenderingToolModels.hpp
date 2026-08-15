#pragma once

#include "../../engine/rendering/MaterialGraph.hpp"
#include "../../engine/rendering/RenderGraph.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Engine::Editor {

struct MaterialEditorNodeView {
    Rendering::MaterialNodeId id{Rendering::InvalidMaterialNode};
    std::string title;
    Rendering::MaterialNodeKind kind{Rendering::MaterialNodeKind::Constant};
    Rendering::MaterialValueType outputType{Rendering::MaterialValueType::Float};
    bool selected{false};
};

class MaterialEditorModel final {
public:
    explicit MaterialEditorModel(Rendering::MaterialGraph* graph = nullptr);
    void bind(Rendering::MaterialGraph* graph);
    [[nodiscard]] Rendering::MaterialGraph* graph() const noexcept { return graph_; }
    [[nodiscard]] bool select(Rendering::MaterialNodeId id);
    void clear_selection() noexcept { selected_ = Rendering::InvalidMaterialNode; }
    [[nodiscard]] Rendering::MaterialNodeId selected() const noexcept { return selected_; }
    [[nodiscard]] bool connect(Rendering::MaterialNodeId source, Rendering::MaterialNodeId destination,
                               size_t inputIndex);
    [[nodiscard]] bool remove_selected();
    [[nodiscard]] Rendering::MaterialCompileResult compile_preview();
    [[nodiscard]] const Rendering::MaterialCompileResult& last_compile() const noexcept { return lastCompile_; }
    [[nodiscard]] std::vector<MaterialEditorNodeView> nodes(std::string_view search = {}) const;
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }
    [[nodiscard]] uint64_t revision() const noexcept { return revision_; }
    void mark_saved() noexcept { dirty_ = false; }

private:
    void changed();
    Rendering::MaterialGraph* graph_{};
    Rendering::MaterialNodeId selected_{Rendering::InvalidMaterialNode};
    Rendering::MaterialCompileResult lastCompile_;
    uint64_t revision_{};
    bool dirty_{false};
};

struct RenderGraphPassView {
    Rendering::RenderPassId id{Rendering::InvalidRenderPass};
    std::string name;
    Rendering::RenderQueue queue{Rendering::RenderQueue::Graphics};
    uint32_t compiledIndex{};
    bool selected{false};
    std::vector<Rendering::RenderResourceId> reads;
    std::vector<Rendering::RenderResourceId> writes;
};

struct RenderGraphResourceView {
    Rendering::RenderResourceId id{Rendering::InvalidRenderResource};
    std::string name;
    Rendering::RenderResourceKind kind{Rendering::RenderResourceKind::Image};
    uint32_t firstUse{};
    uint32_t lastUse{};
    bool transient{true};
    bool selected{false};
};

class RenderGraphViewerModel final {
public:
    void rebuild(const Rendering::RenderGraph& graph);
    [[nodiscard]] const Rendering::RenderGraphCompileResult& compilation() const noexcept { return compilation_; }
    [[nodiscard]] std::vector<RenderGraphPassView> passes(std::string_view search = {}) const;
    [[nodiscard]] std::vector<RenderGraphResourceView> resources(std::string_view search = {}) const;
    [[nodiscard]] std::vector<Rendering::RenderBarrier> barriers_for_pass(Rendering::RenderPassId pass) const;
    [[nodiscard]] bool select_pass(Rendering::RenderPassId pass);
    [[nodiscard]] bool select_resource(Rendering::RenderResourceId resource);
    [[nodiscard]] std::optional<Rendering::RenderPassId> selected_pass() const noexcept { return selectedPass_; }
    [[nodiscard]] std::optional<Rendering::RenderResourceId> selected_resource() const noexcept { return selectedResource_; }
    void clear_selection() noexcept;

private:
    Rendering::RenderGraphCompileResult compilation_;
    std::vector<RenderGraphPassView> passes_;
    std::vector<RenderGraphResourceView> resources_;
    std::optional<Rendering::RenderPassId> selectedPass_;
    std::optional<Rendering::RenderResourceId> selectedResource_;
};

} // namespace Engine::Editor
