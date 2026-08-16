// WorldFeatures.cpp
//
// SDK adapter for engine/procgen/IWorldFeatures.hpp (META section 18 /
// FALTANTES item 14: decorators, features, carvers e distribuição de
// minérios data-driven). The public contract never leaks the backend; this TU
// implements the data tables with the internal JSON parser (RegistryJson.hpp)
// and the deterministic placement hash.
//
// Placement determinism: per-column decisions use the same sin-based hash
// family as the builtin tree pass (Chunk::generate_terrain), keyed by world
// coordinates + a per-decorator salt, so identical data + seed produce
// bit-identical placement between independently generated worlds in the same
// binary. Density semantics: hash < density places the feature.

#include "engine/procgen/IWorldFeatures.hpp"
#include "RegistryJson.hpp"

#include <cmath>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace engine {
namespace procgen {
namespace {

std::string float_str(float value) {
    // %.9g round-trips float32 exactly, so serialized assets rebuild
    // bit-identically.
    std::ostringstream ss;
    ss.precision(9);
    ss << value;
    return ss.str();
}

// Stable per-column hash in [0,1) — the builtin tree pass hash family.
float hash_01(float worldX, float worldZ, float salt) {
    const float value = std::sin(worldX * 12.9898f + worldZ * 78.233f +
                                 salt * 31.719f) *
                        43758.5453f;
    return value - std::floor(value);
}

// ---- OreTable ------------------------------------------------------------

class OreTable final : public IOreTable {
public:
    explicit OreTable(std::vector<OreRule> rules) : rules_(std::move(rules)) {}

    std::size_t rule_count() const override { return rules_.size(); }
    bool rule(std::size_t index, OreRule& out) const override {
        if (index >= rules_.size()) return false;
        out = rules_[index];
        return true;
    }

    std::uint32_t ore_for(float density, int y) const override {
        for (const OreRule& rule : rules_) {
            if (density >= rule.minDensity && density < rule.maxDensity &&
                y >= rule.minY && y <= rule.maxY) {
                return rule.blockId;
            }
        }
        return 0;
    }

    bool serialize(std::string& out) const override {
        std::ostringstream ss;
        ss << "{\"version\":1,\"rules\":[";
        for (std::size_t i = 0; i < rules_.size(); ++i) {
            if (i != 0) ss << ',';
            ss << "{\"blockId\":" << rules_[i].blockId
               << ",\"minDensity\":" << float_str(rules_[i].minDensity)
               << ",\"maxDensity\":" << float_str(rules_[i].maxDensity)
               << ",\"minY\":" << rules_[i].minY
               << ",\"maxY\":" << rules_[i].maxY << '}';
        }
        ss << "]}";
        out = ss.str();
        return true;
    }

    bool deserialize(const std::string& json, std::string& errorOut) override {
        sdk::JsonValue document;
        if (!sdk::json_parse(json, document, errorOut)) {
            errorOut = "ore table: malformed asset - " + errorOut;
            return false;
        }
        if (!document.is_object()) {
            errorOut = "ore table: asset must be a JSON object";
            return false;
        }
        const sdk::JsonValue* version = document.field("version");
        if (version == nullptr || version->kind != sdk::JsonValue::Kind::Number ||
            static_cast<int>(version->number) != 1) {
            errorOut = "ore table: unsupported asset version";
            return false;
        }
        const sdk::JsonValue* rules = document.field("rules");
        if (rules == nullptr || !rules->is_array()) {
            errorOut = "ore table: asset has no \"rules\" array";
            return false;
        }
        std::vector<OreRule> parsed;
        for (std::size_t i = 0; i < rules->array.size(); ++i) {
            const sdk::JsonValue& entry = rules->array[i];
            if (!entry.is_object()) {
                errorOut = "ore table: rule " + std::to_string(i) +
                           " must be an object";
                return false;
            }
            OreRule rule;
            rule.blockId = static_cast<std::uint32_t>(
                sdk::json_number(entry, "blockId", 0.0));
            rule.minDensity = static_cast<float>(
                sdk::json_number(entry, "minDensity", 0.0));
            rule.maxDensity = static_cast<float>(
                sdk::json_number(entry, "maxDensity", 1.0));
            rule.minY = static_cast<int>(sdk::json_number(entry, "minY", 0.0));
            rule.maxY = static_cast<int>(
                sdk::json_number(entry, "maxY", 2147483647.0));
            if (rule.blockId == 0 || rule.minDensity > rule.maxDensity ||
                rule.minY > rule.maxY) {
                errorOut = "ore table: rule " + std::to_string(i) +
                           " has invalid bounds or blockId";
                return false;
            }
            parsed.push_back(rule);
        }
        // All-or-nothing.
        rules_ = std::move(parsed);
        return true;
    }

private:
    std::vector<OreRule> rules_;
};

// ---- Carver --------------------------------------------------------------

class Carver final : public ICarver {
public:
    explicit Carver(CarverSpec spec) : spec_(spec) {}

    const CarverSpec& spec() const override { return spec_; }

    std::uint32_t fill_block(int y) const override {
        if (spec_.fluidBlockId != 0 && y <= spec_.fluidMaxY) {
            return spec_.fluidBlockId;
        }
        return 0;  // Air
    }

    bool serialize(std::string& out) const override {
        std::ostringstream ss;
        ss << "{\"version\":1,\"fluidMaxY\":" << spec_.fluidMaxY
           << ",\"fluidBlockId\":" << spec_.fluidBlockId << '}';
        out = ss.str();
        return true;
    }

    bool deserialize(const std::string& json, std::string& errorOut) override {
        sdk::JsonValue document;
        if (!sdk::json_parse(json, document, errorOut)) {
            errorOut = "carver: malformed asset - " + errorOut;
            return false;
        }
        if (!document.is_object()) {
            errorOut = "carver: asset must be a JSON object";
            return false;
        }
        const sdk::JsonValue* version = document.field("version");
        if (version == nullptr || version->kind != sdk::JsonValue::Kind::Number ||
            static_cast<int>(version->number) != 1) {
            errorOut = "carver: unsupported asset version";
            return false;
        }
        CarverSpec parsed;
        parsed.fluidMaxY =
            static_cast<int>(sdk::json_number(document, "fluidMaxY", 0.0));
        parsed.fluidBlockId = static_cast<std::uint32_t>(
            sdk::json_number(document, "fluidBlockId", 0.0));
        if (parsed.fluidMaxY < 0) {
            errorOut = "carver: fluidMaxY must be non-negative";
            return false;
        }
        spec_ = parsed;  // all-or-nothing: only assigned after validation
        return true;
    }

private:
    CarverSpec spec_;
};

// ---- Decorator -----------------------------------------------------------

bool valid_decorator_spec(const DecoratorSpec& spec, std::string& errorOut) {
    if (spec.type != "tree" && spec.type != "plant" && spec.type != "column" &&
        spec.type != "disk") {
        errorOut = "decorator: unknown type '" + spec.type + "'";
        return false;
    }
    if (spec.density < 0.0f || spec.density > 1.0f) {
        errorOut = "decorator: density must be in [0,1]";
        return false;
    }
    if (spec.minHeight > spec.maxHeight) {
        errorOut = "decorator: minHeight > maxHeight";
        return false;
    }
    if (spec.blocks.empty()) {
        errorOut = "decorator: no blocks configured";
        return false;
    }
    if (spec.type == "tree") {
        if (spec.params.size() < 3) {
            errorOut = "decorator: tree expects params=[trunkMin, trunkMax, "
                       "leafRadius]";
            return false;
        }
        if (spec.blocks.size() < 2) {
            errorOut = "decorator: tree expects blocks=[wood, leaf]";
            return false;
        }
        if (spec.params[0] > spec.params[1]) {
            errorOut = "decorator: tree trunkMin > trunkMax";
            return false;
        }
    } else if (spec.type == "column") {
        if (spec.params.size() < 2) {
            errorOut = "decorator: column expects params=[minHeight, maxHeight]";
            return false;
        }
        if (spec.params[0] > spec.params[1]) {
            errorOut = "decorator: column minHeight > maxHeight";
            return false;
        }
    } else if (spec.type == "disk" && spec.params.size() < 1) {
        errorOut = "decorator: disk expects params=[radius]";
        return false;
    }
    return true;
}

DecoratorSpec parse_decorator_spec(const sdk::JsonValue& document,
                                   std::string& errorOut) {
    DecoratorSpec spec;
    spec.type = sdk::json_string(document, "type", "");
    spec.density = static_cast<float>(sdk::json_number(document, "density", 1.0));
    for (const double value : sdk::json_number_array(document, "params")) {
        spec.params.push_back(static_cast<float>(value));
    }
    for (const double value : sdk::json_number_array(document, "blocks")) {
        spec.blocks.push_back(static_cast<std::uint32_t>(value));
    }
    spec.salt = static_cast<std::uint32_t>(sdk::json_number(document, "salt", 0.0));
    spec.anyBiome = sdk::json_bool(document, "anyBiome", true);
    spec.biomeIndex = static_cast<std::uint32_t>(
        sdk::json_number(document, "biomeIndex", 0.0));
    spec.minHeight = static_cast<int>(sdk::json_number(document, "minHeight", 0.0));
    spec.maxHeight = static_cast<int>(
        sdk::json_number(document, "maxHeight", 2147483647.0));
    return spec;
}

std::string spec_json(const DecoratorSpec& spec) {
    std::ostringstream ss;
    ss << "{\"version\":1,\"type\":\"" << spec.type
       << "\",\"density\":" << float_str(spec.density) << ",\"params\":[";
    for (std::size_t i = 0; i < spec.params.size(); ++i) {
        if (i != 0) ss << ',';
        ss << float_str(spec.params[i]);
    }
    ss << "],\"blocks\":[";
    for (std::size_t i = 0; i < spec.blocks.size(); ++i) {
        if (i != 0) ss << ',';
        ss << spec.blocks[i];
    }
    ss << "],\"salt\":" << spec.salt
       << ",\"anyBiome\":" << (spec.anyBiome ? "true" : "false")
       << ",\"biomeIndex\":" << spec.biomeIndex
       << ",\"minHeight\":" << spec.minHeight
       << ",\"maxHeight\":" << spec.maxHeight << '}';
    return ss.str();
}

class Decorator final : public IDecorator {
public:
    explicit Decorator(DecoratorSpec spec) : spec_(std::move(spec)) {}

    const DecoratorSpec& spec() const override { return spec_; }

    bool place(const voxel::DecorationContext& ctx,
               const voxel::BlockWriter& write) const override {
        // Gates.
        if (!spec_.anyBiome && spec_.biomeIndex != ctx.biomeIndex) return false;
        if (ctx.surfaceHeight < spec_.minHeight ||
            ctx.surfaceHeight > spec_.maxHeight) {
            return false;
        }
        const float roll = hash_01(ctx.worldX, ctx.worldZ,
                                   static_cast<float>(spec_.salt) * 1.7f + 0.37f);
        if (roll >= spec_.density) return false;

        const int surface = ctx.surfaceHeight;
        bool placed = false;
        const float h1 = hash_01(ctx.worldX, ctx.worldZ,
                                 static_cast<float>(spec_.salt) * 1.7f + 1.0f);
        if (spec_.type == "tree") {
            const int trunkMin = static_cast<int>(spec_.params[0]);
            const int trunkMax = static_cast<int>(spec_.params[1]);
            const int leafRadius = static_cast<int>(spec_.params[2]);
            const int trunkHeight =
                trunkMin + static_cast<int>(h1 * static_cast<float>(
                               trunkMax - trunkMin + 1));
            const std::uint32_t wood = spec_.blocks[0];
            const std::uint32_t leaf = spec_.blocks[1];
            for (int ty = 1; ty <= trunkHeight; ++ty) {
                if (write(ctx.localX, surface + ty, ctx.localZ, wood)) {
                    placed = true;
                }
            }
            const int crownCenterY = surface + trunkHeight - 1;
            const float radiusSq = static_cast<float>(leafRadius * leafRadius);
            for (int ly = -2; ly <= 2; ++ly) {
                for (int lx = -leafRadius; lx <= leafRadius; ++lx) {
                    for (int lz = -leafRadius; lz <= leafRadius; ++lz) {
                        const float ellipsoid =
                            static_cast<float>(lx * lx + lz * lz) / radiusSq +
                            static_cast<float>(ly * ly) / 4.0f;
                        const float irregularity =
                            (hash_01(ctx.worldX + static_cast<float>(lx),
                                     ctx.worldZ + static_cast<float>(lz),
                                     static_cast<float>(ly + 8)) -
                             0.5f) *
                            0.32f;
                        if (ellipsoid <= 1.0f + irregularity) {
                            if (write(ctx.localX + lx, crownCenterY + ly,
                                      ctx.localZ + lz, leaf)) {
                                placed = true;
                            }
                        }
                    }
                }
            }
        } else if (spec_.type == "column") {
            const int minH = static_cast<int>(spec_.params[0]);
            const int maxH = static_cast<int>(spec_.params[1]);
            const int height = minH + static_cast<int>(
                                          h1 * static_cast<float>(maxH - minH + 1));
            for (int ty = 1; ty <= height; ++ty) {
                if (write(ctx.localX, surface + ty, ctx.localZ, spec_.blocks[0])) {
                    placed = true;
                }
            }
        } else if (spec_.type == "disk") {
            const int radius = static_cast<int>(spec_.params[0]);
            for (int dx = -radius; dx <= radius; ++dx) {
                for (int dz = -radius; dz <= radius; ++dz) {
                    if (dx * dx + dz * dz <= radius * radius) {
                        if (write(ctx.localX + dx, surface, ctx.localZ + dz,
                                  spec_.blocks[0])) {
                            placed = true;
                        }
                    }
                }
            }
        } else {  // plant
            if (write(ctx.localX, surface + 1, ctx.localZ, spec_.blocks[0])) {
                placed = true;
            }
        }
        return placed;
    }

    bool serialize(std::string& out) const override {
        out = spec_json(spec_);
        return true;
    }

    bool deserialize(const std::string& json, std::string& errorOut) override {
        sdk::JsonValue document;
        if (!sdk::json_parse(json, document, errorOut)) {
            errorOut = "decorator: malformed asset - " + errorOut;
            return false;
        }
        if (!document.is_object()) {
            errorOut = "decorator: asset must be a JSON object";
            return false;
        }
        const sdk::JsonValue* version = document.field("version");
        if (version == nullptr || version->kind != sdk::JsonValue::Kind::Number ||
            static_cast<int>(version->number) != 1) {
            errorOut = "decorator: unsupported asset version";
            return false;
        }
        DecoratorSpec parsed = parse_decorator_spec(document, errorOut);
        if (!valid_decorator_spec(parsed, errorOut)) return false;
        spec_ = std::move(parsed);  // all-or-nothing
        return true;
    }

private:
    DecoratorSpec spec_;
};

class DecoratorSet final : public IDecoratorSet {
public:
    explicit DecoratorSet(std::vector<std::shared_ptr<IDecorator>> decorators)
        : decorators_(std::move(decorators)) {}

    std::size_t decorator_count() const override { return decorators_.size(); }

    void apply(const voxel::DecorationContext& ctx,
               const voxel::BlockWriter& write) const override {
        for (const std::shared_ptr<IDecorator>& decorator : decorators_) {
            (void)decorator->place(ctx, write);
        }
    }

    bool serialize(std::string& out) const override {
        std::ostringstream ss;
        ss << "{\"version\":1,\"decorators\":[";
        for (std::size_t i = 0; i < decorators_.size(); ++i) {
            if (i != 0) ss << ',';
            ss << spec_json(decorators_[i]->spec());
        }
        ss << "]}";
        out = ss.str();
        return true;
    }

    bool deserialize(const std::string& json, std::string& errorOut) override {
        sdk::JsonValue document;
        if (!sdk::json_parse(json, document, errorOut)) {
            errorOut = "decorator set: malformed asset - " + errorOut;
            return false;
        }
        if (!document.is_object()) {
            errorOut = "decorator set: asset must be a JSON object";
            return false;
        }
        const sdk::JsonValue* version = document.field("version");
        if (version == nullptr || version->kind != sdk::JsonValue::Kind::Number ||
            static_cast<int>(version->number) != 1) {
            errorOut = "decorator set: unsupported asset version";
            return false;
        }
        const sdk::JsonValue* decorators = document.field("decorators");
        if (decorators == nullptr || !decorators->is_array()) {
            errorOut = "decorator set: asset has no \"decorators\" array";
            return false;
        }
        std::vector<std::shared_ptr<IDecorator>> parsed;
        for (std::size_t i = 0; i < decorators->array.size(); ++i) {
            const sdk::JsonValue& entry = decorators->array[i];
            if (!entry.is_object()) {
                errorOut = "decorator set: entry " + std::to_string(i) +
                           " must be an object";
                return false;
            }
            DecoratorSpec spec = parse_decorator_spec(entry, errorOut);
            if (!valid_decorator_spec(spec, errorOut)) return false;
            parsed.push_back(std::make_shared<Decorator>(std::move(spec)));
        }
        decorators_ = std::move(parsed);  // all-or-nothing
        return true;
    }

private:
    std::vector<std::shared_ptr<IDecorator>> decorators_;
};

}  // namespace

std::shared_ptr<IOreTable> create_ore_table() {
    return std::make_shared<OreTable>(std::vector<OreRule>{});
}

std::shared_ptr<IOreTable> create_ore_table_from_json(
    const std::string& json, std::string& errorOut) {
    errorOut.clear();
    auto table = std::make_shared<OreTable>(std::vector<OreRule>{});
    if (!table->deserialize(json, errorOut)) return nullptr;
    return table;
}

std::shared_ptr<ICarver> create_carver(const CarverSpec& spec) {
    return std::make_shared<Carver>(spec);
}

std::shared_ptr<ICarver> create_carver_from_json(const std::string& json,
                                                 std::string& errorOut) {
    errorOut.clear();
    auto carver = std::make_shared<Carver>(CarverSpec{});
    if (!carver->deserialize(json, errorOut)) return nullptr;
    return carver;
}

std::shared_ptr<IDecorator> create_decorator(const DecoratorSpec& spec,
                                             std::string& errorOut) {
    errorOut.clear();
    if (!valid_decorator_spec(spec, errorOut)) return nullptr;
    return std::make_shared<Decorator>(spec);
}

std::shared_ptr<IDecorator> create_decorator_from_json(
    const std::string& json, std::string& errorOut) {
    errorOut.clear();
    auto decorator = std::make_shared<Decorator>(DecoratorSpec{});
    if (!decorator->deserialize(json, errorOut)) return nullptr;
    return decorator;
}

std::shared_ptr<IDecoratorSet> create_decorator_set() {
    return std::make_shared<DecoratorSet>(std::vector<std::shared_ptr<IDecorator>>{});
}

std::shared_ptr<IDecoratorSet> create_decorator_set_from_json(
    const std::string& json, std::string& errorOut) {
    errorOut.clear();
    auto set = std::make_shared<DecoratorSet>(
        std::vector<std::shared_ptr<IDecorator>>{});
    if (!set->deserialize(json, errorOut)) return nullptr;
    return set;
}

}  // namespace procgen
}  // namespace engine
