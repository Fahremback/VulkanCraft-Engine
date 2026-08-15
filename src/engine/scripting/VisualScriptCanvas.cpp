#include "VisualScriptCanvas.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include "../core/serialization/JsonMini.hpp"

namespace Engine {

namespace {

bool is_same_or_contains(const std::vector<UUID>& ids, UUID id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

void set_reason(std::string* reason, std::string message) {
    if (reason) *reason = std::move(message);
}

std::string lowercase(std::string value) {
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

// --- JSON helpers for serialization ---
Json::Value vec2_to_json(const glm::vec2& v) {
    Json::Value out = Json::Value::make_object();
    out["x"] = static_cast<double>(v.x);
    out["y"] = static_cast<double>(v.y);
    return out;
}

glm::vec2 vec2_from_json(const Json::Value& v, glm::vec2 fallback = {0.0f, 0.0f}) {
    if (!v.is_object()) return fallback;
    const Json::Value* x = v.find("x");
    const Json::Value* y = v.find("y");
    if (!x || !y || !x->is_number() || !y->is_number()) return fallback;
    return {static_cast<float>(x->as_number()), static_cast<float>(y->as_number())};
}

Json::Value pin_to_json(const ScriptPin& pin) {
    Json::Value out = Json::Value::make_object();
    out["id"] = pin.id.to_string();
    out["name"] = pin.name;
    out["type"] = static_cast<int64_t>(pin.type);
    out["isInput"] = pin.isInput;
    return out;
}

bool pin_from_json(const Json::Value& v, ScriptPin& pin) {
    const Json::Value* id = v.find("id");
    const Json::Value* name = v.find("name");
    const Json::Value* type = v.find("type");
    const Json::Value* isInput = v.find("isInput");
    if (!id || !id->is_string()) return false;
    pin.id = UUID::from_string(id->as_string());
    pin.name = name ? name->as_string() : std::string();
    pin.type = static_cast<PinType>(type ? type->as_int() : 0);
    pin.isInput = isInput ? isInput->as_bool() : true;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Graph access
// ---------------------------------------------------------------------------

ScriptNode* VisualScriptCanvas::find_node(UUID id) noexcept {
    for (ScriptNode& node : graph_.nodes) {
        if (node.id == id) return &node;
    }
    return nullptr;
}

const ScriptNode* VisualScriptCanvas::find_node(UUID id) const noexcept {
    for (const ScriptNode& node : graph_.nodes) {
        if (node.id == id) return &node;
    }
    return nullptr;
}

const ScriptPin* VisualScriptCanvas::find_pin_anywhere(UUID pinID, UUID* ownerNode) const noexcept {
    for (const ScriptNode& node : graph_.nodes) {
        for (const ScriptPin& pin : node.inputs) {
            if (pin.id == pinID) {
                if (ownerNode) *ownerNode = node.id;
                return &pin;
            }
        }
        for (const ScriptPin& pin : node.outputs) {
            if (pin.id == pinID) {
                if (ownerNode) *ownerNode = node.id;
                return &pin;
            }
        }
    }
    return nullptr;
}

UUID VisualScriptCanvas::owner_node(UUID pinID) const noexcept {
    UUID owner;
    find_pin_anywhere(pinID, &owner);
    return owner;
}

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------

UUID VisualScriptCanvas::insert_node(const ScriptNode& node, glm::vec2 position) {
    UUID id = node.id.is_valid() ? node.id : UUID();
    if (find_node(id)) return UUID(0, 0);
    ScriptNode copy = node;
    copy.id = id;
    graph_.nodes.push_back(std::move(copy));
    layouts_[id] = CanvasNodeLayout{id, position, {160.0f, 90.0f}};
    return id;
}

void VisualScriptCanvas::insert_connection(const ScriptConnection& connection) {
    graph_.connections.push_back(connection);
}

UUID VisualScriptCanvas::add_node(const ScriptNode& node, glm::vec2 position) {
    Snapshot before = snapshot();
    const UUID id = insert_node(node, position);
    if (!id.is_valid()) return UUID(0, 0);
    push_command("Add Node", std::move(before), snapshot());
    return id;
}

bool VisualScriptCanvas::remove_node(UUID id) {
    auto it = std::find_if(graph_.nodes.begin(), graph_.nodes.end(),
                           [&](const ScriptNode& node) { return node.id == id; });
    if (it == graph_.nodes.end()) return false;
    Snapshot before = snapshot();

    // Collect pin ids owned by this node so we can drop touching connections.
    std::unordered_set<UUID> ownedPins;
    for (const ScriptPin& pin : it->inputs) ownedPins.insert(pin.id);
    for (const ScriptPin& pin : it->outputs) ownedPins.insert(pin.id);
    graph_.nodes.erase(it);

    graph_.connections.erase(
        std::remove_if(graph_.connections.begin(), graph_.connections.end(),
                       [&](const ScriptConnection& c) {
                           return ownedPins.count(c.fromPinID) != 0 || ownedPins.count(c.toPinID) != 0;
                       }),
        graph_.connections.end());

    layouts_.erase(id);
    selection_.erase(std::remove(selection_.begin(), selection_.end(), id), selection_.end());
    for (CanvasGroup& group : groups_) {
        group.members.erase(std::remove(group.members.begin(), group.members.end(), id),
                            group.members.end());
    }

    push_command("Remove Node", std::move(before), snapshot());
    return true;
}

bool VisualScriptCanvas::move_node(UUID id, glm::vec2 position) {
    auto it = layouts_.find(id);
    if (it == layouts_.end()) return false;
    Snapshot before = snapshot();
    it->second.position = position;
    push_command("Move Node", std::move(before), snapshot());
    return true;
}

bool VisualScriptCanvas::move_selection(glm::vec2 delta) {
    if (selection_.empty()) return false;
    Snapshot before = snapshot();
    for (const UUID& id : selection_) {
        auto it = layouts_.find(id);
        if (it != layouts_.end()) it->second.position += delta;
    }
    push_command("Move Selection", std::move(before), snapshot());
    return true;
}

bool VisualScriptCanvas::set_node_title(UUID id, std::string title) {
    ScriptNode* node = find_node(id);
    if (!node) return false;
    Snapshot before = snapshot();
    node->title = std::move(title);
    push_command("Rename Node", std::move(before), snapshot());
    return true;
}

UUID VisualScriptCanvas::add_pin(UUID nodeID, const ScriptPin& pin) {
    ScriptNode* node = find_node(nodeID);
    if (!node) return UUID(0, 0);
    Snapshot before = snapshot();
    if (pin.isInput) {
        node->inputs.push_back(pin);
    } else {
        node->outputs.push_back(pin);
    }
    push_command("Add Pin", std::move(before), snapshot());
    return pin.id;
}

bool VisualScriptCanvas::remove_pin(UUID nodeID, UUID pinID) {
    ScriptNode* node = find_node(nodeID);
    if (!node) return false;
    const bool wasInput =
        std::any_of(node->inputs.begin(), node->inputs.end(),
                    [&](const ScriptPin& p) { return p.id == pinID; });
    auto& list = wasInput ? node->inputs : node->outputs;
    const auto it = std::find_if(list.begin(), list.end(),
                                 [&](const ScriptPin& p) { return p.id == pinID; });
    if (it == list.end()) return false;

    Snapshot before = snapshot();
    list.erase(it);
    graph_.connections.erase(
        std::remove_if(graph_.connections.begin(), graph_.connections.end(),
                       [&](const ScriptConnection& c) {
                           return c.fromPinID == pinID || c.toPinID == pinID;
                       }),
        graph_.connections.end());
    push_command("Remove Pin", std::move(before), snapshot());
    return true;
}

// ---------------------------------------------------------------------------
// Typed connections
// ---------------------------------------------------------------------------

bool VisualScriptCanvas::can_connect(UUID fromPin, UUID toPin, std::string* reason) const {
    const ScriptPin* from = find_pin_anywhere(fromPin, nullptr);
    const ScriptPin* to = find_pin_anywhere(toPin, nullptr);
    if (!from || !to) {
        set_reason(reason, "One of the pins does not exist");
        return false;
    }
    if (from->isInput) {
        set_reason(reason, "Source pin is an input; outputs connect to inputs");
        return false;
    }
    if (!to->isInput) {
        set_reason(reason, "Target pin is an output; outputs connect to inputs");
        return false;
    }
    if (from->type != to->type) {
        set_reason(reason, "Pin type mismatch: source and target must have the same type");
        return false;
    }
    const UUID fromNode = owner_node(fromPin);
    const UUID toNode = owner_node(toPin);
    if (fromNode == toNode) {
        set_reason(reason, "Cannot connect a node to itself");
        return false;
    }
    for (const ScriptConnection& c : graph_.connections) {
        if (c.fromPinID == fromPin && c.toPinID == toPin) {
            set_reason(reason, "Connection already exists");
            return false;
        }
        if (c.toPinID == toPin) {
            set_reason(reason, "Target input pin is already connected");
            return false;
        }
    }
    return true;
}

bool VisualScriptCanvas::connect(UUID fromPin, UUID toPin, std::string* reason) {
    if (!can_connect(fromPin, toPin, reason)) return false;
    Snapshot before = snapshot();
    insert_connection({fromPin, toPin});
    push_command("Connect Pins", std::move(before), snapshot());
    return true;
}

bool VisualScriptCanvas::disconnect(UUID fromPin, UUID toPin) {
    const auto it = std::find_if(graph_.connections.begin(), graph_.connections.end(),
                                 [&](const ScriptConnection& c) {
                                     return c.fromPinID == fromPin && c.toPinID == toPin;
                                 });
    if (it == graph_.connections.end()) return false;
    Snapshot before = snapshot();
    graph_.connections.erase(it);
    push_command("Disconnect Pins", std::move(before), snapshot());
    return true;
}

// ---------------------------------------------------------------------------
// Viewport
// ---------------------------------------------------------------------------

void VisualScriptCanvas::set_zoom(float zoom) {
    zoom_ = std::clamp(zoom, 0.1f, 8.0f);
}

glm::vec2 VisualScriptCanvas::screen_to_world(glm::vec2 screen) const noexcept {
    return (screen - viewportOffset_) / zoom_;
}

glm::vec2 VisualScriptCanvas::world_to_screen(glm::vec2 world) const noexcept {
    return world * zoom_ + viewportOffset_;
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

void VisualScriptCanvas::select(UUID nodeID, bool additive) {
    if (!find_node(nodeID)) return;
    if (!additive) selection_.clear();
    if (!is_same_or_contains(selection_, nodeID)) selection_.push_back(nodeID);
}

void VisualScriptCanvas::deselect(UUID nodeID) noexcept {
    selection_.erase(std::remove(selection_.begin(), selection_.end(), nodeID), selection_.end());
}

bool VisualScriptCanvas::is_selected(UUID nodeID) const noexcept {
    return is_same_or_contains(selection_, nodeID);
}

std::vector<UUID> VisualScriptCanvas::end_marquee(bool additive) {
    std::vector<UUID> hit;
    if (!marqueeStart_ || !marqueeEnd_) return hit;
    const glm::vec2 a = *marqueeStart_;
    const glm::vec2 b = *marqueeEnd_;
    const CanvasRect screenRect{{std::min(a.x, b.x), std::min(a.y, b.y)},
                                {std::max(a.x, b.x), std::max(a.y, b.y)}};
    for (const ScriptNode& node : graph_.nodes) {
        const CanvasRect worldRect = node_rect(node.id);
        const glm::vec2 topLeft = world_to_screen(worldRect.min);
        const glm::vec2 bottomRight = world_to_screen(worldRect.max);
        const CanvasRect nodeScreen{{std::min(topLeft.x, bottomRight.x), std::min(topLeft.y, bottomRight.y)},
                                    {std::max(topLeft.x, bottomRight.x), std::max(topLeft.y, bottomRight.y)}};
        if (screenRect.overlaps(nodeScreen)) hit.push_back(node.id);
    }
    if (!additive) selection_.clear();
    for (const UUID& id : hit) select(id, true);
    marqueeStart_.reset();
    marqueeEnd_.reset();
    return hit;
}

// ---------------------------------------------------------------------------
// Picking
// ---------------------------------------------------------------------------

CanvasRect VisualScriptCanvas::node_rect(UUID id) const noexcept {
    const CanvasNodeLayout* l = layout(id);
    if (!l) return {{0.0f, 0.0f}, {0.0f, 0.0f}};
    return {l->position, l->position + l->size};
}

UUID VisualScriptCanvas::node_at(glm::vec2 worldPos) const noexcept {
    // Iterate in reverse so the topmost (last drawn) node wins.
    for (auto it = graph_.nodes.rbegin(); it != graph_.nodes.rend(); ++it) {
        const CanvasRect rect = node_rect(it->id).expanded_by(4.0f);
        if (rect.contains(worldPos)) return it->id;
    }
    return UUID(0, 0);
}

glm::vec2 VisualScriptCanvas::pin_world_position(UUID nodeID, UUID pinID) const noexcept {
    const ScriptNode* node = find_node(nodeID);
    const CanvasNodeLayout* l = layout(nodeID);
    if (!node) return glm::vec2(0.0f);
    const glm::vec2 base = l ? l->position : glm::vec2(0.0f);
    const float nodeWidth = l ? l->size.x : 160.0f;
    const auto inputIt = std::find_if(node->inputs.begin(), node->inputs.end(),
                                      [&](const ScriptPin& p) { return p.id == pinID; });
    if (inputIt != node->inputs.end()) {
        const int index = static_cast<int>(inputIt - node->inputs.begin());
        return {base.x, base.y + 24.0f + static_cast<float>(index) * 22.0f};
    }
    const auto outputIt = std::find_if(node->outputs.begin(), node->outputs.end(),
                                       [&](const ScriptPin& p) { return p.id == pinID; });
    if (outputIt != node->outputs.end()) {
        const int index = static_cast<int>(outputIt - node->outputs.begin());
        return {base.x + nodeWidth, base.y + 24.0f + static_cast<float>(index) * 22.0f};
    }
    return base;
}

// ---------------------------------------------------------------------------
// Copy / paste
// ---------------------------------------------------------------------------

CanvasClipboard VisualScriptCanvas::copy_selection() const {
    CanvasClipboard clip;
    if (selection_.empty()) return clip;
    const std::unordered_set<UUID> selected(selection_.begin(), selection_.end());
    glm::vec2 minCorner{std::numeric_limits<float>::max()};
    for (const ScriptNode& node : graph_.nodes) {
        if (selected.count(node.id) == 0) continue;
        clip.nodes.push_back(node);
        const CanvasNodeLayout* l = layout(node.id);
        if (l) minCorner = glm::min(minCorner, l->position);
    }
    if (clip.nodes.empty()) return clip;
    clip.origin = minCorner;
    for (const ScriptConnection& c : graph_.connections) {
        const UUID fromNode = owner_node(c.fromPinID);
        const UUID toNode = owner_node(c.toPinID);
        if (selected.count(fromNode) != 0 && selected.count(toNode) != 0) {
            clip.connections.push_back(c);
        }
    }
    return clip;
}

std::vector<UUID> VisualScriptCanvas::paste(const CanvasClipboard& clip, glm::vec2 target,
                                            bool useViewportCenter) {
    std::vector<UUID> created;
    if (clip.nodes.empty()) return created;
    Snapshot before = snapshot();

    const glm::vec2 placement =
        useViewportCenter ? screen_to_world(glm::vec2(0.0f, 0.0f)) : target;
    const glm::vec2 offset = placement - clip.origin;

    std::unordered_map<UUID, UUID> nodeRemap;
    std::unordered_map<UUID, UUID> pinRemap;
    for (const ScriptNode& src : clip.nodes) {
        ScriptNode copy = src;
        const UUID newNode = UUID();
        nodeRemap[src.id] = newNode;
        copy.id = newNode;
        for (ScriptPin& pin : copy.inputs) {
            const UUID newPin = UUID();
            pinRemap[pin.id] = newPin;
            pin.id = newPin;
        }
        for (ScriptPin& pin : copy.outputs) {
            const UUID newPin = UUID();
            pinRemap[pin.id] = newPin;
            pin.id = newPin;
        }
        glm::vec2 srcPos{0.0f, 0.0f};
        if (const CanvasNodeLayout* l = layout(src.id)) srcPos = l->position;
        insert_node(copy, srcPos + offset);
        created.push_back(newNode);
    }
    for (const ScriptConnection& c : clip.connections) {
        const auto from = pinRemap.find(c.fromPinID);
        const auto to = pinRemap.find(c.toPinID);
        if (from != pinRemap.end() && to != pinRemap.end()) {
            insert_connection({from->second, to->second});
        }
    }

    // Select the freshly pasted cluster.
    selection_.clear();
    for (const UUID& id : created) selection_.push_back(id);

    push_command("Paste Nodes", std::move(before), snapshot());
    return created;
}

std::vector<UUID> VisualScriptCanvas::duplicate_selection(glm::vec2 offset) {
    const CanvasClipboard clip = copy_selection();
    if (clip.empty()) return {};
    return paste(clip, clip.origin + offset, /*useViewportCenter=*/false);
}

// ---------------------------------------------------------------------------
// Groups
// ---------------------------------------------------------------------------

UUID VisualScriptCanvas::add_group(std::string title, glm::vec2 position, glm::vec2 size) {
    Snapshot before = snapshot();
    const UUID id = UUID();
    groups_.push_back(CanvasGroup{id, std::move(title), position, size, {}});
    push_command("Add Group", std::move(before), snapshot());
    return id;
}

bool VisualScriptCanvas::remove_group(UUID id) {
    const auto it = std::find_if(groups_.begin(), groups_.end(),
                                 [&](const CanvasGroup& g) { return g.id == id; });
    if (it == groups_.end()) return false;
    Snapshot before = snapshot();
    groups_.erase(it);
    push_command("Remove Group", std::move(before), snapshot());
    return true;
}

bool VisualScriptCanvas::add_to_group(UUID groupID, UUID nodeID) {
    CanvasGroup* group = find_group(groupID);
    if (!group || !find_node(nodeID)) return false;
    Snapshot before = snapshot();
    if (!group->contains(nodeID)) group->members.push_back(nodeID);
    push_command("Add Node To Group", std::move(before), snapshot());
    return true;
}

bool VisualScriptCanvas::remove_from_group(UUID groupID, UUID nodeID) {
    CanvasGroup* group = find_group(groupID);
    if (!group) return false;
    Snapshot before = snapshot();
    const auto it = std::find(group->members.begin(), group->members.end(), nodeID);
    if (it == group->members.end()) return false;
    group->members.erase(it);
    push_command("Remove Node From Group", std::move(before), snapshot());
    return true;
}

UUID VisualScriptCanvas::group_selection(std::string title) {
    if (selection_.empty()) return UUID(0, 0);
    Snapshot before = snapshot();
    glm::vec2 minCorner{std::numeric_limits<float>::max()};
    glm::vec2 maxCorner{std::numeric_limits<float>::lowest()};
    for (const UUID& id : selection_) {
        const CanvasRect rect = node_rect(id);
        minCorner = glm::min(minCorner, rect.min);
        maxCorner = glm::max(maxCorner, rect.max);
    }
    const UUID id = UUID();
    CanvasGroup group;
    group.id = id;
    group.title = std::move(title);
    group.position = minCorner - glm::vec2(16.0f, 16.0f);
    group.size = (maxCorner - minCorner) + glm::vec2(32.0f, 48.0f);
    group.members = selection_;
    groups_.push_back(std::move(group));
    push_command("Group Selection", std::move(before), snapshot());
    return id;
}

std::vector<UUID> VisualScriptCanvas::groups_containing(UUID nodeID) const {
    std::vector<UUID> result;
    for (const CanvasGroup& group : groups_) {
        if (group.contains(nodeID)) result.push_back(group.id);
    }
    return result;
}

CanvasGroup* VisualScriptCanvas::find_group(UUID id) noexcept {
    const auto it = std::find_if(groups_.begin(), groups_.end(),
                                 [&](const CanvasGroup& g) { return g.id == id; });
    return it == groups_.end() ? nullptr : &*it;
}

const CanvasGroup* VisualScriptCanvas::find_group(UUID id) const noexcept {
    const auto it = std::find_if(groups_.begin(), groups_.end(),
                                 [&](const CanvasGroup& g) { return g.id == id; });
    return it == groups_.end() ? nullptr : &*it;
}

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------

std::vector<UUID> VisualScriptCanvas::find_nodes(const std::string& query) const {
    std::vector<UUID> result;
    if (query.empty()) {
        for (const ScriptNode& node : graph_.nodes) result.push_back(node.id);
        return result;
    }
    const std::string needle = lowercase(query);
    for (const ScriptNode& node : graph_.nodes) {
        if (lowercase(node.title).find(needle) != std::string::npos) {
            result.push_back(node.id);
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

namespace {

// DFS-based cycle detection over node-level adjacency built from connections.
bool detect_cycle(const VisualScriptGraph& graph, std::string* cycleNodeTitle) {
    std::unordered_map<UUID, std::vector<UUID>> adjacency;
    for (const ScriptConnection& c : graph.connections) {
        const UUID fromNode = [&]() {
            for (const ScriptNode& n : graph.nodes) {
                for (const ScriptPin& p : n.outputs) {
                    if (p.id == c.fromPinID) return n.id;
                }
            }
            return UUID();
        }();
        const UUID toNode = [&]() {
            for (const ScriptNode& n : graph.nodes) {
                for (const ScriptPin& p : n.inputs) {
                    if (p.id == c.toPinID) return n.id;
                }
            }
            return UUID();
        }();
        if (fromNode.is_valid() && toNode.is_valid() && fromNode != toNode) {
            adjacency[fromNode].push_back(toNode);
        }
    }
    std::unordered_set<UUID> visited;
    std::unordered_set<UUID> inStack;
    std::function<bool(UUID)> dfs = [&](UUID node) -> bool {
        if (inStack.count(node) != 0) return true;
        if (visited.count(node) != 0) return false;
        visited.insert(node);
        inStack.insert(node);
        const auto it = adjacency.find(node);
        if (it != adjacency.end()) {
            for (const UUID& next : it->second) {
                if (dfs(next)) return true;
            }
        }
        inStack.erase(node);
        return false;
    };
    for (const auto& entry : adjacency) {
        if (dfs(entry.first)) {
            if (cycleNodeTitle) {
                for (const ScriptNode& n : graph.nodes) {
                    if (n.id == entry.first) {
                        *cycleNodeTitle = n.title;
                        break;
                    }
                }
            }
            return true;
        }
    }
    return false;
}

} // namespace

std::vector<CanvasIssue> VisualScriptCanvas::validate() const {
    std::vector<CanvasIssue> issues;

    // Pin-level integrity: every connection endpoint must exist, must be an
    // output on the "from" side and an input on the "to" side, and must not
    // duplicate an existing connection or drive an already-connected input.
    std::unordered_set<std::pair<UUID, UUID>, std::function<std::size_t(const std::pair<UUID, UUID>&)>>
        seenConnections(8, [](const std::pair<UUID, UUID>& key) {
            return std::hash<UUID>{}(key.first) ^ (std::hash<UUID>{}(key.second) << 1);
        });
    std::unordered_map<UUID, int> incomingCount;
    for (const ScriptConnection& c : graph_.connections) {
        UUID fromOwner;
        UUID toOwner;
        const ScriptPin* from = find_pin_anywhere(c.fromPinID, &fromOwner);
        const ScriptPin* to = find_pin_anywhere(c.toPinID, &toOwner);
        if (!from || !to) {
            issues.push_back({CanvasIssue::Severity::Error, "connection",
                              "Connection references a missing pin"});
            continue;
        }
        if (from->isInput || !to->isInput) {
            issues.push_back({CanvasIssue::Severity::Error, "connection",
                              "Connection direction is invalid"});
        }
        if (from->type != to->type) {
            issues.push_back({CanvasIssue::Severity::Error, "connection",
                              "Connection type mismatch between pins"});
        }
        if (fromOwner == toOwner) {
            issues.push_back({CanvasIssue::Severity::Error, "connection",
                              "Connection links a node to itself"});
        }
        const auto key = std::make_pair(c.fromPinID, c.toPinID);
        if (seenConnections.count(key) != 0) {
            issues.push_back({CanvasIssue::Severity::Error, "connection",
                              "Duplicate connection between the same pins"});
        } else {
            seenConnections.insert(key);
        }
        ++incomingCount[c.toPinID];
        if (incomingCount[c.toPinID] > 1) {
            issues.push_back({CanvasIssue::Severity::Error, "connection",
                              "Input pin receives more than one connection"});
        }
    }

    // Cycles anywhere in the graph are invalid for execution graphs.
    std::string cycleNode;
    if (detect_cycle(graph_, &cycleNode)) {
        issues.push_back({CanvasIssue::Severity::Error, "graph",
                          "Graph contains a cycle" + (cycleNode.empty() ? std::string() :
                                                      " (involving '" + cycleNode + "')")});
    }

    // Node-level checks: isolated nodes and pending (unconnected) input pins.
    std::unordered_set<UUID> incidentPins;
    for (const ScriptConnection& c : graph_.connections) {
        incidentPins.insert(c.fromPinID);
        incidentPins.insert(c.toPinID);
    }
    for (const ScriptNode& node : graph_.nodes) {
        bool hasAnyConnection = false;
        for (const ScriptPin& pin : node.inputs) {
            if (incidentPins.count(pin.id) != 0) hasAnyConnection = true;
        }
        for (const ScriptPin& pin : node.outputs) {
            if (incidentPins.count(pin.id) != 0) hasAnyConnection = true;
        }
        if (!hasAnyConnection) {
            issues.push_back({CanvasIssue::Severity::Warning, node.title,
                              "Node is disconnected"});
        }
        // Pending pins: input pins with no incoming connection.
        for (const ScriptPin& pin : node.inputs) {
            const auto it = std::find_if(
                graph_.connections.begin(), graph_.connections.end(),
                [&](const ScriptConnection& c) { return c.toPinID == pin.id; });
            if (it == graph_.connections.end()) {
                issues.push_back({CanvasIssue::Severity::Warning, node.title,
                                  "Input pin '" + pin.name + "' is not connected"});
            }
        }
    }
    return issues;
}

// ---------------------------------------------------------------------------
// Undo / redo / batching
// ---------------------------------------------------------------------------

VisualScriptCanvas::Snapshot VisualScriptCanvas::snapshot() const {
    Snapshot state;
    state.graph = graph_;
    state.layouts.reserve(layouts_.size());
    for (const auto& entry : layouts_) state.layouts.push_back(entry.second);
    state.groups = groups_;
    return state;
}

void VisualScriptCanvas::restore(const Snapshot& state) {
    graph_ = state.graph;
    layouts_.clear();
    for (const CanvasNodeLayout& l : state.layouts) layouts_[l.nodeID] = l;
    groups_ = state.groups;
    // Drop selection entries that no longer reference existing nodes.
    selection_.erase(std::remove_if(selection_.begin(), selection_.end(),
                                    [&](UUID id) { return find_node(id) == nullptr; }),
                     selection_.end());
}

void VisualScriptCanvas::push_command(std::string name, Snapshot before, Snapshot after) {
    if (batchDepth_ > 0) {
        batchEntries_.emplace_back(std::move(before), std::move(after));
        return;
    }
    undoStack_.push_back(Command{std::move(name),
                                 [this, before = std::move(before)]() {
                                     restore(before);
                                     changed();
                                 },
                                 [this, after = std::move(after)]() {
                                     restore(after);
                                     changed();
                                 }});
    redoStack_.clear();
    changed();
}

void VisualScriptCanvas::begin_batch(std::string name) {
    if (batchDepth_ == 0) batchName_ = std::move(name);
    ++batchDepth_;
}

void VisualScriptCanvas::end_batch() {
    if (batchDepth_ == 0) return;
    --batchDepth_;
    if (batchDepth_ != 0 || batchEntries_.empty()) return;
    Snapshot before = std::move(batchEntries_.front().first);
    Snapshot after = std::move(batchEntries_.back().second);
    batchEntries_.clear();
    undoStack_.push_back(Command{std::move(batchName_),
                                 [this, before = std::move(before)]() {
                                     restore(before);
                                     changed();
                                 },
                                 [this, after = std::move(after)]() {
                                     restore(after);
                                     changed();
                                 }});
    redoStack_.clear();
    changed();
}

void VisualScriptCanvas::undo() {
    if (undoStack_.empty()) return;
    Command command = std::move(undoStack_.back());
    undoStack_.pop_back();
    command.undo();
    redoStack_.push_back(std::move(command));
}

void VisualScriptCanvas::redo() {
    if (redoStack_.empty()) return;
    Command command = std::move(redoStack_.back());
    redoStack_.pop_back();
    command.redo();
    undoStack_.push_back(std::move(command));
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

bool VisualScriptCanvas::save_to_file(const std::filesystem::path& path) const {
    Json::Value root = Json::Value::make_object();
    root["format"] = "VulkanEngine.VisualScriptCanvas";
    root["version"] = 1;
    root["graph_id"] = graph_.id.to_string();
    root["name"] = graph_.name;

    Json::Value nodes = Json::Value::make_array();
    for (const ScriptNode& node : graph_.nodes) {
        Json::Value entry = Json::Value::make_object();
        entry["id"] = node.id.to_string();
        entry["title"] = node.title;
        const CanvasNodeLayout* l = layout(node.id);
        entry["position"] = vec2_to_json(l ? l->position : glm::vec2(0.0f));
        entry["size"] = vec2_to_json(l ? l->size : glm::vec2(160.0f, 90.0f));
        Json::Value inputs = Json::Value::make_array();
        for (const ScriptPin& pin : node.inputs) inputs.push(pin_to_json(pin));
        Json::Value outputs = Json::Value::make_array();
        for (const ScriptPin& pin : node.outputs) outputs.push(pin_to_json(pin));
        entry["inputs"] = std::move(inputs);
        entry["outputs"] = std::move(outputs);
        nodes.push(std::move(entry));
    }
    root["nodes"] = std::move(nodes);

    Json::Value connections = Json::Value::make_array();
    for (const ScriptConnection& c : graph_.connections) {
        Json::Value entry = Json::Value::make_object();
        entry["from"] = c.fromPinID.to_string();
        entry["to"] = c.toPinID.to_string();
        connections.push(std::move(entry));
    }
    root["connections"] = std::move(connections);

    Json::Value groups = Json::Value::make_array();
    for (const CanvasGroup& group : groups_) {
        Json::Value entry = Json::Value::make_object();
        entry["id"] = group.id.to_string();
        entry["title"] = group.title;
        entry["position"] = vec2_to_json(group.position);
        entry["size"] = vec2_to_json(group.size);
        Json::Value members = Json::Value::make_array();
        for (const UUID& member : group.members) members.push(member.to_string());
        entry["members"] = std::move(members);
        groups.push(std::move(entry));
    }
    root["groups"] = std::move(groups);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << Json::stringify(root, 2) << "\n";
    return static_cast<bool>(out);
}

bool VisualScriptCanvas::load_from_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string document = buffer.str();
    return load_from_json_text(document);
}

bool VisualScriptCanvas::load_from_json_text(const std::string& document) {
    std::string error;
    const Json::Value root = Json::parse(document, &error);
    if (!root.is_object()) return false;
    const Json::Value* format = root.find("format");
    if (!format || format->as_string() != "VulkanEngine.VisualScriptCanvas") return false;

    VisualScriptGraph loaded;
    const Json::Value* graphId = root.find("graph_id");
    if (graphId && graphId->is_string()) {
        const UUID id = UUID::from_string(graphId->as_string());
        if (id.is_valid()) loaded.id = id;
    }
    const Json::Value* name = root.find("name");
    if (name) loaded.name = name->as_string("Visual Script Graph");

    std::vector<CanvasNodeLayout> layouts;
    const Json::Value* nodes = root.find("nodes");
    if (nodes && nodes->is_array()) {
        for (const Json::Value& entry : nodes->array()) {
            const Json::Value* idVal = entry.find("id");
            if (!idVal || !idVal->is_string()) continue;
            ScriptNode node;
            node.id = UUID::from_string(idVal->as_string());
            node.title = entry.find("title") ? entry.find("title")->as_string() : std::string();
            const Json::Value* inputs = entry.find("inputs");
            if (inputs && inputs->is_array()) {
                for (const Json::Value& pinVal : inputs->array()) {
                    ScriptPin pin;
                    if (pin_from_json(pinVal, pin)) node.inputs.push_back(pin);
                }
            }
            const Json::Value* outputs = entry.find("outputs");
            if (outputs && outputs->is_array()) {
                for (const Json::Value& pinVal : outputs->array()) {
                    ScriptPin pin;
                    if (pin_from_json(pinVal, pin)) node.outputs.push_back(pin);
                }
            }
            CanvasNodeLayout layout;
            layout.nodeID = node.id;
            const Json::Value* positionVal = entry.find("position");
            layout.position = positionVal ? vec2_from_json(*positionVal) : glm::vec2(0.0f);
            const Json::Value* sizeVal = entry.find("size");
            layout.size = sizeVal ? vec2_from_json(*sizeVal, {160.0f, 90.0f}) : glm::vec2(160.0f, 90.0f);
            if (layout.size.x <= 0.0f || layout.size.y <= 0.0f) {
                layout.size = {160.0f, 90.0f};
            }
            layouts.push_back(layout);
            loaded.nodes.push_back(std::move(node));
        }
    }

    const Json::Value* connections = root.find("connections");
    if (connections && connections->is_array()) {
        for (const Json::Value& entry : connections->array()) {
            const Json::Value* from = entry.find("from");
            const Json::Value* to = entry.find("to");
            if (!from || !to || !from->is_string() || !to->is_string()) continue;
            loaded.connections.push_back({UUID::from_string(from->as_string()),
                                          UUID::from_string(to->as_string())});
        }
    }

    std::vector<CanvasGroup> groups;
    const Json::Value* groupsVal = root.find("groups");
    if (groupsVal && groupsVal->is_array()) {
        for (const Json::Value& entry : groupsVal->array()) {
            const Json::Value* idVal = entry.find("id");
            if (!idVal || !idVal->is_string()) continue;
            CanvasGroup group;
            group.id = UUID::from_string(idVal->as_string());
            group.title = entry.find("title") ? entry.find("title")->as_string() : std::string("Group");
            const Json::Value* groupPos = entry.find("position");
            group.position = groupPos ? vec2_from_json(*groupPos) : glm::vec2(0.0f);
            const Json::Value* groupSize = entry.find("size");
            group.size = groupSize ? vec2_from_json(*groupSize, {240.0f, 140.0f}) : glm::vec2(240.0f, 140.0f);
            const Json::Value* members = entry.find("members");
            if (members && members->is_array()) {
                for (const Json::Value& member : members->array()) {
                    if (member.is_string()) group.members.push_back(UUID::from_string(member.as_string()));
                }
            }
            groups.push_back(std::move(group));
        }
    }

    graph_ = std::move(loaded);
    layouts_.clear();
    for (const CanvasNodeLayout& l : layouts) layouts_[l.nodeID] = l;
    groups_ = std::move(groups);
    selection_.clear();
    clear_undo();
    dirty_ = false;
    return true;
}

// ---------------------------------------------------------------------------
// Layout access
// ---------------------------------------------------------------------------

const CanvasNodeLayout* VisualScriptCanvas::layout(UUID nodeID) const noexcept {
    const auto it = layouts_.find(nodeID);
    return it == layouts_.end() ? nullptr : &it->second;
}

CanvasNodeLayout* VisualScriptCanvas::layout(UUID nodeID) noexcept {
    const auto it = layouts_.find(nodeID);
    return it == layouts_.end() ? nullptr : &it->second;
}

} // namespace Engine
