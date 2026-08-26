#pragma once

// IBehaviorTree (agente 4 §3 "IA reutilizável"): the PUBLIC reusable-AI
// contract. The engine has NO behavior-tree/blackboard/perception surface
// today — mob AI is hardcoded wander/flee/chase inside IMobBehavior. This
// contract turns reusable AI into DATA + a deterministic decision runtime:
//   - BLACKBOARD: a typed key/value store (bool/number/string) shared by
//     every node — the memory/perception surface a behavior tree reads and
//     writes. Caller-owned and explicit (the IAnimationLod pattern): the
//     caller creates the Blackboard, fills it from sensors, ticks the tree,
//     then reads the resulting keys to drive animation/steering/movement.
//   - BEHAVIOR TREE (data-driven, JSON versioned, all-or-nothing): nodes are
//     composites (sequence/selector), decorators (inverter/succeeder/failer/
//     repeat/service/cooldown), and leaves (condition/action/wait). The same
//     document authoring shape as SimulationLodSpec/AbilityDefinition:
//     versioned JSON, bit-exact round-trip, every invalid node refused with a
//     diagnostic (never silently clamped).
//   - REACTIVE RE-EVALUATION (abort): a sequence may declare "abort":"self" —
//     every tick it re-ticks its children from the FIRST, so a condition that
//     flips while a later child is still running aborts that child and fails
//     the sequence immediately. The default "abort":"none" keeps the last
//     running child until it completes (classic memorized sequence).
//   - SERVICE: a decorator that runs its child at most once per `interval`
//     seconds and never terminates on its own (always Running) — the periodic
//     perception/blackboard-refresh node.
//   - DETERMINISM: the tree is PURE. Time only enters through the dt passed to
//     tick(); there is no RNG and no wall clock. Same spec + same (dt,
//     blackboard) sequence -> identical status stream and identical
//     blackboard, bit-exact, across instances.
//   - DEBUGGING: debug_trace() returns the ordered {nodeId, status} visits of
//     the LAST tick (node ids are deterministic traversal paths "0", "0.0",
//     "1", ...), so a headless test or an editor can assert/visualize the
//     decision path.
//
// Self-contained (std only). Deterministic. Headless. validate /
// load_from_json / to_json of the spec and the Blackboard's own JSON
// round-trip are implemented by the SDK adapter
// (src/engine/sdk/BehaviorTree.cpp) — the ONLY TU with behavior.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace ai {

// A blackboard value: exactly one of bool, number (double) or string.
// Kind::None is the "absent" sentinel (never stored — has() distinguishes
// absent from a stored value).
enum class BlackboardKind : std::uint8_t { None, Bool, Number, String };

struct BlackboardValue {
    BlackboardKind kind{ BlackboardKind::None };
    bool boolean{ false };
    double number{ 0.0 };
    std::string text;
};

// Typed key/value store shared by behavior-tree nodes. Keys are arbitrary
// strings; iteration order is deterministic (sorted by key). The caller owns
// the instance and fills it from sensors before tick() and reads it after.
class Blackboard {
public:
    Blackboard() = default;

    void set(const std::string& key, bool value);
    void set(const std::string& key, double value);
    void set(const std::string& key, const std::string& value);
    // String-literal overload: binds `set("k", "v")` to the string setter
    // (a `const char*` argument would otherwise bind to the bool overload via
    // pointer->bool, silently storing a bool).
    void set(const std::string& key, const char* value);

    // Reads the stored value (false when the key is absent — never writes out).
    bool get(const std::string& key, BlackboardValue& out) const;
    bool has(const std::string& key) const;
    // Removes a key (true when it existed; a missing key is a no-op).
    bool erase(const std::string& key);
    void clear();
    std::size_t size() const;
    // All keys in deterministic (sorted) order.
    std::vector<std::string> keys() const;

    // JSON round-trip of the whole store: {"version":1,"entries":{...}}.
    // bit-exact on re-emit; malformed/unknown-type input refused all-or-nothing.
    std::string to_json() const;
    bool load_from_json(const std::string& jsonText, std::string& errorOut);

private:
    std::map<std::string, BlackboardValue> entries_;
};

// Tick result of a node (and of the whole tree).
enum class BehaviorStatus : std::uint8_t { Success, Failure, Running };

// One data-driven behavior-tree node. Children are owned INLINE (a tree is a
// value type, no heap per node — the adapter compiles it into a compact
// runtime once). Exactly which fields matter depends on `type`:
//   sequence/selector -> children (+ optional abort)
//   parallel          -> children + policy ("all" AND / "any" OR)
//   inverter/succeeder/failer -> child (single; stored in children[0])
//   repeat            -> child + times
//   service           -> child + intervalSeconds
//   cooldown          -> child + cooldownSeconds
//   condition         -> key + op + value
//   action            -> key + value
//   wait              -> waitSeconds
struct BehaviorNode {
    std::string type;
    std::vector<BehaviorNode> children;

    // parallel policy.
    std::string policy{ "all" };
    // sequence/selector abort (self/none).
    std::string abort{ "none" };
    // repeat count.
    int times{ 1 };
    // service interval / cooldown (seconds).
    double intervalSeconds{ 0.0 };
    double cooldownSeconds{ 0.0 };
    // condition op (eq/ne/lt/lte/gt/gte/exists/not_exists).
    std::string op{ "eq" };
    std::string key;
    BlackboardValue value;
    // wait seconds.
    double waitSeconds{ 0.0 };

    // Recursive all-or-nothing validation. Refuses unknown type, a composite
    // without children, a decorator/condition/action/wait with missing
    // operands, non-finite/negative timings, unknown policy/abort/op, a
    // repeat count <= 0, and an exists/not_exists condition carrying a value
    // (they only test key presence).
    bool validate(std::string& errorOut) const;
};

// The full data-driven behavior tree (versioned JSON, all-or-nothing).
struct BehaviorTreeSpec {
    int version{ 1 };
    BehaviorNode root;

    bool validate(std::string& errorOut) const;
    bool load_from_json(const std::string& jsonText, std::string& errorOut);
    std::string to_json() const;
};

// Compiled behavior-tree runtime. PURE and DETERMINISTIC: tick(dt, bb)
// advances the tree by dt seconds against the given blackboard and returns
// the root status; reset() returns it to the freshly-compiled state (a
// running node restarts). The runtime keeps NO hidden state beyond the
// traversal (which child is running, elapsed service/wait/cooldown timers) —
// all of it is reset by reset() and reconstructed deterministically from the
// spec.
class IBehaviorTree {
public:
    virtual ~IBehaviorTree() = default;

    // Advances the tree one step by dt seconds. dt must be finite and >= 0
    // (a negative dt returns Failure and leaves the blackboard untouched).
    virtual BehaviorStatus tick(double dt, Blackboard& blackboard) = 0;

    // Returns the tree to the freshly-compiled state.
    virtual void reset() = 0;

    // Ordered {nodeId, status} visits of the LAST tick (node ids are
    // deterministic traversal paths). Used for headless assertions and
    // editor/debug visualization. Empty before the first tick.
    virtual std::vector<std::pair<std::string, BehaviorStatus>> debug_trace()
        const = 0;
};

// Compiles a spec (validating it first — a rejected spec returns nullptr and
// fills errorOut). The adapter is the ONLY TU with the runtime.
std::unique_ptr<IBehaviorTree> create_behavior_tree(
    const BehaviorTreeSpec& spec, std::string& errorOut);

}  // namespace ai
}  // namespace engine
