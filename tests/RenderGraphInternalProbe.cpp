// RenderGraphInternalProbe.cpp — Agente 1 (task_plan B2): compiles the REAL
// internal RenderGraph implementation (src/engine/rendering/RenderGraph.cpp)
// into this TU and exposes the compile result of a canonical graph as plain
// data, so the gate can prove the PUBLIC adapter (RenderGraphCore.cpp) is
// bit-exact equivalent to the proven internal core. The two free helpers are
// renamed in this TU only to avoid ODR collisions with the public adapter.
//
// The canonical graph is the SAME one the gate builds through the public API:
//   resources r1, r2 (Image, transient, Undefined)
//   P1 "shadow"    : reads r1 (ShaderRead), writes r2 (ColorAttachment)
//   P2 "gbuffer"   : reads r2 (ShaderRead), writes r1 (ColorAttachment)
//   P3 "composite" : reads r1 (ShaderRead)
//   explicit dep: P2 before P3
// Expected: order [P1, P2, P3]; barriers on r2 (P1→P2) and r1 (P1→P2, P2→P3).

#define render_access_writes render_access_writes_internal
#define render_state_name render_state_name_internal
#include "../src/engine/rendering/RenderGraph.cpp"
#undef render_access_writes
#undef render_state_name

#include "RenderGraphInternalProbe.hpp"

namespace Engine::Rendering::InternalProbe {

ProbeResult compile_canonical_internal() {
    Engine::Rendering::RenderGraph graph;
    const Engine::Rendering::RenderResourceId r1 = graph.add_resource({
        "r1",
        Engine::Rendering::RenderResourceKind::Image,
        0,
        1, 1, 1,
        true,
        false,
        Engine::Rendering::RenderResourceState::Undefined,
    });
    const Engine::Rendering::RenderResourceId r2 = graph.add_resource({
        "r2",
        Engine::Rendering::RenderResourceKind::Image,
        0,
        1, 1, 1,
        true,
        false,
        Engine::Rendering::RenderResourceState::Undefined,
    });

    const Engine::Rendering::RenderPassId p1 = graph.add_pass({
        "shadow",
        Engine::Rendering::RenderQueue::Graphics,
        {
            {r1, Engine::Rendering::RenderAccess::Read, Engine::Rendering::RenderResourceState::ShaderRead},
            {r2, Engine::Rendering::RenderAccess::Write, Engine::Rendering::RenderResourceState::ColorAttachment},
        },
        true,
    });
    const Engine::Rendering::RenderPassId p2 = graph.add_pass({
        "gbuffer",
        Engine::Rendering::RenderQueue::Graphics,
        {
            {r2, Engine::Rendering::RenderAccess::Read, Engine::Rendering::RenderResourceState::ShaderRead},
            {r1, Engine::Rendering::RenderAccess::Write, Engine::Rendering::RenderResourceState::ColorAttachment},
        },
        true,
    });
    const Engine::Rendering::RenderPassId p3 = graph.add_pass({
        "composite",
        Engine::Rendering::RenderQueue::Graphics,
        {
            {r1, Engine::Rendering::RenderAccess::Read, Engine::Rendering::RenderResourceState::ShaderRead},
        },
        true,
    });
    (void)graph.add_dependency(p2, p3);

    const Engine::Rendering::RenderGraphCompileResult compiled = graph.compile();

    ProbeResult result;
    result.order = compiled.order;
    for (const auto& b : compiled.barriers) {
        result.barriers.push_back({
            b.resource, b.sourcePass, b.destinationPass,
            static_cast<std::uint8_t>(b.sourceAccess),
            static_cast<std::uint8_t>(b.destinationAccess),
            static_cast<std::uint8_t>(b.before),
            static_cast<std::uint8_t>(b.after),
            static_cast<std::uint8_t>(b.sourceQueue),
            static_cast<std::uint8_t>(b.destinationQueue),
            b.queueOwnershipTransfer,
        });
    }
    for (const auto& l : compiled.lifetimes) {
        result.lifetimes.push_back({l.resource, l.firstUse, l.lastUse, l.transient});
    }
    result.errors = compiled.errors;
    return result;
}

}  // namespace Engine::Rendering::InternalProbe
