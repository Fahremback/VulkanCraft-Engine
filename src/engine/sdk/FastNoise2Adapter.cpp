// FastNoise2Adapter.cpp
//
// FastNoise2 backend for INoiseGraph (G.fastnoise2 integration).
// Uses the godot-voxel vendored fork (v0.10.0, self-contained FastSIMD).
// Same spec + seed -> deterministic samples (seed-based, not thread-count).
//
// NOTE: In this fork, GenSingle2D/3D take (x, y, seed) — no frequency
// parameter. Frequency is applied by coordinate scaling before sampling.

#include "FastNoise2Adapter.hpp"
#include "RegistryJson.hpp"

// FastNoise2 from the godot-voxel vendored fork.
#include "FastNoise/FastNoise.h"
#include "FastNoise/Generators/BasicGenerators.h"
#include "FastNoise/Generators/Perlin.h"
#include "FastNoise/Generators/Simplex.h"
#include "FastNoise/Generators/Cellular.h"
#include "FastNoise/Generators/Fractal.h"
#include "FastNoise/Generators/Blends.h"
#include "FastNoise/Generators/Modifiers.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace engine {
namespace procgen {
namespace {

struct CompiledNode {
    FastNoise::SmartNode<> node;
    float frequency{ 1.0f };
};

bool validate_sources(const NoiseNodeSpec& ns, std::size_t index,
                      std::string& error) {
    for (std::uint32_t src : ns.sources) {
        if (src >= index) {
            error = "noise graph: node " + std::to_string(index) +
                    " references source " + std::to_string(src) +
                    " (must be < index)";
            return false;
        }
    }
    return true;
}

FastNoise::SmartNode<> create_base_noise(const std::string& type) {
    if (type == "value") return FastNoise::New<FastNoise::Value>();
    if (type == "perlin") return FastNoise::New<FastNoise::Perlin>();
    if (type == "simplex") return FastNoise::New<FastNoise::Simplex>();
    return nullptr;
}

FastNoise::SmartNode<> create_fractal(const std::string& type,
                                       FastNoise::SmartNode<> source,
                                       const std::vector<float>& params) {
    if (params.size() < 4 || !source) return nullptr;
    const int octaves = static_cast<int>(params[0]);
    const float gain = params[1];
    const float lacunarity = params[2];
    const float weighted = params[3];

    if (type == "fbm") {
        auto n = FastNoise::New<FastNoise::FractalFBm>();
        n->SetSource(source);
        n->SetOctaveCount(octaves);
        n->SetGain(gain);
        n->SetLacunarity(lacunarity);
        n->SetWeightedStrength(weighted);
        return n;
    }
    if (type == "ridged") {
        auto n = FastNoise::New<FastNoise::FractalRidged>();
        n->SetSource(source);
        n->SetOctaveCount(octaves);
        n->SetGain(gain);
        n->SetLacunarity(lacunarity);
        n->SetWeightedStrength(weighted);
        return n;
    }
    if (type == "pingpong") {
        auto n = FastNoise::New<FastNoise::FractalPingPong>();
        n->SetSource(source);
        n->SetOctaveCount(octaves);
        n->SetGain(gain);
        n->SetLacunarity(lacunarity);
        n->SetWeightedStrength(weighted);
        return n;
    }
    return nullptr;
}

bool compile_graph(const NoiseGraphSpec& spec,
                   std::vector<CompiledNode>& out,
                   std::string& error) {
    out.clear();
    out.resize(spec.nodes.size());

    for (std::size_t i = 0; i < spec.nodes.size(); ++i) {
        const auto& ns = spec.nodes[i];
        if (!validate_sources(ns, i, error)) return false;

        float frequency = (ns.params.size() > 0) ? ns.params[0] : 0.01f;

        if (ns.type == "constant") {
            out[i].node = FastNoise::New<FastNoise::Value>();
            out[i].frequency = 0.0f;
            continue;
        }

        if (ns.type == "value" || ns.type == "perlin" || ns.type == "simplex") {
            out[i].node = create_base_noise(ns.type);
            out[i].frequency = frequency;
            if (!out[i].node) {
                error = "noise graph: failed to create '" + ns.type + "'";
                return false;
            }
            continue;
        }

        if (ns.type == "fbm" || ns.type == "ridged" || ns.type == "pingpong") {
            if (ns.sources.empty()) {
                error = "noise graph: " + ns.type + " requires a source";
                return false;
            }
            auto& src = out[ns.sources[0]];
            if (!src.node) {
                error = "noise graph: " + ns.type + " has null source";
                return false;
            }
            out[i].node = create_fractal(ns.type, src.node, ns.params);
            out[i].frequency = frequency;
            if (!out[i].node) {
                error = "noise graph: failed to create '" + ns.type + "'";
                return false;
            }
            continue;
        }

        if (ns.type == "add" || ns.type == "multiply" ||
            ns.type == "min" || ns.type == "max") {
            if (ns.sources.size() < 2) {
                error = "noise graph: " + ns.type + " requires 2 sources";
                return false;
            }
            auto& a = out[ns.sources[0]];
            auto& b = out[ns.sources[1]];
            if (!a.node || !b.node) {
                error = "noise graph: " + ns.type + " has null source";
                return false;
            }
            if (ns.type == "add") {
                auto n = FastNoise::New<FastNoise::Add>();
                n->SetLHS(a.node);
                n->SetRHS(b.node);
                out[i].node = n;
            } else if (ns.type == "multiply") {
                auto n = FastNoise::New<FastNoise::Multiply>();
                n->SetLHS(a.node);
                n->SetRHS(b.node);
                out[i].node = n;
            } else if (ns.type == "min") {
                auto n = FastNoise::New<FastNoise::Min>();
                n->SetLHS(a.node);
                n->SetRHS(b.node);
                out[i].node = n;
            } else if (ns.type == "max") {
                auto n = FastNoise::New<FastNoise::Max>();
                n->SetLHS(a.node);
                n->SetRHS(b.node);
                out[i].node = n;
            }
            continue;
        }

        if (ns.type == "lerp") {
            if (ns.sources.size() < 3) {
                error = "noise graph: lerp requires 3 sources";
                return false;
            }
            auto& a = out[ns.sources[0]];
            auto& b = out[ns.sources[1]];
            if (!a.node || !b.node) {
                error = "noise graph: lerp has null source";
                return false;
            }
            auto n = FastNoise::New<FastNoise::Add>();
            n->SetLHS(a.node);
            n->SetRHS(b.node);
            out[i].node = n;
            continue;
        }

        error = "noise graph: unknown node type '" + ns.type + "'";
        return false;
    }

    if (spec.root >= out.size() || !out[spec.root].node) {
        error = "noise graph: root node " + std::to_string(spec.root) +
                " is null or out of range";
        return false;
    }
    return true;
}

std::string float_str(float value) {
    std::ostringstream ss;
    ss.precision(9);
    ss << value;
    return ss.str();
}

float compute_constant(const NoiseNodeSpec& ns) {
    return (ns.params.size() > 0) ? ns.params[0] : 0.0f;
}

}  // namespace

class FastNoise2Graph final : public INoiseGraph {
public:
    explicit FastNoise2Graph(const NoiseGraphSpec& spec)
        : spec_(spec), seed_(spec.seed) {}

    const char* name() const override { return "FastNoise2 (SIMD)"; }
    std::uint32_t seed() const override { return seed_; }
    void set_seed(std::uint32_t s) override {
        seed_ = s;
        spec_.seed = s;
        compiled_.clear();
        root_ = nullptr;
        root_freq_ = 1.0f;
    }

    float sample_2d(float x, float z) const override {
        ensure_compiled();
        if (!root_) return 0.0f;
        if (spec_.root < spec_.nodes.size() &&
            spec_.nodes[spec_.root].type == "constant") {
            return compute_constant(spec_.nodes[spec_.root]);
        }
        return root_->GenSingle2D(x * root_freq_, z * root_freq_,
                                   static_cast<int>(seed_));
    }

    float sample_3d(float x, float y, float z) const override {
        ensure_compiled();
        if (!root_) return 0.0f;
        if (spec_.root < spec_.nodes.size() &&
            spec_.nodes[spec_.root].type == "constant") {
            return compute_constant(spec_.nodes[spec_.root]);
        }
        return root_->GenSingle3D(x * root_freq_, y * root_freq_,
                                   z * root_freq_,
                                   static_cast<int>(seed_));
    }

    bool serialize(std::string& out) const override {
        std::ostringstream ss;
        ss << "{\"version\":1,\"seed\":" << seed_
           << ",\"root\":" << spec_.root
           << ",\"nodes\":[";
        for (std::size_t i = 0; i < spec_.nodes.size(); ++i) {
            if (i != 0) ss << ',';
            ss << "{\"type\":\"" << spec_.nodes[i].type << "\",\"params\":[";
            for (std::size_t p = 0; p < spec_.nodes[i].params.size(); ++p) {
                if (p != 0) ss << ',';
                ss << float_str(spec_.nodes[i].params[p]);
            }
            ss << "],\"sources\":[";
            for (std::size_t s = 0; s < spec_.nodes[i].sources.size(); ++s) {
                if (s != 0) ss << ',';
                ss << spec_.nodes[i].sources[s];
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
        NoiseGraphSpec spec;
        spec.seed = static_cast<std::uint32_t>(
            sdk::json_number(document, "seed", 0.0));
        spec.root = static_cast<std::uint32_t>(
            sdk::json_number(document, "root", 0.0));
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
            for (double value : sdk::json_number_array(entry, "params")) {
                node.params.push_back(static_cast<float>(value));
            }
            const sdk::JsonValue* sources = entry.field("sources");
            if (sources != nullptr && sources->is_array()) {
                for (const sdk::JsonValue& source : sources->array) {
                    if (source.kind != sdk::JsonValue::Kind::Number) {
                        errorOut = "noise graph: node source must be a number";
                        return false;
                    }
                    node.sources.push_back(
                        static_cast<std::uint32_t>(source.number));
                }
            }
            spec.nodes.push_back(std::move(node));
        }
        spec_ = spec;
        seed_ = spec.seed;
        compiled_.clear();
        root_ = nullptr;
        return true;
    }

private:
    void ensure_compiled() const {
        if (root_) return;
        std::string error;
        if (!compile_graph(spec_, compiled_, error)) return;
        root_ = compiled_[spec_.root].node;
        root_freq_ = compiled_[spec_.root].frequency;
    }

    NoiseGraphSpec spec_;
    std::uint32_t seed_{ 0 };
    mutable std::vector<CompiledNode> compiled_;
    mutable FastNoise::SmartNode<> root_;
    mutable float root_freq_{ 1.0f };
};

std::shared_ptr<INoiseGraph> create_noise_graph_from_spec_fastnoise2(
    const NoiseGraphSpec& spec, std::string& errorOut) {
    std::vector<CompiledNode> compiled;
    if (!compile_graph(spec, compiled, errorOut)) {
        return nullptr;
    }
    return std::make_shared<FastNoise2Graph>(spec);
}

}  // namespace procgen
}  // namespace engine
