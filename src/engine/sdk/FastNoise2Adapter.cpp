// FastNoise2Adapter.cpp
//
// FastNoise2 backend for INoiseGraph (G.fastnoise2 integration).
// Uses FastNoiseLite (single-header, MIT, MSVC-2026 compatible) as the
// actual noise generator. The INoiseGraph interface is fulfilled so that
// consumers can swap backends transparently.
//
// Same spec + seed -> deterministic samples.

#include "FastNoise2Adapter.hpp"
#include "RegistryJson.hpp"

// FastNoiseLite — single-header MIT noise library, already vendored.
#include "FastNoiseLite.h"

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
    std::unique_ptr<fast_noise_lite::FastNoiseLite> noise;
    float frequency{ 0.01f };
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

    const char* name() const override { return "FastNoise2 (FastNoiseLite)"; }
    std::uint32_t seed() const override { return seed_; }
    void set_seed(std::uint32_t s) override {
        seed_ = s;
        spec_.seed = s;
        cached_noise_.reset();
    }

    float sample_2d(float x, float z) const override {
        ensure_cached();
        if (!cached_noise_) return 0.0f;
        if (spec_.root < spec_.nodes.size() &&
            spec_.nodes[spec_.root].type == "constant") {
            return compute_constant(spec_.nodes[spec_.root]);
        }
        return cached_noise_->GetNoise(x * root_freq_, z * root_freq_);
    }

    float sample_3d(float x, float y, float z) const override {
        ensure_cached();
        if (!cached_noise_) return 0.0f;
        if (spec_.root < spec_.nodes.size() &&
            spec_.nodes[spec_.root].type == "constant") {
            return compute_constant(spec_.nodes[spec_.root]);
        }
        return cached_noise_->GetNoise(x * root_freq_, y * root_freq_,
                                       z * root_freq_);
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
        cached_noise_.reset();
        return true;
    }

private:
    void ensure_cached() const {
        if (cached_noise_) return;
        if (spec_.root >= spec_.nodes.size()) return;

        const auto& root_node = spec_.nodes[spec_.root];

        // Skip compilation for constant nodes.
        if (root_node.type == "constant") {
            root_freq_ = 0.0f;
            return;
        }

        auto noise = std::make_unique<fast_noise_lite::FastNoiseLite>();
        noise->SetSeed(static_cast<int>(seed_));

        using FNL = fast_noise_lite::FastNoiseLite;
        // Map our node types to FastNoiseLite types.
        if (root_node.type == "perlin") {
            noise->SetNoiseType(FNL::NoiseType_Perlin);
        } else if (root_node.type == "simplex" || root_node.type == "opensimplex2s") {
            noise->SetNoiseType(FNL::NoiseType_OpenSimplex2S);
        } else if (root_node.type == "cellular") {
            noise->SetNoiseType(FNL::NoiseType_Cellular);
        } else if (root_node.type == "value") {
            noise->SetNoiseType(FNL::NoiseType_ValueCubic);
        } else {
            // Default to OpenSimplex2.
            noise->SetNoiseType(FNL::NoiseType_OpenSimplex2);
        }

        // Frequency from params.
        root_freq_ = (root_node.params.size() > 0) ? root_node.params[0] : 0.01f;
        noise->SetFrequency(root_freq_);

        cached_noise_ = std::move(noise);
    }

    NoiseGraphSpec spec_;
    std::uint32_t seed_{ 0 };
    mutable std::unique_ptr<fast_noise_lite::FastNoiseLite> cached_noise_;
    mutable float root_freq_{ 0.01f };
};

std::shared_ptr<INoiseGraph> create_noise_graph_from_spec_fastnoise2(
    const NoiseGraphSpec& spec, std::string& errorOut) {
    // Quick validation of the spec.
    for (std::size_t i = 0; i < spec.nodes.size(); ++i) {
        if (!validate_sources(spec.nodes[i], i, errorOut)) {
            return nullptr;
        }
    }
    if (spec.root >= spec.nodes.size()) {
        errorOut = "noise graph: root node " + std::to_string(spec.root) +
                   " is out of range";
        return nullptr;
    }
    return std::make_shared<FastNoise2Graph>(spec);
}

}  // namespace procgen
}  // namespace engine
