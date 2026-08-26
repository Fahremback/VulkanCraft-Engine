#pragma once

// ITimelineGraph — FALTANTES differential "TimelineGraph": branches
// copy-on-write and causal history (the persistent-multiverse layer over the
// engine's time travel; META section 32 — nothing in external/solutions
// resolves it, so it is engine-own code).
//
// The graph is a FOREST of nodes: every node has at most one parent (the
// state it descends from) and any number of children (branches/versions
// forked or written from it). A node's causal history is its ancestor chain.
//
// Copy-on-write: `fork` and `write` NEVER mutate an existing node — they
// append a new child. A fork carries no own payload (its effective state is
// the nearest ancestor's payload, resolved lazily); only a `write` stores a
// payload. Siblings never observe each other's writes.
//
// Pure + deterministic: the same sequence of operations on a fresh instance
// produces an identical graph (ids, structure, effective payloads). No RNG,
// no hidden state. All refusals are all-or-nothing with a diagnostic.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine::timeline {

// Opaque, stable node ids. 0 is never a valid node id (reserved "none").
using TimelineNodeId = std::uint64_t;

struct TimelineNodeInfo {
    TimelineNodeId id{ 0 };
    TimelineNodeId parent{ 0 };     // 0 = root (no parent)
    std::uint64_t revision{ 0 };    // depth along the causal chain (root = 1)
    bool hasOwnPayload{ false };    // this node stored a payload via `write`
    std::vector<std::byte> payload; // own payload (empty unless hasOwnPayload)
    std::size_t childCount{ 0 };
};

class ITimelineGraph {
public:
    virtual ~ITimelineGraph() = default;

    // Creates the root node carrying the initial payload. Returns 0 when a
    // root already exists (a timeline has exactly one root).
    virtual TimelineNodeId create_root(const std::vector<std::byte>& payload,
                                       std::string& errorOut) = 0;

    // Forks `parent`: a NEW branch node whose effective state is the parent's
    // state AT FORK (copy-on-write — no payload copied; resolved lazily via
    // effective_payload). The parent and its other children are untouched.
    virtual TimelineNodeId fork(TimelineNodeId parent, std::string& errorOut) = 0;

    // Records a new version as a CHILD of `parent` carrying `payload`
    // (copy-on-write: the parent and its other children are immutable — the
    // write never mutates them).
    virtual TimelineNodeId write(TimelineNodeId parent,
                                 const std::vector<std::byte>& payload,
                                 std::string& errorOut) = 0;

    // ---- queries ----
    virtual bool node_info(TimelineNodeId id, TimelineNodeInfo& out) const = 0;

    // Children in deterministic (creation) order.
    virtual std::vector<TimelineNodeId> children(TimelineNodeId id) const = 0;

    // The effective state of `id`: the payload of the nearest ancestor (or
    // itself) that has one, along the causal chain. Empty when no ancestor
    // stored a payload (a fork of an unwritten root).
    virtual bool effective_payload(TimelineNodeId id,
                                   std::vector<std::byte>& outPayload) const = 0;

    // True when `ancestor` is on `node`'s causal chain (strict: a node is not
    // its own ancestor).
    virtual bool is_ancestor(TimelineNodeId ancestor, TimelineNodeId node) const = 0;

    // The FULL causal history of `id`, root first (includes `id` itself).
    virtual std::vector<TimelineNodeId> causal_ancestors(TimelineNodeId id) const = 0;

    // The first shared ancestor of two nodes — the merge point of their
    // branches. 0 when they have no common ancestor (never happens in a
    // single-root timeline; kept for the contract).
    virtual TimelineNodeId common_ancestor(TimelineNodeId a, TimelineNodeId b) const = 0;

    virtual std::size_t node_count() const = 0;
    virtual void clear() = 0;
};

// SDK adapter (single TU, src/engine/sdk/TimelineGraph.cpp). Pure +
// deterministic; refuses unknown ids all-or-nothing.
std::unique_ptr<ITimelineGraph> create_timeline_graph();

}  // namespace engine::timeline
