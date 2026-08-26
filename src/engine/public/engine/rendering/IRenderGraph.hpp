#pragma once

// IRenderGraph — Agente 1 (task_plan B2): the PUBLIC Render Graph contract.
// A render graph is a pure DAG (resources, passes, explicit dependencies plus
// resource-hazard edges) that compiles to a deterministic execution order, a
// set of barriers and resource lifetimes. The Vulkan executor consumes this
// model; the model itself is headless and deterministic (no GPU, no clock).
//
// This is the public surface of the internal RenderGraph (src/engine/rendering/
// RenderGraph.{hpp,cpp}), promoted so SDK/MCP/editor can author render graphs
// without touching the engine tree. Self-contained (std only).

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Engine::Rendering {

using RenderResourceId = std::uint32_t;
using RenderPassId = std::uint32_t;
inline constexpr RenderResourceId InvalidRenderResource = 0;
inline constexpr RenderPassId InvalidRenderPass = 0;

enum class RenderResourceKind : std::uint8_t { Buffer, Image };
enum class RenderQueue : std::uint8_t { Graphics, Compute, Transfer };
enum class RenderAccess : std::uint8_t { Read, Write, ReadWrite };
enum class RenderResourceState : std::uint8_t {
    Undefined,
    ShaderRead,
    ColorAttachment,
    DepthAttachment,
    General,
    TransferSource,
    TransferDestination,
    Present
};

struct RenderResourceDesc {
    std::string name;
    RenderResourceKind kind{RenderResourceKind::Image};
    std::uint64_t byteSize{};
    std::uint32_t width{1};
    std::uint32_t height{1};
    std::uint32_t depth{1};
    bool transient{true};
    bool imported{false};
    RenderResourceState initialState{RenderResourceState::Undefined};
};

struct RenderResourceAccess {
    RenderResourceId resource{InvalidRenderResource};
    RenderAccess access{RenderAccess::Read};
    RenderResourceState state{RenderResourceState::ShaderRead};
};

struct RenderPassDesc {
    std::string name;
    RenderQueue queue{RenderQueue::Graphics};
    std::vector<RenderResourceAccess> resources;
    bool enabled{true};
};

struct RenderBarrier {
    RenderResourceId resource{InvalidRenderResource};
    RenderPassId sourcePass{InvalidRenderPass};
    RenderPassId destinationPass{InvalidRenderPass};
    RenderAccess sourceAccess{RenderAccess::Read};
    RenderAccess destinationAccess{RenderAccess::Read};
    RenderResourceState before{RenderResourceState::Undefined};
    RenderResourceState after{RenderResourceState::Undefined};
    RenderQueue sourceQueue{RenderQueue::Graphics};
    RenderQueue destinationQueue{RenderQueue::Graphics};
    bool queueOwnershipTransfer{false};
};

struct RenderResourceLifetime {
    RenderResourceId resource{InvalidRenderResource};
    std::uint32_t firstUse{std::numeric_limits<std::uint32_t>::max()};
    std::uint32_t lastUse{};
    bool transient{true};
};

struct RenderGraphCompileResult {
    std::vector<RenderPassId> order;
    std::vector<RenderBarrier> barriers;
    std::vector<RenderResourceLifetime> lifetimes;
    std::vector<std::string> errors;
    [[nodiscard]] explicit operator bool() const noexcept { return errors.empty(); }
};

class IRenderGraph {
public:
    virtual ~IRenderGraph() = default;

    // Mutation (all-or-nothing per call; invalid ids/duplicates refuse).
    virtual RenderResourceId add_resource(RenderResourceDesc resource) = 0;
    virtual RenderPassId add_pass(RenderPassDesc pass) = 0;
    virtual bool add_dependency(RenderPassId before, RenderPassId after) = 0;
    virtual bool remove_pass(RenderPassId pass) = 0;
    virtual bool remove_resource(RenderResourceId resource) = 0;
    virtual bool set_pass_enabled(RenderPassId pass, bool enabled) = 0;

    // Introspection.
    virtual const RenderResourceDesc* resource(RenderResourceId id) const noexcept = 0;
    virtual const RenderPassDesc* pass(RenderPassId id) const noexcept = 0;
    virtual const std::vector<RenderResourceDesc>& resources() const noexcept = 0;
    virtual const std::vector<RenderPassDesc>& passes() const noexcept = 0;
    virtual const std::vector<RenderResourceId>& resource_ids() const noexcept = 0;
    virtual const std::vector<RenderPassId>& pass_ids() const noexcept = 0;

    // Topological compile: deterministic order + barriers + lifetimes, or
    // errors (cycle / missing resource / duplicate access / undefined state)
    // with the result invalidated (order cleared).
    virtual RenderGraphCompileResult compile() const = 0;
    virtual void clear() = 0;
};

// Helpers shared by SDK/MCP/editor tooling.
[[nodiscard]] bool render_access_writes(RenderAccess access) noexcept;
[[nodiscard]] std::string_view render_state_name(RenderResourceState state) noexcept;

// Public factory (always succeeds).
std::unique_ptr<IRenderGraph> create_render_graph(std::string& errorOut);

}  // namespace Engine::Rendering
