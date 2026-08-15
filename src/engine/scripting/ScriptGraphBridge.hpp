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
