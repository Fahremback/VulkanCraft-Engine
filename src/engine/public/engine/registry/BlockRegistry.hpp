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
    bool opaque{ true };
    bool collidable{ true };

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
    int32_t version{ 1 };

    std::string namespaced() const { return ns + ":" + name; }
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
//   "builtinId": 3,
//   "tags": ["stone", "mineable"],
//   "drops": ["vulkancraft:stone"],
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
