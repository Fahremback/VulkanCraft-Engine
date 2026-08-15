// VisualAuthoringTests — UI-independent tests for the visual authoring models:
//   * VisualScriptCanvas  (zoom/pan, selection + marquee, copy/paste with UUID
//     remapping, typed connection validation, graph validation, groups, search,
//     undo/redo with batching, JSON persistence)
//   * MissionEditorModel  (graph editing, validation, undo/redo)
//   * DialogueEditorModel (line/choice graph, conditions, reachability, undo)
//   * AudioEditorModel    (variations, mixer buses + effects, reverb/ambient
//     zones, undo/redo, JSON + file serialization)
//
// No UI, no GPU, no audio device is required. Build/run like the other
// engine tests (standalone main() with CHECK).

#include "../src/engine/scripting/VisualScriptCanvas.hpp"
#include "../src/engine/scripting/ScriptGraphBridge.hpp"
#include "../src/editor/tools/MissionEditorModel.hpp"
#include "../src/editor/tools/DialogueEditorModel.hpp"
#include "../src/editor/tools/AudioEditorModel.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace Engine;
using namespace Engine::Editor;

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "VisualAuthoringTests failure at line " << __LINE__ << ": " #condition "\n"; \
    return false; } } while (false)

namespace {

int s_nodeCounter = 100;

ScriptPin make_pin(UUID id, std::string name, PinType type, bool isInput) {
    return ScriptPin{id, std::move(name), type, isInput, {}};
}

ScriptNode make_node(std::string title, std::vector<ScriptPin> inputs,
                     std::vector<ScriptPin> outputs) {
    ScriptNode node;
    node.id = UUID(static_cast<uint64_t>(s_nodeCounter++), 0);
    node.title = std::move(title);
    node.inputs = std::move(inputs);
    node.outputs = std::move(outputs);
    return node;
}

std::size_t count_severity(const std::vector<CanvasIssue>& issues, CanvasIssue::Severity severity) {
    std::size_t count = 0;
    for (const CanvasIssue& issue : issues) {
        if (issue.severity == severity) ++count;
    }
    return count;
}

std::size_t count_severity(const std::vector<ValidationIssue>& issues, ValidationSeverity severity) {
    std::size_t count = 0;
    for (const ValidationIssue& issue : issues) {
        if (issue.severity == severity) ++count;
    }
    return count;
}

bool has_issue(const std::vector<CanvasIssue>& issues, const std::string& fragment) {
    for (const CanvasIssue& issue : issues) {
        if (issue.message.find(fragment) != std::string::npos) return true;
    }
    return false;
}

bool has_issue(const std::vector<ValidationIssue>& issues, const std::string& fragment) {
    for (const ValidationIssue& issue : issues) {
        if (issue.message.find(fragment) != std::string::npos) return true;
    }
    return false;
}

bool near(float a, float b) { return std::abs(a - b) < 1e-4f; }

// A fully wired sample graph (no warnings, no errors).
bool build_clean_graph(VisualScriptCanvas& canvas, UUID& outNodeA, UUID& outNodeB,
                       UUID& outNodeC, UUID& outNodeD) {
    const UUID execOutA(1, 0), execInB(2, 0), execOutB(3, 0), execInC(4, 0),
               floatInC(5, 0), floatOutD(6, 0);
    const ScriptNode a = make_node("On Interact", {}, {make_pin(execOutA, "Then", PinType::Execution, false)});
    const ScriptNode b = make_node("Branch", {make_pin(execInB, "In", PinType::Execution, true)},
                                   {make_pin(execOutB, "True", PinType::Execution, false)});
    const ScriptNode c = make_node("Play Sound",
                                   {make_pin(execInC, "Play", PinType::Execution, true),
                                    make_pin(floatInC, "Volume", PinType::Float, true)},
                                   {});
    const ScriptNode d = make_node("Const 0.8", {}, {make_pin(floatOutD, "Value", PinType::Float, false)});

    outNodeA = canvas.add_node(a, {0.0f, 0.0f});
    outNodeB = canvas.add_node(b, {250.0f, 0.0f});
    outNodeC = canvas.add_node(c, {500.0f, 0.0f});
    outNodeD = canvas.add_node(d, {250.0f, 200.0f});
    std::string reason;
    return canvas.connect(execOutA, execInB, &reason) &&
           canvas.connect(execOutB, execInC, &reason) &&
           canvas.connect(floatOutD, floatInC, &reason);
}

} // namespace

bool run_all() {
    // =====================================================================
    // 1. VisualScriptCanvas: viewport zoom / pan
    // =====================================================================
    {
        VisualScriptCanvas canvas;
        CHECK(canvas.zoom() == 1.0f);
        canvas.set_zoom(2.0f);
        CHECK(canvas.zoom() == 2.0f);
        canvas.set_zoom(0.001f); // clamped
        CHECK(canvas.zoom() >= 0.1f);
        canvas.set_zoom(1000.0f); // clamped
        CHECK(canvas.zoom() <= 8.0f);

        canvas.set_zoom(1.5f);
        canvas.pan_by({100.0f, -40.0f});
        CHECK(canvas.viewport_offset() == glm::vec2(100.0f, -40.0f));

        const glm::vec2 world(320.0f, -64.0f);
        const glm::vec2 screen = canvas.world_to_screen(world);
        const glm::vec2 back = canvas.screen_to_world(screen);
        CHECK(std::abs(back.x - world.x) < 1e-4f && std::abs(back.y - world.y) < 1e-4f);
        // Identity transforms at zoom 1 / offset 0.
        canvas.set_zoom(1.0f);
        canvas.set_viewport_offset({0.0f, 0.0f});
        CHECK(canvas.world_to_screen({12.0f, 7.0f}) == glm::vec2(12.0f, 7.0f));
        CHECK(canvas.screen_to_world({12.0f, 7.0f}) == glm::vec2(12.0f, 7.0f));
    }

    // =====================================================================
    // 2. VisualScriptCanvas: node editing + dirty tracking
    // =====================================================================
    {
        VisualScriptCanvas canvas;
        const ScriptNode node = make_node("On Interact", {}, {});
        const UUID id = canvas.add_node(node, {100.0f, 200.0f});
        CHECK(id.is_valid());
        CHECK(canvas.find_node(id) != nullptr);
        CHECK(canvas.layout(id) != nullptr);
        CHECK(canvas.layout(id)->position == glm::vec2(100.0f, 200.0f));
        CHECK(canvas.dirty());
        CHECK(canvas.revision() == 1);
        canvas.mark_saved();
        CHECK(!canvas.dirty());

        // Duplicate ids are refused.
        CHECK(canvas.add_node(node, {0.0f, 0.0f}) == UUID(0, 0)); // node.id == id already used
        // Auto-generated id when the node id is invalid.
        ScriptNode unnamed = make_node("Auto", {}, {});
        unnamed.id = UUID(0, 0);
        const UUID autoId = canvas.add_node(unnamed, {0.0f, 0.0f});
        CHECK(autoId.is_valid());
        CHECK(autoId != id);

        CHECK(canvas.set_node_title(id, "Renamed"));
        CHECK(canvas.find_node(id)->title == "Renamed");
        CHECK(canvas.set_node_title(UUID(9999, 0), "x") == false);
    }

    // =====================================================================
    // 3. VisualScriptCanvas: selection + marquee
    // =====================================================================
    {
        VisualScriptCanvas canvas;
        const UUID n1 = canvas.add_node(make_node("A", {}, {}), {0.0f, 0.0f});
        const UUID n2 = canvas.add_node(make_node("B", {}, {}), {300.0f, 0.0f});
        const UUID n3 = canvas.add_node(make_node("C", {}, {}), {0.0f, 300.0f});

        canvas.select(n1);
        CHECK(canvas.selection_count() == 1 && canvas.is_selected(n1));
        canvas.select(n2, /*additive=*/true);
        CHECK(canvas.selection_count() == 2);
        canvas.select(UUID(9999, 0), true); // unknown id is ignored
        CHECK(canvas.selection_count() == 2);
        canvas.deselect(n1);
        CHECK(canvas.selection_count() == 1 && !canvas.is_selected(n1));
        canvas.clear_selection();
        CHECK(canvas.selection_count() == 0);

        // Marquee over (0,0)-(400,250) in screen space hits n1 and n2 but not n3.
        canvas.begin_marquee({0.0f, 0.0f});
        canvas.update_marquee({400.0f, 250.0f});
        const std::vector<UUID> hit = canvas.end_marquee();
        CHECK(hit.size() == 2);
        CHECK(canvas.is_selected(n1) && canvas.is_selected(n2) && !canvas.is_selected(n3));
        CHECK(canvas.end_marquee().empty()); // marquee was reset

        // Additive marquee merges instead of replacing.
        canvas.begin_marquee({0.0f, 0.0f});
        canvas.update_marquee({250.0f, 250.0f});
        const std::vector<UUID> additiveHit = canvas.end_marquee(/*additive=*/true);
        CHECK(additiveHit.size() == 1 && additiveHit[0] == n1);
        CHECK(canvas.selection_count() == 2); // n1 + n2 still selected
    }

    // =====================================================================
    // 4. VisualScriptCanvas: copy / paste with UUID remapping
    // =====================================================================
    {
        VisualScriptCanvas canvas;
        const UUID execOutA(1, 0), execInB(2, 0), execOutB(3, 0), execInC(4, 0);
        const UUID a = canvas.add_node(
            make_node("On Interact", {}, {make_pin(execOutA, "Then", PinType::Execution, false)}),
            {0.0f, 0.0f});
        const UUID b = canvas.add_node(
            make_node("Branch", {make_pin(execInB, "In", PinType::Execution, true)},
                      {make_pin(execOutB, "True", PinType::Execution, false)}),
            {200.0f, 0.0f});
        const UUID c = canvas.add_node(
            make_node("Play Sound", {make_pin(execInC, "Play", PinType::Execution, true)}, {}),
            {400.0f, 0.0f});
        CHECK(canvas.connect(execOutA, execInB));
        CHECK(canvas.connect(execOutB, execInC));
        CHECK(canvas.nodes().size() == 3 && canvas.connections().size() == 2);

        // Copy only A+B (connection A->B comes along; B->C does not).
        canvas.select(a);
        canvas.select(b, true);
        const CanvasClipboard clip = canvas.copy_selection();
        CHECK(clip.nodes.size() == 2);
        CHECK(clip.connections.size() == 1);
        CHECK(clip.connections[0].fromPinID == execOutA && clip.connections[0].toPinID == execInB);

        // Paste with explicit placement; ids must be remapped.
        const std::vector<UUID> created = canvas.paste(clip, {1000.0f, 500.0f},
                                                       /*useViewportCenter=*/false);
        CHECK(created.size() == 2);
        CHECK(canvas.nodes().size() == 5);
        CHECK(canvas.connections().size() == 3);
        CHECK(created[0] != a && created[1] != b && created[0] != created[1]);
        CHECK(!canvas.is_selected(a) && !canvas.is_selected(b));
        CHECK(canvas.selection_count() == 2); // pasted nodes end up selected

        // Relative layout preserved: (a-b) spacing equals (pasteA-pasteB) spacing.
        const glm::vec2 deltaOld = canvas.layout(a)->position - canvas.layout(b)->position;
        const glm::vec2 deltaNew = canvas.layout(created[0])->position - canvas.layout(created[1])->position;
        CHECK(std::abs(deltaOld.x - deltaNew.x) < 1e-4f && std::abs(deltaOld.y - deltaNew.y) < 1e-4f);
        // Origin lands on the requested target.
        CHECK(canvas.layout(created[0])->position == glm::vec2(1000.0f, 500.0f));

        // The pasted connection references pins owned by the pasted nodes.
        bool foundRemapped = false;
        for (const ScriptConnection& conn : canvas.connections()) {
            const UUID fromOwner = canvas.owner_node(conn.fromPinID);
            const UUID toOwner = canvas.owner_node(conn.toPinID);
            if (fromOwner == created[0] && toOwner == created[1]) foundRemapped = true;
        }
        CHECK(foundRemapped);

        // Copying a single node carries no connections.
        canvas.clear_selection();
        canvas.select(c);
        const CanvasClipboard single = canvas.copy_selection();
        CHECK(single.nodes.size() == 1 && single.connections.empty());

        // Duplicate in place.
        const std::vector<UUID> dup = canvas.duplicate_selection({50.0f, 50.0f});
        CHECK(dup.size() == 1);
        CHECK(canvas.nodes().size() == 6);

        // Paste is undoable as one step.
        canvas.undo();
        CHECK(canvas.nodes().size() == 5);
        canvas.redo();
        CHECK(canvas.nodes().size() == 6);
    }

    // =====================================================================
    // 5. VisualScriptCanvas: typed connection validation
    // =====================================================================
    {
        VisualScriptCanvas canvas;
        const UUID execOutA(1, 0), execInA(2, 0), execOutB(3, 0), execInB(4, 0), floatInB(5, 0),
                   execInC(6, 0);
        const UUID a = canvas.add_node(
            make_node("A", {make_pin(execInA, "In", PinType::Execution, true)},
                      {make_pin(execOutA, "Out", PinType::Execution, false)}),
            {0.0f, 0.0f});
        const UUID b = canvas.add_node(
            make_node("B", {make_pin(execInB, "In", PinType::Execution, true),
                            make_pin(floatInB, "Value", PinType::Float, true)},
                      {make_pin(execOutB, "Out", PinType::Execution, false)}),
            {200.0f, 0.0f});
        canvas.add_node(make_node("C", {make_pin(execInC, "In", PinType::Execution, true)}, {}),
                        {400.0f, 0.0f});

        std::string reason;
        // Valid: output -> input, same type.
        CHECK(canvas.can_connect(execOutA, execInB, &reason));
        // Wrong direction.
        CHECK(!canvas.can_connect(execInA, execInB, &reason));
        CHECK(!reason.empty());
        CHECK(!canvas.can_connect(execOutA, execOutB, &reason));
        // Type mismatch.
        CHECK(!canvas.can_connect(execOutA, floatInB, &reason));
        // Same node.
        CHECK(!canvas.can_connect(execOutA, execInA, &reason));
        // Missing pin.
        CHECK(!canvas.can_connect(UUID(999, 0), execInB, &reason));
        CHECK(!canvas.can_connect(execOutA, UUID(998, 0), &reason));

        CHECK(canvas.connect(execOutA, execInB, &reason));
        CHECK(canvas.connections().size() == 1);
        // Duplicate pair.
        CHECK(!canvas.can_connect(execOutA, execInB, &reason));
        CHECK(!canvas.connect(execOutA, execInB, &reason));
        // Input already driven by another output.
        CHECK(!canvas.can_connect(execOutB, execInB, &reason));
        CHECK(canvas.connections().size() == 1);

        // Disconnect then reconnect.
        CHECK(canvas.disconnect(execOutA, execInB));
        CHECK(canvas.connections().empty());
        CHECK(canvas.connect(execOutA, execInB, &reason));
        CHECK(canvas.disconnect(execOutA, execInB));
        CHECK(!canvas.disconnect(execOutA, execInB)); // already gone
        CHECK(canvas.connect(execOutA, execInC, &reason));
        CHECK(canvas.connections().size() == 1);
    }

    // =====================================================================
    // 6. VisualScriptCanvas: graph validation
    // =====================================================================
    {
        VisualScriptCanvas canvas;
        UUID nodeA, nodeB, nodeC, nodeD;
        CHECK(build_clean_graph(canvas, nodeA, nodeB, nodeC, nodeD));
        const std::vector<CanvasIssue> cleanIssues = canvas.validate();
        CHECK(cleanIssues.empty());

        // Dangling pin reference.
        canvas.graph().connections.push_back({UUID(777, 0), UUID(5, 0)});
        std::vector<CanvasIssue> issues = canvas.validate();
        CHECK(count_severity(issues, CanvasIssue::Severity::Error) >= 1);
        CHECK(has_issue(issues, "missing pin"));

        // Duplicate connection (inserted directly, bypassing connect()).
        canvas.graph().connections.clear();
        canvas.graph().connections = {
            {UUID(1, 0), UUID(2, 0)}, {UUID(1, 0), UUID(2, 0)}, {UUID(3, 0), UUID(4, 0)},
            {UUID(6, 0), UUID(5, 0)}};
        issues = canvas.validate();
        CHECK(has_issue(issues, "Duplicate connection"));

        // Type mismatch + multi-driver.
        canvas.graph().connections.clear();
        canvas.graph().connections = {{UUID(1, 0), UUID(2, 0)}, {UUID(3, 0), UUID(2, 0)},
                                      {UUID(1, 0), UUID(5, 0)}};
        issues = canvas.validate();
        CHECK(has_issue(issues, "type mismatch"));
        CHECK(has_issue(issues, "more than one connection"));

        // Cycle: X -> Y -> X on the execution pins (built with valid connects).
        {
            VisualScriptCanvas cycleCanvas;
            const UUID xOut(11, 0), xIn(12, 0), yOut(13, 0), yIn(14, 0);
            cycleCanvas.add_node(make_node("X", {make_pin(xIn, "In", PinType::Execution, true)},
                                           {make_pin(xOut, "Out", PinType::Execution, false)}),
                                 {0.0f, 0.0f});
            cycleCanvas.add_node(make_node("Y", {make_pin(yIn, "In", PinType::Execution, true)},
                                           {make_pin(yOut, "Out", PinType::Execution, false)}),
                                 {200.0f, 0.0f});
            CHECK(cycleCanvas.connect(xOut, yIn));
            CHECK(cycleCanvas.connect(yOut, xIn));
            const std::vector<CanvasIssue> cycleIssues = cycleCanvas.validate();
            CHECK(has_issue(cycleIssues, "cycle"));
        }

        // Disconnected node + pending pins.
        canvas.graph().connections.clear();
        canvas.graph().connections = {{UUID(1, 0), UUID(2, 0)}};
        issues = canvas.validate();
        CHECK(has_issue(issues, "disconnected"));
        CHECK(has_issue(issues, "not connected")); // pending input pins
        CHECK(count_severity(issues, CanvasIssue::Severity::Warning) >= 2);
    }

    // =====================================================================
    // 7. VisualScriptCanvas: undo / redo (+ batching)
    // =====================================================================
    {
        VisualScriptCanvas canvas;
        const UUID execOutA(1, 0), execInB(2, 0);
        const UUID a = canvas.add_node(
            make_node("A", {}, {make_pin(execOutA, "Out", PinType::Execution, false)}),
            {0.0f, 0.0f});
        const UUID b = canvas.add_node(
            make_node("B", {make_pin(execInB, "In", PinType::Execution, true)}, {}),
            {200.0f, 0.0f});
        CHECK(canvas.undo_depth() == 2);

        // Move + undo.
        CHECK(canvas.move_node(a, {50.0f, 60.0f}));
        CHECK(canvas.layout(a)->position == glm::vec2(50.0f, 60.0f));
        canvas.undo();
        CHECK(canvas.layout(a)->position == glm::vec2(0.0f, 0.0f));
        canvas.redo();
        CHECK(canvas.layout(a)->position == glm::vec2(50.0f, 60.0f));

        // Connect + undo.
        CHECK(canvas.connect(execOutA, execInB));
        canvas.undo();
        CHECK(canvas.connections().empty());
        canvas.redo();
        CHECK(canvas.connections().size() == 1);

        // Move selection + undo.
        canvas.select(a);
        canvas.select(b, true);
        CHECK(canvas.move_selection({10.0f, 20.0f}));
        CHECK(canvas.layout(a)->position == glm::vec2(60.0f, 80.0f));
        canvas.undo();
        CHECK(canvas.layout(a)->position == glm::vec2(50.0f, 60.0f));

        // Remove node + undo restores node and its connection.
        canvas.clear_selection();
        CHECK(canvas.remove_node(b));
        CHECK(canvas.find_node(b) == nullptr);
        CHECK(canvas.connections().empty());
        canvas.undo();
        CHECK(canvas.find_node(b) != nullptr);
        CHECK(canvas.connections().size() == 1);
        CHECK(canvas.layout(b)->position == glm::vec2(200.0f, 0.0f));

        // Batch: several mutations collapse into a single undo step.
        canvas.begin_batch("Bulk Edit");
        const UUID c = canvas.add_node(make_node("C", {}, {}), {0.0f, 0.0f});
        CHECK(canvas.move_node(c, {300.0f, 300.0f}));
        CHECK(canvas.set_node_title(c, "Renamed C"));
        canvas.end_batch();
        // Depth reflects the collapsed batch: adds(a,b), move(a), connect,
        // move_selection, remove(b) had been pushed, but the two undos above
        // popped move_selection and remove(b); the batch adds exactly one.
        CHECK(canvas.undo_depth() == 5);
        canvas.undo();
        CHECK(canvas.find_node(c) == nullptr); // whole batch reverted
        canvas.redo();
        CHECK(canvas.find_node(c) != nullptr);
        CHECK(canvas.find_node(c)->title == "Renamed C");
        CHECK(canvas.layout(c)->position == glm::vec2(300.0f, 300.0f));

        // clear_undo drops both stacks.
        canvas.clear_undo();
        CHECK(!canvas.can_undo() && !canvas.can_redo());
    }

    // =====================================================================
    // 8. VisualScriptCanvas: groups / comments, search, picking
    // =====================================================================
    {
        VisualScriptCanvas canvas;
        const UUID n1 = canvas.add_node(make_node("Door Lock", {}, {}), {0.0f, 0.0f});
        const UUID n2 = canvas.add_node(make_node("Door Open", {}, {}), {200.0f, 0.0f});
        const UUID n3 = canvas.add_node(make_node("Play Sound", {}, {}), {0.0f, 200.0f});

        // Title search is case-insensitive substring matching.
        CHECK(canvas.find_nodes("door").size() == 2);
        CHECK(canvas.find_nodes("DOOR").size() == 2);
        CHECK(canvas.find_nodes("sound").size() == 1);
        CHECK(canvas.find_nodes("zzz").empty());
        CHECK(canvas.find_nodes("").size() == 3);

        // Groups.
        const UUID group = canvas.add_group("Interaction", {0.0f, 0.0f}, {400.0f, 200.0f});
        CHECK(canvas.find_group(group) != nullptr);
        CHECK(canvas.add_to_group(group, n1));
        CHECK(canvas.add_to_group(group, n2));
        CHECK(canvas.add_to_group(group, UUID(9999, 0)) == false); // unknown node
        CHECK(canvas.groups_containing(n1).size() == 1);
        CHECK(canvas.groups_containing(n3).empty());
        CHECK(canvas.remove_from_group(group, n2));
        CHECK(canvas.groups_containing(n2).empty());
        CHECK(canvas.remove_group(group));
        CHECK(canvas.find_group(group) == nullptr);
        canvas.undo();
        CHECK(canvas.find_group(group) != nullptr); // group + membership restored

        // Wrap selection into a group.
        canvas.clear_selection();
        canvas.select(n1);
        canvas.select(n3, true);
        const UUID wrapped = canvas.group_selection("Comment");
        CHECK(wrapped.is_valid());
        const CanvasGroup* wg = canvas.find_group(wrapped);
        CHECK(wg != nullptr && wg->members.size() == 2);

        // Picking.
        canvas.clear_selection();
        CHECK(canvas.node_at({50.0f, 40.0f}) == n1); // inside n1's rect
        CHECK(canvas.node_at({5000.0f, 5000.0f}) == UUID(0, 0));

        // Pin anchor positions: inputs on the left edge, outputs on the right.
        const UUID outPin(1, 0), inPin(2, 0);
        const UUID node = canvas.add_node(
            make_node("Anchor", {make_pin(inPin, "In", PinType::Float, true)},
                      {make_pin(outPin, "Out", PinType::Float, false)}),
            {100.0f, 200.0f});
        const glm::vec2 inPos = canvas.pin_world_position(node, inPin);
        const glm::vec2 outPos = canvas.pin_world_position(node, outPin);
        CHECK(near(inPos.x, 100.0f) && near(inPos.y, 224.0f));
        CHECK(near(outPos.x, 260.0f) && near(outPos.y, 224.0f));
    }

    // =====================================================================
    // 9. VisualScriptCanvas: JSON persistence
    // =====================================================================
    {
        VisualScriptCanvas canvas;
        UUID a, b, c, d;
        CHECK(build_clean_graph(canvas, a, b, c, d));
        const UUID group = canvas.add_group("Logic", {0.0f, 0.0f}, {600.0f, 300.0f});
        CHECK(canvas.add_to_group(group, a));
        CHECK(canvas.add_to_group(group, b));
        canvas.set_node_title(a, "Begin");
        canvas.mark_saved();

        const std::filesystem::path dir = std::filesystem::temp_directory_path() / "vc_visual_authoring";
        std::filesystem::create_directories(dir);
        const std::filesystem::path file = dir / "canvas.vsc";
        CHECK(canvas.save_to_file(file));

        VisualScriptCanvas loaded;
        CHECK(loaded.load_from_file(file));
        CHECK(loaded.nodes().size() == 4);
        CHECK(loaded.connections().size() == 3);
        CHECK(loaded.find_node(a) != nullptr);
        CHECK(loaded.find_node(a)->title == "Begin");
        CHECK(loaded.layout(a)->position == canvas.layout(a)->position);
        CHECK(loaded.groups().size() == 1);
        CHECK(loaded.find_group(group) != nullptr);
        CHECK(loaded.find_group(group)->members.size() == 2);
        CHECK(loaded.find_group(group)->members[0] == a);
        CHECK(!loaded.dirty());
        std::filesystem::remove_all(dir);
    }

    // =====================================================================
    // 10. MissionEditorModel
    // =====================================================================
    {
        MissionEditorModel mission;
        CHECK(mission.add_node({1, MissionNodeKind::Entry, "Start", "", {0.0f, 0.0f}}));
        CHECK(mission.add_node({2, MissionNodeKind::Objective, "Enter the vehicle", "", {0.0f, 0.0f}}));
        CHECK(mission.add_node({3, MissionNodeKind::Completion, "Done", "", {0.0f, 0.0f}}));
        CHECK(mission.add_node({1, MissionNodeKind::Entry, "Dup", "", {0.0f, 0.0f}}) == false);
        CHECK(mission.add_node({0, MissionNodeKind::Entry, "Zero", "", {0.0f, 0.0f}}) == false);
        CHECK(mission.dirty());

        CHECK(mission.connect({1, 2}));
        CHECK(mission.connect({2, 3}));
        CHECK(mission.connect({1, 1}) == false);              // self loop rejected
        CHECK(mission.connect({1, 99}) == false);            // missing target
        CHECK(mission.connect({1, 2}) == false);             // duplicate edge

        // Clean mission: one entry, all edges valid, no cycles, no isolation.
        CHECK(mission.validate().empty());

        // Introduce a cycle -> validation error.
        CHECK(mission.connect({3, 1}));
        std::vector<ValidationIssue> issues = mission.validate();
        CHECK(has_issue(issues, "cycle"));
        CHECK(count_severity(issues, ValidationSeverity::Error) == 1);
        CHECK(mission.remove_edge(3, 1));
        CHECK(mission.validate().empty());

        // Move + undo.
        CHECK(mission.move_node(2, {500.0f, 500.0f}));
        CHECK(mission.find(2)->position == glm::vec2(500.0f, 500.0f));
        mission.undo();
        CHECK(mission.find(2)->position == glm::vec2(0.0f, 0.0f));
        mission.redo();
        CHECK(mission.find(2)->position == glm::vec2(500.0f, 500.0f));

        // Remove node: incident edges are dropped too; undo restores both.
        CHECK(mission.remove_node(2));
        CHECK(mission.find(2) == nullptr);
        CHECK(mission.edges.empty());
        mission.undo();
        CHECK(mission.find(2) != nullptr);
        CHECK(mission.edges.size() == 2);
        mission.redo();
        CHECK(mission.find(2) == nullptr);

        // Update + undo.
        mission.undo(); // back to the state with node 2
        CHECK(mission.update_node({2, MissionNodeKind::Objective, "Reach the bridge", "", {0.0f, 0.0f}}));
        CHECK(mission.find(2)->title == "Reach the bridge");
        mission.undo();
        CHECK(mission.find(2)->title == "Enter the vehicle");
    }

    // =====================================================================
    // 11. DialogueEditorModel
    // =====================================================================
    {
        DialogueEditorModel dialogue;
        CHECK(dialogue.add_node({1, "Guard", "Halt!", {0.0f, 0.0f}, {}}));
        CHECK(dialogue.add_node({2, "Player", "What is it?", {0.0f, 0.0f}, {}}));
        CHECK(dialogue.add_node({3, "Guard", "Move along.", {0.0f, 0.0f}, {}}));
        CHECK(dialogue.set_entry(1));
        CHECK(dialogue.set_entry(99) == false);

        CHECK(dialogue.add_choice(1, {"Ask", 2, "curious"}));
        CHECK(dialogue.add_choice(2, {"Leave", 3, ""}));
        CHECK(dialogue.add_choice(99, {"Nowhere", 0, ""}) == false);
        CHECK(dialogue.remove_choice(1, 5) == false); // out of range

        // Fully reachable graph with valid targets: no issues.
        CHECK(dialogue.validate().empty());

        // Missing target -> Error; self loop -> Warning; unreachable -> Warning.
        CHECK(dialogue.add_choice(1, {"Loop", 1, ""}));
        CHECK(dialogue.add_choice(1, {"Broken", 77, ""}));
        CHECK(dialogue.add_node({4, "Nobody", "Never seen", {0.0f, 0.0f}, {}}));
        const std::vector<ValidationIssue> issues = dialogue.validate();
        CHECK(has_issue(issues, "does not exist"));
        CHECK(has_issue(issues, "loops back"));
        CHECK(has_issue(issues, "unreachable"));
        CHECK(count_severity(issues, ValidationSeverity::Error) == 1);

        // Remove node: incoming choices are dropped, entry cleared if needed.
        CHECK(dialogue.remove_choice(1, 1)); // drop the broken choice
        CHECK(dialogue.remove_choice(1, 1)); // drop the self loop
        CHECK(dialogue.remove_node(2));
        CHECK(dialogue.find(2) == nullptr);
        // Node 1's "Ask" choice pointed at 2 and must be gone.
        bool askGone = true;
        for (const DialogueChoiceModel& choice : dialogue.find(1)->choices) {
            if (choice.target == 2) askGone = false;
        }
        CHECK(askGone);
        dialogue.undo();
        CHECK(dialogue.find(2) != nullptr);
        CHECK(dialogue.find(1)->choices.size() == 1); // "Ask" choice restored
        CHECK(dialogue.find(1)->choices[0].target == 2);
        dialogue.redo();
        CHECK(dialogue.find(2) == nullptr);
    }

    // =====================================================================
    // 12. AudioEditorModel: event, mixer, zones, undo
    // =====================================================================
    {
        AudioEditorModel audio;
        CHECK(!audio.dirty());
        audio.add_variation({UUID(1, 1), 1.0f, 0.9f, 1.1f});
        audio.add_variation({UUID(2, 2), 2.0f, 1.0f, 1.0f});
        CHECK(audio.dirty() && audio.revision() == 2);
        CHECK(audio.remove_variation(9) == false);
        CHECK(audio.set_variation(1, {UUID(2, 2), 3.0f, 0.8f, 1.2f}));
        CHECK(audio.variations[1].weight == 3.0f);
        audio.mark_saved();
        CHECK(!audio.dirty());

        // Mixer.
        const uint32_t music = audio.add_bus("Music");
        const uint32_t sfx = audio.add_bus("SFX", music);
        CHECK(music != 0 && sfx != 0);
        CHECK(audio.add_bus("Ghost", 999) == 0); // invalid parent rejected
        CHECK(audio.set_bus_gain(sfx, 0.5f));
        CHECK(audio.set_bus_muted(music, true));
        CHECK(audio.add_bus_effect(music, AudioBusEffectKind::LowPass, 0.3f));
        CHECK(audio.clear_bus_effects(sfx) == false); // sfx has no effects yet

        // Zones.
        CHECK(audio.add_reverb_zone({"Cave", {0.0f, 0.0f, 0.0f}, {5.0f, 5.0f, 5.0f}, 0.4f, 0.35f, 0.02f}));
        CHECK(audio.add_ambient_zone({"Forest", {0.0f, 0.0f, 0.0f}, {10.0f, 10.0f, 10.0f},
                                      "ambient/forest.ogg", 0.7f}));

        // Valid document: no errors and no warnings.
        CHECK(audio.validate().empty());

        // Bus hierarchy cycle -> Error.
        audio.buses[0].parent = sfx;
        std::vector<ValidationIssue> issues = audio.validate();
        CHECK(has_issue(issues, "cycle"));
        audio.buses[0].parent = 0;
        CHECK(audio.validate().empty());

        // Undo/redo of a zone add.
        audio.undo();
        CHECK(audio.ambientZones.empty());
        audio.redo();
        CHECK(audio.ambientZones.size() == 1);

        // Undo of bus removal re-parents children correctly.
        CHECK(audio.remove_bus(music));
        CHECK(audio.find_bus(music) == nullptr);
        CHECK(audio.find_bus(sfx)->parent == 0);
        audio.undo();
        CHECK(audio.find_bus(music) != nullptr);
        CHECK(audio.find_bus(sfx)->parent == music);
    }

    // =====================================================================
    // 13. AudioEditorModel: serialization round-trips
    // =====================================================================
    {
        AudioEditorModel audio;
        audio.name = "StoneImpact";
        audio.bus = "SFX";
        audio.volume = 0.8f;
        audio.minPitch = 0.9f;
        audio.maxPitch = 1.3f;
        audio.minDistance = 1.0f;
        audio.maxDistance = 40.0f;
        audio.spatial = true;
        audio.looping = false;
        audio.add_variation({UUID(1, 1), 1.0f, 0.9f, 1.1f});
        audio.add_variation({UUID(2, 2), 2.0f, 1.0f, 1.0f});
        const uint32_t master = audio.add_bus("Master");
        const uint32_t music = audio.add_bus("Music", master);
        audio.set_bus_gain(music, 0.4f);
        audio.set_bus_muted(music, true);
        audio.add_bus_effect(music, AudioBusEffectKind::Delay, 0.2f);
        audio.add_reverb_zone({"Cave", {1.0f, 2.0f, 3.0f}, {5.0f, 5.0f, 5.0f}, 0.4f, 0.35f, 0.02f});
        audio.add_ambient_zone({"Forest", {0.0f, 0.0f, 0.0f}, {10.0f, 10.0f, 10.0f},
                                "ambient/forest.ogg", 0.7f});

        // In-memory JSON round-trip.
        const std::string json = audio.to_json();
        AudioEditorModel restored;
        CHECK(restored.load_from_json_text(json));
        CHECK(restored.name == "StoneImpact");
        CHECK(restored.bus == "SFX");
        CHECK(near(restored.volume, 0.8f) && near(restored.minPitch, 0.9f) && near(restored.maxPitch, 1.3f));
        CHECK(near(restored.minDistance, 1.0f) && near(restored.maxDistance, 40.0f));
        CHECK(restored.spatial && !restored.looping);
        CHECK(restored.variations.size() == 2);
        CHECK(restored.variations[0].clip == UUID(1, 1));
        CHECK(near(restored.variations[1].weight, 2.0f));
        CHECK(restored.buses.size() == 2);
        CHECK(restored.find_bus(master) != nullptr && restored.find_bus(music) != nullptr);
        CHECK(restored.find_bus(music)->parent == master);
        CHECK(near(restored.find_bus(music)->gain, 0.4f));
        CHECK(restored.find_bus(music)->muted);
        CHECK(restored.find_bus(music)->effects.size() == 1);
        CHECK(restored.find_bus(music)->effects[0].kind == AudioBusEffectKind::Delay);
        CHECK(restored.reverbZones.size() == 1);
        CHECK(restored.reverbZones[0].name == "Cave");
        CHECK(near(restored.reverbZones[0].center.x, 1.0f) && near(restored.reverbZones[0].wet, 0.4f));
        CHECK(restored.ambientZones.size() == 1);
        CHECK(restored.ambientZones[0].clipPath == "ambient/forest.ogg");
        CHECK(!restored.dirty()); // loading a document marks it saved

        // Rejected documents leave the model untouched.
        AudioEditorModel untouched;
        untouched.add_variation({UUID(9, 9), 1.0f, 1.0f, 1.0f});
        CHECK(!untouched.load_from_json_text("{\"format\":\"Wrong.Format\"}"));
        CHECK(untouched.variations.size() == 1);
        CHECK(!untouched.load_from_json_text("not json at all"));
        CHECK(untouched.variations.size() == 1);

        // File round-trip.
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / "vc_audio_editor";
        std::filesystem::create_directories(dir);
        const std::filesystem::path file = dir / "event.json";
        CHECK(audio.save_to_file(file));
        AudioEditorModel fromFile;
        CHECK(fromFile.load_from_file(file));
        CHECK(fromFile.variations.size() == 2);
        CHECK(fromFile.buses.size() == 2);
        CHECK(fromFile.reverbZones.size() == 1 && fromFile.ambientZones.size() == 1);
        CHECK(!fromFile.load_from_file(dir / "missing.json"));
        std::filesystem::remove_all(dir);
    }

    // =====================================================================
    // 5. ScriptGraphBridge: ScriptGraphAsset <-> VisualScriptGraph round-trip
    //    (the Script Canvas panel's authoring loop, Fase 7)
    // =====================================================================
    {
        ScriptGraphAsset asset;
        const UUID eventId(50, 1), constantId(50, 2), setVarId(50, 3), addId(50, 4);
        TypedScriptNode eventNode;  eventNode.id = eventId;  eventNode.kind = ScriptNodeKind::Event; eventNode.event = "OnStart";
        TypedScriptNode constNode;  constNode.id = constantId; constNode.kind = ScriptNodeKind::ConstantFloat; constNode.literal = 1.0;
        TypedScriptNode setVarNode; setVarNode.id = setVarId; setVarNode.kind = ScriptNodeKind::SetVariable; setVarNode.variable = "health";
        TypedScriptNode addNode;    addNode.id = addId;    addNode.kind = ScriptNodeKind::AddFloat;
        asset.nodes = {eventNode, constNode, setVarNode, addNode};
        // Event flows into SetVariable; the constant feeds AddFloat's A; the
        // sum feeds SetVariable's value (every target has an input pin).
        asset.links = {{eventId, setVarId}, {constantId, addId}, {addId, setVarId}};

        // → VisualScriptGraph: kinds become typed pins; node→node links become
        //   primary-output → primary-input connections.
        const VisualScriptGraph visual = to_visual_graph(asset);
        CHECK(visual.nodes.size() == 4);
        CHECK(visual.connections.size() == 3);
        const ScriptNode* setVar = nullptr;
        for (const ScriptNode& node : visual.nodes) if (node.id == setVarId) setVar = &node;
        CHECK(setVar != nullptr);
        CHECK(setVar->inputs.size() == 2 && setVar->outputs.size() == 1);
        CHECK(setVar->inputs[1].type == PinType::Float);
        const ScriptNode* event = nullptr;
        for (const ScriptNode& node : visual.nodes) if (node.id == eventId) event = &node;
        CHECK(event != nullptr && event->title == "Event: OnStart" && !event->outputs.empty());

        // → back to ScriptGraphAsset: same nodes, same links, compiles.
        const ScriptGraphAsset back = from_visual_graph(visual);
        CHECK(back.nodes.size() == 4);
        CHECK(back.links.size() == 3);
        bool hasAdd = false, hasSetVar = false;
        for (const TypedScriptNode& node : back.nodes) {
            if (node.kind == ScriptNodeKind::AddFloat) hasAdd = true;
            if (node.kind == ScriptNodeKind::SetVariable && node.variable == "health") hasSetVar = true;
        }
        CHECK(hasAdd && hasSetVar);
        const auto compiled = ScriptCompiler::compile(back);
        CHECK(compiled);
        CHECK(!compiled.diagnostics.empty() || compiled.program.instructions.size() >= 4);

        // Canvas edit path: add a node through the canvas, convert back.
        VisualScriptCanvas canvas(visual);
        const UUID newId = canvas.add_node(make_node("Multiply Float", {}, {}), {10.0f, 10.0f});
        CHECK(newId.is_valid());
        const ScriptGraphAsset edited = from_visual_graph(canvas.graph());
        CHECK(edited.nodes.size() == 5);
        CHECK(edited.nodes[0].id == eventId); // original ids preserved
        CHECK(ScriptCompiler::compile(edited));
    }

    std::cout << "VisualAuthoringTests: all checks passed\n";
    return true;
}

int main() {
    if (!run_all()) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
