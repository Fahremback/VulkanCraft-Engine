// FastWfcStructures.cpp
//
// SDK adapter for engine/procgen/IStructureGenerator.hpp (META section 18 /
// FALTANTES item 14: deterministic structures/interiors). The public contract
// never leaks the backend; this TU is the ONLY one that includes the promoted
// fast-wfc headers (Mathieu Fehr & Nathanaël Courant, MIT — gate §32; pinned
// clone compiled directly, see DEPENDENCY_POLICY).
//
// Model: the sample floor plan defines the local pattern vocabulary (every
// patternSize window), the OverlappingWFC fills a larger consistent plan, and
// each plan block is extruded into a vertical column profile -> a 3D
// structure. Determinism: fast-wfc's std::minstd_rand is seeded by the asset
// seed; on contradiction the generator retries derived seeds (seed+k, bounded
// by kMaxAttempts), so the first successful run is fully determined by
// (asset, output size). The minstd_rand sequence is implementation-defined:
// bit-identical within the same binary/platform (documented in findings).

#include "engine/procgen/IStructureGenerator.hpp"
#include "RegistryJson.hpp"

#include "overlapping_wfc.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine {
namespace procgen {
namespace {

constexpr int kMaxAttempts = 8;

bool valid_asset(const StructureAssetSpec& spec, std::string& errorOut) {
    if (spec.sampleWidth < 1 || spec.sampleHeight < 1 ||
        spec.sample.size() !=
            static_cast<std::size_t>(spec.sampleWidth) * spec.sampleHeight) {
        errorOut = "structure generator: sample dimensions/contents mismatch";
        return false;
    }
    if (spec.patternSize < 1 || spec.patternSize > spec.sampleWidth ||
        spec.patternSize > spec.sampleHeight) {
        errorOut = "structure generator: patternSize must be >= 1 and fit the "
                   "sample (sample must contain at least one pattern window)";
        return false;
    }
    if (spec.symmetry != 1 && spec.symmetry != 2 && spec.symmetry != 4 &&
        spec.symmetry != 8) {
        errorOut = "structure generator: symmetry must be 1, 2, 4 or 8";
        return false;
    }
    for (const auto& profile : spec.profiles) {
        if (profile.first == 0) {
            errorOut = "structure generator: profile blockId must be non-zero";
            return false;
        }
        if (profile.second.empty()) {
            errorOut = "structure generator: profile layers must be non-empty";
            return false;
        }
    }
    return true;
}

class FastWfcStructureGenerator final : public IStructureGenerator {
public:
    explicit FastWfcStructureGenerator(StructureAssetSpec spec)
        : spec_(std::move(spec)) {
        rebuild_profiles();
    }

    const StructureAssetSpec& asset() const override { return spec_; }

    bool generate(int outWidth, int outHeight, StructureOutput& out,
                  std::string& errorOut) const override {
        errorOut.clear();
        if (outWidth < spec_.patternSize || outHeight < spec_.patternSize) {
            errorOut = "structure generator: output smaller than the pattern "
                       "window";
            return false;
        }
        for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
            const int seed = static_cast<int>(spec_.seed) + attempt;
            if (run_wfc(outWidth, outHeight, seed, out)) return true;
        }
        out.succeeded = false;
        errorOut = "structure generator: WFC contradicted on every attempt";
        return false;
    }

    bool serialize(std::string& out) const override {
        std::ostringstream ss;
        ss << "{\"version\":1,\"sampleWidth\":" << spec_.sampleWidth
           << ",\"sampleHeight\":" << spec_.sampleHeight
           << ",\"patternSize\":" << spec_.patternSize
           << ",\"symmetry\":" << spec_.symmetry
           << ",\"periodicOutput\":" << (spec_.periodicOutput ? "true" : "false")
           << ",\"ground\":" << (spec_.ground ? "true" : "false")
           << ",\"seed\":" << spec_.seed << ",\"sample\":[";
        for (std::size_t i = 0; i < spec_.sample.size(); ++i) {
            if (i != 0) ss << ',';
            ss << spec_.sample[i];
        }
        ss << "],\"profiles\":[";
        for (std::size_t i = 0; i < spec_.profiles.size(); ++i) {
            if (i != 0) ss << ',';
            ss << "{\"blockId\":" << spec_.profiles[i].first << ",\"layers\":[";
            for (std::size_t l = 0; l < spec_.profiles[i].second.size(); ++l) {
                if (l != 0) ss << ',';
                ss << spec_.profiles[i].second[l];
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
            errorOut = "structure generator: malformed asset - " + errorOut;
            return false;
        }
        if (!document.is_object()) {
            errorOut = "structure generator: asset must be a JSON object";
            return false;
        }
        const sdk::JsonValue* version = document.field("version");
        if (version == nullptr || version->kind != sdk::JsonValue::Kind::Number ||
            static_cast<int>(version->number) != 1) {
            errorOut = "structure generator: unsupported asset version";
            return false;
        }
        StructureAssetSpec parsed;
        parsed.sampleWidth =
            static_cast<int>(sdk::json_number(document, "sampleWidth", 0.0));
        parsed.sampleHeight =
            static_cast<int>(sdk::json_number(document, "sampleHeight", 0.0));
        parsed.patternSize =
            static_cast<int>(sdk::json_number(document, "patternSize", 3.0));
        parsed.symmetry =
            static_cast<int>(sdk::json_number(document, "symmetry", 1.0));
        parsed.periodicOutput =
            sdk::json_bool(document, "periodicOutput", false);
        parsed.ground = sdk::json_bool(document, "ground", false);
        parsed.seed = static_cast<std::uint32_t>(
            sdk::json_number(document, "seed", 0.0));
        for (const double value : sdk::json_number_array(document, "sample")) {
            parsed.sample.push_back(static_cast<std::uint32_t>(value));
        }
        const sdk::JsonValue* profiles = document.field("profiles");
        if (profiles != nullptr) {
            if (!profiles->is_array()) {
                errorOut = "structure generator: \"profiles\" must be an array";
                return false;
            }
            for (const sdk::JsonValue& entry : profiles->array) {
                if (!entry.is_object()) {
                    errorOut = "structure generator: profile entry must be an "
                               "object";
                    return false;
                }
                const std::uint32_t blockId = static_cast<std::uint32_t>(
                    sdk::json_number(entry, "blockId", 0.0));
                std::vector<std::uint32_t> layers;
                for (const double value : sdk::json_number_array(entry, "layers")) {
                    layers.push_back(static_cast<std::uint32_t>(value));
                }
                parsed.profiles.emplace_back(blockId, std::move(layers));
            }
        }
        if (!valid_asset(parsed, errorOut)) return false;
        // All-or-nothing.
        spec_ = std::move(parsed);
        rebuild_profiles();
        return true;
    }

private:
    void rebuild_profiles() {
        profiles_.clear();
        for (const auto& entry : spec_.profiles) {
            profiles_[entry.first] = entry.second;
        }
    }

    bool run_wfc(int outWidth, int outHeight, int seed,
                 StructureOutput& out) const {
        Array2D<std::uint32_t> input(
            static_cast<std::size_t>(spec_.sampleHeight),
            static_cast<std::size_t>(spec_.sampleWidth));
        for (int z = 0; z < spec_.sampleHeight; ++z) {
            for (int x = 0; x < spec_.sampleWidth; ++x) {
                input.get(static_cast<std::size_t>(z),
                          static_cast<std::size_t>(x)) =
                    spec_.sample[static_cast<std::size_t>(z * spec_.sampleWidth +
                                                         x)];
            }
        }
        OverlappingWFCOptions options;
        // ground pins the sample's bottom-middle window as a floor; this
        // fast-wfc version looks that window up among the extracted patterns,
        // which requires the toric extraction (the canonical samples all pair
        // ground with periodic input).
        options.periodic_input = spec_.ground;
        options.periodic_output = spec_.periodicOutput;
        options.out_height = static_cast<unsigned>(outHeight);
        options.out_width = static_cast<unsigned>(outWidth);
        options.symmetry = static_cast<unsigned>(spec_.symmetry);
        options.ground = spec_.ground;
        options.pattern_size = static_cast<unsigned>(spec_.patternSize);

        OverlappingWFC<std::uint32_t> wfc(input, options, seed);
        const std::optional<Array2D<std::uint32_t>> result = wfc.run();
        if (!result) return false;

        const int planHeight = static_cast<int>(result->height);  // rows (z)
        const int planWidth = static_cast<int>(result->width);    // cols (x)
        int depth = 1;
        for (int z = 0; z < planHeight; ++z) {
            for (int x = 0; x < planWidth; ++x) {
                const std::uint32_t block = result->get(z, x);
                const auto found = profiles_.find(block);
                const int layers =
                    found == profiles_.end()
                        ? 1
                        : static_cast<int>(found->second.size());
                depth = std::max(depth, layers);
            }
        }

        out.width = planWidth;
        out.height = planHeight;
        out.depth = depth;
        out.plan.assign(static_cast<std::size_t>(planWidth) * planHeight, 0);
        out.blocks.assign(static_cast<std::size_t>(planWidth) * planHeight *
                              static_cast<std::size_t>(depth),
                          0);
        for (int z = 0; z < planHeight; ++z) {
            for (int x = 0; x < planWidth; ++x) {
                const std::uint32_t block = result->get(z, x);
                out.plan[static_cast<std::size_t>(x + z * planWidth)] = block;
                const auto found = profiles_.find(block);
                for (int y = 0; y < depth; ++y) {
                    std::uint32_t layerBlock = block;
                    if (found != profiles_.end()) {
                        layerBlock =
                            y < static_cast<int>(found->second.size())
                                ? found->second[static_cast<std::size_t>(y)]
                                : 0;  // Air above the profile end
                    }
                    out.blocks[static_cast<std::size_t>(
                        x + z * planWidth + y * planWidth * planHeight)] =
                        layerBlock;
                }
            }
        }
        out.seedUsed = seed;
        out.succeeded = true;
        return true;
    }

    StructureAssetSpec spec_;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> profiles_;
};

}  // namespace

std::shared_ptr<IStructureGenerator> create_structure_generator(
    const StructureAssetSpec& spec, std::string& errorOut) {
    errorOut.clear();
    if (!valid_asset(spec, errorOut)) return nullptr;
    return std::make_shared<FastWfcStructureGenerator>(spec);
}

std::shared_ptr<IStructureGenerator> create_structure_generator_from_json(
    const std::string& json, std::string& errorOut) {
    errorOut.clear();
    auto generator = std::make_shared<FastWfcStructureGenerator>(StructureAssetSpec{});
    if (!generator->deserialize(json, errorOut)) return nullptr;
    return generator;
}

}  // namespace procgen
}  // namespace engine
