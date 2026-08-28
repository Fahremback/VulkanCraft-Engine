#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Engine::Rendering {

using RenderResourceId = uint32_t;
using RenderPassId = uint32_t;
inline constexpr RenderResourceId InvalidRenderResource = 0;
inline constexpr RenderPassId InvalidRenderPass = 0;

enum class RenderResourceKind : uint8_t { Buffer, Image };
enum class RenderQueue : uint8_t { Graphics, Compute, Transfer };
enum class RenderAccess : uint8_t { Read, Write, ReadWrite };
enum class RenderResourceState : uint8_t {
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
    uint64_t byteSize{};
    uint32_t width{1};
    uint32_t height{1};
    uint32_t depth{1};
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
    uint32_t firstUse{std::numeric_limits<uint32_t>::max()};
    uint32_t lastUse{};
    bool transient{true};
};

struct RenderGraphCompileResult {
    std::vector<RenderPassId> order;
    std::vector<RenderBarrier> barriers;
    std::vector<RenderResourceLifetime> lifetimes;
    std::vector<std::string> errors;
    [[nodiscard]] explicit operator bool() const noexcept { return errors.empty(); }
};

class RenderGraph final {
public:
    [[nodiscard]] RenderResourceId add_resource(RenderResourceDesc resource);
    [[nodiscard]] RenderPassId add_pass(RenderPassDesc pass);
    [[nodiscard]] bool add_dependency(RenderPassId before, RenderPassId after);
    [[nodiscard]] bool remove_pass(RenderPassId pass);
    [[nodiscard]] bool remove_resource(RenderResourceId resource);
    [[nodiscard]] bool set_pass_enabled(RenderPassId pass, bool enabled);

    [[nodiscard]] const RenderResourceDesc* resource(RenderResourceId id) const noexcept;
    [[nodiscard]] const RenderPassDesc* pass(RenderPassId id) const noexcept;
    [[nodiscard]] const std::vector<RenderResourceDesc>& resources() const noexcept { return resources_; }
    [[nodiscard]] const std::vector<RenderPassDesc>& passes() const noexcept { return passes_; }
    [[nodiscard]] const std::vector<RenderResourceId>& resource_ids() const noexcept { return resourceIds_; }
    [[nodiscard]] const std::vector<RenderPassId>& pass_ids() const noexcept { return passIds_; }
    [[nodiscard]] RenderGraphCompileResult compile() const;
    void clear();

private:
    struct Dependency { RenderPassId before; RenderPassId after; };
    RenderResourceId nextResourceId_{1};
    RenderPassId nextPassId_{1};
    std::vector<RenderResourceDesc> resources_;
    std::vector<RenderPassDesc> passes_;
    std::vector<RenderResourceId> resourceIds_;
    std::vector<RenderPassId> passIds_;
    std::vector<Dependency> dependencies_;
};

[[nodiscard]] inline bool render_access_writes(RenderAccess access) noexcept {
    return access == RenderAccess::Write || access == RenderAccess::ReadWrite;
}
[[nodiscard]] inline std::string_view render_state_name(RenderResourceState state) noexcept;

} // namespace Engine::Rendering
