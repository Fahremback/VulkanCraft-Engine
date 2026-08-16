#pragma once

// Public structure-generation contract (META section 18 / FALTANTES item 14):
// deterministic structures/interiors via wave function collapse, implemented
// by the promoted fast-wfc clone (Mathieu Fehr & Nathanaël Courant, MIT —
// gate §32; see DEPENDENCY_POLICY). The contract is self-contained and never
// leaks the backend.
//
// Model: a hand-authored sample floor plan (block ids) defines the local
// vocabulary — every window of `patternSize` in the sample is a pattern. WFC
// then fills a larger plan where every window is one of those patterns, so
// generated interiors are locally consistent with the authored sample. Each
// plan block is extruded into a vertical column profile, producing a full 3D
// structure. A project composes structures/interiors as data (sample + seed +
// profiles), no engine recompile.
//
// Determinism: fast-wfc's RNG is std::minstd_rand seeded by the asset seed
// (contradictions are retried on derived seeds seed+k, bounded). The standard
// library's minstd_rand sequence is implementation-defined, so same spec +
// seed yields bit-identical output within the same binary/platform — the same
// caveat as the builtin sin-hash placement (documented in findings).

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace engine {
namespace procgen {

// Hand-authored sample floor plan + extrusion rules.
struct StructureAssetSpec {
    int sampleWidth{ 0 };
    int sampleHeight{ 0 };
    std::vector<std::uint32_t> sample;  // block ids, row-major (z * w + x)
    int patternSize{ 3 };               // WFC pattern window (>= 1)
    int symmetry{ 1 };                  // pattern symmetries: 1 = identity,
                                        // 2 = +reflection, 4 = +rotations, 8
                                        // = all (higher loosens the wave and
                                        // makes generation more reliable; the
                                        // window-consistency check must then
                                        // accept mirrored/rotated windows)
    bool periodicOutput{ false };
    bool ground{ false };               // pin the sample's bottom pattern as a floor
    std::uint32_t seed{ 0 };
    // Column profiles: block id -> layers from the floor up (e.g. [wall,
    // wall, roof]). Ids without a profile extrude as a single block of that
    // id.
    std::vector<std::pair<std::uint32_t, std::vector<std::uint32_t>>> profiles;
};

// The generated structure: a 3D block grid relative to the structure origin.
// `depth` is the tallest column profile (>= 1); cells above a profile end are
// Air (0).
struct StructureOutput {
    int width{ 0 };   // X size
    int height{ 0 };  // Z size (plan rows)
    int depth{ 0 };   // Y size (tallest profile, >= 1)
    // The raw WFC floor plan (block ids BEFORE extrusion), row-major over
    // (z, x) — index = x + z * width. Every patternSize window of the plan is
    // a window of the sample (identity symmetry), which tests use to verify
    // the output is locally consistent with the authored sample.
    std::vector<std::uint32_t> plan;
    // The extruded 3D grid: index = x + z * width + y * width * height.
    std::vector<std::uint32_t> blocks;
    int seedUsed{ 0 };
    bool succeeded{ false };
};

class IStructureGenerator {
public:
    virtual ~IStructureGenerator() = default;
    virtual const StructureAssetSpec& asset() const = 0;

    // Generates an outWidth x outHeight plan deterministically (derived seeds
    // on WFC contradiction, bounded retries). Returns false only when every
    // attempt contradicts (diagnostic in errorOut). The output is bit-identical
    // for the same asset + size on the same binary.
    virtual bool generate(int outWidth, int outHeight, StructureOutput& out,
                          std::string& errorOut) const = 0;

    // Versioned JSON asset; deserialize is all-or-nothing (on failure the
    // generator keeps its previous spec).
    virtual bool serialize(std::string& out) const = 0;
    virtual bool deserialize(const std::string& json, std::string& errorOut) = 0;
};

// nullptr + diagnostic on malformed specs (empty/undersized sample, bad
// pattern size, empty profiles).
std::shared_ptr<IStructureGenerator> create_structure_generator(
    const StructureAssetSpec& spec, std::string& errorOut);
std::shared_ptr<IStructureGenerator> create_structure_generator_from_json(
    const std::string& json, std::string& errorOut);

}  // namespace procgen
}  // namespace engine
