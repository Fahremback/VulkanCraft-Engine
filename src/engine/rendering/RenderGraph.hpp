#pragma once

// The shared Render Graph model types (RenderResourceId, RenderAccess,
// RenderResourceDesc, RenderBarrier, RenderGraphCompileResult, ...) live in the
// PUBLIC contract engine/public/engine/rendering/IRenderGraph.hpp. This
// concrete implementation header includes that contract instead of redefining
// the identical types — redefining them collided (C2011) in translation units
// that include BOTH this header and the public one (e.g. VulkanEngineApp,
// VulkanEngineServer).
#include "engine/public/engine/rendering/IRenderGraph.hpp"

#include <string_view>
#include <vector>

namespace Engine::Rendering {

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
