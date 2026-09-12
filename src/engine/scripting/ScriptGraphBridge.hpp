#pragma once

// ScriptGraphBridge — converts between the two visual script models:
//
//   ScriptGraphAsset  — the executable model the ScriptCompiler/VM consume
//                       (TypedScriptNode kinds + node→node links, JSON .script).
//   VisualScriptGraph — the authorable model the VisualScriptCanvas edits
//                       (typed pins, execution wires, layout, undo/redo).
//
// The editor's Script Canvas panel loads the scene's .script into a
// VisualScriptCanvas through here, lets the user edit it graphically, and
// writes it back. The bridge is pure and fully testable without any UI
// (Fase 7 — authoring gráfico profundo).

#include "VisualScriptGraph.hpp"
#include "ScriptRuntime.hpp"
#include "engine/scripting/IScriptingBridge.hpp"

#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace engine::entity { class IEntityWorld; }

namespace Engine {

/// Converts an executable script asset into an authorable typed-pin graph.
/// Every node kind maps to a node with typed input/output pins; every
/// node→node link becomes a connection from the source node's primary output
/// pin to the target node's primary input pin. Unknown/empty assets produce an
/// empty graph (never fails).
VisualScriptGraph to_visual_graph(const ScriptGraphAsset& asset);

/// Converts an authored typed-pin graph back into an executable script asset.
/// Connections are collapsed to node→node links (by owner of each pin);
/// nodes that reference no executable kind map to Event/ConstantFloat/Return
/// heuristically so the graph always compiles.
ScriptGraphAsset from_visual_graph(const VisualScriptGraph& graph, const ScriptGraphAsset& original = {});

} // namespace Engine

namespace engine::scripting {

// Internal composition contract used by the real visual/Luau runtime.  The
// public scripting surface remains IScriptingBridge; this factory binds it to
// the canonical IEntityWorld instead of maintaining a second script-only ECS.
struct EcsScriptingBridgeConfig {
    std::string context_id{"runtime.scripting"};
    std::vector<BridgePermission> permissions;
    std::string spawn_type{"script.entity"};
    std::unordered_map<std::string, std::string> component_schemas;
    std::function<bool(const std::string&, const std::string&, std::string&)> event_sink;
};

std::unique_ptr<IScriptingBridge> create_ecs_scripting_bridge(
    engine::entity::IEntityWorld& world, EcsScriptingBridgeConfig config);

} // namespace engine::scripting
