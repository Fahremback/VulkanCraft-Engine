#include "RenderingToolModels.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <unordered_map>

namespace Engine::Editor {
namespace {

std::string lowercase(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

bool matches(std::string_view value, std::string_view search) {
    return search.empty() || lowercase(value).find(lowercase(search)) != std::string::npos;
}

} // namespace

MaterialEditorModel::MaterialEditorModel(Rendering::MaterialGraph* graph) : graph_(graph) {}

void MaterialEditorModel::bind(Rendering::MaterialGraph* graph) {
    graph_ = graph;
    selected_ = Rendering::InvalidMaterialNode;
    lastCompile_ = {};
    revision_ = 0;
    dirty_ = false;
}

bool MaterialEditorModel::select(Rendering::MaterialNodeId id) {
    if (!graph_ || !graph_->find_node(id)) return false;
    selected_ = id;
    return true;
}

bool MaterialEditorModel::connect(Rendering::MaterialNodeId source, Rendering::MaterialNodeId destination,
                                  size_t inputIndex) {
    if (!graph_ || !graph_->connect(source, destination, inputIndex)) return false;
    changed();
    return true;
}

bool MaterialEditorModel::remove_selected() {
    if (!graph_ || selected_ == Rendering::InvalidMaterialNode || !graph_->remove_node(selected_)) return false;
    selected_ = Rendering::InvalidMaterialNode;
    changed();
    return true;
}

Rendering::MaterialCompileResult MaterialEditorModel::compile_preview() {
    lastCompile_ = graph_ ? graph_->compile() : Rendering::MaterialCompileResult{};
    if (!graph_) lastCompile_.errors.push_back({Rendering::InvalidMaterialNode, "No material graph is bound"});
    return lastCompile_;
}

std::vector<MaterialEditorNodeView> MaterialEditorModel::nodes(std::string_view search) const {
    std::vector<MaterialEditorNodeView> result;
    if (!graph_) return result;
    for (const auto& node : graph_->nodes()) {
        if (!matches(node.label, search) && !matches(node.parameter, search)) continue;
        result.push_back({node.id, node.label, node.kind, node.outputType, node.id == selected_});
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    return result;
}

void MaterialEditorModel::changed() {
    ++revision_;
    dirty_ = true;
    lastCompile_ = {};
}

void RenderGraphViewerModel::rebuild(const Rendering::RenderGraph& graph) {
    compilation_ = graph.compile();
    passes_.clear();
    resources_.clear();

    std::unordered_map<Rendering::RenderPassId, uint32_t> compiledIndices;
    for (uint32_t i = 0; i < compilation_.order.size(); ++i) compiledIndices.emplace(compilation_.order[i], i);
    for (size_t i = 0; i < graph.passes().size(); ++i) {
        const auto id = graph.pass_ids()[i];
        const auto compiledIt = compiledIndices.find(id);
        if (compiledIt == compiledIndices.end()) continue;
        const auto& source = graph.passes()[i];
        RenderGraphPassView view{id, source.name, source.queue, compiledIt->second, selectedPass_ == id};
        for (const auto& access : source.resources) {
            if (access.access == Rendering::RenderAccess::Read || access.access == Rendering::RenderAccess::ReadWrite)
                view.reads.push_back(access.resource);
            if (Rendering::render_access_writes(access.access)) view.writes.push_back(access.resource);
        }
        passes_.push_back(std::move(view));
    }
    std::sort(passes_.begin(), passes_.end(), [](const auto& a, const auto& b) { return a.compiledIndex < b.compiledIndex; });

    std::unordered_map<Rendering::RenderResourceId, Rendering::RenderResourceLifetime> lifetimes;
    for (const auto& lifetime : compilation_.lifetimes) lifetimes.emplace(lifetime.resource, lifetime);
    for (size_t i = 0; i < graph.resources().size(); ++i) {
        const auto id = graph.resource_ids()[i];
        const auto lifetime = lifetimes.find(id);
        if (lifetime == lifetimes.end()) continue;
        const auto& source = graph.resources()[i];
        resources_.push_back({id, source.name, source.kind, lifetime->second.firstUse,
                              lifetime->second.lastUse, lifetime->second.transient, selectedResource_ == id});
    }
    std::sort(resources_.begin(), resources_.end(), [](const auto& a, const auto& b) {
        return a.firstUse == b.firstUse ? a.id < b.id : a.firstUse < b.firstUse;
    });

    if (selectedPass_ && !std::any_of(passes_.begin(), passes_.end(), [&](const auto& view) { return view.id == *selectedPass_; }))
        selectedPass_.reset();
    if (selectedResource_ && !std::any_of(resources_.begin(), resources_.end(), [&](const auto& view) { return view.id == *selectedResource_; }))
        selectedResource_.reset();
}

std::vector<RenderGraphPassView> RenderGraphViewerModel::passes(std::string_view search) const {
    std::vector<RenderGraphPassView> result;
    std::copy_if(passes_.begin(), passes_.end(), std::back_inserter(result),
                 [&](const auto& pass) { return matches(pass.name, search); });
    return result;
}

std::vector<RenderGraphResourceView> RenderGraphViewerModel::resources(std::string_view search) const {
    std::vector<RenderGraphResourceView> result;
    std::copy_if(resources_.begin(), resources_.end(), std::back_inserter(result),
                 [&](const auto& resource) { return matches(resource.name, search); });
    return result;
}

std::vector<Rendering::RenderBarrier> RenderGraphViewerModel::barriers_for_pass(Rendering::RenderPassId pass) const {
    std::vector<Rendering::RenderBarrier> result;
    std::copy_if(compilation_.barriers.begin(), compilation_.barriers.end(), std::back_inserter(result),
                 [pass](const auto& barrier) { return barrier.sourcePass == pass || barrier.destinationPass == pass; });
    return result;
}

bool RenderGraphViewerModel::select_pass(Rendering::RenderPassId pass) {
    if (!std::any_of(passes_.begin(), passes_.end(), [pass](const auto& view) { return view.id == pass; })) return false;
    selectedPass_ = pass;
    selectedResource_.reset();
    for (auto& view : passes_) view.selected = view.id == pass;
    for (auto& view : resources_) view.selected = false;
    return true;
}

bool RenderGraphViewerModel::select_resource(Rendering::RenderResourceId resource) {
    if (!std::any_of(resources_.begin(), resources_.end(), [resource](const auto& view) { return view.id == resource; })) return false;
    selectedResource_ = resource;
    selectedPass_.reset();
    for (auto& view : resources_) view.selected = view.id == resource;
    for (auto& view : passes_) view.selected = false;
    return true;
}

void RenderGraphViewerModel::clear_selection() noexcept {
    selectedPass_.reset();
    selectedResource_.reset();
    for (auto& view : passes_) view.selected = false;
    for (auto& view : resources_) view.selected = false;
}

} // namespace Engine::Editor
