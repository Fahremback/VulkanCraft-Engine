// ClimateBiome.cpp
//
// SDK adapter for engine/procgen/IClimateBiome.hpp (META section 18 /
// FALTANTES item 14: climate graph, biome registry and surface rules
// data-driven). The public contract never leaks the backend; this TU composes
// the public noise graph (engine/procgen/INoiseGraph.hpp, implemented by
// FastNoiseGraph.cpp) and the internal JSON parser (RegistryJson.hpp).
//
// Semantics:
//  - ClimateSampler: one graph per climate axis; null axis -> 0.0f. The asset
//    embeds each graph's own serialized document as a JSON string (or null),
//    and deserialize restores the graphs through their own validation, so a
//    broken embedded graph rejects the whole asset (all-or-nothing).
//  - BiomeRegistry: ordered definitions, first match wins with inclusive-min /
//    exclusive-max bounds; serializes to a versioned JSON asset.
//  - SurfaceResolver: per-biome rules, first match wins by depth/height/slope;
//    0 means "keep the engine builtin surface for the mapped engine biome".
//  - ClimateVoxelGenerator: height from a graph (baseHeight + round(h*amp)),
//    slope from neighbor height samples, climate from the sampler, biome index
//    from the registry, surface blocks from the resolver (via the world's
//    IVoxelGenerator::surface_block hook), optional 3D caves/ores.

#include "engine/procgen/IClimateBiome.hpp"
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

bool in_bounds(float value, float minValue, float maxValue) {
    return value >= minValue && value < maxValue;
}

bool rule_matches(const SurfaceRule& rule, int height, int depth, float slope) {
    return depth >= rule.minDepth && depth <= rule.maxDepth &&
           height >= rule.minHeight && height <= rule.maxHeight &&
           slope >= rule.minSlope;
}

// ---- ClimateSampler ------------------------------------------------------

class ClimateSampler final : public IClimateSampler {
public:
    ClimateSampler(std::shared_ptr<INoiseGraph> temperature,
                   std::shared_ptr<INoiseGraph> moisture,
                   std::shared_ptr<INoiseGraph> continentalness,
                   std::shared_ptr<INoiseGraph> erosion,
                   std::shared_ptr<INoiseGraph> weirdness,
                   std::shared_ptr<INoiseGraph> river)
        : temperature_(std::move(temperature)),
          moisture_(std::move(moisture)),
          continentalness_(std::move(continentalness)),
          erosion_(std::move(erosion)),
          weirdness_(std::move(weirdness)),
          river_(std::move(river)) {}

    ClimatePoint sample(float worldX, float worldZ) const override {
        ClimatePoint point;
        point.temperature = temperature_ ? temperature_->sample_2d(worldX, worldZ) : 0.0f;
        point.moisture = moisture_ ? moisture_->sample_2d(worldX, worldZ) : 0.0f;
        point.continentalness =
            continentalness_ ? continentalness_->sample_2d(worldX, worldZ) : 0.0f;
        point.erosion = erosion_ ? erosion_->sample_2d(worldX, worldZ) : 0.0f;
        point.weirdness = weirdness_ ? weirdness_->sample_2d(worldX, worldZ) : 0.0f;
        point.river = river_ ? river_->sample_2d(worldX, worldZ) : 0.0f;
        return point;
    }

    bool serialize(std::string& out) const override {
        std::ostringstream ss;
        ss << "{\"version\":1,\"graphs\":{";
        ss << "\"temperature\":" << graph_json(temperature_) << ',';
        ss << "\"moisture\":" << graph_json(moisture_) << ',';
        ss << "\"continentalness\":" << graph_json(continentalness_) << ',';
        ss << "\"erosion\":" << graph_json(erosion_) << ',';
        ss << "\"weirdness\":" << graph_json(weirdness_) << ',';
        ss << "\"river\":" << graph_json(river_);
        ss << "}}";
        out = ss.str();
        return true;
    }

    bool deserialize(const std::string& json, std::string& errorOut) override {
        sdk::JsonValue document;
        if (!sdk::json_parse(json, document, errorOut)) {
            errorOut = "climate sampler: malformed asset - " + errorOut;
            return false;
        }
        if (!document.is_object()) {
            errorOut = "climate sampler: asset must be a JSON object";
            return false;
        }
        const sdk::JsonValue* version = document.field("version");
        if (version == nullptr || version->kind != sdk::JsonValue::Kind::Number ||
            static_cast<int>(version->number) != 1) {
            errorOut = "climate sampler: unsupported asset version";
            return false;
        }
        const sdk::JsonValue* graphs = document.field("graphs");
        if (graphs == nullptr || !graphs->is_object()) {
            errorOut = "climate sampler: asset has no \"graphs\" object";
            return false;
        }
        const char* names[] = { "temperature", "moisture", "continentalness",
                                "erosion", "weirdness", "river" };
        std::shared_ptr<INoiseGraph> parsed[6];
        for (int i = 0; i < 6; ++i) {
            const sdk::JsonValue* value = graphs->field(names[i]);
            if (value == nullptr) {
                errorOut = std::string("climate sampler: missing graph \"") +
                           names[i] + "\"";
                return false;
            }
            if (value->kind == sdk::JsonValue::Kind::Null) continue;  // null axis
            if (!value->is_string()) {
                errorOut = std::string("climate sampler: graph \"") + names[i] +
                           "\" must be a JSON document string or null";
                return false;
            }
            // Restore through the graph's own validation: parse the embedded
            // document into a scratch graph first, then commit.
            NoiseGraphSpec scratch;
            scratch.nodes.push_back({ "constant", { 0.0f }, {} });
            std::string scratchError;
            auto graph = create_noise_graph_from_spec(scratch, scratchError);
            if (!graph) {
                errorOut = "climate sampler: internal scratch graph failed";
                return false;
            }
            if (!graph->deserialize(value->string, errorOut)) {
                errorOut = std::string("climate sampler: \"") + names[i] +
                           "\" graph - " + errorOut;
                return false;
            }
            parsed[i] = std::move(graph);
        }
        // All-or-nothing commit.
        temperature_ = parsed[0];
        moisture_ = parsed[1];
        continentalness_ = parsed[2];
        erosion_ = parsed[3];
        weirdness_ = parsed[4];
        river_ = parsed[5];
        return true;
    }

private:
    static std::string graph_json(const std::shared_ptr<INoiseGraph>& graph) {
        if (!graph) return "null";
        std::string text;
        graph->serialize(text);
        std::string escaped;
        escaped.reserve(text.size() + 8);
        escaped += '"';
        for (const char c : text) {
            if (c == '"' || c == '\\') escaped += '\\';
            escaped += c;
        }
        escaped += '"';
        return escaped;
    }

    std::shared_ptr<INoiseGraph> temperature_;
    std::shared_ptr<INoiseGraph> moisture_;
    std::shared_ptr<INoiseGraph> continentalness_;
    std::shared_ptr<INoiseGraph> erosion_;
    std::shared_ptr<INoiseGraph> weirdness_;
    std::shared_ptr<INoiseGraph> river_;
};

// ---- BiomeRegistry -------------------------------------------------------

// Compact engine-faithful climate table. Order is priority: ocean by
// continentalness, rivers, cold -> glacial/alpine, hot+dry -> desert/savanna/
// badlands, hot+wet -> jungle/swamp, then meadow as the catch-all last entry.
std::vector<BiomeDefinition> default_biomes() {
    std::vector<BiomeDefinition> biomes;
    auto add = [&biomes](const char* name, std::uint8_t engineIndex,
                         const ClimateBounds& bounds) {
        BiomeDefinition def;
        def.name = name;
        def.engineBiomeIndex = engineIndex;
        def.climate = bounds;
        biomes.push_back(std::move(def));
    };
    ClimateBounds full;
    add("deep_ocean", 0, ClimateBounds{ full.minTemperature, full.maxTemperature,
                                        full.minMoisture, full.maxMoisture,
                                        -1.0f, -0.55f,
                                        full.minErosion, full.maxErosion,
                                        full.minWeirdness, full.maxWeirdness,
                                        full.minRiver, full.maxRiver });
    add("ocean", 1, ClimateBounds{ full.minTemperature, full.maxTemperature,
                                   full.minMoisture, full.maxMoisture,
                                   -0.55f, -0.25f,
                                   full.minErosion, full.maxErosion,
                                   full.minWeirdness, full.maxWeirdness,
                                   full.minRiver, full.maxRiver });
    add("coast", 2, ClimateBounds{ full.minTemperature, full.maxTemperature,
                                   full.minMoisture, full.maxMoisture,
                                   -0.25f, 0.0f,
                                   full.minErosion, full.maxErosion,
                                   full.minWeirdness, full.maxWeirdness,
                                   full.minRiver, full.maxRiver });
    add("river", 3, ClimateBounds{ full.minTemperature, full.maxTemperature,
                                   full.minMoisture, full.maxMoisture,
                                   full.minContinentalness, full.maxContinentalness,
                                   full.minErosion, full.maxErosion,
                                   full.minWeirdness, full.maxWeirdness,
                                   0.35f, 1.0f });
    add("glacial", 16, ClimateBounds{ -1.0f, -0.35f,
                                      full.minMoisture, full.maxMoisture,
                                      full.minContinentalness, full.maxContinentalness,
                                      full.minErosion, full.maxErosion,
                                      full.minWeirdness, full.maxWeirdness,
                                      full.minRiver, full.maxRiver });
    add("alpine", 15, ClimateBounds{ -0.35f, -0.15f,
                                     full.minMoisture, full.maxMoisture,
                                     full.minContinentalness, full.maxContinentalness,
                                     full.minErosion, full.maxErosion,
                                     full.minWeirdness, full.maxWeirdness,
                                     full.minRiver, full.maxRiver });
    add("desert", 8, ClimateBounds{ 0.25f, 1.0f,
                                    -1.0f, -0.20f,
                                    full.minContinentalness, full.maxContinentalness,
                                    full.minErosion, full.maxErosion,
                                    full.minWeirdness, full.maxWeirdness,
                                    full.minRiver, full.maxRiver });
    add("savanna", 11, ClimateBounds{ 0.15f, 1.0f,
                                      -0.20f, 0.05f,
                                      full.minContinentalness, full.maxContinentalness,
                                      full.minErosion, full.maxErosion,
                                      full.minWeirdness, full.maxWeirdness,
                                      full.minRiver, full.maxRiver });
    add("badlands", 10, ClimateBounds{ 0.35f, 1.0f,
                                       -1.0f, -0.35f,
                                       full.minContinentalness, full.maxContinentalness,
                                       full.minErosion, full.maxErosion,
                                       full.minWeirdness, full.maxWeirdness,
                                       full.minRiver, full.maxRiver });
    add("jungle", 13, ClimateBounds{ 0.25f, 1.0f,
                                     0.45f, 1.0f,
                                     full.minContinentalness, full.maxContinentalness,
                                     full.minErosion, full.maxErosion,
                                     full.minWeirdness, full.maxWeirdness,
                                     full.minRiver, full.maxRiver });
    add("swamp", 12, ClimateBounds{ 0.0f, 0.35f,
                                    0.45f, 1.0f,
                                    full.minContinentalness, full.maxContinentalness,
                                    full.minErosion, full.maxErosion,
                                    full.minWeirdness, full.maxWeirdness,
                                    full.minRiver, full.maxRiver });
    add("birch_taiga", 6, ClimateBounds{ -0.15f, 0.10f,
                                         0.15f, 1.0f,
                                         full.minContinentalness, full.maxContinentalness,
                                         full.minErosion, full.maxErosion,
                                         full.minWeirdness, full.maxWeirdness,
                                         full.minRiver, full.maxRiver });
    add("forest", 5, ClimateBounds{ 0.0f, 0.30f,
                                    0.05f, 0.45f,
                                    full.minContinentalness, full.maxContinentalness,
                                    full.minErosion, full.maxErosion,
                                    full.minWeirdness, full.maxWeirdness,
                                    full.minRiver, full.maxRiver });
    // Catch-all: every climate not matched above lands on meadow.
    add("meadow", 7, full);
    return biomes;
}

bool valid_bounds(const ClimateBounds& bounds) {
    return bounds.minTemperature <= bounds.maxTemperature &&
           bounds.minMoisture <= bounds.maxMoisture &&
           bounds.minContinentalness <= bounds.maxContinentalness &&
           bounds.minErosion <= bounds.maxErosion &&
           bounds.minWeirdness <= bounds.maxWeirdness &&
           bounds.minRiver <= bounds.maxRiver;
}

bool valid_rule(const SurfaceRule& rule, std::string& errorOut) {
    if (rule.blockId == 0) {
        errorOut = "biome registry: surface rule blockId must be non-zero";
        return false;
    }
    if (rule.minDepth > rule.maxDepth || rule.minHeight > rule.maxHeight ||
        rule.minSlope < 0.0f) {
        errorOut = "biome registry: surface rule has inverted/invalid bounds";
        return false;
    }
    return true;
}

// Builds definitions from a parsed JSON document. Returns false + diagnostic
// on any malformed entry (no partial result is produced).
bool parse_biomes(const sdk::JsonValue& document, std::vector<BiomeDefinition>& out,
                  std::string& errorOut) {
    if (!document.is_object()) {
        errorOut = "biome registry: asset must be a JSON object";
        return false;
    }
    const sdk::JsonValue* version = document.field("version");
    if (version == nullptr || version->kind != sdk::JsonValue::Kind::Number ||
        static_cast<int>(version->number) != 1) {
        errorOut = "biome registry: unsupported asset version";
        return false;
    }
    const sdk::JsonValue* biomes = document.field("biomes");
    if (biomes == nullptr || !biomes->is_array()) {
        errorOut = "biome registry: asset has no \"biomes\" array";
        return false;
    }
    out.clear();
    for (std::size_t i = 0; i < biomes->array.size(); ++i) {
        const sdk::JsonValue& entry = biomes->array[i];
        if (!entry.is_object()) {
            errorOut = "biome registry: entry " + std::to_string(i) +
                       " must be an object";
            return false;
        }
        BiomeDefinition def;
        def.name = sdk::json_string(entry, "name", "");
        if (def.name.empty()) {
            errorOut = "biome registry: entry " + std::to_string(i) +
                       " has an empty name";
            return false;
        }
        const double engineIndex = sdk::json_number(entry, "engineBiomeIndex", -1.0);
        if (engineIndex < 0.0 || engineIndex > 255.0) {
            errorOut = "biome registry: entry '" + def.name +
                       "' engineBiomeIndex out of range";
            return false;
        }
        def.engineBiomeIndex = static_cast<std::uint8_t>(engineIndex);

        const sdk::JsonValue* climate = entry.field("climate");
        if (climate != nullptr) {
            if (!climate->is_object()) {
                errorOut = "biome registry: entry '" + def.name +
                           "' climate must be an object";
                return false;
            }
            const auto axis = [&](const char* key, float& minOut, float& maxOut) {
                const sdk::JsonValue* value = climate->field(key);
                if (value != nullptr) {
                    if (!value->is_array() || value->array.size() != 2 ||
                        value->array[0].kind != sdk::JsonValue::Kind::Number ||
                        value->array[1].kind != sdk::JsonValue::Kind::Number) {
                        errorOut = "biome registry: entry '" + def.name +
                                   "' climate '" + key +
                                   "' must be [min, max]";
                        return false;
                    }
                    minOut = static_cast<float>(value->array[0].number);
                    maxOut = static_cast<float>(value->array[1].number);
                }
                return true;
            };
            if (!axis("temperature", def.climate.minTemperature, def.climate.maxTemperature) ||
                !axis("moisture", def.climate.minMoisture, def.climate.maxMoisture) ||
                !axis("continentalness", def.climate.minContinentalness, def.climate.maxContinentalness) ||
                !axis("erosion", def.climate.minErosion, def.climate.maxErosion) ||
                !axis("weirdness", def.climate.minWeirdness, def.climate.maxWeirdness) ||
                !axis("river", def.climate.minRiver, def.climate.maxRiver)) {
                return false;
            }
        }
        if (!valid_bounds(def.climate)) {
            errorOut = "biome registry: entry '" + def.name +
                       "' has inverted climate bounds";
            return false;
        }

        const sdk::JsonValue* surface = entry.field("surface");
        if (surface != nullptr) {
            if (!surface->is_array()) {
                errorOut = "biome registry: entry '" + def.name +
                           "' surface must be an array";
                return false;
            }
            for (const sdk::JsonValue& ruleValue : surface->array) {
                if (!ruleValue.is_object()) {
                    errorOut = "biome registry: entry '" + def.name +
                               "' has a non-object surface rule";
                    return false;
                }
                SurfaceRule rule;
                rule.blockId = static_cast<std::uint32_t>(
                    sdk::json_number(ruleValue, "blockId", 0.0));
                rule.minDepth = static_cast<int>(sdk::json_number(ruleValue, "minDepth", 0.0));
                rule.maxDepth = static_cast<int>(sdk::json_number(ruleValue, "maxDepth", 2147483647.0));
                rule.minHeight = static_cast<int>(sdk::json_number(ruleValue, "minHeight", -2147483648.0));
                rule.maxHeight = static_cast<int>(sdk::json_number(ruleValue, "maxHeight", 2147483647.0));
                rule.minSlope = static_cast<float>(sdk::json_number(ruleValue, "minSlope", 0.0));
                if (!valid_rule(rule, errorOut)) return false;
                def.surface.push_back(rule);
            }
        }
        out.push_back(std::move(def));
    }
    if (out.empty()) {
        errorOut = "biome registry: asset defines no biomes";
        return false;
    }
    return true;
}

class BiomeRegistry final : public IBiomeRegistry {
public:
    explicit BiomeRegistry(std::vector<BiomeDefinition> biomes)
        : biomes_(std::move(biomes)) {}

    std::size_t biome_count() const override { return biomes_.size(); }

    bool biome_definition(std::size_t index, BiomeDefinition& out) const override {
        if (index >= biomes_.size()) return false;
        out = biomes_[index];
        return true;
    }

    bool biome_for(const ClimatePoint& climate,
                   std::uint32_t& outIndex) const override {
        for (std::size_t i = 0; i < biomes_.size(); ++i) {
            const BiomeDefinition& biome = biomes_[i];
            if (in_bounds(climate.temperature, biome.climate.minTemperature,
                          biome.climate.maxTemperature) &&
                in_bounds(climate.moisture, biome.climate.minMoisture,
                          biome.climate.maxMoisture) &&
                in_bounds(climate.continentalness, biome.climate.minContinentalness,
                          biome.climate.maxContinentalness) &&
                in_bounds(climate.erosion, biome.climate.minErosion,
                          biome.climate.maxErosion) &&
                in_bounds(climate.weirdness, biome.climate.minWeirdness,
                          biome.climate.maxWeirdness) &&
                in_bounds(climate.river, biome.climate.minRiver,
                          biome.climate.maxRiver)) {
                outIndex = static_cast<std::uint32_t>(i);
                return true;
            }
        }
        return false;
    }

    bool serialize(std::string& out) const override {
        std::ostringstream ss;
        ss << "{\"version\":1,\"biomes\":[";
        for (std::size_t i = 0; i < biomes_.size(); ++i) {
            if (i != 0) ss << ',';
            const BiomeDefinition& biome = biomes_[i];
            ss << "{\"name\":\"" << biome.name
               << "\",\"engineBiomeIndex\":" << static_cast<unsigned>(biome.engineBiomeIndex)
               << ",\"climate\":{"
               << "\"temperature\":[" << float_str(biome.climate.minTemperature) << ','
               << float_str(biome.climate.maxTemperature) << "],"
               << "\"moisture\":[" << float_str(biome.climate.minMoisture) << ','
               << float_str(biome.climate.maxMoisture) << "],"
               << "\"continentalness\":[" << float_str(biome.climate.minContinentalness) << ','
               << float_str(biome.climate.maxContinentalness) << "],"
               << "\"erosion\":[" << float_str(biome.climate.minErosion) << ','
               << float_str(biome.climate.maxErosion) << "],"
               << "\"weirdness\":[" << float_str(biome.climate.minWeirdness) << ','
               << float_str(biome.climate.maxWeirdness) << "],"
               << "\"river\":[" << float_str(biome.climate.minRiver) << ','
               << float_str(biome.climate.maxRiver) << "]},\"surface\":[";
            for (std::size_t r = 0; r < biome.surface.size(); ++r) {
                if (r != 0) ss << ',';
                const SurfaceRule& rule = biome.surface[r];
                ss << "{\"blockId\":" << rule.blockId
                   << ",\"minDepth\":" << rule.minDepth
                   << ",\"maxDepth\":" << rule.maxDepth
                   << ",\"minHeight\":" << rule.minHeight
                   << ",\"maxHeight\":" << rule.maxHeight
                   << ",\"minSlope\":" << float_str(rule.minSlope) << '}';
            }
            ss << "]}";
        }
        ss << "]}";
        out = ss.str();
        return true;
    }

    bool deserialize(const std::string& json, std::string& errorOut) override {
        sdk::JsonValue document;
        if (!sdk::json_parse(json, document, errorOut)) {
            errorOut = "biome registry: malformed asset - " + errorOut;
            return false;
        }
        std::vector<BiomeDefinition> parsed;
        if (!parse_biomes(document, parsed, errorOut)) return false;
        // All-or-nothing: on failure the registry keeps its previous state.
        biomes_ = std::move(parsed);
        return true;
    }

private:
    std::vector<BiomeDefinition> biomes_;
};

// ---- SurfaceResolver -----------------------------------------------------

class SurfaceResolver final : public ISurfaceResolver {
public:
    explicit SurfaceResolver(std::shared_ptr<const IBiomeRegistry> registry)
        : registry_(std::move(registry)) {}

    std::uint32_t block_for(std::uint32_t biomeIndex, const ClimatePoint& climate,
                            int height, int depth, float slope) const override {
        (void)climate;
        BiomeDefinition def;
        if (!registry_->biome_definition(biomeIndex, def)) return 0;
        for (const SurfaceRule& rule : def.surface) {
            if (rule_matches(rule, height, depth, slope)) return rule.blockId;
        }
        return 0;  // keep the engine builtin surface for the mapped engine biome
    }

private:
    std::shared_ptr<const IBiomeRegistry> registry_;
};

// ---- ClimateVoxelGenerator ----------------------------------------------

class ClimateVoxelGenerator final : public IClimateVoxelGenerator {
public:
    ClimateVoxelGenerator(std::shared_ptr<const INoiseGraph> height,
                          std::shared_ptr<const IClimateSampler> sampler,
                          std::shared_ptr<const IBiomeRegistry> registry,
                          std::shared_ptr<const ISurfaceResolver> resolver,
                          std::shared_ptr<const IDensityFunction> caves,
                          std::shared_ptr<const IDensityFunction> ores,
                          int baseHeight, int amplitude,
                          std::shared_ptr<const IOreTable> oreTable,
                          std::shared_ptr<const ICarver> carver,
                          std::shared_ptr<const IDecoratorSet> decoratorSet)
        : height_(std::move(height)),
          sampler_(std::move(sampler)),
          registry_(std::move(registry)),
          resolver_(std::move(resolver)),
          caves_(std::move(caves)),
          ores_(std::move(ores)),
          oreTable_(std::move(oreTable)),
          carver_(std::move(carver)),
          decoratorSet_(std::move(decoratorSet)),
          baseHeight_(baseHeight),
          amplitude_(amplitude) {}

    voxel::TerrainPoint sample(float worldX, float worldZ) const override {
        voxel::TerrainPoint point;
        const float h = height_ ? height_->sample_2d(worldX, worldZ) : 0.0f;
        point.height = baseHeight_ +
                       static_cast<int>(std::lround(h * static_cast<float>(amplitude_)));
        const ClimatePoint climate = climate_at(worldX, worldZ);
        point.temperature = climate.temperature;
        point.moisture = climate.moisture;
        point.continentalness = climate.continentalness;
        point.erosion = climate.erosion;
        point.weirdness = climate.weirdness;
        point.river = climate.river;
        point.slope = compute_slope(worldX, worldZ);
        point.biomeIndex = static_cast<std::uint8_t>(engine_biome_at(worldX, worldZ));
        return point;
    }

    ClimatePoint climate_at(float worldX, float worldZ) const override {
        return sampler_ ? sampler_->sample(worldX, worldZ) : ClimatePoint{};
    }

    std::uint32_t biome_at(float worldX, float worldZ) const override {
        std::uint32_t index = 0;
        if (!registry_ || !registry_->biome_for(climate_at(worldX, worldZ), index)) {
            return 0;
        }
        return index;
    }

    std::uint32_t engine_biome_at(float worldX, float worldZ) const override {
        BiomeDefinition def;
        if (!registry_ || !registry_->biome_definition(biome_at(worldX, worldZ), def)) {
            return 4;  // engine BiomeType::Plains — neutral fallback
        }
        return def.engineBiomeIndex;
    }

    float cave_density(float worldX, float worldY, float worldZ) const override {
        return caves_ ? caves_->sample(worldX, worldY, worldZ) : -1.0f;
    }

    float ore_density(float worldX, float worldY, float worldZ) const override {
        return ores_ ? ores_->sample(worldX, worldY, worldZ) : -1.0f;
    }

    std::uint32_t surface_block(const voxel::TerrainPoint& point,
                                int depth) const override {
        if (!registry_ || !resolver_) return 0;
        std::uint32_t biomeIndex = 0;
        const ClimatePoint climate{ point.temperature, point.moisture,
                                    point.continentalness, point.erosion,
                                    point.weirdness, point.river };
        if (!registry_->biome_for(climate, biomeIndex)) return 0;
        return resolver_->block_for(biomeIndex, climate, point.height, depth,
                                    point.slope);
    }

    // Data-driven ore veins: the table's first matching rule replaces the
    // builtin vein; 0 (no rule) keeps the builtin result.
    std::uint32_t ore_block(float oreDensity, int y,
                            std::uint32_t builtinBlock) const override {
        (void)builtinBlock;
        return oreTable_ ? oreTable_->ore_for(oreDensity, y) : 0;
    }

    // Data-driven carver fill: air caves, with an optional fluid pool at low
    // y (full data mode replaces the builtin lava/air choice).
    std::uint32_t carve_block(float caveDensity, int y, int depth,
                              std::uint32_t builtinCarve) const override {
        (void)caveDensity;
        (void)depth;
        return carver_ ? carver_->fill_block(y) : builtinCarve;
    }

    // Data-driven decoration: the decorator set replaces the builtin per-biome
    // tree pass for every land column (data mode).
    bool decorate_column(const voxel::DecorationContext& ctx,
                         const voxel::BlockWriter& write) const override {
        if (!decoratorSet_) return false;
        decoratorSet_->apply(ctx, write);
        return true;
    }

private:
    // Gradient magnitude of the height field (in blocks) over a 2-block
    // span; the engine's surface code treats slope > 4.2 as exposed cliff.
    float compute_slope(float worldX, float worldZ) const {
        if (!height_) return 0.0f;
        const float dx = (height_->sample_2d(worldX + 1.0f, worldZ) -
                          height_->sample_2d(worldX - 1.0f, worldZ)) *
                         static_cast<float>(amplitude_);
        const float dz = (height_->sample_2d(worldX, worldZ + 1.0f) -
                          height_->sample_2d(worldX, worldZ - 1.0f)) *
                         static_cast<float>(amplitude_);
        return std::sqrt(dx * dx + dz * dz);
    }

    std::shared_ptr<const INoiseGraph> height_;
    std::shared_ptr<const IClimateSampler> sampler_;
    std::shared_ptr<const IBiomeRegistry> registry_;
    std::shared_ptr<const ISurfaceResolver> resolver_;
    std::shared_ptr<const IDensityFunction> caves_;
    std::shared_ptr<const IDensityFunction> ores_;
    std::shared_ptr<const IOreTable> oreTable_;
    std::shared_ptr<const ICarver> carver_;
    std::shared_ptr<const IDecoratorSet> decoratorSet_;
    int baseHeight_;
    int amplitude_;
};

}  // namespace

std::shared_ptr<IBiomeRegistry> create_biome_registry() {
    return std::make_shared<BiomeRegistry>(default_biomes());
}

std::shared_ptr<IBiomeRegistry> create_biome_registry_from_json(
    const std::string& json, std::string& errorOut) {
    errorOut.clear();
    sdk::JsonValue document;
    if (!sdk::json_parse(json, document, errorOut)) {
        errorOut = "biome registry: malformed asset - " + errorOut;
        return nullptr;
    }
    std::vector<BiomeDefinition> biomes;
    if (!parse_biomes(document, biomes, errorOut)) return nullptr;
    return std::make_shared<BiomeRegistry>(std::move(biomes));
}

std::shared_ptr<IClimateSampler> create_climate_sampler(
    std::shared_ptr<INoiseGraph> temperature, std::shared_ptr<INoiseGraph> moisture,
    std::shared_ptr<INoiseGraph> continentalness, std::shared_ptr<INoiseGraph> erosion,
    std::shared_ptr<INoiseGraph> weirdness, std::shared_ptr<INoiseGraph> river) {
    return std::make_shared<ClimateSampler>(
        std::move(temperature), std::move(moisture), std::move(continentalness),
        std::move(erosion), std::move(weirdness), std::move(river));
}

std::shared_ptr<ISurfaceResolver> create_surface_resolver(
    std::shared_ptr<const IBiomeRegistry> registry) {
    return std::make_shared<SurfaceResolver>(std::move(registry));
}

std::shared_ptr<IClimateVoxelGenerator> create_climate_voxel_generator(
    std::shared_ptr<const INoiseGraph> height,
    std::shared_ptr<const IClimateSampler> sampler,
    std::shared_ptr<const IBiomeRegistry> registry,
    std::shared_ptr<const ISurfaceResolver> resolver,
    std::shared_ptr<const IDensityFunction> caves,
    std::shared_ptr<const IDensityFunction> ores, int baseHeight, int amplitude,
    std::shared_ptr<const IOreTable> oreTable,
    std::shared_ptr<const ICarver> carver,
    std::shared_ptr<const IDecoratorSet> decoratorSet) {
    return std::make_shared<ClimateVoxelGenerator>(
        std::move(height), std::move(sampler), std::move(registry),
        std::move(resolver), std::move(caves), std::move(ores), baseHeight,
        amplitude, std::move(oreTable), std::move(carver),
        std::move(decoratorSet));
}

}  // namespace procgen
}  // namespace engine
