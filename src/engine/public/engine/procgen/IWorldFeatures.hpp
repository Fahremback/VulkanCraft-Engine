#pragma once

// Public world-feature contracts (META section 18 / FALTANTES item 14):
// data-driven ore distribution, carvers and surface decorators.
//
// The builtin generator's hardcoded pieces become DATA, in the same family as
// INoiseGraph/IClimateBiome:
//   - IOreTable ....... density/y -> vein block (the world's ore-field sample
//                      plus the builtin thresholds are replaced by rules).
//   - ICarver ......... what fills a carved cell (air, or a fluid at low y).
//   - IDecorator ...... a single surface feature (tree/plant/column/disk) with
//                      per-column probability, biome/height gates and block
//                      composition; IDecoratorSet applies a list of them.
//
// The world consumes these through three additive hooks on the public
// IVoxelGenerator (ore_block / carve_block / decorate_column); every hook
// defaults to the builtin behavior, so existing generators are unchanged.
// A data-driven generator (IClimateVoxelGenerator) composes the tables and
// overrides the hooks.
//
// Placement is deterministic: per-column decisions use a stable hash family
// (the same sin-based hash the builtin tree pass uses), keyed by world
// coordinates + a per-decorator salt. Same data + seed -> same placement,
// bit-identical between independently generated worlds in the same binary.

#include "engine/voxel/IVoxelWorld.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace procgen {

// ---- Ore distribution (veins) -------------------------------------------

// One data-driven vein rule: blockId is placed when the world's ore-field
// sample at height y falls in [minDensity, maxDensity) and y in [minY, maxY].
// Rules are evaluated in order; first match wins (0 = no rule matched).
struct OreRule {
    std::uint32_t blockId{ 0 };
    float minDensity{ 0.0f };
    float maxDensity{ 1.0f };
    int minY{ 0 };
    int maxY{ 0x7FFFFFFF };
};

class IOreTable {
public:
    virtual ~IOreTable() = default;
    virtual std::size_t rule_count() const = 0;
    virtual bool rule(std::size_t index, OreRule& out) const = 0;

    // First rule matching (density, y); 0 when no rule matches.
    virtual std::uint32_t ore_for(float density, int y) const = 0;

    virtual bool serialize(std::string& out) const = 0;
    virtual bool deserialize(const std::string& json, std::string& errorOut) = 0;
};

// ---- Carvers -------------------------------------------------------------

// Data-driven carver fill: carved cells (3D density above the carve
// threshold) become `fluidBlockId` at y <= fluidMaxY, otherwise Air (0).
struct CarverSpec {
    int fluidMaxY{ 0 };            // 0 = no fluid pool (pure air caves)
    std::uint32_t fluidBlockId{ 0 };
};

class ICarver {
public:
    virtual ~ICarver() = default;
    virtual const CarverSpec& spec() const = 0;
    // Block for a carved cell at height y (Air when no fluid applies).
    virtual std::uint32_t fill_block(int y) const = 0;
    virtual bool serialize(std::string& out) const = 0;
    virtual bool deserialize(const std::string& json, std::string& errorOut) = 0;
};

// ---- Decorators (surface features) --------------------------------------

// One data-driven feature. Per column, the decorator first checks the gates
// (biome when !anyBiome, surface height range, per-column probability via the
// stable hash), then places blocks from the surface upward. Type-specific
// params and blocks:
//
//   tree   : params=[trunkMin, trunkMax, leafRadius]  blocks=[wood, leaf]
//   plant  : params=[]                                 blocks=[block] (1 block)
//   column : params=[minHeight, maxHeight]             blocks=[block]
//   disk   : params=[radius]                           blocks=[block] (flat)
struct DecoratorSpec {
    std::string type;
    float density{ 1.0f };          // per-column placement probability [0,1]
    std::vector<float> params;
    std::vector<std::uint32_t> blocks;
    std::uint32_t salt{ 0 };        // deterministic placement seed offset
    bool anyBiome{ true };
    std::uint32_t biomeIndex{ 0 };  // required engine biome when !anyBiome
    int minHeight{ 0 };
    int maxHeight{ 0x7FFFFFFF };
};

class IDecorator {
public:
    virtual ~IDecorator() = default;
    virtual const DecoratorSpec& spec() const = 0;

    // Gates + deterministic placement for one column. Returns true when the
    // feature was actually placed (writer accepted at least one block).
    virtual bool place(const voxel::DecorationContext& ctx,
                       const voxel::BlockWriter& write) const = 0;

    virtual bool serialize(std::string& out) const = 0;
    virtual bool deserialize(const std::string& json, std::string& errorOut) = 0;
};

// Ordered list of decorators. apply() runs every decorator that passes its
// gates for the column (data-driven mode replaces the builtin tree pass).
class IDecoratorSet {
public:
    virtual ~IDecoratorSet() = default;
    virtual std::size_t decorator_count() const = 0;
    virtual void apply(const voxel::DecorationContext& ctx,
                       const voxel::BlockWriter& write) const = 0;
    virtual bool serialize(std::string& out) const = 0;
    virtual bool deserialize(const std::string& json, std::string& errorOut) = 0;
};

// Empty ore table (no veins; the builtin substitution stays).
std::shared_ptr<IOreTable> create_ore_table();
std::shared_ptr<IOreTable> create_ore_table_from_json(
    const std::string& json, std::string& errorOut);

std::shared_ptr<ICarver> create_carver(const CarverSpec& spec);
std::shared_ptr<ICarver> create_carver_from_json(
    const std::string& json, std::string& errorOut);

// nullptr + diagnostic on unknown type / malformed params / empty blocks.
std::shared_ptr<IDecorator> create_decorator(const DecoratorSpec& spec,
                                             std::string& errorOut);
std::shared_ptr<IDecorator> create_decorator_from_json(
    const std::string& json, std::string& errorOut);

// Empty decorator set; add via deserialize (all-or-nothing).
std::shared_ptr<IDecoratorSet> create_decorator_set();
std::shared_ptr<IDecoratorSet> create_decorator_set_from_json(
    const std::string& json, std::string& errorOut);

}  // namespace procgen
}  // namespace engine
