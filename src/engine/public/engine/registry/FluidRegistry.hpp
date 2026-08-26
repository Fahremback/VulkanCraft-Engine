#pragma once

// Data-driven fluid registry (SDK, META section 13). Fluids are not a hardcoded
// water special case: a FluidDefinition attaches simulation behavior to a
// registered block (by namespaced name), so the project declares what water,
// lava or any custom fluid IS. The engine owns the simulation machinery
// (levels, spreading, scheduler cadence, persistence); the definition owns the
// parameters.
//
// Identities are persistent UUIDs (explicit or derived from the block name),
// so definitions survive reorder, updates, save/load and multiplayer. An
// invalid asset fails with a clear diagnostic and never breaks the registry.

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine {
namespace registry {

// Simulation parameters for one fluid. The fluid is the block's behavior: the
// block (BlockClass::Fluid or not — the runtime does not require it) carries
// identity, light emission and material; this definition carries how it flows.
struct FluidDefinition {
    // Persistent canonical UUID. Empty means "derive a stable id from the
    // block's namespaced name" (the same block always produces the same UUID).
    std::string uuid;
    // Namespaced block name the fluid drives (required, e.g. "minecraft:lava"
    // or "test:sludge"). Must be registered in the block registry before the
    // fluid registry is attached to a world.
    std::string block;

    // 0..1 rheology hint. 0 (thin) spreads 2 levels per simulation step;
    // >= 0.75 (thick) spreads 1 level per step. The spread budget is `range`.
    float viscosity{ 0.5f };
    float density{ 1.0f };
    // Horizontal spread budget in levels (1..7; 7 = the engine's level space).
    // A cell only spreads while its next level stays within the budget.
    int range{ 7 };
    // Seconds between simulation steps for this fluid (0 = every fluid tick).
    // The world's fluid cadence is 0.08s; a value of 0.16 makes the fluid step
    // every 2nd tick. Slower fluids visibly lag faster ones.
    float tickInterval{ 0.08f };
    // A source cell (level 0) is always fed and never decays. Placing a fluid
    // block creates a source; flowing cells are non-source.
    bool source{ true };
    // Falling-flag semantics: fluid over air falls (drops downward carrying
    // the falling flag) instead of only spreading horizontally.
    bool falling{ true };
    // true: an unfed non-source cell disappears (current water behavior).
    // false: an unfed cell keeps its level (pooled fluid, never evaporates).
    bool evaporation{ true };
    // Damage per simulated second applied to entities inside the fluid
    // (e.g. lava 4.0). 0 = harmless.
    float damagePerTick{ 0.0f };
    // Visual material hint (linear RGBA 0..1) for the renderer milestone.
    glm::vec4 color{ 0.30f, 0.60f, 1.00f, 0.65f };
    bool compressible{ false };

    // ---- Temperature / solidification / combustion (task D.3) ----------
    // Declared heat axis (Kelvin). Finite; carried into the world's fluid
    // table. Not a thermal simulation — it is the data-driven marker the
    // reaction rules below hang off.
    float temperature{ 300.0f };
    // Namespaced block an UNFED flowing cell turns into ("solidifies"): the
    // fluid's cooled/edge solid form (e.g. lava -> obsidian/cobblestone).
    // Empty = no solidification (normal evaporation/pooling). Resolved at
    // world build; an unknown block is refused (never guessed).
    std::string solidifiesInto;
    // The fluid ignites adjacent flammable blocks (registry blocks with
    // flammability > 0): they are consumed to air. Deterministic and bounded
    // to the fluid tick.
    bool ignites{ false };
    int32_t version{ 1 };
};

// Registry JSON schema (single object or array of objects):
// {
//   "block": "minecraft:lava",     // required (namespaced block name)
//   "id": "00000000-...",          // optional; derived from the block name
//   "viscosity": 1.0,
//   "density": 1.0,
//   "range": 3,
//   "tickInterval": 0.16,
//   "source": true,
//   "falling": true,
//   "evaporation": true,
//   "damagePerTick": 4.0,
//   "color": [1.0, 0.4, 0.1, 0.9],
//   "compressible": false,
//   "temperature": 1300.0,          // optional declared heat (Kelvin)
//   "solidifiesInto": "test:obsidian",  // optional block the edge cell cools into
//   "ignites": true,                // optional: ignites adjacent flammable blocks
//   "version": 1
// }
class FluidRegistry {
public:
    FluidRegistry();

    // Programmatic registration. Returns false with a structured diagnostic
    // on invalid definitions; duplicate block/uuid is an error.
    bool register_fluid(const FluidDefinition& definition, std::string& errorOut);

    // Loads a JSON asset (single object or array of objects). Parsing and
    // field errors are reported with a clear diagnostic; valid entries are
    // registered, invalid ones are skipped with per-entry diagnostics.
    bool load_from_json(const std::string& jsonText, std::string& errorOut);

    const FluidDefinition* find_by_uuid(const std::string& uuid) const;
    const FluidDefinition* find_by_block(const std::string& namespacedBlock) const;

    std::size_t size() const { return byBlock_.size(); }

    // All registered definitions, sorted by UUID (deterministic order; the
    // world builds its fluid table from this sequence).
    std::vector<FluidDefinition> all_definitions() const;

    // Cross-reference validation (FALTANTES item 9): the namespaced block each
    // fluid drives must resolve in the given BlockRegistry (builtin or
    // catalog). Collects one diagnostic per dangling reference and returns
    // whether the registry is clean.
    bool validate_block_references(const class BlockRegistry& blocks,
                                   std::vector<std::string>& errorsOut) const;

private:
    bool add(FluidDefinition definition, std::string& errorOut);

    std::unordered_map<std::string, FluidDefinition> byBlock_;
    std::unordered_map<std::string, FluidDefinition> byUuid_;
};

}  // namespace registry
}  // namespace engine
