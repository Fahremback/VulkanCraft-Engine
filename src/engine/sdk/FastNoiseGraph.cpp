// FastNoiseGraph.cpp
//
// SDK adapter for engine/procgen/INoiseGraph.hpp (META section 18 / FALTANTES
// item 14). The public contract never leaks the backend; this TU implements it
// with the COMPLETE FastNoiseLite (Auburn, MIT — the single-header library the
// engine already adopted; the full version is vendored at
// third_party/fastnoise_lite/FastNoiseLite.h from the godot-voxel catalog
// clone, which keeps the complete noise-type/fractal machinery the engine's
// trimmed in-simulation copy dropped). Deterministic per seed. The catalog
// authority FastNoise2 is BLOCKED at the promotion gate (its pinned clone
// requires FastSIMD, not vendored) — swapping the backend only rewrites this
// file (see DEPENDENCY_POLICY).
//
// Node semantics: base noise nodes (value/perlin/simplex) and fractals
// (fbm/ridged/pingpong) build a FastNoiseLite instance each; fractal sources
// must be base noise nodes (FastNoiseLite folds the fractal into the noise
// instance). Blends (add/multiply/min/max/lerp) and constants evaluate
// recursively. Same spec + seed -> bit-identical samples from any instance or
// sampling order (determinism is seed-based, not thread-count-based).

#include "engine/procgen/INoiseGraph.hpp"
#include "RegistryJson.hpp"

#include "fastnoise_lite/FastNoiseLite.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace engine {
namespace procgen {
namespace {

using NoiseLite = fast_noise_lite::FastNoiseLite;

struct NodeState {
    std::string type;
    std::vector<float> params;
    std::vector<std::uint32_t> sources;
    std::optional<NoiseLite> noise;  // base + fractal nodes only
};

bool validate_node(const NoiseNodeSpec& node, std::uint32_t index,
                   std::string& error) {
    const auto require_sources = [&](std::size_t count) {
        if (node.sources.size() != count) {
            error = "noise graph: node " + std::to_string(index) + " ('" +
                    node.type + "') expects " + std::to_string(count) +
                    " source(s), got " + std::to_string(node.sources.size());
            return false;
        }
        for (const std::uint32_t source : node.sources) {
            if (source >= index) {
                error = "noise graph: node " + std::to_string(index) +
                        " references source " + std::to_string(source) +
                        " (must be an earlier node)";
                return false;
            }
        }
        return true;
    };
    if (node.type == "constant") {
        return node.params.size() >= 1 ? true
                                       : (error = "noise graph: constant expects params=[value]",
                                          false);
    }
    if (node.type == "value" || node.type == "perlin" || node.type == "simplex") {
        if (node.params.size() < 2) {
            error = "noise graph: " + node.type +
                    " expects params=[frequency, seedOffset]";
            return false;
        }
        return true;
    }
    if (node.type == "fbm" || node.type == "ridged" || node.type == "pingpong") {
        if (!require_sources(1)) return false;
        if (node.params.size() < 4) {
            error = "noise graph: " + node.type +
                    " expects params=[octaves, gain, lacunarity, weightedStrength]";
            return false;
        }
        return true;
    }
    if (node.type == "add" || node.type == "multiply" || node.type == "min" ||
        node.type == "max") {
        return require_sources(2);
    }
    if (node.type == "lerp") {
        return require_sources(3);
    }
    error = "noise graph: unknown node type '" + node.type + "'";
    return false;
}

NoiseLite::NoiseType base_noise_type(const std::string& type) {
    if (type == "perlin") return NoiseLite::NoiseType_Perlin;
    if (type == "simplex") return NoiseLite::NoiseType_OpenSimplex2;
    return NoiseLite::NoiseType_Value;
}

NoiseLite::FractalType fractal_type(const std::string& type) {
    if (type == "ridged") return NoiseLite::FractalType_Ridged;
    if (type == "pingpong") return NoiseLite::FractalType_PingPong;
    return NoiseLite::FractalType_FBm;
}

// Builds the per-node FastNoiseLite instances (base + fractal) and validates
// the whole graph. `seed` is baked into the instances (set_seed rebuilds).
bool build_graph(const NoiseGraphSpec& spec, std::uint32_t seed,
                 std::vector<NodeState>& states, std::string& error) {
    if (spec.nodes.empty()) {
        error = "noise graph: graph has no nodes";
        return false;
    }
    if (spec.root >= spec.nodes.size()) {
        error = "noise graph: root " + std::to_string(spec.root) +
                " out of range (0.." + std::to_string(spec.nodes.size() - 1) + ")";
        return false;
    }
    states.clear();
    states.reserve(spec.nodes.size());
    for (std::uint32_t i = 0; i < spec.nodes.size(); ++i) {
        if (!validate_node(spec.nodes[i], i, error)) return false;
        NodeState state;
        state.type = spec.nodes[i].type;
        state.params = spec.nodes[i].params;
        state.sources = spec.nodes[i].sources;
        if (state.type == "value" || state.type == "perlin" ||
            state.type == "simplex") {
            NoiseLite noise(static_cast<int>(seed) +
                            static_cast<int>(state.params[1]));
            noise.SetNoiseType(base_noise_type(state.type));
            noise.SetFractalType(NoiseLite::FractalType_None);
            noise.SetFrequency(state.params[0]);
            state.noise = std::move(noise);
        } else if (state.type == "fbm" || state.type == "ridged" ||
                   state.type == "pingpong") {
            const std::uint32_t source = state.sources[0];
            const NodeState& src = states[source];
            if (src.type != "value" && src.type != "perlin" &&
                src.type != "simplex") {
                error = "noise graph: node " + std::to_string(i) +
                        " ('" + state.type +
                        "') source must be a base noise node (value/perlin/"
                        "simplex), got '" +
                        src.type + "'";
                return false;
            }
            // Same seed/frequency/noise type as the source, fractal folded in.
            NoiseLite noise(static_cast<int>(seed) +
                            static_cast<int>(src.params[1]));
            noise.SetNoiseType(base_noise_type(src.type));
            noise.SetFrequency(src.params[0]);
            noise.SetFractalType(fractal_type(state.type));
            noise.SetFractalOctaves(static_cast<int>(state.params[0]));
            noise.SetFractalGain(state.params[1]);
            noise.SetFractalLacunarity(state.params[2]);
            noise.SetFractalWeightedStrength(state.params[3]);
            state.noise = std::move(noise);
        }
        states.push_back(std::move(state));
    }
    return true;
}

float eval_2d(const std::vector<NodeState>& states, std::uint32_t index, float x,
              float z) {
    const NodeState& node = states[index];
    if (node.noise) return node.noise->GetNoise(x, z);
    if (node.type == "constant") return node.params[0];
    if (node.type == "add") {
        return eval_2d(states, node.sources[0], x, z) +
               eval_2d(states, node.sources[1], x, z);
    }
    if (node.type == "multiply") {
        return eval_2d(states, node.sources[0], x, z) *
               eval_2d(states, node.sources[1], x, z);
    }
    if (node.type == "min") {
        return std::min(eval_2d(states, node.sources[0], x, z),
                        eval_2d(states, node.sources[1], x, z));
    }
    if (node.type == "max") {
        return std::max(eval_2d(states, node.sources[0], x, z),
                        eval_2d(states, node.sources[1], x, z));
    }
    // lerp(a, b, control)
    const float a = eval_2d(states, node.sources[0], x, z);
    const float b = eval_2d(states, node.sources[1], x, z);
    const float c = eval_2d(states, node.sources[2], x, z);
    return a * (1.0f - c) + b * c;
}

float eval_3d(const std::vector<NodeState>& states, std::uint32_t index, float x,
              float y, float z) {
    const NodeState& node = states[index];
    if (node.noise) return node.noise->GetNoise(x, y, z);
    if (node.type == "constant") return node.params[0];
    if (node.type == "add") {
        return eval_3d(states, node.sources[0], x, y, z) +
               eval_3d(states, node.sources[1], x, y, z);
    }
    if (node.type == "multiply") {
        return eval_3d(states, node.sources[0], x, y, z) *
               eval_3d(states, node.sources[1], x, y, z);
    }
    if (node.type == "min") {
        return std::min(eval_3d(states, node.sources[0], x, y, z),
                        eval_3d(states, node.sources[1], x, y, z));
    }
    if (node.type == "max") {
        return std::max(eval_3d(states, node.sources[0], x, y, z),
                        eval_3d(states, node.sources[1], x, y, z));
    }
    const float a = eval_3d(states, node.sources[0], x, y, z);
    const float b = eval_3d(states, node.sources[1], x, y, z);
    const float c = eval_3d(states, node.sources[2], x, y, z);
    return a * (1.0f - c) + b * c;
}

std::string float_str(float value) {
    // %.9g round-trips float32 exactly, so a serialized graph re-samples
    // bit-identically after deserialize.
    std::ostringstream ss;
    ss.precision(9);
    ss << value;
    return ss.str();
}

class FastNoiseGraph final : public INoiseGraph {
public:
    explicit FastNoiseGraph(const NoiseGraphSpec& spec, std::string& error) {
        seed_ = spec.seed;
        rootIndex_ = spec.root;
        nodes_ = spec.nodes;
        if (!rebuild(error)) {
            nodes_.clear();
            states_.clear();
        }
    }

    const char* name() const override { return "fastnoise-lite"; }
    std::uint32_t seed() const override { return seed_; }
    void set_seed(std::uint32_t seed) override {
        seed_ = seed;
        std::string error;
        (void)rebuild(error);  // spec was already validated at build
    }

    float sample_2d(float x, float z) const override {
        return eval_2d(states_, rootIndex_, x, z);
    }

    float sample_3d(float x, float y, float z) const override {
        return eval_3d(states_, rootIndex_, x, y, z);
    }

    bool serialize(std::string& out) const override {
        std::ostringstream ss;
        ss << "{\"version\":1,\"seed\":" << seed_ << ",\"root\":" << rootIndex_
           << ",\"nodes\":[";
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            if (i != 0) ss << ',';
            ss << "{\"type\":\"" << nodes_[i].type << "\",\"params\":[";
            for (std::size_t p = 0; p < nodes_[i].params.size(); ++p) {
                if (p != 0) ss << ',';
                ss << float_str(nodes_[i].params[p]);
            }
            ss << "],\"sources\":[";
            for (std::size_t s = 0; s < nodes_[i].sources.size(); ++s) {
                if (s != 0) ss << ',';
                ss << nodes_[i].sources[s];
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
            errorOut = "noise graph: malformed asset — " + errorOut;
            return false;
        }
        if (!document.is_object()) {
            errorOut = "noise graph: asset must be a JSON object";
            return false;
        }
        const sdk::JsonValue* version = document.field("version");
        if (version == nullptr || version->kind != sdk::JsonValue::Kind::Number ||
            static_cast<int>(version->number) != 1) {
            errorOut = "noise graph: unsupported asset version";
            return false;
        }
        NoiseGraphSpec spec;
        spec.seed = static_cast<std::uint32_t>(sdk::json_number(document, "seed", 0.0));
        spec.root = static_cast<std::uint32_t>(sdk::json_number(document, "root", 0.0));
        const sdk::JsonValue* nodes = document.field("nodes");
        if (nodes == nullptr || !nodes->is_array()) {
            errorOut = "noise graph: asset has no \"nodes\" array";
            return false;
        }
        for (const sdk::JsonValue& entry : nodes->array) {
            if (!entry.is_object()) {
                errorOut = "noise graph: node entry must be an object";
                return false;
            }
            NoiseNodeSpec node;
            node.type = sdk::json_string(entry, "type", "");
            for (const double value : sdk::json_number_array(entry, "params")) {
                node.params.push_back(static_cast<float>(value));
            }
            const sdk::JsonValue* sources = entry.field("sources");
            if (sources != nullptr && sources->is_array()) {
                for (const sdk::JsonValue& source : sources->array) {
                    if (source.kind != sdk::JsonValue::Kind::Number) {
                        errorOut = "noise graph: node source must be a number";
                        return false;
                    }
                    node.sources.push_back(static_cast<std::uint32_t>(source.number));
                }
            }
            spec.nodes.push_back(std::move(node));
        }
        // All-or-nothing: on failure the graph keeps its previous valid state
        // (seed, root, node list and built instances stay consistent).
        const std::uint32_t oldSeed = seed_;
        const std::uint32_t oldRoot = rootIndex_;
        const std::vector<NoiseNodeSpec> oldNodes = nodes_;
        seed_ = spec.seed;
        rootIndex_ = spec.root;
        nodes_ = spec.nodes;
        if (!rebuild(errorOut)) {
            seed_ = oldSeed;
            rootIndex_ = oldRoot;
            nodes_ = oldNodes;
            return false;
        }
        return true;
    }

private:
    bool rebuild(std::string& error) {
        std::vector<NodeState> states;
        if (!build_graph(NoiseGraphSpec{ seed_, rootIndex_, nodes_ }, seed_, states,
                         error)) {
            return false;
        }
        states_ = std::move(states);
        return true;
    }

    std::vector<NoiseNodeSpec> nodes_;
    std::vector<NodeState> states_;
    std::uint32_t seed_{ 0 };
    std::uint32_t rootIndex_{ 0 };
};

class GraphDensityFunction final : public IDensityFunction {
public:
    GraphDensityFunction(std::shared_ptr<const INoiseGraph> graph, float scale,
                         float offset)
        : graph_(std::move(graph)), scale_(scale), offset_(offset) {}

    float sample(float x, float y, float z) const override {
        return graph_->sample_3d(x, y, z) * scale_ + offset_;
    }

private:
    std::shared_ptr<const INoiseGraph> graph_;
    float scale_;
    float offset_;
};

class GraphVoxelGenerator final : public IGraphVoxelGenerator {
public:
    GraphVoxelGenerator(std::shared_ptr<const INoiseGraph> height,
                        std::shared_ptr<const IDensityFunction> caves,
                        std::shared_ptr<const IDensityFunction> ores,
                        int baseHeight, int amplitude)
        : height_(std::move(height)),
          caves_(std::move(caves)),
          ores_(std::move(ores)),
          baseHeight_(baseHeight),
          amplitude_(amplitude) {}

    voxel::TerrainPoint sample(float worldX, float worldZ) const override {
        voxel::TerrainPoint point;
        const float height =
            height_->sample_2d(worldX, worldZ) * static_cast<float>(amplitude_);
        point.height = baseHeight_ + static_cast<int>(std::lround(height));
        point.temperature = 0.5f;
        point.moisture = 0.5f;
        point.slope = 0.0f;
        point.biomeIndex = 0;
        return point;
    }

    float cave_density(float worldX, float worldY, float worldZ) const override {
        return caves_ ? caves_->sample(worldX, worldY, worldZ) : -1.0f;
    }

    float ore_density(float worldX, float worldY, float worldZ) const override {
        return ores_ ? ores_->sample(worldX, worldY, worldZ) : -1.0f;
    }

private:
    std::shared_ptr<const INoiseGraph> height_;
    std::shared_ptr<const IDensityFunction> caves_;
    std::shared_ptr<const IDensityFunction> ores_;
    int baseHeight_;
    int amplitude_;
};

}  // namespace

std::shared_ptr<INoiseGraph> create_noise_graph_from_spec(
    const NoiseGraphSpec& spec, std::string& errorOut) {
    errorOut.clear();
    auto graph = std::make_shared<FastNoiseGraph>(spec, errorOut);
    if (!errorOut.empty()) return nullptr;
    return graph;
}

std::shared_ptr<IDensityFunction> create_graph_density_function(
    std::shared_ptr<const INoiseGraph> graph, float scale, float offset) {
    return std::make_shared<GraphDensityFunction>(std::move(graph), scale, offset);
}

std::shared_ptr<IGraphVoxelGenerator> create_graph_voxel_generator(
    std::shared_ptr<const INoiseGraph> height,
    std::shared_ptr<const IDensityFunction> caves,
    std::shared_ptr<const IDensityFunction> ores, int baseHeight, int amplitude) {
    return std::make_shared<GraphVoxelGenerator>(
        std::move(height), std::move(caves), std::move(ores), baseHeight, amplitude);
}

}  // namespace procgen
}  // namespace engine
