#pragma once

// Public shape-grammar contracts (META section 18 / FALTANTES item 14).
//
// A permissive, data-driven shape grammar that generates voxel mass models
// (buildings/structures as axis-aligned block volumes). The language is an
// original design inspired by the public concepts of shape grammars / CGA
// (Stiny 1975, Muller 2006 — the academic lineage, not any GPL code): rules
// are named, one rule is the entry point ("Axiom"), and each rule body is an
// ordered list of operations that transform a scope (position + size) and
// emit block volumes.
//
// Semantics (deterministic, pure — no randomness):
//   - A scope is a position + size in integer voxel coordinates. The runner
//     starts at the Axiom rule with scope (0,0,0) size (1,1,1).
//   - `size(w,h,d)` replaces the current scope size (position unchanged).
//   - `extrude(h)` sets the scope height.
//   - `mass(blockId)` emits one box covering the current scope (0 = Air is
//     allowed, e.g. to carve).
//   - `split(axis, sizes, rules)` lays `sizes` consecutive parts along
//     `axis` starting at the scope minimum, clipped to the scope extent
//     (parts beyond the scope are clamped to its max; space left over is
//     unused), and runs the named rule on each part's child scope. The
//     current scope is unchanged afterward.
//   - `call(rule)` runs the named rule on the current scope; the scope is
//     unchanged afterward.
//   - Rules may reference rules defined anywhere in the grammar (including
//     themselves); a recursion depth limit stops runaway grammars.
//
// A grammar is a pure function: two runs (or two runner instances) with the
// same grammar produce bit-identical box lists. Grammars are versioned JSON
// assets with all-or-nothing deserialization.

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace engine {
namespace procgen {

// Operation payloads -------------------------------------------------------

struct GrammarOpSize {
    int width{ 1 };
    int height{ 1 };
    int depth{ 1 };
};

struct GrammarOpExtrude {
    int height{ 1 };
};

struct GrammarOpMass {
    std::uint32_t blockId{ 0 };
};

// Splits the current scope along `axis` ('x', 'y' or 'z') into consecutive
// parts with the given absolute sizes, each part processed by the named rule
// (sizes.size() == rules.size()).
struct GrammarOpSplit {
    char axis{ 'x' };
    std::vector<int> sizes;
    std::vector<std::string> rules;
};

struct GrammarOpCall {
    std::string rule;
};

// A single operation of a rule body.
using GrammarOp = std::variant<GrammarOpSize, GrammarOpExtrude, GrammarOpMass,
                               GrammarOpSplit, GrammarOpCall>;

// A named rule: an ordered list of operations.
struct GrammarRule {
    std::string name;
    std::vector<GrammarOp> ops;
};

// A shape grammar asset. The entry rule is the one named "Axiom".
struct ShapeGrammar {
    std::vector<GrammarRule> rules;
};

// One emitted block volume (half-open integer bounds).
struct GrammarBox {
    int minX{ 0 };
    int minY{ 0 };
    int minZ{ 0 };
    int maxX{ 0 };
    int maxY{ 0 };
    int maxZ{ 0 };
    std::uint32_t blockId{ 0 };
};

// The output of running a grammar: the emitted block volumes.
struct GrammarResult {
    std::vector<GrammarBox> boxes;
};

// Runs shape grammars and (de)serializes them as versioned JSON assets.
class IShapeGrammarRunner {
public:
    virtual ~IShapeGrammarRunner() = default;

    // Runs the grammar from its Axiom rule and fills `out` with the emitted
    // boxes. Deterministic. Returns false with a message on invalid grammar
    // (missing Axiom, unknown rule reference, split/call recursion beyond
    // the depth limit).
    virtual bool run(const ShapeGrammar& grammar, GrammarResult& out,
                     std::string& errorOut) = 0;

    // Validates a grammar without running it (structure + references).
    virtual bool validate(const ShapeGrammar& grammar,
                          std::string& errorOut) const = 0;

    virtual bool serialize(const ShapeGrammar& grammar,
                           std::string& out) const = 0;
    virtual bool deserialize(const std::string& json, ShapeGrammar& out,
                             std::string& errorOut) const = 0;
};

// Factory (implemented by the SDK adapter — self-contained, no backend).
std::shared_ptr<IShapeGrammarRunner> create_shape_grammar_runner();

}  // namespace procgen
}  // namespace engine
