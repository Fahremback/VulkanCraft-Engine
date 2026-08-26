// RenderGraphInternalProbe.hpp — Agente 1 (task_plan B2): interface of the
// probe TU that compiles the REAL internal RenderGraph and exposes the
// canonical compile result as plain data, so the gate can prove bit-exact
// equivalence between the public adapter and the internal implementation.
// Lives in tests/ (not public API).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Engine::Rendering::InternalProbe {

struct PlainBarrier {
    std::uint32_t resource{0};
    std::uint32_t sourcePass{0};
    std::uint32_t destinationPass{0};
    std::uint8_t sourceAccess{0};
    std::uint8_t destinationAccess{0};
    std::uint8_t before{0};
    std::uint8_t after{0};
    std::uint8_t sourceQueue{0};
    std::uint8_t destinationQueue{0};
    bool queueOwnershipTransfer{false};
};

struct PlainLifetime {
    std::uint32_t resource{0};
    std::uint32_t firstUse{0};
    std::uint32_t lastUse{0};
    bool transient{true};
};

struct ProbeResult {
    std::vector<std::uint32_t> order;
    std::vector<PlainBarrier> barriers;
    std::vector<PlainLifetime> lifetimes;
    std::vector<std::string> errors;
};

// Builds the canonical 3-pass/2-resource graph through the INTERNAL
// RenderGraph and returns its compile result as plain data.
ProbeResult compile_canonical_internal();

}  // namespace Engine::Rendering::InternalProbe
