#include "RenderGraph.hpp"

#include <algorithm>
#include <compare>
#include <cstddef>
#include <functional>
#include <queue>
#include <set>
#include <unordered_set>

namespace Engine::Rendering {
namespace {

struct Edge {
    RenderPassId before;
    RenderPassId after;
    auto operator<=>(const Edge&) const = default;
};

bool access_conflicts(RenderAccess a, RenderAccess b) noexcept {
    return render_access_writes(a) || render_access_writes(b);
}

} // namespace

std::string_view render_state_name(RenderResourceState state) noexcept {
    switch (state) {
    case RenderResourceState::Undefined: return "Undefined";
    case RenderResourceState::ShaderRead: return "ShaderRead";
    case RenderResourceState::ColorAttachment: return "ColorAttachment";
    case RenderResourceState::DepthAttachment: return "DepthAttachment";
    case RenderResourceState::General: return "General";
    case RenderResourceState::TransferSource: return "TransferSource";
    case RenderResourceState::TransferDestination: return "TransferDestination";
    case RenderResourceState::Present: return "Present";
    }
    return "Unknown";
}

RenderResourceId RenderGraph::add_resource(RenderResourceDesc resourceDesc) {
    if (resourceDesc.name.empty()) resourceDesc.name = "Resource" + std::to_string(nextResourceId_);
    const RenderResourceId id = nextResourceId_++;
    resources_.push_back(std::move(resourceDesc));
    resourceIds_.push_back(id);
    return id;
}

RenderPassId RenderGraph::add_pass(RenderPassDesc passDesc) {
    if (passDesc.name.empty()) passDesc.name = "Pass" + std::to_string(nextPassId_);
    const RenderPassId id = nextPassId_++;
    passes_.push_back(std::move(passDesc));
    passIds_.push_back(id);
    return id;
}

bool RenderGraph::add_dependency(RenderPassId before, RenderPassId after) {
    if (before == after || !pass(before) || !pass(after)) return false;
    if (std::any_of(dependencies_.begin(), dependencies_.end(), [&](const auto& dependency) {
        return dependency.before == before && dependency.after == after;
    })) return false;
    dependencies_.push_back({before, after});
    return true;
}

bool RenderGraph::remove_pass(RenderPassId id) {
    const auto it = std::find(passIds_.begin(), passIds_.end(), id);
    if (it == passIds_.end()) return false;
    const size_t index = static_cast<size_t>(std::distance(passIds_.begin(), it));
    passIds_.erase(it);
    passes_.erase(passes_.begin() + static_cast<std::ptrdiff_t>(index));
    std::erase_if(dependencies_, [id](const auto& dependency) { return dependency.before == id || dependency.after == id; });
    return true;
}

bool RenderGraph::remove_resource(RenderResourceId id) {
    const auto it = std::find(resourceIds_.begin(), resourceIds_.end(), id);
    if (it == resourceIds_.end()) return false;
    for (const auto& passDesc : passes_)
        if (std::any_of(passDesc.resources.begin(), passDesc.resources.end(), [id](const auto& access) { return access.resource == id; }))
            return false;
    const size_t index = static_cast<size_t>(std::distance(resourceIds_.begin(), it));
    resourceIds_.erase(it);
    resources_.erase(resources_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

bool RenderGraph::set_pass_enabled(RenderPassId id, bool enabled) {
    const auto it = std::find(passIds_.begin(), passIds_.end(), id);
    if (it == passIds_.end()) return false;
    passes_[static_cast<size_t>(std::distance(passIds_.begin(), it))].enabled = enabled;
    return true;
}

const RenderResourceDesc* RenderGraph::resource(RenderResourceId id) const noexcept {
    const auto it = std::find(resourceIds_.begin(), resourceIds_.end(), id);
    return it == resourceIds_.end() ? nullptr : &resources_[static_cast<size_t>(std::distance(resourceIds_.begin(), it))];
}

const RenderPassDesc* RenderGraph::pass(RenderPassId id) const noexcept {
    const auto it = std::find(passIds_.begin(), passIds_.end(), id);
    return it == passIds_.end() ? nullptr : &passes_[static_cast<size_t>(std::distance(passIds_.begin(), it))];
}

RenderGraphCompileResult RenderGraph::compile() const {
    RenderGraphCompileResult result;
    std::unordered_map<RenderPassId, size_t> passIndex;
    std::unordered_map<RenderResourceId, size_t> resourceIndex;
    for (size_t i = 0; i < passIds_.size(); ++i) passIndex.emplace(passIds_[i], i);
    for (size_t i = 0; i < resourceIds_.size(); ++i) resourceIndex.emplace(resourceIds_[i], i);

    std::vector<RenderPassId> active;
    for (size_t i = 0; i < passes_.size(); ++i) {
        if (!passes_[i].enabled) continue;
        active.push_back(passIds_[i]);
        std::unordered_set<RenderResourceId> seen;
        for (const auto& access : passes_[i].resources) {
            if (!resourceIndex.contains(access.resource))
                result.errors.push_back("Pass '" + passes_[i].name + "' references a missing resource");
            else if (!seen.insert(access.resource).second)
                result.errors.push_back("Pass '" + passes_[i].name + "' accesses a resource more than once");
            if (access.state == RenderResourceState::Undefined)
                result.errors.push_back("Pass '" + passes_[i].name + "' requests Undefined state");
        }
    }

    std::set<Edge> edges;
    for (const auto& dependency : dependencies_) {
        const auto beforeIt = passIndex.find(dependency.before);
        const auto afterIt = passIndex.find(dependency.after);
        if (beforeIt == passIndex.end() || afterIt == passIndex.end()) {
            result.errors.push_back("Render graph contains a dependency on a missing pass");
            continue;
        }
        if (passes_[beforeIt->second].enabled && passes_[afterIt->second].enabled)
            edges.insert({dependency.before, dependency.after});
    }

    // Insertion order is the declaration order used to disambiguate resource hazards.
    for (size_t left = 0; left < passes_.size(); ++left) {
        if (!passes_[left].enabled) continue;
        for (size_t right = left + 1; right < passes_.size(); ++right) {
            if (!passes_[right].enabled) continue;
            bool hazard = false;
            for (const auto& a : passes_[left].resources) {
                for (const auto& b : passes_[right].resources) {
                    if (a.resource == b.resource && resourceIndex.contains(a.resource) && access_conflicts(a.access, b.access)) {
                        hazard = true;
                        break;
                    }
                }
                if (hazard) break;
            }
            if (hazard) edges.insert({passIds_[left], passIds_[right]});
        }
    }

    std::unordered_map<RenderPassId, uint32_t> indegree;
    std::unordered_map<RenderPassId, std::vector<RenderPassId>> outgoing;
    for (RenderPassId id : active) indegree[id] = 0;
    for (const auto& edge : edges) {
        ++indegree[edge.after];
        outgoing[edge.before].push_back(edge.after);
    }
    for (auto& [_, successors] : outgoing) {
        std::sort(successors.begin(), successors.end(), [&](RenderPassId a, RenderPassId b) {
            return passIndex.at(a) < passIndex.at(b);
        });
    }

    using Candidate = std::pair<size_t, RenderPassId>;
    std::priority_queue<Candidate, std::vector<Candidate>, std::greater<>> ready;
    for (RenderPassId id : active) if (indegree[id] == 0) ready.push({passIndex.at(id), id});
    while (!ready.empty()) {
        const RenderPassId id = ready.top().second;
        ready.pop();
        result.order.push_back(id);
        for (RenderPassId successor : outgoing[id])
            if (--indegree[successor] == 0) ready.push({passIndex.at(successor), successor});
    }
    if (result.order.size() != active.size()) result.errors.push_back("Render graph contains a dependency cycle");
    if (!result.errors.empty()) {
        result.order.clear();
        return result;
    }

    struct LastUse {
        RenderPassId pass{InvalidRenderPass};
        RenderAccess access{RenderAccess::Read};
        RenderResourceState state{RenderResourceState::Undefined};
        RenderQueue queue{RenderQueue::Graphics};
        bool used{false};
    };
    std::unordered_map<RenderResourceId, LastUse> lastUses;
    std::unordered_map<RenderResourceId, RenderResourceLifetime> lifetimes;
    for (RenderResourceId id : resourceIds_) {
        const auto* desc = resource(id);
        lastUses[id].state = desc->initialState;
        lifetimes.emplace(id, RenderResourceLifetime{id, std::numeric_limits<uint32_t>::max(), 0, desc->transient});
    }

    for (uint32_t compiledIndex = 0; compiledIndex < result.order.size(); ++compiledIndex) {
        const RenderPassId passId = result.order[compiledIndex];
        const RenderPassDesc& passDesc = passes_[passIndex.at(passId)];
        for (const auto& access : passDesc.resources) {
            auto& lifetime = lifetimes.at(access.resource);
            lifetime.firstUse = std::min(lifetime.firstUse, compiledIndex);
            lifetime.lastUse = compiledIndex;
            auto& previous = lastUses.at(access.resource);
            const bool transition = previous.state != access.state;
            const bool hazard = previous.used && access_conflicts(previous.access, access.access);
            const bool queueTransfer = previous.used && previous.queue != passDesc.queue;
            if (transition || hazard || queueTransfer) {
                result.barriers.push_back({access.resource, previous.pass, passId,
                    previous.access, access.access, previous.state, access.state,
                    previous.queue, passDesc.queue, queueTransfer});
            }
            previous = {passId, access.access, access.state, passDesc.queue, true};
        }
    }

    for (RenderResourceId id : resourceIds_) {
        const auto& lifetime = lifetimes.at(id);
        if (lifetime.firstUse != std::numeric_limits<uint32_t>::max()) result.lifetimes.push_back(lifetime);
    }
    std::sort(result.lifetimes.begin(), result.lifetimes.end(), [](const auto& a, const auto& b) {
        return a.firstUse == b.firstUse ? a.resource < b.resource : a.firstUse < b.firstUse;
    });
    return result;
}

void RenderGraph::clear() {
    nextResourceId_ = 1;
    nextPassId_ = 1;
    resources_.clear();
    passes_.clear();
    resourceIds_.clear();
    passIds_.clear();
    dependencies_.clear();
}

} // namespace Engine::Rendering
