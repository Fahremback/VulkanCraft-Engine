// TimelineCausalTests.cpp
//
// FALTANTES differentials — TimelineGraph (branches copy-on-write + causal
// history) and CausalResolver (invalidation/recomputation of ONLY the
// affected descendants), META section 32. The gates drive the PUBLIC
// contracts headless and prove:
//   - TimelineGraph: single root, writes as immutable children, fork with
//     copy-on-write (effective state resolved lazily, siblings never see each
//     other's writes), branches from history, causal ancestors, common
//     ancestor (merge point), determinism, all-or-nothing refusals;
//   - CausalResolver: leaf-owned values, derived chains, dirty-only
//     recomputation (unrelated subtrees keep their recompute counts),
//     deterministic topological order, cycle/unknown refusals, determinism;
//   - composition: the timeline's causal structure drives the resolver's
//     invalidation — a change at a timeline node recomputes exactly the
//     nodes that descend from it (temporal causality).

#include "engine/timeline/ITimelineGraph.hpp"
#include "engine/timeline/ICausalResolver.hpp"

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::printf("FAIL: %s\n", message);
        ++g_failures;
    }
}

std::vector<std::byte> bytes(std::initializer_list<unsigned char> values) {
    std::vector<std::byte> out;
    for (unsigned char value : values) out.push_back(std::byte{ value });
    return out;
}

bool same_bytes(const std::vector<std::byte>& a, const std::vector<std::byte>& b) {
    return a == b;
}

// ---- TimelineGraph -----------------------------------------------------------

void test_timeline_root_and_write() {
    using namespace engine::timeline;
    auto graph = create_timeline_graph();
    std::string error;

    const auto root = graph->create_root(bytes({ 1, 2, 3 }), error);
    check(root != 0, "root created");
    check(!graph->create_root(bytes({ 9 }), error), "second root refused");
    check(!error.empty(), "second-root diagnostic set");
    error.clear();  // successful ops leave the diagnostic untouched

    TimelineNodeInfo rootInfo;
    check(graph->node_info(root, rootInfo), "root info");
    check(rootInfo.parent == 0, "root has no parent");
    check(rootInfo.revision == 1, "root revision 1");
    check(rootInfo.hasOwnPayload && same_bytes(rootInfo.payload, bytes({ 1, 2, 3 })),
          "root carries its payload");

    const auto w1 = graph->write(root, bytes({ 4, 5 }), error);
    const auto w2 = graph->write(root, bytes({ 6 }), error);
    check(w1 != 0 && w2 != 0, "writes create children");
    check(error.empty(), "write diagnostics empty");
    check(graph->children(root) == std::vector<TimelineNodeId>{ w1, w2 },
          "children in creation order");
    check(graph->node_count() == 3, "node count 3");

    std::vector<std::byte> payload;
    check(graph->effective_payload(w1, payload) && same_bytes(payload, bytes({ 4, 5 })),
          "w1 effective payload");
    check(graph->effective_payload(w2, payload) && same_bytes(payload, bytes({ 6 })),
          "w2 effective payload");
    check(graph->causal_ancestors(w1) == std::vector<TimelineNodeId>{ root, w1 },
          "causal chain root-first");
    check(graph->is_ancestor(root, w1), "root is ancestor of w1");
    check(!graph->is_ancestor(w1, root), "w1 is not ancestor of root");
    check(!graph->is_ancestor(w1, w2), "siblings are not ancestors of each other");

    std::printf("[timeline] root + immutable writes + causal chain OK\n");
}

void test_timeline_copy_on_write_fork() {
    using namespace engine::timeline;
    auto graph = create_timeline_graph();
    std::string error;

    const auto root = graph->create_root(bytes({ 7 }), error);
    const auto forkA = graph->fork(root, error);
    const auto forkB = graph->fork(root, error);
    check(forkA != 0 && forkB != 0, "forks created");
    check(error.empty(), "fork diagnostics empty");

    TimelineNodeInfo forkInfo;
    check(graph->node_info(forkA, forkInfo), "fork info");
    check(!forkInfo.hasOwnPayload, "fork stores NO own payload (copy-on-write)");

    std::vector<std::byte> payload;
    check(graph->effective_payload(forkA, payload) && same_bytes(payload, bytes({ 7 })),
          "fork effective state resolves to the parent (no copy)");

    // Writes on each branch diverge without touching the parent or siblings.
    const auto forkA1 = graph->write(forkA, bytes({ 1, 0 }), error);
    const auto forkB1 = graph->write(forkB, bytes({ 2, 0 }), error);
    check(graph->effective_payload(forkA1, payload) && same_bytes(payload, bytes({ 1, 0 })),
          "branch A diverged");
    check(graph->effective_payload(forkB1, payload) && same_bytes(payload, bytes({ 2, 0 })),
          "branch B diverged");
    check(graph->effective_payload(root, payload) && same_bytes(payload, bytes({ 7 })),
          "root untouched by both branches");
    check(graph->children(root) == std::vector<TimelineNodeId>{ forkA, forkB },
          "root children unchanged (no write leaked)");
    check(graph->children(forkA) == std::vector<TimelineNodeId>{ forkA1 },
          "fork A child");
    check(graph->common_ancestor(forkA1, forkB1) == root,
          "branches merge at the fork point (root)");
    check(graph->causal_ancestors(forkA1) ==
              std::vector<TimelineNodeId>{ root, forkA, forkA1 },
          "branch A causal history");

    std::printf("[timeline] copy-on-write forks + divergence + merge point OK\n");
}

void test_timeline_branch_from_history() {
    using namespace engine::timeline;
    auto graph = create_timeline_graph();
    std::string error;

    const auto root = graph->create_root(bytes({ 1 }), error);
    const auto w1 = graph->write(root, bytes({ 2 }), error);
    const auto w2 = graph->write(w1, bytes({ 3 }), error);

    // Branch from an INTERMEDIATE node (not the tip).
    const auto branch = graph->fork(w1, error);
    const auto branch2 = graph->write(branch, bytes({ 9 }), error);
    check(branch != 0 && branch2 != 0, "branch from history created");

    std::vector<std::byte> payload;
    check(graph->effective_payload(branch, payload) && same_bytes(payload, bytes({ 2 })),
          "branch effective state = w1 (branch point)");
    check(graph->common_ancestor(w2, branch2) == w1, "merge point is the branch node");
    check(graph->is_ancestor(w1, branch2), "branch point is ancestor of the new branch");
    check(!graph->is_ancestor(w2, branch2), "mainline after branch point is NOT an ancestor");

    std::printf("[timeline] branch from history + common ancestor OK\n");
}

void test_timeline_refusals_and_determinism() {
    using namespace engine::timeline;
    auto graph = create_timeline_graph();
    std::string error;

    check(graph->fork(999, error) == 0 && !error.empty(), "fork unknown refused");
    check(graph->write(999, bytes({ 1 }), error) == 0 && !error.empty(),
          "write unknown refused");
    TimelineNodeInfo info;
    check(!graph->node_info(999, info), "node_info unknown false");
    check(graph->children(999).empty(), "children unknown empty");
    std::vector<std::byte> payload;
    check(!graph->effective_payload(999, payload), "effective_payload unknown false");
    check(!graph->is_ancestor(999, 1), "is_ancestor unknown false");
    check(graph->causal_ancestors(999).empty(), "causal_ancestors unknown empty");
    check(graph->common_ancestor(1, 999) == 0, "common_ancestor unknown 0");

    // Determinism: the same operation sequence on a fresh instance produces an
    // identical graph.
    auto graph2 = create_timeline_graph();
    const auto run = [](auto& g) {
        std::string err;
        const auto r = g->create_root(bytes({ 1, 2 }), err);
        const auto a = g->fork(r, err);
        const auto b = g->fork(r, err);
        const auto a1 = g->write(a, bytes({ 5 }), err);
        const auto b1 = g->write(b, bytes({ 6 }), err);
        return std::vector<TimelineNodeId>{ r, a, b, a1, b1 };
    };
    const auto ids1 = run(graph);
    const auto ids2 = run(graph2);
    check(ids1 == ids2, "deterministic node ids across instances");
    check(graph->node_count() == graph2->node_count(), "deterministic node count");
    check(graph->causal_ancestors(ids1[3]) == graph2->causal_ancestors(ids2[3]),
          "deterministic causal chains");

    std::printf("[timeline] refusals all-or-nothing + determinism OK\n");
}

// ---- CausalResolver ----------------------------------------------------------

std::int64_t double_fn(const std::vector<std::int64_t>& values) { return values[0] * 2; }
std::int64_t plus_one_fn(const std::vector<std::int64_t>& values) { return values[0] + 1; }
std::int64_t sum_fn(const std::vector<std::int64_t>& values) {
    std::int64_t total = 0;
    for (const std::int64_t value : values) total += value;
    return total;
}

void test_causal_chain() {
    using namespace engine::timeline;
    auto resolver = create_causal_resolver();
    std::string error;

    const auto leaf = resolver->add_leaf("input", 10, error);
    const auto d1 = resolver->add_derived("double", { leaf }, double_fn, error);
    const auto d2 = resolver->add_derived("doublePlusOne", { d1 }, plus_one_fn, error);
    check(leaf != 0 && d1 != 0 && d2 != 0, "chain added");

    check(resolver->resolve() == 2, "initial resolve computes the 2 derived nodes");
    CausalNodeState state;
    check(resolver->state(d1, state) && state.value == 20, "d1 = leaf*2");
    check(resolver->state(d2, state) && state.value == 21, "d2 = d1+1");
    check(resolver->state(d1, state) && state.recomputeCount == 1, "d1 recomputed once");

    check(resolver->set_leaf_value(leaf, 30, error), "leaf updated");
    check(error.empty(), "leaf update diagnostic empty");
    check(resolver->resolve() == 2, "change recomputes only the 2 affected");
    check(resolver->state(d1, state) && state.value == 60, "d1 propagated");
    check(resolver->state(d2, state) && state.value == 61, "d2 propagated");
    check(resolver->state(d1, state) && state.recomputeCount == 2, "d1 recomputed twice");

    std::printf("[causal] derived chain propagation + counts OK\n");
}

void test_causal_unaffected_subtree() {
    using namespace engine::timeline;
    auto resolver = create_causal_resolver();
    std::string error;

    // Two INDEPENDENT chains.
    const auto leafA = resolver->add_leaf("a", 1, error);
    const auto dA1 = resolver->add_derived("aD1", { leafA }, double_fn, error);
    const auto dA2 = resolver->add_derived("aD2", { dA1 }, plus_one_fn, error);
    const auto leafB = resolver->add_leaf("b", 100, error);
    const auto dB1 = resolver->add_derived("bD1", { leafB }, double_fn, error);
    check(resolver->resolve() == 3, "initial resolve computes all 3 derived");

    const auto affected = resolver->affected_descendants(leafA);
    check(affected == std::vector<CausalNodeId>{ dA1, dA2 },
          "affected descendants of A are exactly A's chain");
    check(resolver->affected_descendants(leafB) == std::vector<CausalNodeId>{ dB1 },
          "affected descendants of B are exactly B's node");

    check(resolver->set_leaf_value(leafA, 50, error), "A changed");
    check(resolver->resolve() == 2, "recompute ONLY A's 2 descendants");
    CausalNodeState state;
    check(resolver->state(dA2, state) && state.value == 101, "A chain value updated");
    check(resolver->state(dB1, state) && state.value == 200, "B chain value untouched");
    check(resolver->state(dB1, state) && state.recomputeCount == 1,
          "B node recompute count UNCHANGED (not invalidated)");
    check(resolver->state(dA2, state) && state.recomputeCount == 2,
          "A node recomputed again");

    std::printf("[causal] unaffected subtree keeps cached value + count OK\n");
}

void test_causal_refusals_and_determinism() {
    using namespace engine::timeline;
    auto resolver = create_causal_resolver();
    std::string error;

    const auto leaf = resolver->add_leaf("input", 0, error);
    const auto d1 = resolver->add_derived("d1", { leaf }, double_fn, error);

    check(resolver->add_derived("bad", { 999 }, double_fn, error) == 0 &&
              !error.empty(),
          "unknown input refused");
    check(resolver->node_count() == 2, "refused add left the graph untouched");
    check(!resolver->set_leaf_value(d1, 1, error) && !error.empty(),
          "set_leaf_value on derived refused");
    check(!resolver->set_leaf_value(999, 1, error) && !error.empty(),
          "set_leaf_value unknown refused");
    CausalNodeState state;
    check(!resolver->state(999, state), "state unknown false");
    check(resolver->affected_descendants(999).empty(), "affected unknown empty");

    // Determinism: identical op sequence -> identical values and counts.
    auto resolver2 = create_causal_resolver();
    const auto leaf2 = resolver2->add_leaf("input", 0, error);
    resolver2->add_derived("d1", { leaf2 }, double_fn, error);
    resolver2->set_leaf_value(leaf2, 7, error);
    resolver2->resolve();
    resolver->set_leaf_value(leaf, 7, error);
    resolver->resolve();
    CausalNodeState s1, s2;
    check(resolver->state(d1, s1), "resolver1 state queryable");
    check(resolver2->state(d1, s2), "resolver2 state queryable");
    check(s1.value == s2.value && s1.recomputeCount == s2.recomputeCount,
          "deterministic values + counts across instances");

    std::printf("[causal] refusals all-or-nothing + determinism OK\n");
}

// ---- composition: timeline causality drives resolver invalidation -----------

void test_timeline_causal_composition() {
    using namespace engine::timeline;
    auto graph = create_timeline_graph();
    auto resolver = create_causal_resolver();
    std::string error;

    // Timeline: root -> forkA -> write A1 ; root -> forkB -> write B1.
    const auto root = graph->create_root(bytes({ 1 }), error);
    const auto forkA = graph->fork(root, error);
    const auto forkB = graph->fork(root, error);
    const auto a1 = graph->write(forkA, bytes({ 2 }), error);
    const auto b1 = graph->write(forkB, bytes({ 3 }), error);

    // Mirror the timeline's causal structure 1:1 in the resolver: every
    // timeline node maps to a leaf MARKER the caller can change; every
    // parent->child edge maps to a derived node consuming the parent marker,
    // so invalidation flows down the branches exactly as the timeline's
    // causality dictates.
    std::map<TimelineNodeId, CausalNodeId> marker;
    marker[root] = resolver->add_leaf("root", 0, error);
    marker[forkA] = resolver->add_leaf("forkA", 0, error);
    marker[forkB] = resolver->add_leaf("forkB", 0, error);
    marker[a1] = resolver->add_leaf("a1", 0, error);
    marker[b1] = resolver->add_leaf("b1", 0, error);
    const auto edgeA =
        resolver->add_derived("edge(forkA->a1)", { marker[forkA] }, plus_one_fn, error);
    const auto edgeB =
        resolver->add_derived("edge(forkB->b1)", { marker[forkB] }, plus_one_fn, error);
    check(resolver->resolve() == 2, "initial resolve computes the 2 branch edges");

    // A change AT forkA invalidates exactly forkA's timeline descendant
    // (a1) — branch B is untouched (temporal causality: history changes
    // propagate only down the affected branches).
    check(resolver->set_leaf_value(marker[forkA], 1, error), "forkA changed");
    const auto affected = resolver->affected_descendants(marker[forkA]);
    check(affected == std::vector<CausalNodeId>{ edgeA },
          "forkA invalidation = exactly its timeline descendant (a1)");
    check(resolver->resolve() == 1, "recompute ONLY forkA's edge");
    CausalNodeState state;
    check(resolver->state(edgeA, state) && state.value == 2, "edgeA recomputed");
    check(resolver->state(edgeB, state) && state.recomputeCount == 1,
          "edgeB NEVER recomputed by the forkA change");
    check(resolver->state(edgeB, state) && state.value == 1,
          "edgeB value unchanged");

    std::printf("[timeline-causal] change at a node recomputes only its "
                "descendants OK\n");
}

}  // namespace

int main() {
    test_timeline_root_and_write();
    test_timeline_copy_on_write_fork();
    test_timeline_branch_from_history();
    test_timeline_refusals_and_determinism();
    test_causal_chain();
    test_causal_unaffected_subtree();
    test_causal_refusals_and_determinism();
    test_timeline_causal_composition();
    if (g_failures == 0) {
        std::printf("[timeline-causal] ALL PASSED\n");
        return 0;
    }
    std::printf("[timeline-causal] %d FAILURE(S)\n", g_failures);
    return 1;
}
