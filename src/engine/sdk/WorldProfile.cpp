// WorldProfile.cpp
//
// SDK adapter for engine/procgen/IWorldProfile.hpp (META section 18 /
// FALTANTES item 14, item 16 da ordem: "Permitir geração equivalente a
// Minecraft moderno sem recompilar a engine"). Each generation domain is
// already data-driven behind a public contract; this TU is the COMPOSITION —
// one versioned JSON document that assembles them into the generator the
// world uses and the structure-placement system. No backend: every section is
// parsed/validated by its OWN subsystem factory/parser (single source of
// truth) and the generator is composed through the public
// create_climate_voxel_generator factory.
//
// Round-trip: each present section is stored in canonical form (the piece's
// own serialize() output, or the deterministic stringified object for the
// caves/ores density sections), so serialize() re-emits the exact canonical
// document and deserialize(serialize(p)) is byte-identical.

#include "engine/procgen/IWorldProfile.hpp"
#include "engine/procgen/INoiseGraph.hpp"
#include "engine/procgen/IClimateBiome.hpp"
#include "engine/procgen/IWorldFeatures.hpp"
#include "RegistryJson.hpp"

#include <memory>
#include <sstream>
#include <string>
#include <utility>

namespace engine {
namespace procgen {
namespace {

// Minimal valid graph used as a seed instance before deserialize replaces the
// contents (the graph factory requires a non-empty spec; deserialize is
// all-or-nothing and swaps in the document's nodes).
std::shared_ptr<INoiseGraph> make_seed_graph(std::string& errorOut) {
    NoiseGraphSpec spec;
    spec.seed = 1;
    spec.nodes.push_back({ "constant", { 0.5f }, {} });
    spec.root = 0;
    return create_noise_graph_from_spec(spec, errorOut);
}

std::string json_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

// RegistryJson.hpp only parses; this minimal stringifier re-encodes a parsed
// section so each subsystem's OWN deserializer is the single parser (the
// object map is std::map — sorted — so the output is deterministic).
std::string json_stringify(const sdk::JsonValue& value) {
    switch (value.kind) {
        case sdk::JsonValue::Kind::Null: return "null";
        case sdk::JsonValue::Kind::Bool:
            return value.boolean ? "true" : "false";
        case sdk::JsonValue::Kind::Number: {
            std::ostringstream ss;
            ss.precision(17);
            ss << value.number;
            return ss.str();
        }
        case sdk::JsonValue::Kind::String:
            return "\"" + json_escape(value.string) + "\"";
        case sdk::JsonValue::Kind::Array: {
            std::string out = "[";
            for (std::size_t i = 0; i < value.array.size(); ++i) {
                if (i != 0) out += ",";
                out += json_stringify(value.array[i]);
            }
            return out + "]";
        }
        case sdk::JsonValue::Kind::Object: {
            std::string out = "{";
            bool first = true;
            for (const auto& entry : value.object) {
                if (!first) out += ",";
                first = false;
                out += "\"" + json_escape(entry.first) + "\":" +
                       json_stringify(entry.second);
            }
            return out + "}";
        }
    }
    return "null";
}

class WorldProfile final : public IWorldProfile {
public:
    std::shared_ptr<engine::voxel::IVoxelGenerator> generator() const override {
        return generator_;
    }

    std::shared_ptr<IStructurePlacementSystem> structure_placement() const override {
        return placement_;
    }

    bool serialize(std::string& out) const override {
        std::ostringstream ss;
        ss.precision(9);
        ss << "{\"version\":1";
        if (!heightJson_.empty()) {
            ss << ",\"height\":" << heightJson_ << ",\"baseHeight\":" << baseHeight_
               << ",\"amplitude\":" << amplitude_;
        }
        if (!climateJson_.empty()) ss << ",\"climate\":" << climateJson_;
        if (!biomesJson_.empty()) ss << ",\"biomes\":" << biomesJson_;
        if (!cavesJson_.empty()) ss << ",\"caves\":" << cavesJson_;
        if (!oresJson_.empty()) ss << ",\"ores\":" << oresJson_;
        if (!carverJson_.empty()) ss << ",\"carver\":" << carverJson_;
        if (!decoratorsJson_.empty()) ss << ",\"decorators\":" << decoratorsJson_;
        if (!structuresJson_.empty()) ss << ",\"structures\":" << structuresJson_;
        ss << "}";
        out = ss.str();
        return true;
    }

    // Setters used by the factory after all sections validated (commit).
    void set_height(std::string json, int baseHeight, int amplitude,
                    std::shared_ptr<INoiseGraph> graph) {
        heightJson_ = std::move(json);
        baseHeight_ = baseHeight;
        amplitude_ = amplitude;
        height_ = std::move(graph);
    }
    void set_climate(std::string json, std::shared_ptr<IClimateSampler> sampler) {
        climateJson_ = std::move(json);
        sampler_ = std::move(sampler);
    }
    void set_biomes(std::string json, std::shared_ptr<const IBiomeRegistry> registry,
                    std::shared_ptr<const ISurfaceResolver> resolver) {
        biomesJson_ = std::move(json);
        registry_ = std::move(registry);
        resolver_ = std::move(resolver);
    }
    void set_caves(std::string json, std::shared_ptr<const IDensityFunction> caves) {
        cavesJson_ = std::move(json);
        caves_ = std::move(caves);
    }
    void set_ores(std::string json, std::shared_ptr<const IDensityFunction> ores,
                  std::shared_ptr<const IOreTable> table) {
        oresJson_ = std::move(json);
        ores_ = std::move(ores);
        oreTable_ = std::move(table);
    }
    void set_carver(std::string json, std::shared_ptr<const ICarver> carver) {
        carverJson_ = std::move(json);
        carver_ = std::move(carver);
    }
    void set_decorators(std::string json,
                        std::shared_ptr<const IDecoratorSet> decorators) {
        decoratorsJson_ = std::move(json);
        decoratorSet_ = std::move(decorators);
    }
    void set_structures(std::string json,
                        std::shared_ptr<IStructurePlacementSystem> placement) {
        structuresJson_ = std::move(json);
        placement_ = std::move(placement);
    }
    void compose_generator() {
        generator_ = create_climate_voxel_generator(
            height_, sampler_, registry_, resolver_, caves_, ores_, baseHeight_,
            amplitude_, oreTable_, carver_, decoratorSet_);
    }

private:
    // Canonical JSON per section (verbatim re-emission).
    std::string heightJson_;
    std::string climateJson_;
    std::string biomesJson_;
    std::string cavesJson_;
    std::string oresJson_;
    std::string carverJson_;
    std::string decoratorsJson_;
    std::string structuresJson_;
    // Composed pieces.
    std::shared_ptr<INoiseGraph> height_;
    std::shared_ptr<IClimateSampler> sampler_;
    std::shared_ptr<const IBiomeRegistry> registry_;
    std::shared_ptr<const ISurfaceResolver> resolver_;
    std::shared_ptr<const IDensityFunction> caves_;
    std::shared_ptr<const IDensityFunction> ores_;
    std::shared_ptr<const IOreTable> oreTable_;
    std::shared_ptr<const ICarver> carver_;
    std::shared_ptr<const IDecoratorSet> decoratorSet_;
    std::shared_ptr<IStructurePlacementSystem> placement_;
    std::shared_ptr<engine::voxel::IVoxelGenerator> generator_;
    int baseHeight_{ 0 };
    int amplitude_{ 1 };
};

// Parses a noise-graph section (object) through the graph's own parser.
// Returns false + diagnostic on failure; on success `canonicalOut` is the
// graph's canonical JSON and `graphOut` the instance.
bool parse_graph_section(const sdk::JsonValue& section, std::string& canonicalOut,
                         std::shared_ptr<INoiseGraph>& graphOut,
                         std::string& errorOut) {
    std::string error;
    auto graph = make_seed_graph(error);
    if (!graph || !graph->deserialize(json_stringify(section), error)) {
        errorOut = "world profile: invalid noise graph - " + error;
        return false;
    }
    std::string canonical;
    if (!graph->serialize(canonical)) {
        errorOut = "world profile: noise graph failed to serialize";
        return false;
    }
    canonicalOut = std::move(canonical);
    graphOut = std::move(graph);
    return true;
}

// Parses a density-field section {density, scale, offset} (optional). On a
// missing section, leaves the outputs untouched and returns true.
bool parse_density_section(const sdk::JsonValue* section, std::string& canonicalOut,
                           std::shared_ptr<const IDensityFunction>& densityOut,
                           std::string& errorOut) {
    if (section == nullptr) return true;
    if (!section->is_object()) {
        errorOut = "world profile: density section must be an object";
        return false;
    }
    const sdk::JsonValue* density = section->field("density");
    if (density == nullptr || !density->is_object()) {
        errorOut = "world profile: density section needs a \"density\" graph";
        return false;
    }
    std::string graphJson;
    std::shared_ptr<INoiseGraph> graph;
    if (!parse_graph_section(*density, graphJson, graph, errorOut)) return false;
    const float scale =
        static_cast<float>(sdk::json_number(*section, "scale", 1.0));
    const float offset =
        static_cast<float>(sdk::json_number(*section, "offset", 0.0));
    if (!std::isfinite(scale) || !std::isfinite(offset)) {
        errorOut = "world profile: density scale/offset must be finite";
        return false;
    }
    std::ostringstream ss;
    ss.precision(9);
    ss << "{\"density\":" << graphJson << ",\"scale\":" << scale
       << ",\"offset\":" << offset << "}";
    canonicalOut = ss.str();
    densityOut = create_graph_density_function(std::move(graph), scale, offset);
    return true;
}

}  // namespace

std::shared_ptr<IWorldProfile> create_world_profile_from_json(
    const std::string& json, std::string& errorOut) {
    errorOut.clear();
    sdk::JsonValue document;
    if (!sdk::json_parse(json, document, errorOut)) {
        errorOut = "world profile: malformed document - " + errorOut;
        return nullptr;
    }
    if (!document.is_object()) {
        errorOut = "world profile: document must be a JSON object";
        return nullptr;
    }
    const sdk::JsonValue* version = document.field("version");
    if (version == nullptr || version->kind != sdk::JsonValue::Kind::Number ||
        static_cast<int>(version->number) != 1) {
        errorOut = "world profile: unsupported document version";
        return nullptr;
    }

    auto profile = std::make_shared<WorldProfile>();

    // ---- height + baseHeight/amplitude ----
    if (const sdk::JsonValue* height = document.field("height")) {
        std::string graphJson;
        std::shared_ptr<INoiseGraph> graph;
        if (!parse_graph_section(*height, graphJson, graph, errorOut)) return nullptr;
        const int baseHeight =
            static_cast<int>(sdk::json_number(document, "baseHeight", 0.0));
        const int amplitude =
            static_cast<int>(sdk::json_number(document, "amplitude", 1.0));
        if (amplitude < 0) {
            errorOut = "world profile: amplitude must be >= 0";
            return nullptr;
        }
        profile->set_height(std::move(graphJson), baseHeight, amplitude,
                            std::move(graph));
    }

    // ---- climate axes (each optional, each through the graph parser) ----
    if (const sdk::JsonValue* climate = document.field("climate")) {
        if (!climate->is_object()) {
            errorOut = "world profile: \"climate\" must be an object";
            return nullptr;
        }
        std::shared_ptr<INoiseGraph> temperature;
        std::shared_ptr<INoiseGraph> moisture;
        std::shared_ptr<INoiseGraph> continentalness;
        std::shared_ptr<INoiseGraph> erosion;
        std::shared_ptr<INoiseGraph> weirdness;
        std::shared_ptr<INoiseGraph> river;
        std::ostringstream ss;
        ss << "{";
        bool first = true;
        // Emits an axis when present: canonical graph JSON into the document
        // AND the graph into the sampler (missing axes are omitted and null).
        const auto axis = [&](const char* name, std::shared_ptr<INoiseGraph>& out) {
            if (const sdk::JsonValue* graph = climate->field(name)) {
                std::string graphJson;
                if (!parse_graph_section(*graph, graphJson, out, errorOut)) {
                    return false;
                }
                if (!first) ss << ',';
                first = false;
                ss << "\"" << name << "\":" << graphJson;
            }
            return true;
        };
        if (!axis("temperature", temperature) || !axis("moisture", moisture) ||
            !axis("continentalness", continentalness) || !axis("erosion", erosion) ||
            !axis("weirdness", weirdness) || !axis("river", river)) {
            return nullptr;
        }
        ss << "}";
        profile->set_climate(
            ss.str(),
            create_climate_sampler(std::move(temperature), std::move(moisture),
                                   std::move(continentalness), std::move(erosion),
                                   std::move(weirdness), std::move(river)));
    }

    // ---- biomes + surface resolver ----
    if (const sdk::JsonValue* biomes = document.field("biomes")) {
        std::string error;
        auto registry = create_biome_registry_from_json(json_stringify(*biomes), error);
        if (!registry) {
            errorOut = "world profile: invalid biomes - " + error;
            return nullptr;
        }
        std::string canonical;
        if (!registry->serialize(canonical)) {
            errorOut = "world profile: biome registry failed to serialize";
            return nullptr;
        }
        profile->set_biomes(std::move(canonical), std::move(registry),
                            create_surface_resolver(registry));
    }

    // ---- caves / ores (density fields + tables) ----
    std::string cavesJson;
    std::shared_ptr<const IDensityFunction> caves;
    if (!parse_density_section(document.field("caves"), cavesJson, caves, errorOut))
        return nullptr;
    profile->set_caves(std::move(cavesJson), std::move(caves));

    std::string oresJson;
    std::shared_ptr<const IDensityFunction> ores;
    std::shared_ptr<const IOreTable> oreTable;
    if (const sdk::JsonValue* oresSection = document.field("ores")) {
        if (!oresSection->is_object()) {
            errorOut = "world profile: \"ores\" must be an object";
            return nullptr;
        }
        std::ostringstream ss;
        ss << "{";
        bool first = true;
        if (const sdk::JsonValue* density = oresSection->field("density")) {
            std::string graphJson;
            std::shared_ptr<INoiseGraph> graph;
            if (!parse_graph_section(*density, graphJson, graph, errorOut))
                return nullptr;
            const float scale =
                static_cast<float>(sdk::json_number(*oresSection, "scale", 1.0));
            const float offset =
                static_cast<float>(sdk::json_number(*oresSection, "offset", 0.0));
            if (!std::isfinite(scale) || !std::isfinite(offset)) {
                errorOut = "world profile: ores scale/offset must be finite";
                return nullptr;
            }
            ss << "\"density\":" << graphJson << ",\"scale\":" << scale
               << ",\"offset\":" << offset;
            ores = create_graph_density_function(std::move(graph), scale, offset);
            first = false;
        }
        if (const sdk::JsonValue* table = oresSection->field("table")) {
            std::string error;
            auto parsed = create_ore_table_from_json(json_stringify(*table), error);
            if (!parsed) {
                errorOut = "world profile: invalid ore table - " + error;
                return nullptr;
            }
            std::string canonical;
            if (!parsed->serialize(canonical)) {
                errorOut = "world profile: ore table failed to serialize";
                return nullptr;
            }
            if (!first) ss << ',';
            ss << "\"table\":" << canonical;
            oreTable = std::move(parsed);
        }
        ss << "}";
        oresJson = ss.str();
    }
    profile->set_ores(std::move(oresJson), std::move(ores), std::move(oreTable));

    // ---- carver ----
    if (const sdk::JsonValue* carver = document.field("carver")) {
        std::string error;
        auto parsed = create_carver_from_json(json_stringify(*carver), error);
        if (!parsed) {
            errorOut = "world profile: invalid carver - " + error;
            return nullptr;
        }
        std::string canonical;
        if (!parsed->serialize(canonical)) {
            errorOut = "world profile: carver failed to serialize";
            return nullptr;
        }
        profile->set_carver(std::move(canonical), std::move(parsed));
    }

    // ---- decorators ----
    if (const sdk::JsonValue* decorators = document.field("decorators")) {
        std::string error;
        auto parsed =
            create_decorator_set_from_json(json_stringify(*decorators), error);
        if (!parsed) {
            errorOut = "world profile: invalid decorators - " + error;
            return nullptr;
        }
        std::string canonical;
        if (!parsed->serialize(canonical)) {
            errorOut = "world profile: decorator set failed to serialize";
            return nullptr;
        }
        profile->set_decorators(std::move(canonical), std::move(parsed));
    }

    // ---- structures (definitions + spawn rules) ----
    if (const sdk::JsonValue* structures = document.field("structures")) {
        std::string error;
        auto parsed =
            create_structure_placement_system_from_json(json_stringify(*structures),
                                                        error);
        if (!parsed) {
            errorOut = "world profile: invalid structures - " + error;
            return nullptr;
        }
        std::string canonical;
        if (!parsed->serialize(canonical)) {
            errorOut = "world profile: structure placement failed to serialize";
            return nullptr;
        }
        profile->set_structures(std::move(canonical), std::move(parsed));
    } else {
        profile->set_structures("", create_structure_placement_system());
    }

    // Compose the generator last (all pieces committed).
    profile->compose_generator();
    return profile;
}

}  // namespace procgen
}  // namespace engine
