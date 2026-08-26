// TimelineGraph.cpp — SDK adapter for the public ITimelineGraph contract
// (FALTANTES differential: branches copy-on-write + causal history). Single
// TU, pure, deterministic; refuses unknown ids all-or-nothing.
#include "engine/timeline/ITimelineGraph.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace engine::timeline {
namespace {

class TimelineGraph final : public ITimelineGraph {
public:
    TimelineNodeId create_root(const std::vector<std::byte>& payload,
                               std::string& errorOut) override {
        if (!nodes_.empty()) {
            errorOut = "a timeline already has a root";
            return 0;
        }
        const TimelineNodeId id = nextId_++;
        Node& node = nodes_[id];
        node.id = id;
        node.parent = 0;
        node.revision = 1;
        node.payload = payload;
        node.hasOwnPayload = true;
        return id;
    }

    TimelineNodeId fork(TimelineNodeId parent, std::string& errorOut) override {
        const auto found = nodes_.find(parent);
        if (found == nodes_.end()) {
            errorOut = "unknown node";
            return 0;
        }
        const TimelineNodeId id = nextId_++;
        Node& node = nodes_[id];
        node.id = id;
        node.parent = parent;
        node.revision = found->second.revision + 1;
        // copy-on-write: no own payload — the effective state resolves lazily
        // to the nearest ancestor that wrote one.
        found->second.children.push_back(id);
        return id;
    }

    TimelineNodeId write(TimelineNodeId parent, const std::vector<std::byte>& payload,
                         std::string& errorOut) override {
        const auto found = nodes_.find(parent);
        if (found == nodes_.end()) {
            errorOut = "unknown node";
            return 0;
        }
        const TimelineNodeId id = nextId_++;
        Node& node = nodes_[id];
        node.id = id;
        node.parent = parent;
        node.revision = found->second.revision + 1;
        node.payload = payload;
        node.hasOwnPayload = true;
        found->second.children.push_back(id);
        return id;
    }

    bool node_info(TimelineNodeId id, TimelineNodeInfo& out) const override {
        const auto found = nodes_.find(id);
        if (found == nodes_.end()) return false;
        out.id = found->second.id;
        out.parent = found->second.parent;
        out.revision = found->second.revision;
        out.hasOwnPayload = found->second.hasOwnPayload;
        out.payload = found->second.payload;
        out.childCount = found->second.children.size();
        return true;
    }

    std::vector<TimelineNodeId> children(TimelineNodeId id) const override {
        const auto found = nodes_.find(id);
        if (found == nodes_.end()) return {};
        return found->second.children;
    }

    bool effective_payload(TimelineNodeId id, std::vector<std::byte>& outPayload) const override {
        TimelineNodeId cursor = id;
        while (cursor != 0) {
            const auto found = nodes_.find(cursor);
            if (found == nodes_.end()) return false;
            if (found->second.hasOwnPayload) {
                outPayload = found->second.payload;
                return true;
            }
            cursor = found->second.parent;
        }
        return false;
    }

    bool is_ancestor(TimelineNodeId ancestor, TimelineNodeId node) const override {
        TimelineNodeId cursor = node;
        while (cursor != 0) {
            const auto found = nodes_.find(cursor);
            if (found == nodes_.end()) return false;
            cursor = found->second.parent;
            if (cursor == ancestor) return true;
        }
        return false;
    }

    std::vector<TimelineNodeId> causal_ancestors(TimelineNodeId id) const override {
        std::vector<TimelineNodeId> chain;
        TimelineNodeId cursor = id;
        while (cursor != 0) {
            const auto found = nodes_.find(cursor);
            if (found == nodes_.end()) return {};
            chain.push_back(cursor);
            cursor = found->second.parent;
        }
        std::reverse(chain.begin(), chain.end());
        return chain;
    }

    TimelineNodeId common_ancestor(TimelineNodeId a, TimelineNodeId b) const override {
        std::set<TimelineNodeId> aChain;
        TimelineNodeId cursor = a;
        while (cursor != 0) {
            const auto found = nodes_.find(cursor);
            if (found == nodes_.end()) return 0;
            aChain.insert(cursor);
            cursor = found->second.parent;
        }
        cursor = b;
        while (cursor != 0) {
            const auto found = nodes_.find(cursor);
            if (found == nodes_.end()) return 0;
            if (aChain.count(cursor) != 0) return cursor;
            cursor = found->second.parent;
        }
        return 0;
    }

    std::size_t node_count() const override { return nodes_.size(); }
    void clear() override {
        nodes_.clear();
        nextId_ = 1;
    }

private:
    struct Node {
        TimelineNodeId id{ 0 };
        TimelineNodeId parent{ 0 };
        std::uint64_t revision{ 0 };
        std::vector<std::byte> payload;
        bool hasOwnPayload{ false };
        std::vector<TimelineNodeId> children;
    };

    std::map<TimelineNodeId, Node> nodes_;
    TimelineNodeId nextId_{ 1 };
};

}  // namespace

std::unique_ptr<ITimelineGraph> create_timeline_graph() {
    return std::unique_ptr<ITimelineGraph>(new TimelineGraph());
}

}  // namespace engine::timeline
