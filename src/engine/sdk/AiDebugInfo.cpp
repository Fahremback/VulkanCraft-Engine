// AiDebugInfo.cpp — the only TU implementing the public AI debug recorder
// (Agente 4 §3 item 40 CORE): per-tick snapshots of node visits + blackboard
// for editor/profiling. Pure std; deterministic (blackboard sorted by key).

#include "engine/ai/IAiDebugInfo.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace engine {
namespace ai {
namespace {

std::string json_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char ch : text) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += ch; break;
        }
    }
    return out;
}

class AiDebugRecorder final : public IAiDebugRecorder {
public:
    AiDebugRecorder() = default;

    void begin_tick(std::uint64_t agentId, const std::string& treeName,
                    std::uint64_t tick) override {
        current_.agentId = agentId;
        current_.treeName = treeName;
        current_.tick = tick;
        current_.nodes.clear();
        current_.blackboard.clear();
        blackboard_.clear();  // blackboard é por tick
        nodeIds_.clear();
        open_ = true;
    }

    bool node_visit(const std::string& id, const std::string& status,
                    int depth, const std::string& detail) override {
        if (!open_) return false;
        if (!nodeIds_.insert(id).second) return false;  // duplicado no tick
        AiDebugNode node;
        node.id = id;
        node.status = status;
        node.depth = depth;
        node.detail = detail;
        current_.nodes.push_back(std::move(node));
        return true;
    }

    bool blackboard_set(const std::string& key, const std::string& value) override {
        if (!open_) return false;
        blackboard_[key] = value;  // substitui
        return true;
    }

    const AiDebugSnapshot* snapshot() const override {
        // Sincroniza o blackboard (mapa ordenado) no snapshot corrente.
        current_.blackboard.clear();
        current_.blackboard.reserve(blackboard_.size());
        for (const auto& entry : blackboard_) {
            AiDebugBlackboard item;
            item.key = entry.first;
            item.value = entry.second;
            current_.blackboard.push_back(std::move(item));
        }
        return &current_;
    }

    std::string to_json() const override {
        std::ostringstream out;
        out << "{\"agentId\":" << current_.agentId << ",\"treeName\":\""
            << json_escape(current_.treeName) << "\",\"tick\":" << current_.tick
            << ",\"nodes\":[";
        for (std::size_t i = 0; i < current_.nodes.size(); ++i) {
            if (i != 0) out << ",";
            const AiDebugNode& node = current_.nodes[i];
            out << "{\"id\":\"" << json_escape(node.id) << "\",\"status\":\""
                << json_escape(node.status) << "\",\"depth\":" << node.depth
                << ",\"detail\":\"" << json_escape(node.detail) << "\"}";
        }
        out << "],\"blackboard\":[";
        bool first = true;
        for (const auto& entry : blackboard_) {  // map: ordem crescente
            if (!first) out << ",";
            first = false;
            out << "{\"key\":\"" << json_escape(entry.first) << "\",\"value\":\""
                << json_escape(entry.second) << "\"}";
        }
        out << "]}";
        return out.str();
    }

    void clear() override {
        current_ = AiDebugSnapshot{};
        blackboard_.clear();
        nodeIds_.clear();
        open_ = false;
    }

private:
    mutable AiDebugSnapshot current_;  // snapshot() é const mas sincroniza o bb
    std::map<std::string, std::string> blackboard_;
    std::set<std::string> nodeIds_;
    bool open_{ false };
};

}  // namespace

std::unique_ptr<IAiDebugRecorder> create_ai_debug_recorder() {
    return std::make_unique<AiDebugRecorder>();
}

}  // namespace ai
}  // namespace engine
