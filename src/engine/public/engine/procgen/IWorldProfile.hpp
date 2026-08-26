#pragma once

// Public world-profile contract (META section 18 / FALTANTES item 14, item 16
// da ordem — "Permitir geração equivalente a Minecraft moderno sem recompilar
// a engine"). The individual generation domains are already data-driven
// (noise graph, climate/biome/surface, ores/carvers/decorators, structure
// assets, structure placement); this is the missing COMPOSITION: ONE versioned
// JSON document that assembles them all into the generator the world uses and
// the structure-placement system, so a project authors its world generation
// entirely as data — no per-world C++ assembly, no engine recompile.
//
// Document shape (all sections optional; a section present is validated
// all-or-nothing through its OWN subsystem parser — single source of truth):
//
//   {
//     "version": 1,
//     "height":    { <noise graph JSON> },   // 2D height field
//     "baseHeight": 131, "amplitude": 4,     // height = base + round(h*amp)
//     "climate": { "temperature": {...}, "moisture": {...},
//                  "continentalness": {...}, "erosion": {...},
//                  "weirdness": {...}, "river": {...} },   // axes optional
//     "biomes":   { <biome registry JSON> },
//     "caves":    { "density": { <noise graph JSON> }, "scale": 1.0, "offset": 0.0 },
//     "ores":     { "density": { <noise graph JSON> }, "scale": 1.0, "offset": 0.0,
//                   "table": { <ore table JSON> } },
//     "carver":     { <carver JSON> },
//     "decorators": { <decorator set JSON> },
//     "structures": { <structure placement document: definitions + rules> }
//   }
//
// generator() composes height + climate + biomes + surface + caves/ores +
// carver + decorators through the public IClimateVoxelGenerator factory; a
// missing section falls back to that factory's builtin default for the hook
// (null table = builtin behavior), so a minimal profile still yields a usable
// flat world. structure_placement() holds the structures section (empty system
// when absent). serialize() re-emits the canonical document (byte-identical
// round-trip). Self-contained: composes public SDK contracts only.

#include "engine/procgen/IStructurePlacement.hpp"
#include "engine/voxel/IVoxelWorld.hpp"

#include <memory>
#include <string>

namespace engine {
namespace procgen {

// A composed, data-driven world generator + structure placement system.
class IWorldProfile {
public:
    virtual ~IWorldProfile() = default;

    // The voxel generator the world should register (engine::voxel::IVoxelGenerator).
    // Composed from the profile sections via the public factories; never null
    // (a profile without height/biomes still yields the flat builtin default).
    virtual std::shared_ptr<engine::voxel::IVoxelGenerator> generator() const = 0;

    // The structure-placement system (definitions + spawn rules) loaded from
    // the "structures" section. Empty system when the section is absent.
    virtual std::shared_ptr<IStructurePlacementSystem> structure_placement() const = 0;

    // Canonical versioned document. deserialize(serialize(p)) is byte-identical.
    virtual bool serialize(std::string& out) const = 0;
};

// nullptr + diagnostic on malformed/unknown-version documents; every present
// section is validated all-or-nothing through its own subsystem parser.
std::shared_ptr<IWorldProfile> create_world_profile_from_json(
    const std::string& json, std::string& errorOut);

}  // namespace procgen
}  // namespace engine
