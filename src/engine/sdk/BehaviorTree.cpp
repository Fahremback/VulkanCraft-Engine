// BehaviorTree.cpp — the ONLY TU with the reusable-AI runtime (agente 4 §3).
// Blackboard (typed key/value store) + data-driven behavior tree (composites,
// decorators, leaves), all pure and deterministic: time only enters through
// the dt passed to tick(); no RNG, no wall clock. JSON parse/emit uses the
// shared RegistryJson helpers (same canonical parser every data-driven
// contract uses).

#include "engine/ai/IBehaviorTree.hpp"

#include "RegistryJson.hpp"

#include <cmath>
#include <cstdint>
#include <sstream>

namespace engine {
namespace ai {

namespace {

constexpr const char* kBlackboardVersion = "1";

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

bool is_finite_number(double v) {
    return std::isfinite(v);
}

const char* kind_name(BlackboardKind kind) {
    switch (kind) {
        case BlackboardKind::Bool: return "bool";
        case BlackboardKind::Number: return "number";
        case BlackboardKind::String: return "string";
        case BlackboardKind::None: break;
    }
    return "none";
}

bool kind_from_name(const std::string& name, BlackboardKind& out) {
    if (name == "bool") { out = BlackboardKind::Bool; return true; }
    if (name == "number") { out = BlackboardKind::Number; return true; }
    if (name == "string") { out = BlackboardKind::String; return true; }
    return false;
}

const char* status_name(BehaviorStatus status) {
    switch (status) {
        case BehaviorStatus::Success: return "success";
        case BehaviorStatus::Failure: return "failure";
        case BehaviorStatus::Running: return "running";
    }
    return "running";
}

// Serializes a blackboard value to a JSON object {"type":..,"value":..}.
std::string value_to_json(const BlackboardValue& value) {
    std::ostringstream out;
    out << "{\"type\":\"" << kind_name(value.kind) << "\",\"value\":";
    switch (value.kind) {
        case BlackboardKind::Bool:
            out << (value.boolean ? "true" : "false");
            break;
        case BlackboardKind::Number:
            // Bit-exact %.9g — the canonical numeric emitter convention.
            {
                std::ostringstream num;
                num.precision(9);
                num << value.number;
                out << num.str();
            }
            break;
        case BlackboardKind::String:
            out << '"' << json_escape(value.text) << '"';
            break;
        case BlackboardKind::None:
            out << "null";
            break;
    }
    out << '}';
    return out.str();
}

// Parses a blackboard value object; returns false on unknown type/malformed.
bool value_from_json(const sdk::JsonValue& obj, BlackboardValue& out,
                     std::string& errorOut) {
    const sdk::JsonValue* typeField = obj.field("type");
    if (typeField == nullptr || !typeField->is_string()) {
        errorOut = "blackboard value missing type";
        return false;
    }
    BlackboardKind kind;
    if (!kind_from_name(typeField->string, kind)) {
        errorOut = "unknown blackboard value type: " + typeField->string;
        return false;
    }
    const sdk::JsonValue* valueField = obj.field("value");
    if (valueField == nullptr) {
        errorOut = "blackboard value missing value";
        return false;
    }
    out.kind = kind;
    switch (kind) {
        case BlackboardKind::Bool:
            if (valueField->kind != sdk::JsonValue::Kind::Bool) {
                errorOut = "blackboard bool value must be boolean";
                return false;
            }
            out.boolean = valueField->boolean;
            break;
        case BlackboardKind::Number:
            if (valueField->kind != sdk::JsonValue::Kind::Number) {
                errorOut = "blackboard number value must be numeric";
                return false;
            }
            out.number = valueField->number;
            break;
        case BlackboardKind::String:
            if (valueField->kind != sdk::JsonValue::Kind::String) {
                errorOut = "blackboard string value must be a string";
                return false;
            }
            out.text = valueField->string;
            break;
        case BlackboardKind::None:
            break;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Blackboard
// ---------------------------------------------------------------------------

void Blackboard::set(const std::string& key, bool value) {
    BlackboardValue v;
    v.kind = BlackboardKind::Bool;
    v.boolean = value;
    entries_[key] = v;
}

void Blackboard::set(const std::string& key, double value) {
    BlackboardValue v;
    v.kind = BlackboardKind::Number;
    v.number = value;
    entries_[key] = v;
}

void Blackboard::set(const std::string& key, const std::string& value) {
    BlackboardValue v;
    v.kind = BlackboardKind::String;
    v.text = value;
    entries_[key] = v;
}

void Blackboard::set(const std::string& key, const char* value) {
    BlackboardValue v;
    v.kind = BlackboardKind::String;
    v.text = value != nullptr ? value : std::string();
    entries_[key] = v;
}

bool Blackboard::get(const std::string& key, BlackboardValue& out) const {
    const auto found = entries_.find(key);
    if (found == entries_.end()) return false;
    out = found->second;
    return true;
}

bool Blackboard::has(const std::string& key) const {
    return entries_.find(key) != entries_.end();
}

bool Blackboard::erase(const std::string& key) {
    return entries_.erase(key) != 0;
}

void Blackboard::clear() {
    entries_.clear();
}

std::size_t Blackboard::size() const {
    return entries_.size();
}

std::vector<std::string> Blackboard::keys() const {
    std::vector<std::string> result;
    result.reserve(entries_.size());
    for (const auto& [key, value] : entries_) result.push_back(key);
    return result;
}

std::string Blackboard::to_json() const {
    std::ostringstream out;
    out << "{\"version\":" << kBlackboardVersion << ",\"entries\":{";
    bool first = true;
    for (const auto& [key, value] : entries_) {
        if (!first) out << ',';
        first = false;
        out << '"' << json_escape(key) << "\":" << value_to_json(value);
    }
    out << "}}";
    return out.str();
}

bool Blackboard::load_from_json(const std::string& jsonText,
                                std::string& errorOut) {
    sdk::JsonValue doc;
    if (!sdk::json_parse(jsonText, doc, errorOut)) return false;
    if (!doc.is_object()) {
        errorOut = "blackboard document must be an object";
        return false;
    }
    // Version field is optional-but-1 (forward-compat refusal pattern: an
    // unknown version is refused, never silently downgraded).
    const sdk::JsonValue* versionField = doc.field("version");
    if (versionField != nullptr && versionField->kind == sdk::JsonValue::Kind::Number &&
        static_cast<int>(versionField->number) != 1) {
        errorOut = "unsupported blackboard version";
        return false;
    }
    const sdk::JsonValue* entriesField = doc.field("entries");
    if (entriesField == nullptr || !entriesField->is_object()) {
        errorOut = "blackboard missing entries object";
        return false;
    }
    // Parse into a temporary store, then commit (all-or-nothing).
    std::map<std::string, BlackboardValue> parsed;
    for (const auto& [key, raw] : entriesField->object) {
        BlackboardValue value;
        if (!raw.is_object()) {
            errorOut = "blackboard entry \"" + key + "\" must be an object";
            return false;
        }
        if (!value_from_json(raw, value, errorOut)) return false;
        parsed[key] = value;
    }
    entries_.swap(parsed);
    return true;
}

// ---------------------------------------------------------------------------
// BehaviorNode / BehaviorTreeSpec — validation + JSON
// ---------------------------------------------------------------------------

namespace {

bool node_validate(const BehaviorNode& node, std::string& errorOut) {
    const std::string& type = node.type;
    if (type == "sequence" || type == "selector") {
        if (node.children.empty()) {
            errorOut = type + " requires at least one child";
            return false;
        }
        if (node.abort != "none" && node.abort != "self") {
            errorOut = type + " has unknown abort: " + node.abort;
            return false;
        }
    } else if (type == "parallel") {
        if (node.children.empty()) {
            errorOut = "parallel requires at least one child";
            return false;
        }
        if (node.policy != "all" && node.policy != "any") {
            errorOut = "parallel has unknown policy: " + node.policy;
            return false;
        }
    } else if (type == "inverter" || type == "succeeder" || type == "failer" ||
               type == "repeat" || type == "service" || type == "cooldown") {
        if (node.children.size() != 1) {
            errorOut = type + " requires exactly one child";
            return false;
        }
        if (type == "repeat" && node.times <= 0) {
            errorOut = "repeat times must be > 0";
            return false;
        }
        if (type == "service" &&
            (!is_finite_number(node.intervalSeconds) || node.intervalSeconds < 0.0)) {
            errorOut = "service intervalSeconds must be finite and >= 0";
            return false;
        }
        if (type == "cooldown" &&
            (!is_finite_number(node.cooldownSeconds) || node.cooldownSeconds < 0.0)) {
            errorOut = "cooldown cooldownSeconds must be finite and >= 0";
            return false;
        }
    } else if (type == "condition") {
        if (node.key.empty()) {
            errorOut = "condition requires a key";
            return false;
        }
        if (node.op != "eq" && node.op != "ne" && node.op != "lt" &&
            node.op != "lte" && node.op != "gt" && node.op != "gte" &&
            node.op != "exists" && node.op != "not_exists") {
            errorOut = "condition has unknown op: " + node.op;
            return false;
        }
        const bool presenceOp = (node.op == "exists" || node.op == "not_exists");
        if (presenceOp && node.value.kind != BlackboardKind::None) {
            errorOut = "condition op \"" + node.op + "\" must not carry a value";
            return false;
        }
        if (!presenceOp && node.value.kind == BlackboardKind::None) {
            errorOut = "condition op \"" + node.op + "\" requires a value";
            return false;
        }
    } else if (type == "action") {
        if (node.key.empty()) {
            errorOut = "action requires a key";
            return false;
        }
        if (node.value.kind == BlackboardKind::None) {
            errorOut = "action requires a value";
            return false;
        }
    } else if (type == "wait") {
        if (!is_finite_number(node.waitSeconds) || node.waitSeconds < 0.0) {
            errorOut = "wait waitSeconds must be finite and >= 0";
            return false;
        }
    } else {
        errorOut = "unknown node type: " + type;
        return false;
    }
    for (const BehaviorNode& child : node.children) {
        if (!node_validate(child, errorOut)) return false;
    }
    return true;
}

// Deterministic node-id path builder ("0", "0.0", "1", ...).
void node_to_json(const BehaviorNode& node, const std::string& id,
                  std::ostringstream& out);

void emit_node(const BehaviorNode& node, std::ostringstream& out) {
    out << "{\"type\":\"" << json_escape(node.type) << '"';
    if (node.type == "parallel") {
        out << ",\"policy\":\"" << json_escape(node.policy) << '"';
    } else if (node.type == "sequence" || node.type == "selector") {
        out << ",\"abort\":\"" << json_escape(node.abort) << '"';
    } else if (node.type == "repeat") {
        out << ",\"times\":" << node.times;
    } else if (node.type == "service") {
        out << ",\"intervalSeconds\":";
        std::ostringstream num;
        num.precision(9);
        num << node.intervalSeconds;
        out << num.str();
    } else if (node.type == "cooldown") {
        out << ",\"cooldownSeconds\":";
        std::ostringstream num;
        num.precision(9);
        num << node.cooldownSeconds;
        out << num.str();
    } else if (node.type == "condition") {
        out << ",\"key\":\"" << json_escape(node.key) << "\",\"op\":\""
            << json_escape(node.op) << "\",\"value\":" << value_to_json(node.value);
    } else if (node.type == "action") {
        out << ",\"key\":\"" << json_escape(node.key) << "\",\"value\":"
            << value_to_json(node.value);
    } else if (node.type == "wait") {
        out << ",\"waitSeconds\":";
        std::ostringstream num;
        num.precision(9);
        num << node.waitSeconds;
        out << num.str();
    }
    if (!node.children.empty()) {
        out << ",\"children\":[";
        for (std::size_t i = 0; i < node.children.size(); ++i) {
            if (i != 0) out << ',';
            emit_node(node.children[i], out);
        }
        out << ']';
    }
    out << '}';
}

void node_to_json(const BehaviorNode& node, const std::string& id,
                  std::ostringstream& out) {
    (void)id;
    emit_node(node, out);
}

// Parses a node JSON object. The caller has already verified obj.is_object().
bool node_from_json(const sdk::JsonValue& obj, BehaviorNode& out,
                    std::string& errorOut) {
    const sdk::JsonValue* typeField = obj.field("type");
    if (typeField == nullptr || !typeField->is_string() || typeField->string.empty()) {
        errorOut = "node missing type";
        return false;
    }
    out.type = typeField->string;

    if (const sdk::JsonValue* policy = obj.field("policy"); policy != nullptr) {
        out.policy = sdk::json_string(obj, "policy", out.policy);
    }
    if (const sdk::JsonValue* abort = obj.field("abort"); abort != nullptr) {
        out.abort = sdk::json_string(obj, "abort", out.abort);
    }
    out.times = static_cast<int>(sdk::json_number(obj, "times", 1));
    out.intervalSeconds = sdk::json_number(obj, "intervalSeconds", 0.0);
    out.cooldownSeconds = sdk::json_number(obj, "cooldownSeconds", 0.0);
    out.key = sdk::json_string(obj, "key", "");
    out.op = sdk::json_string(obj, "op", "eq");
    out.waitSeconds = sdk::json_number(obj, "waitSeconds", 0.0);

    if (const sdk::JsonValue* value = obj.field("value"); value != nullptr) {
        if (!value_from_json(*value, out.value, errorOut)) return false;
    } else {
        out.value = BlackboardValue{};
    }

    if (const sdk::JsonValue* children = obj.field("children"); children != nullptr) {
        if (!children->is_array()) {
            errorOut = "node children must be an array";
            return false;
        }
        out.children.clear();
        out.children.reserve(children->array.size());
        for (const sdk::JsonValue& child : children->array) {
            BehaviorNode parsed;
            if (!child.is_object()) {
                errorOut = "node child must be an object";
                return false;
            }
            if (!node_from_json(child, parsed, errorOut)) return false;
            out.children.push_back(std::move(parsed));
        }
    }
    return true;
}

}  // namespace

bool BehaviorNode::validate(std::string& errorOut) const {
    errorOut.clear();
    return node_validate(*this, errorOut);
}

bool BehaviorTreeSpec::validate(std::string& errorOut) const {
    errorOut.clear();
    if (version != 1) {
        errorOut = "unsupported behavior tree version";
        return false;
    }
    if (root.type.empty()) {
        errorOut = "behavior tree root has no type";
        return false;
    }
    return node_validate(root, errorOut);
}

std::string BehaviorTreeSpec::to_json() const {
    std::ostringstream out;
    out << "{\"version\":1,\"root\":";
    emit_node(root, out);
    out << '}';
    return out.str();
}

bool BehaviorTreeSpec::load_from_json(const std::string& jsonText,
                                      std::string& errorOut) {
    sdk::JsonValue doc;
    if (!sdk::json_parse(jsonText, doc, errorOut)) return false;
    if (!doc.is_object()) {
        errorOut = "behavior tree document must be an object";
        return false;
    }
    const int version = static_cast<int>(sdk::json_number(doc, "version", 1));
    if (version != 1) {
        errorOut = "unsupported behavior tree version";
        return false;
    }
    const sdk::JsonValue* rootField = doc.field("root");
    if (rootField == nullptr || !rootField->is_object()) {
        errorOut = "behavior tree missing root object";
        return false;
    }
    BehaviorNode parsedRoot;
    if (!node_from_json(*rootField, parsedRoot, errorOut)) return false;
    BehaviorTreeSpec candidate;
    candidate.version = version;
    candidate.root = std::move(parsedRoot);
    if (!candidate.validate(errorOut)) return false;
    *this = std::move(candidate);
    return true;
}

// ---------------------------------------------------------------------------
// Runtime
// ---------------------------------------------------------------------------

namespace {

// A compiled tree is the spec node plus per-node runtime timers, walked
// directly on the value tree (no heap per node; the spec is the blueprint and
// the runtime overlays timers by node-id path).
struct RuntimeNode {
    const BehaviorNode* node{ nullptr };
    std::string id;
    // Traversal state (which child was Running, elapsed service/wait/cooldown).
    std::size_t runningChild{ 0 };
    bool hasRunningChild{ false };
    double elapsed{ 0.0 };     // service/wait/cooldown accumulator
    int repeatCount{ 0 };      // completed repeat iterations
    bool started{ false };     // repeat/service need an explicit start guard
};

class BehaviorTreeRuntime final : public IBehaviorTree {
public:
    explicit BehaviorTreeRuntime(const BehaviorTreeSpec& spec) : spec_(spec) {
        build_runtime(spec_.root, "0");
    }

    BehaviorStatus tick(double dt, Blackboard& blackboard) override {
        trace_.clear();
        if (!std::isfinite(dt) || dt < 0.0) return BehaviorStatus::Failure;
        return tick_node("0", dt, blackboard);
    }

    void reset() override {
        runtime_.clear();
        trace_.clear();
        build_runtime(spec_.root, "0");
    }

    std::vector<std::pair<std::string, BehaviorStatus>> debug_trace()
        const override {
        return trace_;
    }

private:
    // Builds the flat runtime overlay, indexed by node id (deterministic path).
    void build_runtime(const BehaviorNode& node, const std::string& id) {
        RuntimeNode rn;
        rn.node = &node;
        rn.id = id;
        runtime_.emplace(id, rn);
        for (std::size_t i = 0; i < node.children.size(); ++i) {
            build_runtime(node.children[i],
                          id + "." + std::to_string(i));
        }
    }

    RuntimeNode& get(const std::string& id) { return runtime_.at(id); }
    const RuntimeNode& get(const std::string& id) const {
        return runtime_.at(id);
    }

    BehaviorStatus tick_node(const std::string& id, double dt,
                             Blackboard& blackboard) {
        RuntimeNode& rn = get(id);
        const BehaviorNode& node = *rn.node;
        const std::string& type = node.type;

        BehaviorStatus status = BehaviorStatus::Failure;
        if (type == "sequence") {
            status = tick_sequence(id, dt, blackboard);
        } else if (type == "selector") {
            status = tick_selector(id, dt, blackboard);
        } else if (type == "parallel") {
            status = tick_parallel(id, dt, blackboard);
        } else if (type == "inverter") {
            status = tick_inverter(id, dt, blackboard);
        } else if (type == "succeeder") {
            tick_node(id + ".0", dt, blackboard);
            status = BehaviorStatus::Success;
        } else if (type == "failer") {
            tick_node(id + ".0", dt, blackboard);
            status = BehaviorStatus::Failure;
        } else if (type == "repeat") {
            status = tick_repeat(id, dt, blackboard);
        } else if (type == "service") {
            status = tick_service(id, dt, blackboard);
        } else if (type == "cooldown") {
            status = tick_cooldown(id, dt, blackboard);
        } else if (type == "condition") {
            status = tick_condition(node, blackboard);
        } else if (type == "action") {
            status = tick_action(node, blackboard);
        } else if (type == "wait") {
            status = tick_wait(rn, dt);
        } else {
            status = BehaviorStatus::Failure;
        }

        trace_.emplace_back(id, status);
        return status;
    }

    // Sequence: tick children in order. On "abort":"self", re-tick from the
    // FIRST every tick (so a flipped condition aborts a running later child);
    // on "abort":"none", resume the last running child (memorized).
    BehaviorStatus tick_sequence(const std::string& id, double dt,
                                 Blackboard& blackboard) {
        RuntimeNode& rn = get(id);
        const BehaviorNode& node = *rn.node;
        const bool reactive = (node.abort == "self");
        std::size_t start = reactive ? 0 : (rn.hasRunningChild ? rn.runningChild : 0);
        if (!reactive) rn.hasRunningChild = false;

        for (std::size_t i = start; i < node.children.size(); ++i) {
            const std::string childId = id + "." + std::to_string(i);
            const BehaviorStatus child = tick_node(childId, dt, blackboard);
            if (child == BehaviorStatus::Failure) return BehaviorStatus::Failure;
            if (child == BehaviorStatus::Running) {
                rn.runningChild = i;
                rn.hasRunningChild = true;
                return BehaviorStatus::Running;
            }
        }
        return BehaviorStatus::Success;
    }

    // Selector (fallback): tick children until one succeeds. Reactive
    // re-evaluation: always start from the first (a selector's earlier
    // conditions must be re-checked).
    BehaviorStatus tick_selector(const std::string& id, double dt,
                                 Blackboard& blackboard) {
        RuntimeNode& rn = get(id);
        const BehaviorNode& node = *rn.node;
        for (std::size_t i = 0; i < node.children.size(); ++i) {
            const std::string childId = id + "." + std::to_string(i);
            const BehaviorStatus child = tick_node(childId, dt, blackboard);
            if (child == BehaviorStatus::Success) return BehaviorStatus::Success;
            if (child == BehaviorStatus::Running) {
                rn.runningChild = i;
                rn.hasRunningChild = true;
                return BehaviorStatus::Running;
            }
        }
        return BehaviorStatus::Failure;
    }

    // Parallel: tick ALL children every tick. policy "all" succeeds only when
    // every child succeeds (fails on the first failure); "any" succeeds on the
    // first success. Running propagates when no decisive result is reached.
    BehaviorStatus tick_parallel(const std::string& id, double dt,
                                 Blackboard& blackboard) {
        const BehaviorNode& node = *get(id).node;
        const bool allPolicy = (node.policy == "all");
        bool anyRunning = false;
        for (std::size_t i = 0; i < node.children.size(); ++i) {
            const std::string childId = id + "." + std::to_string(i);
            const BehaviorStatus child = tick_node(childId, dt, blackboard);
            if (allPolicy) {
                if (child == BehaviorStatus::Failure) return BehaviorStatus::Failure;
                if (child == BehaviorStatus::Running) anyRunning = true;
            } else {
                if (child == BehaviorStatus::Success) return BehaviorStatus::Success;
                if (child == BehaviorStatus::Running) anyRunning = true;
            }
        }
        if (anyRunning) return BehaviorStatus::Running;
        return allPolicy ? BehaviorStatus::Success : BehaviorStatus::Failure;
    }

    BehaviorStatus tick_inverter(const std::string& id, double dt,
                                 Blackboard& blackboard) {
        const BehaviorStatus child = tick_node(id + ".0", dt, blackboard);
        if (child == BehaviorStatus::Running) return BehaviorStatus::Running;
        return child == BehaviorStatus::Success ? BehaviorStatus::Failure
                                                : BehaviorStatus::Success;
    }

    // Repeat: run the child `times` times; the child must SUCCEED each
    // iteration. A failure aborts; a Running child is resumed.
    BehaviorStatus tick_repeat(const std::string& id, double dt,
                               Blackboard& blackboard) {
        RuntimeNode& rn = get(id);
        const BehaviorNode& node = *rn.node;
        while (rn.repeatCount < node.times) {
            const BehaviorStatus child = tick_node(id + ".0", dt, blackboard);
            if (child == BehaviorStatus::Failure) return BehaviorStatus::Failure;
            if (child == BehaviorStatus::Running) return BehaviorStatus::Running;
            // Success: count one completed iteration and continue.
            ++rn.repeatCount;
            reset_child(id + ".0");
        }
        return BehaviorStatus::Success;
    }

    // Service: runs the child at most once per intervalSeconds; between runs
    // it returns Running and never terminates on its own (a periodic
    // side-effect node).
    BehaviorStatus tick_service(const std::string& id, double dt,
                                Blackboard& blackboard) {
        RuntimeNode& rn = get(id);
        const BehaviorNode& node = *rn.node;
        rn.elapsed += dt;
        if (!rn.started || rn.elapsed >= node.intervalSeconds) {
            rn.elapsed = 0.0;
            rn.started = true;
            tick_node(id + ".0", dt, blackboard);
        }
        return BehaviorStatus::Running;
    }

    // Cooldown: runs the child only when the cooldown has elapsed; the child's
    // result is the result. Between runs it returns Running (the parent keeps
    // waiting). Once the child completes, the cooldown restarts.
    BehaviorStatus tick_cooldown(const std::string& id, double dt,
                                 Blackboard& blackboard) {
        RuntimeNode& rn = get(id);
        const BehaviorNode& node = *rn.node;
        if (!rn.started) {
            rn.started = true;
            rn.elapsed = node.cooldownSeconds;  // ready immediately on first tick
        }
        if (rn.elapsed >= node.cooldownSeconds) {
            const BehaviorStatus child = tick_node(id + ".0", dt, blackboard);
            if (child != BehaviorStatus::Running) {
                rn.elapsed = 0.0;  // child finished; restart the cooldown
            }
            return child;
        }
        rn.elapsed += dt;
        return BehaviorStatus::Running;
    }

    BehaviorStatus tick_condition(const BehaviorNode& node,
                                  const Blackboard& blackboard) {
        const bool present = blackboard.has(node.key);
        if (node.op == "exists") return present ? BehaviorStatus::Success : BehaviorStatus::Failure;
        if (node.op == "not_exists") return present ? BehaviorStatus::Failure : BehaviorStatus::Success;
        if (!present) return BehaviorStatus::Failure;

        BlackboardValue stored;
        blackboard.get(node.key, stored);
        // Compare by type: mismatched types fail (a bool != a number).
        if (stored.kind != node.value.kind) return BehaviorStatus::Failure;

        bool result = false;
        switch (node.op[0]) {
            case 'e':  // eq
                result = compare_equal(stored, node.value);
                break;
            case 'n':  // ne
                result = !compare_equal(stored, node.value);
                break;
            case 'l':  // lt / lte
                result = (node.op == "lt") ? compare_less(stored, node.value)
                                           : !compare_less(node.value, stored);
                break;
            case 'g':  // gt / gte
                result = (node.op == "gt") ? compare_less(node.value, stored)
                                           : !compare_less(stored, node.value);
                break;
            default:
                result = false;
        }
        return result ? BehaviorStatus::Success : BehaviorStatus::Failure;
    }

    static bool compare_equal(const BlackboardValue& a, const BlackboardValue& b) {
        switch (a.kind) {
            case BlackboardKind::Bool: return a.boolean == b.boolean;
            case BlackboardKind::Number: return a.number == b.number;
            case BlackboardKind::String: return a.text == b.text;
            case BlackboardKind::None: return true;
        }
        return false;
    }

    static bool compare_less(const BlackboardValue& a, const BlackboardValue& b) {
        switch (a.kind) {
            case BlackboardKind::Number: return a.number < b.number;
            case BlackboardKind::String: return a.text < b.text;
            case BlackboardKind::Bool:
            case BlackboardKind::None:
                return false;
        }
        return false;
    }

    BehaviorStatus tick_action(const BehaviorNode& node, Blackboard& blackboard) {
        switch (node.value.kind) {
            case BlackboardKind::Bool: blackboard.set(node.key, node.value.boolean); break;
            case BlackboardKind::Number: blackboard.set(node.key, node.value.number); break;
            case BlackboardKind::String: blackboard.set(node.key, node.value.text); break;
            case BlackboardKind::None: break;
        }
        return BehaviorStatus::Success;
    }

    BehaviorStatus tick_wait(RuntimeNode& rn, double dt) {
        const BehaviorNode& node = *rn.node;
        rn.elapsed += dt;
        if (rn.elapsed >= node.waitSeconds) {
            rn.elapsed = 0.0;
            return BehaviorStatus::Success;
        }
        return BehaviorStatus::Running;
    }

    // Resets the runtime overlay of a subtree (used after a repeat iteration).
    void reset_child(const std::string& id) {
        // Recursively reset the overlay for this subtree.
        for (auto& [nodeId, rn] : runtime_) {
            if (nodeId == id ||
                (nodeId.size() > id.size() && nodeId.compare(0, id.size(), id) == 0 &&
                 nodeId[id.size()] == '.')) {
                rn.runningChild = 0;
                rn.hasRunningChild = false;
                rn.elapsed = 0.0;
                rn.repeatCount = 0;
                rn.started = false;
            }
        }
    }

    BehaviorTreeSpec spec_;
    std::map<std::string, RuntimeNode> runtime_;
    std::vector<std::pair<std::string, BehaviorStatus>> trace_;
};

}  // namespace

std::unique_ptr<IBehaviorTree> create_behavior_tree(
    const BehaviorTreeSpec& spec, std::string& errorOut) {
    errorOut.clear();
    if (!spec.validate(errorOut)) return nullptr;
    return std::make_unique<BehaviorTreeRuntime>(spec);
}

}  // namespace ai
}  // namespace engine
