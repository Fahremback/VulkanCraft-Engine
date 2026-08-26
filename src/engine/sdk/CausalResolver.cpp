// CausalResolver.cpp — SDK adapter for the public ICausalResolver contract
// (FALTANTES differential: invalidation/recomputation of ONLY the affected
// descendants). Single TU, pure, deterministic; refuses unknown ids and
// cycles all-or-nothing (cycles are structurally impossible with immutable
// edges — the guard is defensive for the contract).
#include "engine/timeline/ICausalResolver.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace engine::timeline {
namespace {

class CausalResolver final : public ICausalResolver {
public:
    CausalNodeId add_leaf(const std::string& name, std::int64_t initialValue,
                          std::string& errorOut) override {
        const CausalNodeId id = nextId_++;
        Node& node = nodes_[id];
        node.id = id;
        node.name = name;
        node.isLeaf = true;
        node.value = initialValue;
        return id;
    }

    CausalNodeId add_derived(const std::string& name, const std::vector<CausalNodeId>& inputs,
                             CausalComputeFn fn, std::string& errorOut) override {
        for (const CausalNodeId input : inputs) {
            if (nodes_.find(input) == nodes_.end()) {
                errorOut = "unknown input node";
                return 0;
            }
        }
        // Cycle guard (defensive: a fresh node has no consumers yet, so a
        // cycle cannot be formed through immutable edges — kept for the
        // all-or-nothing contract).
        const CausalNodeId id = nextId_++;
        for (const CausalNodeId input : inputs) {
            if (reachable(input, id)) {
                nodes_.erase(id);
                --nextId_;
                errorOut = "cycle would be formed";
                return 0;
            }
        }
        Node& node = nodes_[id];
        node.id = id;
        node.name = name;
        node.isLeaf = false;
        node.fn = fn;
        node.inputs = inputs;
        for (const CausalNodeId input : inputs) {
            nodes_[input].consumers.push_back(id);
        }
        node.dirty = true;  // a fresh derived needs its first computation
        return id;
    }

    bool set_leaf_value(CausalNodeId id, std::int64_t value, std::string& errorOut) override {
        const auto found = nodes_.find(id);
        if (found == nodes_.end()) {
            errorOut = "unknown node";
            return false;
        }
        if (!found->second.isLeaf) {
            errorOut = "only leaf values are caller-owned";
            return false;
        }
        found->second.value = value;
        found->second.dirty = true;
        mark_descendants_dirty(id);
        return true;
    }

    std::vector<CausalNodeId> affected_descendants(CausalNodeId changedLeaf) const override {
        std::vector<CausalNodeId> result;
        const auto found = nodes_.find(changedLeaf);
        if (found == nodes_.end()) return result;
        std::set<CausalNodeId> visited;
        std::vector<CausalNodeId> queue = found->second.consumers;
        while (!queue.empty()) {
            const CausalNodeId id = queue.back();
            queue.pop_back();
            if (visited.count(id) != 0) continue;
            visited.insert(id);
            result.push_back(id);
            const auto it = nodes_.find(id);
            if (it == nodes_.end()) continue;
            queue.insert(queue.end(), it->second.consumers.begin(),
                         it->second.consumers.end());
        }
        // Deterministic order: ascending id.
        std::sort(result.begin(), result.end());
        return result;
    }

    std::size_t resolve() override {
        std::vector<CausalNodeId> dirty;
        for (const auto& entry : nodes_) {
            if (entry.second.dirty) dirty.push_back(entry.first);
        }
        if (dirty.empty()) return 0;

        // Indegree within the dirty subgraph.
        std::map<CausalNodeId, std::size_t> indegree;
        for (const CausalNodeId id : dirty) {
            std::size_t degree = 0;
            const Node& node = nodes_.at(id);
            for (const CausalNodeId input : node.inputs) {
                if (nodes_.at(input).dirty) ++degree;
            }
            indegree[id] = degree;
        }

        // Deterministic min-first processing.
        std::set<CausalNodeId> ready;
        for (const CausalNodeId id : dirty) {
            if (indegree[id] == 0) ready.insert(id);
        }
        std::size_t recomputed = 0;
        while (!ready.empty()) {
            const CausalNodeId id = *ready.begin();
            ready.erase(ready.begin());
            Node& node = nodes_.at(id);
            node.dirty = false;
            if (!node.isLeaf) {
                std::vector<std::int64_t> values;
                values.reserve(node.inputs.size());
                for (const CausalNodeId input : node.inputs) {
                    values.push_back(nodes_.at(input).value);
                }
                node.value = node.fn(values);
                ++node.recomputeCount;
                ++recomputed;
            }
            for (const CausalNodeId consumer : node.consumers) {
                if (!nodes_.at(consumer).dirty) continue;
                if (--indegree[consumer] == 0) ready.insert(consumer);
            }
        }
        return recomputed;
    }

    bool state(CausalNodeId id, CausalNodeState& out) const override {
        const auto found = nodes_.find(id);
        if (found == nodes_.end()) return false;
        out.id = found->second.id;
        out.isLeaf = found->second.isLeaf;
        out.name = found->second.name;
        out.inputs = found->second.inputs;
        out.consumers = found->second.consumers;
        out.value = found->second.value;
        out.recomputeCount = found->second.recomputeCount;
        out.dirty = found->second.dirty;
        return true;
    }

    std::size_t node_count() const override { return nodes_.size(); }
    void clear() override {
        nodes_.clear();
        nextId_ = 1;
    }

private:
    struct Node {
        CausalNodeId id{ 0 };
        bool isLeaf{ false };
        std::string name;
        std::vector<CausalNodeId> inputs;
        std::vector<CausalNodeId> consumers;
        CausalComputeFn fn{ nullptr };
        std::int64_t value{ 0 };
        std::uint64_t recomputeCount{ 0 };
        bool dirty{ false };
    };

    // True when `from` can reach `target` following input edges.
    bool reachable(CausalNodeId from, CausalNodeId target) const {
        std::set<CausalNodeId> visited;
        std::vector<CausalNodeId> queue{ from };
        while (!queue.empty()) {
            const CausalNodeId id = queue.back();
            queue.pop_back();
            if (id == target) return true;
            if (visited.count(id) != 0) continue;
            visited.insert(id);
            const auto found = nodes_.find(id);
            if (found == nodes_.end()) continue;
            queue.insert(queue.end(), found->second.inputs.begin(),
                         found->second.inputs.end());
        }
        return false;
    }

    void mark_descendants_dirty(CausalNodeId changedLeaf) {
        std::set<CausalNodeId> visited;
        std::vector<CausalNodeId> queue = nodes_.at(changedLeaf).consumers;
        while (!queue.empty()) {
            const CausalNodeId id = queue.back();
            queue.pop_back();
            if (visited.count(id) != 0) continue;
            visited.insert(id);
            Node& node = nodes_.at(id);
            node.dirty = true;
            queue.insert(queue.end(), node.consumers.begin(), node.consumers.end());
        }
    }

    std::map<CausalNodeId, Node> nodes_;
    CausalNodeId nextId_{ 1 };
};

}  // namespace

std::unique_ptr<ICausalResolver> create_causal_resolver() {
    return std::unique_ptr<ICausalResolver>(new CausalResolver());
}

}  // namespace engine::timeline
