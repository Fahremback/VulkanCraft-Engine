#pragma once

// ICausalResolver — FALTANTES differential "CausalResolver": invalidation and
// recomputation of ONLY the affected descendants (META section 32 — engine-own
// code; nothing in external/solutions resolves temporal causality).
//
// Computations form a DAG: DERIVED nodes consume the outputs of other nodes
// (leaves or derived); LEAF nodes hold values the caller owns. When a leaf
// changes, the dirty set is EXACTLY that leaf plus its transitive consumers
// (the affected descendants) — unrelated subtrees keep their cached values
// and their recompute counts.
//
// Pure + deterministic: resolve() recomputes dirty derived nodes in a
// deterministic topological order (registration id as tie-break), so the same
// operation sequence reproduces identical values and recompute counts. No RNG,
// no hidden state. All refusals are all-or-nothing with a diagnostic.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine::timeline {

// Opaque, stable node ids. 0 is never a valid node id (reserved "none").
using CausalNodeId = std::uint64_t;

// Pure compute function for a derived node: maps the input values (in the
// declared input order) to the node's output. Must be deterministic.
using CausalComputeFn = std::int64_t (*)(const std::vector<std::int64_t>&);

struct CausalNodeState {
    CausalNodeId id{ 0 };
    bool isLeaf{ false };
    std::string name;
    std::vector<CausalNodeId> inputs;     // dependencies (leaf: empty)
    std::vector<CausalNodeId> consumers;  // reverse edges (who reads me)
    std::int64_t value{ 0 };
    std::uint64_t recomputeCount{ 0 };    // how many resolves recomputed this node
    bool dirty{ false };
};

class ICausalResolver {
public:
    virtual ~ICausalResolver() = default;

    // Adds a leaf whose value the caller owns. Setting it marks it (and its
    // transitive consumers) dirty.
    virtual CausalNodeId add_leaf(const std::string& name, std::int64_t initialValue,
                                  std::string& errorOut) = 0;

    // Adds a derived node: output = fn(input values). Inputs must already
    // exist; cycles are refused all-or-nothing (the graph stays untouched).
    virtual CausalNodeId add_derived(const std::string& name,
                                     const std::vector<CausalNodeId>& inputs,
                                     CausalComputeFn fn,
                                     std::string& errorOut) = 0;

    // Changes a leaf's value and marks exactly its affected descendants dirty.
    // Refused for unknown ids or derived nodes (the caller owns leaves only).
    virtual bool set_leaf_value(CausalNodeId id, std::int64_t value,
                                std::string& errorOut) = 0;

    // The transitive consumers of `changedLeaf` (the invalidation set).
    virtual std::vector<CausalNodeId> affected_descendants(CausalNodeId changedLeaf) const = 0;

    // Recomputes the dirty derived nodes in deterministic topological order.
    // Returns how many nodes were recomputed. Leaves are never recomputed
    // (their value is caller-owned); a dirty leaf only propagates dirtiness.
    virtual std::size_t resolve() = 0;

    virtual bool state(CausalNodeId id, CausalNodeState& out) const = 0;
    virtual std::size_t node_count() const = 0;
    virtual void clear() = 0;
};

// SDK adapter (single TU, src/engine/sdk/CausalResolver.cpp). Pure +
// deterministic; refuses unknown ids and cycles all-or-nothing.
std::unique_ptr<ICausalResolver> create_causal_resolver();

}  // namespace engine::timeline
