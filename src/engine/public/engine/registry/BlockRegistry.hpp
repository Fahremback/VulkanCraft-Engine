#pragma once

// Data-driven block registry (SDK). Blocks are defined by assets (JSON) or
// programmatically; identities are persistent UUIDs, so ids survive reorder,
// updates, save/load and multiplayer. An invalid asset fails with a clear
// diagnostic and never breaks the registry: lookups for unknown ids return
// nullptr and callers fall back to BlockRegistry::fallback().

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine {
namespace registry {

enum class BlockClass : uint8_t { Solid = 0, Fluid = 1, Transparent = 2, NonSolid = 3 };

// One named block state (FALTANTES item 5): an alternative material/look for
// the same block, resolved by the runtime when a state is active. states[0] is
// the default state (what the block looks like with no state). Per-state face
// overrides follow the same semantics as the block-level ones; unset faces
// fall back to the state's base color.
struct BlockState {
    std::string name;                  // unique within the block; non-empty
    glm::vec4 color{ 1.0f, 1.0f, 1.0f, 1.0f };
    glm::vec4 faceTop{ 1.0f, 1.0f, 1.0f, 1.0f };
    glm::vec4 faceBottom{ 1.0f, 1.0f, 1.0f, 1.0f };
    glm::vec4 faceSide{ 1.0f, 1.0f, 1.0f, 1.0f };
    bool faceTopSet{ false };
    bool faceBottomSet{ false };
    bool faceSideSet{ false };
    float lightEmission{ 0.0f };       // optional per-state light override
};

// A versioned state transition rule (FALTANTES item 5): "from state X, when
// trigger T fires, become state Y". "" refers to the default state (states[0]
// / no state). Rules are data-driven; the runtime that fires triggers is a
// later milestone (abilities/block entities).
struct BlockTransition {
    std::string fromState;             // "" = default state
    std::string toState;
    std::string trigger;               // non-empty trigger name
};

// Tool class required to mine a block efficiently (FALTANTES item 4). Any =
// no dedicated tool (hand suffices); the specific classes gate the mining
// speed. Unknown values are refused (strict enum, never guessed).
enum class BlockTool : uint8_t { Any = 0, Pickaxe, Axe, Shovel, Hoe, Sword };

// Collision shape (FALTANTES item 2): how the block occupies a cell for
// collision/raycast. Full = the whole cell (default), Cross = a thin
// X-shaped hitbox (fences/plants — consumed by the physics milestone), None =
// not collidable at all (ghost blocks; the voxel raycast skips them).
// Effective solidity is `collidable && collisionShape != None`, so a
// collisionShape of None wins even when the legacy `collidable: true` bool is
// set. JSON: "collisionShape": "full|cross|none".
enum class CollisionShape : uint8_t { Full = 0, Cross = 1, None = 2 };

// Selection shape (FALTANTES item 2): the pick-box the editor/highlight uses
// for this block. Full = the whole cell, Cross = thin selection (plants),
// None = not selectable. Mirrored into the runtime table for the editor
// milestone; the headless selection path is a later milestone.
// JSON: "selectionShape": "full|cross|none".
enum class SelectionShape : uint8_t { Full = 0, Cross = 1, None = 2 };

// Inline fluid binding (FALTANTES item 7): a block can declare the fluid
// behavior it drives directly, so a catalog-only fluid block needs no separate
// FluidRegistry asset. `declared` distinguishes "the block is this fluid" from
// "no inline binding". Semantics mirror FluidDefinition; an explicit
// FluidRegistry entry for the same block wins over the inline binding when the
// world builds its fluid table.
struct FluidBinding {
    bool declared{ false };
    float viscosity{ 0.5f };           // 0..1 (0 = thin, 2 levels/tick)
    float density{ 1.0f };
    int range{ 7 };                    // 1..7 spread budget
    float tickInterval{ 0.08f };       // seconds between simulation steps
    bool source{ true };
    bool falling{ true };
    bool evaporation{ true };
    float damagePerTick{ 0.0f };
    bool compressible{ false };
};

struct BlockDefinition {
    // Persistent canonical UUID. Empty means "derive a stable id from ns:name"
    // (the same namespaced name always produces the same UUID).
    std::string uuid;
    std::string ns{ "vulkancraft" };
    std::string name;

    BlockClass blockClass{ BlockClass::Solid };
    float hardness{ 1.0f };
    float lightEmission{ 0.0f };
    float lightAbsorption{ 1.0f };
    // Data-driven base color (linear RGBA 0..1). Used when a block has no
    // baked-in texture (JSON-only blocks); builtin blocks keep their engine
    // material table. JSON: "color": [r, g, b] or [r, g, b, a].
    glm::vec4 color{ 1.0f, 1.0f, 1.0f, 1.0f };
    // Per-face material overrides (FALTANTES §14 "material/modelo por face"):
    // when the *_set flag is true the mesher uses that face color instead of
    // `color` for the +Y (top), -Y (bottom) and horizontal (side) faces.
    // JSON: "faceTop": [r,g,b(,a)], "faceBottom": ..., "faceSide": ...
    glm::vec4 faceTop{ 1.0f, 1.0f, 1.0f, 1.0f };
    glm::vec4 faceBottom{ 1.0f, 1.0f, 1.0f, 1.0f };
    glm::vec4 faceSide{ 1.0f, 1.0f, 1.0f, 1.0f };
    bool faceTopSet{ false };
    bool faceBottomSet{ false };
    bool faceSideSet{ false };
    // Occlusion (FALTANTES §14 "oclusão"): when false the mesher draws faces
    // even against opaque neighbors (face-culling hint for cross-shape blocks
    // like fences/plants). JSON: "occlusion": false.
    bool occludes{ true };
    // Render layer hint (FALTANTES §14 "render layer"): 0 = opaque world
    // layer, >0 = layered/overlay. JSON: "renderLayer": 1 (valid 0..255).
    int32_t renderLayer{ 0 };
    bool opaque{ true };
    bool collidable{ true };

    // Collision/selection shapes (FALTANTES item 2). collisionShape drives the
    // runtime solidity (None = not solid even with collidable true); the voxel
    // raycast and future physics consume it. selectionShape feeds the editor
    // pick-box milestone.
    CollisionShape collisionShape{ CollisionShape::Full };
    SelectionShape selectionShape{ SelectionShape::Full };

    // Effective collision: legacy `collidable` bool ANDED with the shape (None
    // always wins). Mirrored into RuntimeBlockInfo.solid by the facade.
    bool is_collidable() const {
        return collidable && collisionShape != CollisionShape::None;
    }

    // Engine builtin block id (BlockType) when this definition maps to one of
    // the engine's baked-in blocks. hasBuiltinMapping distinguishes "maps to a
    // baked-in engine block (id < BlockType::Count)" from "catalog-only": a
    // purely data-driven block without a mapping gets a DYNAMIC runtime id
    // (>= Count) allocated deterministically from its UUID — it participates
    // in the world (place/raycast/save/load) with data-driven material.
    uint32_t builtinId{ 0 };
    bool hasBuiltinMapping{ false };

    std::vector<std::string> tags;
    std::vector<std::string> drops;

    // Named block states + versioned transitions (FALTANTES item 5). states[0]
    // is the default; transitions reference state names or "" (default).
    std::vector<BlockState> states;
    std::vector<BlockTransition> transitions;

    // Inline fluid binding (FALTANTES item 7): when `fluid.declared` is true
    // the block drives the fluid simulation with these properties directly.
    FluidBinding fluid;

    // Sound + particle component (FALTANTES item 4): namespaced asset ids for
    // the block's interaction events; empty = engine default. Reference
    // format is validated at registration (ns:name); playback belongs to the
    // audio/particle systems (project/milestone).
    std::string soundPlace;       // "soundPlace": "vulkancraft:stone_place"
    std::string soundBreak;
    std::string soundStep;
    std::string soundHit;
    std::string particleBreak;    // "particleBreak": "vulkancraft:stone_dust"

    // Tool component (FALTANTES item 4): required tool class + tier for
    // efficient mining (tier 0..4 = wood..netherite). Tool gating belongs to
    // the gameplay milestone; the values are validated and mirrored here.
    BlockTool tool{ BlockTool::Any };
    int toolTier{ 0 };

    // Behavior component (FALTANTES item 6): namespaced reference (ns:name)
    // to a registered block behavior id; empty = no behavior. Validated (not
    // resolved) at registry time — resolution belongs to the abilities/block
    // entity milestone, mirroring ItemDefinition.behaviorId.
    std::string behaviorId;

    // Resistance + physical properties (FALTANTES item 4). resistance scales
    // destruction/explosion work (hardness is the mining counterpart);
    // friction/bounciness/density feed the physics response when a block
    // produces dynamic bodies (Jolt milestone).
    float resistance{ 0.0f };
    float friction{ 0.5f };
    float bounciness{ 0.0f };
    float density{ 1.0f };

    int32_t version{ 1 };

    std::string namespaced() const { return ns + ":" + name; }

    // Index of a named state (0 = default when absent); -1 if unknown.
    int state_index(const std::string& name) const {
        if (name.empty()) return 0;
        for (std::size_t i = 0; i < states.size(); ++i) {
            if (states[i].name == name) return static_cast<int>(i);
        }
        return -1;
    }
};

// Registry JSON schema (single object or array of objects):
// {
//   "id": "00000000-0000-0000-0000-000000000001",   // optional; derived from ns:name when absent
//   "name": "stone",                                // required
//   "namespace": "vulkancraft",                     // optional (default)
//   "class": "solid|fluid|transparent|nonsolid",
//   "hardness": 1.5,
//   "lightEmission": 0.0,
//   "lightAbsorption": 1.0,
//   "opaque": true,
//   "collidable": true,
//   "collisionShape": "full",         // full|cross|none (item 2; none = not solid)
//   "selectionShape": "full",         // full|cross|none (item 2; editor pick-box)
//   "faceTop": [0.2, 0.8, 0.2],     // optional per-face color overrides
//   "faceBottom": [0.4, 0.3, 0.2],  // (top = +Y, bottom = -Y, side = horizontal)
//   "faceSide": [0.4, 0.3, 0.2],
//   "occlusion": true,
//   "renderLayer": 0,
//   "builtinId": 3,
//   "tags": ["stone", "mineable"],
//   "drops": ["vulkancraft:stone"],
//   "states": [                                   // optional named states
//     { "name": "lit", "color": [1.0, 0.6, 0.1], "lightEmission": 0.8 }
//   ],
//   "transitions": [                              // optional versioned rules
//     { "from": "", "to": "lit", "trigger": "ignite" }
//   ],
//   "fluid": {                                    // optional inline fluid binding (item 7)
//     "viscosity": 0.5, "density": 1.0, "range": 7, "tickInterval": 0.08,
//     "source": true, "falling": true, "evaporation": true,
//     "damagePerTick": 0.0, "compressible": false
//   },
//   "soundPlace": "vulkancraft:stone_place",      // optional namespaced ids (item 4)
//   "soundBreak": "vulkancraft:stone_break",
//   "soundStep": "vulkancraft:stone_step",
//   "soundHit": "vulkancraft:stone_hit",
//   "particleBreak": "vulkancraft:stone_dust",
//   "tool": "pickaxe",                            // any|pickaxe|axe|shovel|hoe|sword
//   "toolTier": 0,                                 // 0..4
//   "resistance": 6.0,                             // >= 0
//   "friction": 0.6,                               // 0..1
//   "bounciness": 0.0,                             // 0..1
//   "density": 2.5,                                // > 0
//   "behaviorId": "vulkancraft:spread_fire",      // optional namespaced ref (item 6)
//   "version": 1
// }
class BlockRegistry {
public:
    BlockRegistry();

    // Programmatic registration. Returns false with a structured diagnostic
    // (field + reason) on invalid definitions; duplicate name/uuid is an error.
    bool register_block(const BlockDefinition& definition, std::string& errorOut);

    // Loads a JSON asset (single object or array of objects). Parsing and
    // field errors are reported with a clear diagnostic; valid entries are
    // registered, invalid ones are skipped with per-entry diagnostics.
    bool load_from_json(const std::string& jsonText, std::string& errorOut);

    const BlockDefinition* find_by_uuid(const std::string& uuid) const;
    const BlockDefinition* find_by_name(const std::string& namespacedName) const;
    const BlockDefinition* find_by_builtin(uint32_t builtinId) const;

    // Safe fallback for unknown ids (never null).
    const BlockDefinition& fallback() const { return fallback_; }
    std::size_t size() const { return byUuid_.size(); }
    std::vector<std::string> all_names() const;

    // Cross-reference validation (FALTANTES item 9): every drop of every
    // catalog-only (user-authored) block must be namespaced and resolvable in
    // the given ItemRegistry. Builtin blocks are the engine's own contract and
    // are skipped. Collects one diagnostic per dangling reference and returns
    // whether the registry is clean.
    bool validate_item_references(const class ItemRegistry& items,
                                  std::vector<std::string>& errorsOut) const;

    // All registered definitions, sorted by UUID. Runtime id allocation uses
    // this deterministic order so ids never depend on JSON load order.
    std::vector<BlockDefinition> all_definitions() const;

private:
    bool add(BlockDefinition definition, std::string& errorOut);

    std::unordered_map<std::string, BlockDefinition> byUuid_;
    std::unordered_map<std::string, BlockDefinition> byName_;
    std::unordered_map<uint32_t, BlockDefinition> byBuiltin_;
    BlockDefinition fallback_;
};

}  // namespace registry
}  // namespace engine
