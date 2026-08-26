// RenderGraphCoreTests.cpp — Agente 1 (task_plan B2): headless gate for the
// PUBLIC Render Graph contract (IRenderGraph). Proves the DAG core compiles
// deterministically (topological order, hazard edges, barriers, lifetimes),
// rejects cycles/missing resources/duplicate access/undefined state, and —
// crucially — is BIT-EXACT equivalent to the proven internal RenderGraph
// implementation (compiled via RenderGraphInternalProbe.cpp). No GPU.

#include "engine/rendering/IRenderGraph.hpp"
#include "RenderGraphInternalProbe.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::printf("FAIL: %s\n", message.c_str());
        ++g_failures;
    }
}

using Engine::Rendering::IRenderGraph;
using Engine::Rendering::RenderAccess;
using Engine::Rendering::RenderGraphCompileResult;
using Engine::Rendering::RenderPassId;
using Engine::Rendering::RenderQueue;
using Engine::Rendering::RenderResourceDesc;
using Engine::Rendering::RenderResourceId;
using Engine::Rendering::RenderResourceKind;
using Engine::Rendering::RenderResourceState;
using Engine::Rendering::create_render_graph;

// ---- canonical graph shared with the internal probe ----
struct CanonicalIds {
    RenderResourceId r1;
    RenderResourceId r2;
    RenderPassId p1;
    RenderPassId p2;
    RenderPassId p3;
};

CanonicalIds build_canonical(IRenderGraph& graph) {
    const RenderResourceId r1 = graph.add_resource({
        "r1", RenderResourceKind::Image, 0, 1, 1, 1, true, false,
        RenderResourceState::Undefined,
    });
    const RenderResourceId r2 = graph.add_resource({
        "r2", RenderResourceKind::Image, 0, 1, 1, 1, true, false,
        RenderResourceState::Undefined,
    });
    const RenderPassId p1 = graph.add_pass({
        "shadow", RenderQueue::Graphics,
        {
            {r1, RenderAccess::Read, RenderResourceState::ShaderRead},
            {r2, RenderAccess::Write, RenderResourceState::ColorAttachment},
        },
        true,
    });
    const RenderPassId p2 = graph.add_pass({
        "gbuffer", RenderQueue::Graphics,
        {
            {r2, RenderAccess::Read, RenderResourceState::ShaderRead},
            {r1, RenderAccess::Write, RenderResourceState::ColorAttachment},
        },
        true,
    });
    const RenderPassId p3 = graph.add_pass({
        "composite", RenderQueue::Graphics,
        {
            {r1, RenderAccess::Read, RenderResourceState::ShaderRead},
        },
        true,
    });
    graph.add_dependency(p2, p3);
    return {r1, r2, p1, p2, p3};
}

}  // namespace

int main() {
    using namespace Engine::Rendering;
    using Engine::Rendering::InternalProbe::compile_canonical_internal;

    // ---- 1. factory ----
    {
        std::string error;
        auto graph = create_render_graph(error);
        check(graph != nullptr && error.empty(), "create_render_graph succeeds");
        const auto& res = graph->resources();
        const auto& passes = graph->passes();
        check(res.empty() && passes.empty(), "fresh graph is empty");
        check(graph->resource_ids().empty() && graph->pass_ids().empty(),
              "fresh graph has no ids");
    }

    // ---- 2. canonical compile: topological order + hazards ----
    {
        std::string error;
        auto graph = create_render_graph(error);
        build_canonical(*graph);
        const RenderGraphCompileResult compiled = graph->compile();
        check(static_cast<bool>(compiled), "canonical graph compiles clean");
        check(compiled.order.size() == 3, "canonical order has 3 passes");
        if (compiled.order.size() == 3) {
            // hazards force P1 before P2; explicit dep forces P2 before P3.
            check(compiled.order[0] == 1 && compiled.order[1] == 2 && compiled.order[2] == 3,
                  "order is declaration order [1,2,3] (hazard + dep consistent)");
        }
        check(compiled.barriers.size() >= 3, "canonical graph emits >=3 barriers");
        check(compiled.lifetimes.size() == 2, "both resources have lifetimes");
    }

    // ---- 3. cycle detection (all-or-nothing: order cleared) ----
    {
        std::string error;
        auto graph = create_render_graph(error);
        const RenderResourceId r1 = graph->add_resource({
            "r", RenderResourceKind::Image, 0, 1, 1, 1, true, false,
            RenderResourceState::Undefined,
        });
        const RenderPassId a = graph->add_pass({
            "a", RenderQueue::Graphics,
            {{r1, RenderAccess::Read, RenderResourceState::ShaderRead}},
            true,
        });
        const RenderPassId b = graph->add_pass({
            "b", RenderQueue::Graphics,
            {{r1, RenderAccess::Read, RenderResourceState::ShaderRead}},
            true,
        });
        check(graph->add_dependency(a, b), "dep a->b added");
        check(graph->add_dependency(b, a), "dep b->a added");
        const RenderGraphCompileResult compiled = graph->compile();
        check(!static_cast<bool>(compiled), "cycle rejected");
        check(compiled.order.empty(), "cycle invalidates order (all-or-nothing)");
        bool foundCycle = false;
        for (const auto& e : compiled.errors)
            if (e.find("cycle") != std::string::npos) foundCycle = true;
        check(foundCycle, "cycle error names the cycle");
    }

    // ---- 4. missing resource / duplicate access / undefined state ----
    {
        std::string error;
        auto graph = create_render_graph(error);
        const RenderPassId p = graph->add_pass({
            "bad", RenderQueue::Graphics,
            {{999, RenderAccess::Read, RenderResourceState::ShaderRead}},
            true,
        });
        const RenderGraphCompileResult compiled = graph->compile();
        check(!static_cast<bool>(compiled), "missing resource rejected");
        check(compiled.order.empty(), "missing resource invalidates order");

        std::string error2;
        auto graph2 = create_render_graph(error2);
        const RenderResourceId r = graph2->add_resource({
            "r", RenderResourceKind::Image, 0, 1, 1, 1, true, false,
            RenderResourceState::Undefined,
        });
        graph2->add_pass({
            "dup", RenderQueue::Graphics,
            {{r, RenderAccess::Read, RenderResourceState::ShaderRead},
             {r, RenderAccess::Read, RenderResourceState::ShaderRead}},
            true,
        });
        const RenderGraphCompileResult compiled2 = graph2->compile();
        check(!static_cast<bool>(compiled2), "duplicate access rejected");

        std::string error3;
        auto graph3 = create_render_graph(error3);
        const RenderResourceId r3 = graph3->add_resource({
            "r", RenderResourceKind::Image, 0, 1, 1, 1, true, false,
            RenderResourceState::Undefined,
        });
        graph3->add_pass({
            "undef", RenderQueue::Graphics,
            {{r3, RenderAccess::Read, RenderResourceState::Undefined}},
            true,
        });
        const RenderGraphCompileResult compiled3 = graph3->compile();
        check(!static_cast<bool>(compiled3), "undefined state rejected");
    }

    // ---- 5. disabled pass skips dependencies + hazards ----
    {
        std::string error;
        auto graph = create_render_graph(error);
        const RenderResourceId r1 = graph->add_resource({
            "r", RenderResourceKind::Image, 0, 1, 1, 1, true, false,
            RenderResourceState::Undefined,
        });
        const RenderPassId a = graph->add_pass({
            "a", RenderQueue::Graphics,
            {{r1, RenderAccess::Write, RenderResourceState::ColorAttachment}},
            true,
        });
        const RenderPassId b = graph->add_pass({
            "b", RenderQueue::Graphics,
            {{r1, RenderAccess::Write, RenderResourceState::ColorAttachment}},
            true,
        });
        check(graph->add_dependency(a, b), "dep a->b added");
        check(graph->set_pass_enabled(b, false), "pass b disabled");
        const RenderGraphCompileResult compiled = graph->compile();
        check(static_cast<bool>(compiled), "graph with disabled pass compiles");
        check(compiled.order.size() == 1 && compiled.order[0] == a,
              "only enabled pass executes");
        // The only barrier is the initial-state transition (Undefined ->
        // ColorAttachment) on the resource's FIRST use; there must be NO
        // hazard/transition barrier between pass a and the disabled pass b.
        bool edgeToDisabled = false;
        for (const auto& barrier : compiled.barriers)
            if (barrier.sourcePass == a || barrier.destinationPass == b) edgeToDisabled = true;
        check(!edgeToDisabled, "no barrier edge to/from disabled pass");
    }

    // ---- 6. remove_pass / remove_resource ----
    {
        std::string error;
        auto graph = create_render_graph(error);
        const RenderResourceId r = graph->add_resource({
            "r", RenderResourceKind::Image, 0, 1, 1, 1, true, false,
            RenderResourceState::Undefined,
        });
        const RenderPassId p = graph->add_pass({
            "p", RenderQueue::Graphics,
            {{r, RenderAccess::Read, RenderResourceState::ShaderRead}},
            true,
        });
        check(graph->remove_pass(p), "pass removed");
        check(graph->remove_resource(r), "resource removed (no pass references it)");
        check(graph->pass_ids().empty() && graph->resource_ids().empty(),
              "graph empty after removes");
    }

    // ---- 7. BIT-EXACT equivalence with the internal RenderGraph ----
    {
        std::string error;
        auto graph = create_render_graph(error);
        build_canonical(*graph);
        const RenderGraphCompileResult mine = graph->compile();
        const auto reference = compile_canonical_internal();

        check(mine.order == reference.order,
              "order bit-exact vs internal implementation");
        check(mine.barriers.size() == reference.barriers.size(),
              "barrier count bit-exact vs internal");
        check(mine.lifetimes.size() == reference.lifetimes.size(),
              "lifetime count bit-exact vs internal");
        check(mine.errors == reference.errors,
              "errors bit-exact vs internal");

        if (mine.barriers.size() == reference.barriers.size()) {
            bool allMatch = true;
            for (std::size_t i = 0; i < mine.barriers.size(); ++i) {
                const auto& a = mine.barriers[i];
                const auto& b = reference.barriers[i];
                if (a.resource != b.resource || a.sourcePass != b.sourcePass ||
                    a.destinationPass != b.destinationPass ||
                    static_cast<std::uint8_t>(a.sourceAccess) != b.sourceAccess ||
                    static_cast<std::uint8_t>(a.destinationAccess) != b.destinationAccess ||
                    static_cast<std::uint8_t>(a.before) != b.before ||
                    static_cast<std::uint8_t>(a.after) != b.after ||
                    static_cast<std::uint8_t>(a.sourceQueue) != b.sourceQueue ||
                    static_cast<std::uint8_t>(a.destinationQueue) != b.destinationQueue ||
                    a.queueOwnershipTransfer != b.queueOwnershipTransfer) {
                    allMatch = false;
                    break;
                }
            }
            check(allMatch, "barrier fields bit-exact vs internal");
        }
        if (mine.lifetimes.size() == reference.lifetimes.size()) {
            bool allMatch = true;
            for (std::size_t i = 0; i < mine.lifetimes.size(); ++i) {
                const auto& a = mine.lifetimes[i];
                const auto& b = reference.lifetimes[i];
                if (a.resource != b.resource || a.firstUse != b.firstUse ||
                    a.lastUse != b.lastUse || a.transient != b.transient) {
                    allMatch = false;
                    break;
                }
            }
            check(allMatch, "lifetime fields bit-exact vs internal");
        }
    }

    // ---- 8. determinism: identical graphs compile identically ----
    {
        std::string error;
        auto a = create_render_graph(error);
        auto b = create_render_graph(error);
        build_canonical(*a);
        build_canonical(*b);
        const RenderGraphCompileResult ra = a->compile();
        const RenderGraphCompileResult rb = b->compile();
        check(ra.order == rb.order, "deterministic order cross-instance");
        check(ra.barriers.size() == rb.barriers.size(), "deterministic barriers cross-instance");
        check(ra.lifetimes.size() == rb.lifetimes.size(), "deterministic lifetimes cross-instance");
    }

    // ---- 9. queue transfer barrier (graphics -> compute) ----
    {
        std::string error;
        auto graph = create_render_graph(error);
        const RenderResourceId r = graph->add_resource({
            "r", RenderResourceKind::Image, 0, 1, 1, 1, true, false,
            RenderResourceState::Undefined,
        });
        graph->add_pass({
            "gfx", RenderQueue::Graphics,
            {{r, RenderAccess::Write, RenderResourceState::ColorAttachment}},
            true,
        });
        graph->add_pass({
            "cmp", RenderQueue::Compute,
            {{r, RenderAccess::Read, RenderResourceState::ShaderRead}},
            true,
        });
        const RenderGraphCompileResult compiled = graph->compile();
        check(static_cast<bool>(compiled), "cross-queue graph compiles");
        bool foundTransfer = false;
        for (const auto& b : compiled.barriers)
            if (b.queueOwnershipTransfer) foundTransfer = true;
        check(foundTransfer, "queue ownership transfer barrier emitted");
    }

    if (g_failures == 0) {
        std::printf("[render-graph-core] ALL PASSED\n");
        return 0;
    }
    std::printf("[render-graph-core] %d FAILURE(S)\n", g_failures);
    return 1;
}
