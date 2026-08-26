// BlockMaterialResolverTests.cpp — Agente 1 (task_plan B.1): headless gate
// for the PUBLIC per-face block material resolver (IBlockMaterialResolver).
// Proves the deterministic pure core consuming the data-driven BlockRegistry:
// face overrides, state precedence, render info and stable variant keys — no
// GPU required.

#include "engine/rendering/IBlockMaterialResolver.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::printf("FAIL: %s\n", message);
        ++g_failures;
    }
}

void check(bool condition, const std::string& message) {
    check(condition, message.c_str());
}

bool near(float a, float b, float eps = 1e-5f) {
    return std::fabs(a - b) <= eps;
}

using Engine::Rendering::IBlockMaterialResolver;
using Engine::Rendering::BlockMaterialConfig;
using Engine::Rendering::BlockFace;
using Engine::Rendering::FaceMaterial;
using Engine::Rendering::BlockRenderInfo;
using Engine::Rendering::create_block_material_resolver;
using Engine::Rendering::create_block_material_resolver_json;

// A block with base color + per-face overrides + one named state.
engine::registry::BlockDefinition makeBlock() {
    engine::registry::BlockDefinition d;
    d.ns = "test";
    d.name = "dirt";
    d.color = glm::vec4(0.5f, 0.35f, 0.2f, 1.0f);
    d.faceTop = glm::vec4(0.1f, 0.8f, 0.1f, 1.0f);
    d.faceTopSet = true;
    d.faceBottom = glm::vec4(0.6f, 0.3f, 0.1f, 1.0f);
    d.faceBottomSet = true;
    // faceSide unset: sides fall back to the base color.
    d.lightEmission = 0.1f;
    d.opaque = true;
    d.occludes = false;  // cross-shape hint (e.g. a plant)
    d.renderLayer = 1;
    // Registry semantics: states[0] is the DEFAULT look ("what the block
    // looks like with no state"); named states start at index 1.
    engine::registry::BlockState def0;
    def0.name = "";
    def0.color = d.color;
    d.states.push_back(def0);
    engine::registry::BlockState s;
    s.name = "wet";
    s.color = glm::vec4(0.2f, 0.2f, 0.35f, 1.0f);
    s.faceTop = glm::vec4(0.05f, 0.05f, 0.5f, 1.0f);
    s.faceTopSet = true;
    s.lightEmission = 0.5f;
    d.states.push_back(s);
    return d;
}

}  // namespace

int main() {
    using namespace Engine::Rendering;

    // ---- 1. default config + all-or-nothing refusal ----
    {
        std::string error;
        auto r = create_block_material_resolver(error);
        check(r != nullptr, "default resolver created");
        check(error.empty(), "default config diagnostic empty");

        BlockMaterialConfig bad = r->config();
        bad.variantSeed = 0;
        check(!r->configure(bad, error) && !error.empty(),
              "variantSeed 0 refused");
        check(r->config().variantSeed == 1,
              "config unchanged after refusals (never clamps)");
    }

    // ---- 2. JSON round-trip (bit-exact) + refusals ----
    {
        std::string error;
        auto a = create_block_material_resolver(error);
        BlockMaterialConfig cfg = a->config();
        cfg.variantSeed = 7;
        check(a->configure(cfg, error), "custom config applied");
        auto b = create_block_material_resolver_json(a->config_to_json(), error);
        check(b != nullptr && b->config().variantSeed == 7,
              "json round-trip bit-exact");
        check(create_block_material_resolver_json(
                  "{ \"version\": 1, \"bogus\": 1 }", error) == nullptr,
              "unknown key refused");
        check(create_block_material_resolver_json("{ \"version\": 2 }",
                                                  error) == nullptr,
              "unsupported version refused");
        check(create_block_material_resolver_json(
                  "{ \"version\": 1, \"variantSeed\": 0 }", error) == nullptr,
              "invalid json config refused all-or-nothing");
    }

    // ---- 3. block level: face overrides and base fallback ----
    {
        std::string error;
        auto r = create_block_material_resolver(error);
        const auto def = makeBlock();
        FaceMaterial m;

        // Top uses the block's faceTop override.
        check(r->resolveFace(def, 0, BlockFace::Top, m),
              "top face resolves");
        check(near(m.color.x, 0.1f) && near(m.color.y, 0.8f) &&
                  near(m.color.z, 0.1f),
              "top face uses the block faceTop override");
        // Bottom uses its own override.
        check(r->resolveFace(def, 0, BlockFace::Bottom, m) &&
                  near(m.color.x, 0.6f),
              "bottom face uses the block faceBottom override");
        // Sides are unset -> base color.
        check(r->resolveFace(def, 0, BlockFace::SideNorth, m) &&
                  near(m.color.x, 0.5f) && near(m.color.y, 0.35f),
              "unset side falls back to the block base color");
        check(r->resolveFace(def, 0, BlockFace::SideSouth, m) &&
                  near(m.color.x, 0.5f) && near(m.color.y, 0.35f),
              "all four sides share the side/base material");

        // Invalid face refused, output untouched.
        FaceMaterial untouched;
        untouched.color = glm::vec4(9.0f, 9.0f, 9.0f, 9.0f);
        check(!r->resolveFace(def, 0, static_cast<BlockFace>(99), untouched) &&
                  untouched.color == glm::vec4(9.0f, 9.0f, 9.0f, 9.0f),
              "invalid face refused, output untouched");
    }

    // ---- 4. state precedence ----
    {
        std::string error;
        auto r = create_block_material_resolver(error);
        const auto def = makeBlock();
        FaceMaterial m;

        // stateIndex 0 is the default look -> block level.
        check(r->resolveFace(def, 0, BlockFace::SideNorth, m) &&
                  near(m.color.x, 0.5f),
              "state 0 (default look) resolves at the block level");

        // Active state 1 overrides: its own faceTop wins over the block's.
        check(r->resolveFace(def, 1, BlockFace::Top, m) &&
                  near(m.color.x, 0.05f) && near(m.color.z, 0.5f),
              "active state faceTop overrides the block's");
        // State without a bottom override -> state base color.
        check(r->resolveFace(def, 1, BlockFace::Bottom, m) &&
                  near(m.color.x, 0.2f) && near(m.color.z, 0.35f),
              "state bottom falls back to the state base color");
        // State without a side override -> state base color (not the block's).
        check(r->resolveFace(def, 1, BlockFace::SideEast, m) &&
                  near(m.color.x, 0.2f),
              "state side falls back to the state base color");

        // Out-of-range state index -> block level.
        check(r->resolveFace(def, 5, BlockFace::Top, m) &&
                  near(m.color.x, 0.1f),
              "out-of-range state resolves at the block level");
    }

    // ---- 5. render info ----
    {
        std::string error;
        auto r = create_block_material_resolver(error);
        const auto def = makeBlock();

        BlockRenderInfo base = r->renderInfo(def, 0);
        check(near(base.baseColor.x, 0.5f) && near(base.lightEmission, 0.1f),
              "block render info carries the base color and emission");
        check(!base.occludes && base.renderLayer == 1 &&
                  base.blockClass == engine::registry::BlockClass::Solid &&
                  base.opaque,
              "block render info carries occlusion/layer/class/opacity");

        BlockRenderInfo state = r->renderInfo(def, 1);
        check(near(state.baseColor.x, 0.2f) && near(state.lightEmission, 0.5f),
              "state render info applies the state color and light override");
    }

    // ---- 6. variant keys: stable, distinct, deterministic ----
    {
        std::string error;
        auto r = create_block_material_resolver(error);
        const auto def = makeBlock();

        const std::uint32_t kBlock = r->variantKey(def, 0);
        const std::uint32_t kState = r->variantKey(def, 1);
        check(kBlock != kState, "block and state variants have distinct keys");
        check(r->variantKey(def, 0) == kBlock,
              "variant key is stable across calls (bit-exact)");
        check(r->variantKey(def, 5) == kBlock,
              "out-of-range state uses the block key");

        // A different block (same name, different ns) has a different key.
        engine::registry::BlockDefinition other = def;
        other.ns = "other";
        check(r->variantKey(other, 0) != kBlock,
              "different namespaced name yields a different key");

        // Same material resolved on different faces shares the variant key.
        FaceMaterial a, b;
        check(r->resolveFace(def, 0, BlockFace::Top, a) &&
                  r->resolveFace(def, 0, BlockFace::SideNorth, b) &&
                  a.variantKey == b.variantKey,
              "resolved faces of the same (block, state) share the variant key");
    }

    // ---- 7. determinism (bit-exact) ----
    {
        std::string error;
        auto r = create_block_material_resolver(error);
        const auto def = makeBlock();
        FaceMaterial a, b;
        check(r->resolveFace(def, 1, BlockFace::SideWest, a) &&
                  r->resolveFace(def, 1, BlockFace::SideWest, b),
              "determinism resolves");
        check(std::memcmp(&a, &b, sizeof(FaceMaterial)) == 0,
              "resolveFace reproduces bit-exact results");
        const BlockRenderInfo ri1 = r->renderInfo(def, 1);
        const BlockRenderInfo ri2 = r->renderInfo(def, 1);
        // Field comparison (the struct has bools/enums -> padding bytes are
        // indeterminate; memcmp across two by-value copies is invalid).
        check(ri1.baseColor == ri2.baseColor &&
                  ri1.lightEmission == ri2.lightEmission &&
                  ri1.opaque == ri2.opaque && ri1.occludes == ri2.occludes &&
                  ri1.renderLayer == ri2.renderLayer &&
                  ri1.blockClass == ri2.blockClass,
              "renderInfo reproduces identical results (field-exact)");
    }

    if (g_failures == 0) {
        std::printf("[block-material-resolver] ALL PASSED\n");
        return 0;
    }
    std::printf("[block-material-resolver] %d FAILURE(S)\n", g_failures);
    return 1;
}
