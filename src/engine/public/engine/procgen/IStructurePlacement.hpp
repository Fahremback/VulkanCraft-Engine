#pragma once

// Public structure-placement contract (META section 18 / FALTANTES item 14 —
// "sistema data-driven de estruturas, sockets e regras de spawn"). The
// structure GENERATION is closed (IStructureGenerator / fast-wfc); this is the
// missing half: WHERE and WHEN structures appear in the world, data-driven,
// plus the SOCKETS that connect structures to each other.
//
// Model:
//   - A StructureDefinition = a named WFC asset (StructureAssetSpec) + its
//     declared sockets (local connection points with a facing and a tag).
//   - A StructureSpawnRule = when/where a structure spawns: biome names,
//     surface-height range, density (per candidate cell), spacing (the
//     candidate grid), y offset and a per-rule seed offset. Rules are
//     data-driven (JSON, versioned, all-or-nothing) — a project composes
//     world population without recompiling the engine.
//   - The placement system is a PURE function of (rules, world seed, column):
//     the same inputs always produce the same decision and the same derived
//     placement seed (splitmix64 mixing), so world population is deterministic
//     per seed and never depends on call order or threads.
//
// Self-contained: public SDK headers + glm only. No backend beyond composing
// the public IStructureGenerator.

#include "engine/procgen/IStructureGenerator.hpp"
#include "engine/voxel/IVoxelWorld.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace procgen {

// A connection point on a structure: a local position where another structure
// can attach. Two sockets connect when their connectTag matches (either may be
// empty = any) AND their facings are opposite (they face each other).
// `facing` is a cardinal world direction: 0 = +X, 1 = -X, 2 = +Z, 3 = -Z.
struct StructureSocket {
    std::string name;             // unique within the structure
    glm::ivec3 position{ 0, 0, 0 };  // local position (structure space)
    int facing{ 0 };              // 0=+X 1=-X 2=+Z 3=-Z
    std::string connectTag;       // "" = connects to any

    bool operator==(const StructureSocket&) const = default;
};

// A named structure definition: the WFC asset + its declared sockets.
struct StructureDefinition {
    std::string id;               // namespaced "ns:name" (unique)
    StructureAssetSpec spec;
    // Generated footprint; 0 = default to the sample size. Must be >= the
    // pattern window when set.
    int outputWidth{ 0 };
    int outputHeight{ 0 };
    std::vector<StructureSocket> sockets;
};

// Data-driven spawn rule (FALTANTES item 14): when/where a structure appears.
// The system evaluates rules in order for each candidate cell — the FIRST rule
// whose gates (biome, surface height, density hash) all pass wins. Gates are
// strict: a rule whose structureId is unknown is a hard error (dangling data
// is refused with a diagnostic, never silently skipped).
struct StructureSpawnRule {
    std::string structureId;          // ref to a registered StructureDefinition
    std::vector<std::string> biomes;  // allowed biome names; empty = any
    int minSurfaceHeight{ -100000 };  // gate: surface in [min, max]
    int maxSurfaceHeight{ 100000 };
    float density{ 0.0f };            // 0..1 spawn probability per candidate cell
    int spacing{ 8 };                 // candidate grid size (blocks, >= 1)
    int yOffset{ 1 };                 // blocks above the surface for the origin
    std::uint32_t seedOffset{ 0 };    // per-rule seed mixing (determinism)

    bool operator==(const StructureSpawnRule&) const = default;
};

// A structure placed in the world by a spawn rule: the world origin of the
// structure's local (0,0,0) and the generated content. `placementSeed` is
// derived deterministically from (world seed, rule, cell) — the caller uses it
// for any further per-instance randomization so content is stable per seed.
struct StructurePlacement {
    std::string structureId;       // the definition that spawned
    glm::ivec3 origin{ 0, 0, 0 };  // world position of the local origin
    std::uint32_t placementSeed{ 0 };
    StructureOutput output;        // the generated structure
};

// Data-driven structure placement service. One instance is a registry of
// structure definitions + a pure placement engine.
class IStructurePlacementSystem {
public:
    virtual ~IStructurePlacementSystem() = default;

    // ---- structure definitions (assets + sockets) ----
    // Registers a definition. False with a diagnostic for an empty/duplicate
    // id or an invalid structure asset (the same validation the structure
    // generator factory applies).
    virtual bool add_definition(const StructureDefinition& definition,
                                std::string& errorOut) = 0;
    virtual const StructureDefinition* definition(const std::string& id) const = 0;
    virtual std::vector<std::string> definition_ids() const = 0;

    // The spawn rules owned by the system (loaded via deserialize or set
    // programmatically). They are NOT used implicitly by try_place/plan_region
    // (those take rules as a parameter) EXCEPT when the caller passes an
    // empty vector, which falls back to the stored rules — so a JSON-only
    // project can author rules in the document and drive placement with
    // try_place({}, ...).
    virtual const std::vector<StructureSpawnRule>& rules() const = 0;
    // Replaces the stored rules. False with a diagnostic when any rule
    // references an unknown structureId (dangling data is refused, never
    // silently skipped) — the system state is unchanged on failure.
    virtual bool set_rules(const std::vector<StructureSpawnRule>& rules,
                           std::string& errorOut) = 0;

    // Decides whether a structure spawns at this column and, if so, generates
    // it. `surfaceHeight`/`biomeName` describe the column (the caller samples
    // its world/generator; biomeName "" = no biome info, so only rules
    // without a biome gate can match). The candidate cell is derived from the
    // column by the rule's spacing; the placement origin is the CELL ORIGIN
    // (so querying at the cell origin is canonical). Deterministic per
    // (rules, worldSeed, column). Returns false WITHOUT a diagnostic when no
    // rule matches (normal outcome); false WITH a diagnostic on a hard error
    // (unknown structureId, unregistered definition, generation failure). An
    // empty `rules` uses the system's stored rules (see rules()).
    virtual bool try_place(const std::vector<StructureSpawnRule>& rules,
                           int worldX, int worldZ, int surfaceHeight,
                           const std::string& biomeName, std::uint32_t worldSeed,
                           StructurePlacement& out, std::string& errorOut) const = 0;

    // Plans every structure the rules spawn inside a region of chunks
    // (16-block chunks, row-major chunk order; within a chunk, cells in
    // row-major order). Deterministic: same rules + seed + region + surfaces
    // always produce the same list. `surfaceAt(wx, wz)` / `biomeAt(wx, wz)`
    // are sampled at each candidate cell origin. An empty `rules` uses the
    // system's stored rules (see rules()).
    virtual bool plan_region(const std::vector<StructureSpawnRule>& rules,
                             int minChunkX, int minChunkZ,
                             int chunkCountX, int chunkCountZ,
                             const std::function<int(int, int)>& surfaceAt,
                             const std::function<std::string(int, int)>& biomeAt,
                             std::uint32_t worldSeed,
                             std::vector<StructurePlacement>& out,
                             std::string& errorOut) const = 0;

    // Resolves a placement's sockets to world space (local position + origin;
    // facing unchanged). False when the placement's structure is not a
    // registered definition.
    virtual bool resolve_sockets(const StructurePlacement& placement,
                                 std::vector<StructureSocket>& out,
                                 std::string& errorOut) const = 0;

    // Aligns placement `b` so its socket `socketB` meets placement `a`'s
    // `socketA`: the two socket positions coincide in world space AND their
    // facings oppose (they face each other). Returns the world origin b must
    // take (a is unmoved). False with a diagnostic when either socket is
    // unknown, the facings do not oppose, or the tag does not match.
    virtual bool connect_sockets(const StructurePlacement& a,
                                 const std::string& socketA,
                                 const StructurePlacement& b,
                                 const std::string& socketB,
                                 glm::ivec3& bOriginOut,
                                 std::string& errorOut) const = 0;

    // Versioned JSON: the definitions (spec + sockets) and the rules. The
    // document is {version, definitions, rules}; deserialize is all-or-nothing
    // (on failure the system keeps its previous state).
    virtual bool serialize(std::string& out) const = 0;
    virtual bool deserialize(const std::string& json, std::string& errorOut) = 0;
};

// Builds an empty system (definitions + rules loaded programmatically or via
// JSON).
std::shared_ptr<IStructurePlacementSystem> create_structure_placement_system();

// Builds a system from a versioned JSON document (definitions + rules).
// nullptr + diagnostic on malformed/unknown-version documents.
std::shared_ptr<IStructurePlacementSystem> create_structure_placement_system_from_json(
    const std::string& json, std::string& errorOut);

// Writes a placed structure into a voxel world through the world's
// TRANSACTIONAL path (the single mutation authority — META §11 / FALTANTES
// §7): every non-air block of `placement.output.blocks` (index
// x + z*width + y*width*height) is written at `origin + (x, y, z)` inside ONE
// transaction. The commit is all-or-nothing: an unregistered block id, an
// out-of-bounds cell or an unloaded chunk fails the WHOLE placement with a
// diagnostic (nothing partial is ever observable). Returns false with a
// diagnostic on a malformed output or a failed commit. Deterministic.
bool place_structure(engine::voxel::IVoxelWorld& world,
                     const StructurePlacement& placement,
                     std::string& errorOut);

}  // namespace procgen
}  // namespace engine
