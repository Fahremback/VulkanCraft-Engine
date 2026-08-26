#pragma once

// IBlockMaterialResolver — Agente 1 (task_plan B.1), the PUBLIC per-face
// material resolver: consumes the engine's data-driven BlockRegistry (the
// voxel domain's contract) and resolves the effective material color, render
// info and a deterministic variant key for each face of a block, so the
// renderer has no builtin-only paths for block materials. One surface for the
// material RESOLUTION MATH, without depending on the concrete backend.
//
// SCOPE: the deterministic, headless ALGORITHM of block material resolution:
//   precedence  — an active named state (index >= 1) overrides the block:
//                 the state's face override when set, else the state's base
//                 color; otherwise the block's face override when set, else
//                 the block's base color. states[0] IS the default look
//                 ("what the block looks like with no state"), so index 0 and
//                 out-of-range resolve at the block level;
//   faces       — 0 = +Y (top), 1 = -Y (bottom), 2..5 = horizontal sides;
//   renderInfo  — lightEmission (per-state override), opaque, occludes,
//                 renderLayer and block class for the renderer's draw policy;
//   variantKey  — a stable FNV-1a key of (namespaced name + state), the
//                 dedup key the renderer uses to merge identical materials.
// Self-contained (std + glm, consumes engine/registry/BlockRegistry.hpp),
// bit-exact for the same inputs on every machine.

#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <string>

#include "engine/registry/BlockRegistry.hpp"

namespace Engine::Rendering {

// ---- config (validated all-or-nothing) ----

struct BlockMaterialConfig {
    std::uint32_t variantSeed{ 1 };  // FNV offset basis for variant keys (non-zero)

    // All-or-nothing: out-of-range values are refused, never clamped.
    bool valid(std::string& errorOut) const;
};

// ---- the deterministic, headless core (task_plan B.1) ----

// Face convention: 0 = +Y (top), 1 = -Y (bottom), 2..5 = sides (NSWE, fixed
// order). Any other value is refused (face() returns false).
enum class BlockFace : std::uint8_t {
    Top = 0,
    Bottom = 1,
    SideNorth = 2,
    SideSouth = 3,
    SideEast = 4,
    SideWest = 5,
    Count
};

struct FaceMaterial {
    glm::vec4 color{ 1.0f, 1.0f, 1.0f, 1.0f };  // resolved linear RGBA
    std::uint32_t variantKey{ 0 };               // renderer dedup key
};

struct BlockRenderInfo {
    glm::vec4 baseColor{ 1.0f, 1.0f, 1.0f, 1.0f };
    float lightEmission{ 0.0f };              // per-state override applied
    bool opaque{ true };
    bool occludes{ true };
    std::int32_t renderLayer{ 0 };
    engine::registry::BlockClass blockClass{ engine::registry::BlockClass::Solid };
};

class IBlockMaterialResolver {
public:
    virtual ~IBlockMaterialResolver() = default;

    // Validates and applies the config (all-or-nothing: refused, never clamped).
    virtual bool configure(const BlockMaterialConfig& config,
                           std::string& errorOut) = 0;
    virtual const BlockMaterialConfig& config() const noexcept = 0;

    // JSON {variantSeed, version:1}. version != 1 or a malformed field
    // refuses all-or-nothing.
    virtual bool configure_json(const std::string& jsonText,
                                std::string& errorOut) = 0;
    virtual std::string config_to_json() const = 0;

    // Resolves the effective material of one face. `stateIndex` follows the
    // registry semantics: 0 or out-of-range = block level (states[0] is the
    // default look); a valid active state (>= 1) overrides the block. Returns
    // false (output untouched) for an invalid face index.
    virtual bool resolveFace(const engine::registry::BlockDefinition& definition,
                             int stateIndex, BlockFace face,
                             FaceMaterial& out) const noexcept = 0;

    // The renderer's per-block draw policy (light emission, opacity,
    // occlusion, layer, class), with the state's light override applied.
    virtual BlockRenderInfo renderInfo(
        const engine::registry::BlockDefinition& definition,
        int stateIndex) const noexcept = 0;

    // Stable dedup key: FNV-1a of (namespaced name + active state name).
    // Identical (block, state) inputs always produce the same key.
    virtual std::uint32_t variantKey(
        const engine::registry::BlockDefinition& definition,
        int stateIndex) const noexcept = 0;
};

// ---- public factory ----

std::unique_ptr<IBlockMaterialResolver> create_block_material_resolver(
    std::string& errorOut);
std::unique_ptr<IBlockMaterialResolver> create_block_material_resolver_json(
    const std::string& jsonText, std::string& errorOut);

}  // namespace Engine::Rendering
