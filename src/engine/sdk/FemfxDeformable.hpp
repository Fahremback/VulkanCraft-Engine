// FemfxDeformable.hpp — internal seam between the deformable factory
// (XpbdDeformable.cpp) and the FEM truss provider (FemfxDeformable.cpp).
//
// NOT part of the public API: the public seam is
// `create_deformable_provider(kind, config, errorOut)` in
// engine/deformable/IDeformableProvider.hpp, which dispatches to this
// function for DeformableProviderKind::Femfx.

#pragma once

#include "engine/deformable/IDeformableProvider.hpp"

#include <memory>
#include <string>

namespace Engine::Deformable {

// Creates the FEM truss provider (linear finite elements over the node/edge
// mesh) with the shared DeformableConfig. Returns nullptr + diagnostic on an
// invalid config (all-or-nothing, same rules as the XPBD provider).
std::unique_ptr<IDeformableProvider> create_femfx_provider(
    const DeformableConfig& config, std::string& errorOut);

}  // namespace Engine::Deformable
