#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>
#include <filesystem>
#include <any>
#include "../core/uuid/UUID.hpp"
#include "../scene/Scene.hpp"

namespace Engine {

enum class PinType {
    Execution,
    Boolean,
    Integer,
    Float,
    Vector3,
    EntityRef,
    AssetRef,
    Event
};

struct ScriptPin {
    UUID id;
    std::string name;
    PinType type{ PinType::Execution };
    bool isInput{ true };
    std::any defaultValue;
};

struct ScriptNode {
    UUID id;
    std::string title;
    std::vector<ScriptPin> inputs;
    std::vector<ScriptPin> outputs;
    std::function<void(Scene*, Entity)> executionCallback;
};

struct ScriptConnection {
    UUID fromPinID;
    UUID toPinID;
};

class VisualScriptGraph {
public:
    UUID id;
    std::string name{ "Visual Script Graph" };

    std::vector<ScriptNode> nodes;
    std::vector<ScriptConnection> connections;

    void add_node(const ScriptNode& node) {
        nodes.push_back(node);
    }

    void connect_pins(UUID fromPin, UUID toPin) {
        connections.push_back({ fromPin, toPin });
    }

    void execute_event(const std::string& eventName, Scene* scene, Entity instigator) {
        for (const auto& node : nodes) {
            if (node.title == eventName && node.executionCallback) {
                node.executionCallback(scene, instigator);
            }
        }
    }

    bool save_to_file(const std::filesystem::path& path) const;
    bool load_from_file(const std::filesystem::path& path);
};

} // namespace Engine
